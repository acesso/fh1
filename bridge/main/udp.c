// The Wi-Fi link to the FH1 (bridge_udp.h). One socket on FH1B_UDP_PORT: this file's task receives on it and
// sends the beacons; bridge.c sends on it from BTstack's run loop (lwIP's sockets allow that). What the
// FH1 sends is queued and run on the run loop.
#include <string.h>
#include <errno.h>
#include "bridge_udp.h"
#include "bridge_wifi.h"
#include "audio_rx.h"
#include "lwip/sockets.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <btstack_run_loop.h>

static const char *TAG = "bridge_udp";

typedef struct {
    uint8_t type;                      // FH1B_UDP_HELLO or FH1B_CHR_COMMAND
    uint8_t len;
    uint8_t data[110];                 // the largest command: Wi-Fi credentials (refused here, but parsed)
} item_t;

static int s_sock = -1;
static QueueHandle_t s_q;
static btstack_context_callback_registration_t s_reg;
static bridge_udp_hello_fn s_hello;
static bridge_udp_command_fn s_command;
static volatile uint32_t s_peer_ip;    // network order; 0: none
static volatile uint32_t s_peer_ms;    // last datagram from it
static fh1b_beacon_t s_beacon;
static volatile bool s_beacon_ready;
static volatile uint32_t s_fails;      // sends that failed (no buffers), and why
static volatile int s_errno;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// On the run loop: what the FH1 sent.
static void drain(void *ctx)
{
    item_t it;
    while (xQueueReceive(s_q, &it, 0) == pdTRUE) {
        if (it.type == FH1B_UDP_HELLO) s_hello();
        else s_command(it.data, it.len);
    }
}

static void post(const item_t *it)
{
    if (xQueueSend(s_q, it, 0) != pdTRUE) return;          // full: dropped (a ping, most likely)
    btstack_run_loop_execute_on_main_thread(&s_reg);       // once while pending (BTstack keeps it in a list)
}

void bridge_udp_init(bridge_udp_hello_fn hello, bridge_udp_command_fn command)
{
    s_hello = hello;
    s_command = command;
    s_q = xQueueCreate(8, sizeof(item_t));
    s_reg.callback = drain;
}

void bridge_udp_set_beacon(const fh1b_beacon_t *b)
{
    s_beacon = *b;
    s_beacon_ready = true;
}

bool bridge_udp_linked(void)
{
    if (!s_peer_ip) return false;
    if (now_ms() - s_peer_ms > FH1B_UDP_TIMEOUT_MS) {
        s_peer_ip = 0;
        ESP_LOGI(TAG, "FH1 silent: unlinked");
        return false;
    }
    return true;
}

static bool send_to(uint32_t ip, uint8_t type, const void *data, uint16_t len)
{
    uint8_t buf[FH1B_UDP_HDR + 200];
    if (s_sock < 0 || len > sizeof(buf) - FH1B_UDP_HDR) return false;
    buf[0] = 'F';
    buf[1] = '1';
    buf[2] = type;
    memcpy(buf + FH1B_UDP_HDR, data, len);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(FH1B_UDP_PORT), .sin_addr.s_addr = ip };
    if (sendto(s_sock, buf, FH1B_UDP_HDR + len, 0, (struct sockaddr *)&to, sizeof(to)) == FH1B_UDP_HDR + len) return true;
    s_fails++;
    s_errno = errno;
    return false;
}

void bridge_udp_fails(uint32_t *n, int *err)
{
    *n = s_fails;
    *err = s_errno;
    s_fails = 0;
}

bool bridge_udp_send(uint8_t type, const void *data, uint16_t len)
{
    const uint32_t ip = s_peer_ip;
    return ip && send_to(ip, type, data, len);
}

static void task(void *arg)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "no socket");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(FH1B_UDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s_sock, (struct sockaddr *)&me, sizeof(me)) != 0) ESP_LOGE(TAG, "bind failed");
    ESP_LOGI(TAG, "listening on port %d", FH1B_UDP_PORT);
    uint32_t beacon_at = 0;
    for (;;) {
        item_t it;
        static uint8_t buf[FH1B_UDP_HDR + sizeof(fh1b_audio_hdr_t) + FH1B_AUDIO_PCM_SAMPLES * 2];   // the largest: sound
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n >= FH1B_UDP_HDR && buf[0] == 'F' && buf[1] == '1') {
            const uint32_t ip = from.sin_addr.s_addr;
            const uint8_t type = buf[2];
            if (type == FH1B_UDP_HELLO) {
                if (s_peer_ip != ip) ESP_LOGI(TAG, "FH1 at " IPSTR " linked", IP2STR((esp_ip4_addr_t *)&ip));
                s_peer_ms = now_ms();
                s_peer_ip = ip;
                it.type = FH1B_UDP_HELLO;
                it.len = 0;
                post(&it);
            } else if (ip == s_peer_ip) {
                s_peer_ms = now_ms();
                if (type == FH1B_UDP_BYE) {
                    s_peer_ip = 0;
                    ESP_LOGI(TAG, "FH1 unlinked");
                } else if (type == FH1B_UDP_AUDIO) {
                    audio_rx_packet(buf + FH1B_UDP_HDR, (size_t)(n - FH1B_UDP_HDR));   // straight to the codec's ring
                } else if (type == FH1B_CHR_COMMAND && n > FH1B_UDP_HDR && n - FH1B_UDP_HDR <= (int)sizeof(it.data)) {
                    it.type = FH1B_CHR_COMMAND;
                    it.len = (uint8_t)(n - FH1B_UDP_HDR);
                    memcpy(it.data, buf + FH1B_UDP_HDR, it.len);
                    post(&it);
                }
            }
        }
        // The beacon, every 2 s, while connected and no FH1 is linked.
        const uint32_t t = now_ms();
        if (s_beacon_ready && !bridge_udp_linked() && bridge_wifi_connected() && t - beacon_at >= 2000) {
            beacon_at = t;
            send_to(htonl(INADDR_BROADCAST), FH1B_UDP_BEACON, &s_beacon, sizeof(s_beacon));
        }
    }
}

void bridge_udp_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    if (xTaskCreatePinnedToCore(task, "fh1_udp", 3072, NULL, 4, NULL, 0) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "no task");
    }
}

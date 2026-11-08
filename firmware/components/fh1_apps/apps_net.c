// Users between FH1 units (firmware/docs/users.md, U4 and U5): profiles and trust statements over Wi-Fi.
//
// Each unit announces itself by mDNS (_fh1._tcp, TXT unit=<its id>) and serves on the console's web server:
//   GET  /fh1/users   its users' signed profiles: "FH1R" [1] [unit id, 16] [count] then [length u16][profile]...
//   POST /fh1/trust   a trust statement for one of its users (fh1_users_receive's message); answered 202
// A task looks for other units every minute (and at once when asked): it fetches their profiles, which become
// contacts, and delivers the outbox (statements to their units). Its stack is in PSRAM, so it never touches
// the flash: everything stored goes through the UI task (fh1_ui_async), which also shows the notifications.
// Nothing here trusts the network: profiles and statements are checked by their signatures (fh1_users).
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "apps_priv.h"
#include "fh1_ui.h"
#include "fh1_i18n.h"
#include "fh1_users.h"
#include "fh1_devconsole.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "net";

#define ROUND_MS 60000
#define PEERS_MAX 8
#define BODY_MAX 4096
#define OUTBOX_MAX 8
#define STACK 6144

static TaskHandle_t s_task;
static uint8_t *s_buf;                 // the task's HTTP buffer (PSRAM)

// ── The server side (the console's HTTP task) ──

static esp_err_t users_get(httpd_req_t *req)
{
    uint8_t *out = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!out) return httpd_resp_send_500(req);
    size_t n = 0;
    memcpy(out, "FH1R", 4);
    out[4] = 1;
    memcpy(out + 5, fh1_users_unit_id(), FH1_USER_ID_LEN);
    n = 5 + FH1_USER_ID_LEN + 1;
    int count = 0;
    fh1_user_t u;
    for (int i = 0; fh1_users_at(i, &u); i++) {
        if (n + 2 + 200 > BODY_MAX) break;
        const size_t rn = fh1_users_record(u.id, out + n + 2, 200);
        if (!rn) continue;
        out[n] = rn >> 8, out[n + 1] = rn;
        n += 2 + rn;
        count++;
    }
    out[5 + FH1_USER_ID_LEN] = count;
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t r = httpd_resp_send(req, (const char *)out, n);
    heap_caps_free(out);
    return r;
}

static uint32_t peer_ip(httpd_req_t *req)
{
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &al) != 0) return 0;
    if (a.ss_family == AF_INET) return ((struct sockaddr_in *)&a)->sin_addr.s_addr;
    if (a.ss_family == AF_INET6) {                      // an IPv4 address mapped into IPv6
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&a;
        uint32_t ip;
        memcpy(&ip, (const uint8_t *)&a6->sin6_addr + 12, 4);
        return ip;
    }
    return 0;
}

typedef struct {
    uint32_t ip;
    size_t n;
    uint8_t msg[];
} inbox_t;

static void got_trust(void *arg);

static esp_err_t trust_post(httpd_req_t *req)
{
    if (req->content_len < 9 || req->content_len > FH1_TRUST_MSG_MAX) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size");
    inbox_t *in = heap_caps_malloc(sizeof(inbox_t) + req->content_len, MALLOC_CAP_SPIRAM);
    if (!in) return httpd_resp_send_500(req);
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, (char *)in->msg + got, req->content_len - got);
        if (r <= 0) {
            heap_caps_free(in);
            return ESP_FAIL;
        }
        got += r;
    }
    in->n = got;
    in->ip = peer_ip(req);
    fh1_ui_async(got_trust, in);                      // checked and kept by the UI task
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "ok", 2);
}

// ── A small HTTP client (plain sockets, the body in a PSRAM buffer) ──

// method "GET" or "POST"; returns the status (or -1) and the body in s_buf (*len).
static int http(uint32_t ip, uint16_t port, const char *method, const char *path, const uint8_t *body, size_t blen, size_t *len)
{
    *len = 0;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = 4 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    int status = -1;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        char head[160];
        const int hn = snprintf(head, sizeof(head),
                                "%s %s HTTP/1.1\r\nHost: fh1\r\nConnection: close\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n\r\n",
                                method, path, (unsigned)blen);
        if (send(s, head, hn, 0) == hn && (!blen || send(s, body, blen, 0) == (int)blen)) {
            size_t n = 0;
            int r;
            while (n < BODY_MAX && (r = recv(s, s_buf + n, BODY_MAX - n, 0)) > 0) n += r;
            // "HTTP/1.1 200 OK\r\n...\r\n\r\nbody"
            const uint8_t *end = NULL;
            for (size_t i = 0; i + 3 < n; i++)
                if (s_buf[i] == '\r' && s_buf[i + 1] == '\n' && s_buf[i + 2] == '\r' && s_buf[i + 3] == '\n') {
                    end = s_buf + i + 4;
                    break;
                }
            if (n > 12 && end && memcmp(s_buf, "HTTP/1.", 7) == 0) {
                status = atoi((const char *)s_buf + 9);
                *len = n - (end - s_buf);
                memmove(s_buf, end, *len);
            }
        }
    }
    close(s);
    return status;
}

// ── Into the UI task: store, then tell ──

typedef struct {
    uint32_t ip;
    uint16_t port;
    char unit[FH1_USER_ID_LEN + 1];
    size_t n;
    uint8_t body[];
} users_got_t;

static void got_users(void *arg)
{
    users_got_t *g = arg;
    const uint8_t *b = g->body;
    if (g->n > 6 + FH1_USER_ID_LEN && memcmp(b, "FH1R", 4) == 0 && b[4] == 1) {
        int count = b[5 + FH1_USER_ID_LEN];
        size_t i = 6 + FH1_USER_ID_LEN;
        bool any = false;
        while (count-- > 0 && i + 2 <= g->n) {
            const size_t rn = b[i] << 8 | b[i + 1];
            if (i + 2 + rn > g->n) break;
            bool is_new = false;
            if (fh1_contacts_add(b + i + 2, rn, g->unit, g->ip, g->port, &is_new) == ESP_OK) {
                any = true;
                fh1_user_t u;
                if (is_new && fh1_users_verify_record(b + i + 2, rn, &u) == ESP_OK)
                    users_notify_contact(u.id, T("is nearby (another FH1)"));
            }
            i += 2 + rn;
        }
        if (any) users_net_changed();
    }
    heap_caps_free(g);
}

static void got_trust(void *arg)
{
    inbox_t *in = arg;
    char local[FH1_USER_ID_LEN + 1], from[FH1_USER_ID_LEN + 1];
    uint8_t level;
    const esp_err_t r = fh1_users_receive(in->msg, in->n, in->ip, local, from, &level);
    heap_caps_free(in);
    if (r != ESP_OK) {
        ESP_LOGI(TAG, "a trust message was refused: %s", esp_err_to_name(r));
        return;
    }
    users_trust_arrived(local, from, level);
    users_net_changed();
}

// The outbox, read by the UI task for the network task (which must not read the flash).
static struct {
    fh1_outbox_t *box;
    int n;
    SemaphoreHandle_t done;
} s_ob;

static void outbox_read(void *arg)
{
    s_ob.n = fh1_users_outbox(s_ob.box, OUTBOX_MAX);
    xSemaphoreGive(s_ob.done);
}

typedef struct {
    char local[FH1_USER_ID_LEN + 1], other[FH1_USER_ID_LEN + 1];
} sent_t;

static void outbox_sent(void *arg)
{
    sent_t *s = arg;
    fh1_users_outbox_done(s->local, s->other);
    heap_caps_free(s);
    users_net_changed();
}

// ── The task ──

static void round_once(void)
{
    char unit[24];
    snprintf(unit, sizeof(unit), "%s", fh1_users_unit_id());
    fh1_mdns_announce("_fh1", "unit", unit);
    fh1_mdns_peer_t peers[PEERS_MAX];
    const int np = fh1_mdns_find("_fh1", "unit", peers, PEERS_MAX, 3000);
    for (int i = 0; i < np; i++) {
        if (strcmp(peers[i].txt, unit) == 0) continue;            // this unit
        size_t len;
        const int st = http(peers[i].ip, peers[i].port, "GET", "/fh1/users", NULL, 0, &len);
        if (st != 200 || !len) continue;
        users_got_t *g = heap_caps_malloc(sizeof(users_got_t) + len, MALLOC_CAP_SPIRAM);
        if (!g) continue;
        g->ip = peers[i].ip;
        g->port = peers[i].port;
        snprintf(g->unit, sizeof(g->unit), "%s", peers[i].txt);
        g->n = len;
        memcpy(g->body, s_buf, len);
        fh1_ui_async(got_users, g);
    }
    // The outbox: statements to their units, as last seen.
    xSemaphoreTake(s_ob.done, 0);
    fh1_ui_async(outbox_read, NULL);
    if (xSemaphoreTake(s_ob.done, pdMS_TO_TICKS(5000)) != pdTRUE) return;
    for (int i = 0; i < s_ob.n; i++) {
        const fh1_outbox_t *o = &s_ob.box[i];
        if (!o->ip) continue;
        size_t len;
        const int st = http(o->ip, o->port ? o->port : 80, "POST", "/fh1/trust", o->msg, o->len, &len);
        ESP_LOGI(TAG, "statement %s -> %s: %d", o->local, o->other, st);
        if (st == 202 || st == 200) {
            sent_t *s = heap_caps_malloc(sizeof(sent_t), MALLOC_CAP_SPIRAM);
            if (!s) continue;
            memcpy(s->local, o->local, sizeof(s->local));
            memcpy(s->other, o->other, sizeof(s->other));
            fh1_ui_async(outbox_sent, s);
        }
    }
}

static void net_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(8000));                  // after start-up settles
    for (;;) {
        if (fh1_wifi_connected()) round_once();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ROUND_MS));
    }
}

void net_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void net_start(void)
{
    static const httpd_uri_t get = { .uri = "/fh1/users", .method = HTTP_GET, .handler = users_get };
    static const httpd_uri_t post = { .uri = "/fh1/trust", .method = HTTP_POST, .handler = trust_post };
    fh1_console_add_uri(&get);
    fh1_console_add_uri(&post);
    s_buf = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);
    s_ob.box = heap_caps_calloc(OUTBOX_MAX, sizeof(fh1_outbox_t), MALLOC_CAP_SPIRAM);
    s_ob.done = xSemaphoreCreateBinary();
    static StaticTask_t *tcb;
    static StackType_t *stack;
    tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
    if (!s_buf || !s_ob.box || !tcb || !stack) {
        ESP_LOGE(TAG, "no memory for the users' network task");
        return;
    }
    s_task = xTaskCreateStaticPinnedToCore(net_task, "users_net", STACK, NULL, 2, stack, tcb, 0);
}

// Saved states between units (firmware/docs/users.md): one put into a user's data folder over Wi-Fi.
//
//   POST /fh1/state?user=NAME&app=ID&name=STATE   header X-FH1-Code, body: the state's file as fh1_state_save
//                                                  writes it ("FH1S", length, CRC-32, data)
// It replaces that user's STATE for the app (their own kept as STATE.state.bak next to it). Refused while the app
// runs (it would write its own over it), for an unknown user, or when the file's length or CRC do not match.
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include "apps_priv.h"
#include "fh1_sdk.h"
#include "fh1_users.h"
#include "fh1_storage.h"
#include "fh1_devconsole.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_log.h"

static const char *TAG = "saves";

#define STATE_MAX (256 * 1024)
#define STATE_MAGIC 0x53314846u                                   // "FH1S", as run.c writes it

static bool word_ok(const char *k)                                // letters, digits, _ . - (no folders)
{
    const size_t n = strlen(k);
    if (!n || n > 31 || strstr(k, "..")) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t state_post(httpd_req_t *req)
{
    char code[12] = "", query[160] = "", user[24] = "", app[40] = "", name[40] = "";
    httpd_req_get_hdr_value_str(req, "X-FH1-Code", code, sizeof(code));
    if (strcmp(code, fh1_console_token()) != 0) return fail(req, "403 Forbidden", "code needed");
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "user", user, sizeof(user));
    httpd_query_key_value(query, "app", app, sizeof(app));
    httpd_query_key_value(query, "name", name, sizeof(name));
    if (!word_ok(app) || !word_ok(name)) return fail(req, "400 Bad Request", "app and name: letters, digits, _ . -");
    fh1_user_t u;
    bool found = false;
    for (int i = 0; !found && fh1_users_at(i, &u); i++) found = strcasecmp(u.name, user) == 0;
    if (!found) return fail(req, "404 Not Found", "no such user here");
    if (strcmp(fh1_app_id(), app) == 0) return fail(req, "409 Conflict", "the app is running: close it first");
    const size_t len = req->content_len;
    if (len < 12 || len > STATE_MAX) return fail(req, "400 Bad Request", "not a saved state (size)");
    uint8_t *b = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!b) return fail(req, "500 Internal Server Error", "no memory");
    size_t got = 0;
    while (got < len) {
        const int r = httpd_req_recv(req, (char *)b + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += r;
    }
    uint32_t hdr[3];
    memcpy(hdr, b, sizeof(hdr));
    if (got != len || hdr[0] != STATE_MAGIC || hdr[1] != len - 12 || hdr[2] != esp_rom_crc32_le(0, b + 12, len - 12)) {
        heap_caps_free(b);
        return fail(req, "400 Bad Request", "not a saved state (header, length or CRC)");
    }
    char dir[96], path[140], tmp[148], bak[148];
    fh1_users_data_dir(u.id, app, dir, sizeof(dir));
    fh1_mkdirs(dir);
    snprintf(path, sizeof(path), "%s/%s.state", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(b, len, 1, f) == 1;
    if (f) ok = fclose(f) == 0 && ok;
    heap_caps_free(b);
    if (ok) {
        remove(bak);
        rename(path, bak);                                        // (none yet: nothing to keep)
        ok = rename(tmp, path) == 0;
    }
    fh1_int_info_changed();
    if (!ok) return fail(req, "500 Internal Server Error", "cannot write");
    ESP_LOGI(TAG, "%s's %s for %s: %u bytes", u.name, name, app, (unsigned)len);
    return fail(req, "200 OK", "saved");
}

void saves_start(void)
{
    static const httpd_uri_t post = { .uri = "/fh1/state", .method = HTTP_POST, .handler = state_post };
    fh1_console_add_uri(&post);
}

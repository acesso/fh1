// A package installed as it is uploaded (firmware/docs/sdk.md, "Installing"): no copy of the package kept, so a
// unit without an SD card needs room for the app only, not for the package and the app at once.
//
//   POST /fh1/install   header X-FH1-Code, body: the .fpk
// Refused while an app runs (close it first: op stop). The installed apps are rescanned once it is in.
#include <string.h>
#include <stdio.h>
#include "apps_priv.h"
#include "fh1_sdk.h"
#include "fh1_storage.h"
#include "fh1_devconsole.h"
#include "esp_log.h"

static const char *TAG = "install";

typedef struct {
    httpd_req_t *req;
    size_t left;
} upload_t;

// The upload as a stream (funopen): reads what the request still holds.
static int upload_read(void *cookie, char *buf, int n)
{
    upload_t *u = cookie;
    int got = 0;
    while (got < n && u->left) {
        const size_t want = (size_t)(n - got) < u->left ? (size_t)(n - got) : u->left;
        const int r = httpd_req_recv(u->req, buf + got, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return got ? got : -1;
        got += r;
        u->left -= (size_t)r;
    }
    return got;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t install_post(httpd_req_t *req)
{
    char code[12] = "";
    httpd_req_get_hdr_value_str(req, "X-FH1-Code", code, sizeof(code));
    if (strcmp(code, fh1_console_token()) != 0) return reply(req, "403 Forbidden", "code needed");
    if (fh1_app_id()[0]) return reply(req, "409 Conflict", "an app is running: close it first (op stop)");
    if (req->content_len < 1024 || req->content_len > 64 * 1024 * 1024) return reply(req, "400 Bad Request", "not a package (size)");
    upload_t u = { req, req->content_len };
    FILE *in = funopen(&u, upload_read, NULL, NULL, NULL);
    if (!in) return reply(req, "500 Internal Server Error", "no memory");
    setvbuf(in, NULL, _IOFBF, 4096);
    fh1_manifest_t m;
    char err[96], msg[160];
    const esp_err_t r = pkg_install_stream(in, req->content_len, &m, err, sizeof(err));
    fclose(in);
    fh1_int_info_changed();
    apps_installed_changed();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "not installed: %s", err);
        snprintf(msg, sizeof(msg), "not installed: %s", err);
        return reply(req, "400 Bad Request", msg);
    }
    snprintf(msg, sizeof(msg), "installed %s %s", m.name, m.version);
    return reply(req, "200 OK", msg);
}

void install_start(void)
{
    static const httpd_uri_t post = { .uri = "/fh1/install", .method = HTTP_POST, .handler = install_post };
    fh1_console_add_uri(&post);
}

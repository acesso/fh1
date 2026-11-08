// The Users menu (fh1_users.h; firmware/docs/users.md, phase U1), a top-level menu of the launcher next to Apps
// and System: the people on this FH1. It shows each user as name#tag with what they are here (owner, visitor)
// and the player they are now, then New user, New visitor, Restore from SD. A user's window:
// their controllers, play as player 1, link or free a controller, rename, PIN, backup to the SD card, make
// owner, remove. Actions that act as a user ask for their PIN when they have one; owner actions ask for the
// owner's. Keyboard answers go on with fh1_ui_async, after the keyboard has closed.
#include <string.h>
#include <stdio.h>
#include "apps_priv.h"
#include "fh1_ui.h"
#include "fh1_i18n.h"
#include "fh1_input.h"
#include "fh1_gamepad.h"
#include "fh1_users.h"
#include "fh1_avatar.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"

#define ROWS_MAX (FH1_USERS_MAX + 4)

static struct {
    char ids[FH1_USERS_MAX][FH1_USER_ID_LEN + 1];
    void (*menu_changed)(void);
} s_list;

static struct {
    fh1_win_t *win;
    char id[FH1_USER_ID_LEN + 1];
} s_user;

// A flow over several keyboards: whose, what was typed so far.
static struct {
    char id[FH1_USER_ID_LEN + 1];
    char pin[FH1_USER_PIN_MAX + 1];
    char text[65], text2[65];
    char pad[18];
    void (*next)(void);
} s_flow;

static void flow_clear(void) { memset(&s_flow, 0, sizeof(s_flow)); }

static void user_open(const char *id);
static void editor_open(const char *id);
static void watch_tick(lv_timer_t *t);
static void contacts_fill(void);
static void fr_fill(void);
static void friends_open(void *u);

// Users are shown by name only (the tag is in their window).
static void label_of(const fh1_user_t *u, char *out, size_t n) { snprintf(out, n, "%s", u->name); }

// A user's avatar: theirs, or one drawn from their id until they make one.
static void avatar_of(const fh1_user_t *u, fh1_avatar_t *a)
{
    if (!u->avatar_len || !fh1_avatar_decode(u->avatar, u->avatar_len, a)) fh1_avatar_from_seed(u->id, FH1_USER_ID_LEN, a);
}

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int player_of(const char *id)
{
    fh1_user_t u;
    for (int p = 0; p < FH1_PLAYERS; p++)
        if (fh1_users_player(p, &u) && strcmp(u.id, id) == 0) return p;
    return -1;
}

static void err_message(esp_err_t r)
{
    const char *t = r == ESP_ERR_INVALID_CRC ? "Wrong PIN." : r == ESP_ERR_NO_MEM ? "No room for more users." :
                    r == ESP_ERR_INVALID_STATE ? "Not possible now." : "It did not work.";
    fh1_ui_message(T("Users"), T(t));
}

// ── PINs: ask, check, go on ──

static void pin_typed_next(void *u)
{
    if (!fh1_users_check_pin(s_flow.id, s_flow.pin)) {
        flow_clear();
        fh1_ui_message(T("PIN"), T("Wrong PIN."));
        return;
    }
    if (s_flow.next) s_flow.next();
}

static void pin_typed(const char *text, void *u)
{
    if (!text) {
        flow_clear();
        return;
    }
    strlcpy(s_flow.pin, text, sizeof(s_flow.pin));
    fh1_ui_async(pin_typed_next, NULL);
}

// Asks for the user's PIN when they have one, then calls next (s_flow.id and s_flow.pin set).
static void with_pin(const char *id, void (*next)(void))
{
    fh1_user_t u;
    if (!fh1_users_get(id, &u)) return;
    strlcpy(s_flow.id, id, sizeof(s_flow.id));
    s_flow.pin[0] = 0;
    s_flow.next = next;
    if (!u.pin) {
        next();
        return;
    }
    char t[48];
    snprintf(t, sizeof(t), T("PIN of %s"), u.name);
    fh1_ui_keyboard_secret(t, FH1_USER_PIN_MAX, pin_typed, NULL);
}

// ── New users ──

static bool s_new_visitor;

static void new_named(void *u)
{
    const char *why;
    char clean[FH1_USER_NAME_MAX + 1];
    if (!fh1_users_name_check(s_flow.text, NULL, clean, sizeof(clean), &why)) {
        flow_clear();
        fh1_ui_message(T("Users"), T(why));
        return;
    }
    char id[FH1_USER_ID_LEN + 1];
    const bool first = fh1_users_count() == 0;
    esp_err_t r = fh1_users_create(clean, s_new_visitor ? FH1_USER_VISITOR : FH1_USER_LOCAL, id);
    flow_clear();
    if (r != ESP_OK) {
        err_message(r);
        return;
    }
    fh1_user_t nu;
    fh1_users_get(id, &nu);
    user_open(id);
    editor_open(id);                       // a new user makes their avatar first
    if (first) {
        char b[160], who[32];
        label_of(&nu, who, sizeof(who));
        snprintf(b, sizeof(b), T("%s is this FH1's owner. The saves made so far are theirs now."), who);
        fh1_ui_message(T("Users"), b);
    }
}

static void new_typed(const char *text, void *u)
{
    if (!text) return;
    flow_clear();
    strlcpy(s_flow.text, text, sizeof(s_flow.text));
    fh1_ui_async(new_named, NULL);
}

static void new_user_cb(void *u)
{
    s_new_visitor = (bool)(intptr_t)u;
    fh1_ui_keyboard(s_new_visitor ? T("Visitor's name") : T("Name"), "", FH1_USER_NAME_MAX, new_typed, NULL);
}

// ── Restoring a backup ──

static struct {
    fh1_win_t *win;
    char names[8][40];
} s_rest;

static void restore_go(void *u)
{
    char id[FH1_USER_ID_LEN + 1];
    esp_err_t r = fh1_users_import(s_flow.text2, s_flow.text, id);
    flow_clear();
    const char *t = NULL;
    if (r == ESP_ERR_INVALID_CRC) t = "Wrong passphrase, or the file is damaged.";
    else if (r == ESP_ERR_INVALID_STATE) t = "That user is already here, as new or newer.";
    else if (r == ESP_ERR_INVALID_SIZE) t = "Someone here has that name: rename them first.";
    else if (r != ESP_OK) t = "It did not work.";
    if (t) {
        fh1_ui_message(T("Restore"), T(t));
        return;
    }
    fh1_user_t nu;
    fh1_users_get(id, &nu);
    char b[96], who[32];
    label_of(&nu, who, sizeof(who));
    snprintf(b, sizeof(b), T("%s is restored."), who);
    fh1_ui_message(T("Restore"), b);
}

static void restore_typed(const char *text, void *u)
{
    if (!text) {
        flow_clear();
        return;
    }
    strlcpy(s_flow.text, text, sizeof(s_flow.text));
    fh1_ui_async(restore_go, NULL);
}

static void restore_pick(void *u)
{
    const int i = (int)(intptr_t)u;
    flow_clear();
    strlcpy(s_flow.text2, s_rest.names[i], sizeof(s_flow.text2));
    if (s_rest.win) fh1_win_close(s_rest.win);
    fh1_ui_keyboard_secret(T("Passphrase"), 64, restore_typed, NULL);
}

static void restore_closed(fh1_win_t *w, void *u)
{
    if (s_rest.win == w) s_rest.win = NULL;
}

static void restore_cb(void *u)
{
    const int n = fh1_users_backups(s_rest.names, 8);
    if (!n) {
        fh1_ui_message(T("Restore"), T("No backups on the SD card (folder users)."));
        return;
    }
    fh1_win_t *w = fh1_win_create(T("Restore"), 220, 150);
    if (!w) return;
    s_rest.win = w;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, restore_closed, NULL);
    for (int i = 0; i < n; i++) fh1_ui_button(w, fh1_win_body(w), s_rest.names[i], restore_pick, (void *)(intptr_t)i);
}

// ── The menu ──
// Each user: their mini face (16 x 16, animated) with badges over it: a crown for the owner or a tag for a
// visitor (top right), and their controller connected now in its colour (bottom right).

#define ICON 16
#define FR_MAX (FH1_CONTACTS_MAX + FH1_USERS_MAX)
#define FACES_MAX 40

// Faces shown as small animated pictures (in the menu, in lists): each with whose it is and how it is drawn.
// One timer draws them all; a face leaves the list when its picture is deleted.
typedef struct {
    lv_obj_t *img;
    char id[FH1_USER_ID_LEN + 1];
    fh1_avatar_t a;
    uint8_t expr, size;
    bool local;                        // one of this unit's users: badges (16 x 16 only)
} face_t;
static face_t *s_faces;               // FACES_MAX, in PSRAM (internal RAM is for Wi-Fi)
static lv_timer_t *s_face_timer;
static uint32_t *s_face_px;

// Badges: K ink, Y gold, W white, C visitor cyan, P the controller's colour.
static const char *const BADGE_OWNER[] = { ".K.K.K.", "KYKYKYK", "KYYYYYK", "KKKKKKK" };
static const char *const BADGE_VISITOR[] = { "KKKKKKK", "KWCCCWK", "KCWCWCK", "KCCWCCK", "KKKKKKK" };
static const char *const BADGE_PAD[] = { ".KKKKK.", "KPPPPPK", "KPPPPPK", "KPKKKPK", ".K...K." };

static void badge(uint32_t *px, const char *const *rows, int nrows, int x0, int y0, uint32_t pad_rgb)
{
    for (int y = 0; y < nrows; y++) {
        for (int x = 0; rows[y][x]; x++) {
            uint32_t c;
            switch (rows[y][x]) {
            case 'K': c = 0x1a1222; break;
            case 'Y': c = 0xf4c21f; break;
            case 'W': c = 0xffffff; break;
            case 'C': c = 0x26a7c7; break;
            case 'P': c = pad_rgb; break;
            default: continue;
            }
            const int px_ = x0 + x, py = y0 + y;
            if (px_ >= 0 && py >= 0 && px_ < ICON && py < ICON) px[py * ICON + px_] = 0xff000000u | c;
        }
    }
}

// The user's controller connected now, if any.
static bool attached_pad(const char *id, fh1_gamepad_info_t *g)
{
    char ids[FH1_GAMEPADS_MAX][18];
    const int n = fh1_users_pads_of(id, ids, FH1_GAMEPADS_MAX);
    for (int i = 0; i < n; i++)
        for (int j = 0; fh1_gamepad_at(j, g, NULL); j++)
            if (strcmp(g->id, ids[i]) == 0) return true;
    return false;
}

// Anyone's avatar: a local user's, or a contact's (from their profile), or one drawn from the id.
static void avatar_any(const char *id, fh1_avatar_t *a)
{
    fh1_user_t u;
    fh1_contact_t c;
    if (fh1_users_get(id, &u)) avatar_of(&u, a);
    else if (fh1_contacts_get(id, &c)) avatar_of(&c.u, a);
    else fh1_avatar_from_seed(id, strlen(id), a);
}

static void face_draw(int i, uint32_t t)
{
    if (!s_faces[i].img || !s_face_px) return;
    const int sz = s_faces[i].size;
    fh1_avatar_render_face(&s_faces[i].a, s_faces[i].expr, t + i * 977, sz, s_face_px);   // each at its own moment
    fh1_user_t u;
    if (s_faces[i].local && sz == ICON && fh1_users_get(s_faces[i].id, &u)) {
        if (u.owner) badge(s_face_px, BADGE_OWNER, 4, ICON - 7, 0, 0);
        else if (u.kind == FH1_USER_VISITOR) badge(s_face_px, BADGE_VISITOR, 5, ICON - 7, 0, 0);
        fh1_gamepad_info_t g;
        if (attached_pad(u.id, &g)) badge(s_face_px, BADGE_PAD, 5, ICON - 7, ICON - 5, g.rgb ? g.rgb : 0xb0b4ba);
    }
    fh1_ui_pixels_set(s_faces[i].img, s_face_px);
}

static void faces_tick(lv_timer_t *t)
{
    bool any = false;
    if (!s_faces) return;
    const uint32_t now = ms_now();
    for (int i = 0; i < FACES_MAX; i++) {
        if (!s_faces[i].img) continue;
        any = true;
        face_draw(i, now);
    }
    if (!any) {                                    // nothing shows a face: stop
        lv_timer_delete(s_face_timer);
        s_face_timer = NULL;
    }
}

static void face_deleted(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_faces) s_faces[i].img = NULL;
}

// Animates a picture (size x size, already made) as id's face.
static void face_add(lv_obj_t *img, const char *id, int expr, int size)
{
    if (!img) return;
    if (!s_face_px) s_face_px = heap_caps_malloc(32 * 32 * 4, MALLOC_CAP_SPIRAM);
    if (!s_faces) s_faces = heap_caps_calloc(FACES_MAX, sizeof(face_t), MALLOC_CAP_SPIRAM);
    if (!s_faces) return;
    for (int i = 0; i < FACES_MAX; i++) {
        if (s_faces[i].img) continue;
        s_faces[i].img = img;
        strlcpy(s_faces[i].id, id, sizeof(s_faces[i].id));
        avatar_any(id, &s_faces[i].a);
        s_faces[i].expr = expr, s_faces[i].size = size;
        s_faces[i].local = fh1_users_get(id, NULL);
        lv_obj_add_event_cb(img, face_deleted, LV_EVENT_DELETE, (void *)(intptr_t)i);
        face_draw(i, ms_now());
        if (!s_face_timer) s_face_timer = lv_timer_create(faces_tick, 120, NULL);
        return;
    }
}

// A list row: a button with id's mini face before the text.
static lv_obj_t *face_button(fh1_win_t *win, lv_obj_t *parent, const char *id, const char *text, void (*cb)(void *), void *user)
{
    lv_obj_t *btn = fh1_ui_button(win, parent, text, cb, user);
    face_add(fh1_ui_button_lead_pixels(btn, ICON, ICON), id, FH1_AV_NEUTRAL, ICON);
    return btn;
}

static void list_row(void *u) { user_open(s_list.ids[(int)(intptr_t)u]); }
static void contacts_open(void *u);

void users_menu_fill(fh1_win_t *win)
{
    lv_obj_t *body = fh1_win_body(win);
    const int n = fh1_users_count();
    if (!n) {
        fh1_ui_label(body, T("No users yet. The first user made is this FH1's owner: they look after its settings, apps and other users."));
    }
    for (int i = 0; i < n; i++) {
        fh1_user_t u;
        if (!fh1_users_at(i, &u)) continue;
        strlcpy(s_list.ids[i], u.id, sizeof(s_list.ids[i]));
        char b[64];
        const int pl = player_of(u.id);
        if (pl >= 0) snprintf(b, sizeof(b), "%s  %dP", u.name, pl + 1);
        else snprintf(b, sizeof(b), "%s", u.name);
        face_button(win, body, u.id, b, list_row, (void *)(intptr_t)i);
    }
    fh1_ui_button(win, body, n ? T("New user") : T("New user (owner)"), new_user_cb, (void *)(intptr_t)0);
    if (n) fh1_ui_button(win, body, T("New visitor"), new_user_cb, (void *)(intptr_t)1);
    char c[48];
    snprintf(c, sizeof(c), "%s (%d)", T("People nearby"), fh1_contacts_count());
    fh1_ui_button(win, body, c, contacts_open, NULL);
    fh1_ui_button(win, body, T("Restore from SD"), restore_cb, NULL);
}

// ── The avatar editor ──
// The full figure, and beside it the face and the mini face, all animated; the choices on the right: Left/Right
// (or A) change the focused one. Colours show as a chip. The expression is only for looking. Save asks for
// the user's PIN when they have one and signs the new profile.

enum { ED_ACC0 = FH1_AV_FIELDS, ED_EXPR = ED_ACC0 + FH1_AV_ACCS, ED_RANDOM, ED_SAVE, ED_ROWS };
#define CHIP 6
static struct {
    fh1_win_t *win;
    char id[FH1_USER_ID_LEN + 1];
    fh1_avatar_t a;
    int expr, tick;
    lv_obj_t *full, *face, *mini, *rows[ED_ROWS], *chips[ED_ROWS];
    lv_timer_t *timer;
    uint32_t *px;                      // the full figure's pixels (the largest)
} s_ed;

static bool is_colour(int f) { return f < FH1_AV_FIELDS && !fh1_avatar_choice_name(f, 0); }

static void ed_chip(int r)
{
    if (!s_ed.chips[r]) return;
    uint32_t px[CHIP * CHIP];
    const uint32_t c = fh1_avatar_choice_colour(r, s_ed.a.f[r]);
    for (int y = 0; y < CHIP; y++)
        for (int x = 0; x < CHIP; x++)
            px[y * CHIP + x] = 0xff000000u | (x == 0 || y == 0 || x == CHIP - 1 || y == CHIP - 1 ? 0x1a1222 : c);
    fh1_ui_pixels_set(s_ed.chips[r], px);
}

static void ed_text(int r)
{
    char b[64];
    if (r < FH1_AV_FIELDS) {
        const char *nm = fh1_avatar_choice_name(r, s_ed.a.f[r]);
        if (nm) snprintf(b, sizeof(b), "%s: %s", T(fh1_avatar_field_name(r)), T(nm));
        else snprintf(b, sizeof(b), "%s", T(fh1_avatar_field_name(r)));
        ed_chip(r);
    } else if (r < ED_EXPR) {
        snprintf(b, sizeof(b), "%s: %s", T(fh1_avatar_acc_name(r - ED_ACC0)), (s_ed.a.acc >> (r - ED_ACC0)) & 1 ? T("yes") : T("no"));
    } else if (r == ED_EXPR) {
        snprintf(b, sizeof(b), "%s: %s", T("Expression"), T(fh1_avatar_expr_name(s_ed.expr)));
    } else {
        snprintf(b, sizeof(b), "%s", r == ED_RANDOM ? T("Random") : T("Save"));
    }
    fh1_ui_button_set_text(s_ed.rows[r], b);
}

static void ed_draw(bool full)
{
    if (!s_ed.px) return;
    const uint32_t t = ms_now();
    if (full) {
        fh1_avatar_render_full(&s_ed.a, s_ed.expr, t, s_ed.px);
        fh1_ui_pixels_set(s_ed.full, s_ed.px);
    }
    fh1_avatar_render_face(&s_ed.a, s_ed.expr, t, 32, s_ed.px);
    fh1_ui_pixels_set(s_ed.face, s_ed.px);
    fh1_avatar_render_face(&s_ed.a, s_ed.expr, t, 16, s_ed.px);
    fh1_ui_pixels_set(s_ed.mini, s_ed.px);
}

static void ed_tick(lv_timer_t *t) { ed_draw(s_ed.tick++ % 2 == 0); }

static void ed_change(int r, int d)
{
    if (r < FH1_AV_FIELDS) {
        const int n = fh1_avatar_choices(r);
        s_ed.a.f[r] = (uint8_t)((s_ed.a.f[r] + d + n) % n);
    } else if (r < ED_EXPR) {
        const int k = r - ED_ACC0;
        s_ed.a.acc ^= 1 << k;
        // One pair of glasses at a time.
        if (k == FH1_AV_ACC_ROUND_GLASSES && (s_ed.a.acc >> k & 1)) s_ed.a.acc &= ~(1 << FH1_AV_ACC_SQUARE_GLASSES);
        if (k == FH1_AV_ACC_SQUARE_GLASSES && (s_ed.a.acc >> k & 1)) s_ed.a.acc &= ~(1 << FH1_AV_ACC_ROUND_GLASSES);
        ed_text(ED_ACC0 + FH1_AV_ACC_ROUND_GLASSES);
        ed_text(ED_ACC0 + FH1_AV_ACC_SQUARE_GLASSES);
    } else if (r == ED_EXPR) {
        s_ed.expr = (s_ed.expr + d + FH1_AV_EXPRS) % FH1_AV_EXPRS;
    } else {
        return;
    }
    ed_text(r);
    ed_draw(true);
}

static void ed_save_go(void)
{
    uint8_t code[FH1_AVATAR_CODE_LEN];
    const size_t n = fh1_avatar_encode(&s_ed.a, code, sizeof(code));
    esp_err_t r = fh1_users_set_avatar(s_flow.id, code, n, s_flow.pin);
    flow_clear();
    if (r != ESP_OK) {
        err_message(r);
        return;
    }
    if (s_ed.win) fh1_win_close(s_ed.win);
}

static void ed_row(void *u)
{
    const int r = (int)(intptr_t)u;
    if (r <= ED_EXPR) {
        ed_change(r, 1);
    } else if (r == ED_RANDOM) {
        uint32_t seed[2] = { esp_random(), (uint32_t)esp_timer_get_time() };
        fh1_avatar_from_seed(seed, sizeof(seed), &s_ed.a);
        for (int i = 0; i < ED_EXPR; i++) ed_text(i);
        ed_draw(true);
    } else {
        with_pin(s_ed.id, ed_save_go);
    }
}

static bool ed_key(fh1_win_t *w, int key, void *u)
{
    if (key != FH1_KEY_LEFT && key != FH1_KEY_RIGHT) return false;
    const int r = fh1_ui_focused_index(w);
    if (r >= 0 && r <= ED_EXPR) ed_change(r, key == FH1_KEY_RIGHT ? 1 : -1);
    return true;
}

static void ed_closed(fh1_win_t *w, void *u)
{
    if (s_ed.timer) lv_timer_delete(s_ed.timer);
    heap_caps_free(s_ed.px);
    memset(&s_ed, 0, sizeof(s_ed));
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_gap(o, 2 * fh1_ui_scale(), 0);
    return o;
}

static void editor_open(const char *id)
{
    fh1_user_t u;
    if (!fh1_users_get(id, &u)) return;
    if (s_ed.win && fh1_win_is_open(s_ed.win)) fh1_win_close(s_ed.win);
    char title[48];
    snprintf(title, sizeof(title), T("Avatar of %s"), u.name);
    fh1_win_t *w = fh1_win_create(title, 310, 212);
    if (!w) return;
    memset(&s_ed, 0, sizeof(s_ed));
    s_ed.win = w;
    strlcpy(s_ed.id, id, sizeof(s_ed.id));
    avatar_of(&u, &s_ed.a);
    s_ed.px = heap_caps_malloc(FH1_AVATAR_FULL_W * FH1_AVATAR_FULL_H * 4, MALLOC_CAP_SPIRAM);
    const fh1_hint_t h[] = { { FH1_HINT_LEFTRIGHT, T("Change") }, { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 3);
    fh1_win_on_key(w, ed_key, NULL);
    fh1_win_on_close(w, ed_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(body, false);
    // Left: the full figure, and beside it the face over the mini face.
    s_ed.full = fh1_ui_pixels(body, FH1_AVATAR_FULL_W, FH1_AVATAR_FULL_H, 1);
    lv_obj_t *small = box(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(small, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ed.face = fh1_ui_pixels(small, 32, 32, 1);
    s_ed.mini = fh1_ui_pixels(small, 16, 16, 1);
    // Right: the choices.
    lv_obj_t *right = box(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_style_pad_left(right, 9 * fh1_ui_scale(), 0);         // room for the cursor
    lv_obj_set_style_pad_gap(right, fh1_ui_scale(), 0);
    lv_obj_set_scrollable(right, true);
    lv_obj_set_scroll_dir(right, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_OFF);
    for (int r = 0; r < ED_ROWS; r++) {
        s_ed.rows[r] = fh1_ui_button(w, right, "", ed_row, (void *)(intptr_t)r);
        if (is_colour(r)) {
            s_ed.chips[r] = fh1_ui_pixels(s_ed.rows[r], CHIP, CHIP, 1);
            lv_obj_align(s_ed.chips[r], LV_ALIGN_RIGHT_MID, 0, 0);
        }
        ed_text(r);
    }
    ed_draw(true);
    s_ed.timer = lv_timer_create(ed_tick, 120, NULL);
}

static void user_fill(void);

static void on_users_changed(void *arg)
{
    if (s_list.menu_changed) s_list.menu_changed();
    contacts_fill();
    fr_fill();
    if (s_user.win && fh1_win_is_open(s_user.win)) {
        if (fh1_users_get(s_user.id, NULL)) user_fill();
        else fh1_win_close(s_user.win);
    }
}


void users_ui_init(void (*menu_changed)(void))
{
    s_list.menu_changed = menu_changed;
    fh1_users_on_change(on_users_changed, NULL);
    lv_timer_create(watch_tick, 1000, NULL);          // controllers connecting, player 1 changing hands
}

// ── One user ──

static void use_go(void)
{
    fh1_users_use(s_flow.id);
    char who[32] = "";
    fh1_user_t u;
    if (fh1_users_get(s_flow.id, &u)) label_of(&u, who, sizeof(who));
    flow_clear();
    char b[160];
    snprintf(b, sizeof(b), T("%s is player 1 while no controller of another user plays as player 1."), who);
    fh1_ui_message(T("Users"), b);
}
static void use_cb(void *u) { with_pin(s_user.id, use_go); }

static void rename_go(void *u)
{
    const char *why;
    if (!fh1_users_name_check(s_flow.text, s_flow.id, NULL, 0, &why)) {
        flow_clear();
        fh1_ui_message(T("Rename"), T(why));
        return;
    }
    esp_err_t r = fh1_users_rename(s_flow.id, s_flow.text, s_flow.pin);
    flow_clear();
    if (r != ESP_OK) err_message(r);
}
static void rename_typed(const char *text, void *u)
{
    if (!text) {
        flow_clear();
        return;
    }
    strlcpy(s_flow.text, text, sizeof(s_flow.text));
    fh1_ui_async(rename_go, NULL);
}
static void rename_ask(void)
{
    fh1_user_t u;
    fh1_users_get(s_flow.id, &u);
    fh1_ui_keyboard(T("New name"), u.name, FH1_USER_NAME_MAX, rename_typed, NULL);
}
static void rename_cb(void *u) { with_pin(s_user.id, rename_ask); }

// PIN: the new one twice; "" removes it.
static void pin_set_go(void *u)
{
    if (strcmp(s_flow.text, s_flow.text2) != 0) {
        flow_clear();
        fh1_ui_message(T("PIN"), T("The two PINs differ."));
        return;
    }
    esp_err_t r = fh1_users_set_pin(s_flow.id, s_flow.pin, s_flow.text);
    flow_clear();
    if (r == ESP_ERR_INVALID_ARG) fh1_ui_message(T("PIN"), T("A PIN has 4 to 8 digits."));
    else if (r != ESP_OK) err_message(r);
    else fh1_ui_message(T("PIN"), T("PIN saved."));
}
static void pin_again_typed(const char *text, void *u)
{
    if (!text) {
        flow_clear();
        return;
    }
    strlcpy(s_flow.text2, text, sizeof(s_flow.text2));
    fh1_ui_async(pin_set_go, NULL);
}
static void pin_again(void *u) { fh1_ui_keyboard_secret(T("New PIN again"), FH1_USER_PIN_MAX, pin_again_typed, NULL); }
static void pin_new_typed(const char *text, void *u)
{
    if (!text) {
        flow_clear();
        return;
    }
    strlcpy(s_flow.text, text, sizeof(s_flow.text));
    fh1_ui_async(pin_again, NULL);
}
static void pin_new(void) { fh1_ui_keyboard_secret(T("New PIN (4 to 8 digits)"), FH1_USER_PIN_MAX, pin_new_typed, NULL); }
static void pin_cb(void *u) { with_pin(s_user.id, pin_new); }

static void pin_remove_go(void)
{
    esp_err_t r = fh1_users_set_pin(s_flow.id, s_flow.pin, "");
    flow_clear();
    if (r != ESP_OK) err_message(r);
}
static void pin_remove_cb(void *u) { with_pin(s_user.id, pin_remove_go); }

// Backup: the passphrase twice, then the file.
static void backup_go(void *u)
{
    if (strcmp(s_flow.text, s_flow.text2) != 0) {
        flow_clear();
        fh1_ui_message(T("Back up"), T("The two passphrases differ."));
        return;
    }
    char path[96];
    esp_err_t r = fh1_users_export(s_flow.id, s_flow.pin, s_flow.text, path, sizeof(path));
    flow_clear();
    char b[200];
    if (r == ESP_ERR_INVALID_ARG) snprintf(b, sizeof(b), "%s", T("A passphrase has at least 6 characters."));
    else if (r == ESP_ERR_INVALID_STATE) snprintf(b, sizeof(b), "%s", T("No SD card, or it cannot be written."));
    else if (r != ESP_OK) snprintf(b, sizeof(b), "%s", T("It did not work."));
    else snprintf(b, sizeof(b), T("Saved %s. Keep the passphrase: without it the file cannot be opened. With both, anyone can be this user."), path);
    fh1_ui_message(T("Back up"), b);
}
static void backup_again_typed(const char *text, void *u)
{
    if (!text) {
        flow_clear();
        return;
    }
    strlcpy(s_flow.text2, text, sizeof(s_flow.text2));
    fh1_ui_async(backup_go, NULL);
}
static void backup_again(void *u) { fh1_ui_keyboard_secret(T("Passphrase again"), 64, backup_again_typed, NULL); }
static void backup_typed(const char *text, void *u)
{
    if (!text) {
        flow_clear();
        return;
    }
    strlcpy(s_flow.text, text, sizeof(s_flow.text));
    fh1_ui_async(backup_again, NULL);
}
static void backup_ask(void) { fh1_ui_keyboard_secret(T("Passphrase (6+ characters)"), 64, backup_typed, NULL); }
static void backup_cb(void *u) { with_pin(s_user.id, backup_ask); }

// Owner actions: the owner's PIN, then a confirmation.
static char s_target[FH1_USER_ID_LEN + 1];

static void owner_yes(bool yes, void *u)
{
    if (yes) {
        esp_err_t r = fh1_users_make_owner(s_target);
        if (r != ESP_OK) err_message(r);
    }
}
static void owner_go(void)
{
    flow_clear();
    fh1_user_t t;
    if (!fh1_users_get(s_target, &t)) return;
    char b[140], who[32];
    label_of(&t, who, sizeof(who));
    snprintf(b, sizeof(b), T("Make %s the owner of this FH1? Only the owner manages users."), who);
    fh1_ui_confirm(T("Make owner"), b, owner_yes, NULL);
}
static void owner_cb(void *u)
{
    strlcpy(s_target, s_user.id, sizeof(s_target));
    with_pin(fh1_users_owner(), owner_go);
}

static void remove_yes(bool yes, void *u)
{
    if (yes) {
        esp_err_t r = fh1_users_remove(s_target);
        if (r != ESP_OK) err_message(r);
    }
}
static void remove_go(void)
{
    flow_clear();
    fh1_user_t t;
    if (!fh1_users_get(s_target, &t)) return;
    char b[160], who[32];
    label_of(&t, who, sizeof(who));
    snprintf(b, sizeof(b), T("Remove %s and their saves from this FH1? Without a backup this cannot be undone."), who);
    fh1_ui_confirm(T("Remove"), b, remove_yes, NULL);
}
static void remove_cb(void *u)
{
    strlcpy(s_target, s_user.id, sizeof(s_target));
    with_pin(fh1_users_owner(), remove_go);
}

// Controllers: link one connected now, or free one of theirs.
static struct {
    fh1_win_t *win;
    char ids[FH1_GAMEPADS_MAX][18];
    bool free;                         // freeing the user's controllers, not linking
} s_pads;

static void pads_closed(fh1_win_t *w, void *u)
{
    if (s_pads.win == w) s_pads.win = NULL;
}

static void pads_pick(void *u)
{
    const int i = (int)(intptr_t)u;
    esp_err_t r = fh1_users_link_pad(s_pads.ids[i], s_pads.free ? "" : s_user.id);
    if (s_pads.win) fh1_win_close(s_pads.win);
    if (r != ESP_OK) err_message(r);
}

// The controller's name and look: as connected now, else its id.
static bool pad_look(const char *id, fh1_gamepad_info_t *g)
{
    for (int i = 0; fh1_gamepad_at(i, g, NULL); i++)
        if (strcmp(g->id, id) == 0) return true;
    memset(g, 0, sizeof(*g));
    strlcpy(g->id, id, sizeof(g->id));
    return false;
}

static void pads_window(bool free_them)
{
    fh1_win_t *w = fh1_win_create(free_them ? T("Free a controller") : T("Link a controller"), 230, 160);
    if (!w) return;
    memset(&s_pads, 0, sizeof(s_pads));
    s_pads.win = w;
    s_pads.free = free_them;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, pads_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    int k = 0;
    char b[80];
    if (free_them) {
        char ids[FH1_GAMEPADS_MAX][18];
        const int n = fh1_users_pads_of(s_user.id, ids, FH1_GAMEPADS_MAX);
        for (int i = 0; i < n; i++) {
            fh1_gamepad_info_t g;
            const bool on = pad_look(ids[i], &g);
            if (on) snprintf(b, sizeof(b), "%s " FH1_SYM_BRIDGE "%s", g.name, g.via);
            else snprintf(b, sizeof(b), "%s (%s)", ids[i], T("away"));
            strlcpy(s_pads.ids[k], ids[i], sizeof(s_pads.ids[k]));
            lv_obj_t *btn = fh1_ui_button(w, body, b, pads_pick, (void *)(intptr_t)k++);
            if (on) fh1_ui_lead_pad(btn, g.kind, g.rgb);
        }
    } else {
        fh1_gamepad_info_t g;
        for (int i = 0; k < FH1_GAMEPADS_MAX && fh1_gamepad_at(i, &g, NULL); i++) {
            char owner[FH1_USER_ID_LEN + 1], who[40] = "";
            fh1_user_t o;
            if (fh1_users_pad_user(g.id, owner) && fh1_users_get(owner, &o)) snprintf(who, sizeof(who), " · %s", o.name);
            snprintf(b, sizeof(b), "%s " FH1_SYM_BRIDGE "%s%s", g.name, g.via, who);
            strlcpy(s_pads.ids[k], g.id, sizeof(s_pads.ids[k]));
            fh1_ui_lead_pad(fh1_ui_button(w, body, b, pads_pick, (void *)(intptr_t)k++), g.kind, g.rgb);
        }
        if (!k) fh1_ui_line(body, T("No gamepads connected (System > Bridges)."));
    }
}

static void link_go(void)
{
    flow_clear();
    pads_window(false);
}
static void link_cb(void *u) { with_pin(s_user.id, link_go); }
static void free_cb(void *u) { pads_window(true); }

static void user_closed(fh1_win_t *w, void *u)
{
    if (s_user.win == w) memset(&s_user, 0, sizeof(s_user));
}

static void edit_cb(void *u) { editor_open(s_user.id); }

static void user_fill(void)
{
    fh1_user_t u;
    if (!s_user.win || !fh1_users_get(s_user.id, &u)) return;
    const int focus = fh1_ui_focused_index(s_user.win);
    fh1_win_clear(s_user.win);
    char b[80];
    label_of(&u, b, sizeof(b));
    fh1_win_set_title(s_user.win, b);
    lv_obj_t *body = fh1_win_body(s_user.win);
    face_add(fh1_ui_pixels(body, 32, 32, 1), u.id, FH1_AV_HAPPY, 32);    // their face, animated
    fh1_ui_button(s_user.win, body, T("Edit avatar"), edit_cb, NULL);
    fh1_ui_button(s_user.win, body, T("Friends"), friends_open, NULL);
    fh1_ui_value_set(fh1_ui_value(body, T("Is")), u.owner ? T("owner") : u.kind == FH1_USER_VISITOR ? T("visitor") : T("local user"));
    const int pl = player_of(u.id);
    if (pl >= 0) snprintf(b, sizeof(b), T("Player %d"), pl + 1);
    fh1_ui_value_set(fh1_ui_value(body, T("Plays")), pl >= 0 ? b : "-");
    // Their controllers, with their look when connected.
    char ids[FH1_GAMEPADS_MAX][18];
    const int np = fh1_users_pads_of(u.id, ids, FH1_GAMEPADS_MAX);
    for (int i = 0; i < np; i++) {
        fh1_gamepad_info_t g;
        const bool on = pad_look(ids[i], &g);
        if (on) snprintf(b, sizeof(b), "%s " FH1_SYM_BRIDGE "%s", g.name, g.via);
        else snprintf(b, sizeof(b), "%s (%s)", ids[i], T("away"));
        lv_obj_t *l = fh1_ui_line(body, b);
        if (on) fh1_ui_lead_pad(l, g.kind, g.rgb);
    }
    if (pl != 0) fh1_ui_button(s_user.win, body, T("Play as player 1"), use_cb, NULL);
    fh1_ui_button(s_user.win, body, T("Link a controller"), link_cb, NULL);
    if (np) fh1_ui_button(s_user.win, body, T("Free a controller"), free_cb, NULL);
    fh1_ui_button(s_user.win, body, T("Rename"), rename_cb, NULL);
    fh1_ui_button(s_user.win, body, u.pin ? T("Change PIN") : T("Set a PIN"), pin_cb, NULL);
    if (u.pin) fh1_ui_button(s_user.win, body, T("Remove the PIN"), pin_remove_cb, NULL);
    fh1_ui_button(s_user.win, body, T("Back up to SD"), backup_cb, NULL);
    if (!u.owner && u.kind == FH1_USER_LOCAL) fh1_ui_button(s_user.win, body, T("Make owner"), owner_cb, NULL);
    if (!u.owner) fh1_ui_button(s_user.win, body, T("Remove"), remove_cb, NULL);
    fh1_ui_value_set(fh1_ui_value(body, T("PIN")), u.pin ? T("yes") : T("no"));
    snprintf(b, sizeof(b), "#%s", u.tag);
    fh1_ui_value_set(fh1_ui_value(body, T("Tag")), b);
    fh1_ui_value_set(fh1_ui_value(body, "Id"), u.id);
    if (focus >= 0) fh1_ui_focus_index(s_user.win, focus);
}

static void user_open(const char *id)
{
    if (s_user.win && fh1_win_is_open(s_user.win)) fh1_win_close(s_user.win);
    fh1_win_t *w = fh1_win_create("", 240, 210);
    if (!w) return;
    memset(&s_user, 0, sizeof(s_user));
    s_user.win = w;
    strlcpy(s_user.id, id, sizeof(s_user.id));
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, user_closed, NULL);
    user_fill();
}

// ── From a controller's window: whose it is ──

static char s_belong_pad[18];
static char s_belong_ids[FH1_USERS_MAX + 1][FH1_USER_ID_LEN + 1];
static fh1_win_t *s_belong_win;

static void belong_go(void)
{
    esp_err_t r = fh1_users_link_pad(s_belong_pad, s_flow.id);
    flow_clear();
    if (r != ESP_OK) err_message(r);
}

static void belong_pick(void *u)
{
    const int i = (int)(intptr_t)u;
    if (s_belong_win) fh1_win_close(s_belong_win);
    s_belong_win = NULL;
    if (!s_belong_ids[i][0]) {
        fh1_users_link_pad(s_belong_pad, "");
        return;
    }
    with_pin(s_belong_ids[i], belong_go);
}

static void belong_closed(fh1_win_t *w, void *u)
{
    if (s_belong_win == w) s_belong_win = NULL;
}

void users_pick_for_pad(const char *pad_id)
{
    if (!fh1_users_count()) {
        fh1_ui_message(T("Belongs to"), T("No users yet (the Users menu)."));
        return;
    }
    fh1_win_t *w = fh1_win_create(T("Belongs to"), 220, 160);
    if (!w) return;
    s_belong_win = w;
    strlcpy(s_belong_pad, pad_id, sizeof(s_belong_pad));
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, belong_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    char cur[FH1_USER_ID_LEN + 1] = "";
    fh1_users_pad_user(pad_id, cur);
    int focus = 0, k = 0;
    s_belong_ids[k][0] = 0;
    fh1_ui_button(w, body, T("Nobody"), belong_pick, (void *)(intptr_t)k++);
    for (int i = 0; i < fh1_users_count(); i++) {
        fh1_user_t u;
        if (!fh1_users_at(i, &u)) continue;
        char b[40];
        label_of(&u, b, sizeof(b));
        strlcpy(s_belong_ids[k], u.id, sizeof(s_belong_ids[k]));
        if (strcmp(u.id, cur) == 0) focus = k;
        fh1_ui_button(w, body, b, belong_pick, (void *)(intptr_t)k++);
    }
    fh1_ui_focus_index(w, focus);
}

// "ana#3FQ1", or "" when the controller belongs to nobody.
void users_pad_owner_text(const char *pad_id, char *out, size_t n)
{
    char id[FH1_USER_ID_LEN + 1];
    fh1_user_t u;
    if (fh1_users_pad_user(pad_id, id) && fh1_users_get(id, &u)) label_of(&u, out, n);
    else if (n) out[0] = 0;
}

// ── Notifications with a face (U3) ──

typedef struct {
    fh1_avatar_t a;
    uint8_t expr;
} face_note_t;

static void note_face(const void *user, uint32_t t, uint32_t *px)
{
    const face_note_t *f = user;
    fh1_avatar_render_face(&f->a, f->expr, t, 32, px);
}

// A notification: id's face with an expression, their name as the title (a contact's with their tag).
static void notify_face(const char *id, int expr, const char *text)
{
    face_note_t f = { .expr = (uint8_t)expr };
    avatar_any(id, &f.a);
    char title[40] = "";
    fh1_user_t u;
    fh1_contact_t c;
    if (fh1_users_get(id, &u)) snprintf(title, sizeof(title), "%s", u.name);
    else if (fh1_contacts_get(id, &c)) snprintf(title, sizeof(title), "%s#%s", c.u.name, c.u.tag);
    fh1_ui_notify(title, text, 32, 32, note_face, &f, sizeof(f));
}

void users_notify_contact(const char *id, const char *text) { notify_face(id, FH1_AV_HAPPY, text); }

// Who plays, when an app starts: player 1's face, and player 2 if they are someone.
void users_announce_players(void)
{
    fh1_user_t p1, p2;
    if (!fh1_users_count() || !fh1_users_player(0, &p1)) return;
    char b[96];
    if (fh1_users_player(1, &p2) && strcmp(p1.id, p2.id) != 0) snprintf(b, sizeof(b), T("plays as player 1, %s as player 2"), p2.name);
    else snprintf(b, sizeof(b), "%s", T("plays as player 1"));
    notify_face(p1.id, FH1_AV_HAPPY, b);
}

// Controllers and players: a user's controller connecting, player 1 changing hands.
static struct {
    char seen[FH1_GAMEPADS_MAX][18];
    int nseen;
    char p1[FH1_USER_ID_LEN + 1];
    bool started;
} s_watch;

static void watch_tick(lv_timer_t *t)
{
    char now[FH1_GAMEPADS_MAX][18];
    int n = 0;
    fh1_gamepad_info_t g;
    for (int i = 0; n < FH1_GAMEPADS_MAX && fh1_gamepad_at(i, &g, NULL); i++) {
        strlcpy(now[n++], g.id, 18);
        bool known = false;
        for (int j = 0; j < s_watch.nseen && !known; j++) known = strcmp(s_watch.seen[j], g.id) == 0;
        char uid[FH1_USER_ID_LEN + 1];
        if (!known && s_watch.started && fh1_users_pad_user(g.id, uid)) {
            char b[64];
            snprintf(b, sizeof(b), T("%s connected"), g.name);
            notify_face(uid, FH1_AV_HAPPY, b);
        }
    }
    memcpy(s_watch.seen, now, sizeof(now));
    s_watch.nseen = n;
    fh1_user_t u;
    const char *p1 = fh1_users_player(0, &u) ? u.id : "";
    if (strcmp(p1, s_watch.p1) != 0) {
        if (s_watch.started && p1[0]) notify_face(p1, FH1_AV_WINK, T("is player 1 now"));
        strlcpy(s_watch.p1, p1, sizeof(s_watch.p1));
        if (s_list.menu_changed) s_list.menu_changed();      // the "1P" marks
    }
    s_watch.started = true;
}

// ── People nearby: other units' users (contacts, U4) ──

static struct {
    fh1_win_t *win, *one;
    char (*ids)[FH1_USER_ID_LEN + 1];  // FH1_CONTACTS_MAX, in PSRAM
    char id[FH1_USER_ID_LEN + 1];
} s_ct;

static void contact_open(void *u);

static void contacts_fill(void)
{
    if (!s_ct.win || !fh1_win_is_open(s_ct.win) || !s_ct.ids) return;
    const int focus = fh1_ui_focused_index(s_ct.win);
    fh1_win_clear(s_ct.win);
    lv_obj_t *body = fh1_win_body(s_ct.win);
    const int n = fh1_contacts_count();
    if (!n) fh1_ui_label(body, T("Nobody yet. Users of other FH1s on the same Wi-Fi show here by themselves (checked every minute)."));
    for (int i = 0; i < n; i++) {
        fh1_contact_t c;
        if (!fh1_contacts_at(i, &c)) continue;
        strlcpy(s_ct.ids[i], c.u.id, sizeof(s_ct.ids[i]));
        char b[48];
        snprintf(b, sizeof(b), "%s#%s", c.u.name, c.u.tag);         // others may share a name: the tag tells them apart
        face_button(s_ct.win, body, c.u.id, b, contact_open, (void *)(intptr_t)i);
    }
    fh1_ui_button(s_ct.win, body, T("Look now"), (void (*)(void *))net_kick, NULL);
    if (focus >= 0) fh1_ui_focus_index(s_ct.win, focus);
}

static void ct_closed(fh1_win_t *w, void *u)
{
    if (s_ct.win == w) s_ct.win = NULL;
    if (s_ct.one == w) s_ct.one = NULL;
}

static void contacts_open(void *u)
{
    if (!s_ct.ids) s_ct.ids = heap_caps_calloc(FH1_CONTACTS_MAX, sizeof(*s_ct.ids), MALLOC_CAP_SPIRAM);
    if (s_ct.win && fh1_win_is_open(s_ct.win)) {
        fh1_win_focus(s_ct.win);
        return;
    }
    fh1_win_t *w = fh1_win_create(T("People nearby"), 240, 190);
    if (!w) return;
    s_ct.win = w;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, ct_closed, NULL);
    contacts_fill();
}

static void ct_forget_yes(bool yes, void *u)
{
    if (!yes) return;
    fh1_contacts_remove(s_ct.id);
    if (s_ct.one) fh1_win_close(s_ct.one);
    contacts_fill();
}

static void ct_forget(void *u) { fh1_ui_confirm(T("People nearby"), T("Forget this person? They come back if their FH1 is seen again."), ct_forget_yes, NULL); }

static void contact_open(void *u)
{
    fh1_contact_t c;
    if (!fh1_contacts_at((int)(intptr_t)u, &c)) return;
    if (s_ct.one && fh1_win_is_open(s_ct.one)) fh1_win_close(s_ct.one);
    char t[48];
    snprintf(t, sizeof(t), "%s#%s", c.u.name, c.u.tag);
    fh1_win_t *w = fh1_win_create(t, 230, 170);
    if (!w) return;
    s_ct.one = w;
    strlcpy(s_ct.id, c.u.id, sizeof(s_ct.id));
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, ct_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    face_add(fh1_ui_pixels(body, 32, 32, 1), c.u.id, FH1_AV_HAPPY, 32);
    char b[40];
    snprintf(b, sizeof(b), "%.4s", c.unit);
    fh1_ui_value_set(fh1_ui_value(body, T("Their FH1")), c.unit[0] ? b : "-");
    snprintf(b, sizeof(b), "%lu.%lu.%lu.%lu", (unsigned long)(c.ip & 0xff), (unsigned long)(c.ip >> 8 & 0xff),
             (unsigned long)(c.ip >> 16 & 0xff), (unsigned long)(c.ip >> 24));
    fh1_ui_value_set(fh1_ui_value(body, T("Address")), c.ip ? b : "-");
    fh1_ui_value_set(fh1_ui_value(body, "Id"), c.u.id);
    fh1_ui_button(w, body, T("Forget"), ct_forget, NULL);
}

// ── Friends (U5): a local user's friends, requests both ways, adding and removing ──

static struct {
    fh1_win_t *win, *add;
    char local[FH1_USER_ID_LEN + 1];
    char (*ids)[FH1_USER_ID_LEN + 1];  // FR_MAX, in PSRAM
    char other[FH1_USER_ID_LEN + 1];
    uint8_t level;
} s_fr;

static const char *who_name(const char *id, char *b, size_t n)
{
    fh1_user_t u;
    fh1_contact_t c;
    if (fh1_users_get(id, &u)) snprintf(b, n, "%s", u.name);
    else if (fh1_contacts_get(id, &c)) snprintf(b, n, "%s#%s", c.u.name, c.u.tag);
    else snprintf(b, n, "#%.4s", id);
    return b;
}

static void friend_go(void)
{
    esp_err_t r = fh1_users_trust(s_fr.local, s_fr.other, s_fr.level, s_flow.pin);
    flow_clear();
    if (r != ESP_OK) {
        err_message(r);
        return;
    }
    net_kick();                                       // deliver it now
    if (s_fr.add) fh1_win_close(s_fr.add);
}

// Says level about other, as the window's user (their PIN).
static void say(const char *other, uint8_t level)
{
    strlcpy(s_fr.other, other, sizeof(s_fr.other));
    s_fr.level = level;
    with_pin(s_fr.local, friend_go);
}

static void fr_undo_yes(bool yes, void *u);
static void fr_add_open(void *u);

static void fr_row(void *u)
{
    const char *other = s_fr.ids[(int)(intptr_t)u];
    uint8_t out, in;
    fh1_users_relation(s_fr.local, other, &out, &in);
    char b[160], who[40];
    who_name(other, who, sizeof(who));
    if (in >= FH1_TRUST_FRIEND && out < FH1_TRUST_FRIEND) {          // their request: accept
        say(other, FH1_TRUST_FRIEND);
        return;
    }
    if (out >= FH1_TRUST_FRIEND) {                                     // a friend, or our request: undo
        snprintf(b, sizeof(b), in >= FH1_TRUST_FRIEND ? T("Stop being friends with %s?") : T("Take back the request to %s?"), who);
        strlcpy(s_fr.other, other, sizeof(s_fr.other));
        fh1_ui_confirm(T("Friends"), b, fr_undo_yes, NULL);
    }
}

static void fr_undo_yes(bool yes, void *u)
{
    if (yes) say(s_fr.other, FH1_TRUST_NONE);
}

static void fr_fill(void)
{
    if (!s_fr.win || !fh1_win_is_open(s_fr.win) || !s_fr.ids) return;
    const int focus = fh1_ui_focused_index(s_fr.win);
    fh1_win_clear(s_fr.win);
    lv_obj_t *body = fh1_win_body(s_fr.win);
    static char (*ids)[FH1_USER_ID_LEN + 1];
    if (!ids) ids = heap_caps_malloc(FR_MAX * sizeof(*ids), MALLOC_CAP_SPIRAM);
    if (!ids) return;
    const int n = fh1_users_relations(s_fr.local, ids, FR_MAX);
    int k = 0;
    // Requests to answer first, then friends, then requests sent.
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < n; i++) {
            uint8_t out, in;
            fh1_users_relation(s_fr.local, ids[i], &out, &in);
            const bool req = in >= FH1_TRUST_FRIEND && out < FH1_TRUST_FRIEND, fr = in >= FH1_TRUST_FRIEND && out >= FH1_TRUST_FRIEND,
                       sent = out >= FH1_TRUST_FRIEND && in < FH1_TRUST_FRIEND;
            if (!((pass == 0 && req) || (pass == 1 && fr) || (pass == 2 && sent))) continue;
            char b[64], who[40];
            who_name(ids[i], who, sizeof(who));
            snprintf(b, sizeof(b), "%s%s", who, req ? T(" · wants to be friends (A: accept)") : sent ? T(" · waiting") : "");
            strlcpy(s_fr.ids[k], ids[i], sizeof(s_fr.ids[k]));
            face_button(s_fr.win, body, ids[i], b, fr_row, (void *)(intptr_t)k++);
        }
    }
    if (!k) fh1_ui_label(body, T("No friends yet."));
    fh1_ui_button(s_fr.win, body, T("Add a friend"), fr_add_open, NULL);
    if (focus >= 0) fh1_ui_focus_index(s_fr.win, focus);
}

// Adding: anyone this unit knows who is not a friend yet (people nearby, and the other users here).
static void fr_add_pick(void *u) { say(s_fr.ids[(int)(intptr_t)u], FH1_TRUST_FRIEND); }

static void fr_add_closed(fh1_win_t *w, void *u)
{
    if (s_fr.add == w) s_fr.add = NULL;
}

static void fr_add_open(void *u)
{
    if (!s_fr.ids) return;
    fh1_win_t *w = fh1_win_create(T("Add a friend"), 230, 170);
    if (!w) return;
    s_fr.add = w;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Ask") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, fr_add_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    int k = 0;
    char b[48];
    for (int i = 0; i < fh1_users_count() + fh1_contacts_count(); i++) {
        char id[FH1_USER_ID_LEN + 1];
        if (i < fh1_users_count()) {
            fh1_user_t x;
            if (!fh1_users_at(i, &x) || strcmp(x.id, s_fr.local) == 0) continue;
            strlcpy(id, x.id, sizeof(id));
        } else {
            fh1_contact_t c;
            if (!fh1_contacts_at(i - fh1_users_count(), &c)) continue;
            strlcpy(id, c.u.id, sizeof(id));
        }
        uint8_t out;
        fh1_users_relation(s_fr.local, id, &out, NULL);
        if (out >= FH1_TRUST_FRIEND) continue;
        strlcpy(s_fr.ids[k], id, sizeof(s_fr.ids[k]));
        face_button(w, body, id, who_name(id, b, sizeof(b)), fr_add_pick, (void *)(intptr_t)k++);
    }
    if (!k) fh1_ui_label(body, T("Nobody to ask: other FH1s' users show in People nearby when they are on the same Wi-Fi."));
}

static void fr_closed(fh1_win_t *w, void *u)
{
    if (s_fr.win == w) s_fr.win = NULL;
}

static void friends_open(void *u)
{
    if (!s_fr.ids) s_fr.ids = heap_caps_calloc(FR_MAX, sizeof(*s_fr.ids), MALLOC_CAP_SPIRAM);
    fh1_user_t me;
    if (!fh1_users_get(s_user.id, &me)) return;
    if (s_fr.win && fh1_win_is_open(s_fr.win)) fh1_win_close(s_fr.win);
    char t[48];
    snprintf(t, sizeof(t), T("Friends of %s"), me.name);
    fh1_win_t *w = fh1_win_create(t, 250, 190);
    if (!w) return;
    s_fr.win = w;
    strlcpy(s_fr.local, me.id, sizeof(s_fr.local));
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, fr_closed, NULL);
    fr_fill();
}

// ── From the network (apps_net.c) ──

void users_net_changed(void)
{
    if (s_list.menu_changed) s_list.menu_changed();
    contacts_fill();
    fr_fill();
}

void users_trust_arrived(const char *local, const char *from, uint8_t level)
{
    uint8_t out;
    fh1_users_relation(local, from, &out, NULL);
    fh1_user_t me;
    if (!fh1_users_get(local, &me)) return;
    char b[96];
    if (level >= FH1_TRUST_FRIEND && out >= FH1_TRUST_FRIEND) {
        snprintf(b, sizeof(b), T("and %s are friends now"), me.name);
        notify_face(from, FH1_AV_LOVE, b);
    } else if (level >= FH1_TRUST_FRIEND) {
        snprintf(b, sizeof(b), T("wants to be friends with %s (Users > %s > Friends)"), me.name, me.name);
        notify_face(from, FH1_AV_HAPPY, b);
    } else if (out >= FH1_TRUST_FRIEND) {
        snprintf(b, sizeof(b), T("is no longer friends with %s"), me.name);
        notify_face(from, FH1_AV_SAD, b);
    }
}

// ── For the SDK (run.c) ──

lv_obj_t *users_face_widget(lv_obj_t *parent, const char *id, int size)
{
    lv_obj_t *img = fh1_ui_pixels(parent, size, size, 1);
    face_add(img, id, FH1_AV_NEUTRAL, size);
    return img;
}

void users_notify_player(int player, const char *text)
{
    fh1_user_t u;
    if (fh1_users_player(player, &u)) notify_face(u.id, FH1_AV_HAPPY, text);
    else fh1_ui_notify("FH1", text, 0, 0, NULL, NULL, 0);
}

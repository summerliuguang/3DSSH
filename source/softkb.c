#include "softkb.h"
#include "renderer.h"
#include "keyboard.h"
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ──────────────────────────────────────────────────────────────────────
 * Geometry — bottom screen 320×240, top-left = (0,0)
 * ──────────────────────────────────────────────────────────────────────
 *  y =  0 .. 33   status / candidate row (34 px — bumped from 30 in
 *                  M4 polish so IME candidates have more vertical room)
 *  y = 34 .. 35   margin
 *  y = 36 .. 78   key row 0 (43 px — 1 px shaved from M4-first to free
 *                  space for the bottom row at y=214..239)
 *  y = 81 .. 123  key row 1
 *  y = 126 .. 168 key row 2
 *  y = 171 .. 213 key row 3
 *  y = 214..239   bottom row owned by main.c — clock on the left,
 *                  Anthropic crab mascot on the right.  softkb does NOT
 *                  draw here.
 *
 *  Stagger (mimics PC keyboards: each row is offset by ~half a key):
 *    row 0: x offset =  0   (10 keys × 32 px = 320 — exact fit)
 *    row 1: x offset = 16   (9 keys → ends at 304, 16 px right margin)
 *    row 2: x offset = 32   (9 keys → ends at 320 — exact fit)
 *    row 3: x offset =  0   (control row; large keys, no stagger)
 *
 *  z layers (citro2d uses depth for overdraw ordering with alpha):
 *    0.05  bottom screen background
 *    0.10  key shadow / outline
 *    0.15  key body
 *    0.18  key top highlight
 *    0.20  key label glyph
 *    0.30  status row (drawn earlier so labels overlay)
 */

#define KEY_W       32
#define KEY_H       43
#define KEY_GAP_Y   2
#define ROW_BASE_Y  36
#define ROW_Y(r)    (ROW_BASE_Y + (r) * (KEY_H + KEY_GAP_Y))

#define ROW_X_0     0
#define ROW_X_1     16
#define ROW_X_2     32
#define ROW_X_3     0

#define STATUS_Y    0
#define STATUS_H    34
#define CELL_W      6
#define CELL_H      12

/* ── Colors (RGBA 0xRRGGBBAA, Catppuccin Mocha-ish) ────────────────── */
#define COL_BG              0x11111bff   /* darkest, bottom screen base */
#define COL_STATUS_BG       0x181825ff   /* 1 step lighter — status row */
#define COL_CANDIDATE_BG    0x1e1e2eff   /* candidate strip background */
#define COL_CHAMFER         0x181825ff   /* corner punch-out — must match
                                          * the C2D_TargetClear bg color
                                          * in main.c so the rounded
                                          * corners blend invisibly */

#define COL_KEY_BODY        0x313244ff   /* normal key */
#define COL_KEY_TOP         0x45475aff   /* 1px highlight on top edge */
#define COL_KEY_BOT_SHADOW  0x06060eff   /* 1px shadow under key */
#define COL_KEY_BORDER      0x1e1e2eff   /* outline */
#define COL_KEY_LABEL       0xcdd6f4ff

#define COL_KEY_SPECIAL     0x45475a8b   /* slightly different grey */
#define COL_KEY_PG_BODY     0x74c7ecff   /* page-switch — sky blue */
#define COL_KEY_SPACE_BODY  0x6c7086ff   /* space — neutral mid grey */

#define COL_KEY_PRESSED     0x89b4faff   /* tap highlight */
#define COL_KEY_PRESSED_FG  0x11111bff   /* dark label on bright press */

#define COL_STATUS_FG_HOLD  0xa6e3a1ff   /* green when modifier held */
#define COL_STATUS_FG_FLASH 0xfab387ff   /* orange when transient flash */
#define COL_STATUS_DIM      0x45475aff
#define COL_STATUS_HOLD_BG  0x89b4fa50   /* faint blue tint behind held mod */

#define COL_MODE_EN         0xf5c2e7ff
#define COL_MODE_CN         0xf9e2afff
#define COL_MODE_LBL_BG     0x313244ff

/* ── key kinds ─────────────────────────────────────────────────────── */

typedef enum {
    KIND_CHAR,
    KIND_SEQ,
    KIND_PAGE_TOGGLE,
    KIND_SPACE,        /* visual variant: large neutral */
    KIND_PAGE_BTN,     /* visual variant: page switch — accent color */
} key_kind_t;

typedef struct {
    int x, y, w, h;
    char base;
    const char *seq;
    const char *label;
    key_kind_t kind;
} softkey_t;

#define K(col, row, ch, lbl) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), KEY_W - 2, KEY_H, \
      (ch), NULL, (lbl), KIND_CHAR }

#define KW(col, row, span, ch, lbl, kind_) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      (ch), NULL, (lbl), (kind_) }

#define KS(col, row, span, seq_, lbl, kind_) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      0, (seq_), (lbl), (kind_) }

#define KP(col, row, span, lbl) \
    { ROW_X_##row + (col) * KEY_W + 1, ROW_Y(row), (span) * KEY_W - 2, KEY_H, \
      0, NULL, (lbl), KIND_PAGE_BTN }

/* ── Page 1: Letters ───────────────────────────────────────────────── */
static const softkey_t keys_letters[] = {
    /* row 0 (qwerty), 10 keys, no stagger */
    K(0,0,'q',"q"), K(1,0,'w',"w"), K(2,0,'e',"e"), K(3,0,'r',"r"), K(4,0,'t',"t"),
    K(5,0,'y',"y"), K(6,0,'u',"u"), K(7,0,'i',"i"), K(8,0,'o',"o"), K(9,0,'p',"p"),
    /* row 1 (asdf), 9 keys, half-key stagger */
    K(0,1,'a',"a"), K(1,1,'s',"s"), K(2,1,'d',"d"), K(3,1,'f',"f"), K(4,1,'g',"g"),
    K(5,1,'h',"h"), K(6,1,'j',"j"), K(7,1,'k',"k"), K(8,1,'l',"l"),
    /* row 2 (zxcv), 9 keys, full-key stagger */
    K(0,2,'z',"z"), K(1,2,'x',"x"), K(2,2,'c',"c"), K(3,2,'v',"v"), K(4,2,'b',"b"),
    K(5,2,'n',"n"), K(6,2,'m',"m"), K(7,2,',',","), K(8,2,'.',"."),
    /* row 3 (controls): [123] (2 cols) | tab (2 cols) | space (6 cols).
     * [123] is sized to match [abc] on the symbols page (also w=2 at
     * col 0) — both position AND width align across pages, so the
     * page-toggle target is identical no matter which page you're on.
     * tab gets w=2 for an easier touch hit than the previous w=1. */
    KP(0,3,2,"123"),
    KS(2,3,2,"\t","tab", KIND_SEQ),
    KW(4,3,6,' ',"space", KIND_SPACE),
};
#define N_LETTERS (sizeof(keys_letters) / sizeof(keys_letters[0]))

/* Absolute-X variant of K — used for the symbols row 1 where we want
 * 10 keys with no stagger (so `(` and `)` line up with `9` and `0`). */
#define KX(absx, row, ch, lbl) \
    { (absx), ROW_Y(row), KEY_W - 2, KEY_H, (ch), NULL, (lbl), KIND_CHAR }

/* ── Page 2: Symbols / Numbers ─────────────────────────────────────── */
static const softkey_t keys_symbols[] = {
    /* row 0: 1-9 0 */
    K(0,0,'1',"1"), K(1,0,'2',"2"), K(2,0,'3',"3"), K(3,0,'4',"4"), K(4,0,'5',"5"),
    K(5,0,'6',"6"), K(6,0,'7',"7"), K(7,0,'8',"8"), K(8,0,'9',"9"), K(9,0,'0',"0"),
    /* row 1: 10 shifted-numbers, no stagger so '(' and ')' align with '9' '0' */
    KX(  1,1,'!',"!"), KX( 33,1,'@',"@"), KX( 65,1,'#',"#"), KX( 97,1,'$',"$"),
    KX(129,1,'%',"%"), KX(161,1,'^',"^"), KX(193,1,'&',"&"), KX(225,1,'*',"*"),
    KX(257,1,'(',"("), KX(289,1,')',")"),
    /* row 2: 10 punctuation, no stagger to keep alignment with rows 0/1
     * and to fit '?' (was missing from M4 layout, added in M7 polish 2). */
    KX(  1,2,'-',"-"), KX( 33,2,'+',"+"), KX( 65,2,'=',"="), KX( 97,2,'[',"["),
    KX(129,2,']',"]"), KX(161,2,';',";"), KX(193,2,':',":"), KX(225,2,'\'',"'"),
    KX(257,2,'/',"/"), KX(289,2,'?',"?"),
    /* row 3: [abc] | ` < > | space (4 cols) | \ ~ */
    KP(0,3,2,"abc"),
    K(2,3,'`',"`"),
    K(3,3,'<',"<"),
    K(4,3,'>',">"),
    KW(5,3,4,' ',"space", KIND_SPACE),
    K(9,3,'\\',"\\"),
};
#define N_SYMBOLS (sizeof(keys_symbols) / sizeof(keys_symbols[0]))

/* ── softkb_t ──────────────────────────────────────────────────────── */

struct softkb_t {
    softkb_page_t page;
    int           pressed_idx;     /* index of currently-touched key, -1 = none */
    int           pressed_frames;  /* visual press-down animation timer */
    char          out_buf[16];
    int           out_len;

    /* Hold-to-repeat state — independent from the visual press animation
     * because that one fades after 14 frames while a true hold may last
     * arbitrarily long.
     *   prev_pressed         touch state on the previous frame (for
     *                        down-edge detection inside softkb_touch)
     *   repeat_idx           key currently being held; -1 = none.  Set to
     *                        -2 ("swallow until release") right after a
     *                        page toggle so the new layout's key under
     *                        the finger doesn't get fired.
     *   repeat_held_frames   frames since first contact with repeat_idx;
     *                        compared against DPAD-style ramp constants.
     */
    int           prev_pressed;
    int           repeat_idx;
    int           repeat_held_frames;

    /* Debug page state */
    int           debug_mode;            /* 0 = keyboard, 1 = debug overlay */
    int           badge_last_tap_frame;  /* kbd->frame at last badge tap */
    int           mascot_enabled;        /* 1 = crab visible (default) */

    /* Recv ring — last up to 32 SSH-bound bytes for debug display.
     * recv_head is the next write slot; recv_count saturates at 32. */
    uint8_t       recv_ring[32];
    int           recv_head;
    int           recv_count;

    /* M7 IME: pinyin engine pointer (may be NULL if dict failed to load).
     * The candidate-strip layout is recomputed each frame in
     * draw_status_row and stored here so softkb_touch can hit-test
     * tapped candidates back to indices. */
    ime_t        *ime;
    int           cand_box_x[IME_PAGE_SIZE];
    int           cand_box_w[IME_PAGE_SIZE];
    int           cand_box_n;            /* candidates currently visible */

    /* M11 voice input — read-only handle, NULL when voice disabled.
     * draw_status_row queries it each frame so the REC / spinner / ERR
     * badge preempts the modifier label. */
    const voice_t *voice;

    /* ── Settings page state ──
     * cfg points at main.c's ssh_config_t (NULL until softkb_set_config).
     * set_srv is the server slot being viewed/edited (0-based, may point
     * one past server_count = "new server" slot).  set_edit >= 0 means a
     * field edit is in progress and set_edit_buf holds the working copy. */
    ssh_config_t  *cfg;
    int           settings_mode;
    int           set_srv;
    int           set_edit;
    char          set_edit_buf[CONFIG_STR_MAX];
    softkb_action_t set_action;

    /* ── Window switcher (WIN button) ── */
    char          win_label[8];     /* "2/3" — active/total */
    int           win_total;
};

softkb_t *softkb_init(ime_t *ime) {
    softkb_t *kb = calloc(1, sizeof(*kb));
    if (!kb) return NULL;
    kb->page = PAGE_LETTERS;
    kb->pressed_idx = -1;
    kb->repeat_idx = -1;
    kb->mascot_enabled = 1;
    /* Far-past sentinel so the very first badge tap can never look
     * like the second half of a double-tap. */
    kb->badge_last_tap_frame = -1000;
    kb->ime = ime;
    kb->cfg = NULL;
    kb->settings_mode = 0;
    kb->set_srv = 0;
    kb->set_edit = -1;
    kb->set_action = SOFTKB_ACT_NONE;
    return kb;
}

void softkb_set_config(softkb_t *kb, ssh_config_t *cfg) {
    if (!kb) return;
    kb->cfg = cfg;
    if (cfg) kb->set_srv = cfg->active_server;
}

void softkb_set_win_info(softkb_t *kb, const char *label, int total) {
    if (!kb) return;
    snprintf(kb->win_label, sizeof(kb->win_label), "%s",
             label ? label : "");
    kb->win_total = total;
}

int softkb_in_settings(const softkb_t *kb) {
    return kb ? kb->settings_mode : 0;
}

int softkb_settings_editing(const softkb_t *kb) {
    return kb ? (kb->settings_mode && kb->set_edit >= 0) : 0;
}

/* Defined with the settings geometry below — needed early because
 * main.c routes bottom-row taps through them. */
static int setbtn_hit(int tx, int ty);
static int winbtn_hit(int tx, int ty);
int softkb_settings_button_hit(const softkb_t *kb, int tx, int ty);

int softkb_settings_button_hit(const softkb_t *kb, int tx, int ty) {
    (void)kb;
    return setbtn_hit(tx, ty);
}

int softkb_win_button_hit(const softkb_t *kb, int tx, int ty) {
    (void)kb;
    return winbtn_hit(tx, ty);
}

void softkb_free(softkb_t *kb) { free(kb); }

void softkb_set_ime(softkb_t *kb, ime_t *ime) {
    if (!kb) return;
    kb->ime = ime;
}

void softkb_set_voice(softkb_t *kb, const voice_t *v) {
    if (!kb) return;
    kb->voice = v;
}

softkb_page_t softkb_current_page(const softkb_t *kb) {
    return kb ? kb->page : PAGE_LETTERS;
}

void softkb_record_recv(softkb_t *kb, const char *bytes, int n) {
    if (!kb || !bytes || n <= 0) return;
    for (int i = 0; i < n; i++) {
        kb->recv_ring[kb->recv_head] = (uint8_t)bytes[i];
        kb->recv_head = (kb->recv_head + 1) % 32;
        if (kb->recv_count < 32) kb->recv_count++;
    }
}

int softkb_in_debug(const softkb_t *kb) {
    return kb ? kb->debug_mode : 0;
}

int softkb_mascot_enabled(const softkb_t *kb) {
    return kb ? kb->mascot_enabled : 1;
}

/* ── Hit-test geometry for the right-side ENG/CHN badge and the
 *    debug page's mascot toggle button.  Mirrors the layout in
 *    draw_status_row / draw_debug_screen so a tap maps 1:1. */
#define BADGE_W       (3 * CELL_W + 4)        /* 22 px — same as slot_w */
#define BADGE_H       (STATUS_H - 4)          /* 30 px — same as slot_h */
#define BADGE_X       (320 - BADGE_W - 2)     /* 296 */
#define BADGE_Y       2

#define DBG_TOGGLE_X  60
#define DBG_TOGGLE_Y  170
#define DBG_TOGGLE_W  200
#define DBG_TOGGLE_H  40

/* Number of frames that count as a "double" tap on the badge.
 * 30 frames ≈ 500 ms at 60 fps — matches iOS double-tap window. */
#define BADGE_DOUBLE_TAP_FRAMES 30

static int badge_hit(int tx, int ty) {
    return tx >= BADGE_X && tx < BADGE_X + BADGE_W &&
           ty >= BADGE_Y && ty < BADGE_Y + BADGE_H;
}

static int dbg_toggle_hit(int tx, int ty) {
    return tx >= DBG_TOGGLE_X && tx < DBG_TOGGLE_X + DBG_TOGGLE_W &&
           ty >= DBG_TOGGLE_Y && ty < DBG_TOGGLE_Y + DBG_TOGGLE_H;
}

/* ── settings page geometry ────────────────────────────────────────── */

/* Pinned SET button — bottom-right corner of the bottom row (the strip
 * main.c owns for clock + mascot).  Drawn in normal AND settings mode;
 * hidden on the debug page. */
#define SETBTN_W     28
#define SETBTN_H     22
#define SETBTN_X     (320 - SETBTN_W - 2)    /* 290 */
#define SETBTN_Y     (240 - SETBTN_H - 2)    /* 216 */

/* Pinned WIN button — sits left of SET, switches SSH windows. */
#define WINBTN_W     28
#define WINBTN_H     22
#define WINBTN_X     (SETBTN_X - WINBTN_W - 2)   /* 260 */
#define WINBTN_Y     SETBTN_Y

#define SET_TITLE_Y    40
#define SET_SEL_Y      58
#define SET_SEL_H      18
#define SET_SEL_BTN_W  20
#define SET_SEL_L_X    8
#define SET_SEL_R_X    76
#define SET_ROW_Y0     82
#define SET_ROW_H      15
#define SET_ROW_LABEL_X  6
#define SET_ROW_VALUE_X  72
#define SET_ROW_VALUE_W  236   /* 72 + 236 = 308 */
#define SET_BTN_Y      192
#define SET_BTN_H      18
#define SET_SAVE_X       6
#define SET_SAVE_W      84
#define SET_RECONN_X    98
#define SET_RECONN_W   126
/* Edit bar occupies the status row's band (y 0..34) so the keyboard
 * grid (y 36..) stays fully tappable while editing — see
 * draw_settings_screen for why this matters. */
#define SET_EDITBAR_Y  0
#define SET_EDITBAR_H  STATUS_H

static int setbtn_hit(int tx, int ty) {
    return tx >= SETBTN_X && tx < SETBTN_X + SETBTN_W &&
           ty >= SETBTN_Y && ty < SETBTN_Y + SETBTN_H;
}

static int winbtn_hit(int tx, int ty) {
    return tx >= WINBTN_X && tx < WINBTN_X + WINBTN_W &&
           ty >= WINBTN_Y && ty < WINBTN_Y + WINBTN_H;
}

/* Field order = row order on the settings page. */
typedef enum {
    SET_FLD_HOST = 0,
    SET_FLD_PORT,
    SET_FLD_USER,
    SET_FLD_AUTH,
    SET_FLD_PASSWORD,
    SET_FLD_KEYPATH,
    SET_FLD_VOICEAPI,
    SET_FLD_COUNT
} settings_field_t;

static const char *set_field_label(settings_field_t f) {
    switch (f) {
        case SET_FLD_HOST:     return "HOST";
        case SET_FLD_PORT:     return "PORT";
        case SET_FLD_USER:     return "USER";
        case SET_FLD_AUTH:     return "AUTH";
        case SET_FLD_PASSWORD: return "PASSWORD";
        case SET_FLD_KEYPATH:  return "KEY PATH";
        case SET_FLD_VOICEAPI: return "VOICE API";
        default:               return "?";
    }
}

/* Current value of a field as a display/edit string.  PORT goes through
 * the caller's small scratch buffer. */
static const char *set_field_value(const ssh_config_t *cfg, int slot,
                                   settings_field_t f,
                                   char *tmp, int tmp_sz) {
    const ssh_server_t *s = &cfg->servers[slot];
    switch (f) {
        case SET_FLD_HOST:     return s->host;
        case SET_FLD_PORT:     snprintf(tmp, (size_t)tmp_sz, "%d", s->port); return tmp;
        case SET_FLD_USER:     return s->user;
        case SET_FLD_AUTH:     return s->auth == SSH_AUTH_PASSWORD ? "password" : "key";
        case SET_FLD_PASSWORD: return s->password;
        case SET_FLD_KEYPATH:  return s->key_path;
        case SET_FLD_VOICEAPI: return cfg->voice_api_url;
        default:               return "";
    }
}

static void set_field_commit(ssh_config_t *cfg, int slot,
                             settings_field_t f, const char *val) {
    ssh_server_t *s = &cfg->servers[slot];
    switch (f) {
        case SET_FLD_HOST:
            snprintf(s->host, CONFIG_STR_MAX, "%s", val);
            /* First host typed into an empty slot grows the server list. */
            if (slot >= cfg->server_count && val[0])
                cfg->server_count = slot + 1;
            break;
        case SET_FLD_PORT: {
            int p = atoi(val);
            if (p > 0 && p < 65536) s->port = p;
            break;
        }
        case SET_FLD_USER:     snprintf(s->user, CONFIG_STR_MAX, "%s", val); break;
        case SET_FLD_PASSWORD: snprintf(s->password, CONFIG_STR_MAX, "%s", val); break;
        case SET_FLD_KEYPATH:  snprintf(s->key_path, CONFIG_STR_MAX, "%s", val); break;
        case SET_FLD_VOICEAPI: snprintf(cfg->voice_api_url, CONFIG_STR_MAX, "%s", val); break;
        default: break;
    }
}

static void settings_begin_edit(softkb_t *kb, settings_field_t f) {
    if (!kb->cfg) return;
    char tmp[16];
    const char *cur = set_field_value(kb->cfg, kb->set_srv, f,
                                      tmp, (int)sizeof(tmp));
    snprintf(kb->set_edit_buf, sizeof(kb->set_edit_buf), "%s", cur);
    kb->set_edit = (int)f;
}

void softkb_settings_commit(softkb_t *kb) {
    if (!kb || !kb->cfg || kb->set_edit < 0) return;
    set_field_commit(kb->cfg, kb->set_srv,
                     (settings_field_t)kb->set_edit, kb->set_edit_buf);
    kb->set_edit = -1;
}

void softkb_settings_cancel(softkb_t *kb) {
    if (!kb) return;
    kb->set_edit = -1;
}

void softkb_settings_backspace(softkb_t *kb) {
    if (!kb || kb->set_edit < 0) return;
    int len = (int)strlen(kb->set_edit_buf);
    if (len == 0) return;
    len--;
    /* Keep whole UTF-8 glyphs: strip continuation bytes back to the lead. */
    while (len > 0 &&
           ((unsigned char)kb->set_edit_buf[len] & 0xC0) == 0x80)
        len--;
    kb->set_edit_buf[len] = 0;
}

void softkb_settings_feed(softkb_t *kb, const char *bytes) {
    if (!kb || !bytes || kb->set_edit < 0) return;
    if (bytes[0] == '\x1b') return;   /* escape sequences aren't text */
    int len = (int)strlen(kb->set_edit_buf);
    for (const char *p = bytes; *p && len < CONFIG_STR_MAX - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) continue;       /* control bytes (tab, CR, …) */
        kb->set_edit_buf[len++] = (char)c;
    }
    kb->set_edit_buf[len] = 0;
}

softkb_action_t softkb_settings_consume_action(softkb_t *kb) {
    if (!kb) return SOFTKB_ACT_NONE;
    softkb_action_t a = kb->set_action;
    kb->set_action = SOFTKB_ACT_NONE;
    return a;
}

static int set_row_hit(int tx, int ty) {
    if (tx < 4 || tx > 316) return -1;
    if (ty < SET_ROW_Y0 || ty >= SET_ROW_Y0 + SET_ROW_H * SET_FLD_COUNT)
        return -1;
    return (ty - SET_ROW_Y0) / SET_ROW_H;
}

/* Browse-mode taps: server selector, field rows, SAVE / RECONNECT. */
static void settings_browse_tap(softkb_t *kb, int tx, int ty) {
    ssh_config_t *cfg = kb->cfg;
    if (!cfg) return;

    if (ty >= SET_SEL_Y && ty < SET_SEL_Y + SET_SEL_H) {
        if (tx >= SET_SEL_L_X && tx < SET_SEL_L_X + SET_SEL_BTN_W) {
            if (kb->set_srv > 0) kb->set_srv--;
        } else if (tx >= SET_SEL_R_X && tx < SET_SEL_R_X + SET_SEL_BTN_W) {
            if (kb->set_srv < CONFIG_SERVERS_MAX - 1) kb->set_srv++;
        }
        /* Browsing also activates — but only a configured slot can be
         * the active server, otherwise SELECT would dial an empty host. */
        if (cfg->servers[kb->set_srv].host[0])
            cfg->active_server = kb->set_srv;
        return;
    }

    if (ty >= SET_BTN_Y && ty < SET_BTN_Y + SET_BTN_H) {
        if (tx >= SET_SAVE_X && tx < SET_SAVE_X + SET_SAVE_W)
            kb->set_action = SOFTKB_ACT_SAVE;
        else if (tx >= SET_RECONN_X && tx < SET_RECONN_X + SET_RECONN_W)
            kb->set_action = SOFTKB_ACT_RECONNECT;
        return;
    }

    int row = set_row_hit(tx, ty);
    if (row < 0) return;
    if (row == SET_FLD_AUTH) {
        ssh_server_t *s = &cfg->servers[kb->set_srv];
        s->auth = (s->auth == SSH_AUTH_PASSWORD) ? SSH_AUTH_KEY
                                                 : SSH_AUTH_PASSWORD;
        return;
    }
    settings_begin_edit(kb, (settings_field_t)row);
}

static const softkey_t *current_layout(const softkb_t *kb, int *count) {
    if (kb->page == PAGE_SYMBOLS) {
        *count = N_SYMBOLS; return keys_symbols;
    }
    *count = N_LETTERS; return keys_letters;
}

/* ── touch hit-test ────────────────────────────────────────────────── */

static int hit_test(const softkb_t *kb, int tx, int ty) {
    int n;
    const softkey_t *layout = current_layout(kb, &n);
    for (int i = 0; i < n; i++) {
        const softkey_t *k = &layout[i];
        if (tx >= k->x && tx < k->x + k->w &&
            ty >= k->y && ty < k->y + k->h)
            return i;
    }
    return -1;
}

/* Hold-to-repeat ramp — mirrors keyboard.c's D-pad/Backspace cadence so
 * a held soft key feels identical to a held physical key.  Not exposed
 * from keyboard.c because that would couple the two modules; the
 * constants are short and stable enough to duplicate. */
#define SOFTKB_INITIAL_DELAY 15  /* frames before first auto-repeat (~250 ms) */

static int softkb_repeat_period(int phase) {
    if (phase <  30) return 5;   /* 0.0-0.5 s post-delay: 12 fires/sec */
    if (phase <  90) return 2;   /* 0.5-1.5 s:            30 fires/sec */
    return 1;                    /* >1.5 s:               60 fires/sec */
}

const char *softkb_touch(softkb_t *kb,
                         keyboard_t *kbd,
                         int tx, int ty,
                         int pressed) {
    if (!kb || !kbd) return NULL;

    /* Down-edge derived from per-frame state — required because main.c
     * now passes the held flag (not the down-edge flag) so we can track
     * holds for auto-repeat. */
    int down_edge = pressed && !kb->prev_pressed;
    kb->prev_pressed = pressed;

    if (!pressed) {
        /* Release: clear repeat tracking and let the visual press-down
         * animation fade out naturally. */
        kb->repeat_idx = -1;
        kb->repeat_held_frames = 0;
        if (kb->pressed_idx >= 0) {
            kb->pressed_frames++;
            if (kb->pressed_frames > 14) kb->pressed_idx = -1;
        }
        return NULL;
    }
    if (tx < 0 || ty < 0) return NULL;

    /* All non-key widgets (badge, debug toggle, IME candidate strip)
     * fire only on the down-edge.  Subsequent held frames in those
     * regions are swallowed via repeat_idx = -2 so a swipe from there
     * to a real key during the same gesture also does nothing. */

    /* ── Mode badge: double-tap to enter debug, single-tap to leave ── */
    if (badge_hit(tx, ty)) {
        if (down_edge) {
            int now  = kbd->frame;
            int diff = now - kb->badge_last_tap_frame;
            if (kb->debug_mode) {
                kb->debug_mode = 0;
            } else if (diff > 0 && diff < BADGE_DOUBLE_TAP_FRAMES) {
                kb->debug_mode = 1;
            }
            kb->badge_last_tap_frame = now;
        }
        kb->repeat_idx = -2;
        return NULL;
    }

    /* ── Debug-mode taps go to the debug-page widgets, not to keys. ── */
    if (kb->debug_mode) {
        if (down_edge && dbg_toggle_hit(tx, ty)) {
            kb->mascot_enabled = !kb->mascot_enabled;
        }
        kb->repeat_idx = -2;
        return NULL;
    }

    /* ── Pinned WIN button: cycle SSH windows.  Normal keyboard mode
     * only; a no-op (visually dimmed) while fewer than two servers are
     * configured. ── */
    if (winbtn_hit(tx, ty)) {
        if (down_edge && kb->win_total >= 2) {
            kb->set_action = SOFTKB_ACT_WIN_NEXT;
        }
        kb->repeat_idx = -2;
        return NULL;
    }

    /* ── Pinned SET button: toggle the settings overlay.  Visible on
     * the keyboard AND on the settings page (as its exit button); not
     * on the debug page.  Ignore while a field edit is open so a
     * mis-tap near the corner doesn't discard the buffer. ── */
    if (setbtn_hit(tx, ty)) {
        if (down_edge && kb->cfg && kb->set_edit < 0) {
            kb->settings_mode = !kb->settings_mode;
            if (kb->settings_mode) {
                kb->set_edit = -1;
                kb->set_srv = kb->cfg->active_server;
            }
        }
        kb->repeat_idx = -2;
        return NULL;
    }

    /* ── Settings page ── */
    if (kb->settings_mode) {
        if (kb->set_edit < 0) {
            /* Browse mode: selector / rows / buttons. */
            if (down_edge) settings_browse_tap(kb, tx, ty);
            kb->repeat_idx = -2;
            return NULL;
        }
        /* Edit mode: tapping the edit bar commits (same as A);
         * everything else falls through to the keyboard layout. */
        if (down_edge && ty >= SET_EDITBAR_Y &&
            ty < SET_EDITBAR_Y + SET_EDITBAR_H) {
            softkb_settings_commit(kb);
            kb->repeat_idx = -2;
            return NULL;
        }
    }

    /* ── IME candidate strip: tap on a visible candidate commits it. ── */
    if (kb->ime && ime_active(kb->ime) && ty < STATUS_H &&
        !kb->settings_mode) {
        if (down_edge) {
            for (int i = 0; i < kb->cand_box_n; i++) {
                if (tx >= kb->cand_box_x[i] &&
                    tx <  kb->cand_box_x[i] + kb->cand_box_w[i]) {
                    kb->repeat_idx = -2;
                    return ime_select(kb->ime, i);
                }
            }
        }
        kb->repeat_idx = -2;
        return NULL;
    }

    /* ── Normal keyboard mode: hit-test keys, dispatch a byte. ── */
    int idx = hit_test(kb, tx, ty);
    if (idx < 0) {
        if (down_edge) kb->pressed_idx = -1;
        kb->repeat_idx = -1;
        kb->repeat_held_frames = 0;
        return NULL;
    }

    int n;
    const softkey_t *layout = current_layout(kb, &n);
    const softkey_t *k = &layout[idx];

    /* Decide whether *this* frame fires an emission.  Three cases:
     *   a) repeat_idx == -2  → swallow-until-release sentinel from a
     *      non-key widget tap or a recent page toggle.  No fire.
     *   b) idx != repeat_idx → fresh contact OR finger swiped to a new
     *      key.  Fire once, reset hold counter.
     *   c) idx == repeat_idx → held on the same key.  Fire only once
     *      the initial delay has elapsed and we're on a period boundary.
     */
    int fire = 0;
    if (kb->repeat_idx == -2) {
        return NULL;
    } else if (idx != kb->repeat_idx) {
        kb->repeat_idx         = idx;
        kb->repeat_held_frames = 0;
        kb->pressed_idx        = idx;
        kb->pressed_frames     = 0;
        fire = 1;
    } else {
        kb->repeat_held_frames++;
        if (k->kind == KIND_PAGE_BTN || k->kind == KIND_PAGE_TOGGLE) {
            /* Page toggles never auto-repeat — would whip the page back
             * and forth every few frames. */
        } else if (kb->repeat_held_frames >= SOFTKB_INITIAL_DELAY) {
            int phase  = kb->repeat_held_frames - SOFTKB_INITIAL_DELAY;
            int period = softkb_repeat_period(phase);
            if (phase % period == 0) fire = 1;
        }
    }
    if (!fire) return NULL;

    /* True iff we should route this tap through the IME instead of
     * sending it raw.  CN mode + a-z key + no held modifier → IME.
     * Modifiers (Shift/Ctrl/Alt) always bypass IME so Ctrl-C, Alt-b,
     * etc. still work in CN mode.  Settings edit mode never routes to
     * IME — field buffers take raw characters. */
    int route_to_ime =
        kb->ime &&
        !kb->settings_mode &&
        keyboard_get_mode(kbd) == MODE_CN &&
        !keyboard_shift_held(kbd) &&
        !keyboard_ctrl_held(kbd) &&
        !keyboard_alt_held(kbd);

    switch (k->kind) {
        case KIND_PAGE_TOGGLE:
        case KIND_PAGE_BTN:
            kb->page = (kb->page == PAGE_LETTERS) ? PAGE_SYMBOLS : PAGE_LETTERS;
            /* After the page flips, whatever key now sits under the
             * finger must NOT auto-fire on the next held frame. */
            kb->repeat_idx = -2;
            return NULL;
        case KIND_SEQ:
            /* Shift + Tab = "cursor backward tabulation" (CSI Z) — the
             * terminal-standard escape for shift-tab.  Catches the
             * common case (only seq we currently bind is "\t"); other
             * KIND_SEQ keys pass through verbatim. */
            if (keyboard_shift_held(kbd) && k->seq
                && k->seq[0] == '\t' && k->seq[1] == '\0') {
                return "\x1b[Z";
            }
            return k->seq;
        case KIND_SPACE:
            /* In CN mode with an active pinyin buffer, space commits
             * the currently-selected candidate (defaults to the first
             * one until the user moves the cursor with D-pad ←→). */
            if (route_to_ime && ime_active(kb->ime)) {
                return ime_select_current(kb->ime);
            }
            return keyboard_emit_for(kbd, k->base);
        case KIND_CHAR:
            if (route_to_ime && k->base >= 'a' && k->base <= 'z') {
                ime_input_letter(kb->ime, k->base);
                return NULL;
            }
            return keyboard_emit_for(kbd, k->base);
    }
    return NULL;
}

/* ── rendering helpers ─────────────────────────────────────────────── */

static u32 rgba_to_c2d_(uint32_t rgba) {
    return C2D_Color32((rgba >> 24) & 0xff,
                       (rgba >> 16) & 0xff,
                       (rgba >>  8) & 0xff,
                        rgba        & 0xff);
}

/* Press-down animation timing.  When a key is tapped, pressed_frames
 * counts up from 0; press_depth() turns that frame counter into a
 * pixel-offset and an interpolation factor used to smoothly translate
 * the key downward and blend its body color toward the press color.
 *
 *   0          PEAK              END
 *   |-----------|-----------------|
 *   depth: 0 → MAX               → 0
 *
 * 60 fps means PRESS_ANIM_FRAMES=14 ≈ 230 ms — slow enough to feel
 * like a key going down and snapping back, fast enough to keep up
 * with rapid typing. */
#define PRESS_ANIM_FRAMES  14
#define PRESS_ANIM_PEAK     6
#define PRESS_MAX_DEPTH     2

static int press_depth(int frames) {
    if (frames < 0 || frames >= PRESS_ANIM_FRAMES) return 0;
    if (frames <= PRESS_ANIM_PEAK) {
        /* down phase: 0 → MAX */
        return (frames * PRESS_MAX_DEPTH + PRESS_ANIM_PEAK / 2)
             / PRESS_ANIM_PEAK;
    }
    /* up phase: MAX → 0 */
    int up    = PRESS_ANIM_FRAMES - PRESS_ANIM_PEAK;
    int phase = PRESS_ANIM_FRAMES - frames;
    return (phase * PRESS_MAX_DEPTH + up / 2) / up;
}

/* Channel-wise lerp between two RGBA colors.  t / range → 0..1 weight
 * toward `b`.  Saturating, so out-of-range t just clamps. */
static uint32_t blend_rgba(uint32_t a, uint32_t b, int t, int range) {
    if (range <= 0 || t <= 0) return a;
    if (t >= range) return b;
    uint32_t ar = (a >> 24) & 0xff, ag = (a >> 16) & 0xff;
    uint32_t ab = (a >>  8) & 0xff, aa =  a        & 0xff;
    uint32_t br = (b >> 24) & 0xff, bg = (b >> 16) & 0xff;
    uint32_t bb = (b >>  8) & 0xff, ba =  b        & 0xff;
    uint32_t r  = (ar * (range - t) + br * t) / range;
    uint32_t g  = (ag * (range - t) + bg * t) / range;
    uint32_t bl = (ab * (range - t) + bb * t) / range;
    uint32_t al = (aa * (range - t) + ba * t) / range;
    return (r << 24) | (g << 16) | (bl << 8) | al;
}

/* 3-pixel triangular round-corner.  Punches the key's four corners with
 * the screen bg color so the rectangle looks rounded:
 *
 *   ###..      (row 0:  3 px)
 *   ##...      (row 1:  2 px)
 *   #....      (row 2:  1 px)
 *   .....
 *
 * 3 px is ~10% of the 32 px key width — the same proportion iOS uses
 * for its keyboard, and visibly more rounded than the previous 2-px
 * L-shaped chamfer. */
static void round_corners(int x, int y, int w, int h, float z) {
    uint32_t c = rgba_to_c2d_(COL_CHAMFER);
    /* TL */
    C2D_DrawRectSolid((float)x,       (float)y,       z, 3, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y + 1), z, 2, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y + 2), z, 1, 1, c);
    /* TR */
    C2D_DrawRectSolid((float)(x+w-3), (float)y,       z, 3, 1, c);
    C2D_DrawRectSolid((float)(x+w-2), (float)(y + 1), z, 2, 1, c);
    C2D_DrawRectSolid((float)(x+w-1), (float)(y + 2), z, 1, 1, c);
    /* BL */
    C2D_DrawRectSolid((float)x,       (float)(y+h-1), z, 3, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y+h-2), z, 2, 1, c);
    C2D_DrawRectSolid((float)x,       (float)(y+h-3), z, 1, 1, c);
    /* BR */
    C2D_DrawRectSolid((float)(x+w-3), (float)(y+h-1), z, 3, 1, c);
    C2D_DrawRectSolid((float)(x+w-2), (float)(y+h-2), z, 2, 1, c);
    C2D_DrawRectSolid((float)(x+w-1), (float)(y+h-3), z, 1, 1, c);
}

/* Render one key.  `depth` is 0 when at rest and grows up to
 * PRESS_MAX_DEPTH (2 px) at the peak of the press animation.  The
 * whole key — border, body, top highlight, label, corners — translates
 * downward by `depth`, mimicking a real keyboard cap going down.
 *
 *   - At rest (depth=0):  shadow under the key + top highlight strip
 *                          give the 3D lifted look.
 *   - Mid-press:           shadow fades, body slides down, color blends
 *                          toward the press color.
 *
 * Z layers (base 0.10, increments of 0.01):
 *   0.10  shadow strip below resting key
 *   0.12  border (dark stroke around body)
 *   0.15  body fill
 *   0.18  top highlight strip (resting only)
 *   0.21  rounded-corner chamfer
 */
static void draw_key_button(int x, int y, int w, int h,
                            uint32_t body, int depth) {
    int yd = y + depth;

    /* Resting drop shadow, fades out as depth grows. */
    if (depth == 0) {
        C2D_DrawRectSolid((float)(x + 1), (float)(y + h), 0.10f,
                          (float)(w - 2), 1,
                          rgba_to_c2d_(COL_KEY_BOT_SHADOW));
    }

    /* Border around the (possibly translated) body. */
    C2D_DrawRectSolid((float)x, (float)yd, 0.12f,
                      (float)w, (float)h,
                      rgba_to_c2d_(COL_KEY_BORDER));
    /* Body fill, inset 1 px from the border. */
    C2D_DrawRectSolid((float)(x + 1), (float)(yd + 1), 0.15f,
                      (float)(w - 2), (float)(h - 2),
                      rgba_to_c2d_(body));
    /* Top highlight only when resting (the sliver at the top of a
     * physical keycap that catches light). */
    if (depth == 0) {
        C2D_DrawRectSolid((float)(x + 1), (float)(yd + 1), 0.18f,
                          (float)(w - 2), 1,
                          rgba_to_c2d_(COL_KEY_TOP));
    }
    /* Corners follow the body's translated position. */
    round_corners(x, yd, w, h, 0.21f);
}

/* Centered text label.  Like draw_key_button, the label translates
 * downward by `depth` so it stays glued to the moving body. */
static void draw_label(int rx, int ry, int rw, int rh,
                       const char *text, u32 fg_rgba, int depth) {
    if (!text) return;
    int tlen = (int)strlen(text);     /* labels are ASCII; bytes == cols */
    int tw   = tlen * CELL_W;
    int x0   = rx + (rw - tw) / 2;
    int y0   = ry + (rh - CELL_H) / 2 + depth;
    renderer_draw_text_px(x0, y0, text, fg_rgba);
}

/* Resting (un-pressed) body fill color per key kind.  The press
 * animation blends from this toward COL_KEY_PRESSED rather than
 * snapping. */
static uint32_t key_body_color_resting(const softkey_t *k) {
    switch (k->kind) {
        case KIND_PAGE_BTN:    return COL_KEY_PG_BODY;
        case KIND_SPACE:       return COL_KEY_SPACE_BODY;
        case KIND_PAGE_TOGGLE: return COL_KEY_SPECIAL;
        case KIND_SEQ:         return COL_KEY_SPECIAL;
        case KIND_CHAR:
        default:               return COL_KEY_BODY;
    }
}

static uint32_t key_label_color_resting(const softkey_t *k) {
    if (k->kind == KIND_PAGE_BTN) return COL_KEY_PRESSED_FG; /* dark on bright */
    return COL_KEY_LABEL;
}

/* ── status / candidate row ─────────────────────────────────────────── */

#define COL_IME_PINYIN_FG     0xa6e3a1ff  /* matched pinyin chars  — green */
#define COL_IME_PINYIN_NOMATCH 0xf38ba8ff /* unmatched buffer tail — red */
#define COL_IME_CANDIDATE_FG  0xcdd6f4ff  /* main candidate text */
#define COL_IME_SELECTED_BG   0x89b4fa66  /* highlight under selected cand */
#define COL_IME_PAGE_HINT     0x6c7086ff  /* "1/4" page indicator */

/* Lay out the IME bar: pinyin buffer + visible candidates within the
 * candidate strip.  Records hit-test boxes into kb->cand_box_*.
 * `strip_x` is the strip's left edge, `strip_end` is one past the
 * right edge.  Returns how many candidates were rendered. */
static int draw_ime_strip(softkb_t *kb,
                          int strip_x, int strip_end, int strip_y) {
    if (!kb->ime || !ime_active(kb->ime)) return 0;

    int x = strip_x + 4;
    int y = strip_y + (STATUS_H - 4 - CELL_H) / 2;

    /* Buffer display — split into matched (green) and unmatched (red)
     * portions so the user sees which part of their input still has
     * candidates and which trailing letters they should backspace. */
    const char *buf  = ime_buffer(kb->ime);
    int buf_len      = ime_buffer_len(kb->ime);
    int matched_len  = ime_matched_prefix_len(kb->ime);
    if (matched_len > 0) {
        char head[IME_BUFFER_MAX + 1];
        memcpy(head, buf, (size_t)matched_len);
        head[matched_len] = 0;
        renderer_draw_text_px(x, y, head, COL_IME_PINYIN_FG);
        x += matched_len * CELL_W;
    }
    if (matched_len < buf_len) {
        renderer_draw_text_px(x, y, buf + matched_len,
                              COL_IME_PINYIN_NOMATCH);
        x += (buf_len - matched_len) * CELL_W;
    }
    x += 6;  /* gap between buffer and candidates */

    /* Page hint "p/n" before the candidates if multi-page. */
    int page_count = ime_page_count(kb->ime);
    if (page_count > 1) {
        char hint[24];
        snprintf(hint, sizeof(hint), "%d/%d",
                 ime_page(kb->ime) + 1, page_count);
        renderer_draw_text_px(x, y, hint, COL_IME_PAGE_HINT);
        x += renderer_utf8_text_width_px(hint) + 6;
    }

    /* Lay out the current page's candidates left-to-right.  The
     * selection cursor (driven by D-pad ←→) gets a stronger
     * highlight so the user can see which one A/Space commits. */
    int n_in_page = ime_candidate_count(kb->ime);
    int sel       = ime_selection_idx(kb->ime);
    int n_drawn   = 0;
    for (int i = 0; i < n_in_page && n_drawn < IME_PAGE_SIZE; i++) {
        const char *cand = ime_candidate(kb->ime, i);
        if (!cand) break;
        int w = renderer_utf8_text_width_px(cand);
        if (x + w > strip_end - 2) break;
        if (i == sel) {
            C2D_DrawRectSolid((float)(x - 2), (float)(strip_y + 2),
                              0.072f, (float)(w + 4), (float)(STATUS_H - 8),
                              rgba_to_c2d_(COL_IME_SELECTED_BG));
        }
        renderer_draw_text_px(x, y, cand, COL_IME_CANDIDATE_FG);
        kb->cand_box_x[n_drawn] = x - 2;
        kb->cand_box_w[n_drawn] = w + 4;
        n_drawn++;
        x += w + 6;
    }
    kb->cand_box_n = n_drawn;
    return n_drawn;
}

/* Layout (left → right):
 *   [2..2+slot_w]              left slot  (status indicator)
 *   [slot_w+6..320-slot_w-8]   candidate strip
 *   [320-slot_w-2..318]        right slot (mode badge)
 *
 * slot_w = 3 chars * 6 px + 4 px padding = 22 px.
 * slot_h = STATUS_H - 4 (2 px top/bot margin).
 *
 * All labels rendered with renderer_draw_text_px so 3-char labels land
 * exactly on their geometric centers, no cell-grid snapping. */
static void draw_status_row(softkb_t *kb, renderer_t *r,
                            const keyboard_t *kbd) {
    const int slot_w = 3 * CELL_W + 4;     /* 22 px */
    const int slot_h = STATUS_H - 4;
    const int slot_y = 2;
    const int label_tw = 3 * CELL_W;       /* 18 px */
    const int label_y  = slot_y + (slot_h - CELL_H) / 2;

    /* Status row band (full width). */
    C2D_DrawRectSolid(0, 0, 0.05f, 320, STATUS_H,
                      rgba_to_c2d_(COL_STATUS_BG));

    /* Candidate strip between the two slots. */
    int strip_x   = slot_w + 6;
    int strip_end = 320 - slot_w - 6;
    C2D_DrawRectSolid((float)strip_x, (float)slot_y, 0.06f,
                      (float)(strip_end - strip_x), (float)slot_h,
                      rgba_to_c2d_(COL_CANDIDATE_BG));

    /* IME candidates over the strip (when buffer non-empty). */
    kb->cand_box_n = 0;
    if (kb && kb->ime && ime_active(kb->ime) && !kb->settings_mode) {
        draw_ime_strip(kb, strip_x, strip_end, slot_y);
    }

    /* ── Left slot: status indicator ─────────────────────────────────── */
    /* Voice has highest priority — when REC / spinner / ERR is showing
     * we suppress the modifier label entirely.  When voice is IDLE we
     * fall through to keyboard_status_label() for SFT/CTL/ALT/etc. */
    const char *voice_lbl = (kb && kb->voice) ? voice_status_label(kb->voice) : NULL;
    uint32_t voice_bg = voice_lbl ? voice_status_bg(kb->voice) : 0;
    uint32_t voice_fg = voice_lbl ? voice_status_fg(kb->voice) : 0;

    const char *status = kbd ? keyboard_status_label(kbd) : "   ";
    int kbd_active = (status && strcmp(status, "   ") != 0);

    /* Always-drawn slot bg keeps the layout visually anchored. */
    C2D_DrawRectSolid(2, (float)slot_y, 0.07f,
                      (float)slot_w, (float)slot_h,
                      rgba_to_c2d_(COL_MODE_LBL_BG));

    if (voice_lbl) {
        /* Voice tint over the slot. */
        if (voice_bg) {
            C2D_DrawRectSolid(2, (float)slot_y, 0.072f,
                              (float)slot_w, (float)slot_h,
                              rgba_to_c2d_(voice_bg));
        }
        if (strlen(voice_lbl) > 3) {
            /* Surfaced error reason (e.g. "open ctx 0x…") — left-align
             * into the candidate strip; centered in the 22 px slot it
             * overflows and is unreadable. */
            renderer_draw_text_px(strip_x, label_y, voice_lbl, voice_fg);
        } else {
            int vx = 2 + (slot_w - label_tw) / 2;
            renderer_draw_text_px(vx, label_y, voice_lbl, voice_fg);
        }
    } else if (kbd_active) {
        /* Highlight overlay: faint blue tint behind the active label. */
        C2D_DrawRectSolid(2, (float)slot_y, 0.072f,
                          (float)slot_w, (float)slot_h,
                          rgba_to_c2d_(COL_STATUS_HOLD_BG));
        int x0 = 2 + (slot_w - label_tw) / 2;
        renderer_draw_text_px(x0, label_y, status, COL_STATUS_FG_HOLD);
    }

    /* ── Right slot: 3-char mode badge "ENG"/"CHN" ───────────────────── */
    ime_mode_t m = kbd ? keyboard_get_mode(kbd) : MODE_EN;
    const char *mode_label = (m == MODE_CN) ? "CHN" : "ENG";
    uint32_t mode_color    = (m == MODE_CN) ? COL_MODE_CN : COL_MODE_EN;
    int rx = 320 - slot_w - 2;
    C2D_DrawRectSolid((float)rx, (float)slot_y, 0.07f,
                      (float)slot_w, (float)slot_h,
                      rgba_to_c2d_(COL_MODE_LBL_BG));
    int mx = rx + (slot_w - label_tw) / 2;
    renderer_draw_text_px(mx, label_y, mode_label, mode_color);

    (void)r;  /* unused with the px path */
}

/* ── debug page ────────────────────────────────────────────────────── */

static void draw_debug_screen(softkb_t *kb, renderer_t *r,
                              const keyboard_t *kbd) {
    /* Solid background over the whole bottom screen. */
    C2D_DrawRectSolid(0, 0, 0.05f, 320, 240,
                      rgba_to_c2d_(COL_STATUS_BG));

    /* Keep the status bar so the ENG/CHN badge is still visible/tappable
     * — that's how the user gets back out. */
    draw_status_row(kb, r, kbd);

    /* Title + exit hint */
    renderer_draw_text_px(8, 40, "DEBUG", COL_STATUS_FG_HOLD);
    renderer_draw_text_px(48, 40, "tap CHN/ENG to exit",
                          COL_STATUS_DIM);

    /* Recv hex strip — last up to 32 SSH-bound bytes, two lines of 16
     * with their byte indices for byte-level traceback. */
    renderer_draw_text_px(8, 60, "recv:", COL_KEY_LABEL);
    int hex_x = 8 + 6 * 6;
    int hex_y = 60;
    int show  = kb->recv_count;          /* 0..32 */
    int start = (kb->recv_head - show + 32) % 32;
    for (int i = 0; i < show; i++) {
        char hex[4];
        uint8_t b = kb->recv_ring[(start + i) % 32];
        snprintf(hex, sizeof(hex), "%02x", b);
        renderer_draw_text_px(hex_x, hex_y, hex, COL_KEY_LABEL);
        hex_x += 18;                     /* 2 chars + 1 char gap */
        if (hex_x > 320 - 18) {
            hex_x = 8 + 6 * 6;
            hex_y += 14;
        }
    }
    if (show == 0) {
        renderer_draw_text_px(hex_x, hex_y, "(no SSH bytes yet)",
                              COL_STATUS_DIM);
    }

    /* Physical-key bindings legend.  Mirrors keyboard.c — keep in sync.
     * Five 14-px lines fit cleanly between the recv hex (ends ~y=86)
     * and the mascot toggle button (y=170). */
    int kb_y = 100;
    renderer_draw_text_px(8, kb_y +  0,
        "L=Shift   Y=Ctrl   X=Alt", COL_KEY_LABEL);
    renderer_draw_text_px(8, kb_y + 14,
        "A=Enter (IME:emit pinyin as English)", COL_KEY_LABEL);
    renderer_draw_text_px(8, kb_y + 28,
        "B=Backsp  SELECT=Esc  R=mode toggle", COL_KEY_LABEL);
    renderer_draw_text_px(8, kb_y + 42,
        "D-pad=arrows/IME  Space=commit cand", COL_KEY_LABEL);
    renderer_draw_text_px(8, kb_y + 56,
        "Circle=scroll  L+Circle=right pane", COL_KEY_LABEL);

    /* Mascot toggle button — drawn as a regular key for visual
     * consistency with the keyboard layout. */
    char btn_label[24];
    snprintf(btn_label, sizeof(btn_label), "MASCOT: %s",
             kb->mascot_enabled ? "ON" : "OFF");
    draw_key_button(DBG_TOGGLE_X, DBG_TOGGLE_Y,
                    DBG_TOGGLE_W, DBG_TOGGLE_H,
                    COL_KEY_BODY, 0);
    int blen = (int)strlen(btn_label);
    renderer_draw_text_px(
        DBG_TOGGLE_X + (DBG_TOGGLE_W - blen * CELL_W) / 2,
        DBG_TOGGLE_Y + (DBG_TOGGLE_H - CELL_H)   / 2,
        btn_label, COL_KEY_LABEL);

    /* Voice transport status — shows which way plain voice recordings
     * go (HTTP API endpoint vs SSH shim), for debugging "voice is not
     * working" reports. */
    if (kb->cfg) {
        char voice_line[64];
        snprintf(voice_line, sizeof(voice_line), "VOICE: %s",
                 kb->cfg->voice_api_url[0] ? "HTTP API" : "SSH shim");
        renderer_draw_text_px(8, kb_y + 70, voice_line, COL_KEY_LABEL);
    }
}

/* ── settings page ─────────────────────────────────────────────────── */

/* Draw a value string clipped to max_px, walking whole UTF-8 glyphs. */
static void draw_value_clipped(int x, int y, const char *val, int max_px,
                               uint32_t color) {
    if (!val[0]) {
        renderer_draw_text_px(x, y, "(empty)", COL_STATUS_DIM);
        return;
    }
    char glyph[5];
    int w = 0;
    int pos = 0;
    while (val[pos]) {
        int clen = 1;
        unsigned char lead = (unsigned char)val[pos];
        if (lead >= 0xF0)      clen = 4;
        else if (lead >= 0xE0) clen = 3;
        else if (lead >= 0xC0) clen = 2;
        if ((int)strlen(val + pos) < clen) break;   /* torn tail */
        memcpy(glyph, val + pos, (size_t)clen);
        glyph[clen] = 0;
        int cw = renderer_utf8_text_width_px(glyph);
        if (w + cw > max_px) break;
        renderer_draw_text_px(x + w, y, glyph, color);
        w += cw;
        pos += clen;
    }
}

static void draw_set_button(int active) {
    draw_key_button(SETBTN_X, SETBTN_Y, SETBTN_W, SETBTN_H,
                    active ? COL_KEY_PG_BODY : COL_KEY_BODY, 0);
    const char *lbl = "SET";
    int tw = (int)strlen(lbl) * CELL_W;
    renderer_draw_text_px(SETBTN_X + (SETBTN_W - tw) / 2,
                          SETBTN_Y + (SETBTN_H - CELL_H) / 2,
                          lbl,
                          active ? COL_KEY_PRESSED_FG : COL_KEY_LABEL);
}

/* Window switcher: shows the "2/3" active/total label when multiple
 * servers are configured; renders dimmed "WIN" otherwise (taps are
 * swallowed as no-ops in that case). */
static void draw_win_button(const softkb_t *kb) {
    int multi = kb && kb->win_total >= 2;
    draw_key_button(WINBTN_X, WINBTN_Y, WINBTN_W, WINBTN_H,
                    multi ? COL_KEY_BODY : COL_KEY_SPECIAL, 0);
    const char *lbl = multi ? kb->win_label : "WIN";
    int tw = (int)strlen(lbl) * CELL_W;
    renderer_draw_text_px(WINBTN_X + (WINBTN_W - tw) / 2,
                          WINBTN_Y + (WINBTN_H - CELL_H) / 2,
                          lbl,
                          multi ? COL_KEY_LABEL : COL_STATUS_DIM);
}

/* Reusable keyboard-layout painter (normal mode and settings edit mode). */
static void draw_keyboard_keys(softkb_t *kb) {
    int n;
    const softkey_t *layout = current_layout(kb, &n);
    for (int i = 0; i < n; i++) {
        const softkey_t *k = &layout[i];
        /* Only the actively-pressed key has nonzero depth/blend; every
         * other key renders at rest with depth=0, which makes both
         * blend_rgba calls below short-circuit to the resting color. */
        int depth = (i == kb->pressed_idx)
                  ? press_depth(kb->pressed_frames)
                  : 0;
        uint32_t resting_body = key_body_color_resting(k);
        uint32_t resting_lbl  = key_label_color_resting(k);
        uint32_t body = blend_rgba(resting_body, COL_KEY_PRESSED,
                                   depth, PRESS_MAX_DEPTH);
        uint32_t lbl  = blend_rgba(resting_lbl,  COL_KEY_PRESSED_FG,
                                   depth, PRESS_MAX_DEPTH);
        draw_key_button(k->x, k->y, k->w, k->h, body, depth);
        draw_label(k->x, k->y, k->w, k->h, k->label, lbl, depth);
    }
}

static void draw_settings_screen(softkb_t *kb, renderer_t *r,
                                 const keyboard_t *kbd) {
    C2D_DrawRectSolid(0, 0, 0.05f, 320, 240,
                      rgba_to_c2d_(COL_STATUS_BG));

    /* ── Edit mode: field bar (replaces the status row) + keyboard ── */
    if (kb->set_edit >= 0) {
        /* The edit bar takes over y 0..34 (the status row's band) so the
         * keyboard keeps its full y=36.. grid.  It used to sit at y=40,
         * covering keyboard row 0 — the symbols page's digit row — and
         * the bar's commit-tap zone swallowed every tap on those keys,
         * making digits untappable while editing. */
        C2D_DrawRectSolid(0, (float)SET_EDITBAR_Y, 0.06f, 320,
                          (float)SET_EDITBAR_H,
                          rgba_to_c2d_(COL_CANDIDATE_BG));
        char title[48];
        snprintf(title, sizeof(title), "%s:",
                 set_field_label((settings_field_t)kb->set_edit));
        renderer_draw_text_px(6, SET_EDITBAR_Y + 3, title, COL_STATUS_FG_HOLD);
        /* Field value (plaintext while editing — you need to see what
         * you type; passwords revert to the masked row once committed). */
        draw_value_clipped(6 + renderer_utf8_text_width_px(title) + 4,
                           SET_EDITBAR_Y + 3, kb->set_edit_buf,
                           320 - 24, COL_KEY_LABEL);
        renderer_draw_text_px(6, SET_EDITBAR_Y + 20,
                              "A=ok  B=del  SELECT=cancel  tap bar=ok",
                              COL_STATUS_DIM);

        draw_keyboard_keys(kb);
        return;
    }

    draw_status_row(kb, r, kbd);

    /* ── Browse mode ── */
    renderer_draw_text_px(6, SET_TITLE_Y, "SETTINGS", COL_STATUS_FG_HOLD);
    renderer_draw_text_px(70, SET_TITLE_Y, "tap SET to close",
                          COL_STATUS_DIM);

    /* Server selector: [<] SRV n/N [>] — browsing also activates the
     * slot when it has a host. */
    draw_key_button(SET_SEL_L_X, SET_SEL_Y, SET_SEL_BTN_W, SET_SEL_H,
                    COL_KEY_BODY, 0);
    draw_key_button(SET_SEL_R_X, SET_SEL_Y, SET_SEL_BTN_W, SET_SEL_H,
                    COL_KEY_BODY, 0);
    draw_label(SET_SEL_L_X, SET_SEL_Y, SET_SEL_BTN_W, SET_SEL_H,
               "<", COL_KEY_LABEL, 0);
    draw_label(SET_SEL_R_X, SET_SEL_Y, SET_SEL_BTN_W, SET_SEL_H,
               ">", COL_KEY_LABEL, 0);
    char sel_lbl[40];
    snprintf(sel_lbl, sizeof(sel_lbl), "SRV %d/%d %s",
             kb->set_srv + 1, CONFIG_SERVERS_MAX,
             (kb->cfg && kb->set_srv == kb->cfg->active_server &&
              kb->cfg->servers[kb->set_srv].host[0]) ? "(active)" : "");
    int sel_lbl_w = (int)strlen(sel_lbl) * CELL_W;
    renderer_draw_text_px((320 - sel_lbl_w) / 2, SET_SEL_Y + 3,
                          sel_lbl,
                          (kb->cfg && kb->set_srv == kb->cfg->active_server)
                              ? COL_STATUS_FG_HOLD : COL_KEY_LABEL);

    /* Field rows. */
    if (kb->cfg) {
        char tmp[16];
        for (int f = 0; f < SET_FLD_COUNT; f++) {
            int ry = SET_ROW_Y0 + f * SET_ROW_H;
            const char *label = set_field_label((settings_field_t)f);
            renderer_draw_text_px(SET_ROW_LABEL_X,
                                  ry + (SET_ROW_H - CELL_H) / 2,
                                  label, COL_KEY_LABEL);
            draw_key_button(SET_ROW_VALUE_X, ry + 1,
                            SET_ROW_VALUE_W, SET_ROW_H - 2,
                            COL_KEY_SPECIAL, 0);
            const char *val = set_field_value(kb->cfg, kb->set_srv,
                                              (settings_field_t)f,
                                              tmp, (int)sizeof(tmp));
            int vy = ry + (SET_ROW_H - CELL_H) / 2;
            if (f == SET_FLD_AUTH) {
                /* Auth is a toggle, not an edit field — highlight it. */
                renderer_draw_text_px(SET_ROW_VALUE_X + 6, vy,
                                      strcmp(val, "password") == 0
                                          ? "PWD LOGIN" : "KEY (RSA)",
                                      COL_STATUS_FG_HOLD);
            } else if (f == SET_FLD_PASSWORD) {
                renderer_draw_text_px(SET_ROW_VALUE_X + 6, vy,
                                      val[0] ? "********" : "",
                                      COL_KEY_LABEL);
                if (!val[0])
                    renderer_draw_text_px(SET_ROW_VALUE_X + 6, vy,
                                          "(empty)", COL_STATUS_DIM);
            } else {
                draw_value_clipped(SET_ROW_VALUE_X + 6, vy, val,
                                   SET_ROW_VALUE_W - 12, COL_KEY_LABEL);
            }
        }
    }

    /* SAVE / RECONNECT buttons. */
    draw_key_button(SET_SAVE_X, SET_BTN_Y, SET_SAVE_W, SET_BTN_H,
                    COL_KEY_PG_BODY, 0);
    draw_label(SET_SAVE_X, SET_BTN_Y, SET_SAVE_W, SET_BTN_H,
               "SAVE", COL_KEY_PRESSED_FG, 0);
    draw_key_button(SET_RECONN_X, SET_BTN_Y, SET_RECONN_W, SET_BTN_H,
                    COL_KEY_BODY, 0);
    draw_label(SET_RECONN_X, SET_BTN_Y, SET_RECONN_W, SET_BTN_H,
               "RECONNECT", COL_KEY_LABEL, 0);

    draw_set_button(1);
}

/* ── public draw ───────────────────────────────────────────────────── */

void softkb_draw(softkb_t *kb, renderer_t *r, const keyboard_t *kbd) {
    if (!kb || !r) return;

    if (kb->debug_mode) {
        draw_debug_screen(kb, r, kbd);
        return;
    }

    if (kb->settings_mode) {
        draw_settings_screen(kb, r, kbd);
        return;
    }

    draw_status_row(kb, r, kbd);
    draw_keyboard_keys(kb);
    draw_win_button(kb);
    draw_set_button(0);
}

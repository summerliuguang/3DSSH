/*
 * 3dssh — Nintendo 3DS SSH client (M4).
 *
 * M4 changes from M3:
 *   - All modifiers (Shift / Ctrl / Alt) on physical shoulder/face buttons
 *     (L=Shift, Y=Ctrl, X=Alt — see source/keyboard.c).  No more
 *     SELECT-driven sticky-Ctrl state machine.
 *   - SELECT now emits Esc, R now toggles IME mode (EN ↔ CN).
 *     SELECT is overloaded: when the SSH session is dead (hard disconnect
 *     after lid-close sleep), pressing SELECT reconnects instead of emit-
 *     ting Esc — so the user can recover without exiting DSSH (M13).
 *   - Bottom screen is the full custom soft keyboard (source/softkb.c)
 *     — no more system swkbd applet popup.  X key is for Alt now.
 *   - Touch screen drives the soft keyboard.  Tapped letter goes through
 *     keyboard_emit_for() so any held modifier (L/Y/X) is applied.
 *
 * Single-threaded polling main loop.  60 fps render cadence.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <3ds.h>
#include <citro2d.h>
#include <ts3ds.h>

#include "ssh_client.h"
#include "config.h"
#include "keychain_protocol.h"
#include "terminal.h"
#include "renderer.h"
#include "keyboard.h"
#include "softkb.h"
#include "mascot.h"

#ifndef DSSH_TAILSCALE_PATH
#define DSSH_TAILSCALE_PATH "auto"
#endif
#ifndef DSSH_TAILSCALE_VERBOSE
#define DSSH_TAILSCALE_VERBOSE 0
#endif
#include "ime_pinyin.h"
#include "voice.h"
#include "ai_modal.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define CONFIG_PATH     "sdmc:/3ds/3dssh/config.ini"
#define READ_BUFSZ      2048

/* Interactive shell startup can be noticeably slower over Tailscale/DERP,
 * especially when fish/zsh startup hooks perform network or filesystem I/O. */
#define SHELL_READY_TIMEOUT_MS      60000
#define KEYCHAIN_PROMPT_TIMEOUT_MS  30000
#define KEYCHAIN_RESULT_TIMEOUT_MS  30000

#define COLOR_OK        0xa6e3a1ff
#define COLOR_WARN      0xfab387ff
#define COLOR_ERR       0xf38ba8ff
#define COLOR_FG        0xcdd6f4ff
#define COLOR_DIM       0x6c7086ff
#define COLOR_ACCENT    0x89b4faff

static u32 *soc_buf = NULL;

#define TS_DEBUG_LINE_COUNT 24
#define TS_DEBUG_LINE_SIZE  224

typedef struct tailscale_debug_log {
    LightLock lock;
    char lines[TS_DEBUG_LINE_COUNT][TS_DEBUG_LINE_SIZE];
    unsigned char levels[TS_DEBUG_LINE_COUNT];
    unsigned head;
    unsigned count;
    unsigned dropped;
    int startup_verbose;
} tailscale_debug_log;

static void tailscale_debug_init(tailscale_debug_log *debug) {
    memset(debug, 0, sizeof(*debug));
    LightLock_Init(&debug->lock);
    debug->startup_verbose = DSSH_TAILSCALE_VERBOSE ? 1 : 0;
}

/* libts3ds may log from its DERP worker. Keep that callback independent of
 * terminal rendering, then drain it from DSSH's main thread. */
static void tailscale_debug_capture(void *userdata, int level,
                                    const char *message) {
    tailscale_debug_log *debug = (tailscale_debug_log *)userdata;
    if (!debug || !message) return;
    LightLock_Lock(&debug->lock);
    /* Keep rich diagnostics while Tailscale and SSH are connecting. Once an
     * interactive shell exists, control/DERP maintenance is independent of
     * the established WireGuard data path and must never corrupt the user's
     * command line. SSH failure still has its own visible connection state. */
    if (level >= 3 || !debug->startup_verbose) {
        LightLock_Unlock(&debug->lock);
        return;
    }
    if (debug->count == TS_DEBUG_LINE_COUNT) {
        debug->head = (debug->head + 1) % TS_DEBUG_LINE_COUNT;
        debug->count--;
        debug->dropped++;
    }
    unsigned slot = (debug->head + debug->count) % TS_DEBUG_LINE_COUNT;
    snprintf(debug->lines[slot], TS_DEBUG_LINE_SIZE, "%s", message);
    debug->levels[slot] = (unsigned char)level;
    debug->count++;
    LightLock_Unlock(&debug->lock);
}

static void tailscale_debug_set_runtime(tailscale_debug_log *debug) {
    if (!debug) return;
    LightLock_Lock(&debug->lock);
    debug->startup_verbose = 0;
    LightLock_Unlock(&debug->lock);
}

static void tailscale_debug_flush(tailscale_debug_log *debug,
                                  terminal_t *term) {
    if (!debug || !term) return;
    for (;;) {
        char message[TS_DEBUG_LINE_SIZE];
        unsigned level;
        unsigned dropped = 0;
        LightLock_Lock(&debug->lock);
        if (debug->count == 0) {
            dropped = debug->dropped;
            debug->dropped = 0;
            LightLock_Unlock(&debug->lock);
            if (dropped) {
                char line[96];
                snprintf(line, sizeof(line),
                         "\x1b[33m[ts3ds] %u earlier log lines dropped"
                         "\x1b[0m\r\n", dropped);
                terminal_write(term, line);
            }
            return;
        }
        unsigned slot = debug->head;
        snprintf(message, sizeof(message), "%s", debug->lines[slot]);
        level = debug->levels[slot];
        debug->head = (debug->head + 1) % TS_DEBUG_LINE_COUNT;
        debug->count--;
        LightLock_Unlock(&debug->lock);

        char line[TS_DEBUG_LINE_SIZE + 40];
        const char *color = level == 0 ? "\x1b[31m"
                            : level == 1 ? "\x1b[33m" : "\x1b[90m";
        snprintf(line, sizeof(line), "%s[ts3ds] %s\x1b[0m\r\n",
                 color, message);
        terminal_write(term, line);
    }
}

static int net_init(char *err, int err_sz) {
    soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buf) { snprintf(err, err_sz, "memalign failed"); return -1; }
    int rc = socInit(soc_buf, SOC_BUFFERSIZE);
    if (rc != 0) {
        snprintf(err, err_sz, "socInit 0x%08lX", (unsigned long)rc);
        return -1;
    }
    return 0;
}

static void net_fini(void) {
    socExit();
    if (soc_buf) { free(soc_buf); soc_buf = NULL; }
}

/* ── SSH windows ─────────────────────────────────────────────────────
 * One window per configured server slot.  Each owns an independent
 * terminal grid + UTF-8 fragment state; sessions stay connected in the
 * background while another window is active.  Rendering and input
 * routing go through the ACTIVE window (g_win[g_active]); background
 * windows are only polled (non-blocking read + keepalive) so their
 * shells keep running and their terminals stay current for instant
 * switching — an unread background session would also hit libssh2 flow
 * control and stall the remote shell. */
typedef struct {
    ssh_client_t *ssh;          /* NULL = never connected / torn down */
    terminal_t   *term;         /* lazily created (~580 KB each) */
    char          utf8_frag[4]; /* packet boundaries can split a UTF-8 char */
    int           frag_len;
    int           dead;         /* session lost — SELECT reconnects */
    time_t        last_rx;      /* last receive, for the stall detector */
} ssh_window_t;

static ssh_window_t g_win[CONFIG_SERVERS_MAX];
static int          g_active = 0;   /* active slot index */

/* Window-button label "i/n" — both arguments clamped to the slot cap so
 * the 8-byte buffers passed by callers can never truncate. */
static void win_label(char *dst, size_t cap, int slot, int total) {
    if (slot < 1) slot = 1;
    if (slot > CONFIG_SERVERS_MAX) slot = CONFIG_SERVERS_MAX;
    if (total < 1) total = 1;
    if (total > CONFIG_SERVERS_MAX) total = CONFIG_SERVERS_MAX;
    snprintf(dst, cap, "%d/%d", slot, total);
}

static void feed_terminal(terminal_t *term, char *frag, int *frag_len,
                          const char *raw, int raw_len) {
    char buf[4 + READ_BUFSZ];
    int total;
    if (*frag_len > 0) {
        memcpy(buf, frag, (size_t)*frag_len);
        memcpy(buf + *frag_len, raw, (size_t)raw_len);
        total = *frag_len + raw_len;
        *frag_len = 0;
    } else {
        memcpy(buf, raw, (size_t)raw_len);
        total = raw_len;
    }
    int valid_end = total;
    for (int j = total - 1; j >= total - 3 && j >= 0; j--) {
        unsigned char b = (unsigned char)buf[j];
        if (b >= 0xc0) {
            int seq_len = (b < 0xe0) ? 2 : (b < 0xf0) ? 3 : 4;
            if (total - j < seq_len) {
                *frag_len = total - j;
                memcpy(frag, buf + j, (size_t)*frag_len);
                valid_end = j;
            }
            break;
        } else if (b < 0x80) {
            break;
        }
    }
    terminal_write_n(term, buf, valid_end);
}

typedef struct {
    int unlock_status;
    int verify_status;
} keychain_report_t;

static int startup_write_all(ssh_client_t *ssh, const char *data, int len,
                             int timeout_ms);

static void flush_terminal_responses(ssh_client_t *ssh, terminal_t *term) {
    char reply[TERM_RESPONSE_MAX];
    int n;
    while ((n = terminal_take_response(term, reply, sizeof(reply))) > 0)
        if (startup_write_all(ssh, reply, n, 1000) != 0) break;
}

static int startup_write_all(ssh_client_t *ssh, const char *data, int len,
                             int timeout_ms) {
    int sent = 0;
    u64 deadline = osGetTime() + (u64)timeout_ms;
    while (sent < len && osGetTime() < deadline) {
        ssh_poll_transport(ssh);
        int n = ssh_write(ssh, data + sent, len - sent);
        if (n < 0) return -1;
        if (n == 0) {
            svcSleepThread(10 * 1000 * 1000LL);
        } else {
            sent += n;
        }
    }
    return sent == len ? 0 : -1;
}

static void append_startup_tail(char *tail, int cap, int *tail_len,
                                const char *data, int len) {
    if (len >= cap - 1) {
        data += len - (cap - 1);
        len = cap - 1;
        *tail_len = 0;
    } else if (*tail_len + len >= cap) {
        int drop = *tail_len + len - (cap - 1);
        memmove(tail, tail + drop, (size_t)(*tail_len - drop));
        *tail_len -= drop;
    }
    memcpy(tail + *tail_len, data, (size_t)len);
    *tail_len += len;
    tail[*tail_len] = 0;
}

/* Pump the main interactive PTY until either marker/prompt appears. Feeding
 * the terminal while waiting is essential for fish: its startup asks CSI 6n
 * and will not finish drawing the prompt until DSSH sends the queued CPR
 * reply. The alternate marker is optional; a return value of 1 means needle,
 * 2 means alternate, 0 means timeout, and -1 means SSH disconnected. */
static int wait_for_remote_text_any(ssh_client_t *ssh, terminal_t *term,
                                    char *frag, int *frag_len,
                                    const char *needle,
                                    const char *alternate,
                                    int timeout_ms,
                                    char *capture, int capture_sz) {
    char raw[READ_BUFSZ];
    char tail[768] = {0};
    int tail_len = 0;
    u64 deadline = osGetTime() + (u64)timeout_ms;

    while (osGetTime() < deadline) {
        /* The keychain bootstrap runs before the main loop, so the UI is
         * frozen while we wait (up to 60s if the remote auto-starts tmux,
         * which swallows the OSC readiness marker).  Give the user a way
         * out: START aborts the wait and hands the shell over as-is. */
        hidScanInput();
        if (hidKeysDown() & KEY_START) return -2;
        /* A Tailscale-backed SSH socket lives in libts3ds's private lwIP
         * stack. It cannot progress while this synchronous bootstrap loop
         * sleeps unless its transport is explicitly pumped. */
        ssh_poll_transport(ssh);
        int n = ssh_read(ssh, raw, sizeof(raw));
        if (n < 0) return -1;
        if (n == 0) {
            svcSleepThread(10 * 1000 * 1000LL);
            continue;
        }
        append_startup_tail(tail, sizeof(tail), &tail_len, raw, n);
        feed_terminal(term, frag, frag_len, raw, n);
        flush_terminal_responses(ssh, term);
        char *found = strstr(tail, needle);
        char *found_alternate = alternate ? strstr(tail, alternate) : NULL;
        if (found || found_alternate) {
            if (capture && capture_sz > 0)
                snprintf(capture, (size_t)capture_sz, "%s", tail);
            if (!found) return 2;
            if (!found_alternate) return 1;
            return found <= found_alternate ? 1 : 2;
        }
    }
    if (capture && capture_sz > 0)
        snprintf(capture, (size_t)capture_sz, "%s", tail);
    return 0;
}

static int wait_for_remote_text(ssh_client_t *ssh, terminal_t *term,
                                char *frag, int *frag_len,
                                const char *needle, int timeout_ms,
                                char *capture, int capture_sz) {
    return wait_for_remote_text_any(ssh, term, frag, frag_len,
                                    needle, NULL, timeout_ms,
                                    capture, capture_sz);
}

static int parse_keychain_result(const char *capture,
                                 keychain_report_t *report,
                                 char *err, int err_sz) {
    /* Match the raw OSC form (ESC ] 777 ; ...) — the shell's echo of the
     * unlock command contains the same words as printable text and would
     * shadow the real result if we matched the bare marker name. */
    const char *result = strstr(capture, DSSH_KEYCHAIN_RESULT_MARKER);
    if (!result || sscanf(result + 6, /* skip ESC ] 7 7 7 ; */
            "DSSH_KEYCHAIN_RESULT unlock=%d verify=%d",
            &report->unlock_status, &report->verify_status) != 2) {
        snprintf(err, (size_t)err_sz, "invalid keychain result marker");
        return -1;
    }
    if (report->unlock_status != 0) {
        snprintf(err, (size_t)err_sz,
                 "security unlock-keychain failed (status=%d)",
                 report->unlock_status);
        return -1;
    }
    if (report->verify_status != 0) {
        snprintf(err, (size_t)err_sz,
                 "keychain verification failed (status=%d)",
                 report->verify_status);
        return -1;
    }
    err[0] = 0;
    return 0;
}

/* Unlock through the already-open interactive PTY, mirroring ServerCC's
 * proven macOS flow: wait for a prompt-ready shell, start security, wait for
 * its password prompt, then write the password.  An exec channel without a
 * PTY can fail with "User interaction is not allowed" on macOS. */
static int unlock_macos_keychain(ssh_client_t *ssh, terminal_t *term,
                                 char *frag, int *frag_len,
                                 const char *password,
                                 keychain_report_t *report,
                                 char *err, int err_sz) {
    static const char ready_command[] =
        "printf '\\033]777;%s_READY_%s\\007' DSSH SHELL\n";
    static const char ready_marker[] = "DSSH_READY_SHELL";
    static const char unlock_command[] =
        "sh -c '/usr/bin/security unlock-keychain "
        "\"$HOME/Library/Keychains/login.keychain-db\"; u=$?; v=-1; "
        "if [ \"$u\" -eq 0 ]; then /usr/bin/security show-keychain-info "
        "\"$HOME/Library/Keychains/login.keychain-db\" >/dev/null 2>&1; "
        "v=$?; fi; "
        "printf \"\\033]777;DSSH_KEYCHAIN_RESULT unlock=%d verify=%d\\007\" "
        "\"$u\" \"$v\"; "
        "printf \"\\033[2J\\033[H\"; "
        "if [ \"$u\" -ne 0 ] || [ \"$v\" -ne 0 ]; then "
        "printf \"[keychain] unlock failed (unlock=%d verify=%d)\\n\" "
        "\"$u\" \"$v\"; fi'\n";
    static const char result_marker[] = DSSH_KEYCHAIN_RESULT_MARKER;
    char capture[768];

    report->unlock_status = -1;
    report->verify_status = -1;
    if (!password || !*password) {
        snprintf(err, (size_t)err_sz, "keychain password is empty");
        return -1;
    }

    if (startup_write_all(ssh, ready_command,
                          sizeof(ready_command) - 1, 3000) != 0) {
        snprintf(err, (size_t)err_sz, "shell readiness probe write failed");
        return -1;
    }
    int rc = wait_for_remote_text(ssh, term, frag, frag_len, ready_marker,
                                  SHELL_READY_TIMEOUT_MS,
                                  NULL, 0);
    if (rc <= 0) {
        if (rc == -2) {
            snprintf(err, (size_t)err_sz,
                     "keychain bootstrap aborted (START)");
        } else if (rc < 0) {
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected while waiting for shell");
        } else {
            snprintf(err, (size_t)err_sz,
                     "shell readiness probe timed out after %ds",
                     SHELL_READY_TIMEOUT_MS / 1000);
        }
        return -1;
    }

    if (startup_write_all(ssh, unlock_command,
                          sizeof(unlock_command) - 1, 3000) != 0) {
        snprintf(err, (size_t)err_sz, "unlock command write failed");
        return -1;
    }
    rc = wait_for_remote_text_any(ssh, term, frag, frag_len,
                                  "password to unlock",
                                  result_marker,
                                  KEYCHAIN_PROMPT_TIMEOUT_MS,
                                  capture, sizeof(capture));
    if (rc == 2) {
        /* security can fail before it ever asks for a password (for example,
         * when the keychain path or utility is unavailable). Consume and
         * report that result immediately instead of waiting for a prompt that
         * will never arrive. */
        return parse_keychain_result(capture, report, err, err_sz);
    }
    if (rc <= 0) {
        if (rc == -2) {
            snprintf(err, (size_t)err_sz,
                     "keychain bootstrap aborted (START)");
        } else if (rc < 0) {
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected before keychain password prompt");
        } else {
            snprintf(err, (size_t)err_sz,
                     "keychain password prompt timed out after %ds",
                     KEYCHAIN_PROMPT_TIMEOUT_MS / 1000);
        }
        return -1;
    }

    /* The password is sent only after security owns the foreground PTY and
     * has disabled echo, so it is neither displayed nor stored in history. */
    if (startup_write_all(ssh, password, (int)strlen(password), 3000) != 0 ||
        startup_write_all(ssh, "\n", 1, 3000) != 0) {
        snprintf(err, (size_t)err_sz, "keychain password write failed");
        return -1;
    }

    rc = wait_for_remote_text(ssh, term, frag, frag_len, result_marker,
                              KEYCHAIN_RESULT_TIMEOUT_MS,
                              capture, sizeof(capture));
    if (rc <= 0) {
        if (rc == -2) {
            snprintf(err, (size_t)err_sz,
                     "keychain bootstrap aborted (START)");
        } else if (rc < 0) {
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected during keychain verification");
        } else {
            snprintf(err, (size_t)err_sz,
                     "keychain result timed out after %ds",
                     KEYCHAIN_RESULT_TIMEOUT_MS / 1000);
        }
        return -1;
    }
    return parse_keychain_result(capture, report, err, err_sz);
}

/* Wall-clock of the last successful ssh_write.  Compared against
 * last_rx_at by the main loop's interactivity-stall detector — if we
 * sent input recently but haven't received anything back, the network
 * is unresponsive even when libssh2 hasn't yet declared the socket
 * dead.  Updated only by send_to_ssh below. */
static time_t g_last_tx_at = 0;

static void clear_secret(char *s, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (len-- > 0) *p++ = 0;
}

static void render_connecting_frame(C3D_RenderTarget *top,
                                    C3D_RenderTarget *bot,
                                    renderer_t *renderer, terminal_t *term,
                                    softkb_t *keyboard,
                                    keyboard_t *physical_keyboard) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top, C2D_Color32(0x1a, 0x1b, 0x26, 0xff));
    C2D_SceneBegin(top);
    renderer_draw_terminal(renderer, term);
    C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
    C2D_SceneBegin(bot);
    softkb_draw(keyboard, renderer, physical_keyboard);
    C3D_FrameEnd(0);
}

/* Run the macOS keychain bootstrap on a freshly connected window, if a
 * keychain password is configured (no-op otherwise).  Shared between the
 * initial connect and every reconnect path (SELECT / SETTINGS), so a
 * reconnected macOS session gets its keychain unlocked again.  Returns
 * the window's session pointer, or NULL if a hard disconnect during
 * bootstrap tore the session down. */
static ssh_client_t *keychain_bootstrap(ssh_window_t *win,
                                        const ssh_config_t *cfg,
                                        renderer_t *r,
                                        softkb_t *kb, keyboard_t *kbd,
                                        C3D_RenderTarget *top,
                                        C3D_RenderTarget *bot,
                                        char *status_buf, int status_sz,
                                        uint32_t *status_color,
                                        char *err, int err_sz) {
    ssh_client_t *ssh = win->ssh;
    if (!ssh || !cfg->macos_keychain_password[0]) return ssh;

    /* Keep this local-only progress line visible while bootstrap blocks,
     * then reset again so fish CPR uses remote coordinates. */
    terminal_write(win->term,
                   "\x1b[36mUnlocking macOS keychain...\x1b[0m\r\n");
    render_connecting_frame(top, bot, r, win->term, kb, kbd);
    terminal_reset(win->term);

    keychain_report_t report = { -1, -1 };
    int unlock_rc = unlock_macos_keychain(
        win->ssh, win->term, win->utf8_frag, &win->frag_len,
        cfg->macos_keychain_password, &report, err, err_sz);

    /* ssh_read()/ssh_write() clear the connected flag on a hard error.
     * Do not leave a non-NULL but unusable session in the idle loop. */
    if (!ssh_is_connected(win->ssh)) {
        if (unlock_rc == 0)
            snprintf(err, (size_t)err_sz,
                     "SSH disconnected during keychain bootstrap");
        char line[320];
        snprintf(line, sizeof(line), "\x1b[31mSSH error:\x1b[0m %s\r\n", err);
        terminal_write(win->term, line);
        snprintf(status_buf, (size_t)status_sz, "ssh err");
        *status_color = COLOR_ERR;
        ssh_disconnect(win->ssh);
        win->ssh = NULL;
        return NULL;
    }
    if (unlock_rc != 0) {
        /* Completed commands print FAILED + exit codes themselves.
         * Only transport/prompt timeouts need a local fallback. */
        if (report.unlock_status < 0 && report.verify_status < 0) {
            /* A prompt/result timeout can leave `security` owning the
             * foreground PTY. Abort it before handing control to the
             * user so keyboard input reaches the normal shell. */
            (void)startup_write_all(win->ssh, "\x03\n", 2, 2000);
            char line[320];
            snprintf(line, sizeof(line),
                     "\x1b[33mkeychain bootstrap failed:\x1b[0m %s\r\n", err);
            terminal_write(win->term, line);
        }
    }
    return win->ssh;
}

/* Snap the local terminal view to the bottom (canceling any user-side
 * scrollback peek) right before sending a key.  This way the user always
 * sees what they just typed, even if they were glancing at history. */
static void send_to_ssh(ssh_client_t *ssh, terminal_t *term,
                        const char *bytes, int n, mascot_t *mc) {
    if (!ssh || !ssh_is_connected(ssh) || n <= 0) return;
    if (term && term->sb_offset != 0) terminal_scroll_view(term, -term->sb_offset);
    ssh_write(ssh, bytes, n);
    g_last_tx_at = time(NULL);
    /* Kick the typing animation once per send so the crab pecks its
     * claws on every keystroke / handwriting commit / voice glyph. */
    if (mc) mascot_type_kick(mc);
}

/* Establish (or re-establish) the SSH session for server slot `slot`.
 * Used for the initial connect (slot 0), the SELECT-key reconnect of the
 * active window and the SETTINGS RECONNECT button.  On success: returns
 * a new ssh_client_t, resets the window's local terminal (fish CPR needs
 * remote coordinates), sizes the PTY and sets a status string.  On
 * failure: returns NULL, writes the red SSH-error banner + the
 * diagnostic in err.  Dispatches on the slot's auth mode: "password"
 * servers use ssh_connect_password, everything else publickey. */
static ssh_client_t *reconnect_ssh(const ssh_config_t *cfg, int slot,
                                   ts3ds *tailscale,
                                   terminal_t *term,
                                   char *status_buf, int status_sz,
                                   char *err, int err_sz) {
    if (slot < 0 || slot >= cfg->server_count) slot = 0;
    const ssh_server_t *srv = &cfg->servers[slot];
    ssh_client_t *ssh;
    if (srv->auth == SSH_AUTH_PASSWORD) {
        ssh = ssh_connect_password(srv->host, srv->port, srv->user,
                                   srv->password,
                                   R_TOP_COLS, R_TOP_ROWS,
                                   tailscale, err, err_sz);
    } else {
        ssh = ssh_connect_pubkey(srv->host, srv->port, srv->user,
                                 srv->key_path, NULL,
                                 srv->passphrase[0] ? srv->passphrase : NULL,
                                 R_TOP_COLS, R_TOP_ROWS,
                                 tailscale, err, err_sz);
    }

    if (!ssh) {
        char line[256];
        snprintf(line, sizeof(line), "\x1b[31mSSH error:\x1b[0m %s\r\n", err);
        terminal_write(term, line);
        snprintf(status_buf, status_sz, "ssh err");
        return NULL;
    }

    /* Local banners ("Reconnecting...", startup text) are not part of the
     * remote PTY screen. Reset before parsing shell output so fish's
     * cursor-position queries see the same coordinate system as sshd. */
    terminal_reset(term);
    ssh_set_pty_size(ssh, R_TOP_COLS, R_TOP_ROWS);
    snprintf(status_buf, status_sz, "connected %.56s:%d",
             srv->host, srv->port);
    return ssh;
}

/* Shared by the SELECT-key reconnect and the SETTINGS-page RECONNECT
 * button.  The caller must have torn down the window's old session
 * first (voice abort + ssh_disconnect + win->ssh = NULL).  Renders a
 * "Reconnecting..." frame (so the banner is visible during the blocking
 * handshake), dials the window's server slot and re-runs the keychain
 * bootstrap.  Returns the new session or NULL; on success the caller
 * clears the window's dead flag and celebrates. */
static ssh_client_t *attempt_reconnect(const ssh_config_t *cfg, int slot,
                                       ts3ds *tailscale,
                                       ssh_window_t *win,
                                       renderer_t *r,
                                       softkb_t *kb,
                                       keyboard_t *kbd,
                                       mascot_t *mc,
                                       C3D_RenderTarget *top,
                                       C3D_RenderTarget *bot,
                                       char *status_buf, int status_sz,
                                       uint32_t *status_color,
                                       char *err, int err_sz) {
    terminal_write(win->term, "\x1b[33mReconnecting...\x1b[0m\r\n");
    /* Put the crab into its "looking up / waiting" pose before we flush
     * the frame below, so the user sees it react. */
    mascot_set_reconnecting(mc, 1);
    /* The blocking handshake below stalls the loop — flush one frame
     * right now so the banner is actually on screen during the wait. */
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top, C2D_Color32(0x1a, 0x1b, 0x26, 0xff));
    C2D_SceneBegin(top);
    renderer_draw_terminal(r, win->term);
    C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
    C2D_SceneBegin(bot);
    softkb_draw(kb, r, kbd);
    /* Bottom row: clock + window/mascot buttons + mascot, mirroring the
     * main render path so the reconnect frame isn't missing anything. */
    if (!softkb_in_debug(kb)) {
        char clock_buf[24];
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(clock_buf, sizeof(clock_buf),
                 "%02d-%02d %02d:%02d",
                 lt.tm_mon + 1, lt.tm_mday,
                 lt.tm_hour, lt.tm_min);
        renderer_draw_text_px(2, 221, clock_buf, COLOR_DIM);
        if (softkb_mascot_enabled(kb)) mascot_draw(mc);
    }
    C3D_FrameEnd(0);

    ssh_client_t *ssh = reconnect_ssh(cfg, slot, tailscale, win->term,
                                      status_buf, status_sz,
                                      err, err_sz);
    /* Reconnected to a macOS host: its login keychain locked again with
     * the old session — unlock it again so Claude Code keeps finding
     * its credentials. */
    ssh = keychain_bootstrap(win, cfg, r, kb, kbd, top, bot,
                             status_buf, status_sz, status_color,
                             err, err_sz);
    mascot_set_reconnecting(mc, 0);
    return ssh;
}

int main(int argc, char *argv[]) {
    char err[256] = {0};
    char status_buf[80] = "starting...";
    uint32_t status_color = COLOR_WARN;
    /* `ssh` / `term` are aliases of the ACTIVE window's session and
     * terminal; they are re-synced on window switches, reconnects and
     * disconnects.  Window 0's terminal exists from the start (early
     * banners + the first connection live there); other windows are
     * created lazily on first switch (~580 KB each). */
    ssh_client_t *ssh = NULL;
    terminal_t *term = NULL;
    ts3ds *tailscale = NULL;
    static tailscale_debug_log tailscale_debug;
    tailscale_debug_init(&tailscale_debug);

    /* ── Graphics init (audio disabled — see audio.{c,h} kept for future) ── */
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(32768);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* ── romfs (carries pinyin_dict.bin for M7 IME) ── */
    Result romfs_rc = romfsInit();
    int romfs_ok   = R_SUCCEEDED(romfs_rc);

    /* ── Config ── */
    ssh_config_t cfg;
    int loaded = config_load(&cfg, CONFIG_PATH);
    snprintf(status_buf, sizeof(status_buf),
             loaded ? "config: SD" : "config: defaults");

    /* Seed RNG so the mascot's idle/walk transitions don't repeat across
     * runs.  time(NULL) is fine — we don't need cryptographic randomness. */
    srand((unsigned)time(NULL));

    /* ── Sub-systems ── */
    /* Window 0 owns the early banners and the boot connection; its
     * terminal is created here.  `term` stays an alias of the ACTIVE
     * window's terminal for the rest of main(). */
    g_win[0].term = terminal_init(R_TOP_COLS, R_TOP_ROWS);
    term = g_win[0].term;
    renderer_t *r    = renderer_init(top, bot);
    keyboard_t *kbd  = keyboard_init();
    /* Mascot lives in the bottom row (y=214..239, 26 px tall).  Clock
     * occupies x=2..67 on the left; mascot scampers in x=72..254 — the
     * pinned WIN (x=260..288) and SET (x=290..318) buttons own the right
     * corner.  Crab is 11 px tall (10-row body + 1-row feet) so
     * y_top = 214 + (26-11)/2 = 221 centers it in the row. */
    mascot_t   *mc   = mascot_init(72, 254, 221);
    /* IME loads later (after the M7 banner pumps); softkb tolerates
     * a NULL ime by falling back to passthrough in CN mode. */
    ime_t      *ime  = NULL;
    softkb_t   *kb   = softkb_init(NULL);
    /* Voice input: physical START toggles record/transcribe.  Server
     * needs `dssh-whisper-shim` on PATH (installed by
     * tools/install_whisper_server.sh), or an HTTP voice API endpoint in
     * the config.  voice_t is allocated up-front; mic only opens during
     * the RECORDING state. */
    voice_t    *voice = voice_init();
    if (kb && voice) softkb_set_voice(kb, voice);
    /* SETTINGS page edits cfg in place; SAVE persists it to the SD card
     * and refreshes the voice API endpoint. */
    if (kb) softkb_set_config(kb, &cfg);
    voice_set_api(voice, cfg.voice_api_url);
    {
        char wl[8];
        win_label(wl, sizeof(wl), g_active + 1, cfg.server_count);
        softkb_set_win_info(kb, wl, cfg.server_count);
    }
    /* M12: AI-ask modal — pops over the soft keyboard when the user
     * presses L+START and a Q&A returns from DeepSeek. */
    ai_modal_t *aim   = ai_modal_init();
    if (!term || !r || !kbd || !kb || !mc) goto cleanup;

    if (net_init(err, sizeof(err)) != 0) {
        snprintf(status_buf, sizeof(status_buf), "net err: %s", err);
        status_color = COLOR_ERR;
        goto idle_loop;
    }

    /* Banner inside the terminal. */
    terminal_write(term,
                   "\x1b[36mDSSH\x1b[0m  "
                   "\x1b[2m●\x1b[0m  pinyin IME (R toggle)  "
                   "\x1b[2m●\x1b[0m  voice (START to record)\r\n");
    if (romfs_ok) {
        terminal_write(term, "loading pinyin dictionary...\r\n");
    } else {
        terminal_write(term, "\x1b[33mromfs init failed — IME unavailable\x1b[0m\r\n");
    }

    if (config_tailscale_should_start(&cfg)) {
        ts3ds_config tailscale_config;
        ts3ds_config_init(&tailscale_config);
        tailscale_config.auth_key = cfg.tailscale_auth_key[0]
                                        ? cfg.tailscale_auth_key : NULL;
        tailscale_config.hostname = cfg.tailscale_hostname;
        tailscale_config.state_path = cfg.tailscale_state;
        tailscale_config.control_url = cfg.tailscale_control_url;
        if (ts3ds_path_policy_parse(DSSH_TAILSCALE_PATH,
                                    &tailscale_config.path_policy) !=
            TS3DS_OK) {
            char line[192];
            snprintf(line, sizeof(line),
                     "\x1b[31mTailscale build error:\x1b[0m invalid "
                     "path policy='%.64s' (use auto, direct, "
                     "peer-relay, or derp)\r\n",
                     DSSH_TAILSCALE_PATH);
            terminal_write(term, line);
            snprintf(status_buf, sizeof(status_buf), "tailscale build err");
            status_color = COLOR_ERR;
            goto idle_loop;
        }
        if (DSSH_TAILSCALE_VERBOSE) {
            tailscale_config.log = tailscale_debug_capture;
            tailscale_config.log_userdata = &tailscale_debug;
        }
        terminal_write(term,
                       "\x1b[36mConnecting to Tailscale...\x1b[0m\r\n");
        if (DSSH_TAILSCALE_VERBOSE) {
            char line[256];
            snprintf(line, sizeof(line),
                     "  node=%.63s auth=%s path=%.16s state=%.96s"
                     "\r\n",
                     cfg.tailscale_hostname,
                     cfg.tailscale_auth_key[0] ? "yes" : "no",
                     ts3ds_path_policy_name(tailscale_config.path_policy),
                     cfg.tailscale_state);
            terminal_write(term, line);
        }
        render_connecting_frame(top, bot, r, term, kb, kbd);
        tailscale = ts3ds_new(&tailscale_config);
        int tailscale_result = tailscale ? ts3ds_up(tailscale)
                                         : TS3DS_ERR_ARGUMENT;
        memset(cfg.tailscale_auth_key, 0,
               sizeof(cfg.tailscale_auth_key));
        tailscale_debug_flush(&tailscale_debug, term);
        if (!tailscale || tailscale_result != TS3DS_OK) {
            const char *reason = tailscale
                                     ? ts3ds_last_error(tailscale)
                                     : "invalid Tailscale configuration";
            char line[256];
            snprintf(line, sizeof(line),
                     "\x1b[31mTailscale error rc=%d status=%d:\x1b[0m "
                     "%.170s\r\n", tailscale_result,
                     tailscale ? (int)ts3ds_get_status(tailscale) : -1,
                     reason);
            terminal_write(term, line);
            snprintf(status_buf, sizeof(status_buf), "tailscale err");
            status_color = COLOR_ERR;
            tailscale_debug_set_runtime(&tailscale_debug);
            goto idle_loop;
        }
        {
            uint32_t ip = ts3ds_get_ipv4(tailscale);
            char line[96];
            snprintf(line, sizeof(line),
                     "\x1b[32mTailscale online:\x1b[0m %u.%u.%u.%u\r\n",
                     (unsigned)((ip >> 24) & 0xff),
                     (unsigned)((ip >> 16) & 0xff),
                     (unsigned)((ip >> 8) & 0xff),
                     (unsigned)(ip & 0xff));
            terminal_write(term, line);
        }
    }

    /* Pump one frame so the user sees the loading banner during the
     * (synchronous, ~5s) dict read.  The bottom screen still has the
     * keyboard rendered — the badge and mascot work normally. */
    render_connecting_frame(top, bot, r, term, kb, kbd);

    /* Load the pinyin dict (~9 MB).  Failure here is non-fatal — we
     * just leave ime NULL and softkb degrades CN mode to passthrough. */
    if (romfs_ok) {
        ime = ime_init("romfs:/pinyin_dict.bin");
        if (ime) {
            softkb_set_ime(kb, ime);
            keyboard_set_ime(kbd, ime);
            terminal_write(term, "\x1b[32mdictionary loaded.\x1b[0m\r\n");
        } else {
            terminal_write(term, "\x1b[31mdictionary load failed — IME disabled\x1b[0m\r\n");
        }
    }

    {
        const ssh_server_t *srv = config_active_const(&cfg);
        char banner[224];
        snprintf(banner, sizeof(banner),
                 "connecting to \x1b[33m%.48s@%.64s:%d\x1b[0m (%s auth)...\r\n",
                 srv->user, srv->host, srv->port,
                 srv->auth == SSH_AUTH_PASSWORD ? "password" : "publickey");
        terminal_write(term, banner);
    }

    /* Pump again so the user sees the loaded/connecting banners before
     * the SSH handshake blocks the main loop. */
    render_connecting_frame(top, bot, r, term, kb, kbd);

    ssh = reconnect_ssh(&cfg, 0, tailscale, term, status_buf,
                        sizeof(status_buf), err, sizeof(err));
    g_win[0].ssh = ssh;
    if (!ssh) g_win[0].dead = 1;
    g_win[0].last_rx = time(NULL);

    tailscale_debug_set_runtime(&tailscale_debug);
    tailscale_debug_flush(&tailscale_debug, term);
    status_color = ssh ? COLOR_OK : COLOR_ERR;

    ssh = keychain_bootstrap(&g_win[0], &cfg, r, kb, kbd, top, bot,
                             status_buf, sizeof(status_buf), &status_color,
                             err, sizeof(err));

    /* NOTE: the per-server passwords/passphrases and the global
     * macos_keychain_password intentionally stay in memory — the
     * SELECT-key reconnect path re-authenticates with them.  All of them
     * are wiped by clear_secret() in the cleanup path at exit. */

idle_loop:
    {
        char rbuf[READ_BUFSZ];

        /* ALERT triggers (mascot raises the red ✕), tracked for the
         * ACTIVE window:
         *
         *  - Interactive-stall (5 s): user sent bytes more recently than
         *    we've received any reply, and no reply has come for 5 s.
         *    Means the user is actively waiting and not getting through.
         *
         *  - Dead session: ssh_read returned < 0 or the window was never
         *    connected.  The alert stays on until SELECT reconnects. */
        const int STALL_TX_NORX_S = 5;
        g_last_tx_at      = time(NULL);
        int    stall_alert = 0;
        int    ssh_dead    = ssh ? 0 : 1;

        while (aptMainLoop()) {
            if (tailscale && ts3ds_get_status(tailscale) ==
                                 TS3DS_STATUS_ONLINE)
                ts3ds_poll(tailscale);
            tailscale_debug_flush(&tailscale_debug, term);
            hidScanInput();
            u32 down = hidKeysDown();
            u32 held = hidKeysHeld();
            circlePosition cpad;
            hidCircleRead(&cpad);

            /* M12 — AI-ask modal swallows input.  When the modal is up
             * (or animating in/out), suppress all routing to softkb /
             * keyboard / IME / mascot.  A=keep history, B=clear, touch
             * anywhere on the bottom screen=clear. */
            int modal_open = ai_modal_visible(aim);

            /* SETTINGS edit mode owns A / B / SELECT before anything
             * else: A commits the field buffer, B backspaces one glyph,
             * SELECT cancels.  Tapped keyboard bytes are redirected in
             * the touch section below. */
            int set_open = softkb_in_settings(kb);
            int set_edit = softkb_settings_editing(kb);
            if (set_edit) {
                if (down & KEY_A)      softkb_settings_commit(kb);
                if (down & KEY_B)      softkb_settings_backspace(kb);
                if (down & KEY_SELECT) softkb_settings_cancel(kb);
            }

            if (modal_open) {
                if (down & KEY_A) {
                    voice_ai_close_keep(voice);
                    ai_modal_close(aim);
                }
                if (down & KEY_B) {
                    voice_ai_close_clear(voice);
                    ai_modal_close(aim);
                }
            } else if (!set_open) {
                /* L + START → AI ask mode.  Plain START → voice-IME mode.
                 * 3DS HOME exits any homebrew, so START is fully ours.
                 * Suppressed while the SETTINGS page is open — voice types
                 * into the SSH shell, which is not what a user editing
                 * server fields expects. */
                if (down & KEY_START) {
                    if (held & KEY_L) voice_ai_toggle(voice, ssh);
                    else              voice_toggle(voice, ssh);
                }
            }

            /* Per-frame voice tick — drives recording cap, async aux
             * channel write/eof/read, and forwards the transcribed
             * Chinese text to the main SSH channel via ssh_write.  Also
             * transitions us into VOICE_AI_SHOWING after a successful
             * AI transcribe; we open the modal once that happens. */
            voice_state_t prev_state = voice_state(voice);
            voice_tick(voice, ssh);
            voice_state_t cur_state = voice_state(voice);
            if (prev_state != VOICE_AI_SHOWING &&
                cur_state == VOICE_AI_SHOWING) {
                ai_modal_open(aim,
                              voice_ai_question(voice),
                              voice_ai_answer(voice));
            }
            ai_modal_tick(aim);

            /* Voice ↔ mascot link (level-driven, every frame).  RECORDING
             * → mic pose; TRANSCRIBING → ? pose; TYPING → no static pose
             * (the per-glyph mascot_type_kick below drives the typing
             * peck instead, same animation the soft keyboard uses).  We
             * must drive this every frame rather than on state edges,
             * because voice_toggle() (IDLE→RECORDING) and begin_transcribe()
             * (RECORDING→TRANSCRIBING) run inside the key-handler /
             * voice_tick before we sample prev_state, so an edge-triggered
             * link would sample prev==cur and skip the exact transition
             * we need to catch. */
            int rec  = (cur_state == VOICE_RECORDING);
            int thk  = (cur_state == VOICE_TRANSCRIBING);
            if (rec)      { mascot_set_recording(mc, 1); mascot_set_thinking(mc, 0); }
            else if (thk) { mascot_set_thinking(mc, 1); mascot_set_recording(mc, 0); }
            else          { mascot_set_recording(mc, 0); mascot_set_thinking(mc, 0); }

            /* Voice TYPING streams glyphs straight to ssh_write inside
             * voice.c, bypassing send_to_ssh — so the per-glyph typing
             * kick wired into send_to_ssh wouldn't fire.  Latch on the
             * voice side, consume here and kick manually so the crab
             * pecks in sync with each streamed character. */
            if (cur_state == VOICE_TYPING && voice_consume_typed(voice)) {
                mascot_type_kick(mc);
            }

            /* ── SSH receive — EVERY window, so background shells keep
             * running and their terminals stay current for instant
             * switching.  This is also a flow-control requirement: a
             * background session whose output is left unread fills its
             * libssh2 window and the remote process blocks. ── */
            for (int wi = 0; wi < cfg.server_count; wi++) {
                ssh_window_t *w = &g_win[wi];
                if (!w->ssh || !ssh_is_connected(w->ssh)) continue;
                ssh_keepalive_tick(w->ssh);
                int n = ssh_read(w->ssh, rbuf, sizeof(rbuf));
                if (n > 0) {
                    /* Debug recv-ring only traces the visible window. */
                    if (wi == g_active) softkb_record_recv(kb, rbuf, n);
                    feed_terminal(w->term, w->utf8_frag, &w->frag_len,
                                  rbuf, n);
                    /* Interactive shells such as fish query cursor/device
                     * state and wait for the terminal emulator to reply.
                     * Background windows must answer too, or their shell
                     * stalls on a pending query. */
                    flush_terminal_responses(w->ssh, w->term);
                    w->last_rx = time(NULL);
                } else if (n < 0) {
                    /* Hard disconnect.  Tear the session down and mark
                     * the window dead.  For the ACTIVE window this also
                     * aborts in-flight voice work (its aux channel dies
                     * with the session) and shows the recovery banner. */
                    ssh_disconnect(w->ssh);
                    w->ssh = NULL;
                    w->dead = 1;
                    if (wi == g_active) {
                        voice_abort(voice);
                        ssh = NULL;
                        ssh_dead = 1;
                        terminal_write(w->term,
                            "\r\n\x1b[31mConnection lost\r\n\x1b[0m"
                            "\x1b[33mPress SELECT to reconnect\r\n\x1b[0m");
                    }
                }
            }

            /* ALERT = hard disconnect, OR active interactive stall.
             * While voice input is active (RECORDING/TRANSCRIBING/TYPING)
             * the user is talking to the mic, not typing, so the SSH
             * channel going quiet is expected.  Skip the stall detector
             * entirely during voice so it can neither raise nor lower
             * the alert — the crab is owned by the voice-state link
             * above (mic / think poses).  A real hard disconnect is
             * still caught the moment voice goes idle again. */
            time_t now_t = time(NULL);
            voice_state_t vs = voice_state(voice);
            int voice_busy = (vs == VOICE_RECORDING || vs == VOICE_TRANSCRIBING ||
                              vs == VOICE_TYPING);
            if (!voice_busy) {
                int interactive_stall =
                    ssh && ssh_is_connected(ssh) &&
                    g_last_tx_at > g_win[g_active].last_rx &&
                    (now_t - g_last_tx_at) > STALL_TX_NORX_S;
                int want_alert = ssh_dead || interactive_stall;
                if (want_alert != stall_alert) {
                    stall_alert = want_alert;
                    mascot_set_alert(mc, stall_alert);
                }
            }

            /* ── SELECT reconnect (only when the active window's session
             * is dead) ── A hard disconnect (lid-close sleep) leaves
             * ssh_dead=1.  Pressing SELECT re-dials the ACTIVE window's
             * server slot, so the user recovers without relaunching.
             * While connected, SELECT falls through to
             * keyboard_handle_input as Esc (see the input_down mask).
             *
             * select_consumed records that THIS frame's SELECT triggered
             * a reconnect.  It can't be derived from ssh_dead because a
             * successful reconnect clears ssh_dead to 0 in this same
             * frame — without an independent flag, that just-used SELECT
             * would slip through to keyboard_handle_input as a stray Esc
             * into the freshly opened session. */
            int select_consumed = 0;
            if (ssh_dead && !modal_open && !set_edit && (down & KEY_SELECT)) {
                select_consumed = 1;
                ssh = attempt_reconnect(&cfg, g_active, tailscale,
                                        &g_win[g_active],
                                        r, kb, kbd, mc, top, bot,
                                        status_buf, sizeof(status_buf),
                                        &status_color, err, sizeof(err));
                if (ssh) {
                    g_win[g_active].dead = 0;
                    g_win[g_active].ssh  = ssh;
                    g_win[g_active].last_rx = time(NULL);
                    stall_alert = 0;
                    mascot_celebrate(mc);
                    g_last_tx_at = time(NULL);
                } else {
                    mascot_sadden(mc);
                    terminal_write(term,
                        "\x1b[31mReconnect failed; press SELECT to retry.\x1b[0m\r\n");
                }
            }

            /* ── Physical keys (Esc / Enter / BS / D-pad / R / scroll) ──
             * When the AI-ask modal is showing (or animating), suppress
             * the keyboard handler entirely so A=close-keep doesn't
             * also fire Enter to the SSH terminal, B=close-clear
             * doesn't also fire Backspace, etc.  We pass `down=0` so
             * the handler still ticks held-state timers (keeping
             * modifier release detection coherent for the post-modal
             * world) but registers no edge events. */
            u32 input_down = (modal_open || set_open) ? 0u : down;
            /* Mask SELECT when the session is dead or was just consumed by
             * the reconnect above, so it isn't emitted as Esc. */
            if (ssh_dead || select_consumed) input_down &= ~KEY_SELECT;
            const char *out = keyboard_handle_input(kbd, term, input_down, held, cpad.dy);
            if (out && !modal_open) send_to_ssh(ssh, term, out, (int)strlen(out), mc);

            /* ── Touch / soft keyboard ── */
            int touch_pressed = (held & KEY_TOUCH) ? 1 : 0;
            int tx = -1, ty = -1;
            if (touch_pressed) {
                touchPosition tp;
                hidTouchRead(&tp);
                tx = tp.px; ty = tp.py;
            }
            int touch_down = (down & KEY_TOUCH) ? 1 : 0;

            /* Bottom row (y >= 214) belongs to mascot — taps there don't
             * reach softkb.  On the down-edge we hit-test the crab; if
             * it's where the finger lands the crab flees.
             *
             * The mascot is suppressed entirely when the debug overlay
             * is up or the user has toggled it off, so taps in that
             * region fall through to softkb_touch (which no-ops since
             * no key extends below y=213 in normal mode, and the debug
             * page handles its own widgets). */
            int show_mascot = !softkb_in_debug(kb) && softkb_mascot_enabled(kb);
            if (modal_open) {
                /* While the AI-ask modal is up, any bottom-screen tap
                 * dismisses it (= clear history).  Block all routing
                 * to softkb / mascot. */
                if (touch_down) {
                    voice_ai_close_clear(voice);
                    ai_modal_close(aim);
                }
            } else if (touch_down && ty >= 214 && show_mascot &&
                       !softkb_settings_button_hit(kb, tx, ty) &&
                       !softkb_win_button_hit(kb, tx, ty)) {
                /* The pinned WIN/SET buttons sit in the mascot's row —
                 * route their taps to softkb, the rest go to the crab. */
                if (mascot_hit_test(mc, tx, ty))
                    mascot_on_touched(mc, tx);
            } else {
                /* Pass the held flag (not just the down-edge) so softkb
                 * can detect holds for auto-repeat.  softkb derives the
                 * down-edge internally from prev_pressed.  On no-touch
                 * frames this also runs the release-fade path. */
                const char *kt = softkb_touch(kb, kbd, tx, ty, touch_pressed);
                if (kt) {
                    if (set_edit) {
                        /* Field edit in progress: tapped characters go
                         * into the settings buffer, not the SSH shell. */
                        softkb_settings_feed(kb, kt);
                    } else {
                        send_to_ssh(ssh, term, kt, (int)strlen(kt), mc);
                    }
                }
            }

            /* ── SETTINGS actions ──
             * SAVE persists the edited config to the SD card and
             * refreshes the voice API endpoint; RECONNECT tears down the
             * live session and immediately re-dials with the current
             * config (same path as the SELECT reconnect). */
            softkb_action_t act;
            while ((act = softkb_settings_consume_action(kb)) !=
                   SOFTKB_ACT_NONE) {
                if (act == SOFTKB_ACT_SAVE) {
                    if (config_save(&cfg, CONFIG_PATH) == 0) {
                        voice_set_api(voice, cfg.voice_api_url);
                        char wl[8];
                        win_label(wl, sizeof(wl),
                                  g_active + 1, cfg.server_count);
                        softkb_set_win_info(kb, wl, cfg.server_count);
                        terminal_write(term,
                            "\x1b[32msettings saved to SD\x1b[0m\r\n");
                    } else {
                        terminal_write(term,
                            "\x1b[31mconfig save FAILED (SD writable?)\x1b[0m"
                            "\r\n");
                    }
                } else if (act == SOFTKB_ACT_RECONNECT) {
                    voice_abort(voice);
                    if (ssh) {
                        ssh_disconnect(ssh);
                        ssh = NULL;
                    }
                    g_win[g_active].ssh  = NULL;
                    ssh_dead = 1;
                    ssh = attempt_reconnect(&cfg, g_active, tailscale,
                                            &g_win[g_active],
                                            r, kb, kbd, mc, top, bot,
                                            status_buf, sizeof(status_buf),
                                            &status_color, err, sizeof(err));
                    if (ssh) {
                        g_win[g_active].ssh  = ssh;
                        g_win[g_active].dead = 0;
                        g_win[g_active].last_rx = time(NULL);
                        ssh_dead    = 0;
                        stall_alert = 0;
                        mascot_celebrate(mc);
                        g_last_tx_at = time(NULL);
                    } else {
                        mascot_sadden(mc);
                        terminal_write(term,
                            "\x1b[31mReconnect failed; press SELECT to "
                            "retry.\x1b[0m\r\n");
                    }
                } else if (act == SOFTKB_ACT_WIN_NEXT) {
                    /* Cycle to the next configured slot that has a host.
                     * Switching is instant: the target terminal is created
                     * empty on first visit and connects only when the user
                     * presses SELECT there (no surprise blocking
                     * handshakes mid-cycle). */
                    int next = g_active, tries = 0;
                    do {
                        next = (next + 1) % cfg.server_count;
                        tries++;
                    } while (next != g_active &&
                             !cfg.servers[next].host[0] &&
                             tries < CONFIG_SERVERS_MAX);
                    if (next != g_active && cfg.servers[next].host[0]) {
                        g_active = next;
                        ssh = g_win[next].ssh;
                        if (!g_win[next].term) {
                            g_win[next].term =
                                terminal_init(R_TOP_COLS, R_TOP_ROWS);
                            char hdr[160];
                            const ssh_server_t *ns = &cfg.servers[next];
                            int rc = snprintf(hdr, sizeof(hdr),
                                     "\x1b[90m── window %d: "
                                     "%.36s@%.44s:%d ──\x1b[0m\r\n",
                                     next + 1, ns->user, ns->host, ns->port);
                            if (rc > 0 && rc < (int)sizeof(hdr))
                                terminal_write(g_win[next].term, hdr);
                            if (!ssh)
                                terminal_write(g_win[next].term,
                                    "\x1b[33moffline — SELECT to "
                                    "connect\x1b[0m\r\n");
                        }
                        term = g_win[next].term;
                        ssh_dead = (ssh == NULL);
                        stall_alert = 0;
                        char wl[8];
                        win_label(wl, sizeof(wl),
                                  g_active + 1, cfg.server_count);
                        softkb_set_win_info(kb, wl, cfg.server_count);
                        if (ssh) {
                            /* Resync per-window stall tracking. */
                            g_last_tx_at = time(NULL);
                        }
                        mascot_set_alert(mc, ssh_dead);
                    }
                }
            }

            /* Mascot ticks only when it's actually being shown.  When
             * paused this way it freezes in place; on re-enable it
             * resumes from wherever it stopped. */
            if (show_mascot) mascot_update(mc);

            /* ── Render ── */
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top, C2D_Color32(0x1a, 0x1b, 0x26, 0xff));
            C2D_SceneBegin(top);
            renderer_draw_terminal(r, term);

            C2D_TargetClear(bot, C2D_Color32(0x18, 0x18, 0x25, 0xff));
            C2D_SceneBegin(bot);
            softkb_draw(kb, r, kbd);

            /* Bottom row: clock on the left, mascot on the right.
             * Suppressed entirely when the debug overlay is up — the
             * debug page draws its own full-screen background. */
            if (!softkb_in_debug(kb)) {
                char clock_buf[24];
                time_t now = time(NULL);
                struct tm lt;
                localtime_r(&now, &lt);
                snprintf(clock_buf, sizeof(clock_buf),
                         "%02d-%02d %02d:%02d",
                         lt.tm_mon + 1, lt.tm_mday,
                         lt.tm_hour, lt.tm_min);
                /* Vertically center 12-px text in the 26-px bottom row:
                 * y = 214 + (26-12)/2 = 221. */
                renderer_draw_text_px(2, 221, clock_buf, COLOR_DIM);
                if (softkb_mascot_enabled(kb)) mascot_draw(mc);
            }

            /* M12 — modal lays over softkb + clock + mascot.  Drawn
             * last so its dimming overlay correctly covers everything
             * below. */
            ai_modal_draw(aim);

            C3D_FrameEnd(0);
        }

        /* Release voice's aux channel BEFORE freeing the sessions —
         * voice_free → release_aux would otherwise call libssh2_channel_*
         * on a freed LIBSSH2_SESSION (use-after-free).  Then tear down
         * EVERY window's session — background shells die with the app. */
        voice_abort(voice);
        for (int i = 0; i < cfg.server_count; i++) {
            if (g_win[i].ssh) {
                ssh_disconnect(g_win[i].ssh);
                g_win[i].ssh = NULL;
            }
        }
    }
    if (tailscale) {
        ts3ds_close(tailscale);
        tailscale = NULL;
        tailscale_debug_flush(&tailscale_debug, term);
    }
    net_fini();

cleanup:
    if (tailscale) {
        ts3ds_close(tailscale);
        tailscale = NULL;
        tailscale_debug_flush(&tailscale_debug, term);
    }
    /* Multi-server: wipe every slot's credentials. */
    for (int i = 0; i < cfg.server_count; i++) {
        clear_secret(cfg.servers[i].password,
                     sizeof(cfg.servers[i].password));
        clear_secret(cfg.servers[i].passphrase,
                     sizeof(cfg.servers[i].passphrase));
    }
    clear_secret(cfg.macos_keychain_password,
                 sizeof(cfg.macos_keychain_password));
    clear_secret(cfg.tailscale_auth_key,
                 sizeof(cfg.tailscale_auth_key));
    /* Per-window terminals (window 0's included — `term` is an alias). */
    for (int i = 0; i < CONFIG_SERVERS_MAX; i++) {
        if (g_win[i].term) {
            terminal_free(g_win[i].term);
            g_win[i].term = NULL;
        }
    }
    if (aim)   ai_modal_free(aim);
    if (voice) voice_free(voice);
    if (ime)  ime_free(ime);
    if (mc)   mascot_free(mc);
    if (kb)   softkb_free(kb);
    if (kbd)  keyboard_free(kbd);
    if (r)    renderer_free(r);
    if (romfs_ok) romfsExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}

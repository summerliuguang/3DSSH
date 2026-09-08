#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

/* Remove an inline comment while preserving # inside quoted values. */
static void strip_comment(char *s) {
    char quote = 0;
    int escaped = 0;
    for (; *s; s++) {
        if (quote) {
            if (escaped) {
                escaped = 0;
            } else if (*s == '\\') {
                escaped = 1;
            } else if (*s == quote) {
                quote = 0;
            }
        } else if (*s == '\'' || *s == '"') {
            quote = *s;
        } else if (*s == '#') {
            *s = 0;
            return;
        }
    }
}

/* Strip matching quotes and decode only escaped quote/backslash pairs.
 * Other backslashes remain literal so existing paths/passwords do not change. */
static void unquote(char *s) {
    size_t len = strlen(s);
    if (len < 2 || (s[0] != '\'' && s[0] != '"') || s[len - 1] != s[0])
        return;

    char quote = s[0];
    char *src = s + 1;
    char *end = s + len - 1;
    char *dst = s;
    while (src < end) {
        if (*src == '\\' && src + 1 < end &&
            (src[1] == quote || src[1] == '\\')) {
            src++;
        }
        *dst++ = *src++;
    }
    *dst = 0;
}

static void set_str(char *dst, const char *src) {
    snprintf(dst, CONFIG_STR_MAX, "%s", src);
}

/* Default voice-transcription endpoint, injectable at build time so the
 * binary works out of the box with a personal STT server (e.g. mimo-
 * voice-hub /api/stt) without baking LAN addresses into the repo:
 *   make DSSH_VOICE_API_DEFAULT=https://192.168.x.x:29006/api/stt
 * An explicitly empty value in config.ini still falls back to the SSH
 * shim transport. */
#ifndef DSSH_VOICE_API_DEFAULT
#define DSSH_VOICE_API_DEFAULT ""
#endif

static void server_defaults(ssh_server_t *s) {
    s->host[0] = 0;
    s->port = 22;
    s->user[0] = 0;
    s->auth = SSH_AUTH_KEY;
    s->password[0] = 0;
    s->key_path[0] = 0;
    s->passphrase[0] = 0;
}

/* Fill legacy flat keys into a scratch server — applied as server 1 only
 * when the file carries no serverN_host (backward compatibility). */
static void apply_legacy_defaults(ssh_server_t *s) {
    set_str(s->host, "your-server.example.com");
    s->port = 22;
    set_str(s->user, "ubuntu");
    s->auth = SSH_AUTH_KEY;
    s->password[0] = 0;
    set_str(s->key_path, "sdmc:/3ds/3dssh/id_rsa");
    s->passphrase[0] = 0;
}

int config_load(ssh_config_t *cfg, const char *path) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->server_count = 0;
    cfg->active_server = 0;
    set_str(cfg->voice_api_url, DSSH_VOICE_API_DEFAULT);
    cfg->tailscale_hostname[0] = 0;
    cfg->tailscale_state[0] = 0;
    cfg->tailscale_control_url[0] = 0;

    /* Scratch servers parsed from serverN_ keys; flat legacy keys land in
     * legacy and are folded in below unless a serverN_host was seen. */
    ssh_server_t parsed[CONFIG_SERVERS_MAX];
    unsigned char seen[CONFIG_SERVERS_MAX] = {0};
    ssh_server_t legacy;
    apply_legacy_defaults(&legacy);
    int legacy_host_seen = 0;
    int active_1based = 0;

    for (int i = 0; i < CONFIG_SERVERS_MAX; i++)
        server_defaults(&parsed[i]);

    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[CONFIG_STR_MAX * 2];
        while (fgets(line, sizeof(line), fp)) {
            strip_comment(line);
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *key = line;
            char *val = eq + 1;
            trim(key);
            trim(val);
            unquote(val);
            if (!*key) continue;

            /* ── per-server keys: serverN_field ── */
            if (!strncmp(key, "server", 6) &&
                key[6] >= '1' && key[6] <= '0' + CONFIG_SERVERS_MAX) {
                int idx = key[6] - '1';
                const char *field = key + 7;
                ssh_server_t *s = &parsed[idx];
                seen[idx] = 1;
                if      (!strcmp(field, "_host"))       set_str(s->host, val);
                else if (!strcmp(field, "_port"))       s->port = atoi(val);
                else if (!strcmp(field, "_user"))       set_str(s->user, val);
                else if (!strcmp(field, "_auth"))
                    s->auth = !strcmp(val, "password")
                              ? SSH_AUTH_PASSWORD : SSH_AUTH_KEY;
                else if (!strcmp(field, "_password"))   set_str(s->password, val);
                else if (!strcmp(field, "_key_path"))   set_str(s->key_path, val);
                else if (!strcmp(field, "_passphrase")) set_str(s->passphrase, val);
                continue;
            }

            if      (!strcmp(key, "host"))       { set_str(legacy.host, val); legacy_host_seen = 1; }
            else if (!strcmp(key, "port"))       legacy.port = atoi(val);
            else if (!strcmp(key, "user"))       set_str(legacy.user, val);
            else if (!strcmp(key, "key_path"))   set_str(legacy.key_path, val);
            else if (!strcmp(key, "passphrase")) set_str(legacy.passphrase, val);
            else if (!strcmp(key, "active_server")) active_1based = atoi(val);
            else if (!strcmp(key, "voice_api_url")) set_str(cfg->voice_api_url, val);
            else if (!strcmp(key, "macos_keychain_password"))
                set_str(cfg->macos_keychain_password, val);
            else if (!strcmp(key, "tailscale_auth_key"))
                set_str(cfg->tailscale_auth_key, val);
            else if (!strcmp(key, "tailscale_hostname"))
                set_str(cfg->tailscale_hostname, val);
            else if (!strcmp(key, "tailscale_state"))
                set_str(cfg->tailscale_state, val);
            else if (!strcmp(key, "tailscale_control_url"))
                set_str(cfg->tailscale_control_url, val);
        }
        fclose(fp);
    }

    /* Fold servers in ascending order; legacy flat keys fill slot 1 when
     * the file has no server1_host (fully-legacy or default config). */
    for (int i = 0; i < CONFIG_SERVERS_MAX; i++) {
        if (seen[i]) cfg->servers[cfg->server_count++] = parsed[i];
    }
    if (cfg->server_count == 0 && legacy_host_seen) {
        cfg->servers[cfg->server_count++] = legacy;
    }
    if (cfg->server_count == 0) {
        /* Nothing configured at all — placeholder so the UI and connect
         * path have something to show (matches the old defaults behavior). */
        apply_legacy_defaults(&cfg->servers[0]);
        cfg->server_count = 1;
    }

    if (active_1based >= 1 && active_1based <= cfg->server_count)
        cfg->active_server = active_1based - 1;

    /* An explicitly empty optional value has the same meaning as omitting it. */
    if (!cfg->tailscale_hostname[0])
        set_str(cfg->tailscale_hostname, "dssh-3ds");
    if (!cfg->tailscale_state[0])
        set_str(cfg->tailscale_state,
                "sdmc:/3ds/3dssh/tailscale.state");
    if (!cfg->tailscale_control_url[0])
        set_str(cfg->tailscale_control_url,
                "https://controlplane.tailscale.com");
    return fp ? 1 : 0;
}

/* Append "key = value" with minimal quoting: values containing whitespace,
 * '#', or quotes get double-quoted with backslash escapes (matching what
 * config_load's unquote() understands). */
static void write_kv(FILE *fp, const char *key, const char *val) {
    int needs_quote = *val == 0;
    for (const char *p = val; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '#' || *p == '"' || *p == '\'') {
            needs_quote = 1;
            break;
        }
    }
    if (!needs_quote) {
        fprintf(fp, "%s = %s\n", key, val);
        return;
    }
    fprintf(fp, "%s = \"", key);
    for (const char *p = val; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', fp);
        fputc(*p, fp);
    }
    fputs("\"\n", fp);
}

int config_save(const ssh_config_t *cfg, const char *path) {
    if (!cfg || cfg->server_count <= 0 ||
        cfg->server_count > CONFIG_SERVERS_MAX)
        return -1;

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    fprintf(fp, "# DSSH config — written by the on-device SETTINGS page\n");

    for (int i = 0; i < cfg->server_count; i++) {
        const ssh_server_t *s = &cfg->servers[i];
        fprintf(fp, "\n# ── server %d ──\n", i + 1);
        char nkey[32];
        snprintf(nkey, sizeof(nkey), "server%d_host", i + 1);
        write_kv(fp, nkey, s->host);
        snprintf(nkey, sizeof(nkey), "server%d_port", i + 1);
        fprintf(fp, "%s = %d\n", nkey, s->port);
        snprintf(nkey, sizeof(nkey), "server%d_user", i + 1);
        write_kv(fp, nkey, s->user);
        snprintf(nkey, sizeof(nkey), "server%d_auth", i + 1);
        write_kv(fp, nkey,
                 s->auth == SSH_AUTH_PASSWORD ? "password" : "key");
        snprintf(nkey, sizeof(nkey), "server%d_password", i + 1);
        write_kv(fp, nkey, s->password);
        snprintf(nkey, sizeof(nkey), "server%d_key_path", i + 1);
        write_kv(fp, nkey,
                 s->key_path[0] ? s->key_path : "sdmc:/3ds/3dssh/id_rsa");
        snprintf(nkey, sizeof(nkey), "server%d_passphrase", i + 1);
        write_kv(fp, nkey, s->passphrase);
    }

    fprintf(fp, "\n# ── global ──\n");
    fprintf(fp, "active_server = %d\n", cfg->active_server + 1);
    write_kv(fp, "voice_api_url", cfg->voice_api_url);
    write_kv(fp, "macos_keychain_password", cfg->macos_keychain_password);
    write_kv(fp, "tailscale_auth_key", cfg->tailscale_auth_key);
    write_kv(fp, "tailscale_hostname", cfg->tailscale_hostname);
    write_kv(fp, "tailscale_state", cfg->tailscale_state);
    write_kv(fp, "tailscale_control_url", cfg->tailscale_control_url);

    fclose(fp);
    return 0;
}

ssh_server_t *config_active(ssh_config_t *cfg) {
    if (!cfg) return NULL;
    if (cfg->active_server < 0 || cfg->active_server >= cfg->server_count)
        cfg->active_server = 0;
    return &cfg->servers[cfg->active_server];
}

const ssh_server_t *config_active_const(const ssh_config_t *cfg) {
    return config_active((ssh_config_t *)cfg);
}

int config_tailscale_should_start(const ssh_config_t *cfg) {
    if (!cfg) return 0;
    if (cfg->tailscale_auth_key[0]) return 1;
    if (!cfg->tailscale_state[0]) return 0;

    FILE *state = fopen(cfg->tailscale_state, "rb");
    if (!state) return 0;
    fclose(state);
    return 1;
}

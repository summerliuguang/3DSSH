#ifndef CONFIG_H
#define CONFIG_H

/*
 * Loads SSH connection config from sdmc:/3ds/3dssh/config.ini at startup.
 * Falls back to compiled-in defaults if the file is missing or unparseable.
 *
 * Multi-server support: the on-device SETTINGS page (softkb) edits the
 * active server and writes the file back, so a manually prepared config.ini
 * is only needed for the very first setup — or not at all if the user
 * configures everything on the 3DS.
 *
 * Format (simple key=value, # for comments):
 *
 *   # Global settings
 *   active_server = 1                  # 1-based index into servers
 *   voice_api_url = http://host/api/stt  # optional; empty = SSH shim transport
 *   macos_keychain_password =          # quote values containing # or edge spaces
 *   tailscale_auth_key =               # setting a key enables Tailscale
 *   tailscale_hostname = dssh-3ds
 *   tailscale_state = sdmc:/3ds/3dssh/tailscale.state
 *   tailscale_control_url = https://controlplane.tailscale.com
 *
 *   # Per-server settings, N = 1..CONFIG_SERVERS_MAX
 *   serverN_host = your-server.example.com
 *   serverN_port = 22
 *   serverN_user = ubuntu
 *   serverN_auth = key                 # "key" (default) or "password"
 *   serverN_password =                 # password-auth only
 *   serverN_key_path = sdmc:/3ds/3dssh/id_rsa
 *   serverN_passphrase =               # encrypted-key passphrase
 *
 * Legacy flat keys (host/port/user/key_path/passphrase) are still parsed
 * and mapped to server 1 when no serverN_host is present.
 */

#define CONFIG_STR_MAX 256
#define CONFIG_SERVERS_MAX 4

typedef enum {
    SSH_AUTH_KEY = 0,
    SSH_AUTH_PASSWORD,
} ssh_auth_t;

typedef struct {
    char host[CONFIG_STR_MAX];
    int  port;
    char user[CONFIG_STR_MAX];
    ssh_auth_t auth;
    char password[CONFIG_STR_MAX];   /* password-auth credential */
    char key_path[CONFIG_STR_MAX];   /* PEM RSA private key on SD */
    char passphrase[CONFIG_STR_MAX]; /* encrypted-key passphrase */
} ssh_server_t;

typedef struct {
    ssh_server_t servers[CONFIG_SERVERS_MAX];
    int  server_count;               /* 1..CONFIG_SERVERS_MAX */
    int  active_server;              /* 0-based index into servers */

    /* Optional HTTP voice-transcription API. When set, plain (non-AI)
     * voice recordings POST as WAV to this URL; the JSON response's
     * "text" field is typed into the terminal. Empty = classic SSH-shim
     * transport (dssh-whisper-shim). AI-ask (L+START) always uses the
     * SSH shim regardless. */
    char voice_api_url[CONFIG_STR_MAX];

    /* Global (legacy flat) settings. */
    char macos_keychain_password[CONFIG_STR_MAX];
    char tailscale_auth_key[CONFIG_STR_MAX];
    char tailscale_hostname[CONFIG_STR_MAX];
    char tailscale_state[CONFIG_STR_MAX];
    char tailscale_control_url[CONFIG_STR_MAX];
} ssh_config_t;

/* Fills cfg with defaults then overlays values from config file (if present).
 * Returns 1 if the file was loaded successfully, 0 if defaults only. */
int config_load(ssh_config_t *cfg, const char *path);

/* Write the full config back to `path` (used by the on-device SETTINGS
 * page). Returns 0 on success, -1 on failure. */
int config_save(const ssh_config_t *cfg, const char *path);

/* Active server pointer — always valid (config_load guarantees at least
 * one server slot exists and active_server is in range). */
ssh_server_t *config_active(ssh_config_t *cfg);
const ssh_server_t *config_active_const(const ssh_config_t *cfg);

/* Tailscale starts automatically when an auth key is configured or when a
 * persisted node state exists at tailscale_state. */
int config_tailscale_should_start(const ssh_config_t *cfg);

#endif /* CONFIG_H */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "../source/config.h"

static int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

int main(void) {
    ssh_config_t cfg;
    CHECK(config_load(&cfg, "/definitely/not/a/dssh/config") == 0,
          "missing config should use defaults");
    ssh_server_t *srv = config_active(&cfg);
    CHECK(strcmp(srv->host, "your-server.example.com") == 0,
          "placeholder host default");
    CHECK(strcmp(srv->key_path, "sdmc:/3ds/3dssh/id_rsa") == 0,
          "key path default");
    CHECK(srv->auth == SSH_AUTH_KEY, "default auth should be key");
    CHECK(cfg.server_count == 1, "default config should have one server");
    CHECK(cfg.active_server == 0, "default active server index");
    CHECK(cfg.macos_keychain_password[0] == 0,
          "keychain password should default empty");
    CHECK(cfg.tailscale_auth_key[0] == 0,
          "Tailscale auth key should default empty");
    CHECK(strcmp(cfg.tailscale_hostname, "dssh-3ds") == 0,
          "Tailscale hostname default");
    CHECK(strcmp(cfg.tailscale_state,
                 "sdmc:/3ds/3dssh/tailscale.state") == 0,
          "Tailscale state path default");
    CHECK(config_tailscale_should_start(&cfg) == 0,
          "Tailscale should stay off without key or state");

    /* ── legacy flat-key file still maps to server 1 ── */
    char path[] = "/tmp/dssh-config-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp failed");
    if (fd < 0) return 1;

    FILE *fp = fdopen(fd, "w");
    CHECK(fp != NULL, "fdopen failed");
    if (!fp) {
        close(fd);
        unlink(path);
        return 1;
    }
    fputs("host = mac.example # comment\n"
          "user = alice\n"
          "macos_keychain_password = \"  p#ass\\\"word  \" # comment\n"
          "tailscale_auth_key = test-key\n"
          "tailscale_hostname =\n"
          "tailscale_state =\n"
          "tailscale_control_url =\n",
          fp);
    fclose(fp);

    CHECK(config_load(&cfg, path) == 1, "temporary config should load");
    unlink(path);
    srv = config_active(&cfg);
    CHECK(strcmp(srv->host, "mac.example") == 0, "inline comment parsing");
    CHECK(strcmp(srv->user, "alice") == 0, "legacy user maps to server 1");
    CHECK(strcmp(cfg.macos_keychain_password, "  p#ass\"word  ") == 0,
          "quoted password should preserve spaces, #, and escaped quote");
    CHECK(config_tailscale_should_start(&cfg) == 1,
          "auth key should enable Tailscale automatically");
    CHECK(strcmp(cfg.tailscale_hostname, "dssh-3ds") == 0,
          "empty Tailscale hostname should restore default");
    CHECK(strcmp(cfg.tailscale_state,
                 "sdmc:/3ds/3dssh/tailscale.state") == 0,
          "empty Tailscale state path should restore default");
    CHECK(strcmp(cfg.tailscale_control_url,
                 "https://controlplane.tailscale.com") == 0,
          "empty control URL should restore default");

    /* ── multi-server file: parsing + active selection ── */
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    CHECK(fd >= 0, "multi-server config create failed");
    fp = fdopen(fd, "w");
    CHECK(fp != NULL, "multi-server fdopen failed");
    if (fp) {
        fputs("server1_host = a.example\n"
              "server1_port = 2222\n"
              "server1_user = alice\n"
              "server1_auth = password\n"
              "server1_password = s3cret\n"
              "server2_host = b.example\n"
              "server2_auth = key\n"
              "server2_passphrase = \"key phrase\"\n"
              "server4_host = d.example\n"
              "active_server = 2\n"
              "voice_api_url = http://192.0.2.10:29006/api/stt\n",
              fp);
        fclose(fp);
    } else {
        close(fd);
    }
    CHECK(config_load(&cfg, path) == 1, "multi-server config should load");
    CHECK(cfg.server_count == 3, "server3 absent, server4 folds up to slot 3");
    CHECK(cfg.active_server == 1, "active_server 2 is 1-based");
    CHECK(strcmp(cfg.servers[0].host, "a.example") == 0, "server1 host");
    CHECK(cfg.servers[0].port == 2222, "server1 port");
    CHECK(cfg.servers[0].auth == SSH_AUTH_PASSWORD, "server1 password auth");
    CHECK(strcmp(cfg.servers[0].password, "s3cret") == 0, "server1 password");
    CHECK(cfg.servers[1].auth == SSH_AUTH_KEY, "server2 key auth");
    CHECK(strcmp(cfg.servers[1].passphrase, "key phrase") == 0,
          "server2 passphrase");
    CHECK(strcmp(cfg.servers[2].host, "d.example") == 0,
          "sparse serverN folds into next slot");
    CHECK(strcmp(cfg.voice_api_url,
                 "http://192.0.2.10:29006/api/stt") == 0,
          "voice api url parsed");

    /* ── save → reload roundtrip preserves everything ── */
    CHECK(config_save(&cfg, path) == 0, "config_save should succeed");
    CHECK(config_load(&cfg, path) == 1, "saved config should reload");
    CHECK(cfg.server_count == 3, "roundtrip server count");
    CHECK(cfg.active_server == 1, "roundtrip active server");
    CHECK(strcmp(cfg.servers[0].host, "a.example") == 0, "roundtrip s1 host");
    CHECK(cfg.servers[0].port == 2222, "roundtrip s1 port");
    CHECK(cfg.servers[0].auth == SSH_AUTH_PASSWORD, "roundtrip s1 auth");
    CHECK(strcmp(cfg.servers[0].password, "s3cret") == 0,
          "roundtrip s1 password");
    CHECK(strcmp(cfg.servers[1].passphrase, "key phrase") == 0,
          "roundtrip s2 passphrase");
    CHECK(strcmp(cfg.voice_api_url,
                 "http://192.0.2.10:29006/api/stt") == 0,
          "roundtrip voice api url");
    CHECK(strcmp(cfg.tailscale_state,
                 "sdmc:/3ds/3dssh/tailscale.state") == 0,
          "roundtrip tailscale state default");
    unlink(path);

    /* ── out-of-range active_server clamps ── */
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    CHECK(fd >= 0, "clamp config create failed");
    if (fd >= 0) {
        fp = fdopen(fd, "w");
        CHECK(fp != NULL, "clamp fdopen failed");
        if (fp) {
            fputs("server1_host = only.example\nactive_server = 9\n", fp);
            fclose(fp);
        } else {
            close(fd);
        }
    }
    CHECK(config_load(&cfg, path) == 1, "clamp config should load");
    CHECK(cfg.active_server == 0, "out-of-range active_server clamps to 0");
    unlink(path);

    /* ── legacy unreleased key must stay rejected ── */
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    CHECK(fd >= 0, "legacy-key config create failed");
    if (fd >= 0) {
        fp = fdopen(fd, "w");
        CHECK(fp != NULL, "legacy-key fdopen failed");
        if (fp) {
            fputs("keychain_password = should-not-load\n", fp);
            fclose(fp);
            CHECK(config_load(&cfg, path) == 1,
                  "legacy-key config should still be readable");
            CHECK(cfg.macos_keychain_password[0] == 0,
                  "unreleased legacy key should not be accepted");
        } else {
            close(fd);
        }
        unlink(path);
    }

    if (failures) {
        fprintf(stderr, "%d config test(s) failed\n", failures);
        return 1;
    }
    puts("config tests passed");
    return 0;
}

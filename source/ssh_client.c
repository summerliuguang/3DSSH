#include "ssh_client.h"
#include <3ds.h>
#include <ts3ds.h>
#include <libssh2.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct ssh_client_t {
    int              sock;
    ts3ds_conn      *tailscale_conn;
    void            *transport;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    int              connected;
};

typedef struct ssh_transport {
    ts3ds *tailscale;
    ts3ds_conn *connection;
    int blocking;
} ssh_transport;

/* libssh2's blocking flag alone is insufficient for callback transports:
 * its socket wait path cannot advance libts3ds. Keep both layers in the same
 * mode so the callbacks poll Tailscale while synchronous SSH operations wait. */
static void ssh_set_io_blocking(ssh_client_t *ssh, int blocking) {
    if (!ssh || !ssh->session) return;
    ssh_transport *transport = (ssh_transport *)ssh->transport;
    if (transport) transport->blocking = blocking ? 1 : 0;
    libssh2_session_set_blocking(ssh->session, blocking ? 1 : 0);
}

/* libssh2's callback contract is "-errno", but ts3ds error codes are not
 * errnos: TS3DS_ERR_INTERNAL (-11) would read as -EAGAIN and turn a hard
 * transport failure into an infinite silent retry.  Map AGAIN faithfully
 * and collapse every other error onto -EIO. */
static ssize_t map_ts3ds_result(ssize_t result) {
    if (result >= 0) return result;
    if (result == TS3DS_ERR_AGAIN) return -EAGAIN;
    return -EIO;
}

static ssize_t tailscale_send_cb(libssh2_socket_t socket,
                                 const void *buffer, size_t length, int flags,
                                 void **abstract) {
    ssh_transport *transport = abstract ? (ssh_transport *)*abstract : NULL;
    (void)socket; (void)flags;
    if (!transport || !transport->connection) return -EIO;
    for (;;) {
        ssize_t result = ts3ds_conn_write(transport->connection, buffer, length);
        if (result != TS3DS_ERR_AGAIN || !transport->blocking) {
            return map_ts3ds_result(result);
        }
        ts3ds_poll(transport->tailscale);
        svcSleepThread(1000000);
    }
}

static ssize_t tailscale_recv_cb(libssh2_socket_t socket, void *buffer,
                                 size_t length, int flags, void **abstract) {
    ssh_transport *transport = abstract ? (ssh_transport *)*abstract : NULL;
    (void)socket; (void)flags;
    if (!transport || !transport->connection) return -EIO;
    for (;;) {
        ssize_t result = ts3ds_conn_read(transport->connection, buffer, length);
        if (result != TS3DS_ERR_AGAIN || !transport->blocking) {
            return map_ts3ds_result(result);
        }
        ts3ds_poll(transport->tailscale);
        svcSleepThread(1000000);
    }
}

static void copy_err(char *dst, int dst_sz, const char *src) {
    if (dst && dst_sz > 0) {
        snprintf(dst, dst_sz, "%s", src ? src : "(unknown)");
    }
}

/* Capture both the function's own return value and libssh2 session's last
 * recorded error. They can disagree, especially in mbedTLS backend paths
 * where some failures don't get logged into the session. */
static void copy_libssh2_err(char *dst, int dst_sz, LIBSSH2_SESSION *sess,
                             const char *prefix, int fn_rc) {
    if (!dst || dst_sz <= 0) return;
    char *msg = NULL;
    int msg_len = 0;
    int sess_errnum = libssh2_session_last_error(sess, &msg, &msg_len, 0);
    const char *msg_disp = (msg && msg_len > 0) ? msg : "(empty)";
    int eff_len = (msg && msg_len > 0) ? msg_len : 7;
    snprintf(dst, dst_sz, "%s fn=%d sess=%d \"%.*s\"",
             prefix, fn_rc, sess_errnum, eff_len, msg_disp);
}

static int tcp_connect(const char *host, int port, char *err, int err_sz) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        copy_err(err, err_sz, "DNS lookup failed");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        copy_err(err, err_sz, "socket() failed");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        copy_err(err, err_sz, "connect() refused / unreachable");
        closesocket(sock);
        return -1;
    }
    return sock;
}

static void close_pending_transport(int sock, ts3ds_conn *connection,
                                    ssh_transport *transport) {
    if (sock >= 0) closesocket(sock);
    if (connection) ts3ds_conn_close(connection);
    free(transport);
}

/* Dial TCP (or Tailscale) and run the blocking SSH banner/kex handshake.
 * On failure: cleans up every resource it created, calls libssh2_exit(),
 * fills err_buf, and returns NULL.  On success the outputs describe the
 * resources the caller now owns. */
static LIBSSH2_SESSION *dial_session(const char *host, int port,
                                     ts3ds *tailscale,
                                     int *sock_out,
                                     ts3ds_conn **tsconn_out,
                                     ssh_transport **transport_out,
                                     char *err_buf, int err_sz) {
    *sock_out = -1;
    *tsconn_out = NULL;
    *transport_out = NULL;

    if (libssh2_init(0) != 0) {
        copy_err(err_buf, err_sz, "libssh2_init failed");
        return NULL;
    }

    if (tailscale) {
        int dial_result = ts3ds_dial_tcp(tailscale, host,
                                         (uint16_t)port, tsconn_out);
        if (dial_result != TS3DS_OK) {
            snprintf(err_buf, err_sz,
                     "Tailscale TCP dial failed (%d): %.180s",
                     dial_result, ts3ds_last_error(tailscale));
            libssh2_exit();
            return NULL;
        }
        *transport_out = calloc(1, sizeof(ssh_transport));
        if (!*transport_out) {
            copy_err(err_buf, err_sz, "out of memory");
            ts3ds_conn_close(*tsconn_out);
            *tsconn_out = NULL;
            libssh2_exit();
            return NULL;
        }
        (*transport_out)->tailscale = tailscale;
        (*transport_out)->connection = *tsconn_out;
        (*transport_out)->blocking = 1;
    } else {
        *sock_out = tcp_connect(host, port, err_buf, err_sz);
        if (*sock_out < 0) {
            libssh2_exit();
            return NULL;
        }
    }

    LIBSSH2_SESSION *session = libssh2_session_init_ex(NULL, NULL, NULL,
                                                       *transport_out);
    if (!session) {
        copy_err(err_buf, err_sz, "session_init failed");
        close_pending_transport(*sock_out, *tsconn_out, *transport_out);
        libssh2_exit();
        return NULL;
    }
    if (*transport_out) {
        libssh2_session_callback_set(session, LIBSSH2_CALLBACK_SEND,
                                     (void *)tailscale_send_cb);
        libssh2_session_callback_set(session, LIBSSH2_CALLBACK_RECV,
                                     (void *)tailscale_recv_cb);
    }
    libssh2_session_set_blocking(session, 1);

    int hs_rc = libssh2_session_handshake(session, *transport_out ? 0 : *sock_out);
    if (hs_rc != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "handshake", hs_rc);
        libssh2_session_free(session);
        close_pending_transport(*sock_out, *tsconn_out, *transport_out);
        libssh2_exit();
        return NULL;
    }
    return session;
}

/* Tear down everything finish_shell may have created, in the right
 * order.  Shared by all of its failure branches. */
static void finish_shell_fail(LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                              const char *reason, int sock,
                              ts3ds_conn *tailscale_conn,
                              ssh_transport *transport,
                              char *err_buf, int err_sz) {
    if (channel) {
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
    libssh2_session_disconnect(session, reason);
    libssh2_session_free(session);
    close_pending_transport(sock, tailscale_conn, transport);
    libssh2_exit();
}

/* Post-auth: open the session channel, PTY + shell, flip back to
 * non-blocking, enable keepalives, allocate the client handle.  On failure
 * tears down session + transport + libssh2 and returns NULL. */
static ssh_client_t *finish_shell(LIBSSH2_SESSION *session, int sock,
                                  ts3ds_conn *tailscale_conn,
                                  ssh_transport *transport,
                                  int pty_cols, int pty_rows,
                                  char *err_buf, int err_sz) {
    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel) {
        copy_libssh2_err(err_buf, err_sz, session, "channel_open", -1);
        finish_shell_fail(session, channel, "channel failed", sock,
                          tailscale_conn, transport, err_buf, err_sz);
        return NULL;
    }

    libssh2_channel_setenv(channel, "COLORTERM", "truecolor");
    if (pty_cols <= 0) pty_cols = 80;
    if (pty_rows <= 0) pty_rows = 24;
    int pty_rc = libssh2_channel_request_pty_ex(
        channel, "xterm-256color", sizeof("xterm-256color") - 1,
        NULL, 0, pty_cols, pty_rows, 0, 0);
    if (pty_rc != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "pty", pty_rc);
        finish_shell_fail(session, channel, "pty failed", sock,
                          tailscale_conn, transport, err_buf, err_sz);
        return NULL;
    }

    int sh_rc = libssh2_channel_shell(channel);
    if (sh_rc != 0) {
        copy_libssh2_err(err_buf, err_sz, session, "shell", sh_rc);
        finish_shell_fail(session, channel, "shell failed", sock,
                          tailscale_conn, transport, err_buf, err_sz);
        return NULL;
    }

    libssh2_session_set_blocking(session, 0);
    if (transport) transport->blocking = 0;

    /* Enable keepalive so we can tell idle-connection from broken-network.
     * want_reply=1 makes the server SSH_MSG_GLOBAL_REQUEST/keepalive
     * round-trip so any successful round produces ssh_read traffic
     * — main.c's stall detector watches for that. */
    libssh2_keepalive_config(session, 1, 10);

    ssh_client_t *ssh = calloc(1, sizeof(*ssh));
    if (!ssh) {
        copy_err(err_buf, err_sz, "out of memory");
        finish_shell_fail(session, channel, "oom", sock,
                          tailscale_conn, transport, err_buf, err_sz);
        return NULL;
    }

    ssh->sock      = sock;
    ssh->tailscale_conn = tailscale_conn;
    ssh->transport = transport;
    ssh->session   = session;
    ssh->channel   = channel;
    ssh->connected = 1;
    return ssh;
}

/* Tear down a session whose post-handshake pre-auth checks failed. */
static void abort_session(LIBSSH2_SESSION *session, const char *reason,
                          int sock, ts3ds_conn *tailscale_conn,
                          ssh_transport *transport) {
    libssh2_session_disconnect(session, reason);
    libssh2_session_free(session);
    close_pending_transport(sock, tailscale_conn, transport);
    libssh2_exit();
}

/* Query the auth methods the server allows for this user.  Returns the
 * libssh2-allocated methods string (do NOT free — libssh2 owns it) or
 * NULL; the string stays valid until the next libssh2 call, which is all
 * the connect paths need it for. */
static const char *query_auth_methods(LIBSSH2_SESSION *session,
                                      const char *user) {
    return libssh2_userauth_list(session, user,
                                 (unsigned int)strlen(user));
}

ssh_client_t *ssh_connect_pubkey(const char *host, int port,
                                 const char *user,
                                 const char *key_path,
                                 const char *pubkey_path,
                                 const char *passphrase,
                                 int pty_cols, int pty_rows,
                                 ts3ds *tailscale,
                                 char *err_buf, int err_sz) {
    int sock;
    ts3ds_conn *tailscale_conn;
    ssh_transport *transport;

    LIBSSH2_SESSION *session = dial_session(host, port, tailscale,
                                            &sock, &tailscale_conn,
                                            &transport, err_buf, err_sz);
    if (!session) return NULL;

    /* Pre-check 1: can we open the key file at all? Capture size + header. */
    long key_size = 0;
    {
        FILE *kf = fopen(key_path, "rb");
        if (!kf) {
            snprintf(err_buf, err_sz,
                     "open key file failed: %s (errno=%d)", key_path, errno);
            abort_session(session, "no key", sock, tailscale_conn, transport);
            return NULL;
        }
        fseek(kf, 0, SEEK_END);
        key_size = ftell(kf);
        fseek(kf, 0, SEEK_SET);
        char first[40] = {0};
        size_t n = fread(first, 1, sizeof(first) - 1, kf);
        fclose(kf);
        first[n] = 0;
        if (!strstr(first, "BEGIN") ||
            (!strstr(first, "RSA PRIVATE KEY") &&
             !strstr(first, "PRIVATE KEY"))) {
            snprintf(err_buf, err_sz,
                     "key not PEM (size=%ld). Head: %.30s", key_size, first);
            abort_session(session, "bad key", sock, tailscale_conn, transport);
            return NULL;
        }
    }

    /* Pre-check 2: ask server which auth methods it accepts for this user.
     * Always include the methods string in any later auth error for context. */
    const char *methods = query_auth_methods(session, user);
    if (methods && !strstr(methods, "publickey")) {
        snprintf(err_buf, err_sz,
                 "server rejects publickey for %s. Allowed: %s",
                 user, methods);
        abort_session(session, "no pubkey method",
                      sock, tailscale_conn, transport);
        return NULL;
    }

    /* Pre-check 3: parse key directly with mbedTLS so we get its specific
     * error code (libssh2's mbedTLS backend swallows mbedTLS errors and just
     * returns -1). This is purely diagnostic — we then let libssh2 do its
     * own parse during userauth_publickey_fromfile_ex. */
    {
        mbedtls_pk_context tctx;
        mbedtls_pk_init(&tctx);
        int mb_rc = mbedtls_pk_parse_keyfile(
            &tctx, key_path,
            (passphrase && *passphrase) ? passphrase : NULL);
        if (mb_rc != 0) {
            char mb_msg[80] = {0};
            mbedtls_strerror(mb_rc, mb_msg, sizeof(mb_msg));
            snprintf(err_buf, err_sz,
                     "mbedTLS parse: rc=-0x%04X %s | path=%s",
                     (unsigned)-mb_rc, mb_msg, key_path);
            mbedtls_pk_free(&tctx);
            abort_session(session, "mbedtls parse",
                          sock, tailscale_conn, transport);
            return NULL;
        }
        /* Stash success info into err_buf as a side-channel; if any later
         * step fails, the caller will still see the type/bits we saw. */
        snprintf(err_buf, err_sz, "(parse OK: %s %u-bit) ",
                 mbedtls_pk_get_name(&tctx),
                 (unsigned)mbedtls_pk_get_bitlen(&tctx));
        mbedtls_pk_free(&tctx);
    }

    /* RSA pubkey from file. pubkey_path may be NULL — libssh2 derives it. */
    int auth = libssh2_userauth_publickey_fromfile_ex(
        session, user, (unsigned int)strlen(user),
        pubkey_path, key_path,
        passphrase ? passphrase : "");
    if (auth != 0) {
        char inner[160] = {0};
        copy_libssh2_err(inner, sizeof(inner), session, "auth", auth);
        /* Note: err_buf currently holds "(parse OK: RSA 4096-bit) " prefix
         * from the mbedTLS pre-check success path; preserve it so we know
         * mbedTLS itself parsed the key fine and the issue is downstream. */
        char prefix[64] = {0};
        snprintf(prefix, sizeof(prefix), "%.63s", err_buf);
        snprintf(err_buf, err_sz,
                 "%s%s | key_size=%ld user=%s methods=[%s]",
                 prefix, inner, key_size, user, methods ? methods : "?");
        abort_session(session, "auth failed",
                      sock, tailscale_conn, transport);
        return NULL;
    }
    /* Auth succeeded — clear the diagnostic prefix from err_buf. */
    err_buf[0] = 0;

    return finish_shell(session, sock, tailscale_conn, transport,
                        pty_cols, pty_rows, err_buf, err_sz);
}

ssh_client_t *ssh_connect_password(const char *host, int port,
                                   const char *user,
                                   const char *password,
                                   int pty_cols, int pty_rows,
                                   ts3ds *tailscale,
                                   char *err_buf, int err_sz) {
    int sock;
    ts3ds_conn *tailscale_conn;
    ssh_transport *transport;

    LIBSSH2_SESSION *session = dial_session(host, port, tailscale,
                                            &sock, &tailscale_conn,
                                            &transport, err_buf, err_sz);
    if (!session) return NULL;

    const char *methods = query_auth_methods(session, user);
    if (methods && !strstr(methods, "password")) {
        snprintf(err_buf, err_sz,
                 "server rejects password auth for %s. Allowed: %s",
                 user, methods);
        abort_session(session, "no password method",
                      sock, tailscale_conn, transport);
        return NULL;
    }

    int auth = libssh2_userauth_password_ex(
        session, user, (unsigned int)strlen(user),
        password, (unsigned int)strlen(password), NULL);
    if (auth != 0) {
        char inner[160] = {0};
        copy_libssh2_err(inner, sizeof(inner), session, "auth", auth);
        snprintf(err_buf, err_sz,
                 "%s | user=%s methods=[%s] "
                 "(wrong password or server forbids password auth)",
                 inner, user, methods ? methods : "?");
        abort_session(session, "auth failed",
                      sock, tailscale_conn, transport);
        return NULL;
    }
    err_buf[0] = 0;

    return finish_shell(session, sock, tailscale_conn, transport,
                        pty_cols, pty_rows, err_buf, err_sz);
}

void ssh_disconnect(ssh_client_t *ssh) {
    if (!ssh) return;
    if (ssh->channel) {
        libssh2_channel_close(ssh->channel);
        libssh2_channel_free(ssh->channel);
    }
    if (ssh->session) {
        libssh2_session_disconnect(ssh->session, "Bye");
        libssh2_session_free(ssh->session);
    }
    if (ssh->sock >= 0) closesocket(ssh->sock);
    if (ssh->tailscale_conn) ts3ds_conn_close(ssh->tailscale_conn);
    free(ssh->transport);
    libssh2_exit();
    free(ssh);
}

int ssh_is_connected(ssh_client_t *ssh) { return ssh && ssh->connected; }

void ssh_poll_transport(ssh_client_t *ssh) {
    ssh_transport *transport;
    if (!ssh || !ssh->transport) return;
    transport = (ssh_transport *)ssh->transport;
    if (transport->tailscale &&
        ts3ds_get_status(transport->tailscale) == TS3DS_STATUS_ONLINE)
        (void)ts3ds_poll(transport->tailscale);
}

int ssh_read(ssh_client_t *ssh, char *buf, int len) {
    if (!ssh || !ssh->connected) return -1;
    ssize_t n = libssh2_channel_read(ssh->channel, buf, (size_t)len);
    if (n == LIBSSH2_ERROR_EAGAIN) return 0;
    if (n < 0) { ssh->connected = 0; return -1; }
    if (libssh2_channel_eof(ssh->channel)) ssh->connected = 0;
    return (int)n;
}

int ssh_write(ssh_client_t *ssh, const char *buf, int len) {
    if (!ssh || !ssh->connected || len <= 0) return -1;
    int sent = 0;
    while (sent < len) {
        ssize_t n = libssh2_channel_write(ssh->channel,
                                          buf + sent,
                                          (size_t)(len - sent));
        if (n == LIBSSH2_ERROR_EAGAIN) break;
        if (n < 0) { ssh->connected = 0; return -1; }
        sent += (int)n;
    }
    return sent;
}

void ssh_set_pty_size(ssh_client_t *ssh, int cols, int rows) {
    if (!ssh || !ssh->connected || !ssh->channel) return;
    libssh2_channel_request_pty_size(ssh->channel, cols, rows);
}

void ssh_keepalive_tick(ssh_client_t *ssh) {
    if (!ssh || !ssh->connected || !ssh->session) return;
    /* libssh2 internally tracks the configured interval (10s) and
     * only actually emits a packet when it's due — calling every
     * frame is cheap and lets the library own the timing. */
    int unused;
    libssh2_keepalive_send(ssh->session, &unused);
}

/* ── Auxiliary channel implementation ─────────────────────────────── */

struct ssh_aux_channel_t {
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    int              eof_sent;
};

ssh_aux_channel_t *ssh_aux_exec(ssh_client_t *ssh, const char *cmd,
                                char *err_buf, int err_sz) {
    if (!ssh || !ssh->connected || !ssh->session || !cmd) return NULL;

    /* Briefly flip both libssh2 and its optional callback transport to
     * blocking mode so open + exec can complete inline. This stalls the main
     * loop for roughly one RTT, which is invisible beside transcription. */
    ssh_set_io_blocking(ssh, 1);

    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session(ssh->session);
    if (!ch) {
        copy_libssh2_err(err_buf, err_sz, ssh->session, "aux open_session", 0);
        ssh_set_io_blocking(ssh, 0);
        return NULL;
    }
    int rc = libssh2_channel_exec(ch, cmd);
    if (rc != 0) {
        copy_libssh2_err(err_buf, err_sz, ssh->session, "aux exec", rc);
        /* Free while still blocking: in nonblocking mode channel_free can
         * return EAGAIN without freeing, leaking the channel. */
        libssh2_channel_free(ch);
        ssh_set_io_blocking(ssh, 0);
        return NULL;
    }

    ssh_aux_channel_t *a = calloc(1, sizeof(*a));
    if (!a) {
        copy_err(err_buf, err_sz, "aux alloc oom");
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        ssh_set_io_blocking(ssh, 0);
        return NULL;
    }
    ssh_set_io_blocking(ssh, 0);
    a->session = ssh->session;
    a->channel = ch;
    return a;
}

int ssh_aux_write(ssh_aux_channel_t *aux, const char *buf, int len) {
    if (!aux || !aux->channel || len < 0) return -1;
    if (len == 0) return 0;
    ssize_t n = libssh2_channel_write(aux->channel, buf, (size_t)len);
    if (n == LIBSSH2_ERROR_EAGAIN) return 0;
    if (n < 0) return -1;
    return (int)n;
}

int ssh_aux_send_eof(ssh_aux_channel_t *aux) {
    if (!aux || !aux->channel) return -1;
    if (aux->eof_sent) return 0;
    int rc = libssh2_channel_send_eof(aux->channel);
    if (rc == 0) { aux->eof_sent = 1; return 0; }
    if (rc == LIBSSH2_ERROR_EAGAIN) return 1;
    return -1;
}

int ssh_aux_read(ssh_aux_channel_t *aux, char *buf, int len) {
    if (!aux || !aux->channel || len <= 0) return -1;
    ssize_t n = libssh2_channel_read(aux->channel, buf, (size_t)len);
    if (n == LIBSSH2_ERROR_EAGAIN) return 0;
    if (n < 0) return -1;
    return (int)n;
}

int ssh_aux_eof(const ssh_aux_channel_t *aux) {
    if (!aux || !aux->channel) return 1;
    /* libssh2_channel_eof takes a non-const pointer in older libssh2
     * versions; drop const here to match. */
    return libssh2_channel_eof((LIBSSH2_CHANNEL *)aux->channel) ? 1 : 0;
}

void ssh_aux_close(ssh_aux_channel_t *aux) {
    if (!aux) return;
    if (aux->channel) {
        libssh2_channel_close(aux->channel);
        libssh2_channel_free(aux->channel);
    }
    free(aux);
}

#include "voice_api.h"

#include <stdio.h>
#include <string.h>

/* ── WAV framing (host-compilable pure code) ───────────────────────── */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >>  8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

int voice_api_wav_header(uint8_t *buf, int cap, uint32_t pcm_bytes,
                         uint32_t sample_rate) {
    if (!buf || cap < VOICE_API_WAV_HEADER) return -1;

    const uint32_t data_len = pcm_bytes;
    const uint32_t riff_len = 36 + data_len;   /* everything after "RIFF" */
    const uint16_t channels = 1;
    const uint16_t bits     = 16;
    const uint32_t byte_rate = sample_rate * channels * (bits / 8);
    const uint16_t block_align = channels * (bits / 8);

    memcpy(buf +  0, "RIFF", 4);
    put_u32(buf +  4, riff_len);
    memcpy(buf +  8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_u32(buf + 16, 16);                      /* PCM fmt chunk size */
    put_u16(buf + 20, 1);                       /* format = PCM */
    put_u16(buf + 22, channels);
    put_u32(buf + 24, sample_rate);
    put_u32(buf + 28, byte_rate);
    put_u16(buf + 32, block_align);
    put_u16(buf + 34, bits);
    memcpy(buf + 36, "data", 4);
    put_u32(buf + 40, data_len);
    return VOICE_API_WAV_HEADER;
}

#ifdef __3DS__

/* ── libctru httpc transport (3DS only) ────────────────────────────── */

#include <3ds.h>

static void copy_err(char *dst, int dst_sz, const char *msg) {
    if (dst && dst_sz > 0) snprintf(dst, (size_t)dst_sz, "%s", msg);
}

int voice_api_post(const char *url, const uint8_t *body, int body_len,
                   char *resp, int resp_cap, char *err, int err_sz) {
    if (!url || !*url || !body || body_len <= 0 || !resp || resp_cap < 1) {
        copy_err(err, err_sz, "bad args");
        return -1;
    }

    httpcContext ctx;
    Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_POST, (char *)url, 0);
    if (R_FAILED(rc)) {
        snprintf(err, (size_t)err_sz, "open ctx 0x%08lX", (unsigned long)rc);
        return -1;
    }

    /* Self-signed LAN certs (nginx 29xxx) fail default verification —
     * we're on a trusted LAN, so disable the server-cert check. */
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcAddRequestHeaderField(&ctx, "Content-Type", "audio/wav");
    httpcAddRequestHeaderField(&ctx, "User-Agent", "DSSH/1.0 (3DS)");

    /* POST body must be attached before BeginRequest.  httpcAddPostDataRaw
     * wants a word-aligned buffer — malloc/memalign satisfy that. */
    int ret = -1;
    rc = httpcAddPostDataRaw(&ctx, (const u32 *)body, (u32)body_len);
    if (R_FAILED(rc)) {
        snprintf(err, (size_t)err_sz, "post data 0x%08lX",
                 (unsigned long)rc);
        goto out;
    }

    rc = httpcBeginRequest(&ctx);
    if (R_FAILED(rc)) {
        snprintf(err, (size_t)err_sz, "begin 0x%08lX", (unsigned long)rc);
        goto out;
    }

    {
        u32 status = 0;
        rc = httpcGetResponseStatusCode(&ctx, &status);
        if (R_FAILED(rc)) {
            snprintf(err, (size_t)err_sz, "status 0x%08lX",
                     (unsigned long)rc);
            goto out;
        }
        if (status != 200) {
            snprintf(err, (size_t)err_sz, "http %lu", (unsigned long)status);
            goto out;
        }
    }

    {
        u32 pos = 0;
        while (pos < (u32)(resp_cap - 1)) {
            u32 got = 0;
            rc = httpcDownloadData(&ctx, (u8 *)resp + pos,
                                   (u32)(resp_cap - 1 - pos), &got);
            pos += got;
            /* A short body relative to our request is fine — libctru
             * reports it as an error but still fills `got`. */
            if (R_FAILED(rc) || got == 0) break;
        }
        resp[pos] = 0;
        copy_err(err, err_sz, "ok");
        ret = (int)pos;
    }

out:
    httpcCloseContext(&ctx);
    return ret;
}

#endif /* __3DS__ */

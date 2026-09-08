#ifndef VOICE_API_H
#define VOICE_API_H

/*
 * HTTP voice-transcription transport (the "voice API" path).
 *
 * When the user configures voice_api_url in SETTINGS, plain (non-AI)
 * voice recordings are POSTed as a 16 kHz PCM16 mono WAV blob to that
 * URL instead of being streamed over the SSH aux channel.  The server
 * answers {"text": "..."}; the text replaces the raw JSON reply.
 *
 * Pure helpers (WAV framing) are host-compilable so tools/test_voice_api.c
 * can verify the header bytes; the httpc transport only exists on 3DS.
 */

#include <stdint.h>

/* WAV/RIFF header size — everything before the PCM payload. */
#define VOICE_API_WAV_HEADER 44

/* Total upload size for `pcm_bytes` of payload. */
#define VOICE_API_WAV_TOTAL(pcm) (VOICE_API_WAV_HEADER + (uint32_t)(pcm))

/* Write a canonical 44-byte RIFF/WAVE header describing `pcm_bytes` of
 * PCM16 mono samples at `sample_rate` into buf[0..cap-1].  Returns the
 * number of bytes written (44), or -1 if cap is too small. */
int voice_api_wav_header(uint8_t *buf, int cap, uint32_t pcm_bytes,
                         uint32_t sample_rate);

#ifdef __3DS__

/* Blocking POST `body_len` bytes of `body` to `url` (http or https; https
 * skips server-certificate verification so self-signed LAN nginx works).
 * The response body (up to resp_cap-1 bytes) is NUL-terminated into resp.
 * Returns the number of response bytes on success (>=0), -1 on failure
 * with a short reason in err.  Intended to run on a worker thread; the
 * caller keeps pumping its own loop. */
int voice_api_post(const char *url, const uint8_t *body, int body_len,
                   char *resp, int resp_cap, char *err, int err_sz);

#endif /* __3DS__ */

#endif /* VOICE_API_H */

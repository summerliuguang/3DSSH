#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "../source/voice_api.h"

static int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int main(void) {
    uint8_t buf[VOICE_API_WAV_HEADER + 8];
    memset(buf, 0xAA, sizeof(buf));

    /* Too small a buffer must be rejected, not overwritten. */
    CHECK(voice_api_wav_header(buf, 10, 1000, 16360) == -1,
          " undersized buffer should fail");
    CHECK(buf[0] == 0xAA, "undersized call must not write");

    const uint32_t pcm = 1024u * 1024u;   /* 1 MB, ~32 s at 16 kHz */
    int n = voice_api_wav_header(buf, (int)sizeof(buf), pcm, 16360);
    CHECK(n == VOICE_API_WAV_HEADER, "header returns 44");

    CHECK(memcmp(buf, "RIFF", 4) == 0, "RIFF magic");
    CHECK(memcmp(buf + 8, "WAVE", 4) == 0, "WAVE magic");
    CHECK(memcmp(buf + 12, "fmt ", 4) == 0, "fmt chunk id");
    CHECK(memcmp(buf + 36, "data", 4) == 0, "data chunk id");

    CHECK(rd32(buf + 4) == 36 + pcm, "riff size = 36 + data");
    CHECK(rd32(buf + 16) == 16, "fmt chunk size");
    CHECK(rd16(buf + 20) == 1, "PCM format");
    CHECK(rd16(buf + 22) == 1, "mono");
    CHECK(rd32(buf + 24) == 16360, "sample rate (MICU_SAMPLE_RATE_16360)");
    CHECK(rd32(buf + 28) == 16360u * 2u, "byte rate = rate * ch * 2");
    CHECK(rd16(buf + 32) == 2, "block align");
    CHECK(rd16(buf + 34) == 16, "bits per sample");
    CHECK(rd32(buf + 40) == pcm, "data chunk size");

    /* Total upload size formula used by voice.c. */
    CHECK(VOICE_API_WAV_TOTAL(pcm) == 44 + pcm, "total = 44 + pcm");

    /* The WAV must be loadable by standard tooling — ffprobe round-trip
     * when available (skips silently if not installed). */
    if (system("command -v ffprobe >/dev/null 2>&1") == 0) {
        const uint32_t tiny = 4;
        n = voice_api_wav_header(buf, (int)sizeof(buf), tiny, 16360);
        CHECK(n == VOICE_API_WAV_HEADER, "tiny header regen");
        FILE *fp = fopen("/tmp/dssh-voice-api-test.wav", "wb");
        CHECK(fp != NULL, "temp wav open");
        if (fp) {
            fwrite(buf, 1, (size_t)n, fp);
            const uint8_t payload[4] = {0x00, 0x00, 0x00, 0x00};
            fwrite(payload, 1, sizeof(payload), fp);
            fclose(fp);
            int rc = system(
                "ffprobe -v error -show_entries stream=codec_name,sample_rate,"
                "channels -of csv=p=0 /tmp/dssh-voice-api-test.wav"
                " | grep -q '^pcm_s16le,16360,1$'");
            CHECK(rc == 0, "ffprobe parses generated WAV header");
            unlink("/tmp/dssh-voice-api-test.wav");
        }
    }

    if (failures) {
        fprintf(stderr, "%d voice_api test(s) failed\n", failures);
        return 1;
    }
    puts("voice_api tests passed");
    return 0;
}

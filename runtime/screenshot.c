// F12 screenshots: the presented frame (without the menu or the performance overlay) as a compressed PNG in
// screenshots/ next to the exe. The present thread only copies the pixels; a worker thread encodes and writes them.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "platform/platform.h"
#include "screenshot.h"
#include "ui/menu.h"
#include "ui_text.h"

static volatile int32_t requested;

void screenshot_request(void) { plat_xchg32(&requested, 1); }
int screenshot_claim(void) { return plat_xchg32(&requested, 0) != 0; }

// ---- deflate: one fixed-Huffman block, LZ77 with a single-probe hash (fast, about 2x worse than zlib's default)
typedef struct { uint8_t *p; size_t n; uint64_t bits; int nb; } Bits;

static void put(Bits *b, uint32_t v, int n) {   // n <= 32, LSB first
    b->bits |= (uint64_t)v << b->nb;
    b->nb += n;
    while (b->nb >= 8) {
        b->p[b->n++] = (uint8_t)b->bits;
        b->bits >>= 8;
        b->nb -= 8;
    }
}

static uint32_t rev(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r = r << 1 | (v >> i & 1);
    return r;
}

static void put_sym(Bits *b, int s) {   // a literal/length symbol in the fixed code
    if (s < 144) put(b, rev(0x30 + s, 8), 8);
    else if (s < 256) put(b, rev(0x190 + s - 144, 9), 9);
    else if (s < 280) put(b, rev(s - 256, 7), 7);
    else put(b, rev(0xC0 + s - 280, 8), 8);
}

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                      115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                       12, 12, 13, 13};

static void put_match(Bits *b, int len, int dist) {
    int i = 28;
    while (len_base[i] > len) i--;
    put_sym(b, 257 + i);
    if (len_extra[i]) put(b, (uint32_t)(len - len_base[i]), len_extra[i]);
    int d = 29;
    while (dist_base[d] > dist) d--;
    put(b, rev((uint32_t)d, 5), 5);
    if (dist_extra[d]) put(b, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

// zlib stream of in[0..n) into a new buffer; *out_n its size. NULL when out of memory.
static uint8_t *zlib_compress(const uint8_t *in, size_t n, size_t *out_n) {
    enum { HBITS = 16, WIN = 32768, MAXLEN = 258 };
    Bits b = {malloc(n + n / 8 + 64), 0, 0, 0};
    int32_t *head = malloc(sizeof(int32_t) << HBITS);
    if (!b.p || !head) {
        free(b.p);
        free(head);
        return NULL;
    }
    memset(head, 0xff, sizeof(int32_t) << HBITS);
    b.p[b.n++] = 0x78, b.p[b.n++] = 0x01;
    put(&b, 1, 1);   // BFINAL
    put(&b, 1, 2);   // fixed Huffman
    size_t i = 0;
    while (i < n) {
        int best = 0;
        size_t from = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t)in[i] << 16 | in[i + 1] << 8 | in[i + 2]) * 2654435761u >> (32 - HBITS);
            int32_t c = head[h];
            head[h] = (int32_t)i;
            if (c >= 0 && i - (size_t)c <= WIN) {
                size_t lim = n - i < MAXLEN ? n - i : MAXLEN;
                size_t l = 0;
                while (l < lim && in[c + l] == in[i + l]) l++;
                if (l >= 3) best = (int)l, from = (size_t)c;
            }
        }
        if (best) {
            put_match(&b, best, (int)(i - from));
            i += best;
        } else {
            put_sym(&b, in[i++]);
        }
    }
    put_sym(&b, 256);
    if (b.nb) put(&b, 0, 8 - b.nb);
    uint32_t a = 1, s = 0;
    for (size_t k = 0; k < n;) {   // adler32 in runs short enough not to overflow
        size_t end = n - k > 5552 ? k + 5552 : n;
        for (; k < end; k++) a += in[k], s += a;
        a %= 65521, s %= 65521;
    }
    uint32_t ad = s << 16 | a;
    b.p[b.n++] = (uint8_t)(ad >> 24), b.p[b.n++] = (uint8_t)(ad >> 16), b.p[b.n++] = (uint8_t)(ad >> 8),
    b.p[b.n++] = (uint8_t)ad;
    free(head);
    *out_n = b.n;
    return b.p;
}

// ---- PNG (RGB, each row with the filter of least absolute sum)
static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hdr[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc32_of(crc32_of(~0u, hdr + 4, 4), data, n) ^ ~0u;
    uint8_t tail[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    fwrite(tail, 1, 4, f);
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static int write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    size_t stride = 3ull * w, row = stride + 1;
    uint8_t *raw = malloc(row * h), *cand = malloc(4 * stride);
    if (!raw || !cand) {
        free(raw);
        free(cand);
        return 0;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *cur = rgb + stride * y, *up = y ? cur - stride : NULL;
        uint64_t best_sum = UINT64_MAX;
        int best = 0;
        for (int f = 0; f < 4; f++) {   // none, sub, up, paeth
            uint8_t *o = cand + stride * f;
            uint64_t sum = 0;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= 3 ? cur[x - 3] : 0, b = up ? up[x] : 0, c = up && x >= 3 ? up[x - 3] : 0;
                int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : paeth(a, b, c);
                o[x] = (uint8_t)(cur[x] - pred);
                sum += o[x] < 128 ? o[x] : 256 - o[x];
            }
            if (sum < best_sum) best_sum = sum, best = f;
        }
        raw[row * y] = (uint8_t)(best == 3 ? 4 : best);
        memcpy(raw + row * y + 1, cand + stride * best, stride);
    }
    free(cand);
    size_t zn = 0;
    uint8_t *z = zlib_compress(raw, row * h, &zn);
    free(raw);
    if (!z) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return 0;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) == 0;
}

// ---- the worker
typedef struct { uint8_t *rgb; uint32_t w, h; char path[2200]; } Job;

static void job_run(void *arg) {
    Job *j = arg;
    uint64_t t0 = plat_now_ns();
    int ok = write_png(j->path, j->rgb, j->w, j->h);
    if (ok) {
        rt_log("screenshot: %s (%ux%u, %.0f ms)", j->path, j->w, j->h, (plat_now_ns() - t0) / 1e6);
        menu_toast(menu_text(UI_MENU_SCREENSHOT_SAVED));
    }
    else rt_log("screenshot: cannot write %s", j->path);
    free(j->rgb);
    free(j);
}

// The folder: screenshots/ next to the exe. 0 when the exe's path is unknown.
static int shots_dir(char *out, size_t cap) {
    char exe[2048];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *cut = strrchr(exe, '/'), *back = strrchr(exe, '\\');
    if (back > cut) cut = back;
    if (!cut) return 0;
    *cut = 0;
    return snprintf(out, cap, "%s/screenshots", exe) < (int)cap;
}

void screenshot_take(const uint8_t *px, int w, int h, int bgra) {
    Job *j = calloc(1, sizeof *j);
    char dir[2100];
    if (!j || !shots_dir(dir, sizeof dir)) {
        free(j);
        return;
    }
    j->rgb = malloc(3ull * w * h);
    if (!j->rgb) {
        free(j);
        return;
    }
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        j->rgb[3 * i] = px[4 * i + (bgra ? 2 : 0)];
        j->rgb[3 * i + 1] = px[4 * i + 1];
        j->rgb[3 * i + 2] = px[4 * i + (bgra ? 0 : 2)];
    }
    j->w = (uint32_t)w, j->h = (uint32_t)h;
    plat_file_mkdir_p(dir);
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    static int seq;   // two shots in one second
    snprintf(j->path, sizeof j->path, "%s/Engage_%04d%02d%02d_%02d%02d%02d_%d.png", dir, tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, seq++ % 100);
    if (!plat_thread_spawn(job_run, j, "screenshot", 0)) job_run(j);
}

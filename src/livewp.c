/*
 * livewp.c - a live wallpaper: a short clip of 320x480 RGB frames that
 * loops, made by the plugin from a video.
 *
 * wallpaper.live: a 16-byte header ("DVLW", version 1, fps, width, height,
 * tile side, frame count; little endian), then each frame as a u32 length
 * and a zlib stream. A frame inflates to a kind byte and then either
 *   0 (key): every pixel, R G B, rows top down; or
 *   1 (delta): a bitmap of the 16x16 tiles that changed (raster order, bit
 *      i % 8 of byte i / 8), then for each such tile its 16 rows of 16
 *      pixels as per-channel differences from the frame before (mod 256).
 * The first frame is a key frame; after the last one the clip starts over
 * from it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "livewp.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void livewp_close(livewp_t *lw)
{
    if (lw->map) munmap((void *)lw->map, lw->size);
    free(lw->off);
    free(lw->len);
    memset(lw, 0, sizeof *lw);
}

int livewp_open(livewp_t *lw, const char *path)
{
    memset(lw, 0, sizeof *lw);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);   /* the mapping keeps the file, even once the plugin replaces it */
    if (m == MAP_FAILED) return -1;
    lw->map = m;
    lw->size = (size_t)st.st_size;
    const uint8_t *h = lw->map;
    int n = (int)rd32(h + 12);
    if (memcmp(h, "DVLW", 4) || h[4] != 1 || h[5] < 1 || h[5] > LIVEWP_FPS_MAX ||
        rd16(h + 6) != LIVEWP_W || rd16(h + 8) != LIVEWP_H || rd16(h + 10) != LIVEWP_TILE ||
        n < 1 || n > LIVEWP_FRAMES_MAX) {
        livewp_close(lw);
        return -1;
    }
    lw->fps = h[5];
    lw->off = malloc((size_t)n * sizeof *lw->off);
    lw->len = malloc((size_t)n * sizeof *lw->len);
    if (!lw->off || !lw->len) { livewp_close(lw); return -1; }
    size_t pos = 16;
    for (int i = 0; i < n; i++) {
        if (lw->size - pos < 4) { livewp_close(lw); return -1; }
        uint32_t len = rd32(lw->map + pos);
        pos += 4;
        if (len == 0 || len > lw->size - pos) { livewp_close(lw); return -1; }
        lw->off[i] = pos;
        lw->len[i] = len;
        pos += len;
    }
    lw->n = n;
    return 0;
}

int livewp_apply(const livewp_t *lw, int i, uint8_t *rgb, uint8_t *dirty, uint8_t *scratch)
{
    enum { TX = LIVEWP_W / LIVEWP_TILE, TY = LIVEWP_H / LIVEWP_TILE, ROW = LIVEWP_TILE * 3 };
    if (i < 0 || i >= lw->n || lw->len[i] > 0x7fffffffu) return -1;
    int got = stbi_zlib_decode_buffer((char *)scratch, LIVEWP_PAYLOAD_MAX, (const char *)lw->map + lw->off[i], (int)lw->len[i]);
    if (got < 1) return -1;
    if (scratch[0] == 0) {
        if (got != 1 + LIVEWP_W * LIVEWP_H * 3) return -1;
        memcpy(rgb, scratch + 1, (size_t)LIVEWP_W * LIVEWP_H * 3);
        memset(dirty, 1, TX * TY);
        return 0;
    }
    if (scratch[0] != 1 || got < 1 + (TX * TY + 7) / 8) return -1;
    const uint8_t *bits = scratch + 1, *p = bits + (TX * TY + 7) / 8;
    int tiles = 0;
    for (int t = 0; t < TX * TY; t++) tiles += bits[t >> 3] >> (t & 7) & 1;
    if (got != (int)(p - scratch) + tiles * LIVEWP_TILE * ROW) return -1;
    for (int t = 0; t < TX * TY; t++) {
        if (!(bits[t >> 3] >> (t & 7) & 1)) continue;
        dirty[t] = 1;
        uint8_t *o = rgb + ((size_t)(t / TX) * LIVEWP_TILE * LIVEWP_W + (size_t)(t % TX) * LIVEWP_TILE) * 3;
        for (int y = 0; y < LIVEWP_TILE; y++, o += LIVEWP_W * 3)
            for (int k = 0; k < ROW; k++) o[k] = (uint8_t)(o[k] + *p++);
    }
    return 0;
}

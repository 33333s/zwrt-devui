/*
 * livewp.h - reads a live wallpaper clip (see livewp.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LIVEWP_H_INCLUDED
#define LIVEWP_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LIVEWP_W = 320, LIVEWP_H = 480, LIVEWP_TILE = 16,
    LIVEWP_FPS_MAX = 30, LIVEWP_FRAMES_MAX = 30 * 30,
    LIVEWP_TILES = (LIVEWP_W / LIVEWP_TILE) * (LIVEWP_H / LIVEWP_TILE),
    LIVEWP_PAYLOAD_MAX = 1 + LIVEWP_W * LIVEWP_H * 3 + (LIVEWP_TILES + 7) / 8,
};

typedef struct {
    const uint8_t *map;   /* the file, mapped */
    size_t size;
    size_t *off;          /* each frame's zlib stream */
    uint32_t *len;
    int n, fps;
} livewp_t;

/* 0 when path is a well-formed clip; lw then holds it until livewp_close. */
int  livewp_open(livewp_t *lw, const char *path);
void livewp_close(livewp_t *lw);

/* Bring rgb (LIVEWP_W x LIVEWP_H, R G B) from frame i - 1 (or anything, for
 * a key frame) to frame i, and set dirty[t] for each tile t that changed.
 * scratch: LIVEWP_PAYLOAD_MAX bytes. Returns 0, or -1 for a damaged frame
 * (rgb may then be half updated). */
int  livewp_apply(const livewp_t *lw, int i, uint8_t *rgb, uint8_t *dirty, uint8_t *scratch);

#ifdef __cplusplus
}
#endif

#endif

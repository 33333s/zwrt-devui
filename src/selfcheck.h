/*
 * selfcheck.h - notices when the UI stops, and says where (see selfcheck.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SELFCHECK_H
#define SELFCHECK_H

#include <stdint.h>

/* Exit status after a hang, which start.sh takes as "run me again". */
#define SELFCHECK_EXIT_HUNG 70

/* Log to path; exit when the main loop has not come round for limit_ms. Also
 * notes fatal signals in the log before they take the process down. */
void selfcheck_start(const char *path, uint32_t limit_ms);

/* The main loop came round. */
void selfcheck_beat(void);

/* What the main loop is doing now (a string that stays valid). */
void selfcheck_stage(const char *what);

#endif

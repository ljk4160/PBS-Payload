// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdio.h>

#include "common/process/inject.h"

/* Create a private RPC worker inside SceRemotePlay, then detach ptrace.
 * The bootstrap resolves exports by name and allocates its own mapping; it
 * does not use firmware offsets or overwrite executable module padding. */
int pbs_ps5_worker_boot(pbs_inject_t *ctx, FILE *log);

/* Stop and join the worker, release its private mapping and remove the RPC
 * hooks. The only target stop after boot is this bounded final cleanup. */
int pbs_ps5_worker_shutdown(FILE *log);

/* Address of the worker-reserved 2MB frame buffer inside SceRemotePlay, or 0
 * when the reservation failed at boot (caller falls back to malloc). */
intptr_t pbs_ps5_worker_frame_buffer(void);

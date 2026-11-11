// SPDX-License-Identifier: GPL-3.0-or-later
/* PBS - remote process calls.
 * Uses a ptrace technique analogous to prospero-menu (libNineS / John Tornblom,
 * GPL-3.0-or-later) and MemDBG: temporarily elevate caller credentials, stop
 * the target, invoke a function with SysV arguments, then restore registers
 * and detach. PS4 preserves x87/SSE/AVX inside a temporary trampoline in
 * validated Avcap padding; it does not use FP ptrace or change authid/caps.
 * Scratch memory is reserved below the stopped thread stack. */
#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

struct pbs_ps4_firmware;

typedef struct {
  const struct pbs_ps4_firmware *firmware; /* PS4 private ABI, NULL on PS5. */
  pid_t pid;
  pid_t lwp;           /* Stopped thread used for register operations. */
  intptr_t scratch;    /* Writable target memory below the stack. */
  size_t scratch_len;
  int attached;
  int worker;          /* Detached context served by the socket worker. */
  int running;         /* PT_CONTINUE resumed the target after a call. */
  int asserted;        /* The last call triggered an assertion. */
  int nudges;          /* SIGCONT recoveries after a stop was not resumed. */
  intptr_t kbase;      /* libkernel mapping. A thread stopped inside it is
                        * usually parked in a syscall; resuming it rewinds the
                        * PC (ERESTART) and drops an injected call. */
  intptr_t kend;
  int lwp_kernel;      /* Selected LWP was inside the libkernel range. */
  intptr_t lwp_rip;    /* RIP of the selected LWP after the last stop. */
  intptr_t trap;       /* Fixed return-trap address in mapped executable
                        * memory; 0 keeps the current-RIP breakpoint trick. */
  const char *call_stage; /* Last fast-call stage, for failure attribution. */
  int call_errno;
} pbs_inject_t;

#ifdef PBS_PS4
#include "platform/ps4/system/firmware.h"
#endif

/* Return value for a remote call that did not complete. */
#define PBS_CALL_TIMEOUT LONG_MIN

/* Find a process by name. */
pid_t pbs_find_pid(const char *name);
int pbs_find_all_pids(const char *name, pid_t *out, int max);
#ifdef PBS_PS4
/* Read-only process birth epoch from kinfo_proc. Returns 0 if unavailable.
 * Used to bind global RemotePlay capture ownership to a specific process
 * instance, not merely a PID that the kernel may reuse. */
uint64_t pbs_process_birth_epoch(pid_t pid);
#endif

/* Resolve a module export by symbol NID in the target process. */
intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol);

/* Attach and prepare scratch memory; return zero on success. */
int pbs_inject_begin(const char *proc_name, pbs_inject_t *ctx);
/* Same, but prefer an already-validated target page if it is still safely
 * below the new stack pointer. Used by the capture backend to reuse the page
 * validated during the control phase while taking a fresh PT_ATTACH. */
int pbs_inject_begin_preferred(const char *proc_name, pbs_inject_t *ctx,
                                intptr_t preferred_page);
/* Make sure the traced target is stopped and that the scratch page is still
 * safely below its current RSP. Used after control-phase calls before reusing
 * the same attachment for capture. */
int pbs_inject_ensure_stopped(pbs_inject_t *ctx);
int pbs_inject_is_stopped(pbs_inject_t *ctx);
int pbs_inject_revalidate_scratch(pbs_inject_t *ctx);
/* Resume the target between capture reads so its own threads keep running. */
int pbs_inject_resume(pbs_inject_t *ctx);
/* Read the stopped thread's current stack pointer. */
int pbs_inject_target_rsp(pbs_inject_t *ctx, intptr_t *rsp);

/* Call a function with up to six arguments; return LONG_MIN on timeout. */
long pbs_inject_call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]);
/* Run to a return breakpoint instead of single-stepping. */
long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]);
/* Fast call with XMM0 set to the first float argument. */
long pbs_inject_call_fast_fp(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6], float xmm0);

/* Copy from or to target scratch memory. */
int pbs_inject_read(pbs_inject_t *ctx, void *out, size_t len);
int pbs_inject_write(pbs_inject_t *ctx, const void *in, size_t len);

/* Read from an arbitrary target address. */
int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t addr, void *out, size_t len);

/* Write to an arbitrary target address. */
int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t addr, const void *in, size_t len);
/* When installed, every injected call and remote memory access is served by
 * the dedicated worker thread inside the target instead of ptrace, so the
 * target keeps running. Hooks must not call back into pbs_inject_*. */
typedef long (*pbs_worker_call_hook)(intptr_t fn, const uint64_t args[6]);
typedef int (*pbs_worker_read_hook)(intptr_t addr, void *out, size_t len);
typedef int (*pbs_worker_write_hook)(intptr_t addr, const void *in, size_t len);
void pbs_inject_set_worker_hooks(pbs_worker_call_hook call,
                                 pbs_worker_read_hook read,
                                 pbs_worker_write_hook write);

/* Detach; safe to call after partial initialization. */
void pbs_inject_end(pbs_inject_t *ctx);

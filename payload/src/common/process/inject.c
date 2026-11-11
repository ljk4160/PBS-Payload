// SPDX-License-Identifier: GPL-3.0-or-later
/* PBS - PS5 Broadcast Stream: remote calls inside another process. */

#include "common/process/inject.h"
#include "platform/ps4/system/credentials.h"

static pbs_worker_call_hook worker_call_hook;
static pbs_worker_read_hook worker_read_hook;
static pbs_worker_write_hook worker_write_hook;

void pbs_inject_set_worker_hooks(pbs_worker_call_hook call,
                                 pbs_worker_read_hook read,
                                 pbs_worker_write_hook write) {
  worker_call_hook = call;
  worker_read_hook = read;
  worker_write_hook = write;
}


#include <errno.h>
#include <machine/reg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef PBS_PS4
#include <ps4/kernel.h>
#include "platform/ps4/system/trampoline.h"
#else
#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#endif

/* PS5 ptrace identity. PS4 inherits the shared GoldHEN loader credentials;
 * changing its authid/caps on each tracing operation is not appropriate. */
#define PBS_PTRACE_AUTHID 0x4800000000010003L

static int pb_ptrace(int request, pid_t pid, caddr_t addr, int data) {
#ifndef PBS_PS4
  pid_t me = getpid();
  uint8_t privcaps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t caps[16];
  uint64_t authid = kernel_get_ucred_authid(me);
  if (!authid || kernel_get_ucred_caps(me, caps)) return -1;
  if (kernel_set_ucred_authid(me, PBS_PTRACE_AUTHID)) return -1;
  if (kernel_set_ucred_caps(me, privcaps)) {
    kernel_set_ucred_authid(me, authid);
    return -1;
  }
#endif

  long ret;
  int op_errno;
#ifdef PBS_PS4
  /* Orbis requires its libc ptrace wrapper for the SDK tracing path.
   * Issuing SYS_ptrace directly can attach successfully but leave the target
   * permanently stopped (observed as a black game screen).  Debug-register
   * requests still use the raw syscall first, matching ps4debug-NG. */
  if (request == PT_GETDBREGS || request == PT_SETDBREGS) {
    errno = 0;
    ret = syscall(SYS_ptrace, request, pid, addr, data);
    if (ret == -1 && errno != 0) {
      errno = 0;
      ret = ptrace(request, pid, addr, data);
    }
  } else {
    errno = 0;
    ret = ptrace(request, pid, addr, data);
  }
  op_errno = errno;
  /* Orbis reports successful register requests as -1 without setting errno. */
  if (ret == -1 && op_errno == 0 &&
      (request == PT_GETREGS || request == PT_SETREGS ||
       request == PT_GETFPREGS || request == PT_SETFPREGS ||
       request == PT_GETDBREGS || request == PT_SETDBREGS))
    ret = 0;
#else
  errno = 0;
  ret = syscall(SYS_ptrace, request, pid, addr, data);
  op_errno = errno;
#endif

#ifndef PBS_PS4
  int restore = kernel_set_ucred_caps(me, caps) |
                kernel_set_ucred_authid(me, authid);
  if (restore) return -1;
#endif
  errno = op_errno;
  return (int)ret;
}

pid_t pbs_find_pid(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0)) return -1;
  void *buf = malloc(size);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }
  pid_t found = -1;
  for (unsigned char *p = buf; p < (unsigned char *)buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    p += ki->ki_structsize;
    if (!strcmp(ki->ki_comm, name)) {
      found = ki->ki_pid;
      break;
    }
  }
  free(buf);
  return found;
}

int pbs_find_all_pids(const char *name, pid_t *out, int max) {
  int count = 0;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0)) return -1;
  void *buf = malloc(size);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }
  for (unsigned char *p = buf; p < (unsigned char *)buf + size && count < max;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    p += ki->ki_structsize;
    if (!strcmp(ki->ki_comm, name)) out[count++] = ki->ki_pid;
  }
  free(buf);
  return count;
}

#ifdef PBS_PS4
uint64_t pbs_process_birth_epoch(pid_t pid) {
  if (pid <= 0) return 0;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) || !size) return 0;
  void *buf = malloc(size);
  if (!buf) return 0;
  uint64_t epoch = 0;
  if (!sysctl(mib, 4, buf, &size, NULL, 0)) {
    for (unsigned char *p = buf; p + sizeof(uint32_t) <= (unsigned char *)buf + size;) {
      struct kinfo_proc *ki = (struct kinfo_proc *)p;
      if ((size_t)ki->ki_structsize < offsetof(struct kinfo_proc, ki_start) + sizeof ki->ki_start ||
          (size_t)ki->ki_structsize > (size_t)((unsigned char *)buf + size - p)) break;
      if (ki->ki_pid == pid && ki->ki_start.tv_sec > 0 &&
          ki->ki_start.tv_usec >= 0 && ki->ki_start.tv_usec < 1000000) {
        epoch = ((uint64_t)ki->ki_start.tv_sec << 20) |
                (uint64_t)ki->ki_start.tv_usec;
        break;
      }
      p += ki->ki_structsize;
    }
  }
  free(buf);
  return epoch;
}
#endif

intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  uint32_t handle = 0;
  if (kernel_dynlib_handle(pid, module, &handle)) return 0;
  return kernel_dynlib_dlsym(pid, handle, symbol);
}

/* Wait for a process attached with PT_ATTACH to stop; report the stop signal. */
static uint64_t pb_milliseconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int pb_wait_stop_sig(pid_t pid, int timeout_ms, int *sig) {
  if (sig) *sig = 0;
  for (int waited = 0; waited < timeout_ms; waited += 5) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED);
    if (r == pid) {
      if (sig && WIFSTOPPED(status)) *sig = WSTOPSIG(status);
      return WIFSTOPPED(status) ? 0 : -1;
    }
    if (r < 0 && errno != EINTR) return -1;
    usleep(5 * 1000);
  }
  return -1;
}

static int pb_wait_stop(pid_t pid, int timeout_ms) {
  return pb_wait_stop_sig(pid, timeout_ms, NULL);
}

/* Register operations address a thread (LWP), not the process.  Pick the
 * stopped LWP; the first PT_GETREGS that succeeds is the one the attach
 * stopped.  Fall back to the process id only when the LWP list is unavailable. */
static int pb_select_lwp(pbs_inject_t *ctx) {
#ifdef PBS_PS4
  struct reg regs;
  lwpid_t fallback_lwp = 0;
  struct reg fallback_regs;
  ctx->lwp = 0;
  ctx->lwp_kernel = 0;
  int count = (int)pb_ptrace(PT_GETNUMLWPS, ctx->pid, 0, 0);
  if (count > 0 && count <= 64) {
    lwpid_t lwps[64];
    long n = pb_ptrace(PT_GETLWPLIST, ctx->pid, (caddr_t)lwps, count);
    if (n > 0) {
      if (n > count) n = count;
      for (long i = 0; i < n; ++i) {
        if (pb_ptrace(PT_GETREGS, (pid_t)lwps[i], (caddr_t)&regs, 0)) continue;
        if (!fallback_lwp) {
          fallback_lwp = lwps[i];
          fallback_regs = regs;
        }
        /* Prefer a thread that is not parked in a libkernel syscall: the
         * kernel restarts such a syscall on resume and discards the RIP we
         * installed, so the injected call never returns to the trap. */
        if (!ctx->kbase || (intptr_t)regs.r_rip < ctx->kbase ||
            (intptr_t)regs.r_rip >= ctx->kend) {
          ctx->lwp = (pid_t)lwps[i];
          ctx->lwp_rip = (intptr_t)regs.r_rip;
          return 0;
        }
      }
      if (fallback_lwp) {
        ctx->lwp = (pid_t)fallback_lwp;
        ctx->lwp_rip = (intptr_t)fallback_regs.r_rip;
        ctx->lwp_kernel = 1;
        return 0;
      }
    }
  }
  if (!pb_ptrace(PT_GETREGS, ctx->pid, (caddr_t)&regs, 0)) {
    ctx->lwp = ctx->pid;
    ctx->lwp_rip = (intptr_t)regs.r_rip;
    ctx->lwp_kernel = ctx->kbase && ctx->lwp_rip >= ctx->kbase && ctx->lwp_rip < ctx->kend;
    return 0;
  }
  return -1;
#else
  ctx->lwp = ctx->pid;
  return 0;
#endif
}

static int pb_read(pid_t pid, intptr_t addr, void *out, size_t len) {
  struct ptrace_io_desc iod = {
      .piod_op = PIOD_READ_D,
      .piod_offs = (void *)addr,
      .piod_addr = out,
      .piod_len = len,
  };
  int rc = pb_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
  return rc ? rc : (iod.piod_len == len ? 0 : -1);
}

static int pb_write(pid_t pid, intptr_t addr, const void *in, size_t len) {
  struct ptrace_io_desc iod = {
      .piod_op = PIOD_WRITE_D,
      .piod_offs = (void *)addr,
      .piod_addr = (void *)in,
      .piod_len = len,
  };
  int rc = pb_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
  return rc ? rc : (iod.piod_len == len ? 0 : -1);
}

static int pb_begin(const char *proc_name, pbs_inject_t *ctx,
                    intptr_t preferred_page) {
  memset(ctx, 0, sizeof *ctx);
#ifdef PBS_PS4
  ctx->firmware = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (!ctx->firmware) { errno = ENOTSUP; return -1; }
#endif
  ctx->pid = pbs_find_pid(proc_name);
  if (ctx->pid <= 0) return -1;

  /* FW11 can report EEXIST/EBUSY for a short window after a previous
   * PT_DETACH. Retry only those statuses; every other attach failure is
   * fatal. A 2-second budget covers the observed detach/reattach window. */
  unsigned attach_attempts = 0;
  for (;;) {
    if (!pb_ptrace(PT_ATTACH, ctx->pid, 0, 0)) break;
    if ((errno != EEXIST && errno != EBUSY) || ++attach_attempts >= 200)
      return -1;
    usleep(10 * 1000);
  }
  ctx->attached = 1;
  if (pb_wait_stop(ctx->pid, 2000)) {
    pbs_inject_end(ctx);
    return -1;
  }
  if (pb_select_lwp(ctx)) {
    pbs_inject_end(ctx);
    return -1;
  }

  /* Reserve a full writable page below the stopped thread stack pointer.
   * Aligning to the page base guarantees every PBS_PS4R_* offset (up to
   * 0xfff) stays inside the verified mapping even when PT_IO writes span
   * the original 16-byte-aligned base's page boundary. */
  struct reg regs;
  if (pb_ptrace(PT_GETREGS, ctx->lwp, (caddr_t)&regs, 0)) {
    pbs_inject_end(ctx);
    return -1;
  }
  ctx->scratch_len = 0x1000;
#ifdef PBS_PS4
  unsigned char probe = 0x5a;
  static const intptr_t scratch_offsets[] = {0x10000, 0x8000, 0x4000, 0x2000};
  static const intptr_t probes[] = {0, 0x800, 0xfff};
  if (preferred_page > 0) {
    /* The page validated by the control phase remains mapped in the same
     * process. Reuse it only if the new RSP still leaves a safe gap above it;
     * otherwise the target's active stack could overlap the page. */
    intptr_t limit = (intptr_t)regs.r_rsp - 0x2000;
    if (preferred_page + 0x1000 <= limit) {
      int writable = 1;
      for (size_t i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
        if (pb_read(ctx->pid, preferred_page + probes[i], &probe, 1) ||
            pb_write(ctx->pid, preferred_page + probes[i], &probe, 1)) {
          writable = 0;
          break;
        }
      }
      if (writable) {
        ctx->scratch = preferred_page;
        return 0;
      }
    }
  }
  for (size_t o = 0; o < sizeof scratch_offsets / sizeof scratch_offsets[0]; ++o) {
    intptr_t candidate = (intptr_t)((regs.r_rsp - scratch_offsets[o]) & ~(intptr_t)0xfff);
    if (candidate <= 0) continue;
    int writable = 1;
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
      if (pb_read(ctx->pid, candidate + probes[i], &probe, 1) ||
          pb_write(ctx->pid, candidate + probes[i], &probe, 1)) {
        writable = 0;
        break;
      }
    }
    if (writable) {
      ctx->scratch = candidate;
      return 0;
    }
  }
#else
  ctx->scratch = (intptr_t)((regs.r_rsp - 0x4000) & ~(intptr_t)0xfff);
  if (ctx->scratch > 0) {
    int prot = kernel_get_vmem_protection(ctx->pid, ctx->scratch, 0x1000);
    if (prot >= 0 && (prot & PROT_WRITE)) return 0;
  }
#endif
  /* Distinguish "no writable page below the current RSP" from an attach
   * conflict, so callers can wait for the target to return to a shallower
   * call state instead of retrying PT_ATTACH. */
  errno = ENOMEM;
  pbs_inject_end(ctx);
  return -1;
}

int pbs_inject_begin(const char *proc_name, pbs_inject_t *ctx) {
  (void)pbs_ps4_cred_elevate(stderr);
  return pb_begin(proc_name, ctx, 0);
}

int pbs_inject_begin_preferred(const char *proc_name, pbs_inject_t *ctx,
                               intptr_t preferred_page) {
  /* PT_ATTACH is credential-gated on this firmware: adopt the jailbreak
   * identity first (prison0 + rootvnode + uid 0), as ps4debug/MemDBG do. */
  (void)pbs_ps4_cred_elevate(stderr);
  return pb_begin(proc_name, ctx, preferred_page);
}

int pbs_inject_ensure_stopped(pbs_inject_t *ctx) {
  if (worker_call_hook) return 0; /* the worker runs while the target is free */
  if (!ctx || !ctx->attached) return -1;
  struct reg regs;
  /* PT_GETREGS succeeds only while the tracee is stopped. A prior
   * pb_call_fast already consumed its SIGTRAP event, so waitpid cannot be
   * used to distinguish "stopped, event consumed" from "running". */
  if (ctx->lwp > 0 && !pb_ptrace(PT_GETREGS, ctx->lwp, (caddr_t)&regs, 0)) {
    ctx->running = 0;
    return 0;
  }
  if (kill(ctx->pid, SIGSTOP)) return -1;
  if (pb_wait_stop(ctx->pid, 2000)) return -1;
  if (pb_select_lwp(ctx)) return -1;
  ctx->running = 0;
  return 0;
}

int pbs_inject_revalidate_scratch(pbs_inject_t *ctx) {
  if (worker_call_hook) return 0;
  if (!ctx || !ctx->attached || ctx->scratch <= 0) return -1;
  struct reg regs;
  pid_t reg_pid = ctx->lwp > 0 ? ctx->lwp : ctx->pid;
  if (pb_ptrace(PT_GETREGS, reg_pid, (caddr_t)&regs, 0)) return -1;
  intptr_t limit = (intptr_t)regs.r_rsp - 0x2000;
  if (ctx->scratch + 0x1000 > limit) return -1;
  unsigned char probe = 0x5a;
  static const intptr_t probes[] = {0, 0x800, 0xfff};
  for (size_t i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
    if (pb_read(ctx->pid, ctx->scratch + probes[i], &probe, 1) ||
        pb_write(ctx->pid, ctx->scratch + probes[i], &probe, 1))
      return -1;
  }
  return 0;
}

long pbs_inject_call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  if (worker_call_hook) return worker_call_hook(fn, args);
  struct reg bak, jmp;
  if (!ctx || pbs_inject_ensure_stopped(ctx)) return LONG_MIN;
  pid_t reg_pid = ctx->lwp > 0 ? ctx->lwp : ctx->pid;
  if (pb_ptrace(PT_GETREGS, reg_pid, (caddr_t)&bak, 0)) return LONG_MIN;
  memcpy(&jmp, &bak, sizeof jmp);
  jmp.r_rip = fn;
  /* SysV call frame: align RSP to 16 bytes and leave 0x200 bytes for the callee. */
  jmp.r_rsp = (bak.r_rsp - 0x200) & ~UINT64_C(15);
  jmp.r_rdi = args[0];
  jmp.r_rsi = args[1];
  jmp.r_rdx = args[2];
  jmp.r_rcx = args[3];
  jmp.r_r8 = args[4];
  jmp.r_r9 = args[5];
  if (pb_ptrace(PT_SETREGS, reg_pid, (caddr_t)&jmp, 0)) return LONG_MIN;

  /* Single-step until RSP passes the initial value, indicating a return. Resume assertion stops with SIGCONT and record the assertion. */
  uint64_t start_rsp = (uint64_t)jmp.r_rsp;
  int steps = 0;
  long result = LONG_MIN;
  while (steps++ < 20000) {
    if (pb_ptrace(PT_STEP, ctx->pid, (caddr_t)1, 0)) break;
    int sig = 0;
    if (pb_wait_stop_sig(ctx->pid, 500, &sig)) break;
    if (sig == SIGSTOP) {
      kill(ctx->pid, SIGCONT);
      ctx->asserted = 1;
      break;
    }
    if (pb_ptrace(PT_GETREGS, reg_pid, (caddr_t)&jmp, 0)) break;
    if ((uint64_t)jmp.r_rsp > start_rsp) {
      result = (long)jmp.r_rax;
      break;
    }
  }
  pb_ptrace(PT_SETREGS, reg_pid, (caddr_t)&bak, 0);
  ctx->running = 0;
  return result;
}

int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t addr, void *out, size_t len) {
  if (worker_read_hook) return worker_read_hook(addr, out, len);
  if (!ctx || pbs_inject_ensure_stopped(ctx)) return -1;
  return pb_read(ctx->pid, addr, out, len);
}

int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t addr, const void *in, size_t len) {
  if (worker_write_hook) return worker_write_hook(addr, in, len);
  if (!ctx || pbs_inject_ensure_stopped(ctx)) return -1;
  return pb_write(ctx->pid, addr, in, len);
}

/* Public ptrace register ABI from machine/reg.h, not a kernel object layout.
 * The former hand-written 368-byte structure was too small for PT_GETFPREGS
 * (512 bytes), and put XMM0 at the wrong offset. */
typedef struct fpreg pbs_fpreg_t;
_Static_assert(sizeof(pbs_fpreg_t) == 512, "amd64 ptrace FP register ABI");
_Static_assert(offsetof(pbs_fpreg_t, fpr_xacc) == 160, "amd64 XMM register offset");

static long pb_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6], const float *xmm0) {
  struct reg bak, regs;
  if (ctx) { ctx->call_stage = "ensure-stopped"; ctx->call_errno = 0; }
  if (ctx && ctx->asserted) {
    ctx->call_stage = "quarantined-context";
    ctx->call_errno = EIO;
    return LONG_MIN;
  }
#ifdef PBS_PS4
  /* MemDBG pal/debug/freebsd.inc explicitly disables Orbis FP ptrace:
   * PT_GETFPREGS can break the debugger connection and strand the tracee.
   * Never issue this request merely because the SDK declares it. */
  if (xmm0) {
    if (ctx) { ctx->call_stage = "ps4-fpregs-unsupported"; ctx->call_errno = ENOTSUP; }
    return LONG_MIN;
  }
#endif
  if (!ctx || !fn || pbs_inject_ensure_stopped(ctx)) return LONG_MIN;
  pid_t reg_pid = ctx->lwp > 0 ? ctx->lwp : ctx->pid;
  int plant = 1;
#ifdef PBS_PS4
  if (pb_ptrace(PT_GETREGS, reg_pid, (caddr_t)&bak, 0)) return LONG_MIN;
  /* Never fall back to running a callee without preserving SIMD state. */
  if (ctx->trap <= 0 || (ctx->scratch & 63) || ctx->scratch_len < 0xc08) {
    ctx->call_stage = "ps4-trampoline-unavailable";
    ctx->call_errno = ENOTSUP;
    return LONG_MIN;
  }
  unsigned char code[PBS_PS4_CALL_SLOTS][10], code_backup[PBS_PS4_CALL_SLOTS][10];
  unsigned code_installed = 0;
  intptr_t entry_addr = ctx->trap;
  intptr_t trap_addr = entry_addr + PBS_PS4_CALL_TRAP_DELTA;
  plant = 0;
  ctx->call_stage = "ps4-trampoline-validate";
  if (pbs_ps4_call_code(code, entry_addr, ctx->scratch + PBS_PS4_CALL_STATE_OFF,
                       ctx->scratch + PBS_PS4_CALL_FN_OFF)) return LONG_MIN;
  for (unsigned i = 0; i < PBS_PS4_CALL_SLOTS; ++i) {
    if (pb_read(ctx->pid, entry_addr + i * 16, code_backup[i], 10)) return LONG_MIN;
    for (unsigned j = 0; j < 10; ++j)
      if (code_backup[i][j] != 0xcc) {
        ctx->call_errno = ENOTSUP;
        return LONG_MIN;
      }
  }
#else
  intptr_t trap_addr = ctx->scratch + 0xf00;
  intptr_t page = trap_addr & ~(intptr_t)0x3fff;
  int prot = kernel_get_vmem_protection(ctx->pid, page, 0x4000);
#endif
  unsigned char original = 0, trap = 0xcc;
  uint64_t stack_word;
  pbs_fpreg_t fpbak, fpnew;
  int fp_changed = 0;
  int preserve_fp = xmm0 != NULL;
#ifndef PBS_PS4
  if (prot < 0 || pb_ptrace(PT_GETREGS, reg_pid, (caddr_t)&bak, 0)) return LONG_MIN;
#endif
  if (plant && pb_read(ctx->pid, trap_addr, &original, 1)) return LONG_MIN;
  if (preserve_fp) {
    ctx->call_stage = "get-fpregs";
    if (pb_ptrace(PT_GETFPREGS, reg_pid, (caddr_t)&fpbak, 0)) {
      ctx->call_errno = errno;
      return LONG_MIN;
    }
    memcpy(&fpnew, &fpbak, sizeof fpnew);
    if (xmm0) memcpy(&fpnew.fpr_xacc[0][0], xmm0, sizeof *xmm0);
  }
  regs = bak;
  regs.r_rsp = ((bak.r_rsp - 0x200) & ~(intptr_t)15) - 8;
  ctx->call_stage = "read-stack";
  if (pb_read(ctx->pid, regs.r_rsp, &stack_word, sizeof stack_word)) {
    ctx->call_errno = errno;
    return LONG_MIN;
  }
#ifndef PBS_PS4
  if (kernel_set_vmem_protection(ctx->pid, page, 0x4000, prot | PROT_EXEC)) return LONG_MIN;
#endif
  long result = LONG_MIN;
  int changed_regs = 0, running = 0, sig = 0;
#ifdef PBS_PS4
  unsigned char state_zero[PBS_PS4_CALL_STATE_SIZE] = {0};
  uint64_t target_fn = (uint64_t)fn;
  ctx->call_stage = "ps4-trampoline-install";
  if (pb_write(ctx->pid, ctx->scratch + PBS_PS4_CALL_STATE_OFF,
               state_zero, sizeof state_zero) ||
      pb_write(ctx->pid, ctx->scratch + PBS_PS4_CALL_FN_OFF,
               &target_fn, sizeof target_fn)) goto fast_cleanup;
  for (unsigned i = 0; i < PBS_PS4_CALL_SLOTS; ++i) {
    /* Count even a failed write: PT_IO may have partially changed the slot. */
    code_installed = i + 1;
    if (pb_write(ctx->pid, entry_addr + i * 16, code[i], 10)) goto fast_cleanup;
  }
#endif
  ctx->call_stage = "write-return";
  if ((plant && pb_write(ctx->pid, trap_addr, &trap, 1)) ||
      pb_write(ctx->pid, regs.r_rsp, &trap_addr, sizeof trap_addr)) goto fast_cleanup;
  regs.r_rip = fn;
#ifdef PBS_PS4
  regs.r_rip = entry_addr;
#endif
  regs.r_rdi = args[0]; regs.r_rsi = args[1]; regs.r_rdx = args[2];
  regs.r_rcx = args[3]; regs.r_r8 = args[4]; regs.r_r9 = args[5];
  ctx->call_stage = "set-regs";
  if (pb_ptrace(PT_SETREGS, reg_pid, (caddr_t)&regs, 0)) goto fast_cleanup;
  changed_regs = 1;
  if (preserve_fp) {
    ctx->call_stage = "set-fpregs";
    if (pb_ptrace(PT_SETFPREGS, reg_pid, (caddr_t)&fpnew, 0)) goto fast_cleanup;
    fp_changed = 1;
  }
  ctx->call_stage = "continue";
  if (pb_ptrace(PT_CONTINUE, ctx->pid, (caddr_t)1, 0)) goto fast_cleanup;
  running = 1;
  ctx->running = 1;
  ctx->call_stage = "wait-return-trap";
  {
    /* A SIGSTOP park followed by PT_CONTINUE is not always enough to put the
     * tracee back on the CPU on FW11: the first remote ReadVideo after a
     * resume timed out for the full 12 s with the callee never returning.
     * While the trap has not fired yet, nudge the process with SIGCONT and
     * pass any unrelated stop through instead of abandoning the call. */
    uint64_t wait_until = pb_milliseconds() + 12000;
    int nudged = 0, passes = 0;
    for (;;) {
      uint64_t left = wait_until > pb_milliseconds() ? wait_until - pb_milliseconds() : 0;
      if (!left) break;
      int slice = (int)(left < 250 ? left : 250);
      if (pb_wait_stop_sig(ctx->pid, slice, &sig)) {
        if (!nudged) {
          kill(ctx->pid, SIGCONT);
          ++ctx->nudges;
          nudged = 1;
        }
        continue;
      }
      running = 0;
      ctx->running = 0;
      if (pb_ptrace(PT_GETREGS, reg_pid, (caddr_t)&regs, 0)) break;
      if (sig == SIGTRAP && regs.r_rip == trap_addr + 1) {
        result = (long)regs.r_rax;
        break;
      }
      if (++passes > 4) break;
      int deliver = (sig == SIGSTOP || sig == SIGCONT) ? 0 : sig;
      if (pb_ptrace(PT_CONTINUE, ctx->pid, (caddr_t)1, deliver)) break;
      running = 1;
      ctx->running = 1;
    }
  }
fast_cleanup:
  if (result == LONG_MIN) ctx->call_errno = errno;
  if (running) {
    kill(ctx->pid, SIGSTOP);
    if (pb_wait_stop(ctx->pid, 2000)) return LONG_MIN;
    ctx->running = 0;
  }
  int restore = plant ? pb_write(ctx->pid, trap_addr, &original, 1) : 0;
#ifdef PBS_PS4
  for (unsigned i = 0; i < code_installed; ++i)
    restore |= pb_write(ctx->pid, entry_addr + i * 16, code_backup[i], 10);
#endif
  intptr_t call_rsp = ((bak.r_rsp - 0x200) & ~(intptr_t)15) - 8;
  restore |= pb_write(ctx->pid, call_rsp, &stack_word, sizeof stack_word);
  if (changed_regs) restore |= pb_ptrace(PT_SETREGS, reg_pid, (caddr_t)&bak, 0);
  if (fp_changed) restore |= pb_ptrace(PT_SETFPREGS, reg_pid, (caddr_t)&fpbak, 0);
  if (restore) { ctx->call_stage = "restore-registers-or-stack"; ctx->call_errno = errno; }
  else if (result != LONG_MIN) ctx->call_stage = "complete";
#ifndef PBS_PS4
  restore |= kernel_set_vmem_protection(ctx->pid, page, 0x4000, prot);
#endif
  return restore ? LONG_MIN : result;
}

long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  if (!ctx || ctx->asserted) return PBS_CALL_TIMEOUT;
  if (worker_call_hook) return worker_call_hook(fn, args);
  return pb_call_fast(ctx, fn, args, NULL);
}

long pbs_inject_call_fast_fp(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6], float xmm0) {
  return pb_call_fast(ctx, fn, args, &xmm0);
}

int pbs_inject_read(pbs_inject_t *ctx, void *out, size_t len) {
  if (!ctx || pbs_inject_ensure_stopped(ctx)) return -1;
  if (len > ctx->scratch_len) len = ctx->scratch_len;
  return pbs_inject_read_at(ctx, ctx->scratch, out, len);
}

int pbs_inject_write(pbs_inject_t *ctx, const void *in, size_t len) {
  if (!ctx || pbs_inject_ensure_stopped(ctx)) return -1;
  if (len > ctx->scratch_len) len = ctx->scratch_len;
  return pbs_inject_write_at(ctx, ctx->scratch, in, len);
}

int pbs_inject_resume(pbs_inject_t *ctx) {
  if (!ctx || !ctx->attached || ctx->running) return 0;
  if (pb_ptrace(PT_CONTINUE, ctx->pid, (caddr_t)1, 0)) return -1;
  ctx->running = 1;
  return 0;
}

int pbs_inject_target_rsp(pbs_inject_t *ctx, intptr_t *rsp) {
  if (!ctx || !rsp || pbs_inject_ensure_stopped(ctx)) return -1;
  struct reg regs;
  pid_t reg_pid = ctx->lwp > 0 ? ctx->lwp : ctx->pid;
  if (pb_ptrace(PT_GETREGS, reg_pid, (caddr_t)&regs, 0)) return -1;
  *rsp = (intptr_t)regs.r_rsp;
  return 0;
}

void pbs_inject_end(pbs_inject_t *ctx) {
  if (!ctx || !ctx->attached) return;
  if (ctx->running && pbs_inject_ensure_stopped(ctx)) {
    /* Never leave a half-attached console stopped: fall through to detach. */
  }
  if (pb_ptrace(PT_DETACH, ctx->pid, 0, 0)) {
    /* PT_DETACH resumes the tracee; a job-control stop may still need
     * SIGCONT if detach itself failed. */
    kill(ctx->pid, SIGCONT);
  }
  ctx->attached = 0;
  ctx->running = 0;
  ctx->lwp = 0;
}

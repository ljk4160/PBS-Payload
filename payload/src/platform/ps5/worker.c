// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/worker.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <ps5/kernel.h>

#include "platform/ps4/worker/client.h"
#include "platform/ps4/worker/address.h"
#include "platform/ps4/worker/remote.h"

#define PBS_PS5_WORKER_MAP_SIZE 0x4000u
#define PBS_PS5_WORKER_THREAD_OFF 0x700u
/* A private frame buffer held by the worker for the whole boot lifetime.
 * Repeated captures used to malloc(0x200000) inside SceRemotePlay on every
 * start; long sessions plus restart cycles fragmented its heap until the
 * allocation returned 0 and every later injection failed immediately. The
 * fallback buffer is allocated once from the same MAP_ANON pool at boot and
 * munmapped at cleanup, so capture restarts never touch the target heap. */
#define PBS_PS5_WORKER_FRAME_SIZE 0x200000u

typedef struct {
  pbs_worker_link link;
  pid_t pid;
  intptr_t map;
  size_t map_size;
  uint64_t thread;
  intptr_t fn_join;
  intptr_t fn_munmap;
  intptr_t frame_buffer;
  int running;
  /* A late bootstrap/shutdown failure can leave a live thread or mapping in
   * the target.  Keep its identity and refuse another boot rather than
   * overwriting the only information that could explain or clean it up. */
  int quarantined;
} pbs_ps5_worker_state;

static pbs_ps5_worker_state worker = {
    .link = {.listen_fd = -1, .fd = -1},
};

static long hook_call(intptr_t fn, const uint64_t args[6]) {
  int64_t value = 0;
  if (pbs_worker_link_call(&worker.link, (uint64_t)fn, args, &value, 4000))
    return PBS_CALL_TIMEOUT;
  return (long)value;
}

static int hook_read(intptr_t addr, void *out, size_t len) {
  if (!len || len > PBS_WORKER_COPY_MAX) return -1;
  return pbs_worker_link_copy(&worker.link, (uint64_t)addr, out,
                              (uint32_t)len, 4000);
}

static int hook_write(intptr_t addr, const void *in, size_t len) {
  if (!len || len > PBS_WORKER_WRITE_MAX) return -1;
  return pbs_worker_link_write(&worker.link, (uint64_t)addr, in,
                               (uint32_t)len, 4000);
}

static intptr_t resolve_first(pbs_inject_t *ctx, FILE *log,
                              const char *const *symbols,
                              const char *label) {
  static const char *const modules[] = {
      "libkernel_sys.sprx", "libSceLibcInternal.sprx", "libkernel.sprx", NULL};
  for (size_t m = 0; modules[m]; ++m) {
    for (size_t s = 0; symbols[s]; ++s) {
      intptr_t fn = pbs_resolve_in(ctx->pid, modules[m], symbols[s]);
      if (fn) {
        fprintf(log, "PS5 worker: %s -> %s!%s fn=%p\n", label,
                modules[m], symbols[s], (void *)fn);
        return fn;
      }
    }
  }
  fprintf(log, "PS5 worker: %s unresolved\n", label);
  return 0;
}

static int cleanup_mapping(FILE *log, int join_thread) {
  pbs_inject_t cleanup;
  int result = -1;
  if (!worker.pid || !worker.map || !worker.fn_munmap) return -1;
  if (pbs_inject_begin("SceRemotePlay", &cleanup)) {
    fprintf(log, "PS5 worker cleanup: attach failed errno=%d; mapping retained\n",
            errno);
    return -1;
  }
  if (cleanup.pid != worker.pid) {
    fprintf(log,
            "PS5 worker cleanup: process replacement old=%d new=%d; stale mapping ignored\n",
            (int)worker.pid, (int)cleanup.pid);
    pbs_inject_end(&cleanup);
    return -1;
  }
  if (worker.frame_buffer) {
    long frc = pbs_inject_call_fast(
        &cleanup, worker.fn_munmap,
        (uint64_t[6]){worker.frame_buffer, PBS_PS5_WORKER_FRAME_SIZE, 0, 0, 0, 0});
    fprintf(log, "PS5 worker cleanup: munmap(frame 0x%zx) -> 0x%08lx\n",
            (size_t)worker.frame_buffer, (unsigned long)frc);
    worker.frame_buffer = 0;
  }
  if (join_thread) {
    long rc = pbs_inject_call_fast(
        &cleanup, worker.fn_join,
        (uint64_t[6]){worker.thread, 0, 0, 0, 0, 0});
    fprintf(log, "PS5 worker cleanup: pthread_join -> 0x%08lx\n",
            (unsigned long)rc);
    if (rc == PBS_CALL_TIMEOUT || (int32_t)(uint32_t)rc != 0) {
      pbs_inject_end(&cleanup);
      return -1;
    }
  }
  long rc = pbs_inject_call_fast(
      &cleanup, worker.fn_munmap,
      (uint64_t[6]){(uint64_t)worker.map, worker.map_size, 0, 0, 0, 0});
  fprintf(log, "PS5 worker cleanup: munmap(%p,0x%zx) -> 0x%08lx\n",
          (void *)worker.map, worker.map_size, (unsigned long)rc);
  result = rc != PBS_CALL_TIMEOUT && (int32_t)(uint32_t)rc == 0 ? 0 : -1;
  pbs_inject_end(&cleanup);
  return result;
}

static int discard_attached_mapping(pbs_inject_t *ctx, FILE *log) {
  if (!ctx || !ctx->attached || !worker.map || !worker.fn_munmap) return -1;
  long rc = pbs_inject_call_fast(
      ctx, worker.fn_munmap,
      (uint64_t[6]){(uint64_t)worker.map, worker.map_size, 0, 0, 0, 0});
  fprintf(log, "PS5 worker rollback: munmap(%p,0x%zx) -> 0x%08lx\n",
          (void *)worker.map, worker.map_size, (unsigned long)rc);
  return rc != PBS_CALL_TIMEOUT && (int32_t)(uint32_t)rc == 0 ? 0 : -1;
}

static void reset_worker(void) {
  memset(&worker, 0, sizeof worker);
  worker.link.listen_fd = worker.link.fd = -1;
}

static void quarantine_worker(FILE *log, const char *reason) {
  worker.running = 0;
  worker.quarantined = 1;
  fprintf(log, "PS5 worker quarantined: %s; no automatic reboot\n", reason);
}

int pbs_ps5_worker_boot(pbs_inject_t *ctx, FILE *log) {
  if (!ctx || !log || !ctx->attached) return -1;
  if (worker.running || worker.quarantined || worker.map) {
    fprintf(log, "PS5 worker: existing or uncertain state blocks bootstrap\n");
    return -1;
  }

  const size_t blob =
      (size_t)(__stop_pbs_worker_remote - __start_pbs_worker_remote);
  const size_t context_off = (blob + 0xfu) & ~(size_t)0xfu;
  if (blob < 32 || context_off + sizeof(pbs_worker_remote_ctx) >
                       PBS_PS5_WORKER_MAP_SIZE) {
    fprintf(log, "PS5 worker: invalid blob size=%zu context=0x%zx\n", blob,
            context_off);
    return -1;
  }

  if (pbs_worker_link_open(&worker.link, 0)) {
    fprintf(log, "PS5 worker: loopback listener failed errno=%d\n", errno);
    return -1;
  }

  static const char *const create_names[] = {"pthread_create", "scePthreadCreate", NULL};
  static const char *const join_names[] = {"pthread_join", "scePthreadJoin", NULL};
  static const char *const mmap_names[] = {"mmap", "sceKernelMmap", NULL};
  static const char *const munmap_names[] = {"munmap", "sceKernelMunmap", NULL};
  static const char *const socket_names[] = {"socket", NULL};
  static const char *const connect_names[] = {"connect", NULL};
  static const char *const recv_names[] = {"recv", NULL};
  static const char *const send_names[] = {"send", NULL};
  static const char *const close_names[] = {"close", NULL};
  static const char *const setopt_names[] = {"setsockopt", NULL};
  static const char *const sleep_names[] = {"usleep", NULL};

  intptr_t fn_create = resolve_first(ctx, log, create_names, "pthread_create");
  intptr_t fn_mmap = resolve_first(ctx, log, mmap_names, "mmap");
  worker.fn_join = resolve_first(ctx, log, join_names, "pthread_join");
  worker.fn_munmap = resolve_first(ctx, log, munmap_names, "munmap");
  intptr_t fn_socket = resolve_first(ctx, log, socket_names, "socket");
  intptr_t fn_connect = resolve_first(ctx, log, connect_names, "connect");
  intptr_t fn_recv = resolve_first(ctx, log, recv_names, "recv");
  intptr_t fn_send = resolve_first(ctx, log, send_names, "send");
  intptr_t fn_close = resolve_first(ctx, log, close_names, "close");
  intptr_t fn_setopt = resolve_first(ctx, log, setopt_names, "setsockopt");
  intptr_t fn_sleep = resolve_first(ctx, log, sleep_names, "usleep");
  if (!fn_create || !worker.fn_join || !fn_mmap || !worker.fn_munmap ||
      !fn_socket || !fn_connect || !fn_recv || !fn_send || !fn_close ||
      !fn_setopt || !fn_sleep) {
    fprintf(log, "PS5 worker: required target exports unavailable\n");
    pbs_worker_link_close(&worker.link);
    return -1;
  }

  long map = pbs_inject_call_fast(
      ctx, fn_mmap,
      (uint64_t[6]){0, PBS_PS5_WORKER_MAP_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, (uint64_t)-1, 0});
  if (map <= 0 || map == PBS_CALL_TIMEOUT) {
    fprintf(log, "PS5 worker: mmap RW failed result=0x%016lx\n",
            (unsigned long)map);
    pbs_worker_link_close(&worker.link);
    return -1;
  }

  worker.pid = ctx->pid;
  worker.map = (intptr_t)map;
  worker.map_size = PBS_PS5_WORKER_MAP_SIZE;
  /* Reserve the private frame buffer now: capture restarts reuse it instead
   * of malloc(2MB) inside SceRemotePlay, which fragments the target heap. */
  long frame_map = pbs_inject_call_fast(
      ctx, fn_mmap,
      (uint64_t[6]){0, PBS_PS5_WORKER_FRAME_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, (uint64_t)-1, 0});
  if (frame_map > 0 && frame_map != PBS_CALL_TIMEOUT) {
    worker.frame_buffer = (intptr_t)frame_map;
    fprintf(log, "PS5 worker: frame buffer reserved at %p size=0x%x\n",
            (void *)worker.frame_buffer, PBS_PS5_WORKER_FRAME_SIZE);
  } else {
    worker.frame_buffer = 0;
    fprintf(log, "PS5 worker: frame buffer reservation failed (heap capture will try malloc)\n");
  }
  const intptr_t arg = worker.map + (intptr_t)context_off;

  pbs_worker_remote_ctx remote;
  memset(&remote, 0, sizeof remote);
  remote.fn_socket = (uint64_t)fn_socket;
  remote.fn_connect = (uint64_t)fn_connect;
  remote.fn_recv = (uint64_t)fn_recv;
  remote.fn_send = (uint64_t)fn_send;
  remote.fn_close = (uint64_t)fn_close;
  remote.fn_sleep = (uint64_t)fn_sleep;
  remote.fn_setsockopt = (uint64_t)fn_setopt;
  remote.cookie = worker.link.cookie;
  pbs_ps4_worker_addr(remote.addr, worker.link.port);

  if (pbs_inject_write_at(ctx, worker.map, __start_pbs_worker_remote, blob) ||
      pbs_inject_write_at(ctx, arg, &remote, sizeof remote)) {
    fprintf(log, "PS5 worker: mapping write failed errno=%d\n", errno);
    int rollback = discard_attached_mapping(ctx, log);
    pbs_worker_link_close(&worker.link);
    if (rollback) quarantine_worker(log, "mapping write rollback failed");
    else reset_worker();
    return -1;
  }

  if (kernel_set_vmem_protection(ctx->pid, worker.map, worker.map_size,
                                 PROT_READ | PROT_EXEC)) {
    fprintf(log, "PS5 worker: RW->RX protection failed errno=%d\n", errno);
    int rollback = discard_attached_mapping(ctx, log);
    pbs_worker_link_close(&worker.link);
    if (rollback) quarantine_worker(log, "protection rollback failed");
    else reset_worker();
    return -1;
  }

  long rc = pbs_inject_call_fast(
      ctx, fn_create,
      (uint64_t[6]){(uint64_t)(ctx->scratch + PBS_PS5_WORKER_THREAD_OFF), 0,
                    (uint64_t)worker.map, (uint64_t)arg, 0, 0});
  if (rc == PBS_CALL_TIMEOUT) {
    fprintf(log, "PS5 worker: pthread_create outcome uncertain\n");
    pbs_worker_link_close(&worker.link);
    quarantine_worker(log, "pthread_create timed out");
    return -1;
  }
  if ((int32_t)(uint32_t)rc != 0) {
    fprintf(log, "PS5 worker: pthread_create failed result=0x%016lx\n",
            (unsigned long)rc);
    int rollback = discard_attached_mapping(ctx, log);
    pbs_worker_link_close(&worker.link);
    if (rollback) quarantine_worker(log, "pthread_create rollback failed");
    else reset_worker();
    return -1;
  }
  if (pbs_inject_read_at(ctx, ctx->scratch + PBS_PS5_WORKER_THREAD_OFF,
                         &worker.thread, sizeof worker.thread) ||
      !worker.thread) {
    fprintf(log, "PS5 worker: created thread handle could not be read\n");
    pbs_inject_end(ctx);
    pbs_worker_link_close(&worker.link);
    quarantine_worker(log, "created thread handle unavailable");
    return -1;
  }

  fprintf(log,
          "PS5 worker: created map=%p size=0x%zx thread=0x%llx; detaching target\n",
          (void *)worker.map, worker.map_size,
          (unsigned long long)worker.thread);
  pbs_inject_end(ctx);

  if (pbs_worker_link_accept(&worker.link, 5000)) {
    fprintf(log, "PS5 worker: connection timeout; joining bootstrap thread\n");
    pbs_worker_link_close(&worker.link);
    if (cleanup_mapping(log, 1)) quarantine_worker(log, "connection-timeout cleanup failed");
    else reset_worker();
    return -1;
  }

  uint64_t scratch = 0;
  if (pbs_worker_link_scratch(&worker.link, &scratch)) {
    fprintf(log, "PS5 worker: private scratch handshake failed\n");
    (void)pbs_worker_link_exit(&worker.link);
    pbs_worker_link_close(&worker.link);
    if (cleanup_mapping(log, 1)) quarantine_worker(log, "handshake cleanup failed");
    else reset_worker();
    return -1;
  }

  pbs_inject_set_worker_hooks(hook_call, hook_read, hook_write);
  ctx->worker = 1;
  ctx->scratch = (intptr_t)scratch;
  ctx->scratch_len = PBS_WORKER_WRITE_MAX;
  worker.running = 1;
  fprintf(log,
          "PS5 worker: active scratch=%p; SceRemotePlay is detached and running\n",
          (void *)ctx->scratch);
  return 0;
}

int pbs_ps5_worker_shutdown(FILE *log) {
  if (worker.quarantined) {
    fprintf(log, "PS5 worker shutdown: quarantined state retained\n");
    return -1;
  }
  if (!worker.running) return 0;
  int exit_rc = pbs_worker_link_exit(&worker.link);
  pbs_inject_set_worker_hooks(NULL, NULL, NULL);
  pbs_worker_link_close(&worker.link);
  worker.running = 0;
  if (exit_rc) {
    fprintf(log, "PS5 worker shutdown: RPC exit uncertain; no join/unmap attempted\n");
    quarantine_worker(log, "RPC exit uncertain");
    return -1;
  }
  int cleanup_rc = cleanup_mapping(log, 1);
  if (cleanup_rc) quarantine_worker(log, "joined cleanup failed");
  else reset_worker();
  return cleanup_rc;
}

intptr_t pbs_ps5_worker_frame_buffer(void) {
  return worker.frame_buffer;
}

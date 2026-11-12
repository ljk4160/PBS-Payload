// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/http/server.h"
#include "webui.h"
#include "banner.h"
#include "common/media/rtp.h"
#include "common/media/buffer.h"
#include "common/media/profiles.h"
#include "common/http/byte_range.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef PBS_PS4
#include <fcntl.h>
static void ps4_http_trace(const char *text) {
  int fd = open("/data/pbs/ps4/http.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd >= 0) { write(fd, text, strlen(text)); close(fd); }
}
#define HTTP_TRACE(text) ps4_http_trace(text)
#else
#define HTTP_TRACE(text) ((void)0)
#endif

#define SLOTS 64
#define FRAME_MAX 0x200000u
#define KEYFRAME_INTERVAL_MS 4000u
#define AUDIO_SLOTS 256
#define AUDIO_MAX 0x10000u
#include "common/http/recording_storage.h"
#define REC_DIR recording_directory
typedef struct { unsigned char *data; size_t len, capacity; uint64_t seq; uint32_t pts; int key; } frame_t;
typedef struct { unsigned char *data; size_t len; uint64_t seq; uint32_t pts; } audio_chunk_t;
static struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  frame_t frames[SLOTS];
  unsigned char sps[128], pps[128];
  size_t sps_len, pps_len;
  uint64_t seq, bytes, started;
  uint64_t last_keyframe_ms, last_key_request_ms;
  int running, listener, port, clients, key_request, rec_pending, width, height, fps;
  int media_clients;
  int restart_request;
  int bitrate_kbps;
  int resume_request;
  int parked_listener;
  int profiles_enabled, profile_pending, profile_changing, profile_height, profile_error, profile_error_height;
  unsigned profile_mask;
  uint64_t generation;
  FILE *rec;
  int rec_headers_pending;
  uint64_t rec_frames, rec_bytes, rec_started;
  int rec_error, rec_width, rec_height, rec_fps;
  char rec_name[96];
  audio_chunk_t audio[AUDIO_SLOTS];
  uint64_t audio_seq, audio_bytes, audio_chunks;
  uint64_t audio_started;
  uint64_t audio_generation;
  int audio_channels, audio_rate;
  char audio_codec[16];
  pthread_t acceptor, workers[16];
  int acceptor_valid, worker_valid[16], worker_active[16], worker_fd[16];
} live = { .lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER, .listener = -1,
           .audio_channels = 2, .audio_rate = 48000 };

/* Wake periodically even when capture is suspended. A disconnected reader
 * must release its slot rather than waiting forever for another frame. */
static int wait_media(int fd, uint64_t generation, int audio) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec++;
  pthread_cond_timedwait(&live.changed, &live.lock, &deadline);
  char byte;
  ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
  return live.running && generation == (audio ? live.audio_generation : live.generation) &&
    !(n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR));
}
void pbs_h264_discontinuity(void) {
  pthread_mutex_lock(&live.lock);
  live.generation++;
  if (live.rec) live.rec_headers_pending = 1;
  live.key_request = 1;
  pthread_cond_broadcast(&live.changed);
  pthread_mutex_unlock(&live.lock);
}
int pbs_h264_take_restart_request(void) {
  pthread_mutex_lock(&live.lock);
  int requested = live.restart_request; live.restart_request = 0;
  pthread_mutex_unlock(&live.lock);
  return requested;
}
void pbs_h264_enable_profiles(void) {
  pbs_h264_enable_profile_mask(PBS_CAPTURE_PROFILES_ALL);
}
void pbs_h264_enable_profile_mask(unsigned mask) {
  pthread_mutex_lock(&live.lock);
  live.profiles_enabled=1; live.profile_mask=mask&PBS_CAPTURE_PROFILES_ALL;
  pthread_mutex_unlock(&live.lock);
}
int pbs_h264_take_profile_request(int *height) {
  pthread_mutex_lock(&live.lock);
  int requested = live.profile_pending;
  if (requested) { *height = live.profile_height; live.profile_pending = 0; }
  pthread_mutex_unlock(&live.lock);
  return requested;
}
void pbs_h264_profile_failed(void) {
  pthread_mutex_lock(&live.lock);
  live.profile_error = 1; live.profile_changing = live.profile_pending = 0;
  live.profile_error_height = live.profile_height;
  live.profile_height = live.height;
  pthread_mutex_unlock(&live.lock);
}
int pbs_h264_take_bitrate_request(void) {
  pthread_mutex_lock(&live.lock);
  int kbps = live.bitrate_kbps;
  pthread_mutex_unlock(&live.lock);
  return kbps;
}
void pbs_h264_request_keyframe(void) {
  pthread_mutex_lock(&live.lock);
  live.key_request = 1;
  pthread_mutex_unlock(&live.lock);
}
/* Parked state: after /stop the process stays alive and the HTTP listener
 * keeps serving status/config, but capture stays down until the web UI
 * posts /capture/start. Parked is not running: status.json reports
 * "stopped" so the viewer shows the idle banner instead of reconnecting. */
static int parked;
/* Screen confirmation uses the same notification channel main.c declares;
 * keep server.c self-contained with a weak local declaration. */
int sceKernelSendNotificationRequest(int, void *, size_t, int);
static void notify_park(int port) {
  typedef struct { char pad[45]; char message[3075]; } notify_request_t;
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message,
           "PBS parked\nResume from the web dashboard (port %d)", port);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
int pbs_h264_park_until_resume(int port) {
  parked = 1;
  notify_park(port);
  pthread_mutex_lock(&live.lock);
  while (!live.resume_request && live.parked_listener >= 0) {
    pthread_cond_wait(&live.changed, &live.lock);
  }
  int resume = live.resume_request;
  live.resume_request = 0;
  pthread_mutex_unlock(&live.lock);
  parked = 0;
  return resume ? 0 : -1;
}
int pbs_h264_is_parked(void) { return parked; }


static uint64_t milliseconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int send_all(int fd, const void *data, size_t size) {
  const unsigned char *p = data;
  while (size) {
    ssize_t sent = send(fd, p, size, 0);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return -1;
    p += sent; size -= sent;
  }
  return 0;
}
static int reply(int fd, const char *type, const void *data, size_t size) {
  char header[256];
  int n = snprintf(header, sizeof header, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", type, size);
  return send_all(fd, header, n) || send_all(fd, data, size);
}
static void profile_reply(int fd, int code, const char *message) {
  char body[192], header[256];
  int n = snprintf(body, sizeof body, "{\"message\":\"%s\"}", message);
  int h = snprintf(header, sizeof header, "HTTP/1.1 %d Profile\r\nContent-Type: application/json\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, n);
  send_all(fd, header, (size_t)h); send_all(fd, body, (size_t)n);
}
#include "common/http/controller_routes.h"
#include "common/system/power.h"
static void select_profile(int fd, const char *path) {
  int height = 0, consumed = 0, code = 200;
  const char *message = "Resolution change requested";
  if (sscanf(path, "/capture/profile?height=%d%n", &height, &consumed) != 1 ||
      path[consumed] || pbs_capture_profile_for_height(height, NULL)) {
    profile_reply(fd, 400, "Unsupported resolution"); return;
  }
  pthread_mutex_lock(&live.lock);
  if (!live.profiles_enabled) { code = 501; message = "This capture backend does not support resolution changes"; }
  else if (!(live.profile_mask & pbs_capture_profile_bit(height))) { code=422; message="This firmware does not support that capture resolution"; }
  else if (live.rec || live.rec_pending) { code = 409; message = "Stop recording before changing resolution"; }
  else if (live.profile_changing) { code = 409; message = "A resolution change is already in progress"; }
  else if (height == live.height) { message = "Resolution already active"; }
  else {
    live.profile_height = height; live.profile_pending = live.profile_changing = 1;
    live.profile_error = live.profile_error_height = 0; live.restart_request = 1;
  }
  pthread_mutex_unlock(&live.lock);
  profile_reply(fd, code, message);
}
/* Runtime bitrate ceiling. Applies live via sceAvcap2SetVideoBitrate on the
 * running session and to every later session restart, so the cap survives
 * resolution switches and stream recovery. 0 restores the startup rate. */
/* Resolution + bitrate in one request: the UI's Apply button must not restart
 * the capture twice, and a bitrate-only change needs a reopen too (the encoder
 * rate group is written at OpenVideo time). */
static void apply_profile_and_bitrate(int fd, const char *path) {
  int height = 0, kbps = -1, consumed = 0, code = 200;
  const char *message = "Applied";
  if (sscanf(path, "/capture/apply?height=%d&kbps=%d%n", &height, &kbps, &consumed) != 2 ||
      path[consumed]) {
    profile_reply(fd, 400, "Invalid apply request"); return;
  }
  if (pbs_capture_profile_for_height(height, NULL)) { profile_reply(fd, 400, "Unsupported resolution"); return; }
  if (kbps && (kbps < 1000 || kbps > 100000)) { profile_reply(fd, 422, "Bitrate out of range"); return; }
  pthread_mutex_lock(&live.lock);
  const int bitrate_changed = kbps != live.bitrate_kbps;
  const int height_changed = height != live.height;
  if (!live.profiles_enabled) { code = 501; message = "This capture backend does not support resolution changes"; }
  else if (live.rec || live.rec_pending) { code = 409; message = "Stop recording before changing the profile"; }
  else if (live.profile_changing) { code = 409; message = "A profile change is already in progress"; }
  else if (!bitrate_changed && !height_changed) { message = "Already active"; }
  else {
    live.bitrate_kbps = kbps;
    if (height_changed) { live.profile_height = height; live.profile_pending = live.profile_changing = 1;
                          live.profile_error = live.profile_error_height = 0; }
    live.restart_request = 1;
  }
  pthread_mutex_unlock(&live.lock);
  profile_reply(fd, code, message);
}
static void select_bitrate(int fd, const char *path) {
  int kbps = 0, consumed = 0, code = 200;
  const char *message = "Bitrate limit applied";
  if (sscanf(path, "/capture/bitrate?kbps=%d%n", &kbps, &consumed) != 1 || path[consumed]) {
    profile_reply(fd, 400, "Invalid bitrate"); return;
  }
  if (kbps && (kbps < 1000 || kbps > 100000)) {
    profile_reply(fd, 422, "Bitrate out of range (1000-100000 kbps)"); return;
  }
  pthread_mutex_lock(&live.lock);
  const int changed = kbps != live.bitrate_kbps;
  live.bitrate_kbps = kbps;
  /* SetVideoBitrate alone does not move the encoder: its rate group is fixed
   * at OpenVideo time. Reopen the capture so the new cap is written into the
   * request - but only when the value actually changed, otherwise every page
   * load would interrupt the stream for nothing. */
  if (changed) live.restart_request = 1;
  pthread_mutex_unlock(&live.lock);
  char body[128];
  int n = snprintf(body, sizeof body, "{\"kbps\":%d}", kbps);
  if (n > 0 && (size_t)n < sizeof body) reply(fd, "application/json", body, (size_t)n);
}
static int keyframe(const unsigned char *p, size_t size) {
  for (size_t i = 0; i + 4 < size; i++) {
    if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1 && (p[i+3] & 31) == 5) return 1;
  }
  return 0;
}
static int rec_headers(FILE *f) {
  const unsigned char start[4] = {0, 0, 0, 1};
  return fwrite(start, 1, 4, f) != 4 || fwrite(live.sps, 1, live.sps_len, f) != live.sps_len ||
         fwrite(start, 1, 4, f) != 4 || fwrite(live.pps, 1, live.pps_len, f) != live.pps_len;
}
/* Record stream metadata alongside the Annex B file, so a preview does not
 * borrow dimensions or timing from an unrelated current live profile. */
static void close_recording(void) {
  if (!live.rec) return;
  if (fclose(live.rec)) live.rec_error=1;
  live.rec=NULL;
  char path[512]; snprintf(path,sizeof path,"%s/%s.json",REC_DIR,live.rec_name);
  FILE *meta=fopen(path,"wb");
  if (meta) {
    fprintf(meta,"{\"width\":%d,\"height\":%d,\"fps\":%d,\"frames\":%llu}\n",
        live.rec_width,live.rec_height,live.rec_fps,(unsigned long long)live.rec_frames);
    fclose(meta);
  }
}
int pbs_h264_publish(const void *data, size_t length, uint32_t pts) {
  if (!length || length > FRAME_MAX) return -1;
  pthread_mutex_lock(&live.lock);
  uint64_t seq = live.seq + 1;
  frame_t *frame = &live.frames[seq % SLOTS];
  unsigned char *buffer = pbs_video_grow(frame->data, frame->capacity, length);
  if (!buffer) { pthread_mutex_unlock(&live.lock); return -1; }
  frame->data = buffer;
  if (length > frame->capacity) frame->capacity = pbs_video_capacity(length);
  memcpy(buffer, data, length);
  frame->len = length; frame->pts = pts; frame->seq = seq;
  frame->key = keyframe(data, length);
  if (frame->key) live.last_keyframe_ms = milliseconds();
  live.seq = seq; live.bytes += length;
  if (live.rec_pending && frame->key && !live.rec) {
    char path[512];
    snprintf(live.rec_name, sizeof live.rec_name, "pbs-%llu-%llu.h264", (unsigned long long)time(NULL), (unsigned long long)seq);
    snprintf(path, sizeof path, "%s/%s", REC_DIR, live.rec_name);
    int record_fd=storage_prepare(REC_DIR,1)?-1:open(path,O_WRONLY|O_CREAT|O_EXCL,0666);
    live.rec=record_fd<0?NULL:fdopen(record_fd,"wb");
    if (!live.rec && record_fd>=0) close(record_fd);
    live.rec_width=live.width; live.rec_height=live.height; live.rec_fps=live.fps;
    live.rec_pending = 0;
    live.rec_headers_pending = 0;
    live.rec_frames = live.rec_bytes = 0;
    live.rec_started = milliseconds();
    if (!live.rec || rec_headers(live.rec)) {
      close_recording();
      live.rec = NULL;
      live.rec_error = 1;
    }
  }
  /* An encoder restart changes the elementary stream's decoding context.
   * Do not append unreferenced P frames from the new session to a live file;
   * wait for its first IDR and insert current SPS/PPS ahead of that IDR. */
  if (live.rec && live.rec_headers_pending && frame->key) {
    if (rec_headers(live.rec)) {
      close_recording(); live.rec_error = 1;
    } else live.rec_headers_pending = 0;
  }
  if (live.rec && !live.rec_headers_pending) {
    if (fwrite(data, 1, length, live.rec) != length || fflush(live.rec)) {
      close_recording(); live.rec_error = 1;
    } else { live.rec_frames++; live.rec_bytes += length; }
  }
  pthread_cond_broadcast(&live.changed);
  pthread_mutex_unlock(&live.lock);
  return 0;
}
void pbs_audio_configure(const char *codec, int channels, int rate) {
  pthread_mutex_lock(&live.lock);
  if ((codec && strcmp(live.audio_codec, codec)) ||
      (channels > 0 && live.audio_channels != channels) || (rate > 0 && live.audio_rate != rate)) {
    live.audio_generation++;
    pthread_cond_broadcast(&live.changed);
  }
  if (codec) snprintf(live.audio_codec, sizeof live.audio_codec, "%s", codec);
  if (channels > 0) live.audio_channels = channels;
  if (rate > 0) live.audio_rate = rate;
  pthread_mutex_unlock(&live.lock);
}
int pbs_audio_publish(const void *data, size_t length, uint32_t pts) {
  if (!length || length > AUDIO_MAX) return -1;
  pthread_mutex_lock(&live.lock);
  uint64_t seq = live.audio_seq + 1;
  audio_chunk_t *chunk = &live.audio[seq % AUDIO_SLOTS];
  unsigned char *buffer = realloc(chunk->data, length);
  if (!buffer) { pthread_mutex_unlock(&live.lock); return -1; }
  chunk->data = buffer;
  memcpy(buffer, data, length);
  chunk->len = length; chunk->pts = pts; chunk->seq = seq;
  live.audio_seq = seq;
  live.audio_bytes += length;
  live.audio_chunks++;
  if (!live.audio_started) live.audio_started = milliseconds();
  pthread_cond_broadcast(&live.changed);
  pthread_mutex_unlock(&live.lock);
  return 0;
}
int pbs_h264_server_running(void) {
  pthread_mutex_lock(&live.lock); int result = live.running; pthread_mutex_unlock(&live.lock); return result;
}
int pbs_h264_server_port(void) {
  pthread_mutex_lock(&live.lock);
  int result = live.running ? live.port : 0;
  pthread_mutex_unlock(&live.lock);
  return result;
}
int pbs_h264_take_keyframe_request(void) {
  pthread_mutex_lock(&live.lock);
  uint64_t now = milliseconds();
  /* Refresh the encoder even for clients that never POST /video/keyframe
   * (including native players and recording-only sessions). Static scenes
   * must not depend on motion or a browser watchdog to end a long GOP.
   * Track received IDRs, not requests: a refused request is retried, while
   * natural encoder IDRs postpone the next periodic request. */
  int periodic = live.running &&
      now - live.last_keyframe_ms >= KEYFRAME_INTERVAL_MS &&
      now - live.last_key_request_ms >= KEYFRAME_INTERVAL_MS;
  /* Explicit requests (viewer watchdog, resync after a ring overwrite) used to
   * bypass the interval, so three viewers could each ask on every stall and the
   * encoder answered with an IDR per frame - a 1.5k-request storm and a constant
   * bitrate spike at low caps. Honour at most one per second. */
  int explicit_ok = live.key_request && (now - live.last_key_request_ms >= 1000u);
  int result = explicit_ok || periodic;
  live.key_request = 0;
  if (result) live.last_key_request_ms = now;
  pthread_mutex_unlock(&live.lock);
  return result;
}
void pbs_h264_server_stop(void) {
  pthread_mutex_lock(&live.lock);
  live.running = 0;
  live.port = 0;
  live.rec_pending = 0;
  live.rec_headers_pending = 0;
  close_recording();
  if (live.listener >= 0) shutdown(live.listener, SHUT_RDWR);
  /* The accept socket is not the only blocking descriptor: a client may be
   * inside recv() on a partial HTTP header, or send() on a stalled viewer.
   * Wake every owned worker before joining it. Worker descriptors are closed
   * under this same mutex so fd reuse cannot redirect shutdown elsewhere. */
  for (int i = 0; i < 16; ++i)
    if (live.worker_active[i] && live.worker_fd[i] >= 0)
      shutdown(live.worker_fd[i], SHUT_RDWR);
  pthread_cond_broadcast(&live.changed);
  pthread_mutex_unlock(&live.lock);
  pbs_input_stop();
}
static void stream(int fd) {
  HTTP_TRACE("stream entered\n");
  const char *header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (send_all(fd, header, strlen(header))) { HTTP_TRACE("stream header send failed\n"); return; }
  unsigned char *copy = NULL;
  size_t capacity = 0;
  pthread_mutex_lock(&live.lock);
  uint64_t next = live.seq + 1;
  uint64_t generation = live.generation;
  live.key_request = 1;
  uint64_t last_key_request = milliseconds();
  int synced = 0;
  while (live.running && generation == live.generation) {
    /* A failed or missed encoder IDR request must not strand the viewer. */
    uint64_t now = milliseconds();
    if (!synced && now - last_key_request >= 2000) {
      live.key_request = 1;
      last_key_request = now;
    }
    int alive = 1;
    while (alive && live.running && next > live.seq) alive = wait_media(fd, generation, 0);
    if (!alive || !live.running || generation != live.generation) { HTTP_TRACE("stream reader no longer alive\n"); break; }
    /* A short socket stall does not invalidate frames still in the ring.
     * Dropping at six frames (100 ms at 60 fps) needlessly broke reference
     * chains and triggered repeated expensive IDRs during menu movement.
     * Resynchronize only once the producer actually overwrites unread data. */
    if (live.seq - next >= SLOTS) { next = live.seq; synced = 0; live.key_request = 1; }
    frame_t *f = &live.frames[next % SLOTS];
    next++;
    if (f->seq != next - 1 || (!synced && !f->key)) continue;
    synced = 1;
    size_t size = f->len;
    unsigned char *grown = pbs_video_grow(copy, capacity, size + 12);
    if (!grown) { HTTP_TRACE("stream allocation failed\n"); break; }
    copy = grown;
    if (size + 12 > capacity) capacity = pbs_video_capacity(size + 12);
    /* 12-byte header: payload size, 90 kHz capture timestamp, server send time
     * in monotonic milliseconds (same clock as status.json now_ms) so the
     * viewer can measure one-way delay instead of guessing from the buffer. */
    uint32_t packet[3] = {htonl((uint32_t)size), htonl(f->pts), htonl((uint32_t)milliseconds())};
    memcpy(copy, packet, sizeof packet);
    memcpy(copy + sizeof packet, f->data, size);
    pthread_mutex_unlock(&live.lock);
    int error = send_all(fd, copy, sizeof packet + size);
    pthread_mutex_lock(&live.lock);
    if (error) break;
  }
  pthread_mutex_unlock(&live.lock);
  pbs_video_free(copy, capacity);
}
static void audio_stream(int fd) {
  const char *header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (send_all(fd, header, strlen(header))) return;
  unsigned char *copy = malloc(AUDIO_MAX + 8);
  if (!copy) return;
  pthread_mutex_lock(&live.lock);
  uint64_t next = live.audio_seq + 1; /* Start with the first newly published block. */
  uint64_t generation = live.audio_generation;
  while (live.running && generation == live.audio_generation) {
    int alive = 1;
    while (alive && live.running && next > live.audio_seq) alive = wait_media(fd, generation, 1);
    if (!alive || !live.running || generation != live.audio_generation) break;
    if (live.audio_seq - next >= 4) { next = live.audio_seq; continue; }
    audio_chunk_t *chunk = &live.audio[next % AUDIO_SLOTS];
    next++;
    if (chunk->seq != next - 1) continue;
    size_t size = chunk->len;
    uint32_t packet[2] = {htonl((uint32_t)size), htonl(chunk->pts)};
    memcpy(copy, packet, sizeof packet);
    memcpy(copy + sizeof packet, chunk->data, size);
    pthread_mutex_unlock(&live.lock);
    int error = send_all(fd, copy, sizeof packet + size);
    pthread_mutex_lock(&live.lock);
    if (error) break;
  }
  pthread_mutex_unlock(&live.lock);
  free(copy);
}
struct rtp_target { int socket; struct sockaddr_in address; };
static int rtp_send(void *ctx, const unsigned char *packet, size_t size) {
  struct rtp_target *target = ctx;
  return sendto(target->socket, packet, size, 0, (struct sockaddr *)&target->address,
                sizeof target->address) == (ssize_t)size ? 0 : -1;
}
static void rtp_stream(int fd, int port) {
  struct rtp_target target;
  socklen_t addr_size = sizeof target.address;
  if (getpeername(fd,(struct sockaddr *)&target.address,&addr_size)) return;
  target.address.sin_port = htons(port);
  target.socket = socket(AF_INET,SOCK_DGRAM,0);
  if (target.socket < 0) return;
  const char *header = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nConnection: close\r\n\r\nRTP\n";
  if (send_all(fd,header,strlen(header))) { close(target.socket); return; }
  unsigned char *copy = NULL;
  size_t capacity = 0;
  uint16_t sequence = 0;
  uint32_t ssrc = (uint32_t)(milliseconds() ^ (uint64_t)fd * 0x9e3779b9u);
  uint64_t heartbeat = milliseconds();
  pthread_mutex_lock(&live.lock);
  uint64_t next=live.seq+1, generation=live.generation;
  int synced=0; live.key_request=1;
  while(live.running && generation==live.generation) {
    int alive=1;
    while(alive && live.running && next>live.seq) alive=wait_media(fd,generation,0);
    if(!alive || !live.running || generation!=live.generation)break;
    if(live.seq-next>=PBS_H264_RTP_MAX_BACKLOG){next=live.seq;synced=0;live.key_request=1;}
    frame_t *frame=&live.frames[next++%SLOTS];
    if(frame->seq!=next-1 || (!synced && !frame->key))continue;
    size_t prefix = synced ? 0 : 8 + live.sps_len + live.pps_len;
    synced=1; size_t size=frame->len+prefix; uint32_t pts=frame->pts;
    unsigned char *grown = pbs_video_grow(copy, capacity, size);
    if (!grown) break;
    copy = grown;
    if (size > capacity) capacity = pbs_video_capacity(size);
    if (prefix) {
      const unsigned char start[4] = {0, 0, 0, 1};
      memcpy(copy, start, 4);
      memcpy(copy + 4, live.sps, live.sps_len);
      size_t pps_at = 4 + live.sps_len;
      memcpy(copy + pps_at, start, 4);
      memcpy(copy + pps_at + 4, live.pps, live.pps_len);
    }
    memcpy(copy+prefix,frame->data,frame->len);
    pthread_mutex_unlock(&live.lock);
    int error=pbs_rtp_frame(copy,size,pts,ssrc,&sequence,rtp_send,&target);
    if(milliseconds()-heartbeat>=500){error|=send_all(fd,".\n",2);heartbeat=milliseconds();}
    pthread_mutex_lock(&live.lock);
    if(error)break;
  }
  pthread_mutex_unlock(&live.lock);
  pbs_video_free(copy, capacity);close(target.socket);
}
static void configuration(int fd) {
  char sps[257], pps[257], body[1536], audio[160], resolutions[64];
#ifdef PBS_PS4
  const char *platform = "PS4";
#else
  const char *platform = "PS5";
#endif
  pthread_mutex_lock(&live.lock);
  size_t used=1;resolutions[0]='[';
  const int heights[]={720,1080,1440,2160};
  for (unsigned i=0;i<4;i++) if (live.profiles_enabled && (live.profile_mask&pbs_capture_profile_bit(heights[i])))
    used+=(size_t)snprintf(resolutions+used,sizeof resolutions-used,"%s%d",used>1?",":"",heights[i]);
  resolutions[used++]=']';resolutions[used]=0;
  for (size_t i = 0; i < live.sps_len; i++) snprintf(sps + i*2, 3, "%02x", live.sps[i]);
  for (size_t i = 0; i < live.pps_len; i++) snprintf(pps + i*2, 3, "%02x", live.pps[i]);
  if (!strcmp(live.audio_codec, "none")) snprintf(audio, sizeof audio, "null");
  else snprintf(audio, sizeof audio, "{\"codec\":\"%s\",\"channels\":%d,\"rate\":%d,\"bits\":32}",
      live.audio_codec[0] ? live.audio_codec : "pcm_f32le", live.audio_channels, live.audio_rate);
  int n = snprintf(body, sizeof body, "{\"platform\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":%d,\"codec\":\"avc1.%02x%02x%02x\",\"sps\":\"%s\",\"pps\":\"%s\",\"audio\":%s,\"resolutions\":%s,\"cap_kbps\":%d,\"requested_height\":%d,\"profile_changing\":%s,\"profile_error\":%s,\"profile_error_height\":%d}", platform, live.width, live.height, live.fps, live.sps[1], live.sps[2], live.sps[3], sps, pps, audio,
      resolutions, live.bitrate_kbps, live.profile_changing ? live.profile_height : live.height,
      live.profile_changing ? "true" : "false", live.profile_error ? "true" : "false", live.profile_error_height);
  pthread_mutex_unlock(&live.lock);
  if (n > 0 && (size_t)n < sizeof body) reply(fd, "application/json", body, (size_t)n);
}
static void status(int fd) {
  char body[1024];
  pthread_mutex_lock(&live.lock);
  double seconds = (milliseconds() - live.started) / 1000.0;
  int n = snprintf(body, sizeof body, "{\"backend\":\"h264\",\"now_ms\":%llu,\"parked\":%s,\"width\":%d,\"height\":%d,\"frames\":%llu,\"bytes\":%llu,\"fps\":%.1f,\"kbps\":%.1f,\"clients\":%d,\"recording\":%s,\"recording_name\":\"%s\",\"recording_frames\":%llu,\"recording_bytes\":%llu,\"recording_seconds\":%.1f,\"recording_error\":%s,\"audio_chunks\":%llu,\"audio_bytes\":%llu,\"audio_kbps\":%.1f,\"audio_codec\":\"%s\"}",
      (unsigned long long)(uint32_t)milliseconds(), parked ? "true" : "false", live.width, live.height, (unsigned long long)live.seq, (unsigned long long)live.bytes, seconds > 0 ? live.seq/seconds : 0,
      seconds > 0 ? live.bytes*8/seconds/1000 : 0, live.clients, live.rec || live.rec_pending ? "true" : "false", live.rec_name,
      (unsigned long long)live.rec_frames, (unsigned long long)live.rec_bytes,
      live.fps > 0 ? (double)live.rec_frames/live.fps : 0, live.rec_error ? "true" : "false",
      (unsigned long long)live.audio_chunks, (unsigned long long)live.audio_bytes,
      live.audio_started ? (double)live.audio_bytes * 8 / ((milliseconds() - live.audio_started) / 1000.0) / 1000 : 0,
      live.audio_codec[0] ? live.audio_codec : "pcm_f32le");
  pthread_mutex_unlock(&live.lock);
  reply(fd, "application/json", body, n);
}
static int valid_recording_name(const char *name) {
  size_t length = strlen(name);
  if (length < 6 || length >= 96 || name[0] == '.' || strcmp(name + length - 5, ".h264")) return 0;
  for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.') return 0;
  return !strstr(name, "..");
}
static int recording_reference(const char *input,char name[96],char directory[PBS_STORAGE_PATH]) {
  const char *query=strchr(input,'?');size_t size=query?(size_t)(query-input):strlen(input);
  if (!size || size>=96) return -1;
  memcpy(name,input,size);name[size]=0;directory[0]=0;
  if (query && (strncmp(query,"?directory=",11) ||
      storage_decode(query+11,directory,PBS_STORAGE_PATH) || !directory[0])) return -1;
  return valid_recording_name(name)?0:-1;
}
static void recording_settings(int fd,const char *encoded) {
  char path[PBS_STORAGE_PATH],body[512]; int code=200;
  const char *message="Recording directory saved";
  if (encoded && (storage_decode(encoded,path,sizeof path) || !storage_path_valid(path))) {
    profile_reply(fd,400,"Use an absolute directory inside /data or a mounted USB drive"); return;
  }
  pthread_mutex_lock(&live.lock); storage_load();
  if (encoded) {
    if (live.rec || live.rec_pending) { code=409; message="Stop recording before changing its destination"; }
    else if (storage_prepare(path,1)) { code=400; message="Destination unavailable; check the mount and directory"; }
    else if (storage_save(path)) { code=500; message="Unable to save the recording directory"; }
    else if (strcmp(REC_DIR,path)) {
      strcpy(REC_DIR,path); live.rec_name[0]=0; live.rec_frames=live.rec_bytes=0;
    }
  }
  int n=snprintf(body,sizeof body,"{\"directory\":\"%s\",\"recording\":%s}",REC_DIR,
      live.rec || live.rec_pending?"true":"false");
  pthread_mutex_unlock(&live.lock);
  if (encoded) profile_reply(fd,code,message); else reply(fd,"application/json",body,(size_t)n);
}
static void delete_recording(int fd,const char *input) {
  char name[96],directory[PBS_STORAGE_PATH];
  if (recording_reference(input,name,directory)) { profile_reply(fd,400,"Invalid recording name"); return; }
  pthread_mutex_lock(&live.lock); storage_load();
  int code=200; const char *message="Recording deleted";
  char path[512]; struct stat st;
  snprintf(path,sizeof path,"%s/%.95s",REC_DIR,name);
  if (directory[0] && strcmp(directory,REC_DIR)) { code=409; message="Recording destination changed; reload the list"; }
  else if (live.rec && !strcmp(name,live.rec_name)) { code=409; message="Stop this recording before deleting it"; }
  else if (lstat(path,&st) || !S_ISREG(st.st_mode)) { code=404; message="Recording not found"; }
  else if (unlink(path)) { code=500; message="Unable to delete recording"; }
  else {
    snprintf(path,sizeof path,"%s/%.95s.json",REC_DIR,name); unlink(path);
    if (!strcmp(live.rec_name,name)) live.rec_name[0]=0;
  }
  pthread_mutex_unlock(&live.lock); profile_reply(fd,code,message);
}
static void recording_info(int fd,const char *name) {
  if (!valid_recording_name(name)) { profile_reply(fd,400,"Invalid recording name"); return; }
  char path[512],body[256];
  pthread_mutex_lock(&live.lock); storage_load();
  if (live.rec && !strcmp(name,live.rec_name)) {
    pthread_mutex_unlock(&live.lock); profile_reply(fd,409,"Stop recording before previewing it"); return;
  }
  snprintf(path,sizeof path,"%s/%.95s.json",REC_DIR,name);
  pthread_mutex_unlock(&live.lock);
  FILE *f=fopen(path,"rb");
  if (!f) { reply(fd,"application/json","{}",2); return; }
  size_t n=fread(body,1,sizeof body,f); int valid=n<sizeof body && !ferror(f); fclose(f);
  if (valid) reply(fd,"application/json",body,n);
  else profile_reply(fd,400,"Recording metadata unavailable");
}
static void recordings(int fd) {
  char body[16384]; size_t used = 1; body[0] = '[';
  char directory[PBS_STORAGE_PATH],active[96];
  pthread_mutex_lock(&live.lock); storage_load(); strcpy(directory,REC_DIR);
  snprintf(active,sizeof active,"%s",live.rec?live.rec_name:"");
  pthread_mutex_unlock(&live.lock);
  DIR *dir = opendir(directory);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) && used < sizeof body - 256) {
      if (!valid_recording_name(entry->d_name)) continue;
      char path[512]; struct stat st;
      snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
      if (lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
      used += snprintf(body + used, sizeof body - used, "%s{\"name\":\"%s\",\"bytes\":%llu,\"active\":%s}", used > 1 ? "," : "", entry->d_name, (unsigned long long)st.st_size, !strcmp(active,entry->d_name)?"true":"false");
    }
    closedir(dir);
  }
  body[used++] = ']';
  reply(fd, "application/json", body, used);
}
static void download(int fd, const char *input,const char *range) {
  char name[96],directory[PBS_STORAGE_PATH];
  if (recording_reference(input,name,directory)) { profile_reply(fd,400,"Invalid recording name"); return; }
  char path[512];
  pthread_mutex_lock(&live.lock); storage_load();
  if (directory[0] && strcmp(directory,REC_DIR)) {
    pthread_mutex_unlock(&live.lock); profile_reply(fd,409,"Recording destination changed; reload the list"); return;
  }
  snprintf(path, sizeof path, "%s/%.95s", REC_DIR, name);
  pthread_mutex_unlock(&live.lock);
  FILE *file = fopen(path, "rb"); if (!file) { profile_reply(fd,404,"Recording not found"); return; }
  struct stat st;
  if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode)) { fclose(file); return; }
  uint64_t first,last; int partial=recording_range(range,(uint64_t)st.st_size,&first,&last);
  char header[512]; int n;
  if (partial<0) {
    n=snprintf(header,sizeof header,"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",(unsigned long long)st.st_size);
    send_all(fd,header,(size_t)n); fclose(file); return;
  }
  uint64_t length=st.st_size?(last-first+1):0;
  if (fseeko(file,(off_t)first,SEEK_SET)) { fclose(file); return; }
  char content_range[128]="";
  if (partial) snprintf(content_range,sizeof content_range,"Content-Range: bytes %llu-%llu/%llu\r\n",(unsigned long long)first,(unsigned long long)last,(unsigned long long)st.st_size);
  n = snprintf(header, sizeof header, "HTTP/1.1 %s\r\nContent-Type: video/h264\r\nContent-Disposition: attachment; filename=\"%s\"\r\nAccept-Ranges: bytes\r\n%sContent-Length: %llu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",partial?"206 Partial Content":"200 OK",name,content_range,(unsigned long long)length);
  if (!send_all(fd, header, n)) {
    unsigned char buf[16384]; size_t got; uint64_t left = length;
    while (left && (got = fread(buf, 1, left < sizeof buf ? (size_t)left : sizeof buf, file))) {
      if (send_all(fd, buf, got)) break;
      left -= got;
    }
  }
  fclose(file);
}
typedef struct { int fd, slot; } client_arg_t;
static void *client_thread(void *arg) {
  client_arg_t client = *(client_arg_t *)arg; free(arg);
  int fd = client.fd;
  int media = 0;
  int no_delay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof no_delay);
  int send_buffer = 65536;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof send_buffer);
  struct timeval timeout = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
  struct timeval header_timeout = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &header_timeout, sizeof header_timeout);
  char request[4096], method[12], path[1024]; size_t used = 0;
  uint64_t header_started = milliseconds();
  while (used < sizeof request - 1 && milliseconds() - header_started < 5000) {
    ssize_t got = recv(fd, request + used, sizeof request - used - 1, 0);
    if (got <= 0) goto done;
    used += got; request[used] = 0;
    if (strstr(request, "\r\n\r\n")) break;
  }
  if (!strstr(request, "\r\n\r\n")) goto done;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  if (sscanf(request, "%11s %1023s", method, path) != 2) goto done;
  char range[128]="";
  for (const char *line=strstr(request,"\r\n");line && line[2];line=strstr(line+2,"\r\n")) {
    if (!strncasecmp(line+2,"Range:",6)) {
      const char *start=line+8; while (*start==' ' || *start=='\t') start++;
      const char *end=strstr(start,"\r\n"); if (!end || (size_t)(end-start)>=sizeof range) goto done;
      memcpy(range,start,(size_t)(end-start)); range[end-start]=0; break;
    }
  }
  int rtp_port = 0, consumed = 0;
  if (sscanf(path, "/rtp/video?port=%d%n", &rtp_port, &consumed) != 1 || path[consumed] || rtp_port < 1024 || rtp_port > 65534) rtp_port = 0;
  if (!strcmp(path, "/frames") || !strcmp(path, "/audio") || rtp_port) {
    pthread_mutex_lock(&live.lock);
    media = live.media_clients < 8;
    if (media) live.media_clients++;
    pthread_mutex_unlock(&live.lock);
    if (!media) { const char busy[] = "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; send_all(fd, busy, sizeof busy - 1); goto done; }
  }
  if (!strcmp(method, "GET") && !strcmp(path, "/")) reply(fd, "text/html; charset=utf-8", PBS_H264_HTML, sizeof PBS_H264_HTML - 1);
  else if (!strcmp(method, "GET") && !strcmp(path, "/banner.png")) {
    /* The banner is optional branding: without it the viewer drops the corner
     * logo on its own, so answer with a 1x1 transparent PNG (a 404 here just
     * adds a failed-request line to every browser console). */
    if (PBS_BANNER_LEN) reply(fd, "image/png", PBS_BANNER_BYTES, PBS_BANNER_LEN);
    else {
      /* No banner embedded in this build. Answer with a 1x1 transparent PNG
       * instead of a 404: the viewer drops the <img> on error, but a 404 shows
       * up in every browser console as a failed request. */
      static const unsigned char blank[] = {
          0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
          0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,
          0x89,0x00,0x00,0x00,0x0a,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x00,0x01,0x00,0x00,
          0x05,0x00,0x01,0x0d,0x0a,0x2d,0xb4,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,
          0x42,0x60,0x82};
      reply(fd, "image/png", (const char *)blank, sizeof blank);
    }
  }
  else if (!strncmp(path,"/input/",7)) input_route(fd,method,path,request,used);
  else if (!strcmp(method, "GET") && !strcmp(path, "/config.json")) configuration(fd);
  else if (!strcmp(method, "GET") && !strcmp(path, "/status.json")) status(fd);
  else if (!strcmp(method, "GET") && !strcmp(path, "/frames")) stream(fd);
  else if (!strcmp(method, "GET") && rtp_port) rtp_stream(fd,rtp_port);
  else if (!strcmp(method, "GET") && !strcmp(path, "/audio")) audio_stream(fd);
  else if (!strcmp(method,"GET") && !strcmp(path,"/recording/settings")) recording_settings(fd,NULL);
  else if (!strcmp(method,"POST") && !strncmp(path,"/recording/settings?path=",25)) recording_settings(fd,path+25);
  else if (!strcmp(method,"DELETE") && !strncmp(path,"/recordings/",12)) delete_recording(fd,path+12);
  else if (!strcmp(method,"GET") && !strncmp(path,"/recordings/info?name=",22)) recording_info(fd,path+22);
  else if (!strcmp(method, "GET") && !strcmp(path, "/recordings")) recordings(fd);
  else if (!strcmp(method, "GET") && !strncmp(path, "/recordings/", 12)) download(fd, path + 12,range);
  else if (!strcmp(method, "POST") && !strcmp(path, "/record/start")) {
    pthread_mutex_lock(&live.lock);
    storage_load();
    if (live.profile_changing) {
      pthread_mutex_unlock(&live.lock);
      profile_reply(fd, 409, "Wait for the resolution change before recording");
      goto done;
    }
    if (!live.rec && !live.rec_pending) {
      live.rec_pending = 1; live.key_request = 1; live.rec_error = 0;
      live.rec_frames = live.rec_bytes = 0;
    }
    pthread_mutex_unlock(&live.lock);
    reply(fd, "application/json", "{}", 2);
  } else if (!strcmp(method, "POST") && !strcmp(path, "/record/stop")) {
    pthread_mutex_lock(&live.lock);
    live.rec_pending = 0;
    live.rec_headers_pending = 0;
    close_recording();
    pthread_mutex_unlock(&live.lock);
    reply(fd, "application/json", "{}", 2);
  } else if (!strcmp(method, "POST") && !strcmp(path, "/video/keyframe")) {
    pthread_mutex_lock(&live.lock); live.key_request = 1; pthread_mutex_unlock(&live.lock);
    reply(fd, "application/json", "{}", 2);
  } else if (!strcmp(method, "POST") && !strncmp(path, "/capture/apply", 14)) {
    apply_profile_and_bitrate(fd, path);
  } else if (!strcmp(method, "POST") && !strncmp(path, "/capture/profile", 16)) {
    select_profile(fd, path);
  } else if (!strcmp(method, "POST") && !strncmp(path, "/capture/bitrate", 16)) {
    select_bitrate(fd, path);
  } else if (!strcmp(method, "GET") && !strcmp(path, "/capture/bitrate")) {
    char body[64];
    pthread_mutex_lock(&live.lock);
    int n = snprintf(body, sizeof body, "{\"kbps\":%d}", live.bitrate_kbps);
    pthread_mutex_unlock(&live.lock);
    if (n > 0 && (size_t)n < sizeof body) reply(fd, "application/json", body, (size_t)n);
  } else if (!strcmp(method, "POST") && !strcmp(path, "/capture/restart")) {
    pthread_mutex_lock(&live.lock); live.restart_request = 1; pthread_mutex_unlock(&live.lock);
    reply(fd, "application/json", "{}", 2);
  } else if (!strcmp(method, "POST") && !strcmp(path, "/stop")) {
    reply(fd, "application/json", "{}", 2);
    pbs_h264_server_stop();
  } else if (!strcmp(method, "POST") &&
             (!strcmp(path, "/system/standby") || !strcmp(path, "/system/reboot") ||
              !strcmp(path, "/system/poweroff"))) {
    /* Answer first: every one of these tears the network down. */
    int action = path[8] == 's' ? PBS_POWER_STANDBY
                 : path[8] == 'r' ? PBS_POWER_REBOOT
                                  : PBS_POWER_OFF;
    reply(fd, "application/json", "{\"ok\":true}", 11);
    pbs_power_request(action);
  } else if (!strcmp(method, "POST") && !strcmp(path, "/capture/start")) {
    /* Wake the parked payload: the capture thread reopens the session. */
    pthread_mutex_lock(&live.lock);
    live.resume_request = 1;
    pthread_cond_broadcast(&live.changed);
    pthread_mutex_unlock(&live.lock);
    reply(fd, "application/json", "{}", 2);
  } else reply(fd, "text/plain", "Unavailable route", 17);
done:
  pthread_mutex_lock(&live.lock);
  /* Own close under the same lock as server_stop's shutdown loop; do not
   * leave a stale numeric fd that another connection could have reused. */
  close(fd);
  live.worker_fd[client.slot] = -1;
  live.clients--; if (media) live.media_clients--;
  live.worker_active[client.slot] = 0;
  pthread_mutex_unlock(&live.lock);
  return NULL;
}
static void *accept_thread(void *arg) {
  (void)arg;
  while (pbs_h264_server_running()) {
    int fd = accept(live.listener, NULL, NULL);
    if (fd < 0) { if (errno == EINTR) continue; break; }
    pthread_mutex_lock(&live.lock);
    int slot = -1;
    for (int i = 0; i < 16; ++i) if (!live.worker_active[i]) { slot = i; break; }
    int allowed = slot >= 0 && live.running;
    if (allowed) { live.clients++; live.worker_active[slot] = 1; live.worker_fd[slot] = fd; }
    pthread_mutex_unlock(&live.lock);
    if (!allowed) { close(fd); continue; }
    if (live.worker_valid[slot]) { pthread_join(live.workers[slot], NULL); live.worker_valid[slot] = 0; }
    client_arg_t *value = malloc(sizeof *value);
    if (value) *value = (client_arg_t){fd, slot};
    if (!value || pthread_create(&live.workers[slot], NULL, client_thread, value)) {
      free(value);
      pthread_mutex_lock(&live.lock);
      close(fd); live.worker_fd[slot] = -1;
      live.clients--; live.worker_active[slot] = 0;
      pthread_mutex_unlock(&live.lock);
    } else live.worker_valid[slot] = 1;
  }
  pthread_mutex_lock(&live.lock);
  int listener = live.listener;
  live.listener = -1;
  pthread_mutex_unlock(&live.lock);
  if (listener >= 0) close(listener);
  return NULL;
}
void pbs_h264_server_wait(void) {
  pbs_input_wait();
  if (live.acceptor_valid) { pthread_join(live.acceptor, NULL); live.acceptor_valid = 0; }
  for (int i = 0; i < 16; ++i) {
    if (live.worker_valid[i]) { pthread_join(live.workers[i], NULL); live.worker_valid[i] = 0; }
  }
  for (int i = 0; i < SLOTS; ++i) {
    pbs_video_free(live.frames[i].data, live.frames[i].capacity);
    live.frames[i].data = NULL; live.frames[i].capacity = 0;
  }
  for (int i = 0; i < AUDIO_SLOTS; ++i) { free(live.audio[i].data); live.audio[i].data = NULL; }
}
int pbs_h264_server_start(int port, int width, int height, const unsigned char *sps,
    size_t sps_len, const unsigned char *pps, size_t pps_len) {
  return pbs_h264_server_start_at_rate(port, width, height, 60, sps, sps_len, pps, pps_len);
}
int pbs_h264_server_start_at_rate(int port, int width, int height, int fps,
    const unsigned char *sps, size_t sps_len, const unsigned char *pps, size_t pps_len) {
  if (fps != 30 && fps != 60) return -1;
  if (sps_len < 4 || sps_len > 128 || !pps_len || pps_len > 128 || port < 1 || port > 65535) return -1;
  pthread_mutex_lock(&live.lock);
  if (live.running) {
    /* A new encoder cannot publish packets left in the previous profile's
     * ring. Invalidate both media histories before installing its headers. */
    for (int i = 0; i < SLOTS; ++i) { live.frames[i].seq = 0; live.frames[i].len = 0; live.frames[i].key = 0; }
    for (int i = 0; i < AUDIO_SLOTS; ++i) { live.audio[i].seq = 0; live.audio[i].len = 0; }
    live.seq = live.bytes = live.audio_seq = live.audio_bytes = live.audio_chunks = 0;
    live.started = milliseconds(); live.audio_started = 0; live.audio_generation++;
    live.last_keyframe_ms = live.last_key_request_ms = live.started;
    live.width = width; live.height = height; live.fps = fps;
    if (live.profile_changing && live.profile_height == height) live.profile_changing = 0;
    memcpy(live.sps, sps, sps_len); live.sps_len = sps_len;
    memcpy(live.pps, pps, pps_len); live.pps_len = pps_len;
    live.generation++; live.key_request = 1;
    if (live.rec) live.rec_headers_pending = 1;
    pthread_cond_broadcast(&live.changed);
    pthread_mutex_unlock(&live.lock);
    return 0;
  }
  pthread_mutex_unlock(&live.lock);
  signal(SIGPIPE, SIG_IGN);
  /* A port owned by another instance must not abort the session: walk the
   * scan window and keep the first port that binds. SO_REUSEADDR covers
   * TIME_WAIT, not an active listener, so a real conflict still fails. */
  int fd = -1, bound_port = 0, last_error = 0;
  for (int candidate = port; candidate < port + PBS_H264_PORT_SCAN && candidate <= 65535; ++candidate) {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) { last_error = errno; break; }
    int yes = 1; setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(candidate),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (!bind(probe, (struct sockaddr *)&addr, sizeof addr) && !listen(probe, 8)) {
      fd = probe; bound_port = candidate; break;
    }
    last_error = errno;
    close(probe);
  }
  if (fd < 0) { errno = last_error; return -1; }
  live.listener = fd; live.port = bound_port; live.running = 1; live.started = milliseconds();
  live.last_keyframe_ms = live.last_key_request_ms = live.started;
  live.key_request = 1;
  live.width = width; live.height = height; live.fps = fps;
  memcpy(live.sps, sps, sps_len); live.sps_len = sps_len;
  memcpy(live.pps, pps, pps_len); live.pps_len = pps_len;
  if (pthread_create(&live.acceptor, NULL, accept_thread, NULL)) { live.running = 0; close(fd); live.listener = -1; return -1; }
  live.acceptor_valid = 1;
  (void)pbs_input_start();
  return 0;
}

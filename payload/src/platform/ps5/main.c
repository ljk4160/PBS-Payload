// SPDX-License-Identifier: GPL-3.0-or-later
/* PBS streaming payload. Research commands remain in the legacy repository. */
#define PBS_DATA_DIR "/data/pbs"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ps5/kernel.h>



#include "common/http/server.h"
#include "common/input/controller.h"
#include "common/media/profiles.h"
#include "common/process/inject.h"
#include "platform/ps5/capture_fallback.h"
#include "platform/ps5/capture_resolution.h"
#include "platform/ps5/capture_audio_request.h"
#include "platform/ps5/capture_cleanup.h"
#include "platform/ps5/offset_dump.h"
#include "platform/ps5/offset/profile.h"
#include "platform/ps5/module_dump.h"
#include "platform/ps5/controller.h"
#include "platform/ps5/request_template.h"
#include "platform/ps5/worker.h"

#define PBS_PS5_RUNTIME_REVISION "native-request-v3"


/* An interrupted remote call may still have acquired resources. Do not issue
 * further remote calls or retry capture until that outcome is known. The
 * streaming loop and its cleanup execute serially on the capture thread. */
static int capture_call_uncertain;
static unsigned capture_audio_unclosed;

static long capture_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  if (capture_call_uncertain || ctx->asserted) return LONG_MIN;
  long rc = pbs_inject_call_fast(ctx, fn, args);
  if (rc == LONG_MIN || ctx->asserted) capture_call_uncertain = 1;
  return rc;
}

static long capture_call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  if (capture_call_uncertain || ctx->asserted) return LONG_MIN;
  long rc = pbs_inject_call(ctx, fn, args);
  if (rc == LONG_MIN || ctx->asserted) capture_call_uncertain = 1;
  return rc;
}

static long capture_call_fast_fp(pbs_inject_t *ctx, intptr_t fn,
                                const uint64_t args[6], float value) {
  if (capture_call_uncertain || ctx->asserted) return LONG_MIN;
  long rc = pbs_inject_call_fast_fp(ctx, fn, args, value);
  if (rc == LONG_MIN || ctx->asserted) capture_call_uncertain = 1;
  return rc;
}

typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, uint32_t *);

static int notify(const char *text) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  strncpy(req.message, text, sizeof req.message - 1);
  return sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static FILE *open_report(const char *name, const char *mode) {
  char path[160];
  snprintf(path, sizeof path, "%s/%s", PBS_DATA_DIR, name);
  FILE *f = fopen(path, mode);
  if (f) setvbuf(f, NULL, _IONBF, 0);
  return f;
}

static unsigned char scratch_zero[4096];
static uint32_t requested_video_bitrate;
static uint32_t requested_video_budget;
static int native_rate_config;
static uint32_t requested_min_qp = 35;
static unsigned stress_keyframe_period;
static int stress_live;
static int live_request;
static int live_server_started;
/* Set when the listener could not be created at all, so the closing
 * notification reports the port conflict instead of a capture failure. */
static char stream_failure[96];
static int live_profile_ready;
static int bitrate_after_start;
static int applied_bitrate_kbps;
/* Start() 0x81950008 = the encoder slot for this feature is still owned by a
 * dying session. A fresh open a second later usually succeeds, so retry the
 * whole open a couple of times before giving up. */
static int audio_codec = 2;  /* 0 = f32 (3 Mbps), 1 = s16 (1.5), 2 = IMA ADPCM (0.38) */
static int start_slot_busy;   /* Start() reported 0x81950008 */
static int start_retry_done;   /* full reopens already spent on it */
static int alt_profile_tried;   /* tried the other feature slot once */
static int applied_bitrate_kbps;
/* Last videoOutMode reported by the console: 0x10 means the source only
 * produces 1080p, so 1440p/2160p requests can never open. */
static uint32_t last_video_mode;

static const char *fmt_rc(long rc);

static void set_video_bitrate(FILE *f, pbs_inject_t *ctx, uint64_t handle) {
  intptr_t fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2SetVideoBitrate");
  unsigned char params[16] = {0};
  uint64_t size = sizeof params;
  memcpy(params, &size, 8); memcpy(params + 8, &requested_video_bitrate, 4);
  memcpy(params + 12, &requested_video_budget, 4);
  if (requested_video_budget) fprintf(f, "SetVideoBitrate companion=%u\n", requested_video_budget);
  if (fn && !pbs_inject_write_at(ctx, ctx->scratch + 0x780, params, sizeof params))
    fprintf(f, "SetVideoBitrate(%u%s) -> %s\n", requested_video_bitrate,
      bitrate_after_start ? " after Start" : "", fmt_rc(capture_call_fast(ctx, fn,
      (uint64_t[6]){handle, (uint64_t)ctx->scratch + 0x780, 0, 0, 0, 0})));
}

static void param_u64(unsigned char *p, size_t offset, uint64_t value) {
  memcpy(p + offset, &value, sizeof value);
}

static void param_u32(unsigned char *p, size_t offset, uint32_t value) {
  memcpy(p + offset, &value, sizeof value);
}

static const char *fmt_rc(long rc) {
  static char buf[64];
  if (rc == LONG_MIN) {
    snprintf(buf, sizeof buf, "NO RESPONSE");
  } else {
    snprintf(buf, sizeof buf, "0x%08lx%s", (unsigned long)rc,
             ((int32_t)(uint32_t)rc >= 0 ? " (ok)" : " (error)"));
  }
  return buf;
}

static void dump_scratch(FILE *f, pbs_inject_t *ctx, size_t len) {
  unsigned char buf[4096];
  if (len > sizeof buf) len = sizeof buf;
  if (pbs_inject_read(ctx, buf, len)) {
    fprintf(f, "  scratch: read failed errno=%d\n", errno);
    return;
  }
  fprintf(f, "  scratch: ");
  for (size_t i = 0; i < len; i++) fprintf(f, "%02x", buf[i]);
  fprintf(f, "\n");
  fflush(f);
}

static uint64_t capture_clock_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* The encoder can deliver fewer than the requested 60 frames/s. Timestamp
 * actual publication time so playback does not consume the stream faster
 * than capture produces it. The HTTP wire clock wraps naturally at 32 bits. */
static uint64_t capture_clock_90k(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 90000 + (uint64_t)t.tv_nsec * 9 / 100000;
}

/* IMA ADPCM (4 bit/sample) tables: 384 kbps stereo at 48 kHz instead of
 * 1536 kbps for s16 PCM, with the same decoder cost as a table lookup. */
static const int pbs_ima_index[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static const int pbs_ima_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
    130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767};
static int pbs_ima_encode(int sample, int *pred, int *index) {
  int step = pbs_ima_step[*index];
  int diff = sample - *pred;
  int sign = 0;
  if (diff < 0) { sign = 8; diff = -diff; }
  int delta = step >> 3;
  int code = 0;
  if (diff >= step) { code |= 4; diff -= step; delta += step; }
  if (diff >= (step >> 1)) { code |= 2; diff -= (step >> 1); delta += (step >> 1); }
  if (diff >= (step >> 2)) { code |= 1; delta += (step >> 2); }
  int next = *pred + (sign ? -delta : delta);
  if (next > 32767) next = 32767; else if (next < -32768) next = -32768;
  *pred = next;
  int ni = *index + pbs_ima_index[code | sign];
  if (ni < 0) ni = 0; else if (ni > 88) ni = 88;
  *index = ni;
  return code | sign;
}

typedef struct {
  uint64_t handle;
  intptr_t read_fn, stop_fn, close_fn, alloc_fn, free_fn;
  intptr_t buffer;
  FILE *raw, *info;
  uint64_t packets, bytes;
  int publish; /* Publish audio blocks to the HTTP server instead of a file. */
  int to_s16;  /* Publish 16-bit PCM (half the wire bytes) instead of float32. */
  int adpcm;   /* 4-bit IMA per channel, packed as (L<<4)|R per frame. */
  int ima_index[2];
  int ima_pred[2];
  int analyze; /* Accumulate float32 sample statistics. */
  uint32_t last_pts;
  uint32_t vol_original;
  intptr_t setvol_fn;
  int vol_changed;
  double sumsq;
  uint64_t floats;
  float peak;
  unsigned fails;      /* consecutive failed reads */
  int32_t last_code;   /* last error code, to log each distinct failure once */
  int dead;            /* give up reading audio for this session */
  int reopen_tried;    /* one reopen attempt per session */
  unsigned capture_feature;
  uint32_t format;
} pbs_audio_t;

/* Open the audio channel with the verified 0x38-byte parameter structure: common size 0x18, usage 2, format, arbitrator feature and 128 kbit/s rate. */
static int pbs_audio_open(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio, unsigned capture_feature, uint32_t format) {
  memset(audio, 0, sizeof *audio);
  intptr_t open_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2OpenAudio");
  intptr_t start_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Start");
  audio->read_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2ReadAudio");
  audio->stop_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Stop");
  audio->close_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Close");
  audio->alloc_fn = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "malloc");
  audio->free_fn = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "free");
  if (!open_fn || !start_fn || !audio->read_fn || !audio->alloc_fn) return -1;
  unsigned char params[0x440] = {0};
  pbs_ps5_audio_request(params+0x408,capture_feature,format);
  if (pbs_inject_write(ctx, params, sizeof params)) return -1;
  long rc = capture_call_fast(ctx, open_fn, (uint64_t[6]){
      (uint64_t)ctx->scratch + 0x400, (uint64_t)ctx->scratch + 0x408, 0, 0, 0, 0});
  fprintf(f, "Audio OpenAudio(format=%u feature=%u) -> %s%s\n", format, capture_feature, fmt_rc(rc), ctx->asserted ? " [ASSERT]" : "");
  if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0 || ctx->asserted) return -1;
  audio->capture_feature = capture_feature;
  audio->format = format;
  ++capture_audio_unclosed;
  if (pbs_inject_read_at(ctx, ctx->scratch + 0x400, &audio->handle, sizeof audio->handle) || !audio->handle) {
    fprintf(f, "Audio handle mancante\n");
    return -1;
  }
  audio->buffer = (intptr_t)capture_call_fast(ctx, audio->alloc_fn, (uint64_t[6]){65536, 0, 0, 0, 0, 0});
  if (audio->buffer <= 0) return -1;
  rc = capture_call_fast(ctx, start_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0});
  fprintf(f, "Audio Start -> %s%s\n", fmt_rc(rc), ctx->asserted ? " [ASSERT]" : "");
  if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0) return -1;
  return 0;
}

static void pbs_audio_close(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio) {
  if (audio->vol_changed && audio->setvol_fn && audio->handle && !ctx->asserted) {
    float original;
    memcpy(&original, &audio->vol_original, sizeof original);
    fprintf(f, "Audio SetAudioOutVolume(restore %.6f) -> %s\n", (double)original,
            fmt_rc(capture_call_fast_fp(ctx, audio->setvol_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0}, original)));
    audio->vol_changed = 0;
  }
  if (audio->handle && !ctx->asserted) {
    if (audio->stop_fn) fprintf(f, "Audio Stop -> %s\n", fmt_rc(capture_call_fast(ctx, audio->stop_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0})));
    if (audio->close_fn) {
      long rc = capture_call_fast(ctx, audio->close_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0});
      fprintf(f, "Audio Close -> %s\n", fmt_rc(rc));
      if (rc != LONG_MIN && (int32_t)(uint32_t)rc >= 0 && !ctx->asserted)
        --capture_audio_unclosed;
    }
  }
  if (audio->buffer > 0 && !capture_audio_unclosed && !ctx->asserted && audio->free_fn)
    capture_call_fast(ctx, audio->free_fn, (uint64_t[6]){(uint64_t)audio->buffer, 0, 0, 0, 0, 0});
  if (audio->raw) fclose(audio->raw);
  if (audio->info) fclose(audio->info);
  if (audio->analyze && audio->floats)
    fprintf(f, "Audio analisi: rms=%.6f picco=%.6f campioni=%llu\n",
            sqrt(audio->sumsq / (double)audio->floats), (double)audio->peak,
            (unsigned long long)audio->floats);
  fprintf(f, "Audio captured packets=%llu bytes=%llu\n",
          (unsigned long long)audio->packets, (unsigned long long)audio->bytes);
  memset(audio, 0, sizeof *audio);
}

/* Drain at most rounds available audio packets into the reusable local frame buffer. */
static void pbs_audio_drain(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio, unsigned char *frame, int rounds) {
  if (audio->dead) return;
  for (int i = 0; i < rounds && audio->read_fn; i++) {
    long rc = capture_call_fast(ctx, audio->read_fn, (uint64_t[6]){audio->handle,
        (uint64_t)audio->buffer, 65536, (uint64_t)ctx->scratch + 0x500, 1, 0});
    if (rc == LONG_MIN) { fprintf(f, "Audio ReadAudio timeout\n"); break; }
    int32_t count = (int32_t)(uint32_t)rc;
    if (count <= 0) {
      int32_t code = (int32_t)(uint32_t)rc;
      /* One line per distinct failure code instead of one per poll. */
      if (!audio->packets && audio->last_code != code) {
        fprintf(f, "Audio ReadAudio -> %s\n", fmt_rc(rc));
        audio->last_code = code;
      }
      if (++audio->fails >= 120) {
        if (!audio->reopen_tried) {
          unsigned feat = audio->capture_feature;
          uint32_t fmt = audio->format;
          fprintf(f, "Audio read failed (%s); reopening the audio channel once\n", fmt_rc(rc));
          pbs_audio_close(f, ctx, audio);
          if (pbs_audio_open(f, ctx, audio, feat, fmt) == 0) {
            audio->reopen_tried = 1;
            audio->fails = 0;
            break;
          }
        }
        audio->dead = 1;
        fprintf(f, "Audio disabled for this session after %u failed reads (%s); reopen capture to retry\n",
                audio->fails, fmt_rc(rc));
      }
      break;
    }
    audio->fails = 0;
    if (count > 65536 || pbs_inject_read_at(ctx, audio->buffer, frame, count)) break;
    unsigned char info[16] = {0};
    pbs_inject_read_at(ctx, ctx->scratch + 0x500, info, sizeof info);
    uint32_t pts = 0;
    memcpy(&pts, info + 8, sizeof pts);
    audio->last_pts = pts;
    if (audio->publish && audio->adpcm) {
      /* f32 stereo -> 4-bit IMA per channel, packed one byte per frame as
       * (left<<4)|right: 384 kbps at 48 kHz. */
      static unsigned char conv[8192];
      const float *src = (const float *)frame;
      int frames = count / 8;
      if (frames > (int)sizeof conv) frames = (int)sizeof conv;
      for (int i2 = 0; i2 < frames; ++i2) {
        int l = (int)(src[i2 * 2] * 32767.0f);
        int r = (int)(src[i2 * 2 + 1] * 32767.0f);
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        const int nl = pbs_ima_encode(l, &audio->ima_pred[0], &audio->ima_index[0]);
        const int nr = pbs_ima_encode(r, &audio->ima_pred[1], &audio->ima_index[1]);
        conv[i2] = (unsigned char)((nl << 4) | nr);
      }
      if (pbs_audio_publish(conv, (size_t)frames, pts)) break;
    } else if (audio->publish) {
      if (audio->to_s16) {
        static int16_t conv[16384];
        int samples = count / 4;
        if (samples > (int)(sizeof conv / sizeof conv[0])) samples = (int)(sizeof conv / sizeof conv[0]);
        const float *src = (const float *)frame;
        for (int k = 0; k < samples; ++k) {
          float v = src[k];
          if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
          conv[k] = (int16_t)(v * 32767.0f);
        }
        if (pbs_audio_publish(conv, (size_t)samples * 2, pts)) break;
      } else if (pbs_audio_publish(frame, (size_t)count, pts)) break;
    } else {
      if (audio->info) { uint32_t size = (uint32_t)count; fwrite(&size, 1, 4, audio->info); fwrite(info, 1, 16, audio->info); }
      if (audio->raw) fwrite(frame, 1, count, audio->raw);
    }
    if (audio->analyze && (count & 3) == 0) {
      const float *samples = (const float *)frame;
      for (int j = 0; j < count / 4; j++) {
        double v = samples[j];
        audio->sumsq += v * v;
        float a = v < 0 ? (float)-v : (float)v;
        if (a > audio->peak) audio->peak = a;
      }
      audio->floats += (uint64_t)(count / 4);
    }
    if (!audio->packets) {
      fprintf(f, "Audio first packet bytes=%d prefix=", count);
      for (unsigned j = 0; j < 24 && j < (unsigned)count; j++) fprintf(f, "%02x", frame[j]);
      fprintf(f, " metadata=");
      for (unsigned j = 0; j < sizeof info; j++) fprintf(f, "%02x", info[j]);
      fprintf(f, "\n");
    }
    audio->packets++; audio->bytes += (uint64_t)count;
  }
}

static int capture_stream(FILE *f, pbs_inject_t *ctx, uint64_t handle, int seconds, int port, int width, int height, unsigned capture_feature, int audio_mode) {
  if (seconds < 0 || seconds > 60) { fprintf(f, "capture_stream: invalid seconds=%d\n", seconds); return -1; }
  int serving = seconds == 0 || stress_live;
  int recover = 0;
  intptr_t start = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Start");
  intptr_t stop = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Stop");
  intptr_t read = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2ReadVideo");
  intptr_t alloc = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "malloc");
  intptr_t release = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "free");
  intptr_t request_key = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2RequestKeyFrame");
  if (!start || !stop || !read || !alloc || !release) {
    fprintf(f, "capture_stream: export resolution failed (start=%p stop=%p read=%p alloc=%p release=%p)\n",
            (void *)(uintptr_t)start, (void *)(uintptr_t)stop, (void *)(uintptr_t)read,
            (void *)(uintptr_t)alloc, (void *)(uintptr_t)release);
    return -1;
  }
  long buffer = capture_call_fast(ctx, alloc, (uint64_t[6]){0x200000, 0, 0, 0, 0, 0});
  if (buffer <= 0) {
    /* SceRemotePlay's heap fragments after many capture restarts and its
     * malloc(2MB) starts returning 0. Fall back to the worker-reserved frame
     * buffer allocated once at boot from the same MAP_ANON pool; the capture
     * restart cycle then never allocates from the target heap again. */
    buffer = pbs_ps5_worker_frame_buffer();
    if (buffer > 0) {
      fprintf(f, "capture_stream: target malloc failed; using worker frame buffer at %p\n",
              (void *)(uintptr_t)buffer);
    } else {
      fprintf(f, "capture_stream: frame buffer alloc failed rc=0 and no worker fallback\n");
      return -1;
    }
  }
  unsigned char *frame = malloc(0x200000);
  if (!frame) goto capture_free;
  FILE *out = fopen("/data/pbs/h264.stream", "wb");
  if (!out) goto capture_free;
  if (requested_video_bitrate && !bitrate_after_start) set_video_bitrate(f, ctx, handle);
  pbs_audio_t audio;
  memset(&audio, 0, sizeof audio);
  if (audio_mode == 2 && !pbs_audio_open(f, ctx, &audio, capture_feature, 0)) {
    audio.publish = 1; audio.adpcm = audio_codec == 2; audio.to_s16 = audio_codec == 1;
  }
  long rc = capture_call_fast(ctx, start, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
  fprintf(f, "Start() -> %s\n", fmt_rc(rc));
  if (rc != LONG_MIN && (uint32_t)rc == 0x81950008u && serving) start_slot_busy = 1;
  if (rc != LONG_MIN && (int32_t)(uint32_t)rc >= 0) { start_retry_done = 0; alt_profile_tried = 0; }
  if (serving && native_rate_config && requested_video_bitrate > 30000000 &&
      rc != LONG_MIN && (int32_t)(uint32_t)rc < 0 && !ctx->asserted) {
    fprintf(f, "Start rejected at requested bitrate=%u; bitrate and QP remain unchanged\n",
            requested_video_bitrate);
  }
  uint64_t total = 0, frames = 0;
  uint64_t measurement_start = 0, measurement_end = 0;
  uint64_t last_stress_key_frame = UINT64_MAX;
  if (serving && rc != LONG_MIN && (int32_t)(uint32_t)rc < 0 && pbs_h264_server_running()) recover = 1;
  if (rc != LONG_MIN && (int32_t)(uint32_t)rc >= 0) {
    if (requested_video_bitrate && bitrate_after_start) set_video_bitrate(f, ctx, handle);
    intptr_t headers = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2GetNonVclNalUnitStream");
    if (headers) {
      long hr = capture_call_fast(ctx, headers, (uint64_t[6]){handle, (uint64_t)buffer, 0, 0, 0, 0});
      fprintf(f, "GetNonVclNalUnitStream() -> %s\n", fmt_rc(hr));
      unsigned char metadata[0x1a8];
      if (hr != LONG_MIN && (int32_t)(uint32_t)hr >= 0 &&
          !pbs_inject_read_at(ctx, buffer, metadata, sizeof metadata)) {
        FILE *hf = fopen("/data/pbs/h264.headers", "wb");
        if (hf) { fwrite(metadata, 1, sizeof metadata, hf); fclose(hf); }
        uint64_t sps_len, pps_len;
        memcpy(&sps_len, metadata + 0x10, 8);
        memcpy(&pps_len, metadata + 0x98, 8);
        if (sps_len > 0 && sps_len <= 128 && pps_len > 0 && pps_len <= 128 &&
            (metadata[0x18] & 31) == 7 && (metadata[0xa0] & 31) == 8) {
          const unsigned char annexb[4] = {0, 0, 0, 1};
          fwrite(annexb, 1, 4, out);
          fwrite(metadata + 0x18, 1, sps_len, out);
          fwrite(annexb, 1, 4, out);
          fwrite(metadata + 0xa0, 1, pps_len, out);
          fprintf(f, "Annex B headers: SPS=%llu PPS=%llu\n",
                  (unsigned long long)sps_len, (unsigned long long)pps_len);
          if (serving) {
            int server_rc = pbs_h264_server_start(port, width, height, metadata + 0x18, sps_len, metadata + 0xa0, pps_len);
            int bound_port = server_rc ? 0 : pbs_h264_server_port();
            fprintf(f, "H.264 HTTP :%d -> %d\n", port, server_rc);
            if (server_rc) {
              /* A failed bind used to fall through to the timed loop, which
               * exited immediately and silently: the payload quit right
               * after OpenVideo with no diagnosis. Say it out loud so the
               * log carries the cause (typically the port still held by a
               * just-replaced instance in kernel TIME_WAIT). */
              fprintf(f, "HTTP server bind failed on port %d (errno=%d)\n",
                      port, errno);
              snprintf(stream_failure, sizeof stream_failure,
                       "PBS: HTTP bind failed on port %d", port);
              notify(stream_failure);
            } else {
              const int first_start = !live_server_started;
              live_server_started = 1;
              live_profile_ready = 1;
              stream_failure[0] = 0;
              if (bound_port && bound_port != port)
                fprintf(f, "port %d busy, serving on %d\n", port, bound_port);
              if (audio.publish)
                pbs_audio_configure(audio_codec == 2 ? "adpcm_ima"
                                     : audio_codec == 1 ? "pcm_s16le" : "pcm_f32le",
                                    2, 48000);
              /* Capture recovery/profile changes keep this payload instance alive. */
              if (first_start) {
                notify("PBS - Made By Seregonwar\n"
                       "Thanks to: stonedmodder, zer0day, nexgen999, NSTG, and ONE Jailbreak for testing");
                char ready_message[160];
                if (bound_port && bound_port != port)
                  snprintf(ready_message, sizeof ready_message,
                           "PBS started successfully\nPort %d was busy: streaming and web dashboard on port %d",
                           port, bound_port);
                else
                  snprintf(ready_message, sizeof ready_message,
                           "PBS started successfully\nStreaming and web dashboard ready on port %d", bound_port);
                notify(ready_message);
              }
            }
          }
        } else fprintf(f, "unexpected SPS/PPS metadata layout\n");
      }
    }
    uint64_t until = capture_clock_ms() + (uint64_t)seconds * 1000;
    uint64_t last_video_ms = capture_clock_ms();
    int reports = 0;
    measurement_start = capture_clock_ms();
    const uint64_t video_clock_start = capture_clock_90k();
    while ((serving ? pbs_h264_server_running() && (!stress_live || capture_clock_ms() < until)
                    : capture_clock_ms() < until) && !ctx->asserted) {
      if (serving && pbs_h264_take_restart_request()) { recover = 1; fprintf(f, "capture restart requested\n"); break; }
      int key_requested = serving && pbs_h264_take_keyframe_request();
      /* Live bitrate ceiling from the web UI: apply once per change, then a
       * keyframe forces the new rate to take effect immediately. */
      if (serving && request_key) {
        int cap_kbps = pbs_h264_take_bitrate_request();
        if (cap_kbps && cap_kbps != applied_bitrate_kbps) {
          requested_video_bitrate = (uint32_t)cap_kbps * 1000;
          requested_video_budget = requested_video_bitrate;
          set_video_bitrate(f, ctx, handle);
          applied_bitrate_kbps = cap_kbps;
          fprintf(f, "Bitrate cap applied: %d kbps\n", cap_kbps);
          pbs_h264_request_keyframe();
        }
      }
      int stress_key = (!serving || stress_live) && stress_keyframe_period &&
                      frames % stress_keyframe_period == 0 && frames != last_stress_key_frame;
      if (request_key && (key_requested || stress_key)) {
        if (stress_key) last_stress_key_frame = frames;
        long kr = capture_call_fast(ctx, request_key, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
        if (key_requested || kr == LONG_MIN || (int32_t)(uint32_t)kr < 0)
          fprintf(f, "RequestKeyFrame() -> %s%s\n", fmt_rc(kr), ctx->asserted ? " [ASSERT]" : "");
        if (kr == LONG_MIN || ctx->asserted) break;
      }
      rc = capture_call_fast(ctx, read, (uint64_t[6]){handle, (uint64_t)buffer,
          0x200000, (uint64_t)ctx->scratch + 0x300, 0, 0});
      if (rc == LONG_MIN) {
        fprintf(f, "ReadVideo timeout\n");
        recover = serving && !ctx->asserted;
        break;
      }
      int32_t bytes = (int32_t)(uint32_t)rc;
      if (bytes > 0 && bytes <= 0x200000) {
        if (pbs_inject_read_at(ctx, buffer, frame, bytes)) {
          fprintf(f, "frame memory read failed errno=%d\n", errno);
          break;
        }
        if (serving) {
          uint32_t pts = (uint32_t)(capture_clock_90k() - video_clock_start);
          if (pbs_h264_publish(frame, bytes, pts)) {
            fprintf(f, "frame publish failed\n");
            break;
          }
        } else if (fwrite(frame, 1, bytes, out) != (size_t)bytes) break;
        total += bytes;
        frames++;
        last_video_ms = capture_clock_ms();
      } else if (reports++ < 3) {
        fprintf(f, "ReadVideo() -> %s\n", fmt_rc(rc));
        dump_scratch(f, ctx, 16);
      }
      /* ReadVideo blocks until a frame is available; no extra delay after
       * a successful read. Back off only on an empty/error result. */
      if (bytes <= 0) usleep(1000);
      if (serving && capture_clock_ms() - last_video_ms >= 2000) {
        fprintf(f, "capture stalled: last ReadVideo=%s; reopening session\n", fmt_rc(rc));
        recover = 1;
        break;
      }
      if (audio.handle && !ctx->asserted) pbs_audio_drain(f, ctx, &audio, frame, 2);
    }
    if (serving) fprintf(f, "live loop exit running=%d asserted=%d\n", pbs_h264_server_running(), ctx->asserted);
    long sr = capture_call_fast(ctx, stop, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
    measurement_end = capture_clock_ms();
    fprintf(f, "Stop() -> %s\n", fmt_rc(sr));
  }
  if (serving) {
    if (recover) pbs_h264_discontinuity();
    else pbs_h264_server_stop();
  }
  if (audio.handle) pbs_audio_close(f, ctx, &audio);
  if (measurement_start && measurement_end > measurement_start) {
    double elapsed = (measurement_end - measurement_start) / 1000.0;
    fprintf(f, "capture metrics seconds=%.3f fps=%.3f Mbps=%.3f stress_keyframe_period=%u\n",
            elapsed, frames / elapsed, total * 8 / elapsed / 1000000, stress_keyframe_period);
  }
  fprintf(f, "capture-fast: %llu reads, %llu bytes -> /data/pbs/h264.stream\n",
          (unsigned long long)frames, (unsigned long long)total);
  fclose(out);
capture_free:
  free(frame);
  capture_call_fast(ctx, release, (uint64_t[6]){(uint64_t)buffer, 0, 0, 0, 0, 0});
  return recover;
}

typedef struct {
  FILE *log;
  pbs_inject_t *ctx;
  long (*call)(pbs_inject_t *,intptr_t,const uint64_t[6]);
  intptr_t change, clear, terminate, state_address;
  unsigned feature;
} capture_release_context;

static int release_capture_step(void *opaque,pbs_ps5_release_step step) {
  capture_release_context *r=opaque;
  if (r->ctx->asserted) return -1;
  intptr_t fn=step==PBS_PS5_RELEASE_STOP?r->change:
              step==PBS_PS5_RELEASE_CALLBACK?r->clear:r->terminate;
  if (!fn) return -1;
  uint64_t args[6]={0};
  if (step!=PBS_PS5_RELEASE_INITIALIZE) args[0]=r->feature;
  if (step==PBS_PS5_RELEASE_STOP) {
    const char state[32]="STOP";
    if (pbs_inject_write_at(r->ctx,r->state_address,state,sizeof state)) return -1;
    args[1]=(uint64_t)r->state_address;
  }
  long rc=r->call(r->ctx,fn,args);
  if (step==PBS_PS5_RELEASE_STOP)
    fprintf(r->log,"ChangeState(%u, STOP) -> %s\n",r->feature,fmt_rc(rc));
  else if (step==PBS_PS5_RELEASE_CALLBACK)
    fprintf(r->log,"ClearCallback(%u) -> %s\n",r->feature,fmt_rc(rc));
  else fprintf(r->log,"Terminate(owned Initialize) -> %s\n",fmt_rc(rc));
  return rc==LONG_MIN || r->ctx->asserted || (int32_t)(uint32_t)rc<0 ? -1:0;
}

/* Production entry point: exported capture APIs and caller-owned handles. */
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "avserve";
  if (strcmp(mode, "avserve")) { notify("PBS: use avserve [720p|1080p|1440p|2160p] [bitrate=N] [minqp=N] [norate] [offsetdump] [dependencydump] [moduledump] [noaudio] [nocontroller] [port]"); return 1; }
  char *serve_args[] = {argv[0], "avopenfast", "SceRemotePlay", "grant", "12", "2", "0", "9999", "1280", "720", "1"};
#if defined(PBS_PS5_DEFAULT_1080P)
  serve_args[4] = "12"; serve_args[8] = "1920"; serve_args[9] = "1080";
#elif defined(PBS_PS5_DEFAULT_720P)
  serve_args[4] = "12"; serve_args[8] = "1280"; serve_args[9] = "720";
#elif defined(PBS_PS5_DEFAULT_2160P)
  serve_args[4] = "10"; serve_args[8] = "3840"; serve_args[9] = "2160";
#endif
  int offset_dump = 0;
  int offset_dumped = 0;
  int dependency_dump = 0;
  int dependency_dumped = 0;
  int module_file_dump = 0;
  int diagnostic_open_only = 0;
  int controller_enabled = PBS_PS5_CONTROLLER;
#ifdef PBS_PS5_OFFSET_DUMP_DEFAULT
  offset_dump = 1;
#endif
#ifdef PBS_PS5_DEPENDENCY_DUMP_DEFAULT
  dependency_dump = 1;
#endif
#ifdef PBS_PS5_MODULE_FILE_DUMP_DEFAULT
  module_file_dump = 1;
#endif
#ifdef PBS_PS5_DIAGNOSTIC_OPEN_ONLY
  diagnostic_open_only = 1;
#endif
  int audio_mode = 2;
  int automatic_resolution = 1;
  if (!strcmp(mode, "avserve")) {
    live_request = 1;
    native_rate_config = 1;
#ifdef PBS_PS5_NO_NATIVE_RATE_DEFAULT
    native_rate_config = 0;
#endif
    requested_min_qp = 18;
    requested_video_bitrate = 60000000;
    for (int i = 2; i < argc; i++) {
      if (!strncmp(argv[i], "bitrate=", 8)) {
        char *end = NULL;
        unsigned long rate = strtoul(argv[i] + 8, &end, 10);
        if (!end || *end || rate < 1000000 || rate > 100000000) return 1;
        requested_video_bitrate = (uint32_t)rate;
      } else if (!strncmp(argv[i], "minqp=", 6)) {
        char *end = NULL;
        unsigned long qp = strtoul(argv[i] + 6, &end, 10);
        if (!end || *end || qp < 18 || qp > 35) return 1;
        requested_min_qp = (uint32_t)qp;
      } else if (!strcmp(argv[i], "norate")) {
        native_rate_config = 0;
      } else if (!strcmp(argv[i], "offsetdump")) {
        offset_dump = 1;
      } else if (!strcmp(argv[i], "dependencydump")) {
        dependency_dump = 1;
      } else if (!strcmp(argv[i], "moduledump")) {
        module_file_dump = 1;
      } else if (!strcmp(argv[i], "nocontroller")) {
        controller_enabled = 0;
      }
    }
    requested_video_budget = requested_video_bitrate;
    audio_mode = 2; /* Live PCM audio for connected viewers. */
    for (int i = 2; i < argc; i++) {
      if (!strncmp(argv[i], "bitrate=", 8)) continue;
      if (!strncmp(argv[i], "minqp=", 6)) continue;
      if (!strcmp(argv[i], "norate")) continue;
      if (!strcmp(argv[i], "offsetdump")) continue;
      if (!strcmp(argv[i], "dependencydump")) continue;
      if (!strcmp(argv[i], "moduledump")) continue;
      if (!strcmp(argv[i], "nocontroller")) continue;
      if (!strcmp(argv[i], "noaudio")) { audio_mode = 0; continue; }
      if (!strcmp(argv[i], "audio16")) { audio_codec = 1; continue; }
      if (!strcmp(argv[i], "audio32")) { audio_codec = 0; continue; }
      if (!strcmp(argv[i], "720p")) { automatic_resolution = 0; serve_args[4] = "12"; serve_args[8] = "1280"; serve_args[9] = "720"; continue; }
      if (!strcmp(argv[i], "1080p")) { automatic_resolution = 0; serve_args[4] = "12"; serve_args[8] = "1920"; serve_args[9] = "1080"; continue; }
      if (!strcmp(argv[i], "1440p") || !strcmp(argv[i], "2k")) { automatic_resolution = 0; serve_args[4] = "11"; serve_args[8] = "2560"; serve_args[9] = "1440"; continue; }
      if (!strcmp(argv[i], "2160p") || !strcmp(argv[i], "4k")) { automatic_resolution = 0; serve_args[4] = "10"; serve_args[8] = "3840"; serve_args[9] = "2160"; continue; }
      char *end = NULL;
      unsigned long port = strtoul(argv[i], &end, 10);
      if (!end || *end || port < 1 || port > 65535) return 1;
      serve_args[7] = argv[i];
    }
    argv = serve_args; argc = 10; mode = "avopenfast";
  }

  if (mkdir(PBS_DATA_DIR, 0777) && errno != EEXIST) {
    notify("PBS: cannot create /data/pbs");
    return 1;
  }
  /* Replace any previous PBS instance instead of fighting it for port 9999
   * and the encoder: repeated injections used to leave two live payloads
   * sharing one avcap2 session (0x81950008 storm, port drift to 10000).
   * The process name comes from elfldr's argv[0] ("payload.elf" for raw
   * uploads, the ELF filename otherwise); pids reported by sysctl exclude
   * nothing, so skip our own pid and never self-terminate. */
  {
    pid_t pids[16];
    int count = pbs_find_all_pids(argv[0], pids, 16);
    pid_t self = getpid();
    int killed = 0;
    for (int i = 0; i < count; i++) {
      if (pids[i] == self || pids[i] <= 0) continue;
      kill(pids[i], SIGKILL);
      killed++;
    }
    if (killed) {
      /* The dying instance releases the listener and its avcap2 session on
       * process teardown. A fixed delay loses the race sometimes: the old
       * listener socket can still sit in the kernel while the new instance
       * binds, which failed silently and quit the payload. Instead of
       * guessing, probe-bind the target port until it accepts or give up
       * after 10 seconds (the port scan still offers fallbacks). */
      const uint16_t probe_port = 9999;
      for (int wait_ms = 0; wait_ms < 10000; wait_ms += 200) {
        usleep(200000);
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe < 0) continue;
        int yes = 1; setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        struct sockaddr_in addr = {.sin_family = AF_INET,
                                   .sin_port = htons(probe_port),
                                   .sin_addr.s_addr = htonl(INADDR_ANY)};
        int can_bind = !bind(probe, (struct sockaddr *)&addr, sizeof addr);
        close(probe);
        if (can_bind) break;
      }
      FILE *rk = open_report("inject.txt", "a");
      if (rk) {
        fprintf(rk, "PBS replace: killed %d previous instance(s) of %s\n", killed, argv[0]);
        fclose(rk);
      }
      notify("PBS: replaced previous instance");
    }
  }
  FILE *startup_report = open_report("inject.txt", "a");
  if (startup_report) {
    uint32_t firmware = kernel_get_fw_version();
    fprintf(startup_report, "PBS startup firmware=0x%08x capture_profile=%s\n",
            firmware, pbs_ps5_find_offsets(firmware) ? "available" : "missing");
    fprintf(startup_report,
            "Runtime revision=%s exact-initializers=1 safe-worker-diagnostics=1\n",
            PBS_PS5_RUNTIME_REVISION);
    fprintf(startup_report, "Controller compiled=%d enabled=%d\n",
            PBS_PS5_CONTROLLER, controller_enabled);

    /* HEVC capability probe: avcap2 only exposes H.264 today. Resolve a set of
     * plausible codec entry points once at startup so the console itself
     * answers whether an HEVC switch exists. */
    {
      static const char *const codec_candidates[] = {
        "sceAvcap2SetVideoCodec", "sceAvcap2SetCodec", "sceAvcap2SetEncodeCodec",
        "sceAvcap2SetVideoFormat", "sceAvcap2SetEncodeParam", "sceAvcap2SetEncoderParam",
        "sceAvcap2SetVideoParameter", "sceAvcap2SetCodecParam", "sceAvcap2GetVideoCodec",
        "sceAvcap2GetSupportedCodec", "sceAvcap2EnableHevc", "sceAvcap2SetHevc"};
      pid_t probe_pid = pbs_find_pid("SceRemotePlay");
      fprintf(startup_report, "avcap2 codec probe pid=%d\n", (int)probe_pid);
      if (probe_pid > 0) {
        for (size_t i = 0; i < sizeof codec_candidates / sizeof codec_candidates[0]; ++i) {
          intptr_t addr = pbs_resolve_in(probe_pid, "libSceAvcap2.sprx", codec_candidates[i]);
          fprintf(startup_report, "  %s=%s\n", codec_candidates[i], addr ? "present" : "absent");
        }
      }
    }
    fclose(startup_report);
  }
  if (module_file_dump) {
    FILE *dump_report = open_report("inject.txt", "a");
    if (dump_report) {
      pid_t target = pbs_find_pid("SceRemotePlay");
      if (target > 0) pbs_ps5_dump_module_files(dump_report, target, PBS_DATA_DIR "/dumps");
      else fprintf(dump_report, "module file dump: SceRemotePlay unavailable\n");
      fclose(dump_report);
    }
  }

  if (!strcmp(mode, "avopen1") || !strcmp(mode, "avopenfast")) {
    /* Open the capture session; recovery retries only caller-owned resources. */
    if (argc < 3) return 1;
    const uint32_t capture_firmware = kernel_get_fw_version();
    const pbs_ps5_offset_profile *layout = pbs_ps5_find_offsets(capture_firmware);
    if (!layout) {
      notify("PBS: firmware capture layout unavailable; use dump payload");
      return 1;
    }
    const pbs_ps5_avcap2_offsets *avcap = &layout->avcap2;
    if (controller_enabled) {
      pbs_ps5_controller_configure();
    }
    const pbs_ps5_remoteplay_offsets *rp = &layout->remoteplay;
    /* FW4.00 ends its request after the short 0x14-byte mask descriptor. */
    const uint32_t mask_span = rp->mask_span ? rp->mask_span : 0x24;
    if ((mask_span != 0x14 && mask_span != 0x24) ||
        avcap->request_size > 0x400 || avcap->request_size > sizeof scratch_zero - 8 ||
        rp->rate.offset + 0x24 > avcap->request_size ||
        rp->image.offset + 0x10 > avcap->request_size ||
        rp->width_offset + 4 > avcap->request_size ||
        rp->height_offset + 4 > avcap->request_size ||
        rp->paste_offset + 0x88 > avcap->request_size ||
        rp->video_mode_offset + 4 > avcap->request_size ||
        avcap->kind_offset + 4 > avcap->request_size ||
        rp->mask_mode_offset + mask_span > avcap->request_size) {
      notify("PBS: invalid capture layout");
      return 1;
    }
    const char *how = argc > 3 ? argv[3] : "none";
    int width = argc > 8 ? atoi(argv[8]) : 1280;
    int height = argc > 9 ? atoi(argv[9]) : 720;
    unsigned capture_feature = (unsigned)strtoul(argv[4], NULL, 0);
    if (automatic_resolution && height==1440 && pbs_ps5_legacy_resolution_modes(capture_firmware)) {
      width=1920; height=1080; capture_feature=12;
    }
    pbs_capture_profile previous_profile = {width, height, capture_feature};
    int changing_profile = 0;
    if (live_request) pbs_h264_enable_profile_mask(pbs_ps5_legacy_resolution_modes(capture_firmware)?11u:PBS_CAPTURE_PROFILES_ALL);
    /* Diagnostic native frame-rate enum: 0=30, 1=60; 2 is unverified.
     * Streaming keeps the verified 60 fps profile and matching timestamps. */
    uint32_t fps_mode = argc > 10 ? (uint32_t)strtoul(argv[10], NULL, 0) : 1;
    if (fps_mode > 2 || (argc > 6 && !atoi(argv[6]) && fps_mode != 1)) return 1;
    if (!((width == 1280 && height == 720) || (width == 1920 && height == 1080) ||
          (width == 2560 && height == 1440) || (width == 3840 && height == 2160))) return 1;
    long (*call)(pbs_inject_t *, intptr_t, const uint64_t[6]) =
        !strcmp(mode, "avopenfast") ? capture_call_fast : capture_call;
    char failure_note[96] = "PBS: capture unavailable";
avopen_retry:;
    /* The web cap lives in the server state, not in argv: re-read it on every
     * (re)open so a bitrate change survives the restart that applies it. */
    if (live_request) {
      int cap_now = pbs_h264_take_bitrate_request();
      if (cap_now > 0) {
        requested_video_bitrate = (uint32_t)cap_now * 1000;
        requested_video_budget = requested_video_bitrate;
      }
    }
    uint32_t requested_resolution;
    if (pbs_ps5_resolution_mode(capture_firmware,height,&requested_resolution)) {
      notify("PBS: 1440p capture unavailable on this firmware; select 1080p or 2160p");
      return 1;
    }
    int retry_lower = 0; int retry_width = width; int retry_height = height;
    static int open_retry_done;
    int open_attempted = 0;
    int opened_session = 0;
    live_profile_ready = 0;
    int owned_session_closed = 1;
    int dump_this_open = 0;
    FILE *f = open_report("inject.txt", "a");
    if (!f) return 1;
    fprintf(f, "%s %s %s\n", mode, argv[2], how);
    fprintf(f, "Capture lifecycle: owned-init + resource STOP cleanup v1\n");
    pbs_inject_t ctx;
    capture_call_uncertain = 0;
    capture_audio_unclosed = 0;
    {
      int begin_ok = 0;
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (!pbs_inject_begin(argv[2], &ctx)) { begin_ok = 1; break; }
        const int err = errno;
        fprintf(f, "begin failed errno=%d (attempt %d/3)\n", err, attempt + 1);
        /* 17 = EEXIST: the killed instance has not released its target-side
         * channel yet. Anything else is not worth waiting for. */
        if (err != 17) break;
        usleep(800000);
      }
      if (!begin_ok) {
        FILE *br = open_report("inject.txt", "a");
        if (br) { fprintf(br, "injection unavailable: restart the console before retrying\n"); fclose(br); }
        fclose(f);
        return 1;
      }
    }
    unsigned char native_request[0x400];
    uint32_t native_usage = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 2;
    int native_template_found = !pbs_ps5_find_native_request(
        ctx.pid, layout, (uint32_t)width, (uint32_t)height, capture_feature,
        native_usage, requested_resolution, fps_mode,
        native_request, sizeof native_request, f);
    if (pbs_ps5_worker_boot(&ctx, f)) {
      fprintf(f,
              "PS5 worker unavailable; refusing sustained capture with SceRemotePlay traced\n");
      if (ctx.attached) pbs_inject_end(&ctx);
      fclose(f);
      notify("PBS: Remote Play worker unavailable; capture not started");
      return 1;
    }
    const intptr_t base = ctx.scratch;
    if (automatic_resolution && width == 2560 && height == 1440) {
      intptr_t fn_probe_mode = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2GetVideoOutMode");
      uint32_t output_mode = 0;
      if (fn_probe_mode && !pbs_inject_write_at(&ctx, base + 0x700, &output_mode, sizeof output_mode)) {
        long probe_rc = call(&ctx, fn_probe_mode,
                             (uint64_t[6]){(uint64_t)base + 0x700, 0, 0, 0, 0, 0});
        if (probe_rc != LONG_MIN && (int32_t)(uint32_t)probe_rc >= 0 &&
            !pbs_inject_read_at(&ctx, base + 0x700, &output_mode, sizeof output_mode)) {
          fprintf(f, "Preflight videoOutMode=0x%x\n", output_mode);
          if (output_mode == 0x10u ||
              pbs_ps5_prefer_1080_before_open(capture_firmware, output_mode)) {
            width = 1920;
            height = 1080;
            capture_feature = 12;
            fprintf(f, "Auto capture profile=1080p feature=12 (fw=0x%08x mode=0x%x)\n",
                    capture_firmware, output_mode);
          }
        } else {
          fprintf(f, "Preflight videoOutMode unavailable; keeping requested profile\n");
        }
        if (capture_call_uncertain || ctx.asserted) {
          fprintf(f, "Preflight videoOutMode interrupted/asserted; stopping capture\n");
          (void)pbs_ps5_worker_shutdown(f);
          pbs_inject_end(&ctx);
          fclose(f);
          notify("PBS: video mode query interrupted. Restart the console before retrying.");
          return 1;
        }
      }
      /* FW13.42 was observed rejecting an OpenVideo request, but the screenshot
       * does not expose GetVideoOutMode. Its native layout matches 13.60, so
       * select the compatibility profile even when the preflight query fails.
       * This only affects automatic startup, never explicit resolution modes. */
      if (width == 2560 && height == 1440 &&
          pbs_ps5_prefer_1080_before_open(capture_firmware, 0u)) {
        width = 1920;
        height = 1080;
        capture_feature = 12;
        fprintf(f, "Auto capture profile=1080p feature=12 (fw=0x%08x, conservative startup)\n",
                capture_firmware);
      }
    }
    memset(scratch_zero, 0, sizeof scratch_zero);
    if (argc > 4) {
      /* Native SceRemotePlay constructor and Avcap2 must agree on this ABI. */
      uint64_t size = avcap->request_size, common_size = 0x28;
      uint32_t feature = capture_feature;
      uint32_t usage = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 2;
      /* Preflight may have changed the automatic profile since admission. */
      pbs_ps5_resolution_mode(capture_firmware,height,&requested_resolution);
      uint32_t resolution = requested_resolution;
      unsigned char *params = scratch_zero + 8;
      const int exact_profile = capture_firmware == layout->firmware;
      if (exact_profile && pbs_ps5_apply_request_initializers(
                               params, avcap->request_size, rp)) {
        fprintf(f, "Native request initializer application failed; refusing incomplete exact profile\n");
        (void)pbs_ps5_worker_shutdown(f);
        pbs_inject_end(&ctx);
        fclose(f);
        notify("PBS: invalid native request initializer catalogue");
        return 1;
      }
      /* The constructor's mode selector references descriptors that native
       * RemotePlay fills after construction. PBS builds a synthetic request,
       * so keep the proven kind=0 path instead of enabling that mode. */
      size_t mode_begin = rp->rate.offset + rp->rate.size;
      if (mode_begin < rp->image.offset)
        memset(params + mode_begin, 0, rp->image.offset - mode_begin);
      fprintf(f, "Native request source=%s template-found=%d reuse-disabled applied-initializers=%u\n",
              exact_profile ? "firmware-constructor" : "synthetic-family-fallback",
              native_template_found,
              exact_profile ? (unsigned)rp->initializer_count : 0u);
      /* Constructor constants establish the firmware-native nested request
       * descriptors.  Caller-owned fields are deliberately written after
       * them so runtime choices cannot be replaced by constructor defaults. */
      memcpy(scratch_zero + 8, &size, 8);
      memcpy(scratch_zero + 0x10, &common_size, 8);
      memcpy(scratch_zero + 8 + 0x14, &usage, 4);
      memcpy(scratch_zero + 8 + 0x18, &resolution, 4);
      memcpy(scratch_zero + 8 + 0x1c, &fps_mode, 4);
      memcpy(scratch_zero + 8 + 0x28, &feature, 4);
      /* ImagePaste uses the full output rectangle and all surface masks; offsets are relative to Params. */
      if (native_rate_config) {
        /* Rate group layout comes from this firmware's native constructor.
         * Keep initial capacity >= the backend's budget-per-frame floor. */
        uint32_t denominator = fps_mode == 0 ? 30000 : 60000;
        uint32_t initial = (uint32_t)(((uint64_t)requested_video_budget * 1001 + denominator - 1) / denominator);
        param_u64(params, rp->rate.offset, rp->rate.size);
        param_u64(params, rp->rate.offset + 8, rp->rate.descriptor_size);
        param_u32(params, rp->rate.offset + 0x10, requested_video_bitrate);
        param_u32(params, rp->rate.offset + 0x14, requested_video_budget);
        param_u32(params, rp->rate.offset + 0x18, initial);
        param_u32(params, rp->rate.offset + 0x1c, requested_min_qp);
        param_u32(params, rp->rate.offset + 0x20, 51);
        fprintf(f, "Native rate config rate=%u companion=%u initial=%u QP=%u..51\n",
                requested_video_bitrate, requested_video_budget, initial, requested_min_qp);
      }
      param_u64(params, rp->image.offset, rp->image.size);
      param_u64(params, rp->image.offset + 8, rp->image.descriptor_size);
      param_u32(params, rp->width_offset, width);
      param_u32(params, rp->height_offset, height);
      param_u64(params, rp->paste_offset, 0x88);
      memset(params + rp->paste_offset + 8, 0xff, 0x80);
      param_u32(params, rp->mask_mode_offset, 2);
      param_u64(params, rp->mask_mode_offset + 4, 0x10);
      param_u32(params, rp->mask_mode_offset + 0xc, 1);
      params[rp->mask_mode_offset + 0x10] = 1;
      if (mask_span == 0x24) {
        param_u64(params, rp->mask_mode_offset + 0x14, 0x10);
        params[rp->mask_mode_offset + 0x1c] = 1;
        param_u32(params, rp->mask_mode_offset + 0x20, 0x6e);
      }
      fprintf(f, "Params size=0x%x feature=%u usage=%u %dx%d fps-mode=%u fw=0x%08x profile=0x%08x selection=%s\n",
              avcap->request_size, feature, usage, width, height, fps_mode,
              capture_firmware, layout->firmware,
              capture_firmware == layout->firmware ? "exact" : "family-range");
      fprintf(f,"Capture resolution enum=%u mapping=%s\n",resolution,
              pbs_ps5_legacy_resolution_modes(capture_firmware)?"legacy-without-1440p":"with-1440p");
    }
    intptr_t fn_init = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Initialize");
    intptr_t fn_open = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2OpenVideo");
    intptr_t fn_oaudio = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2OpenAudio");
    intptr_t fn_term = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Terminate");
    intptr_t fn_clear = 0;
    intptr_t fn_change = 0;
    uint32_t grant_feature = 0;
    pbs_ps5_capture_ownership owned={0};
    if (pbs_inject_write(&ctx, scratch_zero, 0x800)) {
      fprintf(f, "initial capture parameters could not be written\n");
      goto avopen_cleanup;
    }
    if (!fn_open) {
      fprintf(f, "OpenVideo export unavailable\n");
      snprintf(failure_note, sizeof failure_note, "PBS: OpenVideo export missing");
      goto avopen_cleanup;
    }
    uint32_t avcap_handle = 0;
    if (!kernel_dynlib_handle(ctx.pid, "libSceAvcap2.sprx", &avcap_handle)) {
      intptr_t avcap_base = kernel_dynlib_mapbase_addr(ctx.pid, avcap_handle);
      if (avcap_base > 0 && fn_open - avcap_base != (intptr_t)avcap->open_video_rva)
        fprintf(f, "OpenVideo RVA differs: resolved=0x%lx profile=0x%x\n",
                (unsigned long)(fn_open - avcap_base), avcap->open_video_rva);
    }
    if (dependency_dump && !dependency_dumped) {
      pbs_ps5_dump_dependencies(f, &ctx);
      pbs_ps5_dump_initial_user(f, &ctx);
      dependency_dumped = 1;
      if (ctx.asserted) goto avopen_cleanup;
    }
    if (!strcmp(how, "tinit") && fn_term) {
      long tr = call(&ctx, fn_term, (uint64_t[6]){0, 0, 0, 0, 0, 0});
      fprintf(f, "Terminate() -> %s\n", fmt_rc(tr));
    }
    if (strcmp(how, "none")) {
      if (!fn_init || !fn_term) {
        fprintf(f,"Initialize refused: Initialize/Terminate export pair unavailable\n");
        goto avopen_cleanup;
      }
      long ir = call(&ctx, fn_init, (uint64_t[6]){0, 0, 0, 0, 0, 0});
      fprintf(f, "Initialize() -> %s\n", fmt_rc(ir));
      if (ir==LONG_MIN || ctx.asserted) { owned.uncertain=1; goto avopen_cleanup; }
      if ((int32_t)(uint32_t)ir<0) goto avopen_cleanup;
      owned.initialized=1;
    }
    if (!strcmp(how, "grant")) {
      grant_feature = capture_feature;
      /* Use capture profiles from ra-config.json. Do not modify features owned by active services. */
      if (strcmp(mode, "avopenfast") ||
          grant_feature != (width == 3840 ? 10u : width == 2560 ? 11u : 12u)) goto avopen_cleanup;
      intptr_t fn_set = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorSetCallback");
      fn_change = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorChangeState");
      fn_clear = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorClearCallback");
      /* Use getpid as a stable no-op callback that ignores notification arguments. */
      intptr_t callback = pbs_resolve_in(ctx.pid, "libkernel_sys.sprx", "getpid");
      if (!fn_set || !fn_change || !fn_clear || !callback) {
        fprintf(f, "missing resource arbitrator exports\n");
        goto avopen_cleanup;
      }
      long ar = call(&ctx, fn_set, (uint64_t[6]){grant_feature, (uint64_t)callback, 0, 0, 0, 0});
      fprintf(f, "SetCallback(%u) -> %s\n", grant_feature, fmt_rc(ar));
      if (ar==LONG_MIN || ctx.asserted) { owned.uncertain=1; goto avopen_cleanup; }
      if ((int32_t)(uint32_t)ar < 0 &&
          !pbs_ps5_callback_already_registered(ar)) goto avopen_cleanup;
      if (pbs_ps5_callback_already_registered(ar))
        fprintf(f, "Resource callback already registered; borrowing without ClearCallback ownership\n");
      else
        owned.callback = 1;
      char state[32] = "RUNNING";
      memcpy(scratch_zero + 0x600, state, sizeof state);
      if (pbs_inject_write(&ctx, scratch_zero, 0x800)) goto avopen_cleanup;
      ar = call(&ctx, fn_change, (uint64_t[6]){grant_feature, (uint64_t)base + 0x600, 0, 0, 0, 0});
      fprintf(f, "ChangeState(%u, RUNNING) -> %s\n", grant_feature, fmt_rc(ar));
      if (ar==LONG_MIN || ctx.asserted) { owned.uncertain=1; goto avopen_cleanup; }
      if ((int32_t)(uint32_t)ar < 0) goto avopen_cleanup;
      owned.running=1;
      intptr_t fn_enable = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2EnableScanInEncode");
      if (fn_enable) fprintf(f, "EnableScanInEncode() -> %s\n", fmt_rc(call(&ctx,
          fn_enable, (uint64_t[6]){0, 0, 0, 0, 0, 0})));
    }
    if (argc > 4) {
      intptr_t fn_mode = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2GetVideoOutMode");
      uint32_t video_mode = 0;
      long mr = fn_mode ? call(&ctx, fn_mode,
          (uint64_t[6]){(uint64_t)base + 0x700, 0, 0, 0, 0, 0}) : LONG_MIN;
      if (mr == LONG_MIN || (int32_t)(uint32_t)mr < 0 ||
          pbs_inject_read_at(&ctx, base + 0x700, &video_mode, sizeof video_mode)) {
        fprintf(f, "GetVideoOutMode failed -> %s\n", fmt_rc(mr));
        goto avopen_cleanup;
      }
      /* Use the firmware's video-mode field, read immediately before OpenVideo. */
      memcpy(scratch_zero + 8 + rp->video_mode_offset, &video_mode, sizeof video_mode);
      if (pbs_inject_write(&ctx, scratch_zero, 0x800)) {
        fprintf(f, "video output mode could not be written\n");
        goto avopen_cleanup;
      }
      last_video_mode = video_mode;
      fprintf(f, "Params videoOutMode=0x%x\n", video_mode);
    }
    if (native_template_found)
      pbs_ps5_dump_native_request_diff(f, scratch_zero + 8, native_request,
                                       avcap->request_size, layout);
    if (offset_dump && !offset_dumped) {
      pbs_ps5_dump_export_offsets(f, ctx.pid);
      pbs_ps5_dump_request(f, "before OpenVideo", scratch_zero + 8, avcap->request_size);
      offset_dumped = dump_this_open = 1;
    }
    open_attempted = 1;
    long rc = fn_open ? call(&ctx, fn_open,
                              (uint64_t[6]){(uint64_t)base, (uint64_t)base + 8, 0, 0, 0, 0}) : LONG_MIN;
    if (rc==LONG_MIN || ctx.asserted) owned.uncertain=1;
    else if ((int32_t)(uint32_t)rc>=0) owned_session_closed=0;
    fprintf(f, "OpenVideo() -> %s%s\n", fmt_rc(rc), ctx.asserted ? " [ASSERT]" : "");
    /* 1440p/2160p cannot open when the console only renders 1080p;
     * retry once at 1080p instead of quarantining the session. */
    if ((int32_t)(uint32_t)rc < 0 && height > 1080 && !open_retry_done) {
      retry_lower = 1; retry_width = width; retry_height = height;
    }
    /* Venc propagates this code from several distinct failures. Preserve the
     * actual request and resolved export fingerprints instead of guessing a
     * resolution/bitrate downgrade. These observers issue no remote calls. */
    if (rc != LONG_MIN && (uint32_t)rc == 0x80c78000u &&
        !ctx.asserted && !offset_dumped) {
      fprintf(f,"Encoder OpenVideo failure: collecting request/export diagnostics once; requested quality preserved\n");
      pbs_ps5_dump_request(f,"encoder OpenVideo failure",scratch_zero+8,avcap->request_size);
      pbs_ps5_dump_dependencies(f,&ctx);
      offset_dumped=1;
    }
    if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0 || ctx.asserted)
      snprintf(failure_note, sizeof failure_note, "PBS: OpenVideo 0x%08x%s",
               (unsigned)(uint32_t)rc, ctx.asserted ? " ASSERT" : "");
    if (dump_this_open && rc != LONG_MIN && !ctx.asserted) {
      unsigned char returned[0x400];
      if (pbs_inject_read_at(&ctx, base + 8, returned, avcap->request_size))
        fprintf(f, "offset dump: after OpenVideo request read failed errno=%d\n", errno);
      else {
        pbs_ps5_dump_request(f, "after OpenVideo", returned, avcap->request_size);
        pbs_ps5_dump_request_changes(f, scratch_zero + 8, returned, avcap->request_size);
      }
    }
    if (fn_oaudio && strcmp(mode, "avopenfast")) {
      long ar = capture_call(&ctx, fn_oaudio,
                                (uint64_t[6]){(uint64_t)base + 0x10, (uint64_t)base + 0x18, 0, 0, 0, 0});
      fprintf(f, "OpenAudio() -> %s%s\n", fmt_rc(ar), ctx.asserted ? " [ASSERT]" : "");
    }
    unsigned char hb[16];
    if (!pbs_inject_read(&ctx, hb, sizeof hb)) {
      uint64_t h = 0;
      memcpy(&h, hb, 8);
      fprintf(f, "handle=0x%llx\n", (unsigned long long)h);
      if (!strcmp(mode, "avopenfast") && rc != LONG_MIN &&
          (int32_t)(uint32_t)rc >= 0 && h) {
        opened_session = 1;
        owned_session_closed = 0;
        if (diagnostic_open_only) fprintf(f, "diagnostic open-only: closing handle without starting stream\n");
        else if (argc > 6) capture_stream(f, &ctx, h, atoi(argv[6]), argc > 7 ? atoi(argv[7]) : 9999, width, height, capture_feature, audio_mode);
        intptr_t close_fn = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Close");
        if (close_fn && !ctx.asserted) {
          long close_rc = call(&ctx, close_fn, (uint64_t[6]){h, 0, 0, 0, 0, 0});
          fprintf(f, "Close() -> %s\n", fmt_rc(close_rc));
          owned_session_closed = close_rc != LONG_MIN && (int32_t)(uint32_t)close_rc >= 0;
        }
      }
    }
avopen_cleanup:;
    if (capture_call_uncertain || capture_audio_unclosed || ctx.asserted) owned.uncertain=1;
    capture_release_context release_ctx={f,&ctx,call,fn_change,fn_clear,fn_term,base+0x600,grant_feature};
    int clear_ok = !pbs_ps5_release_capture(&owned,owned_session_closed,
                                           release_capture_step,&release_ctx);
    if (!clear_ok) fprintf(f,"Capture cleanup incomplete; no automatic retry or profile change\n");
    if (pbs_ps5_worker_shutdown(f)) {
      fprintf(f,
              "PS5 worker cleanup incomplete; no automatic retry or profile change\n");
      clear_ok = 0;
    }
    if (open_attempted && !opened_session)
      fprintf(f, "OpenVideo failure quarantined; no automatic reopen in this payload execution\n");
    pbs_inject_end(&ctx);
    fclose(f);
    if (retry_lower) {
      retry_lower = 0; open_retry_done = 1;
      width = 1920; height = 1080; capture_feature = 12;
      FILE *rr = open_report("inject.txt", "a");
      if (rr) { fprintf(rr, "OpenVideo failed at %ux%u; retrying once at 1080p\n", retry_width, retry_height); fclose(rr); }
      goto avopen_retry;
    }
    if (start_slot_busy && start_retry_done < 5 && clear_ok && !ctx.asserted) {
      start_slot_busy = 0; start_retry_done++;
      /* Encoder slots are per feature: if the same one keeps rejecting Start,
       * try the other resolution once before giving up. */
      FILE *sr = open_report("inject.txt", "a");
      if (sr) { fprintf(sr, "Start slot busy; reopening (attempt %d/5, %dx%d)\n", start_retry_done, width, height); fclose(sr); }
      usleep(start_retry_done >= 3 ? 2000000u : 1000000u);
      goto avopen_retry;
    }
    /* Reconfigure only after closing handles, stopping the resource request,
     * clearing its callback and balancing our Initialize reference. */
    if (changing_profile && !live_profile_ready) {
      pbs_h264_profile_failed();
      width = previous_profile.width; height = previous_profile.height;
      capture_feature = previous_profile.feature;
    }
    changing_profile = 0;
    int requested_height = 0;
    if (live_request && clear_ok && !ctx.asserted && pbs_h264_take_profile_request(&requested_height)) {
      pbs_capture_profile next;
      if (!pbs_capture_profile_for_height(requested_height, &next)) {
        /* A web/Studio selection is explicit, just like a command-line
         * profile. Do not silently replace it with the automatic startup profile. */
        automatic_resolution = 0;
        previous_profile = (pbs_capture_profile){width, height, capture_feature};
        width = next.width; height = next.height; capture_feature = next.feature;
        /* The console told us it only renders 1080p; asking avcap2 for more
         * always returns 0x81950008 on this profile table. Clamp instead of
         * failing every reopen after a bitrate change. */
        if (last_video_mode == 0x10u && height > 1080) {
          width = 1920; height = 1080; capture_feature = 12;
        }
        changing_profile = 1;
      }
    }
    /* A rejected OpenVideo may leave a backend slot occupied even when the
     * public cleanup calls succeed. Reopen only after closing a real handle. */
    int server_running = live_request && pbs_h264_server_running();
    if (pbs_ps5_can_reopen_capture(clear_ok, ctx.asserted,
                                   opened_session, server_running)) {
      usleep(250000);
      if (pbs_h264_server_running()) goto avopen_retry;
    }
    if (live_request && live_server_started && !ctx.asserted) {
      /* Park instead of exiting: the HTTP listener stays up, the web
       * dashboard shows "stopped" and POST /capture/start resumes capture
       * in-place — no re-injection, no elfldr reflash, no console reboot. */
      pbs_h264_server_wait();
      int resumed = 0;
      while (!resumed) {
        if (pbs_h264_park_until_resume(9999)) break; /* listener lost */
        pbs_h264_server_wait();
        fprintf(f, "PBS resume requested; reopening capture\n");
        notify("PBS resumed\nRestarting capture");
        live_server_started = 0;
        goto avopen_retry;
      }
      notify("PBS: listener lost; re-inject to restart");
      return 1;
    }
    if (live_request) { pbs_h264_server_stop(); pbs_h264_server_wait(); }
    pbs_input_shutdown();
    notify(live_server_started ? "PBS: streaming stopped"
                               : (stream_failure[0] ? stream_failure : failure_note));
    return live_server_started ? 0 : 1;
  }

  return 1;
}

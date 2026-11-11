// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

/* The listener binds the requested port or, when another process already owns
 * it (typically a previous payload instance), the next free port inside this
 * window. Callers advertise pbs_h264_server_port(), not the request. */
#define PBS_H264_PORT_SCAN 10
/* RTP only: a sender that falls this far behind live treats the backlog as
 * stale, jumps to the newest frame and asks capture for a fresh IDR instead of
 * replaying lag. A shorter stall keeps the queued frames. */
#define PBS_H264_RTP_MAX_BACKLOG 6
int pbs_h264_server_start(int port, int width, int height, const unsigned char *sps, size_t sps_len,
                           const unsigned char *pps, size_t pps_len);
int pbs_h264_server_start_at_rate(int port, int width, int height, int fps,
    const unsigned char *sps, size_t sps_len, const unsigned char *pps, size_t pps_len);
/* Port actually bound by the running listener, 0 when it is not running. */
int pbs_h264_server_port(void);
int pbs_h264_publish(const void *data, size_t length, uint32_t pts);
int pbs_audio_publish(const void *data, size_t length, uint32_t pts);
void pbs_audio_configure(const char *codec, int channels, int rate);
int pbs_h264_server_running(void);
/* Capture thread: consume client requests and periodic encoder refreshes. */
int pbs_h264_take_keyframe_request(void);
void pbs_h264_server_stop(void);
/* Call from the capture thread after stop, before the payload can unload. */
void pbs_h264_server_wait(void);
void pbs_h264_discontinuity(void);
int pbs_h264_take_restart_request(void);
/* Only enable on backends that can reopen all advertised capture profiles. */
void pbs_h264_enable_profiles(void);
void pbs_h264_enable_profile_mask(unsigned mask);
int pbs_h264_take_profile_request(int *height);
void pbs_h264_profile_failed(void);
/* Web-configured bitrate ceiling, kbps; 0 = use the startup rate. */
int pbs_h264_take_bitrate_request(void);
/* Force an IDR on the running session (used after a live bitrate change). */
void pbs_h264_request_keyframe(void);
/* Park after /stop: keep the HTTP listener and wait for POST /capture/start.
 * Returns once a resume was requested. 0 on resume, -1 if the listener died. */
int pbs_h264_park_until_resume(int port);
/* True while parked between /stop and /capture/start. */
int pbs_h264_is_parked(void);
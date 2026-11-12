// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#define PBS_INPUT_TOKEN_SIZE 33
/* 0x20000 is the PS (home) button, 0x10000 is the screenshot/share button.
 * Both are system keys on the pad report; the virtual device accepts them as
 * regular bits, they were simply filtered out by this mask before. */
#define PBS_INPUT_BUTTONS 0x0033fffeu
typedef struct {
  uint32_t buttons;
  uint8_t lx, ly, rx, ry, l2, r2;
} pbs_input_state;
typedef struct {
  int (*connect)(void *);
  int (*write)(void *, const pbs_input_state *);
  /* Ends the viewer session but keeps the backend device alive. */
  int (*disconnect)(void *);
  /* Final teardown once the payload is leaving; destroys the backend device. */
  int (*destroy)(void *);
} pbs_input_backend;
typedef struct {
  int supported, connected, cleanup_required, last_rc;
  uint64_t packets;
} pbs_input_status;

/* Configure before starting the HTTP server. One owner, latest state wins. */
void pbs_input_configure(const pbs_input_backend *backend, void *opaque);
int pbs_input_start(void);
void pbs_input_stop(void);
void pbs_input_wait(void);
/* Final teardown after pbs_input_wait(); destroys the persistent device. */
void pbs_input_shutdown(void);
int pbs_input_connect(char token[PBS_INPUT_TOKEN_SIZE]);
int pbs_input_submit(const char *token, uint32_t sequence, const pbs_input_state *state);
int pbs_input_disconnect(const char *token);
void pbs_input_get_status(pbs_input_status *status);
int pbs_input_parse_state(const char *text, uint32_t *sequence, pbs_input_state *state);

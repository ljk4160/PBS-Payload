/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PBS_POWER_H
#define PBS_POWER_H

/* Ordered as the web UI sends them. */
enum {
  PBS_POWER_STANDBY = 0,
  PBS_POWER_REBOOT = 1,
  PBS_POWER_OFF = 2
};

/* Fire-and-forget: returns immediately, the actual Sony call happens on a
 * detached thread after a short delay so the HTTP reply has time to flush
 * (all three actions tear the network down). */
void pbs_power_request(int action);

#endif

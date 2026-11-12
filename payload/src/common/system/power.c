/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console power control: standby / reboot / power off.
 *
 * The "request" family lives in libSceSystemService but the payload SDK ships
 * no public header for it, so the two stable entry points are forward
 * declared here (they are referenced at link time, which also guarantees the
 * library is mapped). The standby/turn-off entry points are resolved through
 * dlsym because not every firmware exports them - a missing symbol must
 * degrade gracefully instead of failing our own load.
 */
#include "common/system/power.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

extern int sceSystemServiceRequestPowerOff(void);
extern int sceSystemServiceRequestReboot(void);

typedef int (*pbs_power_fn0)(void);
typedef int (*pbs_power_fn1)(int);

static void pbs_power_run(int action) {
  if (action == PBS_POWER_REBOOT) {
    pbs_power_fn0 reboot = (pbs_power_fn0)dlsym(RTLD_DEFAULT, "sceSystemServiceRequestReboot");
    if (!reboot) reboot = (pbs_power_fn0)dlsym(RTLD_DEFAULT, "sceSystemStateMgrReboot");
    if (reboot) reboot();
    else (void)sceSystemServiceRequestReboot();
    return;
  }
  if (action == PBS_POWER_OFF) {
    pbs_power_fn1 off = (pbs_power_fn1)dlsym(RTLD_DEFAULT, "sceSystemStateMgrTurnOff");
    if (off) {
      off(0);
      return;
    }
    pbs_power_fn0 power_off =
        (pbs_power_fn0)dlsym(RTLD_DEFAULT, "sceSystemServiceRequestPowerOff");
    if (power_off) power_off();
    else (void)sceSystemServiceRequestPowerOff();
    return;
  }
  pbs_power_fn0 standby = (pbs_power_fn0)dlsym(RTLD_DEFAULT, "sceSystemStateMgrEnterStandby");
  if (standby) standby();
}

static void *pbs_power_thread(void *arg) {
  int action = (int)(intptr_t)arg;
  /* Let the HTTP response reach the browser before the console drops link. */
  usleep(400000);
  pbs_power_run(action);
  return NULL;
}

void pbs_power_request(int action) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, pbs_power_thread, (void *)(intptr_t)action) == 0)
    pthread_detach(thread);
}

// SPDX-License-Identifier: GPL-3.0-or-later
/* Native virtual-pad exports replace the legacy prototype's fixed ioctls,
 * profile tables and demo input loop. No target-process injection or offsets. */
#include "platform/ps5/controller.h"
#if PBS_PS5_CONTROLLER
#include "common/input/controller.h"
#include "platform/ps5/offset/profile.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ps5/kernel.h>

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, uint32_t *);
/* Link-time libScePad imports (SDK stub lib). On FW12.00 resolving exports on
 * the fly over a fresh LoadStartModule handle crashed the payload before the
 * first scePadInit log line, so prefer the statically bound imports and keep
 * the runtime path only as a fallback for SDKs without the stub library. */
int scePadInit(void);
int scePadSetProcessPrivilege(int);
int scePadVirtualDeviceAddDevice(const void *, void *);
int scePadVirtualDeviceInsertData(int, const void *);
int scePadVirtualDeviceDeleteDevice(int);
typedef struct {
  int (*init)(void);
  int (*privilege)(int);
  int (*add)(const void *, void *);
  int (*insert)(int, const void *);
  int (*remove)(int);
  uint32_t device;
  int ready, privileged, owned, uncertain, unhealthy;
} native_pad;
static native_pad pad;
#ifndef PBS_CONTROLLER_LOG
#define PBS_CONTROLLER_LOG "/data/pbs/controller.log"
#endif
static void log_result(const char *operation, int rc) {
  FILE *f = fopen(PBS_CONTROLLER_LOG, "a");
  if (f) {
    fprintf(f, "fw=0x%08x %s rc=0x%08x device=0x%08x owned=%d\n", kernel_get_fw_version(),
            operation, (unsigned)rc, pad.device, pad.owned);
    fclose(f);
  }
}
static int prepare(void) {
  if (pad.ready) {
    return 0;
  }
  uint32_t module = 0;
  if (kernel_dynlib_handle(getpid(), "libScePad.sprx", &module)) {
    int rc = sceKernelLoadStartModule("/system/common/lib/libScePad.sprx", 0, NULL, 0, NULL, NULL);
    log_result("Load libScePad", rc);
    /* LoadStartModule returns the loaded module handle on success.  Do not
     * discard it and look the module up by name again: on newer firmwares
     * (observed on 10.60) the load succeeds with e.g. 0x2023 while the
     * immediate name lookup still fails, preventing all controller exports
     * from being resolved. */
    if (rc <= 0) {
      return -1;
    }
    module = (uint32_t)rc;
    /* Prefer the link-time imports bound to the SDK stub lib. Resolving the
     * same exports over the fresh handle crashed FW12.00 payloads here. */
    pad.init = scePadInit;
    pad.privilege = scePadSetProcessPrivilege;
    pad.add = scePadVirtualDeviceAddDevice;
    pad.insert = scePadVirtualDeviceInsertData;
    pad.remove = scePadVirtualDeviceDeleteDevice;
    log_result("Use link-time libScePad imports", 0);
    goto exports_ready;
  }
  pad.init = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadInit");
  pad.privilege = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadSetProcessPrivilege");
  pad.add = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadVirtualDeviceAddDevice");
  pad.insert = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadVirtualDeviceInsertData");
  pad.remove = (void *)kernel_dynlib_dlsym(getpid(), module, "scePadVirtualDeviceDeleteDevice");
exports_ready:
  if ((intptr_t)pad.init <= 0 || (intptr_t)pad.privilege <= 0 || (intptr_t)pad.add <= 0 ||
      (intptr_t)pad.insert <= 0 || (intptr_t)pad.remove <= 0) {
    log_result("Required virtual-pad exports unavailable", -1);
    return -1;
  }
  int rc = pad.init();
  log_result("scePadInit", rc);
  if (rc) {
    return rc;
  }
  pad.ready = 1;
  return 0;
}
static int connect_pad(void *unused) {
  (void)unused;
  if (pad.uncertain || pad.unhealthy) {
    return -1;
  }
  int rc = prepare();
  if (rc) {
    return rc;
  }
  if (!pad.privileged) {
    rc = pad.privilege(1);
    log_result("SetProcessPrivilege(1)", rc);
    if (rc) {
      return rc;
    }
    pad.privileged = 1;
  }
  /* The virtual device belongs to the payload, not to a viewer session.
   * Reuse it for later sessions instead of creating and deleting one device
   * per connect: repeated Add/Delete exhausts libScePad's device pool. */
  if (pad.owned) {
    return 0;
  }
  /* AddDevice reads the caller's u32 parameter and profile byte at +4.
   * The library selects its firmware-native kernel representation itself. */
  unsigned char request[0x40] = {0}, output[16] = {0};
  request[4] = 1;
  memset(output, 0xff, 4);
  rc = pad.add(request, output);
  if (!rc) {
    memcpy(&pad.device, output, 4);
    if (pad.device == UINT32_MAX) {
      pad.uncertain = 1;
      rc = -1;
    } else {
      pad.owned = 1;
    }
  }
  log_result("VirtualDeviceAddDevice(profile=1)", rc);
  return rc;
}
static int write_pad(void *unused, const pbs_input_state *state) {
  (void)unused;
  if (!pad.owned) {
    return -1;
  }
  /* Stable exported InsertData input: buttons u32, sticks u8[4], triggers
   * u8[2], followed by zeroed motion/touch fields. Native wrapper owns the
   * larger driver packet, including its connection flag. */
  unsigned char data[0x80] = {0};
  memcpy(data, &state->buttons, 4);
  data[4] = state->lx;
  data[5] = state->ly;
  data[6] = state->rx;
  data[7] = state->ry;
  data[8] = state->l2;
  data[9] = state->r2;
  float orientation_w = 1.0f;
  memcpy(data + 0x18, &orientation_w, 4);
  int rc = pad.insert((int)pad.device, data);
  if (state->buttons & 0x10000u) {
    /* PS is a system key: log what reached the driver and what it answered so a
     * rejected tap is visible instead of silent. */
    FILE *lf = fopen("/data/pbs/inject.txt", "a");
    if (lf) { fprintf(lf, "virtual PS: buttons=0x%x rc=%d\n", state->buttons, rc); fclose(lf); }
  }
  if (rc) {
    /* One rejected state (an unsupported button bit) must not quarantine the
     * whole device: that is what dropped the controller as soon as an unknown
     * key was probed. Skip the frame and keep the session alive. */
    static int rejected_logged;
    if (!rejected_logged) { log_result("VirtualDeviceInsertData", rc); rejected_logged = 1; }
  }
  return rc;
}
static int disconnect_pad(void *unused) {
  (void)unused;
  if (pad.uncertain) {
    log_result("Unknown device ID; no blind delete or reconnect", -1);
    return -1;
  }
  if (pad.unhealthy) {
    log_result("Virtual device quarantined; reconnect disabled until teardown", -1);
    return -1;
  }
  /* Session release only: keep the device and process privilege registered
   * until destroy_pad(), so a reconnect never calls AddDevice again. */
  return 0;
}
static int destroy_pad(void *unused) {
  (void)unused;
  if (pad.uncertain) {
    log_result("Unknown device ID; no blind delete or reconnect", -1);
    return -1;
  }
  if (pad.owned) {
    int rc = pad.remove((int)pad.device);
    log_result("VirtualDeviceDeleteDevice", rc);
    if (rc) {
      return rc;
    }
    pad.owned = 0;
    pad.device = 0;
  }
  if (pad.privileged) {
    int rc = pad.privilege(0);
    log_result("SetProcessPrivilege(0)", rc);
    if (rc) {
      return rc;
    }
    pad.privileged = 0;
  }
  pad.ready = 0;
  pad.unhealthy = 0;
  return 0;
}
void pbs_ps5_controller_configure(void) {
  static const pbs_input_backend backend = {connect_pad, write_pad, disconnect_pad, destroy_pad};
  /* Follow the same exact/family selector as video; no copied firmware list. */
  if (pbs_ps5_find_offsets(kernel_get_fw_version())) {
    pbs_input_configure(&backend, NULL);
  }
}
#endif

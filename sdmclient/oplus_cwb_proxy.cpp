/*
 * SPDX-FileCopyrightText: 2026 AlphaDroid
 * SPDX-License-Identifier: Apache-2.0
 */

#include "oplus_cwb_proxy.h"

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define OPLUS_CWB_LOG_TAG "OplusCwbProxy"
#define CWB_LOGI(...) \
  __android_log_print(ANDROID_LOG_INFO, OPLUS_CWB_LOG_TAG, __VA_ARGS__)
#define CWB_LOGE(...) \
  __android_log_print(ANDROID_LOG_ERROR, OPLUS_CWB_LOG_TAG, __VA_ARGS__)

namespace sdm {

namespace {

constexpr size_t kCwbProxySize = 0x20;
// CwbProxy layout after initialize(): shared_ptr<CwbService> at +0x10
// (get() pointer in the first word). getRGBValue requires:
//   +0x2c8 status  (CwbService::updateCwbStatus)
//   +0x2c9 enable  (CwbService::enable) — libcwb_client never calls enable()
constexpr size_t kProxyServicePtr = 0x10;
constexpr size_t kServiceEnableFlag = 0x2c9;
// Verbose getRGBValue logging inside libcwb_qcom_aidl (+0xd4)
constexpr size_t kServiceDebugFlag = 0xd4;

constexpr const char kLib[] = "libcwb_qcom_aidl.so";
constexpr const char kInitSym[] = "_ZN8CwbProxy10initializeEv";
constexpr const char kUpdateSym[] = "_ZN8CwbProxy15updateCwbStatusEb";
constexpr const char kSetPowerSym[] = "_ZN8CwbProxy12setPowerModeEmi";
constexpr const char kInvalidCacheSym[] = "_ZN8CwbProxy12invalidCacheEv";
constexpr const char kNotifyConfigSym[] = "_ZN8CwbProxy18notifyConfigChangeEmj";
constexpr const char kNotifyColorSym[] =
    "_ZN8CwbProxy21notifyColorModeChangeEmii";
constexpr const char kNotifySym[] =
    "_ZN4aidl6vendor5oplus8hardware3cwb14implementation10CwbService15notifyCwbStatusEv";
constexpr const char kGateProp[] = "ro.vendor.oplus.display.cwb.display_id";
// SDMPowerMode::POWER_MODE_ON — CwbService treats 0 as screen off.
constexpr int32_t kPowerModeOn = 2;

using InitializeFn = int (*)(void *self);
using UpdateCwbStatusFn = void (*)(void *self, bool ready);
using SetPowerModeFn = void (*)(void *self, uint64_t display, int32_t mode);
using InvalidCacheFn = void (*)(void *self);
using NotifyConfigFn = void (*)(void *self, uint64_t display, uint32_t config);
using NotifyColorFn = void (*)(void *self, uint64_t display, int32_t mode,
                               int32_t intent);
using NotifyCwbStatusFn = void (*)(void *service);

alignas(16) char gProxy[kCwbProxySize];
UpdateCwbStatusFn gUpdate = nullptr;
SetPowerModeFn gSetPower = nullptr;
InvalidCacheFn gInvalidCache = nullptr;
NotifyConfigFn gNotifyConfig = nullptr;
NotifyColorFn gNotifyColor = nullptr;
NotifyCwbStatusFn gNotify = nullptr;
bool gStarted = false;
bool gLoggedEnable = false;

void *ServicePtr() {
  return *reinterpret_cast<void **>(gProxy + kProxyServicePtr);
}

void EnsureEnabled(bool notify) {
  void *service = ServicePtr();
  if (service == nullptr) {
    return;
  }

  auto *bytes = reinterpret_cast<uint8_t *>(service);
  const bool was = bytes[kServiceEnableFlag] != 0;
  bytes[kServiceEnableFlag] = 1;
  bytes[kServiceDebugFlag] = 1;
  if (notify && gNotify != nullptr) {
    gNotify(service);
  }
  if (!gLoggedEnable || !was) {
    CWB_LOGI("CwbService enabled (+0x2c9=%u debug=%u)", bytes[kServiceEnableFlag],
             bytes[kServiceDebugFlag]);
    gLoggedEnable = true;
  }
}

void *StatusLoop(void *) {
  while (gUpdate != nullptr) {
    gUpdate(gProxy, true);
    // Re-assert enable/debug bits in case something clears them; no notify spam.
    EnsureEnabled(false);
    usleep(200 * 1000);
  }
  return nullptr;
}

bool GateEnabled() {
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(kGateProp, value) <= 0) {
    return false;
  }
  return value[0] != '\0';
}

}  // namespace

void InitOplusCwbProxy() {
  if (gStarted) {
    return;
  }
  if (!GateEnabled()) {
    return;
  }

  void *handle = dlopen(kLib, RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    CWB_LOGE("dlopen(%s) failed: %s", kLib, dlerror());
    return;
  }

  auto initialize = reinterpret_cast<InitializeFn>(dlsym(handle, kInitSym));
  gUpdate = reinterpret_cast<UpdateCwbStatusFn>(dlsym(handle, kUpdateSym));
  gSetPower = reinterpret_cast<SetPowerModeFn>(dlsym(handle, kSetPowerSym));
  gInvalidCache =
      reinterpret_cast<InvalidCacheFn>(dlsym(handle, kInvalidCacheSym));
  gNotifyConfig =
      reinterpret_cast<NotifyConfigFn>(dlsym(handle, kNotifyConfigSym));
  gNotifyColor = reinterpret_cast<NotifyColorFn>(dlsym(handle, kNotifyColorSym));
  gNotify = reinterpret_cast<NotifyCwbStatusFn>(dlsym(handle, kNotifySym));
  if (initialize == nullptr || gUpdate == nullptr) {
    CWB_LOGE("dlsym CwbProxy symbols failed: %s", dlerror());
    gUpdate = nullptr;
    return;
  }
  if (gSetPower == nullptr) {
    CWB_LOGE("dlsym setPowerMode failed: %s", dlerror());
  }
  if (gInvalidCache == nullptr) {
    CWB_LOGE("dlsym invalidCache failed: %s", dlerror());
  }

  memset(gProxy, 0, sizeof(gProxy));
  const int ret = initialize(gProxy);
  CWB_LOGI("CwbProxy::initialize() -> %d (in composer/libsdmclient)", ret);
  if (ret != 0) {
    gUpdate = nullptr;
    return;
  }

  gUpdate(gProxy, true);
  EnsureEnabled(true);
  // PostInit runs after the panel is already ON; stock SetPowerMode may have
  // fired before CwbProxy existed, so push ON for the gated display id.
  char disp[PROP_VALUE_MAX] = {};
  __system_property_get(kGateProp, disp);
  const uint64_t display_id = static_cast<uint64_t>(strtoull(disp, nullptr, 0));
  if (gSetPower != nullptr) {
    gSetPower(gProxy, display_id, kPowerModeOn);
    CWB_LOGI("CwbProxy::setPowerMode(display=%llu, ON)",
             (unsigned long long)display_id);
  }
  gStarted = true;

  pthread_t t;
  if (pthread_create(&t, nullptr, StatusLoop, nullptr) == 0) {
    pthread_detach(t);
  }
  CWB_LOGI("in-composer ICwbService host up (invalidCache=%d)",
           gInvalidCache != nullptr);
}

void OplusCwbSetPowerMode(uint64_t display, int32_t mode) {
  if (!gStarted || gSetPower == nullptr) {
    return;
  }
  gSetPower(gProxy, display, mode);
}

void OplusCwbInvalidCache() {
  if (!gStarted || gInvalidCache == nullptr) {
    return;
  }
  gInvalidCache(gProxy);
}

void OplusCwbNotifyConfigChange(uint64_t display, uint32_t config) {
  if (!gStarted || gNotifyConfig == nullptr) {
    return;
  }
  gNotifyConfig(gProxy, display, config);
}

void OplusCwbNotifyColorModeChange(uint64_t display, int32_t mode,
                                   int32_t intent) {
  if (!gStarted || gNotifyColor == nullptr) {
    return;
  }
  gNotifyColor(gProxy, display, mode, intent);
}

}  // namespace sdm

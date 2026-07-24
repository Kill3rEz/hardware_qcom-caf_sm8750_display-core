/*
 * SPDX-FileCopyrightText: 2026 AlphaDroid
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stock ColorOS calls CwbProxy::initialize() from ConcurrencyMgr::Init so
 * ICwbService lives in the composer process next to DisplayConfig / SDM CWB.
 * CAF never links libcwb_qcom_aidl; this brings that path up when OPLUS CWB
 * props are present (dodge fusion-light bring-up).
 */
#ifndef __OPLUS_CWB_PROXY_H__
#define __OPLUS_CWB_PROXY_H__

#include <stdint.h>

namespace sdm {

// Idempotent. No-ops when ro.vendor.oplus.display.cwb.display_id is unset or
// libcwb_qcom_aidl.so is missing.
void InitOplusCwbProxy();

// Forward composer/SDM power-mode changes into CwbService. Without this,
// getRGBValue sees PowerMode 0 ("Screen power off") and returns 0-0-0.
void OplusCwbSetPowerMode(uint64_t display, int32_t mode);

// Stock: SDMDisplay::PostCommitLayerStack → CwbProxy::invalidCache every
// frame. Wakes WaitForLayerChange and clears the stale RGB cache.
void OplusCwbInvalidCache();

void OplusCwbNotifyConfigChange(uint64_t display, uint32_t config);
void OplusCwbNotifyColorModeChange(uint64_t display, int32_t mode,
                                   int32_t intent);

}  // namespace sdm

#endif  // __OPLUS_CWB_PROXY_H__

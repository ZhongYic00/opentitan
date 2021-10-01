// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pwrmgr.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/aon_timer_testutils.h"
#include "sw/device/lib/testing/check.h"
#include "sw/device/lib/testing/test_framework/test_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

static const dif_pwrmgr_wakeup_reason_t kWakeUpReasonTest = {
    .types = kDifPwrmgrWakeupTypeRequest,
    .request_sources = kDifPwrmgrWakeupRequestSourceFive,
};

static const dif_pwrmgr_wakeup_reason_t kWakeUpReasonPor = {
    .types = 0,
    .request_sources = 0,
};

const test_config_t kTestConfig;

bool compare_wakeup_reasons(const dif_pwrmgr_wakeup_reason_t *lhs,
                            const dif_pwrmgr_wakeup_reason_t *rhs) {
  return lhs->types == rhs->types &&
         lhs->request_sources == rhs->request_sources;
}

bool test_main(void) {
  dif_pwrmgr_t pwrmgr;
  dif_rstmgr_t rstmgr;

  // Initialize pwrmgr
  CHECK_DIF_OK(dif_pwrmgr_init(
      mmio_region_from_addr(TOP_EARLGREY_PWRMGR_AON_BASE_ADDR), &pwrmgr));

  // Initialize rstmgr since this will check some registers.
  CHECK_DIF_OK(dif_rstmgr_init(
      mmio_region_from_addr(TOP_EARLGREY_RSTMGR_AON_BASE_ADDR), &rstmgr));

  // Assuming the chip hasn't slept yet, wakeup reason should be empty.
  // Notice we are clear rstmgr's RESET_INFO, so after the aon wakeup there
  // is only one bit set.
  dif_pwrmgr_wakeup_reason_t wakeup_reason;
  CHECK_DIF_OK(dif_pwrmgr_wakeup_reason_get(&pwrmgr, &wakeup_reason));

  if (compare_wakeup_reasons(&wakeup_reason, &kWakeUpReasonPor)) {
    LOG_INFO("Powered up for the first time, begin test");

    LOG_INFO("Check the rstmgr reset_info is POR");
    dif_rstmgr_reset_info_bitfield_t info;
    CHECK_DIF_OK(dif_rstmgr_reset_info_get(&rstmgr, &info));
    CHECK(info == (dif_rstmgr_reset_info_bitfield_t)(kDifRstmgrResetInfoPor));

    // Clear reset_info.
    CHECK_DIF_OK(dif_rstmgr_reset_info_clear(&rstmgr));

    // Issue a wakeup signal in ~150us through the AON timer.
    //
    // At 200kHz, threshold of 30 is equal to 150us. There is an additional
    // ~4 cycle overhead for the CSR value to synchronize with the AON clock.
    // We should expect the wake up to trigger in ~170us. This is sufficient
    // time to allow pwrmgr config and the low power entry on WFI to complete.
    //
    // Adjust the threshold for Verilator since it runs on different clock
    // frequencies.
    uint32_t wakeup_threshold = 30;
    if (kDeviceType == kDeviceSimVerilator) {
      wakeup_threshold = 300;
    }
    dif_aon_timer_t aon_timer;
    CHECK(dif_aon_timer_init(
              (dif_aon_timer_params_t){
                  .base_addr = mmio_region_from_addr(
                      TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR),
              },
              &aon_timer) == kDifAonTimerOk);
    aon_timer_testutils_wakeup_config(&aon_timer, wakeup_threshold);

    // Enable low power on the next WFI with default settings.
    // All clocks and power domains are turned off during low power.
    dif_pwrmgr_domain_config_t config;
    // Issue #6504: USB clock in active power must be left enabled.
    config = kDifPwrmgrDomainOptionUsbClockInActivePower;

    CHECK_DIF_OK(dif_pwrmgr_set_request_sources(
        &pwrmgr, kDifPwrmgrReqTypeWakeup, kDifPwrmgrWakeupRequestSourceFive));
    CHECK_DIF_OK(dif_pwrmgr_set_domain_config(&pwrmgr, config));
    CHECK_DIF_OK(dif_pwrmgr_low_power_set_enabled(&pwrmgr, kDifToggleEnabled));

    // Enter low power mode.
    wait_for_interrupt();

  } else if (compare_wakeup_reasons(&wakeup_reason, &kWakeUpReasonTest)) {
    LOG_INFO("Aon timer wakeup detected");
    LOG_INFO("Check the rstmgr reset_info is LOW_POWER timer wakeup detected");
    dif_rstmgr_reset_info_bitfield_t info;
    CHECK_DIF_OK(dif_rstmgr_reset_info_get(&rstmgr, &info));
    CHECK(info ==
          (dif_rstmgr_reset_info_bitfield_t)(kDifRstmgrResetInfoLowPowerExit));
    return true;

  } else {
    LOG_ERROR("Unexpected wakeup detected: type = %d, request_source = %d",
              wakeup_reason.types, wakeup_reason.request_sources);
    return false;
  }

  return false;
}

/*
 * LP (40 MHz) core standby manager for the ESP32-P4 front-end.
 *
 * tiny386's SeaBIOS APM hook reports a guest-initiated low-power request by
 * writing the bare ASCII message "Standby"/"Suspend" to port 0x8900 (see
 * pc.c).  When that happens the guest has stopped and is waiting to be
 * resumed, so the emulator can park the HP cores and let the LP core keep the
 * RTC domain alive as the wake supervisor:
 *
 *   - Core 0 calls pc_main() -> pm_suspend(), which runs the LP firmware with
 *     an LP timer wake source and enters light sleep.  The emulated machine
 *     lives in PSRAM, which light sleep must keep powered for the state to
 *     survive -- verify retention on your module.
 *   - The LP core wakes every LP_PM_WAKE_PERIOD_US, records the wake cause and
 *     asserts the HP wakeup trigger, which brings the HP cores back up.
 *   - USB HS activity (a keyboard/mouse using remote wakeup) and an optional
 *     LP_PM_WAKE_GPIO button also wake the HP cores, so the guest resumes as
 *     soon as the user touches the machine.  A safety timeout bounds the
 *     standby regardless.
 */
#ifdef USE_LP_CORE_PM

#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "ulp_lp_core.h"

#include "lp_pm.h"
#include "../../pc.h"

/* LP supervisor image embedded by ulp_embed_binary (see CMakeLists.txt). */
extern const uint8_t lp_pm_bin_start[] asm("_binary_lp_pm_bin_start");
extern const uint8_t lp_pm_bin_end[] asm("_binary_lp_pm_bin_end");

static const char *TAG = "pm";
static bool s_lp_ready;

void pm_init(void)
{
	size_t len = lp_pm_bin_end - lp_pm_bin_start;
	esp_err_t err = ulp_lp_core_load_binary(lp_pm_bin_start, len);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "failed to load LP supervisor firmware: %s",
			 esp_err_to_name(err));
		return;
	}
	s_lp_ready = true;
	ESP_LOGI(TAG, "LP standby supervisor loaded (%d bytes)", (int)len);
}

/* Park the HP cores while the guest is suspended.  Runs on Core 0 (the
 * emulation core) and returns once the guest should be resumed. */
void pm_suspend(PC *pc)
{
	if (!s_lp_ready)
		return;

	ESP_LOGI(TAG, "guest APM %s -> LP core standby",
		 pc->apm_suspend == 2 ? "suspend" : "standby");

	int64_t started = esp_timer_get_time();

	while (pc->apm_suspend) {
		ulp_lp_core_cfg_t cfg = {
			.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
			.lp_timer_sleep_duration_us = LP_PM_WAKE_PERIOD_US,
		};
		if (ulp_lp_core_run(&cfg) != ESP_OK)
			break;

		esp_sleep_enable_ulp_wakeup();
		/* USB HS activity brings the HP cores back up while light
		 * sleeping, so keyboard/mouse input still wakes the guest. */
		if (esp_sleep_enable_usb_wakeup() != ESP_OK)
			ESP_LOGW(TAG, "USB wakeup not available");
#ifdef LP_PM_WAKE_GPIO
		esp_sleep_enable_ext1_wakeup(1ULL << LP_PM_WAKE_GPIO,
					     ESP_EXT1_WAKEUP_ANY_LOW);
#endif

		esp_light_sleep_start();
		ulp_lp_core_stop();

		/* ESP_SLEEP_WAKEUP_ULP just means the LP timer ticked: the guest
		 * stays parked and we go straight back to sleep. */
		uint32_t causes = esp_sleep_get_wakeup_causes();
		if (causes & ((1U << ESP_SLEEP_WAKEUP_USB) |
			      (1U << ESP_SLEEP_WAKEUP_EXT1)))
			break;
		if (esp_timer_get_time() - started >= LP_PM_MAX_SUSPEND_US)
			break;
	}

	ulp_lp_core_stop();
	pc->apm_suspend = 0;
	ESP_LOGI(TAG, "guest resumed (LP wakes: %u, last cause: %u)",
		 (unsigned)ulp_pm_wake_count, (unsigned)ulp_pm_wake_cause);
}

#endif /* USE_LP_CORE_PM */

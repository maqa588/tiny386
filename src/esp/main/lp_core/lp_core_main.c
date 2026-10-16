/*
 * LP (40 MHz) core standby supervisor for the ESP32-P4 front-end.
 *
 * The HP core loads this firmware once (pm.c) and arms it with
 * ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER before parking the HP cores in light
 * sleep.  While the HP cores are down the LP core keeps the RTC domain alive
 * and wakes itself every LP_PM_WAKE_PERIOD_US; each wake runs main(), where we
 * record the wake cause and ask the main processor to come back up.  Returning
 * from main() lets the LP startup code re-arm the timer and halt the LP core
 * again, so the cycle repeats without the HP cores having to re-arm anything.
 *
 * The counters below are exported to the HP core through the generated
 * lp_pm.h / lp_pm.ld as ulp_pm_wake_count and ulp_pm_wake_cause (see pm.c).
 */
#include <stdint.h>
#include "ulp_lp_core_utils.h"

volatile uint32_t pm_wake_count;	/* number of times the LP core has woken */
volatile uint32_t pm_wake_cause;	/* last ulp_lp_core_get_wakeup_cause() */

int main(void)
{
	pm_wake_count++;
	pm_wake_cause = ulp_lp_core_get_wakeup_cause();

	/* This is what actually asserts the HP wakeup trigger; without it the
	 * HP cores would stay parked even though the LP core is running. */
	ulp_lp_core_wakeup_main_processor();

	return 0;
}

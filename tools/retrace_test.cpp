/*
 * retrace_test — verifies VBlank interrupt delivery end to end.
 *
 * BScreen::WaitForRetrace() blocks on the intel_extreme vblank
 * semaphore, released by the driver's interrupt handler: if this
 * passes, display interrupts flow. Guards the MSI edge-loss fix
 * (DEIER master mask in intel_interrupt_handler): before that fix a
 * GT-interrupt race could kill all delivery, WaitForRetrace timed out
 * forever and GLTeapot's render thread hung until app_server killed it.
 *
 * Build: g++ -Wall -O2 -o retrace_test retrace_test.cpp -lbe
 * Exit code: 0 = all waits OK at a sane period, 1 = timeout/failure.
 */

#include <Application.h>
#include <Screen.h>

#include <stdio.h>
#include <string.h>

int
main()
{
	BApplication app("application/x-vnd.retrace-test");
	BScreen screen;
	if (!screen.IsValid()) {
		printf("retrace: no valid screen\n");
		return 1;
	}

	int failures = 0;
	for (int i = 0; i < 5; i++) {
		bigtime_t start = system_time();
		status_t status = screen.WaitForRetrace(2000000);
		bigtime_t elapsed = system_time() - start;

		bool ok = status == B_OK && elapsed < 100000;
		printf("WaitForRetrace #%d: %s (%.1f ms)\n", i,
			status == B_OK ? "OK"
				: (status == B_TIMED_OUT ? "TIMED OUT" : strerror(status)),
			elapsed / 1000.0);
		if (!ok)
			failures++;
	}

	return failures > 0 ? 1 : 0;
}

/*
 * blit2d_bench — 2D throughput micro-benchmark for the intel_extreme
 * accelerant. Draws solid FillRects and CopyBits blits onto an on-screen
 * BView so the operations go through app_server's hardware path, i.e.
 * the accelerant's fill_rectangle / screen_to_screen_blit hooks.
 *
 * Used to A/B the two 2D submission mechanisms:
 *   - GEM execbuffer path (M4, default)
 *   - legacy ring path (toggle file
 *     /boot/home/config/settings/intel_2d_legacy present)
 *
 * The bench does NOT know which path is active; it prints whether the
 * toggle file is present so each run is self-labelling. Flip the toggle
 * and re-run to compare.
 *
 * Build: g++ -Wall -O2 -o blit2d_bench blit2d_bench.cpp -lbe
 */

#include <Application.h>
#include <View.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


static const int kW = 1200;
static const int kH = 700;
static const char* kLegacyToggle =
	"/boot/home/config/settings/intel_2d_legacy";


class BenchView : public BView {
public:
	BenchView(BRect frame)
		:
		BView(frame, "bench", B_FOLLOW_ALL_SIDES, B_WILL_DRAW)
	{
		SetViewColor(0, 0, 0);
	}
};


static BWindow* sWindow;
static BenchView* sView;


// Batch throughput: N fills, one Sync at the end. Measures how fast the
// submission path drains a stream of BLTs.
static double
bench_fill_throughput(int count, int size)
{
	if (!sWindow->LockLooper())
		return 0.0;

	bigtime_t start = system_time();
	int x = 0, y = 0;
	for (int i = 0; i < count; i++) {
		rgb_color c = { (uint8)(i * 7), (uint8)(i * 13),
			(uint8)(i * 29), 255 };
		sView->SetHighColor(c);
		sView->FillRect(BRect(x, y, x + size - 1, y + size - 1));
		x += 17;
		if (x + size > kW) { x = 0; y += 19; if (y + size > kH) y = 0; }
	}
	sView->Sync();
	bigtime_t elapsed = system_time() - start;

	sWindow->UnlockLooper();
	return count * 1000000.0 / (double)elapsed;
}


// Per-op latency: Sync after every fill. Measures the full round-trip
// (submit + GPU + retire) of a single BLT.
static double
bench_fill_latency_us(int count, int size)
{
	if (!sWindow->LockLooper())
		return 0.0;

	bigtime_t start = system_time();
	int x = 0, y = 0;
	for (int i = 0; i < count; i++) {
		sView->SetHighColor(i & 1 ? 200 : 40, 80, 160, 255);
		sView->FillRect(BRect(x, y, x + size - 1, y + size - 1));
		sView->Sync();
		x += 23;
		if (x + size > kW) { x = 0; y += 29; if (y + size > kH) y = 0; }
	}
	bigtime_t elapsed = system_time() - start;

	sWindow->UnlockLooper();
	return (double)elapsed / count;
}


// Blit throughput: CopyBits within the view (screen_to_screen_blit).
static double
bench_blit_throughput(int count, int size)
{
	if (!sWindow->LockLooper())
		return 0.0;

	// Seed a source region with something to copy.
	sView->SetHighColor(120, 200, 90, 255);
	sView->FillRect(BRect(0, 0, size - 1, size - 1));
	sView->Sync();

	bigtime_t start = system_time();
	int x = size, y = 0;
	BRect src(0, 0, size - 1, size - 1);
	for (int i = 0; i < count; i++) {
		sView->CopyBits(src, BRect(x, y, x + size - 1, y + size - 1));
		x += 31;
		if (x + size > kW) { x = size; y += 37; if (y + size > kH) y = 0; }
	}
	sView->Sync();
	bigtime_t elapsed = system_time() - start;

	sWindow->UnlockLooper();
	return count * 1000000.0 / (double)elapsed;
}


static int32
bench_thread(void*)
{
	snooze(500000);		// let the window map + first Draw settle

	bool legacy = access(kLegacyToggle, F_OK) == 0;
	printf("2D path: %s\n", legacy
		? "LEGACY RING (toggle present)"
		: "GEM EXECBUFFER (M4, default)");

	const int size = 128;

	// Warm up (first submissions pay one-time setup)
	bench_fill_throughput(2000, size);

	double fillTp = bench_fill_throughput(20000, size);
	double blitTp = bench_blit_throughput(20000, size);
	double fillLat = bench_fill_latency_us(2000, size);

	printf("  FillRect throughput : %10.0f ops/s\n", fillTp);
	printf("  CopyBits throughput : %10.0f ops/s\n", blitTp);
	printf("  FillRect latency    : %10.2f us/op (sync each)\n", fillLat);

	be_app->PostMessage(B_QUIT_REQUESTED);
	return 0;
}


class BenchApp : public BApplication {
public:
	BenchApp()
		:
		BApplication("application/x-vnd.blit2d-bench")
	{
	}

	virtual void ReadyToRun()
	{
		sWindow = new BWindow(BRect(40, 40, 40 + kW, 40 + kH),
			"blit2d_bench", B_TITLED_WINDOW,
			B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
		sView = new BenchView(sWindow->Bounds());
		sWindow->AddChild(sView);
		sWindow->Show();

		thread_id t = spawn_thread(bench_thread, "bench",
			B_NORMAL_PRIORITY, NULL);
		resume_thread(t);
	}
};


int
main()
{
	BenchApp app;
	app.Run();
	return 0;
}

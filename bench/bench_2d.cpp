/*
 * Simple 2D accelerant benchmark for Haiku
 * Measures fill_rectangle and screen_to_screen_blit performance
 */

#include <Application.h>
#include <Window.h>
#include <View.h>
#include <Screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <OS.h>

#define NUM_RECTS 5000
#define NUM_BLITS 2000
#define WINDOW_W 800
#define WINDOW_H 600

class BenchView : public BView {
public:
	BenchView()
		: BView(BRect(0, 0, WINDOW_W - 1, WINDOW_H - 1), "bench",
			B_FOLLOW_ALL, B_WILL_DRAW)
	{
	}

	void AttachedToWindow()
	{
		Window()->PostMessage('BNCH');
	}

	void DetectDriver()
	{
		FILE* fp = popen("listimage 2>/dev/null | grep accelerant", "r");
		if (fp) {
			char line[512];
			while (fgets(line, sizeof(line), fp)) {
				// strip newline
				char* nl = strchr(line, '\n');
				if (nl) *nl = 0;
				if (strstr(line, "non-packaged"))
					printf("Driver: PATCHED (non-packaged)\n");
				else if (strstr(line, "accelerant"))
					printf("Driver: SYSTEM (package)\n");
			}
			pclose(fp);
		}
	}

	void RunBenchmark()
	{
		bigtime_t start, elapsed;
		BRect bounds = Bounds();

		static int runNumber = 0;
		runNumber++;

		printf("=== Haiku 2D Benchmark - Run #%d ===\n", runNumber);
		DetectDriver();
		printf("Window: %dx%d\n\n", WINDOW_W, WINDOW_H);

		// Warm up
		for (int i = 0; i < 100; i++) {
			SetHighColor(0, 0, 0);
			FillRect(bounds);
		}
		Sync();

		// Test 1: FillRect (solid color)
		srand(12345);
		start = system_time();
		for (int i = 0; i < NUM_RECTS; i++) {
			SetHighColor(rand() % 256, rand() % 256, rand() % 256);
			float x1 = rand() % WINDOW_W;
			float y1 = rand() % WINDOW_H;
			float x2 = x1 + (rand() % 200) + 10;
			float y2 = y1 + (rand() % 200) + 10;
			FillRect(BRect(x1, y1, x2, y2));
		}
		Sync();
		elapsed = system_time() - start;
		printf("FillRect:  %d rects in %lld us (%.1f rects/sec)\n",
			NUM_RECTS, elapsed,
			(double)NUM_RECTS * 1000000.0 / elapsed);

		// Test 2: CopyBits (screen to screen blit)
		// First fill with a pattern
		for (int y = 0; y < WINDOW_H; y += 20) {
			SetHighColor(y % 256, (y * 3) % 256, (y * 7) % 256);
			FillRect(BRect(0, y, WINDOW_W - 1, y + 19));
		}
		Sync();

		srand(54321);
		start = system_time();
		for (int i = 0; i < NUM_BLITS; i++) {
			float sx = rand() % (WINDOW_W / 2);
			float sy = rand() % (WINDOW_H / 2);
			float w = (rand() % 200) + 50;
			float h = (rand() % 200) + 50;
			float dx = rand() % (WINDOW_W / 2);
			float dy = rand() % (WINDOW_H / 2);
			CopyBits(BRect(sx, sy, sx + w, sy + h),
				BRect(dx, dy, dx + w, dy + h));
		}
		Sync();
		elapsed = system_time() - start;
		printf("CopyBits:  %d blits in %lld us (%.1f blits/sec)\n",
			NUM_BLITS, elapsed,
			(double)NUM_BLITS * 1000000.0 / elapsed);

		// Test 3: InvertRect
		srand(99999);
		start = system_time();
		for (int i = 0; i < NUM_RECTS; i++) {
			float x1 = rand() % WINDOW_W;
			float y1 = rand() % WINDOW_H;
			float x2 = x1 + (rand() % 200) + 10;
			float y2 = y1 + (rand() % 200) + 10;
			InvertRect(BRect(x1, y1, x2, y2));
		}
		Sync();
		elapsed = system_time() - start;
		printf("InvertRect: %d rects in %lld us (%.1f rects/sec)\n",
			NUM_RECTS, elapsed,
			(double)NUM_RECTS * 1000000.0 / elapsed);

		// Test 4: Large fill (full window clear)
		start = system_time();
		for (int i = 0; i < 500; i++) {
			SetHighColor(i % 256, (i * 2) % 256, (i * 3) % 256);
			FillRect(bounds);
		}
		Sync();
		elapsed = system_time() - start;
		printf("FullClear: %d fills in %lld us (%.1f fills/sec, %.1f MB/s)\n",
			500, elapsed,
			500.0 * 1000000.0 / elapsed,
			500.0 * WINDOW_W * WINDOW_H * 4.0 / elapsed);

		// Test 5: Sync latency
		start = system_time();
		for (int i = 0; i < 1000; i++) {
			SetHighColor(i % 256, 0, 0);
			FillRect(BRect(0, 0, 10, 10));
			Sync();
		}
		elapsed = system_time() - start;
		printf("SyncLat:   1000 syncs in %lld us (%.1f us/sync)\n",
			elapsed, (double)elapsed / 1000.0);

		// Test 6: Small rects (per-command overhead dominated)
		srand(77777);
		start = system_time();
		for (int i = 0; i < 10000; i++) {
			SetHighColor(rand() % 256, rand() % 256, rand() % 256);
			int x = rand() % (WINDOW_W - 16);
			int y = rand() % (WINDOW_H - 16);
			FillRect(BRect(x, y, x + 15, y + 15));
		}
		Sync();
		elapsed = system_time() - start;
		printf("SmallRect: 10000 16x16 in %lld us (%.1f rects/sec)\n",
			elapsed, 10000.0 * 1000000.0 / elapsed);

		// Test 7: Large rects (bandwidth dominated)
		start = system_time();
		for (int i = 0; i < 100; i++) {
			SetHighColor(rand() % 256, rand() % 256, rand() % 256);
			FillRect(bounds);
		}
		Sync();
		elapsed = system_time() - start;
		printf("LargeRect: 100 fills in %lld us (%.1f MB/s)\n",
			elapsed, 100.0 * WINDOW_W * WINDOW_H * 4.0 / elapsed);

		printf("\n=== Benchmark complete ===\n");

		// Quit after benchmark
		be_app->PostMessage(B_QUIT_REQUESTED);
	}
};


class BenchWindow : public BWindow {
public:
	BenchWindow()
		: BWindow(BRect(50, 50, 50 + WINDOW_W - 1, 50 + WINDOW_H - 1),
			"2D Benchmark", B_TITLED_WINDOW, 0)
	{
		fView = new BenchView();
		AddChild(fView);
	}

	void MessageReceived(BMessage* msg)
	{
		if (msg->what == 'BNCH') {
			fView->RunBenchmark();
		} else {
			BWindow::MessageReceived(msg);
		}
	}

	bool QuitRequested() { be_app->PostMessage(B_QUIT_REQUESTED); return true; }

private:
	BenchView* fView;
};


class BenchApp : public BApplication {
public:
	BenchApp() : BApplication("application/x-vnd.bench2d") {}
	void ReadyToRun() { (new BenchWindow())->Show(); }
};


int main()
{
	BenchApp app;
	app.Run();
	return 0;
}

/*
 * gpu_monitor — live GPU-utilisation graph for the Ironlake (Gen5) GPU,
 * styled after the CPU-core graphs in ActivityMonitor / Pulse.
 *
 * "Busy" is sampled from the render command streamer, read straight out of
 * the cloned MMIO register area (reads are allowed from userspace; only
 * writes are ignored). The GPU counts as busy at an instant when either:
 *   - INSTDONE != 0xffffffff  (some pipeline stage is not "done"), or
 *   - ring HEAD != ring TAIL  (commands are queued but not yet retired).
 * A background thread samples this a few thousand times a second and every
 * ~100 ms reports the busy fraction, which the view scrolls across as a
 * filled graph (green -> yellow -> red).
 *
 * Build (from the accelerant dir so intel_extreme.h resolves):
 *   g++ -O2 -o gpu_monitor ../tools/gpu_monitor.cpp \
 *       -I/boot/system/develop/headers/private/graphics/intel_extreme \
 *       -I/boot/system/develop/headers/private/graphics \
 *       -I/boot/system/develop/headers/private/shared \
 *       -lbe
 * Run: ./gpu_monitor   (no reboot; it only reads registers)
 */
#include <Application.h>
#include <Window.h>
#include <View.h>
#include <String.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <OS.h>

#include "intel_extreme.h"

static const uint32 kActhd      = 0x2074;	// DWORD address being executed
static const uint32 kHeadOff    = 0x0004;
static const uint32 kTailOff    = 0x0000;
static const uint32 kRingMask   = 0x001ffffc;

// ---- shared register access ----
static volatile uint8*	sRegs = NULL;
static uint32			sRingBase = 0;
static area_id			sSharedArea = -1;
static area_id			sRegsArea = -1;
static int				sFd = -1;
static uint16			sDeviceId = 0;

static inline uint32
rd(uint32 off)
{
	return *(volatile uint32*)(sRegs + off);
}

// The CS is busy at an instant if the ring still has queued commands
// (HEAD != TAIL) or its active head advanced since the previous read
// (ACTHD moved = a command is being executed). INSTDONE is unusable as an
// idle signal on this part: bit 0 reads cleared even when fully idle.
static uint32 sLastActhd = 0;

static inline bool
gpu_busy()
{
	uint32 head = rd(sRingBase + kHeadOff) & kRingMask;
	uint32 tail = rd(sRingBase + kTailOff) & kRingMask;
	uint32 acthd = rd(kActhd);
	bool moved = (acthd != sLastActhd);
	sLastActhd = acthd;
	return (head != tail) || moved;
}

static status_t
open_gpu()
{
	sFd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
	if (sFd < 0)
		return errno;

	intel_get_private_data data;
	data.magic = INTEL_PRIVATE_DATA_MAGIC;
	if (ioctl(sFd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
		close(sFd);
		return errno;
	}

	intel_shared_info* info = NULL;
	sSharedArea = clone_area("gpumon shared", (void**)&info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
	if (sSharedArea < B_OK) {
		close(sFd);
		return sSharedArea;
	}

	sRingBase = info->primary_ring_buffer.register_base;
	sDeviceId = info->device_type.type;

	void* regs = NULL;
	sRegsArea = clone_area("gpumon regs", &regs, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, info->registers_area);
	if (sRegsArea < B_OK) {
		delete_area(sSharedArea);
		close(sFd);
		return sRegsArea;
	}
	sRegs = (volatile uint8*)regs;
	return B_OK;
}

static void
close_gpu()
{
	if (sRegsArea >= B_OK) delete_area(sRegsArea);
	if (sSharedArea >= B_OK) delete_area(sSharedArea);
	if (sFd >= 0) close(sFd);
}


// ---------------------------------------------------------------------------
// The scrolling graph view
// ---------------------------------------------------------------------------
static const int kHistory = 200;

class GraphView : public BView {
public:
	GraphView(BRect frame)
		: BView(frame, "graph", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS),
		  fCount(0), fCur(0.0f), fPeak(0.0f), fAvg(0.0f)
	{
		SetViewColor(16, 18, 22);
		for (int i = 0; i < kHistory; i++)
			fHist[i] = 0.0f;
	}

	void AddSample(float pct)
	{
		fCur = pct;
		for (int i = 0; i < kHistory - 1; i++)
			fHist[i] = fHist[i + 1];
		fHist[kHistory - 1] = pct;
		if (fCount < kHistory) fCount++;
		float sum = 0.0f; fPeak = 0.0f;
		for (int i = kHistory - fCount; i < kHistory; i++) {
			sum += fHist[i];
			if (fHist[i] > fPeak) fPeak = fHist[i];
		}
		fAvg = fCount ? sum / fCount : 0.0f;
	}

	static void level_color(float pct, rgb_color* c)
	{
		if (pct < 50.0f)      { c->red = 60;  c->green = 200; c->blue = 90; }
		else if (pct < 80.0f) { c->red = 220; c->green = 190; c->blue = 40; }
		else                  { c->red = 230; c->green = 70;  c->blue = 60; }
		c->alpha = 255;
	}

	virtual void Draw(BRect)
	{
		BRect b = Bounds();
		float top = 46.0f;                 // header band
		BRect graph(b.left + 10, top, b.right - 10, b.bottom - 24);

		// header: big percentage
		BString big;
		big.SetToFormat("%d%%", (int)(fCur + 0.5f));
		SetFontSize(30);
		rgb_color cc; level_color(fCur, &cc);
		SetHighColor(cc);
		DrawString(big.String(), BPoint(b.left + 12, 36));

		SetFontSize(11);
		SetHighColor(150, 155, 165);
		BString sub;
		sub.SetToFormat("GPU  Intel Ironlake  Gen5      avg %d%%   peak %d%%",
			(int)(fAvg + 0.5f), (int)(fPeak + 0.5f));
		DrawString(sub.String(), BPoint(b.left + 96, 24));

		// graph frame + gridlines
		SetHighColor(40, 44, 52);
		StrokeRect(graph);
		for (int g = 1; g < 4; g++) {
			float y = graph.top + (graph.Height() * g / 4.0f);
			SetHighColor(30, 33, 40);
			StrokeLine(BPoint(graph.left + 1, y), BPoint(graph.right - 1, y));
			SetHighColor(90, 95, 105);
			BString lbl; lbl.SetToFormat("%d", 100 - g * 25);
			DrawString(lbl.String(), BPoint(graph.right + 2 - 24, y + 4));
		}

		// filled bars, newest at the right
		float gw = graph.Width() - 2;
		float gh = graph.Height() - 2;
		float bx = graph.right - 1;
		float step = gw / (float)kHistory;
		for (int i = kHistory - 1; i >= 0; i--) {
			float pct = fHist[i];
			float h = gh * pct / 100.0f;
			rgb_color c; level_color(pct, &c);
			c.red = (uint8)(c.red * 0.85f);
			c.green = (uint8)(c.green * 0.85f);
			c.blue = (uint8)(c.blue * 0.85f);
			SetHighColor(c);
			float x1 = bx - step;
			FillRect(BRect(x1, graph.bottom - 1 - h, bx, graph.bottom - 1));
			bx -= step;
			if (bx < graph.left) break;
		}

		SetHighColor(120, 125, 135);
		SetFontSize(10);
		DrawString("busy = CS active (INSTDONE / ring HEAD!=TAIL)",
			BPoint(b.left + 12, b.bottom - 8));
	}

private:
	float	fHist[kHistory];
	int		fCount;
	float	fCur, fPeak, fAvg;
};


// ---------------------------------------------------------------------------
class MonWindow : public BWindow {
public:
	MonWindow()
		: BWindow(BRect(120, 120, 120 + 440, 120 + 250), "GPU Monitor",
			B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_QUIT_ON_WINDOW_CLOSE),
		  fRun(true)
	{
		fView = new GraphView(Bounds());
		AddChild(fView);
		fSampler = spawn_thread(_Sample, "gpu-sampler", B_NORMAL_PRIORITY, this);
		resume_thread(fSampler);
	}

	virtual bool QuitRequested()
	{
		fRun = false;
		status_t dummy;
		wait_for_thread(fSampler, &dummy);
		return true;
	}

	void Push(float pct)
	{
		if (LockWithTimeout(20000) == B_OK) {
			fView->AddSample(pct);
			fView->Invalidate();
			Unlock();
		}
	}

private:
	static int32 _Sample(void* arg)
	{
		MonWindow* self = (MonWindow*)arg;
		const bigtime_t window = 100000;   // 100 ms display interval
		const bigtime_t gap = 25;          // ~25 us between reads (catch bursts)
		while (self->fRun) {
			bigtime_t start = system_time();
			uint32 busy = 0, total = 0;
			while (system_time() - start < window) {
				if (gpu_busy()) busy++;
				total++;
				snooze(gap);
			}
			float pct = total ? (100.0f * busy / total) : 0.0f;
			self->Push(pct);
		}
		return 0;
	}

	GraphView*	fView;
	thread_id	fSampler;
	volatile bool fRun;
};


class MonApp : public BApplication {
public:
	MonApp() : BApplication("application/x-vnd.gpu-monitor") {}
	virtual void ReadyToRun() { (new MonWindow())->Show(); }
};


int
main(int argc, char** argv)
{
	status_t st = open_gpu();
	if (st != B_OK) {
		fprintf(stderr, "gpu_monitor: cannot open GPU: %s\n", strerror(st));
		return 1;
	}
	printf("gpu_monitor: Intel Ironlake (Gen5), ring base 0x%05x\n",
		(unsigned)sRingBase);
	fflush(stdout);

	// Headless self-test: print the busy % once per 100 ms, no GUI. Useful
	// for verifying the register sampling (run a GPU test alongside it).
	if (argc > 1 && strcmp(argv[1], "test") == 0) {
		for (int i = 0; i < 20; i++) {
			bigtime_t start = system_time();
			uint32 busy = 0, total = 0;
			while (system_time() - start < 100000) {
				if (gpu_busy()) busy++;
				total++;
				snooze(25);
			}
			printf("busy %5.1f%%   (head=0x%05x tail=0x%05x acthd=0x%08x)\n",
				total ? 100.0f * busy / total : 0.0f,
				(unsigned)(rd(sRingBase + kHeadOff) & kRingMask),
				(unsigned)(rd(sRingBase + kTailOff) & kRingMask),
				(unsigned)rd(kActhd));
			fflush(stdout);
		}
		close_gpu();
		return 0;
	}

	MonApp app;
	app.Run();

	close_gpu();
	return 0;
}

/*
 * gpu_monitor — live GPU-utilisation graph for the Ironlake (Gen5) GPU,
 * styled after the CPU-core graphs in ActivityMonitor / Pulse, and usable
 * as a Desktop replicant (drag the view onto the desktop with "Show
 * Replicants" enabled).
 *
 * "Busy" is sampled from the render command streamer, read out of the cloned
 * MMIO register area (reads are allowed from userspace). The GPU counts as
 * busy at an instant when the ring has queued commands (HEAD != TAIL) or its
 * active head advanced since the last read (ACTHD moved). INSTDONE is
 * unusable as an idle signal on this part (bit 0 reads cleared even at idle).
 *
 * Each view instance opens its own register clones, so the replicant works
 * inside Tracker's process independently of the launcher.
 *
 * Build (from the accelerant dir so intel_extreme.h resolves):
 *   g++ -O2 -o gpu_monitor tools/gpu_monitor.cpp \
 *       -I/boot/system/develop/headers/private/graphics/intel_extreme \
 *       -I/boot/system/develop/headers/private/graphics \
 *       -I/boot/system/develop/headers/private/graphics/common \
 *       -I/boot/system/develop/headers/private/shared -lbe
 * Run: ./gpu_monitor        (window; drag the graph onto the Desktop)
 *      ./gpu_monitor test    (headless numbers)
 */
#include <Application.h>
#include <Window.h>
#include <View.h>
#include <Dragger.h>
#include <String.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <OS.h>

#include "intel_extreme.h"

static const char* kSignature = "application/x-vnd.GPUMonitor";

static const uint32 kActhd    = 0x2074;		// DWORD address being executed
static const uint32 kHeadOff  = 0x0004;
static const uint32 kTailOff  = 0x0000;
static const uint32 kRingMask = 0x001ffffc;
static const int    kHistory  = 160;


// ---------------------------------------------------------------------------
// GpuTap — a self-contained handle on the GPU's command-streamer registers.
// ---------------------------------------------------------------------------
class GpuTap {
public:
	GpuTap() : fFd(-1), fShared(-1), fRegs(-1), fRegBase(NULL),
		fRingBase(0), fLastActhd(0) {}
	~GpuTap() { Close(); }

	status_t Open()
	{
		fFd = open("/dev/graphics/intel_extreme_000200", B_READ_WRITE);
		if (fFd < 0)
			return errno;

		intel_get_private_data data;
		data.magic = INTEL_PRIVATE_DATA_MAGIC;
		if (ioctl(fFd, INTEL_GET_PRIVATE_DATA, &data, sizeof(data)) != 0) {
			close(fFd); fFd = -1;
			return errno;
		}

		intel_shared_info* info = NULL;
		fShared = clone_area("gpumon shared", (void**)&info, B_ANY_ADDRESS,
			B_READ_AREA | B_WRITE_AREA, data.shared_info_area);
		if (fShared < B_OK) { close(fFd); fFd = -1; return fShared; }
		fRingBase = info->primary_ring_buffer.register_base;

		void* regs = NULL;
		fRegs = clone_area("gpumon regs", &regs, B_ANY_ADDRESS,
			B_READ_AREA | B_WRITE_AREA, info->registers_area);
		if (fRegs < B_OK) {
			delete_area(fShared); fShared = -1;
			close(fFd); fFd = -1;
			return fRegs;
		}
		fRegBase = (volatile uint8*)regs;
		return B_OK;
	}

	void Close()
	{
		if (fRegs >= B_OK) { delete_area(fRegs); fRegs = -1; }
		if (fShared >= B_OK) { delete_area(fShared); fShared = -1; }
		if (fFd >= 0) { close(fFd); fFd = -1; }
		fRegBase = NULL;
	}

	bool Valid() const { return fRegBase != NULL; }

	uint32 Read(uint32 off) const
	{
		return *(volatile uint32*)(fRegBase + off);
	}

	bool Busy()
	{
		uint32 head = Read(fRingBase + kHeadOff) & kRingMask;
		uint32 tail = Read(fRingBase + kTailOff) & kRingMask;
		uint32 acthd = Read(kActhd);
		bool moved = (acthd != fLastActhd);
		fLastActhd = acthd;
		return (head != tail) || moved;
	}

	// Sample the busy fraction over `window_us`, `gap_us` between reads.
	float Sample(bigtime_t window_us, bigtime_t gap_us)
	{
		bigtime_t start = system_time();
		uint32 busy = 0, total = 0;
		while (system_time() - start < window_us) {
			if (Busy()) busy++;
			total++;
			snooze(gap_us);
		}
		return total ? 100.0f * busy / total : 0.0f;
	}

private:
	int			fFd;
	area_id		fShared;
	area_id		fRegs;
	volatile uint8*	fRegBase;
	uint32		fRingBase;
	uint32		fLastActhd;
};


// ---------------------------------------------------------------------------
// GpuView — the replicable utilisation graph.
// ---------------------------------------------------------------------------
class GpuView : public BView {
public:
	GpuView(BRect frame);
	GpuView(BMessage* archive);
	virtual ~GpuView();

	static BArchivable* Instantiate(BMessage* archive);
	virtual status_t Archive(BMessage* archive, bool deep = true) const;

	virtual void AttachedToWindow();
	virtual void DetachedFromWindow();
	virtual void Draw(BRect updateRect);

	void AddSample(float pct);

private:
	void _Init();
	void _AddDragger();
	static int32 _Sampler(void* arg);
	static void _LevelColor(float pct, rgb_color* c);

	GpuTap		fTap;
	thread_id	fThread;
	volatile bool fRun;
	float		fHist[kHistory];
	int			fCount;
	float		fCur, fAvg, fPeak;
	bool		fOpened;
};


GpuView::GpuView(BRect frame)
	: BView(frame, "gpu_monitor", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS),
	  fThread(-1), fRun(false), fCount(0), fCur(0), fAvg(0), fPeak(0),
	  fOpened(false)
{
	_Init();
	_AddDragger();
}


GpuView::GpuView(BMessage* archive)
	: BView(archive),
	  fThread(-1), fRun(false), fCount(0), fCur(0), fAvg(0), fPeak(0),
	  fOpened(false)
{
	_Init();
	// The BDragger child is restored by BView(archive); do not re-add it.
}


GpuView::~GpuView()
{
}


void
GpuView::_Init()
{
	SetViewColor(16, 18, 22);
	for (int i = 0; i < kHistory; i++)
		fHist[i] = 0.0f;
}


void
GpuView::_AddDragger()
{
	BRect r = Bounds();
	r.left = r.right - 8;
	r.top = r.bottom - 8;
	BDragger* dragger = new BDragger(r, this,
		B_FOLLOW_RIGHT | B_FOLLOW_BOTTOM);
	AddChild(dragger);
}


BArchivable*
GpuView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "GpuView"))
		return NULL;
	return new GpuView(archive);
}


status_t
GpuView::Archive(BMessage* archive, bool deep) const
{
	status_t st = BView::Archive(archive, deep);
	if (st != B_OK)
		return st;
	archive->AddString("add_on", kSignature);
	archive->AddString("class", "GpuView");
	return B_OK;
}


void
GpuView::AttachedToWindow()
{
	BView::AttachedToWindow();
	if (Parent() != NULL)
		SetViewColor(B_TRANSPARENT_COLOR);
	fOpened = (fTap.Open() == B_OK);
	if (fOpened) {
		fRun = true;
		fThread = spawn_thread(_Sampler, "gpu-sampler",
			B_LOW_PRIORITY, this);
		resume_thread(fThread);
	}
}


void
GpuView::DetachedFromWindow()
{
	if (fThread >= 0) {
		fRun = false;
		status_t dummy;
		wait_for_thread(fThread, &dummy);
		fThread = -1;
	}
	fTap.Close();
	fOpened = false;
	BView::DetachedFromWindow();
}


int32
GpuView::_Sampler(void* arg)
{
	GpuView* self = (GpuView*)arg;
	// Gentle for an always-on desktop widget: ~5 kHz reads, 250 ms window.
	while (self->fRun) {
		float pct = self->fTap.Sample(250000, 200);
		if (self->LockLooperWithTimeout(30000) == B_OK) {
			self->AddSample(pct);
			self->Invalidate();
			self->UnlockLooper();
		}
	}
	return 0;
}


void
GpuView::AddSample(float pct)
{
	fCur = pct;
	for (int i = 0; i < kHistory - 1; i++)
		fHist[i] = fHist[i + 1];
	fHist[kHistory - 1] = pct;
	if (fCount < kHistory) fCount++;
	float sum = 0; fPeak = 0;
	for (int i = kHistory - fCount; i < kHistory; i++) {
		sum += fHist[i];
		if (fHist[i] > fPeak) fPeak = fHist[i];
	}
	fAvg = fCount ? sum / fCount : 0;
}


void
GpuView::_LevelColor(float pct, rgb_color* c)
{
	if (pct < 50.0f)      { c->red = 60;  c->green = 200; c->blue = 90; }
	else if (pct < 80.0f) { c->red = 220; c->green = 190; c->blue = 40; }
	else                  { c->red = 230; c->green = 70;  c->blue = 60; }
	c->alpha = 255;
}


void
GpuView::Draw(BRect)
{
	BRect b = Bounds();

	// backdrop (replicants are transparent-view-color, so paint our own)
	SetHighColor(16, 18, 22);
	FillRect(b);
	SetHighColor(44, 48, 58);
	StrokeRect(b);

	if (!fOpened) {
		SetHighColor(200, 90, 80);
		SetFontSize(11);
		DrawString("GPU: no access", BPoint(b.left + 8, b.top + 20));
		return;
	}

	float top = b.top + 28;
	BRect graph(b.left + 4, top, b.right - 4, b.bottom - 4);

	// big percentage + label
	BString big; big.SetToFormat("%d%%", (int)(fCur + 0.5f));
	SetFontSize(18);
	rgb_color cc; _LevelColor(fCur, &cc);
	SetHighColor(cc);
	DrawString(big.String(), BPoint(b.left + 6, b.top + 20));

	SetFontSize(9);
	SetHighColor(150, 155, 165);
	BString sub; sub.SetToFormat("GPU Ironlake  avg %d  peak %d",
		(int)(fAvg + 0.5f), (int)(fPeak + 0.5f));
	DrawString(sub.String(), BPoint(b.left + 52, b.top + 14));

	// gridlines
	for (int g = 1; g < 4; g++) {
		float y = graph.top + graph.Height() * g / 4.0f;
		SetHighColor(28, 31, 38);
		StrokeLine(BPoint(graph.left, y), BPoint(graph.right, y));
	}

	// filled bars, newest at the right
	float gh = graph.Height();
	float step = graph.Width() / (float)kHistory;
	float bx = graph.right;
	for (int i = kHistory - 1; i >= 0; i--) {
		float pct = fHist[i];
		float h = gh * pct / 100.0f;
		rgb_color c; _LevelColor(pct, &c);
		c.red = (uint8)(c.red * 0.85f);
		c.green = (uint8)(c.green * 0.85f);
		c.blue = (uint8)(c.blue * 0.85f);
		SetHighColor(c);
		FillRect(BRect(bx - step, graph.bottom - h, bx, graph.bottom));
		bx -= step;
		if (bx < graph.left) break;
	}
}


// ---------------------------------------------------------------------------
// Standalone host window (also the drag source for the replicant).
// ---------------------------------------------------------------------------
class MonWindow : public BWindow {
public:
	MonWindow()
		: BWindow(BRect(120, 120, 120 + 210, 120 + 110), "GPU Monitor",
			B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_QUIT_ON_WINDOW_CLOSE)
	{
		AddChild(new GpuView(Bounds()));
	}
};


class MonApp : public BApplication {
public:
	MonApp() : BApplication(kSignature) {}
	virtual void ReadyToRun() { (new MonWindow())->Show(); }
};


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "test") == 0) {
		GpuTap tap;
		status_t st = tap.Open();
		if (st != B_OK) {
			fprintf(stderr, "gpu_monitor: cannot open GPU: %s\n",
				strerror(st));
			return 1;
		}
		printf("gpu_monitor: Intel Ironlake (Gen5) — sampling...\n");
		for (int i = 0; i < 20; i++) {
			printf("busy %5.1f%%\n", tap.Sample(100000, 25));
			fflush(stdout);
		}
		return 0;
	}

	MonApp app;
	app.Run();
	return 0;
}

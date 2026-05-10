/*
 * CrocusRenderer — Haiku BGLRenderer add-on using Mesa crocus (Intel Gen5).
 *
 * Links dynamically against libgallium-25.3.3.so which contains crocus
 * driver + Mesa GL stack + HGL frontend. Uses forward declarations to
 * avoid Mesa internal header conflicts.
 *
 * Install to: /boot/system/non-packaged/add-ons/opengl/Crocus Pipe
 */

#include <stdio.h>
#include <dlfcn.h>
#include <GLView.h>
#include <GLRenderer.h>
#include <Screen.h>
#include <Bitmap.h>

/* Minimal Mesa type definitions — avoid full header deps */
struct pipe_screen;

/* driOptionCache from Mesa xmlconfig.h */
typedef struct driOptionValue { unsigned _pad[4]; } driOptionValue;
typedef struct driOptionCache {
	void* info;
	driOptionValue* values;
	unsigned tableSize;
} driOptionCache;

struct pipe_screen_config {
	bool driver_name_is_inferred;
	driOptionCache *options;
	const driOptionCache *options_info;
};

/* From xmlconfig.h */
extern "C" {
void driParseOptionInfo(driOptionCache *info,
	const void *configOptions, unsigned numOptions);
void driParseConfigFiles(driOptionCache *cache, const driOptionCache *info,
	int screen, const char *driver, const char *kernelDriver,
	const char *deviceName, const char *applicationName, unsigned applicationVersion,
	const char *engineName, unsigned engineVersion);
}

struct pipe_frontend_screen;
struct pipe_frontend_drawable;
struct pp_queue_t;

/* pipe_format enum — only need NONE and the formats used by hgl_get_st_visual */
enum pipe_format { PIPE_FORMAT_NONE = 0 };

struct st_visual {
	unsigned buffer_mask;
	enum pipe_format color_format;
	enum pipe_format depth_stencil_format;
	enum pipe_format accum_format;
	unsigned samples;
};

struct st_context;

/* PP_FILTERS count from Mesa — crocus doesn't use post-processing */
#define PP_FILTERS 7

struct hgl_context {
	struct hgl_display* display;
	struct st_context* st;
	struct pp_queue_t* postProcess;
	unsigned int postProcessEnable[PP_FILTERS];
};

struct hgl_display;
struct hgl_buffer;

/* Types matching Mesa's hgl_context.h */
extern "C" {

/* From hgl.c */
struct hgl_display* hgl_create_display(struct pipe_screen* screen);
void hgl_destroy_display(struct hgl_display* display);
struct hgl_context* hgl_create_context(struct hgl_display* display,
	struct st_visual* visual, struct st_context* shared);
void hgl_destroy_context(struct hgl_context* context);
struct hgl_buffer* hgl_create_st_framebuffer(struct hgl_display* display,
	struct st_visual* visual, void* winsysContext);
void hgl_destroy_st_framebuffer(struct hgl_buffer* buffer);
void hgl_get_st_visual(struct st_visual* visual, unsigned long options);

/* From crocus_drm_winsys.c */
struct pipe_screen* crocus_drm_screen_create(int fd,
	const struct pipe_screen_config* config);

/* From drm_helper.h — provides crocus driconf options */
struct drm_driver_descriptor {
	const char* driver_name;
	const void* driconf;       /* driOptionDescription* */
	unsigned    driconf_count;
	void*       create_screen;
};
extern const struct drm_driver_descriptor crocus_driver_descriptor;

/* From state_tracker/st_context.h */
struct st_context* st_api_create_context(struct pipe_frontend_screen* fscreen,
	void* attribs, int* error, struct st_context* shared);
int st_api_make_current(struct st_context* st,
	struct pipe_frontend_drawable* draw,
	struct pipe_frontend_drawable* read);
void st_context_flush(struct st_context* st, unsigned flags,
	void* fence, void* notify_before, void* notify_after);

/* From DRM shim */
int haiku_drm_open(void);
void haiku_drm_close(int fd);

}


class CrocusRenderer : public BGLRenderer {
public:
					CrocusRenderer(BGLView *view, ulong options);
	virtual			~CrocusRenderer();

	virtual void	SwapBuffers(bool VSync = false);
	virtual void	Draw(BRect updateRect);
	virtual void	FrameResized(float width, float height);

private:
	int				fDrmFd;
	struct pipe_screen*	fScreen;
	struct hgl_display*	fDisplay;
	struct hgl_context*	fContext;
	struct hgl_buffer*	fBuffer;
};


CrocusRenderer::CrocusRenderer(BGLView *view, ulong options)
	: BGLRenderer(view, options),
	  fDrmFd(-1), fScreen(NULL), fDisplay(NULL),
	  fContext(NULL), fBuffer(NULL)
{
	printf("CrocusRenderer: init Intel Gen5 GPU\n");

	fDrmFd = haiku_drm_open();
	if (fDrmFd < 0) {
		printf("CrocusRenderer: DRM open failed\n");
		return;
	}

	/* Initialize driconf options from crocus driver descriptor */
	driOptionCache optionsInfo;
	memset(&optionsInfo, 0, sizeof(optionsInfo));
	driOptionCache optionsCache;
	memset(&optionsCache, 0, sizeof(optionsCache));
	driParseOptionInfo(&optionsInfo,
		(const void*)crocus_driver_descriptor.driconf,
		crocus_driver_descriptor.driconf_count);
	driParseConfigFiles(&optionsCache, &optionsInfo, 0, "crocus",
		NULL, NULL, NULL, 0, NULL, 0);

	struct pipe_screen_config config;
	memset(&config, 0, sizeof(config));
	config.options = &optionsCache;
	config.options_info = &optionsInfo;
	fScreen = crocus_drm_screen_create(fDrmFd, &config);
	if (!fScreen) {
		printf("CrocusRenderer: crocus screen failed\n");
		return;
	}
	printf("CrocusRenderer: crocus screen OK\n");

	fDisplay = hgl_create_display(fScreen);
	if (!fDisplay) {
		printf("CrocusRenderer: display failed\n");
		return;
	}

	/* st_visual is 48 bytes — zero-init and let hgl_get_st_visual fill it */
	struct st_visual visual;
	memset(&visual, 0, sizeof(visual));
	hgl_get_st_visual(&visual, options);

	fContext = hgl_create_context(fDisplay, &visual, NULL);
	if (!fContext) {
		printf("CrocusRenderer: GL context failed\n");
		return;
	}

	fBuffer = hgl_create_st_framebuffer(fDisplay, &visual, NULL);

	printf("CrocusRenderer: ready\n");
}


CrocusRenderer::~CrocusRenderer()
{
	if (fContext)
		hgl_destroy_context(fContext);
	if (fBuffer)
		hgl_destroy_st_framebuffer(fBuffer);
	if (fDisplay)
		hgl_destroy_display(fDisplay);
	/* fScreen destroyed by display */
	if (fDrmFd >= 0)
		haiku_drm_close(fDrmFd);
}


void
CrocusRenderer::SwapBuffers(bool VSync)
{
	if (!fContext)
		return;
	if (VSync && GLView()->Window()) {
		BScreen screen(GLView()->Window());
		screen.WaitForRetrace();
	}
	st_context_flush(fContext->st, 0, NULL, NULL, NULL);
}


void
CrocusRenderer::Draw(BRect updateRect)
{
	(void)updateRect;
}


void
CrocusRenderer::FrameResized(float width, float height)
{
	(void)width; (void)height;
}


extern "C" _EXPORT BGLRenderer*
instantiate_gl_renderer(BGLView *view, ulong options)
{
	return new CrocusRenderer(view, options);
}

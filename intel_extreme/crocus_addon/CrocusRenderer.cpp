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

struct pipe_frontend_drawable {
	int32_t stamp;
	uint32_t ID;
	struct pipe_frontend_screen *fscreen;
	const struct st_visual *visual;
	/* validate callback — filled by hgl_create_st_framebuffer */
	void (*validate)(struct st_context*, struct pipe_frontend_drawable*,
		const void*, unsigned, void**);
	bool (*flush_front)(struct st_context*, struct pipe_frontend_drawable*,
		unsigned);
	bool (*get_param)(struct pipe_frontend_drawable*, unsigned);
};
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

/* Enough of pipe_resource/pipe_texture_target for hgl_buffer layout */
enum pipe_texture_target { PIPE_TEXTURE_2D = 2 };
struct pipe_resource;
#define ST_ATTACHMENT_COUNT 5

struct hgl_buffer {
	struct pipe_frontend_drawable base;
	struct st_visual visual;
	unsigned width;
	unsigned height;
	unsigned newWidth;
	unsigned newHeight;
	unsigned mask;
	struct pipe_screen* screen;
	void* winsysContext;
	enum pipe_texture_target target;
	struct pipe_resource* textures[ST_ATTACHMENT_COUNT];
};

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
bool st_api_make_current(struct st_context* st,
	struct pipe_frontend_drawable* draw,
	struct pipe_frontend_drawable* read);
void st_context_flush(struct st_context* st, unsigned flags,
	void* fence, void* notify_before, void* notify_after);

/* Internal Mesa 25 dispatch (compiled into this addon) */
void* _mesa_glapi_get_dispatch(void);
void* _mesa_glapi_get_context(void);

/* From DRM shim */
int haiku_drm_open(void);
void haiku_drm_close(int fd);

}


class CrocusRenderer : public BGLRenderer {
public:
					CrocusRenderer(BGLView *view, ulong options);
	virtual			~CrocusRenderer();

	virtual void	LockGL();
	virtual void	UnlockGL();
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


/* Resolve libglapi.so dispatch bridge at first use.
 * System libGL.so (Mesa 22) reads _glapi_tls_Dispatch from libglapi.so.
 * Our addon (Mesa 25) uses _mesa_glapi_tls_Dispatch internally.
 * We must sync the two after every st_api_make_current. */
typedef void (*glapi_set_dispatch_fn)(void*);
typedef void (*glapi_set_context_fn)(void*);
static glapi_set_dispatch_fn sGlapiSetDispatch = NULL;
static glapi_set_context_fn sGlapiSetContext = NULL;
static bool sGlapiBridgeInit = false;

static void
init_glapi_bridge()
{
	if (sGlapiBridgeInit) return;
	sGlapiBridgeInit = true;

	/* Find the ALREADY LOADED libglapi.so (loaded as libGL.so dependency).
	 * Do NOT use load_add_on which loads a separate copy with different TLS. */
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (strstr(info.name, "libglapi") != NULL) {
			printf("CrocusRenderer: found loaded %s (id=%d)\n",
				info.name, (int)info.id);
			get_image_symbol(info.id, "_glapi_set_dispatch",
				B_SYMBOL_TYPE_TEXT, (void**)&sGlapiSetDispatch);
			get_image_symbol(info.id, "_glapi_set_context",
				B_SYMBOL_TYPE_TEXT, (void**)&sGlapiSetContext);
			break;
		}
	}
	printf("CrocusRenderer: glapi bridge: set_dispatch=%p set_context=%p\n",
		sGlapiSetDispatch, sGlapiSetContext);
}

void
CrocusRenderer::LockGL()
{
	BGLRenderer::LockGL();
	if (fContext && fContext->st && fBuffer) {
		st_api_make_current(fContext->st,
			&fBuffer->base, &fBuffer->base);

		/* Sync Mesa 25 dispatch → libglapi.so (Mesa 22) dispatch */
		init_glapi_bridge();
		void* dispatch = _mesa_glapi_get_dispatch();
		void* ctx = _mesa_glapi_get_context();
		if (sGlapiSetDispatch && dispatch) {
			sGlapiSetDispatch(dispatch);
		}
		if (sGlapiSetContext && ctx) {
			sGlapiSetContext(ctx);
		}
		printf("CrocusRenderer::LockGL: dispatch=%p ctx=%p\n",
			dispatch, ctx);
	}
}


void
CrocusRenderer::UnlockGL()
{
	if (fContext && fContext->st) {
		st_api_make_current(NULL, NULL, NULL);
		/* Clear system dispatch too */
		if (sGlapiSetDispatch)
			sGlapiSetDispatch(NULL);
		if (sGlapiSetContext)
			sGlapiSetContext(NULL);
	}
	BGLRenderer::UnlockGL();
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

/*
 * compat_shim.cpp — ABI shim: C++ mangled st_context_flush → C version.
 */

struct st_context;
struct pipe_fence_handle;

/* C version from libmesa.a */
extern "C" void st_context_flush(struct st_context* st, unsigned flags,
	struct pipe_fence_handle** fence, void* notify_before, void* notify_after);

/* C++ version matching GalliumContext.cpp declaration */
void st_context_flush(struct st_context* st, unsigned flags,
	struct pipe_fence_handle** fence,
	void (*before_flush_cb)(void*), void* args)
{
	st_context_flush(st, flags, fence, (void*)before_flush_cb, args);
}

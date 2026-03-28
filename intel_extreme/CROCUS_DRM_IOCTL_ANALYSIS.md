# Crocus Gallium Driver - Complete i915 DRM Ioctl Analysis

Analysis of Mesa's `crocus` driver (Gen4-8) DRM ioctl usage for building
an Intel GPU server on Haiku OS. Source: Mesa main branch, files:
- `src/gallium/drivers/crocus/crocus_bufmgr.c`
- `src/gallium/drivers/crocus/crocus_batch.c`
- `src/gallium/drivers/crocus/crocus_fence.c`
- `src/gallium/drivers/crocus/crocus_screen.c`
- `src/intel/common/i915/intel_gem.c`
- `src/intel/dev/i915/intel_device_info.c`

---

## 1. BUFFER MANAGEMENT (GEM Object Lifecycle)

### 1.1 DRM_IOCTL_I915_GEM_CREATE
- **File**: `crocus_bufmgr.c` - `alloc_fresh_bo()`
- **Struct**: `struct drm_i915_gem_create`
- **Parameters IN**: `.size` (uint64_t, buffer size in bytes)
- **Returns**: `.handle` (uint32_t, GEM handle)
- **Purpose**: Allocates a new GPU buffer object. All new BOs are kernel-zeroed.
- **Gen5 Essential**: YES - fundamental for all buffer allocation

### 1.2 DRM_IOCTL_GEM_CLOSE
- **File**: `crocus_bufmgr.c` - `bo_close()`
- **Struct**: `struct drm_gem_close`
- **Parameters IN**: `.handle` (uint32_t, GEM handle to close)
- **Returns**: 0 on success
- **Purpose**: Releases a GEM buffer object handle. Called when BO refcount
  reaches zero and BO is idle (or deferred via zombie list).
- **Gen5 Essential**: YES - must close every handle to avoid leaks

### 1.3 DRM_IOCTL_I915_GEM_USERPTR
- **File**: `crocus_bufmgr.c` - `crocus_bo_create_userptr()`
- **Struct**: `struct drm_i915_gem_userptr`
- **Parameters IN**: `.user_ptr` (uintptr_t), `.user_size` (uint64_t)
- **Returns**: `.handle` (uint32_t, GEM handle wrapping user memory)
- **Purpose**: Wraps user-allocated CPU memory as a GEM BO for GPU access.
- **Gen5 Essential**: NO - optional optimization, can use regular BOs instead

### 1.4 DRM_IOCTL_I915_GEM_BUSY
- **File**: `crocus_bufmgr.c` - `crocus_bo_busy()`
- **Struct**: `struct drm_i915_gem_busy`
- **Parameters IN**: `.handle` (uint32_t)
- **Returns**: `.busy` (uint32_t, nonzero if GPU is still using the BO)
- **Purpose**: Non-blocking check if GPU is done with a BO. Used by BO cache
  to skip busy BOs and by zombie list cleanup.
- **Gen5 Essential**: YES - needed for efficient BO cache reuse

### 1.5 DRM_IOCTL_I915_GEM_WAIT
- **File**: `crocus_bufmgr.c` - `crocus_bo_wait()`
- **Struct**: `struct drm_i915_gem_wait`
- **Parameters IN**: `.bo_handle` (uint32_t), `.timeout_ns` (int64_t, -1 = infinite)
- **Returns**: 0 on success, -ETIME on timeout
- **Purpose**: Blocking wait until GPU finishes all operations on a BO.
  Used before CPU mapping when MAP_ASYNC is not set.
- **Gen5 Essential**: YES - primary synchronization mechanism

### 1.6 DRM_IOCTL_I915_GEM_MADVISE
- **File**: `crocus_bufmgr.c` - `crocus_bo_madvise()`
- **Struct**: `struct drm_i915_gem_madvise`
- **Parameters IN**: `.handle`, `.madv` (I915_MADV_WILLNEED or I915_MADV_DONTNEED)
- **Returns**: `.retained` (1 if kernel still has the pages, 0 if purged)
- **Purpose**: Tells kernel about BO usage intent. BOs in cache are marked
  DONTNEED (kernel may reclaim pages under memory pressure). When reusing,
  WILLNEED checks if pages survived. If `.retained == 0`, BO was purged.
- **Gen5 Essential**: YES - critical for BO cache system

---

## 2. MEMORY MAPPING

### 2.1 DRM_IOCTL_I915_GEM_MMAP (Legacy)
- **File**: `crocus_bufmgr.c` - `crocus_bo_gem_mmap_legacy()`
- **Struct**: `struct drm_i915_gem_mmap`
- **Parameters IN**: `.handle`, `.size`, `.flags` (I915_MMAP_WC for write-combining)
- **Returns**: `.addr_ptr` (uint64_t, user-space virtual address)
- **Purpose**: Maps a GEM BO into user-space. Two variants:
  - flags=0: CPU-coherent cacheable mapping (for LLC or read-only)
  - flags=I915_MMAP_WC: write-combining mapping (for non-LLC writes)
- **Gen5 Essential**: YES - primary mapping method for Gen5 (no has_mmap_offset)

### 2.2 DRM_IOCTL_I915_GEM_MMAP_GTT
- **File**: `crocus_bufmgr.c` - `crocus_bo_map_gtt()`
- **Struct**: `struct drm_i915_gem_mmap_gtt`
- **Parameters IN**: `.handle`
- **Returns**: `.offset` (uint64_t, fake offset for mmap())
- **Purpose**: Gets a GTT-based mapping offset. Then user calls
  `mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset)`.
  GTT mappings handle tiling/detiling automatically. Used for tiled
  buffers (X-tiled, Y-tiled).
- **Gen5 Essential**: YES - needed for tiled buffer access

### 2.3 DRM_IOCTL_I915_GEM_MMAP_OFFSET (Newer kernel)
- **File**: `crocus_bufmgr.c` - `crocus_bo_gem_mmap_offset()`
- **Struct**: `struct drm_i915_gem_mmap_offset`
- **Parameters IN**: `.handle`, `.flags` (I915_MMAP_OFFSET_WC or _WB)
- **Returns**: `.offset` (uint64_t, fake offset for mmap())
- **Purpose**: Newer replacement for MMAP and MMAP_GTT. Uses mmap()
  with returned offset. Selected when `has_mmap_offset` is true.
- **Gen5 Essential**: NO - fallback exists via legacy MMAP. Only used
  on newer kernels.

---

## 3. DOMAIN AND CACHE CONTROL

### 3.1 DRM_IOCTL_I915_GEM_SET_DOMAIN
- **File**: `crocus_bufmgr.c` - `alloc_fresh_bo()`, `crocus_bo_create_userptr()`
- **Struct**: `struct drm_i915_gem_set_domain`
- **Parameters IN**: `.handle`, `.read_domains` (e.g. I915_GEM_DOMAIN_CPU),
  `.write_domain` (0 for read, or domain for write)
- **Returns**: 0 on success
- **Purpose**: Two uses:
  1. After GEM_CREATE: Forces kernel to allocate physical pages immediately
     (with read_domains=CPU, write_domain=0) instead of lazy allocation.
  2. After USERPTR: Validates the user pointer is accessible.
- **Gen5 Essential**: YES - ensures pages are allocated before first use

### 3.2 DRM_IOCTL_I915_GEM_SET_CACHING
- **File**: `crocus_bufmgr.c` - `bo_alloc_internal()`
- **Struct**: `struct drm_i915_gem_caching`
- **Parameters IN**: `.handle`, `.caching` (1 = cached/snooped)
- **Returns**: 0 on success
- **Purpose**: Sets BO caching mode to snooped (LLC-like coherent behavior)
  on non-LLC platforms. Used when BO_ALLOC_COHERENT is requested.
  On Ironlake (non-LLC), this makes CPU reads/writes cache-coherent with GPU.
- **Gen5 Essential**: RECOMMENDED - Ironlake has no LLC, so coherent BOs
  need this. Without it, explicit cache flushing is required.

---

## 4. TILING

### 4.1 DRM_IOCTL_I915_GEM_SET_TILING
- **File**: `crocus_bufmgr.c` - `bo_set_tiling_internal()`
- **Struct**: `struct drm_i915_gem_set_tiling`
- **Parameters IN**: `.handle`, `.tiling_mode` (NONE/X/Y), `.stride`
- **Returns**: `.tiling_mode` (actual), `.swizzle_mode`, `.stride` (actual)
- **Purpose**: Configures tiling layout for a BO. Kernel may modify the
  requested tiling. The returned swizzle_mode indicates address bit swizzling.
  Required for correct GTT mappings of tiled surfaces.
- **Gen5 Essential**: YES - X-tiling is essential for display scanout
  and render performance on Gen5

### 4.2 DRM_IOCTL_I915_GEM_GET_TILING
- **File**: `crocus_bufmgr.c` - `crocus_bo_gem_create_from_name()`,
  `crocus_bo_import_dmabuf()`
- **Struct**: `struct drm_i915_gem_get_tiling`
- **Parameters IN**: `.handle`
- **Returns**: `.tiling_mode`, `.swizzle_mode`
- **Purpose**: Queries tiling configuration of an imported/named BO.
  Needed when receiving BOs from other processes (X11, Wayland compositor).
- **Gen5 Essential**: YES (if sharing buffers between processes)

---

## 5. BATCH EXECUTION

### 5.1 DRM_IOCTL_I915_GEM_EXECBUFFER2
- **File**: `crocus_batch.c` - `submit_batch()`
- **Struct**: `struct drm_i915_gem_execbuffer2`
- **Parameters IN**:
  - `.buffers_ptr` -> array of `struct drm_i915_gem_exec_object2`
    (validation list with handle, offset, flags, relocation_count, relocs_ptr)
  - `.buffer_count` - number of BOs in validation list
  - `.batch_start_offset` - offset into first BO where batch begins (always 0)
  - `.batch_len` - size of batch commands (QWord aligned)
  - `.flags`:
    - `I915_EXEC_RENDER` - submit to render engine
    - `I915_EXEC_NO_RELOC` - addresses are already correct
    - `I915_EXEC_BATCH_FIRST` - batch BO is first in validation list
    - `I915_EXEC_HANDLE_LUT` - handles in relocs are indices, not GEM handles
    - `I915_EXEC_FENCE_ARRAY` - use syncobj fences (optional)
  - `.rsvd1` = hw context ID
  - `.num_cliprects` = number of fences (when FENCE_ARRAY flag set)
  - `.cliprects_ptr` -> array of `struct drm_i915_gem_exec_fence`
- **Returns**: Updated `.offset` fields in exec_object2 entries (new GTT offsets)
- **Sub-structures**:
  - `struct drm_i915_gem_exec_object2`: `.handle`, `.offset`, `.flags`
    (EXEC_OBJECT_WRITE, EXEC_OBJECT_NEEDS_GTT for Gen6),
    `.relocation_count`, `.relocs_ptr`
  - `struct drm_i915_gem_relocation_entry`: `.offset`, `.delta`,
    `.target_handle` (index into validation list), `.presumed_offset`
  - `struct drm_i915_gem_exec_fence`: `.handle` (syncobj), `.flags`
    (I915_EXEC_FENCE_WAIT / I915_EXEC_FENCE_SIGNAL)
- **Purpose**: THE critical ioctl. Submits a GPU command batch for execution.
  Kernel validates all BOs are resident, applies relocations, and
  submits to the GPU ring. After return, BO offsets may have changed.
- **Gen5 Essential**: YES - absolutely fundamental, no GPU work without this

---

## 6. CONTEXT MANAGEMENT

### 6.1 DRM_IOCTL_I915_GEM_CONTEXT_CREATE
- **File**: `i915/intel_gem.c` via `intel_gem_create_context()`
- **Struct**: `struct drm_i915_gem_context_create`
- **Parameters IN**: (none meaningful)
- **Returns**: `.ctx_id` (uint32_t, context handle)
- **Purpose**: Creates a hardware GPU context. Each context has its own
  GPU state (registers, page tables on newer gens). Context ID is passed
  in execbuffer2's `.rsvd1` field.
- **Gen5 Essential**: YES - required for execbuffer2

### 6.2 DRM_IOCTL_I915_GEM_CONTEXT_DESTROY
- **File**: `i915/intel_gem.c` via `intel_gem_destroy_context()`
- **Struct**: `struct drm_i915_gem_context_destroy`
- **Parameters IN**: `.ctx_id`
- **Purpose**: Destroys a hardware GPU context.
- **Gen5 Essential**: YES - must clean up contexts

### 6.3 DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM
- **File**: `i915/intel_gem.c` via `intel_gem_set_context_param()`
- **Struct**: `struct drm_i915_gem_context_param`
- **Parameters IN**: `.ctx_id`, `.param`, `.value`
- **Params used by crocus**:
  - `I915_CONTEXT_PARAM_RECOVERABLE` = false (don't auto-recover after hang)
  - `I915_CONTEXT_PARAM_PRIORITY` = batch priority level
- **Purpose**: Configures context parameters.
- **Gen5 Essential**: OPTIONAL - works without these but recommended

### 6.4 DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM
- **File**: `i915/intel_gem.c` via `intel_gem_get_context_param()`
- **Struct**: `struct drm_i915_gem_context_param`
- **Parameters IN**: `.ctx_id`, `.param`
- **Returns**: `.value`
- **Purpose**: Queries context parameters (priority for context cloning).
- **Gen5 Essential**: OPTIONAL

---

## 7. QUERIES AND INFORMATION

### 7.1 DRM_IOCTL_I915_GETPARAM
- **File**: `i915/intel_device_info.c` via `getparam()`, and `i915/intel_gem.c`
- **Struct**: `struct drm_i915_getparam`
- **Parameters IN**: `.param` (I915_PARAM_*)
- **Returns**: `*value` (int)
- **Params queried during device init**:
  - `I915_PARAM_CHIPSET_ID` - PCI device ID
  - `I915_PARAM_REVISION` - GPU stepping/revision
  - `I915_PARAM_CS_TIMESTAMP_FREQUENCY` - GPU timestamp clock
  - `I915_PARAM_SLICE_MASK` - slice topology
  - `I915_PARAM_SUBSLICE_MASK` - subslice topology
  - `I915_PARAM_EU_TOTAL` - total execution units
  - `I915_PARAM_MMAP_GTT_VERSION` - determines has_mmap_offset
  - `I915_PARAM_HAS_USERPTR_PROBE` - userptr support
  - `I915_PARAM_HAS_CONTEXT_ISOLATION` - per-context isolation
- **Purpose**: Query kernel driver capabilities and hardware info.
- **Gen5 Essential**: YES - needed to identify the GPU and its capabilities

### 7.2 DRM_IOCTL_I915_GEM_GET_APERTURE
- **File**: `crocus_screen.c` - `get_aperture_size()`
- **Struct**: `struct drm_i915_gem_get_aperture`
- **Parameters IN**: (none)
- **Returns**: `.aper_size` (uint64_t, GTT aperture size in bytes)
- **Purpose**: Queries the size of the GPU's GTT aperture. Used for
  memory budgeting.
- **Gen5 Essential**: RECOMMENDED - helps with memory management

### 7.3 DRM_IOCTL_I915_GET_RESET_STATS
- **File**: `crocus_batch.c` - `crocus_batch_check_for_reset()`
- **Struct**: `struct drm_i915_reset_stats`
- **Parameters IN**: `.ctx_id`
- **Returns**: `.batch_active` (hangs while this ctx was running),
  `.batch_pending` (hangs while this ctx was pending)
- **Purpose**: Checks if a GPU hang/reset occurred involving this context.
  Determines if context is guilty or innocent. If guilty, context is
  replaced with a fresh one.
- **Gen5 Essential**: RECOMMENDED - robustness, not strictly required

### 7.4 DRM_IOCTL_I915_REG_READ
- **File**: `i915/intel_gem.c`
- **Struct**: `struct drm_i915_reg_read`
- **Parameters IN**: `.offset` (register offset)
- **Returns**: `.val` (register value)
- **Purpose**: Reads a whitelisted GPU register from userspace.
- **Gen5 Essential**: NO - used for timestamps/perf, not core rendering

---

## 8. BUFFER SHARING (Import/Export)

### 8.1 DRM_IOCTL_PRIME_HANDLE_TO_FD
- **File**: `crocus_bufmgr.c` - `crocus_bo_export_dmabuf()`
- **Struct**: `struct drm_prime_handle`
- **Parameters IN**: `.handle` (GEM handle), `.flags` (DRM_CLOEXEC|DRM_RDWR)
- **Returns**: `.fd` (dmabuf file descriptor)
- **Purpose**: Exports a GEM BO as a DMA-BUF fd for sharing between
  processes or devices.
- **Gen5 Essential**: ONLY for multi-process buffer sharing (X11/Wayland)

### 8.2 DRM_IOCTL_PRIME_FD_TO_HANDLE
- **File**: `crocus_bufmgr.c` - `crocus_bo_prime_fd_to_handle()`
- **Struct**: `struct drm_prime_handle`
- **Parameters IN**: `.fd` (dmabuf fd)
- **Returns**: `.handle` (GEM handle)
- **Purpose**: Imports a DMA-BUF fd as a GEM handle.
- **Gen5 Essential**: ONLY for multi-process buffer sharing

### 8.3 DRM_IOCTL_GEM_OPEN
- **File**: `crocus_bufmgr.c` - `crocus_bo_gem_create_from_name()`
- **Struct**: `struct drm_gem_open`
- **Parameters IN**: `.name` (global GEM name, from flink)
- **Returns**: `.handle` (GEM handle), `.size`
- **Purpose**: Opens a globally-named GEM BO (legacy sharing mechanism,
  predates DMA-BUF).
- **Gen5 Essential**: ONLY for legacy X11 DRI2 buffer sharing

### 8.4 DRM_IOCTL_GEM_FLINK
- **File**: `crocus_bufmgr.c` - `crocus_bo_flink()`
- **Struct**: `struct drm_gem_flink`
- **Parameters IN**: `.handle`
- **Returns**: `.name` (global name, uint32_t)
- **Purpose**: Creates a global name for a GEM BO (legacy sharing).
- **Gen5 Essential**: ONLY for legacy X11 DRI2

---

## 9. SYNCHRONIZATION (DRM Syncobj)

### 9.1 DRM_IOCTL_SYNCOBJ_CREATE
- **File**: `crocus_fence.c` - `gem_syncobj_create()`
- **Struct**: `struct drm_syncobj_create`
- **Parameters IN**: `.flags` (0 or DRM_SYNCOBJ_CREATE_SIGNALED)
- **Returns**: `.handle` (syncobj handle)
- **Purpose**: Creates a DRM synchronization object. Used with
  I915_EXEC_FENCE_ARRAY in execbuffer2 for inter-batch ordering.
  Each batch gets a signal syncobj and may wait on others.
- **Gen5 Essential**: YES - needed for batch synchronization in crocus

### 9.2 DRM_IOCTL_SYNCOBJ_DESTROY
- **File**: `crocus_fence.c` - `gem_syncobj_destroy()`
- **Struct**: `struct drm_syncobj_destroy`
- **Parameters IN**: `.handle`
- **Purpose**: Destroys a syncobj.
- **Gen5 Essential**: YES - must clean up syncobjs

### 9.3 DRM_IOCTL_SYNCOBJ_WAIT
- **File**: `crocus_fence.c` - `crocus_wait_syncobj()`
- **Struct**: `struct drm_syncobj_wait`
- **Parameters IN**: `.handles` (ptr to array), `.count_handles`,
  `.timeout_nsec`, `.flags` (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL etc.)
- **Returns**: 0 on success
- **Purpose**: Waits for one or more syncobjs to be signaled. Used for
  fence waiting (GL sync, Vulkan fences, implicit sync).
- **Gen5 Essential**: YES - needed for fence completion queries

### 9.4 DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD
- **File**: `crocus_fence.c` - fence fd export
- **Struct**: `struct drm_syncobj_handle`
- **Parameters IN**: `.handle`, `.flags` (DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)
- **Returns**: `.fd` (sync file fd)
- **Purpose**: Exports a syncobj as a sync_file fd for cross-process
  fence sharing (EGL_ANDROID_native_fence_sync, explicit sync).
- **Gen5 Essential**: NO - only for explicit fence fd export

---

## SUMMARY TABLE: Gen5 (Ironlake) Minimum Required Ioctls

### Tier 1 - ABSOLUTELY REQUIRED (no GPU without these)
| # | Ioctl | Category |
|---|-------|----------|
| 1 | `DRM_IOCTL_I915_GEM_CREATE` | Buffer alloc |
| 2 | `DRM_IOCTL_GEM_CLOSE` | Buffer free |
| 3 | `DRM_IOCTL_I915_GEM_EXECBUFFER2` | Batch submit |
| 4 | `DRM_IOCTL_I915_GEM_CONTEXT_CREATE` | Context create |
| 5 | `DRM_IOCTL_I915_GEM_CONTEXT_DESTROY` | Context destroy |
| 6 | `DRM_IOCTL_I915_GEM_MMAP` | CPU mapping |
| 7 | `DRM_IOCTL_I915_GEM_WAIT` | Sync/wait |
| 8 | `DRM_IOCTL_I915_GETPARAM` | HW queries |

### Tier 2 - STRONGLY RECOMMENDED (needed for real workloads)
| # | Ioctl | Category |
|---|-------|----------|
| 9 | `DRM_IOCTL_I915_GEM_SET_TILING` | Tiling config |
| 10 | `DRM_IOCTL_I915_GEM_GET_TILING` | Tiling query |
| 11 | `DRM_IOCTL_I915_GEM_MMAP_GTT` | GTT map (tiled) |
| 12 | `DRM_IOCTL_I915_GEM_SET_DOMAIN` | Cache/page alloc |
| 13 | `DRM_IOCTL_I915_GEM_BUSY` | Non-blocking poll |
| 14 | `DRM_IOCTL_I915_GEM_MADVISE` | BO cache mgmt |
| 15 | `DRM_IOCTL_I915_GEM_SET_CACHING` | Cache coherency |
| 16 | `DRM_IOCTL_I915_GEM_GET_APERTURE` | Aperture query |
| 17 | `DRM_IOCTL_SYNCOBJ_CREATE` | Fence sync |
| 18 | `DRM_IOCTL_SYNCOBJ_DESTROY` | Fence sync |
| 19 | `DRM_IOCTL_SYNCOBJ_WAIT` | Fence sync |

### Tier 3 - OPTIONAL (buffer sharing, advanced features)
| # | Ioctl | Category |
|---|-------|----------|
| 20 | `DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM` | Ctx config |
| 21 | `DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM` | Ctx query |
| 22 | `DRM_IOCTL_I915_GET_RESET_STATS` | Hang detect |
| 23 | `DRM_IOCTL_PRIME_HANDLE_TO_FD` | DMABUF export |
| 24 | `DRM_IOCTL_PRIME_FD_TO_HANDLE` | DMABUF import |
| 25 | `DRM_IOCTL_GEM_OPEN` | Legacy share |
| 26 | `DRM_IOCTL_GEM_FLINK` | Legacy share |
| 27 | `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` | Fence export |
| 28 | `DRM_IOCTL_I915_GEM_USERPTR` | Userptr BO |
| 29 | `DRM_IOCTL_I915_GEM_MMAP_OFFSET` | New-style mmap |
| 30 | `DRM_IOCTL_I915_REG_READ` | Register read |

---

## KEY ARCHITECTURAL NOTES FOR HAIKU PORT

### Execution Model
Crocus uses `I915_GEM_EXECBUFFER2` with:
- A **validation list** of `drm_i915_gem_exec_object2` entries
- **Relocations** via `drm_i915_gem_relocation_entry` (kernel patches
  buffer addresses into command stream)
- `I915_EXEC_NO_RELOC` optimization (pre-fill addresses, kernel only
  validates)
- `I915_EXEC_HANDLE_LUT` (reloc target_handle is index into validation
  list, not GEM handle)
- Context ID in `.rsvd1`

### Memory Mapping Strategy (Gen5 / Non-LLC)
Ironlake has NO LLC (Last-Level Cache). Crocus handles this:
1. **CPU map** (DRM_IOCTL_I915_GEM_MMAP, flags=0): Used for cache-coherent
   reads. Requires cache invalidation after GPU writes (`util_flush_inval_range`).
2. **WC map** (DRM_IOCTL_I915_GEM_MMAP, flags=I915_MMAP_WC): Used for
   writes on non-LLC. Write-combining, not cached.
3. **GTT map** (DRM_IOCTL_I915_GEM_MMAP_GTT + mmap): Used for tiled
   buffers. Kernel handles detiling. Also used as fallback for stolen memory.

### BO Cache
Crocus maintains a userspace BO cache with power-of-2-ish bucket sizes.
Freed BOs are marked MADV_DONTNEED. On reuse, MADV_WILLNEED checks if
pages survived. This avoids constant GEM_CREATE/GEM_CLOSE syscalls.

### Haiku Implications
For a Haiku GPU server replacing the i915 kernel driver:
1. **Minimum viable**: Implement Tier 1 ioctls (8 ioctls) as server calls
2. **For real rendering**: Add Tier 2 (11 more, total 19 ioctls)
3. Memory mapping requires either `mmap()` equivalent or shared memory areas
4. The relocation mechanism means the server must be able to patch GPU
   command buffers before submission (or the client pre-fills correct addresses)
5. Syncobj can be simplified to a simple fence/semaphore mechanism on Haiku
6. SET_DOMAIN can be a no-op if pages are always allocated eagerly
7. MADVISE can be a no-op if BO cache is handled entirely in userspace

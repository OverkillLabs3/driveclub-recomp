/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    int32_t render_height;      /* the game's 3D render height after patches and upscaling, 0 unknown */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* After guest finalizers: join presentation/GPU/window threads and destroy their owners. */
void bbgpu_shutdown(void);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Exit dialog can open only after its renderer is ready and without an IME dialog. */
int bbgpu_exit_dialog_available(void);
/* Requested mouse-stick mode; modal cursor ownership is applied on the window thread. */
void bbgpu_mouse_stick_capture(int capture);
/* A short line at the top of the screen for a few seconds (a hotkey's new value). */
void bbgpu_overlay_note(const char *text_utf8);
/* A trophy banner (grade 0 platinum, 1 gold, 2 silver, 3 bronze) with its name and description. */
void bbgpu_overlay_trophy(int id, int grade, const char *title_utf8, const char *description_utf8);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
/* Frames presented so far (a freeze watchdog reads it). */
unsigned long long bbgpu_flip_count(void);
/* The last draws and dispatches recorded (shader hashes), for crash and freeze reports. */
void bbgpu_print_recent_passes(void);
/* BB_SHARED_LOG / BB_SHARED_PAD (gpu/shim/bbport_shared_log.cpp): wrapper for the call sites of the game's SystemShared
 * heap allocator (eboot 0x753a80) and the GDS-count site of the VideoShared one; called as wrapper(size, align, tag, site).
 * 0: not enabled. */
uintptr_t bbgpu_shared_alloc_hook(uintptr_t real_alloc, uintptr_t image_base);
/* BB_EXPOSURE_STALE=1 (gpu/shadps4/video_core/page_manager.cpp): the hook for the auto-exposure histogram copies
 * (SysV memcpy signature). 0: not enabled. */
uintptr_t bbgpu_exposure_hook(void);
uintptr_t bbgpu_exposure_value_hook(uintptr_t real_getter);
/* BB_EXPOSURE_EXACT=1: the hook of the exposure's getter call `site` (0, 1 histograms at 0x702ab9 / 0x702ae3, 2 the
 * value at 0x702b0d). 0: not enabled. */
uintptr_t bbgpu_exposure_exact_hook(uintptr_t real_getter, int site);
/* BB_CLOUD_STALE=1: the hook of the cloud sample's getter call (eboot 0x60265e). 0: not enabled. */
uintptr_t bbgpu_cloud_hook(uintptr_t image_base);
uintptr_t bbgpu_query_hook(uintptr_t image_base);
uintptr_t bbgpu_crowd_hook(uintptr_t image_base);
#ifdef __cplusplus
}
#endif
#endif

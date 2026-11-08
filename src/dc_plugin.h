/* Native plugins for the game runtime. A plugin is a DLL in the folder "plugins" of a mods folder; the runtime loads it
 * when DC_PLUGINS=1 is set (a plugin runs with the full rights of the game, so only install plugins you trust) and calls
 *
 *     int dc_plugin_init(const DcPluginApi *api);     (exported, return 0 for success)
 *
 * once, after the game's code is in memory. The plugin can log, read and write the game's memory, and hook an address:
 * the callback runs each time the game reaches the instruction at that address, gets the registers (which it may
 * change) and the game goes on with the instruction. A game code address is the offset in the game's image,
 * as in a disassembly of it, plus 0x800000000; read and write accept any address of the process (see docs/modding/plugins.md).
 *
 * Build for 64 bit Windows, no runtime library is needed: any C or C++ compiler. */
#ifndef DC_PLUGIN_H
#define DC_PLUGIN_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcRegs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15, rip;
} DcRegs;

typedef void (*DcHookFn)(DcRegs *regs, void *user);

#define DC_PLUGIN_API_VERSION 1

typedef struct DcPluginApi {
    uint32_t version;  /* DC_PLUGIN_API_VERSION */
    uint32_t size;     /* sizeof(DcPluginApi) */
    void (*log)(const char *text);                                           /* a line in the game's log, "Plugin: ..." */
    int (*read)(uint64_t guest_addr, void *out, uint64_t length);           /* 0 on success, -1 when the memory is not readable */
    int (*write)(uint64_t guest_addr, const void *data, uint64_t length);   /* 0 on success, -1 when it is not writable */
    int (*hook)(uint64_t guest_addr, DcHookFn fn, void *user);              /* 0 on success; the address must be the start of an instruction */
} DcPluginApi;

#ifdef __cplusplus
}
#endif
#endif

/* Example native plugin that changes a register: the game builds the message "Couldn't find ExciterDsp params for
 * vehicle(<name>)" with a snprintf call whose format string is in rsi. This plugin hooks the call and points rsi at its
 * own format string, so the log line reads differently. The same technique changes any value the game passes to a
 * function: set a register in the hook and the callee sees the new value.
 * Build:  clang -shared -O2 -o message.dll message.c      Install: mods\plugins\message.dll, start with DC_PLUGINS=1 */
#include "dc_plugin.h"

static const char my_format[] = "Message changed by a plugin: no ExciterDsp params for vehicle(%s)";

static void on_call(DcRegs *r, void *user) {
    (void)user;
    r->rsi = (uint64_t)(uintptr_t)my_format;  /* the plugin's memory is the game's memory: the address works as it is */
}

__declspec(dllexport) int dc_plugin_init(const DcPluginApi *api) {
    if (api->version != DC_PLUGIN_API_VERSION) return 1;
    return api->hook(0x800d67cb8ull, on_call, 0);  /* the call instruction after the game loads its format string */
}

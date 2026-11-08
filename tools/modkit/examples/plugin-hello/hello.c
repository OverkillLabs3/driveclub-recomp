/* Example native plugin: hooks one function of the game and logs how often it runs, reads game memory, and writes it.
 * Build:  clang -shared -O2 -o hello.dll hello.c        (64 bit Windows; MSYS2 CLANG64 or any C compiler)
 * Install: put hello.dll in a mods folder's "plugins" folder and start the game with DC_PLUGINS=1. */
#include "dc_plugin.h"
#include <stdio.h>

static const DcPluginApi *api;
static unsigned calls;

/* runs each time the game reaches the hooked address (here the start of a resource lookup) */
static void on_lookup(DcRegs *r, void *user) {
    (void)user;
    ++calls;
    if (calls == 1 || calls % 20000 == 0) {
        char line[160];
        snprintf(line, sizeof line, "lookup #%u rdi=%llx rsi=%llx", calls, (unsigned long long)r->rdi, (unsigned long long)r->rsi);
        api->log(line);
    }
}

__declspec(dllexport) int dc_plugin_init(const DcPluginApi *a) {
    api = a;
    if (a->version != DC_PLUGIN_API_VERSION) return 1;
    unsigned char code[4];
    if (a->read(0x800780f30ull, code, 4) == 0) {
        char line[80];
        snprintf(line, sizeof line, "code at the hook: %02x %02x %02x %02x", code[0], code[1], code[2], code[3]);
        a->log(line);
    }
    return a->hook(0x800780f30ull, on_lookup, 0);
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cstdio>
#include <cstdlib>

extern "C" void test_guest_debug_exit(void);
static bool pool_alive, stopped;
struct Pool {
    Pool() { pool_alive=true; }
    ~Pool() {
        if (!stopped) {
            std::fputs("FAIL: pool destroyed before presentation stopped\n",stderr);
            std::_Exit(91);
        }
        pool_alive=false;
        std::puts("POOL_DESTROYED_AFTER_GPU");
    }
};
static Pool& pool() { static Pool p;return p; }
extern "C" void bbgpu_shutdown(void) {
    if (stopped) return;
    if (!pool_alive) {
        std::fputs("FAIL: presentation accessed a destroyed pool\n",stderr);
        std::_Exit(92);
    }
    stopped=true;
    std::puts("GPU_STOPPED_WITH_POOL_ALIVE");
}
int main() {
    /* Match production: GPU fallback registered first, copy pool created later.
       Without the explicit shutdown in raise_exception(), reverse CRT order
       destroys the pool first. Preserve the original guest exception status 23. */
    std::atexit(bbgpu_shutdown);
    (void)pool();
    test_guest_debug_exit();
    return 93;
}

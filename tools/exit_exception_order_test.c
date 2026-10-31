/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/runtime_kernel.c"
#include <assert.h>

/* Link against the C++ fixture below to exercise the real exit() path. */
void runtime_report(void) {}
void test_guest_debug_exit(void) { raise_exception(0xa002000a,0); }
/* Unrelated resolver entries are retained by the Windows linker. */
void compat_sleep_ns(uint64_t n) { (void)n;assert(0); }
long compat_utc_offset(time_t t) { (void)t;assert(0);return 0; }
int compat_cpu_times(int w,int64_t *u,int64_t *k) { (void)w;(void)u;(void)k;assert(0);return 0; }
int compat_random(void *p,size_t n) { (void)p;(void)n;assert(0);return 0; }
int32_t *runtime_errno(void) { assert(0);return NULL; }
int runtime_start_linked_module(const char *p,uint64_t n,const void *a,int32_t *r) {
    (void)p;(void)n;(void)a;(void)r;assert(0);return 0;
}
uintptr_t runtime_lookup(const RuntimeExport *e,size_t n,const char *s) {
    (void)e;(void)n;(void)s;assert(0);return 0;
}
void runtime_diag_wait_begin(const char *s,uint64_t n,const void *p) { (void)s;(void)n;(void)p;assert(0); }
void runtime_diag_wait_end(void) { assert(0); }

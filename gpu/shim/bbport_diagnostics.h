// Expensive diagnostics are available only in explicitly enabled diagnostic builds.
#pragma once
#include <stdlib.h>
#ifndef BB_DIAGNOSTICS
#define BB_DIAGNOSTICS 0
#endif
#if BB_DIAGNOSTICS
#define BB_DIAGNOSTIC_ENV(name) getenv(name)
#else
#define BB_DIAGNOSTIC_ENV(name) ((char*)0)
#endif

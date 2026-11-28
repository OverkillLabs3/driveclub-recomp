#pragma once

#include <limits.h>
#ifdef __SSE4_1__
#include <smmintrin.h>
#endif

#define FALSE 0
#define TRUE 1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int Max(int a, int b) { return a > b ? a : b; }
static inline int Min(int a, int b) { return a > b ? b : a; }

static inline unsigned int BitReverse32(unsigned int value, int bitCount)
{
	value = ((value & 0xaaaaaaaaU) >> 1) | ((value & 0x55555555U) << 1);
	value = ((value & 0xccccccccU) >> 2) | ((value & 0x33333333U) << 2);
	value = ((value & 0xf0f0f0f0U) >> 4) | ((value & 0x0f0f0f0fU) << 4);
	value = ((value & 0xff00ff00U) >> 8) | ((value & 0x00ff00ffU) << 8);
	value = (value >> 16) | (value << 16);
	return value >> (32 - bitCount);
}

static inline int SignExtend32(int value, int bits)
{
	const int shift = 8 * (int)sizeof(int) - bits;
	return (value << shift) >> shift;
}

static inline short Clamp16(int value)
{
	if (value > SHRT_MAX)
		return SHRT_MAX;
	if (value < SHRT_MIN)
		return SHRT_MIN;
	return (short)value;
}

static inline int Round(double x)
{
#ifdef __SSE4_1__
	return _mm_cvtsd_si32(_mm_floor_sd(_mm_setzero_pd(), _mm_set_sd(x + 0.5)));
#else
	x += 0.5;
	return (int)x - (x < (int)x);
#endif
}

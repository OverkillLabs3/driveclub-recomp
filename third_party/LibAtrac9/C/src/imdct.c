#include "imdct.h"
#include "tables.h"
#if defined(__AVX__) || defined(__SSE3__)
#include <immintrin.h>
#endif

static void Dct4(Mdct* mdct, const double* restrict input, double* restrict output);

void RunImdct(Mdct* mdct, double* input, double* output)
{
	const int size = 1 << mdct->Bits;
	const int half = size / 2;
	double dctOut[MAX_FRAME_SAMPLES] __attribute__((aligned(32)));
	const double* restrict window = ImdctWindow[mdct->Bits - 6];
	double* restrict previous = mdct->ImdctPrevious;
	double* restrict out = output;

	Dct4(mdct, input, dctOut);

	for (int i = 0; i < half; i++)
	{
		out[i] = window[i] * dctOut[i + half] + previous[i];
		out[i + half] = window[i + half] * -dctOut[size - 1 - i] - previous[i + half];
		previous[i] = window[size - 1 - i] * -dctOut[half - i - 1];
		previous[i + half] = window[half - i - 1] * dctOut[i];
	}
}

static void Dct4(Mdct* mdct, const double* restrict input, double* restrict output)
{
	int MdctBits = mdct->Bits;
	int MdctSize = 1 << MdctBits;
	const int* restrict shuffleTable = ShuffleTables[MdctBits];
	const double* restrict sinTable = SinTables[MdctBits];
	const double* restrict cosTable = CosTables[MdctBits];
	double dctTemp[MAX_FRAME_SAMPLES] __attribute__((aligned(32)));

	int size = MdctSize;
	int lastIndex = size - 1;
	int halfSize = size / 2;

	for (int i = 0; i < halfSize; i++)
	{
		int i2 = i * 2;
		double a = input[i2];
		double b = input[lastIndex - i2];
		double s = sinTable[i];
		double c = cosTable[i];
		dctTemp[i2] = a * c + b * s;
		dctTemp[i2 + 1] = a * s - b * c;
	}
	int stageCount = MdctBits - 1;

#ifdef __AVX__
	const __m256d sign_hi256 = _mm256_set_pd(-0.0, 0.0, -0.0, 0.0);
#endif
#if defined(__AVX__) || defined(__SSE3__)
	const __m128d sign_hi128 = _mm_set_pd(-0.0, 0.0);
#endif

	for (int stage = 0; stage < stageCount; stage++)
	{
		int blockCount = 1 << stage;
		int blockSizeBits = stageCount - stage;
		int blockHalfSizeBits = blockSizeBits - 1;
		int blockSize = 1 << blockSizeBits;
		int blockHalfSize = 1 << blockHalfSizeBits;
		sinTable = SinTables[blockHalfSizeBits];
		cosTable = CosTables[blockHalfSizeBits];

#if defined(__AVX__) || defined(__SSE3__)
		if (blockHalfSize == 1)
		{
			const __m128d v_cs = _mm_set_pd(sinTable[0], cosTable[0]);
			const __m128d v_s_nc = _mm_xor_pd(_mm_shuffle_pd(v_cs, v_cs, 1), sign_hi128);
			for (int block = 0; block < blockCount; block++)
			{
				int frontPos = block * 4;
				int backPos = frontPos + 2;
				__m128d vf = _mm_loadu_pd(&dctTemp[frontPos]);
				__m128d vb = _mm_loadu_pd(&dctTemp[backPos]);
				_mm_storeu_pd(&dctTemp[frontPos], _mm_add_pd(vf, vb));
				__m128d diff = _mm_sub_pd(vf, vb);
				__m128d t1 = _mm_mul_pd(_mm_movedup_pd(diff), v_cs);
				__m128d t2 = _mm_mul_pd(_mm_unpackhi_pd(diff, diff), v_s_nc);
				_mm_storeu_pd(&dctTemp[backPos], _mm_add_pd(t1, t2));
			}
			continue;
		}
#endif
#ifdef __AVX__
		for (int block = 0; block < blockCount; block++)
		{
			int base = block * blockSize * 2;
			for (int i = 0; i < blockHalfSize; i += 2)
			{
				int frontPos = base + i * 2;
				int backPos = frontPos + blockSize;
				__m256d vf = _mm256_loadu_pd(&dctTemp[frontPos]);
				__m256d vb = _mm256_loadu_pd(&dctTemp[backPos]);
				_mm256_storeu_pd(&dctTemp[frontPos], _mm256_add_pd(vf, vb));
				__m256d diff = _mm256_sub_pd(vf, vb);
				__m128d c01 = _mm_loadu_pd(&cosTable[i]);
				__m128d s01 = _mm_loadu_pd(&sinTable[i]);
				__m256d v_cs = _mm256_insertf128_pd(
					_mm256_castpd128_pd256(_mm_unpacklo_pd(c01, s01)),
					_mm_unpackhi_pd(c01, s01), 1);
				__m256d v_s_nc = _mm256_xor_pd(_mm256_permute_pd(v_cs, 0x5), sign_hi256);
				__m256d t1 = _mm256_mul_pd(_mm256_movedup_pd(diff), v_cs);
				__m256d t2 = _mm256_mul_pd(_mm256_unpackhi_pd(diff, diff), v_s_nc);
				_mm256_storeu_pd(&dctTemp[backPos], _mm256_add_pd(t1, t2));
			}
		}
#else
		for (int block = 0; block < blockCount; block++)
		{
			for (int i = 0; i < blockHalfSize; i++)
			{
				int frontPos = (block * blockSize + i) * 2;
				int backPos = frontPos + blockSize;
				double a = dctTemp[frontPos] - dctTemp[backPos];
				double b = dctTemp[frontPos + 1] - dctTemp[backPos + 1];
				double s = sinTable[i];
				double c = cosTable[i];
				dctTemp[frontPos] += dctTemp[backPos];
				dctTemp[frontPos + 1] += dctTemp[backPos + 1];
				dctTemp[backPos] = a * c + b * s;
				dctTemp[backPos + 1] = a * s - b * c;
			}
		}
#endif
	}

	for (int i = 0; i < MdctSize; i++)
	{
		output[i] = dctTemp[shuffleTable[i]];
	}
}
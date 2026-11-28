#include "decoder.h"
#include "band_extension.h"
#include "bit_reader.h"
#include "imdct.h"
#include "quantization.h"
#include "tables.h"
#include "unpack.h"
#include "utility.h"
#include <string.h>

static At9Status DecodeFrame(Frame* frame, BitReaderCxt* br);
static void ImdctBlock(Block* block);
static void ApplyIntensityStereo(Block* block);
static void PcmFloatToShort(Frame* frame, short* pcmOut);

At9Status Decode(Atrac9Handle* handle, const unsigned char* audio, unsigned char* pcm, int* bytesUsed)
{
	BitReaderCxt br;
	InitBitReaderCxt(&br, audio);
	ERROR_CHECK(DecodeFrame(&handle->Frame, &br));

	PcmFloatToShort(&handle->Frame, (short*)pcm);

	*bytesUsed = br.Position / 8;
	return ERR_SUCCESS;
}

static At9Status DecodeFrame(Frame* frame, BitReaderCxt* br)
{
	ERROR_CHECK(UnpackFrame(frame, br));

	for (int i = 0; i < frame->Config->ChannelConfig.BlockCount; i++)
	{
		Block* block = &frame->Blocks[i];

		DequantizeSpectra(block);
		ApplyIntensityStereo(block);
		ScaleSpectrumBlock(block);
		ApplyBandExtension(block);
		ImdctBlock(block);
	}

	return ERR_SUCCESS;
}

void PcmFloatToShort(Frame* frame, short* pcmOut)
{
	const int channelCount = frame->Config->ChannelCount;
	const int sampleCount = frame->Config->FrameSamples;
	Channel** channels = frame->Channels;

#ifdef __SSE4_1__
	const __m128d vhalf = _mm_set1_pd(0.5);
	const __m128d vmin = _mm_set1_pd(-32768.0);
	const __m128d vmax = _mm_set1_pd(32767.0);
	if (channelCount == 1)
	{
		const double* src = channels[0]->Pcm;
		int smpl = 0;
		for (; smpl + 4 <= sampleCount; smpl += 4)
		{
			__m128d d0 = _mm_floor_pd(_mm_add_pd(_mm_loadu_pd(src + smpl), vhalf));
			__m128d d1 = _mm_floor_pd(_mm_add_pd(_mm_loadu_pd(src + smpl + 2), vhalf));
			d0 = _mm_min_pd(_mm_max_pd(d0, vmin), vmax);
			d1 = _mm_min_pd(_mm_max_pd(d1, vmin), vmax);
			__m128i i0 = _mm_cvtpd_epi32(d0);
			__m128i i1 = _mm_cvtpd_epi32(d1);
			__m128i packed = _mm_packs_epi32(_mm_unpacklo_epi64(i0, i1), _mm_setzero_si128());
			_mm_storel_epi64((__m128i*)(pcmOut + smpl), packed);
		}
		for (; smpl < sampleCount; smpl++)
		{
			pcmOut[smpl] = Clamp16(Round(src[smpl]));
		}
		return;
	}
	if (channelCount == 2)
	{
		const double* src0 = channels[0]->Pcm;
		const double* src1 = channels[1]->Pcm;
		int smpl = 0;
		for (; smpl + 2 <= sampleCount; smpl += 2)
		{
			__m128d d0 = _mm_floor_pd(_mm_add_pd(_mm_loadu_pd(src0 + smpl), vhalf));
			__m128d d1 = _mm_floor_pd(_mm_add_pd(_mm_loadu_pd(src1 + smpl), vhalf));
			d0 = _mm_min_pd(_mm_max_pd(d0, vmin), vmax);
			d1 = _mm_min_pd(_mm_max_pd(d1, vmin), vmax);
			__m128i i0 = _mm_cvtpd_epi32(d0);
			__m128i i1 = _mm_cvtpd_epi32(d1);
			__m128i interleaved = _mm_unpacklo_epi32(i0, i1);
			__m128i packed = _mm_packs_epi32(interleaved, _mm_setzero_si128());
			_mm_storel_epi64((__m128i*)(pcmOut + smpl * 2), packed);
		}
		for (; smpl < sampleCount; smpl++)
		{
			pcmOut[smpl * 2] = Clamp16(Round(src0[smpl]));
			pcmOut[smpl * 2 + 1] = Clamp16(Round(src1[smpl]));
		}
		return;
	}
#endif

	int i = 0;
	for (int smpl = 0; smpl < sampleCount; smpl++)
	{
		for (int ch = 0; ch < channelCount; ch++, i++)
		{
			pcmOut[i] = Clamp16(Round(channels[ch]->Pcm[smpl]));
		}
	}
}

static void ImdctBlock(Block* block)
{
	for (int i = 0; i < block->ChannelCount; i++)
	{
		Channel* channel = &block->Channels[i];

		RunImdct(&channel->Mdct, channel->Spectra, channel->Pcm);
	}
}

static void ApplyIntensityStereo(Block* block)
{
	if (block->BlockType != Stereo) return;

	const int totalUnits = block->QuantizationUnitCount;
	const int stereoUnits = block->StereoQuantizationUnit;
	if (stereoUnits >= totalUnits) return;

	Channel* source = &block->Channels[block->PrimaryChannelIndex == 0 ? 0 : 1];
	Channel* dest = &block->Channels[block->PrimaryChannelIndex == 0 ? 1 : 0];

	for (int i = stereoUnits; i < totalUnits; i++)
	{
		const int sign = block->JointStereoSigns[i];
		for (int sb = QuantUnitToCoeffIndex[i]; sb < QuantUnitToCoeffIndex[i + 1]; sb++)
		{
			if (sign > 0)
			{
				dest->Spectra[sb] = -source->Spectra[sb];
			}
			else
			{
				dest->Spectra[sb] = source->Spectra[sb];
			}
		}
	}
}

int GetCodecInfo(Atrac9Handle* handle, CodecInfo * pCodecInfo)
{
	pCodecInfo->Channels = handle->Config.ChannelCount;
	pCodecInfo->ChannelConfigIndex = handle->Config.ChannelConfigIndex;
	pCodecInfo->SamplingRate = handle->Config.SampleRate;
	pCodecInfo->SuperframeSize = handle->Config.SuperframeBytes;
	pCodecInfo->FramesInSuperframe = handle->Config.FramesPerSuperframe;
	pCodecInfo->FrameSamples = handle->Config.FrameSamples;
	pCodecInfo->Wlength = handle->Wlength;
	memcpy(pCodecInfo->ConfigData, handle->Config.ConfigData, CONFIG_DATA_SIZE);
	return ERR_SUCCESS;
}

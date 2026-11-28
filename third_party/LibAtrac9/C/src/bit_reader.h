#pragma once

#include "utility.h"

typedef struct {
	const unsigned char * Buffer;
	int Position;
} BitReaderCxt;

int PeekIntFallback(BitReaderCxt* br, int bitCount);

static inline void InitBitReaderCxt(BitReaderCxt* br, const void * buffer)
{
	br->Buffer = (const unsigned char *)buffer;
	br->Position = 0;
}

static inline int PeekInt(BitReaderCxt* br, const int bits)
{
	const int byteIndex = br->Position >> 3;
	const int bitIndex = br->Position & 7;
	const unsigned char* buffer = br->Buffer;

	if (bits <= 9)
	{
		int value = (buffer[byteIndex] << 8) | buffer[byteIndex + 1];
		value &= 0xFFFF >> bitIndex;
		value >>= 16 - bits - bitIndex;
		return value;
	}

	if (bits <= 17)
	{
		int value = (buffer[byteIndex] << 16) | (buffer[byteIndex + 1] << 8) | buffer[byteIndex + 2];
		value &= 0xFFFFFF >> bitIndex;
		value >>= 24 - bits - bitIndex;
		return value;
	}

	if (bits <= 25)
	{
		unsigned int value = ((unsigned int)buffer[byteIndex] << 24)
			| ((unsigned int)buffer[byteIndex + 1] << 16)
			| ((unsigned int)buffer[byteIndex + 2] << 8)
			| (unsigned int)buffer[byteIndex + 3];
		value &= 0xFFFFFFFFU >> bitIndex;
		value >>= 32 - bits - bitIndex;
		return (int)value;
	}
	return PeekIntFallback(br, bits);
}

static inline int ReadInt(BitReaderCxt* br, const int bits)
{
	const int value = PeekInt(br, bits);
	br->Position += bits;
	return value;
}

static inline int ReadSignedInt(BitReaderCxt* br, const int bits)
{
	const int value = PeekInt(br, bits);
	br->Position += bits;
	return SignExtend32(value, bits);
}

static inline int ReadOffsetBinary(BitReaderCxt* br, const int bits)
{
	const int offset = 1 << (bits - 1);
	const int value = PeekInt(br, bits) - offset;
	br->Position += bits;
	return value;
}

static inline void AlignPosition(BitReaderCxt* br, const unsigned int multiple)
{
	const int position = br->Position;
	if ((unsigned int)position % multiple == 0)
	{
		return;
	}

	br->Position = position + (int)(multiple - (unsigned int)position % multiple);
}

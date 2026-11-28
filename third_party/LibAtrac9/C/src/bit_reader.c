#include "bit_reader.h"

int PeekIntFallback(BitReaderCxt* br, int bitCount)
{
	int value = 0;
	int byteIndex = br->Position / 8;
	int bitIndex = br->Position % 8;
	const unsigned char* buffer = br->Buffer;

	while (bitCount > 0)
	{
		if (bitIndex >= 8)
		{
			bitIndex = 0;
			byteIndex++;
		}

		int bitsToRead = bitCount;
		if (bitsToRead > 8 - bitIndex)
		{
			bitsToRead = 8 - bitIndex;
		}

		const int mask = 0xFF >> bitIndex;
		const int currentByte = (mask & buffer[byteIndex]) >> (8 - bitIndex - bitsToRead);

		value = (value << bitsToRead) | currentByte;
		bitIndex += bitsToRead;
		bitCount -= bitsToRead;
	}
	return value;
}
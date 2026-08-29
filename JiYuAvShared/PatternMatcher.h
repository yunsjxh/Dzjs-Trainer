#pragma once

static __inline int
JiYuAvPatternMatches(
    const unsigned char* data,
    const unsigned char* pattern,
    const unsigned char* mask,
    unsigned long length
    )
{
    unsigned long index;

    for (index = 0; index < length; ++index) {
        if ((data[index] & mask[index]) != (pattern[index] & mask[index])) {
            return 0;
        }
    }
    return 1;
}

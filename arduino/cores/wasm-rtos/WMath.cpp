#include "Arduino.h"

void randomSeed(unsigned long seed)
{
    if (seed != 0)
        srand((unsigned int)seed);
}

long random(long max) { return max <= 0 ? 0 : (long)(rand() % max); }

long random(long min, long max) { return min >= max ? min : random(max - min) + min; }

long map(long value, long from_low, long from_high, long to_low, long to_high)
{
    if (from_high == from_low)
        return to_low;
    return (value - from_low) * (to_high - to_low) / (from_high - from_low) + to_low;
}

uint16_t makeWord(uint16_t w) { return w; }
uint16_t makeWord(uint8_t high, uint8_t low) { return (uint16_t)((high << 8) | low); }

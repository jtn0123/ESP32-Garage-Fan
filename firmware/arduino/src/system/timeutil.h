#pragma once
// One shared definition of "do we know what time it is yet".
//
// SNTP start is fired in setup() but sync lands whenever the network allows;
// until then time(nullptr) counts up from the epoch. Anything stamping samples
// or judging staleness must gate on this rather than trusting a 1970 clock.
// The constant is simply "well after this firmware was written".

#include <ctime>

inline bool time_synced() { return time(nullptr) > 1700000000; }

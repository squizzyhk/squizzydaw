/* Squizzy Studio: single implementation TU for the unmodified upstream dr_mp3.h.
   Internal sanity asserts are compiled out in every configuration (as in a release build) so a
   corrupted stream reaching an upstream assert fails the call instead of aborting the process;
   codec-mp3 bounds every call it makes. */
#define DRMP3_ASSERT(expression) ((void)0)
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

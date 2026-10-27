/* Squizzy Studio: single implementation TU for the unmodified upstream dr_flac.h.
   Internal sanity asserts are compiled out in every configuration (as in a release build) so a
   corrupted stream reaching an upstream assert fails the call instead of aborting the process;
   codec-flac additionally validates STREAMINFO before seeking. */
#define DRFLAC_ASSERT(expression) ((void)0)
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

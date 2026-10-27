/* Squizzy Studio: single implementation TU for the unmodified upstream stb_vorbis.c.
   assert() is compiled out in every configuration (as in a release build) so corrupted input
   fails the call instead of aborting the process. */
#ifndef NDEBUG
#define NDEBUG
#endif
#include "stb_vorbis.c"

// Internal debug logging for the ewebview core.
//
// The core has no platform logger of its own (that would be a porting
// concern); diagnostics are compiled in only with -DEWEBVIEW_DEBUG and go to
// stderr, which every libc provides. Release builds keep the arguments
// referenced under an if(0) so perf-timing locals do not trip -Wunused,
// while the dead branch is eliminated and nothing is printed.

#pragma once

#include <stdio.h>

#ifdef EWEBVIEW_DEBUG
#define EWEB_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define EWEB_LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#endif

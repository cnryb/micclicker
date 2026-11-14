#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Device-side G.722 check: encodes the committed reference frame and compares the
// bitstream with the ffmpeg-produced expectation, decodes it back, and times 100
// frames against the 20 ms audio budget. Returns true only when all checks pass.
bool micclicker_g722_selftest_run(void);

#ifdef __cplusplus
}
#endif

// The boot screen: a BIOS-style power-on self test. Each start-up step writes one line with what it really
// found (memory sizes, devices probed and their state) and a progress bar fills. Raw text drawn straight
// into the screen before the window system starts; lines logged before the display is up are kept and
// shown when it is.
#pragma once
#include <stdbool.h>

typedef enum { FH1_POST_INFO, FH1_POST_OK, FH1_POST_WARN, FH1_POST_OFF, FH1_POST_FAIL } fh1_post_status_t;

void fh1_post_begin(int steps);                       // how many steps the progress bar counts
void fh1_post(fh1_post_status_t st, const char *label, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void fh1_post_step(void);                             // one step done: the bar moves
void fh1_post_display_ready(void);                    // the screen works: draw what was logged
void fh1_post_end(int hold_ms);                       // leave it up a moment, then let go

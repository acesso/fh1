// The running full-screen app and the system: signals it checks once per frame, and the restart routine.
#pragma once
#include <stdbool.h>

// The window system wants the screen for a moment (an event's overlay): the app stops its frame loop,
// lends the screen (fh1_ui_lend_screen) and carries on when that returns.
void fh1_app_request_yield(bool on);
bool fh1_app_should_yield(void);
// The system is shutting down: the app closes (saving what it must) and gives the screen back.
void fh1_app_request_quit(void);
bool fh1_app_quit_requested(void);

// Restart gracefully: the running app closes, the windows close one by one, then the chip restarts.
// From any task. If the window system does not finish within a few seconds, it restarts anyway.
void fh1_system_restart(void);

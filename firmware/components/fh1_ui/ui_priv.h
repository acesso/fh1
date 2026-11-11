// Shared between ui.c and ui_sound.c (not part of the UI's public interface).
#pragma once
#include "fh1_ui.h"

// The system sounds, and the voices a theme speaks them in (ui_sound.c).
enum { UI_SND_MOVE, UI_SND_SELECT, UI_SND_BACK, UI_SND_ALERT, UI_SND_NOTE, UI_SND_OPEN, UI_SND_COUNT };
enum { UI_VOICE_SOFT, UI_VOICE_ALARM, UI_VOICE_GARDEN, UI_VOICE_SCIFI, UI_VOICES };

void ui_sound(int voice, int what);

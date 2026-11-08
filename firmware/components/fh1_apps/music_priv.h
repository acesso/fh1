// Between the Music app's library (apps_music.c) and its deck (music_deck.c).
#pragma once

typedef enum { MUSIC_REPEAT_ALL, MUSIC_REPEAT_ONE, MUSIC_SHUFFLE, MUSIC_MODES } music_mode_t;

// apps_music.c. From any task: the playlist work happens on the UI task.
void music_step(int step);                 // +1 next, -1 previous (in shuffle: a random one)
void music_play_current(void);             // the playlist's track again (after a stop)
music_mode_t music_mode(void);
void music_set_mode(music_mode_t m);       // saved
void music_position(int *index, int *count);
void music_deck_closed(void);              // UI task

// music_deck.c (UI task): the full-screen deck, until B or Menu.
void music_deck_open(void);

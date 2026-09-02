/*
 * VoCat bridge for the local LVGL Music Player demo.
 *
 * The actual demo sources are compiled as separate C translation units under
 * bilibili/story/music. Keeping this file tiny avoids the old "include .c"
 * pattern and the resulting static-symbol collisions.
 */
#include "music/lv_demo_music_main.h"

bool vocat_lvgl_music_is_playing(void)
{
    return vocat_lv_demo_music_is_playing();
}

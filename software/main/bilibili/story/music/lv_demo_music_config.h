#ifndef VOCAT_LVGL_MUSIC_CONFIG_H
#define VOCAT_LVGL_MUSIC_CONFIG_H

/* This copy is a standalone Music Player demo used by VoCat.
 * It intentionally does not depend on LVGL's demo registry (lv_demos.c). */
#ifdef LV_USE_DEMO_MUSIC
#undef LV_USE_DEMO_MUSIC
#endif
#define LV_USE_DEMO_MUSIC 1

#ifdef LV_DEMO_MUSIC_ROUND
#undef LV_DEMO_MUSIC_ROUND
#endif
#define LV_DEMO_MUSIC_ROUND 1

#ifdef LV_DEMO_MUSIC_SQUARE
#undef LV_DEMO_MUSIC_SQUARE
#endif
#define LV_DEMO_MUSIC_SQUARE 0

#ifdef LV_DEMO_MUSIC_LANDSCAPE
#undef LV_DEMO_MUSIC_LANDSCAPE
#endif
#define LV_DEMO_MUSIC_LANDSCAPE 0

#ifdef LV_DEMO_MUSIC_LARGE
#undef LV_DEMO_MUSIC_LARGE
#endif
#define LV_DEMO_MUSIC_LARGE 0

#ifdef LV_DEMO_MUSIC_AUTO_PLAY
#undef LV_DEMO_MUSIC_AUTO_PLAY
#endif
#define LV_DEMO_MUSIC_AUTO_PLAY 0

#endif /* VOCAT_LVGL_MUSIC_CONFIG_H */

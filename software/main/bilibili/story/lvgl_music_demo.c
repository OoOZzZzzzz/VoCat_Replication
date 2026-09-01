/*
 * LVGL 9.5.0 Music Player demo adapter.
 *
 * This file is deliberately compiled as C. The official LVGL demo sources
 * are C sources and must not be included directly from bilibili_ui_lvgl.cpp.
 */
#include "lvgl.h"
#include "../../../managed_components/lvgl__lvgl/demos/lv_demos.h"

#undef LV_BUILD_DEMOS
#define LV_BUILD_DEMOS 1
#undef LV_USE_DEMO_MUSIC
#define LV_USE_DEMO_MUSIC 1
#undef LV_DEMO_MUSIC_ROUND
#define LV_DEMO_MUSIC_ROUND 1
#undef LV_DEMO_MUSIC_SQUARE
#define LV_DEMO_MUSIC_SQUARE 0
#undef LV_DEMO_MUSIC_LANDSCAPE
#define LV_DEMO_MUSIC_LANDSCAPE 0
#undef LV_DEMO_MUSIC_LARGE
#define LV_DEMO_MUSIC_LARGE 0
#undef LV_DEMO_MUSIC_AUTO_PLAY
#define LV_DEMO_MUSIC_AUTO_PLAY 0

/*
 * lv_demo_args_init() normally comes from demos/lv_demos.c.
 * This adapter builds only the Music Demo sources, so provide the same
 * minimal initializer here instead of adding the whole demo registry.
 */
void lv_demo_args_init(lv_demo_args_t * args)
{
    if(args == NULL) return;
    args->parent = NULL;
}

#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_corner_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_list_pause.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_list_pause_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_list_play.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_list_play_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_loop.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_loop_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_next.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_next_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_pause.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_pause_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_play.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_play_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_prev.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_prev_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_rnd.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_btn_rnd_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_corner_left.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_corner_left_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_corner_right.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_corner_right_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_cover_1.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_cover_1_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_cover_2.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_cover_2_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_cover_3.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_cover_3_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_1.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_1_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_2.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_2_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_3.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_3_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_4.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_icon_4_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_list_border.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_list_border_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_logo.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_slider_knob.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_slider_knob_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_wave_bottom.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_wave_bottom_large.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_wave_top.c"
#include "../../../managed_components/lvgl__lvgl/demos/music/assets/img_lv_demo_music_wave_top_large.c"

/* Keep the three official source files in one C translation unit without
 * colliding file-static variables that normally have separate translation
 * units in the LVGL build. */
#define list lv_demo_music_list_impl_list
#define font_small lv_demo_music_list_impl_font_small
#include "../../../managed_components/lvgl__lvgl/demos/music/lv_demo_music_list.c"
#undef font_small
#undef list

#define font_small lv_demo_music_main_impl_font_small
#define font_large lv_demo_music_main_impl_font_large
#define music_height lv_demo_music_main_impl_music_height
#define playing vocat_lvgl_music_playing
#include "../../../managed_components/lvgl__lvgl/demos/music/lv_demo_music_main.c"
#undef playing
#undef music_height
#undef font_large
#undef font_small

bool vocat_lvgl_music_is_playing(void)
{
    return vocat_lvgl_music_playing;
}

#define ctrl lv_demo_music_impl_ctrl
#define list lv_demo_music_impl_list
#define music_height lv_demo_music_impl_music_height
#include "../../../managed_components/lvgl__lvgl/demos/music/lv_demo_music.c"
#undef music_height
#undef list
#undef ctrl

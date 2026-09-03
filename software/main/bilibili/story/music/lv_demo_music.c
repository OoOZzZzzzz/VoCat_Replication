/**
 * VoCat local LVGL Music Player demo entry point.
 */
#include "lv_demo_music.h"
#if LV_USE_DEMO_MUSIC

#include "lv_demo_music_main.h"
#include "lv_demo_music_list.h"

#define MUSIC_PAGE_HEIGHT       360
#define MUSIC_PAGE_COUNT        2
#define MUSIC_SWIPE_ANIM_MS     80

static lv_obj_t * music_root;
static lv_obj_t * ctrl;
static lv_obj_t * list;
static uint8_t music_page;

static void music_page_anim_exec(void * var, int32_t value)
{
    lv_obj_t * root = (lv_obj_t *)var;
    lv_obj_set_y(root, -value);
}

static void music_page_to(uint8_t page, bool animated)
{
    if(page > 1U) {
        page = 1U;
    }

    const int32_t target_y = (int32_t)page * MUSIC_PAGE_HEIGHT;
    const int32_t current_y = -lv_obj_get_y(music_root);

    if(current_y == target_y) {
        music_page = page;
        return;
    }

    lv_anim_del(music_root, music_page_anim_exec);

    if(!animated) {
        lv_obj_set_y(music_root, -target_y);
        music_page = page;
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, music_root);
    lv_anim_set_values(&anim, current_y, target_y);
    lv_anim_set_duration(&anim, MUSIC_SWIPE_ANIM_MS);
    lv_anim_set_exec_cb(&anim, music_page_anim_exec);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);

    music_page = page;
}

static void music_page_gesture_cb(lv_event_t * e)
{
    LV_UNUSED(e);

    if(music_root == NULL) {
        return;
    }

    lv_indev_t * indev = lv_indev_active();
    if(indev == NULL) {
        return;
    }

    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    /* Page 0 is a hard upper boundary: there is deliberately no reverse
     * scroll path, overscroll or rubber-band effect. */
    if(dir == LV_DIR_TOP && music_page == 0U) {
        LV_LOG_USER("[MUSIC] swipe up: page 0 -> page 1");
        music_page_to(1U, true);
    }
    else if(dir == LV_DIR_BOTTOM && music_page == 1U) {
        LV_LOG_USER("[MUSIC] swipe down: page 1 -> page 0");
        music_page_to(0U, true);
    }
}

void vocat_lv_demo_music(void)
{
    vocat_lv_demo_args_t args;
    args.parent = NULL;
    vocat_lv_demo_music_with_args(&args);
}

void vocat_lv_demo_music_with_args(const vocat_lv_demo_args_t * args)
{
    LV_ASSERT_NULL(args);

    lv_obj_t * parent = args->parent;
    if(parent == NULL) {
        parent = lv_screen_active();
    }

    music_root = lv_obj_create(parent);
    lv_obj_remove_style_all(music_root);
    lv_obj_set_size(music_root, lv_pct(100), MUSIC_PAGE_HEIGHT * MUSIC_PAGE_COUNT);
    lv_obj_set_pos(music_root, 0, 0);
    lv_obj_set_style_bg_opa(music_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(music_root, lv_color_hex(0x343247), 0);
    /* Do not use LVGL scrolling for the outer two-page layout. LVGL's
     * momentum/elastic scroll requires repeated full-screen redraws on the
     * SPI panel and is the source of the visible jelly/rubber-band motion.
     * The page transition is therefore an explicit, bounded 360 px move. */
    lv_obj_clear_flag(music_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(music_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(music_root, music_page_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(music_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    list = vocat_lv_demo_music_list_create(music_root);
    ctrl = vocat_lv_demo_music_main_create(music_root);

    LV_UNUSED(ctrl);
    LV_UNUSED(list);

    music_page = 0U;
    lv_obj_set_y(music_root, 0);
}

const char * vocat_lv_demo_music_get_title(uint32_t track_id)
{
    static const char * const titles[] = {
        "Waiting for true love",
        "Need a Better Future",
        "Vibrations",
        "Why now?",
        "Never Look Back",
        "It happened Yesterday",
        "Feeling so High",
        "Go Deeper",
        "Find You There",
        "Until the End",
        "Unknown",
        "Unknown",
        "Unknown",
        "Unknown",
    };
    if(track_id >= sizeof(titles) / sizeof(titles[0])) return NULL;
    return titles[track_id];
}

const char * vocat_lv_demo_music_get_artist(uint32_t track_id)
{
    static const char * const artists[] = {
        "The John Smith Band",
        "My True Name",
        "Robotics",
        "John Smith",
        "My True Name",
        "Robotics",
        "Robotics",
        "Unknown artist",
        "Unknown artist",
        "Unknown artist",
        "Unknown artist",
        "Unknown artist",
        "Unknown artist",
        "Unknown artist",
    };
    if(track_id >= sizeof(artists) / sizeof(artists[0])) return NULL;
    return artists[track_id];
}

const char * vocat_lv_demo_music_get_genre(uint32_t track_id)
{
    static const char * const genres[] = {
        "Rock - 1997",
        "Drum'n bass - 2016",
        "Psy trance - 2020",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
        "Metal - 2015",
    };
    if(track_id >= sizeof(genres) / sizeof(genres[0])) return NULL;
    return genres[track_id];
}

uint32_t vocat_lv_demo_music_get_track_length(uint32_t track_id)
{
    static const uint32_t lengths[] = {
        1U * 60U + 14U,
        2U * 60U + 26U,
        1U * 60U + 54U,
        2U * 60U + 24U,
        2U * 60U + 37U,
        3U * 60U + 33U,
        1U * 60U + 56U,
        3U * 60U + 31U,
        2U * 60U + 20U,
        2U * 60U + 19U,
        2U * 60U + 20U,
        2U * 60U + 19U,
        2U * 60U + 20U,
        2U * 60U + 19U,
    };
    if(track_id >= sizeof(lengths) / sizeof(lengths[0])) return 0;
    return lengths[track_id];
}

#endif /* LV_USE_DEMO_MUSIC */

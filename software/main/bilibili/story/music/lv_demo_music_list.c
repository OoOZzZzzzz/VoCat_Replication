#include "lv_demo_music_list.h"
#include "lv_demo_music_main.h"

#if LV_USE_DEMO_MUSIC

static lv_obj_t *list;
static lv_style_t style_scrollbar;
static lv_style_t style_btn;
static lv_style_t style_button_pr;
static lv_style_t style_button_chk;
static lv_style_t style_button_dis;
static lv_style_t style_title;
static lv_style_t style_artist;
static lv_style_t style_time;
static const lv_font_t *font_small;
static const lv_font_t *font_medium;

LV_IMAGE_DECLARE(vocat_music_btn_list_play);
LV_IMAGE_DECLARE(vocat_music_btn_list_pause);
LV_IMAGE_DECLARE(vocat_music_list_border);

static void list_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    lv_style_reset(&style_scrollbar);
    lv_style_reset(&style_btn);
    lv_style_reset(&style_button_pr);
    lv_style_reset(&style_button_chk);
    lv_style_reset(&style_button_dis);
    lv_style_reset(&style_title);
    lv_style_reset(&style_artist);
    lv_style_reset(&style_time);
    list = NULL;
}

static void button_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const uint32_t index = (uint32_t)lv_obj_get_index(btn);
    vocat_lv_demo_music_play(index);
    lv_obj_t *root = lv_obj_get_parent(list);
    if (root) lv_obj_scroll_to_y(root, 0, LV_ANIM_ON);
}

static void init_styles(void)
{
    font_small = LV_FONT_DEFAULT;
    font_medium = LV_FONT_DEFAULT;
#if LV_DEMO_MUSIC_LARGE
# if LV_FONT_MONTSERRAT_16
    font_small = &lv_font_montserrat_16;
# endif
# if LV_FONT_MONTSERRAT_22
    font_medium = &lv_font_montserrat_22;
# endif
#else
# if LV_FONT_MONTSERRAT_12
    font_small = &lv_font_montserrat_12;
# endif
# if LV_FONT_MONTSERRAT_16
    font_medium = &lv_font_montserrat_16;
# endif
#endif

    lv_style_init(&style_scrollbar);
    lv_style_set_width(&style_scrollbar, 4);
    lv_style_set_bg_opa(&style_scrollbar, LV_OPA_COVER);
    lv_style_set_bg_color(&style_scrollbar, lv_color_hex3(0xeee));
    lv_style_set_radius(&style_scrollbar, LV_RADIUS_CIRCLE);

    static const int32_t cols[] = {LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    static const int32_t rows[] = {22, 17, LV_GRID_TEMPLATE_LAST};
    lv_style_init(&style_btn);
    lv_style_set_bg_opa(&style_btn, LV_OPA_TRANSP);
    lv_style_set_grid_column_dsc_array(&style_btn, cols);
    lv_style_set_grid_row_dsc_array(&style_btn, rows);
    lv_style_set_grid_row_align(&style_btn, LV_GRID_ALIGN_CENTER);
    lv_style_set_layout(&style_btn, LV_LAYOUT_GRID);
    lv_style_set_pad_right(&style_btn, 20);

    lv_style_init(&style_button_pr);
    lv_style_set_bg_opa(&style_button_pr, LV_OPA_COVER);
    lv_style_set_bg_color(&style_button_pr, lv_color_hex(0x4c4965));

    lv_style_init(&style_button_chk);
    lv_style_set_bg_opa(&style_button_chk, LV_OPA_COVER);
    lv_style_set_bg_color(&style_button_chk, lv_color_hex(0x4c4965));

    lv_style_init(&style_button_dis);
    lv_style_set_text_opa(&style_button_dis, LV_OPA_40);

    lv_style_init(&style_title);
    lv_style_set_text_font(&style_title, font_medium);
    lv_style_set_text_color(&style_title, lv_color_hex(0xffffff));

    lv_style_init(&style_artist);
    lv_style_set_text_font(&style_artist, font_small);
    lv_style_set_text_color(&style_artist, lv_color_hex(0xb1b0be));

    lv_style_init(&style_time);
    lv_style_set_text_font(&style_time, font_medium);
    lv_style_set_text_color(&style_time, lv_color_hex(0xffffff));
}

static lv_obj_t *add_button(lv_obj_t *parent, uint32_t id)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, lv_pct(100), 60);
    lv_obj_add_style(btn, &style_btn, 0);
    lv_obj_add_style(btn, &style_button_pr, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &style_button_chk, LV_STATE_CHECKED);
    lv_obj_add_style(btn, &style_button_dis, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn, button_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_image_create(btn);
    lv_image_set_src(icon, &vocat_music_btn_list_play);
    lv_obj_set_grid_cell(icon, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 2);

    lv_obj_t *title = lv_label_create(btn);
    lv_label_set_text(title, vocat_lv_demo_music_get_title(id));
    lv_obj_set_width(title, 200);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_grid_cell(title, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_add_style(title, &style_title, 0);

    lv_obj_t *artist = lv_label_create(btn);
    lv_label_set_text(artist, vocat_lv_demo_music_get_artist(id));
    lv_obj_set_grid_cell(artist, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);
    lv_obj_add_style(artist, &style_artist, 0);

    lv_obj_t *time = lv_label_create(btn);
    const uint32_t t = vocat_lv_demo_music_get_track_length(id);
    lv_label_set_text_fmt(time, "%"LV_PRIu32":%02"LV_PRIu32, t / 60U, t % 60U);
    lv_obj_set_grid_cell(time, LV_GRID_ALIGN_END, 2, 1, LV_GRID_ALIGN_CENTER, 0, 2);
    lv_obj_add_style(time, &style_time, 0);

    lv_obj_t *border = lv_image_create(btn);
    lv_image_set_src(border, &vocat_music_list_border);
    lv_image_set_inner_align(border, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_width(border, lv_pct(120));
    lv_obj_align(border, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(border, LV_OBJ_FLAG_IGNORE_LAYOUT);
    return btn;
}

lv_obj_t *vocat_lv_demo_music_list_create(lv_obj_t *parent)
{
    init_styles();
    list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), 340);
    lv_obj_set_y(list, 370);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_style(list, &style_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(list, list_delete_cb, LV_EVENT_DELETE, NULL);

    vocat_lv_demo_music_list_rebuild();
    return list;
}

void vocat_lv_demo_music_list_rebuild(void)
{
    if (list == NULL) return;
    lv_obj_clean(list);
    const uint8_t count = vocat_lv_demo_music_get_track_count();
    for (uint8_t i = 0; i < count; ++i) add_button(list, i);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "等待B站视频列表...");
        lv_obj_set_style_text_color(empty, lv_color_hex(0xb1b0be), 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        vocat_lv_demo_music_list_button_check(0, false);
    }
}

void vocat_lv_demo_music_list_button_check(uint32_t track_id, bool state)
{
    if (list == NULL || track_id >= (uint32_t)lv_obj_get_child_count(list)) return;
    lv_obj_t *btn = lv_obj_get_child(list, track_id);
    if (!btn) return;
    lv_obj_t *icon = lv_obj_get_child(btn, 0);
    if (!icon) return;

    if (state) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        lv_image_set_src(icon, &vocat_music_btn_list_pause);
    } else {
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
        lv_image_set_src(icon, &vocat_music_btn_list_play);
    }
}

#endif

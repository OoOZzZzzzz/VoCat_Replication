#include "bilibili_ui.h"
#include "bilibili_audio.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "display/emote_display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" {
#include "music/lv_demo_music.h"
#include "music/lv_demo_music_main.h"
}

#include "expression_emote.h"

#define TAG "BILI_STORY_LVGL"

#ifndef VOCAT_BILI_LOG_LEVEL
#define VOCAT_BILI_LOG_LEVEL 2
#endif

#if VOCAT_BILI_LOG_LEVEL >= 1
#define BILI_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define BILI_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define BILI_LOGE(...) do { } while (0)
#define BILI_LOGW(...) do { } while (0)
#endif
#if VOCAT_BILI_LOG_LEVEL >= 2
#define BILI_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define BILI_LOGI(...) do { } while (0)
#endif

namespace {
constexpr uint32_t LCD_WIDTH = 360;
constexpr uint32_t LCD_HEIGHT = 360;
constexpr uint32_t DRAW_BUF_LINES = 30;
constexpr uint32_t DRAW_BUF_BYTES = LCD_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t);
constexpr uint32_t LVGL_WAKE_NOTIFY = 1U << 0;
constexpr uint32_t LVGL_COMMAND_NOTIFY = 1U << 1;
constexpr uint32_t LVGL_DATA_NOTIFY = 1U << 2;

struct TouchSample {
    int16_t x;
    int16_t y;
    lv_indev_state_t state;
};

enum class Command : uint8_t {
    None = 0,
    Play,
    Pause,
    Resume,
    Previous,
    Next,
    Close,
};

struct State {
    std::atomic<bool> active{false};
    std::atomic<bool> touch_active{false};
    bool emote_hidden = false;
    bool runtime_ready = false;
    bool input_ready = false;

    SemaphoreHandle_t flush_done = nullptr;
    emote::EmoteDisplay *emote_display = nullptr;
    emote_handle_t emote_handle = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    lv_display_t *display = nullptr;
    lv_indev_t *indev = nullptr;
    lv_obj_t *root = nullptr;
    QueueHandle_t touch_queue = nullptr;
    TouchSample last_touch{0, 0, LV_INDEV_STATE_RELEASED};

    void *draw_buf = nullptr;
    volatile lv_display_t *pending_flush_display = nullptr;
    volatile bool flush_pending = false;
};

State s;
TaskHandle_t lvgl_task = nullptr;
TaskHandle_t search_task = nullptr;
std::mutex search_mutex;
char search_request_name[64] = {};
std::atomic<uint32_t> search_request_generation{0};
std::atomic<bool> search_task_ready{false};
constexpr uint32_t SEARCH_NOTIFY = 1U << 0;
std::mutex pending_mutex;
/* Search results live outside the search task stack. 8 records contain several
 * hundred bytes each; keeping this buffer static avoids stack pressure while
 * vocat_bilibili_search_up() performs HTTP/JSON work. */
bili_video_t search_result_buffer[BILI_RECORD_MAX] = {};
bili_video_t pending_tracks[BILI_RECORD_MAX] = {};
uint8_t pending_count = 0;
uint32_t pending_generation = 0;
uint32_t applied_generation = 0;
std::string pending_search_name;
std::string current_search_name;
std::atomic<uint32_t> search_generation{0};
std::atomic<Command> pending_command{Command::None};
std::atomic<int> pending_index{-1};
std::atomic<uint8_t> failed_mask{0};
char current_audio_bvid[BILI_BVID_MAX_LEN + 1] = {};
std::atomic<bool> lvgl_task_stop{false};
bool audio_media_applied = false;
bool audio_quiet_applied = false;
bool wifi_ps_applied = false;
wifi_ps_type_t saved_wifi_ps = WIFI_PS_MAX_MODEM;
int last_device_state = -1;

static uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

static emote::EmoteDisplay *get_emote_display()
{
    Display *display = Board::GetInstance().GetDisplay();
    return display ? dynamic_cast<emote::EmoteDisplay *>(display) : nullptr;
}

static bool acquire_display_handles()
{
    s.emote_display = get_emote_display();
    if (!s.emote_display) {
        BILI_LOGE("[DISPLAY] EmoteDisplay unavailable");
        return false;
    }
    s.emote_handle = s.emote_display->GetEmoteHandle();
    if (!s.emote_handle) {
        BILI_LOGE("[DISPLAY] GetEmoteHandle returned NULL");
        return false;
    }
    s.panel = static_cast<esp_lcd_panel_handle_t>(
        emote_get_user_data(s.emote_handle));
    if (!s.panel) {
        BILI_LOGE("[DISPLAY] invalid emote/panel handle");
        return false;
    }
    return true;
}

static void flush_done_isr(void *context)
{
    SemaphoreHandle_t done = static_cast<SemaphoreHandle_t>(context);
    lv_display_t *display = const_cast<lv_display_t *>(s.pending_flush_display);
    s.pending_flush_display = nullptr;
    s.flush_pending = false;
    if (display) lv_display_flush_ready(display);
    if (done) {
        BaseType_t hp = pdFALSE;
        xSemaphoreGiveFromISR(done, &hp);
        if (hp == pdTRUE) portYIELD_FROM_ISR();
    }
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    if (!s.panel || !area || !px_map || !s.flush_done) {
        if (display) lv_display_flush_ready(display);
        return;
    }
    s.pending_flush_display = display;
    s.flush_pending = true;
    const esp_err_t ret = esp_lcd_panel_draw_bitmap(
        s.panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    if (ret != ESP_OK) {
        BILI_LOGE("[FLUSH] draw bitmap failed: %s", esp_err_to_name(ret));
        s.pending_flush_display = nullptr;
        s.flush_pending = false;
        lv_display_flush_ready(display);
    }
}

static void hide_emote()
{
    if (!s.emote_handle || s.emote_hidden) return;
    (void)emote_stop_anim_dialog(s.emote_handle);
    emote_lock(s.emote_handle);
    emote_set_anim_visible(s.emote_handle, false);
    emote_unlock(s.emote_handle);
    s.emote_hidden = true;
}

static void restore_emote()
{
    if (!s.emote_handle || !s.emote_hidden) return;
    emote_lock(s.emote_handle);
    emote_set_anim_visible(s.emote_handle, true);
    emote_unlock(s.emote_handle);
    s.emote_hidden = false;
    if (s.emote_display) s.emote_display->RefreshAll();
}

static bool takeover_display()
{
    if (!s.flush_done) {
        s.flush_done = xSemaphoreCreateBinary();
        if (!s.flush_done) return false;
    }
    while (xSemaphoreTake(s.flush_done, 0) == pdTRUE) { }
    s.emote_display->SetExternalDisplayMode(true);
    if (!s.emote_display->WaitForFlushIdle(1000)) {
        s.emote_display->SetExternalDisplayMode(false);
        return false;
    }
    s.emote_display->SetExternalFlushDoneCallback(flush_done_isr, s.flush_done);
    return true;
}

static void release_display()
{
    if (!s.emote_display) return;
    s.emote_display->SetExternalFlushDoneCallback(nullptr, nullptr);
    s.emote_display->SetExternalDisplayMode(false);
}

static bool init_lvgl_runtime()
{
    if (!lv_is_initialized()) lv_init();
    lv_tick_set_cb([]() -> uint32_t { return now_ms(); });

    s.display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (!s.display) return false;
    lv_display_set_color_format(s.display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(s.display, flush_cb);
    s.draw_buf = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s.draw_buf) return false;
    lv_display_set_buffers(s.display, s.draw_buf, nullptr, DRAW_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(s.display);
    return true;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    TouchSample sample;
    if (s.touch_queue && xQueueReceive(s.touch_queue, &sample, 0) == pdTRUE) s.last_touch = sample;
    data->point.x = s.last_touch.x;
    data->point.y = s.last_touch.y;
    data->state = s.last_touch.state;
}

static bool create_music_ui()
{
    s.root = lv_obj_create(lv_layer_top());
    if (!s.root) return false;
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s.root, 0, 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s.root, lv_color_hex(0x343247), 0);

    vocat_lv_demo_args_t args = { .parent = s.root };
    vocat_lv_demo_music_with_args(&args);
    vocat_lv_demo_music_set_state_callback([](bool playing, uint32_t id) {
        const bili_video_t *track = vocat_lv_demo_music_get_track(id);
        BILI_LOGI("[PLAY_STATE] playing=%d id=%u track=%p bvid=%s running=%d paused=%d failed_mask=0x%02x",
                  playing ? 1 : 0, id, static_cast<const void *>(track),
                  (track && track->bvid[0]) ? track->bvid : "<none>",
                  bilibili_audio_is_running() ? 1 : 0,
                  bilibili_audio_is_paused() ? 1 : 0,
                  static_cast<unsigned>(failed_mask.load(std::memory_order_acquire)));
        if (!playing) {
            bilibili_audio_set_paused(true);
            return;
        }
        if (!track || !track->bvid[0]) {
            BILI_LOGW("[PLAY_STATE] playing requested but track is invalid id=%u", id);
            return;
        }

        if (std::strcmp(current_audio_bvid, track->bvid) == 0 &&
            bilibili_audio_is_running()) {
            BILI_LOGI("[PLAY_STATE] resume existing bvid=%s id=%u", track->bvid, id);
            bilibili_audio_set_paused(false);
            return;
        }

        failed_mask.fetch_and(static_cast<uint8_t>(~(1U << id)));
        std::strncpy(current_audio_bvid, track->bvid, sizeof(current_audio_bvid) - 1);
        current_audio_bvid[sizeof(current_audio_bvid) - 1] = '\0';
        BILI_LOGI("[PLAY_STATE] start audio id=%u bvid=%s", id, track->bvid);
        if (!bilibili_audio_start_ex(track->bvid,
                                     [](void *) {
                                         BILI_LOGI("[PLAY] EOF -> queue next");
                                         pending_command.store(Command::Next, std::memory_order_release);
                                         if (lvgl_task) xTaskNotify(lvgl_task, LVGL_COMMAND_NOTIFY, eSetBits);
                                     },
                                     [](void *arg, int error_code) {
                                         const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
                                         failed_mask.fetch_or(static_cast<uint8_t>(1U << id), std::memory_order_acq_rel);
                                         BILI_LOGW("[PLAY] track=%u failed error=%d -> auto skip mask=0x%02x",
                                                   id, error_code,
                                                   static_cast<unsigned>(failed_mask.load(std::memory_order_acquire)));
                                         pending_command.store(Command::Next, std::memory_order_release);
                                         if (lvgl_task) xTaskNotify(lvgl_task, LVGL_COMMAND_NOTIFY, eSetBits);
                                     },
                                     reinterpret_cast<void *>(static_cast<uintptr_t>(id)))) {
            BILI_LOGW("[PLAY] bilibili_audio_start_ex rejected id=%u", id);
            failed_mask.fetch_or(static_cast<uint8_t>(1U << id), std::memory_order_acq_rel);
            pending_command.store(Command::Next, std::memory_order_release);
        }
    });

    if (!s.touch_queue) s.touch_queue = xQueueCreate(1, sizeof(TouchSample));
    if (!s.touch_queue) return false;
    s.indev = lv_indev_create();
    if (!s.indev) return false;
    lv_indev_set_type(s.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s.indev, touch_read_cb);
    lv_indev_set_display(s.indev, s.display);
    return true;
}

static void destroy_lvgl_runtime()
{
    if (s.root) {
        lv_obj_delete(s.root);
        s.root = nullptr;
    }
    if (s.indev) {
        lv_indev_delete(s.indev);
        s.indev = nullptr;
    }
    if (s.display) {
        lv_display_set_default(nullptr);
        lv_display_delete(s.display);
        s.display = nullptr;
    }
    if (s.draw_buf) {
        heap_caps_free(s.draw_buf);
        s.draw_buf = nullptr;
    }
    if (s.touch_queue) {
        vQueueDelete(s.touch_queue);
        s.touch_queue = nullptr;
    }
    if (s.flush_done) {
        vSemaphoreDelete(s.flush_done);
        s.flush_done = nullptr;
    }
    s.last_touch = {0, 0, LV_INDEV_STATE_RELEASED};
    s.pending_flush_display = nullptr;
    s.flush_pending = false;
    s.runtime_ready = false;
    s.input_ready = false;
}

static void push_pending_tracks(const bili_video_t *videos, uint8_t count, const char *name)
{
    std::lock_guard<std::mutex> lock(pending_mutex);
    pending_count = std::min<uint8_t>(count, BILI_RECORD_MAX);
    std::memset(pending_tracks, 0, sizeof(pending_tracks));
    if (videos && pending_count) {
        std::memcpy(pending_tracks, videos, sizeof(bili_video_t) * pending_count);
    }
    if (name != nullptr) pending_search_name = name;
    ++pending_generation;
}

static bool consume_pending_tracks()
{
    if (!s.active) return false;
    std::lock_guard<std::mutex> lock(pending_mutex);
    if (applied_generation == pending_generation) return false;
    bili_video_t local[BILI_RECORD_MAX] = {};
    const uint8_t count = pending_count;
    std::memcpy(local, pending_tracks, sizeof(local));
    current_search_name = pending_search_name;
    applied_generation = pending_generation;

    vocat_lv_demo_music_pause();
    bilibili_audio_stop();
    std::memset(current_audio_bvid, 0, sizeof(current_audio_bvid));
    failed_mask.store(0);
    vocat_lv_demo_music_set_tracks(local, count);
    vocat_lv_demo_music_refresh_tracks();
    return true;
}

static void process_command()
{
    const Command command = pending_command.exchange(Command::None);
    if (command == Command::None || !s.active) return;

    const uint8_t count = vocat_lv_demo_music_get_track_count();
    if (count == 0) return;

    if (command == Command::Play) {
        const int index = pending_index.exchange(-1);
        if (index >= 0 && index < count) vocat_lv_demo_music_play(static_cast<uint32_t>(index));
        return;
    }
    if (command == Command::Pause) {
        vocat_lv_demo_music_pause();
        bilibili_audio_stop();
        return;
    }
    if (command == Command::Resume) {
        vocat_lv_demo_music_resume();
        return;
    }

    if (command == Command::Previous || command == Command::Next) {
        const bool forward = command == Command::Next;
        const uint8_t mask = failed_mask.load(std::memory_order_acquire);
        if (mask == static_cast<uint8_t>((1U << count) - 1U)) {
            BILI_LOGW("[PLAY] every track failed; stopping instead of looping forever");
            vocat_lv_demo_music_pause();
            bilibili_audio_stop();
            return;
        }

        uint32_t candidate = vocat_lv_demo_music_get_current_id();
        for (uint8_t step = 0; step < count; ++step) {
            candidate = forward ? ((candidate + 1U) % count)
                                : ((candidate == 0U) ? (count - 1U) : (candidate - 1U));
            if ((mask & static_cast<uint8_t>(1U << candidate)) == 0U) {
                vocat_lv_demo_music_play(candidate);
                return;
            }
        }
    }
}

static void apply_audio_policy()
{
    const bool touch_active = s.touch_active.load(std::memory_order_acquire);
    const bool playing = vocat_lv_demo_music_is_playing() &&
                         bilibili_audio_is_running() &&
                         !bilibili_audio_is_paused();
    auto &audio = Application::GetInstance().GetAudioService();

    if (touch_active) {
        if (!audio_quiet_applied) {
            audio.EnableVoiceProcessing(false);
            audio.EnableWakeWordDetection(false);
            audio_quiet_applied = true;
            BILI_LOGI("[AI] touch active -> AI input paused; Bilibili playback unchanged");
        }
    } else if (audio_quiet_applied && !playing) {
        audio_quiet_applied = false;
        last_device_state = -1;
        BILI_LOGI("[AI] touch released -> restore AI policy");
    }

    if (playing) {
        if (!wifi_ps_applied) {
            if (esp_wifi_get_ps(&saved_wifi_ps) != ESP_OK) saved_wifi_ps = WIFI_PS_MAX_MODEM;
            const esp_err_t ps_ret = esp_wifi_set_ps(WIFI_PS_NONE);
            wifi_ps_applied = true;
            BILI_LOGI("[POWER] playback wifi_ps=NONE ret=%s", esp_err_to_name(ps_ret));
        }
        if (!audio_media_applied) {
            audio.SetExternalMediaPlaybackMode(true);
            audio_media_applied = true;
            last_device_state = -1;
            BILI_LOGI("[AUDIO] external media mode ON");
        }
        return;
    }

    if (audio_media_applied) {
        audio.SetExternalMediaPlaybackMode(false);
        audio_media_applied = false;
        if (wifi_ps_applied) {
            const esp_err_t ps_ret = esp_wifi_set_ps(saved_wifi_ps);
            wifi_ps_applied = false;
            BILI_LOGI("[POWER] playback wifi_ps restore ret=%s", esp_err_to_name(ps_ret));
        }
        last_device_state = -1;
        BILI_LOGI("[AUDIO] external media mode OFF");
    }

    if (touch_active) return;

    const int state = static_cast<int>(Application::GetInstance().GetDeviceState());
    if (state != last_device_state) {
        if (state == static_cast<int>(kDeviceStateIdle)) {
            audio.EnableVoiceProcessing(true);
            audio.EnableWakeWordDetection(true);
        } else if (state == static_cast<int>(kDeviceStateListening)) {
            audio.EnableVoiceProcessing(true);
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
#else
            audio.EnableWakeWordDetection(false);
#endif
        } else {
            audio.EnableVoiceProcessing(false);
            audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
        }
        last_device_state = state;
    }
}

static void lvgl_task_entry(void *)
{
    uint32_t policy_deadline = now_ms();
    while (!lvgl_task_stop.load()) {
        uint32_t notify = 0;
        (void)xTaskNotifyWait(0, LVGL_WAKE_NOTIFY | LVGL_COMMAND_NOTIFY | LVGL_DATA_NOTIFY,
                              &notify, pdMS_TO_TICKS(20));
        if (lvgl_task_stop.load()) break;

        const int64_t start = esp_timer_get_time();
        if (s.display) {
            consume_pending_tracks();
            process_command();
            (void)lv_timer_handler();
        }
        if (now_ms() >= policy_deadline) {
            apply_audio_policy();
            policy_deadline = now_ms() + 100;
        }
        const int64_t elapsed = esp_timer_get_time() - start;
        if (elapsed > 50000) BILI_LOGW("[LVGL] handler slow=%lld us", static_cast<long long>(elapsed));
    }
    lvgl_task = nullptr;
    vTaskDelete(nullptr);
}

static bool start_lvgl_task()
{
    lvgl_task_stop.store(false, std::memory_order_release);
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    constexpr BaseType_t LVGL_CORE = 0;
#else
    constexpr BaseType_t LVGL_CORE = 1;
#endif
    const BaseType_t result = xTaskCreatePinnedToCore(
        lvgl_task_entry, "bili_lvgl", 8192, nullptr, 4, &lvgl_task, LVGL_CORE);
    if (result != pdPASS) {
        lvgl_task = nullptr;
        BILI_LOGE("[LVGL] failed to create task on core=%d", static_cast<int>(LVGL_CORE));
        return false;
    }
    BILI_LOGI("[LVGL] task created core=%d", static_cast<int>(LVGL_CORE));
    return true;
}

static void stop_lvgl_task()
{
    if (!lvgl_task) return;
    lvgl_task_stop.store(true);
    xTaskNotify(lvgl_task, LVGL_WAKE_NOTIFY, eSetBits);
    for (int i = 0; i < 100 && lvgl_task != nullptr; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

static void search_task_entry(void *)
{
    BILI_LOGI("[SEARCH] worker started core=%d", xPortGetCoreID());
    search_task_ready.store(true, std::memory_order_release);

    for (;;) {
        uint32_t notify = 0;
        (void)xTaskNotifyWait(0, SEARCH_NOTIFY, &notify, portMAX_DELAY);
        if ((notify & SEARCH_NOTIFY) == 0U) continue;

        BILI_LOGI("[SEARCH] worker woke notified=0x%08lx stack_free=%lu",
                  static_cast<unsigned long>(notify),
                  static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

        char name[sizeof(search_request_name)] = {};
        uint32_t request_id = 0;
        {
            std::lock_guard<std::mutex> lock(search_mutex);
            std::strncpy(name, search_request_name, sizeof(name) - 1);
            request_id = search_request_generation.load(std::memory_order_acquire);
        }

        if (!name[0]) continue;
        BILI_LOGI("[SEARCH] begin name=%s request=%u", name, request_id);

        std::memset(search_result_buffer, 0, sizeof(search_result_buffer));
        const uint8_t count = vocat_bilibili_search_up(
            name, search_result_buffer, BILI_RECORD_MAX);

        if (!s.active.load(std::memory_order_acquire) ||
            request_id != search_request_generation.load(std::memory_order_acquire)) {
            BILI_LOGW("[SEARCH] stale result ignored request=%u count=%u", request_id, count);
            continue;
        }

        push_pending_tracks(search_result_buffer, count, name);
        if (lvgl_task) {
            xTaskNotify(lvgl_task, LVGL_DATA_NOTIFY, eSetBits);
        }
        BILI_LOGI("[SEARCH] completed name=%s count=%u", name, count);
    }
}

static bool ensure_search_task()
{
    if (search_task != nullptr) {
        return true;
    }

    /* Keep exactly one reusable search worker on CPU0.  Do not wait for the
     * worker to report "ready": FreeRTOS task notifications are latched, so a
     * notification sent immediately after creation will still be consumed when
     * the worker first reaches xTaskNotifyWait().  Waiting here could also block
     * the MCP caller and make the first search disappear behind a scheduler
     * timing race. */
    constexpr BaseType_t SEARCH_CORE = 0;
    constexpr uint32_t SEARCH_TASK_STACK = 8192;

    search_task_ready.store(false, std::memory_order_release);
    const BaseType_t result = xTaskCreatePinnedToCore(
        search_task_entry,
        "bili_search",
        SEARCH_TASK_STACK,
        nullptr,
        2,
        &search_task,
        SEARCH_CORE);

    if (result != pdPASS) {
        search_task = nullptr;
        BILI_LOGE("[SEARCH] failed to create worker task on core=%d",
                  static_cast<int>(SEARCH_CORE));
        return false;
    }

    BILI_LOGI("[SEARCH] worker task handle=%p created core=%d",
              static_cast<void *>(search_task),
              static_cast<int>(SEARCH_CORE));
    return true;
}

static bool push_touch(int x, int y, lv_indev_state_t state)
{
    if (!s.touch_queue) return false;
    TouchSample sample{static_cast<int16_t>(x), static_cast<int16_t>(y), state};
    xQueueOverwrite(s.touch_queue, &sample);
    return true;
}

} // namespace

extern "C" bool bilibili_story_open(void)
{
    if (s.active) return true;
    BILI_LOGI("========== BILIBILI LVGL OPEN ==========");

    if (!acquire_display_handles()) return false;
    hide_emote();
    if (!takeover_display()) {
        restore_emote();
        return false;
    }
    vocat_lv_demo_music_set_tracks(nullptr, 0);
    if (!init_lvgl_runtime() || !create_music_ui()) {
        destroy_lvgl_runtime();
        release_display();
        restore_emote();
        return false;
    }

    s.touch_active.store(false, std::memory_order_release);
    s.active = true;
    s.runtime_ready = true;
    s.input_ready = true;
    audio_media_applied = false;
    audio_quiet_applied = false;
    last_device_state = -1;
    current_search_name.clear();
    failed_mask.store(0);
    pending_command.store(Command::None, std::memory_order_release);
    pending_index.store(-1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        applied_generation = pending_generation;
        pending_search_name.clear();
    }
    if (!start_lvgl_task()) {
        s.active = false;
    s.touch_active.store(false, std::memory_order_release);
        destroy_lvgl_runtime();
        release_display();
        restore_emote();
        return false;
    }

    BILI_LOGI("[OPEN] LVGL ready; audio policy remains AI-enabled while idle");
    return true;
}

extern "C" void bilibili_story_close(void)
{
    if (!s.active) return;
    BILI_LOGI("========== BILIBILI LVGL CLOSE ==========");
    s.active = false;
    pending_command.store(Command::None, std::memory_order_release);
    pending_index.store(-1, std::memory_order_release);
    search_generation.fetch_add(1);
    search_request_generation.fetch_add(1);
    bilibili_audio_stop();
    stop_lvgl_task();

    auto &audio = Application::GetInstance().GetAudioService();
    audio.SetExternalMediaPlaybackMode(false);
    audio_media_applied = false;
    if (wifi_ps_applied) {
        (void)esp_wifi_set_ps(saved_wifi_ps);
        wifi_ps_applied = false;
    }
    audio_quiet_applied = false;
    last_device_state = -1;
    audio.EnableVoiceProcessing(true);
    audio.EnableWakeWordDetection(true);

    if (s.flush_pending && s.flush_done) {
        (void)xSemaphoreTake(s.flush_done, pdMS_TO_TICKS(1000));
        s.flush_pending = false;
        s.pending_flush_display = nullptr;
    }
    vocat_lv_demo_music_set_state_callback(nullptr);
    destroy_lvgl_runtime();
    release_display();
    restore_emote();
    std::memset(current_audio_bvid, 0, sizeof(current_audio_bvid));
}

extern "C" bool bilibili_story_is_active(void) { return s.active; }

extern "C" void bilibili_story_search(const char *up_name)
{
    if (!up_name || !up_name[0]) return;
    if (!s.active.load(std::memory_order_acquire) && !bilibili_story_open()) return;
    if (!ensure_search_task()) return;

    const uint32_t request_id = search_request_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(search_mutex);
        std::strncpy(search_request_name, up_name, sizeof(search_request_name) - 1);
        search_request_name[sizeof(search_request_name) - 1] = '\0';
    }

    BILI_LOGI("[SEARCH] request queued name=%s request=%u worker=%p",
              up_name, request_id, static_cast<void *>(search_task));
    const BaseType_t notify_ret = xTaskNotify(search_task, SEARCH_NOTIFY, eSetBits);
    if (notify_ret != pdPASS) {
        BILI_LOGE("[SEARCH] xTaskNotify failed ret=%ld worker=%p",
                  static_cast<long>(notify_ret), static_cast<void *>(search_task));
    }
}

extern "C" void bilibili_story_show_list(const bili_video_t *videos, uint8_t count)
{
    if (!s.active && !bilibili_story_open()) return;
    push_pending_tracks(videos, count, nullptr);
    if (lvgl_task) xTaskNotify(lvgl_task, LVGL_DATA_NOTIFY, eSetBits);
}

extern "C" void bilibili_story_show_player(uint8_t index)
{
    pending_index.store(index, std::memory_order_release);
    pending_command.store(Command::Play, std::memory_order_release);
    if (lvgl_task) xTaskNotify(lvgl_task, LVGL_COMMAND_NOTIFY, eSetBits);
}

extern "C" void bilibili_story_set_playing(bool playing)
{
    pending_command.store(playing ? Command::Resume : Command::Pause, std::memory_order_release);
    if (lvgl_task) xTaskNotify(lvgl_task, LVGL_COMMAND_NOTIFY, eSetBits);
}

extern "C" void bilibili_story_set_track(uint8_t index) { bilibili_story_show_player(index); }
extern "C" void bilibili_story_previous(void)
{
    pending_command.store(Command::Previous, std::memory_order_release);
    if (lvgl_task) xTaskNotify(lvgl_task, LVGL_COMMAND_NOTIFY, eSetBits);
}
extern "C" void bilibili_story_next(void)
{
    pending_command.store(Command::Next, std::memory_order_release);
    if (lvgl_task) xTaskNotify(lvgl_task, LVGL_COMMAND_NOTIFY, eSetBits);
}
extern "C" void bilibili_story_back(void) { bilibili_story_close(); }

extern "C" bool bilibili_story_handle_touch(int x, int y)
{
    return vocat_bilibili_ui_handle_touch_event(x, y, 0);
}

extern "C" bool vocat_bilibili_ui_handle_touch_event(int x, int y, int event)
{
    if (!s.active) return false;

    const bool pressed = (event == 1 || event == 2);
    const bool previous = s.touch_active.exchange(pressed, std::memory_order_acq_rel);

    // This only controls the music demo's touch visual state and the AI input
    // policy. It never calls bilibili_audio_set_paused().
    vocat_lv_demo_music_set_touch_active(pressed);
    push_touch(x, y, pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED);

    if (previous != pressed && lvgl_task) {
        (void)xTaskNotify(lvgl_task, LVGL_WAKE_NOTIFY, eSetBits);
    }
    return true;
}

extern "C" bool bilibili_story_handle_swipe(int, int, int, int)
{
    return s.active;
}
extern "C" void vocat_bilibili_render_screen_async(void) { (void)bilibili_story_open(); }
extern "C" bool vocat_bilibili_render_screen(void) { return bilibili_story_open(); }
extern "C" void vocat_bilibili_ui_clear(void) { bilibili_story_close(); }
extern "C" void vocat_bilibili_ui_draw(const bili_video_t *videos, uint8_t count)
{
    bilibili_story_show_list(videos, count);
}
extern "C" bool vocat_bilibili_ui_handle_touch(int x, int y)
{
    return bilibili_story_handle_touch(x, y);
}
extern "C" bool vocat_bilibili_ui_is_active(void) { return bilibili_story_is_active(); }

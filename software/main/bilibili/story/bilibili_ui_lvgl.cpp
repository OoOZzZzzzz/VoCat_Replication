#include "bilibili_ui.h"
#include "application.h"

#include <lvgl.h>

#include <inttypes.h>
#include <atomic>

/* VoCat-local LVGL Music Player demo. */
extern "C" {
#include "music/lv_demo_music.h"
}

#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board.h"
#include "display/emote_display.h"
#include "expression_emote.h"

namespace {

constexpr char TAG[] = "BILI_STORY_LVGL";
constexpr uint32_t LCD_WIDTH = 360;
constexpr uint32_t LCD_HEIGHT = 360;
constexpr uint32_t DRAW_BUF_LINES = 30;
constexpr uint32_t DRAW_BUF_BYTES = LCD_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t);

struct State {
    bool active = false;
    bool lvgl_initialized_here = false;
    bool emote_hidden = false;

    SemaphoreHandle_t flush_done = nullptr;
    emote::EmoteDisplay* emote_display = nullptr;
    esp_lcd_panel_io_handle_t panel_io = nullptr;

    lv_display_t* display = nullptr;
    lv_obj_t* root = nullptr;
    lv_indev_t* indev = nullptr;
    struct TouchSample {
        int16_t x;
        int16_t y;
        lv_indev_state_t state;
    };

    QueueHandle_t touch_queue = nullptr;
    TouchSample last_touch = {0, 0, LV_INDEV_STATE_RELEASED};

    esp_lcd_panel_handle_t panel = nullptr;
    emote_handle_t emote_handle = nullptr;

    void* draw_buf_1 = nullptr;
    void* draw_buf_2 = nullptr;

    volatile lv_display_t* pending_flush_display = nullptr;
    volatile bool flush_pending = false;
};

State s;
SemaphoreHandle_t lvgl_mutex = nullptr;
static TaskHandle_t lvgl_task = nullptr;
static TaskHandle_t audio_policy_task = nullptr;
static volatile bool lvgl_task_stop = false;
static volatile bool audio_policy_task_stop = false;
static std::atomic_bool touch_is_down(false);
static std::atomic<uint32_t> flush_count(0);
static std::atomic<uint32_t> flush_last_us(0);
static std::atomic<uint32_t> flush_max_us(0);
static std::atomic<int64_t> flush_start_us(0);

constexpr uint32_t AUDIO_NOTIFY_TOUCH_DOWN = 1U << 0;
constexpr uint32_t AUDIO_NOTIFY_TOUCH_UP = 1U << 1;
constexpr uint32_t AUDIO_NOTIFY_WAKE = 1U << 2;

extern "C" bool vocat_lvgl_music_is_playing(void);

constexpr uint32_t TOUCH_AUDIO_QUIET_MS = 1000;
#if CONFIG_FREERTOS_UNICORE
constexpr BaseType_t LVGL_CORE = 0;
constexpr BaseType_t AUDIO_POLICY_CORE = 0;
#else
constexpr BaseType_t LVGL_CORE = 1;
constexpr BaseType_t AUDIO_POLICY_CORE = 0;
#endif

static std::atomic<uint32_t> touch_audio_quiet_until_ms{0};
static bool audio_quiet_applied = false;
static bool external_media_mode_applied = false;

static void log_state(const char* stage)
{
    Display* base = Board::GetInstance().GetDisplay();
    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(base);

    ESP_LOGI(
        TAG,
        "[%s] base=%p emote=%p handle=%p panel=%p lvgl_init=%d display=%p active=%d mutex=%p",
        stage,
        static_cast<void*>(base),
        static_cast<void*>(emote_display),
        static_cast<void*>(s.emote_handle),
        static_cast<void*>(s.panel),
        lv_is_initialized() ? 1 : 0,
        static_cast<void*>(s.display),
        s.active ? 1 : 0,
        static_cast<void*>(lvgl_mutex)
    );
}

static bool ensure_mutex()
{
    if (lvgl_mutex != nullptr) {
        return true;
    }

    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == nullptr) {
        ESP_LOGE(TAG, "[LOCK] xSemaphoreCreateMutex failed");
        return false;
    }

    ESP_LOGI(TAG, "[LOCK] mutex created: %p", static_cast<void*>(lvgl_mutex));
    return true;
}

static bool lock_lvgl(TickType_t timeout = pdMS_TO_TICKS(3000))
{
    if (!ensure_mutex()) {
        return false;
    }

    BaseType_t ret = xSemaphoreTake(lvgl_mutex, timeout);

    if (ret != pdTRUE && timeout != 0) {
        ESP_LOGW(TAG, "[LOCK] take timeout/fail mutex=%p timeout=%lu ms",
                 static_cast<void*>(lvgl_mutex),
                 static_cast<unsigned long>(pdTICKS_TO_MS(timeout)));
    }

    return ret == pdTRUE;
}

static void unlock_lvgl()
{
    if (lvgl_mutex != nullptr) {
        xSemaphoreGive(lvgl_mutex);
    }
}

static emote::EmoteDisplay* get_emote_display()
{
    Display* display = Board::GetInstance().GetDisplay();

    if (display == nullptr) {
        ESP_LOGE(TAG, "[DISPLAY] Board::GetDisplay() returned NULL");
        return nullptr;
    }

    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);

    if (emote_display == nullptr) {
        ESP_LOGE(TAG,
                 "[DISPLAY] current Display is not EmoteDisplay: %p",
                 static_cast<void*>(display));
        return nullptr;
    }

    ESP_LOGI(TAG, "[DISPLAY] EmoteDisplay=%p", static_cast<void*>(emote_display));
    return emote_display;
}

static bool get_panel_from_emote()
{
    auto* display = get_emote_display();
    if (display == nullptr) {
        return false;
    }

    s.emote_display = display;
    s.panel_io = display->GetPanelIo();

    ESP_LOGI(TAG, "[LCD] panel_io=%p", static_cast<void*>(s.panel_io));

    if (s.panel_io == nullptr) {
        ESP_LOGE(TAG, "[LCD] panel_io from EmoteDisplay is NULL");
        return false;
    }

    emote_handle_t handle = display->GetEmoteHandle();
    s.emote_handle = handle;

    ESP_LOGI(TAG, "[EMOTE] handle=%p initialized=%d",
             static_cast<void*>(handle),
             handle != nullptr && emote_is_initialized(handle) ? 1 : 0);

    if (handle == nullptr) {
        ESP_LOGE(TAG, "[EMOTE] GetEmoteHandle() returned NULL");
        return false;
    }

    void* user_data = emote_get_user_data(handle);

    ESP_LOGI(TAG, "[EMOTE] user_data=%p", user_data);

    s.panel = static_cast<esp_lcd_panel_handle_t>(user_data);

    if (s.panel == nullptr) {
        ESP_LOGE(TAG, "[LCD] panel handle from Emote user_data is NULL");
        return false;
    }

    ESP_LOGI(TAG, "[LCD] panel=%p", static_cast<void*>(s.panel));
    return true;
}

static void lvgl_flush_done_from_isr(void* context);

static uint32_t lvgl_tick_get_cb()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

static bool take_over_emote_display()
{
    if (s.emote_display == nullptr) {
        ESP_LOGE(TAG, "[EMOTE] takeover failed: display is NULL");
        return false;
    }

    if (s.flush_done == nullptr) {
        s.flush_done = xSemaphoreCreateBinary();
        if (s.flush_done == nullptr) {
            ESP_LOGE(TAG, "[LVGL] xSemaphoreCreateBinary failed");
            return false;
        }
        ESP_LOGI(TAG, "[LVGL] flush semaphore created=%p", static_cast<void*>(s.flush_done));
    }

    while (xSemaphoreTake(s.flush_done, 0) == pdTRUE) {
    }

    ESP_LOGI(TAG, "[EMOTE] enabling external display mode");
    s.emote_display->SetExternalDisplayMode(true);

    if (!s.emote_display->WaitForFlushIdle(1000)) {
        ESP_LOGE(TAG, "[EMOTE] pending flushes did not become idle");
        s.emote_display->SetExternalDisplayMode(false);
        return false;
    }

    s.emote_display->SetExternalFlushDoneCallback(
        lvgl_flush_done_from_isr,
        static_cast<void*>(s.flush_done)
    );

    ESP_LOGI(TAG, "[EMOTE] LCD ownership handed to LVGL");
    return true;
}

static void release_emote_display()
{
    if (s.emote_display == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "[EMOTE] clearing LVGL flush callback");
    s.emote_display->SetExternalFlushDoneCallback(nullptr, nullptr);
    s.emote_display->SetExternalDisplayMode(false);
    ESP_LOGI(TAG, "[EMOTE] LCD ownership returned to Emote");
}

static void hide_emote_ui()
{
    if (s.emote_handle == nullptr) {
        ESP_LOGW(TAG, "[EMOTE] hide skipped: handle is NULL");
        return;
    }

    ESP_LOGI(TAG, "[EMOTE] stopping dialog animation");
    bool stopped = emote_stop_anim_dialog(s.emote_handle);
    ESP_LOGI(TAG, "[EMOTE] emote_stop_anim_dialog -> %d", stopped ? 1 : 0);

    ESP_LOGI(TAG, "[EMOTE] locking manager for visibility change");
    emote_lock(s.emote_handle);
    emote_set_anim_visible(s.emote_handle, false);
    emote_unlock(s.emote_handle);

    s.emote_hidden = true;
    ESP_LOGI(TAG, "[EMOTE] face visibility=false");
}

static void lvgl_flush_done_from_isr(void* context)
{
    SemaphoreHandle_t semaphore = static_cast<SemaphoreHandle_t>(context);

    lv_display_t* display = const_cast<lv_display_t*>(s.pending_flush_display);
    s.pending_flush_display = nullptr;
    s.flush_pending = false;

    const int64_t start_us = flush_start_us.load(std::memory_order_relaxed);
    if(start_us > 0) {
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - start_us);
        flush_last_us.store(elapsed, std::memory_order_relaxed);
        uint32_t max_us = flush_max_us.load(std::memory_order_relaxed);
        while(elapsed > max_us &&
              !flush_max_us.compare_exchange_weak(max_us, elapsed, std::memory_order_relaxed)) {
        }
        flush_count.fetch_add(1, std::memory_order_relaxed);
    }

    if (display != nullptr) {
        lv_display_flush_ready(display);
    }

    if (semaphore == nullptr) {
        return;
    }

    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(
        semaphore,
        &higher_priority_task_woken
    );

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void restore_emote_ui()
{
    if (!s.emote_hidden || s.emote_handle == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "[EMOTE] restoring face visibility=true");

    emote_lock(s.emote_handle);
    emote_set_anim_visible(s.emote_handle, true);
    emote_unlock(s.emote_handle);

    s.emote_hidden = false;

    auto* display = dynamic_cast<emote::EmoteDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        display->RefreshAll();
    }
}

static void flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map)
{
    if (s.panel == nullptr || area == nullptr || px_map == nullptr) {
        ESP_LOGE(TAG, "[FLUSH] invalid args panel=%p area=%p buffer=%p",
                 static_cast<void*>(s.panel),
                 static_cast<const void*>(area),
                 static_cast<void*>(px_map));
        lv_display_flush_ready(display);
        return;
    }

    const int32_t x1 = area->x1;
    const int32_t y1 = area->y1;
    const int32_t x2 = area->x2 + 1;
    const int32_t y2 = area->y2 + 1;

    const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
    const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
    const uint32_t pixels = width * height;

    LV_UNUSED(width);
    LV_UNUSED(height);
    LV_UNUSED(pixels);

    if (s.flush_done == nullptr) {
        ESP_LOGE(TAG, "[FLUSH] completion semaphore is NULL");
        lv_display_flush_ready(display);
        return;
    }

    while (xSemaphoreTake(s.flush_done, 0) == pdTRUE) {
    }

    s.pending_flush_display = display;
    s.flush_pending = true;
    flush_start_us.store(esp_timer_get_time(), std::memory_order_relaxed);

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s.panel,
        x1,
        y1,
        x2,
        y2,
        px_map
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[FLUSH] esp_lcd_panel_draw_bitmap failed: %s (0x%x)",
                 esp_err_to_name(err), static_cast<unsigned>(err));
        s.pending_flush_display = nullptr;
        s.flush_pending = false;
        lv_display_flush_ready(display);
        return;
    }

    /* DMA completion callback calls lv_display_flush_ready(). */
}

static bool init_lvgl_runtime()
{
    log_state("init-enter");

    if (!get_panel_from_emote()) {
        ESP_LOGE(TAG, "[INIT] failed to obtain LCD panel");
        return false;
    }

    ESP_LOGI(TAG, "[LVGL] compile version=%d.%d.%d",
             LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH);

    ESP_LOGI(TAG, "[LVGL] initialized before call=%d",
             lv_is_initialized() ? 1 : 0);

    if (!lv_is_initialized()) {
        ESP_LOGI(TAG, "[LVGL] calling lv_init()");
        lv_init();
        s.lvgl_initialized_here = true;
    }

    ESP_LOGI(TAG, "[LVGL] initialized after call=%d",
             lv_is_initialized() ? 1 : 0);

    lv_tick_set_cb(lvgl_tick_get_cb);

    s.display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (s.display == nullptr) {
        ESP_LOGE(TAG, "[LVGL] lv_display_create failed");
        return false;
    }

    ESP_LOGI(TAG, "[LVGL] display created=%p %lux%lu",
             static_cast<void*>(s.display),
             static_cast<unsigned long>(LCD_WIDTH),
             static_cast<unsigned long>(LCD_HEIGHT));

    // The Emote display is configured for byte-swapped RGB565 over SPI.
    // LVGL 9.5 supports the swapped color format natively, so no temporary
    // buffer mutation is needed in flush_cb.
    lv_display_set_color_format(s.display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(s.display, flush_cb);

    s.draw_buf_1 = heap_caps_malloc(
        DRAW_BUF_BYTES,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
    );
    // Use one DMA buffer. The flush callback waits for the LCD color
    // transfer to finish, so a second buffer is not required here.

    ESP_LOGI(TAG, "[BUF] buf1=%p buf2=%p bytes=%lu",
             s.draw_buf_1,
             s.draw_buf_2,
             static_cast<unsigned long>(DRAW_BUF_BYTES));

    if (s.draw_buf_1 == nullptr) {
        ESP_LOGE(TAG, "[BUF] draw buffer allocation failed");

        if (s.draw_buf_1 != nullptr) {
            heap_caps_free(s.draw_buf_1);
            s.draw_buf_1 = nullptr;
        }
        if (s.draw_buf_2 != nullptr) {
            heap_caps_free(s.draw_buf_2);
            s.draw_buf_2 = nullptr;
        }
        lv_display_delete(s.display);
        s.display = nullptr;
        // Music UI objects are not created before this point.
        return false;
    }

    lv_display_set_buffers(
        s.display,
        s.draw_buf_1,
        nullptr,
        DRAW_BUF_BYTES,
        LV_DISPLAY_RENDER_MODE_PARTIAL
    );

    lv_display_set_default(s.display);

    ESP_LOGI(TAG, "[LVGL] display configured successfully");
    log_state("init-done");
    return true;
}

static void music_touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    LV_UNUSED(indev);

    State::TouchSample sample;
    if (s.touch_queue != nullptr && xQueueReceive(s.touch_queue, &sample, 0) == pdTRUE) {
        s.last_touch = sample;
    }

    data->point.x = s.last_touch.x;
    data->point.y = s.last_touch.y;
    data->state = s.last_touch.state;
}

static void create_music_input()
{
    if (s.indev != nullptr) {
        return;
    }

    if (s.touch_queue == nullptr) {
        s.touch_queue = xQueueCreate(1, sizeof(State::TouchSample));
        if (s.touch_queue == nullptr) {
            ESP_LOGE(TAG, "[INPUT] xQueueCreate failed");
            return;
        }
    }

    s.indev = lv_indev_create();
    if (s.indev == nullptr) {
        ESP_LOGE(TAG, "[INPUT] lv_indev_create failed");
        return;
    }

    lv_indev_set_type(s.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s.indev, music_touch_read_cb);
    lv_indev_set_display(s.indev, s.display);

    ESP_LOGI(TAG, "[INPUT] local Music Player pointer input created=%p",
             static_cast<void*>(s.indev));
}

static void create_music_ui()
{
    ESP_LOGI(TAG, "[MUSIC] creating local LVGL 9.5 Music Player demo");

    s.root = lv_obj_create(lv_layer_top());
    if (s.root == nullptr) {
        ESP_LOGE(TAG, "[MUSIC] failed to create root");
        return;
    }

    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_center(s.root);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s.root, lv_color_hex(0x343247), 0);
    lv_obj_set_style_radius(s.root, 0, 0);
    lv_obj_set_style_clip_corner(s.root, false, 0);

    vocat_lv_demo_args_t args = {
        .parent = s.root,
    };

    vocat_lv_demo_music_with_args(&args);
    create_music_input();

    ESP_LOGI(TAG, "[MUSIC] local Music Player demo created root=%p",
             static_cast<void*>(s.root));
}

static uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

static void restore_audio_after_quiet()
{
    auto& audio = Application::GetInstance().GetAudioService();
    auto state = Application::GetInstance().GetDeviceState();

    if (external_media_mode_applied) {
        audio.SetExternalMediaPlaybackMode(false);
        external_media_mode_applied = false;
    }

    if (!audio_quiet_applied) {
        return;
    }

    audio_quiet_applied = false;

    if (state == kDeviceStateIdle) {
        audio.EnableVoiceProcessing(false);
        audio.EnableWakeWordDetection(true);
    } else if (state == kDeviceStateListening) {
        audio.EnableVoiceProcessing(true);
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
        audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
#else
        audio.EnableWakeWordDetection(false);
#endif
    } else if (state == kDeviceStateSpeaking) {
        audio.EnableVoiceProcessing(false);
        audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
    }
}

static void apply_audio_policy()
{
    if (!s.active) {
        return;
    }

    const uint32_t now = now_ms();
    const bool music_playing = vocat_lvgl_music_is_playing();
    const uint32_t touch_quiet_until = touch_audio_quiet_until_ms.load(std::memory_order_acquire);
    const bool touch_busy = touch_is_down.load(std::memory_order_acquire) ||
                            static_cast<int32_t>(touch_quiet_until - now) > 0;

    /*
     * Music owns the audio path. While a user is pausing/stopping music by
     * touch, keep external-media mode until the touch quiet window expires;
     * this avoids a rapid external-media -> AFE -> external-media sequence.
     */
    if (music_playing) {
        if (!external_media_mode_applied) {
            ESP_LOGI(TAG, "[AUDIO] music active -> disabling AI audio pipeline");
            const int64_t t0 = esp_timer_get_time();
            Application::GetInstance().GetAudioService().SetExternalMediaPlaybackMode(true);
            ESP_LOGI(TAG, "[AUDIO] external media ON took=%lld us", static_cast<long long>(esp_timer_get_time() - t0));
            external_media_mode_applied = true;
            audio_quiet_applied = false;
        }
        return;
    }

    if (external_media_mode_applied) {
        if (touch_busy) {
            return;
        }

        ESP_LOGI(TAG, "[AUDIO] music stopped and touch idle -> leaving external media mode");
        const int64_t t0 = esp_timer_get_time();
        restore_audio_after_quiet();
        ESP_LOGI(TAG, "[AUDIO] external media OFF/AFE restore took=%lld us", static_cast<long long>(esp_timer_get_time() - t0));
        return;
    }

    if (touch_busy) {
        if (!audio_quiet_applied) {
            ESP_LOGI(TAG, "[AUDIO] touch active -> temporarily disabling AI audio pipeline");
            auto& audio = Application::GetInstance().GetAudioService();
            const int64_t t0 = esp_timer_get_time();
            audio.EnableVoiceProcessing(false);
            audio.EnableWakeWordDetection(false);
            ESP_LOGI(TAG, "[AUDIO] AI audio OFF took=%lld us", static_cast<long long>(esp_timer_get_time() - t0));
            audio_quiet_applied = true;
        }
    }
    else if (audio_quiet_applied) {
        ESP_LOGI(TAG, "[AUDIO] touch idle -> restoring AI audio pipeline");
        const int64_t t0 = esp_timer_get_time();
        restore_audio_after_quiet();
        ESP_LOGI(TAG, "[AUDIO] AI audio restore took=%lld us", static_cast<long long>(esp_timer_get_time() - t0));
    }
}

static void audio_policy_task_entry(void* arg)
{
    LV_UNUSED(arg);
    ESP_LOGI(TAG, "[AUDIO] policy task started core=%d priority=%lu stack_free=%lu",
             xPortGetCoreID(),
             static_cast<unsigned long>(uxTaskPriorityGet(nullptr)),
             static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    /* Reconcile AI state once at startup: no touch + no music means the normal
     * Xiaozhi audio pipeline remains enabled. */
    apply_audio_policy();

    while (!audio_policy_task_stop) {
        uint32_t notified = 0;
        (void)xTaskNotifyWait(
            0,
            AUDIO_NOTIFY_TOUCH_DOWN | AUDIO_NOTIFY_TOUCH_UP | AUDIO_NOTIFY_WAKE,
            &notified,
            pdMS_TO_TICKS(50)
        );

        if((notified & AUDIO_NOTIFY_TOUCH_DOWN) != 0U ||
           (notified & AUDIO_NOTIFY_TOUCH_UP) != 0U) {
            touch_audio_quiet_until_ms.store(
                now_ms() + TOUCH_AUDIO_QUIET_MS,
                std::memory_order_release);
        }

        apply_audio_policy();
    }

    ESP_LOGI(TAG, "[AUDIO] policy task stopped");
    audio_policy_task = nullptr;
    vTaskDelete(nullptr);
}

static bool start_audio_policy_task()
{
    audio_policy_task_stop = false;

    if (xTaskCreatePinnedToCore(
            audio_policy_task_entry,
            "bili_audio_policy",
            4096,
            nullptr,
            2,
            &audio_policy_task,
            AUDIO_POLICY_CORE) != pdPASS) {
        audio_policy_task = nullptr;
        ESP_LOGE(TAG, "[AUDIO] failed to create policy task");
        return false;
    }

    return true;
}

static void stop_audio_policy_task()
{
    if (audio_policy_task == nullptr) {
        return;
    }

    audio_policy_task_stop = true;
    (void)xTaskNotify(audio_policy_task, AUDIO_NOTIFY_WAKE, eSetBits);
    for (int i = 0; i < 100 && audio_policy_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void lvgl_task_entry(void* arg)
{
    LV_UNUSED(arg);
    ESP_LOGI(TAG, "[LVGL] UI task started core=%d priority=%lu stack_free=%lu",
             xPortGetCoreID(),
             static_cast<unsigned long>(uxTaskPriorityGet(nullptr)),
             static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    uint32_t slow_frames = 0;
    uint32_t frame_count = 0;
    uint64_t frame_sum_us = 0;
    uint32_t frame_max_us = 0;
    uint32_t flush_count_prev = 0;

    while (!lvgl_task_stop) {
        uint32_t delay_ms = 10;
        if (lock_lvgl(0)) {
            if (!lvgl_task_stop && s.display != nullptr) {
                const int64_t start_us = esp_timer_get_time();
                delay_ms = lv_timer_handler();
                const int64_t elapsed_us = esp_timer_get_time() - start_us;

                ++frame_count;
                frame_sum_us += static_cast<uint64_t>(elapsed_us);
                if(static_cast<uint32_t>(elapsed_us) > frame_max_us) {
                    frame_max_us = static_cast<uint32_t>(elapsed_us);
                }

                if (elapsed_us > 30000) {
                    ++slow_frames;
                    ESP_LOGW(TAG,
                             "[LVGL] slow frame=%" PRIi64 "us count=%lu touch=%d music=%d flush_last=%" PRIu32 "us",
                             elapsed_us,
                             static_cast<unsigned long>(slow_frames),
                             touch_is_down.load(std::memory_order_relaxed) ? 1 : 0,
                             vocat_lvgl_music_is_playing() ? 1 : 0,
                             flush_last_us.load(std::memory_order_relaxed));
                }

                if ((frame_count % 60U) == 0U) {
                    const uint32_t flush_now = flush_count.load(std::memory_order_relaxed);
                    ESP_LOGI(TAG,
                             "[LVGL][PERF] avg=%" PRIu64 "us max=%" PRIu32 "us frames=60 flushes=%" PRIu32 " flush_max=%" PRIu32 "us",
                             frame_sum_us / 60U,
                             frame_max_us,
                             flush_now - flush_count_prev,
                             flush_max_us.load(std::memory_order_relaxed));
                    frame_sum_us = 0;
                    frame_max_us = 0;
                    flush_count_prev = flush_now;
                }

                if (delay_ms < 5) delay_ms = 5;
                if (delay_ms > 10) delay_ms = 10;
            }
            unlock_lvgl();
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESP_LOGI(TAG, "[LVGL] UI task stopped");
    lvgl_task = nullptr;
    vTaskDelete(nullptr);
}

static bool start_lvgl_task()
{
    lvgl_task_stop = false;
    if (xTaskCreatePinnedToCore(
            lvgl_task_entry,
            "bili_lvgl",
            8192,
            nullptr,
            4,
            &lvgl_task,
            LVGL_CORE) != pdPASS) {
        lvgl_task = nullptr;
        ESP_LOGE(TAG, "[LVGL] failed to create UI task");
        return false;
    }
    return true;
}

static void stop_lvgl_task()
{
    if (lvgl_task == nullptr) return;

    lvgl_task_stop = true;
    for (int i = 0; i < 100 && lvgl_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool clear_panel_before_emote()
{
    if (s.panel == nullptr) {
        ESP_LOGE(TAG, "[CLOSE] panel is NULL, cannot clear LCD");
        return false;
    }

    constexpr uint32_t CLEAR_LINES = 20;
    constexpr size_t CLEAR_BYTES = LCD_WIDTH * CLEAR_LINES * sizeof(uint16_t);
    static uint16_t clear_buffer[LCD_WIDTH * CLEAR_LINES] = {};

    ESP_LOGI(TAG, "[CLOSE] clearing whole LCD before returning ownership");

    for (uint32_t y = 0; y < LCD_HEIGHT; y += CLEAR_LINES) {
        const uint32_t y_end = (y + CLEAR_LINES > LCD_HEIGHT) ? LCD_HEIGHT : y + CLEAR_LINES;
        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            s.panel,
            0,
            y,
            LCD_WIDTH,
            y_end,
            clear_buffer
        );

        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "[CLOSE] clear failed y=%lu-%lu err=%s (0x%x)",
                     static_cast<unsigned long>(y),
                     static_cast<unsigned long>(y_end),
                     esp_err_to_name(err),
                     static_cast<unsigned>(err));
            return false;
        }

        if (s.flush_done == nullptr ||
            xSemaphoreTake(s.flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG,
                     "[CLOSE] timeout waiting for LCD DMA y=%lu-%lu",
                     static_cast<unsigned long>(y),
                     static_cast<unsigned long>(y_end));
            return false;
        }
    }

    ESP_LOGI(TAG, "[CLOSE] whole LCD cleared (%lu bytes/segment)",
             static_cast<unsigned long>(CLEAR_BYTES));
    return true;
}

static void destroy_lvgl_runtime()
{
    ESP_LOGI(TAG, "[DESTROY] begin");

    if (s.root != nullptr) {
        ESP_LOGI(TAG, "[DESTROY] deleting root=%p", static_cast<void*>(s.root));
        lv_obj_delete(s.root);
        s.root = nullptr;
    }

    if (s.indev != nullptr) {
        ESP_LOGI(TAG, "[DESTROY] deleting input=%p", static_cast<void*>(s.indev));
        lv_indev_delete(s.indev);
        s.indev = nullptr;
    }

    if (s.display != nullptr) {
        ESP_LOGI(TAG, "[DESTROY] deleting display=%p", static_cast<void*>(s.display));
        lv_display_set_default(nullptr);
        lv_display_delete(s.display);
        s.display = nullptr;
    }

    if (s.draw_buf_1 != nullptr) {
        heap_caps_free(s.draw_buf_1);
        s.draw_buf_1 = nullptr;
    }

    if (s.draw_buf_2 != nullptr) {
        heap_caps_free(s.draw_buf_2);
        s.draw_buf_2 = nullptr;
    }

    if (s.touch_queue != nullptr) {
        vQueueDelete(s.touch_queue);
        s.touch_queue = nullptr;
    }

    s.panel = nullptr;
    s.emote_handle = nullptr;
    s.last_touch = {0, 0, LV_INDEV_STATE_RELEASED};
    s.pending_flush_display = nullptr;
    s.flush_pending = false;
    s.lvgl_initialized_here = false;

    ESP_LOGI(TAG, "[DESTROY] done");
}

static bool push_touch_event(int x, int y, lv_indev_state_t state)
{
    if (!s.active && state != LV_INDEV_STATE_PRESSED) {
        return false;
    }

    if (s.touch_queue == nullptr) {
        return false;
    }

    State::TouchSample sample = {
        static_cast<int16_t>(x),
        static_cast<int16_t>(y),
        state
    };

    /* The queue has length 1 on purpose: LVGL must always consume the newest
     * pointer position instead of replaying stale drag samples. */
    xQueueOverwrite(s.touch_queue, &sample);
    return true;
}

}  // namespace

extern "C" bool bilibili_story_open(void)
{
    ESP_LOGI(TAG, "========== BILIBILI LVGL OPEN ==========");
    log_state("open-enter");

    if (s.active) {
        ESP_LOGW(TAG, "[OPEN] already active");
        return true;
    }

    // IMPORTANT: create the mutex before taking it. The previous test build
    // attempted to take a still-null mutex on the first open.
    if (!ensure_mutex()) {
        ESP_LOGE(TAG, "[OPEN] ensure_mutex failed");
        return false;
    }

    if (!lock_lvgl()) {
        ESP_LOGE(TAG, "[OPEN] lock failed");
        return false;
    }

    bool ok = false;

    do {
        if (!get_panel_from_emote()) {
            ESP_LOGE(TAG, "[OPEN] failed to obtain Emote display/panel");
            break;
        }

        hide_emote_ui();

        if (!take_over_emote_display()) {
            ESP_LOGE(TAG, "[OPEN] failed to take over LCD from Emote");
            restore_emote_ui();
            break;
        }

        if (!init_lvgl_runtime()) {
            ESP_LOGE(TAG, "[OPEN] init_lvgl_runtime failed");
            release_emote_display();
            restore_emote_ui();
            break;
        }

        create_music_ui();

        if (s.root == nullptr || s.indev == nullptr) {
            ESP_LOGE(TAG, "[OPEN] music UI creation failed");
            destroy_lvgl_runtime();
            release_emote_display();
            restore_emote_ui();
            break;
        }

        s.active = true;
        touch_is_down.store(false, std::memory_order_release);
        vocat_lv_demo_music_set_touch_active(false);
        touch_audio_quiet_until_ms.store(0, std::memory_order_release);
        audio_quiet_applied = false;
        external_media_mode_applied = false;
        flush_count.store(0, std::memory_order_relaxed);
        flush_last_us.store(0, std::memory_order_relaxed);
        flush_max_us.store(0, std::memory_order_relaxed);
        flush_start_us.store(0, std::memory_order_relaxed);

        if (!start_lvgl_task()) {
            ESP_LOGE(TAG, "[OPEN] LVGL UI task start failed");
            s.active = false;
            destroy_lvgl_runtime();
            release_emote_display();
            restore_emote_ui();
            break;
        }

        if (!start_audio_policy_task()) {
            ESP_LOGE(TAG, "[OPEN] audio policy task start failed");
            s.active = false;
            stop_lvgl_task();
            destroy_lvgl_runtime();
            release_emote_display();
            restore_emote_ui();
            break;
        }

        ESP_LOGI(TAG, "[OPEN] task split: LVGL core=%d priority=4, audio policy core=%d priority=2",
                 static_cast<int>(LVGL_CORE),
                 static_cast<int>(AUDIO_POLICY_CORE));
        ok = true;
    } while (false);

    log_state(ok ? "open-success" : "open-failed");
    unlock_lvgl();

    ESP_LOGI(TAG, "========== BILIBILI LVGL OPEN RESULT=%s ==========",
             ok ? "SUCCESS" : "FAIL");

    return ok;
}

extern "C" void bilibili_story_close(void)
{
    ESP_LOGI(TAG, "========== BILIBILI LVGL CLOSE ==========");

    if (!ensure_mutex()) {
        ESP_LOGE(TAG, "[CLOSE] ensure_mutex failed");
        return;
    }

    if (!lock_lvgl()) {
        ESP_LOGE(TAG, "[CLOSE] lock failed");
        return;
    }

    s.active = false;

    stop_audio_policy_task();
    stop_lvgl_task();

    if (external_media_mode_applied) {
        Application::GetInstance().GetAudioService().SetExternalMediaPlaybackMode(false);
        external_media_mode_applied = false;
    }
    if (audio_quiet_applied) {
        Application::GetInstance().GetAudioService().EnableVoiceProcessing(false);
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
        audio_quiet_applied = false;
    }
    touch_is_down.store(false, std::memory_order_release);
    touch_audio_quiet_until_ms.store(0, std::memory_order_release);

    ESP_LOGI(TAG, "[CLOSE] clearing LVGL pixels before returning LCD ownership");
    (void)clear_panel_before_emote();

    destroy_lvgl_runtime();
    release_emote_display();
    restore_emote_ui();
    unlock_lvgl();

    log_state("close-done");
}

extern "C" bool bilibili_story_is_active(void)
{
    return s.active;
}

extern "C" void bilibili_story_search(const char*) {}
extern "C" void bilibili_story_show_list(const bili_video_t*, uint8_t) {}
extern "C" void bilibili_story_show_player(uint8_t) {}
extern "C" void bilibili_story_set_playing(bool) {}
extern "C" void bilibili_story_set_track(uint8_t) {}
extern "C" void bilibili_story_previous(void) {}
extern "C" void bilibili_story_next(void) {}

extern "C" void bilibili_story_back(void)
{
    bilibili_story_close();
}

extern "C" bool bilibili_story_handle_touch(int x, int y)
{
    if (!s.active) {
        return false;
    }

    touch_is_down.store(false, std::memory_order_release);
    vocat_lv_demo_music_set_touch_active(false);
    touch_audio_quiet_until_ms.store(
        now_ms() + TOUCH_AUDIO_QUIET_MS,
        std::memory_order_release);
    if(audio_policy_task != nullptr) {
        (void)xTaskNotify(audio_policy_task, AUDIO_NOTIFY_TOUCH_UP, eSetBits);
    }
    (void)push_touch_event(x, y, LV_INDEV_STATE_RELEASED);

    /* Keep all touch events inside the LVGL Music Player while Bilibili is active. */
    return true;
}

extern "C" bool vocat_bilibili_ui_handle_touch_event(int x, int y, int event)
{
    if (!s.active) {
        return false;
    }

    lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
    if (event == 1 || event == 2) {
        state = LV_INDEV_STATE_PRESSED;
        touch_is_down.store(true, std::memory_order_release);
        vocat_lv_demo_music_set_touch_active(true);
        if(audio_policy_task != nullptr) {
            (void)xTaskNotify(audio_policy_task, AUDIO_NOTIFY_TOUCH_DOWN, eSetBits);
        }
    }
    else {
        touch_is_down.store(false, std::memory_order_release);
        vocat_lv_demo_music_set_touch_active(false);
        touch_audio_quiet_until_ms.store(
            now_ms() + TOUCH_AUDIO_QUIET_MS,
            std::memory_order_release);
        if(audio_policy_task != nullptr) {
            (void)xTaskNotify(audio_policy_task, AUDIO_NOTIFY_TOUCH_UP, eSetBits);
        }
    }

    (void)push_touch_event(x, y, state);
    return true;
}

extern "C" bool bilibili_story_handle_swipe(int start_x, int start_y, int end_x, int end_y)
{
    LV_UNUSED(start_x);
    LV_UNUSED(start_y);
    LV_UNUSED(end_x);
    LV_UNUSED(end_y);

    if (!s.active) {
        return false;
    }

    /* The local Music Player demo owns its gesture handling inside LVGL. */
    return true;
}

extern "C" void vocat_bilibili_render_screen_async(void)
{
    (void)bilibili_story_open();
}

extern "C" bool vocat_bilibili_render_screen(void)
{
    return bilibili_story_open();
}

extern "C" void vocat_bilibili_ui_clear(void)
{
    bilibili_story_close();
}

extern "C" void vocat_bilibili_ui_draw(const bili_video_t*, uint8_t)
{
    if (!s.active) {
        (void)bilibili_story_open();
    }
}

extern "C" bool vocat_bilibili_ui_handle_touch(int x, int y)
{
    return bilibili_story_handle_touch(x, y);
}

extern "C" bool vocat_bilibili_ui_is_active(void)
{
    return bilibili_story_is_active();
}

#include "emote_display.h"

// Standard C++ headers
#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <cinttypes>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// Project headers
#include "assets/lang_config.h"
#include "assets.h"
#include "board.h"
#include "gfx.h"
#include "expression_emote.h"


namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";
// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================
static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* const edata, void* user_ctx)
{
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    if (handle && EmoteDisplay::GetInstance()) {
        EmoteDisplay::GetInstance()->NotifyFlushDone();
    }
    return true;
}
// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data, emote_handle_t handle)
{
    if (EmoteDisplay::GetInstance() && EmoteDisplay::GetInstance()->IsExternalDisplayMode()) {
        emote_notify_flush_finished(handle);
        return;
    }

    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel != nullptr) {
        if (EmoteDisplay::GetInstance()) {
            EmoteDisplay::GetInstance()->MarkFlushPending();
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
        if (err != ESP_OK && EmoteDisplay::GetInstance()) {
            EmoteDisplay::GetInstance()->NotifyFlushDone();
        }
    }
}
// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width, const int height)
{
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }
    emote_config_t emote_cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = width,
            .v_res = height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = false,
        },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };
    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

static EmoteDisplay* g_emote_display_instance = nullptr;

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height)
{
    emote_handle_ = InitializeEmote(panel, width, height);
    panel_io_ = panel_io;
    g_emote_display_instance = this;

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}
EmoteDisplay::~EmoteDisplay()
{
    external_display_mode_.store(false);
    external_flush_done_callback_.store(nullptr);
    external_flush_done_context_.store(nullptr);

    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
    if (g_emote_display_instance == this) {
        g_emote_display_instance = nullptr;
    }
}
void EmoteDisplay::SetExternalDisplayMode(bool enabled)
{
    ESP_LOGI(TAG, "SetExternalDisplayMode: %s", enabled ? "ON" : "OFF");
    external_display_mode_.store(enabled);
}
void EmoteDisplay::SetExternalFlushDoneCallback(ExternalFlushDoneCallback callback, void* context)
{
    external_flush_done_context_.store(context);
    external_flush_done_callback_.store(callback);
}
bool EmoteDisplay::WaitForFlushIdle(int timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (pending_flushes_.load() != 0) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "WaitForFlushIdle timeout, pending=%" PRIu32, pending_flushes_.load());
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}
void EmoteDisplay::MarkFlushPending()
{
    pending_flushes_.fetch_add(1);
}
void EmoteDisplay::NotifyFlushDone()
{
    uint32_t pending = pending_flushes_.load();
    while (pending != 0 &&
           !pending_flushes_.compare_exchange_weak(pending, pending - 1)) {
    }

    ExternalFlushDoneCallback callback = external_flush_done_callback_.load();
    void* context = external_flush_done_context_.load();
    if (callback != nullptr) {
        callback(context);
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (emote_handle_ && emotion && strlen(emotion) > 0) {
        emote_set_anim_emoji(emote_handle_, emotion);
    }
}
void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (emote_handle_ && content && strlen(content) > 0) {
        if ((std::strcmp(role, "system") == 0) && std::strstr(content, "xiaozhi.me")) {
            size_t len = strlen(content);
            char* new_content = new char[len + 1];
            strcpy(new_content, content);
            std::replace(new_content, new_content + len, static_cast<char>(0x0A), static_cast<char>(0x20));
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, new_content);
            delete[] new_content;
        } else {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, content);
        }
    }
}
void EmoteDisplay::SetStatus(const char* const status)
{
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (emote_handle_ && status && strlen(status) > 0) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, NULL);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
        } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, NULL);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SET, NULL);
        }
    }
}
void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (emote_handle_ && notification && strlen(notification) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
    }
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
}
void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);
}
bool EmoteDisplay::Lock(const int timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock()
{
}

bool EmoteDisplay::StopAnimDialog()
{
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_);
    }
    return false;
}
bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms);
    }
    return false;
}

void EmoteDisplay::RefreshAll()
{
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

EmoteDisplay* EmoteDisplay::GetInstance()
{
    return g_emote_display_instance;
}

} // namespace emote

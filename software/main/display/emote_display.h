#pragma once
#include "display.h"
#include <memory>
#include <string>
#include <atomic>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    using ExternalFlushDoneCallback = void (*)(void* context);

    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);
    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);
    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }
    esp_lcd_panel_io_handle_t GetPanelIo() const { return panel_io_; }

    // Temporarily hand the LCD flush path to an external renderer such as LVGL.
    static EmoteDisplay* GetInstance();
    void SetExternalDisplayMode(bool enabled);
    bool IsExternalDisplayMode() const { return external_display_mode_.load(); }
    void SetExternalFlushDoneCallback(ExternalFlushDoneCallback callback, void* context);
    bool WaitForFlushIdle(int timeout_ms = 1000);
    void MarkFlushPending();
    void NotifyFlushDone();

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    emote_handle_t emote_handle_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    std::atomic_bool external_display_mode_ = false;
    std::atomic<uint32_t> pending_flushes_ = 0;
    std::atomic<ExternalFlushDoneCallback> external_flush_done_callback_ = nullptr;
    std::atomic<void*> external_flush_done_context_ = nullptr;
};

} // namespace emote

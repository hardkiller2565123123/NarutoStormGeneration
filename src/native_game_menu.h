#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include <rex/input/input.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::graphics {
class GraphicsSystem;
}

namespace rex::ui {
class ImGuiDrawer;
class KeyEvent;
class Window;
}

struct ImGuiIO;
struct ImVec2;

namespace generations {

// Controller-first launcher rendered by ReXGlue's Dear ImGui host overlay.
// Start + Back toggles it; gameplay input is blocked while it is visible.
class NativeGameMenu final : public rex::ui::ImGuiDialog {
 public:
  NativeGameMenu(rex::ui::ImGuiDrawer* imgui_drawer, rex::ui::Window* window,
                 rex::graphics::GraphicsSystem* graphics_system,
                 std::filesystem::path config_path,
                 std::filesystem::path user_data_root,
                 std::function<void()> request_open_on_ui_thread,
                 std::function<void(bool)> request_fullscreen_on_ui_thread);
  ~NativeGameMenu();

  NativeGameMenu(const NativeGameMenu&) = delete;
  NativeGameMenu& operator=(const NativeGameMenu&) = delete;

  void AttachPersistentSurfaceIfNeeded() {}
  void DetachFromUIThread();
  void OpenFromUIThread();
  void ApplyFullscreenOnUIThread(bool enabled);

  bool HandleControllerKeystroke(uint32_t user_index,
                                 rex::input::X_INPUT_KEYSTROKE& keystroke);
  void HandleControllerState(uint32_t user_index,
                             rex::input::X_INPUT_STATE& state);
  void HandleKeyDown(rex::ui::KeyEvent& event);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class PendingAction : uint8_t {
    kNone,
    kUp,
    kDown,
    kLeft,
    kRight,
    kConfirm,
    kClose,
    kPreviousTab,
    kNextTab,
  };

  enum class Tab : uint8_t {
    kDisplay,
    kGraphics,
    kPerformance,
    kControls,
    kAudio,
    kGame,
    kInterface,
    kRuntime,
    kDeveloper,
    kAbout,
  };

  void QueueAction(PendingAction action);
  void ProcessPendingAction();
  void ChangeSelectedValue(int direction);
  void OpenSelectedDropdown();
  void CommitDropdownSelection();
  void ApplyOptionValue(Tab tab, size_t row, const char* value);
  void CycleTab(int direction);
  void CloseFromUIThread();

  size_t RowCount(Tab tab) const;
  std::string RowLabel(Tab tab, size_t row) const;
  std::string RowValue(Tab tab, size_t row) const;
  const char* RowState(Tab tab, size_t row) const;
  const char* RowDescription(Tab tab, size_t row) const;

  void ApplyRecommendedProfile();
  void ApplySmooth30Profile();
  void ApplyNvidiaProfile();
  void ApplyAmdProfile();
  void ApplyScalerProfile(int profile);
  void SetSwapPostEffect(const char* value);
  void SetFullscreen(bool enabled);
  void SaveSettings(const char* status);
  void CreateSaveBackup();

  void ApplyImGuiStyle();
  void DrawFpsOverlay();
  void DrawLauncher(ImGuiIO& io);
  void DrawHeader(const ImVec2& display_size);
  void DrawTabs();
  void DrawOptionsPanel();
  void DrawDetailsPanel();
  void DrawDropdownOverlay();
  void DrawFooter();

  bool IsVisible() const { return visible_.load(std::memory_order_acquire); }
  bool IsFpsCounterEnabled() const;

  rex::ui::Window* window_ = nullptr;
  rex::graphics::GraphicsSystem* graphics_system_ = nullptr;
  std::filesystem::path config_path_;
  std::filesystem::path user_data_root_;
  std::function<void()> request_open_on_ui_thread_;
  std::function<void(bool)> request_fullscreen_on_ui_thread_;

  std::atomic<bool> visible_{false};
  std::atomic<bool> open_enqueued_{false};
  std::atomic<bool> menu_combo_latched_{false};
  std::atomic<uint16_t> previous_buttons_{0};
  std::atomic<uint8_t> pending_action_{static_cast<uint8_t>(PendingAction::kNone)};

  Tab active_tab_ = Tab::kGraphics;
  std::array<size_t, 10> selected_rows_{};
  bool dropdown_open_ = false;
  size_t dropdown_row_ = 0;
  size_t dropdown_index_ = 0;
  size_t scaler_profile_index_ = 0;
  size_t performance_profile_index_ = 0;
  std::array<float, 120> fps_history_{};
  size_t fps_history_cursor_ = 0;
  std::string status_ = "READY";
  bool status_is_warning_ = false;
  bool style_applied_ = false;
  float animation_progress_ = 0.0f;
  float tab_transition_ = 0.0f;
  float selection_pulse_ = 0.0f;
};

}  // namespace generations

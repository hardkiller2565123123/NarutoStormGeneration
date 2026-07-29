#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  // Called after all physical input drivers have been merged and before the
  // state is returned to the guest. Applications can use this for a native
  // overlay without polling a second controller API or leaking overlay input
  // into the title.
  using StateTransformCallback = std::function<void(uint32_t, X_INPUT_STATE&)>;
  // Allows native overlays to consume button events from titles that use the
  // XInput keystroke API for menus rather than polling controller state.
  using KeystrokeTransformCallback = std::function<bool(uint32_t, X_INPUT_KEYSTROKE&)>;

  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);
  void SetStateTransformCallback(StateTransformCallback callback);
  void SetKeystrokeTransformCallback(KeystrokeTransformCallback callback);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

 private:
  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;
  std::mutex state_transform_callback_mutex_;
  StateTransformCallback state_transform_callback_;
  std::mutex keystroke_transform_callback_mutex_;
  KeystrokeTransformCallback keystroke_transform_callback_;
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input

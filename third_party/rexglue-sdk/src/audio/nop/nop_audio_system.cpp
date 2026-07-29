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

#include <rex/audio/flags.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/nop/nop_audio_system.h>

namespace rex::audio::nop {

namespace {

class NopAudioDriver final : public AudioDriver {
 public:
  explicit NopAudioDriver(memory::Memory* memory) : AudioDriver(memory) {}

  void SubmitFrame(uint32_t /*samples_ptr*/) override {}
};

}  // namespace

std::unique_ptr<AudioSystem> NopAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<NopAudioSystem>(function_dispatcher);
}

NopAudioSystem::NopAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

NopAudioSystem::~NopAudioSystem() = default;

X_STATUS NopAudioSystem::CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  assert_not_null(semaphore);
  assert_not_null(out_driver);
  *out_driver = new NopAudioDriver(memory_);
  return X_STATUS_SUCCESS;
}

void NopAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  delete driver;
}

}  // namespace rex::audio::nop

#include "generations_app.h"
#include "native_game_menu.h"
#include "native_options_patch.h"
#include "profile_setup_dialog.h"
#include "launcher.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <fstream>

#include <toml++/toml.hpp>

#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(generations_preserve_original_timing, true,
                    "Generations Recomp",
                    "Keep title simulation and animation on the original guest clock");
REXCVAR_DEFINE_BOOL(generations_shadow_compatibility, true,
                    "Generations Recomp",
                    "Enable GPU query and memexport paths used by title effects");
REXCVAR_DEFINE_BOOL(generations_mods_enabled, true, "Generations Recomp",
                    "Enable the local mods directory");
REXCVAR_DEFINE_BOOL(generations_replay_autosave, true, "Generations Recomp",
                    "Save title replay data to the local replay library");
REXCVAR_DEFINE_BOOL(generations_achievement_toasts, true,
                    "Generations Recomp",
                    "Show native host achievement notifications");
REXCVAR_DEFINE_BOOL(generations_fast_media_verification, true,
                    "Generations Recomp",
                    "Skip the title's redundant media verification wait");
REXCVAR_DEFINE_BOOL(generations_legacy_overlay, false,
                    "Generations Recomp",
                    "Enable the deprecated in-game settings overlay");

namespace {

bool IsHiddenDebugRun() {
  const char *value = std::getenv("STORM_RECOMP_DEBUG_HIDDEN");
  return value && *value && std::string_view(value) != "0";
}

bool ShouldOpenNativeMenuForHiddenTest() {
  const char *value = std::getenv("STORM_RECOMP_DEBUG_OPEN_MENU");
  return value && *value && std::string_view(value) != "0";
}

bool ShouldAutopilotHiddenTest() {
  const char *value = std::getenv("STORM_RECOMP_DEBUG_AUTOPILOT");
  return value && *value && std::string_view(value) != "0";
}

std::filesystem::path PackageGameDirectory() {
  const auto executable_directory = rex::filesystem::GetExecutableFolder();
  const auto launcher_config = executable_directory / "launcher.toml";
  if (std::filesystem::is_regular_file(launcher_config)) {
    try {
      const auto table = toml::parse_file(launcher_config.string());
      if (const auto imported = table["imported_game_path"].value<std::string>()) {
        const std::filesystem::path selected = std::filesystem::u8path(*imported);
        if (std::filesystem::is_regular_file(selected / "default.xex")) {
          return selected;
        }
      }
    } catch (...) {
      REXLOG_WARN("Could not parse launcher.toml imported game path");
    }
  }
  if (std::filesystem::is_regular_file(executable_directory / "default.xex")) {
    return executable_directory;
  }
  return executable_directory / "game";
}

} // namespace

GenerationsApp::~GenerationsApp() = default;

bool GenerationsApp::SetupEnvironment() {
  if (!generations::launcher::RunLauncher()) {
    return false;
  }
  return rex::ReXApp::SetupEnvironment();
}

void GenerationsApp::OnPreSetup(rex::RuntimeConfig &config) {
  if (rex::cvar::GetFlagByName("generations_mods_enabled") != "false") {
    const auto loose_mods =
        rex::filesystem::GetExecutableFolder() / "mods" / "loose";
    std::error_code error;
    std::filesystem::create_directories(loose_mods, error);
    if (!error) {
      rex::cvar::SetFlagByName("game_mods_root", loose_mods.string());
    } else {
      REXLOG_WARN("Could not create loose mod directory '{}': {}",
                  loose_mods.string(), error.message());
    }
  }

  // Persist compiled shaders and keep cache misses off the guest thread. Old
  // menu builds saved both flags as false, causing long, hardware-dependent
  // stalls at the title's shader-heavy loading checkpoints.
  if (rex::cvar::GetFlagByName("store_shaders") != "true") {
    rex::cvar::SetFlagByName("store_shaders", "true");
    REXLOG_INFO("Enabled persistent shader storage for Generations");
  }
  if (rex::cvar::GetFlagByName("async_shader_compilation") != "true") {
    rex::cvar::SetFlagByName("async_shader_compilation", "true");
    REXLOG_INFO("Enabled asynchronous shader compilation for Generations");
  }

  // Repeated battles stream many character, stage, portrait, and effect
  // textures. Keep GPU-written memory state coherent and give the cache enough
  // headroom to avoid character previews disappearing after several matches.
  // These are forced here so older launcher configs cannot restore unstable
  // values such as clear_memory_page_state=false or very small cache limits.
  rex::cvar::SetFlagByName("clear_memory_page_state", "true");
  rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", "768");
  rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", "1536");
  rex::cvar::SetFlagByName("texture_cache_memory_limit_render_to_texture", "64");
  rex::cvar::SetFlagByName("texture_cache_memory_limit_soft_lifetime", "180");
  rex::cvar::SetFlagByName("d3d12_pipeline_creation_threads", "4");
  REXLOG_INFO(
      "Applied Generations repeated-match texture stability profile "
      "(soft=768MB hard=1536MB lifetime=180s)");

  // The host render-target path displays black for this title on the tested
  // NVIDIA hardware. Use the working ROV path unless a player selected a
  // different path explicitly via config or command line.
  if (rex::cvar::GetFlagByName("render_target_path_d3d12").empty()) {
    rex::cvar::SetFlagByName("render_target_path_d3d12", "rov");
    REXLOG_INFO("Using D3D12 ROV render target path for Generations");
  }

  // The title's animation and simulation are authored around the Xbox guest
  // clock. Host presentation is configurable separately, but the guest timing
  // must stay throttled or menus, battles, and cutscenes advance too quickly.
  if (rex::cvar::GetFlagByName("generations_preserve_original_timing") !=
      "false") {
    rex::cvar::SetFlagByName("vsync", "true");
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "60");
  }

  // Generations uses query results and guest-memory exports for effects that
  // include scene visibility and shadow-related passes. Keep these coherent as
  // one compatibility group so a launcher preset cannot accidentally enable
  // only half of the path.
  if (rex::cvar::GetFlagByName("generations_shadow_compatibility") !=
      "false") {
    rex::cvar::SetFlagByName("occlusion_query_enable", "true");
    rex::cvar::SetFlagByName("readback_memexport", "true");
    rex::cvar::SetFlagByName("readback_memexport_fast", "true");
    rex::cvar::SetFlagByName("half_pixel_offset", "true");
    rex::cvar::SetFlagByName(
        "resolve_resolution_scale_fill_half_pixel_offset", "true");
  }

  if (!IsHiddenDebugRun()) {
    return;
  }

  config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
  REXLOG_INFO(
      "Hidden diagnostic mode: native window hidden and audio output disabled");
}

void GenerationsApp::OnConfigurePaths(rex::PathConfig &paths) {
  const auto executable_directory = rex::filesystem::GetExecutableFolder();
  if (paths.game_data_root.empty()) {
    // Release builds support both package layouts: a game directory beside the
    // executable, or an executable placed directly beside default.xex.
    paths.game_data_root = PackageGameDirectory();
  }

  // Keep every profile and title-created save beside the executable. The
  // content manager appends <XUID>/<title>/<content type> below this root, so
  // saves from Documents or a copied retail profile can never leak in.
  paths.user_data_root = executable_directory / "saves";

  // Generations asks for UPDATE: during its title and menu boot path even
  // when a separate title update is not installed. Mount the owned game-data
  // directory there read-only so those lookups resolve without changing the
  // original files.
  if (paths.update_data_root.empty()) {
    paths.update_data_root = paths.game_data_root;
  }
}

std::optional<rex::PathConfig>
GenerationsApp::OnFinalizePaths(const rex::PathConfig &defaults,
                                std::function<void(rex::PathConfig)> resume) {
  rex::PathConfig paths = defaults;
  auto identity = generations::LoadProfileIdentity(paths.user_data_root);
  if (!identity.has_value()) {
    identity = generations::CreateProfileIdentity(paths.user_data_root);
  }

  // Never block startup behind a profile popup. Create/repair the local
  // identity immediately so every machine reaches the title and system-link
  // menu with its own persistent XUID.
  std::string error;
  if (!generations::SaveAndActivateProfileIdentity(paths.user_data_root,
                                                   *identity, &error)) {
    REXLOG_ERROR("Player profile activation failed: {}", error);
    return std::nullopt;
  }
  return paths;
}

void GenerationsApp::OnPostSetup() {
  generations::InstallNativeOptionsPatch(game_data_root());

  const bool hidden_overlay_test =
      IsHiddenDebugRun() && ShouldOpenNativeMenuForHiddenTest();
  const bool hidden_autopilot_test =
      IsHiddenDebugRun() && ShouldAutopilotHiddenTest();
  if (rex::cvar::GetFlagByName("generations_legacy_overlay") != "true" &&
      !hidden_overlay_test) {
    if (hidden_autopilot_test) {
      if (auto *input_system = static_cast<rex::input::InputSystem *>(
              runtime()->input_system())) {
        const auto started = std::chrono::steady_clock::now();
        input_system->SetStateTransformCallback(
            [started](uint32_t user_index, rex::input::X_INPUT_STATE &state) {
              if (user_index != 0) {
                return;
              }
              const auto elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
              const auto phase = elapsed % 2000;
              if (phase < 160) {
                state.gamepad.buttons =
                    uint16_t(state.gamepad.buttons) |
                    rex::input::X_INPUT_GAMEPAD_START;
              } else if (phase >= 500 && phase < 660) {
                state.gamepad.buttons = uint16_t(state.gamepad.buttons) |
                                        rex::input::X_INPUT_GAMEPAD_A;
              }
            });
        REXLOG_INFO(
            "Hidden diagnostic autopilot enabled for first-save testing");
      }
    }
    REXLOG_INFO("Dedicated launcher active; legacy in-game settings overlay "
                "is disabled");
    return;
  }

  auto *graphics_system = static_cast<rex::graphics::GraphicsSystem *>(
      runtime()->graphics_system());
  if (!graphics_system || !imgui_drawer()) {
    REXLOG_WARN("ImGui launcher is unavailable because presentation was not "
                "initialized");
    return;
  }

  native_game_menu_ = std::make_unique<generations::NativeGameMenu>(
      imgui_drawer(), window(), graphics_system, config_path(),
      user_data_root(),
      [this]() {
        app_context().CallInUIThreadDeferred([this]() {
          if (native_game_menu_) {
            native_game_menu_->OpenFromUIThread();
          }
        });
      },
      [this](bool enabled) {
        app_context().CallInUIThreadDeferred([this, enabled]() {
          if (native_game_menu_) {
            native_game_menu_->ApplyFullscreenOnUIThread(enabled);
          }
        });
      });
  native_game_menu_->AttachPersistentSurfaceIfNeeded();
  REXLOG_INFO("Controller launcher initialized through the ReXGlue Dear ImGui "
              "host overlay");

  // This exists only for automated hidden smoke runs. Release launches use
  // the controller Start+Back chord and never open the launcher by themselves.
  if (IsHiddenDebugRun() && ShouldOpenNativeMenuForHiddenTest()) {
    rex::input::X_INPUT_STATE state = {};
    state.gamepad.buttons =
        rex::input::X_INPUT_GAMEPAD_START | rex::input::X_INPUT_GAMEPAD_BACK;
    native_game_menu_->HandleControllerState(0, state);
    REXLOG_INFO(
        "Hidden diagnostic run exercised the Start+Back launcher entry");
  }

  if (auto *input_system =
          static_cast<rex::input::InputSystem *>(runtime()->input_system())) {
    input_system->SetStateTransformCallback(
        [this](uint32_t user_index, rex::input::X_INPUT_STATE &state) {
          if (native_game_menu_) {
            native_game_menu_->HandleControllerState(user_index, state);
          }
        });
    input_system->SetKeystrokeTransformCallback(
        [this](uint32_t user_index, rex::input::X_INPUT_KEYSTROKE &keystroke) {
          return native_game_menu_ &&
                 native_game_menu_->HandleControllerKeystroke(user_index,
                                                              keystroke);
        });
  }
}

void GenerationsApp::OnShutdown() {
  if (runtime()) {
    if (auto *input_system =
            static_cast<rex::input::InputSystem *>(runtime()->input_system())) {
      input_system->SetStateTransformCallback({});
      input_system->SetKeystrokeTransformCallback({});
    }
  }
  if (native_game_menu_) {
    native_game_menu_->DetachFromUIThread();
    native_game_menu_.reset();
  }
}

void GenerationsApp::OnKeyDown(rex::ui::KeyEvent &event) {
  if (native_game_menu_) {
    native_game_menu_->HandleKeyDown(event);
  }
}

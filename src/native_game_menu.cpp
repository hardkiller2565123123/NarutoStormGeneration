#include "native_game_menu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_BOOL(generations_show_game_fps, false, "Generations Recomp", "Show guest FPS")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_dim_background, false, "Generations Recomp", "Dim game behind menu")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_show_status_bar, true, "Generations Recomp", "Show status bar")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_high_contrast, false, "Generations Recomp", "High contrast selection")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_animations, true, "Generations Recomp", "Enable menu animations")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_compact_tabs, false, "Generations Recomp", "Compact navigation tabs")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_large_text, true, "Generations Recomp", "Larger menu text")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_tooltips, true, "Generations Recomp", "Show detailed descriptions")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_show_graph, true, "Generations Recomp", "Show performance graph")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(generations_menu_reduce_motion, false, "Generations Recomp", "Reduce motion effects")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace generations {
namespace {

bool IsEnabled(std::string_view name) {
  const std::string value = rex::cvar::GetFlagByName(name);
  return value == "true" || value == "1" || value == "yes";
}

bool SetEnabled(std::string_view name, bool enabled) {
  return rex::cvar::SetFlagByName(name, enabled ? "true" : "false");
}

enum class OptionType : uint8_t { kAction, kBool, kCycle };
enum class Action : uint8_t {
  kNone, kResume, kRecommended, kSmooth30, kNvidia, kAmd, kScalerLowCost,
  kScalerBalanced, kScalerQuality, kScalerUltra, kFullscreen, kBackup,
  kSaveConfig, kResetInterface, kInfo30Fps, kClose
};

struct OptionSpec {
  const char* label;
  const char* description;
  const char* state;
  OptionType type;
  const char* cvar;
  std::vector<const char*> values;
  Action action = Action::kNone;
  const char* success = "SETTING UPDATED";
};

#define ACT(label, desc, action) OptionSpec{label, desc, "ACTION", OptionType::kAction, nullptr, {}, action, "ACTION COMPLETE"}
#define BOOL_LIVE(label, desc, cvar) OptionSpec{label, desc, "LIVE", OptionType::kBool, cvar, {}, Action::kNone, "SETTING APPLIED"}
#define BOOL_RESTART(label, desc, cvar) OptionSpec{label, desc, "RESTART", OptionType::kBool, cvar, {}, Action::kNone, "SAVED - RESTART REQUIRED"}
#define CYCLE_LIVE(label, desc, cvar, ...) OptionSpec{label, desc, "LIVE", OptionType::kCycle, cvar, {__VA_ARGS__}, Action::kNone, "SETTING APPLIED"}
#define CYCLE_RESTART(label, desc, cvar, ...) OptionSpec{label, desc, "RESTART", OptionType::kCycle, cvar, {__VA_ARGS__}, Action::kNone, "SAVED - RESTART REQUIRED"}

const std::vector<OptionSpec>& OptionsFor(int tab) {
  static const std::vector<OptionSpec> display = {
      BOOL_RESTART("PRESENT VSYNC", "Synchronize D3D12 presentation to the display.", "present_vsync"),
      BOOL_LIVE("GPU VSYNC", "Synchronize the guest command processor presentation path.", "vsync"),
      BOOL_RESTART("VARIABLE REFRESH / TEARING", "Allow VRR and tearing on supported displays.", "d3d12_allow_variable_refresh_rate_and_tearing"),
      BOOL_LIVE("LETTERBOX", "Preserve aspect ratio using letterboxing.", "present_letterbox"),
      BOOL_RESTART("ALLOW OVERSCAN CUTOFF", "Allow safe-area settings to crop the output.", "present_allow_overscan_cutoff"),
      CYCLE_RESTART("SAFE AREA X", "Horizontal protected screen percentage.", "present_safe_area_x", "70", "75", "80", "85", "90", "95", "100"),
      CYCLE_RESTART("SAFE AREA Y", "Vertical protected screen percentage.", "present_safe_area_y", "70", "75", "80", "85", "90", "95", "100"),
      ACT("FULLSCREEN", "Toggle the host window fullscreen state.", Action::kFullscreen),
      CYCLE_RESTART("MONITOR", "Display index selected on next launch.", "monitor", "0", "1", "2", "3", "4", "5"),
      CYCLE_RESTART("OUTPUT RESOLUTION", "Host output resolution preset.", "resolution", "", "720p", "1080p", "1440p", "4k"),
      CYCLE_RESTART("WINDOW WIDTH", "Windowed-mode width on next launch.", "window_width", "0", "1280", "1600", "1920", "2560", "3840"),
      CYCLE_RESTART("WINDOW HEIGHT", "Windowed-mode height on next launch.", "window_height", "0", "720", "900", "1080", "1440", "2160"),
      CYCLE_RESTART("HOST REFRESH RATE", "Requested video mode refresh rate.", "video_mode_refresh_rate", "30", "50", "60", "75", "90", "120", "144", "165", "240"),
      BOOL_RESTART("CLEAR PRESENT PASS", "Clear the present render pass before drawing output.", "present_render_pass_clear"),
  };
  static const std::vector<OptionSpec> graphics = {
      CYCLE_RESTART("SCALER PRESET", "Choose a complete internal-scaling profile from one dropdown.", "__scaler_profile", "custom", "low_cost", "balanced", "quality", "ultra"),
      CYCLE_RESTART("PRESENT UPSCALER", "Final guest-output scaling method.", "present_effect", "bilinear", "cas", "fsr", "fsr2", "fsr3"),
      CYCLE_RESTART("CAS SHARPNESS", "Additional CAS sharpening strength.", "present_cas_additional_sharpness", "0", "0.2", "0.4", "0.6", "0.8", "1"),
      CYCLE_RESTART("FSR QUALITY", "Temporal FSR quality preset when available.", "present_fsr_quality_mode", "auto", "nativeaa", "quality", "balanced", "performance", "ultra_performance"),
      CYCLE_RESTART("FSR PASSES", "Maximum chained spatial FSR upsampling passes.", "present_fsr_max_upsampling_passes", "1", "2", "3", "4"),
      CYCLE_RESTART("FSR SHARPNESS REDUCTION", "RCAS sharpness reduction in stops.", "present_fsr_sharpness_reduction", "0", "0.25", "0.5", "0.75", "1", "1.5", "2"),
      BOOL_RESTART("OUTPUT DITHER", "Dither the final output to reduce banding.", "present_dither"),
      CYCLE_LIVE("POST AA", "Swap-chain post anti-aliasing effect.", "swap_post_effect", "none", "fxaa", "fxaa_extreme"),
      CYCLE_LIVE("ANISOTROPIC FILTER", "Override texture anisotropy: -1 default, 0 off, 1-5 increasing.", "anisotropic_override", "-1", "0", "1", "2", "3", "4", "5"),
      CYCLE_RESTART("INTERNAL SCALE", "Integer internal draw resolution scale.", "resolution_scale", "1", "2", "3", "4"),
      BOOL_LIVE("3D TEXTURE COMPATIBILITY", "Use the 3D-to-2D texture compatibility path.", "gpu_3d_to_2d_texture"),
      BOOL_RESTART("NATIVE 2X MSAA", "Enable native two-sample multisampling.", "native_2x_msaa"),
      BOOL_RESTART("GAMMA UNORM16", "Use 16-bit UNORM gamma render targets.", "gamma_render_target_as_unorm16"),
      BOOL_RESTART("FUZZY ALPHA EPSILON", "Improve compatibility with fuzzy alpha tests.", "use_fuzzy_alpha_epsilon"),
      BOOL_RESTART("SCALED TEXTURE OFFSETS", "Scale shader texture offsets with internal resolution.", "draw_resolution_scaled_texture_offsets"),
      BOOL_LIVE("HALF PIXEL OFFSET", "Enable the Xbox 360 half-pixel sampling offset.", "half_pixel_offset"),
      BOOL_LIVE("SCALED RESOLVE HALF PIXEL", "Apply half-pixel handling to scaled resolves.", "resolve_resolution_scale_fill_half_pixel_offset"),
      BOOL_LIVE("DIRECT HOST RESOLVE", "Use direct host render-target resolve when possible.", "direct_host_resolve"),
      BOOL_RESTART("DEPTH FLOAT24 ROUND", "Round emulated float24 depth values.", "depth_float24_round"),
      BOOL_RESTART("DEPTH FLOAT24 PIXEL CONVERT", "Convert float24 depth in the pixel shader.", "depth_float24_convert_in_pixel_shader"),
      BOOL_RESTART("DEPTH NOT-EQUAL TRANSFER", "Preserve depth transfer not-equal tests.", "depth_transfer_not_equal_test"),
      BOOL_RESTART("NATIVE STENCIL OUTPUT", "Use native stencil value output.", "native_stencil_value_output"),
      BOOL_RESTART("SNORM16 FULL RANGE", "Use the full signed-normalized 16-bit render-target range.", "snorm16_render_target_full_range"),
      BOOL_RESTART("PRE-MASK RESOLVE L2", "Pre-mask scaled resolve L2 blocks.", "pre_mask_resolve_l2_block"),
      BOOL_RESTART("MRT EDRAM RANGE CLAMP", "Clamp MRT EDRAM used range to minimum bounds.", "mrt_edram_used_range_clamp_to_min"),
      BOOL_RESTART("D3D12 BINDLESS", "Use bindless D3D12 resources when available.", "d3d12_bindless"),
  };
  static const std::vector<OptionSpec> performance = {
      CYCLE_RESTART("PERFORMANCE PRESET", "Choose a complete host performance profile from one dropdown.", "__performance_profile", "custom", "recommended", "smooth_30", "nvidia", "amd"),
      BOOL_LIVE("ASYNC SHADER COMPILATION", "Compile shaders and pipelines on background threads.", "async_shader_compilation"),
      BOOL_RESTART("ASYNC PIPELINE WAIT", "Wait for asynchronous pipelines at submission.", "d3d12_async_pipeline_wait_at_submission"),
      CYCLE_RESTART("PIPELINE THREADS", "D3D12 pipeline creation worker count.", "d3d12_pipeline_creation_threads", "-1", "0", "2", "4", "8", "12", "16"),
      CYCLE_LIVE("GPU QUEUE PRIORITY", "D3D12 queue priority: normal, high, realtime.", "d3d12_queue_priority", "0", "1", "2"),
      BOOL_LIVE("PRESENT OFF UI THREAD", "Move presentation away from the UI thread.", "host_present_from_non_ui_thread"),
      BOOL_LIVE("FAST MEMEXPORT", "Use double-buffered guest memexport readback.", "readback_memexport_fast"),
      BOOL_LIVE("MEMEXPORT READBACK", "Keep shader memexport writes coherent with guest memory.", "readback_memexport"),
      CYCLE_LIVE("RESOLVE READBACK", "Trade resolve accuracy for synchronization cost.", "readback_resolve", "none", "fast", "some", "full"),
      BOOL_LIVE("RESOLVE HALF PIXEL", "Center scaled-block samples during readback.", "readback_resolve_half_pixel_offset"),
      BOOL_LIVE("OCCLUSION QUERIES", "Enable host occlusion-query processing.", "occlusion_query_enable"),
      CYCLE_LIVE("FAKE OCCLUSION SAMPLES", "Fallback sample count for emulated queries.", "query_occlusion_fake_sample_count", "100", "500", "1000", "2500", "5000", "10000"),
      BOOL_LIVE("CLEAR MEMORY PAGE STATE", "Refresh GPU-written guest-memory page validity each frame.", "clear_memory_page_state"),
      BOOL_LIVE("STORE SHADERS", "Persist compiled shaders for future launches.", "store_shaders"),
      CYCLE_RESTART("TEXTURE SOFT LIMIT", "Texture cache soft memory limit in MB.", "texture_cache_memory_limit_soft", "128", "256", "384", "512", "768", "1024", "1536", "2048"),
      CYCLE_RESTART("TEXTURE HARD LIMIT", "Texture cache hard memory limit in MB.", "texture_cache_memory_limit_hard", "256", "512", "768", "1024", "1536", "2048", "3072", "4096"),
      CYCLE_RESTART("RENDER-TARGET TEXTURE LIMIT", "Render-to-texture memory limit in MB.", "texture_cache_memory_limit_render_to_texture", "16", "24", "32", "48", "64", "96", "128"),
      CYCLE_RESTART("TEXTURE EVICTION AGE", "Seconds before old textures may be evicted.", "texture_cache_memory_limit_soft_lifetime", "5", "10", "20", "30", "60", "120"),
      BOOL_RESTART("IGNORE GUEST THREAD PRIORITIES", "Use host scheduling rather than guest priorities.", "ignore_thread_priorities"),
      BOOL_RESTART("IGNORE GUEST AFFINITIES", "Allow host OS to schedule guest threads freely.", "ignore_thread_affinities"),
  };
  static const std::vector<OptionSpec> controls = {
      CYCLE_RESTART("INPUT BACKEND", "Select SDL or XInput controller handling.", "input_backend", "sdl", "xinput"),
      BOOL_LIVE("MOUSE + KEYBOARD MODE", "Map mouse and keyboard input to a guest controller.", "mnk_mode"),
      CYCLE_LIVE("MOUSE SENSITIVITY", "Mouse-to-right-stick sensitivity.", "mnk_sensitivity", "0.25", "0.5", "0.75", "1", "1.25", "1.5", "2", "3", "5"),
      CYCLE_LIVE("KEYBOARD PLAYER", "Guest controller slot receiving mouse and keyboard input.", "mnk_user_index", "0", "1", "2", "3"),
      BOOL_LIVE("GUIDE BUTTON", "Forward the controller Guide button.", "guide_button"),
      CYCLE_RESTART("CONTROLLER DATABASE", "SDL controller mapping database filename.", "hid_mappings_file", "gamecontrollerdb.txt", ""),
      CYCLE_LIVE("A BUTTON KEY", "Keyboard binding used for guest A.", "keybind_a", "Space", "Enter", "E", "F"),
      CYCLE_LIVE("B BUTTON KEY", "Keyboard binding used for guest B.", "keybind_b", "Shift", "Escape", "Q", "R"),
      CYCLE_LIVE("X BUTTON KEY", "Keyboard binding used for guest X.", "keybind_x", "R", "E", "F", "Mouse4"),
      CYCLE_LIVE("Y BUTTON KEY", "Keyboard binding used for guest Y.", "keybind_y", "E", "Q", "R", "Mouse5"),
      CYCLE_LIVE("LEFT TRIGGER KEY", "Keyboard/mouse binding for left trigger.", "keybind_left_trigger", "RMB", "LMB", "Q", "Shift"),
      CYCLE_LIVE("RIGHT TRIGGER KEY", "Keyboard/mouse binding for right trigger.", "keybind_right_trigger", "LMB", "RMB", "E", "Ctrl"),
  };
  static const std::vector<OptionSpec> audio = {
      BOOL_LIVE("MUTE AUDIO", "Mute all runtime audio output.", "audio_mute"),
      CYCLE_RESTART("AUDIO BUFFER FRAMES", "Queued audio frames; lower latency may increase crackle.", "audio_maxqframes", "4", "6", "8", "10", "12", "16", "24", "32", "48", "64"),
      CYCLE_RESTART("AUDIO BACKEND", "Select the runtime audio implementation.", "apu", "sdl", "nop"),
  };
  static const std::vector<OptionSpec> gameplay = {
      ACT("30 FPS TIMING INFO", "Explain why guest timing is not blindly unlocked.", Action::kInfo30Fps),
      BOOL_LIVE("FPS OVERLAY", "Show measured guest frame rate.", "generations_show_game_fps"),
      CYCLE_RESTART("USER LANGUAGE", "Xbox user language identifier.", "user_language", "1", "2", "3", "4", "5", "6", "7", "8", "9"),
      CYCLE_RESTART("USER COUNTRY", "Xbox user country identifier.", "user_country", "103", "1", "44", "49", "81", "33", "39", "34"),
      BOOL_RESTART("VIRTUAL LIVE PROFILE", "Expose a virtual signed-in profile for system-link features.", "system_link_virtual_live_profile"),
      BOOL_RESTART("ALLOW SYSTEM-LINK PRIVILEGES", "Grant local system-link privileges.", "system_link_allow_privileges"),
      CYCLE_RESTART("LICENSE MASK", "Content license mask used by the runtime.", "license_mask", "0", "1", "3", "7", "15", "31", "63", "255"),
      ACT("CREATE SAVE BACKUP", "Create a numbered backup of the current save.", Action::kBackup),
      ACT("SAVE CONFIG", "Persist all currently selected settings.", Action::kSaveConfig),
  };
  static const std::vector<OptionSpec> interface_rows = {
      BOOL_LIVE("FPS COUNTER", "Show the compact FPS overlay.", "generations_show_game_fps"),
      BOOL_LIVE("DIM BACKGROUND", "Darken the game behind Generation Setting.", "generations_menu_dim_background"),
      BOOL_LIVE("STATUS BAR", "Show status messages and controller hints.", "generations_menu_show_status_bar"),
      BOOL_LIVE("HIGH CONTRAST", "Increase selected-row contrast.", "generations_menu_high_contrast"),
      BOOL_LIVE("ANIMATIONS", "Enable opening, page and selection animation.", "generations_menu_animations"),
      BOOL_LIVE("REDUCE MOTION", "Reduce movement while keeping soft fades.", "generations_menu_reduce_motion"),
      BOOL_LIVE("LARGE TEXT", "Increase launcher font scale.", "generations_menu_large_text"),
      BOOL_LIVE("COMPACT TABS", "Force a single compact top navigation row.", "generations_menu_compact_tabs"),
      BOOL_LIVE("TOOLTIPS / DESCRIPTIONS", "Show detailed descriptions in the right panel.", "generations_menu_tooltips"),
      BOOL_LIVE("PERFORMANCE GRAPH", "Show the live guest performance graph.", "generations_menu_show_graph"),
      ACT("RESET INTERFACE", "Restore Generation Setting appearance defaults.", Action::kResetInterface),
      ACT("SAVE INTERFACE", "Save interface settings to config.", Action::kSaveConfig),
  };
  static const std::vector<OptionSpec> runtime = {
      BOOL_RESTART("CLOCK NO SCALING", "Disable runtime clock scaling.", "clock_no_scaling"),
      BOOL_RESTART("RAW CLOCK SOURCE", "Use the raw host clock source.", "clock_source_raw"),
      BOOL_RESTART("PROTECT ZERO PAGE", "Protect guest address zero from access.", "protect_zero"),
      BOOL_RESTART("PROTECT RELEASED MEMORY", "Protect guest pages after release.", "protect_on_release"),
      BOOL_RESTART("SCRIBBLE HEAP", "Fill allocated heap memory with debug data.", "scribble_heap"),
      BOOL_RESTART("ALLOW GAME RELATIVE WRITES", "Allow game-relative VFS writes.", "allow_game_relative_writes"),
      BOOL_RESTART("APPLY XEX PATCHES", "Apply available XEX patches during module loading.", "xex_apply_patches"),
      BOOL_RESTART("SYSTEM LINK PROFILE", "Enable virtual profile support for local multiplayer.", "system_link_virtual_live_profile"),
      CYCLE_RESTART("SYSTEM LINK ADAPTER", "Network adapter name used for discovery.", "system_link_adapter", "Radmin", "Ethernet", "Wi-Fi", "Hamachi", "ZeroTier"),
      CYCLE_RESTART("DISCOVERY PORT", "UDP port used by system-link discovery.", "system_link_discovery_port", "3074", "32137", "32138", "27015"),
      BOOL_RESTART("CRT HEAP", "Enable the runtime CRT heap implementation.", "rexcrt_heap_enable"),
      CYCLE_RESTART("CRT HEAP SIZE", "Runtime CRT heap size in MB.", "rexcrt_heap_size_mb", "128", "256", "384", "512", "768", "1024"),
      ACT("SAVE RUNTIME CONFIG", "Persist current runtime settings.", Action::kSaveConfig),
  };
  static const std::vector<OptionSpec> developer = {
      CYCLE_LIVE("LOG LEVEL", "Runtime log severity threshold.", "log_level", "trace", "debug", "info", "warn", "error", "critical", "off"),
      BOOL_LIVE("VERBOSE LOGGING", "Enable trace-level verbose logging.", "log_verbose"),
      BOOL_LIVE("NOISY LOGGING", "Enable high-frequency diagnostic messages.", "log_noisy"),
      CYCLE_LIVE("LOG FLUSH INTERVAL", "Periodic log flush interval in seconds.", "log_flush_interval", "0", "1", "2", "5", "10", "30"),
      CYCLE_LIVE("LOG ROTATION SIZE", "Maximum log file size before rotation in MB.", "log_max_file_size_mb", "1", "5", "10", "25", "50", "100"),
      BOOL_LIVE("GPU DEBUG MARKERS", "Insert host GPU debug markers.", "gpu_debug_markers"),
      BOOL_LIVE("GPU TRACE STREAM", "Enable GPU trace streaming.", "trace_gpu_stream"),
      BOOL_RESTART("D3D12 DEBUG LAYER", "Enable the D3D12 and DXGI debug layer.", "d3d12_debug"),
      BOOL_RESTART("D3D12 BREAK ON ERROR", "Break into debugger on D3D12 errors.", "d3d12_break_on_error"),
      BOOL_RESTART("D3D12 BREAK ON WARNING", "Break into debugger on D3D12 warnings.", "d3d12_break_on_warning"),
      BOOL_RESTART("D3D12 TESSELLATION WIREFRAME", "Render tessellation output as wireframe.", "d3d12_tessellation_wireframe"),
      BOOL_RESTART("VULKAN VALIDATION", "Enable Vulkan validation layers.", "vulkan_validation_enabled"),
      BOOL_RESTART("VULKAN DEBUG LOG", "Log Vulkan debug messages.", "vulkan_log_debug_messages"),
      BOOL_RESTART("VULKAN WIREFRAME", "Render Vulkan tessellation output as wireframe.", "vulkan_tessellation_wireframe"),
      BOOL_RESTART("KERNEL DEBUG MONITOR", "Enable the guest kernel debug monitor.", "kernel_debug_monitor"),
      BOOL_RESTART("KERNEL CERT MONITOR", "Enable the guest certification monitor.", "kernel_cert_monitor"),
      BOOL_RESTART("PIX SUPPORT", "Enable PIX debugging support.", "kernel_pix"),
      BOOL_RESTART("HIGH-FREQUENCY KERNEL LOG", "Log high-frequency kernel calls.", "log_high_frequency_kernel_calls"),
      ACT("SAVE CONFIGURATION", "Write all runtime CVars to the config file.", Action::kSaveConfig),
  };
  static const std::vector<OptionSpec> about = {
      ACT("GENERATION SETTING", "Controller-first ReXGlue ImGui settings dashboard.", Action::kNone),
      ACT("RUNTIME INTEGRATION", "All listed controls map to real ReXGlue CVars or project actions.", Action::kNone),
      ACT("60 FPS STATUS", "A true 60 FPS gameplay patch still requires game-specific PPC timing work.", Action::kInfo30Fps),
      ACT("CLOSE", "Return to Ultimate Ninja Storm Generations.", Action::kClose),
  };
  switch (tab) {
    case 0: return display; case 1: return graphics; case 2: return performance;
    case 3: return controls; case 4: return audio; case 5: return gameplay;
    case 6: return interface_rows; case 7: return runtime; case 8: return developer;
    default: return about;
  }
}

std::filesystem::path FindSaveFile(const std::filesystem::path& root) {
  std::error_code ec;
  if (root.empty() || !std::filesystem::is_directory(root, ec)) return {};
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().filename() == "STORM_G.S") return it->path();
  }
  return {};
}

std::string Uppercase(std::string_view value) {
  std::string result(value);
  for (char& c : result) { if (c == '_') c = ' '; else if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); }
  return result;
}

}  // namespace

NativeGameMenu::NativeGameMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
    rex::graphics::GraphicsSystem* graphics, std::filesystem::path config,
    std::filesystem::path user_root, std::function<void()> open,
    std::function<void(bool)> fullscreen)
    : rex::ui::ImGuiDialog(drawer), window_(window), graphics_system_(graphics),
      config_path_(std::move(config)), user_data_root_(std::move(user_root)),
      request_open_on_ui_thread_(std::move(open)),
      request_fullscreen_on_ui_thread_(std::move(fullscreen)) {}
NativeGameMenu::~NativeGameMenu() = default;

void NativeGameMenu::DetachFromUIThread() { visible_ = false; open_enqueued_ = false; pending_action_ = 0; dropdown_open_ = false; }
void NativeGameMenu::OpenFromUIThread() { open_enqueued_ = false; visible_ = true; dropdown_open_ = false; animation_progress_ = 0; tab_transition_ = 0; status_ = "READY"; status_is_warning_ = false; }
void NativeGameMenu::ApplyFullscreenOnUIThread(bool enabled) { if (window_ && window_->IsFullscreen() != enabled) window_->SetFullscreen(enabled); }
bool NativeGameMenu::HandleControllerKeystroke(uint32_t, rex::input::X_INPUT_KEYSTROKE&) { return IsVisible(); }
void NativeGameMenu::HandleControllerState(uint32_t, rex::input::X_INPUT_STATE& state) {
  const uint16_t buttons = static_cast<uint16_t>(state.gamepad.buttons);
  const uint16_t previous = previous_buttons_.exchange(buttons);
  constexpr uint16_t combo = rex::input::X_INPUT_GAMEPAD_START | rex::input::X_INPUT_GAMEPAD_BACK;
  const bool combo_down = (buttons & combo) == combo;
  if (combo_down && !menu_combo_latched_.exchange(true)) {
    if (IsVisible()) QueueAction(PendingAction::kClose);
    else if (!open_enqueued_.exchange(true) && request_open_on_ui_thread_) request_open_on_ui_thread_();
  } else if (!combo_down) menu_combo_latched_ = false;
  if (!IsVisible()) { if (combo_down) state.gamepad.buttons = static_cast<uint16_t>(buttons & ~combo); return; }
  const uint16_t pressed = static_cast<uint16_t>(buttons & ~previous);
  if (!combo_down) {
    if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) QueueAction(PendingAction::kUp);
    else if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) QueueAction(PendingAction::kDown);
    else if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) QueueAction(PendingAction::kLeft);
    else if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) QueueAction(PendingAction::kRight);
    else if (pressed & rex::input::X_INPUT_GAMEPAD_A) QueueAction(PendingAction::kConfirm);
    else if (pressed & rex::input::X_INPUT_GAMEPAD_B) QueueAction(PendingAction::kClose);
    else if (pressed & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) QueueAction(PendingAction::kPreviousTab);
    else if (pressed & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) QueueAction(PendingAction::kNextTab);
  }
  state.gamepad = {};
}
void NativeGameMenu::HandleKeyDown(rex::ui::KeyEvent&) {}
void NativeGameMenu::QueueAction(PendingAction a) { pending_action_ = static_cast<uint8_t>(a); }
void NativeGameMenu::ProcessPendingAction() {
  const auto a = static_cast<PendingAction>(pending_action_.exchange(0));
  if (a == PendingAction::kNone) return;

  if (dropdown_open_) {
    const auto& options = OptionsFor(static_cast<int>(active_tab_));
    if (dropdown_row_ >= options.size()) { dropdown_open_ = false; return; }
    const OptionSpec& option = options[dropdown_row_];
    const size_t value_count = option.values.size();
    if (!value_count) { dropdown_open_ = false; return; }
    switch (a) {
      case PendingAction::kUp:
      case PendingAction::kLeft:
        dropdown_index_ = dropdown_index_ ? dropdown_index_ - 1 : value_count - 1;
        break;
      case PendingAction::kDown:
      case PendingAction::kRight:
        dropdown_index_ = (dropdown_index_ + 1) % value_count;
        break;
      case PendingAction::kConfirm:
        CommitDropdownSelection();
        break;
      case PendingAction::kClose:
        dropdown_open_ = false;
        status_ = "DROPDOWN CLOSED";
        status_is_warning_ = false;
        break;
      default:
        break;
    }
    return;
  }

  size_t& selected = selected_rows_[static_cast<size_t>(active_tab_)];
  const size_t count = RowCount(active_tab_);
  if (!count) return;
  switch (a) {
    case PendingAction::kUp: selected = selected ? selected - 1 : count - 1; break;
    case PendingAction::kDown: selected = (selected + 1) % count; break;
    case PendingAction::kLeft: ChangeSelectedValue(-1); break;
    case PendingAction::kRight: ChangeSelectedValue(1); break;
    case PendingAction::kConfirm: {
      const auto& options = OptionsFor(static_cast<int>(active_tab_));
      if (selected < options.size() && options[selected].type == OptionType::kCycle) {
        OpenSelectedDropdown();
      } else {
        ChangeSelectedValue(1);
      }
      break;
    }
    case PendingAction::kClose: CloseFromUIThread(); break;
    case PendingAction::kPreviousTab: CycleTab(-1); break;
    case PendingAction::kNextTab: CycleTab(1); break;
    default: break;
  }
}

void NativeGameMenu::OpenSelectedDropdown() {
  const auto& options = OptionsFor(static_cast<int>(active_tab_));
  const size_t row = selected_rows_[static_cast<size_t>(active_tab_)];
  if (row >= options.size() || options[row].type != OptionType::kCycle || options[row].values.empty()) return;
  const OptionSpec& option = options[row];
  std::string current;
  if (std::string_view(option.cvar) == "__scaler_profile") {
    current = option.values[std::min(scaler_profile_index_, option.values.size() - 1)];
  } else if (std::string_view(option.cvar) == "__performance_profile") {
    current = option.values[std::min(performance_profile_index_, option.values.size() - 1)];
  } else {
    current = rex::cvar::GetFlagByName(option.cvar);
  }
  dropdown_row_ = row;
  dropdown_index_ = 0;
  for (size_t i = 0; i < option.values.size(); ++i) {
    if (current == option.values[i]) { dropdown_index_ = i; break; }
  }
  dropdown_open_ = true;
  status_ = "SELECT A VALUE - A APPLY / B CANCEL";
  status_is_warning_ = false;
}

void NativeGameMenu::CommitDropdownSelection() {
  const auto& options = OptionsFor(static_cast<int>(active_tab_));
  if (dropdown_row_ >= options.size()) { dropdown_open_ = false; return; }
  const OptionSpec& option = options[dropdown_row_];
  if (dropdown_index_ >= option.values.size()) { dropdown_open_ = false; return; }
  ApplyOptionValue(active_tab_, dropdown_row_, option.values[dropdown_index_]);
  dropdown_open_ = false;
}

void NativeGameMenu::CycleTab(int direction) {
  constexpr int count = 10;
  active_tab_ = static_cast<Tab>((static_cast<int>(active_tab_) + direction + count) % count);
  tab_transition_ = 0;
}

void NativeGameMenu::ApplyOptionValue(Tab tab, size_t row, const char* value) {
  const auto& options = OptionsFor(static_cast<int>(tab));
  if (row >= options.size()) return;
  const OptionSpec& option = options[row];

  if (option.type == OptionType::kCycle) {
    const std::string_view cvar = option.cvar ? std::string_view(option.cvar) : std::string_view{};
    if (cvar == "__scaler_profile") {
      for (size_t i = 0; i < option.values.size(); ++i) if (std::string_view(value) == option.values[i]) scaler_profile_index_ = i;
      if (std::string_view(value) == "low_cost") ApplyScalerProfile(0);
      else if (std::string_view(value) == "balanced") ApplyScalerProfile(1);
      else if (std::string_view(value) == "quality") ApplyScalerProfile(2);
      else if (std::string_view(value) == "ultra") ApplyScalerProfile(3);
      else { status_ = "SCALER PRESET SET TO CUSTOM"; status_is_warning_ = false; }
      return;
    }
    if (cvar == "__performance_profile") {
      for (size_t i = 0; i < option.values.size(); ++i) if (std::string_view(value) == option.values[i]) performance_profile_index_ = i;
      if (std::string_view(value) == "recommended") ApplyRecommendedProfile();
      else if (std::string_view(value) == "smooth_30") ApplySmooth30Profile();
      else if (std::string_view(value) == "nvidia") ApplyNvidiaProfile();
      else if (std::string_view(value) == "amd") ApplyAmdProfile();
      else { status_ = "PERFORMANCE PRESET SET TO CUSTOM"; status_is_warning_ = false; }
      return;
    }
    if (cvar == "swap_post_effect") { SetSwapPostEffect(value); return; }
    if (cvar == "resolution_scale") {
      rex::cvar::SetFlagByName("draw_resolution_scale_x", value);
      rex::cvar::SetFlagByName("draw_resolution_scale_y", value);
      if ((std::string_view(value) == "3" || std::string_view(value) == "4") && IsEnabled("native_2x_msaa")) {
        SetEnabled("native_2x_msaa", false);
        status_ = "HIGH SCALE SAVED; 2X MSAA DISABLED TO PREVENT EXTREME GPU COST";
        status_is_warning_ = true;
      }
    }
    if (tab == Tab::kGraphics) scaler_profile_index_ = 0;
    if (tab == Tab::kPerformance) performance_profile_index_ = 0;
    if (!rex::cvar::SetFlagByName(option.cvar, value)) {
      status_ = "VALUE IS NOT SUPPORTED BY THIS RUNTIME BUILD";
      status_is_warning_ = true;
      return;
    }
    SaveSettings(option.success);
    status_is_warning_ = std::string_view(option.state) == "RESTART";
    return;
  }
}

void NativeGameMenu::ChangeSelectedValue(int direction) {
  const auto& options = OptionsFor(static_cast<int>(active_tab_));
  const size_t row = selected_rows_[static_cast<size_t>(active_tab_)];
  if (row >= options.size()) return;
  const OptionSpec& option = options[row];
  if (option.type == OptionType::kBool) {
    const bool requested = !IsEnabled(option.cvar);
    if (std::string_view(option.cvar) == "native_2x_msaa" && requested) {
      const std::string scale = rex::cvar::GetFlagByName("draw_resolution_scale_x");
      if (scale == "3" || scale == "4") {
        status_ = "MSAA BLOCKED AT 3X/4X INTERNAL SCALE - LOWER SCALE FIRST";
        status_is_warning_ = true;
        return;
      }
    }
    if (!SetEnabled(option.cvar, requested)) {
      status_ = "OPTION IS NOT AVAILABLE IN THIS RUNTIME BUILD"; status_is_warning_ = true; return;
    }
    if (active_tab_ == Tab::kGraphics) scaler_profile_index_ = 0;
    if (active_tab_ == Tab::kPerformance) performance_profile_index_ = 0;
    SaveSettings(option.success); status_is_warning_ = std::string_view(option.state) == "RESTART"; return;
  }
  if (option.type == OptionType::kCycle) {
    std::string current;
    if (std::string_view(option.cvar) == "__scaler_profile") current = option.values[std::min(scaler_profile_index_, option.values.size() - 1)];
    else if (std::string_view(option.cvar) == "__performance_profile") current = option.values[std::min(performance_profile_index_, option.values.size() - 1)];
    else current = rex::cvar::GetFlagByName(option.cvar);
    size_t index = 0;
    for (size_t i = 0; i < option.values.size(); ++i) if (current == option.values[i]) { index = i; break; }
    index = static_cast<size_t>((static_cast<int>(index) + direction + static_cast<int>(option.values.size())) % static_cast<int>(option.values.size()));
    ApplyOptionValue(active_tab_, row, option.values[index]);
    return;
  }
  switch (option.action) {
    case Action::kResume: case Action::kClose: CloseFromUIThread(); break;
    case Action::kRecommended: ApplyRecommendedProfile(); break;
    case Action::kSmooth30: ApplySmooth30Profile(); break;
    case Action::kNvidia: ApplyNvidiaProfile(); break;
    case Action::kAmd: ApplyAmdProfile(); break;
    case Action::kScalerLowCost: ApplyScalerProfile(0); break;
    case Action::kScalerBalanced: ApplyScalerProfile(1); break;
    case Action::kScalerQuality: ApplyScalerProfile(2); break;
    case Action::kScalerUltra: ApplyScalerProfile(3); break;
    case Action::kFullscreen: SetFullscreen(!window_ || !window_->IsFullscreen()); break;
    case Action::kBackup: CreateSaveBackup(); break;
    case Action::kSaveConfig: SaveSettings("ALL SETTINGS SAVED"); break;
    case Action::kResetInterface:
      SetEnabled("generations_menu_dim_background", false); SetEnabled("generations_menu_show_status_bar", true);
      SetEnabled("generations_menu_high_contrast", false); SetEnabled("generations_menu_animations", true);
      SetEnabled("generations_menu_reduce_motion", false); SetEnabled("generations_menu_large_text", true);
      SetEnabled("generations_menu_compact_tabs", false); SetEnabled("generations_menu_tooltips", true);
      SetEnabled("generations_menu_show_graph", true); SaveSettings("INTERFACE DEFAULTS RESTORED"); break;
    case Action::kInfo30Fps:
      status_ = "GUEST LOGIC REMAINS 30 FPS; HOST PACING OPTIONS ARE SAFE"; status_is_warning_ = true; break;
    default: status_ = "INFORMATION ONLY"; status_is_warning_ = false; break;
  }
}

void NativeGameMenu::CloseFromUIThread() { dropdown_open_ = false; visible_ = false; }
size_t NativeGameMenu::RowCount(Tab tab) const { return OptionsFor(static_cast<int>(tab)).size(); }
std::string NativeGameMenu::RowLabel(Tab tab, size_t row) const { const auto& o = OptionsFor(static_cast<int>(tab)); return row < o.size() ? o[row].label : ""; }
std::string NativeGameMenu::RowValue(Tab tab, size_t row) const {
  const auto& o = OptionsFor(static_cast<int>(tab)); if (row >= o.size()) return {};
  const OptionSpec& opt = o[row];
  if (opt.type == OptionType::kBool) return IsEnabled(opt.cvar) ? "ON" : "OFF";
  if (opt.type == OptionType::kCycle) {
    std::string v;
    if (std::string_view(opt.cvar) == "__scaler_profile") v = opt.values[std::min(scaler_profile_index_, opt.values.size() - 1)];
    else if (std::string_view(opt.cvar) == "__performance_profile") v = opt.values[std::min(performance_profile_index_, opt.values.size() - 1)];
    else v = rex::cvar::GetFlagByName(opt.cvar);
    return (v.empty() ? "AUTO" : Uppercase(v)) + "  v";
  }
  switch (opt.action) {
    case Action::kResume: return "RETURN"; case Action::kRecommended: return "APPLY";
    case Action::kSmooth30: return "APPLY"; case Action::kNvidia: return "APPLY";
    case Action::kAmd: return "APPLY";
    case Action::kScalerLowCost: case Action::kScalerBalanced:
    case Action::kScalerQuality: case Action::kScalerUltra: return "APPLY";
    case Action::kFullscreen: return window_ && window_->IsFullscreen() ? "WINDOWED" : "FULLSCREEN";
    case Action::kBackup: return "BACKUP"; case Action::kSaveConfig: return "SAVE";
    case Action::kResetInterface: return "RESET"; case Action::kInfo30Fps: return "INFO";
    case Action::kClose: return "CLOSE"; default: return "INFO";
  }
}
const char* NativeGameMenu::RowState(Tab tab, size_t row) const { const auto& o = OptionsFor(static_cast<int>(tab)); return row < o.size() ? o[row].state : "LIVE"; }
const char* NativeGameMenu::RowDescription(Tab tab, size_t row) const { const auto& o = OptionsFor(static_cast<int>(tab)); return row < o.size() ? o[row].description : ""; }

void NativeGameMenu::ApplyRecommendedProfile() {
  SetEnabled("present_vsync", true); SetEnabled("vsync", true); SetEnabled("present_letterbox", true);
  SetEnabled("async_shader_compilation", true); SetEnabled("store_shaders", true);
  SetEnabled("host_present_from_non_ui_thread", true); SetEnabled("readback_memexport", true);
  SetEnabled("readback_memexport_fast", true); SetEnabled("occlusion_query_enable", true);
  SetEnabled("clear_memory_page_state", true);
  rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", "768");
  rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", "1536");
  rex::cvar::SetFlagByName("texture_cache_memory_limit_render_to_texture", "64");
  rex::cvar::SetFlagByName("texture_cache_memory_limit_soft_lifetime", "180");
  rex::cvar::SetFlagByName("d3d12_pipeline_creation_threads", "4");
  rex::cvar::SetFlagByName("anisotropic_override", "5"); rex::cvar::SetFlagByName("d3d12_queue_priority", "1");
  SetSwapPostEffect("fxaa"); SaveSettings("RECOMMENDED PROFILE APPLIED");
}
void NativeGameMenu::ApplySmooth30Profile() {
  SetEnabled("present_vsync", true); SetEnabled("vsync", true);
  SetEnabled("d3d12_allow_variable_refresh_rate_and_tearing", true);
  SetEnabled("host_present_from_non_ui_thread", true); SetEnabled("async_shader_compilation", true);
  SetEnabled("store_shaders", true); SetEnabled("readback_memexport_fast", true);
  rex::cvar::SetFlagByName("d3d12_pipeline_creation_threads", "4");
  rex::cvar::SetFlagByName("d3d12_queue_priority", "1");
  rex::cvar::SetFlagByName("video_mode_refresh_rate", "60");
  rex::cvar::SetFlagByName("audio_maxqframes", "6"); SaveSettings("SMOOTH 30 PROFILE APPLIED");
}
void NativeGameMenu::ApplyNvidiaProfile() {
  ApplyRecommendedProfile(); rex::cvar::SetFlagByName("present_effect", "cas");
  rex::cvar::SetFlagByName("present_cas_additional_sharpness", "0.4");
  SetEnabled("d3d12_bindless", true); rex::cvar::SetFlagByName("d3d12_queue_priority", "1");
  SaveSettings("NVIDIA PROFILE SAVED - RESTART FOR UPSCALER"); status_is_warning_ = true;
}
void NativeGameMenu::ApplyAmdProfile() {
  ApplyRecommendedProfile();
  if (!rex::cvar::SetFlagByName("present_effect", "fsr")) rex::cvar::SetFlagByName("present_effect", "cas");
  rex::cvar::SetFlagByName("present_fsr_quality_mode", "quality");
  rex::cvar::SetFlagByName("present_cas_additional_sharpness", "0.3");
  SaveSettings("AMD PROFILE SAVED - RESTART FOR UPSCALER"); status_is_warning_ = true;
}
void NativeGameMenu::ApplyScalerProfile(int profile) {
  const char* scale = "1";
  const char* effect = "fsr";
  const char* quality = "quality";
  const char* sharpness = "0.25";
  const char* status = "LOW COST SCALER SAVED - RESTART REQUIRED";

  switch (profile) {
    case 1:
      scale = "2"; effect = "fsr"; quality = "quality"; sharpness = "0.25";
      status = "BALANCED SCALER SAVED - RESTART REQUIRED";
      break;
    case 2:
      scale = "2"; effect = "cas"; quality = "nativeaa"; sharpness = "0.35";
      status = "QUALITY SCALER SAVED - RESTART REQUIRED";
      break;
    case 3:
      scale = "3"; effect = "cas"; quality = "nativeaa"; sharpness = "0.25";
      status = "ULTRA SCALER SAVED - VERY HIGH GPU COST";
      break;
    default:
      break;
  }

  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
  rex::cvar::SetFlagByName("present_effect", effect);
  rex::cvar::SetFlagByName("present_fsr_quality_mode", quality);
  rex::cvar::SetFlagByName("present_cas_additional_sharpness", sharpness);
  rex::cvar::SetFlagByName("present_fsr_max_upsampling_passes", "1");
  rex::cvar::SetFlagByName("present_fsr_sharpness_reduction", "0.5");
  SetEnabled("native_2x_msaa", false);
  SetEnabled("draw_resolution_scaled_texture_offsets", true);
  SetEnabled("present_dither", true);
  rex::cvar::SetFlagByName("anisotropic_override", "5");
  SetSwapPostEffect(profile == 0 ? "fxaa" : "none");
  SaveSettings(status);
  status_is_warning_ = true;
}

void NativeGameMenu::SetSwapPostEffect(const char* value) {
  rex::cvar::SetFlagByName("swap_post_effect", value);
  if (graphics_system_ && graphics_system_->command_processor()) {
    auto effect = rex::graphics::CommandProcessor::SwapPostEffect::kNone;
    if (std::string_view(value) == "fxaa") effect = rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
    else if (std::string_view(value) == "fxaa_extreme") effect = rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
    graphics_system_->command_processor()->SetDesiredSwapPostEffect(effect);
  }
  SaveSettings("POST EFFECT APPLIED");
}
void NativeGameMenu::SetFullscreen(bool enabled) {
  rex::cvar::SetFlagByName("fullscreen", enabled ? "true" : "false"); SaveSettings(enabled ? "FULLSCREEN APPLIED" : "WINDOWED MODE APPLIED");
  if (request_fullscreen_on_ui_thread_) request_fullscreen_on_ui_thread_(enabled); else ApplyFullscreenOnUIThread(enabled);
}
void NativeGameMenu::SaveSettings(const char* status) { rex::cvar::SaveConfig(config_path_); status_ = status; status_is_warning_ = false; }
void NativeGameMenu::CreateSaveBackup() {
  const auto save = FindSaveFile(user_data_root_); if (save.empty()) { status_ = "NO GENERATIONS SAVE FOUND"; status_is_warning_ = true; return; }
  std::error_code ec; const auto dir = user_data_root_ / "recomp_backups"; std::filesystem::create_directories(dir, ec);
  if (ec) { status_ = "BACKUP FOLDER FAILED"; status_is_warning_ = true; return; }
  auto path = dir / "STORM_G.S.backup"; for (uint32_t i = 2; std::filesystem::exists(path, ec); ++i) path = dir / ("STORM_G.S.backup." + std::to_string(i));
  std::filesystem::copy_file(save, path, std::filesystem::copy_options::none, ec);
  status_ = ec ? "SAVE BACKUP FAILED" : "SAVE BACKUP CREATED"; status_is_warning_ = static_cast<bool>(ec);
}
bool NativeGameMenu::IsFpsCounterEnabled() const { return IsEnabled("generations_show_game_fps"); }

void NativeGameMenu::ApplyImGuiStyle() {
  if (style_applied_) return;
  style_applied_ = true;
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(0.0f, 0.0f);
  style.WindowRounding = 10.0f;
  style.ChildRounding = 7.0f;
  style.FrameRounding = 5.0f;
  style.ScrollbarRounding = 7.0f;
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 0.0f;
  style.FrameBorderSize = 0.0f;
  style.ItemSpacing = ImVec2(10.0f, 10.0f);
  style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  style.ScrollbarSize = 9.0f;

  // Neutral charcoal palette with restrained orange focus accents. This keeps
  // the menu bright enough for release without the heavy blue cast.
  auto& c = style.Colors;
  c[ImGuiCol_Text] = ImVec4(0.985f, 0.985f, 0.98f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.74f, 0.74f, 0.73f, 1.0f);
  c[ImGuiCol_WindowBg] = ImVec4(0.115f, 0.112f, 0.108f, 0.985f);
  c[ImGuiCol_ChildBg] = ImVec4(0.145f, 0.141f, 0.136f, 0.97f);
  c[ImGuiCol_PopupBg] = ImVec4(0.135f, 0.130f, 0.125f, 0.995f);
  c[ImGuiCol_Border] = ImVec4(0.46f, 0.43f, 0.40f, 0.62f);
  c[ImGuiCol_FrameBg] = ImVec4(0.190f, 0.183f, 0.176f, 0.98f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.245f, 0.230f, 0.215f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.300f, 0.265f, 0.235f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.235f, 0.215f, 0.195f, 0.99f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.285f, 0.255f, 0.225f, 1.0f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.340f, 0.295f, 0.250f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.205f, 0.198f, 0.190f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.275f, 0.255f, 0.235f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.36f, 0.08f, 1.0f);
  c[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.43f, 0.10f, 1.0f);
  c[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.43f, 0.10f, 1.0f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.58f, 0.23f, 1.0f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.105f, 0.100f, 0.095f, 0.80f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.50f, 0.47f, 0.44f, 0.90f);
  c[ImGuiCol_Separator] = ImVec4(0.50f, 0.46f, 0.42f, 0.62f);
  c[ImGuiCol_PlotLines] = ImVec4(1.00f, 0.43f, 0.10f, 1.0f);
}

void NativeGameMenu::DrawFpsOverlay() {
  if (!IsFpsCounterEnabled() || IsVisible()) return;
  const auto stats = graphics_system_ ? graphics_system_->guest_swap_stats()
                                      : rex::graphics::GuestSwapStats{};
  ImGui::SetNextWindowPos(ImVec2(14.0f, 14.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.72f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
  ImGui::Begin("##GenerationFps", nullptr, ImGuiWindowFlags_NoDecoration |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.10f, 1.0f), "FPS");
  ImGui::SameLine();
  if (stats.frame_count) ImGui::Text("%.0f", stats.fps); else ImGui::Text("--");
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void NativeGameMenu::DrawHeader(const ImVec2& display_size) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos();
  const float width = ImGui::GetWindowWidth();

  // Compact title strip: no oversized framed banner and no heavy top border.
  draw->AddRectFilled(p, ImVec2(p.x + width, p.y + 54.0f),
                      IM_COL32(47, 44, 41, 252), 10.0f,
                      ImDrawFlags_RoundCornersTop);
  draw->AddLine(ImVec2(p.x + 22.0f, p.y + 53.0f),
                ImVec2(p.x + width - 22.0f, p.y + 53.0f),
                IM_COL32(255, 106, 24, 115), 1.0f);

  ImGui::SetCursorPos(ImVec2(26.0f, 10.0f));
  ImGui::TextColored(ImVec4(0.98f, 0.98f, 0.97f, 1.0f), "GENERATION SETTINGS");
  ImGui::SetCursorPos(ImVec2(26.0f, 31.0f));
  ImGui::TextColored(ImVec4(0.96f, 0.45f, 0.13f, 1.0f),
                     "ULTIMATE NINJA STORM GENERATIONS");

  const auto stats = graphics_system_ ? graphics_system_->guest_swap_stats()
                                      : rex::graphics::GuestSwapStats{};
  const char* hint = "START + BACK  CLOSE";
  const float hint_width = ImGui::CalcTextSize(hint).x;
  ImGui::SetCursorPos(ImVec2(width - hint_width - 24.0f, 31.0f));
  ImGui::TextColored(ImVec4(0.76f, 0.75f, 0.73f, 1.0f), "%s", hint);

  const char* fps_label = stats.frame_count ? "GUEST FPS" : "GUEST -- FPS";
  if (stats.frame_count) {
    char fps_text[64]{};
    std::snprintf(fps_text, sizeof(fps_text), "GUEST %.0f FPS", stats.fps);
    const float fps_width = ImGui::CalcTextSize(fps_text).x;
    ImGui::SetCursorPos(ImVec2(width - fps_width - 24.0f, 10.0f));
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.93f, 1.0f), "%s", fps_text);
  } else {
    const float fps_width = ImGui::CalcTextSize(fps_label).x;
    ImGui::SetCursorPos(ImVec2(width - fps_width - 24.0f, 10.0f));
    ImGui::TextColored(ImVec4(0.76f, 0.75f, 0.73f, 1.0f), "%s", fps_label);
  }
  (void)display_size;
}

void NativeGameMenu::DrawTabs() {
  static constexpr std::array<const char*, 10> names = {
      "VIDEO", "GRAPHICS", "PERFORMANCE", "CONTROLS", "AUDIO",
      "GAMEPLAY", "INTERFACE", "RUNTIME", "DEVELOPER", "ABOUT"};
  const float available = ImGui::GetContentRegionAvail().x;
  const bool two_rows = !IsEnabled("generations_menu_compact_tabs") && available < 1540.0f;
  const int columns = two_rows ? 5 : 10;
  const float gap = 4.0f;
  const float tab_width = (available - gap * float(columns - 1)) / float(columns);
  const float tab_height = 40.0f;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  for (size_t i = 0; i < names.size(); ++i) {
    if (i && int(i % columns) != 0) ImGui::SameLine(0.0f, gap);
    const bool active = i == static_cast<size_t>(active_tab_);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_Header, active ? ImVec4(0.29f, 0.25f, 0.22f, 0.98f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.22f, 0.20f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(1.0f, 0.82f, 0.66f, 1.0f) : ImVec4(0.84f, 0.83f, 0.81f, 1.0f));
    const std::string id = std::string(names[i]) + "##top_tab";
    ImGui::Selectable(id.c_str(), active, ImGuiSelectableFlags_None, ImVec2(tab_width, tab_height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (active) {
      const float pulse = 0.82f + 0.18f * std::sin(selection_pulse_ * 3.0f);
      draw->AddRectFilled(ImVec2(min.x + 14.0f, max.y - 3.0f),
                          ImVec2(max.x - 14.0f, max.y),
                          ImGui::GetColorU32(ImVec4(1.0f, 0.45f, 0.12f, pulse)), 2.0f);
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
  }
}

void NativeGameMenu::DrawOptionsPanel() {
  const float total_width = ImGui::GetContentRegionAvail().x;
  const float gap = 16.0f;
  const float left_width = std::floor((total_width - gap) * 0.60f);
  ImGui::BeginChild("##OptionsPanel", ImVec2(left_width, 0.0f), false,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoInputs);
  const ImVec2 panel_min = ImGui::GetWindowPos();
  const ImVec2 panel_max(panel_min.x + ImGui::GetWindowWidth(), panel_min.y + ImGui::GetWindowHeight());
  ImGui::GetWindowDrawList()->AddRectFilled(panel_min, panel_max, IM_COL32(45, 42, 39, 246), 9.0f);

  const size_t count = RowCount(active_tab_);
  size_t& selected = selected_rows_[static_cast<size_t>(active_tab_)];
  if (selected >= count) selected = 0;

  ImGui::SetCursorPos(ImVec2(20.0f, 16.0f));
  ImGui::TextColored(ImVec4(0.82f, 0.81f, 0.79f, 1.0f), "AVAILABLE SETTINGS");
  ImGui::SetCursorPosY(48.0f);
  for (size_t row = 0; row < count; ++row) {
    const bool active = row == selected;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Header,
                            IsEnabled("generations_menu_high_contrast")
                                ? ImVec4(0.38f, 0.32f, 0.27f, 1.0f)
                                : ImVec4(0.31f, 0.27f, 0.235f, 0.99f));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    }
    const std::string id = "##option_" + std::to_string(row);
    ImGui::Selectable(id.c_str(), active, ImGuiSelectableFlags_SpanAllColumns,
                      ImVec2(0.0f, 62.0f));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (active) {
      draw->AddRectFilled(ImVec2(min.x, min.y + 8.0f),
                          ImVec2(min.x + 4.0f, max.y - 8.0f),
                          IM_COL32(255, 119, 33, 255), 2.0f);
    }
    if (row + 1 < count) {
      draw->AddLine(ImVec2(min.x + 18.0f, max.y), ImVec2(max.x - 18.0f, max.y),
                    IM_COL32(104, 98, 92, active ? 82 : 42), 1.0f);
    }
    draw->AddText(ImVec2(min.x + 22.0f, min.y + 21.0f),
                  active ? IM_COL32(255, 255, 252, 255) : IM_COL32(226, 224, 219, 255),
                  RowLabel(active_tab_, row).c_str());
    const std::string value = RowValue(active_tab_, row);
    const ImVec2 value_size = ImGui::CalcTextSize(value.c_str());
    draw->AddText(ImVec2(max.x - value_size.x - 24.0f, min.y + 21.0f),
                  active ? IM_COL32(255, 177, 96, 255) : IM_COL32(240, 235, 228, 255),
                  value.c_str());
    if (active) ImGui::SetScrollHereY(0.5f);
    ImGui::PopStyleColor();
  }
  ImGui::EndChild();
  ImGui::SameLine(0.0f, gap);
}

void NativeGameMenu::DrawDetailsPanel() {
  ImGui::BeginChild("##DetailsPanel", ImVec2(0.0f, 0.0f), true,
                    ImGuiWindowFlags_NoInputs);

  const size_t selected = selected_rows_[static_cast<size_t>(active_tab_)];
  const float panel_width = ImGui::GetContentRegionAvail().x;
  const float inner_width = std::max(120.0f, panel_width - 36.0f);

  // Keep every details section inside its own padded child. This prevents long
  // values and descriptions from touching or being clipped by the panel edge.
  ImGui::SetCursorPos(ImVec2(18.0f, 14.0f));
  ImGui::BeginChild("##SelectedOptionCard", ImVec2(inner_width, 136.0f), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                        ImGuiWindowFlags_NoInputs);
  ImGui::SetCursorPos(ImVec2(16.0f, 12.0f));
  ImGui::TextDisabled("SELECTED OPTION");
  ImGui::SetCursorPos(ImVec2(16.0f, 39.0f));
  ImGui::TextColored(ImVec4(1.0f, 0.47f, 0.11f, 1.0f), "%s",
                     RowLabel(active_tab_, selected).c_str());
  ImGui::SetCursorPos(ImVec2(16.0f, 66.0f));
  ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16.0f);
  if (IsEnabled("generations_menu_tooltips")) ImGui::TextWrapped("%s", RowDescription(active_tab_, selected));
  else ImGui::TextDisabled("Descriptions disabled in Interface settings.");
  ImGui::PopTextWrapPos();
  ImGui::EndChild();

  ImGui::SetCursorPosX(18.0f);
  ImGui::BeginChild("##CurrentValueCard", ImVec2(inner_width, 104.0f), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                        ImGuiWindowFlags_NoInputs);
  ImGui::SetCursorPos(ImVec2(16.0f, 12.0f));
  ImGui::TextDisabled("CURRENT VALUE");
  ImGui::SetCursorPos(ImVec2(16.0f, 40.0f));
  ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16.0f);
  ImGui::TextColored(ImVec4(0.42f, 0.82f, 1.0f, 1.0f), "%s",
                     RowValue(active_tab_, selected).c_str());
  if (RowLabel(active_tab_, selected) == "INTERNAL SCALE") {
    const std::string scale = rex::cvar::GetFlagByName("draw_resolution_scale_x");
    int multiplier = 1;
    if (!scale.empty() && scale[0] >= '1' && scale[0] <= '4') multiplier = scale[0] - '0';
    const int pixel_cost = multiplier * multiplier;
    ImGui::TextDisabled("Approximate pixel workload: %dx native", pixel_cost);
  }
  ImGui::PopTextWrapPos();
  ImGui::EndChild();

  const char* state = RowState(active_tab_, selected);
  const ImVec4 state_color = std::string_view(state) == "RESTART"
      ? ImVec4(1.0f, 0.56f, 0.22f, 1.0f)
      : std::string_view(state) == "ACTION"
            ? ImVec4(1.0f, 0.43f, 0.10f, 1.0f)
            : ImVec4(0.38f, 0.91f, 0.66f, 1.0f);

  ImGui::SetCursorPosX(18.0f);
  ImGui::BeginChild("##ApplyModeCard", ImVec2(inner_width, 90.0f), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                        ImGuiWindowFlags_NoInputs);
  ImGui::SetCursorPos(ImVec2(16.0f, 12.0f));
  ImGui::TextDisabled("APPLY MODE");
  ImGui::SetCursorPos(ImVec2(16.0f, 40.0f));
  ImGui::TextColored(state_color, "%s", state);
  ImGui::EndChild();

  const auto stats = graphics_system_ ? graphics_system_->guest_swap_stats()
                                      : rex::graphics::GuestSwapStats{};
  fps_history_[fps_history_cursor_++ % fps_history_.size()] =
      stats.frame_count ? static_cast<float>(stats.fps) : 0.0f;

  const float graph_height = IsEnabled("generations_menu_show_graph") ? 122.0f : 0.0f;
  const float graph_y = std::max(ImGui::GetCursorPosY() + 10.0f,
                                 ImGui::GetWindowHeight() - graph_height - 18.0f);
  if (graph_height > 0.0f) {
  ImGui::SetCursorPos(ImVec2(18.0f, graph_y));
  ImGui::BeginChild("##GuestPerformanceCard", ImVec2(inner_width, graph_height), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                        ImGuiWindowFlags_NoInputs);
  ImGui::SetCursorPos(ImVec2(16.0f, 12.0f));
  ImGui::TextDisabled("LIVE GUEST PERFORMANCE");
  ImGui::SetCursorPos(ImVec2(12.0f, 38.0f));
  ImGui::PlotLines("##GuestFps", fps_history_.data(),
                   static_cast<int>(fps_history_.size()),
                   static_cast<int>(fps_history_cursor_ % fps_history_.size()),
                   nullptr, 0.0f, 75.0f,
                   ImVec2(std::max(80.0f, ImGui::GetWindowWidth() - 24.0f), 66.0f));
  ImGui::EndChild();
  }

  ImGui::EndChild();
}

void NativeGameMenu::DrawDropdownOverlay() {
  if (!dropdown_open_) return;
  const auto& options = OptionsFor(static_cast<int>(active_tab_));
  if (dropdown_row_ >= options.size()) return;
  const OptionSpec& option = options[dropdown_row_];
  if (option.values.empty()) return;

  ImDrawList* foreground = ImGui::GetForegroundDrawList();
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const ImVec2 window_size = ImGui::GetWindowSize();
  foreground->AddRectFilled(window_pos, ImVec2(window_pos.x + window_size.x, window_pos.y + window_size.y),
                            IM_COL32(10, 8, 6, 105), 16.0f);

  const float popup_width = std::min(560.0f, window_size.x - 100.0f);
  const float row_height = 48.0f;
  const float popup_height = 88.0f + row_height * static_cast<float>(option.values.size()) + 54.0f;
  ImGui::SetNextWindowPos(ImVec2(window_pos.x + window_size.x * 0.5f,
                                window_pos.y + window_size.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(popup_width, popup_height), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.145f, 0.138f, 0.132f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.40f, 0.08f, 0.95f));
  ImGui::Begin("##SettingDropdown", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoInputs);
  ImGui::TextDisabled("SELECT OPTION");
  ImGui::TextColored(ImVec4(1.0f, 0.50f, 0.14f, 1.0f), "%s", option.label);
  ImGui::Separator();
  for (size_t i = 0; i < option.values.size(); ++i) {
    const bool active = i == dropdown_index_;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.21f, 0.18f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.56f, 0.18f, 1.0f));
    }
    const std::string id = "##dropdown_value_" + std::to_string(i);
    ImGui::Selectable(id.c_str(), active, ImGuiSelectableFlags_SpanAllColumns,
                      ImVec2(0.0f, row_height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (active) {
      ImGui::GetWindowDrawList()->AddRectFilled(
          ImVec2(min.x, min.y + 7.0f), ImVec2(min.x + 4.0f, max.y - 7.0f),
          IM_COL32(255, 110, 24, 255), 2.0f);
    }
    const std::string text = Uppercase(option.values[i]);
    ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 18.0f, min.y + 15.0f),
                                        ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    if (active) ImGui::PopStyleColor(2);
  }
  ImGui::Separator();
  ImGui::TextDisabled("DPAD  SELECT     A  APPLY     B  CANCEL");
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}

void NativeGameMenu::DrawFooter() {
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 50.0f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos();
  draw->AddRectFilled(ImVec2(p.x, p.y + ImGui::GetWindowHeight() - 50.0f),
                      ImVec2(p.x + ImGui::GetWindowWidth(), p.y + ImGui::GetWindowHeight()),
                      IM_COL32(48, 45, 42, 250), 14.0f, ImDrawFlags_RoundCornersBottom);
  draw->AddLine(ImVec2(p.x + 20.0f, p.y + ImGui::GetWindowHeight() - 50.0f),
                ImVec2(p.x + ImGui::GetWindowWidth() - 20.0f,
                       p.y + ImGui::GetWindowHeight() - 50.0f),
                IM_COL32(110, 101, 94, 90), 1.0f);
  ImGui::SetCursorPos(ImVec2(24.0f, ImGui::GetWindowHeight() - 32.0f));
  if (IsEnabled("generations_menu_show_status_bar")) {
    ImGui::TextColored(status_is_warning_ ? ImVec4(1.0f, 0.67f, 0.35f, 1.0f)
                                          : ImVec4(0.48f, 0.88f, 0.69f, 1.0f),
                       "%s", status_.c_str());
  }
  const char* controls = dropdown_open_
      ? "DPAD SELECT   A APPLY   B CANCEL"
      : "DPAD NAVIGATE   A OPEN / APPLY   B BACK   LB / RB SWITCH TAB";
  const float width = ImGui::CalcTextSize(controls).x;
  ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - width - 24.0f,
                             ImGui::GetWindowHeight() - 32.0f));
  ImGui::TextColored(ImVec4(0.78f, 0.77f, 0.75f, 1.0f), "%s", controls);
}

void NativeGameMenu::DrawLauncher(ImGuiIO& io) {
  const ImVec2 display = io.DisplaySize;
  const bool animations = IsEnabled("generations_menu_animations");
  const float speed = std::min(1.0f, io.DeltaTime * 7.5f);
  animation_progress_ = animations ? animation_progress_ + (1.0f - animation_progress_) * speed : 1.0f;
  tab_transition_ = animations ? tab_transition_ + (1.0f - tab_transition_) * std::min(1.0f, io.DeltaTime * 11.0f) : 1.0f;
  selection_pulse_ += io.DeltaTime;

  if (IsEnabled("generations_menu_dim_background")) {
    const int alpha = static_cast<int>(58.0f * animation_progress_);
    ImGui::GetForegroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), display, IM_COL32(20, 16, 12, alpha));
  }

  const ImVec2 size(std::max(1040.0f, display.x * 0.94f),
                    std::max(650.0f, display.y * 0.88f));
  const ImVec2 clamped(std::min(size.x, display.x - 28.0f),
                       std::min(size.y, display.y - 28.0f));
  const bool reduced_motion = IsEnabled("generations_menu_reduce_motion");
  const float y_offset = animations && !reduced_motion ? (1.0f - animation_progress_) * 28.0f : 0.0f;
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f + y_offset),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(clamped, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.995f * animation_progress_);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.05f, animation_progress_));
  ImGui::Begin("##GenerationsLauncher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
  if (IsEnabled("generations_menu_large_text")) ImGui::SetWindowFontScale(1.10f);
  DrawHeader(display);

  const bool two_tab_rows = !IsEnabled("generations_menu_compact_tabs") && (ImGui::GetWindowWidth() - 36.0f) < 1540.0f;
  const float nav_height = two_tab_rows ? 92.0f : 44.0f;
  ImGui::SetCursorPos(ImVec2(18.0f, 58.0f));
  ImGui::BeginChild("##TopNavigation", ImVec2(ImGui::GetWindowWidth() - 36.0f, nav_height), false, ImGuiWindowFlags_NoInputs);
  DrawTabs(); ImGui::EndChild();

  const float body_y = 68.0f + nav_height;
  const float page_offset = animations && !reduced_motion ? (1.0f - tab_transition_) * 18.0f : 0.0f;
  ImGui::SetCursorPos(ImVec2(18.0f + page_offset, body_y));
  ImGui::BeginChild("##PageBody", ImVec2(ImGui::GetWindowWidth() - 36.0f - page_offset,
                                         ImGui::GetWindowHeight() - body_y - 66.0f),
                    false, ImGuiWindowFlags_NoInputs);
  DrawOptionsPanel();
  DrawDetailsPanel();
  ImGui::EndChild();
  DrawFooter();
  DrawDropdownOverlay();
  ImGui::End();
  ImGui::PopStyleVar();
}

void NativeGameMenu::OnDraw(ImGuiIO& io) {
  ApplyImGuiStyle();
  ProcessPendingAction();
  DrawFpsOverlay();
  if (IsVisible()) DrawLauncher(io);
}

}  // namespace generations

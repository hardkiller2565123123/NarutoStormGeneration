// Embedded launcher implementation for the generations target.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

#include <toml++/toml.hpp>

#include "resource.h"
#include "launcher.h"
#include "unlock_patch_data.h"
#include "social_store.h"
#include "cpk_extractor.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr wchar_t kWindowClass[] = L"NarutoGenerationsRecompControllerLauncher";
constexpr wchar_t kWindowTitle[] = L"Naruto Generations Recomp";
constexpr UINT kFrameMessage = WM_APP + 0x51;
constexpr float kTargetFrameSeconds = 1.0f / 60.0f;
constexpr float kPageTransitionSeconds = 0.34f;
constexpr float kLaunchTransitionSeconds = 0.62f;
constexpr int kDesignWidth = 1280;
constexpr int kDesignHeight = 720;
constexpr float kPi = 3.14159265358979323846f;

enum class Page : uint8_t { kHome, kFriends, kSettings, kMods, kOptions, kAbout };
enum class Direction : int { kBackward = -1, kForward = 1 };

struct LauncherSettings {
  bool skip_launcher = false;
};

struct GameSettings {
  int display_mode = 1;
  int monitor = 0;
  int resolution = 1;
  int internal_scale = 1;
  int post_aa = 1;
  int texture_filter = 2;
  bool present_vsync = true;
  bool shadow_compatibility = true;
  bool show_fps = false;
  bool mods_enabled = true;
  bool replay_autosave = true;
  bool achievement_toasts = true;
  bool fast_startup = true;
};


struct ToastNotification {
  std::wstring title;
  std::wstring message;
  float age = 0.0f;
  float duration = 4.35f;
};

struct Particle {
  float x = 0.0f;
  float y = 0.0f;
  float speed = 0.0f;
  float size = 0.0f;
  float phase = 0.0f;
  bool blue = false;
};

struct MenuEntry {
  const wchar_t* label;
  const wchar_t* detail;
};

struct ControllerState {
  WORD buttons = 0;
  int axis_x = 0;
  int axis_y = 0;
  bool connected = false;
};

constexpr USHORT kSonyVendorId = 0x054C;

bool IsDualSenseProduct(USHORT product_id) {
  switch (product_id) {
    case 0x0CE6:  // DualSense
    case 0x0DF2:  // DualSense Edge
      return true;
    default:
      return false;
  }
}

int AxisDirection(BYTE value) {
  return value < 70 ? -1 : (value > 185 ? 1 : 0);
}

void AddDpadButtons(BYTE hat, WORD* buttons) {
  if (!buttons) return;
  switch (hat & 0x0F) {
    case 0: *buttons |= XINPUT_GAMEPAD_DPAD_UP; break;
    case 1: *buttons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 2: *buttons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 3: *buttons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 4: *buttons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 5: *buttons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 6: *buttons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 7: *buttons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT; break;
    default: break;
  }
}

std::wstring GetExecutableDirectory() {
  std::array<wchar_t, 32768> path{};
  const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                          static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) {
    return L".";
  }
  return fs::path(std::wstring(path.data(), length)).parent_path().wstring();
}

std::wstring ToWide(std::string_view value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), length);
  return result;
}

template <typename T>
T TomlValue(const toml::table& table, std::string_view name, T fallback) {
  if (const auto value = table[name].value<T>()) return *value;
  return fallback;
}

toml::table LoadToml(const fs::path& path) {
  if (!fs::is_regular_file(path)) return {};
  try { return toml::parse_file(path.string()); } catch (...) { return {}; }
}

bool SaveTomlAtomically(const fs::path& path, const toml::table& table) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  const fs::path temporary = path.wstring() + L".tmp";
  {
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream) return false;
    stream << "# Naruto Generations Recomp settings\n" << table << '\n';
    if (!stream) return false;
  }
  return MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool IsProcessRunning(std::wstring_view executable_name) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, std::wstring(executable_name).c_str()) == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

std::optional<std::vector<uint8_t>> ReadBinaryFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) return std::nullopt;
  const std::streamoff size = stream.tellg();
  if (size <= 0 || size > 128 * 1024 * 1024) return std::nullopt;
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  stream.seekg(0, std::ios::beg);
  stream.read(reinterpret_cast<char*>(bytes.data()), size);
  return stream ? std::optional<std::vector<uint8_t>>(std::move(bytes)) : std::nullopt;
}

bool SaveBinaryAtomically(const fs::path& path, const std::vector<uint8_t>& bytes) {
  const fs::path temporary = path.wstring() + L".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) return false;
  }
  return MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int HexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool ApplyConsensusUnlockPatch(std::vector<uint8_t>* save) {
  constexpr size_t kRyoOffset = 0x11C68;
  constexpr std::array<uint8_t, 4> kMaxRyo = {0x00, 0x98, 0x96, 0x7F};
  const std::string_view patch = generations::launcher::kUnlockConsensusPatchHex;
  if (!save || patch.size() % 6 != 0 || save->size() < 0x12400) return false;
  for (size_t cursor = 0; cursor < patch.size(); cursor += 6) {
    uint32_t relative_offset = 0;
    for (size_t digit = 0; digit < 4; ++digit) {
      const int nibble = HexNibble(patch[cursor + digit]);
      if (nibble < 0) return false;
      relative_offset = (relative_offset << 4) | nibble;
    }
    const int high = HexNibble(patch[cursor + 4]);
    const int low = HexNibble(patch[cursor + 5]);
    const size_t offset = generations::launcher::kUnlockPatchBase + relative_offset;
    if (high < 0 || low < 0 || offset >= save->size()) return false;
    (*save)[offset] = static_cast<uint8_t>((high << 4) | low);
  }
  std::copy(kMaxRyo.begin(), kMaxRyo.end(), save->begin() + kRyoOffset);
  return true;
}

int IndexOf(std::string_view value, const std::vector<std::string_view>& values,
            int fallback) {
  const auto found = std::find(values.begin(), values.end(), value);
  return found == values.end() ? fallback : static_cast<int>(found - values.begin());
}


void SetHighResolutionTimer(bool enabled) {
  using TimePeriodFn = UINT (WINAPI*)(UINT);
  static HMODULE winmm = LoadLibraryW(L"winmm.dll");
  if (!winmm) return;
  const char* name = enabled ? "timeBeginPeriod" : "timeEndPeriod";
  const auto fn = reinterpret_cast<TimePeriodFn>(GetProcAddress(winmm, name));
  if (fn) fn(1);
}

float EaseOutCubic(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  const float inverse = 1.0f - value;
  return 1.0f - inverse * inverse * inverse;
}

float EaseInOut(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

std::unique_ptr<Gdiplus::Bitmap> LoadPngResource(HINSTANCE instance, int id) {
  HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
  if (!resource) return {};
  HGLOBAL loaded = LoadResource(instance, resource);
  if (!loaded) return {};
  const DWORD size = SizeofResource(instance, resource);
  const void* data = LockResource(loaded);
  if (!data || !size) return {};
  HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!copy) return {};
  void* destination = GlobalLock(copy);
  std::memcpy(destination, data, size);
  GlobalUnlock(copy);
  IStream* stream = nullptr;
  if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK) {
    GlobalFree(copy);
    return {};
  }
  auto image = std::make_unique<Gdiplus::Bitmap>(stream, FALSE);
  stream->Release();
  if (image->GetLastStatus() != Gdiplus::Ok) return {};
  return image;
}

class LauncherWindow {
 public:
  LauncherWindow(HINSTANCE instance, bool auto_launch = false)
      : instance_(instance), root_(GetExecutableDirectory()),
        config_path_(root_ / L"generations.toml"),
        launcher_config_path_(root_ / L"launcher.toml"),
        auto_launch_(auto_launch), social_store_(root_ / L"social") {}

  bool Run(int show_command) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &input, nullptr) != Gdiplus::Ok)
      return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = &LauncherWindow::WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = nullptr;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return false;

    RECT bounds{0, 0, kDesignWidth, kDesignHeight};
    AdjustWindowRect(&bounds, WS_POPUP, FALSE);
    window_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle,
                              WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                              bounds.right - bounds.left, bounds.bottom - bounds.top,
                              nullptr, nullptr, instance_, this);
    if (!window_) return false;

    RegisterPlayStationControllers();
    LoadSettings();
    social_store_.EnsureLayout();
    social_store_.SeedPreviewData();
    friends_ = social_store_.LoadFriends();
    QueueToast(L"FRIENDS ONLINE", L"Fuijin is waiting in Player Match");
    background_ = LoadPngResource(instance_, IDR_LAUNCHER_BACKGROUND);
    BuildStaticBackground();
    InitializeParticles();
    CenterWindow();
    ShowCursor(FALSE);
    ShowWindow(window_, show_command);
    UpdateWindow(window_);
    SetHighResolutionTimer(true);
    last_frame_ = Clock::now();
    StartFrameThread();
    if (auto_launch_) BeginPlayAnimation();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    StopFrameThread();
    if (import_thread_.joinable()) import_thread_.join();
    ShowCursor(TRUE);
    static_background_.reset();
    background_.reset();
    SetHighResolutionTimer(false);
    Gdiplus::GdiplusShutdown(gdiplus_token_);
    if (uninitialize_com) CoUninitialize();
    return launched_;
  }

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
    LauncherWindow* self = nullptr;
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<LauncherWindow*>(create->lpCreateParams);
      self->window_ = window;
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
  }

  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_INPUT:
        ProcessRawControllerInput(reinterpret_cast<HRAWINPUT>(lparam));
        return 0;
      case kFrameMessage:
        Tick();
        return 0;
      case WM_PAINT:
        Paint();
        return 0;
      case WM_ERASEBKGND:
        return TRUE;
      case WM_SETCURSOR:
        SetCursor(nullptr);
        return TRUE;
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
      case WM_CHAR:
        return 0;  // Controller-only launcher: keyboard input is intentionally ignored.
      case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
      case WM_DESTROY:
        StopFrameThread();
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
  }

  void CenterWindow() {
    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(window_, HWND_TOP, (screen_width - kDesignWidth) / 2,
                 (screen_height - kDesignHeight) / 2, kDesignWidth, kDesignHeight,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }


  void StartFrameThread() {
    frame_thread_running_.store(true, std::memory_order_release);
    frame_thread_ = std::thread([this]() {
      using namespace std::chrono;
      auto next = steady_clock::now();
      const auto step = duration_cast<steady_clock::duration>(duration<double>(1.0 / 60.0));
      while (frame_thread_running_.load(std::memory_order_acquire)) {
        next += step;
        if (window_) PostMessageW(window_, kFrameMessage, 0, 0);
        std::this_thread::sleep_until(next);
        const auto now = steady_clock::now();
        if (now - next > milliseconds(100)) next = now;
      }
    });
  }

  void StopFrameThread() {
    frame_thread_running_.store(false, std::memory_order_release);
    if (frame_thread_.joinable()) frame_thread_.join();
  }

  void BuildStaticBackground() {
    static_background_ = std::make_unique<Gdiplus::Bitmap>(
        kDesignWidth, kDesignHeight, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(static_background_.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    if (background_) {
      const float image_w = static_cast<float>(background_->GetWidth());
      const float image_h = static_cast<float>(background_->GetHeight());
      const float scale = std::max(kDesignWidth / image_w, kDesignHeight / image_h);
      const float src_w = kDesignWidth / scale;
      const float src_h = kDesignHeight / scale;
      const float src_x = (image_w - src_w) * 0.5f;
      const float src_y = (image_h - src_h) * 0.5f;
      g.DrawImage(background_.get(), Gdiplus::Rect(0, 0, kDesignWidth, kDesignHeight),
                  src_x, src_y, src_w, src_h, Gdiplus::UnitPixel);
    } else {
      Gdiplus::SolidBrush fallback(Gdiplus::Color(255, 8, 10, 14));
      g.FillRectangle(&fallback, 0, 0, kDesignWidth, kDesignHeight);
    }

    // One continuous veil only: no midpoint gradient boundary or center seam.
    Gdiplus::SolidBrush veil(Gdiplus::Color(82, 0, 0, 0));
    g.FillRectangle(&veil, 0, 0, kDesignWidth, kDesignHeight);

  }

  void InitializeParticles() {
    std::mt19937 random(static_cast<uint32_t>(GetTickCount64()));
    std::uniform_real_distribution<float> x(0.0f, static_cast<float>(kDesignWidth));
    std::uniform_real_distribution<float> y(0.0f, static_cast<float>(kDesignHeight));
    std::uniform_real_distribution<float> speed(10.0f, 34.0f);
    std::uniform_real_distribution<float> size(1.2f, 5.5f);
    std::uniform_real_distribution<float> phase(0.0f, kPi * 2.0f);
    particles_.resize(92);
    for (size_t i = 0; i < particles_.size(); ++i) {
      particles_[i] = {x(random), y(random), speed(random), size(random),
                       phase(random), (i % 3) == 0};
    }
  }

  void LoadSettings() {
    const toml::table launcher = LoadToml(launcher_config_path_);
    launcher_settings_.skip_launcher = TomlValue(launcher, "skip_launcher", false);
    const toml::table game = LoadToml(config_path_);
    settings_.display_mode = TomlValue(game, "fullscreen", true) ? 1 : 0;
    settings_.monitor = std::clamp<int>(TomlValue<int64_t>(game, "monitor", 0), 0, 5);
    settings_.resolution = IndexOf(TomlValue(game, "resolution", std::string("1080p")),
                                   {"720p", "1080p", "1440p", "4k"}, 1);
    settings_.internal_scale = std::clamp<int>(TomlValue<int64_t>(game, "resolution_scale", 1) - 1, 0, 3);
    settings_.post_aa = IndexOf(TomlValue(game, "swap_post_effect", std::string("fxaa")),
                                {"none", "fxaa", "fxaa_extreme"}, 1);
    const int filter = static_cast<int>(TomlValue<int64_t>(game, "anisotropic_override", 5));
    settings_.texture_filter = filter >= 5 ? 2 : (filter >= 3 ? 1 : 0);
    settings_.present_vsync = TomlValue(game, "present_vsync", true);
    settings_.shadow_compatibility = TomlValue(game, "generations_shadow_compatibility", true);
    settings_.show_fps = TomlValue(game, "generations_show_game_fps", false);
    settings_.mods_enabled = TomlValue(game, "generations_mods_enabled", true);
    settings_.replay_autosave = TomlValue(game, "generations_replay_autosave", true);
    settings_.achievement_toasts = TomlValue(game, "generations_achievement_toasts", true);
    settings_.fast_startup = TomlValue(game, "generations_fast_media_verification", true);
  }

  bool SaveSettings() {
    toml::table launcher = LoadToml(launcher_config_path_);
    launcher.insert_or_assign("skip_launcher", false);
    toml::table game = LoadToml(config_path_);
    static constexpr std::array resolutions = {"720p", "1080p", "1440p", "4k"};
    static constexpr std::array anti_aliasing = {"none", "fxaa", "fxaa_extreme"};
    static constexpr std::array filters = {-1, 3, 5};
    const int scale = settings_.internal_scale + 1;
    game.insert_or_assign("fullscreen", settings_.display_mode == 1);
    game.insert_or_assign("monitor", settings_.monitor);
    game.insert_or_assign("resolution", resolutions[settings_.resolution]);
    game.insert_or_assign("resolution_scale", scale);
    game.insert_or_assign("draw_resolution_scale_x", scale);
    game.insert_or_assign("draw_resolution_scale_y", scale);
    game.insert_or_assign("swap_post_effect", anti_aliasing[settings_.post_aa]);
    game.insert_or_assign("anisotropic_override", filters[settings_.texture_filter]);
    game.insert_or_assign("present_vsync", settings_.present_vsync);
    game.insert_or_assign("vsync", true);
    game.insert_or_assign("video_mode_refresh_rate", 60.0);
    game.insert_or_assign("present_letterbox", true);
    game.insert_or_assign("present_effect", scale > 1 ? "cas" : "bilinear");
    game.insert_or_assign("present_cas_additional_sharpness", 0.3);
    game.insert_or_assign("render_target_path_d3d12", "rov");
    game.insert_or_assign("async_shader_compilation", true);
    game.insert_or_assign("store_shaders", true);
    game.insert_or_assign("host_present_from_non_ui_thread", true);
    game.insert_or_assign("occlusion_query_enable", settings_.shadow_compatibility);
    game.insert_or_assign("readback_memexport", settings_.shadow_compatibility);
    game.insert_or_assign("readback_memexport_fast", settings_.shadow_compatibility);
    game.insert_or_assign("generations_shadow_compatibility", settings_.shadow_compatibility);
    game.insert_or_assign("generations_preserve_original_timing", true);
    game.insert_or_assign("generations_show_game_fps", settings_.show_fps);
    game.insert_or_assign("generations_mods_enabled", settings_.mods_enabled);
    game.insert_or_assign("generations_replay_autosave", settings_.replay_autosave);
    game.insert_or_assign("generations_achievement_toasts", settings_.achievement_toasts);
    game.insert_or_assign("generations_fast_media_verification", settings_.fast_startup);
    std::error_code error;
    fs::create_directories(root_ / L"mods" / L"loose", error);
    fs::create_directories(root_ / L"replays" / L"local", error);
    fs::create_directories(root_ / L"achievements", error);
    return SaveTomlAtomically(launcher_config_path_, launcher) &&
           SaveTomlAtomically(config_path_, game);
  }

  void RegisterPlayStationControllers() {
    const RAWINPUTDEVICE devices[] = {
        {0x01, 0x04, RIDEV_INPUTSINK, window_},  // Joystick
        {0x01, 0x05, RIDEV_INPUTSINK, window_},  // Game pad
    };
    RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)),
                            sizeof(RAWINPUTDEVICE));
  }

  void ProcessRawControllerInput(HRAWINPUT input_handle) {
    UINT size = 0;
    if (GetRawInputData(input_handle, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 || !size) {
      return;
    }
    std::vector<BYTE> storage(size);
    if (GetRawInputData(input_handle, RID_INPUT, storage.data(), &size,
                        sizeof(RAWINPUTHEADER)) != size) {
      return;
    }
    const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(storage.data());
    if (input->header.dwType != RIM_TYPEHID) return;

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT info_size = sizeof(info);
    if (GetRawInputDeviceInfoW(input->header.hDevice, RIDI_DEVICEINFO, &info,
                               &info_size) == static_cast<UINT>(-1) ||
        info.hid.dwVendorId != kSonyVendorId) {
      return;
    }

    const BYTE* report = input->data.hid.bRawData;
    const DWORD report_size = input->data.hid.dwSizeHid;
    for (DWORD index = 0; index < input->data.hid.dwCount; ++index) {
      ParsePlayStationReport(info.hid.dwProductId, report + index * report_size,
                             report_size);
    }
  }

  void ParsePlayStationReport(USHORT product_id, const BYTE* report,
                              size_t report_size) {
    if (!report || report_size < 10) return;

    const bool dualsense = IsDualSenseProduct(product_id);
    size_t stick_offset = 0;
    size_t button_offset = 0;

    if (dualsense) {
      if (report[0] == 0x31) {  // Bluetooth DualSense
        stick_offset = 2;
        button_offset = 9;
      } else {                  // USB DualSense (report 0x01)
        stick_offset = 1;
        button_offset = 8;
      }
    } else {
      if (report[0] == 0x11) {  // Bluetooth DualShock 4
        stick_offset = 3;
        button_offset = 7;
      } else {                  // USB DualShock 4 (report 0x01)
        stick_offset = 1;
        button_offset = 5;
      }
    }

    if (stick_offset + 1 >= report_size || button_offset + 1 >= report_size)
      return;

    ControllerState state{};
    state.connected = true;
    state.axis_x = AxisDirection(report[stick_offset]);
    // PlayStation Y axes increase downward; launcher navigation expects up=1.
    state.axis_y = -AxisDirection(report[stick_offset + 1]);

    const BYTE face = report[button_offset];
    const BYTE shoulder = report[button_offset + 1];
    AddDpadButtons(face, &state.buttons);
    if (face & 0x20) state.buttons |= XINPUT_GAMEPAD_A;  // Cross
    if (face & 0x40) state.buttons |= XINPUT_GAMEPAD_B;  // Circle
    if (face & 0x10) state.buttons |= XINPUT_GAMEPAD_X;  // Square
    if (face & 0x80) state.buttons |= XINPUT_GAMEPAD_Y;  // Triangle
    if (shoulder & 0x01) state.buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (shoulder & 0x02) state.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (shoulder & 0x40) state.buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (shoulder & 0x80) state.buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    // Share/Create and Options map to Back and Start for future launcher actions.
    if (shoulder & 0x10) state.buttons |= XINPUT_GAMEPAD_BACK;
    if (shoulder & 0x20) state.buttons |= XINPUT_GAMEPAD_START;

    playstation_state_ = state;
    last_playstation_report_ = Clock::now();
  }

  void Tick() {
    const auto now = Clock::now();
    const float elapsed = std::clamp(
        std::chrono::duration<float>(now - last_frame_).count(), 0.0f, 0.033f);
    last_frame_ = now;
    PollController();

    // Drive motion from real elapsed time. A dedicated 60 Hz posting thread
    // avoids WM_TIMER coalescing, which was making visible motion feel 30 FPS.
    time_ += elapsed;
    UpdateParticles(elapsed);
    UpdateToast(elapsed);

    if (transitioning_) {
      transition_time_ += elapsed;
      if (transition_time_ >= kPageTransitionSeconds) {
        current_page_ = target_page_;
        transitioning_ = false;
        transition_time_ = 0.0f;
        selected_row_ = 0;
      }
    }

    if (launching_) {
      launch_time_ += elapsed;
      if (launch_time_ >= kLaunchTransitionSeconds) {
        if (ValidateGameFiles(true) && SaveSettings()) {
          launched_ = true;
          DestroyWindow(window_);
          return;
        }
        launching_ = false;
        launch_time_ = 0.0f;
      }
    }

    if (import_complete_.exchange(false)) {
      status_text_ = L"GAME IMPORT AND EXTRACTION COMPLETE";
      QueueToast(L"GAME READY", L"The extracted installation is now selected");
    } else if (!import_running_.load() && import_failed_.exchange(false)) {
      std::scoped_lock lock(import_status_mutex_);
      status_text_ = import_status_text_;
      MessageBeep(MB_ICONERROR);
    } else if (import_running_.load()) {
      std::scoped_lock lock(import_status_mutex_);
      const int percent = static_cast<int>(import_progress_.load() * 100.0f);
      status_text_ = L"IMPORT " + std::to_wstring(percent) + L"% - " + import_status_text_;
    }

    InvalidateRect(window_, nullptr, FALSE);
  }

  void UpdateParticles(float dt) {
    for (auto& particle : particles_) {
      particle.y -= particle.speed * dt;
      particle.x += std::sin(time_ * 0.7f + particle.phase) * 8.0f * dt;
      if (particle.y < -12.0f) {
        particle.y = static_cast<float>(kDesignHeight + 12);
        particle.x = std::fmod(particle.x + 197.0f, static_cast<float>(kDesignWidth));
      }
    }
  }

  int RowCount(Page page) const {
    switch (page) {
      case Page::kHome: return 6;
      case Page::kFriends: return std::max(1, static_cast<int>(friends_.size()));
      case Page::kSettings: return 8;
      case Page::kMods: return 4;
      case Page::kOptions: return 7;
      case Page::kAbout: return 1;
    }
    return 1;
  }

  void PollController() {
    ControllerState controller{};

    XINPUT_STATE xinput{};
    if (XInputGetState(0, &xinput) == ERROR_SUCCESS) {
      controller.connected = true;
      controller.buttons = xinput.Gamepad.wButtons;
      controller.axis_y = xinput.Gamepad.sThumbLY > 16000
                              ? 1
                              : (xinput.Gamepad.sThumbLY < -16000 ? -1 : 0);
      controller.axis_x = xinput.Gamepad.sThumbLX > 16000
                              ? 1
                              : (xinput.Gamepad.sThumbLX < -16000 ? -1 : 0);
    } else if (playstation_state_.connected &&
               Clock::now() - last_playstation_report_ <
                   std::chrono::seconds(2)) {
      controller = playstation_state_;
    }

    if (!controller.connected) {
      previous_buttons_ = 0;
      previous_axis_y_ = 0;
      previous_axis_x_ = 0;
      return;
    }

    const WORD pressed = controller.buttons & ~previous_buttons_;
    previous_buttons_ = controller.buttons;
    const int axis_y = controller.axis_y;
    const int axis_x = controller.axis_x;
    const bool up = (pressed & XINPUT_GAMEPAD_DPAD_UP) ||
                    (axis_y == 1 && previous_axis_y_ != 1);
    const bool down = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) ||
                      (axis_y == -1 && previous_axis_y_ != -1);
    const bool left = (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ||
                      (axis_x == -1 && previous_axis_x_ != -1);
    const bool right = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ||
                       (axis_x == 1 && previous_axis_x_ != 1);
    previous_axis_y_ = axis_y;
    previous_axis_x_ = axis_x;

    if (launching_ || transitioning_) return;
    if (up)
      selected_row_ =
          (selected_row_ + RowCount(current_page_) - 1) % RowCount(current_page_);
    if (down) selected_row_ = (selected_row_ + 1) % RowCount(current_page_);
    if (left) ChangeValue(-1);
    if (right) ChangeValue(1);
    if (pressed & XINPUT_GAMEPAD_A) Confirm();
    if (pressed & XINPUT_GAMEPAD_B) Back();
    if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) &&
        current_page_ != Page::kHome)
      Back();
    if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) &&
        current_page_ == Page::kHome)
      OpenPage(Page::kSettings, Direction::kForward);
  }

  void Confirm() {
    if (current_page_ == Page::kHome) {
      switch (selected_row_) {
        case 0: BeginPlayAnimation(); break;
        case 1: OpenPage(Page::kFriends, Direction::kForward); break;
        case 2: OpenPage(Page::kSettings, Direction::kForward); break;
        case 3: OpenPage(Page::kMods, Direction::kForward); break;
        case 4: OpenPage(Page::kOptions, Direction::kForward); break;
        case 5: OpenPage(Page::kAbout, Direction::kForward); break;
      }
      return;
    }
    if (current_page_ == Page::kFriends) {
      if (friends_.empty()) return;
      auto& friend_entry = friends_[static_cast<size_t>(selected_row_)];
      if (friend_entry.pending_incoming) {
        friend_entry.pending_incoming = false;
        friend_entry.presence = generations::social::PresenceState::kOnline;
        friend_entry.rich_presence = "Friend request accepted";
        social_store_.SaveFriends(friends_);
        QueueToast(L"FRIEND ADDED", ToWide(friend_entry.gamertag) + L" is now on your friends list");
      } else if (friend_entry.presence != generations::social::PresenceState::kOffline) {
        generations::social::InviteEntry invite;
        invite.id = "preview_" + std::to_string(GetTickCount64());
        invite.from_xuid = "0009000000000000";
        invite.from_gamertag = "LocalPlayer";
        invite.to_xuid = friend_entry.xuid;
        invite.session_id = "GENERATIONS_PREVIEW_SESSION";
        invite.radmin_ip = friend_entry.radmin_ip;
        invite.created_utc = "pending-runtime-hookup";
        social_store_.QueueInvite(invite);
        QueueToast(L"GAME INVITE QUEUED", ToWide(friend_entry.gamertag) + L" will receive it when social networking is connected");
      } else {
        friend_entry.favorite = !friend_entry.favorite;
        social_store_.SaveFriends(friends_);
        QueueToast(friend_entry.favorite ? L"FAVORITE ADDED" : L"FAVORITE REMOVED", ToWide(friend_entry.gamertag));
      }
      return;
    }
    if (current_page_ == Page::kSettings) {
      ChangeValue(1);
      return;
    }
    if (current_page_ == Page::kMods) {
      if (selected_row_ == 0) settings_.mods_enabled = !settings_.mods_enabled;
      else if (selected_row_ == 1) OpenFolder(root_ / L"mods");
      else if (selected_row_ == 2) settings_.replay_autosave = !settings_.replay_autosave;
      else if (selected_row_ == 3) settings_.achievement_toasts = !settings_.achievement_toasts;
      return;
    }
    if (current_page_ == Page::kOptions) {
      switch (selected_row_) {
        case 0: ApplyRecommended(); status_text_ = L"RECOMMENDED SETTINGS APPLIED"; break;
        case 1: RestoreDefaults(); status_text_ = L"DEFAULT SETTINGS RESTORED"; break;
        case 2: UnlockAllContent(); break;
        case 3: OpenFolder(root_ / L"saves"); break;
        case 4: ClearShaderCache(); break;
        case 5: StartGameImport(); break;
        case 6: OpenImportedGameFolder(); break;
      }
      return;
    }
    Back();
  }

  void Back() {
    if (current_page_ == Page::kHome) {
      DestroyWindow(window_);
      return;
    }
    SaveSettings();
    OpenPage(Page::kHome, Direction::kBackward);
  }

  void OpenPage(Page page, Direction direction) {
    target_page_ = page;
    transition_direction_ = direction;
    transition_time_ = 0.0f;
    transitioning_ = true;
  }

  void BeginPlayAnimation() {
    if (!ValidateGameFiles(true)) return;
    launching_ = true;
    launch_time_ = 0.0f;
  }

  void ChangeValue(int direction) {
    if (current_page_ != Page::kSettings) return;
    auto cycle = [direction](int& value, int count) { value = (value + direction + count) % count; };
    switch (selected_row_) {
      case 0: cycle(settings_.display_mode, 2); break;
      case 1: cycle(settings_.resolution, 4); break;
      case 2: cycle(settings_.internal_scale, 4); break;
      case 3: cycle(settings_.post_aa, 3); break;
      case 4: cycle(settings_.texture_filter, 3); break;
      case 5: settings_.present_vsync = !settings_.present_vsync; break;
      case 6: settings_.shadow_compatibility = !settings_.shadow_compatibility; break;
      case 7: settings_.show_fps = !settings_.show_fps; break;
    }
  }

  void ApplyRecommended() {
    settings_.display_mode = 1;
    settings_.resolution = 1;
    settings_.internal_scale = 1;
    settings_.post_aa = 1;
    settings_.texture_filter = 2;
    settings_.present_vsync = true;
    settings_.shadow_compatibility = true;
  }

  void RestoreDefaults() {
    settings_ = GameSettings{};
  }

  void OpenFolder(const fs::path& folder) {
    std::error_code error;
    fs::create_directories(folder, error);
    ShellExecuteW(window_, L"open", folder.c_str(), nullptr, root_.c_str(), SW_SHOWNORMAL);
  }


  std::optional<fs::path> PickGameXex() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return std::nullopt;
    COMDLG_FILTERSPEC filters[] = {{L"Xbox 360 executable", L"*.xex"},
                                   {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"Select the legally owned default.xex");
    dialog->SetFileName(L"default.xex");
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(window_))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR value = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
          result = fs::path(value);
          CoTaskMemFree(value);
        }
        item->Release();
      }
    }
    dialog->Release();
    return result;
  }

  std::optional<fs::path> PickDestinationFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose where the extracted game will be installed");
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(window_))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR value = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
          result = fs::path(value);
          CoTaskMemFree(value);
        }
        item->Release();
      }
    }
    dialog->Release();
    return result;
  }

  fs::path ImportedGamePath() const {
    const toml::table launcher = LoadToml(launcher_config_path_);
    if (const auto path = launcher["imported_game_path"].value<std::string>()) {
      return fs::path(ToWide(*path));
    }
    return {};
  }

  bool SaveImportedGamePath(const fs::path& path) {
    toml::table launcher = LoadToml(launcher_config_path_);
    const auto utf8 = path.u8string();
    launcher.insert_or_assign("imported_game_path",
                              std::string(reinterpret_cast<const char*>(utf8.data()),
                                          utf8.size()));
    launcher.insert_or_assign("skip_launcher", false);
    return SaveTomlAtomically(launcher_config_path_, launcher);
  }

  void SetImportStatus(std::wstring text) {
    std::scoped_lock lock(import_status_mutex_);
    import_status_text_ = std::move(text);
  }

  void StartGameImport() {
    if (import_running_.load()) return;
    const auto selected_xex = PickGameXex();
    if (!selected_xex) { status_text_ = L"GAME IMPORT CANCELLED"; return; }
    if (_wcsicmp(selected_xex->filename().c_str(), L"default.xex") != 0) {
      status_text_ = L"SELECT DEFAULT.XEX FROM THE ORIGINAL GAME";
      MessageBeep(MB_ICONERROR);
      return;
    }
    const fs::path source = selected_xex->parent_path();
    const fs::path data_cpk = source / L"data" / L"cpk";
    if (!fs::is_directory(data_cpk)) {
      status_text_ = L"THE SELECTED FOLDER DOES NOT CONTAIN DATA\\CPK";
      MessageBeep(MB_ICONERROR);
      return;
    }
    const auto chosen_destination = PickDestinationFolder();
    if (!chosen_destination) { status_text_ = L"GAME IMPORT CANCELLED"; return; }
    fs::path destination = *chosen_destination / L"Naruto Generations Recomp Game";
    std::error_code path_error;
    const fs::path canonical_source = fs::weakly_canonical(source, path_error);
    path_error.clear();
    const fs::path canonical_destination =
        fs::weakly_canonical(destination.parent_path(), path_error) / destination.filename();
    const auto source_text = canonical_source.native();
    const auto destination_text = canonical_destination.native();
    if (_wcsicmp(source_text.c_str(), destination_text.c_str()) == 0 ||
        (destination_text.size() > source_text.size() &&
         _wcsnicmp(destination_text.c_str(), source_text.c_str(), source_text.size()) == 0 &&
         (destination_text[source_text.size()] == L'\\' ||
          destination_text[source_text.size()] == L'/'))) {
      status_text_ = L"DESTINATION CANNOT BE INSIDE THE ORIGINAL GAME FOLDER";
      return;
    }

    import_running_.store(true);
    import_progress_.store(0.0f);
    import_files_done_.store(0);
    import_files_total_.store(0);
    status_text_ = L"IMPORTING GAME FILES...";
    SetImportStatus(L"Scanning original game files");
    if (import_thread_.joinable()) import_thread_.join();
    import_thread_ = std::thread([this, source, destination]() {
      std::error_code error;
      uint64_t total_bytes = 0;
      uint64_t copied_bytes = 0;
      uint32_t total_files = 0;
      for (fs::recursive_directory_iterator it(source,
               fs::directory_options::skip_permission_denied, error), end;
           it != end; it.increment(error)) {
        if (error) { error.clear(); continue; }
        if (it->is_regular_file(error)) {
          total_bytes += it->file_size(error);
          total_files++;
        }
      }
      import_files_total_.store(total_files);
      fs::create_directories(destination, error);
      if (error) {
        SetImportStatus(L"Could not create the selected installation folder");
        import_failed_.store(true);
        import_running_.store(false);
        return;
      }

      std::vector<fs::path> copied_cpks;
      for (fs::recursive_directory_iterator it(source,
               fs::directory_options::skip_permission_denied, error), end;
           it != end; it.increment(error)) {
        if (error) { error.clear(); continue; }
        const fs::path relative = it->path().lexically_relative(source);
        const fs::path target = destination / relative;
        if (it->is_directory(error)) {
          fs::create_directories(target, error);
          error.clear();
          continue;
        }
        if (!it->is_regular_file(error)) continue;
        fs::create_directories(target.parent_path(), error);
        error.clear();
        SetImportStatus(L"Copying " + relative.wstring());
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, error);
        if (error) {
          SetImportStatus(L"Copy failed: " + relative.wstring());
          import_failed_.store(true);
          import_running_.store(false);
          return;
        }
        const uint64_t size = it->file_size(error);
        copied_bytes += error ? 0 : size;
        error.clear();
        import_files_done_.fetch_add(1);
        import_progress_.store(total_bytes ? static_cast<float>(copied_bytes) /
                                                  static_cast<float>(total_bytes)
                                            : 1.0f);
        if (_wcsicmp(target.extension().c_str(), L".cpk") == 0)
          copied_cpks.push_back(target);
      }

      uint64_t extracted_files = 0;
      uint64_t extracted_bytes = 0;
      for (size_t index = 0; index < copied_cpks.size(); ++index) {
        const auto& cpk = copied_cpks[index];
        const fs::path relative_cpk = cpk.lexically_relative(destination);
        const fs::path output = destination / L"extracted" /
                                relative_cpk.parent_path() / cpk.stem();
        SetImportStatus(L"Reading CPK table: " + relative_cpk.wstring());
        const auto extraction = generations::launcher::cpk::ExtractArchive(
            cpk, output, [this, &relative_cpk](const generations::launcher::cpk::Progress& progress) {
              std::wstring text = L"Extracting " + relative_cpk.wstring();
              if (!progress.current_file.empty())
                text += L"\n" + progress.current_file.wstring();
              if (progress.files_total)
                text += L" (" + std::to_wstring(progress.files_done) + L"/" +
                        std::to_wstring(progress.files_total) + L")";
              SetImportStatus(std::move(text));
            });
        if (!extraction.ok) {
          SetImportStatus(L"CPK extraction failed: " + relative_cpk.wstring() +
                          L"\n" + extraction.error);
          import_failed_.store(true);
          import_running_.store(false);
          return;
        }
        extracted_files += extraction.files_extracted;
        extracted_bytes += extraction.bytes_written;
      }

      if (!SaveImportedGamePath(destination)) {
        SetImportStatus(L"Files extracted, but the installation path could not be saved");
        import_failed_.store(true);
        import_running_.store(false);
        return;
      }
      import_progress_.store(1.0f);
      SetImportStatus(L"Game import completed: " + std::to_wstring(extracted_files) +
                      L" files extracted (" + std::to_wstring(extracted_bytes / (1024 * 1024)) +
                      L" MiB)");
      import_failed_.store(false);
      import_complete_.store(true);
      import_running_.store(false);
    });
  }

  void OpenImportedGameFolder() {
    const fs::path imported = ImportedGamePath();
    if (imported.empty() || !fs::is_directory(imported)) {
      status_text_ = L"NO IMPORTED GAME INSTALLATION FOUND";
      return;
    }
    OpenFolder(imported);
  }

  void ClearShaderCache() {
    std::error_code error;
    const std::array candidates = {root_ / L"cache" / L"shaders", root_ / L"shader_cache"};
    for (const auto& path : candidates) {
      if (fs::exists(path, error)) fs::remove_all(path, error);
      error.clear();
    }
    status_text_ = L"SHADER CACHE CLEARED";
  }

  std::optional<fs::path> FindActiveSave() const {
    const toml::table profile = LoadToml(root_ / L"saves" / L"profile.toml");
    const auto xuid = profile["user_profile_xuid"].value<std::string>();
    if (!xuid || xuid->size() != 16) return std::nullopt;
    const fs::path title_root = root_ / L"saves" / ToWide(*xuid) / L"4E4D0842";
    std::error_code error;
    if (!fs::is_directory(title_root, error)) return std::nullopt;
    for (fs::recursive_directory_iterator it(title_root,
             fs::directory_options::skip_permission_denied, error), end;
         it != end; it.increment(error)) {
      if (error) { error.clear(); continue; }
      if (it->is_regular_file(error) &&
          _wcsicmp(it->path().filename().c_str(), L"STORM_G.S") == 0 &&
          it->file_size(error) >= 0x12400) return it->path();
    }
    return std::nullopt;
  }

  void UnlockAllContent() {
    if (IsProcessRunning(L"NarutoStormGeneration.exe")) {
      status_text_ = L"CLOSE THE GAME BEFORE EDITING THE SAVE";
      return;
    }
    const auto save_path = FindActiveSave();
    if (!save_path) { status_text_ = L"NO ACTIVE SAVE FOUND"; return; }
    auto save = ReadBinaryFile(*save_path);
    constexpr std::array<uint8_t, 4> magic = {0x82, 0x05, 0x6A, 0x38};
    if (!save || save->size() < 0x12400 || !std::equal(magic.begin(), magic.end(), save->begin())) {
      status_text_ = L"SAVE FORMAT DID NOT MATCH";
      return;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u_%02u%02u%02u", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond);
    const fs::path backup_dir = root_ / L"saves" / L"backups";
    std::error_code error;
    fs::create_directories(backup_dir, error);
    const fs::path backup = backup_dir / (std::wstring(L"STORM_G_before_unlock_") + stamp + L".bak");
    if (error || !fs::copy_file(*save_path, backup, fs::copy_options::none, error)) {
      status_text_ = L"BACKUP FAILED - SAVE WAS NOT CHANGED";
      return;
    }
    if (!ApplyConsensusUnlockPatch(&*save) || !SaveBinaryAtomically(*save_path, *save)) {
      status_text_ = L"UNLOCK FAILED - BACKUP IS SAFE";
      return;
    }
    status_text_ = L"ALL CONTENT UNLOCKED + MAX RYO";
  }

  bool ValidateGameFiles(bool show_error) {
    const fs::path imported = ImportedGamePath();
    if (!imported.empty() && fs::is_regular_file(imported / L"default.xex"))
      return true;
    if (fs::is_regular_file(root_ / L"default.xex")) return true;
    if (fs::is_regular_file(root_ / L"game" / L"default.xex")) return true;
    status_text_ = L"IMPORT THE ORIGINAL GAME FROM OPTIONS FIRST";
    if (show_error) MessageBeep(MB_ICONERROR);
    return false;
  }

  void Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;

    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    Gdiplus::Graphics graphics(memory);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    DrawBackground(graphics, width, height);

    const float transition_progress = transitioning_
                                          ? std::clamp(transition_time_ / kPageTransitionSeconds,
                                                       0.0f, 1.0f)
                                          : 0.0f;
    if (transitioning_) {
      const float direction =
          static_cast<float>(static_cast<int>(transition_direction_));

      // Stage the transition instead of drawing two fully visible pages over
      // each other. The outgoing page clears first, then the destination page
      // settles in. This removes the doubled headings/rows visible during the
      // old cross-slide while preserving the Storm-style movement.
      const float outgoing_progress =
          EaseInOut(std::clamp(transition_progress / 0.58f, 0.0f, 1.0f));
      const float incoming_progress = EaseOutCubic(
          std::clamp((transition_progress - 0.30f) / 0.70f, 0.0f, 1.0f));
      constexpr float kOutgoingSlide = 132.0f;
      constexpr float kIncomingSlide = 158.0f;

      if (outgoing_progress < 1.0f) {
        DrawPage(graphics, current_page_,
                 -direction * outgoing_progress * kOutgoingSlide,
                 1.0f - outgoing_progress);
      }
      if (incoming_progress > 0.0f) {
        DrawPage(graphics, target_page_,
                 direction * (1.0f - incoming_progress) * kIncomingSlide,
                 incoming_progress);
      }
    } else {
      DrawPage(graphics, current_page_, 0.0f, 1.0f);
    }

    if (launching_) DrawLaunchOverlay(graphics, width, height);
    DrawToast(graphics);
    BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window_, &paint);
  }

  void DrawBackground(Gdiplus::Graphics& g, int width, int height) {
    if (static_background_) {
      g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
      g.DrawImage(static_background_.get(), 0, 0, width, height);
    } else {
      Gdiplus::SolidBrush fallback(Gdiplus::Color(255, 8, 10, 14));
      g.FillRectangle(&fallback, 0, 0, width, height);
    }

    for (const auto& particle : particles_) {
      const float pulse = 0.58f + 0.42f * std::sin(time_ * 2.0f + particle.phase);
      const BYTE glow_alpha = static_cast<BYTE>(10 + 22 * pulse);
      const BYTE core_alpha = static_cast<BYTE>(30 + 58 * pulse);
      const float glow_size = particle.size * 3.2f;
      Gdiplus::SolidBrush glow(particle.blue ? Gdiplus::Color(glow_alpha, 42, 170, 255)
                                             : Gdiplus::Color(glow_alpha, 255, 112, 18));
      g.FillEllipse(&glow, particle.x - glow_size * 0.5f,
                    particle.y - glow_size * 0.5f, glow_size, glow_size);
      Gdiplus::SolidBrush core(particle.blue ? Gdiplus::Color(core_alpha, 95, 210, 255)
                                             : Gdiplus::Color(core_alpha, 255, 180, 55));
      g.FillEllipse(&core, particle.x, particle.y, particle.size, particle.size);
    }
  }

  void DrawPage(Gdiplus::Graphics& g, Page page, float offset_x, float alpha) {
    Gdiplus::GraphicsState state = g.Save();
    g.TranslateTransform(offset_x, 0.0f);
    if (page == Page::kHome) DrawHome(g, alpha);
    else if (page == Page::kFriends) DrawFriends(g, alpha);
    else DrawSubPage(g, page, alpha);
    g.Restore(state);
  }

  void DrawHome(Gdiplus::Graphics& g, float alpha) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font heading(&family, 18.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush headingBrush(Gdiplus::Color(220, 255, 190, 70));
    g.DrawString(L"NARUTO GENERATIONS RECOMP", -1, &heading,
                 Gdiplus::PointF(752.0f, 130.0f), &headingBrush);
    DrawMenuList(g, 748.0f, 180.0f,
      {{L"PLAY", L"ENTER THE NINJA WORLD"},
       {L"FRIENDS", L"PRESENCE, INVITES AND RADMIN"},
       {L"SETTINGS", L"GRAPHICS AND DISPLAY"},
       {L"MODS", L"LOCAL MOD CONTENT"},
       {L"OPTIONS", L"TOOLS AND SAVE FEATURES"},
       {L"ABOUT", L"PROJECT INFORMATION"}}, alpha, 455.0f, 62.0f);
    DrawFooter(g, L"A  SELECT", L"B  EXIT", L"DPAD / LEFT STICK  MOVE");
  }

  void QueueToast(std::wstring title, std::wstring message) {
    ToastNotification next{std::move(title), std::move(message), 0.0f, 4.35f};
    if (!toast_) {
      toast_ = std::move(next);
    } else {
      toast_queue_.push_back(std::move(next));
      if (toast_queue_.size() > 12) toast_queue_.pop_front();
    }
  }

  void UpdateToast(float elapsed) {
    if (!toast_) {
      if (!toast_queue_.empty()) {
        toast_ = std::move(toast_queue_.front());
        toast_queue_.pop_front();
      }
      return;
    }

    toast_->age += elapsed;
    if (toast_->age >= toast_->duration) {
      toast_.reset();
      if (!toast_queue_.empty()) {
        toast_ = std::move(toast_queue_.front());
        toast_queue_.pop_front();
      }
    }
  }

  void DrawToast(Gdiplus::Graphics& g) {
    if (!toast_) return;

    // Xbox 360 Guide-style timing: quick overshoot-free slide, long hold,
    // then a slightly faster exit. The panel stays fully opaque while held.
    constexpr float in_time = 0.24f;
    constexpr float out_time = 0.20f;
    constexpr float panel_width = 382.0f;
    constexpr float panel_height = 78.0f;
    constexpr float resting_x = 24.0f;
    constexpr float resting_y = 720.0f - panel_height - 28.0f;

    const float age = toast_->age;
    const float remaining = toast_->duration - age;
    float position = 1.0f;
    float opacity = 1.0f;
    if (age < in_time) {
      position = EaseOutCubic(std::clamp(age / in_time, 0.0f, 1.0f));
    } else if (remaining < out_time) {
      const float t = std::clamp(1.0f - remaining / out_time, 0.0f, 1.0f);
      position = 1.0f - t * t;
      opacity = std::clamp(remaining / out_time, 0.0f, 1.0f);
    }

    const float x = -panel_width - 20.0f + (resting_x + panel_width + 20.0f) * position;
    const float y = resting_y;
    const BYTE alpha = static_cast<BYTE>(255.0f * opacity);
    const float radius = 14.0f;

    Gdiplus::GraphicsState state = g.Save();
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    Gdiplus::GraphicsPath panel;
    panel.AddArc(x, y, radius * 2.0f, radius * 2.0f, 180.0f, 90.0f);
    panel.AddArc(x + panel_width - radius * 2.0f, y, radius * 2.0f, radius * 2.0f, 270.0f, 90.0f);
    panel.AddArc(x + panel_width - radius * 2.0f, y + panel_height - radius * 2.0f,
                 radius * 2.0f, radius * 2.0f, 0.0f, 90.0f);
    panel.AddArc(x, y + panel_height - radius * 2.0f, radius * 2.0f, radius * 2.0f, 90.0f, 90.0f);
    panel.CloseFigure();

    Gdiplus::GraphicsPath shadow_path;
    const float sx = x + 3.0f;
    const float sy = y + 5.0f;
    shadow_path.AddArc(sx, sy, radius * 2.0f, radius * 2.0f, 180.0f, 90.0f);
    shadow_path.AddArc(sx + panel_width - radius * 2.0f, sy, radius * 2.0f, radius * 2.0f, 270.0f, 90.0f);
    shadow_path.AddArc(sx + panel_width - radius * 2.0f, sy + panel_height - radius * 2.0f,
                       radius * 2.0f, radius * 2.0f, 0.0f, 90.0f);
    shadow_path.AddArc(sx, sy + panel_height - radius * 2.0f,
                       radius * 2.0f, radius * 2.0f, 90.0f, 90.0f);
    shadow_path.CloseFigure();
    Gdiplus::SolidBrush shadow(Gdiplus::Color(static_cast<BYTE>(128.0f * opacity), 0, 0, 0));
    g.FillPath(&shadow, &shadow_path);

    Gdiplus::LinearGradientBrush panel_fill(
        Gdiplus::PointF(x, y), Gdiplus::PointF(x, y + panel_height),
        Gdiplus::Color(alpha, 52, 54, 55), Gdiplus::Color(alpha, 17, 18, 18));
    g.FillPath(&panel_fill, &panel);

    // Thin silver Guide border with a darker inner edge.
    Gdiplus::Pen outer_rim(Gdiplus::Color(alpha, 171, 174, 176), 1.0f);
    g.DrawPath(&outer_rim, &panel);
    Gdiplus::Pen top_highlight(Gdiplus::Color(static_cast<BYTE>(145.0f * opacity), 235, 236, 237), 1.0f);
    g.DrawLine(&top_highlight, x + 17.0f, y + 1.5f, x + panel_width - 17.0f, y + 1.5f);

    // Compact Xbox sphere. No oversized glow or generic letter icon.
    const float orb_x = x + 12.0f;
    const float orb_y = y + 11.0f;
    const float orb_size = 56.0f;
    Gdiplus::LinearGradientBrush orb_fill(
        Gdiplus::PointF(orb_x, orb_y), Gdiplus::PointF(orb_x, orb_y + orb_size),
        Gdiplus::Color(alpha, 174, 222, 82), Gdiplus::Color(alpha, 52, 124, 20));
    g.FillEllipse(&orb_fill, orb_x, orb_y, orb_size, orb_size);
    Gdiplus::Pen orb_border(Gdiplus::Color(alpha, 212, 225, 204), 1.2f);
    g.DrawEllipse(&orb_border, orb_x, orb_y, orb_size, orb_size);

    // Draw the white segmented Xbox mark using curved strokes.
    Gdiplus::Pen mark(Gdiplus::Color(alpha, 247, 248, 244), 4.2f);
    mark.SetStartCap(Gdiplus::LineCapRound);
    mark.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::GraphicsPath left_mark;
    left_mark.AddBezier(orb_x + 13.0f, orb_y + 16.0f,
                        orb_x + 20.0f, orb_y + 20.0f,
                        orb_x + 24.0f, orb_y + 29.0f,
                        orb_x + 29.0f, orb_y + 43.0f);
    Gdiplus::GraphicsPath right_mark;
    right_mark.AddBezier(orb_x + 43.0f, orb_y + 16.0f,
                         orb_x + 36.0f, orb_y + 20.0f,
                         orb_x + 32.0f, orb_y + 29.0f,
                         orb_x + 27.0f, orb_y + 43.0f);
    g.DrawPath(&mark, &left_mark);
    g.DrawPath(&mark, &right_mark);

    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font header_font(&family, 12.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font body_font(&family, 16.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush header_brush(Gdiplus::Color(alpha, 205, 207, 207));
    Gdiplus::SolidBrush body_brush(Gdiplus::Color(alpha, 255, 255, 255));

    Gdiplus::RectF header_rect(x + 80.0f, y + 13.0f, panel_width - 94.0f, 19.0f);
    Gdiplus::RectF body_rect(x + 80.0f, y + 31.0f, panel_width - 94.0f, 36.0f);
    g.DrawString(toast_->title.c_str(), -1, &header_font, header_rect, nullptr, &header_brush);
    g.DrawString(toast_->message.c_str(), -1, &body_font, body_rect, nullptr, &body_brush);

    g.Restore(state);
  }

  void DrawFriends(Gdiplus::Graphics& g, float alpha) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font title_font(&family, 38.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush title_brush(Gdiplus::Color(static_cast<BYTE>(255 * alpha), 255, 185, 55));
    g.DrawString(L"FRIENDS", -1, &title_font, Gdiplus::PointF(532.0f, 50.0f), &title_brush);

    Gdiplus::SolidBrush panel(Gdiplus::Color(static_cast<BYTE>(220 * alpha), 7, 12, 20));
    Gdiplus::Pen panel_rim(Gdiplus::Color(static_cast<BYTE>(180 * alpha), 50, 145, 205), 1.2f);
    g.FillRectangle(&panel, 470, 108, 760, 530);
    g.DrawRectangle(&panel_rim, 470, 108, 760, 530);

    Gdiplus::Font name_font(&family, 19.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font small_font(&family, 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    const int visible_count = std::min<int>(6, static_cast<int>(friends_.size()));
    for (int i = 0; i < visible_count; ++i) {
      const auto& f = friends_[static_cast<size_t>(i)];
      const bool selected = i == selected_row_;
      const float y = 126.0f + i * 78.0f;
      Gdiplus::SolidBrush row(selected ? Gdiplus::Color(235, 202, 73, 16)
                                       : Gdiplus::Color(185, 12, 20, 31));
      g.FillRectangle(&row, 492.0f, y, 420.0f, 66.0f);
      Gdiplus::Pen border(selected ? Gdiplus::Color(255, 255, 176, 38)
                                   : Gdiplus::Color(130, 48, 98, 128), selected ? 2.5f : 1.0f);
      g.DrawRectangle(&border, 492.0f, y, 420.0f, 66.0f);
      const bool online = f.presence != generations::social::PresenceState::kOffline;
      Gdiplus::SolidBrush status(online ? Gdiplus::Color(255, 92, 214, 52)
                                         : Gdiplus::Color(255, 104, 108, 114));
      g.FillEllipse(&status, 510.0f, y + 20.0f, 22.0f, 22.0f);
      Gdiplus::SolidBrush white(Gdiplus::Color(255, 247, 247, 247));
      g.DrawString(ToWide(f.gamertag).c_str(), -1, &name_font, Gdiplus::PointF(546, y + 8), &white);
      Gdiplus::SolidBrush muted(Gdiplus::Color(235, 126, 205, 245));
      const std::wstring presence = ToWide(f.rich_presence);
      g.DrawString(presence.c_str(), -1, &small_font, Gdiplus::PointF(548, y + 37), &muted);
      if (f.pending_incoming) {
        Gdiplus::SolidBrush request(Gdiplus::Color(255, 255, 188, 58));
        g.DrawString(L"REQUEST", -1, &small_font, Gdiplus::PointF(825, y + 23), &request);
      }
    }

    if (!friends_.empty()) {
      const auto& f = friends_[static_cast<size_t>(std::clamp(selected_row_, 0, static_cast<int>(friends_.size()) - 1))];
      Gdiplus::SolidBrush card(Gdiplus::Color(220, 15, 24, 36));
      g.FillRectangle(&card, 932, 126, 276, 374);
      Gdiplus::SolidBrush avatar(Gdiplus::Color(255, 25, 161, 226));
      g.FillEllipse(&avatar, 1010, 150, 120, 120);
      Gdiplus::Font avatar_font(&family, 44.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
      Gdiplus::SolidBrush white(Gdiplus::Color(255, 248, 248, 248));
      const std::wstring initial = f.gamertag.empty() ? L"?" : std::wstring(1, ToWide(f.gamertag)[0]);
      g.DrawString(initial.c_str(), -1, &avatar_font, Gdiplus::PointF(1052, 181), &white);
      g.DrawString(ToWide(f.gamertag).c_str(), -1, &name_font, Gdiplus::PointF(962, 292), &white);
      Gdiplus::SolidBrush muted(Gdiplus::Color(235, 130, 205, 242));
      std::wstring network = f.radmin_ip.empty() ? L"Radmin: Not available" : L"Radmin: " + ToWide(f.radmin_ip);
      g.DrawString(network.c_str(), -1, &small_font, Gdiplus::PointF(962, 326), &muted);
      std::wstring stats = L"BP " + std::to_wstring(f.battle_points) + L"   W " + std::to_wstring(f.wins) + L"   L " + std::to_wstring(f.losses);
      g.DrawString(stats.c_str(), -1, &small_font, Gdiplus::PointF(962, 355), &muted);
      std::wstring action = f.pending_incoming ? L"A  ACCEPT REQUEST" :
                            (f.presence != generations::social::PresenceState::kOffline ? L"A  QUEUE GAME INVITE" : L"A  TOGGLE FAVORITE");
      Gdiplus::SolidBrush action_brush(Gdiplus::Color(255, 255, 186, 48));
      g.DrawString(action.c_str(), -1, &small_font, Gdiplus::PointF(962, 430), &action_brush);
      g.DrawString(L"Networking is staged only. No live service calls are active.", -1, &small_font,
                   Gdiplus::RectF(962, 462, 220, 54), nullptr, &muted);
    }
    DrawFooter(g, L"A  ACCEPT / INVITE", L"B  BACK", L"DPAD / LEFT STICK  MOVE");
  }

  void DrawSubPage(Gdiplus::Graphics& g, Page page, float alpha) {
    const wchar_t* title = L"";
    std::vector<MenuEntry> rows;
    std::array<std::wstring, 5> setting_values;
    if (page == Page::kSettings) {
      title = L"SETTINGS";
      setting_values = {ValueDisplayMode(), ValueResolution(), ValueInternalScale(),
                        ValuePostAA(), ValueTextureFilter()};
      rows = {{L"DISPLAY MODE", setting_values[0].c_str()},
              {L"OUTPUT RESOLUTION", setting_values[1].c_str()},
              {L"INTERNAL SCALE", setting_values[2].c_str()},
              {L"ANTI-ALIASING", setting_values[3].c_str()},
              {L"TEXTURE FILTERING", setting_values[4].c_str()},
              {L"VSYNC", OnOff(settings_.present_vsync)},
              {L"SHADOW COMPATIBILITY", OnOff(settings_.shadow_compatibility)},
              {L"FPS COUNTER", OnOff(settings_.show_fps)}};
    } else if (page == Page::kMods) {
      title = L"MODS";
      rows = {{L"ENABLE MODS", OnOff(settings_.mods_enabled)},
              {L"OPEN MODS FOLDER", L"ACTION"},
              {L"LOCAL MATCH RECORDS", OnOff(settings_.replay_autosave)},
              {L"ACHIEVEMENT TOASTS", OnOff(settings_.achievement_toasts)}};
    } else if (page == Page::kOptions) {
      title = L"OPTIONS";
      rows = {{L"RECOMMENDED SETTINGS", L"APPLY"},
              {L"RESTORE DEFAULTS", L"APPLY"},
              {L"UNLOCK ALL + MAX RYO", L"SAVE TOOL"},
              {L"OPEN SAVE FOLDER", L"ACTION"},
              {L"CLEAR SHADER CACHE", L"ACTION"},
              {L"IMPORT / EXTRACT GAME", L"SETUP"},
              {L"OPEN INSTALLED GAME", L"ACTION"}};
    } else {
      title = L"ABOUT";
      rows = {{L"BACK TO MAIN MENU", L"NARUTO GENERATIONS RECOMP"}};
    }

    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font title_font(&family, 38.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush title_brush(Gdiplus::Color(static_cast<BYTE>(255 * alpha), 255, 185, 55));
    g.DrawString(title, -1, &title_font, Gdiplus::PointF(700.0f, 66.0f), &title_brush);
    DrawMenuList(g, 600.0f, 126.0f, rows, alpha, 620.0f, 52.0f);
    if (!status_text_.empty()) {
      Gdiplus::Font status_font(&family, 15.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
      Gdiplus::SolidBrush status_brush(Gdiplus::Color(230, 92, 210, 255));
      g.DrawString(status_text_.c_str(), -1, &status_font, Gdiplus::PointF(610.0f, 620.0f), &status_brush);
    }
    DrawFooter(g, L"A  SELECT / CHANGE", L"B  BACK", L"LEFT / RIGHT  CHANGE VALUE");
  }


  void DrawMenuList(Gdiplus::Graphics& g, float x, float y,
                    const std::vector<MenuEntry>& entries, float alpha,
                    float width = 430.0f, float height = 62.0f) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font label_font(&family, 24.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font detail_font(&family, 12.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    for (size_t i = 0; i < entries.size(); ++i) {
      const bool selected = static_cast<int>(i) == selected_row_;
      const float item_x = x;
      const float item_y = y + static_cast<float>(i) * (height + 10.0f);
      const float pulse = 0.5f + 0.5f * std::sin(time_ * 3.0f);
      const float scale = selected ? 1.006f + 0.004f * pulse : 1.0f;
      const float draw_w = width * scale;
      const float draw_h = height * scale;
      const float draw_x = item_x - (draw_w - width) * 0.5f;
      const float draw_y = item_y - (draw_h - height) * 0.5f;
      Gdiplus::GraphicsPath path;
      path.AddRectangle(Gdiplus::RectF(draw_x, draw_y, draw_w, draw_h));
      Gdiplus::SolidBrush fill(selected ? Gdiplus::Color(235, 205, 73, 14)
                                        : Gdiplus::Color(210, 8, 14, 24));
      g.FillPath(&fill, &path);
      Gdiplus::Pen border(selected ? Gdiplus::Color(255, 255, 183, 38)
                                   : Gdiplus::Color(145, 55, 105, 138),
                           selected ? 3.0f : 1.2f);
      g.DrawPath(&border, &path);
      if (selected) {
        Gdiplus::SolidBrush accent(Gdiplus::Color(255, 30, 179, 255));
        g.FillRectangle(&accent, draw_x, draw_y, 8.0f, draw_h);
        Gdiplus::SolidBrush arrow(Gdiplus::Color(255, 255, 241, 210));
        Gdiplus::PointF points[3] = {{draw_x - 20.0f, draw_y + draw_h * 0.5f},
                                    {draw_x - 5.0f, draw_y + draw_h * 0.30f},
                                    {draw_x - 5.0f, draw_y + draw_h * 0.70f}};
        g.FillPolygon(&arrow, points, 3);
      }
      Gdiplus::SolidBrush white(Gdiplus::Color(255, 249, 247, 242));
      Gdiplus::SolidBrush muted(Gdiplus::Color(225, 115, 205, 245));
      g.DrawString(entries[i].label, -1, &label_font,
                   Gdiplus::PointF(draw_x + 25.0f, draw_y + 9.0f), &white);
      if (entries[i].detail && *entries[i].detail) {
        Gdiplus::RectF detail_rect(draw_x + draw_w * 0.50f, draw_y + 20.0f,
                                  draw_w * 0.45f, 24.0f);
        Gdiplus::StringFormat right;
        right.SetAlignment(Gdiplus::StringAlignmentFar);
        g.DrawString(entries[i].detail, -1, &detail_font, detail_rect, &right, &muted);
      }
    }
  }

  void DrawFooter(Gdiplus::Graphics& g, const wchar_t* first,
                  const wchar_t* second, const wchar_t* third) {
    Gdiplus::SolidBrush bar(Gdiplus::Color(225, 3, 7, 13));
    g.FillRectangle(&bar, 0, 674, kDesignWidth, 46);
    Gdiplus::Pen line(Gdiplus::Color(220, 0, 175, 255), 2.0f);
    g.DrawLine(&line, 0, 674, kDesignWidth, 674);
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, 14.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(245, 238, 242, 245));
    g.DrawString(first, -1, &font, Gdiplus::PointF(42.0f, 688.0f), &white);
    g.DrawString(second, -1, &font, Gdiplus::PointF(300.0f, 688.0f), &white);
    g.DrawString(third, -1, &font, Gdiplus::PointF(520.0f, 688.0f), &white);
  }

  void DrawLaunchOverlay(Gdiplus::Graphics& g, int width, int height) {
    const float progress = EaseOutCubic(launch_time_ / kLaunchTransitionSeconds);
    if (static_background_) {
      const float zoom = 1.0f + progress * 0.13f;
      const int draw_w = static_cast<int>(width * zoom);
      const int draw_h = static_cast<int>(height * zoom);
      g.DrawImage(static_background_.get(),
                  Gdiplus::Rect((width - draw_w) / 2, (height - draw_h) / 2, draw_w, draw_h));
    }
    Gdiplus::SolidBrush flash(Gdiplus::Color(
        static_cast<BYTE>(210 * std::pow(progress, 2.4f)), 255, 246, 225));
    g.FillRectangle(&flash, 0, 0, width, height);
    Gdiplus::Pen streak(Gdiplus::Color(static_cast<BYTE>(255 * progress), 255, 117, 12), 14.0f);
    const int sweep = static_cast<int>((width + 500) * progress) - 250;
    g.DrawLine(&streak, sweep - 320, height, sweep + 100, 0);
  }

  std::wstring ValueDisplayMode() const { return settings_.display_mode ? L"BORDERLESS FULLSCREEN" : L"WINDOWED"; }
  std::wstring ValueResolution() const {
    static constexpr std::array<const wchar_t*, 4> values = {L"1280 x 720", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160"};
    return values[settings_.resolution];
  }
  std::wstring ValueInternalScale() const {
    static constexpr std::array<const wchar_t*, 4> values = {L"NATIVE 1X", L"HIGH 2X", L"VERY HIGH 3X", L"ULTRA 4X"};
    return values[settings_.internal_scale];
  }
  std::wstring ValuePostAA() const {
    static constexpr std::array<const wchar_t*, 3> values = {L"OFF", L"FXAA", L"FXAA EXTREME"};
    return values[settings_.post_aa];
  }
  std::wstring ValueTextureFilter() const {
    static constexpr std::array<const wchar_t*, 3> values = {L"GAME DEFAULT", L"8X", L"16X"};
    return values[settings_.texture_filter];
  }
  static const wchar_t* OnOff(bool enabled) { return enabled ? L"ON" : L"OFF"; }

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  fs::path root_;
  fs::path config_path_;
  fs::path launcher_config_path_;
  LauncherSettings launcher_settings_;
  GameSettings settings_;
  generations::social::Store social_store_;
  std::vector<generations::social::FriendEntry> friends_;
  std::optional<ToastNotification> toast_;
  std::deque<ToastNotification> toast_queue_;
  ULONG_PTR gdiplus_token_ = 0;
  std::unique_ptr<Gdiplus::Bitmap> background_;
  std::unique_ptr<Gdiplus::Bitmap> static_background_;
  std::thread frame_thread_;
  std::atomic_bool frame_thread_running_{false};
  std::thread import_thread_;
  std::atomic_bool import_running_{false};
  std::atomic_bool import_failed_{false};
  std::atomic_bool import_complete_{false};
  std::atomic<float> import_progress_{0.0f};
  std::atomic<uint32_t> import_files_done_{0};
  std::atomic<uint32_t> import_files_total_{0};
  std::mutex import_status_mutex_;
  std::wstring import_status_text_;
  std::vector<Particle> particles_;
  Clock::time_point last_frame_{};
  float time_ = 0.0f;
  Page current_page_ = Page::kHome;
  Page target_page_ = Page::kHome;
  Direction transition_direction_ = Direction::kForward;
  bool transitioning_ = false;
  float transition_time_ = 0.0f;
  int selected_row_ = 0;
  ControllerState playstation_state_{};
  Clock::time_point last_playstation_report_{};
  WORD previous_buttons_ = 0;
  int previous_axis_y_ = 0;
  int previous_axis_x_ = 0;
  bool launching_ = false;
  float launch_time_ = 0.0f;
  bool launched_ = false;
  bool auto_launch_ = false;
  std::wstring status_text_;
};

}  // namespace

namespace generations::launcher {

bool RunLauncher() {
  wchar_t hidden_test[2]{};
  const bool hidden = GetEnvironmentVariableW(L"STORM_RECOMP_DEBUG_HIDDEN", hidden_test,
                                               static_cast<DWORD>(std::size(hidden_test))) > 0;
  wchar_t launcher_test[2]{};
  const bool auto_launch = GetEnvironmentVariableW(L"STORM_RECOMP_DEBUG_LAUNCHER_SMOKE", launcher_test,
                                                    static_cast<DWORD>(std::size(launcher_test))) > 0;
  if (hidden && !auto_launch) return true;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  LauncherWindow launcher(GetModuleHandleW(nullptr), auto_launch);
  return launcher.Run(auto_launch ? SW_HIDE : SW_SHOWNORMAL);
}

}  // namespace generations::launcher

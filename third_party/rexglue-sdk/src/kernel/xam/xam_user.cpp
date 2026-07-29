/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <charconv>
#include <limits>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xam/achievements.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string/util.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xio.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");
REXCVAR_DEFINE_BOOL(system_link_virtual_live_profile, true, "Network",
                    "Expose the local profile as signed in for system-link play only")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(system_link_allow_privileges, true, "Network",
                    "Grant multiplayer privileges to the local system-link profile")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {

constexpr u32 kSigninStateNotSignedIn = 0;
constexpr u32 kSigninStateSignedInLocally = 1;
constexpr u32 kSigninStateSignedInToLive = 2;

u32 EffectiveSigninState() {
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const u32 signin_state = user_profile->signin_state();

  // The title uses the Xbox Live sign-in state as the entry point to its online
  // menus. This is a local system-link identity only - no Xbox service is used.
  if (signin_state == kSigninStateSignedInLocally && REXCVAR_GET(system_link_virtual_live_profile)) {
    return kSigninStateSignedInToLive;
  }

  return signin_state;
}

bool HasSystemLinkPrivilege() {
  return EffectiveSigninState() == kSigninStateSignedInToLive &&
         REXCVAR_GET(system_link_allow_privileges);
}

}  // namespace

i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  assert_true(type_mask == 1 || type_mask == 2 || type_mask == 3 || type_mask == 4 ||
              type_mask == 7);
  if (!xuid_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      auto type = user_profile->type() & type_mask;
      if (type & (2 | 4)) {
        // maybe online profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      } else if (type & 1) {
        // maybe offline profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      }
    }
  } else {
    result = X_E_INVALIDARG;
  }
  *xuid_ptr = xuid;
  return result;
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  u32 signin_state = kSigninStateNotSignedIn;
  if (user_index < 4) {
    if (user_index == 0) {
      signin_state = EffectiveSigninState();
    }
  }
  return signin_state;
}

typedef struct {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> unk08;  // maybe zero?
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;  // ?
  rex::be<uint32_t> unk14;  // ?
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  info->xuid = user_profile->xuid();
  info->signin_state = EffectiveSigninState();
  rex::string::util_copy_truncating(info->name, user_profile->name(), rex::countof(info->name));
  return X_E_SUCCESS;
}

u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const auto& user_name = user_profile->name();
  rex::string::util_copy_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  auto user_name = rex::string::to_utf16(user_profile->name());
  rex::string::util_copy_and_swap_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

typedef struct {
  rex::be<uint32_t> setting_count;
  rex::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                                      be<uint64_t>* xuids, uint32_t setting_count,
                                      be<uint32_t>* setting_ids, uint32_t unk,
                                      be<uint32_t>* buffer_size_ptr, uint8_t* buffer,
                                      XAM_OVERLAPPED* overlapped) {
  if (!xuid_count) {
    if (xuids) {
      return X_ERROR_INVALID_PARAMETER;
    }
  } else {
    if (!xuids || xuid_count > 32) {
      return X_ERROR_INVALID_PARAMETER;
    }
  }
  assert_zero(unk);  // probably flags

  // must have at least 1 to 32 settings
  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // buffer size pointer must be valid
  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // if buffer size is non-zero, buffer pointer must be valid
  auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  // Title ID = 0 means us.
  // 0xfffe07d1 = profile?

  if (!xuids && user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // TODO(gibbed): setting validity checking without needing a user profile
  // object.
  for (uint32_t i = 0; i < setting_count; ++i) {
    const auto setting_id = static_cast<uint32_t>(setting_ids[i]);
    if (!user_profile->GetSetting(setting_id)) {
      REXKRNL_INFO("[XEXONLINE][PROFILE] setting {:08X} is unset; returning an empty value",
                   setting_id);
    }
  }

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr = REX_KERNEL_MEMORY()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(REX_KERNEL_MEMORY()->HostToGuestVirtual(buffer), buffer,
                                            buffer_size, needed_header_size);
  for (uint32_t n = 0; n < setting_count; ++n) {
    uint32_t setting_id = setting_ids[n];
    auto setting = user_profile->GetSetting(setting_id);

    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    out_setting->from = !setting || !setting->is_set ? 0 : setting->is_title_specific() ? 2 : 1;
    if (xuids) {
      out_setting->xuid = static_cast<uint64_t>(xuids[n / setting_count]);
    } else {
      out_setting->user_index = static_cast<uint32_t>(user_index);
    }
    out_setting->setting_id = setting_id;

    if (setting && setting->is_set) {
      setting->Append(&out_setting->data, &out_stream);
    }
    ++out_setting;
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(
        REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, mapped_u64 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                     ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, 0, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       mapped_u64 xuids, u32 setting_count, mapped_u32 setting_ids,
                                       mapped_u32 buffer_size_ptr, u32 unk_2,
                                       mapped_void buffer_ptr,
                                       ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, unk_2, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count,
                                      ppc_ptr_t<X_USER_PROFILE_SETTING> settings,
                                      ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(),
                                                      X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  // Update and save settings.
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type = static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::UNSET) {
      continue;
    }

    REXKRNL_DEBUG(
        "XamUserWriteProfileSettings: setting index [{}]:"
        " from={} setting_id={:08X} data.type={}",
        n, (uint32_t)setting.from, (uint32_t)setting.setting_id, setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::CONTENT:
      case UserProfile::Setting::Type::BINARY: {
        uint8_t* binary_ptr = REX_KERNEL_MEMORY()->TranslateVirtual(setting.data.binary.ptr);
        size_t binary_size = setting.data.binary.size;
        std::vector<uint8_t> bytes;
        if (setting.data.binary.ptr) {
          // Copy provided data
          bytes.resize(binary_size);
          std::memcpy(bytes.data(), binary_ptr, binary_size);
        } else {
          // Data pointer was NULL, so just fill with zeroes
          bytes.resize(binary_size, 0);
        }
        user_profile->AddSetting(
            std::make_unique<xam::UserProfile::BinarySetting>(setting.setting_id, bytes));
      } break;
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::DOUBLE:
      case UserProfile::Setting::Type::FLOAT:
      case UserProfile::Setting::Type::INT32:
      case UserProfile::Setting::Type::INT64:
      case UserProfile::Setting::Type::DATETIME:
      default: {
        REXKRNL_ERROR("XamUserWriteProfileSettings: Unimplemented data type {}", setting_type);
      } break;
    };
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  if (!out_value) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // checking all users?
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return X_ERROR_INVALID_PARAMETER;
    }

    if (user_index) {
      return X_ERROR_NO_SUCH_USER;
    }
  }

  // XUserCheckPrivilege reports whether the user holds the requested privilege.
  // The local system-link profile is intentionally granted access so titles can
  // reach their LAN menus without attempting an Xbox service sign-in.
  *out_value = HasSystemLinkPrivilege() ? 1 : 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetFlags_entry(u32 user_index, mapped_u32 out_flags) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetRating_entry(u32 user_index, u32 unk1, mapped_u32 out_unk2,
                                             mapped_u32 out_unk3) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionCheckAccess_entry(u32 user_index, u32 unk1, u32 unk2, u32 unk3,
                                               u32 unk4, mapped_u32 out_unk5, u32 overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}

u32 XamUserIsOnlineEnabled_entry(u32 user_index) {
  return user_index == 0 && HasSystemLinkPrivilege();
}

u32 XamUserGetMembershipTier_entry(u32 user_index) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }
  return 6 /* 6 appears to be Gold */;
}

u32 XamUserAreUsersFriends_entry(u32 user_index, u32 unk1, u32 unk2, mapped_u32 out_value,
                                 u32 overlapped_ptr) {
  uint32_t are_friends = 0;
  X_RESULT result;

  if (user_index >= 4) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      if (user_profile->signin_state() == 0) {
        result = X_ERROR_NOT_LOGGED_ON;
      } else {
        // No friends!
        are_friends = 0;
        result = X_ERROR_SUCCESS;
      }
    } else {
      // Only support user 0.
      result = X_ERROR_NO_SUCH_USER;  // if user is local -> X_ERROR_NOT_LOGGED_ON
    }
  }

  if (out_value) {
    assert_true(!overlapped_ptr);
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!out_value);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(
        overlapped_ptr, result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result), result == X_ERROR_SUCCESS ? are_friends : 0);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamShowSigninUI_entry(u32 unk, u32 unk_mask) {
  // Mask values vary. Probably matching user types? Local/remote?

  // To fix game modes that display a 4 profile signin UI (even if playing
  // alone):
  // XN_SYS_SIGNINCHANGED
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, 1);
  // Games seem to sit and loop until we trigger this notification:
  // XN_SYS_UI (off)
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);
  return X_ERROR_SUCCESS;
}

// TODO(gibbed): probably a FILETIME/LARGE_INTEGER, unknown currently
struct X_ACHIEVEMENT_UNLOCK_TIME {
  rex::be<uint32_t> unk_0;
  rex::be<uint32_t> unk_4;
};

struct X_ACHIEVEMENT_DETAILS {
  rex::be<uint32_t> id;
  rex::be<uint32_t> label_ptr;
  rex::be<uint32_t> description_ptr;
  rex::be<uint32_t> unachieved_ptr;
  rex::be<uint32_t> image_id;
  rex::be<uint32_t> gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  rex::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);

class XStaticAchievementEnumerator : public XEnumerator {
 public:
  struct AchievementDetails {
    uint32_t id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    uint32_t image_id;
    uint32_t gamerscore;
    struct {
      uint32_t unk_0;
      uint32_t unk_4;
    } unlock_time;
    uint32_t flags;
  };

  XStaticAchievementEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                               uint32_t flags)
      : XEnumerator(kernel_state, items_per_enumerate,
                    sizeof(X_ACHIEVEMENT_DETAILS) +
                        (!!(flags & 7) ? X_ACHIEVEMENT_DETAILS::kStringBufferSize : 0)),
        flags_(flags) {}

  void AppendItem(AchievementDetails item) { items_.push_back(std::move(item)); }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset = items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<uint32_t>(string_offset), &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0, o = current_item_; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr = !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr = !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time.unk_0 = item.unlock_time.unk_0;
      details[i].unlock_time.unk_4 = item.unlock_time.unk_4;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    uint32_t ptr;
    uint8_t* data;
    size_t remaining_bytes;
  };

  uint32_t AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      assert_always();
      return 0;
    }
    auto ptr = sb.ptr;
    rex::string::util_copy_and_swap_truncating(reinterpret_cast<char16_t*>(sb.data), string, count);
    sb.ptr += static_cast<uint32_t>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

 private:
  uint32_t flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

u32 XamUserCreateAchievementEnumerator_entry(u32 title_id, u32 user_index, u32 xuid, u32 flags,
                                             u32 offset, u32 count, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(entry_size) * count;
  }

  auto e = object_ref<XStaticAchievementEnumerator>(
      new XStaticAchievementEnumerator(REX_KERNEL_STATE(), count, flags));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  const util::XdbfGameData db = REX_KERNEL_STATE()->title_xdbf();

  if (db.is_valid()) {
    const XLanguage language =
        db.GetExistingLanguage(static_cast<XLanguage>(REXCVAR_GET(user_language)));
    const std::vector<util::XdbfAchievementTableEntry> achievement_list = db.GetAchievements();

    for (const util::XdbfAchievementTableEntry& entry : achievement_list) {
      constexpr uint32_t kAchievementUnlocked = 0x00020000;
      uint32_t achievement_flags = entry.flags;
      X_ACHIEVEMENT_UNLOCK_TIME unlock_time{};
      const uint32_t resolved_title_id =
          title_id ? title_id : REX_KERNEL_STATE()->title_id();
      if (const auto unlocked = GetAchievementUnlock(
              REX_KERNEL_STATE(), REX_KERNEL_STATE()->user_profile()->xuid(),
              resolved_title_id, entry.id)) {
        achievement_flags |= kAchievementUnlocked;
        unlock_time.unk_0 =
            static_cast<uint32_t>(unlocked->unlocked_at_filetime >> 32);
        unlock_time.unk_4 =
            static_cast<uint32_t>(unlocked->unlocked_at_filetime);
      }
      auto item = XStaticAchievementEnumerator::AchievementDetails{
          entry.id,
          rex::string::to_utf16(db.GetStringTableEntry(language, entry.label_id)),
          rex::string::to_utf16(db.GetStringTableEntry(language, entry.description_id)),
          rex::string::to_utf16(db.GetStringTableEntry(language, entry.unachieved_id)),
          entry.image_id,
          entry.gamerscore,
          {unlock_time.unk_0, unlock_time.unk_4},
          achievement_flags};

      e->AppendItem(item);
    }
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

namespace {

constexpr uint32_t kStatsMaxRows = 100;
constexpr uint32_t kStatsMaxSpecs = 64;
constexpr uint32_t kStatsColumnIdsPerSpec = 64;

struct X_USER_STATS_SPEC {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> num_column_ids;
  rex::be<uint16_t> column_ids[kStatsColumnIdsPerSpec];
};
static_assert_size(X_USER_STATS_SPEC, 0x88);

struct X_USER_STATS_DATA {
  uint8_t type;
  uint8_t padding[7];
  uint8_t value[8];
};
static_assert_size(X_USER_STATS_DATA, 0x10);

struct X_USER_STATS_COLUMN {
  rex::be<uint16_t> column_id;
  uint8_t padding[6];
  X_USER_STATS_DATA data;
};
static_assert_size(X_USER_STATS_COLUMN, 0x18);

struct X_USER_STATS_ROW {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> rank;
  uint8_t rank_padding[4];
  rex::be<int64_t> rating;
  char gamertag[16];
  rex::be<uint32_t> num_columns;
  rex::be<uint32_t> columns_ptr;
};
static_assert_size(X_USER_STATS_ROW, 0x30);

struct X_USER_STATS_VIEW {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> total_view_rows;
  rex::be<uint32_t> num_rows;
  rex::be<uint32_t> rows_ptr;
};
static_assert_size(X_USER_STATS_VIEW, 0x10);

struct X_USER_STATS_READ_RESULTS {
  rex::be<uint32_t> num_views;
  rex::be<uint32_t> views_ptr;
};
static_assert_size(X_USER_STATS_READ_RESULTS, 0x08);

struct LanLeaderboardPlayer {
  uint64_t xuid = 0;
  std::string gamertag;
  int64_t battle_points = 0;
  uint64_t games = 0;
  uint64_t wins = 0;
  uint64_t losses = 0;
  uint64_t draws = 0;
  uint64_t disconnects = 0;
  std::unordered_map<uint64_t, std::string> raw_stats;
  std::unordered_map<uint64_t, uint8_t> raw_types;
};

uint64_t ParseLeaderboardHex(std::string_view text) {
  uint64_t value = 0;
  for (char ch : text) {
    value <<= 4;
    if (ch >= '0' && ch <= '9') value |= uint64_t(ch - '0');
    else if (ch >= 'a' && ch <= 'f') value |= uint64_t(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') value |= uint64_t(ch - 'A' + 10);
    else return 0;
  }
  return value;
}

std::filesystem::path LanLeaderboardDatabasePath() {
  auto root = REX_KERNEL_STATE()->emulator()->user_data_root();
  if (!root.empty()) root = root.parent_path();
  return root / "network" / "generations_lan_leaderboard.toml";
}

std::vector<LanLeaderboardPlayer> LoadLanLeaderboardPlayers() {
  std::vector<LanLeaderboardPlayer> players;
  const auto path = LanLeaderboardDatabasePath();
  if (std::filesystem::exists(path)) {
    try {
      const auto document = toml::parse_file(path.string());
      if (const auto* rows = document["players"].as_array()) {
        for (const auto& node : *rows) {
          const auto* row = node.as_table();
          if (!row) continue;
          LanLeaderboardPlayer player;
          player.xuid = ParseLeaderboardHex(
              row->get("xuid")->value_or(std::string()));
          if (!player.xuid) continue;
          player.gamertag =
              row->get("gamertag")->value_or(std::string("Unknown"));
          player.battle_points =
              row->get("battle_points")->value_or<int64_t>(0);
          player.games = static_cast<uint64_t>(
              row->get("games")->value_or<int64_t>(0));
          player.wins = static_cast<uint64_t>(
              row->get("wins")->value_or<int64_t>(0));
          player.losses = static_cast<uint64_t>(
              row->get("losses")->value_or<int64_t>(0));
          player.draws = static_cast<uint64_t>(
              row->get("draws")->value_or<int64_t>(0));
          player.disconnects = static_cast<uint64_t>(
              row->get("disconnects")->value_or<int64_t>(0));
          if (const auto* raw_node = row->get("raw_stats");
              raw_node && raw_node->is_table()) {
            for (const auto& [key, value] : *raw_node->as_table()) {
              if (const auto raw_value = value.value<std::string>()) {
                player.raw_stats[ParseLeaderboardHex(std::string(key.str()))] =
                    *raw_value;
              }
            }
          }
          if (const auto* type_node = row->get("raw_stat_types");
              type_node && type_node->is_table()) {
            for (const auto& [key, value] : *type_node->as_table()) {
              if (const auto raw_type = value.value<int64_t>()) {
                player.raw_types[ParseLeaderboardHex(std::string(key.str()))] =
                    static_cast<uint8_t>(*raw_type);
              }
            }
          }
          players.push_back(std::move(player));
        }
      }
    } catch (const std::exception& exception) {
      REXKRNL_ERROR("[XEXONLINE][LEADERBOARD] Could not read '{}': {}",
                    path.string(), exception.what());
    }
  }

  // The local profile must always exist in My Rank, even before LAN discovery.
  const uint64_t local_xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  if (std::none_of(players.begin(), players.end(),
                   [local_xuid](const LanLeaderboardPlayer& player) {
                     return player.xuid == local_xuid;
                   })) {
    LanLeaderboardPlayer local;
    local.xuid = local_xuid;
    local.gamertag = REX_KERNEL_STATE()->user_profile()->name();
    players.push_back(std::move(local));
  }

  std::sort(players.begin(), players.end(),
            [](const LanLeaderboardPlayer& left,
               const LanLeaderboardPlayer& right) {
              if (left.battle_points != right.battle_points)
                return left.battle_points > right.battle_points;
              if (left.wins != right.wins) return left.wins > right.wins;
              if (left.games != right.games) return left.games > right.games;
              return left.gamertag < right.gamertag;
            });
  return players;
}

constexpr std::array<uint32_t, 12> kGenerationsLeaderboardViews = {
    3, 12, 13, 14, 26, 27, 28, 29, 30, 31, 32, 33};
constexpr std::array<uint16_t, 23> kGenerationsLeaderboardColumns = {
    1, 2, 3, 4, 5, 6, 7, 23, 8, 9, 10, 11,
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};

bool IsGenerationsLeaderboardView(uint32_t view_id) {
  return std::find(kGenerationsLeaderboardViews.begin(),
                   kGenerationsLeaderboardViews.end(), view_id) !=
         kGenerationsLeaderboardViews.end();
}

int64_t ParseLeaderboardNumber(std::string_view text) {
  int64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() ? value : 0;
}

int64_t LeaderboardColumnValue(const LanLeaderboardPlayer& player,
                               uint32_t view_id, uint16_t column_id) {
  const uint64_t exact_key = (static_cast<uint64_t>(view_id) << 32) | column_id;
  if (const auto found = player.raw_stats.find(exact_key);
      found != player.raw_stats.end()) {
    return ParseLeaderboardNumber(found->second);
  }

  // View 3 is the title's primary Ranked Match Ranking table. Keep its first
  // six fields useful for brand-new local records while never overriding a
  // value captured from the title itself.
  if (view_id == 3) {
    switch (column_id) {
      case 1: return player.battle_points;
      case 2: return static_cast<int64_t>(player.wins);
      case 3: return static_cast<int64_t>(player.losses);
      case 4: return static_cast<int64_t>(player.games);
      case 5: return static_cast<int64_t>(player.draws);
      case 6: return static_cast<int64_t>(player.disconnects);
      default: break;
    }
  }
  return 0;
}

uint8_t LeaderboardColumnType(const LanLeaderboardPlayer& player,
                              uint32_t view_id, uint16_t column_id) {
  const uint64_t key = (static_cast<uint64_t>(view_id) << 32) | column_id;
  if (const auto found = player.raw_types.find(key);
      found != player.raw_types.end() && found->second <= 7) {
    return found->second;
  }
  // The retail parser consumes these 23 ranking columns as 32-bit values.
  return 1;  // XUSER_DATA_TYPE_INT32
}

void WriteLeaderboardValue(X_USER_STATS_DATA* data, uint8_t source_type,
                           int64_t value) {
  std::memset(data, 0, sizeof(*data));
  // Strings/binary cannot be safely reconstructed without their original
  // guest buffers. Ranking views are numeric, so normalize any persisted
  // numeric source to INT32 exactly as the title's PPC parser expects.
  data->type = 1;
  const int32_t narrowed = static_cast<int32_t>(std::clamp<int64_t>(
      value, std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::max()));
  const uint32_t encoded = static_cast<uint32_t>(narrowed);
  data->value[0] = static_cast<uint8_t>(encoded >> 24);
  data->value[1] = static_cast<uint8_t>(encoded >> 16);
  data->value[2] = static_cast<uint8_t>(encoded >> 8);
  data->value[3] = static_cast<uint8_t>(encoded);
  (void)source_type;
}

}  // namespace

u32 XamUserCreateStatsEnumerator_entry(u32 title_id, u32 enumerator_type, u64 pivot_user,
                                       u32 num_rows, u32 num_stats_specs,
                                       ppc_ptr_t<X_USER_STATS_SPEC> stats_ptr,
                                       mapped_u32 buffer_size_ptr, mapped_u32 handle_ptr) {
  if (!buffer_size_ptr || !handle_ptr) return X_ERROR_INVALID_PARAMETER;
  *buffer_size_ptr = 0;
  *handle_ptr = 0;

  if (enumerator_type > 3 || !stats_ptr || !num_rows ||
      num_rows > kStatsMaxRows || !num_stats_specs ||
      num_stats_specs > kStatsMaxSpecs) {
    REXKRNL_WARN(
        "XamUserCreateStatsEnumerator rejected title={:08X} type={} "
        "pivot={:016X} rows={} specs={} ptr={:08X}",
        title_id, enumerator_type, pivot_user, num_rows, num_stats_specs,
        stats_ptr.guest_address());
    return X_ERROR_INVALID_PARAMETER;
  }

  auto all_players = LoadLanLeaderboardPlayers();
  const uint32_t total_rows = static_cast<uint32_t>(all_players.size());
  size_t start_index = 0;

  // The two title wrappers use enumerator types 0 and 1 with the same 64-bit
  // argument serving either as a pivot XUID or a 1-based rank. Resolve it by
  // value rather than assuming one wrapper always means one mode.
  if (pivot_user) {
    const auto pivot = std::find_if(
        all_players.begin(), all_players.end(),
        [pivot_user](const LanLeaderboardPlayer& player) {
          return player.xuid == pivot_user;
        });
    if (pivot != all_players.end()) {
      const size_t pivot_index =
          static_cast<size_t>(std::distance(all_players.begin(), pivot));
      const size_t before = num_rows > 1 ? (num_rows - 1) / 2 : 0;
      start_index = pivot_index > before ? pivot_index - before : 0;
    } else if (pivot_user <= std::numeric_limits<uint32_t>::max() &&
               pivot_user > 0) {
      start_index = static_cast<size_t>(pivot_user - 1);
    }
  }

  std::vector<LanLeaderboardPlayer> players;
  if (start_index < all_players.size()) {
    const size_t end_index =
        std::min(all_players.size(), start_index + static_cast<size_t>(num_rows));
    players.assign(all_players.begin() + start_index,
                   all_players.begin() + end_index);
  }

  const uint32_t views_size = num_stats_specs * sizeof(X_USER_STATS_VIEW);
  const uint32_t views_address = REX_KERNEL_MEMORY()->SystemHeapAlloc(views_size);
  if (!views_address) return X_STATUS_NO_MEMORY;
  auto* views =
      REX_KERNEL_MEMORY()->TranslateVirtual<X_USER_STATS_VIEW*>(views_address);
  std::memset(views, 0, views_size);

  for (uint32_t spec_index = 0; spec_index < num_stats_specs; ++spec_index) {
    const uint32_t requested_columns = std::min<uint32_t>(
        stats_ptr[spec_index].num_column_ids, kStatsColumnIdsPerSpec);
    std::string column_list;
    for (uint32_t column = 0; column < requested_columns; ++column) {
      if (!column_list.empty()) column_list += ",";
      column_list += fmt::format(
          "{:04X}", static_cast<uint16_t>(stats_ptr[spec_index].column_ids[column]));
    }

    const uint32_t requested_view = stats_ptr[spec_index].view_id;
    views[spec_index].view_id = requested_view;
    views[spec_index].total_view_rows = total_rows;
    if (!IsGenerationsLeaderboardView(requested_view)) {
      REXKRNL_WARN(
          "[XEXONLINE][LEADERBOARD] unknown view {:08X}; preserving requested "
          "columns with zero/default values",
          requested_view);
    }
    views[spec_index].num_rows = static_cast<uint32_t>(players.size());

    if (!players.empty()) {
      const uint32_t rows_size =
          static_cast<uint32_t>(players.size() * sizeof(X_USER_STATS_ROW));
      const uint32_t rows_address = REX_KERNEL_MEMORY()->SystemHeapAlloc(rows_size);
      auto* rows =
          REX_KERNEL_MEMORY()->TranslateVirtual<X_USER_STATS_ROW*>(rows_address);
      std::memset(rows, 0, rows_size);
      views[spec_index].rows_ptr = rows_address;

      for (size_t player_index = 0; player_index < players.size(); ++player_index) {
        const auto& player = players[player_index];
        rows[player_index].xuid = player.xuid;
        rows[player_index].rank = static_cast<uint32_t>(start_index + player_index + 1);
        rows[player_index].rating = player.battle_points;
        std::memcpy(rows[player_index].gamertag, player.gamertag.data(),
                    std::min<size_t>(sizeof(rows[player_index].gamertag) - 1,
                                     player.gamertag.size()));
        rows[player_index].num_columns = requested_columns;

        if (requested_columns) {
          const uint32_t columns_size =
              requested_columns * sizeof(X_USER_STATS_COLUMN);
          const uint32_t columns_address =
              REX_KERNEL_MEMORY()->SystemHeapAlloc(columns_size);
          auto* columns = REX_KERNEL_MEMORY()->TranslateVirtual<
              X_USER_STATS_COLUMN*>(columns_address);
          std::memset(columns, 0, columns_size);
          rows[player_index].columns_ptr = columns_address;
          for (uint32_t column = 0; column < requested_columns; ++column) {
            const uint16_t column_id =
                static_cast<uint16_t>(stats_ptr[spec_index].column_ids[column]);
            columns[column].column_id = column_id;
            WriteLeaderboardValue(
                &columns[column].data,
                LeaderboardColumnType(player, requested_view, column_id),
                LeaderboardColumnValue(player, requested_view, column_id));
          }
        }
      }
    } else {
      views[spec_index].rows_ptr = 0;
    }

    REXKRNL_INFO(
        "[XEXONLINE][LEADERBOARD] spec={} view={:08X} columns={} [{}] "
        "returned_rows={}",
        spec_index, static_cast<uint32_t>(stats_ptr[spec_index].view_id),
        requested_columns, column_list, players.size());
  }

  auto enumerator = object_ref<XStaticEnumerator<X_USER_STATS_READ_RESULTS>>(
      new XStaticEnumerator<X_USER_STATS_READ_RESULTS>(REX_KERNEL_STATE(), 1));
  auto result = enumerator->Initialize(0xFF, 0xFB, 0xB0023, 0xB0024, 0);
  if (XFAILED(result)) {
    REX_KERNEL_MEMORY()->SystemHeapFree(views_address);
    return result;
  }

  auto* output = enumerator->AppendItem();
  output->num_views = num_stats_specs;
  output->views_ptr = views_address;

  uint64_t required_size = sizeof(X_USER_STATS_READ_RESULTS) + views_size;
  for (uint32_t spec_index = 0; spec_index < num_stats_specs; ++spec_index) {
    const uint32_t requested_columns = std::min<uint32_t>(
        stats_ptr[spec_index].num_column_ids, kStatsColumnIdsPerSpec);
    required_size += players.size() * sizeof(X_USER_STATS_ROW);
    required_size += players.size() * requested_columns *
                     sizeof(X_USER_STATS_COLUMN);
  }
  *buffer_size_ptr = static_cast<uint32_t>(std::min<uint64_t>(
      required_size, std::numeric_limits<uint32_t>::max()));
  *handle_ptr = enumerator->handle();
  REXKRNL_INFO(
      "[XEXONLINE][LEADERBOARD] XamUserCreateStatsEnumerator title={:08X} "
      "type={} pivot={:016X} requested_rows={} returned_rows={} specs={} "
      "size={} -> handle={:08X}",
      title_id, enumerator_type, pivot_user, num_rows, players.size(),
      num_stats_specs, static_cast<uint32_t>(*buffer_size_ptr),
      enumerator->handle());
  return X_ERROR_SUCCESS;
}

u32 XamParseGamerTileKey_entry(mapped_u32 key_ptr, mapped_u32 out1_ptr, mapped_u32 out2_ptr,
                               mapped_u32 out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}

u32 XamReadTileToTexture_entry(u32 unknown, u32 title_id, u64 tile_id, u32 user_index,
                               mapped_void buffer_ptr, u32 stride, u32 height, u32 overlapped_ptr) {
  // TODO(gibbed): unknown=0,2,3,9
  if (!tile_id) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t size = size_t(stride) * size_t(height);
  std::memset(buffer_ptr, 0xFF, size);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamWriteGamerTile_entry(u32 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 overlapped_ptr) {
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

namespace {

// XamSessionRefObjByHandle returns this guest object to titles. Its first word
// is the XAM session handle, which titles retain and inspect after creating a
// lobby. XObject also stashes its lookup signature and handle in the dispatch
// header fields at offsets 0x08 and 0x0C, so this must reserve a full dispatch
// header even though the title only consumes the first word.
struct X_KSESSION {
  rex::be<uint32_t> handle;
  uint8_t dispatch_storage[0x0C];
};
static_assert_size(X_KSESSION, 0x10);

class XSession final : public XObject {
 public:
  static constexpr XObject::Type kObjectType = XObject::Type::Session;

  explicit XSession(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

  bool Initialize() {
    auto* guest_session = CreateNative<X_KSESSION>();
    if (!guest_session) {
      return false;
    }
    guest_session->handle = handle();
    return true;
  }
};

}  // namespace

u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  if (!handle_ptr.guest_address()) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto session = object_ref<XSession>(new XSession(REX_KERNEL_STATE()));
  if (!session->Initialize()) {
    return X_STATUS_NO_MEMORY;
  }

  const u32 handle = session->handle();
  // The XAM session wrapper balances its temporary reference with NtClose.
  // Keep the original session handle alive for the title's later queries.
  session->RetainHandle();
  REXKRNL_INFO("XamSessionCreateHandle -> handle={:08X}, object={:08X}", handle,
               session->guest_object());
  *handle_ptr = handle;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  if (!obj_ptr.guest_address()) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto session = REX_KERNEL_OBJECTS()->LookupObject<XSession>(handle);
  if (!session) {
    REXKRNL_ERROR("XamSessionRefObjByHandle({:08X}) failed", handle);
    return X_ERROR_INVALID_HANDLE;
  }

  // This synthetic guest object is an ABI token for the title; the real
  // lifetime is owned by the session handle created by XamSessionCreateHandle.
  // Storm Generations dereferences the token immediately after JoinLocal, and
  // treating that as a real handle release kills the lobby path.
  *obj_ptr = session->guest_object();
  REXKRNL_INFO("XamSessionRefObjByHandle({:08X}) -> object={:08X}", handle,
               session->guest_object());
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserGetXUID, rex::kernel::xam::XamUserGetXUID_entry)
REX_EXPORT(__imp__XamUserGetSigninState, rex::kernel::xam::XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, rex::kernel::xam::XamUserGetSigninInfo_entry)
REX_EXPORT(__imp__XamUserGetName, rex::kernel::xam::XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, rex::kernel::xam::XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamUserReadProfileSettings, rex::kernel::xam::XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx,
           rex::kernel::xam::XamUserReadProfileSettingsEx_entry)
REX_EXPORT(__imp__XamUserWriteProfileSettings, rex::kernel::xam::XamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__XamUserCheckPrivilege, rex::kernel::xam::XamUserCheckPrivilege_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetFlags,
           rex::kernel::xam::XamUserContentRestrictionGetFlags_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetRating,
           rex::kernel::xam::XamUserContentRestrictionGetRating_entry)
REX_EXPORT(__imp__XamUserContentRestrictionCheckAccess,
           rex::kernel::xam::XamUserContentRestrictionCheckAccess_entry)
REX_EXPORT(__imp__XamUserIsOnlineEnabled, rex::kernel::xam::XamUserIsOnlineEnabled_entry)
REX_EXPORT(__imp__XamUserGetMembershipTier, rex::kernel::xam::XamUserGetMembershipTier_entry)
REX_EXPORT(__imp__XamUserAreUsersFriends, rex::kernel::xam::XamUserAreUsersFriends_entry)
REX_EXPORT(__imp__XamShowSigninUI, rex::kernel::xam::XamShowSigninUI_entry)
REX_EXPORT(__imp__XamUserCreateAchievementEnumerator,
           rex::kernel::xam::XamUserCreateAchievementEnumerator_entry)
REX_EXPORT(__imp__XamUserCreateStatsEnumerator,
           rex::kernel::xam::XamUserCreateStatsEnumerator_entry)
REX_EXPORT(__imp__XamParseGamerTileKey, rex::kernel::xam::XamParseGamerTileKey_entry)
REX_EXPORT(__imp__XamReadTileToTexture, rex::kernel::xam::XamReadTileToTexture_entry)
REX_EXPORT(__imp__XamWriteGamerTile, rex::kernel::xam::XamWriteGamerTile_entry)
REX_EXPORT(__imp__XamSessionCreateHandle, rex::kernel::xam::XamSessionCreateHandle_entry)
REX_EXPORT(__imp__XamSessionRefObjByHandle, rex::kernel::xam::XamSessionRefObjByHandle_entry)

REX_EXPORT_STUB(__imp__XamUserAddRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserAllowedToPostToSocialNetwork);
REX_EXPORT_STUB(__imp__XamUserCreateAvatarAssetEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreatePlayerEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateTitlesPlayedEnumerator);
REX_EXPORT_STUB(__imp__XamUserFlushLogonQueue);
REX_EXPORT_STUB(__imp__XamUserGetAge);
REX_EXPORT_STUB(__imp__XamUserGetAgeGroup);
REX_EXPORT_STUB(__imp__XamUserGetCachedUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetDeviceId);
REX_EXPORT_STUB(__imp__XamUserGetIndexFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetMembershipTierFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineCountryFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineLanguageFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineXUIDFromOfflineXUID);
REX_EXPORT_STUB(__imp__XamUserGetReportingInfo);
REX_EXPORT_STUB(__imp__XamUserGetRequestedUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserGetUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetUserFlagsFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetUserTenure);
REX_EXPORT_STUB(__imp__XamUserGetUsersMissingAvatars);
REX_EXPORT_STUB(__imp__XamUserGetXUIDForTFA);
REX_EXPORT_STUB(__imp__XamUserInvalidateProfileSetting);
REX_EXPORT_STUB(__imp__XamUserIsGuest);
REX_EXPORT_STUB(__imp__XamUserIsLogonPreviewModeEnabled);
REX_EXPORT_STUB(__imp__XamUserIsParentalControlled);
REX_EXPORT_STUB(__imp__XamUserIsPartial);
REX_EXPORT_STUB(__imp__XamUserIsPartialProfile);
REX_EXPORT_STUB(__imp__XamUserIsUnsafeProgrammingAllowed);
REX_EXPORT_STUB(__imp__XamUserLockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserLogon);
REX_EXPORT_STUB(__imp__XamUserLogonEx);
REX_EXPORT_STUB(__imp__XamUserLookupDevice);
REX_EXPORT_STUB(__imp__XamUserNuiBind);
REX_EXPORT_STUB(__imp__XamUserNuiEnableBiometric);
REX_EXPORT_STUB(__imp__XamUserNuiGetEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForBind);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForSignin);
REX_EXPORT_STUB(__imp__XamUserNuiIsBiometricEnabled);
REX_EXPORT_STUB(__imp__XamUserNuiUnbind);
REX_EXPORT_STUB(__imp__XamUserOverrideBindingCallbacks);
REX_EXPORT_STUB(__imp__XamUserOverrideDeviceBindings);
REX_EXPORT_STUB(__imp__XamUserOverrideGlobalState);
REX_EXPORT_STUB(__imp__XamUserOverrideUserInfo);
REX_EXPORT_STUB(__imp__XamUserPrefetchProfileSettings);
REX_EXPORT_STUB(__imp__XamUserProfileSync);
REX_EXPORT_STUB(__imp__XamUserReadUserPreference);
REX_EXPORT_STUB(__imp__XamUserResetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserUnlockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserUpdateRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserValidateAvatarManifest);
REX_EXPORT_STUB(__imp__XamUserWriteUserPreference);
REX_EXPORT_STUB(__imp__XamVerifyPasscode);

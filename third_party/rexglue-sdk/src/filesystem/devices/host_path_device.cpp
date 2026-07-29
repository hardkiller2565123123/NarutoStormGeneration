/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path, bool read_only,
                               std::filesystem::path overlay_path)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      overlay_path_(std::move(overlay_path)),
      read_only_(read_only) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      REXFS_ERROR("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  if (!overlay_path_.empty() && std::filesystem::is_directory(overlay_path_)) {
    REXFS_INFO("Layered loose-file mods from {} over {}", overlay_path_.string(),
               host_path_.string());
  }

  return true;
}

void HostPathDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  auto* resolved = root_entry_->ResolvePath(path);
  if (resolved) {
    return resolved;
  }

  // Fallback to a lazy case-insensitive host lookup when an entry is missing
  // from the in-memory tree (for example because casing differs on Linux).
  auto* current_entry = static_cast<HostPathEntry*>(root_entry_.get());
  for (const auto& part : rex::string::utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }

    auto* child = current_entry->GetChild(part);
    if (!child) {
      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());
      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
        return rex::string::utf8_equal_case(rex::path_to_utf8(info.name), part);
      });
      if (match == child_infos.end()) {
        return nullptr;
      }

      auto new_child = HostPathEntry::Create(this, current_entry,
                                             current_entry->host_path() / match->name, *match);
      if (!new_child) {
        return nullptr;
      }
      child = new_child;
      current_entry->children_.push_back(std::unique_ptr<Entry>(new_child));
    }

    current_entry = static_cast<HostPathEntry*>(child);
  }

  return current_entry;
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry,
                                   const std::filesystem::path& relative_path) {
  struct LayeredEntry {
    std::optional<rex::filesystem::FileInfo> base;
    std::optional<rex::filesystem::FileInfo> overlay;
  };

  const auto normalized_name = [](const std::filesystem::path& name) {
    std::string value = rex::path_to_utf8(name);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    return value;
  };

  std::map<std::string, LayeredEntry> entries;
  for (auto& info : rex::filesystem::ListFiles(host_path_ / relative_path)) {
    entries[normalized_name(info.name)].base = std::move(info);
  }
  if (!overlay_path_.empty()) {
    for (auto& info : rex::filesystem::ListFiles(overlay_path_ / relative_path)) {
      entries[normalized_name(info.name)].overlay = std::move(info);
    }
  }

  for (auto& [name, layered] : entries) {
    // An overlay entry replaces a base entry of the same case-insensitive
    // guest name. Directories remain merged recursively.
    auto& child_info = layered.overlay ? *layered.overlay : *layered.base;
    const bool is_directory =
        child_info.type == rex::filesystem::FileInfo::Type::kDirectory;
    const auto full_path =
        (layered.overlay ? overlay_path_ : host_path_) / relative_path /
        child_info.name;
    auto child = HostPathEntry::Create(this, parent_entry, full_path, child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (is_directory) {
      PopulateEntry(child, relative_path / child_info.name);
    }
  }
}

}  // namespace rex::filesystem

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

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>
#include <rex/filesystem/read_patch.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstring>

namespace rex::filesystem {
namespace {

constexpr size_t kArchiveReadAheadSize = 32ull * 1024ull * 1024ull;
constexpr size_t kArchiveReadAheadAlignment = 2ull * 1024ull * 1024ull;
constexpr size_t kArchiveDirectReadThreshold = 8ull * 1024ull * 1024ull;
constexpr size_t kArchiveMinimumSize = 64ull * 1024ull * 1024ull;

size_t AlignDown(size_t value, size_t alignment) {
  return value & ~(alignment - 1);
}

}  // namespace

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry),
      file_handle_(std::move(file_handle)),
      read_patch_set_(FindReadPatchSet(entry->host_path())) {
  // Runtime read patches are currently registered for the large Generations
  // data.cpk archive. Restrict the aggressive cache to that read-only archive
  // so normal saves/config files remain fully coherent and inexpensive.
  FileInfo info{};
  if (read_patch_set_ && file_handle_ &&
      !(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                        FileAccess::kFileAppendData)) &&
      GetInfo(file_handle_->path(), &info) && info.type == FileInfo::Type::kFile &&
      info.total_size >= kArchiveMinimumSize) {
    file_size_ = info.total_size;
    read_cache_.resize(std::min(kArchiveReadAheadSize, file_size_));
    read_cache_enabled_ = !read_cache_.empty();
    REXLOG_INFO("Enabled {} MiB archive read-ahead cache for '{}'",
                read_cache_.size() / (1024 * 1024), file_handle_->path().string());
  }
}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  delete this;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  size_t local_bytes_read = 0;
  auto* bytes_read = out_bytes_read ? out_bytes_read : &local_bytes_read;
  *bytes_read = 0;

  if (buffer.empty()) {
    return X_STATUS_SUCCESS;
  }

  if (read_cache_enabled_ && buffer.size() <= kArchiveDirectReadThreshold) {
    std::scoped_lock cache_lock(read_cache_mutex_);

    const bool offset_valid = byte_offset <= file_size_;
    const size_t request_end =
        offset_valid && buffer.size() <= file_size_ - byte_offset
            ? byte_offset + buffer.size()
            : file_size_;
    const bool cache_hit =
        byte_offset >= read_cache_offset_ && request_end <= read_cache_offset_ + read_cache_size_;

    if (!cache_hit) {
      if (!offset_valid || byte_offset >= file_size_) {
        return X_STATUS_END_OF_FILE;
      }

      read_cache_offset_ = AlignDown(byte_offset, kArchiveReadAheadAlignment);
      const size_t available = file_size_ - read_cache_offset_;
      const size_t requested = std::min(read_cache_.size(), available);
      size_t cached_bytes = 0;
      if (!file_handle_->Read(read_cache_offset_, read_cache_.data(), requested, &cached_bytes) ||
          cached_bytes == 0) {
        read_cache_size_ = 0;
        return X_STATUS_END_OF_FILE;
      }
      read_cache_size_ = cached_bytes;

      if (read_patch_set_) {
        ApplyReadPatchSet(*read_patch_set_, read_cache_offset_,
                          std::span<uint8_t>(read_cache_.data(), read_cache_size_));
      }
    }

    if (byte_offset < read_cache_offset_ ||
        byte_offset >= read_cache_offset_ + read_cache_size_) {
      return X_STATUS_END_OF_FILE;
    }

    const size_t cache_index = byte_offset - read_cache_offset_;
    const size_t copy_size = std::min(buffer.size(), read_cache_size_ - cache_index);
    std::memcpy(buffer.data(), read_cache_.data() + cache_index, copy_size);
    *bytes_read = copy_size;
    return X_STATUS_SUCCESS;
  }

  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), bytes_read)) {
    if (read_patch_set_ && *bytes_read) {
      const size_t patched_size = std::min(*bytes_read, buffer.size());
      ApplyReadPatchSet(*read_patch_set_, byte_offset, buffer.first(patched_size));
    }
    return X_STATUS_SUCCESS;
  }
  return X_STATUS_END_OF_FILE;
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem

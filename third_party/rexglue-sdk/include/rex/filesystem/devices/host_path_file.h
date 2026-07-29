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

#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/file.h>

namespace rex::filesystem {

class HostPathEntry;
class ReadPatchSet;

class HostPathFile : public File {
 public:
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<rex::filesystem::FileHandle> file_handle);
  ~HostPathFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override;

 private:
  std::unique_ptr<rex::filesystem::FileHandle> file_handle_;
  std::shared_ptr<const ReadPatchSet> read_patch_set_;

  // Large archive read-ahead cache. Generations performs thousands of tiny,
  // mostly nearby reads from data.cpk. Serving those from one 32 MiB window
  // avoids repeated kernel transitions and repeated patch-range processing.
  std::mutex read_cache_mutex_;
  std::vector<uint8_t> read_cache_;
  size_t read_cache_offset_ = 0;
  size_t read_cache_size_ = 0;
  size_t file_size_ = 0;
  bool read_cache_enabled_ = false;
};

}  // namespace rex::filesystem

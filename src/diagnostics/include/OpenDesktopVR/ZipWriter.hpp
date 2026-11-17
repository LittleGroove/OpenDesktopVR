// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// A minimal .zip writer for the debug bundle (doc 09 "Debug bundle"):
// files are stored, not compressed, with their CRC-32, in memory. Written
// from the published ZIP file format (PKWARE APPNOTE): local headers, the
// central directory, its end record. No ZIP64: the whole archive stays
// under MaxArchiveBytes, and a file that would pass it is refused.
//
// Windows' own "Extract All" and tar open it.
//
//   ZipWriter zip;
//   zip.Add("logs/host.log", text);
//   FileUtil::WriteAtomic(path, zip.Finish());
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace OpenDesktopVR::Diagnostics {

uint32_t Crc32(std::string_view data, uint32_t crc = 0) noexcept;

class ZipWriter {
 public:
  static constexpr std::size_t MaxArchiveBytes = 256u << 20;

  // `name` uses forward slashes; the file is dated now (local time). False
  // if the archive would pass MaxArchiveBytes, or the name is unusable.
  bool Add(std::string_view name, std::string_view data);

  // The archive's bytes; the writer is empty again after
  std::string Finish();

  std::size_t FileCount() const noexcept {
    return mCount;
  }
  std::size_t Bytes() const noexcept {
    return mBody.size() + mDirectory.size();
  }

 private:
  std::string mBody;     // local headers and data
  std::string mDirectory;// central directory entries
  std::size_t mCount {0};
};

}// namespace OpenDesktopVR::Diagnostics

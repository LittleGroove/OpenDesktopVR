// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The setup program's payload (ADR-043): the program's files, each packed
// with Windows' own LZMS (the Compression API) and checked by its SHA-256,
// behind a small JSON header:
//
//   "ODVRPAK1"     8 bytes
//   header size    4 bytes, little-endian
//   header         JSON: {"version", "build",
//                         "files": [{"path", "size", "packed", "sha256"}]}
//   the files' packed bytes, in the header's order
//
// The setup program carries it as a resource (odvr-pack adds it), so
// signing the program (M12) covers it. Unpacking checks everything as if it
// came from outside (TH-9): the header's bounds, every path (relative, and
// inside the install folder), every size and every file's hash.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenDesktopVR::Install {

inline constexpr std::size_t MaxPayloadFiles = 1024;
inline constexpr std::size_t MaxPayloadFileBytes = 256u << 20;
inline constexpr std::size_t MaxPayloadBytes = 512u << 20;// all files, unpacked
inline constexpr std::size_t MaxPayloadHeaderBytes = 1u << 20;
inline constexpr std::size_t MaxPathLength = 200;
// The resource the setup program carries it in (RT_RCDATA)
inline constexpr unsigned PayloadResourceID = 101;

struct PayloadFile {
  std::string mPath;// relative, with forward slashes: "tools/odvr-mirror.exe"
  std::string mData;
};

struct Payload {
  std::string mVersion;// "0.7.0"
  std::string mBuild;// "commit 560263b on m7, built 2026-11-18 12:00 (Release)"
  std::vector<PayloadFile> mFiles;
};

// Nullopt (and why) if a path isn't safe, a file is too big, or packing fails
std::optional<std::string> Pack(const Payload& payload, std::string* error = nullptr);
std::optional<Payload> Unpack(std::string_view bytes, std::string* error = nullptr) noexcept;

// A path that stays inside the install folder: relative, forward slashes, no
// empty, "." or ".." parts, none of the characters or device names Windows
// refuses, at most MaxPathLength bytes
bool IsSafeRelativePath(std::string_view path) noexcept;

// SHA-256 as 64 lowercase hex digits (Windows' CNG); empty if it failed
std::string Sha256Hex(std::string_view data) noexcept;

}// namespace OpenDesktopVR::Install

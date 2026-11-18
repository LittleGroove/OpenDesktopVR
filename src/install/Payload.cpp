// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/JsonUtil.hpp>
#include <OpenDesktopVR/Payload.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/Version.hpp>

#include <Windows.h>

#include <bcrypt.h>
#include <compressapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <set>

namespace OpenDesktopVR::Install {

namespace {

constexpr std::string_view Magic = "ODVRPAK1";
constexpr std::size_t PreambleBytes = 12;// the magic and the header's size

// Packed with LZMS (Windows' Compression API, buffer mode); an empty file
// stays empty
std::optional<std::string> PackBytes(std::string_view data) {
  if (data.empty()) {
    return std::string {};
  }
  COMPRESSOR_HANDLE compressor = nullptr;
  if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) {
    return std::nullopt;
  }
  SIZE_T needed = 0;
  Compress(compressor, data.data(), data.size(), nullptr, 0, &needed);
  std::optional<std::string> packed;
  if (needed > 0) {
    std::string out(needed, '\0');
    SIZE_T written = 0;
    if (Compress(compressor, data.data(), data.size(), out.data(), out.size(), &written)) {
      out.resize(written);
      packed = std::move(out);
    }
  }
  CloseCompressor(compressor);
  return packed;
}

std::optional<std::string> UnpackBytes(std::string_view packed, std::size_t size) {
  if (size == 0) {
    return packed.empty() ? std::optional<std::string> {std::string {}} : std::nullopt;
  }
  if (packed.empty()) {
    return std::nullopt;
  }
  DECOMPRESSOR_HANDLE decompressor = nullptr;
  if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) {
    return std::nullopt;
  }
  std::string out(size, '\0');
  SIZE_T written = 0;
  const BOOL ok = Decompress(decompressor, packed.data(), packed.size(), out.data(), out.size(), &written);
  CloseDecompressor(decompressor);
  if (!ok || written != size) {
    return std::nullopt;
  }
  return out;
}

bool IsHex64(std::string_view text) noexcept {
  return text.size() == 64
    && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool IsReservedName(std::string_view part) noexcept {
  // The device names, with or without an extension ("NUL.txt" is refused too)
  const auto stem = part.substr(0, part.find('.'));
  std::string upper;
  for (const char c: stem) {
    upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
  }
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
    return true;
  }
  return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' && upper[3] <= '9';
}

std::string Lower(std::string_view text) {
  std::string lower;
  for (const char c: text) {
    lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  return lower;
}

}// namespace

bool IsSafeRelativePath(std::string_view path) noexcept {
  if (path.empty() || path.size() > MaxPathLength) {
    return false;
  }
  std::size_t start = 0;
  while (true) {
    const auto end = std::min(path.find('/', start), path.size());
    const auto part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' || IsReservedName(part)) {
      return false;
    }
    for (const char c: part) {
      if (static_cast<unsigned char>(c) < 32 || std::strchr("<>:\"\\|?*", c) != nullptr) {
        return false;
      }
    }
    if (end == path.size()) {
      return true;
    }
    start = end + 1;
  }
}

std::string Sha256Hex(std::string_view data) noexcept {
  std::array<unsigned char, 32> hash {};
  if (data.size() > 0xFFFF'FFFFu
      || !BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash.data(),
        static_cast<ULONG>(hash.size())))) {
    return {};
  }
  try {
    std::string hex;
    for (const auto byte: hash) {
      hex += std::format("{:02x}", byte);
    }
    return hex;
  } catch (...) {
    return {};
  }
}

std::optional<std::string> Pack(const Payload& payload, std::string* error) {
  const auto fail = [&](std::string why) -> std::optional<std::string> {
    if (error) {
      *error = std::move(why);
    }
    return std::nullopt;
  };
  if (!ParseVersion(payload.mVersion)) {
    return fail(std::format("the version '{}' isn't one", payload.mVersion));
  }
  if (payload.mFiles.empty() || payload.mFiles.size() > MaxPayloadFiles) {
    return fail(std::format("{} files (1 to {})", payload.mFiles.size(), MaxPayloadFiles));
  }
  JsonUtil::Json header = JsonUtil::Json::object();
  header["version"] = payload.mVersion;
  header["build"] = payload.mBuild;
  header["files"] = JsonUtil::Json::array();
  std::string body;
  std::set<std::string> seen;
  std::size_t total = 0;
  for (const auto& file: payload.mFiles) {
    if (!IsSafeRelativePath(file.mPath)) {
      return fail(std::format("the path '{}' isn't safe", file.mPath));
    }
    if (!seen.insert(Lower(file.mPath)).second) {
      return fail(std::format("'{}' is listed twice", file.mPath));
    }
    total += file.mData.size();
    if (file.mData.size() > MaxPayloadFileBytes || total > MaxPayloadBytes) {
      return fail(std::format("'{}' makes it too big", file.mPath));
    }
    auto packed = PackBytes(file.mData);
    if (!packed) {
      return fail(std::format("'{}' couldn't be compressed", file.mPath));
    }
    JsonUtil::Json entry = JsonUtil::Json::object();
    entry["path"] = file.mPath;
    entry["size"] = file.mData.size();
    entry["packed"] = packed->size();
    entry["sha256"] = Sha256Hex(file.mData);
    header["files"].push_back(std::move(entry));
    body += *packed;
  }
  const auto headerText = JsonUtil::Dump(header, -1);
  if (headerText.empty() || headerText.size() > MaxPayloadHeaderBytes) {
    return fail("its header is too big");
  }
  std::string bytes {Magic};
  const auto headerSize = static_cast<std::uint32_t>(headerText.size());
  for (int shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<char>((headerSize >> shift) & 0xFF));
  }
  bytes += headerText;
  bytes += body;
  return bytes;
}

std::optional<std::string> PeekVersion(std::string_view bytes) noexcept {
  try {
    if (bytes.size() < PreambleBytes || bytes.substr(0, Magic.size()) != Magic) {
      return std::nullopt;
    }
    std::uint32_t headerSize = 0;
    for (int i = 0; i < 4; ++i) {
      headerSize |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[8 + i])) << (8 * i);
    }
    if (headerSize == 0 || headerSize > MaxPayloadHeaderBytes || headerSize > bytes.size() - PreambleBytes) {
      return std::nullopt;
    }
    const auto header = JsonUtil::Parse(bytes.substr(PreambleBytes, headerSize));
    const auto version = header ? JsonUtil::Get<std::string>(*header, "version") : std::nullopt;
    return version && ParseVersion(*version) ? version : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> SetupProgramVersion(const std::filesystem::path& setup) noexcept {
  const HMODULE module = LoadLibraryExW(setup.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module) {
    return std::nullopt;
  }
  std::optional<std::string> version;
  const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(PayloadResourceID), RT_RCDATA);
  const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
  if (const void* data = loaded ? LockResource(loaded) : nullptr) {
    version = PeekVersion({static_cast<const char*>(data), SizeofResource(module, resource)});
  }
  FreeLibrary(module);
  return version;
}

std::optional<Payload> Unpack(std::string_view bytes, std::string* error) noexcept {
  const auto fail = [&](std::string_view why) -> std::optional<Payload> {
    if (error) {
      try {
        *error = why;
      } catch (...) {
      }
    }
    return std::nullopt;
  };
  try {
    if (bytes.size() < PreambleBytes || bytes.substr(0, Magic.size()) != Magic) {
      return fail("it isn't a payload");
    }
    std::uint32_t headerSize = 0;
    for (int i = 0; i < 4; ++i) {
      headerSize |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[8 + i])) << (8 * i);
    }
    if (headerSize == 0 || headerSize > MaxPayloadHeaderBytes || headerSize > bytes.size() - PreambleBytes) {
      return fail("its header's size is wrong");
    }
    const auto header = JsonUtil::Parse(bytes.substr(PreambleBytes, headerSize));
    if (!header || !header->is_object()) {
      return fail("its header isn't readable");
    }
    Payload payload;
    payload.mVersion = JsonUtil::GetOr<std::string>(*header, "version", "");
    if (!ParseVersion(payload.mVersion)) {
      return fail("its version isn't readable");
    }
    payload.mBuild =
      std::string {StringUtil::TruncateUtf8(JsonUtil::GetOr<std::string>(*header, "build", ""), MaxPathLength)};
    const auto files = JsonUtil::Get<JsonUtil::Json>(*header, "files");
    if (!files || !files->is_array() || files->empty() || files->size() > MaxPayloadFiles) {
      return fail("its file list isn't readable");
    }
    std::size_t at = PreambleBytes + headerSize;
    std::size_t total = 0;
    std::set<std::string> seen;
    for (const auto& entry: *files) {
      const auto path = JsonUtil::GetOr<std::string>(entry, "path", "");
      const auto size = JsonUtil::Get<std::uint64_t>(entry, "size");
      const auto packed = JsonUtil::Get<std::uint64_t>(entry, "packed");
      const auto sha256 = JsonUtil::GetOr<std::string>(entry, "sha256", "");
      if (!IsSafeRelativePath(path) || !size || !packed || !IsHex64(sha256)) {
        return fail("a file in its list isn't readable");
      }
      if (!seen.insert(Lower(path)).second) {
        return fail("a file is listed twice");
      }
      if (*size > MaxPayloadFileBytes || *packed > bytes.size() - at) {
        return fail(std::format("'{}' is too big for it", path));
      }
      total += static_cast<std::size_t>(*size);
      if (total > MaxPayloadBytes) {
        return fail("its files are too big");
      }
      auto data = UnpackBytes(bytes.substr(at, static_cast<std::size_t>(*packed)), static_cast<std::size_t>(*size));
      if (!data) {
        return fail(std::format("'{}' is damaged", path));
      }
      if (Sha256Hex(*data) != sha256) {
        return fail(std::format("'{}' doesn't match its checksum", path));
      }
      at += static_cast<std::size_t>(*packed);
      payload.mFiles.push_back({path, std::move(*data)});
    }
    if (at != bytes.size()) {
      return fail("it has bytes after its files");
    }
    return payload;
  } catch (...) {
    return fail("it couldn't be read (out of memory?)");
  }
}

}// namespace OpenDesktopVR::Install

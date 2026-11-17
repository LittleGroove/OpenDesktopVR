// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.

#include <OpenDesktopVR/ZipWriter.hpp>

#include <Windows.h>

#include <array>

namespace OpenDesktopVR::Diagnostics {

namespace {

// CRC-32 as ZIP uses it: reflected, polynomial 0xEDB88320
constexpr std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> table {};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[i] = value;
  }
  return table;
}
constexpr auto CrcTable = MakeCrcTable();

void Put16(std::string& out, uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void Put32(std::string& out, uint32_t value) {
  Put16(out, value & 0xFFFF);
  Put16(out, value >> 16);
}

// MS-DOS date and time, as ZIP stores them
std::pair<uint16_t, uint16_t> DosNow() noexcept {
  SYSTEMTIME now {};
  GetLocalTime(&now);
  const auto time = static_cast<uint16_t>((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
  const auto year = now.wYear < 1980 ? 0 : now.wYear - 1980;
  const auto date = static_cast<uint16_t>((year << 9) | (now.wMonth << 5) | now.wDay);
  return {time, date};
}

constexpr uint32_t LocalSignature = 0x04034B50;
constexpr uint32_t CentralSignature = 0x02014B50;
constexpr uint32_t EndSignature = 0x06054B50;
constexpr uint16_t VersionNeeded = 10;// 1.0: stored files
constexpr uint16_t Utf8Names = 1u << 11;// general purpose flag bit 11

}// namespace

uint32_t Crc32(std::string_view data, uint32_t crc) noexcept {
  crc = ~crc;
  for (const char c: data) {
    crc = CrcTable[(crc ^ static_cast<unsigned char>(c)) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

bool ZipWriter::Add(std::string_view name, std::string_view data) {
  if (name.empty() || name.size() > 0xFFFF || mCount >= 0xFFFF) {
    return false;
  }
  constexpr std::size_t HeaderBytes = 30 + 46;
  if (Bytes() + data.size() + 2 * name.size() + HeaderBytes + 22 > MaxArchiveBytes) {
    return false;
  }
  const uint32_t crc = Crc32(data);
  const auto size = static_cast<uint32_t>(data.size());
  const auto offset = static_cast<uint32_t>(mBody.size());
  const auto [time, date] = DosNow();

  Put32(mBody, LocalSignature);
  Put16(mBody, VersionNeeded);
  Put16(mBody, Utf8Names);
  Put16(mBody, 0);// stored
  Put16(mBody, time);
  Put16(mBody, date);
  Put32(mBody, crc);
  Put32(mBody, size);// compressed
  Put32(mBody, size);// uncompressed
  Put16(mBody, static_cast<uint32_t>(name.size()));
  Put16(mBody, 0);// no extra field
  mBody.append(name);
  mBody.append(data);

  Put32(mDirectory, CentralSignature);
  Put16(mDirectory, VersionNeeded);// made by: MS-DOS, 1.0
  Put16(mDirectory, VersionNeeded);
  Put16(mDirectory, Utf8Names);
  Put16(mDirectory, 0);
  Put16(mDirectory, time);
  Put16(mDirectory, date);
  Put32(mDirectory, crc);
  Put32(mDirectory, size);
  Put32(mDirectory, size);
  Put16(mDirectory, static_cast<uint32_t>(name.size()));
  Put16(mDirectory, 0);// extra
  Put16(mDirectory, 0);// comment
  Put16(mDirectory, 0);// disk
  Put16(mDirectory, 0);// internal attributes
  Put32(mDirectory, 0);// external attributes
  Put32(mDirectory, offset);
  mDirectory.append(name);
  ++mCount;
  return true;
}

std::string ZipWriter::Finish() {
  std::string out = std::move(mBody);
  const auto directoryOffset = static_cast<uint32_t>(out.size());
  out.append(mDirectory);
  Put32(out, EndSignature);
  Put16(out, 0);// this disk
  Put16(out, 0);// the directory's disk
  Put16(out, static_cast<uint32_t>(mCount));
  Put16(out, static_cast<uint32_t>(mCount));
  Put32(out, static_cast<uint32_t>(mDirectory.size()));
  Put32(out, directoryOffset);
  Put16(out, 0);// no comment
  mBody.clear();
  mDirectory.clear();
  mCount = 0;
  return out;
}

}// namespace OpenDesktopVR::Diagnostics

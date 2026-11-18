// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/ProcessUtil.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/SharedMemory.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <format>
#include <utility>

namespace OpenDesktopVR {

namespace {

SharedMemory::Status StatusFromError(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
      return SharedMemory::Status::NotFound;
    case ERROR_ALREADY_EXISTS:
      return SharedMemory::Status::AlreadyExists;
    case ERROR_ACCESS_DENIED:
      return SharedMemory::Status::AccessDenied;
    default:
      return SharedMemory::Status::Failed;
  }
}

}// namespace

SharedMemory::~SharedMemory() {
  Reset();
}

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
  : mMapping(std::exchange(other.mMapping, nullptr)),
    mView(std::exchange(other.mView, nullptr)),
    mSize(std::exchange(other.mSize, 0)) {
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
  if (this != &other) {
    Reset();
    mMapping = std::exchange(other.mMapping, nullptr);
    mView = std::exchange(other.mView, nullptr);
    mSize = std::exchange(other.mSize, 0);
  }
  return *this;
}

void SharedMemory::Reset() noexcept {
  if (mView) {
    UnmapViewOfFile(mView);
    mView = nullptr;
  }
  if (mMapping) {
    CloseHandle(mMapping);
    mMapping = nullptr;
  }
  mSize = 0;
}

SharedMemory SharedMemory::Create(std::wstring_view name, std::size_t size, Status& status) noexcept {
  SharedMemory result;
  try {
    const std::wstring terminated {name};
    const auto high = static_cast<DWORD>(static_cast<uint64_t>(size) >> 32);
    const auto low = static_cast<DWORD>(size & 0xFFFFFFFFu);
    HANDLE mapping
      = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low, terminated.c_str());
    const DWORD error = GetLastError();
    if (!mapping) {
      status = StatusFromError(error);
      return result;
    }
    if (error == ERROR_ALREADY_EXISTS) {
      // Someone else created it first: never adopt a block we didn't make
      CloseHandle(mapping);
      status = Status::AlreadyExists;
      return result;
    }
    result.mMapping = mapping;
    result.mView = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
    if (!result.mView) {
      status = StatusFromError(GetLastError());
      result.Reset();
      return result;
    }
    result.mSize = size;
    status = Status::Ok;
  } catch (...) {
    status = Status::Failed;
    result.Reset();
  }
  return result;
}

SharedMemory SharedMemory::Open(std::wstring_view name, std::size_t size, Status& status) noexcept {
  SharedMemory result;
  try {
    const std::wstring terminated {name};
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, terminated.c_str());
    if (!mapping) {
      status = StatusFromError(GetLastError());
      return result;
    }
    result.mMapping = mapping;
    result.mView = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!result.mView) {
      status = StatusFromError(GetLastError());
      result.Reset();
      return result;
    }
    // Never read past a smaller block from another build (SY-7)
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(result.mView, &info, sizeof(info)) == 0 || info.RegionSize < size) {
      status = Status::TooSmall;
      result.Reset();
      return result;
    }
    result.mSize = size;
    status = Status::Ok;
  } catch (...) {
    status = Status::Failed;
    result.Reset();
  }
  return result;
}

const char* SharedMemory::StatusName(Status status) noexcept {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::NotFound:
      return "not found";
    case Status::AlreadyExists:
      return "already exists";
    case Status::AccessDenied:
      return "access denied";
    case Status::TooSmall:
      return "too small";
    case Status::Failed:
      return "failed";
  }
  return "?";
}

std::wstring SharedObjectName(std::wstring_view object, unsigned abiVersion) {
  const auto instance = ProcessUtil::GetEnvironment("OPENDESKTOPVR_INSTANCE");
  if (!instance || instance->empty()) {
    return Product::KernelObjectName(object, abiVersion);
  }
  return std::format(
    L"Local\\{}.{}.v{}.{}", Product::FolderName, StringUtil::Widen(*instance), abiVersion, object);
}

}// namespace OpenDesktopVR

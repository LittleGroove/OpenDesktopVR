// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Makes OpenDesktopVR-Setup.exe (M7a, ADR-043): packs a folder of the
// program's files (cmake --install's "product" component) into a payload,
// and adds it to a copy of the setup program as its resource. Then reads it
// back from the result, as the setup program will, to check it.
//
//   odvr-pack <setup program> <folder> <output> --version <version>

#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/Payload.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace OpenDesktopVR;

namespace {

int Fail(const std::string& why) {
  std::fprintf(stderr, "odvr-pack: %s\n", why.c_str());
  return 1;
}

// "commit 560263b on m7, built 2026-11-18 12:00 (Release)", from BUILD-INFO.txt
std::string BuildLine(const std::string& info) {
  std::string commit;
  std::string built;
  std::size_t start = 0;
  while (start < info.size()) {
    auto end = info.find('\n', start);
    end = end == std::string::npos ? info.size() : end;
    std::string line = info.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto value = [&line] {
      const auto at = line.find_first_not_of(' ', line.find(':') + 1);
      return at == std::string::npos ? std::string {} : line.substr(at);
    };
    if (line.starts_with("Commit:")) {
      commit = value();
    } else if (line.starts_with("Built:")) {
      built = value();
    }
    start = end + 1;
  }
  return commit.empty() ? std::string {"a development build"} : "commit " + commit + ", built " + built;
}

// Adds the payload as the program's resource; antivirus may hold a new file
// for a moment, so it's retried
bool AddResource(const fs::path& output, const std::string& bytes) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (const HANDLE update = BeginUpdateResourceW(output.c_str(), FALSE)) {
      const bool added = UpdateResourceW(update, RT_RCDATA, MAKEINTRESOURCEW(Install::PayloadResourceID),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), const_cast<char*>(bytes.data()), static_cast<DWORD>(bytes.size()));
      if (EndUpdateResourceW(update, added ? FALSE : TRUE) && added) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

std::optional<Install::Payload> ReadBack(const fs::path& output, std::string& error) {
  const HMODULE module = LoadLibraryExW(output.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module) {
    error = "the result can't be opened";
    return std::nullopt;
  }
  std::optional<Install::Payload> payload;
  const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(Install::PayloadResourceID), RT_RCDATA);
  const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
  if (const void* data = loaded ? LockResource(loaded) : nullptr) {
    payload = Install::Unpack({static_cast<const char*>(data), SizeofResource(module, resource)}, &error);
  } else {
    error = "the result has no payload";
  }
  FreeLibrary(module);
  return payload;
}

}// namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 6 || std::wstring_view {argv[4]} != L"--version") {
    std::printf("usage: odvr-pack <setup program> <folder> <output> --version <version>\n");
    return 2;
  }
  const fs::path stub {argv[1]};
  const fs::path folder {argv[2]};
  const fs::path output {argv[3]};
  Install::Payload payload;
  payload.mVersion = StringUtil::Narrow(argv[5]);

  std::error_code error;
  for (auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator();
       it.increment(error)) {
    if (!it->is_regular_file(error)) {
      continue;
    }
    auto relative = fs::relative(it->path(), folder, error).generic_wstring();
    auto data = FileUtil::ReadAll(it->path());
    if (!data) {
      return Fail("can't read " + StringUtil::Narrow(it->path().wstring()));
    }
    payload.mFiles.push_back({StringUtil::Narrow(relative), std::move(*data)});
  }
  if (error || payload.mFiles.empty()) {
    return Fail("nothing to pack in " + StringUtil::Narrow(folder.wstring()));
  }
  std::sort(payload.mFiles.begin(), payload.mFiles.end(), [](const auto& a, const auto& b) { return a.mPath < b.mPath; });
  if (const auto info = FileUtil::ReadAll(folder / L"BUILD-INFO.txt")) {
    payload.mBuild = BuildLine(*info);
  }

  std::string why;
  const auto bytes = Install::Pack(payload, &why);
  if (!bytes) {
    return Fail("packing failed: " + why);
  }
  fs::create_directories(output.parent_path(), error);
  if (!CopyFileW(stub.c_str(), output.c_str(), FALSE)) {
    return Fail("can't copy the setup program to " + StringUtil::Narrow(output.wstring()));
  }
  if (!AddResource(output, *bytes)) {
    DeleteFileW(output.c_str());
    return Fail("can't add the payload to " + StringUtil::Narrow(output.wstring()));
  }
  const auto back = ReadBack(output, why);
  if (!back || back->mFiles.size() != payload.mFiles.size()) {
    DeleteFileW(output.c_str());
    return Fail("the result doesn't read back: " + why);
  }
  std::size_t unpacked = 0;
  for (const auto& file: payload.mFiles) {
    unpacked += file.mData.size();
  }
  std::printf("odvr-pack: %zu files (%zu KB) packed into %zu KB: %s (%s, %s)\n", payload.mFiles.size(), unpacked / 1024,
    bytes->size() / 1024, StringUtil::Narrow(output.filename().wstring()).c_str(), payload.mVersion.c_str(),
    payload.mBuild.c_str());
  return 0;
}

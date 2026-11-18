// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/DebugBundle.hpp>
#include <OpenDesktopVR/FileUtil.hpp>
#include <OpenDesktopVR/KnownLayers.hpp>
#include <OpenDesktopVR/Log.hpp>
#include <OpenDesktopVR/StringUtil.hpp>
#include <OpenDesktopVR/ZipWriter.hpp>

#include <Windows.h>

#include <algorithm>
#include <format>

namespace OpenDesktopVR::Diagnostics {

namespace fs = std::filesystem;

namespace {

struct Found {
  fs::path mPath;
  fs::file_time_type mWritten;
};

// Files in a folder (not below it) whose names pass `keep`, newest first
template <class Keep>
std::vector<Found> FilesIn(const fs::path& folder, Keep keep) {
  std::vector<Found> found;
  std::error_code error;
  for (const auto& entry: fs::directory_iterator(folder, error)) {
    std::error_code itemError;
    if (entry.is_regular_file(itemError) && keep(entry.path().filename().wstring())) {
      found.push_back({entry.path(), entry.last_write_time(itemError)});
    }
  }
  std::ranges::sort(found, [](const Found& a, const Found& b) { return a.mWritten > b.mWritten; });
  return found;
}

bool EndsWith(const std::wstring& name, std::wstring_view suffix) {
  return name.size() >= suffix.size() && StringUtil::EqualsIgnoreCase(name.substr(name.size() - suffix.size()), suffix);
}

bool StartsWith(const std::wstring& name, std::wstring_view prefix) {
  return name.size() >= prefix.size() && StringUtil::EqualsIgnoreCase(name.substr(0, prefix.size()), prefix);
}

class Builder {
 public:
  explicit Builder(const Redaction& redaction) : mRedaction(redaction) {
  }

  void AddText(const std::string& name, std::string_view text) {
    Add(name, Redact(text, mRedaction));
  }

  // A file from disk; text is redacted, binary isn't
  void AddFile(const std::string& name, const fs::path& path, bool redact) {
    const auto data = FileUtil::ReadAll(path);
    if (!data) {
      mNotes.push_back(std::format("{}: couldn't be read", name));
      return;
    }
    Add(name, redact ? Redact(*data, mRedaction) : *data);
  }

  void Add(const std::string& name, std::string_view data) {
    if (mZip.Add(name, data)) {
      mNames.push_back(name);
    } else {
      mNotes.push_back(std::format("{}: left out (the bundle is full)", name));
    }
  }

  ZipWriter mZip;
  std::vector<std::string> mNames;
  std::vector<std::string> mNotes;

 private:
  const Redaction& mRedaction;
};

}// namespace

std::string_view PrivacyNotice() noexcept {
  return "The debug info file holds what helps find a problem: this PC's Windows version, graphics cards and "
         "OpenXR setup, Open Desktop VR's settings and profiles, its logs, and any crash dumps.\n\n"
         "Your user folder is hidden in paths, but window titles of your panels and apps are not, and crash dumps "
         "can hold whatever was in Open Desktop VR's memory, including what was on screen.\n\n"
         "Nothing is sent anywhere: the file is saved where you choose, and you decide who gets it.\n\n"
         "Tip: for a useful log, start Open Desktop VR before the game.";
}

std::wstring SuggestedBundleName() {
  SYSTEMTIME now {};
  GetLocalTime(&now);
  return std::format(L"OpenDesktopVR-debug-{:04}{:02}{:02}-{:02}{:02}.zip", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
}

std::string DescribeLayers(const std::vector<Registration::LayerEntry>& layers) {
  if (layers.empty()) {
    return "No implicit API layers are registered.\r\n";
  }
  std::string text = "Implicit OpenXR API layers, as registered (HKCU and HKLM, both registry views):\r\n";
  for (const auto& entry: layers) {
    const auto kind = entry.mLayerName ? KnownLayers::Classify(*entry.mLayerName) : KnownLayers::None;
    text += std::format(
      "- {} [{}]{}{}\r\n    manifest: {}{}\r\n    name: {}\r\n    library: {}\r\n",
      StringUtil::Narrow(entry.mWhere),
      entry.mEnabled ? "on" : "off",
      entry.mIsOurs ? " (Open Desktop VR)" : "",
      kind != KnownLayers::None ? std::format(" ({})", KnownLayers::Describe(kind)) : std::string {},
      StringUtil::Narrow(entry.mManifestPath.wstring()),
      entry.mManifestExists ? "" : " (missing)",
      entry.mLayerName.value_or("(unreadable)"),
      entry.mLibraryPath.value_or("(unreadable)"));
  }
  return text;
}

BundleResult WriteBundle(const fs::path& zipPath, const BundleSources& sources) noexcept {
  BundleResult result;
  try {
    Builder builder {sources.mRedaction};
    for (const auto& [name, text]: sources.mTexts) {
      builder.AddText(name, text);
    }
    const auto& data = sources.mDataFolder;
    if (!data.empty()) {
      if (FileUtil::Exists(data / L"settings.json")) {
        builder.AddFile("settings/settings.json", data / L"settings.json", true);
      }
      for (const auto& file: FilesIn(data / L"profiles", [](const std::wstring& name) { return EndsWith(name, L".json"); })) {
        builder.AddFile("settings/profiles/" + StringUtil::Narrow(file.mPath.filename().wstring()), file.mPath, true);
      }
      std::size_t layerLogs = 0;
      for (const auto& file: FilesIn(data / L"logs", [](const std::wstring& name) { return EndsWith(name, L".log"); })) {
        const auto name = file.mPath.filename().wstring();
        if (StartsWith(name, L"layer-") && layerLogs++ >= sources.mMaxLayerLogs) {
          continue;
        }
        builder.AddFile("logs/" + StringUtil::Narrow(name), file.mPath, true);
      }
      const auto dumps = data / L"crashdumps";
      if (FileUtil::Exists(dumps / L"crashes.txt")) {
        builder.AddFile("crashdumps/crashes.txt", dumps / L"crashes.txt", true);
      }
      std::size_t dumpCount = 0;
      for (const auto& file: FilesIn(dumps, [](const std::wstring& name) { return EndsWith(name, L".dmp"); })) {
        if (dumpCount++ >= sources.mMaxDumps) {
          break;
        }
        builder.AddFile("crashdumps/" + StringUtil::Narrow(file.mPath.filename().wstring()), file.mPath, false);
      }
    }

    std::string readme = std::format(
      "Open Desktop VR debug info\r\n\r\nWhat's here:\r\n");
    for (const auto& name: builder.mNames) {
      readme += "- " + name + "\r\n";
    }
    if (!builder.mNotes.empty()) {
      readme += "\r\nNot included:\r\n";
      for (const auto& note: builder.mNotes) {
        readme += "- " + note + "\r\n";
      }
    }
    readme += "\r\nPaths have the user's folder replaced by %USERPROFILE% or <user>. Crash dumps are as written.\r\n";
    builder.Add("README.txt", readme);

    result.mFiles = builder.mZip.FileCount();
    const auto bytes = builder.mZip.Finish();
    result.mBytes = bytes.size();
    const auto written = FileUtil::WriteAtomic(zipPath, bytes);
    result.mOk = written.mOutcome != FileUtil::WriteOutcome::Failed;
    if (!result.mOk) {
      result.mError = std::format("couldn't write the file (error {})", written.mLastError);
    }
  } catch (const std::exception& error) {
    result.mOk = false;
    result.mError = error.what();
  } catch (...) {
    result.mOk = false;
    result.mError = "unexpected error";
  }
  return result;
}

}// namespace OpenDesktopVR::Diagnostics

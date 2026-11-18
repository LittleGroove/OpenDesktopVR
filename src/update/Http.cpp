// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/Http.hpp>
#include <OpenDesktopVR/Product.hpp>
#include <OpenDesktopVR/StringUtil.hpp>

#include <Windows.h>

#include <winhttp.h>

#include <format>

namespace OpenDesktopVR::Update {

namespace {

// Closes a WinHTTP handle when it goes
class Handle {
 public:
  explicit Handle(HINTERNET handle = nullptr) noexcept : mHandle(handle) {
  }
  ~Handle() {
    if (mHandle) {
      WinHttpCloseHandle(mHandle);
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HINTERNET Get() const noexcept {
    return mHandle;
  }
  explicit operator bool() const noexcept {
    return mHandle != nullptr;
  }

 private:
  HINTERNET mHandle;
};

std::string Failure(std::string_view what) {
  return std::format("{} (error {})", what, GetLastError());
}

// The whole exchange: `sink` gets the body in chunks; false from it stops
template <class Sink>
HttpResult Fetch(std::string_view url, const HttpOptions& options, Sink&& sink) {
  HttpResult result;
  const auto wideUrl = StringUtil::Widen(url);
  URL_COMPONENTS parts {sizeof(parts)};
  wchar_t host[256] {};
  wchar_t path[2048] {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  parts.dwSchemeLength = 1;
  parts.dwExtraInfoLength = 1;
  if (url.size() > 2000 || !WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
    result.mError = "the address isn't one";
    return result;
  }
  // The path with its query: lpszExtraInfo points into wideUrl
  std::wstring target {path};
  if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
    target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
  const bool localHttp = parts.nScheme == INTERNET_SCHEME_HTTP && options.mAllowLocalHttp && std::wstring_view {host} == L"127.0.0.1";
  if (!https && !localHttp) {
    result.mError = "only https addresses are used";
    return result;
  }

  const auto agent = std::format(L"OpenDesktopVR/{}", StringUtil::Widen(Product::Version));
  Handle session {WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
    WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session) {
    result.mError = Failure("WinHTTP couldn't start");
    return result;
  }
  const int timeout = static_cast<int>(options.mTimeout.count());
  WinHttpSetTimeouts(session.Get(), timeout, timeout, timeout, timeout);
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if (!WinHttpSetOption(session.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;// Windows 10 before TLS 1.3
    WinHttpSetOption(session.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
  Handle connection {WinHttpConnect(session.Get(), host, parts.nPort, 0)};
  Handle request {connection ? WinHttpOpenRequest(connection.Get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
                             : nullptr};
  if (!request) {
    result.mError = Failure("couldn't open the request");
    return result;
  }
  std::wstring headers;
  for (const auto& header: options.mHeaders) {
    headers += header + L"\r\n";
  }
  if (!WinHttpSendRequest(request.Get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
      || !WinHttpReceiveResponse(request.Get(), nullptr)) {
    result.mError = Failure("no answer from the server");
    return result;
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
    &status, &size, WINHTTP_NO_HEADER_INDEX);
  result.mStatus = status;
  if (status != 200) {
    result.mError = std::format("the server answered {}", status);
    return result;
  }
  std::uint64_t total = 0;
  wchar_t lengthText[32] {};
  DWORD lengthSize = sizeof(lengthText);
  if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
        &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
    total = _wcstoui64(lengthText, nullptr, 10);
  }
  if (total > options.mMaxBytes) {
    result.mError = std::format("the answer is too big ({} bytes)", total);
    return result;
  }
  std::string chunk;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.Get(), &available)) {
      result.mError = Failure("the answer broke off");
      return result;
    }
    if (available == 0) {
      break;
    }
    if (result.mBytes + available > options.mMaxBytes) {
      result.mError = "the answer is too big";
      return result;
    }
    chunk.resize(available);
    DWORD read = 0;
    if (!WinHttpReadData(request.Get(), chunk.data(), available, &read)) {
      result.mError = Failure("the answer broke off");
      return result;
    }
    result.mBytes += read;
    if (!sink(std::string_view {chunk.data(), read}, result.mBytes, total)) {
      result.mError = "stopped";
      return result;
    }
  }
  if (total != 0 && result.mBytes != total) {
    result.mError = std::format("the answer was cut short ({} of {} bytes)", result.mBytes, total);
    return result;
  }
  result.mOk = true;
  return result;
}

}// namespace

HttpResult Get(std::string_view url, const HttpOptions& options) noexcept {
  try {
    std::string body;
    auto result = Fetch(url, options, [&body](std::string_view chunk, std::uint64_t, std::uint64_t) {
      body += chunk;
      return true;
    });
    result.mBody = std::move(body);
    return result;
  } catch (...) {
    HttpResult result;
    result.mError = "out of memory";
    return result;
  }
}

HttpResult Download(std::string_view url, const std::filesystem::path& file, const HttpOptions& options,
  const std::function<bool(std::uint64_t, std::uint64_t)>& progress) noexcept {
  try {
    const HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
      HttpResult result;
      result.mError = Failure("couldn't write the download");
      return result;
    }
    bool written = true;
    auto result = Fetch(url, options, [&](std::string_view chunk, std::uint64_t done, std::uint64_t total) {
      DWORD wrote = 0;
      written = WriteFile(out, chunk.data(), static_cast<DWORD>(chunk.size()), &wrote, nullptr) && wrote == chunk.size();
      return written && (!progress || progress(done, total));
    });
    const bool flushed = FlushFileBuffers(out);
    CloseHandle(out);
    if (result.mOk && (!written || !flushed)) {
      result.mOk = false;
      result.mError = "couldn't write the download";
    }
    if (!result.mOk) {
      DeleteFileW(file.c_str());
    }
    return result;
  } catch (...) {
    DeleteFileW(file.c_str());
    HttpResult result;
    result.mError = "out of memory";
    return result;
  }
}

}// namespace OpenDesktopVR::Update

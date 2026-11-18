// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// A tiny HTTP/1.1 server on 127.0.0.1 for tests (ADR-045): a fake GitHub
// for the update checks. One request per connection; the handler picks
// each answer by path. Plain Winsock, a thread, nothing else.
//
//   FakeHttpServer server {[](const std::string& path) {
//     return FakeHttpServer::Response {200, "[]"};
//   }};
//   const auto url = server.Url("/repos/x/y/releases");
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace OpenDesktopVR::Testing {

class FakeHttpServer {
 public:
  struct Response {
    int mStatus {200};
    std::string mBody;
    std::string mLocation;// for a redirect
    // Claims more bytes than it sends, then closes: a cut-off answer
    bool mCutShort {false};
  };
  using Handler = std::function<Response(const std::string& path)>;

  explicit FakeHttpServer(Handler handler) : mHandler(std::move(handler)) {
    WSADATA data {};
    WSAStartup(MAKEWORD(2, 2), &data);
    mListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(mListener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int size = sizeof(address);
    getsockname(mListener, reinterpret_cast<sockaddr*>(&address), &size);
    mPort = ntohs(address.sin_port);
    listen(mListener, 16);
    mThread = std::thread([this] { Serve(); });
  }

  ~FakeHttpServer() {
    mStopping = true;
    closesocket(mListener);
    if (mThread.joinable()) {
      mThread.join();
    }
    WSACleanup();
  }

  FakeHttpServer(const FakeHttpServer&) = delete;
  FakeHttpServer& operator=(const FakeHttpServer&) = delete;

  std::string Url(std::string_view path) const {
    return std::format("http://127.0.0.1:{}{}", mPort, path);
  }

  int Requests(const std::string& path) const {
    std::lock_guard lock(mMutex);
    const auto it = mCounts.find(path);
    return it == mCounts.end() ? 0 : it->second;
  }

 private:
  void Serve() {
    while (!mStopping) {
      const SOCKET client = accept(mListener, nullptr, nullptr);
      if (client == INVALID_SOCKET) {
        break;
      }
      Answer(client);
      closesocket(client);
    }
  }

  void Answer(SOCKET client) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
      const int got = recv(client, buffer, sizeof(buffer), 0);
      if (got <= 0) {
        return;
      }
      request.append(buffer, static_cast<std::size_t>(got));
    }
    // "GET /path HTTP/1.1"
    const auto start = request.find(' ');
    const auto end = request.find(' ', start + 1);
    if (start == std::string::npos || end == std::string::npos) {
      return;
    }
    const auto path = request.substr(start + 1, end - start - 1);
    {
      std::lock_guard lock(mMutex);
      ++mCounts[path];
    }
    Response response;
    try {
      response = mHandler(path);
    } catch (...) {
      response = {500, "handler threw"};
    }
    const auto length = response.mCutShort ? response.mBody.size() * 2 + 10 : response.mBody.size();
    auto head = std::format("HTTP/1.1 {} X\r\nContent-Length: {}\r\nConnection: close\r\n", response.mStatus, length);
    if (!response.mLocation.empty()) {
      head += "Location: " + response.mLocation + "\r\n";
    }
    head += "\r\n";
    const auto all = head + response.mBody;
    std::size_t sent = 0;
    while (sent < all.size()) {
      const int wrote = send(client, all.data() + sent, static_cast<int>(std::min<std::size_t>(all.size() - sent, 65536)), 0);
      if (wrote <= 0) {
        return;
      }
      sent += static_cast<std::size_t>(wrote);
    }
    shutdown(client, SD_SEND);
  }

  Handler mHandler;
  SOCKET mListener {INVALID_SOCKET};
  unsigned short mPort {0};
  std::atomic<bool> mStopping {false};
  std::thread mThread;
  mutable std::mutex mMutex;
  std::map<std::string, int> mCounts;
};

}// namespace OpenDesktopVR::Testing

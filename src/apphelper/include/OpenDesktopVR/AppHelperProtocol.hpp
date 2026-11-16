// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// The host <-> app helper protocol (doc 06, "Helper behavior requirements";
// HK-1 to HK-11; ADR-031).
//
// The app helper (OpenDesktopVR-AppHelper.dll) runs inside a captured app.
// The host loads it with a WH_GETMESSAGE hook on the captured window's UI
// thread, one hook per thread (HK-2).
//
// - **Commands:** the host posts CommandMessageName's message to a window of
//   the hooked thread. wParam is a Command, lParam its argument. The hook acts
//   on it only when it's removed from the queue (HK-3), then turns it into
//   WM_NULL, so the app never sees it. Posted, never sent: a hook on
//   WH_GETMESSAGE only sees posted messages, and the host never waits on
//   another process.
// - **Replies:** the helper posts ReplyMessageName's message to the host's
//   reply window (given by Hello). wParam packs the command, a Result and a
//   detail; lParam is the hooked thread's ID.
//
// The keyboard commands (0x10 to 0x12) are in InjectionHookProtocol.hpp.
#pragma once

#include <Windows.h>

#include <cstdint>

namespace OpenDesktopVR::AppHelperProtocol {

inline constexpr wchar_t CommandMessageName[] = L"OpenDesktopVR.AppHelper.Command";
inline constexpr wchar_t ReplyMessageName[] = L"OpenDesktopVR.AppHelper.Reply";
// The hook procedure the DLL exports
inline constexpr char HookProcName[] = "OdvrAppHelperGetMessageProc";
// Hello's reply detail: a host and helper from different builds don't mix
inline constexpr uint16_t Version = 1;

enum class Command : uint8_t {
  // lParam: the host's reply window. Posted first after each install, so
  // nothing else is sent before the helper answers (HK-4)
  Hello = 0x01,
  // lParam: a top-level window of this thread. While any window is on, other
  // processes' top-level windows look hidden to the app (HK-6), so it keeps
  // painting when they cover it; the app is then told to look again (HK-7).
  // Reply detail: how many import slots point at the helper.
  KeepPaintingOn = 0x02,
  // lParam: the window. With no window left on, the app sees the real
  // visibility again (and is told to look again)
  KeepPaintingOff = 0x03,
  // Undo everything and wait (bounded) for calls already in the helper,
  // before the host removes the hook (HK-9). Reply detail: calls still in
  // flight
  Unload = 0x04,
};

enum class Result : uint8_t {
  Ok = 0,
  // KeepPaintingOn: none of the app's modules import the visibility check
  NothingToPatch = 1,
  // KeepPaintingOn: more windows than the helper keeps
  TooManyWindows = 2,
  // Unload: calls were still in the helper after the wait; it stays loaded
  // (pinned) so the app doesn't crash, and does nothing more
  StillBusy = 3,
  Failed = 4,
};

struct Reply {
  Command mCommand {};
  Result mResult {};
  uint16_t mDetail {0};
};

inline constexpr WPARAM Pack(const Reply& reply) noexcept {
  return static_cast<WPARAM>(static_cast<uint32_t>(reply.mCommand) | (static_cast<uint32_t>(reply.mResult) << 8)
    | (static_cast<uint32_t>(reply.mDetail) << 16));
}

inline constexpr Reply Unpack(WPARAM packed) noexcept {
  const auto value = static_cast<uint32_t>(packed);
  return {static_cast<Command>(value & 0xFF), static_cast<Result>((value >> 8) & 0xFF),
    static_cast<uint16_t>(value >> 16)};
}

}// namespace OpenDesktopVR::AppHelperProtocol

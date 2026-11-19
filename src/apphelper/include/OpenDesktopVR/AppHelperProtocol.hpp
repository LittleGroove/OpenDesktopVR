// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// The host <-> app helper protocol (doc 06, "Helper behavior requirements";
// HK-1 to HK-11; ADR-031).
//
// The app helper (OpenDesktopVR-AppHelper.dll) runs inside a captured app.
// The host loads it with hooks on the captured window's UI thread, one set
// per thread (HK-2): WH_GETMESSAGE, and for input WH_CALLWNDPROC too.
//
// - **Posted commands** (setting up, switching modes, unloading): the host
//   posts CommandMessageName's message to a window of the hooked thread.
//   wParam is a Command, lParam its argument. The WH_GETMESSAGE hook acts on
//   it only when it's removed from the queue (HK-3), then turns it into
//   WM_NULL, so the app never sees it. The host never waits for these.
// - **Sent commands** (around each injected event, M4b): the injector's
//   worker sends the same registered message with SendMessageTimeout, in
//   order with the mouse messages it sends; the WH_CALLWNDPROC hook sees it
//   before the window does (the window then ignores a message it doesn't
//   know). Never answered. Only from a worker thread, never the UI thread
//   (INJ-2).
// - **Replies:** the helper posts ReplyMessageName's message to the host's
//   reply window (given by Hello). wParam packs the command, a Result and a
//   detail; lParam is the hooked thread's ID.
//
// The keyboard's sent commands (0x30 to 0x32) are in KeyChannel.hpp.
#pragma once

#include <Windows.h>

#include <cstdint>

namespace OpenDesktopVR::AppHelperProtocol {

inline constexpr wchar_t CommandMessageName[] = L"OpenDesktopVR.AppHelper.Command";
inline constexpr wchar_t ReplyMessageName[] = L"OpenDesktopVR.AppHelper.Reply";
// The hook procedure the DLL exports
inline constexpr char HookProcName[] = "OdvrAppHelperGetMessageProc";
inline constexpr char CallHookProcName[] = "OdvrAppHelperCallWndProc";
// Hello's reply detail: a host and helper from different builds don't mix
// 2: input (InputOn, InputOff, the sent commands)
// 3: SetMouseButtons
// 4: the first keyboard commands
// 5: the keyboard commands restated, with new values (C-2)
inline constexpr uint16_t Version = 5;

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
  // lParam: a top-level window of this thread. Sets up the input fakes
  // (doc 06 "Why a helper is needed"); they act only inside an injection
  // window (BeginInjection). Reply detail: how many import slots point at
  // the helper.
  InputOn = 0x05,
  // lParam: the window
  InputOff = 0x06,

  // ---- Sent, around each injected event; never answered ----
  // 0x30 to 0x32 belong to the keyboard (KeyChannel::KeyCommand):
  // activating the window on its thread, the modifier state, and a character
  // for the next key-down.
  //
  // lParam: the event's screen point (PackPoint). Opens an injection window
  // on this thread (HK-5): from here to EndInjection, the app finds the
  // cursor at that point; until 100 ms after EndInjection it sees its
  // window as the foreground one, can't change its z-order and can't
  // SendInput. If EndInjection never comes, it closes after 2 s.
  BeginInjection = 0x20,
  EndInjection = 0x21,
  // lParam: the mouse buttons held (MK_LBUTTON ...). Until the next one,
  // inside injection windows, the app sees them held (GetKeyState,
  // GetAsyncKeyState): browsers check the button, not just the message.
  SetMouseButtons = 0x22,
};

// A screen point as GetMessagePos packs it: signed 16-bit x and y, so
// points left of or above the main monitor survive
inline constexpr LPARAM PackPoint(int x, int y) noexcept {
  return static_cast<LPARAM>(static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(x)))
    | (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(y))) << 16));
}
inline constexpr POINT UnpackPoint(LPARAM packed) noexcept {
  const auto value = static_cast<uint32_t>(packed);
  return {static_cast<int16_t>(value & 0xFFFF), static_cast<int16_t>(value >> 16)};
}

enum class Result : uint8_t {
  Ok = 0,
  // KeepPaintingOn, InputOn: none of the app's modules import what the
  // helper replaces
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

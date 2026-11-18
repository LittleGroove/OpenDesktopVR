# Open Desktop VR

**Your desktop's windows in VR games, used with your hands.**

Open Desktop VR puts windows from your desktop on panels inside OpenXR
games: Discord, a browser, Spotify, a game's companion app. With hand
tracking you tap, scroll, drag and type on them without taking the headset
off, and the game keeps the focus.

It's free software under the GNU GPL, version 3.

## What it does

- **Windows as panels** in any OpenXR game, placed where you want them,
  with their own size, opacity, brightness and tint. Profiles switch the
  set of panels by hand or by game.
- **Your hands:** tap, drag and scroll on panels; a virtual keyboard shows
  when you tap into a text field.
- **A quick menu in VR** for showing and hiding panels, moving them,
  recentering and switching profiles.
- **Keybinds** on the keyboard or a game controller (HOTAS included), and
  commands for Stream Deck and similar tools.
- **Diagnostics:** a self-test, the log, and a debug bundle to share when
  something's wrong.

## What you need

- Windows 10 or 11, 64-bit.
- An OpenXR game, and a headset with hand tracking to touch panels.
- It's developed and tested with a Meta Quest 3 through Virtual Desktop,
  playing Elite Dangerous. Other headsets, runtimes and games should work
  too; please report what you find.

## Install

1. Download `OpenDesktopVR-Setup-<version>.exe` from
   [Releases](https://github.com/LittleGroove/OpenDesktopVR/releases).
2. Run it. It installs for your Windows user only, with no administrator
   rights.
   - It isn't code-signed yet, so Windows may say **Windows protected your
     PC**: click **More info**, then **Run anyway**.
3. Its settings window opens the first time, with a short welcome. After
   that it waits in the tray by the clock; starting it again from the Start
   menu opens its window.

To uninstall it, use **Settings**, **Apps** in Windows. Your panels and
settings are kept unless you tick the box to delete them.

## Updates and the beta

- Open Desktop VR checks this repository's Releases every few hours. A new
  version installs by itself while no game is running and its window is
  closed. Both can be turned off: **General** tab, **Updates**.
- **Beta versions:** tick **Get beta versions (pre-releases)** to get
  pre-releases too: test versions of what's coming. Untick it to go back to
  releases from the next one on.
- Checking sends GitHub your address and `OpenDesktopVR/<version>`, nothing
  else. There's no telemetry.

## Build it yourself

You need Visual Studio 2022 with the C++ desktop workload (its CMake
included) and a Windows 10 or 11 SDK. Third-party code is downloaded at
configure time, pinned by version and SHA-256 (`cmake/OdvrThirdParty.cmake`).

```
cmake --preset default
cmake --build --preset release
ctest --preset release
```

The setup program is `build/bin/Release/OpenDesktopVR-Setup.exe`. Some
tests drive real windows and need an unlocked desktop to themselves.

## Licence

Open Desktop VR is free software: you can redistribute it and modify it
under the terms of the GNU General Public License, version 3, as published
by the Free Software Foundation. It comes with no warranty. See
[LICENSE](LICENSE).

The third-party components it includes, all under GPL-compatible licences,
are listed in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).

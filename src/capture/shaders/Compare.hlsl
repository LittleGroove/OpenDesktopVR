// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
//
// Did a captured frame change? Compares the new frame with the one shown and
// sets the result to 1 if any pixel differs. Windows 10 delivers frames for
// some unchanged windows (test round 4: Notepad at the 30/s cap), and each
// skipped frame saves a panel redraw and a copy in the game.

Texture2D<float4> gNew : register(t0);
Texture2D<float4> gShown : register(t1);
RWByteAddressBuffer gResult : register(u0);

[numthreads(8, 8, 1)]
void CompareCS(uint3 id: SV_DispatchThreadID) {
  uint width, height;
  gNew.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) {
    return;
  }
  const int3 at = int3(id.xy, 0);
  if (any(gNew.Load(at) != gShown.Load(at))) {
    gResult.Store(0, 1);
  }
}

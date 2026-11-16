// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Busy GPU work for tests: each thread does a long chain of arithmetic and
// writes the result, so the work can't be skipped. Stands in for a game
// keeping the GPU full (test round 7).

RWTexture2D<float4> gOutput : register(u0);

[numthreads(8, 8, 1)]
void GpuLoadCS(uint3 id: SV_DispatchThreadID) {
  float4 value = float4(id.xy, 1, 1) * 0.001;
  [loop] for (uint i = 0; i < 512; ++i) {
    value = frac(value * 1.618 + 0.271);
  }
  gOutput[id.xy] = value;
}

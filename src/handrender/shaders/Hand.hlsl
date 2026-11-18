// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Robert McGough
// Provenance: the author's own prototype code, imported at handover v2 (2026-11-15).
//
// See-through 'ghost' hands, styled after the Quest's hands. Each bone is a
// tapered tube, and each joint a sphere; the geometry is generated from the
// vertex ID, so no vertex buffers are needed.
//
// The hands are drawn twice: first depth-only, then in color, only where the
// depth matches. That way only the nearest surface is shaded, giving a single
// see-through layer instead of overlapping tubes and spheres. Output is
// premultiplied alpha.

#define MAX_HANDS 2
#define CONES_PER_HAND 24
#define SPHERES_PER_HAND 26
#define MAX_CONES (MAX_HANDS * CONES_PER_HAND)
#define MAX_SPHERES (MAX_HANDS * SPHERES_PER_HAND)

// Tessellation; must match HandRenderer.cpp
#define CONE_SEGMENTS 12
#define SPHERE_SEGMENTS 12
#define SPHERE_RINGS 8

static const float Pi = 3.14159265;

cbuffer HandConstants : register(b0) {
  row_major float4x4 gViewProjection;
  float4 gEyePosition;// xyz
  float4 gHandWrist[MAX_HANDS];// xyz = wrist, w = visibility (0..1)
  float4 gHandAxis[MAX_HANDS];// xyz = unit vector from the wrist to the knuckles
  float4 gConeA[MAX_CONES];// xyz = one end, w = its radius
  float4 gConeB[MAX_CONES];// xyz = the other end, w = its radius
  float4 gSpheres[MAX_SPHERES];// xyz = center, w = radius
};

struct VSOutput {
  float4 mPosition : SV_Position;
  float3 mWorldPosition : POSITION0;
  float3 mNormal : NORMAL0;
  nointerpolation uint mHand : HAND0;
};

// The two triangles of a quad, as (column, row) offsets
static const uint2 QuadCorners[6] = {
  uint2(0, 0),
  uint2(1, 0),
  uint2(0, 1),
  uint2(1, 0),
  uint2(1, 1),
  uint2(0, 1),
};

VSOutput MakeOutput(float3 position, float3 normal, uint hand) {
  VSOutput output;
  output.mPosition = mul(float4(position, 1), gViewProjection);
  output.mWorldPosition = position;
  output.mNormal = normal;
  output.mHand = hand;
  return output;
}

// The side of a tapered tube between two joints
VSOutput HandConeVertexShader(uint vertex : SV_VertexID, uint cone : SV_InstanceID) {
  const uint segment = vertex / 6;
  const uint2 corner = QuadCorners[vertex % 6];

  const float4 a = gConeA[cone];
  const float4 b = gConeB[cone];
  const float3 axis = b.xyz - a.xyz;
  const float len = max(length(axis), 1e-5);
  const float3 w = axis / len;
  const float3 helper = (abs(w.y) < 0.99) ? float3(0, 1, 0) : float3(1, 0, 0);
  const float3 u = normalize(cross(helper, w));
  const float3 v = cross(w, u);

  const float angle = (segment + corner.x) * (2 * Pi / CONE_SEGMENTS);
  const float3 radial = (cos(angle) * u) + (sin(angle) * v);
  const float t = corner.y;
  const float3 position = lerp(a.xyz, b.xyz, t) + (radial * lerp(a.w, b.w, t));
  // Tilted to account for the taper
  const float3 normal = normalize(radial - (w * ((b.w - a.w) / len)));
  return MakeOutput(position, normal, cone / CONES_PER_HAND);
}

// A sphere at a joint
VSOutput HandSphereVertexShader(uint vertex : SV_VertexID, uint sphere : SV_InstanceID) {
  const uint quad = vertex / 6;
  const uint2 corner = QuadCorners[vertex % 6];
  const uint ring = quad / SPHERE_SEGMENTS;
  const uint segment = quad % SPHERE_SEGMENTS;

  const float theta = (ring + corner.y) * (Pi / SPHERE_RINGS);
  const float phi = (segment + corner.x) * (2 * Pi / SPHERE_SEGMENTS);
  const float3 direction
    = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));

  const float4 s = gSpheres[sphere];
  return MakeOutput(
    s.xyz + (direction * s.w), direction, sphere / SPHERES_PER_HAND);
}

float4 HandPixelShader(VSOutput input) : SV_Target {
  const float3 n = normalize(input.mNormal);
  const float3 toEye = normalize(gEyePosition.xyz - input.mWorldPosition);
  // A soft grey body with a bright rim, like the Quest's hands; the grey body
  // keeps the hand visible over light content, the rim over dark content
  const float facing = abs(dot(n, toEye));
  const float rim = pow(1.0 - facing, 2.0);
  // Fade out toward the wrist
  const float alongHand = dot(
    input.mWorldPosition - gHandWrist[input.mHand].xyz,
    gHandAxis[input.mHand].xyz);
  const float wristFade = smoothstep(-0.01, 0.05, alongHand);
  const float alpha = saturate(
    (0.42 + (0.48 * rim)) * wristFade * gHandWrist[input.mHand].w);
  const float3 color = lerp(float3(0.42, 0.45, 0.52), float3(1, 1, 1), rim);
  return float4(color * alpha, alpha);
}

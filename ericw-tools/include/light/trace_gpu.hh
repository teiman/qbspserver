/*  Copyright (C) 2026 ericw-tools contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

// Vulkan ray-query backend for direct lighting (vklight; -cpu disables it).
//
// The GPU evaluates point lights and suns for batches of lightmap samples with the same
// math as LightFace_Entity / LightFace_Sky. Rays that meet "filtered" geometry (glass,
// fences, switchable shadows, channel masks, shadowself...) are not resolved on the GPU:
// they are reported back so the CPU can re-trace them with Embree.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

struct mbsp_t;

namespace gpu_light
{

// layouts must match gpu_shaders/direct.comp (std430)

enum light_type_t : std::uint32_t
{
    LIGHT_POINT = 0,
    LIGHT_SUN = 1,
};

enum light_flags_t : std::uint32_t
{
    LIGHT_FLAG_SPOT = 1u << 0,
    LIGHT_FLAG_BLEED = 1u << 1,
    LIGHT_FLAG_DIRT = 1u << 2, // apply dirt (ambient occlusion) scaling
    LIGHT_FLAG_DIRT_RADIUS = 1u << 3, // _dirt_off_radius / _dirt_on_radius set
    LIGHT_FLAG_SUNTEXTURE = 1u << 4, // sun only lights through sky of texture `suntexture`
};

struct light_t
{
    float pos[3] = {}; // point: origin
    float light = 0; // point: light value; sun: sunlight
    float dir[3] = {}; // point: spotvec; sun: normalized incoming direction (towards the sun)
    float maxdist = 0; // sun: trace distance
    float color[3] = {}; // 0..255
    float gate = 0;
    float scaled_atten = 0; // cfg.scaledist * atten
    float falloff = 0;
    float anglescale = 0;
    float spotfalloff = 0;
    float spotfalloff2 = 0;
    float dirtgain = 0;
    float dirtscale = 0;
    float dirt_off_radius = 0;
    float dirt_on_radius = 0;
    std::uint32_t suntexture = 0; // sky texture id (see sky_texture ids passed to init)
    std::uint32_t flags = 0;
    std::uint32_t type = LIGHT_POINT;
    std::uint32_t formula = 0; // light_formula_t
    std::uint32_t pad[3] = {};
};
static_assert(sizeof(light_t) == 112);

constexpr std::uint32_t SAMPLE_SKIP = 0xffffffffu;

struct sample_t
{
    float pos[3] = {};
    float occlusion = 0; // raw dirt value (0..1)
    float normal[3] = {};
    std::uint32_t face = SAMPLE_SKIP; // index into the batch's faces, or SAMPLE_SKIP
};
static_assert(sizeof(sample_t) == 32);

enum face_flags_t : std::uint32_t
{
    FACE_FLAG_TWOSIDED = 1u << 0,
    FACE_FLAG_NODIRT = 1u << 1,
};

struct face_t
{
    std::uint32_t entry_begin = 0; // into entries
    std::uint32_t entry_count = 0;
    std::uint32_t flags = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(face_t) == 16);

// one (face, light) pair: light index | (style slot << 24)
constexpr std::uint32_t ENTRY_SLOT_SHIFT = 24;
constexpr std::uint32_t ENTRY_LIGHT_MASK = (1u << ENTRY_SLOT_SHIFT) - 1;

struct accum_t
{
    float color[3] = {};
    float hits = 0; // number of samples-light pairs that contributed
    float direction[3] = {};
    float pad = 0;
};
static_assert(sizeof(accum_t) == 32);

// a (sample, entry) pair the GPU couldn't resolve (filtered geometry on the path)
struct retrace_t
{
    std::uint32_t sample = 0; // index into the batch's samples
    std::uint32_t entry = 0; // index into the batch's entries
};
static_assert(sizeof(retrace_t) == 8);

struct batch_t
{
    const light_t *lights = nullptr;
    std::size_t light_count = 0;
    const sample_t *samples = nullptr;
    std::size_t sample_count = 0;
    const face_t *faces = nullptr;
    std::size_t face_count = 0;
    const std::uint32_t *entries = nullptr;
    std::size_t entry_count = 0;
    std::uint32_t slot_count = 1; // style slots per sample in the output
};

struct batch_result_t
{
    // sample_count * slot_count, indexed [sample * slot_count + slot]
    const accum_t *accum = nullptr;
    const retrace_t *retraces = nullptr;
    std::size_t retrace_count = 0;
    double gpu_ms = 0;
};

struct stats_t
{
    std::size_t solid_tris = 0, sky_tris = 0, filtered_tris = 0;
};

// Builds the acceleration structure from the Embree scene (call after Embree_TraceInit).
bool init(const mbsp_t *bsp, std::string &error);
void shutdown();
bool available();
stats_t stats();

// id for light_t::suntexture of a sky texture (img::texture *); never matches if the
// texture is not used by any sky face
std::uint32_t sky_texture_id(const void *texture);

// Runs one batch. The result points into backend-owned memory, valid until the next call.
bool trace_batch(const batch_t &batch, batch_result_t &result, std::string &error);

// Surface lights and bounce lights, as LightFace_SurfaceLight. An "emitter" is one style of one
// emissive surface (surfacelight_t::per_style_t); its points are in `points`.
enum emitter_flags_t : std::uint32_t
{
    EMITTER_FLAG_OMNIDIRECTIONAL = 1u << 0,
    EMITTER_FLAG_RESCALE = 1u << 1,
};

struct emitter_t
{
    float surfnormal[3] = {};
    float intensity = 0;
    float color[3] = {};
    float atten = 1;
    std::uint32_t point_begin = 0; // into points
    std::uint32_t point_count = 0;
    std::uint32_t flags = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(emitter_t) == 48);

struct surflight_params_t
{
    float standard_scale = 1; // LightFace_SurfaceLight arguments
    float sky_scale = 1;
    float hotspot_clamp = 16;
    float gate = 0; // surflight_gate
    float sky_dist = 0; // cfg.surflightskydist
    float scaledist = 1; // cfg.scaledist
    float dirtgain = 1; // Dirt_GetScaleFactor without an entity (global settings)
    float dirtscale = 1;
    std::uint32_t dirt = 0; // dirt_in_use
};

struct surflight_batch_t
{
    const emitter_t *emitters = nullptr; // entries index these (| slot << ENTRY_SLOT_SHIFT)
    std::size_t emitter_count = 0;
    const float *points = nullptr; // point_count * 4 floats (xyz, unused)
    std::size_t point_count = 0;
    const sample_t *samples = nullptr;
    std::size_t sample_count = 0;
    const face_t *faces = nullptr; // FACE_FLAG_TWOSIDED / FACE_FLAG_NODIRT as for direct lighting
    std::size_t face_count = 0;
    const std::uint32_t *entries = nullptr;
    std::size_t entry_count = 0;
    std::uint32_t slot_count = 1;
    surflight_params_t params;
};

// accum color/hits per (sample, slot); direction is unused. A retrace means: redo this
// (sample, emitter) pair on the CPU; the GPU added nothing for it.
bool trace_surflight(const surflight_batch_t &batch, batch_result_t &result, std::string &error);

// dirt (ambient occlusion), as LightFace_CalculateDirt with dirtmode 0
struct dirt_batch_t
{
    const sample_t *samples = nullptr; // face == SAMPLE_SKIP for occluded samples
    std::size_t sample_count = 0;
    const float *vectors = nullptr; // vector_count * 4 floats (xyz, unused)
    std::size_t vector_count = 0;
    float depth = 0; // dirtdepth
};

// per sample: occlusion (0..1), or DIRT_NEEDS_CPU if a ray met filtered geometry
constexpr float DIRT_NEEDS_CPU = -1.0f;

struct dirt_result_t
{
    const float *occlusion = nullptr; // sample_count values, valid until the next call
    double gpu_ms = 0;
};

bool trace_dirt(const dirt_batch_t &batch, dirt_result_t &result, std::string &error);

} // namespace gpu_light

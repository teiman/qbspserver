/*  Copyright (C) 1996-1997  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <light/ltface.hh>
#include <light/trace_gpu.hh>

#include <light/light.hh>
#include <light/trace_embree.hh>
#include <light/phong.hh>
#include <light/surflight.hh> //mxd
#include <light/entities.hh>
#include <light/lightgrid.hh>
#include <light/trace.hh>
#include <light/write.hh> // for facesup_t

#include <common/imglib.hh>
#include <common/log.hh>
#include <common/bsputils.hh>
#include <common/qvec.hh>
#include <common/ostream.hh>
#include <common/bspxfile.hh>

#include <atomic>
#include <chrono>
#include <tbb/parallel_for.h>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <string>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <fstream>

#if 0
std::atomic<uint32_t> total_light_rays, total_light_ray_hits, total_samplepoints;
std::atomic<uint32_t> total_bounce_rays, total_bounce_ray_hits;
std::atomic<uint32_t> total_surflight_rays, total_surflight_ray_hits; // mxd
#endif

thread_local static raystream_occlusion_t occlusion_stream;
thread_local static raystream_intersection_t intersection_stream;

/* Debug helper - move elsewhere? */
void PrintFaceInfo(const mface_t *face, const mbsp_t *bsp)
{
    const mtexinfo_t *tex = &bsp->texinfo[face->texinfo];
    const char *texname = Face_TextureName(bsp, face);

    logging::print("face {}, texture {}, {} edges; vectors:\n"
                   "{}\n",
        Face_GetNum(bsp, face), texname, face->numedges, tex->vecs);

    for (int i = 0; i < face->numedges; i++) {
        int edge = bsp->dsurfedges[face->firstedge + i];
        int vert = Face_VertexAtIndex(bsp, face, i);
        const qvec3f &point = GetSurfaceVertexPoint(bsp, face, i);
        const qvec3f norm = GetSurfaceVertexNormal(bsp, face, i).normal;
        logging::print("{} {:3} ({:3.3}, {:3.3}, {:3.3}) :: normal ({:3.3}, {:3.3}, {:3.3}) :: edge {}\n",
            i ? "          " : "    verts ", vert, point[0], point[1], point[2], norm[0], norm[1], norm[2], edge);
    }
}

class position_t
{
public:
    bool m_unoccluded;
    const mface_t *m_actualFace;
    qvec3f m_position;
    qvec3f m_interpolatedNormal;

    position_t(qvec3f position)
        : m_unoccluded(false),
          m_actualFace(nullptr),
          m_position(position),
          m_interpolatedNormal({})
    {
    }

    position_t(const mface_t *actualFace, const qvec3f &position, const qvec3f &interpolatedNormal)
        : m_unoccluded(true),
          m_actualFace(actualFace),
          m_position(position),
          m_interpolatedNormal(interpolatedNormal) { };
};

static constexpr float sampleOffPlaneDist = 1.0f;

/*

 Why this is so complicated:

 - vanilla tools just did a trace from the face centroid to the desired location of the sample point.
   This doesn't work because we want to allow pillars blocking parts of the face. (func_detail_wall).

 - must avoid solutions that leak light through walls (e.g. having an interior wall
   pick up light from outside), and avoid light leaks e.g. in V-shaped sconces
   that have a light inside the "V".

 - it's critical to allow sample points to extend onto neighbouring faces,
   both for phong shading to look good, as well as general light quality

 */
static position_t PositionSamplePointOnFace(
    const mbsp_t *bsp, const mface_t *face, const bool phongShaded, const qvec3f &point, const qvec3f &modelOffset);

std::vector<const mface_t *> NeighbouringFaces_old(const mbsp_t *bsp, const mface_t *face)
{
    std::vector<const mface_t *> result;
    for (int i = 0; i < face->numedges; i++) {
        const mface_t *smoothed = Face_EdgeIndexSmoothed(bsp, face, i);
        if (smoothed != nullptr && smoothed != face) {
            result.push_back(smoothed);
        }
    }
    return result;
}

position_t CalcPointNormal(const mbsp_t *bsp, const mface_t *face, const qvec3f &origPoint, bool phongShaded,
    const faceextents_t &faceextents, int recursiondepth, const qvec3f &modelOffset)
{
    const auto &facecache = FaceCacheForFNum(Face_GetNum(bsp, face));
    const qvec4f &surfplane = facecache.plane();
    const auto &points = facecache.points();
    const auto &edgeplanes = facecache.edgePlanes();
    // const auto &neighbours = facecache.neighbours();

    // check for degenerate face
    if (points.empty() || edgeplanes.empty())
        return position_t(origPoint);

    // project `point` onto the surface plane, then lift it off again
    const qvec3f point = ProjectPointOntoPlane(surfplane, origPoint) + (qvec3f(surfplane) * sampleOffPlaneDist);

    // check if in face..
    if (EdgePlanes_PointInside(edgeplanes, point)) {
        return PositionSamplePointOnFace(bsp, face, phongShaded, point, modelOffset);
    }

#if 0
    // fixme: handle "not possible to compute"
    const qvec3f centroid = Face_Centroid(bsp, face);

    for (const neighbour_t &n : neighbours) {
        /*
         
         check if in XXX area:
         
   "in1"     "in2"
        \XXXX|
         \XXX|
          |--|----|
          |\ |    |
          |  *    |  * = centroid
          |------/
         
         */
        
        const qvec3f in1_normal = qv::cross(qv::normalize(n.p0 - centroid), facecache.normal());
        const qvec3f in2_normal = qv::cross(facecache.normal(), qv::normalize(n.p1 - centroid));
        const qvec4f in1 = MakePlane(in1_normal, n.p0);
        const qvec4f in2 = MakePlane(in2_normal, n.p1);
        
        const float in1_dist = DistAbovePlane(in1, point);
        const float in2_dist = DistAbovePlane(in2, point);
        if (in1_dist >= 0 && in2_dist >= 0) {
            const auto &n_facecache = FaceCacheForFNum(Face_GetNum(bsp, n.face));
            const qvec4f &n_surfplane = n_facecache.plane();
            const auto &n_edgeplanes = n_facecache.edgePlanes();
            
            // project `point` onto the surface plane, then lift it off again
            const qvec3f n_point = ProjectPointOntoPlane(n_surfplane, origPoint) + (qvec3f(n_surfplane) * sampleOffPlaneDist);
            
            // check if in face..
            if (EdgePlanes_PointInside(n_edgeplanes, n_point)) {
                return PositionSamplePointOnFace(bsp, n.face, phongShaded, n_point, modelOffset);
            }
        }
    }
#endif

    // not in any triangle. among the edges this point is _behind_,
    // search for the one that the point is least past the endpoints of the edge
    {
        int bestplane = -1;
        float bestdist = FLT_MAX;

        for (int i = 0; i < face->numedges; i++) {
            const qvec3f &v0 = points.at(i);
            const qvec3f &v1 = points.at((i + 1) % points.size());

            const auto edgeplane = MakeInwardFacingEdgePlane(v0, v1, surfplane);
            if (!edgeplane.first)
                continue; // degenerate edge

            const float planedist = DistAbovePlane(edgeplane.second, point);
            if (planedist < POINT_EQUAL_EPSILON) {
                // behind this plane. check whether we're between the endpoints.

                const qvec3f v0v1 = v1 - v0;
                const float v0v1dist = qv::length(v0v1);

                const float t = FractionOfLine(v0, v1, point); // t=0 for point=v0, t=1 for point=v1

                float edgedist;
                if (t < 0)
                    edgedist = fabs(t) * v0v1dist;
                else if (t > 1)
                    edgedist = t * v0v1dist;
                else
                    edgedist = 0;

                if (edgedist < bestdist) {
                    bestplane = i;
                    bestdist = edgedist;
                }
            }
        }

        if (bestplane != -1) {
            // FIXME: Also need to handle non-smoothed but same plane
            const mface_t *smoothed = Face_EdgeIndexSmoothed(bsp, face, bestplane);
            if (smoothed) {
                // try recursive search
                if (recursiondepth < 3) {
                    // call recursively to look up normal in the adjacent face
                    return CalcPointNormal(
                        bsp, smoothed, point, phongShaded, faceextents, recursiondepth + 1, modelOffset);
                }
            }
        }
    }

    // 2. Try snapping to poly

    const std::pair<int, qvec3f> closest = ClosestPointOnPolyBoundary(points, point);
    float luxelSpaceDist;
    {
        auto desired_point_in_lmspace = faceextents.worldToLMCoord(point);
        auto closest_point_on_face_in_lmspace = faceextents.worldToLMCoord(closest.second);

        luxelSpaceDist = qv::distance(desired_point_in_lmspace, closest_point_on_face_in_lmspace);
    }

    if (luxelSpaceDist <= 1) {
        // Snap it to the face edge. Add the 1 unit off plane.
        const qvec3f snapped = closest.second + (qvec3f(surfplane) * sampleOffPlaneDist);
        return PositionSamplePointOnFace(bsp, face, phongShaded, snapped, modelOffset);
    }

    // This point is too far from the polygon to be visible in game, so don't bother calculating lighting for it.
    // Dont contribute to interpolating.
    // We could safely colour it in pink for debugging.
    return position_t(point);
}

// Dump points to a .map file
static void CalcPoints_Debug(const lightsurf_t *surf, const mbsp_t *bsp)
{
    std::ofstream f("calcpoints.map");

    for (int t = 0; t < surf->height; t++) {
        for (int s = 0; s < surf->width; s++) {
            const int i = t * surf->width + s;
            const auto &sample = surf->samples[i];
            const qvec3f &point = sample.point;
            const qvec3f mangle = qv::mangle_from_vec(sample.normal);

            f << "{\n";
            f << "\"classname\" \"light\"\n";
            ewt::print(f, "\"origin\" \"{}\"\n", point);
            ewt::print(f, "\"mangle\" \"{}\"\n", mangle);
            ewt::print(f, "\"face\" \"{}\"\n", sample.realfacenum);
            ewt::print(f, "\"occluded\" \"{}\"\n", sample.occluded);
            ewt::print(f, "\"s\" \"{}\"\n", s);
            ewt::print(f, "\"t\" \"{}\"\n", t);
            f << "}\n";
        }
    }

    logging::print("wrote face {}'s sample points ({}x{}) to calcpoints.map\n", Face_GetNum(bsp, surf->face),
        surf->width, surf->height);

    PrintFaceInfo(surf->face, bsp);
}

/// Checks if the point is in any solid (solid or sky leaf)
/// 1. the world
/// 2. any shadow-casting bmodel
/// 3. the `self` model (regardless of whether it's selfshadowing)
///
/// This is used for marking sample points as occluded.
static bool Light_PointInAnySolid(const mbsp_t *bsp, const dmodelh2_t *self, const qvec3f &point)
{
    if (Light_PointInSolid(bsp, self, extended_content_flags, point))
        return true;

    auto *self_modelinfo = ModelInfoForModel(bsp, self - bsp->dmodels.data());
    if (self_modelinfo->object_channel_mask.value() == CHANNEL_MASK_DEFAULT) {
        if (Light_PointInWorld(bsp, extended_content_flags, point))
            return true;
    }

    for (const auto &modelinfo : tracelist) {
        if (modelinfo->object_channel_mask.value() != self_modelinfo->object_channel_mask.value())
            continue;

        if (Light_PointInSolid(bsp, modelinfo->model, extended_content_flags, point - modelinfo->offset)) {
            // Only mark occluded if the bmodel is fully opaque
            if (modelinfo->alpha.value() == 1.0f)
                return true;
        }
    }

    return false;
}

// precondition: `point` is on the same plane as `face` and within the bounds.
static position_t PositionSamplePointOnFace(
    const mbsp_t *bsp, const mface_t *face, const bool phongShaded, const qvec3f &point, const qvec3f &modelOffset)
{
    const auto &facecache = FaceCacheForFNum(Face_GetNum(bsp, face));
    const auto &points = facecache.points();
    const auto &normals = facecache.normals();
    const auto &edgeplanes = facecache.edgePlanes();
    const auto &plane = facecache.plane();

    if (edgeplanes.empty()) {
        // degenerate polygon
        return position_t(point);
    }

    const float planedist = DistAbovePlane(plane, point);
    if (!(fabs(planedist - sampleOffPlaneDist) <= 0.1)) {
        // something is wrong?
        return position_t(point);
    }

    const float insideDist = EdgePlanes_PointInsideDist(edgeplanes, point);
    if (insideDist < -POINT_EQUAL_EPSILON) {
        // Non-convex polygon
        return position_t(point);
    }

    const modelinfo_t *mi = ModelInfoForFace(bsp, Face_GetNum(bsp, face));
    if (mi == nullptr) {
        // missing model ("skip" faces) don't get lighting
        return position_t(point);
    }

    // Get the point normal
    qvec3f pointNormal;
    if (phongShaded) {
        const auto interpNormal = InterpolateNormal(points, normals, point);
        // We already know the point is in the face, so this should always succeed
        if (!interpNormal.first)
            return position_t(point);
        pointNormal = interpNormal.second;
    } else {
        pointNormal = plane;
    }

    const bool inSolid = Light_PointInAnySolid(bsp, mi->model, point + modelOffset);
    if (inSolid) {
#if 1
        // try +/- 0.5 units in X/Y/Z (8 tests)

        for (int x = -1; x <= 1; x += 2) {
            for (int y = -1; y <= 1; y += 2) {
                for (int z = -1; z <= 1; z += 2) {
                    const qvec3f jitter = qvec3f(x, y, z) * 0.5;
                    const qvec3f new_point = point + jitter;

                    if (!Light_PointInAnySolid(bsp, mi->model, new_point + modelOffset)) {
                        return position_t(face, new_point, pointNormal);
                    }
                }
            }
        }
#else
        // this has issues with narrow sliver-shaped faces moving the sample points a lot into vastly different lighting

        // Check distance to border
        const float distanceInside = EdgePlanes_PointInsideDist(edgeplanes, point);
        if (distanceInside < 1.0f) {
            // Point is too close to the border. Try nudging it inside.
            const auto &shrunk = facecache.pointsShrunkBy1Unit();
            if (!shrunk.empty()) {
                const pair<int, qvec3f> closest = ClosestPointOnPolyBoundary(shrunk, point);
                const qvec3f newPoint = closest.second + (qvec3f(plane) * sampleOffPlaneDist);
                if (!Light_PointInAnySolid(bsp, mi->model, newPoint + modelOffset))
                    return position_t(face, newPoint, pointNormal);
            }
        }
#endif
        return position_t(point);
    }

    return position_t(face, point, pointNormal);
}

/*
 * =================
 * CalcPoints
 * For each texture aligned grid point, back project onto the plane
 * to get the world xyz value of the sample point
 * =================
 */
static void CalcPoints(
    const modelinfo_t *modelinfo, const qvec3f &offset, lightsurf_t *surf, const mbsp_t *bsp, const mface_t *face)
{
    const settings::worldspawn_keys &cfg = *surf->cfg;

    surf->width = surf->extents.width() * light_options.extra.value();
    surf->height = surf->extents.height() * light_options.extra.value();

    const float starts = -0.5 + (0.5 / light_options.extra.value());
    const float startt = -0.5 + (0.5 / light_options.extra.value());
    const float st_step = 1.0f / light_options.extra.value();

    /* Allocate surf->points */
    size_t num_points = surf->width * surf->height;
    surf->samples.resize(num_points);

    const auto points = Face_Points(bsp, face);
    const auto edgeplanes = MakeInwardFacingEdgePlanes(points);

    for (int t = 0; t < surf->height; t++) {
        for (int s = 0; s < surf->width; s++) {
            const int i = t * surf->width + s;
            auto &sample = surf->samples[i];

            const float us = starts + s * st_step;
            const float ut = startt + t * st_step;

            sample.point =
                surf->extents.LMCoordToWorld(qvec2f(us, ut)) + surf->plane.normal; // one unit in front of face

            // do this before correcting the point, so we can wrap around the inside of pipes
            const bool phongshaded = (surf->curved && cfg.phongallowed.value());
            const auto res = CalcPointNormal(bsp, face, sample.point, phongshaded, surf->extents, 0, offset);

            sample.occluded = !res.m_unoccluded;
            sample.realfacenum = res.m_actualFace != nullptr ? Face_GetNum(bsp, res.m_actualFace) : -1;
            sample.point = res.m_position + offset;
            sample.normal = res.m_interpolatedNormal;
        }
    }

    if (dump_facenum == Face_GetNum(bsp, face)) {
        CalcPoints_Debug(surf, bsp);
    }
}

static bool Mod_LeafPvs(const mbsp_t *bsp, const mleaf_t *leaf, uint8_t *out)
{
    const size_t num_pvsclusterbytes = DecompressedVisSize(bsp);

    // init to all visible
    memset(out, 0xFF, num_pvsclusterbytes);

    if (bsp->loadversion->game->has_cluster_support) {
        auto it = UncompressedVis().find(leaf->cluster);
        if (it == UncompressedVis().end()) {
            return false;
        }

        memcpy(out, it->second.data(), num_pvsclusterbytes);
    } else {
        auto it = UncompressedVis().find(leaf->visofs);
        if (it == UncompressedVis().end()) {
            return false;
        }

        memcpy(out, it->second.data(), num_pvsclusterbytes);
    }

    return true;
}

static const std::vector<uint8_t> *Mod_LeafPvs(const mbsp_t *bsp, const mleaf_t *leaf)
{
    if (bsp->loadversion->game->create_contents_from_native(leaf->contents).is_liquid()) {
        // the liquid case is because leaf->contents might be in an opaque liquid,
        // which we typically want light to pass through, but visdata would report that
        // there's no visibility across the opaque liquid. so, skip culling and do the raytracing.
        return nullptr;
    }

    const int key = (bsp->loadversion->game->has_cluster_support) ? leaf->cluster : leaf->visofs;
    if (auto it = UncompressedVis().find(key); it != UncompressedVis().end()) {
        return &it->second;
    }
    return nullptr;
}

// face number -> leaves listing it in their marksurfaces, in leaf order (see PrepareFaceLeaves)
static const mbsp_t *face_leaves_bsp = nullptr;
static std::vector<std::vector<const mleaf_t *>> face_leaves;

void PrepareFaceLeaves(const mbsp_t *bsp)
{
    face_leaves.assign(bsp->dfaces.size(), {});
    for (auto &leaf : bsp->dleafs) {
        for (size_t surf = 0; surf < leaf.nummarksurfaces; surf++) {
            const size_t face_index = bsp->dleaffaces[leaf.firstmarksurface + surf];
            if (face_index < face_leaves.size())
                face_leaves[face_index].push_back(&leaf);
        }
    }
    face_leaves_bsp = bsp;
}

void ClearFaceLeaves()
{
    face_leaves.clear();
    face_leaves.shrink_to_fit();
    face_leaves_bsp = nullptr;
}

static void CalcPvs(const mbsp_t *bsp, lightsurf_t *lightsurf)
{
    if (!bsp->dvis.bits.size()) {
        return;
    }

    const int pvssize = DecompressedVisSize(bsp);

    // set lightsurf->pvs
    uint8_t *pointpvs = (uint8_t *)alloca(pvssize);
    lightsurf->pvs.resize(pvssize);

    if (lightsurf->modelinfo->isWorld() && face_leaves_bsp == bsp) {
        // same result as the loop below, from the index built once per map
        const size_t face_index = lightsurf->face - bsp->dfaces.data();
        lightsurf->leaves = face_leaves[face_index];
    } else if (lightsurf->modelinfo->isWorld()) {
        size_t face_index = lightsurf->face - bsp->dfaces.data();

        for (auto &leaf : bsp->dleafs) {
            for (size_t surf = 0; surf < leaf.nummarksurfaces; surf++) {
                if (bsp->dleaffaces[leaf.firstmarksurface + surf] == face_index) {
                    lightsurf->leaves.push_back(&leaf);
                }
            }
        }
    } else {
        for (auto &sample : lightsurf->samples) {
            const mleaf_t *leaf = Light_PointInLeaf(bsp, sample.point);

            if (std::find(lightsurf->leaves.begin(), lightsurf->leaves.end(), leaf) == lightsurf->leaves.end()) {
                lightsurf->leaves.push_back(leaf);
            }
        }
    }

    for (auto &leaf : lightsurf->leaves) {
        /* copy the pvs for this leaf into pointpvs */
        Mod_LeafPvs(bsp, leaf, pointpvs);

        if (bsp->loadversion->game->create_contents_from_native(leaf->contents).is_liquid()) {
            // hack for when the sample point might be in an opaque liquid, blocking vis,
            // but we typically want light to pass through these.
            // see also VisCullEntity() which handles the case when the light emitter is in liquid.
            for (int j = 0; j < pvssize; j++) {
                lightsurf->pvs[j] |= 0xff;
            }
            break;
        }

        /* merge the pvs for this sample point into lightsurf->pvs */
        for (int j = 0; j < pvssize; j++) {
            lightsurf->pvs[j] |= pointpvs[j];
        }
    }

    lightsurf->leaves.shrink_to_fit();
}

static lightsurf_t Lightsurf_Init(const modelinfo_t *modelinfo, const settings::worldspawn_keys &cfg,
    const mface_t *face, const mbsp_t *bsp, const facesup_t *facesup,
    const bspx_decoupled_lm_perface *facesup_decoupled)
{
    auto spaceToWorld = TexSpaceToWorld(bsp, face);

    /* Check for invalid texture axes */
    if (std::isnan(spaceToWorld.at(0, 0))) {
        logging::print("Bad texture axes on face:\n");
        PrintFaceInfo(face, bsp);
        return {};
    }

    lightsurf_t lightsurf;
    lightsurf.cfg = &cfg;
    lightsurf.modelinfo = modelinfo;
    lightsurf.bsp = bsp;
    lightsurf.face = face;

    if (Face_IsLightmapped(bsp, face)) {
        /* if liquid doesn't have the TEX_SPECIAL flag set, the map was qbsp'ed with
         * lit water in mind. In that case receive light from both top and bottom.
         * (lit will only be rendered in compatible engines, but degrades gracefully.)
         */
        lightsurf.twosided = Face_IsTranslucent(bsp, face);

        // pick the larger of the two scales
        lightsurf.lightmapscale =
            (facesup && facesup->lmscale < modelinfo->lightmapscale) ? facesup->lmscale : modelinfo->lightmapscale;

        const surfflags_t &extended_flags = extended_texinfo_flags[face->texinfo];
        lightsurf.curved = extended_flags.phong_angle != 0 || Q2_FacePhongValue(bsp, face);

        // override the autodetected twosided setting?
        if (extended_flags.light_twosided) {
            lightsurf.twosided = *extended_flags.light_twosided;
        }

        // nodirt
        if (modelinfo->dirt.is_changed()) {
            lightsurf.nodirt = (modelinfo->dirt.value() == -1);
        } else {
            lightsurf.nodirt = extended_flags.no_dirt;
        }

        // minlight
        if (modelinfo->minlight.is_changed()) {
            lightsurf.minlight = modelinfo->minlight.value();
        } else if (extended_flags.minlight) {
            lightsurf.minlight = *extended_flags.minlight;
        } else {
            lightsurf.minlight = light_options.minlight.value();
        }

        // minlightMottle
        if (modelinfo->minlightMottle.is_changed()) {
            lightsurf.minlightMottle = modelinfo->minlightMottle.value();
        } else if (light_options.minlightMottle.is_changed()) {
            lightsurf.minlightMottle = light_options.minlightMottle.value();
        } else {
            lightsurf.minlightMottle = false;
        }

        // Q2 uses a 0-1 range for minlight
        if (bsp->loadversion->game->id == GAME_QUAKE_II) {
            lightsurf.minlight *= 128.f;
        }

        // maxlight
        if (modelinfo->maxlight.is_changed()) {
            lightsurf.maxlight = modelinfo->maxlight.value();
        } else {
            lightsurf.maxlight = extended_flags.maxlight;
        }

        // lightcolorscale
        if (modelinfo->lightcolorscale.is_changed()) {
            lightsurf.lightcolorscale = modelinfo->lightcolorscale.value();
        } else {
            lightsurf.lightcolorscale = extended_flags.lightcolorscale;
        }

        // Q2 uses a 0-1 range for minlight
        if (bsp->loadversion->game->id == GAME_QUAKE_II) {
            lightsurf.maxlight *= 128.f;
        }

        // minlight_color
        if (modelinfo->minlight_color.is_changed()) {
            lightsurf.minlight_color = modelinfo->minlight_color.value();
        } else if (!qv::emptyExact(extended_flags.minlight_color)) {
            lightsurf.minlight_color = extended_flags.minlight_color;
        } else {
            lightsurf.minlight_color = light_options.minlight_color.value();
        }

        /* never receive dirtmapping on lit liquids */
        if (Face_IsTranslucent(bsp, face)) {
            lightsurf.nodirt = true;
        }

        /* handle glass alpha */
        if (modelinfo->alpha.value() < 1) {
            /* skip culling of rays coming from the back side of the face */
            lightsurf.twosided = true;
        }

        /* object channel mask */
        if (extended_flags.object_channel_mask) {
            lightsurf.object_channel_mask = *extended_flags.object_channel_mask;
        } else {
            lightsurf.object_channel_mask = modelinfo->object_channel_mask.value();
        }

        if (extended_flags.surflight_minlight_scale) {
            lightsurf.surflight_minlight_scale = *extended_flags.surflight_minlight_scale;
        } else {
            lightsurf.surflight_minlight_scale = 1.0f;
        }

        /* Set up the plane, not including model offset */
        qplane3f &plane = lightsurf.plane;
        if (face->side) {
            plane = -bsp->dplanes[face->planenum];
        } else {
            plane = bsp->dplanes[face->planenum];
        }

        const mtexinfo_t *tex = &bsp->texinfo[face->texinfo];
        lightsurf.snormal = qv::normalize(tex->vecs.row(0).xyz());
        lightsurf.tnormal = -qv::normalize(tex->vecs.row(1).xyz());

        /* Set up the surface points */
        if (light_options.world_units_per_luxel.is_changed()) {
            if (bsp->loadversion->game->id == GAME_QUAKE_II &&
                (Face_Texinfo(bsp, face)->flags.native_q2 & Q2_SURF_SKY)) {
                lightsurf.extents = faceextents_t(*face, *bsp, world_units_per_luxel_t{}, 512.f);
            } else if (extended_flags.world_units_per_luxel) {
                lightsurf.extents =
                    faceextents_t(*face, *bsp, world_units_per_luxel_t{}, *extended_flags.world_units_per_luxel);
            } else {
                lightsurf.extents =
                    faceextents_t(*face, *bsp, world_units_per_luxel_t{}, light_options.world_units_per_luxel.value());
            }
        } else {
            lightsurf.extents = faceextents_t(*face, *bsp, lightsurf.lightmapscale);
        }
        lightsurf.vanilla_extents = faceextents_t(*face, *bsp, LMSCALE_DEFAULT);

        CalcPoints(modelinfo, modelinfo->offset, &lightsurf, bsp, face);

        /* Correct the plane for the model offset (must be done last,
           calculation of face extents / points needs the uncorrected plane) */
        qvec3f planepoint = (plane.normal * plane.dist) + modelinfo->offset;
        plane.dist = qv::dot(plane.normal, planepoint);

        /* Correct bounding sphere */
        lightsurf.extents.origin += modelinfo->offset;
        lightsurf.extents.bounds = lightsurf.extents.bounds.translate(modelinfo->offset);

        intersection_stream.resize(lightsurf.samples.size());
        occlusion_stream.resize(lightsurf.samples.size());

        /* Setup vis data */
        CalcPvs(bsp, &lightsurf);
    }

    // emissiveness is handled later and allocated only if necessary

    return lightsurf;
}

static void Lightmap_AllocOrClear(lightmap_t *lightmap, const lightsurf_t *lightsurf)
{
    if (!lightmap->samples.size()) {
        /* first use of this lightmap, allocate the storage for it. */
        lightmap->samples.resize(lightsurf->samples.size());
    } else if (lightmap->style != INVALID_LIGHTSTYLE) {
        /* clear only the data that is going to be merged to it. there's no point clearing more */
        std::fill_n(lightmap->samples.begin(), lightsurf->samples.size(), lightsample_t{});
        lightmap->bounce_color = {};
    }
}

#if 0
static const lightmap_t *Lightmap_ForStyle_ReadOnly(const lightsurf_t *lightsurf, const int style)
{
    for (const auto &lm : lightsurf->lightmapsByStyle) {
        if (lm.style == style)
            return &lm;
    }
    return nullptr;
}
#endif

/*
 * Lightmap_ForStyle
 *
 * If lightmap with given style has already been allocated, return it.
 * Otherwise, return the next available map.  A new map is not marked as
 * allocated since it may not be kept if no lights hit.
 */
lightmap_t *Lightmap_ForStyle(lightmapdict_t *lightmaps, const int style, const lightsurf_t *lightsurf)
{
    for (auto &lm : *lightmaps) {
        if (lm.style == style)
            return &lm;
    }

    // no exact match, check for an unsaved one
    for (auto &lm : *lightmaps) {
        if (lm.style == INVALID_LIGHTSTYLE) {
            Lightmap_AllocOrClear(&lm, lightsurf);
            return &lm;
        }
    }

    // add a new one to the vector (invalidates existing lightmap_t pointers)
    lightmap_t &newLightmap = lightmaps->emplace_back();
    newLightmap.style = INVALID_LIGHTSTYLE;
    Lightmap_AllocOrClear(&newLightmap, lightsurf);
    return &newLightmap;
}

/*
 * Lightmap_Save
 *
 * As long as we have space for the style, mark as allocated,
 * otherwise emit a warning.
 */
static void Lightmap_Save(
    const mbsp_t *bsp, lightmapdict_t *lightmaps, const lightsurf_t *lightsurf, lightmap_t *lightmap, const int style)
{
    Q_assert(Face_IsLightmapped(bsp, lightsurf->face));

    if (lightmap->style == INVALID_LIGHTSTYLE) {
        lightmap->style = style;
    }
}

/*
 * ============================================================================
 * FACE LIGHTING
 * ============================================================================
 */

// returns the light contribution at a given distance, without regard for angle
static float GetLightValue(const settings::worldspawn_keys &cfg, const light_formula_t formula, const float light,
    const float falloff, const float atten, const float dist, const float hotspot_clamp)
{
    if (formula == LF_INFINITE || formula == LF_LOCALMIN)
        return light;

    // mxd. Apply falloff?
    if (falloff > 0.0f) {
        if (formula == LF_LINEAR) {
            // Light can affect surface?
            if (falloff > dist)
                return light * (1.0f - (dist / falloff));
            else
                return 0.0f; // Surface is unaffected
        }
    }

    float value = cfg.scaledist.value() * atten * dist;

    switch (formula) {
        case LF_INVERSE: return light / (value / hotspot_clamp);
        case LF_INVERSE2A:
            value += hotspot_clamp;
            /* Fall through */
        case LF_INVERSE2: return light / ((value * value) / (hotspot_clamp * hotspot_clamp));
        case LF_LINEAR:
            if (light > 0)
                return (light - value > 0) ? light - value : 0;
            else
                return (light + value < 0) ? light + value : 0;
        case LF_QRAD3: {
            const float d = std::max(value, hotspot_clamp); // Clamp away hotspots, also avoid division by 0...
            return light / (d * d);
        }
        default: Error("Internal error: unknown light formula");
    }
}

// mxd. Surface light falloff. Returns color in [0,255]
inline qvec3f SurfaceLight_ColorAtDist(const settings::worldspawn_keys &cfg, float surf_scale, float intensity,
    const qvec3f &color, float dist, float atten, float hotspot_clamp)
{
    const float v = GetLightValue(cfg, LF_QRAD3, intensity, 0.0f, atten, dist, hotspot_clamp) * surf_scale;
    return color * v;
}

static float GetLightValueWithAngle(const settings::worldspawn_keys &cfg, const light_formula_t formula,
    const float light, const float falloff, const float atten, const bool bleed, const float anglescale,
    const qvec3f &surfnorm, bool use_surfnorm, const qvec3f &surfpointToLightDir, const float dist, bool twosided,
    const float hotspot_clamp)
{
    float angle;

    if (use_surfnorm) {
        angle = qv::dot(surfpointToLightDir, surfnorm);
    } else {
        angle = 1.0f;
    }

    if (bleed || twosided) {
        if (angle < 0) {
            angle = -angle; // ericw -- support "_bleed" option
        }
    }

    /* Light behind sample point? Zero contribution, period.
       see: https://github.com/ericwa/ericw-tools/issues/181 */
    if (angle < 0) {
        return 0;
    }

    /* Apply anglescale */
    angle = (1.0 - anglescale) + (anglescale * angle);

    return GetLightValue(cfg, formula, light, falloff, atten, dist, hotspot_clamp) * angle;
}

// returns the light contribution at a given distance, without regard for angle
static float GetLightValue(const settings::worldspawn_keys &cfg, const light_t *entity, const float dist)
{
    return GetLightValue(cfg, entity->getFormula(), entity->light.value(), entity->falloff.value(),
        entity->atten.value(), dist, LF_SCALE);
}

static float GetLightValueWithAngle(const settings::worldspawn_keys &cfg, const light_t *entity, const qvec3f &surfnorm,
    bool use_surfnorm, const qvec3f &surfpointToLightDir, float dist, bool twosided)
{
    float value = GetLightValueWithAngle(cfg, entity->formula.value(), entity->light.value(), entity->falloff.value(),
        entity->atten.value(), entity->bleed.value(), entity->anglescale.value(), surfnorm, use_surfnorm,
        surfpointToLightDir, dist, twosided, LF_SCALE);

    /* Check spotlight cone */
    float spotscale = 1;

    if (entity->spotlight) {
        const float falloff = qv::dot(entity->spotvec, surfpointToLightDir);
        if (falloff > entity->spotfalloff) {
            return 0;
        }
        if (falloff > entity->spotfalloff2) {
            /* Interpolate between the two spotlight falloffs */
            spotscale = falloff - entity->spotfalloff2;
            spotscale /= entity->spotfalloff - entity->spotfalloff2;
            spotscale = 1.0 - spotscale;
        }
    }

    return value * spotscale;
}

template<typename T>
static void Matrix4x4_CM_Transform4(const std::array<T, 16> &matrix, const qvec<T, 4> &vector, qvec<T, 4> &product)
{
    product[0] = matrix[0] * vector[0] + matrix[4] * vector[1] + matrix[8] * vector[2] + matrix[12] * vector[3];
    product[1] = matrix[1] * vector[0] + matrix[5] * vector[1] + matrix[9] * vector[2] + matrix[13] * vector[3];
    product[2] = matrix[2] * vector[0] + matrix[6] * vector[1] + matrix[10] * vector[2] + matrix[14] * vector[3];
    product[3] = matrix[3] * vector[0] + matrix[7] * vector[1] + matrix[11] * vector[2] + matrix[15] * vector[3];
}

template<typename T>
static bool Matrix4x4_CM_Project(const qvec<T, 3> &in, qvec<T, 3> &out, const std::array<T, 16> &modelviewproj)
{
    bool result = true;

    qvec<T, 4> v;
    Matrix4x4_CM_Transform4(modelviewproj, {in, 1}, v);

    v[0] /= v[3];
    v[1] /= v[3];
    if (v[2] < 0)
        result = false; // too close to the view
    v[2] /= v[3];

    out[0] = (1 + v[0]) / 2;
    out[1] = (1 + v[1]) / 2;
    out[2] = (1 + v[2]) / 2;
    if (out[2] > 1)
        result = false; // beyond far clip plane
    return result;
}

static bool LightFace_SampleMipTex(
    const img::texture *tex, const std::array<float, 16> &projectionmatrix, const qvec3f &point, qvec3f &result)
{
    // okay, yes, this is weird, yes we're using a vec3_t for a coord...
    // this is because we're treating it like a cubemap. why? no idea.
    float weight[4];
    qvec4b pi[4];

    qvec3f coord;
    if (!Matrix4x4_CM_Project(point, coord, projectionmatrix) || coord[0] <= 0 || coord[0] >= 1 || coord[1] <= 0 ||
        coord[1] >= 1) {
        result = {};
        return false; // mxd
    }

    float sfrac = (coord[0]) * (tex->meta.width - 1); // mxd. We are sampling sbase+1 pixels, so multiplying by
                                                      // tex->width will result in an 1px overdraw, same for tbase
    const int sbase = sfrac;
    sfrac -= sbase;
    float tfrac = (1 - coord[1]) * (tex->meta.height - 1);
    const int tbase = tfrac;
    tfrac -= tbase;

    pi[0] = tex->pixels[((sbase + 0) % tex->meta.width) + (tex->meta.width * ((tbase + 0) % tex->meta.height))];
    weight[0] = (1 - sfrac) * (1 - tfrac);
    pi[1] = tex->pixels[((sbase + 1) % tex->meta.width) + (tex->meta.width * ((tbase + 0) % tex->meta.height))];
    weight[1] = (sfrac) * (1 - tfrac);
    pi[2] = tex->pixels[((sbase + 0) % tex->meta.width) + (tex->meta.width * ((tbase + 1) % tex->meta.height))];
    weight[2] = (1 - sfrac) * (tfrac);
    pi[3] = tex->pixels[((sbase + 1) % tex->meta.width) + (tex->meta.width * ((tbase + 1) % tex->meta.height))];
    weight[3] = (sfrac) * (tfrac);
    result = pi[0].xyz() * weight[0] + pi[1].xyz() * weight[1] + pi[2].xyz() * weight[2] + pi[3].xyz() * weight[3];
    result *= 2;

    return true;
}

static void GetLightContrib(const settings::worldspawn_keys &cfg, const light_t *entity, const qvec3f &surfnorm,
    bool use_surfnorm, const qvec3f &surfpoint, bool twosided, qvec3f &color_out, qvec3f &surfpointToLightDir_out,
    qvec3f &normalmap_addition_out, float *dist_out)
{
    float dist = GetDir(surfpoint, entity->origin.value(), surfpointToLightDir_out);
    if (dist < 0.1) {
        // Catch 0 distance between sample point and light (produces infinite brightness / nan's) and causes
        // problems later
        dist = 0.1f;
        surfpointToLightDir_out = {0, 0, 1};
    }
    const float add =
        GetLightValueWithAngle(cfg, entity, surfnorm, use_surfnorm, surfpointToLightDir_out, dist, twosided);

    /* write out the final color */
    if (entity->projectedmip) {
        qvec3f col;
        if (LightFace_SampleMipTex(entity->projectedmip, entity->projectionmatrix, surfpoint, col)) {
            // mxd. Modulate by light color...
            const auto &entcol = entity->color.value();
            for (int i = 0; i < 3; i++)
                col[i] *= entcol[i] * (1.0f / 255.0f);
        }

        color_out = col * add * (1.0f / 255.0f);
    } else {
        color_out = entity->color.value() * add * (1.0f / 255.0f);
    }

    // write normalmap contrib
    normalmap_addition_out = surfpointToLightDir_out * add;

    *dist_out = dist;
}

constexpr float SQR(float x)
{
    return x * x;
}

// CHECK: naming? why clamp*min*?
constexpr bool Light_ClampMin(lightsample_t &sample, const float light, const qvec3f &color)
{
    bool changed = false;

    for (int i = 0; i < 3; i++) {
        float c = (float)(color[i] * (light / 255.0f));

        if (c > sample.color[i]) {
            sample.color[i] = c;
            changed = true;
        }
    }

    return changed;
}

constexpr float fraction(float min, float val, float max)
{
    if (val >= max)
        return 1.0;
    if (val <= min)
        return 0.0;

    return (val - min) / (max - min);
}

/*
 * ============
 * Dirt_GetScaleFactor
 *
 * returns scale factor for dirt/ambient occlusion
 * ============
 */
inline float Dirt_GetScaleFactor(const settings::worldspawn_keys &cfg, float occlusion, const light_t *entity,
    const float entitydist, const lightsurf_t *surf)
{
    float light_dirtgain = cfg.dirtgain.value();
    float light_dirtscale = cfg.dirtscale.value();
    bool usedirt;

    /* is dirt processing disabled entirely? */
    if (!dirt_in_use)
        return 1.0f;
    if (surf && surf->nodirt)
        return 1.0f;

    /* should this light be affected by dirt? */
    if (entity) {
        if (entity->dirt.value() == -1) {
            usedirt = false;
        } else if (entity->dirt.value() == 1) {
            usedirt = true;
        } else {
            usedirt = cfg.dirt.value();
        }
    } else {
        /* no entity is provided, assume the caller wants dirt */
        usedirt = true;
    }

    /* if not, quit */
    if (!usedirt)
        return 1.0;

    /* override the global scale and gain values with the light-specific
       values, if present */
    if (entity) {
        if (entity->dirtgain.value())
            light_dirtgain = entity->dirtgain.value();
        if (entity->dirtscale.value())
            light_dirtscale = entity->dirtscale.value();
    }

    /* early out */
    if (occlusion <= 0.0f) {
        return 1.0f;
    }

    /* apply gain (does this even do much? heh) */
    float outDirt = pow(occlusion, light_dirtgain);
    if (outDirt > 1.0f) {
        outDirt = 1.0f;
    }

    /* apply scale */
    outDirt *= light_dirtscale;
    if (outDirt > 1.0f) {
        outDirt = 1.0f;
    }

    /* lerp based on distance to light */
    if (entity) {
        // From 0 to _dirt_off_radius units, no dirt.
        // From _dirt_off_radius to _dirt_on_radius, the dirt linearly ramps from 0 to full, and after _dirt_on_radius,
        // it's full dirt.

        if (entity->dirt_on_radius.is_changed() && entity->dirt_off_radius.is_changed()) {

            const float onRadius = entity->dirt_on_radius.value();
            const float offRadius = entity->dirt_off_radius.value();

            if (entitydist < offRadius) {
                outDirt = 0.0;
            } else if (entitydist >= offRadius && entitydist < onRadius) {
                const float frac = fraction(offRadius, entitydist, onRadius);
                outDirt = frac * outDirt;
            }
        }
    }

    /* return to sender */
    return 1.0f - outDirt;
}

/*
 * ================
 * CullLight
 *
 * Returns true if the given light doesn't reach lightsurf.
 * ================
 */
inline bool CullLight(const light_t *entity, const lightsurf_t *lightsurf)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;

    if (light_options.visapprox.value() == visapprox_t::RAYS &&
        entity->bounds.disjoint(lightsurf->extents.bounds, 0.001f) &&
        entity->light_channel_mask.value() == CHANNEL_MASK_DEFAULT &&
        entity->shadow_channel_mask.value() == CHANNEL_MASK_DEFAULT) {
        // EstimateVisibleBoundsAtPoint uses CHANNEL_MASK_DEFAULT
        // for its rays, so only cull lights that are also going to be using
        // CHANNEL_MASK_DEFAULT for rendering
        return true;
    }

    qvec3f distvec = entity->origin.value() - lightsurf->extents.origin;
    const float dist = qv::length(distvec) - lightsurf->extents.radius;

    /* light is inside surface bounding sphere => can't cull */
    if (dist < 0) {
        return false;
    }

    /* return true if the light level at the closest point on the
     surface bounding sphere to the light source is <= fadegate.
     need fabs to handle antilights. */
    return fabs(GetLightValue(cfg, entity, dist)) <= light_options.gate.value();
}

static bool VisCullEntity(const mbsp_t *bsp, const std::vector<uint8_t> &pvs, const mleaf_t *entleaf)
{
    if (pvs.empty()) {
        return false;
    }
    if (entleaf == nullptr) {
        return false;
    }

    auto contents = bsp->loadversion->game->create_contents_from_native(entleaf->contents);

    if (contents.is_solid() || contents.is_sky() || contents.is_liquid()) {
        // the liquid case is because entleaf->contents might be in an opaque liquid,
        // which we typically want light to pass through, but visdata would report that
        // there's no visibility across the opaque liquid. so, skip culling and do the raytracing.
        return false;
    }

    return !Pvs_LeafVisible(bsp, pvs, entleaf);
}

/*
 * ================
 * LightFace_Entity
 * ================
 */
// only_samples: if set, only these sample indices are lit (used to re-trace GPU results)
static void LightFace_Entity(const mbsp_t *bsp, const light_t *entity, lightsurf_t *lightsurf,
    lightmapdict_t *lightmaps, const std::vector<int> *only_samples = nullptr)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;
    const modelinfo_t *modelinfo = lightsurf->modelinfo;
    const qplane3f &plane = lightsurf->plane;

    /* vis cull */
    if (light_options.visapprox.value() == visapprox_t::VIS &&
        entity->light_channel_mask.value() == CHANNEL_MASK_DEFAULT &&
        entity->shadow_channel_mask.value() == CHANNEL_MASK_DEFAULT &&
        VisCullEntity(bsp, lightsurf->pvs, entity->leaf)) {
        return;
    }

    const float planedist = plane.distance_to(entity->origin.value());

    /* don't bother with lights behind the surface.

       if the surface is curved, the light may be behind the surface, but it may
       still have a line of sight to a samplepoint, and that sample point's
       normal may be facing such that it receives some light, so we can't use this
       test in the curved case.
    */
    if (planedist < 0 && !entity->bleed.value() && !lightsurf->curved && !lightsurf->twosided) {
        return;
    }

    /* sphere cull surface and light */
    if (CullLight(entity, lightsurf)) {
        return;
    }

    // check lighting channels
    if (!(entity->light_channel_mask.value() & lightsurf->object_channel_mask)) {
        return;
    }

    /*
     * Check it for real
     */
    raystream_occlusion_t &rs = occlusion_stream;
    rs.clearPushedRays();

    const int num_samples = only_samples ? only_samples->size() : lightsurf->samples.size();
    for (int n = 0; n < num_samples; n++) {
        const int i = only_samples ? (*only_samples)[n] : n;
        const auto &sample = lightsurf->samples[i];

        if (sample.occluded)
            continue;

        const qvec3f &surfpoint = sample.point;
        const qvec3f &surfnorm = sample.normal;

        qvec3f surfpointToLightDir;
        float surfpointToLightDist;
        qvec3f color;
        qvec3f normalcontrib;

        GetLightContrib(cfg, entity, surfnorm, true, surfpoint, lightsurf->twosided, color, surfpointToLightDir,
            normalcontrib, &surfpointToLightDist);

        const float occlusion = Dirt_GetScaleFactor(cfg, sample.occlusion, entity, surfpointToLightDist, lightsurf);
        color *= occlusion;

        /* Quick distance check first */
        if (fabs(LightSample_Brightness(color)) <= light_options.gate.value()) {
            continue;
        }

        rs.pushRay(i, surfpoint, surfpointToLightDir, surfpointToLightDist, &color, &normalcontrib);
    }

    // don't need closest hit, just checking for occlusion between light and surface point
    rs.tracePushedRaysOcclusion(modelinfo, entity->shadow_channel_mask.value());
#if 0
    total_light_rays += rs.numPushedRays();
#endif

    int cached_style = entity->style.value();
    lightmap_t *cached_lightmap = Lightmap_ForStyle(lightmaps, cached_style, lightsurf);

    const int N = rs.numPushedRays();
    for (int j = 0; j < N; j++) {
        if (rs.getPushedRayOccluded(j)) {
            continue;
        }

#if 0
        total_light_ray_hits++;
#endif

        const ray_io &ray = rs.getRay(j);

        int i = ray.index;

        // check if we hit a dynamic shadow caster (only applies to style 0 lights)
        //
        // note, this still works even though we're doing an occlusion trace - closest
        // hit doesn't matter. All that matters is whether there is a real (solid) occluder
        // between the ray start and end.
        //
        // If there is, the light is fully blocked and we bail out above, regardless of any
        // dynamic shadow casters that also might be along the ray.
        //
        // If not, then we are guaranteed to detect the dynamic shadow caster in the ray filter
        // (if any), and handle it here.
        int desired_style = entity->style.value();
        if (desired_style == 0) {
            desired_style = ray.dynamic_style;
        }

        // if necessary, switch which lightmap we are writing to.
        if (desired_style != cached_style) {
            cached_style = desired_style;
            cached_lightmap = Lightmap_ForStyle(lightmaps, cached_style, lightsurf);
        }

        lightsample_t &sample = cached_lightmap->samples[i];

        sample.color += rs.getPushedRayColor(j);
        cached_lightmap->bounce_color += rs.getPushedRayColor(j);
        sample.direction += ray.normalcontrib;

        Lightmap_Save(bsp, lightmaps, lightsurf, cached_lightmap, cached_style);
    }
}

/**
 * Calculates light at a given point from an entity
 */
static void LightPoint_Entity(const mbsp_t *bsp, raystream_occlusion_t &rs, const light_t *entity,
    const qvec3f &surfpoint, lightgrid_samples_t &result)
{
    // check lighting channels
    if (entity->light_channel_mask.value() != CHANNEL_MASK_DEFAULT) {
        return;
    }

    rs.clearPushedRays();

    qvec3f surfpointToLightDir;
    float surfpointToLightDist;
    qvec3f color;

    qvec3f normalcontrib_unused;

    GetLightContrib(light_options, entity, qvec3f(0, 0, 0), false, surfpoint, false, color, surfpointToLightDir,
        normalcontrib_unused, &surfpointToLightDist);

    const float anglescale = entity->anglescale.value();

    /* Quick distance check first */
    if (fabs(LightSample_Brightness(color)) <= light_options.gate.value()) {
        return;
    }

    rs.pushRay(0, surfpoint, surfpointToLightDir, surfpointToLightDist, &color);

    rs.tracePushedRaysOcclusion(nullptr, CHANNEL_MASK_DEFAULT);

    // add result
    const int N = rs.numPushedRays();
    for (int j = 0; j < N; j++) {
        if (rs.getPushedRayOccluded(j)) {
            continue;
        }

        result.add(rs.getPushedRayColor(j), entity->style.value(), surfpointToLightDir, anglescale);
    }
}

/*
 * =============
 * LightFace_Sky
 * =============
 */
// only_samples: if set, only these sample indices are lit (used to re-trace GPU results)
static void LightFace_Sky(const mbsp_t *bsp, const sun_t *sun, lightsurf_t *lightsurf, lightmapdict_t *lightmaps,
    const std::vector<int> *only_samples = nullptr)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;
    const modelinfo_t *modelinfo = lightsurf->modelinfo;
    const qplane3f &plane = lightsurf->plane;

    // FIXME: Normalized sun vector should be stored in the sun_t. Also clarify which way the vector points (towards or
    // away..)
    // FIXME: Much of this is copied/pasted from LightFace_Entity, should probably be merged
    qvec3f incoming = qv::normalize(sun->sunvec);

    /* Don't bother if surface facing away from sun */
    const float dp = qv::dot(incoming, plane.normal);
    if (dp < -LIGHT_ANGLE_EPSILON && !lightsurf->curved && !lightsurf->twosided) {
        return;
    }

    // check lighting channels (currently sunlight is always on CHANNEL_MASK_DEFAULT)
    if (!(lightsurf->object_channel_mask & CHANNEL_MASK_DEFAULT)) {
        return;
    }

    /* Check each point... */
    raystream_intersection_t &rs = intersection_stream;
    rs.clearPushedRays();

    const int num_samples = only_samples ? only_samples->size() : lightsurf->samples.size();
    for (int n = 0; n < num_samples; n++) {
        const int i = only_samples ? (*only_samples)[n] : n;
        const auto &sample = lightsurf->samples[i];

        if (sample.occluded)
            continue;

        const qvec3f &surfpoint = sample.point;
        const qvec3f &surfnorm = sample.normal;

        float angle = qv::dot(incoming, surfnorm);
        if (lightsurf->twosided) {
            if (angle < 0) {
                angle = -angle;
            }
        }

        angle = std::max(0.0f, angle);

        angle = (1.0f - sun->anglescale) + sun->anglescale * angle;
        float value = angle * sun->sunlight;

        if (sun->dirt) {
            value *= Dirt_GetScaleFactor(cfg, sample.occlusion, NULL, 0.0f, lightsurf);
        }

        qvec3f color = sun->sunlight_color * (value / 255.0f);

        /* Quick distance check first */
        if (fabs(LightSample_Brightness(color)) <= light_options.gate.value()) {
            continue;
        }

        qvec3f normalcontrib = incoming * value;

        rs.pushRay(i, surfpoint, incoming, MAX_SKY_DIST, &color, &normalcontrib);
    }

    // We need to check if the first hit face is a sky face, so we need
    // to test intersection (not occlusion)
    rs.tracePushedRaysIntersection(modelinfo, CHANNEL_MASK_DEFAULT);

    /* if sunlight is set, use a style 0 light map */
    int cached_style = sun->style;
    lightmap_t *cached_lightmap = Lightmap_ForStyle(lightmaps, cached_style, lightsurf);

    const int N = rs.numPushedRays();
#if 0
    total_light_rays += N;
#endif

    for (int j = 0; j < N; j++) {
        if (rs.getPushedRayHitType(j) != hittype_t::SKY) {
            continue;
        }

        // check if we hit the wrong texture
        if (sun->suntexture_value) {
            const triinfo *face = rs.getPushedRayHitFaceInfo(j);
            if (sun->suntexture_value != face->texture) {
                continue;
            }
        }

        const ray_io &ray = rs.getRay(j);
        const int i = ray.index;

        // check if we hit a dynamic shadow caster
        int desired_style = sun->style;
        if (desired_style == 0) {
            desired_style = ray.dynamic_style;
        }

        // if necessary, switch which lightmap we are writing to.
        if (desired_style != cached_style) {
            cached_style = desired_style;
            cached_lightmap = Lightmap_ForStyle(lightmaps, cached_style, lightsurf);
        }

        lightsample_t &sample = cached_lightmap->samples[i];

        sample.color += rs.getPushedRayColor(j);
        cached_lightmap->bounce_color += rs.getPushedRayColor(j);
        sample.direction += ray.normalcontrib;
#if 0
        total_light_ray_hits++;
#endif

        Lightmap_Save(bsp, lightmaps, lightsurf, cached_lightmap, cached_style);
    }
}

static void LightPoint_Sky(const mbsp_t *bsp, raystream_intersection_t &rs, const sun_t *sun, const qvec3f &surfpoint,
    lightgrid_samples_t &result)
{
    // FIXME: Normalized sun vector should be stored in the sun_t. Also clarify which way the vector points (towards or
    // away..)
    // FIXME: Much of this is copied/pasted from LightFace_Entity, should probably be merged
    qvec3f incoming = qv::normalize(sun->sunvec);

    rs.clearPushedRays();

    // only 1 ray
    {
        qvec3f color = sun->sunlight_color * (sun->sunlight / 255.0f);

        /* Quick distance check first */
        if (fabs(LightSample_Brightness(color)) <= light_options.gate.value()) {
            return;
        }

        qvec3f normalcontrib{}; // unused

        rs.pushRay(0, surfpoint, incoming, MAX_SKY_DIST, &color, &normalcontrib);
    }

    // We need to check if the first hit face is a sky face, so we need
    // to test intersection (not occlusion)
    rs.tracePushedRaysIntersection(nullptr, CHANNEL_MASK_DEFAULT);

    // add result
    const int N = rs.numPushedRays();
    for (int j = 0; j < N; j++) {
        if (rs.getPushedRayHitType(j) != hittype_t::SKY) {
            continue;
        }

        result.add(rs.getPushedRayColor(j), sun->style, incoming, sun->anglescale);
    }
}

// Mottle

static constexpr int mod_round_to_neg_inf(int x, int y)
{
    assert(y > 0);
    if (x >= 0) {
        return x % y;
    }
    // e.g. with mod_round_to_neg_inf(-7, 3) we want +2
    const int temp = (-x) % y;

    // mod_round_to_neg_inf(-X, X) should return 0
    if (temp == 0)
        return 0;

    return y - temp;
}

// some quick checks
static_assert(mod_round_to_neg_inf(-4, 3) == 2);
static_assert(mod_round_to_neg_inf(-3, 3) == 0);
static_assert(mod_round_to_neg_inf(-2, 3) == 1);
static_assert(mod_round_to_neg_inf(-1, 3) == 2);
static_assert(mod_round_to_neg_inf(+0, 3) == 0);
static_assert(mod_round_to_neg_inf(+1, 3) == 1);
static_assert(mod_round_to_neg_inf(+2, 3) == 2);
static_assert(mod_round_to_neg_inf(+3, 3) == 0);
static_assert(mod_round_to_neg_inf(+4, 3) == 1);

constexpr int mottle_texsize = 256;

/**
 * integers 0 through 255 shuffled with Python:
 *
 * import random
 * a = list(range(0, 256))
 * random.shuffle(a)
 */
static constexpr uint8_t MottlePermutation256[] = {11, 255, 250, 82, 217, 9, 144, 93, 136, 153, 55, 71, 73, 204, 96,
    180, 126, 8, 50, 46, 113, 91, 238, 143, 30, 215, 191, 243, 65, 58, 208, 33, 86, 1, 182, 118, 83, 115, 207, 52, 94,
    112, 205, 48, 99, 254, 117, 101, 157, 140, 72, 242, 244, 154, 10, 135, 155, 168, 125, 183, 148, 116, 187, 166, 25,
    156, 177, 231, 165, 57, 221, 105, 28, 211, 127, 41, 142, 253, 146, 87, 122, 229, 162, 137, 194, 174, 167, 15, 220,
    26, 235, 3, 39, 80, 88, 42, 202, 12, 97, 53, 70, 123, 170, 110, 214, 192, 173, 84, 169, 188, 64, 102, 147, 158, 100,
    69, 213, 193, 43, 20, 13, 237, 171, 103, 32, 190, 223, 150, 131, 206, 85, 124, 163, 18, 139, 132, 79, 29, 216, 232,
    178, 74, 24, 141, 201, 181, 152, 4, 7, 159, 134, 212, 226, 245, 164, 239, 47, 66, 27, 40, 197, 81, 78, 219, 228,
    241, 121, 23, 120, 230, 76, 252, 199, 184, 45, 203, 161, 89, 16, 21, 119, 5, 209, 196, 68, 130, 195, 176, 225, 233,
    128, 22, 248, 179, 249, 61, 108, 138, 145, 31, 49, 107, 56, 172, 224, 210, 6, 160, 189, 104, 200, 44, 175, 133, 77,
    62, 106, 92, 186, 227, 14, 38, 247, 37, 17, 222, 36, 75, 129, 185, 251, 240, 54, 151, 2, 98, 149, 0, 63, 218, 60,
    198, 19, 59, 90, 246, 234, 67, 51, 109, 95, 236, 35, 34, 114, 111};

/**
 * Return a noise texture value from 0-47.
 *
 * Vanilla Q2 tools just called (rand() % 48) per-luxel, which generates seams
 * and scales in size with lightmap scale.
 *
 * Replacement code uses "value noise", generating a tiling 3D texture
 * in world space.
 */
static float Mottle(const qvec3f &position)
{
#if 0
    return rand() % 48;
#else
    const float world_to_tex = 1 / 16.0f;

    qvec3f texspace_pos = position * world_to_tex;

    int coord_floor_x = static_cast<int>(floor(texspace_pos[0]));
    int coord_floor_y = static_cast<int>(floor(texspace_pos[1]));
    int coord_floor_z = static_cast<int>(floor(texspace_pos[2]));

    float coord_frac_x = static_cast<float>(texspace_pos[0] - coord_floor_x);
    float coord_frac_y = static_cast<float>(texspace_pos[1] - coord_floor_y);
    float coord_frac_z = static_cast<float>(texspace_pos[2] - coord_floor_z);

    assert(coord_frac_x >= 0 && coord_frac_x <= 1);
    assert(coord_frac_y >= 0 && coord_frac_y <= 1);
    assert(coord_frac_z >= 0 && coord_frac_z <= 1);

    coord_floor_x = mod_round_to_neg_inf(coord_floor_x, mottle_texsize);
    coord_floor_y = mod_round_to_neg_inf(coord_floor_y, mottle_texsize);
    coord_floor_z = mod_round_to_neg_inf(coord_floor_z, mottle_texsize);

    assert(coord_floor_x >= 0 && coord_floor_x < mottle_texsize);
    assert(coord_floor_y >= 0 && coord_floor_y < mottle_texsize);
    assert(coord_floor_z >= 0 && coord_floor_z < mottle_texsize);

    // look up sample in the 3d texture at an integer coordinate
    auto tex = [](int x, int y, int z) -> uint8_t {
        int v;
        v = MottlePermutation256[x % 256];
        v = MottlePermutation256[(v + y) % 256];
        v = MottlePermutation256[(v + z) % 256];
        return v;
    };

    // 3D bilinear interpolation
    float res = mix(mix(mix(tex(coord_floor_x, coord_floor_y, coord_floor_z),
                            tex(coord_floor_x + 1, coord_floor_y, coord_floor_z), coord_frac_x),
                        mix(tex(coord_floor_x, coord_floor_y + 1, coord_floor_z),
                            tex(coord_floor_x + 1, coord_floor_y + 1, coord_floor_z), coord_frac_x),
                        coord_frac_y),
        mix(mix(tex(coord_floor_x, coord_floor_y, coord_floor_z + 1),
                tex(coord_floor_x + 1, coord_floor_y, coord_floor_z + 1), coord_frac_x),
            mix(tex(coord_floor_x, coord_floor_y + 1, coord_floor_z + 1),
                tex(coord_floor_x + 1, coord_floor_y + 1, coord_floor_z + 1), coord_frac_x),
            coord_frac_y),
        coord_frac_z);

    return (res / 255.0f) * 48.0f;
#endif
}

/*
 * ============
 * LightFace_Min
 * ============
 */
static void LightFace_Min(const mbsp_t *bsp, const mface_t *face, const qvec3f &color, float light,
    lightsurf_t *lightsurf, lightmapdict_t *lightmaps, int32_t style)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;

    const surfflags_t &extended_flags = extended_texinfo_flags[face->texinfo];
    if (extended_flags.no_minlight) {
        return; /* this face is excluded from minlight */
    }

    /* Find the lightmap */
    lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, style, lightsurf);

    bool hit = false;
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        const auto &surf_sample = lightsurf->samples[i];
        lightsample_t &sample = lightmap->samples[i];

        float value = light;
        if (cfg.minlight_dirt.value()) {
            value *= Dirt_GetScaleFactor(cfg, surf_sample.occlusion, NULL, 0.0f, lightsurf);
        }
        if (cfg.addminlight.value()) {
            sample.color += color * (value / 255.0f);
            hit = true;
        } else {
            if (lightsurf->minlightMottle) {
                value += Mottle(surf_sample.point);
            }
            hit = Light_ClampMin(sample, value, color) || hit;
        }
    }

    if (hit) {
        Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, style);
    }
}

static void LightFace_LocalMin(
    const mbsp_t *bsp, const mface_t *face, lightsurf_t *lightsurf, lightmapdict_t *lightmaps)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;
    const modelinfo_t *modelinfo = lightsurf->modelinfo;

    const surfflags_t &extended_flags = extended_texinfo_flags[face->texinfo];
    if (extended_flags.no_minlight) {
        return; /* this face is excluded from minlight */
    }

    // FIXME: Refactor this?
    if (lightsurf->modelinfo->lightignore.value() || extended_flags.light_ignore)
        return;

    /* Cast rays for local minlight entities */
    for (const auto &entity : GetLights()) {
        if (entity->getFormula() != LF_LOCALMIN) {
            continue;
        }
        if (entity->nostaticlight.value()) {
            continue;
        }

        if (CullLight(entity.get(), lightsurf)) {
            continue;
        }

        // check lighting channels
        if (!(entity->light_channel_mask.value() & lightsurf->object_channel_mask)) {
            return;
        }

        raystream_occlusion_t &rs = occlusion_stream;
        rs.clearPushedRays();

        lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, entity->style.value(), lightsurf);

        bool hit = false;
        for (int i = 0; i < lightsurf->samples.size(); i++) {
            const auto &surf_sample = lightsurf->samples[i];

            if (surf_sample.occluded)
                continue;

            const lightsample_t &sample = lightmap->samples[i];
            const qvec3f &surfpoint = surf_sample.point;
            if (cfg.addminlight.value() || LightSample_Brightness(sample.color) < entity->light.value()) {
                qvec3f surfpointToLightDir;
                const float surfpointToLightDist = GetDir(surfpoint, entity->origin.value(), surfpointToLightDir);

                rs.pushRay(i, surfpoint, surfpointToLightDir, surfpointToLightDist);
            }
        }

        // local minlight just needs occlusion, not closest hit
        rs.tracePushedRaysOcclusion(modelinfo, entity->shadow_channel_mask.value());
#if 0
        total_light_rays += rs.numPushedRays();
#endif

        const int N = rs.numPushedRays();
        for (int j = 0; j < N; j++) {
            if (rs.getPushedRayOccluded(j)) {
                continue;
            }

            const ray_io &ray = rs.getRay(j);
            int i = ray.index;
            float value = entity->light.value();
            lightsample_t &sample = lightmap->samples[i];

            value *= Dirt_GetScaleFactor(
                cfg, lightsurf->samples[i].occlusion, entity.get(), 0.0 /* TODO: pass distance */, lightsurf);
            if (cfg.addminlight.value()) {
                sample.color += entity->color.value() * (value / 255.0f);
                hit = true;
            } else {
                hit = Light_ClampMin(sample, value, entity->color.value()) || hit;
            }

#if 0
            total_light_ray_hits++;
#endif
        }

        if (hit) {
            Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, entity->style.value());
        }
    }
}

static void LightFace_AutoMin(const mbsp_t *bsp, const mface_t *face, lightsurf_t *lightsurf, lightmapdict_t *lightmaps)
{
    const modelinfo_t *modelinfo = lightsurf->modelinfo;

    if (!modelinfo)
        return;

    /**
     * if true, apply to all luxels, not just occluded ones
     */
    bool apply_to_all = false;

    const bool any_occluded = std::any_of(lightsurf->samples.begin(), lightsurf->samples.end(),
        [](const lightsurf_t::sample_data_t &v) { return v.occluded; });

    if (!modelinfo->autominlight.is_changed()) {
        // default: apply autominlight to occluded luxels only
        if (!any_occluded)
            return;
    } else if (!modelinfo->autominlight.value()) {
        // force off
        return;
    } else {
        // force on
        apply_to_all = true;
    }

    // we are going to apply at least some

    qvec3f center = (modelinfo->model->mins + modelinfo->model->maxs) / 2;
    if (!modelinfo->autominlight_target.value().empty()) {
        for (auto &entity : GetEntdicts()) {
            if (entity.get("targetname") == modelinfo->autominlight_target.value()) {
                qvec3f point{};
                entity.get_vector("origin", point);
                center = point;
                break;
            }
        }
    }

    auto grid_samples = FixPointAndCalcLightgrid(bsp, center);

    if (!grid_samples.occluded) {
        // process each of the captured styles
        for (const auto &grid_sample : grid_samples.samples_by_style) {
            if (!grid_sample.used)
                break;

            lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, grid_sample.style, lightsurf);

            // for each luxel (or only occluded luxels, depending on the setting),
            // apply the minlight
            for (int i = 0; i < lightsurf->samples.size(); i++) {
                if (apply_to_all || lightsurf->samples[i].occluded) {
                    lightmap->samples[i].color = qv::max(grid_sample.undirectional_color, lightmap->samples[i].color);
                }
            }

            Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, grid_sample.style);
        }

        // clear occluded state, since we filled in all occluded samples with a color
        for (int i = 0; i < lightsurf->samples.size(); i++) {
            lightsurf->samples[i].occluded = false;
        }
    }
}

/*
 * =============
 * LightFace_DirtDebug
 * =============
 */
static void LightFace_DirtDebug(const mbsp_t *bsp, const lightsurf_t *lightsurf, lightmapdict_t *lightmaps)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;
    /* use a style 0 light map */
    lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, 0, lightsurf);

    /* Overwrite each point with the dirt value for that sample... */
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        lightsample_t &sample = lightmap->samples[i];
        const float light = 255 * Dirt_GetScaleFactor(cfg, lightsurf->samples[i].occlusion, nullptr, 0.0f, lightsurf);
        sample.color = {light};
    }

    Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, 0);
}

/*
 * =============
 * LightFace_PhongDebug
 * =============
 */
static void LightFace_PhongDebug(const mbsp_t *bsp, const lightsurf_t *lightsurf, lightmapdict_t *lightmaps)
{
    /* use a style 0 light map */
    lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, 0, lightsurf);

    /* Overwrite each point with the normal for that sample... */
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        lightsample_t &sample = lightmap->samples[i];
        // scale from [-1..1] to [0..1], then multiply by 255
        sample.color = lightsurf->samples[i].normal;

        for (auto &v : sample.color) {
            v = std::abs(v) * 255;
        }
    }

    Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, 0);
}

static void LightFace_DebugMottle(const mbsp_t *bsp, const lightsurf_t *lightsurf, lightmapdict_t *lightmaps)
{
    /* use a style 0 light map */
    lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, 0, lightsurf);

    /* Overwrite each point with the mottle noise for that sample... */
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        lightsample_t &sample = lightmap->samples[i];
        // mottle is meant to be applied on top of minlight, so add some here
        // for preview purposes.
        const float minlight = 20.0f;
        sample.color = qvec3f(minlight + Mottle(lightsurf->samples[i].point));
    }

    Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, 0);
}

// dir: vpl -> sample point direction
// mxd. returns color in [0,255]
inline qvec3f GetSurfaceLighting(const settings::worldspawn_keys &cfg, const surfacelight_t &vpl,
    const surfacelight_t::per_style_t &vpl_settings, const qvec3f &dir, float dist, const qvec3f &normal,
    bool use_normal, float standard_scale, float sky_scale, float hotspot_clamp)
{
    float dotProductFactor = 1.0f;

    float dp1 = qv::dot(vpl.surfnormal, dir);
    const qvec3f sp_vpl = dir * -1.0f;
    float dp2 = use_normal ? qv::dot(sp_vpl, normal) : 1.0f;

    if (!vpl_settings.omnidirectional) {
        if (dp1 < -LIGHT_ANGLE_EPSILON)
            return {0}; // sample point behind vpl
        if (dp2 < -LIGHT_ANGLE_EPSILON)
            return {0}; // vpl behind sample face

        // Rescale a bit to brighten the faces nearly-perpendicular to the surface light plane...
        if (vpl_settings.rescale) {
            dp1 = 0.5f + dp1 * 0.5f;
            dp2 = 0.5f + dp2 * 0.5f;
        }

        dotProductFactor = dp1 * dp2;
    } else {
        // used for sky face surface lights
        dotProductFactor = dp2 * 0.5f;
    }

    dotProductFactor = std::max(0.0f, dotProductFactor);

    // quick exit
    if (!dotProductFactor) {
        return {0};
    }

    if (vpl_settings.omnidirectional) {
        dist += cfg.surflightskydist.value();
    }

    // Get light contribution
    return SurfaceLight_ColorAtDist(cfg, (vpl_settings.omnidirectional ? sky_scale : standard_scale) * dotProductFactor,
        vpl_settings.intensity, vpl_settings.color, dist, vpl_settings.atten, hotspot_clamp);
}

static bool // mxd
SurfaceLight_SphereCull(const surfacelight_t *vpl, const lightsurf_t *lightsurf,
    const surfacelight_t::per_style_t &vpl_settings, float bouncelight_gate, float hotspot_clamp)
{
    if (light_options.visapprox.value() == visapprox_t::RAYS &&
        vpl->bounds.disjoint(lightsurf->extents.bounds, 0.001f)) {
        return true;
    } else if (!bouncelight_gate) {
        return false;
    }

    const settings::worldspawn_keys &cfg = *lightsurf->cfg;
    const qvec3f dir = qvec3f(lightsurf->extents.origin) - qvec3f(vpl->pos); // vpl -> sample point
    const float dist = qv::length(dir) + lightsurf->extents.radius;

    // Get light contribution
    const qvec3f color = SurfaceLight_ColorAtDist(cfg,
        vpl_settings.omnidirectional ? cfg.surflightskyscale.value() : cfg.surflightscale.value(),
        vpl_settings.totalintensity, vpl_settings.color, dist, vpl_settings.atten, hotspot_clamp);

    return qv::gate(color, (float)bouncelight_gate);
}

static bool SurfaceLight_VisCull(const mbsp_t *bsp, const std::vector<uint8_t> *pvs, const lightsurf_t *lightsurf_b)
{
    if (pvs && light_options.visapprox.value() == visapprox_t::VIS) {
        for (auto &leaf : lightsurf_b->leaves) {
            if (VisCullEntity(bsp, *pvs, leaf)) {
                return true;
            }
        }
    }

    return false;
}

/*
 * Spatial grid over the emissive surfaces (surface lights and bounce lights), so each face only
 * runs SurfaceLight_SphereCull on emitters that can reach it instead of on every emitter of the
 * map. reach = distance at which SurfaceLight_SphereCull's color drops to the gate (the QRAD3
 * falloff is non-increasing in distance); the cull uses |d| + face radius, so any emitter that
 * passes it is closer than reach to the face center. Candidates are visited in the original
 * order, so the result is unchanged.
 */
struct surflight_grid_t
{
    static constexpr float CELL = 256.0f;
    static constexpr size_t MAX_CELLS_PER_EMITTER = 4096;

    const std::vector<lightsurf_t *> *emitters = nullptr; // the list this grid was built for
    size_t emitter_count = 0;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells;
    std::vector<std::uint32_t> global; // emitters without a finite reach
    std::vector<float> reach;

    // Inverted index for SurfaceLight_VisCull (visapprox VIS): an emitter passes it if the face's
    // pvs has the bit of every one of its leaves that VisCullEntity checks. vis_emitters[bit] =
    // emitters with such a leaf on that pvs bit; vis_needed[e] = how many distinct bits e needs
    // (VIS_NEVER: a leaf can never be visible, so the emitter is always culled).
    static constexpr std::uint32_t VIS_NEVER = 0xffffffffu;
    bool vis_index = false;
    std::vector<std::vector<std::uint32_t>> vis_emitters;
    std::vector<std::uint32_t> vis_needed;
    std::vector<std::uint32_t> vis_always; // emitters no leaf of which is checked

    // emitters passing the vis test, per distinct pvs (faces in the same area share their pvs)
    std::mutex vis_cache_mutex;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint32_t>>> vis_cache;

    static std::int64_t key(int x, int y, int z)
    {
        return (std::int64_t(x & 0x1fffff) << 42) | (std::int64_t(y & 0x1fffff) << 21) | std::int64_t(z & 0x1fffff);
    }
    static int cell(float v) { return static_cast<int>(std::floor(v / CELL)); }
};
static surflight_grid_t surflight_grid;

void BuildSurfaceLightGrid()
{
    const settings::worldspawn_keys &cfg = light_options;
    const std::vector<lightsurf_t *> &emitters = EmissiveLightSurfaces();
    const float surflight_gate = light_options.emissivequality.value() == emissivequality_t::HIGH ? 0.0f : 0.01f;
    // smallest hotspot clamp LightFace_SurfaceLight is called with: brightest near the emitter
    constexpr float MIN_HOTSPOT_CLAMP = 16.0f;

    surflight_grid_t &g = surflight_grid;
    g.emitters = nullptr;
    g.cells.clear();
    g.global.clear();
    g.reach.clear();
    g.vis_index = false;
    g.vis_emitters.clear();
    g.vis_needed.clear();
    g.vis_always.clear();
    g.vis_cache.clear();
    g.emitters = &emitters;
    g.emitter_count = emitters.size();
    g.reach.assign(emitters.size(), std::numeric_limits<float>::infinity());

    for (std::uint32_t idx = 0; idx < emitters.size(); idx++) {
        const surfacelight_t &vpl = *emitters[idx]->vpl;
        float reach = 0.0f;
        if (surflight_gate > 0) {
            for (const auto &style : vpl.styles) {
                auto gated = [&](float d) {
                    return qv::gate(SurfaceLight_ColorAtDist(cfg,
                                        style.omnidirectional ? cfg.surflightskyscale.value()
                                                              : cfg.surflightscale.value(),
                                        style.totalintensity, style.color, d, style.atten, MIN_HOTSPOT_CLAMP),
                        surflight_gate);
                };
                float hi = 1.0f;
                while (!gated(hi) && hi < 1e7f)
                    hi *= 2.0f;
                if (hi >= 1e7f) {
                    reach = std::numeric_limits<float>::infinity();
                    break;
                }
                float lo = 0.0f;
                for (int it = 0; it < 40; it++) {
                    const float mid = 0.5f * (lo + hi);
                    (gated(mid) ? hi : lo) = mid;
                }
                reach = std::max(reach, hi * 1.001f + 1.0f);
            }
        } else {
            reach = std::numeric_limits<float>::infinity(); // no gate: every emitter reaches every face
        }
        g.reach[idx] = reach;

        const qvec3f &o = vpl.pos;
        if (!std::isfinite(reach)) {
            g.global.push_back(idx);
            continue;
        }
        const int x0 = g.cell(o[0] - reach), x1 = g.cell(o[0] + reach);
        const int y0 = g.cell(o[1] - reach), y1 = g.cell(o[1] + reach);
        const int z0 = g.cell(o[2] - reach), z1 = g.cell(o[2] + reach);
        if (size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1) * size_t(z1 - z0 + 1) > g.MAX_CELLS_PER_EMITTER) {
            g.global.push_back(idx);
            continue;
        }
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                for (int z = z0; z <= z1; z++)
                    g.cells[g.key(x, y, z)].push_back(idx);
    }
    // vis index; mirrors SurfaceLight_VisCull / VisCullEntity / Pvs_LeafVisible
    const mbsp_t *bsp = emitters.empty() ? nullptr : emitters[0]->bsp;
    if (bsp && light_options.visapprox.value() == visapprox_t::VIS && !bsp->dvis.bits.empty()) {
        g.vis_index = true;
        g.vis_needed.assign(emitters.size(), 0);
        const bool clusters = bsp->loadversion->game->has_cluster_support;
        std::vector<int> bits;
        for (std::uint32_t idx = 0; idx < emitters.size(); idx++) {
            bits.clear();
            bool never = false;
            for (const mleaf_t *leaf : emitters[idx]->leaves) {
                if (leaf == nullptr)
                    continue;
                auto contents = bsp->loadversion->game->create_contents_from_native(leaf->contents);
                if (contents.is_solid() || contents.is_sky() || contents.is_liquid())
                    continue; // VisCullEntity never culls these
                int bit;
                if (clusters) {
                    bit = leaf->cluster;
                    if (bit < 0 || bit >= bsp->dvis.bit_offsets.size() ||
                        bsp->dvis.get_bit_offset(VIS_PVS, bit) >= bsp->dvis.bits.size())
                        bit = -1;
                } else {
                    const int leafnum = leaf - bsp->dleafs.data();
                    bit = LeafnumToVisleaf(leafnum);
                    if (leafnum == 0 || bit < -1 || bit >= bsp->dmodels[0].visleafs)
                        bit = -1;
                }
                if (bit < 0) {
                    never = true; // Pvs_LeafVisible is false for any pvs
                    break;
                }
                bits.push_back(bit);
            }
            if (never) {
                g.vis_needed[idx] = g.VIS_NEVER;
                continue;
            }
            std::sort(bits.begin(), bits.end());
            bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
            g.vis_needed[idx] = static_cast<std::uint32_t>(bits.size());
            if (bits.empty()) {
                g.vis_always.push_back(idx);
                continue;
            }
            for (int bit : bits) {
                if (bit >= g.vis_emitters.size())
                    g.vis_emitters.resize(bit + 1);
                g.vis_emitters[bit].push_back(idx);
            }
        }
    }
    logging::print("surface light grid: {} emitters, {} unbounded, vis index {}\n", emitters.size(), g.global.size(),
        g.vis_index ? "on" : "off");
}

enum class surflight_candidates_t
{
    all, // the grid is stale: visit every emitter
    near, // emitters within reach
    near_visible // emitters within reach that pass SurfaceLight_VisCull
};

// emitter indices (into EmissiveLightSurfaces()) that may reach the face, in order
static surflight_candidates_t SurfaceLightCandidates(const lightsurf_t &ls, std::vector<std::uint32_t> &out)
{
    const surflight_grid_t &g = surflight_grid;
    const std::vector<lightsurf_t *> &emitters = EmissiveLightSurfaces();
    if (g.emitters != &emitters || g.emitter_count != emitters.size())
        return surflight_candidates_t::all;

    out.clear();
    const qvec3f c = ls.extents.origin;
    const float r = ls.extents.radius;

    if (g.vis_index && !ls.pvs.empty()) {
        // emitters whose checked leaves are all in the face's pvs (exactly SurfaceLight_VisCull),
        // cached per pvs, then the same reach test as below
        const std::string key(reinterpret_cast<const char *>(ls.pvs.data()), ls.pvs.size());
        std::shared_ptr<const std::vector<std::uint32_t>> visible;
        {
            std::lock_guard<std::mutex> lock(surflight_grid.vis_cache_mutex);
            auto it = surflight_grid.vis_cache.find(key);
            if (it != surflight_grid.vis_cache.end())
                visible = it->second;
        }
        if (!visible) {
            thread_local std::vector<std::uint32_t> counts;
            thread_local std::vector<std::uint32_t> touched;
            if (counts.size() < emitters.size())
                counts.assign(emitters.size(), 0);
            touched.clear();
            const size_t nbits = std::min(ls.pvs.size() * 8, g.vis_emitters.size());
            for (size_t byte = 0; byte * 8 < nbits; byte++) {
                const std::uint8_t v = ls.pvs[byte];
                if (!v)
                    continue;
                for (int b = 0; b < 8; b++) {
                    const size_t bit = byte * 8 + b;
                    if (!(v & (1 << b)) || bit >= nbits)
                        continue;
                    for (std::uint32_t idx : g.vis_emitters[bit]) {
                        if (counts[idx]++ == 0)
                            touched.push_back(idx);
                    }
                }
            }
            auto list = std::make_shared<std::vector<std::uint32_t>>();
            for (std::uint32_t idx : touched) {
                if (counts[idx] == g.vis_needed[idx])
                    list->push_back(idx);
                counts[idx] = 0;
            }
            list->insert(list->end(), g.vis_always.begin(), g.vis_always.end());
            std::sort(list->begin(), list->end());
            std::lock_guard<std::mutex> lock(surflight_grid.vis_cache_mutex);
            visible = surflight_grid.vis_cache.try_emplace(key, std::move(list)).first->second;
        }
        for (std::uint32_t idx : *visible) {
            if (qv::distance(emitters[idx]->vpl->pos, c) < g.reach[idx])
                out.push_back(idx);
        }
        return surflight_candidates_t::near_visible;
    }

    for (int x = g.cell(c[0] - r); x <= g.cell(c[0] + r); x++)
        for (int y = g.cell(c[1] - r); y <= g.cell(c[1] + r); y++)
            for (int z = g.cell(c[2] - r); z <= g.cell(c[2] + r); z++) {
                auto it = g.cells.find(g.key(x, y, z));
                if (it == g.cells.end())
                    continue;
                for (std::uint32_t idx : it->second) {
                    if (qv::distance(emitters[idx]->vpl->pos, c) < g.reach[idx])
                        out.push_back(idx);
                }
            }
    out.insert(out.end(), g.global.begin(), g.global.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return surflight_candidates_t::near;
}

// one style of one emissive surface lighting a face (the body of LightFace_SurfaceLight's loop)
// only_samples: if set, only these sample indices are lit (used to re-trace GPU results)
static void LightFace_SurfaceLightEmitter(lightsurf_t *lightsurf, lightmapdict_t *lightmaps,
    const surfacelight_t &vpl, const surfacelight_t::per_style_t &vpl_setting, float standard_scale, float sky_scale,
    float hotspot_clamp, float surflight_gate, const std::vector<int> *only_samples = nullptr)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;
    const mbsp_t *bsp = lightsurf->bsp;
    raystream_occlusion_t &rs = occlusion_stream;

    const int num_samples = only_samples ? only_samples->size() : lightsurf->samples.size();
    for (int c = 0; c < vpl.points.size(); c++) {
        rs.clearPushedRays();

        for (int n = 0; n < num_samples; n++) {
            const int i = only_samples ? (*only_samples)[n] : n;
            const auto &sample = lightsurf->samples[i];

            if (sample.occluded)
                continue;

            const qvec3f &lightsurf_pos = sample.point;
            const qvec3f &lightsurf_normal = sample.normal;

            const qvec3f &pos = vpl.points[c];
            qvec3f dir = lightsurf_pos - pos;
            float dist = std::max(0.01f, qv::length(dir));
            bool use_normal = true;

            if (lightsurf->twosided) {
                use_normal = false;
                dir /= dist;
            } else if (dist == 0.0f) {
                dir = lightsurf_normal;
                use_normal = false;
            } else {
                dir /= dist;
            }

            const qvec3f indirect = GetSurfaceLighting(cfg, vpl, vpl_setting, dir, dist, lightsurf_normal, use_normal,
                standard_scale, sky_scale, hotspot_clamp);
            if (!qv::gate(indirect, surflight_gate)) { // Each point contributes very little to the final result
                rs.pushRay(i, pos, dir, dist, &indirect);
            }
        }

        if (!rs.numPushedRays())
            continue;

#if 0
        total_surflight_rays += rs.numPushedRays();
#endif
        rs.tracePushedRaysOcclusion(lightsurf->modelinfo, CHANNEL_MASK_DEFAULT);

        const int lightmapstyle = vpl_setting.style;
        lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, lightmapstyle, lightsurf);

        bool hit = false;
        const int numrays = rs.numPushedRays();
        for (int j = 0; j < numrays; j++) {
            if (rs.getPushedRayOccluded(j))
                continue;

            const ray_io &ray = rs.getRay(j);
            const int i = ray.index;
            qvec3f indirect = rs.getPushedRayColor(j);

            // Q_assert(!std::isnan(indirect[0]));

            // Use dirt scaling on the surface lighting.
            const float dirtscale =
                Dirt_GetScaleFactor(cfg, lightsurf->samples[i].occlusion, nullptr, 0.0, lightsurf);
            indirect *= dirtscale;

            lightsample_t &sample = lightmap->samples[i];
            sample.color += indirect;
            lightmap->bounce_color += indirect;

            hit = true;
#if 0
            ++total_surflight_ray_hits;
#endif
        }

        // If surface light contributed anything, save.
        if (hit)
            Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, lightmapstyle);
    }
}

static float SurfaceLightGate()
{
    return light_options.emissivequality.value() == emissivequality_t::HIGH ? 0.0f : 0.01f;
}

static void // mxd
LightFace_SurfaceLight(const mbsp_t *bsp, lightsurf_t *lightsurf, lightmapdict_t *lightmaps,
    std::optional<size_t> bounce_depth, float standard_scale, float sky_scale, float hotspot_clamp)
{
    const float surflight_gate = SurfaceLightGate();

    // check lighting channels (currently surface lights are always on CHANNEL_MASK_DEFAULT)
    if (!(lightsurf->object_channel_mask & CHANNEL_MASK_DEFAULT)) {
        return;
    }

    const std::vector<lightsurf_t *> &emitters = EmissiveLightSurfaces();
    thread_local std::vector<std::uint32_t> candidates;
    const surflight_candidates_t kind = SurfaceLightCandidates(*lightsurf, candidates);
    if (kind == surflight_candidates_t::all) {
        candidates.resize(emitters.size());
        for (std::uint32_t i = 0; i < emitters.size(); i++)
            candidates[i] = i;
    }

    for (std::uint32_t candidate : candidates) {
        const auto &surf_ptr = emitters[candidate];
        auto &vpl = *surf_ptr->vpl.get();

        for (const auto &vpl_setting : surf_ptr->vpl->styles) {

            if (vpl_setting.bounce_level != bounce_depth)
                continue;
            else if (SurfaceLight_SphereCull(&vpl, lightsurf, vpl_setting, surflight_gate, hotspot_clamp))
                continue;
            else if (kind != surflight_candidates_t::near_visible &&
                     SurfaceLight_VisCull(bsp, &lightsurf->pvs, surf_ptr))
                continue;

            LightFace_SurfaceLightEmitter(
                lightsurf, lightmaps, vpl, vpl_setting, standard_scale, sky_scale, hotspot_clamp, surflight_gate);
        }
    }
}

static void // mxd
LightPoint_SurfaceLight(const mbsp_t *bsp, const std::vector<uint8_t> *pvs, raystream_occlusion_t &rs, bool bounce,
    float standard_scale, float sky_scale, float hotspot_clamp, const qvec3f &surfpoint, lightgrid_samples_t &result)
{
    const settings::worldspawn_keys &cfg = light_options;
    const float surflight_gate = light_options.emissivequality.value() == emissivequality_t::HIGH ? 0 : 0.01f;

    for (const auto &surf : EmissiveLightSurfaces()) {
        const surfacelight_t &vpl = *surf->vpl;

        if (SurfaceLight_VisCull(bsp, pvs, surf)) {
            continue;
        }

        for (int c = 0; c < vpl.points.size(); c++) {
            // 1 ray
            for (auto &vpl_settings : vpl.styles) {
                if (vpl_settings.bounce_level.has_value() != bounce)
                    continue;

                qvec3f pos = vpl.points[c];
                qvec3f dir = surfpoint - pos;
                float dist = qv::length(dir);

                if (dist == 0.0f)
                    dir = {0, 0, 1};
                else
                    dir /= dist;

                qvec3f indirect{};

                rs.clearPushedRays();

                indirect = GetSurfaceLighting(
                    cfg, vpl, vpl_settings, dir, dist, qvec3f(), false, standard_scale, sky_scale, hotspot_clamp);

                if (!qv::gate(indirect, surflight_gate)) { // Each point contributes very little to the final result
                    rs.pushRay(0, pos, dir, dist, &indirect);
                }

                if (!rs.numPushedRays())
                    continue;

                rs.tracePushedRaysOcclusion(nullptr, CHANNEL_MASK_DEFAULT);

                const int numrays = rs.numPushedRays();
                for (int j = 0; j < numrays; j++) {
                    if (rs.getPushedRayOccluded(j))
                        continue;

                    qvec3f indirect = rs.getPushedRayColor(j);

                    // Q_assert(!std::isnan(indirect[0]));

                    result.add(indirect, vpl_settings.style, -dir, 1.0f);
                }
            }
        }
    }
}

static void LightFace_OccludedDebug(const mbsp_t *bsp, lightsurf_t *lightsurf, lightmapdict_t *lightmaps)
{
    Q_assert(light_options.debugmode == debugmodes::debugoccluded);

    /* use a style 0 light map */
    lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, 0, lightsurf);

    /* Overwrite each point, red=occluded, green=ok */
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        auto &surf_sample = lightsurf->samples[i];
        lightsample_t &sample = lightmap->samples[i];
        if (surf_sample.occluded) {
            sample.color = {255, 0, 0};
        } else {
            sample.color = {0, 255, 0};
        }
        // N.B.: Mark it as un-occluded now, to disable special handling later in the -extra/-extra4 downscaling code
        surf_sample.occluded = false;
    }

    Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, 0);
}

static void LightFace_DebugNeighbours(const mbsp_t *bsp, lightsurf_t *lightsurf, lightmapdict_t *lightmaps)
{
    Q_assert(light_options.debugmode == debugmodes::debugneighbours);

    /* use a style 0 light map */
    lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, 0, lightsurf);

    //    std::vector<neighbour_t> neighbours = NeighbouringFaces_new(lightsurf->bsp, BSP_GetFace(lightsurf->bsp,
    //    dump_facenum)); bool found = false; for (auto &f : neighbours) {
    //        if (f.face == lightsurf->face)
    //            found = true;
    //    }

    bool has_sample_on_dumpface = false;
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        if (lightsurf->samples[i].realfacenum == dump_facenum) {
            has_sample_on_dumpface = true;
            break;
        }
    }

    /* Overwrite each point */
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        lightsample_t &sample = lightmap->samples[i];
        const int sample_face = lightsurf->samples[i].realfacenum;

        if (sample_face == dump_facenum) {
            /* Red - the sample is on the selected face */
            sample.color = {255, 0, 0};
        } else if (has_sample_on_dumpface) {
            /* Green - the face has some samples on the selected face */
            sample.color = {0, 255, 0};
        } else {
            sample.color = {};
        }
        // N.B.: Mark it as un-occluded now, to disable special handling later in the -extra/-extra4 downscaling code
        lightsurf->samples[i].occluded = false;
    }

    Lightmap_Save(bsp, lightmaps, lightsurf, lightmap, 0);
}

/* Dirtmapping borrowed from q3map2, originally by RaP7oR */

constexpr size_t DIRT_NUM_ANGLE_STEPS = 16;
constexpr size_t DIRT_NUM_ELEVATION_STEPS = 3;
constexpr size_t DIRT_NUM_VECTORS = (DIRT_NUM_ANGLE_STEPS * DIRT_NUM_ELEVATION_STEPS);

static qvec3f dirtVectors[DIRT_NUM_VECTORS];
int numDirtVectors = 0;

/*
 * ============
 * SetupDirt
 *
 * sets up dirtmap (ambient occlusion)
 * ============
 */
void SetupDirt(settings::worldspawn_keys &cfg)
{
    // check if needed

    if (!cfg.dirt.value() && cfg.dirt.get_source() == settings::source::COMMANDLINE) {
        // HACK: "-dirt 0" disables all dirtmapping even if we would otherwise use it.
        dirt_in_use = false;
        return;
    }

    if (cfg.dirt.value() || cfg.minlight_dirt.value() || cfg.sunlight_dirt.boolValue() ||
        cfg.sunlight2_dirt.boolValue()) {
        dirt_in_use = true;
    }

    if (!dirt_in_use) {
        // check entities, maybe only a few lights use it
        for (const auto &light : GetLights()) {
            if (light->dirt.boolValue()) {
                dirt_in_use = true;
                break;
            }
        }
    }

    if (!dirt_in_use) {
        // dirtmapping is not used by this map.
        return;
    }

    /* note it */
    logging::funcheader();

    /* calculate angular steps */
    constexpr float angleStep = (float)DEG2RAD(360.0f / DIRT_NUM_ANGLE_STEPS);
    const float elevationStep = (float)DEG2RAD(cfg.dirtangle.value() / DIRT_NUM_ELEVATION_STEPS);

    /* iterate angle */
    float angle = 0.0f;
    numDirtVectors = 0;
    for (int i = 0; i < DIRT_NUM_ANGLE_STEPS; i++, angle += angleStep) {
        /* iterate elevation */
        float elevation = elevationStep * 0.5f;
        for (int j = 0; j < DIRT_NUM_ELEVATION_STEPS; j++, elevation += elevationStep) {
            dirtVectors[numDirtVectors][0] = sin(elevation) * cos(angle);
            dirtVectors[numDirtVectors][1] = sin(elevation) * sin(angle);
            dirtVectors[numDirtVectors][2] = cos(elevation);
            numDirtVectors++;
        }
    }

    /* emit some statistics */
    logging::print("{:9} dirtmap vectors\n", numDirtVectors);
}

// from q3map2
inline qvec3f TransformToTangentSpace(
    const qvec3f &normal, const qvec3f &myUp, const qvec3f &myRt, const qvec3f &inputvec)
{
    return myRt * inputvec[0] + myUp * inputvec[1] + normal * inputvec[2];
}

// from q3map2
inline qvec3f GetDirtVector(const settings::worldspawn_keys &cfg, int i)
{
    Q_assert(i < numDirtVectors);

    if (cfg.dirtmode.value() == 1) {
        /* get random vector */
        float angle = Random() * DEG2RAD(360.0f);
        float elevation = Random() * DEG2RAD(cfg.dirtangle.value());
        return {cos(angle) * sin(elevation), sin(angle) * sin(elevation), cos(elevation)};
    }

    return dirtVectors[i];
}

/*
 * ============
 * LightFace_CalculateDirt
 * ============
 */
static void LightFace_CalculateDirt(lightsurf_t *lightsurf)
{
    const settings::worldspawn_keys &cfg = *lightsurf->cfg;

    Q_assert(dirt_in_use);

    // batch implementation:

    thread_local static std::vector<qvec3f> myUps, myRts;

    myUps.resize(lightsurf->samples.size());
    myRts.resize(lightsurf->samples.size());

    // init
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        lightsurf->samples[i].occlusion = 0;
    }

    // this stuff is just per-point
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        const auto [tangent, bitangent] = qv::MakeTangentAndBitangentUnnormalized(lightsurf->samples[i].normal);

        myUps[i] = qv::normalize(tangent);
        myRts[i] = qv::normalize(bitangent);
    }

    for (int j = 0; j < numDirtVectors; j++) {
        raystream_intersection_t &rs = intersection_stream;
        rs.clearPushedRays();

        // fill in input buffers

        for (int i = 0; i < lightsurf->samples.size(); i++) {
            const auto &sample = lightsurf->samples[i];

            if (sample.occluded)
                continue;

            qvec3f dirtvec = GetDirtVector(cfg, j);
            qvec3f dir = TransformToTangentSpace(sample.normal, myUps[i], myRts[i], dirtvec);

            rs.pushRay(i, sample.point, dir, cfg.dirtdepth.value());
        }

        // trace the batch. need closest hit for dirt, so intersection.
        //
        // use the model's own channel mask as the shadow mask, e.g. so a model in channel 2's AO rays will only hit
        // other things in channel 2
        rs.tracePushedRaysIntersection(lightsurf->modelinfo, lightsurf->object_channel_mask);

        // accumulate hitdists
        for (int k = 0; k < rs.numPushedRays(); k++) {
            const ray_io &ray = rs.getRay(k);
            const int i = ray.index;
            if (rs.getPushedRayHitType(k) == hittype_t::SOLID) {
                const float dist = rs.getPushedRayHitDist(k);
                lightsurf->samples[i].occlusion += std::min(cfg.dirtdepth.value(), dist);
            } else {
                lightsurf->samples[i].occlusion += cfg.dirtdepth.value();
            }
        }
    }

    // process the results.
    for (int i = 0; i < lightsurf->samples.size(); i++) {
        float avgHitdist = lightsurf->samples[i].occlusion / (float)numDirtVectors;
        lightsurf->samples[i].occlusion = 1.0f - (avgHitdist / cfg.dirtdepth.value());
    }
}

/**
 * Same as above LightFace_ScaleAndClamp, but for lightgrid
 * TODO: share code with above
 */
static void LightPoint_ScaleAndClamp(qvec3f &color)
{
    const settings::worldspawn_keys &cfg = light_options;

    /* Fix any negative values */
    color = qv::max(color, {0});

    // before any other scaling, apply maxlight
    if (cfg.maxlight.value()) {
        float maxcolor = qv::max(color);
        // FIXME: for colored lighting, this doesn't seem to generate the right values...
        float maxval = cfg.maxlight.value() * 2.0f;

        if (maxcolor > maxval) {
            color *= (maxval / maxcolor);
        }
    }

    /* Scale and handle gamma adjustment */
    color *= cfg.rangescale.value();

    if (cfg.lightmapgamma.value() != 1.0f) {
        for (auto &c : color) {
            c = pow(c / 255.0f, 1.0f / cfg.lightmapgamma.value()) * 255.0f;
        }
    }
}

static void LightPoint_ScaleAndClamp(lightgrid_samples_t &result)
{
    for (auto &sample : result.samples_by_style) {
        if (sample.used) {
            for (qvec3f &color : sample.colors) {
                LightPoint_ScaleAndClamp(color);
            }
            LightPoint_ScaleAndClamp(sample.undirectional_color);
        }
    }
}

#if 0
static void WritePPM(const fs::path &fname, int width, int height, const uint8_t *rgbdata)
{
    qfile_t file = SafeOpenWrite(fname);

    // see: http://netpbm.sourceforge.net/doc/ppm.html
    ewt::print(file.get(), "P6 {} {} 255 ", width, height);
    int bytes = width * height * 3;
    Q_assert(bytes == SafeWrite(file, rgbdata, bytes));
}

static void DumpFullSizeLightmap(const mbsp_t *bsp, const lightsurf_t *lightsurf)
{
    const lightmap_t *lm = Lightmap_ForStyle_ReadOnly(lightsurf, 0);
    if (lm != nullptr) {
        int fnum = Face_GetNum(bsp, lightsurf->face);

        std::vector<uint8_t> rgbdata;
        for (int i = 0; i < lightsurf->numpoints; i++) {
            const qvec3f &color = lm->samples[i].color;
            for (int j = 0; j < 3; j++) {
                int intval = static_cast<int>(clamp(color[j], 0.0, 255.0));
                rgbdata.push_back(static_cast<uint8_t>(intval));
            }
        }

        const int oversampled_width = (lightsurf->texsize[0] + 1) * settings::extra.value();
        const int oversampled_height = (lightsurf->texsize[1] + 1) * settings::extra.value();

        Q_assert(lightsurf->numpoints == (oversampled_height * oversampled_width));

        WritePPM(fmt::format("face{:04}.ppm", fnum), oversampled_width, oversampled_height, rgbdata.data());
    }
}

static void DumpGLMVector(std::string fname, std::vector<qvec3f> vec, int width, int height)
{
    std::vector<uint8_t> rgbdata;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const qvec3f &sample = vec.at((y * width) + x);
            for (int j = 0; j < 3; j++) {
                int intval = static_cast<int>(clamp(sample[j], 0.0f, 255.0f));
                rgbdata.push_back(static_cast<uint8_t>(intval));
            }
        }
    }
    Q_assert(rgbdata.size() == (width * height * 3));
    WritePPM(fname, width, height, rgbdata.data());
}

static void DumpDownscaledLightmap(const mbsp_t *bsp, const mface_t *face, int w, int h, const qvec3f *colors)
{
    int fnum = Face_GetNum(bsp, face);

    std::vector<uint8_t> rgbdata;
    for (int i = 0; i < (w * h); i++) {
        for (int j = 0; j < 3; j++) {
            int intval = static_cast<int>(clamp(colors[i][j], 0.0, 255.0));
            rgbdata.push_back(static_cast<uint8_t>(intval));
        }
    }

    WritePPM(fmt::format("face-small{:04}.ppm", fnum), w, h, rgbdata.data());
}
#endif

// check if the given face can actually store luxel data
bool Face_IsLightmapped(const mbsp_t *bsp, const mface_t *face)
{
    const mtexinfo_t *texinfo = Face_Texinfo(bsp, face);

    if (texinfo == nullptr) {
        return false;
    }

    const char *texname = Face_TextureName(bsp, face);

    return bsp->loadversion->game->surf_is_lightmapped(texinfo->flags, texname,
        light_options.q2rtx.value() &&
            bsp->loadversion->game->id == GAME_QUAKE_II, // FIXME: move to own config option. -light_nodraw?
        light_options.lightgrid.value());
}

// check if the given face can actually emit light
bool Face_IsEmissive(const mbsp_t *bsp, const mface_t *face)
{
    const mtexinfo_t *texinfo = Face_Texinfo(bsp, face);

    if (texinfo == nullptr) {
        return false;
    }

    const char *texname = Face_TextureName(bsp, face);

    return bsp->loadversion->game->surf_is_emissive(texinfo->flags, texname);
}

lightsurf_t CreateLightmapSurface(const mbsp_t *bsp, const mface_t *face, const facesup_t *facesup,
    const bspx_decoupled_lm_perface *facesup_decoupled, const settings::worldspawn_keys &cfg)
{
    /* Find the correct model offset */
    const modelinfo_t *modelinfo = ModelInfoForFace(bsp, Face_GetNum(bsp, face));
    if (modelinfo == nullptr) {
        return {};
    }

    /* don't bother with degenerate faces */
    if (face->numedges < 3)
        return {};

    // if we don't have a lightmap or emissive, we're done
    if (!Face_IsEmissive(bsp, face) && !Face_IsLightmapped(bsp, face)) {
        return {};
    }

    return Lightsurf_Init(modelinfo, cfg, face, bsp, facesup, facesup_decoupled);
}

/*
 * ============================================================================
 * GPU DIRECT LIGHTING (default; -cpu disables it)
 *
 * Runs before "Direct Lighting". For every lightmapped face it evaluates, on the GPU, the
 * positive lights that GPU_HandlesLight / GPU_HandlesSun accept, with the same culls and math
 * as LightFace_Entity / LightFace_Sky, and adds them to the face's lightmaps. Samples whose
 * rays meet filtered geometry are re-traced here with LightFace_Entity / LightFace_Sky.
 * DirectLightFace then skips those lights on faces marked gpu_direct_done.
 * ============================================================================
 */

// lights the GPU evaluates (everything else stays on the CPU)
static bool GPU_HandlesLight(const light_t &entity)
{
    return entity.light.value() > 0 && !entity.nostaticlight.value() && entity.getFormula() != LF_LOCALMIN &&
           !entity.projectedmip && entity.light_channel_mask.value() == CHANNEL_MASK_DEFAULT &&
           entity.shadow_channel_mask.value() == CHANNEL_MASK_DEFAULT;
}

static bool GPU_HandlesSun(const sun_t &sun)
{
    return sun.sunlight > 0;
}

// positive lights DirectLightFace still does on faces the GPU pass handled, in GetLights() order
static std::vector<const light_t *> gpu_cpu_lights;
static std::vector<const sun_t *> gpu_cpu_suns;

#if defined(HAVE_GPU_LIGHT)

// faces using more light styles than this stay entirely on the CPU
constexpr size_t GPU_MAX_STYLE_SLOTS = 8;
// samples per GPU batch
constexpr size_t GPU_BATCH_SAMPLES = 1u << 20;

struct gpu_light_ref_t
{
    const light_t *entity = nullptr; // either entity or sun
    const sun_t *sun = nullptr;
    int style = 0;
};

static gpu_light::light_t GPU_MakeLight(const light_t &e, const settings::worldspawn_keys &cfg)
{
    gpu_light::light_t L{};
    const qvec3f origin = e.origin.value();
    const qvec3f color = e.color.value();
    for (int i = 0; i < 3; i++) {
        L.pos[i] = origin[i];
        L.dir[i] = e.spotvec[i];
        L.color[i] = color[i];
    }
    L.type = gpu_light::LIGHT_POINT;
    L.light = e.light.value();
    L.gate = light_options.gate.value();
    L.scaled_atten = cfg.scaledist.value() * e.atten.value();
    L.falloff = e.falloff.value();
    L.anglescale = e.anglescale.value();
    L.formula = e.formula.value();
    if (e.spotlight) {
        L.flags |= gpu_light::LIGHT_FLAG_SPOT;
        L.spotfalloff = e.spotfalloff;
        L.spotfalloff2 = e.spotfalloff2;
    }
    if (e.bleed.value())
        L.flags |= gpu_light::LIGHT_FLAG_BLEED;

    // Dirt_GetScaleFactor with an entity
    bool usedirt;
    if (e.dirt.value() == -1)
        usedirt = false;
    else if (e.dirt.value() == 1)
        usedirt = true;
    else
        usedirt = cfg.dirt.value();
    if (dirt_in_use && usedirt) {
        L.flags |= gpu_light::LIGHT_FLAG_DIRT;
        L.dirtgain = e.dirtgain.value() ? e.dirtgain.value() : cfg.dirtgain.value();
        L.dirtscale = e.dirtscale.value() ? e.dirtscale.value() : cfg.dirtscale.value();
        if (e.dirt_on_radius.is_changed() && e.dirt_off_radius.is_changed()) {
            L.flags |= gpu_light::LIGHT_FLAG_DIRT_RADIUS;
            L.dirt_on_radius = e.dirt_on_radius.value();
            L.dirt_off_radius = e.dirt_off_radius.value();
        }
    }
    return L;
}

static gpu_light::light_t GPU_MakeSun(const sun_t &sun, const settings::worldspawn_keys &cfg)
{
    gpu_light::light_t L{};
    const qvec3f incoming = qv::normalize(sun.sunvec);
    for (int i = 0; i < 3; i++) {
        L.dir[i] = incoming[i];
        L.color[i] = sun.sunlight_color[i];
    }
    L.type = gpu_light::LIGHT_SUN;
    L.light = sun.sunlight;
    L.maxdist = MAX_SKY_DIST;
    L.gate = light_options.gate.value();
    L.anglescale = sun.anglescale;
    // Dirt_GetScaleFactor without an entity: dirt always wanted, global gain/scale
    if (dirt_in_use && sun.dirt) {
        L.flags |= gpu_light::LIGHT_FLAG_DIRT;
        L.dirtgain = cfg.dirtgain.value();
        L.dirtscale = cfg.dirtscale.value();
    }
    if (sun.suntexture_value) {
        L.flags |= gpu_light::LIGHT_FLAG_SUNTEXTURE;
        L.suntexture = gpu_light::sky_texture_id(sun.suntexture_value);
    }
    return L;
}

// the culls at the top of LightFace_Entity
static bool GPU_EntityReachesFace(const mbsp_t *bsp, const light_t &entity, const lightsurf_t &lightsurf)
{
    if (light_options.visapprox.value() == visapprox_t::VIS && VisCullEntity(bsp, lightsurf.pvs, entity.leaf))
        return false;
    const float planedist = lightsurf.plane.distance_to(entity.origin.value());
    if (planedist < 0 && !entity.bleed.value() && !lightsurf.curved && !lightsurf.twosided)
        return false;
    if (CullLight(&entity, &lightsurf))
        return false;
    return (entity.light_channel_mask.value() & lightsurf.object_channel_mask) != 0;
}

// the culls at the top of LightFace_Sky
static bool GPU_SunReachesFace(const sun_t &sun, const lightsurf_t &lightsurf)
{
    const qvec3f incoming = qv::normalize(sun.sunvec);
    const float dp = qv::dot(incoming, lightsurf.plane.normal);
    if (dp < -LIGHT_ANGLE_EPSILON && !lightsurf.curved && !lightsurf.twosided)
        return false;
    return (lightsurf.object_channel_mask & CHANNEL_MASK_DEFAULT) != 0;
}

// ERICW_GPU_VALIDATE=1: after the GPU pass, re-light each face's GPU lights on the CPU and report
// samples that differ, with the lights that contribute to them
static std::atomic<size_t> gpu_validate_samples = 0, gpu_validate_mismatches = 0, gpu_validate_dirt_mismatches = 0,
                           gpu_validate_dirt_big_mismatches = 0;

static void GPU_ValidateFace(const mbsp_t *bsp, lightsurf_t &ls, const std::vector<std::uint32_t> &face_entries,
    const std::vector<int> &styles, const std::vector<gpu_light_ref_t> &refs)
{
    constexpr float TOLERANCE = 2.0f;
    constexpr size_t MAX_REPORTS = 40;

    auto light_face = [&](const gpu_light_ref_t &ref, lightmapdict_t *dict, const std::vector<int> *only) {
        if (ref.entity)
            LightFace_Entity(bsp, ref.entity, &ls, dict, only);
        else
            LightFace_Sky(bsp, ref.sun, &ls, dict, only);
    };
    auto sample_color = [](lightmapdict_t &dict, int style, size_t i) -> qvec3f {
        for (auto &lm : dict)
            if (lm.style == style)
                return lm.samples[i].color;
        return {};
    };

    // dirt: recompute on the CPU, compare, keep the GPU values
    if (dirt_in_use) {
        std::vector<float> gpu_occlusion(ls.samples.size());
        for (size_t i = 0; i < ls.samples.size(); i++)
            gpu_occlusion[i] = ls.samples[i].occlusion;
        LightFace_CalculateDirt(&ls);
        for (size_t i = 0; i < ls.samples.size(); i++) {
            const float cpu_occ = ls.samples[i].occlusion;
            const float d = std::fabs(cpu_occ - gpu_occlusion[i]);
            if (d > 0.01f) {
                gpu_validate_dirt_mismatches++;
                if (d > 0.1f && gpu_validate_dirt_big_mismatches++ < 20) {
                    logging::print("GPU validate dirt: face {} sample {} at ({}) normal ({}) occluded {}: gpu {} cpu {}\n",
                        Face_GetNum(bsp, ls.face), i, ls.samples[i].point, ls.samples[i].normal,
                        ls.samples[i].occluded, gpu_occlusion[i], cpu_occ);
                }
            }
            ls.samples[i].occlusion = gpu_occlusion[i];
        }
    }

    lightmapdict_t cpu;
    for (std::uint32_t e : face_entries)
        light_face(refs[e & gpu_light::ENTRY_LIGHT_MASK], &cpu, nullptr);

    gpu_validate_samples += ls.samples.size();
    for (int style : styles) {
        for (size_t i = 0; i < ls.samples.size(); i++) {
            const qvec3f g = sample_color(ls.lightmapsByStyle, style, i);
            const qvec3f c = sample_color(cpu, style, i);
            if (qv::length(g - c) <= TOLERANCE)
                continue;
            if (gpu_validate_mismatches++ >= MAX_REPORTS)
                continue;
            const auto &s = ls.samples[i];
            std::string msg = fmt::format("GPU validate: face {} sample {} style {} at ({}) normal ({}): gpu ({}) cpu ({})\n",
                Face_GetNum(bsp, ls.face), i, style, s.point, s.normal, g, c);
            const std::vector<int> only{static_cast<int>(i)};
            for (std::uint32_t e : face_entries) {
                const gpu_light_ref_t &ref = refs[e & gpu_light::ENTRY_LIGHT_MASK];
                if (ref.entity && ref.style == style) {
                    // unshadowed contribution: if it matches the difference, this light's visibility differs
                    qvec3f color, dir, normalcontrib;
                    float dist;
                    GetLightContrib(*ls.cfg, ref.entity, s.normal, true, s.point, ls.twosided, color, dir,
                        normalcontrib, &dist);
                    color *= Dirt_GetScaleFactor(*ls.cfg, s.occlusion, ref.entity, dist, &ls);
                    if (qv::length(color - (g - c)) < 0.01f * qv::length(g - c) + 0.01f ||
                        qv::length(color + (g - c)) < 0.01f * qv::length(g - c) + 0.01f) {
                        msg += fmt::format("    visibility differs for light at ({}) dist {:.3f} ({})\n",
                            ref.entity->origin.value(), dist, color);
                    }
                }
                lightmapdict_t one;
                light_face(ref, &one, &only);
                const qvec3f contrib = sample_color(one, style, i);
                if (qv::length(contrib) <= 0)
                    continue;
                if (ref.entity)
                    msg += fmt::format("    cpu light at ({}) style {} dist {:.3f}: ({})\n", ref.entity->origin.value(),
                        ref.style, qv::distance(ref.entity->origin.value(), s.point), contrib);
                else
                    msg += fmt::format("    cpu sun dir ({}) style {}: ({})\n", ref.sun->sunvec, ref.style, contrib);
            }
            logging::print("{}", msg);
        }
    }
}

void GPU_DirectLighting(const mbsp_t *bsp, std::span<lightsurf_t> surfaces)
{
    static const bool validate = std::getenv("ERICW_GPU_VALIDATE") != nullptr;
    const auto t_start = std::chrono::steady_clock::now();
    const settings::worldspawn_keys &cfg = light_options;

    // lights handled by the GPU, in the order DirectLightFace visits them
    std::vector<gpu_light::light_t> gpu_lights;
    std::vector<gpu_light_ref_t> refs;
    for (const auto &entity : GetLights()) {
        if (GPU_HandlesLight(*entity)) {
            gpu_lights.push_back(GPU_MakeLight(*entity, cfg));
            refs.push_back({entity.get(), nullptr, entity->style.value()});
        }
    }
    for (const sun_t &sun : GetSuns()) {
        if (GPU_HandlesSun(sun)) {
            gpu_lights.push_back(GPU_MakeSun(sun, cfg));
            refs.push_back({nullptr, &sun, sun.style});
        }
    }
    gpu_cpu_lights.clear();
    gpu_cpu_suns.clear();
    for (const auto &entity : GetLights()) {
        if (entity->light.value() > 0 && !entity->nostaticlight.value() && entity->getFormula() != LF_LOCALMIN &&
            !GPU_HandlesLight(*entity))
            gpu_cpu_lights.push_back(entity.get());
    }
    for (const sun_t &sun : GetSuns()) {
        if (sun.sunlight > 0 && !GPU_HandlesSun(sun))
            gpu_cpu_suns.push_back(&sun);
    }
    logging::print("{} lights on the GPU, {} positive lights left on the CPU\n", gpu_lights.size(),
        gpu_cpu_lights.size() + gpu_cpu_suns.size());
    if (gpu_lights.size() > gpu_light::ENTRY_LIGHT_MASK)
        FError("too many lights for the GPU path");

    // Spatial grid over point lights, so each face only runs the exact culls on nearby lights.
    // reach = distance beyond which GetLightValue <= gate (the formulas are non-increasing in
    // distance), so CullLight would cull any face farther than reach + face radius.
    constexpr float GRID_CELL = 256.0f;
    constexpr size_t GRID_MAX_CELLS_PER_LIGHT = 4096;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> light_grid;
    std::vector<std::uint32_t> global_lights; // suns, unbounded lights, huge lights
    std::vector<float> light_reach(refs.size(), std::numeric_limits<float>::infinity());
    auto grid_key = [](int x, int y, int z) -> std::int64_t {
        return (std::int64_t(x & 0x1fffff) << 42) | (std::int64_t(y & 0x1fffff) << 21) | std::int64_t(z & 0x1fffff);
    };
    for (std::uint32_t li = 0; li < refs.size(); li++) {
        const light_t *e = refs[li].entity;
        if (!e) {
            global_lights.push_back(li);
            continue;
        }
        const float gate = light_options.gate.value();
        auto value = [&](float d) { return std::fabs(GetLightValue(cfg, e, d)); };
        if (e->getFormula() != LF_INFINITE && e->getFormula() != LF_LOCALMIN) {
            float hi = 1.0f;
            while (value(hi) > gate && hi < 1e7f)
                hi *= 2.0f;
            if (hi < 1e7f) {
                float lo = 0.0f;
                for (int it = 0; it < 40; it++) {
                    const float mid = 0.5f * (lo + hi);
                    (value(mid) > gate ? lo : hi) = mid;
                }
                light_reach[li] = hi * 1.001f + 1.0f;
            }
        }
        const float r = light_reach[li];
        const qvec3f o = e->origin.value();
        if (!std::isfinite(r)) {
            global_lights.push_back(li);
            continue;
        }
        const qvec3i lo{static_cast<int>(std::floor((o[0] - r) / GRID_CELL)),
            static_cast<int>(std::floor((o[1] - r) / GRID_CELL)), static_cast<int>(std::floor((o[2] - r) / GRID_CELL))};
        const qvec3i hi{static_cast<int>(std::floor((o[0] + r) / GRID_CELL)),
            static_cast<int>(std::floor((o[1] + r) / GRID_CELL)), static_cast<int>(std::floor((o[2] + r) / GRID_CELL))};
        const size_t cells = size_t(hi[0] - lo[0] + 1) * size_t(hi[1] - lo[1] + 1) * size_t(hi[2] - lo[2] + 1);
        if (cells > GRID_MAX_CELLS_PER_LIGHT) {
            global_lights.push_back(li);
            continue;
        }
        for (int x = lo[0]; x <= hi[0]; x++)
            for (int y = lo[1]; y <= hi[1]; y++)
                for (int z = lo[2]; z <= hi[2]; z++)
                    light_grid[grid_key(x, y, z)].push_back(li);
    }
    // candidate lights for a face, in refs order (keeps the per-sample sums deterministic)
    auto candidate_lights = [&](const lightsurf_t &ls, std::vector<std::uint32_t> &out) {
        out.clear();
        const qvec3f c = ls.extents.origin;
        const float r = ls.extents.radius;
        const qvec3i lo{static_cast<int>(std::floor((c[0] - r) / GRID_CELL)),
            static_cast<int>(std::floor((c[1] - r) / GRID_CELL)), static_cast<int>(std::floor((c[2] - r) / GRID_CELL))};
        const qvec3i hi{static_cast<int>(std::floor((c[0] + r) / GRID_CELL)),
            static_cast<int>(std::floor((c[1] + r) / GRID_CELL)), static_cast<int>(std::floor((c[2] + r) / GRID_CELL))};
        for (int x = lo[0]; x <= hi[0]; x++)
            for (int y = lo[1]; y <= hi[1]; y++)
                for (int z = lo[2]; z <= hi[2]; z++) {
                    auto it = light_grid.find(grid_key(x, y, z));
                    if (it == light_grid.end())
                        continue;
                    for (std::uint32_t li : it->second) {
                        // sphere test: the light can only pass CullLight within reach + face radius
                        const float d = qv::distance(refs[li].entity->origin.value(), c);
                        if (d < light_reach[li] + r)
                            out.push_back(li);
                    }
                }
        out.insert(out.end(), global_lights.begin(), global_lights.end());
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    };

    // faces the GPU pass processes (same conditions as the positive lights in DirectLightFace)
    std::vector<size_t> face_indices;
    for (size_t i = 0; i < surfaces.size(); i++) {
        const lightsurf_t &ls = surfaces[i];
        const mface_t *face = &bsp->dfaces[i];
        if (!Face_IsLightmapped(bsp, face) || ls.samples.empty() || ls.cfg != &cfg)
            continue;
        const modelinfo_t *modelinfo = ModelInfoForFace(bsp, i);
        if (modelinfo->lightignore.value() || extended_texinfo_flags[face->texinfo].light_ignore)
            continue;
        face_indices.push_back(i);
    }

    struct face_work_t
    {
        std::vector<std::uint32_t> entries; // light index | slot << ENTRY_SLOT_SHIFT
        std::vector<int> styles; // slot -> style
        std::vector<const light_t *> overflow_lights; // left to DirectLightFace (no free style slot)
        std::vector<const sun_t *> overflow_suns;
        size_t first_sample = 0;
    };

    size_t total_samples = 0, total_entries = 0, total_retraces = 0, total_overflow = 0;
    double gpu_ms = 0, prep_ms = 0, apply_ms = 0, dirt_total_ms = 0;
    std::atomic<std::int64_t> dirt_ns = 0; // CPU dirt, summed over threads
    std::atomic<size_t> dirt_cpu_faces = 0;
    double dirt_gpu_ms = 0;

    // GPU dirt reproduces dirtmode 0 (fixed vectors); dirtmode 1 uses random vectors
    const bool gpu_dirt = dirt_in_use && cfg.dirtmode.value() != 1;
    std::vector<std::array<float, 4>> dirt_vectors4(std::max(numDirtVectors, 1));
    for (int i = 0; i < numDirtVectors; i++)
        dirt_vectors4[i] = {dirtVectors[i][0], dirtVectors[i][1], dirtVectors[i][2], 0.0f};

    std::vector<gpu_light::sample_t> samples;
    std::vector<gpu_light::face_t> faces;
    std::vector<std::uint32_t> entries;

    for (size_t batch_start = 0; batch_start < face_indices.size();) {
        // pick faces up to GPU_BATCH_SAMPLES samples
        size_t batch_end = batch_start, batch_samples = 0;
        while (batch_end < face_indices.size() &&
               (batch_end == batch_start || batch_samples + surfaces[face_indices[batch_end]].samples.size() <=
                                                GPU_BATCH_SAMPLES)) {
            batch_samples += surfaces[face_indices[batch_end]].samples.size();
            batch_end++;
        }
        const size_t n = batch_end - batch_start;

        // prep: culls, style slots, CPU dirt for faces the GPU dirt can't do (parallel)
        auto t0 = std::chrono::steady_clock::now();
        std::vector<face_work_t> work(n);
        tbb::parallel_for(size_t(0), n, [&](size_t k) {
            lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
            if (dirt_in_use && !ls.dirt_calculated && !(gpu_dirt && ls.object_channel_mask == CHANNEL_MASK_DEFAULT)) {
                const auto d0 = std::chrono::steady_clock::now();
                LightFace_CalculateDirt(&ls);
                ls.dirt_calculated = true;
                dirt_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - d0)
                               .count();
            }
            face_work_t &w = work[k];
            thread_local std::vector<std::uint32_t> candidates;
            candidate_lights(ls, candidates);
            for (std::uint32_t li : candidates) {
                const gpu_light_ref_t &ref = refs[li];
                const bool reaches =
                    ref.entity ? GPU_EntityReachesFace(bsp, *ref.entity, ls) : GPU_SunReachesFace(*ref.sun, ls);
                if (!reaches)
                    continue;
                auto it = std::find(w.styles.begin(), w.styles.end(), ref.style);
                if (it == w.styles.end()) {
                    if (w.styles.size() == GPU_MAX_STYLE_SLOTS) {
                        if (ref.entity)
                            w.overflow_lights.push_back(ref.entity);
                        else
                            w.overflow_suns.push_back(ref.sun);
                        continue;
                    }
                    w.styles.push_back(ref.style);
                    it = w.styles.end() - 1;
                }
                const std::uint32_t slot = static_cast<std::uint32_t>(it - w.styles.begin());
                w.entries.push_back(li | (slot << gpu_light::ENTRY_SLOT_SHIFT));
            }
        });

        // pack the batch
        std::uint32_t slot_count = 1;
        samples.clear();
        faces.clear();
        entries.clear();
        for (size_t k = 0; k < n; k++) {
            face_work_t &w = work[k];
            total_overflow += w.overflow_lights.size() + w.overflow_suns.size();
            const lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
            gpu_light::face_t f{};
            f.entry_begin = static_cast<std::uint32_t>(entries.size());
            f.entry_count = static_cast<std::uint32_t>(w.entries.size());
            f.flags = (ls.twosided ? gpu_light::FACE_FLAG_TWOSIDED : 0) | (ls.nodirt ? gpu_light::FACE_FLAG_NODIRT : 0);
            const std::uint32_t face_id = static_cast<std::uint32_t>(faces.size());
            faces.push_back(f);
            entries.insert(entries.end(), w.entries.begin(), w.entries.end());
            slot_count = std::max(slot_count, static_cast<std::uint32_t>(w.styles.size()));

            w.first_sample = samples.size();
            for (const auto &sample : ls.samples) {
                gpu_light::sample_t s{};
                for (int i = 0; i < 3; i++) {
                    s.pos[i] = sample.point[i];
                    s.normal[i] = sample.normal[i];
                }
                s.occlusion = sample.occlusion;
                // for the dirt pass: skip occluded samples and faces whose dirt is already done
                s.face = (sample.occluded || ls.dirt_calculated) ? gpu_light::SAMPLE_SKIP : face_id;
                samples.push_back(s);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        prep_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

        // dirt on the GPU; samples whose rays met filtered geometry are redone on the CPU per face
        if (dirt_in_use) {
            std::vector<std::uint8_t> needs_cpu(n, 0);
            if (gpu_dirt) {
                gpu_light::dirt_batch_t dirt_batch;
                dirt_batch.samples = samples.data();
                dirt_batch.sample_count = samples.size();
                dirt_batch.vectors = &dirt_vectors4[0][0];
                dirt_batch.vector_count = numDirtVectors;
                dirt_batch.depth = cfg.dirtdepth.value();
                gpu_light::dirt_result_t dirt_result;
                std::string error;
                if (!gpu_light::trace_dirt(dirt_batch, dirt_result, error))
                    FError("GPU dirt failed: {}", error);
                dirt_gpu_ms += dirt_result.gpu_ms;

                for (size_t k = 0; k < n; k++) {
                    lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
                    if (ls.dirt_calculated)
                        continue;
                    const face_work_t &w = work[k];
                    for (size_t i = 0; i < ls.samples.size(); i++) {
                        const float occ = dirt_result.occlusion[w.first_sample + i];
                        if (occ == gpu_light::DIRT_NEEDS_CPU) {
                            needs_cpu[k] = 1;
                            break;
                        }
                        ls.samples[i].occlusion = occ;
                    }
                    if (!needs_cpu[k])
                        ls.dirt_calculated = true;
                }
            }
            tbb::parallel_for(size_t(0), n, [&](size_t k) {
                lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
                if (ls.dirt_calculated)
                    return;
                const auto d0 = std::chrono::steady_clock::now();
                LightFace_CalculateDirt(&ls);
                ls.dirt_calculated = true;
                dirt_cpu_faces++;
                dirt_ns +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - d0).count();
            });
        }

        // samples for the direct pass: final dirt, skip occluded samples and faces without GPU lights
        for (size_t k = 0; k < n; k++) {
            const lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
            const face_work_t &w = work[k];
            for (size_t i = 0; i < ls.samples.size(); i++) {
                gpu_light::sample_t &s = samples[w.first_sample + i];
                s.occlusion = ls.samples[i].occlusion;
                s.face = (ls.samples[i].occluded || w.entries.empty()) ? gpu_light::SAMPLE_SKIP
                                                                        : static_cast<std::uint32_t>(k);
            }
        }
        dirt_total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();

        gpu_light::batch_t batch;
        batch.lights = gpu_lights.data();
        batch.light_count = gpu_lights.size();
        batch.samples = samples.data();
        batch.sample_count = samples.size();
        batch.faces = faces.data();
        batch.face_count = faces.size();
        batch.entries = entries.data();
        batch.entry_count = entries.size();
        batch.slot_count = slot_count;

        gpu_light::batch_result_t result;
        std::string error;
        if (!gpu_light::trace_batch(batch, result, error))
            FError("GPU direct lighting failed: {}", error);
        gpu_ms += result.gpu_ms;
        total_samples += samples.size();
        total_entries += entries.size();
        total_retraces += result.retrace_count;

        // retraces, bucketed by face (k)
        std::vector<size_t> gpu_face_to_k;
        gpu_face_to_k.reserve(faces.size());
        for (size_t k = 0; k < n; k++)
            gpu_face_to_k.push_back(k);
        std::vector<std::vector<gpu_light::retrace_t>> retraces_by_face(n);
        for (size_t r = 0; r < result.retrace_count; r++) {
            const gpu_light::retrace_t &rt = result.retraces[r];
            retraces_by_face[gpu_face_to_k[samples[rt.sample].face]].push_back(rt);
        }

        // apply (parallel): add GPU sums, then re-trace what the GPU couldn't resolve
        auto t2 = std::chrono::steady_clock::now();
        tbb::parallel_for(size_t(0), n, [&](size_t k) {
            face_work_t &w = work[k];
            lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
            lightmapdict_t *lightmaps = &ls.lightmapsByStyle;

            for (std::uint32_t slot = 0; slot < w.styles.size(); slot++) {
                bool any = false;
                for (size_t i = 0; i < ls.samples.size() && !any; i++)
                    any = result.accum[(w.first_sample + i) * slot_count + slot].hits > 0;
                if (!any)
                    continue;
                const int style = w.styles[slot];
                lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, style, &ls);
                for (size_t i = 0; i < ls.samples.size(); i++) {
                    const gpu_light::accum_t &a = result.accum[(w.first_sample + i) * slot_count + slot];
                    if (!(a.hits > 0))
                        continue;
                    const qvec3f color{a.color[0], a.color[1], a.color[2]};
                    lightsample_t &sample = lightmap->samples[i];
                    sample.color += color;
                    sample.direction += qvec3f{a.direction[0], a.direction[1], a.direction[2]};
                    lightmap->bounce_color += color;
                }
                Lightmap_Save(bsp, lightmaps, &ls, lightmap, style);
            }

            auto &face_retraces = retraces_by_face[k];
            if (!face_retraces.empty()) {
                // group by light, keep DirectLightFace's light order
                std::sort(face_retraces.begin(), face_retraces.end(),
                    [&](const gpu_light::retrace_t &a, const gpu_light::retrace_t &b) {
                        const std::uint32_t la = entries[a.entry] & gpu_light::ENTRY_LIGHT_MASK;
                        const std::uint32_t lb = entries[b.entry] & gpu_light::ENTRY_LIGHT_MASK;
                        return la != lb ? la < lb : a.sample < b.sample;
                    });
                std::vector<int> only_samples;
                for (size_t r = 0; r < face_retraces.size();) {
                    const std::uint32_t li = entries[face_retraces[r].entry] & gpu_light::ENTRY_LIGHT_MASK;
                    only_samples.clear();
                    for (; r < face_retraces.size() &&
                           (entries[face_retraces[r].entry] & gpu_light::ENTRY_LIGHT_MASK) == li;
                         r++) {
                        only_samples.push_back(static_cast<int>(face_retraces[r].sample - w.first_sample));
                    }
                    const gpu_light_ref_t &ref = refs[li];
                    if (ref.entity)
                        LightFace_Entity(bsp, ref.entity, &ls, lightmaps, &only_samples);
                    else
                        LightFace_Sky(bsp, ref.sun, &ls, lightmaps, &only_samples);
                }
            }
            if (validate)
                GPU_ValidateFace(bsp, ls, w.entries, w.styles, refs);
            ls.gpu_overflow_lights = std::move(w.overflow_lights);
            ls.gpu_overflow_suns = std::move(w.overflow_suns);
            ls.gpu_direct_done = true;
        });
        apply_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();

        batch_start = batch_end;
        logging::percent(batch_start, face_indices.size());
    }

    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
    logging::print("GPU direct lighting: {} faces, {} samples, {} face-light pairs ({} left to the CPU: "
                   "more than {} styles on a face), {} rays re-traced on the CPU\n",
        face_indices.size(), total_samples, total_entries, total_overflow, GPU_MAX_STYLE_SLOTS, total_retraces);
    logging::print("GPU direct lighting: {:.0f} ms total = {:.0f} prep (culls) + {:.0f} dirt ({:.0f} GPU; {} faces "
                   "on the CPU, {:.0f} ms CPU time) + {:.0f} GPU + {:.0f} apply\n",
        total_ms, prep_ms, dirt_total_ms, dirt_gpu_ms, dirt_cpu_faces.load(), dirt_ns.load() / 1e6, gpu_ms, apply_ms);
    if (validate) {
        logging::print("GPU validate: {} of {} samples differ from the CPU by more than 2; dirt differs by more than "
                       "0.01 on {}, by more than 0.1 on {}\n",
            gpu_validate_mismatches.load(), gpu_validate_samples.load(), gpu_validate_dirt_mismatches.load(),
            gpu_validate_dirt_big_mismatches.load());
    }
}

void GPU_SurfaceLighting(const mbsp_t *bsp, std::span<lightsurf_t> surfaces, std::optional<size_t> bounce_depth,
    float standard_scale, float sky_scale, float hotspot_clamp)
{
    const auto t_start = std::chrono::steady_clock::now();
    const settings::worldspawn_keys &cfg = light_options;
    const float gate = SurfaceLightGate();
    const std::vector<lightsurf_t *> &emitter_surfs = EmissiveLightSurfaces();

    // one GPU emitter per (emissive surface, style) of this bounce level
    struct emitter_ref_t
    {
        const lightsurf_t *surf;
        const surfacelight_t::per_style_t *style;
    };
    std::vector<gpu_light::emitter_t> emitters;
    std::vector<emitter_ref_t> emitter_refs;
    std::vector<std::array<float, 4>> points;
    std::vector<std::uint32_t> first_emitter(emitter_surfs.size() + 1); // surface index -> emitters
    for (size_t idx = 0; idx < emitter_surfs.size(); idx++) {
        first_emitter[idx] = static_cast<std::uint32_t>(emitters.size());
        const surfacelight_t &vpl = *emitter_surfs[idx]->vpl;
        const std::uint32_t point_begin = static_cast<std::uint32_t>(points.size());
        bool any = false;
        for (const auto &style : vpl.styles) {
            if (style.bounce_level != bounce_depth)
                continue;
            any = true;
            gpu_light::emitter_t E{};
            for (int i = 0; i < 3; i++) {
                E.surfnormal[i] = vpl.surfnormal[i];
                E.color[i] = style.color[i];
            }
            E.intensity = style.intensity;
            E.atten = style.atten;
            E.point_begin = point_begin;
            E.point_count = static_cast<std::uint32_t>(vpl.points.size());
            E.flags = (style.omnidirectional ? gpu_light::EMITTER_FLAG_OMNIDIRECTIONAL : 0) |
                      (style.rescale ? gpu_light::EMITTER_FLAG_RESCALE : 0);
            emitters.push_back(E);
            emitter_refs.push_back({emitter_surfs[idx], &style});
        }
        if (any) {
            for (const qvec3f &p : vpl.points)
                points.push_back({p[0], p[1], p[2], 0.0f});
        }
    }
    first_emitter[emitter_surfs.size()] = static_cast<std::uint32_t>(emitters.size());
    if (points.empty())
        points.push_back({}); // keep the buffer non-empty
    if (emitters.size() > gpu_light::ENTRY_LIGHT_MASK)
        FError("too many surface lights for the GPU path");

    auto mark_done = [&](lightsurf_t &ls) {
        if (bounce_depth)
            ls.gpu_bounce_depth_done = static_cast<int>(*bounce_depth);
        else
            ls.gpu_surflights_done = true;
    };

    // faces: the same ones DirectLightFace / IndirectLightFace would light
    std::vector<size_t> face_indices;
    for (size_t i = 0; i < surfaces.size(); i++) {
        lightsurf_t &ls = surfaces[i];
        const mface_t *face = &bsp->dfaces[i];
        if (!Face_IsLightmapped(bsp, face) || ls.samples.empty() || ls.cfg != &cfg)
            continue;
        const modelinfo_t *modelinfo = ModelInfoForFace(bsp, i);
        if (modelinfo->lightignore.value() || extended_texinfo_flags[face->texinfo].light_ignore)
            continue;
        if (emitters.empty() || !(ls.object_channel_mask & CHANNEL_MASK_DEFAULT)) {
            mark_done(ls); // LightFace_SurfaceLight would do nothing
            continue;
        }
        face_indices.push_back(i);
    }

    gpu_light::surflight_params_t params;
    params.standard_scale = standard_scale;
    params.sky_scale = sky_scale;
    params.hotspot_clamp = hotspot_clamp;
    params.gate = gate;
    params.sky_dist = cfg.surflightskydist.value();
    params.scaledist = cfg.scaledist.value();
    params.dirtgain = cfg.dirtgain.value();
    params.dirtscale = cfg.dirtscale.value();
    params.dirt = dirt_in_use ? 1 : 0;

    struct face_work_t
    {
        std::vector<std::uint32_t> entries; // emitter index | slot << ENTRY_SLOT_SHIFT
        std::vector<int> styles; // slot -> style
        std::vector<std::uint32_t> overflow; // emitters done on the CPU (no free style slot)
        size_t first_sample = 0;
    };

    size_t total_samples = 0, total_entries = 0, total_retraces = 0, total_overflow = 0;
    double gpu_ms = 0, prep_ms = 0, apply_ms = 0;
    std::vector<gpu_light::sample_t> samples;
    std::vector<gpu_light::face_t> faces;
    std::vector<std::uint32_t> entries;

    for (size_t batch_start = 0; batch_start < face_indices.size();) {
        size_t batch_end = batch_start, batch_samples = 0;
        while (batch_end < face_indices.size() &&
               (batch_end == batch_start || batch_samples + surfaces[face_indices[batch_end]].samples.size() <=
                                                GPU_BATCH_SAMPLES)) {
            batch_samples += surfaces[face_indices[batch_end]].samples.size();
            batch_end++;
        }
        const size_t n = batch_end - batch_start;

        // prep: the culls of LightFace_SurfaceLight, style slots (parallel)
        auto t0 = std::chrono::steady_clock::now();
        std::vector<face_work_t> work(n);
        tbb::parallel_for(size_t(0), n, [&](size_t k) {
            lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
            face_work_t &w = work[k];
            thread_local std::vector<std::uint32_t> candidates;
            const surflight_candidates_t kind = SurfaceLightCandidates(ls, candidates);
            if (kind == surflight_candidates_t::all) {
                candidates.resize(emitter_surfs.size());
                for (std::uint32_t i = 0; i < emitter_surfs.size(); i++)
                    candidates[i] = i;
            }
            for (std::uint32_t idx : candidates) {
                const lightsurf_t *surf = emitter_surfs[idx];
                const surfacelight_t &vpl = *surf->vpl;
                std::optional<bool> vis_culled;
                for (std::uint32_t e = first_emitter[idx]; e < first_emitter[idx + 1]; e++) {
                    const surfacelight_t::per_style_t &style = *emitter_refs[e].style;
                    if (SurfaceLight_SphereCull(&vpl, &ls, style, gate, hotspot_clamp))
                        continue;
                    if (!vis_culled)
                        vis_culled = kind != surflight_candidates_t::near_visible &&
                                     SurfaceLight_VisCull(bsp, &ls.pvs, surf);
                    if (*vis_culled)
                        continue;
                    auto it = std::find(w.styles.begin(), w.styles.end(), style.style);
                    if (it == w.styles.end()) {
                        if (w.styles.size() == GPU_MAX_STYLE_SLOTS) {
                            w.overflow.push_back(e);
                            continue;
                        }
                        w.styles.push_back(style.style);
                        it = w.styles.end() - 1;
                    }
                    const std::uint32_t slot = static_cast<std::uint32_t>(it - w.styles.begin());
                    w.entries.push_back(e | (slot << gpu_light::ENTRY_SLOT_SHIFT));
                }
            }
        });

        // pack the batch
        std::uint32_t slot_count = 1;
        samples.clear();
        faces.clear();
        entries.clear();
        for (size_t k = 0; k < n; k++) {
            face_work_t &w = work[k];
            total_overflow += w.overflow.size();
            const lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
            gpu_light::face_t f{};
            f.entry_begin = static_cast<std::uint32_t>(entries.size());
            f.entry_count = static_cast<std::uint32_t>(w.entries.size());
            f.flags = (ls.twosided ? gpu_light::FACE_FLAG_TWOSIDED : 0) | (ls.nodirt ? gpu_light::FACE_FLAG_NODIRT : 0);
            faces.push_back(f);
            entries.insert(entries.end(), w.entries.begin(), w.entries.end());
            slot_count = std::max(slot_count, static_cast<std::uint32_t>(w.styles.size()));

            w.first_sample = samples.size();
            for (const auto &sample : ls.samples) {
                gpu_light::sample_t s{};
                for (int i = 0; i < 3; i++) {
                    s.pos[i] = sample.point[i];
                    s.normal[i] = sample.normal[i];
                }
                s.occlusion = sample.occlusion;
                s.face = (sample.occluded || w.entries.empty()) ? gpu_light::SAMPLE_SKIP : static_cast<std::uint32_t>(k);
                samples.push_back(s);
            }
        }
        prep_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        gpu_light::surflight_batch_t batch;
        batch.emitters = emitters.data();
        batch.emitter_count = emitters.size();
        batch.points = &points[0][0];
        batch.point_count = points.size();
        batch.samples = samples.data();
        batch.sample_count = samples.size();
        batch.faces = faces.data();
        batch.face_count = faces.size();
        batch.entries = entries.data();
        batch.entry_count = entries.size();
        batch.slot_count = slot_count;
        batch.params = params;

        gpu_light::batch_result_t result;
        std::string error;
        if (!gpu_light::trace_surflight(batch, result, error))
            FError("GPU surface lighting failed: {}", error);
        gpu_ms += result.gpu_ms;
        total_samples += samples.size();
        total_entries += entries.size();
        total_retraces += result.retrace_count;

        std::vector<std::vector<gpu_light::retrace_t>> retraces_by_face(n);
        for (size_t r = 0; r < result.retrace_count; r++) {
            const gpu_light::retrace_t &rt = result.retraces[r];
            retraces_by_face[samples[rt.sample].face].push_back(rt);
        }

        // apply (parallel): GPU sums, CPU re-traces, emitters that didn't fit in the style slots
        auto t2 = std::chrono::steady_clock::now();
        tbb::parallel_for(size_t(0), n, [&](size_t k) {
            face_work_t &w = work[k];
            lightsurf_t &ls = surfaces[face_indices[batch_start + k]];
            lightmapdict_t *lightmaps = &ls.lightmapsByStyle;

            for (std::uint32_t slot = 0; slot < w.styles.size(); slot++) {
                bool any = false;
                for (size_t i = 0; i < ls.samples.size() && !any; i++)
                    any = result.accum[(w.first_sample + i) * slot_count + slot].hits > 0;
                if (!any)
                    continue;
                const int style = w.styles[slot];
                lightmap_t *lightmap = Lightmap_ForStyle(lightmaps, style, &ls);
                for (size_t i = 0; i < ls.samples.size(); i++) {
                    const gpu_light::accum_t &a = result.accum[(w.first_sample + i) * slot_count + slot];
                    if (!(a.hits > 0))
                        continue;
                    const qvec3f color{a.color[0], a.color[1], a.color[2]};
                    lightmap->samples[i].color += color;
                    lightmap->bounce_color += color;
                }
                Lightmap_Save(bsp, lightmaps, &ls, lightmap, style);
            }

            auto &face_retraces = retraces_by_face[k];
            if (!face_retraces.empty()) {
                std::sort(face_retraces.begin(), face_retraces.end(),
                    [&](const gpu_light::retrace_t &a, const gpu_light::retrace_t &b) {
                        const std::uint32_t ea = entries[a.entry] & gpu_light::ENTRY_LIGHT_MASK;
                        const std::uint32_t eb = entries[b.entry] & gpu_light::ENTRY_LIGHT_MASK;
                        return ea != eb ? ea < eb : a.sample < b.sample;
                    });
                std::vector<int> only_samples;
                for (size_t r = 0; r < face_retraces.size();) {
                    const std::uint32_t e = entries[face_retraces[r].entry] & gpu_light::ENTRY_LIGHT_MASK;
                    only_samples.clear();
                    for (; r < face_retraces.size() &&
                           (entries[face_retraces[r].entry] & gpu_light::ENTRY_LIGHT_MASK) == e;
                         r++) {
                        only_samples.push_back(static_cast<int>(face_retraces[r].sample - w.first_sample));
                    }
                    LightFace_SurfaceLightEmitter(&ls, lightmaps, *emitter_refs[e].surf->vpl, *emitter_refs[e].style,
                        standard_scale, sky_scale, hotspot_clamp, gate, &only_samples);
                }
            }
            for (std::uint32_t e : w.overflow) {
                LightFace_SurfaceLightEmitter(&ls, lightmaps, *emitter_refs[e].surf->vpl, *emitter_refs[e].style,
                    standard_scale, sky_scale, hotspot_clamp, gate);
            }
            mark_done(ls);
        });
        apply_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();

        batch_start = batch_end;
        logging::percent(batch_start, face_indices.size());
    }

    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
    logging::print("GPU surface lights ({}): {} emitters, {} faces, {} samples, {} face-emitter pairs ({} on the CPU: "
                   "more than {} styles), {} pairs re-traced on the CPU\n",
        bounce_depth ? fmt::format("bounce {}", *bounce_depth) : std::string("direct"), emitters.size(),
        face_indices.size(), total_samples, total_entries, total_overflow, GPU_MAX_STYLE_SLOTS, total_retraces);
    logging::print("GPU surface lights: {:.0f} ms total = {:.0f} prep (culls) + {:.0f} GPU + {:.0f} apply\n", total_ms,
        prep_ms, gpu_ms, apply_ms);
}

#endif // HAVE_GPU_LIGHT

/*
 * ============
 * LightFace
 * ============
 */
void DirectLightFace(const mbsp_t *bsp, lightsurf_t &lightsurf, const settings::worldspawn_keys &cfg)
{
    auto face = lightsurf.face;
    const modelinfo_t *modelinfo = ModelInfoForFace(bsp, Face_GetNum(bsp, face));

    lightmapdict_t *lightmaps = &lightsurf.lightmapsByStyle;

    /* calculate dirt (ambient occlusion) but don't use it yet */
    if (dirt_in_use && (light_options.debugmode != debugmodes::phong) && !lightsurf.dirt_calculated)
        LightFace_CalculateDirt(&lightsurf);

    /*
     * The lighting procedure is: cast all positive lights, fix
     * minlight levels, then cast all negative lights. Finally, we
     * clamp any values that may have gone negative.
     */

    if (light_options.debugmode == debugmodes::none) {
#if 0
        total_samplepoints += lightsurf.samples.size();
#endif

        const surfflags_t &extended_flags = extended_texinfo_flags[face->texinfo];

        /* positive lights */
        if (!(modelinfo->lightignore.value() || extended_flags.light_ignore)) {
            if (lightsurf.gpu_direct_done) {
                // the GPU pass did the other positive lights
                for (const light_t *entity : gpu_cpu_lights)
                    LightFace_Entity(bsp, entity, &lightsurf, lightmaps);
                for (const light_t *entity : lightsurf.gpu_overflow_lights)
                    LightFace_Entity(bsp, entity, &lightsurf, lightmaps);
                for (const sun_t *sun : gpu_cpu_suns)
                    LightFace_Sky(bsp, sun, &lightsurf, lightmaps);
                for (const sun_t *sun : lightsurf.gpu_overflow_suns)
                    LightFace_Sky(bsp, sun, &lightsurf, lightmaps);
            } else {
                for (const auto &entity : GetLights()) {
                    if (entity->getFormula() == LF_LOCALMIN)
                        continue;
                    if (entity->nostaticlight.value())
                        continue;
                    if (entity->light.value() > 0)
                        LightFace_Entity(bsp, entity.get(), &lightsurf, lightmaps);
                }
                for (const sun_t &sun : GetSuns())
                    if (sun.sunlight > 0)
                        LightFace_Sky(bsp, &sun, &lightsurf, lightmaps);
            }

            // mxd. Add surface lights...
            // FIXME: negative surface lights
            if (!lightsurf.gpu_surflights_done)
                LightFace_SurfaceLight(bsp, &lightsurf, lightmaps, std::nullopt, cfg.surflightscale.value(),
                    cfg.surflightskyscale.value(), 16.0f);
        }

        LightFace_LocalMin(bsp, face, &lightsurf, lightmaps);
    }

    /* replace lightmaps with AO for debugging */
    if (light_options.debugmode == debugmodes::dirt)
        LightFace_DirtDebug(bsp, &lightsurf, lightmaps);

    if (light_options.debugmode == debugmodes::phong)
        LightFace_PhongDebug(bsp, &lightsurf, lightmaps);

    if (light_options.debugmode == debugmodes::debugoccluded)
        LightFace_OccludedDebug(bsp, &lightsurf, lightmaps);

    if (light_options.debugmode == debugmodes::debugneighbours)
        LightFace_DebugNeighbours(bsp, &lightsurf, lightmaps);
}

/*
 * ============
 * IndirectLightFace
 * ============
 */
void IndirectLightFace(
    const mbsp_t *bsp, lightsurf_t &lightsurf, const settings::worldspawn_keys &cfg, size_t bounce_depth)
{
    auto face = lightsurf.face;
    const modelinfo_t *modelinfo = ModelInfoForFace(bsp, Face_GetNum(bsp, face));
    lightmapdict_t *lightmaps = &lightsurf.lightmapsByStyle;

    if (light_options.debugmode == debugmodes::none) {
        const surfflags_t &extended_flags = extended_texinfo_flags[face->texinfo];

        /* positive lights */
        if (!(modelinfo->lightignore.value() || extended_flags.light_ignore)) {

            /* add bounce lighting */
            // note: scale here is just to keep it close-ish to the old code
            if (lightsurf.gpu_bounce_depth_done != static_cast<int>(bounce_depth))
                LightFace_SurfaceLight(bsp, &lightsurf, lightmaps, bounce_depth, cfg.bouncescale.value() * 0.5,
                    cfg.bouncescale.value(), 128.0f);
        }
    }
}

/*
 * ============
 * PostProcessLightFace
 * ============
 */
void PostProcessLightFace(const mbsp_t *bsp, lightsurf_t &lightsurf, const settings::worldspawn_keys &cfg)
{
    auto face = lightsurf.face;
    const modelinfo_t *modelinfo = ModelInfoForFace(bsp, Face_GetNum(bsp, face));

    lightmapdict_t *lightmaps = &lightsurf.lightmapsByStyle;

    if (light_options.debugmode == debugmodes::none) {
#if 0
        total_samplepoints += lightsurf.samples.size();
#endif

        const surfflags_t &extended_flags = extended_texinfo_flags[face->texinfo];

        float minlight = 0;
        qvec3f minlight_color;

        // first, check for global minlight; this only affects style 0
        if (lightsurf.minlight > cfg.minlight.value()) {
            minlight = lightsurf.minlight;
            minlight_color = lightsurf.minlight_color;
        } else {
            minlight = cfg.minlight.value();
            minlight_color = cfg.minlight_color.value();
        }

        if (minlight) {
            LightFace_Min(bsp, face, minlight_color, minlight, &lightsurf, lightmaps, 0);
        }

        if (lightsurf.vpl) {
            if (auto value = IsSurfaceLitFace(bsp, face)) {
                auto *entity = std::get<3>(value.value());
                float surface_minlight_scale = entity ? entity->surflight_minlight_scale.value() : 1.f;
                surface_minlight_scale *= lightsurf.surflight_minlight_scale;

                if (surface_minlight_scale > 0) {
                    minlight = std::get<0>(value.value()) * surface_minlight_scale;
                    minlight_color = std::get<2>(value.value());
                    LightFace_Min(
                        bsp, face, minlight_color, minlight, &lightsurf, lightmaps, std::get<1>(value.value()));
                }
            }
        }

        if (!modelinfo->isWorld()) {
            LightFace_AutoMin(bsp, face, &lightsurf, lightmaps);
        }

        /* negative lights */
        if (!(modelinfo->lightignore.value() || extended_flags.light_ignore)) {
            for (const auto &entity : GetLights()) {
                if (entity->getFormula() == LF_LOCALMIN)
                    continue;
                if (entity->nostaticlight.value())
                    continue;
                if (entity->light.value() < 0)
                    LightFace_Entity(bsp, entity.get(), &lightsurf, lightmaps);
            }
            for (const sun_t &sun : GetSuns())
                if (sun.sunlight < 0)
                    LightFace_Sky(bsp, &sun, &lightsurf, lightmaps);
        }
    }

    if (light_options.debugmode == debugmodes::mottle)
        LightFace_DebugMottle(bsp, &lightsurf, lightmaps);
}
// lightgrid

void lightgrid_samples_t::add(const qvec3f &color, int style, const qvec3f &grid_to_light_dir, float anglescale)
{
    for (auto &sample : samples_by_style) {
        if (!sample.used) {
            // allocate new style
            sample.used = true;
            sample.style = style;

            sample.add(color, grid_to_light_dir, anglescale);
            return;
        }

        if (sample.style == style) {
            // found matching style
            sample.add(color, grid_to_light_dir, anglescale);
            return;
        }
    }

    // uncommon case: all slots are used, but we didn't find a matching style
    // drop the style with the lowest brightness

    int min_brightness_index = 0;
    float min_brightness = FLT_MAX;

    for (int i = 0; i < samples_by_style.size(); ++i) {
        float i_brightness = samples_by_style[i].brightness();
        if (i_brightness < min_brightness) {
            min_brightness_index = i;
            min_brightness = i_brightness;
        }
    }

    // overwrite the sample at min_brightness_index
    auto &target = samples_by_style[min_brightness_index];
    target = lightgrid_sample_t(); // clear it
    target.used = true;
    target.style = style;

    target.add(color, grid_to_light_dir, anglescale);
}

qvec3b lightgrid_sample_t::round_to_int() const
{
    return ::round_to_int(undirectional_color);
}

qvec3b lightgrid_sample_t::round_to_int(int side) const
{
    return ::round_to_int(colors[side]);
}

qvec3b round_to_int(const qvec3f &color)
{
    return qvec3b{std::clamp((int)round(color[0]), 0, 255), std::clamp((int)round(color[1]), 0, 255),
        std::clamp((int)round(color[2]), 0, 255)};
}

float lightgrid_sample_t::brightness() const
{
    float total = 0.0f;
    for (const qvec3f &c : colors)
        for (int i = 0; i < 3; ++i)
            total += c[i];
    return total / 18.0f; // 3 components * 6 cube sides
}

void lightgrid_sample_t::add(const qvec3f &color, const qvec3f &grid_to_light_dir, float anglescale)
{
    float max_angle_coefficient = 0.0f;

    for (int i = 0; i < 6; ++i) {
        const qvec3f &cube_normal = BSPX_LIGHTGRIDS_NORMAL_ORDER[i];

        float dp = qv::dot(cube_normal, grid_to_light_dir);
        if (dp < 0)
            continue;

        float angle_coefficient = mix(1.0f, dp, anglescale);
        colors[i] += color * angle_coefficient;

        max_angle_coefficient = std::max(max_angle_coefficient, angle_coefficient);
    }

    // for backwards compat, update undirectional_color
    undirectional_color += color * max_angle_coefficient;
}

int lightgrid_samples_t::used_styles() const
{
    int used = 0;
    for (auto &sample : samples_by_style) {
        if (sample.used) {
            used++;
        } else {
            break;
        }
    }
    return used;
}

bspx_lightgrid_samples_t lightgrid_samples_t::to_bspx_lightgrid_samples() const
{
    bspx_lightgrid_samples_t sample_out;

    if (occluded) {
        sample_out.occluded = true;
        return sample_out;
    }

    for (int i = 0; i < used_styles(); ++i) {
        sample_out.insert(bspx_lightgrid_sample_t{
            .color = samples_by_style[i].round_to_int(),
            .style = static_cast<uint8_t>(samples_by_style[i].style),
        });
    }

    return sample_out;
}

lightgrids_sampleset_t lightgrid_samples_t::to_lightgrids_sampleset_t() const
{
    lightgrids_sampleset_t sample_out;

    if (occluded) {
        sample_out.occluded = true;
        return sample_out;
    }

    for (int i = 0; i < used_styles(); ++i) {
        lightgrids_sample_t samp;

        for (int j = 0; j < 6; ++j) {
            samp.colors[j] = samples_by_style[i].round_to_int(j);
        }
        samp.style = static_cast<uint8_t>(samples_by_style[i].style);

        sample_out.insert(samp);
    }

    return sample_out;
}

lightgrid_samples_t CalcLightgridAtPoint(const mbsp_t *bsp, const qvec3f &world_point)
{
    // TODO: use more than 1 ray for better performance
    raystream_occlusion_t rs(1);
    raystream_intersection_t rsi(1);

    const auto *pvs = Mod_LeafPvs(bsp, BSP_FindLeafAtPoint(bsp, &bsp->dmodels[0], world_point));

    auto &cfg = light_options;

    lightgrid_samples_t result;

    // from DirectLightFace

    /*
     * The lighting procedure is: cast all positive lights, fix
     * minlight levels, then cast all negative lights. Finally, we
     * clamp any values that may have gone negative.
     */

    /* positive lights */
    for (const auto &entity : GetLights()) {
        if (entity->getFormula() == LF_LOCALMIN)
            continue;
        if (entity->nostaticlight.value())
            continue;
        if (entity->light.value() > 0)
            LightPoint_Entity(bsp, rs, entity.get(), world_point, result);
    }

    for (const sun_t &sun : GetSuns())
        if (sun.sunlight > 0)
            LightPoint_Sky(bsp, rsi, &sun, world_point, result);

    // mxd. Add surface lights...
    // FIXME: negative surface lights
    LightPoint_SurfaceLight(
        bsp, pvs, rs, false, cfg.surflightscale.value(), cfg.surflightskyscale.value(), 16.0f, world_point, result);

#if 0
    // FIXME: port to lightgrid
    float minlight = cfg.minlight.value();
    qvec3f minlight_color = cfg.minlight_color.value();

    if (minlight) {
        LightFace_Min(bsp, face, minlight_color, minlight, &lightsurf, lightmaps, 0);
    }

    LightFace_LocalMin(bsp, face, &lightsurf, lightmaps);
#endif

    /* negative lights */
    for (const auto &entity : GetLights()) {
        if (entity->getFormula() == LF_LOCALMIN)
            continue;
        if (entity->nostaticlight.value())
            continue;
        if (entity->light.value() < 0)
            LightPoint_Entity(bsp, rs, entity.get(), world_point, result);
    }
    for (const sun_t &sun : GetSuns())
        if (sun.sunlight < 0)
            LightPoint_Sky(bsp, rsi, &sun, world_point, result);

    // from IndirectLightFace

    /* add bounce lighting */
    // note: scale here is just to keep it close-ish to the old code
    LightPoint_SurfaceLight(
        bsp, pvs, rs, true, cfg.bouncescale.value() * 0.5, cfg.bouncescale.value(), 128.0f, world_point, result);

    LightPoint_ScaleAndClamp(result);

    return result;
}

void ResetLtFace()
{
#if 0
    total_light_rays = 0;
    total_light_ray_hits = 0;
    total_samplepoints = 0;

    total_bounce_rays = 0;
    total_bounce_ray_hits = 0;
    total_surflight_rays = 0;
    total_surflight_ray_hits = 0;
#endif
}

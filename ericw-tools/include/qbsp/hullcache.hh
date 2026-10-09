/*  qbspserver: clipping hulls reused between compiles.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#pragma once

#include <common/aabb.hh>

#include <cstddef>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

class mapentity_t;
struct node_t;
struct tree_t;
struct bspbrush_t;
struct buildportal_t;

/*
 * The clipping hulls (1, 2...) are most of a compile's time, and they only depend on the
 * shape and contents of the brushes, not on their textures (only on texture flags like skip
 * or hint). So a long-running process (qbspserver) keeps each entity's finished clipping
 * tree, and when a later compile has the same inputs it exports the stored tree instead of
 * building it again. The output is the same as rebuilding it.
 *
 * For the world, the outside filling also depends on the point entities, but only on whether
 * filling happens at all (see occupants_compatible() in hullcache.cc).
 *
 * Disabled by default: plain qbsp never caches.
 */
namespace hullcache
{
void set_enabled(bool enabled);
bool enabled();

// call at the start of every compile
void begin_compile();

// hull 0 of the world: whether FillOutside filled it (no leak); hulls 1+ rely on it
void note_hull0_filled(bool filled);

// drops everything the last compile didn't use (the benchmark calls this so that a measured
// edit doesn't find its own result from an earlier repetition)
void forget_unused();

// Before building `entity`'s clipping hull: if a stored tree can be reused, exports it
// (clipnodes, planes, model headnode) and returns true.
bool replay(mapentity_t &entity, int hullnum);

// After building and exporting `entity`'s clipping hull. `cacheable` is false when the result
// must not be reused (e.g. the world leaked, so no filling was done).
void store(mapentity_t &entity, int hullnum, node_t *headnode, bool cacheable);

/*
 * When the world's hull must be rebuilt (its brushes changed): qbsp builds it twice, a quick
 * tree only to find which brush sides face nothing but the void (FillOutside), then the final
 * tree, which uses those sides as splitters last. Those marks only steer the choice of
 * splitters: with marks from a previous compile (new or changed brushes: every side visible)
 * the final tree is just as valid, and the first tree is skipped.
 *
 * Only for the clipping hulls. Tried on hull 0 too: its faces come from the final tree's
 * portals, and a tree built with the final pass's marks (instead of the first pass's) lost a few
 * faces (e3m4: 0.01% of the drawn area, caught by bspcheck). Hull 0 keeps its two passes.
 */
// sets the side marks from the last compile and returns true if there are any (then skip the first tree)
bool apply_visibility(int hullnum);
// after the final FillOutside of the world's hull: remember the side marks for the next compile
void remember_visibility(int hullnum);

/*
 * Cells: when the world's hull must be rebuilt, most of it usually didn't change. Around the
 * final BrushBSP of a world clipping hull (cell_hull_begin / cell_hull_end), the top of the
 * tree is split on a fixed grid of cells (cell_size units, like qbsp's own midsplit for big
 * nodes), and each cell's subtree depends only on the brush fragments inside it. A cell whose
 * fragments (and bounds) are the same as in a previous compile gets its stored subtree grafted
 * instead of being built. Only the tree's shape changes (more grid splits), not what is solid.
 */
constexpr double cell_size = 1024;

void cell_hull_begin(int hullnum);
void cell_hull_end();

// for BrushBSP / BuildTree_r (brushbsp.cc)
bool cells_active();
// before building: the grid planes (created here, single-threaded) and brush lookups
void cells_prepare(const aabb3d &tree_bounds, const std::vector<std::shared_ptr<bspbrush_t>> &brushes);
// the grid plane to split `bounds` on, or nothing if `bounds` is a single cell
std::optional<size_t> grid_plane(const aabb3d &bounds);
// at a cell's root: grafts the stored subtree and returns true, or remembers the cell to store it
// once built and returns false. Thread-safe (BuildTree_r runs in parallel).
bool cell_from_cache(tree_t &tree, node_t *node, const std::vector<std::shared_ptr<bspbrush_t>> &brushes);
// after building: stores the cells that were built
void cells_finish();

/*
 * Cell portals: a portal is made from its node's plane clipped by every parent's plane, then
 * split down both sides. Inside a cell, the parents above the cell are grid planes, so the
 * portals between two leaves of the same cell only depend on the cell's box and subtree: a
 * cell keeps them with its subtree. MakeTreePortals_r (portals.cc) then only has to split the
 * portals on the cell's boundary down the cell's subtree. Active for the tree whose BrushBSP
 * just ran with cells, until cell_portals_done().
 */
struct cell_t;
// the cell whose root is `node` in the tree being portalized, or nullptr
cell_t *portal_cell(const node_t *node);
// the stored portals between two leaves of the cell; false if the cell has none stored yet
bool cell_internal_portals(cell_t &cell, node_t *node, std::list<buildportal_t> &out);
// after portalizing a cell normally: keep its internal portals
void cell_remember_portals(cell_t &cell, node_t *node, const std::list<buildportal_t> &portals);
void cell_portals_done();

/*
 * The stored fill of the world's clipping hulls. Their filling (portals of the whole tree, flood
 * from the void, void leaves made solid) costs more than building the cells that changed. After
 * a real filling, the server notes per cell which leaves were void, and the (hull-expanded) box
 * of every brush. On the next compile, if no box of a changed brush (old or new place) touches
 * a void leaf of the previous tree, the edit can't have changed the void: it can't have opened
 * or closed a way to it. Then the void is applied instead of computed: unchanged cells copy
 * their void leaves; a leaf of a rebuilt cell is connected empty space, so all void or none of
 * it, and an inside point of it (taken while building) is looked up in the previous tree of
 * that cell (a point in a changed box is not void). Anything unsure: a real filling.
 * Only with -filltype auto (FillDetail skipped in these hulls) and when hull 0 was filled.
 */
// after BrushBSP (with cells) of a world clipping hull: fills it from the stored data and returns
// true, or returns false (then: MakeTreePortals, FillOutside, record_void)
bool fill_from_cells(int hullnum, node_t *headnode);
// QBSPSERVER_CHECK_FILL=1: fill_from_cells only works out its answer (check_fill_pending), the
// usual filling runs, then check_fill_compare logs every leaf where the two disagree
bool check_fill_enabled();
void check_fill_compare(int hullnum);
// after a real FillOutside of a world clipping hull built with cells: note its void leaves
void record_void(int hullnum, node_t *headnode);
// in BuildTree_r, for a leaf of a cell being built, while its volume is still there
void cell_leaf_point(const node_t *leaf);

/*
 * qbsp chops the clipping hulls' brushes (removes their overlaps) so that BrushBSP has fewer
 * brush tests. With cells, BrushBSP only runs on the cells that changed, while chopping still
 * runs on the whole map every compile: the server skips it for the world's clipping hulls.
 * The solid space is the same either way. QBSPSERVER_CHOP_HULLS=1 chops anyway (to compare).
 */
bool chop_world_hulls();

/*
 * The cell grid (and not chopping) gives the clipping hulls 6-14% more clipnodes. A map that
 * just fits a format's limits with plain qbsp (BSP29: 32767 clipnodes) could then not fit, and
 * qbsp would silently switch to the extended format (BSP2), which the mapper's engine may not
 * load. So when the output doesn't fit while the grid is on, FinishBSPFile throws
 * limits_exceeded instead; the server turns the grid off (hulls_like_qbsp) and compiles again.
 * If the map doesn't fit without the grid either, plain qbsp would switch format too: the grid
 * goes back on and the format switches as usual. QBSPSERVER_FAKE_CLIPNODE_LIMIT=N pretends the
 * format holds N clipnodes (to test this).
 */
struct limits_exceeded : std::runtime_error
{
    limits_exceeded() : std::runtime_error("the output exceeds the format's limits with the cell grid") { }
};
// called by FinishBSPFile; `fits`: the output fits the requested format
void check_limits(bool fits, size_t clipnodes);
// whether to throw limits_exceeded (the grid is on and could be the reason)
bool grid_active();
// after limits_exceeded: build the clipping hulls like plain qbsp (no grid, chopped)
void hulls_like_qbsp();
std::optional<size_t> fake_clipnode_limit();

/*
 * Hull 0 of the world: its final tree (before filling) only depends on the brushes (shapes,
 * contents, texture flags, order) and on the side marks of qbsp's first pass. Those marks come
 * from the first filling, which keeps the areas of connected empty space that hold a point
 * entity (and FillDetail the same with detail as walls). These areas are a property of the
 * geometry, not of the tree. So with the same brushes and the same set of occupied areas, the
 * final tree is exactly the one a fresh compile would build: the server keeps it and skips both
 * BrushBSP passes and the first portals and filling. The filling of the final tree, the faces
 * (textures!), T-junctions and output are still done every compile.
 */
// before hull 0's first BrushBSP: if a stored tree applies, puts it in `tree` and returns true
bool hull0_reuse(tree_t &tree, mapentity_t &entity, const std::vector<std::shared_ptr<bspbrush_t>> &brushes);
// after hull 0's first tree and its portals, before its filling: keep it to place the entities
void hull0_store_first(tree_t &tree);
// after hull 0's final tree and its portals, before its filling: store both trees
void hull0_store(tree_t &tree, mapentity_t &entity, const std::vector<std::shared_ptr<bspbrush_t>> &brushes);

// one line per compile: hulls reused / rebuilt
void print_summary();
} // namespace hullcache

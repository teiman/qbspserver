/*  qbspserver: clipping hulls reused between compiles.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#include <qbsp/hullcache.hh>

#include <qbsp/map.hh>
#include <qbsp/qbsp.hh>
#include <qbsp/writebsp.hh>
#include <qbsp/brush.hh>
#include <qbsp/tree.hh>
#include <qbsp/portals.hh>
#include <common/log.hh>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hullcache
{
// entries not used by the last few compiles are dropped (keeps an edit / undo pair cached)
constexpr uint64_t keep_generations = 4;
// an entity this close to a node plane can't be placed reliably: rebuild
constexpr double on_plane_epsilon = 0.01;

struct key_t
{
    uint64_t a = 0, b = 0;
    bool operator==(const key_t &) const = default;
};

struct key_hash
{
    size_t operator()(const key_t &k) const { return static_cast<size_t>(k.a ^ (k.b * 0x9e3779b97f4a7c15ull)); }
};

// two independent 64-bit FNV-1a hashes: a collision would need both to collide
class hasher
{
public:
    void bytes(const void *data, size_t size)
    {
        auto *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; i++) {
            a = (a ^ p[i]) * 0x100000001b3ull;
            b = (b ^ p[i]) * 0x100000001b3ull;
        }
    }
    template<typename T>
    void value(const T &v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof(v));
    }
    void string(std::string_view s)
    {
        value(s.size());
        bytes(s.data(), s.size());
    }
    key_t key() const { return {a, b}; }

private:
    uint64_t a = 0xcbf29ce484222325ull;
    uint64_t b = 0x84222325cbf29ce4ull;
};

struct cnode_t
{
    qplane3d plane;
    int32_t children[2]; // >= 0: index in nodes, < 0: native contents
};

// brush -> which of its sides were visible (see apply_visibility)
using marks_t = std::unordered_map<key_t, std::vector<bool>, key_hash>;

struct entry_t
{
    std::vector<cnode_t> nodes;
    int32_t head = 0; // index in nodes, or native contents for a tree that is a single leaf
    // world only: the outside filling depends on these (entities that can occupy a leaf)
    bool check_occupants = false;
    std::vector<qvec3f> occupants;
    // world only: the side marks of the brushes after this tree's filling
    std::shared_ptr<const marks_t> marks;
    // world only: the cells the tree was built from (kept alive while this entry is used)
    std::vector<key_t> cell_keys;
    uint64_t last_used = 0;
};

static bool is_enabled = false;

// the cell grid / unchopped clipping hulls (see limits_exceeded in hullcache.hh)
enum class grid_mode_t
{
    on, // default
    off_for_limits, // the output didn't fit with the grid: hulls built like plain qbsp
    on_extended, // it didn't fit without the grid either: plain qbsp switches format too
};
static grid_mode_t grid_mode = grid_mode_t::on;
static uint64_t generation = 0;
static std::unordered_map<key_t, entry_t, key_hash> entries;

// the key computed by replay(), for store() right after
static const mapentity_t *pending_entity = nullptr;
static int pending_hull = -1;
static key_t pending_key;

static size_t reused = 0, rebuilt = 0, reused_nodes = 0;

// whether this compile's hull 0 world was filled (no leak, entities in empty space)
static bool hull0_filled = false;

// per hull: the side marks of the world as last compiled (built or reused)
static std::unordered_map<int, std::shared_ptr<const marks_t>> last_marks;
// from remember_visibility(), for the store() that follows
static std::shared_ptr<const marks_t> pending_marks;
// from cells_finish(), for the store() that follows
static std::vector<key_t> pending_cell_keys;

// marks the stored cells as used by this compile (defined with the cells, below)
static void touch_cells(int hullnum, const std::vector<key_t> &keys);

void set_enabled(bool enabled)
{
    is_enabled = enabled;
    if (!enabled) {
        entries.clear();
    }
}

bool enabled()
{
    return is_enabled;
}

// drop stored cells / hull 0 trees last used before `oldest` (defined with them, below)
static void prune_cells(uint64_t oldest);
static void prune_hull0(uint64_t oldest);

void begin_compile()
{
    generation++;
    std::erase_if(entries, [](const auto &e) { return e.second.last_used + keep_generations < generation; });
    prune_cells(generation > keep_generations ? generation - keep_generations : 0);
    prune_hull0(generation > keep_generations ? generation - keep_generations : 0);
    pending_entity = nullptr;
    reused = rebuilt = reused_nodes = 0;
    hull0_filled = false;
}

void note_hull0_filled(bool filled)
{
    hull0_filled = filled;
}

void forget_unused()
{
    std::erase_if(entries, [](const auto &e) { return e.second.last_used < generation; });
    prune_cells(generation);
    prune_hull0(generation);
}

// (the side marks of the last compile stay: they belong to what it used)

// everything of a brush that the clipping hulls read: shape, contents, and the texture flags
// that change how a side is treated (skip / hint / no_expand...). Not the texture itself.
static void hash_brush(hasher &h, const mapbrush_t &brush)
{
    h.value(brush.contents.flags);
    h.value(brush.is_hint);
    h.value(brush.chop_index);
    h.value(brush.faces.size());
    for (auto &face : brush.faces) {
        const qbsp_plane_t &plane = face.get_plane();
        h.value(plane.get_normal());
        h.value(plane.get_dist());
        h.value(face.contents.flags);
        h.value(face.bevel);
        const surfflags_t &flags = face.get_texinfo().flags;
        h.value(flags.native_q1);
        h.value(flags.native_q2);
        h.value(flags.is_nodraw());
        h.value(flags.is_hint());
        h.value(flags.is_hintskip());
        h.value(static_cast<bool>(flags.no_expand));
    }
}

static void hash_entity(hasher &h, const mapentity_t &entity)
{
    h.value(entity.origin);
    h.value(entity.rotation);
    for (auto &[k, v] : entity.epairs) {
        h.string(k);
        h.string(v);
    }
    h.value(entity.mapbrushes.size());
    for (auto &brush : entity.mapbrushes) {
        hash_brush(h, brush);
    }
}

static key_t entity_key(const mapentity_t &entity, int hullnum)
{
    hasher h;
    h.value(hullnum);
    hash_entity(h, entity);

    // the world also gets the brushes of func_group, func_detail... (see Brush_LoadEntity)
    if (map.is_world_entity(entity)) {
        for (size_t i = 1; i < map.entities.size(); i++) {
            const mapentity_t &source = map.entities[i];
            if (IsWorldBrushEntity(source) || IsNonRemoveWorldBrushEntity(source)) {
                hash_entity(h, source);
            }
        }
    }
    return h.key();
}

// the point entities that MarkOccupiedLeafs (outside.cc) places in the tree
static std::vector<qvec3f> occupants()
{
    std::vector<qvec3f> result;
    for (size_t i = 1; i < map.entities.size(); i++) {
        const mapentity_t &entity = map.entities[i];
        if (qv::epsilonEmpty(entity.origin, (float)QBSP_EQUAL_EPSILON)) {
            continue;
        }
        if (entity.epairs.get_int("_nofill") || entity.epairs.get_int("_lightgrid_hint")) {
            continue;
        }
        result.push_back(entity.origin);
    }
    std::ranges::sort(result);
    return result;
}

struct leaf_t
{
    bool solid;
};

static std::optional<leaf_t> locate(const entry_t &e, const qvec3d &p, int32_t empty_contents)
{
    int32_t num = e.head;
    while (num >= 0) {
        const cnode_t &n = e.nodes[num];
        double d = qv::dot(n.plane.normal, p) - n.plane.dist;
        if (std::fabs(d) < on_plane_epsilon) {
            return std::nullopt;
        }
        num = n.children[d > 0 ? 0 : 1];
    }
    return leaf_t{num != empty_contents};
}

/*
 * How the world's clipping hulls depend on the point entities (outside.cc). With the default
 * -filltype auto, hulls 1, 2... are filled from the void (FillOutside, OUTSIDE), and FillDetail
 * is skipped. The entities then only decide whether filling happens at all:
 * - no filling if an entity is in the void (a leak). The hull's void is inside hull 0's void
 *   (its brushes are the same, only bigger), so if hull 0 of this compile was filled (no leak),
 *   no entity is in this hull's void either;
 * - no filling if no entity is in empty space. The stored tree's empty leaves are empty
 *   space, so one entity in one of them is enough.
 * Otherwise the result doesn't depend on where the entities are, and the stored tree (which was
 * filled) is reused. With another -filltype, any change in the entities rebuilds the hull.
 */
static bool occupants_compatible(const entry_t &e, std::string &why)
{
    const std::vector<qvec3f> current = occupants();

    if (qbsp_options.filltype.value() != settings::filltype_t::AUTO) {
        if (current != e.occupants) {
            why = "the point entities changed (and -filltype is not auto)";
            return false;
        }
        return true;
    }

    if (!hull0_filled) {
        why = "hull 0 of this compile was not filled (a leak?)";
        return false;
    }

    const int32_t empty = qbsp_options.target_game->contents_to_native(contentflags_t::make(EWT_VISCONTENTS_EMPTY));
    for (auto &p : current) {
        if (auto leaf = locate(e, p, empty); leaf && !leaf->solid) {
            return true;
        }
    }
    why = "no point entity is in an empty leaf";
    return false;
}

// same order as ExportClipNodes (writebsp.cc): a node, its children, then its plane
static int32_t emit(const entry_t &e, int32_t num)
{
    if (num < 0) {
        return num;
    }
    const size_t nodenum = map.bsp.dclipnodes.size();
    map.bsp.dclipnodes.emplace_back();

    const int32_t child0 = emit(e, e.nodes[num].children[0]);
    const int32_t child1 = emit(e, e.nodes[num].children[1]);

    bsp2_dclipnode_t &clipnode = map.bsp.dclipnodes[nodenum];
    clipnode.planenum = ExportMapPlane(map.add_or_find_plane(e.nodes[num].plane));
    clipnode.children[0] = child0;
    clipnode.children[1] = child1;
    return static_cast<int32_t>(nodenum);
}

bool replay(mapentity_t &entity, int hullnum)
{
    if (!is_enabled) {
        return false;
    }

    const key_t key = entity_key(entity, hullnum);
    pending_entity = &entity;
    pending_hull = hullnum;
    pending_key = key;

    // QBSPSERVER_NO_HULL_REPLAY=1: never reuse a whole hull (to test the cells alone)
    static const bool no_replay = std::getenv("QBSPSERVER_NO_HULL_REPLAY") != nullptr;
    auto it = no_replay ? entries.end() : entries.find(key);
    std::string why;
    if (it == entries.end()) {
        why = "its brushes changed";
    } else if (it->second.check_occupants) {
        occupants_compatible(it->second, why);
    }
    if (!why.empty()) {
        rebuilt++;
        if (map.is_world_entity(entity)) {
            logging::print("hull {}: world rebuilt: {}\n", hullnum, why);
        }
        return false;
    }

    entry_t &e = it->second;
    e.last_used = generation;
    if (e.marks) {
        last_marks[hullnum] = e.marks;
    }
    touch_cells(hullnum, e.cell_keys);
    auto &model = map.bsp.dmodels.at(entity.outputmodelnumber.value());
    model.headnode[hullnum] = emit(e, e.head);

    reused++;
    reused_nodes += e.nodes.size();
    if (map.is_world_entity(entity)) {
        logging::print("hull {}: world reused from a previous compile ({} clipnodes)\n", hullnum, e.nodes.size());
    }
    return true;
}

static int32_t record(entry_t &e, node_t *node)
{
    if (auto *leafdata = node->get_leafdata()) {
        return qbsp_options.target_game->contents_to_native(leafdata->contents);
    }
    auto *nodedata = node->get_nodedata();
    const int32_t num = static_cast<int32_t>(e.nodes.size());
    e.nodes.emplace_back();

    const int32_t child0 = record(e, nodedata->children[0]);
    const int32_t child1 = record(e, nodedata->children[1]);

    const qbsp_plane_t &plane = map.get_plane(nodedata->planenum);
    e.nodes[num] = {qplane3d{plane.get_normal(), plane.get_dist()}, {child0, child1}};
    return num;
}

void store(mapentity_t &entity, int hullnum, node_t *headnode, bool cacheable)
{
    if (!is_enabled || pending_entity != &entity || pending_hull != hullnum) {
        return;
    }
    pending_entity = nullptr;
    if (!cacheable) {
        return;
    }

    entry_t e;
    e.head = record(e, headnode);
    e.last_used = generation;
    if (map.is_world_entity(entity) && !qbsp_options.nofill.value()) {
        e.check_occupants = true;
        e.occupants = occupants();
        e.marks = std::move(pending_marks);
        e.cell_keys = std::move(pending_cell_keys);
    }
    entries.insert_or_assign(pending_key, std::move(e));
}

// the brushes that make up the world's hulls (see Brush_LoadEntity)
template<typename F>
static void for_each_world_brush(F &&f)
{
    for (auto &brush : map.world_entity().mapbrushes) {
        f(brush);
    }
    for (size_t i = 1; i < map.entities.size(); i++) {
        mapentity_t &source = map.entities[i];
        if (IsWorldBrushEntity(source) || IsNonRemoveWorldBrushEntity(source)) {
            for (auto &brush : source.mapbrushes) {
                f(brush);
            }
        }
    }
}

static key_t brush_key(const mapbrush_t &brush)
{
    hasher h;
    hash_brush(h, brush);
    return h.key();
}

bool apply_visibility(int hullnum)
{
    if (!is_enabled) {
        return false;
    }
    auto it = last_marks.find(hullnum);
    if (it == last_marks.end() || !it->second || it->second->empty()) {
        return false;
    }
    const marks_t &marks = *it->second;

    size_t known = 0, total = 0;
    for_each_world_brush([&](mapbrush_t &brush) {
        total++;
        auto m = marks.find(brush_key(brush));
        if (m == marks.end() || m->second.size() != brush.faces.size()) {
            return; // a new or changed brush keeps LoadBrush's marks: every side with a winding is visible
        }
        known++;
        for (size_t i = 0; i < brush.faces.size(); i++) {
            brush.faces[i].visible = m->second[i];
        }
    });

    logging::print("hull {}: world built in one pass, with the side visibility of a previous compile "
                   "({} of {} brushes known)\n",
        hullnum, known, total);
    return true;
}

void remember_visibility(int hullnum)
{
    if (!is_enabled) {
        return;
    }
    auto marks = std::make_shared<marks_t>();
    for_each_world_brush([&](mapbrush_t &brush) {
        std::vector<bool> faces(brush.faces.size());
        for (size_t i = 0; i < brush.faces.size(); i++) {
            faces[i] = brush.faces[i].visible;
        }
        marks->insert_or_assign(brush_key(brush), std::move(faces));
    });
    last_marks[hullnum] = marks;
    pending_marks = std::move(marks);
}

// ---- cells -------------------------------------------------------------------------------

struct cell_node_t
{
    qplane3d plane; // positive (even) plane
    int32_t children[2]; // >= 0: index in nodes, < 0: leaf -1 - index in leaves
};

struct cell_leaf_t
{
    contentflags_t contents;
    std::vector<key_t> original_brushes; // brush keys, mapped back to this compile's brushes
};

struct cell_t
{
    std::vector<cell_node_t> nodes;
    std::vector<cell_leaf_t> leaves;
    int32_t head = 0;
    uint64_t last_used = 0;
};

using cell_map_t = std::unordered_map<key_t, cell_t, key_hash>;
// per hull
static std::unordered_map<int, cell_map_t> cells;

// the BrushBSP in progress (between cell_hull_begin and cell_hull_end)
struct cell_build_t
{
    int hullnum = -1;
    bool prepared = false;
    std::map<std::pair<int, int64_t>, size_t> grid_planes; // (axis, line) -> planenum
    std::unordered_map<const bspbrush_t *, key_t> key_of; // original brush -> key
    std::unordered_map<key_t, bspbrush_t *, key_hash> brush_of; // key -> original brush
    std::mutex mutex;
    std::vector<std::pair<node_t *, key_t>> built; // cells built in this BrushBSP, to store
    std::vector<key_t> used; // every cell of this tree, grafted or built
    std::atomic<size_t> grafted = 0, total = 0;
    cell_map_t *stored = nullptr; // this hull's stored cells (looked up in parallel: never inserted into while building)
};
static std::unique_ptr<cell_build_t> build;

// QBSPSERVER_DEBUG_CELLS=1: log every cell that is rebuilt
static const bool debug_cells = std::getenv("QBSPSERVER_DEBUG_CELLS") != nullptr;

static void touch_cells(int hullnum, const std::vector<key_t> &keys)
{
    auto it = cells.find(hullnum);
    if (it == cells.end()) {
        return;
    }
    for (auto &k : keys) {
        if (auto c = it->second.find(k); c != it->second.end()) {
            c->second.last_used = generation;
        }
    }
}

static void prune_cells(uint64_t oldest)
{
    for (auto &[hull, stored] : cells) {
        std::erase_if(stored, [oldest](const auto &c) { return c.second.last_used < oldest; });
    }
}

void cell_hull_begin(int hullnum)
{
    build.reset();
    if (is_enabled && grid_mode != grid_mode_t::off_for_limits) {
        build = std::make_unique<cell_build_t>();
        build->hullnum = hullnum;
    }
}

void cell_hull_end()
{
    build.reset();
}

bool cells_active()
{
    return build && build->prepared;
}

/*
 * Plane values in keys are rounded. qbsp shares planes across the map and merges planes closer
 * than NORMAL_EPSILON / DIST_EPSILON, so the exact value a brush side gets depends on which
 * similar plane was created first, somewhere else in the map: an edit anywhere could change the
 * last digits (~1e-12) of planes everywhere. Rounding far below the epsilons hides that; two
 * planes that round alike are ones qbsp itself would almost always merge.
 */
static int64_t quantize(double v, double step)
{
    return static_cast<int64_t>(std::llround(v / step));
}

static std::array<int64_t, 4> plane_key(const qbsp_plane_t &p)
{
    return {quantize(p.get_normal()[0], NORMAL_EPSILON * 0.1), quantize(p.get_normal()[1], NORMAL_EPSILON * 0.1),
        quantize(p.get_normal()[2], NORMAL_EPSILON * 0.1), quantize(p.get_dist(), DIST_EPSILON * 0.1)};
}

// a brush's shape: its contents and its planes, in any order
static key_t shape_key(const bspbrush_t &brush)
{
    std::vector<std::array<int64_t, 4>> planes;
    planes.reserve(brush.sides.size());
    for (auto &side : brush.sides) {
        planes.push_back(plane_key(map.get_plane(side.planenum)));
    }
    std::ranges::sort(planes);
    hasher h;
    h.value(brush.contents.flags);
    h.value(planes.size());
    for (auto &p : planes) {
        h.value(p);
    }
    return h.key();
}

void cells_prepare(const aabb3d &tree_bounds, const std::vector<std::shared_ptr<bspbrush_t>> &brushes)
{
    if (!build) {
        return;
    }
    // every grid line the tree can be split on (the root is the bounds grown by SIDESPACE)
    for (int axis = 0; axis < 3; axis++) {
        const int64_t first = static_cast<int64_t>(std::floor(tree_bounds.mins()[axis] / cell_size)) - 1;
        const int64_t last = static_cast<int64_t>(std::ceil(tree_bounds.maxs()[axis] / cell_size)) + 1;
        for (int64_t line = first; line <= last; line++) {
            qvec3d normal{};
            normal[axis] = 1;
            build->grid_planes[{axis, line}] = map.add_or_find_plane({normal, line * cell_size});
        }
    }
    for (auto &brush : brushes) {
        key_t k = shape_key(*brush);
        build->key_of[brush.get()] = k;
        build->brush_of.try_emplace(k, brush.get());
    }
    build->stored = &cells[build->hullnum];
    build->prepared = true;
}

std::optional<size_t> grid_plane(const aabb3d &bounds)
{
    constexpr double margin = 0.5;
    int best_axis = -1;
    int64_t best_line = 0;
    double best_extent = 0;
    for (int axis = 0; axis < 3; axis++) {
        const double lo = bounds.mins()[axis], hi = bounds.maxs()[axis];
        const int64_t first = static_cast<int64_t>(std::floor((lo + margin) / cell_size)) + 1;
        const int64_t last = static_cast<int64_t>(std::ceil((hi - margin) / cell_size)) - 1;
        if (first > last || hi - lo <= best_extent) {
            continue;
        }
        // the grid line closest to the middle
        const int64_t mid = static_cast<int64_t>(std::llround((lo + hi) / 2 / cell_size));
        best_axis = axis;
        best_line = std::clamp(mid, first, last);
        best_extent = hi - lo;
    }
    if (best_axis < 0) {
        return std::nullopt;
    }
    auto it = build->grid_planes.find({best_axis, best_line});
    if (it == build->grid_planes.end()) {
        return std::nullopt;
    }
    return it->second;
}

static key_t cell_key(const aabb3d &bounds, const std::vector<std::shared_ptr<bspbrush_t>> &brushes)
{
    std::vector<key_t> fragments;
    fragments.reserve(brushes.size());
    for (auto &brush : brushes) {
        fragments.push_back(shape_key(*brush));
    }
    std::ranges::sort(fragments, [](const key_t &a, const key_t &b) { return a.a != b.a ? a.a < b.a : a.b < b.b; });
    hasher h;
    h.value(build->hullnum);
    for (int i = 0; i < 3; i++) {
        h.value(quantize(bounds.mins()[i], DIST_EPSILON * 0.1));
        h.value(quantize(bounds.maxs()[i], DIST_EPSILON * 0.1));
    }
    for (auto &f : fragments) {
        h.value(f);
    }
    return h.key();
}

// builds node's subtree from a stored cell; planes must all exist already (no plane is created
// while BuildTree_r runs in parallel)
static bool graft(tree_t &tree, node_t *node, const cell_t &cell)
{
    std::vector<size_t> planenums(cell.nodes.size());
    for (size_t i = 0; i < cell.nodes.size(); i++) {
        auto found = map.find_plane_nonfatal(cell.nodes[i].plane);
        if (!found || (*found & 1)) {
            return false;
        }
        planenums[i] = *found;
    }

    auto make = [&](auto &self, node_t *n, int32_t num) -> void {
        if (num < 0) {
            const cell_leaf_t &leaf = cell.leaves[-1 - num];
            auto *leafdata = n->make_leaf();
            leafdata->contents = leaf.contents;
            for (auto &k : leaf.original_brushes) {
                if (auto it = build->brush_of.find(k); it != build->brush_of.end()) {
                    leafdata->original_brushes.push_back(it->second);
                }
            }
            n->volume.reset();
            return;
        }
        auto *nodedata = n->get_nodedata();
        nodedata->planenum = planenums[num];
        const qbsp_plane_t &plane = map.get_plane(planenums[num]);
        for (int i = 0; i < 2; i++) {
            node_t *child = nodedata->children[i] = tree.create_node();
            child->parent = n;
            child->bounds = n->bounds;
        }
        // same bounds as BuildTree_r gives the children
        for (int i = 0; i < 3; i++) {
            if (plane.get_normal()[i] == 1.0) {
                nodedata->children[0]->bounds[0][i] = plane.get_dist();
                nodedata->children[1]->bounds[1][i] = plane.get_dist();
                break;
            }
        }
        n->volume.reset();
        self(self, nodedata->children[0], cell.nodes[num].children[0]);
        self(self, nodedata->children[1], cell.nodes[num].children[1]);
    };
    make(make, node, cell.head);
    return true;
}

bool cell_from_cache(tree_t &tree, node_t *node, const std::vector<std::shared_ptr<bspbrush_t>> &brushes)
{
    build->total++;
    const key_t key = cell_key(node->bounds, brushes);

    if (auto it = build->stored->find(key); it != build->stored->end() && graft(tree, node, it->second)) {
        std::lock_guard lock(build->mutex);
        it->second.last_used = generation;
        build->used.push_back(key);
        build->grafted++;
        return true;
    }

    std::lock_guard lock(build->mutex);
    build->built.emplace_back(node, key);
    build->used.push_back(key);
    if (debug_cells) {
        logging::print("hull {}: cell {} .. {} rebuilt ({} brushes)\n", build->hullnum, node->bounds.mins(),
            node->bounds.maxs(), brushes.size());
    }
    return false;
}

static int32_t record_cell(cell_t &cell, node_t *node)
{
    if (auto *leafdata = node->get_leafdata()) {
        cell_leaf_t leaf{leafdata->contents, {}};
        for (auto *brush : leafdata->original_brushes) {
            if (auto it = build->key_of.find(brush); it != build->key_of.end()) {
                leaf.original_brushes.push_back(it->second);
            }
        }
        cell.leaves.push_back(std::move(leaf));
        return -static_cast<int32_t>(cell.leaves.size());
    }
    auto *nodedata = node->get_nodedata();
    const int32_t num = static_cast<int32_t>(cell.nodes.size());
    cell.nodes.emplace_back();
    const int32_t child0 = record_cell(cell, nodedata->children[0]);
    const int32_t child1 = record_cell(cell, nodedata->children[1]);
    const qbsp_plane_t &plane = map.get_plane(nodedata->planenum);
    cell.nodes[num] = {qplane3d{plane.get_normal(), plane.get_dist()}, {child0, child1}};
    return num;
}

void cells_finish()
{
    if (!cells_active()) {
        return;
    }
    cell_map_t &map_cells = *build->stored;
    for (auto &[node, key] : build->built) {
        cell_t cell;
        cell.head = record_cell(cell, node);
        cell.last_used = generation;
        map_cells.insert_or_assign(key, std::move(cell));
    }
    logging::print("hull {}: world tree in {} cells of {} units, {} from a previous compile\n", build->hullnum,
        build->total.load(), cell_size, build->grafted.load());
    pending_cell_keys = std::move(build->used);
    build->prepared = false;
}

// ---- hull 0 of the world -----------------------------------------------------------------

struct h0_node_t
{
    qplane3d plane; // positive (even) plane, the exact values qbsp used
    int32_t children[2]; // >= 0: node, < 0: leaf -1 - index
    bool detail_separator;
};

// a leaf of the final tree (to rebuild it)
struct h0_leaf_t
{
    contentflags_t contents; // before filling
    std::vector<int32_t> originals; // indices in the brush list given to BrushBSP
};

// a leaf of the first pass's tree (to find which areas the point entities are in)
struct h0_first_leaf_t
{
    bool seals; // LeafSealsMap
    int32_t outside_area; // connected empty space as FillOutside floods it (-1: seals)
    int32_t detail_area; // the same for FillDetail, where detail also seals (-1: seals)
};

struct hull0_t
{
    key_t key;
    std::vector<key_t> shapes; // the brushes given to BrushBSP, in order
    // the final tree
    aabb3d tree_bounds, head_bounds;
    std::vector<h0_node_t> nodes;
    std::vector<h0_leaf_t> leaves;
    int32_t head = 0;
    // the first pass's tree, only to place the point entities
    std::vector<h0_node_t> first_nodes;
    std::vector<h0_first_leaf_t> first_leaves;
    int32_t first_head = 0;
    // the areas holding a point entity: they decide the first pass's filling, so its side marks
    std::vector<int32_t> occupied_outside, occupied_detail; // sorted, unique
    // brush entities: the side marks of their first pass (FillBrushEntity), which their faces use
    bool bmodel = false;
    std::vector<std::vector<bool>> marks; // per map brush of the entity, per face
    uint64_t last_used = 0;
};

// by key; a few trees can share a key (same brushes, the entities in other areas)
static std::unordered_multimap<key_t, hull0_t, key_hash> hull0_trees;
// the first pass's tree of the compile in progress, until hull0_store()
static std::unique_ptr<hull0_t> hull0_pending;

static size_t hull0_bmodels_reused = 0;

static void prune_hull0(uint64_t oldest)
{
    std::erase_if(hull0_trees, [oldest](const auto &t) { return t.second.last_used < oldest; });
}

static std::vector<key_t> brush_shapes(const std::vector<std::shared_ptr<bspbrush_t>> &brushes)
{
    std::vector<key_t> shapes;
    shapes.reserve(brushes.size());
    for (auto &b : brushes) {
        shapes.push_back(shape_key(*b));
    }
    return shapes;
}

static key_t hull0_key(mapentity_t &entity)
{
    // brushes, contents, texture flags, the entity keys (the options are the same for a server's life)
    return entity_key(entity, 0);
}

// records a tree's nodes; `leaf` records a leaf and returns its index
template<typename F>
static int32_t record_tree(std::vector<h0_node_t> &nodes, const node_t *node, F &&leaf)
{
    if (node->get_leafdata()) {
        return -1 - leaf(node);
    }
    auto *nodedata = node->get_nodedata();
    const int32_t num = static_cast<int32_t>(nodes.size());
    nodes.emplace_back();
    const int32_t c0 = record_tree(nodes, nodedata->children[0], leaf);
    const int32_t c1 = record_tree(nodes, nodedata->children[1], leaf);
    const qbsp_plane_t &plane = map.get_plane(nodedata->planenum);
    nodes[num] = {qplane3d{plane.get_normal(), plane.get_dist()}, {c0, c1}, nodedata->detail_separator};
    return num;
}

/*
 * Which first-pass leaf MarkOccupiedLeafs (outside.cc) puts a point in: the same arithmetic as
 * PointInLeaf on the same plane values, so the same answer. On a node plane both sides are
 * looked at, and for hull 0 the sealing one wins.
 */
static int32_t first_leaf_of(const hull0_t &t, int32_t num, const qvec3d &p)
{
    if (num < 0) {
        return -1 - num;
    }
    const h0_node_t &n = t.first_nodes[num];
    const double dist = qv::dot(n.plane.normal, p) - n.plane.dist;
    if (dist > 0) {
        return first_leaf_of(t, n.children[0], p);
    }
    if (dist < 0) {
        return first_leaf_of(t, n.children[1], p);
    }
    const int32_t front = first_leaf_of(t, n.children[0], p);
    const int32_t back = first_leaf_of(t, n.children[1], p);
    constexpr bool prefer_sealing = true; // hull 0
    return prefer_sealing == t.first_leaves[front].seals ? front : back;
}

static void occupied_areas(const hull0_t &t, std::vector<int32_t> &outside, std::vector<int32_t> &detail)
{
    outside.clear();
    detail.clear();
    for (auto &origin : occupants()) {
        const h0_first_leaf_t &leaf = t.first_leaves[first_leaf_of(t, t.first_head, qvec3d(origin))];
        if (leaf.seals) {
            continue; // MarkOccupiedLeafs skips it
        }
        outside.push_back(leaf.outside_area);
        if (leaf.detail_area >= 0) {
            detail.push_back(leaf.detail_area);
        }
    }
    for (auto *v : {&outside, &detail}) {
        std::ranges::sort(*v);
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
}

void hull0_store_first(tree_t &tree)
{
    hull0_pending.reset();
    if (!is_enabled) {
        return;
    }
    auto t = std::make_unique<hull0_t>();

    std::unordered_map<const node_t *, int32_t> leaf_index;
    std::vector<const node_t *> leaf_nodes;
    t->first_head = record_tree(t->first_nodes, tree.headnode, [&](const node_t *node) {
        const contentflags_t &c = node->get_leafdata()->contents;
        const int32_t index = static_cast<int32_t>(t->first_leaves.size());
        t->first_leaves.push_back({c.seals_map(), -1, -1});
        leaf_index[node] = index;
        leaf_nodes.push_back(node);
        return index;
    });

    // label the connected areas of empty space through the portals, the way FillOutside
    // (LeafSealsMap) and FillDetail (LeafSealsForDetailFill) flood them
    auto label = [&](auto seals, int32_t h0_first_leaf_t::*area) {
        int32_t next = 0;
        for (size_t i = 0; i < leaf_nodes.size(); i++) {
            if (t->first_leaves[i].*area >= 0 || seals(leaf_nodes[i]->get_leafdata()->contents)) {
                continue;
            }
            std::vector<const node_t *> stack{leaf_nodes[i]};
            t->first_leaves[i].*area = next;
            while (!stack.empty()) {
                const node_t *n = stack.back();
                stack.pop_back();
                int side;
                for (const portal_t *p = n->portals; p; p = p->next[!side]) {
                    side = (p->nodes[0] == n);
                    if (!p->onnode) {
                        continue; // to outside_node
                    }
                    const node_t *other = p->nodes[side];
                    auto it = leaf_index.find(other);
                    if (it == leaf_index.end() || t->first_leaves[it->second].*area >= 0 ||
                        seals(other->get_leafdata()->contents)) {
                        continue;
                    }
                    t->first_leaves[it->second].*area = next;
                    stack.push_back(other);
                }
            }
            next++;
        }
    };
    label([](const contentflags_t &c) { return c.seals_map(); }, &h0_first_leaf_t::outside_area);
    label([](const contentflags_t &c) { return c.is_any_solid() || c.is_sky(); }, &h0_first_leaf_t::detail_area);

    occupied_areas(*t, t->occupied_outside, t->occupied_detail);
    hull0_pending = std::move(t);
}

void hull0_store(tree_t &tree, mapentity_t &entity, const std::vector<std::shared_ptr<bspbrush_t>> &brushes)
{
    const bool bmodel = !map.is_world_entity(entity);
    if (!is_enabled || (!bmodel && !hull0_pending)) {
        return;
    }
    std::unique_ptr<hull0_t> t = bmodel ? std::make_unique<hull0_t>() : std::move(hull0_pending);
    t->bmodel = bmodel;
    if (bmodel) {
        for (auto &brush : entity.mapbrushes) {
            std::vector<bool> faces(brush.faces.size());
            for (size_t i = 0; i < brush.faces.size(); i++) {
                faces[i] = brush.faces[i].visible;
            }
            t->marks.push_back(std::move(faces));
        }
    }
    t->key = hull0_key(entity);
    t->shapes = brush_shapes(brushes);
    t->tree_bounds = tree.bounds;
    t->head_bounds = tree.headnode->bounds;

    std::unordered_map<const bspbrush_t *, int32_t> index_of;
    for (size_t i = 0; i < brushes.size(); i++) {
        index_of[brushes[i].get()] = static_cast<int32_t>(i);
    }
    t->head = record_tree(t->nodes, tree.headnode, [&](const node_t *node) {
        auto *leafdata = node->get_leafdata();
        h0_leaf_t leaf{leafdata->contents, {}};
        for (auto *b : leafdata->original_brushes) {
            if (auto it = index_of.find(b); it != index_of.end()) {
                leaf.originals.push_back(it->second);
            }
        }
        t->leaves.push_back(std::move(leaf));
        return static_cast<int32_t>(t->leaves.size() - 1);
    });

    t->last_used = generation;
    auto [first, last] = hull0_trees.equal_range(t->key);
    for (auto it = first; it != last;) {
        it = it->second.shapes == t->shapes ? hull0_trees.erase(it) : std::next(it);
    }
    hull0_trees.emplace(t->key, std::move(*t));
}

bool hull0_reuse(tree_t &tree, mapentity_t &entity, const std::vector<std::shared_ptr<bspbrush_t>> &brushes)
{
    if (!is_enabled) {
        return false;
    }
    const bool bmodel = !map.is_world_entity(entity);
    const key_t key = hull0_key(entity);
    auto [first, last] = hull0_trees.equal_range(key);
    if (first == last) {
        return false;
    }
    const std::vector<key_t> shapes = brush_shapes(brushes);
    for (auto it = first; it != last; ++it) {
        hull0_t &t = it->second;
        if (t.shapes != shapes || t.bmodel != bmodel) {
            continue;
        }
        if (!bmodel) {
            std::vector<int32_t> outside, detail;
            occupied_areas(t, outside, detail);
            if (outside != t.occupied_outside || detail != t.occupied_detail) {
                logging::print("hull 0: world rebuilt: the point entities are in other areas\n");
                return false;
            }
        } else if (t.marks.size() != entity.mapbrushes.size()) {
            continue;
        }

        // every plane must exist already
        std::vector<size_t> planenums(t.nodes.size());
        for (size_t i = 0; i < t.nodes.size(); i++) {
            auto found = map.find_plane_nonfatal(t.nodes[i].plane);
            if (!found || (*found & 1)) {
                return false;
            }
            planenums[i] = *found;
        }

        tree.bounds = t.tree_bounds;
        tree.headnode = tree.create_node();
        tree.headnode->bounds = t.head_bounds;
        auto make = [&](auto &self, node_t *n, int32_t num) -> void {
            if (num < 0) {
                const h0_leaf_t &leaf = t.leaves[-1 - num];
                auto *leafdata = n->make_leaf();
                leafdata->contents = leaf.contents;
                for (int32_t i : leaf.originals) {
                    leafdata->original_brushes.push_back(brushes[i].get());
                }
                return;
            }
            auto *nodedata = n->get_nodedata();
            nodedata->planenum = planenums[num];
            nodedata->detail_separator = t.nodes[num].detail_separator;
            const qbsp_plane_t &plane = map.get_plane(planenums[num]);
            for (int i = 0; i < 2; i++) {
                node_t *child = nodedata->children[i] = tree.create_node();
                child->parent = n;
                child->bounds = n->bounds;
            }
            // same bounds as BuildTree_r gives the children
            for (int i = 0; i < 3; i++) {
                if (plane.get_normal()[i] == 1.0) {
                    nodedata->children[0]->bounds[0][i] = plane.get_dist();
                    nodedata->children[1]->bounds[1][i] = plane.get_dist();
                    break;
                }
            }
            self(self, nodedata->children[0], t.nodes[num].children[0]);
            self(self, nodedata->children[1], t.nodes[num].children[1]);
        };
        make(make, tree.headnode, t.head);
        t.last_used = generation;
        if (bmodel) {
            // the side marks its first pass left, which its faces depend on
            for (size_t b = 0; b < entity.mapbrushes.size(); b++) {
                auto &faces = entity.mapbrushes[b].faces;
                for (size_t i = 0; i < faces.size() && i < t.marks[b].size(); i++) {
                    faces[i].visible = t.marks[b][i];
                }
            }
            hull0_bmodels_reused++;
        } else {
            logging::print("hull 0: world tree reused from a previous compile ({} nodes, {} leafs)\n",
                t.nodes.size(), t.leaves.size());
        }
        return true;
    }
    return false;
}

bool chop_world_hulls()
{
    static const bool force_chop = std::getenv("QBSPSERVER_CHOP_HULLS") != nullptr;
    return !is_enabled || force_chop || grid_mode == grid_mode_t::off_for_limits;
}

std::optional<size_t> fake_clipnode_limit()
{
    static const char *env = std::getenv("QBSPSERVER_FAKE_CLIPNODE_LIMIT");
    if (!env || !is_enabled) {
        return std::nullopt;
    }
    return static_cast<size_t>(std::strtoull(env, nullptr, 10));
}

bool grid_active()
{
    return is_enabled && grid_mode == grid_mode_t::on;
}

void check_limits(bool fits, size_t clipnodes)
{
    if (!is_enabled) {
        return;
    }
    if (auto fake = fake_clipnode_limit(); fake && clipnodes > *fake) {
        fits = false;
    }
    if (fits) {
        return;
    }
    if (grid_mode == grid_mode_t::on) {
        throw limits_exceeded();
    }
    if (grid_mode == grid_mode_t::off_for_limits) {
        // plain qbsp would not fit either: the grid isn't the reason, put it back
        logging::print("hullcache: the map doesn't fit the format without the cell grid either; grid back on\n");
        grid_mode = grid_mode_t::on_extended;
    }
}

void hulls_like_qbsp()
{
    grid_mode = grid_mode_t::off_for_limits;
    // the stored clipping hulls were built with the grid: drop them
    entries.clear();
    cells.clear();
    last_marks.clear();
}

void print_summary()
{
    if (!is_enabled) {
        return;
    }
    logging::print("hullcache: {} clipping hulls reused ({} clipnodes), {} rebuilt, {} stored; "
                   "{} brush entity hull 0 trees reused\n",
        reused, reused_nodes, rebuilt, entries.size(), hull0_bmodels_reused);
    hull0_bmodels_reused = 0;
}
} // namespace hullcache

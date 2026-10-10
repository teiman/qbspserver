/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

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

#pragma once

#include <common/bspfile.hh>
#include <qbsp/brush.hh>
#include <qbsp/qbsp.hh>

#include <tbb/concurrent_vector.h>

#include <atomic>
#include <memory>

struct side_t;
struct tree_t;

struct portal_t
{
    qbsp_plane_t plane;
    // nullptr = portal to the outside of the world (one of six sides of a box)
    node_t *onnode;
    // .front/.back side of planenum
    twosided<node_t *> nodes;
    // front = next portal in nodes[0]'s list of portals
    twosided<portal_t *> next;
    winding_t winding;

    // front = the brush side visible on nodes.front - it could come from a brush in nodes.back
    // nullptr = non-visible
    twosided<side_t *> sides;

    // false if ->side hasn't been checked
    bool sidefound;
};

// helper used for building the portals in paralllel.
struct buildportal_t
{
    qbsp_plane_t plane;
    // nullptr = portal to the outside of the world (one of six sides of a box)
    node_t *onnode = nullptr;
    // .front/.back side of planenum
    twosided<node_t *> nodes = {nullptr, nullptr};
    winding_t winding;
    // the next piece in its buildportal_list_t
    buildportal_t *next = nullptr;
};

/*
 * The pieces of portal while a tree is portalized live in a pool (for one MakeTreePortals /
 * WritePortalFile), and lists of them are chains through `next`: splitting and joining lists
 * (order kept, like std::list's splice) allocates nothing. A std::list node per piece was ~5% of
 * qbsp's time in allocations.
 */
struct buildportal_pool_t
{
    tbb::concurrent_vector<buildportal_t> pieces; // (thread-safe growth; elements never move)

    buildportal_t *make() { return &*pieces.emplace_back(); }
};

struct buildportal_list_t
{
    buildportal_t *head = nullptr;
    buildportal_t *tail = nullptr;

    buildportal_list_t() = default;
    buildportal_list_t(const buildportal_list_t &) = delete;
    buildportal_list_t &operator=(const buildportal_list_t &) = delete;
    buildportal_list_t(buildportal_list_t &&other) noexcept : head(other.head), tail(other.tail)
    {
        other.head = other.tail = nullptr;
    }
    buildportal_list_t &operator=(buildportal_list_t &&other) noexcept
    {
        head = other.head;
        tail = other.tail;
        other.head = other.tail = nullptr;
        return *this;
    }

    bool empty() const { return head == nullptr; }

    void push_back(buildportal_t *p)
    {
        p->next = nullptr;
        if (tail) {
            tail->next = p;
        } else {
            head = p;
        }
        tail = p;
    }

    // moves all of `other` to the end of this list
    void splice(buildportal_list_t &other)
    {
        if (!other.head) {
            return;
        }
        if (tail) {
            tail->next = other.head;
        } else {
            head = other.head;
        }
        tail = other.tail;
        other.head = other.tail = nullptr;
    }

    // (reads the next piece before the current one is visited: the loop may move the current piece
    // to another list)
    struct iterator
    {
        buildportal_t *cur, *nxt;
        buildportal_t &operator*() const { return *cur; }
        iterator &operator++()
        {
            cur = nxt;
            nxt = cur ? cur->next : nullptr;
            return *this;
        }
        bool operator!=(const iterator &other) const { return cur != other.cur; }
    };
    iterator begin() const { return {head, head ? head->next : nullptr}; }
    iterator end() const { return {nullptr, nullptr}; }
};

struct portalstats_t : logging::stat_tracker_t
{
    stat &c_tinyportals = register_stat("tiny portals");
};

contentflags_t ClusterContents(const node_t *node);
bool Portal_VisFlood(const portal_t *p);
bool Portal_EntityFlood(const portal_t *p, int32_t s);
enum class portaltype_t
{
    NONE,
    TREE,
    VIS
};
buildportal_list_t MakeTreePortals_r(node_t *node, portaltype_t type, buildportal_list_t boundary_portals,
    buildportal_pool_t &pool, portalstats_t &stats, logging::percent_clock &clock);
void MakeTreePortals(tree_t &tree);
buildportal_list_t MakeHeadnodePortals(tree_t &tree, buildportal_pool_t &pool);
void MakePortalsFromBuildportals(tree_t &tree, buildportal_list_t &buildportals);
void EmitAreaPortals(tree_t &tree);
void MarkVisibleSides(tree_t &tree, bspbrush_t::container &brushes);

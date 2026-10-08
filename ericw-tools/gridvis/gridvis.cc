/*  gridvis: screen-footprint visibility for top-down Quake maps.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#include <gridvis/gridvis.hh>

#include <common/bspfile.hh>
#include <common/bsputils.hh>
#include <common/cmdlib.hh>
#include <common/log.hh>
#include <common/parallel.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace settings
{
setting_group gridvis_footprint_group{"Footprint", 100, expected_source::commandline};
setting_group gridvis_output_group{"Output", 200, expected_source::commandline};

void gridvis_settings::initialize(int argc, const char **argv)
{
    try {
        common_settings::initialize(argc - 1, argv + 1);

        if (remainder.size() != 1) {
            print_help(true);
        }

        sourceMap = DefaultExtension(remainder[0], "bsp");
    } catch (parse_exception &ex) {
        print_help(false);
        logging::print("ERROR OCCURRED WHEN TRYING TO PARSE ARGUMENTS:\n");
        logging::print(ex.what());
        logging::print("\n\n");
        throw settings::quit_after_help_exception();
    }
}
} // namespace settings

settings::gridvis_settings gridvis_options;

namespace
{
// XY bounds of a vis leaf
struct box2_t
{
    double mins[2];
    double maxs[2];
};

// Q1 native contents of the leafs that get no PVS (like vis, which leaves them out of the .prt)
constexpr int32_t Q1_CONTENTS_SOLID = -2;
constexpr int32_t Q1_CONTENTS_SKY = -6;

// distance between two XY boxes (0 when they overlap)
double BoxDistance(const box2_t &a, const box2_t &b)
{
    double d[2];
    for (int i = 0; i < 2; i++) {
        d[i] = std::max({0.0, b.mins[i] - a.maxs[i], a.mins[i] - b.maxs[i]});
    }
    return std::sqrt(d[0] * d[0] + d[1] * d[1]);
}

bool BoxesOverlap(const box2_t &a, const box2_t &b)
{
    for (int i = 0; i < 2; i++) {
        if (b.mins[i] > a.maxs[i] || b.maxs[i] < a.mins[i])
            return false;
    }
    return true;
}

void PrintLeafSizeHistogram(const std::vector<box2_t> &boxes, const std::vector<bool> &solid)
{
    constexpr std::array<double, 7> limits{128, 256, 512, 1024, 2048, 4096, 8192};
    std::array<int, limits.size() + 1> counts{};

    for (size_t i = 0; i < boxes.size(); i++) {
        if (solid[i])
            continue;
        const double size = std::max(boxes[i].maxs[0] - boxes[i].mins[0], boxes[i].maxs[1] - boxes[i].mins[1]);
        size_t bucket = 0;
        while (bucket < limits.size() && size > limits[bucket])
            bucket++;
        counts[bucket]++;
    }

    logging::print("leaf size histogram (largest XY side):\n");
    for (size_t i = 0; i < counts.size(); i++) {
        if (i < limits.size())
            logging::print("  <= {:5}: {:7}\n", limits[i], counts[i]);
        else
            logging::print("   > {:5}: {:7}\n", limits.back(), counts[i]);
    }
}
} // namespace

int gridvis_main(int argc, const char **argv)
{
    gridvis_options.preinitialize(argc, argv);
    gridvis_options.initialize(argc, argv);
    gridvis_options.postinitialize(argc, argv);

    gridvis_options.sourceMap.replace_extension("bsp");

    logging::init(fs::path(gridvis_options.sourceMap)
                      .replace_filename(gridvis_options.sourceMap.stem().string() + "-gridvis")
                      .replace_extension("log"),
        gridvis_options);

    gridvis_options.print_summary();

    const auto starttime = I_FloatTime();

    bspdata_t bspdata;
    LoadBSPFile(gridvis_options.sourceMap, &bspdata);

    const bspversion_t *loadversion = bspdata.version;
    ConvertBSPFormat(&bspdata, &bspver_generic);

    mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

    if (bsp.loadversion->game->has_cluster_support) {
        FError("gridvis only supports Quake 1 style BSPs (BSP29, BSP2, Hexen 2)");
    }
    if (bsp.dmodels.empty()) {
        FError("BSP has no models");
    }

    // vis leafs are dleafs[1 .. visleafs]; leaf 0 is the shared solid leaf
    const int numleafs = bsp.dmodels[0].visleafs;
    const size_t rowbytes = (numleafs + 7) >> 3;

    std::vector<box2_t> boxes(numleafs);
    std::vector<bool> solid(numleafs); // solid or sky: no PVS of their own, never visible
    for (int i = 0; i < numleafs; i++) {
        const mleaf_t &leaf = bsp.dleafs[i + 1];
        boxes[i] = {{leaf.mins[0], leaf.mins[1]}, {leaf.maxs[0], leaf.maxs[1]}};
        solid[i] = leaf.contents == Q1_CONTENTS_SOLID || leaf.contents == Q1_CONTENTS_SKY;
    }

    const bool circular = gridvis_options.radius.value() > 0;
    const double margin = gridvis_options.margin.value();
    const double offset[2] = {gridvis_options.offsetx.value(), gridvis_options.offsety.value()};
    const double halfext[2] = {gridvis_options.rectx.value() / 2 + margin, gridvis_options.recty.value() / 2 + margin};
    const double reach = gridvis_options.radius.value() + margin;

    logging::print("{:7} vis leafs\n", numleafs);
    if (circular) {
        logging::print("footprint: radius {} + margin {}, offset ({}, {})\n", gridvis_options.radius.value(), margin,
            offset[0], offset[1]);
    } else {
        logging::print("footprint: {} x {} + margin {}, offset ({}, {})\n", gridvis_options.rectx.value(),
            gridvis_options.recty.value(), margin, offset[0], offset[1]);
    }

    logging::header("GridVis");

    // one compressed row per leaf, built in parallel
    std::vector<std::vector<uint8_t>> rows(numleafs);
    std::vector<int> numvisible(numleafs);

    logging::parallel_for(0, numleafs, [&](int a) {
        if (solid[a])
            return;

        // the PVS point can be anywhere inside leaf A
        box2_t from = boxes[a];
        for (int k = 0; k < 2; k++) {
            from.mins[k] += offset[k];
            from.maxs[k] += offset[k];
        }
        if (!circular) {
            for (int k = 0; k < 2; k++) {
                from.mins[k] -= halfext[k];
                from.maxs[k] += halfext[k];
            }
        }

        std::vector<uint8_t> row(rowbytes, 0);
        int count = 0;
        for (int b = 0; b < numleafs; b++) {
            if (solid[b])
                continue;
            const bool visible =
                b == a || (circular ? BoxDistance(from, boxes[b]) <= reach : BoxesOverlap(from, boxes[b]));
            if (visible) {
                row[b >> 3] |= 1 << (b & 7);
                count++;
            }
        }

        CompressRow(row.data(), rowbytes, std::back_inserter(rows[a]));
        numvisible[a] = count;
    });

    // write the rows, sharing identical ones
    std::vector<uint8_t> vismap;
    std::unordered_map<std::string, int32_t> unique_rows;
    int64_t totalvis = 0;
    int maxvis = 0;
    int nonsolid = 0;

    bsp.dleafs[0].visofs = -1;
    for (int i = 0; i < numleafs; i++) {
        mleaf_t &leaf = bsp.dleafs[i + 1];
        if (solid[i]) {
            leaf.visofs = -1;
            continue;
        }
        nonsolid++;
        totalvis += numvisible[i];
        maxvis = std::max(maxvis, numvisible[i]);

        std::string key(rows[i].begin(), rows[i].end());
        auto [it, inserted] = unique_rows.try_emplace(std::move(key), static_cast<int32_t>(vismap.size()));
        if (inserted) {
            vismap.insert(vismap.end(), rows[i].begin(), rows[i].end());
        }
        leaf.visofs = it->second;
    }

    bsp.dvis.bit_offsets.clear();
    bsp.dvis.bits = std::move(vismap);

    const double avg = nonsolid ? static_cast<double>(totalvis) / nonsolid : 0.0;
    logging::print("average leafs visible: {:.1f} ({:.1f}%)\n", avg, nonsolid ? 100.0 * avg / nonsolid : 0.0);
    logging::print("max leafs visible: {}\n", maxvis);
    logging::print("unique rows: {} of {}\n", unique_rows.size(), nonsolid);
    logging::print("visdatasize: {}  uncompressed {}\n", bsp.dvis.bits.size(), size_t(nonsolid) * rowbytes);

    // culling is only as fine as the leafs: warn when leafs are bigger than the footprint
    double largest = 0;
    for (int i = 0; i < numleafs; i++) {
        if (!solid[i])
            largest = std::max(
                {largest, boxes[i].maxs[0] - boxes[i].mins[0], boxes[i].maxs[1] - boxes[i].mins[1]});
    }
    const double smallest_footprint = circular ? 2 * gridvis_options.radius.value()
                                               : std::min(gridvis_options.rectx.value(), gridvis_options.recty.value());
    logging::print("largest leaf XY side: {}\n", largest);
    if (largest > smallest_footprint) {
        logging::print("WARNING: some leafs are bigger than the footprint, so culling will be coarse.\n"
                       "         Split open areas with a hint grid (tools/hintgrid.py).\n");
    }

    if (gridvis_options.stats.value()) {
        PrintLeafSizeHistogram(boxes, solid);
    }

    ConvertBSPFormat(&bspdata, loadversion);
    WriteBSPFile(gridvis_options.sourceMap, &bspdata);

    logging::print("{:.2} elapsed\n", I_FloatTime() - starttime);
    logging::close();

    return 0;
}

/*  gridvis: screen-footprint visibility for top-down Quake maps.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#pragma once

#include <common/settings.hh>
#include <common/fs.hh>

#include <string>
#include <vector>

namespace settings
{
extern setting_group gridvis_footprint_group;
extern setting_group gridvis_output_group;

class gridvis_settings : public common_settings
{
public:
    setting_scalar rectx{this, "rectx", 2048.0, 0.0, 1e6, &gridvis_footprint_group,
        "footprint width (world X) in units: the ground the screen shows at the furthest zoom"};
    setting_scalar recty{this, "recty", 2048.0, 0.0, 1e6, &gridvis_footprint_group,
        "footprint depth (world Y) in units"};
    setting_scalar offsetx{this, "offsetx", 0.0, -1e6, 1e6, &gridvis_footprint_group,
        "moves the footprint along X relative to the PVS point"};
    setting_scalar offsety{this, "offsety", 0.0, -1e6, 1e6, &gridvis_footprint_group,
        "moves the footprint along Y relative to the PVS point (a tilted camera sees more ahead than behind)"};
    setting_scalar radius{this, "radius", 0.0, 0.0, 1e6, &gridvis_footprint_group,
        "if > 0, use a circular footprint of this radius instead of -rectx/-recty (for cameras that rotate)"};
    setting_scalar margin{this, "margin", 64.0, 0.0, 1e6, &gridvis_footprint_group,
        "extra border added to the footprint so entities don't pop at the screen edge"};
    setting_bool stats{this, "stats", false, &gridvis_output_group,
        "print a leaf size histogram and per-leaf visibility statistics"};

    fs::path sourceMap;

    void set_parameters(int argc, const char **argv) override
    {
        common_settings::set_parameters(argc, argv);
        program_description =
            "gridvis writes a PVS based on a top-down screen footprint instead of\n"
            "portal visibility. Leaf B is visible from leaf A when B's XY bounds\n"
            "overlap A's XY bounds grown by the footprint. Height is ignored.\n"
            "No .prt file is needed.\n\n";
        remainder_name = "mapname.bsp";
    }
    void initialize(int argc, const char **argv) override;
};
} // namespace settings

extern settings::gridvis_settings gridvis_options;

int gridvis_main(int argc, const char **argv);

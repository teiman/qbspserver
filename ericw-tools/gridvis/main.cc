/*  gridvis: screen-footprint visibility for top-down Quake maps.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#include <gridvis/gridvis.hh>
#include <common/settings.hh>
#include <common/log.hh>

int main(int argc, const char **argv)
{
    logging::preinitialize();

    try {
        return gridvis_main(argc, argv);
    } catch (const settings::quit_after_help_exception &) {
        return 1;
    } catch (const std::exception &e) {
        exit_on_exception(e);
    }
}

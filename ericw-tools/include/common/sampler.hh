/*  Copyright (C) 2026 qbspserver

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#pragma once

#include <cstddef>
#include <string>

/*
 * A sampling profiler inside the program (no profiler is installed on the dev machine): with
 * ERICW_SAMPLE=1 (and ERICW_BENCH), a thread stops every other thread of the process for an
 * instant about every millisecond and records where it is. At the end, the hottest functions and
 * source lines (needs a build with debug info: build-prof). Windows only.
 */
namespace sampler
{
void start();
// stops sampling and returns the report (empty if it wasn't running)
std::string stop_and_report(size_t top = 40);
} // namespace sampler

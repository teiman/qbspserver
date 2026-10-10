qbspserver - incremental qbsp for Quake maps (based on ericw-tools)
=====================================================================

qbspserver keeps your map loaded in a background process and recompiles it
when you run qbspreceive, redoing only what your edit touched. A texture or
entity change on a big map takes seconds instead of minutes.

The BSP it writes is not byte-identical to plain qbsp's, but it plays the
same: same entities, same drawn surfaces and textures, same collision.
For release builds, compile with plain qbsp as usual.


Files
-----
qbspserver.exe    the server: compiles the map once, then again on each request
qbspreceive.exe   asks the running server to compile, prints its log
qbsp.exe          plain qbsp (ericw-tools), about 3x faster on big maps,
                  with byte-identical output: a drop-in replacement


Use
---
1. Start the server once, with the options you give qbsp, and leave it open:

       qbspserver.exe -bsp2 C:\maps\mymap.map

   The first compile is a full one.

2. Each time you want to test, save the map in your editor and run:

       qbspreceive.exe C:\maps\mymap.map

   It writes mymap.bsp (and .prt) next to the .map, like qbsp. Then run
   vis / light / your engine as usual.

TrenchBroom: in the compile profile, replace the qbsp step with
qbspreceive.exe and the map path. Start qbspserver.exe yourself before.

Options
-------
qbspserver [-port N] [-nohullcache] [qbsp options...] mapname[.map]
    Any qbsp option (-bsp2, -notex...); it is used for every compile.
    -port N        another port (default 26600; use the same for qbspreceive)
    -nohullcache   rebuild everything on every compile (to compare)

qbspreceive [-port N] mapname[.map]
    Exit code: 0 compiled, 1 compile failed, 2 no server running,
    3 rejected (the server holds another map). Usable in scripts:
        qbspreceive mymap.map && light mymap.bsp

One server holds one map. Close it with Ctrl+C. Windows only (64-bit).


Speed (Intel Core 7 laptop)
---------------------------
                         e3m4      E1M1     quakemash (35k brushes)
plain qbsp (original)    8.7 s     1.6 s    271 s
qbspserver, no change    0.04 s    0.03 s   instant
  1 entity changed       0.25 s    0.10 s   ~2 s
  1 texture changed      0.65 s    0.2 s    5 s
  1 brush moved / added  1.3 s     0.3 s    11 s


Licenses
--------
ericw-tools is GPL v3 (licenses\ericw-tools-gpl_v3.txt); oneTBB is
Apache 2.0 (licenses\tbb-LICENSE.txt). The source code of this build,
qbspserver included, is in the repository this release comes from.

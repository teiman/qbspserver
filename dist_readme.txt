qbspserver
==========

qbspserver is the qbsp from ericw-tools, changed so that it can stay open
while you work on a map. You start it once, and from then on each compile
only redoes the parts of the map you changed. vis and light are not affected.

More, with pictures: https://github.com/teiman/qbspserver


Files
-----
qbspserver.exe    stays open while you map, does the qbsp work
qbspreceive.exe   goes where qbsp used to be in your compile: asks
                  qbspserver to compile, shows the same messages
qbsp.exe          the normal ericw-tools qbsp with speed fixes: identical
                  output, about a third of the time on big maps. Use it
                  instead of your qbsp.exe if you like, with or without
                  qbspserver.


What changes in the way you work
--------------------------------
1. When you sit down to map, open qbspserver on your map (a .bat file you
   double-click, see below). It compiles once, then waits in its window.
2. In your compile .bat or TrenchBroom profile, the qbsp line uses
   qbspreceive instead.

vis, light, your engine and the way you test stay as they are.


If you compile with a .bat file
-------------------------------
start-server.bat (double-click it when you start mapping, leave it open):

    C:\tools\qbspserver\qbspserver.exe C:\maps\mymap.map

Your compile file, with the first line changed:

    C:\tools\qbspserver\qbspreceive.exe C:\maps\mymap.map
    C:\tools\ericw-tools\vis.exe -fast C:\maps\mymap.bsp
    C:\tools\ericw-tools\light.exe C:\maps\mymap.bsp

qbsp options (like -bsp2) go in start-server.bat:
    qbspserver.exe -bsp2 C:\maps\mymap.map


If you compile from TrenchBroom
-------------------------------
In a copy of your profile (Run > Compile Map...), change only the qbsp task:

    Tool:        C:\tools\qbspserver\qbspreceive.exe
    Parameters:  ${MAP_BASE_NAME}-compile.map

Compile once with your usual profile, so mymap-compile.map exists in the
profile's working directory. In that folder, make a start-server.bat:

    C:\tools\qbspserver\qbspserver.exe mymap-compile.map mymap.bsp

Double-click it, leave it open, and compile from TrenchBroom as usual.
(TrenchBroom can't open qbspserver itself: it would wait for it to finish.)


Day to day
----------
- Open qbspserver when you start mapping, close its window when you're
  done. It holds one map: for another map, close it and open it on that one.
- "no server": qbspserver isn't open. "holds another map": the map path in
  your compile isn't the one qbspserver was opened with.
- Leaks and errors show up as with qbsp. qbspserver keeps running.
- The first compile after opening it takes the usual time.

The map plays the same as with plain qbsp, but the .bsp file isn't
identical byte for byte. For the version you release, use plain qbsp.


Command line
------------
qbspserver [-port N] [qbsp options...] mapname[.map] [output.bsp]
qbspreceive [-port N] mapname[.map]
    -port N   a second qbspserver on another port (default 26600)
    qbspreceive exit codes: 0 compiled, 1 compile failed, 2 no qbspserver,
    3 qbspserver holds another map


Licenses
--------
ericw-tools is GPL v3 (licenses\ericw-tools-gpl_v3.txt); oneTBB is
Apache 2.0 (licenses\tbb-LICENSE.txt). The source code is at
https://github.com/teiman/qbspserver

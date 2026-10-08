# qbspserver

A client/server version of `qbsp` (from [ericw-tools](https://github.com/ericwa/ericw-tools)) that keeps the compiled map in memory, so recompiling after a small edit is fast.

## Components

| Program | Role |
|---|---|
| `qbspserver` | Long-running terminal app. Holds one `.map` file and its compiled data in memory, and writes a log. Runs until it is closed. |
| `qbspreceive` | Small client. Asks the server to compile the map, waits until the compile is done, then exits. |

## Workflow

1. Start `qbspserver` on a `.map` file. It does a full first compile and keeps the results in memory.
2. Edit the map in the editor and save it.
3. Run `qbspreceive`. It tells the server to compile and blocks until the server finishes.
4. `qbspreceive` exits with a code that reflects the result, so scripts can react to it (for example, run `light` and `vis` only if the compile succeeded).

The server **only compiles when `qbspreceive` asks**, never on file save. Compiling on every save could slow the computer down, or keep the server busy with a compile nobody wanted.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Compile succeeded |
| `1` | Compile failed (errors in the map) |
| `2` | Could not reach the server, or the connection was lost |
| `3` | Request rejected: the server holds another map, or bad arguments |

## Communication

Client and server talk over a **local TCP socket**:

- The server listens on `127.0.0.1` only, so no other machine on the network can trigger compiles.
- Default port `26600`, which both programs can change with `-port N` (to run two servers on two maps).
- The client sends the path of the `.map`. If it is not the map the server holds, the server answers with an error instead of compiling the wrong map.

The protocol is plain text, one message per line:

```
client → compile D:\maps\foo.map
server → log <line>        (log lines while compiling, optional)
server → done 0            (exit code for qbspreceive: 0, 1 or 3)
```

`qbspreceive` exits with the code from `done`, or `2` if it cannot connect or the connection drops before `done`. Unknown messages are ignored, so new ones can be added later.

### Log streaming must not slow down the compile

Sending a line over a local socket costs a few microseconds and the data never leaves the machine, so the cost itself is small next to the compile (and the server has to print the log anyway; the Windows console is likely slower than the socket). The real risk is **blocking**: if the client reads slowly or hangs, the socket buffer fills, `send()` blocks, and the compile waits for the client. Rules:

1. **The compile never touches the socket.** Each log line goes into an in-memory queue; a separate thread drains the queue to the socket. A slow client makes the queue grow, it never stalls the compile.
2. **Send in batches.** The sender thread sends everything waiting in the queue in one `send()`, not one call per line.
3. **No progress bars.** ericw-tools redraws percentage bars many times; those are not sent (or only a value every so often).
4. **A disconnected client is ignored.** The server stops sending the log and finishes the compile anyway. A client that stops reading is dropped after a send timeout (10 s), so it cannot hold the server either.

To check: compile the same map with and without a connected client and compare the times.

## Why

A normal `qbsp` run starts from zero every time, even when only one brush changed. `qbspserver` keeps all the data it has already worked out in memory and tracks which parts of the map changed and which did not. On the next compile it reuses the unchanged parts and only redoes the work for the changed ones.

The goal: **a small change to the `.map` gives a fast compile.**

## Output requirements

The `.bsp` does **not** need to match what a full `qbsp` run produces, byte for byte or even closely. It only needs to be:

- **Valid:** engines and the next tools (`light`, `vis`, `gridvis`) load it without problems.
- **Equivalent in game:** it plays the same as the full `qbsp` result. Same collision, same visible geometry, same contents (water, lava, sky...), same entities.

This leaves room for shortcuts, for example keeping a BSP tree that is less balanced than a fresh one would be.

`tools/bspcheck.py` checks exactly this, and the benchmark runs it on every scenario (see Measuring).

## Change tracking

How fine-grained change tracking is was decided by measuring (see Incremental compiles below): **per entity and hull** for whole clipping hulls, and **per grid cell of 1024 units** inside the world's clipping hulls.

What a full compile spends its time on (`ERICW_BENCH` phase table, `-bsp2`):

| Phase | quakemash (271 s) | small / E1M1 (1.8 s) |
|---|---|---|
| hull 1 + hull 2 `BrushBSP` | 174 s (64%) | 0.66 s (37%) |
| hull 1 + hull 2 `MakeTreePortals` | 41 s (15%) | 0.32 s (18%) |
| hull 1 + hull 2 rest (`ChopBrushes`, `FillOutside`, `CountLeafs`...) | 34 s (13%) | 0.29 s (16%) |
| hull 0, all phases | 20 s (7%) | 0.28 s (16%) |
| loading the map, writing the BSP | 2 s (1%) | 0.24 s (14%) |

So on a big map the **collision hulls (1 and 2) are over 90% of the time** (on small, 70%), and their `BrushBSP` alone two thirds. They are the first target. They are also the simplest: no faces, no T-junctions, no portal file; only solid / empty / liquid leaves.

## Incremental compiles

What the server reuses between compiles, in the order it was built. Each step was measured with the benchmark (see Measuring and `testmap/bench_history.txt`). The code is in `qbsp/hullcache.cc` (`include/qbsp/hullcache.hh`), and is only active in `qbspserver`: plain `qbsp` builds everything, as before. `qbspserver -nohullcache` turns it all off, to compare.

### 0. No throttling by Windows

Before any caching: every compile after the first one in the server took **25% longer** than the same compile in a fresh `qbsp` process, in every phase, even single-threaded ones. Windows 11 treats a process that mostly waits (the server between compiles) as background work and throttles it (EcoQoS: lower clocks, efficiency cores; this laptop has an Intel Core 7 240H, a hybrid CPU). `logging::preinitialize()` now opts every ericw-tools program out of execution-speed throttling (`SetProcessInformation(ProcessPowerThrottling)`), so the server, and also `qbsp`, `vis` and `light` started from scripts or editors, run at full speed.

### 1. Whole clipping hulls (hull cache)

The clipping hulls only depend on the brushes' shape and contents, not on their textures (only on texture flags like skip or hint). After building an entity's hull 1 or 2, the server keeps the finished tree, keyed by a hash of everything the hull reads: brush planes, contents, texture flags, the entity's keys, and for the world also the brushes of `func_group`, `func_detail`... When a later compile has the same key, the stored tree is exported again (same clipnodes, in the same order) instead of being built.

For the world, the outside filling also depends on the point entities. With the default `-filltype auto`, hulls 1 and 2 are filled from the void and `FillDetail` is skipped, so the entities only decide whether filling happens at all: not if an entity is in the void (a leak), not if no entity is in empty space. A hull's void is inside hull 0's void (the same brushes, only bigger), so if this compile's hull 0 was filled, no entity is in the hull's void either; and one entity in an empty leaf of the stored tree is enough for the other condition. With another `-filltype`, any change in the entities rebuilds the hull.

Result: when only textures or point entities change (or nothing), hulls 1 and 2 cost nothing. e3m4: ~9.5 s → 1.4 s (7-8x); benchmark TOTAL 74.9 s → 44.9 s (-40%).

### 2. One pass instead of two (side marks)

When the world's brushes changed, qbsp builds each clipping hull twice: a quick tree (FAST) only to find which brush sides face nothing but the void (`FillOutside`), then the final tree, which uses those sides as splitters last. Those marks only steer the choice of splitters, not what is solid. The server keeps the marks of the last compile per brush; the next compile applies them (new or changed brushes: every side visible) and builds the final tree directly: no FAST tree, no first portals, no first filling. The final filling then produces fresh marks for the next compile.

Result: geometry edits about 2x faster (e3m4 move: 8.3 s → 4.2 s).

### 3. Cells

Most of the world doesn't change in one edit. Around the final `BrushBSP` of a world clipping hull, the top of the tree is split on a fixed grid of 1024-unit cells (like qbsp's own midsplit for nodes bigger than `maxnodesize`), so each cell's subtree depends only on the brush fragments inside it. A cell's key is a hash of its bounds and of its fragments (contents and planes, in any order). A cell with a stored subtree for that key gets it grafted (nodes, planes, leaf contents, the leaves' original brushes for the side marks); the others are built, then stored. The rest (portals, filling, pruning, export) runs on the whole tree as usual.

One trap, found with `QBSPSERVER_DEBUG_CELLS=1` (logs every rebuilt cell): qbsp shares planes across the map and merges planes closer than `NORMAL_EPSILON` / `DIST_EPSILON`, so the exact value a side gets depends on which similar plane was created first, anywhere. Moving one brush changed the last digits (~1e-12) of planes all over the map, and hull 2 rebuilt 25 of 70 cells. Keys now round plane values (and cell bounds) to a tenth of the epsilons: hull 2 rebuilds 1 of 70.

Result: e3m4 move 4.2 s → 3.1 s; the clipping hulls' `BrushBSP` 0.8-0.9 s → 0.08 s each.

Steps 0, 2 and 3 measured together: benchmark TOTAL 44.9 s → **20.8 s** (-54%); geometry edits on e3m4 8.3 s → 3.0 s, on small 1.5 s → 0.65 s. Against the start (baseline m3, 74.9 s): **3.6x faster**, 3.5x faster than plain `qbsp`, every check OK.

Debug switches: `QBSPSERVER_DEBUG_CELLS=1` (log rebuilt cells), `QBSPSERVER_NO_HULL_REPLAY=1` (never reuse a whole hull, to test the cells alone).

### 4. No chopping of the world's clipping hulls

qbsp chops the clipping hulls' brushes (removes their overlaps) so BrushBSP has fewer brush tests. With cells, BrushBSP only runs where something changed, while chopping ran on the whole map every compile (0.5 s on e3m4, 10 s on quakemash). The server skips it for the world's clipping hulls; the solid space is the same. `QBSPSERVER_CHOP_HULLS=1` chops anyway. Quick A/B: geometry edits -9%; in the full benchmark within the noise.

### 5. Unchanged map

If the map file is byte for byte the one of the last successful compile, and the wads it names (`"wad"` / `"_wad"`) and the `.bsp` written then are untouched (size and time), the `.bsp` on disk is already the result: the server answers at once (~30-40 ms instead of 0.3-1.3 s). Not with `misc_external_map` (it reads other files), and never for the first request (the server can't know which file the first compile read). Tested: touching the wad, deleting the `.bsp` or editing the map all compile again.

Steps 4 and 5 measured together: TOTAL 20.8 s → **18.7 s** (-9.8%). Against the start (74.9 s): **4.0x**; 3.9x faster than plain `qbsp`.

### 6. Hull 0's tree

Hull 0 (the drawing hull: faces, leaves, the `.prt` for vis) is built in two passes like the clipping hulls, and its tree is rebuilt every compile, even when only a texture changed. Its final tree (before filling) only depends on the brushes (shapes, contents, texture flags, order) and on the side marks of the first pass. Those come from the first filling, which keeps the areas of connected empty space that hold a point entity (`FillOutside`), and the same with detail counting as wall (`FillDetail`). These areas are a property of the geometry, not of the tree.

So the server keeps hull 0's final tree, plus the first pass's tree with its areas labelled (flooding through its portals, the way the two fillings flood). On the next compile, if the brushes are the same, the point entities are placed in the stored first tree with exactly qbsp's arithmetic (`PointInLeaf`, the same plane values, the sealing side winning on a node plane) and if they occupy the same set of areas, the final tree is exactly the one a fresh compile would build: it is grafted, and both `BrushBSP` passes, the first portals and the first filling are skipped. Its filling, the faces (with this compile's textures), T-junctions and output are done as usual. Moving a light inside the same room keeps the tree; moving it into another room rebuilds it.

Brush entities (doors, platforms, buttons...) the same, per entity, without the entities part; but their faces depend on the side marks their first pass left (`FillBrushEntity`, never redone on the final tree), so those marks are stored with the tree and restored.

Result: TOTAL 18.7 s → **13.8 s** (-26%); e3m4 entity / texture edits 1.3-1.4 s → 0.66-0.78 s (10-12x faster than `qbsp`), geometry edits -12% to -30% (the 263 brush entities are reused). quakemash: entity / texture edits 17 s → **5 s** (54x), brush edits 26 s → 21 s (13x). Against the start (74.9 s): **5.4x**.

### Tried and dropped

- **Hull 0 in one pass** (like step 2): bspcheck failed on e3m4. Hull 0's faces come from the final tree's portals, and a tree built with the final pass's marks lost a few faces (0.01% of the drawn area). Hull 0 keeps its two passes.
- **Hull 0's final tree with midsplit for big nodes** (`AUTO`, like its first pass): no time saved on small or e3m4, and 70-90% more nodes and leaves, 26-40% more faces: slower `vis` and `light`, bigger BSP. Same reason not to put hull 0 on the cell grid: grid planes cut through open space.
- **`-fasthulls`** (clipping hulls' final tree in `AUTO`): 10-15% less hull BrushBSP on e3m4 but 43% more clipnodes; superseded by the cells.

### At scale: quakemash

35,839 brushes; plain `qbsp`: 271 s. One run, no reference (`bench.py --maps quakemash --no-ref --reps 1`):

| | time |
|---|---|
| first compile in the server | 155 s (no hull chopping, cell grid: already faster) |
| no change / texture | 17 s (16x); with step 6: 5 s (54x) |
| one brush moved / a few moved, retextured, added | 26 s (10x); with step 6: 21 s (13x) |

What's left for geometry edits at this scale: hull 0 rebuilt (its two `BrushBSP` passes, 5.4 s), the clipping hulls' portals (`MakeTreePortals`, 1.8-2.2 s each), map loading (0.9 s). For entity / texture edits (5 s): map loading, hull 0's portals, T-junctions, faces, writing.

## Measuring

Every change is measured with the same benchmark, and every run is logged, so progress (or a regression) is visible version after version.

```
bench.bat "what changed"          maps small + e3m4, 3 repetitions (~15 min), logged
bench.bat "what changed" quick    small only, 1 repetition (~1 min), not logged
set BENCH_ARGS=--maps all         adds quakemash (a full compile takes ~4.5 min: hours of benchmark)
```

For each map, `tools/bench.py` starts its own `qbspserver` (port 26650, on a work copy of the map) and runs these **scenarios**, from no change to many changes (`tools/mapedit.py --list`):

| Scenario | Edits |
|---|---|
| `nochange` | none: recompile the same map |
| `entity` | 1 point entity moved 16 units (and its light value changed) |
| `texture` | 1 brush retextured |
| `move` | 1 small brush moved 8 units |
| `add` | 1 brush added (16-unit box under a light) |
| `geotex` | 3 brushes moved + 3 retextured + 1 added |
| `many` | 2% of the brushes moved + 2% retextured + 10 added + 10 entities changed |

For each scenario:

1. Writes the edited map and times `qbspreceive` (wall time: what the user waits for). The server's own time and its per-phase table are recorded too.
2. Writes the base map back and recompiles, so every scenario starts from the same server state.
3. Right after, compiles the same edited map with a plain `qbsp` (the **reference**), in every repetition. On the first one, also checks with `tools/bspcheck.py` that the server's BSP plays the same as the reference.

Rules that keep the numbers comparable between versions:

- **Same edits every time.** `mapedit.py` picks brushes with fixed seeds. Edits that make the map leak are not used (a leaking map skips the outside filling and does other work): the first run tries other picks with a quick hull-0 compile and stores the one that keeps the map sealed in `testmap/bench_seeds.json`. Runs are only compared when the scenarios (`SCENARIO_VERSION`), picks, maps and qbsp options are the same.
- **Medians and noise.** Each scenario runs 3 times; its time is the median. On this laptop the same compile varies 7-10% from run to run, so a change smaller than the spread of the repetitions (at least 5% per scenario, 2% for totals) is marked `~` (noise).
- **Heat.** The laptop slows down under sustained load: e3m4 takes 9.5 s cold and 12.5 s after 10 minutes of compiles, for the server and for plain `qbsp` alike (checked: 15 compiles in one server, with no slowdown of its own and memory steady at 330-540 MB). So `bench.py` warms up for 60 s before measuring (`--warmup`), and runs the reference **paired** with the server in every repetition: the speedup compares two compiles made in the same thermal state, and stays valid even if the machine runs hotter or cooler than last time. Comparisons of absolute times between runs are only as good as the machine's state: same power plan, plugged in, nothing else running.
- **Every edit is new to the server.** Once the server caches, a repetition would find the result of the same edit from the previous repetition. After writing the base map back and recompiling it, `bench.py` runs `qbspreceive -forget`: the server drops everything its last compile (the base map) didn't use, as if it had only ever seen the base map. (Method 3.)
- **Nothing else running.** Two runs died without a word while other heavy commands ran at the same time; with the machine left alone they finish. Besides, anything else running is noise.
- **Same method.** `METHOD_VERSION` in `bench.py` changes when the way of measuring changes; runs measured differently are not compared. Method 3's baseline is `baseline m3: full qbsp per request (-nohullcache)`.
- **One number per version: the TOTAL**, the sum of all scenario medians on all maps. Adding up independent scenarios makes it much steadier than any single one. Next to it: the reference total and the **speedup** (reference / server).
- **Correctness is part of the result.** A scenario whose BSP does not play like the reference is reported as `FAIL`, and `bench.py` exits with code 1.

Results:

| File | Content |
|---|---|
| `testmap/bench_history.txt` | One line per run: date, label, build hash, TOTAL, reference, speedup, checks, change vs the previous comparable run, every scenario's median |
| `testmap/bench_history.jsonl` | The same run with every number (repetitions, spreads, phases, checks), for comparisons |
| `testmap/bench_details.txt` | Per scenario: the edits, every repetition, the server's and the reference's phase tables, the check report |
| `testmap/bench/` | Logs and BSPs of the last run (server output, server and reference BSP per scenario) |

At the end, `bench.py` prints a table with the previous comparable run and the change per scenario, and the slowest server phases.

### bspcheck: does it play the same?

`tools/bspcheck.py a.bsp b.bsp` compares what the game sees, not the bytes:

- **Entities:** the entity lump, line by line.
- **Models:** the same number of models.
- **Surfaces:** per model and texture, the drawn area and its area-weighted centre (catches moved geometry, not just added or removed).
- **Contents:** empty / solid / water / slime / lava / sky, in hull 0 (drawing) and hulls 1 and 2 (player and shambler collision), at 20,000 random points in the world plus points in every brush model, **and 1 unit on both sides of every face that has no twin in the other BSP** (for hulls 1 and 2, on both sides of the expanded collision plane). Random points alone miss a 16-unit box; the points next to changed faces find it.

Tested: two compiles of the same map (BSP2 vs BSP2, and BSP29 vs BSP2) are `SAME`; one entity moved, one brush retextured, one 16-unit box added, or one brush moved 8 units are all `DIFFERENT`. About 4 s on quakemash.

### Per-phase times

`common/log.cc` already had `ERICW_BENCH=1` (from vklight / gridvis): a table of the time per phase (`---- Name ----` headers), printed when the program exits. For the server:

- `logging::bench_restart()` / `logging::bench_report()` measure each compile on its own; the server prints the table at the end of every compile, so `qbspreceive` shows it too.
- `logging::bench_set_group()` prefixes the phases, and qbsp uses it per hull (`hull 1: BrushBSP`), so each hull's cost is visible.
- `(init)` is the time before the first phase (options, filesystem, wads); `hull N: (start)` and `(after hull 2)` the time between phases.

## Project layout

Reused from `D:\code\gridvis`:

- `ericw-tools/`: the ericw-tools source tree (with gridvis), including the `qbsp` code this project builds on. `ericw-tools/testmaps/` has ~200 small test maps.
- `build.bat` / `build.sh`: build ericw-tools on Windows and Linux, download Embree and oneTBB. `build.bat qbspserver` builds one target.
- `tools/makemap.py` (+ `hintgrid.py`) and `testmap/gfx/`: generate big test maps.

Test maps, in `testmap/`:

| Map | From | Size | Full compile |
|---|---|---|---|
| `small.map` | vklight (E1M1) | 1,257 brushes | 1.6 s |
| `e3m4.map` | ericw-tools/testmaps (LibreQuake), textures missing (its wads are not in the tree) | 3,441 brushes | 9 s |
| `quakemash.map` | vklight | 35,839 brushes | 271 s |
| `gridtest.map` | gridvis (`makemap.py`) | 163 brushes | 0.5 s |

New code, in `ericw-tools/`:

| File | Content |
|---|---|
| `include/qbspserver/net.hh`, `qbspserver/net.cc` | Local TCP socket (Winsock / BSD sockets), exit codes, `-port` parsing |
| `qbspserver/server.cc` | `qbspserver`: request loop, log streaming, compile |
| `qbspserver/receive.cc` | `qbspreceive` |
| `common/log.cc`, `include/common/log.hh` | Per-compile phase times (`bench_restart`, `bench_report`, `bench_set_group`) |
| `qbsp/qbsp.cc` | Phase times per hull |

Tools, in `tools/`: `bench.py` (benchmark), `mapedit.py` (scenario edits), `bspcheck.py` (do two BSPs play the same). Scripts in the root: `server.bat`, `receive.bat`, `play.bat`, `bench.bat`, `build.bat`.

Built programs end up in `ericw-tools/build-windows/qbspserver/Release/`.

## Usage

```
qbspserver [-port N] [qbsp options...] mapname[.map]
qbspreceive [-port N] mapname[.map]
```

`qbspserver` takes the same options as `qbsp`, and uses them for every compile.

## Status

1. **Done: client/server.** Both programs, the protocol, log streaming and exit codes. Each request still runs a **full** `qbsp` compile in the server process (`InitQBSP` + `ProcessFile`, like `qbsp` itself).
2. **Done: measuring.** Benchmark with scenarios, reference compiles, equivalence checks and a history (see Measuring). Profile: the collision hulls are ~90% of the time (see Change tracking). Baseline logged as `baseline: full qbsp per request`: **TOTAL 81.8 s** (small 11.5 s, e3m4 70.3 s), reference 81.9 s, speedup 1.00x, all checks OK. Over the 42 paired repetitions the server is as fast as plain `qbsp` (ratio median 1.03 on small, 0.99 on e3m4), as expected while it still compiles everything.
3. **In progress: incremental compiles** (see Incremental compiles). Done: no throttling, whole clipping hulls, one-pass hulls, cells, no hull chopping, unchanged-map shortcut, hull 0 tree reuse. Benchmark TOTAL 74.9 s → 13.8 s (5.4x). Next candidates: the clipping hulls' `MakeTreePortals` (per-cell portals), hull 0 for geometry edits without degrading its tree, map loading (parse only the entities that changed).

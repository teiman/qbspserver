#!/usr/bin/env python3
"""Correctness sweep: qbspserver against plain qbsp on many small test maps.

For every Quake 1 test map in ericw-tools/testmaps (water, sky, detail, fences, clip, hint,
skip, origin brushes, external maps...), it:
  1. compiles the map with plain qbsp (the reference);
  2. starts qbspserver on a copy of the map (its first compile: cells, no hull chopping);
  3. appends a comment to the map and asks the server again: this compile goes through the
     caches (whole clipping hulls, hull 0 trees...);
and checks both server BSPs against the reference with tools/bspcheck.py.

    sweep.py [--maps q1_*.map ...] [--qbsp-args "-bsp2"]

Maps that plain qbsp can't compile (on purpose: leaks, errors) are skipped.
"""
import argparse
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bspcheck  # noqa: E402
from bench import QBSP, RECEIVE, SERVER, ROOT  # noqa: E402

TESTMAPS = os.path.join(ROOT, 'ericw-tools', 'testmaps')
PORT = 26690


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, capture_output=True)


def check(server_bsp, ref_bsp):
    try:
        ok, lines = bspcheck.compare(bspcheck.Bsp(server_bsp), bspcheck.Bsp(ref_bsp), points=5000)
    except Exception as e:  # noqa: BLE001
        return False, [f'bspcheck error: {e}']
    return ok, [l for l in lines if l.startswith('FAIL')]


def sweep_map(name, args, work):
    src = os.path.join(TESTMAPS, name)
    base = os.path.splitext(name)[0]
    # compile next to the original so relative paths (wads, external maps) still work
    work_map = os.path.join(TESTMAPS, f'_sweep_{base}.map')
    ref_bsp = os.path.join(work, f'{base}_ref.bsp')
    shutil.copyfile(src, work_map)
    try:
        r = run([QBSP, *args.qbsp_args, work_map, ref_bsp], TESTMAPS)
        if r.returncode != 0 or not os.path.exists(ref_bsp):
            return 'skip', ['plain qbsp fails on it']

        log = open(os.path.join(work, f'{base}_server.txt'), 'wb')
        srv = subprocess.Popen([SERVER, '-port', str(PORT), *args.qbsp_args, work_map], stdout=log,
                               stderr=subprocess.STDOUT, cwd=TESTMAPS)
        try:
            start = time.perf_counter()
            while True:
                text = open(log.name, 'rb').read()
                if b'listening on' in text:
                    break
                if srv.poll() is not None or time.perf_counter() - start > 120:
                    return 'FAIL', ['the server did not start']
                time.sleep(0.05)
            if b'first compile done (0)' not in text:
                return 'FAIL', ['the server\'s first compile failed']

            server_bsp = os.path.splitext(work_map)[0] + '.bsp'
            first = os.path.join(work, f'{base}_first.bsp')
            shutil.copyfile(server_bsp, first)

            with open(work_map, 'a', encoding='utf-8', errors='surrogateescape') as f:
                f.write('\n// sweep\n')
            r = run([RECEIVE, '-port', str(PORT), work_map], TESTMAPS)
            if r.returncode != 0:
                return 'FAIL', [f'the second compile failed ({r.returncode})']
            second = os.path.join(work, f'{base}_second.bsp')
            shutil.copyfile(server_bsp, second)
        finally:
            srv.kill()
            srv.wait()
            log.close()

        problems = []
        for label, bsp in (('first compile', first), ('cached compile', second)):
            ok, fails = check(bsp, ref_bsp)
            if not ok:
                problems += [f'{label}: {f}' for f in fails]
        return ('FAIL' if problems else 'ok'), problems
    finally:
        for f in os.listdir(TESTMAPS):
            if f.startswith(f'_sweep_{base}.'):
                os.remove(os.path.join(TESTMAPS, f))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--maps', nargs='+', default=['q1_*.map'], help='patterns in ericw-tools/testmaps')
    ap.add_argument('--qbsp-args', default='-bsp2')
    args = ap.parse_args()
    args.qbsp_args = args.qbsp_args.split()

    names = sorted({n for n in os.listdir(TESTMAPS) for p in args.maps if fnmatch.fnmatch(n, p)})
    work = tempfile.mkdtemp(prefix='qbspsweep_')
    counts = {'ok': 0, 'FAIL': 0, 'skip': 0}
    for name in names:
        status, notes = sweep_map(name, args, work)
        counts[status] += 1
        print(f'{status:4} {name}' + ('' if status == 'ok' else '  ' + '; '.join(notes[:3])), flush=True)
    print(f'\n{counts["ok"]} ok, {counts["FAIL"]} FAIL, {counts["skip"]} skipped   (outputs in {work})')
    return 1 if counts['FAIL'] else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Compile benchmark for qbspserver: how long a recompile takes after typical edits.

For each map it starts its own qbspserver (on a work copy of the map, on its own port) and,
for each scenario (see tools/mapedit.py --list: no change, one entity, one texture, one brush
moved, one brush added, a few of each, many changes):

  1. writes the edited map and times qbspreceive (wall time, what the user waits for);
  2. writes the base map back and recompiles, so every scenario starts from the same state;
  3. on the first repetition, also compiles the edited map with a plain full qbsp (the reference)
     and checks with tools/bspcheck.py that the server's BSP plays the same as the reference.

Every scenario runs --reps times; its time is the median. The TOTAL is the sum over all maps and
scenarios: one number to compare versions. Each run is logged with its --label and build:

  testmap/bench_history.txt     one line per run (what to read to follow progress)
  testmap/bench_history.jsonl   the same, with every number, for comparing runs
  testmap/bench_details.txt     per scenario: the edits, every repetition, per-phase times, checks

At the end it compares with the previous comparable run (same scenarios, maps and options).

    bench.py [--label text] [--maps small e3m4 | all] [--scenarios ...] [--reps 3] [--no-ref]
"""
import argparse
import faulthandler
import datetime
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bspcheck  # noqa: E402
import mapedit  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTMAP = os.path.join(ROOT, 'testmap')
WORK = os.path.join(TESTMAP, 'bench')  # server / reference outputs and logs of the last run
HISTORY = os.path.join(TESTMAP, 'bench_history.txt')
HISTORY_JSON = os.path.join(TESTMAP, 'bench_history.jsonl')
DETAILS = os.path.join(TESTMAP, 'bench_details.txt')
# per map and scenario, which pick of brushes (mapedit attempt) does not make the map leak
SEEDS = os.path.join(TESTMAP, 'bench_seeds.json')
MAX_ATTEMPTS = 20
# Bump when the way of measuring changes: runs are only compared with runs measured the same way.
#   1: reference only in the first repetition, no warm-up
#   2: reference in every repetition (paired with the server), warm-up before measuring
#   3: the server forgets cached results of the edits between repetitions (needed once it caches)
METHOD_VERSION = 3

if os.name == 'nt':
    BUILD = os.path.join(ROOT, 'ericw-tools', 'build-windows')
    SERVER = os.path.join(BUILD, 'qbspserver', 'Release', 'qbspserver.exe')
    RECEIVE = os.path.join(BUILD, 'qbspserver', 'Release', 'qbspreceive.exe')
    QBSP = os.path.join(BUILD, 'qbsp', 'Release', 'qbsp.exe')
else:
    BUILD = os.path.join(ROOT, 'ericw-tools', 'build-linux')
    SERVER = os.path.join(BUILD, 'qbspserver', 'qbspserver')
    RECEIVE = os.path.join(BUILD, 'qbspserver', 'qbspreceive')
    QBSP = os.path.join(BUILD, 'qbsp', 'qbsp')

# maps in testmap/: small = E1M1 (~1 s), e3m4 = LibreQuake e3m4 (~9 s), quakemash = huge
DEFAULT_MAPS = ['small', 'e3m4']
ALL_MAPS = ['small', 'e3m4', 'quakemash']
DEFAULT_PORT = 26650  # not qbspserver's default, so a server you have open is not disturbed
SERVER_START_TIMEOUT = 3600

BENCH_LINE_RE = re.compile(r'^BENCH status=\S+ elapsed=([\d.]+)')
PHASE_RE = re.compile(r'^\s*([\d.]+)s\s+(.+?)(?: \(x(\d+)\))?\s*$')


def parse_bench(text):
    """Last '==== BENCH' block of a compile log -> (elapsed, [(seconds, phase)], block text)."""
    i = text.rfind('==== BENCH')
    if i < 0:
        return None, [], ''
    block = text[i:]
    end = block.find('\n', block.find('BENCH status='))
    block = block[:end if end >= 0 else len(block)].strip()
    elapsed = None
    phases = []
    for line in block.splitlines()[1:]:
        m = BENCH_LINE_RE.match(line)
        if m:
            elapsed = float(m.group(1))
            continue
        m = PHASE_RE.match(line)
        if m:
            phases.append((float(m.group(1)), m.group(2)))
    return elapsed, phases, block


def build_id():
    h = hashlib.sha1()
    for exe in (SERVER, QBSP):
        with open(exe, 'rb') as f:
            h.update(f.read())
    stamp = datetime.datetime.fromtimestamp(os.path.getmtime(SERVER)).strftime('%m-%d %H:%M')
    return f'{h.hexdigest()[:8]} ({stamp})'


class Server:
    def __init__(self, work_map, port, qbsp_args, log_path, server_args=()):
        self.port = port
        self.log_path = log_path
        self.log = open(log_path, 'wb')
        self.proc = subprocess.Popen([SERVER, '-port', str(port), *server_args, *qbsp_args, work_map],
                                     stdout=self.log, stderr=subprocess.STDOUT, cwd=TESTMAP)

    def wait_ready(self):
        start = time.perf_counter()
        while True:
            text = self.read_log()
            if 'listening on' in text:
                return text
            if self.proc.poll() is not None:
                raise RuntimeError(f'qbspserver exited ({self.proc.returncode}), see {self.log_path}')
            if time.perf_counter() - start > SERVER_START_TIMEOUT:
                raise RuntimeError(f'qbspserver did not start, see {self.log_path}')
            time.sleep(0.1)

    def read_log(self):
        with open(self.log_path, 'rb') as f:
            return f.read().decode('utf-8', 'replace')

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.log.close()


def receive(work_map, port, *extra):
    start = time.perf_counter()
    r = subprocess.run([RECEIVE, '-port', str(port), *extra, work_map], capture_output=True, cwd=TESTMAP)
    wall = time.perf_counter() - start
    out = r.stdout.decode('utf-8', 'replace') + r.stderr.decode('utf-8', 'replace')
    return r.returncode, wall, out


def reference(work_map, out_bsp, qbsp_args):
    env = dict(os.environ, ERICW_BENCH='1')
    for ext in ('.pts', '.bsp'):
        p = os.path.splitext(out_bsp)[0] + ext
        if os.path.exists(p):
            os.remove(p)
    start = time.perf_counter()
    r = subprocess.run([QBSP, *qbsp_args, work_map, out_bsp], capture_output=True, cwd=TESTMAP, env=env)
    wall = time.perf_counter() - start
    out = r.stdout.decode('utf-8', 'replace') + r.stderr.decode('utf-8', 'replace')
    leaked = os.path.exists(os.path.splitext(out_bsp)[0] + '.pts')
    return r.returncode, wall, out, leaked


def write(path, text):
    with open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write(text)


def edit(base_text, scen, attempt):
    ents = mapedit.parse_map(base_text)
    edits = mapedit.apply_scenario(ents, scen, attempt)
    return mapedit.write_map(ents), edits


def leaks(name, text, qbsp_args):
    """Compiles hull 0 only (where qbsp looks for leaks) and says whether the map leaks."""
    work_map = os.path.join(TESTMAP, f'_bench_{name}_calib.map')
    out_bsp = os.path.join(WORK, f'{name}_calib.bsp')
    write(work_map, text)
    try:
        code, _, _, leaked = reference(work_map, out_bsp, [*qbsp_args, '-noclip'])
    finally:
        for f in os.listdir(TESTMAP):
            if f.startswith(f'_bench_{name}_calib.'):
                os.remove(os.path.join(TESTMAP, f))
    if code != 0:
        raise RuntimeError(f'{name}: qbsp failed while checking for leaks ({code})')
    return leaked


def calibrate(name, base_text, scenarios, qbsp_args):
    """Picks, once, edits that keep the map sealed; stored in bench_seeds.json.

    A leaking map skips the outside filling and does other work, so its times would not be
    comparable. The picks are kept until the base map, the scenarios or the qbsp options change."""
    seeds = {}
    if os.path.exists(SEEDS):
        with open(SEEDS, encoding='utf-8') as f:
            seeds = json.load(f)
    base_hash = hashlib.sha1(base_text.encode('utf-8', 'surrogateescape')).hexdigest()[:12]
    key = ' '.join(qbsp_args)
    entry = seeds.get(name)
    if not entry or entry.get('base_hash') != base_hash or entry.get('scenario_version') != mapedit.SCENARIO_VERSION \
            or entry.get('qbsp_args') != key:
        entry = {'base_hash': base_hash, 'scenario_version': mapedit.SCENARIO_VERSION, 'qbsp_args': key,
                 'attempts': {}}

    changed = False
    if 'base_leaks' not in entry:
        print('   checking that the base map is sealed...', flush=True)
        entry['base_leaks'] = leaks(name, base_text, qbsp_args)
        changed = True
        if entry['base_leaks']:
            print(f'   WARNING: {name} leaks without edits; leak checks are skipped', flush=True)

    for scen in scenarios:
        if scen in entry['attempts']:
            continue
        attempt = 0
        if mapedit.can_leak(scen) and not entry['base_leaks']:
            for attempt in range(MAX_ATTEMPTS):
                print(f'   calibrating {scen}: attempt {attempt}...', flush=True)
                if not leaks(name, edit(base_text, scen, attempt)[0], qbsp_args):
                    break
            else:
                print(f'   WARNING: {name}/{scen} leaks on every attempt; using attempt 0', flush=True)
                attempt = 0
        entry['attempts'][scen] = attempt
        changed = True

    if changed:
        seeds[name] = entry
        with open(SEEDS, 'w', encoding='utf-8') as f:
            json.dump(seeds, f, indent=2, sort_keys=True)
            f.write('\n')
    return {scen: entry['attempts'][scen] for scen in scenarios}


def bench_map(name, args, details):
    src = os.path.join(TESTMAP, name + '.map')
    with open(src, encoding='utf-8', errors='surrogateescape') as f:
        base_entities = mapedit.parse_map(f.read())
    base_text = mapedit.write_map(base_entities)

    # the edited maps: the same for every repetition, and every version (fixed seeds)
    print(f'\n== {name}', flush=True)
    attempts = calibrate(name, base_text, args.scenarios, args.qbsp_args)
    edited = {scen: edit(base_text, scen, attempts[scen]) for scen in args.scenarios}

    # in testmap/ itself, so the map's relative wad paths work
    work_map = os.path.join(TESTMAP, f'_bench_{name}.map')
    server_bsp = os.path.splitext(work_map)[0] + '.bsp'
    write(work_map, base_text)

    print('   starting qbspserver (full compile)...', flush=True)
    server = Server(work_map, args.port, args.qbsp_args, os.path.join(WORK, f'{name}_server.txt'), args.server_args)
    result = {'scenarios': {}, 'attempts': attempts}
    try:
        log = server.wait_ready()
        initial, _, _ = parse_bench(log)
        if 'first compile done (0)' not in log:
            raise RuntimeError(f'the base map does not compile, see {server.log_path}')
        result['initial'] = initial
        print(f'   initial full compile in the server: {initial:.3f}s', flush=True)

        # A laptop slows down as it heats up (e3m4: 9.5s cold, 12.5s after 10 minutes of compiles).
        # Warm up first, so the first scenarios are not measured on a cooler CPU than the rest.
        if args.warmup > 0:
            start, count = time.perf_counter(), 0
            while time.perf_counter() - start < args.warmup:
                receive(work_map, args.port)
                count += 1
            print(f'   warm-up: {count} compiles in {time.perf_counter() - start:.0f}s', flush=True)

        runs = {scen: [] for scen in args.scenarios}
        for rep in range(args.reps):
            for scen in args.scenarios:
                text, edits = edited[scen]
                write(work_map, text)
                code, wall, out = receive(work_map, args.port)
                elapsed, phases, block = parse_bench(out)
                run = {'wall': wall, 'server': elapsed, 'code': code, 'phases': phases, 'block': block}
                if code != 0:
                    tail = '\n'.join(out.strip().splitlines()[-5:])
                    raise RuntimeError(f'{name}/{scen}: qbspreceive exit code {code}\n{tail}')

                # the reference right after the server, in every repetition: both are measured in the
                # same thermal state, so the speedup holds even while the machine heats up
                if args.ref:
                    kept = os.path.join(WORK, f'{name}_{scen}_server.bsp')
                    shutil.copyfile(server_bsp, kept)
                    ref_bsp = os.path.join(WORK, f'{name}_{scen}_ref.bsp')
                    rcode, rwall, rout, leaked = reference(work_map, ref_bsp, args.qbsp_args)
                    if rcode != 0:
                        raise RuntimeError(f'{name}/{scen}: reference qbsp failed ({rcode})')
                    _, rphases, rblock = parse_bench(rout)
                    run['ref'] = {'wall': rwall, 'phases': rphases, 'block': rblock, 'leaked': leaked}
                    if rep == 0:
                        ok, check_lines = bspcheck.compare(bspcheck.Bsp(kept), bspcheck.Bsp(ref_bsp))
                        run['check'] = {'ok': ok, 'lines': check_lines}

                # back to the base map, so the next scenario starts from the same state
                write(work_map, base_text)
                rcode, rwall, _ = receive(work_map, args.port)
                if rcode != 0:
                    raise RuntimeError(f'{name}/{scen}: recompiling the base map failed ({rcode})')
                run['revert'] = rwall
                # the server keeps only what the base map uses: the next repetition's edit must
                # not find its own result from this one (a real edit is always new to the server)
                fcode, _, _ = receive(work_map, args.port, '-forget')
                if fcode != 0:
                    raise RuntimeError(f'{name}/{scen}: forget failed ({fcode})')
                runs[scen].append(run)

                mark = ''
                if 'ref' in run:
                    mark = f'  ref {run["ref"]["wall"]:.3f}s' + ('  LEAK' if run['ref']['leaked'] else '')
                if 'check' in run:
                    mark += '  check ' + ('OK' if run['check']['ok'] else 'FAIL')
                server_time = f' (server {elapsed:.3f}s)' if elapsed is not None else ''
                print(f'   rep {rep + 1}/{args.reps}  {scen:9} {wall:8.3f}s{server_time}{mark}', flush=True)

        for scen in args.scenarios:
            rs = runs[scen]
            walls = [r['wall'] for r in rs]
            median = statistics.median(walls)
            mid = min(rs, key=lambda r: abs(r['wall'] - median))  # the repetition closest to the median
            first = rs[0]
            ref = None
            if args.ref:
                ref_walls = [r['ref']['wall'] for r in rs]
                ref_median = statistics.median(ref_walls)
                ref_mid = min(rs, key=lambda r: abs(r['ref']['wall'] - ref_median))['ref']
                ref = {'wall': ref_median, 'walls': ref_walls, 'phases': ref_mid['phases'], 'block': ref_mid['block'],
                       'leaked': any(r['ref']['leaked'] for r in rs)}
            result['scenarios'][scen] = {
                'edits': edited[scen][1],
                'walls': walls,
                'median': median,
                'spread': (max(walls) - min(walls)) / median if median else 0.0,
                'server': statistics.median(r['server'] for r in rs if r['server'] is not None)
                if any(r['server'] is not None for r in rs) else None,
                'reverts': [r['revert'] for r in rs],
                'phases': mid['phases'],
                'block': mid['block'],
                'ref': ref,
                'check': first.get('check'),
            }
    finally:
        server.stop()

    result['total'] = sum(s['median'] for s in result['scenarios'].values())
    refs = [s['ref']['wall'] for s in result['scenarios'].values() if s['ref']]
    result['ref_total'] = sum(refs) if len(refs) == len(result['scenarios']) else None
    details.append(format_details(name, result))
    if not args.keep:
        for f in os.listdir(TESTMAP):
            if f.startswith(f'_bench_{name}.'):
                os.remove(os.path.join(TESTMAP, f))
    return result


def format_details(name, result):
    out = [f'-- {name}: initial full compile {result["initial"]:.3f}s']
    for scen, s in result['scenarios'].items():
        out.append(f'-- {name} / {scen}: {mapedit.SCENARIOS[scen][0]}')
        for e in s['edits'][:20]:
            out.append(f'     edit: {e}')
        if len(s['edits']) > 20:
            out.append(f'     ... {len(s["edits"]) - 20} more edits')
        out.append('   times: ' + ' '.join(f'{w:.3f}' for w in s['walls']) + f'  median {s["median"]:.3f}s'
                   + (f'  (server {s["server"]:.3f}s)' if s['server'] is not None else '')
                   + '  reverts: ' + ' '.join(f'{w:.3f}' for w in s['reverts']))
        out.append('   server phases (median repetition):')
        out += ['   ' + line for line in s['block'].splitlines()[1:]]
        if s['ref']:
            out.append('   reference qbsp: ' + ' '.join(f'{w:.3f}' for w in s['ref']['walls'])
                       + f'  median {s["ref"]["wall"]:.3f}s' + ('  (LEAKS)' if s['ref']['leaked'] else ''))
            out += ['   ' + line for line in s['ref']['block'].splitlines()[1:]]
        if s['check']:
            out.append('   check: ' + ('SAME' if s['check']['ok'] else 'DIFFERENT'))
            out += ['     ' + line for line in s['check']['lines']]
    return '\n'.join(out)


def comparable(a, b):
    return (a.get('method', 1) == b.get('method', 1) and a.get('scenario_version') == b.get('scenario_version')
            and a.get('qbsp_args') == b.get('qbsp_args')
            and sorted(a['maps']) == sorted(b['maps'])
            and all(sorted(a['maps'][m]['scenarios']) == sorted(b['maps'][m]['scenarios'])
                    and a['maps'][m].get('attempts') == b['maps'][m].get('attempts') for m in a['maps']))


def previous_run(record):
    if not os.path.exists(HISTORY_JSON):
        return None
    prev = None
    with open(HISTORY_JSON, encoding='utf-8') as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                if comparable(r, record):
                    prev = r
    return prev


def pct(new, old):
    return f'{100 * (new - old) / old:+.1f}%' if old else ''


# Below this, a change is treated as noise even when the repetitions agree closely: on a laptop,
# the same compile varies 7-10% from run to run (measured: stdev 0.13-0.19s on a 1.8s compile).
MIN_NOISE = 0.05
# the total adds up independent scenarios, so its noise is smaller
MIN_TOTAL_NOISE = 0.02


def total_noise(m):
    """Noise of a total: the scenarios' spreads added in quadrature, relative to the total."""
    var = sum((max(s.get('spread', 0.0), MIN_NOISE) * s['median']) ** 2 for s in m['scenarios'].values())
    return max(var ** 0.5 / m['total'], MIN_TOTAL_NOISE) if m['total'] else MIN_TOTAL_NOISE


def grand_noise(record):
    """Noise of the TOTAL: the maps' totals added in quadrature."""
    var = sum((total_noise(m) * m['total']) ** 2 for m in record['maps'].values())
    return max(var ** 0.5 / record['total'], MIN_TOTAL_NOISE) if record['total'] else MIN_TOTAL_NOISE


def change(new, old, noise, floor=MIN_NOISE):
    """'-12.3%', or '+1.0% ~' when the change is within the measured noise."""
    if not old:
        return ''
    rel = (new - old) / old
    return pct(new, old) + (' ~' if abs(rel) <= max(noise, floor) else '  ')


def print_summary(record, prev):
    print('\n' + '=' * 100)
    print(f'{"map / scenario":24} {"median":>9} {"min":>8} {"max":>8} {"spread":>7} {"server":>8} {"ref qbsp":>9} {"speedup":>8}'
          f' {"check":>6}' + (f' {"previous":>9} {"change":>8}' if prev else ''))
    for name, m in record['maps'].items():
        for scen, s in m['scenarios'].items():
            spread = s['spread']
            ref = s['ref']['wall'] if s['ref'] else None
            check = '' if not s['check'] else ('OK' if s['check']['ok'] else 'FAIL')
            if s['ref'] and s['ref']['leaked']:
                check += '+LEAK'
            line = (f'{name + " / " + scen:24} {s["median"]:8.3f}s {min(s["walls"]):7.3f}s {max(s["walls"]):7.3f}s'
                    f' {100 * spread:6.1f}% ' + (f'{s["server"]:7.3f}s ' if s['server'] is not None else ' ' * 9)
                    + (f'{ref:8.3f}s {ref / s["median"]:7.2f}x' if ref else ' ' * 18)
                    + f' {check:>6}')
            if prev:
                ps = prev['maps'][name]['scenarios'][scen]
                noise = max(spread, ps.get('spread', 0.0))
                line += f' {ps["median"]:8.3f}s {change(s["median"], ps["median"], noise):>9}'
            print(line)
        ref_total = m['ref_total']
        line = (f'{"  total " + name:24} {m["total"]:8.3f}s {"":35}'
                + (f'{ref_total:8.3f}s {ref_total / m["total"]:7.2f}x' if ref_total else ' ' * 18) + ' ' * 7)
        if prev:
            pm = prev['maps'][name]
            noise = max(total_noise(m), total_noise(pm))
            line += f' {pm["total"]:8.3f}s {change(m["total"], pm["total"], noise, MIN_TOTAL_NOISE):>9}'
        print(line)
    print('-' * 100)
    line = f'{"TOTAL":24} {record["total"]:8.3f}s {"":35}'
    if record['ref_total']:
        line += f'{record["ref_total"]:8.3f}s {record["ref_total"] / record["total"]:7.2f}x'
    if prev:
        noise = max(grand_noise(record), grand_noise(prev))
        line += (f'   previous [{prev["label"]}] {prev["total"]:.3f}s '
                 f'{change(record["total"], prev["total"], noise, MIN_TOTAL_NOISE)}')
    print(line)

    # where the server spends its time: phases summed over all scenarios (median repetitions)
    for name, m in record['maps'].items():
        totals = {}
        for s in m['scenarios'].values():
            for secs, phase in s['phases']:
                totals[phase] = totals.get(phase, 0.0) + secs
        top = sorted(totals.items(), key=lambda t: -t[1])[:10]
        whole = sum(totals.values()) or 1
        print(f'\n{name}: slowest server phases (sum over scenarios)')
        for phase, secs in top:
            print(f'   {secs:8.3f}s  {100 * secs / whole:5.1f}%  {phase}')

    if prev:
        print(f'\n(~ = within the noise: the spread of the repetitions, at least {100 * MIN_NOISE:.0f}% per scenario'
              f' and {100 * MIN_TOTAL_NOISE:.0f}% for totals)')
    if record['failed_checks']:
        print(f'\nWARNING: {record["failed_checks"]} scenario(s) produce a BSP that does not play like the reference.'
              f' See {DETAILS}')
    if record['leaks']:
        print(f'WARNING: {record["leaks"]} scenario(s) leak with the edits: their times are not representative.')


def main():
    # a crash (of Python itself) prints where it was, instead of ending the run without a word
    faulthandler.enable()
    sys.stdout.reconfigure(line_buffering=True)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', default='-', help='tag for this run in the history (what changed)')
    ap.add_argument('--maps', nargs='+', default=DEFAULT_MAPS, help=f'maps in testmap/ ("all" = {" ".join(ALL_MAPS)})')
    ap.add_argument('--scenarios', nargs='+', default=list(mapedit.SCENARIOS), choices=list(mapedit.SCENARIOS))
    ap.add_argument('--reps', type=int, default=3, help='repetitions per scenario (median)')
    ap.add_argument('--no-ref', dest='ref', action='store_false', help='skip the full qbsp reference and checks')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT)
    ap.add_argument('--qbsp-args', default='-bsp2', help='qbsp options, for the server and the reference')
    ap.add_argument('--server-args', default='', help='qbspserver-only options, e.g. -nohullcache (to compare)')
    ap.add_argument('--warmup', type=float, default=60, help='seconds of compiles before measuring (0 = none)')
    ap.add_argument('--keep', action='store_true', help='keep testmap/_bench_* work files')
    ap.add_argument('--no-log', dest='log', action='store_false', help='do not write the history (trial runs)')
    args = ap.parse_args()
    if args.maps == ['all']:
        args.maps = ALL_MAPS
    args.qbsp_args = args.qbsp_args.split()
    args.server_args = args.server_args.split()

    for exe in (SERVER, RECEIVE, QBSP):
        if not os.path.exists(exe):
            print(f'Missing {exe}. Build it: build.bat qbspserver / qbspreceive / qbsp')
            return 1
    os.makedirs(WORK, exist_ok=True)

    date = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    build = build_id()
    print(f'[bench] {date}  [{args.label}]  build {build}  maps {" ".join(args.maps)}  reps {args.reps}'
          f'  qbsp {" ".join(args.qbsp_args)}')

    details = []
    maps = {}
    start = time.perf_counter()
    for i, name in enumerate(args.maps):
        # each map's server on its own port: the previous server's port may still be held
        map_args = argparse.Namespace(**vars(args))
        map_args.port = args.port + i
        maps[name] = bench_map(name, map_args, details)

    record = {
        'date': date, 'label': args.label, 'build': build, 'scenario_version': mapedit.SCENARIO_VERSION,
        'method': METHOD_VERSION, 'reps': args.reps, 'warmup': args.warmup,
        'qbsp_args': ' '.join(args.qbsp_args), 'server_args': ' '.join(args.server_args), 'maps': maps,
        'total': sum(m['total'] for m in maps.values()),
        'ref_total': sum(m['ref_total'] for m in maps.values()) if all(m['ref_total'] for m in maps.values()) else None,
        'failed_checks': sum(1 for m in maps.values() for s in m['scenarios'].values()
                             if s['check'] and not s['check']['ok']),
        'leaks': sum(1 for m in maps.values() for s in m['scenarios'].values() if s['ref'] and s['ref']['leaked']),
        'bench_seconds': round(time.perf_counter() - start, 1),
    }
    prev = previous_run(record)
    print_summary(record, prev)

    if args.log:
        checks = 'n/a' if record['ref_total'] is None else (
            'OK' if not record['failed_checks'] else f'FAIL x{record["failed_checks"]}')
        line = (f'{date}  [{args.label}]  build={build}  m{METHOD_VERSION} v{mapedit.SCENARIO_VERSION} reps={args.reps}'
                f' qbsp="{" ".join(args.qbsp_args)}"  TOTAL={record["total"]:.3f}s')
        if record['ref_total']:
            line += f'  ref={record["ref_total"]:.3f}s  x{record["ref_total"] / record["total"]:.2f}'
        line += f'  check={checks}'
        if prev:
            noise = max(grand_noise(record), grand_noise(prev))
            line += f'  vs [{prev["label"]}] ' + change(record['total'], prev['total'], noise, MIN_TOTAL_NOISE).strip()
        for name, m in maps.items():
            line += f'  | {name} {m["total"]:.3f}: ' + ' '.join(
                f'{scen}={s["median"]:.3f}' for scen, s in m['scenarios'].items())
        with open(HISTORY, 'a', encoding='utf-8') as f:
            f.write(line + '\n')

        # drop the bulky text blocks from the json; they are in the details file
        slim = json.loads(json.dumps(record))
        for m in slim['maps'].values():
            for s in m['scenarios'].values():
                s.pop('block', None)
                if s['ref']:
                    s['ref'].pop('block', None)
        with open(HISTORY_JSON, 'a', encoding='utf-8') as f:
            f.write(json.dumps(slim) + '\n')
        with open(DETAILS, 'a', encoding='utf-8') as f:
            f.write(f'==== {date}  [{args.label}]  build={build}  reps={args.reps}  qbsp {" ".join(args.qbsp_args)}'
                    f'  TOTAL={record["total"]:.3f}s\n' + '\n'.join(details) + '\n\n')
        print(f'\nLogged to {HISTORY}')
    print(f'[bench] done in {record["bench_seconds"]:.0f}s')
    return 1 if record['failed_checks'] else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Runs the end-to-end tests: the shell tests, then every tests/cp_test.py case in parallel, each
in its own process with its own ports. A failed case keeps its cluster dir and prints its output.
Run: python3 tests/e2e.py [-j JOBS] [--shard K/N] [-k SUBSTRING]
With --shard, the shell tests run only in shard 1.
"""
import argparse
import os
import queue
import re
import shutil
import subprocess
import sys
import time
import unittest
from concurrent.futures import ThreadPoolExecutor

TESTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TESTS)
import cp_test  # noqa: E402

CP_TEST = os.path.join(TESTS, 'cp_test.py')
SHELL_TESTS = ['http_test.sh', 'node_test.sh', 'shutdown_test.sh']
# Each worker owns 100 ports from here: cp at +0, node id i at +10+i.
PORT_BASE = int(os.environ.get('KV_TEST_E2E_PORT', 10000))
CASE_TIMEOUT_S = 300


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 1)
    ap.add_argument('--shard', default='1/1', help='K/N: run every Nth case, from the Kth')
    ap.add_argument('-k', default='', help='run only cp cases whose name has this substring')
    args = ap.parse_args()
    k, n = (int(x) for x in args.shard.split('/'))

    if k == 1 and not args.k:
        for t in SHELL_TESTS:
            print(f'== {t}', flush=True)
            if subprocess.run([os.path.join(TESTS, t)]).returncode != 0:
                sys.exit(f'{t} failed')

    suite = unittest.TestLoader().loadTestsFromModule(cp_test)
    names = [t.id().split('.', 1)[1] for s in suite for t in s]
    names = [t for t in names if args.k in t][k - 1::n]

    slots = queue.Queue()
    for s in range(args.jobs):
        slots.put(s)

    def run(name):
        slot = slots.get()
        env = dict(os.environ, KV_TEST_KEEP='1', KV_TEST_CP_PORT=str(PORT_BASE + slot * 100),
                   KV_TEST_BASE_PORT=str(PORT_BASE + slot * 100 + 10))
        start = time.monotonic()
        try:
            p = subprocess.run([sys.executable, CP_TEST, name], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=CASE_TIMEOUT_S)
            out, ok = p.stdout, p.returncode == 0
        except subprocess.TimeoutExpired as e:
            out, ok = (e.stdout or b'').decode(errors='replace') + '\nTIMEOUT', False
        finally:
            slots.put(slot)
        if ok:
            for d in re.findall(r'kept (\S+)', out):
                shutil.rmtree(d, ignore_errors=True)
        print(f'{"ok  " if ok else "FAIL"} {time.monotonic() - start:6.1f}s {name}', flush=True)
        return name, ok, out

    start = time.monotonic()
    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(run, names))
    failed = [(name, out) for name, ok, out in results if not ok]
    for name, out in failed:
        print(f'\n===== {name}\n{out}', file=sys.stderr)
    print(f'\n{len(names) - len(failed)}/{len(names)} cp cases ok in '
          f'{time.monotonic() - start:.1f}s, {args.jobs} jobs')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Find the max sustainable RPS of the KV server per workload mix and profile it.

Per mix: start build/app on a fresh DB, preload the keys, then run k6 constant-arrival-rate steps
at rising rates until the first step that breaks the SLO. While each step runs, perf records a
window of the server. Writes results/<timestamp>/summary.md. See README "Benchmark".
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K6_SCRIPT = os.path.join(ROOT, 'bench', 'k6', 'kv.js')
CLK_TCK = os.sysconf('SC_CLK_TCK')


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--mix', default='95,50', help='comma-separated read %% per workload (default %(default)s)')
    p.add_argument('--addr', default='127.0.0.1:7890', help='host:port the server listens on (default %(default)s)')
    p.add_argument('--p99-ms', type=float, default=50, help='SLO: p99 latency limit (default %(default)s)')
    p.add_argument('--max-error', type=float, default=0.01, help='SLO: max failed request rate (default %(default)s)')
    p.add_argument('--max-dropped', type=float, default=0.01,
                   help='SLO: max fraction of iterations k6 could not start in time (default %(default)s)')
    p.add_argument('--start-rate', type=int, default=2000, help='first step, req/s (default %(default)s)')
    p.add_argument('--step-factor', type=float, default=1.25, help='rate multiplier per step (default %(default)s)')
    p.add_argument('--max-rate', type=int, default=500000, help='last step, req/s (default %(default)s)')
    p.add_argument('--step-duration', type=float, default=20, help='seconds per step (default %(default)s)')
    p.add_argument('--perf-seconds', type=float, default=5, help='perf record window per step (default %(default)s)')
    p.add_argument('--call-graph', default='dwarf', help='perf --call-graph mode (default %(default)s)')
    p.add_argument('--keys', type=int, default=10000)
    p.add_argument('--value-size', type=int, default=100)
    p.add_argument('--threads', help='KV_THREADS for the server (default: server default)')
    p.add_argument('--flush-ms', help='KV_FLUSH_INTERVAL_MS for the server (default: server default)')
    p.add_argument('--no-build', action='store_true', help='do not run make')
    p.add_argument('--no-perf', action='store_true', help='do not run perf record / perf stat')
    p.add_argument('--no-server', action='store_true',
                   help='do not start a server; drive k6 at the server on --addr (implies --no-build --no-perf)')
    p.add_argument('--out', help='output dir (default results/<timestamp>)')
    args = p.parse_args()
    if args.no_server:
        args.no_build = args.no_perf = True
    try:
        args.mix = [int(m) for m in args.mix.split(',')]
    except ValueError:
        p.error('--mix: want integers, e.g. 100,95,50,0')
    if any(not 0 <= m <= 100 for m in args.mix):
        p.error('--mix: each read % must be 0..100')
    if len(set(args.mix)) != len(args.mix):
        p.error('--mix: each read % only once')
    return args


def ping(base_url):
    try:
        with urllib.request.urlopen(base_url + '/', timeout=1):
            return True
    except urllib.error.HTTPError:
        return True
    except OSError:
        return False


def cpu_seconds(pid):
    with open(f'/proc/{pid}/stat') as f:
        fields = f.read().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / CLK_TCK


def context_switches(pid):
    total = 0
    for tid in os.listdir(f'/proc/{pid}/task'):
        try:
            with open(f'/proc/{pid}/task/{tid}/status') as f:
                total += sum(int(line.split()[1]) for line in f if 'ctxt_switches' in line)
        except FileNotFoundError:
            pass
    return total


def run_k6(args, base_url, read_pct, env, log_path):
    full_env = dict(os.environ, BASE_URL=base_url, READ_PCT=str(read_pct), KEYS=str(args.keys),
                    VALUE_SIZE=str(args.value_size), P99_MS=str(args.p99_ms), **env)
    log = open(log_path, 'w')
    proc = subprocess.Popen(['k6', 'run', '--quiet', K6_SCRIPT], env=full_env, stdout=log, stderr=subprocess.STDOUT)
    return proc, log


def wait_rusage(proc):
    _, status, usage = os.wait4(proc.pid, 0)
    proc.returncode = os.waitstatus_to_exitcode(status)
    return usage.ru_utime + usage.ru_stime


def metric(summary, name, key, default=None):
    return summary['metrics'].get(name, {}).get('values', {}).get(key, default)


def run_step(args, base_url, read_pct, rate, mix_dir, pid):
    summary_path = os.path.join(mix_dir, f'k6-{rate}.json')
    env = {'MODE': 'step', 'RATE': str(rate), 'DURATION': f'{args.step_duration}s', 'PRELOAD': '0',
           'SUMMARY_PATH': summary_path}
    cpu0 = cpu_seconds(pid) if pid else None
    ctx0 = context_switches(pid) if pid else None
    t0 = time.monotonic()
    proc, log = run_k6(args, base_url, read_pct, env, os.path.join(mix_dir, f'k6-{rate}.log'))
    perf_data = None
    if not args.no_perf:
        window_start = min(2.0, args.step_duration / 4)
        window = max(0.5, min(args.perf_seconds, args.step_duration - window_start - 1))
        time.sleep(window_start)
        perf_data = os.path.join(mix_dir, f'perf-{rate}.data')
        with open(os.path.join(mix_dir, 'perf-record.log'), 'a') as perf_log:
            subprocess.run(['perf', 'record', '-q', '-e', 'cycles:u', '-F', '999', '--call-graph', args.call_graph,
                            '--user-callchains', '-p', str(pid), '-o', perf_data, '--', 'sleep', str(window)],
                           stdout=subprocess.DEVNULL, stderr=perf_log)
    k6_cpu = wait_rusage(proc)
    log.close()
    wall = time.monotonic() - t0
    step = {'rate': rate, 'k6_exit': proc.returncode, 'k6_cpu_cores': k6_cpu / wall, 'perf_data': perf_data}
    if pid:
        step['server_cpu_cores'] = (cpu_seconds(pid) - cpu0) / wall
        step['ctx_switches_per_s'] = (context_switches(pid) - ctx0) / wall
    if not os.path.exists(summary_path):
        step.update(ok=False, reason=f'k6 exit {proc.returncode}, no summary')
        return step
    with open(summary_path) as f:
        s = json.load(f)
    iterations = metric(s, 'iterations', 'count', 0)
    dropped = metric(s, 'dropped_iterations', 'count', 0)
    step.update(
        achieved_rps=metric(s, 'iterations', 'rate', 0),
        error_rate=metric(s, 'http_req_failed{phase:main}', 'rate', 0),
        dropped_frac=dropped / max(1, iterations + dropped),
        latency={op: {q: metric(s, f'http_req_duration{{{tag}}}', q) for q in ('med', 'p(95)', 'p(99)')}
                 for op, tag in (('all', 'phase:main'), ('read', 'op:read'), ('write', 'op:write'))},
    )
    p99 = step['latency']['all']['p(99)'] or 0
    reasons = []
    if step['error_rate'] >= args.max_error:
        reasons.append(f"errors {step['error_rate']:.2%}")
    if p99 >= args.p99_ms:
        reasons.append(f'p99 {p99:.1f} ms')
    if step['dropped_frac'] > args.max_dropped:
        reasons.append(f"dropped {step['dropped_frac']:.2%}")
    if proc.returncode not in (0, 99):
        reasons.append(f'k6 exit {proc.returncode}')
    step.update(ok=not reasons, reason=', '.join(reasons))
    return step


def perf_outputs(args, step, label, mix_dir):
    data = step['perf_data']
    if not data or not os.path.exists(data) or os.path.getsize(data) == 0:
        return None
    kept = os.path.join(mix_dir, f'perf-{label}.data')
    os.replace(data, kept)
    step['perf_data'] = kept
    report = subprocess.run(['perf', 'report', '-i', kept, '--stdio', '--no-children', '-g', 'none',
                             '-F', 'overhead,dso,sym', '--percent-limit', '0.2'],
                            capture_output=True, text=True).stdout
    with open(os.path.join(mix_dir, f'report-{label}.txt'), 'w') as f:
        f.write(report)
    top = re.findall(r'^\s+([\d.]+)%\s+(\S+)\s+\[.\]\s+(.+?)\s*$', report, re.M)[:10]
    flame = None
    if shutil.which('inferno-collapse-perf') and shutil.which('inferno-flamegraph'):
        tools = (['inferno-collapse-perf'], ['inferno-flamegraph'])
    elif shutil.which('stackcollapse-perf.pl') and shutil.which('flamegraph.pl'):
        tools = (['stackcollapse-perf.pl'], ['flamegraph.pl'])
    else:
        tools = None
    if tools:
        flame = os.path.join(mix_dir, f'flame-{label}.svg')
        script = subprocess.run(['perf', 'script', '-i', kept], capture_output=True).stdout
        folded = subprocess.run(tools[0], input=script, capture_output=True).stdout
        with open(flame, 'wb') as f:
            f.write(subprocess.run(tools[1], input=folded, capture_output=True).stdout)
    return {'step': step['rate'], 'label': label, 'top': top, 'flame': flame}


def run_mix(args, read_pct, run_dir):
    mix_dir = os.path.join(run_dir, f'mix-{read_pct}')
    os.makedirs(mix_dir, exist_ok=True)
    base_url = f'http://{args.addr}'
    result = {'mix': read_pct, 'steps': [], 'profiles': [], 'server_exit': None, 'perf_stat': None}
    server = perf_stat = None
    tmp = tempfile.mkdtemp(prefix='kv-bench-')
    try:
        if not args.no_server:
            if ping(base_url):
                sys.exit(f'profile: a server already answers on {base_url}; stop it or pass --addr/--no-server')
            env = dict(os.environ, KV_LISTEN_ADDR=args.addr, KV_DB_PATH=os.path.join(tmp, 'kv.db'),
                       KV_LOG_PATH=os.path.join(mix_dir, 'kv.log'))
            if args.threads:
                env['KV_THREADS'] = args.threads
            if args.flush_ms:
                env['KV_FLUSH_INTERVAL_MS'] = args.flush_ms
            server = subprocess.Popen([os.path.join(ROOT, 'build', 'app')], env=env,
                                      stdout=open(os.path.join(mix_dir, 'server.out'), 'w'), stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 10
            while not ping(base_url):
                if server.poll() is not None or time.monotonic() > deadline:
                    sys.exit(f'profile: server did not start, see {mix_dir}/server.out')
                time.sleep(0.1)
        elif not ping(base_url):
            sys.exit(f'profile: no server answers on {base_url}')

        print(f'[mix {read_pct}] preload {args.keys} keys', flush=True)
        proc, log = run_k6(args, base_url, read_pct, {'MODE': 'step', 'RATE': '1', 'DURATION': '1s', 'PRELOAD': '1'},
                           os.path.join(mix_dir, 'k6-preload.log'))
        wait_rusage(proc)
        log.close()
        if proc.returncode != 0:
            sys.exit(f'profile: preload failed, see {mix_dir}/k6-preload.log')

        pid = server.pid if server else None
        if not args.no_perf:
            perf_stat_path = os.path.join(mix_dir, 'perf-stat.csv')
            perf_stat = subprocess.Popen(['perf', 'stat', '-x', ',', '-e', 'cycles:u,instructions:u,task-clock',
                                          '-p', str(pid), '-o', perf_stat_path],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rate = args.start_rate
        while rate <= args.max_rate:
            step = run_step(args, base_url, read_pct, rate, mix_dir, pid)
            result['steps'].append(step)
            p99 = (step.get('latency') or {}).get('all', {}).get('p(99)') or 0
            print(f"[mix {read_pct}] {rate:>7} req/s: achieved {step.get('achieved_rps', 0):9.0f}  "
                  f"p99 {p99:7.2f} ms  {'ok' if step['ok'] else 'FAIL ' + step['reason']}", flush=True)
            if not step['ok']:
                break
            rate = max(rate + 1, round(rate * args.step_factor))

        if perf_stat:
            perf_stat.send_signal(signal.SIGINT)
            perf_stat.wait()
            perf_stat = None
            result['perf_stat'] = {}
            with open(perf_stat_path) as f:
                for line in f:
                    cols = line.strip().split(',')
                    if len(cols) > 2 and cols[0] and not line.startswith('#'):
                        try:
                            result['perf_stat'][cols[2]] = float(cols[0])
                        except ValueError:
                            pass
        if server:
            result['ctx_switches_total'] = context_switches(server.pid)
            server.send_signal(signal.SIGTERM)
            try:
                result['server_exit'] = server.wait(timeout=30)
            except subprocess.TimeoutExpired:
                server.kill()
                result['server_exit'] = 'killed after 30 s'
            server = None

        passed = [s for s in result['steps'] if s['ok']]
        failed = [s for s in result['steps'] if not s['ok']]
        result['sustained'] = passed[-1] if passed else None
        if not args.no_perf:
            for step, label in ((result['sustained'], 'sustained'), (failed[0] if failed else None, 'saturation')):
                if step:
                    profile = perf_outputs(args, step, label, mix_dir)
                    if profile:
                        result['profiles'].append(profile)
            for step in result['steps']:
                if step['perf_data'] and step['perf_data'].endswith(f"-{step['rate']}.data"):
                    if os.path.exists(step['perf_data']):
                        os.remove(step['perf_data'])
                    step['perf_data'] = None
        with open(os.path.join(mix_dir, 'steps.json'), 'w') as f:
            json.dump(result, f, indent=2)
        return result
    finally:
        for p in (perf_stat, server):
            if p and p.poll() is None:
                p.kill()
        shutil.rmtree(tmp, ignore_errors=True)


def fmt(v, spec='.2f', unit=''):
    return 'n/a' if v is None else f'{v:{spec}}{unit}'


def write_summary(args, run_dir, results):
    lines = [f'# KV-Store benchmark {os.path.basename(run_dir)}', '',
             f"Target `http://{args.addr}`{' (external server)' if args.no_server else ''}. "
             f'SLO per step: p99 < {args.p99_ms:g} ms, errors < {args.max_error:.0%}, '
             f'dropped iterations <= {args.max_dropped:.0%}. Steps: {args.step_duration:g} s each, from '
             f'{args.start_rate} req/s x{args.step_factor:g}. Keys {args.keys} (Zipf s=0.99), values '
             f'{args.value_size} B. Server threads: {args.threads or "default"}, flush: '
             f'{args.flush_ms or "default"} ms. Host CPUs: {os.cpu_count()}.', '',
             '"Max sustainable RPS" = rate of the last step that met the SLO. Latency and CPU columns are '
             'from that step. Times in ms.', '',
             '| | ' + ' | '.join(f"{r['mix']}% read" for r in results) + ' |',
             '|---|' + '---|' * len(results)]

    def row(name, get):
        lines.append(f'| {name} | ' + ' | '.join(get(r) for r in results) + ' |')

    sus = {r['mix']: r['sustained'] or {} for r in results}
    row('Max sustainable RPS', lambda r: fmt(sus[r['mix']].get('achieved_rps'), '.0f') +
        (f" (target {sus[r['mix']]['rate']})" if sus[r['mix']] else ' (no step passed)'))
    row('First failing step', lambda r: next((f"{s['rate']}: {s['reason']}" for s in r['steps'] if not s['ok']),
                                             'none (hit --max-rate)'))
    for op in ('all', 'read', 'write'):
        row(f'{op} p50 / p95 / p99', lambda r, op=op: ' / '.join(
            fmt(((sus[r['mix']].get('latency') or {}).get(op) or {}).get(q)) for q in ('med', 'p(95)', 'p(99)')))
    row('Error rate', lambda r: fmt(sus[r['mix']].get('error_rate'), '.3%'))
    row('Server CPU (cores)', lambda r: fmt(sus[r['mix']].get('server_cpu_cores')))
    row('k6 CPU (cores)', lambda r: fmt(sus[r['mix']].get('k6_cpu_cores')))
    row('Context switches/s', lambda r: fmt(sus[r['mix']].get('ctx_switches_per_s'), '.0f'))
    row('Context switches (run)', lambda r: fmt(r.get('ctx_switches_total'), '.0f'))

    def ipc(r):
        ps = r['perf_stat'] or {}
        return fmt(ps['instructions:u'] / ps['cycles:u'] if ps.get('cycles:u') and 'instructions:u' in ps else None)
    row('IPC (user, whole run)', ipc)
    row('Server exit status', lambda r: 'n/a' if r['server_exit'] is None else str(r['server_exit']))
    lines.append('')
    lines.append('If k6 CPU is close to the free CPUs of the host, k6 is the limit, not the server: run k6 on a '
                 'second host (`--no-server --addr`).')

    for r in results:
        lines += ['', f"## {r['mix']}% read: steps", '',
                  '| Target | Achieved | p99 all | p99 read | p99 write | Errors | Dropped | Server CPU | k6 CPU | Result |',
                  '|---|---|---|---|---|---|---|---|---|---|']
        for s in r['steps']:
            lat = s.get('latency') or {}
            lines.append(f"| {s['rate']} | {fmt(s.get('achieved_rps'), '.0f')} | "
                         f"{fmt((lat.get('all') or {}).get('p(99)'))} | {fmt((lat.get('read') or {}).get('p(99)'))} | "
                         f"{fmt((lat.get('write') or {}).get('p(99)'))} | {fmt(s.get('error_rate'), '.2%')} | "
                         f"{fmt(s.get('dropped_frac'), '.2%')} | {fmt(s.get('server_cpu_cores'))} | "
                         f"{fmt(s.get('k6_cpu_cores'))} | {'ok' if s['ok'] else 'FAIL: ' + s['reason']} |")
        if args.no_perf:
            continue
        if not r['profiles']:
            lines += ['', f'No perf profile. See `mix-{r["mix"]}/perf-record.log`.']
        for prof in r['profiles']:
            lines += ['', f"### {r['mix']}% read: top 10 hotspots at {prof['step']} req/s ({prof['label']})", '',
                      f"Full list: `mix-{r['mix']}/report-{prof['label']}.txt`, data: "
                      f"`mix-{r['mix']}/perf-{prof['label']}.data`. "
                      + (f"Flame graph: `{os.path.relpath(prof['flame'], run_dir)}`." if prof['flame'] else
                         'No flame graph: install inferno (`cargo install inferno`) or FlameGraph '
                         '(stackcollapse-perf.pl, flamegraph.pl) on PATH.'), '',
                      '| Self % | Object | Symbol |', '|---|---|---|']
            lines += [f'| {pct} | {dso} | `{sym}` |' for pct, dso, sym in prof['top']]

    rps = {r['mix']: (r['sustained'] or {}).get('achieved_rps', 0) for r in results}
    best = max(rps, key=rps.get)
    if len(rps) > 1 and len({(r['sustained'] or {}).get('rate') for r in results}) == 1:
        higher = (f'no clear winner: all mixes passed the same step (step size x{args.step_factor:g}); use a '
                  'smaller --step-factor to separate them')
    else:
        higher = f'**{best}% read** ({rps[best]:.0f} req/s)'
    lines += ['', '## Verdict', '',
              f'- Higher max sustainable RPS: {higher}.',
              '- Limit at saturation (server CPU, k6 CPU, write p99, lock or writer hotspots): _fill in_',
              '- Workload that fits the server, and why: _fill in_',
              '- Next change to try: _fill in_', '']
    path = os.path.join(run_dir, 'summary.md')
    with open(path, 'w') as f:
        f.write('\n'.join(lines))
    return path


def main():
    args = parse_args()
    if not shutil.which('k6'):
        sys.exit('profile: k6 not found on PATH')
    if not args.no_perf and not shutil.which('perf'):
        sys.exit('profile: perf not found on PATH (or pass --no-perf)')
    run_dir = args.out or os.path.join(ROOT, 'results', time.strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    if not args.no_build:
        subprocess.run(['make', '-C', ROOT, '-s'], check=True)
    results = [run_mix(args, mix, run_dir) for mix in args.mix]
    print(write_summary(args, run_dir, results))


if __name__ == '__main__':
    main()

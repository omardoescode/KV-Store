#!/usr/bin/env python3
"""Measure throughput and latency at 1, 2, and 3 storage nodes.

--step finds the max sustainable rate (the knee) per setup; --rates runs fixed rates for latency.
--driver process forks cp and nodes on the host; --driver dind runs cp and node containers inside
one resource-limited Docker-in-Docker container, with k6 on the host.
"""

import argparse
import contextlib
import csv
import http.client
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import tempfile
import time
from datetime import datetime
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CP_BIN = os.path.join(ROOT, "build", "kv-cp-proc")
NODE_BIN = os.path.join(ROOT, "build", "app")
K6_SCRIPT = os.path.join(ROOT, "bench", "k6", "kv.js")
CLK_TCK = os.sysconf("SC_CLK_TCK")
# Logical CPUs i and i+6 share a physical core on a 6-core/12-thread host. k6 gets two cores so the
# load generator is not the limit; cp and each node get one core for their two threads.
PIN_K6 = "0,1,6,7"
PIN_CP = "2,8"
PIN_NODES = ["3,9", "4,10", "5,11"]
DIND_NAME = "kv-bench-dind"
RELAY_IMAGE = "alpine/socat:1.8.1.1"
NODE_EVENT = re.compile(r"SUSPECT|DEAD|spawn|RESPAWN|adding a node|migration")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--driver", choices=["process", "dind"], default="process")
    parser.add_argument("--nodes", default="1,2,3", help="comma-separated node counts")
    parser.add_argument("--replicas", default="1",
                        help="comma-separated N per setup: a number, or n for N = node count")
    parser.add_argument("--mix", default="95,50", help="comma-separated read percentages")
    parser.add_argument("--rates", default="", help="comma-separated fixed k6 req/s")
    parser.add_argument("--step", action="store_true", help="find the max sustainable rate")
    parser.add_argument("--closed", default="",
                        help="comma-separated VU counts for closed-loop max-throughput runs through cp")
    parser.add_argument("--baseline", action="store_true",
                        help="at 1 node, also run each --rates value straight to the node and "
                             "through a plain TCP relay")
    parser.add_argument("--start-rate", type=int, default=2000)
    parser.add_argument("--step-factor", type=float, default=1.25)
    parser.add_argument("--max-rate", type=int, default=200000)
    parser.add_argument("--p99-ms", type=float, default=50, help="SLO: p99 latency limit")
    parser.add_argument("--max-error", type=float, default=0.01, help="SLO: max failed rate")
    parser.add_argument("--max-dropped", type=float, default=0.01,
                        help="SLO: max fraction of iterations k6 could not start in time")
    parser.add_argument("--duration", type=float, default=30, help="seconds per k6 run")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--no-pin", action="store_true", help="do not pin processes to CPUs")
    parser.add_argument("--keys", type=int, default=10000)
    parser.add_argument("--value-size", type=int, default=100)
    parser.add_argument("--cp-port", type=int,
                        help="cp port on the host (default 7980 process, 8080 dind)")
    parser.add_argument("--base-port", type=int, default=7990)
    parser.add_argument("--out", help="output directory")
    parser.add_argument("--dind-image", default="docker:29.9.2-dind")
    parser.add_argument("--cpus", default="6", help="dind: CPU limit of the DinD container")
    parser.add_argument("--memory", default="8g", help="dind: memory limit of the DinD container")
    args = parser.parse_args()
    args.cp_port = args.cp_port or (7980 if args.driver == "process" else 8080)
    if args.driver == "dind":
        args.no_pin = True
    args.nodes = [int(v) for v in args.nodes.split(",")]
    args.replicas = args.replicas.split(",")
    args.rates = [int(v) for v in args.rates.split(",") if v]
    args.mix = [int(v) for v in args.mix.split(",")]
    args.closed = [int(v) for v in args.closed.split(",") if v]
    if any(n < 1 or n > 3 for n in args.nodes):
        parser.error("--nodes: only 1, 2, or 3 are supported")
    if any(r != "n" and not r.isdigit() for r in args.replicas):
        parser.error("--replicas: want numbers or n")
    if any(r <= 0 for r in args.rates):
        parser.error("--rates must contain positive integers")
    if any(m < 0 or m > 100 for m in args.mix):
        parser.error("--mix read percentages must be 0..100")
    if any(v <= 0 for v in args.closed):
        parser.error("--closed must contain positive integers")
    if not args.step and not args.rates and not args.closed:
        parser.error("give --step, --rates, --closed, or a mix of them")
    if args.baseline and not args.rates:
        parser.error("--baseline needs --rates")
    if not args.no_pin and os.cpu_count() != 12:
        parser.error("pinning assumes 12 logical CPUs; use --no-pin")
    return args


def wait_active(port, count, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/_cp/nodes")
            nodes = json.loads(conn.getresponse().read())["nodes"]
            conn.close()
            if sum(1 for n in nodes if n["state"] == "ACTIVE") == count:
                return
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(0.05)
    raise RuntimeError(f"control plane on port {port} did not get {count} ACTIVE nodes")


def node_pids(node_dir):
    pids = []
    for name in sorted(os.listdir(node_dir)):
        try:
            with open(os.path.join(node_dir, name, "pid")) as stream:
                pids.append(int(stream.read()))
        except (OSError, ValueError):
            pass
    return pids


def kill_nodes(node_dir):
    # The process driver lets nodes outlive cp, and the next setup reuses their ports.
    if not os.path.isdir(node_dir):
        return
    for pid in node_pids(node_dir):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def cpu_seconds(pid):
    with open(f"/proc/{pid}/stat") as stream:
        fields = stream.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / CLK_TCK


def cgroup_cpu_seconds(group):
    with open(os.path.join(group, "cpu.stat")) as stream:
        return int(stream.readline().split()[1]) / 1e6


def metric(summary, name, key, default=None):
    return summary["metrics"].get(name, {}).get("values", {}).get(key, default)


def run_k6(args, read_pct, env, cpu, url=None):
    full_env = dict(os.environ, BASE_URL=url or f"http://127.0.0.1:{args.cp_port}", READ_PCT=str(read_pct),
                    KEYS=str(args.keys), VALUE_SIZE=str(args.value_size),
                    P99_MS=str(args.p99_ms), **env)
    cmd = ["k6", "run", "--quiet", K6_SCRIPT]
    if not args.no_pin:
        cmd = ["taskset", "-c", PIN_K6] + cmd
    def sample():
        # A node that dies mid-run takes its counter with it; the node event check flags the run.
        try:
            return cpu()
        except OSError:
            return {}

    cpu0 = sample()
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=ROOT, env=full_env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        _, status, usage = os.wait4(proc.pid, 0)
    except KeyboardInterrupt:
        proc.kill()
        proc.wait()
        raise
    wall = time.monotonic() - t0
    cpu1 = sample()
    return {
        "k6_exit": os.waitstatus_to_exitcode(status),
        "k6_cpu": (usage.ru_utime + usage.ru_stime) / wall,
        **{key: (cpu1[key] - cpu0[key]) / wall for key in cpu1 if key in cpu0},
    }


def cp_nodes(port):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    conn.request("GET", "/_cp/nodes")
    nodes = json.loads(conn.getresponse().read())["nodes"]
    conn.close()
    return nodes


def measure(args, read_pct, rate, summary_path, cluster, url=None, vus=None):
    started_at = datetime.now().isoformat(timespec="seconds")
    load1 = os.getloadavg()[0]
    nodes0 = cluster.nodes()
    log_mark = cluster.log_mark()
    # Enough VUs for 100 ms stalls, so dropped iterations mean saturation, not a k6 VU shortage.
    load = {"MODE": "closed", "VUS": str(vus)} if vus else {
        "MODE": "step", "RATE": str(rate), "PRE_VUS": str(max(100, rate // 10)),
        "MAX_VUS": str(max(200, rate // 5))}
    row = run_k6(args, read_pct, {**load, "DURATION": f"{args.duration}s", "PRELOAD": "0",
                                  "SUMMARY_PATH": summary_path}, cluster.cpu, url)
    nodes1 = cluster.nodes()
    events = [f"node {k}: {nodes0.get(k)} -> {nodes1.get(k)}"
              for k in sorted(nodes0.keys() | nodes1.keys(), key=str) if nodes0.get(k) != nodes1.get(k)]
    events += [line.strip() for line in cluster.log_since(log_mark).splitlines()
               if NODE_EVENT.search(line)]
    row.update(rate=rate, vus=vus, started_at=started_at, load1=load1,
               summary_file=os.path.basename(summary_path), node_events=len(events),
               node_event_detail="; ".join(events)[:500])
    if not os.path.exists(summary_path):
        row.update(ok=False, reason=f"k6 exit {row['k6_exit']}, no summary" + (", node_event" if events else ""))
        return row
    with open(summary_path) as stream:
        summary = json.load(stream)
    iterations = metric(summary, "iterations", "count", 0)
    dropped = metric(summary, "dropped_iterations", "count", 0)
    row.update(
        achieved_rps=metric(summary, "iterations", "rate", 0),
        avg_ms=metric(summary, "http_req_duration{phase:main}", "avg"),
        error_pct=(metric(summary, "http_req_failed{phase:main}", "rate", 0) or 0) * 100,
        dropped_pct=dropped / max(1, iterations + dropped) * 100,
    )
    for op, tag in (("", "phase:main"), ("read_", "op:read"), ("write_", "op:write")):
        for q, name in (("med", "p50"), ("p(95)", "p95"), ("p(99)", "p99")):
            row[f"{op}{name}_ms"] = metric(summary, f"http_req_duration{{{tag}}}", q)
    reasons = []
    if row["error_pct"] >= args.max_error * 100:
        reasons.append(f"errors {row['error_pct']:.2f}%")
    if (row["p99_ms"] or 0) >= args.p99_ms:
        reasons.append(f"p99 {row['p99_ms']:.1f} ms")
    if row["dropped_pct"] > args.max_dropped * 100:
        reasons.append(f"dropped {row['dropped_pct']:.2f}%")
    if row["k6_exit"] not in (0, 99):
        reasons.append(f"k6 exit {row['k6_exit']}")
    if events:
        reasons.append("node_event")
    row.update(ok=not reasons, reason=", ".join(reasons))
    return row


@contextlib.contextmanager
def process_cluster(args, node_count, replicas):
    temp_dir = tempfile.mkdtemp(prefix="kv-cluster-bench-")
    node_dir = os.path.join(temp_dir, "nodes")
    log_path = os.path.join(temp_dir, "cp.log")
    env = dict(os.environ)
    env.update({
        "KV_CP_LISTEN_ADDR": f"127.0.0.1:{args.cp_port}",
        "KV_CP_DB_PATH": os.path.join(temp_dir, "cp.db"),
        "KV_CP_LOG_PATH": log_path,
        "KV_CP_THREADS": "2",
        "KV_CP_INITIAL_NODES": str(node_count),
        "KV_CP_MAX_NODES": str(node_count),
        "KV_CP_REPLICAS": str(replicas),
        "KV_CP_SCALE_RPS": "0",
        "KV_CP_NODE_BASE_PORT": str(args.base_port),
        "KV_CP_NODE_BIN": NODE_BIN,
        "KV_CP_NODE_DIR": node_dir,
        "KV_CP_PROBE_INTERVAL_MS": "100",
        "KV_CP_PROBE_FAILS": "3",
    })
    cmd = [CP_BIN] if args.no_pin else ["taskset", "-c", PIN_CP, CP_BIN]
    cp = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    relay = None
    try:
        wait_active(args.cp_port, node_count)
        pids = node_pids(node_dir)
        if not args.no_pin:
            # Nodes are forked by cp and start on its CPUs.
            for pid, cpus in zip(pids, PIN_NODES):
                subprocess.run(["taskset", "-a", "-pc", cpus, str(pid)], check=True,
                               stdout=subprocess.DEVNULL)

        def nodes():
            try:
                snap = {n["id"]: f"{n['state']} {n['addr']} ring={n['in_ring']}"
                        for n in cp_nodes(args.cp_port)}
            except (OSError, ValueError, KeyError) as e:
                snap = {"cp": f"unreachable ({e})"}
            for name in sorted(os.listdir(node_dir)):
                with contextlib.suppress(OSError):
                    with open(os.path.join(node_dir, name, "pid")) as stream:
                        snap[name] = "pid " + stream.read().strip()
            return snap

        def log_since(mark):
            with open(log_path, errors="replace") as stream:
                stream.seek(mark)
                return stream.read()

        cluster = SimpleNamespace(
            cpu=lambda: {"cp_cpu": cpu_seconds(cp.pid),
                         "nodes_cpu": sum(cpu_seconds(p) for p in pids)},
            nodes=nodes, log_mark=lambda: os.path.getsize(log_path), log_since=log_since)
        if args.baseline and node_count == 1:
            addr = cp_nodes(args.cp_port)[0]["addr"]
            relay_port = args.cp_port + 1
            cmd = ["socat", f"TCP-LISTEN:{relay_port},bind=127.0.0.1,fork,reuseaddr", f"TCP:{addr}"]
            relay = subprocess.Popen(cmd if args.no_pin else ["taskset", "-c", PIN_CP] + cmd,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            cluster.direct_url = f"http://{addr}"
            cluster.relay_url = f"http://127.0.0.1:{relay_port}"
            wait_port(relay_port)
        yield cluster
    finally:
        if relay:
            relay.kill()
            relay.wait()
        if cp.poll() is None:
            cp.send_signal(signal.SIGTERM)
            try:
                cp.wait(30)
            except subprocess.TimeoutExpired:
                cp.kill()
                cp.wait()
        kill_nodes(node_dir)
        shutil.rmtree(temp_dir, ignore_errors=True)


def wait_port(port, timeout=10):
    deadline = time.monotonic() + timeout
    while True:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/key_0")
            conn.getresponse().read()
            conn.close()
            return
        except OSError:
            if time.monotonic() > deadline:
                raise RuntimeError(f"nothing answers HTTP on port {port}")
            time.sleep(0.1)


@contextlib.contextmanager
def dind_cluster(args, node_count, replicas):
    docker = ["docker", "exec", DIND_NAME, "docker"]
    iptables = []
    try:
        subprocess.run(docker + ["network", "create", "kv-net"], check=True,
                       stdout=subprocess.DEVNULL)
        cp_id = subprocess.run(
            docker + ["run", "-d", "--name", "kv-cp", "--network", "kv-net", "-p", "7890:7890",
                      "-v", "/var/run/docker.sock:/var/run/docker.sock",
                      "-e", "KV_CP_THREADS=2",
                      "-e", f"KV_CP_INITIAL_NODES={node_count}",
                      "-e", f"KV_CP_MAX_NODES={node_count}",
                      "-e", f"KV_CP_REPLICAS={replicas}",
                      "-e", "KV_CP_SCALE_RPS=0",
                      "-e", "KV_CP_PROBE_INTERVAL_MS=100",
                      "-e", "KV_CP_PROBE_FAILS=3",
                      "kv-control"],
            check=True, capture_output=True, text=True).stdout.strip()
        wait_active(args.cp_port, node_count)
        list_nodes = docker + ["ps", "--no-trunc", "--filter", "label=kv.role=node",
                               "--format", '{{.Label "kv.id"}} {{.ID}}']
        node_ids = [line.split()[1] for line in subprocess.run(
            list_nodes, check=True, capture_output=True, text=True).stdout.splitlines()]

        def nodes():
            try:
                snap = {n["id"]: f"{n['state']} {n['addr']} ring={n['in_ring']}"
                        for n in cp_nodes(args.cp_port)}
            except (OSError, ValueError, KeyError) as e:
                snap = {"cp": f"unreachable ({e})"}
            for line in subprocess.run(list_nodes, capture_output=True, text=True).stdout.splitlines():
                node_id, container = line.split()
                snap[f"container {node_id}"] = container[:12]
            return snap

        # The nested daemon uses cgroupfs, so its containers sit under docker/<id> in the
        # DinD container's cgroup, which the host can read directly.
        nested = os.path.join(args.dind_cgroup, "docker")
        cluster = SimpleNamespace(
            cpu=lambda: {
                "cp_cpu": cgroup_cpu_seconds(os.path.join(nested, cp_id)),
                "nodes_cpu": sum(cgroup_cpu_seconds(os.path.join(nested, i)) for i in node_ids),
                "dind_cpu": cgroup_cpu_seconds(args.dind_cgroup),
            },
            nodes=nodes, log_mark=time.time,
            log_since=lambda mark: subprocess.run(
                docker + ["logs", "--since", f"{mark:.6f}", "kv-cp"],
                capture_output=True, text=True, errors="replace").stdout)
        if args.baseline and node_count == 1:
            addr = cp_nodes(args.cp_port)[0]["addr"]
            ip = addr.split(":")[0]
            # Direct: kernel NAT from the DinD container to node-1, the same way its -p publish
            # reaches cp. DOCKER-USER lets it past Docker's drop for unpublished container ports.
            iptables = [["-t", "nat", "PREROUTING", "-p", "tcp", "--dport", "7891", "-j", "DNAT",
                         "--to-destination", addr],
                        ["-t", "filter", "DOCKER-USER", "-d", ip, "-p", "tcp", "--dport", "7890",
                         "-j", "ACCEPT"]]
            for rule in iptables:
                subprocess.run(["docker", "exec", DIND_NAME, "iptables", rule[0], rule[1], "-I"]
                               + rule[2:], check=True)
            subprocess.run(docker + ["run", "-d", "--name", "kv-relay", "--network", "kv-net",
                                     "-p", "7892:7892", RELAY_IMAGE,
                                     "TCP-LISTEN:7892,fork,reuseaddr", f"TCP:{addr}"],
                           check=True, stdout=subprocess.DEVNULL)
            cluster.direct_url = f"http://127.0.0.1:{args.cp_port + 1}"
            cluster.relay_url = f"http://127.0.0.1:{args.cp_port + 2}"
            wait_port(args.cp_port + 1)
            wait_port(args.cp_port + 2)
        yield cluster
    finally:
        for rule in iptables:
            subprocess.run(["docker", "exec", DIND_NAME, "iptables", rule[0], rule[1], "-D"] + rule[2:],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # cp goes first, else it respawns the nodes we remove.
        subprocess.run(["docker", "exec", DIND_NAME, "sh", "-c",
                        "docker rm -f -v kv-cp; ids=$(docker ps -aq); "
                        "[ -z \"$ids\" ] || docker rm -f -v $ids; "
                        "docker volume prune -a -f; docker network rm kv-net"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_setup(args, node_count, replicas, repeat, out, rows):
    make_cluster = process_cluster if args.driver == "process" else dind_cluster
    with make_cluster(args, node_count, replicas) as cluster:
        run_k6(args, 100, {"MODE": "step", "RATE": "1", "DURATION": "1s", "PRELOAD": "1"}, cluster.cpu)
        quorum = int(replicas) // 2 + 1
        base = {"repeat": repeat, "nodes": node_count, "replicas": replicas,
                "write_quorum": quorum, "read_quorum": quorum}
        # Baselines run right before the cp run at the same rate, so host noise hits all three alike.
        targets = [("rate", None)]
        if hasattr(cluster, "direct_url"):
            targets = [("direct", cluster.direct_url), ("relay", cluster.relay_url)] + targets
        for read_pct in args.mix:
            if args.step:
                rate = args.start_rate
                while rate <= args.max_rate:
                    name = f"r{repeat}-n{node_count}-N{replicas}-mix{read_pct}-step-{rate}.json"
                    row = measure(args, read_pct, rate, os.path.join(out, name), cluster)
                    rows.append({**base, "read_pct": read_pct, "kind": "step", **row})
                    print(f"{name}: {'ok' if row['ok'] else 'FAIL ' + row['reason']}", flush=True)
                    if not row["ok"]:
                        break
                    rate = max(rate + 1, round(rate * args.step_factor))
            for rate in args.rates:
                for kind, url in targets:
                    name = f"r{repeat}-n{node_count}-N{replicas}-mix{read_pct}-{kind}-{rate}.json"
                    row = measure(args, read_pct, rate, os.path.join(out, name), cluster, url)
                    rows.append({**base, "read_pct": read_pct, "kind": kind, **row})
                    print(f"{name}: {'ok' if row['ok'] else 'FAIL ' + row['reason']}", flush=True)
            for vus in args.closed:
                name = f"r{repeat}-n{node_count}-N{replicas}-mix{read_pct}-closed-{vus}vu.json"
                row = measure(args, read_pct, None, os.path.join(out, name), cluster, vus=vus)
                rows.append({**base, "read_pct": read_pct, "kind": "closed", **row})
                print(f"{name}: {'ok' if row['ok'] else 'FAIL ' + row['reason']}", flush=True)


def fmt(value, spec=".2f"):
    return "" if value is None else format(value, spec)


def write_summary(args, out, rows):
    setups = sorted({(r["nodes"], r["replicas"], r["read_pct"]) for r in rows},
                    key=lambda s: (s[2] * -1, s[1] != 1, s[0]))
    dind = args.driver == "dind"
    if dind:
        setup = (f"Driver: dind ({args.dind_image}, --cpus {args.cpus} --memory {args.memory}; cp and node "
                 f"containers inside it, k6 on the host, no pinning). DinD CPU = the whole DinD container")
    else:
        setup = f"Driver: process. Pinning: {'off' if args.no_pin else 'k6 ' + PIN_K6 + ', cp ' + PIN_CP + ', nodes ' + ' / '.join(PIN_NODES)}"
    dind_head, dind_sep = (" DinD CPU |", "---:|") if dind else ("", "")
    lines = ["# KV cluster benchmark", "",
             f"Host CPUs: {os.cpu_count()}. {setup}. "
             f"{args.repeats} repeat(s); each cell is the median over repeats. {args.duration:g} s per run, "
             f"{args.keys} keys (Zipf s=0.99), {args.value_size}-byte values. "
             f"SLO: p99 < {args.p99_ms:g} ms, errors < {args.max_error:.0%}, dropped <= {args.max_dropped:.0%}.", ""]
    if args.step:
        lines += ["## Max sustainable rate", "",
                  f"Steps from {args.start_rate} req/s x{args.step_factor:g}. Knee = last passing step.", "",
                  f"| Read % | Nodes | N | Knee req/s (each repeat) | Median knee | p99 ms at knee | cp CPU | nodes CPU |{dind_head} k6 CPU | First fail |",
                  f"|---:|---:|---:|---|---:|---:|---:|---:|{dind_sep}---:|---|"]
        for nodes, replicas, read_pct in setups:
            knees, at_knee, fails = [], [], []
            for repeat in range(1, args.repeats + 1):
                steps = [r for r in rows if (r["repeat"], r["nodes"], r["replicas"], r["read_pct"], r["kind"])
                         == (repeat, nodes, replicas, read_pct, "step")]
                passed = [r for r in steps if r["ok"]]
                knees.append(passed[-1]["rate"] if passed else 0)
                if passed:
                    at_knee.append(passed[-1])
                fails += [f"{r['rate']}: {r['reason']}" for r in steps if not r["ok"]]
            med = lambda key: statistics.median(r[key] for r in at_knee) if at_knee else None
            dind_cell = f" {fmt(med('dind_cpu'))} |" if dind else ""
            lines.append(f"| {read_pct} | {nodes} | {replicas} | {', '.join(map(str, knees))} | "
                         f"{statistics.median(knees):g} | {fmt(med('p99_ms'))} | {fmt(med('cp_cpu'))} | "
                         f"{fmt(med('nodes_cpu'))} |{dind_cell} "
                         f"{fmt(med('k6_cpu'))} | {'; '.join(fails)} |")
        lines.append("")
    if args.rates:
        lines += ["## Fixed rates", "",
                  f"| Read % | Nodes | N | Target req/s | Achieved req/s | p50 ms | p95 ms | p99 ms | read p99 ms | write p99 ms | Errors % | cp CPU | nodes CPU |{dind_head} Passed |",
                  f"|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|{dind_sep}---|"]
        for nodes, replicas, read_pct in setups:
            for rate in args.rates:
                runs = [r for r in rows if (r["nodes"], r["replicas"], r["read_pct"], r["kind"], r["rate"])
                        == (nodes, replicas, read_pct, "rate", rate)]
                med = lambda key: statistics.median(r[key] for r in runs if r.get(key) is not None) \
                    if any(r.get(key) is not None for r in runs) else None
                dind_cell = f" {fmt(med('dind_cpu'))} |" if dind else ""
                lines.append(f"| {read_pct} | {nodes} | {replicas} | {rate} | {fmt(med('achieved_rps'), '.0f')} | "
                             f"{fmt(med('p50_ms'))} | {fmt(med('p95_ms'))} | {fmt(med('p99_ms'))} | "
                             f"{fmt(med('read_p99_ms'))} | {fmt(med('write_p99_ms'))} | {fmt(med('error_pct'))} | "
                             f"{fmt(med('cp_cpu'))} | {fmt(med('nodes_cpu'))} |{dind_cell} "
                             f"{sum(r['ok'] for r in runs)}/{len(runs)} |")
    if args.baseline:
        lines += ["", "## Hop cost", "",
                  "1 node, N=1. direct: k6 to node-1. relay: k6 to a plain TCP relay (socat, where cp "
                  "runs) to node-1. cp: k6 to cp to node-1. Hop = relay - direct; cp own = cp - relay "
                  "(differences of medians).", "",
                  "| Read % | req/s | direct p50 | direct p99 | relay p50 | relay p99 | cp p50 | cp p99 | hop p50 | hop p99 | cp own p50 | cp own p99 |",
                  "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for read_pct in args.mix:
            for rate in args.rates:
                cells = {}
                for kind in ("direct", "relay", "rate"):
                    for q in ("p50", "p99"):
                        values = [r[f"{q}_ms"] for r in rows if (r["nodes"], r["read_pct"], r["rate"], r["kind"])
                                  == (1, read_pct, rate, kind) and r.get(f"{q}_ms") is not None]
                        cells[kind, q] = statistics.median(values) if values else None
                diff = lambda a, b, q: None if None in (cells[a, q], cells[b, q]) else cells[a, q] - cells[b, q]
                lines.append(f"| {read_pct} | {rate} | " + " | ".join(
                    fmt(v) for v in (cells["direct", "p50"], cells["direct", "p99"], cells["relay", "p50"],
                                     cells["relay", "p99"], cells["rate", "p50"], cells["rate", "p99"],
                                     diff("relay", "direct", "p50"), diff("relay", "direct", "p99"),
                                     diff("rate", "relay", "p50"), diff("rate", "relay", "p99"))) + " |")
    if args.closed:
        lines += ["", "## Closed loop (max throughput)", "",
                  "Workload:", "",
                  f"- Operations: {'/'.join(map(str, args.mix))}% reads (`GET /<key>`), the rest writes "
                  f"(`POST /<key>`, JSON `{{\"value\": ...}}`).",
                  f"- Keys: {args.keys}, all written before the test. Key choice: Zipf, s = 0.99 (a few hot keys).",
                  f"- Value size: {args.value_size} bytes.",
                  f"- Load: closed loop. Each k6 VU sends its next request when the last one returns. "
                  f"k6 -> cp -> node(s), HTTP/1.1 keep-alive.",
                  f"- Run: {args.duration:g} s per run, {args.repeats} repeat(s), median shown.", "",
                  "Settings:", "",
                  "- KV stores: storage nodes in the cluster.",
                  "- N: copies of each key (replication factor).",
                  "- W: copies that must confirm a write before cp answers.",
                  "- R: copies cp reads; it returns the newest. R + W > N, so a read sees the latest write.",
                  "- VUs: k6 clients, each with one request in flight.", "",
                  "| KV stores | N | W | R | Read % | VUs | Throughput req/s | avg ms | p50 ms | p99 ms | Errors % | cp CPU | nodes CPU | Passed |",
                  "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
        for nodes, replicas, read_pct in setups:
            for vus in args.closed:
                runs = [r for r in rows if (r["nodes"], r["replicas"], r["read_pct"], r["kind"], r["vus"])
                        == (nodes, replicas, read_pct, "closed", vus)]
                med = lambda key: statistics.median(r[key] for r in runs if r.get(key) is not None) \
                    if any(r.get(key) is not None for r in runs) else None
                quorum = int(replicas) // 2 + 1
                lines.append(f"| {nodes} | {replicas} | {quorum} | {quorum} | {read_pct} | {vus} | "
                             f"{fmt(med('achieved_rps'), '.0f')} | "
                             f"{fmt(med('avg_ms'))} | {fmt(med('p50_ms'))} | {fmt(med('p99_ms'))} | "
                             f"{fmt(med('error_pct'))} | {fmt(med('cp_cpu'))} | {fmt(med('nodes_cpu'))} | "
                             f"{sum(r['ok'] for r in runs)}/{len(runs)} |")
    with open(os.path.join(out, "summary.md"), "w") as stream:
        stream.write("\n".join(lines) + "\n")


def run(args):
    if not shutil.which("k6"):
        raise RuntimeError("k6 is required; install k6 and rerun")
    if args.driver == "process":
        subprocess.run(["make", "-j2", "build/kv-cp-proc", "build/app"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
    else:
        for dockerfile, image in (("Dockerfile.storage", "kv-storage"), ("Dockerfile.control", "kv-control")):
            subprocess.run(["docker", "build", "-q", "-f", dockerfile, "-t", image, "."], cwd=ROOT,
                           check=True, stdout=subprocess.DEVNULL)
        if args.baseline:
            subprocess.run(["docker", "pull", "-q", RELAY_IMAGE], check=True, stdout=subprocess.DEVNULL)
    # SIGTERM takes the Ctrl-C path, so the finally blocks remove the containers.
    signal.signal(signal.SIGTERM, signal.default_int_handler)

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out = args.out or os.path.join(ROOT, "cluster-results", stamp)
    os.makedirs(out, exist_ok=True)
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                            text=True).stdout.strip()
    rows = []
    try:
        if args.driver == "dind":
            subprocess.run(["docker", "rm", "-f", "-v", DIND_NAME], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            ports = ["-p", f"127.0.0.1:{args.cp_port}:7890"]
            if args.baseline:
                ports += ["-p", f"127.0.0.1:{args.cp_port + 1}:7891",
                          "-p", f"127.0.0.1:{args.cp_port + 2}:7892"]
            subprocess.run(["docker", "run", "-d", "--privileged", "--name", DIND_NAME,
                            "--cpus", args.cpus, "--memory", args.memory, *ports, args.dind_image,
                            "dockerd", "--host=unix:///var/run/docker.sock"],
                           check=True, stdout=subprocess.DEVNULL)
            deadline = time.monotonic() + 60
            while subprocess.run(["docker", "exec", DIND_NAME, "docker", "info"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                if time.monotonic() > deadline:
                    raise RuntimeError("the DinD daemon did not start")
                time.sleep(0.5)
            save = subprocess.Popen(["docker", "save", "kv-storage", "kv-control"]
                                    + ([RELAY_IMAGE] if args.baseline else []),
                                    stdout=subprocess.PIPE)
            subprocess.run(["docker", "exec", "-i", DIND_NAME, "docker", "load"], stdin=save.stdout,
                           check=True, stdout=subprocess.DEVNULL)
            save.stdout.close()
            if save.wait():
                raise RuntimeError("docker save failed")
            pid = subprocess.run(["docker", "inspect", "-f", "{{.State.Pid}}", DIND_NAME], check=True,
                                 capture_output=True, text=True).stdout.strip()
            with open(f"/proc/{pid}/cgroup") as stream:
                group = stream.read().strip().split("::", 1)[1]
            # The DinD entrypoint moves its own processes to a child "init" group.
            args.dind_cgroup = "/sys/fs/cgroup" + group.removesuffix("/init")
        for repeat in range(1, args.repeats + 1):
            for node_count in args.nodes:
                for replicas in dict.fromkeys(node_count if r == "n" else int(r) for r in args.replicas):
                    if replicas > node_count:
                        continue
                    run_setup(args, node_count, replicas, repeat, out, rows)
    finally:
        if args.driver == "dind":
            subprocess.run(["docker", "rm", "-f", "-v", DIND_NAME], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        fields = ["started_at", "commit", "driver", "repeat", "nodes", "replicas", "write_quorum", "read_quorum", "read_pct",
                  "kind", "rate",
                  "vus", "achieved_rps", "avg_ms", "p50_ms", "p95_ms", "p99_ms", "read_p50_ms", "read_p95_ms", "read_p99_ms",
                  "write_p50_ms", "write_p95_ms", "write_p99_ms", "error_pct", "dropped_pct", "cp_cpu",
                  "nodes_cpu", "dind_cpu", "k6_cpu", "load1", "k6_exit", "node_events", "node_event_detail",
                  "ok", "reason", "summary_file"]
        with open(os.path.join(out, "summary.csv"), "w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows({**r, "commit": commit, "driver": args.driver} for r in rows)
        if rows:
            write_summary(args, out, rows)
    print(out)


if __name__ == "__main__":
    run(parse_args())

#!/usr/bin/env python3
"""Measure throughput and latency at 1, 2, and 3 storage nodes."""

import argparse
import csv
import http.client
import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CP_BIN = os.path.join(ROOT, "build", "kv-cp-proc")
NODE_BIN = os.path.join(ROOT, "build", "app")
K6_SCRIPT = os.path.join(ROOT, "bench", "k6", "kv.js")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nodes", default="1,2,3", help="comma-separated node counts")
    parser.add_argument("--rates", default="2000,4000,8000", help="comma-separated k6 req/s")
    parser.add_argument("--mix", default="95,50", help="comma-separated read percentages")
    parser.add_argument("--duration", type=float, default=20, help="seconds per k6 run")
    parser.add_argument("--keys", type=int, default=10000)
    parser.add_argument("--value-size", type=int, default=100)
    parser.add_argument("--cp-port", type=int, default=7980)
    parser.add_argument("--base-port", type=int, default=7990)
    parser.add_argument("--out", help="output directory")
    args = parser.parse_args()
    args.nodes = [int(v) for v in args.nodes.split(",")]
    args.rates = [int(v) for v in args.rates.split(",")]
    args.mix = [int(v) for v in args.mix.split(",")]
    if any(n < 1 or n > 3 for n in args.nodes):
        parser.error("--nodes: only 1, 2, or 3 are supported")
    if any(r <= 0 for r in args.rates):
        parser.error("--rates must contain positive integers")
    if any(m < 0 or m > 100 for m in args.mix):
        parser.error("--mix read percentages must be 0..100")
    return args


def wait_http(port, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/")
            response = conn.getresponse()
            response.read()
            conn.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"control plane did not listen on port {port}")


def metric(summary, name, key, default=None):
    return summary["metrics"].get(name, {}).get("values", {}).get(key, default)


def run(args):
    if not shutil.which("k6"):
        raise RuntimeError("k6 is required; install k6 and rerun")
    if not os.path.exists(CP_BIN) or not os.path.exists(NODE_BIN):
        subprocess.run(["make", "-j2", "build/kv-cp-proc", "build/app"], cwd=ROOT, check=True)

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out = args.out or os.path.join(ROOT, "cluster-results", stamp)
    os.makedirs(out, exist_ok=True)
    rows = []

    for node_count in args.nodes:
        temp_dir = tempfile.mkdtemp(prefix="kv-cluster-bench-")
        env = dict(os.environ)
        env.update({
            "KV_CP_LISTEN_ADDR": f"127.0.0.1:{args.cp_port}",
            "KV_CP_DB_PATH": os.path.join(temp_dir, "cp.db"),
            "KV_CP_LOG_PATH": os.path.join(temp_dir, "cp.log"),
            "KV_CP_THREADS": "2",
            "KV_CP_INITIAL_NODES": str(node_count),
            "KV_CP_MAX_NODES": str(node_count),
            "KV_CP_REPLICAS": "1",
            "KV_CP_NODE_BASE_PORT": str(args.base_port),
            "KV_CP_NODE_BIN": NODE_BIN,
            "KV_CP_NODE_DIR": os.path.join(temp_dir, "nodes"),
            "KV_CP_PROBE_INTERVAL_MS": "100",
            "KV_CP_PROBE_FAILS": "3",
        })
        cp = subprocess.Popen([CP_BIN], cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
        try:
            wait_http(args.cp_port)
            time.sleep(1)
            for read_pct in args.mix:
                for rate in args.rates:
                    name = f"nodes-{node_count}-mix-{read_pct}-rate-{rate}"
                    summary_path = os.path.join(out, name + ".json")
                    k6_env = dict(os.environ, BASE_URL=f"http://127.0.0.1:{args.cp_port}",
                                  READ_PCT=str(read_pct), KEYS=str(args.keys),
                                  VALUE_SIZE=str(args.value_size), MODE="step",
                                  RATE=str(rate), DURATION=f"{args.duration}s",
                                  SUMMARY_PATH=summary_path)
                    subprocess.run(["k6", "run", "--quiet", K6_SCRIPT], cwd=ROOT, env=k6_env,
                                   check=False)
                    if not os.path.exists(summary_path):
                        rows.append({"nodes": node_count, "read_pct": read_pct, "rate": rate,
                                     "achieved_rps": "", "p50_ms": "", "p95_ms": "", "p99_ms": "",
                                     "error_pct": "", "status": "no-k6-summary"})
                        continue
                    with open(summary_path) as stream:
                        summary = json.load(stream)
                    duration = metric(summary, "http_req_duration{phase:main}", "p(99)")
                    rows.append({
                        "nodes": node_count,
                        "read_pct": read_pct,
                        "rate": rate,
                        "achieved_rps": metric(summary, "iterations", "rate", ""),
                        "p50_ms": metric(summary, "http_req_duration{phase:main}", "med", ""),
                        "p95_ms": metric(summary, "http_req_duration{phase:main}", "p(95)", ""),
                        "p99_ms": duration if duration is not None else "",
                        "error_pct": (metric(summary, "http_req_failed{phase:main}", "rate", 0) or 0) * 100,
                        "status": "ok",
                    })
        finally:
            if cp.poll() is None:
                cp.send_signal(signal.SIGTERM)
                try:
                    cp.wait(30)
                except subprocess.TimeoutExpired:
                    cp.kill()
                    cp.wait()
            shutil.rmtree(temp_dir, ignore_errors=True)

    csv_path = os.path.join(out, "summary.csv")
    fields = ["nodes", "read_pct", "rate", "achieved_rps", "p50_ms", "p95_ms", "p99_ms",
              "error_pct", "status"]
    with open(csv_path, "w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    with open(os.path.join(out, "summary.md"), "w") as stream:
        stream.write("# KV cluster benchmark\n\n")
        stream.write("| Nodes | Read % | Target RPS | Achieved RPS | p50 ms | p95 ms | p99 ms | Errors % | Status |\n")
        stream.write("|---:|---:|---:|---:|---:|---:|---:|---:|---|\n")
        for row in rows:
            stream.write("| {nodes} | {read_pct} | {rate} | {achieved_rps} | {p50_ms} | "
                         "{p95_ms} | {p99_ms} | {error_pct} | {status} |\n".format(**row))
    print(out)


if __name__ == "__main__":
    run(parse_args())

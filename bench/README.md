# Benchmark

## One-to-three-node cluster benchmark

`cluster_profile.py` measures the control plane with the existing process driver. It starts one,
two, and three storage processes, and uses k6 for each setup. `--step` finds the max sustainable
rate (same SLO rules as `profile.py`); `--rates` runs fixed rates for latency.

```sh
make bench-cluster BENCH_ARGS="--step --replicas 1,n --repeats 3"
python3 bench/cluster_profile.py --replicas 1,n --rates 1000,2000 --repeats 3
python3 bench/cluster_profile.py --help
```

`--replicas 1,n` runs each node count with N=1 and with N = node count. Repeats loop outermost, so
slow host drift spreads over all setups. By default k6, cp and each node are pinned to their own
physical cores with `taskset` (12 logical CPUs; `--no-pin` turns it off), and each run records cp,
node and k6 CPU use.

Output is written to `cluster-results/<timestamp>/` (gitignored): `summary.csv` has one row per k6
run (setup, rate, latency per operation, errors, dropped iterations, CPU, host load, commit),
`summary.md` has median tables, and each run keeps its k6 JSON summary.

### Docker-in-Docker mode

`--driver dind` runs the real Docker driver (`kv-control` and `kv-storage` images) inside one
privileged `docker:29.9.2-dind` container (`kv-bench-dind`), limited by `--cpus` (default 6) and
`--memory` (default 8g). cp spawns its node containers on the nested daemon; k6 runs on the host
without a limit and sends to cp through `127.0.0.1:8080` (`--cp-port`). The script builds both
images on the host and loads them into the nested daemon. Each setup starts a fresh cp, nodes and
volumes; the DinD container is removed at the end, also on error, Ctrl-C or SIGTERM.

```sh
python3 bench/cluster_profile.py --driver dind --step --replicas 1,n --repeats 3
python3 bench/cluster_profile.py --driver dind --cpus 4 --memory 4g --rates 1000,2000
```

Requires Docker that can run privileged containers, cgroup v2 on the host, and host port 8080
free. There is no pinning. cp and nodes CPU come from the nested containers' cgroups; the
`DinD CPU` column is the whole DinD container (also the nested dockerd and networking). The
host-to-cp path has extra NAT hops, so latency is higher than in process mode.

### Hop cost and node events

`--baseline` (needs `--rates`, both drivers) runs each rate at 1 node three times, back to back:
`direct` (k6 to node-1), `relay` (k6 to a plain `socat` TCP relay to node-1; dind: an
`alpine/socat:1.8.1.1` container next to cp, host port 8082; direct uses host port 8081), and the
normal cp run (`kind` `rate`). `summary.md` gets a "Hop cost" table: hop = relay - direct, cp own =
cp - relay. Process mode needs `socat` on the host (relay on port cp-port + 1).

Before and after each k6 run the script compares the node set (`/_cp/nodes` plus node pids or
container ids) and reads the cp log lines of the run that match
`SUSPECT|DEAD|spawn|RESPAWN|adding a node|migration`. Any hit is a node event (`node_events`,
`node_event_detail` in the CSV) and fails the run with reason `node_event`. Autoscale is off
(`KV_CP_SCALE_RPS=0`, max nodes = node count).

### Closed loop

`--closed 4,16,64` runs closed-loop k6 (`constant-vus`) through cp for each VU count: each VU
sends its next request when the last one returns, so throughput is as high as the system allows.
`kind` is `closed`, with `vus` and `avg_ms` in the CSV, and a "Closed loop" table in
`summary.md`. The SLO still decides `ok`. These numbers are not comparable with open-loop rates.

`profile.py` (Python 3, stdlib only) finds the max sustainable RPS of the server for each workload mix in `--mix` (read %, default `95,50`: 95% read / 5% write and 50% read / 50% write), and profiles the server with `perf`. Requires `k6` and, for profiles, `perf` (`kernel.perf_event_paranoid` <= 2 is enough: it records user space of the server only).

```sh
make bench                                         # 95/5 and 50/50, default steps (some minutes)
python3 bench/profile.py --mix 100,95,50,0         # read-only to write-only
python3 bench/profile.py --mix 50 --p99-ms 20      # one mix, stricter SLO
python3 bench/profile.py --help                    # all options
```

For each mix the script builds the server (`--no-build` skips), starts `build/app` on a new database in a temp dir (`--threads`, `--flush-ms` set `KV_THREADS`, `KV_FLUSH_INTERVAL_MS`), preloads `--keys` keys, then runs k6 steps at a constant arrival rate: `--start-rate`, multiplied by `--step-factor` per step, each `--step-duration` seconds. It stops at the first step that breaks the SLO: p99 >= `--p99-ms` (default 50), errors >= 1%, or more than 1% of iterations dropped (k6 could not send them in time). The max sustainable RPS is the rate of the last step that passed. Separate constant-rate steps give each rate its own latency numbers; a single ramp mixes the latency of rates before and after saturation. During each step `perf record` samples the server for `--perf-seconds`, with DWARF call graphs (the build has no frame pointers, so `--call-graph fp` gives broken stacks; both cost about the same here). `perf stat` counts the whole run.

Output in `results/<timestamp>/` (gitignored):

| File | Content |
|---|---|
| `summary.md` | Max sustainable RPS, p50/p95/p99 per operation, errors, server and k6 CPU, context switches, IPC, top 10 hotspots per mix, verdict template |
| `mix-<pct>/k6-<rate>.json` | k6 summary of each step |
| `mix-<pct>/perf-sustained.data`, `report-sustained.txt` | Profile at the max sustainable rate (`perf report -i` it for call graphs) |
| `mix-<pct>/perf-saturation.data`, `report-saturation.txt` | Profile at the first failing rate |
| `mix-<pct>/flame-*.svg` | Flame graphs, only if `inferno-collapse-perf`/`inferno-flamegraph` or `stackcollapse-perf.pl`/`flamegraph.pl` are on `PATH` |
| `mix-<pct>/kv.log`, `server.out`, `perf-stat.csv`, `steps.json` | Server request log and output, counters, raw step data |

k6 uses CPU too. If the k6 CPU column is near the free CPUs of the host, k6 is the limit: run k6 on a second host.

Remote server: start the server there, then from the client host drive only k6 (no build, no server, no perf):

```sh
python3 bench/profile.py --no-server --addr 10.0.0.5:7890
```

To profile the remote server, run the script on that host instead. The k6 script also runs alone; it ramps the arrival rate until the thresholds abort the run:

```sh
k6 run -e BASE_URL=http://127.0.0.1:7890 -e READ_PCT=50 -e MAX_RATE=50000 -e SUMMARY_PATH=out.json bench/k6/kv.js
```

Variables: `READ_PCT` (95), `KEYS` (10000), `VALUE_SIZE` (100), `ZIPF_S` (0.99), `P99_MS` (50), `MODE` (`ramp` or `step`), `START_RATE`, `MAX_RATE`, `STAGES`, `RAMP`, `HOLD` for `ramp`; `RATE`, `DURATION` for `step`; `PRELOAD` (1), `PRE_VUS`, `MAX_VUS`.

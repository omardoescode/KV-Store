# Benchmark

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

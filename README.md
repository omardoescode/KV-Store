# KV-Store
A unecessarily elaborate KV Store. Might be better than DynamoDB

## Development

Requires `gcc`, a C++ compiler (only for the H2O build), `make`, `clang-format`, `clang-tidy`, `cmake`, `pkg-config`, `valgrind`, `curl`, the `sqlite3` CLI, and the OpenSSL, zlib and SQLite 3 development files.

Persistence: the hashmap in memory serves reads. A writer thread commits each POST/DELETE to the SQLite database at `KV_DB_PATH` (WAL, `synchronous=FULL`) every `KV_FLUSH_INTERVAL_MS`; a failed commit is kept and retried. While `KV_MAX_PENDING` changes (default 1,000,000) wait for the database, POST and DELETE return 503 and do not change the map; GET still works. At start the server loads all rows into memory.

HTTP server: [H2O](https://github.com/h2o/h2o) (`libh2o-evloop`), a git submodule in `third_party/h2o`. `make` builds it with CMake into `build/h2o/`. Supported: Linux, macOS, BSD.

Only `src/http/` can include H2O. All other code uses the wrapper in `src/http/http.h`; see `src/main.c` for an example.

JSON: [yyjson](https://github.com/ibireme/yyjson), a git submodule in `third_party/yyjson`. Only `src/json/` can include it. All other code uses the wrapper in `src/json/json.h`, which has no dependency on the HTTP wrapper.

After clone, run the following command to make sure all needed commands are installed, the submodule is fetched, and git hooks are enabled

```sh
make init
```

Before `make run`, create the env file. `KV_LISTEN_ADDR`, `KV_LOG_PATH` and `KV_DB_PATH` are required; `.env.example` lists all keys. Local DB and log go in `./in/` (gitignored, `make run` creates it); `KV_LOG_PATH=-` logs to stdout:

```sh
cp .env.example .env
```

The pre-commit hook checks format, runs clang-tidy on staged files, and runs `make memcheck`. CI (`.github/workflows/ci.yml`) runs the same checks on each pull request.


| Command | Function |
|---|---|
| `make` | Build `build/app`, write `build/compile_commands.json` for clangd |
| `make run` | Build and run the server with env from `./.env` (`curl -d '{"value":"v"}' 127.0.0.1:7890/k; curl 127.0.0.1:7890/k`) |
| `make test` | Build and run all unit tests, then `tests/http_test.sh` (HTTP status/body checks) and `tests/shutdown_test.sh`, which start `build/app`; stop on the first failure. Needs `curl`, `sqlite3`, and free ports 7891 and 7892 (set `KV_LISTEN_ADDR` to change them) |
| `make memcheck` | Run `tests/http_test.sh` with the server under valgrind, fail on a memory error or leak. Port 7890 must be free |
| `make bench` | Stress test and profile the server, see [Benchmark](#benchmark). Pass options with `BENCH_ARGS='...'` |
| `make format` | Format sources |
| `make format-check` | Check format, no changes |
| `make lint` | Run clang-tidy |
| `make lint-fix` | Run clang-tidy and apply fixes |
| `make clean` | Remove the app build, keep the H2O build |
| `make distclean` | Remove `build/` |

## Docker

Multi-stage build (Debian bookworm): builds `build/app` from the checked-out sources and submodules (run `make init` or `git submodule update --init` first), then copies it to a `bookworm-slim` image that runs as a non-root user.

```sh
docker build -t kv-store .
docker run -d --name kv -p 7890:7890 -v kv-data:/data kv-store
curl -d '{"value":"v"}' 127.0.0.1:7890/k
curl 127.0.0.1:7890/k
docker logs -f kv
docker stop kv
```

Image defaults: `KV_LISTEN_ADDR=0.0.0.0:7890`, `KV_LOG_PATH=-` (request log goes to `docker logs`), `KV_DB_PATH=/data/kv.db`. `/data` is a volume; mount a named volume to keep the database. Set other keys with `-e`, e.g. `-e KV_THREADS=4 -e KV_FLUSH_INTERVAL_MS=50`. `docker stop` sends SIGTERM: the server commits pending writes and exits 0.

## Benchmark

`bench/profile.py` (Python 3, stdlib only) finds the max sustainable RPS of the server for each workload mix in `--mix` (read %, default `95,50`: 95% read / 5% write and 50% read / 50% write), and profiles the server with `perf`. Requires `k6` and, for profiles, `perf` (`kernel.perf_event_paranoid` <= 2 is enough: it records user space of the server only).

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

# KV-Store
A unecessarily elaborate KV Store. Might be better than DynamoDB

## Quick start

If you only want to run the server, see [Docker](#docker) instead.

```sh
make init                # check tools, fetch submodules, enable git hooks
cp .env.example .env     # config; see comments in the file
make run
curl -d '{"value":"v"}' 127.0.0.1:7890/k
curl 127.0.0.1:7890/k
```

Requires `gcc`, a C++ compiler, `make`, `cmake`, `pkg-config`, `clang-format`, `clang-tidy`, `valgrind`, `curl`, `sqlite3`, and OpenSSL, zlib and SQLite 3 dev files.

## How it works

- Reads come from an in-memory hashmap. At start, all rows load from SQLite (`KV_DB_PATH`).
- A writer thread commits POST/DELETE to SQLite every `KV_FLUSH_INTERVAL_MS`. Failed commits retry.
- If `KV_MAX_PENDING` changes wait, POST/DELETE return 503. GET still works.
- HTTP: [H2O](https://github.com/h2o/h2o), only `src/http/` includes it; use `src/http/http.h`.
- JSON: [yyjson](https://github.com/ibireme/yyjson), only `src/json/` includes it; use `src/json/json.h`.

## Commands

| Command | Function |
|---|---|
| `make` | Build `build/app` and `build/compile_commands.json` |
| `make run` | Build and run with `./.env` |
| `make test` | Unit tests + HTTP and shutdown tests (ports 7891, 7892) |
| `make memcheck` | HTTP tests under valgrind (port 7890) |
| `make bench` | Load test and profile, see [bench/README.md](bench/README.md) |
| `make format` / `format-check` | Format / check format |
| `make lint` / `lint-fix` | clang-tidy / apply fixes |
| `make clean` / `distclean` | Remove app build / remove `build/` |

Pre-commit hook and CI run format check, clang-tidy and `make memcheck`.

## Docker

Run `make init` first (submodules).

Full system (control plane on port 7890; it spawns storage nodes as containers through the Docker socket):

```sh
docker compose up --build
```

`compose.yaml` builds `Dockerfile.control` (image `kv-control`) and `Dockerfile.storage` (image `kv-storage`), and starts only cp. cp creates `kv-node-<id>` containers with volumes `kv-data-<id>` on network `kv-net`. Set `KV_CP_INITIAL_NODES`, `KV_CP_MAX_NODES`, `KV_CP_REPLICAS` (N), `KV_CP_READ_QUORUM` (R), `KV_CP_WRITE_QUORUM` (W), `KV_CP_SCALE_RPS` and `KV_SYNC_ACK` in the shell or `.env` (see `.env.cp.example`). `docker compose down` stops cp only; the node containers keep running and cp adopts them on its next start.

One storage node without cp:

```sh
docker build -f Dockerfile.storage -t kv-storage .
docker run -d --name kv -p 7890:7890 -v kv-data:/data kv-storage
```

Storage image defaults: `KV_LISTEN_ADDR=0.0.0.0:7890`, `KV_LOG_PATH=-` (request log goes to `docker logs`), `KV_DB_PATH=/data/kv.db`. `/data` is a volume; mount a named volume to keep the database. Set other keys with `-e`, e.g. `-e KV_THREADS=4 -e KV_FLUSH_INTERVAL_MS=50`. `docker stop` sends SIGTERM: the server commits pending writes and exits 0.

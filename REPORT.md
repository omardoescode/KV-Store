# KV-Store Consistent-Hashing Verification Report

## Implementation

The repository uses a control plane in front of storage nodes. Its ring implementation
([`src/cp/ring.c`](src/cp/ring.c)) adds virtual nodes for each storage
node, hashes the virtual-node names deterministically with SipHash, sorts hash positions, and selects
the first distinct nodes clockwise from a key's position. Replication preferences are capped by the
number of nodes in the ring. Rebuilding the ring after a join or leave changes only keys in affected hash
ranges, and the cluster code transfers and purges keys while preserving live replicas.

The Docker setup defines a control-plane container and a storage-node image
([`compose.yaml`](compose.yaml)). The control plane starts nodes with
named volumes and has `KV_CP_INITIAL_NODES`, `KV_CP_MAX_NODES`, and `KV_CP_REPLICAS` configuration.
The storage image exposes port 7890 and persists SQLite data in `/data`.

## Verification

The existing test suite validates ring balance and deterministic owners, 1–3-node cluster state,
replication, failure detection, restart repair, joins, writes, HTTP behavior, and shutdown. Run:

```sh
make test
```

The ring unit tests check that 30,000 keys remain within 7,000–13,000 owners per node and that a
3-to-4-node join moves approximately 25% of 40,000 keys. The control-plane tests exercise the process
driver, which does not depend on Docker.

An earlier run on another machine reported `test_rf3_copy_target`, `test_rf3_drain_1` and
`test_rf3_drain_2` as failing. They did not reproduce: at `cf90eaf` each passed 12 of 12 runs, and
`make test` passed (`Ran 90 tests ... OK`, all 72 fault-matrix cases). If a fault case fails, keep its
data with `KV_TEST_KEEP=1 python3 tests/cp_test.py FaultMatrixTest.<case>`.

## Performance measurement procedure

Use the harness on a host with k6:

```sh
make bench-cluster
# or: python3 bench/cluster_profile.py --nodes 1,2,3 --rates 2000,4000,8000 --duration 20
```

The harness starts 1, 2, and 3 storage processes through the existing process driver and records
target RPS, achieved RPS, p50/p95/p99 request latency, and HTTP error percentage in
`cluster-results/<timestamp>/summary.csv` and `summary.md`.

## Container procedure

On a Docker host:

```sh
docker compose up --build
KV_CP_INITIAL_NODES=1 KV_CP_MAX_NODES=1 docker compose up --build -d
KV_CP_INITIAL_NODES=2 KV_CP_MAX_NODES=2 docker compose up --build -d
KV_CP_INITIAL_NODES=3 KV_CP_MAX_NODES=3 docker compose up --build -d
```

Run the benchmark against `127.0.0.1:7890` for the live cluster. Incrementally changing
`KV_CP_INITIAL_NODES` from 1 to 3 is the intended 1–3-node scalability comparison.

## Results table

Live measurements should be entered below after running the benchmark. The table deliberately
contains no fabricated measurements:

| Nodes | Target RPS | Achieved RPS | p50 ms | p95 ms | p99 ms | Error % |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | Not measured | - | - | - | - | - |
| 2 | Not measured | - | - | - | - | - |
| 3 | Not measured | - | - | - | - | - |

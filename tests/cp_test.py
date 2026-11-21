#!/usr/bin/env python3
"""Tests of the control plane (cp) through its HTTP API, with nodes as local processes.

Starts build/kv-cp-proc (cp linked with the process driver) and the storage nodes it spawns from
build/app. KV_CP_FAULT makes the process driver kill a node, or cp, when a phase starts.
Run: python3 tests/cp_test.py [-k name]
"""
import http.client
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CP_BIN = os.path.join(ROOT, 'build', 'kv-cp-proc')
NODE_BIN = os.path.join(ROOT, 'build', 'app')
# Command prefix for cp, e.g. valgrind (tests/memcheck.sh).
CP_WRAPPER = os.environ.get('KV_CP_WRAPPER', '').split()
# Separate ports let several test runs go in parallel.
CP_PORT = int(os.environ.get('KV_TEST_CP_PORT', 7980))
BASE_PORT = int(os.environ.get('KV_TEST_BASE_PORT', 7990))


def request(port, method, path, body=None, headers=None, timeout=10):
    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
    try:
        data = json.dumps(body).encode() if isinstance(body, (dict, list)) else body
        conn.request(method, path, body=data, headers=headers or {})
        resp = conn.getresponse()
        return resp.status, resp.read().decode(), dict(resp.getheaders())
    finally:
        conn.close()


def wait_for(cond, timeout=20, what='condition'):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if cond():
                return
        except (OSError, http.client.HTTPException, ValueError):
            pass
        time.sleep(0.05)
    raise AssertionError(f'timed out waiting for {what}')


class Cluster:
    """One cp process and its node processes, in a temp dir that holds cp.db and node data."""

    def __init__(self, **env):
        self.dir = tempfile.mkdtemp(prefix='kv-cp-test-')
        self.env = {
            'KV_CP_LISTEN_ADDR': f'127.0.0.1:{CP_PORT}',
            'KV_CP_DB_PATH': os.path.join(self.dir, 'cp.db'),
            'KV_CP_LOG_PATH': os.path.join(self.dir, 'cp.log'),
            'KV_CP_THREADS': '2',
            'KV_CP_NODE_BASE_PORT': str(BASE_PORT),
            'KV_CP_NODE_BIN': NODE_BIN,
            'KV_CP_NODE_DIR': os.path.join(self.dir, 'nodes'),
            'KV_CP_PROBE_INTERVAL_MS': '100',
            'KV_CP_PROBE_FAILS': '3',
            'KV_CP_SCALE_COOLDOWN_S': '0',
            'KV_CP_INITIAL_NODES': '1',
            'KV_CP_REPLICAS': '1',
        }
        self.env.update({k: str(v) for k, v in env.items()})
        # R and W default to 2 in cp; a test with N=1 gets 1.
        quorum = str(min(2, int(self.env['KV_CP_REPLICAS'])))
        self.env.setdefault('KV_CP_READ_QUORUM', quorum)
        self.env.setdefault('KV_CP_WRITE_QUORUM', quorum)
        self.proc = None
        self.writers = []

    def start(self, wait_nodes=None, **env):
        self.env.update({k: str(v) for k, v in env.items()})
        full = dict(os.environ)
        full.update(self.env)
        with open(os.path.join(self.dir, 'cp.stderr'), 'ab') as err:
            self.proc = subprocess.Popen(CP_WRAPPER + [CP_BIN], env=full, stdout=subprocess.DEVNULL,
                                         stderr=err)
        n = wait_nodes if wait_nodes is not None else int(self.env.get('KV_CP_INITIAL_NODES', 1))
        if n:
            self.wait_active(n)
        return self

    def stop(self, sig=signal.SIGTERM):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(sig)
            try:
                self.proc.wait(30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def close(self):
        for w in self.writers:
            w.stop_flag = True
        for w in self.writers:
            w.join(15)
        self.stop()
        for pid in self.node_pids().values():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if os.environ.get('KV_TEST_KEEP'):
            print('kept', self.dir, file=sys.stderr)
        else:
            shutil.rmtree(self.dir, ignore_errors=True)

    def stderr(self):
        try:
            with open(os.path.join(self.dir, 'cp.stderr')) as f:
                return f.read()
        except OSError:
            return ''

    def node_pids(self):
        pids = {}
        root = self.env['KV_CP_NODE_DIR']
        if not os.path.isdir(root):
            return pids
        for name in os.listdir(root):
            try:
                with open(os.path.join(root, name, 'pid')) as f:
                    pids[int(name.split('-')[1])] = int(f.read())
            except (OSError, ValueError, IndexError):
                pass
        return pids

    def kill_node(self, node_id):
        os.kill(self.node_pids()[node_id], signal.SIGKILL)

    def req(self, method, path, body=None, headers=None):
        return request(CP_PORT, method, path, body, headers)

    def node_req(self, node_id, method, path, body=None):
        return request(BASE_PORT + node_id, method, path, body)

    def put(self, key, value):
        return self.req('POST', '/' + key, {'value': value})[0]

    def get(self, key):
        status, body, _ = self.req('GET', '/' + key)
        return json.loads(body)['value'] if status == 200 else status

    def delete(self, key):
        return self.req('DELETE', '/' + key)[0]

    def nodes(self):
        status, body, _ = self.req('GET', '/_cp/nodes')
        assert status == 200, body
        return {n['id']: n for n in json.loads(body)['nodes']}

    def migration(self):
        return json.loads(self.req('GET', '/_cp/migration')[1])

    def wait_active(self, n, timeout=20):
        wait_for(lambda: sum(1 for x in self.nodes().values() if x['state'] == 'ACTIVE') == n
                 and self.migration()['phase'] == 'IDLE', timeout, f'{n} ACTIVE nodes')

    def dump(self, node_id):
        """Committed rows of one node: {key: (value, (clock, ts, coord), deleted)}."""
        self.node_req(node_id, 'POST', '/_internal/flush')
        rows, cursor = {}, ''
        while True:
            status, body, _ = self.node_req(node_id, 'GET', f'/_internal/dump?limit=1000&cursor={cursor}')
            assert status == 200, body
            page = json.loads(body)
            for it in page['items']:
                rows[it['key']] = (it['value'], (it['clock'], it['ts'], it['coord']), it['deleted'])
            if page['next'] is None:
                return rows
            cursor = page['next']


class ClusterTest(unittest.TestCase):
    def cluster(self, **env):
        c = Cluster(**env)
        self.addCleanup(c.close)
        return c


class PublicApiTest(ClusterTest):
    def test_same_api_as_a_node(self):
        c = self.cluster().start()
        self.assertEqual(c.put('a', 'hello'), 200)
        self.assertEqual(c.req('GET', '/a')[:2], (200, '{"value":"hello"}'))
        self.assertEqual(c.get('missing'), 404)
        self.assertEqual(c.delete('a'), 200)
        self.assertEqual(c.get('a'), 404)
        self.assertEqual(c.delete('a'), 404)
        self.assertEqual(c.req('POST', '/a', b'not json')[0], 400)
        self.assertEqual(c.req('GET', '/a%00b')[0], 400)
        status, _, headers = c.req('PUT', '/a')
        self.assertEqual(status, 405)
        self.assertEqual(headers.get('allow'), 'GET, HEAD, POST, DELETE')
        self.assertEqual(c.req('GET', '/')[0], 404)

    def test_admin_shows_nodes_and_ring(self):
        c = self.cluster(KV_CP_INITIAL_NODES=2, KV_CP_REPLICAS=2, KV_CP_VNODES=8).start()
        nodes = c.nodes()
        self.assertEqual(sorted(nodes), [1, 2])
        self.assertEqual(nodes[1]['addr'], f'127.0.0.1:{BASE_PORT + 1}')
        ring = json.loads(c.req('GET', '/_cp/ring')[1])
        self.assertEqual(ring['nodes'], [1, 2])
        self.assertEqual(ring['vnodes'], 8)
        self.assertEqual(ring['replicas'], 2)
        self.assertEqual(c.migration(), {'phase': 'IDLE', 'target': None})


class ReplicationTest(ClusterTest):
    def test_write_goes_to_every_replica_with_one_version(self):
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=2).start()
        for i in range(50):
            self.assertEqual(c.put(f'k{i}', f'v{i}'), 200)
        self.assertEqual(c.delete('k0'), 200)
        dumps = {n: c.dump(n) for n in (1, 2, 3)}
        for i in range(50):
            holders = [n for n in dumps if f'k{i}' in dumps[n]]
            self.assertEqual(len(holders), 2, f'k{i} on {holders}')
            rows = {dumps[n][f'k{i}'] for n in holders}
            self.assertEqual(len(rows), 1, f'replicas of k{i} differ: {rows}')
        self.assertTrue(all(dumps[n]['k0'][2] for n in dumps if 'k0' in dumps[n]))

    def test_cp_refuses_bad_config(self):
        c = self.cluster(KV_CP_INITIAL_NODES=1, KV_CP_REPLICAS=2)
        c.start(wait_nodes=0)
        self.assertEqual(c.proc.wait(10), 1)
        self.assertIn('KV_CP_REPLICAS', c.stderr())

    def test_cp_exits_after_repeated_spawn_failures(self):
        c = self.cluster(KV_CP_NODE_BIN='/bin/false')
        c.start(wait_nodes=0)
        self.assertNotEqual(c.proc.wait(120), 0)
        self.assertIn('failed 5 times', c.stderr())


class FailureTest(ClusterTest):
    def test_reads_and_writes_go_on_when_a_replica_dies(self):
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=3, KV_CP_READ_QUORUM=2,
                         KV_CP_WRITE_QUORUM=2).start()
        for i in range(20):
            c.put(f'k{i}', 'before')
        c.kill_node(1)
        self.assertEqual(c.put('k0', 'after'), 200)
        self.assertEqual(c.get('k0'), 'after')
        for i in range(1, 20):
            self.assertEqual(c.get(f'k{i}'), 'before')
            self.assertEqual(c.put(f'k{i}', 'after'), 200)
            self.assertEqual(c.get(f'k{i}'), 'after')

    def test_read_writes_the_winner_back_to_a_stale_replica(self):
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=3, KV_CP_READ_QUORUM=2,
                         KV_CP_WRITE_QUORUM=2, KV_CP_FORWARD_TIMEOUT_MS=300,
                         KV_CP_PROBE_FAILS=100).start(wait_nodes=3)
        self.assertEqual(c.put('k', 'old'), 200)
        # An acked write that missed node 1: nodes 2 and 3 get a newer version of the coordinator.
        old = json.loads(c.node_req(2, 'GET', '/_internal/get?key=k')[1])
        coord, n, _ = old['clock'].split(':')
        newer = {'key': 'k', 'value': 'new', 'clock': f'{coord}:{int(n) + 1}:{old["ts"] + 1}',
                 'ts': old['ts'] + 1,
                 'coord': int(coord), 'deleted': False}
        for node in (2, 3):
            self.assertEqual(c.node_req(node, 'POST', '/_internal/batch', {'items': [newer]})[0], 200)
        entry = lambda node: json.loads(c.node_req(node, 'GET', '/_internal/get?key=k')[1])
        self.assertEqual(entry(1)['value'], 'old')
        # With node 2 stopped, the read quorum is nodes 1 (stale) and 3.
        pid = c.node_pids()[2]
        os.kill(pid, signal.SIGSTOP)
        try:
            self.assertEqual(c.get('k'), 'new')
            self.assertEqual(entry(1), entry(3))
        finally:
            os.kill(pid, signal.SIGCONT)

    def test_write_needs_only_w_live_replicas_when_r_is_larger(self):
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=3, KV_CP_READ_QUORUM=3,
                         KV_CP_WRITE_QUORUM=2, KV_CP_FORWARD_TIMEOUT_MS=300,
                         KV_CP_PROBE_FAILS=100).start(wait_nodes=3)
        self.assertEqual(c.put('k', 'v1'), 200)
        pid = c.node_pids()[1]
        os.kill(pid, signal.SIGSTOP)
        try:
            self.assertEqual(c.put('k', 'v2'), 200)
            self.assertEqual(c.delete('gone'), 404)
            self.assertEqual(c.get('k'), 503)
        finally:
            os.kill(pid, signal.SIGCONT)
        self.assertEqual(c.get('k'), 'v2')

    def test_read_repairs_a_stale_replica_that_answers_after_the_read_returns(self):
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=3, KV_CP_READ_QUORUM=2,
                         KV_CP_WRITE_QUORUM=2, KV_CP_FORWARD_TIMEOUT_MS=3000,
                         KV_CP_PROBE_FAILS=100).start(wait_nodes=3)

        def posts(node, key):
            with open(os.path.join(c.env['KV_CP_NODE_DIR'], f'node-{node}', 'kv.log')) as f:
                return f.read().count(f'POST /{key} 200')

        # Each node in turn is the stale one, so one of them is last in the preference list.
        for stale in (1, 2, 3):
            key = f'k{stale}'
            self.assertEqual(c.put(key, 'old'), 200)
            wait_for(lambda: posts(stale, key) == 1, 10, 'the put in the log')
            old = json.loads(c.node_req(stale, 'GET', f'/_internal/get?key={key}')[1])
            coord, n, _ = old['clock'].split(':')
            newer = {'key': key, 'value': 'new', 'clock': f'{coord}:{int(n) + 1}:{old["ts"] + 1}',
                     'ts': old['ts'] + 1, 'coord': int(coord), 'deleted': False}
            for node in {1, 2, 3} - {stale}:
                self.assertEqual(
                    c.node_req(node, 'POST', '/_internal/batch', {'items': [newer]})[0], 200)
            pid = c.node_pids()[stale]
            os.kill(pid, signal.SIGSTOP)
            resume = threading.Timer(0.5, os.kill, (pid, signal.SIGCONT))
            resume.start()
            try:
                self.assertEqual(c.get(key), 'new')
            finally:
                resume.join()
            wait_for(lambda: posts(stale, key) == 2, 10, f'read repair of node {stale}')
            entry = json.loads(c.node_req(stale, 'GET', f'/_internal/get?key={key}')[1])
            self.assertEqual(entry['value'], 'new')

    def test_a_replica_down_past_the_tombstone_ttl_does_not_bring_a_deleted_key_back(self):
        # Node 1 misses the delete and comes back (respawned after 40 failed probes) only after
        # the TTL; its peers keep the tombstone until it holds it too.
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=3, KV_CP_READ_QUORUM=2,
                         KV_CP_WRITE_QUORUM=2, KV_TOMBSTONE_TTL_S=1,
                         KV_CP_PROBE_FAILS=40).start(wait_nodes=3)
        self.assertEqual(c.put('k', 'v'), 200)
        wait_for(lambda: all(c.dump(n).get('k', ('',))[0] == 'v' for n in (1, 2, 3)), 10,
                 'k on every replica')
        c.kill_node(1)
        self.assertEqual(c.delete('k'), 200)
        wait_for(lambda: c.nodes()[1]['state'] != 'ACTIVE', 20, 'node 1 seen dead')
        c.wait_active(3, 30)
        self.assertEqual(c.get('k'), 404)
        time.sleep(2.5)
        self.assertEqual(c.get('k'), 404)
        for n in (1, 2, 3):
            self.assertNotEqual(c.dump(n).get('k', ('', None, True))[2], False, f'k live on {n}')

    def test_write_fails_with_two_home_replicas_down_and_no_other_node_takes_it(self):
        c = self.cluster(KV_CP_INITIAL_NODES=4, KV_CP_MAX_NODES=4, KV_CP_REPLICAS=3,
                         KV_CP_READ_QUORUM=2, KV_CP_WRITE_QUORUM=2, KV_CP_FORWARD_TIMEOUT_MS=300,
                         KV_CP_PROBE_FAILS=100).start(wait_nodes=4)
        self.assertEqual(c.put('k', 'v1'), 200)
        homes = [n for n in (1, 2, 3, 4) if 'k' in c.dump(n)]
        other = ({1, 2, 3, 4} - set(homes)).pop()
        self.assertEqual(len(homes), 3)
        pids = c.node_pids()
        for n in homes[:2]:
            os.kill(pids[n], signal.SIGSTOP)
        try:
            status, _, headers = c.req('POST', '/k', {'value': 'v2'})
        finally:
            for n in homes[:2]:
                os.kill(pids[n], signal.SIGCONT)
        self.assertEqual(status, 503)
        self.assertEqual(headers.get('retry-after'), '1')
        self.assertNotIn('k', c.dump(other))

    def test_write_fails_when_every_replica_is_stopped(self):
        c = self.cluster(KV_CP_INITIAL_NODES=2, KV_CP_REPLICAS=2, KV_CP_FORWARD_TIMEOUT_MS=300,
                         KV_CP_PROBE_FAILS=100).start()
        pids = c.node_pids()
        for pid in pids.values():
            os.kill(pid, signal.SIGSTOP)
        try:
            status, _, headers = c.req('POST', '/k', {'value': 'v'})
        finally:
            for pid in pids.values():
                os.kill(pid, signal.SIGCONT)
        self.assertEqual(status, 503)
        self.assertEqual(headers.get('retry-after'), '1')

    def test_dead_node_is_respawned_and_repaired_before_reads(self):
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=3, KV_CP_READ_QUORUM=2,
                         KV_CP_WRITE_QUORUM=2).start(wait_nodes=3)
        for i in range(100):
            c.put(f'k{i}', 'old')
        c.delete('k99')
        c.kill_node(1)
        wait_for(lambda: c.nodes()[1]['state'] != 'ACTIVE', 10, 'node 1 not ACTIVE')
        # Changes made while node 1 is down must reach it through the repair.
        wait_for(lambda: c.put('k0', 'new') == 200, 10, 'writes with node 1 down')
        for i in range(1, 50):
            self.assertEqual(c.put(f'k{i}', 'new'), 200)
        self.assertEqual(c.delete('k98'), 200)
        c.wait_active(3, 30)
        d1, d2, d3 = c.dump(1), c.dump(2), c.dump(3)
        self.assertEqual(d1, d2)
        self.assertEqual(d1, d3)
        self.assertEqual(d1['k0'][0], 'new')
        self.assertEqual(d1['k60'][0], 'old')
        self.assertTrue(d1['k98'][2] and d1['k99'][2])

    def test_503_carries_retry_after(self):
        c = self.cluster().start()
        c.kill_node(1)
        status, _, headers = c.req('POST', '/k', {'value': 'v'})
        self.assertEqual(status, 503)
        self.assertEqual(headers.get('retry-after'), '1')

    def test_rf1_respawned_node_keeps_its_volume(self):
        c = self.cluster(KV_SYNC_ACK=1).start()
        for i in range(20):
            c.put(f'k{i}', f'v{i}')
        c.kill_node(1)
        wait_for(lambda: c.nodes()[1]['state'] != 'ACTIVE', 10, 'node 1 not ACTIVE')
        c.wait_active(1, 30)
        for i in range(20):
            self.assertEqual(c.get(f'k{i}'), f'v{i}')


class Writer(threading.Thread):
    """Writes and deletes keys w<i> through cp until stopped; keeps the last acked op per key."""

    def __init__(self, c, keys=40):
        super().__init__(daemon=True)
        self.c, self.keys, self.stop_flag, self.acked, self.errors = c, keys, False, {}, 0
        c.writers.append(self)

    def run(self):
        i = 0
        while not self.stop_flag:
            key = f'w{i % self.keys}'
            try:
                if i % 7 == 3:
                    status, value = self.c.delete(key), None
                    ok = status in (200, 404)
                else:
                    value = f'v{i}'
                    ok = self.c.put(key, value) == 200
            except OSError:
                ok = False
            if ok:
                self.acked[key] = value
            else:
                # Not acked: the key may hold the old or the new value.
                self.acked.pop(key, None)
                self.errors += 1
            i += 1

    def stop(self):
        self.stop_flag = True
        self.join(10)


def check_invariants(test, c, expected, deleted, replicas):
    """Acked writes readable, deleted keys gone, and every key's replicas hold one row."""
    for key, value in expected.items():
        got = c.get(key)
        if value is None:
            test.assertEqual(got, 404, f'deleted key {key} came back')
        else:
            test.assertEqual(got, value, f'acked write of {key} lost')
    for key in deleted:
        test.assertEqual(c.get(key), 404, f'deleted key {key} came back')
    ring = json.loads(c.req('GET', '/_cp/ring')[1])['nodes']
    dumps = {n: c.dump(n) for n in ring}
    keys = set().union(*[set(d) for d in dumps.values()])
    for key in keys:
        rows = [dumps[n][key] for n in ring if key in dumps[n]]
        live = {r for r in rows}
        test.assertEqual(len(live), 1, f'replicas of {key} differ: {rows}')
        test.assertEqual(len(rows), min(replicas, len(ring)), f'{key} on {len(rows)} nodes')


class JoinTest(ClusterTest):
    def test_manual_join_moves_keys_without_losing_writes(self):
        c = self.cluster(KV_CP_MAX_NODES=2).start()
        for i in range(300):
            c.put(f'k{i}', f'v{i}')
        for i in range(0, 300, 10):
            c.delete(f'k{i}')
        w = Writer(c)
        w.start()
        time.sleep(0.3)
        status, body, _ = c.req('POST', '/_cp/nodes')
        self.assertEqual((status, body), (202, '{"queued":true}'))
        c.wait_active(2, 30)
        time.sleep(0.3)
        w.stop()
        self.assertEqual(w.errors, 0)
        expected = {f'k{i}': (None if i % 10 == 0 else f'v{i}') for i in range(300)}
        expected.update(w.acked)
        check_invariants(self, c, expected, [], 1)
        live = lambda d: {k for k, r in d.items() if not r[2]}
        on2 = live(c.dump(2))
        on1 = live(c.dump(1))
        self.assertFalse(on1 & on2, 'a key is on both nodes with RF=1')
        self.assertTrue(60 < len({k for k in on2 if k.startswith('k')}) < 210, len(on2))
        self.assertEqual(c.req('POST', '/_cp/nodes')[0], 409)

    def test_join_with_replicas(self):
        c = self.cluster(KV_CP_INITIAL_NODES=2, KV_CP_REPLICAS=2, KV_CP_MAX_NODES=3).start()
        for i in range(200):
            c.put(f'k{i}', f'v{i}')
        w = Writer(c)
        w.start()
        c.req('POST', '/_cp/nodes')
        c.wait_active(3, 30)
        w.stop()
        self.assertEqual(w.errors, 0)
        expected = {f'k{i}': f'v{i}' for i in range(200)}
        expected.update(w.acked)
        check_invariants(self, c, expected, [], 2)
        self.assertGreater(len(c.dump(3)), 50)

    def test_autoscale_adds_a_node_under_load(self):
        c = self.cluster(KV_CP_MAX_NODES=2, KV_CP_SCALE_RPS=20, KV_CP_SCALE_WINDOW_S=1).start()
        deadline = time.monotonic() + 15
        while len(c.nodes()) < 2 and time.monotonic() < deadline:
            c.put('hot', 'x')
        c.wait_active(2, 30)
        self.assertEqual(c.get('hot'), 'x')

    def test_no_autoscale_without_threshold(self):
        c = self.cluster(KV_CP_MAX_NODES=2, KV_CP_SCALE_WINDOW_S=1).start()
        end = time.monotonic() + 2.5
        while time.monotonic() < end:
            c.put('hot', 'x')
        self.assertEqual(len(c.nodes()), 1)


class RestartTest(ClusterTest):
    def test_cp_restart_keeps_nodes_and_data(self):
        c = self.cluster(KV_CP_INITIAL_NODES=2, KV_CP_REPLICAS=2).start()
        for i in range(50):
            c.put(f'k{i}', f'v{i}')
        pids = c.node_pids()
        c.stop(signal.SIGKILL)
        c.start()
        self.assertEqual(c.node_pids(), pids)
        for i in range(50):
            self.assertEqual(c.get(f'k{i}'), f'v{i}')
        # Versions stay above those before the restart, so new writes win.
        self.assertEqual(c.put('k1', 'newer'), 200)
        self.assertEqual(c.get('k1'), 'newer')
        self.assertEqual(c.dump(1)['k1'], c.dump(2)['k1'])


INF = float('inf')


def linearizable(ops):
    """Whether one key's history fits a register that starts empty (Wing-Gong search).

    ops: (start, end, kind, arg, result). kind 'put' (arg value), 'del', or 'get' (result value or
    None). A del result is True when the key existed. An op with no answer has end INF: it may or
    may not have taken effect, so the search may also leave it out.
    """
    ops = [o for o in ops if not (o[2] == 'get' and o[1] == INF)]
    n = len(ops)
    need = sum(1 << i for i, o in enumerate(ops) if o[1] != INF)
    seen = set()

    def step(done, value):
        if done & need == need:
            return True
        if (done, value) in seen:
            return False
        seen.add((done, value))
        first_end = min(ops[i][1] for i in range(n) if not done >> i & 1)
        for i in range(n):
            if done >> i & 1 or ops[i][0] > first_end:
                continue
            start, end, kind, arg, result = ops[i]
            if kind == 'put':
                nxt = arg
            elif kind == 'del':
                if end != INF and result != (value is not None):
                    continue
                nxt = None
            else:
                if result != value:
                    continue
                nxt = value
            if step(done | 1 << i, nxt):
                return True
        return False

    return step(0, None)


class History(threading.Thread):
    """One client: puts, deletes and gets on a few keys; records each op with its time span."""

    def __init__(self, c, name, keys, ops, log):
        super().__init__(daemon=True)
        self.c, self.name, self.keys, self.n, self.log = c, name, keys, ops, log

    def run(self):
        for i in range(self.n):
            key = self.keys[i % len(self.keys)]
            kind = ('put', 'get', 'del', 'get', 'put')[i % 5]
            arg = f'{self.name}-{i}' if kind == 'put' else None
            start = time.monotonic()
            try:
                if kind == 'put':
                    status = self.c.put(key, arg)
                    result = None
                elif kind == 'del':
                    status = self.c.delete(key)
                    result = status == 200
                    status = 200 if status == 404 else status
                else:
                    got = self.c.get(key)
                    status = 200 if got == 404 or isinstance(got, str) else got
                    result = got if isinstance(got, str) else None
            except OSError:
                status = 0
            end = time.monotonic() if status == 200 else INF
            self.log.append((key, (start, end, kind, arg, result)))
            time.sleep(0.03)


class LinearizabilityTest(ClusterTest):
    def test_checker_rejects_a_stale_read(self):
        self.assertTrue(linearizable([(0, 1, 'put', 'a', None), (2, 3, 'get', None, 'a')]))
        self.assertFalse(linearizable([(0, 1, 'put', 'a', None), (2, 3, 'get', None, None)]))
        self.assertFalse(linearizable([(0, 1, 'put', 'a', None), (2, 3, 'put', 'b', None),
                                       (4, 5, 'get', None, 'a')]))
        self.assertTrue(linearizable([(0, INF, 'put', 'a', None), (2, 3, 'get', None, None)]))
        self.assertFalse(linearizable([(0, 1, 'del', None, True)]))

    def check_history(self, log):
        by_key = {}
        for key, op in log:
            by_key.setdefault(key, []).append(op)
        self.assertGreater(sum(1 for _, o in log if o[1] != INF), 100)
        for key, ops in by_key.items():
            self.assertTrue(linearizable(ops), f'history of {key} is not linearizable: {ops}')

    def test_ops_on_one_key_are_linearizable_with_quorums_and_a_killed_node(self):
        c = self.cluster(KV_CP_INITIAL_NODES=3, KV_CP_REPLICAS=3, KV_CP_READ_QUORUM=2,
                         KV_CP_WRITE_QUORUM=2).start(wait_nodes=3)
        log = []
        clients = [History(c, f'c{n}', ['x', 'y', 'z'], 90, log) for n in range(4)]
        for t in clients:
            t.start()
        time.sleep(0.5)
        c.kill_node(1)
        for t in clients:
            t.join(120)
        self.check_history(log)

    def test_ops_on_one_key_are_linearizable_during_join_and_failure(self):
        c = self.cluster(KV_CP_INITIAL_NODES=2, KV_CP_REPLICAS=2, KV_CP_MAX_NODES=3).start()
        log = []
        clients = [History(c, f'c{n}', ['x', 'y', 'z'], 90, log) for n in range(4)]
        for t in clients:
            t.start()
        c.req('POST', '/_cp/nodes')
        time.sleep(0.5)
        c.kill_node(1)
        for t in clients:
            t.join(120)
        self.assertEqual(c.migration()['phase'], 'IDLE')
        self.assertEqual(len(c.nodes()), 3)
        self.check_history(log)


def wait_settled(c, count):
    """Wait for count ACTIVE ring nodes and no migration. The probes see a killed node only after
    a while, so every node must also answer its own health check, twice in a row."""
    def settled():
        nodes = c.nodes()
        return c.migration()['phase'] == 'IDLE' and len(nodes) == count and all(
            n['state'] == 'ACTIVE' and n['in_ring'] and c.node_req(i, 'GET', '/_internal/health')[0] == 200
            for i, n in nodes.items())
    wait_for(lambda: settled() and (time.sleep(0.5) or settled()), 90, 'cluster settles')


class FaultMatrixTest(ClusterTest):
    """A join with a node or cp killed when a phase starts keeps every invariant."""

    def run_repair_case(self, replicas, phase, victim):
        c = self.cluster(KV_CP_INITIAL_NODES=replicas, KV_CP_REPLICAS=replicas, KV_SYNC_ACK=1)
        c.start(KV_CP_FAULT=f'{phase}:{victim}')
        for i in range(100):
            c.put(f'k{i}', f'v{i}')
        c.delete('k5')
        w = Writer(c)
        w.start()
        c.kill_node(1)
        if victim == 'cp':
            c.proc.wait(30)
            del c.env['KV_CP_FAULT']
            c.start(wait_nodes=0)
        else:
            wait_for(lambda: c.nodes()[1]['state'] != 'ACTIVE', 10, 'node 1 seen dead')
        wait_settled(c, replicas)
        w.stop()
        expected = {f'k{i}': (None if i == 5 else f'v{i}') for i in range(100)}
        expected.update(w.acked)
        check_invariants(self, c, expected, [], replicas)

    def run_case(self, replicas, phase, victim):
        initial = replicas
        c = self.cluster(KV_CP_INITIAL_NODES=initial, KV_CP_REPLICAS=replicas,
                         KV_CP_MAX_NODES=initial + 1, KV_SYNC_ACK=1).start()
        for i in range(150):
            c.put(f'k{i}', f'v{i}')
        for i in range(0, 150, 15):
            c.delete(f'k{i}')
        c.stop()
        c.start(KV_CP_FAULT=f'{phase}:{victim}')
        w = Writer(c)
        w.start()
        c.req('POST', '/_cp/nodes')
        if victim == 'cp':
            c.proc.wait(30)
            del c.env['KV_CP_FAULT']
            c.start(wait_nodes=0)
        # An aborted join is retried, so the new node joins; but a cp restart before FLIP aborts the
        # join for good, as the admin request is not stored.
        aborted = victim == 'cp' and phase in ('SPAWN', 'COPY')
        wait_settled(c, initial if aborted else initial + 1)
        w.stop()
        expected = {f'k{i}': (None if i % 15 == 0 else f'v{i}') for i in range(150)}
        expected.update(w.acked)
        check_invariants(self, c, expected, [], replicas)


def add_matrix():
    for replicas in (1, 2, 3):
        for phase in ('SPAWN', 'COPY', 'FLIP', 'DRAIN', 'PURGE'):
            for victim in ('target', 'cp') + tuple(str(n) for n in range(1, replicas + 1)):
                def case(self, r=replicas, p=phase, v=victim):
                    self.run_case(r, p, v)
                setattr(FaultMatrixTest, f'test_rf{replicas}_{phase.lower()}_{victim}', case)
        for phase in ('RESPAWN', 'REPAIR'):
            for victim in ('target', 'cp'):
                def case(self, r=replicas, p=phase, v=victim):
                    self.run_repair_case(r, p, v)
                setattr(FaultMatrixTest, f'test_repair_rf{replicas}_{phase.lower()}_{victim}', case)


add_matrix()


if __name__ == '__main__':
    unittest.main(verbosity=2)

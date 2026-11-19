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
CP_PORT = 7980
BASE_PORT = 7990


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
        }
        self.env.update({k: str(v) for k, v in env.items()})
        self.proc = None

    def start(self, wait_nodes=None, **env):
        self.env.update({k: str(v) for k, v in env.items()})
        full = dict(os.environ)
        full.update(self.env)
        with open(os.path.join(self.dir, 'cp.stderr'), 'ab') as err:
            self.proc = subprocess.Popen([CP_BIN], env=full, stdout=subprocess.DEVNULL, stderr=err)
        n = wait_nodes if wait_nodes is not None else int(self.env.get('KV_CP_INITIAL_NODES', 1))
        if n:
            self.wait_active(n)
        return self

    def stop(self, sig=signal.SIGTERM):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(sig)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def close(self):
        self.stop(signal.SIGKILL)
        for pid in self.node_pids().values():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
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
        """Committed rows of one node: {key: (value, version, deleted)}."""
        self.node_req(node_id, 'POST', '/_internal/flush')
        rows, cursor = {}, ''
        while True:
            status, body, _ = self.node_req(node_id, 'GET', f'/_internal/dump?limit=1000&cursor={cursor}')
            assert status == 200, body
            page = json.loads(body)
            for it in page['items']:
                rows[it['key']] = (it['value'], it['version'], it['deleted'])
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


if __name__ == '__main__':
    unittest.main(verbosity=2)

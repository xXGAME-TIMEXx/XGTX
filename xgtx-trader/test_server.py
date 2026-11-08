import hashlib
import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from server import Trader, RPC, make_server, read_login

CHAIN, OWNER, TX, DRAFT = '11' * 32, '22' * 32, '33' * 32, '44' * 32


class FakeRPC:
    def __init__(self):
        self.owner = OWNER
        self.calls = []

    def call(self, method, params=None):
        self.calls.append((method, params))
        if method == 'xgtx_identity':
            return dict(owner=self.owner, chain=CHAIN, address='fixture-address')
        if method == 'xgtx_get_exchange':
            return dict(chain=CHAIN, tip=TX, assets=[], pools=[], native_backing=True)
        if method == 'get_info':
            return dict(height=123, offline=True)
        if method == 'get_balance':
            return dict(balance=(1 << 64) - 1, unlocked_balance=123)
        if method == 'xgtx_operation':
            return dict(draft_id=DRAFT, tx_hash=TX, fee='7', relayed=False, owner=self.owner, chain=CHAIN)
        if method == 'xgtx_relay_operation':
            return dict(draft_id=DRAFT, tx_hash=TX, fee='7', relayed=True)
        if method == 'refresh':
            return {}
        raise AssertionError('Unexpected method ' + method)


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.rpc = FakeRPC()
        self.trader = Trader(self.rpc, self.rpc)
        self.server = make_server(self.trader, 0, 'fixture-session')
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'

    def tearDown(self):
        self.server.shutdown();self.server.server_close();self.thread.join()

    def post(self, path, params, **headers):
        selected = {'Content-Type': 'application/json', 'Origin': self.url, 'Authorization': 'Bearer fixture-session', **headers}
        request = urllib.request.Request(self.url + path, json.dumps(params).encode(), selected)
        try:
            response = urllib.request.urlopen(request)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response), response.headers

    def payload(self):
        return dict(kind=1, quantity='18446744073709551615', ticker='TEST', max_fee='10', expected_owner=OWNER, expected_chain=CHAIN)

    def test_session_origin_host_and_rpc_allowlist(self):
        for headers, status in [({'Authorization': 'Bearer wrong'}, 401), ({'Origin': 'https://attacker.invalid'}, 403), ({'Host': 'attacker.invalid'}, 421)]:
            code, _, response = self.post('/api/preview', self.payload(), **headers)
            self.assertEqual(code, status)
            self.assertNotIn('Access-Control-Allow-Origin', response)
        code, _, _ = self.post('/api/query_key', {})
        self.assertEqual(code, 400)
        self.assertFalse(any(call[0] == 'xgtx_operation' for call in self.rpc.calls))

    def test_exact_draft_submission_and_repeat_do_not_rebuild_or_relay_twice(self):
        code, preview, _ = self.post('/api/preview', self.payload())
        self.assertEqual(code, 200)
        for _ in range(2):
            code, receipt, _ = self.post('/api/submit', {'draft_id': preview['draft_id']})
            self.assertEqual(code, 200);self.assertEqual(receipt['tx_hash'], preview['tx_hash'])
        operations = [call for call in self.rpc.calls if call[0] == 'xgtx_operation']
        self.assertEqual(len(operations), 1)
        self.assertEqual(operations[0][1]['quantity'], '18446744073709551615')
        self.assertTrue(operations[0][1]['do_not_relay'])
        self.assertNotIn('expected_owner', operations[0][1])
        self.assertEqual(len([call for call in self.rpc.calls if call[0] == 'xgtx_relay_operation']), 1)
        self.assertEqual(self.post('/api/submit', {'draft_id': DRAFT, 'tx_blob': 'replacement'})[0], 400)

    def test_invalid_numbers_and_wallet_switch_never_reach_signer(self):
        for update in ({'quantity': 42}, {'quantity': '01'}, {'quantity': '18446744073709551616'}, {'do_not_relay': False}, {'expected_owner': 'ff' * 32}):
            self.assertEqual(self.post('/api/preview', {**self.payload(), **update})[0], 400)
        self.assertFalse(any(call[0] == 'xgtx_operation' for call in self.rpc.calls))
        self.post('/api/preview', self.payload())
        self.rpc.owner = 'ff' * 32
        self.assertEqual(self.post('/api/submit', {'draft_id': DRAFT})[0], 400)
        self.assertFalse(any(call[0] == 'xgtx_relay_operation' for call in self.rpc.calls))

    def test_wallet_balances_leave_python_as_exact_decimal_strings(self):
        code, state, _ = self.post('/api/state', {})
        self.assertEqual(code, 200)
        self.assertEqual(state['wallet']['balance'], '18446744073709551615')
        self.assertEqual(self.post('/api/state', {'tip': '00' * 32})[0], 400)

    def test_uncertain_submission_cannot_trigger_automatic_retry(self):
        self.post('/api/preview', self.payload())
        original = self.rpc.call
        def failure(method, params=None):
            if method == 'xgtx_relay_operation':
                self.rpc.calls.append((method, params));raise RuntimeError('Timeout after send')
            return original(method, params)
        self.rpc.call = failure
        self.assertEqual(self.post('/api/submit', {'draft_id': DRAFT})[0], 502)
        self.assertEqual(self.post('/api/submit', {'draft_id': DRAFT})[0], 400)
        self.assertEqual(len([call for call in self.rpc.calls if call[0] == 'xgtx_relay_operation']), 1)

    def test_static_source_cannot_expose_server_or_credentials(self):
        with urllib.request.urlopen(self.url + '/') as response:
            self.assertEqual(response.status, 200)
            self.assertIn("frame-ancestors 'none'", response.headers['Content-Security-Policy'])
        with self.assertRaises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(self.url + '/server.py')
        self.assertEqual(error.exception.code, 404)


class RPCConfigurationTests(unittest.TestCase):
    def test_remote_urls_credentials_and_paths_are_refused(self):
        for value in ('https://example.com', 'http://localhost:48081', 'http://127.0.0.1:48081/path', 'http://user:password@127.0.0.1:48081'):
            with self.assertRaises(ValueError):RPC(value)
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / 'login.json';file.write_text('{"username":"test","password":"fixture"}')
            file.chmod(0o644)
            with self.assertRaises(ValueError):read_login(str(file), False)
            file.chmod(0o600)
            self.assertEqual(read_login(str(file), False)['username'], 'test')

    def test_digest_challenge_is_answered_without_putting_credentials_in_url(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):pass
            def do_POST(self):
                length = int(self.headers['Content-Length']);self.rfile.read(length)
                auth = self.headers.get('Authorization', '')
                if not auth:
                    self.send_response(401);self.send_header('WWW-Authenticate', 'Digest realm="xgtx", nonce="fixture-nonce", algorithm=MD5, qop="auth"');self.end_headers();return
                import re
                values = dict((key, quoted or bare) for key, quoted, bare in re.findall(r'(\w+)=(?:"([^"]*)"|([^, ]+))', auth))
                md5 = lambda value: hashlib.md5(value.encode()).hexdigest()
                expected = md5(':'.join([md5('test:xgtx:fixture'), 'fixture-nonce', values['nc'], values['cnonce'], 'auth', md5('POST:/json_rpc')]))
                result = {'result': {'authenticated': values['response'] == expected}}
                data = json.dumps(result).encode();self.send_response(200);self.send_header('Content-Length', str(len(data)));self.end_headers();self.wfile.write(data)
        server = HTTPServer(('127.0.0.1', 0), Handler);thread = threading.Thread(target=server.serve_forever, daemon=True);thread.start()
        try:
            rpc = RPC(f'http://127.0.0.1:{server.server_port}', {'username': 'test', 'password': 'fixture'})
            self.assertTrue(rpc.call('test')['authenticated'])
            self.assertNotIn('fixture', rpc.url)
        finally:server.shutdown();server.server_close();thread.join()


if __name__ == '__main__':
    unittest.main()

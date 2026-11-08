#!/usr/bin/env python3
"""Loopback-only XGTX wallet UI. No signing keys or arbitrary RPC passthrough."""
import argparse
import hmac
import json
import re
import secrets
import stat
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HEX = re.compile(r"[0-9a-fA-F]{64}\Z")
UINT64 = (1 << 64) - 1


def decimal(value, positive=False):
    if not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]{0,19}", value):
        raise ValueError("Amounts must be canonical decimal strings")
    number = int(value)
    if number > UINT64 or (positive and not number):
        raise ValueError("Amount outside permitted range")
    return number


class RPC:
    def __init__(self, url, login=None):
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != "http" or parsed.hostname != "127.0.0.1" or
                parsed.username or parsed.password or parsed.path not in ("", "/") or
                parsed.query or parsed.fragment or not parsed.port):
            raise ValueError("RPC URL must be http://127.0.0.1:PORT")
        self.url = url.rstrip("/") + "/json_rpc"
        handlers = [urllib.request.ProxyHandler({})]
        if login:
            manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            manager.add_password(None, url, login["username"], login["password"])
            handlers.append(urllib.request.HTTPDigestAuthHandler(manager))
        self.opener = urllib.request.build_opener(*handlers)

    def call(self, method, params=None):
        request = urllib.request.Request(self.url, json.dumps({"jsonrpc": "2.0", "id": "trader", "method": method, "params": params or {}}).encode(), {"Content-Type": "application/json"})
        try:
            with self.opener.open(request, timeout=120) as response:
                data = response.read(4 * 1024 * 1024 + 1)
        except (urllib.error.URLError, TimeoutError) as error:
            raise RuntimeError("RPC unavailable or timed out; check the local node/wallet. Check transaction status before retrying a submission.") from error
        if len(data) > 4 * 1024 * 1024:
            raise RuntimeError("RPC response exceeds interface limit")
        result = json.loads(data)
        if result.get("error"):
            raise RuntimeError(str(result["error"].get("message", "RPC rejected request"))[:512])
        if not isinstance(result.get("result"), dict):
            raise RuntimeError("Unexpected RPC response")
        return result["result"]


class Trader:
    def __init__(self, daemon, wallet):
        self.daemon, self.wallet = daemon, wallet
        self.drafts = {}
        self.lock = threading.Lock()

    def identity(self):
        identity = self.wallet.call("xgtx_identity")
        if not HEX.fullmatch(identity.get("owner", "")) or not HEX.fullmatch(identity.get("chain", "")):
            raise RuntimeError("Wallet identity API unavailable; install Phase29")
        return identity

    def state(self, params):
        identity = self.identity()
        page = self.daemon.call("xgtx_get_exchange", {"owner": identity["owner"], "limit": 100,
            "after_asset": params.get("after_asset", ""), "after_pool": params.get("after_pool", "")})
        if page.get("chain") != identity["chain"]:
            raise RuntimeError("Wallet and exchange use different chains")
        if params.get("tip") and params["tip"] != page.get("tip"):
            raise ValueError("Chain changed while paging; refresh balances")
        result = {"identity": identity, "exchange": page}
        if not params:
            info = self.daemon.call("get_info")
            balance = self.wallet.call("get_balance", {"account_index": 0})
            result["node"] = {key: info.get(key) for key in ("height", "offline", "incoming_connections_count", "outgoing_connections_count")}
            result["wallet"] = {key: str(balance.get(key, 0)) for key in ("balance", "unlocked_balance")}
        return result

    def dispatch(self, route, params):
        if not isinstance(params, dict):
            raise ValueError("Expected JSON object")
        with self.lock:
            if route == "/api/state":
                if set(params) - {"after_asset", "after_pool", "tip"}:
                    raise ValueError("Unexpected state fields")
                for key in ("after_asset", "after_pool", "tip"):
                    if params.get(key) and not HEX.fullmatch(params[key]):
                        raise ValueError("Invalid page identifier")
                return self.state(params)
            if route == "/api/refresh":
                if params:
                    raise ValueError("Unexpected refresh fields")
                self.wallet.call("refresh")
                return self.state({})
            if route == "/api/quote":
                if set(params) != {"asset_in", "asset_out", "amount_in"}:
                    raise ValueError("Invalid quote fields")
                if not all(isinstance(params[x], str) and HEX.fullmatch(params[x]) for x in ("asset_in", "asset_out")):
                    raise ValueError("Invalid quote assets")
                decimal(params["amount_in"], True)
                result = self.daemon.call("xgtx_quote_swap", params)
                if result.get("chain") != self.identity()["chain"]:
                    raise RuntimeError("Quote and wallet use different chains")
                return result
            if route == "/api/preview":
                allowed = {"kind", "quantity", "asset", "other", "recipient", "limit", "minimum", "ticker", "decimals", "max_fee", "expected_owner", "expected_chain"}
                if set(params) - allowed or type(params.get("kind")) is not int or not 1 <= params["kind"] <= 9:
                    raise ValueError("Invalid operation")
                decimal(params.get("quantity"), True)
                maximum = decimal(params.get("max_fee"), True)
                for key in ("limit", "minimum"):
                    if key in params:
                        decimal(params[key])
                for key in ("asset", "other"):
                    if key in params and (not isinstance(params[key], str) or not HEX.fullmatch(params[key])):
                        raise ValueError("Invalid asset identifier")
                for key, length in (("recipient", 200), ("ticker", 12)):
                    if key in params and (not isinstance(params[key], str) or len(params[key]) > length):
                        raise ValueError("Invalid recipient or ticker")
                if "decimals" in params and (type(params["decimals"]) is not int or not 0 <= params["decimals"] <= 12):
                    raise ValueError("Invalid decimal precision")
                identity = self.identity()
                if params.get("expected_owner") != identity["owner"] or params.get("expected_chain") != identity["chain"]:
                    raise ValueError("Open wallet changed; refresh before previewing")
                operation = {key: value for key, value in params.items() if key not in ("expected_owner", "expected_chain")}
                result = self.wallet.call("xgtx_operation", {**operation, "do_not_relay": True, "deadline_window": 20})
                if (result.get("relayed") is not False or not HEX.fullmatch(result.get("draft_id", "")) or
                        not HEX.fullmatch(result.get("tx_hash", "")) or decimal(result.get("fee"), True) > maximum or
                        result.get("chain") != identity["chain"] or result.get("owner") != identity["owner"]):
                    raise RuntimeError("Wallet preview does not match required constraints")
                if params["kind"] == 9 and decimal(result.get("payout"), True) + decimal(result["fee"]) != decimal(params["quantity"]):
                    raise RuntimeError("Withdrawal payout and fee differ from approved total debit")
                # The native wallet retains its selected inputs and transaction
                # keys. Browser receives no keys and cannot replace draft bytes.
                if len(self.drafts) >= 8:
                    self.drafts.pop(next(iter(self.drafts)))
                self.drafts[result["draft_id"]] = {"preview": result, "owner": identity["owner"], "chain": identity["chain"], "state": "ready"}
                return result
            if route == "/api/submit":
                if set(params) != {"draft_id"} or not isinstance(params["draft_id"], str):
                    raise ValueError("Submit accepts only a retained draft identifier")
                draft = self.drafts.get(params["draft_id"])
                if not draft:
                    raise ValueError("Draft missing; preview again")
                if draft["state"] == "submitted":
                    return draft["receipt"]
                if draft["state"] != "ready":
                    raise ValueError("Submission outcome uncertain. Check the transaction hash; do not submit a replacement automatically.")
                identity = self.identity()
                if (identity["owner"], identity["chain"]) != (draft["owner"], draft["chain"]):
                    raise ValueError("Open wallet changed; preview again")
                draft["state"] = "submitting"
                try:
                    result = self.wallet.call("xgtx_relay_operation", params)
                    if result.get("tx_hash") != draft["preview"]["tx_hash"] or result.get("fee") != draft["preview"]["fee"] or result.get("relayed") is not True:
                        raise RuntimeError("Relay response differs from reviewed transaction; check its hash")
                except Exception:
                    draft["state"] = "uncertain"
                    raise
                draft["state"] = "submitted"
                draft["receipt"] = result
                return result
            raise ValueError("Unknown interface action")


def make_server(trader, port=48190, token=None):
    token = token or secrets.token_urlsafe(32)

    class Handler(BaseHTTPRequestHandler):
        server_version = "XGTX-local"

        def setup(self):
            super().setup()
            self.connection.settimeout(15)

        def log_message(self, *_):
            pass

        def headers_out(self, status, mime, data):
            self.send_response(status)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'")
            self.end_headers()
            self.wfile.write(data)

        def answer(self, status, value):
            self.headers_out(status, "application/json", json.dumps(value).encode())

        def host_ok(self):
            return self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}"

        def do_GET(self):
            if not self.host_ok():
                self.answer(421, {"error": "Invalid local host"});return
            assets = {"/": ("index.html", "text/html; charset=utf-8"), "/app.mjs": ("app.mjs", "text/javascript"), "/amounts.mjs": ("amounts.mjs", "text/javascript"), "/style.css": ("style.css", "text/css")}
            item = assets.get(self.path)
            if not item:
                self.answer(404, {"error": "Not found"});return
            self.headers_out(200, item[1], (ROOT / item[0]).read_bytes())

        def do_POST(self):
            if not self.host_ok():
                self.answer(421, {"error": "Invalid local host"});return
            if self.headers.get("Origin") != f"http://127.0.0.1:{self.server.server_port}":
                self.answer(403, {"error": "Invalid local origin"});return
            provided = self.headers.get("Authorization", "")
            if not provided.isascii() or not hmac.compare_digest(provided, "Bearer " + token):
                self.answer(401, {"error": "Open the session link printed by the local launcher"});return
            if self.headers.get_content_type() != "application/json":
                self.answer(415, {"error": "JSON required"});return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 16384:
                    raise ValueError("Request exceeds interface limit")
                payload = self.rfile.read(length)
                result = trader.dispatch(self.path, json.loads(payload))
                self.answer(200, result)
            except (ValueError, TypeError, KeyError) as error:
                self.answer(400, {"error": str(error)[:512]})
            except Exception as error:
                self.answer(502, {"error": str(error)[:512]})

    server = HTTPServer(("127.0.0.1", port), Handler)
    server.token = token
    return server


def read_login(path, allow_empty):
    if not path:
        if allow_empty:
            return None
        raise ValueError("Provide RPC login files; unauthenticated RPC is allowed only with explicit fixture option")
    source = Path(path).expanduser()
    if not source.is_file() or stat.S_IMODE(source.stat().st_mode) & 0o077:
        raise ValueError("RPC login file must be private (chmod 600)")
    data = json.loads(source.read_text())
    if set(data) != {"username", "password"} or not all(isinstance(x, str) and x for x in data.values()):
        raise ValueError("Login file requires username and password")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon-url", default="http://127.0.0.1:48081")
    parser.add_argument("--wallet-url", default="http://127.0.0.1:48083")
    parser.add_argument("--daemon-login-file")
    parser.add_argument("--wallet-login-file")
    parser.add_argument("--allow-unauthenticated-fixture", action="store_true")
    parser.add_argument("--port", type=int, default=48190)
    args = parser.parse_args()
    trader = Trader(RPC(args.daemon_url, read_login(args.daemon_login_file, args.allow_unauthenticated_fixture)), RPC(args.wallet_url, read_login(args.wallet_login_file, args.allow_unauthenticated_fixture)))
    if args.allow_unauthenticated_fixture and trader.daemon.call("get_info").get("offline") is not True:
        raise ValueError("Unauthenticated fixture mode requires an offline node")
    server = make_server(trader, args.port)
    print(f"Open locally: http://127.0.0.1:{server.server_port}/#token={server.token}", flush=True)
    print("Local wallet control only. Ctrl+C closes this session. No public network activation.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""XGTX public market snapshot indexer and read-only HTTP API."""

from __future__ import annotations

import argparse
import json
import re
import signal
import sqlite3
import threading
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from decimal import Decimal, getcontext
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

getcontext().prec = 80
HEX64 = re.compile(r"^[0-9a-f]{64}$")
NATIVE_ID = "0" * 64
EXPECTED_CHAIN = "6cc86a50eca6febe795dc25490d02f29fe8a5590e93daf03d1e0c368191d7435"


def atomic(value: str, decimals: int) -> Decimal:
    if not isinstance(value, str) or not value.isdigit() or not 0 <= decimals <= 18:
        raise ValueError("invalid atomic amount")
    return Decimal(value) / (Decimal(10) ** decimals)


def validate_state(state: dict) -> dict:
    if state.get("chain") != EXPECTED_CHAIN or not HEX64.fullmatch(str(state.get("tip", ""))):
        raise ValueError("unexpected chain or tip")
    if not isinstance(state.get("height"), int) or state["height"] < 1:
        raise ValueError("invalid height")
    assets = state.get("assets")
    pools = state.get("pools")
    if not isinstance(assets, list) or not isinstance(pools, list) or len(assets) > 10000 or len(pools) > 10000:
        raise ValueError("invalid market collection")
    known = set()
    for asset in assets:
        if not HEX64.fullmatch(str(asset.get("id", ""))) or not HEX64.fullmatch(str(asset.get("issuer", ""))):
            raise ValueError("invalid asset identity")
        ticker = asset.get("ticker")
        if not isinstance(ticker, str) or not 1 <= len(ticker) <= 12 or not ticker.isascii():
            raise ValueError("invalid ticker")
        decimals = asset.get("decimals")
        atomic(asset.get("supply", ""), decimals)
        holders = asset.get("holders")
        if holders is not None and (not isinstance(holders, str) or not holders.isdigit() or int(holders) > 18446744073709551615):
            raise ValueError("invalid holder count")
        treasury_balance = asset.get("treasury_balance")
        if treasury_balance is not None:
            atomic(treasury_balance, decimals)
        if asset["id"] in known:
            raise ValueError("duplicate asset")
        known.add(asset["id"])
    for pool in pools:
        if not all(HEX64.fullmatch(str(pool.get(key, ""))) for key in ("id", "asset_a", "asset_b")):
            raise ValueError("invalid pool identity")
        if pool["asset_a"] == pool["asset_b"] or pool["asset_a"] not in known or pool["asset_b"] not in known:
            raise ValueError("invalid pool assets")
        for key in ("reserve_a", "reserve_b", "shares"):
            if not isinstance(pool.get(key), str) or not pool[key].isdigit():
                raise ValueError("invalid pool amount")
    for key in ("lp_fee_bps", "protocol_fee_bps", "peers"):
        if not isinstance(state.get(key), int) or state[key] < 0:
            raise ValueError("invalid network metric")
    return state


SCHEMA = """
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS snapshots(
  id INTEGER PRIMARY KEY, observed_at INTEGER NOT NULL, chain TEXT NOT NULL,
  height INTEGER NOT NULL, tip TEXT NOT NULL, synchronized INTEGER NOT NULL,
  peers INTEGER NOT NULL, lp_fee_bps INTEGER NOT NULL, protocol_fee_bps INTEGER NOT NULL,
  UNIQUE(height, tip)
);
CREATE TABLE IF NOT EXISTS assets(
  snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
  asset_id TEXT NOT NULL, issuer TEXT NOT NULL, ticker TEXT NOT NULL,
  decimals INTEGER NOT NULL, supply_atomic TEXT NOT NULL,
  holder_count TEXT, treasury_balance_atomic TEXT,
  PRIMARY KEY(snapshot_id, asset_id)
);
CREATE TABLE IF NOT EXISTS pools(
  snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
  pool_id TEXT NOT NULL, asset_a TEXT NOT NULL, asset_b TEXT NOT NULL,
  reserve_a_atomic TEXT NOT NULL, reserve_b_atomic TEXT NOT NULL, shares_atomic TEXT NOT NULL,
  PRIMARY KEY(snapshot_id, pool_id)
);
CREATE INDEX IF NOT EXISTS snapshots_observed ON snapshots(observed_at DESC);
CREATE INDEX IF NOT EXISTS pools_identity ON pools(pool_id, snapshot_id);
"""


@dataclass
class Indexer:
    database: Path

    def connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.database, timeout=15)
        connection.row_factory = sqlite3.Row
        connection.executescript(SCHEMA)
        columns = {row[1] for row in connection.execute("PRAGMA table_info(assets)")}
        if "holder_count" not in columns:
            connection.execute("ALTER TABLE assets ADD COLUMN holder_count TEXT")
        if "treasury_balance_atomic" not in columns:
            connection.execute("ALTER TABLE assets ADD COLUMN treasury_balance_atomic TEXT")
        return connection

    def record(self, source: dict, observed_at: int | None = None) -> bool:
        state = validate_state(source)
        observed_at = int(observed_at or time.time())
        with self.connect() as db:
            existing = db.execute("SELECT id FROM snapshots WHERE height=? AND tip=?", (state["height"], state["tip"])).fetchone()
            if existing:
                snapshot_id = existing["id"]
                stored = {row["asset_id"] for row in db.execute("SELECT asset_id FROM assets WHERE snapshot_id=?", (snapshot_id,))}
                incoming = {asset["id"] for asset in state["assets"]}
                if stored != incoming:
                    raise ValueError("duplicate snapshot asset set differs")
                db.executemany(
                    "UPDATE assets SET holder_count=COALESCE(?,holder_count),treasury_balance_atomic=COALESCE(?,treasury_balance_atomic) WHERE snapshot_id=? AND asset_id=?",
                    [(asset.get("holders"), asset.get("treasury_balance"), snapshot_id, asset["id"]) for asset in state["assets"]],
                )
                return False
            cursor = db.execute(
                "INSERT INTO snapshots(observed_at,chain,height,tip,synchronized,peers,lp_fee_bps,protocol_fee_bps) VALUES(?,?,?,?,?,?,?,?)",
                (observed_at, state["chain"], state["height"], state["tip"], int(bool(state.get("synchronized"))),
                 state["peers"], state["lp_fee_bps"], state["protocol_fee_bps"]),
            )
            snapshot_id = cursor.lastrowid
            db.executemany(
                "INSERT INTO assets(snapshot_id,asset_id,issuer,ticker,decimals,supply_atomic,holder_count,treasury_balance_atomic) VALUES(?,?,?,?,?,?,?,?)",
                [(snapshot_id, a["id"], a["issuer"], a["ticker"], a["decimals"], a["supply"],
                  a.get("holders"), a.get("treasury_balance")) for a in state["assets"]],
            )
            db.executemany(
                "INSERT INTO pools VALUES(?,?,?,?,?,?,?)",
                [(snapshot_id, p["id"], p["asset_a"], p["asset_b"], p["reserve_a"], p["reserve_b"], p["shares"]) for p in state["pools"]],
            )
        return True

    def latest(self) -> dict | None:
        with self.connect() as db:
            snapshot = db.execute("SELECT * FROM snapshots ORDER BY observed_at DESC,id DESC LIMIT 1").fetchone()
            if not snapshot:
                return None
            assets = [dict(row) for row in db.execute("SELECT asset_id,issuer,ticker,decimals,supply_atomic,holder_count,treasury_balance_atomic FROM assets WHERE snapshot_id=? ORDER BY asset_id", (snapshot["id"],))]
            pools = [dict(row) for row in db.execute("SELECT pool_id,asset_a,asset_b,reserve_a_atomic,reserve_b_atomic,shares_atomic FROM pools WHERE snapshot_id=? ORDER BY pool_id", (snapshot["id"],))]
            return {**dict(snapshot), "assets": assets, "pools": pools, "coverage": coverage(assets)}

    def market_history(self, asset_id: str, limit: int = 500) -> list[dict]:
        if not HEX64.fullmatch(asset_id) or asset_id == NATIVE_ID:
            raise ValueError("invalid non-native asset id")
        limit = max(1, min(limit, 5000))
        query = """
          SELECT s.observed_at,s.height,s.tip,a.decimals AS a_decimals,n.decimals AS n_decimals,
                 p.pool_id,p.asset_a,p.asset_b,p.reserve_a_atomic,p.reserve_b_atomic
          FROM snapshots s JOIN pools p ON p.snapshot_id=s.id
          JOIN assets a ON a.snapshot_id=s.id AND a.asset_id=?
          JOIN assets n ON n.snapshot_id=s.id AND n.asset_id=?
          WHERE (p.asset_a=? AND p.asset_b=?) OR (p.asset_a=? AND p.asset_b=?)
          ORDER BY s.observed_at DESC,s.id DESC LIMIT ?
        """
        with self.connect() as db:
            rows = db.execute(query, (asset_id, NATIVE_ID, asset_id, NATIVE_ID, NATIVE_ID, asset_id, limit)).fetchall()
        history = []
        for row in reversed(rows):
            native_atomic = row["reserve_a_atomic"] if row["asset_a"] == NATIVE_ID else row["reserve_b_atomic"]
            asset_atomic = row["reserve_a_atomic"] if row["asset_a"] == asset_id else row["reserve_b_atomic"]
            native = atomic(native_atomic, row["n_decimals"])
            asset = atomic(asset_atomic, row["a_decimals"])
            if asset == 0:
                continue
            history.append({"observed_at": row["observed_at"], "height": row["height"], "tip": row["tip"],
                            "pool_id": row["pool_id"], "price_xgtx": str(native / asset),
                            "native_depth": str(native * 2)})
        return history

    def candles(self, asset_id: str, interval: int, limit: int) -> list[dict]:
        interval = max(60, min(interval, 86400))
        samples = self.market_history(asset_id, min(max(limit, 1) * 100, 5000))
        buckets: dict[int, list[Decimal]] = {}
        for sample in samples:
            bucket = sample["observed_at"] // interval * interval
            buckets.setdefault(bucket, []).append(Decimal(sample["price_xgtx"]))
        result = []
        for bucket in sorted(buckets)[-max(1, min(limit, 1000)):]:
            values = buckets[bucket]
            result.append({"time": bucket, "open": str(values[0]), "high": str(max(values)),
                           "low": str(min(values)), "close": str(values[-1]), "samples": len(values),
                           "volume_xgtx": None})
        return result


def coverage(assets: list[dict] | None = None) -> dict:
    has_holders = bool(assets) and all(asset.get("holder_count") is not None for asset in assets)
    has_treasury = bool(assets) and all(asset.get("treasury_balance_atomic") is not None for asset in assets)
    return {"prices_xgtx": True, "liquidity_history": True, "candles": True,
            "usd_price": False, "transaction_volume": False, "holder_count": has_holders,
            "treasury_balance": has_treasury, "treasury_activity": False,
            "note": "Unavailable fields remain null until authoritative daemon event APIs exist."}


def fetch_json(url: str, timeout: int = 10) -> dict:
    request = urllib.request.Request(url, headers={"Accept": "application/json", "User-Agent": "xgtx-indexer/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        if response.status != 200 or response.headers.get_content_type() != "application/json":
            raise RuntimeError("market source returned an unexpected response")
        return json.load(response)


class API(BaseHTTPRequestHandler):
    indexer: Indexer

    def send_json(self, status: int, value: object) -> None:
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        try:
            if parsed.path == "/health":
                latest = self.indexer.latest()
                self.send_json(200 if latest else 503, {"ok": bool(latest), "latest_height": latest and latest["height"]})
            elif parsed.path == "/v1/summary":
                latest = self.indexer.latest()
                self.send_json(200 if latest else 503, latest or {"error": "no snapshots recorded"})
            elif parsed.path == "/v1/history":
                self.send_json(200, {"asset_id": params.get("asset_id", [""])[0], "samples": self.indexer.market_history(
                    params.get("asset_id", [""])[0], int(params.get("limit", ["500"])[0]))})
            elif parsed.path == "/v1/candles":
                asset_id = params.get("asset_id", [""])[0]
                latest = self.indexer.latest()
                self.send_json(200, {"asset_id": asset_id, "quote": "XGTX", "candles": self.indexer.candles(
                    asset_id, int(params.get("interval", ["300"])[0]), int(params.get("limit", ["288"])[0])),
                    "coverage": coverage((latest or {}).get("assets", []))})
            else:
                self.send_json(404, {"error": "not found"})
        except (ValueError, OverflowError) as error:
            self.send_json(400, {"error": str(error)})
        except Exception:
            self.send_json(500, {"error": "indexer request failed"})

    def log_message(self, format: str, *args: object) -> None:
        return


def poll(indexer: Indexer, source_url: str, interval: int, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            indexer.record(fetch_json(source_url))
        except Exception as error:
            print(f"indexer poll failed: {error}", flush=True)
        stop.wait(interval)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-url", default="https://xgtxchain.com/dex/api/state")
    parser.add_argument("--database", default="xgtx-indexer.sqlite3")
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=48400)
    parser.add_argument("--interval", type=int, default=30)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    if not 10 <= args.interval <= 3600 or not 1 <= args.port <= 65535:
        parser.error("interval or port outside allowed range")
    indexer = Indexer(Path(args.database))
    if args.once:
        print(json.dumps({"recorded": indexer.record(fetch_json(args.source_url)), "latest": indexer.latest()}, indent=2))
        return
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    thread = threading.Thread(target=poll, args=(indexer, args.source_url, args.interval, stop), daemon=True)
    thread.start()
    API.indexer = indexer
    server = ThreadingHTTPServer((args.listen, args.port), API)
    try:
        server.serve_forever(poll_interval=.5)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set(); server.shutdown(); thread.join(timeout=5)


if __name__ == "__main__":
    main()

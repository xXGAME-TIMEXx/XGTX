import tempfile
import unittest
import sqlite3
from pathlib import Path

from xgtx_indexer import EXPECTED_CHAIN, Indexer, NATIVE_ID, validate_state

TOKEN = "1" * 64
POOL = "2" * 64
ISSUER = "3" * 64


def state(height=100, tip="4" * 64, native="10000000000", token="2000000"):
    return {"chain": EXPECTED_CHAIN, "tip": tip, "height": height, "synchronized": True, "peers": 2,
            "lp_fee_bps": 25, "protocol_fee_bps": 5,
            "assets": [{"id": NATIVE_ID, "issuer": EXPECTED_CHAIN, "ticker": "XGTX", "decimals": 10, "supply": "10000000000"},
                       {"id": TOKEN, "issuer": ISSUER, "ticker": "TEST", "decimals": 6, "supply": "9000000"}],
            "pools": [{"id": POOL, "asset_a": NATIVE_ID, "asset_b": TOKEN,
                       "reserve_a": native, "reserve_b": token, "shares": "100"}]}


class IndexerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.indexer = Indexer(Path(self.temp.name) / "index.sqlite3")

    def tearDown(self):
        self.temp.cleanup()

    def test_records_exact_snapshot_and_rejects_duplicate(self):
        self.assertTrue(self.indexer.record(state(), 1000))
        self.assertFalse(self.indexer.record(state(), 1001))
        latest = self.indexer.latest()
        self.assertEqual(latest["height"], 100)
        self.assertEqual(latest["assets"][1]["supply_atomic"], "9000000")
        self.assertFalse(latest["coverage"]["holder_count"])
        enriched = state()
        for asset in enriched["assets"]:
            asset.update({"holders": "2", "treasury_balance": "50"})
        self.assertFalse(self.indexer.record(enriched, 1002))
        latest = self.indexer.latest()
        self.assertTrue(latest["coverage"]["holder_count"])
        self.assertTrue(latest["coverage"]["treasury_activity"])
        self.assertEqual(latest["assets"][1]["holder_count"], "2")

    def test_history_and_candles_use_exact_reserve_ratio(self):
        self.indexer.record(state(), 1000)
        self.indexer.record(state(101, "5" * 64, "12000000000", "2000000"), 1100)
        history = self.indexer.market_history(TOKEN)
        self.assertEqual([row["price_xgtx"] for row in history], ["0.5", "0.6"])
        candle = self.indexer.candles(TOKEN, 600, 10)[0]
        self.assertEqual((candle["open"], candle["high"], candle["close"]), ("0.5", "0.6", "0.6"))
        self.assertIsNone(candle["volume_xgtx"])

    def test_optional_public_metrics_are_validated_and_preserved(self):
        measured = state()
        measured["assets"][0].update({"holders": "2", "treasury_balance": "50"})
        measured["assets"][1].update({"holders": "3", "treasury_balance": "75"})
        self.indexer.record(measured, 1000)
        latest = self.indexer.latest()
        self.assertEqual(latest["assets"][1]["holder_count"], "3")
        self.assertEqual(latest["assets"][1]["treasury_balance_atomic"], "75")
        self.assertTrue(latest["coverage"]["holder_count"])
        self.assertTrue(latest["coverage"]["treasury_balance"])
        invalid = state(101, "5" * 64)
        invalid["assets"][0]["holders"] = "-1"
        with self.assertRaises(ValueError):
            validate_state(invalid)

    def test_existing_database_schema_migrates_metrics_columns(self):
        with sqlite3.connect(self.indexer.database) as db:
            db.execute("CREATE TABLE assets(snapshot_id INTEGER,asset_id TEXT,issuer TEXT,ticker TEXT,decimals INTEGER,supply_atomic TEXT)")
        with self.indexer.connect() as db:
            columns = {row[1] for row in db.execute("PRAGMA table_info(assets)")}
        self.assertIn("holder_count", columns)
        self.assertIn("treasury_balance_atomic", columns)

    def test_reorg_tip_at_same_height_is_preserved(self):
        self.indexer.record(state(), 1000)
        self.assertTrue(self.indexer.record(state(tip="6" * 64), 1010))
        self.assertEqual(len(self.indexer.market_history(TOKEN)), 2)

    def test_validation_rejects_wrong_chain_and_unknown_pool_asset(self):
        wrong = state(); wrong["chain"] = "f" * 64
        with self.assertRaises(ValueError): validate_state(wrong)
        wrong = state(); wrong["pools"][0]["asset_b"] = "a" * 64
        with self.assertRaises(ValueError): validate_state(wrong)


if __name__ == "__main__":
    unittest.main()

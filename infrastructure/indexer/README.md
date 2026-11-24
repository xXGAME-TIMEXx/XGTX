# XGTX historical market indexer

This service records validated snapshots from the public native XGTX market feed and exposes read-only history and candle APIs. It uses only Python's standard library and SQLite.

It intentionally does **not** invent unavailable data. USD price, transaction volume, holder count and treasury activity remain `null`/unsupported until authoritative daemon event APIs are deployed.

## Run

```bash
python3 xgtx_indexer.py --database /var/lib/xgtx-indexer/index.sqlite3 --listen 127.0.0.1 --port 48400
```

Default source: `https://xgtxchain.com/dex/api/state`. The source must report the expected XGTX mainnet chain ID. Identifiers, quantities, collections and fee fields are validated before a snapshot is committed.

## API

- `GET /health`
- `GET /v1/summary`
- `GET /v1/history?asset_id=<64-hex>&limit=500`
- `GET /v1/candles?asset_id=<64-hex>&interval=300&limit=288`

Candles are XGTX-denominated and include observation count. `volume_xgtx` remains `null` because reserve snapshots alone cannot prove trade volume.

## Test

```bash
python3 -m unittest -v test_indexer.py
```

## Production boundary

Keep the process unprivileged and bind it to loopback behind a TLS reverse proxy. Back up the SQLite database. Monitor `/health`. The public API is read-only and never connects to wallet-RPC, handles recovery words, or signs transactions.

The production installer also enables a daily maintenance timer. It verifies the loopback health response, creates a SQLite online backup with an integrity check, and retains the newest 14 backups under `/var/backups/xgtx-indexer/data`.

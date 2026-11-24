#!/usr/bin/env python3
import argparse
import json
import sqlite3
import time
import urllib.request
from pathlib import Path


def backup(database: Path, backup_dir: Path, retain: int) -> Path:
    backup_dir.mkdir(parents=True, exist_ok=True)
    destination = backup_dir / time.strftime("index-%Y%m%dT%H%M%SZ.sqlite3", time.gmtime())
    with sqlite3.connect(database) as source, sqlite3.connect(destination) as target:
        source.backup(target)
        row = target.execute("PRAGMA integrity_check").fetchone()
        if not row or row[0] != "ok":
            raise RuntimeError("backup integrity check failed")
    backups = sorted(backup_dir.glob("index-*.sqlite3"), reverse=True)
    for expired in backups[max(1, retain):]:
        expired.unlink()
    return destination


def health(url: str) -> dict:
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        payload = json.load(response)
    if payload.get("ok") is not True or not isinstance(payload.get("latest_height"), int):
        raise RuntimeError("indexer health response is invalid")
    return payload


def main() -> None:
    parser = argparse.ArgumentParser(description="Back up and verify the XGTX indexer")
    parser.add_argument("--database", default="/var/lib/xgtx-indexer/index.sqlite3")
    parser.add_argument("--backup-dir", default="/var/backups/xgtx-indexer/data")
    parser.add_argument("--retain", type=int, default=14)
    parser.add_argument("--health-url", default="http://127.0.0.1:48400/health")
    args = parser.parse_args()
    if args.retain < 1 or args.retain > 365:
        parser.error("retain must be between 1 and 365")
    result = {"health": health(args.health_url), "backup": str(backup(Path(args.database), Path(args.backup_dir), args.retain))}
    print(json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    main()

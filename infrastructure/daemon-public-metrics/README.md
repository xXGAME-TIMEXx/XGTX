# Daemon public metrics patch

This Protocol 16 source change extends the read-only `xgtx_get_exchange` asset rows with:

- `holders`: count of owners with a positive exchange-ledger balance for that asset.
- `treasury_balance`: exact exchange-ledger balance held by the configured treasury owner for that asset.

It does not expose native-wallet balances, recovery material, private keys or an address directory. Metrics are derived from the consensus-projected exchange state already held by the daemon.

Apply `public-metrics.patch` to the complete `source.tar.gz` from release `v0.18.1.0-xgtx-p16`, rebuild all affected binaries, run the existing XGTX suite, and run the included focused test. Do not deploy a replacement daemon until its release provenance and connected-node replay are verified.

Focused validation used during preparation:

```text
g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/xgtx_exchange/test_public_metrics.cpp -o /tmp/xgtx-public-metrics-test
/tmp/xgtx-public-metrics-test
```

Expected output: `4 checks passed: public holder and treasury metrics`.

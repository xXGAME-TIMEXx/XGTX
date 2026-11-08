Build this reviewed source on Ubuntu 24.04 using [BUILD-UBUNTU24.md](BUILD-UBUNTU24.md).
The Phase28 instructions below are historical and reference inputs absent from
this source archive; use the new guide for clean builds.

XGTX Phase28 — software-wallet native withdrawals (native verification pending)

Requires verified Phase27 in ~/xgtx-exchange-dev. Extract this ZIP there and run
bash run_wallet_withdraw.sh. Build output is shown and saved to
~/xgtx-wallet-withdraw-build.log. The script runs only fresh temporary wallets
and an isolated offline regression chain, then the combined regression runner.

Wallet RPC xgtx_operation accepts kind9 with recipient as a standard native
wallet address, quantity as the total native ledger debit INCLUDING mining fee,
and max_fee as the maximum permitted mining fee. Decimal amounts use base units
and must be strings. Native XGTX has ten decimal places. Preview remains the
default; do_not_relay=false explicitly submits. The response adds payout:
quantity = payout + fee. No treasury fee is added to withdrawals. Swap fees
remain 0.25% to LP reserves and 0.05% to the designated treasury.

The wallet reads confirmed owner balance/nonce, checks chain against its own
genesis, requests fixed sixteen-member reserve rings bound to one chain tip,
authenticates pruned origin hashes, carrier signatures, stored public keys,
commitments and public openings, and signs the withdrawal intent internally.
It builds native CLSAG/BulletproofPlus proofs and ordinary tagged recipient
outputs with an automatically estimated fee. Partial withdrawals recreate
protected reserve change; full physical releases create two ordinary payouts.
The recipient's standard wallet scans and spends the payout normally.

Relay uses native daemon validation with sanity checks. It does not mark any
ordinary wallet inputs spent or create an incorrect zero-input outgoing wallet
entry. Only the outgoing transaction proof key is retained when wallet settings
enable storage. Ordinary wallet transaction construction/commit is unchanged.
Confirmed withdrawals currently appear through exchange state, not as a new
dedicated wallet-history entry; a complete interface/history remains pending.

Read-only daemon xgtx_get_reserve_ring requires start (decimal string) and tip
(hex). It returns exactly sixteen authenticated reserve origins, metadata and
spent status on a configured backing node. Restricted RPC cannot use it.
Each pruned origin is capped at 128 KiB. Wallet lookup is capped at 256 pages
and sixteen selected inputs, retains origins only for selected rings, requires
zero unlock time and ten blocks for every member. It may request a smaller
amount or retry when insufficient mature liquidity is found within these caps.
This bounded initial selection needs production scaling/resource review.

Integration test covers immature rejection, stable ring API and malformed/stale
requests, full withdrawal of the initial49 XGTX bootstrap through wallet preview
and relay, exact54 XGTX redeposit, all token/LP/swap operations, partial wallet
withdrawal with protected change, fee ceiling and unauthorized-owner rejection,
restart/pop/replay and ordinary recipient spending of both native payouts.

Local installer guards and script syntax have been checked. This environment
cannot compile the native code; the supplied WSL run is REQUIRED to establish
native build/test success. This package does not activate any public network.
Production bootstrap, connected-node validation, full UI/history, resource
hardening, independent review, release packaging and activation remain pending.

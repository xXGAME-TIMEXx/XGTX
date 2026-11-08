# XGTX experimental operation format, revision 1

Status: isolated codec proposal, no consensus activation and no signatures.
This supersedes Phase 5 JSON only as an experimental binary representation.
It does not decide public accounts versus confidential asset UTXOs.

All integers are unsigned, amounts are 64-bit little-endian base units.
No varints, optional fields, padding, or trailing bytes are allowed.
Header: six ASCII bytes `XGTXOP`, zero byte, version byte 1, kind byte.
Then chain identifier (32 bytes), nonce (32 bytes).

| Kind | Remaining fields in order | Length |
|---|---|---|
| 1 Create | supply u64, decimals u8, ticker length u8, ticker ASCII | 85–95 bytes |
| 2 Transfer | asset ID 32 bytes, amount u64, recipient identifier 32 bytes | 145 bytes |
| 3 Burn | asset ID 32 bytes, amount u64 | 113 bytes |

Supply and amount must be 1 through UINT64_MAX. Decimals range 0–12.
Ticker is 2–12 characters, first A–Z, remaining A–Z or 0–9.
Unused object fields must be zero/empty to prevent silent information loss.
Identifiers are opaque bytes here. Zero values are allowed at codec level;
network matching, cryptographic validation and replay checks belong to future
semantic validation. A nonce and chain field alone provide no authorization.

Proposed issuance policy for future review: fixed supply, no subsequent mint.
Recipient encoding is deliberately unresolved: it must be defined alongside
the spending model; these 32 bytes are not a validated CryptoNote address.
Asset ID generation, signature preimage, input binding and nonce uniqueness
are also pending. No wallet address or activation height has been configured.

## Integration boundaries inspected

`src/cryptonote_basic/cryptonote_basic.h`: transaction prefix has version,
unlock time, vin, vout and extra; no asset-aware outputs added here.
`src/cryptonote_core/blockchain.cpp`: check_tx_inputs near 3006/3308,
check_tx_outputs near 3042, m_db->add_block near 4303, pop block near 586.
`src/blockchain_db/blockchain_db.h`: persistent DB boundary.
Adding a payload to extra without asset-aware conservation and authorization
would not create spendable tokens. These functions were inspected, not changed.

## Fee policy retained

Swap input fees: floor(input × 25 / 10000) to LP reserves;
floor(input × 5 / 10000) to protocol recipient, in input asset.
Rounding permits zero fees on dust. Final consensus must explicitly adopt or
replace this dust policy. No actual fees are collected by these tests.

## Verification

New codec: 398 checks passed using g++ C++17 with warnings as errors and
AddressSanitizer/UndefinedBehaviorSanitizer. Leak detection disabled because
the execution environment cannot expose the required process information.
Phase 5: 10 Python tests passed. Phase 2: 11 Python tests passed.
Phase 4: existing C++ swap suite passed.
CMake configuration was supplied but not executed because CMake is absent.
No full daemon build, chain synchronization or on-chain exchange test run.

## Remaining sequence

1. Resolve asset spending model and signature/transaction binding.
2. Implement asset conservation and issuance authorization with adversarial tests.
3. Add atomic persistent token/pool state plus block undo and reorg tests.
4. Integrate pools, LP shares, deposits/withdrawals and deterministic swaps.
5. Add wallet signing, RPC and configured fee recipient.
6. Run multi-node isolated testnet and independent security review.
7. Coordinate a separately authorized network upgrade.

Source and prototypes retrieved 2026-10-14. Only one saved consensus checkpoint
was located. This package completes the codec milestone, not the exchange.

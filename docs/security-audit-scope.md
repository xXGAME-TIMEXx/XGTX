# XGTX independent security audit package

Prepared November 23, 2026 Pacific time. This document scopes an external review; it is not an audit opinion or certification.

## Review objective

Determine whether the native XGTX Protocol 16 mainnet, wallet signing paths, exchange projection, liquidity accounting, public RPC surfaces and deployment controls preserve funds, consensus state and user privacy under adversarial inputs, restarts and reorganizations.

## Canonical release inputs

Release: `v0.18.1.0-xgtx-p16` — Ubuntu 24.04 x86_64 prerelease.

| Artifact | Published SHA-256 | Purpose |
| --- | --- | --- |
| `source.tar.gz` | `fa9d5f5f140afbdf09d0418f4aac8cec550243c81456f74f781c7bdf2c70d89b` | Complete native source and release recipe |
| `runtime.tar.gz` | `98884185e10bb09f8cdcd3bd55f5839129ab320168be33409164f7aea0624040` | Published daemon, wallet CLI, wallet RPC and inspection runtime |
| `SHA256SUMS` | Asset digest `6b43276dfd244679894e425bc8fb579d1df02e3dad86f860f6fd1bd249eb197a` | Release checksum file |

Release URL: `https://github.com/xXGAME-TIMEXx/XGTX/releases/tag/v0.18.1.0-xgtx-p16`

Do not audit GitHub's automatically generated source archive as a substitute for the attached `source.tar.gz`.

## Network identity and economics

- Genesis/network identity: `6cc86a50eca6febe795dc25490d02f29fe8a5590e93daf03d1e0c368191d7435`.
- Public P2P: `5.78.219.248:48080`; public node RPC and wallet RPC must remain loopback-only.
- Precision: 10 decimals.
- Maximum supply: 100,000,000 XGTX.
- Genesis allocation: 5,000,000 XGTX.
- Initial reward: 50 XGTX through height 400,000; then 25 XGTX with halvings every 1,500,000 blocks.
- Native DEX fees: 0.25% retained in LP reserves plus 0.05% to treasury, rounded separately; mining fees are additional.
- Protocol 16 activation: native block index 700 / exchange block count 701.

## Required audit scope

### Consensus and chain state

- Activation boundary, replay determinism, database migration and rollback.
- Reorganization handling, including two-block undo and connected-node replay.
- Emission schedule, genesis allocation, amount overflow and decimal conversion.
- RandomX/Monero-derived consensus modifications and network-identity separation.

### Exchange and liquidity

- Asset issuance, burns, transfers and authorization.
- Pool creation, deposits, withdrawals, share accounting and invariant preservation.
- Swap rounding, price impact, minimum output, fee separation and treasury recipient.
- Native reserve locking, withdrawal proofs, replay protection and failure atomicity.
- Projection/database consistency across restart and reorganization.

### Wallet and transaction construction

- Wallet creation, restore, address validation, signing and transaction review.
- Wallet-RPC authentication and safe loopback defaults.
- Native deposit/withdrawal construction and confirmation handling.
- Seed, private-key and password handling; no browser or public service should receive them.

### Public services

- Explorer input validation and privacy boundaries.
- `/dex/api/state`, quote endpoints and historical indexer validation.
- Nginx isolation, CORS, rate limits and denial-of-service controls.
- Indexer schema migration, reorganization retention, backups and restore procedure.
- Confirm missing volume, USD, holder and treasury metrics remain unavailable rather than inferred.

### Operations and release engineering

- Reproducibility of the attached release recipe on Ubuntu 24.04 x86_64.
- Binary/source correspondence and dependency inventory.
- Least-privilege services, rollback, backup integrity and controlled daemon replacement.
- Production SOL checkout and payout services must remain paused and outside the native-chain audit unless separately scoped.

## Existing evidence to reproduce

- Monero-derived suite: 1,312 tests recorded; 1,308 passed and 2 skipped after DNS correction.
- Exchange engine: 2,441 recorded checks passed.
- Native carrier suite: 392 recorded checks passed.
- Lifecycle coverage includes issuance, pool operations, swaps, burns, transfers, restart, two-block undo and deterministic replay.
- Historical indexer: six unit tests pass for exact reserve ratios, duplicate snapshots, reorg tips, validation, optional public metrics and old-schema migration.
- Focused holder/treasury metrics test: four checks pass.
- Live indexer and Console were production-verified at indexed height 19,545 after Console version 15 publication.

The auditor must reproduce relevant tests from canonical source. Recorded totals are evidence, not a substitute for independent execution.

## High-priority attack cases

1. Overflow, underflow or precision loss at maximum amounts and fee-rounding boundaries.
2. Duplicate, reordered or replayed exchange operations.
3. Reorganization during deposit, swap, withdrawal and payout acknowledgement.
4. Malformed asset IDs, pool IDs, ownership proofs and treasury configuration.
5. Mempool/confirmed-state disagreement and restart during partial processing.
6. Unauthorized or externally reachable daemon/wallet RPC.
7. Quote manipulation, stale snapshots, thin-liquidity slippage and missing minimum-output enforcement.
8. Indexer poisoning, chain-identity mismatch, same-height alternate tips and database corruption.
9. Browser wallet/provider spoofing and transaction-summary mismatch.
10. Release artifact substitution, dependency compromise and rollback to an incompatible binary.

## Required deliverables from the independent reviewer

- Exact source and artifact hashes reviewed.
- Build environment and reproduction commands.
- Findings with severity, exploit preconditions, affected components and proof of concept where safe.
- Confirmation of fixed findings against exact replacement commits/artifacts.
- Explicit exclusions and residual risks.
- Final report suitable for public disclosure, with any temporary embargo limited to active vulnerabilities.

## Deployment gate

Do not label XGTX independently audited until an unaffiliated reviewer completes the scope and signs a report identifying exact artifacts. Do not deploy the public-metrics daemon patch until it receives a provenance-matched full build, the required regression suites, connected replay, backup verification and a rehearsed rollback. Do not open real-money checkout or payout broadcasting as part of a daemon deployment.


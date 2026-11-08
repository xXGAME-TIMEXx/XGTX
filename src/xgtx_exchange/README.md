# XGTX exchange development module

This is tested standalone C++ source for an exchange state machine. It is **not
registered with the daemon, wallet, RPC, transaction parser or block validator**.
It must not be copied over a running node as a network upgrade.

## Implemented and tested

- Fixed-supply token issuance, owner-signed transfers and owner-signed burns.
- Asset IDs derived from the canonical creation operation, rather than ticker.
- Public token balances and pool reserves. Native CryptoNote privacy has not
  been modified. This public exchange accounting is a proposed design choice.
- Canonical, versioned binary operations and signed envelopes.
- Chain-specific signatures, strictly sequential per-sender nonces, expiry,
  activation-height gating and a 1,024-operation block cap.
- Canonical token/token pools in either asset order; geometric initial LP
  shares; proportional deposits; proportional withdrawals; full pool closure.
- Constant-product swaps, owner-selected minimum output, LP deposit and
  withdrawal limits; checked uint64 amounts and uint128 intermediate products.
- LP fees of 25/10,000 of input, retained in reserves; protocol fees of
  5/10,000 of input, credited to the configured public token account. Each
  portion is independently floored to whole base units. Dust may pay zero fees.
- Supply and LP conservation after every operation; candidate-state updates
  prevent failed operations from partially changing the caller's state.
- LMDB state and block undo in the caller's transaction, allowing native block
  and exchange updates to share one atomic database commit in future integration.
- Immutable stored chain/treasury/activation policy; restart recovery,
  caller-transaction abort, rollback, and replacement-branch replay tests.

## Verification status

The standalone suite passed **2,441 explicit checks** using real OpenSSL
Ed25519 signatures and SHA-256 IDs, plus the repository's bundled LMDB C code.
It tests two independently executed copies of signed block data, not networked
daemons. The source CryptoNote signing/hash adapter exists but is **uncompiled
and unverified** because full build dependencies are unavailable here.

Run from a complete copy of the original source:

```bash
bash tests/xgtx_exchange/run-tests.sh
XGTX_SANITIZE=1 bash tests/xgtx_exchange/run-tests.sh
```

This requires gcc, g++, OpenSSL development headers/libcrypto and pthreads.
The standalone CMake file is supplied as another build route, but was not run
here. LeakSanitizer could not run under the execution environment; address and
undefined-behavior instrumentation ran with leak detection disabled.

The test wallets are deterministic fixtures. They are test-only and must never
hold real coins. They are not the user's designated wallet.

## Explicitly unfinished

**Native XGTX is rejected by every asset/pool operation.** There is no public
credit/faucet API and no method to invent an XGTX balance from an RPC response.
Token/token trading in tests does not prove token/XGTX trading is available.

Remaining work:

1. Specify and implement authenticated native locking/redemption, including
   carrier-transaction binding and native value conservation.
2. Add an activated native transaction payload, semantic validation, mempool
   conflict handling, block-ordered execution and alt-chain validation.
3. Wire `lmdb_store` into the actual native block write/pop transactions;
   increase/check named-table capacity and define a state migration.
4. Build the CryptoNote signing adapter and use the wallet's key/device paths.
   OpenSSL test signatures are not interchangeable with CryptoNote signatures.
5. Add wallet construction/signing, balance/quote/create/liquidity/swap RPCs,
   SDK/UI workflows, network activation policy and the user's verified fee
   recipient. The test treasury is only a fixture.
6. Build the full daemon and wallet, run multi-node isolated testnet sync,
   adversarial/reorg/upgrade tests, and obtain independent security review.

## Storage limits

The first store uses full state snapshots for undo, capped at 64 MiB and
1,000,000 entries per table. It is a correctness-first development store, not
a long-term scaling design. Sparse per-key undo, bounded work accounting,
pruning and migration must be designed before public launch.

If a database operation fails after writes begin, the caller must abort the
owning LMDB transaction. This module never commits a transaction itself.
Only the daemon's already validated canonical native block path may call
`append`; this is not a public RPC authority to submit arbitrary block IDs.

# Native XGTX integration requirements

The uploaded source was inspected at transaction-prefix serialization,
`core::check_tx_semantic`, both `Blockchain::check_tx_inputs` overloads,
`Blockchain::check_tx_outputs`, miner reward validation, block insertion and
block pop. No native consensus function was changed by this module.

## Current constraints from source

- Current transaction version is 2. Prefix contains version, unlock time,
  CryptoNote inputs/outputs and extra, without an asset-aware output type.
- Ordinary input deserialization and semantic validation require
  `txin_to_key`. Adding an exchange input without coordinated validation
  changes would be rejected.
- RingCT transaction outputs must have public amount zero. An arbitrary
  publicly claimed deposit amount is not proof of deposited XGTX.
- Native RingCT balance verification and signatures must still cover any
  value moved into or out of the exchange.
- Coinbase validation requires mining reward plus native fees to equal
  the miner outputs at this chain's protocol version. Appending redemption
  outputs without changing conservation and validation would cause inflation
  or rejection.
- Persistent insertion at `blockchain.cpp` near 4303 and pop near 586 must
  share atomic updates with exchange state; a separately committed sidecar
  cannot provide that invariant.

## Required bridge contract

For every native deposit D and redemption R:

`native inputs + R = native outputs + native mining fees + D`

`exchange native obligations after = obligations before + D - R`

Every debit/redemption must have a verified account signature, a fresh nonce,
deadline, chain ID, and an independently reconstructed commitment to its native
carrier outputs and amounts. A signature on an operation that does not bind
its withdrawal destination must not authorize a native redemption.

The RingCT sum verification, wallet generator and signature preimages would
need coordinated changes for a virtual-commitment bridge. This is a design
candidate, not implemented or security-reviewed code. An alternative is a
separate publicly committed escrow output type with explicit spend rules;
that also requires coordinated transaction, wallet and database changes.
Neither may be replaced with an administrator-controlled balance credit.

The native proof adapter must reject unconfirmed/RPC-only receipts, duplicate
deposits, wrong chains, malformed/out-of-range commitments, wrong destinations,
insufficient backing and already redeemed requests. Block rollback must undo
both native and exchange effects together.

## Build blockage observed

Standalone g++/OpenSSL/LMDB tests work. CMake, Boost headers, pkg-config,
libsodium/unbound/ZeroMQ development dependencies and Rust tools were absent.
An ordinary dependency installation found no package candidates; updating
package lists failed with environment setgroups/seteuid permission errors.
No elevated access or mainnet deployment was attempted.

Consequently the full node, CryptoNote adapter, wallet, RPC and native bridge
have not been built or verified. A working full-source build environment is
required before those changes can be validated. The running node's data and
configuration are outside this development copy and were not accessed.

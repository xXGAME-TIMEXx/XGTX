# xXGAME-TIMEXx (XGTX)

XGTX is a native RandomX proof-of-work blockchain derived from Monero. This repository is the public release and setup entry point for the native chain; it is separate from earlier Solana tokens using the same name.

## Public beta — open to miners and early users

XGTX is open for public participation as of October 16, 2026. Download the native software to run a node, mine XGTX, or try the experimental local wallet interface. Development continues during this public beta; report problems through GitHub Issues.

**Start with Ubuntu 24.04 x86_64.** Each participant runs their own wallet locally and controls their recovery words. Mining does not require a purchase. There is no established dollar price, external-chain bridge, or guaranteed income.

## Start here

**Public block explorer:** http://5.78.219.248/ — browse recent blocks and look up native block heights, block hashes and transaction hashes. This read-only beta reflects one node’s chain view. It currently uses HTTP; do not enter wallet credentials or recovery words. Hidden transfer amounts and wallet balances are not exposed by the explorer.

1. **Mine XGTX:** [download the Ubuntu 24.04 x86_64 native release](https://github.com/xXGAME-TIMEXx/XGTX/releases/tag/v0.18.1.0-xgtx-p16), then follow the mining setup below.
2. **Use the experimental local wallet interface:** [download the guided setup edition](https://github.com/xXGAME-TIMEXx/XGTX/releases/tag/wallet-ui-20261016-setup) and follow its included README.
3. **Report installation problems:** [open an issue](https://github.com/xXGAME-TIMEXx/XGTX/issues). Include your OS version, release tag and error message. Never include wallet files, recovery words, passwords or session links.

The native binary release requires Ubuntu 24.04 runtime libraries. It is not a universal Linux binary or a Windows installer; Ubuntu 26.04 is not a supported runtime for this package.

## Release status

**The initial Ubuntu 24.04 x86_64 public mining pre-release is published.** Download `runtime.tar.gz`, `source.tar.gz` and `SHA256SUMS` together from [v0.18.1.0-xgtx-p16](https://github.com/xXGAME-TIMEXx/XGTX/releases/tag/v0.18.1.0-xgtx-p16). Do not use GitHub's automatically generated source ZIP as a substitute for the complete release source archive.

Verified on the operator's mainnet on October 16, 2026 UTC:

- The upgraded Ubuntu 24.04 server and a separate laptop node synchronize over public P2P and relay mined blocks.
- Protocol 16 activated at native block index 700 (block count 701).
- A fresh Ubuntu 24.04 container using the actual GitHub downloads synchronized from genesis to block count 1403 and reconstructed the live reserve state: 98.9877872 XGTX and 17 reserve outputs, with matching native and exchange tips. This was an operator-run clean installation check, not an independent user's test.
- A real wallet-funded reserve deposit was confirmed. A subsequent 1 XGTX ledger withdrawal was confirmed in block 1346, returning exactly 0.9968414 XGTX to the wallet after its 0.0031586 XGTX native mining fee.
- Native/token AMM, liquidity and swap operations have passed offline integration tests. During the operator's October 16 live mainnet session, GTTK was created with 6 decimals and an initial supply of 1,000,000 tokens. An operator-funded pool was created with 10 XGTX and 100,000 GTTK, followed by a buy and a sell observed in the local wallet interface. These were operator-run functional tests, not independent user validation or evidence of outside trading demand.
- The pool creation transaction `32b5d25de9dee8d0ecb9242ec7ad6b306eec19c14d865c694721d13bed986f77` was confirmed at native block index 1431. GTTK asset ID: `25595ec5c662ee6065ea5eb7233fc16fd5d4d12727eca3c580892e295f6ecd2a`. Pool reserves change with trades; the initial amounts are not a current quote or dollar valuation.

These are functional checks, not an independent security audit. This is an early network with limited peers and mining participation. There is no established dollar price or guaranteed mining or fee income.

## Network identity

| Setting | Value |
| --- | --- |
| Native ticker | XGTX |
| Decimal places | 10 |
| Proof of work | RandomX |
| Target block interval | 120 seconds; actual intervals vary |
| Initial public peer | `5.78.219.248:48080` |
| Local daemon RPC | `127.0.0.1:48081` |
| Genesis hash | `6cc86a50eca6febe795dc25490d02f29fe8a5590e93daf03d1e0c368191d7435` |
| Exchange activation | Native block index 700, exchange block count 701 |

The chain includes a founder allocation of 5,000,000 XGTX at genesis. Mining does not require buying XGTX or adding liquidity.

## Ubuntu 24.04 x86_64 mining setup

The first binary package targets **Ubuntu 24.04 x86_64** with listed system runtime dependencies. A native Windows installer is not part of this release. The operator also runs an integration build in WSL; that does not establish support for arbitrary Windows or Linux versions.

Download `runtime.tar.gz`, `source.tar.gz` and `SHA256SUMS` from the same release into a new directory. Verify both archives before extracting or running them:

```bash
sha256sum -c SHA256SUMS
tar -xzf runtime.tar.gz
cd xgtx-ubuntu24-x86_64
sha256sum -c SHA256SUMS
bash install-runtime-deps.sh
```

The dependency installer uses sudo for Ubuntu packages. Run the daemon and wallet as your ordinary user, not root.

Start a new node using its own data directory, leaving RPC accessible only on your computer:

```bash
./bin/xgtxd --data-dir "$HOME/.xgtx-public" --add-priority-node 5.78.219.248:48080 --rpc-bind-ip 127.0.0.1 --no-zmq --no-igd --disable-dns-checkpoints --check-updates disabled
```

Wait for synchronization. In a second terminal, return to the extracted runtime directory and create a **new** wallet filename:

```bash
./bin/xgtx-wallet-cli --generate-new-wallet "$HOME/xgtx-mining-wallet" --daemon-address 127.0.0.1:48081 --trusted-daemon
```

Choose a wallet password locally and back up the recovery words privately. Never upload wallet files, seeds, private keys or RPC credentials to this repository. Existing wallet owners should use their established recovery/opening procedure instead of creating over an existing filename.

At the wallet prompt, start mining to your wallet with two CPU threads:

```text
start_mining 2
```

To stop mining:

```text
stop_mining
```

Keep the node running and the computer awake while mining. Rewards and other received outputs must mature before they are spendable. Hash rate, network difficulty and competition determine block discovery; rewards are not scheduled payments to each miner.

## Experimental local wallet interface

Download `xgtx-local-wallet-ui-20261016-setup.tar.gz` and its `SHA256SUMS` from the [guided setup release](https://github.com/xXGAME-TIMEXx/XGTX/releases/tag/wallet-ui-20261016-setup). Keep these in a separate folder from the native release checksums.

Verify the archive, extract it and follow its README:

```bash
sha256sum -c SHA256SUMS
tar -xzf xgtx-local-wallet-ui-20261016-setup.tar.gz
cd xgtx-local-wallet-ui
sha256sum -c SHA256SUMS
```

The interface requires Python 3, Requests and the matching native binaries. Its setup helper generates private RPC credentials, matching configuration files and separate launch scripts for a local node, wallet RPC and browser interface. It uses its own node database and ports; it does not automatically start mining or submit transactions.

Create or open your own wallet and keep its recovery words private. Close any other application using that wallet before starting wallet RPC. Run the setup as your ordinary user. Keep the application in a permanent location because generated launch scripts reference it.

The interface is experimental. Native deposits require an existing reserve bootstrap. Operator-run live pool creation, buying and selling have been exercised as described above; independent user validation remains pending. Older README files inside the existing downloads predate these tests. The published package contains no user's wallet or RPC credentials.

## Native tokens, pairing and disclosed fees

The protocol supports native-chain tokens and XGTX/token pairs. These are not bridges to BTC, ETH, SOL or other external-chain assets. A usable pool requires someone to supply both assets; the project is not promising dollar liquidity.

For supported AMM swaps, the input-side fee is **0.30% total**:

- **0.25%** stays in pool reserves for liquidity providers.
- **0.05%** is credited to the designated project treasury in the input asset.

The two fee components are rounded down separately in base units. Native transaction mining fees are separate. The swap protocol fee is not a fee on every ordinary coin transfer and does not guarantee revenue.

Treasury public address:

```text
CmS3B2SaPyCfwGAB7bsaqm6jT4qd5MHkPByjW5Db5jECQ7YkKw9A69rjAe8LZaXfHjQuyrybaKTa5S9ctmXnFQ3H7Y3ETjF
```

The local wallet-control interface is separate from this public page. Keep daemon/wallet RPC and wallet controls bound to localhost. Never publish the interface session token or expose the founder wallet to visitors.

## Source and licensing

The corresponding complete release source is supplied as `source.tar.gz`, including the existing source LICENSE and third-party components and notices. Preserve those licenses and notices when redistributing. No replacement license is asserted by this README.

# Future-build source input — 2026-11-07

This snapshot is input for a future XGTX Ubuntu 24.04 x86_64 release build.
It has not been compiled. It is NOT source proven to match the existing tested
four-binary candidate. No tag, publisher signature or Git commit is asserted.

## Provenance

The original reviewed source archive and its SHA-256 are unavailable. The
`upstream_archive.sha256` field in SOURCE-PROVENANCE.json is therefore null.
The supplied baseline file manifest is retained as `source-manifest.json`,
SHA-256 `3b6f05f652e17c9a34c654a41155af2fafb59edbd8ecc6742e95ab72077597c4`.
This digest is a manifest hash, not the missing original archive hash.
SOURCE-PROVENANCE.json records selected baseline matches and verified local
changes with hashes. SOURCE-MANIFEST.json identifies this snapshot's files.

## Included inputs and exclusions

Includes native source, bundled C/C++ dependencies and licenses, the current
four-binary build recipe, source test programs, legacy tooling and current
documentation. Legacy scripts retain their historical input/path requirements;
use the command below for this snapshot.

Locked Rust dependencies are vendored as source under
`src/fcmp_pp/fcmp_pp_rust/vendor`. Root `.cargo/config.toml` redirects Cargo to
these files; this directory contains configuration, not a cache. Cargo vendor
used a copied local cache, without network access. Upstream test/example key
fixtures and non-build tests/examples/benches were pruned from vendor packages;
retained Cargo file checksums remain verified. See vendor-pruned-paths.json.

Excludes build outputs, Cargo caches, generated depends outputs/downloads,
Git metadata, user wallets, credentials and live chain data. `tests/data` is
omitted entirely, including bundled wallet and chain fixtures, so full native
fixture tests cannot run from this snapshot alone. Wallet source code is
included to build wallet executables; no wallet data is included.

The checkpoint constants `src/blocks/checkpoints.dat` and empty
`testnet_blocks.dat`/`stagenet_blocks.dat` are retained because CMake embeds them
in the executable. They are source build inputs, not an operational chain DB.

## Future build recipe (not executed during snapshot preparation)

On Ubuntu 24.04 x86_64, install the system build packages listed in
BUILD-UBUNTU24.md. Rust toolchain and Ubuntu packages are not bundled.
Use Rust 1.99.1 for the next build, superseding the historical guide's inherited
1.99.0 pin. With rustup available:

```bash
rustup toolchain install 1.99.1 --profile minimal
rustup run 1.99.1 rustc --version
rustup run 1.99.1 cargo --version
rustup run 1.99.1 python3 -B build_ubuntu24.py \
  --output "build/future-$(date -u +%Y%m%dT%H%M%S)-$$" \
  --offline --jobs 2 --package
```

Run from the extracted snapshot root. No previous build or cache seed is
required. Public exchange activation stays OFF. The build script records tool
versions and compiles/packages xgtxd, xgtx-wallet-cli, xgtx-wallet-rpc and
xgtx-projection-migrate without running them. Start with `SNAPSHOT-BUILD.md`
instead of the historical Phase28 README.

## Verification and limits

Before extraction, verify the adjacent archive SHA256SUMS. After extraction,
run `sha256sum -c SOURCE-FILES.sha256` from the snapshot root. The file checksum
list covers payload files except itself and SOURCE-MANIFEST.json; the archive
checksum covers both metadata files as well.

Preparation validated Cargo locked offline dependency resolution with an empty
Cargo home and checked vendor file hashes. These checks are not compilation,
functional tests or an independent security review. No binaries were rebuilt,
no node or wallet was opened, and no migration was run.

Fresh Ubuntu dependency installation and byte-for-byte build reproducibility
remain unverified. Normalized source archive metadata does not establish
reproducible binary builds. The existing tested candidate remains separate and
unchanged; earlier browser/runtime results do not certify this new snapshot.

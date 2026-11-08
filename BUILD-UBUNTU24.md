# Ubuntu 24.04 x86_64 source build

This source archive supersedes the historical Phase28 README for build setup.
The Makefile and CMakeLists.txt remain the native build system. The new entry
point builds four dynamically linked executables without starting them:
`daemon` → `xgtxd`, `simplewallet` → `xgtx-wallet-cli`,
`wallet_rpc_server` → `xgtx-wallet-rpc`, and `xgtx_projection_migrate` →
`xgtx-projection-migrate`. These names are defined in the daemon, simplewallet,
wallet and blockchain_utilities CMakeLists.txt files. It does not package the
trader interface or approve public use. No user wallet files are read or created.

Install prerequisites on a disposable Ubuntu 24.04 x86_64 build machine:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends build-essential cmake pkg-config \
  git ca-certificates curl libboost-all-dev libssl-dev libunbound-dev \
  libsodium-dev libzmq3-dev libhidapi-dev libusb-1.0-0-dev libprotobuf-dev \
  protobuf-compiler libreadline-dev libunwind-dev liblzma-dev libexpat1-dev
```

Rust and Cargo are also required. The historical builder specifies Rust 1.99.0;
this is an inherited requirement, not a toolchain validated by this repair.
Using an existing rustup installation on the disposable build machine:

```sh
rustup toolchain install 1.99.0 --profile minimal
rustup run 1.99.0 python3 build_ubuntu24.py --output build/ubuntu24-clean --jobs 2 --package
```

If rustup is absent, install it using the official Rust installation procedure
before these commands. Toolchain downloads and Cargo's locked Git/crate
dependencies require network access; the Rust sources are not vendored. Do not
substitute Ubuntu's Rust package without checking dependency compatibility.
Cargo already uses `--locked` through the native CMake build.
The entry point also runs `cargo fetch --locked` before configuring, and sets
`CARGO_HOME` to the fresh output directory's `cargo-home` so dependency writes
stay inside this local source copy.

With the intended Rust/Cargo already selected on PATH, the exact entry point is:

```sh
python3 -B build_ubuntu24.py --output build/ubuntu24-clean --jobs 2 --package
```

Use a different, previously nonexistent output directory for each attempt. The
script requires this to avoid reusing CMake/native build outputs. It uses
`ARCH=default` instead of the upstream `native` default, and
`MANUAL_SUBMODULES=ON` because dependencies are included in this source archive
without Git submodule metadata. This does not verify those dependencies' origin.
The historical public activation candidate is explicitly OFF; enabling it is a
separate consensus decision, not a build fix.

To preserve a successful output and reuse its downloaded dependencies offline:

```sh
/home/xgtxuser/.cargo/bin/rustup run stable python3 -B build_ubuntu24.py \
  --output "build/ubuntu24-four-$(date -u +%Y%m%dT%H%M%S)-$$" \
  --seed-output build/ubuntu24-20261106T023712-12702 --offline --jobs 2 --package
```

Verify `rustup run stable rustc --version` first: stable is a movable alias,
observed here as 1.99.1. `--seed-output` copies only the Cargo dependency cache;
it never changes the seed. Native objects are rebuilt because CMake's existing
build tree uses absolute paths and cannot safely be relocated. This is a cache
assisted build, not an independent clean build for reproducibility purposes.
If the offline cache is incomplete, retry in a new output from a regular Ubuntu
terminal without `--offline` to allow Cargo downloads.

Logs, tool versions and `build-report.json` are saved in the output directory.
On success, `--package` creates a local four-binary candidate archive, dependency
listing, package inventory and SHA256SUMS. It includes the project license;
third-party redistribution notices still require review before public release.
The archive is reopened to verify all four executable members and their hashes
against the build outputs. No daemon or wallet is executed, and no browser tests
are run. Historical attempts below used the earlier daemon-only builder.

## Legacy release workflow limitations

`run_ubuntu24_release.py` is preserved as a historical workflow. It requires a
specific home-directory snapshot, prior approval reports, `phase35-updates/`,
`phase35-source-hashes.json`, and `release/inside-build.sh`, all absent here.
It also tries to patch a unit-test timeout already present in this copy.
It must not be used as the clean build entry point. The regression runner also
hardcodes `~/xgtx-exchange-dev` and invokes temporary wallet fixtures; it is not
invoked by this recipe. The missing `run_wallet_withdraw.sh` from README.md is
not a clean build prerequisite.

The original `source-manifest.json` is retained as the reviewed-input baseline.
It does not cover these new build instructions or subsequent local changes.

## Validation limits

Latest attempt on 2026-11-05 used the installed `stable` toolchain, verified as
rustc 1.99.1 (58cf8f9c3 2026-10-13) and Cargo 1.99.1
(5f94df478 2026-08-27). No rustup alias named `1.99.1` was installed.
Other tools: CMake 3.28.3, GCC/G++ 13.3.0
(Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU Make 4.3, pkg-config 1.8.1.

```sh
/home/xgtxuser/.cargo/bin/rustup run stable python3 -B build_ubuntu24.py --output build/ubuntu24-rust1991-attempt1 --jobs 2 --package
/home/xgtxuser/.cargo/bin/rustup run stable python3 -B build_ubuntu24.py --output build/ubuntu24-rust1991-attempt2 --jobs 2 --package
```

Attempt1 configured successfully and compiled to roughly 62%, then Cargo failed
writing to the read-only user Cargo cache. The entry point was fixed to use a
local cache and fetch dependencies early. Attempt2 passed version checks and
cache creation, but `cargo fetch` exited 101 because github.com could not be
resolved. The requested crypto-bigint commit is
`78352771313f1e9b8e48abe5ce30d50d6bdd291d`; the nested "revision not found"
message does not establish a missing upstream commit because DNS failed.
Working DNS/network access or a verified offline dependency vendor bundle is
needed to proceed. No final executable or package was produced. Reports and
logs are under the two output directories above. Compilation/linking with Rust
1.99.1 remains unverified.

Earlier prerequisite-only attempt (before tool installation):

Local attempt on 2026-11-05, Ubuntu 24.04.5 x86_64:

```sh
python3 -B build_ubuntu24.py --output build/ubuntu24-clean --jobs 2 --package
```

Exited 1 at prerequisite checking: `cmake, gcc, g++, make, rustc, cargo,
pkg-config` are absent. Required development packages are also absent. System
package installation is outside this workspace's permitted writes. Python
syntax parsing and CLI help succeeded. CMake configuration, compilation,
linking and packaging have not run; further source/build failures may emerge
after installing prerequisites. The attempt report is at
`build/ubuntu24-clean/build-report.json`.

A successful compile is not a public-release approval. Full native regression,
inherited RPC integration, baseline CPU execution, mainnet replay, resource and
security review, source distribution packaging and activation planning remain
separate work. Existing offline browser tests are already complete.

Reproducibility is not established. Two independent clean environments must
build the same inputs and compare resulting artifacts before making that claim.
The current recipe records versions but does not pin Ubuntu package versions,
vendor Cargo inputs or normalize archive timestamps. Its tarballs should not be
expected to match byte-for-byte across builds yet.

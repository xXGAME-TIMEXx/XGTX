# XGTX candidate installation: Ubuntu 24.04 x86_64

These instructions apply to the existing four-binary candidate archive in
`build/ubuntu24-four-binaries-20261106`. This is a local installation draft,
not a public-release approval. Public exchange activation was compiled OFF.

## Runtime test evidence

The user reported a separate runtime smoke test in the Ubuntu 24.04.5 distro
`XGTX-Install-Test`: archive checksum passed, all four binaries had no missing
shared libraries, three `--version` checks passed, and the migration tool
printed usage and exited 1 when run without arguments.

No packages were installed during that test. The distro may already contain
runtime dependencies; this does not establish that the archive runs on a fresh
Ubuntu installation or validate a dependency installation command. Raw logs
and the distro's pre-test package inventory were not supplied. These results
are recorded separately from the earlier build-environment checks in
`build/ubuntu24-four-binaries-20261106/build-report.json`.

## Verify the archive before extraction

Place the archive and its matching `SHA256SUMS` in the same directory, then:

```bash
sha256sum -c SHA256SUMS
```

Expected archive SHA-256:

```text
aeecf9bec857fa14d98059b3f47ba411d85666e083d4e90ada81a7f9d197569e  xgtx-ubuntu24-x86_64-candidate.tar.gz
```

Continue only if verification reports `OK`. A matching checksum detects file
changes; publisher authenticity requires obtaining the expected checksum from
a trusted source. No signed checksum is supplied by this draft.

## Runtime dependencies

**The following install command is unverified on a fresh Ubuntu installation.**
It lists runtime packages observed during the earlier build-environment shared
library checks, including transitive dependencies. It is not a package inventory
from XGTX-Install-Test, nor a proven minimal dependency set. Development packages,
Rust and Cargo are not needed to launch these prebuilt executables.

```bash
sudo apt-get update && sudo apt-get install --no-install-recommends \
  libboost-chrono1.83.0t64 libboost-filesystem1.83.0 \
  libboost-program-options1.83.0 libboost-serialization1.83.0 \
  libboost-thread1.83.0 libbsd0 libc6 libcom-err2 libevent-2.1-7t64 \
  libgcc-s1 libgssapi-krb5-2 libk5crypto3 libkeyutils1 libkrb5-3 \
  libkrb5support0 libmd0 libnorm1t64 libpgm-5.3-0t64 libreadline8t64 \
  libsodium23 libssl3t64 libstdc++6 libtinfo6 libunbound8 libzmq5
```

The observed package versions are recorded in
`runtime_smoke_test.runtime_package_versions` in the build report. The command
above uses the versions offered by Ubuntu's configured repositories, not pinned
versions; compatibility with a fresh installation remains to be tested.

## Extract locally and check launch behavior

Run from the directory containing the verified archive. Use a new directory so
an existing installation is not overwritten:

```bash
(
set -eu
sha256sum -c SHA256SUMS
install_dir=$(mktemp -d "$PWD/xgtx-candidate.XXXXXX")
tar -xzf xgtx-ubuntu24-x86_64-candidate.tar.gz -C "$install_dir"
cd "$install_dir/xgtx"

ldd bin/xgtxd
ldd bin/xgtx-wallet-cli
ldd bin/xgtx-wallet-rpc
ldd bin/xgtx-projection-migrate

bin/xgtxd --version
bin/xgtx-wallet-cli --version
bin/xgtx-wallet-rpc --version

if bin/xgtx-projection-migrate; then
  printf 'Unexpected success: expected usage error with exit code 1\n' >&2
  exit 1
else
  result=$?
  printf 'Migration tool usage check exit code: %s (expected 1)\n' "$result"
  test "$result" -eq 1
fi
)
```

All `ldd` checks should resolve libraries with no `not found` entries. The three
version commands should exit 0. The migration tool does not implement `--help`
or `--version`; with no arguments, its first argument-count check prints usage
to stderr and exits 1 before database construction or opening. Its expected
output is:

```text
FAIL: Usage: xgtx-projection-migrate inspect LMDB | rehearse LMDB COUNT GENESIS TIP ACTIVATION_COUNT
```

These commands do not start a node, open a wallet or run a migration. The archive
also includes LICENSE, historical candidate notes, build instructions, and
build-environment dependency/package listings. This installation draft is a
separate file; the existing archive has not been repackaged to include it.

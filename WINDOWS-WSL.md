# XGTX on Windows using WSL 2 and Ubuntu 24.04

This is the public beta setup for Windows users running the Linux release through WSL 2. It is not a native Windows installer. Use an x86_64/AMD64 computer; this package was not validated on Windows ARM.

On October 17, 2026, the operator installed a separate Ubuntu 24.04 WSL 2 distribution, downloaded and verified the public runtime and interface packages, created a new wallet, synchronized from genesis, opened the connected interface, and restarted the three services through desktop shortcuts. The restarted interface displayed block count 2165. This was an operator-run installation test, not an independent user test. The new wallet was unfunded; transfers, swaps and mining were not tested through this new wallet during this check.

Run commands in order. Where several commands appear in one block, run each line separately. Stop if a checksum fails or a command reports an unexpected error.

## 1. Install Ubuntu 24.04

In **Windows PowerShell**, inspect existing installations:

```powershell
wsl --list --verbose
wsl --list --online
```

If Ubuntu-24.04 is not installed, run:

```powershell
wsl --install -d Ubuntu-24.04
```

Windows may request administrator access or a restart when installing WSL for the first time. Follow [Microsoft's WSL installation instructions](https://learn.microsoft.com/en-us/windows/wsl/install) if needed. Do not remove an existing distribution or wallet. A distribution named just `Ubuntu` may be another version; explicitly select Ubuntu-24.04.

Create a Linux username and password when prompted. `xgtxuser` is an example username. The username can appear in file paths and screenshots. Password characters do not appear while typing.

Open the distribution with:

```powershell
wsl -d Ubuntu-24.04
```

In **Ubuntu**, confirm VERSION_ID is `24.04` and the architecture is `x86_64`:

```bash
cat /etc/os-release
uname -m
```

## 2. Install download prerequisites

Run in **Ubuntu 24.04**, as your ordinary Linux user:

```bash
cd ~
sudo apt update
sudo apt install -y curl ca-certificates python3-requests
```

Use your Linux password for sudo. Run the node and wallet without sudo.

## 3. Download and verify the native runtime

Use a new download directory. If these directories already contain an installation, use the existing installation or choose different paths throughout this guide; do not overwrite existing wallet files.

```bash
mkdir -p ~/xgtx-public-download
cd ~/xgtx-public-download
curl -fL -o runtime.tar.gz https://github.com/xXGAME-TIMEXx/XGTX/releases/download/v0.18.1.0-xgtx-p16/runtime.tar.gz
echo '98884185e10bb09f8cdcd3bd55f5839129ab320168be33409164f7aea0624040  runtime.tar.gz' | sha256sum -c -
```

Require `runtime.tar.gz: OK` before continuing. This checksum identifies this specific release archive; it is not an independent security audit or a separate signing authority.

```bash
tar -xzf runtime.tar.gz
cd ~/xgtx-public-download/xgtx-ubuntu24-x86_64
sha256sum -c SHA256SUMS
```

Require every listed file to report OK, then install runtime dependencies:

```bash
bash install-runtime-deps.sh
```

Complete corresponding source is available as `source.tar.gz` on the [same native release](https://github.com/xXGAME-TIMEXx/XGTX/releases/tag/v0.18.1.0-xgtx-p16). GitHub's automatic source ZIP is not a replacement for that archive.

## 4. Create your own wallet

This command is for a **new wallet**. Do not use it to replace an existing wallet. Choose a filename that does not already exist:

```bash
./bin/xgtx-wallet-cli --offline --generate-new-wallet "$HOME/xgtx-public-test-wallet"
```

Choose a wallet password and securely record its recovery words privately. Never upload recovery words, private keys, wallet files or passwords. The wallet prints a public address that is safe to share to receive funds. The wallet filename is not its public address.

Answer `N` if asked to enable background mining. A daemon connection warning is expected in offline mode. At the wallet prompt, enter:

```text
exit
```

Wait for the ordinary Ubuntu prompt before continuing. Do not open this same wallet simultaneously in the CLI and wallet RPC.

## 5. Download the guided interface

Keep the application folder in a permanent location because generated launchers reference it.

```bash
mkdir -p ~/xgtx-wallet-ui-download
cd ~/xgtx-wallet-ui-download
curl -fL -o wallet-ui.tar.gz https://github.com/xXGAME-TIMEXx/XGTX/releases/download/wallet-ui-20261016-setup/xgtx-local-wallet-ui-20261016-setup.tar.gz
echo '8fae9db92c577331fc72dd5f517e87a56d1786eb01e0bf3e5b5bf92a5606019d  wallet-ui.tar.gz' | sha256sum -c -
```

Require `wallet-ui.tar.gz: OK`, then:

```bash
tar -xzf wallet-ui.tar.gz
cd ~/xgtx-wallet-ui-download/xgtx-local-wallet-ui
sha256sum -c SHA256SUMS
```

Require every listed file to report OK. Prepare the private configuration:

```bash
python3 setup-local.py --bin-dir "$HOME/xgtx-public-download/xgtx-ubuntu24-x86_64/bin" --wallet-file "$HOME/xgtx-public-test-wallet" --output "$HOME/xgtx-local-setup"
```

Expect `PASS: private configuration prepared`. The output directory must not already exist. If you already prepared it, use its launchers rather than rerunning setup. Keep this folder private: it contains RPC credentials. Setup does not start services, start mining or submit transactions.

## 6. Start the three services

Use three separate terminal tabs. In each new PowerShell tab, first run `wsl -d Ubuntu-24.04` to enter the correct distribution.

**First Ubuntu tab — node:**

```bash
"$HOME/xgtx-local-setup/start-node.sh"
```

Wait for `You are now synchronized with the XGTX network`. Keep this terminal running.

**Second Ubuntu tab — wallet:**

```bash
"$HOME/xgtx-local-setup/start-wallet.sh"
```

Enter the wallet password locally. Wait for `Starting wallet RPC server`. Keep this terminal running.

**Third Ubuntu tab — interface:**

```bash
"$HOME/xgtx-local-setup/start-interface.sh"
```

Copy the complete session link printed by the launcher into the browser's address bar. Do not search for it or share it. It controls your open wallet. Keep this terminal running too.

The dashboard should say `Connected to your local wallet`. A new unfunded wallet should show zero balances. Seeing a GTTK trading option does not mean you own GTTK. Installing this stack does not start mining or create income.

The setup uses P2P 48280, daemon RPC 48281, wallet RPC 48283 and interface 48290. All are bound to loopback. Do not open wallet controls or RPC ports to the public.

## 7. Optional Windows desktop shortcuts

After the manual startup works, open **Windows PowerShell**, outside Ubuntu, and run this block. It creates or updates three shortcuts with the names below; it does not start services. It discovers your Linux home directory, so your username need not be `xgtxuser`.

```powershell
$desktop = [Environment]::GetFolderPath('Desktop')
$linuxHome = (wsl -d Ubuntu-24.04 -- printenv HOME).Trim()
if ($LASTEXITCODE -ne 0 -or $linuxHome -notmatch '^/home/[a-zA-Z0-9_-]+$') {
    throw 'Could not identify a regular Ubuntu-24.04 user home. Open Ubuntu as your ordinary user first.'
}
$shell = New-Object -ComObject WScript.Shell
$launchers = @{
    'XGTX 1 - Start Node' = 'start-node.sh'
    'XGTX 2 - Open Wallet' = 'start-wallet.sh'
    'XGTX 3 - Open Interface' = 'start-interface.sh'
}
foreach ($name in $launchers.Keys) {
    $shortcut = $shell.CreateShortcut("$desktop\$name.lnk")
    $shortcut.TargetPath = "$env:WINDIR\System32\wsl.exe"
    $shortcut.Arguments = "-d Ubuntu-24.04 -- $linuxHome/xgtx-local-setup/$($launchers[$name])"
    $shortcut.Save()
}
Write-Host 'Created three XGTX desktop shortcuts.'
```

After stopping the existing services, start shortcuts in numerical order, waiting for synchronization and wallet startup as above. Open the newly printed session link from shortcut 3. Do not start duplicate instances. These shortcuts open the wallet configured in your local setup, not somebody else's treasury wallet. Shortcut startup was tested with the example username; home-directory discovery is a portability adjustment.

## Stop and restart

Press Ctrl+C in the interface terminal first, then the wallet terminal. Wait for `Successfully saved`. Finally press Ctrl+C in the node terminal and wait for shutdown. Start node, wallet and interface in that order next time. Sleeping or shutting down Windows interrupts the local services.

## Troubleshooting

### Interface window opens and immediately closes

In PowerShell enter Ubuntu:

```powershell
wsl -d Ubuntu-24.04
```

Then in Ubuntu run:

```bash
"$HOME/xgtx-local-setup/start-interface.sh"
```

Read the error. `Address already in use` means another process owns the port. Find it from a separate PowerShell tab:

```powershell
wsl -d Ubuntu-24.04 -- ss -ltnp "sport = :48290"
```

Prefer returning to the existing interface terminal and pressing Ctrl+C. If its terminal is unavailable, inspect the PID reported by `ss` in Ubuntu with `ps -p PID -o args= -ww` (replace PID with the actual number). Only after confirming the command is this installation's interface `server.py`, stop that exact process with `kill -TERM PID`. Do not copy a PID from another person's example or kill unrelated Python processes. Then restart shortcut 3 and use its new session link.

### Wallet reports no connection to daemon

Keep the node terminal open, wait for synchronization, then refresh the wallet. If disconnected messages persist, inspect node connectivity before attempting transactions. A no-incoming-connections message alone does not mean you need to expose RPC; this loopback setup connects outward to the public peer.

### Base URL opens but wallet controls do not work

Use the complete link from the currently running interface terminal. A previous session link may no longer work after restarting. Do not post session links in support requests.

### Need help

Open an [issue](https://github.com/xXGAME-TIMEXx/XGTX/issues) with the release tags, Ubuntu version, the step that failed and a redacted error. Never attach wallet files, private setup files, recovery words, passwords or session links.

## Limits

This guide validates installation, synchronization and opening a fresh wallet interface on one operator's Windows/WSL environment. It does not establish support for every Windows version or computer. It is not a native Windows GUI miner, a bridge or an external exchange listing. Native deposits require the network's existing reserve bootstrap. Read the project's [fee and founder-allocation disclosures](README.md) before using the experimental exchange. There is no established dollar price or guaranteed mining or trading income.


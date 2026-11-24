# Production deployment

These commands require administrator access to the XGTX public web host.

## One-command deployment from the XGTX Windows laptop

Download or clone this repository, open PowerShell in `infrastructure/indexer`, and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy-from-windows.ps1
```

The script uses the existing passphrase-protected key at
`$env:USERPROFILE\.ssh\xgtx-deploy`, uploads only the indexer deployment files to
`root@5.78.219.248`, creates timestamped backups, validates the source feed,
starts the unprivileged service, checks Nginx before reloading it, and verifies
the public TLS health endpoint. It refuses to modify Nginx unless it finds
exactly one enabled `xgtxchain.com` configuration.

## Manual deployment

1. Create an unprivileged service account and data directory.

   ```bash
   sudo useradd --system --home /var/lib/xgtx-indexer --shell /usr/sbin/nologin xgtx-indexer
   sudo install -d -o xgtx-indexer -g xgtx-indexer -m 0750 /var/lib/xgtx-indexer /opt/xgtx/indexer
   ```

2. Copy `xgtx_indexer.py` to `/opt/xgtx/indexer/`, owned by root and mode `0755`. Copy `xgtx-indexer.service` to `/etc/systemd/system/`.

3. Verify the live source once before enabling the service.

   ```bash
   sudo -u xgtx-indexer /usr/bin/python3 /opt/xgtx/indexer/xgtx_indexer.py \
     --once --database /var/lib/xgtx-indexer/index.sqlite3
   ```

4. Enable and verify.

   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable --now xgtx-indexer
   curl --fail http://127.0.0.1:48400/health
   ```

5. Add `nginx-location.conf` inside the existing TLS virtual host. Ensure the referenced `limit_req_zone` exists in the HTTP configuration, validate with `sudo nginx -t`, then reload Nginx.

6. Monitor service failures and database growth. Back up `/var/lib/xgtx-indexer/index.sqlite3` with SQLite's online backup mechanism or while the service is stopped.

Do not expose daemon RPC or wallet-RPC through this proxy. Only the indexer's read-only HTTP port belongs behind the public reverse proxy.

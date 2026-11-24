$ErrorActionPreference = "Stop"
$server = "root@5.78.219.248"
$key = Join-Path $env:USERPROFILE ".ssh\xgtx-deploy"
$source = $PSScriptRoot
$remote = "/root/xgtx-indexer-deploy"

if (-not (Test-Path $key)) {
    throw "SSH key not found: $key"
}

$files = @(
    "xgtx_indexer.py",
    "xgtx_indexer_maintenance.py",
    "xgtx-indexer.service",
    "xgtx-indexer-maintenance.service",
    "xgtx-indexer-maintenance.timer",
    "nginx-location.conf",
    "install-production.sh"
)
foreach ($file in $files) {
    if (-not (Test-Path (Join-Path $source $file))) {
        throw "Missing deployment file: $file"
    }
}

ssh.exe -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -i $key $server "install -d -m 0700 $remote"
if ($LASTEXITCODE -ne 0) { throw "Could not create the remote deployment directory." }

foreach ($file in $files) {
    scp.exe -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -i $key `
        (Join-Path $source $file) "${server}:${remote}/$file"
    if ($LASTEXITCODE -ne 0) { throw "Upload failed: $file" }
}

ssh.exe -t -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -i $key `
    $server "chmod 0700 $remote/install-production.sh && $remote/install-production.sh"
if ($LASTEXITCODE -ne 0) { throw "Production installation failed. Read the error above; nginx is not reloaded after a failed validation." }

Write-Host "XGTX historical indexer is live at https://xgtxchain.com/indexer/health" -ForegroundColor Green

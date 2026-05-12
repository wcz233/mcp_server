$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

Push-Location $repoRoot
try {
    Write-Host "[mcp_server] Syncing submodules..."
    git submodule sync --recursive
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "[mcp_server] Initializing submodules..."
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "[mcp_server] Submodule status:"
    git submodule status --recursive
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

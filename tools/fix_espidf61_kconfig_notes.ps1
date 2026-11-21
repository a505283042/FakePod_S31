# Correct three malformed bool defaults in the installed ESP-IDF 6.1.0
# Kconfig files. The upstream release/v6.1 branch already uses "default n".
# This is deliberately OPT-IN: normal PlatformIO builds never modify SDK files.
#
# Usage (PowerShell from repository root):
#   powershell -ExecutionPolicy Bypass -File .\tools\fix_espidf61_kconfig_notes.ps1
#
# A .fakepod-original.bak file is created next to each changed Kconfig.
param(
    [string] $FrameworkDir = (Join-Path $env:USERPROFILE ".platformio\packages\framework-espidf")
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$changes = @(
    @{
        RelativePath = "components\bt\host\nimble\Kconfig.in"
        Symbol = "BT_NIMBLE_MESH_PROVISIONER"
        Before = '(?m)(^[ \t]*config[ \t]+BT_NIMBLE_MESH_PROVISIONER[ \t]*\r?\n[ \t]*bool[^\r\n]*\r?\n[ \t]*default[ \t]+)0(?=[ \t]*\r?$)'
        After = '(?m)^[ \t]*config[ \t]+BT_NIMBLE_MESH_PROVISIONER[ \t]*\r?\n[ \t]*bool[^\r\n]*\r?\n[ \t]*default[ \t]+n[ \t]*\r?$'
    },
    @{
        RelativePath = "components\fatfs\Kconfig"
        Symbol = "FATFS_PRINT_LLI"
        Before = '(?m)(^[ \t]*config[ \t]+FATFS_PRINT_LLI[ \t]*\r?\n[ \t]*depends[^\r\n]*\r?\n[ \t]*bool[^\r\n]*\r?\n[ \t]*default[ \t]+)0(?=[ \t]*\r?$)'
        After = '(?m)^[ \t]*config[ \t]+FATFS_PRINT_LLI[ \t]*\r?\n[ \t]*depends[^\r\n]*\r?\n[ \t]*bool[^\r\n]*\r?\n[ \t]*default[ \t]+n[ \t]*\r?$'
    },
    @{
        RelativePath = "components\fatfs\Kconfig"
        Symbol = "FATFS_PRINT_FLOAT"
        Before = '(?m)(^[ \t]*config[ \t]+FATFS_PRINT_FLOAT[ \t]*\r?\n[ \t]*depends[^\r\n]*\r?\n[ \t]*bool[^\r\n]*\r?\n[ \t]*default[ \t]+)0(?=[ \t]*\r?$)'
        After = '(?m)^[ \t]*config[ \t]+FATFS_PRINT_FLOAT[ \t]*\r?\n[ \t]*depends[^\r\n]*\r?\n[ \t]*bool[^\r\n]*\r?\n[ \t]*default[ \t]+n[ \t]*\r?$'
    }
)

# Validate EVERY pattern before touching any file. Avoid partial patches for
# unexpected framework versions, duplicate options, or already-modified SDKs.
$updates = @{}
foreach ($item in $changes) {
    $path = Join-Path $FrameworkDir $item.RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "ESP-IDF Kconfig not found: $path"
    }
    if (-not $updates.ContainsKey($path)) {
        $updates[$path] = [System.IO.File]::ReadAllText($path)
    }
    $current = $updates[$path]
    $pattern = [regex]::new($item.Before)
    $matches = $pattern.Matches($current)
    if ($matches.Count -eq 1) {
        $updates[$path] = $pattern.Replace($current, '${1}n', 1)
        Write-Host "Will fix: $($item.Symbol)"
    } elseif ($matches.Count -eq 0 -and [regex]::IsMatch($current, $item.After)) {
        Write-Host "Already fixed: $($item.Symbol)"
    } else {
        throw "Unexpected ESP-IDF Kconfig format for $($item.Symbol); no files changed."
    }
}

foreach ($path in $updates.Keys) {
    $original = [System.IO.File]::ReadAllText($path)
    $modified = $updates[$path]
    if ($modified -ceq $original) { continue }
    $backup = "$path.fakepod-original.bak"
    if (-not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $path -Destination $backup
    }
    [System.IO.File]::WriteAllText($path, $modified, $utf8NoBom)
    Write-Host "Patched: $path"
    Write-Host "Backup : $backup"
}

Write-Host "Done. This changes only the local PlatformIO framework Kconfig text."

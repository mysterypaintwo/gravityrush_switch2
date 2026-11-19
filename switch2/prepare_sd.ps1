# Prepares the Switch 2 port's SD card files from your own unpacked copy of the game (Windows).
# Usage (PowerShell, from the repository folder):
#   powershell -ExecutionPolicy Bypass -File switch2\prepare_sd.ps1 <game folder> <output folder>
#     <game folder>    the unpacked game (eboot.bin, sce_module, sce_sys, arc), see switch2\SETUP.txt
#     <output folder>  the SD card's root (for example E:\) or any folder to copy to it later
# Writes <output>\switch\gravityrush_remastered\: the boot files made from your game (prep\), your game's
# files (game\) and the NRO when it is built (out\gr_switch\gravityrush_remastered.nro). Same steps as
# prepare_sd.sh (macOS, Linux). Nothing is downloaded.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Game -PathType Container)) { throw "No folder $Game" }
$Game = (Resolve-Path -LiteralPath $Game).Path
foreach ($need in 'eboot.bin', 'sce_module', 'sce_sys', 'arc') {
    if (-not (Test-Path -LiteralPath (Join-Path $Game $need))) {
        throw "$Game has no $need (see switch2\SETUP.txt)"
    }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo

# Python 3.8 or newer: the launcher (py -3), then python3, then python.
$py = $null
foreach ($candidate in @(@('py', '-3'), @('python3'), @('python'))) {
    $exe = $candidate[0]
    $rest = @($candidate | Select-Object -Skip 1)
    if (Get-Command $exe -ErrorAction SilentlyContinue) {
        & $exe @rest -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>$null
        if ($LASTEXITCODE -eq 0) { $py = $candidate; break }
    }
}
if (-not $py) { throw 'Python 3.8 or newer is needed (python.org, or: winget install Python.Python.3.12)' }
function Invoke-Py {
    $exe = $py[0]
    $rest = @($py | Select-Object -Skip 1)
    & $exe @rest @args
    if ($LASTEXITCODE -ne 0) { throw "Python step failed: $args" }
}

$dest = Join-Path $Out 'switch\gravityrush_remastered'
foreach ($sub in 'prep', 'game', 'user') { New-Item -ItemType Directory -Force -Path (Join-Path $dest $sub) | Out-Null }

Write-Host '== boot files (made from your eboot.bin and modules)'
$prep = Join-Path ([System.IO.Path]::GetTempPath()) ("gr_prep_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $prep | Out-Null
try {
    Invoke-Py scripts/prepare.py $Game --out $prep
    Invoke-Py scripts/link_libc.py $Game --out $prep
    Invoke-Py scripts/link_modules.py $Game --out $prep --modules libc.prx libSceFios2.prx
    Invoke-Py scripts/content_profile.py $Game --out $prep --sku full
    Invoke-Py scripts/patches.py --out $prep --game-dir $Game --patches-dir patches --patches-config patches/patches.json
    foreach ($f in 'boot-linked.bin', 'content.bin', 'patches.bin') {
        Copy-Item -LiteralPath (Join-Path $prep $f) -Destination (Join-Path $dest 'prep') -Force
    }
} finally {
    Remove-Item -LiteralPath $prep -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host '== game files (about 9.3 GB; files already copied unchanged are skipped)'
$gameDest = Join-Path $dest 'game'
Copy-Item -LiteralPath (Join-Path $Game 'eboot.bin') -Destination $gameDest -Force
# args.txt holds the game's launch arguments ("-region E" for the European release): without it the
# game picks the Japanese region and shows Japanese text whatever its language setting.
$argsFile = Join-Path $Game 'args.txt'
if (Test-Path -LiteralPath $argsFile) {
    Copy-Item -LiteralPath $argsFile -Destination $gameDest -Force
} else {
    Write-Warning "no args.txt in $Game; the game will start in Japanese (gravityrush_remastered.nro writes one for the European release)"
}
foreach ($dir in 'sce_module', 'sce_sys', 'arc') {
    # robocopy: exit codes below 8 mean success (0 nothing to copy, 1 files copied, ...).
    robocopy (Join-Path $Game $dir) (Join-Path $gameDest $dir) /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying $dir failed (robocopy $LASTEXITCODE)" }
}

$nro = Join-Path $repo 'out\gr_switch\gravityrush_remastered.nro'
if (Test-Path -LiteralPath $nro) {
    # The homebrew menu icon: your game's own icon (sce_sys\icon0.png) as a 256x256 JPEG, put into
    # the NRO copied to the SD card (the built NRO has none). Needs elf2nro (devkitPro tools).
    $elf = [IO.Path]::ChangeExtension($nro, '.elf')
    $nacp = [IO.Path]::ChangeExtension($nro, '.nacp')
    $png = Join-Path $Game 'sce_sys\icon0.png'
    $devkitpro = if ($env:DEVKITPRO) { $env:DEVKITPRO } else { 'C:\devkitPro' }
    $elf2nro = Join-Path $devkitpro 'tools\bin\elf2nro.exe'
    $done = $false
    if ((Test-Path -LiteralPath $elf) -and (Test-Path -LiteralPath $nacp) -and (Test-Path -LiteralPath $png) -and (Test-Path -LiteralPath $elf2nro)) {
        $jpg = Join-Path ([IO.Path]::GetTempPath()) 'gravityrush_icon.jpg'
        try {
            Add-Type -AssemblyName System.Drawing
            $src = [Drawing.Image]::FromFile($png)
            $bmp = New-Object Drawing.Bitmap 256, 256
            $g = [Drawing.Graphics]::FromImage($bmp)
            $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.DrawImage($src, 0, 0, 256, 256)
            $g.Dispose(); $src.Dispose()
            $bmp.Save($jpg, [Drawing.Imaging.ImageFormat]::Jpeg); $bmp.Dispose()
            & $elf2nro $elf (Join-Path $dest 'gravityrush_remastered.nro') "--icon=$jpg" "--nacp=$nacp" | Out-Null
            $done = $LASTEXITCODE -eq 0
        } catch { $done = $false }
        Remove-Item -LiteralPath $jpg -ErrorAction SilentlyContinue
    }
    if ($done) {
        Write-Host "== gravityrush_remastered.nro copied, with your game's icon"
    } else {
        Copy-Item -LiteralPath $nro -Destination $dest -Force
        Write-Host '== gravityrush_remastered.nro copied (without an icon: see switch2\SETUP.txt)'
    }
} else {
    Write-Host "== gravityrush_remastered.nro is not built yet: copy it to $dest when it is"
}
Write-Host "Done: copy the 'switch' folder in $Out to the root of the SD card (merge with the one there)."

# Convert source audio (mp3, m4a, etc.) into the exact PCM format the
# RP2350 audio path expects: 24 kHz, mono, 16-bit signed little-endian WAV.
#
# Why we pre-convert on the host:
#   - ElevenLabs free tier hands back MP3, but on-device MP3 decoding eats
#     a lot of flash and CPU.
#   - The codec pipeline (audio_pio + ES8311) is already wired for
#     24 kHz mono 16-bit PCM; matching that means zero on-device resampling.
#
# Requires ffmpeg on PATH (winget install Gyan.FFmpeg, or scoop install ffmpeg).
#
# Usage:
#   tools\convert_voicelines.ps1
#   tools\convert_voicelines.ps1 -InputDir voicelines\raw -OutputDir voicelines\wav
#
# Defaults are resolved relative to the repo root (the script's parent dir),
# so it works the same whether you run it from the repo root or from tools\.

param(
    [string]$InputDir,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $InputDir)  { $InputDir  = Join-Path $RepoRoot "voicelines\raw" }
if (-not $OutputDir) { $OutputDir = Join-Path $RepoRoot "voicelines\wav" }

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Error "ffmpeg not found on PATH. Install it (e.g. 'winget install Gyan.FFmpeg') and re-run."
}

if (-not (Test-Path $InputDir)) {
    Write-Error "Input dir '$InputDir' doesn't exist. Drop your source audio there and re-run."
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$converted = 0
$skipped   = 0

Get-ChildItem -Path $InputDir -File | ForEach-Object {
    $src = $_.FullName
    $dst = Join-Path $OutputDir ($_.BaseName + ".wav")

    if ((Test-Path $dst) -and ((Get-Item $dst).LastWriteTime -ge $_.LastWriteTime)) {
        Write-Host "skip   $($_.Name) (up to date)"
        $skipped++
        return
    }

    Write-Host "encode $($_.Name) -> $(Split-Path $dst -Leaf)"
    & ffmpeg -y -loglevel warning `
        -i $src `
        -ar 24000 -ac 1 -acodec pcm_s16le `
        $dst
    if ($LASTEXITCODE -ne 0) {
        Write-Error "ffmpeg failed on $src (exit $LASTEXITCODE)"
    }
    $converted++
}

Write-Host ""
Write-Host "done: $converted converted, $skipped skipped"
Write-Host "copy '$OutputDir\*' to the SD card root (or wherever the firmware looks)"

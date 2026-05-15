# Parse voicelines/SCRIPT.md and emit a C table mapping voiceline filename
# stems to the spoken line of text (for on-screen display during playback).
#
# Reads pairs of lines like:
#   ### 28_sa_much_later.mp3
#   Ask again later. Much later. Like, never.
# and emits an entry like:
#   { "much_later", "Ask again later. Much later. Like, never." },
#
# Unicode punctuation is normalized to ASCII (Montserrat font shipped in
# this build is ASCII-only; multi-byte chars would render as boxes).
#
# Output is only rewritten when content changes, so incremental builds
# don't get pointless rebuilds.

param(
    [Parameter(Mandatory = $true)][string]$ScriptMd,
    [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $ScriptMd)) {
    Write-Error "SCRIPT.md not found at: $ScriptMd"
}

function Sanitize-Text {
    param([string]$s)
    # Normalize fancy unicode punctuation to ASCII equivalents.
    $s = $s -replace [char]0x2026, '...'   # … horizontal ellipsis
    $s = $s -replace [char]0x2014, '--'    # — em dash
    $s = $s -replace [char]0x2013, '-'     # – en dash
    $s = $s -replace [char]0x2018, "'"     # ' left single quote
    $s = $s -replace [char]0x2019, "'"     # ' right single quote
    $s = $s -replace [char]0x201C, '"'     # " left double quote
    $s = $s -replace [char]0x201D, '"'     # " right double quote
    # Escape for a C double-quoted string literal.
    $s = $s -replace '\\', '\\'
    $s = $s -replace '"', '\"'
    return $s
}

$entries = New-Object System.Collections.Generic.List[object]
$pendingStem = $null
foreach ($line in (Get-Content -Path $ScriptMd -Encoding UTF8)) {
    if ($line -match '^###\s+\d+_(?:om|sa|dh)_([a-z0-9_]+)\.mp3\s*$') {
        $pendingStem = $Matches[1]
        continue
    }
    if ($pendingStem -and $line.Trim()) {
        $text = Sanitize-Text $line.Trim()
        $entries.Add([pscustomobject]@{ Stem = $pendingStem; Text = $text }) | Out-Null
        $pendingStem = $null
    }
}

$entries = $entries | Sort-Object Stem

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("// AUTO-GENERATED from voicelines/SCRIPT.md by tools/gen_voicelines_text.ps1.")
[void]$sb.AppendLine("// Do not edit by hand. Re-run the build to regenerate.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine('#include "sage_text.h"')
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const sage_text_entry_t sage_text_table[] = {")
foreach ($e in $entries) {
    [void]$sb.AppendLine("    { `"$($e.Stem)`", `"$($e.Text)`" },")
}
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const size_t sage_text_table_count =")
[void]$sb.AppendLine("    sizeof(sage_text_table) / sizeof(sage_text_table[0]);")

$new = $sb.ToString()

if (Test-Path $Output) {
    $old = Get-Content -Raw -Path $Output
    if ($old -eq $new) {
        Write-Host "sage_text_table.gen.c up to date ($($entries.Count) entries)"
        return
    }
}

$dir = Split-Path -Parent $Output
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
Set-Content -Path $Output -Value $new -NoNewline -Encoding ascii
Write-Host "Generated $Output ($($entries.Count) entries)"

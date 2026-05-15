# Parse voicelines/SCRIPT.md and emit a C table mapping voiceline filename
# stems to their tone bucket (ominous / sarcastic / dry humor).
#
# Reads headings of the form:
#   ### 28_sa_much_later.mp3
# and emits an entry like:
#   { "much_later", SAGE_TONE_SARCASTIC },
#
# The convert_voicelines.ps1 script strips the leading "NN_xx_" prefix
# during the mp3 -> wav bake (it just copies the source basename), so the
# stem we record here is what actually shows up on the SD card.
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

$entries = New-Object System.Collections.Generic.List[object]
foreach ($line in (Get-Content -Path $ScriptMd)) {
    if ($line -match '^###\s+\d+_(om|sa|dh)_([a-z0-9_]+)\.mp3\s*$') {
        $bucket = $Matches[1]
        $stem = $Matches[2]
        $tone = switch ($bucket) {
            'om' { 'SAGE_TONE_OMINOUS' }
            'sa' { 'SAGE_TONE_SARCASTIC' }
            'dh' { 'SAGE_TONE_DRY' }
        }
        $entries.Add([pscustomobject]@{ Stem = $stem; Tone = $tone }) | Out-Null
    }
}

$entries = $entries | Sort-Object Stem

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("// AUTO-GENERATED from voicelines/SCRIPT.md by tools/gen_voicelines_tones.ps1.")
[void]$sb.AppendLine("// Do not edit by hand. Re-run the build to regenerate.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine('#include "sage_tone.h"')
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const sage_tone_entry_t sage_tone_table[] = {")
foreach ($e in $entries) {
    [void]$sb.AppendLine("    { `"$($e.Stem)`", $($e.Tone) },")
}
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const size_t sage_tone_table_count =")
[void]$sb.AppendLine("    sizeof(sage_tone_table) / sizeof(sage_tone_table[0]);")

$new = $sb.ToString()

if (Test-Path $Output) {
    $old = Get-Content -Raw -Path $Output
    if ($old -eq $new) {
        Write-Host "sage_tone_table.gen.c up to date ($($entries.Count) entries)"
        return
    }
}

$dir = Split-Path -Parent $Output
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
Set-Content -Path $Output -Value $new -NoNewline -Encoding ascii
Write-Host "Generated $Output ($($entries.Count) entries)"

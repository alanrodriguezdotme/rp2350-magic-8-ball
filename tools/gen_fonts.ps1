# Regenerates the Cinzel LVGL fonts used by the tarot face (app/font_cinzel_*.c).
# Requires Node.js (uses npx to run lv_font_conv). Run from anywhere:
#   .\tools\gen_fonts.ps1
#
# Cinzel is licensed under the SIL Open Font License 1.1; see tools/fonts/OFL.txt.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ttf = Join-Path $PSScriptRoot 'fonts\Cinzel.ttf'

# size -> output name (lv_font_conv derives the C symbol from the file
# name). 17 px fits every caption in 3 lines on the plate;
# 22 px is the letter-spaced "THE SAGE" title.
$fonts = @(
    @{ Size = 17; Name = 'font_cinzel_17' },
    @{ Size = 22; Name = 'font_cinzel_22' }
)

foreach ($f in $fonts) {
    $out = Join-Path $root "app\$($f.Name).c"
    npx --yes lv_font_conv@1.5.2 `
        --font $ttf -r 0x20-0x7E `
        --size $f.Size --bpp 4 --format lvgl --no-compress `
        --lv-include lvgl.h `
        -o $out
    if ($LASTEXITCODE -ne 0) { throw "lv_font_conv failed for $($f.Name)" }
    Write-Host "wrote $out"
}

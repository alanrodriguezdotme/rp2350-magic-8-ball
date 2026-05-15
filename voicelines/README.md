# voicelines/

Source audio for the Magic 8-Ball voice responses lives here. **Don't commit
the audio itself** — `.gitignore` keeps both subfolders ignored.

## Layout

- `raw/` — drop your source files here (mp3, m4a, wav, whatever ffmpeg can read)
- `wav/` — output of the conversion script: 24 kHz mono 16-bit PCM WAV, ready
  for the firmware to stream off the SD card

## Workflow

```powershell
# from repo root
.\tools\convert_voicelines.ps1
```

That converts every file in `raw/` to the matching `<name>.wav` in `wav/`.
Reruns are cheap — files whose source hasn't changed are skipped.

Then copy `wav/*.wav` to the SD card root. The firmware currently looks for
`0:/answer.wav`; for the very first end-to-end test, just rename your one
clip to `answer.wav` on the card.

## Format constraints (strict)

The on-device WAV player will reject anything that isn't:

- PCM (`audio_format == 1`)
- 1 channel (mono)
- 24000 Hz sample rate
- 16 bits per sample

Pre-converting on the host avoids on-device resampling and keeps a heavy
MP3 decoder out of the firmware.

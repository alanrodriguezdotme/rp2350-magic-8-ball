# voicelines/

Audio for the Magic 8-Ball's answers. The converted, ready-to-use WAVs
are committed, so you can copy them straight to an SD card. The source
MP3s in `raw/` are not committed.

## Layout

- `raw/` — source files from ElevenLabs (mp3, m4a, wav, anything ffmpeg
  can read). Not committed.
- `wav/` — the answers, converted by the script below: 24 kHz mono 16-bit
  PCM WAV, ready for the firmware to stream off the SD card.
- `sfx/` — the two sound effects, `answer.wav` and `shake.wav`, in the
  same format.

## Workflow

To use the included audio, skip to copying. To make your own lines,
generate them from [`SCRIPT.md`](SCRIPT.md), save them in `raw/`, and
convert:

```powershell
# from repo root
.\tools\convert_voicelines.ps1
```

That converts every file in `raw/` to the matching `<name>.wav` in `wav/`.
Reruns are cheap — files whose source hasn't changed are skipped.

Then copy `wav/*.wav` to the SD card's `voicelines/` folder and `sfx/*.wav`
to the card's `sfx/` folder; see the SD card layout in the
[main README](../README.md#sd-card-layout).

## Format constraints (strict)

The on-device WAV player will reject anything that isn't:

- PCM (`audio_format == 1`)
- 1 channel (mono)
- 24000 Hz sample rate
- 16 bits per sample

Pre-converting on the host avoids on-device resampling and keeps a heavy
MP3 decoder out of the firmware.

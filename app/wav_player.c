#include "wav_player.h"

#include "audio_pio.h"
#include "es8311.h"
#include "ff.h"
#include "f_util.h"

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// 4096 samples per buffer * 4 bytes (int32) = 16 KiB.
// At 24 kHz that's ~170 ms of audio per buffer; two buffers gives ~340 ms
// of cushion against SD read latency. The previous 85 ms was tight
// enough that an LVGL full-frame redraw landing in the same window as a
// slow cluster crossing could miss a refill, letting DMA replay stale
// buffer contents and producing the staticky-loud symptom.
#define WAV_BUF_SAMPLES 4096u
#define WAV_BUF_BYTES (WAV_BUF_SAMPLES * sizeof(int32_t))

typedef struct
{
    bool initialized;
    bool playing;

    FIL file;

    // Two DMA channels chained to each other; while one feeds the PIO,
    // the other is being refilled by wav_player_poll().
    int dma_ch[2];

    int32_t buf[2][WAV_BUF_SAMPLES];

    // refill_pending[i] == true means buffer i has finished playing and
    // the IRQ flagged it for refill. Cleared by wav_player_poll() once
    // the new chunk is loaded and the DMA channel re-armed.
    volatile bool refill_pending[2];

    // Set true once we've read the final partial buffer; on the next
    // refill request we stop playback rather than loop garbage.
    bool eof_pending;

    // Number of refill IRQs serviced after eof_pending was first set.
    // We need TWO completions before stopping: the buffer holding the
    // last real samples, and the peer buffer that was already in flight
    // when EOF was detected. Stopping any earlier truncates audio.
    int eof_drain_count;

    uint32_t bytes_remaining; // PCM data bytes left to read from file

    // Lipsync stats from the most recently loaded chunk. Updated in
    // load_buffer(), read by wav_player_get_lipsync(). uint8 reads/writes
    // on Cortex-M33 are atomic, so no locking needed for a face that's
    // happy with ±1 sample of staleness.
    //   rms : 0..255, log-shaped envelope of the chunk's amplitude
    //   zcr : 0..255, fraction of samples whose sign flipped
    volatile uint8_t lipsync_rms;
    volatile uint8_t lipsync_zcr;
} wav_player_t;

static wav_player_t s_p;

// --- lipsync stats ---------------------------------------------------------
//
// Compute simple amplitude (RMS) and zero-crossing rate over the int16
// samples we just read off the SD card. Granularity is one chunk
// (WAV_BUF_SAMPLES at 24 kHz = ~170 ms) — coarse but free, and the face
// module low-passes the result anyway.
static void compute_lipsync_stats(const int16_t *samples, UINT n)
{
    if (n == 0)
    {
        s_p.lipsync_rms = 0;
        s_p.lipsync_zcr = 0;
        return;
    }

    // RMS via float sqrt — RP2350 M33 has single-precision FPU, this is
    // a few microseconds per chunk and we run it ~6 times/second.
    uint64_t sum_sq = 0;
    UINT zero_x = 0;
    int16_t prev = samples[0];
    for (UINT i = 0; i < n; i++)
    {
        int32_t s = samples[i];
        sum_sq += (uint64_t)(s * s);
        if ((prev ^ samples[i]) < 0)
        {
            zero_x++;
        }
        prev = samples[i];
    }
    float rms_f = sqrtf((float)((double)sum_sq / (double)n)); // 0..32768

    // Map RMS to a perceptual 0..255. Voice peaks rarely hit > ~12000
    // RMS even when loud; clamp at 12000 so the "mouth fully open" point
    // sits at typical-loud rather than only on full-scale clipping.
    if (rms_f > 12000.0f)
        rms_f = 12000.0f;
    s_p.lipsync_rms = (uint8_t)(rms_f * (255.0f / 12000.0f));

    // ZCR: typical voice = 100..600 zero crossings per ~170 ms chunk for
    // vowels, 1500..3500 for sibilants/fricatives. Normalize at 2000 so
    // the face's "sibilant" extreme is reachable on real speech.
    UINT cap = (zero_x > 2000) ? 2000 : zero_x;
    s_p.lipsync_zcr = (uint8_t)((cap * 255u) / 2000u);
}

// ---- WAV header parsing ----------------------------------------------------

#pragma pack(push, 1)
typedef struct
{
    char riff[4]; // "RIFF"
    uint32_t file_size;
    char wave[4]; // "WAVE"
} riff_hdr_t;

typedef struct
{
    char id[4];
    uint32_t size;
} chunk_hdr_t;

typedef struct
{
    uint16_t audio_format; // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} fmt_chunk_t;
#pragma pack(pop)

// On entry: file is open, position 0. On success: file is positioned at
// the start of audio data, *data_size_out holds its length in bytes.
static bool parse_wav_header(FIL *fp, uint32_t *data_size_out)
{
    UINT br = 0;
    riff_hdr_t hdr;
    if (f_read(fp, &hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr))
    {
        printf("[wav] short read on RIFF header\r\n");
        return false;
    }
    if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0)
    {
        printf("[wav] not a RIFF/WAVE file\r\n");
        return false;
    }

    bool got_fmt = false;
    for (;;)
    {
        chunk_hdr_t ch;
        if (f_read(fp, &ch, sizeof(ch), &br) != FR_OK || br != sizeof(ch))
        {
            printf("[wav] hit EOF before 'data' chunk\r\n");
            return false;
        }

        if (memcmp(ch.id, "fmt ", 4) == 0)
        {
            fmt_chunk_t fmt;
            UINT to_read = sizeof(fmt) < ch.size ? sizeof(fmt) : ch.size;
            if (f_read(fp, &fmt, to_read, &br) != FR_OK || br != to_read)
            {
                printf("[wav] short read on fmt chunk\r\n");
                return false;
            }
            // Skip any extra fmt bytes (extensible WAV).
            if (ch.size > to_read)
            {
                f_lseek(fp, f_tell(fp) + (ch.size - to_read));
            }
            // fmt chunks with odd size get a pad byte.
            if (ch.size & 1)
            {
                f_lseek(fp, f_tell(fp) + 1);
            }

            if (fmt.audio_format != 1 || fmt.num_channels != 1 ||
                fmt.sample_rate != 24000 || fmt.bits_per_sample != 16)
            {
                printf("[wav] format mismatch: fmt=%u ch=%u rate=%lu bits=%u (need 1/1/24000/16)\r\n",
                       fmt.audio_format, fmt.num_channels,
                       (unsigned long)fmt.sample_rate, fmt.bits_per_sample);
                return false;
            }
            got_fmt = true;
        }
        else if (memcmp(ch.id, "data", 4) == 0)
        {
            if (!got_fmt)
            {
                printf("[wav] 'data' chunk before 'fmt '\r\n");
                return false;
            }
            *data_size_out = ch.size;
            return true;
        }
        else
        {
            // Skip unknown chunk (LIST, INFO, etc.). +1 if odd size.
            uint32_t skip = ch.size + (ch.size & 1);
            if (f_lseek(fp, f_tell(fp) + skip) != FR_OK)
            {
                printf("[wav] failed to skip chunk '%c%c%c%c'\r\n",
                       ch.id[0], ch.id[1], ch.id[2], ch.id[3]);
                return false;
            }
        }
    }
}

// ---- DMA ------------------------------------------------------------------

static void __isr wav_player_dma_irq(void)
{
    if (dma_channel_get_irq1_status(s_p.dma_ch[0]))
    {
        dma_channel_acknowledge_irq1(s_p.dma_ch[0]);
        s_p.refill_pending[0] = true;
    }
    if (dma_channel_get_irq1_status(s_p.dma_ch[1]))
    {
        dma_channel_acknowledge_irq1(s_p.dma_ch[1]);
        s_p.refill_pending[1] = true;
    }
}

// Fill buf[idx] with up to WAV_BUF_SAMPLES from the open file, expanding
// int16 -> int32 (sample << 16) inline. Returns number of samples loaded;
// pads remainder with silence. Sets eof_pending if this was the last read.
static UINT load_buffer(int idx)
{
    // Read int16 PCM into the BOTTOM of the int32 buffer (bytes 0..2*N-1),
    // then expand in place walking backwards. Iteration i reads bytes
    // [2i, 2i+1] and writes bytes [4i, 4i+3]; for any i, the read region
    // sits strictly below the write regions of all higher i's, so source
    // is never clobbered. (The earlier "source in upper half" layout had
    // the source overlapping the destination's high end and produced a
    // single correct sample followed by a buffer of zeros — i.e. an 85 ms
    // click train that sounded like a dial-up modem.)
    int16_t *as_int16 = (int16_t *)s_p.buf[idx];
    UINT to_read = WAV_BUF_SAMPLES * sizeof(int16_t);
    if (s_p.bytes_remaining < to_read)
    {
        to_read = s_p.bytes_remaining;
    }

    UINT br = 0;
    if (to_read > 0)
    {
        if (f_read(&s_p.file, as_int16, to_read, &br) != FR_OK)
        {
            printf("[wav] f_read failed mid-stream\r\n");
            br = 0;
        }
    }
    s_p.bytes_remaining -= br;
    UINT samples = br / sizeof(int16_t);

    // Compute RMS and ZCR before we destroy the int16 buffer with the
    // int32 expansion below. EOF chunks (samples=0) zero the stats so the
    // face's mouth shuts cleanly when audio stops.
    compute_lipsync_stats(as_int16, samples);

    for (int i = (int)samples - 1; i >= 0; i--)
    {
        int16_t s = as_int16[i];
        s_p.buf[idx][i] = ((int32_t)s) << 16;
    }
    // Pad tail with silence.
    for (UINT i = samples; i < WAV_BUF_SAMPLES; i++)
    {
        s_p.buf[idx][i] = 0;
    }

    if (s_p.bytes_remaining == 0)
    {
        s_p.eof_pending = true;
    }
    return samples;
}

static void start_dma_chain(void)
{
    PIO pio = pico_audio.pio_2;
    uint sm = pico_audio.sm_dout;
    uint dreq = pio_get_dreq(pio, sm, true); // true = TX

    for (int i = 0; i < 2; i++)
    {
        dma_channel_config c = dma_channel_get_default_config(s_p.dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, dreq);
        channel_config_set_chain_to(&c, s_p.dma_ch[1 - i]);

        dma_channel_configure(
            s_p.dma_ch[i],
            &c,
            &pio->txf[sm],   // dest: PIO TX FIFO
            s_p.buf[i],      // source: our buffer
            WAV_BUF_SAMPLES, // count
            false            // don't start yet
        );

        dma_channel_set_irq1_enabled(s_p.dma_ch[i], true);
    }

    // Trigger the first channel; it'll auto-chain to the second.
    dma_channel_start(s_p.dma_ch[0]);
}

static void stop_dma_chain(void)
{
    // Break the chain BEFORE aborting. The RP2040 had a documented
    // erratum where aborting a chained channel could leave the chain
    // target in a half-triggered state; the RP2350 DMA block inherits
    // enough that the safe sequence is "self-chain, then abort".
    PIO pio = pico_audio.pio_2;
    uint sm = pico_audio.sm_dout;
    uint dreq = pio_get_dreq(pio, sm, true);

    for (int i = 0; i < 2; i++)
    {
        if (s_p.dma_ch[i] < 0)
            continue;

        dma_channel_config c = dma_channel_get_default_config(s_p.dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, dreq);
        channel_config_set_chain_to(&c, s_p.dma_ch[i]); // self = no chain
        // set_config without trigger; channel may still be running.
        dma_channel_set_config(s_p.dma_ch[i], &c, false);
    }

    for (int i = 0; i < 2; i++)
    {
        if (s_p.dma_ch[i] < 0)
            continue;
        dma_channel_set_irq1_enabled(s_p.dma_ch[i], false);
        dma_channel_abort(s_p.dma_ch[i]);
        dma_channel_acknowledge_irq1(s_p.dma_ch[i]);
    }

    // Drain the PIO TX FIFO so leftover words from the aborted DMA
    // don't get played as garbage when the next pio_sm_put or DMA
    // starts up. This is cheap (max 4 words) and prevents the
    // "synthy noise on next boot beep" symptom.
    pio_sm_clear_fifos(pio, sm);

    s_p.refill_pending[0] = false;
    s_p.refill_pending[1] = false;
}

// ---- public API -----------------------------------------------------------

bool wav_player_init(void)
{
    if (s_p.initialized)
    {
        return true;
    }

    s_p.dma_ch[0] = dma_claim_unused_channel(true);
    s_p.dma_ch[1] = dma_claim_unused_channel(true);
    if (s_p.dma_ch[0] < 0 || s_p.dma_ch[1] < 0)
    {
        printf("[wav] could not claim two DMA channels\r\n");
        return false;
    }

    irq_set_exclusive_handler(DMA_IRQ_1, wav_player_dma_irq);
    irq_set_enabled(DMA_IRQ_1, true);

    s_p.initialized = true;
    printf("[wav] init ok (dma %d/%d, irq DMA_IRQ_1)\r\n",
           s_p.dma_ch[0], s_p.dma_ch[1]);
    return true;
}

bool wav_player_is_playing(void)
{
    return s_p.playing;
}

void wav_player_stop(void)
{
    if (!s_p.playing)
    {
        return;
    }
    stop_dma_chain();
    f_close(&s_p.file);
    s_p.playing = false;
    s_p.eof_pending = false;
    s_p.bytes_remaining = 0;
    s_p.lipsync_rms = 0;
    s_p.lipsync_zcr = 0;
}

void wav_player_get_lipsync(uint8_t *rms_out, uint8_t *zcr_out)
{
    if (rms_out)
        *rms_out = s_p.lipsync_rms;
    if (zcr_out)
        *zcr_out = s_p.lipsync_zcr;
}

bool wav_player_play_file(const char *path)
{
    if (!s_p.initialized)
    {
        printf("[wav] play before init\r\n");
        return false;
    }

    if (s_p.playing)
    {
        wav_player_stop();
    }

    FRESULT fr = f_open(&s_p.file, path, FA_READ);
    if (fr != FR_OK)
    {
        printf("[wav] f_open(%s) failed: %s\r\n", path, FRESULT_str(fr));
        return false;
    }

    uint32_t data_size = 0;
    if (!parse_wav_header(&s_p.file, &data_size))
    {
        f_close(&s_p.file);
        return false;
    }
    s_p.bytes_remaining = data_size;
    s_p.eof_pending = false;
    s_p.eof_drain_count = 0;

    // Re-assert mute=off + configured volume before each play. The
    // codec was occasionally drifting between plays and producing
    // randomly-quiet or randomly-loud-and-staticky audio; clobbering
    // it back to known-good state on every play_file fixes it.
    es8311_voice_mute(false);
    es8311_voice_volume_set(pico_audio.volume);

    // Drain any leftover words in the PIO TX FIFO before we start
    // pushing fresh audio. Stale words from the boot beep or a prior
    // playback's tail can otherwise leak into the first few frames as
    // a click or burst of static.
    pio_sm_clear_fifos(pico_audio.pio_2, pico_audio.sm_dout);

    // Pre-fill both buffers before starting DMA so first chain flip has
    // valid audio to play.
    (void)load_buffer(0);
    (void)load_buffer(1);
    s_p.refill_pending[0] = false;
    s_p.refill_pending[1] = false;

    s_p.playing = true;
    start_dma_chain();
    printf("[wav] playing %s (%lu bytes)\r\n", path, (unsigned long)data_size);
    return true;
}

void wav_player_poll(void)
{
    if (!s_p.playing)
    {
        return;
    }

    // Underrun check: if BOTH channels have refill pending, the chain
    // already re-triggered an empty channel before we got here. Audio
    // will have a glitch. Logged as a diagnostic; bigger buffers should
    // make this rare.
    if (s_p.refill_pending[0] && s_p.refill_pending[1])
    {
        printf("[wav] UNDERRUN (both buffers pending)\r\n");
    }

    for (int i = 0; i < 2; i++)
    {
        if (!s_p.refill_pending[i])
        {
            continue;
        }

        if (s_p.eof_pending)
        {
            s_p.eof_drain_count++;
            if (s_p.eof_drain_count >= 2)
            {
                // First post-EOF completion: the buffer that was in
                // flight when EOF was detected just finished.
                // Second post-EOF completion: the buffer holding the
                // last real samples (with silence pad) just finished.
                // Safe to stop now.
                wav_player_stop();
                printf("[wav] playback complete\r\n");
                return;
            }
            // First post-EOF completion: refill this buffer with silence
            // so if the chain swings back to it before we stop, it
            // doesn't play stale audio. (In practice we stop on the next
            // IRQ, so this silence rarely actually plays.)
            memset(s_p.buf[i], 0, sizeof(s_p.buf[i]));
            dma_channel_set_read_addr(s_p.dma_ch[i], s_p.buf[i], false);
            dma_channel_set_trans_count(s_p.dma_ch[i], WAV_BUF_SAMPLES, false);
            s_p.refill_pending[i] = false;
            continue;
        }

        load_buffer(i);

        // Re-arm this channel for the next chain flip. read_addr resets
        // because it auto-incremented to the end during the last play.
        // trans_count_trig would start it immediately; we want chain to
        // do that, so use the non-trigger setter.
        dma_channel_set_read_addr(s_p.dma_ch[i], s_p.buf[i], false);
        dma_channel_set_trans_count(s_p.dma_ch[i], WAV_BUF_SAMPLES, false);

        s_p.refill_pending[i] = false;
    }
}

#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include <isr.h>          // registers_t for the IRQ handler signature

// ─── AC'97 audio driver (Intel 82801AA ICH codec) ───────────────
// Targets QEMU's `-device AC97` (PCI 8086:2415, a Sigmatel STAC9700
// codec emulation).  Real 16-bit stereo PCM is streamed from guest
// RAM through a Buffer Descriptor List (BDL) ring, so the main CPU
// only refills descriptors every 20 ms — the DMA engine + codec do
// the rest.  This replaces the old PC-speaker "robot voice" for the
// video player's audio track.
//
// The emulated channel is an LVI-extension model: the hardware plays
// descriptors civ, civ+1, ... and HALTS (SR_DCH) when it completes
// descriptor lvi.  The driver keeps lvi pointing at the newest fully
// filled descriptor, extending it one slot per 20 ms PIT tick, so a
// continuous stream plays with no underrun.  Because the refill runs
// inside the PIT IRQ (priority above every task), the UI thread can
// never starve audio: even if the desktop freezes for 600 ms, the
// DMA ring keeps playing.
//
// Data format the codec expects: 16-bit signed little-endian STEREO
// at the DAC rate (48 kHz).  The caller (cpu/audio.c) resamples the
// 8-bit 6 kHz mono sidecar before handing frames here.

// ─── NAM (Native Audio Mixer) register offsets ───────────────────
#define AC97_NAM_RESET            0x00   // write any value = codec reset
#define AC97_NAM_MASTER_VOL       0x02   // 6-bit per ch, inverse, bit15 mute
#define AC97_NAM_PCM_OUT_VOL      0x18   // 5-bit per ch, inverse, bit15 mute
#define AC97_NAM_EXT_AUDIO_ID     0x28   // EACS_VRA bit 0 = variable rate OK
#define AC97_NAM_EXT_AUDIO_CTRL   0x2A   // set EACS_VRA to accept rate writes
#define AC97_NAM_PCM_FRONT_RATE   0x2C   // DAC sample rate (0xBB80 = 48000)

// ─── NABM (Native Audio Bus Mastering) PCM-out offsets ───────────
#define AC97_NABM_PO_BDBAR        0x10   // 32-bit: BDL base phys addr
#define AC97_NABM_PO_CIV          0x14   // 8-bit r: current descriptor index
#define AC97_NABM_PO_LVI          0x15   // 8-bit w: last valid descriptor
#define AC97_NABM_PO_SR           0x16   // 16-bit: status
#define AC97_NABM_PO_PICB         0x18   // 16-bit r: pos in current buffer
#define AC97_NABM_PO_PIV          0x1A   // 8-bit r: prefetched index
#define AC97_NABM_PO_CR           0x1B   // 8-bit: channel control

// Status register bits (write-1-to-clear for the WCLEAR ones)
#define AC97_SR_FIFOE             0x10
#define AC97_SR_BCIS              0x08   // buffer completion
#define AC97_SR_LVBCI             0x04   // last valid buffer complete
#define AC97_SR_CELV              0x02
#define AC97_SR_DCH               0x01   // DMA channel halted

// Control register bits
#define AC97_CR_IOCE              0x10   // interrupt on completion
#define AC97_CR_FEIE              0x08
#define AC97_CR_LVBIE             0x04
#define AC97_CR_RR                0x02   // reset channel
#define AC97_CR_RPBM              0x01   // run/pause: 1 = run

// BDL descriptor ctl_len field bits
#define AC97_BD_IOC               0x80000000u  // interrupt on completion
#define AC97_BD_BUP               0x40000000u  // back up (play silence)
#define AC97_BD_LEN_MASK          0x0000FFFFu  // length in 16-bit samples

// ─── Ring geometry ───────────────────────────────────────────────
// One slot = one 20 ms PIT tick of 48 kHz stereo = 960 frames.
// QEMU's channel walks descriptor indices mod 32, so the ring is
// 32 slots (640 ms of buffered audio) — civ/piv never outrun the
// BDL, and the refill always stays far ahead of the DMA cursor.
#define AC97_SLOT_FRAMES   960
#define AC97_SLOT_SAMPLES  (AC97_SLOT_FRAMES * 2)   // BDL len unit = 16-bit
#define AC97_NSLOTS        32

// ─── Driver API ──────────────────────────────────────────────────
int  ac97_init(void);                  // probe + reset codec; 1 if present
int  ac97_present(void);               // found & programmed successfully

// (Re)start playback: reset the channel and preload the first
// `nframes` frames.  Call once before the first ac97_push().
void ac97_begin(const int16_t* frames, int nframes);

// Push interleaved L/R 16-bit frames into the ring (splits them into
// AC97_SLOT_FRAMES slots, extending LVI as each slot completes).
void ac97_push(const int16_t* frames, int nframes);

// Per-PIT-tick housekeeping: underrun restart, IRQ ack + volume sync.
// Call from audio_tick() while a track is playing.
void ac97_service(int playing);

// Halt the channel immediately (silence).
void ac97_stop(void);

// Mute/unmute the codec's PCM-out channel.
void ac97_mute(int m);

// IRQ handler registered for the codec's PCI IRQ line; acks status.
void ac97_irq(registers_t regs);

#endif

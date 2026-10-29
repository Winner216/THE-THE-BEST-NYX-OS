// cpu/ac97.c  –  Nyx OS AC'97 audio driver (Intel 82801AA ICH codec)
//
// Drives QEMU's `-device AC97` emulation (PCI 8086:2415).  Real PCM
// (16-bit signed little-endian stereo @ 48 kHz) is streamed through a
// 32-entry Buffer Descriptor List (BDL) ring in guest RAM.  The codec
// DMA engine pulls the buffers out of memory and the emulated Sigmatel
// codec converts them to analog — the CPU only refills descriptors.
//
// PLAYBACK MODEL (LVI extension):
//   The channel plays descriptors civ, civ+1, ... and HALTS
//   (SR_DCH) when it completes descriptor lvi.  Each 20 ms PIT tick
//   (audio_tick -> ac97_push) fills ONE whole slot (960 frames = one
//   tick of audio) and writes PO_LVI = that slot, extending the play
//   list.  Fill rate == drain rate, so the ring never empties and
//   never halts.  Underruns (shouldn't happen) are recovered by
//   rewriting LVI, which restarts the channel from its prefetched
//   index.
//
// SCHEDULING: the refill happens inside the PIT IRQ — priority above
// every task the scheduler can run.  The DMA ring adds a 640 ms
// cushion, so even if the UI thread stalls completely, audio keeps
// playing from memory.  This is the "high-priority audio thread" the
// video player needs; there is no user-space decoder thread that can
// be starved.
//
// Volume is handled in software by cpu/audio.c (smoothed per-sample
// gain following the kernel-wide `sound_volume` knob); this driver
// only drives the codec's mute bit.  The PCM-out register stays at
// full scale so the software gain is the single volume control.

#include <ac97.h>
#include <io.h>
#include <speaker.h>     // sound_volume
#include <isr.h>         // irq_install_handler
#include <stdint.h>

// ─── PCI config space helpers (same 0xCF8/0xCFC mechanism as rtl8139) ──
static uint32_t ac97_pci_cfg(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                 ((uint32_t)func << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void ac97_pci_cfg_w(uint8_t bus, uint8_t slot, uint8_t off, uint32_t val) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | (off & 0xFC));
    outl(0xCFC, val);
}

// ─── Driver state ────────────────────────────────────────────────
#define AC97_VENDOR  0x8086u
#define AC97_DEVICE  0x2415u

static int      ac97_ok     = 0;
static uint16_t ac97_nam    = 0;    // Native Audio Mixer I/O base
static uint16_t ac97_nabm   = 0;    // Native Audio Bus Master I/O base
static int      ac97_irqln  = 0;    // PCI IRQ line (0 = none)

// ─── DMA ring + descriptor list (kernel BSS is identity-mapped, so
//     &ring[i] IS its physical address — exactly what the DMA engine
//     reads).  uint32_t alignment: QEMU masks descriptor addrs to
//     &~3 and the BDL base must be 4-byte aligned too.
static int16_t ac97_ring[AC97_NSLOTS][AC97_SLOT_FRAMES * 2]
        __attribute__((aligned(16)));
static uint32_t ac97_bdl[AC97_NSLOTS][2]
        __attribute__((aligned(16)));      // [i][0]=addr [i][1]=ctl_len

static int  ac97_w_idx = 0;    // next slot to fill
static int  ac97_fill  = 0;    // frames written into the current slot
static int  ac97_lvi   = -1;   // last fully-filled slot, -1 = none yet
static int  ac97_vol   = -1;   // last PCM volume reg value written
static int  ac97_muted = 0;

// COM1 probe log (invisible on the VESA screen, visible in QEMU's
// serial log — the same trick isr.c uses for page-fault diagnostics).
static void ac97_serial(const char* s) {
    for (; *s; s++) {
        int spins = 0;
        while ((inb(0x3F8 + 5) & 0x20) == 0 && spins++ < 200000) { }
        outb(0x3F8, (uint8_t)*s);
    }
}

// ─── Codec mixer access ──────────────────────────────────────────
static void ac97_mixer_wr(uint8_t reg, uint16_t v) { outw((uint16_t)(ac97_nam + reg), v); }

// Volume is applied in SOFTWARE by cpu/audio.c (a smoothed 16.16
// per-sample gain), so the codec's PCM-out register stays at full
// scale and only the mute bit is driven here — this keeps volume
// changes zipper-free instead of stepping through the codec's
// coarse 31-step register.  Master volume is left at full/unmuted.
static void ac97_apply_volume(void) {
    if (!ac97_ok) return;
    uint16_t vol = ac97_muted ? 0x8000u : 0x0000u;   // mute bit only
    if ((int)vol != ac97_vol) {
        ac97_vol = vol;
        ac97_mixer_wr(AC97_NAM_PCM_OUT_VOL, vol);
    }
}

// ─── Ring helpers ────────────────────────────────────────────────
static uint8_t ac97_civ(void) {
    return (uint8_t)(inb((uint16_t)(ac97_nabm + AC97_NABM_PO_CIV)) & 0x1F);
}
static void ac97_extend_lvi(void) {
    outb((uint16_t)(ac97_nabm + AC97_NABM_PO_LVI), (uint8_t)(ac97_w_idx & 0x1F));
    ac97_lvi = ac97_w_idx;
    ac97_w_idx = (ac97_w_idx + 1) % AC97_NSLOTS;
    ac97_fill = 0;
}

// ─── Public API ──────────────────────────────────────────────────
int ac97_present(void) { return ac97_ok; }

int ac97_init(void) {
    // 1. Probe PCI bus 0 for the ICH AC'97 controller.
    int found = 0;
    for (int slot = 0; slot < 32 && !found; slot++) {
        uint32_t id = ac97_pci_cfg(0, (uint8_t)slot, 0, 0x00);
        if ((id & 0xFFFFu) == AC97_VENDOR && (id >> 16) == AC97_DEVICE) {
            // 2. Enable I/O space + bus mastering on the device.
            uint32_t cmd = ac97_pci_cfg(0, (uint8_t)slot, 0, 0x04);
            ac97_pci_cfg_w(0, (uint8_t)slot, 0x04, cmd | 0x0005u);
            uint32_t bar0 = ac97_pci_cfg(0, (uint8_t)slot, 0, 0x10);
            uint32_t bar1 = ac97_pci_cfg(0, (uint8_t)slot, 0, 0x14);
            uint8_t  irq  = (uint8_t)(ac97_pci_cfg(0, (uint8_t)slot, 0, 0x3C) & 0xFF);
            ac97_nam  = (uint16_t)(bar0 & ~3u);
            ac97_nabm = (uint16_t)(bar1 & ~3u);
            ac97_irqln = (irq < 16) ? (int)irq : 0;
            found = 1;
        }
    }
    if (!found) {
        ac97_serial("AC97: not present, keeping PC-speaker audio\n");
        return 0;
    }

    // 3. Reset the codec (QEMU's mixer_reset() also defaults the DAC
    //    rate to 48 kHz and opens the PCM-out voice).
    ac97_mixer_wr(AC97_NAM_RESET, 0x0001);
    // 4. Ask for 48 kHz explicitly (Extended Audio Ctrl already has
    //    EACS_VRA set after reset, so the rate write is honoured).
    ac97_mixer_wr(AC97_NAM_PCM_FRONT_RATE, 0xBB80);   // 48000
    // 5. Un-mute: QEMU resets both volumes to MUTED (bit 15).
    ac97_mixer_wr(AC97_NAM_MASTER_VOL, 0x0000);       // full, unmuted
    ac97_mixer_wr(AC97_NAM_PCM_OUT_VOL, 0x0000);

    // 6. Build the descriptor list: each entry points at one ring
    //    slot, length in 16-bit samples, interrupt-on-complete on.
    for (int i = 0; i < AC97_NSLOTS; i++) {
        ac97_bdl[i][0] = (uint32_t)&ac97_ring[i][0];
        ac97_bdl[i][1] = (uint32_t)AC97_SLOT_SAMPLES | AC97_BD_IOC;
    }

    // 7. Register the codec's interrupt (PIT/refill works without it,
    //    but a real driver should service its status register).
    if (ac97_irqln > 0)
        irq_install_handler(ac97_irqln, ac97_irq);

    ac97_ok = 1;
    ac97_vol = -1;
    ac97_muted = 0;
    ac97_apply_volume();

    ac97_serial("AC97: OK (NAM 0x");
    { static const char* hex = "0123456789ABCDEF";
      for (int i = 3; i >= 0; i--) { outb(0x3F8, (uint8_t)hex[(ac97_nam >> (i*4)) & 0xF]); } }
    ac97_serial(" NABM 0x");
    { static const char* hex = "0123456789ABCDEF";
      for (int i = 3; i >= 0; i--) { outb(0x3F8, (uint8_t)hex[(ac97_nabm >> (i*4)) & 0xF]); } }
    ac97_serial(" IRQ ");
    { char b[8]; int n = 0, x = ac97_irqln; if (!x) { b[n++]='0'; } while (x) { b[n++] = (char)('0' + x % 10); x /= 10; } for (int i = n - 1; i >= 0; i--) outb(0x3F8, (uint8_t)b[i]); }
    ac97_serial("\n");
    return 1;
}

void ac97_begin(const int16_t* frames, int nframes) {
    if (!ac97_ok) return;
    // Reset the channel: CIV/LVI/PIV cleared, SR_DCH set, channel idle.
    outb((uint16_t)(ac97_nabm + AC97_NABM_PO_CR), AC97_CR_RR);
    outl((uint16_t)(ac97_nabm + AC97_NABM_PO_BDBAR), (uint32_t)ac97_bdl);
    ac97_w_idx = 0;
    ac97_fill  = 0;
    ac97_lvi   = -1;
    // Preload whatever we were handed (audio.c passes the first few
    // ticks so playback starts immediately).
    if (frames && nframes > 0) ac97_push(frames, nframes);
    // If nothing filled even one slot, make slot 0 silence so the
    // channel never plays stale memory.
    if (ac97_lvi < 0) {
        for (int i = 0; i < AC97_SLOT_FRAMES * 2; i++) ac97_ring[0][i] = 0;
        outb((uint16_t)(ac97_nabm + AC97_NABM_PO_LVI), 0);
        ac97_lvi = 0;
        ac97_w_idx = 1;
        ac97_fill = 0;
    }
    // Start the channel: interrupt-on-completion + last-valid-buffer
    // interrupt enable + run.  QEMU begins at CIV = PIV = 0.
    outb((uint16_t)(ac97_nabm + AC97_NABM_PO_CR),
         AC97_CR_IOCE | AC97_CR_LVBIE | AC97_CR_RPBM);
}

void ac97_push(const int16_t* frames, int nframes) {
    if (!ac97_ok || !frames || nframes <= 0) return;
    while (nframes > 0) {
        // Never overwrite the slot the DMA engine is currently
        // reading: if w_idx caught up to CIV the whole ring is queued.
        if (ac97_lvi >= 0 && ac97_w_idx == (int)ac97_civ())
            return;                       // window full — drop the rest
        int room = AC97_SLOT_FRAMES - ac97_fill;
        int take = (nframes < room) ? nframes : room;
        int16_t* dst = ac97_ring[ac97_w_idx] + ac97_fill * 2;
        for (int i = 0; i < take * 2; i++) dst[i] = frames[i];
        ac97_fill += take;
        nframes  -= take;
        frames   += take * 2;             // take frames = take*2 int16
        if (ac97_fill == AC97_SLOT_FRAMES)
            ac97_extend_lvi();            // slot full → extend the play list
    }
}

void ac97_service(int playing) {
    if (!ac97_ok) return;
    // Ack any pending completion/fifo status bits (write-1-to-clear).
    uint16_t sr = inw((uint16_t)(ac97_nabm + AC97_NABM_PO_SR));
    if (sr & (AC97_SR_FIFOE | AC97_SR_BCIS | AC97_SR_LVBCI))
        outw((uint16_t)(ac97_nabm + AC97_NABM_PO_SR),
             (uint16_t)(sr & (AC97_SR_FIFOE | AC97_SR_BCIS | AC97_SR_LVBCI)));
    // Underrun recovery: if the channel halted (DCH) while we still
    // want sound, silence the current slot and rewrite LVI to restart
    // from the prefetched index.  The silence prevents a "pop" from
    // stale data left in the buffer by a previous track or a prior
    // underrun.  Then the next audio_tick push fills fresh frames.
    if (playing && (sr & AC97_SR_DCH) && ac97_lvi >= 0) {
        // Silence the slot the DMA will restart from.
        int sil_idx = ac97_lvi & 0x1F;
        for (int i = 0; i < AC97_SLOT_FRAMES * 2; i++)
            ac97_ring[sil_idx][i] = 0;
        outb((uint16_t)(ac97_nabm + AC97_NABM_PO_LVI), (uint8_t)sil_idx);
    }
    ac97_apply_volume();
}

void ac97_stop(void) {
    if (!ac97_ok) return;
    outb((uint16_t)(ac97_nabm + AC97_NABM_PO_CR), AC97_CR_RR);
    ac97_lvi = -1;
    ac97_w_idx = 0;
    ac97_fill = 0;
}

void ac97_mute(int m) {
    ac97_muted = m ? 1 : 0;
    ac97_vol = -1;                        // force a register rewrite
    ac97_apply_volume();
}

void ac97_irq(registers_t regs) {
    (void)regs;
    if (!ac97_ok) return;
    uint16_t sr = inw((uint16_t)(ac97_nabm + AC97_NABM_PO_SR));
    if (sr & (AC97_SR_FIFOE | AC97_SR_BCIS | AC97_SR_LVBCI))
        outw((uint16_t)(ac97_nabm + AC97_NABM_PO_SR),
             (uint16_t)(sr & (AC97_SR_FIFOE | AC97_SR_BCIS | AC97_SR_LVBCI)));
}

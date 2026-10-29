# Nyx OS

A custom 32-bit x86 operating system built from scratch in C and assembly, running as a hobbyist teaching/demonstration OS in QEMU (and potentially on real hardware). Boots via GRUB/Multiboot, initializes a full graphical desktop with a compositing window manager, and includes a wide set of built-in applications, drivers, network stacks, multimedia decoders, and a package/app-store system.
███╗   ██╗██╗   ██╗██╗  ██╗     ██████╗ ███████╗
████╗  ██║╚██╗ ██╔╝╚██╗██╔╝    ██╔═══██╗██╔════╝
██╔██╗ ██║ ╚████╔╝  ╚███╔╝     ██║   ██║███████╗
██║╚██╗██║  ╚██╔╝   ██╔██╗     ██║   ██║╚════██║
██║ ╚████║   ██║   ██╔╝ ██╗    ╚██████╔╝███████║
╚═╝  ╚═══╝   ╚═╝   ╚═╝  ╚═╝     ╚═════╝ ╚══════╝  
  
---

## Table of Contents

- [Quick Start](#quick-start)
- [What Is Nyx OS](#what-is-nyx-os)
- [Architecture Overview](#architecture-overview)
- [Kernel & Low-Level Subsystems](#kernel--low-level-subsystems)
- [System Calls (INT 0x80 ABI)](#system-calls-int-0x80-abi)
- [Desktop & User Interface](#desktop--user-interface)
- [Built-In Applications](#built-in-applications)
- [Graphics & Multimedia](#graphics--multimedia)
- [Networking](#networking)
- [Filesystem](#filesystem)
- [Security](#security)
- [Package / App Store System](#package--app-store-system)
- [Ring-3 User Programs & Games](#ring-3-user-programs--games)
- [Tooling & Build System](#tooling--build-system)
- [Testing & E2E Harness](#testing--e2e-harness)
- [Project Structure](#project-structure)
- [Configuration, KVM, and Run Modes](#configuration-kvm-and-run-modes)
- [Limitations & Known Trade-Offs](#limitations--known-trade-offs)
- [License](#license)

---

## Quick Start

Prerequisites: a Linux host with `gcc` (32-bit target), `nasm`, GNU `ld`, `grub-mkrescue`, and (for `make run`) `qemu-system-i386`. A 64-bit build additionally needs `qemu-system-x86_64`.

```bash
# Build the kernel and the bootable ISO
make

# Build everything needed for a full desktop session (ISO + app-store repo)
make run

# Run just the ISO in QEMU without networking or disk
make run-iso

# Clean build artifacts (does NOT delete disk.img or persisted files)
make clean
```

The default QEMU target boots to a graphical desktop. Log in (the default session is configured for immediate desktop access), then click any desktop icon or use the taskbar/start area to launch apps.

```bash
# 64-bit long-mode build
make run64

# Hardware-accelerated run (KVM) when /dev/kvm is available
make run KVM=1
# Force software-emulated (TCG) path
make run KVM=0
```

---

## What Is Nyx OS

Nyx OS is a freestanding, from-scratch x86 OS whose entire userland and kernel are compiled with `-ffreestanding -nostdlib` and linked as a static ELF, then wrapped into a Multiboot-compliant bootable image. It is **not** Linux-based and has no POSIX compatibility layer; instead it defines its own system-call ABI, its own flat in-memory filesystem, its own compositing window manager, and its own ring-3 application model.

Key properties:

- **32-bit x86 primary target** (i386), with a parallel **64-bit long-mode** kernel path (boot64/boot64.asm, paging64, gdt64, idt64, kernel64.c).
- **Multiboot-compliant boot** via GRUB, using VBE for framebuffer mode-setting (not the Multiboot VBE path that QEMU 8.x disables).
- **Full graphical desktop** at up to 1920×1080, driven by a VESA framebuffer and a compositor that blends active windows with drop shadows, per-window alpha, and decorations.
- **Preemptive multitasking** with a PIT-driven timer IRQ, per-task page directories (CR3 switching), and a cooperative yield/sleep syscall layer.
- **Ring-3 application model**: PE32 game executables (DOOM.EXE, CHESS.EXE) run as separate tasks and paint through a game-mode canvas (`SYS_GAMESTART`/`SYS_GAMEBLIT`/`SYS_GETKEY`/`SYS_GETMOUSE`).
- **Rich built-in app suite**: Terminal, Files, Editor, App Library, Trash, Settings, Music, Calculator, Games, Paint, System Monitor, Clock, Calendar, Weather, Organizer, Security, Gallery, Browser, Quick Settings, Video, App Store, and full-screen games.
- **Networking stack** (from the NIC up through sockets, DNS, HTTP, TLS, and a user-space Browser/HTML viewer).
- **Multimedia decoders** written in-kernel: JPEG, PNG, BMP, GIF, SVG, PDF, MP3 (tables + async decode), and MJPEG video playback.
- **App Store / package manager** with a host-side repo server and ring-3 installable packages.

---

## Architecture Overview

The OS is organized as a single kernel image (`nyx.elf`) plus optional ring-3 PE executables embedded in the kernel or served at runtime from an App Store. Boot proceeds through these stages:

1. **Bootloader / Multiboot stub** — `boot/boot.asm` (32-bit) and `boot/boot64.asm` (64-bit). The 32-bit stub passes Multiboot info to `kernel_main()`; the 64-bit path is combined into a Multiboot2-compatible binary for `run64`.
2. **Early kernel init** — VGA text terminal, VESA framebuffer + double buffering, VBE mode set, byte-order self-test, GDT, IDT, IRQs, keyboard, mouse, auth.
3. **Virtual memory** — paging is enabled before interrupts, identity-mapping the kernel (0–1 GiB) and the VESA LFB above 1 GiB, with per-task page directories ready for CR3 switching.
4. **Heap + syscalls** — bump allocator + isolated page allocator, then the `INT 0x80` syscall gate (DPL 3, callable from ring-3 tasks).
5. **High-res clock** — TSC calibrated against the PIT for a µs clock used by the video player master clock / PTS time base.
6. **Multitasking** — `task_init()` registers the kernel loop as task 0 and spawns demo background tasks (worker, blinker).
7. **Devices** — AC'97 audio, SMBus (battery %), embedded filesystem load, desktop icon pre-decode warmup.
8. **Splash → Login → Session loop** — splash shown once, then login, then the desktop event loop (IRQ-driven keyboard/mouse, heartbeat, icon pump, video pump, async JPEG pump, power/session action polling).

The session loop is **IRQ-driven for input** and polls session actions (`power_get_session_action()`) between `hlt` cycles, so Lock/Restart/Shutdown are handled at the top level rather than by returning out of a deep call chain.

---

## Kernel & Low-Level Subsystems

### Boot & early init

- `boot/boot.asm` / `boot/boot64.asm` — Multiboot entry points and early transition.
- `boot/multiboot_transition.asm` / `boot/multiboot_combined.asm` — 64-bit combined binary support.
- `kernel/kernel.c` — `kernel_main()`: orchestrates the full init sequence described above.

### CPU primitives

- **GDT** — `cpu/gdt.c`, `cpu/gdt64.c`, `cpu/gdt_flush.asm` / `gdt_flush.asm`.
- **IDT / IRQs / ISRs** — `cpu/idt.c`, `cpu/idt64.c`, `cpu/isr.c`, `cpu/interrupts.asm` / `interrupts64.asm`, plus the `idt_load.asm` stubs.
- **Paging** — `cpu/paging.c` (32-bit identity map + per-task page dirs + frame allocator), `cpu/paging64.c` (long mode).
- **String / libc scaffolding** — `cpu/string.c`, `cpu/string64.c`, `cpu/bigint.c`, `cpu/divdi3.c` (32-bit division helper), `cpu/crt_stubs.c`.

### Interrupts & devices

- `cpu/interrupts.asm` / `interrupts64.asm` — interrupt stubs; `cpu/isr.c` dispatches.
- `cpu/timer.c`, `cpu/smbus.c` (QEMU PIIX4 PM / battery), `cpu/rtc.c`, `cpu/ec.c`, `cpu/timezone.c`.
- `cpu/power.c` — session actions (lock/restart/shutdown), screensaver integration.
- `cpu/screensaver.c` — idle-driven screen blanking.

### Memory management

- `cpu/heap.c` — kernel heap (identity-mapped region after the kernel image).
- `cpu/task.c` — task struct, creation, scheduling hook, CR3 switching, demo background tasks.
- `cpu/syscall.c` — `INT 0x80` dispatch for the syscall table below.

### Graphics primitives

- `cpu/vesa.c` — VESA framebuffer init, mode set, double buffering, pixel/char/text drawing, self-test.
- `cpu/vbe.c` — VBE mode-setting from Multiboot info.
- `cpu/compositor.c` — per-window surfaces, drop shadows, alpha blending, decorations.
- `cpu/window.c` — window geometry/hit-test helpers used by the compositor and desktop.
- `cpu/gdi32.c` — GDI-style drawing wrappers layered over VESA (`include/gdi32.h`).
- `cpu/video.c` — video playback (MJPEG decode + presentation), including the ECO rung / decode-budget ladder tuned by `make bench`.

### Input

- `cpu/keyboard.c` — PS/2 keyboard IRQ, scancode translation, desktop keyboard dispatch, Alt+Tab switcher integration.
- `cpu/mouse.c` — PS/2 mouse IRQ, packet state machine, acceleration curve, desktop cursor + drag-and-drop ghost rendering at the PS/2 packet rate.

### Audio

- `cpu/ac97.c` — AC'97 PCI codec driver (QEMU `AC97` device), BDL ring, high-priority PIT-IRQ refill.
- `cpu/audio.c` — software volume (smoothed per-sample gain), voice management.
- `cpu/music.c`, `cpu/speaker.c`, `cpu/wasapi.c` — audio playback paths, PC-speaker fallback.
- `cpu/mp3dec.c`, `cpu/mp3tables.c`, `cpu/mp3async.c` — MP3 decode tables + async decode worker.
- `cpu/snd*` conceptually covered by the `SYS_SND*` syscall family.

---

## System Calls (INT 0x80 ABI)

Defined in `include/syscall.h`. The ABI is:

```
eax = syscall number
ebx = arg0   ecx = arg1   edx = arg2
return value in eax
```

Wrappers are callable from any task, including demo background tasks.

| Number | Name | Purpose |
|--------|------|---------|
| 0 | `SYS_EXIT` | Terminate calling task |
| 1 | `SYS_YIELD` | Cooperative CPU handoff |
| 2 | `SYS_SLEEP` | Sleep for `ticks` PIT ticks |
| 3 | `SYS_CREATE` | Spawn a new task (name, entry) |
| 4 | `SYS_GETPID` | Calling task's id |
| 5 | `SYS_ALLOC` | Bump-allocate in task arena |
| 6 | `SYS_ALLOC_PAGE` | Allocate isolated private page(s) |
| 7 | `SYS_PAGES` | Private pages mapped for the task |
| 8 | `SYS_WRITE` | Write bytes to debug console (COM1) |
| 9 | `SYS_SOCKET` | Open a BSD stream socket → fd |
| 10 | `SYS_CONNECT` | Connect fd to ip:port |
| 11 | `SYS_SEND` | Send bytes on fd |
| 12 | `SYS_RECV` | Recv into buffer on fd |
| 13 | `SYS_CLOSE` | Close fd |
| 14 | `SYS_POLL` | Buffered bytes on fd |
| 15 | `SYS_RESOLVE` | Hostname → IP (LE wire format) |
| 16 | `SYS_GAMESTART` | Enter game mode (canvas, title) |
| 17 | `SYS_GAMEBLIT` | Blit BGRA frame into game canvas |
| 18 | `SYS_GETKEY` | Pop one key event (-1 if empty) |
| 19 | `SYS_GAMESTOP` | Leave game mode |
| 20 | `SYS_SNDLOAD` | Cache PCM buffer → handle |
| 21 | `SYS_SNDPLAY` | Trigger voice (handle, vol, chan/sep) |
| 22 | `SYS_SNDSTOP` | Stop voice on chan |
| 23 | `SYS_SNDPARAMS` | Live vol/sep update on chan |
| 24 | `SYS_SNDISPLAY` | Query if voice on chan is playing |
| 25 | `SYS_GETMOUSE` | Drain mouse dx/dy + button bits (-1 off game mode) |

Game-mode syscalls (`SYS_GAMESTART`/`SYS_GAMEBLIT`/`SYS_GETKEY`/`SYS_GETMOUSE`/`SYS_GAMESTOP`) are documented further in `include/game.h` and are the contract between ring-3 games (DOOM.EXE, CHESS.EXE) and the kernel.

---

## Desktop & User Interface

The desktop is implemented in `cpu/desktop.c` (~10.5 kLOC) and is the heart of the user experience. It is a **compositing desktop** with:

- **Taskbar / status bar** with clock, status messages, visual feedback flashes, and app launching.
- **Icon grid** — 14 columns × up to 8 rows, edge-to-edge below the taskbar, with 19 default app icons and empty slots for user arrangement.
- **Squircle tiles** with rounded corners matching the icon art style.
- **Rubber-band multi-selection** — drag from empty desktop to box-select multiple icons; the IRQ handler queries `desktop_sel_count()` / `desktop_sel_has()` at drag-threshold time.
- **Drag-and-drop** with ghost rendering at the PS/2 packet rate (200 Hz), slide-in animation with quadratic ease-out, spring-back snap animation on self-drops, and squish-drop confirmation.
- **Context menus** (right-click) for desktop icons and sidebar items, with hover, separators, and action IDs.
- **Window dragging** and a compositor that blends the active window surface with drop shadows and per-window alpha.
- **Alt+Tab task switcher** — MRU list, swatches open/commit/cancel, keyboard.c swallows keys while active.
- **Quick Settings panel** — brightness, Bluetooth, night light, airplane mode, power mode, volume/brightness sliders, Wi-Fi flyout with a simulated network list.
- **This PC view** in Files — sidebar (Home, Desktop, Documents, Downloads, Music, Pictures, Local Disk, Trash) + main pane with Folders and Devices and drives sections, tile glyphs, and an animated Local Disk capacity bar.
- **Flat filesystem with folder simulation** — folders are entries whose name ends in `/`, and containment is expressed by name prefix (`Folder/readme.txt` lives inside `Folder/`).
- **Trash** — move-to-trash, restore, delete-from-trash, empty-trash.
- **Undo-browse** support for keyboard navigation.

### Visual helpers

- `desktop_set_status(msg)` — temporary status bar message (~1.5 s).
- `desktop_vfx_flash(r,g,b,frames)` — brief full-screen colored flash.
- `desktop_terminal_clear()` — clear the windowed terminal scrollback.
- `desktop_icon_warmup()` — synchronous full icon decode at boot so the first desktop frame is complete.
- `desktop_icon_pump()` / `desktop_video_pump()` / `desktop_async_pump()` — incremental main-loop decoding, never in the PIT IRQ.

---

## Built-In Applications

Nyx OS ships a large built-in app suite. The desktop icon grid order (index into `app_names[]`) and the `MODE_*` constants in `include/desktop.h` are the canonical reference; the list below is the functional summary.

### Core desktop apps

| App | Mode | Notes |
|-----|------|-------|
| Terminal | `MODE_TERMINAL` | Windowed shell with scrollback; VGA text-mode emulation layer renders via VESA pixels when VESA is active, falls back to real VGA text memory otherwise. |
| Files | `MODE_FILES` | Flat filesystem browser with This PC view, folder navigation, rename (F2), create folder/file, drag-and-drop, trash integration. |
| Editor | `MODE_EDITOR` | Text editor; can be opened from Files to view/edit a file, with return-mode tracking so ESC goes back to Files. |
| App Library (AppLib) | `MODE_APPLIB` | App launcher/catalog with categories, search, grid/detail views, and dynamic package entries. |
| Trash | `MODE_TRASH` | Trash bin view with restore / delete / empty. |
| Settings | `MODE_SETTINGS` | Settings panel (sections, selection state). |
| Music | `MODE_MUSIC` | Music player with Playlist/Albums tabs. |
| Calculator (Calc) | `MODE_CALC` | Calculator. |
| Games | `MODE_GAMES` | Games launcher. |
| Paint | `MODE_PAINT` | Paint application. |
| System Monitor (Sysmon) | `MODE_SYSMON` | System monitoring. |
| Clock | `MODE_CLOCK` | Clock. |
| Calendar | `MODE_CALENDAR` | Calendar. |
| Weather | `MODE_WEATHER` | Weather (simulated / no real hardware). |
| Organizer | `MODE_ORGANIZER` | Organizer. |
| Security | `MODE_SECURITY` | Security app. |
| Gallery | `MODE_GALLERY` | Image gallery / slideshow. |
| Browser | `MODE_BROWSER` | HTTP browser with HTML rendering (see Networking). |
| Quick Settings | `MODE_QUICKSETTINGS` | Quick Settings panel. |
| Video | `MODE_VIDEO` | Video player (MJPEG). |
| App Store (Store) | `MODE_STORE` | App Store browser/installer. |
| Doom | full-screen game | DOOM.EXE via game mode. |
| Chess | full-screen game | CHESS.EXE via game mode. |

The "About"/splash content and the desktop's `app_names[]` array list 21 named entries; Doom and Chess appear as desktop icons / games-launcher entries as well as full-screen game-mode apps.

### Keyboard & mouse integration

- Desktop keyboard dispatch handles the full app set's key bindings.
- Mouse IRQ handles cursor, icon hit-testing, drag-and-drop ghost, context-menu navigation, and game-mode close-box hit-testing.
- Game mode is a special full-screen `MODE_GAME` that takes over the canvas and input; the desktop restores the prior mode on exit.

---

## Graphics & Multimedia

### Framebuffer / VESA

- VESA framebuffer with double buffering, VBE mode-setting, and a VGA text-mode emulation layer that renders 8×16 cells as VESA pixel blocks centered in the frame.
- Standard 16-color VGA palette mapped to 24-bit RGB for VESA rendering.
- GDI-style drawing wrappers in `cpu/gdi32.c`.

### Compositing

- `cpu/compositor.c` provides per-window surfaces and composite blending with drop shadows and per-window alpha.
- `cpu/window.c` provides window geometry/hit-test helpers.

### Image decoders (in-kernel)

- **JPEG** — `cpu/jpegdec.c`, `cpu/jpegasync.c` (background worker task), with a de-zigzag / IDCT correction history and a decode-budget ECO ladder tuned via `make bench`.
- **PNG** — `cpu/pngdec.c`.
- **BMP** — `cpu/bmpdec.c`.
- **GIF** — `cpu/gifdec.c`.
- **SVG** — `cpu/svgdec.c` (software rasterizer; desktop icons are SVG/PNG/JPEG/BMP/GIF and are pre-decoded into `host-files/icons.cache` at build time).
- Image Viewer (`MODE_IMGVIEW`) and Gallery (`MODE_GALLERY`) consume these decoders.

### Video playback

- `cpu/video.c` — MJPEG playback with a PTS / master-clock model, decode-budget ECO ladder, and per-frame presentation.
- Host-side transcoding of non-MJPEG MP4s to MJPEG is done at build time by `tools/transcode-videos.py` (gstreamer-based; skipped gracefully if gstreamer is absent).

### PDF

- `cpu/pdfview.c` — PDF viewer.
- `cpu/pdftext.c` — PDF text extraction helper (kernel-side).
- Host-side `tools/extract-pdf-text.py` extracts text from PDFs at build time so the viewer can display content even when the kernel parser can't handle a given compression/font encoding; the companion `.txt` is read by the desktop Enter handler.

### Audio / music

- AC'97 driver (`cpu/ac97.c`) with high-priority PIT-IRQ buffer refill.
- Software volume in `cpu/audio.c`.
- Music player, PC-speaker fallback, and MP3 decode.

### Game mode

- `include/game.h` + `cpu/game.c` define the full-screen game canvas (default 640×400, max 1920×1080), the keyboard event ring, the mouse feed/poll model, and lifecycle (start/stop, PID tracking, title, close-box hit-test).
- Ring-3 games use `SYS_GAMESTART`/`SYS_GAMEBLIT`/`SYS_GETKEY`/`SYS_GETMOUSE` and exit via `SYS_GAMESTOP` or task death.

---

## Networking

Nyx OS has a full TCP/IP stack from the NIC up to user-space sockets and a browser.

### Drivers & link layer

- `cpu/rtl8139.c`, `cpu/e1000.c`, `cpu/nic.c` — PCI Ethernet drivers (RTL8139, e1000) with PCI config-space helpers.
- `cpu/net.c` — low-level network helpers.

### Stack

- `cpu/netstack.c` — IP/TCP/UDP stack.
- `cpu/dhcp.c` — DHCP client.
- `cpu/dns.c` — DNS resolution (`SYS_RESOLVE`).
- `cpu/socket.c`, `cpu/winsock.c` — BSD-style socket API exposed via syscalls.
- `cpu/http.c` — HTTP client.
- `cpu/tls.c`, `cpu/openssl.c` — TLS client (AES-128-CBC/GCM, SHA-1/SHA-256, bigint for RSA).

### Security crypto

- `cpu/aes.c` — AES-128 (FIPS-197) with CBC and GCM (NIST SP 800-38D, GHASH, GF(2^128) multiply).
- `cpu/sha1.c`, `cpu/sha256.c` — SHA-1 and SHA-256.
- `cpu/bigint.c` — big-integer arithmetic used by the RSA/TLS path.

### Browser

- `cpu/browser.c` — HTTP browser with HTML rendering (`cpu/htmlview.c`).
- The browser runs as a desktop app (`MODE_BROWSER`) and reaches the network through the socket/syscall layer.
- A host-side HTTPS proxy (`tools/https_proxy.py`) bridges the guest to the host network for TLS sites during `make run`.

---

## Filesystem

Nyx OS uses a **flat in-memory filesystem** (`cpu/fs.c`, `cpu/fileapi.c`) with these properties:

- Fixed `MAX_FILES` table of entries with name, size, used flag, and content pointer.
- **Folder simulation**: folders are entries whose name ends in `/`; containment is by name prefix (`Folder/readme.txt` is inside `Folder/`).
- Files can be created/deleted/renamed at runtime; the desktop icon list (`desktop_files[]`) mirrors a subset of filesystem entries for desktop placement.
- **Embedded filesystem** — `cpu/embedded_fs.c` populates an in-RAM `filesystem[]` array from `host-files/` at boot via `fs_load_embedded()`. This path always works without a disk driver.
- **Persisted disk** — an optional ATA-backed NYXFS filesystem (`cpu/ata.c`) with a v7 (NYXFS002) format that auto-grows (QEMU raw driver extends the image on writes past the current end). Disk persistence is **currently disabled** in the boot sequence because it caused boot freezes in testing; the driver and persistence code remain intact for re-enablement once the freeze can be diagnosed.
- `host-files/README.txt` documents the host-file packing limits (max 16 files, 16 KB each, 31-char names, no spaces).

---

## Security

- `cpu/auth.c` / `include/auth.h` — authentication subsystem, initialized early in the kernel.
- `cpu/security.c` — scanprotect state loaded from `.scanprotect`, security app support.
- Crypto stack (AES-128-CBC/GCM, SHA-1, SHA-256, bigint) supports TLS client operations.
- Task isolation via per-task page directories (CR3 switching) and private pages (`SYS_ALLOC_PAGE`); cross-task private-page access faults.

---

## Package / App Store System

Nyx OS has an **App Store** with downloadable ring-3 packages:

- **Packages** are ring-3 ELFs (built from `user/pkg*.c`) that are **not** embedded in the kernel; they are served at runtime by a host-side repo server.
- `tools/pkg_store.py` — host-side App Store repo server (port 8123 by default), auto-started by `make run`.
- `tools/pkg_e2e_test.py` and related E2E tests exercise the store install/run flow.
- `cpu/pkg.c`, `cpu/store.c` — kernel-side package handling; `cpu/applib.c` surfaces installed packages (filesystem slots named `@<name>`) as launcher cards.
- Built-in sample packages: `pkghello`, `pkgquote`, `pkgcalc`, `pkgfetch`, `pkglife`.

---

## Ring-3 User Programs & Games

### Embedded user programs

- `user/hello.c`, `user/fault.c`, `user/sockget.c` — freestanding ring-3 demos embedded into the kernel via `tools/embed-userprogs.py` and launched from the shell with `user`, `userfault`, `sockget`.
- `user/userlib.c` / `user/userlib.h` — shared userlib for these programs.
- `user/user.ld` — fixed-address link script for ring-3 ELFs.

### Full-screen games (PE32 executables)

Nyx OS runs real PE32 executables as ring-3 game tasks through the game-mode syscall contract:

- **DOOM.EXE** — built from a doomgeneric port (`user/doom/`), linked as a static ELF at its final runtime base, converted to a flat binary with `objcopy`, then wrapped into a PE32 by `tools/elf2pe.py`. Embedded in `host-files/DOOM.EXE`.
- **CHESS.EXE** — `user/chess/chess_main.c`, a full chess game with an AI search, chat sidebar with a rule-based PC personality, mood system, and (after this project's face-widget change) a mood-tinted face widget in the chat header that changes expression with the PC's mood (calm / smug / panicking, plus confident / wary intermediates). Built through the same ELF→PE pipeline into `host-files/CHESS.EXE`.

Game-mode flow (from `include/game.h`):

1. `SYS_GAMESTART` → open game mode, take the canvas.
2. `SYS_GAMEBLIT` → copy one BGRA frame into the canvas, flag a repaint.
3. `SYS_GETKEY` → pop one (pressed/scancode) event; -1 if empty.
4. `SYS_GETMOUSE` → drain mouse dx/dy + button bits; -1 when not in game mode.
5. `SYS_EXIT` / `SYS_GAMESTOP` → leave game mode.

The kernel's game.c owns the canvas, the keyboard event ring, and the mouse feed/poll model; the desktop couples mode switch + repaint in `desktop_enter_game`/`desktop_exit_game`.

### Chess specifics (from chess_main.c)

- Full-screen 1920×1080 canvas; board at (480,70), 8×8 of 120 px squares; panel at x 1476–1864; chat sidebar at x 8–468.
- AI search with depth 2–5 and per-level node budgets; mood derived from a cheap periodic depth-2 search on the PC's view.
- Mood scale: calm / smug / confident / wary / panicking, with per-mood taunt pools, capture/check/game-over chat hooks, hint/mood/trivia/gg/new-game commands, and a mood-tinted face widget in the chat header.
- Pieces are user SVG art from `host-files/chess_pieces/`, rasterized at build time into `user/chess/pieces.h` by `tools/make_chess_pieces.py`.

---

## Tooling & Build System

The build is driven by `nyx-os/Makefile` and a set of Python/C tools under `tools/`.

### Make targets

| Target | Purpose |
|--------|---------|
| `all` / default | Build `nyx.elf` |
| `iso` | Build `nyx.iso` (GRUB/Multiboot bootable) |
| `run` | ISO + app-store repo + QEMU with KVM(auto)/TCG, networking, disk, audio, SDL display |
| `run-iso` | Standalone ISO boot (no NIC, no disk) |
| `run-tap` | TAP networking run (requires root; `tools/setup-tap.sh`) |
| `proxy` | Host-side HTTPS bridge only (guest reaches `10.0.2.2:8443`) |
| `pkgstore` | App Store repo server only (guest reaches `10.0.2.2:8123`) |
| `pkgrepo` | Stage App Store packages in `pkgrepo/` |
| `chessexe` | Build `host-files/CHESS.EXE` |
| `doomexe` | Confirm `host-files/DOOM.EXE` ready |
| `bench` | Decode-time benchmark (JPEG decode timing per ECO rung) |
| `clean` | Remove build artifacts (not `disk.img`) |
| `clean64` / `run64` / `iso64` | 64-bit long-mode targets |

### Build pipeline highlights

- **Kernel**: `gcc -m32 -ffreestanding -nostdlib -nostdinc -fno-builtin -Wall -Wextra` with `include/` on the include path; linked with `linker.ld`.
- **Ring-3 user programs**: `gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-builtin` with `user/` on the include path; linked with `user/user.ld` at a fixed virtual address.
- **DOOM.EXE / CHESS.EXE**: static ELF linked at final runtime base → `objcopy -O binary` → `tools/elf2pe.py` wraps the flat image in a PE32 header. CHESS also regenerates `user/chess/pieces.h` from SVGs when needed.
- **Embedded filesystem**: `tools/embed-files.py` regenerates `cpu/embedded_fs.c` every build so new `host-files/` entries are picked up.
- **Icon cache**: `tools/icon_cache_gen.c` runs the real kernel decoders on the host to pre-decode app pictures into `host-files/icons.cache`, so boot skips the 19 SVG decodes.
- **PDF text extraction**: `tools/extract-pdf-text.py` extracts companion `.txt` for PDFs at build time.
- **Video transcoding**: `tools/transcode-videos.py` converts non-MJPEG MP4s to MJPEG (gstreamer; skipped if absent).

### Key tools

- `tools/elf2pe.py` — ELF flat binary → PE32 wrapper.
- `tools/embed-files.py` — generate embedded filesystem C from host-files.
- `tools/embed-userprogs.py` — embed ring-3 ELFs into `cpu/userprogs.c`.
- `tools/make_chess_pieces.py` — rasterize chess SVGs into `pieces.h`.
- `tools/make_doomwad.py` — generate `user/doom/nyx_wad.h` from `doom1.wad`.
- `tools/gen-mp3tables.py` — generate `cpu/mp3tables.c` from `tools/HUFFCODE`.
- `tools/icon_cache_gen.c` — host-side decoder-based icon thumbnail cache.
- `tools/transcode-videos.py` — MP4→MJPEG transcoder.
- `tools/extract-pdf-text.py` — PDF text extraction for the viewer.
- `tools/pkg_store.py` — App Store repo server.
- `tools/https_proxy.py` — host-side TLS bridge.
- `tools/setup-tap.sh` — TAP networking setup/teardown.

---

## Testing & E2E Harness

Nyx OS has a substantial E2E test suite under `tools/` that drives QEMU via QMP + serial log + screendumps and asserts behavior end to end. These are the “ Chesstrix bezel” E2E tests referenced in this project's recent work:

- `_chess_e2e_test.py` — chess end-to-end.
- `_chess_play_e2e_test.py` — play chess: launch CHESS.EXE, play 1.e4, verify the AI reply via before/after board reconstruction.
- `_chess_chat_e2e_test.py` — chat E2E: verify the chat sidebar, Q-to-open, typing, PC replies, trivia/mood/taunt commands, Enter-to-leave, and the mood face widget flipping across moods.
- `_chess_mood_probe.py`, `_chess_input_probe.py`, `_chess_dbg_probe.py` — focused probes used to diagnose input/mood/face behavior.
- `_chess_mouse_probe.py` — mouse calibration probe.
- `_chess_selftest.c` / `_chess_selftest.py` — chess self-test.

Other E2E/battery tests cover DOOM, the window manager, packages, the browser, HTTP features, NIC, MP3, video, filesystem operations, PE loading, and sockets.

The chess tests use QMP `input-send-event` for mouse (because HMP `mouse_move` is degenerate with `-display none`) and reconstruct the board from PPM screendumps using solid-interior pixel detection.

---

## Project Structure

```
nyx-os/
├── Makefile                 # Build system (32-bit + 64-bit targets)
├── linker.ld / linker64.ld  # Kernel link scripts
├── nyx.iso                  # Bootable ISO (built)
├── disk.img                 # Persistent raw disk (built, not cleaned)
├── boot/                    # Multiboot stubs (32-bit + 64-bit)
├── kernel/
│   └── kernel.c             # kernel_main(): full init + session loop
├── cpu/                     # Kernel subsystems (drivers, desktop, net, etc.)
│   ├── desktop.c            # Desktop + compositor + file manager + settings + ...
│   ├── shell.c              # Command shell
│   ├── task.c               # Tasking / scheduler
│   ├── fileapi.c            # File API
│   ├── net.c                # Low-level network helpers
│   ├── browser.c            # HTTP browser
│   ├── game.c               # Game-mode canvas + input ring
│   ├── mouse.c              # PS/2 mouse IRQ + cursor + drag ghost
│   ├── keyboard.c           # PS/2 keyboard IRQ + desktop dispatch
│   ├── video.c              # MJPEG playback + decode-budget ladder
│   ├── audio.c              # Software volume / voice management
│   ├── vesa.c               # VESA framebuffer + VGA text emulation
│   ├── compositor.c         # Per-window surfaces + alpha blending
│   ├── window.c             # Window geometry helpers
│   ├── ac97.c               # AC'97 audio driver
│   ├── aes.c                # AES-128 CBC/GCM
│   ├── sha1.c / sha256.c    # SHA-1 / SHA-256
│   ├── bigint.c             # Big-integer (RSA/TLS)
│   ├── netstack.c / socket.c / http.c / tls.c / dns.c / dhcp.c
│   ├── rtl8139.c / e1000.c / nic.c   # Ethernet drivers
│   ├── jpegdec.c / pngdec.c / bmpdec.c / gifdec.c / svgdec.c
│   ├── pdfview.c / pdftext.c
│   ├── mp3dec.c / mp3tables.c / mp3async.c
│   ├── auth.c / security.c
│   ├── pkg.c / store.c / applib.c
│   ├── embedded_fs.c
│   └── … (many more)
├── include/                 # Headers (syscall.h, desktop.h, game.h, etc.)
├── user/                     # Ring-3 user programs + games
│   ├── hello.c / fault.c / sockget.c
│   ├── userlib.c / userlib.h / user.ld
│   ├── doom/                 # DOOM.EXE source (doomgeneric port)
│   └── chess/                # CHESS.EXE source
│       ├── chess_main.c
│       ├── chess.h
│       ├── pieces.h          # generated from SVGs
│       └── chess.ld
├── host-files/               # Files embedded into the OS at build time
│   ├── CHESS.EXE / DOOM.EXE
│   ├── doom1.wad
│   ├── README.txt           # host-file packing limits
│   ├── wallpaper/
│   ├── chess_pieces/        # SVG piece art
│   ├── icons/               # app icon art
│   └── … (images, pdfs, videos, etc.)
├── tools/                   # Build + test tooling
│   ├── elf2pe.py
│   ├── embed-files.py
│   ├── embed-userprogs.py
│   ├── make_chess_pieces.py
│   ├── make_doomwad.py
│   ├── gen-mp3tables.py
│   ├── icon_cache_gen.c
│   ├── transcode-videos.py
│   ├── extract-pdf-text.py
│   ├── pkg_store.py
│   ├── https_proxy.py
│   ├── setup-tap.sh
│   └── *_e2e_test.py, *_probe.py, *_selftest.*
├── iso/ / iso64/            # GRUB build staging
├── pkgrepo/                 # Staged App Store packages
└── build/                   # Build artifacts (chess, doom, userprogs, etc.)
```

---

## Configuration, KVM, and Run Modes

- **KVM acceleration** is the default when `/dev/kvm` exists (`KVM ?= 1`); set `KVM=0` to force TCG software emulation. If `/dev/kvm` exists but init fails (permissions, nested virt), run `KVM=0`.
- **CPU model**: with KVM the run uses `-cpu host`; the kernel currently uses no CPUID/FPU/SSE probing, so host passthrough is safe today.
- **ISO boot** is used instead of direct `-kernel` because QEMU 8.x's built-in multiboot loader removed VBE support, which breaks VESA framebuffer init. Going through GRUB (ISO) restores VBE mode-setting and the full graphical desktop.
- **Audio**: AC'97 via `-device AC97`; falls back to PC speaker when the sound card is absent.
- **Networking**: default `make run` uses user-mode networking (`-netdev user`) with an e1000 device and a packet dump to `net.pcap`; the host-side HTTPS proxy bridges TLS. For real network access, `make run-tap` uses a TAP interface (requires root).
- **Disk**: `disk.img` is a 4M raw image that auto-grows on writes; it is never deleted by `make clean` because cleaning build artifacts should not erase persisted files.

---

## Limitations & Known Trade-Offs

- **32-bit primary**: the main target is i386; the 64-bit path exists but is a separate build and is less feature-complete in practice.
- **No POSIX / Linux compat**: this is a from-scratch OS with its own syscall ABI and flat filesystem; it is not a Linux distribution or a POSIX layer.
- **Flat filesystem**: the filesystem is a fixed-size in-memory table; the optional disk persistence is currently disabled due to boot-freeze issues in testing.
- **Host-file limits**: `host-files/` packing limits (16 files, 16 KB each, 31-char names, no spaces) are documented in `host-files/README.txt`.
- **Browser**: rendering is a custom HTML viewer, not a full modern browser; networking reaches the host via the user-mode stack + optional HTTPS proxy.
- **Games**: DOOM.EXE and CHESS.EXE run as ring-3 PE32 tasks in game mode; they are the main “real executable” demo of that path.
- **Icons / artwork**: app icons and chess pieces are user-supplied SVG/PNG/JPEG/BMP/GIF art; the icon cache is regenerated at build time so decoder fixes are reflected in boot.

---

## License

Nyx OS is a from-scratch hobby OS. License terms, if any, should be stated in a `LICENSE` file at the repository root. If no `LICENSE` file exists, the project is unlicensed by default — check the repository's actual license before redistributing or depending on it.

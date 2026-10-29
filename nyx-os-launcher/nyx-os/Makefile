CC      = gcc
AS      = nasm
LD      = ld

CFLAGS  = -m32 -O2 -nostdlib -nostdinc -fno-builtin \
          -fno-stack-protector -ffreestanding \
          -Wall -Wextra -Iinclude \
          -isystem /usr/lib/gcc/x86_64-linux-gnu/13/include

# Ring-3 user programs: freestanding, no PIE/PIC (linked at a fixed
# virtual address by user/user.ld), no libc (syscalls only).
UCFLAGS = -m32 -O2 -ffreestanding -nostdlib -fno-stack-protector \
          -fno-pic -fno-pie -fno-builtin -Wall -Wextra -Iuser

ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld

KERNEL  = nyx.elf
ISO     = nyx.iso

OBJS = boot/boot.o \
       kernel/kernel.o \
       cpu/terminal.o \
       cpu/vesa.o \
       cpu/desktop.o \
       cpu/mouse.o \
       cpu/fs.o \
       cpu/heap.o \
       cpu/clipboard.o \
       cpu/rtc.o \
       cpu/timezone.o \
       cpu/speaker.o \
       cpu/music.o \
       cpu/splash.o \
       cpu/gdt.o \
       cpu/gdt_flush.o \
       cpu/idt.o \
       cpu/idt_load.o \
       cpu/isr.o \
       cpu/timer.o \
       cpu/interrupts.o \
       cpu/paging.o \
       cpu/syscall.o \
       cpu/keyboard.o \
       cpu/calc.o \
       cpu/divdi3.o \
       cpu/shell.o \
       cpu/auth.o \
       cpu/lockscreen.o \
       cpu/games.o \
       cpu/editor.o \
       cpu/htmlview.o \
       cpu/power.o \
       cpu/paint.o \
       cpu/wallpaper.o \
       cpu/sysmon.o \
       cpu/clock.o \
       cpu/physics.o \
       cpu/calendar.o \
       cpu/theme.o \
       cpu/weather.o \
       cpu/screensaver.o \
       cpu/applib.o \
       cpu/organizer.o \
       cpu/security.o \
       cpu/net.o \
       cpu/ata.o \
       cpu/compositor.o \
       cpu/launcher.o \
       cpu/login.o \
       cpu/pdftext.o \
       cpu/e1000.o \
       cpu/nic.o \
       cpu/rtl8139.o \
       cpu/netstack.o \
       cpu/socket.o \
       cpu/http.o \
       cpu/tls.o \
       cpu/aes.o \
       cpu/bigint.o \
       cpu/sha1.o \
       cpu/sha256.o \
       cpu/ec.o \
       cpu/string.o \
       cpu/dns.o \
       cpu/pkg.o \
       cpu/store.o \
       cpu/pngdec.o \
       cpu/jpegdec.o \
       cpu/jpegasync.o \
       cpu/mp3async.o \
       cpu/imgview.o \
       cpu/gallery.o \
       cpu/bmpdec.o \
       cpu/gifdec.o \
       cpu/svgdec.o \
       cpu/pdfview.o \
       cpu/video.o \
       cpu/game.o \
       cpu/audio.o \
       cpu/gamesnd.o \
       cpu/ac97.o \
       cpu/wasapi.o \
       cpu/mp3dec.o \
       cpu/mp3tables.o \
       cpu/task.o \
       cpu/window.o \
       cpu/browser.o \
       cpu/dhcp.o \
       cpu/vbe.o \
       cpu/elfload.o \
       cpu/peload.o \
       cpu/winapi.o \
       cpu/fileapi.o \
       cpu/sync.o \
       cpu/winsock.o \
       cpu/openssl.o \
       cpu/gdi32.o \
       cpu/qt.o \
       cpu/userprogs.o \
       cpu/smbus.o \
       cpu/embedded_fs.o

all: $(KERNEL)

# ─── Assembly ─────────────────────────────────────────────────────
boot/boot.o: boot/boot.asm
	$(AS) $(ASFLAGS) $< -o $@

cpu/gdt_flush.o: cpu/gdt_flush.asm
	$(AS) $(ASFLAGS) $< -o $@

cpu/idt_load.o: cpu/idt_load.asm
	$(AS) $(ASFLAGS) $< -o $@

cpu/interrupts.o: cpu/interrupts.asm
	$(AS) $(ASFLAGS) $< -o $@

# ─── C sources ────────────────────────────────────────────────────
kernel/kernel.o: kernel/kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/game.o: cpu/game.c include/game.h
	$(CC) $(CFLAGS) -c cpu/game.c -o cpu/game.o

cpu/vesa.o: cpu/vesa.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/terminal.o: cpu/terminal.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/desktop.o: cpu/desktop.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/fs.o: cpu/fs.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/heap.o: cpu/heap.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/clipboard.o: cpu/clipboard.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/rtc.o: cpu/rtc.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/timezone.o: cpu/timezone.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/speaker.o: cpu/speaker.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/music.o: cpu/music.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/splash.o: cpu/splash.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/gdt.o: cpu/gdt.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/idt.o: cpu/idt.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/isr.o: cpu/isr.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/keyboard.o: cpu/keyboard.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/calc.o: cpu/calc.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/shell.o: cpu/shell.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/auth.o: cpu/auth.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/net.o: cpu/net.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/ata.o: cpu/ata.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/launcher.o: cpu/launcher.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/login.o: cpu/login.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/pdftext.o: cpu/pdftext.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/e1000.o: cpu/e1000.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/nic.o: cpu/nic.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/rtl8139.o: cpu/rtl8139.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/netstack.o: cpu/netstack.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/http.o: cpu/http.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/dns.o: cpu/dns.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/lockscreen.o: cpu/lockscreen.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/games.o: cpu/games.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/editor.o: cpu/editor.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/power.o: cpu/power.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/paint.o: cpu/paint.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/wallpaper.o: cpu/wallpaper.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/mouse.o: cpu/mouse.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/sysmon.o: cpu/sysmon.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/clock.o: cpu/clock.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/physics.o: cpu/physics.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/calendar.o: cpu/calendar.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/theme.o: cpu/theme.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/weather.o: cpu/weather.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/applib.o: cpu/applib.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/organizer.o: cpu/organizer.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/security.o: cpu/security.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/screensaver.o: cpu/screensaver.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/bmpdec.o: cpu/bmpdec.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/gifdec.o: cpu/gifdec.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/pdfview.o: cpu/pdfview.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/vbe.o: cpu/vbe.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/task.o: cpu/task.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/mp3dec.o: cpu/mp3dec.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/mp3tables.o: cpu/mp3tables.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/elfload.o: cpu/elfload.c
	$(CC) $(CFLAGS) -c $< -o $@

cpu/peload.o: cpu/peload.c include/peload.h
	$(CC) $(CFLAGS) -c $< -o $@

cpu/winapi.o: cpu/winapi.c include/winapi.h
	$(CC) $(CFLAGS) -c $< -o $@

cpu/sync.o: cpu/sync.c include/sync.h
	$(CC) $(CFLAGS) -c $< -o $@

cpu/winsock.o: cpu/winsock.c include/winsock.h
	$(CC) $(CFLAGS) -c $< -o $@

cpu/openssl.o: cpu/openssl.c include/openssl.h
	$(CC) $(CFLAGS) -c $< -o $@

cpu/gdi32.o: cpu/gdi32.c include/gdi32.h include/vesa.h
	$(CC) $(CFLAGS) -c $< -o $@

cpu/userprogs.o: cpu/userprogs.c include/userprogs.h
	$(CC) $(CFLAGS) -c $< -o $@

doomexe: host-files/DOOM.EXE
	@echo "✅ DOOM.EXE ready in host-files/"

# ─── CHESS.EXE — ring-3 chess game (same PE pipeline as DOOM.EXE) ──
CHESS_SRC  = user/chess
CHESS_BASE = 0xC1001000
CHESS_CFLAGS = -m32 -O2 -fno-pic -fno-pie -fno-stack-protector \
	-fno-builtin -ffreestanding -fno-strict-aliasing -fomit-frame-pointer \
	-Wall -Wno-unused-function -Wno-main -nostdinc -I$(CHESS_SRC) \
	-idirafter /usr/lib/gcc/x86_64-linux-gnu/13/include -MMD -MP
CHESS_OBJS = $(patsubst $(CHESS_SRC)/%.c,build/chess/%.o,$(wildcard $(CHESS_SRC)/*.c))

-include $(CHESS_OBJS:.o=.d)

build/chess/%.o: $(CHESS_SRC)/%.c
	@mkdir -p build/chess
	$(CC) $(CHESS_CFLAGS) -c $< -o $@

# Piece sprite art: rasterize the user's SVGs into pieces.h.  The SVG
# filenames contain SPACES ("bishop black.svg") which make prerequisite
# lists can't carry, so the recipe itself checks freshness with find.
user/chess/pieces.h: tools/make_chess_pieces.py
	@if [ ! -f $@ ] \
	   || [ tools/make_chess_pieces.py -nt $@ ] \
	   || [ -n "$$(find host-files/chess_pieces -name '*.svg' -newer $@ -print -quit 2>/dev/null)" ]; then \
		python3 tools/make_chess_pieces.py; \
	fi

$(CHESS_OBJS): user/chess/pieces.h

build/chess/chess.bin: build/chess/chess.elf
	objcopy -O binary $< $@

host-files/CHESS.EXE: build/chess/chess.bin build/chess/chess.elf tools/elf2pe.py
	python3 tools/elf2pe.py build/chess/chess.bin $@ $(CHESS_BASE) --elf build/chess/chess.elf

build/chess/chess.elf: $(CHESS_OBJS) $(CHESS_SRC)/chess.ld
	$(LD) -m elf_i386 -T $(CHESS_SRC)/chess.ld \
		$(CHESS_OBJS) -o $@ -e nyx_pe_entry \
		/usr/lib/gcc/x86_64-linux-gnu/13/32/libgcc.a

chessexe: host-files/CHESS.EXE
	@echo "✅ CHESS.EXE ready in host-files/"

# ISO HUFFCODE input changes (same pattern as host-files/icons.cache —
# a stale table is how wrong sfb rows slip back in).
cpu/mp3tables.c: tools/gen-mp3tables.py tools/HUFFCODE
	python3 tools/gen-mp3tables.py

# ─── DOOM.EXE — a real PE32 built from the doomgeneric port ────────
# Pipeline: gcc -m32 freestanding → static ELF linked at its FINAL
# runtime base (0xC1001000 = USER_LINK_BASE+0x1000, so no relocs) →
# objcopy -O binary → tools/elf2pe.py wraps the flat image in a PE32
# header → host-files/ so it shows up in Files.  Opening it in the
# desktop runs pe_exec → ring-3 DOOM.
DOOM_SRC = user/doom
DOOM_BASE = 0xC1001000
DOOM_CFLAGS = -m32 -O2 -fno-pic -fno-pie -fno-stack-protector \
	-fno-builtin -ffreestanding -fno-strict-aliasing -fomit-frame-pointer \
	-Wall -Wno-unused-function -Wno-main -nostdinc -I$(DOOM_SRC) -I$(DOOM_SRC)/shim -idirafter /usr/lib/gcc/x86_64-linux-gnu/13/include \
	-MMD -MP \
	-DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400
DOOM_OBJS = $(patsubst $(DOOM_SRC)/%.c,build/doom/%.o,$(wildcard $(DOOM_SRC)/*.c))

# Header dependency files emitted by -MMD: rebuild dependents when a
# struct layout or prototype changes (a player_t offset skew between
# stale r_main.o and fresh p_user.o crashed R_DrawPSprite).
-include $(DOOM_OBJS:.o=.d)

build/doom/%.o: $(DOOM_SRC)/%.c
	@mkdir -p build/doom
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

user/doom/nyx_wad.h: tools/make_doomwad.py host-files/doom1.wad
	python3 tools/make_doomwad.py host-files/doom1.wad user/doom/nyx_wad.h

build/doom/doom.bin: build/doom/doom.elf
	objcopy -O binary $< $@

host-files/DOOM.EXE: build/doom/doom.bin tools/elf2pe.py
	python3 tools/check_doom_elf.py build/doom/doom.elf
	python3 tools/elf2pe.py build/doom/doom.bin $@ $(DOOM_BASE) --elf build/doom/doom.elf

build/doom/doom.elf: $(DOOM_OBJS) user/doom/doom.ld user/doom/nyx_wad.h
	$(LD) -m elf_i386 -T user/doom/doom.ld \
		$(DOOM_OBJS) -o $@ -e nyx_pe_entry \
		/usr/lib/gcc/x86_64-linux-gnu/13/32/libgcc.a

# ─── Ring-3 user programs ────────────────────────────────────────
# Built into ELF32 linked at USER_LINK_BASE (user/user.ld), embedded
# into the kernel via tools/embed-userprogs.py, launched from the
# shell with `user` / `userfault` / `sockget`.  userlib.o is shared.
user/userlib.o: user/userlib.c user/userlib.h
	$(CC) $(UCFLAGS) -c $< -o $@

user/hello.elf: user/hello.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/hello.c -o user/hello.o
	$(LD) -m elf_i386 -T user/user.ld user/hello.o user/userlib.o -o $@

user/fault.elf: user/fault.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/fault.c -o user/fault.o
	$(LD) -m elf_i386 -T user/user.ld user/fault.o user/userlib.o -o $@

user/sockget.elf: user/sockget.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/sockget.c -o user/sockget.o
	$(LD) -m elf_i386 -T user/user.ld user/sockget.o user/userlib.o -o $@

# ─── App Store packages (ring-3 ELFs served by tools/pkg_store.py) ──
# These are NOT embedded into the kernel — they are the downloadable
# apps the kernel package manager installs at runtime.
user/pkghello.elf: user/pkghello.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/pkghello.c -o user/pkghello.o
	$(LD) -m elf_i386 -T user/user.ld user/pkghello.o user/userlib.o -o $@

user/pkgquote.elf: user/pkgquote.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/pkgquote.c -o user/pkgquote.o
	$(LD) -m elf_i386 -T user/user.ld user/pkgquote.o user/userlib.o -o $@

user/pkgcalc.elf: user/pkgcalc.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/pkgcalc.c -o user/pkgcalc.o
	$(LD) -m elf_i386 -T user/user.ld user/pkgcalc.o user/userlib.o -o $@

user/pkgfetch.elf: user/pkgfetch.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/pkgfetch.c -o user/pkgfetch.o
	$(LD) -m elf_i386 -T user/user.ld user/pkgfetch.o user/userlib.o -o $@

user/pkglife.elf: user/pkglife.c user/userlib.o user/user.ld
	$(CC) $(UCFLAGS) -c user/pkglife.c -o user/pkglife.o
	$(LD) -m elf_i386 -T user/user.ld user/pkglife.o user/userlib.o -o $@

# Stage the packages + build the repository directory for pkg_store.py.
pkgrepo: user/pkghello.elf user/pkgquote.elf user/pkgcalc.elf \
         user/pkgfetch.elf user/pkglife.elf
	mkdir -p pkgrepo
	cp user/pkghello.elf pkgrepo/pkghello.elf
	cp user/pkgquote.elf pkgrepo/pkgquote.elf
	cp user/pkgcalc.elf pkgrepo/pkgcalc.elf
	cp user/pkgfetch.elf pkgrepo/pkgfetch.elf
	cp user/pkglife.elf pkgrepo/pkglife.elf
	@echo "✅ App Store packages staged in pkgrepo/"

cpu/userprogs.c: user/hello.elf user/fault.elf user/sockget.elf \
                 tools/embed-userprogs.py
	python3 tools/embed-userprogs.py

cpu/socket.o: cpu/socket.c include/socket.h include/netstack.h
cpu/smbus.o: cpu/smbus.c include/smbus.h

# ─── Link kernel ─────────────────────────────────────────────────
$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@echo "✅ Nyx kernel built: $(KERNEL)"

# ─── Build bootable ISO ──────────────────────────────────────────
iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/nyx.elf
	@echo 'set timeout=3'                           >  iso/boot/grub/grub.cfg
	@echo 'set default=0'                           >> iso/boot/grub/grub.cfg
	@echo 'set gfxmode=1920x1080x32,1366x768x32,1280x800x32,1024x768x32' >> iso/boot/grub/grub.cfg
	@echo 'set gfxpayload=keep'                     >> iso/boot/grub/grub.cfg
	@echo ''                                        >> iso/boot/grub/grub.cfg
	@echo 'menuentry "Nyx OS" {'                    >> iso/boot/grub/grub.cfg
	@echo '    multiboot /boot/nyx.elf'             >> iso/boot/grub/grub.cfg
	@echo '    boot'                                >> iso/boot/grub/grub.cfg
	@echo '}'                                       >> iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso/
	@echo "💿 ISO ready: $(ISO)"

# ─── Host files ─────────────────────────────────────────────────
# Drop .png / .jpg files into host-files/ and they're automatically
# embedded into the kernel at compile time (via cpu/embedded_fs.c)
# AND packed into disk.img for ATA persistence when enabled.
#
# The embed path always works (no disk driver needed) — the gallery
# reads from the in-RAM filesystem[] array that fs_load_embedded()
# populates at boot.
HOST_FILES_DIR = host-files
# Exclude README/doc files from packing (they'd waste filesystem slots)
HOST_FILES     = $(filter-out $(HOST_FILES_DIR)/README%, $(wildcard $(HOST_FILES_DIR)/*))

# Force regeneration of embedded_fs.c every build so new files in
# host-files/ are picked up even when the script itself hasn't changed.
.PHONY: _force-embed

# Auto-extract text from PDFs (host-side, using pdftotext) so the Nyx
# OS PDF viewer can display them even when they use compression or
# complex font encodings that the kernel-side parser can't handle.
# Each .pdf gets a companion .txt file read by desktop.c's ENTER handler.
.PHONY: _extract-pdf-text
_extract-pdf-text:
	@for f in host-files/*.pdf; do \
		[ -f "$$f" ] || continue; \
		txt="$${f%.pdf}.txt"; \
		if [ ! -f "$$txt" ] || [ "$$f" -nt "$$txt" ]; then \
			python3 tools/extract-pdf-text.py "$$f"; \
		fi; \
	done

# Build-time icon thumbnail cache — pre-decodes all app pictures
# (SVG/PNG/JPEG/BMP/GIF) on the HOST with the real kernel decoders
# into host-files/icons.cache, so boot skips the 19 SVG decodes
# entirely.  Rebuilt automatically when any icon or the generator
# changes.
# Depends on the decoder sources too — a decoder fix (e.g. the JPEG
# de-zigzag correction) must regenerate the cached thumbnails, or the
# embedded icons keep the OLD decode until the cache is forced.
host-files/icons.cache: tools/icon_cache_gen.c $(wildcard host-files/icons/*) \
        cpu/svgdec.c cpu/pngdec.c cpu/jpegdec.c cpu/bmpdec.c cpu/gifdec.c
	@gcc -O2 -I include tools/icon_cache_gen.c cpu/svgdec.c cpu/pngdec.c cpu/jpegdec.c cpu/bmpdec.c cpu/gifdec.c -o /tmp/icon_cache_gen
	@/tmp/icon_cache_gen

# Convert non-MJPEG MP4s (H.264/HEVC) in host-files/ to MJPEG so the
# kernel's video player can actually play them.  Requires gst-launch-1.0
# (gstreamer1.0-tools + gst-libav + gst-plugins-good); if it's missing the
# files are skipped with a warning and the build still succeeds.
.PHONY: _transcode-videos
_transcode-videos:
	@python3 tools/transcode-videos.py

# Generate the embedded C source from host files.
# The .PHONY prerequisite forces this to run every make invocation,
# so dropping a new file into host-files/ is always picked up.
cpu/embedded_fs.c: tools/embed-files.py _force-embed _extract-pdf-text _transcode-videos host-files/icons.cache
	python3 tools/embed-files.py

# Also pack into disk.img for ATA persistence (when re-enabled).
# Uses shell globbing (via printf + xargs) instead of Make's
# $(wildcard) so filenames with spaces are handled correctly.
pack-host-files: disk.img
	@files=$$(find host-files -maxdepth 1 -type f ! -name 'README*' -print0 2>/dev/null | xargs -0 echo); \
	if [ -n "$$files" ]; then \
		python3 tools/pack-files.py $$files; \
	fi

# ─── Run in QEMU ─────────────────────────────────────────────────
#
# NOTE: Booting via ISO (CD-ROM) instead of direct -kernel, because
# QEMU 8.x's built-in multiboot loader removed VBE support, which
# breaks VESA framebuffer initialization (error: "multiboot knows
# VBE. we don't"). By going through GRUB (via ISO), VBE mode
# setting works correctly and the OS boots to the full graphical
# desktop with all apps.
#
# If this is too slow (ISO rebuild takes ~5 seconds), you can try
# the legacy -kernel path with:
#   qemu-system-i386 -kernel $(KERNEL) ...
# but note that VESA won't initialize — you'll get VGA text mode
# only, and the desktop/browser/other GUI apps won't render.
#
# NOTE: 'run' intentionally does NOT re-pack disk.img.  pack-host-files
# regenerates disk.img from host-files/, which wipes any files and the
# icon layout persisted at runtime by fs_save_to_disk().  Run
# 'make pack-host-files' manually only when you want to re-baseline the
# disk from the current host-files/ contents.

# ─── Hardware acceleration (KVM) ────────────────────────────────
# QEMU defaults to TCG, which software-emulates every guest
# instruction on the host CPU — the video decode, desktop redraw,
# and boot work all pay that cost.  When the host exposes /dev/kvm,
# accelerate the whole VM with KVM so the guest runs at near-native
# speed on the host's VT-x/AMD-V hardware instead.  The kernel uses
# no CPUID/FPU/SSE probing, so exposing the host CPU model (-cpu
# host) is safe today; if the kernel ever enables FPU/SSE, re-init
# CR0/CR4 first — host passthrough would then expose those features.
# Force the old software-emulated path with:
#     make run KVM=0
KVM ?= 1
# KVM=1 (default): use KVM when /dev/kvm exists, else TCG.  Any other
# value (KVM=0) forces TCG.  If /dev/kvm exists but KVM init fails
# (permissions, nested virt), QEMU errors out loudly — run KVM=0.
KVM_FLAGS := $(if $(filter-out 1,$(KVM)),,$(shell test -e /dev/kvm && echo "-enable-kvm -cpu host" || echo ""))
KVM_MSG := $(if $(KVM_FLAGS),with KVM hardware acceleration (near-native CPU),under TCG software emulation)

run: iso pkgrepo
	@set -e; \
	echo "==> Starting https bridge (port 8443)..."; \
	python3 tools/https_proxy.py > /tmp/nyx_https_proxy.log 2>&1 & \
	PROXY_PID=$$!; \
	echo "==> Starting App Store repo (port 8123)..."; \
	python3 tools/pkg_store.py > /tmp/nyx_pkg_store.log 2>&1 & \
	STORE_PID=$$!; \
	echo "==> QEMU starting $(KVM_MSG)..."; \
	qemu-system-i386 $(KVM_FLAGS) -cdrom $(ISO) -m 1G -boot d \
	    -audiodev alsa,id=snd \
	    -machine pcspk-audiodev=snd \
	    -device AC97,audiodev=snd \
	    -netdev user,id=net0 -device e1000,netdev=net0 \
	    -object filter-dump,id=f1,netdev=net0,file=net.pcap \
	    -drive file=disk.img,format=raw,if=ide,index=1 \
	    -display sdl,gl=on,window-close=on \
	    -vga std; \
kill $$PROXY_PID 2>/dev/null || true; \
kill $$STORE_PID 2>/dev/null || true

# ─── Standalone https bridge ────────────────────────────────────
# Run the host-side TLS bridge on its own (the guest reaches it at
# 10.0.2.2:8443):
#     make proxy
proxy:
	python3 tools/https_proxy.py

# ─── Standalone App Store repo ──────────────────────────────────
# Build the packages and serve the store on its own (the guest
# reaches it at 10.0.2.2:8123 — also auto-started by `make run`):
#     make pkgstore
pkgstore: pkgrepo
	python3 tools/pkg_store.py

# ─── TAP networking run target ───────────────────────────────────
# Requires root. Runs tools/setup-tap.sh to create tap0, configure
# NAT, and clean up on exit. The guest gets 192.168.100.2 on subnet
# 192.168.100.0/24, with gateway 192.168.100.1 (the host tap0 IP).
#
# After booting, open Terminal and run:
#     net config dhcp
# or manually:
#     net config 192.168.100.2 192.168.100.1
# then open the Browser and navigate.
#
# If your host Wi-Fi interface is not wlp2s0, edit HOST_WAN in
# tools/setup-tap.sh or set HOST_WAN=eth0 make run-tap.
run-tap: iso pack-host-files
	sudo ./tools/setup-tap.sh up
	@echo "==> Starting QEMU with TAP networking $(KVM_MSG)..."
	qemu-system-i386 $(KVM_FLAGS) -cdrom $(ISO) -m 1G -boot d \
    -audiodev alsa,id=snd \
    -machine pcspk-audiodev=snd \
    -device AC97,audiodev=snd \
	    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	    -device e1000,netdev=net0 \
	    -drive file=disk.img,format=raw,if=ide,index=1 \
	    -display sdl,gl=on,window-close=on \
	    -vga std; \
	./tools/setup-tap.sh down

# ─── Legacy: direct ISO boot (no NIC, no disk) ───────────────────
run-iso: iso
	@echo "==> QEMU starting $(KVM_MSG)..."
	qemu-system-i386 $(KVM_FLAGS) -cdrom $(ISO) -m 1G -boot d \
    -audiodev alsa,id=snd \
    -machine pcspk-audiodev=snd \
    -device AC97,audiodev=snd \
	    -display sdl,gl=on,window-close=on \
	    -vga std

# ─── Disk image ──────────────────────────────────────────────────
# A persistent disk for the virtual filesystem (cpu/fs.c + cpu/ata.c).
# Only created if it doesn't already exist, so it's never silently
# wiped/recreated by a normal build — your saved files stay put
# across rebuilds. Not part of `clean`'s rm list for the same reason:
# cleaning build artifacts shouldn't erase what's actually stored on
# the "disk".
#
# The v7 (NYXFS002) format AUTO-GROWS: fs_save_to_disk() packs files
# at a frontier and QEMU's raw driver extends the image whenever a
# write lands past the current end.  So the initial image only needs
# to hold the superblock + directory + icon-layout sector (~13
# sectors) — a small 4M start is plenty and capacity scales on its
# own, with no re-running of migration tools.
disk.img:
	qemu-img create -f raw disk.img 4M

# ─── Decode-time benchmark (KVM when available, else TCG) ─────
# Builds tools/bench/ into a tiny multiboot ELF (the kernel's OWN
# jpegdec.c + gdt/idt, booted via -kernel) that times 704×704 decode
# per ECO rung and prints ms over COM1 serial.  Used to tune the ECO
# ladder thresholds in cpu/video.c against real in-guest TCG cost:
#   FULL ~160 ms, vertical-half ~84 ms, both-axes ~50 ms (vs the
#   125 ms frame interval at 8 fps; native ~22/11/5 ms).  With
#   KVM the guest runs near-native, so the ladder stays on full-res.
#   (Readings near 0 ms under KVM are PIT-granularity artifacts of
#   near-native speed — the ladder only cares that they're ≪ budget.)
# Run:   make bench
# (Requires qemu-system-i386; frame.h is regenerated from jace.mp4.)
.PHONY: bench
bench:
	mkdir -p /tmp/nyx-bench
	python3 tools/bench/gen-frame.py
	$(AS) $(ASFLAGS) tools/bench/boot.asm -o /tmp/nyx-bench/boot.o
	$(AS) $(ASFLAGS) tools/bench/irq.asm -o /tmp/nyx-bench/irq.o
	$(AS) $(ASFLAGS) cpu/gdt_flush.asm -o /tmp/nyx-bench/gdtf.o
	$(AS) $(ASFLAGS) cpu/idt_load.asm -o /tmp/nyx-bench/idtl.o
	for f in tools/bench/main.c cpu/gdt.c cpu/idt.c cpu/jpegdec.c cpu/divdi3.c; do \
		$(CC) $(CFLAGS) -fno-pie -Itools/bench -c $$f -o /tmp/nyx-bench/$$(basename $$f .c).o || exit 1; \
	done
	$(LD) $(LDFLAGS) -o /tmp/nyx-bench/tcgbench.elf \
	    /tmp/nyx-bench/boot.o /tmp/nyx-bench/irq.o /tmp/nyx-bench/gdtf.o \
	    /tmp/nyx-bench/idtl.o /tmp/nyx-bench/main.o /tmp/nyx-bench/gdt.o \
	    /tmp/nyx-bench/idt.o /tmp/nyx-bench/jpegdec.o /tmp/nyx-bench/divdi3.o
	@echo "▶ Booting decode benchmark $(KVM_MSG)..."
	# Watchdog: the bare machine idles forever after the bench prints
	# TCGBENCH DONE, so bound the run so `make bench` always returns.
	timeout 200 qemu-system-i386 $(KVM_FLAGS) -kernel /tmp/nyx-bench/tcgbench.elf -m 256M \
	    -display none -serial file:/tmp/nyx-bench/out.txt -no-reboot || true
	@echo "── Decode timing (FULL / vertical-half / both-axes) ──"
	@cat /tmp/nyx-bench/out.txt
	# A triple-faulted ELF or an interrupted boot produces a truncated
	# log — fail the target so a regression is caught, not papered over.
	@grep -q 'TCGBENCH DONE' /tmp/nyx-bench/out.txt || \
	    (echo '✗ bench output incomplete (no TCGBENCH DONE)' >&2; exit 1)

# ─── Clean ───────────────────────────────────────────────────────
clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
	rm -rf iso/
	@echo "🧹 Cleaned build files"


# ═══════════════════════════════════════════════════════════════════
# 64-bit Build Targets (x86_64 long mode)
# ═══════════════════════════════════════════════════════════════════

CC64    = gcc
AS64    = nasm
LD64    = ld

CFLAGS64 = -m64 -O2 -nostdlib -nostdinc -fno-builtin \
           -fno-stack-protector -ffreestanding -mcmodel=large \
           -Wall -Wextra -Iinclude \
           -isystem /usr/lib/gcc/x86_64-linux-gnu/13/include

ASFLAGS64 = -f elf64
LDFLAGS64 = -m elf_x86_64 -T linker64.ld

KERNEL64 = nyx64.elf
ISO64    = nyx64.iso

# 64-bit object files
OBJS64 = boot/boot64.o \
         kernel/kernel64.o \
         cpu/gdt64.o \
         cpu/idt64.o \
         cpu/interrupts64.o \
         cpu/paging64.o \
         cpu/string64.o

# 64-bit build rules
boot/boot64.o: boot/boot64.asm
	$(AS64) $(ASFLAGS64) $< -o $@

cpu/interrupts64.o: cpu/interrupts64.asm
	$(AS64) $(ASFLAGS64) $< -o $@

kernel/kernel64.o: kernel/kernel64.c
	$(CC64) $(CFLAGS64) -c $< -o $@

cpu/gdt64.o: cpu/gdt64.c
	$(CC64) $(CFLAGS64) -c $< -o $@

cpu/idt64.o: cpu/idt64.c
	$(CC64) $(CFLAGS64) -c $< -o $@

cpu/paging64.o: cpu/paging64.c
	$(CC64) $(CFLAGS64) -c $< -o $@

cpu/string64.o: cpu/string64.c
	$(CC64) $(CFLAGS64) -c $< -o $@

$(KERNEL64): $(OBJS64)
	$(LD64) $(LDFLAGS64) $(OBJS64) -o $@
	@echo "✅ 64-bit kernel built: $@"

# The combined binary embeds the 64-bit kernel in a 32-bit Multiboot stub
COMBINED64 = nyx_combined.elf

$(COMBINED64): $(KERNEL64)
	@echo "Building 64-bit combined binary..."
	objcopy -O binary $(KERNEL64) /tmp/nyx64.bin
	nasm -f elf32 boot/multiboot_combined.asm -o boot/multiboot_combined.o
	ld -m elf_i386 -T /tmp/link32.ld boot/multiboot_combined.o -o $(COMBINED64)

$(ISO64): $(COMBINED64)
	mkdir -p iso64/boot/grub
	@echo 'set timeout=3'                          >  iso64/boot/grub/grub.cfg
	@echo 'set default=0'                          >> iso64/boot/grub/grub.cfg
	@echo 'set gfxmode=1920x1080x32,1366x768x32'  >> iso64/boot/grub/grub.cfg
	@echo 'set gfxpayload=keep'                    >> iso64/boot/grub/grub.cfg
	@echo ''                                       >> iso64/boot/grub/grub.cfg
	@echo 'menuentry "Nyx OS (64-bit)" {'        >> iso64/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/nyx64.elf'         >> iso64/boot/grub/grub.cfg
	@echo '    boot'                                >> iso64/boot/grub/grub.cfg
	@echo '}'                                       >> iso64/boot/grub/grub.cfg
	cp $(COMBINED64) iso64/boot/nyx64.elf
	grub-mkrescue -o $(ISO64) iso64/
	@echo "💿 64-bit ISO ready: $(ISO64)"

# Run 64-bit kernel in QEMU
run64: $(COMBINED64)
	qemu-system-x86_64 -kernel $(COMBINED64) -m 2G -vga std -display sdl
	qemu-system-x86_64 -cdrom $(ISO64) -m 2G -vga virtio -display sdl

# Clean 64-bit build artifacts
clean64:
	rm -f $(OBJS64) $(KERNEL64) $(ISO64)

.PHONY: run64 clean64

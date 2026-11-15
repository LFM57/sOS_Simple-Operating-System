PREFIX = i686-linux-gnu-
CC = $(PREFIX)gcc
CFLAGS = -ffreestanding -O0 -nostdlib -Wall -Wextra
AS = $(PREFIX)as
ASFLAGS = 
LD = $(PREFIX)ld
LDFLAGS = -T linker.ld

# Cross-compiler for the user application (Ring 3)
USER_CC = i686-linux-gnu-gcc
USER_LD = i686-linux-gnu-ld
USER_CFLAGS = -ffreestanding -nostdlib -fno-pie -Wall -Wextra -I.

GUI_APPS = paint.sos files.sos sysmon.sos edit.sos calc.sos
USER_APPS = app.sos $(GUI_APPS)

OBJS = boot.o print.o memory.o gdt_idt.o disk.o fs.o vfs.o kernel.o pci.o e1000.o net.o crypto.o mouse.o gui.o

# The 'all' target compiles everything
all: kernel.bin drive.img sos.iso drive.vdi sign

# Renamed myos.bin to kernel.bin
kernel.bin: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

# User-space app.sos target (console app)
app.sos: userland/app.c sos.c sos.h app_linker.ld
	@echo "Compiling user application (app.sos)..."
	$(USER_CC) $(USER_CFLAGS) -c sos.c -o sos.o
	$(USER_CC) $(USER_CFLAGS) -c $< -o app.o
	$(USER_LD) -T app_linker.ld app.o sos.o -o $@
	rm -f app.o sos.o

# User-space GUI apps
$(GUI_APPS): %.sos: userland/%.c libgui.c libgui.h sos.c sos.h font8x16.h app_linker.ld
	@echo "Compiling GUI application ($@)..."
	$(USER_CC) $(USER_CFLAGS) -c sos.c -o sos.o
	$(USER_CC) $(USER_CFLAGS) -c libgui.c -o libgui.o
	$(USER_CC) $(USER_CFLAGS) -c $< -o $*.o
	$(USER_LD) -T app_linker.ld $*.o libgui.o sos.o -o $@
	rm -f $*.o libgui.o sos.o

# Automatic OTA signing: signs uinstruct, kernel.bin, and all files ordered by instructions
sign: kernel.bin $(USER_APPS)
	@if [ -f sign_update.py ]; then \
		echo "Generating automatic OTA signatures..."; \
		python3 sign_update.py; \
	fi

# Creation of the 64 MB raw FAT32 disk
drive.img: $(USER_APPS) kernel.bin
	@echo "Creation of the 64 MB disk..."
	dd if=/dev/zero of=drive.img bs=1M count=64 status=none
	@echo "Formatting in FAT32..."
	/usr/sbin/mkfs.fat -F 32 drive.img
	
	@echo "Creating user database..."
	echo "root:20fd0e45:0" > passwd
	echo "user:364b5f18:1" >> passwd
	echo "guest:811c9dc5:2" >> passwd
	
	@echo "Copying system files to FAT32 Hard Drive..."
	mcopy -i drive.img passwd ::/
	mcopy -i drive.img kernel.bin ::/
	
	@echo "Creating Unix-like filesystem hierarchy..."
	mmd -i drive.img ::/bin
	mmd -i drive.img ::/home
	mmd -i drive.img ::/home/user
	mmd -i drive.img ::/home/guest
	mmd -i drive.img ::/dev
	
	@echo "Populating /bin with binaries..."
	for app in $(USER_APPS); do mcopy -i drive.img $$app ::/bin/; done
	
	@echo "Setting /bin permissions (0x70, owner UID 0)..."
	@python3 -c '\
import struct;\
f = open("drive.img", "r+b");\
bpb = f.read(512);\
bps, spc, res, fats = struct.unpack_from("<HBHB", bpb, 11);\
spf, root = struct.unpack_from("<I4xI", bpb, 36);\
fat0, data, csz = res * bps, (res + fats * spf) * bps, spc * bps;\
walk = lambda c: [] if c < 2 or c >= 0x0FFFFFF8 else [c] + walk((f.seek(fat0 + c * 4), struct.unpack("<I", f.read(4))[0] & 0x0FFFFFFF)[1]);\
rd = lambda c: [(data + (cl - 2) * csz, (f.seek(data + (cl - 2) * csz), f.read(csz))[1]) for cl in walk(c)];\
wr = lambda p, v: (f.seek(p + 12), f.write(bytes([v])));\
bin_c = [];\
[(wr(base + o, 0x70), bin_c.append((struct.unpack_from("<H", buf, o + 20)[0] << 16) | struct.unpack_from("<H", buf, o + 26)[0])) for base, buf in rd(root) for o in range(0, csz, 32) if buf[o] not in (0, 0xE5) and buf[o + 11] != 0x0F and buf[o:o + 11] == b"BIN        "];\
[wr(base + o, 0x70) for c in bin_c for base, buf in rd(c) for o in range(0, csz, 32) if buf[o] not in (0, 0xE5) and buf[o + 11] != 0x0F and (buf[o + 11] & 0x08) == 0 and buf[o:o + 11] != b"..         "];\
f.close()'
	
	rm -f passwd

# The ISO now acts ONLY as a GRUB bootloader. It searches the HDD for kernel.bin.
sos.iso:
	@echo "Preparing GRUB Bootloader ISO..."
	mkdir -p isodir/boot/grub
	@echo 'set default=0' > isodir/boot/grub/grub.cfg
	@echo 'set timeout_style=hidden' >> isodir/boot/grub/grub.cfg
	@echo 'set timeout=2' >> isodir/boot/grub/grub.cfg
	@echo 'menuentry "sOS - Simple Operating System" {' >> isodir/boot/grub/grub.cfg
	@echo '    search --file /kernel.bin --set root' >> isodir/boot/grub/grub.cfg
	@echo '    multiboot /kernel.bin' >> isodir/boot/grub/grub.cfg
	@echo '    boot' >> isodir/boot/grub/grub.cfg
	@echo '}' >> isodir/boot/grub/grub.cfg
	@echo 'menuentry "sOS - Text Login Mode" {' >> isodir/boot/grub/grub.cfg
	@echo '    search --file /kernel.bin --set root' >> isodir/boot/grub/grub.cfg
	@echo '    multiboot /kernel.bin text' >> isodir/boot/grub/grub.cfg
	@echo '    boot' >> isodir/boot/grub/grub.cfg
	@echo '}' >> isodir/boot/grub/grub.cfg
	@echo "Generating bootable ISO with grub-mkrescue..."
	grub-mkrescue -o sos.iso isodir

# Automatic conversion of the hard disk for VirtualBox
drive.vdi: drive.img
	@echo "Converting drive.img to VirtualBox format (drive.vdi)..."
	qemu-img convert -f raw -O vdi drive.img drive.vdi

clean:
	rm -f *.o kernel.bin drive.img test.txt $(USER_APPS) guidemo.sos app.bin sos.iso drive.vdi *.sig
	rm -rf isodir
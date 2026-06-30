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

OBJS = boot.o print.o memory.o gdt_idt.o disk.o fs.o kernel.o pci.o e1000.o net.o crypto.o

# The 'all' target compiles everything
all: kernel.bin drive.img sos.iso drive.vdi

# Renamed myos.bin to kernel.bin
kernel.bin: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

# User-space app.sos target
app.sos: app.c sos.c
	@echo "Compiling user application (app.sos)..."
	$(USER_CC) -ffreestanding -nostdlib -fno-pie -c sos.c -o sos.o
	$(USER_CC) -ffreestanding -nostdlib -fno-pie -c app.c -o app.o
	$(USER_LD) -T app_linker.ld app.o sos.o -o app.sos
	rm -f app.o sos.o

# Creation of the 16 MB raw FAT16 disk
drive.img: app.sos kernel.bin
	@echo "Creation of the 16 MB disk..."
	dd if=/dev/zero of=drive.img bs=1M count=16 status=none
	@echo "Formating in FAT16..."
	/usr/sbin/mkfs.fat -F 16 drive.img
	
	@echo "Creating user database..."
	echo "root:20fd0e45:0" > passwd
	echo "user:364b5f18:1" >> passwd
	echo "guest:811c9dc5:2" >> passwd
	
	@echo "Copying system files to FAT16 Hard Drive..."
	mcopy -i drive.img passwd ::/
	mcopy -i drive.img app.sos ::/
	mcopy -i drive.img kernel.bin ::/
	
	@echo "Creating personnal directories..."
	mmd -i drive.img ::/user
	mmd -i drive.img ::/guest
	
	rm -f passwd

# The ISO now acts ONLY as a GRUB bootloader. It searches the HDD for kernel.bin.
sos.iso:
	@echo "Preparing GRUB Bootloader ISO..."
	mkdir -p isodir/boot/grub
	@echo 'menuentry "sOS - Simple Operating System" {' > isodir/boot/grub/grub.cfg
	@echo '    search --file /kernel.bin --set root' >> isodir/boot/grub/grub.cfg
	@echo '    multiboot /kernel.bin' >> isodir/boot/grub/grub.cfg
	@echo '    boot' >> isodir/boot/grub/grub.cfg
	@echo '}' >> isodir/boot/grub/grub.cfg
	@echo "Generating bootable ISO with grub-mkrescue..."
	grub-mkrescue -o sos.iso isodir

# Automatic conversion of the hard disk for VirtualBox
drive.vdi: drive.img
	@echo "Converting drive.img to VirtualBox format (drive.vdi)..."
	qemu-img convert -f raw -O vdi drive.img drive.vdi

clean:
	rm -f *.o kernel.bin drive.img test.txt app.sos app.bin sos.iso drive.vdi kernel.sig
	rm -rf isodir
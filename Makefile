CC = gcc
LD = ld
NASM = nasm
GRUB_MKRESCUE = grub-mkrescue
QEMU = qemu-system-i386

CFLAGS = -m32 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
         -fno-stack-protector -fno-pie -mno-mmx -mno-sse -mno-sse2 \
         -Os -Wall -Wextra -I.

LDFLAGS = -m elf_i386
ASMFLAGS = -f elf32

C_SOURCES = kernel.c mouse.c wm.c font.c calc.c cpanel.c fat.c disk.c filemanager.c mm.c keyboard.c editor.c
ASM_SOURCES = boot/multiboot.asm boot/vbe.asm
HEADERS = mouse.h wm.h font.h calc.h cpanel.h port.h version.h fat.h disk.h filemanager.h mm.h keyboard.h editor.h

C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = boot/multiboot.o boot/vbe.o
OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

KERNEL_ELF = iso/boot/kernel.elf
VERSION_STR = $(shell grep "KERNEL_BUILD" version.h | head -1 | cut -d '"' -f 2)
ISO_FILE = pixel_neko_build_$(VERSION_STR).iso

.PHONY: all clean run debug rebuild

all: version.h $(ISO_FILE)

version.h: build_counter.sh
	@echo "  build..."
	@./build_counter.sh

%.o: %.c $(HEADERS)
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	@echo "  NASM  $<"
	$(NASM) $(ASMFLAGS) $< -o $@

$(KERNEL_ELF): $(OBJECTS) link_grub.ld
	@echo "  LD    $@"
	@mkdir -p iso/boot/grub
	$(LD) $(LDFLAGS) -T link_grub.ld $(OBJECTS) -o $@

iso/boot/grub/grub.cfg:
	@mkdir -p iso/boot/grub
	@echo 'set timeout=20' > $@
	@echo 'set default=0' >> $@
	@echo '' >> $@
	@echo 'menuentry "Pixel Neko Next" {' >> $@
	@echo '    multiboot /boot/kernel.elf' >> $@
	@echo '    boot' >> $@
	@echo '}' >> $@

$(ISO_FILE): $(KERNEL_ELF) iso/boot/grub/grub.cfg
	@echo "  ISO   $@"
	$(GRUB_MKRESCUE) -o $@ iso/

run: $(ISO_FILE)
	$(QEMU) -cdrom $(ISO_FILE) -m 128M -hda disk.img -boot order=dc

debug: $(ISO_FILE)
	$(QEMU) -cdrom $(ISO_FILE) -m 128M -hda disk.img -s -S

clean:
	rm -f $(C_OBJECTS) $(ASM_OBJECTS)
	rm -f $(KERNEL_ELF)
	rm -f pixel_neko*.iso
	rm -rf iso

rebuild: clean all

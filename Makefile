# =====================================================================
#  DiyorOS — Makefile
#
#  make        — kernelni yig'ish (build/diyoros.bin)
#  make run    — QEMU da ishga tushirish
#  make iso    — yuklanadigan ISO (GRUB kerak, ixtiyoriy)
#  make clean  — build papkasini tozalash
#
#  Mac da i686-elf-gcc (cross-compiler) ishlatiladi.
#  Linux da oddiy gcc -m32 bilan ham ishlaydi.
# =====================================================================

ifneq ($(shell command -v i686-elf-gcc 2>/dev/null),)
  CC       := i686-elf-gcc
  ARCH_C   :=
  ARCH_LD  :=
else
  CC       := gcc
  ARCH_C   := -m32 -fno-pie
  ARCH_LD  := -m32 -no-pie -Wl,--build-id=none
endif

QEMU     := qemu-system-i386
# Mac'da QEMU oynasi kichkina bo'lmasligi uchun: tasvirni oyna o'lchamiga cho'zish
ifeq ($(shell uname -s),Darwin)
  QEMU_DISPLAY := -display cocoa,zoom-to-fit=on
endif
BUILD    := build
KERNEL   := $(BUILD)/diyoros.bin
ISO      := $(BUILD)/diyoros.iso

CFLAGS   := $(ARCH_C) -std=gnu99 -ffreestanding -O2 -Wall -Wextra \
            -fno-stack-protector -fno-asynchronous-unwind-tables \
            -mgeneral-regs-only -Iinclude
LDFLAGS  := $(ARCH_LD) -T linker.ld -ffreestanding -nostdlib -O2

C_SRC    := $(wildcard src/*/*.c)
ASM_SRC  := $(wildcard src/*/*.s)
OBJS     := $(patsubst src/%.s,$(BUILD)/%.o,$(ASM_SRC)) \
            $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRC))

.PHONY: all run iso run-iso clean

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	@echo ">>> Tayyor: $@"

$(BUILD)/%.o: src/%.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_C) -c $< -o $@

run: $(KERNEL)
	$(QEMU) -kernel $(KERNEL) -m 128M -rtc base=localtime $(QEMU_DISPLAY)

iso: $(KERNEL)
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/diyoros.bin
	cp grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso 2>/dev/null || i686-elf-grub-mkrescue -o $(ISO) $(BUILD)/iso

run-iso: iso
	$(QEMU) -cdrom $(ISO) -m 128M -rtc base=localtime $(QEMU_DISPLAY)

clean:
	rm -rf $(BUILD)

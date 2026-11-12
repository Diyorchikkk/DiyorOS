# =====================================================================
#  DiyorOS — Makefile
#
#  make        — kernelni yig'ish (build/diyoros.bin)
#  make run    — QEMU da ishga tushirish
#  make iso    — yuklanadigan ISO (GRUB kerak, ixtiyoriy)
#  make clean  — build papkasini tozalash
#
#  v0.6: disk.img — 64 MB virtual qattiq disk (birinchi 'make run' da yaratiladi,
#  'make clean' uni o'chirmaydi: undagi fayllaringiz saqlanib qoladi).
#  Yangi bo'sh disk: make newdisk
#
#  v0.5: user/ dagi dasturlar alohida yig'iladi (0x40000000 manzilga, ring 3
#  uchun) va user/initrd.s orqali kernel ichiga joylanadi -> /bin.
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
DISK     := disk.img
DISK_MB  := 64
QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0

CFLAGS   := $(ARCH_C) -std=gnu99 -ffreestanding -O2 -Wall -Wextra \
            -fno-stack-protector -fno-asynchronous-unwind-tables \
            -mgeneral-regs-only -Iinclude
LDFLAGS  := $(ARCH_LD) -T linker.ld -ffreestanding -nostdlib -O2

C_SRC    := $(wildcard src/*/*.c)
ASM_SRC  := $(wildcard src/*/*.s)
OBJS     := $(patsubst src/%.s,$(BUILD)/%.o,$(ASM_SRC)) \
            $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRC)) \
            $(BUILD)/initrd.o

# ---- User dasturlar (v0.5) ----
USER_PROGS   := hello guess primes hack isolate kundalik
USER_BINS    := $(addprefix $(BUILD)/user/,$(USER_PROGS))
USER_LIB     := $(BUILD)/user/crt0.o $(BUILD)/user/libc.o
USER_CFLAGS  := $(ARCH_C) -std=gnu99 -ffreestanding -O2 -Wall -Wextra \
                -fno-stack-protector -fno-asynchronous-unwind-tables \
                -fno-builtin -Iinclude -Iuser
USER_LDFLAGS := $(ARCH_LD) -T user/linker.ld -ffreestanding -nostdlib -static

.PHONY: all run iso run-iso clean newdisk
.SECONDARY:

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

$(BUILD)/user/%.o: user/%.c user/libc.h include/syscall_nums.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/crt0.o: user/crt0.s
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_C) -c $< -o $@

$(BUILD)/user/%: $(BUILD)/user/%.o $(USER_LIB) user/linker.ld
	$(CC) $(USER_LDFLAGS) -o $@ $(USER_LIB) $<

$(BUILD)/initrd.o: user/initrd.s $(USER_BINS)
	$(CC) $(ARCH_C) -c $< -o $@

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1048576 count=$(DISK_MB)
	@echo ">>> Bo'sh disk yaratildi: $(DISK). DiyorOS ichida 'mkfs' bilan formatlang."

newdisk:
	rm -f $(DISK)
	$(MAKE) $(DISK)

run: $(KERNEL) $(DISK)
	$(QEMU) -kernel $(KERNEL) -m 128M -rtc base=localtime $(QEMU_DISK) $(QEMU_DISPLAY)

iso: $(KERNEL)
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/diyoros.bin
	cp grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso 2>/dev/null || i686-elf-grub-mkrescue -o $(ISO) $(BUILD)/iso

run-iso: iso $(DISK)
	$(QEMU) -cdrom $(ISO) -m 128M -rtc base=localtime $(QEMU_DISK) $(QEMU_DISPLAY)

clean:
	rm -rf $(BUILD)

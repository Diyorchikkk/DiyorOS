# DiyorOS v0.3

C va Assembly'da noldan yozilgan kichik operatsion tizim. Arxitektura: x86, 32-bit protected mode.
Linux, Windows yoki boshqa OS kodidan foydalanilmagan. Kompyuter yoqilgandan keyin ekranga chiqadigan
hamma narsani shu kod boshqaradi.

---

## 1. Mac'da ishga tushirish (M1/M2/M3)

Terminal'ni ochib, bir marta o'rnatasiz:

```bash
xcode-select --install          # make uchun (oldin o'rnatilgan bo'lsa, o'tkazib yuboring)
brew install i686-elf-gcc qemu  # x86 uchun cross-compiler va emulyator
```

Keyin loyiha papkasida:

```bash
make        # kernelni yig'adi -> build/diyoros.bin
make run    # QEMU oynasida OS ochiladi
```

`make` buyrug'i `i686-elf-gcc` topilmasa, Mac'dagi oddiy `gcc` (aslida clang) bilan yig'ishga
urinadi va xato beradi. Bunday holatda yuqoridagi `brew install` qadamini qayta bajaring.

**Nega cross-compiler kerak?** Mac'ning kompilyatori macOS uchun (ARM, Mach-O format) kod
chiqaradi. Bizga esa hech qanday OS'ga bog'lanmagan x86 ELF kod kerak. `i686-elf-gcc` aynan
shuni qiladi.

QEMU oynasiga bir marta bossangiz, sichqoncha va klaviatura OS'ga o'tadi. Shundan keyin trackpad
bilan ikki barmoqda varaqlash ishlaydi. Sichqonchani Mac'ga qaytarish: **Ctrl + Option + G**.

**Nega QEMU?** M3 — ARM protsessor, x86 kodni to'g'ridan-to'g'ri bajara olmaydi. QEMU x86
kompyuterni dasturiy jihatdan emulyatsiya qiladi.

---

## 2. Ustozga ko'rsatish uchun demo ketma-ketligi

`make run` qilgach, quyidagi buyruqlarni tartib bilan yozing:

| Buyruq | Nimani ko'rsatadi |
|---|---|
| (yuklanish ekrani) | `[ OK ]` qatorlari — har bir tizim qismi ketma-ket ishga tushadi |
| `help` | Shell va buyruqlar jadvali |
| `about` | OS haqida ma'lumot |
| `mem` | Bootloader'dan olingan RAM hajmi va kernelning xotiradagi joyi |
| `time` | CMOS soatidan sana va vaqt — apparat bilan to'g'ridan-to'g'ri ishlash |
| `uptime` | Taymer uzilishlari soni (tick). Yuqoridagi status bar ham har soniyada o'zi yangilanadi |
| `calc 12 * 7` | Oddiy dastur |
| `calc 5 / 0` | Xatoni dastur o'zi tekshiradi |
| ↑ strelka | Buyruqlar tarixi |
| Sichqoncha g'ildiragi / trackpad, Shift+↑↓, Fn+↑↓ | Ekran tarixini varaqlash (scrollback, 500 qator) |
| `color 14` | VGA ranglari |
| `sleep 3` | Taymer asosida kutish |
| `crash` | **Eng muhimi:** protsessor exception'ini ushlash va *kernel panic* ekrani |
| `shutdown` | Tizimni o'chirish (QEMU oynasi yopiladi) |

---

## 3. Loyiha tuzilishi

```
DiyorOS/
├── Makefile              yig'ish qoidalari
├── linker.ld             kernelning xotirada joylashuvi (1 MB dan)
├── grub.cfg              haqiqiy kompyuterda yuklash uchun (keyinroq)
├── include/              sarlavha (.h) fayllar
└── src/
    ├── boot/
    │   ├── boot.s        Multiboot header, stek, kernel_main ga o'tish
    │   └── interrupts.s  GDT/IDT yuklash, 48 ta uzilish "stub"i
    ├── kernel/
    │   ├── kernel.c      asosiy funksiya: hamma qismni tartib bilan ishga tushiradi
    │   ├── task.c        vazifalar va scheduler (v0.3)
    │   ├── gdt.c         segmentlar jadvali (ring 0 / ring 3)
    │   └── idt.c         uzilishlar jadvali, PIC, kernel panic
    ├── drivers/
    │   ├── vga.c         ekran (0xB8000 xotirasi) va scrollback
    │   ├── timer.c       PIT taymer, IRQ0
    │   ├── keyboard.c    PS/2 klaviatura, IRQ1, ring buffer
    │   ├── mouse.c       PS/2 sichqoncha g'ildiragi, IRQ12
    │   └── rtc.c         CMOS real vaqt soati
    ├── mm/               xotira (v0.2) — batafsil: docs/v0.2-xotira.md
    │   ├── pmm.c         fizik xotira menejeri (bitmap)
    │   ├── paging.c      virtual xotira, page directory/table
    │   └── kheap.c       kmalloc / kfree
    ├── lib/
    │   ├── string.c      memset, memcpy, strcmp... (libc yo'q)
    │   └── kprintf.c     printf'ning kernel varianti
    └── shell/
        ├── shell.c       buyruqlar qatori
        ├── cmd_memory.c  xotira buyruqlari (mem, alloc, heap, memtest...)
        └── cmd_process.c vazifa buyruqlari (ps, spawn, kill, sched)
```

---

## 4. Himoyada tushuntirish: OS qanday yuklanadi

1. **BIOS** kompyuterni tekshiradi va bootloader'ni ishga tushiradi.
2. **Bootloader** (QEMU yoki GRUB) kernel faylidan Multiboot header'ni (sehrli raqam `0x1BADB002`)
   topadi. So'ng kernelni 1 MB manzilga yuklaydi, protsessorni 32-bit protected mode'ga o'tkazadi
   va `_start` ga sakraydi.
3. **boot.s** stekni o'rnatadi va C'dagi `kernel_main` funksiyasini chaqiradi.
4. **kernel_main** quyidagilarni tartib bilan bajaradi:
   - **VGA** — 0xB8000 manzilga yozilgan har bir belgi darhol ekranda chiqadi.
   - **GDT** — xotira segmentlari va ruxsat darajalari: ring 0 kernel uchun, ring 3 dasturlar uchun.
   - **IDT** — 256 ta uzilish raqamining har biri uchun qaysi funksiya chaqirilishini belgilaydi.
   - **PIC remap** — qurilma uzilishlarini (IRQ) 32–47 raqamlarga ko'chiradi, chunki 0–31 raqamlar
     protsessor exception'lari uchun band.
   - **Taymer** — sekundiga 100 marta uzilish keladi. Bu tizim soati, kelajakda esa scheduler asosi.
   - **Klaviatura** — tugma bosilganda uzilish keladi, scancode harfga aylantiriladi.
   - **`sti`** — uzilishlar yoqiladi.
5. **Shell** buyruqlarni o'qiydi, tahlil qiladi va bajaradi.

### Uzilish qanday ishlaydi (masalan, tugma bosilganda)

```
Klaviatura → PIC → protsessor → IDT[33] → isr33 (asm) → isr_common (registrlarni saqlaydi)
→ interrupt_handler (C) → keyboard_handler → belgini buferga yozadi
→ PIC'ga EOI ("tugatdim") yuboriladi → iret (oldingi ishga qaytish)
```

Shell esa `keyboard_getchar()` orqali bufferdan o'qiydi. Bu **producer–consumer** modeli:
uzilish ma'lumotni yozadi, shell uni o'qiydi. Bufer bo'sh bo'lsa, protsessor `hlt` buyrug'i bilan
keyingi uzilishgacha uxlaydi va 100% band bo'lmaydi.

### Ehtimoliy savollar

- **Nega libc (printf, malloc) ishlatmadingiz?** Ular OS ustida ishlaydi. Bizda esa hali OS yo'q,
  shuning uchun `string.c` va `kprintf.c` ni o'zimiz yozdik.
- **Exception va IRQ farqi?** Exception protsessorning o'zidan keladi, masalan 0 ga bo'lishda.
  IRQ esa tashqi qurilmadan keladi.
- **`crash` buyrug'ida nima bo'ladi?** `idiv` buyrug'i 0 ga bo'ladi. Protsessor 0-exception'ni
  chiqaradi, IDT[0] orqali `kernel_panic` chaqiriladi va u registrlarni ekranga chiqaradi.
- **Ekranni varaqlash qanday ishlaydi?** VGA xotirasiga faqat 25 qator sig'adi. Ekrandan yuqoriga
  chiqib ketgan qator 500 qatorlik aylanma buferga (`scrollback`) saqlanadi. Varaqlanganda ekran
  shu buferdan qayta chiziladi. Sichqoncha g'ildiragi IRQ12 orqali keladi va drayver uni
  klaviatura buferiga "varaqlash tugmasi" sifatida qo'shadi.
- **Status bar qanday o'zi yangilanadi?** Taymer uzilishi har 100-tick'da (1 soniya)
  `status_bar_update` funksiyasini chaqiradi. Bu shell bilan bir vaqtda ishlaydi.

---

## 5. Keyingi bosqichlar (diplom ishigacha)

| Bosqich | Nima qo'shiladi | OS nazariyasidagi mavzu |
|---|---|---|
| **v0.1** ✅ | boot, GDT, IDT, taymer, klaviatura, shell | Uzilishlar, drayverlar |
| **v0.2** ✅ | Fizik xotira menejeri (bitmap), **paging**, `kmalloc`/`kfree` — [batafsil](docs/v0.2-xotira.md) | Xotirani boshqarish, virtual xotira |
| **v0.3** ✅ | **Multitasking**: kontekst almashtirish, Round-Robin scheduler, `ps`, `kill`, `spawn` — [batafsil](docs/v0.3-jarayonlar.md) | Jarayonlar, rejalashtirish |
| v0.4 | RAM'dagi fayl tizimi: `ls`, `cat`, `touch`, `mkdir` | Fayl tizimlari |
| v0.5 | User mode (ring 3), **syscall**'lar, ELF dasturlarni yuklash | Himoya, tizim chaqiruvlari |
| v0.6 | Disk drayveri (ATA), FAT12/FAT32 o'qish | Kiritish-chiqarish, disklar |
| Diplom | Semaforlar/mutex, oddiy grafik rejim, tarmoq yoki ARM64'ga port qilish | Sinxronizatsiya va boshqalar |

---

## 6. Haqiqiy kompyuterda yuklash (ixtiyoriy, keyinroq)

`make iso` buyrug'i GRUB orqali yuklanadigan `build/diyoros.iso` faylini yaratadi. Buning uchun
`grub-mkrescue` va `xorriso` kerak; ularni Linux'da o'rnatish osonroq. ISO'ni USB'ga yozib,
eski x86 noutbukda (BIOS/Legacy rejimida) yuklash mumkin. Mac'ning o'zida bu OS yuklanmaydi,
chunki Mac ARM protsessorli.

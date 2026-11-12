# DiyorOS v0.6.1

C va Assembly'da noldan yozilgan operatsion tizim. Arxitektura: x86, 32-bit protected mode.
Linux, Windows yoki boshqa OS kodidan foydalanilmagan: kompyuter yoqilgandan keyin ekranga chiqadigan
hamma narsani shu kod boshqaradi.

Nimalari bor:

- **Yuklanish**: Multiboot (QEMU `-kernel` yoki GRUB ISO), GDT, IDT, PIC, TSS
- **Xotira**: fizik xotira menejeri (bitmap), paging, har bir jarayonga alohida manzil maydoni, kernel heap
- **Jarayonlar**: preemptive Round-Robin scheduler, kernel oqimlari, ring 3 dagi dasturlar (ELF), syscall'lar
- **Fayl tizimlari**: VFS, ramfs, procfs (`/proc`), FAT32 (diskda, uzun nomlar bilan)
- **Drayverlar**: VGA matn rejimi, PS/2 klaviatura va sichqoncha, PIT taymer, CMOS soati, ATA disk, serial port
- **Ishonchlilik**: kernel panic (backtrace bilan), kernel jurnali (`dmesg`), stek va heap qo'riqchilari,
  faqat o'qiladigan kernel kodi, Double Fault uchun alohida stek, FPU kontekstini saqlash
- **Shell**: 45 ta buyruq, qator tahriri, tarix, Tab bilan to'ldirish, matn muharriri

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
make run    # QEMU oynasida OS ochiladi (birinchi marta disk.img ham yaratiladi)
```

Birinchi ishga tushirishda disk bo'sh bo'ladi: DiyorOS ichida `mkfs` deb yozib, `ha` bilan tasdiqlang.
Shundan keyin `/disk` dagi fayllar o'chirib-yoqqandan keyin ham saqlanadi.

`make run` paytida DiyorOS ekraniga chiqqan matn va kernel jurnali **Mac terminalida ham** ko'rinadi
(serial konsol). Kernel panic bo'lsa ham xabar terminalda qoladi — undan nusxa olish oson.

| Buyruq | Vazifasi |
|---|---|
| `make` | kernelni yig'ish |
| `make run` | QEMU'da ishga tushirish |
| `make debug` | QEMU'ni GDB kutish rejimida ishga tushirish (`i686-elf-gdb build/diyoros.bin -ex "target remote :1234"`) |
| `make iso` | GRUB orqali yuklanadigan `build/diyoros.iso` (UTM yoki haqiqiy kompyuter uchun) |
| `make newdisk` | yangi bo'sh `disk.img` |
| `make clean` | `build/` ni tozalash (`disk.img` saqlanadi) |

**Nega cross-compiler kerak?** Mac'ning kompilyatori macOS uchun (ARM, Mach-O format) kod
chiqaradi. Bizga esa hech qanday OS'ga bog'lanmagan x86 ELF kod kerak. `i686-elf-gcc` aynan
shuni qiladi.

**Nega QEMU?** M3 — ARM protsessor, x86 kodni to'g'ridan-to'g'ri bajara olmaydi. QEMU x86
kompyuterni dasturiy jihatdan emulyatsiya qiladi.

QEMU oynasiga bir marta bossangiz, sichqoncha va klaviatura OS'ga o'tadi. Sichqonchani Mac'ga
qaytarish: **Ctrl + Option + G**.

---

## 2. Ustozga ko'rsatish uchun demo ketma-ketligi

| Buyruq | Nimani ko'rsatadi |
|---|---|
| (yuklanish ekrani) | `[ OK ]` qatorlari — har bir tizim qismi ketma-ket ishga tushadi |
| `help` | Shell va buyruqlar jadvali |
| `mem`, `memmap` | Fizik xotira, paging, heap |
| `ps`, `spawn spinner`, `spawn busy` | Multitasking: vazifalar parallel ishlaydi, preemption |
| `ls /`, `cat /proc/meminfo`, `cat /proc/interrupts` | VFS, virtual fayllar |
| `edit /disk/salom.txt` | Matn muharriri, diskka yozish (qayta yoqqandan keyin ham turadi) |
| `hello`, `guess` | Ring 3 dagi dasturlar, syscall'lar |
| `hack 1` ... `hack 8` | Himoya: dastur kernelga tega olmaydi, faqat o'zi o'ladi |
| `pi &`, `pi &`, `pi` | Uchta dastur bir vaqtda suzuvchi nuqtali hisob qiladi (FPU konteksti) |
| `dmesg` | Kernel jurnali |
| `pagefault kod` | Kernel o'z kodiga yoza olmaydi (faqat o'qish uchun) |
| `crash stack` | Stek to'lib ketishi → Double Fault → toza *kernel panic* (qayta yuklanib ketmaydi) |
| `crash heap` | Buzilgan heap aniqlanadi |

Har bir bosqichning batafsil demo ketma-ketligi va himoya savollari `docs/` papkasida.

**Shell'da qulayliklar:** ←/→ bilan qator ichida yurish, Home/End, Delete, Ctrl+A/E/K/U, Ctrl+L,
↑/↓ — tarix (32 ta), Tab — to'ldirish, Ctrl+C — dasturni to'xtatish, sichqoncha g'ildiragi yoki
Fn+↑/↓ — ekran tarixini varaqlash (500 qator). Bo'sh joyli nomlar: `"Mening faylim.txt"` yoki `Mening\ faylim.txt`.

---

## 3. Loyiha tuzilishi

```
DiyorOS/
├── Makefile              yig'ish qoidalari
├── linker.ld             kernelning xotirada joylashuvi (1 MB dan; kod va ma'lumot alohida)
├── grub.cfg              ISO uchun GRUB sozlamasi
├── include/              sarlavha (.h) fayllar
├── docs/                 har bir bosqich bo'yicha tushuntirish va himoya savollari
├── src/
│   ├── boot/
│   │   ├── boot.s        Multiboot header, stek (qo'riqchi sahifa bilan), kernel_main ga o'tish
│   │   └── interrupts.s  GDT/IDT yuklash, uzilish "stub"lari, kontekst almashtirish
│   ├── kernel/
│   │   ├── kernel.c      asosiy funksiya: hamma qismni tartib bilan ishga tushiradi
│   │   ├── gdt.c         segmentlar (ring 0 / ring 3), TSS, Double Fault TSS
│   │   ├── idt.c         uzilishlar jadvali, PIC, soxta IRQ, exception'larni taqsimlash
│   │   ├── task.c        vazifalar, scheduler, stek qo'riqchisi, FPU konteksti
│   │   ├── process.c     ELF yuklovchi, dastur xatolarini ushlash
│   │   ├── syscall.c     tizim chaqiruvlari, int 0x80
│   │   ├── panic.c       kernel panic, backtrace, Double Fault (v0.6.1)
│   │   └── klog.c        kernel jurnali — dmesg (v0.6.1)
│   ├── drivers/
│   │   ├── vga.c         ekran (0xB8000), scrollback, kursor
│   │   ├── serial.c      serial port COM1 — konsol nusxasi (v0.6.1)
│   │   ├── timer.c       PIT taymer, IRQ0
│   │   ├── keyboard.c    PS/2 klaviatura, IRQ1, ring buffer
│   │   ├── mouse.c       PS/2 sichqoncha g'ildiragi, IRQ12
│   │   ├── ata.c         ATA (IDE) qattiq disk, PIO
│   │   └── rtc.c         CMOS real vaqt soati
│   ├── mm/
│   │   ├── pmm.c         fizik xotira menejeri (bitmap)
│   │   ├── paging.c      virtual xotira, jarayonlarning sahifa jadvallari
│   │   └── kheap.c       kmalloc / kfree
│   ├── fs/
│   │   ├── vfs.c         yo'llar, fayl deskriptorlari, mount
│   │   ├── ramfs.c       xotiradagi fayl tizimi
│   │   ├── procfs.c      /proc virtual fayllari
│   │   ├── fat32.c       FAT32: diskdagi fayl tizimi
│   │   └── rootfs.c      yuklanishda papka va fayllarni yaratish
│   ├── lib/
│   │   ├── string.c      memset, memcpy, strcmp... (libc yo'q)
│   │   └── kprintf.c     printf'ning kernel varianti
│   └── shell/
│       ├── shell.c       buyruqlar qatori, qator tahriri, Tab
│       ├── cmd_memory.c  mem, alloc, heap, memtest, pagefault...
│       ├── cmd_process.c ps, spawn, kill, sched
│       ├── cmd_fs.c      ls, cd, cat, cp, mv, rm, tree...
│       ├── cmd_disk.c    disk, mkfs, mount, umount
│       └── editor.c      matn muharriri (edit)
└── user/                 dasturlar — ring 3 da ishlaydi, /bin ga tushadi
    ├── crt0.s            dastur boshlanishi: main(argc, argv)
    ├── libc.c            printf, malloc, readline, syscall o'ramlari
    ├── linker.ld         dasturlar 0x40000000 dan joylashadi
    ├── initrd.s          dasturlarni kernel ichiga joylash
    └── hello.c guess.c primes.c hack.c isolate.c kundalik.c pi.c
```

---

## 4. Himoyada tushuntirish: OS qanday yuklanadi

1. **BIOS** kompyuterni tekshiradi va bootloader'ni ishga tushiradi.
2. **Bootloader** (QEMU yoki GRUB) kernel faylidan Multiboot header'ni (sehrli raqam `0x1BADB002`)
   topadi. So'ng kernelni 1 MB manzilga yuklaydi, protsessorni 32-bit protected mode'ga o'tkazadi
   va `_start` ga sakraydi.
3. **boot.s** stekni o'rnatadi va C'dagi `kernel_main` funksiyasini chaqiradi.
4. **kernel_main** quyidagilarni tartib bilan bajaradi (har biri oldingisiga tayanadi):
   - **Serial port, VGA** — xabar chiqara olish.
   - **GDT, TSS** — segmentlar va ruxsat darajalari: ring 0 kernel uchun, ring 3 dasturlar uchun.
   - **IDT, PIC** — 256 ta uzilish raqamining har biri uchun qaysi funksiya chaqirilishi.
   - **PMM → paging → heap** — xotiraning uch qatlami.
   - **FPU, ATA disk, fayl tizimlari** — `/`, `/proc`, `/bin`, `/disk`.
   - **Taymer, klaviatura, sichqoncha**, so'ng **`sti`** — uzilishlar yoqiladi.
   - **Scheduler** — shu oqim 1-vazifaga (shell) aylanadi, idle vazifasi yaratiladi.
5. **Shell** buyruqlarni o'qiydi, tahlil qiladi va bajaradi.

### Uzilish qanday ishlaydi (masalan, tugma bosilganda)

```
Klaviatura → PIC → protsessor → IDT[33] → isr33 (asm) → isr_common (registrlarni saqlaydi)
→ interrupt_handler (C) → keyboard_handler → belgini buferga yozadi
→ PIC'ga EOI ("tugatdim") yuboriladi → scheduler: boshqa vazifaga o'tish kerakmi? → iret
```

Shell `keyboard_getchar()` orqali bufferdan o'qiydi. Bu **producer–consumer** modeli: uzilish
ma'lumotni yozadi, shell uni o'qiydi. Bufer bo'sh bo'lsa, shell BLOCKED holatga o'tadi va
protsessor boshqa vazifalarga beriladi.

### Umumiy savollar

- **Nega libc (printf, malloc) ishlatmadingiz?** Ular OS ustida ishlaydi. Kernel ichida OS yo'q,
  shuning uchun `string.c` va `kprintf.c` ni o'zimiz yozdik. Dasturlar uchun esa alohida kichik
  libc bor (`user/libc.c`) — u syscall'lar orqali kernelga murojaat qiladi.
- **Exception va IRQ farqi?** Exception protsessorning o'zidan keladi (0 ga bo'lish, Page Fault).
  IRQ tashqi qurilmadan keladi (taymer, klaviatura).
- **Kernel xatosi va dastur xatosi farqi?** Dastur (ring 3) xato qilsa, faqat o'sha jarayon
  o'ldiriladi ("Segmentation fault"). Kernelning o'zi xato qilsa — *kernel panic*: tizim to'xtaydi,
  chunki buzilgan kernel bilan davom etish ma'lumotlarni yo'qotishi mumkin.

Har bir bosqichga oid savollar: `docs/v0.2-xotira.md` ... `docs/v0.6.1-mustahkamlash.md`.

---

## 5. Bosqichlar

| Bosqich | Nima qo'shildi | OS nazariyasidagi mavzu |
|---|---|---|
| **v0.1** ✅ | boot, GDT, IDT, taymer, klaviatura, shell | Uzilishlar, drayverlar |
| **v0.2** ✅ | Fizik xotira menejeri, **paging**, `kmalloc`/`kfree` — [batafsil](docs/v0.2-xotira.md) | Xotirani boshqarish |
| **v0.3** ✅ | **Multitasking**: kontekst almashtirish, Round-Robin — [batafsil](docs/v0.3-jarayonlar.md) | Jarayonlar, rejalashtirish |
| **v0.4** ✅ | VFS, ramfs, procfs, matn muharriri — [batafsil](docs/v0.4-fayl-tizimi.md) | Fayl tizimlari |
| **v0.5** ✅ | User mode (ring 3), syscall, ELF, libc — [batafsil](docs/v0.5-user-mode.md) | Himoya, tizim chaqiruvlari |
| **v0.6** ✅ | ATA disk, FAT32, `/disk` — [batafsil](docs/v0.6-disk-fat32.md) | Kiritish-chiqarish, disklar |
| **v0.6.1** ✅ | Kod tekshiruvi: 20+ xato tuzatildi; panic, jurnal, serial, FPU, himoyalar — [batafsil](docs/v0.6.1-mustahkamlash.md) | Ishonchlilik, race condition |
| v0.7 | Grafik rejim (VESA), shriftlar, sichqoncha kursori, oynalar | Grafik quyi tizim |
| v0.8 | Mutex, semafor, pipe, `exec`/`wait` | Sinxronizatsiya, IPC |
| v1.0 | Haqiqiy kompyuterda yuklash, testlar, diplom matni | — |

---

## 6. UTM yoki haqiqiy kompyuterda yuklash

`make iso` buyrug'i GRUB orqali yuklanadigan `build/diyoros.iso` faylini yaratadi (`grub-mkrescue`
va `xorriso` kerak; Linux'da o'rnatish osonroq).

- **UTM**: Emulate → Other, arxitektura x86_64, tizim *Standard PC (i440FX)*, 256 MB RAM, displey VGA,
  IDE disk (CD dan yuqorida), CD sifatida `diyoros.iso`. Birinchi yuklanishda `mkfs`.
- **Haqiqiy kompyuter**: ISO'ni USB'ga yozib, eski x86 noutbukda BIOS/Legacy rejimida yuklash mumkin.
  Mac'ning o'zida yuklanmaydi (ARM protsessor).

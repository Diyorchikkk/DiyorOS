# Ustozga ko'rsatish: 10 daqiqalik reja (shpargalka)

Tayyorgarlik: `make clean && make run`. QEMU oynasiga bir marta bosing. Hammasi matn rejimida
ko'rsatiladi; grafik rejim — oxirida. Adashsangiz: `tozala`, dastur qotsa: **Ctrl+C**.

| № | Buyruq | Nima deyish kerak (1–2 gap) |
|---|---|---|
| 1 | (yuklanish ekrani) | "Bu o'zim yozgan OS: C va assembler, 22 ming qator. Har bir `[ OK ]` — alohida qism: xotira, uzilishlar, drayverlar, fayl tizimi, scheduler." |
| 2 | `jarayonlar`, `yurgiz busy`, `jarayonlar`, `toxtat 2` | "Multitasking preemptive: `busy` cheksiz tsiklda, lekin shell darhol javob beradi — taymer vazifani majburan almashtiradi, MLFQ esa ko'p uxlaydigan shell ni yuqori navbatda ushlaydi." |
| 3 | `hello` | "Bu dastur ring 3 da, o'z manzil fazosida ishlaydi. Kernel bilan faqat `int 0x80` syscall orqali gaplashadi." |
| 4 | `hack 2`, `hack 5` | "Dastur kernel xotirasiga yozmoqchi bo'ldi, keyin uzilishlarni o'chirmoqchi (`cli`) — protsessor ruxsat bermadi, kernel faqat shu dasturni o'ldirdi. Tizim ishlayapti." |
| 5 | `sinx poyga` | "Uch oqim bitta o'zgaruvchini oshiradi. Qulfsiz — o'n minglab qiymat yo'qoladi (race condition). Mutex bilan — aniq 90 000." |
| 6 | `sinx ishlab` | "Producer–consumer: ikkita semafor joy va mahsulotni sanaydi, mutex buferni himoya qiladi. Bufer to'lsa, ishlab chiqaruvchi uxlaydi." |
| 7 | `sinx deadlock` | "Ikki oqim ikki qulfni teskari tartibda oladi. Kernel kutish grafida aylanani topadi va xato qaytaradi; oqim qulfini qaytarib, qayta urinadi." |
| 8 | `sinx faylasuflar`, keyin `sinx faylasuflar tartib` | "Dijkstra masalasi: 5 tali aylana. Ikkinchi variantda resurslar tartiblangan — deadlock umuman bo'lmaydi (0 ta)." |
| 9 | `royxat /bin \| grep o \| wc` | "Kanal: har bir buyruqning chiqishi keyingisining kirishiga. Orada kerneldagi 4 KB bufer — bu ham producer–consumer." |
| 10 | `sanoq 1000000 \| head 3` | "`head` 3 qatordan keyin tugadi — `sanoq` million sonni hisoblab o'tirmadi: yozish xato qaytardi (broken pipe)." |
| 11 | `sanoq 100000000 \| grep 7 \| wc`, 2 soniyadan keyin **Ctrl+C**, `jarayonlar` | "Uchta dastur parallel ishlayapti. Ctrl+C butun guruhni to'xtatdi — `jarayonlar` da hech narsa qolmadi." |
| 12 | `sanoq 3000 > /disk/son.txt`, `wc < /disk/son.txt` | "Chiqish diskdagi faylga (FAT32) yo'naltirildi. Dastur buni bilmaydi — u oddiy `write(1)` qiladi." |
| 13 | `korsat /proc/sync` | "Hamma qulflarning statistikasi: necha marta olingan, necha marta kutilgan, nechta deadlock aniqlangan." |
| 14 | `nusxa` | "`fork` jarayonni ikkiga bo'ladi. 1 MB massiv fork da nusxalanmadi (16 KB ketdi) — sahifalar faqat bola yozganda ko'chirildi: copy-on-write. Oxirida bola `exec` bilan boshqa dasturga aylandi." |
| 15 | `qobiq`, `primes 100 \| wc`, `kir /bin`, `chiq`, `qayerda` | "Bu shell — oddiy dastur (ring 3). Buyruqlarni Unix kabi ishga tushiradi: fork, dup2, exec, wait. Uning `kir` si kernel shell papkasini o'zgartirmaydi — papka har jarayonniki." |
| 16 | `tekshir` | "Avtomatik testlar: xotira, copy-on-write, fayllar, kanallar, qulflar, scheduler, disk va 33 ta syscall testi — 75 ta tekshiruv 1 soniyada. Har o'zgarishdan keyin `make test`." |
| 17 | `reja ulush`, keyin `reja` | "To'rtta rejalashtirish algoritmi. Qat'iy ustuvorlikda past vazifalar 0% oldi — och qolish. Lotereya chiptalarga mos bo'ladi. Standart — MLFQ: o'lchovlarimizda u interaktiv dasturga RR dan 15–30 baravar tez javob berdi." |
| 18 | `grafik` | "Grafik rejim: oyna menejeri, sichqoncha, Fayllar, Tizim monitori, Mandelbrot (ring 3 da, FPU bilan)." |
| 19 | `nosozlik stack` (eng oxirida!) | "Kernelning o'zida xato bo'lsa — toza panic ekrani: sabab, registrlar, backtrace. Tizim jim qotib qolmaydi." |

Vaqt kam bo'lsa: 1, 2, 4, 5, 9, 11, 14, 15, 16, 17, 18.

Diplom himoyasi uchun grafiklar: `docs/grafik/*.png`, tahlil: `docs/v0.11-tadqiqot.md`.

2-qadamdagi `toxtat 2`: `busy` ning PID ini `jarayonlar` dan qarang (odatda 2).

## Eng ehtimoliy 5 savol

**1. Mutex va semafor farqi nima?**
Mutexning egasi bor: faqat olgan vazifa ochadi, u o'zaro istisno uchun. Semafor — hisoblagich,
egasi yo'q: bir vazifa `down`, boshqasi `up` qilishi mumkin; u resurslarni sanash va hodisani
kutish uchun. Producer–consumer'da ikkalasi kerak.

**2. Deadlock uchun qanday shartlar kerak va siz nima qildingiz?**
To'rtta (Coffman): o'zaro istisno, ushlab turib kutish, tortib olinmaslik, aylanma kutish.
Menda ikki usul: **aniqlash** — `mutex_lock` "kim kimni kutyapti" zanjirida aylanani topsa,
uxlamasdan xato qaytaradi; **oldini olish** — resurslarni tartiblash (`faylasuflar tartib`).

**3. Nega uzilishlarni o'chirib qo'ya qolmadingiz?**
v0.6 da shunday edi. Lekin u paytda taymer, klaviatura, sichqoncha kutadi (diskka yozishda
tizim qotardi), uxlab kutib bo'lmaydi va ko'p protsessorda ishlamaydi. Endi uzoq seksiyalar
mutex bilan; uzilishlar faqat mutexning o'zini qurishda, bir necha buyruqqa o'chiriladi.

**4. `royxat | wc` ichida nima bo'ladi?**
Shell kanal yaratadi (4 KB bufer, ikki uch), `wc` ni ishga tushirib stdin'ini o'qish uchiga
ulaydi, `royxat` chiqishini yozish uchiga yozadi va o'z uchlarini **yopadi**. Yozuvchi qolmagach
`wc` `read` dan 0 oladi (EOF) va natijani chiqaradi. Bufer to'lsa yozuvchi, bo'sh bo'lsa
o'quvchi uxlaydi.

**5. `fork` qanday ishlaydi va nega u tez? Zombi nima?**
Bolaning kernel stekiga otaning syscall kadri ko'chiriladi (faqat `EAX = 0`) — shuning uchun
fork ikki marta qaytadi. Xotira nusxalanmaydi: ikkala jarayon bir xil frame larni "faqat o'qish"
holatida oladi, kimdir yozsa Page Fault keladi va kernel faqat o'sha sahifani ko'chiradi
(copy-on-write). Zombi — tugagan, lekin chiqish kodi hali o'qilmagan jarayon; otasi `wait()`
qilganda tozalanadi.

## Halol javoblar ("bu yo'q" deyishdan qo'rqmang)

- Bitta protsessor; SMP yo'q. O'lchovlar emulyatorda (haqiqiy kompyuterda hali o'lchanmagan).
- Deadlock faqat mutexlar orasida aniqlanadi (kanal yoki semafor orqali emas).
- Signal ishlov beruvchilari, swap, tarmoq, USB — yo'q.
- Haqiqiy kompyuterda hali sinalmagan — faqat QEMU (bu v1.0 rejasida).

Batafsil savol-javoblar: har bir `docs/v0.X-*.md` faylining oxirida.

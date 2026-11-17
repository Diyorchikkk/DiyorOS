/*
 * pipe.c — kanal (pipe), v0.8.
 *
 * "ls | wc" da ls ning chiqishi to'g'ridan-to'g'ri wc ning kirishiga ulanadi.
 * Kanal — kernel xotirasidagi 4 KB aylanma bufer va ikkita uch:
 *     yozish uchi (ls ning stdout)  --->  [ bufer ]  --->  o'qish uchi (wc ning stdin)
 *
 * Bu klassik PRODUCER-CONSUMER masalasi:
 *   - bufer bo'sh bo'lsa, o'quvchi uxlaydi (rq navbatida) — yozuvchi uyg'otadi;
 *   - bufer to'la bo'lsa, yozuvchi uxlaydi (wq navbatida) — o'quvchi uyg'otadi.
 * Shu tufayli tez yozadigan dastur sekin o'qiydiganini "bosib" ketmaydi va
 * xotira cheksiz o'smaydi (backpressure).
 *
 * Tugash qoidalari (Unix dagidek):
 *   - barcha yozish uchlari yopilsa va bufer bo'sh bo'lsa — read 0 qaytaradi (EOF);
 *     wc shundan "ma'lumot tugadi" deb biladi;
 *   - barcha o'qish uchlari yopilsa — write EPIPE qaytaradi (Unix da SIGPIPE):
 *     masalan "ota | grep x" da grep tugasa, ota bekorga yozib o'tirmaydi.
 *
 * Holatni o'zgartirish va uxlashga o'tish bo'linmas bo'lishi kerak (lost
 * wakeup bo'lmasin), shuning uchun ichki amallar qisqa irq_save ostida.
 * Kutish "uzilishli" — Ctrl+C bilan to'xtatilgan jarayon kanalda qotib qolmaydi.
 */
#include "pipe.h"
#include "vfs.h"
#include "memory.h"
#include "irq.h"
#include "string.h"
#include "kprintf.h"

static pipe_t  *all_pipes = 0;
static uint32_t next_id = 1;

pipe_t *pipe_create(void)
{
    pipe_t *p = kmalloc(sizeof(pipe_t));
    if (!p)
        return 0;
    memset(p, 0, sizeof(*p));
    p->readers = p->writers = 1;
    wq_init(&p->rq);
    wq_init(&p->wq);
    uint32_t f = irq_save();
    p->id = next_id++;
    p->next = all_pipes;
    all_pipes = p;
    irq_restore(f);
    return p;
}

int32_t pipe_read(pipe_t *p, void *buf, uint32_t len)
{
    if (len == 0)
        return 0;
    uint32_t f = irq_save();
    while (p->count == 0) {
        if (p->writers == 0) {               /* yozadigan hech kim qolmagan: fayl oxiri */
            irq_restore(f);
            return 0;
        }
        p->blocked_reads++;
        if (wq_wait(&p->rq, "pipe o'qish", true) == SYNC_EINTR) {
            irq_restore(f);
            return VFS_EINTR;
        }
    }
    uint32_t n = len < p->count ? len : p->count;
    uint8_t *dst = buf;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_SIZE;
    }
    p->count -= n;
    wq_wake_all(&p->wq);                     /* joy bo'shadi */
    irq_restore(f);
    return (int32_t)n;
}

int32_t pipe_write(pipe_t *p, const void *buf, uint32_t len)
{
    const uint8_t *src = buf;
    uint32_t done = 0;
    uint32_t f = irq_save();
    while (done < len) {
        if (p->readers == 0) {               /* o'qiydigan hech kim yo'q */
            irq_restore(f);
            return VFS_EPIPE;
        }
        if (p->count == PIPE_SIZE) {         /* bufer to'la — o'quvchini kutamiz */
            p->blocked_writes++;
            if (wq_wait(&p->wq, "pipe yozish", true) == SYNC_EINTR) {
                irq_restore(f);
                return done ? (int32_t)done : VFS_EINTR;
            }
            continue;
        }
        uint32_t tail = (p->head + p->count) % PIPE_SIZE;
        while (done < len && p->count < PIPE_SIZE) {
            p->buf[tail] = src[done++];
            tail = (tail + 1) % PIPE_SIZE;
            p->count++;
            p->total++;
        }
        wq_wake_all(&p->rq);                 /* ma'lumot keldi */
    }
    irq_restore(f);
    return (int32_t)done;
}

void pipe_close(pipe_t *p, bool write_end)
{
    uint32_t f = irq_save();
    if (write_end) {
        if (--p->writers == 0)
            wq_wake_all(&p->rq);             /* o'quvchilar EOF ni ko'rsin */
    } else {
        if (--p->readers == 0)
            wq_wake_all(&p->wq);             /* yozuvchilar EPIPE ni ko'rsin */
    }
    bool dead = p->readers == 0 && p->writers == 0;
    if (dead) {
        pipe_t **pp = &all_pipes;
        while (*pp && *pp != p)
            pp = &(*pp)->next;
        if (*pp)
            *pp = p->next;
    }
    irq_restore(f);
    if (dead)
        kfree(p);
}

int pipe_report(char *b, uint32_t size)
{
    uint32_t f = irq_save();
    int n = ksnprintf(b, size, "KANALLAR (pipe)\n%-4s %8s %6s %6s %9s %8s %8s\n",
                      "ID", "BUFERDA", "O'QUV", "YOZUV", "JAMI", "O'Q.KUT", "YOZ.KUT");
    int count = 0;
    for (pipe_t *p = all_pipes; p && n < (int)size - 80; p = p->next, count++)
        n += ksnprintf(b + n, size - n, "%-4u %8u %6d %6d %9u %8u %8u\n", p->id, p->count,
                       p->readers, p->writers, p->total, p->blocked_reads, p->blocked_writes);
    if (!count)
        n += ksnprintf(b + n, size - n, "(ochiq kanal yo'q)\n");
    irq_restore(f);
    return n;
}

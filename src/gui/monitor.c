/*
 * monitor.c — "Tizim monitori" (v0.7). Windows dagi Task Manager ga o'xshash.
 *
 * Har soniyada kernel ichidagi haqiqiy hisoblagichlardan ma'lumot oladi:
 *   - protsessor bandligi = 100% - idle vazifasining ulushi (scheduler hisoblaydi);
 *   - xotira: PMM dagi band frame lar, kernel heap statistikasi;
 *   - vazifalar jadvali (ps buyrug'i bilan bir xil manba);
 *   - uzilishlar soni (/proc/interrupts bilan bir xil).
 */
#include "gui.h"
#include "task.h"
#include "memory.h"
#include "drivers.h"
#include "string.h"
#include "kprintf.h"
#include "cpu.h"

#define W 480
#define H 470
#define SAMPLES 60
#define GRAPH_X 12
#define GRAPH_Y 34
#define GRAPH_W 456
#define GRAPH_H 100

typedef struct {
    uint8_t cpu[SAMPLES];        /* oxirgi 60 soniyadagi protsessor bandligi (%) */
    int     count;
} monitor_t;

static const char *state_name(task_state_t s)
{
    switch (s) {
    case TASK_READY:    return "tayyor";
    case TASK_RUNNING:  return "ishlayapti";
    case TASK_SLEEPING: return "uxlayapti";
    case TASK_BLOCKED:  return "kutyapti";
    case TASK_ZOMBIE:   return "tugagan";
    default:            return "-";
    }
}

static uint32_t current_cpu(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state != TASK_UNUSED && t->pid == 0)
            return 100 - (t->cpu_percent > 100 ? 100 : t->cpu_percent);
    }
    return 0;
}

static void section(surface_t *s, int y, const char *title)
{
    gfx_text_bold(s, 12, y, title, COL_ACCENT);
}

static void redraw(window_t *w)
{
    monitor_t *m = w->data;
    surface_t *s = &w->surf;
    char buf[96];
    gfx_fill(s, 0, 0, W, H, COL_WINDOW_BG);

    /* --- Protsessor --- */
    uint32_t cpu = m->count ? m->cpu[m->count - 1] : 0;
    section(s, 10, "Protsessor");
    ksnprintf(buf, sizeof(buf), "%u%%", cpu);
    gfx_text_bold(s, W - 12 - gfx_text_width(buf), 10, buf, COL_TEXT);
    gfx_fill(s, GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H, RGB(22, 26, 34));
    for (int q = 1; q < 4; q++)                         /* 25%, 50%, 75% chiziqlari */
        gfx_hline(s, GRAPH_X, GRAPH_Y + GRAPH_H * q / 4, GRAPH_W, RGB(48, 54, 66));
    int bar = GRAPH_W / SAMPLES;
    for (int i = 0; i < m->count; i++) {
        int x = GRAPH_X + GRAPH_W - (m->count - i) * bar;
        int h = GRAPH_H * m->cpu[i] / 100;
        if (h < 1)
            h = 1;                                  /* 0% ham ko'rinsin */
        uint32_t c = m->cpu[i] > 80 ? RGB(240, 100, 90) : m->cpu[i] > 40 ? RGB(250, 200, 70) : RGB(90, 200, 120);
        gfx_fill(s, x, GRAPH_Y + GRAPH_H - h, bar - 1, h, c);
    }
    gfx_text(s, GRAPH_X + 4, GRAPH_Y + 2, "100%", RGB(120, 128, 140), GFX_TRANSPARENT);
    gfx_text(s, GRAPH_X + 4, GRAPH_Y + GRAPH_H - 18, "60 soniya", RGB(120, 128, 140), GFX_TRANSPARENT);

    /* --- Xotira --- */
    int y = GRAPH_Y + GRAPH_H + 14;
    section(s, y, "Xotira");
    uint32_t total = pmm_total_frames(), used = pmm_used_frames();
    ksnprintf(buf, sizeof(buf), "%u / %u MB (%u%%)", used * 4 / 1024, total * 4 / 1024,
              total ? used * 100 / total : 0);
    gfx_text(s, W - 12 - gfx_text_width(buf), y, buf, COL_TEXT, GFX_TRANSPARENT);
    y += 22;
    gfx_fill(s, 12, y, GRAPH_W, 14, RGB(220, 224, 232));
    gfx_fill(s, 12, y, (int)(total ? (uint32_t)GRAPH_W * used / total : 0), 14, RGB(70, 130, 220));
    y += 20;
    heap_stats_t hs;
    heap_get_stats(&hs);
    ksnprintf(buf, sizeof(buf), "Kernel heap: %u KB band (%u blok), %u KB bo'sh",
              hs.used_bytes / 1024, hs.used_blocks, hs.free_bytes / 1024);
    gfx_text(s, 12, y, buf, COL_TEXT_DIM, GFX_TRANSPARENT);

    /* --- Vazifalar --- */
    y += 28;
    section(s, y, "Vazifalar");
    y += 20;
    gfx_text_bold(s, 12, y, "PID", COL_TEXT_DIM);
    gfx_text_bold(s, 52, y, "T", COL_TEXT_DIM);
    gfx_text_bold(s, 76, y, "Nomi", COL_TEXT_DIM);
    gfx_text_bold(s, 220, y, "Holat", COL_TEXT_DIM);
    gfx_text_bold(s, 330, y, "CPU", COL_TEXT_DIM);
    gfx_text_bold(s, 390, y, "Xotira", COL_TEXT_DIM);
    y += 18;
    gfx_hline(s, 12, y - 1, GRAPH_W, RGB(210, 214, 222));
    int rows = 0;
    for (int i = 0; i < MAX_TASKS && rows < 11; i++) {
        task_t *t = task_get(i);
        if (t->state == TASK_UNUSED)
            continue;
        if (rows & 1)
            gfx_fill(s, 12, y, GRAPH_W, 17, RGB(232, 236, 242));
        ksnprintf(buf, sizeof(buf), "%u", t->pid);
        gfx_text(s, 12, y, buf, COL_TEXT, GFX_TRANSPARENT);
        gfx_text(s, 52, y, t->user ? "U" : "K", t->user ? RGB(36, 130, 80) : COL_TEXT_DIM, GFX_TRANSPARENT);
        gfx_text(s, 76, y, t->name, COL_TEXT, GFX_TRANSPARENT);
        gfx_text(s, 220, y, state_name(t->state), COL_TEXT_DIM, GFX_TRANSPARENT);
        ksnprintf(buf, sizeof(buf), "%u%%", t->cpu_percent);
        gfx_text(s, 330, y, buf, COL_TEXT, GFX_TRANSPARENT);
        if (t->user)
            ksnprintf(buf, sizeof(buf), "%u KB", t->user_pages * 4);
        else
            strcpy(buf, "-");
        gfx_text(s, 390, y, buf, COL_TEXT, GFX_TRANSPARENT);
        y += 17;
        rows++;
    }

    /* --- Pastki qator --- */
    uint32_t up = timer_seconds();
    ksnprintf(buf, sizeof(buf), "%02u:%02u:%02u  Almashish %u  IRQ: taymer %u, sichq. %u",
              up / 3600, (up / 60) % 60, up % 60, sched_switch_count(), irq_count(0), irq_count(12));
    gfx_fill(s, 0, H - 22, W, 22, RGB(232, 236, 242));
    gfx_text(s, 8, H - 19, buf, COL_TEXT_DIM, GFX_TRANSPARENT);
    wm_invalidate(w);
}

static void monitor_tick(window_t *w)
{
    monitor_t *m = w->data;
    if (m->count == SAMPLES) {
        memmove(m->cpu, m->cpu + 1, SAMPLES - 1);
        m->count--;
    }
    m->cpu[m->count++] = (uint8_t)current_cpu();
    redraw(w);
}

static void monitor_destroy(window_t *w)
{
    kfree(w->data);
}

static const window_ops_t monitor_ops = {
    .tick = monitor_tick,
    .destroy = monitor_destroy,
};

void app_monitor_open(void)
{
    monitor_t *m = kmalloc(sizeof(monitor_t));
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    window_t *w = wm_create("Tizim monitori", W, H, &monitor_ops, m);
    if (!w) {
        kfree(m);
        return;
    }
    monitor_tick(w);
}

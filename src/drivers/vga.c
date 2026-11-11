/*
 * vga.c — VGA matn rejimi drayveri.
 *
 * Ekran 80 ustun x 25 qatordan iborat. Video xotira 0xB8000 manzilda
 * joylashgan: har bir belgi 2 bayt — 1-bayt belgi kodi (ASCII),
 * 2-bayt rang (yuqori 4 bit fon, pastki 4 bit matn rangi).
 * Shu xotiraga yozsak — belgi darhol ekranda paydo bo'ladi.
 *
 * 0-qator status bar uchun ajratilgan, matn 1-qatordan boshlanadi.
 *
 * SCROLLBACK (tarix):
 * VGA xotirasida faqat 25 qator sig'adi. Ekrandan yuqoriga chiqib ketgan
 * qatorlarni yo'qotmaslik uchun ularni alohida "scrollback" buferga
 * (aylanma bufer, 500 qator) saqlaymiz. Foydalanuvchi yuqoriga varaqlasa,
 * ekranga shu buferdan qatorlarni chizamiz.
 */
#include "vga.h"
#include "io.h"
#include "string.h"

#define TEXT_TOP   1                        /* 0-qator — status bar */
#define TEXT_ROWS  (VGA_HEIGHT - TEXT_TOP)  /* matn uchun 24 qator */
#define SB_LINES   500                      /* tarixda saqlanadigan qatorlar */
#define ROW_BYTES  (VGA_WIDTH * 2)

static uint16_t *const vga_buffer = (uint16_t *)0xB8000;
static int cursor_row = TEXT_TOP;
static int cursor_col = 0;
static uint8_t current_color = VGA_LIGHT_GREY | (VGA_BLACK << 4);

/* --- Scrollback ma'lumotlari --- */
static uint16_t scrollback[SB_LINES][VGA_WIDTH];
static int sb_head  = 0;     /* keyingi yoziladigan joy */
static int sb_count = 0;     /* nechta qator saqlangan */
static uint16_t live_screen[TEXT_ROWS][VGA_WIDTH];  /* varaqlashdan oldingi "jonli" ekran */
static int view_offset = 0;  /* 0 — jonli ekran, N — N qator yuqoriga varaqlangan */

static inline uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

static inline uint16_t *screen_row(int row)
{
    return &vga_buffer[row * VGA_WIDTH];
}

/* Ekrandagi miltillovchi kursorni yangilash (VGA controller portlari orqali) */
static void update_hw_cursor(void)
{
    uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void enable_hw_cursor(void)
{
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 13);   /* kursor boshlanish chizig'i */
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);   /* kursor tugash chizig'i */
}

static void disable_hw_cursor(void)
{
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);                       /* 5-bit: kursorni yashirish */
}

/* Qatorni tarix buferiga qo'shish */
static void scrollback_push(const uint16_t *row)
{
    memcpy(scrollback[sb_head], row, ROW_BYTES);
    sb_head = (sb_head + 1) % SB_LINES;
    if (sb_count < SB_LINES)
        sb_count++;
}

/* Tarixdan qator olish: 1 — eng oxirgi saqlangan, 2 — undan oldingisi ... */
static const uint16_t *scrollback_get(int back)
{
    return scrollback[(sb_head - back + SB_LINES) % SB_LINES];
}

/* view_offset ga qarab ekranni qayta chizish */
static void render_view(void)
{
    for (int r = 0; r < TEXT_ROWS; r++) {
        /* from_end: shu qator "tarix + jonli ekran" ketma-ketligining oxiridan nechanchi */
        int from_end = view_offset + (TEXT_ROWS - 1 - r);
        const uint16_t *src;
        if (from_end < TEXT_ROWS)
            src = live_screen[TEXT_ROWS - 1 - from_end];
        else
            src = scrollback_get(from_end - TEXT_ROWS + 1);
        memcpy(screen_row(TEXT_TOP + r), src, ROW_BYTES);
    }
}

void vga_scroll_view(int delta)
{
    int target = view_offset + delta;
    if (target < 0)
        target = 0;
    if (target > sb_count)
        target = sb_count;
    if (target == view_offset)
        return;

    if (view_offset == 0) {                  /* jonli ekrandan chiqyapmiz: saqlab qo'yamiz */
        memcpy(live_screen, screen_row(TEXT_TOP), sizeof(live_screen));
        disable_hw_cursor();
    }

    view_offset = target;

    if (view_offset == 0) {                  /* jonli ekranga qaytdik */
        memcpy(screen_row(TEXT_TOP), live_screen, sizeof(live_screen));
        enable_hw_cursor();
        update_hw_cursor();
    } else {
        render_view();
    }
}

int vga_view_offset(void)
{
    return view_offset;
}

/* Yangi matn chiqsa — avval jonli ekranga qaytamiz (oddiy terminallardagi kabi) */
static inline void snap_to_live(void)
{
    if (view_offset)
        vga_scroll_view(-view_offset);
}

/* Matn pastga yetsa — yuqori qatorni tarixga saqlab, hammasini bitta yuqoriga suramiz */
static void scroll(void)
{
    scrollback_push(screen_row(TEXT_TOP));
    memmove(screen_row(TEXT_TOP), screen_row(TEXT_TOP + 1), (TEXT_ROWS - 1) * ROW_BYTES);
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', current_color);
    cursor_row = VGA_HEIGHT - 1;
}

void vga_init(void)
{
    enable_hw_cursor();
    for (int x = 0; x < VGA_WIDTH; x++)       /* status bar joyini tozalash */
        vga_buffer[x] = vga_entry(' ', VGA_WHITE | (VGA_BLUE << 4));
    for (int y = TEXT_TOP; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', current_color);
    update_hw_cursor();
}

void vga_clear(void)
{
    snap_to_live();
    /* Tozalashdan oldin ekrandagi matnni tarixga o'tkazamiz — yo'qolmaydi */
    for (int y = TEXT_TOP; y <= cursor_row && y < VGA_HEIGHT; y++)
        scrollback_push(screen_row(y));

    for (int y = TEXT_TOP; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', current_color);
    cursor_row = TEXT_TOP;
    cursor_col = 0;
    update_hw_cursor();
}

void vga_putc(char c)
{
    snap_to_live();

    switch (c) {
    case '\n':
        cursor_col = 0;
        cursor_row++;
        break;
    case '\r':
        cursor_col = 0;
        break;
    case '\b':                                  /* backspace: bitta orqaga */
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > TEXT_TOP) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }
        vga_buffer[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ', current_color);
        break;
    case '\t':
        cursor_col = (cursor_col + 4) & ~3;
        break;
    default:
        vga_buffer[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(c, current_color);
        cursor_col++;
        break;
    }

    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= VGA_HEIGHT)
        scroll();

    update_hw_cursor();
}

void vga_puts(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    current_color = (fg & 0x0F) | ((bg & 0x0F) << 4);
}

uint8_t vga_get_color(void)
{
    return current_color;
}

void vga_write_at(int x, int y, const char *s, uint8_t fg, uint8_t bg)
{
    uint8_t color = (fg & 0x0F) | ((bg & 0x0F) << 4);
    while (*s && x < VGA_WIDTH)
        vga_buffer[y * VGA_WIDTH + x++] = vga_entry(*s++, color);
}

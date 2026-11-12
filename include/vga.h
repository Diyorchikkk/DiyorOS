/* vga.h — VGA matn rejimi (80x25) drayveri */
#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stdbool.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11, VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14, VGA_WHITE = 15
};

void    vga_init(void);
void    vga_clear(void);
void    vga_putc(char c);
void    vga_puts(const char *s);
void    vga_set_color(uint8_t fg, uint8_t bg);
uint8_t vga_get_color(void);
/* Kursorga tegmasdan aniq joyga yozish (status bar uchun) */
void    vga_write_at(int x, int y, const char *s, uint8_t fg, uint8_t bg);
/* Tarixni varaqlash: musbat — yuqoriga (eskiroq), manfiy — pastga */
void    vga_scroll_view(int delta);
int     vga_view_offset(void);   /* 0 bo'lsa — jonli ekran */
/* To'liq ekranli dasturlar uchun (matn muharriri) */
void    vga_screen_save(void);
void    vga_screen_restore(void);
void    vga_put_at(int x, int y, char c, uint8_t fg, uint8_t bg);
void    vga_move_hw_cursor(int x, int y);
/* v0.6.1: matn kursori (shell qator tahriri uchun) va serial nusxa */
void    vga_get_cursor(int *x, int *y);
void    vga_set_cursor(int x, int y);
void    vga_putc_local(char c);   /* serial portga nusxalamasdan */

#endif

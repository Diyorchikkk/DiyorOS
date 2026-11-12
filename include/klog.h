/*
 * klog.h — kernel jurnali (log), serial port va panic (v0.6.1).
 *
 *   klog("...")   kernel xabari: xotiradagi jurnalga (dmesg) va serial portga
 *   panic("...")  tuzatib bo'lmaydigan xato: tizimni xavfsiz to'xtatish
 *   KASSERT(x)    "bu shart doim to'g'ri bo'lishi kerak" tekshiruvi
 */
#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "cpu.h"

/* --- Serial port (COM1, 115200 8N1). QEMU da: -serial stdio --- */
void serial_init(void);
bool serial_present(void);
void serial_putc(char c);
void serial_puts(const char *s);

/* --- Kernel jurnali: aylanma bufer, har bir qator oldida vaqt belgisi --- */
void     klog(const char *fmt, ...);            /* jurnal + serial */
void     klog_quiet(const char *fmt, ...);      /* faqat jurnal (ekranga alohida chiqarilgan xabarlar uchun) */
uint32_t klog_size(void);                       /* jurnaldagi baytlar soni */
uint32_t klog_read(uint32_t offset, char *buf, uint32_t len);

/* --- Panic --- */
void panic(const char *fmt, ...) __attribute__((noreturn));
void panic_exception(registers_t *r, const char *name) __attribute__((noreturn));
void double_fault_entry(void) __attribute__((noreturn));   /* #8: task gate orqali, alohida stekda */

#define KASSERT(cond) \
    do { if (!(cond)) panic("KASSERT: %s  (%s:%d)", #cond, __FILE__, __LINE__); } while (0)

#endif

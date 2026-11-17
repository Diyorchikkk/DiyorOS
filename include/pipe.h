/*
 * pipe.h — kanal (pipe): bir jarayon yozadi, ikkinchisi o'qiydi (v0.8).
 */
#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stdbool.h>
#include "sync.h"

#define PIPE_SIZE 4096                /* bufer hajmi (Linux da 64 KB) */

typedef struct pipe {
    uint32_t      id;
    uint8_t       buf[PIPE_SIZE];     /* aylanma (ring) bufer */
    uint32_t      head;               /* keyingi o'qiladigan bayt */
    uint32_t      count;              /* buferdagi baytlar soni */
    int           readers, writers;   /* ochiq o'qish / yozish uchlari */
    wait_queue_t  rq;                 /* ma'lumot kutayotgan o'quvchilar */
    wait_queue_t  wq;                 /* joy kutayotgan yozuvchilar */
    uint32_t      total;              /* statistika: jami o'tgan baytlar */
    uint32_t      blocked_reads, blocked_writes;
    struct pipe  *next;
} pipe_t;

pipe_t  *pipe_create(void);                                  /* readers = writers = 1 */
/* v0.10: dup/fork pipe uchini emas, ochiq fayl yozuvini bo'lishadi (vfs.c: refs) —
   shuning uchun har uchda readers/writers doim 1, yozuv oxirgi havola bilan yopiladi */
int32_t  pipe_read(pipe_t *p, void *buf, uint32_t len);      /* > 0, 0 (EOF), VFS_EINTR */
int32_t  pipe_write(pipe_t *p, const void *buf, uint32_t len); /* len, VFS_EPIPE, VFS_EINTR */
void     pipe_close(pipe_t *p, bool write_end);              /* oxirgisi yopilsa — bo'shatiladi */
int      pipe_report(char *buf, uint32_t size);              /* /proc/sync uchun */

#endif

/*
 * vfs.h — Virtual File System (virtual fayl tizimi).
 *
 * VFS — fayl tizimlari ustidagi umumiy qatlam. Shell va boshqa kod
 * "fayl qayerda saqlanadi" ni bilmaydi: hammasi vfs_open / vfs_read /
 * vfs_write orqali ishlaydi. Har bir fayl tizimi (ramfs, procfs,
 * keyinchalik FAT32) o'z funksiyalarini vfs_ops_t jadvali orqali beradi.
 *
 *   shell (ls, cat, echo >)
 *        |
 *       VFS  ---- yo'llarni tahlil qilish, fayl deskriptorlari, mount
 *      /    \
 *   ramfs   procfs     (v0.6: fat32)
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers.h"

#define VFS_NAME_MAX   32
#define VFS_PATH_MAX   256
#define VFS_MAX_FDS    32
#define VFS_FILE_MAX   (1024 * 1024)   /* bitta fayl maksimum 1 MB */

/* Xato kodlari (manfiy sonlar) */
enum {
    VFS_OK          =  0,
    VFS_ENOENT      = -1,   /* topilmadi */
    VFS_EEXIST      = -2,   /* allaqachon bor */
    VFS_ENOTDIR     = -3,   /* papka emas */
    VFS_EISDIR      = -4,   /* bu papka */
    VFS_ENOTEMPTY   = -5,   /* papka bo'sh emas */
    VFS_ENOMEM      = -6,   /* xotira yetmadi */
    VFS_EINVAL      = -7,   /* noto'g'ri argument */
    VFS_EROFS       = -8,   /* faqat o'qish uchun */
    VFS_EBADF       = -9,   /* noto'g'ri fayl deskriptori */
    VFS_EMFILE      = -10,  /* ochiq fayllar juda ko'p */
    VFS_EFBIG       = -11,  /* fayl juda katta */
    VFS_EBUSY       = -12,  /* band (masalan, mount nuqtasi) */
};

typedef enum { VFS_FILE = 1, VFS_DIR = 2 } vfs_type_t;

struct vfs_node;

/* Har bir fayl tizimi beradigan amallar jadvali */
typedef struct {
    int32_t (*read)(struct vfs_node *n, uint32_t off, uint32_t len, uint8_t *buf);
    int32_t (*write)(struct vfs_node *n, uint32_t off, uint32_t len, const uint8_t *buf);
    int     (*truncate)(struct vfs_node *n, uint32_t size);
    struct vfs_node *(*readdir)(struct vfs_node *dir, uint32_t index);
    struct vfs_node *(*finddir)(struct vfs_node *dir, const char *name);
    int     (*create)(struct vfs_node *dir, const char *name, vfs_type_t type, struct vfs_node **out);
    int     (*unlink)(struct vfs_node *dir, struct vfs_node *child);
    int     (*rename)(struct vfs_node *child, struct vfs_node *new_dir, const char *new_name);
} vfs_ops_t;

/* Fayl yoki papka ("inode") */
typedef struct vfs_node {
    char             name[VFS_NAME_MAX];
    vfs_type_t       type;
    uint32_t         size;
    uint32_t         inode;          /* fayl tizimi ichidagi noyob raqam */
    rtc_time_t       ctime, mtime;   /* yaratilgan va o'zgartirilgan vaqt */
    const char      *fs_name;        /* "ramfs", "procfs" */
    const vfs_ops_t *ops;
    struct vfs_node *parent;
    struct vfs_node *mount;          /* bu papkaga boshqa fayl tizimi ulangan bo'lsa — uning ildizi */
    uint32_t         open_count;     /* nechta fd orqali ochiq */

    /* ramfs uchun */
    struct vfs_node *first_child;
    struct vfs_node *next_sibling;
    uint8_t         *data;
    uint32_t         capacity;

    /* procfs uchun: fayl mazmunini yaratuvchi funksiya */
    int (*generate)(char *buf, uint32_t size);
} vfs_node_t;

/* vfs_open flaglari */
#define VFS_O_READ    0x01
#define VFS_O_WRITE   0x02
#define VFS_O_CREATE  0x04   /* yo'q bo'lsa yaratish */
#define VFS_O_TRUNC   0x08   /* ochishda mazmunni o'chirish */
#define VFS_O_APPEND  0x10   /* oxiriga yozish */

typedef struct {
    vfs_node_t *node;        /* 0 — bo'sh */
    uint32_t    offset;      /* joriy o'qish/yozish joyi */
    uint32_t    flags;
} vfs_fd_t;

/* Ishga tushirish */
void        vfs_init(void);
vfs_node_t *vfs_root(void);
int         vfs_mount(const char *path, vfs_node_t *fs_root);
int         vfs_mount_info(int i, const char **path, const char **fs_name);

/* Yo'llar */
int         vfs_lookup(const char *path, vfs_node_t **out);
int         vfs_get_path(vfs_node_t *node, char *buf, uint32_t size);
int         vfs_chdir(const char *path);
vfs_node_t *vfs_cwd(void);
const char *vfs_basename(const char *path);

/* Fayl va papka amallari */
int         vfs_mkdir(const char *path);
int         vfs_create(const char *path, vfs_node_t **out);   /* touch */
int         vfs_remove(const char *path, bool recursive);
int         vfs_rename(const char *from, const char *to);
vfs_node_t *vfs_readdir(vfs_node_t *dir, uint32_t index);
void        vfs_touch_time(vfs_node_t *n);

/* Fayl deskriptorlari */
int         vfs_open(const char *path, uint32_t flags);
int32_t     vfs_read(int fd, void *buf, uint32_t len);
int32_t     vfs_write(int fd, const void *buf, uint32_t len);
int         vfs_seek(int fd, uint32_t offset);
int         vfs_close(int fd);
const vfs_fd_t *vfs_fd_info(int fd);

const char *vfs_strerror(int err);

/* Fayl tizimlari */
void        rootfs_init(void);           /* yuklanishda: papkalar, fayllar, /proc */
vfs_node_t *ramfs_create_root(void);
vfs_node_t *procfs_create_root(void);

#endif

/*
 * ramfs.c — xotirada (RAM) saqlanadigan fayl tizimi.
 *
 * Har bir fayl va papka — vfs_node_t (kmalloc bilan ajratiladi).
 * Papka ichidagilar bog'langan ro'yxat: first_child -> next_sibling -> ...
 * (nomi bo'yicha alfavit tartibida saqlanadi — ls chiroyli chiqishi uchun).
 * Fayl mazmuni — heap dagi bufer (data). Fayl kattalashsa, bufer ikki
 * baravar kattaroqqa ko'chiriladi (dinamik massiv kabi).
 *
 * Kamchiligi: kompyuter o'chirilsa hammasi yo'qoladi. Diskka saqlash
 * (FAT32) — v0.6 da.
 */
#include "vfs.h"
#include "memory.h"
#include "string.h"

static uint32_t next_inode = 1;
static const vfs_ops_t ramfs_ops;

static vfs_node_t *new_node(const char *name, vfs_type_t type)
{
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n)
        return 0;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->type    = type;
    n->inode   = next_inode++;
    n->fs_name = "ramfs";
    n->ops     = &ramfs_ops;
    rtc_read(&n->ctime);
    n->mtime = n->ctime;
    return n;
}

/* Bolani papkaga nomi bo'yicha tartiblangan joyga qo'shish */
static void attach(vfs_node_t *dir, vfs_node_t *child)
{
    vfs_node_t **pp = &dir->first_child;
    while (*pp && strcmp((*pp)->name, child->name) < 0)
        pp = &(*pp)->next_sibling;
    child->next_sibling = *pp;
    *pp = child;
    child->parent = dir;
}

static void detach(vfs_node_t *dir, vfs_node_t *child)
{
    for (vfs_node_t **pp = &dir->first_child; *pp; pp = &(*pp)->next_sibling)
        if (*pp == child) {
            *pp = child->next_sibling;
            child->next_sibling = 0;
            return;
        }
}

static int32_t ramfs_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf)
{
    if (off >= n->size)
        return 0;                            /* fayl oxiri (EOF) */
    if (len > n->size - off)
        len = n->size - off;
    memcpy(buf, n->data + off, len);
    return (int32_t)len;
}

/* Bufer kamida 'need' baytga sig'adigan bo'lsin */
static int ensure_capacity(vfs_node_t *n, uint32_t need)
{
    if (need <= n->capacity)
        return VFS_OK;
    if (need > VFS_FILE_MAX)
        return VFS_EFBIG;

    uint32_t cap = n->capacity ? n->capacity : 64;
    while (cap < need)
        cap *= 2;
    if (cap > VFS_FILE_MAX)
        cap = VFS_FILE_MAX;

    uint8_t *data = kmalloc(cap);
    if (!data)
        return VFS_ENOMEM;
    if (n->data) {
        memcpy(data, n->data, n->size);
        kfree(n->data);
    }
    n->data = data;
    n->capacity = cap;
    return VFS_OK;
}

static int32_t ramfs_write(vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf)
{
    if (len == 0)
        return 0;
    int err = ensure_capacity(n, off + len);
    if (err)
        return err;
    if (off > n->size)                       /* bo'shliqni nol bilan to'ldirish */
        memset(n->data + n->size, 0, off - n->size);
    memcpy(n->data + off, buf, len);
    if (off + len > n->size)
        n->size = off + len;
    vfs_touch_time(n);
    return (int32_t)len;
}

static int ramfs_truncate(vfs_node_t *n, uint32_t size)
{
    if (size > n->size) {
        int err = ensure_capacity(n, size);
        if (err)
            return err;
        memset(n->data + n->size, 0, size - n->size);
    }
    n->size = size;
    vfs_touch_time(n);
    return VFS_OK;
}

static vfs_node_t *ramfs_readdir(vfs_node_t *dir, uint32_t index)
{
    vfs_node_t *c = dir->first_child;
    while (c && index--)
        c = c->next_sibling;
    return c;
}

static vfs_node_t *ramfs_finddir(vfs_node_t *dir, const char *name)
{
    for (vfs_node_t *c = dir->first_child; c; c = c->next_sibling)
        if (strcmp(c->name, name) == 0)
            return c;
    return 0;
}

static int ramfs_create(vfs_node_t *dir, const char *name, vfs_type_t type, vfs_node_t **out)
{
    vfs_node_t *n = new_node(name, type);
    if (!n)
        return VFS_ENOMEM;
    attach(dir, n);
    vfs_touch_time(dir);
    *out = n;
    return VFS_OK;
}

static int ramfs_unlink(vfs_node_t *dir, vfs_node_t *child)
{
    detach(dir, child);
    vfs_touch_time(dir);
    if (child->data)
        kfree(child->data);
    kfree(child);
    return VFS_OK;
}

static int ramfs_rename(vfs_node_t *child, vfs_node_t *new_dir, const char *new_name)
{
    if (ramfs_finddir(new_dir, new_name))
        return VFS_EEXIST;
    detach(child->parent, child);
    strncpy(child->name, new_name, VFS_NAME_MAX - 1);
    child->name[VFS_NAME_MAX - 1] = 0;
    attach(new_dir, child);
    vfs_touch_time(child);
    return VFS_OK;
}

static const vfs_ops_t ramfs_ops = {
    .read     = ramfs_read,
    .write    = ramfs_write,
    .truncate = ramfs_truncate,
    .readdir  = ramfs_readdir,
    .finddir  = ramfs_finddir,
    .create   = ramfs_create,
    .unlink   = ramfs_unlink,
    .rename   = ramfs_rename,
};

vfs_node_t *ramfs_create_root(void)
{
    return new_node("", VFS_DIR);
}

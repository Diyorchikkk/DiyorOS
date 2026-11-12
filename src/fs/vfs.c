/*
 * vfs.c — Virtual File System: barcha fayl tizimlari uchun umumiy qatlam.
 *
 * Vazifalari:
 *   1. Yo'llarni tahlil qilish: "/home/diyorbek/../etc/./motd" -> fayl tuguni
 *   2. Joriy papka (cwd): nisbiy yo'llar shu yerdan hisoblanadi
 *   3. Mount: bir fayl tizimini boshqasining papkasiga "ulash"
 *      (masalan, procfs -> /proc). Yo'l bo'ylab yurganda mount nuqtasiga
 *      yetsak, ulangan fayl tizimining ildiziga o'tamiz.
 *   4. Fayl deskriptorlari (fd): ochilgan fayllar jadvali. Har bir fd
 *      qaysi faylga va uning qaysi joyiga (offset) ishora qilishini saqlaydi.
 *
 * VFS o'zi ma'lumot saqlamaydi: haqiqiy ish node->ops orqali fayl
 * tizimiga (ramfs, procfs) topshiriladi.
 */
#include "vfs.h"
#include "string.h"
#include "kprintf.h"

#define MAX_MOUNTS 8

static vfs_node_t *root = 0;
static vfs_node_t *cwd = 0;
static vfs_fd_t    fds[VFS_MAX_FDS];

static struct {
    char        path[VFS_PATH_MAX];
    vfs_node_t *fs_root;
} mounts[MAX_MOUNTS];
static int mount_count = 0;

/* ------------------------------- Yordamchilar ------------------------------ */

void vfs_touch_time(vfs_node_t *n)
{
    rtc_read(&n->mtime);
}

static bool valid_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= VFS_NAME_MAX)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '/' || name[i] < 32)
            return false;
    return true;
}

/* ".." — ota papka. Ildizning otasi — o'zi */
static vfs_node_t *parent_of(vfs_node_t *n)
{
    if (n == root || !n->parent)
        return root;
    return n->parent;
}

/* Mount nuqtasi bo'lsa — ulangan fayl tizimining ildiziga o'tamiz */
static vfs_node_t *follow_mount(vfs_node_t *n)
{
    while (n && n->mount)
        n = n->mount;
    return n;
}

/*
 * Yo'lni tugunga aylantirish.
 * "/" bilan boshlansa — ildizdan, aks holda joriy papkadan boshlaymiz.
 * Yo'lni "/" bo'yicha qismlarga ajratib, har birini papkadan qidiramiz.
 */
int vfs_lookup(const char *path, vfs_node_t **out)
{
    if (!path)
        return VFS_EINVAL;

    vfs_node_t *node = (path[0] == '/') ? root : cwd;
    char part[VFS_NAME_MAX];

    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;

        int len = 0;
        while (*path && *path != '/') {
            if (len >= VFS_NAME_MAX - 1)
                return VFS_EINVAL;      /* nom juda uzun */
            part[len++] = *path++;
        }
        part[len] = 0;

        if (strcmp(part, ".") == 0)
            continue;
        if (strcmp(part, "..") == 0) {
            node = parent_of(node);
            continue;
        }
        if (node->type != VFS_DIR)
            return VFS_ENOTDIR;

        vfs_node_t *child = node->ops->finddir ? node->ops->finddir(node, part) : 0;
        if (!child)
            return VFS_ENOENT;
        node = follow_mount(child);
    }

    *out = node;
    return VFS_OK;
}

/* "a/b/c" -> ota papka tuguni (a/b) va oxirgi nom (c) */
static int split_parent(const char *path, vfs_node_t **dir, char *name)
{
    char buf[VFS_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= VFS_PATH_MAX)
        return VFS_EINVAL;
    strcpy(buf, path);

    while (len > 1 && buf[len - 1] == '/')      /* oxirdagi "/" larni olib tashlash */
        buf[--len] = 0;

    char *slash = 0;
    for (char *p = buf; *p; p++)
        if (*p == '/')
            slash = p;

    const char *base;
    int err;
    if (!slash) {
        *dir = cwd;
        base = buf;
        err = VFS_OK;
    } else {
        base = slash + 1;
        if (slash == buf) {
            *dir = root;
            err = VFS_OK;
        } else {
            *slash = 0;
            err = vfs_lookup(buf, dir);
        }
    }
    if (err)
        return err;
    if ((*dir)->type != VFS_DIR)
        return VFS_ENOTDIR;
    if (!valid_name(base))
        return VFS_EINVAL;
    strcpy(name, base);
    return VFS_OK;
}

const char *vfs_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1])
            b = p + 1;
    return b;
}

/* Tugundan ildizgacha yurib, to'liq yo'lni tuzish: "/home/diyorbek" */
int vfs_get_path(vfs_node_t *node, char *buf, uint32_t size)
{
    if (node == root) {
        ksnprintf(buf, size, "/");
        return VFS_OK;
    }

    const char *names[32];
    int depth = 0;
    for (vfs_node_t *n = node; n && n != root && depth < 32; n = n->parent)
        names[depth++] = n->name;

    uint32_t pos = 0;
    buf[0] = 0;
    while (depth--) {
        int w = ksnprintf(buf + pos, size - pos, "/%s", names[depth]);
        pos += w;
        if (pos >= size)
            return VFS_EINVAL;
    }
    return VFS_OK;
}

/* a tuguni b ning ichida (yoki o'zi) ekanmi? */
static bool is_ancestor(vfs_node_t *a, vfs_node_t *b)
{
    for (vfs_node_t *n = b; n; n = (n == root) ? 0 : n->parent)
        if (n == a)
            return true;
    return false;
}

/* --------------------------------- Ildiz ---------------------------------- */

void vfs_init(void)
{
    root = ramfs_create_root();
    root->parent = root;
    cwd = root;
    memset(fds, 0, sizeof(fds));
    strcpy(mounts[0].path, "/");
    mounts[0].fs_root = root;
    mount_count = 1;
}

vfs_node_t *vfs_root(void) { return root; }
vfs_node_t *vfs_cwd(void)  { return cwd; }

/* fs_root ni path papkasiga ulash */
int vfs_mount(const char *path, vfs_node_t *fs_root)
{
    vfs_node_t *dir;
    int err = vfs_lookup(path, &dir);
    if (err)
        return err;
    if (dir->type != VFS_DIR)
        return VFS_ENOTDIR;
    if (dir->mount || mount_count >= MAX_MOUNTS)
        return VFS_EBUSY;

    dir->mount = fs_root;
    fs_root->parent = dir->parent;          /* ".." ulangan fayl tizimidan tashqariga chiqadi */
    strncpy(fs_root->name, dir->name, VFS_NAME_MAX - 1);

    vfs_get_path(dir, mounts[mount_count].path, VFS_PATH_MAX);
    mounts[mount_count].fs_root = fs_root;
    mount_count++;
    return VFS_OK;
}

int vfs_mount_info(int i, const char **path, const char **fs_name)
{
    if (i < 0 || i >= mount_count)
        return VFS_EINVAL;
    *path = mounts[i].path;
    *fs_name = mounts[i].fs_root->fs_name;
    return VFS_OK;
}

int vfs_chdir(const char *path)
{
    vfs_node_t *n;
    int err = vfs_lookup(path, &n);
    if (err)
        return err;
    if (n->type != VFS_DIR)
        return VFS_ENOTDIR;
    cwd = n;
    return VFS_OK;
}

/* ------------------------------ Fayl amallari ------------------------------ */

static int create_node(const char *path, vfs_type_t type, vfs_node_t **out)
{
    vfs_node_t *dir;
    char name[VFS_NAME_MAX];
    int err = split_parent(path, &dir, name);
    if (err)
        return err;
    if (!dir->ops->create)
        return VFS_EROFS;
    if (dir->ops->finddir(dir, name))
        return VFS_EEXIST;
    return dir->ops->create(dir, name, type, out);
}

int vfs_mkdir(const char *path)
{
    vfs_node_t *n;
    return create_node(path, VFS_DIR, &n);
}

int vfs_create(const char *path, vfs_node_t **out)
{
    vfs_node_t *n;
    int err = create_node(path, VFS_FILE, &n);
    if (out && !err)
        *out = n;
    return err;
}

vfs_node_t *vfs_readdir(vfs_node_t *dir, uint32_t index)
{
    dir = follow_mount(dir);
    if (dir->type != VFS_DIR || !dir->ops->readdir)
        return 0;
    return dir->ops->readdir(dir, index);
}

static int remove_node(vfs_node_t *n, bool recursive)
{
    if (n == root || n->mount)
        return VFS_EBUSY;                    /* ildiz yoki mount nuqtasi */
    for (int i = 0; i < mount_count; i++)
        if (mounts[i].fs_root == n)
            return VFS_EBUSY;                /* ulangan fayl tizimining ildizi */
    if (is_ancestor(n, cwd))
        return VFS_EBUSY;                    /* joriy papka yoki uning otasi */
    if (n->open_count)
        return VFS_EBUSY;                    /* fayl ochiq */

    vfs_node_t *parent = n->parent;
    if (!parent->ops->unlink)
        return VFS_EROFS;

    if (n->type == VFS_DIR && n->ops->readdir(n, 0)) {
        if (!recursive)
            return VFS_ENOTEMPTY;
        vfs_node_t *child;
        while ((child = n->ops->readdir(n, 0))) {   /* ichidagilarni birma-bir o'chiramiz */
            int err = remove_node(child, true);
            if (err)
                return err;
        }
    }
    return parent->ops->unlink(parent, n);
}

int vfs_remove(const char *path, bool recursive)
{
    vfs_node_t *n;
    int err = vfs_lookup(path, &n);
    if (err)
        return err;
    return remove_node(n, recursive);
}

/* mv: nomini o'zgartirish yoki boshqa papkaga ko'chirish */
int vfs_rename(const char *from, const char *to)
{
    vfs_node_t *src;
    int err = vfs_lookup(from, &src);
    if (err)
        return err;
    if (src == root || src->mount)
        return VFS_EBUSY;
    for (int i = 0; i < mount_count; i++)
        if (mounts[i].fs_root == src)
            return VFS_EBUSY;

    vfs_node_t *dst_dir;
    char name[VFS_NAME_MAX];
    vfs_node_t *existing;

    if (vfs_lookup(to, &existing) == VFS_OK) {
        if (existing->type != VFS_DIR)
            return VFS_EEXIST;
        dst_dir = existing;                  /* papkaga ko'chirish, nomi o'sha */
        strcpy(name, src->name);
        if (dst_dir->ops->finddir(dst_dir, name))
            return VFS_EEXIST;
    } else {
        err = split_parent(to, &dst_dir, name);
        if (err)
            return err;
    }

    if (dst_dir->ops != src->ops || !src->ops->rename)
        return VFS_EROFS;                    /* boshqa fayl tizimiga ko'chirib bo'lmaydi */
    if (src->type == VFS_DIR && is_ancestor(src, dst_dir))
        return VFS_EINVAL;                   /* papkani o'z ichiga ko'chirib bo'lmaydi */

    return src->ops->rename(src, dst_dir, name);
}

/* --------------------------- Fayl deskriptorlari --------------------------- */

static vfs_fd_t *get_fd(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].node)
        return 0;
    return &fds[fd];
}

int vfs_open(const char *path, uint32_t flags)
{
    vfs_node_t *n;
    int err = vfs_lookup(path, &n);

    if (err == VFS_ENOENT && (flags & VFS_O_CREATE))
        err = vfs_create(path, &n);
    if (err)
        return err;
    if (n->type == VFS_DIR)
        return VFS_EISDIR;
    if ((flags & VFS_O_WRITE) && !n->ops->write)
        return VFS_EROFS;

    /* 0, 1, 2 an'anaviy ravishda stdin, stdout, stderr uchun — 3 dan boshlaymiz */
    int fd = -1;
    for (int i = 3; i < VFS_MAX_FDS; i++)
        if (!fds[i].node) {
            fd = i;
            break;
        }
    if (fd < 0)
        return VFS_EMFILE;

    if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE) && n->ops->truncate) {
        err = n->ops->truncate(n, 0);
        if (err)
            return err;
    }

    fds[fd].node   = n;
    fds[fd].flags  = flags;
    fds[fd].offset = (flags & VFS_O_APPEND) ? n->size : 0;
    n->open_count++;
    return fd;
}

int32_t vfs_read(int fd, void *buf, uint32_t len)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f || !(f->flags & VFS_O_READ))
        return VFS_EBADF;
    if (!f->node->ops->read)
        return VFS_EINVAL;
    int32_t n = f->node->ops->read(f->node, f->offset, len, buf);
    if (n > 0)
        f->offset += n;
    return n;
}

int32_t vfs_write(int fd, const void *buf, uint32_t len)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f || !(f->flags & VFS_O_WRITE))
        return VFS_EBADF;
    if (f->flags & VFS_O_APPEND)
        f->offset = f->node->size;
    int32_t n = f->node->ops->write(f->node, f->offset, len, buf);
    if (n > 0)
        f->offset += n;
    return n;
}

int vfs_seek(int fd, uint32_t offset)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    f->offset = offset;
    return VFS_OK;
}

int vfs_close(int fd)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    f->node->open_count--;
    f->node = 0;
    return VFS_OK;
}

const vfs_fd_t *vfs_fd_info(int fd)
{
    return get_fd(fd);
}

const char *vfs_strerror(int err)
{
    switch (err) {
    case VFS_OK:        return "muvaffaqiyatli";
    case VFS_ENOENT:    return "bunday fayl yoki papka yo'q";
    case VFS_EEXIST:    return "allaqachon mavjud";
    case VFS_ENOTDIR:   return "papka emas";
    case VFS_EISDIR:    return "bu papka";
    case VFS_ENOTEMPTY: return "papka bo'sh emas (rm -r ishlating)";
    case VFS_ENOMEM:    return "xotira yetmadi";
    case VFS_EINVAL:    return "noto'g'ri nom yoki argument";
    case VFS_EROFS:     return "faqat o'qish uchun (yoki boshqa fayl tizimi)";
    case VFS_EBADF:     return "noto'g'ri fayl deskriptori";
    case VFS_EMFILE:    return "ochiq fayllar juda ko'p";
    case VFS_EFBIG:     return "fayl juda katta (maksimum 1 MB)";
    case VFS_EBUSY:     return "band (joriy papka, mount nuqtasi yoki ochiq fayl)";
    default:            return "noma'lum xato";
    }
}

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
 * tizimiga (ramfs, procfs, fat32) topshiriladi.
 *
 * SINXRONIZATSIYA (v0.6.1)
 * VFS ni bir vaqtda bir nechta vazifa ishlatadi: shell (kernel oqimi) va
 * dasturlar (syscall orqali). Agar shell fayl yaratayotgan paytda taymer
 * uni to'xtatib, boshqa dastur ham o'sha papkaga fayl qo'shsa, bog'langan
 * ro'yxat buziladi (race condition). Shuning uchun har bir ochiq (public)
 * funksiya KRITIK SEKSIYA: uzilishlar o'chiq holda, bo'linmas bajariladi.
 * Fayl oxiridagi vfs_* funksiyalar — shu "qulf"ni qo'yadigan qobiqlar,
 * haqiqiy ish *_locked funksiyalarida.
 *
 * v0.8: uzilishlarni o'chirish o'rniga REKURSIV MUTEX (vfs_mutex). Farqi:
 * disk bilan ishlash millisekundlab davom etadi; avval shu paytda taymer,
 * klaviatura, sichqoncha to'xtab turardi. Endi VFS ni kutayotgan vazifa
 * uxlaydi, qolganlari (oyna menejeri, boshqa dasturlar) ishlayveradi.
 * Rekursiv — chunki VFS funksiyalari bir-birini chaqiradi (open -> lookup),
 * fat32 drayveri ham shu qulfdan foydalanadi.
 *
 * Kanallar (pipe) shu jadvalda: fd fayl o'rniga pipe ning bir uchiga
 * ishora qiladi. Pipe dan o'qish/yozish uxlashi mumkin, shuning uchun u
 * vfs_mutex ni USHLAMAGAN holda bajariladi (aks holda "ls | wc" da wc
 * ma'lumot kutib, qulfni ushlab tursa, ls hech narsa yoza olmasdi).
 */
#include "vfs.h"
#include "string.h"
#include "kprintf.h"
#include "task.h"
#include "irq.h"
#include "klog.h"
#include "sync.h"
#include "pipe.h"

#define MAX_MOUNTS 8

static vfs_node_t *root = 0;
static vfs_node_t *cwd = 0;
static vfs_fd_t    fds[VFS_MAX_FDS];
static mutex_t     vfs_mutex;
static bool        vfs_mutex_ready = false;

void vfs_lock(void)
{
    if (!vfs_mutex_ready) {                  /* birinchi chaqiruvda yaratiladi (bo'linmas) */
        uint32_t f = irq_save();
        if (!vfs_mutex_ready) {
            mutex_init(&vfs_mutex, "vfs", true);
            vfs_mutex_ready = true;
        }
        irq_restore(f);
    }
    /* Kernel qulflari qat'iy tartibda olinadi (wm -> vfs -> ata), aylana bo'lishi mumkin
       emas. Aniqlagich baribir topsa — bu kernel xatosi: jim davom etib bo'lmaydi */
    if (mutex_lock(&vfs_mutex) != SYNC_OK)
        panic("vfs qulfida deadlock (qulflar tartibi buzilgan)");
}

void vfs_unlock(void)
{
    mutex_unlock(&vfs_mutex);
}

#define FD_USED(f) ((f).node || (f).pipe)

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
#define HOME_DIR "/home/diyorbek"

static int lookup_locked(const char *path, vfs_node_t **out)
{
    if (!path)
        return VFS_EINVAL;

    /* "~" yoki "~/..." — uy papkasi */
    char expanded[VFS_PATH_MAX];
    if (path[0] == '~' && (path[1] == 0 || path[1] == '/')) {
        ksnprintf(expanded, sizeof(expanded), "%s%s", HOME_DIR, path + 1);
        path = expanded;
    }

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
    if (path[0] == '~' && (path[1] == 0 || path[1] == '/'))
        ksnprintf(buf, sizeof(buf), "%s%s", HOME_DIR, path + 1);
    else
        strcpy(buf, path);
    len = strlen(buf);

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
static int get_path_locked(vfs_node_t *node, char *buf, uint32_t size)
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
static int mount_locked(const char *path, vfs_node_t *fs_root)
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
    strlcpy(fs_root->name, dir->name, VFS_NAME_MAX);

    vfs_get_path(dir, mounts[mount_count].path, VFS_PATH_MAX);
    mounts[mount_count].fs_root = fs_root;
    mount_count++;
    klog("vfs: %s -> %s ulandi", fs_root->fs_name, mounts[mount_count - 1].path);
    return VFS_OK;
}

/* Fayl tizimini uzish: ochiq fayl yoki joriy papka uning ichida bo'lmasligi kerak */
static int unmount_locked(const char *path)
{
    vfs_node_t *dir;
    int err = vfs_lookup(path, &dir);
    if (err)
        return err;
    int idx = -1;
    for (int i = 1; i < mount_count; i++)
        if (mounts[i].fs_root == dir)
            idx = i;
    if (idx < 0)
        return VFS_EINVAL;                  /* bu yerga hech narsa ulanmagan */

    for (vfs_node_t *n = cwd; ; n = n->parent) {   /* joriy papka shu fayl tizimidami? */
        if (n == dir)
            return VFS_EBUSY;
        if (n == root || !n->parent)
            break;
    }
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (fds[i].node && fds[i].node->ops == dir->ops)
            return VFS_EBUSY;               /* ochiq fayllar bor */

    /* Mount nuqtasini topib, ulanishni olib tashlaymiz */
    char mp[VFS_PATH_MAX];
    strcpy(mp, mounts[idx].path);
    vfs_node_t *parent = dir->parent;
    vfs_node_t *point = 0;
    vfs_node_t *c;
    for (uint32_t i = 0; parent->ops->readdir && (c = parent->ops->readdir(parent, i)); i++)
        if (c->mount == dir)
            point = c;
    if (!point)
        return VFS_EINVAL;
    point->mount = 0;

    for (int i = idx; i < mount_count - 1; i++)
        mounts[i] = mounts[i + 1];
    mount_count--;

    klog("vfs: %s uzildi", mp);
    if (dir->ops->unmount)
        dir->ops->unmount(dir);
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

static int chdir_locked(const char *path)
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
    if (dir->mountpoint && !dir->mount)
        return VFS_ENODEV;                   /* masalan, disk ulanmagan /disk */
    if (!dir->ops->create)
        return VFS_EROFS;
    if (dir->ops->finddir(dir, name))
        return VFS_EEXIST;
    return dir->ops->create(dir, name, type, out);
}

static int mkmountpoint_locked(const char *path)
{
    vfs_node_t *n;
    int err = create_node(path, VFS_DIR, &n);
    if (!err)
        n->mountpoint = true;
    return err;
}

static int mkdir_locked(const char *path)
{
    vfs_node_t *n;
    return create_node(path, VFS_DIR, &n);
}

static int create_locked(const char *path, vfs_node_t **out)
{
    vfs_node_t *n;
    int err = create_node(path, VFS_FILE, &n);
    if (out && !err)
        *out = n;
    return err;
}

static vfs_node_t *readdir_locked(vfs_node_t *dir, uint32_t index)
{
    dir = follow_mount(dir);
    if (!dir || dir->type != VFS_DIR || !dir->ops->readdir)
        return 0;
    return dir->ops->readdir(dir, index);
}

static int remove_node(vfs_node_t *n, bool recursive)
{
    if (n == root || n->mount || n->mountpoint)
        return VFS_EBUSY;                    /* ildiz yoki mount nuqtasi (/disk) */
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

static int remove_locked(const char *path, bool recursive)
{
    vfs_node_t *n;
    int err = vfs_lookup(path, &n);
    if (err)
        return err;
    return remove_node(n, recursive);
}

/* mv: nomini o'zgartirish yoki boshqa papkaga ko'chirish */
static int rename_locked(const char *from, const char *to)
{
    vfs_node_t *src;
    int err = vfs_lookup(from, &src);
    if (err)
        return err;
    if (src == root || src->mount || src->mountpoint)
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

    if (dst_dir->mountpoint && !dst_dir->mount)
        return VFS_ENODEV;                   /* ulanmagan /disk ichiga ko'chirib bo'lmaydi */
    if (dst_dir->ops != src->ops)
        return VFS_EXDEV;                    /* boshqa fayl tizimiga ko'chirib bo'lmaydi */
    if (!src->ops->rename)
        return VFS_EROFS;
    if (src->type == VFS_DIR && is_ancestor(src, dst_dir))
        return VFS_EINVAL;                   /* papkani o'z ichiga ko'chirib bo'lmaydi */

    return src->ops->rename(src, dst_dir, name);
}

/* --------------------------- Fayl deskriptorlari --------------------------- */

static vfs_fd_t *get_fd(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !FD_USED(fds[fd]))
        return 0;
    return &fds[fd];
}

/* 0, 1, 2 an'anaviy ravishda stdin, stdout, stderr uchun — 3 dan boshlaymiz */
static int alloc_fd(void)
{
    for (int i = 3; i < VFS_MAX_FDS; i++)
        if (!FD_USED(fds[i]))
            return i;
    return -1;
}

static uint32_t current_pid(void)
{
    return sched_active() ? task_current()->pid : 1;
}

static int open_locked(const char *path, uint32_t flags)
{
    vfs_node_t *n;
    int err = vfs_lookup(path, &n);

    if (err == VFS_ENOENT && (flags & VFS_O_CREATE))
        err = vfs_create(path, &n);
    if (err)
        return err;
    if (n->type == VFS_DIR)
        return VFS_EISDIR;
    if (!(flags & (VFS_O_READ | VFS_O_WRITE)))
        return VFS_EINVAL;                   /* na o'qish, na yozish so'ralgan */
    if ((flags & VFS_O_WRITE) && !n->ops->write)
        return VFS_EROFS;

    int fd = alloc_fd();
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
    fds[fd].owner  = current_pid();
    fds[fd].pipe   = 0;
    n->open_count++;
    return fd;
}

static int32_t read_locked(int fd, void *buf, uint32_t len)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f || !(f->flags & VFS_O_READ) || f->pipe)
        return VFS_EBADF;
    if (!f->node->ops->read)
        return VFS_EINVAL;
    int32_t n = f->node->ops->read(f->node, f->offset, len, buf);
    if (n > 0)
        f->offset += n;
    return n;
}

static int32_t write_locked(int fd, const void *buf, uint32_t len)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f || !(f->flags & VFS_O_WRITE) || f->pipe)
        return VFS_EBADF;
    if (f->flags & VFS_O_APPEND)
        f->offset = f->node->size;
    int32_t n = f->node->ops->write(f->node, f->offset, len, buf);
    if (n > 0)
        f->offset += n;
    return n;
}

static int seek_locked(int fd, uint32_t offset)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    if (f->pipe)
        return VFS_EINVAL;                   /* kanalda "joy" yo'q — faqat ketma-ket */
    f->offset = offset;
    return VFS_OK;
}

static int close_locked(int fd)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    if (f->pipe) {
        pipe_t *p = f->pipe;
        bool w = f->pipe_write;
        f->pipe = 0;
        pipe_close(p, w);
        return VFS_OK;
    }
    f->node->open_count--;
    f->node = 0;
    return VFS_OK;
}

static int pipe_locked(int out[2])
{
    int r = alloc_fd();
    if (r < 0)
        return VFS_EMFILE;
    fds[r].pipe = (pipe_t *)1;               /* vaqtincha band — ikkinchi uch boshqa joy olsin */
    int w = alloc_fd();
    fds[r].pipe = 0;
    if (w < 0)
        return VFS_EMFILE;
    pipe_t *p = pipe_create();
    if (!p)
        return VFS_ENOMEM;
    uint32_t pid = current_pid();
    fds[r] = (vfs_fd_t){ .node = 0, .offset = 0, .flags = VFS_O_READ,  .owner = pid, .pipe = p, .pipe_write = false };
    fds[w] = (vfs_fd_t){ .node = 0, .offset = 0, .flags = VFS_O_WRITE, .owner = pid, .pipe = p, .pipe_write = true };
    out[0] = r;
    out[1] = w;
    return VFS_OK;
}

static int dup_locked(int fd, uint32_t owner)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    int nfd = alloc_fd();
    if (nfd < 0)
        return VFS_EMFILE;
    /*
     * Faylga yozadigan deskriptor nusxalansa (ota va bola bitta faylga yozadi:
     * "ota > log"), ikkalasi ham "oxiriga qo'shish" rejimiga o'tadi. Aks holda har
     * birining o'z pozitsiyasi bo'lib, bola otaning yozganlari ustidan yozib yuborardi.
     * (Unix da buning uchun nusxalar bitta umumiy pozitsiyani bo'lishadi.)
     */
    if (f->node && (f->flags & VFS_O_WRITE))
        f->flags |= VFS_O_APPEND;
    fds[nfd] = *f;
    fds[nfd].owner = owner;
    if (f->pipe)
        pipe_ref(f->pipe, f->pipe_write);
    else
        f->node->open_count++;
    return nfd;
}

const vfs_fd_t *vfs_fd_info(int fd)
{
    return get_fd(fd);
}

bool vfs_fd_owned(int fd, uint32_t pid)
{
    vfs_fd_t *f = get_fd(fd);
    return f && f->owner == pid;
}

static void close_owner_locked(uint32_t pid)
{
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (FD_USED(fds[i]) && fds[i].owner == pid)
            close_locked(i);
}

/* ----------------------- Ochiq funksiyalar: kritik seksiya ----------------------- */
/*
 * Har bir vfs_* funksiya vfs_mutex ni olib, ishni *_locked ga topshiradi.
 * Mutex rekursiv: ichma-ich chaqiruvlar (open -> lookup -> fat32) xavfsiz.
 */
#define VFS_CALL(type, call) do { vfs_lock(); type r_ = (call); vfs_unlock(); return r_; } while (0)

int vfs_lookup(const char *path, vfs_node_t **out)
{
    VFS_CALL(int, lookup_locked(path, out));
}

int vfs_get_path(vfs_node_t *node, char *buf, uint32_t size)
{
    VFS_CALL(int, get_path_locked(node, buf, size));
}

int vfs_mount(const char *path, vfs_node_t *fs_root)
{
    VFS_CALL(int, mount_locked(path, fs_root));
}

int vfs_unmount(const char *path)
{
    VFS_CALL(int, unmount_locked(path));
}

int vfs_chdir(const char *path)
{
    VFS_CALL(int, chdir_locked(path));
}

int vfs_mkmountpoint(const char *path)
{
    VFS_CALL(int, mkmountpoint_locked(path));
}

int vfs_mkdir(const char *path)
{
    VFS_CALL(int, mkdir_locked(path));
}

int vfs_create(const char *path, vfs_node_t **out)
{
    VFS_CALL(int, create_locked(path, out));
}

vfs_node_t *vfs_readdir(vfs_node_t *dir, uint32_t index)
{
    VFS_CALL(vfs_node_t *, readdir_locked(dir, index));
}

int vfs_remove(const char *path, bool recursive)
{
    VFS_CALL(int, remove_locked(path, recursive));
}

int vfs_rename(const char *from, const char *to)
{
    VFS_CALL(int, rename_locked(from, to));
}

int vfs_open(const char *path, uint32_t flags)
{
    VFS_CALL(int, open_locked(path, flags));
}

/*
 * Kanal deskriptori bo'lsa — pipe ni qaytaradi. VFS qulfi olinmaydi: deskriptorni
 * faqat EGASI ishlatadi va faqat egasi yopadi (shu yerda tekshiriladi), demak
 * jadvaldagi bu yozuv biz ishlayotganda o'zgarmaydi. Shu tufayli kanal orqali
 * ma'lumot almashish diskka yozayotgan boshqa vazifani (vfs qulfini) kutmaydi.
 */
static pipe_t *fd_pipe(int fd, uint32_t need_flag)
{
    vfs_fd_t *f = get_fd(fd);
    if (!f || !f->pipe || !(f->flags & need_flag) || f->owner != current_pid())
        return 0;
    return f->pipe;
}

int32_t vfs_read(int fd, void *buf, uint32_t len)
{
    pipe_t *p = fd_pipe(fd, VFS_O_READ);
    if (p)
        return pipe_read(p, buf, len);       /* uxlashi mumkin — VFS qulfi ushlanmagan */
    VFS_CALL(int32_t, read_locked(fd, buf, len));
}

int32_t vfs_write(int fd, const void *buf, uint32_t len)
{
    pipe_t *p = fd_pipe(fd, VFS_O_WRITE);
    if (p)
        return pipe_write(p, buf, len);
    VFS_CALL(int32_t, write_locked(fd, buf, len));
}

int vfs_pipe(int out[2])
{
    VFS_CALL(int, pipe_locked(out));
}

int vfs_dup(int fd, uint32_t owner)
{
    VFS_CALL(int, dup_locked(fd, owner));
}

/* Deskriptorni boshqa jarayonga berish (spawn: bolaning stdin/stdout i).
   Bitta so'z yoziladi — qulfsiz, uzilishlar o'chiq holda ham chaqirsa bo'ladi */
void vfs_fd_set_owner(int fd, uint32_t pid)
{
    if (fd >= 0 && fd < VFS_MAX_FDS && FD_USED(fds[fd]))
        fds[fd].owner = pid;
}

int vfs_seek(int fd, uint32_t offset)
{
    VFS_CALL(int, seek_locked(fd, offset));
}

int vfs_close(int fd)
{
    VFS_CALL(int, close_locked(fd));
}

void vfs_close_owner(uint32_t pid)
{
    vfs_lock();
    close_owner_locked(pid);
    vfs_unlock();
}

/* pid jarayoni nechta fayl ochgan (har bir dasturga chegara qo'yish uchun) */
int vfs_fd_count(uint32_t pid)
{
    vfs_lock();
    int count = 0;
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (FD_USED(fds[i]) && fds[i].owner == pid)
            count++;
    vfs_unlock();
    return count;
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
    case VFS_EXDEV:     return "boshqa fayl tizimiga ko'chirib bo'lmaydi (cp va rm ishlating)";
    case VFS_EIO:       return "disk bilan ishlashda xato";
    case VFS_ENOSPC:    return "diskda joy qolmadi";
    case VFS_ENODEV:    return "disk ulanmagan (formatlash: mkfs, ulash: mount disk)";
    case VFS_EPIPE:     return "kanalning o'qiydigan tomoni yopilgan (broken pipe)";
    case VFS_EINTR:     return "to'xtatildi (signal)";
    default:            return "noma'lum xato";
    }
}

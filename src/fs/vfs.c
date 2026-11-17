/*
 * vfs.c — Virtual File System: barcha fayl tizimlari uchun umumiy qatlam.
 *
 * Vazifalari:
 *   1. Yo'llarni tahlil qilish: "/home/diyorbek/../etc/./motd" -> fayl tuguni
 *   2. Joriy papka (cwd): nisbiy yo'llar shu yerdan hisoblanadi.
 *      v0.10: har VAZIFANING o'z joriy papkasi (task_t.cwd) — dastur "cd" qilsa,
 *      shell ning papkasi o'zgarmaydi
 *   3. Mount: bir fayl tizimini boshqasining papkasiga "ulash"
 *      (masalan, procfs -> /proc). Yo'l bo'ylab yurganda mount nuqtasiga
 *      yetsak, ulangan fayl tizimining ildiziga o'tamiz.
 *   4. Fayl deskriptorlari (fd): ochilgan fayllar jadvali. Har bir fd
 *      qaysi faylga va uning qaysi joyiga (offset) ishora qilishini saqlaydi.
 *      v0.10: ikki qatlam — har vazifaning deskriptor jadvali (task_t.fd) va
 *      tizimning umumiy "ochiq fayllar" jadvali (files[], havolalar soni bilan).
 *      vfs.h dagi rasmga qarang.
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
static vfs_node_t *boot_cwd = 0;              /* scheduler ishga tushmasdan oldingi joriy papka */
static vfs_file_t  files[VFS_MAX_FILES];      /* v0.10: umumiy ochiq fayllar jadvali */
static int16_t     boot_fdt[TASK_MAX_FDS];    /* yuklanish paytidagi deskriptor jadvali */
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

/* Joriy vazifaning deskriptor jadvali va papkasi (yuklanishda — vaqtinchalik) */
static int16_t *cur_fdt(void)
{
    return sched_active() ? task_current()->fd : boot_fdt;
}

static vfs_node_t *cur_cwd(void)
{
    if (!sched_active())
        return boot_cwd;
    task_t *t = task_current();
    return t->cwd ? t->cwd : root;
}

static void set_cwd(vfs_node_t *n)
{
    if (sched_active())
        task_current()->cwd = n;
    else
        boot_cwd = n;
}

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

    vfs_node_t *node = (path[0] == '/') ? root : cur_cwd();
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
        *dir = cur_cwd();
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

/* v0.10: biror tirik vazifaning joriy papkasi n ichidami? (o'chirish / uzishdan oldin) */
static bool cwd_inside(vfs_node_t *n)
{
    if (!sched_active())
        return boot_cwd && is_ancestor(n, boot_cwd);
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state != TASK_UNUSED && t->state != TASK_ZOMBIE && is_ancestor(n, t->cwd ? t->cwd : root))
            return true;
    }
    return false;
}

/* --------------------------------- Ildiz ---------------------------------- */

void vfs_init(void)
{
    root = ramfs_create_root();
    root->parent = root;
    boot_cwd = root;
    memset(files, 0, sizeof(files));
    vfs_fdt_init(boot_fdt);
    strcpy(mounts[0].path, "/");
    mounts[0].fs_root = root;
    mount_count = 1;
}

vfs_node_t *vfs_root(void) { return root; }
vfs_node_t *vfs_cwd(void)  { return cur_cwd(); }

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

    if (cwd_inside(dir))
        return VFS_EBUSY;                   /* biror vazifaning joriy papkasi shu fayl tizimida */
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (files[i].refs && files[i].node && files[i].node->ops == dir->ops)
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
    set_cwd(n);
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
    if (cwd_inside(n))
        return VFS_EBUSY;                    /* biror vazifaning joriy papkasi yoki uning otasi */
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

/* Joriy vazifaning fd raqami -> ochiq fayl yozuvi (konsol yoki bo'sh bo'lsa — 0) */
static vfs_file_t *get_fd(int fd)
{
    if (fd < 0 || fd >= TASK_MAX_FDS)
        return 0;
    int e = cur_fdt()[fd];
    return e >= 0 ? &files[e] : 0;
}

/* Jadvaldagi eng kichik bo'sh raqam (Unix qoidasi: close(1); open(...) -> 1 bo'ladi) */
static int alloc_slot(const int16_t *t)
{
    for (int i = 0; i < TASK_MAX_FDS; i++)
        if (t[i] == FD_FREE)
            return i;
    return -1;
}

static int alloc_file(void)
{
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (!files[i].refs)
            return i;
    return -1;
}

/* Ochiq fayl yozuviga bitta havola kamaydi. Oxirgisi bo'lsa — fayl haqiqatan yopiladi */
static void file_put(int idx)
{
    vfs_file_t *f = &files[idx];
    if (!f->refs || --f->refs)
        return;
    if (f->pipe) {
        pipe_t *p = f->pipe;
        bool w = f->pipe_write;
        f->pipe = 0;
        pipe_close(p, w);                    /* yozuvchi qolmasa — o'quvchi EOF ko'radi */
    } else if (f->node) {
        f->node->open_count--;
        f->node = 0;
    }
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

    int16_t *t = cur_fdt();
    int fd = alloc_slot(t), idx = alloc_file();
    if (fd < 0 || idx < 0)
        return VFS_EMFILE;

    if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE) && n->ops->truncate) {
        err = n->ops->truncate(n, 0);
        if (err)
            return err;
    }

    files[idx] = (vfs_file_t){ .node = n, .flags = flags, .refs = 1,
                               .offset = (flags & VFS_O_APPEND) ? n->size : 0 };
    n->open_count++;
    t[fd] = (int16_t)idx;
    return fd;
}

static int32_t read_locked(int fd, void *buf, uint32_t len)
{
    vfs_file_t *f = get_fd(fd);
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
    vfs_file_t *f = get_fd(fd);
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
    vfs_file_t *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    if (f->pipe)
        return VFS_EINVAL;                   /* kanalda "joy" yo'q — faqat ketma-ket */
    f->offset = offset;
    return VFS_OK;
}

static int close_locked(int fd)
{
    if (fd < 0 || fd >= TASK_MAX_FDS)
        return VFS_EBADF;
    int16_t *t = cur_fdt();
    int e = t[fd];
    if (e == FD_FREE)
        return VFS_EBADF;
    t[fd] = FD_FREE;
    if (e >= 0)
        file_put(e);                         /* konsol bo'lsa — faqat jadvaldan olib tashlanadi */
    return VFS_OK;
}

static int pipe_locked(int out[2])
{
    int16_t *t = cur_fdt();
    int r = alloc_slot(t);
    if (r < 0)
        return VFS_EMFILE;
    t[r] = 0;                                /* vaqtincha band — ikkinchi uch boshqa joy olsin */
    int w = alloc_slot(t);
    t[r] = FD_FREE;
    int ri = alloc_file();
    if (ri >= 0)
        files[ri].refs = 1;                  /* xuddi shunday — vaqtincha band */
    int wi = alloc_file();
    if (ri >= 0)
        files[ri].refs = 0;
    if (w < 0 || ri < 0 || wi < 0)
        return VFS_EMFILE;
    pipe_t *p = pipe_create();
    if (!p)
        return VFS_ENOMEM;
    files[ri] = (vfs_file_t){ .flags = VFS_O_READ,  .refs = 1, .pipe = p, .pipe_write = false };
    files[wi] = (vfs_file_t){ .flags = VFS_O_WRITE, .refs = 1, .pipe = p, .pipe_write = true };
    t[r] = (int16_t)ri;
    t[w] = (int16_t)wi;
    out[0] = r;
    out[1] = w;
    return VFS_OK;
}

/*
 * dup: fd ning NUSXASI — xuddi o'sha ochiq fayl yozuviga yana bitta havola.
 * Pozitsiya umumiy: biri 10 bayt yozsa, ikkinchisi 11-baytdan davom etadi.
 * Shell "dastur > fayl 2>&1" ni shu bilan qiladi.
 */
static int dup_to(int16_t *t, int fd, int to)
{
    int e = t[fd];
    if (to != fd) {
        if (t[to] >= 0)
            file_put(t[to]);                 /* dup2: eski "to" yopiladi */
        t[to] = (int16_t)e;
        if (e >= 0)
            files[e].refs++;
    }
    return to;
}

static int dup_locked(int fd)
{
    int16_t *t = cur_fdt();
    if (fd < 0 || fd >= TASK_MAX_FDS || t[fd] == FD_FREE)
        return VFS_EBADF;
    int to = alloc_slot(t);
    if (to < 0)
        return VFS_EMFILE;
    return dup_to(t, fd, to);
}

static int dup2_locked(int fd, int to)
{
    int16_t *t = cur_fdt();
    if (fd < 0 || fd >= TASK_MAX_FDS || t[fd] == FD_FREE || to < 0 || to >= TASK_MAX_FDS)
        return VFS_EBADF;
    return dup_to(t, fd, to);
}

/* Yangi jadval: 0 (stdin), 1 (stdout), 2 (stderr) — konsol, qolgani bo'sh */
void vfs_fdt_init(int16_t *table)
{
    for (int i = 0; i < TASK_MAX_FDS; i++)
        table[i] = i < 3 ? FD_CONSOLE : FD_FREE;
}

static int fdt_inherit_locked(int16_t *dst, int slot, int fd)
{
    const int16_t *t = cur_fdt();
    if (fd < 0 || fd >= TASK_MAX_FDS || t[fd] == FD_FREE || slot < 0 || slot >= TASK_MAX_FDS)
        return VFS_EBADF;
    dst[slot] = t[fd];
    if (t[fd] >= 0)
        files[t[fd]].refs++;
    return VFS_OK;
}

static void fdt_copy_locked(int16_t *dst)
{
    const int16_t *t = cur_fdt();
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        dst[i] = t[i];
        if (t[i] >= 0)
            files[t[i]].refs++;
    }
}

static void fdt_close_all_locked(int16_t *table)
{
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        int e = table[i];
        table[i] = FD_FREE;
        if (e >= 0)
            file_put(e);
    }
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
 * Kanal deskriptori bo'lsa — pipe ni qaytaradi. VFS qulfi olinmaydi: deskriptor
 * JORIY vazifaning jadvalida, uni faqat shu vazifa o'zgartiradi; jadvaldagi havola
 * turgan ekan, ochiq fayl yozuvi (refs > 0) va undagi pipe bo'shab qolmaydi.
 * Shu tufayli kanal orqali ma'lumot almashish diskka yozayotgan boshqa vazifani
 * (vfs qulfini) kutmaydi.
 */
static pipe_t *fd_pipe(int fd, uint32_t need_flag)
{
    vfs_file_t *f = get_fd(fd);
    if (!f || !f->pipe || !(f->flags & need_flag))
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

int vfs_dup(int fd)
{
    VFS_CALL(int, dup_locked(fd));
}

int vfs_dup2(int fd, int to)
{
    VFS_CALL(int, dup2_locked(fd, to));
}

int vfs_fdt_inherit(int16_t *dst, int slot, int fd)
{
    VFS_CALL(int, fdt_inherit_locked(dst, slot, fd));
}

void vfs_fdt_copy(int16_t *dst)
{
    vfs_lock();
    fdt_copy_locked(dst);
    vfs_unlock();
}

void vfs_fdt_close_all(int16_t *table)
{
    vfs_lock();
    fdt_close_all_locked(table);
    vfs_unlock();
}

/* Deskriptor turi: konsol, fayl yoki kanal (syscall lar uchun) */
int vfs_fd_kind(int fd)
{
    if (fd < 0 || fd >= TASK_MAX_FDS)
        return VFS_EBADF;
    int e = cur_fdt()[fd];
    if (e == FD_CONSOLE)
        return FD_CONSOLE;
    if (e < 0)
        return VFS_EBADF;
    return files[e].pipe ? 1 : 0;
}

int vfs_files_used(void)
{
    int n = 0;
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (files[i].refs)
            n++;
    return n;
}

/*
 * v0.10: uch qatlamni ko'rsatish (fds buyrug'i, /proc/fds):
 *   har jarayonning jadvali (fd -> #yozuv) va umumiy jadval (#yozuv -> fayl, offset, havolalar).
 */
int vfs_fd_report(char *b, uint32_t n)
{
    vfs_lock();
    int len = ksnprintf(b, n, "Jarayonlar jadvallari (fd -> #ochiq fayl, kon - klaviatura/ekran):\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE || t->pid == 0)
            continue;
        len += ksnprintf(b + len, len < (int)n ? n - len : 0, "%3u %-10s", t->pid, t->name);
        for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
            int e = t->fd[fd];
            if (e == FD_CONSOLE)
                len += ksnprintf(b + len, len < (int)n ? n - len : 0, " %d:kon", fd);
            else if (e >= 0)
                len += ksnprintf(b + len, len < (int)n ? n - len : 0, " %d:#%d", fd, e);
        }
        len += ksnprintf(b + len, len < (int)n ? n - len : 0, "\n");
    }
    len += ksnprintf(b + len, len < (int)n ? n - len : 0,
                     "Umumiy jadval (%d / %d band):\n  #  HAVOLA  OFFSET  NIMA\n",
                     vfs_files_used(), VFS_MAX_FILES);
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        vfs_file_t *f = &files[i];
        if (!f->refs)
            continue;
        char what[VFS_PATH_MAX];
        if (f->pipe)
            ksnprintf(what, sizeof(what), "kanal %u, %s uchi", f->pipe->id, f->pipe_write ? "yozish" : "o'qish");
        else if (get_path_locked(f->node, what, sizeof(what)))
            strlcpy(what, f->node->name, sizeof(what));
        len += ksnprintf(b + len, len < (int)n ? n - len : 0, "%3d  %5u  %6u  %s\n",
                         i, f->refs, f->offset, what);
    }
    vfs_unlock();
    return len;
}

int vfs_seek(int fd, uint32_t offset)
{
    VFS_CALL(int, seek_locked(fd, offset));
}

int vfs_close(int fd)
{
    VFS_CALL(int, close_locked(fd));
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

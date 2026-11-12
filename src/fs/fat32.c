/*
 * fat32.c — FAT32 fayl tizimi (v0.6). Diskdagi ma'lumot o'chirib-yoqqandan
 * keyin ham saqlanadi. Windows, macOS va Linux ham FAT32 ni o'qiy oladi —
 * DiyorOS da yozilgan diskni Mac da ochish mumkin.
 *
 * DISK TUZILISHI
 *
 *   sektor 0     Boot sektor (BPB): disk parametrlari — sektor hajmi,
 *                klaster hajmi, FAT jadvali qayerda, ildiz papka qayerda
 *   sektor 1     FSInfo
 *   sektor 6     Boot sektor zaxira nusxasi
 *   sektor 32..  FAT #1 — "klasterlar zanjiri" jadvali
 *   ...          FAT #2 — zaxira nusxa
 *   ...          Ma'lumot hududi: 2-klasterdan boshlab (ildiz papka odatda 2-klasterda)
 *
 * KLASTER va FAT
 *   Ma'lumot klasterlarga bo'linadi (bizda 1 klaster = 1 sektor = 512 bayt).
 *   Katta fayl bir nechta klasterni egallaydi va ular diskda ketma-ket
 *   bo'lishi shart emas. FAT jadvali — "keyingi klaster qaysi" degan ro'yxat:
 *
 *       FAT[5] = 6,  FAT[6] = 9,  FAT[9] = 0x0FFFFFFF (oxiri)
 *       => fayl 5 -> 6 -> 9 klasterlarda joylashgan (bog'langan ro'yxat!)
 *       FAT[x] = 0  => x klaster bo'sh
 *
 * PAPKA — bu 32 baytlik yozuvlardan iborat maxsus fayl:
 *   nom (8.3 ko'rinishida: "SALOM   TXT"), atributlar, vaqt, birinchi klaster, hajm.
 *   Uzun nomlar (LFN, masalan "Mening hujjatim.txt") 8.3 yozuvdan OLDIN
 *   joylashgan qo'shimcha yozuvlarda saqlanadi (har birida 13 ta belgi).
 *
 * Har bir amal uzilishlar o'chiq holda bajariladi (irq_save): disk tuzilmasi
 * bir vaqtda ikki vazifa tomonidan o'zgartirilmasligi uchun. v0.8 da mutex bilan
 * almashtiriladi.
 */
#include "vfs.h"
#include "ata.h"
#include "memory.h"
#include "string.h"
#include "irq.h"
#include "kprintf.h"

#define FAT_EOC        0x0FFFFFF8u    /* bundan katta yoki teng — zanjir oxiri */
#define FAT_EOC_MARK   0x0FFFFFFFu
#define FAT_BAD        0x0FFFFFF7u
#define FAT_MASK       0x0FFFFFFFu

#define ATTR_READONLY  0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIR       0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define NT_LOWER_BASE  0x08            /* 8.3 nomning asosiy qismi kichik harflarda */
#define NT_LOWER_EXT   0x10            /* kengaytmasi kichik harflarda */

typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;             /* FAT32 da 0 */
    uint16_t total16;
    uint8_t  media;
    uint16_t fat_size16;               /* FAT32 da 0 */
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden;
    uint32_t total32;
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive;
    uint8_t  reserved1;
    uint8_t  boot_sig;                 /* 0x29 */
    uint32_t volume_id;
    char     label[11];
    char     fs_type[8];               /* "FAT32   " */
} __attribute__((packed)) fat_bpb_t;

typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  ctime_tenth;
    uint16_t ctime, cdate, adate;
    uint16_t cluster_hi;
    uint16_t mtime, mdate;
    uint16_t cluster_lo;
    uint32_t size;
} __attribute__((packed)) fat_dirent_t;

typedef struct {
    uint8_t  order;                    /* tartib raqami; 0x40 — oxirgi (birinchi joylashgan) */
    uint16_t name1[5];
    uint8_t  attr;                     /* 0x0F */
    uint8_t  type;
    uint8_t  checksum;                 /* 8.3 nomning nazorat yig'indisi */
    uint16_t name2[6];
    uint16_t cluster;                  /* 0 */
    uint16_t name3[2];
} __attribute__((packed)) fat_lfn_t;

typedef struct {
    bool     mounted;
    uint32_t part_lba;                 /* bo'lim (partition) boshlanishi */
    uint32_t spc;                      /* klasterdagi sektorlar */
    uint32_t cluster_bytes;
    uint32_t fat_lba;                  /* birinchi FAT */
    uint32_t fat_size;                 /* bitta FAT ning sektorlari */
    uint32_t fat_count;
    uint32_t data_lba;                 /* 2-klaster */
    uint32_t root_cluster;
    uint32_t cluster_count;            /* ma'lumot klasterlari soni */
    uint32_t free_hint;
    uint32_t volume_id;
    char     label[12];
    vfs_node_t *root;

    /* FAT kesh: bitta sektor (128 ta yozuv) */
    uint32_t cache_sector;
    bool     cache_valid;
    uint8_t  cache[SECTOR_SIZE];
} fat_fs_t;

static fat_fs_t fs;
static const vfs_ops_t fat_ops;
static uint32_t next_inode = 1;

/* ---------------------------- Past daraja ---------------------------- */

static inline uint32_t cluster_lba(uint32_t c)
{
    return fs.data_lba + (c - 2) * fs.spc;
}

static inline bool cluster_valid(uint32_t c)
{
    return c >= 2 && c < fs.cluster_count + 2;
}

static bool cache_load(uint32_t sector)
{
    if (fs.cache_valid && fs.cache_sector == sector)
        return true;
    if (!ata_read(fs.fat_lba + sector, 1, fs.cache))
        return false;
    fs.cache_sector = sector;
    fs.cache_valid = true;
    return true;
}

/* FAT[c] — c klasterdan keyingi klaster */
static uint32_t fat_get(uint32_t c)
{
    if (!cache_load(c * 4 / SECTOR_SIZE))
        return FAT_EOC_MARK;
    return *(uint32_t *)(fs.cache + (c * 4) % SECTOR_SIZE) & FAT_MASK;
}

static bool fat_set(uint32_t c, uint32_t value)
{
    uint32_t sector = c * 4 / SECTOR_SIZE;
    if (!cache_load(sector))
        return false;
    uint32_t *e = (uint32_t *)(fs.cache + (c * 4) % SECTOR_SIZE);
    *e = (*e & ~FAT_MASK) | (value & FAT_MASK);       /* yuqori 4 bit saqlanadi */
    for (uint32_t i = 0; i < fs.fat_count; i++)        /* barcha FAT nusxalariga */
        if (!ata_write(fs.fat_lba + i * fs.fat_size + sector, 1, fs.cache))
            return false;
    return true;
}

static bool zero_cluster(uint32_t c)
{
    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < fs.spc; i++)
        if (!ata_write(cluster_lba(c) + i, 1, zero))
            return false;
    return true;
}

/* Bo'sh klaster topib, zanjirga (prev dan keyin) ulash. 0 — joy yo'q */
static uint32_t alloc_cluster(uint32_t prev)
{
    for (uint32_t i = 0; i < fs.cluster_count; i++) {
        uint32_t c = 2 + (fs.free_hint - 2 + i) % fs.cluster_count;
        if (fat_get(c) != 0)
            continue;
        if (!fat_set(c, FAT_EOC_MARK) || !zero_cluster(c))
            return 0;
        if (prev && !fat_set(prev, c))
            return 0;
        fs.free_hint = c + 1 < fs.cluster_count + 2 ? c + 1 : 2;
        return c;
    }
    return 0;
}

/* Zanjirdagi barcha klasterlarni bo'shatish */
static void free_chain(uint32_t c)
{
    while (cluster_valid(c)) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        if (c < fs.free_hint)
            fs.free_hint = c;
        c = next;
    }
}

/* Zanjirning n-klasteri (0 dan). Yo'q bo'lsa — 0 */
static uint32_t chain_nth(uint32_t c, uint32_t n)
{
    while (n-- && cluster_valid(c))
        c = fat_get(c);
    return cluster_valid(c) ? c : 0;
}

/* Joriy vaqt FAT formatida: yuqori 16 bit — sana, pastki — vaqt */
static uint32_t fat_now(void)
{
    rtc_time_t t;
    rtc_read(&t);
    uint16_t date = (uint16_t)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    uint16_t time = (uint16_t)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
    return ((uint32_t)date << 16) | time;
}

static void to_rtc(uint16_t date, uint16_t time, rtc_time_t *t)
{
    t->year   = 1980 + (date >> 9);
    t->month  = (date >> 5) & 0x0F;
    t->day    = date & 0x1F;
    t->hour   = time >> 11;
    t->minute = (time >> 5) & 0x3F;
    t->second = (time & 0x1F) * 2;
}

/* ------------------------- Papka yozuvlari ------------------------- */

static uint32_t dir_cluster_of(vfs_node_t *dir)
{
    return dir == fs.root ? fs.root_cluster : dir->fat.cluster;
}

/* Papkadagi index-yozuvni o'qish yoki yozish. Papka tugasa — false */
static bool dirent_io(uint32_t dir_cluster, uint32_t index, void *ent, bool write)
{
    uint32_t per_cluster = fs.cluster_bytes / 32;
    uint32_t c = chain_nth(dir_cluster, index / per_cluster);
    if (!c)
        return false;
    uint32_t byte = (index % per_cluster) * 32;
    uint32_t lba = cluster_lba(c) + byte / SECTOR_SIZE;
    uint8_t sec[SECTOR_SIZE];
    if (!ata_read(lba, 1, sec))
        return false;
    if (!write) {
        memcpy(ent, sec + byte % SECTOR_SIZE, 32);
        return true;
    }
    memcpy(sec + byte % SECTOR_SIZE, ent, 32);
    return ata_write(lba, 1, sec);
}

static uint8_t lfn_checksum(const char *short_name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)short_name[i]);
    return sum;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int name_cmp(const char *a, const char *b)       /* FAT da nomlar katta-kichik harfni farqlamaydi */
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return (uint8_t)lower(*a) - (uint8_t)lower(*b);
}

/* "SALOM   TXT" + ntres -> "salom.txt" */
static void short_to_name(const fat_dirent_t *e, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) {
        char c = (i == 0 && (uint8_t)e->name[0] == 0x05) ? (char)0xE5 : e->name[i];
        out[n++] = (e->ntres & NT_LOWER_BASE) ? lower(c) : c;
    }
    if (e->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e->name[i] != ' '; i++)
            out[n++] = (e->ntres & NT_LOWER_EXT) ? lower(e->name[i]) : e->name[i];
    }
    out[n] = 0;
}

static bool short_char_ok(char c)
{
    c = upper(c);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    const char *extra = "$%'-_@~`!(){}^#&";
    for (; *extra; extra++)
        if (c == *extra)
            return true;
    return false;
}

/* Nom 8.3 ko'rinishiga to'g'ridan-to'g'ri sig'adimi? (har bir qismi bir xil registrda) */
static bool fits_short(const char *name, char *out11, uint8_t *ntres)
{
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.') {
            if (dot)
                return false;                  /* bittadan ko'p nuqta */
            dot = p;
        }
    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    int ext_len  = dot ? (int)strlen(dot + 1) : 0;
    if (base_len < 1 || base_len > 8 || ext_len > 3 || (dot && ext_len == 0))
        return false;

    bool lo[2] = { false, false }, up[2] = { false, false };
    memset(out11, ' ', 11);
    for (int i = 0; i < base_len; i++) {
        char c = name[i];
        if (!short_char_ok(c))
            return false;
        if (c >= 'a' && c <= 'z') lo[0] = true;
        if (c >= 'A' && c <= 'Z') up[0] = true;
        out11[i] = upper(c);
    }
    for (int i = 0; i < ext_len; i++) {
        char c = dot[1 + i];
        if (!short_char_ok(c))
            return false;
        if (c >= 'a' && c <= 'z') lo[1] = true;
        if (c >= 'A' && c <= 'Z') up[1] = true;
        out11[8 + i] = upper(c);
    }
    if ((lo[0] && up[0]) || (lo[1] && up[1]))
        return false;                          /* aralash registr — uzun nom kerak */
    *ntres = (lo[0] ? NT_LOWER_BASE : 0) | (lo[1] ? NT_LOWER_EXT : 0);
    if ((uint8_t)out11[0] == 0xE5)
        out11[0] = 0x05;
    return true;
}

static bool short_exists(vfs_node_t *dir, const char *s11)
{
    for (vfs_node_t *c = dir->first_child; c; c = c->next_sibling)
        if (memcmp(c->fat.short_name, s11, 11) == 0)
            return true;
    return false;
}

/* Uzun nom uchun 8.3 taxallus: "Mening hujjatim.txt" -> "MENING~1TXT" */
static bool make_alias(vfs_node_t *dir, const char *name, char *out11)
{
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            dot = p;
    char base[9], ext[4];
    int bl = 0, el = 0;
    for (const char *p = name; *p && (!dot || p < dot) && bl < 8; p++)
        if (*p != ' ' && *p != '.')
            base[bl++] = short_char_ok(*p) ? upper(*p) : '_';
    if (dot)
        for (const char *p = dot + 1; *p && el < 3; p++)
            if (*p != ' ')
                ext[el++] = short_char_ok(*p) ? upper(*p) : '_';
    if (bl == 0)
        base[bl++] = '_';

    for (int n = 1; n < 100; n++) {
        char num[4];
        int nl = ksnprintf(num, sizeof(num), "%d", n);
        int keep = 8 - 1 - nl;
        if (keep > bl)
            keep = bl;
        memset(out11, ' ', 11);
        memcpy(out11, base, keep);
        out11[keep] = '~';
        memcpy(out11 + keep + 1, num, nl);
        memcpy(out11 + 8, ext, el);
        if (!short_exists(dir, out11))
            return true;
    }
    return false;
}

/* ------------------------- Tugunlar (VFS bilan bog'lash) ------------------------- */

static void attach_sorted(vfs_node_t *dir, vfs_node_t *child)
{
    vfs_node_t **pp = &dir->first_child;
    while (*pp && name_cmp((*pp)->name, child->name) < 0)
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

static vfs_node_t *node_from_entry(const fat_dirent_t *e, const char *name, uint32_t index, uint8_t lfn)
{
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n)
        return 0;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->type    = (e->attr & ATTR_DIR) ? VFS_DIR : VFS_FILE;
    n->size    = (e->attr & ATTR_DIR) ? 0 : e->size;
    n->inode   = next_inode++;
    n->fs_name = "fat32";
    n->ops     = &fat_ops;
    n->fat.cluster   = ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
    n->fat.dir_index = index;
    n->fat.lfn_count = lfn;
    n->fat.attr      = e->attr;
    memcpy(n->fat.short_name, e->name, 11);
    to_rtc(e->cdate, e->ctime, &n->ctime);
    to_rtc(e->mdate, e->mtime, &n->mtime);
    return n;
}

/* Papka ichidagilarni diskdan o'qib, tugunlar yaratish (birinchi murojaatda) */
static int load_dir(vfs_node_t *dir)
{
    if (dir->fat.loaded)
        return VFS_OK;
    uint8_t *buf = kmalloc(fs.cluster_bytes);
    if (!buf)
        return VFS_ENOMEM;

    char lfn[VFS_NAME_MAX * 2 + 14];
    int lfn_entries = 0;
    uint8_t lfn_sum = 0;
    bool lfn_ok = false;
    uint32_t index = 0;
    uint32_t per_cluster = fs.cluster_bytes / 32;
    bool done = false;

    for (uint32_t c = dir_cluster_of(dir); cluster_valid(c) && !done; c = fat_get(c)) {
        if (!ata_read(cluster_lba(c), fs.spc, buf)) {
            kfree(buf);
            return VFS_EIO;
        }
        for (uint32_t i = 0; i < per_cluster; i++, index++) {
            fat_dirent_t *e = (fat_dirent_t *)(buf + i * 32);
            uint8_t first = (uint8_t)e->name[0];
            if (first == 0x00) {               /* papka oxiri */
                done = true;
                break;
            }
            if (first == 0xE5) {               /* o'chirilgan yozuv */
                lfn_ok = false;
                continue;
            }
            if (e->attr == ATTR_LFN) {         /* uzun nom bo'lagi */
                fat_lfn_t *l = (fat_lfn_t *)e;
                int seq = l->order & 0x1F;
                if (l->order & 0x40) {
                    memset(lfn, 0, sizeof(lfn));
                    lfn_entries = seq;
                    lfn_sum = l->checksum;
                    lfn_ok = seq > 0 && seq <= 5;   /* 65 belgigacha (VFS chegarasi 31) */
                }
                if (!lfn_ok || seq < 1 || seq > lfn_entries || l->checksum != lfn_sum) {
                    lfn_ok = false;
                    continue;
                }
                uint16_t chars[13];
                memcpy(chars, l->name1, 10);
                memcpy(chars + 5, l->name2, 12);
                memcpy(chars + 11, l->name3, 4);
                for (int k = 0; k < 13; k++) {
                    uint16_t ch = chars[k];
                    char out = (ch == 0 || ch == 0xFFFF) ? 0 : (ch < 0x80 ? (char)ch : '?');
                    lfn[(seq - 1) * 13 + k] = out;
                }
                continue;
            }
            if (e->attr & ATTR_VOLUME) {       /* disk nomi — fayl emas */
                lfn_ok = false;
                continue;
            }

            char name[VFS_NAME_MAX * 2 + 14];
            uint8_t lfn_used = 0;
            if (lfn_ok && lfn_checksum(e->name) == lfn_sum) {
                strcpy(name, lfn);
                lfn_used = (uint8_t)lfn_entries;
            } else {
                short_to_name(e, name);
            }
            lfn_ok = false;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (strlen(name) >= VFS_NAME_MAX)
                name[VFS_NAME_MAX - 1] = 0;    /* VFS nom chegarasi */

            vfs_node_t *n = node_from_entry(e, name, index, lfn_used);
            if (!n) {
                kfree(buf);
                return VFS_ENOMEM;
            }
            attach_sorted(dir, n);
        }
    }
    kfree(buf);
    dir->fat.loaded = true;
    return VFS_OK;
}

/* Faylning 8.3 yozuvini yangilash (hajm, birinchi klaster, vaqt) */
static int update_dirent(vfs_node_t *n)
{
    fat_dirent_t e;
    uint32_t dc = dir_cluster_of(n->parent);
    if (!dirent_io(dc, n->fat.dir_index, &e, false))
        return VFS_EIO;
    e.size = (n->type == VFS_DIR) ? 0 : n->size;
    e.cluster_hi = (uint16_t)(n->fat.cluster >> 16);
    e.cluster_lo = (uint16_t)(n->fat.cluster & 0xFFFF);
    uint32_t now = fat_now();
    e.mdate = (uint16_t)(now >> 16);
    e.mtime = (uint16_t)now;
    e.adate = e.mdate;
    to_rtc(e.mdate, e.mtime, &n->mtime);
    return dirent_io(dc, n->fat.dir_index, &e, true) ? VFS_OK : VFS_EIO;
}

/* Papkada ketma-ket 'count' ta bo'sh yozuv topish (kerak bo'lsa papkani kattalashtirish) */
static int find_free_slots(vfs_node_t *dir, uint32_t count, uint32_t *start)
{
    uint32_t per_cluster = fs.cluster_bytes / 32;
    uint32_t c = dir_cluster_of(dir), prev = 0, index = 0, run = 0, run_start = 0;
    uint8_t *buf = kmalloc(fs.cluster_bytes);
    if (!buf)
        return VFS_ENOMEM;

    for (;;) {
        if (!cluster_valid(c)) {               /* papka tugadi — yangi klaster qo'shamiz */
            c = alloc_cluster(prev);
            if (!c) {
                kfree(buf);
                return VFS_ENOSPC;
            }
            if (run == 0)
                run_start = index;
            run += per_cluster;
            if (run >= count)
                break;
            prev = c;
            c = 0;
            index += per_cluster;
            continue;
        }
        if (!ata_read(cluster_lba(c), fs.spc, buf)) {
            kfree(buf);
            return VFS_EIO;
        }
        for (uint32_t i = 0; i < per_cluster; i++, index++) {
            uint8_t first = buf[i * 32];
            if (first == 0x00 || first == 0xE5) {
                if (run == 0)
                    run_start = index;
                if (++run >= count)
                    goto found;
            } else {
                run = 0;
            }
        }
        prev = c;
        c = fat_get(c);
    }
found:
    kfree(buf);
    *start = run_start;
    return VFS_OK;
}

/*
 * Papkaga yangi yozuv(lar) qo'shish: kerak bo'lsa LFN yozuvlari + 8.3 yozuv.
 * base — atributlar, klaster, hajm va vaqtlar to'ldirilgan yozuv (nomidan tashqari).
 */
static int write_entries(vfs_node_t *dir, const char *name, fat_dirent_t *base,
                         uint32_t *out_index, uint8_t *out_lfn)
{
    uint8_t ntres = 0;
    int lfn_n = 0;
    if (!fits_short(name, base->name, &ntres)) {
        if (!make_alias(dir, name, base->name))
            return VFS_EEXIST;
        lfn_n = (strlen(name) + 12) / 13;
        ntres = 0;
    }
    base->ntres = ntres;

    uint32_t start;
    int err = find_free_slots(dir, lfn_n + 1, &start);
    if (err)
        return err;

    uint32_t dc = dir_cluster_of(dir);
    uint8_t sum = lfn_checksum(base->name);
    int len = strlen(name);
    for (int i = 0; i < lfn_n; i++) {
        int seq = lfn_n - i;                   /* teskari tartibda: oxirgi bo'lak birinchi */
        fat_lfn_t l;
        memset(&l, 0, sizeof(l));
        l.order = (uint8_t)seq | (i == 0 ? 0x40 : 0);
        l.attr = ATTR_LFN;
        l.checksum = sum;
        uint16_t chars[13];
        for (int k = 0; k < 13; k++) {
            int pos = (seq - 1) * 13 + k;
            chars[k] = pos < len ? (uint8_t)name[pos] : (pos == len ? 0x0000 : 0xFFFF);
        }
        memcpy(l.name1, chars, 10);
        memcpy(l.name2, chars + 5, 12);
        memcpy(l.name3, chars + 11, 4);
        if (!dirent_io(dc, start + i, &l, true))
            return VFS_EIO;
    }
    if (!dirent_io(dc, start + lfn_n, base, true))
        return VFS_EIO;
    *out_index = start + lfn_n;
    *out_lfn = (uint8_t)lfn_n;
    return VFS_OK;
}

/* Yozuv(lar)ni o'chirilgan deb belgilash (0xE5) */
static int erase_entries(vfs_node_t *dir, uint32_t index, uint8_t lfn)
{
    uint32_t dc = dir_cluster_of(dir);
    for (uint32_t i = index - lfn; i <= index; i++) {
        fat_dirent_t e;
        if (!dirent_io(dc, i, &e, false))
            return VFS_EIO;
        e.name[0] = (char)0xE5;
        if (!dirent_io(dc, i, &e, true))
            return VFS_EIO;
    }
    return VFS_OK;
}

/* ----------------------------- VFS amallari ----------------------------- */

static int32_t do_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf)
{
    if (off >= n->size)
        return 0;
    if (len > n->size - off)
        len = n->size - off;

    uint8_t *cbuf = kmalloc(fs.cluster_bytes);
    if (!cbuf)
        return VFS_ENOMEM;
    uint32_t c = chain_nth(n->fat.cluster, off / fs.cluster_bytes);
    uint32_t done = 0;
    while (done < len && c) {
        uint32_t in = (off + done) % fs.cluster_bytes;
        uint32_t chunk = fs.cluster_bytes - in;
        if (chunk > len - done)
            chunk = len - done;
        if (!ata_read(cluster_lba(c), fs.spc, cbuf)) {
            kfree(cbuf);
            return VFS_EIO;
        }
        memcpy(buf + done, cbuf + in, chunk);
        done += chunk;
        c = fat_get(c);
        if (!cluster_valid(c))
            c = 0;
    }
    kfree(cbuf);
    return (int32_t)done;
}

/* Fayl kamida 'clusters' ta klasterdan iborat bo'lsin */
static int ensure_clusters(vfs_node_t *n, uint32_t clusters)
{
    if (clusters == 0)
        return VFS_OK;
    if (!n->fat.cluster) {
        uint32_t c = alloc_cluster(0);
        if (!c)
            return VFS_ENOSPC;
        n->fat.cluster = c;
    }
    uint32_t c = n->fat.cluster, have = 1;
    while (have < clusters) {
        uint32_t next = fat_get(c);
        if (!cluster_valid(next)) {
            next = alloc_cluster(c);
            if (!next)
                return VFS_ENOSPC;
        }
        c = next;
        have++;
    }
    return VFS_OK;
}

static int32_t do_write(vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf)
{
    if (len == 0)
        return 0;
    if (off + len < off)
        return VFS_EFBIG;
    uint32_t end = off + len;
    int err = ensure_clusters(n, (end + fs.cluster_bytes - 1) / fs.cluster_bytes);
    if (err)
        return err;

    uint8_t *cbuf = kmalloc(fs.cluster_bytes);
    if (!cbuf)
        return VFS_ENOMEM;
    uint32_t c = chain_nth(n->fat.cluster, off / fs.cluster_bytes);
    uint32_t done = 0;
    while (done < len && c) {
        uint32_t in = (off + done) % fs.cluster_bytes;
        uint32_t chunk = fs.cluster_bytes - in;
        if (chunk > len - done)
            chunk = len - done;
        if (chunk < fs.cluster_bytes && !ata_read(cluster_lba(c), fs.spc, cbuf)) {
            kfree(cbuf);
            return VFS_EIO;
        }
        memcpy(cbuf + in, buf + done, chunk);
        if (!ata_write(cluster_lba(c), fs.spc, cbuf)) {
            kfree(cbuf);
            return VFS_EIO;
        }
        done += chunk;
        c = fat_get(c);
        if (!cluster_valid(c))
            c = 0;
    }
    kfree(cbuf);
    /* Agar off > hajm bo'lsa, oradagi bo'shliq nolligicha qoladi: yangi klasterlar tozalangan */
    if (end > n->size)
        n->size = end;
    err = update_dirent(n);
    return err ? err : (int32_t)done;
}

static int do_truncate(vfs_node_t *n, uint32_t size)
{
    if (size > n->size) {                      /* kattalashtirish: nollar bilan */
        uint8_t zero[SECTOR_SIZE];
        memset(zero, 0, sizeof(zero));
        while (n->size < size) {
            uint32_t chunk = size - n->size > SECTOR_SIZE ? SECTOR_SIZE : size - n->size;
            int32_t w = do_write(n, n->size, chunk, zero);
            if (w < 0)
                return w;
        }
        return VFS_OK;
    }
    uint32_t keep = (size + fs.cluster_bytes - 1) / fs.cluster_bytes;
    if (keep == 0) {
        free_chain(n->fat.cluster);
        n->fat.cluster = 0;
    } else {
        uint32_t last = chain_nth(n->fat.cluster, keep - 1);
        if (last) {
            uint32_t rest = fat_get(last);
            fat_set(last, FAT_EOC_MARK);
            free_chain(rest);
        }
    }
    n->size = size;
    return update_dirent(n);
}

static vfs_node_t *do_readdir(vfs_node_t *dir, uint32_t index)
{
    if (load_dir(dir) != VFS_OK)
        return 0;
    vfs_node_t *c = dir->first_child;
    while (c && index--)
        c = c->next_sibling;
    return c;
}

static vfs_node_t *do_finddir(vfs_node_t *dir, const char *name)
{
    if (load_dir(dir) != VFS_OK)
        return 0;
    for (vfs_node_t *c = dir->first_child; c; c = c->next_sibling)
        if (name_cmp(c->name, name) == 0)
            return c;
    return 0;
}

static int do_create(vfs_node_t *dir, const char *name, vfs_type_t type, vfs_node_t **out)
{
    int err = load_dir(dir);
    if (err)
        return err;

    fat_dirent_t e;
    memset(&e, 0, sizeof(e));
    e.attr = (type == VFS_DIR) ? ATTR_DIR : ATTR_ARCHIVE;
    uint32_t now = fat_now();
    e.cdate = (uint16_t)(now >> 16);
    e.ctime = (uint16_t)now;
    e.mdate = e.adate = e.cdate;
    e.mtime = e.ctime;

    uint32_t cluster = 0;
    if (type == VFS_DIR) {
        /* Yangi papka: o'z klasteri, ichida "." va ".." yozuvlari */
        cluster = alloc_cluster(0);
        if (!cluster)
            return VFS_ENOSPC;
        fat_dirent_t dot = e;
        memcpy(dot.name, ".          ", 11);
        dot.cluster_hi = (uint16_t)(cluster >> 16);
        dot.cluster_lo = (uint16_t)cluster;
        fat_dirent_t dotdot = e;
        memcpy(dotdot.name, "..         ", 11);
        uint32_t pc = (dir == fs.root) ? 0 : dir->fat.cluster;   /* ildiz uchun 0 */
        dotdot.cluster_hi = (uint16_t)(pc >> 16);
        dotdot.cluster_lo = (uint16_t)pc;
        if (!dirent_io(cluster, 0, &dot, true) || !dirent_io(cluster, 1, &dotdot, true)) {
            free_chain(cluster);
            return VFS_EIO;
        }
        e.cluster_hi = (uint16_t)(cluster >> 16);
        e.cluster_lo = (uint16_t)cluster;
    }

    uint32_t index;
    uint8_t lfn;
    err = write_entries(dir, name, &e, &index, &lfn);
    if (err) {
        free_chain(cluster);
        return err;
    }

    vfs_node_t *n = node_from_entry(&e, name, index, lfn);
    if (!n)
        return VFS_ENOMEM;
    if (type == VFS_DIR)
        n->fat.loaded = true;                  /* yangi papka bo'sh */
    attach_sorted(dir, n);
    *out = n;
    return VFS_OK;
}

static int do_unlink(vfs_node_t *dir, vfs_node_t *child)
{
    int err = erase_entries(dir, child->fat.dir_index, child->fat.lfn_count);
    if (err)
        return err;
    free_chain(child->fat.cluster);
    detach(dir, child);
    kfree(child);
    return VFS_OK;
}

static int do_rename(vfs_node_t *child, vfs_node_t *new_dir, const char *new_name)
{
    int err = load_dir(new_dir);
    if (err)
        return err;
    if (do_finddir(new_dir, new_name))
        return VFS_EEXIST;

    vfs_node_t *old_dir = child->parent;
    fat_dirent_t e;
    if (!dirent_io(dir_cluster_of(old_dir), child->fat.dir_index, &e, false))
        return VFS_EIO;

    uint32_t index;
    uint8_t lfn;
    err = write_entries(new_dir, new_name, &e, &index, &lfn);   /* klaster va hajm o'sha */
    if (err)
        return err;
    err = erase_entries(old_dir, child->fat.dir_index, child->fat.lfn_count);
    if (err)
        return err;

    if (child->type == VFS_DIR && old_dir != new_dir) {         /* ".." yangi otaga */
        fat_dirent_t dd;
        if (dirent_io(child->fat.cluster, 1, &dd, false)) {
            uint32_t pc = (new_dir == fs.root) ? 0 : new_dir->fat.cluster;
            dd.cluster_hi = (uint16_t)(pc >> 16);
            dd.cluster_lo = (uint16_t)pc;
            dirent_io(child->fat.cluster, 1, &dd, true);
        }
    }

    detach(old_dir, child);
    strncpy(child->name, new_name, VFS_NAME_MAX - 1);
    child->name[VFS_NAME_MAX - 1] = 0;
    child->fat.dir_index = index;
    child->fat.lfn_count = lfn;
    memcpy(child->fat.short_name, e.name, 11);
    attach_sorted(new_dir, child);
    return VFS_OK;
}

static void free_tree(vfs_node_t *n)
{
    vfs_node_t *c = n->first_child;
    while (c) {
        vfs_node_t *next = c->next_sibling;
        free_tree(c);
        kfree(c);
        c = next;
    }
    n->first_child = 0;
}

static int do_unmount(vfs_node_t *root)
{
    free_tree(root);
    kfree(root);
    fs.mounted = false;
    fs.root = 0;
    fs.cache_valid = false;
    return VFS_OK;
}

/* ----- Har bir amal uzilishlarsiz (atomar) bajariladi ----- */

#define LOCKED(expr) do { uint32_t _f = irq_save(); __typeof__(expr) _r = (expr); irq_restore(_f); return _r; } while (0)

static int32_t op_read(vfs_node_t *n, uint32_t o, uint32_t l, uint8_t *b)        { LOCKED(do_read(n, o, l, b)); }
static int32_t op_write(vfs_node_t *n, uint32_t o, uint32_t l, const uint8_t *b) { LOCKED(do_write(n, o, l, b)); }
static int     op_truncate(vfs_node_t *n, uint32_t s)                            { LOCKED(do_truncate(n, s)); }
static vfs_node_t *op_readdir(vfs_node_t *d, uint32_t i)                         { LOCKED(do_readdir(d, i)); }
static vfs_node_t *op_finddir(vfs_node_t *d, const char *nm)                     { LOCKED(do_finddir(d, nm)); }
static int op_create(vfs_node_t *d, const char *nm, vfs_type_t t, vfs_node_t **o) { LOCKED(do_create(d, nm, t, o)); }
static int op_unlink(vfs_node_t *d, vfs_node_t *c)                               { LOCKED(do_unlink(d, c)); }
static int op_rename(vfs_node_t *c, vfs_node_t *d, const char *nm)               { LOCKED(do_rename(c, d, nm)); }
static int op_unmount(vfs_node_t *r)                                             { LOCKED(do_unmount(r)); }

static const vfs_ops_t fat_ops = {
    .read     = op_read,
    .write    = op_write,
    .truncate = op_truncate,
    .readdir  = op_readdir,
    .finddir  = op_finddir,
    .create   = op_create,
    .unlink   = op_unlink,
    .rename   = op_rename,
    .unmount  = op_unmount,
};

/* ------------------------------ Ulash (mount) ------------------------------ */

int fat32_mount(vfs_node_t **out)
{
    if (!ata_info()->present)
        return VFS_ENOENT;
    if (fs.mounted)
        return VFS_EBUSY;

    uint8_t sec[SECTOR_SIZE];
    if (!ata_read(0, 1, sec))
        return VFS_EIO;
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return VFS_EINVAL;                     /* formatlanmagan */

    /* Bo'limlar jadvali (MBR) bo'lsa — birinchi FAT32 bo'limini topamiz */
    uint32_t part = 0;
    if (sec[0] != 0xEB && sec[0] != 0xE9) {
        for (int i = 0; i < 4; i++) {
            uint8_t *p = sec + 446 + i * 16;
            if (p[4] == 0x0B || p[4] == 0x0C) {
                part = *(uint32_t *)(p + 8);
                break;
            }
        }
        if (!part || !ata_read(part, 1, sec))
            return VFS_EINVAL;
    }

    fat_bpb_t *b = (fat_bpb_t *)sec;
    if (b->bytes_per_sector != SECTOR_SIZE || b->sectors_per_cluster == 0 ||
        (b->sectors_per_cluster & (b->sectors_per_cluster - 1)) ||
        b->fat_size16 != 0 || b->fat_size32 == 0 || b->fat_count == 0 ||
        b->root_cluster < 2 || memcmp(b->fs_type, "FAT32", 5) != 0)
        return VFS_EINVAL;                     /* FAT32 emas */

    memset(&fs, 0, sizeof(fs));
    fs.part_lba      = part;
    fs.spc           = b->sectors_per_cluster;
    fs.cluster_bytes = fs.spc * SECTOR_SIZE;
    fs.fat_lba       = part + b->reserved_sectors;
    fs.fat_size      = b->fat_size32;
    fs.fat_count     = b->fat_count;
    fs.data_lba      = fs.fat_lba + fs.fat_count * fs.fat_size;
    fs.root_cluster  = b->root_cluster;
    uint32_t total   = b->total32 ? b->total32 : b->total16;
    fs.cluster_count = (total - (fs.data_lba - part)) / fs.spc;
    fs.free_hint     = 2;
    fs.volume_id     = b->volume_id;
    memcpy(fs.label, b->label, 11);
    for (int i = 10; i >= 0 && fs.label[i] == ' '; i--)
        fs.label[i] = 0;

    vfs_node_t *root = kmalloc(sizeof(vfs_node_t));
    if (!root)
        return VFS_ENOMEM;
    memset(root, 0, sizeof(*root));
    root->type    = VFS_DIR;
    root->fs_name = "fat32";
    root->ops     = &fat_ops;
    root->inode   = next_inode++;
    root->fat.cluster = fs.root_cluster;
    rtc_read(&root->ctime);
    root->mtime = root->ctime;

    fs.root = root;
    fs.mounted = true;
    *out = root;
    return VFS_OK;
}

void fat32_stats(fat32_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!fs.mounted)
        return;
    uint32_t f = irq_save();
    st->mounted        = true;
    st->volume_id      = fs.volume_id;
    st->total_clusters = fs.cluster_count;
    st->cluster_bytes  = fs.cluster_bytes;
    st->fat_sectors    = fs.fat_size;
    st->partition_lba  = fs.part_lba;
    strcpy(st->label, fs.label);
    for (uint32_t c = 2; c < fs.cluster_count + 2; c++)    /* bo'sh klasterlarni sanash */
        if (fat_get(c) == 0)
            st->free_clusters++;
    irq_restore(f);
}

/* ------------------------------- Formatlash ------------------------------- */

/*
 * Butun diskni FAT32 da formatlash (bo'limlarsiz, "superfloppy" — USB
 * fleshkalardagi kabi). Klaster = 1 sektor (512 bayt): FAT32 uchun kamida
 * 65525 klaster kerak, ya'ni disk kamida ~33 MB bo'lishi kerak.
 */
int fat32_mkfs(const char *label)
{
    const ata_info_t *ai = ata_info();
    if (!ai->present)
        return VFS_ENOENT;
    if (fs.mounted)
        return VFS_EBUSY;

    uint32_t total = ai->sectors;
    uint32_t spc = total <= 532480 ? 1 : (total <= 16777216 ? 8 : 16);   /* 260 MB / 8 GB */
    uint32_t reserved = 32, fats = 2;
    uint32_t clusters_est = (total - reserved) / spc;
    uint32_t fat_size = (clusters_est * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE + 1;
    uint32_t data_sectors = total - reserved - fats * fat_size;
    uint32_t clusters = data_sectors / spc;
    if (clusters < 65525)
        return VFS_ENOSPC;                     /* FAT32 uchun disk juda kichik */

    uint32_t f = irq_save();
    uint8_t sec[SECTOR_SIZE];
    int err = VFS_OK;

    /* 1. Boot sektor (BPB) */
    memset(sec, 0, sizeof(sec));
    fat_bpb_t *b = (fat_bpb_t *)sec;
    b->jmp[0] = 0xEB; b->jmp[1] = 0x58; b->jmp[2] = 0x90;
    memcpy(b->oem, "DIYOROS ", 8);
    b->bytes_per_sector    = SECTOR_SIZE;
    b->sectors_per_cluster = (uint8_t)spc;
    b->reserved_sectors    = (uint16_t)reserved;
    b->fat_count           = (uint8_t)fats;
    b->media               = 0xF8;
    b->sectors_per_track   = 32;
    b->heads               = 64;
    b->total32             = total;
    b->fat_size32          = fat_size;
    b->root_cluster        = 2;
    b->fsinfo_sector       = 1;
    b->backup_boot         = 6;
    b->drive               = 0x80;
    b->boot_sig            = 0x29;
    rtc_time_t t;
    rtc_read(&t);
    b->volume_id = ((uint32_t)t.year << 20) ^ ((uint32_t)t.month << 16) ^
                   ((uint32_t)t.day << 11) ^ ((uint32_t)t.hour << 6) ^ t.minute ^ (t.second << 24);
    char lab[11];
    memset(lab, ' ', 11);
    for (int i = 0; i < 11 && label && label[i]; i++)
        lab[i] = upper(label[i]);
    memcpy(b->label, lab, 11);
    memcpy(b->fs_type, "FAT32   ", 8);
    /* Bu diskdan kompyuter yuklanmaydi: boot kodi o'rniga oddiy xabar */
    const char *msg = "DiyorOS FAT32 disk. Yuklanadigan emas.";
    memcpy(sec + 90, msg, strlen(msg));
    sec[510] = 0x55;
    sec[511] = 0xAA;
    if (!ata_write(0, 1, sec) || !ata_write(6, 1, sec))
        err = VFS_EIO;

    /* 2. FSInfo: bo'sh klasterlar soni "noma'lum" (har bir tizim o'zi sanaydi) */
    memset(sec, 0, sizeof(sec));
    *(uint32_t *)(sec + 0)   = 0x41615252;
    *(uint32_t *)(sec + 484) = 0x61417272;
    *(uint32_t *)(sec + 488) = 0xFFFFFFFF;
    *(uint32_t *)(sec + 492) = 0xFFFFFFFF;
    *(uint32_t *)(sec + 508) = 0xAA550000;
    if (!err && (!ata_write(1, 1, sec) || !ata_write(7, 1, sec)))
        err = VFS_EIO;

    /* 3. Qolgan zaxira sektorlar va ikkala FAT ni tozalash */
    uint8_t *zero = kmalloc(SECTOR_SIZE * 64);
    if (!zero)
        err = VFS_ENOMEM;
    if (!err) {
        memset(zero, 0, SECTOR_SIZE * 64);
        for (uint32_t s = 2; s < reserved && !err; s++)
            if (s != 6 && s != 7 && !ata_write(s, 1, zero))
                err = VFS_EIO;
        for (uint32_t s = 0; s < fats * fat_size && !err; s += 64) {
            uint32_t n = fats * fat_size - s < 64 ? fats * fat_size - s : 64;
            if (!ata_write(reserved + s, n, zero))
                err = VFS_EIO;
        }
    }

    /* 4. FAT ning birinchi yozuvlari: 0 va 1 — maxsus, 2 — ildiz papka (zanjir oxiri) */
    if (!err) {
        memset(sec, 0, sizeof(sec));
        ((uint32_t *)sec)[0] = 0x0FFFFFF8;
        ((uint32_t *)sec)[1] = 0x0FFFFFFF;
        ((uint32_t *)sec)[2] = 0x0FFFFFFF;
        for (uint32_t i = 0; i < fats && !err; i++)
            if (!ata_write(reserved + i * fat_size, 1, sec))
                err = VFS_EIO;
    }

    /* 5. Ildiz papka: tozalangan klaster + disk nomi yozuvi */
    uint32_t data_lba = reserved + fats * fat_size;
    if (!err) {
        for (uint32_t i = 0; i < spc && !err; i++)
            if (!ata_write(data_lba + i, 1, zero))
                err = VFS_EIO;
        fat_dirent_t *v = (fat_dirent_t *)sec;
        memset(sec, 0, sizeof(sec));
        memcpy(v->name, lab, 11);
        v->attr = ATTR_VOLUME;
        uint32_t now = fat_now();
        v->mdate = (uint16_t)(now >> 16);
        v->mtime = (uint16_t)now;
        if (!err && !ata_write(data_lba, 1, sec))
            err = VFS_EIO;
    }

    kfree(zero);
    irq_restore(f);
    return err;
}

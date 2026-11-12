/*
 * rootfs.c — yuklanishda fayl tizimini tayyorlash.
 *
 * ramfs bo'sh holda yaratiladi, shuning uchun har yuklanishda standart
 * papkalar va bir nechta fayl yaratamiz (Linux dagi tuzilishga o'xshash):
 *
 *   /bin            dasturlar (v0.5: kernel ichidagi initrd dan nusxalanadi)
 *   /etc            sozlamalar: motd, os-release
 *   /home/diyorbek  foydalanuvchi papkasi (shell shu yerda boshlanadi)
 *   /proc           procfs ulanadi
 *   /tmp            vaqtinchalik fayllar
 *   /disk           qattiq disk (FAT32) — o'chirib-yoqqanda ham saqlanadi (v0.6)
 */
#include "vfs.h"
#include "string.h"
#include "kprintf.h"
#include "config.h"
#include "process.h"
#include "ata.h"

static void write_file(const char *path, const char *text)
{
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0)
        return;
    vfs_write(fd, text, strlen(text));
    vfs_close(fd);
}

void rootfs_init(void)
{
    vfs_init();

    vfs_mkdir("/bin");
    vfs_mkdir("/etc");
    vfs_mkdir("/home");
    vfs_mkdir("/home/diyorbek");
    vfs_mkdir("/proc");
    vfs_mkdir("/tmp");
    vfs_mkmountpoint("/disk");

    vfs_mount("/proc", procfs_create_root());
    initrd_install("/bin");

    char buf[256];
    ksnprintf(buf, sizeof(buf),
              "NAME=%s\nVERSION=%s\nARCH=%s\nAUTHOR=%s\n",
              OS_NAME, OS_VERSION, OS_ARCH, OS_AUTHOR);
    write_file("/etc/os-release", buf);

    write_file("/etc/motd",
               "DiyorOS ga xush kelibsiz!\n"
               "/disk dagi fayllar diskda saqlanadi, qolganlari xotirada (ramfs).\n");

    write_file("/home/diyorbek/salom.txt",
               "Salom! Bu DiyorOS dagi birinchi fayl.\n"
               "Uni tahrirlash uchun: edit salom.txt\n"
               "Dasturlar: ls /bin, keyin nomini yozing (masalan: hello)\n");

    write_file("/home/diyorbek/rejalar.txt",
               "DiyorOS rejasi\n"
               "==============\n"
               "v0.1  yuklanish, uzilishlar, drayverlar, shell     [tayyor]\n"
               "v0.2  xotira: PMM, paging, kmalloc/kfree           [tayyor]\n"
               "v0.3  multitasking: scheduler, ps, kill            [tayyor]\n"
               "v0.4  fayl tizimi: VFS, ramfs, procfs, muharrir    [tayyor]\n"
               "v0.5  user mode, syscall, ELF dasturlar            [tayyor]\n"
               "v0.6  disk drayveri, FAT32                         [tayyor]\n"
               "v0.7  grafika\n"
               "v0.8  sinxronizatsiya: mutex, semafor, pipe\n"
               "v1.0  diplom ishi\n");

    vfs_chdir("/home/diyorbek");
}

/* Diskni (FAT32) /disk ga ulash. Natija: VFS kodi */
int disk_mount(void)
{
    vfs_node_t *root;
    int err = fat32_mount(&root);
    if (err)
        return err;
    err = vfs_mount("/disk", root);
    if (err)
        root->ops->unmount(root);
    return err;
}

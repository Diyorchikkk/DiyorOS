/*
 * pci.c — PCI shinasini skanerlash (v0.7).
 *
 * Zamonaviy kompyuterda videokarta, disk kontrolleri, tarmoq kartasi kabi
 * qurilmalar PCI shinasiga ulanadi. Har bir qurilmaning 256 baytlik
 * "konfiguratsiya maydoni" bor: unda kim ishlab chiqargani (vendor),
 * qanday qurilma ekani (device, class) va qaysi manzillardan foydalanishi
 * (BAR — Base Address Register) yozilgan.
 *
 * Bu maydonni o'qish uchun ikki port ishlatiladi:
 *   0xCF8 — manzil: 1 (yoqish) | shina (8 bit) | slot (5 bit) | funksiya (3 bit) | registr
 *   0xCFC — ma'lumot (32 bit)
 *
 * Bizga hozir PCI kerak, chunki videokartaning framebuffer xotirasi
 * (ekran piksellari) qayerda turganini uning BAR0 registri aytadi.
 * Kelajakda tarmoq kartasi (e1000) ham shu yo'l bilan topiladi.
 */
#include "pci.h"
#include "io.h"
#include "klog.h"

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

static pci_device_t devices[PCI_MAX_DEVICES];
static int device_count = 0;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_ADDRESS, address);
    return inl(PCI_DATA);
}

static void add_device(uint8_t bus, uint8_t slot, uint8_t func, uint32_t id)
{
    if (device_count >= PCI_MAX_DEVICES)
        return;
    pci_device_t *d = &devices[device_count++];
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = (uint16_t)(id & 0xFFFF);
    d->device = (uint16_t)(id >> 16);
    uint32_t cls = pci_read32(bus, slot, func, 0x08);
    d->class_code = (uint8_t)(cls >> 24);
    d->subclass   = (uint8_t)(cls >> 16);
    d->prog_if    = (uint8_t)(cls >> 8);
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
}

void pci_init(void)
{
    device_count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;                          /* bu joyda qurilma yo'q */
            add_device((uint8_t)bus, (uint8_t)slot, 0, id);

            /* Header type ning 7-biti: qurilma bir nechta funksiyali (masalan, chipset) */
            uint32_t header = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            if (!(header & 0x00800000))
                continue;
            for (int func = 1; func < 8; func++) {
                id = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0);
                if ((id & 0xFFFF) != 0xFFFF)
                    add_device((uint8_t)bus, (uint8_t)slot, (uint8_t)func, id);
            }
        }
    }
    for (int i = 0; i < device_count; i++)
        klog("pci: %02x:%02x.%u %04x:%04x %s", devices[i].bus, devices[i].slot, devices[i].func,
             devices[i].vendor, devices[i].device,
             pci_class_name(devices[i].class_code, devices[i].subclass));
}

int pci_count(void)
{
    return device_count;
}

const pci_device_t *pci_get(int i)
{
    return (i >= 0 && i < device_count) ? &devices[i] : 0;
}

const pci_device_t *pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < device_count; i++)
        if (devices[i].vendor == vendor && devices[i].device == device)
            return &devices[i];
    return 0;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x01:
        if (subclass == 0x01) return "IDE disk kontrolleri";
        if (subclass == 0x06) return "SATA disk kontrolleri";
        return "Disk kontrolleri";
    case 0x02: return subclass == 0x00 ? "Ethernet tarmoq kartasi" : "Tarmoq kontrolleri";
    case 0x03: return subclass == 0x00 ? "VGA videokarta" : "Videokarta";
    case 0x04: return "Multimedia (ovoz)";
    case 0x05: return "Xotira kontrolleri";
    case 0x06:
        if (subclass == 0x00) return "Host bridge (protsessor <-> PCI)";
        if (subclass == 0x01) return "ISA bridge (eski qurilmalar)";
        if (subclass == 0x04) return "PCI-PCI bridge";
        return "Bridge (ko'prik)";
    case 0x07: return "Aloqa kontrolleri";
    case 0x08: return "Tizim qurilmasi";
    case 0x0C:
        if (subclass == 0x03) return "USB kontrolleri";
        if (subclass == 0x05) return "SMBus kontrolleri";
        return "Shina kontrolleri";
    default:   return "Boshqa qurilma";
    }
}

const char *pci_vendor_name(uint16_t vendor)
{
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1234: return "QEMU/Bochs";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1022: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "Realtek";
    case 0x15AD: return "VMware";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x106B: return "Apple";
    default:     return "noma'lum";
    }
}

/*
 * pci.h — PCI shinasi: kompyuterdagi qurilmalarni topish (v0.7).
 */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 32

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if;
    uint32_t bar[6];          /* Base Address Register lar: qurilma xotirasi yoki portlari */
} pci_device_t;

void                pci_init(void);
int                 pci_count(void);
const pci_device_t *pci_get(int i);
const pci_device_t *pci_find(uint16_t vendor, uint16_t device);
const char         *pci_class_name(uint8_t class_code, uint8_t subclass);
const char         *pci_vendor_name(uint16_t vendor);
uint32_t            pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void                pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
void                pci_enable_dma(const pci_device_t *d);   /* v0.9: MMIO + bus master */

#endif

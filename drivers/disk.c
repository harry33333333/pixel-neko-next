#include "disk.h"
#include "port.h"

#define ATA_CMD_READ        0x20
#define ATA_CMD_WRITE       0x30
#define ATA_CMD_READ_EXT    0x24
#define ATA_CMD_WRITE_EXT   0x34
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_TIMEOUT  1000000

static int grub_mode = 1;
disk_drive_t g_drives[MAX_DRIVES];

void disk_set_grub_mode(int mode) { grub_mode = mode; }
int disk_get_grub_mode(void) { return grub_mode; }

static int ata_wait_ready(unsigned int port)
{
    int timeout = ATA_TIMEOUT;
    while (timeout > 0) {
        unsigned char status = inb(port + 7);
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRDY) != 0) return 0;
        timeout--;
    }
    return -1;
}

static int ata_wait_drq(unsigned int port)
{
    int timeout = ATA_TIMEOUT;
    while (timeout > 0) {
        unsigned char status = inb(port + 7);
        if ((status & ATA_SR_BSY) == 0) {
            if (status & ATA_SR_ERR) return -2;
            if (status & ATA_SR_DRQ) return 0;
        }
        timeout--;
    }
    return -1;
}

static void ata_select_drive(unsigned int port, int drive_idx, unsigned int lba)
{
    outb(port + 6, 0xE0 | (drive_idx << 4) | ((lba >> 24) & 0x0F));
    inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);
}

int disk_detect_all(void)
{
    for (int i=0; i<MAX_DRIVES; i++) g_drives[i].present = 0;
    
    unsigned int ports[2] = { 0x1F0, 0x170 };
    int drive_count = 0;
    
    for (int p=0; p<2; p++) {
        for (int d=0; d<2; d++) {
            if (drive_count >= MAX_DRIVES) break;
            unsigned int port = ports[p];
            outb(port + 6, 0xA0 | (d << 4));
            inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);
            
            outb(port + 2, 0);
            outb(port + 3, 0);
            outb(port + 4, 0);
            outb(port + 5, 0);
            outb(port + 7, ATA_CMD_IDENTIFY);
            
            unsigned char status = inb(port + 7);
            if (status == 0) continue; // No device
            
            int is_atapi = 0;
            while (1) {
                status = inb(port + 7);
                if ((status & ATA_SR_ERR) != 0) {
                    // Could be ATAPI
                    unsigned char cl = inb(port + 4);
                    unsigned char ch = inb(port + 5);
                    if (cl == 0x14 && ch == 0xEB) {
                        is_atapi = 1;
                        outb(port + 7, ATA_CMD_IDENTIFY_PACKET);
                        break;
                    } else {
                        break;
                    }
                }
                if ((status & ATA_SR_DRQ) != 0) break;
            }
            
            if (status & ATA_SR_ERR && !is_atapi) continue;
            
            if (is_atapi || ata_wait_drq(port) == 0) {
                unsigned short buf[256];
                for (int i=0; i<256; i++) buf[i] = inw(port + 0);
                
                g_drives[drive_count].present = 1;
                g_drives[drive_count].is_atapi = is_atapi;
                g_drives[drive_count].base_port = port;
                g_drives[drive_count].drive_idx = d;
                g_drives[drive_count].lba_offset = 0;
                
                if (is_atapi) {
                    g_drives[drive_count].total_sectors = 0; // Unknown for now
                } else {
                    unsigned int sectors = (unsigned int)buf[60] | ((unsigned int)buf[61] << 16);
                    if (sectors == 0) sectors = (unsigned int)buf[100] | ((unsigned int)buf[101] << 16); // LBA48
                    g_drives[drive_count].total_sectors = sectors;
                }
                drive_count++;
            }
        }
    }
    return drive_count;
}

int disk_read_drive(int drive, unsigned int lba, unsigned char* buf)
{
    if (drive < 0 || drive >= MAX_DRIVES) return -1;
    disk_drive_t* d = &g_drives[drive];
    if (!d->present) return -1;
    
    unsigned int port = d->base_port;
    if (d->is_atapi) {
        // ATAPI READ 12
        outb(port + 6, (d->drive_idx << 4));
        inb(port + 7); inb(port + 7); inb(port + 7); inb(port + 7);
        outb(port + 1, 0); // Feature
        outb(port + 4, 2048 & 0xFF); // Byte count limit
        outb(port + 5, 2048 >> 8);
        outb(port + 7, 0xA0); // PACKET
        if (ata_wait_drq(port) != 0) return -1;
        
        unsigned char packet[12] = {0};
        packet[0] = 0xA8; // READ(12)
        packet[2] = (lba >> 24) & 0xFF;
        packet[3] = (lba >> 16) & 0xFF;
        packet[4] = (lba >> 8) & 0xFF;
        packet[5] = (lba & 0xFF);
        packet[9] = 1; // 1 sector (2048 bytes)
        
        unsigned short* ptr = (unsigned short*)packet;
        for (int i=0; i<6; i++) outw(port + 0, ptr[i]);
        
        if (ata_wait_drq(port) != 0) return -1;
        
        unsigned short* buf16 = (unsigned short*)buf;
        for (int i=0; i<1024; i++) {
            buf16[i] = inw(port + 0);
        }
        
        while ((inb(port + 7) & ATA_SR_BSY) != 0);
        return 0;
    }
    
    // Normal ATA
    lba += d->lba_offset;
    ata_select_drive(port, d->drive_idx, lba);
    if (ata_wait_ready(port) != 0) return -1;
    
    outb(port + 2, 1);
    outb(port + 3, (unsigned char)(lba & 0xFF));
    outb(port + 4, (unsigned char)((lba >> 8) & 0xFF));
    outb(port + 5, (unsigned char)((lba >> 16) & 0xFF));
    outb(port + 7, ATA_CMD_READ);
    
    if (ata_wait_drq(port) != 0) return -1;
    
    unsigned short* buf16 = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) {
        buf16[i] = inw(port + 0);
    }
    return 0;
}

int disk_write_drive(int drive, unsigned int lba, const unsigned char* buf)
{
    if (drive < 0 || drive >= MAX_DRIVES) return -1;
    disk_drive_t* d = &g_drives[drive];
    if (!d->present || d->is_atapi) return -1;
    
    unsigned int port = d->base_port;
    lba += d->lba_offset;
    ata_select_drive(port, d->drive_idx, lba);
    if (ata_wait_ready(port) != 0) return -1;
    
    outb(port + 2, 1);
    outb(port + 3, (unsigned char)(lba & 0xFF));
    outb(port + 4, (unsigned char)((lba >> 8) & 0xFF));
    outb(port + 5, (unsigned char)((lba >> 16) & 0xFF));
    outb(port + 7, ATA_CMD_WRITE);
    
    if (ata_wait_drq(port) != 0) return -1;
    
    unsigned short* buf16 = (unsigned short*)buf;
    for (int i = 0; i < 256; i++) {
        outw(port + 0, buf16[i]);
    }
    
    outb(port + 7, ATA_CMD_CACHE_FLUSH);
    if (ata_wait_ready(port) != 0) return -1;
    return 0;
}

// Backwards compat: use drive 0
int disk_detect(void) { return disk_detect_all() > 0 ? 0 : -1; }
int disk_read(unsigned int lba, unsigned char* buf) { return disk_read_drive(0, lba, buf); }
int disk_write(unsigned int lba, const unsigned char* buf) { return disk_write_drive(0, lba, buf); }
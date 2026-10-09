#include "mm.h"

unsigned int total_ram_mb = 0;
unsigned int usable_ram_mb = 0;
char cpu_model_string[49] = "Unknown CPU";

#define PAGE_SIZE 4096
#define MAX_PAGES (0x100000000ULL / PAGE_SIZE)
static unsigned char pmm_bitmap[MAX_PAGES / 8];
static unsigned int next_free_page = 0;

struct multiboot_mmap_entry {
    unsigned int size;
    unsigned int addr_low;
    unsigned int addr_high;
    unsigned int len_low;
    unsigned int len_high;
    unsigned int type;
} __attribute__((packed));

static inline int has_cpuid(void) {
    unsigned int flags1, flags2;
    asm volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x200000, %1\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        : "=r"(flags1), "=r"(flags2)
    );
    return ((flags1 ^ flags2) & 0x200000) != 0;
}

static inline void cpuid(unsigned int leaf, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx) {
    asm volatile ("cpuid"
                  : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
                  : "a" (leaf));
}

static int str_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void str_copy_n(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void detect_cpu(void) {
    if (!has_cpuid()) {
        str_copy_n(cpu_model_string, "Classic x86 CPU", sizeof(cpu_model_string));
        return;
    }

    unsigned int eax, ebx, ecx, edx;
    unsigned int max_basic;
    cpuid(0, &max_basic, &ebx, &ecx, &edx);

    char vendor[13];
    *(unsigned int*)&vendor[0] = ebx;
    *(unsigned int*)&vendor[4] = edx;
    *(unsigned int*)&vendor[8] = ecx;
    vendor[12] = '\0';

    // Check for extended brand string (leaf 0x80000000)
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004 && eax <= 0x80000020) {
        char raw_brand[49];
        unsigned int* p = (unsigned int*)raw_brand;
        for (int i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, &eax, &ebx, &ecx, &edx);
            *p++ = eax; *p++ = ebx; *p++ = ecx; *p++ = edx;
        }
        raw_brand[48] = '\0';

        // Trim leading spaces
        int start = 0;
        while (raw_brand[start] == ' ') start++;
        if (raw_brand[start] != '\0') {
            str_copy_n(cpu_model_string, &raw_brand[start], sizeof(cpu_model_string));
            return;
        }
    }

    // Fallback: decode Family / Model / Stepping for older CPUs (Pentium Pro/II/III, K6, Cyrix, VIA, IDT)
    if (max_basic >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        unsigned int model = (eax >> 4) & 0x0F;
        unsigned int family = (eax >> 8) & 0x0F;
        unsigned int ext_model = (eax >> 16) & 0x0F;
        unsigned int ext_family = (eax >> 20) & 0xFF;

        if (family == 6 || family == 15) {
            model |= (ext_model << 4);
        }
        if (family == 15) {
            family += ext_family;
        }

        if (str_equal(vendor, "GenuineIntel")) {
            if (family == 6) {
                switch (model) {
                    case 1:  str_copy_n(cpu_model_string, "Intel Pentium Pro", sizeof(cpu_model_string)); return;
                    case 3:  str_copy_n(cpu_model_string, "Intel Pentium II (Klamath)", sizeof(cpu_model_string)); return;
                    case 5:  str_copy_n(cpu_model_string, "Intel Pentium II / Celeron (Deschutes)", sizeof(cpu_model_string)); return;
                    case 6:  str_copy_n(cpu_model_string, "Intel Celeron (Mendocino)", sizeof(cpu_model_string)); return;
                    case 7:  str_copy_n(cpu_model_string, "Intel Pentium III (Katmai)", sizeof(cpu_model_string)); return;
                    case 8:  str_copy_n(cpu_model_string, "Intel Pentium III (Coppermine)", sizeof(cpu_model_string)); return;
                    case 9:  str_copy_n(cpu_model_string, "Intel Pentium M (Banias)", sizeof(cpu_model_string)); return;
                    case 10: str_copy_n(cpu_model_string, "Intel Pentium III Xeon", sizeof(cpu_model_string)); return;
                    case 11: str_copy_n(cpu_model_string, "Intel Pentium III (Tualatin)", sizeof(cpu_model_string)); return;
                    default: str_copy_n(cpu_model_string, "Intel P6 Family CPU", sizeof(cpu_model_string)); return;
                }
            } else if (family == 5) {
                if (model == 4) str_copy_n(cpu_model_string, "Intel Pentium MMX", sizeof(cpu_model_string));
                else str_copy_n(cpu_model_string, "Intel Pentium (P5)", sizeof(cpu_model_string));
                return;
            } else if (family == 15) {
                str_copy_n(cpu_model_string, "Intel Pentium 4", sizeof(cpu_model_string));
                return;
            }
        } else if (str_equal(vendor, "AuthenticAMD")) {
            if (family == 5) {
                if (model <= 3) str_copy_n(cpu_model_string, "AMD K5", sizeof(cpu_model_string));
                else if (model == 6 || model == 7) str_copy_n(cpu_model_string, "AMD K6", sizeof(cpu_model_string));
                else if (model == 8) str_copy_n(cpu_model_string, "AMD K6-2", sizeof(cpu_model_string));
                else str_copy_n(cpu_model_string, "AMD K6-III", sizeof(cpu_model_string));
                return;
            } else if (family == 6) {
                if (model <= 3) str_copy_n(cpu_model_string, "AMD Athlon / Duron", sizeof(cpu_model_string));
                else if (model == 8 || model == 10) str_copy_n(cpu_model_string, "AMD Athlon XP", sizeof(cpu_model_string));
                else str_copy_n(cpu_model_string, "AMD Athlon", sizeof(cpu_model_string));
                return;
            } else if (family == 15) {
                str_copy_n(cpu_model_string, "AMD K8 (Athlon 64)", sizeof(cpu_model_string));
                return;
            }
        } else if (str_equal(vendor, "CentaurHauls") || str_equal(vendor, "VIA VIA VIA ")) {
            if (family == 5) {
                if (model == 4) str_copy_n(cpu_model_string, "IDT WinChip C6", sizeof(cpu_model_string));
                else if (model == 8) str_copy_n(cpu_model_string, "IDT WinChip 2", sizeof(cpu_model_string));
                else if (model == 9) str_copy_n(cpu_model_string, "IDT WinChip 3", sizeof(cpu_model_string));
                else str_copy_n(cpu_model_string, "IDT WinChip", sizeof(cpu_model_string));
                return;
            } else if (family == 6) {
                if (model == 6 || model == 7) str_copy_n(cpu_model_string, "VIA Cyrix III / C3 (Samuel)", sizeof(cpu_model_string));
                else if (model == 8) str_copy_n(cpu_model_string, "VIA C3 (Ezra)", sizeof(cpu_model_string));
                else if (model == 9) str_copy_n(cpu_model_string, "VIA C3 (Nehemiah)", sizeof(cpu_model_string));
                else str_copy_n(cpu_model_string, "VIA C3 Processor", sizeof(cpu_model_string));
                return;
            }
        } else if (str_equal(vendor, "CyrixInstead")) {
            if (family == 5) {
                if (model == 4) str_copy_n(cpu_model_string, "Cyrix MediaGX", sizeof(cpu_model_string));
                else str_copy_n(cpu_model_string, "Cyrix 6x86", sizeof(cpu_model_string));
                return;
            } else if (family == 6) {
                str_copy_n(cpu_model_string, "Cyrix 6x86MX / MII", sizeof(cpu_model_string));
                return;
            }
        } else if (str_equal(vendor, "RiseRiseRise")) {
            str_copy_n(cpu_model_string, "Rise mP6", sizeof(cpu_model_string));
            return;
        } else if (str_equal(vendor, "TransmetaCPU")) {
            str_copy_n(cpu_model_string, "Transmeta Crusoe", sizeof(cpu_model_string));
            return;
        }

        // Generic fallback with vendor
        str_copy_n(cpu_model_string, vendor, sizeof(cpu_model_string));
        return;
    }

    str_copy_n(cpu_model_string, vendor, sizeof(cpu_model_string));
}

void pmm_init(void) {
    // 1. Get CPU string
    detect_cpu();
    
    // 2. Initialize bitmap: mark all as used initially
    for (unsigned int i = 0; i < sizeof(pmm_bitmap); i++) {
        pmm_bitmap[i] = 0xFF;
    }
    
    // 3. Parse multiboot info
    unsigned int mbi_addr = *(unsigned int*)0x7F64;
    if (mbi_addr == 0) return; // Fallback
    
    unsigned int flags = *(unsigned int*)mbi_addr;
    
    if (flags & (1 << 0)) {
        unsigned int mem_lower = *(unsigned int*)(mbi_addr + 4);
        unsigned int mem_upper = *(unsigned int*)(mbi_addr + 8);
        total_ram_mb = (mem_lower + mem_upper) / 1024 + 1;
    }
    
    if (flags & (1 << 6)) {
        unsigned int mmap_length = *(unsigned int*)(mbi_addr + 44);
        unsigned int mmap_addr = *(unsigned int*)(mbi_addr + 48);
        
        struct multiboot_mmap_entry* entry = (struct multiboot_mmap_entry*)mmap_addr;
        unsigned int usable_bytes = 0;
        
        while ((unsigned int)entry < mmap_addr + mmap_length) {
            if (entry->type == 1) { // 1 = Available RAM
                unsigned long long start = ((unsigned long long)entry->addr_high << 32) | entry->addr_low;
                unsigned long long len = ((unsigned long long)entry->len_high << 32) | entry->len_low;
                
                // Clamp to 4GB for 32-bit system without PAE
                if (start >= 0x100000000ULL) {
                    entry = (struct multiboot_mmap_entry*)((unsigned int)entry + entry->size + 4);
                    continue;
                }
                if (start + len > 0x100000000ULL) {
                    len = 0x100000000ULL - start;
                }
                
                usable_bytes += (unsigned int)len;
                
                unsigned int start_page = (unsigned int)(start / PAGE_SIZE);
                unsigned int end_page = (unsigned int)((start + len) / PAGE_SIZE);
                
                for (unsigned int p = start_page; p < end_page && p < MAX_PAGES; p++) {
                    pmm_bitmap[p / 8] &= ~(1 << (p % 8));
                }
            }
            entry = (struct multiboot_mmap_entry*)((unsigned int)entry + entry->size + 4);
        }
        usable_ram_mb = usable_bytes / (1024 * 1024);
        
        // Let's also sync total_ram_mb to be more accurate if usable is around the same
        if (usable_ram_mb > total_ram_mb) {
            total_ram_mb = usable_ram_mb;
        }
    }
    
    // Reserve first 4MB for safety (kernel, vbe, etc)
    for (unsigned int p = 0; p < (0x400000 / PAGE_SIZE); p++) {
        pmm_bitmap[p / 8] |= (1 << (p % 8));
    }
}

void* pmm_alloc_page(void) {
    for (unsigned int i = next_free_page; i < MAX_PAGES; i++) {
        if (pmm_bitmap[i / 8] != 0xFF) {
            if ((pmm_bitmap[i / 8] & (1 << (i % 8))) == 0) {
                pmm_bitmap[i / 8] |= (1 << (i % 8));
                next_free_page = i + 1;
                return (void*)(i * PAGE_SIZE);
            }
        }
    }
    return 0; // Out of memory
}

void pmm_free_page(void* ptr) {
    unsigned int p = (unsigned int)ptr / PAGE_SIZE;
    if (p < MAX_PAGES) {
        pmm_bitmap[p / 8] &= ~(1 << (p % 8));
        if (p < next_free_page) {
            next_free_page = p;
        }
    }
}

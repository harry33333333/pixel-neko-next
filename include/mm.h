#ifndef MM_H
#define MM_H

extern unsigned int total_ram_mb;
extern unsigned int usable_ram_mb;
extern char cpu_model_string[49];

void pmm_init(void);

// Basic PMM API
void* pmm_alloc_page(void);
void pmm_free_page(void* ptr);

#endif // MM_H

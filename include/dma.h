#ifndef DMA_H
#define DMA_H

typedef struct {
    union {
        void* virt;
        void* virt_addr;
    };
    union {
        unsigned int phys;
        unsigned int phys_addr;
    };
    unsigned int bytes;
    unsigned int pages;
    unsigned int alignment;
    int owned;
} dma_buffer_t;

dma_buffer_t dma_alloc_aligned(unsigned int bytes, unsigned int alignment);
void dma_free(dma_buffer_t* buffer);
void dma_flush(const dma_buffer_t* buffer);
int dma_is_valid(const dma_buffer_t* buffer);
void dma_sync_for_cpu(dma_buffer_t* buf);
void dma_sync_for_device(dma_buffer_t* buf);

#endif

#include "system.h"

#define PAGE_SIZE 4096

uint32_t total_ram_kb = 0;

/* --- PMM --- */
uint32_t pmm_bitmap[32768]; 
uint32_t total_pages = 0;

void pmm_mark_used(uint32_t page) { pmm_bitmap[page / 32] |= (1 << (page % 32)); }
void pmm_mark_free(uint32_t page) { pmm_bitmap[page / 32] &= ~(1 << (page % 32)); }
int pmm_is_used(uint32_t page) { return pmm_bitmap[page / 32] & (1 << (page % 32)); }

void init_pmm(uint32_t ram_kb) {
    total_pages = (ram_kb * 1024) / PAGE_SIZE;
    
    /* Prevent overflow if RAM > 4 GB (bitmap limit) */
    if (total_pages > 32768 * 32) {
        total_pages = 32768 * 32;
    }
    
    for (int i = 0; i < 32768; i++) pmm_bitmap[i] = 0xFFFFFFFF; 
    for (uint32_t i = 0; i < total_pages; i++) pmm_mark_free(i);
    for (uint32_t i = 0; i < 512; i++) pmm_mark_used(i); /* Reserves 2 Mo */
}

void* pmm_alloc_page() {
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!pmm_is_used(i)) {
            pmm_mark_used(i);
            return (void*)(i * PAGE_SIZE);
        }
    }
    return NULL; 
}

void pmm_free_page(void* ptr) { pmm_mark_free((uint32_t)ptr / PAGE_SIZE); }

/* --- PAGING --- */
uint32_t kernel_page_directory[1024] __attribute__((aligned(4096)));
uint32_t first_page_table[4][1024] __attribute__((aligned(4096)));

void init_paging() {
    /* Offline kernel directory initialization in Supervisor mode (0x02) */
    for (int i = 0; i < 1024; i++) kernel_page_directory[i] = 0x02; 
    
    for(int t = 0; t < 4; t++) {
        for (int i = 0; i < 1024; i++) {
            first_page_table[t][i] = ((t * 4194304) + (i * 4096)) | 0x03;
        }
        kernel_page_directory[t] = ((uint32_t)first_page_table[t]) | 0x03;
    }
    
    uint32_t cr0;
    __asm__ volatile("mov %0, %%cr3":: "r"(kernel_page_directory));
    __asm__ volatile("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0":: "r"(cr0));
}

void map_page_pd(uint32_t* pd, uint32_t virt, uint32_t phys) {
    uint32_t pdi = virt >> 22;               
    uint32_t pti = (virt >> 12) & 0x03FF;    

    if (!(pd[pdi] & 0x01)) { 
        uint32_t* new_table = (uint32_t*)pmm_alloc_page();
        for (int i = 0; i < 1024; i++) new_table[i] = 0;
        /* Intermediate user tables must carry the 0x07 flag */
        pd[pdi] = ((uint32_t)new_table) | 0x07;
    }

    uint32_t* table = (uint32_t*)(pd[pdi] & 0xFFFFF000);
    table[pti] = (phys & 0xFFFFF000) | 0x07;

    __asm__ volatile("invlpg (%0)" ::"r" (virt) : "memory");
}

void map_page(uint32_t virt, uint32_t phys) {
    map_page_pd(kernel_page_directory, virt, phys);
}

/* --- HEAP --- */
/* Addition of an integrity signature Magic Number */
#define HEAP_MAGIC 0xDEADBEEF

typedef struct heap_block {
    uint32_t magic;
    size_t size; 
    uint8_t is_free; 
    struct heap_block *next;
} heap_block_t;

heap_block_t *heap_head = NULL;

void init_heap() {
    uint32_t heap_start = 0x10000000; /* Safe kernel virtual address (256 MB mark) */
    uint32_t heap_size = 1024 * 1024 * 4; /* 4 MB Heap */
    
    /* Dynamically allocate and map the physical pages to our virtual heap */
    for (uint32_t i = 0; i < heap_size; i += 4096) {
        void* phys = pmm_alloc_page();
        map_page(heap_start + i, (uint32_t)phys);
    }
    
    heap_head = (heap_block_t*)heap_start;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = heap_size - sizeof(heap_block_t);
    heap_head->is_free = 1; 
    heap_head->next = NULL;
}

void* kmalloc(size_t size) {
    __asm__ volatile("cli"); /* Disable interrupts for safety */
    heap_block_t *curr = heap_head;
    while (curr != NULL) {
        if (curr->magic != HEAP_MAGIC) {
            kprintf("\n[KERNEL PANIC] Heap corruption detected in kmalloc! Block at %x has bad magic %x\n", curr, curr->magic);
            while(1) { __asm__ volatile("cli; hlt"); }
        }

        if (curr->is_free && curr->size >= size) {
            if (curr->size > size + sizeof(heap_block_t) + 4) {
                heap_block_t *new_block = (heap_block_t*)((uint8_t*)curr + sizeof(heap_block_t) + size);
                new_block->magic = HEAP_MAGIC;
                new_block->is_free = 1; 
                new_block->size = curr->size - size - sizeof(heap_block_t);
                new_block->next = curr->next;
                curr->next = new_block; 
                curr->size = size;
            }
            curr->is_free = 0; 
            __asm__ volatile("sti"); /* Re-enable interrupts before returning */
            return (void*)((uint8_t*)curr + sizeof(heap_block_t));
        }
        curr = curr->next;
    }
    __asm__ volatile("sti"); /* Re-enable interrupts if allocation fails */
    return NULL; 
}

void kfree(void* ptr) {
    if (!ptr) return;
    __asm__ volatile("cli"); /* Disable interrupts for safety */
    heap_block_t *block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    
    if (block->magic != HEAP_MAGIC) {
        kprintf("\n[KERNEL PANIC] Heap corruption detected in kfree! Block at %x has bad magic %x\n", block, block->magic);
        while(1) { __asm__ volatile("cli; hlt"); }
    }

    if (block->is_free) {
        kprintf("\n[WARNING] Double free detected at %x! Ignoring operation.\n", ptr);
        __asm__ volatile("sti");
        return;
    }

    block->is_free = 1;
    if (block->next) {
        if (block->next->magic != HEAP_MAGIC) {
            kprintf("\n[KERNEL PANIC] Heap corruption detected during merge! Block at %x has bad magic %x\n", block->next, block->next->magic);
            while(1) { __asm__ volatile("cli; hlt"); }
        }
        if (block->next->is_free) {
            block->size += sizeof(heap_block_t) + block->next->size;
            block->next = block->next->next;
        }
    }
    __asm__ volatile("sti"); /* Re-enable interrupts when done */
}
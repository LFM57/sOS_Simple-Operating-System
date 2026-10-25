#include "system.h"
#define FONT8x16_IMPLEMENTATION /* Tell the header to instantiate the array here */
#include "font8x16.h"

#define PORT_COM1 0x3f8

/* --- GRAPHICS ENGINE --- */
int is_graphics_mode = 0;
int show_cursor = 1; /* Global flag to toggle the software cursor */
uint32_t* lfb_mem = (uint32_t*)0xE0000000;    /* Physical Video RAM mapped here */
uint32_t* backbuffer = (uint32_t*)0xE1000000; /* RAM Backbuffer mapped here */
uint32_t g_width = 0, g_height = 0, g_pitch = 0, g_bpp = 0;

uint32_t term_fg_color = 0x00FFFFFF; /* Default: White */
uint32_t term_bg_color = 0x00000000; /* Default: Black */

/* Fast 32-bit block copy using x86 string instructions (burst hardware speed) */
static inline void fast_memcpy(void* dest, const void* src, uint32_t dwords) {
    __asm__ volatile (
        "cld\n\t"
        "rep movsl"
        : "+D"(dest), "+S"(src), "+c"(dwords)
        :
        : "memory"
    );
}

/* Fast 32-bit block fill using x86 string instructions */
static inline void fast_memset(void* dest, uint32_t val, uint32_t dwords) {
    __asm__ volatile (
        "cld\n\t"
        "rep stosl"
        : "+D"(dest), "+c"(dwords)
        : "a"(val)
        : "memory"
    );
}

void init_graphics(uint32_t phys_addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    if (!phys_addr) return; /* Bootloader failed to set graphical mode */
    
    g_width = width; g_height = height; 
    g_pitch = pitch; g_bpp = bpp;
    is_graphics_mode = 1;

    /* 1. Map Physical LFB to Virtual 0xE0000000 */
    uint32_t fb_size = height * pitch;
    for (uint32_t i = 0; i < fb_size; i += 4096) {
        map_page(0xE0000000 + i, phys_addr + i);
    }

    /* 2. Allocate Backbuffer directly from Physical Memory Manager */
    for (uint32_t i = 0; i < fb_size; i += 4096) {
        void* page = pmm_alloc_page();
        map_page(0xE1000000 + i, (uint32_t)page);
    }
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!is_graphics_mode || x >= g_width || y >= g_height) return;
    
    uint32_t offset = (y * g_pitch) + (x * (g_bpp / 8));
    *(uint32_t*)((uint8_t*)backbuffer + offset) = color;
    *(uint32_t*)((uint8_t*)lfb_mem + offset) = color; 
}

void swap_buffers(void) {
    if (!is_graphics_mode) return;
    uint32_t total_dwords = (g_height * g_pitch) / 4;
    
    /* Fast block copy from RAM backbuffer to Video memory (completes in ~1 ms) */
    fast_memcpy(lfb_mem, backbuffer, total_dwords);

    update_software_cursor(); 
}

/* Optimized glyph rendering: writes scanlines directly without per-pixel call overhead */
void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode || x + 8 > g_width || y + 16 > g_height) return;

    const unsigned char* glyph = font8x16[(unsigned char)c];
    uint32_t bytes_per_pixel = g_bpp / 8;
    uint32_t row_offset = (y * g_pitch) + (x * bytes_per_pixel);

    for (uint32_t cy = 0; cy < 16; cy++) {
        uint32_t* bb_row = (uint32_t*)((uint8_t*)backbuffer + row_offset);
        uint32_t* lfb_row = (uint32_t*)((uint8_t*)lfb_mem + row_offset);
        unsigned char row_bits = glyph[cy];

        for (uint32_t cx = 0; cx < 8; cx++) {
            uint32_t color = (row_bits & (0x80 >> cx)) ? fg_color : bg_color;
            bb_row[cx] = color;
            lfb_row[cx] = color;
        }
        row_offset += g_pitch;
    }
}

void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode) return;
    
    uint32_t current_x = x;
    while (*str) {
        draw_char(*str, current_x, y, fg_color, bg_color);
        current_x += 8;
        str++;
    }
}

/* Global variables for VGA display and ANSI handling */
uint16_t* video_memory = (uint16_t*)0xB8000;
int cursor_x = 0;
int cursor_y = 0;

static int ansi_state = 0;
static int ansi_val = 0;
static uint16_t vga_attribute = 0x0F00; /* White on black by default */

void scroll(void); 

void update_cursor(int x, int y) {
    if (is_graphics_mode) return; 

    uint16_t pos = y * 80 + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void update_software_cursor(void) {
    if (!is_graphics_mode) return;
    static uint32_t last_cx = 0, last_cy = 0;
    uint32_t bytes_per_pixel = g_bpp / 8;

    /* 1. RESTORE: Restore previous cursor glyph area from clean backbuffer */
    for (int cy = 13; cy < 15; cy++) {
        uint32_t offset = ((last_cy * 16 + cy) * g_pitch) + (last_cx * 8 * bytes_per_pixel);
        if (offset + (8 * bytes_per_pixel) <= g_height * g_pitch) {
            fast_memcpy((uint8_t*)lfb_mem + offset, (uint8_t*)backbuffer + offset, 8);
        }
    }

    if (!show_cursor) return;

    /* 2. DRAW: Draw underline cursor (scanlines 13-14) directly into LFB */
    for (int cy = 13; cy < 15; cy++) {
        uint32_t offset = ((cursor_y * 16 + cy) * g_pitch) + (cursor_x * 8 * bytes_per_pixel);
        if (offset + (8 * bytes_per_pixel) <= g_height * g_pitch) {
            fast_memset((uint8_t*)lfb_mem + offset, term_fg_color, 8);
        }
    }

    last_cx = cursor_x;
    last_cy = cursor_y;
}

void write_vga(char c) {
    if (ansi_state == 0) {
        if (c == '\033') {
            ansi_state = 1;
            return;
        }
        
        if (is_graphics_mode) {
            int term_cols = g_width / 8;
            if (c == '\n') {
                cursor_x = 0;
                cursor_y++;
            } else if (c == '\b') {
                if (cursor_x > 0) {
                    cursor_x--;
                } else if (cursor_y > 0) {
                    cursor_y--; 
                    cursor_x = term_cols - 1;
                }
            } else if (c == '\r') {
                cursor_x = 0;
            } else {
                draw_char(c, cursor_x * 8, cursor_y * 16, term_fg_color, term_bg_color);
                cursor_x++;
                if (cursor_x >= term_cols) { cursor_x = 0; cursor_y++; }
            }
            scroll();
            update_software_cursor(); 
            return;
        }

        /* Classic VGA character display */
        if (c == '\n') { cursor_x = 0; cursor_y++; } 
        else if (c == '\b') {
            if (cursor_x > 0) cursor_x--;
            else if (cursor_y > 0) { cursor_y--; cursor_x = 79; }
        } 
        else if (c == '\r') { cursor_x = 0; } 
        else {
            video_memory[cursor_y * 80 + cursor_x] = (uint16_t)c | vga_attribute;
            cursor_x++;
            if (cursor_x >= 80) { cursor_x = 0; cursor_y++; }
        }
        scroll();
        update_cursor(cursor_x, cursor_y);
    } 
    else if (ansi_state == 1) {
        if (c == '[') { ansi_state = 2; ansi_val = 0; } 
        else { ansi_state = 0; }
    } 
    else if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            ansi_val = ansi_val * 10 + (c - '0');
        } else if (c == ';') {
            ansi_val = 0; 
        } else if (c == 'm') {
            if (ansi_val == 0)       { vga_attribute = 0x0F00; term_fg_color = 0x00FFFFFF; }
            else if (ansi_val == 31) { vga_attribute = 0x0C00; term_fg_color = 0x00FF5555; }
            else if (ansi_val == 32) { vga_attribute = 0x0A00; term_fg_color = 0x0055FF55; }
            else if (ansi_val == 33) { vga_attribute = 0x0E00; term_fg_color = 0x00FFFF55; }
            else if (ansi_val == 34) { vga_attribute = 0x0900; term_fg_color = 0x005555FF; }
            else if (ansi_val == 35) { vga_attribute = 0x0D00; term_fg_color = 0x00FF55FF; }
            else if (ansi_val == 36) { vga_attribute = 0x0B00; term_fg_color = 0x0055FFFF; }
            else if (ansi_val == 37) { vga_attribute = 0x0700; term_fg_color = 0x00AAAAAA; }
            ansi_state = 0;
            ansi_val = 0;
        } else {
            ansi_state = 0;
            ansi_val = 0;
        }
    }
}

/* Instantaneous Scrolling Engine */
void scroll(void) {
    if (is_graphics_mode) {
        int term_rows = g_height / 16;
        if (cursor_y < term_rows) return;

        uint32_t row_bytes = 16 * g_pitch;
        uint32_t lines = cursor_y - term_rows + 1;

        if (lines >= (uint32_t)term_rows) {
            fast_memset(backbuffer, term_bg_color, (g_height * g_pitch) / 4);
            cursor_y = 0;
        } else {
            uint32_t shift_bytes = (term_rows - lines) * row_bytes;
            /* Shift the backbuffer up in RAM */
            fast_memcpy(backbuffer, (uint8_t*)backbuffer + (lines * row_bytes), shift_bytes / 4);
            /* Clear the bottom rows with background color */
            fast_memset((uint8_t*)backbuffer + shift_bytes, term_bg_color, (lines * row_bytes) / 4);
            cursor_y = term_rows - 1;
        }

        /* Fast single-pass flush to VRAM */
        swap_buffers(); 
        return;
    }

    /* Fallback VGA Scroll */
    if (cursor_y >= 25) {
        for (int i = 0; i < 24 * 80; i++) {
            video_memory[i] = video_memory[i + 80];
        }
        for (int i = 24 * 80; i < 25 * 80; i++) {
            video_memory[i] = 0x0F20; 
        }
        cursor_y = 24;
    }
}

void kputc(char c) {
    write_vga(c);
}

void clear_terminal(void) {
    if (is_graphics_mode) {
        uint32_t total_pixels = (g_height * g_pitch) / 4;
        fast_memset(backbuffer, term_bg_color, total_pixels);
        cursor_x = 0; 
        cursor_y = 0;
        swap_buffers();
        return;
    }

    for (int i = 0; i < 25 * 80; i++) {
        video_memory[i] = 0x0F20; 
    }
    cursor_x = 0; cursor_y = 0;
    update_cursor(0, 0);
}

void print_number(uint32_t num, int base) {
    if (num == 0) { kputc('0'); return; }
    char buffer[32];
    int i = 0;
    const char* charset = "0123456789ABCDEF";
    while (num > 0) { buffer[i++] = charset[num % base]; num /= base; }
    while (i > 0) { i--; kputc(buffer[i]); }
}

void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%' && format[i + 1] != '\0') {
            i++;
            switch (format[i]) {
                case 's': kprintf(va_arg(args, const char*)); break;
                case 'd': {
                    int num = va_arg(args, int);
                    if (num < 0) { kputc('-'); num = -num; }
                    print_number((uint32_t)num, 10);
                    break;
                }
                case 'x': kprintf("0x"); print_number(va_arg(args, uint32_t), 16); break;
                case 'c': kputc((char)va_arg(args, int)); break;
                case '%': kputc('%'); break;
            }
        } else {
            kputc(format[i]);
        }
    }
    va_end(args);
}

void draw_sprite(const uint32_t* sprite, uint32_t width, uint32_t height, uint32_t x, uint32_t y) {
    if (!is_graphics_mode) return;

    for (uint32_t sy = 0; sy < height; sy++) {
        for (uint32_t sx = 0; sx < width; sx++) {
            if (x + sx >= g_width || y + sy >= g_height) continue;

            uint32_t src_color = sprite[sy * width + sx];
            uint32_t a = (src_color >> 24) & 0xFF;

            if (a == 255) {
                put_pixel(x + sx, y + sy, src_color);
            } 
            else if (a > 0) {
                uint32_t offset = ((y + sy) * g_pitch) + ((x + sx) * (g_bpp / 8));
                uint32_t bg_color = *(uint32_t*)((uint8_t*)backbuffer + offset);

                uint32_t br = (bg_color >> 16) & 0xFF;
                uint32_t bg = (bg_color >> 8) & 0xFF;
                uint32_t bb = bg_color & 0xFF;

                uint32_t sr = (src_color >> 16) & 0xFF;
                uint32_t sg = (src_color >> 8) & 0xFF;
                uint32_t sb = src_color & 0xFF;

                uint32_t new_r = (sr * a + br * (255 - a)) / 255;
                uint32_t new_g = (sg * a + bg * (255 - a)) / 255;
                uint32_t new_b = (sb * a + bb * (255 - a)) / 255;

                uint32_t final_color = (new_r << 16) | (new_g << 8) | new_b;
                put_pixel(x + sx, y + sy, final_color);
            }
        }
    }
}

void draw_test_logo(uint32_t start_x, uint32_t start_y) {
    uint32_t sprite[32 * 32];
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int dx = x - 16;
            int dy = y - 16;
            int dist_squared = (dx * dx + dy * dy);
            
            if (dist_squared < 256) {
                uint32_t alpha = 255 - dist_squared; 
                if (alpha > 255) alpha = 0;
                sprite[y * 32 + x] = (alpha << 24) | 0x00FF8800; 
            } else {
                sprite[y * 32 + x] = 0x00000000;
            }
        }
    }
    draw_sprite(sprite, 32, 32, start_x, start_y);
}
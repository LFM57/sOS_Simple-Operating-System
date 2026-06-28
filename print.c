#include "system.h"
#define FONT8x16_IMPLEMENTATION /* Tell the header to instantiate the array here */
#include "font8x16.h"

#define PORT_COM1 0x3f8

/* --- NEW GRAPHICS ENGINE --- */
int is_graphics_mode = 0;
uint32_t* lfb_mem = (uint32_t*)0xE0000000;    /* Physical Video RAM mapped here */
uint32_t* backbuffer = (uint32_t*)0xE1000000; /* RAM Backbuffer mapped here */
uint32_t g_width = 0, g_height = 0, g_pitch = 0, g_bpp = 0;

uint32_t term_fg_color = 0x00FFFFFF; /* Default: White */
uint32_t term_bg_color = 0x00000000; /* Default: Black */

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

    /* 2. Allocate Backbuffer directly from Physical Memory Manager 
       (Bypasses kmalloc because a 1024x768x32 screen is 3MB, which would exhaust your 4MB heap!) */
    for (uint32_t i = 0; i < fb_size; i += 4096) {
        void* page = pmm_alloc_page();
        map_page(0xE1000000 + i, (uint32_t)page);
    }
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!is_graphics_mode || x >= g_width || y >= g_height) return;
    
    uint32_t offset = (y * g_pitch) + (x * (g_bpp / 8));
    /* Draw to the backbuffer for memory retention (needed for scrolling) */
    *(uint32_t*)((uint8_t*)backbuffer + offset) = color;
    /* Draw directly to the video card for instant text display without lagging */
    *(uint32_t*)((uint8_t*)lfb_mem + offset) = color; 
}

void swap_buffers() {
    if (!is_graphics_mode) return;
    uint32_t total_bytes = g_height * g_pitch;
    
    /* Fast block copy from RAM backbuffer to Video memory */
    uint32_t* dest = lfb_mem;
    uint32_t* src = backbuffer;
    for (uint32_t i = 0; i < total_bytes / 4; i++) {
        dest[i] = src[i];
    }

    update_software_cursor(); 
}

void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode) return;

    /* The compiler automatically strides by 16 bytes using array index notation */
    const unsigned char* glyph = font8x16[(unsigned char)c];

    for (uint32_t cy = 0; cy < 16; cy++) {
        unsigned char row_bits = glyph[cy];
        for (uint32_t cx = 0; cx < 8; cx++) {
            /* We check each bit from left to right (0x80 is 10000000 in binary) */
            if (row_bits & (0x80 >> cx)) {
                put_pixel(x + cx, y + cy, fg_color);
            } else {
                /* If bg_color is a specific transparent mask (like 0xFFFFFFFF), you could skip this. 
                   But for a terminal, we must draw the background to erase old letters. */
                put_pixel(x + cx, y + cy, bg_color);
            }
        }
    }
}

/* Helper function to test rendering an entire string */
void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode) return;
    
    uint32_t current_x = x;
    while (*str) {
        draw_char(*str, current_x, y, fg_color, bg_color);
        current_x += 8; /* Move 8 pixels to the right for the next character */
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

void scroll(); 

/* Function to update the hardware cursor */
void update_cursor(int x, int y) {
    /* Hardware VGA cursors don't exist in graphics mode, so we bypass it. */
    if (is_graphics_mode) return; 

    uint16_t pos = y * 80 + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* --- ADD THIS TO print.c (ABOVE write_vga) --- */
void update_software_cursor() {
    if (!is_graphics_mode) return;
    static uint32_t last_cx = 0, last_cy = 0;

    /* 1. RESTORE: Copy the clean text pixels from the backbuffer to erase the old cursor */
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 8; cx++) {
            uint32_t offset = ((last_cy * 16 + cy) * g_pitch) + ((last_cx * 8 + cx) * (g_bpp / 8));
            if (offset < g_height * g_pitch) { /* Safety bounds check */
                uint32_t clean_pixel = *(uint32_t*)((uint8_t*)backbuffer + offset);
                *(uint32_t*)((uint8_t*)lfb_mem + offset) = clean_pixel;
            }
        }
    }

    /* 2. DRAW: Draw the new cursor (a 2-pixel underline) directly to the screen (LFB), NOT the backbuffer */
    for (int cy = 13; cy < 15; cy++) {
        for (int cx = 0; cx < 8; cx++) {
            uint32_t offset = ((cursor_y * 16 + cy) * g_pitch) + ((cursor_x * 8 + cx) * (g_bpp / 8));
            if (offset < g_height * g_pitch) {
                *(uint32_t*)((uint8_t*)lfb_mem + offset) = term_fg_color;
            }
        }
    }

    /* 3. Save current position for the next update */
    last_cx = cursor_x;
    last_cy = cursor_y;
}

/* Write a character to the VGA screen */
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
                /* BUG FIX: Just move the cursor back. Do NOT draw a space here! */
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
            
            /* ADD THIS LINE: Update the cursor position visually! */
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
            /* Apply the requested ANSI color to BOTH VGA and Graphics mode! */
            if (ansi_val == 0)       { vga_attribute = 0x0F00; term_fg_color = 0x00FFFFFF; } /* Reset */
            else if (ansi_val == 31) { vga_attribute = 0x0C00; term_fg_color = 0x00FF5555; } /* Red */
            else if (ansi_val == 32) { vga_attribute = 0x0A00; term_fg_color = 0x0055FF55; } /* Green */
            else if (ansi_val == 33) { vga_attribute = 0x0E00; term_fg_color = 0x00FFFF55; } /* Yellow */
            else if (ansi_val == 34) { vga_attribute = 0x0900; term_fg_color = 0x005555FF; } /* Blue */
            else if (ansi_val == 35) { vga_attribute = 0x0D00; term_fg_color = 0x00FF55FF; } /* Magenta */
            else if (ansi_val == 36) { vga_attribute = 0x0B00; term_fg_color = 0x0055FFFF; } /* Cyan */
            else if (ansi_val == 37) { vga_attribute = 0x0700; term_fg_color = 0x00AAAAAA; } /* Gray */
            ansi_state = 0;
            ansi_val = 0;
        } else {
            ansi_state = 0;
            ansi_val = 0;
        }
    }
}

/* Scrolling function */
void scroll() {
    if (is_graphics_mode) {
        int term_rows = g_height / 16;
        
        if (cursor_y >= term_rows) {
            uint32_t row_bytes = 16 * g_pitch;
            uint32_t total_shift_bytes = (term_rows - 1) * row_bytes;
            
            /* Shift the backbuffer up by one text row (16 pixels) */
            uint8_t* dst = (uint8_t*)backbuffer;
            uint8_t* src = (uint8_t*)backbuffer + row_bytes;
            for (uint32_t i = 0; i < total_shift_bytes; i++) {
                dst[i] = src[i];
            }
            
            /* Clear the very last text row with the background color */
            uint32_t* last_line = (uint32_t*)(dst + total_shift_bytes);
            uint32_t pixels_to_clear = row_bytes / 4;
            for (uint32_t i = 0; i < pixels_to_clear; i++) {
                last_line[i] = term_bg_color;
            }
            
            cursor_y = term_rows - 1;
            /* Push the newly shifted backbuffer to the actual screen */
            swap_buffers(); 
        }
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

void clear_terminal() {
    if (is_graphics_mode) {
        uint32_t total_pixels = (g_height * g_pitch) / 4;
        for (uint32_t i = 0; i < total_pixels; i++) {
            backbuffer[i] = term_bg_color;
        }
        swap_buffers();
        cursor_x = 0; cursor_y = 0;
        return;
    }

    /* Fallback to legacy VGA text mode */
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
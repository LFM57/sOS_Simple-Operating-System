#include "system.h"

#define PORT_COM1 0x3f8

/* Global variables for VGA display and ANSI handling */
uint16_t* video_memory = (uint16_t*)0xB8000;
int cursor_x = 0;
int cursor_y = 0;

static int ansi_state = 0;
static int ansi_val = 0;
static uint16_t vga_attribute = 0x0F00; /* White on black by default */

/* Function to update the hardware cursor */
void update_cursor(int x, int y) {
    uint16_t pos = y * 80 + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* Write a character to the VGA screen (decodes basic ANSI) */
void write_vga(char c) {
    if (ansi_state == 0) {
        if (c == '\033') {
            ansi_state = 1;
            return;
        }
        
        /* Classic character display */
        if (c == '\n') {
            cursor_x = 0;
            cursor_y++;
        } else if (c == '\b') {
            if (cursor_x > 0) cursor_x--;
            else if (cursor_y > 0) { cursor_y--; cursor_x = 79; }
        } else if (c == '\r') {
            cursor_x = 0;
        } else {
            video_memory[cursor_y * 80 + cursor_x] = (uint16_t)c | vga_attribute;
            cursor_x++;
            if (cursor_x >= 80) { cursor_x = 0; cursor_y++; }
        }
        scroll();
        update_cursor(cursor_x, cursor_y);
    } 
    else if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_val = 0;
        } else {
            ansi_state = 0;
        }
    } 
    else if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            ansi_val = ansi_val * 10 + (c - '0');
        } else if (c == ';') {
            ansi_val = 0; /* /* Ignore complex multiple attributes like (1;32) to keep it simple */
        } else if (c == 'm') {
            /* Apply the requested ANSI color */
            if (ansi_val == 0)       vga_attribute = 0x0F00; /* Reset */
            else if (ansi_val == 31) vga_attribute = 0x0C00; /* Red */
            else if (ansi_val == 32) vga_attribute = 0x0A00; /* Green */
            else if (ansi_val == 33) vga_attribute = 0x0E00; /* Yellow */
            else if (ansi_val == 34) vga_attribute = 0x0900; /* Blue */
            else if (ansi_val == 35) vga_attribute = 0x0D00; /* Magenta */
            else if (ansi_val == 36) vga_attribute = 0x0B00; /* Cyan */
            else if (ansi_val == 37) vga_attribute = 0x0700; /* Gray */
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
    if (cursor_y >= 25) {
        /* Shift all the text one line up */
        for (int i = 0; i < 24 * 80; i++) {
            video_memory[i] = video_memory[i + 80];
        }
        /* Erase the last line */
        for (int i = 24 * 80; i < 25 * 80; i++) {
            video_memory[i] = 0x0F20; /* 0x20 = Space, 0x0F = White text on black background */
        }
        cursor_y = 24;
    }
}

void kputc(char c) {
    write_vga(c);
}

void clear_terminal() {
    
    /* Clean up VGA hardware screen */
    for (int i = 0; i < 25 * 80; i++) {
        video_memory[i] = 0x0F20; /* 0x20 = Space, 0x0F = White on black */
    }
    cursor_x = 0;
    cursor_y = 0;
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
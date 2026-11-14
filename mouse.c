#include "system.h"
#include "mouse.h"

volatile int mouse_x = 512;
volatile int mouse_y = 384;
volatile uint8_t mouse_buttons = 0;

static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[3];
static mouse_callback_t mouse_cb = NULL;

void mouse_set_callback(mouse_callback_t cb) {
    mouse_cb = cb;
}

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        /* Wait until data is available to read from port 0x60 */
        while (timeout--) {
            if (inb(0x64) & 0x01) return;
        }
    } else {
        /* Wait until input buffer is empty and ready to write */
        while (timeout--) {
            if ((inb(0x64) & 0x02) == 0) return;
        }
    }
}

static void mouse_write(uint8_t val) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, val);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void init_mouse(void) {
    if (g_width > 0 && g_height > 0) {
        mouse_x = (int)(g_width / 2);
        mouse_y = (int)(g_height / 2);
    }
    mouse_buttons = 0;
    mouse_cycle = 0;

    /* Drain any stale bytes from the PS/2 output buffer */
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++) {
        (void)inb(0x60);
    }

    /* 1. Enable the auxiliary mouse device */
    mouse_wait(1);
    outb(0x64, 0xA8);

    /* 2. Enable IRQ12 and auxiliary clock in the PS/2 Controller Command Byte */
    mouse_wait(1);
    outb(0x64, 0x20);
    uint8_t status = mouse_read();
    status |= 0x02;   /* Set bit 1: Enable IRQ12 */
    status &= ~0x20;  /* Clear bit 5: Enable auxiliary mouse clock */
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    /* 3. Use default mouse settings (0xF6) and wait for ACK (0xFA) */
    mouse_write(0xF6);
    (void)mouse_read();

    /* 4. Enable packet streaming (0xF4) and wait for ACK (0xFA) */
    mouse_write(0xF4);
    (void)mouse_read();

    /* 5. Unmask IRQ2 (Slave cascade) and IRQ12 (PS/2 Mouse) on the PIC */
    pic_enable_irq(2);
    pic_enable_irq(12);
}

void mouse_handler(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) {
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }

    uint8_t data = inb(0x60);
    if (!(status & 0x20)) {
        /* Byte did not originate from the auxiliary PS/2 port */
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }

    switch (mouse_cycle) {
        case 0:
            /* Bit 3 must always be 1 in a valid first packet byte */
            if (!(data & 0x08)) break;
            mouse_byte[0] = data;
            mouse_cycle = 1;
            break;

        case 1:
            mouse_byte[1] = data;
            mouse_cycle = 2;
            break;

        case 2:
            mouse_byte[2] = data;
            mouse_cycle = 0;

            /* Discard packets with X or Y overflow flags set */
            if (mouse_byte[0] & 0xC0) break;

            int dx = (mouse_byte[0] & 0x10) ? ((int)mouse_byte[1] - 256) : (int)mouse_byte[1];
            int dy = (mouse_byte[0] & 0x20) ? ((int)mouse_byte[2] - 256) : (int)mouse_byte[2];

            mouse_x += dx;
            mouse_y -= dy; /* PS/2 Y-axis is inverted relative to screen coordinates */

            int max_x = (g_width > 0) ? ((int)g_width - 1) : 1023;
            int max_y = (g_height > 0) ? ((int)g_height - 1) : 767;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x > max_x) mouse_x = max_x;
            if (mouse_y > max_y) mouse_y = max_y;

            mouse_buttons = mouse_byte[0] & 0x07;

            if (mouse_cb) {
                mouse_cb(mouse_x, mouse_y, mouse_buttons);
            }
            break;
    }

    /* Send EOI to both Slave and Master PICs */
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

extern volatile int mouse_x;
extern volatile int mouse_y;
extern volatile uint8_t mouse_buttons; /* Bit 0: Left, Bit 1: Right, Bit 2: Middle */
extern volatile int mouse_has_wheel;

typedef void (*mouse_callback_t)(int x, int y, uint8_t buttons, int dz);

void init_mouse(void);
void mouse_handler(void);
void mouse_set_callback(mouse_callback_t cb);

#endif

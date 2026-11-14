#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "system.h"

#define MAX_WINDOWS 16
#define TITLEBAR_HEIGHT 24
#define TASKBAR_HEIGHT 28
#define GUI_EVENT_QUEUE_SIZE 32

#define WIN_VISIBLE   0x01
#define WIN_DECORATED 0x02
#define WIN_BUILTIN   0x04

typedef enum {
    GUI_EVENT_NONE = 0,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_KEY_PRESS,
    GUI_EVENT_WINDOW_CLOSE
} gui_event_type_t;

typedef struct {
    gui_event_type_t type;
    int x, y;          /* Window-relative mouse coordinates */
    uint8_t button;    /* 1 = Left, 2 = Right */
    char key;          /* ASCII key from keyboard */
} gui_event_t;

typedef struct window {
    int id;
    int is_used;
    int x, y;
    int width, height;     /* Client area dimensions */
    char title[64];
    uint32_t flags;        /* WIN_DECORATED, WIN_VISIBLE, WIN_BUILTIN */
    uint32_t owner_pid;
    uint32_t* canvas;      /* Pixel buffer: width * height * 4 bytes (kmalloc) */

    /* Per-window event ring buffer + wait queue for blocking apps */
    gui_event_t events[GUI_EVENT_QUEUE_SIZE];
    int ev_head, ev_tail;
    wait_queue_t event_queue;
} window_t;

extern volatile int gui_dirty;
extern volatile int gui_full_dirty;
extern volatile int gui_exit_requested;
extern wait_queue_t gui_wait_queue;
extern window_t* gui_terminal_window;

void gui_init(void);
void gui_start(void);
void gui_request_exit(void);
int gui_create_window(const char* title, int x, int y, int w, int h, uint32_t flags, uint32_t owner_pid);
void gui_destroy_window(int win_id);
void gui_destroy_windows_by_pid(uint32_t pid);
window_t* gui_get_window(int win_id);
window_t* gui_get_focused_window(void);
void gui_focus_window(window_t* w);

int gui_push_event(window_t* w, const gui_event_t* ev);
int gui_pop_event(window_t* w, gui_event_t* out_ev);

void gui_on_mouse_packet(int x, int y, uint8_t buttons);
void gui_on_key_press(char ascii);
void gui_mark_dirty(int full_redraw);
void gui_process_input(void);
void gui_render(void);

#endif

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

#define GUI_MOD_SHIFT 0x01
#define GUI_MOD_CTRL  0x02
#define GUI_MOD_ALT   0x04

#define GUI_KEY_HOME      1
#define GUI_KEY_END       5
#define GUI_KEY_BACKSPACE '\b'
#define GUI_KEY_TAB       '\t'
#define GUI_KEY_ENTER     '\n'
#define GUI_KEY_UP        17
#define GUI_KEY_DOWN      18
#define GUI_KEY_LEFT      20
#define GUI_KEY_RIGHT     21
#define GUI_KEY_PAGE_UP   25
#define GUI_KEY_PAGE_DOWN 26
#define GUI_KEY_ESC       27
#define GUI_KEY_DELETE    127

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
    char key;          /* ASCII / special key from keyboard */
    uint8_t modifiers; /* GUI_MOD_SHIFT | GUI_MOD_CTRL | GUI_MOD_ALT */
    uint8_t scancode;  /* Set-1 make scancode (0x80 bit set for E0-extended keys) */
} gui_event_t;

typedef struct window {
    int id;
    int is_used;
    int x, y;
    int width, height;     /* Client area dimensions */
    char title[64];
    uint32_t flags;        /* WIN_DECORATED, WIN_VISIBLE, WIN_BUILTIN */
    uint32_t owner_pid;
    uint8_t owner_uid;
    uint32_t* canvas;      /* Pixel buffer: width * height * 4 bytes (kmalloc) */

    /* Per-window event ring buffer + wait queue for blocking apps */
    gui_event_t events[GUI_EVENT_QUEUE_SIZE];
    int ev_head, ev_tail;
    wait_queue_t event_queue;
} window_t;

extern volatile int gui_dirty;
extern volatile int gui_full_dirty;
extern volatile int gui_exit_requested;
extern volatile int is_gui_locked;
extern volatile int is_gui_login;
extern volatile int gui_logout_requested;
extern wait_queue_t gui_wait_queue;
extern window_t* gui_terminal_window;

void gui_init(void);
void gui_start(void);
void gui_boot_login(void);
void gui_show_login_screen(void);
void gui_logout_to_login(void);
void gui_request_exit(void);
void gui_lock_screen(void);
void gui_lock_on_key_press(char ascii);
void gui_login_on_key_press(char ascii);
int gui_create_window(const char* title, int x, int y, int w, int h, uint32_t flags, uint32_t owner_pid);
int gui_create_user_window(const char* title, int w, int h, uint32_t owner_pid);
int gui_present_window(int win_id, const uint32_t* buf, uint32_t owner_pid);
int gui_get_window_event(int win_id, gui_event_t* out_ev, uint32_t owner_pid, int blocking);
int gui_destroy_user_window(int win_id, uint32_t owner_pid);
void gui_destroy_window(int win_id);
void gui_destroy_windows_by_pid(uint32_t pid);
void gui_destroy_windows_by_uid(uint8_t uid);
window_t* gui_get_window(int win_id);
window_t* gui_get_focused_window(void);
void gui_focus_window(window_t* w);

int gui_push_event(window_t* w, const gui_event_t* ev);
int gui_pop_event(window_t* w, gui_event_t* out_ev);

void gui_on_mouse_packet(int x, int y, uint8_t buttons);
void gui_on_key_press(char ascii, uint8_t modifiers, uint8_t scancode);
void gui_mark_dirty(int full_redraw);
void gui_process_input(void);
void gui_render(void);

#endif

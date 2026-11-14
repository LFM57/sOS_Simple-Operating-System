#include "gui.h"
#include "mouse.h"

extern task_t tasks[MAX_TASKS];
extern int current_task;

static window_t windows[MAX_WINDOWS];
static window_t* z_order[MAX_WINDOWS];
static int window_count = 0;

volatile int gui_dirty = 0;
volatile int gui_full_dirty = 0;
volatile int gui_exit_requested = 0;
wait_queue_t gui_wait_queue = {0};
window_t* gui_terminal_window = NULL;

/* Mouse packet ring buffer filled by IRQ12 and drained by gui_process_input() */
#define MOUSE_Q_SIZE 32
typedef struct {
    int x;
    int y;
    uint8_t buttons;
} mouse_pkt_t;

static volatile mouse_pkt_t mouse_queue[MOUSE_Q_SIZE];
static volatile int mq_head = 0;
static volatile int mq_tail = 0;

static int prev_mouse_x = -1;
static int prev_mouse_y = -1;
static uint8_t prev_buttons = 0;
static int rendered_cursor_x = -1;
static int rendered_cursor_y = -1;

static window_t* dragging_window = NULL;
static int drag_offset_x = 0;
static int drag_offset_y = 0;

/* 12x19 32-bit ARGB arrow cursor sprite (0xFF... opaque, 0x00... transparent) */
#define C_T 0x00000000
#define C_B 0xFF0F172A
#define C_W 0xFFFFFFFF
static const uint32_t cursor_sprite[12 * 19] = {
    C_B, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T,
    C_B, C_B, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T,
    C_B, C_W, C_B, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T,
    C_B, C_W, C_W, C_B, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T,
    C_B, C_W, C_W, C_W, C_B, C_T, C_T, C_T, C_T, C_T, C_T, C_T,
    C_B, C_W, C_W, C_W, C_W, C_B, C_T, C_T, C_T, C_T, C_T, C_T,
    C_B, C_W, C_W, C_W, C_W, C_W, C_B, C_T, C_T, C_T, C_T, C_T,
    C_B, C_W, C_W, C_W, C_W, C_W, C_W, C_B, C_T, C_T, C_T, C_T,
    C_B, C_W, C_W, C_W, C_W, C_W, C_W, C_W, C_B, C_T, C_T, C_T,
    C_B, C_W, C_W, C_W, C_W, C_W, C_W, C_W, C_W, C_B, C_T, C_T,
    C_B, C_W, C_W, C_W, C_W, C_W, C_W, C_W, C_W, C_W, C_B, C_T,
    C_B, C_W, C_W, C_W, C_W, C_W, C_W, C_B, C_B, C_B, C_B, C_B,
    C_B, C_W, C_W, C_W, C_B, C_W, C_W, C_B, C_T, C_T, C_T, C_T,
    C_B, C_W, C_W, C_B, C_T, C_B, C_W, C_W, C_B, C_T, C_T, C_T,
    C_B, C_W, C_B, C_T, C_T, C_B, C_W, C_W, C_B, C_T, C_T, C_T,
    C_B, C_B, C_T, C_T, C_T, C_T, C_B, C_W, C_W, C_B, C_T, C_T,
    C_T, C_T, C_T, C_T, C_T, C_T, C_B, C_W, C_W, C_B, C_T, C_T,
    C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_B, C_B, C_T, C_T, C_T,
    C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T, C_T
};
#undef C_T
#undef C_B
#undef C_W

static uint32_t irq_save_disable(void) {
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags));
    return eflags;
}

static void irq_restore(uint32_t eflags) {
    __asm__ volatile("push %0; popf" :: "r"(eflags));
}

static void copy_title(char* dst, const char* src, int max_len) {
    int i = 0;
    if (src) {
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

void gui_init(void) {
    uint32_t eflags = irq_save_disable();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].id = i;
        windows[i].is_used = 0;
        windows[i].canvas = NULL;
        windows[i].ev_head = 0;
        windows[i].ev_tail = 0;
        windows[i].event_queue.head = NULL;
        z_order[i] = NULL;
    }
    window_count = 0;
    dragging_window = NULL;
    mq_head = 0;
    mq_tail = 0;
    prev_mouse_x = mouse_x;
    prev_mouse_y = mouse_y;
    prev_buttons = mouse_buttons;
    rendered_cursor_x = -1;
    rendered_cursor_y = -1;
    gui_dirty = 1;
    gui_full_dirty = 1;
    gui_exit_requested = 0;
    gui_terminal_window = NULL;
    mouse_set_callback(gui_on_mouse_packet);
    irq_restore(eflags);
}

void gui_request_exit(void) {
    gui_exit_requested = 1;
    gui_dirty = 1;
    wake_up(&gui_wait_queue);
}

void gui_mark_dirty(int full_redraw) {
    gui_dirty = 1;
    if (full_redraw) {
        gui_full_dirty = 1;
    }
    wake_up(&gui_wait_queue);
}

int gui_create_window(const char* title, int x, int y, int w, int h, uint32_t flags, uint32_t owner_pid) {
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return -1;

    uint32_t buf_bytes = (uint32_t)w * (uint32_t)h * 4U;
    uint32_t* canvas = (uint32_t*)kmalloc(buf_bytes);
    if (!canvas) return -1;

    for (uint32_t i = 0; i < (uint32_t)w * (uint32_t)h; i++) {
        canvas[i] = 0x00F8FAFC; /* Clean light default surface */
    }

    uint32_t eflags = irq_save_disable();
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].is_used) {
            slot = i;
            break;
        }
    }

    if (slot == -1 || window_count >= MAX_WINDOWS) {
        irq_restore(eflags);
        kfree(canvas);
        return -1;
    }

    window_t* win = &windows[slot];
    win->id = slot;
    win->is_used = 1;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    copy_title(win->title, title ? title : "Window", sizeof(win->title));
    win->flags = flags;
    win->owner_pid = owner_pid;
    win->canvas = canvas;
    win->ev_head = 0;
    win->ev_tail = 0;
    win->event_queue.head = NULL;

    z_order[window_count++] = win;
    gui_full_dirty = 1;
    gui_dirty = 1;
    irq_restore(eflags);

    wake_up(&gui_wait_queue);
    return slot;
}

static int window_cascade = 0;

int gui_create_user_window(const char* title, int w, int h, uint32_t owner_pid) {
    if (!is_gui_mode || w <= 0 || h <= 0 || w > (int)g_width || h > (int)g_height) {
        return -1;
    }
    int x = 60 + ((window_cascade * 32) % 240);
    int y = 52 + ((window_cascade * 28) % 160);
    if (x + w > (int)g_width - 8) x = (int)g_width - w - 8;
    if (y + h > (int)g_height - TASKBAR_HEIGHT - 8) y = (int)g_height - TASKBAR_HEIGHT - h - 8;
    if (x < 8) x = 8;
    if (y < TITLEBAR_HEIGHT + 4) y = TITLEBAR_HEIGHT + 4;

    int win_id = gui_create_window(title, x, y, w, h, WIN_VISIBLE | WIN_DECORATED, owner_pid);
    if (win_id >= 0) {
        window_cascade++;
    }
    return win_id;
}

int gui_present_window(int win_id, const uint32_t* buf, uint32_t owner_pid) {
    if (!is_gui_mode || !buf) return -1;
    window_t* win = gui_get_window(win_id);
    if (!win || win->owner_pid != owner_pid || (win->flags & WIN_BUILTIN) || !win->canvas) {
        return -1;
    }

    uint32_t dwords = (uint32_t)win->width * (uint32_t)win->height;
    uint32_t eflags = irq_save_disable();
    for (uint32_t i = 0; i < dwords; i++) {
        win->canvas[i] = buf[i];
    }
    gui_full_dirty = 1;
    gui_dirty = 1;
    irq_restore(eflags);

    wake_up(&gui_wait_queue);
    return 0;
}

int gui_get_window_event(int win_id, gui_event_t* out_ev, uint32_t owner_pid, int blocking) {
    if (!out_ev) return -1;
    window_t* win = gui_get_window(win_id);
    if (!win || win->owner_pid != owner_pid || (win->flags & WIN_BUILTIN)) {
        return -1;
    }

    if (gui_pop_event(win, out_ev)) {
        return 1;
    }
    if (!blocking) {
        out_ev->type = GUI_EVENT_NONE;
        return 0;
    }

    __asm__ volatile("sti");
    while (1) {
        if (!is_gui_mode) return -1;
        win = gui_get_window(win_id);
        if (!win || win->owner_pid != owner_pid) return -1;
        if (gui_pop_event(win, out_ev)) return 1;
        sleep_on(&win->event_queue);
    }
}

int gui_destroy_user_window(int win_id, uint32_t owner_pid) {
    window_t* win = gui_get_window(win_id);
    if (!win || win->owner_pid != owner_pid || (win->flags & WIN_BUILTIN)) {
        return -1;
    }
    gui_destroy_window(win_id);
    return 0;
}

static void gui_launch_demo_app(void) {
    uint8_t* file_buf = NULL;
    uint32_t file_size = 0;
    if (!fs_load_file("/bin/guidemo.sos", &file_buf, &file_size)) {
        return;
    }
    int task_id = create_user_task(file_buf, file_size);
    kfree(file_buf);
    if (task_id != -1) {
        tasks[task_id].is_background = 1;
    }
}

void gui_destroy_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return;

    uint32_t eflags = irq_save_disable();
    window_t* win = &windows[win_id];
    if (!win->is_used) {
        irq_restore(eflags);
        return;
    }

    if (dragging_window == win) {
        dragging_window = NULL;
    }

    for (int i = 0; i < window_count; i++) {
        if (z_order[i] == win) {
            for (int j = i; j < window_count - 1; j++) {
                z_order[j] = z_order[j + 1];
            }
            z_order[window_count - 1] = NULL;
            window_count--;
            break;
        }
    }

    uint32_t* old_canvas = win->canvas;
    win->canvas = NULL;
    win->is_used = 0;
    win->ev_head = 0;
    win->ev_tail = 0;

    wake_up(&win->event_queue);
    gui_full_dirty = 1;
    gui_dirty = 1;
    irq_restore(eflags);

    if (old_canvas) {
        kfree(old_canvas);
    }
    wake_up(&gui_wait_queue);
}

void gui_destroy_windows_by_pid(uint32_t pid) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].is_used && windows[i].owner_pid == pid && !(windows[i].flags & WIN_BUILTIN)) {
            gui_destroy_window(i);
        }
    }
}

window_t* gui_get_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return NULL;
    if (!windows[win_id].is_used) return NULL;
    return &windows[win_id];
}

window_t* gui_get_focused_window(void) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (z_order[i] && z_order[i]->is_used && (z_order[i]->flags & WIN_VISIBLE)) {
            return z_order[i];
        }
    }
    return NULL;
}

void gui_focus_window(window_t* w) {
    if (!w || !w->is_used) return;
    uint32_t eflags = irq_save_disable();
    int found = -1;
    for (int i = 0; i < window_count; i++) {
        if (z_order[i] == w) {
            found = i;
            break;
        }
    }
    if (found != -1 && found != window_count - 1) {
        for (int i = found; i < window_count - 1; i++) {
            z_order[i] = z_order[i + 1];
        }
        z_order[window_count - 1] = w;
        gui_full_dirty = 1;
        gui_dirty = 1;
    }
    irq_restore(eflags);
    wake_up(&gui_wait_queue);
}

int gui_push_event(window_t* w, const gui_event_t* ev) {
    if (!w || !w->is_used || !ev) return 0;
    uint32_t eflags = irq_save_disable();
    int next = (w->ev_head + 1) % GUI_EVENT_QUEUE_SIZE;
    if (next == w->ev_tail) {
        /* Drop oldest event on overflow so latest events aren't lost */
        w->ev_tail = (w->ev_tail + 1) % GUI_EVENT_QUEUE_SIZE;
    }
    w->events[w->ev_head] = *ev;
    w->ev_head = next;
    irq_restore(eflags);
    wake_up(&w->event_queue);
    return 1;
}

int gui_pop_event(window_t* w, gui_event_t* out_ev) {
    if (!w || !w->is_used || !out_ev) return 0;
    uint32_t eflags = irq_save_disable();
    if (w->ev_head == w->ev_tail) {
        irq_restore(eflags);
        return 0;
    }
    *out_ev = w->events[w->ev_tail];
    w->ev_tail = (w->ev_tail + 1) % GUI_EVENT_QUEUE_SIZE;
    irq_restore(eflags);
    return 1;
}

/* Called from IRQ12 mouse_handler() */
void gui_on_mouse_packet(int x, int y, uint8_t buttons) {
    int next = (mq_head + 1) % MOUSE_Q_SIZE;
    if (next == mq_tail) {
        mq_tail = (mq_tail + 1) % MOUSE_Q_SIZE;
    }
    mouse_queue[mq_head].x = x;
    mouse_queue[mq_head].y = y;
    mouse_queue[mq_head].buttons = buttons;
    mq_head = next;

    gui_dirty = 1;
    wake_up(&gui_wait_queue);
}

/* Called from IRQ1 keyboard_handler() when GUI mode is active */
void gui_on_key_press(char ascii) {
    if (ascii == 0) return;
    window_t* top = gui_get_focused_window();
    if (!top) return;

    gui_event_t ev;
    ev.type = GUI_EVENT_KEY_PRESS;
    ev.x = 0;
    ev.y = 0;
    ev.button = 0;
    ev.key = ascii;
    gui_push_event(top, &ev);
}

static void handle_mouse_step(int mx, int my, uint8_t buttons) {
    uint8_t prev_left = prev_buttons & 0x01;
    uint8_t curr_left = buttons & 0x01;
    uint8_t prev_right = prev_buttons & 0x02;
    uint8_t curr_right = buttons & 0x02;

    if (mx != prev_mouse_x || my != prev_mouse_y) {
        if (dragging_window && dragging_window->is_used) {
            int new_x = mx - drag_offset_x;
            int new_y = my - drag_offset_y;
            int min_y = TITLEBAR_HEIGHT + 2;
            int max_y = (int)g_height - TASKBAR_HEIGHT - 4;
            if (new_y < min_y) new_y = min_y;
            if (new_y > max_y) new_y = max_y;
            dragging_window->x = new_x;
            dragging_window->y = new_y;
            gui_full_dirty = 1;
        } else {
            window_t* top = gui_get_focused_window();
            if (top && mx >= top->x && mx < top->x + top->width &&
                my >= top->y && my < top->y + top->height) {
                gui_event_t ev;
                ev.type = GUI_EVENT_MOUSE_MOVE;
                ev.x = mx - top->x;
                ev.y = my - top->y;
                ev.button = buttons;
                ev.key = 0;
                gui_push_event(top, &ev);
            }
        }
        gui_dirty = 1;
    }

    if (!prev_left && curr_left) {
        /* Left click: check taskbar buttons and window chips first */
        if (my >= (int)g_height - TASKBAR_HEIGHT) {
            int exit_x = (int)g_width - 68;
            if (mx >= exit_x && mx < exit_x + 60) {
                gui_request_exit();
            } else {
                int chip_x = 8;
                int term_btn_w = 88;
                int demo_btn_w = 88;
                if (gui_terminal_window && mx >= chip_x && mx < chip_x + term_btn_w) {
                    gui_terminal_window->flags |= WIN_VISIBLE;
                    gui_focus_window(gui_terminal_window);
                    gui_full_dirty = 1;
                    gui_dirty = 1;
                } else if (mx >= chip_x + term_btn_w + 6 && mx < chip_x + term_btn_w + 6 + demo_btn_w) {
                    gui_launch_demo_app();
                } else {
                    chip_x += term_btn_w + 6 + demo_btn_w + 10;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        if (!windows[i].is_used || !(windows[i].flags & WIN_VISIBLE)) continue;
                        if (&windows[i] == gui_terminal_window) continue;
                        int chip_w = 120;
                        if (mx >= chip_x && mx < chip_x + chip_w) {
                            gui_focus_window(&windows[i]);
                            break;
                        }
                        chip_x += chip_w + 6;
                    }
                }
            }
        } else {
            /* Hit-test windows top-to-bottom */
            for (int i = window_count - 1; i >= 0; i--) {
                window_t* w = z_order[i];
                if (!w || !w->is_used || !(w->flags & WIN_VISIBLE)) continue;

                int decorated = (w->flags & WIN_DECORATED) != 0;
                int fx = decorated ? (w->x - 2) : w->x;
                int fy = decorated ? (w->y - TITLEBAR_HEIGHT - 2) : w->y;
                int fw = decorated ? (w->width + 4) : w->width;
                int fh = decorated ? (w->height + TITLEBAR_HEIGHT + 4) : w->height;

                if (mx >= fx && mx < fx + fw && my >= fy && my < fy + fh) {
                    gui_focus_window(w);

                    if (decorated) {
                        int close_x = w->x + w->width - 20;
                        int close_y = w->y - TITLEBAR_HEIGHT + 4;
                        if (mx >= close_x && mx < close_x + 16 &&
                            my >= close_y && my < close_y + 16) {
                            if (w == gui_terminal_window) {
                                w->flags &= ~WIN_VISIBLE;
                                gui_full_dirty = 1;
                                gui_dirty = 1;
                            } else {
                                gui_event_t ev;
                                ev.type = GUI_EVENT_WINDOW_CLOSE;
                                ev.x = 0;
                                ev.y = 0;
                                ev.button = 1;
                                ev.key = 0;
                                gui_push_event(w, &ev);
                            }
                            break;
                        }
                        if (my < w->y) {
                            dragging_window = w;
                            drag_offset_x = mx - w->x;
                            drag_offset_y = my - w->y;
                            break;
                        }
                    }

                    if (mx >= w->x && mx < w->x + w->width &&
                        my >= w->y && my < w->y + w->height) {
                        gui_event_t ev;
                        ev.type = GUI_EVENT_MOUSE_DOWN;
                        ev.x = mx - w->x;
                        ev.y = my - w->y;
                        ev.button = 1;
                        ev.key = 0;
                        gui_push_event(w, &ev);
                    }
                    break;
                }
            }
        }
    } else if (prev_left && !curr_left) {
        if (dragging_window) {
            dragging_window = NULL;
        } else {
            window_t* top = gui_get_focused_window();
            if (top && mx >= top->x && mx < top->x + top->width &&
                my >= top->y && my < top->y + top->height) {
                gui_event_t ev;
                ev.type = GUI_EVENT_MOUSE_UP;
                ev.x = mx - top->x;
                ev.y = my - top->y;
                ev.button = 1;
                ev.key = 0;
                gui_push_event(top, &ev);
            }
        }
    }

    if (!prev_right && curr_right) {
        window_t* top = gui_get_focused_window();
        if (top && mx >= top->x && mx < top->x + top->width &&
            my >= top->y && my < top->y + top->height) {
            gui_event_t ev;
            ev.type = GUI_EVENT_MOUSE_DOWN;
            ev.x = mx - top->x;
            ev.y = my - top->y;
            ev.button = 2;
            ev.key = 0;
            gui_push_event(top, &ev);
        }
    } else if (prev_right && !curr_right) {
        window_t* top = gui_get_focused_window();
        if (top && mx >= top->x && mx < top->x + top->width &&
            my >= top->y && my < top->y + top->height) {
            gui_event_t ev;
            ev.type = GUI_EVENT_MOUSE_UP;
            ev.x = mx - top->x;
            ev.y = my - top->y;
            ev.button = 2;
            ev.key = 0;
            gui_push_event(top, &ev);
        }
    }

    prev_mouse_x = mx;
    prev_mouse_y = my;
    prev_buttons = buttons;
}

void gui_process_input(void) {
    while (1) {
        uint32_t eflags = irq_save_disable();
        if (mq_head == mq_tail) {
            irq_restore(eflags);
            break;
        }
        mouse_pkt_t pkt = mouse_queue[mq_tail];
        mq_tail = (mq_tail + 1) % MOUSE_Q_SIZE;
        irq_restore(eflags);

        handle_mouse_step(pkt.x, pkt.y, pkt.buttons);
    }
}

static void draw_taskbar(window_t* focused) {
    int bar_y = (int)g_height - TASKBAR_HEIGHT;
    bb_fill_rect(0, bar_y, (int)g_width, TASKBAR_HEIGHT, 0x000F172A);
    bb_fill_rect(0, bar_y, (int)g_width, 1, 0x00334155);

    int chip_x = 8;
    if (gui_terminal_window) {
        int term_btn_w = 88;
        int is_term_act = (gui_terminal_window == focused && (gui_terminal_window->flags & WIN_VISIBLE));
        uint32_t bg = is_term_act ? 0x002563EB : 0x001E293B;
        bb_fill_rect(chip_x, bar_y + 4, term_btn_w, TASKBAR_HEIGHT - 8, bg);
        bb_draw_rect_outline(chip_x, bar_y + 4, term_btn_w, TASKBAR_HEIGHT - 8,
                             is_term_act ? 0x0060A5FA : 0x00334155);
        bb_draw_string_transparent("Terminal", chip_x + 12, bar_y + 6, 0x00F8FAFC);
        chip_x += term_btn_w + 6;
    }

    int demo_btn_w = 88;
    bb_fill_rect(chip_x, bar_y + 4, demo_btn_w, TASKBAR_HEIGHT - 8, 0x000F766E);
    bb_draw_rect_outline(chip_x, bar_y + 4, demo_btn_w, TASKBAR_HEIGHT - 8, 0x0014B8A6);
    bb_draw_string_transparent("Paint", chip_x + 24, bar_y + 6, 0x00F8FAFC);
    chip_x += demo_btn_w + 10;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].is_used || !(windows[i].flags & WIN_VISIBLE)) continue;
        if (&windows[i] == gui_terminal_window) continue;
        int chip_w = 120;
        if (chip_x + chip_w > (int)g_width - 140) break;

        int is_act = (&windows[i] == focused);
        uint32_t bg = is_act ? 0x002563EB : 0x001E293B;
        bb_fill_rect(chip_x, bar_y + 4, chip_w, TASKBAR_HEIGHT - 8, bg);
        bb_draw_rect_outline(chip_x, bar_y + 4, chip_w, TASKBAR_HEIGHT - 8,
                             is_act ? 0x0060A5FA : 0x00334155);

        char label[14];
        copy_title(label, windows[i].title, sizeof(label));
        bb_draw_string_transparent(label, chip_x + 6, bar_y + 6, 0x00F8FAFC);
        chip_x += chip_w + 6;
    }

    int exit_x = (int)g_width - 68;
    bb_fill_rect(exit_x, bar_y + 4, 60, TASKBAR_HEIGHT - 8, 0x00991B1B);
    bb_draw_rect_outline(exit_x, bar_y + 4, 60, TASKBAR_HEIGHT - 8, 0x00EF4444);
    bb_draw_string_transparent("Exit", exit_x + 14, bar_y + 6, 0x00FFFFFF);
}

static void draw_window_title_clipped(window_t* w) {
    int max_chars = (w->width - 34) / 8;
    if (max_chars <= 0) return;

    int x = w->x + 8;
    int y = w->y - TITLEBAR_HEIGHT + 4;
    for (int i = 0; w->title[i] && i < max_chars; i++) {
        bb_draw_char_transparent(w->title[i], x + i * 8, y, 0x00FFFFFF);
    }
}

void gui_render(void) {
    if (!is_graphics_mode || !is_gui_mode) return;

    int full = gui_full_dirty;
    int old_cx = rendered_cursor_x;
    int old_cy = rendered_cursor_y;
    int cur_cx = mouse_x;
    int cur_cy = mouse_y;

    gui_dirty = 0;
    gui_full_dirty = 0;

    /* 1. Desktop background */
    bb_fill_rect(0, 0, (int)g_width, (int)g_height - TASKBAR_HEIGHT, 0x001E293B);

    /* 2. Painter's algorithm: render windows from bottom to top */
    window_t* focused = gui_get_focused_window();
    for (int i = 0; i < window_count; i++) {
        window_t* w = z_order[i];
        if (!w || !w->is_used || !(w->flags & WIN_VISIBLE)) continue;

        int is_focused = (w == focused);
        if (w->flags & WIN_DECORATED) {
            uint32_t border_col = is_focused ? 0x003B82F6 : 0x00334155;
            uint32_t title_col = is_focused ? 0x002563EB : 0x00475569;

            /* Outer border & frame */
            bb_fill_rect(w->x - 2, w->y - TITLEBAR_HEIGHT - 2,
                         w->width + 4, w->height + TITLEBAR_HEIGHT + 4, border_col);

            /* Titlebar */
            bb_fill_rect(w->x, w->y - TITLEBAR_HEIGHT, w->width, TITLEBAR_HEIGHT, title_col);
            draw_window_title_clipped(w);

            /* Close [X] button */
            int close_x = w->x + w->width - 20;
            int close_y = w->y - TITLEBAR_HEIGHT + 4;
            bb_fill_rect(close_x, close_y, 16, 16, 0x00EF4444);
            bb_draw_char_transparent('X', close_x + 4, close_y, 0x00FFFFFF);
        }

        if (w->canvas) {
            bb_blit_surface(w->canvas, w->width, w->height, w->x, w->y);
        }
    }

    /* 3. Bottom taskbar */
    draw_taskbar(focused);

    /* 4. Mouse cursor on top */
    bb_draw_sprite(cursor_sprite, 12, 19, cur_cx, cur_cy);

    /* 5. Flush to VRAM (full swap on window changes, dirty rects on cursor-only move) */
    if (full || old_cx < 0 || old_cy < 0) {
        swap_buffers();
    } else {
        flush_rect(old_cx, old_cy, 12, 19);
        flush_rect(cur_cx, cur_cy, 12, 19);
    }

    rendered_cursor_x = cur_cx;
    rendered_cursor_y = cur_cy;
}

static void gui_teardown(void) {
    uint32_t eflags = irq_save_disable();
    is_gui_mode = 0;
    gui_exit_requested = 0;
    gui_terminal_window = NULL;
    dragging_window = NULL;
    mouse_set_callback(NULL);
    irq_restore(eflags);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].is_used) {
            gui_destroy_window(i);
        }
    }

    term_fg_color = 0x00FFFFFF;
    term_bg_color = 0x00000000;
    clear_terminal();
    kprintf("[GUI] Exited to text shell.\n");
    if (shell_input_enabled && fg_task == -1) {
        print_shell_prompt();
    }
}

static void gui_compositor_task(void) {
    if (current_task > 0) {
        tasks[current_task].is_background = 1;
    }

    gui_render();

    while (is_gui_mode && !gui_exit_requested) {
        gui_process_input();
        if (gui_exit_requested || !is_gui_mode) break;

        if (gui_dirty) {
            gui_render();
        }

        uint32_t eflags = irq_save_disable();
        if (is_gui_mode && !gui_exit_requested && !gui_dirty && mq_head == mq_tail) {
            sleep_on(&gui_wait_queue);
        }
        irq_restore(eflags);
    }

    gui_teardown();
    kill_current_task();
}

void gui_start(void) {
    if (!is_graphics_mode || is_gui_mode) return;

    gui_init();

    int term_w = 640;
    int term_h = 400;
    int term_x = ((int)g_width - term_w) / 2;
    int term_y = ((int)g_height - TASKBAR_HEIGHT - term_h) / 2 + (TITLEBAR_HEIGHT / 2);
    if (term_x < 8) term_x = 8;
    if (term_y < TITLEBAR_HEIGHT + 4) term_y = TITLEBAR_HEIGHT + 4;

    int term_id = gui_create_window("Terminal", term_x, term_y, term_w, term_h,
                                    WIN_VISIBLE | WIN_DECORATED | WIN_BUILTIN, 0);
    if (term_id < 0) {
        kprintf("[GUI] Error: Failed to allocate Terminal window.\n");
        return;
    }

    gui_terminal_window = gui_get_window(term_id);
    term_fg_color = 0x00FFFFFF;
    term_bg_color = 0x00000000;
    cursor_x = 0;
    cursor_y = 0;
    is_gui_mode = 1;

    clear_terminal();
    kprintf("==================================================\n");
    kprintf("  sOS Desktop Terminal (80x25)                    \n");
    kprintf("  Press Ctrl+Q or click [Exit] to return to shell \n");
    kprintf("==================================================\n\n");

    create_task(gui_compositor_task);
}

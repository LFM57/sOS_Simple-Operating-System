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
volatile int is_gui_locked = 0;
volatile int is_gui_login = 0;
volatile int gui_logout_requested = 0;
wait_queue_t gui_wait_queue = {0};
static wait_queue_t gui_login_wait_queue = {0};
window_t* gui_terminal_window = NULL;

/* Lock screen state and IRQ1->compositor key ring buffer */
#define LOCK_KEY_Q_SIZE 32
static volatile char lock_key_queue[LOCK_KEY_Q_SIZE];
static volatile int lkq_head = 0;
static volatile int lkq_tail = 0;
static char lock_pass_buf[32];
static int lock_pass_len = 0;
static char lock_error_msg[64];

/* Graphical display manager (sOS Login) state */
#define GUI_SESSION_DESKTOP 0
#define GUI_SESSION_TEXT    1
static char login_user_buf[32];
static int login_user_len = 0;
static char login_pass_buf[32];
static int login_pass_len = 0;
static int login_focus_field = 0;   /* 0 = Username, 1 = Password */
static int login_session_mode = GUI_SESSION_DESKTOP;
static char login_error_msg[64];
static volatile int boot_login_done = 0;
static volatile int login_to_text_shell = 0;
static int power_menu_open = 0;

#define CTX_MENU_NONE    0
#define CTX_MENU_DESKTOP 1
#define CTX_MENU_WINDOW  2
#define CTX_MENU_W       148
#define CTX_ITEM_H       26
static int ctx_menu_type = CTX_MENU_NONE;
static int ctx_menu_x = 0;
static int ctx_menu_y = 0;
static int ctx_menu_win_id = -1;

/* Mouse packet ring buffer filled by IRQ12 and drained by gui_process_input() */
#define MOUSE_Q_SIZE 32
typedef struct {
    int x;
    int y;
    uint8_t buttons;
    int dz;
} mouse_pkt_t;

static volatile mouse_pkt_t mouse_queue[MOUSE_Q_SIZE];
static volatile int mq_head = 0;
static volatile int mq_tail = 0;

static int prev_mouse_x = -1;
static int prev_mouse_y = -1;
static uint8_t prev_buttons = 0;
static int rendered_cursor_x = -1;
static int rendered_cursor_y = -1;
#define CURSOR_W 12
#define CURSOR_H 19
static uint32_t cursor_bg[CURSOR_W * CURSOR_H];
static int cursor_bg_valid = 0;

static window_t* dragging_window = NULL;
static int drag_offset_x = 0;
static int drag_offset_y = 0;
static window_t* resizing_window = NULL;
static int resize_edges = 0;
static int resize_start_mx = 0;
static int resize_start_my = 0;
static int resize_orig_x = 0;
static int resize_orig_y = 0;
static int resize_orig_w = 0;
static int resize_orig_h = 0;
static int resize_preview_x = 0;
static int resize_preview_y = 0;
static int resize_preview_w = 0;
static int resize_preview_h = 0;
static window_t* mouse_capture_window = NULL;
static uint8_t mouse_capture_buttons = 0;

#define MIN_WIN_W 120
#define MIN_WIN_H 80
#define RESIZE_LEFT   0x01
#define RESIZE_RIGHT  0x02
#define RESIZE_TOP    0x04
#define RESIZE_BOTTOM 0x08

typedef struct {
    int x, y, w, h;
} gui_rect_t;

#define MAX_DIRTY_RECTS 16
static gui_rect_t screen_dirty_rects[MAX_DIRTY_RECTS];
static int screen_dirty_count = 0;

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

static int rects_overlap_or_touch(const gui_rect_t* a, const gui_rect_t* b) {
    return !(a->x + a->w < b->x || b->x + b->w < a->x ||
             a->y + a->h < b->y || b->y + b->h < a->y);
}

static void merge_rect_into(gui_rect_t* dst, const gui_rect_t* src) {
    int x0 = (dst->x < src->x) ? dst->x : src->x;
    int y0 = (dst->y < src->y) ? dst->y : src->y;
    int x1_a = dst->x + dst->w;
    int x1_b = src->x + src->w;
    int y1_a = dst->y + dst->h;
    int y1_b = src->y + src->h;
    int x1 = (x1_a > x1_b) ? x1_a : x1_b;
    int y1 = (y1_a > y1_b) ? y1_a : y1_b;
    dst->x = x0;
    dst->y = y0;
    dst->w = x1 - x0;
    dst->h = y1 - y0;
}

static void add_rect_to_list(gui_rect_t* list, int* count, int max_count, int x, int y, int w, int h) {
    if (!is_graphics_mode || g_width == 0 || g_height == 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_width) w = (int)g_width - x;
    if (y + h > (int)g_height) h = (int)g_height - y;
    if (w <= 0 || h <= 0) return;

    gui_rect_t r = { x, y, w, h };
    for (int i = 0; i < *count; i++) {
        if (rects_overlap_or_touch(&list[i], &r)) {
            merge_rect_into(&list[i], &r);
            return;
        }
    }
    if (*count < max_count) {
        list[(*count)++] = r;
    } else {
        merge_rect_into(&list[*count - 1], &r);
    }
}

static void get_window_frame_rect(const window_t* w, int* fx, int* fy, int* fw, int* fh) {
    int decorated = (w->flags & WIN_DECORATED) != 0;
    *fx = decorated ? (w->x - 2) : w->x;
    *fy = decorated ? (w->y - TITLEBAR_HEIGHT - 2) : w->y;
    *fw = decorated ? (w->width + 4) : w->width;
    *fh = decorated ? (w->height + TITLEBAR_HEIGHT + 4) : w->height;
}

static void gui_dirty_screen_rect_locked(int x, int y, int w, int h) {
    add_rect_to_list(screen_dirty_rects, &screen_dirty_count, MAX_DIRTY_RECTS, x, y, w, h);
    gui_dirty = 1;
}

static void gui_dirty_screen_rect(int x, int y, int w, int h) {
    uint32_t eflags = irq_save_disable();
    gui_dirty_screen_rect_locked(x, y, w, h);
    irq_restore(eflags);
}

static void gui_dirty_window_frame_locked(const window_t* w) {
    if (!w) return;
    int fx, fy, fw, fh;
    get_window_frame_rect(w, &fx, &fy, &fw, &fh);
    gui_dirty_screen_rect_locked(fx, fy, fw, fh);
}

static int is_window_shown(const window_t* w) {
    return w && w->is_used && (w->flags & WIN_VISIBLE) && !(w->flags & WIN_MINIMIZED);
}

static void get_context_menu_rect(int* mx, int* my, int* mw, int* mh) {
    int items = (ctx_menu_type == CTX_MENU_WINDOW) ? 4 : 3;
    int w = CTX_MENU_W;
    int h = items * CTX_ITEM_H + 8;
    int x = ctx_menu_x;
    int y = ctx_menu_y;
    int max_x = (int)g_width - w - 4;
    int max_y = (int)g_height - TASKBAR_HEIGHT - h - 4;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    *mx = x;
    *my = y;
    *mw = w;
    *mh = h;
}

static void gui_dirty_taskbar_locked(void) {
    if (g_height == 0 || g_width == 0) return;
    gui_dirty_screen_rect_locked(0, (int)g_height - TASKBAR_HEIGHT, (int)g_width, TASKBAR_HEIGHT);
}

static void close_context_menu(void) {
    if (ctx_menu_type == CTX_MENU_NONE) return;
    int cx, cy, cw, ch;
    get_context_menu_rect(&cx, &cy, &cw, &ch);
    ctx_menu_type = CTX_MENU_NONE;
    ctx_menu_win_id = -1;
    gui_dirty_screen_rect(cx, cy, cw, ch);
}

void gui_init(void) {
    uint32_t eflags = irq_save_disable();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].id = i;
        windows[i].is_used = 0;
        windows[i].canvas = NULL;
        windows[i].dirty = 0;
        windows[i].dirty_x = 0;
        windows[i].dirty_y = 0;
        windows[i].dirty_w = 0;
        windows[i].dirty_h = 0;
        windows[i].ev_head = 0;
        windows[i].ev_tail = 0;
        windows[i].event_queue.head = NULL;
        z_order[i] = NULL;
    }
    window_count = 0;
    dragging_window = NULL;
    resizing_window = NULL;
    resize_edges = 0;
    mouse_capture_window = NULL;
    mouse_capture_buttons = 0;
    screen_dirty_count = 0;
    mq_head = 0;
    mq_tail = 0;
    prev_mouse_x = mouse_x;
    prev_mouse_y = mouse_y;
    prev_buttons = mouse_buttons;
    rendered_cursor_x = -1;
    rendered_cursor_y = -1;
    cursor_bg_valid = 0;
    gui_dirty = 1;
    gui_full_dirty = 1;
    gui_exit_requested = 0;
    is_gui_locked = 0;
    is_gui_login = 0;
    gui_logout_requested = 0;
    login_to_text_shell = 0;
    power_menu_open = 0;
    ctx_menu_type = CTX_MENU_NONE;
    ctx_menu_win_id = -1;
    lkq_head = 0;
    lkq_tail = 0;
    lock_pass_len = 0;
    lock_pass_buf[0] = '\0';
    lock_error_msg[0] = '\0';
    login_user_len = 0;
    login_user_buf[0] = '\0';
    login_pass_len = 0;
    login_pass_buf[0] = '\0';
    login_focus_field = 0;
    login_session_mode = GUI_SESSION_DESKTOP;
    login_error_msg[0] = '\0';
    gui_terminal_window = NULL;
    mouse_set_callback(gui_on_mouse_packet);
    irq_restore(eflags);
}

void gui_request_exit(void) {
    if (is_gui_locked && !gui_logout_requested) return;
    if (is_gui_login && !login_to_text_shell) return;
    power_menu_open = 0;
    ctx_menu_type = CTX_MENU_NONE;
    ctx_menu_win_id = -1;
    gui_exit_requested = 1;
    gui_dirty = 1;
    wake_up(&gui_wait_queue);
}

void gui_lock_screen(void) {
    if (!is_graphics_mode || !is_gui_mode || is_gui_locked || is_gui_login) return;
    uint32_t eflags = irq_save_disable();
    is_gui_locked = 1;
    power_menu_open = 0;
    ctx_menu_type = CTX_MENU_NONE;
    ctx_menu_win_id = -1;
    dragging_window = NULL;
    resizing_window = NULL;
    resize_edges = 0;
    mouse_capture_window = NULL;
    mouse_capture_buttons = 0;
    lkq_head = 0;
    lkq_tail = 0;
    lock_pass_len = 0;
    lock_pass_buf[0] = '\0';
    lock_error_msg[0] = '\0';
    gui_full_dirty = 1;
    gui_dirty = 1;
    irq_restore(eflags);
    wake_up(&gui_wait_queue);
}

static void push_modal_key(char ascii) {
    uint32_t eflags = irq_save_disable();
    int next = (lkq_head + 1) % LOCK_KEY_Q_SIZE;
    if (next != lkq_tail) {
        lock_key_queue[lkq_head] = ascii;
        lkq_head = next;
    }
    gui_dirty = 1;
    irq_restore(eflags);
    wake_up(&gui_wait_queue);
}

void gui_lock_on_key_press(char ascii) {
    if (!is_gui_mode || !is_gui_locked || ascii == 0) return;
    push_modal_key(ascii);
}

void gui_login_on_key_press(char ascii) {
    if (!is_gui_mode || !is_gui_login || ascii == 0) return;
    push_modal_key(ascii);
}

void gui_mark_dirty(int full_redraw) {
    gui_dirty = 1;
    if (full_redraw) {
        gui_full_dirty = 1;
    }
    wake_up(&gui_wait_queue);
}

void gui_mark_window_dirty(window_t* w, int x, int y, int width, int height) {
    if (!w || !w->is_used || width <= 0 || height <= 0) return;

    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > w->width) width = w->width - x;
    if (y + height > w->height) height = w->height - y;
    if (width <= 0 || height <= 0) return;

    uint32_t eflags = irq_save_disable();
    if (!w->dirty) {
        w->dirty = 1;
        w->dirty_x = x;
        w->dirty_y = y;
        w->dirty_w = width;
        w->dirty_h = height;
    } else {
        int x0 = (w->dirty_x < x) ? w->dirty_x : x;
        int y0 = (w->dirty_y < y) ? w->dirty_y : y;
        int x1_a = w->dirty_x + w->dirty_w;
        int x1_b = x + width;
        int y1_a = w->dirty_y + w->dirty_h;
        int y1_b = y + height;
        int x1 = (x1_a > x1_b) ? x1_a : x1_b;
        int y1 = (y1_a > y1_b) ? y1_a : y1_b;
        w->dirty_x = x0;
        w->dirty_y = y0;
        w->dirty_w = x1 - x0;
        w->dirty_h = y1 - y0;
    }
    gui_dirty = 1;
    irq_restore(eflags);
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

    window_t* prev_focused = gui_get_focused_window();

    window_t* win = &windows[slot];
    win->id = slot;
    win->is_used = 1;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->saved_x = x;
    win->saved_y = y;
    win->saved_width = w;
    win->saved_height = h;
    copy_title(win->title, title ? title : "Window", sizeof(win->title));
    win->flags = flags;
    win->owner_pid = owner_pid;
    if (owner_pid > 0 && owner_pid < MAX_TASKS && tasks[owner_pid].state != 0) {
        win->owner_uid = tasks[owner_pid].uid;
    } else {
        win->owner_uid = current_uid;
    }
    win->canvas = canvas;
    win->dirty = 0;
    win->dirty_x = 0;
    win->dirty_y = 0;
    win->dirty_w = 0;
    win->dirty_h = 0;
    win->ev_head = 0;
    win->ev_tail = 0;
    win->event_queue.head = NULL;

    z_order[window_count++] = win;
    if (is_gui_login || is_gui_locked) {
        gui_full_dirty = 1;
    } else {
        if (prev_focused && (win->flags & WIN_VISIBLE)) {
            gui_dirty_window_frame_locked(prev_focused);
        }
        if (win->flags & WIN_VISIBLE) {
            gui_dirty_window_frame_locked(win);
        }
        gui_dirty_taskbar_locked();
    }
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

    int w = win->width;
    int h = win->height;
    int min_x = w, min_y = h;
    int max_x = -1, max_y = -1;

    uint32_t eflags = irq_save_disable();
    for (int y = 0; y < h; y++) {
        uint32_t* dst_row = win->canvas + (y * w);
        const uint32_t* src_row = buf + (y * w);
        for (int x = 0; x < w; x++) {
            uint32_t px = src_row[x];
            if (dst_row[x] != px) {
                dst_row[x] = px;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }
    irq_restore(eflags);

    if (max_x >= min_x && max_y >= min_y) {
        gui_mark_window_dirty(win, min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
    }
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
    if (resizing_window == win) {
        resizing_window = NULL;
        resize_edges = 0;
        gui_full_dirty = 1;
    }
    if (mouse_capture_window == win) {
        mouse_capture_window = NULL;
        mouse_capture_buttons = 0;
    }
    if (ctx_menu_type == CTX_MENU_WINDOW && ctx_menu_win_id == win_id) {
        int cmx, cmy, cmw, cmh;
        get_context_menu_rect(&cmx, &cmy, &cmw, &cmh);
        ctx_menu_type = CTX_MENU_NONE;
        ctx_menu_win_id = -1;
        if (!is_gui_login && !is_gui_locked) {
            gui_dirty_screen_rect_locked(cmx, cmy, cmw, cmh);
        }
    }

    int was_visible = is_window_shown(win);
    int was_in_taskbar = (win->flags & (WIN_VISIBLE | WIN_MINIMIZED)) != 0;
    int was_focused = (win == gui_get_focused_window());
    int fx = 0, fy = 0, fw = 0, fh = 0;
    if (was_visible) {
        get_window_frame_rect(win, &fx, &fy, &fw, &fh);
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
    win->dirty = 0;
    win->dirty_x = 0;
    win->dirty_y = 0;
    win->dirty_w = 0;
    win->dirty_h = 0;
    win->ev_head = 0;
    win->ev_tail = 0;

    wake_up(&win->event_queue);
    if (is_gui_login || is_gui_locked) {
        gui_full_dirty = 1;
    } else {
        if (was_visible) {
            gui_dirty_screen_rect_locked(fx, fy, fw, fh);
        }
        if (was_in_taskbar) {
            gui_dirty_taskbar_locked();
        }
        if (was_focused) {
            window_t* new_top = gui_get_focused_window();
            if (new_top) {
                gui_dirty_window_frame_locked(new_top);
            }
        }
    }
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

void gui_destroy_windows_by_uid(uint8_t uid) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].is_used && windows[i].owner_uid == uid) {
            uint32_t pid = windows[i].owner_pid;
            if (&windows[i] == gui_terminal_window) {
                gui_terminal_window = NULL;
            }
            gui_destroy_window(i);
            if (pid > 0 && pid < (MAX_TASKS - 1) &&
                tasks[pid].state != 0 &&
                tasks[pid].page_directory != kernel_page_directory) {
                terminate_task((int)pid);
            }
        }
    }
    for (int pid = 1; pid < (MAX_TASKS - 1); pid++) {
        if (tasks[pid].state != 0 &&
            tasks[pid].uid == uid &&
            tasks[pid].page_directory != kernel_page_directory) {
            terminate_task(pid);
        }
    }
    if (fg_task > 0 && fg_task < (MAX_TASKS - 1) &&
        tasks[fg_task].state != 0 &&
        tasks[fg_task].uid == uid) {
        terminate_task((int)fg_task);
    }
    wake_up(&exit_queue);
}

window_t* gui_get_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return NULL;
    if (!windows[win_id].is_used) return NULL;
    return &windows[win_id];
}

window_t* gui_get_focused_window(void) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (is_window_shown(z_order[i])) {
            return z_order[i];
        }
    }
    return NULL;
}

void gui_focus_window(window_t* w) {
    if (!w || !w->is_used) return;
    uint32_t eflags = irq_save_disable();
    window_t* prev_focused = gui_get_focused_window();
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
        if (is_gui_login || is_gui_locked) {
            gui_full_dirty = 1;
        } else {
            if (prev_focused && prev_focused != w) {
                gui_dirty_window_frame_locked(prev_focused);
            }
            gui_dirty_window_frame_locked(w);
            gui_dirty_taskbar_locked();
        }
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
void gui_on_mouse_packet(int x, int y, uint8_t buttons, int dz) {
    int next = (mq_head + 1) % MOUSE_Q_SIZE;
    if (next == mq_tail) {
        mq_tail = (mq_tail + 1) % MOUSE_Q_SIZE;
    }
    mouse_queue[mq_head].x = x;
    mouse_queue[mq_head].y = y;
    mouse_queue[mq_head].buttons = buttons;
    mouse_queue[mq_head].dz = dz;
    mq_head = next;

    gui_dirty = 1;
    wake_up(&gui_wait_queue);
}

static int gui_compositor_tid = -1;

/* Called from IRQ1 keyboard_handler() when GUI mode is active */
void gui_on_key_press(char ascii, uint8_t modifiers, uint8_t scancode) {
    if (ascii == 0 && scancode == 0) return;
    if (is_gui_login) {
        if (ascii != 0) {
            gui_login_on_key_press(ascii);
        }
        return;
    }
    if (is_gui_locked) {
        if (ascii != 0) {
            gui_lock_on_key_press(ascii);
        }
        return;
    }
    window_t* top = gui_get_focused_window();
    if (!top || top == gui_terminal_window) return;

    gui_event_t ev = {0};
    ev.type = GUI_EVENT_KEY_PRESS;
    ev.key = ascii;
    ev.modifiers = modifiers;
    ev.scancode = scancode;
    gui_push_event(top, &ev);
}

static void get_lock_dialog_rect(int* dx, int* dy, int* dw, int* dh) {
    int w = 420;
    int h = 230;
    *dw = w;
    *dh = h;
    *dx = ((int)g_width - w) / 2;
    *dy = ((int)g_height - h) / 2;
}

static void get_login_dialog_rect(int* dx, int* dy, int* dw, int* dh) {
    int w = 460;
    int h = 324;
    *dw = w;
    *dh = h;
    *dx = ((int)g_width - w) / 2;
    *dy = ((int)g_height - TASKBAR_HEIGHT - h) / 2;
}

static void get_power_menu_rect(int* mx, int* my, int* mw, int* mh) {
    int w = 136;
    int h = 120;
    *mw = w;
    *mh = h;
    *mx = (int)g_width - w - 8;
    *my = (int)g_height - TASKBAR_HEIGHT - h - 4;
}

static int create_desktop_terminal_window(int with_prompt) {
    int term_w = 640;
    int term_h = 400;
    int term_x = ((int)g_width - term_w) / 2;
    int term_y = ((int)g_height - TASKBAR_HEIGHT - term_h) / 2 + (TITLEBAR_HEIGHT / 2);
    if (term_x < 8) term_x = 8;
    if (term_y < TITLEBAR_HEIGHT + 4) term_y = TITLEBAR_HEIGHT + 4;

    int term_id = gui_create_window("Terminal", term_x, term_y, term_w, term_h,
                                    WIN_VISIBLE | WIN_DECORATED | WIN_BUILTIN, 0);
    if (term_id < 0) {
        return -1;
    }

    gui_terminal_window = gui_get_window(term_id);
    term_fg_color = 0x00FFFFFF;
    term_bg_color = 0x00000000;
    cursor_x = 0;
    cursor_y = 0;

    clear_terminal();
    kprintf("==================================================\n");
    kprintf("  sOS Desktop Terminal (80x25)                    \n");
    kprintf("  Press Ctrl+L to lock, Ctrl+Q for Text Shell     \n");
    kprintf("==================================================\n\n");
    if (with_prompt) {
        kprintf("Welcome, %s!\n", current_user);
        kprintf("Type 'help' for a list of available commands.\n");
        shell_input_enabled = 1;
        print_shell_prompt();
    }
    return term_id;
}

void gui_show_login_screen(void) {
    uint32_t eflags = irq_save_disable();
    is_gui_locked = 0;
    is_gui_login = 1;
    power_menu_open = 0;
    ctx_menu_type = CTX_MENU_NONE;
    ctx_menu_win_id = -1;
    dragging_window = NULL;
    resizing_window = NULL;
    resize_edges = 0;
    mouse_capture_window = NULL;
    mouse_capture_buttons = 0;
    login_to_text_shell = 0;
    lkq_head = 0;
    lkq_tail = 0;
    for (int i = 0; i < (int)sizeof(login_user_buf); i++) {
        login_user_buf[i] = '\0';
        login_pass_buf[i] = '\0';
    }
    login_user_len = 0;
    login_pass_len = 0;
    login_focus_field = 0;
    login_session_mode = GUI_SESSION_DESKTOP;
    login_error_msg[0] = '\0';
    gui_full_dirty = 1;
    gui_dirty = 1;
    irq_restore(eflags);
    wake_up(&gui_wait_queue);
}

void gui_logout_to_login(void) {
    uint8_t prev_uid = current_uid;
    gui_destroy_windows_by_uid(prev_uid);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].is_used) {
            if (&windows[i] == gui_terminal_window) {
                gui_terminal_window = NULL;
            }
            gui_destroy_window(i);
        }
    }
    if (gui_compositor_tid > 0 && gui_compositor_tid < MAX_TASKS) {
        tasks[gui_compositor_tid].uid = 0;
    }
    reset_session_state();
    gui_logout_requested = 1;
    gui_show_login_screen();
    wake_up(&console_queue);
}

static void gui_perform_power_action(int is_reboot) {
    bb_reset_clip();
    bb_fill_rect(0, 0, (int)g_width, (int)g_height, 0x000B1120);
    bb_draw_string_transparent(is_reboot ? "Rebooting sOS cleanly..." : "Shutting down sOS cleanly...",
                               (int)g_width / 2 - 104, (int)g_height / 2 - 8, 0x00F8FAFC);
    swap_buffers();
    if (is_reboot) {
        system_reboot();
    } else {
        system_shutdown();
    }
}

static void attempt_gui_unlock(void) {
    char real_pass[32] = {0};
    uint8_t uid = 0;
    int ok = get_user_info(current_user, real_pass, &uid) &&
             check_password(lock_pass_buf, real_pass);

    for (int i = 0; i < (int)sizeof(lock_pass_buf); i++) {
        lock_pass_buf[i] = '\0';
        real_pass[i] = '\0';
    }
    lock_pass_len = 0;

    if (ok) {
        lock_error_msg[0] = '\0';
        is_gui_locked = 0;
    } else {
        copy_title(lock_error_msg, "Incorrect password. Try again.", sizeof(lock_error_msg));
    }
    gui_full_dirty = 1;
    gui_dirty = 1;
}

static void attempt_gui_login(void) {
    if (login_user_len == 0) {
        copy_title(login_error_msg, "Please enter a username.", sizeof(login_error_msg));
        login_focus_field = 0;
        gui_full_dirty = 1;
        gui_dirty = 1;
        return;
    }

    char real_pass[32] = {0};
    uint8_t uid = 0;
    int ok = get_user_info(login_user_buf, real_pass, &uid) &&
             check_password(login_pass_buf, real_pass);

    for (int i = 0; i < (int)sizeof(login_pass_buf); i++) {
        login_pass_buf[i] = '\0';
        real_pass[i] = '\0';
    }
    login_pass_len = 0;

    if (!ok) {
        copy_title(login_error_msg, "Login incorrect. Please try again.", sizeof(login_error_msg));
        login_focus_field = 1;
        gui_full_dirty = 1;
        gui_dirty = 1;
        return;
    }

    complete_user_login(login_user_buf, uid);
    if (gui_compositor_tid > 0 && gui_compositor_tid < MAX_TASKS) {
        tasks[gui_compositor_tid].uid = uid;
    }

    for (int i = 0; i < (int)sizeof(login_user_buf); i++) {
        login_user_buf[i] = '\0';
    }
    login_user_len = 0;
    login_error_msg[0] = '\0';

    if (login_session_mode == GUI_SESSION_TEXT) {
        login_to_text_shell = 1;
        boot_login_done = 1;
        gui_request_exit();
        wake_up(&gui_login_wait_queue);
    } else {
        is_gui_login = 0;
        create_desktop_terminal_window(1);
        boot_login_done = 1;
        gui_full_dirty = 1;
        gui_dirty = 1;
        wake_up(&gui_login_wait_queue);
    }
}

static void handle_login_mouse_step(int mx, int my, uint8_t buttons) {
    uint8_t prev_left = prev_buttons & 0x01;
    uint8_t curr_left = buttons & 0x01;

    if (mx != prev_mouse_x || my != prev_mouse_y) {
        gui_dirty = 1;
    }

    if (!prev_left && curr_left) {
        int dx, dy, dw, dh;
        get_login_dialog_rect(&dx, &dy, &dw, &dh);

        int user_x = dx + 28, user_y = dy + 70, field_w = dw - 56, field_h = 28;
        int pass_x = dx + 28, pass_y = dy + 130;
        int sess_gui_x = dx + 108, sess_y = dy + 174, sess_w = 148, sess_h = 28;
        int sess_txt_x = dx + 268;
        int signin_x = dx + 28, signin_y = dy + 250, signin_w = dw - 56, signin_h = 34;

        int bar_y = (int)g_height - TASKBAR_HEIGHT;
        int shut_x = (int)g_width - 84;
        int reb_x = shut_x - 74;

        if (mx >= user_x && mx < user_x + field_w && my >= user_y && my < user_y + field_h) {
            login_focus_field = 0;
            gui_full_dirty = 1;
            gui_dirty = 1;
        } else if (mx >= pass_x && mx < pass_x + field_w && my >= pass_y && my < pass_y + field_h) {
            login_focus_field = 1;
            gui_full_dirty = 1;
            gui_dirty = 1;
        } else if (mx >= sess_gui_x && mx < sess_gui_x + sess_w && my >= sess_y && my < sess_y + sess_h) {
            login_session_mode = GUI_SESSION_DESKTOP;
            gui_full_dirty = 1;
            gui_dirty = 1;
        } else if (mx >= sess_txt_x && mx < sess_txt_x + sess_w && my >= sess_y && my < sess_y + sess_h) {
            login_session_mode = GUI_SESSION_TEXT;
            gui_full_dirty = 1;
            gui_dirty = 1;
        } else if (mx >= signin_x && mx < signin_x + signin_w && my >= signin_y && my < signin_y + signin_h) {
            attempt_gui_login();
        } else if (my >= bar_y && mx >= reb_x && mx < reb_x + 68) {
            gui_perform_power_action(1);
        } else if (my >= bar_y && mx >= shut_x && mx < shut_x + 76) {
            gui_perform_power_action(0);
        }
    }

    prev_mouse_x = mx;
    prev_mouse_y = my;
    prev_buttons = buttons;
}

static void handle_locked_mouse_step(int mx, int my, uint8_t buttons) {
    uint8_t prev_left = prev_buttons & 0x01;
    uint8_t curr_left = buttons & 0x01;

    if (mx != prev_mouse_x || my != prev_mouse_y) {
        gui_dirty = 1;
    }

    if (!prev_left && curr_left) {
        int dx, dy, dw, dh;
        get_lock_dialog_rect(&dx, &dy, &dw, &dh);

        int unlock_x = dx + 24;
        int unlock_y = dy + 168;
        int unlock_w = 148;
        int unlock_h = 32;

        int switch_x = dx + 188;
        int switch_y = dy + 168;
        int switch_w = 208;
        int switch_h = 32;

        if (mx >= unlock_x && mx < unlock_x + unlock_w &&
            my >= unlock_y && my < unlock_y + unlock_h) {
            attempt_gui_unlock();
        } else if (mx >= switch_x && mx < switch_x + switch_w &&
                   my >= switch_y && my < switch_y + switch_h) {
            gui_logout_to_login();
        }
    }

    prev_mouse_x = mx;
    prev_mouse_y = my;
    prev_buttons = buttons;
}

static void deliver_captured_mouse_up(int mx, int my, uint8_t button) {
    uint8_t mask = (button == 1) ? 0x01 : 0x02;
    if (!(mouse_capture_buttons & mask)) return;
    mouse_capture_buttons &= (uint8_t)~mask;

    window_t* target = mouse_capture_window;
    if (mouse_capture_buttons == 0) {
        mouse_capture_window = NULL;
    }
    if (!target || !target->is_used) return;

    int rx = mx - target->x;
    int ry = my - target->y;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx >= target->width) rx = target->width - 1;
    if (ry >= target->height) ry = target->height - 1;

    gui_event_t ev = {0};
    ev.type = GUI_EVENT_MOUSE_UP;
    ev.x = rx;
    ev.y = ry;
    ev.button = button;
    gui_push_event(target, &ev);
}

static void gui_minimize_window(window_t* w) {
    if (!w || !w->is_used) return;
    uint32_t eflags = irq_save_disable();
    int was_focused = (w == gui_get_focused_window());
    int fx, fy, fw, fh;
    get_window_frame_rect(w, &fx, &fy, &fw, &fh);
    w->flags &= ~WIN_VISIBLE;
    w->flags |= WIN_MINIMIZED;
    if (dragging_window == w) {
        dragging_window = NULL;
    }
    if (resizing_window == w) {
        resizing_window = NULL;
        resize_edges = 0;
        gui_full_dirty = 1;
    }
    if (mouse_capture_window == w) {
        mouse_capture_window = NULL;
        mouse_capture_buttons = 0;
    }
    if (is_gui_login || is_gui_locked) {
        gui_full_dirty = 1;
    } else {
        gui_dirty_screen_rect_locked(fx, fy, fw, fh);
        gui_dirty_taskbar_locked();
        if (was_focused) {
            window_t* new_top = gui_get_focused_window();
            if (new_top) {
                gui_dirty_window_frame_locked(new_top);
            }
        }
    }
    gui_dirty = 1;
    irq_restore(eflags);
    wake_up(&gui_wait_queue);
}

static void gui_restore_and_focus_window(window_t* w) {
    if (!w || !w->is_used) return;
    uint32_t eflags = irq_save_disable();
    int was_hidden = !(w->flags & WIN_VISIBLE) || (w->flags & WIN_MINIMIZED);
    w->flags |= WIN_VISIBLE;
    w->flags &= ~WIN_MINIMIZED;
    if (was_hidden && !is_gui_login && !is_gui_locked) {
        gui_dirty_window_frame_locked(w);
        gui_dirty_taskbar_locked();
    }
    gui_dirty = 1;
    irq_restore(eflags);
    gui_focus_window(w);
}

static void gui_show_or_create_terminal(void) {
    if (gui_terminal_window && gui_terminal_window->is_used) {
        gui_restore_and_focus_window(gui_terminal_window);
    } else {
        create_desktop_terminal_window(1);
    }
}

static void gui_toggle_taskbar_window(window_t* w) {
    if (!w || !w->is_used) return;
    window_t* focused = gui_get_focused_window();
    if (w == focused && is_window_shown(w)) {
        gui_minimize_window(w);
    } else {
        gui_restore_and_focus_window(w);
    }
}

static void dirty_resize_preview_outline(int cx, int cy, int cw, int ch) {
    int fx = cx - 2;
    int fy = cy - TITLEBAR_HEIGHT - 2;
    int fw = cw + 4;
    int fh = ch + TITLEBAR_HEIGHT + 4;
    gui_dirty_screen_rect(fx, fy, fw, 2);
    gui_dirty_screen_rect(fx, fy + fh - 2, fw, 2);
    gui_dirty_screen_rect(fx, fy, 2, fh);
    gui_dirty_screen_rect(fx + fw - 2, fy, 2, fh);
}

static void gui_resize_window(window_t* w, int new_x, int new_y, int new_w, int new_h) {
    if (!w || !w->is_used) return;

    int max_w = (int)g_width - 4;
    int max_h = (int)g_height - TASKBAR_HEIGHT - TITLEBAR_HEIGHT - 4;
    if (max_w < MIN_WIN_W) max_w = MIN_WIN_W;
    if (max_h < MIN_WIN_H) max_h = MIN_WIN_H;

    if (new_w < MIN_WIN_W) new_w = MIN_WIN_W;
    if (new_h < MIN_WIN_H) new_h = MIN_WIN_H;
    if (new_w > max_w) new_w = max_w;
    if (new_h > max_h) new_h = max_h;

    if (new_x == w->x && new_y == w->y && new_w == w->width && new_h == w->height) {
        return;
    }

    int size_changed = (new_w != w->width || new_h != w->height);
    uint32_t* new_canvas = NULL;
    uint32_t* old_canvas = NULL;

    if (size_changed) {
        uint32_t buf_bytes = (uint32_t)new_w * (uint32_t)new_h * 4U;
        new_canvas = (uint32_t*)kmalloc(buf_bytes);
        if (!new_canvas) return;

        uint32_t bg_fill = (w == gui_terminal_window) ? term_bg_color : 0x00F8FAFC;
        uint32_t total_px = (uint32_t)new_w * (uint32_t)new_h;
        for (uint32_t i = 0; i < total_px; i++) {
            new_canvas[i] = bg_fill;
        }

        if (w == gui_terminal_window) {
            gui_terminal_before_resize();
        }

        if (w->canvas) {
            int copy_w = (w->width < new_w) ? w->width : new_w;
            int copy_h = (w->height < new_h) ? w->height : new_h;
            for (int row = 0; row < copy_h; row++) {
                const uint32_t* src = w->canvas + (row * w->width);
                uint32_t* dst = new_canvas + (row * new_w);
                for (int col = 0; col < copy_w; col++) {
                    dst[col] = src[col];
                }
            }
        }
    }

    uint32_t eflags = irq_save_disable();
    int old_fx, old_fy, old_fw, old_fh;
    get_window_frame_rect(w, &old_fx, &old_fy, &old_fw, &old_fh);

    w->x = new_x;
    w->y = new_y;
    if (size_changed) {
        old_canvas = w->canvas;
        w->canvas = new_canvas;
        w->width = new_w;
        w->height = new_h;
        w->dirty = 0;
        if (w == gui_terminal_window) {
            gui_terminal_after_resize();
        }
    }

    int new_fx, new_fy, new_fw, new_fh;
    get_window_frame_rect(w, &new_fx, &new_fy, &new_fw, &new_fh);

    if (is_gui_login || is_gui_locked) {
        gui_full_dirty = 1;
    } else if (is_window_shown(w)) {
        gui_dirty_screen_rect_locked(old_fx, old_fy, old_fw, old_fh);
        gui_dirty_screen_rect_locked(new_fx, new_fy, new_fw, new_fh);
    }
    gui_dirty = 1;
    irq_restore(eflags);

    if (old_canvas) {
        kfree(old_canvas);
    }

    if (size_changed && !(w->flags & WIN_BUILTIN)) {
        gui_event_t ev = {0};
        ev.type = GUI_EVENT_WINDOW_RESIZE;
        ev.width = new_w;
        ev.height = new_h;
        gui_push_event(w, &ev);
    }
    wake_up(&gui_wait_queue);
}

static void gui_toggle_maximize_window(window_t* w) {
    if (!w || !w->is_used || !(w->flags & WIN_DECORATED)) return;
    if (w->flags & WIN_MAXIMIZED) {
        w->flags &= ~WIN_MAXIMIZED;
        gui_resize_window(w, w->saved_x, w->saved_y, w->saved_width, w->saved_height);
    } else {
        w->saved_x = w->x;
        w->saved_y = w->y;
        w->saved_width = w->width;
        w->saved_height = w->height;
        w->flags |= WIN_MAXIMIZED;
        int max_x = 2;
        int max_y = TITLEBAR_HEIGHT + 2;
        int max_w = (int)g_width - 4;
        int max_h = (int)g_height - TASKBAR_HEIGHT - TITLEBAR_HEIGHT - 4;
        gui_resize_window(w, max_x, max_y, max_w, max_h);
    }
}

static int detect_resize_edges(const window_t* w, int mx, int my) {
    if (!w || !(w->flags & WIN_DECORATED) || (w->flags & WIN_MAXIMIZED)) return 0;

    int fx, fy, fw, fh;
    get_window_frame_rect(w, &fx, &fy, &fw, &fh);
    if (mx < fx || mx >= fx + fw || my < fy || my >= fy + fh) return 0;

    /* Leave titlebar action buttons clickable */
    int btn_top = w->y - TITLEBAR_HEIGHT + 4;
    if (mx >= w->x + w->width - 62 && my >= btn_top && my < w->y) {
        return 0;
    }

    /* Larger 12x12 bottom-right corner grip */
    if (mx >= w->x + w->width - 10 && my >= w->y + w->height - 10) {
        return RESIZE_RIGHT | RESIZE_BOTTOM;
    }

    int edges = 0;
    if (mx < w->x + 3) edges |= RESIZE_LEFT;
    else if (mx >= w->x + w->width - 3) edges |= RESIZE_RIGHT;

    if (my < fy + 5) edges |= RESIZE_TOP;
    else if (my >= w->y + w->height - 3) edges |= RESIZE_BOTTOM;

    return edges;
}

static void compute_resized_rect(int mx, int my, int* out_x, int* out_y, int* out_w, int* out_h) {
    int dx = mx - resize_start_mx;
    int dy = my - resize_start_my;
    int nx = resize_orig_x;
    int ny = resize_orig_y;
    int nw = resize_orig_w;
    int nh = resize_orig_h;
    int right = resize_orig_x + resize_orig_w;
    int bottom = resize_orig_y + resize_orig_h;

    int work_left = 2;
    int work_top = TITLEBAR_HEIGHT + 2;
    int work_right = (int)g_width - 2;
    int work_bottom = (int)g_height - TASKBAR_HEIGHT - 2;

    if (resize_edges & RESIZE_LEFT) {
        nx = resize_orig_x + dx;
        if (nx < work_left) nx = work_left;
        if (right - nx < MIN_WIN_W) nx = right - MIN_WIN_W;
        nw = right - nx;
    } else if (resize_edges & RESIZE_RIGHT) {
        nw = resize_orig_w + dx;
        if (nw < MIN_WIN_W) nw = MIN_WIN_W;
        if (nx + nw > work_right) nw = work_right - nx;
    }

    if (resize_edges & RESIZE_TOP) {
        ny = resize_orig_y + dy;
        if (ny < work_top) ny = work_top;
        if (bottom - ny < MIN_WIN_H) ny = bottom - MIN_WIN_H;
        nh = bottom - ny;
    } else if (resize_edges & RESIZE_BOTTOM) {
        nh = resize_orig_h + dy;
        if (nh < MIN_WIN_H) nh = MIN_WIN_H;
        if (ny + nh > work_bottom) nh = work_bottom - ny;
    }

    *out_x = nx;
    *out_y = ny;
    *out_w = nw;
    *out_h = nh;
}

static void handle_mouse_step(int mx, int my, uint8_t buttons, int dz) {
    uint8_t prev_left = prev_buttons & 0x01;
    uint8_t curr_left = buttons & 0x01;
    uint8_t prev_right = prev_buttons & 0x02;
    uint8_t curr_right = buttons & 0x02;

    if (mx != prev_mouse_x || my != prev_mouse_y) {
        if (resizing_window && resizing_window->is_used) {
            int nx, ny, nw, nh;
            compute_resized_rect(mx, my, &nx, &ny, &nw, &nh);
            if (nx != resize_preview_x || ny != resize_preview_y ||
                nw != resize_preview_w || nh != resize_preview_h) {
                dirty_resize_preview_outline(resize_preview_x, resize_preview_y,
                                             resize_preview_w, resize_preview_h);
                resize_preview_x = nx;
                resize_preview_y = ny;
                resize_preview_w = nw;
                resize_preview_h = nh;
                dirty_resize_preview_outline(resize_preview_x, resize_preview_y,
                                             resize_preview_w, resize_preview_h);
            }
        } else if (dragging_window && dragging_window->is_used) {
            int old_fx, old_fy, old_fw, old_fh;
            get_window_frame_rect(dragging_window, &old_fx, &old_fy, &old_fw, &old_fh);

            int new_x = mx - drag_offset_x;
            int new_y = my - drag_offset_y;
            int keep_visible = (dragging_window->width < 48) ? dragging_window->width : 48;
            int min_x = keep_visible - dragging_window->width;
            int max_x = (int)g_width - keep_visible;
            int min_y = TITLEBAR_HEIGHT + 2;
            int max_y = (int)g_height - TASKBAR_HEIGHT - 4;
            if (new_x < min_x) new_x = min_x;
            if (new_x > max_x) new_x = max_x;
            if (new_y < min_y) new_y = min_y;
            if (new_y > max_y) new_y = max_y;
            if (new_x != dragging_window->x || new_y != dragging_window->y) {
                dragging_window->x = new_x;
                dragging_window->y = new_y;
                int new_fx, new_fy, new_fw, new_fh;
                get_window_frame_rect(dragging_window, &new_fx, &new_fy, &new_fw, &new_fh);
                gui_dirty_screen_rect(old_fx, old_fy, old_fw, old_fh);
                gui_dirty_screen_rect(new_fx, new_fy, new_fw, new_fh);
            }
        } else if (!power_menu_open && ctx_menu_type == CTX_MENU_NONE &&
                   my < (int)g_height - TASKBAR_HEIGHT) {
            window_t* top = gui_get_focused_window();
            if (top && mx >= top->x && mx < top->x + top->width &&
                my >= top->y && my < top->y + top->height) {
                gui_event_t ev = {0};
                ev.type = GUI_EVENT_MOUSE_MOVE;
                ev.x = mx - top->x;
                ev.y = my - top->y;
                ev.button = buttons;
                gui_push_event(top, &ev);
            }
        }
        gui_dirty = 1;
    }

    if (dz != 0 && !resizing_window && !dragging_window &&
        !power_menu_open && ctx_menu_type == CTX_MENU_NONE &&
        my < (int)g_height - TASKBAR_HEIGHT) {
        for (int i = window_count - 1; i >= 0; i--) {
            window_t* w = z_order[i];
            if (!is_window_shown(w)) continue;
            int fx, fy, fw, fh;
            get_window_frame_rect(w, &fx, &fy, &fw, &fh);
            if (mx >= fx && mx < fx + fw && my >= fy && my < fy + fh) {
                if (mx >= w->x && mx < w->x + w->width &&
                    my >= w->y && my < w->y + w->height) {
                    gui_event_t ev = {0};
                    ev.type = GUI_EVENT_MOUSE_SCROLL;
                    ev.x = mx - w->x;
                    ev.y = my - w->y;
                    ev.button = buttons;
                    ev.dz = dz;
                    gui_push_event(w, &ev);
                }
                break;
            }
        }
    }

    if (!prev_left && curr_left) {
        /* Handle open desktop/titlebar context menu first */
        if (ctx_menu_type != CTX_MENU_NONE) {
            int cmx, cmy, cmw, cmh;
            get_context_menu_rect(&cmx, &cmy, &cmw, &cmh);
            int mtype = ctx_menu_type;
            int target_id = ctx_menu_win_id;
            close_context_menu();

            if (mx >= cmx && mx < cmx + cmw && my >= cmy + 4 && my < cmy + cmh - 4) {
                int item = (my - (cmy + 4)) / CTX_ITEM_H;
                if (mtype == CTX_MENU_DESKTOP) {
                    if (item == 0) {
                        gui_show_or_create_terminal();
                    } else if (item == 1) {
                        gui_launch_demo_app();
                    } else if (item == 2) {
                        gui_lock_screen();
                    }
                } else if (mtype == CTX_MENU_WINDOW) {
                    window_t* tw = gui_get_window(target_id);
                    if (item == 0 && tw) {
                        gui_restore_and_focus_window(tw);
                    } else if (item == 1 && tw) {
                        gui_minimize_window(tw);
                    } else if (item == 2 && tw) {
                        if (tw == gui_terminal_window) {
                            gui_minimize_window(tw);
                        } else {
                            gui_event_t ev = {0};
                            ev.type = GUI_EVENT_WINDOW_CLOSE;
                            ev.button = 1;
                            gui_push_event(tw, &ev);
                        }
                    } else if (item == 3) {
                        gui_show_or_create_terminal();
                    }
                }
                prev_mouse_x = mx;
                prev_mouse_y = my;
                prev_buttons = buttons;
                return;
            }
        }

        /* Handle open Power popup menu */
        if (power_menu_open) {
            int pmx, pmy, pmw, pmh;
            get_power_menu_rect(&pmx, &pmy, &pmw, &pmh);
            int power_x = (int)g_width - 74;

            if (mx >= pmx && mx < pmx + pmw && my >= pmy + 4 && my < pmy + pmh - 4) {
                int item = (my - (pmy + 4)) / 28;
                power_menu_open = 0;
                gui_dirty_screen_rect(pmx, pmy, pmw, pmh);
                gui_dirty_screen_rect(0, (int)g_height - TASKBAR_HEIGHT, (int)g_width, TASKBAR_HEIGHT);
                if (item == 0) {
                    /* Text Shell */
                    gui_request_exit();
                } else if (item == 1) {
                    /* Log Out */
                    gui_logout_to_login();
                } else if (item == 2) {
                    /* Reboot */
                    gui_perform_power_action(1);
                } else if (item == 3) {
                    /* Shutdown */
                    gui_perform_power_action(0);
                }
                prev_mouse_x = mx;
                prev_mouse_y = my;
                prev_buttons = buttons;
                return;
            }

            power_menu_open = 0;
            gui_dirty_screen_rect(pmx, pmy, pmw, pmh);
            gui_dirty_screen_rect(0, (int)g_height - TASKBAR_HEIGHT, (int)g_width, TASKBAR_HEIGHT);
            if (my >= (int)g_height - TASKBAR_HEIGHT && mx >= power_x && mx < power_x + 66) {
                prev_mouse_x = mx;
                prev_mouse_y = my;
                prev_buttons = buttons;
                return;
            }
        }

        /* Left click: check taskbar buttons and window chips first */
        if (my >= (int)g_height - TASKBAR_HEIGHT) {
            int power_x = (int)g_width - 74;
            int lock_x = power_x - 66;
            if (mx >= power_x && mx < power_x + 66) {
                int pmx, pmy, pmw, pmh;
                get_power_menu_rect(&pmx, &pmy, &pmw, &pmh);
                power_menu_open = !power_menu_open;
                gui_dirty_screen_rect(pmx, pmy, pmw, pmh);
                gui_dirty_screen_rect(0, (int)g_height - TASKBAR_HEIGHT, (int)g_width, TASKBAR_HEIGHT);
            } else if (mx >= lock_x && mx < lock_x + 60) {
                gui_lock_screen();
            } else {
                int chip_x = 8;
                int term_btn_w = 88;
                int demo_btn_w = 88;
                if (gui_terminal_window && mx >= chip_x && mx < chip_x + term_btn_w) {
                    gui_toggle_taskbar_window(gui_terminal_window);
                } else if (mx >= chip_x + term_btn_w + 6 && mx < chip_x + term_btn_w + 6 + demo_btn_w) {
                    gui_launch_demo_app();
                } else {
                    chip_x += term_btn_w + 6 + demo_btn_w + 10;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        if (!windows[i].is_used) continue;
                        if (!(windows[i].flags & (WIN_VISIBLE | WIN_MINIMIZED))) continue;
                        if (&windows[i] == gui_terminal_window) continue;
                        int chip_w = 120;
                        if (chip_x + chip_w > lock_x - 10) break;
                        if (mx >= chip_x && mx < chip_x + chip_w) {
                            gui_toggle_taskbar_window(&windows[i]);
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
                if (!is_window_shown(w)) continue;

                int fx, fy, fw, fh;
                get_window_frame_rect(w, &fx, &fy, &fw, &fh);
                int decorated = (w->flags & WIN_DECORATED) != 0;

                if (mx >= fx && mx < fx + fw && my >= fy && my < fy + fh) {
                    gui_focus_window(w);

                    if (decorated) {
                        int close_x = w->x + w->width - 20;
                        int max_x = w->x + w->width - 40;
                        int min_x = w->x + w->width - 60;
                        int btn_y = w->y - TITLEBAR_HEIGHT + 4;
                        if (my >= btn_y && my < btn_y + 16) {
                            if (mx >= close_x && mx < close_x + 16) {
                                if (w == gui_terminal_window) {
                                    gui_minimize_window(w);
                                } else {
                                    gui_event_t ev = {0};
                                    ev.type = GUI_EVENT_WINDOW_CLOSE;
                                    ev.button = 1;
                                    gui_push_event(w, &ev);
                                }
                                break;
                            } else if (mx >= max_x && mx < max_x + 16) {
                                gui_toggle_maximize_window(w);
                                break;
                            } else if (mx >= min_x && mx < min_x + 16) {
                                gui_minimize_window(w);
                                break;
                            }
                        }

                        int edges = detect_resize_edges(w, mx, my);
                        if (edges != 0) {
                            resizing_window = w;
                            resize_edges = edges;
                            resize_start_mx = mx;
                            resize_start_my = my;
                            resize_orig_x = w->x;
                            resize_orig_y = w->y;
                            resize_orig_w = w->width;
                            resize_orig_h = w->height;
                            resize_preview_x = w->x;
                            resize_preview_y = w->y;
                            resize_preview_w = w->width;
                            resize_preview_h = w->height;
                            dirty_resize_preview_outline(resize_preview_x, resize_preview_y,
                                                         resize_preview_w, resize_preview_h);
                            break;
                        }

                        if (my < w->y) {
                            if (!(w->flags & WIN_MAXIMIZED)) {
                                dragging_window = w;
                                drag_offset_x = mx - w->x;
                                drag_offset_y = my - w->y;
                            }
                            break;
                        }
                    }

                    if (mx >= w->x && mx < w->x + w->width &&
                        my >= w->y && my < w->y + w->height) {
                        mouse_capture_window = w;
                        mouse_capture_buttons |= 0x01;
                        gui_event_t ev = {0};
                        ev.type = GUI_EVENT_MOUSE_DOWN;
                        ev.x = mx - w->x;
                        ev.y = my - w->y;
                        ev.button = 1;
                        gui_push_event(w, &ev);
                    }
                    break;
                }
            }
        }
    } else if (prev_left && !curr_left) {
        if (resizing_window) {
            window_t* rw = resizing_window;
            int px = resize_preview_x;
            int py = resize_preview_y;
            int pw = resize_preview_w;
            int ph = resize_preview_h;
            resizing_window = NULL;
            resize_edges = 0;
            dirty_resize_preview_outline(px, py, pw, ph);
            if (rw->is_used) {
                gui_resize_window(rw, px, py, pw, ph);
            }
        } else if (dragging_window) {
            dragging_window = NULL;
        } else {
            deliver_captured_mouse_up(mx, my, 1);
        }
    }

    if (!prev_right && curr_right && !resizing_window && !dragging_window) {
        if (power_menu_open) {
            int pmx, pmy, pmw, pmh;
            get_power_menu_rect(&pmx, &pmy, &pmw, &pmh);
            power_menu_open = 0;
            gui_dirty_screen_rect(pmx, pmy, pmw, pmh);
            gui_dirty_screen_rect(0, (int)g_height - TASKBAR_HEIGHT, (int)g_width, TASKBAR_HEIGHT);
        }
        close_context_menu();

        if (my < (int)g_height - TASKBAR_HEIGHT) {
            int hit_window = 0;
            for (int i = window_count - 1; i >= 0; i--) {
                window_t* w = z_order[i];
                if (!is_window_shown(w)) continue;

                int fx, fy, fw, fh;
                get_window_frame_rect(w, &fx, &fy, &fw, &fh);
                if (mx >= fx && mx < fx + fw && my >= fy && my < fy + fh) {
                    hit_window = 1;
                    if (mx >= w->x && mx < w->x + w->width &&
                        my >= w->y && my < w->y + w->height) {
                        gui_focus_window(w);
                        mouse_capture_window = w;
                        mouse_capture_buttons |= 0x02;
                        gui_event_t ev = {0};
                        ev.type = GUI_EVENT_MOUSE_DOWN;
                        ev.x = mx - w->x;
                        ev.y = my - w->y;
                        ev.button = 2;
                        gui_push_event(w, &ev);
                    } else {
                        ctx_menu_type = CTX_MENU_WINDOW;
                        ctx_menu_x = mx;
                        ctx_menu_y = my;
                        ctx_menu_win_id = w->id;
                        int cmx, cmy, cmw, cmh;
                        get_context_menu_rect(&cmx, &cmy, &cmw, &cmh);
                        gui_dirty_screen_rect(cmx, cmy, cmw, cmh);
                    }
                    break;
                }
            }

            if (!hit_window) {
                ctx_menu_type = CTX_MENU_DESKTOP;
                ctx_menu_x = mx;
                ctx_menu_y = my;
                ctx_menu_win_id = -1;
                int cmx, cmy, cmw, cmh;
                get_context_menu_rect(&cmx, &cmy, &cmw, &cmh);
                gui_dirty_screen_rect(cmx, cmy, cmw, cmh);
            }
        }
    } else if (prev_right && !curr_right) {
        deliver_captured_mouse_up(mx, my, 2);
    }

    prev_mouse_x = mx;
    prev_mouse_y = my;
    prev_buttons = buttons;
}

static void handle_login_key(char c) {
    if (c == '\t' || c == 9) {
        login_focus_field = (login_focus_field == 0) ? 1 : 0;
        gui_full_dirty = 1;
        gui_dirty = 1;
        return;
    }
    if (c == '\n' || c == '\r') {
        if (login_focus_field == 0) {
            if (login_user_len > 0) {
                login_focus_field = 1;
                login_error_msg[0] = '\0';
                gui_full_dirty = 1;
                gui_dirty = 1;
            }
        } else {
            attempt_gui_login();
        }
        return;
    }

    char* buf = (login_focus_field == 0) ? login_user_buf : login_pass_buf;
    int* len = (login_focus_field == 0) ? &login_user_len : &login_pass_len;

    if (c == '\b' || c == 127) {
        if (*len > 0) {
            (*len)--;
            buf[*len] = '\0';
            login_error_msg[0] = '\0';
            gui_full_dirty = 1;
            gui_dirty = 1;
        }
    } else if (c >= 32 && c <= 126) {
        if (*len < 31) {
            buf[(*len)++] = c;
            buf[*len] = '\0';
            login_error_msg[0] = '\0';
            gui_full_dirty = 1;
            gui_dirty = 1;
        }
    }
}

static void handle_lock_key(char c) {
    if (c == '\n' || c == '\r') {
        attempt_gui_unlock();
    } else if (c == '\b' || c == 127) {
        if (lock_pass_len > 0) {
            lock_pass_len--;
            lock_pass_buf[lock_pass_len] = '\0';
            lock_error_msg[0] = '\0';
            gui_full_dirty = 1;
            gui_dirty = 1;
        }
    } else if (c >= 32 && c <= 126) {
        if (lock_pass_len < (int)sizeof(lock_pass_buf) - 1) {
            lock_pass_buf[lock_pass_len++] = c;
            lock_pass_buf[lock_pass_len] = '\0';
            lock_error_msg[0] = '\0';
            gui_full_dirty = 1;
            gui_dirty = 1;
        }
    }
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

        if (is_gui_login) {
            handle_login_mouse_step(pkt.x, pkt.y, pkt.buttons);
        } else if (is_gui_locked) {
            handle_locked_mouse_step(pkt.x, pkt.y, pkt.buttons);
        } else {
            handle_mouse_step(pkt.x, pkt.y, pkt.buttons, pkt.dz);
        }
    }

    while (1) {
        uint32_t eflags = irq_save_disable();
        if (lkq_head == lkq_tail) {
            irq_restore(eflags);
            break;
        }
        char c = lock_key_queue[lkq_tail];
        lkq_tail = (lkq_tail + 1) % LOCK_KEY_Q_SIZE;
        irq_restore(eflags);

        if (is_gui_login) {
            handle_login_key(c);
        } else if (is_gui_locked) {
            handle_lock_key(c);
        }
    }
}

static void draw_power_menu(void) {
    int pmx, pmy, pmw, pmh;
    get_power_menu_rect(&pmx, &pmy, &pmw, &pmh);

    bb_fill_rect(pmx, pmy, pmw, pmh, 0x000F172A);
    bb_draw_rect_outline(pmx, pmy, pmw, pmh, 0x00475569);

    const char* items[4] = { "Text Shell", "Log Out", "Reboot", "Shutdown" };
    uint32_t colors[4] = { 0x00F8FAFC, 0x00FBBF24, 0x0060A5FA, 0x00F87171 };
    for (int i = 0; i < 4; i++) {
        int iy = pmy + 4 + i * 28;
        if (i > 0) {
            bb_fill_rect(pmx + 8, iy, pmw - 16, 1, 0x001E293B);
        }
        bb_draw_string_transparent(items[i], pmx + 14, iy + 6, colors[i]);
    }
}

static void draw_context_menu(void) {
    if (ctx_menu_type == CTX_MENU_NONE) return;
    int cmx, cmy, cmw, cmh;
    get_context_menu_rect(&cmx, &cmy, &cmw, &cmh);

    bb_fill_rect(cmx, cmy, cmw, cmh, 0x000F172A);
    bb_draw_rect_outline(cmx, cmy, cmw, cmh, 0x00475569);

    if (ctx_menu_type == CTX_MENU_WINDOW) {
        const char* items[4] = { "Bring to Front", "Minimize", "Close", "New Terminal" };
        uint32_t colors[4] = { 0x00F8FAFC, 0x00FBBF24, 0x00F87171, 0x0060A5FA };
        for (int i = 0; i < 4; i++) {
            int iy = cmy + 4 + i * CTX_ITEM_H;
            if (i > 0) {
                bb_fill_rect(cmx + 8, iy, cmw - 16, 1, 0x001E293B);
            }
            bb_draw_string_transparent(items[i], cmx + 12, iy + 5, colors[i]);
        }
    } else {
        const char* items[3] = { "New Terminal", "Open Paint", "Lock Screen" };
        uint32_t colors[3] = { 0x0060A5FA, 0x005EEAD4, 0x00FBBF24 };
        for (int i = 0; i < 3; i++) {
            int iy = cmy + 4 + i * CTX_ITEM_H;
            if (i > 0) {
                bb_fill_rect(cmx + 8, iy, cmw - 16, 1, 0x001E293B);
            }
            bb_draw_string_transparent(items[i], cmx + 12, iy + 5, colors[i]);
        }
    }
}

static void draw_taskbar(window_t* focused) {
    int bar_y = (int)g_height - TASKBAR_HEIGHT;
    bb_fill_rect(0, bar_y, (int)g_width, TASKBAR_HEIGHT, 0x000F172A);
    bb_fill_rect(0, bar_y, (int)g_width, 1, 0x00334155);

    int chip_x = 8;
    if (gui_terminal_window) {
        int term_btn_w = 88;
        int is_term_act = (gui_terminal_window == focused && is_window_shown(gui_terminal_window));
        uint32_t bg = is_term_act ? 0x002563EB : 0x001E293B;
        bb_fill_rect(chip_x, bar_y + 4, term_btn_w, TASKBAR_HEIGHT - 8, bg);
        bb_draw_rect_outline(chip_x, bar_y + 4, term_btn_w, TASKBAR_HEIGHT - 8,
                             is_term_act ? 0x0060A5FA : 0x00334155);
        bb_draw_string_transparent("Terminal", chip_x + 12, bar_y + 6,
                                   is_window_shown(gui_terminal_window) ? 0x00F8FAFC : 0x0094A3B8);
        chip_x += term_btn_w + 6;
    }

    int demo_btn_w = 88;
    bb_fill_rect(chip_x, bar_y + 4, demo_btn_w, TASKBAR_HEIGHT - 8, 0x000F766E);
    bb_draw_rect_outline(chip_x, bar_y + 4, demo_btn_w, TASKBAR_HEIGHT - 8, 0x0014B8A6);
    bb_draw_string_transparent("Paint", chip_x + 24, bar_y + 6, 0x00F8FAFC);
    chip_x += demo_btn_w + 10;

    int power_x = (int)g_width - 74;
    int lock_x = power_x - 66;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].is_used) continue;
        if (!(windows[i].flags & (WIN_VISIBLE | WIN_MINIMIZED))) continue;
        if (&windows[i] == gui_terminal_window) continue;
        int chip_w = 120;
        if (chip_x + chip_w > lock_x - 10) break;

        int is_min = (windows[i].flags & WIN_MINIMIZED) != 0;
        int is_act = (!is_min && &windows[i] == focused);
        uint32_t bg = is_act ? 0x002563EB : (is_min ? 0x000F172A : 0x001E293B);
        uint32_t border = is_act ? 0x0060A5FA : (is_min ? 0x00475569 : 0x00334155);
        uint32_t fg = is_min ? 0x0094A3B8 : 0x00F8FAFC;
        bb_fill_rect(chip_x, bar_y + 4, chip_w, TASKBAR_HEIGHT - 8, bg);
        bb_draw_rect_outline(chip_x, bar_y + 4, chip_w, TASKBAR_HEIGHT - 8, border);

        char label[14];
        copy_title(label, windows[i].title, sizeof(label));
        bb_draw_string_transparent(label, chip_x + 6, bar_y + 6, fg);
        chip_x += chip_w + 6;
    }

    bb_fill_rect(lock_x, bar_y + 4, 60, TASKBAR_HEIGHT - 8, 0x00B45309);
    bb_draw_rect_outline(lock_x, bar_y + 4, 60, TASKBAR_HEIGHT - 8, 0x00F59E0B);
    bb_draw_string_transparent("Lock", lock_x + 14, bar_y + 6, 0x00FFFFFF);

    bb_fill_rect(power_x, bar_y + 4, 66, TASKBAR_HEIGHT - 8, power_menu_open ? 0x00B91C1C : 0x00991B1B);
    bb_draw_rect_outline(power_x, bar_y + 4, 66, TASKBAR_HEIGHT - 8, 0x00EF4444);
    bb_draw_string_transparent("Power", power_x + 13, bar_y + 6, 0x00FFFFFF);
}

static void draw_window_title_clipped(window_t* w) {
    int max_chars = (w->width - 74) / 8;
    if (max_chars <= 0) return;

    int x = w->x + 8;
    int y = w->y - TITLEBAR_HEIGHT + 4;
    for (int i = 0; w->title[i] && i < max_chars; i++) {
        bb_draw_char_transparent(w->title[i], x + i * 8, y, 0x00FFFFFF);
    }
}

static void draw_login_screen_overlay(void) {
    bb_fill_rect(0, 0, (int)g_width, (int)g_height, 0x000B1120);
    bb_fill_rect(0, 0, (int)g_width, 4, 0x002563EB);

    int dx, dy, dw, dh;
    get_login_dialog_rect(&dx, &dy, &dw, &dh);

    bb_fill_rect(dx, dy, dw, dh, 0x001E293B);
    bb_draw_rect_outline(dx, dy, dw, dh, 0x003B82F6);

    bb_fill_rect(dx + 1, dy + 1, dw - 2, 36, 0x000F172A);
    bb_draw_string_transparent("sOS Login", dx + 18, dy + 11, 0x00F8FAFC);

    /* Username field */
    bb_draw_string_transparent("Username:", dx + 28, dy + 50, 0x0094A3B8);
    int user_x = dx + 28, user_y = dy + 70, field_w = dw - 56, field_h = 28;
    bb_fill_rect(user_x, user_y, field_w, field_h, 0x000F172A);
    bb_draw_rect_outline(user_x, user_y, field_w, field_h,
                         (login_focus_field == 0) ? 0x003B82F6 : 0x00475569);
    bb_draw_string_transparent(login_user_buf, user_x + 8, user_y + 6, 0x00F8FAFC);
    if (login_focus_field == 0) {
        int cx = user_x + 8 + login_user_len * 8;
        if (cx < user_x + field_w - 10) {
            bb_draw_char_transparent('_', cx, user_y + 6, 0x0060A5FA);
        }
    }

    /* Password field */
    bb_draw_string_transparent("Password:", dx + 28, dy + 110, 0x0094A3B8);
    int pass_x = dx + 28, pass_y = dy + 130;
    bb_fill_rect(pass_x, pass_y, field_w, field_h, 0x000F172A);
    bb_draw_rect_outline(pass_x, pass_y, field_w, field_h,
                         (login_focus_field == 1) ? 0x003B82F6 : 0x00475569);
    char masked[34];
    int m = 0;
    int max_show = (field_w - 24) / 8;
    int count = login_pass_len;
    if (count > max_show) count = max_show;
    for (int i = 0; i < count && m < 31; i++) masked[m++] = '*';
    if (login_focus_field == 1) masked[m++] = '_';
    masked[m] = '\0';
    bb_draw_string_transparent(masked, pass_x + 8, pass_y + 6, 0x00F8FAFC);

    /* Session selector */
    bb_draw_string_transparent("Session:", dx + 28, dy + 180, 0x0094A3B8);
    int sess_gui_x = dx + 108, sess_y = dy + 174, sess_w = 148, sess_h = 28;
    int sess_txt_x = dx + 268;
    int is_desk = (login_session_mode == GUI_SESSION_DESKTOP);
    bb_fill_rect(sess_gui_x, sess_y, sess_w, sess_h, is_desk ? 0x002563EB : 0x000F172A);
    bb_draw_rect_outline(sess_gui_x, sess_y, sess_w, sess_h, is_desk ? 0x0060A5FA : 0x00475569);
    bb_draw_string_transparent("GUI Desktop", sess_gui_x + 30, sess_y + 6, 0x00F8FAFC);

    bb_fill_rect(sess_txt_x, sess_y, sess_w, sess_h, !is_desk ? 0x002563EB : 0x000F172A);
    bb_draw_rect_outline(sess_txt_x, sess_y, sess_w, sess_h, !is_desk ? 0x0060A5FA : 0x00475569);
    bb_draw_string_transparent("Text Shell", sess_txt_x + 34, sess_y + 6, 0x00F8FAFC);

    /* Error / hint message */
    if (login_error_msg[0] != '\0') {
        bb_draw_string_transparent(login_error_msg, dx + 28, dy + 218, 0x00F87171);
    } else {
        bb_draw_string_transparent("Tab: switch field   Enter: next / sign in", dx + 28, dy + 218, 0x0064748B);
    }

    /* Sign In button */
    int signin_x = dx + 28, signin_y = dy + 250, signin_w = dw - 56, signin_h = 34;
    bb_fill_rect(signin_x, signin_y, signin_w, signin_h, 0x002563EB);
    bb_draw_rect_outline(signin_x, signin_y, signin_w, signin_h, 0x0060A5FA);
    bb_draw_string_transparent("Sign In", signin_x + (signin_w - 56) / 2, signin_y + 9, 0x00FFFFFF);

    /* Bottom Display Manager bar with Reboot / Shutdown */
    int bar_y = (int)g_height - TASKBAR_HEIGHT;
    bb_fill_rect(0, bar_y, (int)g_width, TASKBAR_HEIGHT, 0x000F172A);
    bb_fill_rect(0, bar_y, (int)g_width, 1, 0x00334155);
    bb_draw_string_transparent("sOS Display Manager", 12, bar_y + 6, 0x0094A3B8);

    int shut_x = (int)g_width - 84;
    int reb_x = shut_x - 74;
    bb_fill_rect(reb_x, bar_y + 4, 68, TASKBAR_HEIGHT - 8, 0x00334155);
    bb_draw_rect_outline(reb_x, bar_y + 4, 68, TASKBAR_HEIGHT - 8, 0x0064748B);
    bb_draw_string_transparent("Reboot", reb_x + 10, bar_y + 6, 0x00F8FAFC);

    bb_fill_rect(shut_x, bar_y + 4, 76, TASKBAR_HEIGHT - 8, 0x00991B1B);
    bb_draw_rect_outline(shut_x, bar_y + 4, 76, TASKBAR_HEIGHT - 8, 0x00EF4444);
    bb_draw_string_transparent("Shutdown", shut_x + 6, bar_y + 6, 0x00FFFFFF);
}

static void draw_lock_screen_overlay(void) {
    /* Full-screen opaque lock overlay drawn above all windows and taskbar */
    bb_fill_rect(0, 0, (int)g_width, (int)g_height, 0x000B1120);
    bb_fill_rect(0, 0, (int)g_width, 4, 0x002563EB);

    int dx, dy, dw, dh;
    get_lock_dialog_rect(&dx, &dy, &dw, &dh);

    /* Dialog panel */
    bb_fill_rect(dx, dy, dw, dh, 0x001E293B);
    bb_draw_rect_outline(dx, dy, dw, dh, 0x003B82F6);

    /* Header bar */
    bb_fill_rect(dx + 1, dy + 1, dw - 2, 34, 0x000F172A);
    bb_draw_string_transparent("sOS Session Locked", dx + 18, dy + 10, 0x00F8FAFC);

    /* Active user label */
    bb_draw_string_transparent("User:", dx + 24, dy + 52, 0x0094A3B8);
    bb_draw_string_transparent(current_user, dx + 72, dy + 52, 0x00F8FAFC);

    /* Password field */
    bb_draw_string_transparent("Password:", dx + 24, dy + 80, 0x0094A3B8);
    int box_x = dx + 24;
    int box_y = dy + 100;
    int box_w = dw - 48;
    int box_h = 28;
    bb_fill_rect(box_x, box_y, box_w, box_h, 0x000F172A);
    bb_draw_rect_outline(box_x, box_y, box_w, box_h, 0x00475569);

    char masked[34];
    int m = 0;
    int max_show = (box_w - 24) / 8;
    int count = lock_pass_len;
    if (count > max_show) count = max_show;
    for (int i = 0; i < count && m < 31; i++) {
        masked[m++] = '*';
    }
    masked[m++] = '_';
    masked[m] = '\0';
    bb_draw_string_transparent(masked, box_x + 8, box_y + 6, 0x00F8FAFC);

    /* Error / hint message */
    if (lock_error_msg[0] != '\0') {
        bb_draw_string_transparent(lock_error_msg, dx + 24, dy + 138, 0x00F87171);
    } else {
        bb_draw_string_transparent("Enter password and press Enter to unlock", dx + 24, dy + 138, 0x0064748B);
    }

    /* Unlock button */
    int unlock_x = dx + 24;
    int unlock_y = dy + 168;
    int unlock_w = 148;
    int unlock_h = 32;
    bb_fill_rect(unlock_x, unlock_y, unlock_w, unlock_h, 0x002563EB);
    bb_draw_rect_outline(unlock_x, unlock_y, unlock_w, unlock_h, 0x0060A5FA);
    bb_draw_string_transparent("Unlock", unlock_x + 50, unlock_y + 8, 0x00FFFFFF);

    /* Switch User / Logout button */
    int switch_x = dx + 188;
    int switch_y = dy + 168;
    int switch_w = 208;
    int switch_h = 32;
    bb_fill_rect(switch_x, switch_y, switch_w, switch_h, 0x00334155);
    bb_draw_rect_outline(switch_x, switch_y, switch_w, switch_h, 0x0064748B);
    bb_draw_string_transparent("Switch User / Logout", switch_x + 24, switch_y + 8, 0x00F8FAFC);
}

static void save_cursor_bg(int x, int y) {
    if (!is_graphics_mode || g_bpp == 0) return;
    uint32_t bpp_bytes = g_bpp / 8;
    for (int sy = 0; sy < CURSOR_H; sy++) {
        int dy = y + sy;
        for (int sx = 0; sx < CURSOR_W; sx++) {
            int dx = x + sx;
            if (dx >= 0 && dx < (int)g_width && dy >= 0 && dy < (int)g_height) {
                uint32_t offset = ((uint32_t)dy * g_pitch) + ((uint32_t)dx * bpp_bytes);
                cursor_bg[sy * CURSOR_W + sx] = *(uint32_t*)((uint8_t*)backbuffer + offset);
            } else {
                cursor_bg[sy * CURSOR_W + sx] = 0;
            }
        }
    }
    cursor_bg_valid = 1;
}

static void restore_cursor_bg(int x, int y) {
    if (!is_graphics_mode || !cursor_bg_valid || g_bpp == 0) return;
    uint32_t bpp_bytes = g_bpp / 8;
    for (int sy = 0; sy < CURSOR_H; sy++) {
        int dy = y + sy;
        if (dy < 0 || dy >= (int)g_height) continue;
        for (int sx = 0; sx < CURSOR_W; sx++) {
            int dx = x + sx;
            if (dx < 0 || dx >= (int)g_width) continue;
            uint32_t offset = ((uint32_t)dy * g_pitch) + ((uint32_t)dx * bpp_bytes);
            *(uint32_t*)((uint8_t*)backbuffer + offset) = cursor_bg[sy * CURSOR_W + sx];
        }
    }
}

static void draw_window_complete(window_t* w, int is_focused) {
    if (w->flags & WIN_DECORATED) {
        uint32_t border_col = is_focused ? 0x003B82F6 : 0x00334155;
        uint32_t title_col = is_focused ? 0x002563EB : 0x00475569;

        /* Outer border & frame */
        bb_fill_rect(w->x - 2, w->y - TITLEBAR_HEIGHT - 2,
                     w->width + 4, w->height + TITLEBAR_HEIGHT + 4, border_col);

        /* Titlebar */
        bb_fill_rect(w->x, w->y - TITLEBAR_HEIGHT, w->width, TITLEBAR_HEIGHT, title_col);
        draw_window_title_clipped(w);

        /* Minimize [_], Maximize [□], Close [X] buttons */
        int close_x = w->x + w->width - 20;
        int max_x = w->x + w->width - 40;
        int min_x = w->x + w->width - 60;
        int btn_y = w->y - TITLEBAR_HEIGHT + 4;
        uint32_t btn_bg = is_focused ? 0x001D4ED8 : 0x00334155;
        uint32_t btn_border = is_focused ? 0x0060A5FA : 0x0064748B;

        bb_fill_rect(min_x, btn_y, 16, 16, btn_bg);
        bb_draw_rect_outline(min_x, btn_y, 16, 16, btn_border);
        bb_fill_rect(min_x + 4, btn_y + 11, 8, 2, 0x00FFFFFF);

        bb_fill_rect(max_x, btn_y, 16, 16, btn_bg);
        bb_draw_rect_outline(max_x, btn_y, 16, 16, btn_border);
        if (w->flags & WIN_MAXIMIZED) {
            bb_draw_rect_outline(max_x + 5, btn_y + 3, 8, 8, 0x00FFFFFF);
            bb_fill_rect(max_x + 3, btn_y + 5, 8, 8, btn_bg);
            bb_draw_rect_outline(max_x + 3, btn_y + 5, 8, 8, 0x00FFFFFF);
        } else {
            bb_draw_rect_outline(max_x + 3, btn_y + 3, 10, 10, 0x00FFFFFF);
            bb_fill_rect(max_x + 3, btn_y + 3, 10, 2, 0x00FFFFFF);
        }

        bb_fill_rect(close_x, btn_y, 16, 16, 0x00EF4444);
        bb_draw_char_transparent('X', close_x + 4, btn_y, 0x00FFFFFF);

        /* Subtle bottom-right corner resize indicator on the 2px frame */
        if (!(w->flags & WIN_MAXIMIZED)) {
            uint32_t grip_col = is_focused ? 0x0093C5FD : 0x0064748B;
            bb_fill_rect(w->x + w->width - 6, w->y + w->height, 8, 2, grip_col);
            bb_fill_rect(w->x + w->width, w->y + w->height - 6, 2, 8, grip_col);
        }
    }

    if (w->canvas) {
        bb_blit_surface(w->canvas, w->width, w->height, w->x, w->y);
    }
}

static void render_desktop_region(int rx, int ry, int rw, int rh, window_t* focused) {
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > (int)g_width) rw = (int)g_width - rx;
    if (ry + rh > (int)g_height) rh = (int)g_height - ry;
    if (rw <= 0 || rh <= 0) return;

    bb_set_clip(rx, ry, rw, rh);

    int bar_y = (int)g_height - TASKBAR_HEIGHT;
    gui_rect_t region = { rx, ry, rw, rh };

    if (ry < bar_y) {
        int desk_h = (ry + rh <= bar_y) ? rh : (bar_y - ry);
        int start_idx = 0;
        int need_bg = 1;

        /* Skip background & occluded lower windows if an opaque window covers the region */
        for (int i = window_count - 1; i >= 0; i--) {
            window_t* w = z_order[i];
            if (!is_window_shown(w)) continue;
            int fx, fy, fw, fh;
            get_window_frame_rect(w, &fx, &fy, &fw, &fh);
            if (fx <= rx && fy <= ry &&
                fx + fw >= rx + rw && fy + fh >= ry + desk_h) {
                start_idx = i;
                need_bg = 0;
                break;
            }
        }

        if (need_bg) {
            bb_fill_rect(rx, ry, rw, desk_h, 0x001E293B);
        }

        for (int i = start_idx; i < window_count; i++) {
            window_t* w = z_order[i];
            if (!is_window_shown(w)) continue;

            int fx, fy, fw, fh;
            get_window_frame_rect(w, &fx, &fy, &fw, &fh);
            gui_rect_t wr = { fx, fy, fw, fh };
            if (!rects_overlap_or_touch(&region, &wr)) continue;

            draw_window_complete(w, w == focused);
        }

        if (resizing_window && resizing_window->is_used) {
            int pfx = resize_preview_x - 2;
            int pfy = resize_preview_y - TITLEBAR_HEIGHT - 2;
            int pfw = resize_preview_w + 4;
            int pfh = resize_preview_h + TITLEBAR_HEIGHT + 4;
            bb_fill_rect(pfx, pfy, pfw, 2, 0x0038BDF8);
            bb_fill_rect(pfx, pfy + pfh - 2, pfw, 2, 0x0038BDF8);
            bb_fill_rect(pfx, pfy, 2, pfh, 0x0038BDF8);
            bb_fill_rect(pfx + pfw - 2, pfy, 2, pfh, 0x0038BDF8);
        }
    }

    if (ry + rh > bar_y) {
        draw_taskbar(focused);
    }

    if (power_menu_open) {
        int pmx, pmy, pmw, pmh;
        get_power_menu_rect(&pmx, &pmy, &pmw, &pmh);
        gui_rect_t pmr = { pmx, pmy, pmw, pmh };
        if (rects_overlap_or_touch(&region, &pmr)) {
            draw_power_menu();
        }
    }

    if (ctx_menu_type != CTX_MENU_NONE) {
        int cmx, cmy, cmw, cmh;
        get_context_menu_rect(&cmx, &cmy, &cmw, &cmh);
        gui_rect_t cmr = { cmx, cmy, cmw, cmh };
        if (rects_overlap_or_touch(&region, &cmr)) {
            draw_context_menu();
        }
    }

    bb_reset_clip();
}

void gui_render(void) {
    if (!is_graphics_mode || !is_gui_mode) return;

    gui_rect_t rects[MAX_DIRTY_RECTS];
    int rects_count = 0;

    uint32_t eflags = irq_save_disable();
    int full = gui_full_dirty;
    int old_cx = rendered_cursor_x;
    int old_cy = rendered_cursor_y;
    int cur_cx = mouse_x;
    int cur_cy = mouse_y;

    gui_dirty = 0;
    gui_full_dirty = 0;

    if (!full && !is_gui_login && !is_gui_locked) {
        for (int i = 0; i < screen_dirty_count; i++) {
            add_rect_to_list(rects, &rects_count, MAX_DIRTY_RECTS,
                             screen_dirty_rects[i].x, screen_dirty_rects[i].y,
                             screen_dirty_rects[i].w, screen_dirty_rects[i].h);
        }
        for (int i = 0; i < window_count; i++) {
            window_t* w = z_order[i];
            if (!w || !w->is_used || !w->dirty) continue;
            if (is_window_shown(w)) {
                add_rect_to_list(rects, &rects_count, MAX_DIRTY_RECTS,
                                 w->x + w->dirty_x, w->y + w->dirty_y,
                                 w->dirty_w, w->dirty_h);
            }
            w->dirty = 0;
        }
    } else {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            windows[i].dirty = 0;
        }
    }
    screen_dirty_count = 0;
    irq_restore(eflags);

    if (full || old_cx < 0 || old_cy < 0 || !cursor_bg_valid) {
        bb_reset_clip();
        if (is_gui_login) {
            draw_login_screen_overlay();
        } else if (is_gui_locked) {
            draw_lock_screen_overlay();
        } else {
            render_desktop_region(0, 0, (int)g_width, (int)g_height, gui_get_focused_window());
        }

        /* Save clean patch under cursor, draw cursor, and flush full frame */
        save_cursor_bg(cur_cx, cur_cy);
        bb_draw_sprite(cursor_sprite, CURSOR_W, CURSOR_H, cur_cx, cur_cy);
        swap_buffers();
    } else if (rects_count > 0) {
        restore_cursor_bg(old_cx, old_cy);

        window_t* focused = gui_get_focused_window();
        for (int i = 0; i < rects_count; i++) {
            render_desktop_region(rects[i].x, rects[i].y, rects[i].w, rects[i].h, focused);
        }

        save_cursor_bg(cur_cx, cur_cy);
        bb_draw_sprite(cursor_sprite, CURSOR_W, CURSOR_H, cur_cx, cur_cy);

        for (int i = 0; i < rects_count; i++) {
            flush_rect(rects[i].x, rects[i].y, rects[i].w, rects[i].h);
        }
        if (cur_cx != old_cx || cur_cy != old_cy) {
            flush_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
            flush_rect(cur_cx, cur_cy, CURSOR_W, CURSOR_H);
        }
    } else if (cur_cx != old_cx || cur_cy != old_cy) {
        /* Cursor-only move: restore previous 12x19 patch, save new patch, draw cursor, flush dirty rects */
        restore_cursor_bg(old_cx, old_cy);
        save_cursor_bg(cur_cx, cur_cy);
        bb_draw_sprite(cursor_sprite, CURSOR_W, CURSOR_H, cur_cx, cur_cy);
        flush_rect(old_cx, old_cy, CURSOR_W, CURSOR_H);
        flush_rect(cur_cx, cur_cy, CURSOR_W, CURSOR_H);
    }

    rendered_cursor_x = cur_cx;
    rendered_cursor_y = cur_cy;
}

static void gui_teardown(void) {
    uint32_t eflags = irq_save_disable();
    is_gui_mode = 0;
    is_gui_locked = 0;
    is_gui_login = 0;
    power_menu_open = 0;
    ctx_menu_type = CTX_MENU_NONE;
    ctx_menu_win_id = -1;
    screen_dirty_count = 0;
    bb_reset_clip();
    gui_exit_requested = 0;
    gui_terminal_window = NULL;
    dragging_window = NULL;
    resizing_window = NULL;
    resize_edges = 0;
    mouse_capture_window = NULL;
    mouse_capture_buttons = 0;
    gui_compositor_tid = -1;
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
    if (login_to_text_shell) {
        login_to_text_shell = 0;
        kprintf("Welcome, %s!\n", current_user);
        kprintf("Type 'help' for a list of available commands.\n");
        shell_input_enabled = 1;
        if (fg_task == -1) {
            print_shell_prompt();
        }
        wake_up(&console_queue);
    } else {
        kprintf("[GUI] Exited to text shell.\n");
        if (shell_input_enabled && fg_task == -1) {
            print_shell_prompt();
        }
    }
    wake_up(&gui_login_wait_queue);
}

static void gui_compositor_task(void) {
    gui_compositor_tid = current_task;
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
        if (is_gui_mode && !gui_exit_requested && !gui_dirty &&
            mq_head == mq_tail && lkq_head == lkq_tail) {
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

    uint32_t eflags = irq_save_disable();
    is_gui_login = 0;
    is_gui_locked = 0;
    power_menu_open = 0;
    is_gui_mode = 1;
    irq_restore(eflags);

    if (create_desktop_terminal_window(0) < 0) {
        is_gui_mode = 0;
        mouse_set_callback(NULL);
        kprintf("[GUI] Error: Failed to allocate Terminal window.\n");
        return;
    }

    create_task(gui_compositor_task);
}

void gui_boot_login(void) {
    if (!is_graphics_mode || is_gui_mode) return;

    gui_init();
    gui_show_login_screen();

    uint32_t eflags = irq_save_disable();
    is_gui_mode = 1;
    boot_login_done = 0;
    shell_input_enabled = 0;
    irq_restore(eflags);

    create_task(gui_compositor_task);

    while (is_gui_mode && (!boot_login_done || login_to_text_shell)) {
        sleep_on(&gui_login_wait_queue);
    }
}

#include "sos.h"

#define INIT_WIN_W 320
#define INIT_WIN_H 220
#define TOOLBAR_H 32

static int win_w = INIT_WIN_W;
static int win_h = INIT_WIN_H;

static unsigned int palette[6] = {
    0x00EF4444, /* Red */
    0x0022C55E, /* Green */
    0x003B82F6, /* Blue */
    0x00F59E0B, /* Amber */
    0x000F172A, /* Ink */
    0x00FFFFFF  /* Eraser */
};

static void fill_rect(unsigned int* buf, int x, int y, int w, int h, unsigned int col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win_w) w = win_w - x;
    if (y + h > win_h) h = win_h - y;
    if (w <= 0 || h <= 0) return;

    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            buf[py * win_w + px] = col;
        }
    }
}

static void draw_outline(unsigned int* buf, int x, int y, int w, int h, unsigned int col) {
    fill_rect(buf, x, y, w, 1, col);
    fill_rect(buf, x, y + h - 1, w, 1, col);
    fill_rect(buf, x, y, 1, h, col);
    fill_rect(buf, x + w - 1, y, 1, h, col);
}

static void draw_toolbar(unsigned int* buf, int active_idx) {
    fill_rect(buf, 0, 0, win_w, TOOLBAR_H, 0x001E293B);
    fill_rect(buf, 0, TOOLBAR_H - 1, win_w, 1, 0x00334155);

    for (int i = 0; i < 6; i++) {
        int sx = 8 + i * 32;
        int sy = 6;
        fill_rect(buf, sx, sy, 24, 20, palette[i]);
        draw_outline(buf, sx, sy, 24, 20, (i == active_idx) ? 0x00FFFFFF : 0x0064748B);
        if (i == active_idx) {
            draw_outline(buf, sx - 1, sy - 1, 26, 22, 0x0038BDF8);
        }
    }

    /* Clear button on the right */
    int cx = win_w - 68;
    fill_rect(buf, cx, 6, 60, 20, 0x00475569);
    draw_outline(buf, cx, 6, 60, 20, 0x0094A3B8);
    /* Simple "X" icon inside clear button */
    for (int k = 0; k < 10; k++) {
        fill_rect(buf, cx + 25 + k, 11 + k, 2, 2, 0x00F8FAFC);
        fill_rect(buf, cx + 34 - k, 11 + k, 2, 2, 0x00F8FAFC);
    }
}

static void clear_canvas(unsigned int* buf, int active_idx) {
    fill_rect(buf, 0, TOOLBAR_H, win_w, win_h - TOOLBAR_H, 0x00FFFFFF);
    draw_toolbar(buf, active_idx);
}

static void draw_brush_line(unsigned int* buf, int x0, int y0, int x1, int y1, unsigned int col) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = (dx < 0) ? -dx : dx;
    int ady = (dy < 0) ? -dy : dy;
    int steps = (adx > ady) ? adx : ady;
    if (steps == 0) steps = 1;

    for (int i = 0; i <= steps; i++) {
        int px = x0 + (dx * i) / steps;
        int py = y0 + (dy * i) / steps;
        if (py >= TOOLBAR_H + 2) {
            fill_rect(buf, px - 2, py - 2, 5, 5, col);
        }
    }
}

static unsigned int* resize_canvas(unsigned int* buf, unsigned int* buf_cap, int new_w, int new_h) {
    unsigned int needed = (unsigned int)new_w * (unsigned int)new_h;
    int copy_w = (win_w < new_w) ? win_w : new_w;
    int copy_h = (win_h < new_h) ? win_h : new_h;

    if (needed > *buf_cap) {
        unsigned int* new_buf = (unsigned int*)malloc(needed * sizeof(unsigned int));
        if (!new_buf) return buf;
        for (unsigned int i = 0; i < needed; i++) {
            new_buf[i] = 0x00FFFFFF;
        }
        for (int row = TOOLBAR_H; row < copy_h; row++) {
            for (int col = 0; col < copy_w; col++) {
                new_buf[row * new_w + col] = buf[row * win_w + col];
            }
        }
        *buf_cap = needed;
        return new_buf;
    }

    if (new_w <= win_w) {
        for (int row = TOOLBAR_H; row < copy_h; row++) {
            for (int col = 0; col < copy_w; col++) {
                buf[row * new_w + col] = buf[row * win_w + col];
            }
        }
    } else {
        for (int row = copy_h - 1; row >= TOOLBAR_H; row--) {
            for (int col = copy_w - 1; col >= 0; col--) {
                buf[row * new_w + col] = buf[row * win_w + col];
            }
        }
    }

    for (int row = TOOLBAR_H; row < new_h; row++) {
        int start_col = (row < copy_h) ? copy_w : 0;
        for (int col = start_col; col < new_w; col++) {
            buf[row * new_w + col] = 0x00FFFFFF;
        }
    }
    return buf;
}

void _start() {
    int win = win_create("sOS Paint (Ring 3)", win_w, win_h);
    if (win < 0) {
        print("guidemo: Failed to create GUI window (is GUI running?).\n");
        exit(1);
    }

    unsigned int buf_cap = (unsigned int)win_w * (unsigned int)win_h;
    unsigned int* buf = (unsigned int*)malloc(buf_cap * sizeof(unsigned int));
    if (!buf) {
        win_destroy(win);
        exit(1);
    }

    int active_idx = 2; /* Start with Blue */
    int is_drawing = 0;
    int last_x = 0;
    int last_y = 0;

    clear_canvas(buf, active_idx);
    win_present(win, buf);

    gui_event_t ev;
    while (win_get_event(win, &ev) > 0) {
        int dirty = 0;

        if (ev.type == GUI_EVENT_WINDOW_CLOSE) {
            break;
        } else if (ev.type == GUI_EVENT_WINDOW_RESIZE) {
            if (ev.width > 0 && ev.height > 0 && (ev.width != win_w || ev.height != win_h)) {
                unsigned int* next_buf = resize_canvas(buf, &buf_cap, ev.width, ev.height);
                if (next_buf == buf && (unsigned int)ev.width * (unsigned int)ev.height > buf_cap) {
                    continue;
                }
                buf = next_buf;
                win_w = ev.width;
                win_h = ev.height;
                draw_toolbar(buf, active_idx);
                dirty = 1;
            }
        } else if (ev.type == GUI_EVENT_KEY_PRESS) {
            if (!(ev.modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT))) {
                if (ev.key == 'q' || ev.key == 'Q' || ev.key == GUI_KEY_ESC) {
                    break;
                } else if (ev.key == 'c' || ev.key == 'C') {
                    clear_canvas(buf, active_idx);
                    dirty = 1;
                } else if (ev.key >= '1' && ev.key <= '6') {
                    active_idx = ev.key - '1';
                    draw_toolbar(buf, active_idx);
                    dirty = 1;
                }
            }
        } else if (ev.type == GUI_EVENT_MOUSE_DOWN && ev.button == 1) {
            if (ev.y < TOOLBAR_H) {
                if (ev.x >= win_w - 68 && ev.x < win_w - 8 && ev.y >= 6 && ev.y < 26) {
                    clear_canvas(buf, active_idx);
                    dirty = 1;
                } else {
                    for (int i = 0; i < 6; i++) {
                        int sx = 8 + i * 32;
                        if (ev.x >= sx && ev.x < sx + 24 && ev.y >= 6 && ev.y < 26) {
                            active_idx = i;
                            draw_toolbar(buf, active_idx);
                            dirty = 1;
                            break;
                        }
                    }
                }
            } else {
                is_drawing = 1;
                last_x = ev.x;
                last_y = ev.y;
                draw_brush_line(buf, ev.x, ev.y, ev.x, ev.y, palette[active_idx]);
                dirty = 1;
            }
        } else if (ev.type == GUI_EVENT_MOUSE_MOVE && is_drawing && (ev.button & 0x01)) {
            if (ev.y >= TOOLBAR_H) {
                draw_brush_line(buf, last_x, last_y, ev.x, ev.y, palette[active_idx]);
                last_x = ev.x;
                last_y = ev.y;
                dirty = 1;
            }
        } else if (ev.type == GUI_EVENT_MOUSE_UP && ev.button == 1) {
            is_drawing = 0;
        } else if (ev.type == GUI_EVENT_MOUSE_SCROLL && ev.dz != 0) {
            int delta = (ev.dz > 0) ? 1 : -1;
            active_idx = (active_idx + delta + 6) % 6;
            draw_toolbar(buf, active_idx);
            dirty = 1;
        }

        if (dirty) {
            win_present(win, buf);
        }
    }

    win_destroy(win);
    exit(0);
}

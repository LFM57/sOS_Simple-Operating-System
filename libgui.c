#include "libgui.h"

#define FONT8x16_IMPLEMENTATION
#include "font8x16.h"

int lg_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

void lg_put_pixel(lg_surface_t* s, int x, int y, unsigned int col) {
    if (!s || !s->pixels) return;
    if (x < 0 || y < 0 || x >= s->width || y >= s->height) return;
    s->pixels[y * s->width + x] = col;
}

void lg_fill_rect(lg_surface_t* s, int x, int y, int w, int h, unsigned int col) {
    if (!s || !s->pixels || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->width) w = s->width - x;
    if (y + h > s->height) h = s->height - y;
    if (w <= 0 || h <= 0) return;

    for (int py = y; py < y + h; py++) {
        unsigned int* row = s->pixels + py * s->width;
        for (int px = x; px < x + w; px++) {
            row[px] = col;
        }
    }
}

void lg_draw_rect(lg_surface_t* s, int x, int y, int w, int h, unsigned int col) {
    if (w <= 0 || h <= 0) return;
    lg_fill_rect(s, x, y, w, 1, col);
    lg_fill_rect(s, x, y + h - 1, w, 1, col);
    lg_fill_rect(s, x, y, 1, h, col);
    lg_fill_rect(s, x + w - 1, y, 1, h, col);
}

void lg_draw_line(lg_surface_t* s, int x0, int y0, int x1, int y1, unsigned int col) {
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 >= y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        lg_put_pixel(s, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void lg_draw_char(lg_surface_t* s, char c, int x, int y, unsigned int fg, unsigned int bg, int transparent) {
    if (!s || !s->pixels) return;
    const unsigned char* glyph = font8x16[(unsigned char)c];
    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < 0 || py >= s->height) continue;
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= s->width) continue;
            if (bits & (0x80 >> col)) {
                s->pixels[py * s->width + px] = fg;
            } else if (!transparent) {
                s->pixels[py * s->width + px] = bg;
            }
        }
    }
}

void lg_draw_text(lg_surface_t* s, const char* str, int x, int y, unsigned int fg) {
    if (!str) return;
    int cx = x;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            y += 16;
        } else {
            lg_draw_char(s, *str, cx, y, fg, 0, 1);
            cx += 8;
        }
        str++;
    }
}

void lg_draw_text_bg(lg_surface_t* s, const char* str, int x, int y, unsigned int fg, unsigned int bg) {
    if (!str) return;
    int cx = x;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            y += 16;
        } else {
            lg_draw_char(s, *str, cx, y, fg, bg, 0);
            cx += 8;
        }
        str++;
    }
}

int lg_measure_text(const char* str) {
    if (!str) return 0;
    return strlen(str) * 8;
}

void lg_panel_draw(lg_surface_t* s, const lg_panel_t* p) {
    if (!s || !p) return;
    lg_fill_rect(s, p->x, p->y, p->w, p->h, p->bg);
    lg_draw_rect(s, p->x, p->y, p->w, p->h, p->border);
    if (p->title && p->title[0] != '\0') {
        lg_draw_text(s, p->title, p->x + 8, p->y + 6, p->title_fg);
    }
}

void lg_label_draw(lg_surface_t* s, const lg_label_t* l) {
    if (!s || !l || !l->text) return;
    lg_draw_text(s, l->text, l->x, l->y, l->fg);
}

void lg_button_init(lg_button_t* btn, int x, int y, int w, int h, const char* text) {
    if (!btn) return;
    btn->x = x;
    btn->y = y;
    btn->w = w;
    btn->h = h;
    btn->text = text;
    btn->bg = 0x00334155;
    btn->hover_bg = 0x00475569;
    btn->press_bg = 0x001E293B;
    btn->fg = 0x00F8FAFC;
    btn->border = 0x0064748B;
    btn->hovered = 0;
    btn->pressed = 0;
}

void lg_button_draw(lg_surface_t* s, const lg_button_t* btn) {
    if (!s || !btn) return;
    unsigned int fill = btn->bg;
    if (btn->pressed) fill = btn->press_bg;
    else if (btn->hovered) fill = btn->hover_bg;

    lg_fill_rect(s, btn->x, btn->y, btn->w, btn->h, fill);
    lg_draw_rect(s, btn->x, btn->y, btn->w, btn->h, btn->hovered ? 0x0038BDF8 : btn->border);
    if (btn->text) {
        int tw = lg_measure_text(btn->text);
        int tx = btn->x + (btn->w - tw) / 2;
        int ty = btn->y + (btn->h - 16) / 2;
        lg_draw_text(s, btn->text, tx, ty, btn->fg);
    }
}

int lg_button_handle_event(lg_button_t* btn, const gui_event_t* ev, int* out_dirty) {
    if (!btn || !ev) return 0;
    int inside = lg_point_in_rect(ev->x, ev->y, btn->x, btn->y, btn->w, btn->h);
    int clicked = 0;

    if (ev->type == GUI_EVENT_MOUSE_MOVE) {
        if (btn->hovered != inside) {
            btn->hovered = inside;
            if (out_dirty) *out_dirty = 1;
        }
    } else if (ev->type == GUI_EVENT_MOUSE_DOWN && ev->button == 1) {
        if (inside && !btn->pressed) {
            btn->pressed = 1;
            btn->hovered = 1;
            if (out_dirty) *out_dirty = 1;
        }
    } else if (ev->type == GUI_EVENT_MOUSE_UP && ev->button == 1) {
        if (btn->pressed) {
            btn->pressed = 0;
            btn->hovered = inside;
            if (out_dirty) *out_dirty = 1;
            if (inside) clicked = 1;
        }
    }
    return clicked;
}

void lg_checkbox_init(lg_checkbox_t* cb, int x, int y, const char* label, int checked) {
    if (!cb) return;
    cb->x = x;
    cb->y = y;
    cb->size = 16;
    cb->label = label;
    cb->checked = checked ? 1 : 0;
    cb->fg = 0x000F172A;
    cb->box_bg = 0x00FFFFFF;
    cb->border = 0x0064748B;
    cb->check_col = 0x002563EB;
}

void lg_checkbox_draw(lg_surface_t* s, const lg_checkbox_t* cb) {
    if (!s || !cb) return;
    lg_fill_rect(s, cb->x, cb->y, cb->size, cb->size, cb->box_bg);
    lg_draw_rect(s, cb->x, cb->y, cb->size, cb->size, cb->border);
    if (cb->checked) {
        lg_fill_rect(s, cb->x + 3, cb->y + 3, cb->size - 6, cb->size - 6, cb->check_col);
    }
    if (cb->label) {
        lg_draw_text(s, cb->label, cb->x + cb->size + 8, cb->y, cb->fg);
    }
}

int lg_checkbox_handle_event(lg_checkbox_t* cb, const gui_event_t* ev, int* out_dirty) {
    if (!cb || !ev) return 0;
    if (ev->type == GUI_EVENT_MOUSE_DOWN && ev->button == 1) {
        int hit_w = cb->size + (cb->label ? 8 + lg_measure_text(cb->label) : 0);
        if (lg_point_in_rect(ev->x, ev->y, cb->x, cb->y, hit_w, cb->size)) {
            cb->checked = !cb->checked;
            if (out_dirty) *out_dirty = 1;
            return 1;
        }
    }
    return 0;
}

void lg_text_input_init(lg_text_input_t* ti, int x, int y, int w, int h, char* buf, int buf_cap, const char* placeholder) {
    if (!ti) return;
    ti->x = x;
    ti->y = y;
    ti->w = w;
    ti->h = h;
    ti->buf = buf;
    ti->buf_cap = buf_cap;
    ti->len = (buf && buf_cap > 0) ? strlen(buf) : 0;
    ti->cursor = ti->len;
    ti->focused = 0;
    ti->placeholder = placeholder;
    ti->bg = 0x00FFFFFF;
    ti->fg = 0x000F172A;
    ti->border = 0x0094A3B8;
    ti->focus_border = 0x0038BDF8;
}

void lg_text_input_draw(lg_surface_t* s, const lg_text_input_t* ti) {
    if (!s || !ti) return;
    lg_fill_rect(s, ti->x, ti->y, ti->w, ti->h, ti->bg);
    lg_draw_rect(s, ti->x, ti->y, ti->w, ti->h, ti->focused ? ti->focus_border : ti->border);

    int max_chars = (ti->w - 12) / 8;
    if (max_chars <= 0) return;

    int scroll = 0;
    if (ti->cursor >= max_chars) {
        scroll = ti->cursor - max_chars + 1;
    }

    int ty = ti->y + (ti->h - 16) / 2;
    if (ti->len == 0 && ti->placeholder && !ti->focused) {
        for (int i = 0; ti->placeholder[i] && i < max_chars; i++) {
            lg_draw_char(s, ti->placeholder[i], ti->x + 6 + i * 8, ty, 0x0094A3B8, 0, 1);
        }
    } else if (ti->buf) {
        for (int i = 0; i < max_chars && (scroll + i) < ti->len; i++) {
            lg_draw_char(s, ti->buf[scroll + i], ti->x + 6 + i * 8, ty, ti->fg, 0, 1);
        }
    }

    if (ti->focused) {
        int cx = ti->x + 6 + (ti->cursor - scroll) * 8;
        lg_fill_rect(s, cx, ty, 2, 16, ti->focus_border);
    }
}

int lg_text_input_handle_event(lg_text_input_t* ti, const gui_event_t* ev, int* out_dirty) {
    if (!ti || !ev || !ti->buf || ti->buf_cap <= 1) return 0;

    if (ev->type == GUI_EVENT_MOUSE_DOWN && ev->button == 1) {
        int hit = lg_point_in_rect(ev->x, ev->y, ti->x, ti->y, ti->w, ti->h);
        if (ti->focused != hit) {
            ti->focused = hit;
            if (out_dirty) *out_dirty = 1;
        }
        return 0;
    }

    if (ev->type != GUI_EVENT_KEY_PRESS || !ti->focused) return 0;

    if (ev->key == GUI_KEY_ENTER) {
        return 1;
    } else if (ev->key == GUI_KEY_BACKSPACE) {
        if (ti->cursor > 0) {
            for (int i = ti->cursor - 1; i < ti->len; i++) {
                ti->buf[i] = ti->buf[i + 1];
            }
            ti->len--;
            ti->cursor--;
            if (out_dirty) *out_dirty = 1;
        }
    } else if (ev->key == GUI_KEY_DELETE) {
        if (ti->cursor < ti->len) {
            for (int i = ti->cursor; i < ti->len; i++) {
                ti->buf[i] = ti->buf[i + 1];
            }
            ti->len--;
            if (out_dirty) *out_dirty = 1;
        }
    } else if (ev->key == GUI_KEY_LEFT) {
        if (ti->cursor > 0) {
            ti->cursor--;
            if (out_dirty) *out_dirty = 1;
        }
    } else if (ev->key == GUI_KEY_RIGHT) {
        if (ti->cursor < ti->len) {
            ti->cursor++;
            if (out_dirty) *out_dirty = 1;
        }
    } else if (ev->key == GUI_KEY_HOME) {
        if (ti->cursor != 0) {
            ti->cursor = 0;
            if (out_dirty) *out_dirty = 1;
        }
    } else if (ev->key == GUI_KEY_END) {
        if (ti->cursor != ti->len) {
            ti->cursor = ti->len;
            if (out_dirty) *out_dirty = 1;
        }
    } else if ((unsigned char)ev->key >= 32 && (unsigned char)ev->key <= 126 &&
               !(ev->modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT))) {
        if (ti->len < ti->buf_cap - 1) {
            for (int i = ti->len; i >= ti->cursor; i--) {
                ti->buf[i + 1] = ti->buf[i];
            }
            ti->buf[ti->cursor] = ev->key;
            ti->len++;
            ti->cursor++;
            if (out_dirty) *out_dirty = 1;
        }
    }
    return 0;
}

static void clamp_scrollbar_offset(lg_scrollbar_t* sb) {
    int max_off = sb->total - sb->visible;
    if (max_off < 0) max_off = 0;
    if (sb->offset < 0) sb->offset = 0;
    if (sb->offset > max_off) sb->offset = max_off;
}

static void get_scrollbar_thumb(const lg_scrollbar_t* sb, int* out_pos, int* out_size) {
    int track_len = sb->vertical ? sb->h : sb->w;
    if (track_len < 4) track_len = 4;
    if (sb->total <= 0 || sb->visible >= sb->total) {
        *out_pos = 0;
        *out_size = track_len;
        return;
    }
    int thumb_sz = (track_len * sb->visible) / sb->total;
    if (thumb_sz < 16) thumb_sz = 16;
    if (thumb_sz > track_len) thumb_sz = track_len;

    int max_off = sb->total - sb->visible;
    int max_pos = track_len - thumb_sz;
    int thumb_pos = (max_off > 0) ? (sb->offset * max_pos) / max_off : 0;
    *out_pos = thumb_pos;
    *out_size = thumb_sz;
}

void lg_scrollbar_init(lg_scrollbar_t* sb, int x, int y, int w, int h, int vertical) {
    if (!sb) return;
    sb->x = x;
    sb->y = y;
    sb->w = w;
    sb->h = h;
    sb->total = 100;
    sb->visible = 100;
    sb->offset = 0;
    sb->vertical = vertical ? 1 : 0;
    sb->dragging = 0;
    sb->drag_offset = 0;
    sb->track_col = 0x001E293B;
    sb->thumb_col = 0x0064748B;
    sb->border = 0x00334155;
}

void lg_scrollbar_draw(lg_surface_t* s, const lg_scrollbar_t* sb) {
    if (!s || !sb) return;
    lg_fill_rect(s, sb->x, sb->y, sb->w, sb->h, sb->track_col);
    lg_draw_rect(s, sb->x, sb->y, sb->w, sb->h, sb->border);

    int tpos = 0, tsz = 0;
    get_scrollbar_thumb(sb, &tpos, &tsz);
    if (sb->vertical) {
        lg_fill_rect(s, sb->x + 2, sb->y + tpos + 2, sb->w - 4, tsz - 4, sb->thumb_col);
    } else {
        lg_fill_rect(s, sb->x + tpos + 2, sb->y + 2, tsz - 4, sb->h - 4, sb->thumb_col);
    }
}

int lg_scrollbar_handle_event(lg_scrollbar_t* sb, const gui_event_t* ev, int* out_dirty) {
    if (!sb || !ev) return 0;
    int old_off = sb->offset;
    int tpos = 0, tsz = 0;
    get_scrollbar_thumb(sb, &tpos, &tsz);

    if (ev->type == GUI_EVENT_MOUSE_DOWN && ev->button == 1) {
        if (lg_point_in_rect(ev->x, ev->y, sb->x, sb->y, sb->w, sb->h)) {
            int coord = sb->vertical ? (ev->y - sb->y) : (ev->x - sb->x);
            if (coord >= tpos && coord < tpos + tsz) {
                sb->dragging = 1;
                sb->drag_offset = coord - tpos;
            } else {
                int step = (sb->visible > 1) ? sb->visible / 2 : 1;
                sb->offset += (coord < tpos) ? -step : step;
                clamp_scrollbar_offset(sb);
            }
        }
    } else if (ev->type == GUI_EVENT_MOUSE_MOVE && sb->dragging) {
        int track_len = sb->vertical ? sb->h : sb->w;
        int max_pos = track_len - tsz;
        int max_off = sb->total - sb->visible;
        if (max_pos > 0 && max_off > 0) {
            int coord = sb->vertical ? (ev->y - sb->y) : (ev->x - sb->x);
            int new_pos = coord - sb->drag_offset;
            if (new_pos < 0) new_pos = 0;
            if (new_pos > max_pos) new_pos = max_pos;
            sb->offset = (new_pos * max_off) / max_pos;
            clamp_scrollbar_offset(sb);
        }
    } else if (ev->type == GUI_EVENT_MOUSE_UP && ev->button == 1) {
        sb->dragging = 0;
    } else if (ev->type == GUI_EVENT_MOUSE_SCROLL && ev->dz != 0) {
        sb->offset += (ev->dz > 0) ? -3 : 3;
        clamp_scrollbar_offset(sb);
    }

    if (sb->offset != old_off) {
        if (out_dirty) *out_dirty = 1;
        return 1;
    }
    return 0;
}

int lg_message_box(const char* title, const char* message) {
    const char* msg = message ? message : "";
    int text_w = lg_measure_text(msg);
    int w = text_w + 48;
    if (w < 240) w = 240;
    if (w > 560) w = 560;
    int h = 112;

    int win = win_create(title ? title : "Message", w, h);
    if (win < 0) return -1;

    unsigned int* mapped = win_map(win);
    unsigned int* owned = (void*)0;
    unsigned int* pixels = mapped;
    if (!pixels) {
        owned = (unsigned int*)malloc((unsigned int)w * (unsigned int)h * sizeof(unsigned int));
        if (!owned) {
            win_destroy(win);
            return -1;
        }
        pixels = owned;
    }

    lg_surface_t surf = { pixels, w, h };
    lg_panel_t panel = { 0, 0, w, h, 0x00F8FAFC, 0x00CBD5E1, (void*)0, 0x000F172A };
    lg_label_t label = { (w - text_w) / 2, 26, msg, 0x000F172A };
    if (label.x < 16) label.x = 16;

    lg_button_t ok_btn;
    lg_button_init(&ok_btn, (w - 84) / 2, h - 42, 84, 28, "OK");

    lg_panel_draw(&surf, &panel);
    lg_label_draw(&surf, &label);
    lg_button_draw(&surf, &ok_btn);
    if (mapped) win_invalidate(win, 0, 0, w, h);
    else win_present(win, pixels);

    gui_event_t ev;
    while (win_get_event(win, &ev) > 0) {
        int dirty = 0;
        if (ev.type == GUI_EVENT_WINDOW_CLOSE) {
            break;
        }
        if (ev.type == GUI_EVENT_KEY_PRESS && (ev.key == GUI_KEY_ENTER || ev.key == GUI_KEY_ESC)) {
            break;
        }
        if (lg_button_handle_event(&ok_btn, &ev, &dirty)) {
            break;
        }
        if (dirty) {
            lg_button_draw(&surf, &ok_btn);
            if (mapped) win_invalidate(win, ok_btn.x, ok_btn.y, ok_btn.w, ok_btn.h);
            else win_present_rect(win, pixels, ok_btn.x, ok_btn.y, ok_btn.w, ok_btn.h);
        }
    }

    if (owned) free(owned);
    win_destroy(win);
    return 0;
}

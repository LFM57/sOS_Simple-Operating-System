#include "libgui.h"

#define INIT_W 640
#define INIT_H 440
#define TOOLBAR_H 38
#define STATUS_H  24
#define GUTTER_W  42
#define LINE_H    16
#define CHAR_W    8
#define INIT_BUF_CAP 4096

typedef struct {
    int win;
    int mapped;
    lg_surface_t surf;

    char* text;
    int len;
    int cap;
    int cursor;
    int modified;

    char file_path[128];
    char status_msg[96];

    int scroll_line;
    int scroll_col;
    int editor_focused;

    lg_text_input_t path_input;
    lg_button_t btn_new;
    lg_button_t btn_open;
    lg_button_t btn_save;
    lg_button_t btn_copy;
    lg_button_t btn_paste;
    lg_scrollbar_t vscroll;

    int ed_x, ed_y, ed_w, ed_h;
} edit_app_t;

static void flush_win(int win, const lg_surface_t* s, int mapped, int x, int y, int w, int h) {
    if (mapped) {
        win_invalidate(win, x, y, w, h);
    } else {
        win_present_rect(win, s->pixels, x, y, w, h);
    }
}

static char* append_str(char* dst, const char* src, int rem) {
    while (*src && rem > 1) {
        *dst++ = *src++;
        rem--;
    }
    *dst = '\0';
    return dst;
}

static char* append_uint(char* dst, unsigned int v, int rem) {
    char tmp[16];
    int n = 0;
    if (v == 0) {
        if (rem > 1) { *dst++ = '0'; *dst = '\0'; }
        return dst;
    }
    while (v > 0 && n < 15) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && rem > 1) {
        *dst++ = tmp[--n];
        rem--;
    }
    *dst = '\0';
    return dst;
}

static int ensure_cap(edit_app_t* app, int needed) {
    if (needed <= app->cap) return 1;
    int new_cap = app->cap ? app->cap : INIT_BUF_CAP;
    while (new_cap < needed) {
        if (new_cap > (1 << 20)) return 0;
        new_cap *= 2;
    }
    char* nb = (char*)realloc(app->text, (unsigned int)new_cap);
    if (!nb) return 0;
    app->text = nb;
    app->cap = new_cap;
    return 1;
}

static int count_lines(const edit_app_t* app) {
    int lines = 1;
    for (int i = 0; i < app->len; i++) {
        if (app->text[i] == '\n') lines++;
    }
    return lines;
}

static void get_cursor_lc(const edit_app_t* app, int* out_line, int* out_col) {
    int line = 0;
    int col = 0;
    for (int i = 0; i < app->cursor && i < app->len; i++) {
        if (app->text[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    *out_line = line;
    *out_col = col;
}

static int find_pos_for_lc(const edit_app_t* app, int target_line, int target_col) {
    int line = 0;
    int i = 0;
    while (i < app->len && line < target_line) {
        if (app->text[i] == '\n') line++;
        i++;
    }
    int col = 0;
    while (i < app->len && app->text[i] != '\n' && col < target_col) {
        col++;
        i++;
    }
    return i;
}

static int visible_lines(const edit_app_t* app) {
    int v = (app->ed_h - 8) / LINE_H;
    return (v < 1) ? 1 : v;
}

static int visible_cols(const edit_app_t* app) {
    int v = (app->ed_w - GUTTER_W - 22) / CHAR_W;
    return (v < 1) ? 1 : v;
}

static void ensure_cursor_visible(edit_app_t* app) {
    int ln, col;
    get_cursor_lc(app, &ln, &col);
    int vlines = visible_lines(app);
    int vcols = visible_cols(app);

    if (ln < app->scroll_line) app->scroll_line = ln;
    if (ln >= app->scroll_line + vlines) app->scroll_line = ln - vlines + 1;
    if (app->scroll_line < 0) app->scroll_line = 0;

    if (col < app->scroll_col) app->scroll_col = col;
    if (col >= app->scroll_col + vcols) app->scroll_col = col - vcols + 1;
    if (app->scroll_col < 0) app->scroll_col = 0;

    app->vscroll.total = count_lines(app);
    app->vscroll.visible = vlines;
    app->vscroll.offset = app->scroll_line;
}

static void layout_ui(edit_app_t* app) {
    int w = app->surf.width;
    int h = app->surf.height;

    int btn_w = 50;
    int right_x = w - 8;
    lg_button_init(&app->btn_paste, right_x - btn_w, 7, btn_w, 24, "Paste");
    right_x -= (btn_w + 4);
    lg_button_init(&app->btn_copy, right_x - btn_w, 7, btn_w, 24, "Copy");
    right_x -= (btn_w + 4);
    lg_button_init(&app->btn_save, right_x - btn_w, 7, btn_w, 24, "Save");
    app->btn_save.bg = 0x000F766E;
    app->btn_save.hover_bg = 0x0014B8A6;
    app->btn_save.border = 0x002DD4BF;
    right_x -= (btn_w + 4);
    lg_button_init(&app->btn_open, right_x - btn_w, 7, btn_w, 24, "Open");
    right_x -= (btn_w + 4);
    lg_button_init(&app->btn_new, right_x - 42, 7, 42, 24, "New");
    right_x -= 48;

    int path_w = right_x - 8;
    if (path_w < 80) path_w = 80;
    lg_text_input_init(&app->path_input, 8, 7, path_w, 24,
                       app->file_path, sizeof(app->file_path), "/home/user/notes.txt");
    app->path_input.bg = 0x000F172A;
    app->path_input.fg = 0x00F8FAFC;
    app->path_input.border = 0x00475569;

    app->ed_x = 0;
    app->ed_y = TOOLBAR_H;
    app->ed_w = w;
    app->ed_h = h - TOOLBAR_H - STATUS_H;
    if (app->ed_h < 32) app->ed_h = 32;

    lg_scrollbar_init(&app->vscroll, w - 14, app->ed_y, 14, app->ed_h, 1);
    app->vscroll.total = count_lines(app);
    app->vscroll.visible = visible_lines(app);
    app->vscroll.offset = app->scroll_line;
}

static void set_path_text(edit_app_t* app, const char* path) {
    int i = 0;
    while (path && path[i] && i < (int)sizeof(app->file_path) - 1) {
        app->file_path[i] = path[i];
        i++;
    }
    app->file_path[i] = '\0';
    app->path_input.len = i;
    app->path_input.cursor = i;
}

static void insert_char(edit_app_t* app, char c) {
    if (!ensure_cap(app, app->len + 2)) return;
    for (int i = app->len; i >= app->cursor; i--) {
        app->text[i + 1] = app->text[i];
    }
    app->text[app->cursor] = c;
    app->len++;
    app->cursor++;
    app->modified = 1;
    ensure_cursor_visible(app);
}

static void insert_text_block(edit_app_t* app, const char* src, int slen) {
    if (!src || slen <= 0) return;
    if (!ensure_cap(app, app->len + slen + 1)) return;
    for (int i = app->len; i >= app->cursor; i--) {
        app->text[i + slen] = app->text[i];
    }
    for (int i = 0; i < slen; i++) {
        app->text[app->cursor + i] = src[i];
    }
    app->len += slen;
    app->cursor += slen;
    app->modified = 1;
    ensure_cursor_visible(app);
}

static void delete_before_cursor(edit_app_t* app) {
    if (app->cursor <= 0 || app->len <= 0) return;
    for (int i = app->cursor - 1; i < app->len; i++) {
        app->text[i] = app->text[i + 1];
    }
    app->len--;
    app->cursor--;
    app->modified = 1;
    ensure_cursor_visible(app);
}

static void delete_at_cursor(edit_app_t* app) {
    if (app->cursor >= app->len) return;
    for (int i = app->cursor; i < app->len; i++) {
        app->text[i] = app->text[i + 1];
    }
    app->len--;
    app->modified = 1;
    ensure_cursor_visible(app);
}

static void load_file_into_editor(edit_app_t* app, const char* path) {
    if (!path || !path[0]) {
        strcpy(app->status_msg, "Enter a file path first.");
        return;
    }
    set_path_text(app, path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        app->len = 0;
        app->text[0] = '\0';
        app->cursor = 0;
        app->scroll_line = 0;
        app->scroll_col = 0;
        app->modified = 0;
        char* p = append_str(app->status_msg, "New file: ", sizeof(app->status_msg));
        append_str(p, path, sizeof(app->status_msg) - (int)(p - app->status_msg));
        ensure_cursor_visible(app);
        return;
    }

    app->len = 0;
    app->text[0] = '\0';
    char chunk[512];
    while (1) {
        int n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        if (!ensure_cap(app, app->len + n + 1)) break;
        for (int i = 0; i < n; i++) {
            if (chunk[i] != '\r') {
                app->text[app->len++] = chunk[i];
            }
        }
    }
    close(fd);
    app->text[app->len] = '\0';
    app->cursor = 0;
    app->scroll_line = 0;
    app->scroll_col = 0;
    app->modified = 0;
    ensure_cursor_visible(app);

    char* p = append_str(app->status_msg, "Loaded ", sizeof(app->status_msg));
    p = append_uint(p, (unsigned int)app->len, sizeof(app->status_msg) - (int)(p - app->status_msg));
    append_str(p, " bytes", sizeof(app->status_msg) - (int)(p - app->status_msg));
}

static void save_editor_to_file(edit_app_t* app) {
    if (!app->file_path[0]) {
        strcpy(app->status_msg, "Enter a file path in the top bar to save.");
        return;
    }
    int fd = open(app->file_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        strcpy(app->status_msg, "Save failed (permission denied or bad path).");
        return;
    }
    int written = 0;
    while (written < app->len) {
        int n = write(fd, app->text + written, (unsigned int)(app->len - written));
        if (n <= 0) break;
        written += n;
    }
    close(fd);

    if (written == app->len) {
        app->modified = 0;
        char* p = append_str(app->status_msg, "Saved ", sizeof(app->status_msg));
        p = append_uint(p, (unsigned int)written, sizeof(app->status_msg) - (int)(p - app->status_msg));
        p = append_str(p, " bytes -> ", sizeof(app->status_msg) - (int)(p - app->status_msg));
        append_str(p, app->file_path, sizeof(app->status_msg) - (int)(p - app->status_msg));
    } else {
        strcpy(app->status_msg, "Warning: partial write during save.");
    }
}

static void copy_current_line_to_clipboard(edit_app_t* app) {
    int start = app->cursor;
    while (start > 0 && app->text[start - 1] != '\n') start--;
    int end = app->cursor;
    while (end < app->len && app->text[end] != '\n') end++;

    int clen = end - start;
    if (clen <= 0 && app->len > 0) {
        start = 0;
        clen = app->len;
    }
    if (clen > 0) {
        int res = clip_set(app->text + start, clen);
        if (res >= 0) {
            char* p = append_str(app->status_msg, "Copied ", sizeof(app->status_msg));
            p = append_uint(p, (unsigned int)res, sizeof(app->status_msg) - (int)(p - app->status_msg));
            append_str(p, " chars to clipboard", sizeof(app->status_msg) - (int)(p - app->status_msg));
        }
    } else {
        strcpy(app->status_msg, "Nothing to copy.");
    }
}

static void paste_from_clipboard(edit_app_t* app) {
    char buf[1024];
    int n = clip_get(buf, sizeof(buf));
    if (n > 0) {
        insert_text_block(app, buf, n);
        char* p = append_str(app->status_msg, "Pasted ", sizeof(app->status_msg));
        p = append_uint(p, (unsigned int)n, sizeof(app->status_msg) - (int)(p - app->status_msg));
        append_str(p, " chars", sizeof(app->status_msg) - (int)(p - app->status_msg));
    } else {
        strcpy(app->status_msg, "Clipboard is empty.");
    }
}

static void draw_all(edit_app_t* app) {
    lg_surface_t* s = &app->surf;
    int w = s->width;
    int h = s->height;

    /* Toolbar */
    lg_panel_t bar = { 0, 0, w, TOOLBAR_H, 0x001E293B, 0x00334155, (void*)0, 0 };
    lg_panel_draw(s, &bar);
    lg_text_input_draw(s, &app->path_input);
    lg_button_draw(s, &app->btn_new);
    lg_button_draw(s, &app->btn_open);
    lg_button_draw(s, &app->btn_save);
    lg_button_draw(s, &app->btn_copy);
    lg_button_draw(s, &app->btn_paste);

    /* Editor background + gutter */
    lg_fill_rect(s, 0, app->ed_y, GUTTER_W, app->ed_h, 0x000F172A);
    lg_fill_rect(s, GUTTER_W - 1, app->ed_y, 1, app->ed_h, 0x00334155);
    lg_fill_rect(s, GUTTER_W, app->ed_y, w - GUTTER_W - 14, app->ed_h, 0x00090F1D);

    int cur_ln, cur_col;
    get_cursor_lc(app, &cur_ln, &cur_col);
    int vlines = visible_lines(app);
    int vcols = visible_cols(app);
    int text_x = GUTTER_W + 6;

    /* Walk buffer line by line starting from line 0 */
    int line_idx = 0;
    int pos = 0;
    while (pos < app->len && line_idx < app->scroll_line) {
        if (app->text[pos] == '\n') line_idx++;
        pos++;
    }

    for (int row = 0; row < vlines; row++) {
        int ln = app->scroll_line + row;
        int ry = app->ed_y + 4 + row * LINE_H;

        if (ln == cur_ln && app->editor_focused) {
            lg_fill_rect(s, GUTTER_W, ry, w - GUTTER_W - 14, LINE_H, 0x00111C33);
        }

        if (pos <= app->len && (ln == 0 || pos > 0)) {
            int line_exists = (ln == 0 || (pos <= app->len && (pos == 0 || app->text[pos - 1] == '\n')));
            if (line_exists && (ln < count_lines(app))) {
                char lnum[8];
                append_uint(lnum, (unsigned int)(ln + 1), sizeof(lnum));
                int lx = GUTTER_W - 6 - lg_measure_text(lnum);
                if (lx < 2) lx = 2;
                lg_draw_text(s, lnum, lx, ry, (ln == cur_ln) ? 0x0038BDF8 : 0x0064748B);

                int col = 0;
                while (pos < app->len && app->text[pos] != '\n') {
                    if (col >= app->scroll_col && col < app->scroll_col + vcols) {
                        char ch = app->text[pos];
                        if (ch == '\t') ch = ' ';
                        if ((unsigned char)ch >= 32 && (unsigned char)ch <= 126) {
                            lg_draw_char(s, ch, text_x + (col - app->scroll_col) * CHAR_W, ry, 0x00E2E8F0, 0, 1);
                        }
                    }
                    col++;
                    pos++;
                }
                if (pos < app->len && app->text[pos] == '\n') pos++;
            }
        }
    }

    /* Caret */
    if (app->editor_focused &&
        cur_ln >= app->scroll_line && cur_ln < app->scroll_line + vlines &&
        cur_col >= app->scroll_col && cur_col <= app->scroll_col + vcols) {
        int cx = text_x + (cur_col - app->scroll_col) * CHAR_W;
        int cy = app->ed_y + 4 + (cur_ln - app->scroll_line) * LINE_H;
        lg_fill_rect(s, cx, cy, 2, LINE_H, 0x0038BDF8);
    }

    lg_scrollbar_draw(s, &app->vscroll);

    /* Status bar */
    int sy = h - STATUS_H;
    lg_fill_rect(s, 0, sy, w, STATUS_H, 0x001E293B);
    lg_fill_rect(s, 0, sy, w, 1, 0x00334155);

    char right_info[40];
    char* p = append_str(right_info, "Ln ", sizeof(right_info));
    p = append_uint(p, (unsigned int)(cur_ln + 1), sizeof(right_info) - (int)(p - right_info));
    p = append_str(p, ", Col ", sizeof(right_info) - (int)(p - right_info));
    p = append_uint(p, (unsigned int)(cur_col + 1), sizeof(right_info) - (int)(p - right_info));
    if (app->modified) {
        append_str(p, " [Mod]", sizeof(right_info) - (int)(p - right_info));
    }

    int rw = lg_measure_text(right_info);
    lg_draw_text(s, right_info, w - rw - 10, sy + 4, app->modified ? 0x00FBBF24 : 0x0094A3B8);

    int max_status_chars = (w - rw - 32) / CHAR_W;
    if (max_status_chars > 0) {
        char clipped[96];
        int i = 0;
        while (app->status_msg[i] && i < max_status_chars && i < 95) {
            clipped[i] = app->status_msg[i];
            i++;
        }
        clipped[i] = '\0';
        lg_draw_text(s, clipped, 10, sy + 4, 0x00CBD5E1);
    }
}

void _start(int argc, char** argv) {
    edit_app_t* app = (edit_app_t*)malloc(sizeof(edit_app_t));
    if (!app) exit(1);
    memset(app, 0, sizeof(edit_app_t));

    app->cap = INIT_BUF_CAP;
    app->text = (char*)malloc((unsigned int)app->cap);
    if (!app->text) {
        free(app);
        exit(1);
    }
    app->text[0] = '\0';
    app->editor_focused = 1;

    app->win = win_create("sOS Editor", INIT_W, INIT_H);
    if (app->win < 0) {
        free(app->text);
        free(app);
        print("edit: Failed to create GUI window.\n");
        exit(1);
    }

    unsigned int* pixels = win_map(app->win);
    if (pixels) {
        app->mapped = 1;
    } else {
        pixels = (unsigned int*)malloc((unsigned int)INIT_W * (unsigned int)INIT_H * sizeof(unsigned int));
        if (!pixels) {
            win_destroy(app->win);
            free(app->text);
            free(app);
            exit(1);
        }
    }
    app->surf.pixels = pixels;
    app->surf.width = INIT_W;
    app->surf.height = INIT_H;

    layout_ui(app);
    if (argc >= 2 && argv && argv[1] && argv[1][0]) {
        load_file_into_editor(app, argv[1]);
    } else {
        set_path_text(app, "/home/user/notes.txt");
        strcpy(app->status_msg, "Ready | Ctrl+S Save | Ctrl+C/V Copy/Paste");
    }

    draw_all(app);
    flush_win(app->win, &app->surf, app->mapped, 0, 0, app->surf.width, app->surf.height);

    gui_event_t ev;
    while (win_get_event(app->win, &ev) > 0) {
        if (ev.type == GUI_EVENT_WINDOW_CLOSE) {
            break;
        } else if (ev.type == GUI_EVENT_WINDOW_RESIZE) {
            if (ev.width > 200 && ev.height > 140 &&
                (ev.width != app->surf.width || ev.height != app->surf.height)) {
                if (app->mapped) {
                    unsigned int* remapped = win_map(app->win);
                    if (!remapped) continue;
                    app->surf.pixels = remapped;
                } else {
                    unsigned int* next_buf = (unsigned int*)realloc(
                        app->surf.pixels, (unsigned int)ev.width * (unsigned int)ev.height * sizeof(unsigned int));
                    if (!next_buf) continue;
                    app->surf.pixels = next_buf;
                }
                app->surf.width = ev.width;
                app->surf.height = ev.height;
                layout_ui(app);
                ensure_cursor_visible(app);
                draw_all(app);
                flush_win(app->win, &app->surf, app->mapped, 0, 0, app->surf.width, app->surf.height);
            }
            continue;
        }

        int dirty = 0;

        if (lg_text_input_handle_event(&app->path_input, &ev, &dirty)) {
            load_file_into_editor(app, app->file_path);
            app->path_input.focused = 0;
            app->editor_focused = 1;
            dirty = 1;
        }

        if (lg_button_handle_event(&app->btn_new, &ev, &dirty)) {
            app->len = 0;
            app->text[0] = '\0';
            app->cursor = 0;
            app->scroll_line = 0;
            app->scroll_col = 0;
            app->modified = 0;
            app->editor_focused = 1;
            app->path_input.focused = 0;
            ensure_cursor_visible(app);
            strcpy(app->status_msg, "Cleared buffer.");
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_open, &ev, &dirty)) {
            load_file_into_editor(app, app->file_path);
            app->editor_focused = 1;
            app->path_input.focused = 0;
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_save, &ev, &dirty)) {
            save_editor_to_file(app);
            app->editor_focused = 1;
            app->path_input.focused = 0;
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_copy, &ev, &dirty)) {
            copy_current_line_to_clipboard(app);
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_paste, &ev, &dirty)) {
            paste_from_clipboard(app);
            app->editor_focused = 1;
            app->path_input.focused = 0;
            dirty = 1;
        }

        if (ev.type != GUI_EVENT_MOUSE_SCROLL ||
            lg_point_in_rect(ev.x, ev.y, app->ed_x, app->ed_y, app->ed_w, app->ed_h)) {
            if (lg_scrollbar_handle_event(&app->vscroll, &ev, &dirty)) {
                app->scroll_line = app->vscroll.offset;
                dirty = 1;
            }
        }

        if (ev.type == GUI_EVENT_MOUSE_DOWN && ev.button == 1) {
            if (lg_point_in_rect(ev.x, ev.y, app->ed_x, app->ed_y, app->ed_w - 14, app->ed_h)) {
                app->editor_focused = 1;
                app->path_input.focused = 0;
                int clicked_row = (ev.y - (app->ed_y + 4)) / LINE_H;
                if (clicked_row < 0) clicked_row = 0;
                int target_line = app->scroll_line + clicked_row;
                int total = count_lines(app);
                if (target_line >= total) target_line = total - 1;

                int rel_x = ev.x - (GUTTER_W + 6) + (CHAR_W / 2);
                int clicked_col = (rel_x > 0) ? (rel_x / CHAR_W) : 0;
                int target_col = app->scroll_col + clicked_col;
                if (target_col < 0) target_col = 0;

                app->cursor = find_pos_for_lc(app, target_line, target_col);
                ensure_cursor_visible(app);
                dirty = 1;
            } else if (app->path_input.focused) {
                app->editor_focused = 0;
                dirty = 1;
            }
        } else if (ev.type == GUI_EVENT_KEY_PRESS && !app->path_input.focused) {
            if ((ev.modifiers & GUI_MOD_CTRL) && !(ev.modifiers & GUI_MOD_ALT)) {
                if (ev.key == 19) { /* Ctrl+S */
                    save_editor_to_file(app);
                    dirty = 1;
                } else if (ev.key == 15) { /* Ctrl+O */
                    load_file_into_editor(app, app->file_path);
                    dirty = 1;
                } else if (ev.key == 3) { /* Ctrl+C */
                    copy_current_line_to_clipboard(app);
                    dirty = 1;
                } else if (ev.key == 22) { /* Ctrl+V */
                    paste_from_clipboard(app);
                    dirty = 1;
                }
            } else if (!(ev.modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT))) {
                int ln, col;
                get_cursor_lc(app, &ln, &col);
                if (ev.key == GUI_KEY_ESC) {
                    break;
                } else if (ev.key == GUI_KEY_LEFT) {
                    if (app->cursor > 0) app->cursor--;
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_RIGHT) {
                    if (app->cursor < app->len) app->cursor++;
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_UP) {
                    if (ln > 0) app->cursor = find_pos_for_lc(app, ln - 1, col);
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_DOWN) {
                    if (ln < count_lines(app) - 1) app->cursor = find_pos_for_lc(app, ln + 1, col);
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_PAGE_UP) {
                    int step = visible_lines(app);
                    int nl = (ln > step) ? (ln - step) : 0;
                    app->cursor = find_pos_for_lc(app, nl, col);
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_PAGE_DOWN) {
                    int step = visible_lines(app);
                    int max_ln = count_lines(app) - 1;
                    int nl = (ln + step < max_ln) ? (ln + step) : max_ln;
                    app->cursor = find_pos_for_lc(app, nl, col);
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_HOME) {
                    app->cursor = find_pos_for_lc(app, ln, 0);
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_END) {
                    while (app->cursor < app->len && app->text[app->cursor] != '\n') {
                        app->cursor++;
                    }
                    ensure_cursor_visible(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_BACKSPACE) {
                    delete_before_cursor(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_DELETE) {
                    delete_at_cursor(app);
                    dirty = 1;
                } else if (ev.key == GUI_KEY_ENTER) {
                    insert_char(app, '\n');
                    dirty = 1;
                } else if (ev.key == GUI_KEY_TAB) {
                    insert_text_block(app, "    ", 4);
                    dirty = 1;
                } else if ((unsigned char)ev.key >= 32 && (unsigned char)ev.key <= 126) {
                    insert_char(app, ev.key);
                    dirty = 1;
                }
            }
        }

        if (dirty) {
            draw_all(app);
            flush_win(app->win, &app->surf, app->mapped, 0, 0, app->surf.width, app->surf.height);
        }
    }

    if (!app->mapped && app->surf.pixels) {
        free(app->surf.pixels);
    }
    win_destroy(app->win);
    free(app->text);
    free(app);
    exit(0);
}

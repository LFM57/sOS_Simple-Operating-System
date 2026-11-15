#include "libgui.h"

#define WIN_W 268
#define WIN_H 344
#define BTN_COLS 4
#define BTN_ROWS 5
#define BTN_COUNT (BTN_COLS * BTN_ROWS)

typedef struct {
    int win;
    int mapped;
    lg_surface_t surf;

    int acc;
    int cur;
    char pending_op;
    int has_acc;
    int entering_new;
    int error;

    char expr_str[32];
    char disp_str[32];

    lg_button_t btns[BTN_COUNT];
} calc_app_t;

static const char* btn_labels[BTN_COUNT] = {
    "C",   "CE", "BS", "/",
    "7",   "8",  "9",  "*",
    "4",   "5",  "6",  "-",
    "1",   "2",  "3",  "+",
    "+/-", "0",  "%",  "="
};

static void flush_win(int win, const lg_surface_t* s, int mapped, int x, int y, int w, int h) {
    if (mapped) {
        win_invalidate(win, x, y, w, h);
    } else {
        win_present_rect(win, s->pixels, x, y, w, h);
    }
}

static void int_to_str(int v, char* out, int cap) {
    if (cap <= 1) return;
    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    int neg = 0;
    unsigned int uv;
    if (v < 0) {
        neg = 1;
        uv = (unsigned int)(-(v + 1)) + 1U;
    } else {
        uv = (unsigned int)v;
    }

    char tmp[16];
    int n = 0;
    while (uv > 0 && n < 15) {
        tmp[n++] = (char)('0' + (uv % 10U));
        uv /= 10U;
    }
    int k = 0;
    if (neg && k < cap - 1) out[k++] = '-';
    while (n > 0 && k < cap - 1) {
        out[k++] = tmp[--n];
    }
    out[k] = '\0';
}

static void update_display_strings(calc_app_t* app) {
    if (app->error) {
        strcpy(app->disp_str, "Error");
        app->expr_str[0] = '\0';
        return;
    }

    int_to_str(app->cur, app->disp_str, sizeof(app->disp_str));

    if (app->has_acc && app->pending_op) {
        int_to_str(app->acc, app->expr_str, sizeof(app->expr_str) - 3);
        int l = strlen(app->expr_str);
        app->expr_str[l++] = ' ';
        app->expr_str[l++] = app->pending_op;
        app->expr_str[l] = '\0';
    } else {
        app->expr_str[0] = '\0';
    }
}

static void reset_calc(calc_app_t* app) {
    app->acc = 0;
    app->cur = 0;
    app->pending_op = 0;
    app->has_acc = 0;
    app->entering_new = 1;
    app->error = 0;
    update_display_strings(app);
}

static int apply_op(int a, int b, char op, int* out_err) {
    *out_err = 0;
    if (op == '+') return a + b;
    if (op == '-') return a - b;
    if (op == '*') return a * b;
    if (op == '/') {
        if (b == 0) { *out_err = 1; return 0; }
        if (a == (-2147483647 - 1) && b == -1) { *out_err = 1; return 0; }
        return a / b;
    }
    if (op == '%') {
        if (b == 0) { *out_err = 1; return 0; }
        if (a == (-2147483647 - 1) && b == -1) { *out_err = 1; return 0; }
        return a % b;
    }
    return b;
}

static void press_digit(calc_app_t* app, int d) {
    if (app->error) reset_calc(app);
    if (app->entering_new) {
        app->cur = d;
        app->entering_new = 0;
    } else {
        if (app->cur > 200000000 || app->cur < -200000000) return;
        if (app->cur < 0) {
            app->cur = app->cur * 10 - d;
        } else {
            app->cur = app->cur * 10 + d;
        }
    }
    update_display_strings(app);
}

static void press_operator(calc_app_t* app, char op) {
    if (app->error) return;
    if (app->has_acc && !app->entering_new && app->pending_op) {
        int err = 0;
        int res = apply_op(app->acc, app->cur, app->pending_op, &err);
        if (err) {
            app->error = 1;
            update_display_strings(app);
            return;
        }
        app->acc = res;
        app->cur = res;
    } else {
        app->acc = app->cur;
        app->has_acc = 1;
    }
    app->pending_op = op;
    app->entering_new = 1;
    update_display_strings(app);
}

static void press_equals(calc_app_t* app) {
    if (app->error || !app->has_acc || !app->pending_op) return;
    int err = 0;
    int res = apply_op(app->acc, app->cur, app->pending_op, &err);
    if (err) {
        app->error = 1;
        update_display_strings(app);
        return;
    }
    app->cur = res;
    app->acc = res;
    app->has_acc = 0;
    app->pending_op = 0;
    app->entering_new = 1;
    update_display_strings(app);
}

static void handle_button_action(calc_app_t* app, int idx) {
    const char* lbl = btn_labels[idx];
    if (lbl[0] >= '0' && lbl[0] <= '9' && lbl[1] == '\0') {
        press_digit(app, lbl[0] - '0');
    } else if (strcmp(lbl, "C") == 0) {
        reset_calc(app);
    } else if (strcmp(lbl, "CE") == 0) {
        if (app->error) reset_calc(app);
        else {
            app->cur = 0;
            app->entering_new = 1;
            update_display_strings(app);
        }
    } else if (strcmp(lbl, "BS") == 0) {
        if (app->error) {
            reset_calc(app);
        } else if (!app->entering_new) {
            app->cur /= 10;
            update_display_strings(app);
        }
    } else if (strcmp(lbl, "+/-") == 0) {
        if (!app->error) {
            app->cur = -app->cur;
            update_display_strings(app);
        }
    } else if (strcmp(lbl, "=") == 0) {
        press_equals(app);
    } else if (lbl[1] == '\0' && (lbl[0] == '+' || lbl[0] == '-' || lbl[0] == '*' || lbl[0] == '/' || lbl[0] == '%')) {
        press_operator(app, lbl[0]);
    }
}

static void init_buttons(calc_app_t* app) {
    int start_x = 12;
    int start_y = 92;
    int gap = 6;
    int bw = (WIN_W - start_x * 2 - gap * (BTN_COLS - 1)) / BTN_COLS;
    int bh = 42;

    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            int i = r * BTN_COLS + c;
            int bx = start_x + c * (bw + gap);
            int by = start_y + r * (bh + gap);
            lg_button_init(&app->btns[i], bx, by, bw, bh, btn_labels[i]);

            if (c == 3 && r < 4) {
                app->btns[i].bg = 0x001E3A5F;
                app->btns[i].hover_bg = 0x00254D7E;
                app->btns[i].border = 0x003B82F6;
            } else if (i == BTN_COUNT - 1) {
                app->btns[i].bg = 0x000F766E;
                app->btns[i].hover_bg = 0x0014B8A6;
                app->btns[i].press_bg = 0x000D5C56;
                app->btns[i].border = 0x002DD4BF;
            } else if (r == 0 && c < 3) {
                app->btns[i].bg = 0x001E293B;
                app->btns[i].hover_bg = 0x00334155;
                app->btns[i].fg = 0x00FBBF24;
            }
        }
    }
}

static void draw_display(calc_app_t* app) {
    lg_surface_t* s = &app->surf;
    lg_panel_t disp = { 12, 12, WIN_W - 24, 68, 0x00020617, 0x00334155, (void*)0, 0 };
    lg_panel_draw(s, &disp);

    if (app->expr_str[0]) {
        int ew = lg_measure_text(app->expr_str);
        lg_draw_text(s, app->expr_str, WIN_W - 22 - ew, 20, 0x0094A3B8);
    }

    int dw = lg_measure_text(app->disp_str);
    int dx = WIN_W - 22 - dw;
    if (dx < 20) dx = 20;
    unsigned int col = app->error ? 0x00EF4444 : 0x00F8FAFC;
    lg_draw_text(s, app->disp_str, dx, 46, col);
}

static void draw_all(calc_app_t* app) {
    lg_fill_rect(&app->surf, 0, 0, WIN_W, WIN_H, 0x000B1120);
    draw_display(app);
    for (int i = 0; i < BTN_COUNT; i++) {
        lg_button_draw(&app->surf, &app->btns[i]);
    }
}

void _start() {
    calc_app_t* app = (calc_app_t*)malloc(sizeof(calc_app_t));
    if (!app) exit(1);
    memset(app, 0, sizeof(calc_app_t));

    app->win = win_create("sOS Calc", WIN_W, WIN_H);
    if (app->win < 0) {
        free(app);
        print("calc: Failed to create GUI window.\n");
        exit(1);
    }

    unsigned int* pixels = win_map(app->win);
    if (pixels) {
        app->mapped = 1;
    } else {
        pixels = (unsigned int*)malloc((unsigned int)WIN_W * (unsigned int)WIN_H * sizeof(unsigned int));
        if (!pixels) {
            win_destroy(app->win);
            free(app);
            exit(1);
        }
    }
    app->surf.pixels = pixels;
    app->surf.width = WIN_W;
    app->surf.height = WIN_H;

    reset_calc(app);
    init_buttons(app);
    draw_all(app);
    flush_win(app->win, &app->surf, app->mapped, 0, 0, WIN_W, WIN_H);

    gui_event_t ev;
    while (win_get_event(app->win, &ev) > 0) {
        if (ev.type == GUI_EVENT_WINDOW_CLOSE) {
            break;
        } else if (ev.type == GUI_EVENT_KEY_PRESS) {
            if (!(ev.modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT))) {
                int disp_dirty = 0;
                if (ev.key == GUI_KEY_ESC || ev.key == 'q' || ev.key == 'Q') {
                    break;
                } else if (ev.key >= '0' && ev.key <= '9') {
                    press_digit(app, ev.key - '0');
                    disp_dirty = 1;
                } else if (ev.key == '+' || ev.key == '-' || ev.key == '*' || ev.key == '/' || ev.key == '%') {
                    press_operator(app, ev.key);
                    disp_dirty = 1;
                } else if (ev.key == '=' || ev.key == GUI_KEY_ENTER) {
                    press_equals(app);
                    disp_dirty = 1;
                } else if (ev.key == GUI_KEY_BACKSPACE) {
                    if (app->error) reset_calc(app);
                    else if (!app->entering_new) {
                        app->cur /= 10;
                        update_display_strings(app);
                    }
                    disp_dirty = 1;
                } else if (ev.key == 'c' || ev.key == 'C' || ev.key == GUI_KEY_DELETE) {
                    reset_calc(app);
                    disp_dirty = 1;
                }
                if (disp_dirty) {
                    draw_display(app);
                    flush_win(app->win, &app->surf, app->mapped, 12, 12, WIN_W - 24, 68);
                }
            }
            continue;
        }

        for (int i = 0; i < BTN_COUNT; i++) {
            int bdirty = 0;
            int clicked = lg_button_handle_event(&app->btns[i], &ev, &bdirty);
            if (bdirty) {
                lg_button_draw(&app->surf, &app->btns[i]);
                flush_win(app->win, &app->surf, app->mapped,
                          app->btns[i].x, app->btns[i].y, app->btns[i].w, app->btns[i].h);
            }
            if (clicked) {
                handle_button_action(app, i);
                draw_display(app);
                flush_win(app->win, &app->surf, app->mapped, 12, 12, WIN_W - 24, 68);
            }
        }
    }

    if (!app->mapped && app->surf.pixels) {
        free(app->surf.pixels);
    }
    win_destroy(app->win);
    free(app);
    exit(0);
}

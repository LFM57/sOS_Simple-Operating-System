#include "libgui.h"

#define INIT_W 620
#define INIT_H 450
#define HIST_LEN 48
#define ROW_H 20
#define REFRESH_TICKS 50 /* 500ms at 100Hz */

typedef struct {
    int win;
    int mapped;
    lg_surface_t surf;

    sos_sysinfo_t info;
    unsigned char ram_hist[HIST_LEN];
    unsigned char heap_hist[HIST_LEN];
    int hist_count;

    int selected_idx;
    char status_msg[96];

    lg_button_t btn_refresh;
    lg_button_t btn_kill;
    lg_scrollbar_t task_sb;

    int table_x, table_y, table_w, table_h;
} sysmon_app_t;

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

static void fmt_ip(const unsigned char ip[4], char* out, int cap) {
    char* p = out;
    for (int i = 0; i < 4; i++) {
        p = append_uint(p, ip[i], cap - (int)(p - out));
        if (i < 3) p = append_str(p, ".", cap - (int)(p - out));
    }
}

static void fmt_uptime(unsigned int ticks, char* out, int cap) {
    unsigned int secs = ticks / 100U;
    unsigned int hrs = secs / 3600U;
    unsigned int mins = (secs % 3600U) / 60U;
    unsigned int s = secs % 60U;
    char* p = out;
    if (hrs < 10) p = append_str(p, "0", cap - (int)(p - out));
    p = append_uint(p, hrs, cap - (int)(p - out));
    p = append_str(p, ":", cap - (int)(p - out));
    if (mins < 10) p = append_str(p, "0", cap - (int)(p - out));
    p = append_uint(p, mins, cap - (int)(p - out));
    p = append_str(p, ":", cap - (int)(p - out));
    if (s < 10) p = append_str(p, "0", cap - (int)(p - out));
    append_uint(p, s, cap - (int)(p - out));
}

static void push_sample(sysmon_app_t* app, unsigned char ram_pct, unsigned char heap_pct) {
    if (app->hist_count < HIST_LEN) {
        app->ram_hist[app->hist_count] = ram_pct;
        app->heap_hist[app->hist_count] = heap_pct;
        app->hist_count++;
    } else {
        for (int i = 0; i < HIST_LEN - 1; i++) {
            app->ram_hist[i] = app->ram_hist[i + 1];
            app->heap_hist[i] = app->heap_hist[i + 1];
        }
        app->ram_hist[HIST_LEN - 1] = ram_pct;
        app->heap_hist[HIST_LEN - 1] = heap_pct;
    }
}

static void layout_ui(sysmon_app_t* app) {
    int w = app->surf.width;
    int h = app->surf.height;

    lg_button_init(&app->btn_refresh, w - 180, h - 32, 82, 24, "Refresh");
    lg_button_init(&app->btn_kill, w - 92, h - 32, 82, 24, "Kill Task");
    app->btn_kill.bg = 0x007F1D1D;
    app->btn_kill.hover_bg = 0x00B91C1C;
    app->btn_kill.press_bg = 0x00450A0A;
    app->btn_kill.border = 0x00EF4444;

    app->table_x = 10;
    app->table_y = 204;
    app->table_w = w - 20;
    app->table_h = h - app->table_y - 40;
    if (app->table_h < 64) app->table_h = 64;

    int prev_off = app->task_sb.offset;
    lg_scrollbar_init(&app->task_sb, app->table_x + app->table_w - 14, app->table_y + 24, 14, app->table_h - 24, 1);
    int visible = (app->table_h - 26) / ROW_H;
    if (visible < 1) visible = 1;
    app->task_sb.total = (int)app->info.task_count;
    app->task_sb.visible = visible;
    app->task_sb.offset = prev_off;
    if (app->task_sb.offset > (int)app->info.task_count - visible) {
        app->task_sb.offset = (int)app->info.task_count - visible;
    }
    if (app->task_sb.offset < 0) app->task_sb.offset = 0;
}

static void sample_sysinfo(sysmon_app_t* app) {
    unsigned int prev_selected_pid = 0;
    int had_sel = (app->selected_idx >= 0 && app->selected_idx < (int)app->info.task_count);
    if (had_sel) {
        prev_selected_pid = app->info.tasks[app->selected_idx].id;
    }

    if (sysinfo(&app->info) == 0) {
        unsigned int ram_pct = 0;
        if (app->info.total_ram_kb > 0) {
            ram_pct = (app->info.used_ram_kb * 100U) / app->info.total_ram_kb;
            if (ram_pct > 100U) ram_pct = 100U;
        }
        unsigned int heap_pct = 0;
        if (app->info.heap_total_bytes > 0) {
            heap_pct = (app->info.heap_used_bytes * 100U) / app->info.heap_total_bytes;
            if (heap_pct > 100U) heap_pct = 100U;
        }
        push_sample(app, (unsigned char)ram_pct, (unsigned char)heap_pct);

        if (had_sel) {
            int found = -1;
            for (unsigned int i = 0; i < app->info.task_count; i++) {
                if (app->info.tasks[i].id == prev_selected_pid) {
                    found = (int)i;
                    break;
                }
            }
            app->selected_idx = found;
        }
        if (app->selected_idx < 0 && app->info.task_count > 0) {
            app->selected_idx = 0;
        }
        if (app->selected_idx >= (int)app->info.task_count) {
            app->selected_idx = (int)app->info.task_count - 1;
        }
        layout_ui(app);
    }
}

static void draw_metric_card(lg_surface_t* s, int x, int y, int w, int h,
                             const char* title, const char* detail,
                             unsigned int pct, const unsigned char* hist, int hist_count,
                             unsigned int accent) {
    lg_panel_t card = { x, y, w, h, 0x000F172A, 0x00334155, title, 0x00F8FAFC };
    lg_panel_draw(s, &card);

    char pct_str[8];
    char* p = append_uint(pct_str, pct, sizeof(pct_str));
    append_str(p, "%", sizeof(pct_str) - (int)(p - pct_str));
    lg_draw_text(s, pct_str, x + w - 40, y + 6, accent);

    lg_draw_text(s, detail, x + 8, y + 26, 0x00CBD5E1);

    /* Progress bar */
    int bar_x = x + 8;
    int bar_y = y + 46;
    int bar_w = w - 16;
    int bar_h = 10;
    lg_fill_rect(s, bar_x, bar_y, bar_w, bar_h, 0x001E293B);
    lg_draw_rect(s, bar_x, bar_y, bar_w, bar_h, 0x00334155);
    int fill_w = (int)((bar_w - 2) * pct / 100U);
    if (fill_w > 0) {
        lg_fill_rect(s, bar_x + 1, bar_y + 1, fill_w, bar_h - 2, accent);
    }

    /* Live scrolling line chart */
    int gx = x + 8;
    int gy = y + 62;
    int gw = w - 16;
    int gh = h - 70;
    if (gw < 20 || gh < 20) return;

    lg_fill_rect(s, gx, gy, gw, gh, 0x00020617);
    lg_draw_rect(s, gx, gy, gw, gh, 0x001E293B);
    /* 50% grid line */
    lg_fill_rect(s, gx + 1, gy + gh / 2, gw - 2, 1, 0x0017253B);

    if (hist_count >= 2) {
        int start = HIST_LEN - hist_count;
        for (int i = 1; i < hist_count; i++) {
            int idx0 = i - 1;
            int idx1 = i;
            int x0 = gx + 2 + ((start + idx0) * (gw - 5)) / (HIST_LEN - 1);
            int x1 = gx + 2 + ((start + idx1) * (gw - 5)) / (HIST_LEN - 1);
            int y0 = gy + gh - 3 - ((int)hist[idx0] * (gh - 6)) / 100;
            int y1 = gy + gh - 3 - ((int)hist[idx1] * (gh - 6)) / 100;
            lg_draw_line(s, x0, y0, x1, y1, accent);
            lg_draw_line(s, x0, y0 + 1, x1, y1 + 1, accent);
        }
    }
}

static void draw_all(sysmon_app_t* app) {
    lg_surface_t* s = &app->surf;
    int w = s->width;
    int h = s->height;
    lg_fill_rect(s, 0, 0, w, h, 0x000B1120);

    int card_w = (w - 28) / 2;
    if (card_w < 140) card_w = 140;

    /* RAM card */
    char ram_detail[48];
    char* p = ram_detail;
    p = append_uint(p, app->info.used_ram_kb / 1024U, sizeof(ram_detail));
    p = append_str(p, " / ", sizeof(ram_detail) - (int)(p - ram_detail));
    p = append_uint(p, app->info.total_ram_kb / 1024U, sizeof(ram_detail) - (int)(p - ram_detail));
    p = append_str(p, " MB (", sizeof(ram_detail) - (int)(p - ram_detail));
    p = append_uint(p, app->info.free_ram_kb / 1024U, sizeof(ram_detail) - (int)(p - ram_detail));
    append_str(p, " MB free)", sizeof(ram_detail) - (int)(p - ram_detail));
    unsigned int ram_pct = app->info.total_ram_kb ? (app->info.used_ram_kb * 100U) / app->info.total_ram_kb : 0;
    if (ram_pct > 100U) ram_pct = 100U;
    draw_metric_card(s, 10, 10, card_w, 126, "Physical RAM", ram_detail,
                     ram_pct, app->ram_hist, app->hist_count, 0x0038BDF8);

    /* Kernel Heap card */
    char heap_detail[48];
    p = heap_detail;
    p = append_uint(p, app->info.heap_used_bytes / 1024U, sizeof(heap_detail));
    p = append_str(p, " / ", sizeof(heap_detail) - (int)(p - heap_detail));
    p = append_uint(p, app->info.heap_total_bytes / 1024U, sizeof(heap_detail) - (int)(p - heap_detail));
    p = append_str(p, " KB (", sizeof(heap_detail) - (int)(p - heap_detail));
    p = append_uint(p, app->info.heap_free_bytes / 1024U, sizeof(heap_detail) - (int)(p - heap_detail));
    append_str(p, " KB free)", sizeof(heap_detail) - (int)(p - heap_detail));
    unsigned int heap_pct = app->info.heap_total_bytes ? (app->info.heap_used_bytes * 100U) / app->info.heap_total_bytes : 0;
    if (heap_pct > 100U) heap_pct = 100U;
    draw_metric_card(s, 18 + card_w, 10, card_w, 126, "Kernel Heap (16 MB)", heap_detail,
                     heap_pct, app->heap_hist, app->hist_count, 0x0014B8A6);

    /* Network & Uptime strip */
    lg_panel_t net_p = { 10, 144, w - 20, 52, 0x000F172A, 0x00334155, (void*)0, 0 };
    lg_panel_draw(s, &net_p);

    const char* link_str = !app->info.net_installed ? "NO NIC" :
                           (app->info.net_link_up ? "LINK UP" : "LINK DOWN");
    unsigned int link_col = !app->info.net_installed ? 0x00EF4444 :
                            (app->info.net_link_up ? 0x0022C55E : 0x00F59E0B);
    lg_draw_text(s, "Net:", 20, 151, 0x0094A3B8);
    lg_draw_text(s, link_str, 58, 151, link_col);

    char ip_buf[20], gw_buf[20], dns_buf[20], up_buf[16];
    fmt_ip(app->info.ip, ip_buf, sizeof(ip_buf));
    fmt_ip(app->info.gateway, gw_buf, sizeof(gw_buf));
    fmt_ip(app->info.dns, dns_buf, sizeof(dns_buf));
    fmt_uptime(app->info.uptime_ticks, up_buf, sizeof(up_buf));

    char line1[64], line2[64];
    p = append_str(line1, "IP: ", sizeof(line1));
    p = append_str(p, ip_buf, sizeof(line1) - (int)(p - line1));
    p = append_str(p, "   Uptime: ", sizeof(line1) - (int)(p - line1));
    append_str(p, up_buf, sizeof(line1) - (int)(p - line1));
    lg_draw_text(s, line1, 140, 151, 0x00E2E8F0);

    p = append_str(line2, "GW: ", sizeof(line2));
    p = append_str(p, gw_buf, sizeof(line2) - (int)(p - line2));
    p = append_str(p, "   DNS: ", sizeof(line2) - (int)(p - line2));
    append_str(p, dns_buf, sizeof(line2) - (int)(p - line2));
    lg_draw_text(s, line2, 20, 173, 0x0094A3B8);

    /* Running Tasks Table */
    lg_panel_t tbl_p = { app->table_x, app->table_y, app->table_w, app->table_h, 0x000F172A, 0x00334155, (void*)0, 0 };
    lg_panel_draw(s, &tbl_p);

    lg_fill_rect(s, app->table_x + 1, app->table_y + 1, app->table_w - 2, 22, 0x001E293B);
    lg_draw_text(s, "PID", app->table_x + 12, app->table_y + 4, 0x0094A3B8);
    lg_draw_text(s, "Privilege", app->table_x + 68, app->table_y + 4, 0x0094A3B8);
    lg_draw_text(s, "State", app->table_x + 178, app->table_y + 4, 0x0094A3B8);
    lg_draw_text(s, "UID", app->table_x + 280, app->table_y + 4, 0x0094A3B8);
    lg_draw_text(s, "Type", app->table_x + 368, app->table_y + 4, 0x0094A3B8);

    int visible = (app->table_h - 26) / ROW_H;
    for (int r = 0; r < visible; r++) {
        int idx = app->task_sb.offset + r;
        if (idx >= (int)app->info.task_count) break;
        const sos_task_info_t* t = &app->info.tasks[idx];
        int ry = app->table_y + 24 + r * ROW_H;
        int is_sel = (idx == app->selected_idx);
        unsigned int row_bg = is_sel ? 0x001D4ED8 : ((r % 2) ? 0x00111A2E : 0x000F172A);
        lg_fill_rect(s, app->table_x + 2, ry, app->table_w - 18, ROW_H, row_bg);

        char pid_str[12];
        append_uint(pid_str, t->id, sizeof(pid_str));
        lg_draw_text(s, pid_str, app->table_x + 12, ry + 2, 0x00F8FAFC);

        const char* ring_str = t->is_user ? "Ring 3" : "Ring 0";
        unsigned int ring_col = is_sel ? 0x00FFFFFF : (t->is_user ? 0x0038BDF8 : 0x00FBBF24);
        lg_draw_text(s, ring_str, app->table_x + 68, ry + 2, ring_col);

        const char* state_str = (t->state == 1) ? "RUNNING" :
                                (t->state == 2) ? "READY" :
                                (t->state == 3) ? "ZOMBIE" :
                                (t->state == 4) ? "BLOCKED" : "INIT";
        unsigned int st_col = is_sel ? 0x00FFFFFF :
                              (t->state == 1 ? 0x0022C55E :
                               (t->state == 3 ? 0x00EF4444 : 0x00CBD5E1));
        lg_draw_text(s, state_str, app->table_x + 178, ry + 2, st_col);

        const char* uid_str = (t->uid == 0) ? "0 (root)" :
                              (t->uid == 1) ? "1 (user)" :
                              (t->uid == 2) ? "2 (guest)" : "other";
        lg_draw_text(s, uid_str, app->table_x + 280, ry + 2, 0x00CBD5E1);

        const char* mode_str = (t->id == 0) ? "Shell" :
                               (t->id == 15) ? "Idle" :
                               (t->is_background ? "Background" : "Foreground");
        lg_draw_text(s, mode_str, app->table_x + 368, ry + 2, 0x0094A3B8);
    }
    lg_scrollbar_draw(s, &app->task_sb);

    /* Footer controls & status */
    lg_draw_text(s, app->status_msg, 12, h - 28, 0x0094A3B8);
    lg_button_draw(s, &app->btn_refresh);
    lg_button_draw(s, &app->btn_kill);
}

static void handle_kill_selected(sysmon_app_t* app) {
    if (app->selected_idx < 0 || app->selected_idx >= (int)app->info.task_count) {
        strcpy(app->status_msg, "Select a task to terminate.");
        return;
    }
    unsigned int pid = app->info.tasks[app->selected_idx].id;
    if (kill((int)pid) == 0) {
        char* p = append_str(app->status_msg, "Killed PID ", sizeof(app->status_msg));
        append_uint(p, pid, sizeof(app->status_msg) - (int)(p - app->status_msg));
        sample_sysinfo(app);
    } else {
        char* p = append_str(app->status_msg, "Cannot kill PID ", sizeof(app->status_msg));
        p = append_uint(p, pid, sizeof(app->status_msg) - (int)(p - app->status_msg));
        append_str(p, " (protected/denied)", sizeof(app->status_msg) - (int)(p - app->status_msg));
    }
}

void _start() {
    sysmon_app_t* app = (sysmon_app_t*)malloc(sizeof(sysmon_app_t));
    if (!app) exit(1);
    memset(app, 0, sizeof(sysmon_app_t));
    app->selected_idx = 0;
    strcpy(app->status_msg, "Live 500ms telemetry | Select task + Kill");

    app->win = win_create("sOS System Monitor", INIT_W, INIT_H);
    if (app->win < 0) {
        free(app);
        print("sysmon: Failed to create GUI window.\n");
        exit(1);
    }

    unsigned int* pixels = win_map(app->win);
    if (pixels) {
        app->mapped = 1;
    } else {
        pixels = (unsigned int*)malloc((unsigned int)INIT_W * (unsigned int)INIT_H * sizeof(unsigned int));
        if (!pixels) {
            win_destroy(app->win);
            free(app);
            exit(1);
        }
    }
    app->surf.pixels = pixels;
    app->surf.width = INIT_W;
    app->surf.height = INIT_H;

    sample_sysinfo(app);
    draw_all(app);
    flush_win(app->win, &app->surf, app->mapped, 0, 0, app->surf.width, app->surf.height);

    unsigned int last_sample = uptime();
    int running = 1;

    while (running) {
        int dirty = 0;
        gui_event_t ev;
        int ev_rc = win_poll_event(app->win, &ev);
        if (ev_rc < 0) break;

        if (ev_rc > 0) {
            if (ev.type == GUI_EVENT_WINDOW_CLOSE) {
                running = 0;
                break;
            } else if (ev.type == GUI_EVENT_WINDOW_RESIZE) {
                if (ev.width > 180 && ev.height > 200 &&
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
                    dirty = 1;
                }
            } else {
                if (lg_button_handle_event(&app->btn_refresh, &ev, &dirty)) {
                    sample_sysinfo(app);
                    strcpy(app->status_msg, "Telemetry refreshed.");
                    dirty = 1;
                }
                if (lg_button_handle_event(&app->btn_kill, &ev, &dirty)) {
                    handle_kill_selected(app);
                    dirty = 1;
                }
                if (ev.type != GUI_EVENT_MOUSE_SCROLL ||
                    lg_point_in_rect(ev.x, ev.y, app->table_x, app->table_y, app->table_w, app->table_h)) {
                    lg_scrollbar_handle_event(&app->task_sb, &ev, &dirty);
                }

                if (ev.type == GUI_EVENT_MOUSE_DOWN && ev.button == 1) {
                    if (lg_point_in_rect(ev.x, ev.y, app->table_x + 2, app->table_y + 24,
                                         app->table_w - 18, app->table_h - 26)) {
                        int row = (ev.y - (app->table_y + 24)) / ROW_H;
                        int idx = app->task_sb.offset + row;
                        if (idx >= 0 && idx < (int)app->info.task_count) {
                            app->selected_idx = idx;
                            dirty = 1;
                        }
                    }
                } else if (ev.type == GUI_EVENT_KEY_PRESS) {
                    if (ev.key == GUI_KEY_ESC ||
                        (!(ev.modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT)) && (ev.key == 'q' || ev.key == 'Q'))) {
                        running = 0;
                        break;
                    } else if (ev.key == GUI_KEY_UP && app->selected_idx > 0) {
                        app->selected_idx--;
                        if (app->selected_idx < app->task_sb.offset) app->task_sb.offset = app->selected_idx;
                        dirty = 1;
                    } else if (ev.key == GUI_KEY_DOWN && app->selected_idx < (int)app->info.task_count - 1) {
                        app->selected_idx++;
                        if (app->selected_idx >= app->task_sb.offset + app->task_sb.visible) {
                            app->task_sb.offset = app->selected_idx - app->task_sb.visible + 1;
                        }
                        dirty = 1;
                    } else if (ev.key == 'r' || ev.key == 'R') {
                        sample_sysinfo(app);
                        dirty = 1;
                    } else if (ev.key == GUI_KEY_DELETE || ev.key == 'k' || ev.key == 'K') {
                        handle_kill_selected(app);
                        dirty = 1;
                    }
                }
            }
        } else {
            unsigned int now = uptime();
            if (now - last_sample >= REFRESH_TICKS) {
                last_sample = now;
                sample_sysinfo(app);
                dirty = 1;
            } else {
                sleep(20);
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
    free(app);
    exit(0);
}

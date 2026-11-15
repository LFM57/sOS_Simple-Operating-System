#include "libgui.h"

#define INIT_W 640
#define INIT_H 420
#define TOOLBAR_H 38
#define STATUS_H  24
#define ROW_H     20
#define MAX_ENTRIES 128
#define PREVIEW_MAX 1024

typedef struct {
    char name[64];
    unsigned int size;
    unsigned char type;
    unsigned char sec_byte;
    unsigned int inode;
} file_entry_t;

static void flush_win(int win, const lg_surface_t* s, int mapped, int x, int y, int w, int h) {
    if (mapped) {
        win_invalidate(win, x, y, w, h);
    } else {
        win_present_rect(win, s->pixels, x, y, w, h);
    }
}

static char to_lower_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int str_cmp_nocase(const char* a, const char* b) {
    while (*a && *b) {
        char ca = to_lower_ch(*a);
        char cb = to_lower_ch(*b);
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++;
        b++;
    }
    return (unsigned char)to_lower_ch(*a) - (unsigned char)to_lower_ch(*b);
}

static int ends_with_sos(const char* name) {
    int len = strlen(name);
    if (len < 4) return 0;
    const char* ext = name + len - 4;
    return to_lower_ch(ext[0]) == '.' &&
           to_lower_ch(ext[1]) == 's' &&
           to_lower_ch(ext[2]) == 'o' &&
           to_lower_ch(ext[3]) == 's';
}

static void fmt_uint(unsigned int val, char* out, int cap) {
    if (cap <= 1) return;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    char tmp[16];
    int n = 0;
    while (val > 0 && n < 15) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int k = 0;
    while (n > 0 && k < cap - 1) {
        out[k++] = tmp[--n];
    }
    out[k] = '\0';
}

static void fmt_size_short(unsigned int bytes, char* out, int cap) {
    if (bytes < 1024) {
        fmt_uint(bytes, out, cap);
        int l = strlen(out);
        if (l + 2 < cap) { out[l] = ' '; out[l + 1] = 'B'; out[l + 2] = '\0'; }
    } else if (bytes < 1024U * 1024U) {
        fmt_uint(bytes / 1024U, out, cap);
        int l = strlen(out);
        if (l + 3 < cap) { out[l] = ' '; out[l + 1] = 'K'; out[l + 2] = 'B'; out[l + 3] = '\0'; }
    } else {
        fmt_uint(bytes / (1024U * 1024U), out, cap);
        int l = strlen(out);
        if (l + 3 < cap) { out[l] = ' '; out[l + 1] = 'M'; out[l + 2] = 'B'; out[l + 3] = '\0'; }
    }
}

static void fmt_perms(unsigned char sec_byte, char* out) {
    out[0] = '[';
    unsigned char uid = sec_byte & 0x0F;
    out[1] = (uid < 10) ? (char)('0' + uid) : (char)('A' + (uid - 10));
    out[2] = '|';
    out[3] = (sec_byte & 0x10) ? 'R' : '-';
    out[4] = (sec_byte & 0x20) ? 'W' : '-';
    out[5] = (sec_byte & 0x40) ? 'r' : '-';
    out[6] = (sec_byte & 0x80) ? 'w' : '-';
    out[7] = ']';
    out[8] = '\0';
}

static void join_path(const char* dir, const char* name, char* out, int cap) {
    int i = 0;
    if (!dir || dir[0] == '\0') {
        out[i++] = '/';
    } else {
        for (int j = 0; dir[j] && i < cap - 2; j++) {
            out[i++] = dir[j];
        }
        if (i > 0 && out[i - 1] != '/') {
            out[i++] = '/';
        }
    }
    for (int j = 0; name[j] && i < cap - 1; j++) {
        out[i++] = name[j];
    }
    out[i] = '\0';
}

static void parent_path(const char* path, char* out, int cap) {
    int len = strlen(path);
    if (len <= 1) {
        strcpy(out, "/");
        return;
    }
    while (len > 1 && path[len - 1] == '/') len--;
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash <= 0) {
        strcpy(out, "/");
        return;
    }
    int copy = (last_slash < cap - 1) ? last_slash : cap - 1;
    for (int i = 0; i < copy; i++) out[i] = path[i];
    out[copy] = '\0';
}

static void draw_text_clipped(lg_surface_t* s, const char* str, int x, int y, int max_chars, unsigned int fg) {
    if (!str || max_chars <= 0) return;
    int len = strlen(str);
    if (len <= max_chars) {
        lg_draw_text(s, str, x, y, fg);
        return;
    }
    char buf[96];
    int limit = (max_chars > 95) ? 95 : max_chars;
    if (limit > 3) {
        for (int i = 0; i < limit - 2; i++) buf[i] = str[i];
        buf[limit - 2] = '.';
        buf[limit - 1] = '.';
        buf[limit] = '\0';
    } else {
        for (int i = 0; i < limit; i++) buf[i] = str[i];
        buf[limit] = '\0';
    }
    lg_draw_text(s, buf, x, y, fg);
}

typedef struct {
    int win;
    int mapped;
    lg_surface_t surf;

    char cwd[128];
    char path_edit_buf[128];
    char status_msg[96];

    file_entry_t entries[MAX_ENTRIES];
    int entry_count;
    int selected;
    unsigned int last_click_ticks;
    int last_click_idx;

    char preview_buf[PREVIEW_MAX + 1];
    int preview_len;
    int preview_is_binary;

    lg_button_t btn_up;
    lg_button_t btn_root;
    lg_button_t btn_bin;
    lg_button_t btn_home;
    lg_button_t btn_refresh;
    lg_button_t btn_action;
    lg_button_t btn_edit;
    lg_text_input_t path_input;
    lg_scrollbar_t list_sb;

    int split_x;
    int list_x, list_y, list_w, list_h;
    int pane_x, pane_y, pane_w, pane_h;
} files_app_t;

static void sort_entries(file_entry_t* arr, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            int dir_i = (arr[i].type == SOS_TYPE_DIRECTORY) ? 1 : 0;
            int dir_j = (arr[j].type == SOS_TYPE_DIRECTORY) ? 1 : 0;
            int swap = 0;
            if (dir_i != dir_j) {
                swap = (dir_j > dir_i);
            } else if (str_cmp_nocase(arr[i].name, arr[j].name) > 0) {
                swap = 1;
            }
            if (swap) {
                file_entry_t tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
            }
        }
    }
}

static void load_preview(files_app_t* app) {
    app->preview_buf[0] = '\0';
    app->preview_len = 0;
    app->preview_is_binary = 0;

    if (app->selected < 0 || app->selected >= app->entry_count) return;
    file_entry_t* e = &app->entries[app->selected];
    if (e->type == SOS_TYPE_DIRECTORY) {
        strcpy(app->preview_buf, "Directory.\nDouble-click or press Enter\nto browse contents.");
        app->preview_len = strlen(app->preview_buf);
        return;
    }
    if (e->type == SOS_TYPE_CHARDEV) {
        strcpy(app->preview_buf, "Character device node.\nVirtual kernel stream in /dev.");
        app->preview_len = strlen(app->preview_buf);
        return;
    }

    char full[192];
    join_path(app->cwd, e->name, full, sizeof(full));
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        strcpy(app->preview_buf, "Unable to read file\n(permission denied or locked).");
        app->preview_len = strlen(app->preview_buf);
        return;
    }
    int n = read(fd, app->preview_buf, PREVIEW_MAX);
    close(fd);
    if (n <= 0) {
        strcpy(app->preview_buf, "(empty file)");
        app->preview_len = strlen(app->preview_buf);
        return;
    }
    app->preview_len = n;
    app->preview_buf[n] = '\0';

    if (n >= 4 && app->preview_buf[0] == 0x7F && app->preview_buf[1] == 'E' &&
        app->preview_buf[2] == 'L' && app->preview_buf[3] == 'F') {
        app->preview_is_binary = 1;
        strcpy(app->preview_buf, "32-bit ELF Executable\nRing 3 sOS application.\nClick 'Run App' to launch.");
        app->preview_len = strlen(app->preview_buf);
        return;
    }

    int non_print = 0;
    int check_len = (n > 128) ? 128 : n;
    for (int i = 0; i < check_len; i++) {
        unsigned char c = (unsigned char)app->preview_buf[i];
        if (c != '\n' && c != '\r' && c != '\t' && (c < 32 || c > 126)) {
            non_print++;
        }
    }
    if (non_print > check_len / 6) {
        app->preview_is_binary = 1;
        strcpy(app->preview_buf, "Binary data file.\nPreview disabled for non-text content.");
        app->preview_len = strlen(app->preview_buf);
    }
}

static void layout_ui(files_app_t* app) {
    int w = app->surf.width;
    int h = app->surf.height;

    lg_button_init(&app->btn_up, 8, 7, 36, 24, "Up");
    lg_button_init(&app->btn_root, 48, 7, 28, 24, "/");
    lg_button_init(&app->btn_bin, 80, 7, 46, 24, "/bin");
    lg_button_init(&app->btn_home, 130, 7, 54, 24, "/home");
    lg_button_init(&app->btn_refresh, w - 72, 7, 64, 24, "Reload");

    int input_x = 192;
    int input_w = w - 72 - 8 - input_x;
    if (input_w < 80) input_w = 80;
    lg_text_input_init(&app->path_input, input_x, 7, input_w, 24, app->path_edit_buf, sizeof(app->path_edit_buf), "/");
    app->path_input.bg = 0x000F172A;
    app->path_input.fg = 0x00F8FAFC;
    app->path_input.border = 0x00475569;

    app->split_x = (w * 58) / 100;
    if (app->split_x < 260) app->split_x = 260;
    if (app->split_x > w - 180) app->split_x = w - 180;

    app->list_x = 8;
    app->list_y = TOOLBAR_H + 6;
    app->list_w = app->split_x - 12;
    app->list_h = h - TOOLBAR_H - STATUS_H - 12;

    app->pane_x = app->split_x + 4;
    app->pane_y = TOOLBAR_H + 6;
    app->pane_w = w - app->pane_x - 8;
    app->pane_h = app->list_h;

    int prev_off = app->list_sb.offset;
    lg_scrollbar_init(&app->list_sb, app->list_x + app->list_w - 14, app->list_y + 24, 14, app->list_h - 24, 1);
    int visible_rows = (app->list_h - 26) / ROW_H;
    if (visible_rows < 1) visible_rows = 1;
    app->list_sb.total = app->entry_count;
    app->list_sb.visible = visible_rows;
    app->list_sb.offset = prev_off;
    if (app->list_sb.offset > app->entry_count - visible_rows) {
        app->list_sb.offset = app->entry_count - visible_rows;
    }
    if (app->list_sb.offset < 0) app->list_sb.offset = 0;

    int btn_w = (app->pane_w - 28) / 2;
    if (btn_w < 70) btn_w = 70;
    int act_y = app->pane_y + 124;
    lg_button_init(&app->btn_action, app->pane_x + 10, act_y, btn_w, 26, "Open");
    app->btn_action.bg = 0x000F766E;
    app->btn_action.hover_bg = 0x0014B8A6;
    app->btn_action.border = 0x002DD4BF;
    lg_button_init(&app->btn_edit, app->pane_x + 18 + btn_w, act_y, btn_w, 26, "Edit");
}

static int load_directory(files_app_t* app, const char* path) {
    int dfd = opendir(path);
    if (dfd < 0) {
        strcpy(app->status_msg, "Cannot open directory: ");
        int l = strlen(app->status_msg);
        for (int i = 0; path[i] && l < (int)sizeof(app->status_msg) - 1; i++) {
            app->status_msg[l++] = path[i];
        }
        app->status_msg[l] = '\0';
        return 0;
    }

    strcpy(app->cwd, path);
    strcpy(app->path_edit_buf, path);
    app->path_input.len = strlen(app->path_edit_buf);
    app->path_input.cursor = app->path_input.len;

    app->entry_count = 0;
    sos_dirent_t de;
    while (app->entry_count < MAX_ENTRIES && readdir(dfd, &de) > 0) {
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
        if (de.name[0] == '\0') continue;

        file_entry_t* dst = &app->entries[app->entry_count++];
        strcpy(dst->name, de.name);
        dst->size = de.size;
        dst->type = de.type;
        dst->sec_byte = de.sec_byte;
        dst->inode = 0;

        char full[192];
        join_path(path, de.name, full, sizeof(full));
        sos_stat_t st;
        if (stat(full, &st) == 0) {
            dst->inode = st.inode;
            dst->size = st.size;
            dst->sec_byte = st.sec_byte;
        }
    }
    closedir(dfd);

    sort_entries(app->entries, app->entry_count);
    app->selected = (app->entry_count > 0) ? 0 : -1;
    app->list_sb.offset = 0;
    layout_ui(app);
    load_preview(app);

    char num[16];
    fmt_uint((unsigned int)app->entry_count, num, sizeof(num));
    strcpy(app->status_msg, path);
    int l = strlen(app->status_msg);
    if (l + 3 < (int)sizeof(app->status_msg)) {
        app->status_msg[l++] = ' '; app->status_msg[l++] = '-'; app->status_msg[l++] = ' ';
        app->status_msg[l] = '\0';
    }
    for (int i = 0; num[i] && l < (int)sizeof(app->status_msg) - 1; i++) {
        app->status_msg[l++] = num[i];
    }
    const char* suffix = (app->entry_count == 1) ? " item" : " items";
    for (int i = 0; suffix[i] && l < (int)sizeof(app->status_msg) - 1; i++) {
        app->status_msg[l++] = suffix[i];
    }
    app->status_msg[l] = '\0';
    return 1;
}

static void draw_all(files_app_t* app) {
    lg_surface_t* s = &app->surf;
    lg_fill_rect(s, 0, 0, s->width, s->height, 0x000B1120);

    /* Toolbar */
    lg_panel_t bar = { 0, 0, s->width, TOOLBAR_H, 0x001E293B, 0x00334155, (void*)0, 0 };
    lg_panel_draw(s, &bar);
    lg_button_draw(s, &app->btn_up);
    lg_button_draw(s, &app->btn_root);
    lg_button_draw(s, &app->btn_bin);
    lg_button_draw(s, &app->btn_home);
    lg_button_draw(s, &app->btn_refresh);
    lg_text_input_draw(s, &app->path_input);

    /* Left Pane: Directory Table */
    lg_panel_t left_p = { app->list_x, app->list_y, app->list_w, app->list_h, 0x000F172A, 0x00334155, (void*)0, 0 };
    lg_panel_draw(s, &left_p);

    lg_fill_rect(s, app->list_x + 1, app->list_y + 1, app->list_w - 2, 22, 0x001E293B);
    lg_draw_text(s, "Name", app->list_x + 8, app->list_y + 4, 0x0094A3B8);
    if (app->list_w >= 260) {
        lg_draw_text(s, "Size", app->list_x + app->list_w - 154, app->list_y + 4, 0x0094A3B8);
        lg_draw_text(s, "Perms", app->list_x + app->list_w - 86, app->list_y + 4, 0x0094A3B8);
    }

    int visible_rows = (app->list_h - 26) / ROW_H;
    int name_max_chars = (app->list_w - 180) / 8;
    if (name_max_chars < 10) name_max_chars = 10;

    for (int r = 0; r < visible_rows; r++) {
        int idx = app->list_sb.offset + r;
        if (idx >= app->entry_count) break;
        file_entry_t* e = &app->entries[idx];
        int ry = app->list_y + 24 + r * ROW_H;
        int is_sel = (idx == app->selected);
        unsigned int row_bg = is_sel ? 0x001D4ED8 : ((r % 2) ? 0x00111A2E : 0x000F172A);
        lg_fill_rect(s, app->list_x + 2, ry, app->list_w - 18, ROW_H, row_bg);

        const char* tag = (e->type == SOS_TYPE_DIRECTORY) ? "[D]" :
                          (e->type == SOS_TYPE_CHARDEV) ? "[C]" :
                          (ends_with_sos(e->name) ? "[*]" : "   ");
        unsigned int tag_col = (e->type == SOS_TYPE_DIRECTORY) ? 0x00FBBF24 :
                               (e->type == SOS_TYPE_CHARDEV) ? 0x00A78BFA :
                               (ends_with_sos(e->name) ? 0x002DD4BF : 0x0064748B);
        unsigned int name_col = is_sel ? 0x00FFFFFF :
                                (e->type == SOS_TYPE_DIRECTORY ? 0x00FDE68A : 0x00E2E8F0);

        lg_draw_text(s, tag, app->list_x + 6, ry + 2, tag_col);
        draw_text_clipped(s, e->name, app->list_x + 34, ry + 2, name_max_chars, name_col);

        if (app->list_w >= 260) {
            char sz[16];
            if (e->type == SOS_TYPE_DIRECTORY) {
                strcpy(sz, "<DIR>");
            } else {
                fmt_size_short(e->size, sz, sizeof(sz));
            }
            lg_draw_text(s, sz, app->list_x + app->list_w - 154, ry + 2, is_sel ? 0x00E2E8F0 : 0x0094A3B8);

            char perms[12];
            fmt_perms(e->sec_byte, perms);
            lg_draw_text(s, perms, app->list_x + app->list_w - 86, ry + 2, is_sel ? 0x00E2E8F0 : 0x0064748B);
        }
    }

    if (app->entry_count == 0) {
        lg_draw_text(s, "(empty directory)", app->list_x + 16, app->list_y + 38, 0x0064748B);
    }
    lg_scrollbar_draw(s, &app->list_sb);

    /* Right Pane: Inspector + Preview */
    lg_panel_t right_p = { app->pane_x, app->pane_y, app->pane_w, app->pane_h, 0x000F172A, 0x00334155, (void*)0, 0 };
    lg_panel_draw(s, &right_p);
    lg_fill_rect(s, app->pane_x + 1, app->pane_y + 1, app->pane_w - 2, 22, 0x001E293B);
    lg_draw_text(s, "Inspector", app->pane_x + 8, app->pane_y + 4, 0x0094A3B8);

    if (app->selected >= 0 && app->selected < app->entry_count) {
        file_entry_t* e = &app->entries[app->selected];
        int ix = app->pane_x + 10;
        int iy = app->pane_y + 30;
        int max_c = (app->pane_w - 20) / 8;

        draw_text_clipped(s, e->name, ix, iy, max_c, 0x00F8FAFC);

        const char* tname = (e->type == SOS_TYPE_DIRECTORY) ? "Type : Directory" :
                            (e->type == SOS_TYPE_CHARDEV) ? "Type : Char Device" :
                            (ends_with_sos(e->name) ? "Type : Ring 3 App (.sos)" : "Type : Regular File");
        draw_text_clipped(s, tname, ix, iy + 20, max_c, 0x0038BDF8);

        char line[48];
        char num[16];
        fmt_uint(e->size, num, sizeof(num));
        strcpy(line, "Size : ");
        int l = strlen(line);
        for (int i = 0; num[i] && l < 40; i++) line[l++] = num[i];
        const char* bstr = " bytes";
        for (int i = 0; bstr[i] && l < 47; i++) line[l++] = bstr[i];
        line[l] = '\0';
        draw_text_clipped(s, line, ix, iy + 38, max_c, 0x00CBD5E1);

        char perms[12];
        fmt_perms(e->sec_byte, perms);
        strcpy(line, "Perm : ");
        for (int i = 0; perms[i]; i++) line[7 + i] = perms[i];
        line[7 + strlen(perms)] = '\0';
        draw_text_clipped(s, line, ix, iy + 56, max_c, 0x00CBD5E1);

        fmt_uint(e->inode, num, sizeof(num));
        strcpy(line, "Inode: ");
        for (int i = 0; num[i]; i++) line[7 + i] = num[i];
        line[7 + strlen(num)] = '\0';
        draw_text_clipped(s, line, ix, iy + 74, max_c, 0x0094A3B8);

        if (e->type == SOS_TYPE_DIRECTORY) {
            app->btn_action.text = "Open Dir";
            lg_button_draw(s, &app->btn_action);
        } else if (ends_with_sos(e->name)) {
            app->btn_action.text = "Run App";
            lg_button_draw(s, &app->btn_action);
        } else if (e->type == SOS_TYPE_FILE) {
            app->btn_action.text = "Open";
            lg_button_draw(s, &app->btn_action);
            lg_button_draw(s, &app->btn_edit);
        }

        /* Preview box */
        int pbox_y = app->pane_y + 158;
        int pbox_h = app->pane_h - 166;
        if (pbox_h > 32) {
            lg_fill_rect(s, app->pane_x + 8, pbox_y, app->pane_w - 16, pbox_h, 0x00020617);
            lg_draw_rect(s, app->pane_x + 8, pbox_y, app->pane_w - 16, pbox_h, 0x001E293B);

            int max_cols = (app->pane_w - 28) / 8;
            int max_lines = (pbox_h - 10) / 16;
            int row = 0, col = 0;
            for (int i = 0; i < app->preview_len && row < max_lines; i++) {
                char c = app->preview_buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    row++;
                    col = 0;
                    continue;
                }
                if (c == '\t') c = ' ';
                if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '.';
                if (col < max_cols) {
                    lg_draw_char(s, c, app->pane_x + 14 + col * 8, pbox_y + 6 + row * 16,
                                 app->preview_is_binary ? 0x0094A3B8 : 0x00CBD5E1, 0, 1);
                    col++;
                }
            }
        }
    } else {
        lg_draw_text(s, "No item selected.", app->pane_x + 12, app->pane_y + 38, 0x0064748B);
    }

    /* Status Bar */
    int sy = s->height - STATUS_H;
    lg_fill_rect(s, 0, sy, s->width, STATUS_H, 0x001E293B);
    lg_fill_rect(s, 0, sy, s->width, 1, 0x00334155);
    draw_text_clipped(s, app->status_msg, 10, sy + 4, (s->width - 20) / 8, 0x0094A3B8);
}

static void activate_selected(files_app_t* app) {
    if (app->selected < 0 || app->selected >= app->entry_count) return;
    file_entry_t* e = &app->entries[app->selected];
    char full[192];
    join_path(app->cwd, e->name, full, sizeof(full));

    if (e->type == SOS_TYPE_DIRECTORY) {
        load_directory(app, full);
    } else if (ends_with_sos(e->name)) {
        char* argv[2] = { full, (void*)0 };
        int pid = spawn(full, argv);
        if (pid >= 0) {
            strcpy(app->status_msg, "Launched ");
            int l = strlen(app->status_msg);
            for (int i = 0; full[i] && l < 80; i++) app->status_msg[l++] = full[i];
            app->status_msg[l] = '\0';
        } else {
            strcpy(app->status_msg, "Failed to launch executable.");
        }
    } else if (e->type == SOS_TYPE_FILE) {
        char* argv[3] = { "/bin/edit.sos", full, (void*)0 };
        int pid = spawn("/bin/edit.sos", argv);
        if (pid >= 0) {
            strcpy(app->status_msg, "Opened in editor: ");
            int l = strlen(app->status_msg);
            for (int i = 0; full[i] && l < 90; i++) app->status_msg[l++] = full[i];
            app->status_msg[l] = '\0';
        } else {
            strcpy(app->status_msg, "Editor (/bin/edit.sos) unavailable.");
        }
    } else {
        strcpy(app->status_msg, "Character device cannot be executed.");
    }
}

static void ensure_selection_visible(files_app_t* app) {
    if (app->selected < 0) return;
    if (app->selected < app->list_sb.offset) {
        app->list_sb.offset = app->selected;
    } else if (app->selected >= app->list_sb.offset + app->list_sb.visible) {
        app->list_sb.offset = app->selected - app->list_sb.visible + 1;
    }
    if (app->list_sb.offset < 0) app->list_sb.offset = 0;
}

void _start(int argc, char** argv) {
    files_app_t* app = (files_app_t*)malloc(sizeof(files_app_t));
    if (!app) exit(1);
    memset(app, 0, sizeof(files_app_t));
    app->selected = -1;
    app->last_click_idx = -1;

    app->win = win_create("sOS Files", INIT_W, INIT_H);
    if (app->win < 0) {
        free(app);
        print("files: Failed to create GUI window.\n");
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

    const char* start_dir = (argc >= 2 && argv && argv[1] && argv[1][0]) ? argv[1] : "/";
    if (!load_directory(app, start_dir)) {
        load_directory(app, "/");
    }
    draw_all(app);
    flush_win(app->win, &app->surf, app->mapped, 0, 0, app->surf.width, app->surf.height);

    gui_event_t ev;
    while (win_get_event(app->win, &ev) > 0) {
        if (ev.type == GUI_EVENT_WINDOW_CLOSE) {
            break;
        } else if (ev.type == GUI_EVENT_WINDOW_RESIZE) {
            if (ev.width > 120 && ev.height > 120 && (ev.width != app->surf.width || ev.height != app->surf.height)) {
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
                draw_all(app);
                flush_win(app->win, &app->surf, app->mapped, 0, 0, app->surf.width, app->surf.height);
            }
            continue;
        }

        int dirty = 0;

        if (lg_text_input_handle_event(&app->path_input, &ev, &dirty)) {
            load_directory(app, app->path_edit_buf);
            dirty = 1;
        }

        if (lg_button_handle_event(&app->btn_up, &ev, &dirty)) {
            char up[128];
            parent_path(app->cwd, up, sizeof(up));
            load_directory(app, up);
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_root, &ev, &dirty)) {
            load_directory(app, "/");
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_bin, &ev, &dirty)) {
            load_directory(app, "/bin");
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_home, &ev, &dirty)) {
            load_directory(app, "/home");
            dirty = 1;
        } else if (lg_button_handle_event(&app->btn_refresh, &ev, &dirty)) {
            load_directory(app, app->cwd);
            dirty = 1;
        }

        if (app->selected >= 0 && app->selected < app->entry_count) {
            file_entry_t* sel = &app->entries[app->selected];
            if (lg_button_handle_event(&app->btn_action, &ev, &dirty)) {
                activate_selected(app);
                dirty = 1;
            } else if (sel->type == SOS_TYPE_FILE && !ends_with_sos(sel->name) &&
                       lg_button_handle_event(&app->btn_edit, &ev, &dirty)) {
                activate_selected(app);
                dirty = 1;
            }
        }

        if (ev.type != GUI_EVENT_MOUSE_SCROLL ||
            lg_point_in_rect(ev.x, ev.y, app->list_x, app->list_y, app->list_w, app->list_h)) {
            if (lg_scrollbar_handle_event(&app->list_sb, &ev, &dirty)) {
                dirty = 1;
            }
        }

        if (ev.type == GUI_EVENT_MOUSE_DOWN && ev.button == 1) {
            if (lg_point_in_rect(ev.x, ev.y, app->list_x + 2, app->list_y + 24, app->list_w - 18, app->list_h - 26)) {
                int row = (ev.y - (app->list_y + 24)) / ROW_H;
                int idx = app->list_sb.offset + row;
                if (idx >= 0 && idx < app->entry_count) {
                    unsigned int now = uptime();
                    if (idx == app->last_click_idx && (now - app->last_click_ticks) <= 45) {
                        app->selected = idx;
                        activate_selected(app);
                        app->last_click_idx = -1;
                    } else {
                        app->selected = idx;
                        app->last_click_idx = idx;
                        app->last_click_ticks = now;
                        load_preview(app);
                    }
                    dirty = 1;
                }
            }
        } else if (ev.type == GUI_EVENT_KEY_PRESS && !app->path_input.focused) {
            if (ev.key == GUI_KEY_ESC || (!(ev.modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT)) && (ev.key == 'q' || ev.key == 'Q'))) {
                break;
            } else if (ev.key == GUI_KEY_UP && app->entry_count > 0) {
                if (app->selected > 0) app->selected--;
                else app->selected = 0;
                ensure_selection_visible(app);
                load_preview(app);
                dirty = 1;
            } else if (ev.key == GUI_KEY_DOWN && app->entry_count > 0) {
                if (app->selected < app->entry_count - 1) app->selected++;
                ensure_selection_visible(app);
                load_preview(app);
                dirty = 1;
            } else if (ev.key == GUI_KEY_ENTER) {
                activate_selected(app);
                dirty = 1;
            } else if (ev.key == GUI_KEY_BACKSPACE) {
                char up[128];
                parent_path(app->cwd, up, sizeof(up));
                load_directory(app, up);
                dirty = 1;
            } else if (ev.key == 'r' || ev.key == 'R') {
                load_directory(app, app->cwd);
                dirty = 1;
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

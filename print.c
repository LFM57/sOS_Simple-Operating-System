#include "system.h"
#include "gui.h"
#define FONT8x16_IMPLEMENTATION /* Tell the header to instantiate the array here */
#include "font8x16.h"

#define PORT_COM1 0x3f8

/* --- GRAPHICS ENGINE --- */
int is_graphics_mode = 0;
int is_gui_mode = 0;  /* When 1, compositor owns backbuffer and lfb_mem */
int show_cursor = 1; /* Global flag to toggle the software cursor */
uint32_t* lfb_mem = (uint32_t*)0xE0000000;    /* Physical Video RAM mapped here */
uint32_t* backbuffer = (uint32_t*)0xE1000000; /* RAM Backbuffer mapped here */
uint32_t g_width = 0, g_height = 0, g_pitch = 0, g_bpp = 0;

uint32_t term_fg_color = 0x00FFFFFF; /* Default: White */
uint32_t term_bg_color = 0x00000000; /* Default: Black */

/* Fast 32-bit block copy using x86 string instructions (burst hardware speed) */
static inline void fast_memcpy(void* dest, const void* src, uint32_t dwords) {
    __asm__ volatile (
        "cld\n\t"
        "rep movsl"
        : "+D"(dest), "+S"(src), "+c"(dwords)
        :
        : "memory"
    );
}

/* Fast 32-bit block fill using x86 string instructions */
static inline void fast_memset(void* dest, uint32_t val, uint32_t dwords) {
    __asm__ volatile (
        "cld\n\t"
        "rep stosl"
        : "+D"(dest), "+c"(dwords)
        : "a"(val)
        : "memory"
    );
}

void init_graphics(uint32_t phys_addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    if (!phys_addr) return; /* Bootloader failed to set graphical mode */
    
    g_width = width; g_height = height; 
    g_pitch = pitch; g_bpp = bpp;
    is_graphics_mode = 1;

    /* 1. Map Physical LFB to Virtual 0xE0000000 */
    uint32_t fb_size = height * pitch;
    for (uint32_t i = 0; i < fb_size; i += 4096) {
        map_page(0xE0000000 + i, phys_addr + i);
    }

    /* 2. Allocate Backbuffer directly from Physical Memory Manager */
    for (uint32_t i = 0; i < fb_size; i += 4096) {
        void* page = pmm_alloc_page();
        map_page(0xE1000000 + i, (uint32_t)page);
    }
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!is_graphics_mode || x >= g_width || y >= g_height) return;
    
    uint32_t offset = (y * g_pitch) + (x * (g_bpp / 8));
    *(uint32_t*)((uint8_t*)backbuffer + offset) = color;
    if (!is_gui_mode) {
        *(uint32_t*)((uint8_t*)lfb_mem + offset) = color;
    }
}

void update_software_cursor(void);

void swap_buffers(void) {
    if (!is_graphics_mode) return;
    uint32_t total_dwords = (g_height * g_pitch) / 4;
    
    /* Fast block copy from RAM backbuffer to Video memory (completes in ~1 ms) */
    fast_memcpy(lfb_mem, backbuffer, total_dwords);

    if (!is_gui_mode) {
        update_software_cursor();
    }
}

void flush_rect(int x, int y, int w, int h) {
    if (!is_graphics_mode) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_width) w = (int)g_width - x;
    if (y + h > (int)g_height) h = (int)g_height - y;
    if (w <= 0 || h <= 0) return;

    uint32_t bpp_bytes = g_bpp / 8;
    for (int row = y; row < y + h; row++) {
        uint32_t offset = ((uint32_t)row * g_pitch) + ((uint32_t)x * bpp_bytes);
        fast_memcpy((uint8_t*)lfb_mem + offset, (uint8_t*)backbuffer + offset, (uint32_t)w);
    }
}

/* --- BACKBUFFER-ONLY PRIMITIVES (GUI / COMPOSITOR) --- */

static int bb_clip_active = 0;
static int bb_clip_x = 0;
static int bb_clip_y = 0;
static int bb_clip_w = 0;
static int bb_clip_h = 0;

void bb_set_clip(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_width) w = (int)g_width - x;
    if (y + h > (int)g_height) h = (int)g_height - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    bb_clip_x = x;
    bb_clip_y = y;
    bb_clip_w = w;
    bb_clip_h = h;
    bb_clip_active = 1;
}

void bb_reset_clip(void) {
    bb_clip_active = 0;
}

void bb_put_pixel(int x, int y, uint32_t color) {
    if (!is_graphics_mode || x < 0 || y < 0 || x >= (int)g_width || y >= (int)g_height) return;
    if (bb_clip_active) {
        if (x < bb_clip_x || x >= bb_clip_x + bb_clip_w ||
            y < bb_clip_y || y >= bb_clip_y + bb_clip_h) return;
    }
    uint32_t offset = ((uint32_t)y * g_pitch) + ((uint32_t)x * (g_bpp / 8));
    *(uint32_t*)((uint8_t*)backbuffer + offset) = color;
}

void bb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!is_graphics_mode) return;
    int cx0 = bb_clip_active ? bb_clip_x : 0;
    int cy0 = bb_clip_active ? bb_clip_y : 0;
    int cx1 = bb_clip_active ? (bb_clip_x + bb_clip_w) : (int)g_width;
    int cy1 = bb_clip_active ? (bb_clip_y + bb_clip_h) : (int)g_height;

    if (x < cx0) { w -= (cx0 - x); x = cx0; }
    if (y < cy0) { h -= (cy0 - y); y = cy0; }
    if (x + w > cx1) w = cx1 - x;
    if (y + h > cy1) h = cy1 - y;
    if (w <= 0 || h <= 0) return;

    uint32_t bpp_bytes = g_bpp / 8;
    for (int row = y; row < y + h; row++) {
        uint32_t offset = ((uint32_t)row * g_pitch) + ((uint32_t)x * bpp_bytes);
        fast_memset((uint8_t*)backbuffer + offset, color, (uint32_t)w);
    }
}

void bb_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    bb_fill_rect(x, y, w, 1, color);
    bb_fill_rect(x, y + h - 1, w, 1, color);
    bb_fill_rect(x, y, 1, h, color);
    bb_fill_rect(x + w - 1, y, 1, h, color);
}

static void render_glyph_to_surface(uint32_t* surface, int surf_w, int surf_h, uint32_t pitch_bytes,
                                    char c, int x, int y, uint32_t fg_color, uint32_t bg_color, int transparent_bg) {
    unsigned char idx = (unsigned char)c;
    if (idx >= 128) idx = '?';
    const unsigned char* glyph = font8x16[idx];

    int clip_x0 = 0;
    int clip_y0 = 0;
    int clip_x1 = surf_w;
    int clip_y1 = surf_h;
    if (surface == backbuffer && bb_clip_active) {
        clip_x0 = bb_clip_x;
        clip_y0 = bb_clip_y;
        clip_x1 = bb_clip_x + bb_clip_w;
        clip_y1 = bb_clip_y + bb_clip_h;
    }

    for (int cy = 0; cy < 16; cy++) {
        int py = y + cy;
        if (py < clip_y0 || py >= clip_y1) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)surface + ((uint32_t)py * pitch_bytes));
        unsigned char row_bits = glyph[cy];
        for (int cx = 0; cx < 8; cx++) {
            int px = x + cx;
            if (px < clip_x0 || px >= clip_x1) continue;
            if (row_bits & (0x80 >> cx)) {
                row[px] = fg_color;
            } else if (!transparent_bg) {
                row[px] = bg_color;
            }
        }
    }
}

void surface_draw_char(uint32_t* surface, int surf_w, int surf_h, char c, int x, int y, uint32_t fg_color, uint32_t bg_color) {
    if (!surface || surf_w <= 0 || surf_h <= 0) return;
    render_glyph_to_surface(surface, surf_w, surf_h, (uint32_t)surf_w * 4, c, x, y, fg_color, bg_color, 0);
}

void bb_draw_char(char c, int x, int y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode) return;
    render_glyph_to_surface(backbuffer, (int)g_width, (int)g_height, g_pitch, c, x, y, fg_color, bg_color, 0);
}

void bb_draw_char_transparent(char c, int x, int y, uint32_t fg_color) {
    if (!is_graphics_mode) return;
    render_glyph_to_surface(backbuffer, (int)g_width, (int)g_height, g_pitch, c, x, y, fg_color, 0, 1);
}

void bb_draw_string(const char* str, int x, int y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode || !str) return;
    int current_x = x;
    while (*str) {
        bb_draw_char(*str, current_x, y, fg_color, bg_color);
        current_x += 8;
        str++;
    }
}

void bb_draw_string_transparent(const char* str, int x, int y, uint32_t fg_color) {
    if (!is_graphics_mode || !str) return;
    int current_x = x;
    while (*str) {
        bb_draw_char_transparent(*str, current_x, y, fg_color);
        current_x += 8;
        str++;
    }
}

void bb_draw_sprite(const uint32_t* sprite, uint32_t width, uint32_t height, int x, int y) {
    if (!is_graphics_mode || !sprite) return;
    uint32_t bpp_bytes = g_bpp / 8;

    for (uint32_t sy = 0; sy < height; sy++) {
        int dy = y + (int)sy;
        if (dy < 0 || dy >= (int)g_height) continue;

        for (uint32_t sx = 0; sx < width; sx++) {
            int dx = x + (int)sx;
            if (dx < 0 || dx >= (int)g_width) continue;

            uint32_t src_color = sprite[sy * width + sx];
            uint32_t a = (src_color >> 24) & 0xFF;

            if (a == 255) {
                bb_put_pixel(dx, dy, src_color);
            } else if (a > 0) {
                uint32_t offset = ((uint32_t)dy * g_pitch) + ((uint32_t)dx * bpp_bytes);
                uint32_t bg_color = *(uint32_t*)((uint8_t*)backbuffer + offset);

                uint32_t br = (bg_color >> 16) & 0xFF;
                uint32_t bg = (bg_color >> 8) & 0xFF;
                uint32_t bb = bg_color & 0xFF;

                uint32_t sr = (src_color >> 16) & 0xFF;
                uint32_t sg = (src_color >> 8) & 0xFF;
                uint32_t sb = src_color & 0xFF;

                uint32_t new_r = (sr * a + br * (255 - a)) / 255;
                uint32_t new_g = (sg * a + bg * (255 - a)) / 255;
                uint32_t new_b = (sb * a + bb * (255 - a)) / 255;

                bb_put_pixel(dx, dy, (new_r << 16) | (new_g << 8) | new_b);
            }
        }
    }
}

void bb_blit_surface(const uint32_t* src, int src_w, int src_h, int dst_x, int dst_y) {
    if (!is_graphics_mode || !src || src_w <= 0 || src_h <= 0) return;

    int cx0 = bb_clip_active ? bb_clip_x : 0;
    int cy0 = bb_clip_active ? bb_clip_y : 0;
    int cx1 = bb_clip_active ? (bb_clip_x + bb_clip_w) : (int)g_width;
    int cy1 = bb_clip_active ? (bb_clip_y + bb_clip_h) : (int)g_height;

    int src_x_start = 0;
    int src_y_start = 0;
    int copy_w = src_w;
    int copy_h = src_h;

    if (dst_x < cx0) { src_x_start = cx0 - dst_x; copy_w -= src_x_start; dst_x = cx0; }
    if (dst_y < cy0) { src_y_start = cy0 - dst_y; copy_h -= src_y_start; dst_y = cy0; }
    if (dst_x + copy_w > cx1) copy_w = cx1 - dst_x;
    if (dst_y + copy_h > cy1) copy_h = cy1 - dst_y;
    if (copy_w <= 0 || copy_h <= 0) return;

    uint32_t bpp_bytes = g_bpp / 8;
    for (int row = 0; row < copy_h; row++) {
        const uint32_t* src_row = src + ((src_y_start + row) * src_w) + src_x_start;
        uint8_t* dst_row = (uint8_t*)backbuffer + ((uint32_t)(dst_y + row) * g_pitch) + ((uint32_t)dst_x * bpp_bytes);
        fast_memcpy(dst_row, src_row, (uint32_t)copy_w);
    }
}

/* Optimized glyph rendering: writes scanlines directly without per-pixel call overhead */
void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode || x + 8 > g_width || y + 16 > g_height) return;

    unsigned char idx = (unsigned char)c;
    if (idx >= 128) idx = '?';
    const unsigned char* glyph = font8x16[idx];
    uint32_t bytes_per_pixel = g_bpp / 8;
    uint32_t row_offset = (y * g_pitch) + (x * bytes_per_pixel);

    for (uint32_t cy = 0; cy < 16; cy++) {
        uint32_t* bb_row = (uint32_t*)((uint8_t*)backbuffer + row_offset);
        uint32_t* lfb_row = (uint32_t*)((uint8_t*)lfb_mem + row_offset);
        unsigned char row_bits = glyph[cy];

        for (uint32_t cx = 0; cx < 8; cx++) {
            uint32_t color = (row_bits & (0x80 >> cx)) ? fg_color : bg_color;
            bb_row[cx] = color;
            if (!is_gui_mode) {
                lfb_row[cx] = color;
            }
        }
        row_offset += g_pitch;
    }
}

void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if (!is_graphics_mode) return;
    
    uint32_t current_x = x;
    while (*str) {
        draw_char(*str, current_x, y, fg_color, bg_color);
        current_x += 8;
        str++;
    }
}

/* Global variables for VGA display and ANSI handling */
uint16_t* video_memory = (uint16_t*)0xB8000;
int cursor_x = 0;
int cursor_y = 0;

static int ansi_state = 0;
static int ansi_val = 0;
static uint16_t vga_attribute = 0x0F00; /* White on black by default */

void scroll(void); 

void update_cursor(int x, int y) {
    if (is_graphics_mode) return; 

    uint16_t pos = y * 80 + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void update_software_cursor(void) {
    if (!is_graphics_mode || is_gui_mode) return;
    static uint32_t last_cx = 0, last_cy = 0;
    uint32_t bytes_per_pixel = g_bpp / 8;

    /* 1. RESTORE: Restore previous cursor glyph area from clean backbuffer */
    for (int cy = 13; cy < 15; cy++) {
        uint32_t offset = ((last_cy * 16 + cy) * g_pitch) + (last_cx * 8 * bytes_per_pixel);
        if (offset + (8 * bytes_per_pixel) <= g_height * g_pitch) {
            fast_memcpy((uint8_t*)lfb_mem + offset, (uint8_t*)backbuffer + offset, 8);
        }
    }

    if (!show_cursor) return;

    /* 2. DRAW: Draw underline cursor (scanlines 13-14) directly into LFB */
    for (int cy = 13; cy < 15; cy++) {
        uint32_t offset = ((cursor_y * 16 + cy) * g_pitch) + (cursor_x * 8 * bytes_per_pixel);
        if (offset + (8 * bytes_per_pixel) <= g_height * g_pitch) {
            fast_memset((uint8_t*)lfb_mem + offset, term_fg_color, 8);
        }
    }

    last_cx = cursor_x;
    last_cy = cursor_y;
}

/* --- GUI TERMINAL CURSOR HELPERS --- */
static int term_cursor_drawn = 0;
static int last_term_cx = 0;
static int last_term_cy = 0;
static uint32_t saved_term_cursor[16];

static void gui_term_erase_cursor(void) {
    if (!term_cursor_drawn || !gui_terminal_window || !gui_terminal_window->canvas) {
        term_cursor_drawn = 0;
        return;
    }
    int w = gui_terminal_window->width;
    int h = gui_terminal_window->height;
    uint32_t* canvas = gui_terminal_window->canvas;

    for (int cy = 0; cy < 2; cy++) {
        int py = last_term_cy * 16 + 13 + cy;
        if (py < 0 || py >= h) continue;
        for (int cx = 0; cx < 8; cx++) {
            int px = last_term_cx * 8 + cx;
            if (px < 0 || px >= w) continue;
            canvas[py * w + px] = saved_term_cursor[cy * 8 + cx];
        }
    }
    term_cursor_drawn = 0;
}

static void gui_term_draw_cursor(void) {
    if (!show_cursor || !gui_terminal_window || !gui_terminal_window->canvas) return;
    int w = gui_terminal_window->width;
    int h = gui_terminal_window->height;
    uint32_t* canvas = gui_terminal_window->canvas;

    for (int cy = 0; cy < 2; cy++) {
        int py = cursor_y * 16 + 13 + cy;
        for (int cx = 0; cx < 8; cx++) {
            int px = cursor_x * 8 + cx;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                saved_term_cursor[cy * 8 + cx] = canvas[py * w + px];
                canvas[py * w + px] = term_fg_color;
            } else {
                saved_term_cursor[cy * 8 + cx] = term_bg_color;
            }
        }
    }
    last_term_cx = cursor_x;
    last_term_cy = cursor_y;
    term_cursor_drawn = 1;
}

/* --- GUI TERMINAL SCROLLBACK BUFFER (Phase 3.3) --- */
#define TERM_SCROLLBACK_LINES 200
#define TERM_MAX_COLS         160
#define TERM_MAX_ROWS         80

typedef struct {
    char ch;
    uint8_t attr;
} term_sb_cell_t;

static const uint32_t term_palette[10] = {
    0x00000000, /* 0: Black */
    0x00FF5555, /* 1: Red */
    0x0055FF55, /* 2: Green */
    0x00FFFF55, /* 3: Yellow */
    0x005555FF, /* 4: Blue */
    0x00FF55FF, /* 5: Magenta */
    0x0055FFFF, /* 6: Cyan */
    0x00AAAAAA, /* 7: Light Gray */
    0x00FFFFFF, /* 8: White */
    0x000000AA  /* 9: BSOD Blue */
};

static term_sb_cell_t term_sb[TERM_SCROLLBACK_LINES][TERM_MAX_COLS];
static term_sb_cell_t term_render_view[TERM_MAX_ROWS][TERM_MAX_COLS];
static int term_sb_head = 0;
static int term_sb_count = 1;
static int term_scroll_offset = 0;
static volatile int term_scroll_pending_lines = 0;
static volatile int term_scroll_pending_pages = 0;

static uint8_t term_color_to_idx(uint32_t col, uint8_t fallback) {
    for (uint8_t i = 0; i < 10; i++) {
        if (term_palette[i] == col) return i;
    }
    return fallback;
}

static uint8_t term_current_attr(void) {
    uint8_t fg = term_color_to_idx(term_fg_color, 8);
    uint8_t bg = term_color_to_idx(term_bg_color, 0);
    return (uint8_t)(((bg & 0x0F) << 4) | (fg & 0x0F));
}

static void term_sb_fill_row(int slot, uint8_t attr) {
    for (int col = 0; col < TERM_MAX_COLS; col++) {
        term_sb[slot][col].ch = ' ';
        term_sb[slot][col].attr = attr;
    }
}

static void term_sb_clear(void) {
    uint32_t eflags = irq_save_disable();
    term_sb_head = 0;
    term_sb_count = 1;
    term_scroll_offset = 0;
    term_scroll_pending_lines = 0;
    term_scroll_pending_pages = 0;
    term_sb_fill_row(0, term_current_attr());
    irq_restore(eflags);
}

static void term_sb_newline(void) {
    uint32_t eflags = irq_save_disable();
    if (term_sb_count < TERM_SCROLLBACK_LINES) {
        term_sb_count++;
    } else {
        term_sb_head = (term_sb_head + 1) % TERM_SCROLLBACK_LINES;
    }
    int bottom_slot = (term_sb_head + term_sb_count - 1) % TERM_SCROLLBACK_LINES;
    term_sb_fill_row(bottom_slot, term_current_attr());
    irq_restore(eflags);
}

static void term_sb_put_cell(int logical_row, int col, char c) {
    if (col < 0 || col >= TERM_MAX_COLS) return;
    uint32_t eflags = irq_save_disable();
    if (logical_row >= 0 && logical_row < term_sb_count) {
        int slot = (term_sb_head + logical_row) % TERM_SCROLLBACK_LINES;
        term_sb[slot][col].ch = c;
        term_sb[slot][col].attr = term_current_attr();
    }
    irq_restore(eflags);
}

static void gui_term_redraw_from_scrollback(void) {
    if (!gui_terminal_window || !gui_terminal_window->canvas) return;
    int w = gui_terminal_window->width;
    int h = gui_terminal_window->height;
    uint32_t* canvas = gui_terminal_window->canvas;
    int term_cols = w / 8;
    int term_rows = h / 16;
    if (term_cols < 1) term_cols = 1;
    if (term_rows < 1) term_rows = 1;
    int draw_cols = (term_cols < TERM_MAX_COLS) ? term_cols : TERM_MAX_COLS;
    int max_vis_rows = (term_rows < TERM_MAX_ROWS) ? term_rows : TERM_MAX_ROWS;

    uint32_t eflags = irq_save_disable();
    int max_offset = (term_sb_count > term_rows) ? (term_sb_count - term_rows) : 0;
    if (term_scroll_offset < 0) term_scroll_offset = 0;
    if (term_scroll_offset > max_offset) term_scroll_offset = max_offset;
    int offset_snap = term_scroll_offset;
    int sb_count_snap = term_sb_count;
    int view_start = (term_sb_count > term_rows) ? (term_sb_count - term_rows - term_scroll_offset) : 0;
    int copy_rows = term_sb_count - view_start;
    if (copy_rows > max_vis_rows) copy_rows = max_vis_rows;
    for (int r = 0; r < copy_rows; r++) {
        int slot = (term_sb_head + view_start + r) % TERM_SCROLLBACK_LINES;
        for (int c = 0; c < draw_cols; c++) {
            term_render_view[r][c] = term_sb[slot][c];
        }
    }
    irq_restore(eflags);

    term_cursor_drawn = 0;
    fast_memset(canvas, term_bg_color, (uint32_t)w * (uint32_t)h);

    for (int r = 0; r < copy_rows; r++) {
        int py = r * 16;
        for (int c = 0; c < draw_cols; c++) {
            term_sb_cell_t cell = term_render_view[r][c];
            uint32_t fg = term_palette[cell.attr & 0x0F];
            uint32_t bg = term_palette[(cell.attr >> 4) & 0x0F];
            if (cell.ch != ' ' || bg != term_bg_color) {
                surface_draw_char(canvas, w, h, cell.ch, c * 8, py, fg, bg);
            }
        }
    }

    if (offset_snap > 0 && sb_count_snap > term_rows && w >= 6 && h >= 20) {
        int sb_x = w - 4;
        int track_h = h - 4;
        int thumb_h = (term_rows * track_h) / sb_count_snap;
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > track_h) thumb_h = track_h;
        int max_off = sb_count_snap - term_rows;
        int top_line = max_off - offset_snap;
        int thumb_y = 2 + ((max_off > 0) ? ((track_h - thumb_h) * top_line) / max_off : 0);

        for (int y = 2; y < 2 + track_h; y++) {
            uint32_t col = (y >= thumb_y && y < thumb_y + thumb_h) ? 0x0064748B : 0x001E293B;
            for (int dx = 0; dx < 3; dx++) {
                canvas[y * w + sb_x + dx] = col;
            }
        }
    }

    if (offset_snap == 0) {
        gui_term_draw_cursor();
    }
    gui_mark_window_dirty(gui_terminal_window, 0, 0, w, h);
}

void gui_terminal_scroll_lines(int delta_lines) {
    if (!is_graphics_mode || !is_gui_mode || delta_lines == 0) return;
    uint32_t eflags = irq_save_disable();
    term_scroll_pending_lines += delta_lines;
    irq_restore(eflags);
    gui_mark_dirty(0);
}

void gui_terminal_scroll_page(int delta_pages) {
    if (!is_graphics_mode || !is_gui_mode || delta_pages == 0) return;
    uint32_t eflags = irq_save_disable();
    term_scroll_pending_pages += delta_pages;
    irq_restore(eflags);
    gui_mark_dirty(0);
}

void gui_terminal_process_scroll(void) {
    if (!is_graphics_mode || !is_gui_mode || !gui_terminal_window || !gui_terminal_window->canvas) return;
    uint32_t eflags = irq_save_disable();
    int d_lines = term_scroll_pending_lines;
    int d_pages = term_scroll_pending_pages;
    term_scroll_pending_lines = 0;
    term_scroll_pending_pages = 0;
    irq_restore(eflags);

    if (d_lines == 0 && d_pages == 0) return;

    int term_rows = gui_terminal_window->height / 16;
    if (term_rows < 1) term_rows = 1;
    int page_step = (term_rows > 2) ? (term_rows - 2) : 1;
    int total_delta = d_lines + d_pages * page_step;

    int max_offset = (term_sb_count > term_rows) ? (term_sb_count - term_rows) : 0;
    int new_offset = term_scroll_offset + total_delta;
    if (new_offset < 0) new_offset = 0;
    if (new_offset > max_offset) new_offset = max_offset;

    if (new_offset != term_scroll_offset) {
        term_scroll_offset = new_offset;
        gui_term_redraw_from_scrollback();
    }
}

void gui_terminal_before_resize(void) {
    if (!is_graphics_mode || !is_gui_mode) return;
    gui_term_erase_cursor();
}

void gui_terminal_after_resize(void) {
    if (!is_graphics_mode || !is_gui_mode || !gui_terminal_window || !gui_terminal_window->canvas) return;
    int term_cols = gui_terminal_window->width / 8;
    int term_rows = gui_terminal_window->height / 16;
    if (term_cols < 1) term_cols = 1;
    if (term_rows < 1) term_rows = 1;
    if (cursor_x >= term_cols) cursor_x = term_cols - 1;
    if (cursor_x < 0) cursor_x = 0;
    term_scroll_offset = 0;
    cursor_y = (term_sb_count <= term_rows) ? (term_sb_count - 1) : (term_rows - 1);
    if (cursor_y < 0) cursor_y = 0;
    gui_term_redraw_from_scrollback();
}

void write_vga(char c) {
    if (ansi_state == 0) {
        if (c == '\033') {
            ansi_state = 1;
            return;
        }
        
        if (is_graphics_mode) {
            if (is_gui_mode) {
                if (!gui_terminal_window || !gui_terminal_window->canvas) return;
                if (term_scroll_offset != 0) {
                    term_scroll_offset = 0;
                    gui_term_redraw_from_scrollback();
                }
                int had_cursor = term_cursor_drawn;
                int prev_cx = last_term_cx;
                int prev_cy = last_term_cy;
                gui_term_erase_cursor();
                if (had_cursor) {
                    gui_mark_window_dirty(gui_terminal_window, prev_cx * 8, prev_cy * 16 + 13, 8, 2);
                }
                int term_cols = gui_terminal_window->width / 8;
                int term_rows = gui_terminal_window->height / 16;
                if (term_cols < 1) term_cols = 1;
                if (term_rows < 1) term_rows = 1;
                int bottom_start = (term_sb_count > term_rows) ? (term_sb_count - term_rows) : 0;

                if (c == '\n') {
                    cursor_x = 0;
                    cursor_y++;
                    if (bottom_start + cursor_y >= term_sb_count) {
                        term_sb_newline();
                    }
                } else if (c == '\b') {
                    if (cursor_x > 0) {
                        cursor_x--;
                    } else if (cursor_y > 0) {
                        cursor_y--;
                        cursor_x = term_cols - 1;
                    }
                } else if (c == '\r') {
                    cursor_x = 0;
                } else {
                    term_sb_put_cell(bottom_start + cursor_y, cursor_x, c);
                    surface_draw_char(gui_terminal_window->canvas,
                                      gui_terminal_window->width,
                                      gui_terminal_window->height,
                                      c, cursor_x * 8, cursor_y * 16,
                                      term_fg_color, term_bg_color);
                    gui_mark_window_dirty(gui_terminal_window, cursor_x * 8, cursor_y * 16, 8, 16);
                    cursor_x++;
                    if (cursor_x >= term_cols) {
                        cursor_x = 0;
                        cursor_y++;
                        if (bottom_start + cursor_y >= term_sb_count) {
                            term_sb_newline();
                        }
                    }
                }
                scroll();
                gui_term_draw_cursor();
                if (term_cursor_drawn) {
                    gui_mark_window_dirty(gui_terminal_window, cursor_x * 8, cursor_y * 16 + 13, 8, 2);
                }
                return;
            }
            int term_cols = g_width / 8;
            if (c == '\n') {
                cursor_x = 0;
                cursor_y++;
            } else if (c == '\b') {
                if (cursor_x > 0) {
                    cursor_x--;
                } else if (cursor_y > 0) {
                    cursor_y--; 
                    cursor_x = term_cols - 1;
                }
            } else if (c == '\r') {
                cursor_x = 0;
            } else {
                draw_char(c, cursor_x * 8, cursor_y * 16, term_fg_color, term_bg_color);
                cursor_x++;
                if (cursor_x >= term_cols) { cursor_x = 0; cursor_y++; }
            }
            scroll();
            update_software_cursor(); 
            return;
        }

        /* Classic VGA character display */
        if (c == '\n') { cursor_x = 0; cursor_y++; } 
        else if (c == '\b') {
            if (cursor_x > 0) cursor_x--;
            else if (cursor_y > 0) { cursor_y--; cursor_x = 79; }
        } 
        else if (c == '\r') { cursor_x = 0; } 
        else {
            video_memory[cursor_y * 80 + cursor_x] = (uint16_t)c | vga_attribute;
            cursor_x++;
            if (cursor_x >= 80) { cursor_x = 0; cursor_y++; }
        }
        scroll();
        update_cursor(cursor_x, cursor_y);
    } 
    else if (ansi_state == 1) {
        if (c == '[') { ansi_state = 2; ansi_val = 0; } 
        else { ansi_state = 0; }
    } 
    else if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            ansi_val = ansi_val * 10 + (c - '0');
        } else if (c == ';') {
            ansi_val = 0; 
        } else if (c == 'm') {
            if (ansi_val == 0)       { vga_attribute = 0x0F00; term_fg_color = 0x00FFFFFF; }
            else if (ansi_val == 31) { vga_attribute = 0x0C00; term_fg_color = 0x00FF5555; }
            else if (ansi_val == 32) { vga_attribute = 0x0A00; term_fg_color = 0x0055FF55; }
            else if (ansi_val == 33) { vga_attribute = 0x0E00; term_fg_color = 0x00FFFF55; }
            else if (ansi_val == 34) { vga_attribute = 0x0900; term_fg_color = 0x005555FF; }
            else if (ansi_val == 35) { vga_attribute = 0x0D00; term_fg_color = 0x00FF55FF; }
            else if (ansi_val == 36) { vga_attribute = 0x0B00; term_fg_color = 0x0055FFFF; }
            else if (ansi_val == 37) { vga_attribute = 0x0700; term_fg_color = 0x00AAAAAA; }
            ansi_state = 0;
            ansi_val = 0;
        } else {
            ansi_state = 0;
            ansi_val = 0;
        }
    }
}

/* Instantaneous Scrolling Engine */
void scroll(void) {
    if (is_graphics_mode) {
        if (is_gui_mode) {
            if (!gui_terminal_window || !gui_terminal_window->canvas) return;
            int term_rows = gui_terminal_window->height / 16;
            if (cursor_y < term_rows) return;

            uint32_t* canvas = gui_terminal_window->canvas;
            uint32_t row_dwords = (uint32_t)gui_terminal_window->width * 16;
            uint32_t lines = (uint32_t)(cursor_y - term_rows + 1);

            if (lines >= (uint32_t)term_rows) {
                fast_memset(canvas, term_bg_color,
                            (uint32_t)gui_terminal_window->width * (uint32_t)gui_terminal_window->height);
                cursor_y = 0;
            } else {
                uint32_t shift_dwords = (uint32_t)(term_rows - (int)lines) * row_dwords;
                fast_memcpy(canvas, canvas + (lines * row_dwords), shift_dwords);
                fast_memset(canvas + shift_dwords, term_bg_color, lines * row_dwords);
                cursor_y = term_rows - 1;
            }
            gui_mark_window_dirty(gui_terminal_window, 0, 0,
                                  gui_terminal_window->width, gui_terminal_window->height);
            return;
        }

        int term_rows = g_height / 16;
        if (cursor_y < term_rows) return;

        uint32_t row_bytes = 16 * g_pitch;
        uint32_t lines = cursor_y - term_rows + 1;

        if (lines >= (uint32_t)term_rows) {
            fast_memset(backbuffer, term_bg_color, (g_height * g_pitch) / 4);
            cursor_y = 0;
        } else {
            uint32_t shift_bytes = (term_rows - lines) * row_bytes;
            /* Shift the backbuffer up in RAM */
            fast_memcpy(backbuffer, (uint8_t*)backbuffer + (lines * row_bytes), shift_bytes / 4);
            /* Clear the bottom rows with background color */
            fast_memset((uint8_t*)backbuffer + shift_bytes, term_bg_color, (lines * row_bytes) / 4);
            cursor_y = term_rows - 1;
        }

        /* Fast single-pass flush to VRAM */
        swap_buffers(); 
        return;
    }

    /* Fallback VGA Scroll */
    if (cursor_y >= 25) {
        for (int i = 0; i < 24 * 80; i++) {
            video_memory[i] = video_memory[i + 80];
        }
        for (int i = 24 * 80; i < 25 * 80; i++) {
            video_memory[i] = 0x0F20; 
        }
        cursor_y = 24;
    }
}

extern task_t tasks[MAX_TASKS];
extern int current_task;
extern volatile int fg_task;

void kputc(char c) {
    if (current_task == 0 && fg_task == -1 &&
        tasks[0].open_files[1].is_used &&
        tasks[0].open_files[1].node != NULL &&
        tasks[0].open_files[1].node != vfs_get_tty_node()) {
        static int in_redirect = 0;
        if (!in_redirect) {
            in_redirect = 1;
            file_descriptor_t* fd_entry = &tasks[0].open_files[1];
            uint8_t ch = (uint8_t)c;
            if (fd_entry->node->type == VFS_FILE && (fd_entry->flags & O_APPEND)) {
                fd_entry->offset = fd_entry->node->size;
            }
            int w = vfs_write(fd_entry->node, fd_entry->offset, 1, &ch);
            if (w > 0 && fd_entry->node->type == VFS_FILE) {
                fd_entry->offset += (uint32_t)w;
            }
            in_redirect = 0;
            return;
        }
    }
    write_vga(c);
}

void clear_terminal(void) {
    if (is_graphics_mode) {
        if (is_gui_mode) {
            term_cursor_drawn = 0;
            term_sb_clear();
            if (gui_terminal_window && gui_terminal_window->canvas) {
                fast_memset(gui_terminal_window->canvas, term_bg_color,
                            (uint32_t)gui_terminal_window->width * (uint32_t)gui_terminal_window->height);
                cursor_x = 0;
                cursor_y = 0;
                gui_term_draw_cursor();
                gui_mark_window_dirty(gui_terminal_window, 0, 0,
                                      gui_terminal_window->width, gui_terminal_window->height);
            }
            return;
        }
        uint32_t total_pixels = (g_height * g_pitch) / 4;
        fast_memset(backbuffer, term_bg_color, total_pixels);
        cursor_x = 0; 
        cursor_y = 0;
        swap_buffers();
        return;
    }

    for (int i = 0; i < 25 * 80; i++) {
        video_memory[i] = 0x0F20; 
    }
    cursor_x = 0; cursor_y = 0;
    update_cursor(0, 0);
}

void print_number(uint32_t num, int base) {
    if (num == 0) { kputc('0'); return; }
    char buffer[32];
    int i = 0;
    const char* charset = "0123456789ABCDEF";
    while (num > 0) { buffer[i++] = charset[num % base]; num /= base; }
    while (i > 0) { i--; kputc(buffer[i]); }
}

void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%' && format[i + 1] != '\0') {
            i++;
            switch (format[i]) {
                case 's': {
                    const char* str = va_arg(args, const char*);
                    if (!str) str = "(null)";
                    while (*str) {
                        kputc(*str++);
                    }
                    break;
                }
                case 'd': {
                    int num = va_arg(args, int);
                    if (num < 0) { kputc('-'); num = -num; }
                    print_number((uint32_t)num, 10);
                    break;
                }
                case 'x': kprintf("0x"); print_number(va_arg(args, uint32_t), 16); break;
                case 'c': kputc((char)va_arg(args, int)); break;
                case '%': kputc('%'); break;
            }
        } else {
            kputc(format[i]);
        }
    }
    va_end(args);
}

void draw_sprite(const uint32_t* sprite, uint32_t width, uint32_t height, uint32_t x, uint32_t y) {
    if (!is_graphics_mode) return;

    for (uint32_t sy = 0; sy < height; sy++) {
        for (uint32_t sx = 0; sx < width; sx++) {
            if (x + sx >= g_width || y + sy >= g_height) continue;

            uint32_t src_color = sprite[sy * width + sx];
            uint32_t a = (src_color >> 24) & 0xFF;

            if (a == 255) {
                put_pixel(x + sx, y + sy, src_color);
            } 
            else if (a > 0) {
                uint32_t offset = ((y + sy) * g_pitch) + ((x + sx) * (g_bpp / 8));
                uint32_t bg_color = *(uint32_t*)((uint8_t*)backbuffer + offset);

                uint32_t br = (bg_color >> 16) & 0xFF;
                uint32_t bg = (bg_color >> 8) & 0xFF;
                uint32_t bb = bg_color & 0xFF;

                uint32_t sr = (src_color >> 16) & 0xFF;
                uint32_t sg = (src_color >> 8) & 0xFF;
                uint32_t sb = src_color & 0xFF;

                uint32_t new_r = (sr * a + br * (255 - a)) / 255;
                uint32_t new_g = (sg * a + bg * (255 - a)) / 255;
                uint32_t new_b = (sb * a + bb * (255 - a)) / 255;

                uint32_t final_color = (new_r << 16) | (new_g << 8) | new_b;
                put_pixel(x + sx, y + sy, final_color);
            }
        }
    }
}

void draw_test_logo(uint32_t start_x, uint32_t start_y) {
    uint32_t sprite[32 * 32];
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int dx = x - 16;
            int dy = y - 16;
            int dist_squared = (dx * dx + dy * dy);
            
            if (dist_squared < 256) {
                uint32_t alpha = 255 - dist_squared; 
                if (alpha > 255) alpha = 0;
                sprite[y * 32 + x] = (alpha << 24) | 0x00FF8800; 
            } else {
                sprite[y * 32 + x] = 0x00000000;
            }
        }
    }
    draw_sprite(sprite, 32, 32, start_x, start_y);
}
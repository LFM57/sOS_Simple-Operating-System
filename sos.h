#ifndef SOS_H
#define SOS_H

/* --- Open flags and seek whence values --- */
#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* --- New descriptor-based I/O system calls --- */
int open(const char* pathname, int flags);
int write(int fd, const void* buf, unsigned int count);
int read(int fd, void* buf, unsigned int count);
int lseek(int fd, int offset, int whence);
int close(int fd);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);
int fork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
int waitpid(int pid, int* status, int options);

/* Updated basic system calls */
void print(const char* str);
void exit(int code);
void* malloc(unsigned int size);
void free(void* ptr);
void* realloc(void* ptr, unsigned int size);
void* calloc(unsigned int nmemb, unsigned int size);

/* User input */
char getchar();
void gets(char* buffer, int max_len);
void print_int(int num);

/* Time */
unsigned int uptime();
void sleep(unsigned int ms);

/* Standard C functions */
int strlen(const char* str);
char* strcpy(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
void* memset(void* s, int c, unsigned int n);
void* memcpy(void* dest, const void* src, unsigned int n);

/* File System functions (Ring 3) */
#define SOS_TYPE_FILE      0x01
#define SOS_TYPE_DIRECTORY 0x02
#define SOS_TYPE_CHARDEV   0x03

typedef struct {
    char name[64];
    unsigned int size;
    unsigned char type;
    unsigned char sec_byte;
} sos_dirent_t;

typedef struct {
    unsigned int size;
    unsigned char type;
    unsigned char sec_byte;
    unsigned short _pad;
    unsigned int inode;
} sos_stat_t;

typedef struct {
    unsigned int id;
    unsigned char state;
    unsigned char uid;
    unsigned char is_background;
    unsigned char is_user;
} sos_task_info_t;

typedef struct {
    unsigned int total_ram_kb;
    unsigned int used_ram_kb;
    unsigned int free_ram_kb;
    unsigned int heap_total_bytes;
    unsigned int heap_used_bytes;
    unsigned int heap_free_bytes;
    unsigned int uptime_ticks;
    unsigned char ip[4];
    unsigned char subnet[4];
    unsigned char gateway[4];
    unsigned char dns[4];
    unsigned char net_installed;
    unsigned char net_link_up;
    unsigned short _pad;
    unsigned int task_count;
    sos_task_info_t tasks[16];
} sos_sysinfo_t;

int read_file(const char* filename, unsigned char* buffer, unsigned int* size_out);
int write_file(const char* filename, const char* text);
int opendir(const char* path);
int readdir(int dir_fd, sos_dirent_t* out_dirent);
int closedir(int dir_fd);
int stat(const char* path, sos_stat_t* out_stat);
int spawn(const char* path, char* const argv[]);
int sysinfo(sos_sysinfo_t* info);
int clip_set(const char* text, int len);
int clip_get(char* buf, int max_len);

/* Networking */
int socket();
int bind(int fd, unsigned short port);
int sendto(int fd, const unsigned char* dest_ip, unsigned short port, const void* buf, unsigned int len);
int recvfrom(int fd, void* buf, unsigned int len);
int get_dns_ip(unsigned char* ip_buf);
int gethostbyname(const char* hostname, unsigned char* out_ip);
int socket_tcp();
int connect(int fd, const unsigned char* dest_ip, unsigned short port);
int send(int fd, const void* buf, unsigned int len);
int recv(int fd, void* buf, unsigned int len);

/* GUI System Calls (Ring 3) */
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
    GUI_EVENT_WINDOW_CLOSE,
    GUI_EVENT_MOUSE_SCROLL,
    GUI_EVENT_WINDOW_RESIZE
} gui_event_type_t;

typedef struct {
    gui_event_type_t type;
    int x, y;
    unsigned char button;
    char key;
    unsigned char modifiers;
    unsigned char scancode;
    int dz;
    int width, height;
} gui_event_t;

int win_create(const char* title, int w, int h);
int win_present(int win_id, const unsigned int* buf);
int win_present_rect(int win_id, const unsigned int* buf, int x, int y, int w, int h);
unsigned int* win_map(int win_id);
int win_invalidate(int win_id, int x, int y, int w, int h);
int win_get_event(int win_id, gui_event_t* out_ev);
int win_poll_event(int win_id, gui_event_t* out_ev);
int win_destroy(int win_id);

#endif
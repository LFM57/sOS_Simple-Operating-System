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
int read_file(const char* filename, unsigned char* buffer, unsigned int* size_out);
int write_file(const char* filename, const char* text);

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
int win_get_event(int win_id, gui_event_t* out_ev);
int win_poll_event(int win_id, gui_event_t* out_ev);
int win_destroy(int win_id);

#endif
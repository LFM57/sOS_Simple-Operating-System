#include "sos.h"

/* Write 'count' bytes from the buffer to descriptor 'fd' */
int write(int fd, const void* buf, unsigned int count) {
    int ret;
    /* Assembly x86 : eax = syscall (1), ebx = fd, ecx = buf, edx = count */
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(1), "b"(fd), "c"(buf), "d"(count) : "memory");
    return ret;
}

/* Read at most 'count' bytes into the buffer from descriptor 'fd' */
int read(int fd, void* buf, unsigned int count) {
    int ret;
    /* Assembly x86 : eax = syscall (4), ebx = fd, ecx = buf, edx = count */
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(4), "b"(fd), "c"(buf), "d"(count) : "memory");
    return ret;
}

/* The print() function uses standard write to stdout (FD 1) */
void print(const char* str) {
    write(1, str, strlen(str));
}

/* The getchar() function uses standard read from stdin (FD 0) */
char getchar() {
    char c;
    int bytes_read = read(0, &c, 1);
    if (bytes_read <= 0) return 0;
    return c;
}

void exit(int code) {
    __asm__ volatile("int $0x80" : : "a"(2), "b"(code));
}

void* sbrk(int increment) {
    void* ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(3), "b"(increment));
    return ret;
}

void* malloc(unsigned int size) {
    if (size == 0) return (void*)0;
    void* addr = sbrk(size);
    if ((int)addr == -1) return (void*)0;
    return addr;
}

/* --- Implementations of standard functions --- */

int strlen(const char* str) {
    int len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

void* memset(void* s, int c, unsigned int n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void* memcpy(void* dest, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/* --- USER INPUT --- */

/* Magic function that reads an entire line with backspace (\b) handling */
void gets(char* buffer, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c = getchar();
        
        if (c == '\n' || c == '\r') {
            print("\n");
            break;
        } else if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                print("\b \b"); /* Visually erase the character */
            }
        } else {
            buffer[i++] = c;
            /* Shows the entered character (Echo) */
            char s[2] = {c, '\0'};
            print(s);
        }
    }
    buffer[i] = '\0';
}

/* --- NUMBER DISPLAY --- */

/* Converts an integer to text so as to display it */
void print_int(int num) {
    if (num == 0) {
        print("0");
        return;
    }
    
    char buffer[16];
    int is_neg = 0;
    if (num < 0) {
        is_neg = 1;
        num = -num;
    }
    
    int i = 0;
    while (num > 0) {
        buffer[i++] = (num % 10) + '0';
        num /= 10;
    }
    if (is_neg) buffer[i++] = '-';
    
    /* Reverse the chain (because we read backwards) */
    char reversed[16];
    int j = 0;
    while (i > 0) {
        reversed[j++] = buffer[--i];
    }
    reversed[j] = '\0';
    
    print(reversed);
}

/* --- TIME MANAGMENT --- */

unsigned int uptime() {
    unsigned int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(5));
    return ret;
}

/* Take a break in milliseconds (100 Hz = 10 ms per tic) */
void sleep(unsigned int ms) {
    unsigned int start = uptime();
    unsigned int ticks_to_wait = ms / 10; 
    while (uptime() < start + ticks_to_wait) {
        /* Active waiting in user space */
    }
}

/* File System */
int read_file(const char* filename, unsigned char* buffer, unsigned int* size_out) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(6), "b"(filename), "c"(buffer), "d"(size_out));
    return ret;
}

int write_file(const char* filename, const char* text) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(7), "b"(filename), "c"(text));
    return ret;
}

typedef struct {
    unsigned char dest_ip[4];
    unsigned short dest_port;
    const void* payload;
    unsigned int len;
} sendto_args_t;

int socket() {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(8));
    return ret;
}

int bind(int fd, unsigned short port) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(9), "b"(fd), "c"(port));
    return ret;
}

int sendto(int fd, const unsigned char* dest_ip, unsigned short port, const void* buf, unsigned int len) {
    sendto_args_t args;
    for(int i=0; i<4; i++) args.dest_ip[i] = dest_ip[i];
    args.dest_port = port;
    args.payload = buf;
    args.len = len;
    
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(10), "b"(fd), "c"(&args));
    return ret;
}

int recvfrom(int fd, void* buf, unsigned int len) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(11), "b"(fd), "c"(buf), "d"(len));
    return ret;
}
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

#endif
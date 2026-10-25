#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/* Open flags */
#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10

/* Seek whence values */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* VFS Node types */
#define VFS_FILE      0x01
#define VFS_DIRECTORY 0x02
#define VFS_CHARDEV   0x03
#define VFS_SOCKET    0x04

typedef struct {
    char name[64];
    uint32_t size;
    uint8_t type;
    uint8_t sec_byte;
} vfs_dirent_t;

struct vfs_node;

typedef struct vfs_ops {
    int (*read)(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer);
    int (*write)(struct vfs_node* node, uint32_t offset, uint32_t size, const uint8_t* buffer);
    int (*open)(struct vfs_node* node, uint32_t flags);
    int (*close)(struct vfs_node* node);
    int (*readdir)(struct vfs_node* node, uint32_t index, vfs_dirent_t* out_dirent);
    struct vfs_node* (*finddir)(struct vfs_node* node, const char* name);
} vfs_ops_t;

typedef struct vfs_node {
    char name[64];
    uint8_t type;
    uint8_t sec_byte;
    uint32_t size;
    uint32_t inode;      /* FAT32: first_cluster */
    uint32_t dir_sec;    /* FAT32: sector of directory entry */
    uint32_t dir_idx;    /* FAT32: index in directory sector */
    uint32_t ref_count;
    vfs_ops_t* ops;
    void* private_data;
} vfs_node_t;

void init_vfs(void);
vfs_node_t* vfs_get_tty_node(void);
vfs_node_t* vfs_open(const char* path, uint32_t flags);
int vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
int vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer);
void vfs_close(vfs_node_t* node);
int vfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out_dirent);
vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name);

#endif

# sOS — Simple Operating System

<p align="center">
  <img src="https://img.shields.io/badge/version-2.6.0-blue.svg?style=flat-square" alt="Version">
  <img src="https://img.shields.io/badge/arch-i686%20%7C%20x86__32-orange.svg?style=flat-square" alt="Architecture">
  <img src="https://img.shields.io/badge/display-VBE%201024x768x32-green.svg?style=flat-square" alt="Display">
  <img src="https://img.shields.io/badge/filesystem-FAT32-lightgrey.svg?style=flat-square" alt="Filesystem">
  <img src="https://img.shields.io/badge/nic-Intel%20E1000-yellow.svg?style=flat-square" alt="Network">
</p>

<p align="center">
  A 32-bit monolithic x86 hobbyist operating system built from scratch.
  <br />
  Featuring preemptive multitasking, a custom VBE double-buffered graphics engine, FAT32 filesystem support with multi-user permissions, a complete networking stack with Intel E1000 hardware drivers, TCP/UDP sockets, cryptographic Over-The-Air (OTA) updates, and Ring 3 ELF binary execution.
</p>

---

## ⚡ Quick Start

### 1. Prerequisites

You will need a Linux environment (or WSL2) with standard cross-compilation and virtualization packages:

```bash
sudo apt update
sudo apt install build-essential make gcc-multilib grub-pc-bin grub-rescue-pc \
                 xorriso mtools dosfstools qemu-system-x86 qemu-utils
```

### 2. Build the System

A single `make` command compiles the kernel, assembles boot stubs, builds the user-space ELF application (`app.sos`), creates and formats a 64 MB FAT32 disk image, populates files, builds `sos.iso`, and exports a VirtualBox-ready virtual disk (`drive.vdi`):

```bash
make
```

To clean the build environment:
```bash
make clean
```

---

## 🚀 Running sOS

### Option A: Running with QEMU (Fastest)

Run the system in QEMU with full Intel E1000 NIC emulation and user-mode networking:

```bash
qemu-system-i386 -m 64M -boot d -cdrom sos.iso \
                 -drive file=drive.img,format=raw,index=0,media=disk \
                 -net nic,model=e1000 -net user
```

### Option B: Running in VirtualBox

VirtualBox offers native VBE acceleration, sound/timer stability, and hardware NIC emulation:

1. Create a new VM:
   - **Name:** `sOS`
   - **Type:** `Other` / **Version:** `Other/Unknown (32-bit)`
2. Configure Hardware:
   - **RAM:** Minimum `64 MB` (1 CPU core is sufficient).
3. Configure Storage:
   - Attach `sos.iso` as the Optical Drive (IDE Secondary Master).
   - Attach the generated `drive.vdi` as the Hard Disk (IDE Primary Master).
4. Configure Network:
   - **Adapter 1:** Enable Network Adapter.
   - **Attached to:** `NAT`.
   - **Adapter Type (Advanced):** `Intel PRO/1000 MT Desktop (82540EM)`.
5. Start the virtual machine!

---

## 🔑 Default Accounts

On boot, sOS presents a login prompt. The system image is pre-configured with the following user accounts:

| Username | Password | UID | Privilege Level | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **`root`** | `root` | `0` | Superuser / Administrator | Full system access (`/`) |
| **`user`** | `password` | `1` | Standard User | Home directory at `/home/user` |
| **`guest`** | *(None — hit Enter)* | `2` | Guest User | Home directory at `/home/guest` |

---

## ✨ System Features

### 🖥️ VBE 32-Bit Graphics Engine
* **Resolution & Color:** Runs in 1024x768 resolution with 32-bit ARGB true color via VESA BIOS Extensions (VBE).
* **Double Buffering:** 3 MB RAM backbuffer mapped at `0xE1000000` with high-speed memory copying to physical video memory (`0xE0000000`) for flicker-free terminal scrolling and drawing.
* **Rendering Engine:** Custom 8x16 bitmap font engine, ANSI color escape sequence parser (`\033[31m`–`\033[37m`), and an alpha-blended 32-bit sprite renderer (`draw_sprite`).
* **Graphical Kernel Panic:** Custom Blue Screen of Death (BSOD) displaying CPU registers (`EIP`, exception code, error code) upon Ring 0 kernel exceptions.

### 🌐 Network Stack & Hardware Driver
* **Intel E1000 Driver:** Direct Memory Mapped I/O (MMIO) and circular DMA descriptor ring buffers (RX/TX) for the Intel PRO/1000 MT Desktop (`82540EM`, `0x8086:0x100E`).
* **PCI Enumeration:** Scans PCI buses to auto-locate device registers and interrupt routing.
* **Protocol Suite:**
  * **Ethernet II** framing.
  * Dynamic **ARP** resolution and 32-entry ARP cache.
  * **IPv4** packet handling, checksum validation, and loopback/broadcast support.
  * **ICMP** automatic Echo Reply responder (`ping`).
  * **UDP** socket routing.
  * **TCP** state machine supporting connection handshake (`SYN`, `SYN-ACK`, `ACK`), in-order windowed data reception (`32 KB` window), and graceful disconnection (`FIN`).
* **Automatic Configuration:** Automated DHCP client on boot (allocates IP, subnet, gateway router, and DNS servers).
* **DNS Resolver:** Built-in A-record domain name resolver via UDP port 53 (with automatic fallback to `8.8.8.8`).

### 🔒 Cryptographic Over-The-Air (OTA) Updates
* Native in-kernel implementation of **SHA-256** and **HMAC-SHA256**.
* Modern command-based update model: fetches an update instructions file (`uinstruct` and `uinstruct.sig`) containing a list of update commands (e.g. `mkdir /bin/ ; wget [ip]/app.sos /bin/app.sos ; wget [ip]/kernel.bin /kernel.bin`).
* Automatic HMAC-SHA256 signing and verification for every downloaded file to prevent corruption and tampering.
* Run `sudo update <server:port>` to download instructions, verify signatures, execute filesystem modifications and payload downloads, create `/updt.flg`, and trigger an automated hardware reboot via the PS/2 controller (`0x64`).
* Fallback support for legacy single `kernel.bin`/`kernel.sig` updates.

### 📁 Virtual Filesystem (VFS) & Unix-Like Hierarchy
* **Modular VFS Layer:** Unified abstraction routing all file descriptors, standard I/O streams, pipelines, and disk storage through a common `vfs_node_t` interface.
* **Structured Hierarchy:**
  * `/home/[username]/`: Personal user home directories (`/home/user`, `/home/guest`, etc.) with ownership isolation (`0x30` permissions).
  * `/bin/`: User application binaries (e.g. `/bin/app.sos`) accessible globally from the shell.
  * `/dev/`: Virtual device filesystem (`devfs`) exposing character device nodes:
    * `/dev/null`: Data sink / discard stream.
    * `/dev/zero`: Infinite zero-byte generator.
    * `/dev/tty`: Interactive terminal I/O device.
    * `/dev/eth0`: Direct network interface node (read metrics, transmit raw Ethernet frames).
    * `/dev/urandom`: Pseudorandom number generator.
* **Full FAT32 Driver:** Reading/writing sectors via ATA PIO mode (`0x1F0`-`0x1F7`) with Unix permissions (`[UID|RWrw]`), path canonicalization, and wildcard (`*`) support.

### ⏱️ Preemptive Multitasking & Ring 3 User Space
* **Timer:** Programmable Interval Timer (PIT) at 100 Hz delivering 10 ms preemptive time slices.
* **Context Switching:** Hardware Task State Segment (TSS) kernel stack updates and automatic register saving via IRQ0 interrupt gates.
* **ELF32 Binary Loader:** Parses and maps standard 32-bit ELF executables, creating separate user page directories with supervisor/user page protection flags.
* **Dynamic Heap:** Ring 3 processes can dynamically grow their heap using `sbrk()` (Syscall 3).
* **Background Tasks:** Launch applications in the background using the `&` modifier (e.g., `run app.sos &`).

### 🛡️ Multi-User Security
* Salted password security using Pseudo-Random Number Generation (PRNG) and **FNV-1a** key-stretching (10,000 iterations).
* Passwords stored securely in `/passwd` as `salt$hash`.
* Userland memory pointer and buffer boundary validation across all system calls (`int 0x80`) to protect kernel memory space.

---

## 💻 Interactive Shell Reference

| Command | Usage | Description |
| :--- | :--- | :--- |
| **`help`** | `help` | Lists all available system commands. |
| **`ls`** | `ls` | Lists directory contents with permissions, owner UID, type, and size. |
| **`cd`** | `cd <path>` | Changes working directory (supports `.`, `..`, and paths). |
| **`cat`** | `cat <file>` | Displays file contents on screen. |
| **`mkdir`** | `mkdir <dir>` | Creates a new directory. |
| **`touch`** | `touch <file>` | Creates an empty file. |
| **`write`** | `write <file> <text>` | Writes a line of text to a file. |
| **`echo`** | `echo [-n] [text]` | Prints text to terminal or stream (supports redirection). |
| **`cp`** | `cp <src> <dest>` | Copies a file (destination can be a file or a folder). |
| **`mv`** | `mv <src> <dest>` | Moves or renames a file. |
| **`rm`** | `rm [-r] <file/dir>` | Removes files or directories (supports recursive `-r` and `*`). |
| **`df`** | `df [-h\|-k\|-m\|-i]` | Displays file system space usage in standardized Unix table format. |
| **`hex`** | `hex <file>` | Displays a 16-byte formatted hexadecimal and ASCII dump. |
| **`nano`** | `nano <file>` | Graphical full-screen text editor with scrolling and shortcuts (`^S`, `^X`). |
| **`run`** | `run <binary.sos> [&]` | Loads and runs a Ring 3 ELF binary (`&` executes in background). |
| **`ps`** | `ps [-u\|-v]` | Lists processes with PID, UID, type, state, and background mode. |
| **`kill`** | `kill <PID>` | Terminates a running process (must be owner or root). |
| **`ifconfig`**| `ifconfig` | Displays network status: MAC, IP, Subnet Mask, Gateway, and DNS. |
| **`wget`** | `wget <http://host:port/path> <out>`| Downloads a file over HTTP TCP port 80 (or custom port). |
| **`update`** | `sudo update <server:port> [inst_file]` | Modern OTA update: downloads instructions, verifies HMAC signatures, applies commands (fs/binaries), and reboots. |
| **`su`** | `su [username]` | Switches active user session. |
| **`sudo`** | `sudo <command>` | Executes a command with temporary Root privileges. |
| **`userlist`**| `userlist <list\|add\|remove>`| Lists users (shows salts/hashes to root), adds, or removes accounts. |
| **`passwd`** | `passwd [user]` | Updates user password using salted key stretching. |
| **`chown`** | `chown <file> <uid>` | Updates file owner UID (supports `*`). |
| **`chmod`** | `chmod <hex_perms> <file>` | Updates file permission flags (supports `*`). |
| **`whoami`** | `whoami [-u\|-n\|-v]` | Displays current active user, UID, or detailed role. |
| **`uptime`** | `uptime [-p\|-s\|-v]` | Displays formatted uptime duration, user, and task load. |
| **`ram`** / **`free`** | `ram [-h\|-m\|-k\|-v]` | Displays memory statistics (PMM and Kernel Heap) in Unix format. |
| **`uname`** | `uname [-a\|-s\|-r\|-m\|-o]` | Prints system architecture and OS release details. |
| **`clear`** | `clear` | Clears terminal screen and resets cursor. |
| **`lock`** | `lock` | Locks the current terminal session. |
| **`shutdown`**| `sudo shutdown [-r]` | Powers off (`shutdown`) or reboots (`shutdown -r`) the machine (Root only). |

> **Interactive Shell Tips:**
> - **Tab Autocompletion:** Press `Tab` to auto-complete commands and filesystem paths. Double-press `Tab` to list all matching candidates.
> - **Line Editing:** Use `Left` / `Right` arrows to edit within a line, and `Home` / `End` / `Delete`.
> - **History:** Use `Up` / `Down` arrows to navigate previous commands (5-entry FIFO buffer).
> - **Interrupts:** Press `Ctrl+C` to cancel a command line or terminate the currently running foreground user task.

---

## 🛠️ Technical Architecture

### 1. Memory Layout

sOS maps physical memory and user tasks according to the following layout:

```text
+-----------------------+ 0xFFFFFFFF
|  VBE Double Buffer    | 0xE1000000 (Dynamic pages mapped from PMM)
+-----------------------+ 0xE0000000
|  VBE Physical MMIO    | (Hardware Linear Framebuffer)
+-----------------------+
|                       |
+-----------------------+ 0x10000000 (256 MB)
|  Kernel Heap (kmalloc)| 4 MB Dynamic Kernel Heap
+-----------------------+
|  User Space Heap      | Dynamic via sbrk() (Up to 0x10000000 boundary)
|  User Code & Data     | 0x08000000 (ELF default base loading address)
+-----------------------+
|  User Stack (Ring 3)  | 0x07FF0000 - 0x07FF1000 (Top: 0x07FF1000)
+-----------------------+
|  Identity Mapping     | 0x00000000 - 0x04000000 (First 64 MB identity mapped)
|  - Kernel Image       | 0x00100000 (Loaded at 1 MB by GRUB)
|  - PMM Bitmap Buffer  | Tracks all 4 KB physical pages
+-----------------------+ 0x00000000
```

### 2. System Call Interface (`int 0x80`)

User-space applications communicate with the kernel by passing arguments through CPU registers:
- `EAX`: Syscall Number
- `EBX`: Argument 1
- `ECX`: Argument 2
- `EDX`: Argument 3

| EAX | Syscall | Arguments | Description |
| :---: | :--- | :--- | :--- |
| **1** | `write` | `(int fd, const void* buf, uint32_t count)` | Writes data to TTY or file descriptor. |
| **2** | `exit` | `(int code)` | Terminates current process and switches task. |
| **3** | `sbrk` | `(int increment)` | Increments process program break (heap expansion). |
| **4** | `read` | `(int fd, void* buf, uint32_t count)` | Reads data from standard input (keyboard). |
| **5** | `uptime` | *(none)* | Returns PIT timer ticks since boot. |
| **6** | `read_file` | `(const char* name, uint8_t* buf, uint32_t* size)` | Reads file from disk into user memory. |
| **7** | `write_file`| `(const char* name, const char* text)` | Writes text to a file. |
| **8** | `socket` | *(none)* | Allocates a UDP socket descriptor. |
| **9** | `bind` | `(int fd, uint16_t port)` | Binds socket to a local UDP port. |
| **10**| `sendto` | `(int fd, sendto_args_t* args)` | Sends a UDP packet to destination IP/port. |
| **11**| `recvfrom` | `(int fd, uint8_t* buf, uint32_t max_len)` | Reads an incoming UDP packet. |
| **12**| `get_dns_ip`| `(uint8_t* ip_buf)` | Retrieves system DNS IP provided by DHCP. |
| **13**| `socket_tcp`| *(none)* | Allocates a TCP socket descriptor. |
| **14**| `connect` | `(int fd, const uint8_t* ip, uint16_t port)` | Establishes a TCP connection (`SYN` handshake). |
| **15**| `send` | `(int fd, const void* buf, uint32_t len)` | Sends TCP data payload (`PSH+ACK`). |
| **16**| `recv` | `(int fd, void* buf, uint32_t max_len)` | Retrieves incoming TCP payload from socket buffer. |

---

## 📦 Writing User-Space Applications

Ring 3 applications are standard 32-bit ELF binaries linked against `app_linker.ld` with entry point `_start`. The user-space library `sos.h` / `sos.c` provides standard library wrappers:

```c
#include "sos.h"

void _start() {
    print("Hello from Ring 3 user mode!\n");

    /* Allocate dynamic memory on the user heap */
    char* buffer = (char*)malloc(128);
    strcpy(buffer, "Dynamic allocation via sbrk() works!\n");
    print(buffer);

    /* Open a TCP socket and download data */
    int sock = socket_tcp();
    unsigned char ip[4];
    if (gethostbyname("example.com", ip)) {
        if (connect(sock, ip, 80) == 0) {
            char* req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
            send(sock, req, strlen(req));
            
            int len = recv(sock, buffer, 127);
            if (len > 0) {
                buffer[len] = '\0';
                print(buffer);
            }
        }
    }

    exit(0);
}
```

Compile and run your application:
```bash
# Compilation (handled automatically by the Makefile for app.c -> app.sos)
make app.sos

# Run inside sOS:
run app.sos
```

---

## 📜 Credits & Acknowledgments

* **Inspiration:** Special thanks to the French YouTuber [**V2F**](https://www.youtube.com/@V2F). His video series on operating system development inspired the creation of this project, and the name "sOS" is borrowed directly from [his video](https://www.youtube.com/watch?v=ELTwwTsR5w8).
* **Development:** This project has been coded with extreme AI assistance, and does not reflect any personnal skill or capacities. It serves rather as an educational research project in order to explore operating system architecture, hardware driver implementation, and low-level networking.


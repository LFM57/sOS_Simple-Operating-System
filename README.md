# sOS (Simple Operating System)

sOS is a 32-bit (i686) hobbyist x86 Operating System built from scratch. It features preemptive multitasking, a VBE 32-bit linear framebuffer graphics engine, a custom FAT16 filesystem implementation with a multi-user permission model, a full network stack with Intel E1000 driver support, TCP/UDP sockets, DHCP auto-configuration, DNS resolution, HTTP downloads, cryptographic Over-The-Air (OTA) kernel updates, and Ring 3 user-space application execution (ELF binaries).

For an exhaustive deep-dive into the inner workings, memory model, network stack, and ABI specifications, please refer to [TECHNICAL_DETAILS.md](TECHNICAL_DETAILS.md).

---

## Key Features

* **32-Bit Graphics Engine:** 
  * Powered by VBE (VESA BIOS Extensions) at 1024x768 resolution with 32-bit ARGB color depth.
  * Backbuffer double-buffering for lag-free rendering and smooth terminal scrolling.
  * Custom 8x16 font engine with ANSI color escape code support (`\033[31m`, `\033[32m`, etc.).
  * Alpha-blended 32-bit sprite rendering engine.
  * Software cursor and custom Graphical Blue Screen of Death (BSOD) exception screen.

* **Full Network Stack & Driver:**
  * **PCI Bus Driver:** Scans hardware buses to auto-detect peripheral cards.
  * **Intel E1000 Driver:** Direct MMIO register access and Ring Descriptor DMA management for the Intel PRO/1000 MT Desktop NIC (`0x8086:0x100E`).
  * **Network Protocols:** Ethernet, dynamic ARP cache resolution, IPv4, ICMP (Echo reply responder), UDP, and TCP state machine.
  * **Automated Network Config:** Built-in DHCP client configured automatically during boot and DNS domain name resolution.
  * **HTTP Downloader:** Native `wget` utility supporting plain-text HTTP downloads over TCP port 80.

* **Cryptographic OTA System Updates:**
  * Native implementation of SHA-256 and HMAC-SHA256 signature verification algorithms.
  * Cryptographic Over-The-Air (`update`) command: Downloads new kernel binaries over HTTP, computes HMAC signatures, validates firmware integrity against remote `.sig` files, flashes the raw disk via ATA PIO, and reboots safely.

* **Multi-User Security & Hashing:**
  * Extended FAT16 permission model (`sec_byte` hack in directory entries storing owner UID and R/W permissions).
  * Salted password security using Pseudo-Random Number Generation (PRNG) and FNV-1a key-stretching (10,000 iterations). Stored securely as `salt$hash` in `/passwd`.
  * Comprehensive memory boundary checking on all Syscalls (`int 0x80`) to isolate Ring 0 kernel space from user applications.

* **Preemptive Multitasking & Ring 3 User Space:**
  * PIT-driven scheduler running at 100 Hz (10 ms time slices).
  * Task State Segment (TSS) stack switching for hardware interrupt protection.
  * ELF32 binary loader executing Ring 3 user applications with dynamic heap growth via `sbrk()`.
  * Foreground and background execution modes (using the `&` suffix).

* **Custom Shell & Utilities:**
  * Interactive line editing with Left/Right arrow navigation and Backspace bounds.
  * Command history navigation (Up/Down arrows with 5-entry FIFO memory).
  * Auto-completion via Tab and double-Tab listing for system commands AND filesystem paths.
  * Multi-file wildcard (`*`) support for management commands (`rm`, `chown`, `chmod`).
  * Built-in tools: `nano` (graphical text editor with viewport scrolling), `hex` (hexdump inspector), `ifconfig` (network interface monitor), `ps`, `kill`, `sudo`, `su`, `userlist`, `passwd`, and more.

---

## System Requirements

* **CPU:** 1-Core 32-bit (i686) or 64-bit (x86_64) processor.
* **RAM:** Minimum 50 MB required (allocates 3 MB dedicated to the 1024x768x32 VBE backbuffer).
* **Network NIC:** Intel E1000 (Intel PRO/1000 MT Desktop - `0x8086:0x100E`).
* **Disk Space:** 16 MB raw disk image.

---

## Prerequisites & Building

To compile sOS, you need a Linux environment (or WSL2) with the following tools installed:

```bash
sudo apt update
sudo apt install build-essential make gcc-multilib grub-pc-bin grub-rescue-pc xorriso mtools dosfstools qemu-system-x86
```

### Build Commands

* **Build Everything:** Run `make` to compile the kernel, assemble boot stubs, compile Ring 3 user binaries, format the 16 MB FAT16 disk image (`drive.img`), populate files, construct `sos.iso`, and convert to VirtualBox disk format (`drive.vdi`).
  ```bash
  make
  ```
* **Clean Build Tree:**
  ```bash
  make clean
  ```

---

## Running sOS

### 1. VirtualBox (Recommended)

VirtualBox offers full Intel E1000 emulation, VBE graphics, and NAT network connectivity out of the box.

1. Open VirtualBox and create a new Virtual Machine named **sOS**.
2. Set Type to **Other** / **Other/Unknown**.
3. Allocate **64 MB of RAM** and 1 CPU core.
4. Attach `sos.iso` as the Optical CD/DVD drive.
5. In **Storage**, attach the generated `drive.vdi` file as an IDE Hard Disk (Master Controller).
6. In **Network Settings**, enable Adapter 1, set to **NAT**, and ensure the adapter type is **Intel PRO/1000 MT Desktop (82540EM)**.
7. Start the Virtual Machine!

### 2. QEMU

You can run sOS directly using QEMU with network support enabled:

```bash
qemu-system-i386 -m 64M -cdrom sos.iso -drive file=drive.img,format=raw,index=0,media=disk -net nic,model=e1000 -net user
```

---

## Default Accounts

Upon booting, sOS will prompt you to log in. The Makefile pre-configures `/passwd` with the following credentials:

| Username | Password | UID | Privilege Level |
| :--- | :--- | :--- | :--- |
| **`root`** | `root` | `0` | Superuser / Administrator |
| **`user`** | `password` | `1` | Standard User |
| **`guest`** | *(Hit Enter)* | `2` | Guest User |

---

## Shell Command Reference

| Command | Usage | Description |
| :--- | :--- | :--- |
| **`help`** | `help` | Lists all available system commands. |
| **`ifconfig`** | `ifconfig` | Displays MAC address, IP address, Subnet Mask, Gateway, and DNS settings. |
| **`wget`** | `wget <http://host:port/path> <out.txt>` | Downloads a plain-text file over HTTP TCP port 80/custom port. |
| **`update`** | `sudo update <server_ip:port>` | Downloads, verifies (HMAC-SHA256), and flashes an OTA kernel update. |
| **`nano`** | `nano <filename>` | Graphical text editor with viewport scrolling, keyboard shortcuts (`^S`, `^X`). |
| **`hex`** | `hex <filename>` | Displays a 16-byte hexadecimal and ASCII dump of a file. |
| **`run`** | `run <binary.sos> [&]` | Loads and executes a Ring 3 ELF executable (`&` runs in background). |
| **`ps`** | `ps` | Displays active tasks, PID, type (Shell/SysTask/UserApp), and state. |
| **`kill`** | `kill <PID>` | Terminates a running process and releases its memory/sockets. |
| **`ls`** | `ls` | Lists directory contents with permissions, owner UID, type, and size. |
| **`cd`** | `cd <path>` | Changes current directory (supports `.`, `..`, and nested paths). |
| **`mkdir`** | `mkdir <dir>` | Creates a new directory. |
| **`rm`** | `rm [-r] <file/dir>` | Deletes files/directories (supports `-r` recursive and `*` wildcard). |
| **`touch`** | `touch <filename>` | Creates an empty file. |
| **`cat`** | `cat <filename>` | Outputs file contents to the screen. |
| **`cp`** / **`mv`** | `cp <src> <dest>` / `mv <src> <dest>` | Copies or moves files/directories across the filesystem. |
| **`df`** | `df` | Displays free disk space in KB and MB. |
| **`userlist`** | `userlist <list/add/remove>` | Manages system accounts and views salted password hashes (Root only). |
| **`passwd`** | `passwd [username]` | Updates the target user's password using salted key-stretching. |
| **`su`** / **`sudo`** | `su <user>` / `sudo <cmd>` | Switches active user context or runs a single command as Root. |
| **`chmod`** | `chmod <hex_perms> <file>` | Changes file access permissions (supports `*` wildcard). |
| **`chown`** | `chown <file> <uid>` | Changes file owner UID (supports `*` wildcard). |
| **`whoami`** | `whoami` | Prints current user identity and active UID. |
| **`uptime`** | `uptime` | Displays system uptime in seconds. |
| **`ram`** | `ram` | Prints total detected RAM in MB. |
| **`clear`** | `clear` | Clears the terminal screen and resets cursor position. |
| **`lock`** | `lock` | Locks the terminal session and prompts for authentication. |
| **`shutdown`** | `shutdown` | Prompts for confirmation and powers off the machine (ACPI/QEMU/VBox). |

---

## Disclaimer & Credits

**Disclaimer:** sOS was developed as a hobbyist research and learning operating system with support from Artificial Intelligence models (Gemini 3.1 Pro Preview and Gemini 3.5 Flash). It serves as an exploration into x86 kernel architecture, networking, graphics, and operating system design.

**Credits:** Special thanks to the French YouTuber [**V2F**](https://www.youtube.com/@V2F). His video on OS development inspired this project, and the name "sOS" was borrowed directly from [his video](https://www.youtube.com/watch?v=ELTwwTsR5w8).
# sOS: Future Improvements & Roadmap

While sOS is a functional hobby operating system, it is currently limited in scope. Below is a list of features, refactors, and bug fixes that could be implemented in future iterations.

## 🚀 Upcoming Features
1. **Virtual File System (VFS):** 
   Abstract the filesystem layer. Currently, the OS is hardcoded to use FAT16. Implementing a VFS would allow sOS to support Ext2, ISO9660 (for CD-ROMs), and DevFS (`/dev/tty`, `/dev/null`).
2. **Proper Inter-Process Communication (IPC):**
   Add Pipes (`|`) and UNIX Domain Sockets. Currently, tasks cannot easily communicate with each other aside from reading/writing to the same FAT16 files.
3. **Dynamic Memory Allocation in User Space (`malloc` / `free`):**
   While `sbrk` is implemented as a syscall, the user-space `sos.c` library only implements a rudimentary `malloc` that never frees memory. A proper user-space allocator (like a simplified dlmalloc) is needed.
4. **Graphical User Interface (GUI):**
   Transition from the standard 80x25 VGA text mode to VESA BIOS Extensions (VBE) for a frame buffer, allowing for a window compositor and mouse support (PS/2 Mouse driver).
5. **Networking Stack:**
   Implement an RTL8139 or E1000 PCI network card polling driver, followed by a basic IPv4/UDP/TCP stack and sockets.

## 🛠️ Refactoring & Optimization
1. **Interrupt-Driven ATA Driver:**
   The current hard disk driver uses Polling (spinning CPU cycles waiting for the `DRQ` bit). This halts the whole system during disk I/O. It should be refactored to use DMA (Direct Memory Access) and hardware IRQ 14/15, allowing the CPU to execute other tasks while waiting for disk operations to complete.
2. **Standardizing the Standard C Library (libc):**
   The `sos.h` and `sos.c` headers provide custom function signatures that slightly diverge from POSIX standards. Standardizing these would make porting third-party apps (like generic C programs) to sOS much easier.
3. **Environment Variables:**
   Implement environment variables (like `$PATH`, `$USER`) in the shell and pass them to applications via the ELF execution frame (`execve`).

## 🐛 Known Bugs to Fix
1. **FPU Context Saving:**
   The PIT scheduler does not save the FPU (Floating Point Unit / x87) or SSE registers during a context switch. If two programs perform floating-point math simultaneously, they will corrupt each other's calculations. The `FXSAVE` and `FXRSTOR` instructions need to be integrated into `irq0`.
2. **Shell Line Overflows:**
   If the user abuses the arrow keys combined with long paths and history scrolling, the VGA rendering buffer logic inside the shell (`replace_cmd_buffer`) can glitch visually across multiple lines.
3. **FAT16 Fragmentation:**
   The `fs_rm` (Remove) function unlinks FAT clusters properly, but heavy continuous creation and deletion of files might result in fragmentation. No disk defragmentation logic currently exists.
4. **Orphaned Memory on Crash:**
   If an application crashes violently before `cleanup_task_memory` properly traverses its complex page table modifications, some physical pages may remain permanently marked as "Used" in the PMM Bitmap
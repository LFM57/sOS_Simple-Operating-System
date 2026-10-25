### Roadmap

#### Step 1: Replace Busy-Waiting with Sleep/Wait Queues
Right now, whenever a task must wait (for keyboard input, a pipe with no data, a full pipe, or a TCP socket), it either loops actively or calls `hlt` inside a spinlock.

Implement **Wait Queues**:
```c
typedef struct wait_node {
    task_t* task;
    struct wait_node* next;
} wait_node_t;

typedef struct {
    wait_node_t* head;
} wait_queue_t;

void sleep_on(wait_queue_t* queue);
void wake_up(wait_queue_t* queue);
```
* Change a task's state from `2` (READY) to `4` (BLOCKED).
* When a pipe receives data in `pipe_vfs_write`, call `wake_up(&pipe->read_queue)`.
* When keyboard input arrives in `irq1`, wake up the console queue.
* The scheduler skips blocked tasks completely, dropping idle CPU usage in the VM to near zero.

---

#### Step 2: Implement `fork()` and `execve()` (True Concurrent Pipelines)
To make the shell and pipeline engine work properly:

1. **`fork()`**:
   * Allocate a new `task_t`.
   * Clone the parent's page directory (for now, perform a deep copy of user-space pages from `0x08000000` to `prog_break` and the user stack).
   * Clone the file descriptor table (incrementing `vfs_node->ref_count`).
   * Set up child registers so the child returns `0` and the parent returns the child's `PID`.
2. **`execve(const char* path, char* const argv[], char* const envp[])`**:
   * Release the existing address space.
   * Parse and load the new ELF binary into the current task.
   * Reset the program break (`prog_break`) and user stack pointer.
3. **Refactor Pipelines in the Shell**:
   Now, when the shell sees `cat file.txt | nano`:
   * The shell creates a pipe.
   * Forks child 1: redirects stdout to `pipefd[1]`, closes `pipefd[0]`, calls `execve("cat")`.
   * Forks child 2: redirects stdin to `pipefd[0]`, closes `pipefd[1]`, calls `execve("nano")`.
   * Parent closes both pipe ends and calls `waitpid()` on both children.
   Both processes run concurrently, and the 4 KB pipe buffer acts as a stream buffer rather than an arbitrary file size limit.

---

#### Step 3: Move the Shell into Ring 3 (`/bin/sh.sos`)
Currently, the shell (`execute_command`, `current_user`, etc.) is embedded directly inside **Kernel Task 0**. If the shell faults, the whole system halts.

* Strip `kernel.c` down to strictly kernel initialization, driver management, interrupts, and the scheduler loop.
* Move command-line parsing, built-in commands (`ls`, `cd`, `cat`), and terminal prompts into a standalone ELF binary compiled with `app_linker.ld` (`sh.sos`).
* When the kernel finishes booting, it simply launches PID 1 (`/bin/sh.sos` or similar) and drops into the idle scheduler loop.

---

#### Step 4: Implement TCP Server Capabilities (`listen()` & `accept()`)
You already have a working TCP client (`connect`, `send`, `recv`). Completing the server side of the TCP state machine requires:

1. **Socket States**: Expand `net.h` to include `TCP_LISTEN` and `TCP_SYN_RCVD`.
2. **Connection Backlog**: When a socket is placed in `listen()` mode:
   * When an incoming `SYN` packet arrives on that port in `net_handle_packet`, allocate a child socket in `TCP_SYN_RCVD` state and send a `SYN-ACK`.
   * When the remote client responds with `ACK`, transition the child socket to `TCP_ESTABLISHED` and push it to the listening socket's backlog queue.
3. **Syscall `accept(int fd)`**:
   * Dequeue an established child socket and assign it a new file descriptor in the task table.

Once implemented, you can write a tiny **Ring 3 HTTP Web Server** running inside sOS that serves static files from the FAT32 disk to the host computer via QEMU port forwarding:
```bash
# QEMU port forwarding: host port 8080 -> guest sOS port 80
-net nic,model=e1000 -net user,hostfwd=tcp::8080-:80
```

---

#### Step 5: Migrate to a Higher-Half Kernel (3 GB / 1 GB Split)
Currently, the kernel is mapped at virtual address `0x00100000` (1 MB) and user space is awkwardly shoehorned above it starting at `0x08000000`.

* Set up a **higher-half kernel** where the kernel lives at `0xC0000000` (the 3 GB mark).
* User space gets the entire lower virtual address space from `0x00000000` to `0xBFFFFFFF` (3 GB of isolated virtual memory per process).
* This removes the 64 MB physical limit, allows you to manage multiple gigabytes of physical RAM via PMM, and aligns the memory layout with standard Linux/x86 ABIs.
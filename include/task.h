#ifndef TASK_H
#define TASK_H

// Tiny cooperative-then-preemptive round-robin scheduler.
//
// All tasks run in ring 0 and share the kernel page directory. A task is
// really just "a kernel stack + a saved esp". Switching is: push callee-
// saved regs, swap esp, pop callee-saved regs, ret. That's the whole trick.
//
// Lifecycle:
//   tasking_init()   once, from kmain, after the heap is up.
//   task_spawn(fn)   create a new runnable task. fn runs until it returns
//                    or calls task_exit().
//   yield()          voluntarily give up the CPU (cooperative).
//   Once tasking_enable_preemption() is called, the PIT IRQ will call
//   schedule() automatically on every tick.

typedef void (*task_entry_fn)(void);

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,   // waiting on a child (sys_wait); not schedulable until woken
    TASK_DEAD,
} task_state_t;

typedef struct task {
    unsigned int  esp;          // saved kernel stack pointer (offset 0 — asm relies on this)
    unsigned int  stack_base;   // start of the kernel stack alloc, for kfree
    unsigned int  stack_size;   // in bytes
    unsigned int  kstack_top;   // kernel stack top (used as tss.esp0 when user task runs)
    unsigned int  user_eip;     // initial ring-3 EIP (only meaningful if is_user)
    unsigned int  user_esp;     // initial ring-3 ESP (only meaningful if is_user)
    unsigned int  resume_frame; // for forked children: addr of the saved
                                // register frame to iret into on first run
                                // (0 for fresh tasks that use user_eip/esp)
    unsigned int  pd_phys;      // physical addr of this task's page directory;
                                // 0 means use the kernel PD (kernel tasks only)
    int           id;
    task_state_t  state;
    int           is_user;      // 1 if this task runs in ring 3
    char          name[16];

    // --- process hierarchy + exit status (sys_fork / sys_wait) --------
    // parent      : task that forked/spawned us, or NULL. A parent
    //               blocked in sys_wait is woken when one of its
    //               children exits.
    // exit_code   : value passed to sys_exit, valid once has_exited==1.
    // has_exited  : set by task_exit before the task becomes a zombie,
    //               so a parent calling wait *after* the child died can
    //               still collect the status (we stash it on the PCB and
    //               defer reaping until the parent has read it).
    // wait_any    : set while this task is blocked in sys_wait, meaning
    //               "wake me when any child exits".
    // reaped_by_parent : 1 once a parent has collected this zombie's
    //               status (or the task had no parent); the reaper only
    //               frees a zombie once this is set, so status survives
    //               until wait() reads it.
    struct task*  parent;
    int           exit_code;
    int           has_exited;
    int           wait_any;
    int           reaped_by_parent;

    // Optional cleanup hook. Called by the reaper just before the task
    // struct itself is freed. Useful for loaders that need to release
    // user-space pages, file handles, etc. May be NULL.
    void        (*on_exit)(struct task*);
    void*         user_data;    // opaque pointer for on_exit's use

    // --- user-pointer access fault recovery (uaccess.c) ---------------
    // Set non-zero by copy_from_user / copy_to_user / strnlen_user
    // around the inner copy loop. While set, a page fault triggered
    // from the kernel (CPL=0) on a faulting address is *not* fatal:
    // the page-fault handler rewrites the saved EIP to fault_recovery_eip
    // so the helper bails out and returns -EFAULT. Cleared on the way
    // out of every helper. Zero-initialised by memset() in task_spawn.
    unsigned int  in_user_access;
    unsigned int  fault_recovery_eip;
    int           uaccess_faulted;   // set to 1 by page fault handler

    struct task*  next;         // circular ready list
} task_t;

void tasking_init(void);
int  task_spawn(task_entry_fn fn, const char* name);

// Spawn a ring-3 task.  The caller is responsible for:
//   - creating a user page directory via vmm_create_user_pd()
//   - mapping user code and stack into that PD with VMM_USER|VMM_PRESENT
//   - passing the PD's physical address as pd_phys
// Both user_eip and user_esp must be < 0xC0000000.
int  task_spawn_user(unsigned int user_eip, unsigned int user_esp,
                     unsigned int pd_phys, const char* name);

// --- fork support -----------------------------------------------------
// Create a child ring-3 task that, on its first scheduling, resumes in
// ring 3 from a *complete* saved register frame rather than from a fresh
// entry point. `frame` is a 16-field snapshot (struct registers plus the
// cross-ring useresp/ss the CPU pushed) captured at the int 0x80 that
// invoked fork; the caller will have set frame->eax = 0 so the child
// sees 0 as fork's return value. `pd_phys` is the child's freshly built
// address space. The child shares the parent's name with a tag.
//
// Returns the new task id, or -1 on allocation failure. The caller
// (loader_fork) is responsible for populating pd_phys with a copy of the
// parent's user pages and attaching user_data / on_exit / parent.
struct registers;
int  task_clone_user(struct registers* frame, unsigned int pd_phys,
                     const char* name);

void yield(void);
void task_exit(void);

// --- wait() support ---------------------------------------------------
// Block the calling task until one of its children exits, then collect
// that child: write its exit code through *status (if non-NULL, kernel
// pointer), mark the zombie reapable, and return the child's pid.
//
// Returns -1 immediately if the caller has no children at all (nothing
// to wait for). Otherwise it sleeps (TASK_BLOCKED) and is woken by
// task_exit_code when a child dies. Designed to be called from the
// SYS_WAIT handler in syscall.c.
int  task_wait_child(int* status);
// Exit with an explicit status code (used by SYS_EXIT). task_exit() is
// equivalent to task_exit_code(0).
void task_exit_code(int code);
void tasking_enable_preemption(void);

// Called from the PIT IRQ. No-op until preemption is enabled.
void scheduler_tick(void);

// Debug / shell support.
task_t* task_current(void);
task_t* task_find_by_id(int id);
void    task_list_print(void);

#endif
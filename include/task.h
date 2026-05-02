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
    TASK_DEAD,
} task_state_t;

typedef struct task {
    unsigned int  esp;          // saved kernel stack pointer (offset 0 — asm relies on this)
    unsigned int  stack_base;   // start of the kernel stack alloc, for kfree
    unsigned int  stack_size;   // in bytes
    unsigned int  kstack_top;   // kernel stack top (used as tss.esp0 when user task runs)
    unsigned int  user_eip;     // initial ring-3 EIP (only meaningful if is_user)
    unsigned int  user_esp;     // initial ring-3 ESP (only meaningful if is_user)
    unsigned int  pd_phys;      // physical addr of this task's page directory;
                                // 0 means use the kernel PD (kernel tasks only)
    int           id;
    task_state_t  state;
    int           is_user;      // 1 if this task runs in ring 3
    char          name[16];

    // Optional cleanup hook. Called by the reaper just before the task
    // struct itself is freed. Useful for loaders that need to release
    // user-space pages, file handles, etc. May be NULL.
    void        (*on_exit)(struct task*);
    void*         user_data;    // opaque pointer for on_exit's use

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

void yield(void);
void task_exit(void);
void tasking_enable_preemption(void);

// Called from the PIT IRQ. No-op until preemption is enabled.
void scheduler_tick(void);

// Debug / shell support.
task_t* task_current(void);
task_t* task_find_by_id(int id);
void    task_list_print(void);

#endif
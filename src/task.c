#include "task.h"
#include "kheap.h"
#include "printf.h"
#include "string.h"
#include "gdt.h"
#include "vmm.h"
#include "isr.h"

// Defined in task_switch.asm.
extern void task_switch(unsigned int* old_esp_ptr, unsigned int new_esp);
extern void task_trampoline(void);
extern void enter_user_mode(unsigned int user_eip, unsigned int user_esp);
extern void enter_user_resume(struct registers* frame);

#define STACK_SIZE 8192   // 8 KB per task — plenty for our kernel threads

// Switch to the page directory appropriate for `t`.
// User tasks carry their own PD; kernel tasks run under the global kernel PD.
static inline void switch_pd(task_t* t) {
    unsigned int target = t->pd_phys ? t->pd_phys : vmm_kernel_pd_phys();
    vmm_switch_pd(target);
}

static task_t* current  = 0;   // head of "what's running right now"
static task_t* ready    = 0;   // circular list of all tasks (including current)
static int     next_id  = 0;
static int     preempt_enabled = 0;

// Reaped tasks are freed lazily by the scheduler so we never free our
// own stack while we're still standing on it.
static task_t* zombie = 0;

// --------------------------------------------------------------------
// Build the initial stack for a new task so that the very first
// task_switch into it lands cleanly inside task_trampoline, which
// then calls the entry function.
//
// Stack layout at rest (top = low addr, esp points at saved edi):
//
//     [entry_fn]           <- popped by trampoline's first `pop eax`
//     [task_trampoline]    <- popped by final "ret" in task_switch
//     [ebp = 0]
//     [ebx = 0]
//     [esi = 0]
//     [edi = 0]            <- esp points here
//
// task_switch restores edi/esi/ebx/ebp then rets → jumps to trampoline.
// Trampoline pops entry_fn into eax and calls it.
// --------------------------------------------------------------------
static unsigned int task_init_stack(unsigned int stack_top, task_entry_fn fn) {
    unsigned int* sp = (unsigned int*)stack_top;

    *--sp = (unsigned int)fn;                  // entry_fn
    *--sp = (unsigned int)task_trampoline;     // ret target of task_switch
    *--sp = 0;                                 // ebp
    *--sp = 0;                                 // ebx
    *--sp = 0;                                 // esi
    *--sp = 0;                                 // edi  <- esp lands here

    return (unsigned int)sp;
}

// --------------------------------------------------------------------
// List helpers — ready is a circular singly-linked list, current is
// always a member of it unless current is the very first task created.
// --------------------------------------------------------------------
static void ready_insert(task_t* t) {
    if (!ready) {
        ready = t;
        t->next = t;           // self-loop
        return;
    }
    // Insert right after `ready` (O(1), doesn't matter where).
    t->next = ready->next;
    ready->next = t;
}

static void ready_remove(task_t* t) {
    if (!ready || !t) return;

    // Find predecessor.
    task_t* p = ready;
    while (p->next != t) {
        p = p->next;
        if (p == ready) return;   // not in list
    }
    if (p == t) {
        // t was the only node.
        ready = 0;
    } else {
        p->next = t->next;
        if (ready == t) ready = t->next;
    }
    t->next = 0;
}

// Small strncpy since the kernel's string.h doesn't export one.
static void name_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// --------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------
void tasking_init(void) {
    // The currently-executing context (kmain) becomes task 0. We don't
    // need to build a stack for it — it already has one; task_switch
    // will save its esp into task0->esp on the first switch out.
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) { printf("tasking_init: out of memory\n"); return; }
    memset(t, 0, sizeof(*t));

    t->esp        = 0;          // filled in by first task_switch out
    t->stack_base = 0;          // we didn't allocate this stack
    t->stack_size = 0;
    t->kstack_top = 0;          // kmain runs in ring 0; never used as esp0
    t->id         = next_id++;
    t->state      = TASK_RUNNING;
    t->is_user    = 0;
    name_copy(t->name, "kmain", sizeof(t->name));
    t->next       = 0;

    ready_insert(t);
    current = t;
}

int task_spawn(task_entry_fn fn, const char* name) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return -1;

    unsigned int base = (unsigned int)kmalloc(STACK_SIZE);
    if (!base) { kfree(t); return -1; }

    memset(t, 0, sizeof(*t));
    t->stack_base = base;
    t->stack_size = STACK_SIZE;
    t->kstack_top = base + STACK_SIZE;
    t->esp        = task_init_stack(base + STACK_SIZE, fn);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->is_user    = 0;
    name_copy(t->name, name ? name : "task", sizeof(t->name));

    // Atomic wrt the timer IRQ: mask interrupts while we touch the list.
    __asm__ volatile ("cli");
    ready_insert(t);
    __asm__ volatile ("sti");

    return t->id;
}

// --------------------------------------------------------------------
// User-task spawn.
//
// Build a kernel-stack frame whose first run lands in task_trampoline,
// which then calls our shim `user_entry_shim`. The shim reads user_eip
// and user_esp off the task struct and calls enter_user_mode, which
// builds the iret frame and dives to ring 3.
//
// Memory mapping (the actual page-table population) is the caller's
// job. A typical caller will:
//   1. allocate a frame, vmm_map(eip_virt, frame, USER|WRITE|PRESENT)
//   2. copy code into eip_virt
//   3. allocate a stack frame, vmm_map(stack_virt, ..., USER|WRITE)
//   4. call task_spawn_user(eip_virt, stack_virt + 4096, "name")
// --------------------------------------------------------------------

static void user_entry_shim(void);

int task_spawn_user(unsigned int user_eip, unsigned int user_esp,
                    unsigned int pd_phys, const char* name) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return -1;

    unsigned int base = (unsigned int)kmalloc(STACK_SIZE);
    if (!base) { kfree(t); return -1; }

    memset(t, 0, sizeof(*t));
    t->stack_base = base;
    t->stack_size = STACK_SIZE;
    t->kstack_top = base + STACK_SIZE;
    t->esp        = task_init_stack(base + STACK_SIZE, user_entry_shim);
    t->user_eip   = user_eip;
    t->user_esp   = user_esp;
    t->pd_phys    = pd_phys;
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->is_user    = 1;
    name_copy(t->name, name ? name : "user", sizeof(t->name));

    __asm__ volatile ("cli");
    ready_insert(t);
    __asm__ volatile ("sti");

    return t->id;
}

// First-run shim for ring-3 tasks. Reached via task_trampoline, which
// got us here with interrupts already enabled. We grab user_eip and
// user_esp off the task struct and dive into ring 3 — enter_user_mode
// doesn't return.
static void user_entry_shim(void) {
    task_t* t = current;
    if (!t) {
        printf("[task] user shim: no current task\n");
        for (;;) __asm__ volatile ("hlt");
    }

    // The TSS update for user tasks normally happens in the scheduler
    // when switching INTO the task; for the very first run, ensure the
    // CPU has a valid esp0 to use when the user task takes its first
    // interrupt or syscall.
    tss_set_kernel_stack(t->kstack_top);

    enter_user_mode(t->user_eip, t->user_esp);

    // Never reached.
    for (;;) __asm__ volatile ("hlt");
}

// --------------------------------------------------------------------
// Fork support: a child created by task_clone_user starts here on its
// first scheduling. Unlike user_entry_shim (which dives to a fresh
// entry point), this resumes from the complete register frame captured
// at the parent's int 0x80, so the child re-emerges from fork() exactly
// where the parent did — but with eax already set to 0 in its frame.
// --------------------------------------------------------------------
static void fork_resume_shim(void) {
    task_t* t = current;
    if (!t || !t->resume_frame) {
        printf("[task] fork shim: no resume frame\n");
        for (;;) __asm__ volatile ("hlt");
    }
    tss_set_kernel_stack(t->kstack_top);
    enter_user_resume((struct registers*)t->resume_frame);
    for (;;) __asm__ volatile ("hlt");   // never reached
}

// Number of 32-bit words in the cross-ring register frame we snapshot
// for fork: struct registers (14 words) + useresp + ss = 16 words.
#define FORK_FRAME_WORDS 16

int task_clone_user(struct registers* frame, unsigned int pd_phys,
                    const char* name) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return -1;

    unsigned int base = (unsigned int)kmalloc(STACK_SIZE);
    if (!base) { kfree(t); return -1; }

    memset(t, 0, sizeof(*t));
    t->stack_base = base;
    t->stack_size = STACK_SIZE;
    t->kstack_top = base + STACK_SIZE;
    t->pd_phys    = pd_phys;
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->is_user    = 1;
    name_copy(t->name, name ? name : "child", sizeof(t->name));

    // Lay the saved register frame on the very top of the child's kernel
    // stack so it lives as long as the task does, then build the normal
    // task_switch bootstrap frame *below* it. fork_resume_shim reads the
    // frame via t->resume_frame.
    unsigned int sp = base + STACK_SIZE;
    sp -= FORK_FRAME_WORDS * 4;
    unsigned int* fcopy = (unsigned int*)sp;
    for (int i = 0; i < FORK_FRAME_WORDS; i++)
        fcopy[i] = ((unsigned int*)frame)[i];
    t->resume_frame = (unsigned int)fcopy;

    // Bootstrap frame so the first task_switch lands in task_trampoline,
    // which calls fork_resume_shim.
    t->esp = task_init_stack(sp, fork_resume_shim);

    __asm__ volatile ("cli");
    ready_insert(t);
    __asm__ volatile ("sti");

    return t->id;
}

// Pick the next runnable task after `from`. Skips DEAD entries.
static task_t* pick_next(task_t* from) {
    task_t* p = from->next;
    while (p != from) {
        if (p->state == TASK_READY || p->state == TASK_RUNNING) return p;
        p = p->next;
    }
    return from;   // nobody else runnable — stay put
}

static void reap_zombies(void) {
    // Walk the zombie list, freeing only those whose status has been
    // collected (reaped_by_parent). A zombie whose parent may still call
    // wait() is left in place so its exit_code survives until read.
    task_t* prev = 0;
    task_t* z = zombie;
    while (z) {
        task_t* next = z->next;
        if (z->reaped_by_parent) {
            // Unlink z from the zombie list.
            if (prev) prev->next = next;
            else      zombie = next;

            if (z->on_exit) z->on_exit(z);
            if (z->stack_base) kfree((void*)z->stack_base);
            kfree(z);
            // prev stays the same; z is gone.
        } else {
            prev = z;
        }
        z = next;
    }
}

void yield(void) {
    __asm__ volatile ("cli");

    reap_zombies();

    if (!current || !ready) {
        __asm__ volatile ("sti");
        return;
    }

    task_t* next = pick_next(current);
    if (next == current) {
        __asm__ volatile ("sti");
        return;
    }

    task_t* prev = current;
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state  = TASK_RUNNING;
    current      = next;

    if (next->is_user) tss_set_kernel_stack(next->kstack_top);
    switch_pd(next);

    task_switch(&prev->esp, next->esp);

    __asm__ volatile ("sti");
}

void task_exit(void) {
    task_exit_code(0);
}

void task_exit_code(int code) {
    __asm__ volatile ("cli");

    current->exit_code = code;
    current->has_exited = 1;
    current->state = TASK_DEAD;
    ready_remove(current);

    // If a parent is blocked waiting on us, make it runnable again so it
    // can collect our status. The actual status hand-off happens in
    // sys_wait when the parent runs; here we just unblock it.
    task_t* p = current->parent;
    if (p && p->state == TASK_BLOCKED && p->wait_any) {
        p->state = TASK_READY;
    }

    // If we have no parent that will ever wait on us (kernel threads,
    // orphans), mark ourselves collectable so the reaper frees us. A
    // task whose parent may still wait stays un-reaped until wait()
    // reads its exit_code and sets reaped_by_parent.
    if (!p) current->reaped_by_parent = 1;

    // Push onto zombie list; the reaper frees us once reaped_by_parent.
    current->next = zombie;
    zombie = current;

    if (!ready) {
        __asm__ volatile ("sti");
        for (;;) __asm__ volatile ("hlt");
    }

    task_t* next = ready;
    while (next->state == TASK_DEAD || next->state == TASK_BLOCKED) {
        next = next->next;
        if (next == ready) {
            // Everyone left is blocked/dead. Nothing to run — idle until
            // an interrupt (e.g. keyboard) wakes someone. This shouldn't
            // normally happen because the shell is always runnable.
            __asm__ volatile ("sti");
            for (;;) __asm__ volatile ("hlt");
        }
    }
    next->state = TASK_RUNNING;

    current = next;

    if (next->is_user) tss_set_kernel_stack(next->kstack_top);
    switch_pd(next);

    unsigned int dummy;
    task_switch(&dummy, next->esp);

    for (;;) __asm__ volatile ("hlt");   // unreachable
}

void tasking_enable_preemption(void) {
    preempt_enabled = 1;
}

void scheduler_tick(void) {
    if (!preempt_enabled) return;
    if (!current || !ready) return;

    task_t* next = pick_next(current);
    if (next == current) return;

    task_t* prev = current;
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state  = TASK_RUNNING;
    current      = next;

    if (next->is_user) tss_set_kernel_stack(next->kstack_top);
    switch_pd(next);

    task_switch(&prev->esp, next->esp);
}

task_t* task_current(void) { return current; }

// --------------------------------------------------------------------
// wait(): block until a child of `current` exits, collect it, return pid.
//
// A child that exits is moved to the zombie list by task_exit_code (and
// removed from `ready`), with has_exited=1 and reaped_by_parent=0 while a
// parent might still wait. So wait() must scan BOTH lists for our
// children: a zombie that already died (collect immediately) or a live
// child (sleep until it dies and wakes us).
// --------------------------------------------------------------------

// Does `current` have any child anywhere (ready list or zombie list)?
static int has_any_child(void) {
    if (ready) {
        task_t* p = ready;
        do {
            if (p->parent == current && p != current) return 1;
            p = p->next;
        } while (p != ready);
    }
    for (task_t* z = zombie; z; z = z->next)
        if (z->parent == current) return 1;
    return 0;
}

// Find an already-exited, not-yet-collected child on the zombie list.
static task_t* find_dead_child(void) {
    for (task_t* z = zombie; z; z = z->next)
        if (z->parent == current && z->has_exited && !z->reaped_by_parent)
            return z;
    return 0;
}

int task_wait_child(int* status) {
    for (;;) {
        __asm__ volatile ("cli");

        // Already-dead child waiting to be collected?
        task_t* z = find_dead_child();
        if (z) {
            int pid = z->id;
            if (status) *status = z->exit_code;
            z->reaped_by_parent = 1;   // let the reaper free it
            __asm__ volatile ("sti");
            return pid;
        }

        // No dead child yet. If we have no children at all, there's
        // nothing to wait for — return -1 rather than block forever.
        if (!has_any_child()) {
            __asm__ volatile ("sti");
            return -1;
        }

        // Block until a child exit wakes us. task_exit_code flips our
        // state back to READY when one of our children dies.
        current->wait_any = 1;
        current->state    = TASK_BLOCKED;
        __asm__ volatile ("sti");

        // Give up the CPU. When we're scheduled again, loop and re-check.
        yield();

        current->wait_any = 0;
        // loop back around to collect
    }
}

// Look up a task by id. Returns NULL if no such task exists or if the
// task has been reaped. Used by the shell to wait on a spawned task.
// O(N) walk — fine at our scale.
task_t* task_find_by_id(int id) {
    if (!ready) return 0;
    task_t* p = ready;
    do {
        if (p->id == id) return p;
        p = p->next;
    } while (p != ready);
    return 0;
}

void task_list_print(void) {
    if (!ready) { printf("(no tasks)\n"); return; }
    printf("  id  state    name\n");
    task_t* p = ready;
    do {
        const char* s = (p->state == TASK_RUNNING) ? "RUN " :
                        (p->state == TASK_READY)   ? "ready" :
                        (p->state == TASK_BLOCKED) ? "block" : "dead ";
        printf("  %d   %s   %s%s\n",
               p->id, s, p->name, (p == current) ? "  *" : "");
        p = p->next;
    } while (p != ready);
}
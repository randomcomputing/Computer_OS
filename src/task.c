#include "task.h"
#include "kheap.h"
#include "printf.h"
#include "string.h"
#include "gdt.h"
#include "vmm.h"

// Defined in task_switch.asm.
extern void task_switch(unsigned int* old_esp_ptr, unsigned int new_esp);
extern void task_trampoline(void);
extern void enter_user_mode(unsigned int user_eip, unsigned int user_esp);

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
    while (zombie) {
        task_t* z = zombie;
        zombie = z->next;
        // Run any per-task cleanup hook first (loaders use this to
        // release user pages, file handles, etc.). Interrupts are off
        // here — the hook should be quick and side-effect-free beyond
        // freeing its own resources.
        if (z->on_exit) z->on_exit(z);
        if (z->stack_base) kfree((void*)z->stack_base);
        kfree(z);
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
    __asm__ volatile ("cli");

    current->state = TASK_DEAD;
    ready_remove(current);

    // Push onto zombie list; the next task to yield will reap us.
    current->next = zombie;
    zombie = current;

    if (!ready) {
        __asm__ volatile ("sti");
        for (;;) __asm__ volatile ("hlt");
    }

    task_t* next = ready;
    while (next->state == TASK_DEAD) next = next->next;
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
                        (p->state == TASK_READY)   ? "ready" : "dead ";
        printf("  %d   %s   %s%s\n",
               p->id, s, p->name, (p == current) ? "  *" : "");
        p = p->next;
    } while (p != ready);
}
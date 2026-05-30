#include "task.h"
#include "kheap.h"
#include "printf.h"
#include "string.h"
#include "gdt.h"
#include "vmm.h"
#include "isr.h"

/* Defined in task_switch.asm. */
extern void task_switch(uint64_t* old_rsp_ptr, uint64_t new_rsp);
extern void task_trampoline(void);
extern void enter_user_mode(uint64_t user_rip, uint64_t user_rsp);
extern void enter_user_resume(struct registers* frame);

#define STACK_SIZE 8192

static inline void switch_pd(task_t* t) {
    /* Kernel tasks must always run on the real kernel PML4.
     * Do not use vmm_current() here: if we are switching away from
     * a ring-3 task, CR3 is still that task's user PML4.
     */
    uint64_t target = t->pd_phys ? t->pd_phys : vmm_kernel_pd();
    vmm_switch(target);
}

static task_t* current         = 0;
static task_t* ready           = 0;
static int     next_id         = 0;
static int     preempt_enabled = 0;
static task_t* zombie          = 0;

/* Build the initial kernel stack for a new task.
 *
 * Layout (top = low address, rsp points at saved r15):
 *   [entry_fn]          <- popped by trampoline's first `pop rax`
 *   [task_trampoline]   <- popped by `ret` in task_switch
 *   [r15=0 r14=0 r13=0 r12=0 rbx=0 rbp=0]  <- callee-saved regs
 *
 * task_switch pops r15..rbp then rets → jumps to trampoline.
 */
static uint64_t task_init_stack(uint64_t stack_top, task_entry_fn fn) {
    uint64_t* sp = (uint64_t*)stack_top;

    *--sp = (uint64_t)fn;              /* entry_fn           */
    *--sp = (uint64_t)task_trampoline; /* ret addr           */
    *--sp = 0;                         /* r15                */
    *--sp = 0;                         /* r14                */
    *--sp = 0;                         /* r13                */
    *--sp = 0;                         /* r12                */
    *--sp = 0;                         /* rbx                */
    *--sp = 0;                         /* rbp   <- rsp lands */

    return (uint64_t)sp;
}

static void ready_insert(task_t* t) {
    if (!ready) { ready = t; t->next = t; return; }
    t->next = ready->next;
    ready->next = t;
}

static void ready_remove(task_t* t) {
    if (!ready || !t) return;
    task_t* p = ready;
    while (p->next != t) {
        p = p->next;
        if (p == ready) return;
    }
    if (p == t) { ready = 0; }
    else {
        p->next = t->next;
        if (ready == t) ready = t->next;
    }
    t->next = 0;
}

static void name_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void tasking_init(void) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) { printf("tasking_init: OOM\n"); return; }
    memset(t, 0, sizeof(*t));

    t->rsp        = 0;
    t->stack_base = 0;
    t->stack_size = 0;
    t->kstack_top = 0;
    t->id         = next_id++;
    t->state      = TASK_RUNNING;
    t->is_user    = 0;
    name_copy(t->name, "kmain", sizeof(t->name));

    ready_insert(t);
    current = t;
}

int task_spawn(task_entry_fn fn, const char* name) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return -1;

    uint64_t base = (uint64_t)kmalloc(STACK_SIZE);
    if (!base) { kfree(t); return -1; }

    memset(t, 0, sizeof(*t));
    t->stack_base = base;
    t->stack_size = STACK_SIZE;
    t->kstack_top = base + STACK_SIZE;
    t->rsp        = task_init_stack(base + STACK_SIZE, fn);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->is_user    = 0;
    name_copy(t->name, name ? name : "task", sizeof(t->name));

    __asm__ volatile ("cli");
    ready_insert(t);
    __asm__ volatile ("sti");
    return t->id;
}

static void user_entry_shim(void);

int task_spawn_user(uint64_t user_rip, uint64_t user_rsp,
                    uint64_t pd_phys, const char* name) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return -1;

    uint64_t base = (uint64_t)kmalloc(STACK_SIZE);
    if (!base) { kfree(t); return -1; }

    memset(t, 0, sizeof(*t));
    t->stack_base = base;
    t->stack_size = STACK_SIZE;
    t->kstack_top = base + STACK_SIZE;
    t->rsp        = task_init_stack(base + STACK_SIZE, user_entry_shim);
    t->user_rip   = user_rip;
    t->user_rsp   = user_rsp;
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

static void user_entry_shim(void) {
    task_t* t = current;
    if (!t) { for (;;) __asm__ volatile ("hlt"); }
    tss_set_kernel_stack(t->kstack_top);
    enter_user_mode(t->user_rip, t->user_rsp);
    for (;;) __asm__ volatile ("hlt");
}

static void fork_resume_shim(void) {
    task_t* t = current;
    if (!t || !t->resume_frame) {
        printf("[task] fork shim: no resume frame\n");
        for (;;) __asm__ volatile ("hlt");
    }
    tss_set_kernel_stack(t->kstack_top);
    enter_user_resume((struct registers*)t->resume_frame);
    for (;;) __asm__ volatile ("hlt");
}

/* Size of the register frame we copy for fork (struct registers = 176 bytes). */
#define FORK_FRAME_BYTES sizeof(struct registers)

int task_clone_user(struct registers* frame, uint64_t pd_phys,
                    const char* name) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return -1;

    uint64_t base = (uint64_t)kmalloc(STACK_SIZE);
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

    /* Copy the register frame onto the top of the child's kernel stack. */
    uint64_t sp = base + STACK_SIZE - FORK_FRAME_BYTES;
    memcpy((void*)sp, frame, FORK_FRAME_BYTES);
    t->resume_frame = sp;

    t->rsp = task_init_stack(sp, fork_resume_shim);

    __asm__ volatile ("cli");
    ready_insert(t);
    __asm__ volatile ("sti");
    return t->id;
}

static task_t* pick_next(task_t* from) {
    task_t* p = from->next;
    while (p != from) {
        if (p->state == TASK_READY || p->state == TASK_RUNNING) return p;
        p = p->next;
    }
    return from;
}

static void reap_zombies(void) {
    task_t* prev = 0;
    task_t* z = zombie;
    while (z) {
        task_t* next = z->next;
        if (z->reaped_by_parent) {
            if (prev) prev->next = next;
            else      zombie = next;
            if (z->on_exit) z->on_exit(z);
            if (z->stack_base) kfree((void*)z->stack_base);
            kfree(z);
        } else {
            prev = z;
        }
        z = next;
    }
}

void yield(void) {
    __asm__ volatile ("cli");
    reap_zombies();
    if (!current || !ready) { __asm__ volatile ("sti"); return; }

    task_t* next = pick_next(current);
    if (next == current) { __asm__ volatile ("sti"); return; }

    task_t* prev = current;
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current     = next;

    if (next->is_user) tss_set_kernel_stack(next->kstack_top);
    switch_pd(next);
    task_switch(&prev->rsp, next->rsp);
    __asm__ volatile ("sti");
}

void task_exit(void) { task_exit_code(0); }

void task_exit_code(int code) {
    __asm__ volatile ("cli");

    current->exit_code  = code;
    current->has_exited = 1;
    current->state      = TASK_DEAD;
    ready_remove(current);

    task_t* p = current->parent;
    if (p && p->state == TASK_BLOCKED && p->wait_any)
        p->state = TASK_READY;

    if (!p) current->reaped_by_parent = 1;

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
            __asm__ volatile ("sti");
            for (;;) __asm__ volatile ("hlt");
        }
    }
    next->state = TASK_RUNNING;
    current     = next;

    if (next->is_user) tss_set_kernel_stack(next->kstack_top);
    switch_pd(next);

    uint64_t dummy;
    task_switch(&dummy, next->rsp);
    for (;;) __asm__ volatile ("hlt");
}

void tasking_enable_preemption(void) { preempt_enabled = 1; }

void scheduler_tick(void) {
    if (!preempt_enabled || !current || !ready) return;

    task_t* next = pick_next(current);
    if (next == current) return;

    task_t* prev = current;
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current     = next;

    if (next->is_user) tss_set_kernel_stack(next->kstack_top);
    switch_pd(next);
    task_switch(&prev->rsp, next->rsp);
}

task_t* task_current(void) { return current; }

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

static task_t* find_dead_child(void) {
    for (task_t* z = zombie; z; z = z->next)
        if (z->parent == current && z->has_exited && !z->reaped_by_parent)
            return z;
    return 0;
}

int task_wait_child(int* status) {
    for (;;) {
        __asm__ volatile ("cli");
        task_t* z = find_dead_child();
        if (z) {
            int pid = z->id;
            if (status) *status = z->exit_code;
            z->reaped_by_parent = 1;
            __asm__ volatile ("sti");
            return pid;
        }
        if (!has_any_child()) { __asm__ volatile ("sti"); return -1; }
        current->wait_any = 1;
        current->state    = TASK_BLOCKED;
        __asm__ volatile ("sti");
        yield();
        current->wait_any = 0;
    }
}

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
        const char* s = (p->state == TASK_RUNNING) ? "RUN  " :
                        (p->state == TASK_READY)   ? "ready" :
                        (p->state == TASK_BLOCKED) ? "block" : "dead ";
        printf("  %d   %s   %s%s\n",
               p->id, s, p->name, (p == current) ? "  *" : "");
        p = p->next;
    } while (p != ready);
}
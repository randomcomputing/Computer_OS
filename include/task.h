#ifndef TASK_H
#define TASK_H

#include "stdint.h"

typedef void (*task_entry_fn)(void);

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD,
} task_state_t;

typedef struct task {
    uint64_t  rsp;          /* saved kernel stack pointer (offset 0 — asm relies on this) */
    uint64_t  stack_base;
    uint64_t  stack_size;
    uint64_t  kstack_top;   /* used as tss.rsp0 when user task runs */
    uint64_t  user_rip;     /* initial ring-3 RIP */
    uint64_t  user_rsp;     /* initial ring-3 RSP */
    uint64_t  resume_frame; /* forked child: addr of saved register frame */
    uint64_t  pd_phys;      /* physical addr of PML4; 0 = kernel PD */
    int       id;
    task_state_t state;
    int       is_user;
    char      name[16];

    struct task* parent;
    int       exit_code;
    int       has_exited;
    int       wait_any;
    int       reaped_by_parent;

    void    (*on_exit)(struct task*);
    void*     user_data;

    uint64_t  in_user_access;
    uint64_t  fault_recovery_rip;
    int       uaccess_faulted;

    struct task* next;
} task_t;

void     tasking_init(void);
int      task_spawn(task_entry_fn fn, const char* name);
int      task_spawn_user(uint64_t user_rip, uint64_t user_rsp,
                         uint64_t pd_phys, const char* name);

struct registers;
int      task_clone_user(struct registers* frame, uint64_t pd_phys,
                         const char* name);

void     yield(void);
void     task_exit(void);
void     task_exit_trampoline(void);
void     task_exit_code(int code);
int      task_wait_child(int* status);
void     tasking_enable_preemption(void);
void     scheduler_tick(void);

task_t*  task_current(void);
task_t*  task_find_by_id(int id);
void     task_list_print(void);
void     task_count_states(int* out_total, int* out_running,
                           int* out_ready, int* out_blocked);

#endif
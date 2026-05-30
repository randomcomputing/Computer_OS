#include "userprog.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "printf.h"
#include "string.h"
#include "stdint.h"

/* 32-bit hello-world flat binary (int 0x80 syscalls). */
#define HELLO_SIZE 62
static const unsigned char hello_bin[HELLO_SIZE] = {
    0xb8, 0x01, 0x00, 0x00, 0x00, 0xbb, 0x01, 0x00, 0x00, 0x00, 0xb9, 0x2b,
    0x00, 0x40, 0x00, 0xba, 0x13, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x03,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xbb, 0x00,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0xeb, 0xfe, 0x48, 0x65, 0x6c, 0x6c, 0x6f,
    0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x72, 0x69, 0x6e, 0x67, 0x20, 0x33,
    0x21, 0x0a,
};

typedef struct {
    uint64_t code_phys;
    uint64_t stack_phys;
    uint64_t pd_phys;
} hello_resources_t;

static void hello_on_exit(task_t* t) {
    hello_resources_t* hr = (hello_resources_t*)t->user_data;
    if (hr) {
        if (hr->code_phys)  pmm_free(hr->code_phys);
        if (hr->stack_phys) pmm_free(hr->stack_phys);
        if (hr->pd_phys) {
            vmm_free_user_pd(hr->pd_phys);
            t->pd_phys = 0;
        }
    }
}

static hello_resources_t hello_res;

int userprog_run_hello(void) {
    uint64_t pd_phys = vmm_new_user_pd();
    if (!pd_phys) {
        printf("[userprog] failed to create user PD\n");
        return -1;
    }

    uint64_t code_frame = pmm_alloc();
    if (!code_frame) {
        printf("[userprog] OOM (code)\n");
        vmm_free_user_pd(pd_phys);
        return -1;
    }
    if (!vmm_map_in(pd_phys, USERPROG_LOAD_ADDR, code_frame,
                    VMM_PRESENT | VMM_WRITE | VMM_USER)) {
        printf("[userprog] vmm_map_in failed (code)\n");
        pmm_free(code_frame);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    /* Access the frame via the HHDM. */
    unsigned char* dst = (unsigned char*)vmm_phys_to_virt(code_frame);
    memset(dst, 0, 4096);
    for (unsigned int i = 0; i < HELLO_SIZE; i++) dst[i] = hello_bin[i];

    uint64_t stack_frame = pmm_alloc();
    if (!stack_frame) {
        printf("[userprog] OOM (stack)\n");
        pmm_free(code_frame);
        vmm_free_user_pd(pd_phys);
        return -1;
    }
    uint64_t stack_page = USERPROG_STACK_TOP - 0x1000;
    if (!vmm_map_in(pd_phys, stack_page, stack_frame,
                    VMM_PRESENT | VMM_WRITE | VMM_USER)) {
        printf("[userprog] vmm_map_in failed (stack)\n");
        pmm_free(stack_frame);
        pmm_free(code_frame);
        vmm_free_user_pd(pd_phys);
        return -1;
    }
    memset((void*)vmm_phys_to_virt(stack_frame), 0, 4096);

    hello_res.code_phys  = code_frame;
    hello_res.stack_phys = stack_frame;
    hello_res.pd_phys    = pd_phys;

    int id = task_spawn_user(USERPROG_LOAD_ADDR, USERPROG_STACK_TOP,
                             pd_phys, "hello");
    if (id < 0) {
        printf("[userprog] task_spawn_user failed\n");
        pmm_free(stack_frame);
        pmm_free(code_frame);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    task_t* t = task_find_by_id(id);
    if (t) {
        t->user_data = &hello_res;
        t->on_exit   = hello_on_exit;
    }

    return id;
}
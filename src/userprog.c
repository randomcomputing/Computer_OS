#include "userprog.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "printf.h"
#include "string.h"

// --- the demo program -------------------------------------------------
//
// Source (NASM-flavored):
//
//     [BITS 32]
//     [ORG 0x00400000]
//         mov eax, 1                    ; SYS_WRITE
//         mov ebx, 1                    ; fd = stdout
//         mov ecx, msg
//         mov edx, 19                   ; len
//         int 0x80
//         mov eax, 3                    ; SYS_GETPID
//         int 0x80
//         mov eax, 0                    ; SYS_EXIT
//         mov ebx, 0
//         int 0x80
//     hang:
//         jmp hang
//     msg: db 'Hello from ring 3!', 10

#define HELLO_SIZE 62
static const unsigned char hello_bin[HELLO_SIZE] = {
    0xb8, 0x01, 0x00, 0x00, 0x00, 0xbb, 0x01, 0x00, 0x00, 0x00, 0xb9, 0x2b,
    0x00, 0x40, 0x00, 0xba, 0x13, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x03,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xbb, 0x00,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0xeb, 0xfe, 0x48, 0x65, 0x6c, 0x6c, 0x6f,
    0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x72, 0x69, 0x6e, 0x67, 0x20, 0x33,
    0x21, 0x0a,
};

// Resource tracking for the demo task.
typedef struct {
    unsigned int code_phys;
    unsigned int stack_phys;
} hello_resources_t;

static void hello_on_exit(task_t* t) {
    hello_resources_t* hr = (hello_resources_t*)t->user_data;
    if (hr) {
        if (hr->code_phys)  pmm_free(hr->code_phys);
        if (hr->stack_phys) pmm_free(hr->stack_phys);
        // page tables + PD are freed separately via vmm_free_user_pd
        if (t->pd_phys) {
            vmm_free_user_pd(t->pd_phys);
            t->pd_phys = 0;
        }
        // hr itself lives in static storage (no heap allocation needed)
    }
}

static hello_resources_t hello_res;

int userprog_run_hello(void) {
    unsigned int pd_phys = vmm_create_user_pd();
    if (!pd_phys) {
        printf("[userprog] failed to create user PD\n");
        return -1;
    }

    // Allocate the code page and map it into the user PD.
    unsigned int code_frame = pmm_alloc();
    if (!code_frame) {
        printf("[userprog] out of memory (code)\n");
        vmm_free_user_pd(pd_phys);
        return -1;
    }
    if (!vmm_map_pd(pd_phys, USERPROG_LOAD_ADDR, code_frame,
                    VMM_PRESENT | VMM_WRITE | VMM_USER)) {
        printf("[userprog] vmm_map_pd failed (code)\n");
        pmm_free(code_frame);
        vmm_free_user_pd(pd_phys);
        return -1;
    }

    // Copy the program into the frame via its physical (identity-mapped)
    // address, then switch to the user PD — both approaches work here
    // since the binary is tiny.  We use physical access to stay uniform
    // with the loader.
    memset((void*)code_frame, 0, 4096);
    unsigned char* dst = (unsigned char*)code_frame;
    for (unsigned int i = 0; i < HELLO_SIZE; i++) dst[i] = hello_bin[i];

    // Allocate the stack page.
    unsigned int stack_frame = pmm_alloc();
    if (!stack_frame) {
        printf("[userprog] out of memory (stack)\n");
        pmm_free(code_frame);
        vmm_free_user_pd(pd_phys);
        return -1;
    }
    unsigned int stack_page = USERPROG_STACK_TOP - 0x1000;
    if (!vmm_map_pd(pd_phys, stack_page, stack_frame,
                    VMM_PRESENT | VMM_WRITE | VMM_USER)) {
        printf("[userprog] vmm_map_pd failed (stack)\n");
        pmm_free(stack_frame);
        pmm_free(code_frame);
        vmm_free_user_pd(pd_phys);
        return -1;
    }
    memset((void*)stack_frame, 0, 4096);

    hello_res.code_phys  = code_frame;
    hello_res.stack_phys = stack_frame;

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

// forktest.c — exercises the new fork() / wait() / exec() syscalls.
//
// Run from the shell with:   run FORKTEST.ELF
//
// It does three things in sequence:
//
//   1. fork() a child. Parent and child print who they are (the child
//      sees fork()==0 and its own pid via getpid(); the parent sees the
//      child's pid as fork's return value). The child exits with a
//      distinctive code; the parent wait()s and prints the collected
//      status to prove the parent/child split and status hand-off work.
//
//   2. Demonstrates that memory is COPIED, not shared: the child mutates
//      a local variable and the parent confirms its own copy is intact
//      after the child has exited.
//
//   3. fork()s a second child that exec()s ECHO.ELF, replacing its image
//      in place. The parent wait()s for it. This shows exec swapping the
//      program while keeping the same task slot / parent relationship.

#include "stdio.h"
#include "syscalls.h"

int main(void) {
    printf("[forktest] start, my pid = %d\n", getpid());

    // ---- Part 1 + 2: fork, divergent paths, copy-on-fork memory ------
    int shared = 0xAA;          // each process gets its own copy
    int pid = fork();

    if (pid < 0) {
        printf("[forktest] fork failed\n");
        return 1;
    }

    if (pid == 0) {
        // Child path.
        printf("  [child] running, getpid=%d, fork returned 0\n", getpid());
        shared = 0xBB;          // mutate our private copy
        printf("  [child] set shared=0x%x in my address space\n", shared);
        printf("  [child] exiting with code 42\n");
        exit(42);
    }

    // Parent path.
    printf("[parent] forked child pid=%d\n", pid);
    int status = -1;
    int got = wait(&status);
    printf("[parent] wait() collected pid=%d status=%d\n", got, status);
    printf("[parent] my shared is still 0x%x (child's change didn't leak)\n",
           shared);

    // ---- Part 3: fork + exec ----------------------------------------
    printf("[forktest] now forking a child that will exec ECHO.ELF\n");
    int pid2 = fork();
    if (pid2 < 0) {
        printf("[forktest] second fork failed\n");
        return 1;
    }
    if (pid2 == 0) {
        printf("  [child2] calling exec(\"ECHO.ELF\") - type a line for it\n");
        exec("ECHO.ELF");
        // Only reached if exec failed.
        printf("  [child2] exec failed!\n");
        exit(7);
    }

    int status2 = -1;
    int got2 = wait(&status2);
    printf("[parent] exec'd child pid=%d finished, status=%d\n",
           got2, status2);

    printf("[forktest] all done\n");
    return 0;
}

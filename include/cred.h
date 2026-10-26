#ifndef CRED_H
#define CRED_H

#include "stdint.h"

/*
 * Minimal credential model: a single current (uid, gid) the system runs as,
 * switched with the shell's `su`. The system boots as root (uid 0). This is
 * deliberately global rather than per-task for now; per-process credentials
 * that inherit on spawn are a later refinement. CFS reads cred_uid/cred_gid
 * to enforce permissions.
 */

uint32_t cred_uid(void);
uint32_t cred_gid(void);
void     cred_set(uint32_t uid, uint32_t gid);

/* Tiny fixed user table: root=0, user=1000. */
const char* cred_name(uint32_t uid);                 /* name, or 0 if unknown */
int cred_lookup(const char* name, uint32_t* uid, uint32_t* gid);  /* 1 if known */

#endif
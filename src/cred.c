#include "cred.h"
#include "string.h"

/* Boot as root. */
static uint32_t g_uid = 0;
static uint32_t g_gid = 0;

uint32_t cred_uid(void) { return g_uid; }
uint32_t cred_gid(void) { return g_gid; }
void     cred_set(uint32_t uid, uint32_t gid) { g_uid = uid; g_gid = gid; }

const char* cred_name(uint32_t uid) {
    if (uid == 0)    return "root";
    if (uid == 1000) return "user";
    return 0;
}

int cred_lookup(const char* name, uint32_t* uid, uint32_t* gid) {
    if (strcmp(name, "root") == 0) { *uid = 0;    *gid = 0;    return 1; }
    if (strcmp(name, "user") == 0) { *uid = 1000; *gid = 1000; return 1; }
    return 0;
}
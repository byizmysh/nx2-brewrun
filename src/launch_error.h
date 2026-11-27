#ifndef IZ_LAUNCH_ERROR_H
#define IZ_LAUNCH_ERROR_H

#include <stdbool.h>
#include <stdint.h>

#define IZ_LAUNCH_LOG "launch-errors.log"

typedef enum {
    IZ_EXEFS_CHECK_NONE,
    IZ_EXEFS_CHECK_SETTINGS,
    IZ_EXEFS_CHECK_FILE,
    IZ_EXEFS_CHECK_SEGMENTS,
    IZ_EXEFS_CHECK_MAPPING,
    IZ_EXEFS_CHECK_TOTAL
} IzExefsCheck;

typedef struct {
    IzExefsCheck check;
    uint64_t required, available;
} IzExefsFailure;

typedef struct {
    const char *stage, *reason, *module;
    char path[512];
    uint64_t title, file_bytes, stored_bytes;
    unsigned room, saved_budget, initialized_limit, total_limit;
    IzExefsFailure exefs;
    uint32_t result;
    int runtime_exit;
    bool logged;
} IzLaunchError;

const char *iz_exefs_check_label(IzExefsCheck);
unsigned iz_bytes_to_mib_ceil(uint64_t);
void iz_log_launch_error(const char *self, IzLaunchError *);

#endif

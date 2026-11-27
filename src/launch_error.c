#include "launch_error.h"
#include "settings.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

const char *iz_exefs_check_label(IzExefsCheck check) {
    switch (check) {
        case IZ_EXEFS_CHECK_SETTINGS: return "Configured budget";
        case IZ_EXEFS_CHECK_FILE: return "Module file size";
        case IZ_EXEFS_CHECK_SEGMENTS: return "Decoded module segments";
        case IZ_EXEFS_CHECK_MAPPING: return "Module staging";
        case IZ_EXEFS_CHECK_TOTAL: return "Combined module mappings";
        default: return "";
    }
}

unsigned iz_bytes_to_mib_ceil(uint64_t bytes) {
    return (unsigned)(bytes >> 20) + ((bytes & ((1u << 20) - 1)) != 0);
}

void iz_log_launch_error(const char *self, IzLaunchError *error) {
    error->logged = false;
    char path[512];
    if (!iz_sibling_path(self, IZ_LAUNCH_LOG, path, sizeof(path))) return;
    FILE *log = fopen(path, "a");
    if (!log) return;

    char timestamp[32] = "time unavailable";
    const time_t now = time(NULL);
    struct tm utc;
    if (now != (time_t)-1 && gmtime_r(&now, &utc))
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    fprintf(log, "[%s] %s\n  error: %s\n", timestamp, error->stage,
            error->reason ? error->reason : "No additional details were returned.");
    if (error->path[0]) {
        fputs("  image: ", log);
        for (const unsigned char *p = (const unsigned char *)error->path; *p; ++p)
            fputc(*p < 32 || *p == 127 ? '?' : *p, log);
        fputc('\n', log);
    }
    if (error->title) fprintf(log, "  title ID: %016" PRIX64 "\n", error->title);
    if (error->module) fprintf(log, "  module: %s\n", error->module);
    if (error->file_bytes || error->stored_bytes)
        fprintf(log, "  file: %" PRIu64 " bytes, stored: %" PRIu64 " bytes\n",
                error->file_bytes, error->stored_bytes);
    if (error->room) {
        fprintf(log, "  Loader room: %u MiB, ExeFS budget: ", error->room);
        if (error->saved_budget) fprintf(log, "%u MiB\n", error->saved_budget);
        else fputs("Auto\n", log);
    }
    if (error->total_limit)
        fprintf(log, "  ExeFS limits: %u MiB per initialized module, %u MiB total\n",
                error->initialized_limit, error->total_limit);
    if (error->exefs.check)
        fprintf(log, "  %s: requires %" PRIu64 " bytes (%u MiB rounded up), limit %" PRIu64 " bytes\n",
                iz_exefs_check_label(error->exefs.check), error->exefs.required,
                iz_bytes_to_mib_ceil(error->exefs.required), error->exefs.available);
    if (error->result) fprintf(log, "  Horizon result: 0x%08" PRIX32 "\n", error->result);
    if (error->runtime_exit)
        fprintf(log, "  Runtime exit status: %d. See nup-%016" PRIx64 "/log.txt and crash.txt.\n",
                error->runtime_exit, error->title);
    fputc('\n', log);
    const bool written = !ferror(log) && !fflush(log) && !fsync(fileno(log));
    error->logged = !fclose(log) && written;
}

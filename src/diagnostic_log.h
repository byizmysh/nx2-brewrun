#ifndef IZ_DIAGNOSTIC_LOG_H
#define IZ_DIAGNOSTIC_LOG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *text;
    size_t lines, longest_line;
    bool cropped;
} IzDiagnosticLog;

bool iz_read_diagnostic_log(const char *path, IzDiagnosticLog *, const char **error);

#endif

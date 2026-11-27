#include "diagnostic_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

enum { DIAGNOSTIC_LOG_LIMIT = 64 * 1024 };

bool iz_read_diagnostic_log(const char *path, IzDiagnosticLog *log, const char **error) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        *error = errno == ENOENT ? "This log has not been created yet." : "Could not open the log on the SD card.";
        return false;
    }
    const char *reason = "Could not read the log on the SD card.";
    char *data = NULL;
    const off_t size = fseeko(file, 0, SEEK_END) ? -1 : ftello(file);
    if (size <= 0) {
        if (!size) reason = "This log is empty.";
        goto fail;
    }

    const off_t offset = size > DIAGNOSTIC_LOG_LIMIT ? size - DIAGNOSTIC_LOG_LIMIT : 0;
    if (fseeko(file, offset ? offset - 1 : 0, SEEK_SET)) goto fail;
    const int before = offset ? fgetc(file) : '\n';
    if (before == EOF) goto fail;
    size_t length = (size_t)(size - offset);
    data = malloc(length + 1);
    if (!data) {
        reason = "Not enough memory to open this log.";
        goto fail;
    }
    if (fread(data, 1, length, file) != length) goto fail;
    fclose(file);

    if (before != '\n') {
        const char *newline = memchr(data, '\n', length - 1);
        if (newline) {
            const size_t skip = (size_t)(newline - data) + 1;
            length -= skip;
            memmove(data, data + skip, length);
        }
    }
    size_t lines = 1, width = 0, longest = 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)data[i];
        if (c == '\n') {
            if (i + 1 < length) ++lines;
            if (width > longest) longest = width;
            width = 0;
        } else {
            if (c < ' ' || c == 127) data[i] = ' ';
            ++width;
        }
    }
    data[length] = '\0';
    *log = (IzDiagnosticLog){data, lines, width > longest ? width : longest, offset != 0};
    return true;

fail:
    fclose(file);
    free(data);
    *error = reason;
    return false;
}

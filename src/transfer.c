#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "transfer.h"
#include "nup_parts.h"
#include "settings.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef IZ_TRANSFER_WHOLE_LIMIT
#define IZ_TRANSFER_WHOLE_LIMIT UINT32_MAX
#endif
#ifndef IZ_TRANSFER_AUTO_PART
#define IZ_TRANSFER_AUTO_PART ((uint64_t)IZ_SPLIT_MAX_MIB << 20)
#endif

enum {
    NAME_CAP = IZ_TRANSFER_NAME_CAP, PATH_CAP = 512, STAGING_CAP = 480, HEADER_CAP = 8192,
    CHUNK = 512 * 1024, SAMPLE = 64 * 1024, CHECKPOINT = 32 * 1024 * 1024, KEY_SIZE = 24
};
static const char staging_prefix[] = ".brerun-resume-";
static const uint64_t state_magic = UINT64_C(0x4252554e00000001);

typedef struct {
    char name[NAME_CAP], key[KEY_SIZE + 1];
    uint64_t size, length, offset;
    char *code;
    bool expect;
} Upload;

typedef struct {
    uint64_t magic, size, part_size;
    char name[NAME_CAP], key[KEY_SIZE + 1];
} SavedUpload;

typedef struct {
    char staging[STAGING_CAP], target[PATH_CAP];
    SavedUpload saved;
    uint64_t offset;
    uint32_t tail;
} Output;

typedef struct {
    char staging[STAGING_CAP];
    unsigned id;
} SelectedJob;

#include "transfer_page.inc"

static bool stopped(const IzTransfer *transfer) {
    return atomic_load_explicit(&transfer->stop, memory_order_relaxed);
}

void iz_transfer_request(IzTransfer *transfer, unsigned job, IzTransferCommand command) {
    if (job && atomic_load(&transfer->job) == job) atomic_store(&transfer->command, (job << 2) | command);
}

static bool interrupted(const IzTransfer *transfer, unsigned job) {
    return stopped(transfer) || (job && atomic_load(&transfer->command) >> 2 == job);
}

static int hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool decode_name(const char *encoded, char name[NAME_CAP]) {
    size_t n = 0;
    while (*encoded) {
        unsigned char ch = *encoded++;
        if (ch == '%') {
            if (!encoded[0] || !encoded[1]) return false;
            const int hi = hex_digit(encoded[0]), lo = hex_digit(encoded[1]);
            if (hi < 0 || lo < 0) return false;
            ch = (unsigned char)(hi * 16 + lo);
            encoded += 2;
        }
        if (n + 1 >= NAME_CAP || ch < 32 || ch == 127 || strchr("/\\:*?\"<>|", ch)) return false;
        name[n++] = (char)ch;
    }
    name[n] = 0;
    return n > 4 && name[0] != '.' && name[0] != ' ' && !strcasecmp(name + n - 4, ".nup");
}

static bool number(const char *text, uint64_t *value) {
    if (!*text) return false;
    *value = 0;
    while (*text) {
        const unsigned digit = (unsigned)(*text++ - '0');
        if (digit > 9 || *value > (INT64_MAX - digit) / UINT64_C(10)) return false;
        *value = 10 * *value + digit;
    }
    return true;
}

static bool parse_upload(char *fields, Upload *upload, bool sending) {
    bool length = false, size = false, offset = false;
    for (char *line = fields; *line;) {
        char *end = strstr(line, "\r\n");
        if (!end) return false;
        *end = 0;
        if (end == line) break;
        char *value = strchr(line, ':');
        if (!value || value == line || *line == ' ' || *line == '\t') return false;
        *value++ = 0;
        while (*value == ' ' || *value == '\t') ++value;
        char *tail = end;
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!strcasecmp(line, "Content-Length")) {
            if (length || !number(value, &upload->length)) return false;
            length = true;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            return false;
        } else if (!strcasecmp(line, "X-BrewRun-Code")) {
            if (upload->code) return false;
            upload->code = value;
        } else if (!strcasecmp(line, "X-BrewRun-Name")) {
            if (upload->name[0] || !decode_name(value, upload->name)) return false;
        } else if (!strcasecmp(line, "X-BrewRun-Key")) {
            if (upload->key[0] || strlen(value) != KEY_SIZE) return false;
            for (unsigned i = 0; i < KEY_SIZE; ++i)
                if (hex_digit((unsigned char)value[i]) < 0) return false;
            strcpy(upload->key, value);
        } else if (!strcasecmp(line, "X-BrewRun-Size")) {
            if (size || !number(value, &upload->size)) return false;
            size = true;
        } else if (!strcasecmp(line, "X-BrewRun-Offset")) {
            if (offset || !number(value, &upload->offset)) return false;
            offset = true;
        } else if (!strcasecmp(line, "Expect")) {
            if (upload->expect || strcasecmp(value, "100-continue")) return false;
            upload->expect = true;
        }
        line = end + 2;
    }
    if (!upload->name[0] || !upload->key[0] || !size || upload->size < 128) return false;
    return sending ? length && offset && upload->offset <= upload->size &&
        upload->length == upload->size - upload->offset : !upload->length && !offset;
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static ssize_t receive(const IzTransfer *transfer, int client, void *data, size_t size, uint64_t deadline,
                       unsigned job) {
    struct pollfd input = {.fd = client, .events = POLLIN};
    while (!interrupted(transfer, job) && monotonic_ms() < deadline) {
        const int ready = poll(&input, 1, 150);
        if (ready > 0) return recv(client, data, size, 0);
        if (ready < 0 && errno != EINTR) break;
    }
    return -1;
}

static bool send_all(int client, const char *data, size_t size) {
    while (size) {
#ifdef MSG_NOSIGNAL
        const ssize_t sent = send(client, data, size, MSG_NOSIGNAL);
#else
        const ssize_t sent = send(client, data, size, 0);
#endif
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        size -= (size_t)sent;
    }
    return true;
}

static void respond_bytes(int client, const char *status, const char *type, const char *body, size_t size) {
    char header[240];
    const int bytes = snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n", status, type, size);
    if (bytes > 0 && (size_t)bytes < sizeof(header) && send_all(client, header, bytes))
        send_all(client, body, size);
}

static void respond(int client, const char *status, const char *type, const char *body) {
    respond_bytes(client, status, type, body, strlen(body));
}

static bool path_available(const char *path) {
    struct stat st;
#ifdef __SWITCH__
    return stat(path, &st) != 0 && errno == ENOENT;
#else
    return lstat(path, &st) != 0 && errno == ENOENT;
#endif
}

static uint32_t fingerprint(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}

static void set_filename(IzTransfer *transfer, const char *name) {
    for (size_t i = 0; i < NAME_CAP; ++i) {
        atomic_store_explicit(&transfer->filename[i], (unsigned char)name[i], memory_order_relaxed);
        if (!name[i]) break;
    }
}

void iz_transfer_filename(const IzTransfer *transfer, char name[IZ_TRANSFER_NAME_CAP]) {
    for (size_t i = 0; i < NAME_CAP - 1; ++i) {
        name[i] = (char)atomic_load_explicit(&transfer->filename[i], memory_order_relaxed);
        if (!name[i]) return;
    }
    name[NAME_CAP - 1] = 0;
}

static void staged_path(const Output *out, unsigned part, char path[PATH_CAP]) {
    if (out->saved.part_size)
        snprintf(path, PATH_CAP, "%s/data/%03u", out->staging, part + 1);
    else
        snprintf(path, PATH_CAP, "%s/data/original", out->staging);
}

static bool stage_path(Output *out, const char *directory, const char *filename, uint64_t size, const char *key) {
    const uint32_t name = fingerprint(filename, strlen(filename));
    const int n = snprintf(out->staging, sizeof(out->staging), "%s/%s%08" PRIx32 "-%" PRIu64 "-%s",
        directory, staging_prefix, name, size, key);
    if (n > 0 && (size_t)n < sizeof(out->staging)) return true;
    out->staging[0] = 0;
    return false;
}

static bool target_path(Output *out, const char *directory) {
    const int n = snprintf(out->target, sizeof(out->target), "%s/%s", directory, out->saved.name);
    if (n <= 0 || (size_t)n >= sizeof(out->target)) return false;
    if (out->saved.part_size) {
        char *stem = out->target + n - 4;
        while (stem[-1] == ' ' || stem[-1] == '.') --stem;
        *stem = 0;
    }
    return true;
}

static bool publish(const Output *out) {
    if (!path_available(out->target)) return false;
    char source[PATH_CAP];
    if (out->saved.part_size)
        snprintf(source, sizeof(source), "%s/data", out->staging);
    else
        staged_path(out, 0, source);
#ifdef __SWITCH__
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd || strncmp(source, "sdmc:/", 6) || strncmp(out->target, "sdmc:/", 6)) return false;
    return R_SUCCEEDED(out->saved.part_size ? fsFsRenameDirectory(sd, source + 5, out->target + 5) :
                                             fsFsRenameFile(sd, source + 5, out->target + 5));
#else
    return rename(source, out->target) == 0;
#endif
}

static bool remove_job(const Output *out) {
    char folder[PATH_CAP];
    snprintf(folder, sizeof(folder), "%s/data", out->staging);
    DIR *dir = opendir(folder);
    if (dir) {
        bool ok = true, original = false, parts[NUP_PART_LIMIT + 1] = {0};
        for (struct dirent *entry; (entry = readdir(dir));) {
            const char *name = entry->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            if (!strcmp(name, "original")) {
                original = true;
                continue;
            }
            const unsigned part = (unsigned)strtoul(name, NULL, 10);
            if (strlen(name) != 3 || strspn(name, "0123456789") != 3 || !part || part > NUP_PART_LIMIT) {
                ok = false;
                continue;
            }
            parts[part] = true;
        }
        if (closedir(dir) || !ok) return false;
        if (original) {
            char path[PATH_CAP];
            snprintf(path, sizeof(path), "%s/data/original", out->staging);
            if (unlink(path)) return false;
        }
        for (unsigned part = 1; part <= NUP_PART_LIMIT; ++part) {
            if (!parts[part]) continue;
            char path[PATH_CAP];
            snprintf(path, sizeof(path), "%s/data/%03u", out->staging, part);
            if (unlink(path)) return false;
        }
        if (rmdir(folder)) return false;
    } else if (errno != ENOENT) {
        return false;
    }
    char state[PATH_CAP];
    snprintf(state, sizeof(state), "%s/state", out->staging);
    if (unlink(state) && errno != ENOENT) return false;
    return !rmdir(out->staging) || errno == ENOENT;
}

static int reserve(const Output *out, unsigned part) {
    char path[PATH_CAP];
    staged_path(out, part, path);
    uint64_t size = out->saved.size;
    if (out->saved.part_size) {
        size -= part * out->saved.part_size;
        if (size > out->saved.part_size) size = out->saved.part_size;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd < 0 && errno == EEXIST) {
        fd = open(path, O_RDWR);
        if (fd < 0) return -1;
        struct stat status;
        if (!fstat(fd, &status) && S_ISREG(status.st_mode) && (uint64_t)status.st_size == size) return fd;
        close(fd);
        errno = EIO;
        return -1;
    }
    if (fd >= 0 && ftruncate(fd, (off_t)size)) {
        const int error = errno;
        close(fd);
        unlink(path);
        errno = error;
        return -1;
    }
    return fd;
}

static bool output_read(void *user, uint64_t offset, void *data, size_t size) {
    const Output *out = user;
    if (!iz_span(offset, size, out->saved.size)) return false;
    unsigned char *dst = data;
    const uint64_t part = out->saved.part_size;
    while (size) {
        const unsigned index = part ? (unsigned)(offset / part) : 0;
        const uint64_t at = part ? offset % part : offset;
        const size_t amount = part && part - at < size ? (size_t)(part - at) : size;
        char path[PATH_CAP];
        staged_path(out, index, path);
        const int fd = open(path, O_RDONLY);
        if (fd < 0) return false;
        size_t read_size = 0;
        if (lseek(fd, (off_t)at, SEEK_SET) >= 0) {
            while (read_size < amount) {
                const ssize_t got = read(fd, dst + read_size, amount - read_size);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) break;
                read_size += (size_t)got;
            }
        }
        close(fd);
        if (read_size != amount) return false;
        size -= amount;
        dst += amount;
        offset += amount;
    }
    return true;
}

static bool read_state(FILE *file, Output *out) {
    const size_t bytes = fread(&out->saved, 1, sizeof(out->saved), file);
    if (bytes != sizeof(out->saved) || out->saved.magic != state_magic ||
        out->saved.size < 128 || out->saved.size > INT64_MAX || out->saved.part_size > UINT32_MAX ||
        (out->saved.part_size && (out->saved.size - 1) / out->saved.part_size >= NUP_PART_LIMIT) ||
        !memchr(out->saved.name, 0, sizeof(out->saved.name)) ||
        out->saved.key[KEY_SIZE] || strlen(out->saved.key) != KEY_SIZE) return false;
    for (unsigned i = 0; i < KEY_SIZE; ++i)
        if (hex_digit((unsigned char)out->saved.key[i]) < 0) return false;

    char line[64];
    while (fgets(line, sizeof(line), file)) {
        uint64_t offset;
        uint32_t tail;
        const char *digits = strchr(line, ' ');
        if (digits && strlen(digits + 1) == 9 && digits[9] == '\n' &&
            strspn(digits + 1, "0123456789abcdef") == 8 &&
            sscanf(line, "%" SCNu64 " %8" SCNx32, &offset, &tail) == 2 &&
            offset >= 128 && offset > out->offset && offset <= out->saved.size) {
            out->offset = offset;
            out->tail = tail;
        }
    }
    return !ferror(file);
}

static bool tail_hash(const Output *out, uint64_t position, uint32_t *hash) {
    const size_t size = position < SAMPLE ? (size_t)position : SAMPLE;
    unsigned char *buffer = malloc(size);
    if (!buffer) return false;
    const bool ok = output_read((void *)out, position - size, buffer, size);
    if (ok) *hash = fingerprint(buffer, size);
    free(buffer);
    return ok;
}

static bool verify_checkpoint(const Output *out) {
    if (!out->offset) return true;
    const uint64_t step = out->saved.part_size ? out->saved.part_size : out->saved.size;
    const unsigned parts = (unsigned)((out->offset - 1) / step + 1);
    for (unsigned i = 0; i < parts; ++i) {
        char path[PATH_CAP];
        struct stat status;
        staged_path(out, i, path);
        const uint64_t remaining = out->saved.size - i * step;
        const uint64_t expected = remaining < step ? remaining : step;
        if (stat(path, &status) || !S_ISREG(status.st_mode) || (uint64_t)status.st_size != expected)
            return false;
    }
    uint32_t hash;
    return tail_hash(out, out->offset, &hash) && hash == out->tail;
}

typedef enum { JOB_NEW, JOB_SAVED, JOB_UNREADABLE } JobState;

static JobState load_job(Output *out, const Upload *upload, const char *directory) {
    if (!stage_path(out, directory, upload->name, upload->size, upload->key)) return JOB_UNREADABLE;
    char state[PATH_CAP];
    snprintf(state, sizeof(state), "%s/state", out->staging);
    FILE *file = fopen(state, "rb");
    if (!file) return errno == ENOENT ? JOB_NEW : JOB_UNREADABLE;
    const bool valid = read_state(file, out);
    fclose(file);
    if (!valid || out->saved.size != upload->size || strcmp(out->saved.name, upload->name) ||
        strcmp(out->saved.key, upload->key) || !target_path(out, directory) || !verify_checkpoint(out))
        return JOB_UNREADABLE;
    return JOB_SAVED;
}

static void select_job(IzTransfer *transfer, SelectedJob *selection, const char *staging) {
    if (!strcmp(selection->staging, staging)) return;
    if (*staging) selection->id = selection->id == UINT_MAX >> 2 ? 1 : selection->id + 1;
    strcpy(selection->staging, staging);
    atomic_store(&transfer->job, *staging ? selection->id : 0);
}

static IzTransferCommand take_command(IzTransfer *transfer, const SelectedJob *selection) {
    const unsigned command = atomic_exchange(&transfer->command, 0);
    return selection->staging[0] && command >> 2 == selection->id ? command & 3 : 0;
}

static void update_pending(IzTransfer *transfer, SelectedJob *selection) {
    DIR *dir = opendir(transfer->directory);
    if (!dir) return;
    unsigned count = 0;
    for (struct dirent *entry; (entry = readdir(dir));) {
        if (strncmp(entry->d_name, staging_prefix, sizeof(staging_prefix) - 1)) continue;
        char state[PATH_CAP];
        const int n = snprintf(state, sizeof(state), "%s/%s/state", transfer->directory, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(state)) continue;
        FILE *file = fopen(state, "rb");
        if (!file) continue;
        Output out = {0};
        const bool valid = read_state(file, &out);
        fclose(file);
        if (!valid || !stage_path(&out, transfer->directory, out.saved.name, out.saved.size, out.saved.key) ||
            strcmp(strrchr(out.staging, '/') + 1, entry->d_name)) continue;
        if (!count && selection) {
            set_filename(transfer, out.saved.name);
            atomic_store(&transfer->received, out.offset);
            atomic_store(&transfer->total, out.saved.size);
            atomic_store(&transfer->status, IZ_TRANSFER_INTERRUPTED);
            select_job(transfer, selection, out.staging);
        }
        ++count;
    }
    closedir(dir);
    atomic_store(&transfer->pending, count);
}

static IzTransferStatus cancel_job(IzTransfer *transfer, SelectedJob *selection, const Output *out) {
    if (!out->staging[0] || !remove_job(out)) return IZ_TRANSFER_DISK_ERROR;
    if (!strcmp(selection->staging, out->staging)) {
        select_job(transfer, selection, "");
        atomic_store(&transfer->received, 0);
        atomic_store(&transfer->total, 0);
        atomic_store(&transfer->status, IZ_TRANSFER_CANCELLED);
    }
    update_pending(transfer, NULL);
    return IZ_TRANSFER_CANCELLED;
}

static IzTransferStatus prepare_job(IzTransfer *transfer, const Upload *upload, Output *out) {
    out->saved = (SavedUpload){.magic = state_magic, .size = upload->size};
    strcpy(out->saved.name, upload->name);
    strcpy(out->saved.key, upload->key);
    if (transfer->part_size && upload->size > transfer->part_size)
        out->saved.part_size = transfer->part_size;
    if (!target_path(out, transfer->directory)) return IZ_TRANSFER_DISK_ERROR;
    if (!path_available(out->target)) return IZ_TRANSFER_EXISTS;
    struct statvfs space;
    if (!statvfs(transfer->directory, &space) && space.f_frsize &&
        (upload->size - 1) / space.f_frsize >= space.f_bavail) return IZ_TRANSFER_NO_SPACE;

    if (!remove_job(out) || mkdir(out->staging, 0777)) return IZ_TRANSFER_DISK_ERROR;
    char path[PATH_CAP];
    snprintf(path, sizeof(path), "%s/data", out->staging);
    IzTransferStatus failure = IZ_TRANSFER_DISK_ERROR;
    if (mkdir(path, 0777)) goto failed;
    int fd = reserve(out, 0);
    if (fd < 0 && !out->saved.part_size && upload->size > IZ_TRANSFER_WHOLE_LIMIT) {
        out->saved.part_size = IZ_TRANSFER_AUTO_PART;
        if (!target_path(out, transfer->directory)) goto failed;
        if (!path_available(out->target)) {
            failure = IZ_TRANSFER_EXISTS;
            goto failed;
        }
        fd = reserve(out, 0);
    }
    if (fd < 0) {
        if (errno == ENOSPC) failure = IZ_TRANSFER_NO_SPACE;
        goto failed;
    }
    if (close(fd)) goto failed;
    snprintf(path, sizeof(path), "%s/state", out->staging);
    FILE *file = fopen(path, "wb");
    if (!file) goto failed;
    const bool saved = fwrite(&out->saved, sizeof(out->saved), 1, file) == 1 &&
                       !fflush(file) && !fsync(fileno(file));
    if (fclose(file) || !saved) goto failed;
    update_pending(transfer, NULL);
    return IZ_TRANSFER_READY;

failed:
    remove_job(out);
    return failure;
}

static bool flush_part(int *fd) {
    if (*fd < 0) return true;
    const int saved = fsync(*fd), error = errno;
    const int closed = close(*fd);
    *fd = -1;
    if (saved) errno = error;
    return !saved && !closed;
}

static bool checkpoint(Output *out, int *fd, uint64_t position) {
    if (position <= out->offset) return true;
    uint32_t hash;
    if (!flush_part(fd) || !tail_hash(out, position, &hash)) return false;
    char state[PATH_CAP];
    snprintf(state, sizeof(state), "%s/state", out->staging);
    FILE *file = fopen(state, "ab");
    if (!file) return false;
    /* A leading newline keeps the next record separate after a torn write. */
    const bool saved = fprintf(file, "\n%" PRIu64 " %08" PRIx32 "\n", position, hash) > 0 &&
                       !fflush(file) && !fsync(fileno(file));
    if (fclose(file) || !saved) return false;
    out->offset = position;
    out->tail = hash;
    return true;
}

static IzTransferStatus write_upload(IzTransfer *transfer, int client, const Upload *upload,
                                     Output *out, JobState job, unsigned selection,
                                     const unsigned char *initial, size_t initial_size) {
    if (initial_size > upload->length) return IZ_TRANSFER_INVALID;
    unsigned char *buffer = malloc(CHUNK);
    if (!buffer) return IZ_TRANSFER_DISK_ERROR;
    memcpy(buffer, initial, initial_size);
    size_t buffered = initial_size;
    uint64_t position = out->offset;
    IzTransferStatus result = IZ_TRANSFER_INTERRUPTED;
    int file = -1;
    unsigned file_index = NUP_PART_LIMIT;
    if (!position) {
        while (buffered < 128 && !interrupted(transfer, selection)) {
            const ssize_t got = receive(transfer, client, buffer + buffered, 128 - buffered,
                                        monotonic_ms() + 20000, selection);
            if (got <= 0) break;
            buffered += (size_t)got;
        }
        if (buffered < 128 || interrupted(transfer, selection)) goto done;
        if (memcmp(buffer, "NUP\1", 4) || iz_le64(buffer + 64) != upload->size) {
            result = IZ_TRANSFER_INVALID;
            goto done;
        }
        if (job == JOB_NEW) {
            result = prepare_job(transfer, upload, out);
            if (result != IZ_TRANSFER_READY) goto done;
        }
    }

    result = IZ_TRANSFER_INTERRUPTED;
    const uint64_t step = out->saved.part_size ? out->saved.part_size : upload->size;
    while (position < upload->size) {
        const uint64_t left = upload->size - position;
        const size_t want = left < CHUNK ? (size_t)left : CHUNK;
        while (buffered < want && !interrupted(transfer, selection)) {
            const ssize_t got = receive(transfer, client, buffer + buffered, want - buffered,
                                        monotonic_ms() + 20000, selection);
            if (got <= 0) break;
            buffered += (size_t)got;
        }
        size_t written = 0;
        while (written < buffered) {
            const unsigned index = (unsigned)(position / step);
            const uint64_t at = position % step;
            if (file < 0 || file_index != index) {
                if (!flush_part(&file)) goto disk_error;
                file = reserve(out, index);
                if (file < 0 || lseek(file, (off_t)at, SEEK_SET) < 0) goto disk_error;
                file_index = index;
            }
            size_t amount = buffered - written;
            if (step - at < amount) amount = (size_t)(step - at);
            const uint64_t until_checkpoint = CHECKPOINT - position % CHECKPOINT;
            if (until_checkpoint < amount) amount = (size_t)until_checkpoint;
            const ssize_t stored = write(file, buffer + written, amount);
            if (stored < 0 && errno == EINTR) continue;
            if (stored <= 0) goto disk_error;
            written += (size_t)stored;
            position += (size_t)stored;
            if (position % CHECKPOINT == 0 && !checkpoint(out, &file, position)) goto disk_error;
        }
        atomic_store_explicit(&transfer->received, position, memory_order_relaxed);
        if (buffered < want || interrupted(transfer, selection)) break;
        buffered = 0;
    }

    /* Preserve a final partial checkpoint on an ordinary disconnect or stop. */
    if (!checkpoint(out, &file, position) || !flush_part(&file)) goto disk_error;
    if (position < upload->size || interrupted(transfer, selection)) goto done;
    NupPreview preview;
    if (!nup_preview((NupSource){.user = out, .read = output_read, .size = upload->size}, &preview) ||
        preview.incomplete) {
        result = IZ_TRANSFER_INVALID;
        remove_job(out);
    } else if (interrupted(transfer, selection)) {
        goto done;
    } else if (publish(out)) {
        result = out->saved.part_size ? IZ_TRANSFER_SPLIT : IZ_TRANSFER_DONE;
        remove_job(out);
        atomic_fetch_add(&transfer->completed, 1);
    } else {
        result = path_available(out->target) ? IZ_TRANSFER_DISK_ERROR : IZ_TRANSFER_EXISTS;
    }
    goto done;

disk_error:
    result = errno == ENOSPC ? IZ_TRANSFER_NO_SPACE : IZ_TRANSFER_DISK_ERROR;
done:
    if (file >= 0) close(file);
    atomic_store(&transfer->received, out->offset);
    update_pending(transfer, NULL);
    free(buffer);
    return result;
}

static void upload_reply(int client, IzTransferStatus status) {
    const char *code = "500 Internal Server Error", *message = "Could not write the SD card.";
    switch (status) {
        case IZ_TRANSFER_DONE: code = "201 Created"; message = "File saved to SD:/Games/."; break;
        case IZ_TRANSFER_SPLIT: code = "201 Created"; message = "Split NUP saved to SD:/Games/."; break;
        case IZ_TRANSFER_EXISTS: code = "409 Conflict"; message = "A file or folder with this name already exists."; break;
        case IZ_TRANSFER_INVALID: code = "422 Unprocessable Content"; message = "Invalid or incomplete NUP."; break;
        case IZ_TRANSFER_INTERRUPTED: code = "409 Conflict"; message = "Transfer paused. Select the same file to resume."; break;
        case IZ_TRANSFER_CANCELLED: code = "409 Conflict"; message = "Transfer cancelled. Partial data removed."; break;
        case IZ_TRANSFER_RESUME_ERROR: code = "409 Conflict"; message = "Saved transfer could not be verified. Discard it to start over."; break;
        case IZ_TRANSFER_NO_SPACE: code = "507 Insufficient Storage"; message = "Not enough free space on the SD card."; break;
        default: break;
    }
    respond(client, code, "text/plain", message);
}

static void handle_request(IzTransfer *transfer, SelectedJob *selection, int client) {
    char header[HEADER_CAP + 1];
    size_t have = 0, header_size = 0;
    const uint64_t deadline = monotonic_ms() + 8000;
    while (have < HEADER_CAP) {
        const ssize_t bytes = receive(transfer, client, header + have, HEADER_CAP - have, deadline, 0);
        if (bytes <= 0) return;
        have += (size_t)bytes;
        header[have] = 0;
        const char *end = strstr(header, "\r\n\r\n");
        if (end) {
            header_size = (size_t)(end - header) + 4;
            break;
        }
        if (memchr(header, 0, have)) break;
    }
    char *line_end = header_size ? strstr(header, "\r\n") : NULL;
    if (!line_end) {
        respond(client, "400 Bad Request", "text/plain", "Invalid HTTP headers.");
        return;
    }
    *line_end = 0;
    if (!strcmp(header, "GET / HTTP/1.1") || !strcmp(header, "GET / HTTP/1.0")) {
        respond(client, "200 OK", "text/html; charset=utf-8", page);
        return;
    }
    if (transfer->logo_size && (!strcmp(header, "GET /brand.png HTTP/1.1") ||
                                !strcmp(header, "GET /brand.png HTTP/1.0"))) {
        respond_bytes(client, "200 OK", "image/png", (const char *)transfer->logo, transfer->logo_size);
        return;
    }
    const bool sending = !strcmp(header, "POST /upload HTTP/1.1") || !strcmp(header, "POST /upload HTTP/1.0");
    const bool lookup = !strcmp(header, "POST /resume HTTP/1.1") || !strcmp(header, "POST /resume HTTP/1.0");
    const bool discard = !strcmp(header, "POST /discard HTTP/1.1") || !strcmp(header, "POST /discard HTTP/1.0");
    if (!sending && !lookup && !discard) {
        respond(client, "404 Not Found", "text/plain", "Unknown path.");
        return;
    }
    Upload upload = {0};
    const size_t initial = have - header_size;
    if (!parse_upload(line_end + 2, &upload, sending) || initial > upload.length) {
        respond(client, "400 Bad Request", "text/plain", "Select an original .nup file.");
        return;
    }
    if (transfer->attempts >= 8) {
        respond(client, "403 Forbidden", "text/plain", "Stop and reopen Transfers on the console for a new code.");
        return;
    }
    if (!upload.code || strcmp(upload.code, transfer->code)) {
        if (++transfer->attempts >= 8) atomic_store(&transfer->status, IZ_TRANSFER_LOCKED);
        respond(client, "403 Forbidden", "text/plain", "Incorrect transfer code.");
        return;
    }
    transfer->attempts = 0;
    if (upload.size > IZ_TRANSFER_AUTO_PART * NUP_PART_LIMIT) {
        respond(client, "413 Content Too Large", "text/plain", "This file would need too many parts.");
        return;
    }
    Output out = {0};
    if (discard) {
        if (stage_path(&out, transfer->directory, upload.name, upload.size, upload.key) &&
            cancel_job(transfer, selection, &out) == IZ_TRANSFER_CANCELLED) {
            respond(client, "200 OK", "application/json", "{}");
        } else {
            upload_reply(client, IZ_TRANSFER_DISK_ERROR);
        }
        return;
    }
    const JobState job = load_job(&out, &upload, transfer->directory);
    if (job == JOB_UNREADABLE) {
        upload_reply(client, IZ_TRANSFER_RESUME_ERROR);
        return;
    }
    if (job == JOB_SAVED && !path_available(out.target)) {
        upload_reply(client, IZ_TRANSFER_EXISTS);
        return;
    }
    if (lookup) {
        char body[96];
        snprintf(body, sizeof(body), "{\"offset\":%" PRIu64 ",\"tail\":\"%08" PRIx32 "\"}", out.offset, out.tail);
        respond(client, "200 OK", "application/json", body);
        return;
    }
    if (job == JOB_NEW && transfer->part_size && upload.size > transfer->part_size * NUP_PART_LIMIT) {
        respond(client, "413 Content Too Large", "text/plain", "This file would need too many parts with the configured size.");
        return;
    }
    if (upload.offset != out.offset) {
        respond(client, "409 Conflict", "text/plain", "The saved position changed. Select the same file to resume.");
        return;
    }
    const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (upload.expect && !send_all(client, interim, sizeof(interim) - 1)) return;
    set_filename(transfer, upload.name);
    atomic_store(&transfer->received, out.offset);
    atomic_store(&transfer->total, upload.size);
    select_job(transfer, selection, out.staging);
    atomic_store(&transfer->status, IZ_TRANSFER_RECEIVING);
    IzTransferStatus result = write_upload(transfer, client, &upload, &out, job, selection->id,
                                          (const unsigned char *)header + header_size, initial);
    const IzTransferCommand command = take_command(transfer, selection);
    if (result == IZ_TRANSFER_DONE || result == IZ_TRANSFER_SPLIT) select_job(transfer, selection, "");
    else if (command == IZ_TRANSFER_CANCEL) result = cancel_job(transfer, selection, &out);
    else if (path_available(out.staging)) select_job(transfer, selection, "");
    atomic_store(&transfer->status, result);
    if (!stopped(transfer)) upload_reply(client, result);
}

static void close_client(const IzTransfer *transfer, int client) {
    if (!shutdown(client, SHUT_WR)) {
        // Give the client time to read an early error while its upload is still in flight.
        char buffer[2048];
        const uint64_t deadline = monotonic_ms() + 200;
        for (unsigned i = 0; i < 16; ++i)
            if (receive(transfer, client, buffer, sizeof(buffer), deadline, 0) <= 0) break;
    }
    close(client);
}

bool iz_transfer_open(IzTransfer *transfer, const char *directory, uint32_t address, uint16_t port,
                      unsigned split_mib, uint64_t random) {
    if (!address || split_mib > IZ_SPLIT_MAX_MIB) return false;
    struct stat status;
    if (mkdir(directory, 0777) && (errno != EEXIST || stat(directory, &status) || !S_ISDIR(status.st_mode)))
        return false;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int reuse = 1, receive_buffer = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    const struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = address};
    if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) || listen(fd, 2)) {
        close(fd);
        return false;
    }
    transfer->directory = directory;
    transfer->logo = NULL;
    transfer->logo_size = 0;
    transfer->listener = fd;
    transfer->part_size = (uint64_t)split_mib << 20;
    transfer->attempts = 0;
    snprintf(transfer->code, sizeof(transfer->code), "%08u", (unsigned)(random % 100000000));
    atomic_init(&transfer->stop, false);
    atomic_init(&transfer->status, IZ_TRANSFER_READY);
    atomic_init(&transfer->received, 0);
    atomic_init(&transfer->total, 0);
    atomic_init(&transfer->pending, 0);
    atomic_init(&transfer->completed, 0);
    atomic_init(&transfer->job, 0);
    atomic_init(&transfer->command, 0);
    for (unsigned i = 0; i < NAME_CAP; ++i) atomic_init(&transfer->filename[i], 0);
    return true;
}

void iz_transfer_run(void *arg) {
    IzTransfer *transfer = arg;
    SelectedJob selection = {0};
    update_pending(transfer, &selection);
    struct pollfd listener = {.fd = transfer->listener, .events = POLLIN};
    while (!stopped(transfer)) {
        if (take_command(transfer, &selection) == IZ_TRANSFER_CANCEL) {
            Output out = {0};
            strcpy(out.staging, selection.staging);
            atomic_store(&transfer->status, cancel_job(transfer, &selection, &out));
        }
        const int ready = poll(&listener, 1, 150);
        if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
        if (ready < 0 || !(listener.revents & POLLIN)) break;
        const int client = accept(listener.fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        const struct timeval timeout = {.tv_sec = 2};
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle_request(transfer, &selection, client);
        close_client(transfer, client);
    }
    if (!stopped(transfer)) atomic_store(&transfer->status, IZ_TRANSFER_NETWORK_ERROR);
}

void iz_transfer_close(IzTransfer *transfer) {
    close(transfer->listener);
}

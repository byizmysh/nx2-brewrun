#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "transfer.h"
#include "nup_parts.h"
#include "settings.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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

enum { NAME_CAP = 201, PATH_CAP = 512, STAGING_CAP = 280, HEADER_CAP = 8192, CHUNK = 128 * 1024 };

typedef struct {
    char name[NAME_CAP];
    uint64_t size;
    char *code;
    bool expect;
} Upload;

typedef struct {
    char staging[STAGING_CAP], target[PATH_CAP];
    const char *name;
    unsigned files;
    uint64_t part_size;
    bool split;
} Output;

static const char page[] =
    "<!doctype html><html lang=en><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NX2 BreRun Transfer</title><style>"
    "body{font:1rem system-ui;background:#0d1421;color:#f4f7fc;margin:10vh auto;padding:1rem;max-width:36rem}"
    "form{background:#1b2940;border-radius:1rem;padding:1.5rem}label,progress{display:block;margin:1rem 0}"
    "button{background:#8ce8d7;color:#0d1421;border:0;border-radius:.5rem;padding:.7rem 1.2rem;font:inherit}"
    "input{max-width:100%}progress{width:100%;accent-color:#8ce8d7}</style>"
    "<h1>NX2 BreRun Transfer</h1><p>Send a NUP file to the console's SD card.</p>"
    "<form id=f><label>NUP file <input id=n type=file accept='.nup' required></label>"
    "<label>Code shown on the console <input id=c inputmode=numeric autocomplete=off pattern='[0-9]{8}' required></label>"
    "<button id=b>Send file</button><progress id=p max=1 value=0></progress><output id=s></output></form>"
    "<script>const f=document.querySelector('#f'),n=f.querySelector('#n'),c=f.querySelector('#c'),"
    "b=f.querySelector('#b'),p=f.querySelector('#p'),s=f.querySelector('#s');"
    "f.onsubmit=e=>{e.preventDefault();let file=n.files[0],x=new XMLHttpRequest();"
    "x.open('POST','/upload');x.setRequestHeader('X-BrewRun-Code',c.value);"
    "x.setRequestHeader('X-BrewRun-Name',encodeURIComponent(file.name));"
    "x.upload.onprogress=e=>{p.value=e.loaded/file.size;if(e.loaded===file.size)s.textContent='Checking file on SD card…'};"
    "x.onloadend=()=>{s.textContent=x.responseText||'Connection closed.';b.disabled=false};"
    "p.value=0;s.textContent='Sending…';b.disabled=true;x.send(file)};</script></html>";

static bool stopped(const IzTransfer *transfer) {
    return atomic_load_explicit(&transfer->stop, memory_order_relaxed);
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

static bool content_length(const char *text, uint64_t *value) {
    if (!*text) return false;
    *value = 0;
    while (*text) {
        const unsigned digit = (unsigned)(*text++ - '0');
        if (digit > 9 || *value > (INT64_MAX - digit) / UINT64_C(10)) return false;
        *value = 10 * *value + digit;
    }
    return *value >= 128;
}

static bool parse_upload(char *fields, Upload *upload) {
    bool length = false, named = false;
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
            if (length || !content_length(value, &upload->size)) return false;
            length = true;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            return false;
        } else if (!strcasecmp(line, "X-BrewRun-Code")) {
            if (upload->code) return false;
            upload->code = value;
        } else if (!strcasecmp(line, "X-BrewRun-Name")) {
            if (named || !decode_name(value, upload->name)) return false;
            named = true;
        } else if (!strcasecmp(line, "Expect")) {
            if (upload->expect || strcasecmp(value, "100-continue")) return false;
            upload->expect = true;
        }
        line = end + 2;
    }
    return length && named;
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static ssize_t receive(const IzTransfer *transfer, int client, void *data, size_t size, uint64_t deadline) {
    struct pollfd input = {.fd = client, .events = POLLIN};
    while (!stopped(transfer) && monotonic_ms() < deadline) {
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

static void respond(int client, const char *status, const char *type, const char *body) {
    char header[240];
    const int bytes = snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n", status, type, strlen(body));
    if (bytes > 0 && (size_t)bytes < sizeof(header) && send_all(client, header, bytes))
        send_all(client, body, strlen(body));
}

static bool path_available(const char *path) {
    struct stat st;
#ifdef __SWITCH__
    return stat(path, &st) != 0 && errno == ENOENT;
#else
    return lstat(path, &st) != 0 && errno == ENOENT;
#endif
}

static void staged_path(const Output *out, unsigned part, char path[PATH_CAP]) {
    if (out->split)
        snprintf(path, PATH_CAP, "%s/%03u", out->staging, part + 1);
    else
        snprintf(path, PATH_CAP, "%s/original", out->staging);
}

static bool set_output_paths(Output *out, const char *directory) {
    const int target = snprintf(out->target, sizeof(out->target), "%s/%s", directory, out->name);
    const int staging = snprintf(out->staging, sizeof(out->staging), "%s/.brerun-upload-XXXXXX", directory);
    return target > 0 && target < (int)sizeof(out->target) &&
        staging > 0 && staging < (int)sizeof(out->staging);
}

static bool publish(const Output *out) {
    if (!path_available(out->target)) return false;
    char original[PATH_CAP];
    staged_path(out, 0, original);
    const char *source = out->split ? out->staging : original;
#ifdef __SWITCH__
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd || strncmp(source, "sdmc:/", 6) || strncmp(out->target, "sdmc:/", 6)) return false;
    return R_SUCCEEDED(out->split ? fsFsRenameDirectory(sd, source + 5, out->target + 5) :
                                    fsFsRenameFile(sd, source + 5, out->target + 5));
#else
    return rename(source, out->target) == 0;
#endif
}

static int reserve(Output *out, uint64_t size) {
    char path[PATH_CAP];
    staged_path(out, out->files, path);
    const int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (file < 0) return -1;
    if (size > 0 && ftruncate(file, (off_t)size)) {
        const int error = errno;
        close(file);
        unlink(path);
        errno = error;
        return -1;
    }
    ++out->files;
    return file;
}

static bool output_read(void *user, uint64_t offset, void *data, size_t size) {
    const Output *out = user;
    unsigned char *dst = data;
    while (size) {
        const unsigned index = out->split ? (unsigned)(offset / out->part_size) : 0;
        const uint64_t at = out->split ? offset % out->part_size : offset;
        if (index >= out->files) return false;
        uint64_t available = out->split ? out->part_size - at : size;
        const size_t chunk = available < size ? (size_t)available : size;
        char path[PATH_CAP];
        staged_path(out, index, path);
        const int fd = open(path, O_RDONLY);
        if (fd < 0) return false;
        const ssize_t got = lseek(fd, (off_t)at, SEEK_SET) < 0 ? -1 : read(fd, dst, chunk);
        close(fd);
        if (got != (ssize_t)chunk) return false;
        size -= chunk;
        dst += chunk;
        offset += chunk;
    }
    return true;
}

static IzTransferStatus write_upload(IzTransfer *transfer, int client, const Upload *upload,
                                     Output *out, const unsigned char *initial, size_t initial_size) {
    unsigned char *buffer = malloc(CHUNK);
    if (!buffer) return IZ_TRANSFER_DISK_ERROR;
    memcpy(buffer, initial, initial_size);
    size_t buffered = initial_size;
    while (buffered < 128) {
        const ssize_t got = receive(transfer, client, buffer + buffered, 128 - buffered, monotonic_ms() + 20000);
        if (got <= 0) break;
        buffered += (size_t)got;
    }
    IzTransferStatus result = IZ_TRANSFER_INTERRUPTED;
    int file = -1;
    bool staging = false;
    if (buffered < 128 || stopped(transfer)) goto done;
    result = IZ_TRANSFER_INVALID;
    if (buffered > upload->size || memcmp(buffer, "NUP\1", 4) || iz_le64(buffer + 64) != upload->size) goto done;

    result = IZ_TRANSFER_DISK_ERROR;
    staging = mkdtemp(out->staging) != NULL;
    if (!staging) goto done;
    uint64_t part_size = transfer->part_size;
    uint64_t remaining = upload->size, room = remaining;
    if (!part_size || remaining <= part_size) {
        file = reserve(out, remaining);
        if (file < 0) {
            if (remaining <= IZ_TRANSFER_WHOLE_LIMIT || part_size) goto done;
            part_size = IZ_TRANSFER_AUTO_PART;
        }
    }
    if (file < 0) {
        if (!part_size || upload->size > part_size * NUP_PART_LIMIT) goto done;
        char *stem_end = out->target + strlen(out->target) - (sizeof(".nup") - 1);
        // The original name has a visible stem. Remove any trailing spaces or dots for FAT32.
        while (stem_end[-1] == ' ' || stem_end[-1] == '.') --stem_end;
        *stem_end = 0;
        if (!path_available(out->target)) {
            result = IZ_TRANSFER_EXISTS;
            goto done;
        }
        room = remaining < part_size ? remaining : part_size;
        out->part_size = part_size;
        out->split = true;
        file = reserve(out, room);
        if (file < 0) goto done;
    }
    while (remaining && !stopped(transfer)) {
        if (!buffered) {
            const size_t want = remaining < CHUNK ? (size_t)remaining : CHUNK;
            const ssize_t got = receive(transfer, client, buffer, want, monotonic_ms() + 20000);
            if (got <= 0) {
                result = IZ_TRANSFER_INTERRUPTED;
                goto done;
            }
            buffered = (size_t)got;
        }
        size_t written = 0;
        while (written < buffered && !stopped(transfer)) {
            if (!room) {
                const int closed = close(file);
                file = -1;
                if (closed) goto done;
                room = remaining < part_size ? remaining : part_size;
                file = reserve(out, room);
                if (file < 0) goto done;
            }
            size_t chunk = buffered - written;
            if (chunk > room) chunk = (size_t)room;
            const ssize_t stored = write(file, buffer + written, chunk);
            if (stored < 0 && errno == EINTR) continue;
            if (stored <= 0) goto done;
            written += (size_t)stored;
            room -= (size_t)stored;
            remaining -= (size_t)stored;
        }
        atomic_store_explicit(&transfer->received, upload->size - remaining, memory_order_relaxed);
        buffered = 0;
    }
    if (stopped(transfer)) {
        result = IZ_TRANSFER_INTERRUPTED;
        goto done;
    }
    const int closed = close(file);
    file = -1;
    if (closed) goto done;
    NupPreview preview;
    if (!nup_preview((NupSource){.user = out, .read = output_read, .size = upload->size}, &preview) ||
        preview.incomplete) {
        result = IZ_TRANSFER_INVALID;
        goto done;
    }
    if (!stopped(transfer) && publish(out)) {
        result = out->split ? IZ_TRANSFER_SPLIT : IZ_TRANSFER_DONE;
        if (!out->split) rmdir(out->staging);
        staging = false;
    } else if (!path_available(out->target)) {
        result = IZ_TRANSFER_EXISTS;
    }

done:
    if (file >= 0) close(file);
    if (staging) {
        for (unsigned i = 0; i < out->files; ++i) {
            char path[PATH_CAP];
            staged_path(out, i, path);
            unlink(path);
        }
        rmdir(out->staging);
    }
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
        case IZ_TRANSFER_INTERRUPTED: code = "400 Bad Request"; message = "Transfer interrupted."; break;
        case IZ_TRANSFER_NO_SPACE: code = "507 Insufficient Storage"; message = "Not enough free space on the SD card."; break;
        default: break;
    }
    respond(client, code, "text/plain", message);
}

static void handle_request(IzTransfer *transfer, int client) {
    char header[HEADER_CAP + 1];
    size_t have = 0, header_size = 0;
    const uint64_t deadline = monotonic_ms() + 8000;
    while (have < HEADER_CAP) {
        const ssize_t bytes = receive(transfer, client, header + have, HEADER_CAP - have, deadline);
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
        respond(client, "200 OK", "text/html", page);
        return;
    }
    if (strcmp(header, "POST /upload HTTP/1.1") && strcmp(header, "POST /upload HTTP/1.0")) {
        respond(client, "404 Not Found", "text/plain", "Unknown path.");
        return;
    }
    Upload upload = {0};
    if (!parse_upload(line_end + 2, &upload)) {
        respond(client, "400 Bad Request", "text/plain", "Select an original .nup file.");
        return;
    }
    const int previous = atomic_load(&transfer->status);
    if (previous == IZ_TRANSFER_DONE || previous == IZ_TRANSFER_SPLIT || previous == IZ_TRANSFER_LOCKED) {
        respond(client, "403 Forbidden", "text/plain", "Open Transfer again on the console for a new code.");
        return;
    }
    if (!upload.code || strcmp(upload.code, transfer->code)) {
        if (++transfer->attempts >= 8) atomic_store(&transfer->status, IZ_TRANSFER_LOCKED);
        respond(client, "403 Forbidden", "text/plain", "Incorrect transfer code.");
        return;
    }
    const uint64_t part_size = transfer->part_size ? transfer->part_size : IZ_TRANSFER_AUTO_PART;
    if (upload.size > part_size * NUP_PART_LIMIT) {
        respond(client, "413 Content Too Large", "text/plain", "This file would need too many parts.");
        return;
    }
    Output out = {.name = upload.name};
    IzTransferStatus result = IZ_TRANSFER_DISK_ERROR;
    if (set_output_paths(&out, transfer->directory)) {
        if (!path_available(out.target)) {
            result = IZ_TRANSFER_EXISTS;
        } else {
            struct statvfs space;
            if (!statvfs(transfer->directory, &space) && space.f_frsize &&
                (upload.size - 1) / space.f_frsize >= space.f_bavail) {
                result = IZ_TRANSFER_NO_SPACE;
            } else {
                const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
                if (upload.expect && !send_all(client, interim, sizeof(interim) - 1)) return;
                atomic_store(&transfer->received, 0);
                atomic_store(&transfer->total, upload.size);
                atomic_store(&transfer->status, IZ_TRANSFER_RECEIVING);
                result = write_upload(transfer, client, &upload, &out,
                                      (const unsigned char *)header + header_size, have - header_size);
            }
        }
    }
    atomic_store(&transfer->status, result);
    if (!stopped(transfer)) upload_reply(client, result);
}

static void close_client(const IzTransfer *transfer, int client) {
    if (!shutdown(client, SHUT_WR)) {
        // Give the client time to read an early error while its upload is still in flight.
        char buffer[2048];
        const uint64_t deadline = monotonic_ms() + 200;
        for (unsigned i = 0; i < 16; ++i)
            if (receive(transfer, client, buffer, sizeof(buffer), deadline) <= 0) break;
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
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = address};
    if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) || listen(fd, 2)) {
        close(fd);
        return false;
    }
    transfer->directory = directory;
    transfer->listener = fd;
    transfer->part_size = (uint64_t)split_mib << 20;
    transfer->attempts = 0;
    snprintf(transfer->code, sizeof(transfer->code), "%08u", (unsigned)(random % 100000000));
    atomic_init(&transfer->stop, false);
    atomic_init(&transfer->status, IZ_TRANSFER_READY);
    atomic_init(&transfer->received, 0);
    atomic_init(&transfer->total, 0);
    return true;
}

void iz_transfer_run(void *arg) {
    IzTransfer *transfer = arg;
    struct pollfd listener = {.fd = transfer->listener, .events = POLLIN};
    while (!stopped(transfer)) {
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
        handle_request(transfer, client);
        close_client(transfer, client);
    }
    if (!stopped(transfer)) atomic_store(&transfer->status, IZ_TRANSFER_NETWORK_ERROR);
}

void iz_transfer_close(IzTransfer *transfer) {
    close(transfer->listener);
}

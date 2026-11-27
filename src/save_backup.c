#include "save_backup.h"
#include "settings.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { PATH_SIZE = 512, BUFFER_SIZE = 64 * 1024, MAX_DEPTH = 64 };

static bool join(char out[PATH_SIZE], const char *parent, const char *name) {
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || name[strcspn(name, "/\\:")])
        return false;
    const int n = snprintf(out, PATH_SIZE, "%s/%s", parent, name);
    return n > 0 && n < PATH_SIZE;
}

static bool copy_file(const char *source, const char *target, unsigned char *buffer) {
    FILE *in = fopen(source, "rb");
    if (!in) return false;
    FILE *out = fopen(target, "wb");
    bool ok = out != NULL;
    while (ok) {
        const size_t count = fread(buffer, 1, BUFFER_SIZE, in);
        ok = !ferror(in) && fwrite(buffer, 1, count, out) == count;
        if (count < BUFFER_SIZE) break;
    }
    if (out) {
        if (ok) ok = !fflush(out) && !fsync(fileno(out));
        if (fclose(out)) ok = false;
    }
    fclose(in);
    return ok;
}

/* A missing target means cleaning up this attempt's temporary directory. */
static bool walk(const char *source, const char *target, unsigned char *buffer, unsigned depth, bool *has_files) {
    if (!depth) return false;
    DIR *dir = opendir(source);
    if (!dir) return false;
    bool ok = true;
    while (ok) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char from[PATH_SIZE], to[PATH_SIZE];
        struct stat status;
        if (!join(from, source, entry->d_name) || lstat(from, &status) ||
            (target && !join(to, target, entry->d_name))) {
            ok = false;
            break;
        }
        if (S_ISDIR(status.st_mode)) {
            ok = target ? depth > 1 && !mkdir(to, 0777) && walk(from, to, buffer, depth - 1, has_files) :
                walk(from, NULL, NULL, depth - 1, NULL) && !rmdir(from);
        } else if (target) {
            ok = S_ISREG(status.st_mode) && copy_file(from, to, buffer);
            if (ok) *has_files = true;
        } else ok = !unlink(from);
    }
    if (closedir(dir)) ok = false;
    return ok;
}

IzSaveBackupResult iz_backup_saves(const char *self, uint64_t title, char label[IZ_SAVE_BACKUP_LABEL_SIZE]) {
    char source[PATH_SIZE], folder[PATH_SIZE], target[PATH_SIZE], stage[PATH_SIZE];
    struct stat st;
    label[0] = '\0';
    if (!iz_game_save_path(self, title, source, sizeof(source))) return IZ_SAVE_BACKUP_ERROR;
    if (lstat(source, &st)) return errno == ENOENT ? IZ_SAVE_BACKUP_EMPTY : IZ_SAVE_BACKUP_ERROR;
    if (!S_ISDIR(st.st_mode) ||
        !iz_game_data_path(self, title, folder, sizeof(folder) - sizeof("/backups") + 1)) return IZ_SAVE_BACKUP_ERROR;
    strcat(folder, "/backups");
    if (mkdir(folder, 0777) && (errno != EEXIST || lstat(folder, &st) || !S_ISDIR(st.st_mode)))
        return IZ_SAVE_BACKUP_ERROR;

    const time_t now = time(NULL);
    struct tm utc;
    char date[24];
    if (!gmtime_r(&now, &utc) || !strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &utc)) return IZ_SAVE_BACKUP_ERROR;
    for (unsigned i = 0; i < 1000; ++i) {
        char name[IZ_SAVE_BACKUP_LABEL_SIZE], pending[48];
        snprintf(name, sizeof(name), "%s-%03u", date, i);
        snprintf(pending, sizeof(pending), ".pending-%s", name);
        if (!join(target, folder, name) || !join(stage, folder, pending)) return IZ_SAVE_BACKUP_ERROR;
        if (mkdir(stage, 0777)) {
            if (errno == EEXIST) continue;
            return IZ_SAVE_BACKUP_ERROR;
        }
        if (!lstat(target, &st)) {
            rmdir(stage);
            continue;
        }
        if (errno != ENOENT) {
            rmdir(stage);
            return IZ_SAVE_BACKUP_ERROR;
        }
        unsigned char *buffer = malloc(BUFFER_SIZE);
        bool has_files = false;
        const bool copied = buffer && walk(source, stage, buffer, MAX_DEPTH, &has_files);
        free(buffer);
        if (copied && has_files && !rename(stage, target)) {
            strcpy(label, name);
            return IZ_SAVE_BACKUP_DONE;
        }
        if (walk(stage, NULL, NULL, MAX_DEPTH, NULL)) rmdir(stage);
        return copied && !has_files ? IZ_SAVE_BACKUP_EMPTY : IZ_SAVE_BACKUP_ERROR;
    }
    return IZ_SAVE_BACKUP_ERROR;
}

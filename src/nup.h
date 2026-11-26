#ifndef IZ_NUP_H
#define IZ_NUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NUP_NONE UINT32_MAX
#define NUP_NAME_LIMIT 255
#define NUP_TOC_LIMIT (48u << 20)
#define NUP_NODE_LIMIT 500000
#define NUP_PACKAGE_LIMIT 128

typedef bool (*NupRead)(void *, uint64_t, void *, size_t);
typedef bool (*NupHash)(const void *, size_t, const unsigned char *);
typedef struct {
    NupRead read;
    NupHash hash;
    void *user;
    uint64_t size;
} NupSource;

typedef struct {
    const char *name;
    uint32_t name_size;
    uint64_t offset, stored, size;
    const unsigned char *hash;
    unsigned codec;
} NupFile;

typedef struct {
    uint64_t id, meta_at;
    uint32_t root, exefs, romfs, version, meta_size;
    unsigned kind;
} NupPackage;

typedef struct {
    NupSource source;
    unsigned char *table;
    size_t table_size, files_at, children_at, children_count;
    const char *names;
    size_t names_size;
    uint32_t package_count, dir_count, file_count;
    uint64_t data_at;
    NupPackage packages[NUP_PACKAGE_LIMIT];
} Nup;

typedef struct {
    const char *name;
    uint32_t name_size, first, count;
} NupDir;

typedef struct {
    uint64_t size, title_id, cover_offset;
    uint32_t cover_size;
    char title[128];
    bool split;
} NupPreview;

uint32_t iz_le32(const void *);
uint64_t iz_le64(const void *);
void iz_put32(void *, uint32_t);
void iz_put64(void *, uint64_t);
bool iz_span(uint64_t offset, uint64_t bytes, uint64_t size);

bool nup_open(Nup *, NupSource, const char **error);
bool nup_preview(NupSource, NupPreview *);
void nup_close(Nup *);
bool nup_dir(const Nup *, uint32_t, NupDir *);
bool nup_file(const Nup *, uint32_t, NupFile *);
uint32_t nup_child(const Nup *, uint32_t);
uint32_t nup_find_file(const Nup *, uint32_t, const char *);
size_t nup_layers(const Nup *, const NupPackage **);
bool nup_read_file(const Nup *, const NupFile *, void *, size_t);

#endif

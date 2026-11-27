#ifndef IZ_NSO_H
#define IZ_NSO_H

#include "nup.h"
#include "nx2.h"

#define IZ_WHOLE_CAPACITY 7
enum { IZ_NSO_PAGE = 0x1000 };

static inline uint64_t iz_nso_round_pages(uint64_t bytes) {
    return (bytes + IZ_NSO_PAGE - 1) & ~(uint64_t)(IZ_NSO_PAGE - 1);
}

static inline uint64_t iz_nso_mapping_bytes(const Nx2Module *module, const Nx2Image *image) {
    const uint64_t initialized = iz_nso_round_pages(image->image_size);
    return (module->end > initialized ? module->end : initialized) + 2 * IZ_NSO_PAGE;
}

bool iz_nso_prepare(const Nup *, const NupFile *, const char *, Nx2Module *, Nx2Image *,
                    const char **error);
void iz_nso_close(Nx2Image *);

#endif

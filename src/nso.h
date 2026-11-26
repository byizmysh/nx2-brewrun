#ifndef IZ_NSO_H
#define IZ_NSO_H

#include "nup.h"
#include "nx2.h"

#define IZ_WHOLE_CAPACITY 7

bool iz_nso_prepare(const Nup *, const NupFile *, const char *, Nx2Module *, Nx2Image *,
                    const char **error);
void iz_nso_close(Nx2Image *);

#endif

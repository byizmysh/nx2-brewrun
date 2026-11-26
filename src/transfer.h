#ifndef IZ_TRANSFER_H
#define IZ_TRANSFER_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

enum { IZ_TRANSFER_PORT = 28303 };

typedef enum {
    IZ_TRANSFER_READY, IZ_TRANSFER_RECEIVING, IZ_TRANSFER_DONE, IZ_TRANSFER_SPLIT,
    IZ_TRANSFER_INTERRUPTED, IZ_TRANSFER_INVALID, IZ_TRANSFER_EXISTS, IZ_TRANSFER_NO_SPACE,
    IZ_TRANSFER_DISK_ERROR, IZ_TRANSFER_LOCKED, IZ_TRANSFER_NETWORK_ERROR
} IzTransferStatus;

typedef struct {
    const char *directory;
    int listener;
    uint64_t part_size;
    char code[9];
    unsigned attempts;
    atomic_bool stop;
    atomic_int status;
    _Atomic uint64_t received, total;
} IzTransfer;

bool iz_transfer_open(IzTransfer *, const char *directory, uint32_t address, uint16_t port,
                      unsigned split_mib, uint64_t random);
void iz_transfer_run(void *);
void iz_transfer_close(IzTransfer *);

#endif

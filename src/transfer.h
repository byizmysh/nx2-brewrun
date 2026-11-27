#ifndef IZ_TRANSFER_H
#define IZ_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

enum { IZ_TRANSFER_PORT = 38617, IZ_TRANSFER_NAME_CAP = 201 };

typedef enum {
    IZ_TRANSFER_READY, IZ_TRANSFER_RECEIVING, IZ_TRANSFER_DONE, IZ_TRANSFER_SPLIT,
    IZ_TRANSFER_INTERRUPTED, IZ_TRANSFER_INVALID, IZ_TRANSFER_EXISTS, IZ_TRANSFER_NO_SPACE,
    IZ_TRANSFER_DISK_ERROR, IZ_TRANSFER_LOCKED, IZ_TRANSFER_NETWORK_ERROR, IZ_TRANSFER_RESUME_ERROR,
    IZ_TRANSFER_CANCELLED
} IzTransferStatus;

typedef enum { IZ_TRANSFER_PAUSE = 1, IZ_TRANSFER_CANCEL } IzTransferCommand;

typedef struct {
    const char *directory;
    const unsigned char *logo;
    size_t logo_size;
    int listener;
    uint64_t part_size;
    char code[9];
    unsigned attempts;
    atomic_bool stop;
    atomic_int status;
    _Atomic uint64_t received, total;
    atomic_uint pending, completed;
    atomic_uint job, command;
    atomic_uchar filename[IZ_TRANSFER_NAME_CAP];
} IzTransfer;

bool iz_transfer_open(IzTransfer *, const char *directory, uint32_t address, uint16_t port,
                      unsigned split_mib, uint64_t random);
void iz_transfer_run(void *);
void iz_transfer_close(IzTransfer *);
void iz_transfer_filename(const IzTransfer *, char name[IZ_TRANSFER_NAME_CAP]);
void iz_transfer_request(IzTransfer *, unsigned job, IzTransferCommand command);

#endif

#include "rom_io.h"

#include <string.h>

static u32 read_lock;

Result iz_read_rom_file(const FsFile *file, s64 offset, void *buffer, size_t size, u64 *read) {
    *read = 0;
    if (!file->s.session || offset < 0 || (size && !buffer)) return 0x2EE202;
    if (!size) return 0;

    const struct {
        u32 option, padding;
        s64 offset;
        u64 size;
    } input = {.option = FsReadOption_None, .offset = offset, .size = size};
    while (__atomic_exchange_n(&read_lock, 1u, __ATOMIC_ACQUIRE)) svcSleepThread(100000);

    /* Game threads do not have libounce's TLS. Keep their pending filesystem IPC intact. */
    u8 *tls = armGetTls();
    u8 saved[0x100];
    memcpy(saved, tls, sizeof(saved));
    memset(tls, 0, sizeof(saved));
    CmifRequest request = cmifMakeRequest(tls, (CmifRequestFormat){
        .object_id = file->s.object_id, .request_id = 0,
        .data_size = sizeof(input), .num_out_buffers = 1
    });
    memcpy(request.data, &input, sizeof(input));
    cmifRequestOutBuffer(&request, buffer, size, HipcBufferMode_NonSecure);

    Result result = svcSendSyncRequest(file->s.session);
    CmifResponse reply;
    if (R_SUCCEEDED(result)) result = cmifParseResponse(&reply, tls, file->s.object_id != 0, sizeof(*read));
    if (R_SUCCEEDED(result)) {
        u64 bytes;
        memcpy(&bytes, reply.data, sizeof(bytes));
        if (bytes <= size) *read = bytes;
        else result = 0x2EE202;
    }
    memcpy(tls, saved, sizeof(saved));
    __atomic_store_n(&read_lock, 0u, __ATOMIC_RELEASE);
    return result;
}

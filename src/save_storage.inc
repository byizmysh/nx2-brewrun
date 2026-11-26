/* Included by the runtime's rom.c, before the SD save filesystem is opened. */
enum { SdSaveFilesystem = 1, SdCacheFilesystem = 2 };
static FsFileSystem *save_card;
static char cache_root[FS_MAX_PATH];
static size_t cache_root_length;

static void cache_setup(FsFileSystem *sd) {
    save_card = sd;
    cache_root_length = 0;
    const int length = snprintf(cache_root, sizeof cache_root, "%s/cache", SAVE_UP);
    if (length > 0 && (size_t)length + sizeof("/0000") <= sizeof cache_root)
        cache_root_length = (size_t)length;
}

static bool cache_folder(char path[FS_MAX_PATH], u16 index) {
    static const char hex[] = "0123456789abcdef";
    size_t at = cache_root_length;
    if (!at) return false;
    for (size_t i = 0; i < at; ++i) path[i] = cache_root[i];
    path[at++] = '/';
    for (unsigned i = 0; i < 4; ++i) path[at + i] = hex[(index >> (12 - 4 * i)) & 15];
    path[at + 4] = 0;
    return true;
}

static u32 ensure_cache_directory(u16 index) {
    char path[FS_MAX_PATH] = {0};
    if (!save_card || !cache_folder(path, index)) return 0x2EE202;
    FsDirEntryType type;
    if (R_SUCCEEDED(fsFsGetEntryType(save_card, path, &type)))
        return type == FsDirEntryType_Dir ? 0 : 0x402;
    Result rc = fsFsCreateDirectory(save_card, cache_root);
    if (rc && rc != 0x402) return rc;
    rc = fsFsCreateDirectory(save_card, path);
    if (rc == 0x402 && R_SUCCEEDED(fsFsGetEntryType(save_card, path, &type)) && type == FsDirEntryType_Dir)
        rc = 0;
    nx2_log("[save] creating cache index %u: result %x\n", (u64)index, (u64)rc);
    return rc;
}

bool iz_has_sd_cache_storage(void) { return save_card && cache_root_length; }

u32 iz_create_cache_storage(s32 index, s64 data_size, s64 journal_size) {
    if (index < 0 || index > UINT16_MAX || data_size < 0 || journal_size < 0) return 0x2EE202;
    return ensure_cache_directory((u16)index);
}

/* CMIF input bytes for a domain request with no buffers, handles or input objects. */
static u32 inline_save_input_size(const u8 *m) {
    const u32 header = rd32(m + 4), bytes = (header & 0x3ff) * 4;
    if (rd32(m) >> 16 || (header & 0x80003c00u) || bytes < 40 || bytes > 0x100 - 8) return 0;
    const u32 domain = rd32(m + 0x10), size = domain >> 16;
    return (domain & 0xffff) == 1 && size >= 16 && size <= bytes - 24 ? size : 0;
}

/* Included by the runtime's rom.c, before the SD save filesystem is opened. */
enum { SdSaveFilesystem = 1, SdCacheFilesystem = 2, SdForeignFilesystem = SdCacheFilesystem + UINT16_MAX + 1,
       SdDirectoryObject = 1u << 30, SdReadOnlyObject = 1u << 31 };
static FsFileSystem *save_card;
static char cache_root[FS_MAX_PATH];
static size_t cache_root_length;

static bool foreign_save_path(u64 title, char path[FS_MAX_PATH]) {
    static const char hex[] = "0123456789abcdef";
    const char *slash = strrchr(SAVE_UP, '/');
    if (!title || !slash) return false;
    const size_t parent_length = (size_t)(slash - SAVE_UP) + 1;
    if (parent_length + sizeof("nup-0000000000000000/save") > FS_MAX_PATH) return false;

    memset(path, 0, FS_MAX_PATH);
    memcpy(path, SAVE_UP, parent_length);
    char *leaf = path + parent_length;
    memcpy(leaf, "nup-", 4);
    for (unsigned i = 0; i < 16; ++i) leaf[4 + i] = hex[(title >> (60 - 4 * i)) & 15];
    memcpy(leaf + 20, "/save", sizeof("/save"));
    return true;
}

static bool foreign_save_exists(u64 title, char path[FS_MAX_PATH]) {
    if (!host_fs || !foreign_save_path(title, path)) return false;
    /* Send a directory lookup without replacing the game's in-flight IPC message. */
    u8 *tls = armGetTls();
    u8 saved[0x100];
    memcpy(saved, tls, sizeof(saved));
    memset(tls, 0, sizeof(saved));
    CmifRequest request = cmifMakeRequest(tls, (CmifRequestFormat){
        .object_id = host_fs->object_id, .request_id = 7, .num_in_pointers = 1
    });
    cmifRequestInPointer(&request, path, FS_MAX_PATH);
    CmifResponse reply;
    const bool exists = R_SUCCEEDED(svcSendSyncRequest(host_fs->session)) &&
                        R_SUCCEEDED(cmifParseResponse(&reply, tls, true, sizeof(u32))) &&
                        rd32(reply.data) == FsDirEntryType_Dir;
    memcpy(tls, saved, sizeof(saved));
    return exists;
}

static bool save_read_command(bool filesystem, bool directory, u32 command, const u8 *data, const u8 *end) {
    if (data[1]) return false;  /* These requests never consume a domain object. */
    if (!filesystem) return command == 0 || command == (directory ? 1u : 4u);
    if (command == 8)
        return end - data >= 36 && (rd32(data) >> 16) >= 20 && rd32(data + 32) == FsOpenMode_Read;
    return command == 7 || command == 9 || command == 11 || command == 12 || command == 14 || command == 16;
}

static const u8 *save_request_attribute(const u8 *m, const u8 *domain) {
    const u32 header = rd32(m + 4);
    if (rd32(m) >> 16 || (header & 0x3c00u)) return NULL;
    u32 start = 8;
    if (header >> 31) {
        const u32 special = rd32(m + 8);
        if (special & ~1u) return NULL;
        start += 4 + (special ? 8 : 0);
    }
    const u32 aligned = (start + 15) & ~15u;
    const u32 bytes = (header & 0x3ffu) * 4;
    const u32 minimum = aligned - start + 40 + sizeof(FsSaveDataAttribute);
    if (domain != m + aligned || bytes < minimum || bytes > 0x100 - start || domain[1]) return NULL;
    const u32 payload = rd32(domain) >> 16;
    if (payload < 24 + sizeof(FsSaveDataAttribute) || payload > start + bytes - aligned - 16) return NULL;
    return domain + 40;
}

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

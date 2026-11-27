/* Included by the runtime's svc.c, on the thread making the game's request. */
static u32 language_functions, language_host_functions;

typedef enum {
    LanguageNone, LanguageFunctions, LanguageApplet, LanguageSettings, LanguageNsInterface,
    LanguageNsSelection, LanguageNsClone, AppletDisplayVersion
} LanguageCall;

typedef struct {
    LanguageCall call;
    bool domain;
    u8 selection;
} LanguageRequest;

/* NS can return a separate session or an object on the caller's existing session. */
static u64 language_ns_objects[16];
static u32 language_ns_next;

static u64 language_ns_key(u32 handle, u32 object) { return ((u64)handle << 32) | object; }

static bool language_ns_known(u32 handle, u32 object) {
    const u64 key = language_ns_key(handle, object);
    for (unsigned i = 0; key && i < sizeof(language_ns_objects) / sizeof(u64); ++i)
        if (__atomic_load_n(&language_ns_objects[i], __ATOMIC_RELAXED) == key) return true;
    return false;
}

static void language_ns_remember(u32 handle, u32 object) {
    if (!handle || language_ns_known(handle, object)) return;
    const unsigned i = __atomic_fetch_add(&language_ns_next, 1, __ATOMIC_RELAXED) %
        (sizeof(language_ns_objects) / sizeof(u64));
    __atomic_store_n(&language_ns_objects[i], language_ns_key(handle, object), __ATOMIC_RELAXED);
}

static void language_ns_forget(u32 handle, u32 object) {
    for (unsigned i = 0; handle && i < sizeof(language_ns_objects) / sizeof(u64); ++i) {
        u64 key = __atomic_load_n(&language_ns_objects[i], __ATOMIC_RELAXED);
        if ((key >> 32) == handle && (!object || (u32)key == object))
            __atomic_compare_exchange_n(&language_ns_objects[i], &key, 0, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
}

static bool language_report_once(unsigned event) {
    static u32 reported;
    const u32 bit = 1u << event;
    return !(__atomic_fetch_or(&reported, bit, __ATOMIC_RELAXED) & bit);
}

static bool language_name_is(const char *name, const char *target) {
    for (; *target; ++name, ++target)
        if (*name != *target) return false;
    return !*name;
}

static u8 *language_payload(u8 *message, const u32 *words, u32 count, size_t needed) {
    const uintptr_t raw = (uintptr_t)words - (uintptr_t)message;
    const size_t bytes = (size_t)count * sizeof(u32);
    if (raw >= 0x100 || bytes > 0x100 - raw) return NULL;
    const uintptr_t aligned = (raw + 15) & ~(uintptr_t)15;
    if (aligned > raw + bytes || needed > raw + bytes - aligned) return NULL;
    return message + aligned;
}

static LanguageRequest language_request(u8 *m, const u8 *q, u32 handle, u32 type,
                                        u32 cmd, const char *service) {
    const LanguageRequest none = {0};
    const bool language = iz_game_language_override != 0;
    if (!language && !iz_game_display_version[0]) return none;
    const bool request = type == CmifCommandType_Request || type == CmifCommandType_RequestWithContext;
    const bool clone = (type == CmifCommandType_Control || type == CmifCommandType_ControlWithContext) &&
        (cmd == 2 || cmd == 4);
    if (!request && !clone) return none;

    const bool applet = applet_sdk && handle == applet_sdk;
    const bool settings = language && cmd == 0 && (language_name_is(service, "set") ||
        language_name_is(service, "set.2") || language_name_is(service, "set.4"));
    const bool ns = language && service[0] == 'n' && service[1] == 's' && service[2] == ':';
    if (clone) {
        if (ns && language_ns_known(handle, 0)) return (LanguageRequest){.call = LanguageNsClone};
        return none;
    }
    if (!applet && !settings && !ns) return none;

    const HipcParsedRequest hipc = hipcParseRequest(m);
    const u32 *words = hipc.data.data_words;
    const u32 count = hipc.meta.num_data_words;
    const u8 *data = language_payload(m, words, count, sizeof(CmifInHeader));
    if (!data) return none;
    const bool domain = q != data;
    const size_t domain_header = domain ? sizeof(CmifDomainInHeader) : 0;
    const size_t header = domain_header + sizeof(CmifInHeader);
    u32 object = 0;
    size_t input = sizeof(u32);
    if (domain) {
        object = rd32(data + 4);
        if (data[0] == CmifDomainRequestType_Close) {
            if (applet) {
                u32 expected = object;
                __atomic_compare_exchange_n(&language_functions, &expected, 0, false,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
            }
            if (ns && object) language_ns_forget(handle, object);
            return none;
        }
        const size_t cmif_size = data[2] | data[3] << 8;
        if (data[0] != CmifDomainRequestType_SendMessage || data[1] ||
            cmif_size < sizeof(CmifInHeader) || q != data + domain_header) return none;
        input = cmif_size - sizeof(CmifInHeader);
    }
    if (!language_payload(m, words, count, header)) return none;

    if (settings && (!domain || !input)) return (LanguageRequest){.call = LanguageSettings, .domain = domain};
    if (applet && domain && !input) {
        if (cmd == 20 && object == applet_proxy) return (LanguageRequest){.call = LanguageFunctions, .domain = true};
        if (object && (object == language_host_functions ||
            object == __atomic_load_n(&language_functions, __ATOMIC_RELAXED))) {
            if (cmd == 21 && language)
                return (LanguageRequest){.call = LanguageApplet, .domain = true};
            if (cmd == 23 && iz_game_display_version[0])
                return (LanguageRequest){.call = AppletDisplayVersion, .domain = true};
        }
    }
    if (!ns) return none;
    const bool control = language_ns_known(handle, object);
    if (!control && cmd == 7989 && (!domain || !input))
        return (LanguageRequest){.call = LanguageNsInterface, .domain = domain};
    if (!control || cmd != 1 || (input != 1 && input != sizeof(u32)) ||
        !language_payload(m, words, count, header + input)) return none;

    const unsigned selection = (unsigned)iz_game_language_index;
    if (selection >= 32) return none;
    const u32 supported = input == 1 ? data[header] : rd32(data + header);
    if (supported & (1u << selection))
        return (LanguageRequest){.call = LanguageNsSelection, .domain = domain, .selection = selection};
    if (language_report_once(2)) nx2_log("[lang] NS did not list the requested language (mask %x)\n", (u64)supported);
    return none;
}

static void language_reply(u8 *m, LanguageRequest request, u32 handle, u32 result) {
    const LanguageCall call = request.call;
    if (call == LanguageNone) return;
    if (result) {
        if (language_report_once(3)) nx2_log("[lang] language IPC failed (kernel result %x)\n", (u64)result);
        return;
    }
    const HipcResponse hipc = hipcParseResponse(m);
    const bool domain = request.domain;
    const size_t header = (domain ? sizeof(CmifDomainOutHeader) : 0) + sizeof(CmifOutHeader);
    u8 *data = language_payload(m, hipc.data_words, hipc.num_data_words, header);
    if (!data) return;
    u8 *cmif = data + header - sizeof(CmifOutHeader);
    if (rd32(cmif) != CMIF_OUT_HEADER_MAGIC) return;
    result = rd32(cmif + 8);
    if (result) {
        if (language_report_once(4)) nx2_log("[lang] language IPC failed (service result %x)\n", (u64)result);
        return;
    }

    const bool interface = call == LanguageFunctions || call == LanguageNsInterface || call == LanguageNsClone;
    const size_t size = interface ? (domain ? sizeof(u32) : 0) :
        (call == LanguageNsSelection ? sizeof(u8) : sizeof(u64));
    if ((domain && rd32(data) != (u32)interface) ||
        !language_payload(m, hipc.data_words, hipc.num_data_words, header + size)) return;
    if (interface) {
        if (call == LanguageFunctions) {
            const u32 object = rd32(data + header);
            __atomic_store_n(&language_functions, object, __ATOMIC_RELAXED);
            if (language_report_once(5)) nx2_log("[lang] found IApplicationFunctions (object %x)\n", (u64)object);
        } else if (domain) {
            const u32 object = rd32(data + header);
            if (object) language_ns_remember(handle, object);
        } else {
            language_ns_remember(answer_handle(m), 0);
        }
        return;
    }
    if (call == LanguageNsSelection) {
        data[header] = request.selection;
        if (language_report_once(6)) nx2_log("[lang] GetApplicationDesiredLanguage returned index %u\n", (u64)request.selection);
        return;
    }
    char label[9] = {0};
    for (unsigned i = 0; i < sizeof(u64); ++i) {
        const u8 byte = (u8)(iz_game_language_override >> (i * 8));
        data[header + i] = byte;
        label[i] = (char)byte;
    }
    if (language_report_once(call == LanguageApplet ? 0 : 1))
        nx2_log("[lang] %s returned %s\n",
                call == LanguageApplet ? "GetDesiredLanguage" : "GetLanguageCode", label);
}

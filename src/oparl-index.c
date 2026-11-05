#include "search.h"
#include "json.h"
#include "pdf-text.h"
#include "hash_util.h"
#include "runtime.h"
#include "platform.h"

#define NONE UINT32_MAX
#define PATH_SIZE 4096

typedef struct { uint32_t key, value; } MapEntry;
typedef struct { MapEntry *entries; size_t capacity, count; } Map;
typedef struct {
    SearchRecord record;
    uint32_t modified, archive, pdf, hash, search_record, detail;
    unsigned long long mtime, metadata_mtime;
    int rank, deleted;
    ToolArray links, organizations, owners;
} Entity;
typedef struct { uint32_t path; unsigned int year; int incomplete; } ArchiveRoot;
typedef struct {
    SearchBuilder builder;
    ToolArray entities, archives;
    Map ids, files;
    uint32_t archive, current_file;
    unsigned long long mtime;
    int report_fd;
    unsigned long long metadata, pdfs, pages, warning_pages, failed_pdfs, missing_pdfs;
} Import;

#if defined(_WIN32)
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) int __stdcall FreeLibrary(void *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
typedef struct {
    uint32_t attributes, times[6], size_high, size_low, reserved[2];
    char name[260], alternate[14];
} FindData;
typedef void *(__stdcall *FindFirst)(const char *, FindData *);
typedef int (__stdcall *FindNext)(void *, FindData *);
typedef int (__stdcall *FindClose)(void *);
#endif

static int walk_archive(const char *path, ToolWalkCallback callback, void *user, int depth) {
    PlatformDirEntry current;
    ToolWalkControl control;
    char child[PATH_SIZE];
    int status = 0;
    if (depth > 64 || platform_get_path_info(path, &current) != 0)
        return search_error("cannot inspect archive path or directory nesting exceeds 64", path);
    control.prune = 0;
    if (callback(path, &current, depth, &control, user) != 0) return -1;
    if (!current.is_dir || control.prune) return 0;
#if defined(_WIN32)
    {
        union { void *pointer; FindFirst function; } first;
        union { void *pointer; FindNext function; } next;
        union { void *pointer; FindClose function; } close;
        FindData data;
        void *module = LoadLibraryA("KERNEL32.dll"), *handle;
        if (!module) return search_error("cannot load Windows directory functions", path);
        first.pointer = GetProcAddress(module, "FindFirstFileA");
        next.pointer = GetProcAddress(module, "FindNextFileA");
        close.pointer = GetProcAddress(module, "FindClose");
        if (!first.pointer || !next.pointer || !close.pointer ||
            rt_join_path(path, "*", child, sizeof(child)) != 0) {
            FreeLibrary(module); return search_error("cannot prepare Windows directory scan", path);
        }
        handle = first.function(child, &data);
        if (handle == (void *)(intptr_t)-1) {
            unsigned long error = GetLastError();
            FreeLibrary(module);
            return error == 2 ? 0 : search_error("cannot open archive directory", path);
        }
        for (;;) {
            if (data.name[0] != '.') {
                if ((data.attributes & 0x400U) ||
                    rt_join_path(path, data.name, child, sizeof(child)) != 0 ||
                    walk_archive(child, callback, user, depth + 1) != 0) {
                    status = search_error("cannot traverse archive entry or junction", path); break;
                }
            }
            if (!next.function(handle, &data)) {
                if (GetLastError() != 18) status = search_error("cannot read archive directory", path);
                break;
            }
        }
        if (!close.function(handle)) status = search_error("cannot close archive directory", path);
        FreeLibrary(module);
    }
#else
    {
        PlatformLightDirEntry *entries;
        size_t capacity = 1024, count = 0, i;
        int directory = 0;
        for (;;) {
            entries = (PlatformLightDirEntry *)rt_malloc_array(capacity, sizeof(*entries));
            if (!entries) return search_error("cannot allocate archive directory entries", path);
            rt_memset(entries, 0, capacity * sizeof(*entries));
            if (platform_collect_light_entries(path, 0, entries, capacity, &count, &directory) == 0) break;
            i = entries[capacity - 1].name[0] != '\0';
            rt_free(entries);
            if (!i || capacity > SIZE_MAX / 2 / sizeof(*entries))
                return search_error("cannot read archive directory", path);
            capacity *= 2;
        }
        if (!directory) status = search_error("archive directory changed during traversal", path);
        for (i = 0; status == 0 && i < count; i++) {
            if (entries[i].name[0] == '.') continue;
            if (rt_join_path(path, entries[i].name, child, sizeof(child)) != 0 ||
                walk_archive(child, callback, user, depth + 1) != 0) status = -1;
        }
        rt_free(entries);
    }
#endif
    return status;
}

static Entity *entity(Import *import, uint32_t n) {
    return (Entity *)tool_array_get(&import->entities, n);
}
static const char *string(Import *import, uint32_t n) {
    return search_builder_string(&import->builder, n);
}
static uint32_t text(Import *import, const char *value) {
    return search_string(&import->builder, value, rt_strlen(value));
}
static uint64_t hash_key(const char *key) {
    uint64_t hash = 14695981039346656037ULL;
    while (*key) { hash ^= (unsigned char)*key++; hash *= 1099511628211ULL; }
    return hash;
}
static uint32_t map_get(Import *import, const Map *map, const char *key) {
    size_t slot;
    if (!map->capacity) return NONE;
    slot = (size_t)hash_key(key) & (map->capacity - 1);
    while (map->entries[slot].key) {
        if (rt_strcmp(string(import, map->entries[slot].key), key) == 0) return map->entries[slot].value;
        slot = (slot + 1) & (map->capacity - 1);
    }
    return NONE;
}
static int map_put(Import *import, Map *map, uint32_t key, uint32_t value) {
    size_t slot, i;
    if (map->count * 2 >= map->capacity) {
        size_t capacity = map->capacity ? map->capacity * 2 : 4096;
        MapEntry *entries = (MapEntry *)rt_malloc_array(capacity, sizeof(MapEntry));
        if (!entries) return search_error("cannot allocate entity identities", 0);
        rt_memset(entries, 0, capacity * sizeof(MapEntry));
        for (i = 0; i < map->capacity; i++) {
            if (!map->entries[i].key) continue;
            slot = (size_t)hash_key(string(import, map->entries[i].key)) & (capacity - 1);
            while (entries[slot].key) slot = (slot + 1) & (capacity - 1);
            entries[slot] = map->entries[i];
        }
        rt_free(map->entries); map->entries = entries; map->capacity = capacity;
    }
    slot = (size_t)hash_key(string(import, key)) & (map->capacity - 1);
    while (map->entries[slot].key) {
        if (rt_strcmp(string(import, map->entries[slot].key), string(import, key)) == 0) {
            map->entries[slot].value = value;
            return 0;
        }
        slot = (slot + 1) & (map->capacity - 1);
    }
    map->entries[slot].key = key; map->entries[slot].value = value; map->count++;
    return 0;
}
static uint32_t json_text(Import *import, const Json *json, size_t object, const char *key) {
    size_t index = json_get(json, object, key);
    const JsonToken *token = json_token(json, index);
    char *value;
    uint32_t offset;
    if (!token || token->kind == JSON_NULL) return 0;
    if (token->kind != JSON_STRING) {
        search_error("OParl property is not a string", key);
        search_error("metadata source", string(import, import->current_file));
        import->builder.failed = 1;
        return 0;
    }
    value = (char *)rt_malloc(token->end - token->start + 1);
    if (!value) { import->builder.failed = 1; return 0; }
    if (json_string(json, index, value, token->end - token->start + 1) != 0) {
        search_error("cannot decode OParl string property", key);
        rt_free(value); import->builder.failed = 1; return 0;
    }
    offset = text(import, value);
    rt_free(value);
    return offset;
}
static uint32_t new_entity(Import *import, uint32_t id) {
    Entity *e;
    uint32_t n;
    if (import->entities.count >= UINT32_MAX) return NONE;
    n = (uint32_t)import->entities.count;
    e = (Entity *)tool_array_append(&import->entities);
    if (!e) return NONE;
    rt_memset(e, 0, sizeof(*e));
    e->record.id = id; e->record.parent = NONE; e->search_record = NONE;
    tool_array_init(&e->links, sizeof(uint32_t));
    tool_array_init(&e->organizations, sizeof(uint32_t));
    tool_array_init(&e->owners, sizeof(uint32_t));
    if (map_put(import, &import->ids, id, n) != 0) return NONE;
    return n;
}
static int append_id(ToolArray *array, uint32_t id) {
    uint32_t *stored = (uint32_t *)tool_array_append(array);
    if (!stored) return search_error("cannot allocate resource relationships", 0);
    *stored = id;
    return 0;
}

static int ingest(Import *import, const Json *json, size_t object, int rank);
static int link_value(Import *import, const Json *json, size_t value, uint32_t parent, int organization) {
    const JsonToken *token = json_token(json, value);
    uint32_t id;
    size_t child;
    if (!token || token->kind == JSON_NULL) return 0;
    if (token->kind == JSON_ARRAY) {
        for (child = value + 1; child < token->next; child = json_token(json, child)->next)
            if (link_value(import, json, child, parent, organization) != 0) return -1;
        return 0;
    }
    if (token->kind == JSON_OBJECT) {
        id = json_text(import, json, value, "id");
        if (json_equals(json, json_get(json, value, "type"), "https://schema.oparl.org/1.1/File") &&
            ingest(import, json, value, 1) != 0) return -1;
    } else if (token->kind == JSON_STRING) {
        char *value_text = (char *)rt_malloc(token->end - token->start + 1);
        if (!value_text) return -1;
        if (json_string(json, value, value_text, token->end - token->start + 1) != 0) {
            rt_free(value_text); return -1;
        }
        id = text(import, value_text); rt_free(value_text);
    } else return search_error("invalid OParl relationship", 0);
    if (id && append_id(organization ? &entity(import, parent)->organizations :
                       &entity(import, parent)->links, id) != 0) return -1;
    return import->builder.failed ? -1 : 0;
}

static int ingest(Import *import, const Json *json, size_t object, int rank) {
    static const char *const paper[] = {"mainFile","auxiliaryFile","consultation",
        "superordinatedPaper","subordinatedPaper","relatedPaper"};
    static const char *const consultation[] = {"paper","agendaItem","agendaitem","meeting"};
    static const char *const agenda[] = {"meeting","consultation","resolutionFile","auxiliaryFile"};
    static const char *const meeting[] = {"invitation","resultsProtocol","verbatimProtocol","auxiliaryFile"};
    const char *const *keys = 0;
    size_t count = 0, i, type = json_get(json, object, "type");
    uint32_t id, n, modified, kind;
    Entity *e;
    char type_text[128];
    if (json_equals(json, type, "at") || json_equals(json, type, "gr")) type = json_get(json, object, "Type");
    if (json_string(json, type, type_text, sizeof(type_text)) != 0 ||
        rt_strncmp(type_text, "https://schema.oparl.org/1.1/", 29) != 0) return 0;
    id = json_text(import, json, object, "id");
    if (!id) return import->builder.failed ? -1 : 0;
    kind = text(import, type_text + 29);
    modified = json_text(import, json, object, "modified");
    n = map_get(import, &import->ids, string(import, id));
    if (n == NONE && (n = new_entity(import, id)) == NONE) return -1;
    e = entity(import, n);
    if (rank < e->rank) return 0;
    if (rank == e->rank) {
        int compare = modified && e->modified ?
            rt_strcmp(string(import, modified), string(import, e->modified)) : 0;
        if (compare < 0 || (compare == 0 && import->mtime < e->metadata_mtime)) return 0;
    }
    e->rank = rank; e->modified = modified; e->archive = import->archive;
    e->metadata_mtime = import->mtime;
    e->deleted = json_true(json, json_get(json, object, "deleted"));
    e->record.kind = kind; e->record.source = import->current_file;
    e->record.name = json_text(import, json, object, "name");
    if (!e->record.name) e->record.name = json_text(import, json, object, "fileName");
    e->record.reference = json_text(import, json, object, "reference");
    e->record.date = json_text(import, json, object, "date");
    e->record.paper_type = json_text(import, json, object, "paperType");
    e->record.year = 0;
    {
        const JsonToken *token = json_token(json, object);
        ToolByteBuffer detail;
        static const char *const fields[] = {"description","result","role","shortName","classification","meetingState"};
        e->record.payload = search_string(&import->builder, json->text + token->start, token->end - token->start);
        tool_byte_buffer_init(&detail);
        for (i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
            uint32_t value = json_text(import, json, object, fields[i]);
            if (tool_byte_buffer_append_cstr(&detail, string(import, value)) != 0 ||
                tool_byte_buffer_append(&detail, "\x1e", 1) != 0) {
                tool_byte_buffer_free(&detail); return -1;
            }
        }
        e->detail = search_string(&import->builder, (const char *)detail.data, detail.size);
        tool_byte_buffer_free(&detail);
    }
    e->links.count = e->organizations.count = 0;
    if (rt_strcmp(type_text + 29, "File") == 0) {
        unsigned char digest[32];
        char hex[65];
        crypto_sha256_hash((const unsigned char *)string(import, id), rt_strlen(string(import, id)), digest);
        hash_to_hex(digest, 32, hex);
        e->hash = text(import, hex);
        if (map_put(import, &import->files, e->hash, n) != 0) return -1;
    }
    if (e->deleted) return 0;
    if (rt_strcmp(type_text + 29, "Paper") == 0) {
        const char *reference_text = string(import, e->record.reference);
        if (rt_strlen(reference_text) >= 3 && reference_text[0] >= '0' && reference_text[0] <= '9' &&
            reference_text[1] >= '0' && reference_text[1] <= '9' && reference_text[2] == '/')
            e->record.year = (unsigned int)(2000 + (reference_text[0] - '0') * 10 + reference_text[1] - '0');
        keys = paper; count = sizeof(paper)/sizeof(paper[0]);
    } else if (rt_strcmp(type_text + 29, "Consultation") == 0) {
        keys = consultation; count = sizeof(consultation)/sizeof(consultation[0]);
    } else if (rt_strcmp(type_text + 29, "AgendaItem") == 0) {
        keys = agenda; count = sizeof(agenda)/sizeof(agenda[0]);
    } else if (rt_strcmp(type_text + 29, "Meeting") == 0) {
        keys = meeting; count = sizeof(meeting)/sizeof(meeting[0]);
    }
    for (i = 0; i < count; i++)
        if (link_value(import, json, json_get(json, object, keys[i]), n, 0) != 0) return -1;
    if (link_value(import, json, json_get(json, object, "underDirectionOf"), n, 1) != 0 ||
        link_value(import, json, json_get(json, object, "organization"), n, 1) != 0) return -1;
    return import->builder.failed ? -1 : 0;
}

static int metadata_file(const char *path, const PlatformDirEntry *entry, int depth,
                          ToolWalkControl *control, void *user) {
    Import *import = (Import *)user;
    unsigned char *bytes = 0;
    size_t length, index, data;
    Json json;
    int result = 0;
    (void)depth; (void)control;
    if (entry->is_dir) return 0;
    length = rt_strlen(path);
    if (length < 5 || rt_strcmp(path + length - 5, ".json")) return 0;
    if (entry->size > 32ULL * 1024 * 1024 || tool_read_all_input(path, &bytes, &length) != 0)
        return search_error("cannot read archived metadata", path);
    if (json_parse(&json, (const char *)bytes, length) != 0) {
        rt_free(bytes); return search_error("invalid archived JSON", path);
    }
    import->current_file = text(import, path);
    import->mtime = (unsigned long long)entry->mtime;
    result = ingest(import, &json, 0, 2);
    data = json_get(&json, 0, "data");
    if (data != JSON_NONE && json_token(&json, data)->kind == JSON_ARRAY) {
        const JsonToken *array = json_token(&json, data);
        for (index = data + 1; result == 0 && index < array->next; index = json_token(&json, index)->next)
            if (json_token(&json, index)->kind == JSON_OBJECT) result = ingest(import, &json, index, 2);
    }
    import->metadata++;
    json_free(&json); rt_free(bytes);
    if (result != 0) search_error("cannot import metadata resource", path);
    return result;
}

static int archive_root(const char *path, const PlatformDirEntry *entry, int depth,
                         ToolWalkControl *control, void *user) {
    Import *import = (Import *)user;
    char summary[PATH_SIZE];
    PlatformDirEntry info;
    unsigned char *bytes = 0;
    size_t length;
    Json json;
    ArchiveRoot *archive;
    unsigned long long year = 0;
    (void)depth;
    if (!entry->is_dir) return 0;
    if (rt_join_path(path, "archive.json", summary, sizeof(summary)) != 0) return -1;
    if (platform_get_path_info(summary, &info) != 0) return 0;
    control->prune = 1;
    if (info.size > 1024 * 1024 || tool_read_all_input(summary, &bytes, &length) != 0)
        return search_error("cannot read archive summary", summary);
    if (json_parse(&json, (const char *)bytes, length) != 0) {
        rt_free(bytes); return search_error("invalid archive summary", summary);
    }
    {
        int incomplete = !json_true(&json, json_get(&json, 0, "complete")) ||
            json_true(&json, json_get(&json, 0, "metadataOnly"));
        archive = (ArchiveRoot *)tool_array_append(&import->archives);
        if (!archive) { json_free(&json); rt_free(bytes); return -1; }
        archive->incomplete = incomplete;
        if (incomplete) import->builder.incomplete = 1;
    }
    if (json_get(&json, 0, "referenceYear") != JSON_NONE &&
        json_uint(&json, json_get(&json, 0, "referenceYear"), &year) != 0) {
        json_free(&json); rt_free(bytes); return search_error("invalid archive year", summary);
    }
    json_free(&json); rt_free(bytes);
    archive->path = text(import, path); archive->year = (unsigned int)year;
    return 0;
}

static int document_file(const char *path, const PlatformDirEntry *entry, int depth,
                          ToolWalkControl *control, void *user) {
    Import *import = (Import *)user;
    const char *name = path;
    char hash[65];
    uint32_t n;
    Entity *e;
    size_t i, length = rt_strlen(path);
    int preferred;
    (void)depth; (void)control;
    if (entry->is_dir || (length >= 5 && rt_strcmp(path + length - 5, ".part") == 0)) return 0;
    for (i = 0; path[i]; i++) if (path[i] == '/') name = path + i + 1;
    if (rt_strlen(name) < 66 || name[64] != '-') return search_error("unrecognized archived document name", path);
    memcpy(hash, name, 64); hash[64] = '\0';
    for (i = 0; i < 64; i++) if (tool_hex_value(hash[i]) < 0)
        return search_error("invalid document identity hash", path);
    n = map_get(import, &import->files, hash);
    if (n == NONE) {
        char id[128];
        memcpy(id, "urn:oparl-search:archive-file:", 30);
        memcpy(id + 30, hash, 65);
        n = new_entity(import, text(import, id));
        if (n == NONE) return -1;
        e = entity(import, n);
        e->record.kind = text(import, "File"); e->record.name = text(import, name + 65);
        e->hash = text(import, hash);
        if (map_put(import, &import->files, e->hash, n) != 0) return -1;
    }
    e = entity(import, n);
    preferred = e->archive == import->archive;
    if (!e->pdf || preferred || (!e->archive && entry->mtime > (long long)e->mtime)) {
        e->pdf = text(import, path); e->mtime = (unsigned long long)entry->mtime;
        if (!e->record.payload) e->record.source = e->pdf;
    }
    return import->builder.failed ? -1 : 0;
}

static int ownership(Import *import) {
    uint32_t *visited = (uint32_t *)rt_malloc_array(import->entities.count, sizeof(uint32_t));
    ToolArray queue;
    size_t i, j;
    if (!visited) return -1;
    rt_memset(visited, 0, import->entities.count * sizeof(uint32_t));
    tool_array_init(&queue, sizeof(uint32_t));
    for (i = 0; i < import->entities.count; i++) {
        Entity *paper = entity(import, (uint32_t)i);
        uint32_t stamp = (uint32_t)i + 1;
        if (paper->deleted || rt_strcmp(string(import, paper->record.kind), "Paper")) continue;
        queue.count = 0;
        if (append_id(&queue, (uint32_t)i) != 0) goto fail;
        visited[i] = stamp;
        for (j = 0; j < queue.count; j++) {
            uint32_t current = *(const uint32_t *)tool_array_get_const(&queue, j);
            Entity *e = entity(import, current);
            size_t k;
            if (e->deleted) continue;
            if (rt_strcmp(string(import, e->record.kind), "File") == 0 &&
                append_id(&e->owners, (uint32_t)i) != 0) goto fail;
            for (k = 0; k < e->links.count; k++) {
                uint32_t id = *(const uint32_t *)tool_array_get_const(&e->links, k);
                uint32_t target = map_get(import, &import->ids, string(import, id));
                if (target == NONE || visited[target] == stamp) continue;
                visited[target] = stamp;
                if (append_id(&queue, target) != 0) goto fail;
            }
        }
    }
    tool_array_free(&queue); rt_free(visited);
    return 0;
fail:
    tool_array_free(&queue); rt_free(visited);
    return -1;
}

static int record_text(Import *import, SearchRecord *record, const char *body, size_t length) {
    ToolByteBuffer buffer;
    size_t i;
    uint32_t fields[] = {record->name,record->reference,record->paper_type,record->organization};
    tool_byte_buffer_init(&buffer);
    for (i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        if (tool_byte_buffer_append_cstr(&buffer, string(import, fields[i])) != 0 ||
            tool_byte_buffer_append(&buffer, "\x1e", 1) != 0) goto fail;
    }
    record->body_start = (uint32_t)buffer.size;
    if (tool_byte_buffer_append(&buffer, body, length) != 0 || buffer.size > UINT32_MAX) goto fail;
    record->text_length = (uint32_t)buffer.size;
    record->text = search_string(&import->builder, (const char *)buffer.data, buffer.size);
    tool_byte_buffer_free(&buffer);
    return import->builder.failed ? -1 : 0;
fail:
    import->builder.failed = 1;
    tool_byte_buffer_free(&buffer);
    return -1;
}

typedef struct { Import *import; uint32_t entity; } PageContext;
static int extracted_page(unsigned int number, const char *body, size_t length,
                            unsigned int warnings, void *user) {
    PageContext *context = (PageContext *)user;
    Import *import = context->import;
    Entity *e = entity(import, context->entity);
    SearchRecord record = e->record;
    uint32_t n;
    record.kind = text(import, "Page"); record.page = number; record.warnings = warnings;
    record.parent = e->search_record;
    record.source = e->pdf;
    if (record_text(import, &record, body, length) != 0) return -1;
    n = search_add_record(&import->builder, &record);
    if (n == NONE) return -1;
    import->pages++;
    if (warnings) {
        import->warning_pages++; import->builder.incomplete = 1;
        ((SearchRecord *)import->builder.records.data)[e->search_record].warnings |= warnings;
        if (rt_write_cstr(import->report_fd, "{\"file\":") != 0 ||
            tool_json_write_string(import->report_fd, string(import, e->pdf)) != 0 ||
            tool_json_field_uint(import->report_fd, "page", number) != 0 ||
            tool_json_field_uint(import->report_fd, "warnings", warnings) != 0 ||
            rt_write_cstr(import->report_fd, "}\n") != 0) {
            import->builder.failed = 1;
            return -1;
        }
    }
    return 0;
}

static int issue(Import *import, Entity *e, const char *reason) {
    import->builder.incomplete = 1;
    ((SearchRecord *)import->builder.records.data)[e->search_record].warnings |= 8;
    if (rt_write_cstr(import->report_fd, "{\"id\":") != 0 ||
        tool_json_write_string(import->report_fd, string(import, e->record.id)) != 0 ||
        tool_json_field_string(import->report_fd, "file", string(import, e->pdf)) != 0 ||
        tool_json_field_string(import->report_fd, "error", reason) != 0 ||
        rt_write_cstr(import->report_fd, "}\n") != 0) return -1;
    rt_write_cstr(2, "oparl-index: warning: "); rt_write_cstr(2, reason);
    rt_write_cstr(2, ": "); rt_write_line(2, string(import, e->record.id));
    return 0;
}

static int emit(Import *import) {
    size_t i, j;
    for (i = 0; i < import->entities.count; i++) {
        Entity *e = entity(import, (uint32_t)i);
        ToolByteBuffer organizations;
        if (e->deleted) continue;
        tool_byte_buffer_init(&organizations);
        for (j = 0; j < e->organizations.count; j++) {
            uint32_t id = *(const uint32_t *)tool_array_get_const(&e->organizations, j);
            uint32_t target = map_get(import, &import->ids, string(import, id));
            const char *name = target == NONE ? string(import, id) : string(import, entity(import, target)->record.name);
            if (tool_byte_buffer_append_cstr(&organizations, name) != 0 ||
                tool_byte_buffer_append(&organizations, "\n", 1) != 0) {
                tool_byte_buffer_free(&organizations); return -1;
            }
        }
        e->record.organization = search_string(&import->builder,
          organizations.data ? (const char *)organizations.data : "", organizations.size);
        tool_byte_buffer_free(&organizations);
        {
            const char *detail = string(import, e->detail);
            if (record_text(import, &e->record, detail, rt_strlen(detail)) != 0) return -1;
        }
        e->search_record = search_add_record(&import->builder, &e->record);
        if (e->search_record == NONE) return -1;
    }
    for (i = 0; i < import->entities.count; i++) {
        Entity *e = entity(import, (uint32_t)i);
        unsigned char *bytes = 0;
        size_t length;
        PageContext context;
        const char *error = 0;
        if (e->deleted || rt_strcmp(string(import, e->record.kind), "File")) continue;
        for (j = 0; j < e->owners.count; j++) {
            uint32_t parent = *(const uint32_t *)tool_array_get_const(&e->owners, j);
            if (search_add_owner(&import->builder, e->search_record, entity(import, parent)->search_record) != 0)
                return -1;
        }
        if (!e->pdf) {
            import->missing_pdfs++;
            if (issue(import, e, "document is not archived") != 0) return -1;
            continue;
        }
        if (tool_read_all_input(string(import, e->pdf), &bytes, &length) != 0)
            return search_error("cannot read archived document", string(import, e->pdf));
        context.import = import; context.entity = (uint32_t)i;
        if (length < 5 || memcmp(bytes, "%PDF-", 5) != 0 ||
            pdf_text_extract(bytes, length, extracted_page, &context, &error) != 0) {
            import->failed_pdfs++;
            if (import->builder.failed || (error && rt_strcmp(error, "cannot store extracted page") == 0)) {
                rt_free(bytes);
                return search_error("cannot persist extracted page", string(import, e->pdf));
            }
            if (issue(import, e, error ? error : "unsupported document format") != 0) {
                rt_free(bytes); return -1;
            }
        }
        import->pdfs++;
        if (import->pdfs % 100 == 0) {
            rt_write_cstr(2, "indexed PDFs: "); rt_write_uint(2, import->pdfs);
            rt_write_cstr(2, ", pages: "); rt_write_uint(2, import->pages); rt_write_char(2, '\n');
        }
        rt_free(bytes);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *input = "downloads", *output = "build/search/index.ops";
    Import import;
    char path[PATH_SIZE], report[PATH_SIZE], part[PATH_SIZE], scratch[PATH_SIZE];
    size_t i;
    int result = -1;
    unsigned long long started = platform_get_monotonic_time_ns();
    rt_memset(&import, 0, sizeof(import)); import.report_fd = -1;
    search_builder_init(&import.builder);
    tool_array_init(&import.entities, sizeof(Entity));
    tool_array_init(&import.archives, sizeof(ArchiveRoot));
    for (i = 1; i < (size_t)argc; i++) {
        if (rt_strcmp(argv[i], "--help") == 0) {
            rt_write_cstr(1, "Usage: oparl-index [--input ARCHIVE_OR_PARENT] [--output INDEX]\n"
                "Build a binary metadata/full-text index; originals stay unchanged.\n"
                "Exit: 0 complete, 1 error, 3 published index with documented coverage gaps.\n");
            result = 0; goto done;
        }
        if (i + 1 == (size_t)argc) { search_error("missing option value", argv[i]); goto done; }
        if (rt_strcmp(argv[i], "--input") == 0) input = argv[++i];
        else if (rt_strcmp(argv[i], "--output") == 0) output = argv[++i];
        else { search_error("unknown option", argv[i]); goto done; }
    }
    if (walk_archive(input, archive_root, &import, 0) != 0) {
        search_error("cannot discover archives", input); goto done;
    }
    if (!import.archives.count) { search_error("no OParl archives found", input); goto done; }
    for (i = 0; i < import.archives.count; i++) {
        const ArchiveRoot *archive = (const ArchiveRoot *)tool_array_get_const(&import.archives, i);
        import.archive = archive->path;
        if (rt_join_path(string(&import, archive->path), "metadata", path, sizeof(path)) != 0 ||
            walk_archive(path, metadata_file, &import, 0) != 0) {
            search_error("cannot import archive metadata", path); goto done;
        }
    }
    for (i = 0; i < import.archives.count; i++) {
        const ArchiveRoot *archive = (const ArchiveRoot *)tool_array_get_const(&import.archives, i);
        PlatformDirEntry info;
        import.archive = archive->path;
        if (rt_join_path(string(&import, archive->path), "documents", path, sizeof(path)) != 0) goto done;
        if (platform_get_path_info(path, &info) != 0) continue;
        if (walk_archive(path, document_file, &import, 0) != 0) goto done;
    }
    if (tool_path_append_suffix(output, ".coverage.jsonl", report, sizeof(report)) != 0 ||
        tool_path_append_suffix(report, ".part", part, sizeof(part)) != 0 ||
        tool_ensure_parent_dirs(part, scratch, sizeof(scratch)) != 0) goto done;
    import.report_fd = platform_open_write(part, 0644);
    if (import.report_fd < 0) { search_error("cannot create coverage report", part); goto done; }
    for (i = 0; i < import.archives.count; i++) {
        const ArchiveRoot *archive = (const ArchiveRoot *)tool_array_get_const(&import.archives, i);
        if (!archive->incomplete) continue;
        if (rt_write_cstr(import.report_fd, "{\"archive\":") != 0 ||
            tool_json_write_string(import.report_fd, string(&import, archive->path)) != 0 ||
            tool_json_field_uint(import.report_fd, "referenceYear", archive->year) != 0 ||
            tool_json_field_bool(import.report_fd, "incomplete", 1) != 0 ||
            rt_write_cstr(import.report_fd, "}\n") != 0) {
            search_error("cannot write archive coverage", part); goto done;
        }
    }
    if (ownership(&import) != 0 || emit(&import) != 0) {
        search_error("cannot construct full search index", 0); goto done;
    }
    if (platform_close(import.report_fd) != 0) { import.report_fd = -1; goto done; }
    import.report_fd = -1;
    if (platform_rename_path(part, report) != 0 || search_write(&import.builder, output) != 0) goto done;
    rt_write_cstr(2, "Index published: records="); rt_write_uint(2, import.builder.records.count);
    rt_write_cstr(2, ", terms="); rt_write_uint(2, import.builder.terms.count);
    rt_write_cstr(2, ", pages="); rt_write_uint(2, import.pages);
    rt_write_cstr(2, ", warning pages="); rt_write_uint(2, import.warning_pages);
    rt_write_cstr(2, ", failed PDFs="); rt_write_uint(2, import.failed_pdfs);
    rt_write_cstr(2, ", missing PDFs="); rt_write_uint(2, import.missing_pdfs);
    rt_write_cstr(2, ", elapsed ms="); rt_write_uint(2, (platform_get_monotonic_time_ns() - started)/1000000ULL);
    rt_write_cstr(2, import.builder.incomplete ? ", incomplete=true\n" : ", incomplete=false\n");
    result = import.builder.incomplete ? 3 : 0;
done:
    if (import.report_fd >= 0) platform_close(import.report_fd);
    for (i = 0; i < import.entities.count; i++) {
        Entity *e = entity(&import, (uint32_t)i);
        tool_array_free(&e->links); tool_array_free(&e->organizations); tool_array_free(&e->owners);
    }
    tool_array_free(&import.entities); tool_array_free(&import.archives);
    rt_free(import.ids.entries); rt_free(import.files.entries);
    search_builder_free(&import.builder);
    return result < 0 ? 1 : result;
}

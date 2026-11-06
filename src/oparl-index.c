#include "search.h"
#include "json.h"
#include "pdf-text.h"
#include "hash_util.h"
#include "runtime.h"
#include "platform.h"
#include "archive-version.h"
#include "archive-walk.h"
#include "prepare.h"

#define NONE UINT32_MAX
#define PATH_SIZE 4096

typedef struct { uint32_t key, value; } MapEntry;
typedef struct { MapEntry *entries; size_t capacity, count; } Map;
typedef struct { uint32_t id, role, property; } EntityLink;
typedef struct {
    SearchRecord record;
    uint32_t modified, archive, pdf, hash, search_record, detail;
    unsigned long long mtime, metadata_mtime, metadata_observed, pdf_observed, pdf_fetched;
    uint32_t pdf_digest;
    int rank, deleted;
    uint32_t dates[SEARCH_DATE_END], authority, version;
    ToolArray links, organizations, owners;
} Entity;
typedef struct { uint32_t path; unsigned int year; int incomplete; } ArchiveRoot;
typedef struct {
    SearchBuilder builder;
    ToolArray entities, archives;
    Map ids, files;
    uint32_t archive, current_file;
    unsigned long long mtime, observed;
    int report_fd;
    const char *prepared;
    char preparation_key[65];
    unsigned long long metadata, pdfs, pages, warning_pages, failed_pdfs, missing_pdfs;
} Import;
static const char *string(Import *import, uint32_t n);
typedef struct {
    char digest[65], current[256];
    unsigned long long observed, fetched, size;
    int present;
} Attestation;

static int attestation(Import *import, const char *kind, const char *hash, Attestation *out) {
    char name[128], directory[PATH_SIZE], path[PATH_SIZE];
    PlatformDirEntry info;
    unsigned char *bytes = 0;
    size_t length, used, i;
    Json json;
    int valid;
    rt_memset(out, 0, sizeof(*out));
    rt_copy_string(name, sizeof(name), kind); used = rt_strlen(name);
    name[used++] = '-'; memcpy(name + used, hash, 64); memcpy(name + used + 64, ".json", 6);
    if (rt_join_path(string(import, import->archive), "cache", directory, sizeof(directory)) != 0 ||
        rt_join_path(directory, name, path, sizeof(path)) != 0) return search_error("cache path is too long", 0);
    if (platform_get_path_info(path, &info) != 0) return 0;
    if (info.size > 32768 || tool_read_all_input(path, &bytes, &length) != 0)
        return search_error("cannot read archive attestation", path);
    if (json_parse(&json, (const char *)bytes, length) != 0) {
        rt_free(bytes); return search_error("invalid archive attestation JSON", path);
    }
    valid = json_string(&json, json_get(&json, 0, "sha256"), out->digest, sizeof(out->digest)) == 0 &&
        rt_strlen(out->digest) == 64 && json_uint(&json, json_get(&json, 0, "size"), &out->size) == 0;
    if (json_get(&json, 0, "observedAtEpochSeconds") != JSON_NONE &&
        json_uint(&json, json_get(&json, 0, "observedAtEpochSeconds"), &out->observed) != 0) valid = 0;
    if (json_get(&json, 0, "fetchedAtEpochSeconds") != JSON_NONE &&
        json_uint(&json, json_get(&json, 0, "fetchedAtEpochSeconds"), &out->fetched) != 0) valid = 0;
    if (json_get(&json, 0, "currentPath") != JSON_NONE &&
        json_string(&json, json_get(&json, 0, "currentPath"), out->current, sizeof(out->current)) != 0) valid = 0;
    for (i = 0; i < 64; i++) if (tool_hex_value(out->digest[i]) < 0) valid = 0;
    if (out->fetched > out->observed || out->observed > 0x7fffffffffffffffULL) valid = 0;
    if (out->current[0] && !rt_strcmp(kind, "document")) {
        if (rt_strncmp(out->current, "documents/", 10) || rt_strncmp(out->current + 10, hash, 64) ||
            out->current[74] != '-') valid = 0;
        for (i = 75; i < rt_strlen(out->current); i++)
            if (out->current[i] == '/' || out->current[i] == '\\') valid = 0;
    }
    json_free(&json); rt_free(bytes);
    if (!valid) return search_error("invalid archive attestation", path);
    out->present = 1;
    return 0;
}

static int walk_archive(const char *path, ToolWalkCallback callback, void *user, int depth) {
    return archive_walk(path, callback, user, depth);
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
    e->record.id = e->record.identity = id; e->record.parent = NONE; e->search_record = NONE;
    tool_array_init(&e->links, sizeof(EntityLink));
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
static int link_value(Import *import, const Json *json, size_t value, uint32_t parent,
                      uint32_t role, uint32_t property) {
    const JsonToken *token = json_token(json, value);
    uint32_t id;
    size_t child;
    if (!token || token->kind == JSON_NULL) return 0;
    if (token->kind == JSON_ARRAY) {
        if (role == SEARCH_BODY || role == SEARCH_SYSTEM)
            return search_error("OParl context relationship must be singular", string(import, property));
        for (child = value + 1; child < token->next; child = json_token(json, child)->next)
            if (link_value(import, json, child, parent, role, property) != 0) return -1;
        return 0;
    }
    if (token->kind == JSON_OBJECT) {
        id = json_text(import, json, value, "id");
        if (ingest(import, json, value, 1) != 0) return -1;
    } else if (token->kind == JSON_STRING) {
        char *value_text = (char *)rt_malloc(token->end - token->start + 1);
        if (!value_text) return -1;
        if (json_string(json, value, value_text, token->end - token->start + 1) != 0) {
            rt_free(value_text); return -1;
        }
        id = text(import, value_text); rt_free(value_text);
    } else return search_error("invalid OParl relationship", 0);
    if (id) {
        EntityLink *link = (EntityLink *)tool_array_append(&entity(import, parent)->links);
        if (!link) return search_error("cannot allocate typed relationship", 0);
        link->id = id; link->role = role; link->property = property;
        if ((role == SEARCH_DIRECTION || role == SEARCH_ORGANIZATION) &&
            append_id(&entity(import, parent)->organizations, id) != 0) return -1;
        if (role == SEARCH_BODY) entity(import, parent)->record.body = id;
        if (role == SEARCH_SYSTEM) entity(import, parent)->record.system = id;
    }
    return import->builder.failed ? -1 : 0;
}

typedef struct { const char *key; uint32_t role; } RelationKey;
static int ingest(Import *import, const Json *json, size_t object, int rank) {
    static const RelationKey paper[] = {
        {"mainFile",SEARCH_MAIN_FILE},{"auxiliaryFile",SEARCH_PAPER_ATTACHMENT},
        {"consultation",SEARCH_CONSULTATION},{"superordinatedPaper",SEARCH_SUPERORDINATED},
        {"subordinatedPaper",SEARCH_SUBORDINATED},{"relatedPaper",SEARCH_RELATED}};
    static const RelationKey consultation[] = {
        {"paper",SEARCH_CONSULTATION_PAPER},{"agendaItem",SEARCH_AGENDA_ITEM},
        {"agendaitem",SEARCH_AGENDA_ITEM},{"meeting",SEARCH_MEETING}};
    static const RelationKey agenda[] = {
        {"meeting",SEARCH_AGENDA_MEETING},{"consultation",SEARCH_AGENDA_CONSULTATION},
        {"resolutionFile",SEARCH_RESOLUTION_FILE},{"auxiliaryFile",SEARCH_AGENDA_ATTACHMENT}};
    static const RelationKey meeting[] = {
        {"invitation",SEARCH_INVITATION},{"resultsProtocol",SEARCH_RESULTS_PROTOCOL},
        {"verbatimProtocol",SEARCH_VERBATIM_PROTOCOL},{"auxiliaryFile",SEARCH_MEETING_ATTACHMENT},
        {"agendaItem",SEARCH_MEETING_AGENDA}};
    static const RelationKey common[] = {
        {"underDirectionOf",SEARCH_DIRECTION},{"organization",SEARCH_ORGANIZATION},
        {"body",SEARCH_BODY},{"system",SEARCH_SYSTEM}};
    const RelationKey *keys = 0;
    size_t count = 0, i, type = json_get(json, object, "type");
    uint32_t id, n, modified, kind;
    unsigned long long observed;
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
    observed = import->observed;
    if (rank == 1 && !json_true(json, json_get(json, object, "deleted")) &&
        json_get(json, object, "name") == JSON_NONE && json_get(json, object, "fileName") == JSON_NONE &&
        json_get(json, object, "accessUrl") == JSON_NONE && json_get(json, object, "start") == JSON_NONE &&
        json_get(json, object, "reference") == JSON_NONE) observed = 0;
    if (observed < e->metadata_observed) return 0;
    if (observed == e->metadata_observed && rank < e->rank) return 0;
    if (observed == e->metadata_observed && rank == e->rank) {
        int compare = modified && e->modified ?
            rt_strcmp(string(import, modified), string(import, e->modified)) : 0;
        if (compare < 0 || (compare == 0 && import->mtime < e->metadata_mtime)) return 0;
    }
    e->rank = rank; e->modified = modified; e->archive = import->archive;
    e->record.metadata_origin = (uint32_t)rank;
    e->metadata_mtime = import->mtime;
    e->metadata_observed = observed;
    e->deleted = json_true(json, json_get(json, object, "deleted"));
    if (!rt_strcmp(type_text + 29, "AgendaItem")) {
        size_t public = json_get(json, object, "public");
        const JsonToken *token = json_token(json, public);
        if (token && token->kind == JSON_BOOL && !json_true(json, public)) e->deleted = 1;
    }
    e->record.kind = kind; e->record.source = import->current_file;
    e->record.name = json_text(import, json, object, "name");
    if (!e->record.name) e->record.name = json_text(import, json, object, "fileName");
    e->record.reference = json_text(import, json, object, "reference");
    e->record.date = json_text(import, json, object, "date");
    rt_memset(e->dates, 0, sizeof(e->dates));
    if (observed) {
        char timestamp[64];
        if (archive_timestamp(observed, timestamp, sizeof(timestamp)) != 0)
            return search_error("cannot format verified observation time", string(import, id));
        e->dates[SEARCH_OBSERVED] = text(import, timestamp);
    }
    e->dates[SEARCH_CREATED] = json_text(import, json, object, "created");
    e->dates[SEARCH_MODIFIED] = modified;
    e->record.system = e->record.body = 0;
    e->authority = SEARCH_AUTHORITY_UNKNOWN;
    if (!rt_strcmp(type_text + 29, "Consultation")) {
        size_t authoritative = json_get(json, object, "authoritative");
        const JsonToken *token = json_token(json, authoritative);
        if (token && token->kind != JSON_NULL) {
            if (token->kind != JSON_BOOL) return search_error("authoritative is not a boolean", string(import, id));
            e->authority = json_true(json, authoritative) ? SEARCH_AUTHORITATIVE : SEARCH_PRELIMINARY;
        }
    }
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
        e->dates[SEARCH_PAPER_DATE] = e->record.date;
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
        e->dates[SEARCH_MEETING_START] = json_text(import, json, object, "start");
        e->dates[SEARCH_MEETING_END] = json_text(import, json, object, "end");
        keys = meeting; count = sizeof(meeting)/sizeof(meeting[0]);
    }
    for (i = 0; i < count; i++)
        if (link_value(import, json, json_get(json, object, keys[i].key), n,
                       keys[i].role, text(import, keys[i].key)) != 0) return -1;
    for (i = 0; i < sizeof(common)/sizeof(common[0]); i++) {
        uint32_t role = common[i].role;
        if (role == SEARCH_BODY && !rt_strcmp(type_text + 29, "System")) role = SEARCH_BODY_COLLECTION;
        if (link_value(import, json, json_get(json, object, common[i].key), n,
                       role, text(import, common[i].key)) != 0) return -1;
    }
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
    import->observed = 0;
    {
        const char *name = path;
        char hash[65], digest[65];
        Attestation cache;
        for (index = 0; path[index]; index++) if (path[index] == '/') name = path + index + 1;
        if (rt_strlen(name) == 69) {
            memcpy(hash, name, 64); hash[64] = 0;
            for (index = 0; index < 64 && tool_hex_value(hash[index]) >= 0; index++) {}
            if (index == 64) {
                if (attestation(import, "metadata", hash, &cache) != 0) result = -1;
                else if (cache.present) {
                    unsigned char sha[32];
                    crypto_sha256_hash(bytes, length, sha); hash_to_hex(sha, 32, digest);
                    if (length != cache.size || rt_strcmp(digest, cache.digest))
                        result = search_error("metadata differs from archive attestation; refresh the archive", path);
                    else import->observed = cache.observed;
                }
            }
        }
    }
    if (result != 0) { json_free(&json); rt_free(bytes); return -1; }
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
    Attestation cache;
    (void)depth; (void)control;
    if (entry->is_dir || (length >= 5 && rt_strcmp(path + length - 5, ".part") == 0)) return 0;
    for (i = 0; path[i]; i++) if (path[i] == '/') name = path + i + 1;
    if (rt_strlen(name) < 66 || name[64] != '-') return search_error("unrecognized archived document name", path);
    memcpy(hash, name, 64); hash[64] = '\0';
    for (i = 0; i < 64; i++) if (tool_hex_value(hash[i]) < 0)
        return search_error("invalid document identity hash", path);
    if (attestation(import, "document", hash, &cache) != 0) return -1;
    if (cache.current[0]) {
        char current[PATH_SIZE];
        PlatformDirEntry info;
        if (rt_join_path(string(import, import->archive), cache.current, current, sizeof(current)) != 0 ||
            platform_get_path_info(current, &info) != 0 || info.is_dir)
            return search_error("attested current document is missing", cache.current);
        if (rt_strcmp(current, path)) return 0;
    }
    if (cache.present && entry->size != cache.size)
        return search_error("document size differs from archive attestation; refresh the archive", path);
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
    if (!e->pdf || cache.observed > e->pdf_observed ||
        (cache.observed == e->pdf_observed && (preferred || (!e->archive && entry->mtime > (long long)e->mtime)))) {
        e->pdf = text(import, path); e->mtime = (unsigned long long)entry->mtime;
        e->pdf_observed = cache.observed; e->pdf_fetched = cache.fetched;
        e->pdf_digest = cache.present ? text(import, cache.digest) : 0;
        if (!e->record.payload) e->record.source = e->pdf;
    }
    return import->builder.failed ? -1 : 0;
}

static int ownership(Import *import) {
    size_t i, j;
    for (i = 0; i < import->entities.count; i++) {
        Entity *paper = entity(import, (uint32_t)i);
        if (paper->deleted || rt_strcmp(string(import, paper->record.kind), "Paper")) continue;
        for (j = 0; j < paper->links.count; j++) {
            const EntityLink *link = (const EntityLink *)tool_array_get_const(&paper->links, j);
            uint32_t target;
            Entity *file;
            size_t k;
            if (link->role != SEARCH_MAIN_FILE && link->role != SEARCH_PAPER_ATTACHMENT) continue;
            target = map_get(import, &import->ids, string(import, link->id));
            if (target == NONE) continue;
            file = entity(import, target);
            if (file->deleted || rt_strcmp(string(import, file->record.kind), "File")) continue;
            for (k = 0; k < file->owners.count; k++)
                if (*(const uint32_t *)tool_array_get_const(&file->owners, k) == i) break;
            if (k == file->owners.count && append_id(&file->owners, (uint32_t)i) != 0) return -1;
        }
    }
    return 0;
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
    if (record->preparation) {
        Json json;
        const char *summary = string(import, record->preparation);
        size_t list;
        if (json_parse(&json, summary, rt_strlen(summary))) goto fail;
        list = json_get(&json, 0, "fields");
        for (i = list+1; i < json_token(&json, list)->next; i = json_token(&json, i)->next) {
            char value[257];
            if (!json_equals(&json, json_get(&json, i, "status"), "rule-supported")) continue;
            if (json_string(&json, json_get(&json, i, "value"), value, sizeof(value)) ||
                tool_byte_buffer_append_cstr(&buffer, value) ||
                tool_byte_buffer_append(&buffer, "\x1e", 1)) {
                json_free(&json); goto fail;
            }
        }
        json_free(&json);
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

static uint32_t identity(Import *import, uint32_t base, const char *suffix) {
    ToolByteBuffer buffer;
    uint32_t result = 0;
    tool_byte_buffer_init(&buffer);
    if (tool_byte_buffer_append_cstr(&buffer, string(import, base)) != 0 ||
        tool_byte_buffer_append_cstr(&buffer, suffix) != 0) import->builder.failed = 1;
    else result = search_string(&import->builder, (const char *)buffer.data, buffer.size);
    tool_byte_buffer_free(&buffer);
    return result;
}

static uint32_t evidence(Import *import, uint32_t object, uint32_t version, uint32_t page,
                         uint32_t section_id, uint32_t property, const char *rule, uint32_t certainty) {
    SearchEvidence *value;
    uint32_t rule_id = text(import, rule), n = (uint32_t)import->builder.evidence.count;
    if (import->builder.evidence.count >= UINT32_MAX ||
        !(value = (SearchEvidence *)tool_array_append(&import->builder.evidence))) {
        import->builder.failed = 1; return NONE;
    }
    rt_memset(value, 0, sizeof(*value));
    value->object = object; value->version = version; value->page = page;
    value->section = section_id; value->property = property;
    value->rule = rule_id; value->certainty = certainty;
    return n;
}

static int model_entity(Import *import, Entity *e) {
    static const char *const date_properties[SEARCH_DATE_END] = {
        "", "date", "", "start", "end", "", "", "", "", "created", "modified", "observedAtEpochSeconds", "", "reference"
    };
    size_t i;
    char year[32];
    if (e->record.year) {
        rt_unsigned_to_string(e->record.year, year, sizeof(year));
        e->dates[SEARCH_REFERENCE_YEAR] = text(import, year);
    }
    for (i = 1; i < SEARCH_DATE_END; i++) if (e->dates[i]) {
        SearchFact *fact = (SearchFact *)tool_array_append(&import->builder.facts);
        uint32_t property = text(import, date_properties[i]);
        if (!fact) return search_error("cannot allocate dated facts", 0);
        fact->record = e->search_record; fact->role = (uint32_t)i; fact->value = e->dates[i];
        fact->evidence = evidence(import, e->record.id, 0, 0, 0, property,
            i == SEARCH_REFERENCE_YEAR ? "reference-prefix-YY-v1" :
            i == SEARCH_OBSERVED ? "verified-archive-observation-v1" : "oparl-property-v1",
            i == SEARCH_REFERENCE_YEAR || i == SEARCH_OBSERVED ? SEARCH_DERIVED : SEARCH_ASSERTED);
    }
    for (i = 0; i < e->links.count; i++) {
        const EntityLink *link = (const EntityLink *)tool_array_get_const(&e->links, i);
        SearchRelation *relation = (SearchRelation *)tool_array_append(&import->builder.relations);
        if (!relation) return search_error("cannot allocate canonical relationships", 0);
        rt_memset(relation, 0, sizeof(*relation));
        relation->source = e->search_record; relation->target = link->id; relation->role = link->role;
        relation->authority = !rt_strcmp(string(import, e->record.kind), "Consultation") ?
            e->authority : SEARCH_AUTHORITY_UNKNOWN;
        relation->evidence = evidence(import, e->record.id, 0, 0, 0, link->property,
                                     "oparl-relationship-v1", SEARCH_ASSERTED);
    }
    if (e->record.body && e->record.system) {
        int explicit_system = 0;
        uint32_t target = map_get(import, &import->ids, string(import, e->record.body));
        for (i = 0; i < e->links.count; i++)
            if (((const EntityLink *)tool_array_get_const(&e->links, i))->role == SEARCH_SYSTEM) explicit_system = 1;
        if (!explicit_system && target != NONE) {
            SearchRelation *relation = (SearchRelation *)tool_array_append(&import->builder.relations);
            if (!relation) return search_error("cannot allocate derived context", 0);
            rt_memset(relation, 0, sizeof(*relation));
            relation->source = e->search_record; relation->target = e->record.system;
            relation->role = SEARCH_CONTEXT_SYSTEM;
            relation->evidence = evidence(import, entity(import, target)->record.id, 0, 0, 0,
                                          text(import, "system"), "body-system-context-v1", SEARCH_DERIVED);
        }
    }
    return import->builder.failed ? -1 : 0;
}

static int document_version(Import *import, Entity *e, const unsigned char *bytes, size_t length) {
    unsigned char digest[32];
    char content[65];
    SearchVersion *version;
    ToolByteBuffer name;
    uint32_t version_id, hash_id;
    crypto_sha256_hash(bytes, length, digest);
    hash_to_hex(digest, 32, content);
    if (e->pdf_digest && rt_strcmp(content, string(import, e->pdf_digest)))
        return search_error("document differs from archive attestation; refresh the archive", string(import, e->pdf));
    hash_id = text(import, content);
    tool_byte_buffer_init(&name);
    if (tool_byte_buffer_append_cstr(&name, "urn:oparl-search:document:") != 0 ||
        tool_byte_buffer_append_cstr(&name, string(import, e->hash)) != 0 ||
        tool_byte_buffer_append_cstr(&name, ":") != 0 ||
        tool_byte_buffer_append_cstr(&name, content) != 0) {
        tool_byte_buffer_free(&name); return search_error("cannot allocate document version ID", 0);
    }
    version_id = search_string(&import->builder, (const char *)name.data, name.size);
    tool_byte_buffer_free(&name);
    version = (SearchVersion *)tool_array_append(&import->builder.versions);
    if (!version) return search_error("cannot allocate document versions", 0);
    rt_memset(version, 0, sizeof(*version));
    version->record = e->search_record; version->id = version_id; version->hash = hash_id;
    version->bytes = length; version->observed_at = e->pdf_observed; version->fetched_at = e->pdf_fetched;
    e->version = version_id;
    return import->builder.failed ? -1 : 0;
}

typedef struct { Import *import; uint32_t entity; } PageContext;
static int extracted_page(unsigned int number, const char *body, size_t length,
                            unsigned int warnings, void *user) {
    PageContext *context = (PageContext *)user;
    Import *import = context->import;
    Entity *e = entity(import, context->entity);
    SearchRecord record = e->record;
    uint32_t n;
    char number_text[32], digest_text[65];
    unsigned char digest[32];
    SearchSection *section;
    record.kind = text(import, "Page"); record.page = number; record.warnings = warnings;
    record.parent = e->search_record;
    record.source = e->pdf;
    rt_unsigned_to_string(number, number_text, sizeof(number_text));
    record.identity = identity(import, e->version, ":page:");
    record.identity = identity(import, record.identity, number_text);
    if (record_text(import, &record, body, length) != 0) return -1;
    n = search_add_record(&import->builder, &record);
    if (n == NONE) return -1;
    crypto_sha256_hash((const unsigned char *)body, length, digest);
    hash_to_hex(digest, 32, digest_text);
    {
        uint32_t section_id = identity(import, record.identity, ":body:");
        section_id = identity(import, section_id, digest_text);
        section = (SearchSection *)tool_array_append(&import->builder.sections);
        if (!section) return search_error("cannot allocate text sections", 0);
        rt_memset(section, 0, sizeof(*section));
        section->record = n; section->id = section_id;
        section->start = record.body_start; section->length = (uint32_t)length;
        section->evidence = evidence(import, e->record.id, e->version, number, section_id,
                                    text(import, "text"), "pdf-text-v1", SEARCH_DERIVED);
    }
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

static int prepare_entity(Import *import, Entity *e) {
    unsigned char *bytes = 0;
    char hash[65];
    size_t length;
    PreparedDocument doc;
    ToolByteBuffer summary;
    int status;
    if (!import->prepared || !e->pdf || rt_strcmp(string(import, e->record.kind), "File")) return 0;
    if (tool_read_all_input(string(import, e->pdf), &bytes, &length))
        return search_error("cannot read archived document", string(import, e->pdf));
    prepare_hash(bytes, length, hash); rt_free(bytes);
    if (e->pdf_digest && rt_strcmp(hash, string(import, e->pdf_digest)))
        return search_error("document differs from archive attestation", string(import, e->pdf));
    e->pdf_digest = text(import, hash);
    status = prepared_open(&doc, import->prepared, import->preparation_key, hash, length);
    tool_byte_buffer_init(&summary);
    if (!status) status = prepared_summary(&summary, &doc);
    if (!status) e->record.preparation = search_string(&import->builder, (const char *)summary.data, summary.size);
    tool_byte_buffer_free(&summary); prepared_close(&doc);
    return status || import->builder.failed ? -1 : 0;
}

static int prepared_pages(Import *import, Entity *e, PageContext *context, size_t length) {
    PreparedDocument doc;
    const Json *json = &doc.json;
    size_t pages, i, fields;
    int status = prepared_open(&doc, import->prepared, import->preparation_key,
                               string(import, e->pdf_digest), length);
    if (status) { prepared_close(&doc); return -1; }
    fields = json_get(json, doc.data, "fields");
    for (i = fields+1; i < json_token(json, fields)->next; i = json_token(json, i)->next) {
        char value[32], rule[128];
        unsigned long long page;
        SearchFact *fact;
        if (!json_equals(json, json_get(json, i, "field"), "document.date") ||
            !json_equals(json, json_get(json, i, "status"), "rule-supported")) continue;
        if (e->record.date || e->dates[SEARCH_DOCUMENT_DATE]) continue;
        if (json_string(json, json_get(json, i, "value"), value, sizeof(value)) ||
            json_string(json, json_get(json, i, "rule"), rule, sizeof(rule)) ||
            json_uint(json, json_get(json, i, "page"), &page)) { status = -1; goto done; }
        fact = (SearchFact *)tool_array_append(&import->builder.facts);
        if (!fact) { status = search_error("cannot allocate prepared document date", 0); goto done; }
        fact->record = e->search_record; fact->role = SEARCH_DOCUMENT_DATE;
        fact->value = text(import, value); e->dates[SEARCH_DOCUMENT_DATE] = fact->value;
        fact->evidence = evidence(import, e->record.id, e->version, (uint32_t)page, 0,
                                  text(import, "preparation.fields.document.date"), rule, SEARCH_DERIVED);
    }
    pages = json_get(json, doc.data, "pages");
    for (i = pages+1; i < json_token(json, pages)->next; i = json_token(json, i)->next) {
        size_t token = json_get(json, i, "text");
        const JsonToken *t = json_token(json, token);
        char *body = (char *)rt_malloc(t->end-t->start+1);
        unsigned long long number, warnings;
        if (!body || json_string(json, token, body, t->end-t->start+1) ||
            json_uint(json, json_get(json, i, "page"), &number) ||
            json_uint(json, json_get(json, i, "warnings"), &warnings)) {
            rt_free(body); status = search_error("invalid prepared page", string(import, e->pdf)); goto done;
        }
        status = extracted_page((unsigned int)number, body, rt_strlen(body), (unsigned int)warnings, context);
        rt_free(body);
        if (status) goto done;
    }
    if (json_equals(json, json_get(json, doc.data, "status"), "failed")) {
        char error[256];
        import->failed_pdfs++;
        if (json_string(json, json_get(json, doc.data, "error"), error, sizeof(error))) {
            status = search_error("invalid prepared extraction error", string(import, e->pdf)); goto done;
        }
        status = issue(import, e, error);
    }
done:prepared_close(&doc);
    return status || import->builder.failed ? -1 : 0;
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
        if (prepare_entity(import, e)) return -1;
        {
            const char *detail = string(import, e->detail);
            if (record_text(import, &e->record, detail, rt_strlen(detail)) != 0) return -1;
        }
        if (e->record.body && !e->record.system) {
            uint32_t target = map_get(import, &import->ids, string(import, e->record.body));
            if (target != NONE && !entity(import, target)->deleted) {
                if (rt_strcmp(string(import, entity(import, target)->record.kind), "Body"))
                    return search_error("body context does not reference a Body", string(import, e->record.id));
                e->record.system = entity(import, target)->record.system;
            }
        }
        e->search_record = search_add_record(&import->builder, &e->record);
        if (e->search_record == NONE) return -1;
        if (model_entity(import, e) != 0) return -1;
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
        if (document_version(import, e, bytes, length) != 0) { rt_free(bytes); return -1; }
        context.import = import; context.entity = (uint32_t)i;
        if (import->prepared) {
            if (prepared_pages(import, e, &context, length)) { rt_free(bytes); return -1; }
        } else if (length < 5 || memcmp(bytes, "%PDF-", 5) != 0 ||
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
            rt_write_cstr(1, "Usage: oparl-index [--input ARCHIVE_OR_PARENT] [--output INDEX] [--prepared DIRECTORY]\n"
                "Build a binary metadata/full-text index; originals stay unchanged.\n"
                "Exit: 0 complete, 1 error, 3 published index with documented coverage gaps.\n");
            result = 0; goto done;
        }
        if (i + 1 == (size_t)argc) { search_error("missing option value", argv[i]); goto done; }
        if (rt_strcmp(argv[i], "--input") == 0) input = argv[++i];
        else if (rt_strcmp(argv[i], "--output") == 0) output = argv[++i];
        else if (rt_strcmp(argv[i], "--prepared") == 0) import.prepared = argv[++i];
        else { search_error("unknown option", argv[i]); goto done; }
    }
    if (import.prepared && prepare_manifest_read(import.prepared, import.preparation_key)) goto done;
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
        if (walk_archive(path, document_file, &import, 0) != 0) {
            search_error("cannot discover archived documents", path); goto done;
        }
    }
    if (tool_path_append_suffix(output, ".coverage.jsonl", report, sizeof(report)) != 0 ||
        tool_path_append_suffix(report, ".part", part, sizeof(part)) != 0 ||
        tool_ensure_parent_dirs(part, scratch, sizeof(scratch)) != 0) {
        search_error("cannot prepare coverage report", output); goto done;
    }
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

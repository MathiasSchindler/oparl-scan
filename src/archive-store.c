#include "archive-store.h"
#include "archive-version.h"
#include "archive-walk.h"
#include "crypto/sha256.h"
#include "hash_util.h"
#include "runtime.h"

static const char *const type_names[TYPE_COUNT] = {"System","Body","Paper","Meeting","AgendaItem",
    "Consultation","File","Organization","Person","Membership","Location","LegislativeTerm","Resource"};

const char *archive_type_name(unsigned int type) {
    return type < TYPE_COUNT ? type_names[type] : "Resource";
}

unsigned int archive_url_type(const char *url) {
    static const char *const paths[] = {"/system","/bodies/","/papers/","/meetings/","/agendaItems/",
        "/consultations/","/files/","/organizations/","/persons/","/memberships/","/locations/","/legislativeTerms/"};
    size_t i, j;
    for (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++)
        for (j = 0; url[j]; j++) if (!rt_strncmp(url + j, paths[i], rt_strlen(paths[i]))) return (unsigned int)i;
    return TYPE_RESOURCE;
}

size_t archive_resource_type(const ToolJson *json) {
    size_t type = tool_json_get(json, 0, "type");
    if (tool_json_equals(json, type, "at") || tool_json_equals(json, type, "gr")) {
        size_t legacy = tool_json_get(json, 0, "Type");
        if (tool_json_equals(json, legacy, "https://schema.oparl.org/1.1/Organization"))
            return legacy;
    }
    return type;
}

int archive_private_item(const ToolJson *json, size_t object) {
    size_t index = tool_json_get(json, object, "public");
    const ToolJsonToken *token = tool_json_token(json, index);
    return token && token->kind == TOOL_JSON_BOOL && !tool_json_true(json, index);
}

int archive_calendar_member(const Archive *archive, const ToolJson *json, size_t object) {
    char start[128], day[11];
    const ToolJsonToken *token = tool_json_token(json, tool_json_get(json, object, "start"));
    if (!token || token->kind == TOOL_JSON_NULL) return 2;
    if (tool_json_string(json, tool_json_get(json, object, "start"), start, sizeof(start)) != 0 ||
        rt_strlen(start) < 11 || start[10] != 'T') return ris_error("invalid Meeting.start", 0);
    memcpy(day, start, 10); day[10] = 0;
    if (!archive_valid_date(day)) return ris_error("invalid calendar date in Meeting.start", start);
    return !archive->calendar_from[0] ||
        (rt_strcmp(day, archive->calendar_from) >= 0 && rt_strcmp(day, archive->calendar_through) <= 0);
}

void archive_hash_text(const char *text, char out[65]) {
    unsigned char digest[32];
    crypto_sha256_hash((const unsigned char *)text, rt_strlen(text), digest);
    hash_to_hex(digest, sizeof(digest), out);
}

static size_t hash_slot(const char *hash, size_t capacity) {
    size_t value = 0, i;
    for (i = 0; i < 16; i++) value = value * 33U + (unsigned char)hash[i];
    return value & (capacity - 1);
}

int archive_hash_seen(SeenSet *set, const char *hash, int insert) {
    size_t slot;
    if (insert && set->count >= set->capacity / 2) {
        size_t capacity = set->capacity == 0 ? 256 : set->capacity * 2;
        size_t i;
        char (*entries)[65] = (char (*)[65])rt_malloc_array(capacity, 65);
        if (entries == 0) return ris_error("cannot allocate resource identity set", 0);
        rt_memset(entries, 0, capacity * 65);
        for (i = 0; i < set->capacity; i++) {
            if (set->entries[i][0] == '\0') continue;
            slot = hash_slot(set->entries[i], capacity);
            while (entries[slot][0] != '\0') slot = (slot + 1) & (capacity - 1);
            memcpy(entries[slot], set->entries[i], 65);
        }
        rt_free(set->entries);
        set->entries = entries;
        set->capacity = capacity;
    }
    if (set->capacity == 0) return 0;
    slot = hash_slot(hash, set->capacity);
    while (set->entries[slot][0] != '\0') {
        if (rt_strcmp(set->entries[slot], hash) == 0) return 1;
        slot = (slot + 1) & (set->capacity - 1);
    }
    if (insert) {
        memcpy(set->entries[slot], hash, 65);
        set->count++;
    }
    return 0;
}

int archive_known_reference(SeenSet *set, const ToolJson *json, size_t index) {
    const ToolJsonToken *token = tool_json_token(json, index);
    char id[RIS_URL_CAPACITY], hash[65];
    size_t child;
    if (!token || token->kind == TOOL_JSON_NULL) return 0;
    if (token->kind == TOOL_JSON_ARRAY) {
        for (child = index + 1; child < token->next; child = tool_json_token(json, child)->next)
            if (archive_known_reference(set, json, child)) return 1;
        return 0;
    }
    if (token->kind == TOOL_JSON_OBJECT) index = tool_json_get(json, index, "id");
    if (tool_json_string(json, index, id, sizeof(id)) != 0) return 0;
    archive_hash_text(id, hash);
    return archive_hash_seen(set, hash, 0);
}

int archive_path(const Archive *archive, const char *directory,
                 const char *name, char *out) {
    char parent[PATH_CAPACITY];
    if (rt_join_path(archive->output, directory, parent, sizeof(parent)) != 0 ||
        rt_join_path(parent, name, out, PATH_CAPACITY) != 0)
        return ris_error("archive path is too long", archive->output);
    return 0;
}

int archive_prepare_directory(const Archive *archive, const char *name) {
    char path[PATH_CAPACITY], scratch[PATH_CAPACITY];
    if (archive_path(archive, name, "_", path) != 0) return -1;
    if (tool_ensure_parent_dirs(path, scratch, sizeof(scratch)) != 0)
        return ris_error("cannot create archive directory", path);
    return 0;
}

int archive_temporary_path(const char *path, char out[PATH_CAPACITY]) {
    size_t length = rt_strlen(path);
    if (length + 5 >= PATH_CAPACITY) return ris_error("temporary path is too long", path);
    memcpy(out, path, length);
    memcpy(out + length, ".part", 6);
    return 0;
}

int archive_content_digest(const char *path, char out[65]) {
    return archive_content_hash(path, out) == 0 ? 0 : ris_error("cannot hash archived content", path);
}

int archive_preserve_revision(Archive *archive, const char *kind, const char *key,
                              const char *path, const CacheEntry *cache) {
    char relative[256];
    return archive_preserve(archive->output, kind, key, path, cache->digest,
                            relative, sizeof(relative)) == 0 ? 0 :
        ris_error("cannot preserve or verify immutable revision", path);
}

int archive_observation(Archive *archive, const char *kind, const char *key,
                        const char *url, const CacheEntry *cache, int reused) {
    int fd = archive->observations_fd;
    char hash[65], directory[128], identity[256], relative[256];
    const char *history_kind = !rt_strcmp(kind, "document") ? "documents" : "metadata";
    archive_hash_text(key, hash);
    if (rt_join_path("history", history_kind, directory, sizeof(directory)) != 0 ||
        rt_join_path(directory, hash, identity, sizeof(identity)) != 0 ||
        rt_join_path(identity, cache->digest, relative, sizeof(relative)) != 0)
        return ris_error("revision observation path is too long", key);
    return rt_write_cstr(fd, "{\"kind\":") || tool_json_write_string(fd, kind) ||
        tool_json_field_string(fd, "key", key) || tool_json_field_string(fd, "requestUrl", url) ||
        tool_json_field_string(fd, "revisionPath", relative) ||
        tool_json_field_string(fd, "sha256", cache->digest) ||
        tool_json_field_uint(fd, "bytes", cache->size) ||
        tool_json_field_uint(fd, "observedAtEpochSeconds", cache->observed) ||
        tool_json_field_uint(fd, "fetchedAtEpochSeconds", cache->fetched) ||
        (cache->local_modified != 0 &&
         tool_json_field_uint(fd, "localModifiedAtEpochSeconds", cache->local_modified)) ||
        tool_json_field_bool(fd, "revalidated", reused) || rt_write_cstr(fd, "}\n") ?
        ris_error("cannot write revision observation", url) : 0;
}

static int cache_path(const Archive *archive, const char *kind, const char *key, char *path) {
    char hash[65], name[96];
    size_t length = rt_strlen(kind);
    archive_hash_text(key, hash);
    memcpy(name, kind, length);
    name[length++] = '-';
    memcpy(name + length, hash, 64);
    memcpy(name + length + 64, ".json", 6);
    return archive_path(archive, "cache", name, path);
}

int archive_load_cache(const Archive *archive, const char *kind, const char *key,
                       const char *url, char *content, unsigned long long limit,
                       CacheEntry *entry) {
    char path[PATH_CAPACITY], digest[65], request_url[RIS_URL_CAPACITY];
    PlatformDirEntry info;
    unsigned char *bytes = 0;
    size_t length, i;
    ToolJson json;
    const ToolJsonToken *framed;
    int valid;
    rt_memset(entry, 0, sizeof(*entry));
    if (cache_path(archive, kind, key, path) != 0) return -1;
    if (platform_get_path_info(path, &info) != 0) return 0;
    if (info.is_dir || info.size > 16384)
        return ris_error("invalid cache record path; use --refresh", path);
    if (tool_read_all_input(path, &bytes, &length) != 0)
        return ris_error("cannot read cache record", path);
    valid = tool_json_parse(&json, (const char *)bytes, length) == 0;
    if (valid) {
        framed = tool_json_token(&json, tool_json_get(&json, 0, "framed"));
        valid = tool_json_string(&json, tool_json_get(&json, 0, "requestUrl"), request_url,
                            sizeof(request_url)) == 0 &&
                rt_strcmp(request_url, url) == 0 &&
                tool_json_string(&json, tool_json_get(&json, 0, "url"), entry->validators.url,
                            sizeof(entry->validators.url)) == 0 &&
                tool_json_string(&json, tool_json_get(&json, 0, "etag"), entry->validators.etag,
                            sizeof(entry->validators.etag)) == 0 &&
                tool_json_string(&json, tool_json_get(&json, 0, "lastModified"), entry->validators.modified,
                            sizeof(entry->validators.modified)) == 0 &&
                tool_json_string(&json, tool_json_get(&json, 0, "sha256"), entry->digest,
                            sizeof(entry->digest)) == 0 &&
                rt_strlen(entry->digest) == 64 &&
                tool_json_uint(&json, tool_json_get(&json, 0, "size"), &entry->size) == 0 &&
                framed != 0 && framed->kind == TOOL_JSON_BOOL &&
                entry->size <= limit &&
                http_valid_validators(&archive->http, &entry->validators);
        if (valid)
            for (i = 0; i < 64; i++)
                if (tool_hex_value(entry->digest[i]) < 0) valid = 0;
        entry->framed = tool_json_true(&json, tool_json_get(&json, 0, "framed"));
        if (tool_json_get(&json, 0, "observedAtEpochSeconds") != TOOL_JSON_NONE &&
            tool_json_uint(&json, tool_json_get(&json, 0, "observedAtEpochSeconds"), &entry->observed) != 0) valid = 0;
        if (tool_json_get(&json, 0, "fetchedAtEpochSeconds") != TOOL_JSON_NONE &&
            tool_json_uint(&json, tool_json_get(&json, 0, "fetchedAtEpochSeconds"), &entry->fetched) != 0) valid = 0;
        if (tool_json_get(&json, 0, "localModifiedAtEpochSeconds") != TOOL_JSON_NONE &&
            tool_json_uint(&json, tool_json_get(&json, 0, "localModifiedAtEpochSeconds"), &entry->local_modified) != 0) valid = 0;
        if (tool_json_get(&json, 0, "currentPath") != TOOL_JSON_NONE &&
            tool_json_string(&json, tool_json_get(&json, 0, "currentPath"), entry->current, sizeof(entry->current)) != 0) valid = 0;
        tool_json_free(&json);
    }
    rt_free(bytes);
    if (!valid) {
        ris_warning("invalid cache record; fetching resource again", path);
        return 0;
    }
    if (!rt_strcmp(kind, "document") && entry->current[0]) {
        char hash[65];
        size_t n;
        archive_hash_text(key, hash);
        if (rt_strncmp(entry->current, "documents/", 10) ||
            rt_strncmp(entry->current + 10, hash, 64) || entry->current[74] != '-')
            return ris_error("invalid cached document path", path);
        for (n = 75; entry->current[n]; n++)
            if (entry->current[n] == '/' || entry->current[n] == '\\')
                return ris_error("invalid cached document path", path);
        if (rt_join_path(archive->output, entry->current, content, PATH_CAPACITY) != 0) return -1;
    }
    if (platform_get_path_info(content, &info) != 0) return 0;
    if (info.is_dir) return ris_error("cached content is a directory", content);
    if (info.size != entry->size) {
        ris_warning("cache content changed; fetching resource again", content);
        return 0;
    }
    if (archive_content_digest(content, digest) != 0) return -1;
    if (rt_strcmp(digest, entry->digest) != 0) {
        ris_warning("cache content changed; fetching resource again", content);
        return 0;
    }
    return 1;
}

int archive_load_legacy_document(const Archive *archive, const char *id,
                                 const char *content, CacheEntry *entry) {
    char path[PATH_CAPACITY];
    PlatformDirEntry info;
    if (cache_path(archive, "document", id, path) != 0) return -1;
    if (platform_get_path_info(path, &info) == 0) return 0;
    if (platform_get_path_info(content, &info) != 0) return 0;
    if (info.is_dir || info.size > FILE_LIMIT)
        return ris_error("invalid legacy document path or size", content);
    if (info.mtime <= 0 || (unsigned long long)info.mtime >
                           (unsigned long long)platform_get_epoch_time()) {
        ris_warning("legacy document has no usable local timestamp; downloading", content);
        return 0;
    }
    rt_memset(entry, 0, sizeof(*entry));
    entry->size = info.size;
    entry->local_modified = (unsigned long long)info.mtime;
    if (archive_content_digest(content, entry->digest) != 0) return -1;
    return 1;
}

int archive_save_cache(const Archive *archive, const char *kind, const char *key,
                       const char *url, const CacheEntry *entry) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY];
    int fd, result;
    if (cache_path(archive, kind, key, path) != 0 || archive_temporary_path(path, part) != 0) return -1;
    fd = platform_open_write(part, 0644);
    if (fd < 0) return ris_error("cannot write cache record", path);
    result = rt_write_cstr(fd, "{\"requestUrl\":") ||
             tool_json_write_string(fd, url) ||
             tool_json_field_string(fd, "url", entry->validators.url) ||
             tool_json_field_string(fd, "etag", entry->validators.etag) ||
             tool_json_field_string(fd, "lastModified", entry->validators.modified) ||
             tool_json_field_string(fd, "sha256", entry->digest) ||
             tool_json_field_uint(fd, "size", entry->size) ||
             tool_json_field_bool(fd, "framed", entry->framed) ||
             tool_json_field_string(fd, "currentPath", entry->current) ||
             tool_json_field_uint(fd, "observedAtEpochSeconds", entry->observed) ||
             tool_json_field_uint(fd, "fetchedAtEpochSeconds", entry->fetched) ||
             (entry->local_modified != 0 &&
              tool_json_field_uint(fd, "localModifiedAtEpochSeconds", entry->local_modified)) ||
             rt_write_cstr(fd, "}\n");
    if (platform_close(fd) != 0) result = -1;
    if (result != 0 || platform_rename_path(part, path) != 0)
        return ris_error("cannot publish cache record", path);
    return 0;
}

int archive_record_missing(Archive *archive, const char *url, const char *kind) {
    char hash[65];
    int fd = archive->missing_fd;
    archive_hash_text(url, hash);
    if (archive_hash_seen(&archive->seen_missing, hash, 0)) return 0;
    if (rt_write_cstr(fd, "{\"url\":") != 0 ||
        tool_json_write_string(fd, url) != 0 ||
        tool_json_field_string(fd, "kind", kind) != 0 ||
        tool_json_field_uint(fd, "status", 404) != 0 ||
        rt_write_cstr(fd, "}\n") != 0)
        return ris_error("cannot write missing resource report", url);
    if (archive_hash_seen(&archive->seen_missing, hash, 1) < 0) return -1;
    archive->missing_resources++;
    return archive_write_summary(archive, 0, 0);
}

int archive_store_collection_object(Archive *archive, const ToolJson *json, size_t object, const char *source) {
    char id[RIS_URL_CAPACITY], hash[65], name[80], path[PATH_CAPACITY], part[PATH_CAPACITY];
    const ToolJsonToken *token = tool_json_token(json, object);
    CacheEntry cache;
    unsigned char digest[32];
    int cached, fd, result;
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0 ||
        !http_valid_url(&archive->http, id)) return ris_error("invalid collection object ID", source);
    archive_hash_text(id, hash); memcpy(name, hash, 64); memcpy(name + 64, ".json", 6);
    if (archive_path(archive, "metadata", name, path) != 0 || archive_temporary_path(path, part) != 0) return -1;
    cached = archive_load_cache(archive, "metadata", id, id, path, META_LIMIT, &cache);
    if (cached < 0 || (cached && archive_preserve_revision(archive, "metadata", id, path, &cache) != 0)) return -1;
    rt_memset(&cache, 0, sizeof(cache));
    rt_copy_string(cache.validators.url, sizeof(cache.validators.url), id);
    cache.size = token->end - token->start; cache.observed = (unsigned long long)platform_get_epoch_time();
    crypto_sha256_hash((const unsigned char *)json->text + token->start, (size_t)cache.size, digest);
    hash_to_hex(digest, 32, cache.digest); cache.framed = 1;
    fd = platform_open_write(part, 0644);
    if (fd < 0) return ris_error("cannot store collection object", id);
    result = rt_write_all(fd, json->text + token->start, (size_t)cache.size);
    if (platform_close(fd) != 0) result = -1;
    if (result != 0 || archive_preserve_revision(archive, "metadata", id, part, &cache) != 0 ||
        platform_rename_path(part, path) != 0 || archive_save_cache(archive, "metadata", id, id, &cache) != 0 ||
        archive_observation(archive, tool_json_true(json, tool_json_get(json, object, "deleted")) ?
                            "collection-tombstone" : "collection-object", id, source, &cache, 0) != 0)
        return ris_error("cannot publish collection object", id);
    return 0;
}

static int completed_access_denied_run(const ToolJson *json, unsigned long long *checkpoint) {
    unsigned long long started, finished, unavailable, value;
    size_t i, types = tool_json_get(json, 0, "types");
    if (!tool_json_equals(json, tool_json_get(json, 0, "state"), "incomplete") ||
        !tool_json_equals(json, tool_json_get(json, 0, "syncMode"), "full") ||
        tool_json_true(json, tool_json_get(json, 0, "metadataOnly")) ||
        tool_json_true(json, tool_json_get(json, 0, "limited")) ||
        tool_json_uint(json, tool_json_get(json, 0, "runStartedAtEpochSeconds"), &started) != 0 ||
        tool_json_uint(json, tool_json_get(json, 0, "runFinishedAtEpochSeconds"), &finished) != 0 ||
        !started || finished < started ||
        tool_json_uint(json, tool_json_get(json, 0, "unavailableDocuments"), &unavailable) != 0 ||
        !unavailable ||
        tool_json_uint(json, tool_json_get(json, 0, "missingResources"), &value) != 0 || value ||
        tool_json_uint(json, tool_json_get(json, 0, "unknownMeetingDates"), &value) != 0 || value)
        return 0;
    for (i = 0; i < TYPE_COUNT; i++) {
        size_t type = tool_json_get(json, types, type_names[i]);
        unsigned long long scheduled, finished_work, errors;
        if (tool_json_uint(json, tool_json_get(json, type, "scheduledWorkItems"), &scheduled) != 0 ||
            tool_json_uint(json, tool_json_get(json, type, "finishedWorkItems"), &finished_work) != 0 ||
            scheduled != finished_work ||
            (i <= TYPE_BODY && !scheduled) ||
            tool_json_uint(json, tool_json_get(json, type, "errors"), &errors) != 0 ||
            errors != (i == TYPE_FILE ? unavailable : 0) ||
            tool_json_uint(json, tool_json_get(json, type, "missingResources"), &value) != 0 || value ||
            tool_json_uint(json, tool_json_get(json, type, "incompleteCollections"), &value) != 0 || value)
            return 0;
    }
    *checkpoint = started;
    return 1;
}

int archive_validate(Archive *archive) {
    char path[PATH_CAPACITY];
    unsigned char *bytes = 0;
    size_t length, token;
    PlatformDirEntry info;
    ToolJson json;
    unsigned long long year = 0;
    int valid;
    if (rt_join_path(archive->output, "archive.json", path, sizeof(path)) != 0)
        return ris_error("summary path is too long", 0);
    if (platform_get_path_info(path, &info) != 0) return 0;
    if (info.size > META_LIMIT || tool_read_all_input(path, &bytes, &length) != 0)
        return ris_error("cannot read existing archive identity", path);
    if (tool_json_parse(&json, (const char *)bytes, length) != 0) {
        rt_free(bytes);
        return ris_error("invalid existing archive identity", path);
    }
    token = tool_json_get(&json, 0, "referenceYear");
    valid = (token == TOOL_JSON_NONE || tool_json_uint(&json, token, &year) == 0) &&
            year == (archive->calendar_only ? 0 : archive->year) &&
            tool_json_equals(&json, tool_json_get(&json, 0, "system"), archive->system);
    if (tool_json_get(&json, 0, "calendarFrom") != TOOL_JSON_NONE)
        valid = valid && tool_json_equals(&json, tool_json_get(&json, 0, "calendarFrom"), archive->calendar_from) &&
            tool_json_equals(&json, tool_json_get(&json, 0, "calendarThrough"), archive->calendar_through) &&
            tool_json_true(&json, tool_json_get(&json, 0, "calendarOnly")) == archive->calendar_only;
    if (tool_json_get(&json, 0, "lastSuccessfulRunEndEpochSeconds") != TOOL_JSON_NONE &&
        tool_json_uint(&json, tool_json_get(&json, 0, "lastSuccessfulRunEndEpochSeconds"), &archive->last_success) != 0)
        valid = 0;
    archive->baseline_complete = tool_json_true(&json, tool_json_get(&json, 0, "complete"));
    archive->sync_metadata_only = tool_json_true(&json, tool_json_get(&json, 0, "metadataOnly"));
    if (tool_json_get(&json, 0, "syncCheckpointEpochSeconds") != TOOL_JSON_NONE) {
        archive->sync_state_present = 1;
        if (tool_json_uint(&json, tool_json_get(&json, 0, "syncCheckpointEpochSeconds"),
                      &archive->sync_checkpoint) != 0 ||
            tool_json_string(&json, tool_json_get(&json, 0, "syncCheckpointSource"),
                        archive->sync_source, sizeof(archive->sync_source)) != 0)
            valid = 0;
        {
            const ToolJsonToken *baseline = tool_json_token(&json, tool_json_get(&json, 0, "syncBaselineComplete"));
            const ToolJsonToken *metadata_only = tool_json_token(&json, tool_json_get(&json, 0, "syncMetadataOnly"));
            if (!baseline || baseline->kind != TOOL_JSON_BOOL ||
                !metadata_only || metadata_only->kind != TOOL_JSON_BOOL) valid = 0;
            archive->baseline_complete = tool_json_true(&json, tool_json_get(&json, 0, "syncBaselineComplete"));
            archive->sync_metadata_only = tool_json_true(&json, tool_json_get(&json, 0, "syncMetadataOnly"));
        }
    }
    {
        size_t types = tool_json_get(&json, 0, "types"), i;
        for (i = 0; i < TYPE_COUNT; i++) {
            size_t value = tool_json_get(&json, tool_json_get(&json, types, type_names[i]), "lastSuccessfulRunEndEpochSeconds");
            if (value != TOOL_JSON_NONE && tool_json_uint(&json, value, &archive->types[i].last_success) != 0) valid = 0;
        }
    }
    if (tool_json_equals(&json, tool_json_get(&json, 0, "state"), "incomplete") &&
        tool_json_get(&json, 0, "unavailableDocuments") != TOOL_JSON_NONE &&
        tool_json_uint(&json, tool_json_get(&json, 0, "unavailableDocuments"),
                  &archive->previous_unavailable) != 0)
        valid = 0;
    if (valid && archive->sync_state_present && !archive->sync_checkpoint &&
        completed_access_denied_run(&json, &archive->sync_checkpoint)) {
        archive->baseline_complete = 1;
        archive->sync_metadata_only = 0;
        rt_copy_string(archive->sync_source, sizeof(archive->sync_source), "completed-metadata-run-start");
        ris_warning("adopting completed metadata checkpoint; access-denied documents will be retried", path);
    }
    tool_json_free(&json);
    rt_free(bytes);
    return valid ? 0 : ris_error("archive has a different system/year; use a separate output directory", path);
}

static int remember_links(Archive *archive, SeenSet *set, const ToolJson *json, size_t index, unsigned int type) {
    const ToolJsonToken *token = tool_json_token(json, index);
    char id[RIS_URL_CAPACITY], hash[65];
    size_t child;
    if (!token || token->kind == TOOL_JSON_NULL) return 0;
    if (token->kind == TOOL_JSON_ARRAY) {
        for (child = index + 1; child < token->next; child = tool_json_token(json, child)->next)
            if (remember_links(archive, set, json, child, type) != 0) return -1;
        return 0;
    }
    if (token->kind == TOOL_JSON_OBJECT) index = tool_json_get(json, index, "id");
    if (tool_json_string(json, index, id, sizeof(id)) != 0 || !http_valid_url(&archive->http, id))
        return ris_error("invalid archived resource reference; use --full", 0);
    if (archive_url_type(id) != type) return 0;
    archive_hash_text(id, hash);
    return archive_hash_seen(set, hash, 1) < 0 ? -1 : 0;
}

static int archived_paper(const Archive *archive, const ToolJson *json, size_t index) {
    const ToolJsonToken *token = tool_json_token(json, index);
    char id[RIS_URL_CAPACITY], hash[65], name[80], path[PATH_CAPACITY];
    PlatformDirEntry info;
    size_t child;
    if (!token || token->kind == TOOL_JSON_NULL) return 0;
    if (token->kind == TOOL_JSON_ARRAY) {
        for (child = index + 1; child < token->next; child = tool_json_token(json, child)->next)
            if (archived_paper(archive, json, child)) return 1;
        return 0;
    }
    if (token->kind == TOOL_JSON_OBJECT) index = tool_json_get(json, index, "id");
    if (tool_json_string(json, index, id, sizeof(id)) != 0 || archive_url_type(id) != TYPE_PAPER) return 0;
    archive_hash_text(id, hash);
    memcpy(name, hash, 64); memcpy(name + 64, ".json", 6);
    return archive_path(archive, "metadata", name, path) == 0 &&
           platform_get_path_info(path, &info) == 0 && !info.is_dir;
}

static int snapshot_metadata(const char *path, const PlatformDirEntry *info, int depth,
                              ToolWalkControl *control, void *user) {
    Archive *archive = (Archive *)user;
    unsigned char *bytes = 0;
    size_t length, i, type;
    char id[RIS_URL_CAPACITY], hash[65];
    const char *name = path;
    const char *keys[4] = {0,0,0,0};
    ToolJson json;
    int result = 0;
    if (info->is_dir) { if (depth) control->prune = 1; return 0; }
    for (i = 0; path[i]; i++) if (path[i] == '/') name = path + i + 1;
    if (rt_strlen(name) != 69 || rt_strcmp(name + 64, ".json")) return 0;
    if (!archive->sync_state_present) {
        if (info->mtime <= 0 || (unsigned long long)info->mtime > archive->started)
            return ris_error("invalid local metadata timestamp; use --full", path);
        if (!archive->oldest_metadata || (unsigned long long)info->mtime < archive->oldest_metadata)
            archive->oldest_metadata = (unsigned long long)info->mtime;
    }
    if (info->size > META_LIMIT || tool_read_all_input(path, &bytes, &length) != 0)
        return ris_error("cannot read existing metadata; use --full", path);
    if (tool_json_parse(&json, (const char *)bytes, length) != 0) {
        rt_free(bytes);
        return ris_error("invalid existing metadata; use --full", path);
    }
    if (tool_json_string(&json, tool_json_get(&json, 0, "id"), id, sizeof(id)) != 0) goto done;
    archive_hash_text(id, hash);
    if (memcmp(hash, name, 64)) goto done;
    if (!http_valid_url(&archive->http, id)) {
        result = ris_error("invalid archived resource URL; use --full", path); goto done;
    }
    if (archive_hash_seen(&archive->known_resources, hash, 1) < 0) { result = -1; goto done; }
    type = archive_resource_type(&json);
    if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/File")) {
        result = archive_hash_seen(&archive->known_files, hash, 1) < 0 ? -1 : 0;
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Paper")) {
        keys[0] = "mainFile"; keys[1] = "auxiliaryFile";
        result = remember_links(archive, &archive->paper_context, &json,
                                tool_json_get(&json, 0, "consultation"), TYPE_CONSULTATION);
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Consultation") &&
               archived_paper(archive, &json, tool_json_get(&json, 0, "paper"))) {
        if (archive_hash_seen(&archive->paper_context, hash, 1) < 0 ||
            remember_links(archive, &archive->paper_context, &json,
                           tool_json_get(&json, 0, "agendaItem"), TYPE_AGENDA) != 0 ||
            remember_links(archive, &archive->paper_context, &json,
                           tool_json_get(&json, 0, "agendaitem"), TYPE_AGENDA) != 0) result = -1;
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Meeting") && !archive_private_item(&json, 0)) {
        int member = archive_calendar_member(archive, &json, 0);
        if (member < 0) { result = -1; goto done; }
        if (member == 1 && archive_hash_seen(&archive->known_calendar_meetings, hash, 1) < 0) {
            result = -1; goto done;
        }
        keys[0] = "invitation"; keys[1] = "resultsProtocol";
        keys[2] = "verbatimProtocol"; keys[3] = "auxiliaryFile";
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/AgendaItem") &&
               !archive_private_item(&json, 0)) {
        keys[0] = "resolutionFile"; keys[1] = "auxiliaryFile";
    }
    for (i = 0; !result && i < 4 && keys[i]; i++)
        result = remember_links(archive, &archive->known_files, &json, tool_json_get(&json, 0, keys[i]), TYPE_FILE);
done:
    tool_json_free(&json); rt_free(bytes);
    return result;
}

static int snapshot_document(const char *path, const PlatformDirEntry *info, int depth,
                              ToolWalkControl *control, void *user) {
    Archive *archive = (Archive *)user;
    const char *name = path;
    char hash[65];
    size_t i, length;
    if (info->is_dir) { if (depth) control->prune = 1; return 0; }
    for (i = 0; path[i]; i++) if (path[i] == '/') name = path + i + 1;
    length = rt_strlen(name);
    if (length < 66 || name[64] != '-' ||
        (length >= 5 && !rt_strcmp(name + length - 5, ".part"))) return 0;
    for (i = 0; i < 64; i++) if (tool_hex_value(name[i]) < 0) return 0;
    memcpy(hash, name, 64); hash[64] = 0;
    return archive_hash_seen(&archive->known_files, hash, 1) < 0 ? -1 : 0;
}

int archive_prepare_sync(Archive *archive) {
    char path[PATH_CAPACITY];
    PlatformDirEntry info;
    if (archive->full || archive->refresh || (archive->sync_metadata_only && !archive->metadata_only))
        return 0;
    if (archive->sync_checkpoint > archive->started)
        return ris_error("sync checkpoint is in the future; check the clock or use --full", archive->output);
    if (rt_join_path(archive->output, "metadata", path, sizeof(path)) != 0) return -1;
    if (platform_get_path_info(path, &info) == 0 &&
        archive_walk(path, snapshot_metadata, archive, 0) != 0) return -1;
    if (!archive->sync_state_present && archive->oldest_metadata && archive->known_resources.count) {
        archive->sync_checkpoint = archive->oldest_metadata;
        rt_copy_string(archive->sync_source, sizeof(archive->sync_source), "legacy-local-mtime");
        ris_warning("bootstrapping delta checkpoint from oldest local metadata timestamp; existing gaps remain", archive->output);
    }
    if (!archive->sync_checkpoint) return 0;
    if (rt_join_path(archive->output, "documents", path, sizeof(path)) != 0) return -1;
    if (platform_get_path_info(path, &info) == 0 &&
        archive_walk(path, snapshot_document, archive, 0) != 0) return -1;
    archive->incremental = 1;
    archive->sync_since = archive->sync_checkpoint > SYNC_OVERLAP_SECONDS ?
                          archive->sync_checkpoint - SYNC_OVERLAP_SECONDS : 1;
    if (archive_timestamp(archive->sync_since, archive->modified_since, sizeof(archive->modified_since)) != 0 ||
        archive_timestamp(archive->started, archive->modified_until, sizeof(archive->modified_until)) != 0)
        return ris_error("cannot format delta window", archive->output);
    rt_write_cstr(2, "Incremental update: ");
    rt_write_cstr(2, archive->modified_since); rt_write_cstr(2, " through ");
    rt_write_line(2, archive->modified_until);
    return 0;
}

int archive_retain_selection(Archive *archive) {
    char path[PATH_CAPACITY];
    unsigned char *bytes = 0;
    PlatformDirEntry info;
    size_t length, start, end;
    int result = 0;
    if (!archive->incremental || archive->selection_fd < 0) return 0;
    if (rt_join_path(archive->output, "selection.jsonl", path, sizeof(path)) != 0) return -1;
    if (platform_get_path_info(path, &info) != 0) return 0;
    if (info.size > META_LIMIT || tool_read_all_input(path, &bytes, &length) != 0)
        return ris_error("cannot retain existing Paper selection", path);
    for (start = 0; start < length; start = end + 1) {
        char id[RIS_URL_CAPACITY], hash[65];
        ToolJson json;
        for (end = start; end < length && bytes[end] != '\n'; end++) {}
        if (end == start) continue;
        if (tool_json_parse(&json, (const char *)bytes + start, end - start) != 0) { result = -1; break; }
        result = tool_json_string(&json, tool_json_get(&json, 0, "id"), id, sizeof(id));
        tool_json_free(&json);
        if (result != 0) break;
        archive_hash_text(id, hash);
        if (archive_hash_seen(&archive->selected_roots, hash, 1) < 0) { result = -1; break; }
    }
    if (!result && rt_write_all(archive->selection_fd, bytes, length) != 0) result = -1;
    if (!result && length && bytes[length - 1] != '\n' &&
        rt_write_char(archive->selection_fd, '\n') != 0) result = -1;
    rt_free(bytes);
    return result == 0 ? 0 : ris_error("invalid or unwritable existing Paper selection", path);
}

int archive_open_manifest(const Archive *archive, const char *name, int *fd) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY];
    if (rt_join_path(archive->output, name, path, sizeof(path)) != 0 ||
        archive_temporary_path(path, part) != 0)
        return ris_error("manifest path is too long", name);
    *fd = platform_open_write(part, 0644);
    return *fd < 0 ? ris_error("cannot open archive manifest", part) : 0;
}

int archive_finish_manifest(const Archive *archive, const char *name, int *fd) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY];
    int result;
    if (*fd < 0) return 0;
    result = platform_close(*fd);
    *fd = -1;
    if (rt_join_path(archive->output, name, path, sizeof(path)) != 0 ||
        archive_temporary_path(path, part) != 0 || result != 0 ||
        platform_rename_path(part, path) != 0)
        return ris_error("cannot publish archive manifest", name);
    return 0;
}

int archive_write_summary(Archive *archive, int success, int force) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY], import_name[128], import_path[PATH_CAPACITY], import_part[PATH_CAPACITY];
    unsigned long long now = platform_get_monotonic_time_ns();
    unsigned char *bytes = 0;
    size_t i, length;
    int fd, result, metadata_complete = success && !archive->limited &&
        !archive->missing_resources && !archive->unknown_meetings;
    int update_complete = metadata_complete && !archive->unavailable_files;
    int baseline_complete = archive->incremental ? archive->baseline_complete : metadata_complete;
    int complete = update_complete && baseline_complete;
    if (!force && archive->summary_written_ns != 0 && now >= archive->summary_written_ns &&
        now - archive->summary_written_ns < SUMMARY_INTERVAL_NS) return 0;
    if (rt_join_path(archive->output, "archive.json", path, sizeof(path)) != 0 ||
        archive_temporary_path(path, part) != 0) return ris_error("summary path is too long", 0);
    fd = platform_open_write(part, 0644);
    if (fd < 0) return ris_error("cannot write archive summary", path);
    result = rt_write_cstr(fd, "{\"system\":") ||
             tool_json_write_string(fd, archive->system) ||
             tool_json_field_bool(fd, "complete", complete) ||
             tool_json_field_uint(fd, "archiveFormat", 2) ||
             tool_json_field_string(fd, "runId", archive->run_id) ||
             tool_json_field_string(fd, "state", !archive->finalizing ? "running" :
                                    !success ? "failed" : complete ? "complete" : "incomplete") ||
             tool_json_field_uint(fd, "runStartedAtEpochSeconds", archive->started) ||
             tool_json_field_uint(fd, "runFinishedAtEpochSeconds", archive->finished) ||
             tool_json_field_uint(fd, "lastSuccessfulRunEndEpochSeconds",
                                  complete ? archive->finished : archive->last_success) ||
             tool_json_field_string(fd, "syncMode", archive->incremental ? "incremental" : "full") ||
             tool_json_field_uint(fd, "syncSinceEpochSeconds", archive->sync_since) ||
             tool_json_field_uint(fd, "syncUntilEpochSeconds", archive->started) ||
             tool_json_field_uint(fd, "syncCheckpointEpochSeconds",
                                  metadata_complete ? archive->started : archive->sync_checkpoint) ||
             tool_json_field_string(fd, "syncCheckpointSource",
                                    metadata_complete ? "successful-run-start" : archive->sync_source) ||
             tool_json_field_bool(fd, "syncBaselineComplete", baseline_complete) ||
             tool_json_field_bool(fd, "syncMetadataOnly",
                                  metadata_complete ? archive->metadata_only : archive->sync_metadata_only) ||
             tool_json_field_bool(fd, "calendarOnly", archive->calendar_only) ||
             tool_json_field_string(fd, "calendarFrom", archive->calendar_from) ||
             tool_json_field_string(fd, "calendarThrough", archive->calendar_through) ||
             tool_json_field_string(fd, "calendarDateRole", "Meeting.start local calendar date") ||
             tool_json_field_uint(fd, "selectedMeetings", archive->selected_meetings) ||
             tool_json_field_uint(fd, "unknownMeetingDates", archive->unknown_meetings) ||
             tool_json_field_bool(fd, "metadataOnly", archive->metadata_only) ||
             tool_json_field_bool(fd, "skipUnavailableFiles", archive->skip_unavailable_files) ||
             tool_json_field_bool(fd, "acceptSizeMismatch", archive->accept_size_mismatch) ||
             tool_json_field_bool(fd, "limited", archive->limited) ||
             tool_json_field_uint(fd, "referenceYear", archive->calendar_only ? 0 : archive->year) ||
             tool_json_field_uint(fd, "scannedPapers", archive->scanned_papers) ||
             tool_json_field_uint(fd, "selectedPapers", archive->selected_papers) ||
             tool_json_field_uint(fd, "processedResources", archive->processed_resources) ||
             tool_json_field_uint(fd, "newMetadataPages", archive->downloaded_pages) ||
             tool_json_field_uint(fd, "cachedMetadataPages", archive->cached_pages) ||
             tool_json_field_uint(fd, "newDocuments", archive->downloaded_files) ||
             tool_json_field_uint(fd, "cachedDocuments", archive->cached_files) ||
             tool_json_field_uint(fd, "unavailableDocuments", archive->unavailable_files) ||
             tool_json_field_uint(fd, "missingResources", archive->missing_resources) ||
             tool_json_field_uint(fd, "sizeMismatchedDocuments", archive->mismatched_files) ||
             rt_write_cstr(fd, ",\"types\":{");
    for (i = 0; i < TYPE_COUNT && !result; i++) {
        const ImportType *type = &archive->types[i];
        int type_complete = type->scheduled && type->scheduled == type->finished &&
            !type->errors && !type->incomplete_collections &&
            !(i == TYPE_MEETING && archive->unknown_meetings);
        result = (i && rt_write_cstr(fd, ",")) || tool_json_write_string(fd, type_names[i]) ||
            rt_write_cstr(fd, ":{\"scheduledWorkItems\":") || rt_write_uint(fd, type->scheduled) ||
            tool_json_field_uint(fd, "finishedWorkItems", type->finished) ||
            tool_json_field_uint(fd, "fetchedResponses", type->fetched) ||
            tool_json_field_uint(fd, "revalidatedResponses", type->revalidated) ||
            tool_json_field_uint(fd, "listedObjects", type->listed) ||
            tool_json_field_uint(fd, "errors", type->errors) ||
            tool_json_field_uint(fd, "missingResources", type->missing) ||
            tool_json_field_uint(fd, "collections", type->collections) ||
            tool_json_field_uint(fd, "completeCollections", type->complete_collections) ||
            tool_json_field_uint(fd, "incompleteCollections", type->collections - type->complete_collections) ||
            tool_json_field_bool(fd, "complete", archive->finalizing && type_complete) ||
            tool_json_field_uint(fd, "lastSuccessfulRunEndEpochSeconds",
                                 archive->finalizing && type_complete ? archive->finished : type->last_success) ||
            rt_write_cstr(fd, "}");
    }
    if (rt_write_cstr(fd, "}}\n") != 0) result = -1;
    if (platform_close(fd) != 0) result = -1;
    rt_copy_string(import_name, sizeof(import_name), archive->run_id);
    length = rt_strlen(import_name); memcpy(import_name + length, ".json", 6);
    if (result != 0 || archive_path(archive, "imports", import_name, import_path) != 0 ||
        archive_temporary_path(import_path, import_part) != 0 || tool_read_all_input(part, &bytes, &length) != 0)
        return ris_error("cannot prepare import record", path);
    result = tool_write_file_all(import_part, bytes, length);
    rt_free(bytes);
    if (result != 0 || platform_rename_path(import_part, import_path) != 0 ||
        platform_rename_path(part, path) != 0)
        return ris_error("cannot publish archive summary", path);
    archive->summary_written_ns = now;
    return 0;
}

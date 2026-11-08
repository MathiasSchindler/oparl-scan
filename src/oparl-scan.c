#include "http.h"
#include "tool_json_parser.h"
#include "runtime.h"
#include "tool_util.h"
#include "crypto/sha256.h"
#include "hash_util.h"
#include "archive-version.h"
#include "archive-walk.h"

#define META_LIMIT (32ULL * 1024ULL * 1024ULL)
#define FILE_LIMIT (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define SUMMARY_INTERVAL_NS 500000000ULL
#define PATH_CAPACITY 1024U
#define DEFAULT_SYSTEM "https://www.potsdam.sitzung-online.de/oparl/system"
#define PENDING_DOCUMENTS 32U
#define SYNC_OVERLAP_SECONDS 300ULL

typedef enum {
    JOB_COLLECTION, JOB_BODIES, JOB_PAPERS, JOB_RESOURCE, JOB_CALENDAR, JOB_CHANGES
} JobKind;
enum { TYPE_SYSTEM, TYPE_BODY, TYPE_PAPER, TYPE_MEETING, TYPE_AGENDA, TYPE_CONSULTATION,
       TYPE_FILE, TYPE_ORGANIZATION, TYPE_PERSON, TYPE_MEMBERSHIP, TYPE_LOCATION,
       TYPE_TERM, TYPE_RESOURCE, TYPE_COUNT };
static const char *const type_names[TYPE_COUNT] = {"System","Body","Paper","Meeting","AgendaItem",
    "Consultation","File","Organization","Person","Membership","Location","LegislativeTerm","Resource"};
typedef struct {
    unsigned long long scheduled, finished, fetched, revalidated, listed, errors, missing;
    unsigned long long collections, complete_collections, incomplete_collections, last_success;
} ImportType;

typedef enum {
    META_LIST, META_SYSTEM, META_OBJECT, META_INDEX
} MetaKind;

typedef struct {
    char url[RIS_URL_CAPACITY];
    JobKind kind;
    unsigned int calendar, type;
} Job;

typedef struct {
    char (*entries)[65];
    size_t capacity;
    size_t count;
} SeenSet;

typedef struct {
    HttpValidators validators;
    char digest[65];
    unsigned long long size;
    int framed;
    unsigned long long observed, fetched;
    unsigned long long local_modified;
    char current[256];
} CacheEntry;

typedef struct {
    HttpTransfer transfer;
    char id[RIS_URL_CAPACITY];
    char url[RIS_URL_CAPACITY];
    char filename[512];
    char path[PATH_CAPACITY];
    char part[PATH_CAPACITY];
    CacheEntry cache;
} DocumentDownload;

typedef struct {
    HttpClient http;
    const char *output;
    const char *system;
    unsigned long long page_size;
    unsigned long long max_pages;
    unsigned long long max_files;
    unsigned long long downloaded_files;
    unsigned long long downloaded_pages;
    unsigned long long cached_pages;
    unsigned long long cached_files;
    unsigned long long unavailable_files;
    unsigned long long missing_resources;
    unsigned long long mismatched_files;
    unsigned long long summary_written_ns;
    unsigned long long scanned_papers;
    unsigned long long selected_papers;
    unsigned long long processed_resources;
    unsigned int year;
    int selection_fd;
    int unavailable_fd;
    int missing_fd;
    int mismatch_fd;
    int metadata_only;
    int skip_unavailable_files;
    int accept_size_mismatch;
    int refresh;
    int full, incremental, sync_state_present, baseline_complete, sync_metadata_only;
    unsigned long long sync_checkpoint, sync_since, oldest_metadata, previous_unavailable;
    char sync_source[32], modified_since[32], modified_until[32];
    int limited;
    ToolArray jobs;
    SeenSet seen_files;
    SeenSet seen_jobs;
    SeenSet seen_missing;
    SeenSet known_resources, known_files, known_calendar_meetings, paper_context, selected_roots;
    DocumentDownload *pending[PENDING_DOCUMENTS];
    size_t pending_count;
    char calendar_from[11], calendar_through[11], run_id[96];
    int calendar_only, observations_fd, finalizing, run_success;
    unsigned int active_type;
    unsigned long long started, finished, last_success, selected_meetings, unknown_meetings;
    ImportType types[TYPE_COUNT];
} Archive;

static int write_summary(Archive *archive, int success, int force);
static int download_metadata(Archive *archive, HttpTransfer *metadata);
static int flush_documents(Archive *archive);

static void usage(void) {
    rt_write_cstr(1,
        "Usage: oparl-scan [OPTIONS]\n"
        "  --output DIR       Archive directory (default: downloads)\n"
        "  --metadata-only    Do not download document binaries\n"
        "  --skip-unavailable-files  Report access-denied documents and continue (default)\n"
        "  --no-skip-unavailable-files  Abort on document access denials\n"
        "  --accept-size-mismatch   Warn on complete transfers with wrong OParl sizes (default)\n"
        "  --no-accept-size-mismatch  Require matching OParl document sizes\n"
        "  --year YYYY        Select reference prefix YY/, including later follow-ups\n"
        "  --calendar-from DATE     Independent calendar, inclusive YYYY-MM-DD\n"
        "  --calendar-through DATE  End date; default with --year: Jan 1 through Dec 31\n"
        "  --calendar-only          No Paper discovery; requires --year or date range\n"
        "  --refresh          Replace cached metadata and documents\n"
        "  --full             Reconcile all metadata instead of the default delta update\n"
        "  --page-size N      Objects per page, 1..100 (default: 100)\n"
        "  --max-pages N      Limit pages per collection (default: unlimited)\n"
        "  --max-files N      Limit new document downloads (default: unlimited)\n"
        "  --delay-ms N       Pause per destination between requests (default: 500)\n"
        "  --workers N        Concurrent requests, 1..2 (default: 2; one per host)\n"
        "  --timeout-ms N     TLS and read timeout (default: 120000)\n"
        "  --ca-file FILE     PEM CA bundle (or SSL_CERT_FILE/system bundle)\n"
        "  --system URL       System URL; HTTP is allowed only on 127.0.0.1\n"
        "  --color MODE       Diagnostic colors: auto, always, never (default: auto)\n"
        "  --help             Show this help\n"
        "Existing archives use modified_since/until; --full performs a complete reconciliation.\n"
        "HTTP 404: report missing URLs, continue, and mark the archive incomplete.\n"
        "Exit status: 0 complete, 1 error, 2 limits, 3 missing resources.\n");
}

static int options(Archive *archive, int argc, char **argv) {
    int i;
    rt_memset(archive, 0, sizeof(*archive));
    archive->output = "downloads";
    archive->system = DEFAULT_SYSTEM;
    archive->page_size = 100;
    archive->skip_unavailable_files = 1;
    archive->accept_size_mismatch = 1;
    archive->selection_fd = -1;
    archive->unavailable_fd = -1;
    archive->missing_fd = -1;
    archive->mismatch_fd = -1;
    archive->observations_fd = -1;
    archive->http.timeout_ms = 120000;
    archive->http.delay_ms = 500;
    archive->http.workers = 2;
    tool_array_init(&archive->jobs, sizeof(Job));
    for (i = 1; i < argc; i++) {
        const char *flag = argv[i], *value;
        unsigned long long number;
        if (rt_strcmp(flag, "--help") == 0) {
            usage();
            return 1;
        }
        if (rt_strcmp(flag, "--metadata-only") == 0) {
            archive->metadata_only = 1;
            continue;
        }
        if (rt_strcmp(flag, "--skip-unavailable-files") == 0 ||
            rt_strcmp(flag, "--no-skip-unavailable-files") == 0) {
            archive->skip_unavailable_files = rt_strcmp(flag, "--skip-unavailable-files") == 0;
            continue;
        }
        if (rt_strcmp(flag, "--accept-size-mismatch") == 0 ||
            rt_strcmp(flag, "--no-accept-size-mismatch") == 0) {
            archive->accept_size_mismatch = rt_strcmp(flag, "--accept-size-mismatch") == 0;
            continue;
        }
        if (rt_strcmp(flag, "--refresh") == 0) {
            archive->refresh = 1;
            continue;
        }
        if (rt_strcmp(flag, "--full") == 0) { archive->full = 1; continue; }
        if (rt_strcmp(flag, "--calendar-only") == 0) { archive->calendar_only = 1; continue; }
        if (i + 1 == argc) return ris_error("missing option value", flag);
        value = argv[++i];
        if (rt_strcmp(flag, "--output") == 0) archive->output = value;
        else if (rt_strcmp(flag, "--calendar-from") == 0 || rt_strcmp(flag, "--calendar-through") == 0) {
            if (!archive_valid_date(value)) return ris_error("invalid calendar date; expected YYYY-MM-DD", value);
            rt_copy_string(!rt_strcmp(flag, "--calendar-from") ? archive->calendar_from :
                           archive->calendar_through, 11, value);
        }
        else if (rt_strcmp(flag, "--system") == 0) archive->system = value;
        else if (rt_strcmp(flag, "--ca-file") == 0) archive->http.ca_file = value;
        else if (rt_strcmp(flag, "--color") == 0) {
            int mode;
            if (tool_parse_color_mode(value, &mode) != 0)
                return ris_error("invalid color mode; use auto, always or never", value);
            tool_set_global_color_mode(mode);
        }
        else {
            if (rt_parse_uint(value, &number) != 0)
                return ris_error("invalid numeric option", flag);
            if (rt_strcmp(flag, "--page-size") == 0 && number >= 1 && number <= 100)
                archive->page_size = number;
            else if (rt_strcmp(flag, "--year") == 0 && number >= 2000 && number <= 2099)
                archive->year = (unsigned int)number;
            else if (rt_strcmp(flag, "--max-pages") == 0 && number >= 1)
                archive->max_pages = number;
            else if (rt_strcmp(flag, "--max-files") == 0 && number >= 1)
                archive->max_files = number;
            else if (rt_strcmp(flag, "--delay-ms") == 0 && number <= 3600000)
                archive->http.delay_ms = (unsigned int)number;
            else if (rt_strcmp(flag, "--workers") == 0 && number >= 1 && number <= 2)
                archive->http.workers = (unsigned int)number;
            else if (rt_strcmp(flag, "--timeout-ms") == 0 && number >= 1 && number <= 3600000)
                archive->http.timeout_ms = (unsigned int)number;
            else return ris_error("unknown option or value out of range", flag);
        }
    }
    if (archive->output[0] == '\0') return ris_error("output directory must not be empty", 0);
    if (!archive->calendar_from[0] && !archive->calendar_through[0] && archive->year) {
        char year[8];
        rt_unsigned_to_string(archive->year, year, sizeof(year));
        memcpy(archive->calendar_from, year, 4); memcpy(archive->calendar_from + 4, "-01-01", 7);
        memcpy(archive->calendar_through, year, 4); memcpy(archive->calendar_through + 4, "-12-31", 7);
    }
    if (!!archive->calendar_from[0] != !!archive->calendar_through[0] ||
        (archive->calendar_from[0] && rt_strcmp(archive->calendar_from, archive->calendar_through) > 0) ||
        (archive->calendar_only && !archive->calendar_from[0]))
        return ris_error("calendar requires an ordered from/through pair, or --year", 0);
    archive->http.allow_loopback = rt_strncmp(archive->system, "http://127.0.0.1:", 17) == 0 ||
                                   rt_strncmp(archive->system, "http://127.0.0.1/", 17) == 0;
    if (!http_valid_url(&archive->http, archive->system))
        return ris_error("system URL is not allowed", archive->system);
    return 0;
}

static void hash_text(const char *text, char out[65]) {
    unsigned char digest[32];
    crypto_sha256_hash((const unsigned char *)text, rt_strlen(text), digest);
    hash_to_hex(digest, sizeof(digest), out);
}

static size_t hash_slot(const char *hash, size_t capacity) {
    size_t value = 0, i;
    for (i = 0; i < 16; i++) value = value * 33U + (unsigned char)hash[i];
    return value & (capacity - 1);
}

static int hash_seen(SeenSet *set, const char *hash, int insert) {
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

static int known_reference(SeenSet *set, const ToolJson *json, size_t index) {
    const ToolJsonToken *token = tool_json_token(json, index);
    char id[RIS_URL_CAPACITY], hash[65];
    size_t child;
    if (!token || token->kind == TOOL_JSON_NULL) return 0;
    if (token->kind == TOOL_JSON_ARRAY) {
        for (child = index + 1; child < token->next; child = tool_json_token(json, child)->next)
            if (known_reference(set, json, child)) return 1;
        return 0;
    }
    if (token->kind == TOOL_JSON_OBJECT) index = tool_json_get(json, index, "id");
    if (tool_json_string(json, index, id, sizeof(id)) != 0) return 0;
    hash_text(id, hash);
    return hash_seen(set, hash, 0);
}

static int archive_path(const Archive *archive, const char *directory,
                        const char *name, char *out) {
    char parent[PATH_CAPACITY];
    if (rt_join_path(archive->output, directory, parent, sizeof(parent)) != 0 ||
        rt_join_path(parent, name, out, PATH_CAPACITY) != 0)
        return ris_error("archive path is too long", archive->output);
    return 0;
}

static int prepare_directory(const Archive *archive, const char *name) {
    char path[PATH_CAPACITY], scratch[PATH_CAPACITY];
    if (archive_path(archive, name, "_", path) != 0) return -1;
    if (tool_ensure_parent_dirs(path, scratch, sizeof(scratch)) != 0)
        return ris_error("cannot create archive directory", path);
    return 0;
}

static int temporary_path(const char *path, char out[PATH_CAPACITY]) {
    size_t length = rt_strlen(path);
    if (length + 5 >= PATH_CAPACITY) return ris_error("temporary path is too long", path);
    memcpy(out, path, length);
    memcpy(out + length, ".part", 6);
    return 0;
}

static int content_hash(const char *path, char out[65]) {
    return archive_content_hash(path, out) == 0 ? 0 : ris_error("cannot hash archived content", path);
}

static int preserve(Archive *archive, const char *kind, const char *key,
                     const char *path, const CacheEntry *cache) {
    char relative[256];
    return archive_preserve(archive->output, kind, key, path, cache->digest,
                            relative, sizeof(relative)) == 0 ? 0 :
        ris_error("cannot preserve or verify immutable revision", path);
}

static int observation(Archive *archive, const char *kind, const char *key,
                        const char *url, const CacheEntry *cache, int reused) {
    int fd = archive->observations_fd;
    char hash[65], directory[128], identity[256], relative[256];
    const char *history_kind = !rt_strcmp(kind, "document") ? "documents" : "metadata";
    hash_text(key, hash);
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
    hash_text(key, hash);
    memcpy(name, kind, length);
    name[length++] = '-';
    memcpy(name + length, hash, 64);
    memcpy(name + length + 64, ".json", 6);
    return archive_path(archive, "cache", name, path);
}

static int load_cache(const Archive *archive, const char *kind, const char *key,
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
        hash_text(key, hash);
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
    if (content_hash(content, digest) != 0) return -1;
    if (rt_strcmp(digest, entry->digest) != 0) {
        ris_warning("cache content changed; fetching resource again", content);
        return 0;
    }
    return 1;
}

static int load_legacy_document(const Archive *archive, const char *id,
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
    if (content_hash(content, entry->digest) != 0) return -1;
    return 1;
}

static int save_cache(const Archive *archive, const char *kind, const char *key,
                      const char *url, const CacheEntry *entry) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY];
    int fd, result;
    if (cache_path(archive, kind, key, path) != 0 || temporary_path(path, part) != 0) return -1;
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

static size_t resource_type(const ToolJson *json) {
    size_t type = tool_json_get(json, 0, "type");
    if (tool_json_equals(json, type, "at") || tool_json_equals(json, type, "gr")) {
        size_t legacy = tool_json_get(json, 0, "Type");
        if (tool_json_equals(json, legacy, "https://schema.oparl.org/1.1/Organization"))
            return legacy;
    }
    return type;
}

static int page_shape(const ToolJson *json, MetaKind kind) {
    const ToolJsonToken *root = tool_json_token(json, 0), *data;
    unsigned long long current, pages, total;
    size_t pagination;
    if (root == 0 || root->kind != TOOL_JSON_OBJECT) return -1;
    if (kind == META_SYSTEM) {
        char body[RIS_URL_CAPACITY];
        return tool_json_equals(json, tool_json_get(json, 0, "type"),
                           "https://schema.oparl.org/1.1/System") &&
               tool_json_string(json, tool_json_get(json, 0, "body"), body, sizeof(body)) == 0 ? 0 : -1;
    }
    if (kind == META_OBJECT) {
        char id[RIS_URL_CAPACITY], type[256];
        return tool_json_string(json, tool_json_get(json, 0, "id"), id, sizeof(id)) == 0 &&
               tool_json_string(json, resource_type(json), type, sizeof(type)) == 0 &&
               rt_strncmp(type, "https://schema.oparl.org/1.1/", 29) == 0 ? 0 : -1;
    }
    data = tool_json_token(json, tool_json_get(json, 0, "data"));
    pagination = tool_json_get(json, 0, "pagination");
    if (data == 0 || data->kind != TOOL_JSON_ARRAY ||
        tool_json_uint(json, tool_json_get(json, pagination, "currentPage"), &current) != 0 ||
        tool_json_uint(json, tool_json_get(json, pagination, "totalPages"), &pages) != 0 ||
        tool_json_uint(json, tool_json_get(json, pagination, "totalElements"), &total) != 0 ||
        current == 0 || (pages != 0 && current > pages) ||
        (pages == 0 && total != 0)) return -1;
    return 0;
}

static int record_missing(Archive *archive, const char *url, const char *kind) {
    char hash[65];
    int fd = archive->missing_fd;
    hash_text(url, hash);
    if (hash_seen(&archive->seen_missing, hash, 0)) return 0;
    if (rt_write_cstr(fd, "{\"url\":") != 0 ||
        tool_json_write_string(fd, url) != 0 ||
        tool_json_field_string(fd, "kind", kind) != 0 ||
        tool_json_field_uint(fd, "status", 404) != 0 ||
        rt_write_cstr(fd, "}\n") != 0)
        return ris_error("cannot write missing resource report", url);
    if (hash_seen(&archive->seen_missing, hash, 1) < 0) return -1;
    archive->missing_resources++;
    return write_summary(archive, 0, 0);
}

static int read_metadata_impl(Archive *archive, const char *url, MetaKind kind,
                         ToolJson *json, unsigned char **bytes) {
    char hash[65], name[80], path[PATH_CAPACITY], part[PATH_CAPACITY];
    CacheEntry cache;
    HttpTransfer transfer;
    size_t length;
    int cached, result;
    rt_memset(&transfer, 0, sizeof(transfer));
    transfer.url = url;
    transfer.path = part;
    transfer.limit = META_LIMIT;
    transfer.expected_size = RIS_UNKNOWN_SIZE;
    transfer.result = -1;
    hash_text(url, hash);
    rt_copy_string(name, sizeof(name), hash);
    memcpy(name + 64, ".json", 6);
    if (archive_path(archive, kind == META_INDEX ? "index" : "metadata", name, path) != 0 ||
        temporary_path(path, part) != 0) return -1;
    cached = load_cache(archive, "metadata", url, url, path, META_LIMIT, &cache);
    if (cached < 0) return -1;
    if (cached && preserve(archive, "metadata", url, path, &cache) != 0) return -1;
    if (cached && !archive->refresh) {
        transfer.condition = &cache.validators;
        transfer.cached_size = cache.size;
    }
    result = download_metadata(archive, &transfer);
    if (result != 0 && result != RIS_HTTP_NOT_MODIFIED) return result;
    cached = result == RIS_HTTP_NOT_MODIFIED;
    if (tool_read_all_input(cached ? path : part, bytes, &length) != 0)
        return ris_error("cannot read metadata", cached ? path : part);
    if (tool_json_parse(json, (const char *)*bytes, length) != 0) {
        rt_free(*bytes);
        *bytes = 0;
        return ris_error("invalid JSON", url);
    }
    if (page_shape(json, kind) != 0 ||
        (kind == META_OBJECT &&
         !tool_json_equals(json, tool_json_get(json, 0, "id"), url))) {
        tool_json_free(json);
        rt_free(*bytes);
        *bytes = 0;
        return ris_error("unexpected OParl response shape", url);
    }
    cache.validators = transfer.validators;
    cache.size = length;
    if (!cached) cache.framed = transfer.framed;
    {
        unsigned char digest[32];
        crypto_sha256_hash(*bytes, length, digest);
        hash_to_hex(digest, sizeof(digest), cache.digest);
    }
    cache.observed = (unsigned long long)platform_get_epoch_time();
    if (!cached) cache.fetched = cache.observed;
    if (preserve(archive, "metadata", url, cached ? path : part, &cache) != 0) {
        tool_json_free(json); rt_free(*bytes); *bytes = 0; return -1;
    }
    if (!cached) {
        if (platform_rename_path(part, path) != 0) {
            tool_json_free(json);
            rt_free(*bytes);
            *bytes = 0;
            return ris_error("cannot publish metadata download", path);
        }
        archive->downloaded_pages++;
        rt_write_cstr(2, "metadata: ");
        rt_write_line(2, url);
    } else archive->cached_pages++;
    if (save_cache(archive, "metadata", url, url, &cache) != 0 ||
        observation(archive, "metadata", url, url, &cache, cached) != 0) {
        tool_json_free(json);
        rt_free(*bytes);
        *bytes = 0;
        return -1;
    }
    return 0;
}

static int read_metadata(Archive *archive, const char *url, MetaKind kind, ToolJson *json, unsigned char **bytes) {
    unsigned long long previous = archive->cached_pages;
    ImportType *type = &archive->types[archive->active_type];
    int result = read_metadata_impl(archive, url, kind, json, bytes);
    if (result == 0) {
        if (archive->cached_pages != previous) type->revalidated++;
        else type->fetched++;
    } else {
        type->errors++;
        if (result == RIS_HTTP_NOT_FOUND) type->missing++;
    }
    return result;
}

static unsigned int url_type(const char *url) {
    static const char *const paths[] = {"/system","/bodies/","/papers/","/meetings/","/agendaItems/",
        "/consultations/","/files/","/organizations/","/persons/","/memberships/","/locations/","/legislativeTerms/"};
    size_t i, j;
    for (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++)
        for (j = 0; url[j]; j++) if (!rt_strncmp(url + j, paths[i], rt_strlen(paths[i]))) return (unsigned int)i;
    return TYPE_RESOURCE;
}

static int add_scoped_job(Archive *archive, const char *url, JobKind kind, unsigned int calendar, unsigned int type) {
    char hash[65];
    Job *job;
    if (!http_valid_url(&archive->http, url)) return ris_error("collection URL is not allowed", url);
    if (calendar) {
        CryptoSha256Context sha;
        unsigned char bytes[32];
        crypto_sha256_init(&sha); crypto_sha256_update(&sha, (const unsigned char *)"calendar:", 9);
        crypto_sha256_update(&sha, (const unsigned char *)url, rt_strlen(url));
        crypto_sha256_final(&sha, bytes); hash_to_hex(bytes, 32, hash);
    } else hash_text(url, hash);
    if (hash_seen(&archive->seen_jobs, hash, 0)) return 0;
    job = (Job *)tool_array_append(&archive->jobs);
    if (job == 0) return ris_error("cannot allocate collection queue", 0);
    rt_copy_string(job->url, sizeof(job->url), url);
    job->kind = kind;
    job->calendar = calendar; job->type = type;
    archive->types[type].scheduled++;
    if (kind != JOB_RESOURCE) archive->types[type].collections++;
    return hash_seen(&archive->seen_jobs, hash, 1) < 0 ? -1 : 0;
}
static int add_job(Archive *archive, const char *url, JobKind kind) {
    return add_scoped_job(archive, url, kind, 0, kind == JOB_BODIES ? TYPE_BODY :
                          kind == JOB_PAPERS ? TYPE_PAPER : url_type(url));
}

static int discover_collections(Archive *archive, const ToolJson *json, size_t body) {
    static const char *keys[] = {
        "organization", "person", "meeting", "membership", "locationList",
        "legislativeTermList", "agendaItem", "paper", "consultation", "file"
    };
    size_t i;
    char url[RIS_URL_CAPACITY];
    if (tool_json_true(json, tool_json_get(json, body, "deleted"))) return 0;
    if (archive->year != 0 && !archive->calendar_only) {
        if (tool_json_string(json, tool_json_get(json, body, "paper"), url, sizeof(url)) != 0)
            return ris_error("Body has no paper collection for year selection", 0);
        if (!archive->calendar_only && add_job(archive, url, JOB_PAPERS) != 0) return -1;
    }
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        size_t member = tool_json_get(json, body, keys[i]);
        if (!archive->incremental && (archive->year || archive->calendar_only) && rt_strcmp(keys[i], "meeting")) continue;
        if (archive->year && !archive->calendar_only && !rt_strcmp(keys[i], "paper")) continue;
        if (member == TOOL_JSON_NONE) continue;
        if (tool_json_string(json, member, url, sizeof(url)) != 0)
            return ris_error("invalid collection link in Body", keys[i]);
        if (!rt_strcmp(keys[i], "meeting")) {
            if (add_scoped_job(archive, url, JOB_CALENDAR, 1, TYPE_MEETING) != 0) return -1;
        } else if (add_scoped_job(archive, url, archive->incremental ? JOB_CHANGES : JOB_COLLECTION, 0,
                   i == 0 ? TYPE_ORGANIZATION : i == 1 ? TYPE_PERSON : i == 3 ? TYPE_MEMBERSHIP :
                   i == 4 ? TYPE_LOCATION : i == 5 ? TYPE_TERM : i == 6 ? TYPE_AGENDA :
                   i == 7 ? TYPE_PAPER : i == 8 ? TYPE_CONSULTATION : TYPE_FILE) != 0) return -1;
    }
    return 0;
}

static int complete_document_impl(Archive *archive, DocumentDownload *download) {
    const char *id = download->id, *url = download->url, *filename = download->filename;
    unsigned long long expected = download->transfer.expected_size;
    unsigned long long received = download->transfer.received;
    int result = download->transfer.result;
    if (result == RIS_HTTP_NOT_FOUND) {
        if (record_missing(archive, url, "document") != 0) return -1;
        return 0;
    }
    if (result == RIS_HTTP_ACCESS_DENIED && archive->skip_unavailable_files) {
        int fd = archive->unavailable_fd;
        if (rt_write_cstr(fd, "{\"id\":") != 0 ||
            tool_json_write_string(fd, id) != 0 ||
            tool_json_field_string(fd, "url", url) != 0 ||
            tool_json_field_string(fd, "fileName", filename) != 0 ||
            tool_json_field_string(fd, "reason", "access-denied") != 0 ||
            rt_write_cstr(fd, "}\n") != 0)
            return ris_error("cannot write unavailable file report", id);
        archive->unavailable_files++;
        ris_warning("unavailable document recorded", id);
        return 0;
    }
    if (result != 0 && result != RIS_HTTP_NOT_MODIFIED) return -1;
    if (expected != RIS_UNKNOWN_SIZE && received != expected) {
        int fd = archive->mismatch_fd;
        if (rt_write_cstr(fd, "{\"id\":") != 0 ||
            tool_json_write_string(fd, id) != 0 ||
            tool_json_field_string(fd, "url", url) != 0 ||
            tool_json_field_string(fd, "fileName", filename) != 0 ||
            tool_json_field_uint(fd, "expectedSize", expected) != 0 ||
            tool_json_field_uint(fd, "actualSize", received) != 0 ||
            rt_write_cstr(fd, "}\n") != 0)
            return ris_error("cannot write size mismatch report", id);
    }
    if (result == 0) {
        if (content_hash(download->part, download->cache.digest) != 0 ||
            preserve(archive, "documents", id, download->part, &download->cache) != 0) return -1;
        if (platform_rename_path(download->part, download->path) != 0)
            return ris_error("cannot publish document", download->path);
        download->cache.framed = download->transfer.framed;
        download->cache.local_modified = 0;
    }
    download->cache.size = received;
    download->cache.validators = download->transfer.validators;
    download->cache.observed = (unsigned long long)platform_get_epoch_time();
    if (result == 0) download->cache.fetched = download->cache.observed;
    if (save_cache(archive, "document", id, url, &download->cache) != 0 ||
        observation(archive, "document", id, url, &download->cache, result == RIS_HTTP_NOT_MODIFIED) != 0) return -1;
    if (expected != RIS_UNKNOWN_SIZE && received != expected) archive->mismatched_files++;
    if (result == RIS_HTTP_NOT_MODIFIED) {
        archive->cached_files++;
        if (download->transfer.local_modified != 0) {
            rt_write_cstr(2, "document cache adopted from local timestamp: ");
            rt_write_line(2, download->path);
        }
    }
    else {
        archive->downloaded_files++;
        rt_write_cstr(2, "document: ");
        rt_write_line(2, download->path);
    }
    return 0;
}

static int complete_document(Archive *archive, DocumentDownload *download) {
    ImportType *type = &archive->types[TYPE_FILE];
    int status = download->transfer.result;
    int result = complete_document_impl(archive, download);
    type->finished++;
    if (result != 0 || (status != 0 && status != RIS_HTTP_NOT_MODIFIED)) type->errors++;
    if (status == RIS_HTTP_NOT_FOUND) type->missing++;
    if (result == 0 && status == 0) type->fetched++;
    if (result == 0 && status == RIS_HTTP_NOT_MODIFIED) type->revalidated++;
    return result;
}

/* Network tasks own only their transfer slots; archive state is merged after the join. */
static int perform_downloads(Archive *archive, HttpTransfer *metadata,
                             DocumentDownload *first, DocumentDownload *second) {
    HttpTransfer transfers[2];
    DocumentDownload *documents[2] = {first, second};
    size_t count = 0, i, kept = 0;
    int result = 0;
    if (metadata != 0) transfers[count++] = *metadata;
    for (i = 0; i < 2; i++)
        if (documents[i] != 0) transfers[count++] = documents[i]->transfer;
    if (http_download_batch(&archive->http, transfers, count) != 0) return -1;
    count = 0;
    if (metadata != 0) *metadata = transfers[count++];
    for (i = 0; i < 2; i++) {
        if (documents[i] == 0) continue;
        documents[i]->transfer = transfers[count++];
        if (complete_document(archive, documents[i]) != 0) result = -1;
    }
    for (i = 0; i < archive->pending_count; i++) {
        DocumentDownload *download = archive->pending[i];
        if (download == first || download == second) rt_free(download);
        else archive->pending[kept++] = download;
    }
    archive->pending_count = kept;
    if (result == 0) result = write_summary(archive, 0, 0);
    return result;
}

static int download_metadata(Archive *archive, HttpTransfer *metadata) {
    DocumentDownload *document = 0;
    size_t i;
    int result;
    for (i = 0; i < archive->pending_count; i++) {
        if (!http_same_host(&archive->http, metadata->url, archive->pending[i]->url)) {
            document = archive->pending[i];
            break;
        }
    }
    result = perform_downloads(archive, metadata, document, 0);
    if (metadata->result == RIS_HTTP_NOT_FOUND &&
        record_missing(archive, metadata->url, "metadata") != 0) return -1;
    if (result != 0) return -1;
    return metadata->result;
}

static int flush_documents(Archive *archive) {
    while (archive->pending_count != 0) {
        DocumentDownload *first = archive->pending[0], *second = 0;
        size_t i;
        for (i = 1; i < archive->pending_count; i++) {
            if (!http_same_host(&archive->http, first->url, archive->pending[i]->url)) {
                second = archive->pending[i];
                break;
            }
        }
        if (perform_downloads(archive, 0, first, second) != 0) return -1;
    }
    return 0;
}

static int document(Archive *archive, const ToolJson *json, size_t object) {
    char id[RIS_URL_CAPACITY], url[RIS_URL_CAPACITY], filename[512];
    char hash[65], name[180], path[PATH_CAPACITY], part[PATH_CAPACITY];
    unsigned long long expected = RIS_UNKNOWN_SIZE;
    size_t token, i, used;
    DocumentDownload *download, *other = 0;
    CacheEntry cache;
    int cached, legacy = 0;
    if (archive->metadata_only || tool_json_true(json, tool_json_get(json, object, "deleted"))) return 0;
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0)
        return ris_error("File has no valid id", 0);
    token = tool_json_get(json, object, "downloadUrl");
    if (token == TOOL_JSON_NONE) token = tool_json_get(json, object, "accessUrl");
    if (tool_json_string(json, token, url, sizeof(url)) != 0 ||
        !http_valid_url(&archive->http, url)) return ris_error("File has no allowed download URL", id);
    token = tool_json_get(json, object, "size");
    if (token != TOOL_JSON_NONE && tool_json_uint(json, token, &expected) != 0)
        return ris_error("invalid File size", id);
    if (expected != RIS_UNKNOWN_SIZE && expected > FILE_LIMIT)
        return ris_error("document exceeds 4 GiB limit", id);
    token = tool_json_get(json, object, "fileName");
    if (token == TOOL_JSON_NONE) {
        rt_copy_string(filename, sizeof(filename), "document.bin");
    } else if (tool_json_string(json, token, filename, sizeof(filename)) != 0) {
        return ris_error("invalid or oversized fileName", id);
    }
    hash_text(id, hash);
    if (hash_seen(&archive->seen_files, hash, 0)) return 0;
    memcpy(name, hash, 64);
    name[64] = '-';
    used = 65;
    for (i = 0; filename[i] != '\0' && i < 100; i++) {
        char ch = filename[i];
        name[used++] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' ? ch : '_';
    }
    if (used == 65) name[used++] = '_';
    name[used] = '\0';
    if (archive_path(archive, "documents", name, path) != 0) return -1;
    cached = load_cache(archive, "document", id, url, path, FILE_LIMIT, &cache);
    if (cached < 0) return -1;
    if (!cached && !archive->refresh) {
        cached = load_legacy_document(archive, id, path, &cache);
        if (cached < 0) return -1;
        legacy = cached;
    }
    if (temporary_path(path, part) != 0) return -1;
    if (cached && preserve(archive, "documents", id, path, &cache) != 0) return -1;
    if (cached && expected != RIS_UNKNOWN_SIZE && expected != cache.size &&
        (!archive->accept_size_mismatch || !cache.framed)) cached = 0;
    if (archive->pending_count == PENDING_DOCUMENTS ||
        (archive->max_files != 0 &&
         archive->downloaded_files + archive->pending_count >= archive->max_files))
        if (flush_documents(archive) != 0) return -1;
    archive->types[TYPE_FILE].scheduled++;
    if (archive->max_files != 0 && archive->downloaded_files >= archive->max_files) {
        archive->limited = 1;
        return 0;
    }
    download = (DocumentDownload *)rt_malloc(sizeof(*download));
    if (download == 0) return ris_error("cannot allocate document queue entry", id);
    rt_copy_string(download->id, sizeof(download->id), id);
    rt_copy_string(download->url, sizeof(download->url), url);
    rt_copy_string(download->filename, sizeof(download->filename), filename);
    rt_copy_string(download->path, sizeof(download->path), path);
    rt_copy_string(download->part, sizeof(download->part), part);
    rt_memset(&download->transfer, 0, sizeof(download->transfer));
    download->transfer.url = download->url;
    download->transfer.path = download->part;
    download->transfer.limit = FILE_LIMIT;
    download->transfer.expected_size = expected;
    download->transfer.accept_size_mismatch = archive->accept_size_mismatch;
    download->transfer.result = -1;
    download->cache = cache;
    {
        const char *base = path;
        for (i = 0; path[i]; i++) if (path[i] == '/') base = path + i + 1;
        if (rt_join_path("documents", base, download->cache.current, sizeof(download->cache.current)) != 0) {
            rt_free(download); return ris_error("document state path is too long", id);
        }
    }
    if (cached && !archive->refresh) {
        if (legacy) {
            download->transfer.local_modified = cache.local_modified;
            download->transfer.cached_size = cache.size;
            download->transfer.probe = 1;
        } else if (cache.validators.etag[0] != '\0' || cache.validators.modified[0] != '\0') {
            download->transfer.condition = &download->cache.validators;
            download->transfer.cached_size = cache.size;
            download->transfer.probe = 1;
        }
    }
    if (hash_seen(&archive->seen_files, hash, 1) < 0) {
        rt_free(download);
        return -1;
    }
    for (i = 0; i < archive->pending_count; i++)
        if (!http_same_host(&archive->http, url, archive->pending[i]->url)) {
            other = archive->pending[i];
            break;
        }
    archive->pending[archive->pending_count++] = download;
    if (other != 0 || archive->http.workers == 1 ||
        http_same_host(&archive->http, url, archive->system))
        return perform_downloads(archive, 0, download, other);
    return 0;
}

static int select_paper(Archive *archive, const ToolJson *json, size_t object) {
    char reference[256], id[RIS_URL_CAPACITY], hash[65];
    int fd = archive->selection_fd;
    if (tool_json_true(json, tool_json_get(json, object, "deleted"))) return 0;
    archive->scanned_papers++;
    if (!tool_json_equals(json, tool_json_get(json, object, "type"), "https://schema.oparl.org/1.1/Paper") ||
        tool_json_string(json, tool_json_get(json, object, "reference"), reference, sizeof(reference)) != 0)
        return ris_error("Paper has no valid reference for year selection", 0);
    if (rt_strlen(reference) < 3 || reference[0] != (char)('0' + (archive->year % 100) / 10) ||
        reference[1] != (char)('0' + archive->year % 10) || reference[2] != '/') return 0;
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0)
        return ris_error("selected Paper has no id", reference);
    hash_text(id, hash);
    if (hash_seen(&archive->seen_jobs, hash, 0)) return 0;
    if (add_job(archive, id, JOB_RESOURCE) != 0) return -1;
    archive->selected_papers++;
    if (hash_seen(&archive->selected_roots, hash, 0)) return 0;
    if (rt_write_cstr(fd, "{\"id\":") != 0 || tool_json_write_string(fd, id) != 0 ||
        tool_json_field_string(fd, "reference", reference) != 0 ||
        rt_write_cstr(fd, "}\n") != 0)
        return ris_error("cannot write selected paper index", reference);
    return hash_seen(&archive->selected_roots, hash, 1) < 0 ? -1 : 0;
}

static int private_item(const ToolJson *json, size_t object) {
    size_t index = tool_json_get(json, object, "public");
    const ToolJsonToken *token = tool_json_token(json, index);
    return token && token->kind == TOOL_JSON_BOOL && !tool_json_true(json, index);
}

static int calendar_member(const Archive *archive, const ToolJson *json, size_t object) {
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

static int store_collection_object(Archive *archive, const ToolJson *json, size_t object, const char *source) {
    char id[RIS_URL_CAPACITY], hash[65], name[80], path[PATH_CAPACITY], part[PATH_CAPACITY];
    const ToolJsonToken *token = tool_json_token(json, object);
    CacheEntry cache;
    unsigned char digest[32];
    int cached, fd, result;
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0 ||
        !http_valid_url(&archive->http, id)) return ris_error("invalid collection object ID", source);
    hash_text(id, hash); memcpy(name, hash, 64); memcpy(name + 64, ".json", 6);
    if (archive_path(archive, "metadata", name, path) != 0 || temporary_path(path, part) != 0) return -1;
    cached = load_cache(archive, "metadata", id, id, path, META_LIMIT, &cache);
    if (cached < 0 || (cached && preserve(archive, "metadata", id, path, &cache) != 0)) return -1;
    rt_memset(&cache, 0, sizeof(cache));
    rt_copy_string(cache.validators.url, sizeof(cache.validators.url), id);
    cache.size = token->end - token->start; cache.observed = (unsigned long long)platform_get_epoch_time();
    crypto_sha256_hash((const unsigned char *)json->text + token->start, (size_t)cache.size, digest);
    hash_to_hex(digest, 32, cache.digest); cache.framed = 1;
    fd = platform_open_write(part, 0644);
    if (fd < 0) return ris_error("cannot store collection object", id);
    result = rt_write_all(fd, json->text + token->start, (size_t)cache.size);
    if (platform_close(fd) != 0) result = -1;
    if (result != 0 || preserve(archive, "metadata", id, part, &cache) != 0 ||
        platform_rename_path(part, path) != 0 || save_cache(archive, "metadata", id, id, &cache) != 0 ||
        observation(archive, tool_json_true(json, tool_json_get(json, object, "deleted")) ?
                    "collection-tombstone" : "collection-object", id, source, &cache, 0) != 0)
        return ris_error("cannot publish collection object", id);
    return 0;
}

static int select_meeting(Archive *archive, const ToolJson *json, size_t object, const char *source) {
    char id[RIS_URL_CAPACITY];
    int member;
    if (!tool_json_equals(json, tool_json_get(json, object, "type"), "https://schema.oparl.org/1.1/Meeting"))
        return ris_error("non-Meeting object in calendar", source);
    if (tool_json_true(json, tool_json_get(json, object, "deleted")))
        return store_collection_object(archive, json, object, source);
    member = calendar_member(archive, json, object);
    if (member < 0) return -1;
    if (!member && !(archive->incremental &&
                    known_reference(&archive->known_resources, json, object))) return 0;
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0)
        return ris_error("Meeting has no ID", source);
    return add_scoped_job(archive, id, JOB_RESOURCE,
        !member && archive->incremental && !archive->calendar_only &&
        !known_reference(&archive->known_calendar_meetings, json, object) ? 0 : 1, TYPE_MEETING);
}

static int follow_link(Archive *archive, const ToolJson *json, size_t index, unsigned int calendar) {
    const ToolJsonToken *token = tool_json_token(json, index);
    char url[RIS_URL_CAPACITY];
    size_t child;
    if (token == 0 || token->kind == TOOL_JSON_NULL) return 0;
    if (token->kind == TOOL_JSON_ARRAY) {
        for (child = index + 1; child < token->next; child = tool_json_token(json, child)->next)
            if (follow_link(archive, json, child, calendar) != 0) return -1;
        return 0;
    }
    if (token->kind == TOOL_JSON_OBJECT) {
        if (tool_json_true(json, tool_json_get(json, index, "deleted"))) return 0;
        if (calendar && private_item(json, index)) return 0;
        if (tool_json_equals(json, tool_json_get(json, index, "type"), "https://schema.oparl.org/1.1/File") &&
            (tool_json_get(json, index, "accessUrl") != TOOL_JSON_NONE ||
             tool_json_get(json, index, "downloadUrl") != TOOL_JSON_NONE))
            return document(archive, json, index);
        index = tool_json_get(json, index, "id");
    }
    if (tool_json_string(json, index, url, sizeof(url)) != 0)
        return ris_error("invalid linked OParl resource", 0);
    if (archive->incremental) {
        char hash[65];
        hash_text(url, hash);
        if (hash_seen(&archive->known_resources, hash, 0)) return 0;
    }
    return add_scoped_job(archive, url, JOB_RESOURCE, calendar, url_type(url));
}

static int resource(Archive *archive, const Job *job) {
    static const char *paper[] = {
        "mainFile", "auxiliaryFile", "consultation", "superordinatedPaper",
        "subordinatedPaper", "relatedPaper", "originator", "underDirectionOf", "location"
    };
    static const char *consultation[] = {
        "paper", "agendaItem", "agendaitem", "meeting", "organization"
    };
    static const char *agenda[] = {
        "meeting", "consultation", "resolutionFile", "auxiliaryFile"
    };
    static const char *meeting[] = {
        "invitation", "resultsProtocol", "verbatimProtocol", "auxiliaryFile",
        "organization", "location"
    };
    static const char *calendar_consultation[] = {"organization"};
    static const char *calendar_agenda[] = {"resolutionFile","auxiliaryFile","consultation"};
    ToolJson json;
    unsigned char *bytes = 0;
    const char *const *keys = 0;
    size_t count = 0, i, type;
    int result = read_metadata(archive, job->url, META_OBJECT, &json, &bytes);
    if (result == RIS_HTTP_NOT_FOUND) return 0;
    if (result != 0) return -1;
    if (tool_json_true(&json, tool_json_get(&json, 0, "deleted"))) goto done;
    if (job->calendar && private_item(&json, 0)) goto done;
    type = resource_type(&json);
    if (job->calendar && tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Meeting")) {
        int member = calendar_member(archive, &json, 0);
        if (member < 0) { result = -1; goto done; }
        if (member == 2) {
            archive->unknown_meetings++;
            ris_warning("Meeting has no start; retained metadata without expanding calendar documents", job->url);
            goto done;
        }
        if (!member) goto done;
        archive->selected_meetings++;
    }
    if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/File")) {
        result = document(archive, &json, 0);
        goto done;
    }
    if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Paper")) {
        if (job->calendar) goto done;
        keys = paper;
        count = sizeof(paper) / sizeof(paper[0]);
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Consultation")) {
        keys = job->calendar ? calendar_consultation : consultation;
        count = job->calendar ? sizeof(calendar_consultation)/sizeof(calendar_consultation[0]) :
                               sizeof(consultation)/sizeof(consultation[0]);
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/AgendaItem")) {
        keys = job->calendar ? calendar_agenda : agenda;
        count = job->calendar ? sizeof(calendar_agenda)/sizeof(calendar_agenda[0]) : sizeof(agenda)/sizeof(agenda[0]);
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Meeting")) {
        keys = meeting;
        count = sizeof(meeting) / sizeof(meeting[0]);
    } else if (!tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Organization") &&
               !tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Person") &&
               !tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Location") &&
               !tool_json_equals(&json, type, "https://schema.oparl.org/1.1/LegislativeTerm") &&
               !tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Membership")) {
        result = ris_error("unsupported linked OParl resource type", job->url);
        goto done;
    }
    for (i = 0; i < count; i++) {
        if (follow_link(archive, &json, tool_json_get(&json, 0, keys[i]), job->calendar) != 0) {
            result = -1;
            break;
        }
        if (result == 0 && job->calendar && tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Meeting"))
            result = follow_link(archive, &json, tool_json_get(&json, 0, "agendaItem"), 1);
    }
done:
    tool_json_free(&json);
    rt_free(bytes);
    if (result == 0) {
        archive->processed_resources++;
        result = write_summary(archive, 0, 0);
    }
    return result;
}

static int process_changes(Archive *archive, const ToolJson *json, size_t object,
                           unsigned int type, const char *source) {
    char id[RIS_URL_CAPACITY], expected_type[128];
    const char *prefix = "https://schema.oparl.org/1.1/";
    size_t declared = tool_json_get(json, object, "type");
    int scoped = archive->year || archive->calendar_only;
    int known = known_reference(&archive->known_resources, json, object);
    int deleted = tool_json_true(json, tool_json_get(json, object, "deleted"));
    if (type == TYPE_ORGANIZATION && (tool_json_equals(json, declared, "at") || tool_json_equals(json, declared, "gr")))
        declared = tool_json_get(json, object, "Type");
    rt_copy_string(expected_type, sizeof(expected_type), prefix);
    rt_copy_string(expected_type + rt_strlen(prefix), sizeof(expected_type) - rt_strlen(prefix), type_names[type]);
    if (!tool_json_equals(json, declared, expected_type))
        return ris_error("unexpected object type in modification list", source);
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0 ||
        !http_valid_url(&archive->http, id))
        return ris_error("changed object has no valid ID", source);
    if (type == TYPE_FILE) {
        if (scoped && !known_reference(&archive->known_files, json, object)) return 0;
        if (store_collection_object(archive, json, object, source) != 0) return -1;
        return deleted ? 0 : document(archive, json, object);
    }
    if (deleted) {
        return !scoped || known ? store_collection_object(archive, json, object, source) : 0;
    }
    if (type == TYPE_PAPER && archive->year && !archive->calendar_only) {
        if (known) {
            archive->scanned_papers++;
            if (known_reference(&archive->selected_roots, json, object)) archive->selected_papers++;
            return add_scoped_job(archive, id, JOB_RESOURCE, 0, TYPE_PAPER);
        }
        return select_paper(archive, json, object);
    }
    if (type == TYPE_MEETING) return select_meeting(archive, json, object, source);
    if (scoped && !known && type == TYPE_AGENDA && !private_item(json, object) &&
        known_reference(&archive->known_calendar_meetings, json, tool_json_get(json, object, "meeting")))
        return add_scoped_job(archive, id, JOB_RESOURCE, 1, type);
    if (scoped && !known && type == TYPE_AGENDA && !archive->calendar_only && !private_item(json, object) &&
        known_reference(&archive->paper_context, json, tool_json_get(json, object, "consultation")))
        return add_scoped_job(archive, id, JOB_RESOURCE, 0, type);
    if (scoped && !known) return 0;
    return add_scoped_job(archive, id, JOB_RESOURCE,
        archive->calendar_only || (scoped && (type == TYPE_AGENDA || type == TYPE_CONSULTATION) &&
        !known_reference(&archive->paper_context, json, object) &&
        !known_reference(&archive->paper_context, json, tool_json_get(json, object, "consultation"))), type);
}

static int process_objects(Archive *archive, const ToolJson *json, const Job *job, const char *source) {
    JobKind kind = job->kind;
    size_t data = tool_json_get(json, 0, "data"), index;
    const ToolJsonToken *array = tool_json_token(json, data);
    if (archive->incremental && kind != JOB_BODIES) {
        for (index = data + 1; index < array->next; index = tool_json_token(json, index)->next) {
            archive->types[job->type].listed++;
            if (process_changes(archive, json, index, job->type, source) != 0) return -1;
        }
        return 0;
    }
    if (kind == JOB_CALENDAR) {
        for (index = data + 1; index < array->next; index = tool_json_token(json, index)->next) {
            archive->types[TYPE_MEETING].listed++;
            if (select_meeting(archive, json, index, source) != 0) return -1;
        }
        return 0;
    }
    if (kind == JOB_PAPERS) {
        for (index = data + 1; index < array->next; index = tool_json_token(json, index)->next) {
            archive->types[TYPE_PAPER].listed++;
            if (tool_json_true(json, tool_json_get(json, index, "deleted"))) {
                if (store_collection_object(archive, json, index, source) != 0) return -1;
            } else if (select_paper(archive, json, index) != 0) return -1;
        }
        return 0;
    }
    if (kind == JOB_BODIES) {
        for (index = data + 1; index < array->next; index = tool_json_token(json, index)->next) {
            if (!tool_json_equals(json, tool_json_get(json, index, "type"),
                             "https://schema.oparl.org/1.1/Body"))
                return ris_error("non-Body object in bodies collection", 0);
            if (discover_collections(archive, json, index) != 0) return -1;
        }
        for (index = data + 1; index < array->next; index = tool_json_token(json, index)->next)
            archive->types[archive->active_type].listed++;
    }
    for (index = data + 1; index < array->next; index++) {
        const ToolJsonToken *token = tool_json_token(json, index);
        if (token->kind == TOOL_JSON_OBJECT &&
            tool_json_equals(json, tool_json_get(json, index, "type"), "https://schema.oparl.org/1.1/File") &&
            document(archive, json, index) != 0) return -1;
    }
    return 0;
}

static int collection_url(const Archive *archive, const char *base, char *out, int filtered) {
    ToolByteBuffer buffer;
    char number[32];
    size_t path = 0, start, end, key;
    int result = -1;
    tool_byte_buffer_init(&buffer);
    while (base[path] && base[path] != '?') path++;
    if (tool_byte_buffer_append(&buffer, base, path) != 0) goto done;
    for (start = base[path] ? path + 1 : path; base[start]; start = base[end] ? end + 1 : end) {
        for (end = start; base[end] && base[end] != '&'; end++) {}
        for (key = start; key < end && base[key] != '='; key++) {}
        if ((key - start == 4 && !memcmp(base + start, "size", 4)) ||
            (filtered && ((key - start == 14 && !memcmp(base + start, "modified_since", 14)) ||
                          (key - start == 14 && !memcmp(base + start, "modified_until", 14))))) continue;
        if (end > start &&
            (tool_byte_buffer_append_char(&buffer, buffer.size == path ? '?' : '&') != 0 ||
             tool_byte_buffer_append(&buffer, base + start, end - start) != 0)) goto done;
    }
    rt_unsigned_to_string(archive->page_size, number, sizeof(number));
    if (tool_byte_buffer_append_char(&buffer, buffer.size == path ? '?' : '&') != 0 ||
        tool_byte_buffer_append_cstr(&buffer, "size=") != 0 ||
        tool_byte_buffer_append_cstr(&buffer, number) != 0) goto done;
    if (filtered) {
        size_t i;
        const char *values[2] = {archive->modified_since, archive->modified_until};
        const char *names[2] = {"&modified_since=", "&modified_until="};
        for (i = 0; i < 2; i++) {
            size_t j;
            if (tool_byte_buffer_append_cstr(&buffer, names[i]) != 0) goto done;
            for (j = 0; values[i][j]; j++) {
                if (values[i][j] == ':') {
                    if (tool_byte_buffer_append_cstr(&buffer, "%3A") != 0) goto done;
                } else if (tool_byte_buffer_append_char(&buffer, values[i][j]) != 0) goto done;
            }
        }
    }
    if (buffer.size >= RIS_URL_CAPACITY) goto done;
    memcpy(out, buffer.data, buffer.size); out[buffer.size] = 0;
    result = 0;
done:
    tool_byte_buffer_free(&buffer);
    return result == 0 ? 0 : ris_error("cannot construct collection URL", base);
}

static int validate_changes(const Archive *archive, const ToolJson *json, const char *source) {
    size_t data = tool_json_get(json, 0, "data"), index;
    const ToolJsonToken *array = tool_json_token(json, data);
    for (index = data + 1; index < array->next; index = tool_json_token(json, index)->next) {
        char modified[128];
        unsigned long long seconds;
        if (tool_json_string(json, tool_json_get(json, index, "modified"), modified, sizeof(modified)) != 0 ||
            archive_parse_timestamp(modified, &seconds) != 0 ||
            seconds < archive->sync_since || seconds > archive->started)
            return ris_error("OParl modification filter ignored or invalid timestamp; use --full", source);
    }
    return 0;
}

static int collection_impl(Archive *archive, const Job *job) {
    char url[RIS_URL_CAPACITY], next[RIS_URL_CAPACITY];
    unsigned long long seen = 0, previous = 0;
    int filtered = archive->incremental && job->kind != JOB_BODIES;
    if (collection_url(archive, job->url, url, filtered) != 0) return -1;
    for (;;) {
        ToolJson json;
        unsigned char *bytes = 0;
        unsigned long long current, pages;
        size_t pagination, link;
        int has_next, result;
        result = read_metadata(archive, url, filtered || job->kind == JOB_PAPERS || job->kind == JOB_CALENDAR ? META_INDEX : META_LIST,
                               &json, &bytes);
        /* A missing page has no next link; other collections can still be processed. */
        if (result == RIS_HTTP_NOT_FOUND) { archive->types[job->type].incomplete_collections++; return 0; }
        if (result != 0) return -1;
        pagination = tool_json_get(&json, 0, "pagination");
        tool_json_uint(&json, tool_json_get(&json, pagination, "currentPage"), &current);
        tool_json_uint(&json, tool_json_get(&json, pagination, "totalPages"), &pages);
        link = tool_json_get(&json, tool_json_get(&json, 0, "links"), "next");
        has_next = link != TOOL_JSON_NONE && tool_json_token(&json, link)->kind != TOOL_JSON_NULL;
        result = 0;
        if (current != previous + 1 || (has_next && current >= pages) ||
            (!has_next && current < pages)) {
            result = ris_error("inconsistent or incomplete pagination", url);
        } else if (has_next &&
                   (tool_json_string(&json, link, next, sizeof(next)) != 0 ||
                    !http_valid_url(&archive->http, next) || rt_strcmp(next, url) == 0)) {
            result = ris_error("invalid next-page link", url);
        } else {
            if (filtered) result = validate_changes(archive, &json, url);
            if (result == 0) result = process_objects(archive, &json, job, url);
        }
        tool_json_free(&json);
        rt_free(bytes);
        if (result == 0) result = write_summary(archive, 0, 0);
        if (result != 0 || !has_next) return result;
        seen++;
        if (archive->max_pages != 0 && seen >= archive->max_pages) {
            archive->limited = 1;
            archive->types[job->type].incomplete_collections++;
            return 0;
        }
        previous = current;
        if (collection_url(archive, next, url, filtered) != 0) return -1;
    }
}

static int collection(Archive *archive, const Job *job) {
    ImportType *type = &archive->types[job->type];
    unsigned long long before = type->incomplete_collections;
    int result;
    result = collection_impl(archive, job);
    if (result != 0) type->incomplete_collections++;
    else if (before == type->incomplete_collections) type->complete_collections++;
    return result;
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

static int validate_archive(Archive *archive) {
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
    if (url_type(id) != type) return 0;
    hash_text(id, hash);
    return hash_seen(set, hash, 1) < 0 ? -1 : 0;
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
    if (tool_json_string(json, index, id, sizeof(id)) != 0 || url_type(id) != TYPE_PAPER) return 0;
    hash_text(id, hash);
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
    hash_text(id, hash);
    if (memcmp(hash, name, 64)) goto done;
    if (!http_valid_url(&archive->http, id)) {
        result = ris_error("invalid archived resource URL; use --full", path); goto done;
    }
    if (hash_seen(&archive->known_resources, hash, 1) < 0) { result = -1; goto done; }
    type = resource_type(&json);
    if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/File")) {
        result = hash_seen(&archive->known_files, hash, 1) < 0 ? -1 : 0;
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Paper")) {
        keys[0] = "mainFile"; keys[1] = "auxiliaryFile";
        result = remember_links(archive, &archive->paper_context, &json,
                                tool_json_get(&json, 0, "consultation"), TYPE_CONSULTATION);
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Consultation") &&
               archived_paper(archive, &json, tool_json_get(&json, 0, "paper"))) {
        if (hash_seen(&archive->paper_context, hash, 1) < 0 ||
            remember_links(archive, &archive->paper_context, &json,
                           tool_json_get(&json, 0, "agendaItem"), TYPE_AGENDA) != 0 ||
            remember_links(archive, &archive->paper_context, &json,
                           tool_json_get(&json, 0, "agendaitem"), TYPE_AGENDA) != 0) result = -1;
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Meeting") && !private_item(&json, 0)) {
        int member = calendar_member(archive, &json, 0);
        if (member < 0) { result = -1; goto done; }
        if (member == 1 && hash_seen(&archive->known_calendar_meetings, hash, 1) < 0) {
            result = -1; goto done;
        }
        keys[0] = "invitation"; keys[1] = "resultsProtocol";
        keys[2] = "verbatimProtocol"; keys[3] = "auxiliaryFile";
    } else if (tool_json_equals(&json, type, "https://schema.oparl.org/1.1/AgendaItem") &&
               !private_item(&json, 0)) {
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
    return hash_seen(&archive->known_files, hash, 1) < 0 ? -1 : 0;
}

static int prepare_sync(Archive *archive) {
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

static int retain_selection(Archive *archive) {
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
        hash_text(id, hash);
        if (hash_seen(&archive->selected_roots, hash, 1) < 0) { result = -1; break; }
    }
    if (!result && rt_write_all(archive->selection_fd, bytes, length) != 0) result = -1;
    if (!result && length && bytes[length - 1] != '\n' &&
        rt_write_char(archive->selection_fd, '\n') != 0) result = -1;
    rt_free(bytes);
    return result == 0 ? 0 : ris_error("invalid or unwritable existing Paper selection", path);
}

static int open_manifest(const Archive *archive, const char *name, int *fd) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY];
    if (rt_join_path(archive->output, name, path, sizeof(path)) != 0 ||
        temporary_path(path, part) != 0)
        return ris_error("manifest path is too long", name);
    *fd = platform_open_write(part, 0644);
    return *fd < 0 ? ris_error("cannot open archive manifest", part) : 0;
}

static int finish_manifest(const Archive *archive, const char *name, int *fd) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY];
    int result;
    if (*fd < 0) return 0;
    result = platform_close(*fd);
    *fd = -1;
    if (rt_join_path(archive->output, name, path, sizeof(path)) != 0 ||
        temporary_path(path, part) != 0 || result != 0 ||
        platform_rename_path(part, path) != 0)
        return ris_error("cannot publish archive manifest", name);
    return 0;
}

static int queue_unavailable_files(Archive *archive) {
    char path[PATH_CAPACITY];
    PlatformDirEntry info;
    unsigned char *bytes = 0;
    size_t length, start, end;
    unsigned long long count = 0;
    int result = 0;
    if (!archive->incremental || archive->metadata_only) return 0;
    if (rt_join_path(archive->output, "unavailable-files.jsonl", path, sizeof(path)) != 0)
        return ris_error("unavailable file report path is too long", archive->output);
    if (platform_get_path_info(path, &info) != 0)
        return archive->previous_unavailable ?
            ris_error("missing unavailable file report; use --full", path) : 0;
    if (info.is_dir || info.size > META_LIMIT || tool_read_all_input(path, &bytes, &length) != 0)
        return ris_error("cannot read unavailable file report; use --full", path);
    for (start = 0; start < length; start = end + 1) {
        char id[RIS_URL_CAPACITY], hash[65];
        ToolJson json;
        for (end = start; end < length && bytes[end] != '\n'; end++) {}
        if (end == start) continue;
        if (tool_json_parse(&json, (const char *)bytes + start, end - start) != 0) {
            result = -1; break;
        }
        result = tool_json_string(&json, tool_json_get(&json, 0, "id"), id, sizeof(id));
        if (!result && (!tool_json_equals(&json, tool_json_get(&json, 0, "reason"), "access-denied") ||
                        !http_valid_url(&archive->http, id) || url_type(id) != TYPE_FILE))
            result = -1;
        tool_json_free(&json);
        if (result) break;
        hash_text(id, hash);
        if (!hash_seen(&archive->known_files, hash, 0)) { result = -1; break; }
        count++;
        if (!hash_seen(&archive->seen_files, hash, 0) && add_job(archive, id, JOB_RESOURCE) != 0) {
            result = -1; break;
        }
    }
    if (archive->previous_unavailable && count != archive->previous_unavailable) result = -1;
    if (!result) archive->previous_unavailable = count;
    rt_free(bytes);
    return result ? ris_error("invalid unavailable file report; use --full", path) : 0;
}

static int run(Archive *archive) {
    ToolJson system;
    unsigned char *bytes = 0;
    char bodies[RIS_URL_CAPACITY];
    size_t i;
    int result, retries_queued = 0;
    {
        size_t used;
        archive->started = (unsigned long long)platform_get_epoch_time();
        rt_unsigned_to_string(archive->started, archive->run_id, sizeof(archive->run_id));
        used = rt_strlen(archive->run_id); archive->run_id[used++] = '-';
        rt_unsigned_to_string(platform_get_monotonic_time_ns(), archive->run_id + used, sizeof(archive->run_id) - used);
    }
    if (prepare_sync(archive) != 0) return -1;
    if (prepare_directory(archive, "metadata") != 0 ||
        prepare_directory(archive, "cache") != 0 ||
        prepare_directory(archive, "index") != 0 ||
        prepare_directory(archive, "imports") != 0 ||
        prepare_directory(archive, "observations") != 0 ||
        (!archive->metadata_only && prepare_directory(archive, "documents") != 0)) return -1;
    {
        char name[128];
        size_t length = rt_strlen(archive->run_id);
        memcpy(name, "observations/", 13); memcpy(name + 13, archive->run_id, length);
        memcpy(name + 13 + length, ".jsonl", 7);
        if (open_manifest(archive, name, &archive->observations_fd) != 0) return -1;
    }
    if (archive->year != 0 && !archive->calendar_only) {
        if (prepare_directory(archive, "index") != 0 ||
            open_manifest(archive, "selection.jsonl", &archive->selection_fd) != 0)
            return -1;
        if (retain_selection(archive) != 0) return -1;
    }
    if (archive->skip_unavailable_files && !archive->metadata_only &&
        open_manifest(archive, "unavailable-files.jsonl", &archive->unavailable_fd) != 0)
        return -1;
    if (archive->accept_size_mismatch &&
        open_manifest(archive, "size-mismatches.jsonl", &archive->mismatch_fd) != 0)
        return -1;
    if (open_manifest(archive, "missing-resources.jsonl", &archive->missing_fd) != 0)
        return -1;
    if (write_summary(archive, 0, 1) != 0) return -1;
    archive->active_type = TYPE_SYSTEM; archive->types[TYPE_SYSTEM].scheduled++;
    result = read_metadata(archive, archive->system, META_SYSTEM, &system, &bytes);
    if (result == RIS_HTTP_NOT_FOUND)
        return finish_manifest(archive, "selection.jsonl", &archive->selection_fd);
    if (result != 0) return -1;
    archive->types[TYPE_SYSTEM].finished++;
    result = tool_json_string(&system, tool_json_get(&system, 0, "body"), bodies, sizeof(bodies));
    tool_json_free(&system);
    rt_free(bytes);
    if (result != 0 || add_job(archive, bodies, JOB_BODIES) != 0) return -1;
    for (i = 0; i < archive->jobs.count; i++) {
        Job job = *(const Job *)tool_array_get_const(&archive->jobs, i);
        ImportType *type = &archive->types[job.type];
        unsigned long long errors = type->errors;
        archive->active_type = job.type;
        if (job.kind == JOB_RESOURCE) {
            if (finish_manifest(archive, "selection.jsonl", &archive->selection_fd) != 0 ||
                resource(archive, &job) != 0) {
                if (type->errors == errors) type->errors++;
                return -1;
            }
        } else if (collection(archive, &job) != 0) {
            if (type->errors == errors) type->errors++;
            return -1;
        }
        type->finished++;
        if (i + 1 == archive->jobs.count && !retries_queued) {
            retries_queued = 1;
            if (queue_unavailable_files(archive) != 0) return -1;
        }
    }
    return finish_manifest(archive, "selection.jsonl", &archive->selection_fd);
}

static int write_summary(Archive *archive, int success, int force) {
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
        temporary_path(path, part) != 0) return ris_error("summary path is too long", 0);
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
        temporary_path(import_path, import_part) != 0 || tool_read_all_input(part, &bytes, &length) != 0)
        return ris_error("cannot prepare import record", path);
    result = tool_write_file_all(import_part, bytes, length);
    rt_free(bytes);
    if (result != 0 || platform_rename_path(import_part, import_path) != 0 ||
        platform_rename_path(part, path) != 0)
        return ris_error("cannot publish archive summary", path);
    archive->summary_written_ns = now;
    return 0;
}

int main(int argc, char **argv) {
    Archive archive;
    int parsed = options(&archive, argc, argv), result;
    if (parsed != 0) {
        tool_array_free(&archive.jobs);
        return parsed > 0 ? 0 : 1;
    }
    if (validate_archive(&archive) != 0) {
        tool_array_free(&archive.jobs);
        return 1;
    }
    result = run(&archive);
    if (result == 0 && flush_documents(&archive) != 0) result = -1;
    if (result == 0 &&
        finish_manifest(&archive, "missing-resources.jsonl", &archive.missing_fd) != 0)
        result = -1;
    {
        char name[128];
        size_t length = rt_strlen(archive.run_id);
        memcpy(name, "observations/", 13); memcpy(name + 13, archive.run_id, length);
        memcpy(name + 13 + length, ".jsonl", 7);
        if (finish_manifest(&archive, name, &archive.observations_fd) != 0) result = -1;
    }
    archive.finalizing = 1; archive.finished = (unsigned long long)platform_get_epoch_time();
    if (result == 0 && archive.incremental && !archive.metadata_only &&
        archive.previous_unavailable && archive.missing_resources)
        result = ris_error("document retries incomplete after HTTP 404; previous unavailable report retained",
                           archive.output);
    if (result == 0 &&
        finish_manifest(&archive, "unavailable-files.jsonl", &archive.unavailable_fd) != 0)
        result = -1;
    if (result == 0 &&
        finish_manifest(&archive, "size-mismatches.jsonl", &archive.mismatch_fd) != 0)
        result = -1;
    if (write_summary(&archive, result == 0, 1) != 0) result = -1;
    if (result == 0) {
        tool_write_styled(2, tool_get_global_color_mode(),
                          archive.missing_resources || archive.unavailable_files || archive.limited || archive.unknown_meetings ||
                          (archive.incremental && !archive.baseline_complete) ?
                          TOOL_STYLE_BOLD_YELLOW : TOOL_STYLE_PLAIN,
                          archive.missing_resources ? "INCOMPLETE (HTTP 404)" :
                          archive.unavailable_files ? "INCOMPLETE (unavailable documents)" :
                          archive.limited ? "INCOMPLETE (configured limits)" :
                          archive.unknown_meetings ? "INCOMPLETE (unknown calendar dates)" :
                          archive.incremental && !archive.baseline_complete ?
                          "INCOMPLETE (existing archive gaps; recent update completed)" : "Complete");
        rt_write_cstr(2, ": new metadata pages=");
        rt_write_uint(2, archive.downloaded_pages);
        rt_write_cstr(2, ", new documents=");
        rt_write_uint(2, archive.downloaded_files);
        rt_write_cstr(2, ", cached documents=");
        rt_write_uint(2, archive.cached_files);
        rt_write_cstr(2, ", unavailable documents=");
        rt_write_uint(2, archive.unavailable_files);
        rt_write_cstr(2, ", missing resources=");
        rt_write_uint(2, archive.missing_resources);
        rt_write_cstr(2, ", size mismatches=");
        rt_write_uint(2, archive.mismatched_files);
        rt_write_char(2, '\n');
    }
    http_free(&archive.http);
    while (archive.pending_count != 0) rt_free(archive.pending[--archive.pending_count]);
    tool_array_free(&archive.jobs);
    if (archive.selection_fd >= 0) platform_close(archive.selection_fd);
    if (archive.unavailable_fd >= 0 && platform_close(archive.unavailable_fd) != 0) {
        ris_error("cannot close unavailable file report", 0);
        result = -1;
    }
    if (archive.missing_fd >= 0 && platform_close(archive.missing_fd) != 0) {
        ris_error("cannot close missing resource report", 0);
        result = -1;
    }
    if (archive.mismatch_fd >= 0 && platform_close(archive.mismatch_fd) != 0) {
        ris_error("cannot close size mismatch report", 0);
        result = -1;
    }
    rt_free(archive.seen_files.entries);
    rt_free(archive.seen_jobs.entries);
    rt_free(archive.known_resources.entries);
    rt_free(archive.known_files.entries);
    rt_free(archive.known_calendar_meetings.entries);
    rt_free(archive.paper_context.entries);
    rt_free(archive.selected_roots.entries);
    rt_free(archive.seen_missing.entries);
    return result != 0 ? 1 :
           archive.missing_resources != 0 || archive.unavailable_files != 0 || archive.unknown_meetings != 0 ||
           (archive.incremental && !archive.baseline_complete) ? 3 :
           archive.limited ? 2 : 0;
}

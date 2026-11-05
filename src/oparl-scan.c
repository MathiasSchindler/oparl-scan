#include "http.h"
#include "json.h"
#include "runtime.h"
#include "tool_util.h"
#include "crypto/sha256.h"

#define META_LIMIT (32ULL * 1024ULL * 1024ULL)
#define FILE_LIMIT (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define SUMMARY_INTERVAL_NS 500000000ULL
#define PATH_CAPACITY 1024U
#define DEFAULT_SYSTEM "https://www.potsdam.sitzung-online.de/oparl/system"
#define PENDING_DOCUMENTS 32U

typedef enum {
    JOB_COLLECTION, JOB_BODIES, JOB_PAPERS, JOB_RESOURCE
} JobKind;

typedef enum {
    META_LIST, META_SYSTEM, META_OBJECT, META_INDEX
} MetaKind;

typedef struct {
    char url[RIS_URL_CAPACITY];
    JobKind kind;
} Job;

typedef struct {
    char (*entries)[65];
    size_t capacity;
    size_t count;
} SeenSet;

typedef struct {
    HttpTransfer transfer;
    char id[RIS_URL_CAPACITY];
    char url[RIS_URL_CAPACITY];
    char filename[512];
    char path[PATH_CAPACITY];
    char part[PATH_CAPACITY];
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
    int limited;
    ToolArray jobs;
    SeenSet seen_files;
    SeenSet seen_jobs;
    SeenSet seen_missing;
    DocumentDownload *pending[PENDING_DOCUMENTS];
    size_t pending_count;
} Archive;

static int write_summary(Archive *archive, int success, int force);
static int download_metadata(Archive *archive, const char *url, const char *path,
                             unsigned long long *size);
static int flush_documents(Archive *archive);

static void usage(void) {
    rt_write_cstr(1,
        "Usage: oparl-scan [OPTIONS]\n"
        "  --output DIR       Archive directory (default: downloads)\n"
        "  --metadata-only    Do not download document binaries\n"
        "  --skip-unavailable-files  Report access-denied documents and continue\n"
        "  --accept-size-mismatch   Report complete transfers with wrong OParl sizes\n"
        "  --year YYYY        Select reference prefix YY/, including later follow-ups\n"
        "  --refresh          Replace cached metadata and documents\n"
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
        "HTTP 404: report missing URLs, continue, and mark the archive incomplete.\n"
        "Exit status: 0 complete, 1 error, 2 limits, 3 missing resources.\n");
}

static int options(Archive *archive, int argc, char **argv) {
    int i;
    rt_memset(archive, 0, sizeof(*archive));
    archive->output = "downloads";
    archive->system = DEFAULT_SYSTEM;
    archive->page_size = 100;
    archive->selection_fd = -1;
    archive->unavailable_fd = -1;
    archive->missing_fd = -1;
    archive->mismatch_fd = -1;
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
        if (rt_strcmp(flag, "--skip-unavailable-files") == 0) {
            archive->skip_unavailable_files = 1;
            continue;
        }
        if (rt_strcmp(flag, "--accept-size-mismatch") == 0) {
            archive->accept_size_mismatch = 1;
            continue;
        }
        if (rt_strcmp(flag, "--refresh") == 0) {
            archive->refresh = 1;
            continue;
        }
        if (i + 1 == argc) return ris_error("missing option value", flag);
        value = argv[++i];
        if (rt_strcmp(flag, "--output") == 0) archive->output = value;
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
    archive->http.allow_loopback = rt_strncmp(archive->system, "http://127.0.0.1:", 17) == 0 ||
                                   rt_strncmp(archive->system, "http://127.0.0.1/", 17) == 0;
    if (!http_valid_url(&archive->http, archive->system))
        return ris_error("system URL is not allowed", archive->system);
    return 0;
}

static void hash_text(const char *text, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    size_t i;
    crypto_sha256_hash((const unsigned char *)text, rt_strlen(text), digest);
    for (i = 0; i < sizeof(digest); i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
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

static size_t resource_type(const Json *json) {
    size_t type = json_get(json, 0, "type");
    if (json_equals(json, type, "at") || json_equals(json, type, "gr")) {
        size_t legacy = json_get(json, 0, "Type");
        if (json_equals(json, legacy, "https://schema.oparl.org/1.1/Organization"))
            return legacy;
    }
    return type;
}

static int page_shape(const Json *json, MetaKind kind) {
    const JsonToken *root = json_token(json, 0), *data;
    unsigned long long current, pages, total;
    size_t pagination;
    if (root == 0 || root->kind != JSON_OBJECT) return -1;
    if (kind == META_SYSTEM) {
        char body[RIS_URL_CAPACITY];
        return json_equals(json, json_get(json, 0, "type"),
                           "https://schema.oparl.org/1.1/System") &&
               json_string(json, json_get(json, 0, "body"), body, sizeof(body)) == 0 ? 0 : -1;
    }
    if (kind == META_OBJECT) {
        char id[RIS_URL_CAPACITY], type[256];
        return json_string(json, json_get(json, 0, "id"), id, sizeof(id)) == 0 &&
               json_string(json, resource_type(json), type, sizeof(type)) == 0 &&
               rt_strncmp(type, "https://schema.oparl.org/1.1/", 29) == 0 ? 0 : -1;
    }
    data = json_token(json, json_get(json, 0, "data"));
    pagination = json_get(json, 0, "pagination");
    if (data == 0 || data->kind != JSON_ARRAY ||
        json_uint(json, json_get(json, pagination, "currentPage"), &current) != 0 ||
        json_uint(json, json_get(json, pagination, "totalPages"), &pages) != 0 ||
        json_uint(json, json_get(json, pagination, "totalElements"), &total) != 0 ||
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

static int read_metadata(Archive *archive, const char *url, MetaKind kind,
                         Json *json, unsigned char **bytes) {
    char hash[65], name[80], path[PATH_CAPACITY], part[PATH_CAPACITY];
    PlatformDirEntry info;
    unsigned long long size;
    size_t length;
    int cached, result;
    hash_text(url, hash);
    rt_copy_string(name, sizeof(name), hash);
    memcpy(name + 64, ".json", 6);
    if (archive_path(archive, kind == META_INDEX ? "index" : "metadata", name, path) != 0 ||
        temporary_path(path, part) != 0) return -1;
    cached = !archive->refresh && platform_get_path_info(path, &info) == 0;
    if (cached) {
        if (info.is_dir || info.size > META_LIMIT)
            return ris_error("invalid metadata cache; use --refresh", path);
    } else {
        result = download_metadata(archive, url, part, &size);
        if (result != 0) return result;
    }
    if (tool_read_all_input(cached ? path : part, bytes, &length) != 0)
        return ris_error("cannot read metadata", cached ? path : part);
    if (json_parse(json, (const char *)*bytes, length) != 0) {
        rt_free(*bytes);
        *bytes = 0;
        return ris_error("invalid JSON; cache can be replaced with --refresh", url);
    }
    if (page_shape(json, kind) != 0 ||
        (kind == META_OBJECT &&
         !json_equals(json, json_get(json, 0, "id"), url))) {
        json_free(json);
        rt_free(*bytes);
        *bytes = 0;
        return ris_error("unexpected OParl response shape", url);
    }
    if (!cached) {
        if (platform_rename_path(part, path) != 0) {
            json_free(json);
            rt_free(*bytes);
            *bytes = 0;
            return ris_error("cannot publish metadata download", path);
        }
        archive->downloaded_pages++;
        rt_write_cstr(2, "metadata: ");
        rt_write_line(2, url);
    }
    return 0;
}

static int add_job(Archive *archive, const char *url, JobKind kind) {
    char hash[65];
    Job *job;
    if (!http_valid_url(&archive->http, url)) return ris_error("collection URL is not allowed", url);
    hash_text(url, hash);
    if (hash_seen(&archive->seen_jobs, hash, 0)) return 0;
    job = (Job *)tool_array_append(&archive->jobs);
    if (job == 0) return ris_error("cannot allocate collection queue", 0);
    rt_copy_string(job->url, sizeof(job->url), url);
    job->kind = kind;
    return hash_seen(&archive->seen_jobs, hash, 1) < 0 ? -1 : 0;
}

static int discover_collections(Archive *archive, const Json *json, size_t body) {
    static const char *keys[] = {
        "organization", "person", "meeting", "membership", "locationList",
        "legislativeTermList", "agendaItem", "paper", "consultation", "file"
    };
    size_t i;
    char url[RIS_URL_CAPACITY];
    if (json_true(json, json_get(json, body, "deleted"))) return 0;
    if (archive->year != 0) {
        if (json_string(json, json_get(json, body, "paper"), url, sizeof(url)) != 0)
            return ris_error("Body has no paper collection for year selection", 0);
        return add_job(archive, url, JOB_PAPERS);
    }
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        size_t member = json_get(json, body, keys[i]);
        if (member == JSON_NONE) continue;
        if (json_string(json, member, url, sizeof(url)) != 0)
            return ris_error("invalid collection link in Body", keys[i]);
        if (add_job(archive, url, JOB_COLLECTION) != 0) return -1;
    }
    return 0;
}

static int complete_document(Archive *archive, const DocumentDownload *download) {
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
    if (result != 0) return -1;
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
    if (platform_rename_path(download->part, download->path) != 0)
        return ris_error("cannot publish document", download->path);
    if (expected != RIS_UNKNOWN_SIZE && received != expected) archive->mismatched_files++;
    archive->downloaded_files++;
    rt_write_cstr(2, "document: ");
    rt_write_line(2, download->path);
    return 0;
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

static int download_metadata(Archive *archive, const char *url, const char *path,
                             unsigned long long *size) {
    HttpTransfer metadata = {url, path, META_LIMIT, RIS_UNKNOWN_SIZE, 0, 0, -1};
    DocumentDownload *document = 0;
    size_t i;
    int result;
    for (i = 0; i < archive->pending_count; i++) {
        if (!http_same_host(&archive->http, url, archive->pending[i]->url)) {
            document = archive->pending[i];
            break;
        }
    }
    result = perform_downloads(archive, &metadata, document, 0);
    if (metadata.result == RIS_HTTP_NOT_FOUND &&
        record_missing(archive, url, "metadata") != 0) return -1;
    if (result != 0) return -1;
    *size = metadata.received;
    return metadata.result;
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

static int document(Archive *archive, const Json *json, size_t object) {
    char id[RIS_URL_CAPACITY], url[RIS_URL_CAPACITY], filename[512];
    char hash[65], name[180], path[PATH_CAPACITY], part[PATH_CAPACITY];
    unsigned long long expected = RIS_UNKNOWN_SIZE;
    size_t token, i, used;
    DocumentDownload *download, *other = 0;
    PlatformDirEntry info;
    if (archive->metadata_only || json_true(json, json_get(json, object, "deleted"))) return 0;
    if (json_string(json, json_get(json, object, "id"), id, sizeof(id)) != 0)
        return ris_error("File has no valid id", 0);
    token = json_get(json, object, "downloadUrl");
    if (token == JSON_NONE) token = json_get(json, object, "accessUrl");
    if (json_string(json, token, url, sizeof(url)) != 0 ||
        !http_valid_url(&archive->http, url)) return ris_error("File has no allowed download URL", id);
    token = json_get(json, object, "size");
    if (token != JSON_NONE && json_uint(json, token, &expected) != 0)
        return ris_error("invalid File size", id);
    if (expected != RIS_UNKNOWN_SIZE && expected > FILE_LIMIT)
        return ris_error("document exceeds 4 GiB limit", id);
    token = json_get(json, object, "fileName");
    if (token == JSON_NONE) {
        rt_copy_string(filename, sizeof(filename), "document.bin");
    } else if (json_string(json, token, filename, sizeof(filename)) != 0) {
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
    if (archive_path(archive, "documents", name, path) != 0 ||
        temporary_path(path, part) != 0) return -1;
    if (!archive->refresh && platform_get_path_info(path, &info) == 0) {
        if (info.is_dir)
            return ris_error("document cache has wrong size; use --refresh", path);
        if (expected == RIS_UNKNOWN_SIZE || expected == info.size) {
            archive->cached_files++;
            return hash_seen(&archive->seen_files, hash, 1) < 0 ? -1 : 0;
        }
        if (!archive->accept_size_mismatch)
            return ris_error("document cache has wrong size; use --refresh", path);
        ris_warning("rechecking cached document with a size mismatch", id);
    }
    if (archive->pending_count == PENDING_DOCUMENTS ||
        (archive->max_files != 0 &&
         archive->downloaded_files + archive->pending_count >= archive->max_files))
        if (flush_documents(archive) != 0) return -1;
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
    download->transfer = (HttpTransfer){download->url, download->part, FILE_LIMIT, expected,
                                        archive->accept_size_mismatch, 0, -1};
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

static int select_paper(Archive *archive, const Json *json, size_t object) {
    char reference[256], id[RIS_URL_CAPACITY], hash[65];
    int fd = archive->selection_fd;
    if (json_true(json, json_get(json, object, "deleted"))) return 0;
    archive->scanned_papers++;
    if (!json_equals(json, json_get(json, object, "type"), "https://schema.oparl.org/1.1/Paper") ||
        json_string(json, json_get(json, object, "reference"), reference, sizeof(reference)) != 0)
        return ris_error("Paper has no valid reference for year selection", 0);
    if (rt_strlen(reference) < 3 || reference[0] != (char)('0' + (archive->year % 100) / 10) ||
        reference[1] != (char)('0' + archive->year % 10) || reference[2] != '/') return 0;
    if (json_string(json, json_get(json, object, "id"), id, sizeof(id)) != 0)
        return ris_error("selected Paper has no id", reference);
    hash_text(id, hash);
    if (hash_seen(&archive->seen_jobs, hash, 0)) return 0;
    if (add_job(archive, id, JOB_RESOURCE) != 0) return -1;
    if (rt_write_cstr(fd, "{\"id\":") != 0 || tool_json_write_string(fd, id) != 0 ||
        tool_json_field_string(fd, "reference", reference) != 0 ||
        rt_write_cstr(fd, "}\n") != 0)
        return ris_error("cannot write selected paper index", reference);
    archive->selected_papers++;
    return 0;
}

static int follow_link(Archive *archive, const Json *json, size_t index) {
    const JsonToken *token = json_token(json, index);
    char url[RIS_URL_CAPACITY];
    size_t child;
    if (token == 0 || token->kind == JSON_NULL) return 0;
    if (token->kind == JSON_ARRAY) {
        for (child = index + 1; child < token->next; child = json_token(json, child)->next)
            if (follow_link(archive, json, child) != 0) return -1;
        return 0;
    }
    if (token->kind == JSON_OBJECT) {
        if (json_true(json, json_get(json, index, "deleted"))) return 0;
        if (json_equals(json, json_get(json, index, "type"), "https://schema.oparl.org/1.1/File") &&
            (json_get(json, index, "accessUrl") != JSON_NONE ||
             json_get(json, index, "downloadUrl") != JSON_NONE))
            return document(archive, json, index);
        index = json_get(json, index, "id");
    }
    if (json_string(json, index, url, sizeof(url)) != 0)
        return ris_error("invalid linked OParl resource", 0);
    return add_job(archive, url, JOB_RESOURCE);
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
    Json json;
    unsigned char *bytes = 0;
    const char *const *keys = 0;
    size_t count = 0, i, type;
    int result = read_metadata(archive, job->url, META_OBJECT, &json, &bytes);
    if (result == RIS_HTTP_NOT_FOUND) return 0;
    if (result != 0) return -1;
    if (json_true(&json, json_get(&json, 0, "deleted"))) goto done;
    type = resource_type(&json);
    if (json_equals(&json, type, "https://schema.oparl.org/1.1/File")) {
        result = document(archive, &json, 0);
        goto done;
    }
    if (json_equals(&json, type, "https://schema.oparl.org/1.1/Paper")) {
        keys = paper;
        count = sizeof(paper) / sizeof(paper[0]);
    } else if (json_equals(&json, type, "https://schema.oparl.org/1.1/Consultation")) {
        keys = consultation;
        count = sizeof(consultation) / sizeof(consultation[0]);
    } else if (json_equals(&json, type, "https://schema.oparl.org/1.1/AgendaItem")) {
        keys = agenda;
        count = sizeof(agenda) / sizeof(agenda[0]);
    } else if (json_equals(&json, type, "https://schema.oparl.org/1.1/Meeting")) {
        keys = meeting;
        count = sizeof(meeting) / sizeof(meeting[0]);
    } else if (!json_equals(&json, type, "https://schema.oparl.org/1.1/Organization") &&
               !json_equals(&json, type, "https://schema.oparl.org/1.1/Person") &&
               !json_equals(&json, type, "https://schema.oparl.org/1.1/Location") &&
               !json_equals(&json, type, "https://schema.oparl.org/1.1/LegislativeTerm") &&
               !json_equals(&json, type, "https://schema.oparl.org/1.1/Membership")) {
        result = ris_error("unsupported linked OParl resource type", job->url);
        goto done;
    }
    for (i = 0; i < count; i++) {
        if (follow_link(archive, &json, json_get(&json, 0, keys[i])) != 0) {
            result = -1;
            break;
        }
    }
done:
    json_free(&json);
    rt_free(bytes);
    if (result == 0) {
        archive->processed_resources++;
        result = write_summary(archive, 0, 0);
    }
    return result;
}

static int process_objects(Archive *archive, const Json *json, JobKind kind) {
    size_t data = json_get(json, 0, "data"), index;
    const JsonToken *array = json_token(json, data);
    if (kind == JOB_PAPERS) {
        for (index = data + 1; index < array->next; index = json_token(json, index)->next)
            if (select_paper(archive, json, index) != 0) return -1;
        return 0;
    }
    if (kind == JOB_BODIES) {
        for (index = data + 1; index < array->next; index = json_token(json, index)->next) {
            if (!json_equals(json, json_get(json, index, "type"),
                             "https://schema.oparl.org/1.1/Body"))
                return ris_error("non-Body object in bodies collection", 0);
            if (discover_collections(archive, json, index) != 0) return -1;
        }
    }
    for (index = data + 1; index < array->next; index++) {
        const JsonToken *token = json_token(json, index);
        if (token->kind == JSON_OBJECT &&
            json_equals(json, json_get(json, index, "type"), "https://schema.oparl.org/1.1/File") &&
            document(archive, json, index) != 0) return -1;
    }
    return 0;
}

static int first_url(const Archive *archive, const char *base, char *out) {
    char number[32];
    size_t length = rt_strlen(base), digits, i;
    int query = 0;
    rt_unsigned_to_string(archive->page_size, number, sizeof(number));
    digits = rt_strlen(number);
    if (length + digits + 7 >= RIS_URL_CAPACITY) return ris_error("collection URL is too long", base);
    for (i = 0; i < length; i++) if (base[i] == '?') query = 1;
    memcpy(out, base, length);
    out[length++] = query ? '&' : '?';
    memcpy(out + length, "size=", 5);
    memcpy(out + length + 5, number, digits + 1);
    return 0;
}

static int collection(Archive *archive, const Job *job) {
    char url[RIS_URL_CAPACITY], next[RIS_URL_CAPACITY];
    unsigned long long seen = 0, previous = 0;
    if (first_url(archive, job->url, url) != 0) return -1;
    for (;;) {
        Json json;
        unsigned char *bytes = 0;
        unsigned long long current, pages;
        size_t pagination, link;
        int has_next, result;
        result = read_metadata(archive, url, job->kind == JOB_PAPERS ? META_INDEX : META_LIST,
                               &json, &bytes);
        /* A missing page has no next link; other collections can still be processed. */
        if (result == RIS_HTTP_NOT_FOUND) return 0;
        if (result != 0) return -1;
        pagination = json_get(&json, 0, "pagination");
        json_uint(&json, json_get(&json, pagination, "currentPage"), &current);
        json_uint(&json, json_get(&json, pagination, "totalPages"), &pages);
        link = json_get(&json, json_get(&json, 0, "links"), "next");
        has_next = link != JSON_NONE && json_token(&json, link)->kind != JSON_NULL;
        result = 0;
        if (current != previous + 1 || (has_next && current >= pages) ||
            (!has_next && current < pages)) {
            result = ris_error("inconsistent or incomplete pagination", url);
        } else if (has_next &&
                   (json_string(&json, link, next, sizeof(next)) != 0 ||
                    !http_valid_url(&archive->http, next) || rt_strcmp(next, url) == 0)) {
            result = ris_error("invalid next-page link", url);
        } else {
            result = process_objects(archive, &json, job->kind);
        }
        json_free(&json);
        rt_free(bytes);
        if (result == 0) result = write_summary(archive, 0, 0);
        if (result != 0 || !has_next) return result;
        seen++;
        if (archive->max_pages != 0 && seen >= archive->max_pages) {
            archive->limited = 1;
            return 0;
        }
        previous = current;
        rt_copy_string(url, sizeof(url), next);
    }
}

static int validate_archive(const Archive *archive) {
    char path[PATH_CAPACITY];
    unsigned char *bytes = 0;
    size_t length, token;
    PlatformDirEntry info;
    Json json;
    unsigned long long year = 0;
    int valid;
    if (rt_join_path(archive->output, "archive.json", path, sizeof(path)) != 0)
        return ris_error("summary path is too long", 0);
    if (platform_get_path_info(path, &info) != 0) return 0;
    if (info.size > META_LIMIT || tool_read_all_input(path, &bytes, &length) != 0)
        return ris_error("cannot read existing archive identity", path);
    if (json_parse(&json, (const char *)bytes, length) != 0) {
        rt_free(bytes);
        return ris_error("invalid existing archive identity", path);
    }
    token = json_get(&json, 0, "referenceYear");
    valid = (token == JSON_NONE || json_uint(&json, token, &year) == 0) &&
            year == archive->year &&
            json_equals(&json, json_get(&json, 0, "system"), archive->system);
    json_free(&json);
    rt_free(bytes);
    return valid ? 0 : ris_error("archive has a different system/year; use a separate output directory", path);
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

static int run(Archive *archive) {
    Json system;
    unsigned char *bytes = 0;
    char bodies[RIS_URL_CAPACITY];
    size_t i;
    int result;
    if (prepare_directory(archive, "metadata") != 0 ||
        (!archive->metadata_only && prepare_directory(archive, "documents") != 0)) return -1;
    if (archive->year != 0) {
        if (prepare_directory(archive, "index") != 0 ||
            open_manifest(archive, "selection.jsonl", &archive->selection_fd) != 0)
            return -1;
    }
    if (archive->skip_unavailable_files &&
        open_manifest(archive, "unavailable-files.jsonl", &archive->unavailable_fd) != 0)
        return -1;
    if (archive->accept_size_mismatch &&
        open_manifest(archive, "size-mismatches.jsonl", &archive->mismatch_fd) != 0)
        return -1;
    if (open_manifest(archive, "missing-resources.jsonl", &archive->missing_fd) != 0)
        return -1;
    if (write_summary(archive, 0, 1) != 0) return -1;
    result = read_metadata(archive, archive->system, META_SYSTEM, &system, &bytes);
    if (result == RIS_HTTP_NOT_FOUND)
        return finish_manifest(archive, "selection.jsonl", &archive->selection_fd);
    if (result != 0) return -1;
    result = json_string(&system, json_get(&system, 0, "body"), bodies, sizeof(bodies));
    json_free(&system);
    rt_free(bytes);
    if (result != 0 || add_job(archive, bodies, JOB_BODIES) != 0) return -1;
    for (i = 0; i < archive->jobs.count; i++) {
        Job job = *(const Job *)tool_array_get_const(&archive->jobs, i);
        if (job.kind == JOB_RESOURCE) {
            if (finish_manifest(archive, "selection.jsonl", &archive->selection_fd) != 0 ||
                resource(archive, &job) != 0) return -1;
        } else if (collection(archive, &job) != 0) return -1;
    }
    return finish_manifest(archive, "selection.jsonl", &archive->selection_fd);
}

static int write_summary(Archive *archive, int success, int force) {
    char path[PATH_CAPACITY], part[PATH_CAPACITY];
    unsigned long long now = platform_get_monotonic_time_ns();
    int fd, result;
    if (!force && archive->summary_written_ns != 0 && now >= archive->summary_written_ns &&
        now - archive->summary_written_ns < SUMMARY_INTERVAL_NS) return 0;
    if (rt_join_path(archive->output, "archive.json", path, sizeof(path)) != 0 ||
        temporary_path(path, part) != 0) return ris_error("summary path is too long", 0);
    fd = platform_open_write(part, 0644);
    if (fd < 0) return ris_error("cannot write archive summary", path);
    result = rt_write_cstr(fd, "{\"system\":") ||
             tool_json_write_string(fd, archive->system) ||
             tool_json_field_bool(fd, "complete", success && !archive->limited &&
                                  archive->unavailable_files == 0 &&
                                  archive->missing_resources == 0) ||
             tool_json_field_bool(fd, "metadataOnly", archive->metadata_only) ||
             tool_json_field_bool(fd, "skipUnavailableFiles", archive->skip_unavailable_files) ||
             tool_json_field_bool(fd, "acceptSizeMismatch", archive->accept_size_mismatch) ||
             tool_json_field_bool(fd, "limited", archive->limited) ||
             tool_json_field_uint(fd, "referenceYear", archive->year) ||
             tool_json_field_uint(fd, "scannedPapers", archive->scanned_papers) ||
             tool_json_field_uint(fd, "selectedPapers", archive->selected_papers) ||
             tool_json_field_uint(fd, "processedResources", archive->processed_resources) ||
             tool_json_field_uint(fd, "newMetadataPages", archive->downloaded_pages) ||
             tool_json_field_uint(fd, "newDocuments", archive->downloaded_files) ||
             tool_json_field_uint(fd, "cachedDocuments", archive->cached_files) ||
             tool_json_field_uint(fd, "unavailableDocuments", archive->unavailable_files) ||
             tool_json_field_uint(fd, "missingResources", archive->missing_resources) ||
             tool_json_field_uint(fd, "sizeMismatchedDocuments", archive->mismatched_files) ||
             rt_write_cstr(fd, "}\n");
    if (platform_close(fd) != 0) result = -1;
    if (result != 0 || platform_rename_path(part, path) != 0)
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
    if (result == 0 &&
        finish_manifest(&archive, "unavailable-files.jsonl", &archive.unavailable_fd) != 0)
        result = -1;
    if (result == 0 &&
        finish_manifest(&archive, "size-mismatches.jsonl", &archive.mismatch_fd) != 0)
        result = -1;
    if (write_summary(&archive, result == 0, 1) != 0) result = -1;
    if (result == 0) {
        tool_write_styled(2, tool_get_global_color_mode(),
                          archive.missing_resources || archive.unavailable_files || archive.limited ?
                          TOOL_STYLE_BOLD_YELLOW : TOOL_STYLE_PLAIN,
                          archive.missing_resources ? "INCOMPLETE (HTTP 404)" :
                          archive.unavailable_files ? "INCOMPLETE (unavailable documents)" :
                          archive.limited ? "INCOMPLETE (configured limits)" : "Complete");
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
    rt_free(archive.seen_missing.entries);
    return result != 0 ? 1 :
           archive.missing_resources != 0 || archive.unavailable_files != 0 ? 3 :
           archive.limited ? 2 : 0;
}

#include "archive-store.h"
#include "archive-version.h"
#include "crypto/sha256.h"
#include "hash_util.h"
#include "runtime.h"

#define DEFAULT_SYSTEM "https://www.potsdam.sitzung-online.de/oparl/system"

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
               tool_json_string(json, archive_resource_type(json), type, sizeof(type)) == 0 &&
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
    archive_hash_text(url, hash);
    rt_copy_string(name, sizeof(name), hash);
    memcpy(name + 64, ".json", 6);
    if (archive_path(archive, kind == META_INDEX ? "index" : "metadata", name, path) != 0 ||
        archive_temporary_path(path, part) != 0) return -1;
    cached = archive_load_cache(archive, "metadata", url, url, path, META_LIMIT, &cache);
    if (cached < 0) return -1;
    if (cached && archive_preserve_revision(archive, "metadata", url, path, &cache) != 0) return -1;
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
    if (archive_preserve_revision(archive, "metadata", url, cached ? path : part, &cache) != 0) {
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
    if (archive_save_cache(archive, "metadata", url, url, &cache) != 0 ||
        archive_observation(archive, "metadata", url, url, &cache, cached) != 0) {
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
    } else archive_hash_text(url, hash);
    if (archive_hash_seen(&archive->seen_jobs, hash, 0)) return 0;
    job = (Job *)tool_array_append(&archive->jobs);
    if (job == 0) return ris_error("cannot allocate collection queue", 0);
    rt_copy_string(job->url, sizeof(job->url), url);
    job->kind = kind;
    job->calendar = calendar; job->type = type;
    archive->types[type].scheduled++;
    if (kind != JOB_RESOURCE) archive->types[type].collections++;
    return archive_hash_seen(&archive->seen_jobs, hash, 1) < 0 ? -1 : 0;
}
static int add_job(Archive *archive, const char *url, JobKind kind) {
    return add_scoped_job(archive, url, kind, 0, kind == JOB_BODIES ? TYPE_BODY :
                          kind == JOB_PAPERS ? TYPE_PAPER : archive_url_type(url));
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
        if (archive_record_missing(archive, url, "document") != 0) return -1;
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
        if (archive_content_digest(download->part, download->cache.digest) != 0 ||
            archive_preserve_revision(archive, "documents", id, download->part, &download->cache) != 0) return -1;
        if (platform_rename_path(download->part, download->path) != 0)
            return ris_error("cannot publish document", download->path);
        download->cache.framed = download->transfer.framed;
        download->cache.local_modified = 0;
    }
    download->cache.size = received;
    download->cache.validators = download->transfer.validators;
    download->cache.observed = (unsigned long long)platform_get_epoch_time();
    if (result == 0) download->cache.fetched = download->cache.observed;
    if (archive_save_cache(archive, "document", id, url, &download->cache) != 0 ||
        archive_observation(archive, "document", id, url, &download->cache, result == RIS_HTTP_NOT_MODIFIED) != 0) return -1;
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
    if (result == 0) result = archive_write_summary(archive, 0, 0);
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
        archive_record_missing(archive, metadata->url, "metadata") != 0) return -1;
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
    archive_hash_text(id, hash);
    if (archive_hash_seen(&archive->seen_files, hash, 0)) return 0;
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
    cached = archive_load_cache(archive, "document", id, url, path, FILE_LIMIT, &cache);
    if (cached < 0) return -1;
    if (!cached && !archive->refresh) {
        cached = archive_load_legacy_document(archive, id, path, &cache);
        if (cached < 0) return -1;
        legacy = cached;
    }
    if (archive_temporary_path(path, part) != 0) return -1;
    if (cached && archive_preserve_revision(archive, "documents", id, path, &cache) != 0) return -1;
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
    if (archive_hash_seen(&archive->seen_files, hash, 1) < 0) {
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
    archive_hash_text(id, hash);
    if (archive_hash_seen(&archive->seen_jobs, hash, 0)) return 0;
    if (add_job(archive, id, JOB_RESOURCE) != 0) return -1;
    archive->selected_papers++;
    if (archive_hash_seen(&archive->selected_roots, hash, 0)) return 0;
    if (rt_write_cstr(fd, "{\"id\":") != 0 || tool_json_write_string(fd, id) != 0 ||
        tool_json_field_string(fd, "reference", reference) != 0 ||
        rt_write_cstr(fd, "}\n") != 0)
        return ris_error("cannot write selected paper index", reference);
    return archive_hash_seen(&archive->selected_roots, hash, 1) < 0 ? -1 : 0;
}

static int select_meeting(Archive *archive, const ToolJson *json, size_t object, const char *source) {
    char id[RIS_URL_CAPACITY];
    int member;
    if (!tool_json_equals(json, tool_json_get(json, object, "type"), "https://schema.oparl.org/1.1/Meeting"))
        return ris_error("non-Meeting object in calendar", source);
    if (tool_json_true(json, tool_json_get(json, object, "deleted")))
        return archive_store_collection_object(archive, json, object, source);
    member = archive_calendar_member(archive, json, object);
    if (member < 0) return -1;
    if (!member && !(archive->incremental &&
                    archive_known_reference(&archive->known_resources, json, object))) return 0;
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0)
        return ris_error("Meeting has no ID", source);
    return add_scoped_job(archive, id, JOB_RESOURCE,
        !member && archive->incremental && !archive->calendar_only &&
        !archive_known_reference(&archive->known_calendar_meetings, json, object) ? 0 : 1, TYPE_MEETING);
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
        if (calendar && archive_private_item(json, index)) return 0;
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
        archive_hash_text(url, hash);
        if (archive_hash_seen(&archive->known_resources, hash, 0)) return 0;
    }
    return add_scoped_job(archive, url, JOB_RESOURCE, calendar, archive_url_type(url));
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
    if (job->calendar && archive_private_item(&json, 0)) goto done;
    type = archive_resource_type(&json);
    if (job->calendar && tool_json_equals(&json, type, "https://schema.oparl.org/1.1/Meeting")) {
        int member = archive_calendar_member(archive, &json, 0);
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
        result = archive_write_summary(archive, 0, 0);
    }
    return result;
}

static int process_changes(Archive *archive, const ToolJson *json, size_t object,
                           unsigned int type, const char *source) {
    char id[RIS_URL_CAPACITY], expected_type[128];
    const char *prefix = "https://schema.oparl.org/1.1/";
    size_t declared = tool_json_get(json, object, "type");
    int scoped = archive->year || archive->calendar_only;
    int known = archive_known_reference(&archive->known_resources, json, object);
    int deleted = tool_json_true(json, tool_json_get(json, object, "deleted"));
    if (type == TYPE_ORGANIZATION && (tool_json_equals(json, declared, "at") || tool_json_equals(json, declared, "gr")))
        declared = tool_json_get(json, object, "Type");
    rt_copy_string(expected_type, sizeof(expected_type), prefix);
    rt_copy_string(expected_type + rt_strlen(prefix), sizeof(expected_type) - rt_strlen(prefix), archive_type_name(type));
    if (!tool_json_equals(json, declared, expected_type))
        return ris_error("unexpected object type in modification list", source);
    if (tool_json_string(json, tool_json_get(json, object, "id"), id, sizeof(id)) != 0 ||
        !http_valid_url(&archive->http, id))
        return ris_error("changed object has no valid ID", source);
    if (type == TYPE_FILE) {
        if (scoped && !archive_known_reference(&archive->known_files, json, object)) return 0;
        if (archive_store_collection_object(archive, json, object, source) != 0) return -1;
        return deleted ? 0 : document(archive, json, object);
    }
    if (deleted) {
        return !scoped || known ? archive_store_collection_object(archive, json, object, source) : 0;
    }
    if (type == TYPE_PAPER && archive->year && !archive->calendar_only) {
        if (known) {
            archive->scanned_papers++;
            if (archive_known_reference(&archive->selected_roots, json, object)) archive->selected_papers++;
            return add_scoped_job(archive, id, JOB_RESOURCE, 0, TYPE_PAPER);
        }
        return select_paper(archive, json, object);
    }
    if (type == TYPE_MEETING) return select_meeting(archive, json, object, source);
    if (scoped && !known && type == TYPE_AGENDA && !archive_private_item(json, object) &&
        archive_known_reference(&archive->known_calendar_meetings, json, tool_json_get(json, object, "meeting")))
        return add_scoped_job(archive, id, JOB_RESOURCE, 1, type);
    if (scoped && !known && type == TYPE_AGENDA && !archive->calendar_only && !archive_private_item(json, object) &&
        archive_known_reference(&archive->paper_context, json, tool_json_get(json, object, "consultation")))
        return add_scoped_job(archive, id, JOB_RESOURCE, 0, type);
    if (scoped && !known) return 0;
    return add_scoped_job(archive, id, JOB_RESOURCE,
        archive->calendar_only || (scoped && (type == TYPE_AGENDA || type == TYPE_CONSULTATION) &&
        !archive_known_reference(&archive->paper_context, json, object) &&
        !archive_known_reference(&archive->paper_context, json, tool_json_get(json, object, "consultation"))), type);
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
                if (archive_store_collection_object(archive, json, index, source) != 0) return -1;
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
        if (result == 0) result = archive_write_summary(archive, 0, 0);
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
                        !http_valid_url(&archive->http, id) || archive_url_type(id) != TYPE_FILE))
            result = -1;
        tool_json_free(&json);
        if (result) break;
        archive_hash_text(id, hash);
        if (!archive_hash_seen(&archive->known_files, hash, 0)) { result = -1; break; }
        count++;
        if (!archive_hash_seen(&archive->seen_files, hash, 0) && add_job(archive, id, JOB_RESOURCE) != 0) {
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
    if (archive_prepare_sync(archive) != 0) return -1;
    if (archive_prepare_directory(archive, "metadata") != 0 ||
        archive_prepare_directory(archive, "cache") != 0 ||
        archive_prepare_directory(archive, "index") != 0 ||
        archive_prepare_directory(archive, "imports") != 0 ||
        archive_prepare_directory(archive, "observations") != 0 ||
        (!archive->metadata_only && archive_prepare_directory(archive, "documents") != 0)) return -1;
    {
        char name[128];
        size_t length = rt_strlen(archive->run_id);
        memcpy(name, "observations/", 13); memcpy(name + 13, archive->run_id, length);
        memcpy(name + 13 + length, ".jsonl", 7);
        if (archive_open_manifest(archive, name, &archive->observations_fd) != 0) return -1;
    }
    if (archive->year != 0 && !archive->calendar_only) {
        if (archive_prepare_directory(archive, "index") != 0 ||
            archive_open_manifest(archive, "selection.jsonl", &archive->selection_fd) != 0)
            return -1;
        if (archive_retain_selection(archive) != 0) return -1;
    }
    if (archive->skip_unavailable_files && !archive->metadata_only &&
        archive_open_manifest(archive, "unavailable-files.jsonl", &archive->unavailable_fd) != 0)
        return -1;
    if (archive->accept_size_mismatch &&
        archive_open_manifest(archive, "size-mismatches.jsonl", &archive->mismatch_fd) != 0)
        return -1;
    if (archive_open_manifest(archive, "missing-resources.jsonl", &archive->missing_fd) != 0)
        return -1;
    if (archive_write_summary(archive, 0, 1) != 0) return -1;
    archive->active_type = TYPE_SYSTEM; archive->types[TYPE_SYSTEM].scheduled++;
    result = read_metadata(archive, archive->system, META_SYSTEM, &system, &bytes);
    if (result == RIS_HTTP_NOT_FOUND)
        return archive_finish_manifest(archive, "selection.jsonl", &archive->selection_fd);
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
            if (archive_finish_manifest(archive, "selection.jsonl", &archive->selection_fd) != 0 ||
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
    return archive_finish_manifest(archive, "selection.jsonl", &archive->selection_fd);
}

int main(int argc, char **argv) {
    Archive archive;
    int parsed = options(&archive, argc, argv), result;
    if (parsed != 0) {
        tool_array_free(&archive.jobs);
        return parsed > 0 ? 0 : 1;
    }
    if (archive_validate(&archive) != 0) {
        tool_array_free(&archive.jobs);
        return 1;
    }
    result = run(&archive);
    if (result == 0 && flush_documents(&archive) != 0) result = -1;
    if (result == 0 &&
        archive_finish_manifest(&archive, "missing-resources.jsonl", &archive.missing_fd) != 0)
        result = -1;
    {
        char name[128];
        size_t length = rt_strlen(archive.run_id);
        memcpy(name, "observations/", 13); memcpy(name + 13, archive.run_id, length);
        memcpy(name + 13 + length, ".jsonl", 7);
        if (archive_finish_manifest(&archive, name, &archive.observations_fd) != 0) result = -1;
    }
    archive.finalizing = 1; archive.finished = (unsigned long long)platform_get_epoch_time();
    if (result == 0 && archive.incremental && !archive.metadata_only &&
        archive.previous_unavailable && archive.missing_resources)
        result = ris_error("document retries incomplete after HTTP 404; previous unavailable report retained",
                           archive.output);
    if (result == 0 &&
        archive_finish_manifest(&archive, "unavailable-files.jsonl", &archive.unavailable_fd) != 0)
        result = -1;
    if (result == 0 &&
        archive_finish_manifest(&archive, "size-mismatches.jsonl", &archive.mismatch_fd) != 0)
        result = -1;
    if (archive_write_summary(&archive, result == 0, 1) != 0) result = -1;
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

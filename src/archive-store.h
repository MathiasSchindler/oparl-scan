#ifndef OPARL_ARCHIVE_STORE_H
#define OPARL_ARCHIVE_STORE_H

#include "http.h"
#include "tool_json_parser.h"
#include "tool_util.h"

#define META_LIMIT (32ULL * 1024ULL * 1024ULL)
#define FILE_LIMIT (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define SUMMARY_INTERVAL_NS 500000000ULL
#define PATH_CAPACITY 1024U
#define PENDING_DOCUMENTS 32U
#define SYNC_OVERLAP_SECONDS 300ULL

typedef enum {
    JOB_COLLECTION, JOB_BODIES, JOB_PAPERS, JOB_RESOURCE, JOB_CALENDAR, JOB_CHANGES
} JobKind;
enum { TYPE_SYSTEM, TYPE_BODY, TYPE_PAPER, TYPE_MEETING, TYPE_AGENDA, TYPE_CONSULTATION,
       TYPE_FILE, TYPE_ORGANIZATION, TYPE_PERSON, TYPE_MEMBERSHIP, TYPE_LOCATION,
       TYPE_TERM, TYPE_RESOURCE, TYPE_COUNT };

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

const char *archive_type_name(unsigned int type);
unsigned int archive_url_type(const char *url);
size_t archive_resource_type(const ToolJson *json);
int archive_private_item(const ToolJson *json, size_t object);
int archive_calendar_member(const Archive *archive, const ToolJson *json, size_t object);
void archive_hash_text(const char *text, char out[65]);
int archive_hash_seen(SeenSet *set, const char *hash, int insert);
int archive_known_reference(SeenSet *set, const ToolJson *json, size_t index);
int archive_path(const Archive *archive, const char *directory, const char *name, char *out);
int archive_prepare_directory(const Archive *archive, const char *name);
int archive_temporary_path(const char *path, char out[PATH_CAPACITY]);
int archive_content_digest(const char *path, char out[65]);
int archive_preserve_revision(Archive *archive, const char *kind, const char *key,
                              const char *path, const CacheEntry *cache);
int archive_observation(Archive *archive, const char *kind, const char *key,
                        const char *url, const CacheEntry *cache, int reused);
int archive_load_cache(const Archive *archive, const char *kind, const char *key,
                       const char *url, char *content, unsigned long long limit,
                       CacheEntry *entry);
int archive_load_legacy_document(const Archive *archive, const char *id,
                                 const char *content, CacheEntry *entry);
int archive_save_cache(const Archive *archive, const char *kind, const char *key,
                       const char *url, const CacheEntry *entry);
int archive_record_missing(Archive *archive, const char *url, const char *kind);
int archive_store_collection_object(Archive *archive, const ToolJson *json, size_t object, const char *source);
int archive_open_manifest(const Archive *archive, const char *name, int *fd);
int archive_finish_manifest(const Archive *archive, const char *name, int *fd);
int archive_retain_selection(Archive *archive);
int archive_validate(Archive *archive);
int archive_prepare_sync(Archive *archive);
int archive_write_summary(Archive *archive, int success, int force);

#endif

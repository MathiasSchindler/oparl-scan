#include "search.h"
#include "archive-walk.h"
#include "hash_util.h"
#include "runtime.h"

typedef struct {
    const char *hash;
    uint64_t bytes;
    int found;
} PublishedPdf;

typedef struct {
    PublishedPdf *pdfs;
    size_t count, archives, copied, reused;
    uint64_t bytes;
    const char *root;
} Publication;

static int pdf_path(const char *root, const char *hash, char path[4096]) {
    char directory[4096], name[69];
    size_t i;
    if (rt_strlen(hash) != 64) return -1;
    for (i = 0; i < 64; i++)
        if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f'))) return -1;
    memcpy(name, hash, 64); memcpy(name + 64, ".pdf", 5);
    return rt_join_path(root, "pdf", directory, sizeof(directory)) ||
           rt_join_path(directory, name, path, 4096) ? -1 : 0;
}

static int pdf_directory(const char *root) {
    char path[4096];
    PlatformDirEntry info;
    return platform_get_path_info(root, &info) || (info.mode & 0170000U) != 0040000U ||
        rt_join_path(root, "pdf", path, sizeof(path)) || platform_get_path_info(path, &info) ||
        (info.mode & 0170000U) != 0040000U ? -1 : 0;
}

static int verified_open(const char *path, const char *expected, uint64_t bytes) {
    unsigned char digest[32];
    char hash[65];
    PlatformDirEntry info;
    int fd = platform_open_read_secure(path, &info);
    if (fd < 0) return -1;
    if ((info.mode & 0170000U) != 0100000U || info.size != bytes ||
        hash_sha256_stream(fd, digest) || platform_seek(fd, 0, 0) != 0) {
        platform_close(fd); return -2;
    }
    hash_to_hex(digest, sizeof(digest), hash);
    if (rt_strcmp(hash, expected)) { platform_close(fd); return -2; }
    return fd;
}

int search_pdf_open(const SearchIndex *index, const char *root, const char *hash,
                    uint64_t *bytes, const char **error) {
    char path[4096];
    size_t i;
    int fd;
    *error = "local PDF copy is not available for this indexed version";
    if (!root || pdf_path(root, hash, path) || pdf_directory(root)) return -1;
    for (i = 0; i < index->version_count; i++) {
        const SearchVersion *version = &index->versions[i];
        if (rt_strcmp(index->strings + version->hash, hash)) continue;
        if (version->bytes > INT64_MAX) {
            *error = "indexed PDF size exceeds the supported HTTP size"; return -2;
        }
        fd = verified_open(path, hash, version->bytes);
        if (fd == -2) *error = "local PDF copy differs from the indexed SHA-256 or size";
        if (fd >= 0) *bytes = version->bytes;
        return fd;
    }
    return -1;
}

static int compare_pdf(const void *a, const void *b) {
    return rt_strcmp(((const PublishedPdf *)a)->hash, ((const PublishedPdf *)b)->hash);
}

static PublishedPdf *find_pdf(Publication *p, const char *hash) {
    size_t low = 0, high = p->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int order = rt_strcmp(p->pdfs[mid].hash, hash);
        if (order < 0) low = mid + 1;
        else high = mid;
    }
    return low < p->count && !rt_strcmp(p->pdfs[low].hash, hash) ? &p->pdfs[low] : 0;
}

static int copy_pdf(const char *source, const char *destination, const PublishedPdf *pdf) {
    char part[4102], hash[65];
    unsigned char buffer[65536], digest[32];
    CryptoSha256Context sha;
    PlatformDirEntry info;
    uint64_t bytes = 0;
    int input, output = -1, result = -1, created = 0;
    if (tool_path_append_suffix(destination, ".part", part, sizeof(part)))
        return search_error("PDF publication staging path is too long", destination);
    input = platform_open_read_secure(source, &info);
    if (input < 0) return search_error("cannot open PDF publication source", source);
    if ((info.mode & 0170000U) != 0100000U || info.size != pdf->bytes) goto done;
    output = platform_open_create_exclusive(part, 0644);
    if (output < 0) goto done;
    created = 1;
    crypto_sha256_init(&sha);
    for (;;) {
        long got = platform_read(input, buffer, sizeof(buffer));
        if (got < 0) goto done;
        if (!got) break;
        bytes += (uint64_t)got;
        if (bytes > pdf->bytes || rt_write_all(output, buffer, (size_t)got)) goto done;
        crypto_sha256_update(&sha, buffer, (size_t)got);
    }
    crypto_sha256_final(&sha, digest);
    hash_to_hex(digest, sizeof(digest), hash);
    if (bytes == pdf->bytes && !rt_strcmp(hash, pdf->hash)) result = 0;
done:
    if (platform_close(input)) result = -1;
    if (output >= 0 && platform_close(output)) result = -1;
    if (!result && platform_rename_path(part, destination)) result = -1;
    if (result && created && platform_remove_file(part))
        search_error("cannot remove failed PDF publication staging file", part);
    return result ? search_error("cannot publish verified PDF; check source and staging file", source) : 0;
}

static int publish_document(const char *path, const PlatformDirEntry *info, int depth,
                            ToolWalkControl *control, void *user) {
    Publication *p = (Publication *)user;
    PublishedPdf *pdf;
    PlatformDirEntry current;
    unsigned char digest[32];
    char hash[65], destination[4096];
    size_t length = rt_strlen(path);
    int fd, result;
    (void)depth; (void)control;
    if (info->is_dir || (length >= 5 && !rt_strcmp(path + length - 5, ".part"))) return 0;
    fd = platform_open_read_secure(path, &current);
    if (fd < 0) return search_error("cannot open archive PDF for publication", path);
    result = (current.mode & 0170000U) != 0100000U || hash_sha256_stream(fd, digest);
    if (platform_close(fd)) result = -1;
    if (result) return search_error("cannot hash archive PDF for publication", path);
    hash_to_hex(digest, sizeof(digest), hash);
    pdf = find_pdf(p, hash);
    if (!pdf || pdf->found) return 0;
    if (current.size != pdf->bytes || pdf_path(p->root, hash, destination))
        return search_error("archive PDF does not match its indexed size or publication path", path);
    if (!platform_get_path_info(destination, &current)) {
        fd = verified_open(destination, hash, pdf->bytes);
        if (fd < 0) return search_error("existing published PDF is not the indexed version", destination);
        if (platform_close(fd)) return search_error("cannot close published PDF", destination);
        p->reused++;
    } else {
        if (copy_pdf(path, destination, pdf)) return -1;
        p->copied++; p->bytes += pdf->bytes;
    }
    pdf->found = 1;
    if ((p->copied + p->reused) % 500 == 0) {
        if (rt_write_cstr(2, "Published PDF versions: ") ||
            rt_write_uint(2, p->copied + p->reused) || rt_write_char(2, '\n'))
            return search_error("cannot write PDF publication progress", 0);
    }
    return 0;
}

static int publish_archive(const char *path, const PlatformDirEntry *info, int depth,
                           ToolWalkControl *control, void *user) {
    Publication *p = (Publication *)user;
    PlatformDirEntry entry;
    char manifest[4096], documents[4096];
    const char *directories[] = {"documents", "history/documents"};
    size_t i;
    if (!info->is_dir) return 0;
    if (rt_join_path(path, "archive.json", manifest, sizeof(manifest)))
        return search_error("archive publication path is too long", path);
    if (platform_get_path_info(manifest, &entry)) { control->prune = depth > 0; return 0; }
    control->prune = 1; p->archives++;
    for (i = 0; i < 2; i++) {
        if (rt_join_path(path, directories[i], documents, sizeof(documents)))
            return search_error("archive document path is too long", path);
        if (platform_get_path_info(documents, &entry)) continue;
        if (!entry.is_dir || archive_walk(documents, publish_document, p, 0))
            return search_error("cannot read archive documents for publication", documents);
    }
    return 0;
}

int search_publish_pdfs(const SearchIndex *index, const char *input, const char *root) {
    Publication p;
    char path[4096], scratch[4096];
    size_t i, missing = 0;
    int result = -1;
    rt_memset(&p, 0, sizeof(p)); p.root = root;
    if (!index->version_count) return search_error("index has no immutable PDF versions to publish", 0);
    p.pdfs = (PublishedPdf *)rt_malloc_array(index->version_count, sizeof(*p.pdfs));
    if (!p.pdfs) return search_error("cannot allocate PDF publication inventory", 0);
    for (i = 0; i < index->version_count; i++) {
        p.pdfs[i].hash = index->strings + index->versions[i].hash;
        p.pdfs[i].bytes = index->versions[i].bytes; p.pdfs[i].found = 0;
    }
    rt_sort(p.pdfs, index->version_count, sizeof(*p.pdfs), compare_pdf);
    for (i = 0; i < index->version_count; i++) {
        if (p.count && !rt_strcmp(p.pdfs[p.count - 1].hash, p.pdfs[i].hash)) {
            if (p.pdfs[p.count - 1].bytes != p.pdfs[i].bytes) {
                search_error("indexed versions disagree about the size of the same SHA-256", p.pdfs[i].hash);
                goto done;
            }
        } else p.pdfs[p.count++] = p.pdfs[i];
    }
    if (pdf_path(root, p.pdfs[0].hash, path) ||
        tool_ensure_parent_dirs(path, scratch, sizeof(scratch)) || pdf_directory(root)) {
        search_error("cannot create a regular PDF publication directory", root); goto done;
    }
    for (i = 0; i < p.count; i++) {
        PlatformDirEntry info;
        int fd;
        if (pdf_path(root, p.pdfs[i].hash, path)) {
            search_error("invalid indexed PDF hash for publication", p.pdfs[i].hash); goto done;
        }
        if (platform_get_path_info(path, &info)) { missing++; continue; }
        fd = verified_open(path, p.pdfs[i].hash, p.pdfs[i].bytes);
        if (fd < 0) { search_error("existing published PDF is not the indexed version", path); goto done; }
        if (platform_close(fd)) { search_error("cannot close existing published PDF", path); goto done; }
        p.pdfs[i].found = 1; p.reused++;
    }
    if (missing) {
        if (archive_walk(input, publish_archive, &p, 0)) goto done;
        if (!p.archives) { search_error("no OParl archives found for PDF publication", input); goto done; }
    }
    missing = 0;
    for (i = 0; i < p.count; i++) if (!p.pdfs[i].found) {
        missing++; search_error("indexed PDF version is missing from the source archives", p.pdfs[i].hash);
    }
    if (rt_write_cstr(1, "{\"pdfVersions\":") || rt_write_uint(1, p.count) ||
        tool_json_field_uint(1, "copied", p.copied) || tool_json_field_uint(1, "reused", p.reused) ||
        tool_json_field_uint(1, "missing", missing) || tool_json_field_uint(1, "bytesCopied", p.bytes) ||
        rt_write_cstr(1, "}\n")) {
        search_error("cannot write PDF publication summary", 0); goto done;
    }
    result = missing ? -1 : 0;
done:
    rt_free(p.pdfs);
    return result;
}

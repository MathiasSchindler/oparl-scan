#include "eg2.h"
#include "tool_json_parser.h"
#include "index_io.h"
#include "hash_util.h"
#include "crypto/sha256.h"
#include "math/math.h"
#include <stdint.h>

#define PREFIX EG2_DOCUMENT_PREFIX

typedef EgIndexFile Source;

typedef struct {
    const char *argument;
    char path[4096], metadata[4096], archive_file[4096], prefix[4];
    unsigned char sha[32];
    unsigned int year;
    int complete;
    size_t selected, deleted, other;
    ToolArray sources;
} Archive;

typedef struct {
    char *id, *title, *reference;
    size_t archive, source;
    unsigned int tokens;
    unsigned char key[32];
    char vector[80];
    int deleted;
} Paper;

typedef struct {
    EgModel model;
    const char *directory, *output;
    unsigned int threads, count, token_count;
    unsigned int counts[EG2_MAX_BATCH], tokens[EG2_MAX_TOKENS];
    Paper *papers[EG2_MAX_BATCH];
    size_t selected, computed, reused, batches;
    unsigned int max_documents, max_tokens;
    unsigned long long load_ns;
    int loaded;
} TitleRun;

static int index_progress(const TitleRun *run) {
    size_t completed = run->computed + run->reused;
    if (completed == 1 || completed % 25 == 0 || completed == run->selected) {
        if (rt_write_cstr(2, "indexed: ") || rt_write_uint(2, completed) ||
            rt_write_char(2, '/') || rt_write_uint(2, run->selected) || rt_write_char(2, '\n'))
            return eg_error("cannot write index progress", 0);
    }
    return 0;
}

static int flush_titles(TitleRun *run) {
    if (!run->count) return 0;
    char path[4096];
    if (!run->loaded) {
        if (rt_join_path(run->directory, "weights.eg2", path, sizeof(path)))
            return eg_error("model path exceeds supported length", 0);
        unsigned long long start = platform_get_monotonic_time_ns();
        if (eg_model_load(&run->model, path) || eg_model_set_threads(&run->model, run->threads))
            return -1;
        run->load_ns = platform_get_monotonic_time_ns() - start;
        run->loaded = 1;
    }
    if (rt_write_cstr(2, "embedding batch: ") || rt_write_uint(2, run->count) ||
        rt_write_cstr(2, " titles, ") || rt_write_uint(2, run->token_count) ||
        rt_write_cstr(2, " tokens\n"))
        return eg_error("cannot write batch progress", 0);
    float vectors[EG2_MAX_BATCH * EG2_DIM];
    if (eg_embed_batch_profiled(&run->model, run->tokens, run->counts, run->count, vectors, 0))
        return -1;
    run->batches++;
    if (run->count > run->max_documents) run->max_documents = run->count;
    if (run->token_count > run->max_tokens) run->max_tokens = run->token_count;
    for (unsigned int i = 0; i < run->count; i++) {
        const Paper *p = run->papers[i];
        if (rt_join_path(run->output, p->vector, path, sizeof(path)))
            return eg_error("vector path exceeds supported length: ", p->id);
        if (eg_vector_write(path, p->key, vectors + i * EG2_DIM)) return -1;
        run->computed++;
        if (index_progress(run)) return -1;
    }
    run->count = run->token_count = 0;
    return 0;
}

static int read_source(const char *path, unsigned char **bytes, size_t *size) {
    PlatformDirEntry info;
    if (platform_get_path_info(path, &info) || (info.mode & 0170000U) != 0100000U ||
        info.size > 16U * 1024U * 1024U || tool_read_all_input(path, bytes, size))
        return eg_error("cannot read regular source file (limit 16 MiB): ", path);
    if (*size != info.size) {
        rt_free(*bytes);
        *bytes = 0;
        return eg_error("source changed while reading: ", path);
    }
    return 0;
}

static int absolute_path(const char *input, int allow_missing, char *out, size_t capacity) {
    return eg_absolute_path(input, allow_missing, out, capacity);
}

static int list_sources(const char *directory, ToolArray *sources) {
    return eg_index_files(directory, ".json", sources);
}

static char *string_field(const ToolJson *json, const char *key, size_t limit) {
    return tool_json_dup_string(json, tool_json_get(json, 0, key), limit);
}

static int read_paper(const ToolJson *json, size_t source, ToolArray *papers) {
    if (!tool_json_equals_alloc(json, tool_json_get(json, 0, "type"), "https://schema.oparl.org/1.1/Paper"))
        return 0;
    Paper *p = tool_array_append(papers);
    if (!p) return eg_error("cannot allocate paper", 0);
    rt_memset(p, 0, sizeof(*p));
    p->source = source;
    p->id = string_field(json, "id", 4096);
    p->reference = string_field(json, "reference", 512);
    const ToolJsonToken *deleted = tool_json_token(json, tool_json_get(json, 0, "deleted"));
    if (deleted && deleted->kind != TOOL_JSON_BOOL)
        return eg_error("Paper.deleted must be boolean", p->id);
    p->deleted = tool_json_true(json, tool_json_get(json, 0, "deleted"));
    if (!p->id || !p->id[0] || (!p->deleted && !p->reference))
        return eg_error("Paper requires an id and, unless deleted, a reference", 0);
    return 0;
}

static int compare_paper(const void *a, const void *b) {
    return rt_strcmp(((const Paper *)a)->id, ((const Paper *)b)->id);
}

static int compare_archive_paper(const void *a, const void *b) {
    size_t left = ((const Paper *)a)->archive, right = ((const Paper *)b)->archive;
    return left < right ? -1 : left > right ? 1 : compare_paper(a, b);
}

static int add_archive(ToolArray *archives, const char *argument, unsigned int year) {
    Archive *archive = tool_array_append(archives);
    if (!archive) return eg_error("cannot allocate archive configuration", 0);
    rt_memset(archive, 0, sizeof(*archive));
    archive->argument = argument;
    archive->year = year;
    tool_array_init(&archive->sources, sizeof(Source));
    return 0;
}

static int load_archive(Archive *archive, size_t index, ToolArray *papers) {
    unsigned char *bytes = 0;
    size_t size = 0;
    ToolJson json = {0};
    int result = -1;
    char path[4096];
    archive->prefix[0] = (char)('0' + (archive->year % 100) / 10);
    archive->prefix[1] = (char)('0' + archive->year % 10);
    archive->prefix[2] = '/';
    if (rt_join_path(archive->path, "archive.json", archive->archive_file, sizeof(archive->archive_file)) ||
        rt_join_path(archive->path, "metadata", archive->metadata, sizeof(archive->metadata))) {
        eg_error("archive path too long: ", archive->path); goto done;
    }
    if (read_source(archive->archive_file, &bytes, &size)) goto done;
    crypto_sha256_hash(bytes, size, archive->sha);
    if (tool_json_parse(&json, (const char *)bytes, size)) {
        eg_error("invalid archive.json: ", archive->archive_file); goto done;
    }
    const ToolJsonToken *flag = tool_json_token(&json, tool_json_get(&json, 0, "complete"));
    if (!flag || flag->kind != TOOL_JSON_BOOL) {
        eg_error("archive.json requires a boolean complete field: ", archive->archive_file); goto done;
    }
    archive->complete = tool_json_true(&json, tool_json_get(&json, 0, "complete"));
    tool_json_free(&json);
    rt_free(bytes);
    bytes = 0;
    if (!archive->complete &&
        (rt_write_cstr(2, "warning: source archive is incomplete; indexing local subset only: ") ||
         rt_write_line(2, archive->path))) {
        eg_error("cannot write archive warning", 0); goto done;
    }
    if (list_sources(archive->metadata, &archive->sources)) goto done;
    for (size_t i = 0; i < archive->sources.count; i++) {
        Source *source = tool_array_get(&archive->sources, i);
        if (rt_join_path(archive->metadata, source->name, path, sizeof(path))) {
            eg_error("metadata path too long: ", source->name); goto done;
        }
        if (read_source(path, &bytes, &size)) goto done;
        crypto_sha256_hash(bytes, size, source->sha);
        if (tool_json_parse(&json, (const char *)bytes, size)) {
            eg_error("invalid metadata JSON: ", path); goto done;
        }
        if (!tool_json_token(&json, 0) || tool_json_token(&json, 0)->kind != TOOL_JSON_OBJECT) {
            eg_error("expected a canonical metadata object: ", path); goto done;
        }
        size_t before = papers->count;
        if (read_paper(&json, i, papers)) goto done;
        if (papers->count != before) {
            Paper *p = tool_array_get(papers, before);
            p->archive = index;
            if (p->deleted) archive->deleted++;
            else if (!rt_strncmp(p->reference, archive->prefix, 3)) {
                p->title = string_field(&json, "name", EG2_MAX_TEXT - (sizeof(PREFIX) - 1));
                size_t position = 0, title_size = p->title ? rt_strlen(p->title) : 0;
                int content = 0;
                while (position < title_size) {
                    unsigned int codepoint;
                    if (rt_utf8_decode(p->title, title_size, &position, &codepoint)) break;
                    if (!rt_unicode_is_space(codepoint)) { content = 1; break; }
                }
                if (!content) {
                    eg_error("selected Paper requires a nonblank name within input limits: ", path);
                    goto done;
                }
                archive->selected++;
            } else archive->other++;
        }
        tool_json_free(&json);
        rt_free(bytes);
        bytes = 0;
    }
    result = 0;
done:
    tool_json_free(&json);
    rt_free(bytes);
    return result;
}

static int atomic_write(const char *path, const void *data, size_t size) {
    return eg_atomic_write(path, data, size);
}

static int field(ToolByteBuffer *buffer, const char *key, const char *text) {
    return tool_json_append_string(buffer, key) || tool_byte_buffer_append_char(buffer, ':') ||
           tool_json_append_string(buffer, text) || tool_byte_buffer_append_char(buffer, ',');
}

static int number(ToolByteBuffer *buffer, const char *key, unsigned long long value) {
    return tool_json_append_string(buffer, key) || tool_byte_buffer_append_char(buffer, ':') ||
           tool_byte_buffer_append_uint(buffer, value) || tool_byte_buffer_append_char(buffer, ',');
}

static int archive_fields(ToolByteBuffer *buffer, const Archive *archive) {
    return field(buffer, "reference_prefix", archive->prefix) ||
           field(buffer, "source_archive", archive->path) ||
           number(buffer, "year", archive->year) ||
           tool_byte_buffer_append_cstr(buffer, archive->complete ?
               "\"archive_complete\":true" : "\"archive_complete\":false");
}

static int verify_snapshot(const char *metadata, const ToolArray *sources) {
    ToolArray current;
    char path[4096];
    int result = -1;
    tool_array_init(&current, sizeof(Source));
    if (list_sources(metadata, &current)) goto done;
    if (sources->count != current.count) goto changed;
    for (size_t i = 0; i < sources->count; i++) {
        const Source *a = tool_array_get_const(sources, i), *b = tool_array_get_const(&current, i);
        unsigned char *bytes = 0, sha[32];
        size_t size = 0;
        if (rt_strcmp(a->name, b->name)) goto changed;
        if (rt_join_path(metadata, a->name, path, sizeof(path))) {
            eg_error("metadata path too long: ", a->name); goto done;
        }
        if (read_source(path, &bytes, &size)) goto done;
        crypto_sha256_hash(bytes, size, sha);
        rt_free(bytes);
        if (memcmp(a->sha, sha, 32)) goto changed;
    }
    result = 0;
    goto done;
changed:
    eg_error("archive changed during indexing; stop downloads and rerun", 0);
done:
    tool_array_free(&current);
    return result;
}

static int usage(void) {
    return rt_write_cstr(1,
        "Usage: embeddinggemma-index --archive DIR --output DIR [--year 2026]\n"
        "                           [--add-archive YEAR DIR] ...\n"
        "                           [--model models/native] [--threads 1..32]\n"
        "Indexes canonical metadata/*.json Paper.name with matching YY/ reference.\n"
        "--year selects the primary archive; each additional archive has its own year.\n"
        "Loads model once if needed; verifies and reuses cached vectors on rerun.\n"
        "Publishes manifest.json only after verifying the source snapshot.\n");
}

int main(int argc, char **argv) {
    const char *output = 0, *directory = "models/native";
    unsigned int threads = 1;
    char output_path[4096], cache[4096], path[4096];
    size_t selected = 0, deleted = 0, other = 0;
    int result = 1, complete = 1;
    unsigned long long started = platform_get_monotonic_time_ns();
    ToolArray archives, papers;
    ToolByteBuffer manifest, prepared;
    EgTokenizer tokenizer = {0};
    TitleRun run = {0};
    tool_array_init(&archives, sizeof(Archive));
    tool_array_init(&papers, sizeof(Paper));
    tool_byte_buffer_init(&manifest);
    tool_byte_buffer_init(&prepared);
    if (add_archive(&archives, 0, 2026)) goto done;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!rt_strcmp(arg, "--help")) { result = usage() ? 1 : 0; goto done; }
        if (!rt_strcmp(arg, "--add-archive")) {
            unsigned long long year;
            if (i + 2 >= argc || rt_parse_uint(argv[i + 1], &year) || year < 2000 || year > 2099) {
                eg_error("--add-archive requires YEAR (2000..2099) and DIR", 0); goto done;
            }
            if (add_archive(&archives, argv[i + 2], (unsigned int)year)) goto done;
            i += 2;
            continue;
        }
        if (rt_strcmp(arg, "--archive") && rt_strcmp(arg, "--output") &&
            rt_strcmp(arg, "--model") && rt_strcmp(arg, "--year") && rt_strcmp(arg, "--threads")) {
            eg_error("unknown index option: ", arg); goto done;
        }
        if (++i == argc) { eg_error("missing option value: ", arg); goto done; }
        if (!rt_strcmp(arg, "--archive")) ((Archive *)tool_array_get(&archives, 0))->argument = argv[i];
        else if (!rt_strcmp(arg, "--output")) output = argv[i];
        else if (!rt_strcmp(arg, "--model")) directory = argv[i];
        else {
            unsigned long long n;
            if (rt_parse_uint(argv[i], &n) ||
                (!rt_strcmp(arg, "--year") ? n < 2000 || n > 2099 : n < 1 || n > 32)) {
                eg_error("year must be 2000..2099; threads must be 1..32", 0); goto done;
            }
            if (!rt_strcmp(arg, "--year")) ((Archive *)tool_array_get(&archives, 0))->year = (unsigned int)n;
            else threads = (unsigned int)n;
        }
    }
    const Archive *primary = tool_array_get_const(&archives, 0);
    if (!primary->argument || !primary->argument[0] || !output || !output[0]) {
        eg_error("--archive and --output are required", 0); goto done;
    }
    if (absolute_path(output, 1, output_path, sizeof(output_path))) goto done;
    output = output_path;
    for (size_t i = 0; i < archives.count; i++) {
        Archive *archive = tool_array_get(&archives, i);
        if (absolute_path(archive->argument, 0, archive->path, sizeof(archive->path))) goto done;
        size_t archive_length = rt_strlen(archive->path);
        int archive_prefix = !rt_strncmp(output_path, archive->path, archive_length);
#if defined(_WIN32)
        archive_prefix = rt_strlen(output_path) >= archive_length &&
            rt_unicode_normalized_equal(output_path, archive_length, archive->path, archive_length, 1);
#endif
        if (archive_prefix && (archive_length == 1 || !output_path[archive_length] ||
                               output_path[archive_length] == '/')) {
            eg_error("output must be outside every source archive", output_path); goto done;
        }
        for (size_t j = 0; j < i; j++) {
            const Archive *previous = tool_array_get_const(&archives, j);
            int same_path = !rt_strcmp(archive->path, previous->path);
#if defined(_WIN32)
            same_path = rt_unicode_normalized_equal(archive->path, rt_strlen(archive->path),
                previous->path, rt_strlen(previous->path), 1);
#endif
            if (archive->year == previous->year && same_path) {
                eg_error("duplicate archive/year selection: ", archive->path); goto done;
            }
        }
        if (load_archive(archive, i, &papers)) goto done;
        selected += archive->selected;
        deleted += archive->deleted;
        other += archive->other;
        complete = complete && archive->complete;
    }
    rt_sort(papers.data, papers.count, sizeof(Paper), compare_archive_paper);
    for (size_t i = 1; i < papers.count; i++)
        if (!compare_archive_paper(tool_array_get(&papers, i - 1), tool_array_get(&papers, i))) {
            eg_error("duplicate Paper id; ambiguous archive snapshot: ",
                     ((Paper *)tool_array_get(&papers, i))->id); goto done;
        }
    rt_sort(papers.data, papers.count, sizeof(Paper), compare_paper);
    const Paper *previous_selected = 0;
    for (size_t i = 0; i < papers.count; i++) {
        const Paper *paper = tool_array_get_const(&papers, i);
        if (!paper->title) continue;
        if (previous_selected && !rt_strcmp(previous_selected->id, paper->id)) {
            eg_error("Paper selected by multiple archives; choose one authoritative archive/year: ",
                     paper->id); goto done;
        }
        previous_selected = paper;
    }
    if (rt_join_path(output, "cache", cache, sizeof(cache))) goto path_error;
    if (eg_ensure_directory(output) || eg_ensure_directory(cache)) goto done;
    if (selected) {
        if (rt_join_path(directory, "tokenizer.eg2", path, sizeof(path))) goto path_error;
        if (eg_tokenizer_load(&tokenizer, path)) goto done;
    }
    if (tool_byte_buffer_append_char(&manifest, '{') ||
        field(&manifest, "format", archives.count == 1 ?
            "eg2-paper-title-index-v1" : "eg2-paper-title-index-v2") ||
        field(&manifest, "model_revision", EG2_MODEL_REVISION) ||
        field(&manifest, "weights_sha256", EG2_WEIGHTS_SHA256) ||
        field(&manifest, "tokenizer_sha256", EG2_TOKENIZER_SHA256) ||
        field(&manifest, "task", "document") || field(&manifest, "prefix", PREFIX) ||
        field(&manifest, "source_field", "Paper.name") ||
        field(&manifest, "year_basis", "Paper.reference prefix") ||
        number(&manifest, "dimensions", EG2_DIM) || number(&manifest, "count", selected))
        goto allocation_error;
    if (archives.count == 1) {
        if (archive_fields(&manifest, primary)) goto allocation_error;
    } else {
        if (tool_byte_buffer_append_cstr(&manifest, complete ?
            "\"archive_complete\":true,\"archives\":[" : "\"archive_complete\":false,\"archives\":["))
            goto allocation_error;
        for (size_t i = 0; i < archives.count; i++) {
            const Archive *archive = tool_array_get_const(&archives, i);
            if ((i && tool_byte_buffer_append_char(&manifest, ',')) ||
                tool_byte_buffer_append_char(&manifest, '{') ||
                number(&manifest, "count", archive->selected) ||
                archive_fields(&manifest, archive) || tool_byte_buffer_append_char(&manifest, '}'))
                goto allocation_error;
        }
        if (tool_byte_buffer_append_char(&manifest, ']')) goto allocation_error;
    }
    if (tool_byte_buffer_append_cstr(&manifest, ",\"items\":[")) goto allocation_error;
    run.directory = directory;
    run.output = output;
    run.threads = threads;
    run.selected = selected;
    size_t index = 0;
    for (size_t i = 0; i < papers.count; i++) {
        Paper *p = tool_array_get(&papers, i);
        if (!p->title) continue;
        const Archive *archive = tool_array_get_const(&archives, p->archive);
        const Source *source = tool_array_get_const(&archive->sources, p->source);
        unsigned int tokens[EG2_MAX_TOKENS];
        char hex[65], source_hex[65], source_path[4096];
        int found;
        eg_title_key(p->id, p->title, p->key);
        hash_to_hex(p->key, sizeof(p->key), hex);
        hash_to_hex(source->sha, sizeof(source->sha), source_hex);
        eg_title_vector_path(p->key, p->vector);
        if (rt_join_path(output, p->vector, path, sizeof(path)) ||
            rt_join_path(archive->metadata, source->name, source_path, sizeof(source_path))) goto path_error;
        prepared.size = 0;
        if (tool_byte_buffer_append_cstr(&prepared, PREFIX) ||
            tool_byte_buffer_append_cstr(&prepared, p->title)) goto allocation_error;
        if (eg_tokenize(&tokenizer, prepared.data, prepared.size, tokens, &p->tokens) ||
            eg_vector_read(path, p->key, 0, &found)) goto done;
        if (found) {
            run.reused++;
            if (index_progress(&run)) goto done;
        }
        else {
            if (p->tokens > EG2_MAX_TOKENS - run.token_count && flush_titles(&run)) goto done;
            run.papers[run.count] = p;
            run.counts[run.count++] = p->tokens;
            memcpy(run.tokens + run.token_count, tokens, p->tokens * sizeof(*tokens));
            run.token_count += p->tokens;
            if ((run.count == EG2_MAX_BATCH || run.token_count == EG2_MAX_TOKENS) &&
                flush_titles(&run)) goto done;
        }
        if ((index++ && tool_byte_buffer_append_char(&manifest, ',')) ||
            tool_byte_buffer_append_char(&manifest, '{') ||
            field(&manifest, "id", p->id) || field(&manifest, "reference", p->reference) ||
            field(&manifest, "title", p->title) || field(&manifest, "source_file", source_path) ||
            field(&manifest, "source_sha256", source_hex) || field(&manifest, "key", hex) ||
            (archives.count > 1 && number(&manifest, "year", archive->year)) ||
            number(&manifest, "tokens", p->tokens) ||
            tool_byte_buffer_append_cstr(&manifest, "\"vector\":") ||
            tool_json_append_string(&manifest, p->vector) || tool_byte_buffer_append_char(&manifest, '}'))
            goto allocation_error;
    }
    if (flush_titles(&run)) goto done;
    if (tool_byte_buffer_append_cstr(&manifest, "]}\n")) goto allocation_error;
    for (size_t i = 0; i < archives.count; i++) {
        const Archive *archive = tool_array_get_const(&archives, i);
        if (verify_snapshot(archive->metadata, &archive->sources)) goto done;
        unsigned char *current = 0, current_sha[32];
        size_t current_size = 0;
        if (read_source(archive->archive_file, &current, &current_size)) goto done;
        crypto_sha256_hash(current, current_size, current_sha);
        rt_free(current);
        if (memcmp(current_sha, archive->sha, 32)) {
            eg_error("archive.json changed during indexing; rerun with a stable archive: ", archive->path);
            goto done;
        }
    }
    if (rt_join_path(output, "manifest.json", path, sizeof(path))) goto path_error;
    if (atomic_write(path, manifest.data, manifest.size)) goto done;
    if (rt_write_cstr(1, "{\"status\":\"passed\",\"count\":") || rt_write_uint(1, selected) ||
        rt_write_cstr(1, ",\"archives\":") || rt_write_uint(1, archives.count) ||
        rt_write_cstr(1, ",\"computed\":") || rt_write_uint(1, run.computed) ||
        rt_write_cstr(1, ",\"reused\":") || rt_write_uint(1, run.reused) ||
        rt_write_cstr(1, ",\"batches\":") || rt_write_uint(1, run.batches) ||
        rt_write_cstr(1, ",\"batch_documents_max\":") || rt_write_uint(1, run.max_documents) ||
        rt_write_cstr(1, ",\"batch_tokens_max\":") || rt_write_uint(1, run.max_tokens) ||
        rt_write_cstr(1, ",\"excluded_other_year\":") || rt_write_uint(1, other) ||
        rt_write_cstr(1, ",\"excluded_deleted\":") || rt_write_uint(1, deleted) ||
        rt_write_cstr(1, ",\"model_loads\":") || rt_write_uint(1, (unsigned int)run.loaded) ||
        rt_write_cstr(1, ",\"load_ns\":") || rt_write_uint(1, run.load_ns) ||
        rt_write_cstr(1, ",\"total_ns\":") || rt_write_uint(1, platform_get_monotonic_time_ns() - started) ||
        rt_write_cstr(1, "}\n")) goto write_error;
    result = 0;
    goto done;
path_error:
    eg_error("path exceeds supported length or current directory unavailable", 0);
    goto done;
allocation_error:
    eg_error("cannot allocate index manifest or input", 0);
    goto done;
write_error:
    eg_error("cannot write index progress or summary", 0);
done:
    for (size_t i = 0; i < papers.count; i++) {
        Paper *p = tool_array_get(&papers, i);
        rt_free(p->id);
        rt_free(p->title);
        rt_free(p->reference);
    }
    tool_array_free(&papers);
    for (size_t i = 0; i < archives.count; i++)
        tool_array_free(&((Archive *)tool_array_get(&archives, i))->sources);
    tool_array_free(&archives);
    tool_byte_buffer_free(&manifest);
    tool_byte_buffer_free(&prepared);
    eg_model_free(&run.model);
    eg_tokenizer_free(&tokenizer);
    return result;
}

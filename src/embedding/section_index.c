#include "index_io.h"
#include "json.h"
#include "search.h"
#include "sections.h"

#define EG_JSON_BUDGET (64U * 1024U * 1024U)
#define PDF_BUDGET (512U * 1024U * 1024U)

typedef struct {
    char path[4096];
    unsigned char sha[32];
    size_t limit;
    int absent;
} Snapshot;

typedef struct { size_t start, end; } PageRange;

typedef struct {
    char *id, *title, *reference, *file_id, *text;
    char source[4096], pdf[4096], artifact[4096];
    char source_sha[65], pdf_sha[65], prepared_sha[65], text_sha[65];
    const char *coverage;
    size_t pdf_bytes, pdf_pages, main_pages, first, count;
    int attested, separate_reasoning;
    ToolArray pages;
} Document;

typedef struct { EgSection span; size_t document; } Chunk;

typedef struct {
    const char *phase, *reference;
    char output[4096], prepared[4096], archive[4096], model_dir[4096];
    char pipeline[65], engine[129], selection_sha[65];
    unsigned long long started, last_status, timeout_ns;
    unsigned int threads, year;
    size_t selected, scanned, included, excluded, pages, total, processed, computed, reused, needed;
    int plan, loaded, complete, published;
    ToolArray snapshots, files, documents, chunks;
    EgTokenizer tokenizer;
    EgModel model;
} Run;

static int string_field(ToolByteBuffer *buffer, const char *key, const char *value) {
    return eg_json_quote(buffer, key) || tool_byte_buffer_append_char(buffer, ':') ||
        eg_json_quote(buffer, value) || tool_byte_buffer_append_char(buffer, ',');
}

static int integer_field(ToolByteBuffer *buffer, const char *key, unsigned long long value) {
    char digits[32];
    rt_unsigned_to_string(value, digits, sizeof(digits));
    return eg_json_quote(buffer, key) || tool_byte_buffer_append_char(buffer, ':') ||
        tool_byte_buffer_append_cstr(buffer, digits) || tool_byte_buffer_append_char(buffer, ',');
}

static int digest_field(const EgJson *json, size_t object, const char *key, char value[65]) {
    unsigned char digest[32];
    if (eg_json_string(json, eg_json_get(json, object, key), value, 65) || eg_parse_digest(value, digest))
        return eg_error("invalid digest field: ", key);
    return 0;
}

static int join(const char *directory, const char *name, char path[4096]) {
    if (rt_join_path(directory, name, path, 4096)) return eg_error("path exceeds budget: ", name);
    return 0;
}

static void hash_text(const void *bytes, size_t size, char text[65]) {
    unsigned char sha[32];
    crypto_sha256_hash(bytes, size, sha);
    eg_hex_digest(sha, text);
}

static int expired(const Run *run) {
    return platform_get_monotonic_time_ns() - run->started >= run->timeout_ns;
}

static int status(Run *run, int force) {
    unsigned long long now = platform_get_monotonic_time_ns();
    if (!force && now - run->last_status < 1000000000ULL) return 0;
    ToolByteBuffer buffer;
    char path[4096];
    tool_byte_buffer_init(&buffer);
    int failed = join(run->output, "status.json", path) ||
        tool_byte_buffer_append_char(&buffer, '{') ||
        string_field(&buffer, "format", "eg2-section-progress-v1") ||
        string_field(&buffer, "phase", run->phase) ||
        string_field(&buffer, "current_reference", run->reference ? run->reference : "") ||
        integer_field(&buffer, "pid", (unsigned int)platform_get_process_id()) ||
        integer_field(&buffer, "elapsed_ms", (now - run->started) / 1000000ULL) ||
        integer_field(&buffer, "timeout_seconds", run->timeout_ns / 1000000000ULL) ||
        integer_field(&buffer, "documents_total", run->selected) ||
        integer_field(&buffer, "documents_scanned", run->scanned) ||
        integer_field(&buffer, "documents_included", run->included) ||
        integer_field(&buffer, "documents_excluded", run->excluded) ||
        integer_field(&buffer, "main_form_pages", run->pages) ||
        integer_field(&buffer, "chunks_total", run->total) ||
        integer_field(&buffer, "chunks_processed", run->processed) ||
        integer_field(&buffer, "chunks_remaining", run->total - run->processed) ||
        integer_field(&buffer, "computed_vectors", run->computed) ||
        integer_field(&buffer, "reused_chunks", run->reused) ||
        integer_field(&buffer, "missing_vectors", run->needed) ||
        integer_field(&buffer, "model_loads", (unsigned int)run->loaded) ||
        tool_byte_buffer_append_cstr(&buffer, "\"published\":") ||
        tool_byte_buffer_append_cstr(&buffer, run->published ? "true}\n" : "false}\n") ||
        eg_atomic_write(path, buffer.data, buffer.size);
    if (!failed) {
        failed = rt_write_all(2, buffer.data, buffer.size);
        run->last_status = now;
    }
    tool_byte_buffer_free(&buffer);
    if (failed) return eg_error("cannot publish indexing progress", 0);
    return 0;
}

static int read_snapshot(Run *run, const char *path, size_t limit,
                         unsigned char **bytes, size_t *size, int optional, int *found) {
    PlatformDirEntry info;
    Snapshot *snapshot = tool_array_append(&run->snapshots);
    if (!snapshot) return eg_error("cannot allocate source snapshot", 0);
    rt_memset(snapshot, 0, sizeof(*snapshot));
    if (rt_strlen(path) >= sizeof(snapshot->path)) return eg_error("snapshot path too long", 0);
    rt_copy_string(snapshot->path, sizeof(snapshot->path), path);
    snapshot->limit = limit;
    *bytes = 0; *size = 0; *found = 0;
    if (optional && platform_get_path_info(path, &info)) { snapshot->absent = 1; return 0; }
    if (eg_read_regular(path, limit, bytes, size)) return -1;
    crypto_sha256_hash(*bytes, *size, snapshot->sha);
    *found = 1;
    return 0;
}

static int verify_snapshot(Run *run) {
    for (size_t i = 0; i < run->snapshots.count; i++) {
        const Snapshot *snapshot = tool_array_get_const(&run->snapshots, i);
        PlatformDirEntry info;
        if (expired(run)) return -1;
        if (snapshot->absent) {
            if (!platform_get_path_info(snapshot->path, &info))
                return eg_error("previously absent input appeared during indexing: ", snapshot->path);
            continue;
        }
        unsigned char *bytes = 0, sha[32];
        size_t size;
        if (eg_read_regular(snapshot->path, snapshot->limit, &bytes, &size)) return -1;
        crypto_sha256_hash(bytes, size, sha);
        rt_free(bytes);
        if (memcmp(sha, snapshot->sha, 32)) return eg_error("source changed during indexing: ", snapshot->path);
        if (status(run, 0)) return -1;
    }
    return 0;
}

static int read_json(Run *run, const char *path, EgJson *json, unsigned char **bytes,
                     int optional, int *found) {
    size_t size;
    if (read_snapshot(run, path, EG_JSON_BUDGET, bytes, &size, optional, found)) return -1;
    if (!*found) return 0;
    if (eg_json_parse(json, (const char *)*bytes, size)) return eg_error("invalid JSON input: ", path);
    return 0;
}

static int resolve_pdf(Run *run, Document *document) {
    char id_hash[65], relative[4096], path[4096], base[4096], expected[65];
    unsigned char *bytes = 0;
    EgJson json = {0};
    int found, result = -1;
    unsigned long long expected_size = 0;
    size_t size;
    hash_text(document->file_id, rt_strlen(document->file_id), id_hash);
    size_t n = tool_buffer_append_cstr(relative, sizeof(relative), 0, "cache/document-");
    n = tool_buffer_append_cstr(relative, sizeof(relative), n, id_hash);
    tool_buffer_append_cstr(relative, sizeof(relative), n, ".json");
    if (join(run->archive, relative, path) || read_json(run, path, &json, &bytes, 1, &found)) goto done;
    if (found) {
        if (digest_field(&json, 0, "sha256", expected) ||
            eg_json_uint(&json, eg_json_get(&json, 0, "size"), &expected_size) ||
            eg_json_string(&json, eg_json_get(&json, 0, "currentPath"), relative, sizeof(relative)) ||
            rt_strncmp(relative, "documents/", 10) || rt_strncmp(relative + 10, id_hash, 64) ||
            relative[74] != '-' || !relative[75]) goto invalid;
        for (size_t i = 10; relative[i]; i++)
            if (relative[i] == '/' || relative[i] == '\\') goto invalid;
        if (join(run->archive, relative, document->pdf)) goto done;
        document->attested = 1;
    } else {
        size_t matches = 0;
        if (join(run->archive, "documents", base)) goto done;
        for (size_t i = 0; i < run->files.count; i++) {
            const EgIndexFile *file = tool_array_get_const(&run->files, i);
            size_t length = rt_strlen(file->name);
            if (length <= 65 || rt_strncmp(file->name, id_hash, 64) || file->name[64] != '-') continue;
            if (length >= 5 && !rt_strcmp(file->name + length - 5, ".part")) continue;
            if (join(base, file->name, document->pdf)) goto done;
            matches++;
        }
        if (matches != 1) {
            eg_error("expected exactly one legacy mainFile PDF: ", document->file_id);
            goto done;
        }
    }
    eg_json_free(&json); rt_free(bytes); bytes = 0;
    if (read_snapshot(run, document->pdf, PDF_BUDGET, &bytes, &size, 0, &found)) goto done;
    hash_text(bytes, size, document->pdf_sha);
    if (document->attested && (expected_size != size || rt_strcmp(expected, document->pdf_sha))) {
        eg_error("mainFile bytes disagree with archive attestation: ", document->pdf); goto done;
    }
    document->pdf_bytes = size;
    result = 0;
    goto done;
invalid:
    eg_error("invalid mainFile archive attestation: ", path);
done:
    eg_json_free(&json); rt_free(bytes);
    return result;
}

static int contains(const char *text, const char *needle) {
    size_t length = rt_strlen(needle), size = rt_strlen(text);
    for (size_t i = 0; i + length <= size; i++) if (!memcmp(text + i, needle, length)) return 1;
    return 0;
}

static int prepare_document(Run *run, Document *document) {
    char relative[160], data_sha[65], source_sha[65], engine[129], key[65];
    EgJson json = {0};
    unsigned char *bytes = 0;
    int found, result = -1, boundary = 1, selected_warnings = 0;
    unsigned long long format, source_size, pages, warning_pages, observed_warnings = 0;
    ToolByteBuffer text;
    tool_byte_buffer_init(&text);
    size_t n = tool_buffer_append_cstr(relative, sizeof(relative), 0, run->pipeline);
    n = tool_buffer_append_char(relative, sizeof(relative), n, '/');
    n = tool_buffer_append_cstr(relative, sizeof(relative), n, document->pdf_sha);
    tool_buffer_append_cstr(relative, sizeof(relative), n, ".json");
    if (join(run->prepared, relative, document->artifact) ||
        read_json(run, document->artifact, &json, &bytes, 1, &found)) goto done;
    if (!found) { document->coverage = "not-prepared"; result = 0; goto done; }
    size_t data_index = eg_json_get(&json, 0, "data"), array_index = eg_json_get(&json, data_index, "pages");
    const EgJsonToken *data = eg_json_token(&json, data_index), *array = eg_json_token(&json, array_index);
    if (eg_json_uint(&json, eg_json_get(&json, 0, "format"), &format) || format != 1 ||
        !data || data->kind != EG_JSON_OBJECT || digest_field(&json, 0, "sha256", data_sha)) goto invalid;
    hash_text(bytes + data->start, data->end - data->start, document->prepared_sha);
    if (rt_strcmp(data_sha, document->prepared_sha) ||
        digest_field(&json, data_index, "sourceSha256", source_sha) ||
        rt_strcmp(source_sha, document->pdf_sha) ||
        eg_json_uint(&json, eg_json_get(&json, data_index, "sourceBytes"), &source_size) ||
        source_size != document->pdf_bytes ||
        eg_json_string(&json, eg_json_get(&json, data_index, "engine"), engine, sizeof(engine)) ||
        rt_strcmp(engine, run->engine) || digest_field(&json, data_index, "key", key) ||
        rt_strcmp(key, run->pipeline) || !array || array->kind != EG_JSON_ARRAY ||
        eg_json_uint(&json, eg_json_get(&json, data_index, "pageCount"), &pages) || pages > 100000 ||
        eg_json_uint(&json, eg_json_get(&json, data_index, "warningPages"), &warning_pages)) goto invalid;
    if (eg_json_equals(&json, eg_json_get(&json, data_index, "status"), "failed")) {
        document->coverage = "preparation-failed"; result = 0; goto done;
    }
    if ((!eg_json_equals(&json, eg_json_get(&json, data_index, "status"), "complete") &&
         !eg_json_equals(&json, eg_json_get(&json, data_index, "status"), "warnings")) ||
        !eg_json_token(&json, eg_json_get(&json, data_index, "error")) ||
        eg_json_token(&json, eg_json_get(&json, data_index, "error"))->kind != EG_JSON_NULL) goto invalid;
    document->pdf_pages = (size_t)pages;
    size_t observed = 0;
    unsigned int main_pages = 0;
    for (size_t i = array_index + 1; i < array->next; i = eg_json_token(&json, i)->next) {
        unsigned long long ordinal, warnings;
        size_t text_index = eg_json_get(&json, i, "text");
        const EgJsonToken *page_text = eg_json_token(&json, text_index);
        observed++;
        if (eg_json_uint(&json, eg_json_get(&json, i, "page"), &ordinal) || ordinal != observed ||
            eg_json_uint(&json, eg_json_get(&json, i, "warnings"), &warnings) || warnings > 7 ||
            !page_text || page_text->kind != EG_JSON_STRING) goto invalid;
        observed_warnings += warnings != 0;
        if (observed != 1 && observed > main_pages) continue;
        char *decoded = eg_json_dup_string(&json, text_index, EG_JSON_BUDGET);
        if (!decoded) goto invalid;
        unsigned int page = 0, total = 0;
        int numbered = eg_form_number(decoded, &page, &total);
        if (observed == 1) {
            main_pages = numbered && page == 1 && total <= pages ? total : 0;
            boundary = main_pages && eg_form_reference(decoded, document->reference);
        }
        if (!numbered || page != observed || total != main_pages) boundary = 0;
        if (warnings) selected_warnings = 1;
        if (observed <= main_pages) {
            PageRange *range = tool_array_append(&document->pages);
            if (!range) { rt_free(decoded); eg_error("cannot allocate page provenance", 0); goto done; }
            if (text.size && tool_byte_buffer_append_cstr(&text, "\n\n")) { rt_free(decoded); goto memory; }
            range->start = text.size;
            if (tool_byte_buffer_append_cstr(&text, decoded)) { rt_free(decoded); goto memory; }
            range->end = text.size;
        }
        rt_free(decoded);
    }
    if (observed != pages || observed_warnings != warning_pages) goto invalid;
    if (!boundary || document->pages.count != main_pages) {
        document->coverage = "unclear-main-form-boundary"; result = 0; goto done;
    }
    if (selected_warnings) { document->coverage = "main-form-page-warnings"; result = 0; goto done; }
    if (!text.size) { document->coverage = "empty-main-form"; result = 0; goto done; }
    if (tool_byte_buffer_append_char(&text, 0)) goto memory;
    document->text = (char *)text.data; text.data = 0;
    document->main_pages = main_pages;
    document->coverage = "numbered-main-form-only";
    document->separate_reasoning = contains(document->text, "Anlagen:") &&
        contains(document->text, "\n1 Begr\xc3\xbcndung ");
    hash_text(document->text, rt_strlen(document->text), document->text_sha);
    ToolArray sections;
    tool_array_init(&sections, sizeof(EgSection));
    if (eg_sections(&run->tokenizer, document->text, &sections)) { tool_array_free(&sections); goto done; }
    document->first = run->chunks.count;
    document->count = sections.count;
    for (size_t i = 0; i < sections.count; i++) {
        if (run->chunks.count >= 1000000) { tool_array_free(&sections); goto memory; }
        Chunk *chunk = tool_array_append(&run->chunks);
        if (!chunk) { tool_array_free(&sections); goto memory; }
        chunk->span = *(const EgSection *)tool_array_get_const(&sections, i);
        chunk->document = run->documents.count - 1;
    }
    tool_array_free(&sections);
    result = 0;
    goto done;
memory:
    eg_error("section planning exceeds memory budget", 0);
    goto done;
invalid:
    eg_error("invalid or inconsistent prepared artifact: ", document->artifact);
done:
    eg_json_free(&json); rt_free(bytes); tool_byte_buffer_free(&text);
    return result;
}

static int plan_document(Run *run, const EgJson *selection, size_t item) {
    Document *document = tool_array_append(&run->documents);
    EgJson json = {0};
    unsigned char *bytes = 0;
    char expected_sha[65], observed_sha[65], file_id[4096];
    int found, result = -1;
    if (!document) return eg_error("cannot allocate document plan", 0);
    rt_memset(document, 0, sizeof(*document));
    tool_array_init(&document->pages, sizeof(PageRange));
    document->id = eg_json_dup_string(selection, eg_json_get(selection, item, "id"), 4096);
    document->title = eg_json_dup_string(selection, eg_json_get(selection, item, "title"), EG2_MAX_TEXT);
    document->reference = eg_json_dup_string(selection, eg_json_get(selection, item, "reference"), 512);
    run->reference = document->reference;
    if (!document->id || !document->title || !document->reference ||
        eg_json_string(selection, eg_json_get(selection, item, "source_file"), document->source, sizeof(document->source)) ||
        digest_field(selection, item, "source_sha256", expected_sha) ||
        read_json(run, document->source, &json, &bytes, 0, &found)) goto done;
    hash_text(bytes, json.length, observed_sha);
    if (rt_strcmp(expected_sha, observed_sha) ||
        !eg_json_equals(&json, eg_json_get(&json, 0, "id"), document->id) ||
        !eg_json_equals(&json, eg_json_get(&json, 0, "name"), document->title) ||
        !eg_json_equals(&json, eg_json_get(&json, 0, "reference"), document->reference) ||
        !eg_json_equals(&json, eg_json_get(&json, 0, "type"), "https://schema.oparl.org/1.1/Paper") ||
        eg_json_true(&json, eg_json_get(&json, 0, "deleted"))) {
        eg_error("Paper no longer matches title-index source snapshot: ", document->source); goto done;
    }
    rt_copy_string(document->source_sha, sizeof(document->source_sha), observed_sha);
    size_t main_index = eg_json_get(&json, 0, "mainFile");
    const EgJsonToken *main = eg_json_token(&json, main_index);
    if (!main || main->kind == EG_JSON_NULL) { document->coverage = "no-main-file"; result = 0; goto done; }
    if (main->kind == EG_JSON_OBJECT) {
        if (eg_json_true(&json, eg_json_get(&json, main_index, "deleted"))) {
            document->coverage = "main-file-deleted"; result = 0; goto done;
        }
        main_index = eg_json_get(&json, main_index, "id");
    }
    if (eg_json_string(&json, main_index, file_id, sizeof(file_id)) || !file_id[0]) {
        eg_error("invalid Paper.mainFile: ", document->source); goto done;
    }
    document->file_id = eg_json_dup_string(&json, main_index, sizeof(file_id));
    if (!document->file_id || resolve_pdf(run, document) || prepare_document(run, document)) goto done;
    result = 0;
done:
    eg_json_free(&json); rt_free(bytes);
    if (!result) {
        run->scanned++;
        if (document->count) { run->included++; run->pages += document->main_pages; }
        else run->excluded++;
        run->total = run->chunks.count;
    }
    return result;
}

static int manifest(Run *run) {
    ToolByteBuffer buffer;
    char path[4096];
    tool_byte_buffer_init(&buffer);
    int result = -1;
#define APPEND(text) do { if (tool_byte_buffer_append_cstr(&buffer, text)) goto memory; } while (0)
#define STR(key, value) do { if (string_field(&buffer, key, value)) goto memory; } while (0)
#define NUM(key, value) do { if (integer_field(&buffer, key, value)) goto memory; } while (0)
    APPEND("{");
    STR("format", run->plan ? "eg2-paper-section-plan-v1" : "eg2-paper-section-index-v1");
    STR("model_revision", EG2_MODEL_REVISION); STR("weights_sha256", EG2_WEIGHTS_SHA256);
    STR("tokenizer_sha256", EG2_TOKENIZER_SHA256); STR("task", "document"); STR("prefix", EG2_DOCUMENT_PREFIX);
    STR("boundary_recipe", "exact-reference-numbered-main-form-v1");
    STR("chunk_recipe", "utf8-2400bytes-800tokens-sentence-end-overlap80-v1");
    STR("section_key_recipe", EG_SECTION_RECIPE);
    STR("offset_unit", "UTF-8 bytes; zero-based half-open; normalized pages joined with two LF");
    STR("scope", "Main forms only; following PDF pages and separately attached reasoning are not indexed.");
    STR("source_archive", run->archive); STR("papers_manifest_sha256", run->selection_sha);
    STR("prepared_root", run->prepared); STR("preparation_key", run->pipeline); STR("engine", run->engine);
    NUM("year", run->year); NUM("dimensions", EG2_DIM); NUM("max_tokens", EG_SECTION_LIMIT);
    NUM("max_overlap_text_tokens", EG_SECTION_OVERLAP); NUM("count", run->total);
    NUM("documents_selected", run->selected); NUM("documents_included", run->included);
    NUM("documents_excluded", run->excluded); NUM("main_form_pages", run->pages);
    NUM("computed_vectors", run->computed); NUM("reused_chunks", run->reused);
    NUM("elapsed_ms", (platform_get_monotonic_time_ns() - run->started) / 1000000ULL);
    APPEND(run->complete ? "\"archive_complete\":true," : "\"archive_complete\":false,");
    APPEND("\"documents\":[");
    for (size_t i = 0; i < run->documents.count; i++) {
        const Document *d = tool_array_get_const(&run->documents, i);
        if (i) APPEND(",");
        APPEND("{"); STR("id", d->id); STR("reference", d->reference); STR("title", d->title);
        STR("source_file", d->source); STR("source_sha256", d->source_sha);
        STR("file_id", d->file_id ? d->file_id : ""); STR("pdf", d->pdf); STR("pdf_sha256", d->pdf_sha);
        STR("prepared_artifact", d->artifact); STR("prepared_data_sha256", d->prepared_sha);
        STR("joined_text_sha256", d->text_sha); STR("coverage", d->coverage);
        NUM("pdf_pages", d->pdf_pages); NUM("main_form_pages", d->main_pages);
        NUM("following_pages_excluded", d->pdf_pages - d->main_pages);
        NUM("first_chunk", d->first); NUM("chunks", d->count);
        APPEND(d->attested ? "\"archive_attested\":true," : "\"archive_attested\":false,");
        APPEND(d->separate_reasoning ? "\"separate_reasoning_attachment\":true}" : "\"separate_reasoning_attachment\":false}");
    }
    APPEND("],\"items\":[");
    for (size_t i = 0; i < run->chunks.count; i++) {
        const Chunk *chunk = tool_array_get_const(&run->chunks, i);
        const EgSection *s = &chunk->span;
        const Document *d = tool_array_get_const(&run->documents, chunk->document);
        char key[65], relative[80], identity[65];
        eg_hex_digest(s->key, key); eg_title_vector_path(s->key, relative);
        eg_section_identity(d->id, d->pdf_sha, d->text_sha, s->start, s->end, identity);
        size_t first_page = 0, last_page = 0, page_start = 0, page_end = 0;
        for (size_t p = 0; p < d->pages.count; p++) {
            const PageRange *range = tool_array_get_const(&d->pages, p);
            if (s->end <= range->start || s->start >= range->end) continue;
            if (!first_page) { first_page = p + 1; page_start = s->start > range->start ? s->start - range->start : 0; }
            last_page = p + 1; page_end = s->end < range->end ? s->end - range->start : range->end - range->start;
        }
        size_t size = s->end - s->start;
        char *text = rt_malloc(size + 1);
        if (!text) goto memory;
        memcpy(text, d->text + s->start, size); text[size] = 0;
        if (i) APPEND(",");
        APPEND("{"); STR("id", identity); STR("paper_id", d->id);
        NUM("document", chunk->document); NUM("ordinal", i - d->first + 1);
        NUM("start", s->start); NUM("end", s->end); NUM("page_first", first_page); NUM("page_last", last_page);
        NUM("page_first_start", page_start); NUM("page_last_end", page_end); NUM("tokens", s->tokens);
        STR("key", key); STR("vector", relative);
        int failed = eg_json_quote(&buffer, "text") || tool_byte_buffer_append_char(&buffer, ':') ||
            eg_json_quote(&buffer, text) || tool_byte_buffer_append_char(&buffer, '}');
        rt_free(text);
        if (failed) goto memory;
    }
    APPEND("]}\n");
    if (join(run->output, run->plan ? "plan.json" : "manifest.json", path) ||
        eg_atomic_write(path, buffer.data, buffer.size)) goto done;
    result = 0;
    goto done;
memory:
    eg_error("cannot serialize section manifest", 0);
done:
    tool_byte_buffer_free(&buffer);
#undef APPEND
#undef STR
#undef NUM
    return result;
}

static int overlaps(const char *a, const char *b) {
    size_t n = rt_strlen(a);
    return !rt_strncmp(a, b, n) && (!b[n] || b[n] == '/' || (n && a[n - 1] == '/'));
}

int main(int argc, char **argv) {
    Run run = {0};
    const char *papers = 0, *prepared = 0, *output = 0, *model_dir = "models/native";
    char path[4096], lock[4096], paper_root[4096], cache[4096];
    EgJson selection = {0}, pipeline = {0};
    EgTitleIndex titles = {0};
    unsigned char *selection_bytes = 0, *pipeline_bytes = 0;
    int result = 1, locked = 0, found;
    unsigned long long timeout_seconds = 3600, value;
    run.started = platform_get_monotonic_time_ns(); run.threads = 16; run.phase = "planning";
    tool_array_init(&run.snapshots, sizeof(Snapshot)); tool_array_init(&run.files, sizeof(EgIndexFile));
    tool_array_init(&run.documents, sizeof(Document)); tool_array_init(&run.chunks, sizeof(Chunk));
    for (int i = 1; i < argc; i++) {
        if (!rt_strcmp(argv[i], "--plan")) { run.plan = 1; continue; }
        if (!rt_strcmp(argv[i], "--help")) {
            rt_write_cstr(1, "Usage: embeddinggemma-sections --papers TITLE_INDEX --prepared DIR --output DIR\n"
                "       [--model models/native] [--threads 1..32] [--timeout-seconds 1..86400] [--plan]\n"
                "Separate main-form index; progress: OUTPUT/status.json; interrupted runs reuse verified vectors.\n");
            result = 0; goto done;
        }
        if (i + 1 == argc) goto arguments;
        const char *option = argv[i], *argument = argv[++i];
        if (!rt_strcmp(option, "--papers")) papers = argument;
        else if (!rt_strcmp(option, "--prepared")) prepared = argument;
        else if (!rt_strcmp(option, "--output")) output = argument;
        else if (!rt_strcmp(option, "--model")) model_dir = argument;
        else if (!rt_strcmp(option, "--threads")) {
            if (rt_parse_uint(argument, &value) || value < 1 || value > 32) goto arguments;
            run.threads = (unsigned int)value;
        } else if (!rt_strcmp(option, "--timeout-seconds")) {
            if (rt_parse_uint(argument, &timeout_seconds) || !timeout_seconds || timeout_seconds > 86400) goto arguments;
        } else goto arguments;
    }
    if (!papers || !prepared || !output) goto arguments;
    run.timeout_ns = timeout_seconds * 1000000000ULL;
    if (eg_absolute_path(papers, 0, paper_root, sizeof(paper_root)) ||
        eg_absolute_path(prepared, 0, run.prepared, sizeof(run.prepared)) ||
        eg_absolute_path(model_dir, 0, run.model_dir, sizeof(run.model_dir)) ||
        eg_absolute_path(output, 1, run.output, sizeof(run.output))) goto done;
    if (overlaps(run.output, paper_root) || overlaps(paper_root, run.output) ||
        overlaps(run.output, run.prepared) || overlaps(run.prepared, run.output) ||
        overlaps(run.output, run.model_dir) || overlaps(run.model_dir, run.output)) {
        eg_error("section output must not overlap input directories", 0); goto done;
    }
    if (join(paper_root, "manifest.json", path) ||
        read_json(&run, path, &selection, &selection_bytes, 0, &found) ||
        eg_title_index_load(&titles, paper_root)) goto done;
    unsigned long long year, count, format;
    char archive_path[4096];
    if (!eg_json_equals(&selection, eg_json_get(&selection, 0, "format"), "eg2-paper-title-index-v1") ||
        eg_json_uint(&selection, eg_json_get(&selection, 0, "year"), &year) ||
        eg_json_uint(&selection, eg_json_get(&selection, 0, "count"), &count) || count != titles.count ||
        eg_json_string(&selection, eg_json_get(&selection, 0, "source_archive"), archive_path, sizeof(archive_path)) ||
        eg_absolute_path(archive_path, 0, run.archive, sizeof(run.archive))) {
        eg_error("--papers requires a verified single-year title index", 0); goto done;
    }
    run.year = (unsigned int)year; run.selected = (size_t)count; run.complete = titles.complete;
    hash_text(selection_bytes, selection.length, run.selection_sha);
    if (overlaps(run.output, run.archive) || overlaps(run.archive, run.output)) {
        eg_error("section output must not overlap the original archive", 0); goto done;
    }
    if (eg_ensure_directory(run.output) || join(run.output, ".index.lock", lock)) goto done;
    int fd = platform_open_create_exclusive(lock, 0600);
    if (fd < 0) { eg_error("index locked; verify no process is running before removing: ", lock); goto done; }
    locked = 1;
    char pid[32];
    rt_unsigned_to_string((unsigned int)platform_get_process_id(), pid, sizeof(pid));
    int failed = rt_write_cstr(fd, pid);
    if (platform_close(fd)) failed = -1;
    if (failed) { eg_error("cannot write index lock", 0); goto done; }
    if (status(&run, 1)) goto done;
    if (join(run.archive, "documents", path) || eg_index_files(path, 0, &run.files) ||
        join(run.prepared, "manifest.json", path) || read_json(&run, path, &pipeline, &pipeline_bytes, 0, &found)) goto done;
    const EgJsonToken *layout = eg_json_token(&pipeline, eg_json_get(&pipeline, 0, "allPageLayout"));
    char templates[65];
    if (eg_json_uint(&pipeline, eg_json_get(&pipeline, 0, "format"), &format) || format != 1 ||
        digest_field(&pipeline, 0, "key", run.pipeline) || digest_field(&pipeline, 0, "templatesSha256", templates) ||
        eg_json_string(&pipeline, eg_json_get(&pipeline, 0, "engine"), run.engine, sizeof(run.engine)) ||
        !run.engine[0] || !layout || layout->kind != EG_JSON_BOOL) {
        eg_error("invalid preparation pipeline manifest", 0); goto done;
    }
    if (join(run.model_dir, "tokenizer.eg2", path) || eg_tokenizer_load(&run.tokenizer, path) ||
        join(run.output, "cache", cache) || eg_ensure_directory(cache) || verify_snapshot(&run)) goto done;
    size_t items_index = eg_json_get(&selection, 0, "items");
    const EgJsonToken *items = eg_json_token(&selection, items_index);
    if (!items || items->kind != EG_JSON_ARRAY) { eg_error("invalid selected Paper array", 0); goto done; }
    for (size_t i = items_index + 1; i < items->next; i = eg_json_token(&selection, i)->next) {
        if (expired(&run)) goto done;
        if (plan_document(&run, &selection, i) || status(&run, 0)) goto done;
    }
    if (run.scanned != run.selected) { eg_error("inconsistent section plan", 0); goto done; }
    run.phase = run.plan ? "checking-cache" : "indexing";
    if (status(&run, 1)) goto done;
    for (size_t i = 0; i < run.chunks.count; i++) {
        const Chunk *chunk = tool_array_get_const(&run.chunks, i);
        const Document *d = tool_array_get_const(&run.documents, chunk->document);
        char relative[80];
        eg_title_vector_path(chunk->span.key, relative);
        run.reference = d->reference;
        if (expired(&run) || join(run.output, relative, path) ||
            eg_vector_read(path, chunk->span.key, 0, &found)) goto done;
        if (found) run.reused++;
        else if (run.plan) run.needed++;
        else {
            if (!run.loaded) {
                char weights[4096];
                run.phase = "loading-model";
                if (status(&run, 1) || join(run.model_dir, "weights.eg2", weights) ||
                    eg_model_load(&run.model, weights)) goto done;
                run.loaded = 1;
                if (eg_model_set_threads(&run.model, run.threads)) goto done;
                run.phase = "indexing";
                if (expired(&run) || status(&run, 1)) goto done;
            }
            unsigned int tokens[EG2_MAX_TOKENS], token_count;
            float vector[EG2_DIM];
            if (eg_section_tokens(&run.tokenizer, d->text + chunk->span.start,
                                  chunk->span.end - chunk->span.start, tokens, &token_count) ||
                token_count != chunk->span.tokens || token_count > EG_SECTION_LIMIT) {
                eg_error("section token plan changed", d->reference); goto done;
            }
            if (eg_embed(&run.model, tokens, token_count, vector, 0) ||
                eg_vector_write(path, chunk->span.key, vector)) goto done;
            run.computed++;
        }
        run.processed++;
        if (status(&run, 0)) goto done;
    }
    run.phase = "verifying-sources";
    if (status(&run, 1) || verify_snapshot(&run) || expired(&run)) goto done;
    for (size_t i = 0; !run.plan && i < run.chunks.count; i++) {
        const Chunk *chunk = tool_array_get_const(&run.chunks, i);
        char relative[80];
        eg_title_vector_path(chunk->span.key, relative);
        if (join(run.output, relative, path) || eg_vector_read(path, chunk->span.key, 0, &found) || !found) {
            eg_error("section vector disappeared before publication", 0); goto done;
        }
    }
    if (expired(&run) || manifest(&run)) goto done;
    run.published = !run.plan;
    run.phase = run.plan ? "planned" : "complete";
    if (status(&run, 1)) goto done;
    result = 0;
    goto done;
arguments:
    eg_error("invalid arguments; use embeddinggemma-sections --help", 0);
done:
    if (locked) {
        if (result) {
            run.phase = expired(&run) ? "timed-out" : "failed";
            if (expired(&run)) { eg_error("indexing timeout; cached vectors are resumable", 0); result = 3; }
            if (status(&run, 1)) result = 1;
        }
        if (platform_remove_file(lock)) { eg_error("cannot remove owned index lock: ", lock); result = 1; }
    }
    eg_model_free(&run.model); eg_tokenizer_free(&run.tokenizer); eg_title_index_free(&titles);
    eg_json_free(&selection); eg_json_free(&pipeline); rt_free(selection_bytes); rt_free(pipeline_bytes);
    for (size_t i = 0; i < run.documents.count; i++) {
        Document *d = tool_array_get(&run.documents, i);
        rt_free(d->id); rt_free(d->title); rt_free(d->reference); rt_free(d->file_id); rt_free(d->text);
        tool_array_free(&d->pages);
    }
    tool_array_free(&run.snapshots); tool_array_free(&run.files);
    tool_array_free(&run.documents); tool_array_free(&run.chunks);
    return result;
}

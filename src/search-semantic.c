#include "search.h"
#include "embedding/search.h"
#include "embedding/index_io.h"
#include "hash_util.h"

#define NONE UINT32_MAX

struct SearchEmbedding {
    EgTitleIndex titles;
    EgSectionIndex sections;
    EgTokenizer tokenizer;
    EgModel model;
    PlatformMutex inference_lock;
    uint32_t *title_records, *passage_records;
    char model_directory[4096];
    unsigned int threads;
    int loaded;
};

int eg_title_index_main(int argc, char **argv);
int eg_section_index_main(int argc, char **argv);

static int title_order(const void *a, const void *b) {
    return rt_strcmp(((const EgTitle *)a)->id, ((const EgTitle *)b)->id);
}

static int path_hash(const char *directory, const char *name, char sha[65]) {
    char path[4096];
    unsigned char *bytes = 0, digest[32];
    size_t size;
    if (rt_join_path(directory, name, path, sizeof(path)) ||
        eg_read_regular(path, 64U * 1024U * 1024U, &bytes, &size)) return -1;
    crypto_sha256_hash(bytes, size, digest);
    hash_to_hex(digest, sizeof(digest), sha);
    rt_free(bytes);
    return 0;
}

static int generation_root(const ToolJson *json, const char *directory, char root[4096]) {
    char generation[128], base[4096];
    if (tool_json_string(json, tool_json_get(json, 0, "generation"), generation, sizeof(generation)) || !generation[0])
        return search_error("embedding generation is missing", directory);
    for (size_t i = 0; generation[i]; i++)
        if ((generation[i] < '0' || generation[i] > '9') && generation[i] != '-')
            return search_error("invalid embedding generation", directory);
    if (rt_join_path(directory, "generations", base, sizeof(base)) ||
        rt_join_path(base, generation, root, 4096))
        return search_error("embedding generation path is too long", directory);
    return 0;
}

static int reuse_cache(const char *source, const char *destination) {
    char input[4096], output[4096], key_text[65];
    ToolArray files;
    PlatformDirEntry info;
    if (rt_join_path(source, "cache", input, sizeof(input)) ||
        rt_join_path(destination, "cache", output, sizeof(output))) return search_error("cache path is too long", source);
    if (eg_ensure_directory(destination) || eg_ensure_directory(output)) return -1;
    if (platform_get_path_info(input, &info)) {
        rt_write_cstr(2, "oparl-index: no prior vector cache; inference may be needed: ");
        rt_write_cstr(2, input); rt_write_char(2, '\n');
        return 0;
    }
    tool_array_init(&files, sizeof(EgIndexFile));
    int status = eg_index_files(input, ".egv", &files), copy = 0;
    for (size_t i = 0; !status && i < files.count; i++) {
        const EgIndexFile *file = tool_array_get_const(&files, i);
        char from[4096], to[4096];
        unsigned char key[32], *bytes = 0;
        size_t size;
        int found;
        if (rt_strlen(file->name) != 68) { status = search_error("invalid vector cache filename", file->name); break; }
        memcpy(key_text, file->name, 64); key_text[64] = 0;
        if (eg_parse_digest(key_text, key) || rt_join_path(input, file->name, from, sizeof(from)) ||
            rt_join_path(output, file->name, to, sizeof(to)) || eg_vector_read(from, key, 0, &found) || !found) {
            status = search_error("cannot verify prior cached vector", file->name); break;
        }
        if (!copy && !platform_create_hard_link(from, to)) continue;
        if (!copy) {
            rt_write_cstr(2, "oparl-index: cache hardlinks unavailable; copying verified vectors\n");
            copy = 1;
        }
        status = eg_read_regular(from, EG2_VECTOR_SIZE, &bytes, &size);
        if (!status) status = eg_atomic_write(to, bytes, size);
        rt_free(bytes);
        if (status) search_error("cannot copy cached vector", from);
    }
    tool_array_free(&files);
    return status;
}

static void embedding_free(SearchEmbedding *e) {
    if (!e) return;
    eg_title_index_free(&e->titles);
    eg_section_index_free(&e->sections);
    eg_tokenizer_free(&e->tokenizer);
    eg_model_free(&e->model);
    rt_free(e->title_records);
    rt_free(e->passage_records);
    rt_free(e);
}

void search_embedding_close(SearchIndex *index) {
    embedding_free(index->embedding);
    index->embedding = 0;
}

static int append_titles(SearchEmbedding *e, const char *path) {
    EgTitleIndex part = {0};
    if (eg_title_index_load(&part, path)) return -1;
    size_t total = e->titles.count + part.count;
    if (total > 100000) {
        eg_title_index_free(&part);
        return search_error("embedding title count exceeds 100000", path);
    }
    EgTitle *titles = rt_malloc_array(total, sizeof(*titles));
    if (!titles) { eg_title_index_free(&part); return search_error("cannot merge title vectors", path); }
    if (e->titles.count) memcpy(titles, e->titles.titles, e->titles.count * sizeof(*titles));
    memcpy(titles + e->titles.count, part.titles, part.count * sizeof(*titles));
    rt_free(e->titles.titles);
    rt_free(part.titles);
    e->titles.titles = titles;
    e->titles.count = total;
    for (size_t i = 0; i < 100; i++) {
        e->titles.years[i] += part.years[i];
        e->titles.year_present[i] |= part.year_present[i];
    }
    return 0;
}

static int map_records(SearchEmbedding *e, const SearchIndex *index) {
    e->title_records = rt_malloc_array(e->titles.count, sizeof(*e->title_records));
    e->passage_records = rt_malloc_array(e->sections.count ? e->sections.count : 1, sizeof(*e->passage_records));
    if (!e->title_records || !e->passage_records) return search_error("cannot allocate embedding source map", 0);
    for (size_t i = 0; i < e->titles.count; i++) {
        const EgTitle *t = e->titles.titles + i;
        const SearchRecord *r = search_find(index, t->id, 0);
        if (!r || rt_strcmp(index->strings + r->kind, "Paper") ||
            rt_strcmp(index->strings + r->name, t->title) ||
            rt_strcmp(index->strings + r->reference, t->reference) || r->year != t->year)
            return search_error("embedding title does not match this OParl snapshot", t->id);
        e->title_records[i] = (uint32_t)(r - index->records);
    }
    uint32_t *files = rt_malloc_array(e->sections.document_count ? e->sections.document_count : 1, sizeof(*files));
    if (!files) return search_error("cannot allocate main-form source map", 0);
    int status = -1;
    for (size_t i = 0; i < e->sections.document_count; i++) {
        const EgContentDocument *d = e->sections.documents + i;
        files[i] = NONE;
        if (!d->count) continue;
        const SearchRecord *paper = search_find(index, d->id, 0);
        if (!paper) { search_error("embedded main form is not in this snapshot", d->id); goto done; }
        for (size_t j = 0; j < index->relation_count; j++) {
            const SearchRelation *relation = index->relations + j;
            if (relation->source != (uint32_t)(paper - index->records) || relation->role != SEARCH_MAIN_FILE) continue;
            uint32_t file = index->relation_targets[j];
            if (file == NONE) continue;
            uint32_t version = index->version_lookup[file];
            if (version != NONE && !rt_strcmp(index->strings + index->versions[version].hash, d->pdf_sha)) {
                if (files[i] != NONE && files[i] != file) {
                    search_error("ambiguous embedded main-file source", d->id); goto done;
                } else files[i] = file;
            }
        }
        if (files[i] == NONE) { search_error("embedded PDF version does not match this snapshot", d->id); goto done; }
        CryptoSha256Context hash;
        unsigned char digest[32];
        char joined_sha[65];
        crypto_sha256_init(&hash);
        const char *file_id = index->strings + index->records[files[i]].id;
        for (size_t page_number = 1; page_number <= d->pages; page_number++) {
            const SearchRecord *page = search_find(index, file_id, (unsigned int)page_number);
            if (!page || page->parent != files[i] || page->warnings) {
                search_error("embedded main-form page is missing or has extraction warnings", file_id); goto done;
            }
            if (page_number > 1) crypto_sha256_update(&hash, (const unsigned char *)"\n\n", 2);
            crypto_sha256_update(&hash, (const unsigned char *)index->strings + page->text + page->body_start,
                                 page->text_length - page->body_start);
        }
        crypto_sha256_final(&hash, digest); hash_to_hex(digest, sizeof(digest), joined_sha);
        if (rt_strcmp(joined_sha, d->text_sha)) {
            search_error("embedded main-form text does not match indexed PDF page text", d->id); goto done;
        }
    }
    for (size_t i = 0; i < e->sections.count; i++) {
        const EgPassage *p = e->sections.passages + i;
        const SearchRecord *file = index->records + files[p->document];
        const SearchRecord *page = search_find(index, index->strings + file->id, (unsigned int)p->page_first);
        const SearchRecord *last = search_find(index, index->strings + file->id, (unsigned int)p->page_last);
        if (!page || !last || page->parent != files[p->document] || last->parent != files[p->document]) {
            search_error("embedded PDF page is unavailable in this snapshot", index->strings + file->id); goto done;
        }
        e->passage_records[i] = (uint32_t)(page - index->records);
    }
    status = 0;
done:
    rt_free(files);
    return status;
}

static int load_years(SearchEmbedding *e, const SearchIndex *index, const char *directory,
                      const ToolJson *json, size_t years_index) {
    const ToolJsonToken *years = tool_json_token(json, years_index);
    char section_paths[100][4096];
    const char *sections[100];
    size_t count = 0;
    unsigned char present[100] = {0};
    if (!years || years->kind != TOOL_JSON_ARRAY) return search_error("embedding years must be an array", directory);
    for (size_t i = years_index + 1; i < years->next; i = tool_json_token(json, i)->next) {
        unsigned long long year;
        char base[4096], path[4096], expected[65], actual[65], year_text[32];
        if (count == 100 || tool_json_uint(json, tool_json_get(json, i, "year"), &year) ||
            year < 2000 || year > 2099 || present[year - 2000])
            return search_error("invalid or duplicate embedding year", directory);
        present[year - 2000] = 1;
        rt_unsigned_to_string(year, year_text, sizeof(year_text));
        if (rt_join_path(directory, year_text, base, sizeof(base)) ||
            rt_join_path(base, "titles", path, sizeof(path)) ||
            tool_json_string(json, tool_json_get(json, i, "titlesSha256"), expected, sizeof(expected)) ||
            path_hash(path, "manifest.json", actual) || rt_strcmp(actual, expected) ||
            append_titles(e, path)) return search_error("invalid or modified embedding titles", base);
        if (rt_join_path(base, "sections", section_paths[count], sizeof(section_paths[count])) ||
            tool_json_string(json, tool_json_get(json, i, "sectionsSha256"), expected, sizeof(expected)) ||
            path_hash(section_paths[count], "manifest.json", actual) || rt_strcmp(actual, expected))
            return search_error("invalid or modified embedding sections", base);
        sections[count] = section_paths[count];
        count++;
    }
    if (!count) return search_error("embedding index contains no years", directory);
    rt_sort(e->titles.titles, e->titles.count, sizeof(*e->titles.titles), title_order);
    for (size_t i = 1; i < e->titles.count; i++)
        if (!rt_strcmp(e->titles.titles[i - 1].id, e->titles.titles[i].id))
            return search_error("duplicate embedded Paper", e->titles.titles[i].id);
    if (eg_section_indexes_load(&e->sections, sections, count, &e->titles)) return -1;
    return map_records(e, index);
}

int search_embedding_open(SearchIndex *index, const char *directory, const char *model, unsigned int threads) {
    unsigned char *bytes = 0;
    size_t size;
    char path[4096], root[4096], before[65], after[65];
    ToolJson json = {0};
    SearchEmbedding *e = rt_malloc(sizeof(*e));
    if (!e) return search_error("cannot allocate embedding state", 0);
    rt_memset(e, 0, sizeof(*e));
    platform_mutex_init(&e->inference_lock);
    if (rt_join_path(directory, ".oparl-embedding.lock", path, sizeof(path))) goto invalid;
    PlatformDirEntry info;
    if (!platform_get_path_info(path, &info)) {
        search_error("embedding index is being built; wait for publication", directory); goto done;
    }
    if (rt_join_path(directory, "manifest.json", path, sizeof(path)) ||
        eg_read_regular(path, 65536, &bytes, &size) || tool_json_parse(&json, (const char *)bytes, size) ||
        !tool_json_equals(&json, tool_json_get(&json, 0, "format"), "oparl-embeddings-v1") ||
        !tool_json_equals(&json, tool_json_get(&json, 0, "snapshotId"), index->snapshot_id) ||
        !tool_json_equals(&json, tool_json_get(&json, 0, "modelRevision"), EG2_MODEL_REVISION) ||
        threads < 1 || threads > 32 || rt_strlen(model) >= sizeof(e->model_directory)) goto invalid;
    rt_copy_string(e->model_directory, sizeof(e->model_directory), model);
    e->threads = threads;
    unsigned char digest[32];
    crypto_sha256_hash(bytes, size, digest); hash_to_hex(digest, sizeof(digest), before);
    if (generation_root(&json, directory, root) ||
        load_years(e, index, root, &json, tool_json_get(&json, 0, "years"))) goto done;
    if (rt_join_path(model, "tokenizer.eg2", path, sizeof(path)) || eg_tokenizer_load(&e->tokenizer, path)) goto done;
    if (path_hash(directory, "manifest.json", after) || rt_strcmp(before, after) ||
        rt_join_path(directory, ".oparl-embedding.lock", path, sizeof(path)) ||
        !platform_get_path_info(path, &info)) {
        search_error("embedding publication changed while loading; retry after completion", directory); goto done;
    }
    index->embedding = e;
    e = 0;
    tool_json_free(&json); rt_free(bytes);
    return 0;
invalid:
    search_error("invalid embedding index/model or mismatched OParl snapshot", directory);
done:
    tool_json_free(&json); rt_free(bytes); embedding_free(e);
    return -1;
}

static int number_field(ToolByteBuffer *out, const char *name, unsigned long long value) {
    return tool_byte_buffer_append_cstr(out, ",") || tool_json_append_string(out, name) ||
        tool_byte_buffer_append_cstr(out, ":") || tool_byte_buffer_append_uint(out, value);
}

int search_embedding_status_json(ToolByteBuffer *out, const SearchIndex *index) {
    const SearchEmbedding *e = index->embedding;
    if (!e) return tool_byte_buffer_append_cstr(out, "{\"enabled\":false}");
    if (tool_byte_buffer_append_cstr(out, "{\"enabled\":true,\"scope\":\"titles-and-numbered-main-forms\",\"modelRevision\":") ||
        tool_json_append_string(out, EG2_MODEL_REVISION) ||
        number_field(out, "titles", e->titles.count) || number_field(out, "passages", e->sections.count) ||
        number_field(out, "includedMainForms", e->sections.included) ||
        number_field(out, "excludedMainForms", e->sections.excluded) ||
        tool_byte_buffer_append_cstr(out, ",\"modelLoaded\":") ||
        tool_byte_buffer_append_cstr(out, __atomic_load_n(&e->loaded, __ATOMIC_ACQUIRE) ? "true" : "false") ||
        tool_byte_buffer_append_cstr(out, ",\"years\":[")) return -1;
    int comma = 0;
    for (size_t year = 0; year < 100; year++) if (e->titles.year_present[year]) {
        size_t included = 0, excluded = 0, passages = 0;
        for (size_t i = 0; i < e->sections.document_count; i++) {
            const EgContentDocument *d = e->sections.documents + i;
            if (d->year != year + 2000) continue;
            included += d->count != 0; excluded += d->count == 0; passages += d->count;
        }
        if ((comma && tool_byte_buffer_append_cstr(out, ",")) ||
            tool_byte_buffer_append_cstr(out, "{\"scope\":\"reference-year\"") ||
            number_field(out, "year", year + 2000) || number_field(out, "titles", e->titles.years[year]) ||
            number_field(out, "includedMainForms", included) || number_field(out, "excludedMainForms", excluded) ||
            number_field(out, "passages", passages) || tool_byte_buffer_append_cstr(out, "}")) return -1;
        comma = 1;
    }
    return tool_byte_buffer_append_cstr(out, "]}");
}

int search_embedding_source_json(ToolByteBuffer *out, const SearchIndex *index, const SearchHit *hit) {
    char score[32], fraction[7], whole[32];
    float value = hit->semantic_score;
    if (value <= -2) return tool_byte_buffer_append_cstr(out, "null");
    unsigned long long scaled = (unsigned long long)((value < 0 ? -value : value) * 1000000 + 0.5);
    rt_unsigned_to_string(scaled / 1000000, whole, sizeof(whole));
    unsigned long long remainder = scaled % 1000000;
    for (int i = 5; i >= 0; i--) { fraction[i] = (char)('0' + remainder % 10); remainder /= 10; }
    fraction[6] = 0;
    size_t n = tool_buffer_append_cstr(score, sizeof(score), 0, value < 0 ? "-" : "");
    n = tool_buffer_append_cstr(score, sizeof(score), n, whole);
    n = tool_buffer_append_char(score, sizeof(score), n, '.');
    tool_buffer_append_cstr(score, sizeof(score), n, fraction);
    if (tool_byte_buffer_append_cstr(out, "{\"cosine\":") || tool_byte_buffer_append_cstr(out, score) ||
        tool_byte_buffer_append_cstr(out, ",\"source\":") ||
        tool_json_append_string(out, hit->semantic_text ? "main-form-passage" : "paper-title")) return -1;
    if (hit->semantic_text) {
        const SearchEmbedding *e = index->embedding;
        const EgPassage *p = 0;
        for (size_t i = 0; i < e->sections.count; i++)
            if (e->sections.passages[i].text == hit->semantic_text) { p = e->sections.passages + i; break; }
        if (!p) return search_error("semantic passage provenance was lost", 0);
        const EgContentDocument *d = e->sections.documents + p->document;
        if (tool_byte_buffer_append_cstr(out, ",\"chunkId\":") || tool_json_append_string(out, p->id) ||
            number_field(out, "pageFirst", p->page_first) || number_field(out, "pageLast", p->page_last) ||
            number_field(out, "joinedTextStart", p->start) || number_field(out, "joinedTextEnd", p->end) ||
            tool_byte_buffer_append_cstr(out, ",\"joinedTextSha256\":") || tool_json_append_string(out, d->text_sha) ||
            tool_byte_buffer_append_cstr(out, ",\"pdfSha256\":") || tool_json_append_string(out, d->pdf_sha)) return -1;
    }
    return tool_byte_buffer_append_cstr(out, "}");
}

int search_embedding_scores(const SearchIndex *index, const char *query, float *scores,
                            const char **texts, const char **error) {
    SearchEmbedding *e = index->embedding;
    unsigned int tokens[EG2_MAX_TOKENS], count;
    float vector[EG2_DIM];
    char input[4096 + sizeof(EG2_QUERY_PREFIX)], path[4096];
    size_t size = query ? rt_strlen(query) : 0;
    *error = "semantic query must be valid UTF-8, nonblank, <=4096 bytes and <=256 tokens including its prefix";
    int nonblank = 0;
    for (size_t i = 0; i < size; i++) nonblank |= !rt_is_space(query[i]);
    if (!nonblank || size > 4096 || rt_utf8_validate(query, size)) return SEARCH_QUERY_INVALID;
    memcpy(input, EG2_QUERY_PREFIX, sizeof(EG2_QUERY_PREFIX) - 1);
    memcpy(input + sizeof(EG2_QUERY_PREFIX) - 1, query, size);
    if (eg_tokenize(&e->tokenizer, (const unsigned char *)input, size + sizeof(EG2_QUERY_PREFIX) - 1,
                    tokens, &count) || count > 256) return SEARCH_QUERY_INVALID;
    *error = "cannot load or run the verified native embedding model";
    platform_mutex_lock(&e->inference_lock);
    if (!e->loaded) {
        if (rt_join_path(e->model_directory, "weights.eg2", path, sizeof(path)) ||
            eg_model_load(&e->model, path) || eg_model_set_threads(&e->model, e->threads)) {
            eg_model_free(&e->model);
            platform_mutex_unlock(&e->inference_lock);
            return SEARCH_QUERY_RESOURCE;
        }
        __atomic_store_n(&e->loaded, 1, __ATOMIC_RELEASE);
    }
    int status = eg_embed(&e->model, tokens, count, vector, 0);
    platform_mutex_unlock(&e->inference_lock);
    if (status) return SEARCH_QUERY_RESOURCE;
    for (size_t i = 0; i < index->record_count; i++) { scores[i] = -2; texts[i] = 0; }
    for (size_t i = 0; i < e->titles.count; i++) {
        double score = 0;
        for (size_t j = 0; j < EG2_DIM; j++) score += (double)vector[j] * e->titles.titles[i].vector[j];
        scores[e->title_records[i]] = (float)score;
    }
    for (size_t i = 0; i < e->sections.count; i++) {
        double score = 0;
        for (size_t j = 0; j < EG2_DIM; j++) score += (double)vector[j] * e->sections.passages[i].vector[j];
        uint32_t record = e->passage_records[i];
        if (score > scores[record]) {
            scores[record] = (float)score;
            texts[record] = e->sections.passages[i].text;
        }
    }
    *error = 0;
    return 0;
}

int search_embedding_build(const char *path, const char *directory, const char *model,
                           const char *prepared, const SearchEmbeddingArchive *archives,
                           size_t count, unsigned int threads) {
    SearchIndex index = {0};
    ToolByteBuffer manifest = {0};
    SearchEmbedding *verify = 0;
    char lock[4096], output[4096], year_text[32], thread_text[32], target[4096];
    char generation[128], generation_directory[4096], previous[4096], base[4096], stamp[32];
    int result = -1, fd = -1;
    if (!prepared || !count || count > 100 || threads < 1 || threads > 32)
        return search_error("embedding build requires --prepared, 1..100 reference-year archives and 1..32 threads", directory);
    if (eg_ensure_directory(directory) || rt_join_path(directory, ".oparl-embedding.lock", lock, sizeof(lock))) return -1;
    fd = platform_open_create_exclusive(lock, 0600);
    if (fd < 0) return search_error("embedding pipeline is locked; verify its owner before removing the lock", lock);
    char pid[32]; rt_unsigned_to_string((unsigned int)platform_get_process_id(), pid, sizeof(pid));
    int failed = rt_write_cstr(fd, pid);
    if (platform_close(fd)) failed = -1;
    fd = -1;
    if (failed) { search_error("cannot write pipeline lock", lock); goto done; }
    rt_unsigned_to_string(platform_get_monotonic_time_ns(), stamp, sizeof(stamp));
    size_t n = tool_buffer_append_cstr(generation, sizeof(generation), 0, pid);
    n = tool_buffer_append_char(generation, sizeof(generation), n, '-');
    tool_buffer_append_cstr(generation, sizeof(generation), n, stamp);
    if (rt_join_path(directory, "generations", base, sizeof(base)) ||
        rt_join_path(base, generation, generation_directory, sizeof(generation_directory))) {
        search_error("embedding generation path is too long", directory); goto done;
    }
    rt_copy_string(previous, sizeof(previous), directory);
    if (rt_join_path(directory, "manifest.json", target, sizeof(target))) goto done;
    PlatformDirEntry info;
    if (!platform_get_path_info(target, &info)) {
        unsigned char *bytes = 0; size_t size;
        ToolJson prior = {0};
        if (eg_read_regular(target, 65536, &bytes, &size) || tool_json_parse(&prior, (const char *)bytes, size)) {
            rt_free(bytes); tool_json_free(&prior); goto done;
        }
        int valid = generation_root(&prior, directory, previous);
        rt_free(bytes); tool_json_free(&prior);
        if (valid) goto done;
    }
    rt_unsigned_to_string(threads, thread_text, sizeof(thread_text));
    if (search_open(&index, path) || tool_byte_buffer_append_cstr(&manifest,
        "{\"format\":\"oparl-embeddings-v1\",\"snapshotId\":") ||
        tool_json_append_string(&manifest, index.snapshot_id) ||
        tool_byte_buffer_append_cstr(&manifest, ",\"modelRevision\":") ||
        tool_json_append_string(&manifest, EG2_MODEL_REVISION) ||
        tool_byte_buffer_append_cstr(&manifest, ",\"generation\":") ||
        tool_json_append_string(&manifest, generation) ||
        tool_byte_buffer_append_cstr(&manifest, ",\"years\":[")) goto done;
    for (size_t i = 0; i < count; i++) {
        char titles[4096], sections[4096], title_sha[65], section_sha[65];
        if (archives[i].year < 2000 || archives[i].year > 2099) {
            search_error("embedding archives require an explicit reference year", archives[i].path); goto done;
        }
        rt_unsigned_to_string(archives[i].year, year_text, sizeof(year_text));
        if (rt_join_path(generation_directory, year_text, output, sizeof(output)) ||
            rt_join_path(output, "titles", titles, sizeof(titles)) ||
            rt_join_path(output, "sections", sections, sizeof(sections))) goto done;
        char previous_year[4096], source[4096];
        if (rt_join_path(previous, year_text, previous_year, sizeof(previous_year)) ||
            rt_join_path(previous_year, "titles", source, sizeof(source)) || reuse_cache(source, titles) ||
            rt_join_path(previous_year, "sections", source, sizeof(source)) || reuse_cache(source, sections)) goto done;
        char *title_args[] = {"oparl-index titles", "--archive", (char *)archives[i].path,
            "--year", year_text, "--output", titles, "--model", (char *)model, "--threads", thread_text};
        if (eg_title_index_main(11, title_args)) goto done;
        char *section_args[] = {"oparl-index sections", "--papers", titles, "--prepared", (char *)prepared,
            "--output", sections, "--model", (char *)model, "--threads", thread_text,
            "--timeout-seconds", "3600"};
        if (eg_section_index_main(13, section_args) || path_hash(titles, "manifest.json", title_sha) ||
            path_hash(sections, "manifest.json", section_sha)) goto done;
        if ((i && tool_byte_buffer_append_char(&manifest, ',')) ||
            tool_byte_buffer_append_cstr(&manifest, "{\"year\":") ||
            tool_byte_buffer_append_cstr(&manifest, year_text) ||
            tool_byte_buffer_append_cstr(&manifest, ",\"titlesSha256\":") ||
            tool_json_append_string(&manifest, title_sha) ||
            tool_byte_buffer_append_cstr(&manifest, ",\"sectionsSha256\":") ||
            tool_json_append_string(&manifest, section_sha) ||
            tool_byte_buffer_append_char(&manifest, '}')) goto done;
    }
    if (tool_byte_buffer_append_cstr(&manifest, "]}\n")) goto done;
    ToolJson json = {0};
    verify = rt_malloc(sizeof(*verify));
    if (!verify) { search_error("cannot allocate embedding publication validation", 0); goto done; }
    rt_memset(verify, 0, sizeof(*verify));
    if (tool_json_parse(&json, (const char *)manifest.data, manifest.size)) goto done;
    int valid = load_years(verify, &index, generation_directory, &json, tool_json_get(&json, 0, "years"));
    tool_json_free(&json);
    if (valid || rt_join_path(directory, "manifest.json", target, sizeof(target)) ||
        eg_atomic_write(target, manifest.data, manifest.size)) goto done;
    result = index.incomplete ? 3 : 0;
done:
    if (fd >= 0) platform_close(fd);
    if (platform_remove_file(lock)) { search_error("cannot remove embedding pipeline lock", lock); result = -1; }
    embedding_free(verify);
    search_close(&index);
    tool_byte_buffer_free(&manifest);
    return result;
}

#include "search.h"
#include "sections.h"
#include "index_io.h"
#include "hash_util.h"
#include "tool_json_parser.h"

static int boolean(const ToolJson *json, size_t object, const char *name, int *out) {
    size_t i = tool_json_get(json, object, name);
    const ToolJsonToken *token = tool_json_token(json, i);
    if (!token || token->kind != TOOL_JSON_BOOL) return -1;
    *out = tool_json_true(json, i);
    return 0;
}

static int digest(const ToolJson *json, size_t object, const char *name, char text[65]) {
    unsigned char bytes[32];
    return tool_json_string(json, tool_json_get(json, object, name), text, 65) || eg_parse_digest(text, bytes);
}

static int number(const ToolJson *json, size_t object, const char *name, size_t *out) {
    unsigned long long value;
    if (tool_json_uint(json, tool_json_get(json, object, name), &value) || value > 64U * 1024U * 1024U) return -1;
    *out = (size_t)value;
    return 0;
}

static size_t title_position(const EgTitleIndex *titles, const char *id) {
    size_t low = 0, high = titles->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int order = rt_strcmp(titles->titles[mid].id, id);
        if (!order) return mid;
        if (order < 0) low = mid + 1; else high = mid;
    }
    return (size_t)-1;
}

static int compare_document(const void *a, const void *b) {
    const EgContentDocument *const *left = a, *const *right = b;
    return rt_strcmp((*left)->id, (*right)->id);
}

void eg_section_index_free(EgSectionIndex *index) {
    for (size_t i = 0; i < index->document_count; i++) {
        EgContentDocument *d = index->documents + i;
        rt_free(d->id); rt_free(d->title); rt_free(d->reference);
    }
    for (size_t i = 0; i < index->count; i++) {
        rt_free(index->passages[i].id); rt_free(index->passages[i].text);
    }
    rt_free(index->documents); rt_free(index->passages);
    rt_memset(index, 0, sizeof(*index));
}

int eg_section_index_load(EgSectionIndex *index, const char *directory, const EgTitleIndex *titles) {
    ToolJson json = {0};
    unsigned char *bytes = 0;
    char path[4096];
    size_t size, count, selected, included, excluded, year, dimensions, limit, overlap;
    EgContentDocument **ordered = 0;
    ToolByteBuffer joined = {0};
    int result = -1;
    rt_memset(index, 0, sizeof(*index));
    if (rt_join_path(directory, "manifest.json", path, sizeof(path)) ||
        eg_read_regular(path, 64U * 1024U * 1024U, &bytes, &size) ||
        tool_json_parse(&json, (const char *)bytes, size)) goto invalid;
    if (!tool_json_equals_alloc(&json, tool_json_get(&json, 0, "format"), "eg2-paper-section-index-v1") ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "model_revision"), EG2_MODEL_REVISION) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "weights_sha256"), EG2_WEIGHTS_SHA256) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "tokenizer_sha256"), EG2_TOKENIZER_SHA256) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "task"), "document") ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "prefix"), EG2_DOCUMENT_PREFIX) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "section_key_recipe"), EG_SECTION_RECIPE) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "boundary_recipe"), "exact-reference-numbered-main-form-v1") ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "chunk_recipe"), "utf8-2400bytes-800tokens-sentence-end-overlap80-v1") ||
        number(&json, 0, "count", &count) || count > 100000 ||
        number(&json, 0, "documents_selected", &selected) || !selected || selected > 100000 ||
        number(&json, 0, "documents_included", &included) || included > selected ||
        number(&json, 0, "documents_excluded", &excluded) || included + excluded != selected ||
        number(&json, 0, "year", &year) || year < 2000 || year > 2099 ||
        number(&json, 0, "dimensions", &dimensions) || dimensions != EG2_DIM ||
        number(&json, 0, "max_tokens", &limit) || limit != EG_SECTION_LIMIT ||
        number(&json, 0, "max_overlap_text_tokens", &overlap) || overlap != EG_SECTION_OVERLAP) goto invalid;
    index->documents = rt_malloc_array(selected, sizeof(*index->documents));
    index->passages = rt_malloc_array(count ? count : 1, sizeof(*index->passages));
    ordered = rt_malloc_array(selected, sizeof(*ordered));
    if (!index->documents || !index->passages || !ordered) {
        eg_error("cannot allocate resident section index", 0); goto done;
    }
    rt_memset(index->documents, 0, selected * sizeof(*index->documents));
    rt_memset(index->passages, 0, count * sizeof(*index->passages));
    index->document_count = selected; index->count = count; index->year = (unsigned int)year;
    index->year_present[year - 2000] = 1;
    index->included = included; index->excluded = excluded; index->combined_count = titles->count;
    size_t documents_index = tool_json_get(&json, 0, "documents"), n = 0, actual_included = 0, planned = 0;
    const ToolJsonToken *documents = tool_json_token(&json, documents_index);
    if (!documents || documents->kind != TOOL_JSON_ARRAY) goto invalid;
    for (size_t i = documents_index + 1; i < documents->next; i = tool_json_token(&json, i)->next) {
        if (n == selected) goto invalid;
        EgContentDocument *d = index->documents + n;
        d->year = (unsigned int)year;
        ordered[n++] = d;
        d->id = tool_json_dup_string(&json, tool_json_get(&json, i, "id"), 4096);
        d->title = tool_json_dup_string(&json, tool_json_get(&json, i, "title"), EG2_MAX_TEXT);
        d->reference = tool_json_dup_string(&json, tool_json_get(&json, i, "reference"), 512);
        char prefix[4] = {(char)('0' + year % 100 / 10), (char)('0' + year % 10), '/', 0};
        if (!d->id || !d->id[0] || !d->title || !d->title[0] || !d->reference ||
            rt_strncmp(d->reference, prefix, 3) || number(&json, i, "first_chunk", &d->first) ||
            number(&json, i, "chunks", &d->count) || d->count > count ||
            number(&json, i, "main_form_pages", &d->pages) ||
            boolean(&json, i, "separate_reasoning_attachment", &d->separate_reasoning)) goto invalid;
        d->title_index = title_position(titles, d->id);
        if (d->title_index != (size_t)-1) {
            const EgTitle *t = titles->titles + d->title_index;
            if (t->year != year || rt_strcmp(t->title, d->title) || rt_strcmp(t->reference, d->reference)) goto invalid;
        }
        if (d->count) {
            if (d->first != planned || !d->pages ||
                !tool_json_equals_alloc(&json, tool_json_get(&json, i, "coverage"), "numbered-main-form-only") ||
                digest(&json, i, "pdf_sha256", d->pdf_sha) ||
                digest(&json, i, "joined_text_sha256", d->text_sha)) goto invalid;
            planned += d->count; actual_included++;
            if (d->title_index == (size_t)-1) index->combined_count++;
        }
    }
    if (n != selected || actual_included != included || planned != count) goto invalid;
    rt_sort(ordered, selected, sizeof(*ordered), compare_document);
    for (size_t i = 1; i < selected; i++) if (!rt_strcmp(ordered[i - 1]->id, ordered[i]->id)) goto invalid;
    size_t items_index = tool_json_get(&json, 0, "items");
    const ToolJsonToken *items = tool_json_token(&json, items_index);
    if (!items || items->kind != TOOL_JSON_ARRAY) goto invalid;
    n = 0;
    for (size_t i = items_index + 1; i < items->next; i = tool_json_token(&json, i)->next) {
        if (n == count) goto invalid;
        EgPassage *p = index->passages + n;
        size_t ordinal, tokens;
        char key_text[65], stored[80], relative[80], identity[65];
        unsigned char key[32];
        if (number(&json, i, "document", &p->document) || p->document >= selected ||
            number(&json, i, "ordinal", &ordinal) ||
            number(&json, i, "start", &p->start) || number(&json, i, "end", &p->end) ||
            p->end <= p->start || p->end - p->start > 2400 ||
            number(&json, i, "page_first", &p->page_first) || !p->page_first ||
            number(&json, i, "page_last", &p->page_last) || p->page_last < p->page_first ||
            number(&json, i, "tokens", &tokens) || tokens < 2 || tokens > EG_SECTION_LIMIT) goto invalid;
        const EgContentDocument *d = index->documents + p->document;
        if (n < d->first || n >= d->first + d->count || ordinal != n - d->first + 1 || p->page_last > d->pages ||
            !tool_json_equals_alloc(&json, tool_json_get(&json, i, "paper_id"), d->id)) goto invalid;
        p->id = tool_json_dup_string(&json, tool_json_get(&json, i, "id"), 64);
        p->text = tool_json_dup_string(&json, tool_json_get(&json, i, "text"), 2400);
        if (!p->id || !p->text || rt_strlen(p->text) != p->end - p->start ||
            rt_utf8_validate(p->text, rt_strlen(p->text))) goto invalid;
        eg_section_identity(d->id, d->pdf_sha, d->text_sha, p->start, p->end, identity);
        if (rt_strcmp(p->id, identity)) goto invalid;
        CryptoSha256Context hash;
        crypto_sha256_init(&hash);
        crypto_sha256_update(&hash, (const unsigned char *)EG_SECTION_RECIPE, sizeof(EG_SECTION_RECIPE) - 1);
        crypto_sha256_update(&hash, (const unsigned char *)p->text, rt_strlen(p->text));
        crypto_sha256_final(&hash, key); hash_to_hex(key, sizeof(key), key_text);
        if (!tool_json_equals_alloc(&json, tool_json_get(&json, i, "key"), key_text)) goto invalid;
        eg_title_vector_path(key, relative);
        if (tool_json_string(&json, tool_json_get(&json, i, "vector"), stored, sizeof(stored)) || rt_strcmp(relative, stored) ||
            rt_join_path(directory, relative, path, sizeof(path))) goto invalid;
        int found;
        if (eg_vector_read(path, key, p->vector, &found)) goto done;
        if (!found) { eg_error("missing indexed section vector: ", path); goto done; }
        if (ordinal == 1) {
            if (p->start) goto invalid;
            joined.size = 0;
        }
        if (p->start > joined.size || p->end <= joined.size ||
            (ordinal > 1 && p->start <= index->passages[n - 1].start)) goto invalid;
        size_t shared = joined.size - p->start;
        if (shared && memcmp(joined.data + p->start, p->text, shared)) goto invalid;
        if (tool_byte_buffer_append(&joined, p->text + shared, rt_strlen(p->text) - shared)) {
            eg_error("cannot reconstruct section provenance", 0); goto done;
        }
        if (ordinal == d->count) {
            unsigned char sha[32]; char text_sha[65];
            crypto_sha256_hash(joined.data, joined.size, sha); hash_to_hex(sha, sizeof(sha), text_sha);
            if (rt_strcmp(d->text_sha, text_sha)) goto invalid;
        }
        n++;
    }
    if (n != count) goto invalid;
    result = 0;
    goto done;
invalid:
    eg_error("invalid, incompatible or inconsistent section index: ", directory);
done:
    rt_free(ordered); rt_free(bytes); tool_json_free(&json); tool_byte_buffer_free(&joined);
    if (result) eg_section_index_free(index);
    return result;
}

int eg_section_indexes_load(EgSectionIndex *index, const char *const *directories, size_t count,
                            const EgTitleIndex *titles) {
    EgSectionIndex part = {0};
    EgContentDocument **ordered = 0;
    int result = -1;
    rt_memset(index, 0, sizeof(*index));
    if (!count || count > 100) return eg_error("section index count must be 1..100", 0);
    if (count == 1) return eg_section_index_load(index, directories[0], titles);
    index->combined_count = titles->count;
    for (size_t i = 0; i < count; i++) {
        if (eg_section_index_load(&part, directories[i], titles)) goto done;
        if (index->year_present[part.year - 2000]) {
            eg_error("duplicate section index year: ", directories[i]); goto done;
        }
        size_t documents = index->document_count + part.document_count;
        size_t passages = index->count + part.count;
        if (documents > 100000 || passages > 100000) {
            eg_error("combined section index exceeds 100000 documents or passages", 0); goto done;
        }
        EgContentDocument *merged_documents = rt_malloc_array(documents, sizeof(*merged_documents));
        EgPassage *merged_passages = rt_malloc_array(passages ? passages : 1, sizeof(*merged_passages));
        if (!merged_documents || !merged_passages) {
            rt_free(merged_documents); rt_free(merged_passages);
            eg_error("cannot allocate combined section index", 0); goto done;
        }
        if (index->document_count)
            memcpy(merged_documents, index->documents, index->document_count * sizeof(*merged_documents));
        if (index->count)
            memcpy(merged_passages, index->passages, index->count * sizeof(*merged_passages));
        for (size_t j = 0; j < part.document_count; j++) {
            part.documents[j].first += index->count;
            merged_documents[index->document_count + j] = part.documents[j];
        }
        for (size_t j = 0; j < part.count; j++) {
            part.passages[j].document += index->document_count;
            merged_passages[index->count + j] = part.passages[j];
        }
        rt_free(index->documents); rt_free(index->passages);
        index->documents = merged_documents; index->passages = merged_passages;
        index->document_count = documents; index->count = passages;
        index->included += part.included; index->excluded += part.excluded;
        index->combined_count += part.combined_count - titles->count;
        index->year_present[part.year - 2000] = 1;
        rt_free(part.documents); rt_free(part.passages);
        rt_memset(&part, 0, sizeof(part));
    }
    ordered = rt_malloc_array(index->document_count, sizeof(*ordered));
    if (!ordered) { eg_error("cannot validate combined section identities", 0); goto done; }
    for (size_t i = 0; i < index->document_count; i++) ordered[i] = index->documents + i;
    rt_sort(ordered, index->document_count, sizeof(*ordered), compare_document);
    for (size_t i = 1; i < index->document_count; i++) {
        if (!rt_strcmp(ordered[i - 1]->id, ordered[i]->id)) {
            eg_error("duplicate Paper across section indexes: ", ordered[i]->id); goto done;
        }
    }
    result = 0;
done:
    rt_free(ordered); eg_section_index_free(&part);
    if (result) eg_section_index_free(index);
    return result;
}

size_t eg_search_count(const EgTitleIndex *titles, const EgSectionIndex *sections, EgSearchMode mode) {
    return mode == EG_SEARCH_TITLE ? titles->count :
        mode == EG_SEARCH_CONTENT ? sections->included : sections->combined_count;
}

static double score_vector(const float query[EG2_DIM], const float vector[EG2_DIM]) {
    double score = 0;
    for (size_t i = 0; i < EG2_DIM; i++) score += (double)query[i] * vector[i];
    return score;
}

static int compare_result(const void *a, const void *b) {
    const EgResult *left = a, *right = b;
    return left->score > right->score ? -1 : left->score < right->score ? 1 : rt_strcmp(left->id, right->id);
}

int eg_search_rank(const EgTitleIndex *titles, const EgSectionIndex *sections, EgSearchMode mode,
                   const float query[EG2_DIM], size_t offset,
                   EgResult hits[EG_SEARCH_RESULTS], unsigned int *count) {
    size_t total = eg_search_count(titles, sections, mode), n = 0;
    EgResult *ranked = rt_malloc_array(total, sizeof(*ranked));
    size_t *positions = rt_malloc_array(sections->document_count ? sections->document_count : 1, sizeof(*positions));
    int result = -1;
    *count = 0;
    if (!ranked || !positions) { eg_error("cannot allocate grouped search ranking", 0); goto done; }
    if (offset >= total) { eg_error("search page is outside this index", 0); goto done; }
    if (mode != EG_SEARCH_CONTENT) {
        for (size_t i = 0; i < titles->count; i++) {
            const EgTitle *t = titles->titles + i;
            ranked[n++] = (EgResult){t->id, t->title, t->reference, t->year,
                                     score_vector(query, t->vector), 0, 0};
        }
    }
    if (mode != EG_SEARCH_TITLE) {
        for (size_t i = 0; i < sections->document_count; i++) {
            const EgContentDocument *d = sections->documents + i;
            positions[i] = (size_t)-1;
            if (!d->count) continue;
            if (mode == EG_SEARCH_BOTH && d->title_index != (size_t)-1) positions[i] = d->title_index;
            else {
                positions[i] = n;
                ranked[n++] = (EgResult){d->id, d->title, d->reference, d->year, -2, 0, 0};
            }
        }
        for (size_t i = 0; i < sections->count; i++) {
            const EgPassage *p = sections->passages + i;
            EgResult *hit = ranked + positions[p->document];
            double score = score_vector(query, p->vector);
            if (score > hit->score) {
                hit->score = score; hit->passage = p;
                hit->separate_reasoning = sections->documents[p->document].separate_reasoning;
            }
        }
    }
    if (n != total) { eg_error("grouped search counts are inconsistent", 0); goto done; }
    rt_sort(ranked, total, sizeof(*ranked), compare_result);
    size_t remaining = total - offset;
    *count = (unsigned int)(remaining < EG_SEARCH_RESULTS ? remaining : EG_SEARCH_RESULTS);
    memcpy(hits, ranked + offset, *count * sizeof(*hits));
    result = 0;
done:
    rt_free(ranked); rt_free(positions);
    return result;
}

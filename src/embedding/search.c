#include "search.h"
#include "tool_json_parser.h"

static int boolean_field(const ToolJson *json, size_t object, const char *key, int *out) {
    size_t field = tool_json_get(json, object, key);
    const ToolJsonToken *token = tool_json_token(json, field);
    if (!token || token->kind != TOOL_JSON_BOOL) return -1;
    *out = tool_json_true(json, field);
    return 0;
}

static int compare_title(const void *a, const void *b) {
    return rt_strcmp(((const EgTitle *)a)->id, ((const EgTitle *)b)->id);
}

void eg_title_index_free(EgTitleIndex *index) {
    for (size_t i = 0; i < index->count; i++) {
        rt_free(index->titles[i].id);
        rt_free(index->titles[i].title);
        rt_free(index->titles[i].reference);
    }
    rt_free(index->titles);
    rt_memset(index, 0, sizeof(*index));
}

int eg_title_index_load(EgTitleIndex *index, const char *directory) {
    char path[4096];
    unsigned char *data = 0;
    size_t size = 0;
    ToolJson json = {0};
    PlatformDirEntry info;
    unsigned long long dimensions, count, primary_year = 0, declared[100] = {0};
    int result = -1;
    rt_memset(index, 0, sizeof(*index));
    if (rt_join_path(directory, "manifest.json", path, sizeof(path)))
        return eg_error("index directory path too long", directory);
    if (platform_get_path_info(path, &info) || (info.mode & 0170000U) != 0100000U ||
        info.size > 64U * 1024U * 1024U || tool_read_all_input(path, &data, &size)) {
        eg_error("cannot read index manifest (limit 64 MiB): ", path); goto done;
    }
    if (size != info.size || tool_json_parse(&json, (const char *)data, size)) goto invalid;
    int multi = tool_json_equals_alloc(&json, tool_json_get(&json, 0, "format"), "eg2-paper-title-index-v2");
    if ((!multi && !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "format"), "eg2-paper-title-index-v1")) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "model_revision"), EG2_MODEL_REVISION) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "weights_sha256"), EG2_WEIGHTS_SHA256) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "tokenizer_sha256"), EG2_TOKENIZER_SHA256) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "task"), "document") ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "prefix"), EG2_DOCUMENT_PREFIX) ||
        !tool_json_equals_alloc(&json, tool_json_get(&json, 0, "source_field"), "Paper.name") ||
        tool_json_uint(&json, tool_json_get(&json, 0, "dimensions"), &dimensions) || dimensions != EG2_DIM ||
        tool_json_uint(&json, tool_json_get(&json, 0, "count"), &count) || !count || count > 100000U ||
        boolean_field(&json, 0, "archive_complete", &index->complete)) goto invalid;
    if (multi) {
        const ToolJsonToken *archives = tool_json_token(&json, tool_json_get(&json, 0, "archives"));
        if (!archives || archives->kind != TOOL_JSON_ARRAY || archives->next == tool_json_get(&json, 0, "archives") + 1)
            goto invalid;
        int all_complete = 1;
        size_t a = tool_json_get(&json, 0, "archives") + 1;
        while (a < archives->next) {
            unsigned long long year, n;
            int complete;
            if (tool_json_uint(&json, tool_json_get(&json, a, "year"), &year) || year < 2000 || year > 2099 ||
                tool_json_uint(&json, tool_json_get(&json, a, "count"), &n) || n > count ||
                boolean_field(&json, a, "archive_complete", &complete)) goto invalid;
            declared[year - 2000] += n;
            if (declared[year - 2000] > count) goto invalid;
            index->year_present[year - 2000] = 1;
            all_complete = all_complete && complete;
            a = tool_json_token(&json, a)->next;
        }
        if (all_complete != index->complete) goto invalid;
    } else {
        if (tool_json_uint(&json, tool_json_get(&json, 0, "year"), &primary_year) ||
            primary_year < 2000 || primary_year > 2099) goto invalid;
        declared[primary_year - 2000] = count;
        index->year_present[primary_year - 2000] = 1;
    }
    size_t items_index = tool_json_get(&json, 0, "items"), n = 0;
    const ToolJsonToken *items = tool_json_token(&json, items_index);
    if (!items || items->kind != TOOL_JSON_ARRAY) goto invalid;
    index->titles = rt_malloc_array((size_t)count, sizeof(*index->titles));
    if (!index->titles) { eg_error("cannot allocate resident title vectors", 0); goto done; }
    index->count = (size_t)count;
    rt_memset(index->titles, 0, index->count * sizeof(*index->titles));
    for (size_t i = items_index + 1; i < items->next; i = tool_json_token(&json, i)->next) {
        if (n == index->count) goto invalid;
        EgTitle *title = index->titles + n++;
        unsigned long long year = primary_year, tokens;
        unsigned char key[32];
        char relative[80], stored[80], key_hex[65], prefix[4] = "00/";
        if (multi && tool_json_uint(&json, tool_json_get(&json, i, "year"), &year)) goto invalid;
        if (year < 2000 || year > 2099 || !index->year_present[year - 2000]) goto invalid;
        title->year = (unsigned int)year;
        title->id = tool_json_dup_string(&json, tool_json_get(&json, i, "id"), 4096);
        title->title = tool_json_dup_string(&json, tool_json_get(&json, i, "title"),
                                       EG2_MAX_TEXT - (sizeof(EG2_DOCUMENT_PREFIX) - 1));
        title->reference = tool_json_dup_string(&json, tool_json_get(&json, i, "reference"), 512);
        if (!title->id || !title->id[0] || !title->title || !title->title[0] || !title->reference ||
            tool_json_uint(&json, tool_json_get(&json, i, "tokens"), &tokens) ||
            tokens < 2 || tokens > EG2_MAX_TOKENS) goto invalid;
        prefix[0] = (char)('0' + (year % 100) / 10);
        prefix[1] = (char)('0' + year % 10);
        if (rt_strncmp(title->reference, prefix, 3)) goto invalid;
        eg_title_key(title->id, title->title, key);
        eg_title_vector_path(key, relative);
        if (tool_json_string(&json, tool_json_get(&json, i, "vector"), stored, sizeof(stored)) ||
            rt_strcmp(stored, relative) ||
            tool_json_string(&json, tool_json_get(&json, i, "key"), key_hex, sizeof(key_hex)) ||
            rt_strlen(key_hex) != 64 || rt_strncmp(key_hex, relative + 6, 64)) goto invalid;
        if (rt_join_path(directory, relative, path, sizeof(path))) {
            eg_error("vector path too long", relative); goto done;
        }
        int found;
        if (eg_vector_read(path, key, title->vector, &found)) goto done;
        if (!found) { eg_error("missing indexed vector: ", path); goto done; }
        index->years[year - 2000]++;
    }
    if (n != index->count) goto invalid;
    for (size_t i = 0; i < 100; i++) if (declared[i] != index->years[i]) goto invalid;
    rt_sort(index->titles, index->count, sizeof(*index->titles), compare_title);
    for (size_t i = 1; i < index->count; i++)
        if (!rt_strcmp(index->titles[i - 1].id, index->titles[i].id)) goto invalid;
    result = 0;
    goto done;
invalid:
    eg_error("invalid, incompatible or inconsistent title index: ", directory);
done:
    tool_json_free(&json);
    rt_free(data);
    if (result) eg_title_index_free(index);
    return result;
}

static int compare_hit(const void *a, const void *b) {
    const EgSearchHit *left = a, *right = b;
    if (left->score != right->score) return left->score > right->score ? -1 : 1;
    return left->index < right->index ? -1 : left->index > right->index ? 1 : 0;
}

int eg_title_rank(const EgTitleIndex *index, const float query[EG2_DIM], size_t offset,
                  EgSearchHit hits[EG_SEARCH_RESULTS], unsigned int *count) {
    *count = 0;
    if (offset >= index->count) return eg_error("search page is outside the title index", 0);
    EgSearchHit *ranked = rt_malloc_array(index->count, sizeof(*ranked));
    if (!ranked) return eg_error("cannot allocate search ranking", 0);
    for (size_t i = 0; i < index->count; i++) {
        double score = 0;
        for (unsigned int j = 0; j < EG2_DIM; j++)
            score += (double)query[j] * index->titles[i].vector[j];
        ranked[i] = (EgSearchHit){i, score};
    }
    rt_sort(ranked, index->count, sizeof(*ranked), compare_hit);
    size_t remaining = index->count - offset;
    *count = (unsigned int)(remaining < EG_SEARCH_RESULTS ? remaining : EG_SEARCH_RESULTS);
    memcpy(hits, ranked + offset, *count * sizeof(*hits));
    rt_free(ranked);
    return 0;
}

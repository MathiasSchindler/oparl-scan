#include "search.h"
#include "runtime.h"
#include "hash_util.h"
#include "platform.h"

#define NONE UINT32_MAX

void search_builder_init(SearchBuilder *builder) {
    rt_memset(builder, 0, sizeof(*builder));
    tool_byte_buffer_init(&builder->strings);
    tool_array_init(&builder->records, sizeof(SearchRecord));
    tool_array_init(&builder->terms, sizeof(SearchTerm));
    tool_array_init(&builder->occurrences, sizeof(SearchOccurrence));
    tool_array_init(&builder->owners, sizeof(SearchOwner));
    tool_array_init(&builder->relations, sizeof(SearchRelation));
    tool_array_init(&builder->facts, sizeof(SearchFact));
    tool_array_init(&builder->evidence, sizeof(SearchEvidence));
    tool_array_init(&builder->versions, sizeof(SearchVersion));
    tool_array_init(&builder->sections, sizeof(SearchSection));
    {
        long long now = platform_get_epoch_time();
        if (now <= 0) {
            search_error("cannot obtain index import time", 0);
            builder->failed = 1;
        } else builder->imported_at = (uint64_t)now;
    }
    if (tool_byte_buffer_append(&builder->strings, "", 1) != 0) builder->failed = 1;
}

void search_builder_free(SearchBuilder *builder) {
    tool_byte_buffer_free(&builder->strings);
    tool_array_free(&builder->records);
    tool_array_free(&builder->terms);
    tool_array_free(&builder->occurrences);
    tool_array_free(&builder->owners);
    tool_array_free(&builder->relations);
    tool_array_free(&builder->facts);
    tool_array_free(&builder->evidence);
    tool_array_free(&builder->versions);
    tool_array_free(&builder->sections);
    rt_free(builder->table);
}

uint32_t search_string(SearchBuilder *builder, const char *text, size_t length) {
    uint32_t offset;
    if (length == 0) return 0;
    if (builder->failed || length >= UINT32_MAX || builder->strings.size > UINT32_MAX - length - 1) {
        builder->failed = 1;
        return 0;
    }
    offset = (uint32_t)builder->strings.size;
    if (tool_byte_buffer_append(&builder->strings, text, length) != 0 ||
        tool_byte_buffer_append(&builder->strings, "", 1) != 0) builder->failed = 1;
    return offset;
}

const char *search_builder_string(const SearchBuilder *builder, uint32_t offset) {
    return (const char *)builder->strings.data + offset;
}

static uint64_t hash_word(const char *text) {
    uint64_t hash = 14695981039346656037ULL;
    while (*text) { hash ^= (unsigned char)*text++; hash *= 1099511628211ULL; }
    return hash;
}

static uint32_t intern(SearchBuilder *builder, const char *word) {
    SearchTerm *terms;
    size_t slot, i;
    if (builder->terms.count * 2 >= builder->table_capacity) {
        size_t capacity = builder->table_capacity ? builder->table_capacity * 2 : 4096;
        uint32_t *table = (uint32_t *)rt_malloc_array(capacity, sizeof(uint32_t));
        if (table == 0) return NONE;
        for (i = 0; i < capacity; i++) table[i] = NONE;
        terms = (SearchTerm *)builder->terms.data;
        for (i = 0; i < builder->terms.count; i++) {
            slot = (size_t)hash_word(search_builder_string(builder, terms[i].text)) & (capacity - 1);
            while (table[slot] != NONE) slot = (slot + 1) & (capacity - 1);
            table[slot] = (uint32_t)i;
        }
        rt_free(builder->table);
        builder->table = table;
        builder->table_capacity = capacity;
    }
    slot = (size_t)hash_word(word) & (builder->table_capacity - 1);
    terms = (SearchTerm *)builder->terms.data;
    while (builder->table[slot] != NONE) {
        uint32_t id = builder->table[slot];
        if (rt_strcmp(search_builder_string(builder, terms[id].text), word) == 0) return id;
        slot = (slot + 1) & (builder->table_capacity - 1);
    }
    {
        SearchTerm *term = (SearchTerm *)tool_array_append(&builder->terms);
        uint32_t id;
        if (term == 0 || builder->terms.count > UINT32_MAX) return NONE;
        id = (uint32_t)builder->terms.count - 1;
        rt_memset(term, 0, sizeof(*term));
        term->text = search_string(builder, word, rt_strlen(word));
        if (builder->failed) return NONE;
        builder->table[slot] = id;
        return id;
    }
}

uint32_t search_add_record(SearchBuilder *builder, const SearchRecord *record) {
    SearchRecord *stored;
    uint32_t id, position = 0;
    size_t p = 0, start, end;
    char word[1024];
    int result;
    if (builder->records.count >= UINT32_MAX) goto fail;
    id = (uint32_t)builder->records.count;
    stored = (SearchRecord *)tool_array_append(&builder->records);
    if (stored == 0) goto fail;
    *stored = *record;
    stored->words = 0;
    while ((result = search_next_word(search_builder_string(builder, record->text), record->text_length,
                                      &p, &start, &end, &position)) > 0) {
        uint32_t term;
        SearchOccurrence *occurrence;
        const char *text = search_builder_string(builder, record->text);
        if (search_fold_word(text + start, end - start, word, sizeof(word)) != 0) goto fail;
        term = intern(builder, word);
        if (term == NONE) goto fail;
        occurrence = (SearchOccurrence *)tool_array_append(&builder->occurrences);
        if (occurrence == 0 || builder->occurrences.count >= UINT32_MAX) goto fail;
        occurrence->term = term; occurrence->record = id;
        occurrence->position = position++; occurrence->byte = (uint32_t)start;
        stored->words++;
    }
    if (result < 0) goto fail;
    return id;
fail:
    builder->failed = 1;
    return NONE;
}

int search_add_owner(SearchBuilder *builder, uint32_t record, uint32_t paper) {
    SearchOwner *owner = (SearchOwner *)tool_array_append(&builder->owners);
    if (owner == 0) return search_error("cannot allocate document ownership", 0);
    owner->record = record; owner->paper = paper;
    return 0;
}

static int compare_occurrence(const void *a, const void *b) {
    const SearchOccurrence *x = (const SearchOccurrence *)a, *y = (const SearchOccurrence *)b;
    if (x->term != y->term) return x->term < y->term ? -1 : 1;
    if (x->record != y->record) return x->record < y->record ? -1 : 1;
    return x->position < y->position ? -1 : x->position > y->position;
}

static double natural_log(double x) {
    double exponent = 0, y, term, result = 0;
    unsigned int n;
    while (x > 2) { x *= .5; exponent += .693147180559945309; }
    y = (x - 1) / (x + 1); term = y;
    for (n = 1; n < 32; n += 2) { result += term / n; term *= y * y; }
    return exponent + 2 * result;
}

static uint64_t aligned(uint64_t n) { return (n + 7ULL) & ~7ULL; }
static int compare_fact(const void *a, const void *b) {
    const SearchFact *x = (const SearchFact *)a, *y = (const SearchFact *)b;
    if (x->record != y->record) return x->record < y->record ? -1 : 1;
    return x->role < y->role ? -1 : x->role > y->role;
}

typedef struct { const char *word; uint32_t id; } TermOrder;
static int compare_term(const void *a, const void *b) {
    return rt_strcmp(((const TermOrder *)a)->word, ((const TermOrder *)b)->word);
}

static int order_terms(SearchBuilder *builder) {
    SearchTerm *terms = (SearchTerm *)builder->terms.data, *sorted;
    SearchOccurrence *occurrences = (SearchOccurrence *)builder->occurrences.data;
    TermOrder *order;
    size_t i;
    if (!builder->terms.count) return 0;
    order = (TermOrder *)rt_malloc_array(builder->terms.count, sizeof(*order));
    sorted = (SearchTerm *)rt_malloc_array(builder->terms.count, sizeof(*sorted));
    if (!order || !sorted) { rt_free(order); rt_free(sorted); return -1; }
    for (i = 0; i < builder->terms.count; i++) {
        order[i].word = search_builder_string(builder, terms[i].text);
        order[i].id = (uint32_t)i;
    }
    rt_sort(order, builder->terms.count, sizeof(*order), compare_term);
    for (i = 0; i < builder->terms.count; i++) {
        sorted[i] = terms[order[i].id];
        builder->table[order[i].id] = (uint32_t)i;
    }
    for (i = 0; i < builder->occurrences.count; i++)
        occurrences[i].term = builder->table[occurrences[i].term];
    memcpy(terms, sorted, builder->terms.count * sizeof(*terms));
    rt_free(order); rt_free(sorted);
    return 0;
}

static void section(CryptoSha256Context *sha, const void *data, size_t size, uint64_t padded) {
    static const unsigned char zero[8] = {0};
    crypto_sha256_update(sha, (const unsigned char *)data, size);
    crypto_sha256_update(sha, zero, (size_t)padded - size);
}

int search_write(SearchBuilder *builder, const char *path) {
    SearchIndexHeader header;
    CryptoSha256Context sha;
    SearchOccurrence *occurrences = (SearchOccurrence *)builder->occurrences.data;
    SearchTerm *terms = (SearchTerm *)builder->terms.data;
    const void *parts[10];
    size_t sizes[10], i;
    uint64_t offsets[11];
    char part[4096], scratch[4096];
    int fd, result = 0;
    if (builder->failed) return search_error("index allocation/token limit exceeded", 0);
    if (order_terms(builder) != 0) return search_error("cannot sort term dictionary", 0);
    rt_sort(builder->facts.data, builder->facts.count, sizeof(SearchFact), compare_fact);
    rt_sort(occurrences, builder->occurrences.count, sizeof(*occurrences), compare_occurrence);
    for (i = 0; i < builder->occurrences.count; i++) {
        SearchTerm *term = &terms[occurrences[i].term];
        if (term->count == 0) term->start = (uint32_t)i;
        term->count++;
        if (i == term->start || occurrences[i - 1].record != occurrences[i].record) term->documents++;
    }
    for (i = 0; i < builder->terms.count; i++)
        terms[i].idf = (float)natural_log(1.0 +
          ((double)builder->records.count - terms[i].documents + .5) / (terms[i].documents + .5));
    parts[0] = builder->strings.data; sizes[0] = builder->strings.size;
    parts[1] = builder->records.data; sizes[1] = builder->records.count * sizeof(SearchRecord);
    parts[2] = builder->terms.data; sizes[2] = builder->terms.count * sizeof(SearchTerm);
    parts[3] = builder->occurrences.data; sizes[3] = builder->occurrences.count * sizeof(SearchOccurrence);
    parts[4] = builder->owners.data; sizes[4] = builder->owners.count * sizeof(SearchOwner);
    parts[5] = builder->relations.data; sizes[5] = builder->relations.count * sizeof(SearchRelation);
    parts[6] = builder->facts.data; sizes[6] = builder->facts.count * sizeof(SearchFact);
    parts[7] = builder->evidence.data; sizes[7] = builder->evidence.count * sizeof(SearchEvidence);
    parts[8] = builder->versions.data; sizes[8] = builder->versions.count * sizeof(SearchVersion);
    parts[9] = builder->sections.data; sizes[9] = builder->sections.count * sizeof(SearchSection);
    rt_memset(&header, 0, sizeof(header));
    memcpy(header.magic, "OPARLIDX", 8);
    header.version = 3; header.endian = 0x01020304U; header.incomplete = builder->incomplete;
    for (i = 0; i < builder->records.count; i++) {
        const SearchRecord *r = &((const SearchRecord *)builder->records.data)[i];
        if (r->preparation && header.version < 4) header.version = 4;
        if (r->parent != NONE && r->preparation !=
            ((const SearchRecord *)builder->records.data)[r->parent].preparation) header.version = 5;
    }
    offsets[0] = aligned(sizeof(header));
    for (i = 0; i < 10; i++) offsets[i + 1] = offsets[i] + aligned(sizes[i]);
    header.strings = offsets[0]; header.records = offsets[1]; header.terms = offsets[2];
    header.occurrences = offsets[3]; header.owners = offsets[4]; header.size = offsets[10];
    header.relations = offsets[5]; header.facts = offsets[6]; header.evidence = offsets[7];
    header.versions = offsets[8]; header.sections = offsets[9];
    header.string_bytes = builder->strings.size; header.record_count = builder->records.count;
    header.term_count = builder->terms.count; header.occurrence_count = builder->occurrences.count;
    header.owner_count = builder->owners.count;
    header.relation_count = builder->relations.count; header.fact_count = builder->facts.count;
    header.evidence_count = builder->evidence.count; header.version_count = builder->versions.count;
    header.section_count = builder->sections.count; header.imported_at = builder->imported_at;
    if (header.size > SEARCH_INDEX_LIMIT) return search_error("index exceeds the 4 GiB format limit", path);
    crypto_sha256_init(&sha);
    crypto_sha256_update(&sha, (const unsigned char *)&header, sizeof(header));
    for (i = 0; i < 10; i++) section(&sha, parts[i], sizes[i], aligned(sizes[i]));
    crypto_sha256_final(&sha, header.sha256);
    if (tool_path_append_suffix(path, ".part", part, sizeof(part)) != 0 ||
        tool_ensure_parent_dirs(path, scratch, sizeof(scratch)) != 0)
        return search_error("cannot prepare index output", path);
    fd = platform_open_write(part, 0644);
    if (fd < 0) return search_error("cannot create index", part);
    if (rt_write_all(fd, &header, sizeof(header)) != 0) result = -1;
    for (i = 0; i < 10 && result == 0; i++) {
        static const unsigned char zero[8] = {0};
        if (rt_write_all(fd, parts[i], sizes[i]) != 0 ||
            rt_write_all(fd, zero, (size_t)aligned(sizes[i]) - sizes[i]) != 0) result = -1;
    }
    if (platform_close(fd) != 0) result = -1;
    if (result == 0 && platform_rename_path(part, path) == 0) return 0;
    return search_error("cannot publish index", path);
}

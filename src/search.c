#include "search.h"
#include "runtime.h"
#include "hash_util.h"
#include "platform.h"

#define NONE UINT32_MAX
#define QUERY_TERMS 64U
#define QUERY_NODES 128U
#define SEARCH_RAM_LIMIT (4ULL * 1024 * 1024 * 1024 - 16ULL * 1024 * 1024)

_Static_assert(sizeof(SearchIndexHeader) == 232, "fixed binary index header");
_Static_assert(sizeof(SearchRecord) == 88 && sizeof(SearchTerm) == 24 &&
    sizeof(SearchOccurrence) == 16 && sizeof(SearchOwner) == 8, "fixed binary section layouts");
_Static_assert(sizeof(SearchRelation) == 24 && sizeof(SearchFact) == 16 &&
    sizeof(SearchEvidence) == 32 && sizeof(SearchVersion) == 40 &&
    sizeof(SearchSection) == 24, "fixed canonical model layouts");

int search_error(const char *message, const char *detail) {
    rt_write_cstr(2, "oparl: error: ");
    rt_write_cstr(2, message);
    if (detail != 0) { rt_write_cstr(2, ": "); rt_write_cstr(2, detail); }
    rt_write_char(2, '\n');
    return -1;
}

int search_fold_word(const char *text, size_t length, char *out, size_t capacity) {
    size_t p = 0, used = 0;
    unsigned int pending = 0;
    while (p < length || pending) {
        unsigned int cp;
        char bytes[4];
        size_t count;
        if (pending) { cp = pending; pending = 0; }
        else if (rt_utf8_decode(text, length, &p, &cp) != 0) return -1;
        cp = rt_unicode_simple_fold(cp);
        if ((cp == 'a' || cp == 'o' || cp == 'u') && p < length) {
            size_t q = p;
            unsigned int next;
            if (rt_utf8_decode(text, length, &q, &next) == 0 && next == 0x308) {
                cp = cp == 'a' ? 0xe4 : cp == 'o' ? 0xf6 : 0xfc;
                p = q;
            }
        }
        if (cp == 0xdf) {
            if (used + 2 >= capacity) return -1;
            out[used++] = 's'; out[used++] = 's';
            continue;
        }
        if (cp == 0xfb01 || cp == 0xfb02) { pending = cp == 0xfb01 ? 'i' : 'l'; cp = 'f'; }
        if (cp == 0xad) continue;
        if (rt_utf8_encode(cp, bytes, sizeof(bytes), &count) != 0 || count >= capacity - used) return -1;
        memcpy(out + used, bytes, count); used += count;
    }
    out[used] = '\0';
    return 0;
}

int search_next_word(const char *text, size_t length, size_t *p, size_t *start,
                     size_t *end, uint32_t *position) {
    unsigned int cp;
    if (*p >= length) return 0;
    while (*p < length) {
        size_t before = *p;
        if (rt_utf8_decode(text, length, p, &cp) != 0) return -1;
        if (cp == 0x1e) {
            if (*position > UINT32_MAX - 4096) return -1;
            *position += 4096;
        }
        if (rt_unicode_is_word(cp)) { *start = before; break; }
    }
    if (*p == length && (length == 0 || !rt_unicode_is_word(cp))) return 0;
    while (*p < length) {
        size_t before = *p;
        if (rt_utf8_decode(text, length, p, &cp) != 0) return -1;
        if (!rt_unicode_is_word(cp) && cp != 0xad &&
            rt_unicode_word_break_class(cp) != RT_WORD_BREAK_EXTEND) { *p = before; break; }
    }
    *end = *p;
    return 1;
}

static uint64_t aligned(uint64_t n) { return (n + 7ULL) & ~7ULL; }

void search_close(SearchIndex *index) {
    search_embedding_close(index);
    search_research_free(index);
    search_facets_free(index);
    rt_free(index->links);
    rt_free(index->allocation);
    rt_memset(index, 0, sizeof(*index));
}

static int valid_string(const SearchIndex *index, uint32_t offset) {
    size_t p = offset;
    if (p >= index->string_bytes) return 0;
    while (index->string_bytes - p >= sizeof(uint64_t)) {
        uint64_t word;
        __builtin_memcpy(&word, index->strings + p, sizeof(word));
        if ((word - 0x0101010101010101ULL) & ~word & 0x8080808080808080ULL) return 1;
        p += sizeof(word);
    }
    while (p < index->string_bytes && index->strings[p]) p++;
    return p < index->string_bytes;
}

int search_open(SearchIndex *index, const char *path) {
    PlatformDirEntry info;
    SearchIndexHeader header;
    unsigned char *data, digest[32], expected_digest[32];
    CryptoSha256Context sha;
    size_t p = 0, i;
    uint64_t expected, lookup_bytes;
    uint32_t owner_start = 0;
    int fd, closed;
    rt_memset(index, 0, sizeof(*index));
    if (platform_get_path_info(path, &info) != 0 || info.is_dir ||
        info.size < 12 || info.size > SEARCH_INDEX_LIMIT)
        return search_error("invalid index size", path);
    fd = platform_open_read(path);
    if (fd < 0) return search_error("cannot open index", path);
    while (p < 12) {
        long got = platform_read(fd, (unsigned char *)&header + p, 12 - p);
        if (got <= 0) goto invalid;
        p += (size_t)got;
    }
    if (memcmp(header.magic, "OPARLIDX", 8) == 0 && header.version != 3 && header.version != 4 && header.version != 5) {
        platform_close(fd);
        return search_error("unsupported index format; rebuild with oparl-index --input ARCHIVE --output INDEX", path);
    }
    if (info.size < sizeof(header)) goto invalid;
    while (p < sizeof(header)) {
        long got = platform_read(fd, (unsigned char *)&header + p, sizeof(header) - p);
        if (got <= 0) goto invalid;
        p += (size_t)got;
    }
    if (memcmp(header.magic, "OPARLIDX", 8) != 0 || (header.version != 3 && header.version != 4 && header.version != 5) ||
        header.endian != 0x01020304U || header.size != info.size || header.reserved ||
        header.incomplete > 1 || !header.imported_at || header.imported_at > INT64_MAX ||
        header.strings != aligned(sizeof(header)) || header.string_bytes == 0) goto invalid;
    expected = header.strings;
#define CHECK_SECTION(field, count, width) \
    if (header.field != expected || header.count > (info.size - expected) / (width)) goto invalid; \
    expected += aligned(header.count * (width)); \
    if (expected > info.size) goto invalid
    CHECK_SECTION(strings, string_bytes, 1);
    CHECK_SECTION(records, record_count, sizeof(SearchRecord));
    CHECK_SECTION(terms, term_count, sizeof(SearchTerm));
    CHECK_SECTION(occurrences, occurrence_count, sizeof(SearchOccurrence));
    CHECK_SECTION(owners, owner_count, sizeof(SearchOwner));
    CHECK_SECTION(relations, relation_count, sizeof(SearchRelation));
    CHECK_SECTION(facts, fact_count, sizeof(SearchFact));
    CHECK_SECTION(evidence, evidence_count, sizeof(SearchEvidence));
    CHECK_SECTION(versions, version_count, sizeof(SearchVersion));
    CHECK_SECTION(sections, section_count, sizeof(SearchSection));
#undef CHECK_SECTION
    if (expected != info.size || header.record_count > UINT32_MAX ||
        header.term_count > UINT32_MAX || header.occurrence_count > UINT32_MAX) goto invalid;
    lookup_bytes = header.record_count * sizeof(SearchLinks) + header.owner_count * sizeof(uint32_t);
    if (info.size + lookup_bytes > SEARCH_RAM_LIMIT) {
        platform_close(fd);
        return search_error("index plus relationship tables exceeds the 4 GiB search RAM budget", path);
    }
    data = (unsigned char *)rt_malloc((size_t)info.size);
    if (data == 0) {
        platform_close(fd);
        return search_error("cannot allocate index; maximum search RAM is 4 GiB", path);
    }
    index->allocation = data;
    memcpy(data, &header, sizeof(header));
    memcpy(expected_digest, header.sha256, sizeof(expected_digest));
    rt_memset(header.sha256, 0, sizeof(header.sha256));
    crypto_sha256_init(&sha);
    crypto_sha256_update(&sha, (const unsigned char *)&header, sizeof(header));
    while (p < (size_t)info.size) {
        size_t chunk = (size_t)info.size - p;
        long got;
        if (chunk > 1024 * 1024) chunk = 1024 * 1024;
        got = platform_read(fd, data + p, chunk);
        if (got <= 0) goto invalid;
        crypto_sha256_update(&sha, data + p, (size_t)got);
        p += (size_t)got;
    }
    closed = platform_close(fd); fd = -1;
    if (closed != 0) goto invalid;
    crypto_sha256_final(&sha, digest);
    if (memcmp(digest, expected_digest, 32) != 0) goto invalid;
    index->strings = (const char *)data + header.strings;
    index->string_bytes = (size_t)header.string_bytes;
    index->records = (const SearchRecord *)(const void *)(data + header.records);
    index->terms = (const SearchTerm *)(const void *)(data + header.terms);
    index->occurrences = (const SearchOccurrence *)(const void *)(data + header.occurrences);
    index->owners = (const SearchOwner *)(const void *)(data + header.owners);
    index->relations = (const SearchRelation *)(const void *)(data + header.relations);
    index->facts = (const SearchFact *)(const void *)(data + header.facts);
    index->evidence = (const SearchEvidence *)(const void *)(data + header.evidence);
    index->versions = (const SearchVersion *)(const void *)(data + header.versions);
    index->sections = (const SearchSection *)(const void *)(data + header.sections);
    index->relation_count = (size_t)header.relation_count; index->fact_count = (size_t)header.fact_count;
    index->evidence_count = (size_t)header.evidence_count; index->version_count = (size_t)header.version_count;
    index->section_count = (size_t)header.section_count; index->imported_at = header.imported_at;
    hash_to_hex(expected_digest, 32, index->snapshot_id);
    index->record_count = (size_t)header.record_count; index->term_count = (size_t)header.term_count;
    index->occurrence_count = (size_t)header.occurrence_count; index->owner_count = (size_t)header.owner_count;
    index->file_bytes = (size_t)info.size;
    index->memory_bytes = (size_t)(info.size + lookup_bytes);
    index->incomplete = header.incomplete;
    index->format_version = header.version;
    {
        double words = 0;
        for (i = 0; i < index->record_count; i++) {
            const SearchRecord *r = &index->records[i];
            const uint32_t offsets[] = {r->id,r->name,r->reference,r->date,r->paper_type,
                                        r->organization,r->kind,r->source,r->text,r->payload,
                                        r->identity,r->system,r->body,r->preparation};
            size_t j;
            for (j = 0; j < sizeof(offsets)/sizeof(offsets[0]); j++)
                if (!valid_string(index, offsets[j])) goto invalid;
            if (r->text_length >= index->string_bytes - r->text ||
                index->strings[r->text + r->text_length] != '\0' || r->body_start > r->text_length ||
                r->metadata_origin > 2 || (header.version == 3 && r->preparation) ||
                (r->parent != NONE && (r->parent >= index->record_count ||
                 index->records[r->parent].parent != NONE))) goto invalid;
            words += r->words;
        }
        index->average_length = index->record_count ? (float)(words / index->record_count) : 1;
        if (index->average_length < 1) index->average_length = 1;
    }
    for (i = 0; i < index->term_count; i++) {
        const SearchTerm *t = &index->terms[i];
        if (!valid_string(index, t->text) || t->start > index->occurrence_count ||
            t->count > index->occurrence_count - t->start || t->documents > index->record_count ||
            !(t->idf >= 0 && t->idf < 100)) goto invalid;
    }
    for (i = 0; i < index->occurrence_count; i++) {
        const SearchOccurrence *o = &index->occurrences[i];
        if (o->term >= index->term_count || o->record >= index->record_count ||
            o->byte >= index->records[o->record].text_length) goto invalid;
    }
    for (i = 0; i < index->owner_count; i++)
        if (index->owners[i].record >= index->record_count || index->owners[i].paper >= index->record_count)
            goto invalid;
    if (search_model_validate(index) != 0) goto invalid;
    index->links = (SearchLinks *)rt_malloc(lookup_bytes ? (size_t)lookup_bytes : 1);
    if (!index->links) {
        search_close(index);
        return search_error("cannot allocate relationship tables", path);
    }
    rt_memset(index->links, 0, (size_t)lookup_bytes);
    index->related_papers = (uint32_t *)(void *)(index->links + index->record_count);
    for (i = 0; i < index->owner_count; i++) index->links[index->owners[i].record].count++;
    for (i = 0; i < index->record_count; i++) {
        SearchLinks *links = &index->links[i];
        links->start = owner_start; owner_start += links->count; links->count = 0;
        links->paper = rt_strcmp(index->strings + index->records[i].kind, "Paper") == 0;
    }
    for (i = 0; i < index->owner_count; i++) {
        const SearchOwner *owner = &index->owners[i];
        SearchLinks *links = &index->links[owner->record];
        index->related_papers[links->start + links->count++] = owner->paper;
    }
    if (search_facets_build(index) != 0) { search_close(index); return -1; }
    if (search_research_build(index) != 0) { search_close(index); return -1; }
    return 0;
invalid:
    if (fd >= 0) platform_close(fd);
    search_close(index);
    return search_error("corrupt, truncated or incompatible index", path);
}

typedef struct { unsigned int start, count, prefix, phrase; } Atom;
typedef struct { int kind, left, right; unsigned int atom; } Node;
typedef struct {
    const SearchIndex *index;
    const char *query;
    size_t p;
    Atom atoms[QUERY_TERMS];
    char words[QUERY_TERMS][1024];
    Node nodes[QUERY_NODES];
    unsigned int atom_count, node_count, word_count;
    const char *error;
} Parser;

static void spaces(Parser *p) { while (rt_is_space(p->query[p->p])) p->p++; }

static int keyword(Parser *p, const char *word) {
    size_t n = rt_strlen(word);
    spaces(p);
    if (rt_strncmp(p->query + p->p, word, n) != 0) return 0;
    if (p->query[p->p+n] && !rt_is_space(p->query[p->p+n]) && p->query[p->p+n] != '(' &&
        p->query[p->p+n] != ')') return 0;
    p->p += n;
    return 1;
}

static int node(Parser *p, int kind, int left, int right, unsigned int atom) {
    Node *n;
    if (p->node_count == QUERY_NODES) { p->error = "query too complex"; return -1; }
    n = &p->nodes[p->node_count];
    n->kind = kind; n->left = left; n->right = right; n->atom = atom;
    return (int)p->node_count++;
}

static int parse_or(Parser *p, unsigned int depth);

static int primary(Parser *p, unsigned int depth) {
    size_t start, end, position = 0, a, b;
    uint32_t word_position = 0;
    unsigned int id;
    Atom *atom;
    int result, child;
    if (depth > 32) { p->error = "query nesting too deep"; return -1; }
    spaces(p);
    if (keyword(p, "NOT")) {
        child = primary(p, depth + 1);
        return child < 0 ? -1 : node(p, 3, child, -1, 0);
    }
    if (p->query[p->p] == '(') {
        p->p++; child = parse_or(p, depth + 1); spaces(p);
        if (child < 0 || p->query[p->p] != ')') { p->error = "unclosed parentheses"; return -1; }
        p->p++;
        return child;
    }
    if (keyword(p, "AND") || keyword(p, "OR")) {
        p->error = "Boolean operator without an operand"; return -1;
    }
    if (p->atom_count == QUERY_TERMS) { p->error = "too many search terms"; return -1; }
    id = p->atom_count++;
    atom = &p->atoms[id];
    rt_memset(atom, 0, sizeof(*atom));
    atom->start = p->word_count;
    atom->phrase = p->query[p->p] == '"';
    if (atom->phrase) {
        start = ++p->p;
        while (p->query[p->p] && p->query[p->p] != '"') p->p++;
        if (!p->query[p->p]) { p->error = "unclosed phrase"; return -1; }
        end = p->p++;
    } else {
        start = p->p;
        while (p->query[p->p] && !rt_is_space(p->query[p->p]) && p->query[p->p] != ')' &&
               p->query[p->p] != '(') p->p++;
        end = p->p;
        if (end > start && p->query[end-1] == '*') { atom->prefix = 1; end--; }
        for (a = start; a < end; a++) if (p->query[a] == '*' || p->query[a] == '"') {
            p->error = "use only a trailing word-prefix * or a separately quoted phrase"; return -1;
        }
    }
    while ((result = search_next_word(p->query + start, end - start, &position, &a, &b, &word_position)) > 0) {
        if (p->word_count == QUERY_TERMS ||
            search_fold_word(p->query + start + a, b - a, p->words[p->word_count], 1024) != 0) {
            p->error = "search word too long"; return -1;
        }
        atom->count++;
        p->word_count++;
    }
    if (result < 0 || atom->count == 0 || (atom->prefix && atom->count != 1)) {
        p->error = "expected a word, phrase or word prefix"; return -1;
    }
    return node(p, 0, -1, -1, id);
}

static int parse_and(Parser *p, unsigned int depth) {
    int left = primary(p, depth), right;
    if (left < 0) return -1;
    for (;;) {
        size_t saved;
        spaces(p); saved = p->p;
        if (!p->query[p->p] || p->query[p->p] == ')' || keyword(p, "OR")) { p->p = saved; break; }
        (void)keyword(p, "AND");
        right = primary(p, depth);
        if (right < 0) return -1;
        left = node(p, 1, left, right, 0);
        if (left < 0) return -1;
    }
    return left;
}

static int parse_or(Parser *p, unsigned int depth) {
    int left = parse_and(p, depth), right;
    if (left < 0) return -1;
    while (keyword(p, "OR")) {
        right = parse_and(p, depth);
        if (right < 0) return -1;
        left = node(p, 2, left, right, 0);
        if (left < 0) return -1;
    }
    return left;
}

static size_t term_lower_bound(const SearchIndex *index, const char *word) {
    size_t a = 0, b = index->term_count;
    while (a < b) {
        size_t mid = a + (b - a) / 2;
        if (rt_strcmp(index->strings + index->terms[mid].text, word) < 0) a = mid + 1;
        else b = mid;
    }
    return a;
}

static uint32_t term_lookup(const SearchIndex *index, const char *word) {
    size_t i = term_lower_bound(index, word);
    return i < index->term_count && rt_strcmp(index->strings + index->terms[i].text, word) == 0 ?
           (uint32_t)i : NONE;
}

static int position_exists(const SearchIndex *index, uint32_t term, uint32_t record, uint32_t position) {
    const SearchTerm *t;
    size_t a, b;
    uint32_t wanted = position == NONE ? 0 : position;
    if (term == NONE) return 0;
    t = &index->terms[term]; a = t->start; b = a + t->count;
    while (a < b) {
        size_t m = a + (b - a) / 2;
        const SearchOccurrence *o = &index->occurrences[m];
        if (o->record < record || (o->record == record && o->position < wanted)) a = m + 1;
        else b = m;
    }
    return a < (size_t)t->start + t->count && index->occurrences[a].record == record &&
           (position == NONE || index->occurrences[a].position == position);
}

static int metadata_matches(const SearchIndex *index, const SearchRecord *r, const SearchFilter *filter) {
    if (filter->year && r->year != filter->year) return 0;
    if (filter->paper_type && !rt_unicode_normalized_equal(index->strings + r->paper_type,
        rt_strlen(index->strings + r->paper_type), filter->paper_type, rt_strlen(filter->paper_type), 1)) return 0;
    if (filter->organization && !rt_unicode_normalized_contains(index->strings + r->organization,
        rt_strlen(index->strings + r->organization), filter->organization, rt_strlen(filter->organization), 1)) return 0;
    if (!search_actor_matches(index, (uint32_t)(r-index->records), filter->organization_id, 1) ||
        !search_actor_matches(index, (uint32_t)(r-index->records), filter->actor_id, 0)) return 0;
    return 1;
}

typedef struct {
    const SearchIndex *index;
    const SearchFilter *filter;
    float *scores;
    uint32_t *bytes, *owners;
    uint64_t *seen;
    /* Two bits each for source eligibility and direct Paper match: unknown/no/yes. */
    unsigned char *states;
    int filtered;
} Query;

static int paper_matches(Query *query, uint32_t paper) {
    unsigned int state;
    if (!query->index->links[paper].paper) return 0;
    state = query->states[paper] >> 2;
    if (state == 0) {
        state = metadata_matches(query->index, &query->index->records[paper], query->filter) ? 2 : 1;
        query->states[paper] |= (unsigned char)(state << 2);
    }
    return state == 2;
}

static int record_eligible(Query *query, uint32_t record) {
    const SearchIndex *index = query->index;
    uint32_t source = index->records[record].parent, owner = NONE;
    unsigned int state;
    size_t i;
    int eligible;
    if (query->filter->kind && rt_strcmp(index->strings + index->records[record].kind, query->filter->kind))
        return 0;
    if (source == NONE) source = record;
    state = query->states[source] & 3U;
    if (state) return state == 2;
    eligible = !query->filtered || metadata_matches(index, &index->records[source], query->filter);
    for (i = index->links[source].start; i < (size_t)index->links[source].start + index->links[source].count; i++) {
        uint32_t paper = index->related_papers[i];
        if (paper_matches(query, paper)) { eligible = 1; owner = paper; break; }
    }
    query->owners[source] = owner;
    query->states[source] |= eligible ? 2U : 1U;
    return eligible;
}

static void atom_eval(const Parser *parser, const Atom *atom, uint64_t *matched,
                      int positive, Query *query) {
    const SearchIndex *index = parser->index;
    const char *word = parser->words[atom->start];
    uint32_t terms[QUERY_TERMS];
    size_t i, begin = 0, end = index->term_count;
    unsigned int j;
    if (!atom->prefix) {
        for (j = 0; j < atom->count; j++) {
            terms[j] = term_lookup(index, parser->words[atom->start + j]);
            if (terms[j] == NONE) return;
        }
        begin = terms[0]; end = begin + 1;
    } else begin = term_lower_bound(index, word);
    for (i = begin; i < end; i++) {
        const SearchTerm *term = &index->terms[i];
        size_t a = term->start, stop = a + term->count;
        if (atom->prefix && rt_strncmp(index->strings + term->text, word, rt_strlen(word)) != 0) break;
        while (a < stop) {
            uint32_t record = index->occurrences[a].record;
            uint32_t byte = index->occurrences[a].byte;
            float tf = 0;
            if (!record_eligible(query, record)) {
                do { a++; } while (a < stop && index->occurrences[a].record == record);
                continue;
            }
            do {
                const SearchOccurrence *o = &index->occurrences[a++];
                int all = 1;
                for (j = 1; j < atom->count; j++)
                    if (!position_exists(index, terms[j], record, atom->phrase ? o->position + j : NONE)) {
                        all = 0; break;
                    }
                if (all) {
                    if (tf == 0) byte = o->byte;
                    tf += o->byte < index->records[record].body_start ? 3.0f : 1.0f;
                }
            } while (a < stop && index->occurrences[a].record == record);
            if (tf > 0) {
                float length = (float)index->records[record].words / index->average_length;
                float score = term->idf * tf * 2.2f / (tf + 1.2f * (.25f + .75f * length));
                uint64_t bit = 1ULL << (record % 64);
                size_t block = record / 64;
                matched[block] |= bit;
                if (!(query->seen[block] & bit)) {
                    query->seen[block] |= bit;
                    query->scores[record] = 0;
                    query->bytes[record] = NONE;
                }
                if (positive) {
                    query->scores[record] += score;
                    if (query->bytes[record] == NONE) query->bytes[record] = byte;
                }
            }
        }
    }
}

static void positive_atoms(const Parser *parser, int n, int positive, unsigned char *atoms) {
    const Node *value = &parser->nodes[n];
    if (value->kind == 0) { if (positive) atoms[value->atom] = 1; return; }
    if (value->kind == 3) { positive_atoms(parser, value->left, !positive, atoms); return; }
    positive_atoms(parser, value->left, positive, atoms);
    positive_atoms(parser, value->right, positive, atoms);
}

static uint64_t evaluate(const Parser *parser, int n, const uint64_t *atoms, size_t block, size_t blocks) {
    const Node *node_value = &parser->nodes[n];
    if (node_value->kind == 0) return atoms[(size_t)node_value->atom * blocks + block];
    if (node_value->kind == 3) return ~evaluate(parser, node_value->left, atoms, block, blocks);
    if (node_value->kind == 1) return evaluate(parser, node_value->left, atoms, block, blocks) &
                                    evaluate(parser, node_value->right, atoms, block, blocks);
    return evaluate(parser, node_value->left, atoms, block, blocks) |
           evaluate(parser, node_value->right, atoms, block, blocks);
}

void search_workspace_free(SearchWorkspace *workspace) {
    rt_free(workspace->embedding_allocation);
    rt_free(workspace->parser);
    rt_free(workspace->allocation);
    rt_memset(workspace, 0, sizeof(*workspace));
}

static int hit_before(const SearchIndex *index, SearchSort sort, const SearchHit *a, const SearchHit *b) {
    if (sort != SEARCH_RELEVANCE) {
        const SearchDate *x = search_hit_time(index, a), *y = search_hit_time(index, b);
        int known_x = x && x->precision, known_y = y && y->precision, order = 0;
        if (known_x != known_y) return known_x;
        if (known_x) order = x->seconds < y->seconds ? -1 : x->seconds > y->seconds ? 1 :
            x->nanos < y->nanos ? -1 : x->nanos > y->nanos;
        if (order) return sort == SEARCH_DATE_ASC ? order < 0 : order > 0;
    }
    return a->score > b->score || (a->score == b->score &&
        (a->record < b->record || (a->record == b->record && a->paper < b->paper)));
}

/* Keep the worst retained hit at the root, so pagination needs only offset + limit hits. */
static void sift_hits(const SearchIndex *index, SearchSort sort, SearchHit *hits, size_t count, size_t root) {
    SearchHit value = hits[root];
    size_t child;
    while ((child = root * 2 + 1) < count) {
        if (child + 1 < count && hit_before(index, sort, &hits[child], &hits[child + 1])) child++;
        if (!hit_before(index, sort, &value, &hits[child])) break;
        hits[root] = hits[child]; root = child;
    }
    hits[root] = value;
}

static void retain_hit(const SearchIndex *index, SearchSort sort, SearchHit *hits,
                       size_t *retained, size_t keep, SearchHit hit) {
    if (*retained < keep) {
        size_t p = (*retained)++;
        while (p && hit_before(index, sort, &hits[(p - 1) / 2], &hit)) {
            hits[p] = hits[(p - 1) / 2]; p = (p - 1) / 2;
        }
        hits[p] = hit;
    } else if (keep && hit_before(index, sort, &hit, &hits[0])) {
        hits[0] = hit; sift_hits(index, sort, hits, *retained, 0);
    }
}

static void group_hit(const SearchIndex *index, SearchSort sort, SearchHit *groups, uint32_t group, SearchHit hit) {
    uint32_t matches = groups[group].matches + 1;
    if (!groups[group].matches || hit_before(index, sort, &hit, &groups[group]))
        groups[group] = hit;
    groups[group].matches = matches;
}

int search_query(const SearchIndex *index, SearchWorkspace *workspace, const char *text, const SearchFilter *filter,
                  SearchResult *result, const char **error) {
    Parser *parser;
    Query query;
    unsigned char positive[QUERY_TERMS];
    uint64_t *atoms;
    SearchHit *hits, *groups;
    size_t i, count = index->record_count, blocks = (count + 63) / 64;
    size_t base_bytes = (size_t)aligned(count * (3 * sizeof(uint32_t) + 1)), capacity, needed;
    size_t retained = 0, keep = 0, hit_capacity, group_capacity;
    unsigned int limit = filter->limit ? filter->limit : 20;
    SearchDate from, through;
    int root, filter_only;
    rt_memset(result, 0, sizeof(*result));
    *error = "cannot allocate query workspace";
    if (!text) text = "";
    {
        int checked = search_snapshot_check(index, filter->snapshot_id, filter->offset, error);
        if (checked) return checked;
    }
    if ((filter->organization_id && !search_actor_known(index, filter->organization_id, 1)) ||
        (filter->actor_id && !search_actor_known(index, filter->actor_id, 0))) {
        *error = "unknown organizationId/actorId in this snapshot"; return SEARCH_QUERY_INVALID;
    }
    if (limit > 100 || filter->sort > SEARCH_DATE_ASC || filter->group > SEARCH_DOCUMENTS || filter->mode > SEARCH_HYBRID ||
        filter->date_role >= SEARCH_REFERENCE_YEAR || rt_strlen(text) > 16384) {
        *error = "query/limit/sort/group exceeds supported bounds"; return -1;
    }
    if ((filter->kind && (!*filter->kind || !search_kind_known(index, filter->kind))) ||
        (filter->paper_type && !*filter->paper_type) || (filter->organization && !*filter->organization)) {
        *error = "unknown kind or empty metadata filter"; return SEARCH_QUERY_INVALID;
    }
    if (filter->group == SEARCH_DOCUMENTS && filter->kind &&
        rt_strcmp(filter->kind, "File") && rt_strcmp(filter->kind, "Page")) {
        *error = "document grouping requires File/Page records"; return SEARCH_QUERY_INVALID;
    }
    if ((filter->date_from && (search_date_parse(filter->date_from, &from) || from.precision != 1)) ||
        (filter->date_through && (search_date_parse(filter->date_through, &through) || through.precision != 1)) ||
        (filter->date_from && filter->date_through && from.day > through.day)) {
        *error = "expected ordered valid YYYY-MM-DD dateFrom/dateThrough"; return SEARCH_QUERY_INVALID;
    }
    result->group = filter->group;
    result->date_role = filter->date_role; result->sort = filter->sort;
    result->date_from = filter->date_from; result->date_through = filter->date_through;
    result->offset = filter->offset; result->limit = limit;
    result->mode = filter->mode;
    if (filter->mode && !index->embedding) {
        *error = "semantic search requires a verified embedding index and model; configure --embeddings";
        return SEARCH_QUERY_UNAVAILABLE;
    }
    if (!workspace->parser) workspace->parser = rt_malloc(sizeof(*parser));
    parser = (Parser *)workspace->parser;
    if (parser == 0) return SEARCH_QUERY_RESOURCE;
    rt_memset(parser, 0, sizeof(*parser));
    parser->query = filter->mode == SEARCH_SEMANTIC ? "" : text; parser->index = index;
    spaces(parser); filter_only = !text[parser->p]; result->filter_only = filter_only;
    if (filter_only && !filter->kind && !filter->year && !filter->paper_type && !filter->organization &&
        !filter->organization_id && !filter->actor_id &&
        !filter->date_from && !filter->date_through && filter->group != SEARCH_DOCUMENTS) {
        *error = "empty listing requires a kind, reference year, metadata/date filter or document group";
        return SEARCH_QUERY_INVALID;
    }
    if (filter->mode == SEARCH_SEMANTIC) filter_only = result->filter_only = 1;
    root = filter_only ? -1 : parse_or(parser, 0); spaces(parser);
    if ((!filter_only && root < 0) || parser->query[parser->p]) {
        *error = parser->error ? parser->error : "unexpected query operator"; return -1;
    }
    capacity = workspace->record_count == count && workspace->atom_capacity >= parser->atom_count ?
               workspace->atom_capacity : ((parser->atom_count ? parser->atom_count : 1) + 3U) & ~(size_t)3;
    if (filter->offset < count) {
        keep = (size_t)filter->offset + limit;
        if (keep > count) keep = count;
    }
    hit_capacity = workspace->record_count == count && workspace->hit_capacity >= keep ?
                   workspace->hit_capacity : keep;
    group_capacity = workspace->record_count == count ? workspace->group_capacity : 0;
    if (filter->group != SEARCH_RECORDS) group_capacity = count;
    needed = base_bytes + (capacity + 1) * blocks * sizeof(uint64_t) +
             (hit_capacity + group_capacity) * sizeof(SearchHit);
    if (index->memory_bytes + needed + sizeof(*parser) > SEARCH_RAM_LIMIT) {
        *error = "index plus query workspace exceeds the 4 GiB search RAM budget"; return SEARCH_QUERY_RESOURCE;
    }
    if (!workspace->allocation || workspace->record_count != count || workspace->atom_capacity != capacity ||
        workspace->hit_capacity != hit_capacity || workspace->group_capacity != group_capacity) {
        rt_free(workspace->allocation);
        workspace->allocation = rt_malloc(needed ? needed : 1);
        if (!workspace->allocation) return SEARCH_QUERY_RESOURCE;
        workspace->record_count = count; workspace->atom_capacity = capacity; workspace->hit_capacity = hit_capacity;
        workspace->group_capacity = group_capacity;
    }
    query.index = index; query.filter = filter;
    query.scores = (float *)workspace->allocation;
    query.bytes = (uint32_t *)(void *)(query.scores + count);
    query.owners = query.bytes + count;
    query.states = (unsigned char *)(query.owners + count);
    query.filtered = filter->year || filter->paper_type || filter->organization || filter->organization_id || filter->actor_id;
    atoms = (uint64_t *)(void *)((unsigned char *)workspace->allocation + base_bytes);
    query.seen = atoms + capacity * blocks;
    hits = (SearchHit *)(void *)(query.seen + blocks);
    groups = hits + hit_capacity;
    float *semantic_scores = 0;
    const char **semantic_texts = 0;
    if (filter->mode) {
        if (workspace->embedding_capacity != count) {
            rt_free(workspace->embedding_allocation);
            workspace->embedding_allocation = rt_malloc(count * (sizeof(float) + sizeof(char *)));
            workspace->embedding_capacity = count;
        }
        if (!workspace->embedding_allocation) return SEARCH_QUERY_RESOURCE;
        semantic_texts = workspace->embedding_allocation;
        semantic_scores = (float *)(void *)(semantic_texts + count);
        int code = search_embedding_scores(index, text, semantic_scores, semantic_texts, error);
        if (code) return code;
        result->filter_only = 0;
    }
    if (filter->group != SEARCH_RECORDS) rt_memset(groups, 0, count * sizeof(*groups));
    rt_memset(query.states, 0, count);
    rt_memset(atoms, 0, (parser->atom_count ? parser->atom_count : 1) * blocks * sizeof(uint64_t));
    rt_memset(query.seen, 0, blocks * sizeof(uint64_t));
    rt_memset(positive, 0, sizeof(positive));
    if (!filter_only) positive_atoms(parser, root, 1, positive);
    for (i = 0; i < parser->atom_count; i++)
        atom_eval(parser, &parser->atoms[i], atoms + i * blocks, positive[i], &query);
    if (filter_only && filter->kind) {
        for (i = 0; i < index->kind_count; i++) if (!rt_strcmp(index->strings + index->kinds[i].name, filter->kind)) {
            size_t j;
            for (j = 0; j < index->kinds[i].count; j++) {
                uint32_t record = index->kind_records[index->kinds[i].start + j];
                atoms[record / 64] |= 1ULL << (record % 64);
            }
        }
    }
    for (i = 0; i < blocks; i++) {
        uint64_t candidates = filter_only ? (filter->kind ? atoms[i] : UINT64_MAX) :
            evaluate(parser, root, atoms, i, blocks);
        if (filter->mode == SEARCH_SEMANTIC) candidates = 0;
        if (filter->mode) for (unsigned int b = 0; b < 64 && i * 64 + b < count; b++)
            if (semantic_scores[i * 64 + b] > -2) candidates |= 1ULL << b;
        if (i == blocks - 1 && count % 64) candidates &= (1ULL << (count % 64)) - 1;
        while (candidates) {
            unsigned int bit = (unsigned int)__builtin_ctzll(candidates);
            uint32_t record = (uint32_t)(i * 64 + bit), source = index->records[record].parent;
            int seen = (query.seen[i] & (1ULL << bit)) != 0;
            SearchHit hit, best;
            size_t j, contexts;
            int accepted = 0, unknown = 0, invalid_date = 0;
            candidates &= candidates - 1;
            if (!record_eligible(&query, record)) continue;
            hit.record = record; hit.byte = seen && query.bytes[record] != NONE ? query.bytes[record] : 0;
            hit.paper = query.owners[source == NONE ? record : source];
            hit.matches = 1;
            hit.score = seen ? query.scores[record] : 0;
            hit.semantic_text = filter->mode && semantic_scores[record] > -2 ? semantic_texts[record] : 0;
            hit.semantic_score = filter->mode && semantic_scores[record] > -2 ? semantic_scores[record] : -2;
            if (filter->mode) {
                float lexical = hit.score;
                hit.score = hit.semantic_score > -2 ? hit.semantic_score : 0;
                if (filter->mode == SEARCH_HYBRID && seen && lexical > 0)
                    hit.score += lexical / (1 + lexical);
            }
            if (source == NONE) source = record;
            if (filter->group == SEARCH_DOCUMENTS &&
                (rt_strcmp(index->strings + index->records[source].kind, "File") ||
                 index->version_lookup[source] == NONE)) {
                if (!rt_strcmp(index->strings + index->records[source].kind, "File")) result->excluded_unversioned++;
                continue;
            }
            contexts = index->links[source].count;
            for (j = 0; j < (contexts ? contexts : 1); j++) {
                const SearchDate *date;
                if (j < contexts) {
                    hit.paper = index->related_papers[index->links[source].start + j];
                    if (!paper_matches(&query, hit.paper)) continue;
                } else {
                    if (query.filtered && !metadata_matches(index, &index->records[source], filter)) continue;
                    hit.paper = index->links[source].paper ? source : NONE;
                }
                search_hit_date(index, &hit, filter->date_role);
                date = search_hit_time(index, &hit);
                if (filter->date_from || filter->date_through) {
                    if (!date) { unknown = 1; continue; }
                    if (!date->precision) { invalid_date = 1; continue; }
                    if ((filter->date_from && date->day < from.day) ||
                        (filter->date_through && date->day > through.day)) continue;
                }
                if (filter->group == SEARCH_PAPERS)
                    group_hit(index, filter->sort, groups, hit.paper != NONE ? hit.paper : source, hit);
                if (!accepted || hit_before(index, filter->sort, &hit, &best)) best = hit;
                accepted = 1;
            }
            if (!accepted) {
                if (unknown) result->excluded_unknown_dates++;
                if (invalid_date) result->excluded_invalid_dates++;
                continue;
            }
            result->matched_records++;
            if (filter->group == SEARCH_DOCUMENTS)
                group_hit(index, filter->sort, groups, source, best);
            else if (filter->group == SEARCH_RECORDS) {
                result->total++;
                retain_hit(index, filter->sort, hits, &retained, keep, best);
            }
        }
    }
    if (filter->group != SEARCH_RECORDS) for (i = 0; i < count; i++) if (groups[i].matches) {
        result->total++;
        retain_hit(index, filter->sort, hits, &retained, keep, groups[i]);
    }
    for (i = retained; i > 1; i--) {
        SearchHit hit = hits[0]; hits[0] = hits[i - 1]; hits[i - 1] = hit;
        sift_hits(index, filter->sort, hits, i - 1, 0);
    }
    for (i = filter->offset; i < retained; i++) result->hits[result->count++] = hits[i];
    *error = 0;
    return 0;
}

const SearchRecord *search_find(const SearchIndex *index, const char *id, unsigned int page) {
    size_t i;
    if (index->identities) return search_identity_find(index, id, page);
    for (i = 0; i < index->record_count; i++) {
        const SearchRecord *r = &index->records[i];
        if (rt_strcmp(index->strings + r->id, id) == 0 && r->page == page) return r;
        if (rt_strcmp(index->strings + r->identity, id) == 0 && (!page || r->page == page)) return r;
    }
    for (i = 0; i < index->version_count; i++) if (!rt_strcmp(index->strings + index->versions[i].id, id)) {
        uint32_t file = index->versions[i].record;
        size_t j;
        if (!page) return &index->records[file];
        for (j = 0; j < index->record_count; j++)
            if (index->records[j].parent == file && index->records[j].page == page) return &index->records[j];
        return 0;
    }
    if (!page) for (i = 0; i < index->section_count; i++)
        if (!rt_strcmp(index->strings + index->sections[i].id, id))
            return &index->records[index->sections[i].record];
    return 0;
}

#include "search.h"
#include "runtime.h"
#include "hash_util.h"
#include "tool_json_parser.h"

#define NONE UINT32_MAX

static const char *const roles[SEARCH_RELATION_END] = {
    "", "paper.mainFile", "paper.auxiliaryFile", "agendaItem.resolutionFile",
    "agendaItem.auxiliaryFile", "meeting.auxiliaryFile", "meeting.invitation",
    "meeting.resultsProtocol", "meeting.verbatimProtocol", "paper.consultation",
    "consultation.paper", "consultation.agendaItem", "consultation.meeting",
    "agendaItem.consultation", "meeting.agendaItem", "paper.superordinatedPaper",
    "paper.subordinatedPaper", "paper.relatedPaper", "organization", "underDirectionOf",
    "body", "system", "agendaItem.meeting", "system.bodies", "context.system"
};
static const char *const dates[SEARCH_DATE_END] = {
    "", "paper.date", "document.date", "meeting.start", "meeting.end", "submittedAt",
    "decidedAt", "publishedAt", "effectiveAt", "created", "modified", "observedAt",
    "importedAt", "referenceYear"
};
static const char *const certainty[SEARCH_CERTAINTY_END] = {
    "", "source-assertion", "derived", "exact"
};
static const char *const authority[] = {"unknown", "preliminary", "authoritative"};

const char *search_relation_role_name(unsigned int role) {
    return role < SEARCH_RELATION_END ? roles[role] : "";
}

static int valid_text(const SearchIndex *index, uint32_t offset) {
    size_t i;
    for (i = offset; i < index->string_bytes; i++) if (!index->strings[i]) return 1;
    return 0;
}

size_t search_relation_start(const SearchIndex *index, uint32_t source) {
    size_t low = 0, high = index->relation_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (index->relations[mid].source < source) low = mid + 1;
        else high = mid;
    }
    return low;
}

static const SearchRelationSpec relation_specs[] = {
    {"Paper", "mainFile", SEARCH_MAIN_FILE},
    {"Paper", "auxiliaryFile", SEARCH_PAPER_ATTACHMENT},
    {"Paper", "consultation", SEARCH_CONSULTATION},
    {"Paper", "superordinatedPaper", SEARCH_SUPERORDINATED},
    {"Paper", "subordinatedPaper", SEARCH_SUBORDINATED},
    {"Paper", "relatedPaper", SEARCH_RELATED},
    {"Consultation", "paper", SEARCH_CONSULTATION_PAPER},
    {"Consultation", "agendaItem", SEARCH_AGENDA_ITEM},
    {"Consultation", "agendaitem", SEARCH_AGENDA_ITEM},
    {"Consultation", "meeting", SEARCH_MEETING},
    {"AgendaItem", "meeting", SEARCH_AGENDA_MEETING},
    {"AgendaItem", "consultation", SEARCH_AGENDA_CONSULTATION},
    {"AgendaItem", "resolutionFile", SEARCH_RESOLUTION_FILE},
    {"AgendaItem", "auxiliaryFile", SEARCH_AGENDA_ATTACHMENT},
    {"Meeting", "invitation", SEARCH_INVITATION},
    {"Meeting", "resultsProtocol", SEARCH_RESULTS_PROTOCOL},
    {"Meeting", "verbatimProtocol", SEARCH_VERBATIM_PROTOCOL},
    {"Meeting", "auxiliaryFile", SEARCH_MEETING_ATTACHMENT},
    {"Meeting", "agendaItem", SEARCH_MEETING_AGENDA},
    {0, "underDirectionOf", SEARCH_DIRECTION},
    {0, "organization", SEARCH_ORGANIZATION},
    {0, "body", SEARCH_BODY},
    {0, "system", SEARCH_SYSTEM}
};

const SearchRelationSpec *search_relation_specs(size_t *count) {
    *count = sizeof(relation_specs) / sizeof(relation_specs[0]);
    return relation_specs;
}

static size_t fact_start(const SearchIndex *index, uint32_t record) {
    size_t low = 0, high = index->fact_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (index->facts[mid].record < record) low = mid + 1;
        else high = mid;
    }
    return low;
}

static const SearchVersion *version_for(const SearchIndex *index, uint32_t record) {
    size_t low = 0, high = index->version_count;
    if (index->records[record].parent != NONE) record = index->records[record].parent;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (index->versions[mid].record < record) low = mid + 1;
        else high = mid;
    }
    return low < index->version_count && index->versions[low].record == record ? &index->versions[low] : 0;
}

int search_is_direct_source(const SearchIndex *index, uint32_t paper, const char *file) {
    size_t i;
    for (i = search_relation_start(index, paper); i < index->relation_count && index->relations[i].source == paper; i++) {
        const SearchRelation *r = &index->relations[i];
        if ((r->role == SEARCH_MAIN_FILE || r->role == SEARCH_PAPER_ATTACHMENT) &&
            !rt_strcmp(index->strings + r->target, file)) return 1;
    }
    return 0;
}

static const char *domain(uint32_t role) {
    size_t i, count;
    const SearchRelationSpec *specs = search_relation_specs(&count);
    if (role == SEARCH_BODY_COLLECTION) return "System";
    for (i = 0; i < count; i++)
        if (specs[i].role == role) return specs[i].source_kind;
    return 0;
}

int search_model_validate(const SearchIndex *index) {
    size_t i, j;
    for (i = 0; i < index->record_count; i++) {
        const SearchRecord *r = &index->records[i];
        if (!index->records[i].identity || !index->strings[index->records[i].identity]) return -1;
        if (r->parent != NONE) {
            if (r->preparation != index->records[r->parent].preparation &&
                (index->format_version < 5 || search_preparation_validate(index, r))) return -1;
        } else if (r->preparation) {
            ToolJson json;
            const SearchVersion *version = version_for(index, (uint32_t)i);
            const char *summary = index->strings + r->preparation;
            const ToolJsonToken *fields;
            char hash[65];
            unsigned long long format;
            int invalid;
            if (rt_strcmp(index->strings + r->kind, "File") || !version ||
                tool_json_parse(&json, summary, rt_strlen(summary))) return -1;
            fields = tool_json_token(&json, tool_json_get(&json, 0, "fields"));
            invalid = tool_json_uint(&json, tool_json_get(&json, 0, "format"), &format) || format != 1 ||
                tool_json_string(&json, tool_json_get(&json, 0, "sourceSha256"), hash, sizeof(hash)) ||
                rt_strcmp(hash, index->strings + version->hash) || !fields || fields->kind != TOOL_JSON_ARRAY;
            tool_json_free(&json);
            if (invalid) return -1;
        }
    }
    for (i = 0; i < index->evidence_count; i++) {
        const SearchEvidence *e = &index->evidence[i];
        uint32_t offsets[] = {e->object,e->version,e->section,e->property,e->rule};
        for (j = 0; j < sizeof(offsets)/sizeof(offsets[0]); j++)
            if (!valid_text(index, offsets[j])) return -1;
        if (!e->object || !e->rule || !e->certainty || e->certainty >= SEARCH_CERTAINTY_END ||
            e->reserved || (e->page && !e->version) || (e->section && !e->page)) return -1;
    }
    for (i = 0; i < index->relation_count; i++) {
        const SearchRelation *r = &index->relations[i];
        const char *expected;
        if (r->source >= index->record_count || !valid_text(index, r->target) || !r->target ||
            r->evidence >= index->evidence_count || !r->role || r->role >= SEARCH_RELATION_END ||
            r->authority > SEARCH_AUTHORITATIVE || r->reserved ||
            (i && index->relations[i-1].source > r->source)) return -1;
        expected = domain(r->role);
        if (expected && rt_strcmp(index->strings + index->records[r->source].kind, expected)) return -1;
        if (r->authority && rt_strcmp(index->strings + index->records[r->source].kind, "Consultation")) return -1;
        if (r->role == SEARCH_CONTEXT_SYSTEM) {
            if (index->evidence[r->evidence].certainty != SEARCH_DERIVED ||
                rt_strcmp(index->strings + index->evidence[r->evidence].object,
                          index->strings + index->records[r->source].body) ||
                rt_strcmp(index->strings + r->target, index->strings + index->records[r->source].system)) return -1;
        } else if (rt_strcmp(index->strings + index->evidence[r->evidence].object,
                            index->strings + index->records[r->source].id)) return -1;
    }
    for (i = 0; i < index->fact_count; i++) {
        const SearchFact *f = &index->facts[i];
        if (f->record >= index->record_count || !f->role || f->role >= SEARCH_DATE_END ||
            !valid_text(index, f->value) || !f->value || f->evidence >= index->evidence_count ||
            (i && (index->facts[i-1].record > f->record ||
                   (index->facts[i-1].record == f->record && index->facts[i-1].role >= f->role)))) return -1;
    }
    for (i = 0; i < index->version_count; i++) {
        const SearchVersion *v = &index->versions[i];
        if (v->record >= index->record_count || !valid_text(index, v->id) || !v->id ||
            !valid_text(index, v->hash) || rt_strlen(index->strings + v->hash) != 64 || v->reserved ||
            v->fetched_at > v->observed_at || v->observed_at > 0x7fffffffffffffffULL ||
            rt_strcmp(index->strings + index->records[v->record].kind, "File") ||
            (i && index->versions[i-1].record >= v->record)) return -1;
        for (j = 0; j < 64; j++) if (tool_hex_value(index->strings[v->hash+j]) < 0) return -1;
    }
    for (i = 0; i < index->section_count; i++) {
        const SearchSection *s = &index->sections[i];
        const SearchRecord *r;
        const SearchEvidence *e;
        const SearchVersion *v;
        if (s->record >= index->record_count || !valid_text(index, s->id) || !s->id ||
            s->evidence >= index->evidence_count || s->reserved ||
            (i && index->sections[i-1].record >= s->record)) return -1;
        r = &index->records[s->record]; e = &index->evidence[s->evidence];
        v = version_for(index, s->record);
        if (!r->page || r->parent == NONE || !v || s->start < r->body_start ||
            s->start > r->text_length || s->length > r->text_length - s->start ||
            e->page != r->page || rt_strcmp(index->strings + e->object, index->strings + r->id) ||
            rt_strcmp(index->strings + e->section, index->strings + s->id) ||
            rt_strcmp(index->strings + e->version, index->strings + v->id)) return -1;
    }
    for (i = 0; i < index->owner_count; i++) {
        const SearchOwner *o = &index->owners[i];
        if (rt_strcmp(index->strings + index->records[o->record].kind, "File") ||
            rt_strcmp(index->strings + index->records[o->paper].kind, "Paper")) return -1;
        if (!search_is_direct_source(index, o->paper, index->strings + index->records[o->record].id)) return -1;
    }
    return 0;
}

static int put(ToolByteBuffer *out, const char *text) {
    return tool_byte_buffer_append_cstr(out, text);
}
static int value(ToolByteBuffer *out, const SearchIndex *index, uint32_t offset) {
    return offset ? tool_json_append_string(out, index->strings + offset) : put(out, "null");
}
static int key(ToolByteBuffer *out, const char *name) {
    return put(out, ",") != 0 || tool_json_append_string(out, name) != 0 || put(out, ":") != 0 ? -1 : 0;
}
static int field(ToolByteBuffer *out, const SearchIndex *index, const char *name, uint32_t offset) {
    return key(out, name) != 0 || value(out, index, offset) != 0 ? -1 : 0;
}
static int proof(ToolByteBuffer *out, const SearchIndex *index, uint32_t n) {
    const SearchEvidence *e = &index->evidence[n];
    return put(out, "{\"sourceObject\":") != 0 || value(out, index, e->object) != 0 ||
        field(out, index, "documentVersion", e->version) != 0 || key(out, "page") != 0 ||
        (e->page ? tool_byte_buffer_append_uint(out, e->page) : put(out, "null")) != 0 ||
        field(out, index, "section", e->section) != 0 || field(out, index, "property", e->property) != 0 ||
        field(out, index, "rule", e->rule) != 0 || key(out, "certainty") != 0 ||
        tool_json_append_string(out, certainty[e->certainty]) != 0 || put(out, "}") != 0 ? -1 : 0;
}
static int fact(ToolByteBuffer *out, const SearchIndex *index, const SearchFact *f) {
    return put(out, "{\"value\":") != 0 || value(out, index, f->value) != 0 ||
        key(out, "evidence") != 0 || proof(out, index, f->evidence) != 0 || put(out, "}") != 0 ? -1 : 0;
}
static unsigned int scope(uint32_t role) {
    if (role == SEARCH_MAIN_FILE || role == SEARCH_PAPER_ATTACHMENT) return 0;
    if (role >= SEARCH_RESOLUTION_FILE && role <= SEARCH_MEETING_AGENDA) return 1;
    if (role >= SEARCH_SUPERORDINATED && role <= SEARCH_RELATED) return 2;
    if (role == SEARCH_AGENDA_MEETING) return 1;
    return 3;
}

int search_model_json(ToolByteBuffer *out, const SearchIndex *index, const SearchRecord *record) {
    static const char *const groups[] = {"directSources","meetingContext","relatedPapers","structure"};
    uint32_t n = (uint32_t)(record - index->records);
    uint32_t object = record->parent == NONE ? n : record->parent;
    const SearchVersion *version = version_for(index, n);
    size_t i, j, start = search_relation_start(index, object), first_fact = fact_start(index, object);
    if (put(out, "{\"identity\":") != 0 || value(out, index, record->identity) != 0 ||
        field(out, index, "objectId", record->id) != 0 || field(out, index, "kind", record->kind) != 0 ||
        key(out, "metadataOrigin") != 0 ||
        tool_json_append_string(out, record->metadata_origin == 2 ? "archive-resource" :
            record->metadata_origin == 1 ? "embedded" : "archive-file") != 0 ||
        key(out, "snapshotId") != 0 || tool_json_append_string(out, index->snapshot_id) != 0 ||
        put(out, ",\"context\":{\"system\":") != 0 || value(out, index, record->system) != 0 ||
        field(out, index, "body", record->body) != 0 || put(out, "},\"initiative\":null") != 0) return -1;
    if (!rt_strcmp(index->strings + record->kind, "Paper") &&
        put(out, ",\"initiativeStatus\":\"not-established; parents form a graph, not an initiative\","
            "\"documentRole\":null") != 0) return -1;
    if (put(out, ",\"documentVersion\":") != 0) return -1;
    if (version) {
        if (put(out, "{\"id\":") != 0 || value(out, index, version->id) != 0 ||
            field(out, index, "sha256", version->hash) != 0 || key(out, "bytes") != 0 ||
            tool_byte_buffer_append_uint(out, version->bytes) != 0 || key(out, "observedAtEpochSeconds") != 0 ||
            (version->observed_at ? tool_byte_buffer_append_uint(out, version->observed_at) : put(out, "null")) != 0 ||
            key(out, "fetchedAtEpochSeconds") != 0 ||
            (version->fetched_at ? tool_byte_buffer_append_uint(out, version->fetched_at) : put(out, "null")) != 0 ||
            put(out, "}") != 0) return -1;
    } else if (put(out, "null") != 0) return -1;
    if (put(out, ",\"sections\":[") != 0) return -1;
    for (i = 0; i < index->section_count; i++) if (index->sections[i].record == n) {
        const SearchSection *s = &index->sections[i];
        if (put(out, "{\"id\":") != 0 || value(out, index, s->id) != 0 ||
            key(out, "byteStart") != 0 || tool_byte_buffer_append_uint(out, s->start - record->body_start) != 0 ||
            key(out, "byteLength") != 0 || tool_byte_buffer_append_uint(out, s->length) != 0 ||
            key(out, "evidence") != 0 || proof(out, index, s->evidence) != 0 || put(out, "}") != 0) return -1;
    }
    if (put(out, "],\"dates\":{") != 0) return -1;
    for (i = 1; i < SEARCH_DATE_END; i++) {
        const SearchFact *found = 0;
        for (j = first_fact; j < index->fact_count && index->facts[j].record == object; j++)
            if (index->facts[j].role == i) { found = &index->facts[j]; break; }
        if ((i > 1 && put(out, ",") != 0) || tool_json_append_string(out, dates[i]) != 0 || put(out, ":") != 0) return -1;
        if (found) { if (fact(out, index, found) != 0) return -1; }
        else if (i == SEARCH_IMPORTED) {
            if (put(out, "{\"epochSeconds\":") != 0 || tool_byte_buffer_append_uint(out, index->imported_at) != 0 ||
                put(out, ",\"evidence\":{\"sourceObject\":") != 0 || value(out, index, record->id) != 0 ||
                put(out, ",\"rule\":\"local-archive-index-import-v1\",\"scope\":\"local-archive-read\","
                    "\"certainty\":\"exact\"}}") != 0) return -1;
        } else if (put(out, "null") != 0) return -1;
    }
    if (put(out, "},\"events\":[") != 0) return -1;
    {
        int comma = 0;
        for (j = first_fact; j < index->fact_count && index->facts[j].record == object; j++) {
            const SearchFact *f = &index->facts[j];
            CryptoSha256Context sha;
            unsigned char digest[32];
            char hex[65];
            if (f->role < SEARCH_MEETING_START || f->role > SEARCH_EFFECTIVE) continue;
            crypto_sha256_init(&sha);
            crypto_sha256_update(&sha, (const unsigned char *)index->strings + record->id,
                                 rt_strlen(index->strings + record->id) + 1);
            crypto_sha256_update(&sha, (const unsigned char *)dates[f->role], rt_strlen(dates[f->role]) + 1);
            crypto_sha256_update(&sha, (const unsigned char *)index->strings + f->value,
                                 rt_strlen(index->strings + f->value));
            crypto_sha256_final(&sha, digest); hash_to_hex(digest, 32, hex);
            if ((comma && put(out, ",") != 0) || put(out, "{\"id\":\"urn:oparl-search:event:") != 0 ||
                put(out, hex) != 0 || put(out, "\",\"dateRole\":") != 0 ||
                tool_json_append_string(out, dates[f->role]) != 0 || key(out, "time") != 0 ||
                fact(out, index, f) != 0 || put(out, "}") != 0) return -1;
            comma = 1;
        }
    }
    if (put(out, "],\"relations\":{") != 0) return -1;
    for (i = 0; i < sizeof(groups)/sizeof(groups[0]); i++) {
        int comma = 0;
        if ((i && put(out, ",") != 0) || tool_json_append_string(out, groups[i]) != 0 || put(out, ":[") != 0) return -1;
        for (j = start; j < index->relation_count && index->relations[j].source == object; j++) {
            const SearchRelation *r = &index->relations[j];
            if (scope(r->role) != i) continue;
            if ((comma && put(out, ",") != 0) || put(out, "{\"target\":") != 0 || value(out, index, r->target) != 0 ||
                key(out, "role") != 0 || tool_json_append_string(out, roles[r->role]) != 0 ||
                key(out, "authority") != 0 || tool_json_append_string(out, authority[r->authority]) != 0 ||
                key(out, "evidence") != 0 || proof(out, index, r->evidence) != 0 || put(out, "}") != 0) return -1;
            comma = 1;
        }
        if (put(out, "]") != 0) return -1;
    }
    return put(out, "}}");
}

#include "search.h"
#include "tool_json_parser.h"
#include "runtime.h"

#define NONE UINT32_MAX
/* JSON-RPC includes both JSON and its quoted text; reserve room for both copies. */
#define COMPACT_BYTES 7500U

static int record_date_json(ToolByteBuffer *out, const SearchIndex *index, uint32_t record, SearchDateRole role);

static const char *const actor_properties[] = {
    "underDirectionOf", "organization", "originatorPerson", "originatorOrganization", "person"
};
static const char *const metadata_fields[] = {
    "name","reference","date","paperType","description","fileName","mimeType","size","downloadUrl","accessUrl",
    "web","start","end","meetingState","cancelled","result","authoritative","number","order","role","deleted","public"
};
static int put(ToolByteBuffer *out, const char *text) { return tool_byte_buffer_append_cstr(out, text); }
static int key(ToolByteBuffer *out, const char *name) {
    return put(out, ",") || tool_json_append_string(out, name) || put(out, ":");
}
static int string(ToolByteBuffer *out, const char *name, const char *value) {
    return key(out, name) || tool_json_append_string(out, value);
}
static int uint(ToolByteBuffer *out, const char *name, unsigned long long value) {
    return key(out, name) || tool_byte_buffer_append_uint(out, value);
}
static int boolean(ToolByteBuffer *out, const char *name, int value) {
    return key(out, name) || put(out, value ? "true" : "false");
}
static int metadata(const SearchIndex *index, uint32_t record, ToolJson *json) {
    const char *text = index->strings + index->records[record].payload;
    return tool_json_parse(json, *text ? text : "{}", *text ? rt_strlen(text) : 2);
}
static int identity_compare(const void *a, const void *b) {
    const SearchIdentity *x = (const SearchIdentity *)a, *y = (const SearchIdentity *)b;
    int order = rt_strcmp(x->id, y->id);
    return order ? order : x->record < y->record ? -1 : x->record > y->record;
}
const SearchRecord *search_identity_find(const SearchIndex *index, const char *id, unsigned int page) {
    size_t low = 0, high = index->identity_count, i;
    while (low < high) {
        size_t mid = low + (high-low)/2;
        if (rt_strcmp(index->identities[mid].id, id) < 0) low = mid+1; else high = mid;
    }
    for (i = low; i < index->identity_count && !rt_strcmp(index->identities[i].id, id); i++) {
        const SearchRecord *r = &index->records[index->identities[i].record];
        if (r->page == page || (!page && !rt_strcmp(index->strings + r->identity, id))) return r;
    }
    if (low < index->identity_count && !rt_strcmp(index->identities[low].id, id)) {
        const SearchRecord *r = &index->records[index->identities[low].record];
        if (!page) return r;
        if (!r->page && rt_strcmp(index->strings + r->id, id))
            return search_identity_find(index, index->strings + r->id, page);
    }
    return 0;
}
static int actor_value(SearchIndex *index, const ToolJson *json, size_t value, uint32_t source, uint32_t property) {
    const ToolJsonToken *token = tool_json_token(json, value);
    size_t i;
    SearchActor *actor;
    size_t previous_capacity = index->actors.capacity;
    char *id;
    if (!token || token->kind == TOOL_JSON_NULL) return 0;
    if (token->kind == TOOL_JSON_ARRAY) {
        for (i = value+1; i < token->next; i = tool_json_token(json, i)->next)
            if (actor_value(index, json, i, source, property)) return -1;
        return 0;
    }
    if (token->kind == TOOL_JSON_OBJECT) return actor_value(index, json, tool_json_get(json, value, "id"), source, property);
    if (token->kind != TOOL_JSON_STRING) return search_error("actor relationship is not a string/object/array", 0);
    id = (char *)rt_malloc(token->end-token->start+1);
    if (!id) return -1;
    if (tool_json_string(json, value, id, token->end-token->start+1) || !*id) {
        rt_free(id); return search_error("invalid actor relationship ID", 0);
    }
    actor = (SearchActor *)tool_array_append(&index->actors);
    if (!actor) { rt_free(id); return -1; }
    actor->source = source; actor->property = property; actor->id = id;
    index->memory_bytes += (index->actors.capacity-previous_capacity)*sizeof(*actor) + rt_strlen(id)+1;
    return index->memory_bytes > 4ULL*1024*1024*1024 ?
        search_error("actor facets exceed the 4 GiB index budget", 0) : 0;
}
void search_research_free(SearchIndex *index) {
    size_t i;
    for (i = 0; i < index->actors.count; i++)
        rt_free(((SearchActor *)tool_array_get(&index->actors, i))->id);
    tool_array_free(&index->actors);
    rt_free(index->identities); rt_free(index->actor_start);
    rt_free(index->relation_targets);     rt_free(index->agenda_order); rt_free(index->agenda_access);
    rt_free(index->agenda_results); rt_free(index->meeting_results); rt_free(index->relation_hidden);
    rt_free(index->incoming_start); rt_free(index->incoming_relations);
}
int search_research_build(SearchIndex *index) {
    size_t i, j, count = index->record_count, capacity = count*2 + index->version_count + index->section_count;
    size_t bytes = (capacity ? capacity : 1)*sizeof(SearchIdentity) + (count+1)*sizeof(uint32_t) +
        (count ? count : 1)*sizeof(uint64_t) + (index->relation_count ? index->relation_count : 1)*2*sizeof(uint32_t) +
        (count+1)*sizeof(uint32_t) + count*sizeof(uint32_t) + (count ? count : 1)*2 +
        (index->relation_count ? index->relation_count : 1);
    if (index->memory_bytes + bytes > 4ULL*1024*1024*1024) return search_error("research facets exceed RAM budget", 0);
    index->identities = (SearchIdentity *)rt_malloc_array(capacity ? capacity : 1, sizeof(SearchIdentity));
    index->actor_start = (uint32_t *)rt_malloc_array(count+1, sizeof(uint32_t));
    index->agenda_order = (uint64_t *)rt_malloc_array(count ? count : 1, sizeof(uint64_t));
    index->agenda_access = (unsigned char *)rt_malloc(count ? count : 1);
    index->agenda_results = (unsigned char *)rt_malloc(count ? count : 1);
    index->meeting_results = (uint32_t *)rt_malloc_array(count ? count : 1, sizeof(uint32_t));
    index->relation_hidden = (unsigned char *)rt_malloc(index->relation_count ? index->relation_count : 1);
    index->relation_targets = (uint32_t *)rt_malloc_array(index->relation_count ? index->relation_count : 1, sizeof(uint32_t));
    index->incoming_start = (uint32_t *)rt_malloc_array(count+1, sizeof(uint32_t));
    index->incoming_relations = (uint32_t *)rt_malloc_array(index->relation_count ? index->relation_count : 1, sizeof(uint32_t));
    tool_array_init(&index->actors, sizeof(SearchActor));
    if (!index->identities || !index->actor_start || !index->agenda_order || !index->agenda_access || !index->relation_targets ||
        !index->incoming_start || !index->incoming_relations || !index->agenda_results ||
        !index->meeting_results || !index->relation_hidden)
        return search_error("cannot allocate research facets", 0);
    index->memory_bytes += bytes;
    rt_memset(index->incoming_start, 0, (count+1)*sizeof(uint32_t));
    rt_memset(index->agenda_results, 0, count);
    rt_memset(index->meeting_results, 0, count*sizeof(uint32_t));
    rt_memset(index->relation_hidden, 0, index->relation_count);
    for (i = 0; i < count; i++) {
        const SearchRecord *r = &index->records[i];
        SearchIdentity *entry = &index->identities[index->identity_count++];
        entry->id = index->strings + r->id; entry->record = (uint32_t)i;
        if (rt_strcmp(index->strings + r->id, index->strings + r->identity)) {
            entry = &index->identities[index->identity_count++];
            entry->id = index->strings + r->identity; entry->record = (uint32_t)i;
        }
    }
    for (i = 0; i < index->version_count; i++) {
        SearchIdentity *entry = &index->identities[index->identity_count++];
        entry->id = index->strings + index->versions[i].id; entry->record = index->versions[i].record;
    }
    for (i = 0; i < index->section_count; i++) {
        SearchIdentity *entry = &index->identities[index->identity_count++];
        entry->id = index->strings + index->sections[i].id; entry->record = index->sections[i].record;
    }
    rt_sort(index->identities, index->identity_count, sizeof(SearchIdentity), identity_compare);
    for (i = 0; i < count; i++) {
        const SearchRecord *r = &index->records[i];
        const char *kind = index->strings + r->kind;
        ToolJson json;
        index->actor_start[i] = (uint32_t)index->actors.count; index->agenda_order[i] = UINT64_MAX;
        index->agenda_access[i] = 0;
        for (j = 0; rt_strcmp(index->strings + index->kinds[j].name, kind); j++) {}
        if (r->warnings) index->kinds[j].warnings++;
        if (!rt_strcmp(kind, "File")) {
            if (index->version_lookup[i] == NONE) {
                index->kinds[j].unversioned++;
                if (r->warnings & 8) index->kinds[j].not_archived++;
                else index->kinds[j].missing_version_unknown++;
            } else if (r->warnings & 8) index->kinds[j].extraction_failed++;
        }
        if (r->parent != NONE) continue;
        if (metadata(index, (uint32_t)i, &json)) return search_error("cannot parse research metadata", index->strings + r->id);
        for (j = 0; j < sizeof(actor_properties)/sizeof(actor_properties[0]); j++)
            if (actor_value(index, &json, tool_json_get(&json, 0, actor_properties[j]), (uint32_t)i, (uint32_t)j)) {
                tool_json_free(&json); return -1;
            }
        if (!rt_strcmp(kind, "AgendaItem")) {
            unsigned long long order;
            const ToolJsonToken *access = tool_json_token(&json, tool_json_get(&json, 0, "public"));
            const ToolJsonToken *result = tool_json_token(&json, tool_json_get(&json, 0, "result"));
            if (!tool_json_uint(&json, tool_json_get(&json, 0, "order"), &order)) index->agenda_order[i] = order;
            if (access && access->kind == TOOL_JSON_BOOL)
                index->agenda_access[i] = tool_json_true(&json, tool_json_get(&json, 0, "public")) ? 1 : 2;
            index->agenda_results[i] = result && result->kind == TOOL_JSON_STRING && result->end-result->start > 2;
        }
        if (!rt_strcmp(kind, "Meeting")) {
            size_t value = tool_json_get(&json, 0, "agendaItem"), n;
            const ToolJsonToken *items = tool_json_token(&json, value);
            if (items) for (n = items->kind == TOOL_JSON_ARRAY ? value+1 : value;
                n < items->next; n = tool_json_token(&json, n)->next) {
                const ToolJsonToken *access = tool_json_token(&json, tool_json_get(&json, n, "public"));
                size_t id = tool_json_get(&json, n, "id"), k;
                const ToolJsonToken *identity = tool_json_token(&json, id);
                char *decoded;
                if (!access || access->kind != TOOL_JSON_BOOL || tool_json_true(&json, tool_json_get(&json, n, "public"))) continue;
                if (!identity || identity->kind != TOOL_JSON_STRING) continue;
                decoded = (char *)rt_malloc(identity->end-identity->start+1);
                if (!decoded || tool_json_string(&json, id, decoded, identity->end-identity->start+1)) {
                    rt_free(decoded); tool_json_free(&json);
                    return search_error("cannot decode nonpublic agenda reference", index->strings + r->id);
                }
                for (k = search_relation_start(index, (uint32_t)i); k < index->relation_count &&
                    index->relations[k].source == i; k++)
                    if (index->relations[k].role == SEARCH_MEETING_AGENDA &&
                        !rt_strcmp(decoded, index->strings + index->relations[k].target)) index->relation_hidden[k] = 1;
                rt_free(decoded);
            }
        }
        tool_json_free(&json);
    }
    index->actor_start[count] = (uint32_t)index->actors.count;
    for (i = 0; i < index->relation_count; i++) {
        const SearchRelation *relation = &index->relations[i];
        const SearchRecord *target = search_identity_find(index, index->strings + relation->target, 0);
        index->relation_targets[i] = target ? (uint32_t)(target-index->records) : NONE;
        if (target && relation->role == SEARCH_MEETING_AGENDA && index->agenda_access[target-index->records] == 2)
            index->relation_hidden[i] = 1;
        if (target) index->incoming_start[(size_t)(target-index->records)+1]++;
        if (!target) {
            for (j = 0; rt_strcmp(index->strings + index->kinds[j].name,
                index->strings + index->records[relation->source].kind); j++) {}
            index->kinds[j].unresolved++;
            if (index->relation_hidden[i]) index->kinds[j].nonpublic++;
            else index->kinds[j].unresolved_unknown++;
        }
    }
    for (i = 1; i <= count; i++) index->incoming_start[i] += index->incoming_start[i-1];
    {
        uint32_t *positions = (uint32_t *)rt_malloc_array(count ? count : 1, sizeof(uint32_t));
        if (!positions || index->memory_bytes + count*sizeof(uint32_t) > 4ULL*1024*1024*1024) {
            rt_free(positions); return search_error("cannot allocate incoming relationship positions within RAM budget", 0);
        }
        memcpy(positions, index->incoming_start, count*sizeof(uint32_t));
        for (i = 0; i < index->relation_count; i++)
            if (index->relation_targets[i] != NONE)
                index->incoming_relations[positions[index->relation_targets[i]]++] = (uint32_t)i;
        rt_free(positions);
    }
    for (i = 0; i < count; i++) if (index->agenda_results[i] && index->agenda_access[i] != 2) {
        uint32_t context = index->meeting_context[i];
        int context_seen = 0;
        for (j = index->incoming_start[i]; j < index->incoming_start[i+1]; j++) {
            const SearchRelation *r = &index->relations[index->incoming_relations[j]];
            if (r->role != SEARCH_MEETING_AGENDA) continue;
            index->meeting_results[r->source]++;
            if (r->source == context) context_seen = 1;
        }
        if (!context_seen && context < count) index->meeting_results[context]++;
    }
    return 0;
}
int search_actor_matches(const SearchIndex *index, uint32_t record, const char *id, int organization) {
    size_t i;
    if (!id) return 1;
    for (i = index->actor_start[record]; i < index->actor_start[record+1]; i++) {
        const SearchActor *actor = (const SearchActor *)tool_array_get_const(&index->actors, i);
        if ((!organization || actor->property <= 1) && !rt_strcmp(actor->id, id)) return 1;
    }
    return 0;
}
int search_actor_known(const SearchIndex *index, const char *id, int organization) {
    size_t i;
    const SearchRecord *record = search_identity_find(index, id, 0);
    if (record && (!rt_strcmp(index->strings + record->kind, "Organization") ||
        (!organization && !rt_strcmp(index->strings + record->kind, "Person")))) return 1;
    for (i = 0; i < index->actors.count; i++) {
        const SearchActor *actor = (const SearchActor *)tool_array_get_const(&index->actors, i);
        if ((!organization || actor->property <= 1) && !rt_strcmp(actor->id, id)) return 1;
    }
    return 0;
}
int search_snapshot_check(const SearchIndex *index, const char *snapshot, unsigned int offset, const char **error) {
    if (snapshot && rt_strcmp(snapshot, index->snapshot_id)) {
        *error = "snapshot changed: restart the research from offset 0 with the current snapshotId";
        return SEARCH_QUERY_SNAPSHOT;
    }
    if (offset && !snapshot) {
        *error = "pagination requires snapshotId from the first response"; return SEARCH_QUERY_INVALID;
    }
    return 0;
}
static int argument_string(const ToolJson *json, size_t value, const char **out,
                           char **storage, size_t *capacity) {
    const ToolJsonToken *token = tool_json_token(json, value);
    size_t used;
    if (!token || token->kind != TOOL_JSON_STRING || token->end-token->start+1 > *capacity ||
        tool_json_string(json, value, *storage, *capacity) || !**storage) return -1;
    *out = *storage; used = rt_strlen(*storage)+1; *storage += used; *capacity -= used;
    return 0;
}
int search_read_arguments(const ToolJson *json, size_t arguments, SearchRead *request,
                          char *storage, size_t capacity, const char **error) {
    static const char *const names[] = {"id","kind","fields","snapshotId","limit","offset","page","raw","ids","view","direction"};
    const ToolJsonToken *root = tool_json_token(json, arguments);
    unsigned int seen = 0;
    size_t i;
    rt_memset(request, 0, sizeof(*request));
    *error = "invalid research arguments: duplicate/unknown field, wrong type or out-of-range value";
    if (!root || root->kind != TOOL_JSON_OBJECT) return SEARCH_QUERY_INVALID;
    for (i = arguments+1; i < root->next;) {
        size_t n, value = i+1;
        const ToolJsonToken *token = tool_json_token(json, value);
        unsigned long long number;
        for (n = 0; n < sizeof(names)/sizeof(names[0]) && !tool_json_equals(json, i, names[n]); n++) {}
        if (n == sizeof(names)/sizeof(names[0]) || (seen & (1U << n)) || !token) return SEARCH_QUERY_INVALID;
        seen |= 1U << n;
        if (n < 4 || n >= 9) {
            const char **target = n == 0 ? &request->id : n == 1 ? &request->kind :
                n == 2 ? &request->fields : n == 3 ? &request->snapshot_id :
                n == 9 ? &request->view : &request->direction;
            if (argument_string(json, value, target, &storage, &capacity)) return SEARCH_QUERY_INVALID;
        } else if (n < 7) {
            if (tool_json_uint(json, value, &number) || number > (n == 4 ? 100 : n == 5 ? UINT32_MAX : 1000000) ||
                (n != 5 && !number)) return SEARCH_QUERY_INVALID;
            if (n == 4) request->limit = (unsigned int)number;
            else if (n == 5) request->offset = (unsigned int)number;
            else request->page = (unsigned int)number;
        } else if (n == 7) {
            if (token->kind != TOOL_JSON_BOOL) return SEARCH_QUERY_INVALID;
            request->raw = tool_json_true(json, value);
        } else {
            size_t child;
            if (token->kind != TOOL_JSON_ARRAY) return SEARCH_QUERY_INVALID;
            for (child = value+1; child < token->next; child = tool_json_token(json, child)->next) {
                if (request->id_count == 20 || argument_string(json, child, &request->ids[request->id_count],
                    &storage, &capacity)) return SEARCH_QUERY_INVALID;
                request->id_count++;
            }
            if (!request->id_count) return SEARCH_QUERY_INVALID;
        }
        i = token->next;
    }
    return 0;
}
int search_coverage_json(ToolByteBuffer *out, const SearchIndex *index) {
    size_t i;
    if (put(out, "{\"globalIncomplete\":") || put(out, index->incomplete ? "true" : "false") ||
        put(out, ",\"perType\":[")) return -1;
    for (i = 0; i < index->kind_count; i++) {
        const SearchFacet *f = &index->kinds[i];
        if ((i && put(out, ",")) || put(out, "{\"kind\":") || tool_json_append_string(out, index->strings + f->name) ||
            uint(out, "records", f->count) || uint(out, "localWarningRecords", f->warnings) ||
            uint(out, "unresolvedRelationTargets", f->unresolved) ||
            uint(out, "filesWithoutIndexedVersion", f->unversioned) ||
            put(out, ",\"fileVersionReasons\":{\"notArchivedAtIndexBuild\":") || tool_byte_buffer_append_uint(out, f->not_archived) ||
            uint(out, "unknown", f->missing_version_unknown) ||
            uint(out, "binaryPresentExtractionFailed", f->extraction_failed) ||
            put(out, "},\"unresolvedReasons\":{\"explicitlyNonpublic\":") || tool_byte_buffer_append_uint(out, f->nonpublic) ||
            uint(out, "unknown", f->unresolved_unknown) || put(out, "}}")) return -1;
    }
    return put(out, "]}");
}
static int projection_valid(const char *fields) {
    if (fields && !*fields) return 0;
    while (fields && *fields) {
        size_t n = 0, i;
        while (fields[n] && fields[n] != ',') n++;
        for (i = 0; i < sizeof(metadata_fields)/sizeof(metadata_fields[0]); i++)
            if (rt_strlen(metadata_fields[i]) == n && !rt_strncmp(fields, metadata_fields[i], n)) break;
        if (!n || i == sizeof(metadata_fields)/sizeof(metadata_fields[0])) return 0;
        fields += n; if (*fields && !*++fields) return 0;
    }
    return fields == 0 || *fields == 0;
}
static int selected(const char *fields, const char *name) {
    if (!fields) return 1;
    while (*fields) {
        size_t n = 0;
        while (fields[n] && fields[n] != ',') n++;
        if (n == rt_strlen(name) && !rt_strncmp(fields, name, n)) return 1;
        fields += n; if (*fields) fields++;
    }
    return 0;
}
static int projected(ToolByteBuffer *out, const ToolJson *json, const char *fields, int raw,
                     ToolByteBuffer *omitted) {
    size_t i;
    int comma = 0;
    if (put(out, "{")) return -1;
    for (i = 0; i < sizeof(metadata_fields)/sizeof(metadata_fields[0]); i++) {
        const char *name = metadata_fields[i];
        const ToolJsonToken *token = tool_json_token(json, tool_json_get(json, 0, name));
        if (!selected(fields, name) || !token) continue;
        if (token->kind == TOOL_JSON_ARRAY || token->kind == TOOL_JSON_OBJECT || (!raw && token->end-token->start > 514)) {
            if ((omitted->size && put(omitted, ",")) || tool_json_append_string(omitted, name)) return -1;
            continue;
        }
        if ((comma && put(out, ",")) || tool_json_append_string(out, name) || put(out, ":") ||
            tool_byte_buffer_append(out, json->text + token->start, token->end-token->start)) return -1;
        comma = 1;
    }
    return put(out, "}");
}
static const char *lifecycle(const ToolJson *json) {
    char state[128];
    if (tool_json_true(json, tool_json_get(json, 0, "cancelled"))) return "cancelled";
    if (tool_json_string(json, tool_json_get(json, 0, "meetingState"), state, sizeof(state))) return "unknown";
    if (!rt_strcmp(state, "abgesagt")) return "cancelled";
    if (!rt_strcmp(state, "eingeladen") || !rt_strcmp(state, "geplant")) return "planned";
    if (!rt_strcmp(state, "durchgef\xc3\xbchrt") || !rt_strcmp(state, "durchgefuehrt") || !rt_strcmp(state, "abgeschlossen")) return "held";
    return "unknown";
}
static int meeting_state(ToolByteBuffer *out, const SearchIndex *index, uint32_t record, const ToolJson *json) {
    const char *state = lifecycle(json);
    int conflict = index->meeting_results[record] &&
        (!rt_strcmp(state, "planned") || !rt_strcmp(state, "cancelled"));
    return string(out, "lifecycle", state) ||
        string(out, "agendaStatus", !rt_strcmp(state, "planned") ? "provisional" : "unknown") ||
        uint(out, "agendaItemsWithResult", index->meeting_results[record]) ||
        boolean(out, "statusConflict", conflict);
}
static int citation(ToolByteBuffer *out, const SearchIndex *index, uint32_t record, int compact) {
    const SearchRecord *r = &index->records[record];
    uint32_t source = r->parent == NONE ? record : r->parent, version = index->version_lookup[source];
    if (put(out, "{")) return -1;
    if (!compact && (put(out, "\"objectId\":") || tool_json_append_string(out, index->strings + r->id) ||
        string(out, "snapshotId", index->snapshot_id) || put(out, ","))) return -1;
    if (put(out, "\"page\":") || tool_byte_buffer_append_uint(out, r->page) ||
        ((!compact || rt_strcmp(index->strings + r->identity, index->strings + r->id)) &&
            string(out, "identity", index->strings + r->identity)) || key(out, "documentVersion")) return -1;
    if (version != NONE) {
        const SearchVersion *v = &index->versions[version];
        if (put(out, "{\"id\":") || tool_json_append_string(out, index->strings + v->id) ||
            string(out, "sha256", index->strings + v->hash) || uint(out, "bytes", v->bytes) || put(out, "}")) return -1;
    } else if (put(out, "null")) return -1;
    return boolean(out, "originalUrlMayChange", 1) || put(out, "}");
}
typedef struct { uint32_t id, record, parent, relation, priority, nanos; uint64_t order; } Entry;
static int entry_append(ToolArray *entries, const SearchIndex *index, uint32_t id, uint32_t record,
                        uint32_t parent, uint32_t relation) {
    Entry *entry = (Entry *)tool_array_append(entries);
    if (!entry) return -1;
    entry->id = id; entry->record = record; entry->parent = parent; entry->relation = relation;
    entry->priority = entry->nanos = 0;
    entry->order = record == NONE ? UINT64_MAX : index->agenda_order[record];
    return 0;
}
static int entry_add(ToolArray *entries, const SearchIndex *index, uint32_t id, uint32_t record,
                     uint32_t parent, uint32_t relation) {
    size_t i;
    for (i = 0; i < entries->count; i++)
        if (!rt_strcmp(index->strings + ((const Entry *)tool_array_get_const(entries, i))->id, index->strings + id)) return 0;
    return entry_append(entries, index, id, record, parent, relation);
}
static int expand(ToolArray *entries, const SearchIndex *index, uint32_t parent, int root, int meeting_root) {
    const Entry entry = *(const Entry *)tool_array_get_const(entries, parent);
    const char *kind;
    size_t i;
    if (entry.record == NONE) return 0;
    kind = index->strings + index->records[entry.record].kind;
    if (root && !rt_strcmp(kind, "Paper"))
        for (i = search_relation_start(index, entry.record); i < index->relation_count &&
            index->relations[i].source == entry.record; i++)
            if (index->relations[i].role == SEARCH_MAIN_FILE &&
                entry_add(entries, index, index->relations[i].target, index->relation_targets[i], parent, (uint32_t)i)) return -1;
    if (root && !rt_strcmp(kind, "Paper"))
        for (i = search_relation_start(index, entry.record); i < index->relation_count &&
            index->relations[i].source == entry.record; i++)
            if (index->relations[i].role == SEARCH_CONSULTATION &&
                entry_add(entries, index, index->relations[i].target, index->relation_targets[i], parent, (uint32_t)i)) return -1;
    if (root && !rt_strcmp(kind, "Paper"))
        for (i = index->incoming_start[entry.record]; i < index->incoming_start[entry.record+1]; i++) {
            uint32_t relation = index->incoming_relations[i];
            const SearchRelation *r = &index->relations[relation];
            if (r->role == SEARCH_CONSULTATION_PAPER && entry_add(entries, index,
                index->records[r->source].id, r->source, parent, relation)) return -1;
        }
    for (i = search_relation_start(index, entry.record); i < index->relation_count && index->relations[i].source == entry.record; i++) {
        const SearchRelation *r = &index->relations[i];
        int take = !rt_strcmp(kind, "Paper") ? root && (r->role == SEARCH_MAIN_FILE ||
            r->role == SEARCH_PAPER_ATTACHMENT || r->role == SEARCH_CONSULTATION ||
            r->role == SEARCH_SUPERORDINATED || r->role == SEARCH_SUBORDINATED ||
            r->role == SEARCH_RELATED || r->role == SEARCH_DIRECTION) :
            !rt_strcmp(kind, "Consultation") ? r->role == SEARCH_MEETING || r->role == SEARCH_AGENDA_ITEM || r->role == SEARCH_ORGANIZATION :
            !rt_strcmp(kind, "AgendaItem") ? r->role == SEARCH_AGENDA_MEETING || r->role == SEARCH_RESOLUTION_FILE ||
                r->role == SEARCH_AGENDA_ATTACHMENT :
            !rt_strcmp(kind, "Meeting") ? r->role == SEARCH_ORGANIZATION || (meeting_root && (
                r->role == SEARCH_INVITATION || r->role == SEARCH_RESULTS_PROTOCOL || r->role == SEARCH_VERBATIM_PROTOCOL ||
                r->role == SEARCH_MEETING_ATTACHMENT)) : 0;
        if (take && entry_add(entries, index, r->target, index->relation_targets[i], parent, (uint32_t)i)) return -1;
    }
    return 0;
}
static int path(ToolByteBuffer *out, const SearchIndex *index, const ToolArray *entries, const Entry *entry) {
    uint32_t chain[8];
    unsigned char incoming[8];
    size_t used = 0;
    while (entry->relation != NONE && used < 8) {
        const Entry *parent = (const Entry *)tool_array_get_const(entries, entry->parent);
        incoming[used] = index->relations[entry->relation].source != parent->record;
        chain[used++] = entry->relation;
        entry = parent;
    }
    if (entry->relation != NONE) return search_error("research path exceeds eight steps", 0);
    if (put(out, "[")) return -1;
    while (used) {
        const SearchRelation *r = &index->relations[chain[--used]];
        if (put(out, "{\"source\":") || tool_json_append_string(out, index->strings + index->records[r->source].id) ||
            string(out, "role", search_relation_role_name(r->role)) ||
            string(out, "target", index->strings + r->target) ||
            string(out, "direction", incoming[used] ? "incoming" : "outgoing") ||
            put(out, "}") || (used && put(out, ","))) return -1;
    }
    return put(out, "]");
}
static int linked_summary(ToolByteBuffer *out, const SearchIndex *index, uint32_t record) {
    ToolJson json;
    ToolByteBuffer omitted;
    int status;
    tool_byte_buffer_init(&omitted);
    if (metadata(index, record, &json)) return -1;
    status = put(out, "{\"id\":") || tool_json_append_string(out, index->strings + index->records[record].id) ||
        key(out, "metadata") || projected(out, &json, "name,start,meetingState,cancelled,number,result,authoritative", 0, &omitted) ||
        key(out, "omittedFields") || put(out, "[") || tool_byte_buffer_append(out, omitted.data, omitted.size) || put(out, "]");
    if (!status && !rt_strcmp(index->strings + index->records[record].kind, "Meeting"))
        status = meeting_state(out, index, record, &json);
    if (!status) status = put(out, "}");
    tool_json_free(&json); tool_byte_buffer_free(&omitted); return status;
}
static int consultation(ToolByteBuffer *out, const SearchIndex *index, uint32_t record, const ToolJson *json) {
    const ToolJsonToken *flag = tool_json_token(json, tool_json_get(json, 0, "authoritative"));
    size_t i;
    unsigned int emitted = 0, total = 0;
    if (key(out, "consultation") || put(out, "{\"authority\":") ||
        tool_json_append_string(out, flag && flag->kind == TOOL_JSON_BOOL ?
            tool_json_true(json, tool_json_get(json, 0, "authoritative")) ? "authoritative" : "preliminary" : "unknown") ||
        string(out, "authoritySource", index->strings + index->records[record].id) ||
        put(out, ",\"context\":[")) return -1;
    for (i = search_relation_start(index, record); i < index->relation_count && index->relations[i].source == record; i++) {
        const SearchRelation *r = &index->relations[i];
        uint32_t target = index->relation_targets[i];
        if (r->role != SEARCH_MEETING && r->role != SEARCH_AGENDA_ITEM && r->role != SEARCH_ORGANIZATION) continue;
        total++;
        if (emitted >= 3) continue;
        if ((emitted && put(out, ",")) || put(out, "{\"role\":") ||
            tool_json_append_string(out, search_relation_role_name(r->role)) || key(out, "object")) return -1;
        if (target == NONE) {
            if (put(out, "{\"id\":") || tool_json_append_string(out, index->strings + r->target) || boolean(out, "indexed", 0) || put(out, "}")) return -1;
        } else if (linked_summary(out, index, target)) return -1;
        if (put(out, "}")) return -1;
        emitted++;
    }
    {
        int direct_organization = 0;
        for (i = search_relation_start(index, record); i < index->relation_count && index->relations[i].source == record; i++)
            if (index->relations[i].role == SEARCH_ORGANIZATION) direct_organization = 1;
        if (!direct_organization) for (i = search_relation_start(index, record);
            i < index->relation_count && index->relations[i].source == record; i++) {
            uint32_t meeting = index->relation_targets[i];
            size_t j;
            if (index->relations[i].role != SEARCH_MEETING || meeting == NONE) continue;
            for (j = search_relation_start(index, meeting); j < index->relation_count && index->relations[j].source == meeting; j++) {
                uint32_t target = index->relation_targets[j];
                if (index->relations[j].role != SEARCH_ORGANIZATION) continue;
                total++;
                if (emitted == 3) continue;
                if ((emitted && put(out, ",")) || put(out, "{\"role\":\"meeting.organization\"") ||
                    string(out, "via", index->strings + index->records[meeting].id) || key(out, "object")) return -1;
                if (target == NONE) {
                    if (put(out, "{\"id\":") || tool_json_append_string(out, index->strings + index->relations[j].target) ||
                        boolean(out, "indexed", 0) || put(out, "}")) return -1;
                } else if (linked_summary(out, index, target)) return -1;
                if (put(out, "}")) return -1;
                emitted++;
            }
        }
    }
    if (put(out, "]") || uint(out, "contextTotal", total) ||
        boolean(out, "moreContextAvailable", total > emitted) || put(out, ",\"decisionSources\":[")) return -1;
    emitted = 0; total = 0;
    for (i = search_relation_start(index, record); i < index->relation_count && index->relations[i].source == record; i++) {
        uint32_t agenda = index->relation_targets[i];
        size_t j;
        if (index->relations[i].role != SEARCH_AGENDA_ITEM || agenda == NONE) continue;
        for (j = search_relation_start(index, agenda); j < index->relation_count && index->relations[j].source == agenda; j++) {
            const SearchRelation *r = &index->relations[j];
            uint32_t target = index->relation_targets[j];
            if (r->role != SEARCH_RESOLUTION_FILE) continue;
            total++;
            if (emitted == 3) continue;
            if ((emitted && put(out, ",")) || put(out, "{\"id\":") ||
                tool_json_append_string(out, index->strings + r->target) ||
                string(out, "sourceAgenda", index->strings + index->records[agenda].id) ||
                string(out, "role", search_relation_role_name(r->role)) ||
                boolean(out, "indexed", target != NONE) || key(out, "documentVersion")) return -1;
            if (target != NONE && index->version_lookup[target] != NONE) {
                const SearchVersion *v = &index->versions[index->version_lookup[target]];
                if (put(out, "{\"id\":") || tool_json_append_string(out, index->strings + v->id) ||
                    string(out, "sha256", index->strings + v->hash) || put(out, "}")) return -1;
            } else if (put(out, "null")) return -1;
            if (put(out, "}")) return -1;
            emitted++;
        }
    }
    return put(out, "]") || uint(out, "decisionSourcesTotal", total) ||
        boolean(out, "moreDecisionSourcesAvailable", total > emitted) || put(out, "}");
}
static int object_json(ToolByteBuffer *out, const SearchIndex *index, const ToolArray *entries,
                       const Entry *entry, const SearchRead *request, int collection) {
    const SearchRecord *r = entry->record == NONE ? 0 : &index->records[entry->record];
    const char *role = entry->relation == NONE ? "root" : search_relation_role_name(index->relations[entry->relation].role);
    const char *scope = !r ? "unknown" : !rt_strcmp(index->strings + r->kind, "Paper") ?
        entry->parent == NONE ? "paper" : "relatedPaper" :
        !rt_strcmp(index->strings + r->kind, "File") ?
            entry->relation != NONE && (index->relations[entry->relation].role == SEARCH_MAIN_FILE ||
                index->relations[entry->relation].role == SEARCH_PAPER_ATTACHMENT) ? "directSource" : "contextSource" : "context";
    ToolByteBuffer omitted;
    ToolJson json;
    int status;
    if (put(out, "{\"id\":") || tool_json_append_string(out, index->strings + entry->id) ||
        string(out, "kind", r ? index->strings + r->kind : "") || string(out, "scope", scope) ||
        boolean(out, "indexed", r != 0) || string(out, "sourceRole", role)) return -1;
    if (!collection) {
        if (key(out, "relationshipPath") || path(out, index, entries, entry)) return -1;
    } else if (entry->parent != NONE) {
        const Entry *parent = (const Entry *)tool_array_get_const(entries, entry->parent);
        if (string(out, "parentId", index->strings + parent->id) ||
            string(out, "relationshipDirection", index->relations[entry->relation].source == parent->record ?
                "outgoing" : "incoming")) return -1;
    }
    if (key(out, "metadata")) return -1;
    if (!r) return put(out, "null") || boolean(out, "rawAvailable", 0) || put(out, "}");
    if (metadata(index, entry->record, &json)) return -1;
    tool_byte_buffer_init(&omitted);
    status = projected(out, &json, request->fields, request->raw, &omitted) ||
        key(out, "omittedFields") || put(out, "[") || tool_byte_buffer_append(out, omitted.data, omitted.size) || put(out, "]") ||
        boolean(out, "rawAvailable", 1) || uint(out, "warnings", r->warnings) ||
        (!collection && string(out, "source", index->strings + r->source)) ||
        key(out, "citation") || citation(out, index, entry->record, collection);
    if (!status && !rt_strcmp(index->strings + r->kind, "Meeting"))
        status = meeting_state(out, index, entry->record, &json);
    if (!status && !rt_strcmp(index->strings + r->kind, "File"))
        status = string(out, "documentCoverage", index->version_lookup[entry->record] == NONE ?
            r->warnings & 8 ? "not-archived-at-index-build" : "missing-version-cause-unknown" :
            r->warnings & 8 ? "binary-present-extraction-failed" : "version-available");
    if (!status && r->preparation)
        status = key(out, "preparation") ||
            search_preparation_json(out, index, r, 0, collection ? 0 : 2);
    if (!status && !rt_strcmp(index->strings + r->kind, "Consultation")) status = consultation(out, index, entry->record, &json);
    if (!status && (!collection || index->actor_start[entry->record] != index->actor_start[entry->record+1])) {
        size_t i, count = 0;
        status = put(out, ",\"actors\":[");
        for (i = index->actor_start[entry->record]; !status && i < index->actor_start[entry->record+1] && count < 5; i++, count++) {
            const SearchActor *actor = (const SearchActor *)tool_array_get_const(&index->actors, i);
            status = (count && put(out, ",")) || put(out, "{\"id\":") || tool_json_append_string(out, actor->id) ||
                string(out, "sourceProperty", actor_properties[actor->property]) ||
                boolean(out, "indexed", search_identity_find(index, actor->id, 0) != 0) || put(out, "}");
        }
        if (!status) status = put(out, "]") || uint(out, "actorTotal",
            index->actor_start[entry->record+1]-index->actor_start[entry->record]);
    }
    if (!status) status = put(out, "}");
    tool_json_free(&json); tool_byte_buffer_free(&omitted); return status;
}
static int agenda_compare(const void *a, const void *b) {
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    return x->order < y->order ? -1 : x->order > y->order ? 1 : x->id < y->id ? -1 : x->id > y->id;
}
static int paper_compare(const void *a, const void *b) {
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->priority != y->priority) return x->priority < y->priority ? -1 : 1;
    if (x->priority == 1) {
        if (x->order != y->order) return x->order > y->order ? -1 : 1;
        if (x->nanos != y->nanos) return x->nanos > y->nanos ? -1 : 1;
    }
    return agenda_compare(a, b);
}
static void paper_order(ToolArray *entries, const SearchIndex *index) {
    size_t i;
    for (i = 1; i < entries->count; i++) {
        Entry *entry = (Entry *)tool_array_get(entries, i);
        unsigned int role = index->relations[entry->relation].role;
        entry->priority = role == SEARCH_MAIN_FILE ? 0 :
            role == SEARCH_CONSULTATION || role == SEARCH_CONSULTATION_PAPER ? 1 : 2;
        entry->order = i;
        if (entry->priority == 1) {
            uint32_t meeting = entry->record == NONE ? NONE : index->meeting_context[entry->record];
            uint32_t date = meeting >= index->record_count ? NONE :
                index->date_lookup[(size_t)meeting*SEARCH_DATE_END+SEARCH_MEETING_START];
            entry->order = date == NONE || index->dates[date].precision != 2 ? 0 :
                (uint64_t)index->dates[date].seconds ^ (1ULL << 63);
            entry->nanos = date == NONE ? 0 : index->dates[date].nanos;
        }
    }
    if (entries->count > 1) rt_sort((Entry *)entries->data+1, entries->count-1, sizeof(Entry), paper_compare);
}
static int next(ToolByteBuffer *out, const SearchIndex *index, const char *operation,
                const SearchRead *request, size_t offset, size_t total) {
    if (key(out, "next")) return -1;
    if (offset >= total) return put(out, "null");
    if (put(out, "{\"operation\":") || tool_json_append_string(out, operation) ||
        string(out, "snapshotId", index->snapshot_id) || uint(out, "offset", offset) ||
        uint(out, "limit", request->limit ? request->limit : 10)) return -1;
    if (request->id && string(out, "id", request->id)) return -1;
    if (request->kind && string(out, "kind", request->kind)) return -1;
    if (request->fields && string(out, "fields", request->fields)) return -1;
    if (request->view && string(out, "view", request->view)) return -1;
    if (request->direction && string(out, "direction", request->direction)) return -1;
    if (request->raw && boolean(out, "raw", 1)) return -1;
    if (request->id_count && put(out, ",\"retainArguments\":[\"ids\"]")) return -1;
    return put(out, "}");
}
static int paper_types_json(ToolByteBuffer *out, const SearchIndex *index) {
    ToolArray types;
    size_t i, j;
    int status = -1;
    tool_array_init(&types, sizeof(uint32_t));
    for (i = 0; i < index->record_count; i++) {
        uint32_t type = index->records[i].paper_type, *added;
        if (!type || rt_strcmp(index->strings + index->records[i].kind, "Paper")) continue;
        for (j = 0; j < types.count; j++)
            if (!rt_strcmp(index->strings + *(const uint32_t *)tool_array_get_const(&types, j), index->strings + type)) break;
        if (j != types.count) continue;
        added = (uint32_t *)tool_array_append(&types);
        if (!added) goto done;
        *added = type;
    }
    if (put(out, ",\"paperTypes\":[")) goto done;
    for (i = 0; i < types.count; i++)
        if ((i && put(out, ",")) || tool_json_append_string(out,
            index->strings + *(const uint32_t *)tool_array_get_const(&types, i))) goto done;
    status = put(out, "]");
done:
    tool_array_free(&types); return status;
}
static int relationship(ToolByteBuffer *out, const SearchIndex *index, uint32_t n, int full) {
    const SearchRelation *r = &index->relations[n];
    const SearchRecord *source = &index->records[r->source];
    const char *name = index->strings + source->name;
    if (put(out, "{\"source\":") || tool_json_append_string(out, index->strings + source->id) ||
        (rt_strlen(name) <= 512 ? string(out, "sourceName", name) : boolean(out, "sourceNameOmitted", 1)) ||
        string(out, "sourceKind", index->strings + source->kind) ||
        (r->role >= SEARCH_MEETING_ATTACHMENT && r->role <= SEARCH_VERBATIM_PROTOCOL &&
            (key(out, "sourceDate") || record_date_json(out, index, r->source, SEARCH_MEETING_START))) ||
        string(out, "role", search_relation_role_name(r->role)) ||
        string(out, "target", index->strings + r->target)) return -1;
    if (full) {
        const SearchEvidence *e = &index->evidence[r->evidence];
        if (boolean(out, "targetIndexed", index->relation_targets[n] != NONE) ||
            string(out, "authority", r->authority == SEARCH_AUTHORITATIVE ? "authoritative" :
                r->authority == SEARCH_PRELIMINARY ? "preliminary" : "unknown") ||
            string(out, "sourceObject", index->strings + e->object) ||
            string(out, "sourceProperty", index->strings + e->property) ||
            string(out, "rule", index->strings + e->rule) ||
            string(out, "certainty", e->certainty == SEARCH_EXACT ? "exact" :
                e->certainty == SEARCH_DERIVED ? "derived" : "asserted")) return -1;
    }
    return put(out, "}");
}
static int relations_json(ToolByteBuffer *out, const SearchIndex *index, uint32_t record,
                          const SearchRead *request, const char **error) {
    size_t i, total = 0, hidden = 0, returned = 0, offset = 0;
    size_t start = search_relation_start(index, record), end = start, incoming = index->incoming_start[record];
    unsigned int limit = request->limit ? request->limit : 10;
    int in = !request->direction || rt_strcmp(request->direction, "outgoing");
    int outgoing = !request->direction || rt_strcmp(request->direction, "incoming");
    while (end < index->relation_count && index->relations[end].source == record) end++;
    if (outgoing) for (i = start; i < end; i++) {
        if (index->relation_hidden[i]) hidden++; else total++;
    }
    if (in) for (i = incoming; i < index->incoming_start[record+1]; i++)
        if (!index->relation_hidden[index->incoming_relations[i]] &&
            (!outgoing || index->relations[index->incoming_relations[i]].source != record)) total++;
    if (put(out, "{\"snapshotId\":") || tool_json_append_string(out, index->snapshot_id) ||
        string(out, "id", index->strings + index->records[record].id) ||
        string(out, "view", "relations") || string(out, "countUnit", "relationship") ||
        uint(out, "total", total) || uint(out, "nonPublicReferences", hidden) ||
        uint(out, "offset", request->offset) || put(out, ",\"relationships\":[")) return SEARCH_QUERY_RESOURCE;
    for (i = 0; i < (outgoing ? end-start : 0) + (in ? index->incoming_start[record+1]-incoming : 0); i++) {
        uint32_t n = outgoing && i < end-start ? (uint32_t)(start+i) :
            index->incoming_relations[incoming+i-(outgoing ? end-start : 0)];
        ToolByteBuffer row;
        if (index->relation_hidden[n] || (outgoing && i >= end-start && index->relations[n].source == record)) continue;
        if (offset++ < request->offset) continue;
        if (returned == limit) break;
        tool_byte_buffer_init(&row);
        if (relationship(&row, index, n, 1)) { tool_byte_buffer_free(&row); return SEARCH_QUERY_RESOURCE; }
        if (out->size + row.size + 700 > COMPACT_BYTES) {
            tool_byte_buffer_free(&row);
            if (returned) break;
            *error = "one relationship exceeds compact budget; request raw metadata";
            return SEARCH_QUERY_RESOURCE;
        }
        if ((returned && put(out, ",")) || tool_byte_buffer_append(out, row.data, row.size)) {
            tool_byte_buffer_free(&row); return SEARCH_QUERY_RESOURCE;
        }
        tool_byte_buffer_free(&row); returned++;
    }
    return put(out, "]") || uint(out, "returned", returned) ||
        next(out, index, "get_object", request, request->offset+returned, total) || put(out, "}\n") ? SEARCH_QUERY_RESOURCE : 0;
}
int search_read_json(ToolByteBuffer *out, const SearchIndex *index, const char *operation,
                     const SearchRead *request, const char **error) {
    const SearchRecord *root = request->id ? search_find(index, request->id, request->page) : 0;
    ToolArray entries;
    size_t i, returned = 0, first = !rt_strcmp(operation, "get_agenda") ? 1 : 0;
    size_t nonpublic = 0, missing_agenda = 0, unspecified = 0, local_missing = 0, local_warnings = 0;
    unsigned int limit = request->limit ? request->limit : 10;
    int status = -1, checked = search_snapshot_check(index, request->snapshot_id, request->offset, error);
    if (checked) return checked;
    *error = "invalid research parameters, unknown projection field or resource";
    if (limit > 100 || !projection_valid(request->fields) ||
        (request->raw && request->offset && (!rt_strcmp(operation, "get_paper") || !rt_strcmp(operation, "get_meeting"))) ||
        (request->fields && ((request->raw && rt_strcmp(operation, "get_objects") && !first) || request->page)))
        return SEARCH_QUERY_INVALID;
    if ((!rt_strcmp(operation, "get_paper") && root && rt_strcmp(index->strings + root->kind, "Paper")) ||
        ((!rt_strcmp(operation, "get_meeting") || first) && root && rt_strcmp(index->strings + root->kind, "Meeting"))) {
        *error = "resource has a different type in this snapshot"; return SEARCH_READ_NOT_FOUND;
    }
    if ((!rt_strcmp(operation, "get_objects") && (request->id || request->kind || request->page)) ||
        (!rt_strcmp(operation, "get_search_options") && (request->id || request->id_count || request->page || request->raw)) ||
        (rt_strcmp(operation, "get_objects") && rt_strcmp(operation, "get_search_options") &&
            (!request->id || request->id_count || request->kind ||
            (request->page && rt_strcmp(operation, "get_object"))))) return SEARCH_QUERY_INVALID;
    if ((request->view && (rt_strcmp(request->view, "relations") || rt_strcmp(operation, "get_object") ||
            request->raw || request->page || request->fields)) ||
        (request->direction && (!request->view || (rt_strcmp(request->direction, "incoming") &&
            rt_strcmp(request->direction, "outgoing") && rt_strcmp(request->direction, "both")))) ||
        (!rt_strcmp(operation, "get_object") && !request->view && (request->offset || request->limit))) return SEARCH_QUERY_INVALID;
    if (request->id && !root) { *error = "resource/page is not in this snapshot"; return SEARCH_READ_NOT_FOUND; }
    if (request->view)
        return relations_json(out, index, (uint32_t)(root-index->records), request, error);
    if ((!rt_strcmp(operation, "get_object") || !rt_strcmp(operation, "get_meeting")) &&
        root && (request->raw || root->page || request->page))
        return search_record_view_json(out, index, root, request->raw) ? SEARCH_QUERY_RESOURCE : 0;
    if (request->raw && !rt_strcmp(operation, "get_paper") && root) {
        SearchCase context;
        if (search_case_open(&context, index, root, error)) return SEARCH_QUERY_RESOURCE;
        status = search_case_json(out, index, &context); search_case_close(&context);
        return status ? SEARCH_QUERY_RESOURCE : 0;
    }
    tool_array_init(&entries, sizeof(Entry));
    if (!rt_strcmp(operation, "get_objects")) {
        if (!request->id_count || request->id_count > 20) goto invalid;
        for (i = 0; i < request->id_count; i++) {
            const SearchRecord *r = search_find(index, request->ids[i], 0);
            if (!r) { *error = "bulk ID is not in this snapshot"; status = SEARCH_READ_NOT_FOUND; goto done; }
            if (entry_add(&entries, index, r->id, (uint32_t)(r-index->records), NONE, NONE)) goto resource;
        }
    } else if (!rt_strcmp(operation, "get_search_options")) {
        const char *kind = request->kind ? request->kind : "Organization";
        if (!search_kind_known(index, kind)) goto invalid;
        for (i = 0; i < index->record_count; i++) if (index->records[i].parent == NONE &&
            !rt_strcmp(index->strings + index->records[i].kind, kind))
            if (entry_append(&entries, index, index->records[i].id, (uint32_t)i, NONE, NONE)) goto resource;
    } else {
        int paper = !rt_strcmp(operation, "get_paper"), meeting = !rt_strcmp(operation, "get_meeting"),
            agenda = !rt_strcmp(operation, "get_agenda");
        if (!root || (paper && rt_strcmp(index->strings + root->kind, "Paper")) ||
            ((meeting || agenda) && rt_strcmp(index->strings + root->kind, "Meeting")) ||
            (!paper && !meeting && !agenda && rt_strcmp(operation, "get_object"))) goto invalid;
        if (entry_add(&entries, index, root->id, (uint32_t)(root-index->records), NONE, NONE)) goto resource;
        if (agenda) {
            for (i = search_relation_start(index, (uint32_t)(root-index->records)); i < index->relation_count &&
                index->relations[i].source == (uint32_t)(root-index->records); i++)
                if (index->relations[i].role == SEARCH_MEETING_AGENDA) {
                    uint32_t target = index->relation_targets[i];
                    if (target != NONE && index->agenda_access[target] == 2) { nonpublic++; continue; }
                    if (target == NONE && index->relation_hidden[i]) { nonpublic++; continue; }
                    if (target == NONE) missing_agenda++;
                    else if (!index->agenda_access[target]) unspecified++;
                    if (entry_add(&entries, index, index->relations[i].target, target, 0, (uint32_t)i)) goto resource;
                }
            if (entries.count > 1) rt_sort((Entry *)entries.data+1, entries.count-1, sizeof(Entry), agenda_compare);
        } else if (paper || meeting) {
            for (i = 0; i < entries.count; i++) {
                if (expand(&entries, index, (uint32_t)i, i == 0, meeting)) goto resource;
                if (!i && paper) paper_order(&entries, index);
            }
        }
    }
    if (!rt_strcmp(operation, "get_object")) {
        status = object_json(out, index, &entries, (const Entry *)entries.data, request, 0);
        goto done;
    }
    if (put(out, "{\"snapshotId\":") || tool_json_append_string(out, index->snapshot_id) ||
        boolean(out, "incomplete", index->incomplete != 0) || uint(out, "total", entries.count-first) ||
        string(out, "countUnit", first ? "agendaItem" : "object") ||
        string(out, "relationshipEncoding", "parent-reference-v1") || uint(out, "offset", request->offset)) goto resource;
    if (request->id && string(out, "id", request->id)) goto resource;
    if (!rt_strcmp(operation, "get_paper") && !request->offset) {
        size_t documents = 0, consultations = 0, related = 0;
        for (i = 1; i < entries.count; i++) {
            const Entry *entry = (const Entry *)tool_array_get_const(&entries, i);
            unsigned int role = index->relations[entry->relation].role;
            if (entry->parent == 0 && (role == SEARCH_MAIN_FILE || role == SEARCH_PAPER_ATTACHMENT)) documents++;
            if (entry->record != NONE) {
                const char *kind = index->strings + index->records[entry->record].kind;
                if (!rt_strcmp(kind, "Consultation")) consultations++;
                if (!rt_strcmp(kind, "Paper")) related++;
            }
        }
        if (put(out, ",\"overview\":{\"directDocuments\":") || tool_byte_buffer_append_uint(out, documents) ||
            uint(out, "consultations", consultations) || uint(out, "relatedPapers", related) ||
            string(out, "consultationOrder", "meeting.start-desc; unknown last") || put(out, "}")) goto resource;
    }
    if (first && (key(out, "meeting") || linked_summary(out, index, (uint32_t)(root-index->records)))) goto resource;
    for (i = first; i < entries.count; i++) {
        const Entry *entry = (const Entry *)tool_array_get_const(&entries, i);
        if (entry->record == NONE) local_missing++;
        else if (index->records[entry->record].warnings) local_warnings++;
    }
    if (put(out, ",\"localCoverage\":{\"unindexedObjects\":") || tool_byte_buffer_append_uint(out, local_missing) ||
        uint(out, "warningObjects", local_warnings) || put(out, "}")) goto resource;
    if (first && (uint(out, "nonPublicReferences", nonpublic) || uint(out, "unresolvedAgendaReferences", missing_agenda) ||
        uint(out, "publicStatusUnspecified", unspecified))) goto resource;
    if (!rt_strcmp(operation, "get_search_options")) {
        if (put(out, ",\"kinds\":[")) goto resource;
        for (i = 0; i < index->kind_count; i++)
            if ((i && put(out, ",")) || tool_json_append_string(out, index->strings + index->kinds[i].name)) goto resource;
        if (put(out, "],\"dateRoles\":[")) goto resource;
        for (i = 0; i < SEARCH_REFERENCE_YEAR; i++)
            if ((i && put(out, ",")) || tool_json_append_string(out, search_date_role_name((unsigned int)i))) goto resource;
        if (put(out, "],\"projectionFields\":[")) goto resource;
        for (i = 0; i < sizeof(metadata_fields)/sizeof(metadata_fields[0]); i++)
            if ((i && put(out, ",")) || tool_json_append_string(out, metadata_fields[i])) goto resource;
        if (put(out, "]") || paper_types_json(out, index) || boolean(out, "catalogIndexedOnly", 1) ||
            string(out, "actorSemantics", "exact raw relationship IDs; no inferred authorship")) goto resource;
    }
    if (put(out, ",\"objects\":[")) goto resource;
    for (i = (size_t)request->offset + first; i < entries.count && returned < limit; i++) {
        ToolByteBuffer object;
        tool_byte_buffer_init(&object);
        status = object_json(&object, index, &entries, (const Entry *)tool_array_get_const(&entries, i), request, 1);
        if (status) { tool_byte_buffer_free(&object); goto resource; }
        if (!request->raw && out->size + object.size + 600 > COMPACT_BYTES) {
            tool_byte_buffer_free(&object);
            if (!returned) { *error = "one projected object exceeds compact budget; select fewer fields or request raw metadata"; goto resource; }
            break;
        }
        if (out->size + object.size > 16U*1024U*1024U) {
            tool_byte_buffer_free(&object); *error = "raw projection exceeds 16 MiB; reduce fields or limit"; goto resource;
        }
        status = (returned && put(out, ",")) || tool_byte_buffer_append(out, object.data, object.size);
        tool_byte_buffer_free(&object);
        if (status) goto resource;
        returned++;
    }
    if (put(out, "]") || uint(out, "returned", returned) ||
        next(out, index, operation, request, (size_t)request->offset + returned, entries.count-first) || put(out, "}\n")) goto resource;
    status = 0; goto done;
invalid:
    status = SEARCH_QUERY_INVALID; goto done;
resource:
    if (!*error || !rt_strcmp(*error, "invalid research parameters, unknown projection field or resource"))
        *error = "cannot build research response within RAM/result budget";
    status = SEARCH_QUERY_RESOURCE;
done:
    tool_array_free(&entries); return status;
}
static int record_date_json(ToolByteBuffer *out, const SearchIndex *index, uint32_t record, SearchDateRole role) {
    SearchHit date = {0};
    date.record = record; date.paper = NONE;
    search_hit_date(index, &date, role);
    return search_hit_date_json(out, index, &date);
}
int search_hit_source_json(ToolByteBuffer *out, const SearchIndex *index, const SearchHit *hit) {
    uint32_t source = index->records[hit->record].parent;
    size_t i, total = 0, returned = 0;
    int document;
    if (source == NONE) source = hit->record;
    document = !rt_strcmp(index->strings + index->records[source].kind, "File");
    if (put(out, "{\"citation\":") || citation(out, index, hit->record, 0)) return -1;
    if (document &&
        (put(out, ",\"documentDates\":{\"created\":") ||
         record_date_json(out, index, source, SEARCH_CREATED) || put(out, ",\"modified\":") ||
         record_date_json(out, index, source, SEARCH_MODIFIED) || put(out, "}"))) return -1;
    if (put(out, ",\"relationshipPath\":[")) return -1;
    {
        for (i = index->incoming_start[source]; i < index->incoming_start[source+1]; i++) {
            const SearchRelation *r = &index->relations[index->incoming_relations[i]];
            if (hit->paper != NONE && r->source != hit->paper) continue;
            total++;
            if (!document || returned >= 4) continue;
            if ((returned && put(out, ",")) || relationship(out, index, index->incoming_relations[i], 0)) return -1;
            returned++;
        }
    }
    if (put(out, "]") || uint(out, "relationshipTotal", total) ||
        boolean(out, "relationshipsComplete", returned == total) ||
        key(out, "relationsAvailable") || put(out, "{\"operation\":\"get_object\",\"id\":") ||
        tool_json_append_string(out, index->strings + index->records[source].id) ||
        string(out, "view", "relations") || string(out, "direction", "incoming") ||
        string(out, "snapshotId", index->snapshot_id) || put(out, "}")) return -1;
    return put(out, "}");
}

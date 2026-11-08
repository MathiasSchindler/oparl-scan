#include "search.h"
#include "pdf-text.h"
#include "tool_json_parser.h"
#include "runtime.h"

#define NONE UINT32_MAX

typedef struct { ToolOutputBuffer stream; ToolByteBuffer *memory; } SearchOutput;

static int out_write(SearchOutput *out, const char *text, size_t length) {
    if (!out->memory) return tool_output_buffer_write(&out->stream, text, length);
    if (out->memory->size > 16U * 1024U * 1024U || length > 16U * 1024U * 1024U - out->memory->size)
        return -1;
    return tool_byte_buffer_append(out->memory, text, length);
}

static int out_text(SearchOutput *out, const char *text) {
    return out_write(out, text, rt_strlen(text));
}

static int out_char(SearchOutput *out, char ch) {
    return out_write(out, &ch, 1);
}

static int out_uint(SearchOutput *out, unsigned long long value) {
    if (!out->memory) return tool_output_buffer_write_uint(&out->stream, value);
    if (out->memory->size > 16U * 1024U * 1024U ||
        tool_count_decimal_digits(value) > 16U * 1024U * 1024U - out->memory->size) return -1;
    return tool_byte_buffer_append_uint(out->memory, value);
}

static int out_line(SearchOutput *out, const char *text) {
    return out_text(out, text) != 0 || out_char(out, '\n') != 0 ? -1 : 0;
}

/* newos' fd-based JSON writer emits each byte separately; buffer the same escaping. */
static int out_string(SearchOutput *out, const char *text) {
    static const char hex[] = "0123456789abcdef";
    const char *p = text;
    if (out_char(out, '"') != 0) return -1;
    while (*p) {
        const char *start = p;
        unsigned char ch;
        char escape[6] = {'\\', 'u', '0', '0', '0', '0'};
        size_t length = 6;
        while (*p && (unsigned char)*p >= 32 && *p != '"' && *p != '\\') p++;
        if (out_write(out, start, (size_t)(p - start)) != 0) return -1;
        if (!*p) break;
        ch = (unsigned char)*p++;
        if (ch == '"' || ch == '\\' || ch == '\n' || ch == '\r' || ch == '\t') {
            escape[1] = ch == '\n' ? 'n' : ch == '\r' ? 'r' : ch == '\t' ? 't' : (char)ch;
            length = 2;
        } else {
            escape[4] = hex[ch >> 4]; escape[5] = hex[ch & 15];
        }
        if (out_write(out, escape, length) != 0) return -1;
    }
    return out_char(out, '"');
}

static int out_key(SearchOutput *out, const char *name) {
    return out_text(out, ",\"") != 0 || out_text(out, name) != 0 || out_text(out, "\":") != 0 ? -1 : 0;
}

static int out_field_string(SearchOutput *out, const char *name, const char *value) {
    return out_key(out, name) != 0 || out_string(out, value) != 0 ? -1 : 0;
}

static int out_field_uint(SearchOutput *out, const char *name, unsigned long long value) {
    return out_key(out, name) != 0 || out_uint(out, value) != 0 ? -1 : 0;
}

static int out_field_bool(SearchOutput *out, const char *name, int value) {
    return out_key(out, name) != 0 || out_text(out, value ? "true" : "false") != 0 ? -1 : 0;
}

static int out_document_url(SearchOutput *out, const SearchIndex *index, const SearchRecord *record) {
    ToolJson json;
    const ToolJsonToken *token;
    size_t value;
    int status = 0;
    int main_document = !rt_strcmp(index->strings + record->kind, "Paper");
    uint32_t file = search_document_record(index, record);
    if (file == NONE) return 0;
    record = &index->records[file];
    if (!record->payload || rt_strcmp(index->strings + record->kind, "File")) return 0;
    if (tool_json_parse(&json, index->strings + record->payload, rt_strlen(index->strings + record->payload)) != 0)
        return search_error("cannot parse indexed File metadata", index->strings + record->id);
    if (main_document)
        status = out_key(out, "mainDocument") || out_text(out, "{\"fileId\":") ||
            out_string(out, index->strings + record->id);
    value = tool_json_get(&json, 0, "downloadUrl");
    token = tool_json_token(&json, value);
    if (!token || token->kind == TOOL_JSON_NULL || (token->kind == TOOL_JSON_STRING && token->end - token->start == 2))
        token = tool_json_token(&json, tool_json_get(&json, 0, "accessUrl"));
    if (!status && token && token->kind != TOOL_JSON_NULL) {
        if (token->kind != TOOL_JSON_STRING) status = search_error("document URL is not a string", index->strings + record->id);
        else if (token->end - token->start > 2)
            status = out_key(out, "documentUrl") != 0 ||
                out_write(out, json.text + token->start, token->end - token->start) != 0 ? -1 : 0;
    }
    token = tool_json_token(&json, tool_json_get(&json, 0, "fileName"));
    if (!status && token && token->kind != TOOL_JSON_NULL) {
        if (token->kind != TOOL_JSON_STRING) status = search_error("fileName is not a string", index->strings + record->id);
        else if (token->end - token->start > 2)
            status = out_key(out, "fileName") != 0 ||
                out_write(out, json.text + token->start, token->end - token->start) != 0 ? -1 : 0;
    }
    if (!status && main_document) {
        uint32_t version = index->version_lookup[file];
        status = out_key(out, "documentVersion");
        if (!status && version != NONE) {
            const SearchVersion *v = &index->versions[version];
            status = out_text(out, "{\"id\":") || out_string(out, index->strings + v->id) ||
                out_field_string(out, "sha256", index->strings + v->hash) ||
                out_field_uint(out, "bytes", v->bytes) || out_text(out, "}");
        } else if (!status) status = out_text(out, "null");
        if (!status) status = out_text(out, "}");
    }
    tool_json_free(&json);
    return status;
}

static int print_results(SearchOutput *out, const SearchIndex *index, const SearchResult *result, int json) {
    unsigned int i;
    if (json && (out_text(out, "{\"total\":") != 0 || out_uint(out, result->total) != 0 ||
        out_field_bool(out, "incomplete", index->incomplete != 0) != 0 ||
        out_field_string(out, "group", result->group == SEARCH_PAPERS ? "paper" :
                         result->group == SEARCH_DOCUMENTS ? "document" : "record") != 0 ||
        out_field_string(out, "countUnit", result->group == SEARCH_PAPERS ? "paper-or-standalone-object" :
                         result->group == SEARCH_DOCUMENTS ? "document-version" : "record") != 0 ||
        out_field_uint(out, "matchedRecords", result->matched_records) != 0 ||
        out_field_uint(out, "excludedUnknownDateRecords", result->excluded_unknown_dates) != 0 ||
        out_field_uint(out, "excludedInvalidDateRecords", result->excluded_invalid_dates) != 0 ||
        out_field_uint(out, "excludedUnversionedRecords", result->excluded_unversioned) != 0 ||
        out_field_bool(out, "filterOnly", result->filter_only) != 0 ||
        (result->mode && out_field_string(out, "mode", result->mode == SEARCH_SEMANTIC ? "semantic" : "hybrid")) ||
        out_field_string(out, "dateRole", search_date_role_name(result->date_role)) != 0 ||
        out_field_string(out, "dateFrom", result->date_from ? result->date_from : "") != 0 ||
        out_field_string(out, "dateThrough", result->date_through ? result->date_through : "") != 0 ||
        out_field_string(out, "sort", result->sort == SEARCH_DATE_ASC ? "date-asc" :
                         result->sort == SEARCH_DATE_DESC ? "date-desc" : "relevance") != 0 ||
        out_field_string(out, "snapshotId", index->snapshot_id) != 0 ||
        out_text(out, ",\"hits\":[") != 0)) return -1;
    for (i = 0; i < result->count; i++) {
        const SearchHit *hit = &result->hits[i];
        const SearchRecord *r = &index->records[hit->record];
        size_t start = hit->byte > 100 ? hit->byte - 100 : 0;
        size_t end = hit->byte + 220;
        char snippet[512];
        if (end > r->text_length) end = r->text_length;
        while (start < end && ((unsigned char)index->strings[r->text + start] & 0xc0) == 0x80) start++;
        while (end > start && ((unsigned char)index->strings[r->text + end] & 0xc0) == 0x80) end--;
        memcpy(snippet, index->strings + r->text + start, end - start); snippet[end-start] = '\0';
        {
            size_t j, used = 0;
            int space = 0;
            for (j = 0; j < end - start; j++) {
                unsigned char ch = (unsigned char)snippet[j];
                if (ch <= 32) { space = used != 0; continue; }
                if (space) snippet[used++] = ' ';
                snippet[used++] = (char)ch; space = 0;
            }
            snippet[used] = '\0';
        }
        if (json) {
            uint32_t source = r->parent == NONE ? hit->record : r->parent;
            uint32_t group_identity = result->group == SEARCH_DOCUMENTS ?
                index->versions[index->version_lookup[source]].id :
                result->group == SEARCH_PAPERS ? index->records[hit->paper == NONE ? source : hit->paper].identity :
                r->identity;
            if ((i && out_text(out, ",") != 0) ||
                out_text(out, "{\"id\":") != 0 || out_string(out, index->strings + r->id) != 0 ||
                out_field_string(out, "kind", index->strings + r->kind) != 0 ||
                out_field_string(out, "name", index->strings + r->name) != 0 ||
                out_field_string(out, "reference", index->strings + r->reference) != 0 ||
                out_field_string(out, "date", search_hit_date_text(index, hit)) != 0 ||
                out_field_string(out, "rawDate", index->strings + r->date) != 0 ||
                out_field_string(out, "dateRole", search_date_role_name(hit->date_role)) != 0 ||
                out_field_string(out, "identity", index->strings + r->identity) != 0 ||
                out_field_string(out, "groupIdentity", index->strings + group_identity) != 0 ||
                out_field_string(out, "unit", result->group == SEARCH_DOCUMENTS ? "document-version" :
                    result->group == SEARCH_PAPERS ? (hit->paper == NONE ? "standalone-object" : "paper") :
                    !rt_strcmp(index->strings + r->kind, "Page") ? "page" :
                    !rt_strcmp(index->strings + r->kind, "Consultation") ? "consultation" : "object") != 0 ||
                out_field_string(out, "paperType", index->strings + r->paper_type) != 0 ||
                out_field_string(out, "organization", index->strings + r->organization) != 0 ||
                out_field_string(out, "source", index->strings + r->source) != 0 ||
                out_field_uint(out, "page", r->page) != 0 ||
                out_field_uint(out, "warnings", r->warnings) != 0 ||
                out_field_uint(out, "matchCount", hit->matches) != 0 ||
                out_field_string(out, "snippet", snippet) != 0 ||
                out_document_url(out, index, r) != 0) return -1;
            if (result->mode && (out_field_string(out, "semanticText", hit->semantic_text ? hit->semantic_text : "") ||
                out_field_bool(out, "semanticMatch", hit->semantic_score > -2))) return -1;
            if (result->mode) {
                ToolByteBuffer semantic = {0};
                int status = search_embedding_source_json(&semantic, index, hit);
                if (!status) status = out_text(out, ",\"semantic\":") ||
                    out_write(out, (const char *)semantic.data, semantic.size);
                tool_byte_buffer_free(&semantic);
                if (status) return -1;
            }
            {
                ToolByteBuffer date;
                int status;
                tool_byte_buffer_init(&date);
                status = search_hit_date_json(&date, index, hit);
                if (!status) status = out_text(out, ",\"sortDate\":") ||
                    out_write(out, (const char *)date.data, date.size) || out_text(out, ",\"filterDate\":") ||
                    (result->date_from || result->date_through ?
                        out_write(out, (const char *)date.data, date.size) : out_text(out, "null"));
                tool_byte_buffer_free(&date);
                if (status) return -1;
            }
            {
                ToolByteBuffer proof;
                int status;
                tool_byte_buffer_init(&proof);
                status = search_hit_source_json(&proof, index, hit);
                if (!status) status = out_text(out, ",\"evidence\":") ||
                    out_write(out, (const char *)proof.data, proof.size);
                tool_byte_buffer_free(&proof);
                if (status) return -1;
            }
            {
                ToolByteBuffer preparation;
                int status;
                tool_byte_buffer_init(&preparation);
                status = search_preparation_json(&preparation, index, r,
                                                hit->semantic_text ? NONE : hit->byte, 0);
                if (!status) status = out_text(out, ",\"preparation\":") ||
                    out_write(out, (const char *)preparation.data, preparation.size);
                tool_byte_buffer_free(&preparation);
                if (status) return -1;
            }
            if (hit->paper != NONE && out_field_string(out, "relatedPaper",
                index->strings + index->records[hit->paper].id) != 0) return -1;
            if (hit->paper != NONE && out_field_string(out, "relatedName",
                index->strings + index->records[hit->paper].name) != 0) return -1;
            if (hit->paper != NONE && out_field_string(out, "relatedReference",
                index->strings + index->records[hit->paper].reference) != 0) return -1;
            if (hit->paper != NONE && out_field_string(out, "relatedDate",
                index->strings + index->records[hit->paper].date) != 0) return -1;
            if (hit->paper != NONE && out_field_string(out, "relatedPaperType",
                index->strings + index->records[hit->paper].paper_type) != 0) return -1;
            if (hit->paper != NONE && out_field_string(out, "relatedOrganization",
                index->strings + index->records[hit->paper].organization) != 0) return -1;
            if (out_text(out, "}") != 0) return -1;
        } else {
            if (out_text(out, index->strings + r->reference) != 0 || out_text(out, "\t") != 0 ||
                out_line(out, index->strings + r->name) != 0 ||
                out_line(out, index->strings + r->id) != 0 ||
                out_text(out, index->strings + r->source) != 0 ||
                out_text(out, " page=") != 0 || out_uint(out, r->page) != 0 ||
                out_text(out, "\nsortDate=") != 0 || out_text(out, search_hit_date_text(index, hit)) != 0 ||
                out_text(out, " role=") != 0 || out_text(out, search_date_role_name(hit->date_role)) != 0 ||
                out_text(out, " source=") != 0 ||
                out_text(out, hit->date_role == SEARCH_IMPORTED ? index->snapshot_id :
                    index->strings + index->records[hit->date_source].id) != 0 ||
                out_text(out, "\n") != 0 || out_line(out, snippet) != 0) return -1;
        }
    }
    if (!json && (out_text(out, "Total=") || out_uint(out, result->total) ||
        out_text(out, " unit=") || out_text(out, result->group == SEARCH_PAPERS ? "paper-or-standalone-object" :
        result->group == SEARCH_DOCUMENTS ? "document-version" : "record") ||
        out_text(out, " matchedRecords=") || out_uint(out, result->matched_records) ||
        out_text(out, " excludedUnknownDateRecords=") || out_uint(out, result->excluded_unknown_dates) ||
        out_text(out, " excludedInvalidDateRecords=") || out_uint(out, result->excluded_invalid_dates) ||
        out_text(out, " excludedUnversionedRecords=") || out_uint(out, result->excluded_unversioned) ||
        out_text(out, "\n"))) return -1;
    if (json) {
        if (out_text(out, "],\"next\":")) return -1;
        if ((size_t)result->offset + result->count < result->total) {
            if (out_text(out, "{\"snapshotId\":") || out_string(out, index->snapshot_id) ||
                out_field_uint(out, "offset", (size_t)result->offset + result->count) ||
                out_field_uint(out, "limit", result->limit) || out_text(out, "}")) return -1;
        } else if (out_text(out, "null")) return -1;
        return out_text(out, "}\n");
    }
    return 0;
}

static int print_record(SearchOutput *out, const SearchIndex *index, const SearchRecord *r, int json, int raw) {
    const char *body = index->strings + r->text + r->body_start;
    ToolByteBuffer model, preparation;
    int status;
    if (!json) return out_line(out, r->page ? body : index->strings + r->payload);
    tool_byte_buffer_init(&model);
    tool_byte_buffer_init(&preparation);
    status = search_model_json(&model, index, r);
    if (!status) status = search_preparation_json(&preparation, index, r, 0, raw ? 2 : 1);
    if (status != 0) { tool_byte_buffer_free(&model); tool_byte_buffer_free(&preparation); return -1; }
    if (out_text(out, "{\"id\":") != 0 || out_string(out, index->strings + r->id) != 0 ||
        out_field_uint(out, "page", r->page) != 0 ||
        out_field_uint(out, "warnings", r->warnings) != 0 ||
        out_field_bool(out, "incomplete", index->incomplete != 0) != 0 ||
        out_field_string(out, "source", index->strings + r->source) != 0 ||
        out_field_string(out, "text", body) != 0 ||
        out_text(out, ",\"metadata\":") != 0 ||
        out_text(out, r->payload ? index->strings + r->payload : "null") != 0 ||
        out_key(out, "model") != 0 || out_write(out, (const char *)model.data, model.size) != 0 ||
        out_key(out, "preparation") ||
        out_write(out, (const char *)preparation.data, preparation.size) ||
        out_text(out, "}\n") != 0) status = -1;
    tool_byte_buffer_free(&model);
    tool_byte_buffer_free(&preparation);
    return status;
}

int search_print(int fd, const SearchIndex *index, const SearchResult *result, int json) {
    SearchOutput out;
    out.memory = 0; tool_output_buffer_init(&out.stream, fd);
    return print_results(&out, index, result, json) != 0 || tool_output_buffer_flush(&out.stream) != 0 ? -1 : 0;
}

int search_show(int fd, const SearchIndex *index, const SearchRecord *record, int json) {
    SearchOutput out;
    out.memory = 0; tool_output_buffer_init(&out.stream, fd);
    return print_record(&out, index, record, json, 1) != 0 || tool_output_buffer_flush(&out.stream) != 0 ? -1 : 0;
}

int search_results_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchResult *result) {
    SearchOutput out;
    out.memory = buffer;
    return print_results(&out, index, result, 1);
}

int search_record_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchRecord *record) {
    return search_record_view_json(buffer, index, record, 1);
}
int search_record_view_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchRecord *record, int raw) {
    SearchOutput out;
    out.memory = buffer;
    return print_record(&out, index, record, 1, raw);
}

int search_status_json(ToolByteBuffer *buffer, const SearchIndex *index) {
    SearchOutput out;
    size_t i, pages = 0, warnings = 0, ocr = 0;
    out.memory = buffer;
    for (i = 0; i < index->record_count; i++) if (index->records[i].page) {
        pages++;
        if (index->records[i].warnings) warnings++;
        if (index->records[i].warnings & PDF_TEXT_NEEDS_OCR) ocr++;
    }
    return out_text(&out, "{\"records\":") != 0 || out_uint(&out, index->record_count) != 0 ||
        out_field_uint(&out, "terms", index->term_count) != 0 ||
        out_field_uint(&out, "formatVersion", index->format_version) != 0 ||
        out_field_string(&out, "snapshotId", index->snapshot_id) != 0 ||
        out_field_uint(&out, "importedAtEpochSeconds", index->imported_at) != 0 ||
        out_field_uint(&out, "relations", index->relation_count) != 0 ||
        out_field_uint(&out, "documentVersions", index->version_count) != 0 ||
        out_field_uint(&out, "textSections", index->section_count) != 0 ||
        out_field_uint(&out, "indexBytes", index->file_bytes) != 0 ||
        out_field_uint(&out, "indexMemoryBytes", index->memory_bytes) != 0 ||
        out_field_uint(&out, "pages", pages) != 0 || out_field_uint(&out, "warningPages", warnings) != 0 ||
        out_field_uint(&out, "ocrPages", ocr) != 0 || out_field_bool(&out, "incomplete", index->incomplete != 0) != 0 ||
        out_text(&out, ",\"coverage\":") != 0 || search_coverage_json(buffer, index) != 0 ||
        (index->embedding && (out_text(&out, ",\"semantic\":") ||
                             search_embedding_status_json(buffer, index))) ||
        out_text(&out, "}\n") != 0 ? -1 : 0;
}

typedef struct {
    char *id;
    const char *kind;
    const SearchRecord *record;
    ToolJson json;
} CaseObject;

typedef struct { const char *key, *kind; } CaseRelation;

void search_case_close(SearchCase *context) {
    size_t i;
    for (i = 0; i < context->objects.count; i++) {
        CaseObject *object = (CaseObject *)tool_array_get(&context->objects, i);
        if (object->json.text) tool_json_free(&object->json);
        rt_free(object->id);
    }
    tool_array_free(&context->objects);
}

static int case_metadata(CaseObject *object, const char *text, size_t length, const char **error) {
    if (tool_json_parse(&object->json, text, length) != 0) {
        object->json.text = 0;
        *error = "cannot parse case metadata";
        return -1;
    }
    if (tool_json_token(&object->json, 0)->kind != TOOL_JSON_OBJECT) {
        *error = "case metadata is not an object"; return -1;
    }
    return 0;
}

static int case_add(SearchCase *context, const SearchIndex *index, char *id, const char *kind,
                    const ToolJson *embedded, size_t value, const char **error) {
    CaseObject *object;
    size_t i;
    const ToolJsonToken *token = embedded ? tool_json_token(embedded, value) : 0;
    for (i = 0; i < context->objects.count; i++) {
        object = (CaseObject *)tool_array_get(&context->objects, i);
        if (!rt_strcmp(object->id, id)) {
            rt_free(id);
            if (!object->json.text && token && token->kind == TOOL_JSON_OBJECT)
                return case_metadata(object, embedded->text + token->start, token->end - token->start, error);
            return 0;
        }
    }
    if (context->objects.count >= 4096) {
        rt_free(id); *error = "case exceeds the 4096-object response limit"; return -1;
    }
    object = (CaseObject *)tool_array_append(&context->objects);
    if (!object) { rt_free(id); *error = "cannot allocate case relationships"; return -1; }
    rt_memset(object, 0, sizeof(*object));
    object->id = id; object->kind = kind;
    object->record = search_find(index, id, 0);
    if (object->record) {
        object->kind = index->strings + object->record->kind;
        if (object->record->payload) {
            const char *payload = index->strings + object->record->payload;
            return case_metadata(object, payload, rt_strlen(payload), error);
        }
    } else context->incomplete = 1;
    if (token && token->kind == TOOL_JSON_OBJECT)
        return case_metadata(object, embedded->text + token->start, token->end - token->start, error);
    return 0;
}

static int case_value(SearchCase *context, const SearchIndex *index, const ToolJson *json, size_t value,
                      const char *kind, const char **error) {
    const ToolJsonToken *token = tool_json_token(json, value), *identity;
    char *id;
    size_t child;
    if (!token || token->kind == TOOL_JSON_NULL) return 0;
    if (token->kind == TOOL_JSON_ARRAY) {
        for (child = value + 1; child < token->next; child = tool_json_token(json, child)->next)
            if (case_value(context, index, json, child, kind, error) != 0) return -1;
        return 0;
    }
    child = token->kind == TOOL_JSON_OBJECT ? tool_json_get(json, value, "id") : value;
    identity = tool_json_token(json, child);
    if (!identity || identity->kind != TOOL_JSON_STRING) {
        *error = "case relationship has no string ID"; return -1;
    }
    id = (char *)rt_malloc(identity->end - identity->start + 1);
    if (!id) { *error = "cannot allocate case identity"; return -1; }
    if (tool_json_string(json, child, id, identity->end - identity->start + 1) != 0 || !*id) {
        rt_free(id); *error = "invalid case relationship ID"; return -1;
    }
    return case_add(context, index, id, kind, json, value, error);
}

int search_case_open(SearchCase *context, const SearchIndex *index, const SearchRecord *paper, const char **error) {
    static const CaseRelation paper_links[] = {
        {"mainFile","File"},{"auxiliaryFile","File"},{"superordinatedPaper","Paper"},
        {"subordinatedPaper","Paper"},{"relatedPaper","Paper"},{"consultation","Consultation"},
        {"underDirectionOf","Organization"}
    };
    static const CaseRelation consultation_links[] = {
        {"agendaItem","AgendaItem"},{"agendaitem","AgendaItem"},{"meeting","Meeting"},{"organization","Organization"}
    };
    static const CaseRelation agenda_links[] = {
        {"meeting","Meeting"},{"resolutionFile","File"},{"auxiliaryFile","File"}
    };
    static const CaseRelation meeting_links[] = {
        {"organization","Organization"},{"invitation","File"},{"resultsProtocol","File"},
        {"verbatimProtocol","File"},{"auxiliaryFile","File"}
    };
    char *id;
    size_t i, j;
    tool_array_init(&context->objects, sizeof(CaseObject));
    context->incomplete = index->incomplete != 0;
    *error = 0;
    id = (char *)rt_malloc(rt_strlen(index->strings + paper->id) + 1);
    if (!id) { *error = "cannot allocate case identity"; return -1; }
    memcpy(id, index->strings + paper->id, rt_strlen(index->strings + paper->id) + 1);
    if (case_add(context, index, id, "Paper", 0, 0, error) != 0) goto fail;
    for (i = 0; i < context->objects.count; i++) {
        CaseObject object = *(const CaseObject *)tool_array_get_const(&context->objects, i);
        const CaseRelation *links = 0;
        size_t count = 0;
        if (!object.json.text || tool_json_true(&object.json, tool_json_get(&object.json, 0, "deleted"))) continue;
        if (i == 0) { links = paper_links; count = sizeof(paper_links)/sizeof(paper_links[0]); }
        else if (!rt_strcmp(object.kind, "Consultation")) {
            links = consultation_links; count = sizeof(consultation_links)/sizeof(consultation_links[0]);
        } else if (!rt_strcmp(object.kind, "AgendaItem")) {
            links = agenda_links; count = sizeof(agenda_links)/sizeof(agenda_links[0]);
        } else if (!rt_strcmp(object.kind, "Meeting")) {
            links = meeting_links; count = sizeof(meeting_links)/sizeof(meeting_links[0]);
        }
        for (j = 0; j < count; j++)
            if (case_value(context, index, &object.json, tool_json_get(&object.json, 0, links[j].key),
                           links[j].kind, error) != 0) goto fail;
    }
    return 0;
fail:
    search_case_close(context);
    return -1;
}

static int print_case(SearchOutput *out, const SearchIndex *index, const SearchCase *context) {
    size_t i;
    const CaseObject *root = (const CaseObject *)tool_array_get_const(&context->objects, 0);
    if (out_text(out, "{\"id\":") != 0 || out_string(out, root->id) != 0 ||
        out_field_bool(out, "incomplete", context->incomplete) != 0 || out_text(out, ",\"objects\":[") != 0) return -1;
    for (i = 0; i < context->objects.count; i++) {
        const CaseObject *object = (const CaseObject *)tool_array_get_const(&context->objects, i);
        const SearchRecord *record = object->record;
        const char *scope = i == 0 ? "paper" :
            !rt_strcmp(object->kind, "Paper") ? "relatedPaper" :
            !rt_strcmp(object->kind, "File") ?
                (search_is_direct_source(index, (uint32_t)(root->record - index->records), object->id) ?
                    "directSource" : "contextSource") : "context";
        if ((i && out_char(out, ',') != 0) || out_text(out, "{\"id\":") != 0 ||
            out_string(out, object->id) != 0 ||
            out_field_string(out, "kind", object->kind) != 0 ||
            out_field_string(out, "scope", scope) != 0 ||
            out_field_bool(out, "indexed", record != 0) != 0 ||
            out_field_string(out, "source", record ? index->strings + record->source : "") != 0 ||
            out_field_uint(out, "warnings", record ? record->warnings : 0) != 0 ||
            out_key(out, "metadata") != 0) return -1;
        if (object->json.text) {
            if (out_write(out, object->json.text, object->json.length) != 0) return -1;
        } else if (out_text(out, "null") != 0) return -1;
        if (out_char(out, '}') != 0) return -1;
    }
    return out_text(out, "]}\n");
}

int search_case_print(int fd, const SearchIndex *index, const SearchCase *context) {
    SearchOutput out;
    out.memory = 0; tool_output_buffer_init(&out.stream, fd);
    return print_case(&out, index, context) != 0 || tool_output_buffer_flush(&out.stream) != 0 ? -1 : 0;
}

int search_case_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchCase *context) {
    SearchOutput out;
    out.memory = buffer;
    return print_case(&out, index, context);
}

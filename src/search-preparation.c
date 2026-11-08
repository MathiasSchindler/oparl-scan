#include "pdf-structure.h"
#include "runtime.h"
#include "search.h"

#define NONE UINT32_MAX

static int put(ToolByteBuffer *out, const char *text) {
    return tool_byte_buffer_append_cstr(out, text);
}
static int raw(ToolByteBuffer *out, const ToolJson *json, size_t value) {
    const ToolJsonToken *t = tool_json_token(json, value);
    return t ? tool_byte_buffer_append(out, json->text + t->start, t->end - t->start)
             : put(out, "null");
}
static int property(ToolByteBuffer *out, const ToolJson *json, size_t object, const char *name) {
    return put(out, ",") || tool_json_append_string(out, name) || put(out, ":") ||
           raw(out, json, tool_json_get(json, object, name));
}
uint32_t search_document_record(const SearchIndex *index, const SearchRecord *record) {
    uint32_t source = (uint32_t)(record - index->records), found = NONE;
    size_t i;
    if (record->parent != NONE)
        return record->parent;
    if (!rt_strcmp(index->strings + record->kind, "File"))
        return source;
    if (rt_strcmp(index->strings + record->kind, "Paper"))
        return NONE;
    for (i = search_relation_start(index, source); i < index->relation_count && index->relations[i].source == source; i++) {
        uint32_t target;
        if (index->relations[i].role != SEARCH_MAIN_FILE)
            continue;
        target = index->relation_targets[i];
        if (target == NONE || (found != NONE && target != found))
            return NONE;
        found = target;
    }
    return found;
}
int search_preparation_validate(const SearchIndex *index, const SearchRecord *record) {
    const SearchRecord *parent = &index->records[record->parent];
    const char *packet = index->strings + record->preparation;
    const char *summary = index->strings + parent->preparation;
    ToolJson page, document;
    unsigned long long format, number, counts[5] = {0, 0, 0, 0, 0};
    char source[65], expected[65], *layout_text = 0;
    size_t layout, spans, i, length;
    const ToolJsonToken *t;
    int status = -1;
    if (!record->preparation || !parent->preparation ||
        rt_strcmp(index->strings + record->kind, "Page") ||
        tool_json_parse(&page, packet, rt_strlen(packet)))
        return -1;
    if (tool_json_parse(&document, summary, rt_strlen(summary))) {
        tool_json_free(&page);
        return -1;
    }
    if (tool_json_uint(&page, tool_json_get(&page, 0, "format"), &format) || format != 1 ||
        tool_json_uint(&page, tool_json_get(&page, 0, "page"), &number) || number != record->page ||
        tool_json_string(&page, tool_json_get(&page, 0, "sourceSha256"), source, sizeof(source)) ||
        tool_json_string(&document, tool_json_get(&document, 0, "sourceSha256"), expected,
                    sizeof(expected)) ||
        rt_strcmp(source, expected) || pdf_structure_validate_page(&page, 0, counts))
        goto done;
    layout = tool_json_get(&page, 0, "layout");
    if (layout != TOOL_JSON_NONE) {
        size_t text = tool_json_get(&page, layout, "text");
        spans = tool_json_get(&page, layout, "spans");
        t = tool_json_token(&page, text);
        if (!t || t->kind != TOOL_JSON_STRING || !tool_json_token(&page, spans) ||
            tool_json_token(&page, spans)->kind != TOOL_JSON_ARRAY)
            goto done;
        layout_text = (char *)rt_malloc(t->end - t->start + 1);
        if (!layout_text || tool_json_string(&page, text, layout_text, t->end - t->start + 1))
            goto done;
        length = rt_strlen(layout_text);
        for (i = spans + 1; i < tool_json_token(&page, spans)->next; i = tool_json_token(&page, i)->next) {
            unsigned long long start, size;
            if (tool_json_uint(&page, tool_json_get(&page, i, "start"), &start) ||
                tool_json_uint(&page, tool_json_get(&page, i, "length"), &size) || start > length ||
                size > length - start)
                goto done;
        }
    }
    status = 0;
done:
    rt_free(layout_text);
    tool_json_free(&document);
    tool_json_free(&page);
    return status;
}
static int available(ToolByteBuffer *out, const SearchIndex *index, uint32_t file,
                     unsigned int page, unsigned long long page_count) {
    const SearchRecord *record = &index->records[file];
    uint32_t version = index->version_lookup[file];
    char number[32];
    rt_unsigned_to_string(page ? page : 1, number, sizeof(number));
    return put(out, ",\"available\":{\"id\":") ||
           tool_json_append_string(out, index->strings + (version == NONE ? record->identity
                                                             : index->versions[version].id)) ||
           ((page || page_count) && (put(out, ",\"page\":") || put(out, number))) ||
           put(out, ",\"snapshotId\":") || tool_json_append_string(out, index->snapshot_id) || put(out, "}");
}
static int contains_position(const ToolJson *json, size_t cell, size_t byte) {
    size_t list = tool_json_get(json, cell, "sources"), i;
    const ToolJsonToken *t = tool_json_token(json, list);
    if (!t)
        return 0;
    for (i = list + 1; i < t->next; i = tool_json_token(json, i)->next) {
        unsigned long long start, length;
        if (!tool_json_uint(json, tool_json_get(json, i, "start"), &start) &&
            !tool_json_uint(json, tool_json_get(json, i, "length"), &length) && byte >= start &&
            byte - start < length)
            return 1;
    }
    return 0;
}
static int layout_position(const ToolJson *json, const SearchIndex *index, const SearchRecord *record,
                           uint32_t byte, size_t *result) {
    size_t text = tool_json_get(json, tool_json_get(json, 0, "layout"), "text"), p = 0, q = 0, raw_length;
    const ToolJsonToken *t = tool_json_token(json, text);
    const char *body = index->strings + record->text + record->body_start;
    size_t length = record->text_length - record->body_start;
    char *layout;
    int status = 0;
    *result = TOOL_JSON_NONE;
    if (byte < record->body_start || byte >= record->text_length || !t)
        return 0;
    layout = (char *)rt_malloc(t->end - t->start + 1);
    if (!layout)
        return search_error("cannot allocate PDF layout match mapping", 0);
    if (tool_json_string(json, text, layout, t->end - t->start + 1)) {
        status = -1;
        goto done;
    }
    raw_length = rt_strlen(layout);
    while (q < length && layout[p]) {
        unsigned int a, b;
        size_t start = p, next = q;
        if (rt_utf8_decode(layout, raw_length, &p, &a) || rt_utf8_decode(body, length, &next, &b))
            break;
        if (a == b) {
            if (q == byte - record->body_start) {
                *result = start;
                break;
            }
            q = next;
        } else if (a != '-' && a != 0xad && (a > 127 || !rt_is_space((char)a)))
            break;
    }
done:
    rt_free(layout);
    return status;
}
static int context_json(ToolByteBuffer *out, const ToolJson *page, size_t byte, int *matched) {
    size_t structure = tool_json_get(page, 0, "structure"),
           list = tool_json_get(page, structure, "agendaItems");
    size_t i, candidate = TOOL_JSON_NONE, protocols = 0;
    const ToolJsonToken *t = tool_json_token(page, list);
    if (!t)
        return 0;
    for (i = list + 1; i < t->next; i = tool_json_token(page, i)->next) {
        static const char *const cells[] = {"heading", "body", "outcome"};
        size_t k, references;
        int found = 0;
        if (!tool_json_equals(page, tool_json_get(page, i, "kind"), "protocol-top"))
            continue;
        candidate = i;
        protocols++;
        for (k = 0; k < sizeof(cells) / sizeof(cells[0]); k++)
            found |= contains_position(page, tool_json_get(page, i, cells[k]), byte);
        references = tool_json_get(page, i, "references");
        for (k = references + 1; k < tool_json_token(page, references)->next;
             k = tool_json_token(page, k)->next)
            found |= contains_position(page, k, byte);
        if (found) {
            *matched = 1;
            break;
        }
    }
    if (*matched || protocols == 1) {
        static const char *const names[] = {"kind",    "status",     "number",
                                            "heading", "references", "outcome"};
        size_t k;
        if (put(out, "{\"bodyAvailable\":true"))
            return -1;
        for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
            if (property(out, page, candidate, names[k]))
                return -1;
        return put(out, "}");
    }
    list = tool_json_get(page, structure, "tables");
    for (i = list + 1; i < tool_json_token(page, list)->next; i = tool_json_token(page, i)->next) {
        size_t rows = tool_json_get(page, i, "rows"), row;
        for (row = rows + 1; row < tool_json_token(page, rows)->next;
             row = tool_json_token(page, row)->next) {
            size_t cells = tool_json_get(page, row, "cells"), cell;
            int found = 0;
            for (cell = cells + 1; cell < tool_json_token(page, cells)->next;
                 cell = tool_json_token(page, cell)->next)
                found |= contains_position(page, cell, byte);
            if (!found)
                continue;
            *matched = 1;
            return put(out, "{\"kind\":") || raw(out, page, tool_json_get(page, i, "kind")) ||
                   property(out, page, i, "columns") || property(out, page, row, "status") ||
                   property(out, page, row, "cells") || put(out, "}");
        }
    }
    return 0;
}
int search_preparation_json(ToolByteBuffer *out, const SearchIndex *index,
                            const SearchRecord *record, uint32_t byte, int detail) {
    uint32_t file = detail && record->parent == NONE && !record->preparation
                        ? NONE
                        : search_document_record(index, record);
    const SearchRecord *document;
    const char *summary, *packet;
    ToolJson json, page;
    int status = -1, has_page = 0;
    unsigned long long page_count;
    if (file == NONE || !index->records[file].preparation)
        return put(out, "null");
    document = &index->records[file];
    summary = index->strings + document->preparation;
    if (tool_json_parse(&json, summary, rt_strlen(summary)))
        return search_error("invalid PDF preparation", 0);
    if (tool_json_uint(&json, tool_json_get(&json, 0, "pageCount"), &page_count)) {
        tool_json_free(&json);
        return search_error("invalid PDF preparation page count", 0);
    }
    packet = index->strings + record->preparation;
    if (record->parent != NONE && record->preparation != document->preparation) {
        if (tool_json_parse(&page, packet, rt_strlen(packet))) {
            tool_json_free(&json);
            return search_error("invalid PDF page preparation", 0);
        }
        has_page = 1;
    }
    if (detail) {
        const ToolJsonToken *root = tool_json_token(&json, 0);
        if (tool_byte_buffer_append(out, summary + root->start, root->end - root->start - 1) ||
            available(out, index, file, record->page, page_count))
            goto done;
        if (has_page) {
            size_t layout = tool_json_get(&page, 0, "layout");
            if (put(out, ",\"pageData\":"))
                goto done;
            if (detail == 2) {
                if (put(out, packet))
                    goto done;
            } else {
                if (put(out, "{\"format\":1") || property(out, &page, 0, "page") ||
                    property(out, &page, 0, "sourceSha256") || property(out, &page, 0, "structure"))
                    goto done;
                if (layout != TOOL_JSON_NONE &&
                    (put(out, ",\"layoutSpansAvailable\":true,\"layout\":{\"space\":") ||
                     raw(out, &page, tool_json_get(&page, layout, "space")) ||
                     property(out, &page, layout, "text") || put(out, "}")))
                    goto done;
                if (put(out, "}"))
                    goto done;
            }
        }
        status = put(out, "}");
    } else {
        size_t fields = tool_json_get(&json, 0, "fields"), i, emitted = 0, issues = 0;
        ToolByteBuffer context;
        int matched = 0;
        if (put(out, "{\"format\":1,\"fileId\":") ||
            tool_json_append_string(out, index->strings + document->id) ||
            property(out, &json, 0, "sourceSha256") || property(out, &json, 0, "status") ||
            property(out, &json, 0, "structure") || put(out, ",\"fields\":["))
            goto done;
        for (i = fields + 1; i < tool_json_token(&json, fields)->next; i = tool_json_token(&json, i)->next) {
            if (!tool_json_equals(&json, tool_json_get(&json, i, "status"), "rule-supported")) {
                issues++;
                continue;
            }
            if ((emitted++ && put(out, ",")) || put(out, "{\"field\":") ||
                raw(out, &json, tool_json_get(&json, i, "field")) || property(out, &json, i, "value") ||
                property(out, &json, i, "status") || property(out, &json, i, "page") ||
                put(out, "}"))
                goto done;
        }
        {
            char count[32];
            rt_unsigned_to_string(issues, count, sizeof(count));
            if (put(out, "],\"fieldIssues\":") || put(out, count) ||
                available(out, index, file, record->page, page_count))
                goto done;
        }
        tool_byte_buffer_init(&context);
        {
            size_t position = TOOL_JSON_NONE;
            status = has_page ? layout_position(&page, index, record, byte, &position) : 0;
            if (!status && has_page)
                status = context_json(&context, &page, position, &matched);
        }
        if (!status && has_page)
            status = put(out, ",\"pageStatus\":") ||
                     raw(out, &page, tool_json_get(&page, tool_json_get(&page, 0, "structure"), "status"));
        if (!status)
            status =
                put(out, ",\"selection\":") ||
                tool_json_append_string(out, context.size ? matched ? "matching-text" : "page" : "document") ||
                put(out, ",\"context\":") ||
                (context.size && context.size <= 1600
                     ? tool_byte_buffer_append(out, context.data, context.size)
                     : put(out, "null")) ||
                put(out, ",\"contextOmitted\":") ||
                put(out, context.size > 1600 ? "true" : "false") || put(out, "}");
        tool_byte_buffer_free(&context);
    }
done:
    if (has_page)
        tool_json_free(&page);
    tool_json_free(&json);
    return status;
}

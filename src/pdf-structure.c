#include "pdf-structure.h"
#include "runtime.h"

static int array(const ToolJson *j, size_t token) {
    const ToolJsonToken *t = tool_json_token(j, token);
    return t && t->kind == TOOL_JSON_ARRAY;
}
static int cell_shape(const ToolJson *j, size_t cell, size_t length) {
    size_t sources = tool_json_get(j, cell, "sources"), text = tool_json_get(j, cell, "text"), i;
    const ToolJsonToken *t = tool_json_token(j, text);
    if (!t || t->kind != TOOL_JSON_STRING || !array(j, sources))
        return -1;
    if (tool_json_equals(j, text, "") != (tool_json_token(j, sources)->next == sources + 1))
        return -1;
    for (i = sources + 1; i < tool_json_token(j, sources)->next; i = tool_json_token(j, i)->next) {
        unsigned long long start, size;
        if (tool_json_uint(j, tool_json_get(j, i, "start"), &start) ||
            tool_json_uint(j, tool_json_get(j, i, "length"), &size) || !size || start > length ||
            size > length - start)
            return -1;
    }
    return 0;
}
int pdf_structure_validate_page(const ToolJson *j, size_t page, unsigned long long counts[5]) {
    size_t structure = tool_json_get(j, page, "structure"), tables = tool_json_get(j, structure, "tables");
    size_t items = tool_json_get(j, structure, "agendaItems"),
           diagnostics = tool_json_get(j, structure, "diagnostics");
    size_t i, k, cell, table_count = 0, item_count = 0, length = 0;
    unsigned long long format;
    char *layout = 0;
    int status = -1;
    if (tool_json_uint(j, tool_json_get(j, structure, "format"), &format) || format != 1 ||
        !array(j, tables) || !array(j, items) || !array(j, diagnostics))
        return -1;
    if (!tool_json_equals(j, tool_json_get(j, structure, "status"), "rule-supported") &&
        !tool_json_equals(j, tool_json_get(j, structure, "status"), "unrecognized") &&
        !tool_json_equals(j, tool_json_get(j, structure, "status"), "ambiguous") &&
        !tool_json_equals(j, tool_json_get(j, structure, "status"), "pdf-warnings") &&
        !tool_json_equals(j, tool_json_get(j, structure, "status"), "unsupported-layout"))
        return -1;
    if (tool_json_token(j, tables)->next > tables + 1 || tool_json_token(j, items)->next > items + 1) {
        size_t text = tool_json_get(j, tool_json_get(j, page, "layout"), "text");
        const ToolJsonToken *t = tool_json_token(j, text);
        if (!t || t->kind != TOOL_JSON_STRING)
            return -1;
        layout = (char *)rt_malloc(t->end - t->start + 1);
        if (!layout || tool_json_string(j, text, layout, t->end - t->start + 1))
            goto done;
        length = rt_strlen(layout);
    }
    for (i = tables + 1; i < tool_json_token(j, tables)->next; i = tool_json_token(j, i)->next) {
        static const char *const consultation_columns[] = {"date", "committee", "responsibility"};
        static const char *const agenda_columns[] = {"number", "text", "reference"};
        size_t rows = tool_json_get(j, i, "rows"), columns = tool_json_get(j, i, "columns"), n = 0;
        int consultation = tool_json_equals(j, tool_json_get(j, i, "kind"), "consultation");
        if ((!consultation && !tool_json_equals(j, tool_json_get(j, i, "kind"), "agenda")) ||
            !array(j, rows) || !array(j, columns) ||
            (consultation && cell_shape(j, tool_json_get(j, i, "header"), length)))
            goto done;
        for (k = columns + 1; k < tool_json_token(j, columns)->next; k = tool_json_token(j, k)->next) {
            if (n >= 3 ||
                !tool_json_equals(j, k, consultation ? consultation_columns[n] : agenda_columns[n]))
                goto done;
            n++;
        }
        if (n != 3)
            goto done;
        for (k = rows + 1; k < tool_json_token(j, rows)->next; k = tool_json_token(j, k)->next) {
            size_t cells = tool_json_get(j, k, "cells"), n = 0;
            int ambiguous = tool_json_equals(j, tool_json_get(j, k, "status"), "ambiguous");
            if ((!ambiguous && !tool_json_equals(j, tool_json_get(j, k, "status"), "rule-supported")) ||
                !array(j, cells))
                goto done;
            for (cell = cells + 1; cell < tool_json_token(j, cells)->next;
                 cell = tool_json_token(j, cell)->next) {
                if (cell_shape(j, cell, length))
                    goto done;
                n++;
            }
            if (n != 3)
                goto done;
            counts[2] += ambiguous;
        }
        table_count++;
    }
    for (i = items + 1; i < tool_json_token(j, items)->next; i = tool_json_token(j, i)->next) {
        char number_value[32];
        int protocol = tool_json_equals(j, tool_json_get(j, i, "kind"), "protocol-top");
        if ((!protocol && !tool_json_equals(j, tool_json_get(j, i, "kind"), "agenda-top")) ||
            tool_json_string(j, tool_json_get(j, i, "number"), number_value, sizeof(number_value)) ||
            !number_value[0])
            goto done;
        if (protocol) {
            size_t references = tool_json_get(j, i, "references"), outcome = tool_json_get(j, i, "outcome");
            const ToolJsonToken *t = tool_json_token(j, outcome);
            if (cell_shape(j, tool_json_get(j, i, "heading"), length) ||
                cell_shape(j, tool_json_get(j, i, "body"), length) || !array(j, references) || !t ||
                (t->kind != TOOL_JSON_NULL && cell_shape(j, outcome, length)))
                goto done;
            for (k = references + 1; k < tool_json_token(j, references)->next;
                 k = tool_json_token(j, k)->next)
                if (cell_shape(j, k, length))
                    goto done;
        } else {
            unsigned long long table, row;
            size_t target = tables + 1, rows;
            if (tool_json_uint(j, tool_json_get(j, i, "table"), &table) || table >= table_count ||
                tool_json_uint(j, tool_json_get(j, i, "row"), &row) ||
                (!tool_json_equals(j, tool_json_get(j, i, "section"), "public") &&
                 !tool_json_equals(j, tool_json_get(j, i, "section"), "nonpublic") &&
                 !tool_json_equals(j, tool_json_get(j, i, "section"), "unknown")))
                goto done;
            for (k = 0; k < table; k++)
                target = tool_json_token(j, target)->next;
            rows = tool_json_get(j, target, "rows");
            k = rows + 1;
            while (row && k < tool_json_token(j, rows)->next) {
                k = tool_json_token(j, k)->next;
                row--;
            }
            if (k >= tool_json_token(j, rows)->next ||
                !tool_json_equals(j, tool_json_get(j, target, "kind"), "agenda") ||
                !tool_json_equals(j, tool_json_get(j, k, "status"), "rule-supported"))
                goto done;
        }
        item_count++;
    }
    counts[0] += table_count;
    counts[1] += item_count;
    counts[3] += table_count || item_count;
    counts[4] += tool_json_equals(j, tool_json_get(j, structure, "status"), "pdf-warnings") ||
                 tool_json_equals(j, tool_json_get(j, structure, "status"), "unsupported-layout");
    status = 0;
done:
    rt_free(layout);
    return status;
}

typedef struct {
    size_t start, length;
} Source;
typedef struct {
    ToolByteBuffer text;
    ToolArray sources;
    const PdfTextSpan *last;
} Cell;
typedef struct {
    size_t first, count;
    double y;
    Cell all;
} Row;
typedef struct {
    const PdfTextLayout *layout;
    const PdfTextSpan **spans;
    Row *rows;
    size_t count;
    ToolByteBuffer tables, items, diagnostics;
    PdfStructureResult *result;
} Page;

static int put(ToolByteBuffer *out, const char *s) { return tool_byte_buffer_append_cstr(out, s); }
static int contains(const char *text, const char *needle) {
    size_t n = rt_strlen(needle), length = rt_strlen(text), i;
    for (i = 0; i + n <= length; i++)
        if (!memcmp(text + i, needle, n))
            return 1;
    return 0;
}
static int whitespace(const char *text, size_t start, size_t end) {
    while (start < end)
        if (!rt_is_space(text[start++]))
            return 0;
    return 1;
}
static void cell_init(Cell *cell) {
    rt_memset(cell, 0, sizeof(*cell));
    tool_byte_buffer_init(&cell->text);
    tool_array_init(&cell->sources, sizeof(Source));
}
static void cell_free(Cell *cell) {
    tool_byte_buffer_free(&cell->text);
    tool_array_free(&cell->sources);
}
static const char *cell_text(Cell *cell) {
    while (cell->text.size && cell->text.data[cell->text.size - 1] == ' ')
        cell->text.size--;
    if (tool_byte_buffer_terminate(&cell->text))
        return 0;
    return (const char *)cell->text.data;
}
static int cell_add(Cell *cell, const PdfTextLayout *layout, const PdfTextSpan *span) {
    const PdfTextSpan *last = cell->last;
    Source *source =
        cell->sources.count ? (Source *)tool_array_get(&cell->sources, cell->sources.count - 1) : 0;
    size_t i;
    int separate = last && (last->start + last->length != span->start || last->y - span->y > 2 ||
                            span->y - last->y > 2);
    if (separate && cell->text.size && cell->text.data[cell->text.size - 1] != ' ' &&
        put(&cell->text, " "))
        return -1;
    for (i = span->start; i < span->start + span->length; i++) {
        unsigned char ch = (unsigned char)layout->text[i];
        if (rt_is_space((char)ch)) {
            if (!cell->text.size || cell->text.data[cell->text.size - 1] == ' ')
                continue;
            ch = ' ';
        }
        if (tool_byte_buffer_append(&cell->text, &ch, 1))
            return -1;
    }
    if (source && source->start + source->length <= span->start &&
        whitespace(layout->text, source->start + source->length, span->start))
        source->length = span->start + span->length - source->start;
    else {
        source = (Source *)tool_array_append(&cell->sources);
        if (!source)
            return -1;
        source->start = span->start;
        source->length = span->length;
    }
    cell->last = span;
    return 0;
}
static int cell_json(ToolByteBuffer *out, Cell *cell) {
    size_t i;
    const char *text;
    while (cell->text.size && cell->text.data[cell->text.size - 1] == ' ')
        cell->text.size--;
    text = cell_text(cell);
    if (!text || put(out, "{\"text\":") || tool_json_append_string(out, text) || put(out, ",\"sources\":["))
        return -1;
    for (i = 0; i < cell->sources.count; i++) {
        const Source *s = (const Source *)tool_array_get(&cell->sources, i);
        if ((i && put(out, ",")) || put(out, "{\"start\":") || tool_byte_buffer_append_uint(out, s->start) ||
            put(out, ",\"length\":") || tool_byte_buffer_append_uint(out, s->length) || put(out, "}"))
            return -1;
    }
    return put(out, "]}");
}
static int compare_y(const void *left, const void *right) {
    const PdfTextSpan *a = *(const PdfTextSpan *const *)left;
    const PdfTextSpan *b = *(const PdfTextSpan *const *)right;
    if (a->y != b->y)
        return a->y > b->y ? -1 : 1;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    return a->start < b->start ? -1 : a->start != b->start;
}
static int compare_x(const void *left, const void *right) {
    const PdfTextSpan *a = *(const PdfTextSpan *const *)left;
    const PdfTextSpan *b = *(const PdfTextSpan *const *)right;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    return a->start < b->start ? -1 : a->start != b->start;
}
static int geometry(Page *page) {
    const PdfTextLayout *l = page->layout;
    size_t i, used = 0;
    page->spans = (const PdfTextSpan **)rt_malloc(l->span_count * sizeof(*page->spans) + 1);
    page->rows = (Row *)rt_malloc((l->span_count + 1) * sizeof(Row));
    if (!page->spans || !page->rows)
        return -1;
    for (i = 0; i < l->span_count; i++) {
        const PdfTextSpan *s = &l->spans[i];
        if (s->start > l->length || s->length > l->length - s->start ||
            !(s->x >= -1e9 && s->x <= 1e9 && s->y >= -1e9 && s->y <= 1e9) || s->end_y - s->y > 1 ||
            s->y - s->end_y > 1 || s->end_x < s->x - 1)
            return 1;
        if (!whitespace(l->text, s->start, s->start + s->length))
            page->spans[used++] = s;
    }
    rt_sort(page->spans, used, sizeof(*page->spans), compare_y);
    for (i = 0; i < used;) {
        Row *row = &page->rows[page->count++];
        size_t k;
        row->first = i;
        row->y = page->spans[i]->y;
        while (i < used && row->y - page->spans[i]->y <= 2)
            i++;
        row->count = i - row->first;
        rt_sort(page->spans + row->first, row->count, sizeof(*page->spans), compare_x);
        cell_init(&row->all);
        for (k = row->first; k < i; k++)
            if (cell_add(&row->all, l, page->spans[k]))
                return -1;
        while (row->all.text.size && row->all.text.data[row->all.text.size - 1] == ' ')
            row->all.text.size--;
        if (!cell_text(&row->all))
            return -1;
    }
    return 0;
}
static int row_cell(const Page *page, size_t row, double left, double right, Cell *cell) {
    const Row *r = &page->rows[row];
    size_t i;
    for (i = r->first; i < r->first + r->count; i++) {
        const PdfTextSpan *s = page->spans[i];
        if (s->x >= left && s->x < right && cell_add(cell, page->layout, s))
            return -1;
    }
    return 0;
}
static int diagnostic(Page *page, const char *code) {
    return (page->diagnostics.size && put(&page->diagnostics, ",")) ||
           tool_json_append_string(&page->diagnostics, code);
}
static const char *row_text(const Page *page, size_t row) {
    return (const char *)page->rows[row].all.text.data;
}
static double row_x(const Page *page, size_t row) { return page->spans[page->rows[row].first]->x; }
static int digit(char ch) { return ch >= '0' && ch <= '9'; }
static int reference(const char *text) {
    size_t i, n = rt_strlen(text);
    if (n < 11 || !digit(text[0]) || !digit(text[1]) || memcmp(text + 2, "/SVV/", 5))
        return 0;
    for (i = 7; i < 11; i++)
        if (!digit(text[i]))
            return 0;
    if (n == 11)
        return 1;
    if (n > 18 || text[11] != '-')
        return 0;
    for (i = 12; i < n; i++)
        if (!digit(text[i]))
            return 0;
    return n > 12;
}
static int date_shape(const char *s) {
    size_t i;
    if (rt_strlen(s) != 10)
        return 0;
    for (i = 0; i < 10; i++)
        if (i == 2 || i == 5 ? s[i] != '.' : !digit(s[i]))
            return 0;
    return 1;
}
static int top_number(const char *s, char out[32], size_t *length) {
    size_t i = 0, components = 0, digits = 0;
    while (digit(s[i]) || s[i] == '.') {
        if (i >= 30)
            return 0;
        if (s[i] == '.') {
            if (!digits || ++components > 5)
                return 0;
            digits = 0;
        } else if (++digits > 4)
            return 0;
        out[i] = s[i];
        i++;
    }
    if (!i || (!rt_is_space(s[i]) && s[i]))
        return 0;
    *length = i;
    if (!digits)
        i--;
    out[i] = 0;
    return i != 0;
}
static int top_compare(const char *a, const char *b) {
    size_t i = 0, j = 0;
    while (a[i] || b[j]) {
        unsigned int x = 0, y = 0;
        while (digit(a[i]))
            x = x * 10 + a[i++] - '0';
        while (digit(b[j]))
            y = y * 10 + b[j++] - '0';
        if (x != y)
            return x < y ? -1 : 1;
        if (!a[i] || !b[j])
            return a[i] ? 1 : b[j] ? -1 : 0;
        i++;
        j++;
    }
    return 0;
}
static int footer(const char *s) {
    return contains(s, " in Zusammenstellung") || !rt_strncmp(s, "gez.", 4) ||
           !rt_strncmp(s, "Potsdam,", 8);
}
static int outcome(const char *s) {
    static const char *const labels[] = {"unge\u00e4ndert beschlossen",
                                         "ge\u00e4ndert beschlossen",
                                         "beschlossen",
                                         "zur\u00fcckgezogen",
                                         "abgelehnt",
                                         "vertagt",
                                         "erledigt",
                                         "zur Kenntnis genommen",
                                         "\u00fcberwiesen",
                                         "zur\u00fcckgestellt",
                                         "nicht behandelt"};
    size_t i;
    for (i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
        if (!rt_strcmp(s, labels[i]))
            return 1;
    return 0;
}
static int protocol_number(const char *text, char number_value[32]) {
    size_t length, prefix;
    if (!rt_strncmp(text, "Top ", 4) || !rt_strncmp(text, "TOP ", 4))
        prefix = 4;
    else if (!rt_strncmp(text, "zu TOP ", 7))
        prefix = 7;
    else
        return 0;
    return top_number(text + prefix, number_value, &length) && text[prefix + length] != 0;
}
static size_t protocol_metadata(const Page *page, size_t at) {
    size_t i;
    for (i = at + 1; i < page->count && i <= at + 12; i++) {
        const char *s = row_text(page, i);
        char number_value[32];
        if (page->rows[i - 1].y - page->rows[i].y > 24 || protocol_number(s, number_value) ||
            footer(s))
            break;
        if (reference(s) || outcome(s))
            return i;
        if (row_x(page, i) < row_x(page, at) + 16)
            break;
    }
    return (size_t)-1;
}
static int protocols(Page *page) {
    size_t i;
    for (i = 0; i < page->count; i++) {
        char number_value[32];
        size_t metadata, end, j, body, result_row = (size_t)-1;
        Cell heading, content;
        int status = -1;
        if (!protocol_number(row_text(page, i), number_value))
            continue;
        metadata = protocol_metadata(page, i);
        if (metadata == (size_t)-1) {
            if (diagnostic(page, "unresolved-protocol-heading"))
                return -1;
            continue;
        }
        end = metadata;
        while (end < page->count) {
            char next[32];
            if (footer(row_text(page, end)) ||
                (end > metadata && protocol_number(row_text(page, end), next) &&
                 protocol_metadata(page, end) != (size_t)-1))
                break;
            end++;
        }
        body = metadata;
        while (body < end && reference(row_text(page, body)))
            body++;
        if (body < end && outcome(row_text(page, body)))
            result_row = body++;
        cell_init(&heading);
        cell_init(&content);
        for (j = i; j < metadata; j++)
            if (row_cell(page, j, -1e9, 1e9, &heading))
                goto done;
        for (j = body; j < end; j++)
            if (row_cell(page, j, -1e9, 1e9, &content))
                goto done;
        if ((page->items.size && put(&page->items, ",")) ||
            put(&page->items,
                "{\"kind\":\"protocol-top\",\"status\":\"rule-supported\",\"number\":") ||
            tool_json_append_string(&page->items, number_value) || put(&page->items, ",\"heading\":") ||
            cell_json(&page->items, &heading) || put(&page->items, ",\"references\":["))
            goto done;
        for (j = metadata; j < body && j != result_row; j++)
            if ((j > metadata && put(&page->items, ",")) ||
                cell_json(&page->items, &page->rows[j].all))
                goto done;
        if (put(&page->items, "],\"outcome\":") ||
            (result_row == (size_t)-1 ? put(&page->items, "null")
                                      : cell_json(&page->items, &page->rows[result_row].all)) ||
            put(&page->items, ",\"body\":") || cell_json(&page->items, &content) ||
            put(&page->items, "}"))
            goto done;
        page->result->agenda_items++;
        status = 0;
    done:
        cell_free(&heading);
        cell_free(&content);
        if (status)
            return status;
        i = end ? end - 1 : i;
    }
    return 0;
}
static double anchor_x(const Page *page, size_t row, const char *label) {
    const Row *r = &page->rows[row];
    size_t i, n = rt_strlen(label);
    for (i = r->first; i < r->first + r->count; i++) {
        const PdfTextSpan *s = page->spans[i];
        size_t at;
        for (at = s->start; at < s->start + s->length && at + n <= page->layout->length; at++)
            if (!memcmp(page->layout->text + at, label, n))
                return s->x;
    }
    return -1e9;
}
static int cells_json(ToolByteBuffer *out, Cell cells[3], const char *status) {
    size_t i;
    if (put(out, "{\"status\":") || tool_json_append_string(out, status) || put(out, ",\"cells\":["))
        return -1;
    for (i = 0; i < 3; i++)
        if ((i && put(out, ",")) || cell_json(out, &cells[i]))
            return -1;
    return put(out, "]}");
}
static int centered_committee(const Page *page, size_t at, const double x[3], Cell cells[3]) {
    Cell middle[3], lower[3];
    size_t k;
    double upper_gap, lower_gap, offset;
    const char *date;
    int result = 0;
    if (at + 2 >= page->count)
        return 0;
    upper_gap = page->rows[at].y - page->rows[at + 1].y;
    lower_gap = page->rows[at + 1].y - page->rows[at + 2].y;
    offset = row_x(page, at) - row_x(page, at + 2);
    if (upper_gap < 4 || upper_gap > 22 || lower_gap < 4 || lower_gap > 22 ||
        upper_gap - lower_gap > 2 || lower_gap - upper_gap > 2 || offset < -4 || offset > 4 ||
        row_x(page, at) < x[1] - 4)
        return 0;
    for (k = 0; k < 3; k++) {
        cell_init(&middle[k]);
        cell_init(&lower[k]);
    }
    for (k = 0; k < 3; k++)
        if (row_cell(page, at + 1, x[k] - 4, k == 2 ? 1e9 : x[k + 1] - 4, &middle[k]) ||
            row_cell(page, at + 2, x[k] - 4, k == 2 ? 1e9 : x[k + 1] - 4, &lower[k])) {
            result = -1;
            goto done;
        }
    date = cell_text(&middle[0]);
    if (!date) {
        result = -1;
        goto done;
    }
    if (!date_shape(date) || lower[0].text.size || !lower[1].text.size || lower[2].text.size)
        goto done;
    for (k = 0; k < 3; k++)
        if (row_cell(page, at + 1, x[k] - 4, k == 2 ? 1e9 : x[k + 1] - 4, &cells[k]) ||
            row_cell(page, at + 2, x[k] - 4, k == 2 ? 1e9 : x[k + 1] - 4, &cells[k])) {
            result = -1;
            goto done;
        }
    result = 1;
done:
    for (k = 0; k < 3; k++) {
        cell_free(&middle[k]);
        cell_free(&lower[k]);
    }
    return result;
}
static int consultation(Page *page) {
    size_t i;
    for (i = 0; i < page->count; i++) {
        const char *s = row_text(page, i);
        double x[3];
        ToolByteBuffer rows;
        size_t j, count = 0;
        int status = -1;
        if (!contains(s, "Gremium") || !contains(s, "Zust\u00e4ndigkeit") ||
            !(contains(s, "Datum") || contains(s, "Sitzungstermin") || contains(s, "geplanter")))
            continue;
        x[0] = row_x(page, i);
        x[1] = anchor_x(page, i, "Gremium");
        x[2] = anchor_x(page, i, "Zust\u00e4ndigkeit");
        if (x[1] - x[0] < 20 || x[2] - x[1] < 30) {
            if (diagnostic(page, "unseparated-consultation-columns"))
                return -1;
            continue;
        }
        tool_byte_buffer_init(&rows);
        for (j = i + 1; j < page->count; j++) {
            Cell cells[3];
            const char *first;
            int ambiguous = 0, error = 0;
            size_t k;
            if (page->rows[j - 1].y - page->rows[j].y > 32 || footer(row_text(page, j)))
                break;
            for (k = 0; k < 3; k++) {
                cell_init(&cells[k]);
                if (row_cell(page, j, x[k] - 4, k == 2 ? 1e9 : x[k + 1] - 4, &cells[k]))
                    error = 1;
            }
            first = cell_text(&cells[0]);
            if (!first)
                error = 1;
            if (!error && !*first && cells[1].text.size && !cells[2].text.size) {
                int centered = centered_committee(page, j, x, cells);
                if (centered < 0)
                    error = 1;
                else if (centered) {
                    j += 2;
                    first = cell_text(&cells[0]);
                    if (!first)
                        error = 1;
                }
            }
            if (!error && !date_shape(first)) {
                if (!rt_strcmp(first, "Sitzungstermin")) {
                    for (k = 0; k < 3; k++)
                        cell_free(&cells[k]);
                    continue;
                }
                if (*first || !count) {
                    for (k = 0; k < 3; k++)
                        cell_free(&cells[k]);
                    break;
                }
                ambiguous = 1;
            }
            if (!error && ((count && put(&rows, ",")) ||
                           cells_json(&rows, cells, ambiguous ? "ambiguous" : "rule-supported")))
                error = 1;
            for (k = 0; k < 3; k++)
                cell_free(&cells[k]);
            if (error)
                goto done;
            count++;
            page->result->uncertain_rows += ambiguous;
        }
        if (!count) {
            if (diagnostic(page, "consultation-without-supported-rows"))
                goto done;
            status = 0;
            goto done;
        }
        if ((page->tables.size && put(&page->tables, ",")) ||
            put(&page->tables, "{\"kind\":\"consultation\",\"columns\":[\"date\",\"committee\","
                               "\"responsibility\"],\"header\":") ||
            cell_json(&page->tables, &page->rows[i].all) || put(&page->tables, ",\"rows\":[") ||
            tool_byte_buffer_append(&page->tables, rows.data, rows.size) ||
            put(&page->tables, "]}"))
            goto done;
        page->result->tables++;
        status = 0;
    done:
        tool_byte_buffer_free(&rows);
        if (status)
            return -1;
        if (count)
            i = j - 1;
    }
    return 0;
}
static int agenda_flush(Page *page, ToolByteBuffer *rows, Cell cells[3], const char *number_value,
                        int section, size_t *count) {
    int supported = number_value[0] != 0;
    if (!cells[0].text.size && !cells[1].text.size && !cells[2].text.size)
        return 0;
    if ((*count && put(rows, ",")) ||
        cells_json(rows, cells, supported ? "rule-supported" : "ambiguous"))
        return -1;
    if (supported) {
        if ((page->items.size && put(&page->items, ",")) ||
            put(&page->items,
                "{\"kind\":\"agenda-top\",\"status\":\"rule-supported\",\"number\":") ||
            tool_json_append_string(&page->items, number_value) || put(&page->items, ",\"section\":") ||
            tool_json_append_string(&page->items, section == 1   ? "public"
                                     : section == 2 ? "nonpublic"
                                                    : "unknown") ||
            put(&page->items, ",\"table\":") || tool_byte_buffer_append_uint(&page->items, page->result->tables) ||
            put(&page->items, ",\"row\":") || tool_byte_buffer_append_uint(&page->items, *count) ||
            put(&page->items, "}"))
            return -1;
        page->result->agenda_items++;
    } else
        page->result->uncertain_rows++;
    (*count)++;
    return 0;
}
static int agenda_frame(Page *page, size_t from, PdfStructureState *state) {
    size_t i, k;
    for (i = from; i < page->count; i++) {
        const Row *r = &page->rows[i];
        if (!state->text_x) {
            char number_value[32];
            size_t length;
            const PdfTextSpan *first = page->spans[r->first];
            for (k = r->first + 1; k < r->first + r->count; k++) {
                const PdfTextSpan *s = page->spans[k], *last = page->spans[k - 1];
                Cell cell;
                const char *text;
                int valid;
                if (s->x - last->end_x < 8)
                    continue;
                cell_init(&cell);
                if (row_cell(page, i, -1e9, s->x, &cell)) {
                    cell_free(&cell);
                    return -1;
                }
                text = cell_text(&cell);
                if (!text) {
                    cell_free(&cell);
                    return -1;
                }
                valid = top_number(text, number_value, &length) && !text[length] && first->x < 150;
                cell_free(&cell);
                if (valid) {
                    state->number_x = first->x;
                    state->text_x = s->x;
                    break;
                }
            }
        }
        if (!state->reference_x) {
            for (k = r->first; k < r->first + r->count; k++) {
                const PdfTextSpan *s = page->spans[k];
                Cell cell;
                const char *text;
                int valid;
                if (s->x < 300 || !digit(page->layout->text[s->start]))
                    continue;
                cell_init(&cell);
                if (row_cell(page, i, s->x, 1e9, &cell)) {
                    cell_free(&cell);
                    return -1;
                }
                text = cell_text(&cell);
                if (!text) {
                    cell_free(&cell);
                    return -1;
                }
                valid = reference(text);
                cell_free(&cell);
                if (valid) {
                    state->reference_x = s->x;
                    break;
                }
            }
        }
    }
    return 0;
}
static int agendas(Page *page, size_t from, PdfStructureState *state) {
    ToolByteBuffer rows;
    Cell cells[3];
    char current[32] = "";
    size_t i, k, count = 0;
    int status = -1, section = state->section;
    if (agenda_frame(page, from, state))
        return -1;
    if (!state->text_x)
        return diagnostic(page, "unresolved-agenda-columns");
    tool_byte_buffer_init(&rows);
    for (k = 0; k < 3; k++)
        cell_init(&cells[k]);
    for (i = from; i < page->count; i++) {
        const char *s = row_text(page, i);
        Cell left, subject;
        const char *n, *title;
        char next[32];
        size_t length;
        int has_number;
        if (!rt_strcmp(s, "\u00d6ffentlicher Teil") ||
            !rt_strcmp(s, "Nicht\u00f6ffentlicher Teil")) {
            if (agenda_flush(page, &rows, cells, current, section, &count))
                goto done;
            for (k = 0; k < 3; k++) {
                cell_free(&cells[k]);
                cell_init(&cells[k]);
            }
            current[0] = 0;
            section = s[0] == 'N' ? 2 : 1;
            state->last_number[0] = 0;
            continue;
        }
        if (footer(s))
            break;
        cell_init(&left);
        cell_init(&subject);
        if (row_cell(page, i, state->number_x - 4, state->text_x - 4, &left) ||
            row_cell(page, i, state->text_x - 4, state->reference_x ? state->reference_x - 4 : 1e9,
                     &subject)) {
            cell_free(&left);
            cell_free(&subject);
            goto done;
        }
        n = cell_text(&left);
        title = cell_text(&subject);
        if (!n || !title) {
            cell_free(&left);
            cell_free(&subject);
            goto done;
        }
        has_number = top_number(n, next, &length) && !n[length] && title[0];
        if (has_number && state->last_number[0] && top_compare(next, state->last_number) <= 0) {
            if (diagnostic(page, "nonmonotonic-agenda-number")) {
                cell_free(&left);
                cell_free(&subject);
                goto done;
            }
            has_number = 0;
        }
        if (*n && !has_number) {
            cell_free(&left);
            cell_free(&subject);
            break;
        }
        cell_free(&left);
        cell_free(&subject);
        if (has_number) {
            if (agenda_flush(page, &rows, cells, current, section, &count))
                goto done;
            for (k = 0; k < 3; k++) {
                cell_free(&cells[k]);
                cell_init(&cells[k]);
            }
            memcpy(current, next, rt_strlen(next) + 1);
            memcpy(state->last_number, next, rt_strlen(next) + 1);
        } else if (!current[0] && !count && !state->last_number[0])
            continue;
        else if (i > from && page->rows[i - 1].y - page->rows[i].y > 32)
            break;
        if (row_cell(page, i, state->number_x - 4, state->text_x - 4, &cells[0]) ||
            row_cell(page, i, state->text_x - 4, state->reference_x ? state->reference_x - 4 : 1e9,
                     &cells[1]) ||
            (state->reference_x && row_cell(page, i, state->reference_x - 4, 1e9, &cells[2])))
            goto done;
    }
    if (agenda_flush(page, &rows, cells, current, section, &count))
        goto done;
    if (count) {
        if ((page->tables.size && put(&page->tables, ",")) ||
            put(&page->tables, "{\"kind\":\"agenda\",\"columns\":[\"number\",\"text\","
                               "\"reference\"],\"rows\":[") ||
            tool_byte_buffer_append(&page->tables, rows.data, rows.size) ||
            put(&page->tables, "]}"))
            goto done;
        page->result->tables++;
    }
    state->section = section;
    status = 0;
done:
    for (k = 0; k < 3; k++)
        cell_free(&cells[k]);
    tool_byte_buffer_free(&rows);
    return status;
}
int pdf_structure_page(ToolByteBuffer *out, const PdfTextLayout *layout, unsigned int warnings,
                       PdfStructureState *state, PdfStructureResult *result) {
    Page page;
    const char *status = "unrecognized";
    size_t i, from = (size_t)-1;
    int protocol = 0, built, error = -1;
    rt_memset(&page, 0, sizeof(page));
    rt_memset(result, 0, sizeof(*result));
    page.layout = layout;
    page.result = result;
    tool_byte_buffer_init(&page.tables);
    tool_byte_buffer_init(&page.items);
    tool_byte_buffer_init(&page.diagnostics);
    if (warnings) {
        status = "pdf-warnings";
        result->skipped = 1;
        rt_memset(state, 0, sizeof(*state));
        goto serialize;
    }
    if (!state->agenda && !contains(layout->text, "Protokollauszug") &&
        !contains(layout->text, "Tagesordnung:") && !contains(layout->text, "Gremium"))
        goto serialize;
    built = geometry(&page);
    if (built < 0)
        goto done;
    if (built) {
        status = "unsupported-layout";
        result->skipped = 1;
        rt_memset(state, 0, sizeof(*state));
        goto serialize;
    }
    for (i = 0; i < page.count; i++) {
        const char *s = row_text(&page, i);
        if (!rt_strcmp(s, "Protokollauszug") && i + 1 < page.count &&
            !rt_strcmp(row_text(&page, i + 1), "aus der"))
            protocol = 1;
        if (!rt_strcmp(s, "Tagesordnung:") &&
            (contains(layout->text, "Bekanntmachung") || contains(layout->text, "Einladung")))
            from = i + 1;
    }
    if (protocol || contains(layout->text, "Drucksache Nr."))
        rt_memset(state, 0, sizeof(*state));
    if (protocol && protocols(&page))
        goto done;
    if (consultation(&page))
        goto done;
    if (from != (size_t)-1) {
        rt_memset(state, 0, sizeof(*state));
        state->agenda = 1;
    }
    if (state->agenda && agendas(&page, from == (size_t)-1 ? 0 : from, state))
        goto done;
    result->keep_layout = result->tables || result->agenda_items || page.diagnostics.size;
    if (page.diagnostics.size || result->uncertain_rows)
        status = "ambiguous";
    else if (result->tables || result->agenda_items)
        status = "rule-supported";
serialize:
    if (put(out, "{\"format\":1,\"rule\":\"potsdam-layout-structure-v1\",\"status\":") ||
        tool_json_append_string(out, status) || put(out, ",\"tables\":[") ||
        tool_byte_buffer_append(out, page.tables.data, page.tables.size) ||
        put(out, "],\"agendaItems\":[") ||
        tool_byte_buffer_append(out, page.items.data, page.items.size) ||
        put(out, "],\"diagnostics\":[") ||
        tool_byte_buffer_append(out, page.diagnostics.data, page.diagnostics.size) ||
        put(out, "]}"))
        goto done;
    error = 0;
done:
    for (i = 0; i < page.count; i++)
        cell_free(&page.rows[i].all);
    rt_free(page.rows);
    rt_free(page.spans);
    tool_byte_buffer_free(&page.tables);
    tool_byte_buffer_free(&page.items);
    tool_byte_buffer_free(&page.diagnostics);
    return error;
}

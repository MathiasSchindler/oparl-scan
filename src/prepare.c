#include "prepare.h"
#include "hash_util.h"
#include "platform.h"
#include "runtime.h"

#ifndef PREPARE_ENGINE_ID
#error PREPARE_ENGINE_ID must fingerprint the extraction sources
#endif
#define LIMIT (64U * 1024U * 1024U)
static int put(ToolByteBuffer *b, const char *s) { return tool_byte_buffer_append_cstr(b, s); }
static int number(ToolByteBuffer *b, unsigned long long n) {
    char text[32];
    rt_unsigned_to_string(n, text, sizeof(text));
    return put(b, text);
}
static int field(ToolByteBuffer *b, const char *k, const char *v) {
    return put(b, ",") || json_quote(b, k) || put(b, ":") || json_quote(b, v);
}
static int integer(ToolByteBuffer *b, const char *k, unsigned long long n) {
    return put(b, ",") || json_quote(b, k) || put(b, ":") || number(b, n);
}
static int raw(ToolByteBuffer *b, const Json *j, size_t n) {
    const JsonToken *t = json_token(j, n);
    return !t ? -1 : tool_byte_buffer_append(b, j->text + t->start, t->end - t->start);
}
int prepare_error(const char *message, const char *path) {
    tool_write_error("oparl-prepare", message, path ? path : "");
    return -1;
}
void prepare_hash(const unsigned char *bytes, size_t length, char out[65]) {
    unsigned char digest[32];
    crypto_sha256_hash(bytes, length, digest);
    hash_to_hex(digest, 32, out);
}
static int digest_valid(const char *s) {
    size_t i;
    if (rt_strlen(s) != 64)
        return 0;
    for (i = 0; i < 64; i++)
        if (tool_hex_value(s[i]) < 0)
            return 0;
    return 1;
}
static int string(const Json *j, size_t object, const char *name, char *out, size_t cap) {
    return json_string(j, json_get(j, object, name), out, cap);
}
static int array(const Json *j, size_t n) {
    const JsonToken *t = json_token(j, n);
    return t && t->kind == JSON_ARRAY;
}
static int keys(const Json *j, size_t object, const char *const *names, size_t count) {
    const JsonToken *t = json_token(j, object);
    size_t i, k;
    if (!t || t->kind != JSON_OBJECT)
        return -1;
    for (i = object + 1; i < t->next; i = json_token(j, i + 1)->next) {
        for (k = 0; k < count; k++)
            if (json_equals(j, i, names[k]))
                break;
        if (k == count)
            return -1;
        for (k = object + 1; k < i; k = json_token(j, k + 1)->next) {
            char a[128], b[128];
            if (json_string(j, i, a, sizeof(a)) || json_string(j, k, b, sizeof(b)) ||
                !rt_strcmp(a, b))
                return -1;
        }
    }
    return 0;
}
static const char *field_type(const char *name) {
    static const char *const dates[] = {"document.date",           "document.releaseDate",
                                        "submission.receivedDate", "submission.forwardedDate",
                                        "answer.dueDate",          "answer.extendedDueDate",
                                        "answer.receivedDate"};
    size_t i;
    for (i = 0; i < sizeof(dates) / sizeof(dates[0]); i++)
        if (!rt_strcmp(name, dates[i]))
            return "date";
    if (!rt_strcmp(name, "document.role"))
        return "literal";
    if (!rt_strcmp(name, "originator.label") || !rt_strcmp(name, "department.label") ||
        !rt_strcmp(name, "document.reference"))
        return "text";
    return 0;
}
int prepare_config_open(PrepareConfig *c, const char *path, int layout) {
    static const char *const root_keys[] = {"format", "templates"};
    static const char *const template_keys[] = {"id", "anchors", "stopAt", "fields",
                                                "excludeAnchors"};
    static const char *const field_keys[] = {"name",      "label",    "type",  "value",
                                             "placement", "maxLines", "stopAt"};
    size_t length, list, i, j, k;
    unsigned long long version;
    CryptoSha256Context sha;
    unsigned char digest[32];
    PlatformDirEntry info;
    rt_memset(c, 0, sizeof(*c));
    c->layout = layout;
    if (platform_get_path_info(path, &info) || info.size > 65536 ||
        tool_read_all_input(path, &c->bytes, &length) || length > 65536)
        return prepare_error("cannot read templates: ", path);
    if (json_parse(&c->json, (const char *)c->bytes, length))
        return prepare_error("invalid template JSON: ", path);
    list = json_get(&c->json, 0, "templates");
    if (keys(&c->json, 0, root_keys, 2) ||
        json_uint(&c->json, json_get(&c->json, 0, "format"), &version) || version != 1 ||
        !array(&c->json, list))
        return prepare_error("invalid template schema: ", path);
    for (i = list + 1; i < json_token(&c->json, list)->next; i = json_token(&c->json, i)->next) {
        char id[128];
        size_t anchors = json_get(&c->json, i, "anchors"), fields = json_get(&c->json, i, "fields");
        size_t stop = json_get(&c->json, i, "stopAt"), count = 0;
        size_t exclude = json_get(&c->json, i, "excludeAnchors");
        if (keys(&c->json, i, template_keys, 5) || string(&c->json, i, "id", id, sizeof(id)) ||
            !id[0] || !array(&c->json, anchors) || !array(&c->json, fields) ||
            (stop != JSON_NONE && !array(&c->json, stop)) ||
            (exclude != JSON_NONE && !array(&c->json, exclude)))
            goto invalid;
        for (j = list + 1; j < i; j = json_token(&c->json, j)->next)
            if (json_equals(&c->json, json_get(&c->json, j, "id"), id))
                goto invalid;
        for (k = 0; k < 3; k++) {
            size_t a = k == 0 ? anchors : k == 1 ? stop : exclude;
            if (a == JSON_NONE)
                continue;
            for (j = a + 1; j < json_token(&c->json, a)->next; j = json_token(&c->json, j)->next) {
                char text[256];
                if (json_string(&c->json, j, text, sizeof(text)) || !text[0])
                    goto invalid;
                if (!k)
                    count++;
            }
        }
        if (!count || count > 16)
            goto invalid;
        count = 0;
        for (j = fields + 1; j < json_token(&c->json, fields)->next;
             j = json_token(&c->json, j)->next) {
            char name[64], label[256], type[32], value[256], placement[32];
            unsigned long long lines;
            size_t field_stop = json_get(&c->json, j, "stopAt");
            if (++count > 16 || keys(&c->json, j, field_keys, 7) ||
                string(&c->json, j, "name", name, sizeof(name)) ||
                string(&c->json, j, "label", label, sizeof(label)) || !label[0] ||
                string(&c->json, j, "type", type, sizeof(type)))
                goto invalid;
            if (!field_type(name) || rt_strcmp(type, field_type(name)))
                goto invalid;
            if (json_get(&c->json, j, "maxLines") != JSON_NONE &&
                (rt_strcmp(type, "text") ||
                 json_uint(&c->json, json_get(&c->json, j, "maxLines"), &lines) || !lines ||
                 lines > 4))
                goto invalid;
            if (field_stop != JSON_NONE) {
                if (!array(&c->json, field_stop))
                    goto invalid;
                for (k = field_stop + 1; k < json_token(&c->json, field_stop)->next;
                     k = json_token(&c->json, k)->next)
                    if (json_string(&c->json, k, value, sizeof(value)) || !value[0])
                        goto invalid;
            }
            if (!rt_strcmp(type, "literal") &&
                (string(&c->json, j, "value", value, sizeof(value)) || !value[0]))
                goto invalid;
            if (json_get(&c->json, j, "placement") != JSON_NONE &&
                (string(&c->json, j, "placement", placement, sizeof(placement)) ||
                 (rt_strcmp(placement, "right") && rt_strcmp(placement, "below") &&
                  rt_strcmp(placement, "right-or-below"))))
                goto invalid;
            for (k = fields + 1; k < j; k = json_token(&c->json, k)->next)
                if (json_equals(&c->json, json_get(&c->json, k, "name"), name))
                    goto invalid;
        }
        if (!count)
            goto invalid;
    }
    prepare_hash(c->bytes, length, c->template_hash);
    crypto_sha256_init(&sha);
    crypto_sha256_update(&sha, (const unsigned char *)PREPARE_ENGINE_ID, sizeof(PREPARE_ENGINE_ID));
    crypto_sha256_update(&sha, c->bytes, length);
    crypto_sha256_update(&sha, (const unsigned char *)(layout ? "layout" : "header"), 6);
    crypto_sha256_final(&sha, digest);
    hash_to_hex(digest, 32, c->key);
    return 0;
invalid:
    return prepare_error("invalid or duplicate template definition: ", path);
}
void prepare_config_close(PrepareConfig *c) {
    json_free(&c->json);
    rt_free(c->bytes);
}
static int atomic_write(const char *path, const unsigned char *bytes, size_t length) {
    char scratch[4096], prefix[4096], temp[4096];
    int fd, status;
    if (tool_ensure_parent_dirs(path, scratch, sizeof(scratch)))
        return -1;
    tool_path_build_temp_prefix(path, "prepare-", prefix, sizeof(prefix));
    fd = platform_create_temp_file(temp, sizeof(temp), prefix, 0644);
    if (fd < 0)
        return -1;
    status = rt_write_all(fd, bytes, length);
    if (platform_close(fd))
        status = -1;
    if (!status && platform_rename_path(temp, path))
        status = -1;
    if (status)
        tool_remove_path(temp, 0);
    return status;
}
int prepare_manifest_write(const PrepareConfig *c, const char *root) {
    ToolByteBuffer b;
    char path[4096];
    int status;
    tool_byte_buffer_init(&b);
    status = put(&b, "{\"format\":1") || field(&b, "engine", PREPARE_ENGINE_ID) ||
             field(&b, "key", c->key) || field(&b, "templatesSha256", c->template_hash) ||
             put(&b, c->layout ? ",\"allPageLayout\":true}" : ",\"allPageLayout\":false}");
    if (!status)
        status = rt_join_path(root, "manifest.json", path, sizeof(path)) ||
                 atomic_write(path, b.data, b.size);
    tool_byte_buffer_free(&b);
    return status ? prepare_error("cannot publish preparation manifest: ", root) : 0;
}
int prepare_manifest_read(const char *root, char key[65]) {
    char path[4096], engine[128];
    unsigned char *bytes = 0;
    size_t length;
    Json j;
    unsigned long long format;
    int status;
    PlatformDirEntry info;
    if (rt_join_path(root, "manifest.json", path, sizeof(path)) ||
        platform_get_path_info(path, &info) || info.size > 65536 ||
        tool_read_all_input(path, &bytes, &length))
        return prepare_error("missing preparation manifest; run oparl-prepare: ", root);
    if (json_parse(&j, (const char *)bytes, length)) {
        rt_free(bytes);
        return prepare_error("invalid preparation manifest: ", root);
    }
    status = json_uint(&j, json_get(&j, 0, "format"), &format) || format != 1 ||
             string(&j, 0, "key", key, 65) || !digest_valid(key) ||
             string(&j, 0, "engine", engine, sizeof(engine)) ||
             rt_strcmp(engine, PREPARE_ENGINE_ID);
    json_free(&j);
    rt_free(bytes);
    return status ? prepare_error("stale preparation engine; run oparl-prepare: ", root) : 0;
}
static int artifact_path(const char *root, const char *key, const char *hash, char out[4096]) {
    char dir[4096], name[70];
    if (!digest_valid(key) || !digest_valid(hash))
        return -1;
    memcpy(name, hash, 64);
    memcpy(name + 64, ".json", 6);
    return rt_join_path(root, key, dir, sizeof(dir)) || rt_join_path(dir, name, out, 4096);
}
int prepared_open(PreparedDocument *d, const char *root, const char *key, const char *hash,
                  unsigned long long size) {
    char path[4096], actual[65], expected[65], source[65], pipeline[65], engine[128];
    PlatformDirEntry info;
    size_t length, pages, i;
    unsigned long long n, format, page = 0, warning_pages = 0;
    const JsonToken *data;
    rt_memset(d, 0, sizeof(*d));
    if (artifact_path(root, key, hash, path) || platform_get_path_info(path, &info) ||
        info.size > LIMIT || tool_read_all_input(path, &d->bytes, &length))
        return prepare_error("missing/oversized prepared document: ", hash);
    if (json_parse(&d->json, (const char *)d->bytes, length))
        goto invalid;
    d->data = json_get(&d->json, 0, "data");
    data = json_token(&d->json, d->data);
    if (!data || data->kind != JSON_OBJECT ||
        json_uint(&d->json, json_get(&d->json, 0, "format"), &format) || format != 1 ||
        string(&d->json, 0, "sha256", expected, sizeof(expected)))
        goto invalid;
    prepare_hash(d->bytes + data->start, data->end - data->start, actual);
    if (rt_strcmp(actual, expected) ||
        string(&d->json, d->data, "sourceSha256", source, sizeof(source)) ||
        rt_strcmp(source, hash) || string(&d->json, d->data, "key", pipeline, sizeof(pipeline)) ||
        rt_strcmp(pipeline, key) || string(&d->json, d->data, "engine", engine, sizeof(engine)) ||
        rt_strcmp(engine, PREPARE_ENGINE_ID) ||
        json_uint(&d->json, json_get(&d->json, d->data, "sourceBytes"), &n) || n != size)
        goto invalid;
    pages = json_get(&d->json, d->data, "pages");
    if (!array(&d->json, pages) || !array(&d->json, json_get(&d->json, d->data, "fields")))
        goto invalid;
    for (i = pages + 1; i < json_token(&d->json, pages)->next; i = json_token(&d->json, i)->next) {
        const JsonToken *text = json_token(&d->json, json_get(&d->json, i, "text"));
        if (json_uint(&d->json, json_get(&d->json, i, "page"), &n) || n != ++page || !text ||
            text->kind != JSON_STRING ||
            json_uint(&d->json, json_get(&d->json, i, "warnings"), &n) || n > 7)
            goto invalid;
        if (n)
            warning_pages++;
    }
    if (json_uint(&d->json, json_get(&d->json, d->data, "pageCount"), &n) || n != page ||
        json_uint(&d->json, json_get(&d->json, d->data, "warningPages"), &n) || n != warning_pages)
        goto invalid;
    {
        static const char *const names[] = {"matchedHeaders", "unmatchedHeaders",
                                            "ambiguousHeaders"};
        unsigned long long total = 0;
        const JsonToken *error = json_token(&d->json, json_get(&d->json, d->data, "error"));
        for (i = 0; i < 3; i++) {
            if (json_uint(&d->json, json_get(&d->json, d->data, names[i]), &n) || n > 1)
                goto invalid;
            total += n;
        }
        if (total != (page ? 1U : 0U) || !error ||
            (json_equals(&d->json, json_get(&d->json, d->data, "status"), "failed")
                 ? error->kind != JSON_STRING
                 : error->kind != JSON_NULL))
            goto invalid;
    }
    if (!json_equals(&d->json, json_get(&d->json, d->data, "status"), "complete") &&
        !json_equals(&d->json, json_get(&d->json, d->data, "status"), "warnings") &&
        !json_equals(&d->json, json_get(&d->json, d->data, "status"), "failed"))
        goto invalid;
    return 0;
invalid:
    return prepare_error("invalid/corrupt prepared document; regenerate with --force: ", hash);
}
void prepared_close(PreparedDocument *d) {
    json_free(&d->json);
    rt_free(d->bytes);
}
static size_t find(const char *text, size_t length, const char *needle, size_t from) {
    size_t n = rt_strlen(needle), i;
    for (i = from; n <= length && i <= length - n; i++)
        if (!memcmp(text + i, needle, n))
            return i;
    return (size_t)-1;
}
static const PdfTextSpan *span_at(const PdfTextLayout *l, size_t at) {
    size_t i;
    for (i = 0; i < l->span_count; i++)
        if (at >= l->spans[i].start && at - l->spans[i].start < l->spans[i].length)
            return &l->spans[i];
    return 0;
}
static int positioned(const PdfTextLayout *layout, size_t anchor, size_t start, size_t end,
                      const char *placement, unsigned long long max_lines) {
    const PdfTextSpan *a = span_at(layout, anchor), *v = span_at(layout, start);
    int right, below;
    size_t i, lines = 1;
    double y;
    if (!a || !v || a->end_y - a->y > 1 || a->y - a->end_y > 1)
        return 0;
    right = v->y - a->y <= 2 && a->y - v->y <= 2 && v->x >= a->x;
    below = a->y - v->y >= 4 && a->y - v->y <= 22 && v->x - a->x <= 4 && a->x - v->x <= 4;
    if (!((right && rt_strcmp(placement, "below")) || (below && rt_strcmp(placement, "right"))))
        return 0;
    y = v->y;
    for (i = 0; i < layout->span_count; i++) {
        const PdfTextSpan *s = &layout->spans[i];
        if (s->start >= end || s->start + s->length <= start)
            continue;
        if (s->end_y - s->y > 1 || s->y - s->end_y > 1)
            return 0;
        if (s->y - y > 2 || y - s->y > 2) {
            if (++lines > max_lines || y - s->y < 4 || y - s->y > 22 ||
                !((s->x - v->x <= 4 && v->x - s->x <= 4) || (s->x - a->x <= 4 && a->x - s->x <= 4)))
                return 0;
            y = s->y;
        }
    }
    return 1;
}
static int date_value(const char *s, char out[11]) {
    static const unsigned int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned int d, m, y, i, max;
    if (rt_strlen(s) != 10 || s[2] != '.' || s[5] != '.')
        return -1;
    for (i = 0; i < 10; i++)
        if (i != 2 && i != 5 && (s[i] < '0' || s[i] > '9'))
            return -1;
    d = (s[0] - '0') * 10 + s[1] - '0';
    m = (s[3] - '0') * 10 + s[4] - '0';
    y = (s[6] - '0') * 1000 + (s[7] - '0') * 100 + (s[8] - '0') * 10 + s[9] - '0';
    if (y < 1800 || y > 2199 || m < 1 || m > 12)
        return -1;
    max = days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (!d || d > max)
        return -1;
    memcpy(out, s + 6, 4);
    out[4] = '-';
    memcpy(out + 5, s + 3, 2);
    out[7] = '-';
    memcpy(out + 8, s, 2);
    out[10] = 0;
    return 0;
}
typedef struct {
    const PrepareConfig *config;
    ToolByteBuffer pages, fields;
    size_t count, warnings, matched, unmatched, ambiguous;
    int resource;
} Build;
static int header_limit(const Json *json, size_t definition, const PdfTextLayout *layout,
                        size_t *limit) {
    size_t stop = json_get(json, definition, "stopAt"), i;
    *limit = layout->length < 2048 ? layout->length : 2048;
    if (stop == JSON_NONE)
        return 0;
    for (i = stop + 1; i < json_token(json, stop)->next; i = json_token(json, i)->next) {
        char label[256];
        size_t at;
        if (json_string(json, i, label, sizeof(label)))
            return -1;
        at = find(layout->text, *limit, label, 0);
        if (at < *limit)
            *limit = at;
    }
    return 0;
}
static int annotate(Build *b, const PdfTextLayout *layout, unsigned int page,
                    unsigned int warnings) {
    const Json *j = &b->config->json;
    size_t list = json_get(j, 0, "templates"), i, best = JSON_NONE, score = 0, ties = 0;
    size_t cap = layout->length < 2048 ? layout->length : 2048;
    char label[256], value[257], source[257], type[32], name[64], id[128];
    if (warnings) {
        b->unmatched++;
        return 0;
    }
    for (i = list + 1; i < json_token(j, list)->next; i = json_token(j, i)->next) {
        size_t a = json_get(j, i, "anchors"), k, count = 0;
        size_t exclude = json_get(j, i, "excludeAnchors");
        int excluded = 0;
        if (header_limit(j, i, layout, &cap))
            return -1;
        if (exclude != JSON_NONE)
            for (k = exclude + 1; k < json_token(j, exclude)->next; k = json_token(j, k)->next) {
                if (json_string(j, k, label, sizeof(label)))
                    return -1;
                if (find(layout->text, cap, label, 0) != (size_t)-1)
                    excluded = 1;
            }
        if (excluded)
            continue;
        for (k = a + 1; k < json_token(j, a)->next; k = json_token(j, k)->next) {
            if (json_string(j, k, label, sizeof(label)))
                return -1;
            if (find(layout->text, cap, label, 0) == (size_t)-1)
                break;
            count++;
        }
        if (k != json_token(j, a)->next)
            continue;
        if (count > score) {
            best = i;
            score = count;
            ties = 1;
        } else if (count == score)
            ties++;
    }
    if (best == JSON_NONE) {
        b->unmatched++;
        return 0;
    }
    if (ties != 1) {
        b->ambiguous++;
        return 0;
    }
    b->matched++;
    if (string(j, best, "id", id, sizeof(id)))
        return -1;
    if (header_limit(j, best, layout, &cap))
        return -1;
    list = json_get(j, best, "fields");
    for (i = list + 1; i < json_token(j, list)->next; i = json_token(j, i)->next) {
        size_t start, end, at, k, n, boundary;
        const char *state = "rule-supported";
        char placement[32] = "right";
        unsigned long long max_lines = 1;
        if (string(j, i, "name", name, sizeof(name)) ||
            string(j, i, "label", label, sizeof(label)) || string(j, i, "type", type, sizeof(type)))
            return -1;
        if (json_get(j, i, "placement") != JSON_NONE &&
            string(j, i, "placement", placement, sizeof(placement)))
            return -1;
        if (json_get(j, i, "maxLines") != JSON_NONE &&
            json_uint(j, json_get(j, i, "maxLines"), &max_lines))
            return -1;
        at = find(layout->text, cap, label, 0);
        start = end = 0;
        value[0] = 0;
        if (at == (size_t)-1)
            state = "missing";
        else if (find(layout->text, cap, label, at + rt_strlen(label)) != (size_t)-1)
            state = "ambiguous";
        else {
            start = at + rt_strlen(label);
            boundary = cap;
            for (k = list + 1; k < json_token(j, list)->next; k = json_token(j, k)->next) {
                char next[256];
                size_t p;
                if (k == i)
                    continue;
                if (string(j, k, "label", next, sizeof(next)))
                    return -1;
                p = find(layout->text, cap, next, start);
                if (p < boundary)
                    boundary = p;
            }
            k = json_get(j, i, "stopAt");
            if (k != JSON_NONE) {
                size_t stop;
                for (stop = k + 1; stop < json_token(j, k)->next;
                     stop = json_token(j, stop)->next) {
                    char next[256];
                    size_t p;
                    if (json_string(j, stop, next, sizeof(next)))
                        return -1;
                    p = find(layout->text, cap, next, start);
                    if (p < boundary)
                        boundary = p;
                }
            }
            while (start < boundary &&
                   (rt_is_space(layout->text[start]) || layout->text[start] == ':'))
                start++;
            end = start;
            if (!rt_strcmp(type, "literal")) {
                start = at;
                end = at + rt_strlen(label);
                if (string(j, i, "value", value, sizeof(value)))
                    return -1;
            } else if (!rt_strcmp(type, "date")) {
                char normalized[11];
                while (end < boundary && !rt_is_space(layout->text[end]))
                    end++;
                if (end == start)
                    state = "missing";
                else if (end - start != 10)
                    state = "invalid";
                else {
                    memcpy(value, layout->text + start, 10);
                    value[10] = 0;
                    if (date_value(value, normalized))
                        state = "invalid";
                    else if (!rt_strcmp(normalized, "2000-01-01"))
                        state = "placeholder";
                    else
                        memcpy(value, normalized, 11);
                }
            } else {
                while (end < boundary &&
                       (max_lines > 1 || (layout->text[end] != '\n' && layout->text[end] != '\r')))
                    end++;
                while (end > start && rt_is_space(layout->text[end - 1]))
                    end--;
                if (end == start)
                    state = "missing";
                else if (end - start >= sizeof(value))
                    state = "too-long";
                else {
                    size_t used = 0;
                    for (k = start; k < end; k++)
                        if (!rt_is_space(layout->text[k]))
                            value[used++] = layout->text[k];
                        else if (used && value[used - 1] != ' ')
                            value[used++] = ' ';
                    value[used] = 0;
                }
            }
            if (!rt_strcmp(state, "rule-supported") && rt_strcmp(type, "literal") &&
                !positioned(layout, at, start, end, placement, max_lines))
                state = "unsupported-placement";
        }
        if (end - start < sizeof(source)) {
            memcpy(source, layout->text + start, end - start);
            source[end - start] = 0;
        }
        n = b->fields.size;
        if ((n && put(&b->fields, ",")) || put(&b->fields, "{\"field\":") ||
            json_quote(&b->fields, name) || field(&b->fields, "status", state) ||
            field(&b->fields, "templateId", id) ||
            field(&b->fields, "rule", "header-anchor-position-v2") ||
            integer(&b->fields, "page", page) || field(&b->fields, "placement", placement) ||
            field(&b->fields, "sourceLabel", label) || integer(&b->fields, "maxLines", max_lines) ||
            integer(&b->fields, "layoutTextStart", start) ||
            integer(&b->fields, "layoutTextLength", end - start) ||
            put(&b->fields, ",\"sourceText\":") ||
            (end - start < sizeof(source) ? json_quote(&b->fields, source)
                                          : put(&b->fields, "null")) ||
            put(&b->fields, ",\"value\":") ||
            (!rt_strcmp(state, "rule-supported") ? json_quote(&b->fields, value)
                                                 : put(&b->fields, "null")) ||
            put(&b->fields, "}"))
            return -1;
    }
    return 0;
}
static int signed_number(ToolByteBuffer *out, double number_value) {
    char text[40];
    if (!(number_value <= 1e9 && number_value >= -1e9))
        return -1;
    if (number_value < 0) {
        if (put(out, "-"))
            return -1;
        number_value = -number_value;
    }
    rt_unsigned_to_string((unsigned long long)(number_value * 1000), text, sizeof(text));
    return put(out, text);
}
static int page_callback(unsigned int page, const char *text, size_t length, unsigned int warnings,
                         const PdfTextLayout *layout, void *user) {
    Build *b = (Build *)user;
    ToolByteBuffer out;
    size_t i;
    int status = 0;
    tool_byte_buffer_init(&out);
    if (page == 1 && annotate(b, layout, page, warnings))
        goto error;
    if (put(&out, "{\"page\":") || number(&out, page) || integer(&out, "warnings", warnings) ||
        put(&out, ",\"text\":") || json_quote(&out, text))
        goto error;
    if (page == 1 || b->config->layout) {
        if (put(&out, ",\"layout\":{\"space\":\"pdf-user-space-milliunits\",\"text\":") ||
            json_quote(&out, layout->text) || put(&out, ",\"spans\":["))
            goto error;
        for (i = 0; i < layout->span_count; i++) {
            const PdfTextSpan *s = &layout->spans[i];
            if ((i && put(&out, ",")) || put(&out, "{\"start\":") || number(&out, s->start) ||
                integer(&out, "length", s->length) || put(&out, ",\"x\":") ||
                signed_number(&out, s->x) || put(&out, ",\"y\":") || signed_number(&out, s->y) ||
                put(&out, ",\"endX\":") || signed_number(&out, s->end_x) ||
                put(&out, ",\"endY\":") || signed_number(&out, s->end_y) ||
                put(&out, ",\"fontSize\":") || signed_number(&out, s->size) || put(&out, "}"))
                goto error;
        }
        if (put(&out, "]}"))
            goto error;
    }
    if (put(&out, "}") || b->pages.size + out.size > LIMIT / 2 ||
        (b->count && put(&b->pages, ",")) || tool_byte_buffer_append(&b->pages, out.data, out.size))
        goto error;
    b->count++;
    if (warnings)
        b->warnings++;
    tool_byte_buffer_free(&out);
    (void)length;
    return status;
error:
    b->resource = 1;
    tool_byte_buffer_free(&out);
    return -1;
}
int prepare_document(const PrepareConfig *c, const char *root, const unsigned char *bytes,
                     size_t length, int force, int *reused, int *incomplete) {
    char hash[65], path[4096], checksum[65];
    PlatformDirEntry info;
    Build build;
    ToolByteBuffer payload, envelope;
    const char *error = 0, *status;
    int result = -1, extracted;
    *reused = *incomplete = 0;
    prepare_hash(bytes, length, hash);
    if (artifact_path(root, c->key, hash, path))
        return prepare_error("artifact path too long: ", root);
    if (!force && !platform_get_path_info(path, &info)) {
        PreparedDocument doc;
        result = prepared_open(&doc, root, c->key, hash, length);
        if (!result) {
            *reused = 1;
            *incomplete =
                !json_equals(&doc.json, json_get(&doc.json, doc.data, "status"), "complete");
        }
        prepared_close(&doc);
        return result;
    }
    rt_memset(&build, 0, sizeof(build));
    build.config = c;
    tool_byte_buffer_init(&build.pages);
    tool_byte_buffer_init(&build.fields);
    tool_byte_buffer_init(&payload);
    tool_byte_buffer_init(&envelope);
    extracted = pdf_text_extract_layout(bytes, length, page_callback, &build, &error);
    if (build.resource) {
        prepare_error("prepared document exceeds result/RAM budget: ", hash);
        goto done;
    }
    status = extracted ? "failed" : build.warnings ? "warnings" : "complete";
    *incomplete = extracted || build.warnings;
    if (put(&payload, "{\"sourceSha256\":") || json_quote(&payload, hash) ||
        integer(&payload, "sourceBytes", length) || field(&payload, "engine", PREPARE_ENGINE_ID) ||
        field(&payload, "key", c->key) || field(&payload, "status", status) ||
        put(&payload, ",\"error\":") ||
        (extracted ? json_quote(&payload, error ? error : "PDF extraction failed")
                   : put(&payload, "null")) ||
        integer(&payload, "pageCount", build.count) ||
        integer(&payload, "warningPages", build.warnings) ||
        integer(&payload, "matchedHeaders", build.matched) ||
        integer(&payload, "unmatchedHeaders", build.unmatched) ||
        integer(&payload, "ambiguousHeaders", build.ambiguous) || put(&payload, ",\"fields\":[") ||
        tool_byte_buffer_append(&payload, build.fields.data, build.fields.size) ||
        put(&payload, "],\"pages\":[") ||
        tool_byte_buffer_append(&payload, build.pages.data, build.pages.size) ||
        put(&payload, "]}"))
        goto done;
    prepare_hash(payload.data, payload.size, checksum);
    if (put(&envelope, "{\"format\":1,\"sha256\":") || json_quote(&envelope, checksum) ||
        put(&envelope, ",\"data\":") ||
        tool_byte_buffer_append(&envelope, payload.data, payload.size) || put(&envelope, "}\n") ||
        envelope.size > LIMIT || atomic_write(path, envelope.data, envelope.size))
        goto done;
    result = 0;
done:
    tool_byte_buffer_free(&build.pages);
    tool_byte_buffer_free(&build.fields);
    tool_byte_buffer_free(&payload);
    tool_byte_buffer_free(&envelope);
    return result ? prepare_error("cannot prepare document: ", hash) : 0;
}
int prepared_summary(ToolByteBuffer *out, const PreparedDocument *d) {
    static const char *const names[] = {
        "sourceSha256",     "key",          "status",         "error",
        "pageCount",        "warningPages", "matchedHeaders", "unmatchedHeaders",
        "ambiguousHeaders", "fields"};
    size_t i;
    if (put(out, "{\"format\":1"))
        return -1;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (put(out, ",") || json_quote(out, names[i]) || put(out, ":") ||
            raw(out, &d->json, json_get(&d->json, d->data, names[i])))
            return -1;
    return put(out, "}");
}

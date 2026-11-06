#include "json.h"
#include "runtime.h"

typedef struct {
    Json *json;
    size_t position;
} Parser;

const JsonToken *json_token(const Json *json, size_t index) {
    return (const JsonToken *)tool_array_get_const(&json->tokens, index);
}

static void whitespace(Parser *p) {
    while (p->position < p->json->length) {
        char ch = p->json->text[p->position];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        p->position++;
    }
}

static int hex4(const char *text, unsigned int *out) {
    unsigned int value = 0;
    size_t i;
    for (i = 0; i < 4; i++) {
        int digit = tool_hex_value(text[i]);
        if (digit < 0) return -1;
        value = value * 16U + (unsigned int)digit;
    }
    *out = value;
    return 0;
}

static int unicode_escape(const char *text, size_t length, size_t *position,
                          unsigned int *out) {
    unsigned int first, second;
    if (length - *position < 4 || hex4(text + *position, &first) != 0) return -1;
    *position += 4;
    if (first >= 0xd800U && first <= 0xdbffU) {
        if (length - *position < 6 || text[*position] != '\\' ||
            text[*position + 1] != 'u' ||
            hex4(text + *position + 2, &second) != 0 ||
            second < 0xdc00U || second > 0xdfffU) return -1;
        *position += 6;
        first = 0x10000U + ((first - 0xd800U) << 10) + second - 0xdc00U;
    } else if (first >= 0xdc00U && first <= 0xdfffU) {
        return -1;
    }
    *out = first;
    return 0;
}

static int string_end(Parser *p) {
    const char *text = p->json->text;
    size_t length = p->json->length;
    p->position++;
    while (p->position < length) {
        unsigned char ch = (unsigned char)text[p->position++];
        if (ch == '"') return 0;
        if (ch < 32U) return -1;
        if (ch == '\\') {
            unsigned int codepoint;
            if (p->position == length) return -1;
            ch = (unsigned char)text[p->position++];
            if (ch == 'u') {
                if (unicode_escape(text, length, &p->position, &codepoint) != 0) return -1;
            } else if (ch != '"' && ch != '\\' && ch != '/' && ch != 'b' &&
                       ch != 'f' && ch != 'n' && ch != 'r' && ch != 't') {
                return -1;
            }
        }
    }
    return -1;
}

static int number_end(Parser *p) {
    const char *text = p->json->text;
    size_t length = p->json->length;
    size_t i = p->position;
    if (text[i] == '-' && ++i == length) return -1;
    if (text[i] == '0') {
        i++;
    } else {
        if (text[i] < '1' || text[i] > '9') return -1;
        while (i < length && text[i] >= '0' && text[i] <= '9') i++;
    }
    if (i < length && text[i] == '.') {
        size_t start = ++i;
        while (i < length && text[i] >= '0' && text[i] <= '9') i++;
        if (i == start) return -1;
    }
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        size_t start;
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) i++;
        start = i;
        while (i < length && text[i] >= '0' && text[i] <= '9') i++;
        if (i == start) return -1;
    }
    p->position = i;
    return 0;
}

static int value(Parser *p, unsigned int depth) {
    JsonToken *token;
    size_t index;
    char ch;
    whitespace(p);
    if (depth > 64U || p->position == p->json->length ||
        p->json->tokens.count >= 2000000U) return -1;
    index = p->json->tokens.count;
    token = (JsonToken *)tool_array_append(&p->json->tokens);
    if (token == 0) return -1;
    token->start = p->position;
    ch = p->json->text[p->position];
    if (ch == '{' || ch == '[') {
        char close = ch == '{' ? '}' : ']';
        int object = ch == '{';
        token->kind = object ? JSON_OBJECT : JSON_ARRAY;
        p->position++;
        whitespace(p);
        if (p->position < p->json->length && p->json->text[p->position] != close) {
            for (;;) {
                if (object) {
                    if (p->position == p->json->length ||
                        p->json->text[p->position] != '"' ||
                        value(p, depth + 1U) != 0) return -1;
                    whitespace(p);
                    if (p->position == p->json->length ||
                        p->json->text[p->position++] != ':') return -1;
                }
                if (value(p, depth + 1U) != 0) return -1;
                whitespace(p);
                if (p->position == p->json->length) return -1;
                if (p->json->text[p->position] == close) break;
                if (p->json->text[p->position++] != ',') return -1;
                whitespace(p);
            }
        }
        if (p->position == p->json->length ||
            p->json->text[p->position++] != close) return -1;
    } else if (ch == '"') {
        token->kind = JSON_STRING;
        if (string_end(p) != 0) return -1;
    } else if (ch == 't' || ch == 'f' || ch == 'n') {
        const char *word = ch == 't' ? "true" : ch == 'f' ? "false" : "null";
        size_t length = rt_strlen(word);
        token->kind = ch == 'n' ? JSON_NULL : JSON_BOOL;
        if (p->json->length - p->position < length ||
            rt_strncmp(p->json->text + p->position, word, length) != 0) return -1;
        p->position += length;
    } else {
        token->kind = JSON_NUMBER;
        if (number_end(p) != 0) return -1;
    }
    token = (JsonToken *)tool_array_get(&p->json->tokens, index);
    token->end = p->position;
    token->next = p->json->tokens.count;
    return 0;
}

int json_parse(Json *json, const char *text, size_t length) {
    Parser parser;
    json->text = text;
    json->length = length;
    tool_array_init(&json->tokens, sizeof(JsonToken));
    parser.json = json;
    parser.position = 0;
    if (rt_utf8_validate(text, length) != 0 || value(&parser, 0) != 0) goto invalid;
    whitespace(&parser);
    if (parser.position != length) goto invalid;
    return 0;
invalid:
    json_free(json);
    return -1;
}

void json_free(Json *json) {
    tool_array_free(&json->tokens);
}

int json_string(const Json *json, size_t index, char *out, size_t capacity) {
    const JsonToken *token = json_token(json, index);
    size_t position, used = 0;
    if (token == 0 || token->kind != JSON_STRING || capacity == 0) return -1;
    position = token->start + 1;
    while (position < token->end - 1) {
        unsigned char ch = (unsigned char)json->text[position++];
        char encoded[4];
        size_t count = 1;
        encoded[0] = (char)ch;
        if (ch == '\\') {
            ch = (unsigned char)json->text[position++];
            if (ch == 'u') {
                unsigned int codepoint;
                if (unicode_escape(json->text, token->end - 1, &position, &codepoint) != 0 ||
                    codepoint == 0 ||
                    rt_utf8_encode(codepoint, encoded, sizeof(encoded), &count) != 0) return -1;
            } else {
                switch (ch) {
                    case 'b': encoded[0] = '\b'; break;
                    case 'f': encoded[0] = '\f'; break;
                    case 'n': encoded[0] = '\n'; break;
                    case 'r': encoded[0] = '\r'; break;
                    case 't': encoded[0] = '\t'; break;
                    default: encoded[0] = (char)ch; break;
                }
            }
        }
        if (count >= capacity - used) return -1;
        memcpy(out + used, encoded, count);
        used += count;
    }
    out[used] = '\0';
    return 0;
}

int json_equals(const Json *json, size_t index, const char *text) {
    char decoded[256];
    return json_string(json, index, decoded, sizeof(decoded)) == 0 &&
           rt_strcmp(decoded, text) == 0;
}

int json_quote(ToolByteBuffer *buffer, const char *text) {
    static const char hex[] = "0123456789abcdef";
    const char *p = text;
    if (tool_byte_buffer_append_cstr(buffer, "\"")) return -1;
    while (*p) {
        const char *start = p;
        unsigned char ch;
        char escape[6] = {'\\','u','0','0','0','0'};
        size_t length = 6;
        while (*p && (unsigned char)*p >= 32 && *p != '"' && *p != '\\') p++;
        if (tool_byte_buffer_append(buffer, start, (size_t)(p-start))) return -1;
        if (!*p) break;
        ch = (unsigned char)*p++;
        if (ch == '"' || ch == '\\' || ch == '\n' || ch == '\r' || ch == '\t') {
            escape[1] = ch == '\n' ? 'n' : ch == '\r' ? 'r' : ch == '\t' ? 't' : (char)ch;
            length = 2;
        } else { escape[4] = hex[ch>>4]; escape[5] = hex[ch&15]; }
        if (tool_byte_buffer_append(buffer, escape, length)) return -1;
    }
    return tool_byte_buffer_append_cstr(buffer, "\"");
}

size_t json_get(const Json *json, size_t object, const char *key) {
    const JsonToken *parent = json_token(json, object);
    size_t index;
    if (parent == 0 || parent->kind != JSON_OBJECT) return JSON_NONE;
    index = object + 1;
    while (index < parent->next) {
        size_t member = index + 1;
        if (json_equals(json, index, key)) return member;
        index = json_token(json, member)->next;
    }
    return JSON_NONE;
}

int json_uint(const Json *json, size_t index, unsigned long long *out) {
    const JsonToken *token = json_token(json, index);
    char number[32];
    size_t length;
    if (token == 0 || token->kind != JSON_NUMBER) return -1;
    length = token->end - token->start;
    if (length == 0 || length >= sizeof(number)) return -1;
    memcpy(number, json->text + token->start, length);
    number[length] = '\0';
    return rt_parse_uint(number, out);
}

int json_true(const Json *json, size_t index) {
    const JsonToken *token = json_token(json, index);
    return token != 0 && token->kind == JSON_BOOL && json->text[token->start] == 't';
}

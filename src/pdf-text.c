#include "pdf-text.h"
#include "pdf.h"
#include "runtime.h"

#define TEXT_LIMIT (16U * 1024U * 1024U)

typedef struct {
    const unsigned char *data;
    size_t size, start, end;
    int kind;
} Token;

typedef struct {
    unsigned long long key;
    size_t text;
} Glyph;

typedef struct {
    const unsigned char *identity;
    ToolArray glyphs;
    ToolByteBuffer strings;
    unsigned int single[256];
    unsigned int width[256];
    unsigned int lengths;
    unsigned int warnings;
    int cid, mapped;
} Font;

typedef struct {
    PdfDocument document;
    ToolArray fonts, pages;
    unsigned char *visited;
    unsigned int warnings, image;
} Extractor;

typedef struct {
    const unsigned char *data;
    size_t size;
} Dictionary;

static int token_next(Token *token, size_t *offset, unsigned int depth) {
    size_t p = pdf_skip_ws(token->data, token->size, *offset), start = p;
    unsigned char ch;
    if (p == token->size) return 0;
    if (depth > 64) return -1;
    ch = token->data[p++];
    token->kind = 0;
    if (ch == '(') {
        unsigned int nesting = 1;
        while (p < token->size && nesting) {
            ch = token->data[p++];
            if (ch == '\\') {
                if (p < token->size) p++;
            } else if (ch == '(') nesting++;
            else if (ch == ')') nesting--;
        }
        if (nesting) return -1;
        token->kind = 1;
    } else if (ch == '<' && (p == token->size || token->data[p] != '<')) {
        while (p < token->size && token->data[p] != '>') p++;
        if (p == token->size) return -1;
        p++;
        token->kind = 1;
    } else if (ch == '[' || (ch == '<' && p < token->size && token->data[p] == '<')) {
        unsigned char close = ch == '[' ? ']' : '>';
        if (ch == '<') p++;
        for (;;) {
            Token child = *token;
            int result;
            p = pdf_skip_ws(token->data, token->size, p);
            if (p == token->size) return -1;
            if (token->data[p] == close) {
                p++;
                if (close == '>') {
                    if (p == token->size || token->data[p++] != '>') return -1;
                }
                break;
            }
            result = token_next(&child, &p, depth + 1);
            if (result <= 0) return -1;
        }
        token->kind = ch == '[' ? 2 : 3;
    } else {
        if (ch == '/') token->kind = 4;
        while (p < token->size && !pdf_is_delim(token->data[p])) p++;
    }
    token->start = start;
    token->end = p;
    *offset = p;
    return 1;
}

static int equals(const Token *token, const char *text) {
    size_t n = rt_strlen(text);
    return token->end - token->start == n &&
           memcmp(token->data + token->start, text, n) == 0;
}

static int raw_string(const Token *token, ToolByteBuffer *out) {
    size_t p = token->start + 1, end = token->end - 1;
    int hex = token->data[token->start] == '<', high = -1;
    out->size = 0;
    while (p < end) {
        unsigned char ch = token->data[p++];
        if (hex) {
            int digit;
            if (pdf_is_space(ch)) continue;
            digit = tool_hex_value((char)ch);
            if (digit < 0) return -1;
            if (high < 0) { high = digit; continue; }
            ch = (unsigned char)((high << 4) | digit);
            high = -1;
        } else if (ch == '\\') {
            unsigned int digits = 0, value = 0;
            if (p == end) return -1;
            ch = token->data[p++];
            if (ch >= '0' && ch <= '7') {
                value = ch - '0';
                while (p < end && digits++ < 2 && token->data[p] >= '0' && token->data[p] <= '7')
                    value = value * 8 + token->data[p++] - '0';
                ch = (unsigned char)value;
            } else if (ch == '\r' || ch == '\n') {
                if (ch == '\r' && p < end && token->data[p] == '\n') p++;
                continue;
            } else if (ch == 'n') ch = '\n';
            else if (ch == 'r') ch = '\r';
            else if (ch == 't') ch = '\t';
            else if (ch == 'b') ch = '\b';
            else if (ch == 'f') ch = '\f';
        }
        if (tool_byte_buffer_append(out, &ch, 1) != 0) return -1;
    }
    if (high >= 0) {
        unsigned char ch = (unsigned char)(high << 4);
        if (tool_byte_buffer_append(out, &ch, 1) != 0) return -1;
    }
    return 0;
}

static const PdfObjectSpan *object(const Extractor *extractor, unsigned int number,
                                    unsigned int generation) {
    size_t i;
    for (i = 0; i < extractor->document.objects_len; i++) {
        const PdfObjectSpan *span = &extractor->document.objects[i];
        if (span->number == number && span->generation == generation) return span;
    }
    return 0;
}

static Dictionary dictionary(const Extractor *extractor, const PdfObjectSpan *span) {
    Dictionary result;
    result.data = span->data != 0 ? span->data : extractor->document.data;
    result.data += span->body_start;
    result.size = span->body_end - span->body_start;
    return result;
}

static int reference(const Token *token, unsigned int *number, unsigned int *generation) {
    unsigned long long a, b;
    size_t p = token->start;
    if (pdf_parse_u64(token->data, token->size, &p, &a) != 0) return 0;
    p = pdf_skip_ws(token->data, token->size, p);
    if (pdf_parse_u64(token->data, token->size, &p, &b) != 0) return 0;
    p = pdf_skip_ws(token->data, token->size, p);
    if (p >= token->size || token->data[p] != 'R' || a > 0xffffffffULL || b > 0xffffffffULL)
        return 0;
    *number = (unsigned int)a;
    *generation = (unsigned int)b;
    return 1;
}

static int member(Dictionary dict, const char *key, Token *value) {
    size_t p;
    if (!pdf_find_top_key(dict.data, dict.size, key, &p)) return 0;
    p += rt_strlen(key);
    value->data = dict.data;
    value->size = dict.size;
    return token_next(value, &p, 0) == 1;
}

static int resolve(const Extractor *extractor, const Token *value, Dictionary *dict) {
    unsigned int n, g;
    const PdfObjectSpan *span;
    if (value->kind == 3) {
        dict->data = value->data + value->start;
        dict->size = value->end - value->start;
        return 1;
    }
    if (!reference(value, &n, &g) || (span = object(extractor, n, g)) == 0) return 0;
    *dict = dictionary(extractor, span);
    return 1;
}

static int code(const Token *token, unsigned int *value, unsigned int *length) {
    ToolByteBuffer raw;
    size_t i;
    int result;
    tool_byte_buffer_init(&raw);
    result = raw_string(token, &raw);
    if (result != 0 || raw.size == 0 || raw.size > 4) { tool_byte_buffer_free(&raw); return -1; }
    *value = 0;
    *length = (unsigned int)raw.size;
    for (i = 0; i < raw.size; i++) *value = (*value << 8) | raw.data[i];
    tool_byte_buffer_free(&raw);
    return 0;
}

static int utf8_append(ToolByteBuffer *out, unsigned int cp) {
    char bytes[4];
    size_t count;
    return rt_utf8_encode(cp, bytes, sizeof(bytes), &count) != 0 ||
           tool_byte_buffer_append(out, bytes, count) != 0 ? -1 : 0;
}

static int mapping(Font *font, unsigned int value, unsigned int length,
                    const Token *destination, unsigned int increment) {
    ToolByteBuffer raw;
    Glyph *glyph;
    size_t p = 0;
    unsigned int cp;
    tool_byte_buffer_init(&raw);
    if (font->glyphs.count >= 131072 || font->strings.size > TEXT_LIMIT ||
        raw_string(destination, &raw) != 0 || raw.size == 0 || raw.size % 2 != 0) goto fail;
    if (increment) {
        size_t n = raw.size;
        while (increment && n) {
            unsigned int sum = raw.data[--n] + increment;
            raw.data[n] = (unsigned char)sum;
            increment = sum >> 8;
        }
        if (increment) goto fail;
    }
    glyph = (Glyph *)tool_array_append(&font->glyphs);
    if (glyph == 0) goto fail;
    glyph->key = ((unsigned long long)length << 32) | value;
    glyph->text = font->strings.size;
    while (p < raw.size) {
        if (rt_utf16_decode(raw.data, raw.size, &p, 1, &cp) != 0) goto fail;
        if (cp == 0) cp = 0xfffd;
        if (cp < 32 && cp != '\n' && cp != '\t') cp = ' ';
        if (utf8_append(&font->strings, cp) != 0) goto fail;
    }
    if (tool_byte_buffer_append(&font->strings, "", 1) != 0) goto fail;
    font->lengths |= 1U << length;
    tool_byte_buffer_free(&raw);
    return 0;
fail:
    tool_byte_buffer_free(&raw);
    return -1;
}

static int compare_glyph(const void *a, const void *b) {
    unsigned long long x = ((const Glyph *)a)->key, y = ((const Glyph *)b)->key;
    return x < y ? -1 : x > y;
}

static int cmap(Font *font, const unsigned char *bytes, size_t size) {
    Token t;
    size_t p = 0;
    int mode = 0, result;
    t.data = bytes; t.size = size;
    while ((result = token_next(&t, &p, 0)) > 0) {
        if (equals(&t, "beginbfchar")) mode = 1;
        else if (equals(&t, "beginbfrange")) mode = 2;
        else if (equals(&t, "endbfchar") || equals(&t, "endbfrange")) mode = 0;
        else if (mode && t.kind == 1) {
            unsigned int a, b, length, other, j;
            Token target;
            if (code(&t, &a, &length) != 0 || token_next(&t, &p, 0) != 1) return -1;
            if (mode == 1) {
                if (t.kind != 1 || mapping(font, a, length, &t, 0) != 0) return -1;
            } else {
                if (code(&t, &b, &other) != 0 || length != other || b < a || b - a > 65535) return -1;
                target = t;
                if (token_next(&target, &p, 0) != 1) return -1;
                if (target.kind == 1) {
                    for (j = a; ; j++) {
                        if (mapping(font, j, length, &target, j - a) != 0) return -1;
                        if (j == b) break;
                    }
                } else if (target.kind == 2) {
                    size_t q = target.start + 1;
                    Token item = target;
                    for (j = a; ; j++) {
                        if (token_next(&item, &q, 0) != 1 || item.kind != 1 ||
                            mapping(font, j, length, &item, 0) != 0) return -1;
                        if (j == b) break;
                    }
                } else return -1;
            }
        }
    }
    if (result < 0 || font->glyphs.count == 0) return -1;
    rt_sort(font->glyphs.data, font->glyphs.count, sizeof(Glyph), compare_glyph);
    font->mapped = 1;
    return 0;
}

static unsigned int glyph_name(const Token *token) {
    static const struct { const char *name; unsigned int cp; } names[] = {
        {"/space",32},{"/adieresis",228},{"/odieresis",246},{"/udieresis",252},
        {"/Adieresis",196},{"/Odieresis",214},{"/Udieresis",220},{"/germandbls",223},
        {"/endash",0x2013},{"/emdash",0x2014},{"/bullet",0x2022},
        {"/quotesinglbase",0x201a},{"/quotedblbase",0x201e},
        {"/quoteleft",0x2018},{"/quoteright",0x2019},
        {"/quotedblleft",0x201c},{"/quotedblright",0x201d},
        {"/Euro",0x20ac},{"/fi",0xfb01},{"/fl",0xfb02},
        {"/eacute",233},{"/Eacute",201},{"/aacute",225},{"/agrave",224},
        {"/ccedilla",231},{"/ntilde",241},{"/oe",0x153},{"/OE",0x152},
        {"/period",46},{"/comma",44},{"/hyphen",45},{"/colon",58},{"/semicolon",59},
        {"/parenleft",40},{"/parenright",41},{"/slash",47},{"/ampersand",38}
    };
    size_t i;
    if (token->end - token->start == 2) return token->data[token->start + 1];
    for (i = 0; i < sizeof(names)/sizeof(names[0]); i++)
        if (equals(token, names[i].name)) return names[i].cp;
    if (token->end - token->start == 8 &&
        memcmp(token->data + token->start, "/uni", 4) == 0) {
        unsigned int cp = 0;
        for (i = token->start + 4; i < token->end; i++) {
            int v = tool_hex_value((char)token->data[i]);
            if (v < 0) return 0;
            cp = cp * 16 + (unsigned int)v;
        }
        return cp;
    }
    return 0;
}

static Font *font_get(Extractor *extractor, Dictionary dict) {
    Token value, encoding;
    Font *font;
    size_t i;
    unsigned int n, g;
    PdfBuffer decoded;
    for (i = 0; i < extractor->fonts.count; i++) {
        font = (Font *)tool_array_get(&extractor->fonts, i);
        if (font->identity == dict.data) {
            extractor->warnings |= font->warnings;
            return font;
        }
    }
    font = (Font *)tool_array_append(&extractor->fonts);
    if (font == 0) return 0;
    rt_memset(font, 0, sizeof(*font));
    font->identity = dict.data;
    tool_array_init(&font->glyphs, sizeof(Glyph));
    tool_byte_buffer_init(&font->strings);
    for (i = 0; i < 256; i++) { font->single[i] = i < 128 ? (unsigned int)i : 0; font->width[i] = 500; }
    font->single[0] = 0;
    font->cid = member(dict, "/Subtype", &value) && equals(&value, "/Type0");
    if (member(dict, "/BaseFont", &value) &&
        (equals(&value, "/Symbol") || equals(&value, "/ZapfDingbats")))
        for (i = 0; i < 256; i++) font->single[i] = 0;
    if (member(dict, "/Encoding", &encoding)) {
        Dictionary enc;
        Token base;
        int win = equals(&encoding, "/WinAnsiEncoding");
        if (resolve(extractor, &encoding, &enc)) {
            if (member(enc, "/BaseEncoding", &base)) win = equals(&base, "/WinAnsiEncoding");
        }
        if (win) for (i = 128; i < 256; i++) font->single[i] = rt_windows_1252_decode((unsigned char)i);
        if (resolve(extractor, &encoding, &enc) && member(enc, "/Differences", &value) && value.kind == 2) {
            size_t p = value.start + 1;
            unsigned long long index = 0;
            Token item = value;
            while (token_next(&item, &p, 0) == 1 && item.start < value.end - 1) {
                if (item.kind == 4) {
                    if (index < 256) font->single[index++] = glyph_name(&item);
                } else {
                    size_t q = item.start;
                    if (pdf_parse_u64(item.data, item.size, &q, &index) != 0) break;
                }
            }
        }
    }
    if (member(dict, "/Widths", &value) && value.kind == 2) {
        size_t p = value.start + 1, q;
        unsigned long long index = 0, width;
        Token item = value, first;
        if (member(dict, "/FirstChar", &first)) {
            q = first.start;
            (void)pdf_parse_u64(first.data, first.size, &q, &index);
        }
        while (index < 256 && token_next(&item, &p, 0) == 1 && item.start < value.end - 1) {
            q = item.start;
            if (pdf_parse_u64(item.data, item.size, &q, &width) == 0 && width < 10000)
                font->width[index] = (unsigned int)width;
            index++;
        }
    }
    if (member(dict, "/ToUnicode", &value) && reference(&value, &n, &g)) {
        const PdfObjectSpan *span = object(extractor, n, g);
        pdf_buffer_init(&decoded);
        if (span != 0 && pdf_object_stream_data(&extractor->document, span, 1, &decoded) == 0) {
            if (cmap(font, decoded.data, decoded.size) != 0) font->warnings |= PDF_TEXT_UNKNOWN_GLYPH;
        } else font->warnings |= PDF_TEXT_UNKNOWN_GLYPH;
        pdf_buffer_free(&decoded);
    }
    extractor->warnings |= font->warnings;
    return font;
}

static const char *mapped_glyph(const Font *font, unsigned long long key) {
    size_t a = 0, b = font->glyphs.count;
    const Glyph *glyphs = (const Glyph *)font->glyphs.data;
    while (a < b) {
        size_t mid = a + (b - a) / 2;
        if (glyphs[mid].key < key) a = mid + 1;
        else b = mid;
    }
    return a < font->glyphs.count && glyphs[a].key == key ?
           (const char *)font->strings.data + glyphs[a].text : 0;
}

static int show(Extractor *extractor, Font *font, const Token *token, ToolByteBuffer *text,
                 double size, double spacing, double word_spacing, double *advance) {
    ToolByteBuffer raw;
    size_t p = 0;
    int result = -1;
    tool_byte_buffer_init(&raw);
    if (raw_string(token, &raw) != 0) goto done;
    while (p < raw.size) {
        unsigned int cp = 0, length, value;
        const char *mapped = 0;
        value = raw.data[p];
        if (font != 0 && font->mapped) {
            for (length = 4; length != 0; length--) {
                size_t j;
                if (!(font->lengths & (1U << length)) || length > raw.size - p) continue;
                value = 0;
                for (j = 0; j < length; j++) value = (value << 8) | raw.data[p + j];
                mapped = mapped_glyph(font, ((unsigned long long)length << 32) | value);
                if (mapped != 0) break;
            }
            if (mapped != 0) {
                size_t n = rt_strlen(mapped);
                if (text->size > TEXT_LIMIT || n > TEXT_LIMIT - text->size) goto done;
                if (tool_byte_buffer_append_cstr(text, mapped) != 0) goto done;
                if (n >= 3 && rt_unicode_normalized_contains(mapped, n, "\xef\xbf\xbd", 3, 0))
                    extractor->warnings |= PDF_TEXT_UNKNOWN_GLYPH;
                *advance += (font->width[value < 256 ? value : 0] / 1000.0) * size + spacing;
                if (value == 32) *advance += word_spacing;
                p += length;
                continue;
            }
        } else if (font != 0 && !font->cid) cp = font->single[raw.data[p]];
        if (font && font->cid && raw.size - p >= 2) p += 2;
        else p++;
        if (cp == 0) { cp = 0xfffd; extractor->warnings |= PDF_TEXT_UNKNOWN_GLYPH; }
        if (cp < 32 && cp != '\n' && cp != '\t') cp = ' ';
        if (utf8_append(text, cp) != 0) goto done;
        *advance += (font ? font->width[value < 256 ? value : 0] / 1000.0 : .5) * size + spacing;
        if (value == 32) *advance += word_spacing;
        if (text->size > TEXT_LIMIT) goto done;
    }
    result = 0;
done:
    tool_byte_buffer_free(&raw);
    return result;
}

static int separator(ToolByteBuffer *text, unsigned char ch) {
    if (text->size == 0 || rt_is_space((char)text->data[text->size - 1])) return 0;
    return tool_byte_buffer_append(text, &ch, 1);
}

static int named_resource(Extractor *extractor, Dictionary resources, const char *category,
                           const Token *name, Dictionary *result) {
    Dictionary collection;
    Token value;
    char key[128];
    size_t n = name->end - name->start;
    if (n >= sizeof(key)) return 0;
    memcpy(key, name->data + name->start, n); key[n] = '\0';
    return member(resources, category, &value) && resolve(extractor, &value, &collection) &&
           member(collection, key, &value) && resolve(extractor, &value, result);
}

static double numeric(const Token *token) {
    size_t p = token->start;
    double result = 0, scale = 1;
    int sign = 1;
    if (p < token->end && token->data[p] == '-') { sign = -1; p++; }
    else if (p < token->end && token->data[p] == '+') p++;
    while (p < token->end && token->data[p] != '.') result = result * 10 + token->data[p++] - '0';
    if (p < token->end) p++;
    while (p < token->end) { scale *= 10; result += (token->data[p++] - '0') / scale; }
    return result * sign;
}

typedef struct { size_t font; double size, spacing, word_spacing, scale; } TextStyle;
typedef struct {
    TextStyle style, saved[64];
    double x, y, a;
    size_t saved_count;
    int positioned;
} TextState;

static int content(Extractor *extractor, const unsigned char *bytes, size_t size,
                    Dictionary resources, ToolByteBuffer *text, unsigned int depth, TextState *state) {
    Token token, operands[8];
    size_t p = 0, count = 0;
    int result;
    if (depth > 16) return -1;
    token.data = bytes; token.size = size;
    while ((result = token_next(&token, &p, 0)) > 0) {
        unsigned char ch = token.data[token.start];
        if (token.kind != 0 || ch == '-' || ch == '+' || ch == '.' || (ch >= '0' && ch <= '9')) {
            if (count < 8) operands[count++] = token;
            else { memmove(operands, operands + 1, sizeof(Token) * 7); operands[7] = token; }
            continue;
        }
        if (equals(&token, "q")) {
            if (state->saved_count == 64) return -1;
            state->saved[state->saved_count++] = state->style;
        } else if (equals(&token, "Q")) {
            if (state->saved_count) state->style = state->saved[--state->saved_count];
        } else if (equals(&token, "BT")) state->positioned = 0;
        else if (equals(&token, "Tc") && count == 1) state->style.spacing = numeric(operands);
        else if (equals(&token, "Tw") && count == 1) state->style.word_spacing = numeric(operands);
        else if (equals(&token, "Tz") && count == 1) state->style.scale = numeric(operands) / 100;
        else if (equals(&token, "Tf") && count >= 2) {
            Dictionary dict;
            if (named_resource(extractor, resources, "/Font", &operands[count - 2], &dict)) {
                Font *font = font_get(extractor, dict);
                if (font == 0) return -1;
                state->style.font = (size_t)(font - (Font *)extractor->fonts.data);
                state->style.size = numeric(&operands[count - 1]);
            } else {
                state->style.font = (size_t)-1;
                extractor->warnings |= PDF_TEXT_UNKNOWN_GLYPH;
            }
        } else if ((equals(&token, "Tj") || equals(&token, "'") || equals(&token, "\"")) && count) {
            Font *font = state->style.font < extractor->fonts.count ?
                         (Font *)tool_array_get(&extractor->fonts, state->style.font) : 0;
            double advance = 0;
            if (!equals(&token, "Tj") && separator(text, '\n') != 0) return -1;
            if (operands[count - 1].kind != 1 || show(extractor, font, &operands[count - 1], text,
                state->style.size, state->style.spacing, state->style.word_spacing, &advance) != 0)
                return -1;
            state->x += advance * state->a * state->style.scale;
        } else if (equals(&token, "TJ") && count && operands[count - 1].kind == 2) {
            Token item = operands[count - 1];
            size_t q = item.start + 1, end = item.end - 1;
            Font *font = state->style.font < extractor->fonts.count ?
                         (Font *)tool_array_get(&extractor->fonts, state->style.font) : 0;
            while (q < end && token_next(&item, &q, 0) == 1 && item.start < end) {
                if (item.kind == 1) {
                    double advance = 0;
                    if (show(extractor, font, &item, text, state->style.size,
                        state->style.spacing, state->style.word_spacing, &advance) != 0) return -1;
                    state->x += advance * state->a * state->style.scale;
                } else {
                    double adjustment = numeric(&item);
                    if (adjustment < -120 && separator(text, ' ') != 0) return -1;
                    state->x -= adjustment / 1000 * state->style.size * state->style.scale * state->a;
                }
            }
        } else if (equals(&token, "Tm") && count == 6) {
            double x = numeric(&operands[4]), y = numeric(&operands[5]);
            if (state->positioned && (y > state->y + 0.5 || y < state->y - 0.5) && separator(text, '\n') != 0)
                return -1;
            if (state->positioned && x > state->x + state->style.size * .15 && separator(text, ' ') != 0)
                return -1;
            state->x = x; state->y = y; state->a = numeric(operands); state->positioned = 1;
        } else if (equals(&token, "T*") || equals(&token, "ET") ||
                   ((equals(&token, "Td") || equals(&token, "TD")) && count == 2 && numeric(&operands[1]) != 0)) {
            if (separator(text, '\n') != 0) return -1;
        } else if ((equals(&token, "Td") || equals(&token, "TD")) &&
                   count == 2 && numeric(operands) != 0) {
            if (separator(text, ' ') != 0) return -1;
        } else if (equals(&token, "Do") && count) {
            Dictionary dict;
            Token subtype, value;
            if (named_resource(extractor, resources, "/XObject", &operands[count - 1], &dict) &&
                member(dict, "/Subtype", &subtype)) {
                if (equals(&subtype, "/Image")) extractor->image = 1;
                else if (equals(&subtype, "/Form")) {
                    size_t i;
                    Dictionary nested = resources;
                    TextState nested_state = *state;
                    PdfBuffer decoded;
                    if (member(dict, "/Resources", &value)) (void)resolve(extractor, &value, &nested);
                    pdf_buffer_init(&decoded);
                    for (i = 0; i < extractor->document.objects_len; i++) {
                        const PdfObjectSpan *span = &extractor->document.objects[i];
                        Dictionary candidate = dictionary(extractor, span);
                        if (candidate.data != dict.data) continue;
                        if (pdf_object_stream_data(&extractor->document, span, 1, &decoded) != 0 ||
                            content(extractor, decoded.data, decoded.size, nested, text, depth + 1, &nested_state) != 0)
                            extractor->warnings |= PDF_TEXT_UNSUPPORTED_STREAM;
                        break;
                    }
                    pdf_buffer_free(&decoded);
                }
            }
        } else if (equals(&token, "BI")) {
            extractor->image = 1;
            extractor->warnings |= PDF_TEXT_UNSUPPORTED_STREAM;
            return 0;
        }
        count = 0;
    }
    return result < 0 ? -1 : 0;
}

static int page_tree(Extractor *extractor, unsigned int n, unsigned int g, unsigned int depth) {
    const PdfObjectSpan *span = object(extractor, n, g);
    Dictionary dict;
    Token type, kids, child;
    size_t p;
    if (span == 0 || depth > 64 || n > extractor->document.max_object_number || extractor->visited[n])
        return -1;
    extractor->visited[n] = 1;
    dict = dictionary(extractor, span);
    if (member(dict, "/Type", &type) && equals(&type, "/Page")) {
        PdfPageRef *page = (PdfPageRef *)tool_array_append(&extractor->pages);
        if (page == 0) return -1;
        page->object_number = n; page->generation = g;
        return 0;
    }
    if (!member(dict, "/Kids", &kids) || kids.kind != 2) return -1;
    p = kids.start + 1; child = kids;
    while (p < kids.end - 1) {
        unsigned int number, generation;
        unsigned long long a, b;
        p = pdf_skip_ws(kids.data, kids.size, p);
        if (p == kids.end - 1) break;
        if (pdf_parse_u64(kids.data, kids.size, &p, &a) != 0) return -1;
        p = pdf_skip_ws(kids.data, kids.size, p);
        if (pdf_parse_u64(kids.data, kids.size, &p, &b) != 0) return -1;
        p = pdf_skip_ws(kids.data, kids.size, p);
        if (p >= kids.end || kids.data[p++] != 'R' || a > 0xffffffffULL || b > 0xffffffffULL) return -1;
        number = (unsigned int)a; generation = (unsigned int)b;
        if (page_tree(extractor, number, generation, depth + 1) != 0) return -1;
    }
    (void)child;
    return 0;
}

static Dictionary page_resources(Extractor *extractor, Dictionary page) {
    unsigned int depth;
    Token value;
    Dictionary resources;
    resources.data = 0; resources.size = 0;
    for (depth = 0; depth < 64; depth++) {
        if (member(page, "/Resources", &value) && resolve(extractor, &value, &resources)) return resources;
        if (!member(page, "/Parent", &value) || !resolve(extractor, &value, &page)) break;
    }
    return resources;
}

static int page_contents(Extractor *extractor, const Token *value, Dictionary resources,
                          ToolByteBuffer *text, unsigned int depth, TextState *state) {
    unsigned int n, g;
    const PdfObjectSpan *span;
    PdfBuffer decoded;
    if (depth > 64) return -1;
    if (value->kind == 2) {
        size_t p = value->start + 1;
        while (p < value->end - 1) {
            Token child = *value;
            unsigned long long a, b;
            p = pdf_skip_ws(value->data, value->size, p);
            if (p == value->end - 1) break;
            child.start = p;
            if (pdf_parse_u64(value->data, value->size, &p, &a) != 0) return -1;
            p = pdf_skip_ws(value->data, value->size, p);
            if (pdf_parse_u64(value->data, value->size, &p, &b) != 0) return -1;
            p = pdf_skip_ws(value->data, value->size, p);
            if (p >= value->end || value->data[p++] != 'R') return -1;
            child.kind = 0; child.end = p;
            if (page_contents(extractor, &child, resources, text, depth + 1, state) != 0) return -1;
        }
        return 0;
    }
    if (!reference(value, &n, &g) || (span = object(extractor, n, g)) == 0) return -1;
    {
        Dictionary body = dictionary(extractor, span);
        Token indirect;
        size_t p = 0;
        indirect.data = body.data; indirect.size = body.size;
        if (token_next(&indirect, &p, 0) != 1) return -1;
        if (indirect.kind == 2) return page_contents(extractor, &indirect, resources, text, depth + 1, state);
    }
    pdf_buffer_init(&decoded);
    if (pdf_object_stream_data(&extractor->document, span, 1, &decoded) != 0 ||
        content(extractor, decoded.data, decoded.size, resources, text, 0, state) != 0)
        extractor->warnings |= PDF_TEXT_UNSUPPORTED_STREAM;
    pdf_buffer_free(&decoded);
    return 0;
}

static int page_marks_only(const unsigned char *text, size_t length) {
    size_t p = 0;
    while (p < length) {
        int numbered = 0;
        size_t start;
        while (p < length && rt_is_space((char)text[p])) p++;
        if (p == length) return 1;
        if (length - p >= 5 && memcmp(text + p, "Seite", 5) == 0) {
            numbered = 1; p += 5;
            if (p < length && text[p] == ':') p++;
        }
        while (p < length && rt_is_space((char)text[p])) p++;
        start = p;
        while (p < length && text[p] >= '0' && text[p] <= '9') p++;
        if (start == p) return 0;
        while (p < length && rt_is_space((char)text[p])) p++;
        if (p < length && text[p] == '/') {
            p++;
            while (p < length && rt_is_space((char)text[p])) p++;
            start = p;
            while (p < length && text[p] >= '0' && text[p] <= '9') p++;
            if (start == p) return 0;
        } else if (length - p >= 3 && memcmp(text + p, "von", 3) == 0) {
            p += 3;
            while (p < length && rt_is_space((char)text[p])) p++;
            start = p;
            while (p < length && text[p] >= '0' && text[p] <= '9') p++;
            if (start == p) return 0;
            while (p < length && rt_is_space((char)text[p])) p++;
            if (length - p >= 2 && memcmp(text + p, "in", 2) == 0) {
                p += 2;
                while (p < length && rt_is_space((char)text[p])) p++;
                if (length - p < 16 || memcmp(text + p, "Zusammenstellung", 16)) return 0;
                p += 16;
            } else if (!numbered) return 0;
        }
    }
    return 1;
}

int pdf_text_extract(const unsigned char *bytes, size_t length, PdfTextPage callback,
                     void *user, const char **error) {
    Extractor extractor;
    const PdfObjectSpan *catalog;
    Dictionary dict;
    Token root;
    unsigned int n, g;
    size_t i;
    int result = -1;
    rt_memset(&extractor, 0, sizeof(extractor));
    tool_array_init(&extractor.fonts, sizeof(Font));
    tool_array_init(&extractor.pages, sizeof(PdfPageRef));
    *error = "invalid or unsupported PDF";
    if (pdf_document_parse(bytes, length, &extractor.document) != 0) goto done;
    if (extractor.document.max_object_number > 1000000) goto done;
    extractor.visited = (unsigned char *)rt_malloc((size_t)extractor.document.max_object_number + 1);
    if (extractor.visited == 0) goto done;
    rt_memset(extractor.visited, 0, (size_t)extractor.document.max_object_number + 1);
    catalog = object(&extractor, extractor.document.catalog_object_number, extractor.document.catalog_generation);
    if (catalog == 0) goto done;
    dict = dictionary(&extractor, catalog);
    if (!member(dict, "/Pages", &root) || !reference(&root, &n, &g) || page_tree(&extractor, n, g, 0) != 0)
        goto done;
    for (i = 0; i < extractor.pages.count; i++) {
        const PdfPageRef *page = (const PdfPageRef *)tool_array_get_const(&extractor.pages, i);
        const PdfObjectSpan *span = object(&extractor, page->object_number, page->generation);
        ToolByteBuffer text;
        Dictionary resources;
        Token contents;
        TextState state;
        rt_memset(&state, 0, sizeof(state));
        state.style.font = (size_t)-1; state.style.size = 12; state.style.scale = state.a = 1;
        extractor.warnings = extractor.image = 0;
        tool_byte_buffer_init(&text);
        dict = dictionary(&extractor, span);
        resources = page_resources(&extractor, dict);
        if (member(dict, "/Contents", &contents) && page_contents(&extractor, &contents, resources, &text, 0, &state) != 0)
            extractor.warnings |= PDF_TEXT_UNSUPPORTED_STREAM;
        if (extractor.image && page_marks_only(text.data, text.size)) extractor.warnings |= PDF_TEXT_NEEDS_OCR;
        if (tool_byte_buffer_terminate(&text) != 0 ||
            callback((unsigned int)i + 1, text.data != 0 ? (const char *)text.data : "", text.size,
                     extractor.warnings, user) != 0) {
            tool_byte_buffer_free(&text);
            *error = "cannot store extracted page";
            goto done;
        }
        tool_byte_buffer_free(&text);
    }
    result = 0;
done:
    for (i = 0; i < extractor.fonts.count; i++) {
        Font *font = (Font *)tool_array_get(&extractor.fonts, i);
        tool_array_free(&font->glyphs);
        tool_byte_buffer_free(&font->strings);
    }
    tool_array_free(&extractor.fonts);
    tool_array_free(&extractor.pages);
    rt_free(extractor.visited);
    pdf_document_free(&extractor.document);
    return result;
}

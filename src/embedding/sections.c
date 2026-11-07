#include "sections.h"
#include "crypto/sha256.h"
#include "index_io.h"

void eg_section_identity(const char *paper_id, const char pdf_sha[65], const char text_sha[65],
                         size_t start, size_t end, char identity[65]) {
    char offsets[96], number[32];
    unsigned char digest[32];
    CryptoSha256Context hash;
    rt_unsigned_to_string(start, offsets, sizeof(offsets));
    size_t n = tool_buffer_append_char(offsets, sizeof(offsets), rt_strlen(offsets), '-');
    rt_unsigned_to_string(end, number, sizeof(number));
    tool_buffer_append_cstr(offsets, sizeof(offsets), n, number);
    crypto_sha256_init(&hash);
    crypto_sha256_update(&hash, (const unsigned char *)paper_id, rt_strlen(paper_id) + 1);
    crypto_sha256_update(&hash, (const unsigned char *)pdf_sha, 65);
    crypto_sha256_update(&hash, (const unsigned char *)text_sha, 65);
    crypto_sha256_update(&hash, (const unsigned char *)offsets, rt_strlen(offsets));
    crypto_sha256_final(&hash, digest);
    eg_hex_digest(digest, identity);
}

static int continuation(unsigned char c) { return (c & 0xc0U) == 0x80U; }
static int space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

static size_t window(const char *text, size_t characters) {
    size_t i = 0, count = 0;
    while (text[i]) {
        if (!continuation((unsigned char)text[i]) && count++ == characters) break;
        i++;
    }
    return i;
}

static int decimal(const char *text, size_t end, size_t *at, unsigned int *value) {
    size_t begin = *at;
    *value = 0;
    while (*at < end && text[*at] >= '0' && text[*at] <= '9') {
        if (*value > 100000U) return -1;
        *value = *value * 10U + (unsigned int)(text[(*at)++] - '0');
    }
    return begin == *at || !*value ? -1 : 0;
}

int eg_form_number(const char *text, unsigned int *page, unsigned int *total) {
    size_t end = window(text, 400), matches = 0;
    for (size_t i = 0; i + 5 <= end; i++) {
        if (memcmp(text + i, "Seite", 5)) continue;
        size_t at = i + 5;
        while (at < end && (space(text[at]) || text[at] == ':')) at++;
        unsigned int p, n;
        if (decimal(text, end, &at, &p)) continue;
        while (at < end && space(text[at])) at++;
        if (at == end || text[at++] != '/') continue;
        while (at < end && space(text[at])) at++;
        if (decimal(text, end, &at, &n)) continue;
        *page = p; *total = n; matches++;
    }
    return matches == 1;
}

int eg_form_reference(const char *text, const char *reference) {
    size_t size = window(text, 2048), length = rt_strlen(reference);
    for (size_t i = 0; length && i + length <= size; i++) {
        if (memcmp(text + i, reference, length)) continue;
        char before = i ? text[i - 1] : ' ', after = text[i + length];
        if ((before >= '0' && before <= '9') || before == '/' || before == '-') continue;
        if ((after >= '0' && after <= '9') || after == '/' || after == '-') continue;
        return 1;
    }
    return 0;
}

int eg_section_tokens(const EgTokenizer *tokenizer, const char *text, size_t size,
                      unsigned int *tokens, unsigned int *count) {
    unsigned char input[2400 + sizeof(EG2_DOCUMENT_PREFIX)];
    if (size > 2400) return eg_error("section byte budget exceeded", 0);
    memcpy(input, EG2_DOCUMENT_PREFIX, sizeof(EG2_DOCUMENT_PREFIX) - 1);
    memcpy(input + sizeof(EG2_DOCUMENT_PREFIX) - 1, text, size);
    return eg_tokenize(tokenizer, input, size + sizeof(EG2_DOCUMENT_PREFIX) - 1, tokens, count);
}

int eg_sections(const EgTokenizer *tokenizer, const char *text, ToolArray *sections) {
    size_t length = rt_strlen(text), start = 0;
    unsigned int tokens[EG2_MAX_TOKENS], count;
    if (rt_utf8_validate(text, length))
        return eg_error("invalid UTF-8 in prepared main-form text", 0);
    while (start < length) {
        size_t end = start + (length - start < 2400 ? length - start : 2400);
        while (end < length && continuation((unsigned char)text[end])) end--;
        for (;;) {
            if (eg_section_tokens(tokenizer, text + start, end - start, tokens, &count)) return -1;
            if (count <= EG_SECTION_LIMIT) break;
            size_t remove = (end - start) / 8;
            end -= remove ? remove : 1;
            while (end > start && continuation((unsigned char)text[end])) end--;
            if (end == start) return eg_error("cannot fit a UTF-8 character into a section", 0);
        }
        if (end < length) {
            size_t preferred = end;
            while (preferred > start + (end - start) * 3 / 4) {
                char previous = text[preferred - 1];
                if (previous == '\n' || (space(text[preferred]) &&
                    (previous == '.' || previous == '!' || previous == '?'))) break;
                preferred--;
            }
            if (preferred > start + (end - start) * 3 / 4) {
                unsigned int candidate;
                if (eg_section_tokens(tokenizer, text + start, preferred - start, tokens, &candidate)) return -1;
                if (candidate <= EG_SECTION_LIMIT) { end = preferred; count = candidate; }
            }
        }
        EgSection *section = tool_array_append(sections);
        if (!section) return eg_error("cannot allocate section", 0);
        section->start = start; section->end = end; section->tokens = count;
        CryptoSha256Context hash;
        crypto_sha256_init(&hash);
        crypto_sha256_update(&hash, (const unsigned char *)EG_SECTION_RECIPE, sizeof(EG_SECTION_RECIPE) - 1);
        crypto_sha256_update(&hash, (const unsigned char *)text + start, end - start);
        crypto_sha256_final(&hash, section->key);
        if (end == length) break;
        size_t overlap = end - start < 160 ? (end - start) / 2 : 160;
        size_t next = end - overlap;
        while (next < end && continuation((unsigned char)text[next])) next++;
        while (next < end) {
            if (eg_tokenize(tokenizer, (const unsigned char *)text + next, end - next, tokens, &count)) return -1;
            if (count <= EG_SECTION_OVERLAP + 2U) break;
            next++;
            while (next < end && continuation((unsigned char)text[next])) next++;
        }
        if (next <= start) return eg_error("sectioner did not advance", 0);
        start = next;
    }
    return 0;
}

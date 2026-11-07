#ifndef EG_JSON_H
#define EG_JSON_H

/* Adapted from OParl-Scan's CC0 src/json.h and src/json.c. */
#include "tool_util.h"

#define EG_JSON_NONE ((size_t)-1)

typedef enum {
    EG_JSON_OBJECT, EG_JSON_ARRAY, EG_JSON_STRING, EG_JSON_NUMBER, EG_JSON_BOOL, EG_JSON_NULL
} JsonKind;

typedef struct {
    JsonKind kind;
    size_t start, end, next;
} EgJsonToken;

typedef struct {
    const char *text;
    size_t length;
    ToolArray tokens;
} EgJson;

int eg_json_parse(EgJson *json, const char *text, size_t length);
void eg_json_free(EgJson *json);
const EgJsonToken *eg_json_token(const EgJson *json, size_t index);
size_t eg_json_get(const EgJson *json, size_t object, const char *key);
int eg_json_string(const EgJson *json, size_t index, char *out, size_t capacity);
int eg_json_equals(const EgJson *json, size_t index, const char *text);
int eg_json_true(const EgJson *json, size_t index);
int eg_json_quote(ToolByteBuffer *buffer, const char *text);
char *eg_json_dup_string(const EgJson *json, size_t index, size_t limit);
int eg_json_uint(const EgJson *json, size_t index, unsigned long long *out);

#endif

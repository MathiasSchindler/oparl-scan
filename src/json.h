#ifndef RIS_JSON_H
#define RIS_JSON_H

#include "tool_util.h"

#define JSON_NONE ((size_t)-1)

typedef enum {
    JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_BOOL, JSON_NULL
} JsonKind;

typedef struct {
    JsonKind kind;
    size_t start;
    size_t end;
    size_t next;
} JsonToken;

typedef struct {
    const char *text;
    size_t length;
    ToolArray tokens;
} Json;

int json_parse(Json *json, const char *text, size_t length);
void json_free(Json *json);
const JsonToken *json_token(const Json *json, size_t index);
size_t json_get(const Json *json, size_t object, const char *key);
int json_string(const Json *json, size_t index, char *out, size_t capacity);
int json_equals(const Json *json, size_t index, const char *text);
int json_uint(const Json *json, size_t index, unsigned long long *out);
int json_true(const Json *json, size_t index);

#endif

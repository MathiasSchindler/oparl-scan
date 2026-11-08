#include "tool_json_parser.h"
#include "runtime.h"

static int check(int condition, const char *description) {
    if (condition) return 0;
    rt_write_cstr(2, "FAIL: ");
    rt_write_line(2, description);
    return 1;
}

int main(void) {
    static const char *invalid[] = {
        "", "{", "{\"x\":}", "{\"x\":1,}", "[1,]", "[}", "01", "1.",
        "1e+", "true false", "\"\\q\"", "\"\\uD800\"", "\"\\uDC00\"",
        "\"\\uD800\\u0000\"", "\"raw\nnewline\"", "\"\xc0\xaf\""
    };
    const char *text =
        "{\"data\":[{\"id\":\"https:\\/\\/example.test\\/a?x=1\\u0026y=2\","
        "\"name\":\"Gr\\u00fcne \\uD83C\\uDF33\",\"deleted\":false}],"
        "\"n\":18446744073709551615,\"overflow\":18446744073709551616,"
        "\"negative\":-1,\"fraction\":1.5,\"nul\":\"\\u0000\",\"yes\":true}";
    ToolJson json;
    char out[256];
    unsigned long long number;
    size_t data, object, i;
    int failures = 0;
    if (tool_json_parse(&json, text, rt_strlen(text)) != 0)
        return check(0, "valid nested document parses");
    data = tool_json_get(&json, 0, "data");
    object = data + 1;
    failures += check(tool_json_string(&json, tool_json_get(&json, object, "id"), out, sizeof(out)) == 0 &&
                      rt_strcmp(out, "https://example.test/a?x=1&y=2") == 0, "URL escapes decoded");
    failures += check(tool_json_string(&json, tool_json_get(&json, object, "name"), out, sizeof(out)) == 0 &&
                      rt_strcmp(out, "Gr\xc3\xbcne \xf0\x9f\x8c\xb3") == 0, "UTF-16 surrogate pair decoded");
    failures += check(tool_json_uint(&json, tool_json_get(&json, 0, "n"), &number) == 0 &&
                      number == ~0ULL, "unsigned boundary accepted");
    failures += check(tool_json_uint(&json, tool_json_get(&json, 0, "overflow"), &number) != 0,
                      "integer overflow rejected");
    failures += check(tool_json_uint(&json, tool_json_get(&json, 0, "negative"), &number) != 0 &&
                      tool_json_uint(&json, tool_json_get(&json, 0, "fraction"), &number) != 0,
                      "non-unsigned values rejected");
    failures += check(tool_json_string(&json, tool_json_get(&json, 0, "nul"), out, sizeof(out)) != 0,
                      "embedded NUL cannot enter a C URL or filename");
    failures += check(tool_json_string(&json, tool_json_get(&json, object, "id"), out, 3) != 0,
                      "small output buffer rejected");
    failures += check(tool_json_true(&json, tool_json_get(&json, 0, "yes")) &&
                      !tool_json_true(&json, tool_json_get(&json, object, "deleted")) &&
                      tool_json_get(&json, 0, "absent") == TOOL_JSON_NONE, "member lookup and booleans");
    tool_json_free(&json);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        int result = tool_json_parse(&json, invalid[i], rt_strlen(invalid[i]));
        failures += check(result != 0, invalid[i]);
        if (result == 0) tool_json_free(&json);
    }
    {
        ToolByteBuffer buffer;
        tool_byte_buffer_init(&buffer);
        tool_byte_buffer_append_cstr(&buffer, "[");
        for (i = 0; i < 1000; i++)
            tool_byte_buffer_append_cstr(&buffer, i == 0 ? "{\"x\":[1,2]}" : ",{\"x\":[1,2]}");
        tool_byte_buffer_append_cstr(&buffer, "]");
        if (tool_json_parse(&json, (const char *)buffer.data, buffer.size) != 0) {
            failures += check(0, "token storage growth");
        } else {
            failures += check(tool_json_token(&json, 0)->next == json.tokens.count, "subtree token boundaries");
            tool_json_free(&json);
        }
        tool_byte_buffer_free(&buffer);
    }
    if (failures == 0) rt_write_line(1, "JSON tests passed");
    return failures != 0;
}

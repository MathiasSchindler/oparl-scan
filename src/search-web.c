#include "search.h"
#include "search-mcp.h"
#include "runtime.h"
#include "platform.h"

#if defined(_WIN32)
int search_serve(const SearchIndex *index, unsigned int port) {
    (void)index; (void)port;
    return search_error("web mode requires macOS or Linux; pinned newos has no Windows TCP listener", 0);
}
#else
#include "search-web-ui.h"
#include "pdf-text.h"
#if defined(__APPLE__)
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#else
#include "common.h"
#endif

#define WEB_CLIENTS 8U
#define WEB_HEADER_LIMIT 65536U
#define WEB_BODY_LIMIT 65536U
#define WEB_REQUEST_LIMIT (WEB_HEADER_LIMIT + WEB_BODY_LIMIT)
#define WEB_REQUEST_NS 5000000000ULL

typedef struct {
    char *path, *parameters, *host, *origin, *site;
    char *content_type, *accept, *protocol, *method, *name, *body;
    size_t body_length;
    int head, post, has_length;
} WebRequest;

typedef struct {
    int fd;
    size_t used, header_bytes;
    unsigned long long deadline;
    WebRequest request;
    char data[WEB_REQUEST_LIMIT + 1];
} WebClient;

static int configure_client(int fd) {
#if defined(__APPLE__)
    int enabled = 1;
    struct timeval timeout;
    timeout.tv_sec = 5; timeout.tv_usec = 0;
    /* The pinned macOS backend cannot ignore signals; use its setsockopt syscall. */
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0 ||
           setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ? -1 : 0;
#else
    const int socket_level = 1, send_timeout = 21, tcp_level = 6, tcp_nodelay = 1, enabled = 1;
    struct { long seconds, microseconds; } timeout;
    timeout.seconds = 5; timeout.microseconds = 0;
    return linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, tcp_level, tcp_nodelay,
                         (long)&enabled, sizeof(enabled)) < 0 ||
           linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, socket_level, send_timeout,
                         (long)&timeout, sizeof(timeout)) < 0 ? -1 : 0;
#endif
}

static char *find_char(char *text, char ch) {
    while (*text && *text != ch) text++;
    return *text ? text : 0;
}

static const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static int headers_allow(int fd, int status, const char *type, long long length, const char *allow) {
    if (rt_write_cstr(fd, "HTTP/1.1 ") != 0 || rt_write_uint(fd, (unsigned int)status) != 0 ||
        rt_write_char(fd, ' ') != 0 || rt_write_cstr(fd, status_text(status)) != 0 ||
        rt_write_cstr(fd, "\r\nContent-Type: ") != 0 || rt_write_cstr(fd, type) != 0 ||
        rt_write_cstr(fd, "\r\nConnection: close\r\nCache-Control: no-store\r\n"
            "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
            "Referrer-Policy: no-referrer\r\nCross-Origin-Resource-Policy: same-origin\r\n"
            "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
            "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'\r\n") != 0)
        return -1;
    if (status == 405 && (rt_write_cstr(fd, "Allow: ") != 0 || rt_write_cstr(fd, allow) != 0 ||
        rt_write_cstr(fd, "\r\n") != 0)) return -1;
    if (length >= 0 && (rt_write_cstr(fd, "Content-Length: ") != 0 ||
        rt_write_uint(fd, (unsigned long long)length) != 0 || rt_write_cstr(fd, "\r\n") != 0)) return -1;
    return rt_write_cstr(fd, "\r\n");
}

static int headers(int fd, int status, const char *type, long long length) {
    return headers_allow(fd, status, type, length, "GET, HEAD");
}

static int web_error_allow(int fd, int status, int head, const char *detail, const char *allow) {
    search_error(status_text(status), detail);
    if (headers_allow(fd, status, "application/json; charset=utf-8", -1, allow) != 0) return -1;
    if (head) return 0;
    return rt_write_cstr(fd, "{\"error\":") != 0 || tool_json_write_string(fd, detail) != 0 ||
           rt_write_cstr(fd, "}\n") != 0 ? -1 : 0;
}

static int web_error(int fd, int status, int head, const char *detail) {
    return web_error_allow(fd, status, head, detail, "GET, HEAD");
}

static int token_is(const char *text, const char *end, const char *expected) {
    while (text < end && (*text == ' ' || *text == '\t')) text++;
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) end--;
    while (text < end && *expected) {
        unsigned char ch = (unsigned char)*text++;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != (unsigned char)*expected++) return 0;
    }
    return text == end && !*expected;
}

static int media_type(const char *text, const char *expected) {
    const char *end;
    if (!text) return 0;
    for (end = text; *end && *end != ';'; end++) {}
    return token_is(text, end, expected);
}

static int accepts_json(const char *text) {
    while (text && *text) {
        const char *end = text, *type_end, *p;
        int accepted, quality = 1;
        while (*end && *end != ',') end++;
        for (type_end = text; type_end < end && *type_end != ';'; type_end++) {}
        accepted = token_is(text, type_end, "application/json") ||
                   token_is(text, type_end, "application/*") || token_is(text, type_end, "*/*");
        p = type_end;
        while (p < end) {
            const char *name = ++p, *equal, *stop;
            while (p < end && *p != ';') p++;
            stop = p;
            for (equal = name; equal < stop && *equal != '='; equal++) {}
            if (equal < stop && token_is(name, equal, "q")) {
                const char *v = equal + 1;
                int nonzero = 0, valid = 1;
                while (v < stop && (*v == ' ' || *v == '\t')) v++;
                while (stop > v && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
                if (v == stop || (*v != '0' && *v != '1')) valid = 0;
                else {
                    char whole = *v++;
                    nonzero = whole == '1';
                    if (v < stop && *v++ != '.') valid = 0;
                    while (v < stop) {
                        if (*v < '0' || *v > '9' || (whole == '1' && *v != '0')) valid = 0;
                        if (*v != '0') nonzero = 1;
                        v++;
                    }
                }
                quality = valid && nonzero;
            }
        }
        if (accepted && quality) return 1;
        text = *end ? end + 1 : end;
    }
    return 0;
}

static char *next_line(char **cursor) {
    char *start = *cursor, *p = start;
    while (*p && *p != '\r') {
        if (*p == '\n') return 0;
        p++;
    }
    if (*p != '\r' || p[1] != '\n') return 0;
    *p = '\0'; *cursor = p + 2;
    return start;
}

static int local_authority(const char *text, unsigned int port) {
    char expected[40];
    size_t i;
    const char *hosts[] = {"127.0.0.1", "localhost"};
    for (i = 0; i < 2; i++) {
        size_t used = tool_buffer_append_cstr(expected, sizeof(expected), 0, hosts[i]);
        if (port == 80 && tool_str_equal_ignore_case_ascii(text, expected)) return 1;
        used = tool_buffer_append_char(expected, sizeof(expected), used, ':');
        tool_buffer_append_uint(expected, sizeof(expected), used, port);
        if (tool_str_equal_ignore_case_ascii(text, expected)) return 1;
    }
    return 0;
}

/* newos' static httpd parser discards query strings and caps targets at 1 KiB. */
static int parse_request(char *data, WebRequest *request, unsigned int port, const char **error) {
    char *cursor = data, *line = next_line(&cursor), *target, *version;
    int get;
    rt_memset(request, 0, sizeof(*request));
    *error = "malformed HTTP request";
    if (!line || !(target = find_char(line, ' '))) return 400;
    *target++ = '\0';
    if (!(version = find_char(target, ' '))) return 400;
    *version++ = '\0';
    if (rt_strcmp(version, "HTTP/1.1") && rt_strcmp(version, "HTTP/1.0")) return 400;
    request->head = rt_strcmp(line, "HEAD") == 0;
    request->post = rt_strcmp(line, "POST") == 0;
    get = rt_strcmp(line, "GET") == 0;
    if (*target != '/') return 400;
    for (line = target; *line; line++)
        if ((unsigned char)*line <= 32 || (unsigned char)*line >= 127 || *line == '#' || *line == '\\') return 400;
    request->path = target;
    request->parameters = find_char(target, '?');
    if (request->parameters) *request->parameters++ = '\0';
    if (!get && !request->head && !(request->post && !rt_strcmp(request->path, "/mcp"))) {
        *error = "use GET/HEAD, or POST on /mcp"; return 405;
    }
    while ((line = next_line(&cursor)) != 0 && *line) {
        char *colon = find_char(line, ':'), *value, *end, *p;
        if (!colon || colon == line) return 400;
        for (p = line; p < colon; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || find_char("!#$%&'*+-.^_`|~", *p))) return 400;
        *colon = '\0'; value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        end = value + rt_strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        for (p = value; *p; p++) if (((unsigned char)*p < 32 && *p != '\t') || *p == 127) return 400;
        if (tool_str_equal_ignore_case_ascii(line, "Host")) {
            if (request->host) return 400;
            request->host = value;
        } else if (tool_str_equal_ignore_case_ascii(line, "Origin")) {
            if (request->origin) return 400;
            request->origin = value;
        } else if (tool_str_equal_ignore_case_ascii(line, "Sec-Fetch-Site")) {
            if (request->site) return 400;
            request->site = value;
        } else if (tool_str_equal_ignore_case_ascii(line, "Content-Length")) {
            unsigned long long length;
            if (request->has_length || !*value) return 400;
            for (p = value; *p; p++) if (*p < '0' || *p > '9') return 400;
            if (rt_parse_uint(value, &length) != 0) return 400;
            if (length > WEB_BODY_LIMIT) { *error = "MCP body exceeds 64 KiB"; return 413; }
            request->has_length = 1; request->body_length = (size_t)length;
        } else if (tool_str_equal_ignore_case_ascii(line, "Transfer-Encoding") ||
                   tool_str_equal_ignore_case_ascii(line, "Expect")) {
            *error = "Transfer-Encoding and Expect are not supported"; return 400;
        } else {
            char **field = 0;
            if (tool_str_equal_ignore_case_ascii(line, "Content-Type")) field = &request->content_type;
            else if (tool_str_equal_ignore_case_ascii(line, "Accept")) field = &request->accept;
            else if (tool_str_equal_ignore_case_ascii(line, "MCP-Protocol-Version")) field = &request->protocol;
            else if (tool_str_equal_ignore_case_ascii(line, "Mcp-Method")) field = &request->method;
            else if (tool_str_equal_ignore_case_ascii(line, "Mcp-Name")) field = &request->name;
            if (field) { if (*field) return 400; *field = value; }
            if (field == &request->protocol && rt_strlen(value) >= 64) return 400;
        }
    }
    if (!line || *cursor || !request->host) return 400;
    if (!local_authority(request->host, port)) { *error = "Host must name this local service"; return 403; }
    if ((!rt_strncmp(request->path, "/api/", 5) || !rt_strcmp(request->path, "/mcp")) &&
        ((request->origin && (rt_strncmp(request->origin, "http://", 7) ||
          !tool_str_equal_ignore_case_ascii(request->origin + 7, request->host))) ||
         (request->site && rt_strcmp(request->site, "same-origin") && rt_strcmp(request->site, "none")))) {
        *error = "cross-origin API requests are not allowed"; return 403;
    }
    if (request->post) {
        if (request->parameters || !request->has_length) {
            *error = "MCP requires Content-Length and no URL parameters"; return 400;
        }
        if (!media_type(request->content_type, "application/json")) {
            *error = "MCP requires application/json"; return 415;
        }
        if (!accepts_json(request->accept)) {
            *error = "MCP response requires Accept: application/json, text/event-stream"; return 406;
        }
    } else if (request->has_length) {
        *error = "request bodies are only supported on POST /mcp"; return 400;
    }
    return 0;
}

static int decode(char *text) {
    size_t read = 0, written = 0;
    while (text[read]) {
        unsigned int ch = (unsigned char)text[read++];
        if (ch == '%') {
            int a, b;
            if (!text[read] || !text[read + 1]) return -1;
            a = tool_hex_value(text[read]); b = tool_hex_value(text[read + 1]);
            if (a < 0 || b < 0) return -1;
            ch = (unsigned int)(a * 16 + b); read += 2;
        } else if (ch == '+') ch = ' ';
        if (ch < 32 || ch == 127) return -1;
        text[written++] = (char)ch;
    }
    text[written] = '\0';
    return rt_utf8_validate(text, written);
}

static const char *const parameter_names[] = {"q", "year", "paperType", "organization", "kind", "limit",
    "offset", "sort", "id", "page", "group", "dateRole", "dateFrom", "dateThrough",
    "organizationId", "actorId", "snapshotId", "fields", "raw", "ids"};
static int parameters(char *text, char **values, int object) {
    rt_memset(values, 0, 20 * sizeof(*values));
    while (text && *text) {
        char *next = find_char(text, '&'), *value;
        size_t i;
        if (next) { *next++ = '\0'; if (!*next) return -1; }
        value = find_char(text, '=');
        if (!value) return -1;
        *value++ = '\0';
        if (decode(text) != 0 || decode(value) != 0) return -1;
        for (i = 0; i < 20 && rt_strcmp(text, parameter_names[i]); i++) {}
        if (i == 20 || values[i] ||
            (object == 1 && i != 8 && i != 9) ||
            (object == 2 && i != 4 && i != 5 && i != 6 && i != 8 && i != 9 && i < 16) ||
            (!object && (i == 8 || i == 9 || i >= 17))) return -1;
        values[i] = value; text = next;
    }
    return 0;
}

static int number(const char *text, unsigned int min, unsigned int max, unsigned int *out) {
    unsigned long long value;
    if (rt_parse_uint(text, &value) != 0 || value < min || value > max) return -1;
    *out = (unsigned int)value;
    return 0;
}

static int research_response(int fd, const WebRequest *request, const SearchIndex *index, const char *operation) {
    char *values[20], storage[16385];
    ToolByteBuffer arguments, result;
    SearchRead read;
    Json json;
    const char *error = "invalid research parameters";
    size_t i;
    int status = 0, comma = 0, parsed = 0;
    if (parameters(request->parameters, values, 2)) return web_error(fd, 400, request->head, error);
    tool_byte_buffer_init(&arguments); tool_byte_buffer_init(&result);
    if (tool_byte_buffer_append_cstr(&arguments, "{")) { status = SEARCH_QUERY_RESOURCE; goto done; }
    for (i = 0; i < 20; i++) if (values[i]) {
        if ((comma && tool_byte_buffer_append_cstr(&arguments, ",")) ||
            search_json_quote(&arguments, parameter_names[i]) || tool_byte_buffer_append_cstr(&arguments, ":")) {
            status = SEARCH_QUERY_RESOURCE; goto done;
        }
        if (i == 5 || i == 6 || i == 9) {
            unsigned int value;
            char text[32];
            if (number(values[i], i == 6 ? 0 : 1, i == 5 ? 100 : i == 6 ? UINT32_MAX : 1000000, &value)) {
                status = SEARCH_QUERY_INVALID; goto done;
            }
            rt_unsigned_to_string(value, text, sizeof(text));
            if (tool_byte_buffer_append_cstr(&arguments, text)) { status = SEARCH_QUERY_RESOURCE; goto done; }
        } else if (i == 18 || i == 19) {
            Json value;
            if (json_parse(&value, values[i], rt_strlen(values[i]))) { status = SEARCH_QUERY_INVALID; goto done; }
            if (json_token(&value, 0)->kind != (i == 18 ? JSON_BOOL : JSON_ARRAY)) status = SEARCH_QUERY_INVALID;
            json_free(&value);
            if (status) goto done;
            if (tool_byte_buffer_append_cstr(&arguments, values[i])) { status = SEARCH_QUERY_RESOURCE; goto done; }
        } else if (search_json_quote(&arguments, values[i])) { status = SEARCH_QUERY_RESOURCE; goto done; }
        comma = 1;
    }
    if (tool_byte_buffer_append_cstr(&arguments, "}")) { status = SEARCH_QUERY_RESOURCE; goto done; }
    if (json_parse(&json, (const char *)arguments.data, arguments.size)) { status = SEARCH_QUERY_INVALID; goto done; }
    parsed = 1;
    status = search_read_arguments(&json, 0, &read, storage, sizeof(storage), &error);
    if (!status) status = search_read_json(&result, index, operation, &read, &error);
done:
    if (parsed) json_free(&json);
    if (status) status = web_error(fd, status == SEARCH_READ_NOT_FOUND ? 404 : status == SEARCH_QUERY_SNAPSHOT ? 409 :
        status == SEARCH_QUERY_RESOURCE ? 500 : 400, request->head, error);
    else {
        status = headers(fd, 200, "application/json; charset=utf-8", (long long)result.size);
        if (!status && !request->head) status = rt_write_all(fd, result.data, result.size);
    }
    tool_byte_buffer_free(&arguments); tool_byte_buffer_free(&result); return status;
}

static int handle(int fd, const WebRequest *parsed, const SearchIndex *index, SearchWorkspace *workspace) {
    WebRequest request = *parsed;
    SearchFilter filter;
    SearchResult result;
    const char *error, *type = 0;
    const char *const *asset = 0;
    char *values[20];
    int status;
    if (!rt_strcmp(request.path, "/mcp")) {
        ToolByteBuffer reply;
        int http_status;
        if (!request.post) return web_error_allow(fd, 405, request.head, "MCP supports POST only", "POST");
        tool_byte_buffer_init(&reply);
        status = search_mcp(index, workspace, request.body, request.body_length, request.protocol,
                            request.method, request.name, &reply, &http_status);
        if (status) status = web_error(fd, 500, 0, "cannot build MCP response");
        else {
            status = headers(fd, http_status, "application/json; charset=utf-8", (long long)reply.size);
            if (!status && reply.size) status = rt_write_all(fd, reply.data, reply.size);
        }
        tool_byte_buffer_free(&reply);
        return status;
    }
    if (!rt_strcmp(request.path, "/api/paper")) return research_response(fd, &request, index, "get_paper");
    if (!rt_strcmp(request.path, "/api/object")) return research_response(fd, &request, index, "get_object");
    if (!rt_strcmp(request.path, "/api/objects")) return research_response(fd, &request, index, "get_objects");
    if (!rt_strcmp(request.path, "/api/meeting")) return research_response(fd, &request, index, "get_meeting");
    if (!rt_strcmp(request.path, "/api/agenda")) return research_response(fd, &request, index, "get_agenda");
    if (!rt_strcmp(request.path, "/api/options")) return research_response(fd, &request, index, "get_search_options");
    if (!rt_strcmp(request.path, "/paper") || !rt_strcmp(request.path, "/meeting")) {
        const SearchRecord *paper;
        if (parameters(request.parameters, values, 1) != 0 || !values[8] || !*values[8] || values[9])
            return web_error(fd, 400, request.head, "expected only a Paper id");
        paper = search_find(index, values[8], 0);
        if (!paper || rt_strcmp(index->strings + paper->kind, !rt_strcmp(request.path, "/meeting") ? "Meeting" : "Paper"))
            return web_error(fd, 404, request.head, "Paper/Meeting is not in this index");
        asset = search_case_page; type = "text/html; charset=utf-8";
    }
    if (!rt_strcmp(request.path, "/")) { asset = search_web_page; type = "text/html; charset=utf-8"; }
    else if (!rt_strcmp(request.path, "/health")) { asset = search_health_page; type = "text/html; charset=utf-8"; }
    else if (!rt_strcmp(request.path, "/app.js")) { asset = search_web_script; type = "text/javascript; charset=utf-8"; }
    if (asset) {
        size_t i, length = 0;
        for (i = 0; asset[i]; i++) length += rt_strlen(asset[i]);
        if (headers(fd, 200, type, (long long)length) != 0) return -1;
        if (!request.head)
            for (i = 0; asset[i]; i++) if (rt_write_all(fd, asset[i], rt_strlen(asset[i])) != 0) return -1;
        return 0;
    }
    if (!rt_strcmp(request.path, "/favicon.ico")) return headers(fd, 204, "image/x-icon", 0);
    if (!rt_strcmp(request.path, "/api/status")) {
        ToolByteBuffer json;
        if (request.parameters && *request.parameters)
            return web_error(fd, 400, request.head, "status takes no parameters");
        tool_byte_buffer_init(&json);
        if (search_status_json(&json, index) != 0) status = web_error(fd, 500, request.head, "cannot serialize index status");
        else {
            status = headers(fd, 200, "application/json; charset=utf-8", (long long)json.size);
            if (!status && !request.head) status = rt_write_all(fd, json.data, json.size);
        }
        tool_byte_buffer_free(&json);
        return status;
    }
    if (rt_strcmp(request.path, "/api/search"))
        return web_error(fd, 404, request.head, "route not found");
    rt_memset(&filter, 0, sizeof(filter)); filter.limit = 20;
    if (parameters(request.parameters, values, 0) != 0 ||
        (values[1] && number(values[1], 2000, 2099, &filter.year) != 0) ||
        (values[5] && number(values[5], 1, 100, &filter.limit) != 0) ||
        (values[6] && number(values[6], 0, UINT32_MAX, &filter.offset) != 0) ||
        (values[2] && !*values[2]) || (values[3] && !*values[3]) || (values[4] && !*values[4]))
        return web_error(fd, 400, request.head,
                         "expected q and valid year, paperType, organization, kind, limit, offset, sort");
    if (values[7]) {
        if (!rt_strcmp(values[7], "relevance")) filter.sort = SEARCH_RELEVANCE;
        else if (!rt_strcmp(values[7], "date-desc")) filter.sort = SEARCH_DATE_DESC;
        else if (!rt_strcmp(values[7], "date-asc")) filter.sort = SEARCH_DATE_ASC;
        else return web_error(fd, 400, request.head, "sort must be relevance, date-desc or date-asc");
    }
    filter.paper_type = values[2]; filter.organization = values[3]; filter.kind = values[4];
    filter.date_from = values[12]; filter.date_through = values[13];
    filter.organization_id = values[14]; filter.actor_id = values[15]; filter.snapshot_id = values[16];
    if (values[11] && search_date_role(values[11], &filter.date_role))
        return web_error(fd, 400, request.head, "unknown temporal dateRole; reference year uses year");
    if (values[10]) {
        if (!rt_strcmp(values[10], "paper")) filter.group = SEARCH_PAPERS;
        else if (!rt_strcmp(values[10], "record")) filter.group = SEARCH_RECORDS;
        else if (!rt_strcmp(values[10], "document")) filter.group = SEARCH_DOCUMENTS;
        else return web_error(fd, 400, request.head, "group must be record, paper or document");
    }
    status = search_query(index, workspace, values[0], &filter, &result, &error);
    if (status) return web_error(fd, status == SEARCH_QUERY_SNAPSHOT ? 409 :
        status == SEARCH_QUERY_RESOURCE ? 500 : 400, request.head, error);
    if (headers(fd, 200, "application/json; charset=utf-8", -1) != 0) return -1;
    return request.head ? 0 : search_print(fd, index, &result, 1);
}

static void close_client(WebClient *client) {
    if (platform_close(client->fd) != 0) search_error("cannot close HTTP connection", 0);
    client->fd = -1; client->used = 0;
}

int search_serve(const SearchIndex *index, unsigned int port) {
    WebClient *clients;
    SearchWorkspace workspace;
    int listener = -1;
    size_t i;
    rt_memset(&workspace, 0, sizeof(workspace));
#if !defined(__APPLE__)
    int signal_number;
    if (platform_parse_signal_name("PIPE", &signal_number) != 0 || platform_ignore_signal(signal_number) != 0)
        return search_error("cannot disable SIGPIPE for web mode", 0);
#endif
    if (platform_open_tcp_listener("127.0.0.1", port, &listener) != 0)
        return search_error("cannot listen on 127.0.0.1; check whether the port is already in use", 0);
    clients = (WebClient *)rt_malloc_array(WEB_CLIENTS, sizeof(*clients));
    if (!clients) { platform_close(listener); return search_error("cannot allocate HTTP connections", 0); }
    rt_memset(clients, 0, WEB_CLIENTS * sizeof(*clients));
    for (i = 0; i < WEB_CLIENTS; i++) clients[i].fd = -1;
    if (rt_write_cstr(2, "oparl-search: listening on http://127.0.0.1:") != 0 ||
        rt_write_uint(2, port) != 0 || rt_write_cstr(2, "/ (Ctrl+C to stop)\n") != 0) goto done;
    for (;;) {
        PlatformPollFd fds[WEB_CLIENTS + 1];
        unsigned long long now;
        rt_memset(fds, 0, sizeof(fds));
        fds[0].fd = listener; fds[0].events = PLATFORM_POLL_READ;
        for (i = 0; i < WEB_CLIENTS; i++) { fds[i+1].fd = clients[i].fd; fds[i+1].events = PLATFORM_POLL_READ; }
        if (platform_poll(fds, WEB_CLIENTS + 1, 250) < 0 || (fds[0].revents & PLATFORM_POLL_ERROR)) {
            search_error("HTTP listener poll failed", 0); break;
        }
        now = platform_get_monotonic_time_ns();
        if (fds[0].revents & PLATFORM_POLL_READ) {
            int fd;
            if (platform_accept_tcp(listener, &fd) != 0) { search_error("HTTP accept failed", 0); break; }
            if (configure_client(fd) != 0) {
                if (platform_close(fd) != 0) search_error("cannot close HTTP connection", 0);
                search_error("cannot configure HTTP socket timeout/SIGPIPE handling", 0); break;
            }
            for (i = 0; i < WEB_CLIENTS && clients[i].fd >= 0; i++) {}
            if (i == WEB_CLIENTS) {
                if (web_error(fd, 503, 0, "HTTP connection limit reached") != 0)
                    search_error("cannot write HTTP error response", 0);
                if (platform_close(fd) != 0) search_error("cannot close HTTP connection", 0);
            } else {
                clients[i].fd = fd; clients[i].used = clients[i].header_bytes = 0;
                clients[i].deadline = now + WEB_REQUEST_NS;
            }
        }
        for (i = 0; i < WEB_CLIENTS; i++) {
            WebClient *client = &clients[i];
            size_t end, j;
            long got;
            int reject = 0;
            if (client->fd < 0) continue;
            if (now >= client->deadline) {
                if (client->used && web_error(client->fd, 408, 0, "request deadline exceeded") != 0)
                    search_error("cannot write HTTP timeout response", 0);
                close_client(client); continue;
            }
            if (!(fds[i+1].revents & (PLATFORM_POLL_READ | PLATFORM_POLL_ERROR))) continue;
            got = platform_read(client->fd, client->data + client->used, WEB_REQUEST_LIMIT - client->used);
            if (got <= 0) {
                if (got < 0 || client->used) search_error("incomplete HTTP request or client disconnected", 0);
                close_client(client); continue;
            }
            for (j = client->used; j < client->used + (size_t)got; j++)
                if (client->data[j] == '\0') reject = 1;
            client->used += (size_t)got; client->data[client->used] = '\0';
            if (reject) {
                if (web_error(client->fd, 400, 0, "NUL in HTTP request") != 0)
                    search_error("cannot write HTTP error response", 0);
            } else {
                if (!client->header_bytes) {
                    if (tool_find_http_header_end(client->data, client->used, &end) != 0) {
                        if (client->used < WEB_HEADER_LIMIT) continue;
                        if (web_error(client->fd, 431, 0, "request headers exceed 64 KiB") != 0)
                            search_error("cannot write HTTP error response", 0);
                        close_client(client); continue;
                    }
                    if (end > WEB_HEADER_LIMIT) {
                        if (web_error(client->fd, 431, 0, "request headers exceed 64 KiB") != 0)
                            search_error("cannot write HTTP error response", 0);
                        close_client(client); continue;
                    }
                    {
                        char saved = client->data[end];
                        const char *error;
                        int status;
                        client->data[end] = '\0';
                        status = parse_request(client->data, &client->request, port, &error);
                        client->data[end] = saved;
                        if (status) {
                            const char *allow = client->request.path && !rt_strcmp(client->request.path, "/mcp") ?
                                                "POST" : "GET, HEAD";
                            if (web_error_allow(client->fd, status, client->request.head, error, allow) != 0)
                                search_error("cannot write HTTP error response", 0);
                            close_client(client); continue;
                        }
                    }
                    client->header_bytes = end; client->request.body = client->data + end;
                }
                end = client->header_bytes + client->request.body_length;
                if (client->used < end) continue;
                if (client->used > end) {
                    if (web_error(client->fd, 400, client->request.head, "unexpected body bytes or HTTP pipelining") != 0)
                        search_error("cannot write HTTP error response", 0);
                } else if (handle(client->fd, &client->request, index, &workspace) != 0)
                    search_error("cannot write HTTP response; client may have disconnected", 0);
            }
            close_client(client);
        }
    }
done:
    for (i = 0; i < WEB_CLIENTS; i++) if (clients[i].fd >= 0) close_client(&clients[i]);
    if (platform_close(listener) != 0) search_error("cannot close HTTP listener", 0);
    search_workspace_free(&workspace); rt_free(clients);
    return -1;
}
#endif

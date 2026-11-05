#include "search.h"
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
#define WEB_REQUEST_LIMIT 65536U
#define WEB_REQUEST_NS 5000000000ULL

typedef struct {
    int fd;
    size_t used;
    unsigned long long deadline;
    char data[WEB_REQUEST_LIMIT + 1];
} WebClient;

typedef struct {
    char *path, *parameters, *host, *origin, *site;
    int head;
} WebRequest;

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
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static int headers(int fd, int status, const char *type, long long length) {
    if (rt_write_cstr(fd, "HTTP/1.1 ") != 0 || rt_write_uint(fd, (unsigned int)status) != 0 ||
        rt_write_char(fd, ' ') != 0 || rt_write_cstr(fd, status_text(status)) != 0 ||
        rt_write_cstr(fd, "\r\nContent-Type: ") != 0 || rt_write_cstr(fd, type) != 0 ||
        rt_write_cstr(fd, "\r\nConnection: close\r\nCache-Control: no-store\r\n"
            "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
            "Referrer-Policy: no-referrer\r\nCross-Origin-Resource-Policy: same-origin\r\n"
            "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
            "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'\r\n") != 0)
        return -1;
    if (status == 405 && rt_write_cstr(fd, "Allow: GET, HEAD\r\n") != 0) return -1;
    if (length >= 0 && (rt_write_cstr(fd, "Content-Length: ") != 0 ||
        rt_write_uint(fd, (unsigned long long)length) != 0 || rt_write_cstr(fd, "\r\n") != 0)) return -1;
    return rt_write_cstr(fd, "\r\n");
}

static int web_error(int fd, int status, int head, const char *detail) {
    search_error(status_text(status), detail);
    if (headers(fd, status, "application/json; charset=utf-8", -1) != 0) return -1;
    if (head) return 0;
    return rt_write_cstr(fd, "{\"error\":") != 0 || tool_json_write_string(fd, detail) != 0 ||
           rt_write_cstr(fd, "}\n") != 0 ? -1 : 0;
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
    rt_memset(request, 0, sizeof(*request));
    *error = "malformed HTTP request";
    if (!line || !(target = find_char(line, ' '))) return 400;
    *target++ = '\0';
    if (!(version = find_char(target, ' '))) return 400;
    *version++ = '\0';
    if (rt_strcmp(version, "HTTP/1.1") && rt_strcmp(version, "HTTP/1.0")) return 400;
    request->head = rt_strcmp(line, "HEAD") == 0;
    if (rt_strcmp(line, "GET") && !request->head) { *error = "only GET and HEAD are supported"; return 405; }
    if (*target != '/') return 400;
    for (line = target; *line; line++)
        if ((unsigned char)*line <= 32 || (unsigned char)*line >= 127 || *line == '#' || *line == '\\') return 400;
    request->path = target;
    request->parameters = find_char(target, '?');
    if (request->parameters) *request->parameters++ = '\0';
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
        } else if (tool_str_equal_ignore_case_ascii(line, "Content-Length") ||
                   tool_str_equal_ignore_case_ascii(line, "Transfer-Encoding") ||
                   tool_str_equal_ignore_case_ascii(line, "Expect")) {
            *error = "request bodies and pipelining are not supported"; return 400;
        }
    }
    if (!line || *cursor || !request->host) return 400;
    if (!local_authority(request->host, port)) { *error = "Host must name this local service"; return 403; }
    if (rt_strncmp(request->path, "/api/", 5) == 0 &&
        ((request->origin && (rt_strncmp(request->origin, "http://", 7) ||
          !tool_str_equal_ignore_case_ascii(request->origin + 7, request->host))) ||
         (request->site && rt_strcmp(request->site, "same-origin") && rt_strcmp(request->site, "none")))) {
        *error = "cross-origin API requests are not allowed"; return 403;
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

static int parameters(char *text, char **values, int object) {
    static const char *names[] = {"q", "year", "paperType", "organization", "kind", "limit",
                                 "offset", "sort", "id", "page"};
    rt_memset(values, 0, 10 * sizeof(*values));
    while (text && *text) {
        char *next = find_char(text, '&'), *value;
        size_t i;
        if (next) { *next++ = '\0'; if (!*next) return -1; }
        value = find_char(text, '=');
        if (!value) return -1;
        *value++ = '\0';
        if (decode(text) != 0 || decode(value) != 0) return -1;
        for (i = 0; i < 10 && rt_strcmp(text, names[i]); i++) {}
        if (i == 10 || values[i] || (object ? i < 8 : i >= 8)) return -1;
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

static int handle(int fd, char *data, const SearchIndex *index, SearchWorkspace *workspace, unsigned int port) {
    WebRequest request;
    SearchFilter filter;
    SearchResult result;
    const char *error, *type = 0;
    const char *const *asset = 0;
    char *values[10];
    int status = parse_request(data, &request, port, &error);
    if (status) return web_error(fd, status, request.head, error);
    if (!rt_strcmp(request.path, "/paper") || !rt_strcmp(request.path, "/api/paper")) {
        const SearchRecord *paper;
        if (parameters(request.parameters, values, 1) != 0 || !values[8] || !*values[8] || values[9])
            return web_error(fd, 400, request.head, "expected only a Paper id");
        paper = search_find(index, values[8], 0);
        if (!paper || rt_strcmp(index->strings + paper->kind, "Paper"))
            return web_error(fd, 404, request.head, "Paper is not in this index");
        if (!rt_strcmp(request.path, "/api/paper")) {
            SearchCase context;
            if (search_case_open(&context, index, paper, &error) != 0)
                return web_error(fd, 500, request.head, error);
            status = headers(fd, 200, "application/json; charset=utf-8", -1);
            if (!status && !request.head) status = search_case_print(fd, index, &context);
            search_case_close(&context);
            return status;
        }
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
        size_t i, pages = 0, warning_pages = 0, ocr_pages = 0;
        if (request.parameters && *request.parameters)
            return web_error(fd, 400, request.head, "status takes no parameters");
        if (headers(fd, 200, "application/json; charset=utf-8", -1) != 0) return -1;
        if (request.head) return 0;
        for (i = 0; i < index->record_count; i++) {
            const SearchRecord *r = &index->records[i];
            if (!r->page) continue;
            pages++;
            if (r->warnings) warning_pages++;
            if (r->warnings & PDF_TEXT_NEEDS_OCR) ocr_pages++;
        }
        return rt_write_cstr(fd, "{\"records\":") != 0 || rt_write_uint(fd, index->record_count) != 0 ||
            tool_json_field_uint(fd, "terms", index->term_count) != 0 ||
            tool_json_field_uint(fd, "indexBytes", index->file_bytes) != 0 ||
            tool_json_field_uint(fd, "indexMemoryBytes", index->memory_bytes) != 0 ||
            tool_json_field_uint(fd, "pages", pages) != 0 ||
            tool_json_field_uint(fd, "warningPages", warning_pages) != 0 ||
            tool_json_field_uint(fd, "ocrPages", ocr_pages) != 0 ||
            tool_json_field_bool(fd, "incomplete", index->incomplete != 0) != 0 ||
            rt_write_cstr(fd, "}\n") != 0 ? -1 : 0;
    }
    if (!rt_strcmp(request.path, "/api/object")) {
        unsigned int page = 0;
        const SearchRecord *record;
        if (parameters(request.parameters, values, 1) != 0 || !values[8] || !*values[8] ||
            (values[9] && number(values[9], 1, 1000000, &page) != 0))
            return web_error(fd, 400, request.head, "expected id and optional page (1..1000000)");
        record = search_find(index, values[8], page);
        if (!record) return web_error(fd, 404, request.head, "resource/page is not in this index");
        if (headers(fd, 200, "application/json; charset=utf-8", -1) != 0) return -1;
        return request.head ? 0 : search_show(fd, index, record, 1);
    }
    if (rt_strcmp(request.path, "/api/search"))
        return web_error(fd, 404, request.head, "route not found");
    rt_memset(&filter, 0, sizeof(filter)); filter.limit = 20;
    if (parameters(request.parameters, values, 0) != 0 || !values[0] || !*values[0] ||
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
    status = search_query(index, workspace, values[0], &filter, &result, &error);
    if (status) return web_error(fd, status == SEARCH_QUERY_RESOURCE ? 500 : 400, request.head, error);
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
                clients[i].fd = fd; clients[i].used = 0; clients[i].deadline = now + WEB_REQUEST_NS;
            }
        }
        for (i = 0; i < WEB_CLIENTS; i++) {
            WebClient *client = &clients[i];
            size_t end, j;
            long got;
            int reject = 0;
            if (client->fd < 0) continue;
            if (now >= client->deadline) {
                if (client->used && web_error(client->fd, 408, 0, "request header deadline exceeded") != 0)
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
            } else if (tool_find_http_header_end(client->data, client->used, &end) == 0) {
                if (end != client->used) {
                    if (web_error(client->fd, 400, 0, "request bodies and pipelining are not supported") != 0)
                        search_error("cannot write HTTP error response", 0);
                } else if (handle(client->fd, client->data, index, &workspace, port) != 0)
                    search_error("cannot write HTTP response; client may have disconnected", 0);
            } else if (client->used == WEB_REQUEST_LIMIT) {
                if (web_error(client->fd, 431, 0, "request headers exceed 64 KiB") != 0)
                    search_error("cannot write HTTP error response", 0);
            } else continue;
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

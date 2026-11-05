#include "http.h"
#include "runtime.h"
#include "tool_util.h"
#include "tls/tls12_client.h"
#include "tls/tls13_client.h"
#include "crypto/x509.h"

#define HEADER_LIMIT 65536U
#define RESULT_RETRY (-2)
#define RESULT_REDIRECT 1

typedef struct {
    char host[256];
    char origin[320];
    char path[RIS_URL_CAPACITY];
    unsigned int port;
    int tls;
} Url;

typedef struct {
    int fd;
    Tls13Client *tls;
    Tls12Client *tls12;
    unsigned int timeout_ms;
    unsigned char buffer[16384];
    size_t position;
    size_t length;
} Reader;

int ris_error(const char *message, const char *detail) {
    rt_write_cstr(2, "oparl-scan: ");
    rt_write_cstr(2, message);
    if (detail != 0) {
        rt_write_cstr(2, ": ");
        rt_write_cstr(2, detail);
    }
    rt_write_char(2, '\n');
    return -1;
}

static int copy_n(char *out, size_t capacity, const char *text, size_t length) {
    if (length >= capacity) return -1;
    memcpy(out, text, length);
    out[length] = '\0';
    return 0;
}

static int url_parse(const HttpClient *client, const char *text, Url *url) {
    size_t start, end, host_end, i;
    unsigned long long port;
    url->tls = rt_strncmp(text, "https://", 8) == 0;
    if (!url->tls && rt_strncmp(text, "http://", 7) != 0) return -1;
    start = url->tls ? 8U : 7U;
    for (i = 0; text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch <= 32U || ch >= 127U || ch == '\\' || ch == '#' ||
            i >= RIS_URL_CAPACITY - 1) return -1;
    }
    end = start;
    while (text[end] != '\0' && text[end] != '/' && text[end] != '?') end++;
    host_end = start;
    while (host_end < end && text[host_end] != ':') host_end++;
    if (host_end == start ||
        copy_n(url->host, sizeof(url->host), text + start, host_end - start) != 0 ||
        copy_n(url->origin, sizeof(url->origin), text, end) != 0) return -1;
    url->port = url->tls ? 443U : 80U;
    if (host_end < end) {
        char number[8];
        if (copy_n(number, sizeof(number), text + host_end + 1, end - host_end - 1) != 0 ||
            rt_parse_uint(number, &port) != 0 || port == 0 || port > 65535) return -1;
        url->port = (unsigned int)port;
    }
    if (text[end] == '?') return -1;
    if (copy_n(url->path, sizeof(url->path),
               text[end] == '\0' ? "/" : text + end,
               text[end] == '\0' ? 1 : rt_strlen(text + end)) != 0) return -1;
    if (client->allow_loopback && rt_strcmp(url->host, "127.0.0.1") == 0) return 0;
    if (!url->tls || url->port != 443U) return -1;
    if (rt_strcmp(url->host, "www.potsdam.sitzung-online.de") == 0)
        return rt_strncmp(url->path, "/oparl/", 7) == 0 ? 0 : -1;
    if (rt_strcmp(url->host, "egov.potsdam.de") == 0)
        return rt_strncmp(url->path, "/public/", 8) == 0 ? 0 : -1;
    return -1;
}

int http_valid_url(const HttpClient *client, const char *url) {
    Url parsed;
    return url_parse(client, url, &parsed) == 0;
}

static int trust_load(HttpClient *client) {
    static const char *paths[] = {
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt"
    };
    const char *path = client->ca_file;
    size_t i;
    if (client->trust != 0) return 0;
    if (path == 0) path = platform_getenv("SSL_CERT_FILE");
    if (path == 0) {
        for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            if (platform_path_access(paths[i], PLATFORM_ACCESS_READ) == 0) {
                path = paths[i];
                break;
            }
        }
    }
    if (path == 0) return ris_error("no CA bundle found; use --ca-file", 0);
    if (tool_read_all_input(path, &client->trust, &client->trust_length) != 0 ||
        client->trust_length == 0 || client->trust_length > 4U * 1024U * 1024U)
        return ris_error("cannot read CA bundle", path);
    return 0;
}

static int connection_open(HttpClient *client, const Url *url, Reader *reader) {
    rt_memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
    reader->timeout_ms = client->timeout_ms;
    if (url->tls && trust_load(client) != 0) return -1;
    if (platform_connect_tcp(url->host, url->port, &reader->fd) != 0) {
        ris_error("TCP connection failed", url->host);
        return RESULT_RETRY;
    }
    if (url->tls) {
        CryptoX509DerCert certs[TLS13_MAX_PEER_CERTS];
        size_t count, i;
        char status[256];
        int legacy = 0;
        for (i = 0; i < client->tls12_count; i++)
            if (rt_strcmp(client->tls12_hosts[i], url->host) == 0) legacy = 1;
        if (!legacy) {
            reader->tls = (Tls13Client *)rt_malloc(sizeof(*reader->tls));
            if (reader->tls == 0) return ris_error("cannot allocate TLS state", 0);
            tls13_client_init(reader->tls, reader->fd, client->timeout_ms);
            if (tls13_client_handshake(reader->tls, url->host, rt_strlen(url->host)) != 0) {
                rt_write_cstr(2, "oparl-scan: trying TLS 1.2 after TLS 1.3 failed: ");
                rt_write_line(2, tls13_client_last_error(reader->tls));
                rt_free(reader->tls);
                reader->tls = 0;
                platform_close(reader->fd);
                reader->fd = -1;
                if (platform_connect_tcp(url->host, url->port, &reader->fd) != 0) {
                    ris_error("TCP reconnect for TLS 1.2 failed", url->host);
                    return RESULT_RETRY;
                }
                legacy = 1;
            }
        }
        if (legacy) {
            reader->tls12 = (Tls12Client *)rt_malloc(sizeof(*reader->tls12));
            if (reader->tls12 == 0) return ris_error("cannot allocate TLS 1.2 state", 0);
            tls12_client_init(reader->tls12, reader->fd, client->timeout_ms);
            if (tls12_client_handshake(reader->tls12, url->host, rt_strlen(url->host)) != 0) {
                ris_error("TLS 1.2 handshake failed", tls12_client_last_error(reader->tls12));
                return RESULT_RETRY;
            }
            count = tls12_client_peer_certificates(reader->tls12, certs, TLS13_MAX_PEER_CERTS);
        } else {
            count = tls13_client_peer_certificates(reader->tls, certs, TLS13_MAX_PEER_CERTS);
        }
        status[0] = '\0';
        if (crypto_x509_verify_chain(certs, count, url->host, platform_get_epoch_time(),
                                    client->trust, client->trust_length,
                                    status, sizeof(status)) != 0)
            return ris_error("TLS certificate verification failed", status);
        if (legacy && client->tls12_count < 2) {
            for (i = 0; i < client->tls12_count; i++)
                if (rt_strcmp(client->tls12_hosts[i], url->host) == 0) break;
            if (i == client->tls12_count)
                rt_copy_string(client->tls12_hosts[client->tls12_count++], 256, url->host);
        }
    }
    return 0;
}

static void connection_close(Reader *reader) {
    rt_free(reader->tls);
    rt_free(reader->tls12);
    if (reader->fd >= 0) platform_close(reader->fd);
}

static long connection_write(Reader *reader, const void *bytes, size_t size) {
    if (reader->tls12 != 0)
        return tls12_client_write_app(reader->tls12, (const unsigned char *)bytes, size);
    if (reader->tls != 0)
        return tls13_client_write_app(reader->tls, (const unsigned char *)bytes, size);
    return rt_write_all(reader->fd, bytes, size) == 0 ? (long)size : -1;
}

static int reader_fill(Reader *reader) {
    long count;
    if (reader->tls12 != 0) {
        count = tls12_client_read_app(reader->tls12, reader->buffer, sizeof(reader->buffer));
    } else if (reader->tls != 0) {
        count = tls13_client_read_app(reader->tls, reader->buffer, sizeof(reader->buffer));
    } else {
        size_t ready;
        int result = platform_poll_fds(&reader->fd, 1, &ready, (int)reader->timeout_ms);
        if (result <= 0) return -1;
        count = platform_read(reader->fd, reader->buffer, sizeof(reader->buffer));
    }
    if (count < 0) return -1;
    reader->position = 0;
    reader->length = (size_t)count;
    return count == 0 ? 0 : 1;
}

static int reader_byte(Reader *reader, unsigned char *out) {
    if (reader->position == reader->length && reader_fill(reader) <= 0) return -1;
    *out = reader->buffer[reader->position++];
    return 0;
}

static int reader_line(Reader *reader, char *line, size_t capacity) {
    size_t used = 0;
    unsigned char ch, next;
    while (used + 1 < capacity) {
        if (reader_byte(reader, &ch) != 0) return -1;
        if (ch == '\r') {
            if (reader_byte(reader, &next) != 0 || next != '\n') return -1;
            line[used] = '\0';
            return 0;
        }
        if (ch < 32U && ch != '\t') return -1;
        line[used++] = (char)ch;
    }
    return -1;
}

static int write_body(Reader *reader, int fd, unsigned long long count,
                      unsigned long long limit, unsigned long long *total) {
    while (count != 0) {
        size_t take;
        if (reader->position == reader->length && reader_fill(reader) <= 0) return -1;
        take = reader->length - reader->position;
        if ((unsigned long long)take > count) take = (size_t)count;
        if ((unsigned long long)take > limit - *total ||
            rt_write_all(fd, reader->buffer + reader->position, take) != 0) return -1;
        reader->position += take;
        *total += take;
        count -= take;
    }
    return 0;
}

static int body_receive(Reader *reader, int fd, int chunked, unsigned long long length,
                        unsigned long long limit, unsigned long long *total) {
    char line[8192];
    *total = 0;
    if (chunked) {
        for (;;) {
            unsigned long long count = 0;
            size_t i = 0;
            unsigned char cr, lf;
            if (reader_line(reader, line, sizeof(line)) != 0) return -1;
            while (line[i] != '\0' && line[i] != ';') {
                int digit = tool_hex_value(line[i++]);
                if (digit < 0 || count > (RIS_UNKNOWN_SIZE - (unsigned int)digit) / 16U)
                    return -1;
                count = count * 16U + (unsigned int)digit;
            }
            if (i == 0) return -1;
            if (count == 0) {
                size_t trailer_length = 0;
                do {
                    if (reader_line(reader, line, sizeof(line)) != 0) return -1;
                    trailer_length += rt_strlen(line) + 2;
                    if (trailer_length > HEADER_LIMIT) return -1;
                } while (line[0] != '\0');
                return 0;
            }
            if (write_body(reader, fd, count, limit, total) != 0 ||
                reader_byte(reader, &cr) != 0 || reader_byte(reader, &lf) != 0 ||
                cr != '\r' || lf != '\n') return -1;
        }
    }
    if (length != RIS_UNKNOWN_SIZE) return write_body(reader, fd, length, limit, total);
    for (;;) {
        size_t remaining = reader->length - reader->position;
        int result;
        if (remaining != 0 && write_body(reader, fd, remaining, limit, total) != 0) return -1;
        result = reader_fill(reader);
        if (result <= 0) return result;
    }
}

static int redirect_url(const Url *base, const char *location, char *out, size_t capacity) {
    ToolByteBuffer text;
    int result = -1;
    tool_byte_buffer_init(&text);
    if (rt_strncmp(location, "https://", 8) == 0 ||
        rt_strncmp(location, "http://", 7) == 0) {
        result = copy_n(out, capacity, location, rt_strlen(location));
    } else if (location[0] == '/' && location[1] != '/') {
        if (tool_byte_buffer_append_cstr(&text, base->origin) == 0 &&
            tool_byte_buffer_append_cstr(&text, location) == 0 &&
            tool_byte_buffer_terminate(&text) == 0)
            result = copy_n(out, capacity, (const char *)text.data, text.size);
    }
    tool_byte_buffer_free(&text);
    return result;
}

static int fetch_once(HttpClient *client, const char *source, const char *path,
                      unsigned long long limit, unsigned long long expected_size,
                      int accept_size_mismatch,
                      unsigned long long *size_out, char *redirect,
                      unsigned int *retry_after) {
    Url url;
    Reader *reader;
    ToolByteBuffer request;
    char line[8192], location[RIS_URL_CAPACITY];
    unsigned long long length = RIS_UNKNOWN_SIZE, total = 0;
    size_t header_length = 0;
    int chunked = 0, status, result = -1, fd = -1, opened = 0;
    location[0] = '\0';
    if (url_parse(client, source, &url) != 0) return ris_error("URL is not allowed", source);
    if (platform_sleep_milliseconds(client->delay_ms) != 0)
        return ris_error("request delay failed", 0);
    reader = (Reader *)rt_malloc(sizeof(*reader));
    if (reader == 0) return ris_error("cannot allocate HTTP buffer", 0);
    tool_byte_buffer_init(&request);
    result = connection_open(client, &url, reader);
    if (result != 0) goto done;
    result = RESULT_RETRY;
    if (tool_byte_buffer_append_cstr(&request, "GET ") != 0 ||
        tool_byte_buffer_append_cstr(&request, url.path) != 0 ||
        tool_byte_buffer_append_cstr(&request, " HTTP/1.1\r\nHost: ") != 0 ||
        tool_byte_buffer_append_cstr(&request, url.origin + (url.tls ? 8 : 7)) != 0 ||
        tool_byte_buffer_append_cstr(&request,
            "\r\nUser-Agent: OParl-Scan/1.0\r\nAccept: */*\r\n"
            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n") != 0 ||
        connection_write(reader, request.data, request.size) != (long)request.size ||
        reader_line(reader, line, sizeof(line)) != 0) {
        ris_error("cannot send request or read HTTP status", source);
        goto done;
    }
    status = tool_parse_http_status(line);
    if ((rt_strncmp(line, "HTTP/1.1 ", 9) != 0 && rt_strncmp(line, "HTTP/1.0 ", 9) != 0) ||
        status < 100 || status > 599) {
        result = ris_error("invalid HTTP status line", source);
        goto done;
    }
    for (;;) {
        size_t colon = 0, end;
        char *value;
        if (reader_line(reader, line, sizeof(line)) != 0) {
            ris_error("incomplete HTTP headers", source);
            goto done;
        }
        header_length += rt_strlen(line) + 2;
        if (header_length > HEADER_LIMIT) {
            result = ris_error("HTTP headers exceed limit", source);
            goto done;
        }
        if (line[0] == '\0') break;
        while (line[colon] != '\0' && line[colon] != ':') colon++;
        if (line[colon] != ':') {
            result = ris_error("invalid HTTP header", source);
            goto done;
        }
        value = line + colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        end = rt_strlen(value);
        while (end != 0 && (value[end - 1] == ' ' || value[end - 1] == '\t')) value[--end] = '\0';
        if (tool_name_equals_ignore_case_ascii_n(line, colon, "Content-Length")) {
            unsigned long long parsed;
            if (rt_parse_uint(value, &parsed) != 0 || parsed == RIS_UNKNOWN_SIZE ||
                (length != RIS_UNKNOWN_SIZE && parsed != length)) {
                result = ris_error("invalid Content-Length", source);
                goto done;
            }
            length = parsed;
        } else if (tool_name_equals_ignore_case_ascii_n(line, colon, "Transfer-Encoding")) {
            if (!tool_name_equals_ignore_case_ascii_n(value, rt_strlen(value), "chunked")) {
                result = ris_error("unsupported Transfer-Encoding", value);
                goto done;
            }
            chunked = 1;
        } else if (tool_name_equals_ignore_case_ascii_n(line, colon, "Content-Encoding")) {
            if (!tool_name_equals_ignore_case_ascii_n(value, rt_strlen(value), "identity")) {
                result = ris_error("server ignored Accept-Encoding: identity", value);
                goto done;
            }
        } else if (tool_name_equals_ignore_case_ascii_n(line, colon, "Location")) {
            if (copy_n(location, sizeof(location), value, rt_strlen(value)) != 0) {
                result = ris_error("redirect URL is too long", source);
                goto done;
            }
        } else if (tool_name_equals_ignore_case_ascii_n(line, colon, "Retry-After")) {
            unsigned long long seconds;
            if (rt_parse_uint(value, &seconds) != 0 || seconds > 300U) {
                result = ris_error("unsupported Retry-After; retry later manually", value);
                goto done;
            }
            *retry_after = (unsigned int)seconds * 1000U;
        }
    }
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        if (rt_strcmp(location, "./noauth") == 0) {
            ris_error("access denied (redirect to ./noauth)", source);
            result = RIS_HTTP_ACCESS_DENIED;
            goto done;
        }
        if (redirect_url(&url, location, redirect, RIS_URL_CAPACITY) != 0 ||
            !http_valid_url(client, redirect)) {
            result = ris_error("redirect is not allowed", location);
            goto done;
        }
        result = RESULT_REDIRECT;
        goto done;
    }
    if (status != 200) {
        rt_write_cstr(2, "oparl-scan: HTTP ");
        rt_write_uint(2, (unsigned long long)status);
        rt_write_cstr(2, ": ");
        rt_write_line(2, source);
        result = status == 401 || status == 403 ? RIS_HTTP_ACCESS_DENIED :
                 status == 408 || status == 429 || status >= 500 ? RESULT_RETRY : -1;
        goto done;
    }
    if ((chunked && length != RIS_UNKNOWN_SIZE) ||
        (!chunked && length != RIS_UNKNOWN_SIZE && length > limit)) {
        result = ris_error("ambiguous or oversized HTTP response", source);
        goto done;
    }
    fd = platform_open_write(path, 0644);
    if (fd < 0) {
        result = ris_error("cannot create download", path);
        goto done;
    }
    opened = 1;
    if (body_receive(reader, fd, chunked, length, limit, &total) != 0) {
        ris_error("incomplete response, timeout, size limit, or write failure", source);
        goto done;
    }
    if (expected_size != RIS_UNKNOWN_SIZE && total != expected_size) {
        int accepted = accept_size_mismatch && (chunked || length != RIS_UNKNOWN_SIZE);
        ris_error(accepted ? "accepting document size mismatch" :
                  "document size differs from OParl metadata", source);
        rt_write_cstr(2, "oparl-scan: expected bytes=");
        rt_write_uint(2, expected_size);
        rt_write_cstr(2, ", received bytes=");
        rt_write_uint(2, total);
        rt_write_char(2, '\n');
        if (!accepted) {
            result = -1;
            goto done;
        }
    }
    if (platform_close(fd) != 0) {
        fd = -1;
        result = ris_error("cannot close download", path);
        goto done;
    }
    fd = -1;
    *size_out = total;
    result = 0;
done:
    if (fd >= 0) platform_close(fd);
    if (opened && result != 0 && platform_remove_file(path) != 0)
        ris_error("cannot remove incomplete download", path);
    connection_close(reader);
    tool_byte_buffer_free(&request);
    rt_free(reader);
    return result;
}

int http_download(HttpClient *client, const char *url, const char *path,
                  unsigned long long limit, unsigned long long expected_size,
                  int accept_size_mismatch,
                  unsigned long long *size_out) {
    unsigned int attempt;
    for (attempt = 0; attempt < 3; attempt++) {
        char current[RIS_URL_CAPACITY], redirect[RIS_URL_CAPACITY];
        unsigned int hops, retry_after = 0;
        int result = -1;
        rt_copy_string(current, sizeof(current), url);
        for (hops = 0; hops < 6; hops++) {
            result = fetch_once(client, current, path, limit, expected_size,
                                accept_size_mismatch, size_out, redirect, &retry_after);
            if (result != RESULT_REDIRECT) break;
            rt_copy_string(current, sizeof(current), redirect);
        }
        if (result == 0) return 0;
        if (result == RESULT_REDIRECT) return ris_error("too many HTTP redirects", url);
        if (result != RESULT_RETRY) return result;
        if (attempt == 2) return -1;
        if (retry_after < (1000U << attempt)) retry_after = 1000U << attempt;
        rt_write_cstr(2, "oparl-scan: retrying: ");
        rt_write_line(2, url);
        if (platform_sleep_milliseconds(retry_after) != 0)
            return ris_error("retry delay failed", 0);
    }
    return -1;
}

void http_free(HttpClient *client) {
    rt_free(client->trust);
    client->trust = 0;
    client->trust_length = 0;
}

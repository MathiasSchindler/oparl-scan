#ifndef RIS_HTTP_H
#define RIS_HTTP_H

#include "platform.h"

#define RIS_URL_CAPACITY 4096U
#define RIS_UNKNOWN_SIZE (~0ULL)
#define RIS_HTTP_ACCESS_DENIED (-3)

typedef struct {
    const char *ca_file;
    unsigned char *trust;
    size_t trust_length;
    unsigned int timeout_ms;
    unsigned int delay_ms;
    int allow_loopback;
    char tls12_hosts[2][256];
    size_t tls12_count;
} HttpClient;

int ris_error(const char *message, const char *detail);
int http_valid_url(const HttpClient *client, const char *url);
int http_download(HttpClient *client, const char *url, const char *path,
                  unsigned long long limit, unsigned long long expected_size,
                  int accept_size_mismatch,
                  unsigned long long *size_out);
void http_free(HttpClient *client);

#endif

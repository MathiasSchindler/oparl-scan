#ifndef RIS_HTTP_H
#define RIS_HTTP_H

#include "platform.h"

#define RIS_URL_CAPACITY 4096U
#define RIS_UNKNOWN_SIZE (~0ULL)
#define RIS_HTTP_ACCESS_DENIED (-3)
#define RIS_HTTP_NOT_FOUND (-4)
#define RIS_HTTP_NOT_MODIFIED (-5)

typedef struct {
    char url[RIS_URL_CAPACITY];
    char etag[512];
    char modified[128];
} HttpValidators;

typedef struct {
    const char *ca_file;
    unsigned char *trust;
    size_t trust_length;
    unsigned int timeout_ms;
    unsigned int delay_ms;
    int allow_loopback;
    unsigned int workers;
    struct RtTaskPool *pool;
    struct HttpHost *hosts;
    PlatformMutex state_lock;
} HttpClient;

typedef struct {
    const char *url;
    const char *path;
    unsigned long long limit;
    unsigned long long expected_size;
    int accept_size_mismatch;
    unsigned long long received;
    int result;
    const HttpValidators *condition;
    unsigned long long cached_size;
    unsigned long long local_modified;
    int probe;
    int framed;
    HttpValidators validators;
} HttpTransfer;

int ris_error(const char *message, const char *detail);
void ris_warning(const char *message, const char *detail);
int http_valid_url(const HttpClient *client, const char *url);
int http_same_host(const HttpClient *client, const char *a, const char *b);
int http_valid_validators(const HttpClient *client, const HttpValidators *validators);
int http_download_batch(HttpClient *client, HttpTransfer *transfers, size_t count);
int http_download(HttpClient *client, HttpTransfer *transfer);
void http_free(HttpClient *client);

#endif

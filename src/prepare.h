#ifndef OPARL_PREPARE_H
#define OPARL_PREPARE_H
#include "tool_json_parser.h"
#include "pdf-text.h"

typedef struct {
    ToolJson json;
    unsigned char *bytes;
    char key[65], template_hash[65];
    int layout;
} PrepareConfig;
typedef struct {
    ToolJson json;
    unsigned char *bytes;
    size_t data;
} PreparedDocument;
int prepare_error(const char *message, const char *path);
int prepare_config_open(PrepareConfig *config, const char *path, int layout);
void prepare_config_close(PrepareConfig *config);
void prepare_hash(const unsigned char *bytes, size_t length, char out[65]);
int prepare_document(const PrepareConfig *config, const char *root, const unsigned char *bytes, size_t length,
                     int force, int *reused, int *incomplete);
int prepare_manifest_write(const PrepareConfig *config, const char *root);
int prepare_manifest_read(const char *root, char key[65]);
int prepared_open(PreparedDocument *doc, const char *root, const char *key, const char *source_hash,
                  unsigned long long bytes);
void prepared_close(PreparedDocument *doc);
int prepared_summary(ToolByteBuffer *out, const PreparedDocument *doc);
#endif

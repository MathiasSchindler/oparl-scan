#ifndef EG_INDEX_IO_H
#define EG_INDEX_IO_H

#include "eg2.h"
#include "crypto/sha256.h"

typedef struct {
    char name[PLATFORM_NAME_CAPACITY];
    unsigned char sha[32];
} EgIndexFile;

int eg_index_files(const char *directory, const char *suffix, ToolArray *files);
int eg_absolute_path(const char *input, int allow_missing, char *out, size_t capacity);
int eg_read_regular(const char *path, size_t limit, unsigned char **bytes, size_t *size);
int eg_ensure_directory(const char *path);
int eg_atomic_write(const char *path, const void *data, size_t size);
void eg_hex_digest(const unsigned char digest[32], char text[65]);
int eg_parse_digest(const char *text, unsigned char digest[32]);

#endif

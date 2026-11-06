#ifndef OPARL_ARCHIVE_VERSION_H
#define OPARL_ARCHIVE_VERSION_H

#include "platform.h"

int archive_content_hash(const char *path, char digest[65]);
int archive_preserve(const char *root, const char *kind, const char *key,
                     const char *source, const char *digest, char *relative, size_t capacity);
int archive_timestamp(unsigned long long seconds, char *out, size_t capacity);
int archive_parse_timestamp(const char *text, unsigned long long *seconds);
int archive_valid_date(const char *date);

#endif

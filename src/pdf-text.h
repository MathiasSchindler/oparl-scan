#ifndef OPARL_PDF_TEXT_H
#define OPARL_PDF_TEXT_H

#include <stddef.h>

#define PDF_TEXT_UNKNOWN_GLYPH 1U
#define PDF_TEXT_UNSUPPORTED_STREAM 2U
#define PDF_TEXT_NEEDS_OCR 4U

typedef int (*PdfTextPage)(unsigned int page, const char *text, size_t length,
                           unsigned int warnings, void *user);
int pdf_text_extract(const unsigned char *bytes, size_t length, PdfTextPage callback,
                     void *user, const char **error);

#endif

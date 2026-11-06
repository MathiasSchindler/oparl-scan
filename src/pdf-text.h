#ifndef OPARL_PDF_TEXT_H
#define OPARL_PDF_TEXT_H

#include <stddef.h>

#define PDF_TEXT_UNKNOWN_GLYPH 1U
#define PDF_TEXT_UNSUPPORTED_STREAM 2U
#define PDF_TEXT_NEEDS_OCR 4U

typedef int (*PdfTextPage)(unsigned int page, const char *text, size_t length,
                           unsigned int warnings, void *user);
typedef struct {
    size_t start, length;
    double x, y, end_x, end_y, size;
} PdfTextSpan;
typedef struct {
    const char *text;
    size_t length;
    const PdfTextSpan *spans;
    size_t span_count;
} PdfTextLayout;
typedef int (*PdfTextLayoutPage)(unsigned int page, const char *text, size_t length,
    unsigned int warnings, const PdfTextLayout *layout, void *user);
int pdf_text_extract(const unsigned char *bytes, size_t length, PdfTextPage callback,
                     void *user, const char **error);
int pdf_text_extract_layout(const unsigned char *bytes, size_t length, PdfTextLayoutPage callback,
                           void *user, const char **error);

#endif

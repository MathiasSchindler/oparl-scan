#ifndef OPARL_PDF_STRUCTURE_H
#define OPARL_PDF_STRUCTURE_H
#include "json.h"
#include "pdf-text.h"

typedef struct {
    int agenda, section;
    double number_x, text_x, reference_x;
    char last_number[32];
} PdfStructureState;

typedef struct {
    size_t tables, agenda_items, uncertain_rows;
    int keep_layout, skipped;
} PdfStructureResult;

int pdf_structure_page(ToolByteBuffer *out, const PdfTextLayout *layout, unsigned int warnings,
                       PdfStructureState *state, PdfStructureResult *result);
int pdf_structure_validate_page(const Json *json, size_t page, unsigned long long counts[5]);
#endif

#ifndef EG_SECTIONS_H
#define EG_SECTIONS_H
#include "eg2.h"

#define EG_SECTION_RECIPE "eg2-main-form-section-v1\n" EG2_WEIGHTS_SHA256 "\n" EG2_TOKENIZER_SHA256 "\n" EG2_DOCUMENT_PREFIX
#define EG_SECTION_LIMIT 800U
#define EG_SECTION_OVERLAP 80U

typedef struct {
    size_t start, end;
    unsigned int tokens;
    unsigned char key[32];
} EgSection;

int eg_form_number(const char *text, unsigned int *page, unsigned int *total);
int eg_form_reference(const char *text, const char *reference);
int eg_sections(const EgTokenizer *tokenizer, const char *text, ToolArray *sections);
int eg_section_tokens(const EgTokenizer *tokenizer, const char *text, size_t size,
                      unsigned int *tokens, unsigned int *count);
void eg_section_identity(const char *paper_id, const char pdf_sha[65], const char text_sha[65],
                         size_t start, size_t end, char identity[65]);
#endif

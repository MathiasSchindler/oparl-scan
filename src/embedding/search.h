#ifndef EG_SEARCH_H
#define EG_SEARCH_H

#include "eg2.h"

#define EG_SEARCH_RESULTS 25U
#define EG_SEARCH_BYTES 4096U
#define EG_SEARCH_TOKENS 256U

typedef struct {
    char *id, *title, *reference;
    unsigned int year;
    float vector[EG2_DIM];
} EgTitle;

typedef struct {
    EgTitle *titles;
    size_t count;
    unsigned long long years[100];
    int year_present[100], complete;
} EgTitleIndex;

typedef struct {
    size_t index;
    double score;
} EgSearchHit;

int eg_title_index_load(EgTitleIndex *index, const char *directory);
void eg_title_index_free(EgTitleIndex *index);
int eg_title_rank(const EgTitleIndex *index, const float query[EG2_DIM], size_t offset,
                  EgSearchHit hits[EG_SEARCH_RESULTS], unsigned int *count);

typedef enum { EG_SEARCH_TITLE, EG_SEARCH_CONTENT, EG_SEARCH_BOTH } EgSearchMode;

typedef struct {
    char *id, *title, *reference;
    char pdf_sha[65], text_sha[65];
    size_t first, count, pages, title_index;
    unsigned int year;
    int separate_reasoning;
} EgContentDocument;

typedef struct {
    char *id, *text;
    size_t document, start, end, page_first, page_last;
    float vector[EG2_DIM];
} EgPassage;

typedef struct {
    EgContentDocument *documents;
    EgPassage *passages;
    size_t document_count, count, included, excluded, combined_count;
    unsigned int year;
    int year_present[100];
} EgSectionIndex;

typedef struct {
    const char *id, *title, *reference;
    unsigned int year;
    double score;
    const EgPassage *passage;
    int separate_reasoning;
} EgResult;

int eg_section_index_load(EgSectionIndex *index, const char *directory, const EgTitleIndex *titles);
int eg_section_indexes_load(EgSectionIndex *index, const char *const *directories, size_t count,
                            const EgTitleIndex *titles);
void eg_section_index_free(EgSectionIndex *index);
size_t eg_search_count(const EgTitleIndex *titles, const EgSectionIndex *sections, EgSearchMode mode);
int eg_search_rank(const EgTitleIndex *titles, const EgSectionIndex *sections, EgSearchMode mode,
                   const float query[EG2_DIM], size_t offset,
                   EgResult hits[EG_SEARCH_RESULTS], unsigned int *count);

#endif

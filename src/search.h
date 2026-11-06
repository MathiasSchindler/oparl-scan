#ifndef OPARL_SEARCH_H
#define OPARL_SEARCH_H

#include "tool_util.h"
#include <stdint.h>

typedef struct {
    uint32_t id, name, reference, date, paper_type, organization, kind, source;
    uint32_t text, text_length, body_start, words, year, page, warnings, payload;
    uint32_t parent, reserved[3];
} SearchRecord;

typedef struct { uint32_t term, record, position, byte; } SearchOccurrence;
typedef struct { uint32_t text, start, count, documents; float idf; uint32_t reserved; } SearchTerm;
typedef struct { uint32_t record, paper; } SearchOwner;
typedef struct { uint32_t start, count, paper; } SearchLinks;
typedef struct {
    ToolByteBuffer strings;
    ToolArray records, terms, occurrences, owners;
    uint32_t *table;
    size_t table_capacity;
    int failed;
    unsigned int incomplete;
} SearchBuilder;

typedef struct {
    void *allocation;
    SearchLinks *links;
    uint32_t *related_papers;
    const char *strings;
    const SearchRecord *records;
    const SearchTerm *terms;
    const SearchOccurrence *occurrences;
    const SearchOwner *owners;
    size_t string_bytes, record_count, term_count, occurrence_count, owner_count, file_bytes, memory_bytes;
    unsigned int incomplete;
    float average_length;
} SearchIndex;

/* Zero-initialize one workspace per concurrent caller; the index stays immutable. */
typedef struct {
    void *parser, *allocation;
    size_t record_count, atom_capacity, hit_capacity, group_capacity;
} SearchWorkspace;

typedef enum { SEARCH_RELEVANCE, SEARCH_DATE_DESC, SEARCH_DATE_ASC } SearchSort;
typedef enum { SEARCH_RECORDS, SEARCH_PAPERS } SearchGroup;
typedef struct {
    unsigned int year, limit, offset;
    SearchSort sort;
    SearchGroup group;
    const char *paper_type, *organization, *kind;
} SearchFilter;

typedef struct { uint32_t record, byte, paper, matches; float score; } SearchHit;
typedef struct {
    SearchHit hits[100];
    unsigned int count;
    size_t total;
    SearchGroup group;
} SearchResult;

typedef struct { ToolArray objects; int incomplete; } SearchCase;

enum { SEARCH_QUERY_INVALID = -1, SEARCH_QUERY_RESOURCE = -2 };

void search_builder_init(SearchBuilder *builder);
void search_builder_free(SearchBuilder *builder);
uint32_t search_string(SearchBuilder *builder, const char *text, size_t length);
const char *search_builder_string(const SearchBuilder *builder, uint32_t offset);
uint32_t search_add_record(SearchBuilder *builder, const SearchRecord *record);
int search_add_owner(SearchBuilder *builder, uint32_t record, uint32_t paper);
int search_write(SearchBuilder *builder, const char *path);
int search_open(SearchIndex *index, const char *path);
void search_close(SearchIndex *index);
void search_workspace_free(SearchWorkspace *workspace);
int search_query(const SearchIndex *index, SearchWorkspace *workspace, const char *query, const SearchFilter *filter,
                  SearchResult *result, const char **error);
int search_print(int fd, const SearchIndex *index, const SearchResult *result, int json);
const SearchRecord *search_find(const SearchIndex *index, const char *id, unsigned int page);
int search_show(int fd, const SearchIndex *index, const SearchRecord *record, int json);
int search_case_open(SearchCase *context, const SearchIndex *index, const SearchRecord *paper, const char **error);
void search_case_close(SearchCase *context);
int search_case_print(int fd, const SearchIndex *index, const SearchCase *context);
int search_results_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchResult *result);
int search_record_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchRecord *record);
int search_case_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchCase *context);
int search_status_json(ToolByteBuffer *buffer, const SearchIndex *index);
int search_json_quote(ToolByteBuffer *buffer, const char *text);
int search_serve(const SearchIndex *index, unsigned int port);
int search_error(const char *message, const char *detail);

#endif

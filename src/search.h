#ifndef OPARL_SEARCH_H
#define OPARL_SEARCH_H

#include "tool_util.h"
#include "json.h"
#include <stdint.h>

typedef struct {
    uint32_t id, name, reference, date, paper_type, organization, kind, source;
    uint32_t text, text_length, body_start, words, year, page, warnings, payload;
    uint32_t parent, identity, system, body, metadata_origin, reserved;
} SearchRecord;

typedef enum {
    SEARCH_MAIN_FILE = 1, SEARCH_PAPER_ATTACHMENT, SEARCH_RESOLUTION_FILE,
    SEARCH_AGENDA_ATTACHMENT, SEARCH_MEETING_ATTACHMENT, SEARCH_INVITATION,
    SEARCH_RESULTS_PROTOCOL, SEARCH_VERBATIM_PROTOCOL, SEARCH_CONSULTATION,
    SEARCH_CONSULTATION_PAPER, SEARCH_AGENDA_ITEM, SEARCH_MEETING,
    SEARCH_AGENDA_CONSULTATION, SEARCH_MEETING_AGENDA, SEARCH_SUPERORDINATED,
    SEARCH_SUBORDINATED, SEARCH_RELATED, SEARCH_ORGANIZATION, SEARCH_DIRECTION,
    SEARCH_BODY, SEARCH_SYSTEM, SEARCH_AGENDA_MEETING, SEARCH_BODY_COLLECTION,
    SEARCH_CONTEXT_SYSTEM, SEARCH_RELATION_END
} SearchRelationRole;
typedef enum {
    SEARCH_PAPER_DATE = 1, SEARCH_DOCUMENT_DATE, SEARCH_MEETING_START, SEARCH_MEETING_END,
    SEARCH_SUBMITTED, SEARCH_DECIDED, SEARCH_PUBLISHED, SEARCH_EFFECTIVE,
    SEARCH_CREATED, SEARCH_MODIFIED, SEARCH_OBSERVED, SEARCH_IMPORTED,
    SEARCH_REFERENCE_YEAR, SEARCH_DATE_END
} SearchDateRole;
enum { SEARCH_ASSERTED = 1, SEARCH_DERIVED, SEARCH_EXACT, SEARCH_CERTAINTY_END };
enum { SEARCH_AUTHORITY_UNKNOWN, SEARCH_PRELIMINARY, SEARCH_AUTHORITATIVE };

typedef struct { uint32_t source, target, role, evidence, authority, reserved; } SearchRelation;
typedef struct { uint32_t record, role, value, evidence; } SearchFact;
typedef struct { uint32_t object, version, page, section, property, rule, certainty, reserved; } SearchEvidence;
typedef struct {
    uint32_t record, id, hash, reserved;
    uint64_t bytes, observed_at, fetched_at;
} SearchVersion;
typedef struct { uint32_t record, id, start, length, evidence, reserved; } SearchSection;
typedef struct {
    int64_t seconds;
    uint32_t nanos, day, fact, precision;
} SearchDate;
typedef struct { uint32_t name, start, count, warnings, unresolved, unversioned; } SearchFacet;
typedef struct { const char *id; uint32_t record; } SearchIdentity;
typedef struct { uint32_t source, property; char *id; } SearchActor;

typedef struct { uint32_t term, record, position, byte; } SearchOccurrence;
typedef struct { uint32_t text, start, count, documents; float idf; uint32_t reserved; } SearchTerm;
typedef struct { uint32_t record, paper; } SearchOwner;
typedef struct { uint32_t start, count, paper; } SearchLinks;
typedef struct {
    ToolByteBuffer strings;
    ToolArray records, terms, occurrences, owners;
    ToolArray relations, facts, evidence, versions, sections;
    uint64_t imported_at;
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
    const SearchRelation *relations;
    const SearchFact *facts;
    const SearchEvidence *evidence;
    const SearchVersion *versions;
    const SearchSection *sections;
    size_t relation_count, fact_count, evidence_count, version_count, section_count;
    uint64_t imported_at;
    char snapshot_id[65];
    size_t string_bytes, record_count, term_count, occurrence_count, owner_count, file_bytes, memory_bytes;
    unsigned int incomplete;
    float average_length;
    SearchDate *dates;
    uint32_t *date_lookup, *meeting_context, *version_lookup, *kind_records;
    SearchFacet *kinds;
    size_t kind_count;
    SearchDate imported_date;
    char imported_text[64];
    SearchIdentity *identities;
    size_t identity_count;
    ToolArray actors;
    uint32_t *actor_start, *relation_targets;
    uint32_t *incoming_start, *incoming_relations;
    uint64_t *agenda_order;
    unsigned char *agenda_access;
} SearchIndex;

/* Zero-initialize one workspace per concurrent caller; the index stays immutable. */
typedef struct {
    void *parser, *allocation;
    size_t record_count, atom_capacity, hit_capacity, group_capacity;
} SearchWorkspace;

typedef enum { SEARCH_RELEVANCE, SEARCH_DATE_DESC, SEARCH_DATE_ASC } SearchSort;
typedef enum { SEARCH_RECORDS, SEARCH_PAPERS, SEARCH_DOCUMENTS } SearchGroup;
typedef struct {
    unsigned int year, limit, offset;
    SearchSort sort;
    SearchGroup group;
    const char *paper_type, *organization, *kind;
    const char *date_from, *date_through;
    SearchDateRole date_role;
    const char *organization_id, *actor_id, *snapshot_id;
} SearchFilter;

typedef struct {
    uint32_t record, byte, paper, matches;
    float score;
    uint32_t date_source, date_fact, date_role;
} SearchHit;
typedef struct {
    SearchHit hits[100];
    unsigned int count;
    size_t total;
    SearchGroup group;
    SearchDateRole date_role;
    SearchSort sort;
    size_t matched_records, excluded_unknown_dates, excluded_invalid_dates, excluded_unversioned;
    int filter_only;
    const char *date_from, *date_through;
    unsigned int offset, limit;
} SearchResult;

typedef struct {
    const char *id, *kind, *fields, *snapshot_id;
    const char *ids[20];
    unsigned int id_count, limit, offset, page;
    int raw;
} SearchRead;

typedef struct { ToolArray objects; int incomplete; } SearchCase;

enum { SEARCH_QUERY_INVALID = -1, SEARCH_QUERY_RESOURCE = -2, SEARCH_QUERY_SNAPSHOT = -3, SEARCH_READ_NOT_FOUND = -4 };

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
int search_model_validate(const SearchIndex *index);
int search_model_json(ToolByteBuffer *buffer, const SearchIndex *index, const SearchRecord *record);
int search_is_direct_source(const SearchIndex *index, uint32_t paper, const char *file);
const char *search_date_role_name(unsigned int role);
int search_date_role(const char *name, SearchDateRole *role);
int search_date_parse(const char *text, SearchDate *date);
int search_facets_build(SearchIndex *index);
void search_facets_free(SearchIndex *index);
int search_kind_known(const SearchIndex *index, const char *kind);
void search_hit_date(const SearchIndex *index, SearchHit *hit, SearchDateRole role);
const SearchDate *search_hit_time(const SearchIndex *index, const SearchHit *hit);
const char *search_hit_date_text(const SearchIndex *index, const SearchHit *hit);
int search_hit_date_json(ToolByteBuffer *out, const SearchIndex *index, const SearchHit *hit);
int search_json_quote(ToolByteBuffer *buffer, const char *text);
int search_serve(const SearchIndex *index, unsigned int port);
int search_error(const char *message, const char *detail);
const char *search_relation_role_name(unsigned int role);
int search_research_build(SearchIndex *index);
void search_research_free(SearchIndex *index);
const SearchRecord *search_identity_find(const SearchIndex *index, const char *id, unsigned int page);
int search_actor_matches(const SearchIndex *index, uint32_t record, const char *id, int organization);
int search_actor_known(const SearchIndex *index, const char *id, int organization);
int search_snapshot_check(const SearchIndex *index, const char *snapshot, unsigned int offset, const char **error);
int search_read_json(ToolByteBuffer *out, const SearchIndex *index, const char *operation,
                     const SearchRead *request, const char **error);
int search_hit_source_json(ToolByteBuffer *out, const SearchIndex *index, const SearchHit *hit);
int search_coverage_json(ToolByteBuffer *out, const SearchIndex *index);
int search_read_arguments(const Json *json, size_t arguments, SearchRead *request,
                          char *storage, size_t capacity, const char **error);

#endif

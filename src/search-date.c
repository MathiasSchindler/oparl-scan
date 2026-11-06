#include "search.h"
#include "runtime.h"
#include "platform.h"

#define NONE UINT32_MAX

static const char *const roles[] = {
    "auto", "paper.date", "document.date", "meeting.start", "meeting.end", "submittedAt",
    "decidedAt", "publishedAt", "effectiveAt", "created", "modified", "observedAt",
    "importedAt", "referenceYear"
};

const char *search_date_role_name(unsigned int role) {
    return role < SEARCH_DATE_END ? roles[role] : "";
}

int search_date_role(const char *name, SearchDateRole *role) {
    unsigned int i;
    for (i = 0; i < SEARCH_REFERENCE_YEAR; i++) if (!rt_strcmp(name, roles[i])) {
        *role = (SearchDateRole)i; return 0;
    }
    return -1;
}

int search_date_parse(const char *text, SearchDate *date) {
    static const unsigned int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned int y, m, d, hour, minute, second, scale = 100000000;
    size_t n = rt_strlen(text), p;
    int zone = 0;
    int64_t year, era, days;
    rt_memset(date, 0, sizeof(*date)); date->fact = NONE;
    if (n < 10 || text[4] != '-' || text[7] != '-' ||
        tool_parse_fixed_digits(text, 0, 4, &y) || tool_parse_fixed_digits(text, 5, 2, &m) ||
        tool_parse_fixed_digits(text, 8, 2, &d) ||
        !y || !m || m > 12 || !d ||
        d > lengths[m-1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return -1;
    date->day = y * 10000 + m * 100 + d;
    /* Gregorian civil date to days since 1970, independent of local timezone/DST. */
    year = (int64_t)y - (m <= 2);
    era = year / 400;
    days = era * 146097 + (year-era*400)*365 + (year-era*400)/4 - (year-era*400)/100 +
        (153 * ((int64_t)m + (m > 2 ? -3 : 9)) + 2)/5 + d - 1 - 719468;
    date->seconds = days * 86400;
    if (n == 10) { date->precision = 1; return 0; }
    if (n < 20 || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        tool_parse_fixed_digits(text, 11, 2, &hour) || tool_parse_fixed_digits(text, 14, 2, &minute) ||
        tool_parse_fixed_digits(text, 17, 2, &second) || hour > 23 || minute > 59 || second > 59) return -1;
    p = 19;
    if (text[p] == '.') {
        size_t start = ++p;
        while (p < n && text[p] >= '0' && text[p] <= '9') {
            if (!scale) return -1;
            date->nanos += (uint32_t)(text[p++] - '0') * scale; scale /= 10;
        }
        if (start == p) return -1;
    }
    if (!((text[p] == 'Z' && p + 1 == n) ||
          (p + 6 == n && (text[p] == '+' || text[p] == '-') && text[p+3] == ':')) ||
        tool_parse_timezone_suffix(text, &p, &zone, 0) != 0 || p != n || zone < -50400 || zone > 50400) return -1;
    date->seconds += (int64_t)hour*3600 + minute*60 + second -
        zone;
    date->precision = 2;
    return 0;
}

typedef struct { const char *id; uint32_t record; } Identity;
static int identity_compare(const void *a, const void *b) {
    return rt_strcmp(((const Identity *)a)->id, ((const Identity *)b)->id);
}
static uint32_t identity_find(const Identity *ids, size_t count, const char *id) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t mid = low + (high-low)/2;
        if (rt_strcmp(ids[mid].id, id) < 0) low = mid + 1; else high = mid;
    }
    return low < count && !rt_strcmp(ids[low].id, id) ? ids[low].record : NONE;
}

void search_facets_free(SearchIndex *index) {
    rt_free(index->dates); rt_free(index->date_lookup); rt_free(index->meeting_context);
    rt_free(index->version_lookup); rt_free(index->kinds); rt_free(index->kind_records);
}

int search_facets_build(SearchIndex *index) {
    size_t i, j, used = 0, count = index->record_count;
    size_t permanent = count * (SEARCH_DATE_END*sizeof(uint32_t) + 3*sizeof(uint32_t) + sizeof(SearchFacet)) +
        index->fact_count*sizeof(SearchDate);
    Identity *ids;
    if (index->memory_bytes + permanent + count*sizeof(Identity) > 4ULL*1024*1024*1024)
        return search_error("date/type facets exceed the 4 GiB search RAM budget", 0);
    ids = (Identity *)rt_malloc_array(count ? count : 1, sizeof(*ids));
    index->dates = (SearchDate *)rt_malloc_array(index->fact_count ? index->fact_count : 1, sizeof(*index->dates));
    index->date_lookup = (uint32_t *)rt_malloc_array(count ? count * SEARCH_DATE_END : 1, sizeof(uint32_t));
    index->meeting_context = (uint32_t *)rt_malloc_array(count ? count : 1, sizeof(uint32_t));
    index->version_lookup = (uint32_t *)rt_malloc_array(count ? count : 1, sizeof(uint32_t));
    index->kinds = (SearchFacet *)rt_malloc_array(count ? count : 1, sizeof(*index->kinds));
    index->kind_records = (uint32_t *)rt_malloc_array(count ? count : 1, sizeof(uint32_t));
    if (!ids || !index->dates || !index->date_lookup || !index->meeting_context ||
        !index->version_lookup || !index->kinds || !index->kind_records) {
        rt_free(ids); return search_error("cannot allocate date/identity/type facets", 0);
    }
    rt_memset(index->date_lookup, 255, count * SEARCH_DATE_END * sizeof(uint32_t));
    rt_memset(index->meeting_context, 255, count * sizeof(uint32_t));
    rt_memset(index->version_lookup, 255, count * sizeof(uint32_t));
    for (i = 0; i < index->fact_count; i++) {
        const SearchFact *fact = &index->facts[i];
        SearchDate *date = &index->dates[i];
        if (search_date_parse(index->strings + fact->value, date) != 0 ||
            ((fact->role == SEARCH_MEETING_START || fact->role == SEARCH_MEETING_END) && date->precision != 2))
            date->precision = 0;
        date->fact = (uint32_t)i;
        index->date_lookup[(size_t)fact->record * SEARCH_DATE_END + fact->role] = (uint32_t)i;
    }
    for (i = 0; i < count; i++) {
        const SearchRecord *r = &index->records[i];
        if (r->parent == NONE) { ids[used].id = index->strings + r->id; ids[used++].record = (uint32_t)i; }
        for (j = 0; j < index->kind_count &&
            rt_strcmp(index->strings + index->kinds[j].name, index->strings + r->kind); j++) {}
        if (j == index->kind_count) {
            rt_memset(&index->kinds[j], 0, sizeof(index->kinds[j]));
            index->kinds[j].name = r->kind; index->kind_count++;
        }
        index->kinds[j].count++;
    }
    rt_sort(ids, used, sizeof(*ids), identity_compare);
    for (i = 0; i < index->relation_count; i++) {
        const SearchRelation *r = &index->relations[i];
        uint32_t target, *meeting;
        if (r->role != SEARCH_MEETING && r->role != SEARCH_AGENDA_MEETING) continue;
        target = identity_find(ids, used, index->strings + r->target);
        meeting = &index->meeting_context[r->source];
        if (target == NONE || rt_strcmp(index->strings + index->records[target].kind, "Meeting")) {
            *meeting = NONE - 1; continue;
        }
        if (*meeting == NONE) *meeting = target;
        else if (*meeting != target) *meeting = NONE - 1;
    }
    rt_free(ids);
    used = 0;
    for (i = 0; i < index->kind_count; i++) {
        index->kinds[i].start = (uint32_t)used; used += index->kinds[i].count; index->kinds[i].count = 0;
    }
    for (i = 0; i < count; i++) {
        for (j = 0; rt_strcmp(index->strings + index->kinds[j].name,
            index->strings + index->records[i].kind); j++) {}
        index->kind_records[index->kinds[j].start + index->kinds[j].count++] = (uint32_t)i;
    }
    for (i = 0; i < index->version_count; i++)
        index->version_lookup[index->versions[i].record] = (uint32_t)i;
    index->imported_date.seconds = (int64_t)index->imported_at;
    index->imported_date.nanos = 0; index->imported_date.precision = 2; index->imported_date.fact = NONE;
    if (platform_format_time((long long)index->imported_at, 0, "%Y-%m-%dT%H:%M:%SZ",
                             index->imported_text, sizeof(index->imported_text)) != 0 ||
        search_date_parse(index->imported_text, &index->imported_date) != 0)
        return search_error("cannot format index import date", 0);
    index->memory_bytes += permanent;
    return 0;
}

int search_kind_known(const SearchIndex *index, const char *kind) {
    static const char *const standard[] = {"System","Body","Paper","File","Page","Meeting",
        "AgendaItem","Consultation","Organization","Person","Membership","Location","LegislativeTerm"};
    size_t i;
    for (i = 0; i < sizeof(standard)/sizeof(standard[0]); i++) if (!rt_strcmp(kind, standard[i])) return 1;
    for (i = 0; i < index->kind_count; i++) if (!rt_strcmp(kind, index->strings + index->kinds[i].name)) return 1;
    return 0;
}

void search_hit_date(const SearchIndex *index, SearchHit *hit, SearchDateRole role) {
    uint32_t source = index->records[hit->record].parent;
    const char *kind;
    if (source == NONE) source = hit->record;
    kind = index->strings + index->records[source].kind;
    if (!role) role = hit->paper != NONE || !rt_strcmp(kind, "Paper") ? SEARCH_PAPER_DATE :
        !rt_strcmp(kind, "Meeting") || !rt_strcmp(kind, "Consultation") || !rt_strcmp(kind, "AgendaItem") ?
        SEARCH_MEETING_START : SEARCH_DOCUMENT_DATE;
    if (role == SEARCH_PAPER_DATE && hit->paper != NONE) source = hit->paper;
    if ((role == SEARCH_MEETING_START || role == SEARCH_MEETING_END) &&
        index->meeting_context[source] < index->record_count) source = index->meeting_context[source];
    hit->date_source = source; hit->date_role = role;
    hit->date_fact = index->date_lookup[(size_t)source * SEARCH_DATE_END + role];
}

const SearchDate *search_hit_time(const SearchIndex *index, const SearchHit *hit) {
    return hit->date_role == SEARCH_IMPORTED ? &index->imported_date :
        hit->date_fact == NONE ? 0 : &index->dates[hit->date_fact];
}
const char *search_hit_date_text(const SearchIndex *index, const SearchHit *hit) {
    return hit->date_role == SEARCH_IMPORTED ? index->imported_text :
        hit->date_fact == NONE ? "" : index->strings + index->facts[hit->date_fact].value;
}

int search_hit_date_json(ToolByteBuffer *out, const SearchIndex *index, const SearchHit *hit) {
    const SearchDate *date = search_hit_time(index, hit);
    const SearchEvidence *proof = hit->date_role == SEARCH_IMPORTED || hit->date_fact == NONE ? 0 :
        &index->evidence[index->facts[hit->date_fact].evidence];
    const char *property = proof ? index->strings + proof->property : "importedAtEpochSeconds";
    if (!date) return tool_byte_buffer_append_cstr(out, "null");
    return tool_byte_buffer_append_cstr(out, "{\"value\":") ||
        search_json_quote(out, search_hit_date_text(index, hit)) ||
        tool_byte_buffer_append_cstr(out, ",\"role\":") || search_json_quote(out, roles[hit->date_role]) ||
        tool_byte_buffer_append_cstr(out, ",\"precision\":") ||
        search_json_quote(out, !date->precision ? "invalid" : date->precision == 1 ? "day" : "instant") ||
        tool_byte_buffer_append_cstr(out, ",\"sourceObject\":") ||
        (hit->date_role == SEARCH_IMPORTED ?
            tool_byte_buffer_append_cstr(out, "null,\"sourceIndex\":") || search_json_quote(out, index->snapshot_id) :
            search_json_quote(out, index->strings + index->records[hit->date_source].id)) ||
        tool_byte_buffer_append_cstr(out, ",\"property\":") || search_json_quote(out, property) ||
        tool_byte_buffer_append_cstr(out, ",\"rule\":") ||
        search_json_quote(out, proof ? index->strings + proof->rule : "local-archive-index-import-v1") ||
        tool_byte_buffer_append_cstr(out, ",\"certainty\":") ||
        search_json_quote(out, !proof || proof->certainty == SEARCH_EXACT ? "exact" :
            proof->certainty == SEARCH_DERIVED ? "derived" : "source-assertion") ||
        tool_byte_buffer_append_cstr(out, ",\"via\":") ||
        search_json_quote(out, hit->date_role == SEARCH_IMPORTED ? "index" :
            hit->date_source == hit->paper && hit->paper != hit->record ? "direct-paper-owner" :
            hit->date_source == hit->record ? "self" :
            hit->date_source == index->records[hit->record].parent ? "page-parent" : "meeting-context") ||
        tool_byte_buffer_append_cstr(out, "}") ? -1 : 0;
}

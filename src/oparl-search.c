#include "search.h"
#include "runtime.h"
#include "platform.h"

static int compare_time(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    const char *path = "build/search/index.ops", *query = 0, *error = 0, *show = 0;
    SearchFilter filter;
    SearchIndex index;
    SearchResult result;
    SearchWorkspace workspace;
    unsigned long long repetitions = 0, opened, started = platform_get_monotonic_time_ns();
    int i, json = 0, batch = 0, status = 1, serve = 0, port_set = 0, filter_set = 0;
    unsigned int page = 0, port = 8080;
    rt_memset(&workspace, 0, sizeof(workspace));
    rt_memset(&filter, 0, sizeof(filter)); filter.limit = 20;
    for (i = 1; i < argc; i++) {
        const char *flag = argv[i], *value;
        unsigned long long n;
        if (rt_strcmp(flag, "--help") == 0) {
            rt_write_cstr(1, "Usage: oparl-search [OPTIONS] QUERY\n"
                "  --index FILE          Binary index (default: build/search/index.ops)\n"
                "  --year YYYY           Reference year, also through related papers\n"
                "  --paper-type TYPE     Exact case-insensitive paper type\n"
                "  --organization TEXT   Responsible organization substring, not authorship\n"
                "  --kind TYPE           Paper, File, Page, Meeting, Organization, ...\n"
                "  --limit N             Top results, 1..100 (default: 20)\n"
                "  --offset N            Skip N ranked hits (default: 0)\n"
                "  --sort ORDER          relevance (default), date-desc or date-asc\n"
                "  --group MODE          record (default) or paper: one hit per related Paper\n"
                "  --json                Structured results with sources and page numbers\n"
                "  --batch               One query per stdin line; keep the index loaded\n"
                "  --benchmark N         Warm query repetitions, 20..100000\n"
                "  --show ID             Retrieve archived metadata by OParl ID\n"
                "  --page N              With --show: retrieve a PDF text page\n"
                "  --serve               Local read-only web UI and JSON API (127.0.0.1)\n"
                "  --port N              With --serve: TCP port, 1..65535 (default: 8080)\n"
                "Queries: AND (implicit), OR, NOT, parentheses, \"exact phrases\", prefix*\n");
            return 0;
        }
        if (rt_strcmp(flag, "--json") == 0) { json = 1; continue; }
        if (rt_strcmp(flag, "--batch") == 0) { batch = 1; continue; }
        if (rt_strcmp(flag, "--serve") == 0) { serve = 1; continue; }
        if (rt_strncmp(flag, "--", 2) != 0) {
            if (query) { search_error("pass the query as one quoted argument", flag); return 1; }
            query = flag; continue;
        }
        if (i + 1 == argc) { search_error("missing option value", flag); return 1; }
        value = argv[++i];
        if (rt_strcmp(flag, "--year") == 0 || rt_strcmp(flag, "--paper-type") == 0 ||
            rt_strcmp(flag, "--organization") == 0 || rt_strcmp(flag, "--kind") == 0 ||
            rt_strcmp(flag, "--limit") == 0 || rt_strcmp(flag, "--offset") == 0 ||
            rt_strcmp(flag, "--sort") == 0 || rt_strcmp(flag, "--group") == 0) filter_set = 1;
        if (rt_strcmp(flag, "--index") == 0) path = value;
        else if (rt_strcmp(flag, "--paper-type") == 0) filter.paper_type = value;
        else if (rt_strcmp(flag, "--organization") == 0) filter.organization = value;
        else if (rt_strcmp(flag, "--kind") == 0) filter.kind = value;
        else if (rt_strcmp(flag, "--group") == 0) {
            if (rt_strcmp(value, "record") == 0) filter.group = SEARCH_RECORDS;
            else if (rt_strcmp(value, "paper") == 0) filter.group = SEARCH_PAPERS;
            else { search_error("expected record or paper", flag); return 1; }
        }
        else if (rt_strcmp(flag, "--sort") == 0) {
            if (rt_strcmp(value, "relevance") == 0) filter.sort = SEARCH_RELEVANCE;
            else if (rt_strcmp(value, "date-desc") == 0) filter.sort = SEARCH_DATE_DESC;
            else if (rt_strcmp(value, "date-asc") == 0) filter.sort = SEARCH_DATE_ASC;
            else { search_error("expected relevance, date-desc or date-asc", flag); return 1; }
        }
        else if (rt_strcmp(flag, "--show") == 0) show = value;
        else {
            if (rt_parse_uint(value, &n) != 0) { search_error("invalid numeric value", flag); return 1; }
            if (rt_strcmp(flag, "--year") == 0 && n >= 2000 && n <= 2099) filter.year = (unsigned int)n;
            else if (rt_strcmp(flag, "--limit") == 0 && n >= 1 && n <= 100) filter.limit = (unsigned int)n;
            else if (rt_strcmp(flag, "--offset") == 0 && n <= UINT32_MAX) filter.offset = (unsigned int)n;
            else if (rt_strcmp(flag, "--benchmark") == 0 && n >= 20 && n <= 100000) repetitions = n;
            else if (rt_strcmp(flag, "--page") == 0 && n >= 1 && n <= 1000000) page = (unsigned int)n;
            else if (rt_strcmp(flag, "--port") == 0 && n >= 1 && n <= 65535) {
                port = (unsigned int)n; port_set = 1;
            }
            else { search_error("unknown option or value outside bounds", flag); return 1; }
        }
    }
    if ((serve && (query || batch || show || repetitions || page || json || filter_set)) ||
        (port_set && !serve) || (!serve && !batch && !query && !show) ||
        (batch && (query || repetitions || show)) || (show && (query || repetitions)) || (page && !show)) {
        search_error("choose a query, --batch, --show or --serve; --port requires --serve", 0); return 1;
    }
    if (search_open(&index, path) != 0) return 1;
    if (index.incomplete)
        rt_write_cstr(2, "oparl-search: warning: source archives or full-text coverage are incomplete\n");
    opened = platform_get_monotonic_time_ns();
    if (serve) status = search_serve(&index, port) == 0 ? 0 : 1;
    else if (show) {
        const SearchRecord *record = search_find(&index, show, page);
        if (!record) search_error("resource/page is not in this index", show);
        else if (search_show(1, &index, record, json) != 0) search_error("cannot write resource", show);
        else status = 0;
    }
    else if (batch) {
        char line[16385], ch;
        size_t used = 0;
        long got;
        status = 0;
        while ((got = platform_read(0, &ch, 1)) > 0) {
            if (ch != '\n') {
                if (used == sizeof(line) - 1) { search_error("query line too long", 0); status = 1; break; }
                line[used++] = ch; continue;
            }
            if (used && line[used - 1] == '\r') used--;
            line[used] = '\0'; used = 0;
            if (search_query(&index, &workspace, line, &filter, &result, &error) != 0 ||
                search_print(1, &index, &result, json) != 0) {
                search_error(error ? error : "cannot write search results", 0); status = 1; break;
            }
        }
        if (got < 0) { search_error("cannot read queries", 0); status = 1; }
        if (!status && used) {
            line[used] = '\0';
            if (search_query(&index, &workspace, line, &filter, &result, &error) != 0 ||
                search_print(1, &index, &result, json) != 0) {
                search_error(error ? error : "cannot write search results", 0); status = 1;
            }
        }
    } else if (repetitions) {
        unsigned long long *times = (unsigned long long *)rt_malloc_array((size_t)repetitions, sizeof(*times));
        unsigned long long n, first = platform_get_monotonic_time_ns();
        if (!times) { search_error("cannot allocate benchmark samples", 0); goto done; }
        if (search_query(&index, &workspace, query, &filter, &result, &error) != 0) {
            search_error(error, query); rt_free(times); goto done;
        }
        first = platform_get_monotonic_time_ns() - first;
        for (n = 0; n < repetitions; n++) {
            unsigned long long before = platform_get_monotonic_time_ns();
            if (search_query(&index, &workspace, query, &filter, &result, &error) != 0) {
                search_error(error, query); rt_free(times); goto done;
            }
            times[n] = platform_get_monotonic_time_ns() - before;
        }
        rt_sort(times, (size_t)repetitions, sizeof(*times), compare_time);
        if (rt_write_cstr(1, "{\"indexLoadUs\":") != 0 || rt_write_uint(1, (opened - started)/1000) != 0 ||
            tool_json_field_uint(1, "firstQueryUs", first/1000) != 0 ||
            tool_json_field_uint(1, "p50Us", times[(size_t)((repetitions*50+99)/100-1)]/1000) != 0 ||
            tool_json_field_uint(1, "p95Us", times[(size_t)((repetitions*95+99)/100-1)]/1000) != 0 ||
            tool_json_field_uint(1, "p99Us", times[(size_t)((repetitions*99+99)/100-1)]/1000) != 0 ||
            tool_json_field_uint(1, "queries", repetitions) != 0 ||
            tool_json_field_uint(1, "limit", filter.limit) != 0 ||
            tool_json_field_uint(1, "matches", result.total) != 0 ||
            tool_json_field_bool(1, "incomplete", index.incomplete != 0) != 0 ||
            rt_write_cstr(1, "}\n") != 0) {
            search_error("cannot write benchmark results", 0); rt_free(times); goto done;
        }
        rt_free(times);
        status = 0;
    } else {
        if (search_query(&index, &workspace, query, &filter, &result, &error) != 0) search_error(error, query);
        else if (search_print(1, &index, &result, json) != 0) search_error("cannot write search results", 0);
        else status = 0;
    }
done:
    search_workspace_free(&workspace);
    search_close(&index);
    return status;
}

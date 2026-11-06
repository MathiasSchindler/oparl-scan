#include "search.h"
#include "runtime.h"
#include "platform.h"

static int compare_time(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    const char *path = "build/search/index.ops", *query = 0, *error = 0, *show = 0;
    const char *read_operation = 0;
    SearchRead read;
    SearchFilter filter;
    SearchIndex index;
    SearchResult result;
    SearchWorkspace workspace;
    unsigned long long repetitions = 0, opened, started = platform_get_monotonic_time_ns();
    int i, json = 0, batch = 0, status = 1, serve = 0, port_set = 0, filter_set = 0, limit_set = 0;
    unsigned int page = 0, port = 8080;
    rt_memset(&workspace, 0, sizeof(workspace));
    rt_memset(&read, 0, sizeof(read));
    rt_memset(&filter, 0, sizeof(filter)); filter.limit = 20;
    for (i = 1; i < argc; i++) {
        const char *flag = argv[i], *value;
        unsigned long long n;
        if (rt_strcmp(flag, "--help") == 0) {
            rt_write_cstr(1, "Usage: oparl-search [OPTIONS] [QUERY]\n"
                "  --index FILE          Binary index (default: build/search/index.ops)\n"
                "  --year YYYY           Reference year, also through related papers\n"
                "  --paper-type TYPE     Exact case-insensitive paper type\n"
                "  --organization TEXT   Responsible organization substring, not authorship\n"
                "  --organization-id ID  Exact responsible organization ID\n"
                "  --actor-id ID         Exact raw actor relationship ID, not inferred authorship\n"
                "  --kind TYPE           Paper, File, Page, Meeting, Organization, ...\n"
                "  --date-role ROLE      auto, paper.date, meeting.start, created, ...\n"
                "  --date-from DATE      Inclusive local source day, YYYY-MM-DD\n"
                "  --date-through DATE   Inclusive local source day, YYYY-MM-DD\n"
                "  --limit N             Top results, 1..100 (default: 20)\n"
                "  --offset N            Skip N ranked hits (default: 0)\n"
                "  --snapshot-id ID      Bind continuations to the returned snapshot (required for offset>0)\n"
                "  --sort ORDER          relevance (default), date-desc or date-asc\n"
                "  --group MODE          record (default), paper or document (version)\n"
                "  --json                Structured results with sources and page numbers\n"
                "  --batch               One query per stdin line; keep the index loaded\n"
                "  --benchmark N         Warm query repetitions, 20..100000\n"
                "  --show ID             Retrieve archived metadata by OParl ID\n"
                "  --paper ID            Compact Paper, documents and relevant consultations\n"
                "  --meeting ID          Compact meeting status and documents\n"
                "  --agenda ID           Public agenda of a meeting\n"
                "  --object ID           Bulk metadata; repeat up to 20 times\n"
                "  --options             Discover types, dates and stable IDs (with --kind)\n"
                "  --status              Index time, snapshot and per-type coverage\n"
                "  --fields FIELDS       Comma-separated metadata projection fields\n"
                "  --view relations      Paginated relationships with --show\n"
                "  --direction VALUE     incoming, outgoing or both (relations only)\n"
                "  --raw                 Explicit full source metadata with --show/--paper\n"
                "  --page N              With --show: retrieve a PDF text page\n"
                "  --serve               Local read-only web UI and JSON API (127.0.0.1)\n"
                "  --port N              With --serve: TCP port, 1..65535 (default: 8080)\n"
                "Queries: AND (implicit), OR, NOT, parentheses, \"exact phrases\", prefix*\n"
                "Omit QUERY for a filtered listing; unfiltered empty listings are rejected.\n");
            return 0;
        }
        if (rt_strcmp(flag, "--json") == 0) { json = 1; continue; }
        if (rt_strcmp(flag, "--batch") == 0) { batch = 1; continue; }
        if (rt_strcmp(flag, "--serve") == 0) { serve = 1; continue; }
        if (!rt_strcmp(flag, "--raw")) { read.raw = 1; continue; }
        if (!rt_strcmp(flag, "--status") || !rt_strcmp(flag, "--options")) {
            if (read_operation) { search_error("choose one research operation", flag); return 1; }
            read_operation = !rt_strcmp(flag, "--status") ? "get_index_status" : "get_search_options"; continue;
        }
        if (rt_strncmp(flag, "--", 2) != 0) {
            if (query) { search_error("pass the query as one quoted argument", flag); return 1; }
            query = flag; continue;
        }
        if (i + 1 == argc) { search_error("missing option value", flag); return 1; }
        value = argv[++i];
        if (rt_strcmp(flag, "--year") == 0 || rt_strcmp(flag, "--paper-type") == 0 ||
            rt_strcmp(flag, "--organization") == 0 || rt_strcmp(flag, "--kind") == 0 ||
            rt_strcmp(flag, "--limit") == 0 || rt_strcmp(flag, "--offset") == 0 ||
            rt_strcmp(flag, "--sort") == 0 || rt_strcmp(flag, "--group") == 0 ||
            rt_strcmp(flag, "--date-role") == 0 || rt_strcmp(flag, "--date-from") == 0 ||
            rt_strcmp(flag, "--date-through") == 0 || !rt_strcmp(flag, "--organization-id") ||
            !rt_strcmp(flag, "--actor-id") || !rt_strcmp(flag, "--snapshot-id")) filter_set = 1;
        if (rt_strcmp(flag, "--index") == 0) path = value;
        else if (rt_strcmp(flag, "--paper-type") == 0) filter.paper_type = value;
        else if (rt_strcmp(flag, "--organization") == 0) filter.organization = value;
        else if (rt_strcmp(flag, "--kind") == 0) filter.kind = value;
        else if (!rt_strcmp(flag, "--organization-id")) filter.organization_id = value;
        else if (!rt_strcmp(flag, "--actor-id")) filter.actor_id = value;
        else if (!rt_strcmp(flag, "--snapshot-id")) filter.snapshot_id = value;
        else if (!rt_strcmp(flag, "--fields")) read.fields = value;
        else if (!rt_strcmp(flag, "--view")) read.view = value;
        else if (!rt_strcmp(flag, "--direction")) read.direction = value;
        else if (!rt_strcmp(flag, "--date-from")) filter.date_from = value;
        else if (!rt_strcmp(flag, "--date-through")) filter.date_through = value;
        else if (!rt_strcmp(flag, "--date-role")) {
            if (search_date_role(value, &filter.date_role)) {
                search_error("unknown temporal date role; reference year uses --year", value); return 1;
            }
        }
        else if (rt_strcmp(flag, "--group") == 0) {
            if (rt_strcmp(value, "record") == 0) filter.group = SEARCH_RECORDS;
            else if (rt_strcmp(value, "paper") == 0) filter.group = SEARCH_PAPERS;
            else if (!rt_strcmp(value, "document")) filter.group = SEARCH_DOCUMENTS;
            else { search_error("expected record, paper or document; other units are not established", flag); return 1; }
        }
        else if (rt_strcmp(flag, "--sort") == 0) {
            if (rt_strcmp(value, "relevance") == 0) filter.sort = SEARCH_RELEVANCE;
            else if (rt_strcmp(value, "date-desc") == 0) filter.sort = SEARCH_DATE_DESC;
            else if (rt_strcmp(value, "date-asc") == 0) filter.sort = SEARCH_DATE_ASC;
            else { search_error("expected relevance, date-desc or date-asc", flag); return 1; }
        }
        else if (!rt_strcmp(flag, "--show") || !rt_strcmp(flag, "--paper") || !rt_strcmp(flag, "--meeting") ||
                 !rt_strcmp(flag, "--agenda") || !rt_strcmp(flag, "--object")) {
            const char *operation = !rt_strcmp(flag, "--show") ? "get_object" :
                !rt_strcmp(flag, "--paper") ? "get_paper" : !rt_strcmp(flag, "--meeting") ? "get_meeting" :
                !rt_strcmp(flag, "--agenda") ? "get_agenda" : "get_objects";
            if (read_operation && (rt_strcmp(read_operation, "get_objects") || rt_strcmp(operation, "get_objects"))) {
                search_error("choose one research operation", flag); return 1;
            }
            read_operation = operation;
            if (!rt_strcmp(operation, "get_objects")) {
                if (read.id_count == 20) { search_error("bulk metadata is limited to 20 IDs", flag); return 1; }
                read.ids[read.id_count++] = value;
            } else { show = value; read.id = value; }
        }
        else {
            if (rt_parse_uint(value, &n) != 0) { search_error("invalid numeric value", flag); return 1; }
            if (rt_strcmp(flag, "--year") == 0 && n >= 2000 && n <= 2099) filter.year = (unsigned int)n;
            else if (rt_strcmp(flag, "--limit") == 0 && n >= 1 && n <= 100) { filter.limit = (unsigned int)n; limit_set = 1; }
            else if (rt_strcmp(flag, "--offset") == 0 && n <= UINT32_MAX) filter.offset = (unsigned int)n;
            else if (rt_strcmp(flag, "--benchmark") == 0 && n >= 20 && n <= 100000) repetitions = n;
            else if (rt_strcmp(flag, "--page") == 0 && n >= 1 && n <= 1000000) page = (unsigned int)n;
            else if (rt_strcmp(flag, "--port") == 0 && n >= 1 && n <= 65535) {
                port = (unsigned int)n; port_set = 1;
            }
            else { search_error("unknown option or value outside bounds", flag); return 1; }
        }
    }
    if ((serve && (query || batch || read_operation || repetitions || page || json || filter_set || read.raw || read.fields || read.view || read.direction)) ||
        (port_set && !serve) || (!serve && !batch && !query && !read_operation && !filter_set) ||
        (batch && (query || repetitions || read_operation)) || (read_operation && (query || repetitions)) ||
        (page && (!read_operation || rt_strcmp(read_operation, "get_object"))) ||
        (!read_operation && (read.raw || read.fields || read.view || read.direction)) ||
        (read_operation && !rt_strcmp(read_operation, "get_index_status") && (filter_set || read.raw || read.fields || read.view || read.direction)) ||
        (read_operation && (filter.year || filter.paper_type || filter.organization || filter.organization_id ||
            filter.actor_id || filter.date_from || filter.date_through || filter.date_role || filter.sort || filter.group))) {
        search_error("choose a query, --batch, --show or --serve; --port requires --serve", 0); return 1;
    }
    if (search_open(&index, path) != 0) return 1;
    if (index.incomplete)
        rt_write_cstr(2, "oparl-search: warning: source archives or full-text coverage are incomplete\n");
    opened = platform_get_monotonic_time_ns();
    if (serve) status = search_serve(&index, port) == 0 ? 0 : 1;
    else if (read_operation) {
        ToolByteBuffer data;
        int code;
        read.kind = filter.kind;
        read.limit = limit_set ? filter.limit : !rt_strcmp(read_operation, "get_object") ? 0 : 10;
        read.offset = filter.offset;
        read.snapshot_id = filter.snapshot_id; read.page = page;
        tool_byte_buffer_init(&data);
        code = !rt_strcmp(read_operation, "get_index_status") ? search_status_json(&data, &index) :
            search_read_json(&data, &index, read_operation, &read, &error);
        if (code) search_error(error ? error : "cannot read index status", show);
        else if (rt_write_all(1, data.data, data.size)) search_error("cannot write research result", show);
        else status = 0;
        tool_byte_buffer_free(&data);
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

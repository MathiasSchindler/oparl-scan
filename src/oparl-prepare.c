#include "archive-walk.h"
#include "platform.h"
#include "prepare.h"
#include "runtime.h"

typedef struct {
    PrepareConfig config;
    const char *output;
    unsigned long long archives, documents, reused, incomplete;
    int force;
} Prepare;
static int path_contains(const char *parent, const char *child) {
    size_t i;
    for (i = 0; parent[i]; i++) {
        char a = parent[i], b = child[i];
#if defined(_WIN32)
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
#endif
        if (a != b)
            return 0;
    }
    return !child[i] || child[i] == '/' || child[i] == '\\' ||
           (i && (parent[i - 1] == '/' || parent[i - 1] == '\\'));
}
static int separate_output(const char *input, const char *output) {
    char source[4096], target[4096];
    if (tool_canonicalize_path(input, 1, 0, source, sizeof(source)) ||
        tool_canonicalize_path(output, 1, 1, target, sizeof(target)))
        return prepare_error("cannot resolve input/output paths: ", output);
    if (path_contains(source, target) || path_contains(target, source))
        return prepare_error("use a dedicated output directory outside the source archive: ", output);
    return 0;
}
static int document(const char *path, const PlatformDirEntry *info, int depth, ToolWalkControl *control,
                    void *user) {
    Prepare *p = (Prepare *)user;
    unsigned char *bytes = 0;
    size_t length;
    int reused, incomplete, status;
    (void)depth;
    (void)control;
    if (info->is_dir)
        return 0;
    length = rt_strlen(path);
    if (length >= 5 && !rt_strcmp(path + length - 5, ".part"))
        return 0;
    if (info->size > 512U * 1024U * 1024U)
        return prepare_error("source document exceeds 512 MiB input budget: ", path);
    if (tool_read_all_input(path, &bytes, &length))
        return prepare_error("cannot read source document: ", path);
    status = prepare_document(&p->config, p->output, bytes, length, p->force, &reused, &incomplete);
    rt_free(bytes);
    if (status)
        return -1;
    p->documents++;
    p->reused += reused;
    p->incomplete += incomplete;
    if (incomplete)
        tool_write_error("oparl-prepare", "PDF warnings/failure; inspect the prepared artifact: ", path);
    if (p->documents % 100 == 0) {
        rt_write_cstr(2, "Prepared source documents: ");
        rt_write_uint(2, p->documents);
        rt_write_char(2, '\n');
    }
    return 0;
}
static int archive(const char *path, const PlatformDirEntry *info, int depth, ToolWalkControl *control,
                   void *user) {
    Prepare *p = (Prepare *)user;
    PlatformDirEntry entry;
    char manifest[4096], documents[4096];
    unsigned char *bytes = 0;
    size_t length;
    Json json;
    int metadata_only;
    (void)depth;
    if (!info->is_dir)
        return 0;
    if (rt_join_path(path, "archive.json", manifest, sizeof(manifest)))
        return prepare_error("archive path too long: ", path);
    if (platform_get_path_info(manifest, &entry))
        return 0;
    if (separate_output(path, p->output))
        return -1;
    control->prune = 1;
    if (tool_read_all_input(manifest, &bytes, &length))
        return prepare_error("cannot read archive manifest: ", manifest);
    if (json_parse(&json, (const char *)bytes, length)) {
        rt_free(bytes);
        return prepare_error("invalid archive manifest: ", manifest);
    }
    metadata_only = json_true(&json, json_get(&json, 0, "metadataOnly"));
    json_free(&json);
    rt_free(bytes);
    p->archives++;
    if (rt_join_path(path, "documents", documents, sizeof(documents)))
        return prepare_error("documents path too long: ", path);
    if (platform_get_path_info(documents, &entry))
        return metadata_only ? 0 : prepare_error("archive documents directory is missing: ", path);
    return archive_walk(documents, document, p, 0);
}
int main(int argc, char **argv) {
    const char *input = "downloads", *output = "build/prepared", *templates = "templates/potsdam.json";
    Prepare p;
    PlatformDirEntry info;
    int i, result = 1, layout = 0;
    rt_memset(&p, 0, sizeof(p));
    for (i = 1; i < argc; i++) {
        if (!rt_strcmp(argv[i], "--help")) {
            rt_write_cstr(1,
                          "Usage: oparl-prepare [--input ARCHIVE_OR_PARENT_OR_PDF] [--output DIRECTORY]\n"
                          "                     [--templates JSON] [--layout] [--force]\n"
                          "Extract PDF text and positioned header fields without changing originals.\n"
                          "--layout keeps positioned text on every page, not just the first page.\n"
                          "--force regenerates artifacts, including corrupt cached artifacts.\n"
                          "Exit: 0 extracted, 1 fatal error, 3 PDF warnings/failures documented in cache.\n");
            return 0;
        }
        if (!rt_strcmp(argv[i], "--layout")) {
            layout = 1;
            continue;
        }
        if (!rt_strcmp(argv[i], "--force")) {
            p.force = 1;
            continue;
        }
        if (i + 1 == argc) {
            prepare_error("missing option value: ", argv[i]);
            goto done;
        }
        if (!rt_strcmp(argv[i], "--input"))
            input = argv[++i];
        else if (!rt_strcmp(argv[i], "--output"))
            output = argv[++i];
        else if (!rt_strcmp(argv[i], "--templates"))
            templates = argv[++i];
        else {
            prepare_error("unknown option: ", argv[i]);
            goto done;
        }
    }
    p.output = output;
    if (prepare_config_open(&p.config, templates, layout))
        goto done;
    if (platform_get_path_info(input, &info)) {
        prepare_error("cannot inspect input: ", input);
        goto done;
    }
    if (info.is_dir) {
        if (archive_walk(input, archive, &p, 0))
            goto done;
        if (!p.archives) {
            prepare_error("no OParl archives found: ", input);
            goto done;
        }
    } else if (separate_output(input, output) || document(input, &info, 0, 0, &p))
        goto done;
    if (prepare_manifest_write(&p.config, output))
        goto done;
    if (rt_write_cstr(1, "{\"documents\":") || rt_write_uint(1, p.documents) ||
        tool_json_field_uint(1, "reused", p.reused) ||
        tool_json_field_uint(1, "warningOrFailedDocuments", p.incomplete) ||
        tool_json_field_string(1, "key", p.config.key) || rt_write_cstr(1, "}\n")) {
        prepare_error("cannot write preparation summary: ", 0);
        goto done;
    }
    result = p.incomplete ? 3 : 0;
done:
    prepare_config_close(&p.config);
    return result;
}

#include "index_io.h"
#include <stdint.h>

int eg_parse_digest(const char *text, unsigned char digest[32]) {
    if (rt_strlen(text) != 64) return eg_error("expected a SHA-256 digest", 0);
    for (unsigned int i = 0; i < 32; i++) {
        int hi = tool_hex_value(text[i * 2]), lo = tool_hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return eg_error("invalid SHA-256 digest", text);
        digest[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

int eg_read_regular(const char *path, size_t limit, unsigned char **bytes, size_t *size) {
    PlatformDirEntry info;
    *bytes = 0; *size = 0;
    if (platform_get_path_info(path, &info) || (info.mode & 0170000U) != 0100000U ||
        info.size > limit || tool_read_all_input(path, bytes, size))
        return eg_error("cannot read regular file within size budget: ", path);
    if (*size != info.size) {
        rt_free(*bytes); *bytes = 0;
        return eg_error("source changed while reading: ", path);
    }
    return 0;
}

int eg_ensure_directory(const char *path) {
    PlatformDirEntry info;
    char scratch[4096];
    if (!platform_get_path_info(path, &info)) {
        if (info.is_dir && (info.mode & 0170000U) == 0040000U) return 0;
        return eg_error("output path is not a regular directory: ", path);
    }
    if (tool_ensure_parent_dirs(path, scratch, sizeof(scratch)) || platform_make_directory(path, 0700))
        return eg_error("cannot create directory: ", path);
    return 0;
}

int eg_atomic_write(const char *path, const void *data, size_t size) {
    char temporary[4096], suffix[80], pid[32], stamp[32];
    size_t length = rt_strlen(path);
    rt_unsigned_to_string((unsigned int)platform_get_process_id(), pid, sizeof(pid));
    rt_unsigned_to_string(platform_get_monotonic_time_ns(), stamp, sizeof(stamp));
    size_t n = tool_buffer_append_cstr(suffix, sizeof(suffix), 0, ".part-");
    n = tool_buffer_append_cstr(suffix, sizeof(suffix), n, pid);
    n = tool_buffer_append_char(suffix, sizeof(suffix), n, '-');
    tool_buffer_append_cstr(suffix, sizeof(suffix), n, stamp);
    if (length + rt_strlen(suffix) >= sizeof(temporary)) return eg_error("output path too long: ", path);
    memcpy(temporary, path, length);
    memcpy(temporary + length, suffix, rt_strlen(suffix) + 1);
    int fd = platform_open_create_exclusive(temporary, 0600);
    if (fd < 0) return eg_error("cannot create temporary output: ", temporary);
    int failed = rt_write_all(fd, data, size);
    if (platform_close(fd)) failed = -1;
    if (!failed && platform_sync_path(temporary)) failed = -1;
    if (!failed && platform_rename_path(temporary, path)) failed = -1;
    if (failed) {
        eg_error("cannot commit output: ", path);
        if (platform_remove_file(temporary)) eg_error("cannot clean temporary output: ", temporary);
        return -1;
    }
    return 0;
}

#if defined(_WIN32)
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) int __stdcall FreeLibrary(void *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
typedef struct {
    uint32_t attributes, times[6], size_high, size_low, reserved[2];
    char name[260], alternate[14];
} FindData;
typedef void *(__stdcall *FindFirst)(const char *, FindData *);
typedef int(__stdcall *FindNext)(void *, FindData *);
typedef int(__stdcall *FindClose)(void *);
#endif

int eg_absolute_path(const char *input, int allow_missing, char *out, size_t capacity) {
#if defined(_WIN32)
    typedef unsigned long(__stdcall *FullPath)(const char *, unsigned long, char *, char **);
    union { void *pointer; FullPath function; } full;
    void *module = LoadLibraryA("KERNEL32.dll");
    if (!module) return eg_error("cannot load OS path functions", 0);
    full.pointer = GetProcAddress(module, "GetFullPathNameA");
    unsigned long length = full.pointer ? full.function(input, (unsigned long)capacity, out, 0) : 0;
    FreeLibrary(module);
    if (!length || length >= capacity) return eg_error("cannot resolve path: ", input);
    for (size_t i = 0; i < length; i++) if (out[i] == '\\') out[i] = '/';
    (void)allow_missing;
#else
    if (tool_canonicalize_path(input, 1, allow_missing, out, capacity))
        return eg_error("cannot resolve path: ", input);
#endif
    return 0;
}

static int compare_file(const void *a, const void *b) {
    return rt_strcmp(((const EgIndexFile *)a)->name, ((const EgIndexFile *)b)->name);
}

static int add_file(ToolArray *files, const char *name, const char *suffix) {
    size_t length = rt_strlen(name), ending = suffix ? rt_strlen(suffix) : 0;
    if (name[0] == '.' || length < ending || (ending && rt_strcmp(name + length - ending, suffix))) return 0;
    EgIndexFile *file = tool_array_append(files);
    if (!file) return eg_error("cannot allocate file list", 0);
    rt_memset(file, 0, sizeof(*file));
    if (length >= sizeof(file->name)) return eg_error("filename too long: ", name);
    rt_copy_string(file->name, sizeof(file->name), name);
    return 0;
}

int eg_index_files(const char *directory, const char *suffix, ToolArray *files) {
    int result = 0;
#if defined(_WIN32)
    union { void *pointer; FindFirst function; } first;
    union { void *pointer; FindNext function; } next;
    union { void *pointer; FindClose function; } close;
    FindData data;
    char pattern[4096];
    void *module = LoadLibraryA("KERNEL32.dll"), *handle;
    if (!module) return eg_error("cannot load OS directory functions", 0);
    first.pointer = GetProcAddress(module, "FindFirstFileA");
    next.pointer = GetProcAddress(module, "FindNextFileA");
    close.pointer = GetProcAddress(module, "FindClose");
    if (!first.pointer || !next.pointer || !close.pointer ||
        rt_join_path(directory, "*", pattern, sizeof(pattern))) {
        FreeLibrary(module); return eg_error("cannot prepare directory scan: ", directory);
    }
    handle = first.function(pattern, &data);
    if (handle == (void *)(intptr_t)-1) {
        FreeLibrary(module); return eg_error("cannot open directory: ", directory);
    }
    for (;;) {
        if (!(data.attributes & 0x10U) && add_file(files, data.name, suffix)) { result = -1; break; }
        if (!next.function(handle, &data)) {
            if (GetLastError() != 18) result = eg_error("cannot enumerate directory: ", directory);
            break;
        }
    }
    if (!close.function(handle)) result = eg_error("cannot close directory", 0);
    FreeLibrary(module);
#else
    PlatformLightDirEntry *entries;
    size_t capacity = 1024, count = 0;
    int is_directory = 0;
    for (;;) {
        entries = rt_malloc_array(capacity, sizeof(*entries));
        if (!entries) return eg_error("cannot allocate directory scan", 0);
        rt_memset(entries, 0, capacity * sizeof(*entries));
        if (!platform_collect_light_entries(directory, 0, entries, capacity, &count, &is_directory)) break;
        int full = entries[capacity - 1].name[0] != 0;
        rt_free(entries);
        if (!full || capacity >= 1048576U) return eg_error("cannot enumerate directory: ", directory);
        capacity *= 2;
    }
    if (!is_directory) result = eg_error("path is not a directory: ", directory);
    for (size_t i = 0; !result && i < count; i++)
        if (!entries[i].is_dir) result = add_file(files, entries[i].name, suffix);
    rt_free(entries);
#endif
    if (!result) rt_sort(files->data, files->count, sizeof(EgIndexFile), compare_file);
    return result;
}

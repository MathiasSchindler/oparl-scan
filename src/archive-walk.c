#include "archive-walk.h"
#include "runtime.h"
#include <stdint.h>

static int fail(const char *message, const char *path) {
    tool_write_error("archive", message, path);
    return -1;
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
int archive_walk(const char *path, ToolWalkCallback callback, void *user, int depth) {
    PlatformDirEntry current;
    ToolWalkControl control = {0};
    char child[4096];
    int status = 0;
    if (depth > 64 || platform_get_path_info(path, &current))
        return fail("cannot inspect archive path or directory nesting exceeds 64: ", path);
    if (callback(path, &current, depth, &control, user))
        return -1;
    if (!current.is_dir || control.prune)
        return 0;
#if defined(_WIN32)
    {
        union {
            void *pointer;
            FindFirst function;
        } first;
        union {
            void *pointer;
            FindNext function;
        } next;
        union {
            void *pointer;
            FindClose function;
        } close;
        FindData data;
        void *module = LoadLibraryA("KERNEL32.dll"), *handle;
        if (!module)
            return fail("cannot load directory functions: ", path);
        first.pointer = GetProcAddress(module, "FindFirstFileA");
        next.pointer = GetProcAddress(module, "FindNextFileA");
        close.pointer = GetProcAddress(module, "FindClose");
        if (!first.pointer || !next.pointer || !close.pointer ||
            rt_join_path(path, "*", child, sizeof(child))) {
            FreeLibrary(module);
            return fail("cannot prepare directory scan: ", path);
        }
        handle = first.function(child, &data);
        if (handle == (void *)(intptr_t)-1) {
            unsigned long error = GetLastError();
            FreeLibrary(module);
            return error == 2 ? 0 : fail("cannot open directory: ", path);
        }
        for (;;) {
            if (data.name[0] != '.') {
                if ((data.attributes & 0x400U) || rt_join_path(path, data.name, child, sizeof(child)) ||
                    archive_walk(child, callback, user, depth + 1)) {
                    status = fail("cannot traverse archive entry or junction: ", path);
                    break;
                }
            }
            if (!next.function(handle, &data)) {
                if (GetLastError() != 18)
                    status = fail("cannot read directory: ", path);
                break;
            }
        }
        if (!close.function(handle))
            status = fail("cannot close directory: ", path);
        FreeLibrary(module);
    }
#else
    {
        PlatformLightDirEntry *entries;
        size_t capacity = 1024, count = 0, i;
        int directory = 0;
        for (;;) {
            entries = (PlatformLightDirEntry *)rt_malloc_array(capacity, sizeof(*entries));
            if (!entries)
                return fail("cannot allocate directory entries: ", path);
            rt_memset(entries, 0, capacity * sizeof(*entries));
            if (!platform_collect_light_entries(path, 0, entries, capacity, &count, &directory))
                break;
            i = entries[capacity - 1].name[0] != '\0';
            rt_free(entries);
            if (!i || capacity > SIZE_MAX / 2 / sizeof(*entries))
                return fail("cannot read directory: ", path);
            capacity *= 2;
        }
        if (!directory)
            status = fail("directory changed during traversal: ", path);
        for (i = 0; !status && i < count; i++) {
            if (entries[i].name[0] == '.')
                continue;
            if (rt_join_path(path, entries[i].name, child, sizeof(child)) ||
                archive_walk(child, callback, user, depth + 1))
                status = -1;
        }
        rt_free(entries);
    }
#endif
    return status;
}

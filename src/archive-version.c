#include "archive-version.h"
#include "hash_util.h"
#include "runtime.h"
#include "tool_util.h"

int archive_content_hash(const char *path, char digest[65]) {
    unsigned char bytes[32];
    int fd = platform_open_read(path), result;
    if (fd < 0) return -1;
    result = hash_sha256_stream(fd, bytes);
    if (platform_close(fd) != 0) result = -1;
    if (result != 0) return -1;
    hash_to_hex(bytes, sizeof(bytes), digest);
    return 0;
}

int archive_preserve(const char *root, const char *kind, const char *key,
                     const char *source, const char *digest, char *relative, size_t capacity) {
    unsigned char bytes[65536], hashed[32];
    char identity[65], directory[256], path[1024], part[1024], scratch[1024], existing[65];
    PlatformDirEntry info;
    CryptoSha256Context sha;
    int input = -1, output = -1, result = -1;
    crypto_sha256_hash((const unsigned char *)key, rt_strlen(key), hashed);
    hash_to_hex(hashed, sizeof(hashed), identity);
    if (rt_join_path("history", kind, directory, sizeof(directory)) != 0 ||
        rt_join_path(directory, identity, scratch, sizeof(scratch)) != 0 ||
        rt_join_path(scratch, digest, relative, capacity) != 0 ||
        rt_join_path(root, relative, path, sizeof(path)) != 0) return -1;
    if (platform_get_path_info(path, &info) == 0) {
        return !info.is_dir && archive_content_hash(path, existing) == 0 &&
               rt_strcmp(existing, digest) == 0 ? 0 : -1;
    }
    if (tool_path_append_suffix(path, ".part", part, sizeof(part)) != 0 ||
        tool_ensure_parent_dirs(part, scratch, sizeof(scratch)) != 0) return -1;
    input = platform_open_read(source);
    if (input < 0) return -1;
    output = platform_open_write(part, 0644);
    if (output < 0) goto done;
    crypto_sha256_init(&sha);
    for (;;) {
        long got = platform_read(input, bytes, sizeof(bytes));
        if (got < 0) goto done;
        if (!got) break;
        crypto_sha256_update(&sha, bytes, (size_t)got);
        if (rt_write_all(output, bytes, (size_t)got) != 0) goto done;
    }
    crypto_sha256_final(&sha, hashed);
    hash_to_hex(hashed, sizeof(hashed), existing);
    if (rt_strcmp(existing, digest)) goto done;
    result = 0;
done:
    if (platform_close(input) != 0) result = -1;
    if (output >= 0 && platform_close(output) != 0) result = -1;
    if (result == 0 && platform_rename_path(part, path) != 0) result = -1;
    return result;
}

int archive_timestamp(unsigned long long seconds, char *out, size_t capacity) {
    if (!seconds || seconds > 0x7fffffffffffffffULL) return -1;
    return platform_format_time((long long)seconds, 0, "%Y-%m-%dT%H:%M:%SZ", out, capacity);
}

int archive_parse_timestamp(const char *text, unsigned long long *seconds) {
    char day[11];
    unsigned int year = 0, month, date, hour, minute, second, i, offset = 0;
    size_t position = 19, length = rt_strlen(text);
    long long epoch;
    int sign = 1;
    if (length < 20 || text[10] != 'T' || text[13] != ':' || text[16] != ':')
        return -1;
    memcpy(day, text, 10); day[10] = 0;
    if (!archive_valid_date(day)) return -1;
    for (i = 11; i < 19; i++)
        if (i != 13 && i != 16 && (text[i] < '0' || text[i] > '9')) return -1;
    for (i = 0; i < 4; i++) year = year * 10U + (unsigned int)(text[i] - '0');
    month = (unsigned int)((text[5] - '0') * 10 + text[6] - '0');
    date = (unsigned int)((text[8] - '0') * 10 + text[9] - '0');
    hour = (unsigned int)((text[11] - '0') * 10 + text[12] - '0');
    minute = (unsigned int)((text[14] - '0') * 10 + text[15] - '0');
    second = (unsigned int)((text[17] - '0') * 10 + text[18] - '0');
    if (text[position] == '.') {
        size_t first = ++position;
        while (position < length && text[position] >= '0' && text[position] <= '9') position++;
        if (position == first) return -1;
    }
    if (position + 1 == length && text[position] == 'Z') {
        offset = 0;
    } else {
        unsigned int hours, minutes;
        if (position + 6 != length || (text[position] != '+' && text[position] != '-') ||
            text[position + 3] != ':') return -1;
        for (i = 1; i < 6; i++)
            if (i != 3 && (text[position + i] < '0' || text[position + i] > '9')) return -1;
        hours = (unsigned int)((text[position + 1] - '0') * 10 + text[position + 2] - '0');
        minutes = (unsigned int)((text[position + 4] - '0') * 10 + text[position + 5] - '0');
        if (hours > 23 || minutes > 59) return -1;
        offset = hours * 3600U + minutes * 60U;
        sign = text[position] == '+' ? 1 : -1;
    }
    if (tool_build_epoch_timestamp((int)year, month, date, hour, minute, second, &epoch) != 0)
        return -1;
    epoch -= (long long)sign * offset;
    if (epoch <= 0) return -1;
    *seconds = (unsigned long long)epoch;
    return 0;
}

int archive_valid_date(const char *date) {
    unsigned int year = 0, month, day, i, days;
    static const unsigned int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (rt_strlen(date) != 10 || date[4] != '-' || date[7] != '-') return 0;
    for (i = 0; i < 10; i++) if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return 0;
    for (i = 0; i < 4; i++) year = year * 10 + (unsigned int)(date[i] - '0');
    month = (unsigned int)((date[5]-'0')*10 + date[6]-'0');
    day = (unsigned int)((date[8]-'0')*10 + date[9]-'0');
    if (!year || !month || month > 12) return 0;
    days = lengths[month-1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return day >= 1 && day <= days;
}

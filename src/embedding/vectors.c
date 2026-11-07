#include "eg2.h"
#include "index_io.h"
#include "crypto/sha256.h"
#include "math/math.h"

void eg_title_key(const char *id, const char *title, unsigned char key[32]) {
    CryptoSha256Context hash;
    crypto_sha256_init(&hash);
    crypto_sha256_update(&hash, (const unsigned char *)EG2_TITLE_RECIPE, sizeof(EG2_TITLE_RECIPE) - 1);
    crypto_sha256_update(&hash, (const unsigned char *)id, rt_strlen(id) + 1);
    crypto_sha256_update(&hash, (const unsigned char *)title, rt_strlen(title));
    crypto_sha256_final(&hash, key);
}

void eg_title_vector_path(const unsigned char key[32], char path[80]) {
    static const char hex[] = "0123456789abcdef";
    memcpy(path, "cache/", 6);
    for (unsigned int i = 0; i < 32; i++) {
        path[6 + i * 2] = hex[key[i] >> 4];
        path[7 + i * 2] = hex[key[i] & 15];
    }
    memcpy(path + 70, ".egv", 5);
}

int eg_vector_valid(const float *vector) {
    double norm = 0;
    for (unsigned int i = 0; i < EG2_DIM; i++) {
        if (!math_is_finite(vector[i])) return 0;
        norm += (double)vector[i] * vector[i];
    }
    return norm > 0.99999 && norm < 1.00001;
}

int eg_vector_read(const char *path, const unsigned char key[32], float *vector, int *found) {
    PlatformDirEntry info;
    unsigned char *data = 0, sha[32];
    float values[EG2_DIM];
    size_t size = 0;
    *found = 0;
    if (platform_get_path_info(path, &info)) return 0;
    if ((info.mode & 0170000U) != 0100000U || info.size != EG2_VECTOR_SIZE ||
        tool_read_all_input(path, &data, &size)) {
        rt_free(data);
        return eg_error("invalid or unreadable cached vector: ", path);
    }

    if (size != EG2_VECTOR_SIZE || memcmp(data, "EG2VEC01", 8) || memcmp(data + 8, key, 32))
        goto invalid;
    crypto_sha256_hash(data + 72, EG2_DIM * 4U, sha);
    if (memcmp(sha, data + 40, 32)) goto invalid;
    memcpy(values, data + 72, sizeof(values));
    if (!eg_vector_valid(values)) goto invalid;
    if (vector) memcpy(vector, values, sizeof(values));
    rt_free(data);
    *found = 1;
    return 0;
invalid:
    rt_free(data);
    return eg_error("cached vector failed verification (remove this file to recompute): ", path);
}

int eg_vector_write(const char *path, const unsigned char key[32], const float *vector) {
    unsigned char data[EG2_VECTOR_SIZE];
    if (!eg_vector_valid(vector)) return eg_error("inference returned an invalid vector", 0);
    memcpy(data, "EG2VEC01", 8);
    memcpy(data + 8, key, 32);
    crypto_sha256_hash((const unsigned char *)vector, EG2_DIM * 4U, data + 40);
    memcpy(data + 72, vector, EG2_DIM * 4U);
    return eg_atomic_write(path, data, sizeof(data));
}

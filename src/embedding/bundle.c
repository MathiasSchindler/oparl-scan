#include "eg2.h"
#include "crypto/sha256.h"
#include "math/math.h"

#if defined(__APPLE__)
/* LLVM's Darwin backend can emit bzero for aggregate initialization. */
void bzero(void *destination, size_t count) {
    volatile unsigned char *p = destination;
    while (count--) *p++ = 0;
}
#endif

int eg_error(const char *message, const char *detail) {
    tool_write_error("embeddinggemma", message, detail);
    return -1;
}

const unsigned char *eg_bytes(EgReader *r, size_t count) {
    const unsigned char *p;
    if (r->failed || count > r->size - r->offset) {
        r->failed = 1;
        return 0;
    }
    p = r->data + r->offset;
    r->offset += count;
    return p;
}

unsigned int eg_u32(EgReader *r) {
    const unsigned char *p = eg_bytes(r, 4);
    if (!p) return 0;
    return p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
        ((unsigned int)p[3] << 24);
}

float eg_bf16(unsigned int bits) {
    union { unsigned int u; float f; } value;
    value.u = bits << 16;
    return value.f;
}

int eg_read_artifact(const char *path, size_t expected_size, const char *sha256,
                     unsigned char **data, size_t *size) {
    PlatformDirEntry info;
    unsigned char digest[32];
    if (platform_get_path_info(path, &info) || info.is_dir || info.size != expected_size)
        return eg_error("missing artifact or incorrect pinned artifact size: ", path);
    if (tool_read_all_input(path, data, size))
        return eg_error("cannot read artifact: ", path);
    if (*size == expected_size) {
        crypto_sha256_hash(*data, *size, digest);
        unsigned int i;
        for (i = 0; i < 32; i++) {
            unsigned int expected = (unsigned int)(tool_hex_value(sha256[i * 2]) * 16 +
                                                   tool_hex_value(sha256[i * 2 + 1]));
            if (digest[i] != expected) break;
        }
        if (i == 32) return 0;
    }
    rt_free(*data);
    *data = 0;
    *size = 0;
    return eg_error("pinned artifact SHA-256 mismatch: ", path);
}

void eg_model_free(EgModel *m) {
    unsigned int i;
    if (m->pool) {
        rt_task_pool_destroy(m->pool);
        rt_free(m->pool);
    }
    for (i = 0; i < m->count; i++) {
        rt_free(m->tensors[i].values);
        rt_free(m->tensors[i].packed);
    }
    rt_free(m->tensors);
    eg_workspace_free(m->workspace);
    rt_memset(m, 0, sizeof(*m));
}

int eg_model_set_threads(EgModel *m, unsigned int threads) {
    if (!threads || threads > RT_TASK_POOL_MAX_WORKERS || m->pool)
        return eg_error("threads must be 1..32 and configured only once", 0);
    if (threads == 1) return 0;
    if (!platform_worker_threads_supported())
        return eg_error("native worker threads are unavailable on this platform", 0);
    m->pool = rt_malloc(sizeof(*m->pool));
    if (!m->pool) return eg_error("cannot allocate worker pool", 0);
    if (rt_task_pool_init(m->pool, threads)) {
        rt_task_pool_destroy(m->pool);
        rt_free(m->pool);
        m->pool = 0;
        return eg_error("cannot start requested worker threads", 0);
    }
    return 0;
}

int eg_model_load(EgModel *m, const char *path) {
    unsigned char *data = 0;
    size_t size = 0;
    EgReader r;
    unsigned int i;
    rt_memset(m, 0, sizeof(*m));
    if (eg_read_artifact(path, 542024912,
            EG2_WEIGHTS_SHA256,
            &data, &size)) return -1;
    r = (EgReader){data, size, 0, 0};
    const unsigned char *magic = eg_bytes(&r, 8);
    if (!magic || memcmp(magic, "EG2WGT01", 8)) goto invalid;
    m->count = eg_u32(&r);
    if (m->count != 413U) goto invalid;
    m->tensors = rt_malloc_array(m->count, sizeof(*m->tensors));
    if (!m->tensors) goto invalid;
    rt_memset(m->tensors, 0, m->count * sizeof(*m->tensors));
    for (i = 0; i < m->count; i++) {
        EgTensor *t = m->tensors + i;
        unsigned int length = eg_u32(&r), j;
        const unsigned char *name = eg_bytes(&r, length), *raw;
        if (!name || !length || length >= sizeof(t->name) ||
            rt_utf8_validate((const char *)name, length)) goto invalid;
        for (j = 0; j < length; j++) if (!name[j]) goto invalid;
        memcpy(t->name, name, length);
        for (j = 0; j < i; j++) if (!rt_strcmp(t->name, m->tensors[j].name)) goto invalid;
        t->rows = eg_u32(&r);
        t->cols = eg_u32(&r);
        if (!t->rows || !t->cols || t->rows > EG2_VOCAB || t->cols > 2048U ||
            (size_t)t->rows * t->cols > (size_t)EG2_VOCAB * EG2_HIDDEN) goto invalid;
        size_t count = (size_t)t->rows * t->cols;
        raw = eg_bytes(&r, count * 2);
        if (!raw) goto invalid;
        t->values = rt_malloc_array(count, sizeof(float));
        if (!t->values) goto invalid;
        for (size_t k = 0; k < count; k++) {
            t->values[k] = eg_bf16(raw[k * 2] | ((unsigned int)raw[k * 2 + 1] << 8));
            if (!math_is_finite(t->values[k])) goto invalid;
        }
    }

    if (r.failed || r.offset != r.size || eg_workspace_init(m)) goto invalid;
    m->sme = eg_sme_available();
    if (m->sme < 0) goto invalid;
    rt_free(data);
    return 0;
invalid:
    rt_free(data);
    if (m->tensors) eg_model_free(m);
    else m->count = 0;
    return eg_error("invalid, unsupported or unallocatable weight bundle: ", path);
}

const float *eg_weight(const EgModel *m, const char *name, unsigned int rows, unsigned int cols) {
    for (unsigned int i = 0; i < m->count; i++) {
        const EgTensor *t = m->tensors + i;
        if (!rt_strcmp(t->name, name))
            return t->rows == rows && t->cols == cols ? t->values : 0;
    }
    return 0;
}

int eg_prepare_sme(const EgModel *m) {
#if EG2_HAVE_SME
    if (!m->sme) return 0;
    for (unsigned int i = 0; i < m->count; i++) {
        EgTensor *t = m->tensors + i;
        if (t->packed || t->cols < 512 || t->rows % 16 ||
            !rt_strcmp(t->name, "embed_tokens.weight")) continue;
        size_t count = (size_t)t->rows * t->cols;
        float *packed = rt_malloc_array(count, sizeof(float));
        if (!packed) return eg_error("cannot allocate SME weight panels: ", t->name);
        for (unsigned int row = 0; row < t->rows; row += 16)
            for (unsigned int k = 0; k < t->cols; k++)
                for (unsigned int j = 0; j < 16; j++)
                    packed[(size_t)row * t->cols + k * 16 + j] =
                        t->values[(size_t)(row + j) * t->cols + k];
        t->packed = packed;
    }
#else
    if (m->sme) return eg_error("SME backend is not compiled for this target", 0);
#endif
    return 0;
}

const float *eg_packed_weight(const EgModel *m, const float *w, unsigned int input,
                              unsigned int output) {
#if EG2_HAVE_SME
    if (!m || !m->sme || !input || input > 2048 || output % 16) return 0;
    for (unsigned int i = 0; i < m->count; i++) {
        const EgTensor *t = m->tensors + i;
        size_t address = (size_t)w, base = (size_t)t->values;
        size_t bytes = (size_t)t->rows * t->cols * sizeof(float);
        if (t->packed && input == t->cols && address >= base &&
            address - base < bytes && !((address - base) % (16 * input * sizeof(float))) &&
            (size_t)output * input * sizeof(float) <= bytes - (address - base))
            return t->packed + (address - base) / sizeof(float);
    }
#else
    (void)m; (void)w; (void)input; (void)output;
#endif
    return 0;
}

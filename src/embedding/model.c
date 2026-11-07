#include "eg2.h"
#include "math/math.h"

struct EgWorkspace {
    float *buffers[16], *scores;
    unsigned int capacity, score_capacity, rope_tokens;
};

int eg_workspace_init(EgModel *m) {
    m->workspace = rt_malloc(sizeof(*m->workspace));
    if (!m->workspace) return eg_error("cannot allocate model workspace state", 0);
    rt_memset(m->workspace, 0, sizeof(*m->workspace));
    return 0;
}

void eg_workspace_free(EgWorkspace *workspace) {
    if (!workspace) return;
    for (unsigned int i = 0; i < 16; i++) rt_free(workspace->buffers[i]);
    rt_free(workspace->scores);
    rt_free(workspace);
}

#define MEASURE(field, ...) do { \
    unsigned long long measure_start = profile ? platform_get_monotonic_time_ns() : 0; \
    __VA_ARGS__; \
    if (profile) profile->field += platform_get_monotonic_time_ns() - measure_start; \
} while (0)

#define LINEAR(...) do { \
    MEASURE(linear_ns, if (eg_linear_run(m, __VA_ARGS__)) goto done); \
} while (0)

#define NORM(...) do { \
    MEASURE(norm_ns, if (eg_norm_run(m, __VA_ARGS__)) goto done); \
} while (0)

typedef struct {
    const float *x, *w;
    float *out;
    unsigned int width;
} NormWork;

static int norm_body(size_t begin, size_t end, unsigned int worker, void *arg) {
    const NormWork *work = arg;
    const float *x = work->x, *w = work->w;
    float *out = work->out;
    unsigned int width = work->width;
    (void)worker;
    for (size_t t = begin; t < end; t++) {
        const float *row = x + (size_t)t * width;
        float squared = eg_dot(row, row, width);
        double mean = squared / (float)width + 1e-6f;
#if defined(__clang__)
        float scale = (float)(1.0 / __builtin_elementwise_sqrt(mean));
#else
        float scale = (float)(1.0 / math_sqrt(mean));
#endif
        for (unsigned int i = 0; i < width; i++) {
            float v = x[(size_t)t * width + i] * scale;
            out[(size_t)t * width + i] = w ? v * w[i] : v;
        }
    }
    return 0;
}

void eg_norm(const float *x, const float *w, float *out, unsigned int count, unsigned int width) {
    NormWork work = {x, w, out, width};
    (void)norm_body(0, count, 0, &work);
}

int eg_norm_run(const EgModel *m, const float *x, const float *w, float *out,
                unsigned int count, unsigned int width) {
    if (!width) return eg_error("invalid normalization width", 0);
    NormWork work = {x, w, out, width};
    if (m && m->pool && count >= 512) {
        if (eg_parallel_for(m, "normalization", count, 32, norm_body, &work))
            return eg_error("parallel normalization failed", 0);
        return 0;
    }
    return norm_body(0, count, 0, &work);
}

static const float *layer_weight(const EgModel *m, unsigned int layer,
                                const char *suffix, unsigned int rows, unsigned int cols) {
    char name[128], number[16];
    rt_unsigned_to_string(layer, number, sizeof(number));
    rt_copy_string(name, sizeof(name), "layers.");
    size_t n = rt_strlen(name);
    rt_copy_string(name + n, sizeof(name) - n, number);
    n = rt_strlen(name);
    rt_copy_string(name + n, sizeof(name) - n, ".");
    n = rt_strlen(name);
    rt_copy_string(name + n, sizeof(name) - n, suffix);
    const float *p = eg_weight(m, name, rows, cols);
    if (!p) eg_error("missing tensor or incompatible shape: ", name);
    return p;
}

static void add(float *x, const float *y, size_t count) {
    for (size_t i = 0; i < count; i++) x[i] += y[i];
}

static int trace_values(const char *prefix, const char *suffix, const float *values, size_t count) {
    char path[4096];
    if (!prefix) return 0;
    size_t n = rt_strlen(prefix), s = rt_strlen(suffix);
    if (n + s >= sizeof(path)) return eg_error("trace path too long", 0);
    memcpy(path, prefix, n);
    memcpy(path + n, suffix, s + 1);
    if (tool_write_file_all(path, (const unsigned char *)values, count * sizeof(float)))
        return eg_error("cannot write tensor trace: ", path);
    return 0;
}

static void rope_table(float *cosine, float *sine, unsigned int tokens, unsigned int dim, float base) {
    float inverse[256];
    for (unsigned int i = 0; i < dim / 2; i++) {
        float power = (float)math_pow(base, (float)(i * 2) / (float)dim);
        inverse[i] = 1.0f / power;
    }
    for (unsigned int t = 0; t < tokens; t++) {
        for (unsigned int i = 0; i < dim / 2; i++) {
            float angle = (float)t * inverse[i];
            cosine[(size_t)t * (dim / 2) + i] = (float)math_cos(angle);
            sine[(size_t)t * (dim / 2) + i] = (float)math_sin(angle);
        }
    }
}

static void rope(float *restrict x, unsigned int tokens, unsigned int heads, unsigned int dim,
                 const float *restrict cosine, const float *restrict sine) {
    for (unsigned int t = 0; t < tokens; t++) {
        const float *c = cosine + (size_t)t * (dim / 2), *s = sine + (size_t)t * (dim / 2);
        for (unsigned int h = 0; h < heads; h++) {
            float *p = x + ((size_t)t * heads + h) * dim;
            for (unsigned int i = 0; i < dim / 2; i++) {
                float a = p[i], b = p[i + dim / 2];
                p[i] = a * c[i] - b * s[i];
                p[i + dim / 2] = b * c[i] + a * s[i];
            }
        }
    }
}

int eg_embed(const EgModel *m, const unsigned int *tokens, unsigned int count,
             float output[EG2_DIM], const char *trace) {
    return eg_embed_profiled(m, tokens, count, output, trace, 0);
}

static int embed_many(const EgModel *m, const unsigned int *tokens, const unsigned int *counts,
                      unsigned int documents, float *output, const char *trace, EgProfile *profile) {
    enum { HIDDEN, INPUT, NORMAL, TEMP, QUERY, KEY, VALUE, ATTN, GATE, UP, PLE, PROJECTED,
           LOCAL_COS, LOCAL_SIN, GLOBAL_COS, GLOBAL_SIN, BUFFERS };
    const unsigned int widths[BUFFERS] = {512, 512, 512, 512, 2048, 512, 512, 2048,
                                         2048, 2048, 512, 768, 128, 128, 256, 256};
    float *b[BUFFERS] = {0}, *scores = 0;
    int status = -1;
    unsigned long long started = profile ? platform_get_monotonic_time_ns() : 0;
    if (profile) rt_memset(profile, 0, sizeof(*profile));
    if (!m || !tokens || !counts || !output || !documents || documents > EG2_MAX_BATCH)
        return eg_error("invalid embedding batch", 0);
    unsigned int count = 0, maximum_count = 0;
    for (unsigned int d = 0; d < documents; d++) {
        if (!counts[d] || counts[d] > EG2_MAX_TOKENS - count)
            return eg_error("invalid token count or batch exceeds workspace limit", 0);
        count += counts[d];
        if (counts[d] > maximum_count) maximum_count = counts[d];
    }
    for (unsigned int i = 0; i < count; i++)
        if (tokens[i] >= EG2_VOCAB) return eg_error("token ID outside model vocabulary", 0);
    const float *embedding = eg_weight(m, "embed_tokens.weight", EG2_VOCAB, 512);
    const float *projection = eg_weight(m, "embedding_projection.weight", 768, 512);
    const float *norm = eg_weight(m, "norm.weight", 512, 1);
    const float *ple = eg_weight(m, "ple.per_layer_model_projection.weight", 12288, 512);
    const float *ple_norm = eg_weight(m, "ple.per_layer_projection_norm.weight", 512, 1);
    if (!embedding || !projection || !norm || !ple || !ple_norm)
        return eg_error("missing global tensor or incompatible shape", 0);
    if (count >= 64 && eg_prepare_sme(m)) goto done;
    EgWorkspace *workspace = m->workspace;
    if (!workspace) return eg_error("model workspace is unavailable", 0);
    if (count > workspace->capacity) {
        unsigned int capacity = 32;
        while (capacity < count) capacity *= 2;
        float *next[BUFFERS] = {0};
        for (unsigned int i = 0; i < BUFFERS; i++) {
            next[i] = rt_malloc_array((size_t)capacity * widths[i], sizeof(float));
            if (!next[i]) {
                for (unsigned int j = 0; j < BUFFERS; j++) rt_free(next[j]);
                return eg_error("cannot grow model workspace", 0);
            }
        }
        for (unsigned int i = 0; i < BUFFERS; i++) {
            rt_free(workspace->buffers[i]);
            workspace->buffers[i] = next[i];
        }
        workspace->capacity = capacity;
        workspace->rope_tokens = 0;
    }
    unsigned int score_count = maximum_count * 4 * rt_task_pool_width(m->pool);
    if (score_count > workspace->score_capacity) {
        unsigned int capacity = 128;
        while (capacity < score_count) capacity *= 2;
        float *next = rt_malloc_array(capacity, sizeof(float));
        if (!next) return eg_error("cannot grow attention workspace", 0);
        rt_free(workspace->scores);
        workspace->scores = next;
        workspace->score_capacity = capacity;
    }
    for (unsigned int i = 0; i < BUFFERS; i++) b[i] = workspace->buffers[i];
    scores = workspace->scores;
    float embed_scale = (float)math_sqrt(512);
    for (unsigned int t = 0; t < count; t++)
        for (unsigned int c = 0; c < 512; c++)
            b[INPUT][(size_t)t * 512 + c] = embedding[(size_t)tokens[t] * 512 + c] * embed_scale;
    memcpy(b[HIDDEN], b[INPUT], (size_t)count * 512 * sizeof(float));
    if (trace_values(trace, ".input.f32", b[INPUT], (size_t)count * 512)) goto done;
    MEASURE(rope_ns,
        if (maximum_count > workspace->rope_tokens) {
            unsigned int capacity = 32;
            while (capacity < maximum_count) capacity *= 2;
            rope_table(b[LOCAL_COS], b[LOCAL_SIN], capacity, 256, 10000);
            rope_table(b[GLOBAL_COS], b[GLOBAL_SIN], capacity, 512, 1000000);
            workspace->rope_tokens = capacity;
        });
    for (unsigned int l = 0; l < 24; l++) {
        int local = l % 6 != 5;
        unsigned int dim = local ? 256 : 512, kv = local ? 2 : 1;
        const float *in = layer_weight(m, l, "input_layernorm.weight", 512, 1);
        const float *post = layer_weight(m, l, "post_attention_layernorm.weight", 512, 1);
        const float *pre_ff = layer_weight(m, l, "pre_feedforward_layernorm.weight", 512, 1);
        const float *post_ff = layer_weight(m, l, "post_feedforward_layernorm.weight", 512, 1);
        const float *qw = layer_weight(m, l, "self_attn.q_proj.weight", 4 * dim, 512);
        const float *kw = layer_weight(m, l, "self_attn.k_proj.weight", kv * dim, 512);
        const float *vw = layer_weight(m, l, "self_attn.v_proj.weight", kv * dim, 512);
        const float *ow = layer_weight(m, l, "self_attn.o_proj.weight", 512, 4 * dim);
        const float *qn = layer_weight(m, l, "self_attn.q_norm.weight", dim, 1);
        const float *kn = layer_weight(m, l, "self_attn.k_norm.weight", dim, 1);
        const float *gate = layer_weight(m, l, "mlp.gate_proj.weight", 2048, 512);
        const float *up = layer_weight(m, l, "mlp.up_proj.weight", 2048, 512);
        const float *down = layer_weight(m, l, "mlp.down_proj.weight", 512, 2048);
        const float *pgate = layer_weight(m, l, "ple_block.per_layer_input_gate.weight", 512, 512);
        const float *pproj = layer_weight(m, l, "ple_block.per_layer_projection.weight", 512, 512);
        const float *pnorm = layer_weight(m, l, "ple_block.post_per_layer_input_norm.weight", 512, 1);
        const float *scalar = layer_weight(m, l, "layer_scalar", 1, 1);
        if (!in || !post || !pre_ff || !post_ff || !qw || !kw || !vw || !ow || !qn ||
            !kn || !gate || !up || !down || !pgate || !pproj || !pnorm || !scalar) goto done;
        NORM(b[HIDDEN], in, b[NORMAL], count, 512);
        EgLinearJob qkv[] = {{b[NORMAL], qw, b[QUERY], 512, 4 * dim},
                             {b[NORMAL], kw, b[KEY], 512, kv * dim},
                             {b[NORMAL], vw, b[VALUE], 512, kv * dim}};
        MEASURE(linear_ns, if (eg_linear_group(m, qkv, 3, count)) goto done);
        NORM(b[QUERY], qn, b[QUERY], count * 4, dim);
        NORM(b[KEY], kn, b[KEY], count * kv, dim);
        NORM(b[VALUE], 0, b[VALUE], count * kv, dim);
        MEASURE(rope_ns,
            unsigned int offset = 0;
            for (unsigned int d = 0; d < documents; d++) {
                rope(b[QUERY] + (size_t)offset * 4 * dim, counts[d], 4, dim,
                     b[local ? LOCAL_COS : GLOBAL_COS], b[local ? LOCAL_SIN : GLOBAL_SIN]);
                rope(b[KEY] + (size_t)offset * kv * dim, counts[d], kv, dim,
                     b[local ? LOCAL_COS : GLOBAL_COS], b[local ? LOCAL_SIN : GLOBAL_SIN]);
                offset += counts[d];
            });
        MEASURE(attention_ns,
            if (eg_attention_batch(m, b[QUERY], b[KEY], b[VALUE], b[ATTN], scores,
                                   counts, documents, dim, kv, local)) goto done);
        LINEAR(b[ATTN], ow, b[TEMP], count, 4 * dim, 512);
        NORM(b[TEMP], post, b[TEMP], count, 512);
        add(b[HIDDEN], b[TEMP], (size_t)count * 512);
        NORM(b[HIDDEN], pre_ff, b[NORMAL], count, 512);
        EgLinearJob feedforward[] = {{b[NORMAL], gate, b[GATE], 512, 2048},
                                     {b[NORMAL], up, b[UP], 512, 2048}};
        MEASURE(linear_ns, if (eg_linear_group(m, feedforward, 2, count)) goto done);
        MEASURE(activation_ns,
            if (eg_activate(m, b[GATE], b[UP], (size_t)count * 2048)) goto done);
        LINEAR(b[GATE], down, b[TEMP], count, 2048, 512);
        NORM(b[TEMP], post_ff, b[TEMP], count, 512);
        add(b[HIDDEN], b[TEMP], (size_t)count * 512);
        EgLinearJob per_layer[] = {{b[INPUT], ple + (size_t)l * 512 * 512, b[PLE], 512, 512},
                                  {b[HIDDEN], pgate, b[NORMAL], 512, 512}};
        MEASURE(linear_ns, if (eg_linear_group(m, per_layer, 2, count)) goto done);
        float ple_scale = (float)math_pow(512, -0.5);
        for (size_t i = 0; i < (size_t)count * 512; i++) b[PLE][i] *= ple_scale;
        NORM(b[PLE], ple_norm, b[PLE], count, 512);
        MEASURE(activation_ns,
            if (eg_activate(m, b[NORMAL], b[PLE], (size_t)count * 512)) goto done);
        LINEAR(b[NORMAL], pproj, b[TEMP], count, 512, 512);
        NORM(b[TEMP], pnorm, b[TEMP], count, 512);
        add(b[HIDDEN], b[TEMP], (size_t)count * 512);
        for (size_t i = 0; i < (size_t)count * 512; i++) b[HIDDEN][i] *= *scalar;
        if (trace) {
            char suffix[] = ".layer00.f32";
            suffix[6] = (char)('0' + l / 10);
            suffix[7] = (char)('0' + l % 10);
            if (trace_values(trace, suffix, b[HIDDEN], (size_t)count * 512)) goto done;
        }
    }
    NORM(b[HIDDEN], norm, b[NORMAL], count, 512);
    LINEAR(b[NORMAL], projection, b[PROJECTED], count, 512, EG2_DIM);
    if (trace_values(trace, ".tokens.f32", b[PROJECTED], (size_t)count * EG2_DIM)) goto done;
    unsigned int offset = 0;
    for (unsigned int d = 0; d < documents; d++) {
        float squared = 0;
        float *vector = output + (size_t)d * EG2_DIM;
        for (unsigned int c = 0; c < EG2_DIM; c++) {
            float sum = 0;
            for (unsigned int t = 0; t < counts[d]; t++)
                sum += b[PROJECTED][(size_t)(offset + t) * EG2_DIM + c];
            vector[c] = sum / (float)counts[d];
            squared += vector[c] * vector[c];
        }
        float magnitude = (float)math_sqrt(squared);
        if (!math_is_finite(magnitude) || magnitude <= 0) {
            eg_error("non-finite or zero embedding norm", 0); goto done;
        }
        for (unsigned int c = 0; c < EG2_DIM; c++) {
            vector[c] /= magnitude;
            if (!math_is_finite(vector[c])) { eg_error("non-finite embedding", 0); goto done; }
        }
        offset += counts[d];
    }
    status = 0;
done:
    if (profile) profile->total_ns = platform_get_monotonic_time_ns() - started;
    return status;
}

int eg_embed_profiled(const EgModel *m, const unsigned int *tokens, unsigned int count,
                      float output[EG2_DIM], const char *trace, EgProfile *profile) {
    return embed_many(m, tokens, &count, 1, output, trace, profile);
}

int eg_embed_batch_profiled(const EgModel *m, const unsigned int *tokens,
                            const unsigned int *counts, unsigned int documents,
                            float *output, EgProfile *profile) {
    return embed_many(m, tokens, counts, documents, output, 0, profile);
}

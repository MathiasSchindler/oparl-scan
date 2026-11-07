#include "eg2.h"
#include "math/math.h"

/* exp(r), |r| <= log(2)/2: degree 12 keeps the error below FP32 rounding. */
#define EXP_POLYNOMIAL(r) \
    (1.0 + (r) * (1.0 + (r) * (0.5 + (r) * (0.16666666666666666667 + \
    (r) * (0.041666666666666666667 + (r) * (0.0083333333333333333333 + \
    (r) * (0.0013888888888888888889 + (r) * (0.00019841269841269841270 + \
    (r) * (0.000024801587301587301587 + (r) * (0.0000027557319223985890653 + \
    (r) * (0.00000027557319223985890653 + (r) * (0.000000025052108385441718775 + \
    (r) * 0.0000000020876756987868098979))))))))))))

static double exp_reduced(double x) {
    int n = (int)(x * 1.4426950408889634074 + (x < 0 ? -0.5 : 0.5));
    double r = x - (double)n * 0.69314718055994530942, scale;
    unsigned long long bits = (unsigned long long)(n + 1023) << 52;
    memcpy(&scale, &bits, sizeof(scale));
    return EXP_POLYNOMIAL(r) * scale;
}

float eg_exp(float x) {
    if (x < -104.0f) return 0;
    if (!(x <= 88.0f)) return (float)math_exp(x);
    return (float)exp_reduced(x);
}

static double tanh_reduced(double x) {
    if (x > 10) return 1;
    if (x < -10) return -1;
    if (x != x) return x;
    double e = exp_reduced(2 * x);
    return (e - 1) / (e + 1);
}

float eg_gelu(float x) {
    float cube = x * x * x;
    float inner = 0.7978845608028654f * (x + 0.044715f * cube);
    return 0.5f * x * (1.0f + (float)tanh_reduced(inner));
}

#if defined(__clang__) || defined(__GNUC__)
typedef float Float4 __attribute__((vector_size(16), aligned(4)));
typedef double Double2 __attribute__((vector_size(16)));
typedef int Int2 __attribute__((vector_size(8)));

#if defined(__clang__) && defined(__aarch64__) && !defined(EG2_DISABLE_FMA)
#define MADD(a, b, c) (fused ? __builtin_elementwise_fma(a, b, c) : (c) + (a) * (b))
#else
#define MADD(a, b, c) ((c) + (a) * (b))
#endif

static Double2 exp_pair(Double2 x) {
    if (!(x[0] >= -104 && x[0] <= 88 && x[1] >= -104 && x[1] <= 88))
        return (Double2){eg_exp((float)x[0]), eg_exp((float)x[1])};
    Double2 rounded = x * 1.4426950408889634074 +
        (Double2){x[0] < 0 ? -0.5 : 0.5, x[1] < 0 ? -0.5 : 0.5};
    Int2 n = __builtin_convertvector(rounded, Int2);
    Double2 r = x - __builtin_convertvector(n, Double2) * 0.69314718055994530942;
    unsigned long long bits[2] = {(unsigned long long)(n[0] + 1023) << 52,
                                 (unsigned long long)(n[1] + 1023) << 52};
    Double2 scale;
    memcpy(&scale, bits, sizeof(scale));
    return EXP_POLYNOMIAL(r) * scale;
}

static Double2 tanh_pair(Double2 x) {
    if (!(x[0] >= -10 && x[0] <= 10 && x[1] >= -10 && x[1] <= 10))
        return (Double2){tanh_reduced(x[0]), tanh_reduced(x[1])};
    Double2 e = exp_pair(2 * x);
    return (e - 1) / (e + 1);
}

static Float4 exp4(Float4 x) {
    Double2 a = exp_pair((Double2){x[0], x[1]}), b = exp_pair((Double2){x[2], x[3]});
    return (Float4){(float)a[0], (float)a[1], (float)b[0], (float)b[1]};
}

static Float4 gelu4(Float4 x) {
    Float4 cube = x * x * x;
    Float4 inner = 0.7978845608028654f * (x + 0.044715f * cube);
    Double2 a = tanh_pair((Double2){inner[0], inner[1]});
    Double2 b = tanh_pair((Double2){inner[2], inner[3]});
    Float4 t = {(float)a[0], (float)a[1], (float)b[0], (float)b[1]};
    return 0.5f * x * (1.0f + t);
}

static float horizontal(Float4 x) {
    return (x[0] + x[1]) + (x[2] + x[3]);
}
#endif

int eg_softmax(float *scores, unsigned int count, float maximum) {
    if (!count || !math_is_finite(maximum)) return eg_error("invalid softmax input", 0);
    unsigned int i = 0;
    float total = 0;
#if defined(__clang__) || defined(__GNUC__)
    Float4 max4 = {maximum, maximum, maximum, maximum};
    for (; i + 4 <= count; i += 4) {
        Float4 values = exp4(*(const Float4 *)(scores + i) - max4);
        *(Float4 *)(scores + i) = values;
        total += values[0]; total += values[1]; total += values[2]; total += values[3];
    }
#endif
    for (; i < count; i++) {
        scores[i] = eg_exp(scores[i] - maximum);
        total += scores[i];
    }
    if (!(total > 0) || !math_is_finite(total)) return eg_error("invalid softmax normalization", 0);
    for (i = 0; i < count; i++) scores[i] /= total;
    return 0;
}

float eg_dot(const float *a, const float *b, unsigned int count) {
    unsigned int c = 0;
    float sum = 0;
#if defined(__clang__) || defined(__GNUC__)
    Float4 partial = {0, 0, 0, 0};
    for (; c + 4 <= count; c += 4)
        partial += *(const Float4 *)(a + c) * *(const Float4 *)(b + c);
    sum = horizontal(partial);
#endif
    for (; c < count; c++) sum += a[c] * b[c];
    return sum;
}

typedef struct {
    const float *x, *w;
    float *out;
    unsigned int tokens, input, output, row_tiles;
    const float *packed;
    unsigned int tile_rows;
} LinearWork;

#if defined(__clang__) || defined(__GNUC__)
static inline __attribute__((always_inline)) void tile4(
                  const float *a0, const float *b0, float *out, unsigned int input,
                  size_t a_stride, size_t b_stride, size_t out_stride, int fused) {
    (void)fused;
    const float *a1 = a0 + a_stride, *a2 = a1 + a_stride, *a3 = a2 + a_stride;
    const float *b1 = b0 + b_stride, *b2 = b1 + b_stride, *b3 = b2 + b_stride;
    Float4 s00 = {0}, s01 = {0}, s02 = {0}, s03 = {0};
    Float4 s10 = {0}, s11 = {0}, s12 = {0}, s13 = {0};
    Float4 s20 = {0}, s21 = {0}, s22 = {0}, s23 = {0};
    Float4 s30 = {0}, s31 = {0}, s32 = {0}, s33 = {0};
    unsigned int c = 0;
    for (; c + 4 <= input; c += 4) {
        Float4 x0 = *(const Float4 *)(a0 + c), x1 = *(const Float4 *)(a1 + c);
        Float4 x2 = *(const Float4 *)(a2 + c), x3 = *(const Float4 *)(a3 + c);
        Float4 y0 = *(const Float4 *)(b0 + c), y1 = *(const Float4 *)(b1 + c);
        Float4 y2 = *(const Float4 *)(b2 + c), y3 = *(const Float4 *)(b3 + c);
        s00 = MADD(x0, y0, s00); s01 = MADD(x0, y1, s01); s02 = MADD(x0, y2, s02); s03 = MADD(x0, y3, s03);
        s10 = MADD(x1, y0, s10); s11 = MADD(x1, y1, s11); s12 = MADD(x1, y2, s12); s13 = MADD(x1, y3, s13);
        s20 = MADD(x2, y0, s20); s21 = MADD(x2, y1, s21); s22 = MADD(x2, y2, s22); s23 = MADD(x2, y3, s23);
        s30 = MADD(x3, y0, s30); s31 = MADD(x3, y1, s31); s32 = MADD(x3, y2, s32); s33 = MADD(x3, y3, s33);
    }
    const Float4 sums[4][4] = {
        {s00, s01, s02, s03}, {s10, s11, s12, s13},
        {s20, s21, s22, s23}, {s30, s31, s32, s33},
    };
    for (unsigned int i = 0; i < 4; i++) {
        for (unsigned int j = 0; j < 4; j++) {
            float value = horizontal(sums[i][j]);
            for (unsigned int k = c; k < input; k++)
                value += a0[(size_t)i * a_stride + k] * b0[(size_t)j * b_stride + k];
            out[(size_t)i * out_stride + j] = value;
        }
    }
}
#endif

#if EG2_HAVE_SME
static void pack_panel(const float *source, float *out, unsigned int rows, unsigned int input) {
    if (rows < 16) rt_memset(out, 0, (size_t)16 * input * sizeof(float));
    unsigned int r = 0;
    for (; r + 4 <= rows; r += 4) {
        const float *x = source + (size_t)r * input;
        unsigned int k = 0;
        for (; k + 4 <= input; k += 4) {
            Float4 a = *(const Float4 *)(x + k), b = *(const Float4 *)(x + input + k);
            Float4 c = *(const Float4 *)(x + (size_t)2 * input + k);
            Float4 d = *(const Float4 *)(x + (size_t)3 * input + k);
            /* Transpose a 4x4 tile into the channel-major SME input panel. */
            Float4 ab = __builtin_shufflevector(a, b, 0, 4, 1, 5);
            Float4 cd = __builtin_shufflevector(c, d, 0, 4, 1, 5);
            Float4 ef = __builtin_shufflevector(a, b, 2, 6, 3, 7);
            Float4 gh = __builtin_shufflevector(c, d, 2, 6, 3, 7);
            *(Float4 *)(out + (size_t)k * 16 + r) = __builtin_shufflevector(ab, cd, 0, 1, 4, 5);
            *(Float4 *)(out + (size_t)(k + 1) * 16 + r) = __builtin_shufflevector(ab, cd, 2, 3, 6, 7);
            *(Float4 *)(out + (size_t)(k + 2) * 16 + r) = __builtin_shufflevector(ef, gh, 0, 1, 4, 5);
            *(Float4 *)(out + (size_t)(k + 3) * 16 + r) = __builtin_shufflevector(ef, gh, 2, 3, 6, 7);
        }
        for (; k < input; k++)
            for (unsigned int i = 0; i < 4; i++)
                out[(size_t)k * 16 + r + i] = x[(size_t)i * input + k];
    }
    for (; r < rows; r++)
        for (unsigned int k = 0; k < input; k++)
            out[(size_t)k * 16 + r] = source[(size_t)r * input + k];
}

static int linear_sme_body(size_t begin, size_t end, const LinearWork *w) {
    float a[2048 * 16], result[16 * 16];
    unsigned int previous = EG2_NONE;
    for (size_t tile = begin; tile < end; tile++) {
        unsigned int t = (unsigned int)(tile / w->row_tiles) * 16;
        unsigned int r = (unsigned int)(tile % w->row_tiles) * 16;
        unsigned int rows = w->tokens - t < 16 ? w->tokens - t : 16;
        unsigned int cols = w->output - r < 16 ? w->output - r : 16;
        rt_memset(result, 0, sizeof(result));
        if (t != previous) {
            pack_panel(w->x + (size_t)t * w->input, a, rows, w->input);
            previous = t;
        }
        if (eg_sme_block(a, w->packed + (size_t)r * w->input, result, w->input))
            return eg_error("SME requires private ZA state and 512-bit streaming vectors", 0);
        for (unsigned int i = 0; i < rows; i++) {
            float *target = w->out + (size_t)(t + i) * w->output + r;
            const float *source = result + i * 16;
            unsigned int j = 0;
#if defined(__clang__) || defined(__GNUC__)
            for (; j + 4 <= cols; j += 4)
                *(Float4 *)(target + j) = *(const Float4 *)(source + j);
#endif
            for (; j < cols; j++) target[j] = source[j];
        }
    }
    return 0;
}
#endif

static float linear_dot(const float *a, const float *b, unsigned int count) {
#if defined(__clang__) && defined(__aarch64__) && !defined(EG2_DISABLE_FMA)
    unsigned int c = 0;
    Float4 partial = {0};
    for (; c + 4 <= count; c += 4)
        partial = __builtin_elementwise_fma(*(const Float4 *)(a+c), *(const Float4 *)(b+c), partial);
    float sum = horizontal(partial);
    for (; c < count; c++) sum += a[c] * b[c];
    return sum;
#else
    return eg_dot(a, b, count);
#endif
}

static LinearWork linear_work(const EgModel *m, const float *x, const float *w, float *out,
                               unsigned int tokens, unsigned int input, unsigned int output) {
    const float *packed = tokens >= 64 ? eg_packed_weight(m, w, input, output) : 0;
    unsigned int rows = packed ? 16 : 4, cols = packed ? 16 : 4;
    return (LinearWork){x, w, out, tokens, input, output, (output+cols-1)/cols, packed, rows};
}

static int linear_body(size_t begin, size_t end, unsigned int worker, void *arg) {
    LinearWork *w = arg;
    (void)worker;
#if EG2_HAVE_SME
    if (w->packed) return linear_sme_body(begin, end, w);
#endif
    for (size_t tile = begin; tile < end; tile++) {
        unsigned int t = (unsigned int)(tile / w->row_tiles) * 4;
        unsigned int r = (unsigned int)(tile % w->row_tiles) * 4;
#if defined(__clang__) || defined(__GNUC__)
        if (t + 4 <= w->tokens && r + 4 <= w->output) {
            tile4(w->x + (size_t)t * w->input, w->w + (size_t)r * w->input,
                  w->out + (size_t)t * w->output + r, w->input,
                  w->input, w->input, w->output, 1);
            continue;
        }
#endif
        unsigned int t_end = t + 4 < w->tokens ? t + 4 : w->tokens;
        unsigned int r_end = r + 4 < w->output ? r + 4 : w->output;
        for (unsigned int i = t; i < t_end; i++)
            for (unsigned int j = r; j < r_end; j++)
                w->out[(size_t)i * w->output + j] =
                    linear_dot(w->x + (size_t)i * w->input, w->w + (size_t)j * w->input, w->input);
    }
    return 0;
}

int eg_linear_run(const EgModel *m, const float *x, const float *w, float *out,
                  unsigned int tokens, unsigned int input, unsigned int output) {
    LinearWork work = linear_work(m, x, w, out, tokens, input, output);
    size_t tiles = (size_t)((tokens + work.tile_rows - 1) / work.tile_rows) * work.row_tiles;
    if (!tiles) return 0;
    if (m && m->pool && tokens >= 8) {
        if (eg_parallel_for(m, "linear", tiles, work.packed ? 1 : 32, linear_body, &work))
            return eg_error("parallel linear projection failed", 0);
        return 0;
    }
    return linear_body(0, tiles, 0, &work);
}

void eg_linear(const float *x, const float *w, float *out,
               unsigned int tokens, unsigned int input, unsigned int output) {
    (void)eg_linear_run(0, x, w, out, tokens, input, output);
}

typedef struct {
    LinearWork work[3];
    size_t offsets[4];
    unsigned int count;
} LinearGroup;

static int linear_group_body(size_t begin, size_t end, unsigned int worker, void *arg) {
    LinearGroup *group = arg;
    for (unsigned int i = 0; i < group->count; i++) {
        size_t first = group->offsets[i], last = group->offsets[i + 1];
        if (begin < last && end > first &&
            linear_body((begin > first ? begin : first) - first,
                        (end < last ? end : last) - first, worker, group->work + i)) return -1;
    }
    return 0;
}

int eg_linear_group(const EgModel *m, const EgLinearJob *jobs, unsigned int count,
                    unsigned int tokens) {
    if (!count || count > 3) return eg_error("invalid projection group size", 0);
    LinearGroup group = {.count = count};
    int packed = 0;
    for (unsigned int i = 0; i < count; i++) {
        group.work[i] = linear_work(m, jobs[i].x, jobs[i].w, jobs[i].out,
                                   tokens, jobs[i].input, jobs[i].output);
        unsigned int rows = group.work[i].tile_rows;
        if (group.work[i].packed) packed = 1;
        group.offsets[i + 1] = group.offsets[i] +
            (size_t)((tokens + rows - 1) / rows) * group.work[i].row_tiles;
    }
    size_t tiles = group.offsets[count];
    if (!tiles) return 0;
    if (m && m->pool && tokens >= 8) {
        if (eg_parallel_for(m, "linear_group", tiles, packed ? 1 : 32, linear_group_body, &group))
            return eg_error("parallel projection group failed", 0);
        return 0;
    }
    return linear_group_body(0, tiles, 0, &group);
}

typedef struct {
    const float *q, *k, *v;
    float *out, *scores;
    unsigned int tokens, dim, kv_heads;
    int local;
    size_t score_stride;
} AttentionWork;

static void weighted_values(const float *restrict v, const float *scores, float *restrict out,
                            unsigned int begin, unsigned int end, unsigned int dim,
                            unsigned int kv_heads, unsigned int head) {
    unsigned int c = 0;
#if defined(__clang__) || defined(__GNUC__)
    for (; c + 32 <= dim; c += 32) {
        Float4 s0 = {0}, s1 = {0}, s2 = {0}, s3 = {0};
        Float4 s4 = {0}, s5 = {0}, s6 = {0}, s7 = {0};
        for (unsigned int j = begin; j < end; j++) {
            const float *value = v + ((size_t)j * kv_heads + head) * dim + c;
            float score = scores[j];
            s0 += score * *(const Float4 *)(value);
            s1 += score * *(const Float4 *)(value + 4);
            s2 += score * *(const Float4 *)(value + 8);
            s3 += score * *(const Float4 *)(value + 12);
            s4 += score * *(const Float4 *)(value + 16);
            s5 += score * *(const Float4 *)(value + 20);
            s6 += score * *(const Float4 *)(value + 24);
            s7 += score * *(const Float4 *)(value + 28);
        }
        *(Float4 *)(out + c) = s0; *(Float4 *)(out + c + 4) = s1;
        *(Float4 *)(out + c + 8) = s2; *(Float4 *)(out + c + 12) = s3;
        *(Float4 *)(out + c + 16) = s4; *(Float4 *)(out + c + 20) = s5;
        *(Float4 *)(out + c + 24) = s6; *(Float4 *)(out + c + 28) = s7;
    }
#endif
    for (; c < dim; c++) {
        float sum = 0;
        for (unsigned int j = begin; j < end; j++)
            sum += scores[j] * v[((size_t)j * kv_heads + head) * dim + c];
        out[c] = sum;
    }
}

#if defined(__clang__) || defined(__GNUC__)
typedef struct {
    Float4 a, b, c, d;
} ValueTile;

static inline __attribute__((always_inline)) ValueTile value_tile(const float *v) {
    return (ValueTile){*(const Float4 *)v, *(const Float4 *)(v + 4),
                       *(const Float4 *)(v + 8), *(const Float4 *)(v + 12)};
}

static inline __attribute__((always_inline)) void value_add(ValueTile *sum, ValueTile v,
                                                            float score) {
    sum->a += score * v.a; sum->b += score * v.b;
    sum->c += score * v.c; sum->d += score * v.d;
}

static inline __attribute__((always_inline)) void value_span(
        ValueTile *sum, const float *v, const float *scores,
        unsigned int begin, unsigned int end, size_t stride) {
    for (unsigned int j = begin; j < end; j++)
        value_add(sum, value_tile(v + (size_t)j * stride), scores[j]);
}

static inline __attribute__((always_inline)) void value_store(float *out, ValueTile sum) {
    *(Float4 *)out = sum.a; *(Float4 *)(out + 4) = sum.b;
    *(Float4 *)(out + 8) = sum.c; *(Float4 *)(out + 12) = sum.d;
}

static void weighted_values4(const float *restrict v, const float *scores,
                             float *restrict out, const unsigned int *begin,
                             const unsigned int *end, unsigned int tokens,
                             unsigned int dim, unsigned int kv_heads, unsigned int head) {
    size_t stride = (size_t)kv_heads * dim, out_stride = (size_t)4 * dim;
    const float *s0 = scores, *s1 = s0 + tokens, *s2 = s1 + tokens, *s3 = s2 + tokens;
    unsigned int first = begin[3], last = end[0], c = 0;
    for (; c + 16 <= dim; c += 16) {
        const float *values = v + (size_t)head * dim + c;
        ValueTile sum0 = {0}, sum1 = {0}, sum2 = {0}, sum3 = {0};
        /* Prefix/shared/suffix preserves each query's ordered local-window sum. */
        value_span(&sum0, values, s0, begin[0], first, stride);
        value_span(&sum1, values, s1, begin[1], first, stride);
        value_span(&sum2, values, s2, begin[2], first, stride);
        value_span(&sum3, values, s3, begin[3], first, stride);
        for (unsigned int j = first; j < last; j++) {
            ValueTile value = value_tile(values + (size_t)j * stride);
            value_add(&sum0, value, s0[j]); value_add(&sum1, value, s1[j]);
            value_add(&sum2, value, s2[j]); value_add(&sum3, value, s3[j]);
        }
        value_span(&sum0, values, s0, last, end[0], stride);
        value_span(&sum1, values, s1, last, end[1], stride);
        value_span(&sum2, values, s2, last, end[2], stride);
        value_span(&sum3, values, s3, last, end[3], stride);
        value_store(out + c, sum0); value_store(out + out_stride + c, sum1);
        value_store(out + 2 * out_stride + c, sum2);
        value_store(out + 3 * out_stride + c, sum3);
    }
    for (; c < dim; c++)
        for (unsigned int i = 0; i < 4; i++) {
            float sum = 0;
            for (unsigned int j = begin[i]; j < end[i]; j++)
                sum += scores[(size_t)i * tokens + j] *
                    v[((size_t)j * kv_heads + head) * dim + c];
            out[(size_t)i * out_stride + c] = sum;
        }
}
#endif

static int attention_body(size_t first, size_t last, unsigned int worker, void *arg) {
    AttentionWork *w = arg;
    unsigned int tokens = w->tokens, dim = w->dim, kv_heads = w->kv_heads;
    size_t q_stride = (size_t)4 * dim, k_stride = (size_t)kv_heads * dim;
    float *scores = w->scores + (size_t)worker * w->score_stride;
    for (size_t unit = first; unit < last; unit++) {
        unsigned int t = (unsigned int)(unit / 4) * 4, h = (unsigned int)(unit % 4);
        unsigned int rows = tokens - t < 4 ? tokens - t : 4;
        unsigned int kh = h / (4 / kv_heads);
        unsigned int begin = w->local && t > 512 ? t - 512 : 0;
        unsigned int end = w->local && t + rows + 512 < tokens ? t + rows + 512 : tokens;
        const float *query = w->q + (size_t)t * q_stride + (size_t)h * dim;
        unsigned int j = begin;
#if defined(__clang__) || defined(__GNUC__)
        if (rows == 4) {
            for (; j + 4 <= end; j += 4)
                tile4(query, w->k + (size_t)j * k_stride + (size_t)kh * dim,
                      scores + j, dim, q_stride, k_stride, tokens, 0);
        }
#endif
        for (; j < end; j++)
            for (unsigned int i = 0; i < rows; i++)
                scores[(size_t)i * tokens + j] =
                    eg_dot(query + (size_t)i * q_stride,
                           w->k + (size_t)j * k_stride + (size_t)kh * dim, dim);
        unsigned int begins[4], ends[4];
        for (unsigned int i = 0; i < rows; i++) {
            unsigned int row_begin = w->local && t + i > 512 ? t + i - 512 : 0;
            unsigned int row_end = w->local && t + i + 513 < tokens ? t + i + 513 : tokens;
            begins[i] = row_begin; ends[i] = row_end;
            float *row = scores + (size_t)i * tokens;
            float maximum = -3.402823466e38f;
            for (j = row_begin; j < row_end; j++)
                if (row[j] > maximum) maximum = row[j];
            if (eg_softmax(row + row_begin, row_end - row_begin, maximum)) return -1;
        }
#if defined(__clang__) || defined(__GNUC__)
        if (rows == 4 && tokens >= 64 && dim >= 16) {
            weighted_values4(w->v, scores, w->out + ((size_t)t * 4 + h) * dim,
                             begins, ends, tokens, dim, kv_heads, kh);
            continue;
        }
#endif
        for (unsigned int i = 0; i < rows; i++)
            weighted_values(w->v, scores + (size_t)i * tokens,
                            w->out + ((size_t)(t + i) * 4 + h) * dim,
                            begins[i], ends[i], dim, kv_heads, kh);
    }
    return 0;
}

int eg_attention_run(const EgModel *m, const float *q, const float *k, const float *v,
                     float *out, float *scores, unsigned int tokens, unsigned int dim,
                     unsigned int kv_heads, int local) {
    if (!tokens || tokens > EG2_MAX_TOKENS || !dim || dim > 512 ||
        !kv_heads || kv_heads > 4 || 4 % kv_heads)
        return eg_error("invalid attention shape", 0);
    AttentionWork work = {q, k, v, out, scores, tokens, dim, kv_heads, local, (size_t)4 * tokens};
    if (eg_parallel_for(m, "attention", (size_t)((tokens + 3) / 4) * 4, 1, attention_body, &work))
        return eg_error("parallel attention failed", 0);
    return 0;
}

typedef struct {
    AttentionWork work[EG2_MAX_BATCH];
    size_t offsets[EG2_MAX_BATCH + 1];
    unsigned int count;
} AttentionBatch;

static int attention_batch_body(size_t begin, size_t end, unsigned int worker, void *argument) {
    AttentionBatch *batch = argument;
    for (unsigned int i = 0; i < batch->count; i++) {
        size_t first = batch->offsets[i], last = batch->offsets[i + 1];
        if (begin < last && end > first &&
            attention_body((begin > first ? begin : first) - first,
                           (end < last ? end : last) - first, worker, batch->work + i))
            return -1;
    }
    return 0;
}

int eg_attention_batch(const EgModel *m, const float *q, const float *k, const float *v,
                       float *out, float *scores, const unsigned int *counts,
                       unsigned int documents, unsigned int dim, unsigned int kv_heads, int local) {
    if (!m || !q || !k || !v || !out || !scores || !counts ||
        !documents || documents > EG2_MAX_BATCH || !dim || dim > 512 ||
        !kv_heads || kv_heads > 4 || 4 % kv_heads)
        return eg_error("invalid attention batch shape", 0);
    unsigned int total = 0, maximum = 0;
    for (unsigned int i = 0; i < documents; i++) {
        if (!counts[i] || counts[i] > EG2_MAX_TOKENS - total)
            return eg_error("attention batch exceeds token budget or contains empty document", 0);
        total += counts[i];
        if (counts[i] > maximum) maximum = counts[i];
    }
    if (documents == 1)
        return eg_attention_run(m, q, k, v, out, scores, counts[0], dim, kv_heads, local);
    AttentionBatch batch = {.count = documents};
    unsigned int offset = 0;
    for (unsigned int i = 0; i < documents; i++) {
        batch.work[i] = (AttentionWork){
            q + (size_t)offset * 4 * dim, k + (size_t)offset * kv_heads * dim,
            v + (size_t)offset * kv_heads * dim, out + (size_t)offset * 4 * dim,
            scores, counts[i], dim, kv_heads, local, (size_t)4 * maximum};
        batch.offsets[i + 1] = batch.offsets[i] + (size_t)((counts[i] + 3) / 4) * 4;
        offset += counts[i];
    }
    if (eg_parallel_for(m, "attention", batch.offsets[documents], 1, attention_batch_body, &batch))
        return eg_error("parallel batched attention failed", 0);
    return 0;
}

typedef struct {
    float *x;
    const float *y;
} ActivationWork;

static int activation_body(size_t begin, size_t end, unsigned int worker, void *arg) {
    ActivationWork *w = arg;
    (void)worker;
    size_t i = begin;
#if defined(__clang__) || defined(__GNUC__)
    for (; i + 4 <= end; i += 4)
        *(Float4 *)(w->x + i) = gelu4(*(const Float4 *)(w->x + i)) * *(const Float4 *)(w->y + i);
#endif
    for (; i < end; i++) w->x[i] = eg_gelu(w->x[i]) * w->y[i];
    return 0;
}

int eg_activate(const EgModel *m, float *x, const float *y, size_t count) {
    ActivationWork work = {x, y};
    if (eg_parallel_for(m, "activation", count, 1024, activation_body, &work))
        return eg_error("parallel gated activation failed", 0);
    return 0;
}

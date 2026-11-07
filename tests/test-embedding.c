#include "../src/embedding/eg2.h"
#include "../src/embedding/search.h"
#include "math/math.h"

static int close_value(float a, float b, float tolerance) {
    return math_is_finite(a) && math_abs((double)a - b) <= tolerance;
}

static float reference_gelu(float x) {
    float cube = x * x * x;
    float inner = 0.7978845608028654f * (x + 0.044715f * cube);
    return 0.5f * x * (1.0f + (float)math_tanh(inner));
}

static int elementary_case(const EgModel *model) {
    for (int i = -106496; i <= 90112; i++) {
        float x = (float)i / 1024, actual = eg_exp(x), expected = (float)math_exp(x);
        unsigned int a, b;
        memcpy(&a, &actual, sizeof(a)); memcpy(&b, &expected, sizeof(b));
        if (!math_is_finite(actual) || (a > b ? a - b : b - a) > 1)
            return eg_error("exponential differs by more than one FP32 ULP", 0);
    }
    for (int i = -32768; i <= 32768; i++) {
        float x = (float)i / 2048;
        if (!close_value(eg_gelu(x), reference_gelu(x), 1e-6f))
            return eg_error("GELU differs from original double reference", 0);
    }
    if (eg_exp((float)-math_infinity()) != 0 || !math_is_infinite(eg_exp((float)math_infinity())) ||
        !math_is_nan(eg_exp((float)math_nan())) || !math_is_nan(eg_gelu((float)math_nan())) ||
        !math_is_infinite(eg_gelu((float)math_infinity())) ||
        !math_is_nan(eg_gelu((float)-math_infinity())))
        return eg_error("elementary kernel non-finite handling changed", 0);
    float scores[1025], expected[1025], x[4103], y[4103], before[4103];
    static const unsigned int counts[] = {0, 1, 3, 4, 7, 17, 1023, 1024, 1025, 4099};
    for (size_t test = 0; test < sizeof(counts) / sizeof(counts[0]); test++) {
        unsigned int count = counts[test];
        for (size_t i = 0; i < 4103; i++) {
            x[i] = before[i] = (float)((int)(i * 37 % 8192) - 4096) / 512;
            y[i] = (float)((int)(i % 61) - 30) / 31;
        }
        if (eg_activate(model, x + 1, y + 1, count)) return -1;
        for (size_t i = 0; i < 4103; i++) {
            float value = i >= 1 && i <= count ? reference_gelu(before[i]) * y[i] : before[i];
            if (!close_value(x[i], value, 1e-6f))
                return eg_error("vector GELU/tail/guard differs from reference", 0);
            if (i >= 1 && i <= count && x[i] != eg_gelu(before[i]) * y[i])
                return eg_error("scalar and vector GELU disagree", 0);
        }
        if (!count || count > 1025) continue;
        float total = 0;
        for (unsigned int i = 0; i < count; i++) {
            scores[i] = i == 0 ? 0 : -(float)(i * 719 % 110000) / 1024;
            expected[i] = (float)math_exp(scores[i]); total += expected[i];
        }
        if (eg_softmax(scores, count, 0)) return -1;
        double normalized = 0;
        for (unsigned int i = 0; i < count; i++) {
            if (!close_value(scores[i], expected[i] / total, 2e-7f))
                return eg_error("vector softmax differs from original reference", 0);
            normalized += scores[i];
        }
        if (math_abs(normalized - 1) > 3e-6)
            return eg_error("softmax is not normalized", 0);
    }
    if (!eg_softmax(scores, 0, 0) || !eg_softmax(scores, 1, (float)math_nan()))
        return eg_error("softmax accepted invalid inputs", 0);
    scores[0] = (float)math_nan();
    if (!eg_softmax(scores, 1, 0)) return eg_error("softmax accepted non-finite scores", 0);
    return 0;
}

static int linear_case(const EgModel *model, unsigned int tokens, unsigned int input,
                       unsigned int output) {
    size_t nx = (size_t)tokens * input, nw = (size_t)output * input, no = (size_t)tokens * output;
    float *x = rt_malloc_array(nx ? nx : 1, sizeof(float));
    float *w = rt_malloc_array(nw ? nw : 1, sizeof(float));
    float *out = rt_malloc_array(no ? no : 1, sizeof(float));
    int status = -1;
    if (!x || !w || !out) { eg_error("cannot allocate matrix-test buffers", 0); goto done; }
    for (size_t i = 0; i < nx; i++) x[i] = (float)((int)(i * 37 % 101) - 50) / 31.0f;
    for (size_t i = 0; i < nw; i++) w[i] = (float)((int)(i * 23 % 97) - 48) / 29.0f;
    if (eg_linear_run(model, x, w, out, tokens, input, output)) goto done;
    for (unsigned int t = 0; t < tokens; t++) {
        for (unsigned int r = 0; r < output; r++) {
            float expected = 0;
            for (unsigned int c = 0; c < input; c++)
                expected += x[(size_t)t * input + c] * w[(size_t)r * input + c];
            if (!close_value(out[(size_t)t * output + r], expected,
                             3e-4f + 2e-5f * (float)math_abs(expected))) {
                eg_error("tiled matrix projection differs from scalar reference", 0);
                goto done;
            }
        }
    }
    status = 0;
done:
    rt_free(x);
    rt_free(w);
    rt_free(out);
    return status;
}

static int packing_cases(const EgModel *original) {
    EgTensor tensor = {.name = "pack-test.weight", .rows = 64, .cols = 513};
    EgModel model = {.tensors = &tensor, .count = 1, .pool = original->pool, .sme = 1};
    float *x[2] = {0}, *out[3] = {0}, *reference[3] = {0};
    int status = -1;
    tensor.values = rt_malloc_array(64 * 513, sizeof(float));
    for (unsigned int i = 0; i < 2; i++) x[i] = rt_malloc_array(798 * 513 + 2, sizeof(float));
    for (unsigned int i = 0; i < 3; i++) {
        out[i] = rt_malloc_array(798 * 64 + 2, sizeof(float));
        reference[i] = rt_malloc_array(798 * 64, sizeof(float));
    }
    if (!tensor.values || !x[0] || !x[1] || !out[0] || !out[1] || !out[2] ||
        !reference[0] || !reference[1] || !reference[2]) {
        eg_error("cannot allocate packing test", 0); goto done;
    }
    for (unsigned int i = 0; i < 64 * 513; i++) tensor.values[i] = (float)((int)(i % 97) - 48) / 71;
    if (eg_workspace_init(&model) || eg_prepare_sme(&model)) goto done;
    EgModel private = model;
    private.workspace = 0;
    EgLinearJob jobs[] = {{x[0] + 1, tensor.values, out[0] + 1, 513, 64},
                         {x[0] + 1, tensor.values + 16 * 513, out[1] + 1, 513, 32},
                         {x[1] + 1, tensor.values, out[2] + 1, 513, 64}};
    const unsigned int counts[] = {64, 65, 255, 256, 257, 798};
    for (unsigned int epoch = 0; epoch < 2; epoch++)
        for (unsigned int n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
            unsigned int tokens = counts[n];
            for (unsigned int i = 0; i < 2; i++) {
                for (size_t k = 0; k < (size_t)tokens * 513; k++)
                    x[i][k + 1] = (float)((int)(k * 37 % 101) - 50 + (int)epoch + (int)i) / 71;
                x[i][0] = x[i][(size_t)tokens * 513 + 1] = 12345;
            }
            for (unsigned int j = 0; j < 3; j++) {
                out[j][0] = out[j][(size_t)tokens * jobs[j].output + 1] = 12345;
                if (eg_linear_run(&private, jobs[j].x, jobs[j].w, reference[j], tokens,
                                  jobs[j].input, jobs[j].output)) goto done;
            }
            if (eg_linear_group(&model, jobs, 3, tokens)) goto done;
            for (unsigned int j = 0; j < 3; j++) {
                size_t count = (size_t)tokens * jobs[j].output;
                if (memcmp(out[j] + 1, reference[j], count * sizeof(float)) ||
                    out[j][0] != 12345 || out[j][count + 1] != 12345) {
                    eg_error("grouped packing differs or overwrites guard", 0); goto done;
                }
                const unsigned int samples[] = {0, tokens / 2, tokens - 1};
                for (unsigned int i = 0; i < 3; i++) {
                    unsigned int row = samples[i], channel = jobs[j].output - 1;
                    double expected = 0;
                    for (unsigned int k = 0; k < jobs[j].input; k++)
                        expected += (double)jobs[j].x[(size_t)row * jobs[j].input + k] *
                            jobs[j].w[(size_t)channel * jobs[j].input + k];
                    if (math_abs(expected - out[j][1 + (size_t)row * jobs[j].output + channel]) > 5e-5) {
                        eg_error("packed projection differs from double scalar reference", 0); goto done;
                    }
                }
            }
            if (eg_linear_run(&model, jobs[0].x, jobs[0].w, out[0] + 1, tokens, 513, 64) ||
                memcmp(out[0] + 1, reference[0], (size_t)tokens * 64 * sizeof(float))) {
                eg_error("single packing differs from grouped packing", 0); goto done;
            }
            for (unsigned int i = 0; i < 2; i++)
                if (x[i][0] != 12345 || x[i][(size_t)tokens * 513 + 1] != 12345) {
                    eg_error("packing overwrites input guard", 0); goto done;
                }
        }
    status = 0;
done:
    for (unsigned int i = 0; i < 2; i++) rt_free(x[i]);
    for (unsigned int i = 0; i < 3; i++) { rt_free(out[i]); rt_free(reference[i]); }
    rt_free(tensor.values); rt_free(tensor.packed); eg_workspace_free(model.workspace);
    return status;
}

static int sme_cases(const EgModel *original) {
    int available = eg_sme_available();
    if (available <= 0) return available;
    if (packing_cases(original)) return -1;
#if EG2_HAVE_SME
    if (!eg_sme_block(0, 0, 0, 0))
        return eg_error("SME accepted an empty block", 0);
#endif
    EgTensor tensor = {.name = "test.weight", .rows = 64, .cols = 512};
    EgModel model = {.tensors = &tensor, .count = 1, .pool = original->pool, .sme = 1};
    float *x = rt_malloc_array(80 * 512 + 2, sizeof(float));
    float *out = rt_malloc_array(80 * 64 + 2, sizeof(float));
    tensor.values = rt_malloc_array(64 * 512, sizeof(float));
    int status = -1;
    if (!x || !out || !tensor.values) { eg_error("cannot allocate SME test", 0); goto done; }
    for (unsigned int i = 0; i < 80 * 512 + 2; i++) x[i] = (float)((int)(i*37%101)-50)/31;
    for (unsigned int i = 0; i < 64 * 512; i++) tensor.values[i] = (float)((int)(i*23%97)-48)/29;
    if (eg_prepare_sme(&model) || !tensor.packed) goto done;
    static const unsigned int counts[] = {1, 16, 63, 64, 65, 79};
    for (unsigned int n = 0; n < sizeof(counts)/sizeof(counts[0]); n++) {
        unsigned int tokens = counts[n];
        for (unsigned int row = 0; row <= 16; row += 16) {
            unsigned int cols = row ? 32 : 64;
            out[0] = out[tokens*cols+1] = 12345;
            if (eg_linear_run(&model, x+1, tensor.values+row*512, out+1, tokens, 512, cols))
                goto done;
            for (unsigned int t = 0; t < tokens; t++)
                for (unsigned int r = 0; r < cols; r++) {
                    float expected = 0;
                    for (unsigned int k = 0; k < 512; k++)
                        expected += x[1+t*512+k] * tensor.values[(row+r)*512+k];
                    if (!close_value(out[1+t*cols+r], expected,
                                     3e-4f + 2e-5f * (float)math_abs(expected))) {
                        eg_error("SME projection differs from scalar reference", 0); goto done;
                    }
                }
            if (out[0] != 12345 || out[tokens*cols+1] != 12345) {
                eg_error("SME overwrote output guard", 0); goto done;
            }
        }
    }
    if (eg_packed_weight(&model, tensor.values+512, 512, 32) ||
        eg_packed_weight(&model, tensor.values, 512, 17) ||
        eg_packed_weight(&model, tensor.values+48*512, 512, 32)) {
        eg_error("SME accepted an incompatible tensor slice", 0); goto done;
    }
    status = 0;
done:
    rt_free(x); rt_free(out); rt_free(tensor.values); rt_free(tensor.packed);
    return status;
}

static int linear_group_cases(const EgModel *model) {
    float x[17 * 7], w[9 * 7], out[3][17 * 9 + 2], expected[3][17 * 9];
    static const unsigned int counts[] = {0, 1, 7, 8, 9, 17};
    for (size_t i = 0; i < sizeof(x) / sizeof(x[0]); i++) x[i] = (float)((int)(i % 31) - 15) / 17;
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++) w[i] = (float)((int)(i % 29) - 14) / 19;
    EgLinearJob jobs[] = {{x, w, out[0] + 1, 7, 5}, {x, w, out[1] + 1, 3, 9},
                          {x, w, out[2] + 1, 5, 2}};
    for (size_t test = 0; test < sizeof(counts) / sizeof(counts[0]); test++) {
        unsigned int tokens = counts[test];
        for (unsigned int j = 0; j < 3; j++) {
            for (size_t i = 0; i < 17 * 9 + 2; i++) out[j][i] = 12345;
            if (eg_linear_run(model, x, w, expected[j], tokens, jobs[j].input, jobs[j].output))
                return -1;
        }
        if (eg_linear_group(model, jobs, 3, tokens)) return -1;
        for (unsigned int j = 0; j < 3; j++) {
            size_t count = (size_t)tokens * jobs[j].output;
            if (memcmp(out[j] + 1, expected[j], count * sizeof(float)) ||
                out[j][0] != 12345 || out[j][count + 1] != 12345)
                return eg_error("grouped projection differs or overwrites guard", 0);
        }
    }
    if (!eg_linear_group(model, 0, 0, 9) || !eg_linear_group(model, 0, 4, 9))
        return eg_error("projection group accepted invalid size", 0);
    return 0;
}

static int norm_cases(void) {
    float x[9 * 512], original[9 * 512], out[9 * 512], expected[9 * 512], gamma[512];
    static const unsigned int widths[] = {1, 3, 7, 256, 512};
    for (unsigned int c = 0; c < 512; c++) gamma[c] = (float)((int)(c % 21) - 10) / 7;
    for (size_t test = 0; test < sizeof(widths) / sizeof(widths[0]); test++) {
        unsigned int width = widths[test];
        for (int exponent = -60; exponent <= 60; exponent += 3) {
            for (unsigned int i = 0; i < 9 * width; i++)
                original[i] = (float)math_scalbn((double)((int)(i * 37 % 101) - 50) / 31,
                                               exponent);
            for (int weighted = 0; weighted <= 1; weighted++) {
                for (unsigned int t = 0; t < 9; t++) {
                    float squared = eg_dot(original + t * width, original + t * width, width);
                    float scale = (float)math_pow(squared / (float)width + 1e-6f, -0.5);
                    for (unsigned int c = 0; c < width; c++) {
                        float value = original[t * width + c] * scale;
                        expected[t * width + c] = weighted ? value * gamma[c] : value;
                    }
                }
                memcpy(x, original, 9 * width * sizeof(float));
                eg_norm(x, weighted ? gamma : 0, out, 9, width);
                eg_norm(x, weighted ? gamma : 0, x, 9, width);
                for (unsigned int i = 0; i < 9 * width; i++)
                    if (out[i] != x[i] || !close_value(out[i], expected[i],
                            1e-6f + 2e-7f * (float)math_abs(expected[i])))
                        return eg_error("direct RMSNorm differs from power reference", 0);
            }
        }
    }
    return 0;
}

static int attention_case(const EgModel *model, unsigned int tokens, unsigned int dim,
                          unsigned int kv_heads, int local) {
    size_t nq = (size_t)tokens * 4 * dim, nk = (size_t)tokens * kv_heads * dim;
    size_t ns = (size_t)tokens * 4 * rt_task_pool_width(model->pool);
    float *q = rt_malloc_array(nq + 2, sizeof(float));
    float *k = rt_malloc_array(nk + 2, sizeof(float));
    float *v = rt_malloc_array(nk + 2, sizeof(float));
    float *out = rt_malloc_array(nq + 2, sizeof(float));
    float *scores = rt_malloc_array(ns + 2, sizeof(float));
    float *expected = rt_malloc_array(nq, sizeof(float));
    float *row = rt_malloc_array(tokens, sizeof(float));
    int status = -1;
    if (!q || !k || !v || !out || !scores || !expected || !row) {
        eg_error("cannot allocate attention-test buffers", 0); goto done;
    }
    for (size_t i = 0; i < nq + 2; i++) {
        q[i] = (float)((int)(i * 37 % 101) - 50) / 71;
        out[i] = 12345;
    }
    for (size_t i = 0; i < nk + 2; i++) {
        k[i] = (float)((int)(i * 23 % 97) - 48) / 69;
        v[i] = (float)((int)(i * 31 % 107) - 53) / 73;
    }
    for (size_t i = 0; i < ns + 2; i++) scores[i] = 12345;
    if (eg_attention_run(model, q + 1, k + 1, v + 1, out + 1, scores + 1,
                         tokens, dim, kv_heads, local)) goto done;
    for (unsigned int t = 0; t < tokens; t++) {
        unsigned int begin = local && t > 512 ? t - 512 : 0;
        unsigned int end = local && t + 513 < tokens ? t + 513 : tokens;
        for (unsigned int h = 0; h < 4; h++) {
            unsigned int kh = h / (4 / kv_heads);
            float maximum = -3.402823466e38f;
            for (unsigned int j = begin; j < end; j++) {
                row[j] = eg_dot(q + 1 + ((size_t)t * 4 + h) * dim,
                                k + 1 + ((size_t)j * kv_heads + kh) * dim, dim);
                if (row[j] > maximum) maximum = row[j];
            }
            if (eg_softmax(row + begin, end - begin, maximum)) goto done;
            float *target = expected + ((size_t)t * 4 + h) * dim;
            for (unsigned int c = 0; c < dim; c++) target[c] = 0;
            for (unsigned int j = begin; j < end; j++)
                for (unsigned int c = 0; c < dim; c++)
                    target[c] += row[j] * v[1 + ((size_t)j * kv_heads + kh) * dim + c];
        }
    }
    if (memcmp(out + 1, expected, nq * sizeof(float)) ||
        out[0] != 12345 || out[nq + 1] != 12345 ||
        scores[0] != 12345 || scores[ns + 1] != 12345) {
        eg_error("tiled attention differs from ordered reference or overwrites guard", 0);
        goto done;
    }
    status = 0;
done:
    rt_free(q); rt_free(k); rt_free(v); rt_free(out); rt_free(scores);
    rt_free(expected); rt_free(row);
    return status;
}

static int attention_batch_case(const EgModel *model, const unsigned int *counts,
                                unsigned int documents, unsigned int dim,
                                unsigned int kv, int local) {
    unsigned int total = 0, maximum = 0;
    for (unsigned int i = 0; i < documents; i++) {
        total += counts[i];
        if (counts[i] > maximum) maximum = counts[i];
    }
    size_t nq = (size_t)total * 4 * dim, nk = (size_t)total * kv * dim;
    size_t ns = (size_t)maximum * 4 * rt_task_pool_width(model->pool);
    float *q = rt_malloc_array(nq + 2, sizeof(float)), *k = rt_malloc_array(nk + 2, sizeof(float));
    float *v = rt_malloc_array(nk + 2, sizeof(float)), *out = rt_malloc_array(nq + 2, sizeof(float));
    float *reference = rt_malloc_array(nq, sizeof(float)), *scores = rt_malloc_array(ns + 2, sizeof(float));
    int status = -1;
    if (!q || !k || !v || !out || !reference || !scores) {
        eg_error("cannot allocate batched attention test", 0); goto done;
    }
    for (size_t i = 0; i < nq + 2; i++) q[i] = (float)((int)(i * 37 % 101) - 50) / 61;
    for (size_t i = 0; i < nk + 2; i++) {
        k[i] = (float)((int)(i * 23 % 97) - 48) / 59;
        v[i] = (float)((int)(i * 31 % 89) - 44) / 53;
    }
    q[0] = q[nq + 1] = k[0] = k[nk + 1] = v[0] = v[nk + 1] = 12345;
    out[0] = out[nq + 1] = scores[0] = scores[ns + 1] = 12345;
    unsigned int offset = 0;
    for (unsigned int i = 0; i < documents; i++) {
        if (eg_attention_run(model, q + 1 + (size_t)offset * 4 * dim,
                            k + 1 + (size_t)offset * kv * dim, v + 1 + (size_t)offset * kv * dim,
                            reference + (size_t)offset * 4 * dim, scores + 1, counts[i], dim, kv, local))
            goto done;
        offset += counts[i];
    }
    for (unsigned int repeat = 0; repeat < 2; repeat++) {
        if (eg_attention_batch(model, q + 1, k + 1, v + 1, out + 1, scores + 1,
                               counts, documents, dim, kv, local)) goto done;
        if (memcmp(out + 1, reference, nq * sizeof(float)) ||
            out[0] != 12345 || out[nq + 1] != 12345 ||
            scores[0] != 12345 || scores[ns + 1] != 12345 ||
            q[0] != 12345 || q[nq + 1] != 12345 || k[0] != 12345 || k[nk + 1] != 12345 ||
            v[0] != 12345 || v[nk + 1] != 12345) {
            eg_error("batched attention differs, crosses documents or overwrites guards", 0); goto done;
        }
    }
    status = 0;
done:
    rt_free(q); rt_free(k); rt_free(v); rt_free(out); rt_free(reference); rt_free(scores);
    return status;
}

static int attention_cases(const EgModel *model) {
    static const unsigned int counts[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 64, 65};
    static const unsigned int widths[] = {1, 3, 7, 16, 17, 31, 32, 33, 63, 64, 65};
    for (size_t t = 0; t < sizeof(counts) / sizeof(counts[0]); t++)
        for (size_t d = 0; d < sizeof(widths) / sizeof(widths[0]); d++)
            for (unsigned int kv = 1; kv <= 4; kv *= 2)
                for (int local = 0; local <= 1; local++)
                    if (attention_case(model, counts[t], widths[d], kv, local)) return -1;
    static const unsigned int boundaries[] = {511, 512, 513, 514, 515, 516, 517,
                                              1023, 1024, 1025, 1026, 1027};
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++)
        if (attention_case(model, boundaries[i], 33, 2, 1)) return -1;
    if (attention_case(model, 17, 256, 2, 1) || attention_case(model, 17, 512, 1, 0) ||
        attention_case(model, 571, 256, 2, 1) || attention_case(model, 798, 512, 1, 0) ||
        attention_case(model, 515, 3, 1, 0) || attention_case(model, 8192, 1, 1, 1)) return -1;
    float value = 0;
    if (!eg_attention_run(model, &value, &value, &value, &value, &value, 0, 256, 2, 1) ||
        !eg_attention_run(model, &value, &value, &value, &value, &value, 8193, 256, 2, 1) ||
        !eg_attention_run(model, &value, &value, &value, &value, &value, 1, 0, 2, 1) ||
        !eg_attention_run(model, &value, &value, &value, &value, &value, 1, 513, 2, 1) ||
        !eg_attention_run(model, &value, &value, &value, &value, &value, 1, 256, 0, 1) ||
        !eg_attention_run(model, &value, &value, &value, &value, &value, 1, 256, 3, 1))
        return eg_error("attention accepted invalid shape", 0);
    const unsigned int mixed[] = {1, 5, 63, 64, 65, 515};
    const unsigned int global[] = {3, 17, 65, 257};
    const unsigned int single[] = {65};
    unsigned int short_docs[16];
    for (unsigned int i = 0; i < 16; i++) short_docs[i] = 1 + i;
    if (attention_batch_case(model, mixed, 6, 33, 2, 1) ||
        attention_batch_case(model, global, 4, 512, 1, 0) ||
        attention_batch_case(model, short_docs, 16, 256, 2, 1) ||
        attention_batch_case(model, single, 1, 256, 2, 1)) return -1;
    const unsigned int oversized[] = {8192, 1}, empty[] = {0};
    if (!eg_attention_batch(model, &value, &value, &value, &value, &value, mixed, 17, 33, 2, 1) ||
        !eg_attention_batch(model, &value, &value, &value, &value, &value, oversized, 2, 33, 2, 1) ||
        !eg_attention_batch(model, &value, &value, &value, &value, &value, empty, 1, 33, 2, 1) ||
        !eg_attention_batch(model, &value, &value, &value, &value, &value, mixed, 6, 33, 3, 1) ||
        !eg_attention_batch(model, &value, &value, &value, &value, &value, 0, 6, 33, 2, 1))
        return eg_error("attention batch accepted invalid shape", 0);
    return 0;
}

static int parallel_norm_case(const EgModel *model) {
    size_t count = 513 * 512;
    float *x = rt_malloc_array(count + 2, sizeof(float));
    float *out = rt_malloc_array(count + 2, sizeof(float));
    float *expected = rt_malloc_array(count, sizeof(float));
    int status = -1;
    if (!x || !out || !expected) { eg_error("cannot allocate normalization test", 0); goto done; }
    for (size_t i = 0; i < count + 2; i++) {
        x[i] = (float)((int)(i * 37 % 101) - 50) / 31;
        out[i] = 12345;
    }
    eg_norm(x + 1, 0, expected, 513, 512);
    if (eg_norm_run(model, x + 1, 0, out + 1, 513, 512) ||
        memcmp(expected, out + 1, count * sizeof(float)) ||
        out[0] != 12345 || out[count + 1] != 12345) {
        eg_error("parallel normalization differs or overwrites guard", 0); goto done;
    }
    status = 0;
done:
    rt_free(x); rt_free(out); rt_free(expected);
    return status;
}

static int batch_invalid_cases(const EgModel *model) {
    unsigned int tokens[2] = {2, EG2_VOCAB}, counts[2] = {8192, 1};
    float output[EG2_DIM];
    if (!eg_embed_batch_profiled(model, tokens, counts, 0, output, 0) ||
        !eg_embed_batch_profiled(model, tokens, counts, EG2_MAX_BATCH + 1, output, 0) ||
        !eg_embed_batch_profiled(model, tokens, counts, 2, output, 0))
        return eg_error("batch accepted invalid document count or total budget", 0);
    counts[0] = 0;
    if (!eg_embed_batch_profiled(model, tokens, counts, 1, output, 0))
        return eg_error("batch accepted empty token sequence", 0);
    counts[0] = 2;
    if (!eg_embed_batch_profiled(model, tokens, counts, 1, output, 0))
        return eg_error("batch accepted invalid token ID", 0);
    return 0;
}

typedef struct {
    size_t count;
    unsigned int signature, values[1024];
} ClaimStress;

static int claim_stress_body(size_t begin, size_t end, unsigned int worker, void *arg) {
    ClaimStress *work = arg;
    if (begin > end || end > work->count || worker >= 16) return -1;
    for (size_t i = begin; i < end; i++) work->values[i] = work->signature + (unsigned int)i;
    return 0;
}

static int worker_claim_stress(void) {
    RtTaskPool *pool = rt_malloc(sizeof(*pool));
    ClaimStress work;
    static const unsigned int counts[] = {2, 65, 3, 1024, 5, 511, 17, 1};
    int status = -1;
    if (!pool) return eg_error("cannot allocate worker stress pool", 0);
    if (rt_task_pool_init(pool, 16)) { eg_error("cannot start worker stress pool", 0); goto done; }
    for (unsigned int round = 0; round < 4096; round++) {
        unsigned int count = counts[round % 8], signature = (round + 1) * 0x9e3779b9U;
        work.count = count; work.signature = signature;
        rt_memset(work.values, 0, sizeof(work.values));
        if (rt_parallel_for(pool, count, 1, claim_stress_body, &work)) {
            eg_error("worker callback outlived its dispatch context", 0); goto done;
        }
        work.count = 0; work.signature = 0;
        if (__atomic_load_n(&pool->claimed_workers, __ATOMIC_ACQUIRE) != 0xffffffffU) {
            eg_error("completed dispatch leaves late worker claims open", 0); goto done;
        }
        for (unsigned int i = 0; i < count; i++)
            if (work.values[i] != signature + i) {
                eg_error("worker generation stress output mismatch", 0); goto done;
            }
    }
    status = 0;
done:
    rt_task_pool_destroy(pool); rt_free(pool);
    return status;
}

int main(void) {
    if (worker_claim_stress()) return 1;
    EgTitle titles[30] = {0};
    EgTitleIndex index = {.titles = titles, .count = 30};
    EgSearchHit hits[EG_SEARCH_RESULTS];
    float query[EG2_DIM] = {1};
    for (unsigned int i = 0; i < 30; i++) {
        titles[i].vector[0] = i % 3 == 0 ? 1 : i % 3 == 1 ? 0 : -1;
        if (i % 3 == 1) titles[i].vector[1] = 1;
    }
    unsigned int found;
    if (eg_title_rank(&index, query, 0, hits, &found) || found != EG_SEARCH_RESULTS)
        return eg_error("first-page count test failed", 0), 1;
    for (unsigned int i = 0; i < EG_SEARCH_RESULTS; i++)
        if (hits[i].index != (i < 10 ? i * 3 : i < 20 ? (i - 10) * 3 + 1 : (i - 20) * 3 + 2) ||
            hits[i].score != (i < 10 ? 1 : i < 20 ? 0 : -1))
            return eg_error("first-page rank/tie test failed", 0), 1;
    if (eg_title_rank(&index, query, EG_SEARCH_RESULTS, hits, &found) || found != 5)
        return eg_error("last-page count test failed", 0), 1;
    for (unsigned int i = 0; i < found; i++)
        if (hits[i].index != 17 + i * 3 || hits[i].score != -1)
            return eg_error("last-page rank/tie test failed", 0), 1;
    unsigned char bytes[] = {0x78, 0x56, 0x34, 0x12};
    EgReader r = {bytes, sizeof(bytes), 0, 0};
    if (eg_u32(&r) != 0x12345678U || r.failed || eg_bytes(&r, 1) || !r.failed)
        return eg_error("reader test failed", 0), 1;
    if (eg_bf16(0x3f80) != 1 || eg_bf16(0xbf80) != -1 ||
        !math_is_infinite(eg_bf16(0x7f80)))
        return eg_error("BF16 conversion test failed", 0), 1;
    const float x[] = {1, 2, 3, 4};
    const float w[] = {1, 0, 0, 1, 1, 1};
    float out[6];
    eg_linear(x, w, out, 2, 2, 3);
    const float expected[] = {1, 2, 3, 3, 4, 7};
    for (unsigned int i = 0; i < 6; i++)
        if (out[i] != expected[i]) return eg_error("linear projection test failed", 0), 1;
    const float gamma[] = {2, 3};
    eg_norm(x, gamma, out, 2, 2);
    if (!close_value(out[0], 1.2649108f, 1e-6f) ||
        !close_value(out[1], 3.7947323f, 1e-6f))
        return eg_error("RMSNorm test failed", 0), 1;
    if (eg_gelu(0) != 0 || !close_value(eg_gelu(1), 0.841192f, 1e-6f) ||
        !close_value(eg_gelu(-1), -0.158808f, 1e-6f))
        return eg_error("GELU test failed", 0), 1;
    EgModel model = {0};
    if (elementary_case(&model) || attention_cases(&model) || norm_cases() ||
        parallel_norm_case(&model) || linear_group_cases(&model) || batch_invalid_cases(&model) ||
        sme_cases(&model))
        return 1;
    for (unsigned int t = 0; t <= 9; t++)
        for (unsigned int i = 0; i <= 9; i++)
            for (unsigned int o = 0; o <= 9; o++)
                if (linear_case(&model, t, i, o)) return 1;
    if (eg_model_set_threads(&model, 2)) return 1;
    if (elementary_case(&model) || attention_cases(&model) || parallel_norm_case(&model) ||
        linear_group_cases(&model) || sme_cases(&model)) {
        eg_model_free(&model); return 1;
    }
    int failed = linear_case(&model, 17, 513, 257);
    eg_model_free(&model);
    if (failed) return 1;
    return rt_write_line(1, "native primitive tests passed") ? 1 : 0;
}

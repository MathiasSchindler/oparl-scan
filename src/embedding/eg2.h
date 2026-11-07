#ifndef EG2_H
#define EG2_H

#include "runtime.h"
#include "tool_util.h"
#include "concurrency.h"

#define EG2_VOCAB 262144U
#define EG2_LAYERS 24U
#define EG2_HIDDEN 512U
#define EG2_DIM 768U
#define EG2_MAX_TOKENS 8192U
#define EG2_MAX_BATCH 16U
#if defined(__APPLE__) && defined(__aarch64__) && !defined(EG2_DISABLE_SME)
#define EG2_HAVE_SME 1
#else
#define EG2_HAVE_SME 0
#endif
#define EG2_MAX_TEXT 262144U
#define EG2_NONE 0xffffffffU
#define EG2_MODEL_REVISION "f9972ac22e696a105c482202646979e33e4b5a1e"
#define EG2_WEIGHTS_SHA256 "8c97229c2e3a8fea5bc0c1fde35546c6e3fefa5b958a5f259b195f07eecdeff7"
#define EG2_TOKENIZER_SHA256 "05fa11caf2ec0cd1226f42a6f1b56b003969553adc8c94e4a741f9bc22c9b1a0"
#define EG2_DOCUMENT_PREFIX "title: none | text: "
#define EG2_QUERY_PREFIX "task: search result | query: "
#define EG2_TITLE_RECIPE "eg2-paper-title-v1\n" EG2_WEIGHTS_SHA256 "\n" EG2_TOKENIZER_SHA256 "\n" EG2_DOCUMENT_PREFIX
#define EG2_VECTOR_SIZE (72U + EG2_DIM * 4U)

typedef struct {
    const unsigned char *data;
    size_t size, offset;
    int failed;
} EgReader;

unsigned int eg_u32(EgReader *r);
const unsigned char *eg_bytes(EgReader *r, size_t count);
int eg_error(const char *message, const char *detail);
float eg_bf16(unsigned int bits);
int eg_read_artifact(const char *path, size_t expected_size, const char *sha256,
                     unsigned char **data, size_t *size);
void eg_title_key(const char *id, const char *title, unsigned char key[32]);
void eg_title_vector_path(const unsigned char key[32], char path[80]);
int eg_vector_valid(const float *vector);
int eg_vector_read(const char *path, const unsigned char key[32], float *vector, int *found);
int eg_vector_write(const char *path, const unsigned char key[32], const float *vector);

typedef struct {
    char name[128];
    unsigned int rows, cols;
    float *values;
    float *packed;
} EgTensor;

typedef struct EgWorkspace EgWorkspace;

typedef struct {
    EgTensor *tensors;
    unsigned int count;
    RtTaskPool *pool;
    EgWorkspace *workspace;
    int sme;
#if NEWOS_RUNTIME_TASK_STATS
    int (*worker_observer)(const char *operation, size_t count, size_t min_chunk,
                           const RtTaskPool *pool, void *context);
    void *worker_context;
#endif
} EgModel;

#if NEWOS_RUNTIME_TASK_STATS
static inline int eg_parallel_for(const EgModel *m, const char *operation,
                                  size_t count, size_t min_chunk,
                                  RtParallelBody body, void *argument) {
    if (m->worker_observer) rt_task_pool_reset_stats(m->pool);
    int status = rt_parallel_for(m->pool, count, min_chunk, body, argument);
    if (m->worker_observer &&
        m->worker_observer(operation, count, min_chunk, m->pool, m->worker_context))
        return eg_error("cannot capture worker diagnostics", operation);
    return status;
}
#else
#define eg_parallel_for(m, operation, count, min_chunk, body, argument) \
    rt_parallel_for((m)->pool, count, min_chunk, body, argument)
#endif

typedef struct {
    unsigned long long total_ns, linear_ns, attention_ns, norm_ns, rope_ns, activation_ns;
} EgProfile;

typedef struct {
    const float *x, *w;
    float *out;
    unsigned int input, output;
} EgLinearJob;

typedef struct {
    unsigned char *data;
    unsigned int size;
} EgPiece;

typedef struct {
    unsigned int left, right, result;
} EgMerge;

typedef struct {
    unsigned char *storage;
    EgPiece *pieces;
    EgMerge *merges;
    unsigned int *lexical, *pairs;
    unsigned int added[64], bytes[256];
    unsigned int merge_count, added_count;
} EgTokenizer;

int eg_tokenizer_load(EgTokenizer *t, const char *path);
void eg_tokenizer_free(EgTokenizer *t);
int eg_tokenize(const EgTokenizer *t, const unsigned char *text, size_t size,
                unsigned int *tokens, unsigned int *count);
int eg_model_load(EgModel *m, const char *path);
int eg_workspace_init(EgModel *m);
void eg_workspace_free(EgWorkspace *workspace);
void eg_model_free(EgModel *m);
int eg_model_set_threads(EgModel *m, unsigned int threads);
const float *eg_weight(const EgModel *m, const char *name, unsigned int rows, unsigned int cols);
int eg_sme_available(void);
#if EG2_HAVE_SME
int eg_sme_block(const float *a, const float *b, float *out, unsigned int count);
#endif
int eg_prepare_sme(const EgModel *m);
const float *eg_packed_weight(const EgModel *m, const float *w, unsigned int input,
                              unsigned int output);
void eg_linear(const float *x, const float *w, float *out,
               unsigned int tokens, unsigned int input, unsigned int output);
float eg_dot(const float *a, const float *b, unsigned int count);
int eg_linear_run(const EgModel *m, const float *x, const float *w, float *out,
                  unsigned int tokens, unsigned int input, unsigned int output);
/* Jobs must be independent; at most three projections share one dispatch. */
int eg_linear_group(const EgModel *m, const EgLinearJob *jobs, unsigned int count,
                    unsigned int tokens);
int eg_activate(const EgModel *m, float *x, const float *y, size_t count);
void eg_norm(const float *x, const float *w, float *out, unsigned int count, unsigned int width);
int eg_norm_run(const EgModel *m, const float *x, const float *w, float *out,
                unsigned int count, unsigned int width);
float eg_gelu(float x);
float eg_exp(float x);
int eg_softmax(float *scores, unsigned int count, float maximum);
/* scores requires 4 * tokens * rt_task_pool_width(m->pool) floats. */
int eg_attention_run(const EgModel *m, const float *q, const float *k, const float *v,
                     float *out, float *scores, unsigned int tokens, unsigned int dim,
                     unsigned int kv_heads, int local);
/* Scratch holds 4 * max(counts) * pool width floats; documents remain isolated. */
int eg_attention_batch(const EgModel *m, const float *q, const float *k, const float *v,
                       float *out, float *scores, const unsigned int *counts,
                       unsigned int documents, unsigned int dim, unsigned int kv_heads, int local);
int eg_embed(const EgModel *m, const unsigned int *tokens, unsigned int count,
             float output[EG2_DIM], const char *trace);
int eg_embed_profiled(const EgModel *m, const unsigned int *tokens, unsigned int count,
                      float output[EG2_DIM], const char *trace, EgProfile *profile);
/* Concatenated documents, at most EG2_MAX_TOKENS total; one output vector per count. */
int eg_embed_batch_profiled(const EgModel *m, const unsigned int *tokens,
                            const unsigned int *counts, unsigned int documents,
                            float *output, EgProfile *profile);

#endif

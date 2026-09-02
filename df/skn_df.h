/*  skn_df.h — header-only DataFrame library (Polars-inspired)
 *
 *  USAGE
 *    #define SKN_CSV_IMPLEMENTATION
 *    #define SKN_LOG_IMPLEMENTATION
 *    #define SKN_DF_IMPLEMENTATION
 *    #include "skn_df.h"
 *
 *  Depends on skinny-csv and skinny-log from deps/skinny-headers/.
 *
 *  OVERVIEW
 *    DataFrame is a column-oriented table of typed arrays.
 *    Built-in operations:
 *      df_load()       — create a DataFrame from a CSV file
 *      df_write()      — serialize a DataFrame to a CSV file
 *      df_new()        — heap-allocate an empty DataFrame with N column slots
 *      df_set_column_int/float/double/string — fill a column slot
 *      df_get_int/float/double/string — typed column access
 *      df_group_by()   — lazy group-by (returns DfGrouped, no computation), hash-based internally
 *      df_sum()        — terminal sum aggregation (returns new DataFrame)
 *      df_sort()       — stable sort, returns a row-index permutation
 *      df_gather_int/float/double/string — select/reorder a column's rows by index
 *      df_match()/df_match_string() — deterministic m:1/1:1 equi-join
 *      df_assert_unique()/df_assert_unique_string() — cardinality check
 *      df_resample()   — seeded weighted sample-with-replacement
 *      df_stack_v_int/float/double/string — vertical concatenation
 *      df_print()      — print to stdout
 *      df_free()       — free memory
 */

#ifndef SKN_DF_H
#define SKN_DF_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Configuration — override via #define before including this header
 * ---------------------------------------------------------------------- */
#ifndef DF_MAX_COLS
#define DF_MAX_COLS  256
#endif
#ifndef DF_MAX_NAME
#define DF_MAX_NAME   64
#endif

/* -------------------------------------------------------------------------
 * Public types
 * ---------------------------------------------------------------------- */

typedef enum {
    DF_INT    = 0,
    DF_FLOAT  = 1,
    DF_DOUBLE = 2,
    DF_STRING = 3
} DfType;

/* Internal per-column storage — one contiguous typed array per column */
typedef union {
    int    *i;
    float  *f;
    double *d;
    char  **s;
} DfColData;

/* Core data structure */
typedef struct {
    char      names[DF_MAX_COLS][DF_MAX_NAME];
    DfType    types[DF_MAX_COLS];
    DfColData data[DF_MAX_COLS];
    int       col_count;
    int       row_count;
} DataFrame;

/* Typed column views returned by df_get_* */
typedef struct { int    *data; int count; } DfIntCol;
typedef struct { float  *data; int count; } DfFloatCol;
typedef struct { double *data; int count; } DfDoubleCol;
typedef struct { char  **data; int count; } DfStrCol;

/* Lazy grouped frame — returned by df_group_by, consumed by df_sum */
typedef struct {
    const DataFrame *source;
    int              n_by;
    const char      *by_cols[DF_MAX_COLS];  /* pointers into source->names */
} DfGrouped;

/* Result of df_match/df_match_string: parallel arrays, one entry per
 * matched (left row, right row) pair. left_idx is always [0, 1, ..., n-1]
 * (every left row matches, or the whole call fails fast) — kept explicit
 * rather than implied so match results compose the same way df_gather_*'s
 * index arrays do. */
typedef struct { int *left_idx; int *right_idx; int count; } DfMatchResult;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/* Load a CSV file into a new heap-allocated DataFrame.
 * Returns NULL on failure (file not found, parse error). */
DataFrame *df_load(const char *filename);

/* Serialize df to a CSV file at path: header row (df->names) followed by
 * df->row_count data rows. Fields containing a comma, double quote, or
 * newline are quoted per RFC-4180, with embedded '"' doubled.
 * Returns 0 on success. Exits with [Ursula] error via skinny-log on I/O
 * failure (same fail-fast convention as df_get_*'s "column not found"). */
int df_write(const DataFrame *df, const char *path);

/* Heap-allocate an empty DataFrame with col_count column slots reserved.
 * row_count is unset until the first df_set_column_* call. Caller-owned,
 * free with df_free(). Exits with [Ursula] error via skinny-log if
 * col_count exceeds DF_MAX_COLS; returns NULL on allocation failure. */
DataFrame *df_new(int col_count);

/* Fill column slot idx of df with a copy of data (length n), naming it
 * `name`. The first df_set_column_* call on a given df fixes df->row_count
 * to n; every later call on the same df must pass the same n — a mismatch
 * is a fail-fast [Ursula] error via skinny-log, not silent truncation/pad. */
void df_set_column_int   (DataFrame *df, int idx, const char *name, const int    *data, int n);
void df_set_column_float (DataFrame *df, int idx, const char *name, const float  *data, int n);
void df_set_column_double(DataFrame *df, int idx, const char *name, const double *data, int n);
void df_set_column_string(DataFrame *df, int idx, const char *name, char *const  *data, int n);

/* Free a DataFrame previously returned by df_load, df_new, or df_sum. Safe with NULL. */
void df_free(DataFrame *df);

/* Print the DataFrame to stdout in a tabular format. */
void df_print(const DataFrame *df);

/* Typed column access.
 * Exits with [Ursula] error via skinny-log if the column is not found. */
DfIntCol    df_get_int(const DataFrame *df, const char *name);
DfFloatCol  df_get_float(const DataFrame *df, const char *name);
DfDoubleCol df_get_double(const DataFrame *df, const char *name);
DfStrCol    df_get_string(const DataFrame *df, const char *name);

/* Group by the named columns (lazy — no computation performed).
 * by_cols is an array of column-name pointers of length n_by.
 * The caller must keep df alive until the aggregation is done. */
DfGrouped df_group_by(const DataFrame *df, const char *by_cols[], int n_by);

/* Sum all numeric columns per group. Terminal operation.
 * Returns a new heap-allocated DataFrame — caller must free with df_free().
 * String columns not in the group-by set are dropped from the output.
 * Groups rows via an internal hash table (O(row_count) average, not
 * O(row_count * group_count)) — safe at multi-million-row scale. */
DataFrame *df_sum(const DfGrouped *grp);

/* Stable sort: returns a caller-owned int* of length df->row_count — the
 * row-index permutation that puts by_col in ascending order. Rows with
 * equal by_col values keep their relative input order (stability matters:
 * downstream group-then-sample pipelines rely on it for run-to-run
 * reproducibility under a fixed seed). Free with free().
 * Exits with [Ursula] error via skinny-log if df is NULL or by_col is not
 * found; returns NULL on allocation failure. */
int *df_sort(const DataFrame *df, const char *by_col);

/* Select/reorder col's rows by index: output row k = input row idx[k].
 * idx may repeat a row (fan-out) and idx_count may differ from col.count —
 * the output always has length idx_count. Returns a freshly heap-allocated
 * column (data: free() it; df_gather_string's data: free() each string
 * then the array — same shape as df_get_string's DfStrCol, just owned
 * instead of borrowed). Exits with [Ursula] error via skinny-log on a NULL
 * idx (when idx_count > 0) or an out-of-range index, same fail-fast
 * convention as df_get_*'s "column not found". */
DfIntCol    df_gather_int   (DfIntCol    col, const int *idx, int idx_count);
DfFloatCol  df_gather_float (DfFloatCol  col, const int *idx, int idx_count);
DfDoubleCol df_gather_double(DfDoubleCol col, const int *idx, int idx_count);
DfStrCol    df_gather_string(DfStrCol    col, const int *idx, int idx_count);

/* Deterministic m:1/1:1 equi-join: for each row of left_key, find the row
 * of right_key with an equal value. Not an outer join — a left_key value
 * with no match in right_key is a df__error fail-fast (every row is
 * expected to resolve). Not a fan-out either: if right_key holds a
 * duplicate value, df_match returns some one match for it, not all of
 * them — call df_assert_unique(right_key) first to rule that case out
 * before trusting the 1:1 assumption. Returns a caller-owned
 * DfMatchResult (free left_idx and right_idx with free()); left_idx is
 * always [0..left_key.count) and right_idx[k] indexes right_key. */
DfMatchResult df_match       (DfIntCol left_key, DfIntCol right_key);
DfMatchResult df_match_string(DfStrCol left_key, DfStrCol right_key);

/* df__error fail-fast if key contains any duplicate value. Used to
 * validate cardinality (e.g. "every idx maps to exactly one
 * household_conf") before trusting df_match's 1:1 assumption. */
void df_assert_unique       (DfIntCol key);
void df_assert_unique_string(DfStrCol key);

/* Weighted sample-with-replacement, seeded and reproducible (two calls
 * with the same seed and inputs are byte-identical, same determinism bar
 * as df_sort). weight_key/weights together describe the sampling pool: a
 * pool row j belongs to group weight_key.data[j] with weight
 * weights.data[j] (weight_key.count must equal weights.count). For each
 * row of residual_idx (a per-row group id — think "which group's demand
 * this output row is for"), draws one pool row from that group's weighted
 * distribution and returns its index into weight_key/weights — chain a
 * df_gather_* call on whatever column holds the actual values (e.g.
 * family_type) to turn indices into values, the same two-step shape as
 * df_sort + df_gather_*.
 * A group with zero total weight in weight_key/weights produces no output
 * for those residual_idx rows — the result's count can be less than
 * residual_idx.count. That's deliberate: this function doesn't widen the
 * pool or retry at a coarser key, the caller does (thread the gap into a
 * second df_resample call against a coarser group key). */
DfIntCol df_resample(DfIntCol residual_idx, DfIntCol weight_key, DfDoubleCol weights, unsigned seed);

/* Vertical concatenation: n same-typed columns end to end, in argument
 * order, into one output column of their combined length. Not the same
 * operation as df_set_column_* (which places columns side by side into a
 * wider frame) — kept a separate function on purpose, not merged into one
 * that tries to do both. Returns a freshly heap-allocated column, freed
 * the same way as df_gather_*'s output. */
DfIntCol    df_stack_v_int   (DfIntCol    *cols, int n);
DfFloatCol  df_stack_v_float (DfFloatCol  *cols, int n);
DfDoubleCol df_stack_v_double(DfDoubleCol *cols, int n);
DfStrCol    df_stack_v_string(DfStrCol    *cols, int n);

/* -------------------------------------------------------------------------
 * Implementation
 * ---------------------------------------------------------------------- */

#ifdef SKN_DF_IMPLEMENTATION

#include "../deps/skinny-headers/skinny-csv/skn_csv.h"
#include "../deps/skinny-headers/skinny-log/skn_log.h"

/* ---- internal helpers ---- */

static SknLog *df__log(void)
{
    static SknLog *log = NULL;
    if (!log) log = slog_init(SKN_LOG_LEVEL, stderr);
    return log;
}

static void df__error(const char *msg)
{
    slog_print(df__log(), SKN_LOG_ERR, "[Ursula] %s\n", msg);
    exit(1);
}

/* Return column index, or -1 if not found. */
static int df__col_index(const DataFrame *df, const char *name)
{
    for (int i = 0; i < df->col_count; i++)
        if (strcmp(df->names[i], name) == 0) return i;
    return -1;
}

/* Check if two rows have identical values across all by-columns. */
static int df__rows_match(const DataFrame *df, int a, int b,
                          const int *by_idx, int n_by)
{
    for (int i = 0; i < n_by; i++) {
        int c = by_idx[i];
        switch (df->types[c]) {
            case DF_INT:
                if (df->data[c].i[a] != df->data[c].i[b]) return 0;
                break;
            case DF_FLOAT:
                if (df->data[c].f[a] != df->data[c].f[b]) return 0;
                break;
            case DF_DOUBLE:
                if (df->data[c].d[a] != df->data[c].d[b]) return 0;
                break;
            case DF_STRING:
                if (strcmp(df->data[c].s[a], df->data[c].s[b]) != 0) return 0;
                break;
        }
    }
    return 1;
}

/* FNV-1a hash of one row's by-column values, mixed across columns. Used to
 * bucket candidate groups in df_sum's grouping hash table — df__rows_match
 * still does the authoritative equality check on any hash collision, so a
 * weak/colliding hash only costs performance, never correctness. */
static uint64_t df__hash_row(const DataFrame *df, int row, const int *by_idx, int n_by)
{
    uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */

    for (int i = 0; i < n_by; i++) {
        int c = by_idx[i];
        const unsigned char *bytes;
        size_t len;
        int    iv;
        float  fv;
        double dv;

        switch (df->types[c]) {
            case DF_INT:
                iv = df->data[c].i[row];
                bytes = (const unsigned char *)&iv;
                len = sizeof(iv);
                break;
            case DF_FLOAT:
                fv = df->data[c].f[row];
                bytes = (const unsigned char *)&fv;
                len = sizeof(fv);
                break;
            case DF_DOUBLE:
                dv = df->data[c].d[row];
                bytes = (const unsigned char *)&dv;
                len = sizeof(dv);
                break;
            case DF_STRING:
            default:
                bytes = (const unsigned char *)df->data[c].s[row];
                len = strlen(df->data[c].s[row]);
                break;
        }

        for (size_t b = 0; b < len; b++) {
            h ^= bytes[b];
            h *= 1099511628211ULL; /* FNV-1a prime */
        }
        h ^= 0xffu; /* separator so column boundaries affect the hash */
        h *= 1099511628211ULL;
    }
    return h;
}

/* Smallest power of two >= n (n >= 1). Used to size every hash-chained
 * bucket table in this file: df_sum's grouping, df_match/df_assert_unique,
 * and df_resample's group pool. */
static size_t df__next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* FNV-1a over a raw int/string value — the same hash family as
 * df__hash_row, just for df_match/df_assert_unique/df_resample, which key
 * their tables on bare DfIntCol/DfStrCol values rather than DataFrame
 * rows. */
static uint64_t df__hash_int(int v)
{
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *bytes = (const unsigned char *)&v;
    for (size_t b = 0; b < sizeof(v); b++) {
        h ^= bytes[b];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t df__hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- seeded PRNG for df_resample ----
 * splitmix64: fully self-contained (state is caller-owned, passed by
 * pointer), so reproducibility depends only on the `seed` df_resample was
 * given — never on libc's global rand()/srand() state, which anything
 * else in the same process could perturb. */
static uint64_t df__splitmix64_next(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform double in [0,1), 53 significant bits. */
static double df__rand_uniform(uint64_t *state)
{
    uint64_t r = df__splitmix64_next(state);
    return (double)(r >> 11) * (1.0 / 9007199254740992.0); /* / 2^53 */
}

/* ---- df_sort: stable merge sort over a row-index permutation ---- */

typedef int (*df__cmp_fn)(const DataFrame *df, int col, int a, int b);

static int df__cmp_int(const DataFrame *df, int col, int a, int b)
{
    int va = df->data[col].i[a], vb = df->data[col].i[b];
    return (va > vb) - (va < vb);
}

static int df__cmp_float(const DataFrame *df, int col, int a, int b)
{
    float va = df->data[col].f[a], vb = df->data[col].f[b];
    return (va > vb) - (va < vb);
}

static int df__cmp_double(const DataFrame *df, int col, int a, int b)
{
    double va = df->data[col].d[a], vb = df->data[col].d[b];
    return (va > vb) - (va < vb);
}

static int df__cmp_string(const DataFrame *df, int col, int a, int b)
{
    return strcmp(df->data[col].s[a], df->data[col].s[b]);
}

static df__cmp_fn df__cmp_for_type(DfType t)
{
    switch (t) {
        case DF_INT:    return df__cmp_int;
        case DF_FLOAT:  return df__cmp_float;
        case DF_DOUBLE: return df__cmp_double;
        case DF_STRING: default: return df__cmp_string;
    }
}

/* Merge idx[lo,mid) and idx[mid,hi) (each already sorted) into idx[lo,hi),
 * using tmp as scratch space. <= (not <) on the comparator result keeps
 * left-half elements first on ties, which is what makes the whole sort
 * stable. */
static void df__merge(int *idx, int *tmp, int lo, int mid, int hi,
                       const DataFrame *df, int col, df__cmp_fn cmp)
{
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (cmp(df, col, idx[i], idx[j]) <= 0) tmp[k++] = idx[i++];
        else                                   tmp[k++] = idx[j++];
    }
    while (i < mid) tmp[k++] = idx[i++];
    while (j < hi)  tmp[k++] = idx[j++];
    memcpy(idx + lo, tmp + lo, (size_t)(hi - lo) * sizeof(int));
}

static void df__merge_sort(int *idx, int *tmp, int lo, int hi,
                            const DataFrame *df, int col, df__cmp_fn cmp)
{
    if (hi - lo <= 1) return;
    int mid = lo + (hi - lo) / 2;
    df__merge_sort(idx, tmp, lo, mid, df, col, cmp);
    df__merge_sort(idx, tmp, mid, hi, df, col, cmp);
    df__merge(idx, tmp, lo, mid, hi, df, col, cmp);
}

/* Extract a numeric value from a cell as a double (for summing). */
static double df__as_double(const DataFrame *df, int col, int row)
{
    switch (df->types[col]) {
        case DF_INT:    return (double)df->data[col].i[row];
        case DF_FLOAT:  return (double)df->data[col].f[row];
        case DF_DOUBLE: return df->data[col].d[row];
        default:        return 0.0; /* unreachable if caller checks numeric */
    }
}

/* Copy a single cell value from src[col][row] to dst[col][row] (string dups). */
static void df__copy_cell(const DataFrame *src, int col, int row,
                          DataFrame *dst, int dst_col, int dst_row)
{
    switch (src->types[col]) {
        case DF_INT:
            dst->data[dst_col].i[dst_row] = src->data[col].i[row];
            break;
        case DF_FLOAT:
            dst->data[dst_col].f[dst_row] = src->data[col].f[row];
            break;
        case DF_DOUBLE:
            dst->data[dst_col].d[dst_row] = src->data[col].d[row];
            break;
        case DF_STRING:
            dst->data[dst_col].s[dst_row] = strdup(src->data[col].s[row]);
            break;
    }
}

/* Write one CSV field, quoting per RFC-4180 if it contains a comma, a
 * double quote, or a newline. Embedded '"' is doubled. */
static void df__csv_write_field(FILE *fp, const char *s)
{
    if (!s) s = "";

    int need_quote = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') { need_quote = 1; break; }
    }

    if (!need_quote) {
        fputs(s, fp);
        return;
    }

    fputc('"', fp);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', fp);
        fputc(*p, fp);
    }
    fputc('"', fp);
}

/* Format and write cell [col][row] as a CSV field.
 * Floats are written with a trailing 'f' (skinny-csv's own convention,
 * see csv__infer_cell) so they re-infer as DF_FLOAT rather than DF_DOUBLE
 * on reload. Both float and double use enough decimal digits (9 / 17,
 * i.e. FLT_DECIMAL_DIG / DBL_DECIMAL_DIG) to round-trip exactly. */
static void df__csv_write_cell(FILE *fp, const DataFrame *df, int col, int row)
{
    char buf[64];
    switch (df->types[col]) {
        case DF_INT:
            snprintf(buf, sizeof(buf), "%d", df->data[col].i[row]);
            df__csv_write_field(fp, buf);
            break;
        case DF_FLOAT:
            snprintf(buf, sizeof(buf), "%.9gf", (double)df->data[col].f[row]);
            df__csv_write_field(fp, buf);
            break;
        case DF_DOUBLE:
            snprintf(buf, sizeof(buf), "%.17g", df->data[col].d[row]);
            df__csv_write_field(fp, buf);
            break;
        case DF_STRING:
            df__csv_write_field(fp, df->data[col].s[row]);
            break;
    }
}

/* Fix (on first call) or validate (on later calls) df->row_count against a
 * column of length n being written into df by df_set_column_*. */
static void df__set_row_count(DataFrame *df, int n, const char *fname)
{
    if (df->row_count < 0) {
        df->row_count = n;
    } else if (df->row_count != n) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s: row count mismatch (column has %d rows, frame already has %d)",
                 fname, n, df->row_count);
        df__error(buf);
    }
}

/* Shared argument validation + bookkeeping for every df_set_column_* call:
 * NULL checks, index bounds, row-count agreement, and recording the name. */
static void df__set_column_check(DataFrame *df, int idx, const char *name, int n,
                                  const char *fname)
{
    char buf[256];
    if (!df)   { snprintf(buf, sizeof(buf), "%s: df is NULL", fname); df__error(buf); }
    if (!name) { snprintf(buf, sizeof(buf), "%s: name is NULL", fname); df__error(buf); }
    if (idx < 0 || idx >= df->col_count) {
        snprintf(buf, sizeof(buf), "%s: index %d out of range [0,%d)", fname, idx, df->col_count);
        df__error(buf);
    }
    if (n < 0) { snprintf(buf, sizeof(buf), "%s: n must be >= 0", fname); df__error(buf); }

    df__set_row_count(df, n, fname);

    strncpy(df->names[idx], name, DF_MAX_NAME - 1);
    df->names[idx][DF_MAX_NAME - 1] = '\0';
}

/* ---- public: lifecycle ---- */

DataFrame *df_load(const char *filename)
{
    if (!filename) df__error("df_load: filename is NULL");

    CsvDocument *doc = csv_load(filename, ',');
    if (!doc) return NULL;

    DataFrame *df = (DataFrame *)calloc(1, sizeof(DataFrame));
    if (!df) { csv_free(doc); return NULL; }

    df->col_count = doc->col_count;
    df->row_count = doc->row_count;

    for (int i = 0; i < doc->col_count && i < DF_MAX_COLS; i++) {
        /* copy name */
        strncpy(df->names[i], doc->names[i], DF_MAX_NAME - 1);
        df->names[i][DF_MAX_NAME - 1] = '\0';

        /* map CsvType -> DfType */
        switch (doc->types[i]) {
            case CSV_INT:    df->types[i] = DF_INT;    break;
            case CSV_FLOAT:  df->types[i] = DF_FLOAT;  break;
            case CSV_DOUBLE: df->types[i] = DF_DOUBLE; break;
            case CSV_STRING: df->types[i] = DF_STRING; break;
        }

        /* allocate and copy data */
        int n = doc->row_count;
        switch (df->types[i]) {
            case DF_INT: {
                df->data[i].i = (int *)malloc((size_t)n * sizeof(int));
                if (!df->data[i].i) { csv_free(doc); df_free(df); return NULL; }
                memcpy(df->data[i].i, doc->data[i].i, (size_t)n * sizeof(int));
                break;
            }
            case DF_FLOAT: {
                df->data[i].f = (float *)malloc((size_t)n * sizeof(float));
                if (!df->data[i].f) { csv_free(doc); df_free(df); return NULL; }
                memcpy(df->data[i].f, doc->data[i].f, (size_t)n * sizeof(float));
                break;
            }
            case DF_DOUBLE: {
                df->data[i].d = (double *)malloc((size_t)n * sizeof(double));
                if (!df->data[i].d) { csv_free(doc); df_free(df); return NULL; }
                memcpy(df->data[i].d, doc->data[i].d, (size_t)n * sizeof(double));
                break;
            }
            case DF_STRING: {
                df->data[i].s = (char **)calloc((size_t)n, sizeof(char *));
                if (!df->data[i].s) { csv_free(doc); df_free(df); return NULL; }
                for (int r = 0; r < n; r++) {
                    df->data[i].s[r] = strdup(doc->data[i].s[r]);
                    if (!df->data[i].s[r]) { csv_free(doc); df_free(df); return NULL; }
                }
                break;
            }
        }
    }

    csv_free(doc);
    return df;
}

int df_write(const DataFrame *df, const char *path)
{
    if (!df)   df__error("df_write: df is NULL");
    if (!path) df__error("df_write: path is NULL");

    FILE *fp = fopen(path, "w");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "df_write: failed to open '%s' for writing", path);
        df__error(buf);
    }

    /* header row */
    for (int i = 0; i < df->col_count; i++) {
        if (i > 0) fputc(',', fp);
        df__csv_write_field(fp, df->names[i]);
    }
    fputc('\n', fp);

    /* data rows */
    for (int r = 0; r < df->row_count; r++) {
        for (int i = 0; i < df->col_count; i++) {
            if (i > 0) fputc(',', fp);
            df__csv_write_cell(fp, df, i, r);
        }
        fputc('\n', fp);
    }

    int write_failed = ferror(fp);
    int close_failed = (fclose(fp) != 0);

    if (write_failed || close_failed) {
        char buf[512];
        snprintf(buf, sizeof(buf), "df_write: I/O error writing '%s'", path);
        df__error(buf);
    }

    return 0;
}

DataFrame *df_new(int col_count)
{
    if (col_count < 0) df__error("df_new: col_count must be >= 0");
    if (col_count > DF_MAX_COLS) df__error("df_new: col_count exceeds DF_MAX_COLS");

    DataFrame *df = (DataFrame *)calloc(1, sizeof(DataFrame));
    if (!df) return NULL;

    df->col_count = col_count;
    df->row_count = -1; /* unset until the first df_set_column_* call */
    return df;
}

#define DF_MAKE_SETTER(ctype, dtype, dtype_enum, member)                          \
    void df_set_column_##dtype(DataFrame *df, int idx, const char *name,          \
                                const ctype *data, int n)                         \
    {                                                                             \
        df__set_column_check(df, idx, name, n, "df_set_column_" #dtype);          \
        if (n > 0 && !data)                                                       \
            df__error("df_set_column_" #dtype ": data is NULL");                  \
                                                                                    \
        ctype *arr = (ctype *)malloc((size_t)(n > 0 ? n : 1) * sizeof(ctype));    \
        if (!arr) df__error("df_set_column_" #dtype ": allocation failed");       \
        if (n > 0) memcpy(arr, data, (size_t)n * sizeof(ctype));                  \
                                                                                    \
        df->data[idx].member = arr;                                               \
        df->types[idx] = dtype_enum;                                              \
    }

DF_MAKE_SETTER(int,    int,    DF_INT,    i)
DF_MAKE_SETTER(float,  float,  DF_FLOAT,  f)
DF_MAKE_SETTER(double, double, DF_DOUBLE, d)

#undef DF_MAKE_SETTER

void df_set_column_string(DataFrame *df, int idx, const char *name, char *const *data, int n)
{
    df__set_column_check(df, idx, name, n, "df_set_column_string");
    if (n > 0 && !data) df__error("df_set_column_string: data is NULL");

    char **arr = (char **)calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    if (!arr) df__error("df_set_column_string: allocation failed");

    for (int r = 0; r < n; r++) {
        arr[r] = strdup(data[r]);
        if (!arr[r]) {
            for (int j = 0; j < r; j++) free(arr[j]);
            free(arr);
            df__error("df_set_column_string: allocation failed");
        }
    }

    df->data[idx].s = arr;
    df->types[idx] = DF_STRING;
}

void df_free(DataFrame *df)
{
    if (!df) return;
    for (int i = 0; i < df->col_count; i++) {
        switch (df->types[i]) {
            case DF_INT:    free(df->data[i].i);   break;
            case DF_FLOAT:  free(df->data[i].f);   break;
            case DF_DOUBLE: free(df->data[i].d);   break;
            case DF_STRING:
                for (int r = 0; r < df->row_count; r++) free(df->data[i].s[r]);
                free(df->data[i].s);
                break;
        }
    }
    free(df);
}

void df_print(const DataFrame *df)
{
    if (!df) return;

    /* header */
    for (int i = 0; i < df->col_count; i++) {
        int w = (int)strlen(df->names[i]) < 20 ? 20 : (int)strlen(df->names[i]);
        printf("%-*s", w, df->names[i]);
        if (i < df->col_count - 1) printf(" | ");
    }
    printf("\n");

    /* separator */
    for (int i = 0; i < df->col_count; i++) {
        int w = (int)strlen(df->names[i]) < 20 ? 20 : (int)strlen(df->names[i]);
        for (int j = 0; j < w; j++) putchar('-');
        if (i < df->col_count - 1) printf("-+-");
    }
    printf("\n");

    /* data rows */
    for (int r = 0; r < df->row_count; r++) {
        for (int i = 0; i < df->col_count; i++) {
            int w = (int)strlen(df->names[i]) < 20 ? 20 : (int)strlen(df->names[i]);
            switch (df->types[i]) {
                case DF_INT:
                    printf("%-*d", w, df->data[i].i[r]);
                    break;
                case DF_FLOAT:
                    printf("%-*g", w, (double)df->data[i].f[r]);
                    break;
                case DF_DOUBLE:
                    printf("%-*.10g", w, df->data[i].d[r]);
                    break;
                case DF_STRING:
                    printf("%-*s", w, df->data[i].s[r]);
                    break;
            }
            if (i < df->col_count - 1) printf(" | ");
        }
        printf("\n");
    }
}

/* ---- public: column access ---- */

#define DF_MAKE_GETTER(ctype, dtype, dtype_enum, member)                    \
    Df##ctype df_get_##dtype(const DataFrame *df, const char *name)        \
    {                                                                      \
        if (!df)  df__error("df_get_" #dtype ": df is NULL");              \
        if (!name) df__error("df_get_" #dtype ": name is NULL");           \
        int idx = df__col_index(df, name);                                 \
        if (idx < 0) {                                                     \
            char buf[256];                                                 \
            snprintf(buf, sizeof(buf), "column '%s' not found", name);     \
            df__error(buf);                                                \
        }                                                                  \
        if (df->types[idx] != dtype_enum) {                                \
            char buf[256];                                                 \
            snprintf(buf, sizeof(buf),                                     \
                     "column '%s' is not " #dtype " type", name);          \
            df__error(buf);                                                \
        }                                                                  \
        Df##ctype col;                                                     \
        col.data  = df->data[idx].member;                                  \
        col.count = df->row_count;                                         \
        return col;                                                        \
    }

DF_MAKE_GETTER(IntCol,    int,    DF_INT,    i)
DF_MAKE_GETTER(FloatCol,  float,  DF_FLOAT,  f)
DF_MAKE_GETTER(DoubleCol, double, DF_DOUBLE, d)
DF_MAKE_GETTER(StrCol,    string, DF_STRING, s)

/* ---- public: group_by + sum ---- */

DfGrouped df_group_by(const DataFrame *df, const char *by_cols[], int n_by)
{
    DfGrouped grp;
    grp.source = df;
    grp.n_by   = 0;

    if (!df)  df__error("df_group_by: df is NULL");
    if (!by_cols) df__error("df_group_by: by_cols is NULL");
    if (n_by < 1) df__error("df_group_by: n_by must be >= 1");
    if (n_by > DF_MAX_COLS) df__error("df_group_by: n_by exceeds DF_MAX_COLS");

    for (int i = 0; i < n_by; i++) {
        int idx = df__col_index(df, by_cols[i]);
        if (idx < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "df_group_by: column '%s' not found", by_cols[i]);
            df__error(buf);
        }
        grp.by_cols[i] = df->names[idx];
    }
    grp.n_by = n_by;
    return grp;
}

DataFrame *df_sum(const DfGrouped *grp)
{
    if (!grp)  df__error("df_sum: grp is NULL");
    if (!grp->source) df__error("df_sum: grouped source is NULL");
    if (grp->n_by < 1) df__error("df_sum: no group-by columns");

    const DataFrame *src = grp->source;
    int nr = src->row_count;

    /* ---- identify column roles ---- */
    int by_idx[DF_MAX_COLS];   /* indices of by-columns   */
    int num_idx[DF_MAX_COLS];  /* indices of numeric cols to sum */
    int n_sum = 0;

    for (int i = 0; i < grp->n_by; i++) {
        by_idx[i] = df__col_index(src, grp->by_cols[i]);
        /* by_idx[i] is guaranteed valid because df_group_by validated it */
    }

    for (int i = 0; i < src->col_count; i++) {
        /* skip by-columns */
        int is_by = 0;
        for (int j = 0; j < grp->n_by; j++) {
            if (i == by_idx[j]) { is_by = 1; break; }
        }
        if (is_by) continue;

        /* only numeric columns are summed */
        if (src->types[i] == DF_INT || src->types[i] == DF_FLOAT || src->types[i] == DF_DOUBLE) {
            num_idx[n_sum++] = i;
        }
    }

    /* ---- first pass: assign group IDs via a hash table ----
     * O(nr) average instead of the old O(nr * n_groups) nested-loop scan:
     * each row hashes to a bucket holding the (typically short) chain of
     * *distinct groups* seen in that bucket so far; df__rows_match still
     * makes the final call on any hash collision, so correctness doesn't
     * depend on the hash being collision-free, only performance does. */
    size_t alloc_rows = (size_t)(nr > 0 ? nr : 1);
    int *group_id    = (int *)malloc(alloc_rows * sizeof(int));
    int *group_first = (int *)malloc(alloc_rows * sizeof(int));
    int *group_next  = (int *)malloc(alloc_rows * sizeof(int)); /* per-group chain link, indexed by group id */

    size_t bucket_cap = df__next_pow2(alloc_rows);
    size_t bucket_mask = bucket_cap - 1;
    int *bucket_head = (int *)malloc(bucket_cap * sizeof(int));

    if (!group_id || !group_first || !group_next || !bucket_head) {
        free(group_id); free(group_first); free(group_next); free(bucket_head);
        return NULL;
    }
    for (size_t b = 0; b < bucket_cap; b++) bucket_head[b] = -1;

    int n_groups = 0;

    for (int r = 0; r < nr; r++) {
        uint64_t h = df__hash_row(src, r, by_idx, grp->n_by);
        size_t b = (size_t)h & bucket_mask;

        int found = -1;
        for (int g = bucket_head[b]; g != -1; g = group_next[g]) {
            if (df__rows_match(src, r, group_first[g], by_idx, grp->n_by)) {
                found = g;
                break;
            }
        }

        if (found >= 0) {
            group_id[r] = found;
        } else {
            int g = n_groups++;
            group_first[g] = r;
            group_id[r] = g;
            group_next[g] = bucket_head[b];
            bucket_head[b] = g;
        }
    }

    free(group_next);
    free(bucket_head);

    /* ---- build result ---- */
    int out_cols = grp->n_by + n_sum;
    if (out_cols > DF_MAX_COLS) {
        free(group_id);
        free(group_first);
        df__error("df_sum: result exceeds DF_MAX_COLS");
    }

    DataFrame *res = df_new(out_cols);
    if (!res) { free(group_id); free(group_first); return NULL; }

    int out = 0;
    size_t alloc_n = (size_t)(n_groups > 0 ? n_groups : 1);

    /* by-columns: gather each group's representative value, then hand the
     * buffer to the frame builder (which copies it in) */
    for (int j = 0; j < grp->n_by; j++, out++) {
        int src_col = by_idx[j];

        switch (src->types[src_col]) {
            case DF_INT: {
                int *buf = (int *)malloc(alloc_n * sizeof(int));
                if (!buf) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    buf[g] = src->data[src_col].i[group_first[g]];
                df_set_column_int(res, out, src->names[src_col], buf, n_groups);
                free(buf);
                break;
            }
            case DF_FLOAT: {
                float *buf = (float *)malloc(alloc_n * sizeof(float));
                if (!buf) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    buf[g] = src->data[src_col].f[group_first[g]];
                df_set_column_float(res, out, src->names[src_col], buf, n_groups);
                free(buf);
                break;
            }
            case DF_DOUBLE: {
                double *buf = (double *)malloc(alloc_n * sizeof(double));
                if (!buf) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    buf[g] = src->data[src_col].d[group_first[g]];
                df_set_column_double(res, out, src->names[src_col], buf, n_groups);
                free(buf);
                break;
            }
            case DF_STRING: {
                char **buf = (char **)malloc(alloc_n * sizeof(char *));
                if (!buf) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    buf[g] = src->data[src_col].s[group_first[g]];
                df_set_column_string(res, out, src->names[src_col], buf, n_groups);
                free(buf); /* buf holds pointers borrowed from src; df_set_column_string strdup'd them */
                break;
            }
        }
    }

    /* summed columns: always DF_DOUBLE in the output */
    for (int j = 0; j < n_sum; j++, out++) {
        int src_col = num_idx[j];

        double *buf = (double *)calloc(alloc_n, sizeof(double));
        if (!buf) { df_free(res); free(group_id); free(group_first); return NULL; }

        for (int r = 0; r < nr; r++)
            buf[group_id[r]] += df__as_double(src, src_col, r);

        df_set_column_double(res, out, src->names[src_col], buf, n_groups);
        free(buf);
    }

    free(group_id);
    free(group_first);
    return res;
}

/* ---- public: sort ---- */

int *df_sort(const DataFrame *df, const char *by_col)
{
    if (!df) df__error("df_sort: df is NULL");
    if (!by_col) df__error("df_sort: by_col is NULL");

    int col = df__col_index(df, by_col);
    if (col < 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "df_sort: column '%s' not found", by_col);
        df__error(buf);
    }

    int n = df->row_count;
    int *idx = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!idx) return NULL;
    for (int i = 0; i < n; i++) idx[i] = i;
    if (n <= 1) return idx;

    int *tmp = (int *)malloc((size_t)n * sizeof(int));
    if (!tmp) { free(idx); return NULL; }

    df__merge_sort(idx, tmp, 0, n, df, col, df__cmp_for_type(df->types[col]));

    free(tmp);
    return idx;
}

/* ---- public: gather ---- */

#define DF_MAKE_GATHER(SUF, dtype, ctype)                                             \
    Df##SUF df_gather_##dtype(Df##SUF col, const int *idx, int idx_count)             \
    {                                                                                 \
        if (idx_count < 0) df__error("df_gather_" #dtype ": idx_count must be >= 0"); \
        if (idx_count > 0 && !idx) df__error("df_gather_" #dtype ": idx is NULL");    \
                                                                                        \
        ctype *out_data =                                                             \
            (ctype *)malloc((size_t)(idx_count > 0 ? idx_count : 1) * sizeof(ctype)); \
        if (!out_data) df__error("df_gather_" #dtype ": allocation failed");          \
                                                                                        \
        for (int k = 0; k < idx_count; k++) {                                         \
            int i = idx[k];                                                           \
            if (i < 0 || i >= col.count) {                                            \
                char buf[256];                                                        \
                snprintf(buf, sizeof(buf),                                            \
                         "df_gather_" #dtype ": index %d out of range [0,%d)",        \
                         i, col.count);                                               \
                free(out_data);                                                       \
                df__error(buf);                                                       \
            }                                                                         \
            out_data[k] = col.data[i];                                                \
        }                                                                             \
                                                                                        \
        Df##SUF result;                                                               \
        result.data  = out_data;                                                      \
        result.count = idx_count;                                                     \
        return result;                                                                \
    }

DF_MAKE_GATHER(IntCol,    int,    int)
DF_MAKE_GATHER(FloatCol,  float,  float)
DF_MAKE_GATHER(DoubleCol, double, double)

#undef DF_MAKE_GATHER

DfStrCol df_gather_string(DfStrCol col, const int *idx, int idx_count)
{
    if (idx_count < 0) df__error("df_gather_string: idx_count must be >= 0");
    if (idx_count > 0 && !idx) df__error("df_gather_string: idx is NULL");

    char **out_data = (char **)malloc((size_t)(idx_count > 0 ? idx_count : 1) * sizeof(char *));
    if (!out_data) df__error("df_gather_string: allocation failed");

    for (int k = 0; k < idx_count; k++) {
        int i = idx[k];
        if (i < 0 || i >= col.count) {
            char buf[256];
            snprintf(buf, sizeof(buf), "df_gather_string: index %d out of range [0,%d)", i, col.count);
            free(out_data);
            df__error(buf);
        }
        out_data[k] = strdup(col.data[i]);
        if (!out_data[k]) {
            for (int j = 0; j < k; j++) free(out_data[j]);
            free(out_data);
            df__error("df_gather_string: allocation failed");
        }
    }

    DfStrCol result;
    result.data  = out_data;
    result.count = idx_count;
    return result;
}

/* ---- public: match / assert_unique ---- */

DfMatchResult df_match(DfIntCol left_key, DfIntCol right_key)
{
    int rn = right_key.count;
    size_t cap = df__next_pow2((size_t)(rn > 0 ? rn : 1));
    size_t mask = cap - 1;

    int *bucket_head = (int *)malloc(cap * sizeof(int));
    int *next        = (int *)malloc((size_t)(rn > 0 ? rn : 1) * sizeof(int));
    if (!bucket_head || !next) {
        free(bucket_head); free(next);
        df__error("df_match: allocation failed");
    }
    for (size_t b = 0; b < cap; b++) bucket_head[b] = -1;
    for (int j = 0; j < rn; j++) {
        size_t b = (size_t)df__hash_int(right_key.data[j]) & mask;
        next[j] = bucket_head[b];
        bucket_head[b] = j;
    }

    int n = left_key.count;
    int *lidx = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    int *ridx = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!lidx || !ridx) {
        free(bucket_head); free(next); free(lidx); free(ridx);
        df__error("df_match: allocation failed");
    }

    for (int i = 0; i < n; i++) {
        int key = left_key.data[i];
        size_t b = (size_t)df__hash_int(key) & mask;

        int found = -1;
        for (int j = bucket_head[b]; j != -1; j = next[j]) {
            if (right_key.data[j] == key) { found = j; break; }
        }
        if (found < 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "df_match: left key %d has no match in right_key", key);
            free(bucket_head); free(next); free(lidx); free(ridx);
            df__error(buf);
        }
        lidx[i] = i;
        ridx[i] = found;
    }

    free(bucket_head);
    free(next);

    DfMatchResult result;
    result.left_idx  = lidx;
    result.right_idx = ridx;
    result.count     = n;
    return result;
}

DfMatchResult df_match_string(DfStrCol left_key, DfStrCol right_key)
{
    int rn = right_key.count;
    size_t cap = df__next_pow2((size_t)(rn > 0 ? rn : 1));
    size_t mask = cap - 1;

    int *bucket_head = (int *)malloc(cap * sizeof(int));
    int *next        = (int *)malloc((size_t)(rn > 0 ? rn : 1) * sizeof(int));
    if (!bucket_head || !next) {
        free(bucket_head); free(next);
        df__error("df_match_string: allocation failed");
    }
    for (size_t b = 0; b < cap; b++) bucket_head[b] = -1;
    for (int j = 0; j < rn; j++) {
        size_t b = (size_t)df__hash_str(right_key.data[j]) & mask;
        next[j] = bucket_head[b];
        bucket_head[b] = j;
    }

    int n = left_key.count;
    int *lidx = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    int *ridx = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!lidx || !ridx) {
        free(bucket_head); free(next); free(lidx); free(ridx);
        df__error("df_match_string: allocation failed");
    }

    for (int i = 0; i < n; i++) {
        const char *key = left_key.data[i];
        size_t b = (size_t)df__hash_str(key) & mask;

        int found = -1;
        for (int j = bucket_head[b]; j != -1; j = next[j]) {
            if (strcmp(right_key.data[j], key) == 0) { found = j; break; }
        }
        if (found < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "df_match_string: left key '%s' has no match in right_key", key);
            free(bucket_head); free(next); free(lidx); free(ridx);
            df__error(buf);
        }
        lidx[i] = i;
        ridx[i] = found;
    }

    free(bucket_head);
    free(next);

    DfMatchResult result;
    result.left_idx  = lidx;
    result.right_idx = ridx;
    result.count     = n;
    return result;
}

void df_assert_unique(DfIntCol key)
{
    int n = key.count;
    size_t cap = df__next_pow2((size_t)(n > 0 ? n : 1));
    size_t mask = cap - 1;

    int *bucket_head = (int *)malloc(cap * sizeof(int));
    int *next        = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!bucket_head || !next) {
        free(bucket_head); free(next);
        df__error("df_assert_unique: allocation failed");
    }
    for (size_t b = 0; b < cap; b++) bucket_head[b] = -1;

    for (int i = 0; i < n; i++) {
        int v = key.data[i];
        size_t b = (size_t)df__hash_int(v) & mask;
        for (int j = bucket_head[b]; j != -1; j = next[j]) {
            if (key.data[j] == v) {
                char buf[128];
                snprintf(buf, sizeof(buf), "df_assert_unique: duplicate value %d at rows %d and %d", v, j, i);
                free(bucket_head); free(next);
                df__error(buf);
            }
        }
        next[i] = bucket_head[b];
        bucket_head[b] = i;
    }

    free(bucket_head);
    free(next);
}

void df_assert_unique_string(DfStrCol key)
{
    int n = key.count;
    size_t cap = df__next_pow2((size_t)(n > 0 ? n : 1));
    size_t mask = cap - 1;

    int *bucket_head = (int *)malloc(cap * sizeof(int));
    int *next        = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!bucket_head || !next) {
        free(bucket_head); free(next);
        df__error("df_assert_unique_string: allocation failed");
    }
    for (size_t b = 0; b < cap; b++) bucket_head[b] = -1;

    for (int i = 0; i < n; i++) {
        const char *v = key.data[i];
        size_t b = (size_t)df__hash_str(v) & mask;
        for (int j = bucket_head[b]; j != -1; j = next[j]) {
            if (strcmp(key.data[j], v) == 0) {
                char buf[320];
                snprintf(buf, sizeof(buf),
                         "df_assert_unique_string: duplicate value '%s' at rows %d and %d", v, j, i);
                free(bucket_head); free(next);
                df__error(buf);
            }
        }
        next[i] = bucket_head[b];
        bucket_head[b] = i;
    }

    free(bucket_head);
    free(next);
}

/* ---- public: resample ---- */

DfIntCol df_resample(DfIntCol residual_idx, DfIntCol weight_key, DfDoubleCol weights, unsigned seed)
{
    if (weight_key.count != weights.count)
        df__error("df_resample: weight_key.count and weights.count must match");

    int wn = weight_key.count;
    size_t cap = df__next_pow2((size_t)(wn > 0 ? wn : 1));
    size_t mask = cap - 1;

    int *bucket_head = (int *)malloc(cap * sizeof(int));
    int *next        = (int *)malloc((size_t)(wn > 0 ? wn : 1) * sizeof(int));
    if (!bucket_head || !next) {
        free(bucket_head); free(next);
        df__error("df_resample: allocation failed");
    }
    for (size_t b = 0; b < cap; b++) bucket_head[b] = -1;
    for (int j = 0; j < wn; j++) {
        size_t b = (size_t)df__hash_int(weight_key.data[j]) & mask;
        next[j] = bucket_head[b];
        bucket_head[b] = j;
    }

    int n = residual_idx.count;
    int *out = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!out) {
        free(bucket_head); free(next);
        df__error("df_resample: allocation failed");
    }

    /* Mix the seed through splitmix64 once up front so seed=0 and
     * seed=1 don't start from adjacent, visibly-correlated states. */
    uint64_t rng_state = (uint64_t)seed;
    (void)df__splitmix64_next(&rng_state);

    int out_count = 0;
    for (int k = 0; k < n; k++) {
        int g = residual_idx.data[k];
        size_t b = (size_t)df__hash_int(g) & mask;

        double total = 0.0;
        for (int j = bucket_head[b]; j != -1; j = next[j])
            if (weight_key.data[j] == g) total += weights.data[j];

        /* Draw once per residual row regardless of total weight, so a
         * later row's draw never shifts depending on how many earlier
         * rows happened to land in a zero-weight group. */
        double r = df__rand_uniform(&rng_state) * total;

        if (total <= 0.0) continue; /* no output for this row -- caller's job to retry at a coarser key */

        double cum = 0.0;
        int picked = -1;
        for (int j = bucket_head[b]; j != -1; j = next[j]) {
            if (weight_key.data[j] != g) continue;
            cum += weights.data[j];
            if (r < cum) { picked = j; break; }
        }
        if (picked < 0) {
            /* r landed exactly on (or past, by rounding) the cumulative
             * total -- fall back to the group's last pool row rather than
             * dropping a draw that should have counted. */
            for (int j = bucket_head[b]; j != -1; j = next[j])
                if (weight_key.data[j] == g) picked = j;
        }
        out[out_count++] = picked;
    }

    free(bucket_head);
    free(next);

    DfIntCol result;
    result.data  = out;
    result.count = out_count;
    return result;
}

/* ---- public: vertical stack ---- */

#define DF_MAKE_STACK_V(SUF, dtype, ctype)                                          \
    Df##SUF df_stack_v_##dtype(Df##SUF *cols, int n)                                \
    {                                                                               \
        if (n < 0) df__error("df_stack_v_" #dtype ": n must be >= 0");              \
        if (n > 0 && !cols) df__error("df_stack_v_" #dtype ": cols is NULL");        \
                                                                                      \
        int total = 0;                                                              \
        for (int c = 0; c < n; c++) {                                               \
            if (cols[c].count < 0)                                                  \
                df__error("df_stack_v_" #dtype ": a source column has negative count"); \
            total += cols[c].count;                                                 \
        }                                                                            \
                                                                                      \
        ctype *out = (ctype *)malloc((size_t)(total > 0 ? total : 1) * sizeof(ctype)); \
        if (!out) df__error("df_stack_v_" #dtype ": allocation failed");             \
                                                                                      \
        int pos = 0;                                                                \
        for (int c = 0; c < n; c++) {                                               \
            if (cols[c].count > 0)                                                  \
                memcpy(out + pos, cols[c].data, (size_t)cols[c].count * sizeof(ctype)); \
            pos += cols[c].count;                                                   \
        }                                                                            \
                                                                                      \
        Df##SUF result;                                                             \
        result.data  = out;                                                         \
        result.count = total;                                                       \
        return result;                                                              \
    }

DF_MAKE_STACK_V(IntCol,    int,    int)
DF_MAKE_STACK_V(FloatCol,  float,  float)
DF_MAKE_STACK_V(DoubleCol, double, double)

#undef DF_MAKE_STACK_V

DfStrCol df_stack_v_string(DfStrCol *cols, int n)
{
    if (n < 0) df__error("df_stack_v_string: n must be >= 0");
    if (n > 0 && !cols) df__error("df_stack_v_string: cols is NULL");

    int total = 0;
    for (int c = 0; c < n; c++) {
        if (cols[c].count < 0) df__error("df_stack_v_string: a source column has negative count");
        total += cols[c].count;
    }

    char **out = (char **)malloc((size_t)(total > 0 ? total : 1) * sizeof(char *));
    if (!out) df__error("df_stack_v_string: allocation failed");

    int pos = 0;
    for (int c = 0; c < n; c++) {
        for (int r = 0; r < cols[c].count; r++) {
            out[pos] = strdup(cols[c].data[r]);
            if (!out[pos]) {
                for (int j = 0; j < pos; j++) free(out[j]);
                free(out);
                df__error("df_stack_v_string: allocation failed");
            }
            pos++;
        }
    }

    DfStrCol result;
    result.data  = out;
    result.count = total;
    return result;
}

#endif /* SKN_DF_IMPLEMENTATION */
#endif /* SKN_DF_H */

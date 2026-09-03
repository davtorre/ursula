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
 *      df_map1()/df_map2()/df_map_scalar() — elementwise ops via C function pointers
 *      df_map1_arr()/df_map2_arr()/df_map_scalar_arr() — same, on raw double* arrays
 *      df_cast_to_int()/df_cast_to_float() — explicit, opt-in narrowing cast
 *      df_shuffle_within_groups()/_string() — seeded, group-boundary-preserving shuffle
 *      df_pair()/df_pair_string() — merge-style within-group positional pairing
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
#include <limits.h>

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

/* Result of df_pair/df_pair_string: original-frame row indices, split into
 * matched pairs and demand-side leftovers. demand_idx[k]/supply_idx[k]
 * share a key value, for every k in [0, matched_count). residual_idx holds
 * every demand row df_pair couldn't pair (including a whole demand group
 * absent from supply). What happens to residual_idx next — retry at a
 * coarser key, df_resample it, or treat it as a terminal gap — is the
 * caller's decision, same boundary df_resample's own residual already
 * drew. */
typedef struct {
    int *demand_idx;
    int *supply_idx;
    int  matched_count;
    int *residual_idx;
    int  residual_count;
} DfPairResult;

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

/* Elementwise column operations via real C function pointers — <math.h>'s
 * own signatures, no dispatch table, no string/enum op-code. col (and
 * col_a/col_b) must be numeric (DF_INT/DF_FLOAT/DF_DOUBLE); a DF_STRING
 * column, a column not found, or fn == NULL are all df__error fail-fasts,
 * same convention as df_get_*'s "column not found". Non-double columns are
 * coerced per-row via the existing df__as_double helper (the same one
 * df_sum already uses) — output is always DfDoubleCol regardless of the
 * source column's type, since these are inherently floating-point
 * operations. Freshly heap-allocated, caller-owned (free() the usual
 * way, same shape as df_gather_double's return) — df or its columns are
 * never mutated; compose the result into a frame yourself via
 * df_new/df_set_column_double, same separation PM-004 drew between
 * df_stack_v and df_set_column_*. */
DfDoubleCol df_map1(const DataFrame *df, const char *col, double (*fn)(double));

/* col_a and col_b must have equal row counts (both come from the same df,
 * so they always will through the public API — checked explicitly anyway,
 * df__error fail-fast on a mismatch rather than silently truncating to
 * the shorter one). Row k of the output is fn(as_double(col_a[k]),
 * as_double(col_b[k])). */
DfDoubleCol df_map2(const DataFrame *df, const char *col_a, const char *col_b,
                     double (*fn)(double, double));

/* Row k of the output is fn(as_double(col[k]), scalar) — col's value is
 * always fn's first argument, scalar always the second; use the *_r
 * helpers below to flip operand order for non-commutative operators. */
DfDoubleCol df_map_scalar(const DataFrame *df, const char *col, double scalar,
                           double (*fn)(double, double));

/* Named arithmetic helpers for df_map_scalar/df_map2 — <math.h> doesn't
 * provide +, -, *, / as functions. Deliberately not named sum/diff/etc.
 * to avoid colliding with df_sum's existing aggregation meaning. */
double df_add (double a, double b); /* a + b */
double df_sub (double a, double b); /* a - b   e.g. column_A - 2 -> df_map_scalar(df, "column_A", 2.0, df_sub)  */
double df_rsub(double a, double b); /* b - a   e.g. 2 - column_A -> df_map_scalar(df, "column_A", 2.0, df_rsub) */
double df_mul (double a, double b); /* a * b */
double df_div (double a, double b); /* a / b   e.g. column_A / 2 -> df_map_scalar(df, "column_A", 2.0, df_div)  */
double df_rdiv(double a, double b); /* b / a   e.g. 2 / column_A -> df_map_scalar(df, "column_A", 2.0, df_rdiv) */

/* Array-level counterparts to df_map1/df_map2/df_map_scalar: same
 * elementwise operation, operating on a raw double* (or a DfDoubleCol's
 * .data/.count) instead of a (DataFrame*, column name) pair — so a map
 * chains directly off another map's or df_gather_*'s output without a
 * df_new/df_set_column_double/df_get_double round-trip back into
 * DataFrame land. df_map1/df_map2/df_map_scalar are unchanged and stay
 * implemented independently of these (see skn_df.h's PM-006 note above
 * df_map1_arr's implementation for why). Freshly heap-allocated,
 * caller-owned output — identical ownership shape to their DataFrame-based
 * siblings and to df_gather_double. df_add/df_sub/df_rsub/df_mul/df_div/
 * df_rdiv work unchanged as df_map_scalar_arr's fn. */

/* out[r] = fn(in[r]) for r in [0,n). in NULL with n > 0, or fn NULL, is a
 * df__error fail-fast (same convention as df_map1). */
DfDoubleCol df_map1_arr(const double *in, int n, double (*fn)(double));

/* n_a and n_b are taken separately, not a single shared n -- unlike
 * df_map2's column-name version, a and b have no structural guarantee of
 * equal length here, so n_a != n_b is a real, live df__error fail-fast,
 * not a defensive-only formality. out[r] = fn(a[r], b[r]) for r in
 * [0,n_a) once the lengths are confirmed equal. a/b NULL with n > 0, or
 * fn NULL, fail fast the same way. */
DfDoubleCol df_map2_arr(const double *a, int n_a, const double *b, int n_b,
                         double (*fn)(double, double));

/* out[r] = fn(in[r], scalar) for r in [0,n). Same NULL/fn fail-fast rules
 * as df_map1_arr. */
DfDoubleCol df_map_scalar_arr(const double *in, int n, double scalar,
                               double (*fn)(double, double));

/* Explicit, opt-in narrowing casts from DfDoubleCol (df_map1/df_map2/
 * df_map_scalar and their _arr siblings' output shape) back to
 * DF_INT/DF_FLOAT. Deliberately not automatic inference inside those
 * functions — see the PM-007 brief for why a bare double(*)(double)
 * carries no information about whether its result is
 * meant to be integral, and why two differently-typed input columns have
 * no single "original type" to cast back to. The caller decides.
 *
 * Both fail fast (df__error, same convention as df_get_*'s "column not
 * found") on a NaN or +-Infinity element, and on the narrower type not
 * being able to represent the (rounded, for int) result. col.count == 0
 * doesn't crash, same allocation-guard pattern as df_map1_arr etc. */

/* Rounds each element to the nearest integer via round() (round-half-
 * away-from-zero -- chosen over rint/nearbyint specifically because those
 * depend on the current floating-point rounding mode) before narrowing,
 * not a truncating (int) cast. df__error fail-fast if the rounded value
 * falls outside [INT_MIN, INT_MAX] -- a real overflow to check, not
 * defensive-only, since a df_map_scalar result can genuinely be out of
 * int range. */
DfIntCol df_cast_to_int(DfDoubleCol col);

/* Narrows each element to float via a plain C cast (double -> float
 * narrowing already rounds to the nearest representable float per IEEE
 * 754 -- no extra rounding logic needed, unlike the int case). df__error
 * fail-fast if the resulting float is +-Infinity while the input double
 * was finite (checked on the output, not just the input) -- that's
 * magnitude overflow: finite but too large for float's range. */
DfFloatCol df_cast_to_float(DfDoubleCol col);

/* Within-group shuffle: sorted_idx/sorted_key must be df_sort's output and
 * the group-key column gathered through it (df_gather_int/df_gather_string
 * on sorted_idx) — both length n; sorted_key.count != n is a df__error
 * fail-fast. Scans sorted_key once for contiguous runs of equal value
 * (cheap: it's already sorted, a run boundary is just "value differs from
 * the previous element") and applies an independent seeded Fisher-Yates
 * permutation to each run's slice of sorted_idx's *positions* — values in
 * sorted_idx are never altered, only reordered within their own run, so
 * group boundaries never move. Seeded and reproducible: two calls with
 * the same seed and inputs are byte-identical, same determinism bar
 * df_sort/df_resample already established. Returns a freshly
 * heap-allocated int* of length n, caller-owned (free() the usual way);
 * n == 0 doesn't crash. */
int *df_shuffle_within_groups       (const int *sorted_idx, DfIntCol sorted_key, int n, unsigned seed);
int *df_shuffle_within_groups_string(const int *sorted_idx, DfStrCol sorted_key, int n, unsigned seed);

/* Merge-style two-pointer pairing: demand_idx/demand_key and
 * supply_idx/supply_key are each expected to already be
 * grouped-and-shuffled-within-group (df_sort -> df_gather_* ->
 * df_shuffle_within_groups -> df_gather_* again with the shuffled index)
 * — df_pair does not re-sort or re-shuffle anything itself, it trusts its
 * inputs are already in that shape. Because both key arrays are
 * independently sorted by the same key ordering, distinct group values
 * appear in the same relative order on both sides, so a single
 * merge-style scan (no hashing) finds each demand run's matching supply
 * run. Per matching group, takes min(demand_run_len, supply_run_len)
 * pairs from the *start* of each run — within-run order is already
 * randomized by df_shuffle_within_groups, so "start of run" is exactly as
 * random as any other position. Any demand-run remainder (including a
 * whole demand run absent from supply) becomes residual_idx. Deciding
 * what happens to residual_idx next is not this function's job — same
 * boundary df_resample's own residual already drew. n_demand == 0 yields
 * an entirely empty result; n_supply == 0 sends all of demand_idx to
 * residual_idx. Output arrays are caller-owned, freed individually. */
DfPairResult df_pair       (const int *demand_idx, DfIntCol demand_key, int n_demand,
                             const int *supply_idx, DfIntCol supply_key, int n_supply);
DfPairResult df_pair_string(const int *demand_idx, DfStrCol demand_key, int n_demand,
                             const int *supply_idx, DfStrCol supply_key, int n_supply);

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

/* In-place Fisher-Yates shuffle of arr[lo, hi), seeded via *state.
 * Used by df_shuffle_within_groups(_string) to permute one run at a time. */
static void df__fisher_yates(int *arr, int lo, int hi, uint64_t *state)
{
    for (int i = hi - 1; i > lo; i--) {
        int range = i - lo + 1;
        int j = lo + (int)(df__rand_uniform(state) * (double)range);
        if (j > i) j = i; /* guard the extremely rare uniform() rounding up to 1.0 */
        int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
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
        case DF_DOUBLE: {
            snprintf(buf, sizeof(buf), "%.17g", df->data[col].d[row]);
            /* "%.17g" of an exact integer value (e.g. 1.0) produces "1" --
             * indistinguishable from a real int on reload, since
             * csv__infer_cell only classifies a digit run followed by '.'
             * as CSV_DOUBLE (a bare digit run is CSV_INT). If the buffer
             * is nothing but an optional '-' and digits, append ".0" so
             * it round-trips as DF_DOUBLE, not DF_INT. Doesn't apply to
             * "inf"/"nan"/exponent forms -- those already contain a
             * non-digit character and are left alone. */
            int all_digits = 1;
            for (const char *p = buf; *p; p++) {
                if (*p == '-' && p == buf) continue;
                if (*p < '0' || *p > '9') { all_digits = 0; break; }
            }
            if (all_digits) strncat(buf, ".0", sizeof(buf) - strlen(buf) - 1);
            df__csv_write_field(fp, buf);
            break;
        }
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

/* ---- public: elementwise map ---- */

/* Shared column lookup + numeric-type check for df_map1/df_map2/df_map_scalar.
 * Same fail-fast convention as df_get_*'s "column not found". */
static int df__map_col_check(const DataFrame *df, const char *col, const char *fname)
{
    char buf[256];
    if (!df)  { snprintf(buf, sizeof(buf), "%s: df is NULL", fname); df__error(buf); }
    if (!col) { snprintf(buf, sizeof(buf), "%s: column name is NULL", fname); df__error(buf); }

    int idx = df__col_index(df, col);
    if (idx < 0) {
        snprintf(buf, sizeof(buf), "%s: column '%s' not found", fname, col);
        df__error(buf);
    }
    if (df->types[idx] == DF_STRING) {
        snprintf(buf, sizeof(buf), "%s: column '%s' is not numeric", fname, col);
        df__error(buf);
    }
    return idx;
}

DfDoubleCol df_map1(const DataFrame *df, const char *col, double (*fn)(double))
{
    int idx = df__map_col_check(df, col, "df_map1");
    if (!fn) df__error("df_map1: fn is NULL");

    int n = df->row_count;
    double *out = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!out) df__error("df_map1: allocation failed");

    for (int r = 0; r < n; r++)
        out[r] = fn(df__as_double(df, idx, r));

    DfDoubleCol result;
    result.data  = out;
    result.count = n;
    return result;
}

DfDoubleCol df_map2(const DataFrame *df, const char *col_a, const char *col_b,
                     double (*fn)(double, double))
{
    int idx_a = df__map_col_check(df, col_a, "df_map2");
    int idx_b = df__map_col_check(df, col_b, "df_map2");
    if (!fn) df__error("df_map2: fn is NULL");

    /* col_a and col_b are both columns of df, and DataFrame stores a
     * single shared row_count for every column (df__set_column_check,
     * PM-002, refuses to construct a df where a column's length disagrees
     * with the others) -- so there is no independent "col_a length" for
     * col_b's to mismatch against through the public API. Using
     * df->row_count once, for both, below, *is* the explicit check: it
     * would be dishonest to compare df->row_count to itself and call that
     * a real mismatch guard. See test_map.c for the full reasoning. */
    int n = df->row_count;
    double *out = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!out) df__error("df_map2: allocation failed");

    for (int r = 0; r < n; r++)
        out[r] = fn(df__as_double(df, idx_a, r), df__as_double(df, idx_b, r));

    DfDoubleCol result;
    result.data  = out;
    result.count = n;
    return result;
}

DfDoubleCol df_map_scalar(const DataFrame *df, const char *col, double scalar,
                           double (*fn)(double, double))
{
    int idx = df__map_col_check(df, col, "df_map_scalar");
    if (!fn) df__error("df_map_scalar: fn is NULL");

    int n = df->row_count;
    double *out = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!out) df__error("df_map_scalar: allocation failed");

    for (int r = 0; r < n; r++)
        out[r] = fn(df__as_double(df, idx, r), scalar);

    DfDoubleCol result;
    result.data  = out;
    result.count = n;
    return result;
}

double df_add (double a, double b) { return a + b; }
double df_sub (double a, double b) { return a - b; }
double df_rsub(double a, double b) { return b - a; }
double df_mul (double a, double b) { return a * b; }
double df_div (double a, double b) { return a / b; }
double df_rdiv(double a, double b) { return b / a; }

/* ---- public: array-level map (PM-006) ----
 * Deliberately independent of df_map1/df_map2/df_map_scalar above, not
 * implemented in terms of them or vice versa -- routing the DataFrame
 * versions through these would mean materializing a temporary coercion
 * buffer for DF_INT/DF_FLOAT columns for no actual benefit, and isn't
 * worth the churn against already-shipped, already-tested code. */

DfDoubleCol df_map1_arr(const double *in, int n, double (*fn)(double))
{
    if (n < 0) df__error("df_map1_arr: n must be >= 0");
    if (n > 0 && !in) df__error("df_map1_arr: in is NULL");
    if (!fn) df__error("df_map1_arr: fn is NULL");

    double *out = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!out) df__error("df_map1_arr: allocation failed");

    for (int r = 0; r < n; r++)
        out[r] = fn(in[r]);

    DfDoubleCol result;
    result.data  = out;
    result.count = n;
    return result;
}

DfDoubleCol df_map2_arr(const double *a, int n_a, const double *b, int n_b,
                         double (*fn)(double, double))
{
    if (n_a < 0) df__error("df_map2_arr: n_a must be >= 0");
    if (n_b < 0) df__error("df_map2_arr: n_b must be >= 0");
    if (n_a > 0 && !a) df__error("df_map2_arr: a is NULL");
    if (n_b > 0 && !b) df__error("df_map2_arr: b is NULL");
    if (!fn) df__error("df_map2_arr: fn is NULL");
    if (n_a != n_b) {
        char buf[128];
        snprintf(buf, sizeof(buf), "df_map2_arr: length mismatch (n_a=%d, n_b=%d)", n_a, n_b);
        df__error(buf);
    }

    int n = n_a;
    double *out = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!out) df__error("df_map2_arr: allocation failed");

    for (int r = 0; r < n; r++)
        out[r] = fn(a[r], b[r]);

    DfDoubleCol result;
    result.data  = out;
    result.count = n;
    return result;
}

DfDoubleCol df_map_scalar_arr(const double *in, int n, double scalar,
                               double (*fn)(double, double))
{
    if (n < 0) df__error("df_map_scalar_arr: n must be >= 0");
    if (n > 0 && !in) df__error("df_map_scalar_arr: in is NULL");
    if (!fn) df__error("df_map_scalar_arr: fn is NULL");

    double *out = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!out) df__error("df_map_scalar_arr: allocation failed");

    for (int r = 0; r < n; r++)
        out[r] = fn(in[r], scalar);

    DfDoubleCol result;
    result.data  = out;
    result.count = n;
    return result;
}

/* ---- public: explicit narrowing cast (PM-007) ---- */

DfIntCol df_cast_to_int(DfDoubleCol col)
{
    int n = col.count;
    int *out = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!out) df__error("df_cast_to_int: allocation failed");

    for (int r = 0; r < n; r++) {
        double v = col.data[r];
        if (isnan(v) || isinf(v)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "df_cast_to_int: element %d is %s, not castable to int",
                     r, isnan(v) ? "NaN" : (v > 0 ? "+Infinity" : "-Infinity"));
            free(out);
            df__error(buf);
        }

        double rounded = round(v);
        if (rounded < (double)INT_MIN || rounded > (double)INT_MAX) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "df_cast_to_int: element %d (%.17g, rounded %.17g) is out of int range",
                     r, v, rounded);
            free(out);
            df__error(buf);
        }

        out[r] = (int)rounded;
    }

    DfIntCol result;
    result.data  = out;
    result.count = n;
    return result;
}

DfFloatCol df_cast_to_float(DfDoubleCol col)
{
    int n = col.count;
    float *out = (float *)malloc((size_t)(n > 0 ? n : 1) * sizeof(float));
    if (!out) df__error("df_cast_to_float: allocation failed");

    for (int r = 0; r < n; r++) {
        double v = col.data[r];
        if (isnan(v) || isinf(v)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "df_cast_to_float: element %d is %s, not castable to float",
                     r, isnan(v) ? "NaN" : (v > 0 ? "+Infinity" : "-Infinity"));
            free(out);
            df__error(buf);
        }

        float f = (float)v;
        if (isinf(f)) { /* finite double, but too large in magnitude for float's range */
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "df_cast_to_float: element %d (%.17g) overflows float's range", r, v);
            free(out);
            df__error(buf);
        }

        out[r] = f;
    }

    DfFloatCol result;
    result.data  = out;
    result.count = n;
    return result;
}

/* ---- public: within-group shuffle (PM-008) ---- */

int *df_shuffle_within_groups(const int *sorted_idx, DfIntCol sorted_key, int n, unsigned seed)
{
    if (n < 0) df__error("df_shuffle_within_groups: n must be >= 0");
    if (n > 0 && !sorted_idx) df__error("df_shuffle_within_groups: sorted_idx is NULL");
    if (sorted_key.count != n) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "df_shuffle_within_groups: sorted_key.count (%d) != n (%d)", sorted_key.count, n);
        df__error(buf);
    }

    int *out = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!out) df__error("df_shuffle_within_groups: allocation failed");
    if (n > 0) memcpy(out, sorted_idx, (size_t)n * sizeof(int));

    uint64_t rng_state = (uint64_t)seed;
    (void)df__splitmix64_next(&rng_state);

    int run_start = 0;
    for (int i = 1; i <= n; i++) {
        if (i == n || sorted_key.data[i] != sorted_key.data[i - 1]) {
            df__fisher_yates(out, run_start, i, &rng_state);
            run_start = i;
        }
    }

    return out;
}

int *df_shuffle_within_groups_string(const int *sorted_idx, DfStrCol sorted_key, int n, unsigned seed)
{
    if (n < 0) df__error("df_shuffle_within_groups_string: n must be >= 0");
    if (n > 0 && !sorted_idx) df__error("df_shuffle_within_groups_string: sorted_idx is NULL");
    if (sorted_key.count != n) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "df_shuffle_within_groups_string: sorted_key.count (%d) != n (%d)", sorted_key.count, n);
        df__error(buf);
    }

    int *out = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!out) df__error("df_shuffle_within_groups_string: allocation failed");
    if (n > 0) memcpy(out, sorted_idx, (size_t)n * sizeof(int));

    uint64_t rng_state = (uint64_t)seed;
    (void)df__splitmix64_next(&rng_state);

    int run_start = 0;
    for (int i = 1; i <= n; i++) {
        if (i == n || strcmp(sorted_key.data[i], sorted_key.data[i - 1]) != 0) {
            df__fisher_yates(out, run_start, i, &rng_state);
            run_start = i;
        }
    }

    return out;
}

/* ---- public: merge-style pairing (PM-008) ---- */

DfPairResult df_pair(const int *demand_idx, DfIntCol demand_key, int n_demand,
                      const int *supply_idx, DfIntCol supply_key, int n_supply)
{
    if (n_demand < 0) df__error("df_pair: n_demand must be >= 0");
    if (n_supply < 0) df__error("df_pair: n_supply must be >= 0");
    if (n_demand > 0 && !demand_idx) df__error("df_pair: demand_idx is NULL");
    if (n_supply > 0 && !supply_idx) df__error("df_pair: supply_idx is NULL");
    if (demand_key.count != n_demand) {
        char buf[160];
        snprintf(buf, sizeof(buf), "df_pair: demand_key.count (%d) != n_demand (%d)",
                 demand_key.count, n_demand);
        df__error(buf);
    }
    if (supply_key.count != n_supply) {
        char buf[160];
        snprintf(buf, sizeof(buf), "df_pair: supply_key.count (%d) != n_supply (%d)",
                 supply_key.count, n_supply);
        df__error(buf);
    }

    size_t cap = (size_t)(n_demand > 0 ? n_demand : 1);
    int *out_demand   = (int *)malloc(cap * sizeof(int));
    int *out_supply   = (int *)malloc(cap * sizeof(int));
    int *out_residual = (int *)malloc(cap * sizeof(int));
    if (!out_demand || !out_supply || !out_residual) {
        free(out_demand); free(out_supply); free(out_residual);
        df__error("df_pair: allocation failed");
    }

    int matched = 0, residual = 0;
    int di = 0, si = 0;

    while (di < n_demand) {
        int d_start = di;
        int key = demand_key.data[di];
        while (di < n_demand && demand_key.data[di] == key) di++;
        int d_len = di - d_start;

        while (si < n_supply && supply_key.data[si] < key) si++;

        int s_start = si;
        int s_len = 0;
        if (si < n_supply && supply_key.data[si] == key) {
            while (si < n_supply && supply_key.data[si] == key) si++;
            s_len = si - s_start;
        }

        int take = (d_len < s_len) ? d_len : s_len;
        for (int k = 0; k < take; k++) {
            out_demand[matched] = demand_idx[d_start + k];
            out_supply[matched] = supply_idx[s_start + k];
            matched++;
        }
        for (int k = take; k < d_len; k++)
            out_residual[residual++] = demand_idx[d_start + k];
    }

    DfPairResult result;
    result.demand_idx     = out_demand;
    result.supply_idx     = out_supply;
    result.matched_count   = matched;
    result.residual_idx   = out_residual;
    result.residual_count = residual;
    return result;
}

DfPairResult df_pair_string(const int *demand_idx, DfStrCol demand_key, int n_demand,
                             const int *supply_idx, DfStrCol supply_key, int n_supply)
{
    if (n_demand < 0) df__error("df_pair_string: n_demand must be >= 0");
    if (n_supply < 0) df__error("df_pair_string: n_supply must be >= 0");
    if (n_demand > 0 && !demand_idx) df__error("df_pair_string: demand_idx is NULL");
    if (n_supply > 0 && !supply_idx) df__error("df_pair_string: supply_idx is NULL");
    if (demand_key.count != n_demand) {
        char buf[160];
        snprintf(buf, sizeof(buf), "df_pair_string: demand_key.count (%d) != n_demand (%d)",
                 demand_key.count, n_demand);
        df__error(buf);
    }
    if (supply_key.count != n_supply) {
        char buf[160];
        snprintf(buf, sizeof(buf), "df_pair_string: supply_key.count (%d) != n_supply (%d)",
                 supply_key.count, n_supply);
        df__error(buf);
    }

    size_t cap = (size_t)(n_demand > 0 ? n_demand : 1);
    int *out_demand   = (int *)malloc(cap * sizeof(int));
    int *out_supply   = (int *)malloc(cap * sizeof(int));
    int *out_residual = (int *)malloc(cap * sizeof(int));
    if (!out_demand || !out_supply || !out_residual) {
        free(out_demand); free(out_supply); free(out_residual);
        df__error("df_pair_string: allocation failed");
    }

    int matched = 0, residual = 0;
    int di = 0, si = 0;

    while (di < n_demand) {
        int d_start = di;
        const char *key = demand_key.data[di];
        while (di < n_demand && strcmp(demand_key.data[di], key) == 0) di++;
        int d_len = di - d_start;

        while (si < n_supply && strcmp(supply_key.data[si], key) < 0) si++;

        int s_start = si;
        int s_len = 0;
        if (si < n_supply && strcmp(supply_key.data[si], key) == 0) {
            while (si < n_supply && strcmp(supply_key.data[si], key) == 0) si++;
            s_len = si - s_start;
        }

        int take = (d_len < s_len) ? d_len : s_len;
        for (int k = 0; k < take; k++) {
            out_demand[matched] = demand_idx[d_start + k];
            out_supply[matched] = supply_idx[s_start + k];
            matched++;
        }
        for (int k = take; k < d_len; k++)
            out_residual[residual++] = demand_idx[d_start + k];
    }

    DfPairResult result;
    result.demand_idx     = out_demand;
    result.supply_idx     = out_supply;
    result.matched_count   = matched;
    result.residual_idx   = out_residual;
    result.residual_count = residual;
    return result;
}

#endif /* SKN_DF_IMPLEMENTATION */
#endif /* SKN_DF_H */

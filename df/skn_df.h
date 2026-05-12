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
 *      df_get_int/float/double/string — typed column access
 *      df_group_by()   — lazy group-by (returns DfGrouped, no computation)
 *      df_sum()        — terminal sum aggregation (returns new DataFrame)
 *      df_print()      — print to stdout
 *      df_free()       — free memory
 */

#ifndef SKN_DF_H
#define SKN_DF_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/* Load a CSV file into a new heap-allocated DataFrame.
 * Returns NULL on failure (file not found, parse error). */
DataFrame *df_load(const char *filename);

/* Free a DataFrame previously returned by df_load or df_sum. Safe with NULL. */
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
 * String columns not in the group-by set are dropped from the output. */
DataFrame *df_sum(const DfGrouped *grp);

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

    /* ---- first pass: assign group IDs ---- */
    int *group_id = (int *)malloc((size_t)(nr > 0 ? nr : 1) * sizeof(int));
    int *group_first = (int *)malloc((size_t)(nr > 0 ? nr : 1) * sizeof(int));
    int n_groups = 0;

    for (int r = 0; r < nr; r++) {
        int found = -1;
        for (int g = 0; g < n_groups; g++) {
            if (df__rows_match(src, r, group_first[g], by_idx, grp->n_by)) {
                found = g;
                break;
            }
        }
        if (found >= 0) {
            group_id[r] = found;
        } else {
            group_first[n_groups] = r;
            group_id[r] = n_groups;
            n_groups++;
        }
    }

    /* ---- build result ---- */
    int out_cols = grp->n_by + n_sum;
    if (out_cols > DF_MAX_COLS) {
        free(group_id);
        free(group_first);
        df__error("df_sum: result exceeds DF_MAX_COLS");
    }

    DataFrame *res = (DataFrame *)calloc(1, sizeof(DataFrame));
    if (!res) { free(group_id); free(group_first); return NULL; }
    res->col_count = out_cols;
    res->row_count = n_groups;

    int out = 0;

    /* by-columns: copy names, types, allocate arrays, fill values */
    for (int j = 0; j < grp->n_by; j++, out++) {
        int src_col = by_idx[j];
        strncpy(res->names[out], src->names[src_col], DF_MAX_NAME - 1);
        res->names[out][DF_MAX_NAME - 1] = '\0';
        res->types[out] = src->types[src_col];

        switch (res->types[out]) {
            case DF_INT:
                res->data[out].i = (int *)calloc((size_t)n_groups, sizeof(int));
                if (!res->data[out].i) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    res->data[out].i[g] = src->data[src_col].i[group_first[g]];
                break;
            case DF_FLOAT:
                res->data[out].f = (float *)calloc((size_t)n_groups, sizeof(float));
                if (!res->data[out].f) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    res->data[out].f[g] = src->data[src_col].f[group_first[g]];
                break;
            case DF_DOUBLE:
                res->data[out].d = (double *)calloc((size_t)n_groups, sizeof(double));
                if (!res->data[out].d) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    res->data[out].d[g] = src->data[src_col].d[group_first[g]];
                break;
            case DF_STRING:
                res->data[out].s = (char **)calloc((size_t)n_groups, sizeof(char *));
                if (!res->data[out].s) { df_free(res); free(group_id); free(group_first); return NULL; }
                for (int g = 0; g < n_groups; g++)
                    res->data[out].s[g] = strdup(src->data[src_col].s[group_first[g]]);
                break;
        }
    }

    /* summed columns: always DF_DOUBLE in the output */
    for (int j = 0; j < n_sum; j++, out++) {
        int src_col = num_idx[j];
        strncpy(res->names[out], src->names[src_col], DF_MAX_NAME - 1);
        res->names[out][DF_MAX_NAME - 1] = '\0';
        res->types[out] = DF_DOUBLE;

        res->data[out].d = (double *)calloc((size_t)n_groups, sizeof(double));
        if (!res->data[out].d) { df_free(res); free(group_id); free(group_first); return NULL; }

        for (int r = 0; r < nr; r++)
            res->data[out].d[group_id[r]] += df__as_double(src, src_col, r);
    }

    free(group_id);
    free(group_first);
    return res;
}

#endif /* SKN_DF_IMPLEMENTATION */
#endif /* SKN_DF_H */

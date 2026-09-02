/* test_group_scale.c — df_group_by/df_sum performance at the shape that
 * motivated PM-003's hash-based rewrite: 5M rows, ~700 distinct group keys
 * (citypop/briefs/PMD-005.md's verified production shape).
 *
 * Performance bar: 5,000,000 rows / 700 groups completes in under 5 seconds.
 * That's a concrete, recorded number (not "fast enough") comfortably above
 * hashing overhead on typical dev hardware, chosen so a CLI tool built on
 * this doesn't visibly stall — and a huge margin below the old O(rows *
 * groups) behavior, which at this shape would mean ~3.8 billion row
 * comparisons and not finish in any reasonable time at all.
 */
#define SKN_CSV_IMPLEMENTATION
#define SKN_LOG_IMPLEMENTATION
#define SKN_DF_IMPLEMENTATION
#include "../df/skn_df.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N_ROWS       5000000
#define N_GROUPS     700
#define BUDGET_SECS  5.0

int main(void)
{
    printf("=== Group-by scale test: %d rows, %d groups ===\n", N_ROWS, N_GROUPS);

    int    *key = (int *)malloc((size_t)N_ROWS * sizeof(int));
    double *val = (double *)malloc((size_t)N_ROWS * sizeof(double));
    if (!key || !val) {
        fprintf(stderr, "allocation failed building synthetic data\n");
        return 1;
    }
    for (int r = 0; r < N_ROWS; r++) {
        key[r] = r % N_GROUPS;
        val[r] = 1.0;
    }

    DataFrame *df = df_new(2);
    df_set_column_int(df, 0, "key", key, N_ROWS);
    df_set_column_double(df, 1, "val", val, N_ROWS);
    free(key);
    free(val);

    DfGrouped grp = df_group_by(df, (const char *[]){"key"}, 1);

    clock_t t0 = clock();
    DataFrame *res = df_sum(&grp);
    clock_t t1 = clock();
    double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;

    int ok = 1;

    if (!res) {
        printf("FAIL: df_sum returned NULL\n");
        df_free(df);
        return 1;
    }

    printf("df_sum: %d rows -> %d groups in %.3f s (budget %.1f s)\n",
           N_ROWS, res->row_count, secs, BUDGET_SECS);

    if (res->row_count != N_GROUPS) {
        printf("FAIL: expected %d distinct groups, got %d\n", N_GROUPS, res->row_count);
        ok = 0;
    }
    if (secs > BUDGET_SECS) {
        printf("FAIL: took %.3fs, over the %.1fs budget\n", secs, BUDGET_SECS);
        ok = 0;
    }

    /* correctness alongside speed: every input row contributed val=1.0,
     * so the summed groups must add back up to N_ROWS exactly. */
    DfDoubleCol sums = df_get_double(res, "val");
    double total = 0.0;
    for (int g = 0; g < sums.count; g++) total += sums.data[g];
    if (total != (double)N_ROWS) {
        printf("FAIL: summed groups total %g, expected %d\n", total, N_ROWS);
        ok = 0;
    }

    df_free(res);
    df_free(df);

    printf("\n%s\n", ok ? "Scale test PASS" : "Scale test FAILED");
    return ok ? 0 : 1;
}

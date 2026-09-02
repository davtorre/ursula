/* test_sort_determinism.c — df_sort stability + reproducibility regression.
 *
 * This targets the exact failure mode citypop/briefs/PMD-005.md hit: an
 * unstable sort in a grouped-aggregation pipeline caused two runs with the
 * *same seed* to diverge in ~12% of assigned rows, despite the RNG itself
 * being correctly seeded. The fix there was sorting before any
 * shuffle/sample step — this test proves df_sort is stable enough to make
 * that fix hold, both in isolation and in the actual sort-then-sample shape.
 */
#define SKN_CSV_IMPLEMENTATION
#define SKN_LOG_IMPLEMENTATION
#define SKN_DF_IMPLEMENTATION
#include "../df/skn_df.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int all_ok = 1;

static void check(int cond, const char *what)
{
    printf("  %-68s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) all_ok = 0;
}

/* group in [0, n_groups), value pseudo-random but seed-reproducible */
static DataFrame *build_synthetic(int n, int n_groups, unsigned seed)
{
    srand(seed);
    int *grp = (int *)malloc((size_t)n * sizeof(int));
    int *val = (int *)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) {
        grp[i] = i % n_groups;
        val[i] = rand() % 50; /* small range: guarantees plenty of ties to test stability on */
    }

    DataFrame *df = df_new(2);
    df_set_column_int(df, 0, "group", grp, n);
    df_set_column_int(df, 1, "value", val, n);
    free(grp);
    free(val);
    return df;
}

static void test_sort_repeatable_and_stable(void)
{
    printf("=== df_sort: repeat runs identical, ties keep input order ===\n");

    DataFrame *df = build_synthetic(2000, 5, 42);

    int *idx1 = df_sort(df, "value");
    int *idx2 = df_sort(df, "value");

    check(idx1 != NULL && idx2 != NULL, "df_sort returns non-NULL");
    check(memcmp(idx1, idx2, (size_t)df->row_count * sizeof(int)) == 0,
          "two df_sort() calls on the same frame return byte-identical permutations");

    DfIntCol val = df_get_int(df, "value");
    int ascending = 1, stable = 1;
    for (int k = 1; k < df->row_count; k++) {
        int prev = idx1[k - 1], cur = idx1[k];
        if (val.data[prev] > val.data[cur]) ascending = 0;
        if (val.data[prev] == val.data[cur] && prev > cur) stable = 0;
    }
    check(ascending, "df_sort output is in ascending order by the sort column");
    check(stable, "df_sort is stable: rows with equal keys keep their input order");

    free(idx1);
    free(idx2);
    df_free(df);
}

/* Sort-then-sample: take the first `take_per_group` rows of each group, in
 * sorted order. Returns the selected row indices (against a fresh frame
 * built from the given seed) and how many were selected. */
static void run_pipeline(int *out_selected, int *out_n, int n, int n_groups,
                          unsigned seed, int take_per_group)
{
    DataFrame *df = build_synthetic(n, n_groups, seed);
    int *order = df_sort(df, "value");
    DfIntCol grp = df_get_int(df, "group");

    int *taken = (int *)calloc((size_t)n_groups, sizeof(int));
    int sel_count = 0;
    for (int k = 0; k < df->row_count; k++) {
        int r = order[k];
        int g = grp.data[r];
        if (taken[g] < take_per_group) {
            out_selected[sel_count++] = r;
            taken[g]++;
        }
    }
    *out_n = sel_count;

    free(taken);
    free(order);
    df_free(df);
}

static void test_sort_then_sample_pipeline(void)
{
    printf("\n=== Sort-then-sample pipeline: same seed reproduces identical output (PMD-005) ===\n");

    const int n = 5000, n_groups = 5, take = 40;
    int *sel1 = (int *)malloc((size_t)n * sizeof(int));
    int *sel2 = (int *)malloc((size_t)n * sizeof(int));
    int n1, n2;

    run_pipeline(sel1, &n1, n, n_groups, 1234, take);
    run_pipeline(sel2, &n2, n, n_groups, 1234, take);

    check(n1 == n2, "pipeline selects the same number of rows on both runs");
    check(n1 == n2 && memcmp(sel1, sel2, (size_t)n1 * sizeof(int)) == 0,
          "pipeline selects byte-identical rows on both runs under the same seed");

    free(sel1);
    free(sel2);
}

int main(void)
{
    test_sort_repeatable_and_stable();
    test_sort_then_sample_pipeline();

    printf("\n%s\n", all_ok ? "All sort determinism tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

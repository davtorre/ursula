/* test_resample_string.c — unit tests for PM-005-A's df_resample_string:
 * the string-keyed sibling of df_resample (PM-004), added because the
 * downstream household_conf use case ("youth_F|youth_M", etc.) needs a
 * string group key, not an int one. Mirrors test_match_resample_stack.c's
 * df_resample test shape, string-keyed, plus the two cases that are
 * specific to strings (identity vs. equality, and the household_conf
 * end-to-end scenario that motivated this brief).
 */
#define SKN_CSV_IMPLEMENTATION
#define SKN_LOG_IMPLEMENTATION
#define SKN_DF_IMPLEMENTATION
#include "../df/skn_df.h"
#include <stdio.h>
#include <string.h>

static int all_ok = 1;

static void check(int cond, const char *what)
{
    printf("  %-68s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) all_ok = 0;
}

/* ---- correctness + determinism + zero-weight group ---- */

static void test_resample_string_repeatable_and_weighted(void)
{
    printf("=== df_resample_string: reproducible, weighted, respects zero-weight groups ===\n");

    /* pool: group "adult" has two options (weight 1 and weight 99 -- draws
     * should skew heavily toward the high-weight one); group "senior" has
     * zero weight. */
    char   *wk_data[3] = {"adult", "adult", "senior"};
    double   w_data[3] = {1.0, 99.0, 0.0};
    DfStrCol weight_key; weight_key.data = wk_data; weight_key.count = 3;
    DfDoubleCol weights; weights.data    = w_data;  weights.count   = 3;

    char *residual_data[2000];
    for (int i = 0; i < 1000; i++)    residual_data[i] = "adult";   /* has weight */
    for (int i = 1000; i < 2000; i++) residual_data[i] = "senior";  /* zero weight */
    DfStrCol residual; residual.data = residual_data; residual.count = 2000;

    DfIntCol r1 = df_resample_string(residual, weight_key, weights, 20260812u);
    DfIntCol r2 = df_resample_string(residual, weight_key, weights, 20260812u);

    check(r1.count == 1000, "df_resample_string: zero-weight group (1000 rows) produces no output");
    check(r1.count == r2.count &&
          memcmp(r1.data, r2.data, (size_t)r1.count * sizeof(int)) == 0,
          "df_resample_string: same seed and inputs -> byte-identical output across two calls");

    int all_from_adult = 1, high_weight_hits = 0;
    for (int i = 0; i < r1.count; i++) {
        if (strcmp(weight_key.data[r1.data[i]], "adult") != 0) all_from_adult = 0;
        if (r1.data[i] == 1) high_weight_hits++; /* pool row 1 has weight 99 */
    }
    check(all_from_adult, "df_resample_string: every draw for group \"adult\" picks an \"adult\" pool row");
    /* expect ~990/1000 with weight 99:1 -- generous bound to avoid flakiness */
    check(high_weight_hits > 900,
          "df_resample_string: draws are weighted (high-weight pool row picked far more often)");

    DfIntCol r3 = df_resample_string(residual, weight_key, weights, 1u); /* different seed */
    check(!(r1.count == r3.count && memcmp(r1.data, r3.data, (size_t)r1.count * sizeof(int)) == 0),
          "df_resample_string: a different seed produces a different draw sequence");

    free(r1.data);
    free(r2.data);
    free(r3.data);
}

/* ---- string equality, not pointer identity ---- */

static void test_resample_string_equality_not_identity(void)
{
    printf("\n=== df_resample_string: matches by content, not by pointer ===\n");

    /* Every string below is its own separate allocation (strdup), never
     * shared with the array it's compared against -- a pointer-equality
     * bug here would under-match without ever crashing, which is exactly
     * why this is its own explicit test rather than folded into the case
     * above. */
    char *wk_data[2] = { strdup("group_x"), strdup("group_y") };
    double w_data[2] = { 1.0, 1.0 };
    DfStrCol weight_key; weight_key.data = wk_data; weight_key.count = 2;
    DfDoubleCol weights; weights.data    = w_data;  weights.count   = 2;

    char *residual_data[1] = { strdup("group_x") }; /* content-equal, pointer-distinct from wk_data[0] */
    DfStrCol residual; residual.data = residual_data; residual.count = 1;

    check(residual_data[0] != wk_data[0], "test setup: residual and weight_key strings are distinct allocations");

    DfIntCol r = df_resample_string(residual, weight_key, weights, 42u);
    check(r.count == 1, "df_resample_string: content-equal (pointer-distinct) key still produces a draw");
    check(r.count == 1 && strcmp(weight_key.data[r.data[0]], "group_x") == 0,
          "df_resample_string: the draw lands in the content-matching group, not by pointer");

    free(r.data);
    free(wk_data[0]); free(wk_data[1]);
    free(residual_data[0]);
}

/* ---- end-to-end: household_conf-shaped scenario ---- */

static void test_resample_string_household_conf(void)
{
    printf("\n=== df_resample_string: household_conf-shaped end-to-end ===\n");

    /* Supply pool: household_conf key/weight columns, as df_resample's
     * weight_key/weights are documented to expect -- a pool row belongs to
     * the group named by its household_conf value. */
    char   *supply_conf[6]  = {"youth_F", "youth_F", "youth_M", "adult_F", "adult_F", "adult_M"};
    double  supply_wt[6]    = {2.0, 8.0, 5.0, 1.0, 1.0, 3.0};
    DfStrCol weight_key; weight_key.data = supply_conf; weight_key.count = 6;
    DfDoubleCol weights;  weights.data   = supply_wt;   weights.count   = 6;

    /* Demand side: a residual set's *key column* (see the brief's warning
     * -- residual_idx.data[k] is a group key value, not a row-position),
     * gathered as it would be from a demand frame's household_conf column.
     * "senior_F" has no matching supply group at all -- must produce no
     * output for those rows, same as the zero-weight case, since "absent"
     * and "zero total weight" are the same thing from df_resample's point
     * of view. */
    char *demand_conf[5] = {"youth_F", "adult_M", "adult_F", "youth_M", "senior_F"};
    DfStrCol residual_idx; residual_idx.data = demand_conf; residual_idx.count = 5;

    DfIntCol drawn = df_resample_string(residual_idx, weight_key, weights, 7u);

    check(drawn.count == 4, "df_resample_string: 4/5 demand rows have a matching supply group (\"senior_F\" doesn't)");

    /* Every drawn position must index back into weight_key at a row whose
     * household_conf value equals the corresponding demand row's group --
     * check against the demand rows that actually produced output (every
     * row except the unmatched "senior_F" one, in order). */
    const char *expected_group[4] = {"youth_F", "adult_M", "adult_F", "youth_M"};
    int ok = 1;
    for (int k = 0; k < drawn.count; k++) {
        int pos = drawn.data[k];
        if (pos < 0 || pos >= weight_key.count) { ok = 0; break; }
        if (strcmp(weight_key.data[pos], expected_group[k]) != 0) ok = 0;
    }
    check(ok, "df_resample_string: every drawn position shares its demand row's group value");

    free(drawn.data);
}

int main(void)
{
    test_resample_string_repeatable_and_weighted();
    test_resample_string_equality_not_identity();
    test_resample_string_household_conf();

    printf("\n%s\n", all_ok ? "All df_resample_string tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

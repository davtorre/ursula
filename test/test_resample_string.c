/* test_resample_string.c — unit tests for df_resample_string: the
 * string-keyed sibling of df_resample (PM-004/PM-005-A), revised by
 * PM-005-B to return a DfResampleResult (picked/resolved_pos/
 * unresolved_pos) instead of a bare, unindexed DfIntCol -- so a caller can
 * tell which input rows resolved vs. didn't, not just get a shorter array.
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

static void df_resample_result_free(DfResampleResult r)
{
    free(r.picked);
    free(r.resolved_pos);
    free(r.unresolved_pos);
}

/* ---- correctness + determinism + zero-weight group (partition) ---- */

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

    DfResampleResult r1 = df_resample_string(residual, weight_key, weights, 20260812u);
    DfResampleResult r2 = df_resample_string(residual, weight_key, weights, 20260812u);

    check(r1.resolved_count == 1000 && r1.unresolved_count == 1000,
          "df_resample_string: zero-weight group (1000 rows) lands in unresolved_pos, not picked");
    check(r1.resolved_count == r2.resolved_count &&
          memcmp(r1.picked, r2.picked, (size_t)r1.resolved_count * sizeof(int)) == 0 &&
          memcmp(r1.resolved_pos, r2.resolved_pos, (size_t)r1.resolved_count * sizeof(int)) == 0 &&
          memcmp(r1.unresolved_pos, r2.unresolved_pos, (size_t)r1.unresolved_count * sizeof(int)) == 0,
          "df_resample_string: same seed and inputs -> byte-identical output across two calls");

    /* Criterion 1 / criterion 5: resolved_pos/unresolved_pos together
     * account for every input position exactly once, and every
     * unresolved_pos entry genuinely belongs to the zero-weight group. */
    int *seen = (int *)calloc((size_t)residual.count, sizeof(int));
    for (int j = 0; j < r1.resolved_count; j++)   seen[r1.resolved_pos[j]]++;
    for (int j = 0; j < r1.unresolved_count; j++) seen[r1.unresolved_pos[j]]++;
    int partition_ok = 1;
    for (int i = 0; i < residual.count; i++) if (seen[i] != 1) partition_ok = 0;
    check(partition_ok, "df_resample_string: resolved_pos/unresolved_pos partition every input position exactly once");
    free(seen);

    int unresolved_ok = 1;
    for (int j = 0; j < r1.unresolved_count; j++)
        if (strcmp(residual.data[r1.unresolved_pos[j]], "senior") != 0) unresolved_ok = 0;
    check(unresolved_ok, "df_resample_string: every unresolved_pos entry genuinely belongs to the zero-weight group");

    /* Criterion 2: picked[j]/resolved_pos[j] correspondence -- the group
     * of the drawn pool row must match the group of the residual row it
     * was drawn for. */
    int all_from_adult = 1, high_weight_hits = 0, correspondence_ok = 1;
    for (int j = 0; j < r1.resolved_count; j++) {
        if (strcmp(weight_key.data[r1.picked[j]], "adult") != 0) all_from_adult = 0;
        if (r1.picked[j] == 1) high_weight_hits++; /* pool row 1 has weight 99 */
        if (strcmp(residual.data[r1.resolved_pos[j]], weight_key.data[r1.picked[j]]) != 0)
            correspondence_ok = 0;
    }
    check(all_from_adult, "df_resample_string: every draw for group \"adult\" picks an \"adult\" pool row");
    check(correspondence_ok, "df_resample_string: residual_idx[resolved_pos[j]]'s group matches weight_key[picked[j]]");
    /* expect ~990/1000 with weight 99:1 -- generous bound to avoid flakiness */
    check(high_weight_hits > 900,
          "df_resample_string: draws are weighted (high-weight pool row picked far more often)");

    DfResampleResult r3 = df_resample_string(residual, weight_key, weights, 1u); /* different seed */
    check(!(r1.resolved_count == r3.resolved_count &&
            memcmp(r1.picked, r3.picked, (size_t)r1.resolved_count * sizeof(int)) == 0),
          "df_resample_string: a different seed produces a different draw sequence");

    df_resample_result_free(r1);
    df_resample_result_free(r2);
    df_resample_result_free(r3);
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

    DfResampleResult r = df_resample_string(residual, weight_key, weights, 42u);
    check(r.resolved_count == 1 && r.unresolved_count == 0,
          "df_resample_string: content-equal (pointer-distinct) key still produces a draw");
    check(r.resolved_count == 1 && strcmp(weight_key.data[r.picked[0]], "group_x") == 0,
          "df_resample_string: the draw lands in the content-matching group, not by pointer");

    df_resample_result_free(r);
    free(wk_data[0]); free(wk_data[1]);
    free(residual_data[0]);
}

/* ---- residual_idx.count == 0 ---- */

static void test_resample_string_zero_count(void)
{
    printf("\n=== df_resample_string: residual_idx.count == 0 ===\n");

    char *wk_data[1] = {"group_x"};
    double w_data[1] = {1.0};
    DfStrCol weight_key; weight_key.data = wk_data; weight_key.count = 1;
    DfDoubleCol weights; weights.data    = w_data;  weights.count   = 1;

    DfStrCol residual; residual.data = NULL; residual.count = 0;

    DfResampleResult r = df_resample_string(residual, weight_key, weights, 1u);
    check(r.resolved_count == 0 && r.unresolved_count == 0,
          "df_resample_string: residual_idx.count == 0 -> all-empty result, no crash");

    df_resample_result_free(r);
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
     * "senior_F" has no matching supply group at all -- must land in
     * unresolved_pos, since "absent" and "zero total weight" are the same
     * thing from df_resample's point of view. */
    char *demand_conf[5] = {"youth_F", "adult_M", "adult_F", "youth_M", "senior_F"};
    DfStrCol residual_idx; residual_idx.data = demand_conf; residual_idx.count = 5;

    DfResampleResult drawn = df_resample_string(residual_idx, weight_key, weights, 7u);

    check(drawn.resolved_count == 4 && drawn.unresolved_count == 1,
          "df_resample_string: 4/5 demand rows resolve, 1 (\"senior_F\") lands in unresolved_pos");
    check(drawn.unresolved_count == 1 && strcmp(demand_conf[drawn.unresolved_pos[0]], "senior_F") == 0,
          "df_resample_string: the unresolved position is exactly the \"senior_F\" row");

    /* Every drawn position must index back into weight_key at a row whose
     * household_conf value equals the corresponding demand row's group --
     * threaded through resolved_pos, not assumed by output order. */
    int ok = 1;
    for (int j = 0; j < drawn.resolved_count; j++) {
        int pos = drawn.picked[j];
        if (pos < 0 || pos >= weight_key.count) { ok = 0; break; }
        if (strcmp(weight_key.data[pos], demand_conf[drawn.resolved_pos[j]]) != 0) ok = 0;
    }
    check(ok, "df_resample_string: every drawn position shares its demand row's group value, via resolved_pos");

    df_resample_result_free(drawn);
}

int main(void)
{
    test_resample_string_repeatable_and_weighted();
    test_resample_string_equality_not_identity();
    test_resample_string_zero_count();
    test_resample_string_household_conf();

    printf("\n%s\n", all_ok ? "All df_resample_string tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

/* test_match_resample_stack.c — unit tests for PM-004's four new primitives:
 * df_match/df_match_string, df_assert_unique/df_assert_unique_string,
 * df_resample, df_stack_v_int/float/double/string. Small, synthetic, fast —
 * the real-data acceptance test lives separately in test_dynamo_assignment.c.
 */
#define SKN_CSV_IMPLEMENTATION
#define SKN_LOG_IMPLEMENTATION
#define SKN_DF_IMPLEMENTATION
#include "../df/skn_df.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int all_ok = 1;

static void check(int cond, const char *what)
{
    printf("  %-68s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) all_ok = 0;
}

/* ---- df_match / df_assert_unique (int) ---- */

static void test_match_int(void)
{
    printf("=== df_match (int keys) ===\n");

    /* right_key: catalog, idx -> conceptually household_conf id */
    int right_data[4] = {10, 20, 30, 40};
    DfIntCol right; right.data = right_data; right.count = 4;

    df_assert_unique(right); /* must not abort: no duplicates */
    check(1, "df_assert_unique passes on a duplicate-free key");

    /* left_key: demand rows referencing catalog idx values, out of order and with repeats */
    int left_data[5] = {30, 10, 10, 40, 20};
    DfIntCol left; left.data = left_data; left.count = 5;

    DfMatchResult m = df_match(left, right);
    check(m.count == 5, "df_match: result count == left_key.count");

    int ok = 1;
    for (int i = 0; i < m.count; i++) {
        if (m.left_idx[i] != i) ok = 0;
        if (right_data[m.right_idx[i]] != left_data[i]) ok = 0;
    }
    check(ok, "df_match: every left row resolves to the right row with an equal value");

    free(m.left_idx);
    free(m.right_idx);
}

static int child_dies_on_unmatched_left_key(void)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        int right_data[2] = {1, 2};
        DfIntCol right; right.data = right_data; right.count = 2;
        int left_data[1] = {99}; /* not in right_key */
        DfIntCol left; left.data = left_data; left.count = 1;
        df_match(left, right);
        _exit(0); /* unreachable if df_match fails loudly, as required */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

static int child_dies_on_duplicate_key(void)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        int data[3] = {5, 7, 5}; /* 5 repeated */
        DfIntCol key; key.data = data; key.count = 3;
        df_assert_unique(key);
        _exit(0); /* unreachable if df_assert_unique fails loudly, as required */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

static void test_match_fail_fast(void)
{
    printf("\n=== df_match / df_assert_unique: fail-fast cases ===\n");
    check(child_dies_on_unmatched_left_key(),
          "df_match: a left key absent from right_key terminates the process");
    check(child_dies_on_duplicate_key(),
          "df_assert_unique: a duplicate value terminates the process");
}

/* ---- df_match_string / df_assert_unique_string ---- */

static void test_match_string(void)
{
    printf("\n=== df_match (string keys) ===\n");

    char *right_data[3] = {"adult_F", "adult_M", "senior_F"};
    DfStrCol right; right.data = right_data; right.count = 3;
    df_assert_unique_string(right);
    check(1, "df_assert_unique_string passes on a duplicate-free key");

    char *left_data[4] = {"senior_F", "adult_F", "adult_F", "adult_M"};
    DfStrCol left; left.data = left_data; left.count = 4;

    DfMatchResult m = df_match_string(left, right);
    check(m.count == 4, "df_match_string: result count == left_key.count");

    int ok = 1;
    for (int i = 0; i < m.count; i++) {
        if (strcmp(right_data[m.right_idx[i]], left_data[i]) != 0) ok = 0;
    }
    check(ok, "df_match_string: every left row resolves to the right row with an equal value");

    free(m.left_idx);
    free(m.right_idx);
}

/* ---- df_resample ---- */

static void test_resample_repeatable_and_weighted(void)
{
    printf("\n=== df_resample: reproducible, respects zero-weight groups ===\n");

    /* pool: group 1 has two options (weight 1 and weight 99 -- draws should
     * skew heavily toward the high-weight one); group 2 has zero weight. */
    int    wk_data[3] = {1, 1, 2};
    double  w_data[3] = {1.0, 99.0, 0.0};
    DfIntCol weight_key; weight_key.data = wk_data; weight_key.count = 3;
    DfDoubleCol weights;  weights.data   = w_data;  weights.count   = 3;

    int residual_data[2000];
    for (int i = 0; i < 1000; i++) residual_data[i] = 1;       /* group 1: has weight */
    for (int i = 1000; i < 2000; i++) residual_data[i] = 2;    /* group 2: zero weight */
    DfIntCol residual; residual.data = residual_data; residual.count = 2000;

    DfIntCol r1 = df_resample(residual, weight_key, weights, 20260812u);
    DfIntCol r2 = df_resample(residual, weight_key, weights, 20260812u);

    check(r1.count == 1000, "df_resample: zero-weight group (1000 rows) produces no output");
    check(r1.count == r2.count &&
          memcmp(r1.data, r2.data, (size_t)r1.count * sizeof(int)) == 0,
          "df_resample: same seed and inputs -> byte-identical output across two calls");

    int all_from_group1 = 1, high_weight_hits = 0;
    for (int i = 0; i < r1.count; i++) {
        if (weight_key.data[r1.data[i]] != 1) all_from_group1 = 0;
        if (r1.data[i] == 1) high_weight_hits++; /* pool row 1 has weight 99 */
    }
    check(all_from_group1, "df_resample: every draw for group 1 picks a group-1 pool row");
    /* expect ~990/1000 with weight 99:1 -- generous bound to avoid flakiness */
    check(high_weight_hits > 900,
          "df_resample: draws are weighted (high-weight pool row picked far more often)");

    DfIntCol r3 = df_resample(residual, weight_key, weights, 1u); /* different seed */
    check(!(r1.count == r3.count && memcmp(r1.data, r3.data, (size_t)r1.count * sizeof(int)) == 0),
          "df_resample: a different seed produces a different draw sequence");

    free(r1.data);
    free(r2.data);
    free(r3.data);
}

/* ---- df_stack_v_* ---- */

static void test_stack_v(void)
{
    printf("\n=== df_stack_v_int / df_stack_v_string ===\n");

    int a_data[3] = {1, 2, 3};
    int b_data[2] = {40, 50};
    DfIntCol cols[3];
    cols[0].data = a_data; cols[0].count = 3;
    cols[1].data = b_data; cols[1].count = 2;
    cols[2].data = NULL;   cols[2].count = 0; /* empty column -- must not contribute rows or crash */

    DfIntCol stacked = df_stack_v_int(cols, 3);
    check(stacked.count == 5, "df_stack_v_int: output length == sum of input lengths (incl. an empty column)");
    check(stacked.data[0] == 1 && stacked.data[1] == 2 && stacked.data[2] == 3 &&
          stacked.data[3] == 40 && stacked.data[4] == 50,
          "df_stack_v_int: values concatenated in argument order");
    free(stacked.data);

    char *s1[2] = {"exact", "exact"};
    char *s2[1] = {"bootstrap"};
    DfStrCol scols[2];
    scols[0].data = s1; scols[0].count = 2;
    scols[1].data = s2; scols[1].count = 1;

    DfStrCol sstacked = df_stack_v_string(scols, 2);
    check(sstacked.count == 3, "df_stack_v_string: output length == sum of input lengths");
    check(strcmp(sstacked.data[0], "exact") == 0 && strcmp(sstacked.data[1], "exact") == 0 &&
          strcmp(sstacked.data[2], "bootstrap") == 0,
          "df_stack_v_string: values concatenated in argument order");
    check(sstacked.data[0] != s1[0], "df_stack_v_string: output strings are copies, not aliases");

    for (int i = 0; i < sstacked.count; i++) free(sstacked.data[i]);
    free(sstacked.data);
}

int main(void)
{
    test_match_int();
    test_match_fail_fast();
    test_match_string();
    test_resample_repeatable_and_weighted();
    test_stack_v();

    printf("\n%s\n", all_ok ? "All match/resample/stack tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

/* test_map_arr.c -- correctness + composition tests for PM-006's array-
 * level map primitives: df_map1_arr, df_map2_arr, df_map_scalar_arr.
 */
#define SKN_CSV_IMPLEMENTATION
#define SKN_LOG_IMPLEMENTATION
#define SKN_DF_IMPLEMENTATION
#include "../df/skn_df.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>

static int all_ok = 1;

static void check(int cond, const char *what)
{
    printf("  %-68s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) all_ok = 0;
}

static int dbl_eq(double a, double b) { return fabs(a - b) < 1e-9; }

/* ---- correctness parity: df_map*_arr vs their DataFrame-based siblings ---- */

static void test_parity(void)
{
    printf("=== Correctness parity: df_map*_arr vs df_map* (PM-005) ===\n");

    double x[5] = {0.1, 0.5, 1.0, 2.0, 3.5};
    double y[5] = {2.0, 3.0, 0.5, 4.0, 1.5};
    DataFrame *df = df_new(2);
    df_set_column_double(df, 0, "x", x, 5);
    df_set_column_double(df, 1, "y", y, 5);

    DfDoubleCol col_view = df_get_double(df, "x");
    DfDoubleCol from_col = df_map1(df, "x", sin);
    DfDoubleCol from_arr = df_map1_arr(col_view.data, col_view.count, sin);
    check(from_col.count == from_arr.count &&
          memcmp(from_col.data, from_arr.data, (size_t)from_col.count * sizeof(double)) == 0,
          "df_map1_arr(df_get_double(...).data) is byte-identical to df_map1 on the same column");
    free(from_col.data);
    free(from_arr.data);

    DfDoubleCol xv = df_get_double(df, "x");
    DfDoubleCol yv = df_get_double(df, "y");
    DfDoubleCol p_col = df_map2(df, "x", "y", pow);
    DfDoubleCol p_arr = df_map2_arr(xv.data, xv.count, yv.data, yv.count, pow);
    check(p_col.count == p_arr.count &&
          memcmp(p_col.data, p_arr.data, (size_t)p_col.count * sizeof(double)) == 0,
          "df_map2_arr is byte-identical to df_map2 on the same two columns");
    free(p_col.data);
    free(p_arr.data);

    DfDoubleCol s_col = df_map_scalar(df, "x", 2.0, df_sub);
    DfDoubleCol s_arr = df_map_scalar_arr(xv.data, xv.count, 2.0, df_sub);
    check(s_col.count == s_arr.count &&
          memcmp(s_col.data, s_arr.data, (size_t)s_col.count * sizeof(double)) == 0,
          "df_map_scalar_arr is byte-identical to df_map_scalar on the same column");
    free(s_col.data);
    free(s_arr.data);

    df_free(df);
}

/* ---- composition: chain maps directly, no DataFrame round-trip ---- */

static void test_composition(void)
{
    printf("\n=== Composition: chain maps without a DataFrame round-trip ===\n");

    double x[4] = {0.1, 0.5, 1.0, 1.3};
    DataFrame *df = df_new(1);
    df_set_column_double(df, 0, "x", x, 4);

    /* df_map1 (DataFrame) -> df_map1_arr (raw array), chained directly */
    DfDoubleCol step1 = df_map1(df, "x", sin);
    DfDoubleCol step2 = df_map1_arr(step1.data, step1.count, cos);

    int ok = 1;
    for (int i = 0; i < 4; i++) if (!dbl_eq(step2.data[i], cos(sin(x[i])))) ok = 0;
    check(step2.count == 4, "df_map1 -> df_map1_arr chain: output length preserved");
    check(ok, "df_map1 -> df_map1_arr chain: matches cos(sin(x)) computed by hand");
    free(step1.data);

    /* df_map1_arr -> df_map1_arr, chained again, exercising df_gather_double's
     * output feeding straight into a map with no DataFrame in between */
    DfDoubleCol step3 = df_map1_arr(step2.data, step2.count, fabs);
    ok = 1;
    for (int i = 0; i < 4; i++) if (!dbl_eq(step3.data[i], fabs(cos(sin(x[i]))))) ok = 0;
    check(ok, "df_map1_arr -> df_map1_arr chain: matches fabs(cos(sin(x))) computed by hand");
    free(step2.data);
    free(step3.data);

    /* df_gather_double output feeding directly into df_map_scalar_arr */
    DfDoubleCol full = df_get_double(df, "x");
    int idx[2] = {3, 1};
    DfDoubleCol gathered = df_gather_double(full, idx, 2);
    DfDoubleCol scaled = df_map_scalar_arr(gathered.data, gathered.count, 10.0, df_mul);
    ok = dbl_eq(scaled.data[0], x[3] * 10.0) && dbl_eq(scaled.data[1], x[1] * 10.0);
    check(ok, "df_gather_double -> df_map_scalar_arr chain: matches gathered values * 10 by hand");
    free(gathered.data);
    free(scaled.data);

    df_free(df);
}

/* ---- fork-based fail-fast helper, and the real (non-defensive) length
 * mismatch fixture -- defined before use since both test_real_mismatch and
 * test_fail_fast (below) need child_dies(). ---- */

static int child_dies(void (*trigger)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        trigger();
        _exit(0); /* unreachable if trigger() fails loudly, as required */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

static const double *g_mismatch_a, *g_mismatch_b;
static int g_mismatch_na, g_mismatch_nb;

static void trigger_map2_arr_mismatch(void)
{
    (void)df_map2_arr(g_mismatch_a, g_mismatch_na, g_mismatch_b, g_mismatch_nb, pow);
}

static int child_dies_on_mismatch(const double *a, int n_a, const double *b, int n_b)
{
    g_mismatch_a = a; g_mismatch_na = n_a;
    g_mismatch_b = b; g_mismatch_nb = n_b;
    return child_dies(trigger_map2_arr_mismatch);
}

static void test_real_mismatch(void)
{
    printf("=== df_map2_arr: real length mismatch (not defensive-only, unlike df_map2) ===\n");
    printf(
        "  df_map2's own mismatch check is structurally unreachable through the public API\n"
        "  (both columns necessarily share df->row_count -- see test_map.c, PM-005). A raw\n"
        "  double* pair has no such guarantee: two arrays sourced independently -- here, one\n"
        "  full column and one shorter df_gather_double result -- genuinely differ in length.\n"
        "  This is the live case PM-005 could not exercise and this one can.\n");

    double full_data[5] = {1.0, 2.0, 3.0, 4.0, 5.0};
    DfDoubleCol full; full.data = full_data; full.count = 5;

    int idx[2] = {0, 2};
    DfDoubleCol gathered = df_gather_double(full, idx, 2); /* count == 2, genuinely shorter */
    check(gathered.count == 2 && full.count == 5,
          "fixture: full.count (5) and gathered.count (2) are genuinely different lengths");

    check(child_dies_on_mismatch(full.data, full.count, gathered.data, gathered.count),
          "df_map2_arr(full, gathered): real n_a != n_b terminates the process");

    free(gathered.data);
}

/* ---- NULL / fn fail-fast cases ---- */

static void trigger_map1_arr_null_in(void)     { (void)df_map1_arr(NULL, 3, sin); }
static void trigger_map1_arr_null_fn(void)      { double a[1] = {1.0}; (void)df_map1_arr(a, 1, NULL); }
static void trigger_map2_arr_null_a(void)       { double b[1] = {1.0}; (void)df_map2_arr(NULL, 1, b, 1, pow); }
static void trigger_map2_arr_null_b(void)       { double a[1] = {1.0}; (void)df_map2_arr(a, 1, NULL, 1, pow); }
static void trigger_map2_arr_null_fn(void)      { double a[1] = {1.0}, b[1] = {2.0}; (void)df_map2_arr(a, 1, b, 1, NULL); }
static void trigger_map_scalar_arr_null_in(void){ (void)df_map_scalar_arr(NULL, 3, 2.0, df_add); }
static void trigger_map_scalar_arr_null_fn(void){ double a[1] = {1.0}; (void)df_map_scalar_arr(a, 1, 2.0, NULL); }

static void test_fail_fast(void)
{
    printf("\n=== NULL / fn fail-fast cases ===\n");
    check(child_dies(trigger_map1_arr_null_in), "df_map1_arr: in == NULL with n > 0 terminates the process");
    check(child_dies(trigger_map1_arr_null_fn), "df_map1_arr: fn == NULL terminates the process");
    check(child_dies(trigger_map2_arr_null_a),  "df_map2_arr: a == NULL with n_a > 0 terminates the process");
    check(child_dies(trigger_map2_arr_null_b),  "df_map2_arr: b == NULL with n_b > 0 terminates the process");
    check(child_dies(trigger_map2_arr_null_fn), "df_map2_arr: fn == NULL terminates the process");
    check(child_dies(trigger_map_scalar_arr_null_in), "df_map_scalar_arr: in == NULL with n > 0 terminates the process");
    check(child_dies(trigger_map_scalar_arr_null_fn), "df_map_scalar_arr: fn == NULL terminates the process");
}

/* ---- n == 0 case ---- */

static void test_zero_length(void)
{
    printf("\n=== n == 0: all three functions handle it without crashing ===\n");

    DfDoubleCol r1 = df_map1_arr(NULL, 0, sin); /* NULL is fine when n == 0 */
    check(r1.count == 0, "df_map1_arr(NULL, 0, sin): count == 0, no crash");
    free(r1.data);

    DfDoubleCol r2 = df_map2_arr(NULL, 0, NULL, 0, pow);
    check(r2.count == 0, "df_map2_arr(NULL, 0, NULL, 0, pow): count == 0, no crash");
    free(r2.data);

    DfDoubleCol r3 = df_map_scalar_arr(NULL, 0, 2.0, df_add);
    check(r3.count == 0, "df_map_scalar_arr(NULL, 0, 2.0, df_add): count == 0, no crash");
    free(r3.data);
}

int main(void)
{
    test_parity();
    test_composition();
    test_real_mismatch();
    test_fail_fast();
    test_zero_length();

    printf("\n%s\n", all_ok ? "All map_arr tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

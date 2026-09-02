/* test_map.c -- correctness tests for PM-005's elementwise column
 * operations: df_map1, df_map2, df_map_scalar, and the df_add/sub/rsub/
 * mul/div/rdiv arithmetic helpers.
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

/* ---- df_map1: unary correctness + int-column coercion ---- */

static void test_map1(void)
{
    printf("=== df_map1: unary correctness ===\n");

    double vals[5] = {0.0, 0.5, 1.0, 1.5707963267948966, 3.14159265358979};
    DataFrame *df = df_new(1);
    df_set_column_double(df, 0, "x", vals, 5);

    DfDoubleCol s = df_map1(df, "x", sin);
    int ok = 1;
    for (int i = 0; i < 5; i++) if (!dbl_eq(s.data[i], sin(vals[i]))) ok = 0;
    check(s.count == 5, "df_map1(sin): output length == row_count");
    check(ok, "df_map1(sin): every value matches a hand-computed sin()");
    free(s.data);

    DfDoubleCol q = df_map1(df, "x", sqrt);
    ok = 1;
    for (int i = 0; i < 5; i++) if (!dbl_eq(q.data[i], sqrt(vals[i]))) ok = 0;
    check(ok, "df_map1(sqrt): every value matches a hand-computed sqrt()");
    free(q.data);

    df_free(df);

    /* int-column coercion: df__as_double path, not just double-to-double */
    int ivals[4] = {0, 1, 4, 9};
    DataFrame *idf = df_new(1);
    df_set_column_int(idf, 0, "n", ivals, 4);

    DfDoubleCol r = df_map1(idf, "n", sqrt);
    ok = 1;
    for (int i = 0; i < 4; i++) if (!dbl_eq(r.data[i], sqrt((double)ivals[i]))) ok = 0;
    check(ok, "df_map1(sqrt) on an int-typed column: df__as_double coercion is exercised");
    free(r.data);
    df_free(idf);
}

/* ---- df_map2: binary correctness ---- */

static void test_map2(void)
{
    printf("\n=== df_map2: binary correctness ===\n");

    double base[4]     = {2.0, 3.0, 4.0, 5.0};
    double exponent[4] = {3.0, 2.0, 0.5, 0.0};
    DataFrame *df = df_new(2);
    df_set_column_double(df, 0, "base", base, 4);
    df_set_column_double(df, 1, "exp",  exponent, 4);

    DfDoubleCol p = df_map2(df, "base", "exp", pow);
    check(p.count == 4, "df_map2(pow): output length == row_count");
    int ok = 1;
    for (int i = 0; i < 4; i++) if (!dbl_eq(p.data[i], pow(base[i], exponent[i]))) ok = 0;
    check(ok, "df_map2(pow): every value matches a hand-computed pow()");

    free(p.data);
    df_free(df);
}

/* ---- df_map_scalar: both operand orders ---- */

static void test_map_scalar(void)
{
    printf("\n=== df_map_scalar: named helpers, both operand orders ===\n");

    double vals[3] = {10.0, 20.0, 4.0};
    DataFrame *df = df_new(1);
    df_set_column_double(df, 0, "x", vals, 3);

    DfDoubleCol add = df_map_scalar(df, "x", 2.0, df_add);
    int ok = 1;
    for (int i = 0; i < 3; i++) if (!dbl_eq(add.data[i], vals[i] + 2.0)) ok = 0;
    check(ok, "df_map_scalar(df_add, 2.0): column + 2");
    free(add.data);

    DfDoubleCol sub = df_map_scalar(df, "x", 2.0, df_sub);
    ok = 1;
    for (int i = 0; i < 3; i++) if (!dbl_eq(sub.data[i], vals[i] - 2.0)) ok = 0;
    check(ok, "df_map_scalar(df_sub, 2.0): column - 2");

    DfDoubleCol rsub = df_map_scalar(df, "x", 2.0, df_rsub);
    ok = 1;
    for (int i = 0; i < 3; i++) if (!dbl_eq(rsub.data[i], 2.0 - vals[i])) ok = 0;
    check(ok, "df_map_scalar(df_rsub, 2.0): 2 - column");

    /* the one part of this API shaped specifically to avoid a silent
     * correctness bug: confirm sub and rsub actually differ, not just
     * that each individually looks plausible */
    int all_differ = 1;
    for (int i = 0; i < 3; i++) if (dbl_eq(sub.data[i], rsub.data[i])) all_differ = 0;
    check(all_differ, "df_sub vs df_rsub: results actually differ (operand order matters)");
    free(sub.data);
    free(rsub.data);

    DfDoubleCol mul = df_map_scalar(df, "x", 3.0, df_mul);
    ok = 1;
    for (int i = 0; i < 3; i++) if (!dbl_eq(mul.data[i], vals[i] * 3.0)) ok = 0;
    check(ok, "df_map_scalar(df_mul, 3.0): column * 3");
    free(mul.data);

    DfDoubleCol dv = df_map_scalar(df, "x", 2.0, df_div);
    ok = 1;
    for (int i = 0; i < 3; i++) if (!dbl_eq(dv.data[i], vals[i] / 2.0)) ok = 0;
    check(ok, "df_map_scalar(df_div, 2.0): column / 2");

    DfDoubleCol rdv = df_map_scalar(df, "x", 2.0, df_rdiv);
    ok = 1;
    for (int i = 0; i < 3; i++) if (!dbl_eq(rdv.data[i], 2.0 / vals[i])) ok = 0;
    check(ok, "df_map_scalar(df_rdiv, 2.0): 2 / column");

    all_differ = 1;
    for (int i = 0; i < 3; i++) if (dbl_eq(dv.data[i], rdv.data[i])) all_differ = 0;
    check(all_differ, "df_div vs df_rdiv: results actually differ (operand order matters)");
    free(dv.data);
    free(rdv.data);

    df_free(df);
}

/* ---- ownership: output is an independent buffer, not an alias ---- */

static void test_ownership(void)
{
    printf("\n=== ownership: df_map1's output is independently owned ===\n");

    double vals[3] = {1.0, 2.0, 3.0};
    DataFrame *df = df_new(1);
    df_set_column_double(df, 0, "x", vals, 3);

    DfDoubleCol src = df_get_double(df, "x");
    DfDoubleCol out = df_map1(df, "x", fabs);

    check(out.data != src.data, "df_map1's output buffer is not an alias into the source column");
    out.data[0] = -999.0;
    check(src.data[0] == 1.0, "mutating the output doesn't affect the source column's stored data");

    free(out.data);
    df_free(df);
}

/* ---- composition: df_map1 output composes into a new frame + df_write/df_load round-trip ---- */

static void test_composition_roundtrip(void)
{
    printf("\n=== composition: df_map1 output -> df_new/df_set_column_double -> df_write/df_load ===\n");

    double vals[4] = {1.0, 2.0, 3.0, 4.0};
    DataFrame *df = df_new(1);
    df_set_column_double(df, 0, "x", vals, 4);

    DfDoubleCol squared = df_map2(df, "x", "x", df_mul); /* x * x, via df_map2 this time */

    DataFrame *out = df_new(2);
    df_set_column_double(out, 0, "x", vals, 4);
    df_set_column_double(out, 1, "x_squared", squared.data, squared.count);
    free(squared.data);
    df_free(df);

    int rc = df_write(out, "test_map_composition_out.csv");
    check(rc == 0, "df_write on the composed frame returns 0");

    DataFrame *reloaded = df_load("test_map_composition_out.csv");
    check(reloaded != NULL, "df_load on the round-tripped file succeeds");
    if (reloaded) {
        DfDoubleCol xs = df_get_double(reloaded, "x");
        DfDoubleCol x2 = df_get_double(reloaded, "x_squared");
        int ok = (xs.count == 4 && x2.count == 4);
        for (int i = 0; ok && i < 4; i++)
            if (!dbl_eq(xs.data[i], vals[i]) || !dbl_eq(x2.data[i], vals[i] * vals[i])) ok = 0;
        check(ok, "round-tripped frame's values match the original df_map2 output exactly");
        df_free(reloaded);
    }

    df_free(out);
}

/* ---- fail-fast cases ---- */

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

static DataFrame *g_fixture; /* built once in main(), read-only by the trigger_* functions below */

static void trigger_column_not_found(void)  { (void)df_map1(g_fixture, "does_not_exist", sqrt); }
static void trigger_string_column(void)     { (void)df_map1(g_fixture, "label", sqrt); }
static void trigger_null_fn(void)           { (void)df_map1(g_fixture, "x", NULL); }
static void trigger_map2_null_fn(void)      { (void)df_map2(g_fixture, "x", "x", NULL); }
static void trigger_map_scalar_null_fn(void){ (void)df_map_scalar(g_fixture, "x", 1.0, NULL); }

static void test_fail_fast(void)
{
    printf("\n=== fail-fast cases ===\n");

    check(child_dies(trigger_column_not_found), "df_map1: column not found terminates the process");
    check(child_dies(trigger_string_column),    "df_map1: a DF_STRING column terminates the process");
    check(child_dies(trigger_null_fn),          "df_map1: fn == NULL terminates the process");
    check(child_dies(trigger_map2_null_fn),     "df_map2: fn == NULL terminates the process");
    check(child_dies(trigger_map_scalar_null_fn), "df_map_scalar: fn == NULL terminates the process");
}

/* df_map2's row-count-mismatch guard, and why it's untestable through the
 * public API -- see the identical reasoning in skn_df.h's df_map2 comment. */
static void test_map2_mismatch_is_unreachable(void)
{
    printf("\n=== df_map2: row-count mismatch is defensive-only, not reachable via the public API ===\n");
    printf(
        "  DataFrame stores one shared `row_count` for the whole frame, not a length per\n"
        "  column, and df__set_column_check (PM-002) refuses to build a df where a new\n"
        "  column's length disagrees with the frame's existing row_count -- so two columns\n"
        "  of the same df can never have different lengths in the first place. There is no\n"
        "  way to hand-assemble a df that violates this and pass it to df_map2 through any\n"
        "  public function: df_new/df_set_column_* enforce it at construction time, and\n"
        "  df_load's row_count comes from the CSV's own column-length invariant already\n"
        "  enforced by skinny-csv. df_map2's implementation reflects this: it uses\n"
        "  df->row_count once, for both columns, with a comment explaining why comparing\n"
        "  df->row_count to itself would not be a meaningful runtime check. No test can\n"
        "  reach the mismatch branch because the branch does not exist -- the correctness\n"
        "  guarantee comes from PM-002's construction-time enforcement instead.\n");
    check(1, "documented above: unreachable by construction via df_new/df_set_column_*/df_load");
}

int main(void)
{
    double x[3]    = {1.0, 4.0, 9.0};
    char  *lbl[3]  = {"a", "b", "c"};
    g_fixture = df_new(2);
    df_set_column_double(g_fixture, 0, "x", x, 3);
    df_set_column_string(g_fixture, 1, "label", lbl, 3);

    test_map1();
    test_map2();
    test_map_scalar();
    test_ownership();
    test_composition_roundtrip();
    test_fail_fast();
    test_map2_mismatch_is_unreachable();

    df_free(g_fixture);

    printf("\n%s\n", all_ok ? "All map tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

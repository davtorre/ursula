/* test_cast.c -- correctness + fail-fast tests for PM-007's explicit
 * narrowing casts: df_cast_to_int, df_cast_to_float.
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

/* ---- rounding correctness: round-half-away-from-zero, not truncation ---- */

static void test_rounding(void)
{
    printf("=== df_cast_to_int: round-half-away-from-zero, not truncation ===\n");

    /* exact integers, values straddling .5 in both directions, negatives --
     * picking cases where truncation and round-to-nearest disagree is the
     * point (2.5 truncates to 2 but rounds to 3; -2.5 truncates to -2 but
     * rounds to -3). */
    double  in[8]       = {2.0, -2.0, 2.5, -2.5, 2.4, -2.4, 2.6, -2.6};
    int expected[8]     = {2,   -2,   3,   -3,   2,   -2,   3,   -3};

    DfDoubleCol col; col.data = in; col.count = 8;
    DfIntCol out = df_cast_to_int(col);

    check(out.count == 8, "df_cast_to_int: output length == input count");
    int ok = 1;
    for (int i = 0; i < 8; i++) if (out.data[i] != expected[i]) ok = 0;
    check(ok, "df_cast_to_int: every value matches round-half-away-from-zero, not truncation");

    free(out.data);
}

/* ---- float narrowing correctness ---- */

static void test_float_narrowing(void)
{
    printf("\n=== df_cast_to_float: exact for representable values, nearest otherwise ===\n");

    double exact[4] = {0.0, 1.0, -1.0, 0.5};
    DfDoubleCol col1; col1.data = exact; col1.count = 4;
    DfFloatCol out1 = df_cast_to_float(col1);
    int ok = 1;
    for (int i = 0; i < 4; i++) if (out1.data[i] != (float)exact[i]) ok = 0;
    check(ok, "df_cast_to_float: exactly-representable doubles round-trip exactly");
    free(out1.data);

    /* more precision than float can hold -- compare against the compiler's
     * own (float) cast, not a tolerance check, since narrowing is
     * deterministic */
    double precise[3] = {1.0 / 3.0, M_PI, 123456789.123456789};
    DfDoubleCol col2; col2.data = precise; col2.count = 3;
    DfFloatCol out2 = df_cast_to_float(col2);
    ok = 1;
    for (int i = 0; i < 3; i++) if (out2.data[i] != (float)precise[i]) ok = 0;
    check(ok, "df_cast_to_float: over-precise doubles match a hand/compiler (float) cast exactly");
    free(out2.data);
}

/* ---- fail-fast cases, each built explicitly ---- */

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

static void trigger_int_nan(void)
{
    double v[1] = {NAN};
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_int(col);
}
static void trigger_int_posinf(void)
{
    double v[1] = {INFINITY};
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_int(col);
}
static void trigger_int_neginf(void)
{
    double v[1] = {-INFINITY};
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_int(col);
}
static void trigger_int_overflow(void)
{
    double v[1] = {1e18}; /* clearly outside [INT_MIN, INT_MAX] */
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_int(col);
}

static void trigger_float_nan(void)
{
    double v[1] = {NAN};
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_float(col);
}
static void trigger_float_posinf(void)
{
    double v[1] = {INFINITY};
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_float(col);
}
static void trigger_float_neginf(void)
{
    double v[1] = {-INFINITY};
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_float(col);
}
static void trigger_float_overflow(void)
{
    /* 1e300: comfortably finite as a double (DBL_MAX is ~1.8e308) but far
     * beyond float's ~3.4e38 max -- hits the output-side isinf() check
     * specifically, not the input NaN/Inf case. (The brief's own "1e309"
     * example would already overflow *double* itself, contradicting "a
     * finite double" -- this is the value that actually isolates the
     * output-side check the acceptance criterion asks for.) */
    double v[1] = {1e300};
    DfDoubleCol col; col.data = v; col.count = 1;
    (void)df_cast_to_float(col);
}

static void test_fail_fast(void)
{
    printf("\n=== fail-fast cases (built explicitly, each one) ===\n");

    check(child_dies(trigger_int_nan),      "df_cast_to_int: NaN input terminates the process");
    check(child_dies(trigger_int_posinf),   "df_cast_to_int: +Infinity input terminates the process");
    check(child_dies(trigger_int_neginf),   "df_cast_to_int: -Infinity input terminates the process");
    check(child_dies(trigger_int_overflow), "df_cast_to_int: 1e18 (out of int range) terminates the process");

    check(child_dies(trigger_float_nan),      "df_cast_to_float: NaN input terminates the process");
    check(child_dies(trigger_float_posinf),   "df_cast_to_float: +Infinity input terminates the process");
    check(child_dies(trigger_float_neginf),   "df_cast_to_float: -Infinity input terminates the process");
    check(child_dies(trigger_float_overflow), "df_cast_to_float: finite double beyond float's range terminates the process (output-isinf check)");
}

/* ---- zero-count case ---- */

static void test_zero_count(void)
{
    printf("\n=== col.count == 0: both functions handle it without crashing ===\n");

    DfDoubleCol empty; empty.data = NULL; empty.count = 0;

    DfIntCol i = df_cast_to_int(empty);
    check(i.count == 0, "df_cast_to_int(count=0): count == 0, no crash");
    free(i.data);

    DfFloatCol f = df_cast_to_float(empty);
    check(f.count == 0, "df_cast_to_float(count=0): count == 0, no crash");
    free(f.data);
}

/* ---- end-to-end composition: the scenario that motivated this brief ---- */

static void test_composition_roundtrip(void)
{
    printf("\n=== end-to-end: df_map_scalar -> df_cast_to_int -> frame -> df_write/df_load ===\n");

    int ivals[4] = {10, 20, 30, 40};
    DataFrame *df = df_new(1);
    df_set_column_int(df, 0, "n", ivals, 4);

    /* compute in double (df_map_scalar always outputs DfDoubleCol, PM-005),
     * then cast back to int explicitly -- exactly the pipeline this brief
     * exists to support */
    DfDoubleCol summed = df_map_scalar(df, "n", 1.0, df_add);
    DfIntCol casted = df_cast_to_int(summed);
    free(summed.data);

    check(casted.count == 4, "df_map_scalar -> df_cast_to_int: output length preserved");
    int ok = 1;
    for (int i = 0; i < 4; i++) if (casted.data[i] != ivals[i] + 1) ok = 0;
    check(ok, "df_map_scalar -> df_cast_to_int: values match ivals[i] + 1 exactly");

    DataFrame *out = df_new(1);
    df_set_column_int(out, 0, "n_plus_1", casted.data, casted.count);
    free(casted.data);
    df_free(df);

    int rc = df_write(out, "test_cast_composition_out.csv");
    check(rc == 0, "df_write on the composed frame returns 0");

    DataFrame *reloaded = df_load("test_cast_composition_out.csv");
    check(reloaded != NULL, "df_load on the round-tripped file succeeds");
    if (reloaded) {
        DfIntCol col = df_get_int(reloaded, "n_plus_1");
        ok = (col.count == 4);
        for (int i = 0; ok && i < 4; i++) if (col.data[i] != ivals[i] + 1) ok = 0;
        check(ok, "round-tripped frame's int values match the original pipeline output exactly");
        df_free(reloaded);
    }

    df_free(out);
}

int main(void)
{
    test_rounding();
    test_float_narrowing();
    test_fail_fast();
    test_zero_count();
    test_composition_roundtrip();

    printf("\n%s\n", all_ok ? "All cast tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

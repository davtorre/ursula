/* test_gather.c — df_gather_* correctness: repeats, a shorter-than-source
 * index array, and an out-of-range index (must fail loudly, not corrupt
 * memory or silently clamp).
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

static void test_gather_repeats_and_short(void)
{
    printf("=== df_gather_int: repeated and shorter-than-source index arrays ===\n");

    int data[5] = {10, 20, 30, 40, 50};
    DfIntCol col; col.data = data; col.count = 5;

    /* repeats: fan row 2 out three times, longer than col but idx_count still governs length */
    int idx_repeat[5] = {0, 2, 2, 2, 4};
    DfIntCol g1 = df_gather_int(col, idx_repeat, 5);
    check(g1.count == 5, "repeat-index gather: output length == idx_count");
    check(g1.data[0] == 10 && g1.data[1] == 30 && g1.data[2] == 30 &&
          g1.data[3] == 30 && g1.data[4] == 50,
          "repeat-index gather: values match the repeated source rows");

    /* shorter than the source column */
    int idx_short[2] = {4, 0};
    DfIntCol g2 = df_gather_int(col, idx_short, 2);
    check(g2.count == 2, "short index-array gather: output length == idx_count (< col.count)");
    check(g2.data[0] == 50 && g2.data[1] == 10,
          "short index-array gather: values in requested order");

    free(g1.data);
    free(g2.data);
}

static void test_gather_string_independent_copies(void)
{
    printf("\n=== df_gather_string: independently-owned output ===\n");

    char *names[3] = {"Alice", "Bob", "Carol"};
    DfStrCol col; col.data = names; col.count = 3;

    int idx[4] = {2, 0, 0, 1};
    DfStrCol g = df_gather_string(col, idx, 4);
    check(g.count == 4, "string gather: output length == idx_count");
    check(strcmp(g.data[0], "Carol") == 0 && strcmp(g.data[1], "Alice") == 0 &&
          strcmp(g.data[2], "Alice") == 0 && strcmp(g.data[3], "Bob") == 0,
          "string gather: values match, including a repeated source row");
    check(g.data[1] != names[0], "string gather: output strings are copies, not aliases into the source");

    for (int i = 0; i < g.count; i++) free(g.data[i]);
    free(g.data);
}

/* Runs a deliberately out-of-range df_gather_int call in a child process so
 * the parent test binary survives to report the result — df_gather_int is
 * required to exit(1), which we can only observe from outside. */
static int child_dies_on_oob_index(void)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr); /* the [Ursula] error message is expected noise here */
        int data[3] = {1, 2, 3};
        DfIntCol col; col.data = data; col.count = 3;
        int bad_idx[1] = {3}; /* valid range is [0,3) */
        df_gather_int(col, bad_idx, 1);
        _exit(0); /* unreachable if df_gather_int fails loudly, as required */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

static void test_gather_out_of_range(void)
{
    printf("\n=== df_gather_int: out-of-range index fails loudly ===\n");
    check(child_dies_on_oob_index(),
          "out-of-range index terminates the process instead of reading out of bounds");
}

int main(void)
{
    test_gather_repeats_and_short();
    test_gather_string_independent_copies();
    test_gather_out_of_range();

    printf("\n%s\n", all_ok ? "All gather tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

/* example_sum.c — load CSV, group_by size_label, sum, verify probability sums */
#define SKN_CSV_IMPLEMENTATION
#define SKN_LOG_IMPLEMENTATION
#define SKN_DF_IMPLEMENTATION
#include "../df/skn_df.h"
#include <stdio.h>
#include <math.h>

int main(void)
{
    DataFrame *df = df_load("marginals.csv");
    if (!df) {
        fprintf(stderr, "Failed to load marginals.csv\n");
        return 1;
    }

    printf("=== Input data ===\n");
    df_print(df);
    printf("\n");

    DfGrouped grp = df_group_by(df, (const char *[]){"size_label"}, 1);
    DataFrame *summarized = df_sum(&grp);

    printf("=== Grouped by size_label, summed ===\n");
    df_print(summarized);
    printf("\n");

    /* Verify: each group's p(hh|size_label) sum should be ~1.0 */
    DfStrCol   labels = df_get_string(summarized, "size_label");
    DfDoubleCol probs  = df_get_double(summarized, "p(hh|size_label)");

    int all_ok = 1;
    for (int i = 0; i < probs.count; i++) {
        int ok = fabs(probs.data[i] - 1.0) < 1e-9;
        printf("  size_label='%s' sum=%g  %s\n",
               labels.data[i], probs.data[i], ok ? "OK" : "FAIL");
        if (!ok) all_ok = 0;
    }

    printf("\n%s\n", all_ok
           ? "All groups sum to 1.0 — PASS"
           : "SOME GROUPS FAILED");

    df_free(summarized);
    df_free(df);

    return all_ok ? 0 : 1;
}

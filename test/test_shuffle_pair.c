/* test_shuffle_pair.c -- correctness, determinism, and alignment tests for
 * PM-008: df_shuffle_within_groups(_string), df_pair(_string).
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

/* Returns 1 if arr[lo,hi) contains exactly the same multiset of values as
 * expected[0..len) -- used to confirm a shuffle only reorders within a
 * range, never changes which values live there. */
static int same_multiset(const int *arr, int lo, int hi, const int *expected, int len)
{
    if (hi - lo != len) return 0;
    int used[64] = {0};
    for (int i = lo; i < hi; i++) {
        int found = 0;
        for (int j = 0; j < len; j++) {
            if (!used[j] && arr[i] == expected[j]) { used[j] = 1; found = 1; break; }
        }
        if (!found) return 0;
    }
    return 1;
}

/* ---- criterion 1: group boundaries never move ---- */

static void test_boundaries_preserved(void)
{
    printf("=== df_shuffle_within_groups: group boundaries never move ===\n");

    int sorted_idx[9] = {10, 11, 12, 20, 21, 30, 31, 32, 33};
    int key_data[9]   = {1,  1,  1,  2,  2,  3,  3,  3,  3};
    DfIntCol sorted_key; sorted_key.data = key_data; sorted_key.count = 9;

    int *out = df_shuffle_within_groups(sorted_idx, sorted_key, 9, 42u);

    int grpA[3] = {10, 11, 12};
    int grpB[2] = {20, 21};
    int grpC[4] = {30, 31, 32, 33};
    check(same_multiset(out, 0, 3, grpA, 3), "group A's position range [0,3) still holds exactly {10,11,12}");
    check(same_multiset(out, 3, 5, grpB, 2), "group B's position range [3,5) still holds exactly {20,21}");
    check(same_multiset(out, 5, 9, grpC, 4), "group C's position range [5,9) still holds exactly {30,31,32,33}");

    free(out);
}

/* ---- criterion 2: determinism, same bar as df_sort/df_resample ---- */

static void test_determinism(void)
{
    printf("\n=== df_shuffle_within_groups: determinism (same seed identical, different seed differs) ===\n");

    /* one group of 8 -- 8! = 40320 possible orderings, low collision odds */
    int sorted_idx[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    int key_data[8]   = {1, 1, 1, 1, 1, 1, 1, 1};
    DfIntCol sorted_key; sorted_key.data = key_data; sorted_key.count = 8;

    int *out1 = df_shuffle_within_groups(sorted_idx, sorted_key, 8, 777u);
    int *out2 = df_shuffle_within_groups(sorted_idx, sorted_key, 8, 777u);
    int *out3 = df_shuffle_within_groups(sorted_idx, sorted_key, 8, 778u);

    check(memcmp(out1, out2, 8 * sizeof(int)) == 0,
          "two calls with the same seed and inputs are byte-identical");
    check(memcmp(out1, out3, 8 * sizeof(int)) != 0,
          "a different seed produces a different within-group order (not silently a no-op)");

    free(out1); free(out2); free(out3);
}

/* ---- criterion 3: alignment preservation across two gathered columns ---- */

static void test_alignment(void)
{
    printf("\n=== alignment: two columns gathered through a shuffled index still travel together ===\n");

    /* row r: group_key r/3 (three groups of 3), value = r*10, id = r --
     * value and id are deterministically derivable from each other
     * (value == id * 10), so after shuffling, if they ever came from
     * different rows this check catches it. */
    int group[9], value[9], id[9];
    for (int r = 0; r < 9; r++) { group[r] = r / 3; value[r] = r * 10; id[r] = r; }

    DataFrame *df = df_new(3);
    df_set_column_int(df, 0, "group", group, 9);
    df_set_column_int(df, 1, "value", value, 9);
    df_set_column_int(df, 2, "id",    id,    9);

    int *sorted_idx = df_sort(df, "group"); /* already grouped by construction, but exercise the real pipeline */
    DfIntCol group_col = df_get_int(df, "group");
    DfIntCol sorted_key = df_gather_int(group_col, sorted_idx, 9);

    int *shuffled_idx = df_shuffle_within_groups(sorted_idx, sorted_key, 9, 12345u);

    DfIntCol value_col = df_get_int(df, "value");
    DfIntCol id_col    = df_get_int(df, "id");
    DfIntCol shuffled_value = df_gather_int(value_col, shuffled_idx, 9);
    DfIntCol shuffled_id    = df_gather_int(id_col,    shuffled_idx, 9);

    int ok = 1;
    for (int k = 0; k < 9; k++)
        if (shuffled_value.data[k] != shuffled_id.data[k] * 10) ok = 0;
    check(ok, "row k of both gathered columns (value, id) still comes from the same original row");

    free(sorted_idx);
    free(sorted_key.data);
    free(shuffled_idx);
    free(shuffled_value.data);
    free(shuffled_id.data);
    df_free(df);
}

/* ---- criterion 4: df_pair correctness, hand-checkable case ---- */

static void test_pair_int(void)
{
    printf("\n=== df_pair: hand-checkable correctness (full / partial / no-supply groups) ===\n");
    printf("  group 1: demand=3, supply=5 (fully resolved)\n");
    printf("  group 2: demand=4, supply=2 (partial: 2 matched, 2 residual)\n");
    printf("  group 3: demand=2, supply=0 (absent from supply: all residual)\n");

    int demand_idx[9]  = {100, 101, 102, 200, 201, 202, 203, 300, 301};
    int demand_key[9]  = {1,   1,   1,   2,   2,   2,   2,   3,   3};
    int supply_idx[7]  = {900, 901, 902, 903, 904, 910, 911};
    int supply_key[7]  = {1,   1,   1,   1,   1,   2,   2};

    DfIntCol dk; dk.data = demand_key; dk.count = 9;
    DfIntCol sk; sk.data = supply_key; sk.count = 7;

    DfPairResult r = df_pair(demand_idx, dk, 9, supply_idx, sk, 7);

    check(r.matched_count == 5, "matched_count == 5 (3 from group 1, 2 from group 2)");
    check(r.residual_count == 4, "residual_count == 4 (2 from group 2, 2 from group 3)");

    int exp_demand[5]  = {100, 101, 102, 200, 201};
    int exp_supply[5]  = {900, 901, 902, 910, 911};
    int exp_residual[4] = {202, 203, 300, 301};

    int ok = 1;
    for (int k = 0; k < r.matched_count && k < 5; k++)
        if (r.demand_idx[k] != exp_demand[k] || r.supply_idx[k] != exp_supply[k]) ok = 0;
    check(ok, "matched pairs are exactly {(100,900),(101,901),(102,902),(200,910),(201,911)}");

    ok = 1;
    for (int k = 0; k < r.residual_count && k < 4; k++)
        if (r.residual_idx[k] != exp_residual[k]) ok = 0;
    check(ok, "residual is exactly {202, 203, 300, 301}");

    /* every matched pair actually shares a key value -- the invariant
     * df_pair exists to guarantee */
    ok = 1;
    for (int k = 0; k < r.matched_count; k++) {
        int d_pos = -1, s_pos = -1;
        for (int i = 0; i < 9; i++) if (demand_idx[i] == r.demand_idx[k]) d_pos = i;
        for (int i = 0; i < 7; i++) if (supply_idx[i] == r.supply_idx[k]) s_pos = i;
        if (d_pos < 0 || s_pos < 0 || demand_key[d_pos] != supply_key[s_pos]) ok = 0;
    }
    check(ok, "every demand_idx[k]/supply_idx[k] pair shares the same key value");

    free(r.demand_idx); free(r.supply_idx); free(r.residual_idx);
}

/* ---- criterion 5: zero-count cases ---- */

static void test_zero_count(void)
{
    printf("\n=== df_pair: zero-count cases ===\n");

    int idx[2] = {1, 2};
    int key[2] = {1, 1};
    DfIntCol k2; k2.data = key; k2.count = 2;
    DfIntCol k0; k0.data = NULL; k0.count = 0;

    DfPairResult r1 = df_pair(NULL, k0, 0, idx, k2, 2);
    check(r1.matched_count == 0 && r1.residual_count == 0,
          "n_demand == 0: entirely empty result, no crash");
    free(r1.demand_idx); free(r1.supply_idx); free(r1.residual_idx);

    DfPairResult r2 = df_pair(idx, k2, 2, NULL, k0, 0);
    check(r2.matched_count == 0 && r2.residual_count == 2 &&
          r2.residual_idx[0] == 1 && r2.residual_idx[1] == 2,
          "n_supply == 0: all of demand_idx lands in residual_idx, no crash");
    free(r2.demand_idx); free(r2.supply_idx); free(r2.residual_idx);
}

/* ---- criterion 6: string-keyed variant, same case, through the real pipeline ---- */

static void test_string_pipeline(void)
{
    printf("\n=== string-keyed: same case, through the real sort -> gather -> shuffle -> gather -> pair pipeline ===\n");

    /* demand: 3x A, 4x B, 2x C, built in a scrambled (not pre-sorted) order */
    char *d_conf[9] = {"B", "A", "C", "B", "A", "B", "C", "A", "B"};
    int   d_id[9]   = {0, 1, 2, 3, 4, 5, 6, 7, 8}; /* row index, doubles as an id */
    DataFrame *ddf = df_new(2);
    df_set_column_string(ddf, 0, "conf", d_conf, 9);
    df_set_column_int(ddf, 1, "id", d_id, 9);

    /* supply: 5x A, 2x B, 0x C, also scrambled */
    char *s_conf[7] = {"A", "B", "A", "A", "B", "A", "A"};
    int   s_id[7]   = {0, 1, 2, 3, 4, 5, 6};
    DataFrame *sdf = df_new(2);
    df_set_column_string(sdf, 0, "conf", s_conf, 7);
    df_set_column_int(sdf, 1, "id", s_id, 7);

    /* demand side: sort -> gather -> shuffle -> gather again */
    int *d_order = df_sort(ddf, "conf");
    DfStrCol d_conf_col = df_get_string(ddf, "conf");
    DfStrCol d_sorted_key = df_gather_string(d_conf_col, d_order, 9);
    int *d_shuffled = df_shuffle_within_groups_string(d_order, d_sorted_key, 9, 111u);
    DfStrCol d_final_key = df_gather_string(d_conf_col, d_shuffled, 9);

    /* supply side: same pipeline, independent seed */
    int *s_order = df_sort(sdf, "conf");
    DfStrCol s_conf_col = df_get_string(sdf, "conf");
    DfStrCol s_sorted_key = df_gather_string(s_conf_col, s_order, 7);
    int *s_shuffled = df_shuffle_within_groups_string(s_order, s_sorted_key, 7, 222u);
    DfStrCol s_final_key = df_gather_string(s_conf_col, s_shuffled, 7);

    DfPairResult r = df_pair_string(d_shuffled, d_final_key, 9, s_shuffled, s_final_key, 7);

    check(r.matched_count == 5, "matched_count == 5, same as the int-keyed case");
    check(r.residual_count == 4, "residual_count == 4, same as the int-keyed case");

    /* every matched pair actually shares a conf value -- d_shuffled[k]/s_shuffled[k]
     * are row indices into ddf/sdf since id[i] == i by construction */
    int ok = 1;
    for (int k = 0; k < r.matched_count; k++) {
        const char *dc = d_conf[r.demand_idx[k]];
        const char *sc = s_conf[r.supply_idx[k]];
        if (strcmp(dc, sc) != 0) ok = 0;
    }
    check(ok, "every matched pair's rows genuinely share the same conf value");

    /* group C (absent from supply) must be entirely in residual */
    int c_in_residual = 0;
    for (int k = 0; k < r.residual_count; k++)
        if (strcmp(d_conf[r.residual_idx[k]], "C") == 0) c_in_residual++;
    check(c_in_residual == 2, "both group-C demand rows (absent from supply) landed in residual");

    /* group B: exactly 2 matched, 2 residual (demand=4, supply=2) */
    int b_matched = 0, b_residual = 0;
    for (int k = 0; k < r.matched_count; k++)
        if (strcmp(d_conf[r.demand_idx[k]], "B") == 0) b_matched++;
    for (int k = 0; k < r.residual_count; k++)
        if (strcmp(d_conf[r.residual_idx[k]], "B") == 0) b_residual++;
    check(b_matched == 2 && b_residual == 2, "group B split exactly 2 matched / 2 residual");

    free(d_order); free(d_sorted_key.data); free(d_shuffled); free(d_final_key.data);
    free(s_order); free(s_sorted_key.data); free(s_shuffled); free(s_final_key.data);
    free(r.demand_idx); free(r.supply_idx); free(r.residual_idx);
    df_free(ddf);
    df_free(sdf);
}

int main(void)
{
    test_boundaries_preserved();
    test_determinism();
    test_alignment();
    test_pair_int();
    test_zero_count();
    test_string_pipeline();

    printf("\n%s\n", all_ok ? "All shuffle/pair tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

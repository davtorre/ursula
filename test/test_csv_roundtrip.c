/* test_csv_roundtrip.c — df_write round-trip and CSV escaping tests
 *
 * 1. marginals.csv -> df_write -> df_load: column data identical to source.
 * 2. A synthetic CSV whose string column contains a comma, a double quote,
 *    and an embedded newline round-trips correctly through
 *    df_load -> df_write -> df_load.
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
    printf("  %-58s %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) all_ok = 0;
}

/* Compare two DataFrames field-by-field: shape, names, types, values. */
static void compare_frames(const DataFrame *a, const DataFrame *b, const char *label)
{
    char msg[160];

    snprintf(msg, sizeof(msg), "%s: col_count matches", label);
    check(a->col_count == b->col_count, msg);

    snprintf(msg, sizeof(msg), "%s: row_count matches", label);
    check(a->row_count == b->row_count, msg);

    int cols = a->col_count < b->col_count ? a->col_count : b->col_count;
    int rows = a->row_count < b->row_count ? a->row_count : b->row_count;

    for (int c = 0; c < cols; c++) {
        snprintf(msg, sizeof(msg), "%s: column %d name matches ('%s')", label, c, a->names[c]);
        check(strcmp(a->names[c], b->names[c]) == 0, msg);

        snprintf(msg, sizeof(msg), "%s: column '%s' type matches", label, a->names[c]);
        check(a->types[c] == b->types[c], msg);
        if (a->types[c] != b->types[c]) continue;

        int col_ok = 1;
        switch (a->types[c]) {
            case DF_INT:
                for (int r = 0; r < rows; r++)
                    if (a->data[c].i[r] != b->data[c].i[r]) col_ok = 0;
                break;
            case DF_FLOAT:
                for (int r = 0; r < rows; r++)
                    if (a->data[c].f[r] != b->data[c].f[r]) col_ok = 0;
                break;
            case DF_DOUBLE:
                for (int r = 0; r < rows; r++)
                    if (a->data[c].d[r] != b->data[c].d[r]) col_ok = 0;
                break;
            case DF_STRING:
                for (int r = 0; r < rows; r++)
                    if (strcmp(a->data[c].s[r], b->data[c].s[r]) != 0) col_ok = 0;
                break;
        }
        snprintf(msg, sizeof(msg), "%s: column '%s' values match", label, a->names[c]);
        check(col_ok, msg);
    }
}

static void write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) { check(0, "write synthetic input csv"); return; }
    fputs(text, fp);
    fclose(fp);
}

static void test_roundtrip_marginals(void)
{
    printf("=== Round-trip: marginals.csv ===\n");

    DataFrame *orig = df_load("marginals.csv");
    if (!orig) { check(0, "load marginals.csv"); return; }

    int rc = df_write(orig, "test_roundtrip_out.csv");
    check(rc == 0, "df_write returns 0");

    DataFrame *reloaded = df_load("test_roundtrip_out.csv");
    if (!reloaded) { check(0, "reload written csv"); df_free(orig); return; }

    compare_frames(orig, reloaded, "marginals");

    df_free(orig);
    df_free(reloaded);
}

static void test_escaping(void)
{
    printf("\n=== Escaping round-trip ===\n");

    /* note[0] has a literal comma, note[1] a literal double quote,
     * note[2] an embedded newline — all inside RFC-4180 quoted fields. */
    const char *synthetic =
        "id,name,note\n"
        "1,Alice,\"hello, world\"\n"
        "2,Bob,\"she said \"\"hi\"\"\"\n"
        "3,Carol,\"line1\nline2\"\n";

    write_text_file("test_escaping_input.csv", synthetic);

    DataFrame *a = df_load("test_escaping_input.csv");
    if (!a) { check(0, "load synthetic csv"); return; }

    check(a->row_count == 3, "synthetic csv: 3 rows parsed");

    DfStrCol notes = df_get_string(a, "note");
    check(notes.count == 3, "synthetic csv: note column has 3 values");
    check(strcmp(notes.data[0], "hello, world") == 0, "note[0] preserves literal comma");
    check(strcmp(notes.data[1], "she said \"hi\"") == 0, "note[1] preserves literal double quote");
    check(strcmp(notes.data[2], "line1\nline2") == 0, "note[2] preserves embedded newline");

    int rc = df_write(a, "test_escaping_out.csv");
    check(rc == 0, "df_write returns 0 on synthetic frame");

    DataFrame *b = df_load("test_escaping_out.csv");
    if (!b) { check(0, "reload written synthetic csv"); df_free(a); return; }

    compare_frames(a, b, "escaping");

    df_free(a);
    df_free(b);
}

int main(void)
{
    test_roundtrip_marginals();
    test_escaping();

    printf("\n%s\n", all_ok ? "All CSV round-trip tests PASS" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}

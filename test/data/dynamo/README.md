# `test/data/dynamo/` — real-data fixture for `test_dynamo_assignment.c`

## What's here

Three CSVs, exported once from citypop's real files by `prep_dynamo_csvs.py` (read-only
against citypop — never modifies it). This is not synthetic data; every row traces back to
citypop's actual `dynamo_sweden_v1.json` solve output and its real household-configuration /
family-type-supply tables. `skn_df.h` only reads CSV (via skinny-csv), so this conversion —
JSON parsing, Parquet reading — has to happen outside the C library; that's the whole reason
this directory and its prep script exist.

| File | Columns | Rows | Source |
|---|---|---|---|
| `idx_demand.csv` | `idx,demand` | 6,451 | `dynamo_sweden_v1.json`'s per-tract `sparse_vector`s, summed by `idx` across all 6,107 tracts |
| `idx_catalog.csv` | `idx,household_conf` | 15,309 | `prototypes/data/household_config_dynamo_real.csv`, projected to two columns |
| `family_type_supply.csv` | `household_conf,family_type,count` | 1,727 | `data/dynamo-ab-2020-v0.1.0/tables/household_type_given_conf.parquet`, `geo_level`/`geo_id` dropped (both constant: `national`/`SE`) |

Regenerate with:
```
python3 test/data/dynamo/prep_dynamo_csvs.py
```
(needs citypop checked out at `/Users/davide.torre/Research/Projects/citypop` and its
`scripts/report_generation/.venv` for `polars`, per the script's own header.)

`idx_demand.csv`'s total (5,565,743) matches `PMD-005`'s stated total household count exactly
— the solve output itself (`dynamo_sweden_v1.json`) is unchanged from what `PMD-005` used.

## The blocker: `idx_catalog.csv` is a newer vintage than `idx_demand.csv`

Verified this session, against citypop's own briefs, not assumed:

- `dynamo_sweden_v1.json`'s `idx` values were assigned against the **v0.1.0** DYNAMO release
  (`data/dynamo-ab-2020-v0.1.0/`) — confirmed unchanged since `PMD-004`'s original solve
  (`citypop/briefs/PMD-011.md` calls it "the v0.1.0-based checkpoint (`dynamo_sweden_v1.json`)").
- `prototypes/data/household_config_dynamo_real.csv` (the source of `idx_catalog.csv`) was
  **regenerated in place** for the **v0.1.1** release by `citypop/briefs/PMD-010.md` — dated
  *after* `PMD-005`, which explicitly documents this as intentional: "these are untracked build
  artifacts regenerated from source, not versioned snapshots, so overwriting in place is the
  right call." `PMD-010.md` also warns `family_size` itself changed range (1–10 → 1–12) between
  releases, i.e. this isn't a cosmetic revision — the catalog's row structure genuinely changed.
- Both `prototypes/data/` and `scripts/preprocessing_data_scripts/` are gitignored in citypop
  (confirmed via `citypop/.gitignore`), so there is no git history to recover the original
  v0.1.0-era `household_config_dynamo_real.csv` from, and no other copy of it was found anywhere
  in this environment.
- Confirms independently: `prototypes/data/dynamo_family_type_assignment.parquet` (the file
  `PMD-005` itself produced) **currently on disk does not match `PMD-005`'s own documented run
  summary** — it has 5,559,031 rows / 2,263 distinct `household_conf` / 215 `bootstrap`, not
  `PMD-005`'s recorded 5,565,743 / 691 / 14,140. `citypop/briefs/PMD-011.md` and `PMD-014.md`
  confirm why: that file was rebuilt against the v0.1.1 pipeline (a different seed, `20260813`,
  not `PMD-005`'s `20260812`) by later work, overwriting `PMD-005`'s original output in place.

Net effect: joining `idx_demand.csv` (v0.1.0-era `idx` values) against `idx_catalog.csv`
(v0.1.1-era `idx` → `household_conf` mapping) is a **version-mismatched join** — every `idx`
number still resolves to *some* `household_conf` string (so `df_match` doesn't fail-fast; the
numbers just don't mean what `PMD-005` needs them to mean). Concretely: 636 distinct
`household_conf` with demand > 0 (not 691), 170 of them with **zero** supply at all (`PMD-005`
verified this at exactly 0 for the correctly-matched v0.1.0 data), and a 30%+ shortfall rate
(not `PMD-005`'s 0.254%). None of that is "close enough" — it's a different, meaningless join,
exactly the outcome `PM-004`'s own brief says to flag rather than paper over.

**No fix was attempted here.** Reconstructing the original v0.1.0 catalog would mean
reverse-engineering and rerunning citypop's now-edited-in-place preprocessing script against
citypop's still-present raw v0.1.0 tables — a nontrivial, risky change to a sibling project this
session wasn't asked to touch, on the PM's authority to make, not this session's to assume.

## What `test_dynamo_assignment.c` does about it

- **Section A** runs the real join anyway and reports the actual numbers above against
  `PMD-005`'s targets, explicitly labeled `MISMATCH`/`BLOCKED` (never silently passed, never
  silently substituted) — this is the flagged blocker, in full, every time the test runs.
- **Section B** proves `df_match`, `df_assert_unique`, `df_sort`, `df_gather_int/string`,
  `df_resample`, and `df_stack_v_int` are themselves correct at real scale, by running the full
  tiered-assignment pipeline over the subset of this same real data where the pipeline's own
  precondition (every demand `household_conf` has *some* supply) genuinely holds — 466 of the
  636 groups, 5,511,398 households — checking internal-consistency invariants and byte-identical
  reproducibility across two independent runs with the same seed. This is the part the test's
  exit status reflects: a data-provenance gap in a sibling project isn't an Ursula defect.

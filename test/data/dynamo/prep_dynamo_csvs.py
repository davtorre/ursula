#!/usr/bin/env python3
"""One-time data-prep step for ursula's PM-004 acceptance test
(test/test_dynamo_assignment.c). Exports three CSVs from citypop's real
dynamo-ab-2020-v0.1.0 bundle, read-only against citypop (never modifies it).

Ursula's tech stack is CSV-only (skinny-csv) with no JSON/Parquet support,
so this conversion has to happen outside the C library -- it is exactly the
"as CSV" step PM-004's brief itself calls for.

Source files (citypop, read-only):
  - /Users/davide.torre/Research/Projects/citypop/dynamo_sweden_v1.json
      (PMD-004's solve output: per-tract {idx: count} demand)
  - /Users/davide.torre/Research/Projects/citypop/prototypes/data/household_config_dynamo_real.csv
      (idx -> household_conf catalog)
  - /Users/davide.torre/Research/Projects/citypop/data/dynamo-ab-2020-v0.1.0/tables/household_type_given_conf.parquet
      (household_conf/family_type supply counts, national level)

Output (ursula/test/data/dynamo/):
  - idx_demand.csv        idx,demand            (summed across all 6,107 tracts)
  - idx_catalog.csv       idx,household_conf    (catalog projection)
  - family_type_supply.csv household_conf,family_type,count  (geo columns dropped -- constant)
"""
import csv
import json
import sys
from collections import defaultdict

CITYPOP = "/Users/davide.torre/Research/Projects/citypop"
OUT = "/Users/davide.torre/Research/Projects/ursula/test/data/dynamo"

sys.path.insert(0, f"{CITYPOP}/scripts/report_generation/.venv/lib/python3.13/site-packages")
try:
    import polars as pl
except ImportError:
    # fall back: locate the venv's site-packages directory dynamically
    import glob
    candidates = glob.glob(f"{CITYPOP}/scripts/report_generation/.venv/lib/python3*/site-packages")
    if not candidates:
        raise
    sys.path.insert(0, candidates[0])
    import polars as pl

# ---- idx_demand.csv: sum sparse_vector counts per idx across all tracts ----
with open(f"{CITYPOP}/dynamo_sweden_v1.json") as f:
    solve = json.load(f)

demand = defaultdict(int)
total = 0
for tract_id, rec in solve["data"].items():
    for idx_str, cnt in rec["sparse_vector"].items():
        demand[int(idx_str)] += cnt
        total += cnt

with open(f"{OUT}/idx_demand.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["idx", "demand"])
    for idx in sorted(demand):
        w.writerow([idx, demand[idx]])

print(f"idx_demand.csv: {len(demand)} distinct idx, total demand = {total}")

# ---- idx_catalog.csv: idx -> household_conf projection ----
n_catalog = 0
with open(f"{CITYPOP}/prototypes/data/household_config_dynamo_real.csv") as f_in, \
     open(f"{OUT}/idx_catalog.csv", "w", newline="") as f_out:
    r = csv.DictReader(f_in)
    w = csv.writer(f_out)
    w.writerow(["idx", "household_conf"])
    for row in r:
        w.writerow([row["idx"], row["household_conf"]])
        n_catalog += 1

print(f"idx_catalog.csv: {n_catalog} rows")

# ---- family_type_supply.csv: household_conf, family_type, count (drop geo_level/geo_id) ----
supply = pl.read_parquet(
    f"{CITYPOP}/data/dynamo-ab-2020-v0.1.0/tables/household_type_given_conf.parquet"
)
assert supply["geo_level"].n_unique() == 1 and supply["geo_level"][0] == "national"
assert supply["geo_id"].n_unique() == 1 and supply["geo_id"][0] == "SE"
supply.select(["household_conf", "family_type", "count"]).write_csv(
    f"{OUT}/family_type_supply.csv"
)
print(f"family_type_supply.csv: {supply.shape[0]} rows, {supply['household_conf'].n_unique()} distinct household_conf")

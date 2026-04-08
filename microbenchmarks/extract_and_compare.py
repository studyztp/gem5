#!/usr/bin/env python3
"""
Extract gem5 cycle data from simout files, compare with real hardware data,
and produce CSV reports.

Usage:
    python3 extract_and_compare.py

Outputs:
    gem5_cycles.csv        - extracted gem5 per-call cycles
    hw_cycles.csv          - real hardware reference data
    comparison.csv         - full comparison with absolute/relative accuracy
"""

import csv
import os
import re
import sys

# =====================================================================
# Step 1: Extract gem5 cycles from simout files
# =====================================================================


def extract_gem5_cycles(result_dir):
    """Extract per-call and total cycles from all simout.txt files in a
    result directory.

    Returns dict: bench_name -> {total_cycles, per_call_cycles, inner_reps}
    """
    data = {}
    if not os.path.isdir(result_dir):
        print(f"WARNING: directory not found: {result_dir}", file=sys.stderr)
        return data

    for entry in sorted(os.listdir(result_dir)):
        simout = os.path.join(result_dir, entry, "simout.txt")
        if not os.path.isfile(simout):
            continue

        # Extract bench name from directory: m5out_entobench_all_bench-XXX
        m = re.match(r"m5out_entobench_all_bench-(.+)", entry)
        if not m:
            continue
        bench = m.group(1)

        total = None
        per_call = None
        inner_reps = None

        with open(simout) as f:
            for line in f:
                # Match: "  Cycles (per call) : 161.8  (total / 10 inner_reps)"
                m_pc = re.search(
                    r"Cycles \(per call\)\s*:\s*([\d.]+)\s*\(total / (\d+)",
                    line,
                )
                if m_pc:
                    per_call = float(m_pc.group(1))
                    inner_reps = int(m_pc.group(2))

                # Match: "  Cycles (total)    : 1618.0"
                m_tot = re.search(r"Cycles \(total\)\s*:\s*([\d.]+)", line)
                if m_tot:
                    total = float(m_tot.group(1))

        if per_call is not None:
            data[bench] = {
                "total_cycles": total,
                "per_call_cycles": per_call,
                "inner_reps": inner_reps,
            }
        elif total is not None:
            # No per-call line means inner_reps=1 or missing
            data[bench] = {
                "total_cycles": total,
                "per_call_cycles": total,
                "inner_reps": 1,
            }

    return data


def write_gem5_csv(art_data, noart_data, outpath):
    """Write gem5 cycles to CSV."""
    all_benches = sorted(set(art_data.keys()) | set(noart_data.keys()))

    with open(outpath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "benchmark",
                "art_total_cycles",
                "art_per_call_cycles",
                "art_inner_reps",
                "noart_total_cycles",
                "noart_per_call_cycles",
                "noart_inner_reps",
            ]
        )
        for bench in all_benches:
            art = art_data.get(bench, {})
            noart = noart_data.get(bench, {})
            w.writerow(
                [
                    bench,
                    art.get("total_cycles", ""),
                    art.get("per_call_cycles", ""),
                    art.get("inner_reps", ""),
                    noart.get("total_cycles", ""),
                    noart.get("per_call_cycles", ""),
                    noart.get("inner_reps", ""),
                ]
            )

    print(f"Wrote {outpath} ({len(all_benches)} benchmarks)")


# =====================================================================
# Step 2: Real hardware data (from STM32G474RE @ 170 MHz, dual bank)
# =====================================================================

# Source: real board measurements table (dual bank columns)
# C+P = Cache + Prefetch ON
# None = Cache OFF, Prefetch OFF
HW_DATA = {
    # bench:        (ops, hw_cp_dual, hw_none_dual)
    "nop": (100, 105, 167),
    "alu": (100, 107, 323),
    "alu16": (100, 105, 167),
    "mul": (100, 107, 323),
    "div": (20, 247, 260),
    "div_short": (20, 107, 120),
    "pushpop": (6, 55, 76),
    "fpu": (45, 112, 196),
    "dmb": (50, 75, 222),
    "mixed_width": (100, 104, 240),
    "load": (50, 58, 91),
    "load_dep": (50, 108, 119),
    "store": (50, 57, 94),
    "store_burst": (50, 66, 127),
    "ldr_literal": (50, 56, 221),
    "ccm_sram": (100, 112, 178),
    "scs_read": (50, 58, 95),
    "scs_store": (50, 56, 127),
    "art_prefetch": (200, 207, 323),
    "icache_miss": (4096, 5195, 6220),
    "branch": (100, 305, 1013),
    "bp_tight": (200, 605, 2013),
    "bp_long_body": (100, 1105, 2213),
    "bp_forward": (100, 607, 1817),
    "bp_alternating": (200, 1207, 3420),
    "bp_nested": (200, 667, 2319),
    "bp_align": (300, 608, 2211),
}

# Mapping: which gem5 benchmark name to use for each HW benchmark
# (use _v2 variant where it exists and matches the real board's register setup)
GEM5_BENCH_MAP = {
    "nop": "nop",
    "alu": "alu_v2",
    "alu16": "alu16_v2",
    "mul": "mul",
    "div": "div",
    "div_short": "div_short",
    "pushpop": "pushpop_v2",
    "fpu": "fpu",
    "dmb": "dmb",
    "mixed_width": "mixed_width_v2",
    "load": "load",
    "load_dep": "load_dep",
    "store": "store",
    "store_burst": "store_burst",
    "ldr_literal": "ldr_literal",
    "ccm_sram": "ccm_sram",
    "scs_read": "scs_read",
    "scs_store": "scs_store",
    "art_prefetch": "art_prefetch",
    "icache_miss": "icache_miss",
    "branch": "branch",
    "bp_tight": "bp_tight",
    "bp_long_body": "bp_long_body",
    "bp_forward": "bp_forward_v2",
    "bp_alternating": "bp_alternating",
    "bp_nested": "bp_nested_v2",
    "bp_align": "bp_align",
}

# Category assignments
CATEGORIES = {
    "nop": "Compute",
    "alu": "Compute",
    "alu16": "Compute",
    "mul": "Compute",
    "div": "Compute",
    "div_short": "Compute",
    "pushpop": "Compute",
    "fpu": "Compute",
    "dmb": "Compute",
    "mixed_width": "Compute",
    "load": "Memory",
    "load_dep": "Memory",
    "store": "Memory",
    "store_burst": "Memory",
    "ldr_literal": "Memory",
    "ccm_sram": "Memory",
    "scs_read": "Memory",
    "scs_store": "Memory",
    "art_prefetch": "Cache",
    "icache_miss": "Cache",
    "branch": "Branch",
    "bp_tight": "Branch",
    "bp_long_body": "Branch",
    "bp_forward": "Branch",
    "bp_alternating": "Branch",
    "bp_nested": "Branch",
    "bp_align": "Branch",
}


def write_hw_csv(outpath):
    """Write hardware reference data to CSV."""
    with open(outpath, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "benchmark",
                "category",
                "ops",
                "hw_cp_dual_cycles",
                "hw_none_dual_cycles",
                "gem5_bench_name",
            ]
        )
        for bench in sorted(HW_DATA.keys()):
            ops, cp, none = HW_DATA[bench]
            w.writerow(
                [
                    bench,
                    CATEGORIES[bench],
                    ops,
                    cp,
                    none,
                    GEM5_BENCH_MAP[bench],
                ]
            )

    print(f"Wrote {outpath} ({len(HW_DATA)} benchmarks)")


# =====================================================================
# Step 3 & 4: Compare and compute accuracy metrics
# =====================================================================


def safe_pct(a, b):
    """Percentage difference: (a - b) / b * 100. Returns None if b==0."""
    if b == 0:
        return None
    return (a - b) / b * 100


def compare_and_write(art_data, noart_data, outpath):
    """Compare gem5 vs HW and write full comparison CSV."""
    rows = []

    for hw_bench in sorted(HW_DATA.keys()):
        ops, hw_cp, hw_none = HW_DATA[hw_bench]
        gem5_bench = GEM5_BENCH_MAP[hw_bench]
        category = CATEGORIES[hw_bench]

        gem5_art = art_data.get(gem5_bench, {}).get("per_call_cycles")
        gem5_noart = noart_data.get(gem5_bench, {}).get("per_call_cycles")

        # Absolute accuracy: gem5 vs HW for each config
        art_abs_err = safe_pct(gem5_art, hw_cp) if gem5_art else None
        noart_abs_err = safe_pct(gem5_noart, hw_none) if gem5_noart else None

        # Speedups: None -> C+P
        hw_speedup = hw_none / hw_cp if hw_cp > 0 else None
        gem5_speedup = (
            gem5_noart / gem5_art
            if gem5_art and gem5_noart and gem5_art > 0
            else None
        )

        # Relative accuracy: speedup error
        speedup_err = (
            safe_pct(gem5_speedup, hw_speedup)
            if gem5_speedup and hw_speedup
            else None
        )

        rows.append(
            {
                "benchmark": hw_bench,
                "category": category,
                "ops": ops,
                "gem5_bench_name": gem5_bench,
                "hw_cp_dual": hw_cp,
                "hw_none_dual": hw_none,
                "gem5_art": gem5_art,
                "gem5_noart": gem5_noart,
                "art_abs_error_pct": art_abs_err,
                "noart_abs_error_pct": noart_abs_err,
                "hw_speedup": hw_speedup,
                "gem5_speedup": gem5_speedup,
                "speedup_error_pct": speedup_err,
                "speedup_abs_error_pct": (
                    abs(speedup_err) if speedup_err is not None else None
                ),
            }
        )

    # Write CSV
    fieldnames = [
        "benchmark",
        "category",
        "ops",
        "gem5_bench_name",
        "hw_cp_dual",
        "hw_none_dual",
        "gem5_art",
        "gem5_noart",
        "art_abs_error_pct",
        "noart_abs_error_pct",
        "hw_speedup",
        "gem5_speedup",
        "speedup_error_pct",
        "speedup_abs_error_pct",
    ]

    with open(outpath, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for row in rows:
            # Format floats
            out = {}
            for k, v in row.items():
                if isinstance(v, float):
                    out[k] = f"{v:.1f}"
                elif v is None:
                    out[k] = ""
                else:
                    out[k] = v
            w.writerow(out)

    print(f"Wrote {outpath} ({len(rows)} benchmarks)")

    # Print summary to stdout
    print("\n" + "=" * 70)
    print("COMPARISON SUMMARY")
    print("=" * 70)

    for cat in ["Compute", "Memory", "Cache", "Branch"]:
        cat_rows = [r for r in rows if r["category"] == cat]
        art_errs = [
            abs(r["art_abs_error_pct"])
            for r in cat_rows
            if r["art_abs_error_pct"] is not None
        ]
        noart_errs = [
            abs(r["noart_abs_error_pct"])
            for r in cat_rows
            if r["noart_abs_error_pct"] is not None
        ]
        spd_errs = [
            abs(r["speedup_error_pct"])
            for r in cat_rows
            if r["speedup_error_pct"] is not None
        ]

        print(f"\n{cat}:")
        if art_errs:
            print(
                f"  ART abs error:     mean={sum(art_errs)/len(art_errs):.1f}%  "
                f"max={max(art_errs):.1f}%"
            )
        if noart_errs:
            print(
                f"  No-ART abs error:  mean={sum(noart_errs)/len(noart_errs):.1f}%  "
                f"max={max(noart_errs):.1f}%"
            )
        if spd_errs:
            print(
                f"  Speedup abs error: mean={sum(spd_errs)/len(spd_errs):.1f}%  "
                f"max={max(spd_errs):.1f}%"
            )

    # Overall
    all_art = [
        abs(r["art_abs_error_pct"])
        for r in rows
        if r["art_abs_error_pct"] is not None
    ]
    all_noart = [
        abs(r["noart_abs_error_pct"])
        for r in rows
        if r["noart_abs_error_pct"] is not None
    ]
    all_spd = [
        abs(r["speedup_error_pct"])
        for r in rows
        if r["speedup_error_pct"] is not None
    ]

    print(f"\nOverall ({len(rows)} benchmarks):")
    if all_art:
        print(
            f"  ART abs error:     mean={sum(all_art)/len(all_art):.1f}%  "
            f"max={max(all_art):.1f}%"
        )
    if all_noart:
        print(
            f"  No-ART abs error:  mean={sum(all_noart)/len(all_noart):.1f}%  "
            f"max={max(all_noart):.1f}%"
        )
    if all_spd:
        print(
            f"  Speedup abs error: mean={sum(all_spd)/len(all_spd):.1f}%  "
            f"max={max(all_spd):.1f}%"
        )

    print("=" * 70)

    # Write summary CSV
    summary_path = outpath.replace("comparison.csv", "comparison_summary.csv")
    summary_rows = []

    def calc_stats(values):
        if not values:
            return None, None, None
        mean_val = sum(values) / len(values)
        max_val = max(values)
        return mean_val, max_val, len(values)

    for cat in ["Compute", "Memory", "Cache", "Branch", "All"]:
        if cat == "All":
            cat_rows = rows
        else:
            cat_rows = [r for r in rows if r["category"] == cat]

        art_vals = [
            abs(r["art_abs_error_pct"])
            for r in cat_rows
            if r["art_abs_error_pct"] is not None
        ]
        noart_vals = [
            abs(r["noart_abs_error_pct"])
            for r in cat_rows
            if r["noart_abs_error_pct"] is not None
        ]
        spd_vals = [
            abs(r["speedup_error_pct"])
            for r in cat_rows
            if r["speedup_error_pct"] is not None
        ]

        art_mean, art_max, art_n = calc_stats(art_vals)
        noart_mean, noart_max, noart_n = calc_stats(noart_vals)
        spd_mean, spd_max, spd_n = calc_stats(spd_vals)

        summary_rows.append(
            {
                "category": cat,
                "num_benchmarks": len(cat_rows),
                "art_abs_error_mean": art_mean,
                "art_abs_error_max": art_max,
                "art_abs_error_count": art_n,
                "noart_abs_error_mean": noart_mean,
                "noart_abs_error_max": noart_max,
                "noart_abs_error_count": noart_n,
                "speedup_abs_error_mean": spd_mean,
                "speedup_abs_error_max": spd_max,
                "speedup_abs_error_count": spd_n,
            }
        )

    summary_fields = [
        "category",
        "num_benchmarks",
        "art_abs_error_mean",
        "art_abs_error_max",
        "art_abs_error_count",
        "noart_abs_error_mean",
        "noart_abs_error_max",
        "noart_abs_error_count",
        "speedup_abs_error_mean",
        "speedup_abs_error_max",
        "speedup_abs_error_count",
    ]

    with open(summary_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=summary_fields)
        w.writeheader()
        for row in summary_rows:
            out = {}
            for k, v in row.items():
                if v is None:
                    out[k] = ""
                else:
                    out[k] = v
            w.writerow(out)

    print(f"Wrote {summary_path} ({len(summary_rows)} categories)")


# =====================================================================
# Main
# =====================================================================

if __name__ == "__main__":
    base = os.path.dirname(os.path.abspath(__file__))
    art_dir = os.path.join(base, "gem5-result-art")
    noart_dir = os.path.join(base, "gem5-result-noart")

    print("Extracting gem5 ART cycles...")
    art_data = extract_gem5_cycles(art_dir)
    print(f"  Found {len(art_data)} benchmarks")

    print("Extracting gem5 No-ART cycles...")
    noart_data = extract_gem5_cycles(noart_dir)
    print(f"  Found {len(noart_data)} benchmarks")

    # Step 1: gem5 cycles CSV
    write_gem5_csv(art_data, noart_data, os.path.join(base, "gem5_cycles.csv"))

    # Step 2: HW reference CSV
    write_hw_csv(os.path.join(base, "hw_cycles.csv"))

    # Step 3 & 4: comparison CSV
    compare_and_write(
        art_data, noart_data, os.path.join(base, "comparison.csv")
    )

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn a results directory into plots and a SUMMARY.md.

Reads only the language-neutral files described in docs/HARNESS_SPEC.md:

    <dir>/machine.json
    <dir>/latency_summary.csv
    <dir>/percentiles/<scenario>__<queue>.csv
    <dir>/hist/<scenario>__<queue>.csv
    <dir>/throughput.csv            (optional; derived from benchmark.json if absent)

so the same script works for the C++ results and for the Rust port. Several
result directories can be passed to overlay implementations:

    python tools/analyze.py results/i5-6500T_win11
    python tools/analyze.py results/cpp_run results/rust_run --out results/compare
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker as mticker  # noqa: E402

# Validated categorical palette (fixed order, never cycled).
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
SURFACE = "#fcfcfb"

# Colour follows the entity: each queue keeps its colour in every chart.
# Eight categorical slots for eight entities; the ninth folds to neutral gray
# (never a generated hue).
QUEUE_ORDER = ["ring", "mutex_queue", "mutex_ring", "ring_index", "ring_pad64",
               "ring_pause", "ring_naive", "ring_nocache", "ring_packed"]
OTHER = "#8a8984"
QUEUE_COLOR = {q: (SERIES[i] if i < len(SERIES) else OTHER) for i, q in enumerate(QUEUE_ORDER)}
QUEUE_LABEL = {
    "ring": "SPSC ring (lock-free)",
    "ring_index": "ring, signal via tail index",
    "mutex_queue": "std::mutex + std::queue",
    "mutex_ring": "std::mutex + ring",
    "ring_nocache": "ring, no cached index",
    "ring_packed": "ring, no padding",
    "ring_naive": "ring, no padding, no cache",
    "ring_pad64": "ring, 64 B padding",
    "ring_pause": "ring, PAUSE in spin loop",
}
HEADLINE = ["ring", "mutex_queue", "mutex_ring"]
ABLATION = ["ring", "ring_index", "ring_pad64", "ring_pause", "ring_nocache", "ring_naive",
            "ring_packed"]
# The percentile-curve version shows fewer lines so each stays readable.
ABLATION_CURVES = ["ring", "ring_index", "ring_pad64", "ring_pause", "ring_naive"]
TP_SCENARIOS = {"throughput": "one message per operation",
                "throughput_batch32": "batches of up to 32 (try_push_n / try_pop_n)"}

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": INK_2, "text.color": INK,
    "xtick.color": INK_2, "ytick.color": INK_2, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 10, "axes.titlesize": 12, "axes.titleweight": "bold",
    "axes.titlelocation": "left", "legend.frameon": False, "lines.linewidth": 2,
})


# ---- loading ----------------------------------------------------------------

def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def load_run(d: Path) -> dict:
    run = {"dir": d, "machine": json.loads((d / "machine.json").read_text())}
    run["summary"] = read_csv(d / "latency_summary.csv")
    run["impl"] = run["summary"][0]["impl"] if run["summary"] else d.name
    run["throughput"] = load_throughput(d)
    return run


def load_throughput(d: Path) -> dict[str, dict[str, list[float]]]:
    """scenario -> queue -> per-repetition throughput in messages/s."""
    out: dict[str, dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    if (d / "throughput.csv").exists():
        for r in read_csv(d / "throughput.csv"):
            out[r.get("scenario") or "throughput"][r["queue"]].append(float(r["msgs_per_s"]))
        return out
    bj = d / "benchmark.json"
    if bj.exists():
        for b in json.loads(bj.read_text())["benchmarks"]:
            scenario, _, rest = b["name"].partition("/")
            if b.get("run_type") == "iteration" and scenario in TP_SCENARIOS:
                out[scenario][rest.split("/")[0]].append(b["items_per_second"])
    return out


def series(d: Path, kind: str, scenario: str, queue: str) -> tuple[list[float], list[float]] | None:
    p = d / kind / f"{scenario}__{queue}.csv"
    if not p.exists():
        return None
    rows = read_csv(p)
    if kind == "percentiles":
        return [float(r["percentile"]) for r in rows], [float(r["latency_ns"]) for r in rows]
    return [float(r["bucket_lo_ns"]) for r in rows], [float(r["count"]) for r in rows]


def scenarios(run: dict) -> list[str]:
    return sorted({r["scenario"] for r in run["summary"]})


# ---- plots ------------------------------------------------------------------

NINES = [0, 90, 99, 99.9, 99.99, 99.999, 99.9999]


def nines(p: float) -> float:
    """Percentile -> 'number of nines' so the tail gets most of the x-axis."""
    return -math.log10(max(1 - p / 100.0, 1e-7))


def fmt_ns(v: float, _=None) -> str:
    if v >= 1e6:
        return f"{v / 1e6:g} ms"
    if v >= 1e3:
        return f"{v / 1e3:g} µs"
    return f"{v:g} ns"


def style_log_y(ax):
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(fmt_ns))
    ax.yaxis.set_minor_formatter(mticker.NullFormatter())


def titled(ax, title: str, run: dict):
    ax.set_title(title, pad=24)
    ax.text(0, 1.02, subtitle(run), transform=ax.transAxes, fontsize=8, color=INK_2)


def subtitle(run: dict) -> str:
    m = run["machine"]
    return (f"{m['cpu']} · {m['os']} · {m['compiler']} · producer CPU {m['producer_cpu']}, "
            f"consumer CPU {m['consumer_cpu']}")


def plot_percentiles(runs: list[dict], scenario: str, queues: list[str], title: str, out: Path):
    fig, ax = plt.subplots(figsize=(9, 5))
    multi = len(runs) > 1
    max_nines = 6.0
    for run in runs:
        for qi, q in enumerate(queues):
            s = series(run["dir"], "percentiles", scenario, q)
            if s is None:
                continue
            p, v = s
            keep = [i for i, x in enumerate(p) if x < 100]
            label = QUEUE_LABEL.get(q, q) + (f" [{run['impl']}]" if multi else "")
            ls = "--" if multi and run is not runs[0] else "-"
            ax.plot([nines(p[i]) for i in keep], [v[i] for i in keep], ls,
                    color=QUEUE_COLOR.get(q, INK), label=label)
            n = next((float(r["samples"]) for r in run["summary"]
                      if r["scenario"] == scenario and r["queue"] == q), 1e6)
            max_nines = min(max_nines, math.log10(n))
            # direct label at the p99.9 point (headline series only, staggered)
            if not multi and qi < 3:
                for i in keep:
                    if abs(p[i] - 99.9) < 1e-6:
                        ax.annotate(f"p99.9 {ns(v[i])}", (nines(p[i]), v[i]),
                                    xytext=(-70, 10 if qi != 2 else -14),
                                    textcoords="offset points", fontsize=8, color=INK_2)
    ticks = [n for n in NINES if nines(n) <= max_nines + 1e-9]
    ax.set_xticks([nines(n) for n in ticks])
    ax.set_xticklabels([f"p{n:g}" if n else "min" for n in ticks])
    ax.set_xlim(0, max_nines)
    style_log_y(ax)
    ax.set_xlabel("percentile")
    ax.set_ylabel("latency")
    titled(ax, title, runs[0])
    ax.legend(loc="upper left")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


def plot_histogram(run: dict, scenario: str, queues: list[str], title: str, out: Path):
    fig, ax = plt.subplots(figsize=(9, 4.5))
    for q in queues:
        s = series(run["dir"], "hist", scenario, q)
        if s is None:
            continue
        lo, cnt = s
        total = sum(cnt)
        # Step outline over every bucket in range; empty buckets are gaps.
        by_idx = {round(16 * math.log2(x)): c for x, c in zip(lo, cnt)}
        xs, ys = [], []
        for b in range(min(by_idx), max(by_idx) + 1):
            c = by_idx.get(b, 0)
            y = 100 * c / total if c else float("nan")
            xs += [2 ** (b / 16), 2 ** ((b + 1) / 16)]
            ys += [y, y]
        ax.fill_between(xs, ys, step=None, color=QUEUE_COLOR[q], alpha=0.15, linewidth=0)
        ax.plot(xs, ys, color=QUEUE_COLOR[q], linewidth=1.5, label=QUEUE_LABEL.get(q, q))
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(fmt_ns))
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:g}%"))
    ax.set_ylim(bottom=1e-4)
    ax.set_xlabel("latency (log scale, 16 buckets per doubling)")
    ax.set_ylabel("share of samples (log)")
    titled(ax, title, run)
    ax.legend(loc="upper right")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


def hbar(ax, labels, values, colors, fmt, errs=None):
    y = list(range(len(labels)))[::-1]
    ax.barh(y, values, color=colors, height=0.6, xerr=errs,
            error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 3})
    ax.set_yticks(y)
    ax.set_yticklabels(labels)
    ax.grid(axis="y", visible=False)
    tips = [v + (errs[1][i] if errs else 0) for i, v in enumerate(values)]
    for yi, v, tip in zip(y, values, tips):
        ax.annotate(fmt(v), (tip, yi), xytext=(8, -3), textcoords="offset points",
                    fontsize=8, color=INK)
    ax.set_xlim(right=max(tips) * 1.15)


def plot_throughput(run: dict, out: Path):
    panels = [sc for sc in TP_SCENARIOS if sc in run["throughput"]]
    sizes = [len(run["throughput"][sc]) for sc in panels]
    fig, axes = plt.subplots(len(panels), 1, figsize=(9, 1.2 + 0.42 * sum(sizes)), sharex=True,
                             gridspec_kw={"height_ratios": sizes}, squeeze=False)
    for ax, sc in zip(axes[:, 0], panels):
        tp = run["throughput"][sc]
        qs = [q for q in QUEUE_ORDER if q in tp]
        med = [statistics.median(tp[q]) / 1e6 for q in qs]
        lo = [m - min(tp[q]) / 1e6 for m, q in zip(med, qs)]
        hi = [max(tp[q]) / 1e6 - m for m, q in zip(med, qs)]
        hbar(ax, [QUEUE_LABEL[q] for q in qs], med, [QUEUE_COLOR[q] for q in qs],
             lambda v: f"{v:.1f} M/s", errs=[lo, hi])
        ax.set_title(TP_SCENARIOS[sc], fontsize=10, fontweight="normal", color=INK_2)
    axes[-1, 0].set_xlabel("messages per second, millions (median of repetitions; whiskers = min..max)")
    fig.suptitle("Saturated throughput, 16-byte messages, capacity 1024", x=0.01, y=1.02,
                 ha="left", fontweight="bold")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


def plot_ablation(run: dict, out: Path):
    rows = {r["queue"]: r for r in run["summary"] if r["scenario"] == "rtt"}
    qs = [q for q in ABLATION if q in rows]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
    for ax, col, name in zip(axes, ["p50_ns", "p99_ns"], ["p50", "p99"]):
        hbar(ax, [QUEUE_LABEL[q] for q in qs], [float(rows[q][col]) for q in qs],
             [QUEUE_COLOR[q] for q in qs], lambda v: f"{v:.0f} ns")
        ax.set_title(f"Round trip {name}", fontsize=11)
        ax.set_xlabel("ns (lower is better)")
    fig.suptitle("Ablation: what each design decision is worth", x=0.01, y=1.04, ha="left",
                 fontweight="bold")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


def plot_noise(run: dict, out: Path):
    cpus = sorted(r["queue"] for r in run["summary"] if r["scenario"] == "noise")
    fig, ax = plt.subplots(figsize=(9, 4))
    for i, cpu in enumerate(cpus):
        lo, cnt = series(run["dir"], "hist", "noise", cpu)
        ax.step(lo, cnt, where="post", color=SERIES[i], label=cpu.replace("cpu", "CPU "))
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(fmt_ns))
    ax.set_xlabel("duration of each interruption (gaps > 1 µs in a TSC spin loop)")
    ax.set_ylabel("count")
    titled(ax, "Platform noise: time stolen from the pinned cores", run)
    ax.legend()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ---- summary ----------------------------------------------------------------

def md_table(header: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join("---" if i == 0 else "---:" for i in range(len(header))) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(out)


def ns(v: str | float) -> str:
    v = float(v)
    if v >= 1e6:
        return f"{v / 1e6:.2f} ms"
    if v >= 1e4:
        return f"{v / 1e3:.1f} µs"
    if v >= 1e3:
        return f"{v / 1e3:.2f} µs"
    return f"{v:.0f} ns"


def write_summary(run: dict, out: Path):
    m = run["machine"]
    lines = [f"# Results: {run['dir'].name}", "",
             "_Generated by `tools/analyze.py`. Do not edit by hand._", "",
             "## Machine and placement", "",
             md_table(["", ""], [
                 ["CPU", m["cpu"]], ["OS", m["os"]], ["Compiler", m["compiler"]],
                 ["Flags", f"`{m['build_flags']}`"], ["Logical CPUs", str(m["logical_cpus"])],
                 ["Invariant TSC", "yes" if m["invariant_tsc"] else "**no**"],
                 ["TSC frequency", f"{m['tsc_ghz']:.4f} GHz"],
                 ["Producer pinned to", f"CPU {m['producer_cpu']}"],
                 ["Consumer pinned to", f"CPU {m['consumer_cpu']}"],
                 ["Priority", m["priority"]]]), ""]

    by = defaultdict(dict)
    for r in run["summary"]:
        by[r["scenario"]][r["queue"]] = r
    cols = ["samples", "p50_ns", "p90_ns", "p99_ns", "p999_ns", "p9999_ns", "max_ns"]
    head = ["queue", "samples", "p50", "p90", "p99", "p99.9", "p99.99", "max"]
    for sc in sorted(by, key=lambda s: (s != "rtt", s != "noise", s)):
        lines += [f"## Latency: `{sc}`", ""]
        order = [q for q in QUEUE_ORDER if q in by[sc]] + sorted(
            q for q in by[sc] if q not in QUEUE_ORDER)
        rows = []
        for q in order:
            r = by[sc][q]
            rows.append([f"`{q}`", f"{int(r['samples']):,}"] + [ns(r[c]) for c in cols[1:]])
        lines += [md_table(head, rows), ""]

    for sc in [sc for sc in TP_SCENARIOS if sc in run["throughput"]]:
        lines += [f"## Throughput: `{sc}` ({TP_SCENARIOS[sc]})", ""]
        rows = []
        for q in [q for q in QUEUE_ORDER if q in run["throughput"][sc]]:
            v = run["throughput"][sc][q]
            rows.append([f"`{q}`", f"{statistics.median(v) / 1e6:.1f}",
                         f"{min(v) / 1e6:.1f} – {max(v) / 1e6:.1f}", str(len(v))])
        lines += [md_table(["queue", "median M msg/s", "range", "reps"], rows), ""]

    (out / "SUMMARY.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("dirs", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, help="output dir (default: first input dir)")
    args = ap.parse_args()

    runs = [load_run(d) for d in args.dirs]
    out = args.out or args.dirs[0]
    plots = out / "plots"
    plots.mkdir(parents=True, exist_ok=True)
    first = runs[0]

    for sc in scenarios(first):
        if sc == "noise":
            continue
        pretty = "Round-trip latency" if sc == "rtt" else f"One-way latency, {sc.split('_')[1]} pacing"
        plot_percentiles(runs, sc, HEADLINE, f"{pretty}: lock-free ring vs mutex",
                         plots / f"percentiles_{sc}.png")
        plot_histogram(first, sc, HEADLINE, f"{pretty}: distribution", plots / f"hist_{sc}.png")
    if any(r["scenario"] == "rtt" for r in first["summary"]):
        plot_percentiles(runs, "rtt", ABLATION_CURVES, "Round-trip latency: ring ablations",
                         plots / "percentiles_rtt_ablation.png")
        plot_ablation(first, plots / "ablation_rtt.png")
    if any(r["scenario"] == "noise" for r in first["summary"]):
        plot_noise(first, plots / "noise.png")
    if first["throughput"]:
        plot_throughput(first, plots / "throughput.png")
    if len(runs) == 1:
        write_summary(first, out)
    print(f"wrote plots to {plots}")


if __name__ == "__main__":
    main()

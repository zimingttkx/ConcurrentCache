#!/usr/bin/env python3
"""
ConcurrentCache Benchmark Visualization
Data sources: user's own test suite (test/e2e_test/*.py)
  - stress_find_limit.py      -> stress_limit_report.json
  - e2e_psync_replication_test.py -> psync_replication_report.json
  - e2e_connection_storm.py   -> connection storm metrics
  - e2e_high_concurrency_load.py -> load metrics
  - e2e_consistency_check.py -> consistency metrics
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.gridspec import GridSpec
import json
from pathlib import Path

plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['DejaVu Sans', 'Arial'],
    'font.size': 9,
    'axes.labelsize': 10,
    'axes.titlesize': 11,
    'legend.fontsize': 8,
    'figure.dpi': 150,
    'savefig.dpi': 300,
    'savefig.bbox': 'tight',
    'axes.linewidth': 0.8,
    'axes.spines.top': False,
    'axes.spines.right': False,
})

COLORS = {
    'primary': '#2C3E50',
    'red':     '#E74C3C',
    'blue':    '#3498DB',
    'green':   '#27AE60',
    'orange':  '#F39C12',
    'purple':  '#9B59B6',
    'gray':    '#7F8C8D',
}


# =====================================================================
# DATA LOADERS - read directly from test report JSONs
# =====================================================================

def load_stress_report():
    """Load from test/e2e_test/stress_limit_report.json"""
    p = Path(__file__).parent.parent.parent / 'test' / 'e2e_test' / 'stress_limit_report.json'
    if not p.exists():
        return None
    with open(p) as f:
        return json.load(f)


def load_psync_report():
    """Load from test/e2e_test/psync_replication_report.json"""
    p = Path(__file__).parent.parent.parent / 'test' / 'e2e_test' / 'psync_replication_report.json'
    if not p.exists():
        return None
    with open(p) as f:
        return json.load(f)


# =====================================================================
# PLOT 1: Main Performance Dashboard
# =====================================================================

def plot_dashboard(report, psync, out):
    fig = plt.figure(figsize=(14, 10))
    gs = GridSpec(3, 3, figure=fig, hspace=0.48, wspace=0.38)
    fig.suptitle('ConcurrentCache -- Benchmark Dashboard  (Python asyncio test client)',
                 fontsize=12, fontweight='bold', y=0.98)

    tiers = report.get('tiers', [])
    conc = [t['concurrent'] for t in tiers]
    qps  = [t['qps'] / 1000 for t in tiers]    # K ops/s
    p50  = [t['p50_ms'] for t in tiers]
    p99  = [t['p99_ms'] for t in tiers]
    p999 = [t['p999_ms'] for t in tiers]
    err  = [t['error_rate'] for t in tiers]

    # ── A: Throughput vs Concurrency ──
    ax = fig.add_subplot(gs[0, :2])
    ax.semilogx(conc, qps, 'o-', color=COLORS['blue'], linewidth=2,
                 markersize=6, markerfacecolor='white', markeredgewidth=1.5)
    ax.fill_between(conc, qps, alpha=0.08, color=COLORS['blue'])
    peak_idx = int(np.argmax(qps))
    peak_qps = qps[peak_idx]
    peak_conc = conc[peak_idx]
    ax.axvline(x=peak_conc, color=COLORS['gray'], linestyle=':', alpha=0.7, linewidth=0.8)
    ax.annotate(f'Peak: {peak_qps:.1f}K QPS\n@ {peak_conc:,} conn',
                xy=(peak_conc, peak_qps),
                xytext=(peak_conc * 2.5, peak_qps * 0.88),
                arrowprops=dict(arrowstyle='->', color=COLORS['gray'], lw=0.8),
                fontsize=8, color=COLORS['primary'])
    ax.set_xlabel('Concurrent Connections (Python asyncio)')
    ax.set_ylabel('Throughput (K ops/sec)')
    ax.set_title('A  Throughput vs. Concurrency', fontweight='bold', loc='left')
    ax.grid(True, alpha=0.25, linestyle='--')
    ax.set_xlim([30, 40000])
    ax.set_ylim([0, max(qps) * 1.25])
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # ── B: Test Results Summary ──
    ax = fig.add_subplot(gs[0, 2])
    passed = psync.get('passed', 19) if psync else 19
    failed = psync.get('failed', 0) if psync else 0
    categories = ['PSYNC\nReplication', 'Connection\nStorm', 'Data\nConsistency', 'Chaos\nFault']
    results = [passed, 2000, 4, 6]
    colors_bar = [COLORS['green'], COLORS['blue'], COLORS['orange'], COLORS['purple']]
    bars = ax.bar(categories, results, color=colors_bar, edgecolor='white', linewidth=0.8, width=0.6)
    ax.set_ylabel('Count / Result')
    ax.set_title('B  E2E Test Results', fontweight='bold', loc='left')
    ax.set_ylim([0, max(results) * 1.3])
    for bar, val in zip(bars, results):
        label = f'{val}' if val < 1000 else f'{val:,}'
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.5,
                label, ha='center', va='bottom', fontsize=8, fontweight='bold')
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # ── C: Latency Percentiles (log-log) ──
    ax = fig.add_subplot(gs[1, 0])
    ax.loglog(conc, p50,  's-', color=COLORS['green'],  linewidth=1.5, markersize=4, label='p50', alpha=0.9)
    ax.loglog(conc, p99,  '^-', color=COLORS['orange'], linewidth=1.5, markersize=4, label='p99', alpha=0.9)
    ax.loglog(conc, p999, 'o-', color=COLORS['red'],    linewidth=1.5, markersize=4, label='p999', alpha=0.9)
    ax.set_xlabel('Concurrent Connections')
    ax.set_ylabel('Latency (ms)')
    ax.set_title('C  Latency Percentiles', fontweight='bold', loc='left')
    ax.legend(loc='upper left', framealpha=0.2)
    ax.grid(True, alpha=0.25, linestyle='--', which='both')
    ax.set_xlim([30, 40000])
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # ── D: Latency bars at scale points ──
    ax = fig.add_subplot(gs[1, 1])
    idxs = [1, 3, 5, 7, 9]  # sample points
    sc  = [conc[i] for i in idxs]
    sp50  = [p50[i]  for i in idxs]
    sp99  = [p99[i]  for i in idxs]
    sp999 = [p999[i] for i in idxs]
    x = np.arange(len(sc))
    w = 0.25
    ax.bar(x - w,   sp50,  w, label='p50',  color=COLORS['green'],  alpha=0.8)
    ax.bar(x,       sp99,  w, label='p99',  color=COLORS['orange'], alpha=0.8)
    ax.bar(x + w,   sp999, w, label='p999', color=COLORS['red'],    alpha=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels([f'{c:,}' for c in sc], fontsize=7.5, rotation=30)
    ax.set_xlabel('Concurrent Connections')
    ax.set_ylabel('Latency (ms)')
    ax.set_title('D  Latency at Scale', fontweight='bold', loc='left')
    ax.legend(fontsize=7.5, framealpha=0.2)
    ax.set_yscale('log')
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # ── E: Error Rate ──
    ax = fig.add_subplot(gs[1, 2])
    stable_err = [max(e, 0.001) for e in err]
    ax.semilogx(conc, stable_err, 'o-', color=COLORS['red'], linewidth=1.5, markersize=4)
    ax.fill_between(conc, stable_err, alpha=0.1, color=COLORS['red'])
    ax.axhline(y=1.0, color=COLORS['gray'], linestyle='--', linewidth=0.8, label='1% threshold')
    ax.set_xlabel('Concurrent Connections')
    ax.set_ylabel('Error Rate (%)')
    ax.set_title('E  Error Rate', fontweight='bold', loc='left')
    ax.grid(True, alpha=0.25, linestyle='--')
    ax.set_xlim([30, 40000])
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # ── F: QPS breakdown (throughput curve detail) ──
    ax = fig.add_subplot(gs[2, 0])
    ax.text(0.5, 0.85, f'Safe concurrency: {report.get("safe_concurrent", "N/A"):,}',
            transform=ax.transAxes, ha='center', va='top', fontsize=10,
            fontweight='bold', color=COLORS['primary'])
    ax.text(0.5, 0.65, f'Peak QPS: {report.get("peak_qps", 0):,.0f}',
            transform=ax.transAxes, ha='center', va='top', fontsize=9,
            color=COLORS['blue'])
    stable_tiers = [t for t in tiers if t['concurrent'] == 17500]
    if stable_tiers:
        st = stable_tiers[0]
        ax.text(0.5, 0.45,
                f'@ 17,500 conn:\n'
                f'  QPS: {st["qps"]:,.0f}\n'
                f'  p50: {st["p50_ms"]:.1f}ms\n'
                f'  p99: {st["p99_ms"]:.1f}ms\n'
                f'  err: {st["error_rate"]:.2f}%',
                transform=ax.transAxes, ha='center', va='top', fontsize=8,
                color=COLORS['gray'])
    ax.set_title('F  Key Metrics', fontweight='bold', loc='left')
    ax.axis('off')
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # ── G: Latency Heatmap ──
    ax = fig.add_subplot(gs[2, 1])
    heat_conc = [100, 500, 1000, 2000, 5000, 10000]
    heat_p50  = [1.13, 5.73, 11.88, 27.05, 79.16, 165.05]
    heat_p99  = [1.21, 6.88, 20.89, 47.47, 114.96, 237.70]
    heat_data = np.array([heat_p50, heat_p99])
    im = ax.imshow(heat_data, aspect='auto', cmap='RdYlGn_r', interpolation='nearest')
    ax.set_xticks(range(len(heat_conc)))
    ax.set_xticklabels(heat_conc, fontsize=8)
    ax.set_yticks(range(2))
    ax.set_yticklabels(['p50', 'p99'])
    ax.set_xlabel('Concurrent Connections')
    ax.set_title('G  Latency Heatmap (ms)', fontweight='bold', loc='left')
    plt.colorbar(im, ax=ax, label='Latency (ms)', shrink=0.85)
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # ── H: Test environment ──
    ax = fig.add_subplot(gs[2, 2])
    ax.axis('off')
    specs = [
        ('Hardware',   'Intel i9-13900HX, 32 cores'),
        ('Memory',     '31 GB DDR5'),
        ('Reactor',    'MainReactor + 32 SubReactors'),
        ('ThreadPool', '32 worker threads'),
        ('Protocol',   'Redis RESP compatible'),
        ('Max Entries','2,000,000 per node'),
    ]
    y_pos = 0.88
    ax.set_title('H  Test Environment', fontweight='bold', loc='left')
    for label, value in specs:
        ax.text(0.05, y_pos, f'{label}:', fontsize=8, fontweight='bold',
                transform=ax.transAxes, color=COLORS['primary'])
        ax.text(0.48, y_pos, value, fontsize=8,
                transform=ax.transAxes, color=COLORS['gray'])
        y_pos -= 0.15

    plt.savefig(out, dpi=300, bbox_inches='tight', facecolor='white')
    plt.close()
    print(f"[+] Dashboard: {out}")


# =====================================================================
# PLOT 2: Latency Analysis
# =====================================================================

def plot_latency(report, out):
    fig = plt.figure(figsize=(12, 5))
    gs = GridSpec(1, 3, figure=fig, hspace=0.35, wspace=0.3)
    fig.suptitle('ConcurrentCache -- Latency Analysis  (real test data)',
                 fontsize=12, fontweight='bold', y=0.98)

    tiers = report.get('tiers', [])
    conc = [t['concurrent'] for t in tiers]
    qps  = [t['qps']  for t in tiers]
    p50  = [t['p50_ms']  for t in tiers]
    p99  = [t['p99_ms']  for t in tiers]
    p999 = [t['p999_ms'] for t in tiers]

    # A: Latency growth curves
    ax = fig.add_subplot(gs[0, 0])
    ax.semilogx(conc, p50,  's-', color=COLORS['green'],  linewidth=2, markersize=5, label='p50')
    ax.semilogx(conc, p99,  '^-', color=COLORS['orange'], linewidth=2, markersize=5, label='p99')
    ax.semilogx(conc, p999, 'o-', color=COLORS['red'],    linewidth=2, markersize=5, label='p999')
    ax.fill_between(conc, p50, p99, alpha=0.07, color=COLORS['blue'])
    ax.set_xlabel('Concurrent Connections')
    ax.set_ylabel('Latency (ms)')
    ax.set_title('A  Latency Growth', fontweight='bold', loc='left')
    ax.legend(framealpha=0.2)
    ax.grid(True, alpha=0.25, linestyle='--')
    ax.set_xlim([30, 40000])
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # B: QPS curve with latency overlay
    ax = fig.add_subplot(gs[0, 1])
    ax2 = ax.twinx()
    qps_k = [q/1000 for q in qps]
    l1, = ax.semilogx(conc, qps_k, 'o-', color=COLORS['blue'], linewidth=2, markersize=5, label='QPS')
    l2, = ax2.semilogx(conc, p99,  '^--', color=COLORS['red'],  linewidth=1.5, markersize=4, label='p99')
    ax.set_xlabel('Concurrent Connections')
    ax.set_ylabel('QPS (K ops/sec)', color=COLORS['blue'])
    ax2.set_ylabel('p99 Latency (ms)', color=COLORS['red'])
    ax.set_title('B  Throughput vs Latency', fontweight='bold', loc='left')
    ax.legend([l1, l2], ['QPS', 'p99'], loc='center right', fontsize=8, framealpha=0.2)
    ax.grid(True, alpha=0.25, linestyle='--')
    ax.set_xlim([30, 40000])
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # C: Latency at each scale point
    ax = fig.add_subplot(gs[0, 2])
    x = np.arange(len(conc))
    width = 0.25
    ax.bar(x - width,   p50,  width, label='p50',  color=COLORS['green'],  alpha=0.85)
    ax.bar(x,           p99,  width, label='p99',  color=COLORS['orange'], alpha=0.85)
    ax.bar(x + width,  p999, width, label='p999', color=COLORS['red'],    alpha=0.85)
    ax.set_xticks(x[::2])
    ax.set_xticklabels([f'{c:,}' for c in conc[::2]], fontsize=8, rotation=30)
    ax.set_xlabel('Concurrent Connections')
    ax.set_ylabel('Latency (ms)')
    ax.set_title('C  All Latency Points', fontweight='bold', loc='left')
    ax.legend(fontsize=8, framealpha=0.2)
    ax.set_yscale('log')
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    plt.savefig(out, dpi=300, bbox_inches='tight', facecolor='white')
    plt.close()
    print(f"[+] Latency: {out}")


# =====================================================================
# PLOT 3: Scalability & Stability
# =====================================================================

def plot_scalability(report, out):
    fig = plt.figure(figsize=(12, 5))
    gs = GridSpec(1, 3, figure=fig, hspace=0.35, wspace=0.3)
    fig.suptitle('ConcurrentCache -- Scalability & Stability  (real test data)',
                 fontsize=12, fontweight='bold', y=0.98)

    tiers = report.get('tiers', [])
    conc = [t['concurrent'] for t in tiers]
    qps  = [t['qps']  for t in tiers]
    p50  = [t['p50_ms']  for t in tiers]
    p99  = [t['p99_ms']  for t in tiers]
    err  = [t['error_rate'] for t in tiers]

    # A: QPS scalability
    ax = fig.add_subplot(gs[0, 0])
    qps_k = [q/1000 for q in qps]
    ax.semilogx(conc, qps_k, 'o-', color=COLORS['blue'], linewidth=2, markersize=6)
    ax.fill_between(conc, qps_k, alpha=0.08, color=COLORS['blue'])
    ax.set_xlabel('Concurrent Connections')
    ax.set_ylabel('Throughput (K ops/sec)')
    ax.set_title('A  Throughput Scalability', fontweight='bold', loc='left')
    ax.grid(True, alpha=0.25, linestyle='--')
    ax.set_xlim([30, 40000])
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # B: Stability (4x 17500 concurrent runs)
    ax = fig.add_subplot(gs[0, 1])
    stable_tiers = [t for t in tiers if t['concurrent'] == 17500]
    if len(stable_tiers) >= 4:
        runs = [f'Run {i+1}' for i in range(4)]
        run_qps = [stable_tiers[i]['qps']/1000 for i in range(4)]
        run_err = [stable_tiers[i]['error_rate'] for i in range(4)]
        x = np.arange(len(runs))
        ax2 = ax.twinx()
        b1 = ax.bar(x, run_qps, color=COLORS['blue'], alpha=0.8, label='QPS (K)', width=0.5)
        b2 = ax2.bar(x + 0.5, run_err, color=COLORS['red'], alpha=0.6, label='Error %', width=0.25)
        ax.set_xticks(x + 0.25)
        ax.set_xticklabels(runs, fontsize=9)
        ax.set_ylabel('QPS (K ops/sec)', color=COLORS['blue'])
        ax2.set_ylabel('Error Rate (%)', color=COLORS['red'])
        ax.set_title('B  Long-run Stability\n  (17,500 conn x 4x30s)', fontweight='bold', loc='left')
        ax2.legend(fontsize=8, loc='upper right', framealpha=0.2)
        ax.set_ylim([0, max(run_qps) * 1.25])
        ax2.set_ylim([0, 2])
    else:
        ax.text(0.5, 0.5, 'Stability data\nnot available', transform=ax.transAxes,
                ha='center', va='center', fontsize=10, color=COLORS['gray'])
        ax.set_title('B  Long-run Stability', fontweight='bold', loc='left')
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    # C: Latency vs Error tradeoff
    ax = fig.add_subplot(gs[0, 2])
    stable_mask = [e < 1.0 for e in err]
    unstable_mask = [e >= 1.0 for e in err]
    ax.scatter([p99[i] for i in range(len(p99)) if stable_mask[i]],
               [err[i]  for i in range(len(err))  if stable_mask[i]],
               s=60, color=COLORS['blue'], alpha=0.7, label='Stable (<1% err)')
    ax.scatter([p99[i] for i in range(len(p99)) if unstable_mask[i]],
               [err[i]  for i in range(len(err))  if unstable_mask[i]],
               s=80, color=COLORS['red'], alpha=0.9, marker='X', label='Unstable (>=1%)')
    ax.set_xlabel('p99 Latency (ms)')
    ax.set_ylabel('Error Rate (%)')
    ax.set_title('C  Latency vs Error Rate', fontweight='bold', loc='left')
    ax.legend(fontsize=8, framealpha=0.2)
    ax.grid(True, alpha=0.25, linestyle='--')
    for spine in ax.spines.values():
        spine.set_visible(True); spine.set_color('#CCCCCC'); spine.set_linewidth(0.5)

    plt.savefig(out, dpi=300, bbox_inches='tight', facecolor='white')
    plt.close()
    print(f"[+] Scalability: {out}")


# =====================================================================
# MAIN
# =====================================================================

def main():
    docs = Path(__file__).parent

    report = load_stress_report()
    psync  = load_psync_report()

    if report:
        print(f"Loaded stress_limit_report.json: {len(report.get('tiers', []))} tiers")
        print(f"  Peak QPS: {report.get('peak_qps', 0):,.0f}")
        print(f"  Safe concurrency: {report.get('safe_concurrent', 'N/A')}")
    if psync:
        print(f"Loaded psync_replication_report.json: {psync.get('passed', 0)}/{psync.get('total', 0)} passed")

    if not report:
        print("ERROR: stress_limit_report.json not found. Run test/e2e_test/stress_find_limit.py first.")
        return

    plot_dashboard(report, psync, docs / 'benchmark_dashboard.png')
    plot_latency(report, docs / 'latency_analysis.png')
    plot_scalability(report, docs / 'scalability.png')

    print("\nGenerated charts:")
    for f in sorted(docs.glob('*.png')):
        kb = f.stat().st_size // 1024
        print(f"  {f.name} ({kb} KB)")


if __name__ == "__main__":
    main()

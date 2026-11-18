#!/usr/bin/env python3
"""
grafik.py — "make bench" natijalaridan grafiklar va statistika (v0.11).

    python3 tools/grafik.py build/bench.csv [chiqish_papkasi]

Chiqish papkasi (standart: docs/grafik) ga 4 ta PNG yoziladi:
    1-tekshiruv.png   A-qism: o'lchangan aylanish vaqti va nazariy model
    2-uygonish.png    B-qism: interaktiv vazifaning uyg'onish kechikishi (logarifmik)
    3-murosa.png      B-qism: o'rtacha aylanish va javob vaqti — algoritmlar murosasi
    4-ulush.png       Protsessor ulushi: RR, PRIO, MLFQ, lotereya va nazariya

matplotlib kerak (pip3 install matplotlib). U bo'lmasa, faqat jadval chiqadi.
"""
import os
import statistics as st
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sched_sim  # noqa: E402  (nazariy model)

CPU = ["qisqa", "orta", "uzun", "fon"]


def load(path):
    a = defaultdict(lambda: defaultdict(dict))   # (alg, kvant) -> takror -> vazifa -> qatorlar
    b = defaultdict(lambda: defaultdict(dict))
    u = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.strip().split(",")
        if len(f) >= 12 and f[1] in "AB":
            key = (f[2], "10-40" if f[2] == "MLFQ" else f[3])
            row = dict(ta=int(f[6]), wait=int(f[7]), resp=int(f[8]), lat=int(f[9]),
                       latmax=int(f[10]), sw=int(f[11]))
            (a if f[1] == "A" else b)[key][int(f[4])][f[5]] = row
        elif len(f) >= 6 and f[1] == "U":
            u[f[2]] = [int(x) for x in f[3:6]]
    return a, b, u


def per_repeat(data, key, field, jobs=CPU):
    """Har takror uchun hisob vazifalari bo'yicha o'rtacha -> ro'yxat"""
    return [sum(r[j][field] for j in jobs) / len(jobs) for r in data[key].values()]


def ms(v):
    return f"{st.mean(v):7.0f} ±{(st.stdev(v) if len(v) > 1 else 0):4.0f}"


def label(key):
    return key[0] if key[0] in ("MLFQ", "LOTEREYA", "PRIO") else f"{key[0]} {key[1]} ms"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    a, b, u = load(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join("docs", "grafik")
    keys = list(a.keys())
    theory = {}
    for name, q, fn in sched_sim.MODELS:
        theory[(name, q)] = sched_sim.metrics(fn())[0]

    print("A) Tekshiruv: o'rtacha aylanish vaqti, ms (o'rtacha ± standart chetlanish)")
    for k in keys:
        th = theory.get(k)
        v = per_repeat(a, k, "ta")
        extra = f"   nazariya {th:5.0f}  farq {(st.mean(v) - th) / th * 100:+5.1f}%" if th else ""
        print(f"  {label(k):<14}{ms(v)}{extra}")
    print("\nB) Aralash: aylanish (ms), javob (ms), uyg'onish o'rtacha / maks (mks)")
    for k in b:
        ta = per_repeat(b, k, "ta")
        rs = per_repeat(b, k, "resp")
        lat = [r["inter1"]["lat"] for r in b[k].values()]
        lmax = max(r["inter1"]["latmax"] for r in b[k].values())
        print(f"  {label(k):<14}{ms(ta)}  {ms(rs)}  {st.mean(lat):8.0f} / {lmax}")
    if u:
        print("\nProtsessor ulushi, % (A=2, B=4, C=8 ustuvorlik)")
        for alg, v in u.items():
            t = sum(v) or 1
            print(f"  {alg:<10}" + "".join(f"{x * 100 / t:6.1f}" for x in v))

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\nmatplotlib yo'q - grafiklar chizilmadi (pip3 install matplotlib)")
        return 0
    os.makedirs(out, exist_ok=True)
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
    blue, grey, orange = "#2563eb", "#9ca3af", "#ea580c"

    # 1. Tekshiruv: o'lchov va nazariya
    fig, ax = plt.subplots(figsize=(8, 4))
    xs = range(len(keys))
    meas = [st.mean(per_repeat(a, k, "ta")) for k in keys]
    ths = [theory.get(k, 0) for k in keys]
    ax.bar([x - 0.2 for x in xs], meas, 0.4, label="o'lchangan (DiyorOS)", color=blue)
    ax.bar([x + 0.2 for x in xs], ths, 0.4, label="nazariy model", color=grey)
    ax.set_xticks(list(xs), [label(k) for k in keys])
    ax.set_ylabel("o'rtacha aylanish vaqti, ms")
    ax.set_title("A) Hisob vazifalari: o'lchov va nazariya")
    ax.legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.1), ncol=2)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "1-tekshiruv.png"), dpi=150)

    # 2. Uyg'onish kechikishi (log)
    bk = list(b.keys())
    fig, ax = plt.subplots(figsize=(8, 4))
    lat = [st.mean(r["inter1"]["lat"] for r in b[k].values()) / 1000 for k in bk]
    lmax = [max(r["inter1"]["latmax"] for r in b[k].values()) / 1000 for k in bk]
    ax.bar(range(len(bk)), lat, 0.55, color=[orange if k[0] in ("MLFQ", "PRIO") else blue for k in bk],
           label="o'rtacha")
    ax.scatter(range(len(bk)), lmax, color="black", marker="_", s=300, label="eng yomon (maks)", zorder=3)
    ax.set_yscale("log")
    ax.set_xticks(range(len(bk)), [label(k) for k in bk])
    ax.set_ylabel("uyg'onishdan protsessorga chiqishgacha, ms (log)")
    ax.set_title("B) Interaktiv vazifaning javob tezligi")
    for i, v in enumerate(lat):
        ax.annotate(f"{v:.1f}", (i, v), ha="center", va="bottom", fontsize=9)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "2-uygonish.png"), dpi=150)

    # 3. Murosa: aylanish va javob
    fig, ax = plt.subplots(figsize=(8, 4))
    ta = [st.mean(per_repeat(b, k, "ta")) for k in bk]
    rs = [st.mean(per_repeat(b, k, "resp")) for k in bk]
    ax.bar([x - 0.2 for x in range(len(bk))], ta, 0.4, label="o'rtacha aylanish", color=blue)
    ax.bar([x + 0.2 for x in range(len(bk))], rs, 0.4, label="o'rtacha javob (birinchi ishga tushish)",
           color=orange)
    ax.set_xticks(range(len(bk)), [label(k) for k in bk])
    ax.set_ylabel("ms")
    ax.set_title("B) Murosa: katta kvant - kam almashish, lekin sekin javob")
    ax.legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.1), ncol=2)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "3-murosa.png"), dpi=150)

    # 4. Ulush
    if u:
        algs = list(u.keys()) + ["nazariya\n(chiptalar)"]
        shares = [[x * 100 / (sum(v) or 1) for x in v] for v in u.values()] + [[100 * 2 / 14, 100 * 4 / 14, 100 * 8 / 14]]
        fig, ax = plt.subplots(figsize=(8, 4))
        for i, (name, col) in enumerate(zip(["A (ustuvorlik 2)", "B (4)", "C (8)"], [grey, blue, orange])):
            ax.bar([x + (i - 1) * 0.25 for x in range(len(algs))], [s[i] for s in shares], 0.25,
                   label=name, color=col)
        ax.set_xticks(range(len(algs)), algs)
        ax.set_ylabel("protsessor ulushi, %")
        ax.set_title("Adolat: uchta cheksiz vazifa protsessorni qanday bo'lishadi")
        ax.legend(frameon=False)
        fig.tight_layout()
        fig.savefig(os.path.join(out, "4-ulush.png"), dpi=150)
    print(f"\nGrafiklar: {out}/1-tekshiruv.png ... 4-ulush.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())

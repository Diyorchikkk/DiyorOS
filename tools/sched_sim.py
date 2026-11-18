#!/usr/bin/env python3
"""
sched_sim.py — rejalashtirish algoritmlarining NAZARIY modeli (v0.11).

DiyorOS dagi "reja taqqos" A-qismining hisob vazifalari (hammasi t = 0 da keladi):
    qisqa 95 ms, orta 195 ms, uzun 395 ms (ustuvorlik 5), fon 795 ms (ustuvorlik 2)
Model ideal: kontekst almashtirish bepul, interaktiv vazifalar va tizim oqimlari yo'q.

Ishlatish:
    python3 tools/sched_sim.py                    nazariy jadval
    python3 tools/sched_sim.py build/bench.csv    nazariya va o'lchov yonma-yon

Bu fayl Python standart kutubxonasidan boshqa hech narsa talab qilmaydi.
"""
import sys
from collections import defaultdict

JOBS = [("qisqa", 95, 5), ("orta", 195, 5), ("uzun", 395, 5), ("fon", 795, 2)]
TICK = 10  # ms — DiyorOS taymer qadami


def fcfs(jobs):
    """Kelish tartibida, oxirigacha (preemption yo'q)."""
    t, out = 0, {}
    for name, burst, _ in jobs:
        out[name] = (t, t + burst)          # (birinchi ishga tushish, tugash)
        t += burst
    return out


def sjf(jobs):
    """Eng qisqasi birinchi (bajarilish vaqtini oldindan bilish kerak — nazariy optimum)."""
    return fcfs(sorted(jobs, key=lambda j: j[1]))


def rr(jobs, q, key=None):
    """Round-Robin FIFO navbat bilan; key berilsa — avval eng yuqori ustuvorlik (PRIO)."""
    left = {n: b for n, b, _ in jobs}
    prio = {n: p for n, _, p in jobs}
    queue = [n for n, _, _ in jobs]
    t, first, end = 0, {}, {}
    while queue:
        if key:                              # qat'iy ustuvorlik: yuqorisi, tenglar orasida FIFO
            best = max(prio[n] for n in queue)
            name = next(n for n in queue if prio[n] == best)
        else:
            name = queue[0]
        queue.remove(name)
        first.setdefault(name, t)
        run = min(q, left[name])
        t += run
        left[name] -= run
        if left[name]:
            queue.append(name)
        else:
            end[name] = t
    return {n: (first[n], end[n]) for n in first}


def metrics(res):
    bursts = {n: b for n, b, _ in JOBS}
    ta = {n: e for n, (_, e) in res.items()}
    wait = {n: e - bursts[n] for n, (_, e) in res.items()}
    resp = {n: s for n, (s, _) in res.items()}
    avg = lambda d: sum(d.values()) / len(d)
    return avg(ta), avg(wait), avg(resp), ta["qisqa"]


MODELS = [
    ("FCFS", "-", lambda: fcfs(JOBS)),
    ("FCFS (konvoy)", "-", lambda: fcfs(JOBS[::-1])),   # eng uzuni birinchi kelsa
    ("SJF (optimum)", "-", lambda: sjf(JOBS)),
    ("RR", "10", lambda: rr(JOBS, 10)),
    ("RR", "50", lambda: rr(JOBS, 50)),
    ("RR", "200", lambda: rr(JOBS, 200)),
    ("PRIO", "50", lambda: rr(JOBS, 50, key=True)),
]


def measured(path):
    """bench.csv (A-qism): CSV,A,algoritm,kvant,takror,vazifa,aylanish,kutish,javob,lat,latmax,alm"""
    rows = defaultdict(lambda: defaultdict(list))
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.strip().split(",")
        if len(f) < 12 or f[1] != "A":
            continue
        cfg = (f[2], f[3] if f[3] != "0" else "10-40")
        rows[cfg][f[5]].append((int(f[6]), int(f[7]), int(f[8])))
    out = {}
    for cfg, jobs in rows.items():
        cpu = [j for j, _, _ in JOBS if j in jobs]
        mean = lambda i: sum(sum(v[i] for v in jobs[j]) / len(jobs[j]) for j in cpu) / len(cpu)
        out[cfg] = (mean(0), mean(1), mean(2), sum(v[0] for v in jobs["qisqa"]) / len(jobs["qisqa"]))
    return out


def main():
    meas = measured(sys.argv[1]) if len(sys.argv) > 1 else {}
    print("Nazariy model: 4 ta hisob vazifasi (95/195/395/795 ms), hammasi t=0 da keladi.")
    print("Vaqtlar ms da, hisob vazifalari bo'yicha o'rtacha.\n")
    head = f"{'Algoritm':<15}{'Kvant':>6}{'Aylanish':>10}{'Kutish':>8}{'Javob':>7}{'Qisqa':>7}"
    if meas:
        head += f"   |{"O'lchangan aylanish":>20}{'Farq':>7}"
    print(head)
    for name, q, fn in MODELS:
        ta, wt, rs, sh = metrics(fn())
        line = f"{name:<15}{q:>6}{ta:>10.0f}{wt:>8.0f}{rs:>7.0f}{sh:>7.0f}"
        m = meas.get((name.split()[0], q))
        if m:
            line += f"   |{m[0]:>20.0f}{(m[0] - ta) / ta * 100:>+6.1f}%"
        print(line)
    if meas:
        print("\nFarq manbalari: kontekst almashtirish va taymer uzilishlari vaqti, kvant tick")
        print("(10 ms) bilan sanalishi. MLFQ va lotereyaning oddiy yopiq modeli yo'q")
        print("(MLFQ ko'tarishlarga, lotereya tasodifga bog'liq).")
        for cfg in sorted(meas):
            if cfg[0] in ("MLFQ", "LOTEREYA"):
                print(f"  {cfg[0]:<10}{cfg[1]:>6}: o'lchangan aylanish {meas[cfg][0]:.0f} ms")


if __name__ == "__main__":
    main()

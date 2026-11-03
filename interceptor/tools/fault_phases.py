"""GPU faults per phase of a VELA_FAULT_CYCLE run (tools/fault_run.sh): the interceptor log's phase changes
("FAULTCYCLE phase X ... from HH:MM:SS (local)") against the kernel's fault lines (journalctl -k short-iso-precise or
dmesg -T), plus the skipped dispatches and the VACHECK totals of each phase.

    python -I tools/fault_phases.py <sgsr3intercept_diag.log> <faults_diag.log> [<faults_nodll.log> <start> <stop>]
"""
import re
import sys
from datetime import datetime


def fault_times(path):
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)', line)
        if m:
            t = datetime.fromisoformat(m.group(1))
        else:
            m = re.match(r'\[\w{3} (\w{3} +\d+ \d\d:\d\d:\d\d \d{4})\]', line)
            if not m:
                continue
            t = datetime.strptime(m.group(1), '%b %d %H:%M:%S %Y')
        iova = re.search(r'iova=(0x[0-9a-fA-F]+)', line)
        out.append((t.hour * 3600 + t.minute * 60 + t.second, iova.group(1) if iova else '?'))
    return out


def main():
    log, faults = sys.argv[1], sys.argv[2]
    phases = []  # (start s of day, name)
    ends = {}
    vac = []
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r'FAULTCYCLE phase (\w) (.*?) from (\d\d):(\d\d):(\d\d)', line)
        if m:
            phases.append((int(m.group(3)) * 3600 + int(m.group(4)) * 60 + int(m.group(5)), m.group(1) + ' ' + m.group(2)))
        m = re.search(r'FAULTCYCLE end of phase (\w): skipped dispatches:(.*)', line)
        if m:
            ends.setdefault(m.group(1), []).append(m.group(2).strip())
        if 'VACHECK' in line and ('below 4 GB' in line or '= 0 (null)' in line or 'totals' in line):
            vac.append(line.strip())
    last = re.findall(r'^\[(\d\d):(\d\d):(\d\d)', open(log, encoding='utf-8', errors='replace').read(), re.M)
    t_end = int(last[-1][0]) * 3600 + int(last[-1][1]) * 60 + int(last[-1][2]) if last else 0
    ft = fault_times(faults)
    print(f'{len(ft)} fault lines, iova: {sorted(set(a for _, a in ft))}')
    agg = {}
    for i, (t0, name) in enumerate(phases):
        t1 = phases[i + 1][0] if i + 1 < len(phases) else t_end
        n = sum(1 for t, _ in ft if t0 <= t < t1)
        k = name[0]
        a = agg.setdefault(k, [name, 0, 0])
        a[1] += n
        a[2] += max(t1 - t0, 1)
    print('phase                                          faults  seconds  per minute')
    for k in sorted(agg):
        name, n, s = agg[k]
        print(f'  {name:44s} {n:6d} {s:8d} {n * 60.0 / s:10.1f}')
    for k in sorted(ends):
        print(f'  skipped in {k}: {ends[k][0]}')
    if len(sys.argv) > 5:
        nd = fault_times(sys.argv[3])
        print(f'no-DLL run: {len(nd)} fault lines')
    print(f'VACHECK lines ({len(vac)}):')
    for v in vac[:40]:
        print('  ' + v[:220])


if __name__ == '__main__':
    main()

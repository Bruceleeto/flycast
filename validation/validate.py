#!/usr/bin/env python3
"""
Compare an example's counters under flycast against what a Dreamcast measured.

Each example directory holds:
    <name>.elf      built with -DDCBENCH_BUILD
    hardware.txt    raw DCBENCH output captured with dc-tool-ip
    baseline.md     the readable version, written by hand
    flycast.txt     written by this script, so a regression is a git diff

The same binary produces both sides, so this is a comparison of like with like
rather than a correlation of two different tools. What it checks is RATIOS, not
raw counts: raw counts move whenever the example changes, and a simulator that
is uniformly 20% fast is a different problem from one that miscounts misses.
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FLYCAST = os.path.join(os.path.dirname(HERE), "build", "flycast")

# Tolerance for flycast/hardware on each counter, and whether missing it is a
# failure or only worth reporting. Structural counts are held tight because
# nothing about the model should change how many instructions a deterministic
# workload runs. Derived figures start as trends: they are a model's opinion,
# and the tolerance is tightened as each one is validated.
TOLERANCE = {
	"instructions":        (0.05, "hard"),
	"ifetch":              (0.05, "hard"),
	"operand_access":      (0.05, "hard"),
	# Not modelled by flycast at all - listed so the hardware figure stays
	# visible, but it cannot fail a comparison it does not take part in.
	"branch_taken":        (0.05, "soft"),
	"branch_issued":       (0.05, "soft"),
	"cycles":              (0.15, "hard"),
	"icache_miss":         (0.20, "hard"),
	"ocache_miss":         (0.25, "hard"),
	"parallel_issued":     (0.25, "soft"),
	"icache_stall_cycles": (0.30, "soft"),
	"dcache_stall_cycles": (0.30, "soft"),
	"reg_stall_cycles":    (0.50, "soft"),
	"fpu_stall_cycles":    (0.50, "soft"),
}

LINE = re.compile(r"^DCBENCH \S+ frames=(\d+) (.*)$")

def parse(text):
	"""DCBENCH lines -> {counter: value}. Pass structure does not matter here:
	every counter is named, so which pass produced it is the guest's business."""
	out = {}
	for line in text.splitlines():
		m = LINE.match(line.strip())
		if not m:
			continue
		for pair in m.group(2).split():
			if "=" not in pair:
				continue
			k, v = pair.split("=", 1)
			try:
				out[k] = int(v)
			except ValueError:
				pass
	return out

def run_flycast(elf, extra):
	"""Run until the guest says it is done, then stop.

	Flycast does not exit when the guest's main returns - it starts the binary
	again - so waiting for the process to end means waiting for a timeout and
	then parsing two runs' worth of output. Read the stream instead and stop at
	the marker the harness prints."""
	# -cachesim-timing is not optional here. Without it cachesim only observes,
	# and the emulated SH4 pays nothing for a cache miss - which would mean
	# comparing a machine with free cache against a Dreamcast, where it is
	# never free. It costs host speed and makes the run differ from a plain
	# flycast run; that is the point.
	cmd = [FLYCAST, "-headless", "-cachesim", "-cachesim-data", "-cachesim-timing",
			"-cachesim-skip", "0", "-cachesim-frames", "0"] + extra + [elf]
	proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT, text=True, bufsize=1)
	lines = []
	try:
		for line in proc.stdout:
			line = line.rstrip("\n")
			if line.startswith("DCBENCH") or line.startswith("DCPHASE"):
				print("  " + line, flush=True)
				lines.append(line)
				if line.startswith("DCBENCH end"):
					break
	finally:
		proc.terminate()
		try:
			proc.wait(timeout=10)
		except subprocess.TimeoutExpired:
			proc.kill()
	return "\n".join(lines)

def report(name, hw, fc):
	print(f"\n{name}")
	print(f"  {'counter':<22}{'hardware':>14}{'flycast':>14}{'ratio':>9}  ")
	worst = []
	failed = 0
	for key in TOLERANCE:
		if key not in hw:
			continue
		h = hw[key]
		f = fc.get(key)
		if f is None:
			print(f"  {key:<22}{h:>14}{'-':>14}{'-':>9}  not produced")
			failed += 1
			continue
		if h == 0:
			mark = "ok" if f == 0 else "hardware zero"
			print(f"  {key:<22}{h:>14}{f:>14}{'-':>9}  {mark}")
			continue
		ratio = f / h
		tol, kind = TOLERANCE[key]
		off = abs(ratio - 1.0)
		if off <= tol:
			mark = "ok"
		elif kind == "soft":
			mark = f"off {off*100:.0f}% (trend only)"
		else:
			mark = f"FAIL, tolerance {tol*100:.0f}%"
			failed += 1
		if off > tol:
			worst.append((off, key, ratio))
		print(f"  {key:<22}{h:>14}{f:>14}{ratio:>9.3f}  {mark}")

	# Derived ratios. These are the numbers that survive a change to the
	# example, so they are what a regression should actually be judged on.
	def ratio_row(label, num, den):
		if hw.get(den) and fc.get(den):
			a = hw[num] / hw[den]
			b = fc[num] / fc[den]
			print(f"  {label:<22}{a:>14.4f}{b:>14.4f}{b/a if a else 0:>9.3f}")
	print(f"  {'-'*59}")
	# Total instructions. Hardware's "instructions" counter is issue slots, so
	# the instruction count is that plus the parallel-issue counter - and this
	# sum, not either half, is the thing both sides can be held to.
	if all(k in hw and k in fc for k in ("instructions", "parallel_issued")):
		a = hw["instructions"] + hw["parallel_issued"]
		b = fc["instructions"] + fc["parallel_issued"]
		print(f"  {'total instructions':<22}{a:>14}{b:>14}{b/a if a else 0:>9.3f}")
		if a:
			print(f"  {'true IPC':<22}{a/hw['cycles']:>14.4f}"
					f"{b/fc['cycles']:>14.4f}{(b/fc['cycles'])/(a/hw['cycles']):>9.3f}")
	ratio_row("IPC", "instructions", "cycles")
	ratio_row("icache miss rate", "icache_miss", "ifetch")
	ratio_row("ocache miss rate", "ocache_miss", "operand_access")
	ratio_row("cyc per icache miss", "icache_stall_cycles", "icache_miss")
	ratio_row("cyc per ocache miss", "dcache_stall_cycles", "ocache_miss")
	# Share of INSTRUCTIONS that were the second of a pair. The denominator has
	# to be the instruction count - issue slots plus parallel - not the issue
	# slots alone, which produced rates above 100%.
	if all(k in hw and k in fc for k in ("instructions", "parallel_issued")):
		a = hw["parallel_issued"] / (hw["instructions"] + hw["parallel_issued"])
		b = fc["parallel_issued"] / (fc["instructions"] + fc["parallel_issued"])
		print(f"  {'paired share':<22}{a:>14.4f}{b:>14.4f}{b/a if a else 0:>9.3f}")

	if worst:
		worst.sort(reverse=True)
		print(f"\n  biggest divergence: {worst[0][1]} at {worst[0][2]:.3f}x")
	return failed

def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("examples", nargs="*", help="directory names under validation/")
	ap.add_argument("--flycast-arg", action="append", default=[],
			help="extra argument passed through to flycast")
	ap.add_argument("--reuse", action="store_true",
			help="compare the stored flycast.txt instead of running flycast")
	args = ap.parse_args()

	names = args.examples
	if not names:
		names = sorted(d for d in os.listdir(HERE)
				if os.path.isfile(os.path.join(HERE, d, "hardware.txt")))
	if not names:
		print("no examples with a hardware.txt", file=sys.stderr)
		return 2

	total_failed = 0
	for name in names:
		d = os.path.join(HERE, name)
		hwfile = os.path.join(d, "hardware.txt")
		if not os.path.isfile(hwfile):
			print(f"{name}: no hardware.txt, skipping")
			continue
		hw = parse(open(hwfile).read())

		fcfile = os.path.join(d, "flycast.txt")
		if args.reuse:
			if not os.path.isfile(fcfile):
				print(f"{name}: no flycast.txt to reuse")
				total_failed += 1
				continue
			out = open(fcfile).read()
		else:
			elfs = [f for f in os.listdir(d) if f.endswith(".elf")]
			if not elfs:
				print(f"{name}: no .elf")
				total_failed += 1
				continue
			out = run_flycast(os.path.join(d, elfs[0]), args.flycast_arg)
			# Keep only the DCBENCH lines: the rest is boot noise that would
			# make every diff useless.
			kept = [l for l in out.splitlines()
					if l.startswith("DCBENCH") or l.startswith("DCPHASE")]
			with open(fcfile, "w") as fh:
				fh.write("# Written by validate.py. Compare with hardware.txt.\n")
				fh.write("\n".join(kept) + "\n")
			out = "\n".join(kept)

		total_failed += report(name, hw, parse(out))

	print()
	print("all within tolerance" if total_failed == 0
			else f"{total_failed} counter(s) outside tolerance")
	return 1 if total_failed else 0

if __name__ == "__main__":
	sys.exit(main())

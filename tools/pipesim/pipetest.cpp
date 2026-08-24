// Differential test driver for the SH4 pipeline model.
//
// Reads 16-bit opcodes (hex, one per line, blank line = end of block) and
// prints one line per block in the same shape as the reference simulator's
// analyze.js, so the two can be diffed directly.
#include "hw/sh4/cachesim/pipesim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

int main(int argc, char **argv)
{
	const bool verbose = getenv("VERBOSE") != nullptr;
	FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
	if (f == nullptr) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

	std::vector<u16> ops;
	char line[256];
	auto flush = [&]() {
		if (ops.empty())
			return;
		std::vector<pipesim::InsnDetail> detail(ops.size());
		pipesim::Result r = pipesim::analyze(ops.data(), (u32)ops.size(), detail.data());
		printf("cycles=%u  insns=%u  stall-cycles=%u%s\n", r.cycles, r.instructions, r.stallCycles,
				r.stuck ? "  STUCK" : "");
		if (verbose)
			for (size_t i = 0; i < ops.size(); i++)
				if (detail[i].stallCycles != 0)
					printf("   %u  %-14s  op=%04x\n", detail[i].stallCycles,
							pipesim::stallReasonName(detail[i].reason), detail[i].op);
		ops.clear();
	};
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\n' || *p == '\0' || *p == '#') { flush(); continue; }
		ops.push_back((u16)strtoul(p, nullptr, 16));
	}
	flush();
	if (f != stdin) fclose(f);
	return 0;
}

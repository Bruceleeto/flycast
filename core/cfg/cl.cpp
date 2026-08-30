/*
	Copyright 2025 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "cfg/cfg.h"
#include "stdclass.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace config
{

static void usage(const char *exe)
{
	fprintf(stderr, "Usage: %s [option]... [<rom path>]\n", exe);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "-config section:key=value,...  set a transient config value.\n");
	fprintf(stderr, "                               Transient config values won't be saved to emu.cfg.\n");
	fprintf(stderr, "-headless                      run without a window, GUI or renderer.\n");
	fprintf(stderr, "                               Requires a content path. Audio is disabled.\n");
	fprintf(stderr, "-headless-frames n             stop after n emulated frames, then exit 0.\n");
	fprintf(stderr, "-headless-seconds n            stop after n seconds of wall clock, then exit 0.\n");
	fprintf(stderr, "-headless-progress n           log a progress line every n seconds.\n");
	fprintf(stderr, "                               Any -headless-* option implies -headless.\n");
	fprintf(stderr, "-cachesim                      simulate the guest SH4 caches and report miss\n");
	fprintf(stderr, "                               counts. Costs speed; nothing is charged to the\n");
	fprintf(stderr, "                               emulated timing.\n");
	fprintf(stderr, "-cachesim-report file          write the cache report there when the run ends.\n");
	fprintf(stderr, "                               Implies -cachesim.\n");
	fprintf(stderr, "-cachesim-symbols file        name rows using this ELF's symbols. By default the\n");
	fprintf(stderr, "                               ELF beside the content is used if there is one.\n");
	fprintf(stderr, "                               Ignored unless it matches the code that ran.\n");
	fprintf(stderr, "-cachesim-trace file           record the block execution stream, for replaying\n");
	fprintf(stderr, "                               layout changes offline with cachesweep. Large:\n");
	fprintf(stderr, "                               tens of MB per guest second. Implies -cachesim.\n");
	fprintf(stderr, "-cachesim-frames n             measure n guest frames, then report and exit 0.\n");
	fprintf(stderr, "                               0 means no limit: flycast will NOT exit on its own,\n");
	fprintf(stderr, "                               and a guest whose main returns is simply restarted.\n");
	fprintf(stderr, "                               Defines the window in guest time, so runs at\n");
	fprintf(stderr, "                               different speeds stay comparable.\n");
	fprintf(stderr, "-cachesim-skip n               clear the counters after n frames, to drop the\n");
	fprintf(stderr, "                               startup storm and measure steady state.\n");
	fprintf(stderr, "-cachesim-lookahead n          bytes fetched past the end of a block (default 0).\n");
	fprintf(stderr, "                               Calibrated against hardware, see docs/cachesim.\n");
	fprintf(stderr, "-cachesim-timing               charge modelled miss cycles to the emulated SH4.\n");
	fprintf(stderr, "-cachesim-timing-pipeline      also drive timing from the pipeline model instead of\n");
	fprintf(stderr, "                               flycast's per-block estimate. Implies -cachesim-timing.\n");
	fprintf(stderr, "                               Changes emulated time for every guest, not just misses.\n");
	fprintf(stderr, "                               Changes how the guest runs, so results from such\n");
	fprintf(stderr, "                               a run are not comparable with a normal one.\n");
	fprintf(stderr, "                               Implies -cachesim.\n");
	fprintf(stderr, "-cachesim-data                 also model the operand cache. Costs a call per\n");
	fprintf(stderr, "                               guest load and store, so it is off by default.\n");
	fprintf(stderr, "                               Implies -cachesim.\n");
	fprintf(stderr, "-jitdump-region ADDR:SIZE      dump this range of guest memory, and log every\n");
	fprintf(stderr, "                               entry into it. e.g. 0x8c400000:0x100000.\n");
	fprintf(stderr, "                               Writes <out>.bin, the raw bytes, and\n");
	fprintf(stderr, "                               <out>.entries, one record per entry. What those\n");
	fprintf(stderr, "                               bytes mean is left to the offline tool: see\n");
	fprintf(stderr, "                               docs/jitdump.md for the file layout.\n");
	fprintf(stderr, "-jitdump-out prefix            write <prefix>.bin and <prefix>.entries there.\n");
	fprintf(stderr, "                               Default 'jitdump'.\n");
	fprintf(stderr, "-jitdump-skip n                guest frames of warm-up before logging (300).\n");
	fprintf(stderr, "-jitdump-interval n            also write a numbered <prefix>.NNNN.bin every n\n");
	fprintf(stderr, "                               frames, to see the code region evolve.\n");
	fprintf(stderr, "-jitdump-max-entries n         stop logging after n entries (16M). 0 for no cap;\n");
	fprintf(stderr, "                               a record is 16 bytes and they come fast.\n");
	fprintf(stderr, "-jitdump-no-entries            dump the region only. This is the only mode that\n");
	fprintf(stderr, "                               works under the dynarec: the entry log is fed per\n");
	fprintf(stderr, "                               instruction, so -jitdump-region otherwise selects\n");
	fprintf(stderr, "                               interpreter for you.\n");
	fprintf(stderr, "-watch-write ADDR:SIZE         log every SH4 store into this range: the PC that\n");
	fprintf(stderr, "                               stored, the address, the value, the size and the\n");
	fprintf(stderr, "                               cycle. Writes <out>.writes.\n");
	fprintf(stderr, "-watch-badjump ADDR:SIZE       log every jump that leaves this range, with the\n");
	fprintf(stderr, "                               instruction it left from and where it went.\n");
	fprintf(stderr, "                               Writes <out>.jumps.\n");
	fprintf(stderr, "-watch-out prefix              where the watch logs go. Default 'watch'.\n");
	fprintf(stderr, "-watch-max n                   stop after n records (4M). 0 for no cap.\n");
	fprintf(stderr, "                               Both watchpoints need the interpreter, and\n");
	fprintf(stderr, "                               select it the way -jitdump-region does. A range\n");
	fprintf(stderr, "                               matches through any mirror of the same address.\n");
	fprintf(stderr, "-help                          display this help\n");
}

// True for -headless and for every -headless-* option, since all of them need
// the window and the display left alone. Called before the command line is
// parsed, so it cannot rely on settings.
bool headlessRequested(int argc, const char * const argv[])
{
	for (int i = 1; i < argc; i++)
	{
		const char *arg = argv[i];
		if (arg[0] == '-' && arg[1] == '-')
			arg++;
		if (!strncmp(arg, "-headless", 9))
			return true;
	}
	return false;
}

// Reads the value of an option that takes one, e.g. -headless-frames 300
static bool optionValue(int argc, const char * const argv[], int& i, const char *name,
		const char *& value)
{
	const char *arg = argv[i];
	if (arg[0] == '-' && arg[1] == '-')
		arg++;
	if (strcmp(arg, name) != 0)
		return false;
	if (i >= argc - 1) {
		WARN_LOG(COMMON, "Option '%s' needs a value", argv[i]);
		return false;
	}
	value = argv[++i];
	return true;
}

static void parseConfigOption(const std::string& str)
{
	char inQuote = '\0';
	std::string section, key, value;
	int step = 0; // section, key, value
	for (char c : str)
	{
		if (inQuote != '\0' && c == inQuote)
		{
			inQuote = false;
			step = 0;
			setTransient(section, key, value);
			DEBUG_LOG(COMMON, "-config [%s] %s = %s", section.c_str(), key.c_str(), value.c_str());
			section.clear();
			key.clear();
			value.clear();
			continue;
		}
		switch (c)
		{
		case ':':
			switch (step)
			{
			case 0:
				if (section.empty()) {
					WARN_LOG(COMMON, "Invalid -config option '%s'. Format is: -config section:key=value,...", str.c_str());
					return;
				}
				step = 1;
				break;
			case 1:
				key += c;
				break;
			case 2:
				value += c;
				break;
			}
			break;
		case '=':
			switch (step)
			{
			case 0:
				WARN_LOG(COMMON, "Invalid -config option '%s'. Format is: -config section:key=value,...", str.c_str());
				return;
			case 1:
				if (key.empty()) {
					WARN_LOG(COMMON, "Invalid -config option '%s'. Format is: -config section:key=value,...", str.c_str());
					return;
				}
				step = 2;
				break;
			case 2:
				value += c;
				break;
			}
			break;
		case '\'':
		case '"':
			switch (step)
			{
			case 0:
				section += c;
				break;
			case 1:
				key += c;
				break;
			case 2:
				if (inQuote == '\0') {
					inQuote = c;
					value.clear();
				}
				else
					value += c;
				break;
			}
			break;
		case ',':
			switch (step)
			{
			case 0:
				// ignore consecutive commas
				break;
			case 1:
				key += c;
				break;
			case 2:
				if (inQuote != '\0') {
					value += c;
				}
				else
				{
					step = 0;
					setTransient(section, key, value);
					DEBUG_LOG(COMMON, "-config [%s] %s = %s", section.c_str(), key.c_str(), value.c_str());
					section.clear();
					key.clear();
					value.clear();
				}
				break;
			}
			break;
		case ' ':
			switch (step)
			{
			case 0:
			case 1:
				// Ignore
				break;
			case 2:
				value += c;
				break;
			}
			break;
		default:
			switch (step)
			{
			case 0:
				section += c;
				break;
			case 1:
				key += c;
				break;
			case 2:
				value += c;
				break;
			}
		}
	}
	if (step == 2) {
		setTransient(section, key, value);
		DEBUG_LOG(COMMON, "-config [%s] %s = %s", section.c_str(), key.c_str(), value.c_str());
	}
}

void parseCommandLine(int argc, const char * const argv[])
{
	settings.content.path.clear();
	const char *exe = argv[0];
	// The entry log is fed one instruction at a time by the interpreter, so
	// asking for it is asking for the interpreter. Decided before the main
	// pass rather than after it, so that an explicit -config Dynarec.Enabled
	// still overrides it.
	{
		bool jitdump = false;
		bool entries = true;
		for (int i = 1; i < argc; i++)
		{
			const char *arg = argv[i];
			if (arg[0] == '-' && arg[1] == '-')
				arg++;
			if (!strncmp(arg, "-jitdump", 8) || !strncmp(arg, "-watch-", 7))
				jitdump = true;
			if (!strcmp(arg, "-jitdump-no-entries"))
				entries = false;
		}
		if (jitdump && entries)
			setTransient("config", "Dynarec.Enabled", "no");
	}
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help")) {
			usage(exe);
			exit(0);
		}
		if (!strcmp(argv[i], "-headless") || !strcmp(argv[i], "--headless")) {
			settings.headless = true;
			continue;
		}
		const char *value;
		if (optionValue(argc, argv, i, "-headless-frames", value)) {
			settings.headless = true;
			settings.headlessFrames = (u32)strtoul(value, nullptr, 0);
			continue;
		}
		if (optionValue(argc, argv, i, "-headless-seconds", value)) {
			settings.headless = true;
			settings.headlessSeconds = (u32)strtoul(value, nullptr, 0);
			continue;
		}
		if (optionValue(argc, argv, i, "-headless-progress", value)) {
			settings.headless = true;
			settings.headlessProgress = (u32)strtoul(value, nullptr, 0);
			continue;
		}
		if (!strcmp(argv[i], "-cachesim") || !strcmp(argv[i], "--cachesim")) {
			setTransient("config", "Debug.CacheSim", "yes");
			continue;
		}
		if (!strcmp(argv[i], "-cachesim-timing-pipeline")
				|| !strcmp(argv[i], "--cachesim-timing-pipeline")) {
			setTransient("config", "Debug.CacheSim", "yes");
			setTransient("config", "Debug.CacheSimTiming", "yes");
			setTransient("config", "Debug.CacheSimTimingPipeline", "yes");
			continue;
		}
		if (!strcmp(argv[i], "-cachesim-timing") || !strcmp(argv[i], "--cachesim-timing")) {
			setTransient("config", "Debug.CacheSim", "yes");
			setTransient("config", "Debug.CacheSimTiming", "yes");
			continue;
		}
		if (!strcmp(argv[i], "-cachesim-data") || !strcmp(argv[i], "--cachesim-data")) {
			setTransient("config", "Debug.CacheSim", "yes");
			setTransient("config", "Debug.CacheSimData", "yes");
			continue;
		}
		if (optionValue(argc, argv, i, "-cachesim-lookahead", value)) {
			setTransient("config", "Debug.CacheSimLookahead", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-cachesim-symbols", value)) {
			setTransient("config", "Debug.CacheSimSymbols", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-cachesim-trace", value)) {
			setTransient("config", "Debug.CacheSim", "yes");
			setTransient("config", "Debug.CacheSimTrace", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-cachesim-frames", value)) {
			setTransient("config", "Debug.CacheSimFrames", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-cachesim-skip", value)) {
			setTransient("config", "Debug.CacheSimSkipFrames", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-cachesim-report", value)) {
			setTransient("config", "Debug.CacheSim", "yes");
			setTransient("config", "Debug.CacheSimReport", value);
			continue;
		}
		if (!strcmp(argv[i], "-jitdump-no-entries") || !strcmp(argv[i], "--jitdump-no-entries")) {
			setTransient("config", "Debug.JitDump", "yes");
			setTransient("config", "Debug.JitDumpEntries", "no");
			continue;
		}
		if (optionValue(argc, argv, i, "-jitdump-out", value)) {
			setTransient("config", "Debug.JitDump", "yes");
			setTransient("config", "Debug.JitDumpOut", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-jitdump-region", value)) {
			setTransient("config", "Debug.JitDump", "yes");
			setTransient("config", "Debug.JitDumpRegion", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-jitdump-skip", value)) {
			setTransient("config", "Debug.JitDumpSkipFrames", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-jitdump-interval", value)) {
			setTransient("config", "Debug.JitDumpInterval", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-jitdump-max-entries", value)) {
			setTransient("config", "Debug.JitDumpMaxEntries", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-watch-write", value)) {
			setTransient("config", "Debug.WatchWrite", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-watch-badjump", value)) {
			setTransient("config", "Debug.WatchBadJump", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-watch-out", value)) {
			setTransient("config", "Debug.WatchOut", value);
			continue;
		}
		if (optionValue(argc, argv, i, "-watch-max", value)) {
			setTransient("config", "Debug.WatchMaxRecords", value);
			continue;
		}
		if (!strcmp(argv[i], "-config") || !strcmp(argv[i], "--config"))
		{
			if (i < argc - 1)
				parseConfigOption(argv[++i]);
			continue;
		}
		// macOS
		if (!strncmp(argv[i], "-NSDocumentRevisions", 20)) {
			i++;
			continue;
		}
		if (argv[i][0] == '-') {
			WARN_LOG(COMMON, "Ignoring unknown command line option '%s'", argv[i]);
			continue;
		}
		std::string extension = get_file_extension(argv[i]);
		if (extension == "cdi" || extension == "chd"
				|| extension == "gdi"|| extension == "cue")
		{
			INFO_LOG(COMMON, "Using '%s' as CD image", argv[i]);
			settings.content.path = argv[i];
		}
		else if (extension == "elf")
		{
			INFO_LOG(COMMON, "Using '%s' as reios elf file", argv[i]);
			setTransient("config", "bios.UseReios", "yes");
			settings.content.path = argv[i];
		}
		else {
			INFO_LOG(COMMON, "Using '%s' as rom", argv[i]);
			settings.content.path = argv[i];
		}
		if (i < argc - 1)
			WARN_LOG(COMMON, "Rest of command line ignored: '%s'...", argv[i + 1]);
		break;
	}
}

}	// namespace config

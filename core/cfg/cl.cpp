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
	fprintf(stderr, "-cachesim-trace file           record the block execution stream, for replaying\n");
	fprintf(stderr, "                               layout changes offline with cachesweep. Large:\n");
	fprintf(stderr, "                               tens of MB per guest second. Implies -cachesim.\n");
	fprintf(stderr, "-cachesim-frames n             measure n guest frames, then report and stop.\n");
	fprintf(stderr, "                               Defines the window in guest time, so runs at\n");
	fprintf(stderr, "                               different speeds stay comparable.\n");
	fprintf(stderr, "-cachesim-skip n               clear the counters after n frames, to drop the\n");
	fprintf(stderr, "                               startup storm and measure steady state.\n");
	fprintf(stderr, "-cachesim-lookahead n          bytes fetched past the end of a block (default 16).\n");
	fprintf(stderr, "                               Calibrated against hardware, see docs/cachesim.\n");
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
		if (optionValue(argc, argv, i, "-cachesim-lookahead", value)) {
			setTransient("config", "Debug.CacheSimLookahead", value);
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

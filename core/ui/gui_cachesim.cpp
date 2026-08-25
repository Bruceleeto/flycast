/*
	Copyright 2026 Flycast contributors

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
//
// Where the guest's frame goes, ranked.
//
// Everything here is per frame and smoothed over recent frames. A total over a
// run is dominated by loading and compilation and describes no frame that ever
// happened, which is exactly the number that makes a profile misleading.
//
#include "gui_cachesim.h"
#include "gui_util.h"
#include "hw/sh4/cachesim/cachesim.h"
#include "hw/sh4/cachesim/cachesim_symbols.h"

#include "imgui.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <string>
#include <vector>

// Hidden state lives here rather than in the config: it is a view preference
// for this session, not something to persist and then wonder about later when
// the panel does not appear.
static bool panelVisible = true;
// The function whose blocks are expanded below the table, by address range. A
// renderer inlines its whole frame into one symbol, so the per-function view
// can say "main is 100% of the frame" and nothing more useful than that. The
// block level is where a hot span inside a function actually shows up.
static bool blocksOpen = false;
// Frozen view. The numbers are smoothed per frame and still move too fast to
// read on a workload doing 76,000 vertices a frame, so the panel can be held
// still. Measuring carries on underneath: this freezes the DISPLAY, not the
// profiler, and unpausing shows current numbers rather than a resumed replay.
static bool paused = false;
static std::vector<cachesim::ProfileRow> frozenRows;
static double frozenFrame = 0.0, frozenAccounted = 0.0;
static cachesim::Counters frozenCounters{};
static u32 selStart = 0, selEnd = 0;
static std::string selName;

// One row of the block breakdown.
struct BlockRow
{
	u32 vaddr, size;
	double execs, cycles, sqCycles;
	u32 pipeCycles;
	u32 flowDep, resource, stage;
	bool modelled;
};

static std::vector<BlockRow> frozenBlocks;
static u32 lastBlockSel = 0;

// Blocks living inside [start, end), hottest first. Recomputed each frame: the
// pool is small and this only runs while a row is expanded.
static std::vector<BlockRow> blocksIn(u32 start, u32 end)
{
	std::vector<BlockRow> out;

	for (const cachesim::BlockTrace& b : cachesim::blocks())
	{
		if (b.vaddr < start || b.vaddr >= end)
			continue;
		const double execs = cachesim::blockExecsPerFrame(b.id);
		if (execs < 0.5)
			continue;
		const double sq = cachesim::blockSqCyclesPerFrame(b.id);
		out.push_back({ b.vaddr, b.size, execs, execs * b.pipeCycles + sq, sq,
				b.pipeCycles,
				b.pipeByReason[(int)pipesim::StallReason::FlowDep],
				b.pipeByReason[(int)pipesim::StallReason::ResourceHazard],
				(u32)(b.pipeByReason[(int)pipesim::StallReason::StageFull]
						+ b.pipeByReason[(int)pipesim::StallReason::StageLocked]),
				b.pipeModelled });
	}
	std::sort(out.begin(), out.end(),
			[](const BlockRow& a, const BlockRow& b) { return a.cycles > b.cycles; });
	return out;
}

// A row's cost, matching logProfile() exactly: hardware-checked pipeline cycles
// where the model produced any, instruction cache fill, operand cache fill, and
// store queue. All four, since the operand cache miss count was validated in 9g.
static double rowTotal(const cachesim::ProfileRow& r)
{
	return (r.pipeCycles > 0.0 ? r.pipeCycles : r.cycles)
			+ r.missCycles + r.dataMissCycles + r.sqCycles;
}

// A wrapped tooltip. SetTooltip does not wrap, and every one of these is a
// sentence rather than a label.
static void tip(const char *text)
{
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		return;
	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(uiScaled(360.f));
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

// The same, formatted. The two lines that carry live numbers need both.
static void tipf(const char *fmt, ...)
{
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		return;
	va_list args;
	va_start(args, fmt);
	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(uiScaled(360.f));
	ImGui::TextV(fmt, args);
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
	va_end(args);
}

// A column header that explains itself. Nothing in this panel is guessable from
// a four-character label, so every one of them carries a sentence.
static void header(int column, const char *help)
{
	if (!ImGui::TableSetColumnIndex(column))
		return;
	ImGui::TableHeader(ImGui::TableGetColumnName(column));
	tip(help);
}

// Warm where it matters. Scanning a column for the big numbers should not
// require reading any of the small ones.
static ImVec4 heat(double pct)
{
	if (pct >= 50.0)
		return ImVec4(1.00f, 0.55f, 0.35f, 1.f);
	if (pct >= 25.0)
		return ImVec4(0.95f, 0.85f, 0.45f, 1.f);
	return ImGui::GetStyle().Colors[ImGuiCol_Text];
}

// Every cost column is CYCLES PER FRAME now, and they add up to the row's
// total. Shown as a share of the row so a wide table stays scannable, with the
// cycle count in the tooltip - but unlike the old event counts these really are
// time, and really do sum.
static void cycleCell(double part, double total)
{
	if (part <= 0.0 || total <= 0.0)
	{
		ImGui::TextDisabled("-");
		return;
	}
	const double pct = 100.0 * part / total;
	ImGui::TextColored(heat(pct), "%.0f%%", pct);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		ImGui::SetTooltip("%.0f cycles/frame", part);
}

// One sentence saying what this row's problem actually is. The columns carry
// the evidence; this carries the conclusion, which is the part somebody who did
// not build the model cannot be expected to derive from four percentages.
static const char *verdict(const cachesim::ProfileRow& r, double total)
{
	if (total <= 0.0)
		return "";
	if (r.sqCycles > total * 0.25)
		return "Held up sending data out of the CPU. Instruction scheduling will "
				"not touch this - it takes fewer or smaller vertices.";
	if (r.dataMissCycles > total * 0.25)
		return "Held up waiting for data. The access pattern is fighting the "
				"cache - change the data layout, not the code.";
	if (r.missCycles > total * 0.25)
		return "Held up fetching its own code. This is a layout problem: move "
				"functions apart so hot ones stop evicting each other.";
	// Cycles now, and they are shares of the whole row rather than of each
	// other, so the thresholds are against `total` directly.
	const double ev = r.pipeFlowDep + r.pipeResource + r.pipeStage;
	if (ev < total * 0.15)
		return "Issuing steadily with little stalling. To make this faster it has "
				"to do less work - there is no waiting left to reclaim.";
	if (r.pipeFlowDep > ev * 0.5)
		return "Mostly waiting on its own earlier results. The work is fine, the "
				"order is not: interleave independent work between an instruction "
				"and whoever uses its answer.";
	if (r.pipeStage > ev * 0.4)
		return "Long instructions - divide, square root, matrix transform - packed "
				"too closely together. Spread them out, or find a cheaper way to "
				"get the same answer.";
	if (r.pipeResource > ev * 0.4)
		return "Instructions competing for the same execution unit. Reach for "
				"different instructions rather than fewer of them.";
	return "Stalls are spread evenly across all three causes, so no single "
			"change stands out. Doing less work is the reliable lever.";
}

// The share-of-frame column, drawn as a bar behind the number. The bar is what
// makes the ranking readable without comparing digits.
static void pctCell(double pct)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	const float h = ImGui::GetTextLineHeight();
	float frac = (float)(pct / 100.0);
	frac = frac < 0.f ? 0.f : (frac > 1.f ? 1.f : frac);
	if (frac > 0.005f)
		dl->AddRectFilled(p, ImVec2(p.x + w * frac, p.y + h),
				ImGui::GetColorU32(ImVec4(0.30f, 0.58f, 0.90f, 0.40f)), 2.f);
	ImGui::Text("%.1f", pct);
}

void drawCacheSimPanel()
{
	if (!cachesim::armed())
	{
		// Armed again later: come back visible rather than mysteriously hidden
		panelVisible = true;
		return;
	}

	if (!panelVisible)
	{
		// Small enough to ignore, present enough to find again
		ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##cachesimProfileHidden", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize
				| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav))
		{
			if (ImGui::Button("profile"))
				panelVisible = true;
		}
		ImGui::End();
		return;
	}
	if (!paused)
	{
		frozenRows = cachesim::profile(200);
		frozenFrame = cachesim::profileFrameCycles();
		frozenAccounted = cachesim::profileAccountedCycles();
		frozenCounters = cachesim::frameCounters();
	}
	const double frameCycles = frozenFrame;
	const double accounted = frozenAccounted;
	const std::vector<cachesim::ProfileRow>& rows = frozenRows;

	const ImGuiIO& io = ImGui::GetIO();
	// Left edge, full height, by default only: dragging the edge or the body
	// sticks, because 430px is a guess about somebody else's screen
	ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(uiScaled(430.f), io.DisplaySize.y), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(uiScaled(240.f), uiScaled(120.f)),
			ImVec2(io.DisplaySize.x, io.DisplaySize.y));
	ImGui::SetNextWindowBgAlpha(0.75f);
	if (!ImGui::Begin("##cachesimProfile", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoNav))
	{
		ImGui::End();
		return;
	}

	const cachesim::Counters& frame = frozenCounters;
	const int inst = (int)cachesim::Stream::Inst;
	const int data = (int)cachesim::Stream::Data;

	// What the model accounts for, and what it says the frame is. These are two
	// different numbers and showing only one of them is how a profile lies.
	double modelled = 0.0, flowDep = 0.0, resource = 0.0, stage = 0.0, sqCycles = 0.0;
	bool anyIncomplete = false;
	for (const cachesim::ProfileRow& r : rows)
	{
		modelled += rowTotal(r);
		flowDep += r.pipeFlowDep;
		resource += r.pipeResource;
		stage += r.pipeStage;
		sqCycles += r.sqCycles;
		if (!r.pipeComplete)
			anyIncomplete = true;
	}
	const double stallEvents = flowDep + resource + stage;

	// The headline: what the code costs against what a frame can afford. An
	// SH4 runs at 200MHz, so 60fps is 3.33M cycles and 30fps is 6.67M. Showing
	// the raw total alone leaves the one question anybody has unanswered.
	constexpr double BUDGET_60 = 200e6 / 60.0;
	const double budgetPct = 100.0 * modelled / BUDGET_60;
	ImGui::TextColored(budgetPct <= 100.0 ? ImVec4(0.55f, 0.90f, 0.55f, 1.f)
					: ImVec4(1.00f, 0.55f, 0.35f, 1.f),
			"%.2fM cycles/frame - %.0f%% of a 60fps budget", modelled / 1e6, budgetPct);
	tip("What the guest's code costs in one frame, and how that compares with "
			"what a Dreamcast can afford.\n\n"
			"The SH4 runs at 200MHz, so a 60fps frame is 3.33 million cycles and "
			"a 30fps frame is 6.67 million. Over 100% means the CPU alone cannot "
			"hold 60, whatever the graphics hardware is doing. Comfortably under "
			"it means the CPU is not what is holding the frame rate down, and "
			"making this number smaller will not help.");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - uiScaled(112.f));
	if (ImGui::SmallButton(paused ? "resume" : "pause"))
		paused = !paused;
	tip("Hold the numbers still so they can be read. On a busy workload they "
			"move faster than anybody can follow.\n\n"
			"This freezes the DISPLAY only - measuring carries on underneath, and "
			"resuming shows current numbers rather than replaying what was "
			"missed.");
	ImGui::SameLine();
	if (ImGui::SmallButton("copy"))
	{
		std::string out;
		char line[256];
		std::snprintf(line, sizeof(line),
				"%-34s %9s %7s %8s %8s %8s %8s %7s %7s %7s\n",
				"function", "cycles", "%frame", "issue", "flow", "res", "stage",
				"i$", "d$", "sq");
		out += line;
		for (const cachesim::ProfileRow& r : rows)
		{
			const double t = rowTotal(r);
			if (t < 1.0)
				continue;
			// Cycles, not shares: pasted somewhere else these need to be
			// comparable against another run, and percentages of different
			// totals are not.
			std::snprintf(line, sizeof(line),
					"%-34s %9.0f %6.1f%% %8.0f %8.0f %8.0f %8.0f %7.0f %7.0f %7.0f\n",
					r.name.c_str(), t,
					frameCycles == 0 ? 0.0 : 100.0 * t / frameCycles,
					r.pipeIssue, r.pipeFlowDep, r.pipeResource, r.pipeStage,
					r.missCycles, r.dataMissCycles, r.sqCycles);
			out += line;
		}
		ImGui::SetClipboardText(out.c_str());
	}
	tip("Copy the table as plain text, for pasting somewhere it can be compared "
			"against another run.");
	ImGui::SameLine();
	if (ImGui::SmallButton("x"))
		panelVisible = false;
	tip("Hide this panel. Measuring carries on.");

	// The one-line answer. Everything below is the evidence for it.
	if (!rows.empty())
	{
		const cachesim::ProfileRow& top = rows.front();
		const double topTotal = rowTotal(top);
		ImGui::PushTextWrapPos(0.f);
		if (budgetPct > 100.0)
			ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.35f, 1.f),
					"CPU-bound. %s is %.0f%% of it. %s",
					top.name.c_str(),
					modelled == 0 ? 0.0 : 100.0 * topTotal / modelled,
					verdict(top, topTotal));
		else
			ImGui::TextColored(ImVec4(0.55f, 0.90f, 0.55f, 1.f),
					"Fits in a 60fps frame with %.0f%% to spare. If the frame rate "
					"is still short, the limit is elsewhere - graphics hardware, or "
					"waiting on vsync - and making this code faster will not move it.",
					100.0 - budgetPct);
		ImGui::PopTextWrapPos();
	}

	ImGui::TextDisabled("flycast counts %.2fM for the same frame", frameCycles / 1e6);
	tip("Flycast's own cycle estimate for this frame, for comparison.\n\n"
			"It is not hardware truth - flycast does not charge store queue "
			"stalls at all, and retires instructions at a different rate from the "
			"real chip - so the two numbers disagreeing is expected rather than a "
			"fault in either.");

	// The gap between the frame and what the rows add up to is the CPU waiting.
	// Without showing it, every percentage below would silently be a share of
	// work done rather than a share of the frame.
	const double idle = frameCycles > accounted ? frameCycles - accounted : 0.0;
	ImGui::Text("executing %.1f%%   idle or waiting %.1f%%",
			frameCycles == 0 ? 0.0 : 100.0 * accounted / frameCycles,
			frameCycles == 0 ? 0.0 : 100.0 * idle / frameCycles);
	tip("How much of the frame was spent running guest code at all.\n\n"
			"The rest is the CPU idle, asleep, or spinning somewhere that never "
			"executed a block - waiting on the graphics hardware, or on vsync. A "
			"large idle share means the CPU is not the bottleneck.");

	// Where the stalls are, over the whole frame. This is the line that says
	// what KIND of change would help before any single row is read.
	if (stallEvents > 0.0)
	{
		ImGui::Text("stalls: flow-dep %.0f%%  resource %.0f%%  stage %.0f%%",
				100.0 * flowDep / stallEvents, 100.0 * resource / stallEvents,
				100.0 * stage / stallEvents);
		tip("Why the whole frame stalled, before looking at any single row.\n\n"
				"FLOW-DEP: waiting on earlier results. Reorder the code.\n"
				"RESOURCE: instructions competing for the same execution unit. "
				"Use different instructions.\n"
				"STAGE: long instructions - divide, square root, matrix - packed "
				"too closely. Spread them out.\n\n"
				"Whichever is largest is the kind of change worth trying first.");
	}

	const u64 flushes = frame.sqFlushes[(int)cachesim::SqDest::Ram]
			+ frame.sqFlushes[(int)cachesim::SqDest::Ta]
			+ frame.sqFlushes[(int)cachesim::SqDest::Other];
	if (flushes != 0)
	{
		ImGui::Text("store queue: %" PRIu64 " flushes/frame (%.0fk cycles, %.0f%%)",
				flushes, sqCycles / 1000.0,
				frameCycles == 0 ? 0.0 : 100.0 * sqCycles / frameCycles);
		tipf("The store queue sends data out of the CPU 32 bytes at a time - "
				"this is how vertices reach the tile accelerator.\n\n"
				"%" PRIu64 " went to the TA, %" PRIu64 " to RAM: %.1f MB this frame.\n\n"
				"Charged at %.1f cycles per flush to the TA and %.1f to RAM. "
				"Measured, and flat: sweeping the gap between flushes from 16 to "
				"205 cycles does not change it, so the queue always drains faster "
				"than the CPU can refill it.",
				frame.sqFlushes[(int)cachesim::SqDest::Ta],
				frame.sqFlushes[(int)cachesim::SqDest::Ram],
				flushes * 32.0 / (1024.0 * 1024.0),
				cachesim::penaltyConfig().sqFlushTa,
				cachesim::penaltyConfig().sqFlushRam);
	}

	ImGui::Text("icache: %" PRIu64 " misses/frame, %.1f%% conflict",
			frame.misses[inst],
			frame.misses[inst] == 0 ? 0.0
					: 100.0 * frame.missKinds[inst][(int)cachesim::MissKind::Conflict]
							/ frame.misses[inst]);
	tip("How often the CPU had to stop and fetch its own code from RAM.\n\n"
			"CONFLICT means the line was thrown out by other code landing in the "
			"same cache slot, and would still have been there in a cache of the "
			"same size arranged differently. A high conflict share is worth "
			"acting on, because moving code around fixes it. A low one means the "
			"code is simply bigger than the cache.");
	if (cachesim::dataFeed())
	{
		ImGui::Text("ocache: %" PRIu64 " misses/frame, %.1f%% conflict (not charged)",
				frame.misses[data],
				frame.misses[data] == 0 ? 0.0
						: 100.0 * frame.missKinds[data][(int)cachesim::MissKind::Conflict]
								/ frame.misses[data]);
		tip("How often the CPU had to wait for DATA rather than code.\n\n"
				"Counted, but deliberately kept out of the cycle totals: this model "
				"finds about half the misses a real Dreamcast reports, so any cycle "
				"figure built on it would be wrong by the same factor. The count is "
				"still useful for comparing one run against another.");
	}
	else
		ImGui::TextDisabled("ocache: not measured");

	// Modelled cost can legitimately exceed flycast's own frame, because flycast
	// does not stall on the store queue at all. Saying so beats leaving somebody
	// to wonder why the percentages add up to more than a hundred.
	if (modelled > frameCycles * 1.02 && frameCycles > 0.0)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.f, 1.f));
		ImGui::TextWrapped("modelled %.2fM > flycast's %.2fM: flycast does not charge"
				" store queue stalls, so the excess is the gap between them",
				modelled / 1e6, frameCycles / 1e6);
		ImGui::PopStyleColor();
	}
	if (cachesim::timingFeedback())
	{
		// This one changes the game rather than measuring it, so it says so
		// where the numbers are, not only in a log line at startup. Wrapped,
		// because the panel is resizable and this is the longest line in it
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.7f, 0.2f, 1.f));
		ImGui::TextWrapped("charging miss cycles to guest timing: not a normal run");
		ImGui::PopStyleColor();
	}
	if (!cachesim::symbolsLoaded())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
		ImGui::TextWrapped("no symbols: rows are address ranges");
		ImGui::PopStyleColor();
	}

	ImGui::Separator();

	// The footer wraps, so how much room it needs depends on how narrow the
	// panel has been dragged. Measuring it is the only way the table below can
	// reserve the right amount and not clip it.
	static const char *footer =
			"pipeline cycles are hardware-checked; stall columns are shares of a row's"
			" own stalls, not time. Click a row for its blocks.";
	const float footerHeight = ImGui::CalcTextSize(footer, nullptr, false,
			ImGui::GetContentRegionAvail().x).y + ImGui::GetStyle().ItemSpacing.y * 2;

	// Split the remaining height when a row is expanded, so both tables stay
	// usable rather than the block list being squeezed to two rows.
	float tableHeight = ImGui::GetContentRegionAvail().y - footerHeight;
	float blockHeight = 0.f;
	if (blocksOpen)
	{
		blockHeight = tableHeight * 0.45f;
		tableHeight -= blockHeight;
	}

	if (ImGui::BeginTable("##profile", 10,
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit,
			ImVec2(0.f, tableHeight)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("function", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("cycles", ImGuiTableColumnFlags_WidthFixed, uiScaled(46.f));
		ImGui::TableSetupColumn("% frame", ImGuiTableColumnFlags_WidthFixed, uiScaled(50.f));
		ImGui::TableSetupColumn("issue", ImGuiTableColumnFlags_WidthFixed, uiScaled(36.f));
		ImGui::TableSetupColumn("flow", ImGuiTableColumnFlags_WidthFixed, uiScaled(34.f));
		ImGui::TableSetupColumn("res", ImGuiTableColumnFlags_WidthFixed, uiScaled(34.f));
		ImGui::TableSetupColumn("stage", ImGuiTableColumnFlags_WidthFixed, uiScaled(38.f));
		ImGui::TableSetupColumn("i$", ImGuiTableColumnFlags_WidthFixed, uiScaled(30.f));
		ImGui::TableSetupColumn("d$", ImGuiTableColumnFlags_WidthFixed, uiScaled(30.f));
		ImGui::TableSetupColumn("sq", ImGuiTableColumnFlags_WidthFixed, uiScaled(30.f));

		// Every label here is jargon. Hovering any of them explains what it is
		// and, more usefully, what you would do about a big number in it.
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		header(0, "The function this code belongs to.\n\n"
				"Dimmed rows have no symbol - generated code, or a build without "
				"them - and are named by the address range they occupy instead.\n\n"
				"Click a row to break it open into its individual blocks. Worth "
				"doing for anything large: an optimised renderer inlines its whole "
				"frame into one function, and \"main is 90% of the frame\" tells "
				"you nothing until you can see inside it.");
		header(1, "Guest SH4 cycles this row costs in one frame: issuing "
				"instructions, waiting on them, fetching its own code, and "
				"draining the store queue.\n\n"
				"For scale, a Dreamcast has about 3.33 million cycles per frame to "
				"spend if you want 60fps, and 6.67 million at 30. If the top few "
				"rows already add up to more than that, the CPU is what is holding "
				"the frame rate down.");
		header(2, "This row's share of the whole frame. The bar behind the number "
				"is the same value, so the ranking reads at a glance.");
		header(3, "ISSUING - actually starting instructions, as opposed to "
				"waiting to.\n\n"
				"This is the irreducible part: the SH4 starts at most two "
				"instructions per cycle, so a row cannot go below half its "
				"instruction count however well it is scheduled. If issue is "
				"most of the row, the code is not stalling - it is simply doing "
				"a lot, and the only way to make it faster is to do less.");
		header(4, "FLOW DEPENDENCY - waiting on a result that an earlier "
				"instruction has not finished producing yet.\n\n"
				"A big number here means the amount of work is fine but the ORDER "
				"is not. The fix is to move independent work in between an "
				"instruction and whoever consumes its result, so the wait is spent "
				"doing something.");
		header(5, "RESOURCE CONFLICT - two instructions that cannot start in the "
				"same cycle because they need the same kind of execution unit.\n\n"
				"The SH4 can begin two instructions per cycle, but only certain "
				"pairs. A big number here means reaching for DIFFERENT "
				"instructions rather than fewer of them.");
		header(6, "STAGE BUSY - a pipeline stage was occupied or locked by a "
				"long-running instruction: a divide, a square root, a matrix "
				"transform.\n\n"
				"A big number here means the expensive instructions are packed too "
				"closely together. Spread them out, or find a cheaper way to get "
				"the same answer.");
		header(7, "INSTRUCTION CACHE - the share of this row spent fetching its "
				"own code into the cache before it could run.\n\n"
				"This is a layout problem, not a code problem. The fix is moving "
				"functions so hot ones stop evicting each other, not rewriting "
				"them.");
		header(8, "OPERAND CACHE - the share of this row spent waiting for DATA "
				"rather than for code.\n\n"
				"High here means the access pattern is fighting the cache: walking "
				"memory with a stride that skips whole lines, or bouncing between "
				"addresses that land in the same cache slot. The fix is usually to "
				"change the data layout rather than the code.");
		header(9, "STORE QUEUE - the share of this row spent waiting for 32-byte "
				"blocks to drain out to the tile accelerator or to RAM.\n\n"
				"No amount of instruction scheduling fixes this one. It comes down "
				"either way to sending less data - fewer vertices, or smaller "
				"ones.\n\n"
				"Treat the size as a rough guide: it is charged at a flat rate per "
				"flush, and the real cost depends on how much other work sits "
				"between one flush and the next.");

		// Every column is a share of the row and they sum to it. Worth saying
		// once, above the columns, rather than in seven tooltips.
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(3);
		ImGui::TextDisabled("cycles, as a share of the row - these add up to it");

		for (const cachesim::ProfileRow& row : rows)
		{
			const double total = rowTotal(row);
			if (total < 1.0)
				continue;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			const bool selected = blocksOpen && row.start == selStart && row.end == selEnd;
			// Generated code and unsymbolised ranges are dimmed: they are a
			// place, not a name, and cannot be looked up in any source file
			if (!row.named)
				ImGui::PushStyleColor(ImGuiCol_Text,
						ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			if (ImGui::Selectable(row.name.c_str(), selected,
					ImGuiSelectableFlags_SpanAllColumns))
			{
				if (selected)
					blocksOpen = false;
				else
				{
					blocksOpen = true;
					selStart = row.start;
					selEnd = row.end;
					selName = row.name;
				}
			}
			if (!row.named)
				ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
			{
				// Built line by line rather than as one string with a newline
				// in it, which was rendering with the second line clipped.
				// This form sizes each line explicitly.
				ImGui::BeginTooltip();
				ImGui::Text("%08x-%08x", row.start, row.end);
				ImGui::Text("%.0f calls/frame", row.calls);
				if (!row.pipeComplete)
					ImGui::TextDisabled("contains an opcode with no pipeline data");
				ImGui::Separator();
				ImGui::PushTextWrapPos(uiScaled(340.f));
				ImGui::TextUnformatted(verdict(row, total));
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}

			ImGui::TableNextColumn();
			ImGui::Text("%.0fk", total / 1000.0);
			ImGui::TableNextColumn();
			pctCell(frameCycles == 0 ? 0.0 : 100.0 * total / frameCycles);

			// The three stall columns are what make a row actionable. High
			// flow-dep means reorder it; high resource means the groups
			// collide; high stage means a unit is busy or locked.
			ImGui::TableNextColumn(); cycleCell(row.pipeIssue, total);
			ImGui::TableNextColumn(); cycleCell(row.pipeFlowDep, total);
			ImGui::TableNextColumn(); cycleCell(row.pipeResource, total);
			ImGui::TableNextColumn(); cycleCell(row.pipeStage, total);

			// Waiting on code, then waiting on the bus. Different fixes.
			ImGui::TableNextColumn(); cycleCell(row.missCycles, total);
			ImGui::TableNextColumn(); cycleCell(row.dataMissCycles, total);
			ImGui::TableNextColumn(); cycleCell(row.sqCycles, total);
		}
		ImGui::EndTable();
	}

	if (blocksOpen)
	{
		// Recomputed while running, and whenever the selection changes even if
		// held, so expanding a different row while paused still shows something.
		if (!paused || lastBlockSel != selStart)
		{
			frozenBlocks = blocksIn(selStart, selEnd);
			lastBlockSel = selStart;
		}
		const std::vector<BlockRow>& brows = frozenBlocks;
		ImGui::Text("%s", selName.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("- %zu blocks", brows.size());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - uiScaled(18.f));
		if (ImGui::SmallButton("^"))
			blocksOpen = false;

		if (ImGui::BeginTable("##blocks", 7,
				ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit,
				ImVec2(0.f, blockHeight - ImGui::GetTextLineHeightWithSpacing())))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("block", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("cycles", ImGuiTableColumnFlags_WidthFixed, uiScaled(46.f));
			ImGui::TableSetupColumn("% frame", ImGuiTableColumnFlags_WidthFixed, uiScaled(50.f));
			ImGui::TableSetupColumn("each", ImGuiTableColumnFlags_WidthFixed, uiScaled(38.f));
			ImGui::TableSetupColumn("flow", ImGuiTableColumnFlags_WidthFixed, uiScaled(34.f));
			ImGui::TableSetupColumn("res", ImGuiTableColumnFlags_WidthFixed, uiScaled(34.f));
			ImGui::TableSetupColumn("stage", ImGuiTableColumnFlags_WidthFixed, uiScaled(38.f));

			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			header(0, "The address of one block: a straight run of guest "
					"instructions with no branch into or out of the middle.\n\n"
					"This is the address to hand to addr2line if you want the "
					"source line it came from.");
			header(1, "Guest SH4 cycles this block costs across the whole frame - "
					"the cost of running it once, times how often it runs.");
			header(2, "This block's share of the whole frame.");
			header(3, "Cycles for ONE execution of this block, once the pipeline "
					"is full.\n\n"
					"Divide by roughly half the block's byte count to get cycles "
					"per instruction. Much above 1 means it is spending most of "
					"its time waiting rather than working, and the three columns "
					"to the right say what for.");
			header(4, "Waiting on a result an earlier instruction has not finished "
					"producing. Reorder the block so something useful happens "
					"during the wait.");
			header(5, "Two instructions needing the same execution unit in the "
					"same cycle. Use different instructions, not fewer.");
			header(6, "A stage held by a long-running instruction - divide, square "
					"root, matrix transform. Spread them apart.");

			for (const BlockRow& b : brows)
			{
				const double ev = b.flowDep + b.resource + b.stage;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%08x", b.vaddr);
				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Text("%u bytes, %u instructions", b.size, b.size / 2);
					ImGui::Text("%.0f executions/frame", b.execs);
					if (b.sqCycles > 0)
						ImGui::Text("%.0f store queue cycles/frame", b.sqCycles);
					if (!b.modelled)
						ImGui::TextDisabled("contains an opcode with no pipeline data");
					ImGui::EndTooltip();
				}
				ImGui::TableNextColumn();
				ImGui::Text("%.0fk", b.cycles / 1000.0);
				ImGui::TableNextColumn();
				pctCell(frameCycles == 0 ? 0.0 : 100.0 * b.cycles / frameCycles);
				ImGui::TableNextColumn();
				ImGui::Text("%u", b.pipeCycles);
				ImGui::TableNextColumn(); cycleCell(b.flowDep, ev);
				ImGui::TableNextColumn(); cycleCell(b.resource, ev);
				ImGui::TableNextColumn(); cycleCell(b.stage, ev);
			}
			ImGui::EndTable();
		}
	}

	// Not hardware truth, and it should not need explaining twice
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
	ImGui::TextWrapped("%s", footer);
	if (anyIncomplete)
		ImGui::TextWrapped("some blocks had an opcode with no pipeline data and were"
				" charged their issue rate with no stalls");
	ImGui::PopStyleColor();
	ImGui::End();
}

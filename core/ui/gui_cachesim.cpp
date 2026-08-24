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

#include <cinttypes>

// Hidden state lives here rather than in the config: it is a view preference
// for this session, not something to persist and then wonder about later when
// the panel does not appear.
static bool panelVisible = true;

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
	const double frameCycles = cachesim::profileFrameCycles();
	const double accounted = cachesim::profileAccountedCycles();
	const std::vector<cachesim::ProfileRow> rows = cachesim::profile(200);

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

	const cachesim::Counters frame = cachesim::frameCounters();
	const int inst = (int)cachesim::Stream::Inst;

	// Right-aligned, so it does not push the numbers around
	ImGui::Text("guest frame: %.2fM cycles", frameCycles / 1e6);
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - uiScaled(18.f));
	if (ImGui::SmallButton("x"))
		panelVisible = false;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("hide this panel; measuring continues");
	// The gap between the frame and what the rows add up to is the CPU waiting.
	// Without showing it, every percentage below would silently be a share of
	// work done rather than a share of the frame.
	const double idle = frameCycles > accounted ? frameCycles - accounted : 0.0;
	ImGui::Text("executing %.1f%%   idle or waiting %.1f%%",
			frameCycles == 0 ? 0.0 : 100.0 * accounted / frameCycles,
			frameCycles == 0 ? 0.0 : 100.0 * idle / frameCycles);
	ImGui::Text("icache: %" PRIu64 " misses/frame, %.1f%% conflict",
			frame.misses[inst],
			frame.misses[inst] == 0 ? 0.0
					: 100.0 * frame.missKinds[inst][(int)cachesim::MissKind::Conflict]
							/ frame.misses[inst]);
	const int data = (int)cachesim::Stream::Data;
	if (cachesim::dataFeed())
		ImGui::Text("ocache: %" PRIu64 " misses/frame, %.1f%% conflict",
				frame.misses[data],
				frame.misses[data] == 0 ? 0.0
						: 100.0 * frame.missKinds[data][(int)cachesim::MissKind::Conflict]
								/ frame.misses[data]);
	else
		ImGui::TextDisabled("ocache: not measured");
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
	static const char *footer = "estimated issue cycles; ranking is sound, absolutes are not";
	const float footerHeight = ImGui::CalcTextSize(footer, nullptr, false,
			ImGui::GetContentRegionAvail().x).y + ImGui::GetStyle().ItemSpacing.y * 2;

	const int columns = cachesim::dataFeed() ? 5 : 4;
	if (ImGui::BeginTable("##profile", columns,
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit,
			ImVec2(0.f, ImGui::GetContentRegionAvail().y - footerHeight)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("function", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("cycles");
		ImGui::TableSetupColumn("%");
		ImGui::TableSetupColumn("i$");
		if (columns == 5)
			ImGui::TableSetupColumn("d$");
		ImGui::TableHeadersRow();

		for (const cachesim::ProfileRow& row : rows)
		{
			const double total = row.cycles + row.missCycles + row.dataMissCycles;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			// Generated code and unsymbolised ranges are dimmed: they are a
			// place, not a name, and cannot be looked up in any source file
			if (row.named)
				ImGui::TextUnformatted(row.name.c_str());
			else
				ImGui::TextDisabled("%s", row.name.c_str());
			if (ImGui::IsItemHovered())
			{
				// Built line by line rather than as one string with a newline
				// in it, which was rendering with the second line clipped.
				// This form sizes each line explicitly.
				ImGui::BeginTooltip();
				ImGui::Text("%08x-%08x", row.start, row.end);
				ImGui::Text("%.0f calls/frame", row.calls);
				ImGui::EndTooltip();
			}

			ImGui::TableNextColumn();
			ImGui::Text("%.0fk", total / 1000.0);
			ImGui::TableNextColumn();
			ImGui::Text("%.1f", frameCycles == 0 ? 0.0 : 100.0 * total / frameCycles);
			ImGui::TableNextColumn();
			// How much of this row is waiting on the instruction cache, which
			// is the part a code layout change can act on
			if (row.missCycles > 0)
				ImGui::Text("%.0f%%", 100.0 * row.missCycles / total);
			else
				ImGui::TextDisabled("-");

			if (columns == 5)
			{
				ImGui::TableNextColumn();
				// The same for the operand cache: waiting on data rather than
				// on code, which a different kind of change fixes
				if (row.dataMissCycles > 0)
					ImGui::Text("%.0f%%", 100.0 * row.dataMissCycles / total);
				else
					ImGui::TextDisabled("-");
			}
		}
		ImGui::EndTable();
	}

	// Not hardware truth, and it should not need explaining twice
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
	ImGui::TextWrapped("%s", footer);
	ImGui::PopStyleColor();
	ImGui::End();
}

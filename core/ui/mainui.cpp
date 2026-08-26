/*
	Copyright 2020 flyinghead

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

#include "mainui.h"
#include "hw/pvr/Renderer_if.h"
#include "gui.h"
#include "oslib/oslib.h"
#include "wsi/context.h"
#include "cfg/option.h"
#include "emulator.h"
#include "imgui_driver.h"
#include "profiler/fc_profiler.h"
#include "oslib/i18n.h"
#include "hw/sh4/cachesim/cachesim.h"

#include <chrono>
#include <csignal>
#include <thread>

static bool mainui_enabled;
u32 MainFrameCount;
static bool forceReinit;

bool mainui_rend_frame()
{
	FC_PROFILE_SCOPE;

	os_DoEvents();
	os_UpdateInputState();

	if (gui_is_open())
	{
		try {
			gui_display_ui();
		} catch (const FlycastException& e) {
			// Assume this is a graphics API issue
			forceReinit = true;
			return false;
		}
#ifndef TARGET_IPHONE
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
	}
	else
	{
		try {
			if (!emu.render())
				return false;
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
		} catch (const RendererException& e) {
			gui_error(i18n::Ts("Renderer error:") + "\n" + e.what() + "\n\n"
					+ i18n::Ts("The game has been paused but it is recommended to restart Flycast"));
			rend_term_renderer();
			if (!rend_init_renderer())
				ERROR_LOG(RENDERER, "Renderer re-initialization failed");
			gui_open_settings();
			return false;
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
			return false;
		}
	}
	MainFrameCount++;

	return true;
}

void mainui_init()
{
	if (!rend_init_renderer()) {
		ERROR_LOG(RENDERER, "Renderer initialization failed");
		gui_error(i18n::T("Renderer initialization failed.\nPlease select a different graphics API"));
	}
}

void mainui_term()
{
	rend_term_renderer();
}

static volatile std::sig_atomic_t headlessStopRequested;

static void headlessSignalHandler(int sig)
{
	headlessStopRequested = 1;
	// The flag is only checked between frames. Restore the default handler so a
	// second signal kills outright if the current frame never returns.
	std::signal(sig, SIG_DFL);
}

// Runs the emulator with no window, GUI or presentation. The renderer is
// norend, so nothing is drawn.
static void mainui_headless_loop()
{
	if (settings.content.path.empty())
	{
		ERROR_LOG(BOOT, "Headless mode requires a content path");
		return;
	}
	headlessStopRequested = 0;
	std::signal(SIGINT, headlessSignalHandler);
	std::signal(SIGTERM, headlessSignalHandler);

	mainui_init();
	try {
		emu.loadGame(settings.content.path.c_str());
		// Must be after loadGame(): it resets and reloads every option.
		config::ThreadedRendering.set(false);	// nothing presents, so no one to feed
		config::AudioBackend.set("null");
		emu.start();

		using clock = std::chrono::steady_clock;
		const clock::time_point start = clock::now();
		clock::time_point nextProgress = start + std::chrono::seconds(settings.headlessProgress);
		const u32 startFrame = MainFrameCount;

		while (mainui_enabled && !headlessStopRequested && !cachesim::finished() && emu.render())
		{
			MainFrameCount++;
			const u32 frames = MainFrameCount - startFrame;
			if (settings.headlessFrames != 0 && frames >= settings.headlessFrames)
				break;

			if (settings.headlessSeconds != 0 || settings.headlessProgress != 0)
			{
				const clock::time_point now = clock::now();
				const double elapsed = std::chrono::duration<double>(now - start).count();
				if (settings.headlessSeconds != 0 && elapsed >= settings.headlessSeconds)
					break;
				if (settings.headlessProgress != 0 && now >= nextProgress)
				{
					nextProgress = now + std::chrono::seconds(settings.headlessProgress);
					NOTICE_LOG(BOOT, "Headless: %u frames in %.1fs (%.1f fps)",
							frames, elapsed, elapsed == 0 ? 0.0 : frames / elapsed);
					if (cachesim::armed())
						cachesim::logSummary();
				}
			}
		}
		const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
		NOTICE_LOG(BOOT, "Headless: stopped after %u frames in %.1fs (%.1f fps)",
				MainFrameCount - startFrame, elapsed,
				elapsed == 0 ? 0.0 : (MainFrameCount - startFrame) / elapsed);
		if (cachesim::armed())
			cachesim::reportFinal();
		emu.unloadGame();
	} catch (const FlycastException& e) {
		ERROR_LOG(BOOT, "Headless: %s", e.what());
	}
	mainui_term();
}

void mainui_loop(bool forceStart)
{
	ThreadName _("Flycast-rend");
	if (forceStart)
		mainui_enabled = true;
	if (settings.headless)
	{
		mainui_headless_loop();
		return;
	}
	mainui_init();
	RenderType currentRenderer = config::RendererType;

	while (mainui_enabled)
	{
		fc_profiler::startThread("main");

		if (mainui_rend_frame() && imguiDriver != nullptr)
		{
			try {
				imguiDriver->present();
			} catch (const FlycastException& e) {
				forceReinit = true;
			}
		}
		if (imguiDriver == nullptr)
			forceReinit = true;

		if (config::RendererType != currentRenderer || forceReinit)
		{
			mainui_term();
			int prevApi = isOpenGL(currentRenderer) ? 0 : isVulkan(currentRenderer) ? 1 : currentRenderer == RenderType::DirectX9 ? 2 : 3;
			int newApi = isOpenGL(config::RendererType) ? 0 : isVulkan(config::RendererType) ? 1 : config::RendererType == RenderType::DirectX9 ? 2 : 3;
			if (newApi != prevApi || forceReinit)
			{
				try {
					switchRenderApi();
				} catch (const FlycastException& e) {
					ERROR_LOG(RENDERER, "switchRenderApi failed: %s", e.what());
					if (prevApi == newApi)
						// fatal
						throw;
					// try to go back to the previous API
					config::RendererType = currentRenderer;
					try {
						switchRenderApi();
					} catch (const FlycastException& e) {
						ERROR_LOG(RENDERER, "Falling back to previous renderer also failed: %s", e.what());
						// fatal
						throw;
					}
				}
			}
			mainui_init();
			forceReinit = false;
			currentRenderer = config::RendererType;
		}

		fc_profiler::endThread(config::ProfilerFrameWarningTime);
	}

	mainui_term();
}

void mainui_start()
{
	mainui_enabled = true;
}

void mainui_stop()
{
	mainui_enabled = false;
}

void mainui_reinit()
{
	forceReinit = true;
}

/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    In addition, as a special exception, the author gives permission to
*    link the code of this program with the Half-Life Game Engine ("HL
*    Engine") and Modified Game Libraries ("MODs") developed by Valve,
*    L.L.C ("Valve").  You must obey the GNU General Public License in all
*    respects for all of the code used other than the HL Engine and MODs
*    from Valve.  If you modify this file, you may extend this exception
*    to your version of the file, but you are not obligated to do so.  If
*    you do not wish to do so, delete this exception statement from your
*    version.
*
*/

#include "precompiled.h"

bool g_bVGui = false;
char *gpszCvars = nullptr;
bool g_bAppHasBeenTerminated = false;

CSysModule *g_pEngineModule = nullptr;
IDedicatedServerAPI *engineAPI = nullptr;

IFileSystem *g_pFileSystemInterface = nullptr;
CSysModule *g_pFileSystemModule = nullptr;

CreateInterfaceFn g_FilesystemFactoryFn;

void Sys_Printf_Safe(const char *text)
{
	if (sys)
	{
		sys->Printf("%s", text);
	}
}

void Sys_Printf(const char *fmt, ...)
{
	// Dump text to debugging console.
	va_list argptr;
	char szText[1024];

	va_start(argptr, fmt);
	Q_vsnprintf(szText, sizeof(szText), fmt, argptr);
	va_end(argptr);

	// Get Current text and append it.
#ifdef VGUI
	if (g_bVGui)
	{
		VGUIPrintf(szText);
	}
	else
#endif
	{
		console.Print(szText);
	}
}

void ProcessConsoleInput()
{
	if (!engineAPI)
		return;

	const char *inputLine = console.GetLine();
	if (inputLine)
	{
		char szBuf[256];
		Q_snprintf(szBuf, sizeof(szBuf), "%s\n", inputLine);
		engineAPI->AddConsoleText(szBuf);
	}
}

const char *UTIL_GetBaseDir()
{
	return ".";
}

int RunEngine()
{
#ifdef VGUI
	if (g_bVGui)
	{
		// TODO: finish VGUI
		//vgui::ivgui()->SetSleep(0);
	}
#endif

	CreateInterfaceFn engineFactory = Sys_GetFactory(g_pEngineModule);
	RunVGUIFrame();

	if (engineFactory) {
		engineAPI = (IDedicatedServerAPI *)engineFactory(VENGINE_HLDS_API_VERSION, nullptr);
	}

	RunVGUIFrame();
	if (!engineAPI || !engineAPI->Init(UTIL_GetBaseDir(), (char *)CommandLine()->GetCmdLine(), Sys_GetFactoryThis(), g_FilesystemFactoryFn)) {
		return LAUNCHER_ERROR;
	}

#ifdef LAUNCHER_FIXES
	if (engineFactory)
	{
		ISystemModule *pSystemWrapper = (ISystemModule *)engineFactory(BASESYSTEM_INTERFACE_VERSION, nullptr);
		if (pSystemWrapper) {
			console.InitSystem(pSystemWrapper->GetSystem());
		}
	}
#endif // LANUCHER_FIXES

	RunVGUIFrame();

#ifdef VGUI
	// TODO: finish me!
	/*if (g_bVGui) {
		g_pFileSystemInterface->AddSearchPath("platform", "PLATFORM");

		// find our configuration directory
		char szConfigDir[512];
		const char *steamPath = getenv("SteamInstallPath");
		if (steamPath) {
			// put the config dir directly under steam
			Q_snprintf(szConfigDir, sizeof(szConfigDir), "%s/config", steamPath);
		}
		else {
			// we're not running steam, so just put the config dir under the platform
			Q_strlcpy(szConfigDir, "platform/config");
		}
	}*/
#endif // VGUI

	RunVGUIFrame();

	if (gpszCvars) {
		engineAPI->AddConsoleText(gpszCvars);
	}

	VGUIFinishedConfig();
	RunVGUIFrame();

	bool bDone = false;
	// Rate-limit admin console polling. ProcessConsoleInput() ultimately calls
	// select() on stdin via kbhit(), which is a syscall. In high-iteration
	// pingboost modes (4, 5) the unthrottled poll rate (~100k/sec) dominates
	// kernel CPU time. 50ms cadence (~20 polls/sec) is imperceptible for admin
	// input and collapses the syscall overhead by ~5000x.
	static int64_t s_last_console_poll_ns = 0;

#ifdef __linux__
	// KTP Stage C: absolute-time 1ms grid for pingboost 2. Each iteration
	// targets `grid_target` (advanced by exactly 1ms per loop) rather than
	// sleeping a fixed duration from now. clock_nanosleep(TIMER_ABSTIME) sets
	// the hrtimer expiry directly, bypassing the "now + delta" addition that
	// accumulates scheduler jitter in relative nanosleep. Absorbs frame-work
	// time into the next sleep so total cycle ≈ 1ms → ~999 fps at baseline CPU.
	struct timespec grid_target;
	if (g_use_abs_grid) {
		clock_gettime(CLOCK_MONOTONIC, &grid_target);
		// Start one tick in the future.
		grid_target.tv_nsec += 1000000LL;
		if (grid_target.tv_nsec >= 1000000000LL) {
			grid_target.tv_sec++;
			grid_target.tv_nsec -= 1000000000LL;
		}
	}

	// KTP Stage C debug probe (-absgrid_probe). 100µs-bucket histogram of
	// sleep_us / work_us / total_us per iteration, dumped every 10s as
	// [KTP_ABSGRID_PROBE] to stdout (captured by LinuxGSM console.log).
	// Zero overhead when g_absgrid_probe is false. Diagnostic for the
	// 2026-05-11 finding that absgrid measures 696 fps vs. 980 fps that
	// loop math predicts (414µs/iter unaccounted in the engine layer).
	constexpr int PROBE_BUCKETS = 21;          // 0-99, 100-199, ..., 1900-1999, 2000+
	constexpr int64_t PROBE_BUCKET_NS = 100000LL;  // 100µs per bucket
	constexpr int64_t PROBE_DUMP_INTERVAL_NS = 10000000000LL;  // 10s
	static int64_t s_probe_sleep_buckets[PROBE_BUCKETS] = {0};
	static int64_t s_probe_work_buckets[PROBE_BUCKETS]  = {0};
	static int64_t s_probe_total_buckets[PROBE_BUCKETS] = {0};
	static int64_t s_probe_iter_count    = 0;
	static int64_t s_probe_sleep_min_ns  = 0x7FFFFFFFFFFFFFFFLL;
	static int64_t s_probe_sleep_max_ns  = 0;
	static int64_t s_probe_work_min_ns   = 0x7FFFFFFFFFFFFFFFLL;
	static int64_t s_probe_work_max_ns   = 0;
	static int64_t s_probe_total_min_ns  = 0x7FFFFFFFFFFFFFFFLL;
	static int64_t s_probe_total_max_ns  = 0;
	static int64_t s_probe_last_dump_ns  = 0;
	static int64_t s_probe_prev_top_ns   = 0;
#endif

	while (!bDone)
	{
#ifdef __linux__
		// Probe T0: top of iteration (just before sleep). Captured only when
		// the probe flag is on so the steady-state -absgrid path is unchanged.
		struct timespec t_top;
		if (g_absgrid_probe)
			clock_gettime(CLOCK_MONOTONIC, &t_top);

		if (g_use_abs_grid) {
			// Sleep until grid_target. If frame work already overran the target
			// (spike frame), skip the sleep and let the loop catch up naturally
			// over the next few iterations — don't accumulate debt or try to
			// "catch up" any faster than the natural 1ms cadence.
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			bool past_target =
				(now.tv_sec > grid_target.tv_sec) ||
				(now.tv_sec == grid_target.tv_sec && now.tv_nsec >= grid_target.tv_nsec);
			if (!past_target) {
				clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &grid_target, nullptr);
			}
			// Advance grid by 1ms regardless of actual wake time.
			grid_target.tv_nsec += 1000000LL;
			if (grid_target.tv_nsec >= 1000000000LL) {
				grid_target.tv_sec++;
				grid_target.tv_nsec -= 1000000000LL;
			}
		} else {
			sys->Sleep(1);
		}

		// Probe T1: after sleep (start of "work" phase).
		struct timespec t_after_sleep;
		if (g_absgrid_probe)
			clock_gettime(CLOCK_MONOTONIC, &t_after_sleep);
#else
		// Running really fast, yield some time to other apps
		sys->Sleep(1);
#endif

		struct timespec cts;
		clock_gettime(CLOCK_MONOTONIC, &cts);
		int64_t now_ns = (int64_t)cts.tv_sec * 1000000000LL + (int64_t)cts.tv_nsec;
		bool poll_console = (now_ns - s_last_console_poll_ns) >= 50000000LL;
		if (poll_console)
			s_last_console_poll_ns = now_ns;

		if (poll_console)
			Sys_PrepareConsoleInput();

		if (g_bAppHasBeenTerminated)
			break;

#ifdef VGUI
		if (g_bVGui) {
			RunVGUIFrame();
		}
		else
#endif
		if (poll_console) {
			ProcessConsoleInput();
		}

		bDone = !engineAPI->RunFrame();
		sys->UpdateStatus(FALSE /* don't force */);

#ifdef __linux__
		// Probe T2: end of iteration. Compute deltas, update buckets, maybe dump.
		if (g_absgrid_probe) {
			struct timespec t_end;
			clock_gettime(CLOCK_MONOTONIC, &t_end);
			int64_t t_top_ns         = (int64_t)t_top.tv_sec * 1000000000LL + t_top.tv_nsec;
			int64_t t_after_sleep_ns = (int64_t)t_after_sleep.tv_sec * 1000000000LL + t_after_sleep.tv_nsec;
			int64_t t_end_ns         = (int64_t)t_end.tv_sec * 1000000000LL + t_end.tv_nsec;

			int64_t sleep_ns = t_after_sleep_ns - t_top_ns;
			int64_t work_ns  = t_end_ns - t_after_sleep_ns;
			int64_t total_ns = (s_probe_prev_top_ns > 0) ? (t_top_ns - s_probe_prev_top_ns) : 0;
			s_probe_prev_top_ns = t_top_ns;

			auto bucket_idx = [](int64_t ns) -> int {
				if (ns < 0) return 0;
				int idx = (int)(ns / PROBE_BUCKET_NS);
				if (idx >= PROBE_BUCKETS) return PROBE_BUCKETS - 1;
				return idx;
			};
			s_probe_sleep_buckets[bucket_idx(sleep_ns)]++;
			s_probe_work_buckets[bucket_idx(work_ns)]++;
			if (total_ns > 0)
				s_probe_total_buckets[bucket_idx(total_ns)]++;

			if (sleep_ns < s_probe_sleep_min_ns) s_probe_sleep_min_ns = sleep_ns;
			if (sleep_ns > s_probe_sleep_max_ns) s_probe_sleep_max_ns = sleep_ns;
			if (work_ns  < s_probe_work_min_ns)  s_probe_work_min_ns  = work_ns;
			if (work_ns  > s_probe_work_max_ns)  s_probe_work_max_ns  = work_ns;
			if (total_ns > 0) {
				if (total_ns < s_probe_total_min_ns) s_probe_total_min_ns = total_ns;
				if (total_ns > s_probe_total_max_ns) s_probe_total_max_ns = total_ns;
			}
			s_probe_iter_count++;

			if (s_probe_last_dump_ns == 0) s_probe_last_dump_ns = t_end_ns;
			if (t_end_ns - s_probe_last_dump_ns >= PROBE_DUMP_INTERVAL_NS) {
				printf("[KTP_ABSGRID_PROBE] iters=%lld sleep_min_us=%lld sleep_max_us=%lld work_min_us=%lld work_max_us=%lld total_min_us=%lld total_max_us=%lld\n",
					(long long)s_probe_iter_count,
					(long long)(s_probe_sleep_min_ns / 1000),
					(long long)(s_probe_sleep_max_ns / 1000),
					(long long)(s_probe_work_min_ns / 1000),
					(long long)(s_probe_work_max_ns / 1000),
					(long long)(s_probe_total_min_ns / 1000),
					(long long)(s_probe_total_max_ns / 1000));
				printf("[KTP_ABSGRID_PROBE] sleep_us_buckets:");
				for (int i = 0; i < PROBE_BUCKETS; i++)
					if (s_probe_sleep_buckets[i] > 0)
						printf(" [%d-%d)=%lld", i*100, (i+1)*100, (long long)s_probe_sleep_buckets[i]);
				printf("\n[KTP_ABSGRID_PROBE] work_us_buckets:");
				for (int i = 0; i < PROBE_BUCKETS; i++)
					if (s_probe_work_buckets[i] > 0)
						printf(" [%d-%d)=%lld", i*100, (i+1)*100, (long long)s_probe_work_buckets[i]);
				printf("\n[KTP_ABSGRID_PROBE] total_us_buckets:");
				for (int i = 0; i < PROBE_BUCKETS; i++)
					if (s_probe_total_buckets[i] > 0)
						printf(" [%d-%d)=%lld", i*100, (i+1)*100, (long long)s_probe_total_buckets[i]);
				printf("\n");
				fflush(stdout);

				// Reset window.
				for (int i = 0; i < PROBE_BUCKETS; i++) {
					s_probe_sleep_buckets[i] = 0;
					s_probe_work_buckets[i]  = 0;
					s_probe_total_buckets[i] = 0;
				}
				s_probe_iter_count   = 0;
				s_probe_sleep_min_ns = 0x7FFFFFFFFFFFFFFFLL;
				s_probe_sleep_max_ns = 0;
				s_probe_work_min_ns  = 0x7FFFFFFFFFFFFFFFLL;
				s_probe_work_max_ns  = 0;
				s_probe_total_min_ns = 0x7FFFFFFFFFFFFFFFLL;
				s_probe_total_max_ns = 0;
				s_probe_last_dump_ns = t_end_ns;
			}
		}
#endif
	}

#ifdef VGUI
	if (g_bVGui) {
		RunVGUIFrame();
		StopVGUI();
	}
	else
#endif
	{
		sys->DestroyConsoleWindow();
		console.ShutDown();
	}

	int iret = engineAPI->Shutdown();
	VGUIFinishedConfig();

	if (iret == DLL_CLOSE)
		g_bAppHasBeenTerminated = true;

	return LAUNCHER_OK;
}

int StartServer(char* cmdline)
{
	g_bAppHasBeenTerminated = false;

	do {
		CommandLine()->CreateCmdLine(cmdline);
		CommandLine()->AppendParm("-steam", nullptr);

		// Load engine
		g_pEngineModule = Sys_LoadModule(ENGINE_LIB);
		if (!g_pEngineModule) {
			sys->ErrorMessage(0, "Unable to load engine, image is corrupt.");
			return LAUNCHER_ERROR;
		}

		Sys_InitPingboost();
		Sys_WriteProcessIdFile();

		// Load filesystem
		g_pFileSystemModule = Sys_LoadModule(STDIO_FILESYSTEM_LIB);
		if (!g_pFileSystemModule) {
			sys->ErrorMessage(0, "Unable to load filesystem, image is corrupt.");
			return LAUNCHER_ERROR;
		}

		// Get filesystem factory
		g_FilesystemFactoryFn = Sys_GetFactory(g_pFileSystemModule);
		if (!g_FilesystemFactoryFn) {
			sys->ErrorMessage(0, "Unable to get filesystem factory.");
			return LAUNCHER_ERROR;
		}

		// Retrieve filesystem interface
		IFileSystem *pFullFileSystem = (IFileSystem *)g_FilesystemFactoryFn(FILESYSTEM_INTERFACE_VERSION, nullptr);
		if (!pFullFileSystem) {
			sys->ErrorMessage(0, "Can not retrive filesystem interface version '" FILESYSTEM_INTERFACE_VERSION "'.\n");
			return LAUNCHER_ERROR;
		}

		// Mount filesystem
		pFullFileSystem->Mount();

		// Init VGUI or Console mode
#ifdef VGUI
		if (!CommandLine()->CheckParm("-console")) {
			g_bVGui = true;
			StartVGUI();
		}
		else
#endif
		{
			if (!console.Init()) {
				sys->ErrorMessage(0, "Failed to initialize console."); // TODO: add more details
				return LAUNCHER_ERROR;
			}

			if (!sys->CreateConsoleWindow()) {
				sys->ErrorMessage(0, "Failed to create console window.");
				return LAUNCHER_ERROR;
			}

			if (!Sys_SetupConsole()) {
				return LAUNCHER_ERROR;
			}
		}

#ifdef VGUI
		// TODO: finish me!
		/*// run vgui
		if (g_bVGui)
		{
			while (VGUIIsInConfig()	&& VGUIIsRunning()) {
				RunVGUIFrame();
			}
		}
		else*/
#endif
		{
			int ret = RunEngine();
			if (ret == LAUNCHER_ERROR) {
				sys->ErrorMessage(0, "Failed to launch engine.\n");
				return LAUNCHER_ERROR;
			}
		}

		if (gpszCvars)
			free(gpszCvars);

		// Unmount filesystem
		pFullFileSystem->Unmount();

		// Unload modules
		Sys_UnloadModule(g_pFileSystemModule);
		Sys_UnloadModule(g_pEngineModule);

	} while (!g_bAppHasBeenTerminated);

	return LAUNCHER_OK;
}

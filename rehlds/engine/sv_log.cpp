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
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#ifndef _WIN32
#include <pthread.h>
#endif

LOGLIST_T *firstLog;

cvar_t mp_logecho = { "mp_logecho", "1", 0, 0.0f, NULL };
cvar_t mp_logfile = { "mp_logfile", "1", FCVAR_SERVER, 0.0f, NULL };

// KTP: async log writer. The synchronous FS_FPrintf below can block the game
// thread for ~165ms when the SSD stalls a journal commit (fio: ~20ms median
// fdatasync on the affected consumer drives). A dedicated thread owns the log
// FILE* via plain stdio — never the engine FS layer, which isn't thread-safe
// against main-thread map loads. The game thread only enqueues; a full queue
// drops the line and counts it rather than ever blocking.
// Mode is chosen per log-file session at Log_Open (ktp_log_async).
cvar_t ktp_log_async = { "ktp_log_async", "1", 0, 0.0f, NULL };

std::atomic<uint32> g_ktp_logq_drops(0);      // lines lost (queue full / writer has no file / write error); lifetime
std::atomic<uint32> g_ktp_fileq_worst_us(0);  // worst single write on the writer thread; profiler resets per interval

enum ktp_logop_t
{
	KTP_LOGOP_WRITE,
	KTP_LOGOP_OPEN,   // data = resolved on-disk path
	KTP_LOGOP_CLOSE,
};

// Must hold Log_Printf's full formatting buffer (string[1024] below) — the
// static_assert inside Log_Printf enforces the coupling.
struct ktp_logop_s
{
	int type;
	char data[1024];
};

// Power-of-two ring, ~2MB static. A 165ms stall backlogs only tens of lines.
#define KTP_LOGQ_SIZE 2048
#define KTP_LOGQ_MASK (KTP_LOGQ_SIZE - 1)

static ktp_logop_s s_ktpLogQ[KTP_LOGQ_SIZE];
static int s_ktpLogQHead;  // main thread fills
static int s_ktpLogQTail;  // writer thread drains
static std::mutex s_ktpLogMx;
static std::condition_variable s_ktpLogCv;
static bool s_ktpLogThreadRunning;
static bool s_ktpLogStop;
static bool s_ktpLogAsyncActive;  // async session open; g_psvs.log.file stays NULL in this mode
// pthread/CreateThread rather than std::thread: the engine builds with
// -fno-exceptions, so a failed std::thread constructor would terminate the
// process; these report failure by return value instead.
#ifdef _WIN32
static HANDLE s_ktpLogThreadHandle;
#else
static pthread_t s_ktpLogThreadId;
#endif

static void KTP_LogWriterLoop()
{
	FILE *fp = nullptr;  // writer-owned; the main thread never sees this handle
	bool needFlush = false;
	std::unique_lock<std::mutex> lk(s_ktpLogMx);
	for (;;)
	{
		while (s_ktpLogQHead == s_ktpLogQTail && !s_ktpLogStop)
		{
			if (needFlush && fp)
			{
				// Flush only once the queue drains, and off-lock — a stalled
				// flush must never block the game thread's enqueue.
				lk.unlock();
				fflush(fp);
				lk.lock();
				needFlush = false;
				continue;
			}
			s_ktpLogCv.wait(lk);
		}
		if (s_ktpLogQHead == s_ktpLogQTail && s_ktpLogStop)
			break;

		ktp_logop_s op = s_ktpLogQ[s_ktpLogQTail];
		s_ktpLogQTail = (s_ktpLogQTail + 1) & KTP_LOGQ_MASK;
		lk.unlock();

		switch (op.type)
		{
		case KTP_LOGOP_OPEN:
			if (fp)
				fclose(fp);
			// Log_Open already created/truncated the file through the FS layer;
			// append from byte 0.
			fp = fopen(op.data, "a");
			// Line-buffered: one write() per line reaches the kernel, so a crash
			// loses at most the in-flight line — matches the old sync guarantee.
			// The stall this change fixes is the *kernel-side* block, which now
			// lands here instead of the game thread.
			if (fp)
				setvbuf(fp, nullptr, _IOLBF, 0);
			needFlush = false;
			break;
		case KTP_LOGOP_CLOSE:
			if (fp)
			{
				fclose(fp);
				fp = nullptr;
			}
			needFlush = false;
			break;
		case KTP_LOGOP_WRITE:
			if (fp)
			{
				auto ktp_w0 = std::chrono::steady_clock::now();
				if (fputs(op.data, fp) < 0)
				{
					// Disk full / EIO: close so subsequent lines hit the counted
					// no-file branch instead of silently no-oping forever.
					fclose(fp);
					fp = nullptr;
					g_ktp_logq_drops.fetch_add(1, std::memory_order_relaxed);
					needFlush = false;
					break;
				}
				needFlush = true;
				uint32 us = (uint32)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - ktp_w0).count();
				if (us > g_ktp_fileq_worst_us.load(std::memory_order_relaxed))
					g_ktp_fileq_worst_us.store(us, std::memory_order_relaxed);
			}
			else
				g_ktp_logq_drops.fetch_add(1, std::memory_order_relaxed);
			break;
		}

		lk.lock();
	}
	if (fp)
		fclose(fp);
}

static void KTP_LogEnqueue(int type, const char *data)
{
	{
		std::lock_guard<std::mutex> lk(s_ktpLogMx);
		int next = (s_ktpLogQHead + 1) & KTP_LOGQ_MASK;
		if (next == s_ktpLogQTail)
		{
			g_ktp_logq_drops.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		ktp_logop_s *op = &s_ktpLogQ[s_ktpLogQHead];
		op->type = type;
		Q_snprintf(op->data, sizeof(op->data), "%s", data ? data : "");
		s_ktpLogQHead = next;
	}
	s_ktpLogCv.notify_one();
}

#ifdef _WIN32
static DWORD WINAPI KTP_LogWriterThreadMain(LPVOID)
{
	KTP_LogWriterLoop();
	return 0;
}
#else
static void *KTP_LogWriterThreadMain(void *)
{
	KTP_LogWriterLoop();
	return nullptr;
}
#endif

// Returns FALSE if the thread can't be created (resource exhaustion) — caller
// must fall back to synchronous logging for the session, not crash.
static qboolean KTP_LogAsyncEnsureThread()
{
	if (s_ktpLogThreadRunning)
		return TRUE;
#ifdef _WIN32
	s_ktpLogThreadHandle = CreateThread(0, 0, KTP_LogWriterThreadMain, 0, 0, nullptr);
	if (!s_ktpLogThreadHandle)
#else
	if (pthread_create(&s_ktpLogThreadId, nullptr, KTP_LogWriterThreadMain, nullptr) != 0)
#endif
	{
		Con_Printf("KTP: async log writer thread creation failed, falling back to synchronous logging\n");
		return FALSE;
	}
	s_ktpLogThreadRunning = true;
	return TRUE;
}

// Host_Shutdown only: drains the queue (final Log_Close is already enqueued)
// and joins, so the last lines hit disk before the FS/process goes away.
void KTP_Log_AsyncShutdown(void)
{
	if (!s_ktpLogThreadRunning)
		return;
	{
		std::lock_guard<std::mutex> lk(s_ktpLogMx);
		s_ktpLogStop = true;
	}
	s_ktpLogCv.notify_one();
#ifdef _WIN32
	WaitForSingleObject(s_ktpLogThreadHandle, INFINITE);
	CloseHandle(s_ktpLogThreadHandle);
	s_ktpLogThreadHandle = nullptr;
#else
	pthread_join(s_ktpLogThreadId, nullptr);
#endif
	s_ktpLogThreadRunning = false;
	s_ktpLogStop = false;
}

// KTP: I/O timing for the profiler — log lines emitted from entity thinks are
// a suspect for the fleet's sporadic entloop spikes (pty/disk backpressure).
extern bool g_ktp_profiling_enabled;
extern double g_ktp_logio_frame;  // accumulated Log_Printf time this frame
extern double g_ktp_logio_worst;  // worst single Log_Printf this interval
// Split of logio by sink, to find which one blocks on a spike:
extern double g_ktp_logaddr_io_frame;  // Netchan_OutOfBandPrint UDP sendto (logaddress)
extern double g_ktp_file_io_frame;     // FS_FPrintf to qconsole.log (disk)
extern double g_ktp_logaddr_io_worst;
extern double g_ktp_file_io_worst;

void Log_Printf(const char *fmt, ...)
{
	va_list argptr;
	char string[1024];
	time_t ltime;
	tm *today;
	LOGLIST_T *list;

	static_assert(sizeof(string) <= sizeof(ktp_logop_s::data), "async log queue slot smaller than Log_Printf buffer");

	if (!g_psvs.log.net_log_ && !firstLog && !g_psvs.log.active)
		return;

	double ktp_log_t0 = 0.0;
	bool ktp_log_prof = g_ktp_profiling_enabled;
	if (ktp_log_prof) ktp_log_t0 = Sys_FloatTime();

	time(&ltime);
	today = localtime(&ltime);

	va_start(argptr, fmt);
	Q_snprintf(string,sizeof(string), "L %02i/%02i/%04i - %02i:%02i:%02i: ",
		today->tm_mon + 1,
		today->tm_mday,
		today->tm_year + 1900,
		today->tm_hour,
		today->tm_min,
		today->tm_sec);

	Q_vsnprintf(&string[Q_strlen(string)], sizeof(string) - Q_strlen(string), fmt, argptr);
	va_end(argptr);

#ifdef REHLDS_FLIGHT_REC
	FR_Log("REHLDS_LOG", string);
#endif

	if (g_psvs.log.net_log_ || firstLog != NULL)
	{
		double ktp_addr_t0 = ktp_log_prof ? Sys_FloatTime() : 0.0;
		if (g_psvs.log.net_log_)
			Netchan_OutOfBandPrint(NS_SERVER, g_psvs.log.net_address_, "log %s", string);

		for (list = firstLog; list != NULL; list = list->next)
		{
			if (sv_logsecret.value == 0.0f)
				Netchan_OutOfBandPrint(NS_SERVER, list->log.net_address_, "log %s", string);

			else Netchan_OutOfBandPrint(NS_SERVER, list->log.net_address_, "%c%s%s", S2A_LOGKEY, sv_logsecret.string, string);
		}
		if (ktp_log_prof)
		{
			double ktp_addr_dt = Sys_FloatTime() - ktp_addr_t0;
			g_ktp_logaddr_io_frame += ktp_addr_dt;
			if (ktp_addr_dt > g_ktp_logaddr_io_worst)
				g_ktp_logaddr_io_worst = ktp_addr_dt;
		}
	}
	if (g_psvs.log.active && (g_psvs.maxclients > 1 || sv_log_singleplayer.value != 0.0f))
	{
		if (mp_logecho.value != 0.0f)
			Con_Printf("%s", string);

		if (g_psvs.log.file || s_ktpLogAsyncActive)
		{
			if (mp_logfile.value != 0.0f)
			{
				double ktp_file_t0 = ktp_log_prof ? Sys_FloatTime() : 0.0;
				// Async: µs enqueue — the disk stall lives on the writer thread.
				if (s_ktpLogAsyncActive)
					KTP_LogEnqueue(KTP_LOGOP_WRITE, string);
				else
					FS_FPrintf((FileHandle_t)g_psvs.log.file, "%s", string);
				if (ktp_log_prof)
				{
					double ktp_file_dt = Sys_FloatTime() - ktp_file_t0;
					g_ktp_file_io_frame += ktp_file_dt;
					if (ktp_file_dt > g_ktp_file_io_worst)
						g_ktp_file_io_worst = ktp_file_dt;
				}
			}
		}
	}

	if (ktp_log_prof)
	{
		double ktp_log_dt = Sys_FloatTime() - ktp_log_t0;
		g_ktp_logio_frame += ktp_log_dt;
		if (ktp_log_dt > g_ktp_logio_worst)
			g_ktp_logio_worst = ktp_log_dt;
	}
}

void Log_PrintServerVars(void)
{
	cvar_t *var;
	if (!g_psvs.log.active)
		return;

	Log_Printf("Server cvars start\n");
	for (var = cvar_vars; var != NULL; var = var->next)
	{
		if (var->flags & FCVAR_SERVER)
			Log_Printf("Server cvar \"%s\" = \"%s\"\n", var->name, var->string);
	}
	Log_Printf("Server cvars end\n");
}

void Log_Close(void)
{
	if (g_psvs.log.file || s_ktpLogAsyncActive)
		Log_Printf("Log file closed\n");

	if (g_psvs.log.file)
		FS_Close((FileHandle_t)g_psvs.log.file);
	g_psvs.log.file = NULL;

	if (s_ktpLogAsyncActive)
	{
		// Queued behind the "closed" line above; the writer fcloses in order.
		KTP_LogEnqueue(KTP_LOGOP_CLOSE, NULL);
		s_ktpLogAsyncActive = false;
	}
}

void Log_Open(void)
{
	time_t ltime;
	struct tm *today;
	char szFileBase[MAX_PATH];
	char szTestFile[MAX_PATH+8]; // room for extra string
	int i;
	FileHandle_t fp;
	char *temp;

	if (!g_psvs.log.active || (sv_log_onefile.value != 0.0f && (g_psvs.log.file || s_ktpLogAsyncActive)))
		return;

	if (mp_logfile.value == 0.0f)
		Con_Printf("Server logging data to console.\n");
	else
	{
		Log_Close();
		time(&ltime);
		today = localtime(&ltime);

		temp = Cvar_VariableString("logsdir");

		if (!temp || Q_strlen(temp) <= 0 || Q_strstr(temp, ":") || Q_strstr(temp, ".."))
			Q_snprintf(szFileBase, sizeof(szFileBase), "logs/L%02i%02i", today->tm_mon + 1, today->tm_mday);

		else Q_snprintf(szFileBase, sizeof(szFileBase), "%s/L%02i%02i", temp, today->tm_mon + 1, today->tm_mday);

		for (i = 0; i < 1000; i++)
		{
			Q_snprintf(szTestFile, sizeof(szTestFile), "%s%03i.log", szFileBase, i);

			COM_FixSlashes(szTestFile);
			COM_CreatePath(szTestFile);

			fp = FS_OpenPathID(szTestFile, "r", "GAMECONFIG");
			if (!fp)
			{
				COM_CreatePath(szTestFile);
				fp = FS_OpenPathID(szTestFile, "wt", "GAMECONFIG");
				if (fp)
				{
					qboolean ktp_async_ok = FALSE;
					if (ktp_log_async.value != 0.0f)
					{
						// The FS layer created/truncated the file (correct search
						// path + dirs); hand its resolved path to the writer
						// thread, which owns the handle from here on.
						char szLocalPath[MAX_PATH * 2];
						FS_Close(fp);
						fp = NULL;
						if (FS_GetLocalPath(szTestFile, szLocalPath, sizeof(szLocalPath)) && KTP_LogAsyncEnsureThread())
						{
							s_ktpLogAsyncActive = true;  // before the "started" line so it lands in-file
							KTP_LogEnqueue(KTP_LOGOP_OPEN, szLocalPath);
							Con_Printf("Server logging data to file %s (async)\n", szTestFile);
							ktp_async_ok = TRUE;
						}
						else
						{
							// Path didn't resolve or no writer thread — fall back to the synchronous handle.
							fp = FS_OpenPathID(szTestFile, "a", "GAMECONFIG");
							if (!fp)
							{
								Con_Printf("Unable to reopen logfile %s\nLogging disabled\n", szTestFile);
								g_psvs.log.active = FALSE;
								return;
							}
						}
					}
					if (!ktp_async_ok)
					{
						g_psvs.log.file = (void *)fp;
						Con_Printf("Server logging data to file %s\n", szTestFile);
					}
					Log_Printf("Log file started (file \"%s\") (game \"%s\") (version \"%i/%s/%d\")\n", szTestFile, Info_ValueForKey(Info_Serverinfo(), "*gamedir"), PROTOCOL_VERSION, gpszVersionString, build_number());
				}
				return;
			}
			FS_Close(fp);
		}
		Con_Printf("Unable to open logfiles under %s\nLogging disabled\n", szFileBase);
		g_psvs.log.active = FALSE;
	}
}

void SV_SetLogAddress_f(void)
{
	const char *s;
	int nPort;
	char szAdr[MAX_PATH];
	netadr_t adr;

	if (Cmd_Argc() != 3)
	{
		Con_Printf("logaddress:  usage\nlogaddress ip port\n");
		if (g_psvs.log.active)
			Con_Printf("current:  %s\n", NET_AdrToString(g_psvs.log.net_address_));
		return;
	}

	nPort = Q_atoi(Cmd_Argv(2));
	if (!nPort)
	{
		Con_Printf("logaddress:  must specify a valid port\n");
		return;
	}

	s = Cmd_Argv(1);
	if (!s || *s == '\0')
	{
		Con_Printf("logaddress:  unparseable address\n");
		return;
	}

	Q_snprintf(szAdr, sizeof(szAdr), "%s:%i", s, nPort);

	if (!NET_StringToAdr(szAdr, &adr))
	{
		Con_Printf("logaddress:  unable to resolve %s\n", szAdr);
		return;
	}

	g_psvs.log.net_log_ = TRUE;
	Q_memcpy(&g_psvs.log.net_address_, &adr, sizeof(netadr_t));
	Con_Printf("logaddress:  %s\n", NET_AdrToString(adr));
}

void SV_AddLogAddress_f(void)
{
	const char *s;
	int nPort;
	char szAdr[MAX_PATH];
	netadr_t adr;
	LOGLIST_T *list;
	qboolean found = FALSE;
	LOGLIST_T *tmp;

	if (Cmd_Argc() != 3)
	{
		Con_Printf("logaddress_add:  usage\nlogaddress_add ip port\n");
		for (list = firstLog; list != NULL; list = list->next)
			Con_Printf("current:  %s\n", NET_AdrToString(list->log.net_address_));
		return;
	}

	nPort = Q_atoi(Cmd_Argv(2));
	if (!nPort)
	{
		Con_Printf("logaddress_add:  must specify a valid port\n");
		return;
	}

	s = Cmd_Argv(1);
	if (!s || *s == '\0')
	{
		Con_Printf("logaddress_add:  unparseable address\n");
		return;
	}
	Q_snprintf(szAdr, sizeof(szAdr), "%s:%i", s, nPort);

	if (!NET_StringToAdr(szAdr, &adr))
	{
		Con_Printf("logaddress_add:  unable to resolve %s\n", szAdr);
		return;
	}

	if (firstLog)
	{
		for (list = firstLog; list != NULL; list = list->next)
		{
#ifdef REHLDS_FIXES
			//for IPX support
			if (NET_CompareAdr(adr, list->log.net_address_))
#else
			if (Q_memcmp(adr.ip, list->log.net_address_.ip, 4) == 0 && adr.port == list->log.net_address_.port)
#endif // REHLDS_FIXES
			{
				found = TRUE;
				break;
			}
		}
		if (found)
		{
			Con_Printf("logaddress_add:  address already in list\n");
			return;
		}
		tmp = (LOGLIST_T *)Mem_Malloc(sizeof(LOGLIST_T));
		if (!tmp)
		{
			Con_Printf("logaddress_add:  error allocating new node\n");
			return;
		}

		tmp->next = NULL;
		Q_memcpy(&tmp->log.net_address_, &adr, sizeof(netadr_t));

		list = firstLog;

		while (list->next)
			list = list->next;

		list->next = tmp;
	}
	else
	{
		firstLog = (LOGLIST_T *)Mem_Malloc(sizeof(LOGLIST_T));
		if (!firstLog)
		{
			Con_Printf("logaddress_add:  error allocating new node\n");
			return;
		}
		firstLog->next = NULL;
		Q_memcpy(&firstLog->log.net_address_, &adr, sizeof(netadr_t));
	}

	Con_Printf("logaddress_add:  %s\n", NET_AdrToString(adr));
}

void SV_DelLogAddress_f(void)
{
	const char *s;
	int nPort;
	char szAdr[MAX_PATH];
	netadr_t adr;
	LOGLIST_T *list;
	LOGLIST_T *prev;
	qboolean found = FALSE;

	if (Cmd_Argc() != 3)
	{
		Con_Printf("logaddress_del:  usage\nlogaddress_del ip port\n");
		for (list = firstLog; list != NULL; list = list->next)
			Con_Printf("current:  %s\n", NET_AdrToString(list->log.net_address_));
		return;
	}
	nPort = Q_atoi(Cmd_Argv(2));
	if (!nPort)
	{
		Con_Printf("logaddress_del:  must specify a valid port\n");
		return;
	}

	s = Cmd_Argv(1);
	if (!s || *s == '\0')
	{
		Con_Printf("logaddress_del:  unparseable address\n");
		return;
	}
	Q_snprintf(szAdr, sizeof(szAdr), "%s:%i", s, nPort);
	if (!NET_StringToAdr(szAdr,&adr))
	{
		Con_Printf("logaddress_del:  unable to resolve %s\n", szAdr);
		return;
	}
	if (!firstLog)
	{
		Con_Printf("logaddress_del:  No addresses added yet\n");
		return;
	}
	for(list = firstLog, prev = firstLog; list != NULL; list = list->next)
	{
#ifdef REHLDS_FIXES
		//for IPX
		if (NET_CompareAdr(adr,list->log.net_address_))
#else
		if (Q_memcmp(adr.ip, list->log.net_address_.ip, 4) == 0 && adr.port == list->log.net_address_.port)
#endif // REHLDS_FIXES
		{
			found = TRUE;
			if (list == prev)
			{
				firstLog = prev->next;
				Mem_Free(prev);
			}
			else
			{
				prev->next = list->next;
				Mem_Free(list);
			}
			break;
		}
		prev = list;
	}
	if (!found)
	{
		Con_Printf("logaddress_del:  Couldn't find address in list\n");
		return;
	}
	Con_Printf("deleting:  %s\n", NET_AdrToString(adr));
}

void SV_ServerLog_f(void)
{
	if (Cmd_Argc() != 2)
	{
		Con_Printf("usage:  log < on | off >\n");

		if (g_psvs.log.active)
			Con_Printf("currently logging\n");

		else Con_Printf("not currently logging\n");
		return;
	}

	const char *s = Cmd_Argv(1);
	if (Q_stricmp(s, "off"))
	{
		if (Q_stricmp(s, "on"))
			Con_Printf("log:  unknown parameter %s, 'on' and 'off' are valid\n", s);
		else
		{
			g_psvs.log.active = TRUE;
			Log_Open();
		}
	}
	else if (g_psvs.log.active)
	{
		Log_Close();
		Con_Printf("Server logging disabled.\n");
		g_psvs.log.active = FALSE;
	}
}

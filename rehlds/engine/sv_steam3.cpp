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
#include <thread>
#include <chrono>
#include <atomic>

// KTP: Access profiling cvars from sv_main.cpp
extern cvar_t ktp_profile_frame;
extern cvar_t ktp_profile_steam_detail;

// ============================================================
// KTP: Background Steam Callback Thread
// Moves SteamGameServer_RunCallbacks() IPC blocking off the main
// game thread. Callbacks are queued and processed on the main
// thread during RunFrame() — all game state access stays safe.
// ============================================================

enum SteamCallbackType {
	CB_GSClientApprove,
	CB_GSClientDeny,
	CB_GSClientKick,
	CB_GSPolicyResponse,
	CB_LogonSuccess,
	CB_LogonFailure
};

struct QueuedCallback {
	SteamCallbackType type;
	// Raw storage for callback data — avoids non-trivial union member issues.
	// Largest struct is GSClientDeny_t at ~140 bytes.
	char data[256];

	template<typename T>
	void Store(SteamCallbackType t, const T *src) {
		type = t;
		static_assert(sizeof(T) <= sizeof(data), "Callback struct too large for queue storage");
		Q_memcpy(data, src, sizeof(T));
	}

	template<typename T>
	T* Get() { return reinterpret_cast<T*>(data); }
};

static const int CALLBACK_QUEUE_SIZE = 256;
static QueuedCallback s_callbackQueue[CALLBACK_QUEUE_SIZE];
static std::atomic<int> s_queueHead{0};
static std::atomic<int> s_queueTail{0};
static std::thread* s_steamThread = nullptr;
static std::atomic<bool> s_steamThreadRunning{false};

// ============================================================
// KTP: Main → background "refresh server details" work slot.
// The 5s periodic block in RunFrame used to do SetServerName /
// SetMaxPlayerCount / SetBotPlayerCount / SetMapName / SetGameDescription
// / SetGameTags / BUpdateUserData(...) per client inline on the main
// frame — Steam API calls total 3-6ms, and those spikes account for
// ~26% of fleet spike volume post-913. Move that work to the existing
// Steam background thread. Main thread now snapshots state (~20µs of
// cheap reads) and flips a flag; bg thread publishes to Steam API.
//
// Single-slot, race-free without a lock: main writes snapshot only
// when pending==false; bg reads only when pending==true; acquire/release
// ordering makes the write visible across threads. If bg hasn't
// processed the previous one by the next 5s fire, we skip — the next
// interval's snapshot will carry fresh data, no accumulation needed.
// ============================================================
struct SteamRefreshSnapshot {
	int maxPlayers;
	int botCount;
	bool hasPW;
	bool hasGameTags;
	char hostname[64];
	char mapname[64];
	char gameDesc[128];
	char gameTags[128];
	int numClients;
	struct ClientInfo {
		uint64 steamid;
		char name[32];
		int frags;
	} clients[MAX_CLIENTS];
};

static SteamRefreshSnapshot s_refreshSnapshot;
static std::atomic<bool> s_refreshPending{false};

// Main thread. Snapshot state + flip flag. ~20µs of cheap local reads.
// Skips if previous refresh is still in flight.
static void SteamRefresh_Enqueue()
{
	if (s_refreshPending.load(std::memory_order_acquire))
		return;

	int maxPlayers = sv_visiblemaxplayers.value;
	if (maxPlayers < 0)
		maxPlayers = g_psvs.maxclients;
	s_refreshSnapshot.maxPlayers = maxPlayers;

	int botCount = 0;
	for (int i = 0; i < g_psvs.maxclients; i++)
	{
		auto cl = &g_psvs.clients[i];
		if ((cl->active || cl->spawned || cl->connected) && cl->fakeclient)
			++botCount;
	}
	s_refreshSnapshot.botCount = botCount;

	s_refreshSnapshot.hasPW = (sv_password.string[0] && Q_stricmp(sv_password.string, "none"));

	Q_strncpy(s_refreshSnapshot.hostname, Cvar_VariableString("hostname"), sizeof(s_refreshSnapshot.hostname) - 1);
	s_refreshSnapshot.hostname[sizeof(s_refreshSnapshot.hostname) - 1] = 0;

	Q_strncpy(s_refreshSnapshot.mapname, g_psv.name, sizeof(s_refreshSnapshot.mapname) - 1);
	s_refreshSnapshot.mapname[sizeof(s_refreshSnapshot.mapname) - 1] = 0;

#ifdef REHLDS_FIXES
	const char *desc = gEntityInterface.pfnGetGameDescription();
	if (desc) {
		Q_strncpy(s_refreshSnapshot.gameDesc, desc, sizeof(s_refreshSnapshot.gameDesc) - 1);
		s_refreshSnapshot.gameDesc[sizeof(s_refreshSnapshot.gameDesc) - 1] = 0;
	} else {
		s_refreshSnapshot.gameDesc[0] = 0;
	}

	if (sv_tags.string[0]) {
		Q_strncpy(s_refreshSnapshot.gameTags, sv_tags.string, sizeof(s_refreshSnapshot.gameTags) - 1);
		s_refreshSnapshot.gameTags[sizeof(s_refreshSnapshot.gameTags) - 1] = 0;
		Q_strlwr(s_refreshSnapshot.gameTags);
		s_refreshSnapshot.hasGameTags = true;
	} else {
		s_refreshSnapshot.hasGameTags = false;
		s_refreshSnapshot.gameTags[0] = 0;
	}
#else
	s_refreshSnapshot.gameDesc[0] = 0;
	s_refreshSnapshot.gameTags[0] = 0;
	s_refreshSnapshot.hasGameTags = false;
#endif

	int n = 0;
	for (int i = 0; i < g_psvs.maxclients && n < MAX_CLIENTS; i++)
	{
		client_t *cl = &g_psvs.clients[i];
		if (!cl->active) continue;
		// Defensive: transitional windows during map change / edict reuse can
		// leave cl->active set while cl->edict is in an invalid state. Skipping
		// the row is safe — the next 5s refresh will pick them up.
		if (!cl->edict || cl->edict->free) continue;
		auto &ci = s_refreshSnapshot.clients[n++];
		ci.steamid = cl->network_userid.m_SteamID;
		Q_strncpy(ci.name, cl->name, sizeof(ci.name) - 1);
		ci.name[sizeof(ci.name) - 1] = 0;
		ci.frags = cl->edict->v.frags;
	}
	s_refreshSnapshot.numClients = n;

	s_refreshPending.store(true, std::memory_order_release);
}

// Background thread. Publishes the snapshot to Steam API then clears flag.
// Up to 50ms latency from enqueue (the Steam_ThreadFunc loop period); negligible
// vs the 5000ms interval at which refreshes are produced.
static void SteamRefresh_ProcessIfPending()
{
	if (!s_refreshPending.load(std::memory_order_acquire))
		return;

	auto steam = CRehldsPlatformHolder::get()->SteamGameServer();
	steam->SetMaxPlayerCount(s_refreshSnapshot.maxPlayers);
	steam->SetBotPlayerCount(s_refreshSnapshot.botCount);
	steam->SetServerName(s_refreshSnapshot.hostname);
	steam->SetMapName(s_refreshSnapshot.mapname);
	steam->SetPasswordProtected(s_refreshSnapshot.hasPW);

#ifdef REHLDS_FIXES
	if (s_refreshSnapshot.gameDesc[0])
		steam->SetGameDescription(s_refreshSnapshot.gameDesc);
	if (s_refreshSnapshot.hasGameTags)
		steam->SetGameTags(s_refreshSnapshot.gameTags);
#endif

	// NOTE: Must use the direct SteamGameServer->BUpdateUserData() here even in
	// REHLDS_FIXES builds. The ISteamGameServer_BUpdateUserData() helper routes
	// through m_Steam_GSBUpdateUserData hookchain, which plugins (KTP-ReAPI →
	// AMXX forwards) register handlers on. Those handlers touch main-thread-only
	// state (g_psvs.clients, AMX VM, etc.) and are NOT safe from this background
	// thread. Bypassing the hook preserves main-thread safety; plugins that want
	// to observe score updates still fire from the main-thread call sites
	// (ProcessLogonSuccess / NotifyOfLevelChange) which remain synchronous.
	for (int i = 0; i < s_refreshSnapshot.numClients; i++)
	{
		auto &ci = s_refreshSnapshot.clients[i];
		steam->BUpdateUserData(ci.steamid, ci.name, ci.frags);
	}

	s_refreshPending.store(false, std::memory_order_release);
}

static void SteamCallbackQueue_Push(const QueuedCallback &cb)
{
	int head = s_queueHead.load(std::memory_order_relaxed);
	int next = (head + 1) % CALLBACK_QUEUE_SIZE;
	if (next == s_queueTail.load(std::memory_order_acquire))
		return; // Queue full — drop callback (should never happen)
	s_callbackQueue[head] = cb;
	s_queueHead.store(next, std::memory_order_release);
}

static void Steam_ThreadFunc()
{
	char szOutBuf[4096];

	while (s_steamThreadRunning.load(std::memory_order_relaxed))
	{
		// IPC callbacks — this is the main blocking call (3-13ms)
		CRehldsPlatformHolder::get()->SteamGameServer_RunCallbacks();

		// Drain outgoing Steam packets (master server heartbeats, auth responses)
		// NET_SendPacket uses sendto() which is atomic on Linux UDP sockets —
		// safe to call from background thread concurrently with game traffic.
		uint16 port;
		uint32 ip;
		int iLen = CRehldsPlatformHolder::get()->SteamGameServer()->GetNextOutgoingPacket(szOutBuf, sizeof(szOutBuf), &ip, &port);
		while (iLen > 0)
		{
			netadr_t netAdr;
			*((uint32*)&netAdr.ip[0]) = htonl(ip);
			netAdr.port = htons(port);
			netAdr.type = NA_IP;

			NET_SendPacket(NS_SERVER, iLen, szOutBuf, netAdr);

			iLen = CRehldsPlatformHolder::get()->SteamGameServer()->GetNextOutgoingPacket(szOutBuf, sizeof(szOutBuf), &ip, &port);
		}

		// KTP: Publish any pending "refresh server details" work that the main
		// 5s-timer enqueued. Off-main Steam API calls — ~3-6ms per cycle here
		// instead of on the game frame path.
		SteamRefresh_ProcessIfPending();

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

// ============================================================
// Callback handlers — called from background thread via Steam API.
// They only enqueue the callback data for main thread processing.
// ============================================================

// KTP: Callback handlers — enqueue only, no game state access
void CSteam3Server::OnGSPolicyResponse(GSPolicyResponse_t *pPolicyResponse)
{
	QueuedCallback cb;
	cb.Store(CB_GSPolicyResponse, pPolicyResponse);
	SteamCallbackQueue_Push(cb);
}

void CSteam3Server::OnLogonSuccess(SteamServersConnected_t *pLogonSuccess)
{
	QueuedCallback cb;
	cb.Store(CB_LogonSuccess, pLogonSuccess);
	SteamCallbackQueue_Push(cb);
}

uint64 CSteam3Server::GetSteamID()
{
	if (m_bLanOnly)
		return CSteamID(0, k_EUniversePublic, k_EAccountTypeInvalid).ConvertToUint64();
	else
		return m_SteamIDGS.ConvertToUint64();
}

void CSteam3Server::OnLogonFailure(SteamServerConnectFailure_t *pLogonFailure)
{
	QueuedCallback cb;
	cb.Store(CB_LogonFailure, pLogonFailure);
	SteamCallbackQueue_Push(cb);
}

void CSteam3Server::OnGSClientDeny(GSClientDeny_t *pGSClientDeny)
{
	QueuedCallback cb;
	cb.Store(CB_GSClientDeny, pGSClientDeny);
	SteamCallbackQueue_Push(cb);
}

void CSteam3Server::OnGSClientDenyHelper(client_t *cl, EDenyReason eDenyReason, const char *pchOptionalText)
{
	switch (eDenyReason)
	{
	case k_EDenyInvalidVersion:
		SV_DropClient(cl, 0, "Client version incompatible with server. \nPlease exit and restart");
		break;

	case k_EDenyNotLoggedOn:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "No Steam logon\n");
		break;

	case k_EDenyLoggedInElseWhere:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "This Steam account is being used in another location\n");
		break;

	case k_EDenyNoLicense:
		SV_DropClient(cl, 0, "This Steam account does not own this game. \nPlease login to the correct Steam account.");
		break;

	case k_EDenyCheater:
		SV_DropClient(cl, 0, "VAC banned from secure server\n");
		break;

	case k_EDenyUnknownText:
		if (pchOptionalText && *pchOptionalText)
			SV_DropClient(cl, 0, pchOptionalText);
		else
			SV_DropClient(cl, 0, "Client dropped by server");
		break;

	case k_EDenyIncompatibleAnticheat:
		SV_DropClient(cl, 0, "You are running an external tool that is incompatible with Secure servers.");
		break;

	case k_EDenyMemoryCorruption:
		SV_DropClient(cl, 0, "Memory corruption detected.");
		break;

	case k_EDenyIncompatibleSoftware:
		SV_DropClient(cl, 0, "You are running software that is not compatible with Secure servers.");
		break;

	case k_EDenySteamConnectionLost:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "Steam connection lost\n");
		break;

	case k_EDenySteamConnectionError:
		if (!m_bLanOnly)
			SV_DropClient(cl, 0, "Unable to connect to Steam\n");
		break;

	case k_EDenySteamResponseTimedOut:
		SV_DropClient(cl, 0, "Client timed out while answering challenge.\n---> Please make sure that you have opened the appropriate ports on any firewall you are connected behind.\n---> See http://support.steampowered.com for help with firewall configuration.");
		break;

	case k_EDenySteamValidationStalled:
		if (m_bLanOnly)
			cl->network_userid.m_SteamID = 1;
		break;

	default:
		SV_DropClient(cl, 0, "Client dropped by server");
		break;
	}
}

void CSteam3Server::OnGSClientApprove(GSClientApprove_t *pGSClientSteam2Accept)
{
	QueuedCallback cb;
	cb.Store(CB_GSClientApprove, pGSClientSteam2Accept);
	SteamCallbackQueue_Push(cb);
}

void CSteam3Server::OnGSClientKick(GSClientKick_t *pGSClientKick)
{
	QueuedCallback cb;
	cb.Store(CB_GSClientKick, pGSClientKick);
	SteamCallbackQueue_Push(cb);
}

client_t *CSteam3Server::ClientFindFromSteamID(CSteamID &steamIDFind)
{
	for (int i = 0; i < g_psvs.maxclients; i++)
	{
		auto cl = &g_psvs.clients[i];
		if (!cl->connected && !cl->active && !cl->spawned)
			continue;

		if (cl->network_userid.idtype != AUTH_IDTYPE_STEAM)
			continue;

		if (steamIDFind == cl->network_userid.m_SteamID)
			return cl;
	}

	return NULL;
}

// ============================================================
// KTP: Main-thread callback processing (dequeued from background thread)
// These contain the ORIGINAL callback handler logic that touches game state.
// ============================================================

void CSteam3Server::ProcessGSPolicyResponse(GSPolicyResponse_t *p)
{
	if (CRehldsPlatformHolder::get()->SteamGameServer()->BSecure())
		Con_Printf("   VAC secure mode is activated.\n");
	else
		Con_Printf("   VAC secure mode disabled.\n");
}

void CSteam3Server::ProcessLogonSuccess(SteamServersConnected_t *p)
{
	if (m_bLogOnResult)
	{
		if (!m_bLanOnly)
			Con_Printf("Reconnected to Steam servers.\n");
	}
	else
	{
		m_bLogOnResult = true;
		if (!m_bLanOnly)
			Con_Printf("Connection to Steam servers successful.\n");
	}

	m_SteamIDGS = CRehldsPlatformHolder::get()->SteamGameServer()->GetSteamID();
	CSteam3Server::SendUpdatedServerDetails();
}

void CSteam3Server::ProcessLogonFailure(SteamServerConnectFailure_t *p)
{
	if (!m_bLogOnResult)
	{
		if (p->m_eResult == k_EResultServiceUnavailable)
		{
			if (!m_bLanOnly)
			{
				Con_Printf("Connection to Steam servers successful (SU).\n");
				if (m_bWantToBeSecure)
				{
					Con_Printf("   VAC secure mode not available.\n");
					m_bLogOnResult = true;
					return;
				}
			}
		}
		else
		{
			if (!m_bLanOnly)
				Con_Printf("Could not establish connection to Steam servers.\n");
		}
	}

	m_bLogOnResult = true;
}

void CSteam3Server::ProcessGSClientDeny(GSClientDeny_t *p)
{
	client_t* cl = CSteam3Server::ClientFindFromSteamID(p->m_SteamID);
	if (cl)
		OnGSClientDenyHelper(cl, p->m_eDenyReason, p->m_rgchOptionalText);
}

void CSteam3Server::ProcessGSClientApprove(GSClientApprove_t *p)
{
	client_t* cl = ClientFindFromSteamID(p->m_SteamID);
	if (!cl)
		return;

	if (SV_FilterUser(&cl->network_userid))
	{
		char msg[256];
		Q_sprintf(msg, "You have been banned from this server\n");
		SV_RejectConnection(&cl->netchan.remote_address, msg);
		SV_DropClient(cl, 0, "STEAM UserID %s is in server ban list\n", SV_GetClientIDString(cl));
	}
	else if (SV_CheckForDuplicateSteamID(cl) != -1)
	{
		char msg[256];
		Q_sprintf(msg, "Your UserID is already in use on this server.\n");
		SV_RejectConnection(&cl->netchan.remote_address, msg);
		SV_DropClient(cl, 0, "STEAM UserID %s is already\nin use on this server\n", SV_GetClientIDString(cl));
	}
	else
	{
		char msg[512];
		Q_snprintf(msg, ARRAYSIZE(msg), "\"%s<%i><%s><>\" STEAM USERID validated\n", cl->name, cl->userid, SV_GetClientIDString(cl));
#ifdef REHLDS_CHECKS
		msg[ARRAYSIZE(msg) - 1] = 0;
#endif
		Con_DPrintf("%s", msg);
		Log_Printf("%s", msg);
	}
}

void CSteam3Server::ProcessGSClientKick(GSClientKick_t *p)
{
	client_t* cl = CSteam3Server::ClientFindFromSteamID(p->m_SteamID);
	if (cl)
		CSteam3Server::OnGSClientDenyHelper(cl, p->m_eDenyReason, 0);
}

void CSteam3Server::DrainCallbackQueue()
{
	int tail = s_queueTail.load(std::memory_order_relaxed);
	int head = s_queueHead.load(std::memory_order_acquire);

	while (tail != head)
	{
		QueuedCallback &cb = s_callbackQueue[tail];
		switch (cb.type)
		{
		case CB_GSClientApprove:  ProcessGSClientApprove(cb.Get<GSClientApprove_t>()); break;
		case CB_GSClientDeny:     ProcessGSClientDeny(cb.Get<GSClientDeny_t>()); break;
		case CB_GSClientKick:     ProcessGSClientKick(cb.Get<GSClientKick_t>()); break;
		case CB_GSPolicyResponse: ProcessGSPolicyResponse(cb.Get<GSPolicyResponse_t>()); break;
		case CB_LogonSuccess:     ProcessLogonSuccess(cb.Get<SteamServersConnected_t>()); break;
		case CB_LogonFailure:     ProcessLogonFailure(cb.Get<SteamServerConnectFailure_t>()); break;
		}
		tail = (tail + 1) % CALLBACK_QUEUE_SIZE;
	}
	s_queueTail.store(tail, std::memory_order_release);
}

CSteam3Server::CSteam3Server() :
	m_CallbackGSClientApprove(this, &CSteam3Server::OnGSClientApprove),
	m_CallbackGSClientDeny(this, &CSteam3Server::OnGSClientDeny),
	m_CallbackGSClientKick(this, &CSteam3Server::OnGSClientKick),
	m_CallbackGSPolicyResponse(this, &CSteam3Server::OnGSPolicyResponse),
	m_CallbackLogonSuccess(this, &CSteam3Server::OnLogonSuccess),
	m_CallbackLogonFailure(this, &CSteam3Server::OnLogonFailure),
	m_SteamIDGS(1, 0, k_EUniverseInvalid, k_EAccountTypeInvalid)
{
#ifdef REHLDS_FIXES
	m_GameTagsData[0] = '\0';
#endif

	m_bHasActivePlayers = false;
	m_bWantToBeSecure = false;
	m_bLanOnly = false;
}

void CSteam3Server::Activate()
{
	bool bLanOnly;
	int argSteamPort;
	EServerMode eSMode;
	int gamePort;
	char gamedir[MAX_PATH];
	int usSteamPort;
	uint32 unIP;

	if (m_bLoggedOn)
	{
		bLanOnly = sv_lan.value != 0.0;
		if (m_bLanOnly != bLanOnly)
		{
			m_bLanOnly = bLanOnly;
			m_bWantToBeSecure = !COM_CheckParm("-insecure") && !bLanOnly;
		}
	}
	else
	{
		m_bLoggedOn = true;
		unIP = 0;
		usSteamPort = 26900;
		argSteamPort = COM_CheckParm("-sport");
		if (argSteamPort > 0)
			usSteamPort = Q_atoi(com_argv[argSteamPort + 1]);
		eSMode = eServerModeAuthenticationAndSecure;
		if (net_local_adr.type == NA_IP)
			unIP = ntohl(*(u_long *)&net_local_adr.ip[0]);

		m_bLanOnly = sv_lan.value > 0.0;
		m_bWantToBeSecure = !COM_CheckParm("-insecure") && !m_bLanOnly;
		COM_FileBase(com_gamedir, gamedir);

		if (!m_bWantToBeSecure)
			eSMode = eServerModeAuthentication;

		if (m_bLanOnly)
			eSMode = eServerModeNoAuthentication;

		gamePort = (int)iphostport.value;
		if (gamePort == 0)
			gamePort = (int)hostport.value;

		int nAppId = GetGameAppID();
		if (nAppId > 0 && g_pcls.state == ca_dedicated)
		{
			FILE* f = fopen("steam_appid.txt", "w+");
			if (f)
			{
				fprintf(f, "%d\n", nAppId);
				fclose(f);
			}
		}

		if (!CRehldsPlatformHolder::get()->SteamGameServer_Init(unIP, usSteamPort, gamePort, 0xFFFFu, eSMode, gpszVersionString))
			Sys_Error("Unable to initialize Steam.");

		CRehldsPlatformHolder::get()->SteamGameServer()->SetProduct(gpszProductString);
		CRehldsPlatformHolder::get()->SteamGameServer()->SetModDir(gamedir);
		CRehldsPlatformHolder::get()->SteamGameServer()->SetDedicatedServer(g_pcls.state == ca_dedicated);
		CRehldsPlatformHolder::get()->SteamGameServer()->SetGameDescription(gEntityInterface.pfnGetGameDescription());
		CRehldsPlatformHolder::get()->SteamGameServer()->LogOnAnonymous();
		m_bLogOnResult = false;

		if (COM_CheckParm("-nomaster"))
		{
			Con_Printf("Master server communication disabled.\n");
			gfNoMasterServer = TRUE;
		}
		else
		{
			if (!gfNoMasterServer && g_psvs.maxclients > 1)
			{
				CRehldsPlatformHolder::get()->SteamGameServer()->EnableHeartbeats(true);
				double fMasterHeartbeatTimeout = 200.0;
				if (!Q_strcmp(gamedir, "dmc"))
					fMasterHeartbeatTimeout = 150.0;
				if (!Q_strcmp(gamedir, "tfc"))
					fMasterHeartbeatTimeout = 400.0;
				if (!Q_strcmp(gamedir, "cstrike"))
					fMasterHeartbeatTimeout = 400.0;

				CRehldsPlatformHolder::get()->SteamGameServer()->SetHeartbeatInterval((int)fMasterHeartbeatTimeout);
				CSteam3Server::NotifyOfLevelChange(true);
			}
		}
	}
}

void CSteam3Server::Shutdown()
{
	if (m_bLoggedOn)
	{
		SteamGameServer()->EnableHeartbeats(0);
		SteamGameServer()->LogOff();

		SteamGameServer_Shutdown();
		m_bLoggedOn = false;
	}
}

bool CSteam3Server::NotifyClientConnect(client_t *client, const void *pvSteam2Key, uint32 ucbSteam2Key)
{
	class CSteamID steamIDClient;
	bool bRet = false;

	if (client == NULL || !m_bLoggedOn)
		return false;

	client->network_userid.idtype = AUTH_IDTYPE_STEAM;

	bRet = CRehldsPlatformHolder::get()->SteamGameServer()->SendUserConnectAndAuthenticate(htonl(client->network_userid.clientip), pvSteam2Key, ucbSteam2Key, &steamIDClient);
	client->network_userid.m_SteamID = steamIDClient.ConvertToUint64();

	return bRet;
}

bool CSteam3Server::NotifyBotConnect(client_t *client)
{
	if (client == NULL || !m_bLoggedOn)
		return false;

	client->network_userid.idtype = AUTH_IDTYPE_LOCAL;
	CSteamID steamId = CRehldsPlatformHolder::get()->SteamGameServer()->CreateUnauthenticatedUserConnection();
	client->network_userid.m_SteamID = steamId.ConvertToUint64();
	return true;
}

void CSteam3Server::NotifyClientDisconnect(client_t *cl)
{
	if (!cl || !m_bLoggedOn)
		return;

	if (cl->network_userid.idtype == AUTH_IDTYPE_STEAM || cl->network_userid.idtype == AUTH_IDTYPE_LOCAL)
	{
		CRehldsPlatformHolder::get()->SteamGameServer()->SendUserDisconnect(cl->network_userid.m_SteamID);
	}
}

void CSteam3Server::NotifyOfLevelChange(bool bForce)
{
	SendUpdatedServerDetails();
	bool iHasPW = (sv_password.string[0] && Q_stricmp(sv_password.string, "none"));
	CRehldsPlatformHolder::get()->SteamGameServer()->SetPasswordProtected(iHasPW);
	CRehldsPlatformHolder::get()->SteamGameServer()->ClearAllKeyValues();

	for (cvar_t *var = cvar_vars; var; var = var->next)
	{
		if (!(var->flags & FCVAR_SERVER))
			continue;

		const char *szVal;
		if (var->flags & FCVAR_PROTECTED)
		{
			if (Q_strlen(var->string) > 0 && Q_stricmp(var->string, "none"))
				szVal = "1";
			else
				szVal = "0";
		}
		else
		{
			szVal = var->string;
		}
		CRehldsPlatformHolder::get()->SteamGameServer()->SetKeyValue(var->name, szVal);
	}
}

void CSteam3Server::RunFrame()
{
	bool bHasPlayers;
	double fCurTime;

	static double s_fLastRunFragsUpdate;

	if (g_psvs.maxclients <= 1)
		return;

	// KTP: Steam detail profiling state
	qboolean ktp_steam_detail = (ktp_profile_frame.value != 0.0f && ktp_profile_steam_detail.value != 0.0f);
	double ktp_t_frag = 0.0, ktp_t_callbacks = 0.0;

	fCurTime = Sys_FloatTime();
	if (fCurTime - s_fLastRunFragsUpdate > 5.0)  // KTP: Reduced from 1.0s to 5.0s
	{
		double ktp_t_frag_start = 0.0;
		if (ktp_steam_detail)
			ktp_t_frag_start = fCurTime;  // reuse already-captured timestamp

		s_fLastRunFragsUpdate = fCurTime;
		bHasPlayers = false;
		for (int i = 0; i < g_psvs.maxclients; i++)
		{
			client_t* cl = &g_psvs.clients[i];
			if (cl->active || cl->spawned || cl->connected)
			{
				bHasPlayers = true;
				break;
			}
		}

		m_bHasActivePlayers = bHasPlayers;

		// KTP: Snapshot + dispatch to background thread. Replaces the old
		// main-frame synchronous chain of SendUpdatedServerDetails +
		// SetPasswordProtected + SetGameDescription + per-client BUpdateUserData,
		// which caused 3-6ms "steam=" frame spikes (26% of fleet spike volume).
		SteamRefresh_Enqueue();

		if (CRehldsPlatformHolder::get()->SteamGameServer()->WasRestartRequested())
		{
			Con_Printf("%cMasterRequestRestart\n", 3);
			if (COM_CheckParm("-steam"))
			{
				Con_Printf("Your server needs to be restarted in order to receive the latest update.\n");
				Log_Printf("Your server needs to be restarted in order to receive the latest update.\n");
			}
			else
			{
				Con_Printf("Your server is out of date.  Please update and restart.\n");
			}
		}

		if (ktp_steam_detail)
			ktp_t_frag = Sys_FloatTime() - ktp_t_frag_start;
	}

	// KTP: Drain queued callbacks from background thread (replaces direct RunCallbacks)
	// The IPC-blocking SteamGameServer_RunCallbacks() now runs on a background thread.
	// This just processes the queued results — microseconds, not milliseconds.
	{
		double ktp_t_cb_start = 0.0;
		if (ktp_steam_detail)
			ktp_t_cb_start = Sys_FloatTime();

		DrainCallbackQueue();

		if (ktp_steam_detail)
			ktp_t_callbacks = Sys_FloatTime() - ktp_t_cb_start;
	}

	// KTP: SendPackets moved to background thread (Steam_ThreadFunc).
	// NET_SendPacket uses sendto() which is atomic on Linux UDP — safe
	// for concurrent calls from game thread and Steam thread.

	// KTP: Log steam detail when any sub-operation exceeded 1ms
	// Note: sendpackets now runs on background thread, not measured here
	if (ktp_steam_detail)
	{
		double total_ms = (ktp_t_frag + ktp_t_callbacks) * 1000.0;
		if (total_ms > 1.0)
		{
			Log_Printf("[KTP_PROFILE_STEAM] callbacks=%.3fms fragupdate=%.3fms\n",
				ktp_t_callbacks * 1000.0,
				ktp_t_frag * 1000.0);
		}
	}
}

void CSteam3Server::UpdateGameTags()
{
#ifdef REHLDS_FIXES
	if (!m_GameTagsData[0] && !sv_tags.string[0])
		return;

	if (m_GameTagsData[0] && !Q_stricmp(m_GameTagsData, sv_tags.string))
		return;

	Q_strlcpy(m_GameTagsData, sv_tags.string);
	Q_strlwr(m_GameTagsData);
	CRehldsPlatformHolder::get()->SteamGameServer()->SetGameTags(m_GameTagsData);
#endif
}

void CSteam3Server::SendUpdatedServerDetails()
{
	int botCount = 0;
	if (g_psvs.maxclients > 0)
	{

		for (int i = 0; i < g_psvs.maxclients; i++)
		{
			auto cl = &g_psvs.clients[i];
			if ((cl->active || cl->spawned || cl->connected) && cl->fakeclient)
				++botCount;
		}
	}

	int maxPlayers = sv_visiblemaxplayers.value;
	if (maxPlayers < 0)
		maxPlayers = g_psvs.maxclients;

	CRehldsPlatformHolder::get()->SteamGameServer()->SetMaxPlayerCount(maxPlayers);
	CRehldsPlatformHolder::get()->SteamGameServer()->SetBotPlayerCount(botCount);
	CRehldsPlatformHolder::get()->SteamGameServer()->SetServerName(Cvar_VariableString("hostname"));
	CRehldsPlatformHolder::get()->SteamGameServer()->SetMapName(g_psv.name);

	UpdateGameTags();
}

void CSteam3Client::Shutdown()
{
	if (m_bLoggedOn)
	{
		SteamAPI_Shutdown();
		m_bLoggedOn = false;
	}
}

int CSteam3Client::InitiateGameConnection(void *pData, int cbMaxData, uint64 steamID, uint32 unIPServer, uint16 usPortServer, bool bSecure)
{
	return SteamUser()->InitiateGameConnection(pData, cbMaxData, CSteamID(steamID), ntohl(unIPServer), ntohs(usPortServer), bSecure);
}

void CSteam3Client::TerminateConnection(uint32 unIPServer, uint16 usPortServer)
{
	SteamUser()->TerminateGameConnection(ntohl(unIPServer), ntohs(usPortServer));
}

void CSteam3Client::InitClient()
{
	if (m_bLoggedOn)
		return;

	m_bLoggedOn = true;
	_unlink("steam_appid.txt");
	if (!getenv("SteamAppId"))
	{
		int nAppID = GetGameAppID();
		if (nAppID > 0)
		{
			FILE* f = fopen("steam_appid.txt", "w+");
			if (f)
			{
				fprintf(f, "%d\n", nAppID);
				fclose(f);
			}
		}
	}

	if (!SteamAPI_Init())
		Sys_Error("Failed to initalize authentication interface. Exiting...\n");

	m_bLogOnResult = false;
}

void CSteam3Client::OnClientGameServerDeny(ClientGameServerDeny_t *pClientGameServerDeny)
{
	COM_ExplainDisconnection(TRUE, "Invalid server version, unable to connect.");
	CL_Disconnect();
}

void CSteam3Client::OnGameServerChangeRequested(GameServerChangeRequested_t *pGameServerChangeRequested)
{
#ifndef SWDS
	char *cmd;

	Cvar_DirectSet(&password, pGameServerChangeRequested->m_rgchPassword);
	Con_Printf("Connecting to %s\n", pGameServerChangeRequested->m_rgchServer);
	cmd = va("connect %s\n", pGameServerChangeRequested->m_rgchServer);
	Cbuf_AddText(cmd);
#endif
}

void CSteam3Client::OnGameOverlayActivated(GameOverlayActivated_t *pGameOverlayActivated)
{
#ifndef SWDS
	if (Host_IsSinglePlayerGame())
	{
		if (pGameOverlayActivated->m_bActive)
		{
			Cbuf_AddText("setpause;");
		}
		else
		{
			if (!(unsigned __int8)(*(int(**)())(*(_DWORD *)g_pGameUI007 + 44))())
			{
				Cbuf_AddText("unpause;");
			}
		}
	}
#endif
}

void CSteam3Client::RunFrame()
{
	CRehldsPlatformHolder::get()->SteamAPI_RunCallbacks();
}

uint64 ISteamGameServer_CreateUnauthenticatedUserConnection()
{
	if (!CRehldsPlatformHolder::get()->SteamGameServer())
	{
		return 0L;
	}

	return CRehldsPlatformHolder::get()->SteamGameServer()->CreateUnauthenticatedUserConnection().ConvertToUint64();
}

bool Steam_GSBUpdateUserData(uint64 steamIDUser, const char *pchPlayerName, uint32 uScore)
{
	return CRehldsPlatformHolder::get()->SteamGameServer()->BUpdateUserData(steamIDUser, pchPlayerName, uScore);
}

bool ISteamGameServer_BUpdateUserData(uint64 steamid, const char *netname, uint32 score)
{
	if (!CRehldsPlatformHolder::get()->SteamGameServer())
	{
		return false;
	}

	return g_RehldsHookchains.m_Steam_GSBUpdateUserData.callChain(Steam_GSBUpdateUserData, steamid, netname, score);
}

bool ISteamApps_BIsSubscribedApp(uint32 appid)
{
	if (CRehldsPlatformHolder::get()->SteamApps())
	{
		ISteamApps* apps = CRehldsPlatformHolder::get()->SteamApps();
		return apps->BIsSubscribedApp(appid);
	}

	return false;
}

const char *Steam_GetCommunityName()
{
	if (SteamFriends())
		return SteamFriends()->GetPersonaName();

	return NULL;
}

qboolean EXT_FUNC Steam_NotifyClientConnect_api(IGameClient *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key)
{
	return Steam_NotifyClientConnect_internal(cl->GetClient(), pvSteam2Key, ucbSteam2Key);
}

qboolean Steam_NotifyClientConnect(client_t *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key)
{
	return g_RehldsHookchains.m_Steam_NotifyClientConnect
		.callChain(Steam_NotifyClientConnect_api, GetRehldsApiClient(cl), pvSteam2Key, ucbSteam2Key);
}

qboolean Steam_NotifyClientConnect_internal(client_t *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key)
{
	if (Steam3Server())
	{
		return Steam3Server()->NotifyClientConnect(cl, pvSteam2Key, ucbSteam2Key);
	}
	return FALSE;
}

qboolean EXT_FUNC Steam_NotifyBotConnect_api(IGameClient* cl)
{
	return Steam_NotifyBotConnect_internal(cl->GetClient());
}

qboolean Steam_NotifyBotConnect(client_t *cl)
{
	return g_RehldsHookchains.m_Steam_NotifyBotConnect.callChain(Steam_NotifyBotConnect_api, GetRehldsApiClient(cl));
}

qboolean Steam_NotifyBotConnect_internal(client_t *cl)
{
	if (Steam3Server())
	{
		return Steam3Server()->NotifyBotConnect(cl);
	}
	return FALSE;
}

void EXT_FUNC Steam_NotifyClientDisconnect_api(IGameClient* cl)
{
	g_RehldsHookchains.m_Steam_NotifyClientDisconnect.callChain(Steam_NotifyClientDisconnect_internal, cl);
}

void Steam_NotifyClientDisconnect(client_t *cl)
{
	Steam_NotifyClientDisconnect_api(GetRehldsApiClient(cl));
}

void Steam_NotifyClientDisconnect_internal(IGameClient* cl)
{
	if (Steam3Server())
	{
		Steam3Server()->NotifyClientDisconnect(cl->GetClient());
	}
}

void Steam_NotifyOfLevelChange()
{
	if (Steam3Server())
	{
		Steam3Server()->NotifyOfLevelChange(false);
	}
}

void Steam_Shutdown()
{
	// KTP: Stop background callback thread before shutdown
	s_steamThreadRunning.store(false, std::memory_order_relaxed);
	if (s_steamThread)
	{
		s_steamThread->join();
		delete s_steamThread;
		s_steamThread = nullptr;
	}

	if (Steam3Server())
	{
		Steam3Server()->Shutdown();
		delete s_Steam3Server;
		s_Steam3Server = NULL;
	}
}

void Steam_Activate()
{
	if (!Steam3Server())
	{
		s_Steam3Server = new CSteam3Server();
		if (s_Steam3Server == NULL)
			return;
	}

	Steam3Server()->Activate();

	// KTP: Start background callback thread
	if (!s_steamThread)
	{
		s_steamThreadRunning.store(true, std::memory_order_relaxed);
		s_steamThread = new std::thread(Steam_ThreadFunc);
	}
}

void Steam_RunFrame()
{
	if (Steam3Server())
	{
		Steam3Server()->RunFrame();
	}
}

void Steam_SetCVar(const char *pchKey, const char *pchValue)
{
	if (Steam3Server())
	{
		CRehldsPlatformHolder::get()->SteamGameServer()->SetKeyValue(pchKey, pchValue);
	}
}

void Steam_ClientRunFrame()
{
	Steam3Client()->RunFrame();
}

void Steam_InitClient()
{
	Steam3Client()->InitClient();
}

int Steam_GSInitiateGameConnection(void *pData, int cbMaxData, uint64 steamID, uint32 unIPServer, uint16 usPortServer, qboolean bSecure)
{
	return Steam3Client()->InitiateGameConnection(pData, cbMaxData, steamID, unIPServer, usPortServer, bSecure != 0);
}

void Steam_GSTerminateGameConnection(uint32 unIPServer, uint16 usPortServer)
{
	Steam3Client()->TerminateConnection(unIPServer, usPortServer);
}

void Steam_ShutdownClient()
{
	Steam3Client()->Shutdown();
}

uint64 Steam_GSGetSteamID()
{
	return Steam3Server()->GetSteamID();
}

qboolean Steam_GSBSecure()
{
	//useless call
	//Steam3Server();
	return CRehldsPlatformHolder::get()->SteamGameServer()->BSecure();
}

qboolean Steam_GSBLoggedOn()
{
	return Steam3Server()->BLoggedOn() && CRehldsPlatformHolder::get()->SteamGameServer()->BLoggedOn();
}

qboolean Steam_GSBSecurePreference()
{
	return Steam3Server()->BWantsSecure();
}

TSteamGlobalUserID Steam_Steam3IDtoSteam2(uint64 unSteamID)
{
	class CSteamID steamID = unSteamID;
	TSteamGlobalUserID steam2ID;
	steamID.ConvertToSteam2(&steam2ID);
	return steam2ID;
}

uint64 Steam_StringToSteamID(const char *pStr)
{
	CSteamID steamID;
	if (Steam3Server())
	{
		CSteamID serverSteamId(Steam3Server()->GetSteamID());
		steamID.SetFromSteam2String(pStr, serverSteamId.GetEUniverse());
	}
	else
	{
		steamID.SetFromSteam2String(pStr, k_EUniversePublic);
	}

	return steamID.ConvertToUint64();
}

const char *Steam_GetGSUniverse()
{
	CSteamID steamID(Steam3Server()->GetSteamID());
	switch (steamID.GetEUniverse())
	{
	case k_EUniversePublic:
		return "";

	case k_EUniverseBeta:
		return "(beta)";

	case k_EUniverseInternal:
		return "(internal)";

	default:
		return "(u)";
	}
}

CSteam3Server *s_Steam3Server;
CSteam3Client s_Steam3Client;

CSteam3Server *Steam3Server()
{
	return s_Steam3Server;
}

CSteam3Client *Steam3Client()
{
	return &s_Steam3Client;
}

void Master_SetMaster_f()
{
	int i;
	const char * pszCmd;

	i = Cmd_Argc();
	if (!Steam3Server())
	{
		Con_Printf("Usage:\nSetmaster unavailable, start a server first.\n");
		return;
	}

	if (i < 2 || i > 5)
	{
		Con_Printf("Usage:\nSetmaster <enable | disable>\n");
		return;
	}

	pszCmd = Cmd_Argv(1);
	if (!pszCmd || !pszCmd[0])
		return;

	if (Q_stricmp(pszCmd, "disable") || gfNoMasterServer)
	{
		if (!Q_stricmp(pszCmd, "enable"))
		{
			if (gfNoMasterServer)
			{
				gfNoMasterServer = FALSE;
				CRehldsPlatformHolder::get()->SteamGameServer()->EnableHeartbeats(gfNoMasterServer != 0);
			}
		}
	}
	else
	{
		gfNoMasterServer = TRUE;
		CRehldsPlatformHolder::get()->SteamGameServer()->EnableHeartbeats(gfNoMasterServer != 0);
	}
}

void Steam_HandleIncomingPacket(byte *data, int len, int fromip, uint16 port)
{
	CRehldsPlatformHolder::get()->SteamGameServer()->HandleIncomingPacket(data, len, fromip, port);
}

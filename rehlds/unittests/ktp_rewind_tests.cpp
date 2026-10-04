#include "precompiled.h"
#include "ktp_rewind.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

void* EXT_FUNC Rehlds_GetPluginApi(const char *name);

static client_t s_rw_clients[2];
static client_frame_t s_rw_frames[MULTIPLAYER_BACKUP];
static int s_rw_allow = 1;

static int RW_AllowLagCompensation(void)
{
	return s_rw_allow;
}

// A two-slot server with one active shooter in slot 1, and every engine global
// SV_SetupMove reads put back afterwards so no other test inherits it.
class KtpRewindFixture
{
public:
	KtpRewindFixture()
	{
		m_clients = g_psvs.clients;
		m_maxclients = g_psvs.maxclients;
		m_backup = SV_UPDATE_BACKUP;
		m_mask = SV_UPDATE_MASK;
		m_unlag = sv_unlag.value;
		m_maxunlag = sv_maxunlag.value;
		m_push = sv_unlagpush.value;
		m_estimator = sv_unlag_estimator.value;
		m_realtime = realtime;
		m_allow = gEntityInterface.pfnAllowLagCompensation;
		m_record = g_ktp_rewind;
		m_nofind = nofind;

		Q_memset(s_rw_clients, 0, sizeof(s_rw_clients));
		Q_memset(s_rw_frames, 0, sizeof(s_rw_frames));
		g_psvs.clients = s_rw_clients;
		g_psvs.maxclients = 2;
		SV_UPDATE_BACKUP = MULTIPLAYER_BACKUP;
		SV_UPDATE_MASK = MULTIPLAYER_BACKUP - 1;
		sv_unlag.value = 1.0f;
		sv_maxunlag.value = 0.3f;
		sv_unlagpush.value = 0.0f;
		sv_unlag_estimator.value = 0.0f;
		realtime = 100.0;
		s_rw_allow = 1;
		gEntityInterface.pfnAllowLagCompensation = &RW_AllowLagCompensation;

		cl = &s_rw_clients[1];
		cl->active = TRUE;
		cl->lw = TRUE;
		cl->lc = TRUE;
		cl->frames = s_rw_frames;
		cl->latency = 0.1f;
		cl->lastcmd.lerp_msec = 50;
		cl->next_messageinterval = 0.01;
		cl->netchan.outgoing_sequence = 200;
		cl->netchan.incoming_sequence = 77;
		History(true);

		// A stale record from some other packet, so a write that did not happen shows.
		g_ktp_rewind.slot = 0;
		g_ktp_rewind.sequence = 5;
		g_ktp_rewind.open = FALSE;
		g_ktp_rewind.sample.flags = 0x7fu;
		g_ktp_rewind.sample.depth_ms = 1234.0f;
		g_ktp_rewind.sample.want_ms = 1234.0f;
	}

	~KtpRewindFixture()
	{
		g_psvs.clients = m_clients;
		g_psvs.maxclients = m_maxclients;
		SV_UPDATE_BACKUP = m_backup;
		SV_UPDATE_MASK = m_mask;
		sv_unlag.value = m_unlag;
		sv_maxunlag.value = m_maxunlag;
		sv_unlagpush.value = m_push;
		sv_unlag_estimator.value = m_estimator;
		realtime = m_realtime;
		gEntityInterface.pfnAllowLagCompensation = m_allow;
		g_ktp_rewind = m_record;
		nofind = m_nofind;
	}

	// reachable: one frame per 10 ms back from realtime, so any target inside the
	// backup window is found. Otherwise every frame is at realtime and none is old
	// enough, which is the history-miss exit.
	void History(bool reachable)
	{
		for (int i = 0; i < SV_UPDATE_BACKUP; i++)
		{
			client_frame_t *f = &cl->frames[SV_UPDATE_MASK & (cl->netchan.outgoing_sequence + ~i)];
			f->senttime = reachable ? realtime - 0.01 * (i + 1) : realtime;
			f->entities.num_entities = 0;
		}
	}

	unsigned int Setup()
	{
		SV_SetupMove(cl);
		return g_ktp_rewind.sample.flags;
	}

	client_t *cl;

private:
	client_t *m_clients;
	int m_maxclients;
	int m_backup;
	int m_mask;
	float m_unlag;
	float m_maxunlag;
	float m_push;
	float m_estimator;
	double m_realtime;
	int (*m_allow)(void);
	ktp_rewind_record_t m_record;
	qboolean m_nofind;
};

static void CheckOwnedOpen(const char *exit_name)
{
	CHECK(exit_name, g_ktp_rewind.open);
	LONGS_EQUAL(exit_name, 1, g_ktp_rewind.slot);
	LONGS_EQUAL(exit_name, 77, g_ktp_rewind.sequence);
}

TEST(EveryExitWritesTheRecord, KtpRewind, 1000)
{
	{
		KtpRewindFixture f;
		s_rw_allow = 0;
		UINT32_EQUALS("game DLL refused lag compensation", 0u, f.Setup());
		CheckOwnedOpen("game DLL refused lag compensation");
	}
	{
		KtpRewindFixture f;
		sv_unlag.value = 0.0f;
		UINT32_EQUALS("sv_unlag 0", 0u, f.Setup());
		CheckOwnedOpen("sv_unlag 0");
	}
	{
		KtpRewindFixture f;
		f.cl->lw = FALSE;
		UINT32_EQUALS("cl_lw 0", 0u, f.Setup());
		CheckOwnedOpen("cl_lw 0");
	}
	{
		KtpRewindFixture f;
		f.cl->lc = FALSE;
		UINT32_EQUALS("cl_lc 0", 0u, f.Setup());
		CheckOwnedOpen("cl_lc 0");
	}
	{
		KtpRewindFixture f;
		g_psvs.maxclients = 1;
		UINT32_EQUALS("single player", 0u, f.Setup());
		CheckOwnedOpen("single player");
	}
	{
		KtpRewindFixture f;
		f.cl->active = FALSE;
		UINT32_EQUALS("inactive client", 0u, f.Setup());
		CheckOwnedOpen("inactive client");
	}
	{
		KtpRewindFixture f;
		SV_UPDATE_BACKUP = 0;
		UINT32_EQUALS("no backup frames", KTP_REWIND_ATTEMPTED, f.Setup());
		CheckOwnedOpen("no backup frames");
		DOUBLES_EQUAL("no backup frames still has a target", 150.0, g_ktp_rewind.sample.depth_ms, 0.01);
	}
	{
		KtpRewindFixture f;
		f.History(false);
		UINT32_EQUALS("history miss", KTP_REWIND_ATTEMPTED, f.Setup());
		CheckOwnedOpen("history miss");
		DOUBLES_EQUAL("history miss still has a target", 150.0, g_ktp_rewind.sample.depth_ms, 0.01);
	}
	{
		KtpRewindFixture f;
		UINT32_EQUALS("reached", KTP_REWIND_ATTEMPTED | KTP_REWIND_REACHED, f.Setup());
		CheckOwnedOpen("reached");
		DOUBLES_EQUAL("latency + interp", 150.0, g_ktp_rewind.sample.depth_ms, 0.01);
	}
}

TEST(RestoreMoveClosesOnBothPaths, KtpRewind, 1000)
{
	{
		KtpRewindFixture f;
		s_rw_allow = 0;
		f.Setup();
		CHECK("control: not-attempted path set nofind", nofind);
		CHECK("control: open before restore", g_ktp_rewind.open);
		SV_RestoreMove(f.cl);
		CHECK("nofind path left the record open", !g_ktp_rewind.open);
	}
	{
		KtpRewindFixture f;
		f.Setup();
		CHECK("control: rewound path cleared nofind", !nofind);
		CHECK("control: open before restore", g_ktp_rewind.open);
		SV_RestoreMove(f.cl);
		CHECK("restore path left the record open", !g_ktp_rewind.open);
	}
}

TEST(GetCurrentRefusesClosedAndForeign, KtpRewind, 1000)
{
	KtpRewindFixture f;
	ktp_rewind_sample_t out;

	CHECK("stale closed record was readable", !KTP_RewindGetCurrent(0, &out));

	f.Setup();
	Q_memset(&out, 0, sizeof(out));
	CHECK("owner could not read its own record", KTP_RewindGetCurrent(1, &out));
	UINT32_EQUALS("sample flags", g_ktp_rewind.sample.flags, out.flags);
	DOUBLES_EQUAL("sample depth", 150.0, out.depth_ms, 0.01);
	CHECK("another slot read the owner's record", !KTP_RewindGetCurrent(0, &out));
	CHECK("an out-of-range slot read the record", !KTP_RewindGetCurrent(-1, &out));
	CHECK("a NULL out was accepted", !KTP_RewindGetCurrent(1, NULL));

	SV_RestoreMove(f.cl);
	CHECK("a closed record was readable", !KTP_RewindGetCurrent(1, &out));

	// The plugin API hands out this same function under its versioned name.
	KTP_RewindRegisterApi();
	ktp_rewind_api_v1_t *api = (ktp_rewind_api_v1_t *)Rehlds_GetPluginApi(KTP_REWIND_API_V1);
	CHECK("ktp_rewind_v1 is not registered", api == &g_ktp_rewind_api_v1);
	UINT32_EQUALS("api size", (uint32)sizeof(ktp_rewind_api_v1_t), api->size);
	CHECK("api GetCurrent is not the engine's", api->GetCurrent == &KTP_RewindGetCurrent);
}

TEST(BitsMatchTheirInputs, KtpRewind, 1000)
{
	{
		KtpRewindFixture f;
		f.cl->latency = 0.5f;
		unsigned int flags = f.Setup();
		CHECK("over sv_maxunlag is not clamped", flags & KTP_REWIND_CLAMPED);
		CHECK("0.5 s set hardcap", !(flags & KTP_REWIND_HARDCAP));
		DOUBLES_EQUAL("clamped depth", 350.0, g_ktp_rewind.sample.depth_ms, 0.01);
		DOUBLES_EQUAL("pre-clamp want", 550.0, g_ktp_rewind.sample.want_ms, 0.01);
	}
	{
		// The engine compares with >=, so a shot exactly at the ceiling is clamped by 0.
		KtpRewindFixture f;
		f.cl->latency = 0.25f;
		sv_maxunlag.value = 0.25f;
		unsigned int flags = f.Setup();
		CHECK("at the ceiling is not clamped", flags & KTP_REWIND_CLAMPED);
		DOUBLES_EQUAL("zero excess", 0.0, g_ktp_rewind.sample.want_ms - g_ktp_rewind.sample.depth_ms, 0.01);
	}
	{
		KtpRewindFixture f;
		f.cl->latency = 2.0f;
		unsigned int flags = f.Setup();
		CHECK("over 1.5 s is not hardcap", flags & KTP_REWIND_HARDCAP);
		CHECK("over 1.5 s is not clamped", flags & KTP_REWIND_CLAMPED);
		DOUBLES_EQUAL("want is the capped floor", 1550.0, g_ktp_rewind.sample.want_ms, 0.01);
	}
	{
		KtpRewindFixture f;
		f.cl->latency = 0.5f;
		sv_maxunlag.value = 0.0f;
		unsigned int flags = f.Setup();
		CHECK("sv_maxunlag 0 clamped", !(flags & KTP_REWIND_CLAMPED));
		DOUBLES_EQUAL("unclamped depth", 550.0, g_ktp_rewind.sample.depth_ms, 0.01);
	}
	{
		KtpRewindFixture f;
		sv_unlagpush.value = 1.0f;
		unsigned int flags = f.Setup();
		CHECK("push past realtime is not pushed", flags & KTP_REWIND_PUSHED);
		DOUBLES_EQUAL("pushed depth is 0", 0.0, g_ktp_rewind.sample.depth_ms, 0.001);
		DOUBLES_EQUAL("pushed want is 0", 0.0, g_ktp_rewind.sample.want_ms, 0.001);
	}
	{
		KtpRewindFixture f;
		sv_unlagpush.value = 0.05f;
		CHECK("a push that stays behind realtime set pushed", !(f.Setup() & KTP_REWIND_PUSHED));
	}
	{
		KtpRewindFixture f;
		f.cl->lastcmd.lerp_msec = 150;
		CHECK("interp over 0.1 s is not adjusted", f.Setup() & KTP_REWIND_INTERP_ADJUSTED);
		DOUBLES_EQUAL("capped interp", 200.0, g_ktp_rewind.sample.depth_ms, 0.01);
	}
	{
		KtpRewindFixture f;
		f.cl->lastcmd.lerp_msec = 5;
		CHECK("interp under the update interval is not adjusted", f.Setup() & KTP_REWIND_INTERP_ADJUSTED);
		DOUBLES_EQUAL("floored interp", 110.0, g_ktp_rewind.sample.depth_ms, 0.01);
	}
	{
		KtpRewindFixture f;
		CHECK("a compliant interp was adjusted", !(f.Setup() & KTP_REWIND_INTERP_ADJUSTED));
	}
	{
		KtpRewindFixture f;
		sv_unlag_estimator.value = 1.0f;
		CHECK("estimator on is not flagged", f.Setup() & KTP_REWIND_ESTIMATOR);
		s_rw_allow = 0;
		UINT32_EQUALS("estimator bit on a not-attempted packet", KTP_REWIND_ESTIMATOR, f.Setup());
	}
	{
		KtpRewindFixture f;
		CHECK("estimator off is flagged", !(f.Setup() & KTP_REWIND_ESTIMATOR));
	}
}

TEST(WantEqualsDepthUnlessClamped, KtpRewind, 1000)
{
	static const float latencies[] = { 0.0f, 0.013f, 0.1f, 0.2999f, 0.3f, 0.3001f, 0.75f, 1.5f, 3.0f };
	static const short lerps[] = { 0, 5, 33, 100, 250 };
	static const float pushes[] = { 0.0f, 0.02f, 2.0f };
	int unclamped = 0;
	int clamped_apart = 0;

	for (size_t l = 0; l < sizeof(latencies) / sizeof(latencies[0]); l++)
	for (size_t i = 0; i < sizeof(lerps) / sizeof(lerps[0]); i++)
	for (size_t p = 0; p < sizeof(pushes) / sizeof(pushes[0]); p++)
	{
		KtpRewindFixture f;
		f.cl->latency = latencies[l];
		f.cl->lastcmd.lerp_msec = lerps[i];
		sv_unlagpush.value = pushes[p];
		unsigned int flags = f.Setup();
		const ktp_rewind_sample_t &s = g_ktp_rewind.sample;

		if (!(flags & KTP_REWIND_CLAMPED))
		{
			CHECK("want differs from depth without a clamp", Q_memcmp(&s.want_ms, &s.depth_ms, sizeof(float)) == 0);
			unclamped++;
		}
		else if (s.want_ms > s.depth_ms)
		{
			clamped_apart++;
		}
		CHECK("want below depth", s.want_ms >= s.depth_ms);
	}

	CHECK("sweep never left the clamp", unclamped > 0);
	CHECK("sweep never separated want from depth", clamped_apart > 0);
}

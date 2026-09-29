#include "precompiled.h"
#include "ktp_nettelemetry.h"
#include "ktp_pushtelemetry.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

static const qboolean ROTATE = TRUE;
static const qboolean MOVE = FALSE;
static const int NOT_A_CLIENT = -1;
static const int MAP_A = 7;

static void ktp_push_reset_all()
{
	g_ktp_push_rotate = 0;
	g_ktp_push_move = 0;
	g_ktp_push_episodes = 0;
	g_ktp_push_untracked = 0;
	g_ktp_push_track_n = 0;
	for (int i = 0; i < MAX_CLIENTS; i++)
		g_ktp_push_victim_slot[i] = 0;
}

TEST(SplitsRotateFromMove, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();

	KTP_PushSampleBlocked(40, 2, ROTATE, 100, MAP_A);
	KTP_PushSampleBlocked(41, 2, ROTATE, 300, MAP_A);
	KTP_PushSampleBlocked(50, NOT_A_CLIENT, MOVE, 500, MAP_A);

	UINT32_EQUALS("rotate", 2u, g_ktp_push_rotate);
	UINT32_EQUALS("move", 1u, g_ktp_push_move);
	LONGS_EQUAL("distinct pushers", 3, g_ktp_push_track_n);
	UINT32_EQUALS("client blocks on slot 2", 2u, g_ktp_push_victim_slot[2]);

	uint32 attributed = 0;
	for (int i = 0; i < MAX_CLIENTS; i++)
		attributed += g_ktp_push_victim_slot[i];
	UINT32_EQUALS("a non-client victim was attributed to a slot", 2u, attributed);
}

// pfnBlocked fires every frame a door stays blocked, so the raw count scales
// with sys_ticrate. Episodes must not.
TEST(ConsecutiveFramesAreOneEpisode, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();

	for (int f = 1000; f < 1500; f++)
		KTP_PushSampleBlocked(40, 3, ROTATE, f, MAP_A);

	UINT32_EQUALS("blocks", 500u, g_ktp_push_rotate);
	UINT32_EQUALS("one held door is one episode", 1u, g_ktp_push_episodes);

	// The door reverses, swings back and hits the player again.
	KTP_PushSampleBlocked(40, 3, ROTATE, 1600, MAP_A);
	UINT32_EQUALS("a gap starts a new episode", 2u, g_ktp_push_episodes);
	UINT32_EQUALS("per-pusher episodes", 2u, g_ktp_push_track[0].episodes);
	UINT32_EQUALS("per-pusher blocks", 501u, g_ktp_push_track[0].blocks);

	// Same frame from both halves of a rotate+move pusher is not a new episode.
	KTP_PushSampleBlocked(40, 3, MOVE, 1600, MAP_A);
	UINT32_EQUALS("same-frame repeat opened an episode", 2u, g_ktp_push_episodes);

	// A different door in the same frame is its own episode.
	KTP_PushSampleBlocked(41, 3, ROTATE, 1600, MAP_A);
	UINT32_EQUALS("episodes are per pusher", 3u, g_ktp_push_episodes);
}

// An edict index is reused by the next map, so it must not continue an entry
// from the previous one, or push_detail names this map's entity for that one's blocks.
TEST(ChangelevelDoesNotMergePushers, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();

	KTP_PushSampleBlocked(40, 1, ROTATE, 100, MAP_A);
	KTP_PushSampleBlocked(40, 1, ROTATE, 101, MAP_A + 1);

	LONGS_EQUAL("same index on a new map merged", 2, g_ktp_push_track_n);
	UINT32_EQUALS("new map's first block is an episode", 2u, g_ktp_push_episodes);
}

TEST(OverflowStillCounts, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();

	for (int i = 0; i < KTP_PUSH_TRACK_N + 3; i++)
		KTP_PushSampleBlocked(100 + i, NOT_A_CLIENT, MOVE, 10 * i, MAP_A);

	LONGS_EQUAL("table grew past its bound", KTP_PUSH_TRACK_N, g_ktp_push_track_n);
	UINT32_EQUALS("overflow blocks", 3u, g_ktp_push_untracked);
	UINT32_EQUALS("overflow dropped from the total", (uint32)(KTP_PUSH_TRACK_N + 3), g_ktp_push_move);
}

TEST(WorstPusherIsTheBusiest, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();

	LONGS_EQUAL("empty table has a worst", -1, KTP_PushWorstTrack());

	KTP_PushSampleBlocked(40, 1, ROTATE, 100, MAP_A);
	KTP_PushSampleBlocked(41, 1, ROTATE, 200, MAP_A);
	KTP_PushSampleBlocked(41, 1, ROTATE, 201, MAP_A);

	int worst = KTP_PushWorstTrack();
	CHECK("no worst pusher", worst >= 0);
	LONGS_EQUAL("worst pusher", 41, g_ktp_push_track[worst].pusher);
}

TEST(VictimSlotExcludesProxiesAndNonClients, KtpPushTelemetry, 1000)
{
	LONGS_EQUAL("player", 4, KTP_PushVictimSlot(FL_CLIENT, 5));
	LONGS_EQUAL("proxy", -1, KTP_PushVictimSlot(FL_CLIENT | FL_PROXY, 5));
	LONGS_EQUAL("grenade", -1, KTP_PushVictimSlot(0, 90));
	LONGS_EQUAL("worldspawn", -1, KTP_PushVictimSlot(FL_CLIENT, 0));
	LONGS_EQUAL("past the slot array", -1, KTP_PushVictimSlot(FL_CLIENT, MAX_CLIENTS + 1));

	// Out-of-range slots from a bad caller must not write past the array.
	ktp_push_reset_all();
	KTP_PushSampleBlocked(40, MAX_CLIENTS, MOVE, 1, MAP_A);
	UINT32_EQUALS("out-of-range victim was not counted as a block", 1u, g_ktp_push_move);
}

// A counter left out of KTP_ProfileResetInterval reports a lifetime total
// under an interval label.
TEST(IntervalResetCoversPushCounters, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();

	KTP_PushSampleBlocked(40, 1, ROTATE, 100, MAP_A);
	KTP_PushSampleBlocked(50, 2, MOVE, 100, MAP_A);
	for (int i = 0; i < KTP_PUSH_TRACK_N; i++)
		KTP_PushSampleBlocked(200 + i, NOT_A_CLIENT, MOVE, 100, MAP_A);

	CHECK("fixture did not populate the counters",
		g_ktp_push_rotate && g_ktp_push_move && g_ktp_push_episodes && g_ktp_push_untracked
		&& g_ktp_push_track_n && g_ktp_push_victim_slot[1] && g_ktp_push_victim_slot[2]);

	KTP_ProfileResetInterval();

	UINT32_EQUALS("rotate survived the reset", 0u, g_ktp_push_rotate);
	UINT32_EQUALS("move survived the reset", 0u, g_ktp_push_move);
	UINT32_EQUALS("episodes survived the reset", 0u, g_ktp_push_episodes);
	UINT32_EQUALS("untracked survived the reset", 0u, g_ktp_push_untracked);
	LONGS_EQUAL("pusher table survived the reset", 0, g_ktp_push_track_n);
	for (int i = 0; i < MAX_CLIENTS; i++)
		UINT32_EQUALS("per-slot victims survived the reset", 0u, g_ktp_push_victim_slot[i]);
}

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
	g_ktp_push_blocked_time = 0.0;
	g_ktp_push_track_n = 0;
	for (int i = 0; i < MAX_CLIENTS; i++)
		g_ktp_push_victim_slot[i] = 0;
}

static void ktp_contain_reset_all()
{
	g_ktp_contain_stops = 0;
	g_ktp_contain_tests = 0;
	g_ktp_contain_hits = 0;
	g_ktp_contain_unblocked = 0;
	g_ktp_contain_blind = 0;
	g_ktp_contain_last_ent = -1;
	g_ktp_contain_last_spawn = 0;
	g_ktp_contain_episodes = 0;
	g_ktp_contain_untracked = 0;
	g_ktp_contain_track_n = 0;
	for (int i = 0; i < MAX_CLIENTS; i++)
		g_ktp_contain_slot[i] = 0;
}

static const char *MAP_A_NAME = "dod_donner";
static const char *DOOR_CLASS = "func_door_rotating";
static const char *DOOR_TARGET = "church_gate";

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

// A stepped door moves once per step, two or three ~1 ms frames apart, so a door held
// shut blocks once a step. Under the frame rule each of those opened an episode, which
// made episodes= equal blocked= on a fleet stepping at 500 Hz.
static int ktp_push_hold_stepped(int pusher, int frame, double *time, double seconds, float hz, qboolean stepAware)
{
	const double frames[2] = { 0.0009, 0.0011 };
	const double step = 1.0 / hz;
	double bank = 0.0;
	int steps = 0;

	for (double held = 0.0; held < seconds; frame++)
	{
		const double dt = frames[frame & 1];
		*time += dt;
		held += dt;
		bank += dt;
		if (bank + 1.0e-6 >= step)
		{
			KTP_PushSampleBlocked(pusher, 3, ROTATE, frame, MAP_A, *time,
				stepAware ? KTP_PushEpisodeGap(hz, dt) : 0.0, bank);
			bank = 0.0;
			steps++;
		}
	}

	return steps;
}

TEST(SteppedHoldIsOneEpisode, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();
	double time = 100.0;

	int steps = ktp_push_hold_stepped(40, 1000, &time, 1.0, 500.0f, TRUE);

	CHECK("a 1 s hold at 500 Hz is a few hundred steps", steps > 300 && steps <= 501);
	UINT32_EQUALS("every step blocked", (uint32)steps, g_ktp_push_rotate);
	UINT32_EQUALS("one held door is one episode", 1u, g_ktp_push_episodes);
	DOUBLES_EQUAL("blocked time is the time held", 1.0, g_ktp_push_blocked_time, 0.003);

	// The door swings away and comes back half a second later: a new contact.
	time += 0.5;
	ktp_push_hold_stepped(40, 3000, &time, 0.1, 500.0f, TRUE);
	UINT32_EQUALS("a real gap still opens an episode", 2u, g_ktp_push_episodes);
}

TEST(FrameRuleCountsEveryStepAsAnEpisode, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();
	double time = 100.0;

	int steps = ktp_push_hold_stepped(40, 1000, &time, 1.0, 500.0f, FALSE);

	UINT32_EQUALS("without the gap each step is its own episode", (uint32)steps, g_ktp_push_episodes);
}

TEST(EpisodeGapIsOffWithoutStepping, KtpPushTelemetry, 1000)
{
	DOUBLES_EQUAL("hz 0", 0.0, KTP_PushEpisodeGap(0.0f, 0.001), 0.0);
	DOUBLES_EQUAL("negative hz", 0.0, KTP_PushEpisodeGap(-5.0f, 0.001), 0.0);
	DOUBLES_EQUAL("500 Hz at 1 ms frames", 0.003, KTP_PushEpisodeGap(500.0f, 0.001), 0.00001);

	// Spacing just past one step plus the frame is not the same run.
	ktp_push_reset_all();
	const double gap = KTP_PushEpisodeGap(500.0f, 0.001);
	KTP_PushSampleBlocked(40, 3, ROTATE, 1000, MAP_A, 10.0, gap, 0.002);
	KTP_PushSampleBlocked(40, 3, ROTATE, 1003, MAP_A, 10.0 + gap - 1.0e-7, gap, 0.002);
	UINT32_EQUALS("inside the gap", 1u, g_ktp_push_episodes);
	KTP_PushSampleBlocked(40, 3, ROTATE, 1007, MAP_A, 10.0 + 2.0 * gap + 1.0e-6, gap, 0.002);
	UINT32_EQUALS("past the gap", 2u, g_ktp_push_episodes);
}

// One frame of a rotate+move pusher blocked in both halves lost one frame of motion.
TEST(BlockedTimeCountsAFrameOnce, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();
	g_ktp_push_blocked_time = 0.0;

	KTP_PushSampleBlocked(40, 3, ROTATE, 500, MAP_A, 5.0, 0.0, 0.001);
	KTP_PushSampleBlocked(40, 3, MOVE, 500, MAP_A, 5.0, 0.0, 0.001);
	KTP_PushSampleBlocked(41, 3, ROTATE, 500, MAP_A, 5.0, 0.0, 0.001);

	DOUBLES_EQUAL("two doors, one frame each", 0.002, g_ktp_push_blocked_time, 1.0e-9);
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

	KTP_PushSampleBlocked(40, 1, ROTATE, 100, MAP_A, 1.0, 0.0, 0.001);
	KTP_PushSampleBlocked(50, 2, MOVE, 100, MAP_A);
	for (int i = 0; i < KTP_PUSH_TRACK_N; i++)
		KTP_PushSampleBlocked(200 + i, NOT_A_CLIENT, MOVE, 100, MAP_A);

	CHECK("fixture did not populate the counters",
		g_ktp_push_rotate && g_ktp_push_move && g_ktp_push_episodes && g_ktp_push_untracked
		&& g_ktp_push_track_n && g_ktp_push_victim_slot[1] && g_ktp_push_victim_slot[2]
		&& g_ktp_push_blocked_time > 0.0);

	KTP_ProfileResetInterval();

	UINT32_EQUALS("rotate survived the reset", 0u, g_ktp_push_rotate);
	UINT32_EQUALS("move survived the reset", 0u, g_ktp_push_move);
	UINT32_EQUALS("episodes survived the reset", 0u, g_ktp_push_episodes);
	UINT32_EQUALS("untracked survived the reset", 0u, g_ktp_push_untracked);
	DOUBLES_EQUAL("blocked time survived the reset", 0.0, g_ktp_push_blocked_time, 0.0);
	LONGS_EQUAL("pusher table survived the reset", 0, g_ktp_push_track_n);
	for (int i = 0; i < MAX_CLIENTS; i++)
		UINT32_EQUALS("per-slot victims survived the reset", 0u, g_ktp_push_victim_slot[i]);
}

// The containment report's whole claim is "no block was recorded for this pusher",
// so the lookup deciding that has to agree with the table the blocks went into.
TEST(ContainBlockLookupTracksTheTable, KtpPushTelemetry, 1000)
{
	ktp_push_reset_all();

	CHECK("empty table reported a block", !KTP_PusherBlockedThisInterval(40, MAP_A));

	KTP_PushSampleBlocked(40, 1, ROTATE, 100, MAP_A);

	CHECK("a blocked pusher read as never blocked", KTP_PusherBlockedThisInterval(40, MAP_A));
	CHECK("an unrelated pusher read as blocked", !KTP_PusherBlockedThisInterval(41, MAP_A));

	// An edict index is reused by the next map, and attributing the previous map's
	// block to it would silence a real containment on this one.
	CHECK("a block carried across a changelevel", !KTP_PusherBlockedThisInterval(40, MAP_A + 1));

	// Overflow drops the pusher, so the lookup answers "no block" for a door that
	// blocked. g_ktp_contain_blind is what keeps that honest in the record.
	ktp_push_reset_all();
	for (int i = 0; i < KTP_PUSH_TRACK_N + 1; i++)
		KTP_PushSampleBlocked(100 + i, NOT_A_CLIENT, MOVE, 10 * i, MAP_A);

	UINT32_EQUALS("fixture did not overflow the table", 1u, g_ktp_push_untracked);
	CHECK("the dropped pusher is still in the table",
		!KTP_PusherBlockedThisInterval(100 + KTP_PUSH_TRACK_N, MAP_A));
}

// A counter left out of KTP_ProfileResetInterval reports a lifetime total under an
// interval label -- and here that would turn one old containment into a standing one.
TEST(IntervalResetCoversContainCounters, KtpPushTelemetry, 1000)
{
	ktp_contain_reset_all();

	g_ktp_contain_stops = 5;
	g_ktp_contain_tests = 9;
	g_ktp_contain_hits = 2;
	g_ktp_contain_unblocked = 1;
	g_ktp_contain_blind = 1;
	g_ktp_contain_last_ent = 40;
	g_ktp_contain_slot[3] = 2;
	KTP_ContainSampleHit(40, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);

	CHECK("fixture did not populate the episode counters",
		g_ktp_contain_episodes && g_ktp_contain_track_n);

	KTP_ProfileResetInterval();

	UINT32_EQUALS("stops survived the reset", 0u, g_ktp_contain_stops);
	UINT32_EQUALS("tests survived the reset", 0u, g_ktp_contain_tests);
	UINT32_EQUALS("hits survived the reset", 0u, g_ktp_contain_hits);
	UINT32_EQUALS("unblocked survived the reset", 0u, g_ktp_contain_unblocked);
	UINT32_EQUALS("blind survived the reset", 0u, g_ktp_contain_blind);
	LONGS_EQUAL("last pusher survived the reset", -1, g_ktp_contain_last_ent);
	UINT32_EQUALS("episodes survived the reset", 0u, g_ktp_contain_episodes);
	UINT32_EQUALS("untracked survived the reset", 0u, g_ktp_contain_untracked);
	LONGS_EQUAL("contained-pusher table survived the reset", 0, g_ktp_contain_track_n);
	for (int i = 0; i < MAX_CLIENTS; i++)
		UINT32_EQUALS("per-slot containment survived the reset", 0u, g_ktp_contain_slot[i]);
}

// hits= counts one per arrival at rest, so a player who simply stands in a doorway
// inflates it for as long as the door keeps being nudged. Episodes must not.
TEST(ConsecutiveArrivalsAreOneContainEpisode, KtpPushTelemetry, 1000)
{
	ktp_contain_reset_all();

	KTP_ContainSampleHit(40, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);
	for (int i = 0; i < 20; i++)
	{
		KTP_ContainSampleStop(40, MAP_A);
		KTP_ContainSampleHit(40, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);
	}

	UINT32_EQUALS("hits", 21u, g_ktp_contain_hits);
	UINT32_EQUALS("one standing player is one episode", 1u, g_ktp_contain_episodes);
	LONGS_EQUAL("distinct contained pushers", 1, g_ktp_contain_track_n);
	UINT32_EQUALS("per-pusher episodes", 1u, g_ktp_contain_track[0].episodes);
	UINT32_EQUALS("per-pusher hits", 21u, g_ktp_contain_track[0].hits);

	// The player steps out, the door arrives twice empty, and they step back in.
	KTP_ContainSampleStop(40, MAP_A);
	KTP_ContainSampleStop(40, MAP_A);
	KTP_ContainSampleHit(40, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);
	UINT32_EQUALS("a gap did not start a new episode", 2u, g_ktp_contain_episodes);

	// A second client contained on the SAME arrival is one failure of that door.
	KTP_ContainSampleHit(40, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);
	UINT32_EQUALS("same arrival opened a second episode", 2u, g_ktp_contain_episodes);
	UINT32_EQUALS("the second client's hit was dropped", 23u, g_ktp_contain_hits);

	// A different door on the same arrival is its own episode.
	KTP_ContainSampleHit(41, MAP_A, MAP_A_NAME, DOOR_CLASS, "side_gate");
	UINT32_EQUALS("episodes are per pusher", 3u, g_ktp_contain_episodes);
}

// An edict index is reused by the next map, so a containment there must not
// continue this map's entry -- that would name this door for that one's episodes.
TEST(ChangelevelDoesNotMergeContainedPushers, KtpPushTelemetry, 1000)
{
	ktp_contain_reset_all();

	KTP_ContainSampleHit(40, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);
	KTP_ContainSampleHit(40, MAP_A + 1, "dod_kalt", DOOR_CLASS, "bunker_door");

	LONGS_EQUAL("same index on a new map merged", 2, g_ktp_contain_track_n);
	UINT32_EQUALS("the new map's first containment is an episode", 2u, g_ktp_contain_episodes);
	ZSTR_EQUAL("first entry's map", MAP_A_NAME, g_ktp_contain_track[0].map);
	ZSTR_EQUAL("second entry's map", "dod_kalt", g_ktp_contain_track[1].map);
	ZSTR_EQUAL("second entry's targetname", "bunker_door", g_ktp_contain_track[1].target);
}

// The record exists to say WHICH door on WHICH map, and the edict index answers
// neither once the map has changed -- hence the copy at the hit.
TEST(ContainRecordCapturesMapAndTargetname, KtpPushTelemetry, 1000)
{
	ktp_contain_reset_all();

	KTP_ContainSampleHit(40, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);
	ZSTR_EQUAL("map", MAP_A_NAME, g_ktp_contain_track[0].map);
	ZSTR_EQUAL("classname", DOOR_CLASS, g_ktp_contain_track[0].classname);
	ZSTR_EQUAL("targetname", DOOR_TARGET, g_ktp_contain_track[0].target);

	// A door with no targetname must not read as the previous entry's door.
	KTP_ContainSampleHit(41, MAP_A, MAP_A_NAME, "func_door", NULL);
	ZSTR_EQUAL("a NULL targetname was not empty", "", g_ktp_contain_track[1].target);
	ZSTR_EQUAL("a NULL targetname disturbed the classname", "func_door", g_ktp_contain_track[1].classname);

	// Q_strncpy is strncpy: a truncated copy that is not terminated runs the
	// targetname into whatever follows it in the record.
	char longname[KTP_CONTAIN_NAME_N * 2];
	for (int i = 0; i < (int)sizeof(longname) - 1; i++)
		longname[i] = 'x';
	longname[sizeof(longname) - 1] = 0;

	KTP_ContainSampleHit(42, MAP_A, MAP_A_NAME, longname, longname);
	LONGS_EQUAL("truncated targetname was not terminated",
		KTP_CONTAIN_NAME_N - 1, (int)Q_strlen(g_ktp_contain_track[2].target));
	LONGS_EQUAL("truncated classname was not terminated",
		KTP_CONTAIN_NAME_N - 1, (int)Q_strlen(g_ktp_contain_track[2].classname));
}

// A hit the table cannot hold still happened, so it must reach the totals -- but
// it has no entry to date, so it must NOT invent an episode.
TEST(ContainOverflowCountsWithoutInventingEpisodes, KtpPushTelemetry, 1000)
{
	ktp_contain_reset_all();

	for (int i = 0; i < KTP_CONTAIN_TRACK_N + 3; i++)
		KTP_ContainSampleHit(100 + i, MAP_A, MAP_A_NAME, DOOR_CLASS, DOOR_TARGET);

	LONGS_EQUAL("table grew past its bound", KTP_CONTAIN_TRACK_N, g_ktp_contain_track_n);
	UINT32_EQUALS("overflow hits", 3u, g_ktp_contain_untracked);
	UINT32_EQUALS("overflow dropped from the total",
		(uint32)(KTP_CONTAIN_TRACK_N + 3), g_ktp_contain_hits);
	UINT32_EQUALS("an untracked hit opened an episode",
		(uint32)KTP_CONTAIN_TRACK_N, g_ktp_contain_episodes);

	// An arrival of a pusher the table never held must not touch another entry.
	KTP_ContainSampleStop(100 + KTP_CONTAIN_TRACK_N, MAP_A);
	UINT32_EQUALS("an untracked arrival changed the episode count",
		(uint32)KTP_CONTAIN_TRACK_N, g_ktp_contain_episodes);
}

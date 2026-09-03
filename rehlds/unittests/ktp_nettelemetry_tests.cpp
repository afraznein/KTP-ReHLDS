#include "precompiled.h"
#include "ktp_nettelemetry.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

extern uint32 g_ktp_net_drops;
extern uint32 g_ktp_net_latzero;
extern uint32 g_ktp_net_seen_mask;
extern uint32 g_ktp_net_lagcomp_mask;
extern float g_ktp_net_latency_peak;
extern int g_ktp_net_latency_peak_slot;
extern float g_ktp_net_ping_min[MAX_CLIENTS];
extern float g_ktp_net_ping_max[MAX_CLIENTS];
extern double g_ktp_net_ping_stamp[MAX_CLIENTS];

static const qboolean PLAYER = FALSE;
static const qboolean PROXY = TRUE;
static const qboolean LATZERO_OK = TRUE;

static void ktp_reset_all()
{
	g_ktp_net_drops = 0;
	g_ktp_net_latzero = 0;
	g_ktp_net_seen_mask = 0;
	g_ktp_net_lagcomp_mask = 0;
	g_ktp_net_latency_peak = 0.0f;
	g_ktp_net_latency_peak_slot = -1;
	g_ktp_rewind_attempts = 0;
	g_ktp_rewind_miss = 0;
	g_ktp_rewind_skip = 0;
	g_ktp_rewind_depth_peak = 0.0f;
	g_ktp_rewind_depth_slot = -1;
	g_ktp_rewind_dist_peak_sq = 0.0f;
	g_ktp_rewind_dist_slot = -1;

	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		g_ktp_net_drops_slot[i] = 0;
		g_ktp_net_latzero_slot[i] = 0;
		g_ktp_rewind_miss_slot[i] = 0;
		g_ktp_net_ping_min[i] = 9999.0f;
		g_ktp_net_ping_max[i] = -9999.0f;
		g_ktp_net_ping_stamp[i] = 0.0;
	}
}

// The bug ad8972e fixed was an HLTV proxy winning the worst-slot attribution,
// and no test could have caught it because the exclusion lived at the call
// site. It lives in the samplers now, so this is that test.
TEST(ProxyContributesNothing, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(3, PROXY, 0, 0, 0.400f, 100.0, LATZERO_OK);
	KTP_NetSampleDrops(3, PROXY, 7);
	KTP_RewindAttempt(3, PROXY);
	KTP_RewindMiss(3, PROXY);
	KTP_RewindDepth(3, PROXY, 0.250f);
	KTP_RewindSkip(PROXY);
	KTP_RewindDist(3, PROXY, 4096.0f);

	UINT32_EQUALS("proxy set seen_mask", 0u, g_ktp_net_seen_mask);
	UINT32_EQUALS("proxy set lagcomp_mask", 0u, g_ktp_net_lagcomp_mask);
	UINT32_EQUALS("proxy counted drops", 0u, g_ktp_net_drops);
	UINT32_EQUALS("proxy counted drops per slot", 0u, g_ktp_net_drops_slot[3]);
	UINT32_EQUALS("proxy counted rewind attempts", 0u, g_ktp_rewind_attempts);
	UINT32_EQUALS("proxy counted rewind misses", 0u, g_ktp_rewind_miss);
	UINT32_EQUALS("proxy counted rewind skips", 0u, g_ktp_rewind_skip);
	CHECK("proxy won latency peak", g_ktp_net_latency_peak_slot == -1);
	CHECK("proxy won depth peak", g_ktp_rewind_depth_slot == -1);
	CHECK("proxy won dist peak", g_ktp_rewind_dist_slot == -1);

	// A control: the identical calls from a non-proxy must land, or the
	// assertions above would pass against a sampler that does nothing at all.
	KTP_NetSamplePacket(3, PLAYER, 0, 0, 0.400f, 100.0, LATZERO_OK);
	KTP_NetSampleDrops(3, PLAYER, 7);
	KTP_RewindAttempt(3, PLAYER);
	KTP_RewindMiss(3, PLAYER);
	KTP_RewindDepth(3, PLAYER, 0.250f);
	KTP_RewindSkip(PLAYER);
	KTP_RewindDist(3, PLAYER, 4096.0f);

	UINT32_EQUALS("control: seen_mask", 1u << 3, g_ktp_net_seen_mask);
	UINT32_EQUALS("control: lagcomp_mask", 1u << 3, g_ktp_net_lagcomp_mask);
	UINT32_EQUALS("control: drops", 7u, g_ktp_net_drops);
	UINT32_EQUALS("control: rewind attempts", 1u, g_ktp_rewind_attempts);
	UINT32_EQUALS("control: rewind misses", 1u, g_ktp_rewind_miss);
	UINT32_EQUALS("control: rewind skips", 1u, g_ktp_rewind_skip);
	CHECK("control: depth slot", g_ktp_rewind_depth_slot == 3);
	CHECK("control: dist slot", g_ktp_rewind_dist_slot == 3);
}

// lagcomp_first has read -1 on every net_detail: line the fleet has ever
// emitted. That is the healthy value -- nobody plays with cl_lw/cl_lc off --
// but a -1 is also what a broken predicate looks like, so pin the predicate.
TEST(LagcompMaskPredicate, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(1, PLAYER, 1, 1, 0.050f, 10.0, LATZERO_OK);
	UINT32_EQUALS("lw=1 lc=1 flagged lagcomp off", 0u, g_ktp_net_lagcomp_mask);
	UINT32_EQUALS("lw=1 lc=1 not seen", 1u << 1, g_ktp_net_seen_mask);

	KTP_NetSamplePacket(2, PLAYER, 0, 1, 0.050f, 10.0, LATZERO_OK);
	UINT32_EQUALS("lw=0 did not flag", 1u << 2, g_ktp_net_lagcomp_mask);

	KTP_NetSamplePacket(4, PLAYER, 1, 0, 0.050f, 10.0, LATZERO_OK);
	UINT32_EQUALS("lc=0 did not flag", (1u << 2) | (1u << 4), g_ktp_net_lagcomp_mask);

	// Sticky: cl_lc back on mid-interval must not erase the stretch it was off.
	KTP_NetSamplePacket(4, PLAYER, 1, 1, 0.050f, 10.0, LATZERO_OK);
	UINT32_EQUALS("flag was not sticky", (1u << 2) | (1u << 4), g_ktp_net_lagcomp_mask);

	// A new occupant of slot 4 must not inherit it.
	KTP_NetSamplePacket(4, PLAYER, 1, 1, 0.050f, 77.0, LATZERO_OK);
	UINT32_EQUALS("new connection inherited the flag", 1u << 2, g_ktp_net_lagcomp_mask);
}

TEST(DropsAndLatzeroAttribution, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSampleDrops(5, PLAYER, 3);
	KTP_NetSampleDrops(9, PLAYER, 11);
	KTP_NetSampleDrops(5, PLAYER, 4);

	UINT32_EQUALS("total drops", 18u, g_ktp_net_drops);
	UINT32_EQUALS("slot 5 drops", 7u, g_ktp_net_drops_slot[5]);
	UINT32_EQUALS("slot 9 drops", 11u, g_ktp_net_drops_slot[9]);

	uint32 n = 0;
	CHECK("worst drop slot", KTP_NetWorstSlot(g_ktp_net_drops_slot, &n) == 9);
	UINT32_EQUALS("worst drop count", 11u, n);

	// net_drop is client-influenced; the clamp mirrors SV_ParseMove's replay gate.
	KTP_NetSampleDrops(5, PLAYER, 24);
	KTP_NetSampleDrops(5, PLAYER, -1);
	UINT32_EQUALS("clamp let an out-of-range net_drop through", 7u, g_ktp_net_drops_slot[5]);

	// latency 0 on an eligible client is the no-rewind case; a positive latency
	// is not, and an ineligible client is inside the post-changelevel grace.
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK);
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK);
	KTP_NetSamplePacket(7, PLAYER, 1, 1, 0.0f, 1.0, FALSE);
	KTP_NetSamplePacket(7, PLAYER, 1, 1, 0.080f, 1.0, LATZERO_OK);

	UINT32_EQUALS("latzero total", 2u, g_ktp_net_latzero);
	UINT32_EQUALS("latzero slot 6", 2u, g_ktp_net_latzero_slot[6]);
	UINT32_EQUALS("latzero counted an ineligible client", 0u, g_ktp_net_latzero_slot[7]);
}

TEST(WorstSlotSelection, KtpNetTelemetry, 1000)
{
	uint32 counts[MAX_CLIENTS];
	uint32 n = 12345;

	for (int i = 0; i < MAX_CLIENTS; i++)
		counts[i] = 0;

	CHECK("all-zero must report no slot", KTP_NetWorstSlot(counts, &n) == -1);
	UINT32_EQUALS("all-zero count", 0u, n);

	counts[MAX_CLIENTS - 1] = 5;
	CHECK("last slot unreachable", KTP_NetWorstSlot(counts, &n) == MAX_CLIENTS - 1);
	UINT32_EQUALS("last slot count", 5u, n);

	// First slot wins a tie, so the answer does not depend on array order.
	counts[2] = 5;
	counts[8] = 5;
	CHECK("tie did not go to the first slot", KTP_NetWorstSlot(counts, &n) == 2);
}

// KTP_ProfileResetInterval's own comment records that its list had already
// drifted from a second copy. A counter left out of it reports a lifetime
// total under an interval label, which reads as a spike that never ends.
TEST(IntervalResetCoversEveryCounter, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(1, PLAYER, 0, 0, 0.0f, 42.0, LATZERO_OK);
	KTP_NetSamplePacket(2, PLAYER, 1, 1, 0.300f, 42.0, LATZERO_OK);
	KTP_NetSampleDrops(1, PLAYER, 9);
	KTP_RewindAttempt(1, PLAYER);
	KTP_RewindMiss(1, PLAYER);
	KTP_RewindSkip(PLAYER);
	KTP_RewindDepth(1, PLAYER, 0.5f);
	KTP_RewindDist(1, PLAYER, 64.0f);

	// Guard against asserting a reset that had nothing to reset.
	CHECK("fixture did not populate the counters",
		g_ktp_net_drops && g_ktp_net_latzero && g_ktp_net_lagcomp_mask
		&& g_ktp_rewind_attempts && g_ktp_rewind_miss && g_ktp_rewind_skip
		&& g_ktp_net_drops_slot[1] && g_ktp_net_latzero_slot[1]
		&& g_ktp_rewind_miss_slot[1]);

	KTP_ProfileResetInterval();

	UINT32_EQUALS("drops survived the reset", 0u, g_ktp_net_drops);
	UINT32_EQUALS("latzero survived the reset", 0u, g_ktp_net_latzero);
	UINT32_EQUALS("seen_mask survived the reset", 0u, g_ktp_net_seen_mask);
	UINT32_EQUALS("lagcomp_mask survived the reset", 0u, g_ktp_net_lagcomp_mask);
	UINT32_EQUALS("rewind attempts survived the reset", 0u, g_ktp_rewind_attempts);
	UINT32_EQUALS("rewind misses survived the reset", 0u, g_ktp_rewind_miss);
	UINT32_EQUALS("rewind skips survived the reset", 0u, g_ktp_rewind_skip);
	CHECK("depth peak survived the reset", g_ktp_rewind_depth_peak == 0.0f);
	CHECK("depth slot survived the reset", g_ktp_rewind_depth_slot == -1);
	CHECK("dist peak survived the reset", g_ktp_rewind_dist_peak_sq == 0.0f);
	CHECK("dist slot survived the reset", g_ktp_rewind_dist_slot == -1);
	CHECK("latency slot survived the reset", g_ktp_net_latency_peak_slot == -1);

	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		UINT32_EQUALS("per-slot drops survived the reset", 0u, g_ktp_net_drops_slot[i]);
		UINT32_EQUALS("per-slot latzero survived the reset", 0u, g_ktp_net_latzero_slot[i]);
		UINT32_EQUALS("per-slot rewind misses survived the reset", 0u, g_ktp_rewind_miss_slot[i]);
	}

	// ping_stamp tracks connection identity, not the interval, so clearing it
	// would silently reset every ping window on the next packet.
	CHECK("reset clobbered the connection stamp", g_ktp_net_ping_stamp[1] == 42.0);
}

TEST(RewindOutcomeCounters, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_RewindAttempt(1, PLAYER);
	KTP_RewindAttempt(2, PLAYER);
	KTP_RewindAttempt(2, PLAYER);
	KTP_RewindMiss(2, PLAYER);

	// Misses are a subset of attempts; the emit site derives successes from the
	// difference, so a miss that did not also count an attempt would underflow it.
	UINT32_EQUALS("attempts", 3u, g_ktp_rewind_attempts);
	UINT32_EQUALS("misses", 1u, g_ktp_rewind_miss);
	CHECK("successes are derivable", g_ktp_rewind_attempts >= g_ktp_rewind_miss);

	uint32 n = 0;
	CHECK("worst miss slot", KTP_NetWorstSlot(g_ktp_rewind_miss_slot, &n) == 2);
	UINT32_EQUALS("worst miss count", 1u, n);

	KTP_RewindDepth(1, PLAYER, 0.120f);
	KTP_RewindDepth(4, PLAYER, 0.080f);
	CHECK("depth peak kept the smaller value", g_ktp_rewind_depth_slot == 1);

	KTP_RewindDist(1, PLAYER, 100.0f);
	KTP_RewindDist(4, PLAYER, 900.0f);
	CHECK("dist peak slot", g_ktp_rewind_dist_slot == 4);
	CHECK("dist peak is squared", g_ktp_rewind_dist_peak_sq == 900.0f);

	// Out-of-range slots must not write past the arrays.
	KTP_RewindMiss(-1, PLAYER);
	KTP_RewindMiss(MAX_CLIENTS, PLAYER);
	KTP_NetSampleDrops(MAX_CLIENTS + 4, PLAYER, 3);
	UINT32_EQUALS("out-of-range slot was counted", 1u, g_ktp_rewind_miss);
	UINT32_EQUALS("out-of-range drop was counted", 0u, g_ktp_net_drops);
}

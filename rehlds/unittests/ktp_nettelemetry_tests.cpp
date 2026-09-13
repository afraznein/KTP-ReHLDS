#include "precompiled.h"
#include "ktp_nettelemetry.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

extern uint32 g_ktp_net_drops;
extern uint32 g_ktp_net_latzero;
extern uint32 g_ktp_net_updates;
extern uint32 g_ktp_net_seen_mask;
extern uint32 g_ktp_net_lagcomp_mask;
extern float g_ktp_net_latency_peak;
extern int g_ktp_net_latency_peak_slot;
extern float g_ktp_net_ping_min[MAX_CLIENTS];
extern float g_ktp_net_ping_max[MAX_CLIENTS];
extern double g_ktp_net_ping_stamp[MAX_CLIENTS];
extern uint32 g_ktp_net_ignorecmd_hits;

static const qboolean PLAYER = FALSE;
static const qboolean PROXY = TRUE;
static const qboolean LATZERO_OK = TRUE;
static const qboolean SUB = TRUE;
static const qboolean NOT_SUB = FALSE;
static const qboolean LOADING = TRUE;
static const qboolean LOADED = FALSE;

static void ktp_reset_all()
{
	g_ktp_net_ignorecmd_hits = 0;
	g_ktp_net_loss_peak = 0.0f;
	g_ktp_net_loss_peak_slot = -1;
	g_ktp_net_subinterval = 0;
	g_ktp_net_connecting_mask = 0;
	g_ktp_net_established_mask = 0;
	g_ktp_net_synth_ms = 0;
	g_ktp_net_interp_cap_hits = 0;
	g_ktp_net_interp_floor_hits = 0;
	g_ktp_net_interp_diff_peak = 0.0f;
	g_ktp_net_interp_diff_slot = -1;
	g_ktp_net_drops = 0;
	g_ktp_net_latzero = 0;
	g_ktp_net_updates = 0;
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
		g_ktp_net_updates_slot[i] = 0;
		g_ktp_rewind_miss_slot[i] = 0;
		g_ktp_net_ping_min[i] = 9999.0f;
		g_ktp_net_ping_max[i] = -9999.0f;
		g_ktp_net_ping_stamp[i] = 0.0;
		g_ktp_net_subinterval_slot[i] = 0;
		g_ktp_net_synth_ms_slot[i] = 0;
		Q_memset(&g_ktp_net_session[i], 0, sizeof(g_ktp_net_session[i]));
	}
}

// An HLTV proxy could win the worst-slot attribution, and no test could hold
// that while the exclusion lived at the call site. It lives in the samplers
// now, so this is that test.
TEST(ProxyContributesNothing, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(3, PROXY, 0, 0, 0.400f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	// Separate call: the one above carries a latency, so it never reaches the
	// latzero branch and cannot speak for it.
	KTP_NetSamplePacket(3, PROXY, 1, 1, 0.0f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSampleDrops(3, PROXY, 7);
	KTP_NetSampleUpdate(3, PROXY);
	KTP_RewindAttempt(3, PROXY);
	KTP_RewindMiss(3, PROXY);
	KTP_RewindDepth(3, PROXY, 0.250f);
	KTP_RewindSkip(PROXY);
	KTP_RewindDist(3, PROXY, 4096.0f);

	UINT32_EQUALS("proxy set seen_mask", 0u, g_ktp_net_seen_mask);
	UINT32_EQUALS("proxy set lagcomp_mask", 0u, g_ktp_net_lagcomp_mask);
	UINT32_EQUALS("proxy counted drops", 0u, g_ktp_net_drops);
	UINT32_EQUALS("proxy counted drops per slot", 0u, g_ktp_net_drops_slot[3]);
	UINT32_EQUALS("proxy counted latzero", 0u, g_ktp_net_latzero);
	UINT32_EQUALS("proxy counted latzero per slot", 0u, g_ktp_net_latzero_slot[3]);
	UINT32_EQUALS("proxy counted updates", 0u, g_ktp_net_updates);
	UINT32_EQUALS("proxy counted updates per slot", 0u, g_ktp_net_updates_slot[3]);
	UINT32_EQUALS("proxy counted rewind attempts", 0u, g_ktp_rewind_attempts);
	UINT32_EQUALS("proxy counted rewind misses", 0u, g_ktp_rewind_miss);
	UINT32_EQUALS("proxy counted a per-slot miss", 0u, g_ktp_rewind_miss_slot[3]);
	UINT32_EQUALS("proxy counted rewind skips", 0u, g_ktp_rewind_skip);
	CHECK("proxy won latency peak", g_ktp_net_latency_peak_slot == -1);
	CHECK("proxy moved the ping window", g_ktp_net_ping_max[3] == -9999.0f);
	CHECK("proxy won depth peak", g_ktp_rewind_depth_slot == -1);
	CHECK("proxy won dist peak", g_ktp_rewind_dist_slot == -1);

	// A control: the identical calls from a non-proxy must land, or the
	// assertions above would pass against a sampler that does nothing at all.
	// Magnitudes differ from the proxy's so each peak assertion stands alone —
	// with equal values a leaked guard would make the control a silent no-op.
	KTP_NetSamplePacket(3, PLAYER, 0, 0, 0.400f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(3, PLAYER, 1, 1, 0.0f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSampleDrops(3, PLAYER, 7);
	// Twice, so the count differs from every other control magnitude here and a
	// leaked guard cannot make this assertion pass by accident.
	KTP_NetSampleUpdate(3, PLAYER);
	KTP_NetSampleUpdate(3, PLAYER);
	KTP_RewindAttempt(3, PLAYER);
	KTP_RewindMiss(3, PLAYER);
	KTP_RewindDepth(3, PLAYER, 0.500f);
	KTP_RewindSkip(PLAYER);
	KTP_RewindDist(3, PLAYER, 9000.0f);

	UINT32_EQUALS("control: seen_mask", 1u << 3, g_ktp_net_seen_mask);
	UINT32_EQUALS("control: lagcomp_mask", 1u << 3, g_ktp_net_lagcomp_mask);
	UINT32_EQUALS("control: drops", 7u, g_ktp_net_drops);
	UINT32_EQUALS("control: latzero", 1u, g_ktp_net_latzero);
	UINT32_EQUALS("control: latzero per slot", 1u, g_ktp_net_latzero_slot[3]);
	UINT32_EQUALS("control: updates", 2u, g_ktp_net_updates);
	UINT32_EQUALS("control: updates per slot", 2u, g_ktp_net_updates_slot[3]);
	UINT32_EQUALS("control: rewind attempts", 1u, g_ktp_rewind_attempts);
	UINT32_EQUALS("control: rewind misses", 1u, g_ktp_rewind_miss);
	UINT32_EQUALS("control: per-slot miss", 1u, g_ktp_rewind_miss_slot[3]);
	UINT32_EQUALS("control: rewind skips", 1u, g_ktp_rewind_skip);
	CHECK("control: depth slot", g_ktp_rewind_depth_slot == 3);
	CHECK("control: dist slot", g_ktp_rewind_dist_slot == 3);
	CHECK("control: ping window moved", g_ktp_net_ping_max[3] == 0.400f);
}

// lagcomp_first has read -1 on every net_detail: line the fleet has ever
// emitted. That is the healthy value -- nobody plays with cl_lw/cl_lc off --
// but a -1 is also what a broken predicate looks like, so pin the predicate.
TEST(LagcompMaskPredicate, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(1, PLAYER, 1, 1, 0.050f, 10.0, LATZERO_OK, NOT_SUB, LOADED);
	UINT32_EQUALS("lw=1 lc=1 flagged lagcomp off", 0u, g_ktp_net_lagcomp_mask);
	UINT32_EQUALS("lw=1 lc=1 not seen", 1u << 1, g_ktp_net_seen_mask);

	KTP_NetSamplePacket(2, PLAYER, 0, 1, 0.050f, 10.0, LATZERO_OK, NOT_SUB, LOADED);
	UINT32_EQUALS("lw=0 did not flag", 1u << 2, g_ktp_net_lagcomp_mask);

	KTP_NetSamplePacket(4, PLAYER, 1, 0, 0.050f, 10.0, LATZERO_OK, NOT_SUB, LOADED);
	UINT32_EQUALS("lc=0 did not flag", (1u << 2) | (1u << 4), g_ktp_net_lagcomp_mask);

	// Sticky: cl_lc back on mid-interval must not erase the stretch it was off.
	KTP_NetSamplePacket(4, PLAYER, 1, 1, 0.050f, 10.0, LATZERO_OK, NOT_SUB, LOADED);
	UINT32_EQUALS("flag was not sticky", (1u << 2) | (1u << 4), g_ktp_net_lagcomp_mask);

	// A new occupant of slot 4 must not inherit it.
	KTP_NetSamplePacket(4, PLAYER, 1, 1, 0.050f, 77.0, LATZERO_OK, NOT_SUB, LOADED);
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
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(7, PLAYER, 1, 1, 0.0f, 1.0, FALSE, NOT_SUB, LOADED);
	KTP_NetSamplePacket(7, PLAYER, 1, 1, 0.080f, 1.0, LATZERO_OK, NOT_SUB, LOADED);

	UINT32_EQUALS("latzero total", 2u, g_ktp_net_latzero);
	UINT32_EQUALS("latzero slot 6", 2u, g_ktp_net_latzero_slot[6]);
	UINT32_EQUALS("latzero counted an ineligible client", 0u, g_ktp_net_latzero_slot[7]);
}

// The record exists to replace an arithmetic claim about cl_updaterate with a
// count, and the per-slot total is what carries the rate -- the server total
// divides by however many clients happened to be connected, which is the answer
// to a different question.
TEST(UpdateDeliveryCounter, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	for (int i = 0; i < 10; i++)
		KTP_NetSampleUpdate(2, PLAYER);
	for (int i = 0; i < 4; i++)
		KTP_NetSampleUpdate(6, PLAYER);

	UINT32_EQUALS("server total is the sum", 14u, g_ktp_net_updates);
	UINT32_EQUALS("slot 2 total", 10u, g_ktp_net_updates_slot[2]);
	UINT32_EQUALS("slot 6 total", 4u, g_ktp_net_updates_slot[6]);

	uint32 n = 0;
	CHECK("worst slot is the busiest client", KTP_NetWorstSlot(g_ktp_net_updates_slot, &n) == 2);
	UINT32_EQUALS("worst count is that client's own total, not the server's", 10u, n);

	// Out-of-range slots must not write past the array, and the total must not
	// move either -- an unattributed increment would inflate the server figure
	// while no slot accounts for it.
	KTP_NetSampleUpdate(-1, PLAYER);
	KTP_NetSampleUpdate(MAX_CLIENTS, PLAYER);
	UINT32_EQUALS("out-of-range slot was counted", 14u, g_ktp_net_updates);
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

	KTP_NetSamplePacket(1, PLAYER, 0, 0, 0.0f, 42.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(2, PLAYER, 1, 1, 0.300f, 42.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSampleDrops(1, PLAYER, 9);
	KTP_NetSampleUpdate(1, PLAYER);
	KTP_RewindAttempt(1, PLAYER);
	KTP_RewindMiss(1, PLAYER);
	KTP_RewindSkip(PLAYER);
	KTP_RewindDepth(1, PLAYER, 0.5f);
	KTP_RewindDist(1, PLAYER, 64.0f);
	KTP_NetSamplePacket(3, PLAYER, 1, 1, 0.0f, 42.0, LATZERO_OK, SUB, LOADED);
	KTP_NetSamplePacket(4, PLAYER, 1, 1, 1.0f, 42.0, LATZERO_OK, NOT_SUB, LOADING);
	KTP_NetSampleMove(1, PLAYER, 1, 5, 2, 10);
	KTP_NetSampleLoss(1, PLAYER, 50.0f);
	KTP_NetSampleInterp(1, PLAYER, 0.2f, 0.1f, TRUE, TRUE);
	KTP_NetSampleIgnoreCmd(1, PLAYER);

	// Guard against asserting a reset that had nothing to reset.
	CHECK("fixture did not populate the counters",
		g_ktp_net_drops && g_ktp_net_latzero && g_ktp_net_updates && g_ktp_net_lagcomp_mask
		&& g_ktp_rewind_attempts && g_ktp_rewind_miss && g_ktp_rewind_skip
		&& g_ktp_net_drops_slot[1] && g_ktp_net_latzero_slot[1]
		&& g_ktp_rewind_miss_slot[1]
		&& g_ktp_net_subinterval && g_ktp_net_subinterval_slot[3]
		&& g_ktp_net_connecting_mask && g_ktp_net_established_mask
		&& g_ktp_net_synth_ms && g_ktp_net_synth_ms_slot[1]
		&& g_ktp_net_interp_cap_hits && g_ktp_net_interp_floor_hits
		&& g_ktp_net_interp_diff_slot == 1 && g_ktp_net_loss_peak_slot == 1
		&& g_ktp_net_ignorecmd_hits);

	KTP_ProfileResetInterval();

	UINT32_EQUALS("subinterval survived the reset", 0u, g_ktp_net_subinterval);
	UINT32_EQUALS("connecting_mask survived the reset", 0u, g_ktp_net_connecting_mask);
	UINT32_EQUALS("established_mask survived the reset", 0u, g_ktp_net_established_mask);
	UINT32_EQUALS("synth_ms survived the reset", 0u, g_ktp_net_synth_ms);
	UINT32_EQUALS("interp cap hits survived the reset", 0u, g_ktp_net_interp_cap_hits);
	UINT32_EQUALS("interp floor hits survived the reset", 0u, g_ktp_net_interp_floor_hits);
	CHECK("interp diff peak survived the reset", g_ktp_net_interp_diff_peak == 0.0f);
	CHECK("interp diff slot survived the reset", g_ktp_net_interp_diff_slot == -1);
	CHECK("loss slot survived the reset", g_ktp_net_loss_peak_slot == -1);
	UINT32_EQUALS("ignorecmd hits survived the reset", 0u, g_ktp_net_ignorecmd_hits);
	UINT32_EQUALS("per-slot subinterval survived the reset", 0u, g_ktp_net_subinterval_slot[3]);
	UINT32_EQUALS("per-slot synth survived the reset", 0u, g_ktp_net_synth_ms_slot[1]);
	// The session is the connection, and a reset that cleared it would emit a
	// ten-second fragment on disconnect under a whole-session label.
	CHECK("reset wiped the session", g_ktp_net_session[1].packets != 0 && g_ktp_net_session[1].cmds != 0);

	UINT32_EQUALS("drops survived the reset", 0u, g_ktp_net_drops);
	UINT32_EQUALS("latzero survived the reset", 0u, g_ktp_net_latzero);
	UINT32_EQUALS("updates survived the reset", 0u, g_ktp_net_updates);
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
		UINT32_EQUALS("per-slot updates survived the reset", 0u, g_ktp_net_updates_slot[i]);
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

// The samplers added for the session rollup and the new fields take the proxy
// flag like the old ones, so they owe the same proof, with a control that lands.
TEST(NewSamplersExcludeProxies, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(3, PROXY, 1, 1, 0.0f, 100.0, LATZERO_OK, SUB, LOADED);
	KTP_NetSamplePacket(4, PROXY, 1, 1, 0.050f, 100.0, LATZERO_OK, NOT_SUB, LOADING);
	KTP_NetSampleMove(3, PROXY, 2, 5, 2, 10);
	KTP_NetSampleLoss(3, PROXY, 90.0f);
	KTP_NetSampleInterp(3, PROXY, 0.2f, 0.1f, TRUE, FALSE);
	KTP_NetSampleIgnoreCmd(3, PROXY);

	UINT32_EQUALS("proxy counted subinterval", 0u, g_ktp_net_subinterval);
	UINT32_EQUALS("proxy set connecting_mask", 0u, g_ktp_net_connecting_mask);
	UINT32_EQUALS("proxy counted synth_ms", 0u, g_ktp_net_synth_ms);
	CHECK("proxy won loss peak", g_ktp_net_loss_peak_slot == -1);
	UINT32_EQUALS("proxy counted an interp cap", 0u, g_ktp_net_interp_cap_hits);
	CHECK("proxy won interp diff", g_ktp_net_interp_diff_slot == -1);
	UINT32_EQUALS("proxy counted ignorecmd", 0u, g_ktp_net_ignorecmd_hits);
	UINT32_EQUALS("proxy reached its session", 0u,
		g_ktp_net_session[3].packets + g_ktp_net_session[3].cmds + g_ktp_net_session[3].ignorecmd_hits);

	KTP_NetSamplePacket(3, PLAYER, 1, 1, 0.0f, 100.0, LATZERO_OK, SUB, LOADED);
	KTP_NetSamplePacket(4, PLAYER, 1, 1, 0.050f, 100.0, LATZERO_OK, NOT_SUB, LOADING);
	KTP_NetSampleMove(3, PLAYER, 2, 5, 2, 10);
	KTP_NetSampleLoss(3, PLAYER, 90.0f);
	KTP_NetSampleInterp(3, PLAYER, 0.2f, 0.1f, TRUE, FALSE);
	KTP_NetSampleIgnoreCmd(3, PLAYER);

	UINT32_EQUALS("control: subinterval", 1u, g_ktp_net_subinterval);
	UINT32_EQUALS("control: connecting_mask", 1u << 4, g_ktp_net_connecting_mask);
	UINT32_EQUALS("control: synth_ms", 30u, g_ktp_net_synth_ms);
	CHECK("control: loss slot", g_ktp_net_loss_peak_slot == 3);
	UINT32_EQUALS("control: interp cap", 1u, g_ktp_net_interp_cap_hits);
	CHECK("control: interp slot", g_ktp_net_interp_diff_slot == 3);
	UINT32_EQUALS("control: ignorecmd", 1u, g_ktp_net_ignorecmd_hits);
	UINT32_EQUALS("control: session ignorecmd", 1u, g_ktp_net_session[3].ignorecmd_hits);
}

// latzero was mostly one LAN-speed client whose RTT fits inside one update
// interval. Both branches rewind to now; only the no-sample one is a fault.
TEST(SubintervalSplitFromLatzero, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK, SUB, LOADED);
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK, SUB, LOADED);
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK, NOT_SUB, LOADED);

	UINT32_EQUALS("sub-interval packet counted as latzero", 1u, g_ktp_net_latzero);
	UINT32_EQUALS("subinterval total", 2u, g_ktp_net_subinterval);
	UINT32_EQUALS("subinterval slot 6", 2u, g_ktp_net_subinterval_slot[6]);
	UINT32_EQUALS("latzero slot 6", 1u, g_ktp_net_latzero_slot[6]);

	// The grace still mutes both, and a positive latency is neither.
	KTP_NetSamplePacket(7, PLAYER, 1, 1, 0.0f, 1.0, FALSE, SUB, LOADED);
	KTP_NetSamplePacket(7, PLAYER, 1, 1, 0.004f, 1.0, LATZERO_OK, SUB, LOADED);
	UINT32_EQUALS("grace or a positive latency counted as subinterval", 0u, g_ktp_net_subinterval_slot[7]);

	uint32 n = 0;
	CHECK("worst subinterval slot", KTP_NetWorstSlot(g_ktp_net_subinterval_slot, &n) == 6);
	UINT32_EQUALS("worst subinterval count", 2u, n);
	UINT32_EQUALS("session subinterval", 2u, g_ktp_net_session[6].subinterval);
	UINT32_EQUALS("session latzero", 1u, g_ktp_net_session[6].latzero);
}

// Loading clients ack frames no entity update stamped and posted latency maxima
// in the tens of seconds. Excluded from every latency figure, counted apart.
TEST(ConnectingClientsExcludedFromLatency, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSamplePacket(2, PLAYER, 1, 1, 30.0f, 5.0, LATZERO_OK, NOT_SUB, LOADING);
	KTP_NetSamplePacket(2, PLAYER, 1, 1, 0.0f, 5.0, LATZERO_OK, NOT_SUB, LOADING);

	CHECK("loading client won latency_worst", g_ktp_net_latency_peak_slot == -1);
	CHECK("loading client moved its ping window", g_ktp_net_ping_max[2] == -9999.0f);
	UINT32_EQUALS("loading client counted latzero", 0u, g_ktp_net_latzero);
	UINT32_EQUALS("loading client not marked connecting", 1u << 2, g_ktp_net_connecting_mask);
	UINT32_EQUALS("loading client marked established", 0u, g_ktp_net_established_mask);
	UINT32_EQUALS("loading client missing from clients", 1u << 2, g_ktp_net_seen_mask);
	UINT32_EQUALS("loading latency reached the session", 0u, g_ktp_net_session[2].latency_n);

	// The same slot once fully connected must take the ordinary path.
	KTP_NetSamplePacket(2, PLAYER, 1, 1, 0.080f, 5.0, LATZERO_OK, NOT_SUB, LOADED);
	CHECK("control: latency slot", g_ktp_net_latency_peak_slot == 2);
	CHECK("control: latency peak", g_ktp_net_latency_peak == 0.080f);
	UINT32_EQUALS("control: established", 1u << 2, g_ktp_net_established_mask);
	UINT32_EQUALS("control: session latency", 1u, g_ktp_net_session[2].latency_n);
}

TEST(LossWorstAttribution, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSampleLoss(4, PLAYER, 6.0f);
	KTP_NetSampleLoss(9, PLAYER, 95.0f);
	KTP_NetSampleLoss(4, PLAYER, 94.0f);

	CHECK("worst loss slot", g_ktp_net_loss_peak_slot == 9);
	CHECK("worst loss value is not that slot's own", g_ktp_net_loss_peak == 95.0f);

	// A tie keeps the slot that reached it first.
	KTP_NetSampleLoss(2, PLAYER, 95.0f);
	CHECK("tie moved the slot", g_ktp_net_loss_peak_slot == 9);

	KTP_NetSampleLoss(-1, PLAYER, 99.0f);
	KTP_NetSampleLoss(MAX_CLIENTS, PLAYER, 99.0f);
	CHECK("out-of-range slot moved the peak", g_ktp_net_loss_peak == 95.0f);

	DOUBLES_EQUAL("session loss sum", 100.0, g_ktp_net_session[4].loss_sum, 0.001);
	UINT32_EQUALS("session loss samples", 2u, g_ktp_net_session[4].loss_n);
	CHECK("session loss max", g_ktp_net_session[4].loss_max == 94.0f);
}

// drops counts every sequence gap, including the ones cl_cmdbackup recovers
// exactly, so it overstated warping. synth_ms counts only what ran from stale
// input or was skipped.
TEST(SynthMsCountsUnrecoveredMovement, KtpNetTelemetry, 1000)
{
	UINT32_EQUALS("gap within the backups counted as synthesised", 0u, KTP_NetSynthMs(2, 2, 10));
	UINT32_EQUALS("one command past the backups", 10u, KTP_NetSynthMs(3, 2, 10));
	UINT32_EQUALS("largest replayed gap", 210u, KTP_NetSynthMs(23, 2, 10));
	UINT32_EQUALS("a freeze runs nothing, so the whole gap counts", 240u, KTP_NetSynthMs(24, 2, 10));
	UINT32_EQUALS("freeze clamp let a crafted gap through", 60000u, KTP_NetSynthMs(0x3FFFFFFF, 2, 255));
	UINT32_EQUALS("no gap", 0u, KTP_NetSynthMs(0, 2, 10));
	UINT32_EQUALS("negative gap", 0u, KTP_NetSynthMs(-3, 2, 10));
	UINT32_EQUALS("zero msec", 0u, KTP_NetSynthMs(5, 2, 0));
	UINT32_EQUALS("negative backup count not read as none", 50u, KTP_NetSynthMs(5, -1, 10));

	ktp_reset_all();
	KTP_NetSampleMove(5, PLAYER, 1, 3, 2, 10);
	KTP_NetSampleMove(9, PLAYER, 2, 8, 2, 8);
	KTP_NetSampleMove(5, PLAYER, 1, 0, 2, 10);

	UINT32_EQUALS("drops no longer count the raw gap", 11u, g_ktp_net_drops);
	UINT32_EQUALS("server synth total", 58u, g_ktp_net_synth_ms);
	UINT32_EQUALS("slot 5 synth", 10u, g_ktp_net_synth_ms_slot[5]);
	UINT32_EQUALS("slot 9 synth", 48u, g_ktp_net_synth_ms_slot[9]);

	uint32 n = 0;
	CHECK("worst synth slot", KTP_NetWorstSlot(g_ktp_net_synth_ms_slot, &n) == 9);
	UINT32_EQUALS("worst synth ms", 48u, n);
	UINT32_EQUALS("session cmds", 2u, g_ktp_net_session[5].cmds);
	UINT32_EQUALS("session drops", 3u, g_ktp_net_session[5].drops);
	UINT32_EQUALS("session synth", 10u, g_ktp_net_session[5].synth_ms);

	// A freeze counts toward synth_ms and never toward drops, whose clamp is the replay gate.
	KTP_NetSampleMove(5, PLAYER, 1, 30, 2, 10);
	UINT32_EQUALS("freeze counted as drops", 3u, g_ktp_net_session[5].drops);
	UINT32_EQUALS("freeze missing from synth", 310u, g_ktp_net_session[5].synth_ms);
}

// SV_SetupMove rewrites the lerp a client declares in two branches; the record
// reconciles declared against used rather than trusting ex_interp.
TEST(InterpReconciliation, KtpNetTelemetry, 1000)
{
	ktp_reset_all();

	KTP_NetSampleInterp(1, PLAYER, 0.020f, 0.020f, FALSE, FALSE);
	UINT32_EQUALS("untouched lerp counted a cap", 0u, g_ktp_net_interp_cap_hits);
	UINT32_EQUALS("untouched lerp counted a floor", 0u, g_ktp_net_interp_floor_hits);
	CHECK("untouched lerp won the diff", g_ktp_net_interp_diff_slot == -1);

	KTP_NetSampleInterp(2, PLAYER, 0.250f, 0.100f, TRUE, FALSE);
	KTP_NetSampleInterp(3, PLAYER, 0.001f, 0.010f, FALSE, TRUE);
	UINT32_EQUALS("cap hits", 1u, g_ktp_net_interp_cap_hits);
	UINT32_EQUALS("floor hits", 1u, g_ktp_net_interp_floor_hits);
	CHECK("a lowered lerp did not win the diff", g_ktp_net_interp_diff_slot == 2);
	DOUBLES_EQUAL("widest gap", 0.150, g_ktp_net_interp_diff_peak, 0.0001);

	// A floor that raises the lerp is as much a rewrite as a cap that lowers it.
	KTP_NetSampleInterp(4, PLAYER, 0.0f, 0.200f, FALSE, TRUE);
	CHECK("a raised lerp did not win the diff", g_ktp_net_interp_diff_slot == 4);
}

// A worst-only series cannot say whether one client or ten sat above a line.
TEST(LatencyPopulationBuckets, KtpNetTelemetry, 1000)
{
	ktp_reset_all();
	int lat[KTP_NET_LAT_POP_N];
	int jit[KTP_NET_JIT_POP_N];

	KTP_NetSamplePacket(0, PLAYER, 1, 1, 0.003f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(1, PLAYER, 1, 1, 0.025f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(2, PLAYER, 1, 1, 0.080f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(2, PLAYER, 1, 1, 0.140f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(3, PLAYER, 1, 1, 0.450f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(4, PLAYER, 1, 1, 0.0f, 1.0, LATZERO_OK, SUB, LOADED);
	KTP_NetSamplePacket(5, PLAYER, 1, 1, 40.0f, 1.0, LATZERO_OK, NOT_SUB, LOADING);
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 12.0f, 1.0, LATZERO_OK, NOT_SUB, LOADING);
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.060f, 1.0, LATZERO_OK, NOT_SUB, LOADED);

	int connecting = KTP_NetPopulation(lat, jit);

	LONGS_EQUAL("connecting slots", 2, connecting);
	LONGS_EQUAL("lat_pop none", 1, lat[0]);
	LONGS_EQUAL("lat_pop <25", 1, lat[1]);
	LONGS_EQUAL("lat_pop <50 did not take the 25 ms edge", 1, lat[2]);
	LONGS_EQUAL("lat_pop <100", 1, lat[3]);
	LONGS_EQUAL("lat_pop <150", 1, lat[4]);
	LONGS_EQUAL("lat_pop <300", 0, lat[5]);
	LONGS_EQUAL("lat_pop 300+", 1, lat[6]);
	// The loading packet in slot 6 would otherwise widen its spread past the top edge.
	LONGS_EQUAL("jit_pop <10", 4, jit[0]);
	LONGS_EQUAL("jit_pop <25", 0, jit[1]);
	LONGS_EQUAL("jit_pop <50", 0, jit[2]);
	LONGS_EQUAL("jit_pop <100", 1, jit[3]);
	LONGS_EQUAL("jit_pop 100+", 0, jit[4]);
}

// The rollup is the connection, not the interval: a client who is never worst
// in any one window still shows up here, so it must outlive every reset.
TEST(SessionRollup, KtpNetTelemetry, 1000)
{
	ktp_reset_all();
	ktp_net_session_t s;

	KTP_NetSessionBegin(5, 100.0);
	KTP_NetSamplePacket(5, PLAYER, 1, 1, 0.050f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(5, PLAYER, 1, 1, 0.070f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_ProfileResetInterval();
	KTP_NetSamplePacket(5, PLAYER, 1, 1, 0.060f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSamplePacket(5, PLAYER, 1, 1, 0.0f, 100.0, LATZERO_OK, SUB, LOADED);
	KTP_NetSamplePacket(5, PLAYER, 1, 1, 0.0f, 100.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSampleMove(5, PLAYER, 3, 4, 2, 10);
	KTP_NetSampleLoss(5, PLAYER, 2.0f);
	KTP_NetSampleLoss(5, PLAYER, 4.0f);
	KTP_NetSampleIgnoreCmd(5, PLAYER);

	CHECK("take refused a human session", KTP_NetSessionTake(5, PLAYER, FALSE, &s) == TRUE);
	CHECK("start time", s.started == 100.0);
	UINT32_EQUALS("interval reset wiped the session", 5u, s.packets);
	UINT32_EQUALS("cmds", 3u, s.cmds);
	UINT32_EQUALS("drops", 4u, s.drops);
	UINT32_EQUALS("latzero", 1u, s.latzero);
	UINT32_EQUALS("subinterval", 1u, s.subinterval);
	UINT32_EQUALS("synth_ms", 20u, s.synth_ms);
	UINT32_EQUALS("ignorecmd_hits", 1u, s.ignorecmd_hits);
	UINT32_EQUALS("latency samples", 3u, s.latency_n);
	DOUBLES_EQUAL("latency sum", 0.180, s.latency_sum, 0.0001);
	CHECK("latency max", s.latency_max == 0.070f);
	// |70-50| + |60-70| over two deltas; the zero-latency packets do not break the chain.
	UINT32_EQUALS("jitter deltas", 2u, s.jitter_n);
	DOUBLES_EQUAL("jitter sum", 0.030, s.jitter_sum, 0.0001);
	DOUBLES_EQUAL("loss sum", 6.0, s.loss_sum, 0.0001);
	CHECK("loss max", s.loss_max == 4.0f);

	CHECK("take did not clear the slot", KTP_NetSessionTake(5, PLAYER, FALSE, &s) == FALSE);
	UINT32_EQUALS("a second take returned data", 0u, s.packets);

	// A new connection starts clean even when the old one was never taken.
	KTP_NetSamplePacket(6, PLAYER, 1, 1, 0.090f, 1.0, LATZERO_OK, NOT_SUB, LOADED);
	KTP_NetSessionBegin(6, 200.0);
	CHECK("begin kept the previous occupant's data", KTP_NetSessionTake(6, PLAYER, FALSE, &s) == FALSE);

	// Proxies and bots never report, and a refused take still clears the slot.
	KTP_NetSessionBegin(7, 1.0);
	g_ktp_net_session[7].packets = 9;
	CHECK("a proxy session was reported", KTP_NetSessionTake(7, PROXY, FALSE, &s) == FALSE);
	UINT32_EQUALS("a refused take left data behind", 0u, g_ktp_net_session[7].packets);
	g_ktp_net_session[7].cmds = 4;
	CHECK("a bot session was reported", KTP_NetSessionTake(7, PLAYER, TRUE, &s) == FALSE);
	CHECK("out-of-range take", KTP_NetSessionTake(MAX_CLIENTS, PLAYER, FALSE, &s) == FALSE);
}

/*
*	KTP: sampling seam for the [KTP_PROFILE] net:/net_detail:/rewind: records.
*	Declared here, not in per-TU extern blocks, so the tests link against the
*	same declarations the engine does -- a test carrying its own externs still
*	links after a signature drift, and then cannot fail. Slot and proxy flag are
*	explicit because HLTV reaches every one of these paths.
*/

#pragma once

// Per-slot attribution for drops and latzero. A server-wide total cannot
// separate one bursting client from twenty steady ones, and the bursts are
// what the match-night reports track.
extern uint32 g_ktp_net_drops_slot[MAX_CLIENTS];
extern uint32 g_ktp_net_latzero_slot[MAX_CLIENTS];

// Datagrams delivered per slot. cl_updaterate asks for a period; the send loop
// rounds it up to whole frames, so what a client actually receives has only
// ever been derived from the frame grid. This counts it.
extern uint32 g_ktp_net_updates;
extern uint32 g_ktp_net_updates_slot[MAX_CLIENTS];

// Rewind outcome from SV_SetupMove. net: describes the network a shot rode;
// these describe whether the rewind that judges the shot happened at all.
extern uint32 g_ktp_rewind_attempts;
extern uint32 g_ktp_rewind_miss;
extern uint32 g_ktp_rewind_skip;
extern uint32 g_ktp_rewind_miss_slot[MAX_CLIENTS];
extern float g_ktp_rewind_depth_peak;
extern int g_ktp_rewind_depth_slot;
// Squared so the hot path never calls sqrt; rooted once at the emit site.
extern float g_ktp_rewind_dist_peak_sq;
extern int g_ktp_rewind_dist_slot;

// The loss value alone recurs constantly and names nobody.
extern float g_ktp_net_loss_peak;
extern int g_ktp_net_loss_peak_slot;

// Latency 0 because the RTT fell inside one update interval, not because no
// sample existed. Kept apart so latzero means only the no-sample cases.
extern uint32 g_ktp_net_subinterval;
extern uint32 g_ktp_net_subinterval_slot[MAX_CLIENTS];

// Loading clients ack frames no entity update stamped, so their latency is noise.
extern uint32 g_ktp_net_connecting_mask;
extern uint32 g_ktp_net_established_mask;

// Movement run from stale input or skipped outright; drops also counts gaps the backups recover.
extern uint32 g_ktp_net_synth_ms;
extern uint32 g_ktp_net_synth_ms_slot[MAX_CLIENTS];

// What SV_SetupMove did to the lerp a client declared before it shifted targettime.
extern uint32 g_ktp_net_interp_cap_hits;
extern uint32 g_ktp_net_interp_floor_hits;
extern float g_ktp_net_interp_diff_peak;
extern int g_ktp_net_interp_diff_slot;

// One connection, emitted on disconnect. Outlives the interval, so the interval reset never touches it.
typedef struct ktp_net_session_s
{
	double started;
	uint32 packets;
	uint32 cmds;
	uint32 drops;
	uint32 latzero;
	uint32 subinterval;
	uint32 synth_ms;
	uint32 ignorecmd_hits;
	uint32 latency_n;
	double latency_sum;
	float latency_max;
	float latency_prev;
	uint32 jitter_n;
	double jitter_sum;
	uint32 loss_n;
	double loss_sum;
	float loss_max;
} ktp_net_session_t;
extern ktp_net_session_t g_ktp_net_session[MAX_CLIENTS];

// lat_pop leads with clients that had no positive sample, then the latency buckets.
#define KTP_NET_LAT_POP_N 7
#define KTP_NET_JIT_POP_N 5

void KTP_NetSamplePacket(int slot, qboolean proxy, int lw, int lc, float latency,
	double connection_started, qboolean latzero_eligible, qboolean subinterval, qboolean connecting);
// Returns what it counted, after the clamp.
uint32 KTP_NetSampleDrops(int slot, qboolean proxy, int net_drop);
void KTP_NetSampleMove(int slot, qboolean proxy, int numcmds, int net_drop, int numbackup, int lastcmd_msec);
void KTP_NetSampleLoss(int slot, qboolean proxy, float loss);
void KTP_NetSampleInterp(int slot, qboolean proxy, float declared, float used, qboolean capped, qboolean floored);
void KTP_NetSampleIgnoreCmd(int slot, qboolean proxy);
void KTP_NetSampleUpdate(int slot, qboolean proxy);
void KTP_RewindAttempt(int slot, qboolean proxy);
void KTP_RewindMiss(int slot, qboolean proxy);
void KTP_RewindDepth(int slot, qboolean proxy, float depth);
void KTP_RewindSkip(qboolean proxy);
void KTP_RewindDist(int slot, qboolean proxy, float dist_sq);

// Milliseconds of movement one move packet leaves to lastcmd replay or to a freeze.
uint32 KTP_NetSynthMs(int net_drop, int numbackup, int lastcmd_msec);

// Highest count and its slot, or -1 when every slot is zero. First slot wins a
// tie, so the answer does not depend on array order.
int KTP_NetWorstSlot(const uint32 *counts, uint32 *out_count);

// Fills the interval's population buckets; returns the slots seen while connecting.
int KTP_NetPopulation(int *lat_pop, int *jit_pop);

void KTP_NetSessionBegin(int slot, double now);
// Copies and clears the slot; FALSE when there is nothing a human session should report.
qboolean KTP_NetSessionTake(int slot, qboolean proxy, qboolean fakeclient, ktp_net_session_t *out);
void KTP_NetSessionEmit(client_t *cl);

// Declared rather than file-static so a test can assert the reset list covers
// every accumulator -- it has drifted from its callers before.
void KTP_ProfileResetInterval(void);

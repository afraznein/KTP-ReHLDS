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

void KTP_NetSamplePacket(int slot, qboolean proxy, int lw, int lc, float latency,
	double connection_started, qboolean latzero_eligible);
void KTP_NetSampleDrops(int slot, qboolean proxy, int net_drop);
void KTP_RewindAttempt(int slot, qboolean proxy);
void KTP_RewindMiss(int slot, qboolean proxy);
void KTP_RewindDepth(int slot, qboolean proxy, float depth);
void KTP_RewindSkip(qboolean proxy);
void KTP_RewindDist(int slot, qboolean proxy, float dist_sq);

// Highest count and its slot, or -1 when every slot is zero. First slot wins a
// tie, so the answer does not depend on array order.
int KTP_NetWorstSlot(const uint32 *counts, uint32 *out_count);

// Declared rather than file-static so a test can assert the reset list covers
// every accumulator -- it has drifted from its callers before.
void KTP_ProfileResetInterval(void);

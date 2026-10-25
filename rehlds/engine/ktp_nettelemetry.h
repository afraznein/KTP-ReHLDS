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

// One client since connect or the last ktp_net_flush. The session is the whole
// connection; this is the slice a match plugin closes at half end, so a half
// can be read against the settings the client ran in it. Two histograms: the
// client's own frame time (usercmd.msec, one count per command) and the gap
// between its move packets (one count per packet, ms, capped). Steady msec with
// a bursty gap is the upload path; a bursty msec is the client hitching.
#define KTP_NET_HIST_N 256
typedef struct ktp_net_client_s
{
	double started;
	double last_move;     // realtime of the previous move packet, 0 = none yet
	uint32 packets;
	uint32 cmds;
	uint32 drops;
	uint32 interp_cap;
	uint32 interp_floor;
	uint32 stale_delta;   // delta built from an ack older than the frame ring: the client sees FlushEntityPacket
	uint32 msec_hist[KTP_NET_HIST_N];
	uint32 arrival_hist[KTP_NET_HIST_N];
} ktp_net_client_t;
extern ktp_net_client_t g_ktp_net_client[MAX_CLIENTS];

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

void KTP_NetClientBegin(int slot, double now);
// cmds[0..numcmds-1] are this packet's new commands; dropped is the clamped count KTP_NetSampleDrops returned.
void KTP_NetClientSampleMove(int slot, qboolean proxy, const usercmd_t *cmds, int numcmds, uint32 dropped, double now);
void KTP_NetClientStaleDelta(int slot, qboolean proxy);
// Smallest bin whose cumulative count reaches pct% of total; 0 when total is 0.
uint32 KTP_HistPercentile(const uint32 *hist, int n, uint32 total, int pct);
// Prints one net_client: line per populated human slot, then restarts every slot. match/half are echoed verbatim.
void KTP_NetClientEmitAll(const char *match, const char *half, double now);
// One slot's slice, printed and restarted. `end` is appended as `end=<end>` when non-NULL.
void KTP_NetClientEmitOne(int slot, const char *match, const char *half, const char *end, double now);
// The match/half the last ktp_net_flush carried, while it is fresh; "-" otherwise. During a live
// half the plugin flushes every 10 s, so a label older than KTP_NET_LABEL_FRESH_S means no live half.
#define KTP_NET_LABEL_FRESH_S 15.0
void KTP_NetFlushLabel(double now, const char **match, const char **half);
void KTP_NetFlushRemember(const char *match, const char *half, double now);
// At disconnect: print the departing client's slice under the current label, then restart the slot.
// `reason` is SV_DropClient's own string ("Timed out", "Client sent 'drop'", a kick message).
void KTP_NetClientEmitDisconnect(client_t *cl, const char *reason, double now);
// Copies `in` to `out` with every byte outside printable ASCII made '?', so a reason cannot break the line.
void KTP_NetSanitizeReason(const char *in, char *out, int outlen);

// Declared rather than file-static so a test can assert the reset list covers
// every accumulator -- it has drifted from its callers before.
void KTP_ProfileResetInterval(void);

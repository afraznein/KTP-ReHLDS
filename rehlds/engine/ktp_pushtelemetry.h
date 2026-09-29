/*
*	KTP: sampling seam for the [KTP_PROFILE] push:/push_detail: records --
*	pushers (doors, platforms, trains) that failed to move because an entity
*	was in the way. Counters only: the physics around the call sites is stock.
*/

#pragma once

// Distinct pushers tracked per interval. A blocked door is rare, so a short
// linear table is cheaper than anything sized to max_edicts; blocks from a
// pusher that does not fit are still counted, under push_untracked.
#define KTP_PUSH_TRACK_N 16

typedef struct ktp_push_track_s
{
	int pusher;       // edict index
	int spawn;        // g_psvs.spawncount when first seen; the index means nothing on another map
	uint32 blocks;
	uint32 episodes;
	int last_frame;   // host_framecount of the latest block
} ktp_push_track_t;

extern uint32 g_ktp_push_rotate;
extern uint32 g_ktp_push_move;
// pfnBlocked fires once per blocked physics frame, so a door held shut by a
// player for a second reads as sys_ticrate blocks. An episode is a run of
// consecutive blocked frames on one pusher, which does not scale with fps.
extern uint32 g_ktp_push_episodes;
extern uint32 g_ktp_push_untracked;
extern ktp_push_track_t g_ktp_push_track[KTP_PUSH_TRACK_N];
extern int g_ktp_push_track_n;
extern uint32 g_ktp_push_victim_slot[MAX_CLIENTS];

// victim_slot is the client slot, or -1 for a non-client (a grenade, a dropped
// weapon, a proxy). Every block is counted; only client slots are attributed.
void KTP_PushSampleBlocked(int pusher, int victim_slot, qboolean rotate, int frame, int spawn);

// A proxy is FL_CLIENT too, and a slot naming the HLTV would read as a stuck player.
int KTP_PushVictimSlot(int flags, int entindex);

// Index of the tracked pusher with the most blocks, or -1 when none.
int KTP_PushWorstTrack(void);

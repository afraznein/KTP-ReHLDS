/*
*	KTP: how far the open rewind moved one player from where that player really
*	is, as a named plugin API. Fetch it with
*	RehldsFuncs_t::GetPluginApi(KTP_REWIND_SHIFT_API_V1).
*
*	A hit's shift is how far the victim had moved on their own screen by the
*	time the shot that hit them was judged: "shot behind the wall", measured.
*	On LAN it is a few units; online it grows with the shooter's latency.
*
*	Its own header and name, not a widened ktp_rewind_v1: that layout is frozen.
*	The name is the version, the same rule. An engine that does not register the
*	name returns NULL, and a consumer reads that as "no shift".
*
*	Byte-mirrored into KTPAMXX public/resdk/engine/; CI diffs the two.
*/

#pragma once

#define KTP_REWIND_SHIFT_API_V1 "ktp_rewind_shift_v1"

typedef struct ktp_rewind_shift_api_v1_s
{
	unsigned int size; // sizeof(ktp_rewind_shift_api_v1_t) as the engine was built
	// False unless the rewind record is open and belongs to client slot `shooter`,
	// and `target` is a slot (both 0-based, entity index - 1). Otherwise *units is
	// the distance in world units between the target's rewound and true origins:
	// 0 when this rewind did not move it (missed, not attempted, dead, teleported).
	bool (*GetTargetShift)(int shooter, int target, float *units);
} ktp_rewind_shift_api_v1_t;

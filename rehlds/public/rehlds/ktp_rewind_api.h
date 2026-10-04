/*
*	KTP: the rewind lag compensation set up for the packet now being run, as a
*	named plugin API. Fetch it with RehldsFuncs_t::GetPluginApi(KTP_REWIND_API_V1).
*
*	The name is the version: a layout change is registered under a new name and
*	this one is never edited. An engine that does not register the name returns
*	NULL, and a consumer reads that as "no rewind record".
*
*	Byte-mirrored into KTPAMXX public/resdk/engine/; CI diffs the two.
*/

#pragma once

#define KTP_REWIND_API_V1 "ktp_rewind_v1"

// Bits of ktp_rewind_sample_t::flags.
#define KTP_REWIND_ATTEMPTED        (1u << 0) // every lag-compensation gate passed
#define KTP_REWIND_REACHED          (1u << 1) // frame history reached the target time
#define KTP_REWIND_CLAMPED          (1u << 2) // pre-clamp latency >= sv_maxunlag
#define KTP_REWIND_HARDCAP          (1u << 3) // raw latency over the fixed 1.5 s cap
#define KTP_REWIND_PUSHED           (1u << 4) // target time clamped to realtime
#define KTP_REWIND_ESTIMATOR        (1u << 5) // sv_unlag_estimator on for this packet
#define KTP_REWIND_INTERP_ADJUSTED  (1u << 6) // interp capped at 0.1 s or floored

typedef struct ktp_rewind_sample_s
{
	unsigned int flags;
	// realtime - targettime, after the sv_maxunlag clamp. 0 unless ATTEMPTED.
	float depth_ms;
	// The same arithmetic on the pre-clamp latency. Equals depth_ms unless CLAMPED.
	float want_ms;
} ktp_rewind_sample_t;

typedef struct ktp_rewind_api_v1_s
{
	unsigned int size; // sizeof(ktp_rewind_api_v1_t) as the engine was built
	// False unless the record is open (between SV_SetupMove and SV_RestoreMove)
	// and belongs to client slot `slot` (0-based, entity index - 1).
	bool (*GetCurrent)(int slot, ktp_rewind_sample_t *out);
} ktp_rewind_api_v1_t;

/*
*	KTP: the per-packet rewind record behind the ktp_rewind_v1 plugin API.
*	Declared here so the tests hold the same functions the engine calls.
*/

#pragma once

#include "ktp_rewind_api.h"

typedef struct ktp_rewind_record_s
{
	int slot;            // owner client slot, -1 when none
	int sequence;        // owner's netchan.incoming_sequence at SV_SetupMove
	qboolean open;
	ktp_rewind_sample_t sample;
} ktp_rewind_record_t;

extern ktp_rewind_record_t g_ktp_rewind;
extern ktp_rewind_api_v1_t g_ktp_rewind_api_v1;

bool EXT_FUNC KTP_RewindGetCurrent(int slot, ktp_rewind_sample_t *out);
void KTP_RewindRegisterApi(void);

// rehlds_api_impl.cpp; the plugin-API registry has no header of its own.
void EXT_FUNC Rehlds_RegisterPluginApi(const char *name, void *impl);

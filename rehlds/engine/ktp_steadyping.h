/*
*	KTP: the sv_unlag_estimator path of SV_CalcClientTime. Declared here so the
*	tests hold the same functions the engine calls, not a copy.
*/

#pragma once

#define KTP_STEADY_PING_SAMPLES 5

typedef struct ktp_steady_ping_s
{
	double stamp;
	float samples[KTP_STEADY_PING_SAMPLES];
	int count;
	int next;
	int last_ack;
	qboolean have_ack;
	float target;
	float value;
	qboolean have_value;
} ktp_steady_ping_t;

// Per slot: client_t is ABI-exposed to the game DLL and ReAPI and never grows a field.
extern ktp_steady_ping_t g_ktp_steady_ping[MAX_CLIENTS];

// Largest change the estimate may make in one packet, in seconds.
extern const float KTP_STEADY_PING_SLEW;

// One packet. Returns the latency to rewind by, or 0 until a positive sample exists.
float KTP_SteadyPingUpdate(ktp_steady_ping_t *st, double stamp, int ack, float sample);
float KTP_SteadyPing(client_t *cl);

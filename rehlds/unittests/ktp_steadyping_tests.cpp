#include "precompiled.h"
#include "ktp_steadyping.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

// SV_CalcClientTime exactly as it stood before sv_unlag_estimator. The off path is
// held to this copy, so an edit on either side fails a test instead of a match night.
static float Reference_CalcClientTime(client_t *cl)
{
	float minping;
	float maxping;
	int backtrack;

	float ping = 0.0;
	int count = 0;
	backtrack = (int)sv_unlagsamples.value;

	if (backtrack < 1)
		backtrack = 1;

	if (backtrack >= SV_UPDATE_BACKUP)
		backtrack = SV_UPDATE_BACKUP;

	if (backtrack <= 0)
		return 0.0f;

	for (int i = 0; i < backtrack; i++)
	{
		client_frame_t *frame = &cl->frames[SV_UPDATE_MASK & (cl->netchan.incoming_acknowledged - i)];
		if (frame->ping_time <= 0.0f)
			continue;

		++count;
		ping += frame->ping_time;
	}

	if (!count)
		return 0.0f;

	minping = 9999.0;
	maxping = -9999.0;
	ping /= count;

	int jitterWindow = backtrack < SV_UPDATE_BACKUP ? backtrack : SV_UPDATE_BACKUP;
	for (int i = 0; i < jitterWindow; i++)
	{
		client_frame_t *frame = &cl->frames[SV_UPDATE_MASK & (cl->netchan.incoming_acknowledged - i)];
		if (frame->ping_time <= 0.0f)
			continue;

		if (frame->ping_time < minping)
			minping = frame->ping_time;

		if (frame->ping_time > maxping)
			maxping = frame->ping_time;
	}

	if (maxping < minping || fabs(maxping - minping) <= 0.2)
		return ping;

	return 0.0f;
}

static client_t s_ktp_clients[2];
static client_frame_t s_ktp_frames[MULTIPLAYER_BACKUP];

// Points the engine globals the estimator reads at a local two-slot server, and puts
// every one of them back, so no other test inherits this fixture.
class KtpPingFixture
{
public:
	KtpPingFixture()
	{
		m_clients = g_psvs.clients;
		m_backup = SV_UPDATE_BACKUP;
		m_mask = SV_UPDATE_MASK;
		m_samples = sv_unlagsamples.value;
		m_estimator = sv_unlag_estimator.value;

		Q_memset(s_ktp_clients, 0, sizeof(s_ktp_clients));
		Q_memset(s_ktp_frames, 0, sizeof(s_ktp_frames));
		Q_memset(g_ktp_steady_ping, 0, sizeof(g_ktp_steady_ping));
		g_psvs.clients = s_ktp_clients;
		SV_UPDATE_BACKUP = MULTIPLAYER_BACKUP;
		SV_UPDATE_MASK = MULTIPLAYER_BACKUP - 1;
		sv_unlagsamples.value = 1.0f;
		sv_unlag_estimator.value = 0.0f;

		cl = &s_ktp_clients[1];
		cl->frames = s_ktp_frames;
		cl->connection_started = 10.0;
	}

	~KtpPingFixture()
	{
		g_psvs.clients = m_clients;
		SV_UPDATE_BACKUP = m_backup;
		SV_UPDATE_MASK = m_mask;
		sv_unlagsamples.value = m_samples;
		sv_unlag_estimator.value = m_estimator;
	}

	float Packet(int ack, float ping_time)
	{
		cl->netchan.incoming_acknowledged = ack;
		cl->frames[SV_UPDATE_MASK & ack].ping_time = ping_time;
		return SV_CalcClientTime(cl);
	}

	client_t *cl;

private:
	client_t *m_clients;
	int m_backup;
	int m_mask;
	float m_samples;
	float m_estimator;
};

struct KtpPingStep
{
	int ack;
	float ping_time;
};

// Repeat acks, non-positive samples, a spike past the jitter guard's 200 ms, and a
// stretch with no positive sample at all: every branch of the stock function.
static const KtpPingStep s_ktp_sequence[] =
{
	{ 1, 0.050f }, { 2, 0.052f }, { 2, 0.061f }, { 3, 0.0f }, { 4, -0.004f },
	{ 5, 0.049f }, { 6, 0.400f }, { 7, 0.051f }, { 8, 0.0f }, { 9, 0.0f },
	{ 10, 0.0f }, { 11, -1.0f }, { 12, 0.300f }, { 13, 0.050f }, { 14, 0.047f },
	{ 14, 0.058f }, { 15, 0.003f }, { 16, -0.007f }, { 17, 0.002f }, { 18, 0.120f },
	{ 19, 0.121f }, { 20, 0.119f }, { 21, 0.0f }, { 22, 0.075f }, { 23, 0.074f },
};

TEST(OffPathIsTheOriginalFunction, KtpSteadyPing, 1000)
{
	static const float unlagsamples[] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 20.0f, 64.0f, 100.0f };
	const int steps = sizeof(s_ktp_sequence) / sizeof(s_ktp_sequence[0]);
	int compared = 0;
	int stock_zero_on_positive = 0;
	int stock_nonzero = 0;
	int estimator_diverged = 0;

	for (size_t u = 0; u < sizeof(unlagsamples) / sizeof(unlagsamples[0]); u++)
	{
		KtpPingFixture f;
		sv_unlagsamples.value = unlagsamples[u];

		for (int s = 0; s < steps; s++)
		{
			float off = f.Packet(s_ktp_sequence[s].ack, s_ktp_sequence[s].ping_time);
			float reference = Reference_CalcClientTime(f.cl);

			CHECK("off path differs from the stock function", Q_memcmp(&off, &reference, sizeof(float)) == 0);
			compared++;
			if (reference == 0.0f && s_ktp_sequence[s].ping_time > 0.0f)
				stock_zero_on_positive++;
			if (reference > 0.0f)
				stock_nonzero++;
		}

		CHECK("off path touched the estimator state", g_ktp_steady_ping[1].stamp == 0.0 && !g_ktp_steady_ping[1].have_value);

		// Control on the same frames: with the cvar on, the answer must move somewhere,
		// or the comparison above would pass against a gate that never switches.
		Q_memset(s_ktp_frames, 0, sizeof(s_ktp_frames));
		for (int s = 0; s < steps; s++)
		{
			sv_unlag_estimator.value = 0.0f;
			f.cl->netchan.incoming_acknowledged = s_ktp_sequence[s].ack;
			f.cl->frames[SV_UPDATE_MASK & s_ktp_sequence[s].ack].ping_time = s_ktp_sequence[s].ping_time;
			float reference = Reference_CalcClientTime(f.cl);
			sv_unlag_estimator.value = 1.0f;
			float on = SV_CalcClientTime(f.cl);
			if (Q_memcmp(&on, &reference, sizeof(float)) != 0)
				estimator_diverged++;
		}
	}

	CHECK("sequence did not reach the stock jitter guard or empty window", stock_zero_on_positive > 0);
	CHECK("sequence never produced a stock latency", stock_nonzero > 0);
	CHECK("estimator on never diverged from the stock function", estimator_diverged > 0);
	CHECK("nothing was compared", compared > 0);
}

TEST(KeepsLastGoodValue, KtpSteadyPing, 1000)
{
	KtpPingFixture f;
	sv_unlag_estimator.value = 1.0f;

	CHECK("returned a latency before any positive sample", f.Packet(1, 0.0f) == 0.0f);
	CHECK("first positive sample", f.Packet(2, 0.050f) == 0.050f);
	CHECK("a zero sample dropped the rewind to nothing", f.Packet(3, 0.0f) == 0.050f);
	CHECK("a sub-interval sample dropped the rewind to nothing", f.Packet(4, -0.003f) == 0.050f);
}

TEST(MedianIgnoresOneSpike, KtpSteadyPing, 1000)
{
	KtpPingFixture f;
	sv_unlag_estimator.value = 1.0f;

	for (int ack = 1; ack <= 4; ack++)
		f.Packet(ack, 0.050f);

	CHECK("one spike moved the estimate", f.Packet(5, 0.400f) == 0.050f);
	CHECK("the spike was not recorded as a sample", g_ktp_steady_ping[1].count == 5);
}

TEST(SlewLimitsChangePerPacket, KtpSteadyPing, 1000)
{
	KtpPingFixture f;
	sv_unlag_estimator.value = 1.0f;

	for (int ack = 1; ack <= 5; ack++)
		f.Packet(ack, 0.050f);

	// The route steps to 150 ms. The median follows on the third new sample, then the
	// estimate walks there one slew step per packet.
	CHECK("median followed a minority", f.Packet(6, 0.150f) == 0.050f);
	CHECK("median followed a minority", f.Packet(7, 0.150f) == 0.050f);
	DOUBLES_EQUAL("slew limit let the estimate jump", 0.070, f.Packet(8, 0.150f), 0.00001);
	DOUBLES_EQUAL("second step", 0.090, f.Packet(9, 0.150f), 0.00001);
	DOUBLES_EQUAL("third step", 0.110, f.Packet(10, 0.150f), 0.00001);

	// Repeat acks add no sample but still walk toward the target.
	DOUBLES_EQUAL("a repeat ack stalled the walk", 0.130, f.Packet(10, 0.150f), 0.00001);
	DOUBLES_EQUAL("arrived", 0.150, f.Packet(10, 0.150f), 0.00001);
	DOUBLES_EQUAL("overshot the target", 0.150, f.Packet(10, 0.150f), 0.00001);

	// Downward too.
	for (int ack = 11; ack <= 13; ack++)
		f.Packet(ack, 0.010f);
	DOUBLES_EQUAL("slew limit is one-sided", 0.130, g_ktp_steady_ping[1].value, 0.00001);
}

TEST(OneSamplePerAcknowledgedFrame, KtpSteadyPing, 1000)
{
	KtpPingFixture f;
	sv_unlag_estimator.value = 1.0f;

	f.Packet(1, 0.050f);
	f.Packet(1, 0.300f);
	CHECK("a repeat ack was sampled", g_ktp_steady_ping[1].count == 1);
	CHECK("a repeat ack moved the estimate", g_ktp_steady_ping[1].value == 0.050f);

	f.Packet(2, 0.052f);
	CHECK("a new frame was not sampled", g_ktp_steady_ping[1].count == 2);

	// A frame whose first ack was sub-interval is spent; a later ack of it is not an RTT.
	f.Packet(3, -0.002f);
	f.Packet(3, 0.004f);
	CHECK("a spent frame was sampled on a later ack", g_ktp_steady_ping[1].count == 2);
}

TEST(NewConnectionStartsClean, KtpSteadyPing, 1000)
{
	KtpPingFixture f;
	sv_unlag_estimator.value = 1.0f;

	for (int ack = 1; ack <= 5; ack++)
		f.Packet(ack, 0.200f);
	CHECK("fixture did not settle", g_ktp_steady_ping[1].value == 0.200f);

	f.cl->connection_started = 20.0;
	CHECK("the previous connection's route leaked", f.Packet(50, 0.0f) == 0.0f);
	CHECK("first sample of the new connection", f.Packet(51, 0.030f) == 0.030f);
}

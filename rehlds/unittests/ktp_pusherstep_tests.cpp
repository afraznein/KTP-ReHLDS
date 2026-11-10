#include "precompiled.h"
#include <limits>
#include "sv_phys.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

// SV_Physics_Pusher's movetime selection exactly as upstream writes it.
static float ktp_upstream_movetime(double frametime, float thinktime, float ltime)
{
	float movetime;

	if (thinktime < ltime + frametime)
	{
		movetime = thinktime - ltime;

		if (movetime < 0.0f)
			movetime = 0.0f;
	}
	else
	{
		movetime = frametime;
	}

	return movetime;
}

struct ktp_step_swing_t
{
	long thinkFrame;
	int pushes;
	float angle;
	double largestStep;
};

// A 100 deg/s door swinging 90 degrees, stepped by the plan until its think fires.
static ktp_step_swing_t ktp_step_swing(double frametime, float stepHz)
{
	ktp_step_swing_t r = { -1, 0, 0.0f, 0.0 };
	double pending = 0.0;
	float ltime = 10.0f;
	float nextthink = ltime + 0.9f;

	for (long frame = 0; frame < 100000L; frame++)
	{
		float thinktime = nextthink;
		float oldltime = ltime;
		float movetime = KTP_PusherStepPlan(&pending, frametime, thinktime, oldltime, stepHz);

		if (movetime != 0.0f)
		{
			r.angle += 100.0f * movetime;
			ltime += movetime;
			r.pushes++;
			if (movetime > r.largestStep)
				r.largestStep = movetime;
		}

		if (thinktime > oldltime && thinktime <= ltime)
		{
			r.thinkFrame = frame;
			break;
		}
	}

	return r;
}

TEST(OffIsUpstreamAndBanksNothing, KtpPusherStep, 1000)
{
	const float hz[3] = { 0.0f, -100.0f, std::numeric_limits<float>::quiet_NaN() };
	const double frames[3] = { 0.001, 0.00098, 0.01 };
	const float thinkOffsets[5] = { -1.0f, 0.0f, 0.0004f, 0.002f, 5.0f };

	for (int h = 0; h < 3; h++)
	{
		for (int f = 0; f < 3; f++)
		{
			for (int t = 0; t < 5; t++)
			{
				const float ltime = 123.0f;
				const float thinktime = ltime + thinkOffsets[t];
				double pending = 0.25;

				// volatile rounds both to float; this target otherwise compares x87 registers.
				volatile float got = KTP_PusherStepPlan(&pending, frames[f], thinktime, ltime, hz[h]);
				volatile float want = ktp_upstream_movetime(frames[f], thinktime, ltime);

				CHECK("off, the movetime is upstream's to the bit", got == want);
				DOUBLES_EQUAL("off, nothing is banked or cleared", 0.25, pending, 0.0);
			}
		}
	}
}

TEST(TenOneMillisecondFramesMakeOneTenMillisecondStep, KtpPusherStep, 1000)
{
	double pending = 0.0;
	const float ltime = 50.0f;
	const float thinktime = ltime + 5.0f;

	for (int i = 0; i < 9; i++)
	{
		float movetime = KTP_PusherStepPlan(&pending, 0.001, thinktime, ltime, 100.0f);
		CHECK("a frame short of the step does not move the door", movetime == 0.0f);
	}

	float movetime = KTP_PusherStepPlan(&pending, 0.001, thinktime, ltime, 100.0f);
	DOUBLES_EQUAL("the tenth frame pushes the whole step", 0.010, movetime, 1.0e-7);
	DOUBLES_EQUAL("and banks nothing past it", 0.0, pending, 0.0);
}

TEST(FrameAtOrAboveTheStepPushesEveryFrame, KtpPusherStep, 1000)
{
	const float ltime = 50.0f;
	const float thinktime = ltime + 5.0f;

	for (int i = 0; i < 50; i++)
	{
		double pending = 0.0;
		volatile float movetime = KTP_PusherStepPlan(&pending, 0.010, thinktime, ltime, 100.0f);
		CHECK("a 10 ms frame at 100 Hz is upstream's step", movetime == (float)0.010);

		pending = 0.0;
		movetime = KTP_PusherStepPlan(&pending, 0.001, thinktime, ltime, 1000.0f);
		CHECK("a step equal to the frame is upstream's step", movetime == (float)0.001);
		DOUBLES_EQUAL("and banks nothing", 0.0, pending, 0.0);
	}
}

// The fleet's frame does not divide 10 ms, so every step carries a fraction.
TEST(NoTimeLostOrDoubledAcrossRemainders, KtpPusherStep, 1000)
{
	const double frame = 0.00098;
	double pending = 0.0;
	double pushed = 0.0;
	float ltime = 0.5f;
	const float thinktime = 1.0e6f;
	int steps = 0;

	for (int i = 0; i < 100000; i++)
	{
		float movetime = KTP_PusherStepPlan(&pending, frame, thinktime, ltime, 100.0f);

		if (movetime != 0.0f)
		{
			CHECK("no step is shorter than the step", movetime >= 0.010 - 1.0e-6);
			CHECK("no step is a frame longer than the step", movetime < 0.010 + frame);
			pushed += movetime;
			steps++;
		}
	}

	DOUBLES_EQUAL("pushed plus banked is every frame handed in", 100000 * frame, pushed + pending, 1.0e-4);
	CHECK("about one step per 10 ms of frames", steps >= 9000 && steps <= 10000);
}

TEST(ThinkLandsOnItsTimeAndDropsTheBank, KtpPusherStep, 1000)
{
	double pending = 0.005;
	const float ltime = 20.0f;
	const float thinktime = ltime + 0.0055f;

	volatile float movetime = KTP_PusherStepPlan(&pending, 0.001, thinktime, ltime, 100.0f);
	volatile float expected = thinktime - ltime;
	CHECK("a think inside the bank pushes exactly to the think", movetime == expected);
	DOUBLES_EQUAL("the remainder is dropped, as upstream drops it", 0.0, pending, 0.0);

	pending = 0.004;
	movetime = KTP_PusherStepPlan(&pending, 0.001, ltime - 1.0f, ltime, 100.0f);
	CHECK("a stale think moves nothing", movetime == 0.0f);
	DOUBLES_EQUAL("and banks nothing", 0.0, pending, 0.0);
}

// The door must open in the same time and fire its think on the same frame as
// upstream, only in fewer, larger steps.
TEST(SwingTakesTheSameTimeInTenMillisecondSteps, KtpPusherStep, 1000)
{
	const double frames[2] = { 0.001, 0.00098 };

	for (int f = 0; f < 2; f++)
	{
		ktp_step_swing_t upstream = ktp_step_swing(frames[f], 0.0f);
		ktp_step_swing_t stepped = ktp_step_swing(frames[f], 100.0f);

		CHECK("upstream's swing finishes", upstream.thinkFrame > 0);
		CHECK("the think fires within a frame of upstream's", labs(stepped.thinkFrame - upstream.thinkFrame) <= 1);
		DOUBLES_EQUAL("the door ends on the same angle", upstream.angle, stepped.angle, 0.01f);
		DOUBLES_EQUAL("which is the planned 90 degrees", 90.0f, stepped.angle, 0.05f);
		CHECK("under a hundred pushes, not about nine hundred", stepped.pushes >= 80 && stepped.pushes <= 92);
		CHECK("no step exceeds one step plus one frame", stepped.largestStep < 0.010 + frames[f]);
	}
}

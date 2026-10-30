#include "precompiled.h"
#include "sv_phys.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

// entvars_t fields are floats in memory, so the pusher clock rounds on every
// assignment. volatile reproduces that here: this target evaluates float math
// on the x87 stack, and a plain local would keep 80-bit precision instead.
struct ktp_fake_pusher_t
{
	volatile float ltime;
	volatile float nextthink;
	volatile float angle;
};

// SV_Physics_Pusher's movetime selection plus the angle integration
// SV_PushRotate performs, run until the think fires. Returns the final angle.
static float ktp_swing_degrees(float startClock, double frametime, float degrees, float degPerSec, bool quantize)
{
	ktp_fake_pusher_t e;
	e.ltime = startClock;
	e.angle = 0.0f;
	e.nextthink = e.ltime + degrees / degPerSec;

	for (long frames = 0; frames < 200000L; frames++)
	{
		float thinktime = e.nextthink;
		float oldltime = e.ltime;
		float movetime;

		if (thinktime < oldltime + frametime)
		{
			movetime = thinktime - e.ltime;
			if (movetime < 0.0f)
				movetime = 0.0f;
		}
		else
			movetime = (float)frametime;

		if (movetime != 0.0f)
		{
			if (quantize)
				movetime = KTP_PusherClockStep(e.ltime, movetime);

			e.angle = e.angle + degPerSec * movetime;
			e.ltime = e.ltime + movetime;
		}

		if (thinktime > oldltime && thinktime <= e.ltime)
			break;
	}

	return e.angle;
}

// The step must be what the clock can actually hold, which above a few hundred
// seconds is not the step that was asked for.
TEST(StepIsWhatTheClockCanHold, KtpPusherClock, 1000)
{
	const float movetime = 0.001f;

	for (int i = 0; i < 4; i++)
	{
		const float ltime[4] = { 255.0f, 400.0f, 513.0f, 900.0f };
		float step = KTP_PusherClockStep(ltime[i], movetime);

		volatile float advanced = ltime[i] + step;
		CHECK("the clock advances by exactly the step it was handed", (float)(advanced - ltime[i]) == step);
		CHECK("a quantized step must differ from the raw one at this magnitude", step != movetime);
	}
}

// How much the clock loses is set by its own magnitude, which is why this shows
// up at an hour of map time and not at the start of one.
TEST(TheLossGrowsWithTheClock, KtpPusherClock, 1000)
{
	const float movetime = 0.001f;
	float young = fabs(KTP_PusherClockStep(2.0f, movetime) - movetime) / movetime;
	float old = fabs(KTP_PusherClockStep(513.0f, movetime) - movetime) / movetime;

	CHECK("a young clock holds a 1 ms step to well under a tenth of a percent", young < 0.001f);
	CHECK("past the 512 s boundary it loses over a percent of every step", old > 0.01f);
}

// SV_PushMove and SV_PushRotate undo a blocked push with ltime -= movetime, so
// the clock has to come back to exactly where it started.
TEST(BlockedPushRestoresTheClock, KtpPusherClock, 1000)
{
	for (int i = 0; i < 4; i++)
	{
		const float ltime[4] = { 100.0f, 400.0f, 513.0f, 900.0f };
		float step = KTP_PusherClockStep(ltime[i], 0.001f);

		volatile float advanced = ltime[i] + step;
		volatile float restored = advanced - step;
		CHECK("pfnBlocked's ltime -= movetime must land back on the original clock", (float)restored == ltime[i]);
	}
}

// The shortened final step is thinktime - ltime. Quantizing it must not stop the
// pusher short of its think time, or the think never fires and the door hangs.
TEST(ShortenedLastStepStillLandsOnThinktime, KtpPusherClock, 1000)
{
	for (int i = 0; i < 4; i++)
	{
		const float ltime[4] = { 100.0f, 400.0f, 513.0f, 900.0f };
		volatile float thinktime = ltime[i] + 0.0004f;
		float movetime = thinktime - ltime[i];
		float step = KTP_PusherClockStep(ltime[i], movetime);

		DOUBLES_EQUAL("the last step is already representable", movetime, step, 0.0);

		volatile float landed = ltime[i] + step;
		CHECK("the clock lands exactly on thinktime", (float)landed == (float)thinktime);
	}
}

// A step below half the clock's resolution would quantize to nothing. Out of
// reach at fleet uptimes, and a hung pusher if it ever were not.
TEST(StepNeverVanishes, KtpPusherClock, 1000)
{
	DOUBLES_EQUAL("a sub-resolution step falls back to the raw movetime", 0.001f,
		KTP_PusherClockStep(1.0e7f, 0.001f), 0.0);
	CHECK("a representable step at the same clock is not the fallback",
		KTP_PusherClockStep(1.0e7f, 2.0f) == 2.0f);
}

// The defect itself: a 90 degree swing of a 100 deg/s door, which is what every
// func_door_rotating in the live rotation is.
TEST(SwingLandsOnThePlannedAngle, KtpPusherClock, 1000)
{
	const float kThousandFps = 0.001f;
	const float kHundredFps = 0.010f;

	float lateRaw = ktp_swing_degrees(513.0f, kThousandFps, 90.0f, 100.0f, false);
	float earlyRaw = ktp_swing_degrees(400.0f, kThousandFps, 90.0f, 100.0f, false);
	float slowRaw = ktp_swing_degrees(513.0f, kHundredFps, 90.0f, 100.0f, false);

	CHECK("a 1 ms frame past the 512 s boundary must overshoot", lateRaw > 91.0f);
	CHECK("below it the error must take the other sign", earlyRaw < 89.9f);
	CHECK("a 10 ms frame at the same clock must not", slowRaw > 89.9f && slowRaw < 90.1f);

	for (int i = 0; i < 4; i++)
	{
		const float ltime[4] = { 100.0f, 400.0f, 513.0f, 900.0f };
		float fixed1000 = ktp_swing_degrees(ltime[i], kThousandFps, 90.0f, 100.0f, true);
		float fixed100 = ktp_swing_degrees(ltime[i], kHundredFps, 90.0f, 100.0f, true);

		DOUBLES_EQUAL("quantized, a 1 ms swing lands on its planned angle", 90.0f, fixed1000, 0.01f);
		DOUBLES_EQUAL("quantized, a 10 ms swing lands on its planned angle", 90.0f, fixed100, 0.01f);
	}
}

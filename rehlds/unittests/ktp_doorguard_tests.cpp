#include "precompiled.h"
#include <limits>
#include "sv_phys.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

// A door re-targeted the way DoD's AngularMove writes it: the private field holds the
// destination, and avelocity and nextthink are derived from it at the door's speed.
struct ktp_guard_move_t
{
	vec3_t avelocity;
	float nextthink;
	vec3_t finalAngle;
};

static ktp_guard_move_t ktp_angular_move(const vec_t *angles, float ltime, const vec_t *dest, float speed)
{
	ktp_guard_move_t m;
	vec3_t delta;
	VectorSubtract(dest, angles, delta);
	float travel = Length(delta) / speed;
	VectorScale(delta, 1.0f / travel, m.avelocity);
	m.nextthink = ltime + travel;
	VectorCopy(dest, m.finalAngle);
	return m;
}

// Every door in the rotation rests closed at 0.
static const vec3_t ktp_closed0 = { 0.0f, 0.0f, 0.0f };

static void ktp_vec(vec_t *v, float x, float y, float z)
{
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

// thunder2 #241 in the model: closing from -90 toward 0 at 100 deg/s, held at -43 by a
// wedged player, and the opener's facing sends DoorGoUp on to +90 through him.
TEST(SentOnThroughTheBlockerComesBack, KtpDoorGuard, 1000)
{
	const float ltime = 37.5f;
	vec3_t angles, closed, farSide, avelBefore;
	ktp_vec(angles, 0.0f, -43.0f, 0.0f);
	ktp_vec(closed, 0.0f, 0.0f, 0.0f);
	ktp_vec(farSide, 0.0f, 90.0f, 0.0f);

	ktp_guard_move_t closing = ktp_angular_move(angles, ltime, closed, 100.0f);
	VectorCopy(closing.avelocity, avelBefore);
	ktp_guard_move_t game = ktp_angular_move(angles, ltime, farSide, 100.0f);

	vec3_t avel, finalAngle;
	float nextthink = -1.0f;
	int v = KTP_DoorGuardPlan(angles, ltime, avelBefore, closing.nextthink,
		game.avelocity, game.nextthink, game.finalAngle, ktp_closed0, avel, &nextthink, finalAngle);

	LONGS_EQUAL("re-targeted", KTP_DOORGUARD_RETARGET, v);
	DOUBLES_EQUAL("ends on the side it came from", -90.0, finalAngle[1], 1.0e-4);
	DOUBLES_EQUAL("pitch untouched", 0.0, finalAngle[0], 1.0e-6);
	DOUBLES_EQUAL("roll untouched", 0.0, finalAngle[2], 1.0e-6);
	CHECK("turns away from the blocker", _DotProduct(avel, avelBefore) < 0.0f);
	DOUBLES_EQUAL("at the game's speed", 100.0, Length(avel), 1.0e-3);
	DOUBLES_EQUAL("arrives after 47 degrees at 100 deg/s", ltime + 0.47, nextthink, 1.0e-4);

	// The written move must pass the guard's own self-check, or the next block on it
	// would be refused as a corrupt field.
	CHECK("the move it writes is self-consistent", KTP_DoorGuardFieldMatches(angles, avel, nextthink, ltime, finalAngle));
}

TEST(MirrorsFromThePlusSide, KtpDoorGuard, 1000)
{
	const float ltime = 12.0f;
	vec3_t angles, closed, farSide;

	// Open on the + side, closing, sent on to -90.
	ktp_vec(angles, 0.0f, 30.0f, 0.0f);
	ktp_vec(closed, 0.0f, 0.0f, 0.0f);
	ktp_vec(farSide, 0.0f, -90.0f, 0.0f);
	ktp_guard_move_t closing = ktp_angular_move(angles, ltime, closed, 60.0f);
	ktp_guard_move_t game = ktp_angular_move(angles, ltime, farSide, 60.0f);

	vec3_t avel, finalAngle;
	float nextthink;
	LONGS_EQUAL("+ side", KTP_DOORGUARD_RETARGET, KTP_DoorGuardPlan(angles, ltime, closing.avelocity, closing.nextthink,
		game.avelocity, game.nextthink, game.finalAngle, ktp_closed0, avel, &nextthink, finalAngle));
	DOUBLES_EQUAL("+ side goes back to +90", 90.0, finalAngle[1], 1.0e-4);
	DOUBLES_EQUAL("60 degrees at 60 deg/s", ltime + 1.0, nextthink, 1.0e-4);
}

// Every case the guard must leave to the game, and the outputs it must not touch.
TEST(LeavesTheGameAlone, KtpDoorGuard, 1000)
{
	const float ltime = 20.0f;
	vec3_t angles, closed, open, farSide;
	ktp_vec(angles, 0.0f, -43.0f, 0.0f);
	ktp_vec(closed, 0.0f, 0.0f, 0.0f);
	ktp_vec(open, 0.0f, -90.0f, 0.0f);
	ktp_vec(farSide, 0.0f, 90.0f, 0.0f);

	vec3_t avel, finalAngle;
	float nextthink = 1234.0f;
	ktp_vec(avel, 7.0f, 7.0f, 7.0f);
	ktp_vec(finalAngle, 7.0f, 7.0f, 7.0f);

	// The 0.25 s limiter swallowed the call: nothing moved.
	ktp_guard_move_t closing = ktp_angular_move(angles, ltime, closed, 100.0f);
	LONGS_EQUAL("unchanged", KTP_DOORGUARD_UNCHANGED, KTP_DoorGuardPlan(angles, ltime, closing.avelocity, closing.nextthink,
		closing.avelocity, closing.nextthink, closing.finalAngle, ktp_closed0, avel, &nextthink, finalAngle));

	// The game picked the good side itself.
	ktp_guard_move_t good = ktp_angular_move(angles, ltime, open, 100.0f);
	LONGS_EQUAL("reversed by the game", KTP_DOORGUARD_REVERSED, KTP_DoorGuardPlan(angles, ltime, closing.avelocity, closing.nextthink,
		good.avelocity, good.nextthink, good.finalAngle, ktp_closed0, avel, &nextthink, finalAngle));

	// A blocked OPENING door: DoorGoDown heads for closed, which is always a reversal.
	ktp_guard_move_t opening = ktp_angular_move(angles, ltime, open, 100.0f);
	LONGS_EQUAL("opening door backs off", KTP_DOORGUARD_REVERSED, KTP_DoorGuardPlan(angles, ltime, opening.avelocity, opening.nextthink,
		closing.avelocity, closing.nextthink, closing.finalAngle, ktp_closed0, avel, &nextthink, finalAngle));

	// Left on the far heading by an earlier block, DoorGoDown aims at closed, which lies
	// the same way. Closed mirrors onto itself, so there is no other side to send it to.
	ktp_guard_move_t onward = ktp_angular_move(angles, ltime, farSide, 100.0f);
	LONGS_EQUAL("a close has no mirror", KTP_DOORGUARD_SKIP_MIRROR, KTP_DoorGuardPlan(angles, ltime, onward.avelocity, onward.nextthink,
		closing.avelocity, closing.nextthink, closing.finalAngle, ktp_closed0, avel, &nextthink, finalAngle));

	DOUBLES_EQUAL("nextthink untouched", 1234.0, nextthink, 0.0);
	DOUBLES_EQUAL("avel untouched", 7.0, avel[1], 0.0);
	DOUBLES_EQUAL("final untouched", 7.0, finalAngle[1], 0.0);
}

// -final is the side the door came from only when it closes at 0. A door that rests
// elsewhere would be sent to an angle no move of its own ever targets.
TEST(LeavesADoorThatDoesNotCloseAtZero, KtpDoorGuard, 1000)
{
	const float ltime = 20.0f;
	vec3_t angles, closed, farSide, avel, finalAngle;
	float nextthink = 1234.0f;
	ktp_vec(angles, 0.0f, -50.0f, 0.0f);
	ktp_vec(closed, 0.0f, 90.0f, 0.0f);
	ktp_vec(farSide, 0.0f, 180.0f, 0.0f);
	ktp_vec(finalAngle, 7.0f, 7.0f, 7.0f);

	// Heading for its closed angle at 90 and blocked on the way: DoorGoDown aims at 90 again.
	ktp_guard_move_t earlier = ktp_angular_move(angles, ltime, closed, 50.0f);
	ktp_guard_move_t closing = ktp_angular_move(angles, ltime, closed, 100.0f);
	LONGS_EQUAL("closed at 90, sent on to closed", KTP_DOORGUARD_SKIP_MIRROR, KTP_DoorGuardPlan(angles, ltime,
		earlier.avelocity, earlier.nextthink, closing.avelocity, closing.nextthink, closing.finalAngle,
		closed, avel, &nextthink, finalAngle));

	// The same geometry with a DoorGoUp re-target past the blocker is left alone too.
	ktp_guard_move_t onward = ktp_angular_move(angles, ltime, farSide, 100.0f);
	LONGS_EQUAL("closed at 90, sent on to open", KTP_DOORGUARD_SKIP_MIRROR, KTP_DoorGuardPlan(angles, ltime,
		closing.avelocity, closing.nextthink, onward.avelocity, onward.nextthink, onward.finalAngle,
		closed, avel, &nextthink, finalAngle));

	DOUBLES_EQUAL("nothing written", 1234.0, nextthink, 0.0);
	DOUBLES_EQUAL("final untouched", 7.0, finalAngle[1], 0.0);
}

// The self-check is what keeps a write into game private data honest on a field that
// does not hold what the guard believes it holds.
TEST(RefusesAFieldThatDoesNotMatch, KtpDoorGuard, 1000)
{
	const float ltime = 20.0f;
	vec3_t angles, closed, farSide;
	ktp_vec(angles, 0.0f, -43.0f, 0.0f);
	ktp_vec(closed, 0.0f, 0.0f, 0.0f);
	ktp_vec(farSide, 0.0f, 90.0f, 0.0f);
	ktp_guard_move_t closing = ktp_angular_move(angles, ltime, closed, 100.0f);
	ktp_guard_move_t game = ktp_angular_move(angles, ltime, farSide, 100.0f);

	vec3_t avel, finalAngle, field;
	float nextthink = 1234.0f;
	ktp_vec(finalAngle, 7.0f, 7.0f, 7.0f);

	const float bad[][3] = {
		{ 0.0f, 0.0f, 0.0f },     // zeroed memory
		{ 0.0f, 89.5f, 0.0f },    // half a degree off
		{ 0.0f, -90.0f, 0.0f },   // the other side already
		{ 1.0f, 90.0f, 0.0f },    // right yaw, wrong pitch
		{ 0.0f, 90.0f, 1.0e30f }, // garbage
	};

	for (size_t i = 0; i < ARRAYSIZE(bad); i++)
	{
		ktp_vec(field, bad[i][0], bad[i][1], bad[i][2]);
		LONGS_EQUAL("mismatched field", KTP_DOORGUARD_SKIP_FIELD, KTP_DoorGuardPlan(angles, ltime, closing.avelocity, closing.nextthink,
			game.avelocity, game.nextthink, field, ktp_closed0, avel, &nextthink, finalAngle));
	}

	ktp_vec(field, 0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f);
	LONGS_EQUAL("NaN field", KTP_DOORGUARD_SKIP_FIELD, KTP_DoorGuardPlan(angles, ltime, closing.avelocity, closing.nextthink,
		game.avelocity, game.nextthink, field, ktp_closed0, avel, &nextthink, finalAngle));

	// nextthink at or behind ltime is no move in progress.
	LONGS_EQUAL("nextthink in the past", KTP_DOORGUARD_SKIP_FIELD, KTP_DoorGuardPlan(angles, ltime, closing.avelocity, closing.nextthink,
		game.avelocity, ltime, game.finalAngle, ktp_closed0, avel, &nextthink, finalAngle));

	DOUBLES_EQUAL("nothing written", 1234.0, nextthink, 0.0);
	DOUBLES_EQUAL("final untouched", 7.0, finalAngle[1], 0.0);
}

// A door's clock runs with the map, so late in a long map nextthink carries float
// rounding. The real field must still pass, at any speed a door in the rotation uses.
TEST(SelfCheckHoldsLateInAMap, KtpDoorGuard, 1000)
{
	const float clocks[] = { 0.5f, 37.25f, 1800.123f, 3599.987f, 7201.3f };
	const float speeds[] = { 50.0f, 100.0f, 300.0f };

	for (size_t c = 0; c < ARRAYSIZE(clocks); c++)
	{
		for (size_t s = 0; s < ARRAYSIZE(speeds); s++)
		{
			for (int a = -89; a <= 89; a += 7)
			{
				vec3_t angles, farSide, off;
				ktp_vec(angles, 0.0f, (float)a + 0.37f, 0.0f);
				ktp_vec(farSide, 0.0f, a < 0 ? 90.0f : -90.0f, 0.0f);
				ktp_guard_move_t game = ktp_angular_move(angles, clocks[c], farSide, speeds[s]);

				CHECK("the game's own move passes",
					KTP_DoorGuardFieldMatches(angles, game.avelocity, game.nextthink, clocks[c], game.finalAngle));

				VectorCopy(game.finalAngle, off);
				off[1] += 0.5f;
				CHECK("half a degree off fails",
					!KTP_DoorGuardFieldMatches(angles, game.avelocity, game.nextthink, clocks[c], off));
			}
		}
	}
}

TEST(IdentityIsTheFleetGameDll, KtpDoorGuard, 1000)
{
	unsigned char digest[16] = {
		0x4f, 0x47, 0x27, 0xb2, 0x39, 0x0d, 0x3a, 0x0e,
		0xd6, 0xf5, 0xad, 0x86, 0x2d, 0xd6, 0xd4, 0xbe
	};
	CHECK("dod_i386.so 4f4727b2", KTP_DoorGuardIdentityMatches(digest));

	for (int byte = 0; byte < 16; byte++)
	{
		digest[byte] ^= 0x01;
		CHECK("one bit off is another build", !KTP_DoorGuardIdentityMatches(digest));
		digest[byte] ^= 0x01;
	}
}

TEST(HashesTheFileItWasHanded, KtpDoorGuard, 1000)
{
	const char *path = "ktp_doorguard_hash_test.tmp";
	FILE *f = fopen(path, "wb");
	CHECK("scratch file opens", f != NULL);
	fwrite("abc", 1, 3, f);
	fclose(f);

	unsigned char digest[16];
	CHECK("hashes", KTP_DoorGuardHashFile(path, digest));
	ZSTR_EQUAL("md5 of abc", "900150983cd24fb0d6963f7d28e17f72", MD5_Print(digest));
	remove(path);

	CHECK("a missing file is not a match", !KTP_DoorGuardHashFile(path, digest));
}

TEST(MetamodCandidatesFollowItsLoader, KtpDoorGuard, 1000)
{
	char out[KTP_DOORGUARD_CANDIDATES][KTP_DOORGUARD_PATH_MAX];

	LONGS_EQUAL("relative: gamedir first", 2, KTP_DoorGuardMetamodCandidates("dlls/dod_i386.so", "dod", out));
	ZSTR_EQUAL("install target", "dod/dlls/dod_i386.so", out[0]);
	ZSTR_EQUAL("as written", "dlls/dod_i386.so", out[1]);

	LONGS_EQUAL("absolute is taken as is", 1, KTP_DoorGuardMetamodCandidates("/opt/x/dod_i386.so", "dod", out));
	ZSTR_EQUAL("absolute", "/opt/x/dod_i386.so", out[0]);

	LONGS_EQUAL("no gamedir", 1, KTP_DoorGuardMetamodCandidates("dlls/dod_i386.so", "", out));
	ZSTR_EQUAL("no gamedir, as written", "dlls/dod_i386.so", out[0]);

	LONGS_EQUAL("unset localinfo", 0, KTP_DoorGuardMetamodCandidates("", "dod", out));
	LONGS_EQUAL("null localinfo", 0, KTP_DoorGuardMetamodCandidates(NULL, "dod", out));
}

#ifndef _WIN32
TEST(ResolvesOnlyAFileTheLinkerHasMapped, KtpDoorGuard, 1000)
{
	// libc is mapped in every process, so it stands in for the game DLL Metamod loaded.
	void *libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
	CHECK("libc is loaded", libc != NULL);
	Dl_info info;
	CHECK("libc has a path", dladdr(dlsym(libc, "fopen"), &info) && info.dli_fname && info.dli_fname[0] == '/');
	dlclose(libc);

	char *mapped = realpath(info.dli_fname, NULL);
	CHECK("libc path resolves", mapped != NULL);
	char dir[KTP_DOORGUARD_PATH_MAX];
	Q_strcpy(dir, mapped);
	char *slash = Q_strrchr(dir, '/');
	*slash = 0;

	char out[KTP_DOORGUARD_PATH_MAX];
	CHECK("relative to the game dir", KTP_DoorGuardResolveLoaded(slash + 1, dir, out, sizeof(out)));
	ZSTR_EQUAL("resolved to the mapped file", mapped, out);
	CHECK("absolute", KTP_DoorGuardResolveLoaded(mapped, "nowhere", out, sizeof(out)));
	ZSTR_EQUAL("absolute resolved", mapped, out);
	CHECK("too small a buffer is no answer", !KTP_DoorGuardResolveLoaded(mapped, "nowhere", out, 4));
	CHECK("libc is not Metamod", !KTP_DoorGuardIsMetamod(mapped));
	free(mapped);

	const char *path = "ktp_doorguard_notloaded.tmp";
	FILE *f = fopen(path, "wb");
	CHECK("scratch file opens", f != NULL);
	fwrite("abc", 1, 3, f);
	fclose(f);
	CHECK("a file that exists but is not mapped", !KTP_DoorGuardResolveLoaded(path, ".", out, sizeof(out)));
	CHECK("an unmapped file is not Metamod", !KTP_DoorGuardIsMetamod(path));
	remove(path);

	CHECK("a missing file", !KTP_DoorGuardResolveLoaded("dlls/none.so", "dod", out, sizeof(out)));
}
#endif // _WIN32

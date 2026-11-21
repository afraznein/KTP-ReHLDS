/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    In addition, as a special exception, the author gives permission to
*    link the code of this program with the Half-Life Game Engine ("HL
*    Engine") and Modified Game Libraries ("MODs") developed by Valve,
*    L.L.C ("Valve").  You must obey the GNU General Public License in all
*    respects for all of the code used other than the HL Engine and MODs
*    from Valve.  If you modify this file, you may extend this exception
*    to your version of the file, but you are not obligated to do so.  If
*    you do not wish to do so, delete this exception statement from your
*    version.
*
*/

#pragma once

#include "maintypes.h"
#include "model.h"
#include "cvar.h"

extern cvar_t sv_maxvelocity;
extern cvar_t sv_gravity;
extern cvar_t sv_bounce;
extern cvar_t sv_stepsize;
extern cvar_t sv_friction;
extern cvar_t sv_stopspeed;
extern cvar_t ktp_pusher_clock_quantize;
extern cvar_t ktp_pusher_contain_report;
extern cvar_t ktp_pusher_step_hz;
extern cvar_t ktp_door_reversal_guard;

extern vec3_t *g_moved_from;
extern edict_t **g_moved_edict;

NOXREF void SV_CheckAllEnts();
void SV_CheckVelocity(edict_t *ent);
qboolean SV_RunThink(edict_t *ent);
void SV_Impact(edict_t *e1, edict_t *e2, trace_t *ptrace);
void SV_ClipVelocity(vec_t *in, vec_t *normal, vec_t *out, float overbounce);
void SV_FlyMove(edict_t *ent, float time, float bounce);
void SV_AddGravity(edict_t *ent);
NOXREF void SV_AddCorrectGravity(edict_t *ent);
NOXREF void SV_FixupGravityVelocity(edict_t *ent);
trace_t SV_PushEntity(edict_t *ent, vec_t *push);
void SV_PushMove(edict_t *pusher, float movetime);
qboolean SV_PushRotate(edict_t *pusher, float movetime);
float KTP_PusherClockStep(float ltime, float movetime);
float KTP_PusherStepPlan(double *pending, double frametime, float thinktime, float ltime, float stepHz);

// KTP_DoorGuardPlan verdicts. Only RETARGET fills the out parameters.
#define KTP_DOORGUARD_UNCHANGED   0 // the game did not re-target
#define KTP_DOORGUARD_REVERSED    1 // the game turned the door back by itself
#define KTP_DOORGUARD_RETARGET    2 // sent on through the blocker; out* hold the move back
#define KTP_DOORGUARD_SKIP_FIELD  3 // sent on, but the private field failed its self-check
#define KTP_DOORGUARD_SKIP_MIRROR 4 // sent on, but the door does not close at 0 or the other side does not turn it back
int KTP_DoorGuardPlan(const vec_t *angles, float ltime,
	const vec_t *avelBefore, float nextthinkBefore,
	const vec_t *avelAfter, float nextthinkAfter, const vec_t *finalAngle,
	const vec_t *closedAngle,
	vec_t *outAvel, float *outNextthink, vec_t *outFinal);
qboolean KTP_DoorGuardFieldMatches(const vec_t *angles, const vec_t *avelocity, float nextthink, float ltime,
	const vec_t *field);
qboolean KTP_DoorGuardIdentityMatches(const unsigned char digest[16]);
qboolean KTP_DoorGuardHashFile(const char *path, unsigned char digest[16]);
#define KTP_DOORGUARD_PATH_MAX 4096
#define KTP_DOORGUARD_CANDIDATES 2
int KTP_DoorGuardMetamodCandidates(const char *mmGamedll, const char *gameDir,
	char out[KTP_DOORGUARD_CANDIDATES][KTP_DOORGUARD_PATH_MAX]);
qboolean KTP_DoorGuardIsMetamod(const char *path);
qboolean KTP_DoorGuardResolveLoaded(const char *mmGamedll, const char *gameDir, char *out, int outSize);
void KTP_DoorGuardNoteGameDll(const char *path);
void SV_Physics_Pusher(edict_t *ent);
qboolean SV_CheckWater(edict_t *ent);
float SV_RecursiveWaterLevel(const vec_t *origin, float mins, float maxs, int depth);
float SV_Submerged(edict_t *ent);
void SV_Physics_None(edict_t *ent);
void SV_Physics_Follow(edict_t *ent);
void SV_Physics_Noclip(edict_t *ent);
void SV_CheckWaterTransition(edict_t *ent);
void SV_Physics_Toss(edict_t *ent);
void PF_WaterMove(edict_t *pSelf);
void SV_Physics_Step(edict_t *ent);
void SV_Physics(void);
trace_t SV_Trace_Toss(edict_t *ent, edict_t *ignore);

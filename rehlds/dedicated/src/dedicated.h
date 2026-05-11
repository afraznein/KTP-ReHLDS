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

#define LAUNCHER_ERROR	-1
#define LAUNCHER_OK		0

typedef void (*NET_Sleep_t)();
typedef void (*SleepType)(int msec);

extern bool g_bVGui;
extern IDedicatedServerAPI *engineAPI;

// KTP Stage C: set true by Sys_InitPingboost when -pingboost 2 is selected.
// Signals the main loop in sys_ded.cpp to use clock_nanosleep(TIMER_ABSTIME)
// against a tracked 1ms grid target instead of the fixed sys->Sleep(1).
// Linux-only path; Windows builds always go through the legacy sys->Sleep(1).
extern bool g_use_abs_grid;

// KTP Stage C debug instrumentation (additive to -absgrid). When set, the
// main loop in sys_ded.cpp samples 3 timestamps per iteration (top, after
// sleep, after RunFrame) and emits a [KTP_ABSGRID_PROBE] histogram every
// 10s tagging sleep_us, work_us, and total_us bucket counts + min/max.
// Diagnostic for the 2026-05-11 finding that absgrid measures 696 fps vs
// the 980 fps that loop math predicts (414µs/iter unaccounted in the
// engine layer). Opt-in via -absgrid_probe; zero overhead when off.
extern bool g_absgrid_probe;

bool Sys_SetupConsole();
void Sys_PrepareConsoleInput();
void Sys_InitPingboost();
void Sys_WriteProcessIdFile();

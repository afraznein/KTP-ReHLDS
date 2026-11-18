"""Deterministic model of one rotating-door hold on a real map's collision hulls.

A model, not the engine: see README.md beside this file for what it reproduces and what
it leaves out.

Engine side follows sv_phys.cpp SV_Physics_Pusher / SV_PushRotate: step the door, give a
player now in solid the rigid rotation push (one world trace, door non-solid), and if he
is still in solid revert the step and call Blocked.
Game side follows the fleet's dod_i386.so (md5 4f4727b2390d3a0ed6f5ad862dd6d4be):
  * Blocked() acts at most once per 0.25 s of server time; other calls do nothing.
  * DoorGoUp() takes the swing side from the activator's facing, then negates it only when
    entered from TS_GOING_DOWN with angles.y < 0.
Solidity uses the BSP's own clipnode hulls; the door test rotates the player's origin into
the door frame, which is the engine's rotated-brush approximation (--exact replaces it).

usage: python doorsim.py <bsp> <door-entity-lump-index> <vx> <vy> <vz> [options]
"""
import argparse, math, struct, re

DIST_EPSILON = 0.03125
TS_AT_TOP, TS_AT_BOTTOM, TS_GOING_UP, TS_GOING_DOWN = 0, 1, 2, 3


class Bsp:
    def __init__(self, path):
        d = open(path, 'rb').read()
        lumps = [struct.unpack_from('<ii', d, 4 + 8 * i) for i in range(15)]
        po, pl = lumps[1]
        self.planes = [struct.unpack_from('<4fi', d, po + 20 * i) for i in range(pl // 20)]
        co, cl = lumps[9]
        self.clip = [struct.unpack_from('<ihh', d, co + 8 * i) for i in range(cl // 8)]
        mo, ml = lumps[14]
        self.models = [struct.unpack_from('<9f4i3i', d, mo + 64 * i) for i in range(ml // 64)]
        eo, el = lumps[0]
        txt = d[eo:eo + el].decode('latin-1')
        self.ents = [dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', b)) for b in re.findall(r'\{([^{}]*)\}', txt)]

    def contents(self, head, p):
        num = head
        while num >= 0:
            pn, c0, c1 = self.clip[num]
            nx, ny, nz, dist, typ = self.planes[pn]
            dd = (p[typ] - dist) if typ < 3 else (nx * p[0] + ny * p[1] + nz * p[2] - dist)
            num = c0 if dd >= 0 else c1
        return num


class Sim:
    def __init__(self, a):
        self.a = a
        self.bsp = Bsp(a.bsp)
        e = self.bsp.ents[a.door]
        assert e.get('classname') == 'func_door_rotating', e.get('classname')
        self.o = [float(x) for x in e['origin'].split()]
        m = self.bsp.models[int(e['model'][1:])]
        self.hull = a.hull
        self.dmins, self.dmaxs, self.dhull = m[0:3], m[3:6], m[9 + a.hull]
        self.world_hull = self.bsp.models[0][9 + a.hull]
        self.half_h = 36 if a.hull == 1 else 18
        self.speed = float(e.get('speed', 100))
        self.wait = float(e.get('wait', 4))
        sf = int(e.get('spawnflags', 0))
        self.movedir_y = -1.0 if sf & 2 else 1.0
        self.angle2 = self.movedir_y * float(e.get('distance', 90))
        self.oneway = bool(sf & 16)
        self.v = [a.vx, a.vy, a.vz]
        self.act_pos = list(self.v) if a.act_pos is None else a.act_pos
        self.act_yaw = a.yaw
        # door state: open on the side given, about to close
        self.yaw = a.start_yaw
        self.avel = 0.0
        self.ltime = 0.0
        self.nextthink = a.close_at
        self.think = 'goup' if a.open else 'godown'
        self.movedone = None
        self.final = 0.0
        self.state = TS_AT_BOTTOM if a.open else TS_AT_TOP
        self.last_blocked = -1e9
        self.time = 0.0

    # ---- collision -------------------------------------------------------------
    def door_solid(self, p, yaw):
        tx, ty, tz = p[0] - self.o[0], p[1] - self.o[1], p[2] - self.o[2]
        cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        lx, ly = cy * tx + sy * ty, -sy * tx + cy * ty
        if self.a.exact:
            # true box-vs-rotated-box: player AABB (16,16,36) against the leaf box
            if abs(tz - (self.dmins[2] + self.dmaxs[2]) / 2) >= self.half_h + (self.dmaxs[2] - self.dmins[2]) / 2:
                return False
            c = [((self.dmins[0] + self.dmaxs[0]) / 2), ((self.dmins[1] + self.dmaxs[1]) / 2)]
            h = [(self.dmaxs[0] - self.dmins[0]) / 2, (self.dmaxs[1] - self.dmins[1]) / 2]
            # leaf centre in world
            wc = (self.o[0] + cy * c[0] - sy * c[1], self.o[1] + sy * c[0] + cy * c[1])
            dx, dy = p[0] - wc[0], p[1] - wc[1]
            ax = [(1, 0), (0, 1), (cy, sy), (-sy, cy)]
            for (ux, uy) in ax:
                rp = 16 * abs(ux) + 16 * abs(uy)
                rl = h[0] * abs(ux * cy + uy * sy) + h[1] * abs(-ux * sy + uy * cy)
                if abs(dx * ux + dy * uy) >= rp + rl:
                    return False
            return True
        return self.bsp.contents(self.dhull, (lx, ly, tz)) == -2

    def world_solid(self, p):
        return self.bsp.contents(self.world_hull, p) == -2

    def in_solid(self, p, yaw):
        return self.world_solid(p) or self.door_solid(p, yaw)

    def push_entity(self, p, push):
        if self.world_solid(p):
            return p
        L = math.sqrt(sum(c * c for c in push))
        if L == 0:
            return p
        lo, hi = 0.0, 1.0
        end = [p[i] + push[i] for i in range(3)]
        hit = False
        for k in range(1, 17):
            f = k / 16.0
            if self.world_solid([p[i] + push[i] * f for i in range(3)]):
                hi, hit = f, True
                break
            lo = f
        if not hit:
            return end
        for _ in range(30):
            mid = (lo + hi) / 2
            if self.world_solid([p[i] + push[i] * mid for i in range(3)]):
                hi = mid
            else:
                lo = mid
        f = max(0.0, lo - DIST_EPSILON / L)
        return [p[i] + push[i] * f for i in range(3)]

    # ---- game DLL (dod_i386.so) ------------------------------------------------
    def angular_move(self, dest, movedone):
        self.movedone = movedone
        self.final = dest
        delta = dest - self.yaw
        if delta == 0:
            self.angular_move_done()
            return
        travel = abs(delta) / self.speed
        self.nextthink = self.ltime + travel
        self.think = 'movedone'
        self.avel = delta / travel

    def angular_move_done(self):
        self.yaw = self.final
        self.avel = 0.0
        self.nextthink = -1
        self.think = None
        if self.movedone == 'hittop':
            self.state = TS_AT_TOP
            self.nextthink = self.ltime + self.wait
            self.think = 'godown'
        elif self.movedone == 'hitbottom':
            self.state = TS_AT_BOTTOM

    def door_go_up(self):
        was_down = self.state == TS_GOING_DOWN
        self.state = TS_GOING_UP
        sign = 1.0
        if not self.oneway:
            vx, vy = self.act_pos[0] - self.o[0], self.act_pos[1] - self.o[1]
            fx, fy = math.cos(math.radians(self.act_yaw)), math.sin(math.radians(self.act_yaw))
            nx, ny = vx + 10 * fx, vy + 10 * fy
            if vx * ny - vy * nx < 0:
                sign = -1.0
        if was_down and self.yaw < 0 and not self.a.sdk:
            sign = -sign
        if self.a.keep_side and was_down and self.yaw != 0:
            sign = 1.0 if (self.yaw > 0) == (self.angle2 > 0) else -1.0
        self.angular_move(self.angle2 * sign, 'hittop')

    def door_go_down(self):
        self.state = TS_GOING_DOWN
        self.angular_move(0.0, 'hitbottom')

    def blocked(self):
        if not self.a.sdk:
            if self.time - self.last_blocked < 0.25:
                return
            self.last_blocked = self.time
        if self.wait >= 0:
            if self.state == TS_GOING_DOWN:
                self.door_go_up()
            else:
                self.door_go_down()

    # ---- engine ----------------------------------------------------------------
    def push_rotate(self, movetime):
        amove = self.avel * movetime
        old = self.yaw
        self.yaw = old + amove
        self.ltime += movetime
        p = self.v
        # broad phase is the door's absbox; the narrow test is SV_TestEntityPosition
        if not self.in_solid(p, self.yaw):
            return True
        t = [p[0] - self.o[0], p[1] - self.o[1], p[2] - self.o[2]]
        c, s = math.cos(math.radians(amove)), math.sin(math.radians(amove))
        end = [c * t[0] - s * t[1], s * t[0] + c * t[1], t[2]]
        push = [end[0] - t[0], end[1] - t[1], 0.0]
        newp = self.push_entity(p, push)
        if self.in_solid(newp, self.yaw):
            self.yaw = old
            self.ltime -= movetime
            return False
        self.v = newp
        return True

    def run(self, seconds, dt):
        steps = int(round(seconds / dt))
        blocked = 0
        trace = []
        for k in range(steps):
            self.time = (k + 1) * dt
            movetime = dt
            if self.think and self.nextthink > 0 and self.nextthink < self.ltime + dt:
                movetime = max(0.0, self.nextthink - self.ltime)
            if movetime and self.avel:
                if not self.push_rotate(movetime):
                    blocked += 1
                    self.blocked()
            elif movetime:
                self.ltime += movetime
            if self.think and self.nextthink > 0 and self.nextthink <= self.ltime:
                th = self.think
                self.think = None
                if th == 'movedone':
                    self.angular_move_done()
                elif th == 'goup':
                    self.door_go_up()
                elif th == 'godown':
                    self.door_go_down()
            if k % int(round(0.25 / dt)) == 0:
                trace.append((round(self.time, 3), round(self.yaw, 2), self.state, blocked))
        return blocked, trace


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bsp'); ap.add_argument('door', type=int)
    ap.add_argument('vx', type=float); ap.add_argument('vy', type=float); ap.add_argument('vz', type=float)
    ap.add_argument('--yaw', type=float, default=0.0, help='activator facing, degrees')
    ap.add_argument('--act-pos', type=float, nargs=3, default=None, help='activator origin (default: the victim)')
    ap.add_argument('--start-yaw', type=float, default=-90.0, help='door angle when it starts to close')
    ap.add_argument('--open', action='store_true', help='start closed (at --start-yaw) and let the activator open it')
    ap.add_argument('--close-at', type=float, default=0.1)
    ap.add_argument('--seconds', type=float, default=10.0)
    ap.add_argument('--hz', type=float, default=500.0, help='pusher step rate (ktp_pusher_step_hz; 1000 ~ per-frame fleet, 100 = legacy)')
    ap.add_argument('--exact', action='store_true', help='true rotated-box collision instead of the engine approximation')
    ap.add_argument('--sdk', action='store_true', help='stock HLSDK door: no 0.25 s Blocked limiter, no DoD sign flip')
    ap.add_argument('--keep-side', action='store_true', help='proposed: a blocked close reopens on the side it came from')
    ap.add_argument('--sweep', action='store_true', help='sweep activator facing 0..345 in 15 degree steps')
    ap.add_argument('--hull', type=int, default=1, choices=(1, 3), help='1 standing, 3 ducking')
    ap.add_argument('--trace', action='store_true')
    a = ap.parse_args()
    yaws = range(0, 360, 15) if a.sweep else [a.yaw]
    for y in yaws:
        a.yaw = float(y)
        s = Sim(a)
        if s.in_solid(s.v, s.yaw):
            print('victim start position is in solid at door yaw %.1f' % s.yaw)
            return
        b, tr = s.run(a.seconds, 1.0 / a.hz)
        print('activator_yaw=%5.1f blocked_steps=%5d blocked_s=%.3f final_door_yaw=%.2f victim=(%.2f %.2f %.2f)' % (
            a.yaw, b, b / a.hz, s.yaw, *s.v))
        if a.trace:
            for t in tr:
                print('   t=%.3f yaw=%.2f state=%d blocked=%d' % t)


if __name__ == '__main__':
    main()

# -*- coding: utf-8 -*-
# 일반도로 행 검산기 (2026-10-04 최정우 — 사용자 기준 "매칭된 출발 tick ~ 도착 tick, 링크 진입이면 링크 시작부터,
#   누락 링크 복구" 로 거리·체류시간·평균속도를 엔진과 독립적으로 다시 잰다)
# 사용: python3 nsdist.py <trip_id> <start_seq> <end_seq> <trip|link> [truth]
#   trip  = 출발 tick 매칭점부터
#   link  = 출발 tick 링크의 시작 노드부터(시각은 직전 tick 과의 보간)
#   truth = 매칭점 대신 ruc.sim_truth 정답 좌표·링크 사용(시나리오 트립 전용)
# 방식: 매칭점을 링크 형상에 투영해 링크를 따라 재고, 링크가 바뀌면 최단경로로 중간 링크를 복구한다.
# 출력 2줄: [도착 tick 이후 링크 끝 노드까지 잔여] / 거리·체류·평균속도·FROM/TO·복구 링크
import sys, math, heapq, collections, datetime
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import park_scenario as ps
trip, s, e, mode = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
use_truth = len(sys.argv) > 5
con = ps.connect(); cur = con.cursor()
if use_truth:
    cur.execute("""select r.gps_seq, r.gps_dt, t.true_link_id, t.true_lat, t.true_lon from ruc.prim_rawgps r
                   join ruc.sim_truth t on t.trip_id=r.trip_id and t.gps_seq=r.gps_seq where r.trip_id=%s order by 1""", (trip,))
else:
    cur.execute("""select gps_seq, gps_dt, match_link_id, match_lat, match_lon from ruc.prim_rawgps
                   where trip_id=%s and match_status=1 and match_link_id is not null order by 1""", (trip,))
ticks = [(int(a), datetime.datetime.strptime(b, '%Y%m%d%H%M%S'), int(c), float(d), float(f)) for a, b, c, d, f in cur.fetchall()]
lon0, lat0 = ticks[0][4], ticks[0][3]
cur.execute("""select link_id, f_node, t_node, coords, length_m from ruc.road_link
               where max_lon >= %s and min_lon <= %s and max_lat >= %s and min_lat <= %s""", (lon0-.05, lon0+.05, lat0-.05, lat0+.05))
L = {}; out_of = collections.defaultdict(list)
for lid, fn, tn, co, ln in cur.fetchall():
    L[int(lid)] = dict(f=fn, t=tn, pts=[tuple(p[:2]) for p in ps.flat_coords(co)], len=float(ln)); out_of[fn].append(int(lid))
def xy(lon, lat): return ((lon-lon0)*111320*math.cos(math.radians(lat0)), (lat-lat0)*110574)
def geolen(l):
    p = [xy(*q) for q in L[l]['pts']]; return sum(math.dist(p[i], p[i+1]) for i in range(len(p)-1))
def proj(l, lon, lat):  # 링크 시작점부터 투영점까지 거리(형상 기준, 등록 길이로 비례 환산)
    p = [xy(*q) for q in L[l]['pts']]; P = xy(lon, lat); best = (1e18, 0); acc = 0
    for i in range(len(p)-1):
        (ax, ay), (bx, by) = p[i], p[i+1]; d = math.dist(p[i], p[i+1])
        t = 0 if d == 0 else max(0, min(1, ((P[0]-ax)*(bx-ax)+(P[1]-ay)*(by-ay))/d/d))
        q = (ax+t*(bx-ax), ay+t*(by-ay)); dd = math.dist(P, q)
        if dd < best[0]: best = (dd, acc+t*d)
        acc += d
    g = geolen(l); return best[1] * (L[l]['len']/g if g else 1)
def path(a, b):  # 링크 a 끝 노드 -> 링크 b 시작 노드 최단경로(중간 링크 목록)
    if L[a]['t'] == L[b]['f']: return []
    dist = {L[a]['t']: 0}; prev = {}; pq = [(0, L[a]['t'])]
    while pq:
        d, n = heapq.heappop(pq)
        if n == L[b]['f']: break
        if d > dist.get(n, 1e18) or d > 3000: continue
        for l in out_of[n]:
            nd = d + L[l]['len']
            if nd < dist.get(L[l]['t'], 1e18): dist[L[l]['t']] = nd; prev[L[l]['t']] = l; heapq.heappush(pq, (nd, L[l]['t']))
    if L[b]['f'] not in prev: return None
    out = []; n = L[b]['f']
    while n != L[a]['t']: l = prev[n]; out.append(l); n = L[l]['f']
    return out[::-1]
def leg(t1, t2):
    (_, _, l1, la1, lo1), (_, _, l2, la2, lo2) = t1, t2
    p1, p2 = proj(l1, lo1, la1), proj(l2, lo2, la2)
    if l1 == l2: return max(0.0, p2-p1), []
    mid = path(l1, l2)
    if mid is None: return None, None
    return (L[l1]['len']-p1) + sum(L[m]['len'] for m in mid) + p2, mid
sel = [t for t in ticks if s <= t[0] <= e]
dist = 0; rec = []; 
for a, b in zip(sel, sel[1:]):
    d, mid = leg(a, b)
    if d is None: print('경로 복구 실패', a[0], b[0]); sys.exit(1)
    dist += d; rec += mid
t0 = sel[0][1]; head = 0
if mode == 'link':
    head = proj(sel[0][2], sel[0][4], sel[0][3])
    prv = [t for t in ticks if t[0] < sel[0][0]]
    if prv:  # 직전 tick -> 첫 tick 구간에서 링크 시작 노드 통과 시각 보간
        dl, _ = leg(prv[-1], sel[0])
        if dl and dl > 0: t0 = sel[0][1] - datetime.timedelta(seconds=(sel[0][1]-prv[-1][1]).total_seconds()*min(1, head/dl))
dist += head
stay = (sel[-1][1]-t0).total_seconds()
links = []
for t in sel:
    if not links or links[-1] != t[2]: links.append(t[2])
tail = L[sel[-1][2]]["len"] - proj(sel[-1][2], sel[-1][4], sel[-1][3])
print(f"  [도착 tick 이후 링크 끝 노드까지 {tail:.1f}m, 첫 매칭 tick seq{sel[0][0]}]")
print(f"{trip} seq{s}~{e} [{mode}{' truth' if use_truth else ''}] 거리={dist:.1f}m (출발 보정 {head:.1f}m) 체류={stay:.1f}s "
      f"평균속도={dist/stay*3.6 if stay>0 else 0:.1f}km/h FROM={sel[0][2]} TO={sel[-1][2]} 복구링크={rec} 경유링크={links}")

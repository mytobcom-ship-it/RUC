# -*- coding: utf-8 -*-
"""주정차 폴리곤 주행 시나리오 생성·검증 (2026-10-04 최정우)

  목적
    RawLogWorker.cpp ProcessParkingCharge() 위 "[정책 대조표]" 의 목표 정책(B2~B9, C1~C5)을
    구현하기 전·후로 같은 입력을 돌려 과금 결과를 대조한다. 실주행 표본은 판교 RL-Z00001
    근처 8트립뿐이라(D5) 통과·정차·옆 구간단속 등 경우를 직접 만든다.

  시나리오 (모두 판교 RL-Z00001 주변 실제 도로망 ruc.road_link 위)
    S1 통과                 폴리곤을 서지 않고 관통
    S2 정차 후 출차         폴리곤 안 6분 정차 후 진출
    S3 옆 구간단속          RL-Z00003(폴리곤에서 5m)을 달린 뒤 폴리곤 밖 인접 도로로만 주행
    S4 구역 안 종료         폴리곤 안 6분 정차 후 그대로 트립 종료
    S5 전부 미매칭          S1 경로, 폴리곤 안 tick 을 raw_vld=false(ACCURACY_M=30) 로 SKIP 유도
    S6 구간단속 -> 폴리곤   RL-Z00003 출구 직후 폴리곤 관통
    S7 폴리곤 -> 구간단속   폴리곤 관통 후 구간단속 진입
    S8 구간단속 전후        구간단속 -> 폴리곤 -> 구간단속
    S9 출도착 안+미매칭     폴리곤 안에서 출발·정차·종료, 전 tick SKIP

  트립 ID 는 '98000N_...' (시뮬레이터 관례대로 9 로 시작 — check_accuracy.py 기준선에서 자동 제외)

  실행
    python3 park_scenario.py routes      경로만 출력(DB 쓰기 없음)
    python3 park_scenario.py load        기존 98% 트립 삭제 후 prim_rawgps·sim_truth 적재
    python3 park_scenario.py report      시나리오별 과금 행 + 기대 대비 점검
    python3 park_scenario.py clean       98% 트립 삭제(prim_rawgps·sim_truth·prim_chargehand)
"""
import os, sys, json, math, random, collections, datetime, configparser
import psycopg2

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))
CFG = os.path.join(ROOT, 'MapMatchSvr/bin/config.ini')
TRIP_PREFIX = '98'
PARK_ZONE = 'RL-Z00001'
TICK_SEC = 3
DRIVE_KMH = 30
STOP_SEC = 360                                  # base_parking_fine 5분 초과
SKIP_ACC = 30                                   # 미매칭 유도: raw_vld=false + 이 정확도. 50(park_accmax)
                                                #   이하라야 주정차 판정에는 남는다(60 이면 둘 다에서 빠져 C5 검증 불가)
SEED = 20261004


def connect():
    c = configparser.RawConfigParser(strict=False)
    c.read(CFG, encoding='utf-8-sig')
    d = c['database']
    return psycopg2.connect(host=d.get('host'), port=d.get('port', '5432'), dbname=d.get('name'),
                            user=d.get('userid'), password=d.get('password'))


# ── 도로망 ────────────────────────────────────────────────────────────────────
def flat_coords(v):
    if isinstance(v, list) and v and isinstance(v[0], (int, float)):
        return [v]
    out = []
    for x in v:
        out.extend(flat_coords(x))
    return out


def load_graph(cur):
    cur.execute("""SELECT coords FROM ruc.base_roadlink WHERE road_id=%s""", (PARK_ZONE,))
    poly = [tuple(p) for p in cur.fetchone()[0]]
    xs = [p[0] for p in poly]; ys = [p[1] for p in poly]
    pad = 0.012
    cur.execute("""SELECT link_id, f_node, t_node, coords, length_m FROM ruc.road_link
                   WHERE max_lon >= %s AND min_lon <= %s AND max_lat >= %s AND min_lat <= %s""",
                (min(xs) - pad, max(xs) + pad, min(ys) - pad, max(ys) + pad))
    links = {}
    out_of = collections.defaultdict(list)
    for lid, fn, tn, coords, ln in cur.fetchall():
        links[lid] = dict(f=fn, t=tn, pts=[tuple(p[:2]) for p in flat_coords(coords)], len=float(ln))
        out_of[fn].append(lid)
    cur.execute("""SELECT road_id, road_kind, link_ids FROM ruc.base_roadlink
                   WHERE use_yn='Y' AND road_kind IN ('1','2','3','5','0')""")
    kind = {}
    for rid, rk, lids in cur.fetchall():
        for l in (lids or []):
            kind[l] = (rid, rk)
    return poly, links, out_of, kind


def inside(pt, poly):
    x, y = pt; n = len(poly); c = False
    for i in range(n):
        x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def link_in_poly(l, poly):
    return any(inside(p, poly) for p in l['pts'])


def bfs(links, out_of, src, dst_set, avoid=frozenset(), max_hops=25):
    """src 링크 다음부터 dst_set 링크까지 최단(홉) 경로. 결과는 src 제외, dst 포함."""
    prev = {src: None}
    q = collections.deque([(src, 0)])
    while q:
        cur, h = q.popleft()
        if cur in dst_set and cur != src:
            path = []
            while cur != src:
                path.append(cur); cur = prev[cur]
            return path[::-1]
        if h >= max_hops:
            continue
        for nx in out_of.get(links[cur]['t'], []):
            if nx in prev or nx in avoid:
                continue
            prev[nx] = cur
            q.append((nx, h + 1))
    return None


# ── 시나리오 경로 ─────────────────────────────────────────────────────────────
SPEED_LINK = '2040424301'                       # RL-Z00003 구간단속 (TG00012 -> TG00013)
POLY_THRU = ['2040426101', '2040426103', '2040425302', '2040425301', '2040425303',
             '2040425102', '2040425403', '2040425401', '2040426201']
STOP_LINK = '2040425301'                        # 폴리곤 안 244.8m 링크 — 정차 지점


def build_routes(poly, links, out_of, kind):
    poly_links = {l for l, v in links.items() if link_in_poly(v, poly)}
    routes = {}
    routes['S1'] = list(POLY_THRU)
    routes['S2'] = list(POLY_THRU)
    routes['S5'] = list(POLY_THRU)
    # S3: 구간단속 -> 155804 에서 폴리곤 쪽(2040424302) 대신 2040425101 로 빠져 인접 도로만 주행
    up = bfs_rev(links, SPEED_LINK, 2)
    routes['S3'] = up + [SPEED_LINK, '2040425101', '2040425102', '2040425403', '2040425401', '2040426201']
    # S4: 폴리곤 진입 후 STOP_LINK 에서 종료
    routes['S4'] = POLY_THRU[:POLY_THRU.index(STOP_LINK) + 1]
    # S6: 구간단속 -> 폴리곤 관통 -> 밖
    routes['S6'] = up + [SPEED_LINK, '2040424302', '2040425801', '2040425802', '2040426201']
    # S7: 폴리곤 관통 -> 구간단속 진입 (폴리곤 재진입 금지)
    exit_link = '2040425303'
    tail = bfs(links, out_of, exit_link, {SPEED_LINK}, avoid=frozenset(poly_links))
    routes['S7'] = (POLY_THRU[:POLY_THRU.index(exit_link) + 1] + tail + ['2040425101', '2040425102']) if tail else None
    # S8: 구간단속 -> 폴리곤 -> 다시 구간단속
    if tail:
        routes['S8'] = up + [SPEED_LINK, '2040424302', '2040425801', '2040425301', exit_link] + tail \
            + ['2040425101', '2040425102']
    else:
        routes['S8'] = None
    # S9: 폴리곤 안 출발·종료
    routes['S9'] = [STOP_LINK]
    # S13: 구간단속 RL-Z00006 입구(2040424901) -> 출구(2040390202) -> 밖 2링크
    z_in, z_out = '2040424901', '2040390202'
    if z_in in links and z_out in links:
        mid = bfs(links, out_of, z_in, {z_out}, max_hops=12)
        after = []
        cur = z_out
        for _ in range(2):
            nx = [l for l in out_of.get(links[cur]['t'], []) if links[l]['t'] != links[cur]['f']]
            if not nx: break
            cur = max(nx, key=lambda l: links[l]['len']); after.append(cur)
        routes['S13'] = (bfs_rev(links, z_in, 2) + [z_in] + mid + after) if mid else None
    return routes, poly_links


def bfs_rev(links, dst, hops):
    """dst 로 들어오는 상류 링크를 hops 개 거슬러 올라간 경로(진입 게이트 앞 일반도로 확보용)."""
    into = collections.defaultdict(list)
    for l, v in links.items():
        into[v['t']].append(l)
    path = []; cur = dst
    for _ in range(hops):
        cands = [l for l in into.get(links[cur]['f'], []) if links[l]['f'] != links[cur]['t']]
        if not cands:
            break
        cur = max(cands, key=lambda l: links[l]['len'])
        path.append(cur)
    return path[::-1]


# ── 좌표 생성 ─────────────────────────────────────────────────────────────────
def hav(a, b):
    R = 6371008.8
    la1, la2 = math.radians(a[1]), math.radians(b[1])
    dla = la2 - la1; dlo = math.radians(b[0] - a[0])
    h = math.sin(dla / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin(dlo / 2) ** 2
    return 2 * R * math.asin(math.sqrt(h))


def bearing(a, b):
    la1, la2 = math.radians(a[1]), math.radians(b[1]); dlo = math.radians(b[0] - a[0])
    y = math.sin(dlo) * math.cos(la2)
    x = math.cos(la1) * math.sin(la2) - math.sin(la1) * math.cos(la2) * math.cos(dlo)
    return (math.degrees(math.atan2(y, x)) + 360) % 360


def offset(pt, dx, dy):
    lat = pt[1]
    return (pt[0] + dx / (111320 * math.cos(math.radians(lat))), pt[1] + dy / 110574)


def walk(links, route):
    """경로를 1m 간격 점열로 펼친다: [(lon,lat), link_id, heading]"""
    out = []
    for lid in route:
        pts = links[lid]['pts']
        for a, b in zip(pts, pts[1:]):
            d = hav(a, b); hd = bearing(a, b)
            n = max(1, int(d))
            for i in range(n):
                f = i / n
                out.append(((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f), lid, hd))
    last = links[route[-1]]['pts'][-1]
    out.append((last, route[-1], out[-1][2]))
    return out


def to_m(pt, ref):
    return ((pt[0] - ref[0]) * 111320 * math.cos(math.radians(ref[1])), (pt[1] - ref[1]) * 110574)


def from_m(xy, ref):
    return (ref[0] + xy[0] / (111320 * math.cos(math.radians(ref[1]))), ref[1] + xy[1] / 110574)


def nearest_boundary(pt, poly):
    """폴리곤 경계 위 최근접점과 거리(m)"""
    best = None
    for a, b in zip(poly, poly[1:] + poly[:1]):
        ax, ay = to_m(a, pt); bx, by = to_m(b, pt)
        dx, dy = bx - ax, by - ay
        L = dx * dx + dy * dy
        t = 0.0 if L == 0 else max(0.0, min(1.0, -(ax * dx + ay * dy) / L))
        qx, qy = ax + t * dx, ay + t * dy
        d = math.hypot(qx, qy)
        if best is None or d < best[1]:
            best = ((qx, qy), d)
    return best


def gen_trip(links, route, poly, rng, stop_at=None, end_stopped=False, skip_inside=False,
             skip_all=False, push_out=0.0, cut_exit=0, jump_m=0.0, kmh=None, gap_link=None, gap_sec=0,
             max_ticks=0):
    pts = walk(links, route)
    drive_kmh = kmh or DRIVE_KMH
    step = drive_kmh / 3.6 * TICK_SEC
    ticks = []                                       # (true_pt, link, heading, speed, status)
    pos = 0.0; stopped = False
    stop_idx = None
    if stop_at:
        idx = [i for i, p in enumerate(pts) if p[1] == stop_at]
        stop_idx = idx[len(idx) // 2]
    while True:
        i = min(int(pos), len(pts) - 1)
        p = pts[i]
        if stop_idx is not None and not stopped and i >= stop_idx:
            for _ in range(STOP_SEC // TICK_SEC):
                ticks.append((pts[stop_idx][0], pts[stop_idx][1], pts[stop_idx][2], 0, 2))
            stopped = True
            if end_stopped:
                break
        ticks.append((p[0], p[1], p[2], drive_kmh, 0))
        if i >= len(pts) - 1:
            break
        pos += step
    ticks = [t + (k,) for k, t in enumerate(ticks)]           # 마지막 원소 = 원래 tick 순번(시각 인덱스)
    if gap_link and gap_sec:
        # gap_link 에 처음 들어선 tick 부터 gap_sec 이상 지나고 gap_link 를 벗어날 때까지 tick 을 뺀다
        #   (터널·수신불량 모사 — 30초 넘으면 엔진이 매칭 앵커를 리셋해 경유 경로가 사라진다)
        first = next((k for k, t in enumerate(ticks) if t[1] == gap_link), None)
        if first is not None:
            last = first
            while last < len(ticks) and ((last - first) * TICK_SEC < gap_sec or ticks[last][1] == gap_link):
                last += 1
            ticks = ticks[:first] + ticks[last:]
    if max_ticks:
        ticks = ticks[:max_ticks]                     # 종료신호 없이 도중에 끊긴 트립 모사(TTL·서버종료 마감 검증)
    if cut_exit:
        # 마지막으로 폴리곤 안이었던 tick 뒤 cut_exit 개만 남기고 자른다(이탈 디바운스 중 종료 유도)
        last_in = max(k for k, t in enumerate(ticks) if inside(t[0], poly))
        ticks = ticks[:last_in + 1 + cut_exit]
    jump_k = None
    if jump_m and stop_idx is not None:
        # 정차 후 다시 움직인 첫 tick 의 다음 tick 을 진행방향 직각으로 jump_m 만큼 튀게 한다
        moving_after = [k for k in range(1, len(ticks)) if ticks[k - 1][3] == 0 and ticks[k][3] > 0]
        if moving_after:
            jump_k = moving_after[0] + 1
    rows = []
    for k, (tp, lid, hd, spd, st, ti) in enumerate(ticks):
        sig = 1.5 if spd == 0 else 4.0
        dx, dy = rng.gauss(0, sig), rng.gauss(0, sig)
        off = math.hypot(dx, dy)
        raw = offset(tp, dx, dy)
        acc = max(3, int(round(off * 1.5)) + 2)
        if push_out and spd > 0 and inside(tp, poly):
            # 경계 8m 이내 주행 tick 의 원시좌표만 경계 밖 push_out m 로 민다 — 규칙3(원시 밖·매칭 안)
            q, dist = nearest_boundary(tp, poly)
            if dist < 8.0:
                n = (q[0] / dist, q[1] / dist) if dist > 0 else (1.0, 0.0)
                raw = from_m((q[0] + n[0] * push_out, q[1] + n[1] * push_out), tp)
                off = round(dist + push_out, 2); acc = 4
        if jump_k is not None and k == jump_k:
            hr = math.radians(hd + 90)
            raw = offset(tp, jump_m * math.sin(hr), jump_m * math.cos(hr))
            off = jump_m; acc = 6
        vld = True
        if skip_all or (skip_inside and inside(tp, poly)):
            acc = SKIP_ACC; vld = False
        rows.append(dict(true=tp, link=lid, heading=int(hd), speed=spd, status=st, raw=raw,
                         off=round(off, 2), acc=acc, vld=vld, ti=ti))
    return rows


SCEN = [
    # id, 설명, 옵션
    ('S1', '통과', {}),
    ('S2', '정차 후 출차', dict(stop_at=STOP_LINK)),
    ('S3', '옆 구간단속(폴리곤 5m)', {}),
    ('S4', '구역 안 종료', dict(stop_at=STOP_LINK, end_stopped=True)),
    ('S5', '폴리곤 안 전부 미매칭', dict(skip_inside=True)),
    ('S6', '구간단속 -> 폴리곤', {}),
    ('S7', '폴리곤 -> 구간단속', {}),
    ('S8', '구간단속 -> 폴리곤 -> 구간단속', {}),
    ('S9', '출도착 안 + 전부 미매칭', dict(stop_at=STOP_LINK, end_stopped=True, skip_all=True)),
    # 2026-10-04 추가 — B9·B10·C1 검증
    ('S10', '경계 원시만 밖(규칙3, B9)', dict(stop_at=STOP_LINK, push_out=6.0)),
    ('S11', '정차 후 2tick 이탈·종료신호 없음(B10)', dict(stop_at=STOP_LINK, cut_exit=2, _noend=True)),
    ('S11b', 'S11 단말의 다음 트립(트립 전환 유발)', dict(_dev='S11', _route='S3', _delay_min=20)),
    ('S12', '출차 직후 45m 점프(C1)', dict(stop_at=STOP_LINK, jump_m=45.0)),
    # 2026-10-04 추가 — 폐쇄식·구간단속 출구 게이트 복구 탐색(FindExitGateOnRecoveredPath) 검증
    ('S13', '구간단속 출구 앞뒤 GPS 공백 36초(④)', dict(kmh=60, gap_link='2040390202', gap_sec=36)),
    # 2026-10-04 추가 — TTL·서버종료 강제마감 검증(같은 마감 경로 ExpireTtlSessions -> [trip_abend])
    ('S15', '구간단속 통과 후 종료신호 없이 끊김(TTL 마감)', dict(_route='S3', max_ticks=25, _noend=True)),
]

# 목표 정책(현재 고정 동작, 2026-10-04) 기대 결과 — 정책 대조표 B·C 절. report 가 실제와 나란히 보여 준다.
EXPECT = {
    'S1': '일반도로 1행(폴리곤 포함 연속, Y/0) / 주정차 없음',
    'S2': '주정차 1행(Y/0, 체류≈360초+안쪽 주행) + 일반도로 1행(정차 포함, 분할 없음 — C3 변경)',
    'S3': '구간단속 SPEED 1행(제한20<30, 위반) + 미러·일반도로 연속 / 주정차 없음',
    'S4': '주정차 1행(도착까지) + 일반도로 1행(진입 경계까지) — 구역 안 구간은 일반도로 미포함',
    'S5': '일반도로 1행(SKIP 구간 3단계 복구, Y/0) / 주정차 행 없음(체류<5분)',
    'S6': 'SPEED 1행 + 미러와 폴리곤 안 일반도로가 하나로 연속(B8)',
    'S7': '폴리곤 안 일반도로 + 구간단속 미러가 하나로 연속(B8) + SPEED 1행',
    'S8': '미러(전)+폴리곤 안+미러(후) 연속(B8) + SPEED 2행',
    'S9': '주정차 1행만(체류≈360초) / 일반도로 없음(C5)',
    'S10': '주정차 체류가 S2 보다 짧거나 같음(경계라인 보간, B9) — 원시 경계 tick 은 규칙3',
    'S11': '주정차 N/3(62) 체류 종료 = 구역 안 마지막 tick(B10) — 모드0 은 이탈 tick 까지 / 일반도로는 경계까지',
    'S11b': '정상 트립(S3 경로) — S11 마감 유발용',
    'S12': '점프 tick SKIP 또는 그 tick 이 일반도로를 왜곡하지 않음(C1 필요성 판단)',
    'S15': '끝난 구간(일반도로 1~6·구간단속 6~19)은 Y/0 유지, 열린 구간만 N/3(61), 종료시각은 GPS 기준이어야 함',
    'S13': 'RL-Z00006(제한50) 60km/h 위반 — 출구 TG00011 을 복구 경로로 찾아 SPEED Y/0 1행 + 미러(④ 없으면 출구 미확인)',
}


def trip_meta(n, start):
    ts = start.strftime('%Y%m%d%H%M%S')
    return '%s%04d_%s' % (TRIP_PREFIX, n, ts), 'SIM%s%04d' % (TRIP_PREFIX, n)


def cmd_routes(cur):
    poly, links, out_of, kind = load_graph(cur)
    routes, poly_links = build_routes(poly, links, out_of, kind)
    for sid, desc, _ in SCEN:
        r = routes.get(sid)
        if not r:
            print('%s %-22s 경로 없음' % (sid, desc)); continue
        tag = ['%s%s%s' % (l, '*' if l in poly_links else '', ('[' + kind[l][0] + '/' + kind[l][1] + ']') if l in kind else '')
               for l in r]
        print('%s %-22s %5.0fm  %s' % (sid, desc, sum(links[l]['len'] for l in r), ' > '.join(tag)))
    print('\n* = 폴리곤에 걸친 링크, [구역/유형]')
    return routes


def cmd_clean(cur):
    for t in ('ruc.prim_chargehand', 'ruc.sim_truth', 'ruc.prim_rawgps'):
        cur.execute("DELETE FROM %s WHERE trip_id LIKE %%s" % t, (TRIP_PREFIX + '%',))
        print('%-22s %d행 삭제' % (t, cur.rowcount))


def cmd_load(cur):
    poly, links, out_of, kind = load_graph(cur)
    routes, _ = build_routes(poly, links, out_of, kind)
    cmd_clean(cur)
    rng = random.Random(SEED)
    base = datetime.datetime(2026, 10, 4, 10, 0, 0)
    meta = {}
    devs = {}; ends = {}
    for n, (sid, desc, opt) in enumerate(SCEN, 1):
        opt = dict(opt)
        noend = opt.pop('_noend', False)
        dev_of = opt.pop('_dev', None)
        route_of = opt.pop('_route', None)
        delay = opt.pop('_delay_min', None)
        r = routes.get(route_of or sid) or (POLY_THRU if sid in ('S10', 'S11', 'S12') else None)
        if not r:
            print('%s 경로 없음 — 건너뜀' % sid); continue
        start = base + datetime.timedelta(hours=n)
        if dev_of and dev_of in ends:
            start = ends[dev_of] + datetime.timedelta(minutes=delay or 10)
        trip, dev = trip_meta(n, start)
        if dev_of:
            dev = devs[dev_of]
        devs[sid] = dev
        rows = gen_trip(links, r, poly, rng, **opt)
        ends[sid] = start + datetime.timedelta(seconds=rows[-1]['ti'] * TICK_SEC)
        for k, x in enumerate(rows, 1):
            # 시각은 원래 tick 순번 기준 — GPS 공백(gap_sec)으로 뺀 tick 만큼 실제로 시간이 빈다
            dt = (start + datetime.timedelta(seconds=x['ti'] * TICK_SEC)).strftime('%Y%m%d%H%M%S')
            ev = 0 if k == 1 else (2 if (k == len(rows) and not noend) else 1)
            cur.execute("""INSERT INTO ruc.prim_rawgps (gps_seq, device_key, trip_event, drive_status,
                             gps_lat, gps_lon, speed_kmh, heading, altitude_m, accuracy_m, gps_dt, recv_dt,
                             match_status, trip_id, raw_vld)
                           VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,0,%s,%s)""",
                        (k, dev, ev, x['status'], round(x['raw'][1], 7), round(x['raw'][0], 7),
                         x['speed'], x['heading'], 30, x['acc'], dt, dt, trip, x['vld']))
            cur.execute("""INSERT INTO ruc.sim_truth (trip_id, gps_seq, device_key, true_link_id, true_lat,
                             true_lon, offset_m, accuracy_m, raw_vld, drive_status, speed_kmh, gps_dt)
                           VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
                        (trip, k, dev, x['link'], round(x['true'][1], 8), round(x['true'][0], 8), x['off'],
                         x['acc'], x['vld'], x['status'], x['speed'], dt))
        ins = [k for k, x in enumerate(rows, 1) if inside(x['true'], poly)]
        meta[sid] = dict(trip=trip, desc=desc, ticks=len(rows), route=r,
                         route_m=round(sum(links[l]['len'] for l in r), 1),
                         in_poly_seq=[ins[0], ins[-1]] if ins else None)
        print('%s %-22s %s  %3d tick  폴리곤 안 seq %s' % (sid, desc, trip, len(rows), meta[sid]['in_poly_seq']))
    with open(os.path.join(HERE, 'park_scenario.json'), 'w', encoding='utf-8') as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)


TYPE = {'0': '일반', '1': '개방', '2': '폐쇄', '3': '구간', '4': '주정차', '5': '면제'}


def cmd_report(cur):
    meta = json.load(open(os.path.join(HERE, 'park_scenario.json'), encoding='utf-8'))
    for sid, desc, _ in SCEN:
        if sid not in meta:
            continue
        m = meta[sid]
        cur.execute("""SELECT match_status, count(*) FROM ruc.prim_rawgps WHERE trip_id=%s GROUP BY 1""", (m['trip'],))
        st = dict(cur.fetchall())
        cur.execute("""SELECT trip_seq, charge_type::text, charge_yn||'/'||charge_status, start_gps_seq, end_gps_seq,
                              dist_m, stay_seconds, from_id, to_id, coalesce(non_charge_reason::text,'')
                       FROM ruc.prim_chargehand WHERE trip_id=%s ORDER BY trip_seq::int""", (m['trip'],))
        rows = cur.fetchall()
        gen = sum(float(r[5] or 0) for r in rows if r[1] == '0')
        print('━' * 100)
        print('%s %s  (%s, %d tick, 경로 %.0fm, 폴리곤 안 seq %s)  매칭 %s' %
              (sid, desc, m['trip'], m['ticks'], m['route_m'], m['in_poly_seq'],
               ' '.join('%s:%d' % ({1: 'M', 3: 'S', 4: 'E', 0: 'P', 2: 'R'}[k], v) for k, v in sorted(st.items()))))
        print('  기대(목표): %s' % EXPECT[sid])
        for r in rows:
            print('  #%-2s %-4s %s  seq %3s~%-3s  %6sm  %5ss  %s -> %s  %s' %
                  (r[0], TYPE.get(r[1], r[1]), r[2], r[3], r[4], r[5], r[6] or '', r[7], r[8], r[9]))
        print('  일반도로 거리 합 %.0fm / 경로 %.0fm (%.0f%%)' % (gen, m['route_m'], 100 * gen / m['route_m'] if m['route_m'] else 0))


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('routes', 'load', 'report', 'clean'):
        print(__doc__); return
    conn = connect(); cur = conn.cursor()
    {'routes': cmd_routes, 'load': cmd_load, 'report': cmd_report, 'clean': cmd_clean}[sys.argv[1]](cur)
    conn.commit()


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RUC 과금 결과 전수 검증 (2026-09-23 최정우 신규)

왜 만들었나
  엔진을 고칠 때마다 이월·병합·커버리지 복구·유예가 서로 얽혀 파급이 생기는데, 그 파급을
  매번 애드혹 SQL 로 손수 확인해 왔다. 그러다 보니 (a) 이미 고친 것이 다시 깨져도 모르고
  (b) 결함은 사람이 특정 트립을 들여다보다 우연히 찾는 방식이었다. 실제로 2026-09-23 에
  사용자가 손으로 찾아낸 2건과 **같은 유형**의 위반이 전수 스캔에서 49건 나왔다.
  여기서는 그런 규칙을 SQL 불변식으로 고정해 두고, 수정할 때마다 1회 실행해
  "위반이 줄었는지 / 안 늘었는지"로 검증한다. 새 결함 유형을 만나면 RULES 에 1개 추가하면
  그때부터 영구히 감시된다.

사용법
  python3 verify_charge.py                  검사 실행 + 기준선 대비 비교
  python3 verify_charge.py --save           현재 상태를 기준선으로 저장(수정 착수 전에 실행)
  python3 verify_charge.py --detail R_ID    특정 검사의 위반 행 전체 출력
  python3 verify_charge.py --limit 20       검사별 샘플 출력 줄수(기본 5)

  DB 접속정보는 MapMatchSvr/bin/config.ini [database] 를 읽는다.
  기준선은 이 스크립트 옆 baseline.json 에 저장한다.

읽기 전용
  SELECT 만 수행한다. 어떤 테이블도 쓰지 않는다.
"""

import os
import sys
import json
import hashlib
import argparse
import configparser

try:
	import psycopg2
except ImportError:
	sys.stderr.write(
		"psycopg2 없음 — web/.venv312/bin/python 으로 실행할 것:\n"
		"  /home/mytobcom/ruc/web/.venv312/bin/python "
		"/home/mytobcom/ruc/MapMatchSvr/tools/verify_charge.py\n")
	sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_INI = os.path.join(HERE, '..', 'bin', 'config.ini')
BASELINE = os.path.join(HERE, 'baseline.json')

# ── 링크 방위각 산출 (road_link.coords 는 [[[lon,lat],...]] 3중 중첩) ────────────────
#   동서 방향은 위도에 따라 실거리가 줄어들므로 cos(lat) 보정을 넣는다.
SQL_LINK_BEARING = """
	SELECT link_id, (degrees(atan2(
			((coords->0->-1->>0)::float - (coords->0->0->>0)::float)
				* cos(radians((coords->0->0->>1)::float)),
			((coords->0->-1->>1)::float - (coords->0->0->>1)::float)))
		+ 360)::numeric % 360 AS brg
	FROM ruc.road_link
"""

# ── 불변식 규칙 ────────────────────────────────────────────────────────────────────
# id       : 짧은 식별자(기준선 키)
# name     : 사람이 읽는 이름
# severity : ERROR=물리적으로 불가능하거나 명백한 오적재 / WARN=정책상 허용될 수도 있는 후보
# why      : 이 검사가 왜 있는지(발견 경위) — 나중에 "이건 왜 보나" 를 되묻지 않도록
# sql      : 위반 행을 돌려주는 SELECT. 첫 컬럼부터 순서대로 출력한다
RULES = [
	# ── 구조 무결성 ────────────────────────────────────────────────────────────
	dict(id='P7', name='구역 거리가 등록 총연장을 초과', severity='ERROR',
		why='구역 행의 dist_m 은 그 구역 링크 길이 합을 넘을 수 없다(경계보정 꼬리 감안 10%% 여유). '
		    '실측 RL-Z00016 은 125m 구역인데 433m 로 기록됐다 — 구역 **밖** 링크 2040424301(332m)이 '
		    'qwLastLinkID 로 들어가 그 종료 노드까지 더해진 결과였다. '
		    '개방형(1)은 구역 등록길이 고정값을 쓰므로 제외',
		sql="""
		WITH zl AS (
		  SELECT b.road_id, sum(l.length_m) AS 총연장
		    FROM ruc.base_roadlink b,
		         LATERAL jsonb_array_elements_text(b.link_ids) AS e(lid)
		    JOIN ruc.road_link l ON l.link_id = e.lid
		   GROUP BY b.road_id)
		SELECT c.trip_id, c.trip_seq, c.charge_type, c.zone_id,
		       c.start_gps_seq||'~'||c.end_gps_seq AS 구간,
		       c.dist_m, round(zl.총연장)::int AS 구역총연장,
		       round(c.dist_m - zl.총연장)::int AS 초과
		  FROM ruc.prim_chargehand c
		  JOIN zl ON zl.road_id = c.zone_id
		 WHERE c.charge_type IN (2,3,5) AND c.dist_m > zl.총연장 * 1.1
		 ORDER BY (c.dist_m - zl.총연장) DESC"""),

	dict(id='K1', name='주정차 행이 폴리곤 밖', severity='ERROR',
		why='주정차(4)는 폴리곤 기반이라 행 구간의 tick 대부분이 폴리곤 안이어야 한다. '
		    '판정은 원시좌표·매칭좌표 2중(규칙2·규칙4)이므로 **원시 기준**으로 본다. '
		    '2026-09-23 실측에서는 5건 전부 98~100%% 포함이었다',
		sql="""
		WITH poly AS (
		  SELECT b.road_id,
		         polygon('(' || string_agg('(' || (p->>0) || ',' || (p->>1) || ')', ',' ORDER BY ord) || ')') AS pg
		    FROM ruc.base_roadlink b,
		         LATERAL jsonb_array_elements(b.coords) WITH ORDINALITY AS t(p, ord)
		   WHERE b.road_kind='4' GROUP BY b.road_id)
		SELECT c.trip_id, c.trip_seq, c.zone_id,
		       c.start_gps_seq||'~'||c.end_gps_seq AS 구간,
		       count(r.*) AS tick수,
		       count(*) FILTER (WHERE point(r.gps_lon::float, r.gps_lat::float) <@ p.pg) AS 폴리곤안
		  FROM ruc.prim_chargehand c
		  JOIN poly p ON p.road_id = c.zone_id
		  JOIN ruc.prim_rawgps r ON r.trip_id=c.trip_id AND r.match_status=1
		   AND r.gps_seq BETWEEN c.start_gps_seq AND c.end_gps_seq
		 WHERE c.charge_type=4
		 GROUP BY 1,2,3,4
		HAVING count(*) FILTER (WHERE point(r.gps_lon::float, r.gps_lat::float) <@ p.pg) < count(r.*) * 0.8
		 ORDER BY 1"""),

	dict(id='K2', name='과태료 기준 충족 주정차 누락', severity='ERROR',
		why='폴리곤 안에서 저속(<5km/h)으로 base_parking_fine 최소 from_min(=5분/300초) 이상 '
		    '머물렀는데 주정차 행이 없으면 누락이다. 그 미만은 BuildParkRow 가 false 를 돌려 '
		    '**의도적으로 적재하지 않는다**(로그 registered=0). 2026-09-23 실측 0건',
		sql="""
		WITH poly AS (
		  SELECT b.road_id,
		         polygon('(' || string_agg('(' || (p->>0) || ',' || (p->>1) || ')', ',' ORDER BY ord) || ')') AS pg
		    FROM ruc.base_roadlink b,
		         LATERAL jsonb_array_elements(b.coords) WITH ORDINALITY AS t(p, ord)
		   WHERE b.road_kind='4' GROUP BY b.road_id),
		 pin AS (
		  SELECT r.trip_id, r.gps_seq, r.gps_dt, p.road_id,
		         r.gps_seq - row_number() OVER (PARTITION BY r.trip_id, p.road_id ORDER BY r.gps_seq) AS grp
		    FROM ruc.prim_rawgps r
		    JOIN poly p ON point(r.gps_lon::float, r.gps_lat::float) <@ p.pg
		   WHERE r.match_status=1 AND r.speed_kmh < 5),
		 seg AS (
		  SELECT trip_id, road_id, min(gps_seq) s, max(gps_seq) e, count(*) n,
		         EXTRACT(EPOCH FROM (to_timestamp(max(gps_dt),'YYYYMMDDHH24MISS')
		                           - to_timestamp(min(gps_dt),'YYYYMMDDHH24MISS')))::int AS dur
		    FROM pin GROUP BY 1,2,grp)
		SELECT trip_id, road_id, s||'~'||e AS 구간, n AS tick수, dur AS 체류초
		  FROM seg
		 WHERE dur >= (SELECT min(from_min)*60 FROM ruc.base_parking_fine)
		   AND NOT EXISTS (SELECT 1 FROM ruc.prim_chargehand c
		        WHERE c.trip_id=seg.trip_id AND c.charge_type=4
		          AND c.start_gps_seq <= seg.e AND seg.s <= c.end_gps_seq)
		 ORDER BY dur DESC"""),

	dict(id='N1', name='행 구간 내부에 타 유형 구역 링크 혼입', severity='WARN',
		why='Z1 은 행의 **경계(from/to)만** 본다. 행 **구간 내부**의 tick 이 그 행과 다른 유형 '
		    '구역에 매칭된 경우는 잡지 못한다 — 실측 000995_20260904162440 trip_seq2(일반도로 '
		    '32~523)가 seq521~523 의 폐쇄형 RL-Z00008 링크를 품고 있었다. '
		    '원인은 IsLinkNodeStepEligible() 의 ③ 분기 — 게이트형 링크라도 그 유형 run 이 '
		    '열려 있지 않으면 true 를 돌려(게이트 미충족 구간은 일반도로 Y/0, 2026-09-07 원칙) '
		    '일반도로 run 이 유형 전환점에서 끊기지 않는다. '
		    '**대부분 정책 해당이라 WARN 이다** — 구간단속 미러·면제 거리제외·게이트 미충족 흡수. '
		    '[2026-09-23] 폐쇄형(2)은 "게이트 미충족 흡수" 가 아니라 **중간 진입 N/3 코드21 행**으로 '
		    '바뀌었다. 그래서 "그구역 행 없음" 으로 뜨는 폐쇄형 건은 이제 결함 후보다 — 개방형·'
		    '구간단속만 종전대로 정책 해당이다. '
		    '1 tick 짜리는 GPS 간격에 따른 경계 걸침이고, 4 tick 이상이 구간 흡수다',
		sql="""
		WITH mix AS (
		  SELECT c.trip_id, c.trip_seq, c.charge_type, COALESCE(c.zone_id,'') AS 행구역,
		         c.start_gps_seq, c.end_gps_seq, c.dist_m,
		         b.road_id AS 혼입구역, b.road_kind AS 혼입유형, count(*) AS tick수
		    FROM ruc.prim_chargehand c
		    JOIN ruc.prim_rawgps r ON r.trip_id=c.trip_id AND r.match_status=1
		     AND r.gps_seq BETWEEN c.start_gps_seq AND c.end_gps_seq
		    JOIN ruc.base_roadlink b ON b.use_yn='Y' AND b.link_ids @> to_jsonb(r.match_link_id)
		   WHERE b.road_id IS DISTINCT FROM COALESCE(c.zone_id,'')
		     AND b.road_kind <> '0'
		   GROUP BY 1,2,3,4,5,6,7,8,9)
		SELECT trip_id, trip_seq, charge_type AS 행유형,
		       start_gps_seq||'~'||end_gps_seq AS 구간, dist_m,
		       혼입구역||'(k'||혼입유형||')' AS 혼입, tick수,
		       CASE WHEN EXISTS (SELECT 1 FROM ruc.prim_chargehand o
		              WHERE o.trip_id=mix.trip_id AND o.zone_id=mix.혼입구역)
		            THEN '그구역 행 있음' ELSE '그구역 행 없음(게이트 미충족 등)' END AS 상대행
		  FROM mix
		 ORDER BY tick수 DESC, trip_id"""),

	dict(id='S1', name='trip_seq 결번/불연속', severity='ERROR',
		why='정산서버가 PK 로 쓰므로 1..N 연속이어야 한다(2026-09-22 워터마크 큐의 목표)',
		sql="""
		SELECT trip_id, count(*) AS 행수, min(trip_seq) AS 최소, max(trip_seq) AS 최대
		  FROM ruc.prim_chargehand GROUP BY trip_id
		 HAVING min(trip_seq)<>1 OR max(trip_seq)<>count(*)
		 ORDER BY trip_id"""),

	dict(id='S2', name='trip_seq 순 != gps_seq 순', severity='ERROR',
		why='trip_seq 는 주행 순서여야 한다. 워터마크 큐 도입의 확정 요구 2번',
		sql="""
		WITH x AS (SELECT trip_id, trip_seq, start_gps_seq,
			lag(start_gps_seq) OVER (PARTITION BY trip_id ORDER BY trip_seq) AS 직전
			FROM ruc.prim_chargehand)
		SELECT trip_id, trip_seq, start_gps_seq, 직전 FROM x
		 WHERE 직전 IS NOT NULL AND start_gps_seq < 직전 ORDER BY trip_id, trip_seq"""),

	dict(id='S3', name='재부여 잔재(trip_seq>100000)', severity='ERROR',
		why='[trip_seqoff] 가 +100000 오프셋 상태로 굳으면 영구 손상(2026-09-10 실측 버그)',
		sql="""
		SELECT trip_id, trip_seq, charge_type FROM ruc.prim_chargehand
		 WHERE trip_seq > 100000 ORDER BY trip_id, trip_seq"""),

	dict(id='S4', name='gps_seq 범위 역전', severity='ERROR',
		why='start > end 는 구간 정의 자체가 깨진 것',
		sql="""
		SELECT trip_id, trip_seq, charge_type, start_gps_seq, end_gps_seq
		  FROM ruc.prim_chargehand WHERE start_gps_seq > end_gps_seq ORDER BY trip_id, trip_seq"""),

	dict(id='S5', name='non_charge_reason NULL', severity='ERROR',
		why='2026-09-10 코드체계 도입 시 전 행 채우기로 정리했다. NULL 은 미배선 경로의 흔적',
		sql="""
		SELECT trip_id, trip_seq, charge_type, charge_yn, charge_status
		  FROM ruc.prim_chargehand WHERE non_charge_reason IS NULL ORDER BY trip_id, trip_seq"""),

	dict(id='S6', name='charge_yn/status 조합 이상', severity='ERROR',
		why='확정 규칙은 Y→0, N→3(감사) 또는 4(면제/스킵) 뿐이다',
		sql="""
		SELECT trip_id, trip_seq, charge_type, charge_yn, charge_status, non_charge_reason
		  FROM ruc.prim_chargehand
		 WHERE NOT ((charge_yn='Y' AND charge_status=0)
		         OR (charge_yn='N' AND charge_status IN (3,4)))
		 ORDER BY trip_id, trip_seq"""),

	dict(id='S7', name='정상과금(Y/0)인데 사유코드 != 0', severity='ERROR',
		why='Y/0 은 정상 과금이므로 비과금 사유가 붙으면 안 된다',
		sql="""
		SELECT trip_id, trip_seq, charge_type, non_charge_reason FROM ruc.prim_chargehand
		 WHERE charge_yn='Y' AND charge_status=0 AND COALESCE(non_charge_reason,0)<>0
		 ORDER BY trip_id, trip_seq"""),

	# ── 물리 정합성 ────────────────────────────────────────────────────────────
	dict(id='P1', name='구간 전체가 SKIP 인 행', severity='ERROR',
		why='매칭에 실패해 어디를 달렸는지 모르는 구간을 과금한 것. '
		    '2026-09-23 실측 000998 trip_seq1 — 반대편 링크 보정으로 폐기된 링크가 '
		    '경로 누적에 남아 89m 허위 행이 생겼다',
		sql="""
		SELECT c.trip_id, c.trip_seq, c.charge_type, c.from_id,
		       c.start_gps_seq, c.end_gps_seq, c.dist_m
		  FROM ruc.prim_chargehand c
		 WHERE NOT EXISTS (SELECT 1 FROM ruc.prim_rawgps r
		        WHERE r.trip_id=c.trip_id
		          AND r.gps_seq BETWEEN c.start_gps_seq AND c.end_gps_seq
		          AND r.match_status=1)
		 ORDER BY c.dist_m DESC"""),

	dict(id='P2', name='역방향 링크 과금(150도 이상)', severity='ERROR',
		why='왕복분리 도로에서 반대편 링크를 잡으면 방위각이 170도 안팎으로 벌어진다. '
		    '2026-09-22 에 구역 진입 판정에는 150도 가드를 넣었지만 행 생성 경로에는 없다. '
		    '실측 000993 trip_seq20 은 3,003m 짜리가 178도 반대. '
		    '※ 정차(speed_kmh=0) tick 은 heading 이 갱신되지 않아 제외한다 — 넣으면 '
		    '000376_20260826143909 처럼 heading=0 인 정차 tick 이 오탐된다',
		sql="""
		WITH lk AS (""" + SQL_LINK_BEARING + """)
		SELECT c.trip_id, c.trip_seq, c.charge_type, c.from_id,
		       c.start_gps_seq, c.end_gps_seq, c.dist_m,
		       round(lk.brg) AS 링크방위, r.heading AS 주행방위,
		       round(abs(((lk.brg - r.heading)::numeric + 540) % 360 - 180)) AS 차이
		  FROM ruc.prim_chargehand c
		  JOIN lk ON lk.link_id = c.from_id
		  JOIN ruc.prim_rawgps r ON r.trip_id=c.trip_id AND r.gps_seq=c.start_gps_seq
		 WHERE r.match_status=1
		   AND r.speed_kmh > 0							-- 정차 중 heading 은 갱신되지 않아 의미가 없다
		   AND abs(((lk.brg - r.heading)::numeric + 540) % 360 - 180) >= 150
		 ORDER BY c.dist_m DESC"""),

	dict(id='P3', name='평균속도 vs 거리/시간 불일치', severity='WARN',
		why='speed_kmh 는 dist_m/stay_seconds 에서 유도된 값이어야 한다. '
		    '어긋나면 거리나 시각 중 하나가 사후에 덧칠된 것',
		sql="""
		SELECT trip_id, trip_seq, charge_type, dist_m, stay_seconds, speed_kmh,
		       round((dist_m::numeric / stay_seconds) * 3.6) AS 계산속도
		  FROM ruc.prim_chargehand
		 WHERE stay_seconds > 0 AND dist_m > 0
		   AND abs(speed_kmh - (dist_m::numeric / stay_seconds) * 3.6) > 2
		 ORDER BY abs(speed_kmh - (dist_m::numeric / stay_seconds) * 3.6) DESC"""),

	dict(id='P4', name='거리>0 인데 체류시간 0', severity='WARN',
		why='시간 0 에 거리가 있으면 평균속도가 무한대가 된다(과거 speed_kmh smallint 오버플로 사고)',
		sql="""
		SELECT trip_id, trip_seq, charge_type, dist_m, stay_seconds, speed_kmh
		  FROM ruc.prim_chargehand WHERE dist_m > 0 AND stay_seconds = 0
		 ORDER BY dist_m DESC"""),

	dict(id='P6', name='기록 평균속도가 tick 실측을 크게 초과', severity='ERROR',
		why='P3(speed_kmh 가 dist/stay 와 맞는가)는 **둘이 함께 틀린 경우를 못 잡는다** — '
		    '거리가 과대하면 speed_kmh 도 같이 과대해져 정합성 검사를 통과한다. '
		    '구간 안 tick 의 실측 속도와 대조해야 드러난다. 실측 369km/h(102m/1초, tick 실측 16km/h)·'
		    '319km/h(89m/1초, 실측 0km/h — P1 의 역방향 허위 행) 같은 값이 이 검사로 드러났다. '
		    '1.5배를 넘으면 거리나 시간 중 하나가 틀린 것이다',
		sql="""
		WITH m AS (
		  SELECT c.trip_id, c.trip_seq, c.charge_type, c.zone_id, c.dist_m, c.stay_seconds, c.speed_kmh,
		         c.start_gps_seq, c.end_gps_seq,
		         (SELECT max(r.speed_kmh) FROM ruc.prim_rawgps r
		           WHERE r.trip_id=c.trip_id
		             AND r.gps_seq BETWEEN c.start_gps_seq AND c.end_gps_seq) AS 실측최대
		    FROM ruc.prim_chargehand c WHERE c.speed_kmh > 0)
		SELECT trip_id, trip_seq, charge_type, COALESCE(zone_id,'') AS zone,
		       start_gps_seq||'~'||end_gps_seq AS 구간, dist_m, stay_seconds,
		       speed_kmh AS 기록, 실측최대
		  FROM m
		 WHERE 실측최대 IS NOT NULL AND speed_kmh > 실측최대 * 1.5
		 ORDER BY (speed_kmh - 실측최대) DESC"""),

	dict(id='P5', name='트립 과금거리가 실주행거리 대비 과다', severity='WARN',
		why='개방형 구역 고정길이·경계보정 때문에 어느 정도 초과는 정상이나, '
		    '20%%를 넘으면 이중계상 의심. 2026-09-23 이월 이중계상이 이 방식으로 드러났다. '
		    '[2026-10-04 보완, 사용자 확정] 과금 합계에서 주정차(4)·구간단속(3)을 뺀다 — 주정차는 일반도로와 '
		    '같은 구간을 함께 등록(B6·E16)하고 구간단속은 같은 구간을 일반도로 미러로도 등록하므로, 둘을 더하면 '
		    '정책상 허용된 겹침까지 과다로 잡힌다. 일반도로·개방·폐쇄·면제만 합산',
		sql="""
		WITH p AS (SELECT trip_id, gps_seq, match_lat::float la, match_lon::float lo
		             FROM ruc.prim_rawgps WHERE match_status=1),
		     d AS (SELECT trip_id, 6371000*2*asin(sqrt(
		             power(sin(radians(la - lag(la) OVER w)/2),2)
		             + cos(radians(lag(la) OVER w))*cos(radians(la))
		             * power(sin(radians(lo - lag(lo) OVER w)/2),2))) AS m
		             FROM p WINDOW w AS (PARTITION BY trip_id ORDER BY gps_seq)),
		     실주행 AS (SELECT trip_id, sum(m) rm FROM d GROUP BY 1),
		     과금 AS (SELECT trip_id, sum(dist_m) cd FROM ruc.prim_chargehand
		               WHERE charge_type NOT IN ('3','4') GROUP BY 1)
		SELECT 과금.trip_id, round(실주행.rm)::int AS 실주행_m, 과금.cd::int AS 과금_m,
		       round(과금.cd - 실주행.rm)::int AS 초과_m,
		       round((과금.cd/실주행.rm - 1) * 100)::int AS 초과율
		  FROM 과금 JOIN 실주행 USING(trip_id)
		 WHERE 실주행.rm > 100 AND 과금.cd > 실주행.rm * 1.2
		 ORDER BY (과금.cd - 실주행.rm) DESC"""),

	# ── 구역·유형 정합성 ───────────────────────────────────────────────────────
	dict(id='Z1', name='일반도로 경계가 겹치는 타 유형 구역 링크', severity='ERROR',
		why='일반도로 행의 from/to 가 타 유형 구역 링크이고, **같은 트립에 그 구역의 행이 있으며 '
		    '구간까지 겹치면** 두 행이 같은 tick 을 나눠 갖는다. 2026-09-23 실측 000998 trip_seq3 의 '
		    'to_id 가 면제구역 링크 2040005903 이고 면제 행(seq8~9)과 seq8 이 겹쳤다 — '
		    '진입 직전 링크 2040006402 여야 했다. '
		    '※ 다음 둘은 정책상 정상이라 제외한다: (1) road_kind=0 구역(RL-Z00002)은 일반도로 '
		    '등록이므로 경계로 쓰는 게 맞다. (2) 구간단속 비위반 미러 등 그 구역 행 자체가 없는 경우는 '
		    '겹칠 대상이 없다. (3) 일반도로가 상대 구간을 통째로 포함하는 미러·감싸기도 정책이다',
		sql="""
		WITH b AS (
		  SELECT c.trip_id, c.device_key, c.trip_seq, c.start_gps_seq, c.end_gps_seq, c.dist_m,
		         v.id AS 경계, v.구분
		    FROM ruc.prim_chargehand c,
		         LATERAL (VALUES (c.from_id,'FROM'), (c.to_id,'TO')) AS v(id, 구분)
		   WHERE c.charge_type=0 AND COALESCE(v.id,'')<>'')
		SELECT b.trip_id, b.trip_seq, b.start_gps_seq||'~'||b.end_gps_seq AS 일반도로구간, b.dist_m,
		       b.구분||' '||b.경계||'='||z.road_id||'(kind '||z.road_kind||')' AS 위반경계,
		       o.trip_seq AS 상대행, o.start_gps_seq||'~'||o.end_gps_seq AS 상대구간,
		       o.charge_type AS 상대유형
		  FROM b
		  JOIN ruc.base_roadlink z
		    ON z.use_yn='Y' AND z.road_kind<>'0' AND z.link_ids @> to_jsonb(b.경계)
		  JOIN ruc.prim_chargehand o
		    ON o.trip_id=b.trip_id AND o.device_key=b.device_key AND o.zone_id=z.road_id
		   AND o.start_gps_seq<=b.end_gps_seq AND b.start_gps_seq<=o.end_gps_seq
		   -- 일반도로가 상대 구간을 **포함**(미러·감싸기)하는 것은 정책상 정상이므로 제외한다
		   --   (2026-09-16 확인: 구간단속 위반 미러·면제 거리제외·게이트 미충족 흡수 각자 정책).
		   --   남기는 것은 **부분 겹침** — 경계 tick 만 서로 나눠 갖는 경우다
		   AND NOT (b.start_gps_seq <= o.start_gps_seq AND o.end_gps_seq <= b.end_gps_seq)
		 ORDER BY b.trip_id, b.trip_seq"""),

	dict(id='Z2', name='유형별 from_id/to_id 형식 위반', severity='ERROR',
		why='확정 규칙(2026-09-15 사실표): 0=링크ID, 1·4·5=구역 road_id, 2·3=게이트ID(TG…). '
		    '형식이 어긋나면 정산서버가 구간을 해석할 수 없다. '
		    '※ 처음엔 전 유형을 base_roadlink 로만 검사했다가 폐쇄형·구간단속 37건을 '
		    '오탐했다 — 게이트ID가 정상인 유형이다. '
		    '2차: 빈 값 4건도 오탐이었다 — 코드가 "출구 미확인 — 지어내지 않음"으로 '
		    '**의도적으로 비우고** N/3 + ncr 21(진입게이트 미확인)·23(출구게이트 미확인)·'
		    '61(강제마감)을 붙인다. 과금하는 행(Y)이 경계를 모르는 경우만 결함이다',
		sql="""
		WITH x AS (
		  SELECT trip_id, trip_seq, charge_type, charge_yn, id AS 값, 구분 FROM ruc.prim_chargehand,
		    LATERAL (VALUES (from_id,'from'), (to_id,'to')) AS v(id, 구분))
		SELECT x.trip_id, x.trip_seq, x.charge_type, x.구분, COALESCE(x.값,'(NULL)') AS 값,
		       CASE WHEN COALESCE(x.값,'')='' THEN '빈 값'
		            WHEN x.charge_type=0 THEN '링크ID 아님(또는 미등록 링크)'
		            WHEN x.charge_type IN (1,4,5) THEN '구역 road_id 아님'
		            ELSE '게이트ID 아님' END AS 사유
		  FROM x
		 WHERE (COALESCE(x.값,'')='' AND x.charge_yn='Y')	-- 비과금(N)은 경계를 비우는 게 정상
		    OR (x.charge_type=0 AND COALESCE(x.값,'')<>'' AND NOT EXISTS (
		          SELECT 1 FROM ruc.road_link l WHERE l.link_id=x.값))
		    OR (x.charge_type IN (1,4,5) AND COALESCE(x.값,'')<>'' AND NOT EXISTS (
		          SELECT 1 FROM ruc.base_roadlink b WHERE b.road_id=x.값))
		    OR (x.charge_type IN (2,3) AND COALESCE(x.값,'')<>'' AND NOT EXISTS (
		          SELECT 1 FROM ruc.base_tollgate g WHERE g.tollgate_id=x.값))
		 ORDER BY x.trip_id, x.trip_seq"""),

	dict(id='Z7', name='charge_unit 이 유형과 불일치', severity='ERROR',
		why='확정 규칙: 0·1 → 0(NODE), 2·3·5 → 1(LINK), 4 → 2(POLYGON)',
		sql="""
		SELECT trip_id, trip_seq, charge_type, charge_unit,
		       CASE WHEN charge_type IN (0,1) THEN 0
		            WHEN charge_type IN (2,3,5) THEN 1 ELSE 2 END AS 기대값
		  FROM ruc.prim_chargehand
		 WHERE charge_unit IS DISTINCT FROM
		       (CASE WHEN charge_type IN (0,1) THEN 0
		             WHEN charge_type IN (2,3,5) THEN 1 ELSE 2 END)
		 ORDER BY trip_id, trip_seq"""),

	dict(id='Z8', name='link_id 사용(전 유형 미사용이 규칙)', severity='WARN',
		why='구간은 from_id/to_id 로 표현하고 link_id 는 쓰지 않는다(2026-09-15 확인). '
		    '값이 들어가기 시작하면 규칙이 바뀐 것이므로 문서부터 확인할 것',
		sql="""
		SELECT trip_id, trip_seq, charge_type, link_id FROM ruc.prim_chargehand
		 WHERE COALESCE(link_id,'')<>'' ORDER BY trip_id, trip_seq"""),

	dict(id='Z3', name='같은 유형 구간중복', severity='ERROR',
		why='같은 유형이 같은 구간을 두 번 청구하면 이중과금이다',
		sql="""
		SELECT a.trip_id, a.charge_type,
		       a.trip_seq AS 앞, a.start_gps_seq||'~'||a.end_gps_seq AS 앞구간,
		       b.trip_seq AS 뒤, b.start_gps_seq||'~'||b.end_gps_seq AS 뒤구간
		  FROM ruc.prim_chargehand a JOIN ruc.prim_chargehand b
		    ON a.trip_id=b.trip_id AND a.device_key=b.device_key
		   AND a.charge_type=b.charge_type AND a.trip_seq<b.trip_seq
		   AND a.start_gps_seq<=b.end_gps_seq AND b.start_gps_seq<=a.end_gps_seq
		 ORDER BY a.trip_id, a.trip_seq"""),

	dict(id='Z4', name='유형 간 구간중복', severity='WARN',
		why='유형이 다르면 정책상 정당한 겹침이 있다(구간단속 미러·면제 거리제외 등, '
		    '2026-09-16 확인). 급증 여부만 추이로 본다 — 건수 자체로 결함이라 단정하지 말 것',
		sql="""
		SELECT a.trip_id, a.charge_type AS 앞유형, a.trip_seq AS 앞,
		       a.start_gps_seq||'~'||a.end_gps_seq AS 앞구간,
		       b.charge_type AS 뒤유형, b.trip_seq AS 뒤,
		       b.start_gps_seq||'~'||b.end_gps_seq AS 뒤구간
		  FROM ruc.prim_chargehand a JOIN ruc.prim_chargehand b
		    ON a.trip_id=b.trip_id AND a.device_key=b.device_key
		   AND a.charge_type<>b.charge_type AND a.trip_seq<b.trip_seq
		   AND a.start_gps_seq<=b.end_gps_seq AND b.start_gps_seq<=a.end_gps_seq
		 ORDER BY a.trip_id, a.trip_seq"""),

	dict(id='Z5', name='구역 행 구간에 그 구역 링크 매칭이 없음', severity='WARN',
		why='구역 행인데 그 구간의 어느 tick 도 해당 구역 링크에 매칭되지 않은 경우. '
		    '**ERROR 가 아니라 WARN 이다** — 엔진은 tick 이 안 찍힌 **경유 링크**(aqwPathLinkIDs)로도 '
		    '구역을 판정하므로(2026-08-20 도입, 짧은 구역이 두 GPS 사이에 통째로 끼는 경우 보완), '
		    '매칭 tick 이 없어도 실제로 지나간 것일 수 있다. 그 경로는 DB 에 없어 여기서는 못 본다. '
		    '비과금(N)은 제외한다 — RL-Z00004 8.8m 브리지 복구 행(ncr 12) 4건이 그렇게 오탐이었다. '
		    '주정차(4)는 폴리곤 기반이라 제외. '
		    '※ 처음엔 from_id 로 구역을 찾다가 폐쇄형·구간단속 42건을 오탐했다 — 그 유형의 '
		    'from_id 는 게이트ID 라 JOIN 이 성립하지 않는다. zone_id 를 쓸 것',
		sql="""
		SELECT c.trip_id, c.trip_seq, c.charge_type, c.zone_id, c.from_id,
		       c.start_gps_seq, c.end_gps_seq, c.dist_m
		  FROM ruc.prim_chargehand c
		 WHERE c.charge_type IN (1,2,3,5) AND COALESCE(c.zone_id,'')<>''
		   AND c.charge_yn='Y'
		   AND NOT EXISTS (
		     SELECT 1 FROM ruc.prim_rawgps r
		      JOIN ruc.base_roadlink b ON b.road_id = c.zone_id
		     WHERE r.trip_id=c.trip_id
		       AND r.gps_seq BETWEEN c.start_gps_seq AND c.end_gps_seq
		       AND r.match_link_id IS NOT NULL
		       AND b.link_ids @> to_jsonb(r.match_link_id))
		 ORDER BY c.dist_m DESC"""),

	dict(id='Z9', name='zone_id 채움 규칙 위반', severity='ERROR',
		why='일반도로(0)는 zone_id 를 항상 비우고(2026-09-01 확정), 유형 1~5 는 항상 채운다',
		sql="""
		SELECT trip_id, trip_seq, charge_type, COALESCE(zone_id,'(NULL)') AS zone_id,
		       CASE WHEN charge_type=0 THEN '일반도로인데 zone_id 있음'
		            ELSE '구역 유형인데 zone_id 없음' END AS 사유
		  FROM ruc.prim_chargehand
		 WHERE (charge_type=0 AND COALESCE(zone_id,'')<>'')
		    OR (charge_type BETWEEN 1 AND 5 AND COALESCE(zone_id,'')='')
		 ORDER BY trip_id, trip_seq"""),

	dict(id='Z6', name='링크 미덮임(엔진 로그 기준)', severity='WARN', log=True,
		why='주행한 미등록 링크 중 어떤 행에도 안 들어간 것 = 일반도로 청구 누락. '
		    '**DB 만으로는 셀 수 없다** — 행이 덮은 링크(vtCoveredLinks)는 DB 에 없고, '
		    'from_id/to_id 로 추정하면 경계보정값과 매칭값이 다른 기준이라 과대치가 나온다'
		    '(2026-09-22 에 78km 로 잘못 나온 전례). 그래서 엔진이 트립 마감 때 남기는 '
		    'trip link coverage 로그를 읽는다',
		sql=None),
]

# ── 전체 집계(회귀 비교용) ─────────────────────────────────────────────────────────
SQL_SUMMARY = """
	SELECT count(*) AS 행수, count(DISTINCT trip_id) AS 트립수, COALESCE(sum(dist_m),0) AS 총거리
	  FROM ruc.prim_chargehand
"""
SQL_BY_TYPE = """
	SELECT charge_type, count(*), COALESCE(sum(dist_m),0)
	  FROM ruc.prim_chargehand GROUP BY 1 ORDER BY 1
"""
SQL_BY_YN = """
	SELECT charge_yn||'/'||charge_status, count(*)
	  FROM ruc.prim_chargehand GROUP BY 1 ORDER BY 1
"""
# 정렬 md5 — trip_seq 는 비교키로 쓰지 않는다(동점 start_gps_seq 타이브레이크로 흔들린다)
SQL_MD5_ROWS = """
	SELECT trip_id, device_key, charge_type, COALESCE(from_id,''), COALESCE(to_id,''),
	       start_gps_seq, end_gps_seq, dist_m, stay_seconds,
	       charge_yn, charge_status, COALESCE(non_charge_reason,-1)
	  FROM ruc.prim_chargehand
"""


def db_connect():
	cp = configparser.ConfigParser(strict=False)
	cp.read(CONFIG_INI, encoding='utf-8-sig')	# config.ini 는 BOM 포함
	d = cp['database']
	cn = psycopg2.connect(host=d.get('host', '127.0.0.1'), port=d.get('port', '5432'),
		dbname=d.get('name', 'ruc'), user=d.get('userid'), password=d.get('password'))
	cn.set_session(readonly=True)		# 검증 도구는 절대 쓰지 않는다
	return cn


def fetch(cu, sql):
	cu.execute(sql)
	cols = [c[0] for c in cu.description]
	return cols, cu.fetchall()


LOG_DIR = os.path.join(HERE, '..', 'bin', 'log')


def scan_coverage_log():
	"""엔진의 trip link coverage 로그에서 미덮임 링크를 집계 (2026-09-23 최정우 추가)

	로그에는 재매칭을 반복한 만큼 같은 trip_id 가 여러 번 나온다 — **trip_id 별 마지막 줄**만
	현재 상태다. 로그 파일이 없으면 검사를 건너뛴다(건수 None).
	"""
	import glob
	import re
	files = sorted(glob.glob(os.path.join(LOG_DIR, '*.log')), key=os.path.getmtime)
	if not files:
		return None, []

	pat = re.compile(
		r'trip link coverage!trip_id=\[([^\]]*)\] path=\[(\d+)\] unreg=\[(\d+)\] '
		r'uncovered=\[(\d+)\] uncovered_m=\[([0-9.]+)\]')
	last = {}
	for fp in files[-3:]:					# 최근 3개 파일이면 한 번의 전체 재매칭은 충분히 덮는다
		try:
			with open(fp, encoding='utf-8', errors='replace') as f:
				for line in f:
					m = pat.search(line)
					if m:
						last[m.group(1)] = (int(m.group(2)), int(m.group(3)),
							int(m.group(4)), float(m.group(5)))
		except OSError:
			continue

	rows = [(tid, v[0], v[1], v[2], round(v[3]))
			for tid, v in last.items() if v[2] > 0]
	rows.sort(key=lambda r: -r[4])
	return len(rows), rows


def collect(cu):
	"""현재 DB 상태를 요약 + 검사별 위반으로 환산"""
	cols, rows = fetch(cu, SQL_SUMMARY)
	총행수, 트립수, 총거리 = rows[0]

	_, t = fetch(cu, SQL_BY_TYPE)
	_, y = fetch(cu, SQL_BY_YN)

	_, mrows = fetch(cu, SQL_MD5_ROWS)
	lines = sorted('|'.join(str(v) for v in r) for r in mrows)
	md5 = hashlib.md5('\n'.join(lines).encode('utf-8')).hexdigest()[:8]

	검사결과 = {}
	for rule in RULES:
		if rule.get('log'):
			n, rws = scan_coverage_log()
			검사결과[rule['id']] = dict(
				건수=(0 if n is None else n),
				컬럼=['trip_id', '경로링크', '미등록', '미덮임', '미덮임_m'],
				행=rws, 미측정=(n is None))
			continue
		c, rws = fetch(cu, rule['sql'])
		검사결과[rule['id']] = dict(건수=len(rws), 컬럼=c, 행=rws, 미측정=False)

	return dict(
		행수=총행수, 트립수=트립수, 총거리=int(총거리), md5=md5,
		유형별={str(a): [b, int(c)] for a, b, c in t},
		과금구분={a: b for a, b in y},
		검사={k: v['건수'] for k, v in 검사결과.items()},
		_상세=검사결과)


def fmt_delta(now, base):
	if base is None:
		return ''
	d = now - base
	if d == 0:
		return '  (기준선 동일)'
	return '  (기준선 %+d)' % d


def main():
	ap = argparse.ArgumentParser(description='RUC 과금 결과 전수 검증')
	ap.add_argument('--save', action='store_true', help='현재 상태를 기준선으로 저장')
	ap.add_argument('--detail', metavar='ID', help='특정 검사의 위반 행 전체 출력')
	ap.add_argument('--limit', type=int, default=5, help='검사별 샘플 출력 줄수(기본 5)')
	args = ap.parse_args()

	cn = db_connect()
	cu = cn.cursor()
	cur = collect(cu)

	base = None
	if os.path.exists(BASELINE):
		with open(BASELINE, encoding='utf-8') as f:
			base = json.load(f)

	# ── 상세 모드 ─────────────────────────────────────────────────────────────
	if args.detail:
		rid = args.detail.upper()
		rule = next((r for r in RULES if r['id'] == rid), None)
		if rule is None:
			print('그런 검사 없음: %s  (가능: %s)' % (rid, ', '.join(r['id'] for r in RULES)))
			return 2
		d = cur['_상세'][rid]
		print('[%s] %s — %d건\n  근거: %s\n' % (rid, rule['name'], d['건수'], rule['why']))
		print('  ' + ' | '.join(d['컬럼']))
		for r in d['행']:
			print('  ' + ' | '.join('' if v is None else str(v) for v in r))
		return 0

	# ── 요약 ──────────────────────────────────────────────────────────────────
	print('=' * 78)
	print(' RUC 과금 결과 검증   (읽기 전용)')
	print('=' * 78)
	b = base or {}
	print(' 행수 %d%s   트립 %d   총거리 %s m%s' % (
		cur['행수'], fmt_delta(cur['행수'], b.get('행수')),
		cur['트립수'], format(cur['총거리'], ','),
		fmt_delta(cur['총거리'], b.get('총거리'))))
	print(' 정렬 md5 %s%s' % (
		cur['md5'],
		'' if base is None else
		('   (기준선 %s %s)' % (b.get('md5'), '일치' if b.get('md5') == cur['md5'] else '변동'))))

	print('\n 유형별 (행수 / 거리m)')
	유형명 = {'0': '일반도로', '1': '개방형', '2': '폐쇄형', '3': '구간단속', '4': '주정차', '5': '면제'}
	for k in sorted(set(list(cur['유형별'].keys()) + list(b.get('유형별', {}).keys())), key=int):
		n = cur['유형별'].get(k, [0, 0])
		o = b.get('유형별', {}).get(k)
		mark = ''
		if o:
			if n[0] != o[0] or n[1] != o[1]:
				mark = '   <-- 기준선 %d행 %sm' % (o[0], format(o[1], ','))
		print('   %-2s %-6s  %4d행  %10s m%s' % (k, 유형명.get(k, ''), n[0], format(n[1], ','), mark))

	print('\n 과금구분')
	for k in sorted(set(list(cur['과금구분'].keys()) + list(b.get('과금구분', {}).keys()))):
		n = cur['과금구분'].get(k, 0)
		print('   %-4s %4d%s' % (k, n, fmt_delta(n, b.get('과금구분', {}).get(k))))

	# ── 불변식 ────────────────────────────────────────────────────────────────
	print('\n' + '-' * 78)
	print(' 불변식 검사')
	print('-' * 78)
	악화, 개선 = [], []
	for rule in sorted(RULES, key=lambda r: r['id']):
		rid = rule['id']
		n = cur['검사'][rid]
		o = b.get('검사', {}).get(rid)
		delta = fmt_delta(n, o)
		if o is not None:
			if n > o:
				악화.append(rid)
			elif n < o:
				개선.append(rid)
		flag = '  ' if n == 0 else ('!!' if rule['severity'] == 'ERROR' else ' *')
		if cur['_상세'][rid].get('미측정'):
			print('   %-3s %-34s   (로그 없음 — 미측정)' % (rid, rule['name']))
			continue
		print('%s %-3s %-34s %5d건%s' % (flag, rid, rule['name'], n, delta))

		if n > 0 and args.limit > 0:
			d = cur['_상세'][rid]
			for r in d['행'][:args.limit]:
				print('        ' + ' | '.join('' if v is None else str(v) for v in r))
			if n > args.limit:
				print('        ... 외 %d건  (전체: --detail %s)' % (n - args.limit, rid))

	print('-' * 78)
	print(' !! = ERROR(물리적 불가·명백한 오적재)   * = WARN(정책상 허용될 수도 있는 후보)')
	if base is None:
		print(' 기준선 없음 — 수정 착수 전에 --save 로 저장해 두면 다음부터 증감이 표시된다')
	else:
		if 악화:
			print(' 기준선 대비 악화: ' + ', '.join(악화))
		if 개선:
			print(' 기준선 대비 개선: ' + ', '.join(개선))
		if not 악화 and not 개선:
			print(' 기준선 대비 위반 건수 변동 없음')

	if args.save:
		out = {k: v for k, v in cur.items() if not k.startswith('_')}
		with open(BASELINE, 'w', encoding='utf-8') as f:
			json.dump(out, f, ensure_ascii=False, indent=1)
		print('\n 기준선 저장: %s' % BASELINE)

	cn.close()
	return 1 if any(cur['검사'][r['id']] > 0 for r in RULES if r['severity'] == 'ERROR') else 0


if __name__ == '__main__':
	sys.exit(main())

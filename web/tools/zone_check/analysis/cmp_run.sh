#!/bin/bash
# 엔진 전후 비교용 전체 재처리기 (2026-10-04 최정우)
# 사용: cmp_run.sh <엔진 bin 디렉터리> <출력파일>
#   실주행(real_trips.txt) + 시나리오(park_scenario.py, 98%) 트립을 초기화 → 지정 엔진 기동 → 처리 대기
#   → 엔진 종료(종료 플러시 = TTL 마감과 같은 경로) → 과금 행·매칭 상태 덤프.
#   수정 전 엔진은 git worktree 로 이전 커밋을 꺼내 빌드하고 link.psf 를 복사해 두 결과를 diff 한다.
# 주의: 같은 DB 에 엔진은 하나만 — 실행 중인 MapMatchSvr 를 모두 내린다. 실주행 트립의 과금 행을 지우고
#   매칭 상태를 0 으로 되돌리므로(재처리) 운영 DB 에서는 쓰지 말 것.
set -u
BIN=$(cd "$1" && pwd); OUT=$2
AN=$(cd "$(dirname "$0")" && pwd)
CFG=$AN/../../../../MapMatchSvr/bin/config.ini
stop() { pkill -TERM -x MapMatchSvr; for i in $(seq 1 90); do [ -z "$(pgrep -x MapMatchSvr)" ] && return 0; sleep 1; done; echo "엔진 종료 실패"; exit 1; }
stop
db() { sed -n '/^\[database\]/,/^\[/{s/^'"$1"'=//p}' "$CFG" | tr -d '\r'; }
export PGPASSWORD="$(db password)"
Q="psql -X -h $(db host) -p $(db port) -U $(db userid) -d $(db name)"
LIST=$(sed "s/.*/'&'/" "$AN/real_trips.txt" | paste -sd,)
cd "$AN" && python3 park_scenario.py clean >/dev/null
$Q -q -c "DELETE FROM ruc.prim_chargehand WHERE trip_id IN ($LIST)"
$Q -q -c "UPDATE ruc.prim_rawgps SET match_status=0, match_lat=NULL, match_lon=NULL, match_link_id=NULL, intersect_len=0 WHERE trip_id IN ($LIST)"
cd "$BIN" && ./run_svr.sh >/dev/null 2>&1; sleep 3
cd "$AN" && python3 park_scenario.py load >/dev/null
for i in $(seq 1 120); do n=$($Q -At -c "select count(*) from ruc.prim_rawgps where match_status in (0,2)"); [ "$n" = "0" ] && break; sleep 1; done
sleep 3; stop; sleep 1
$Q -A -F'|' -c "select trip_id, trip_seq, charge_type ct, charge_yn||'/'||charge_status st, start_gps_seq s, end_gps_seq e, dist_m, stay_seconds stay, speed_kmh spd, from_id, to_id, non_charge_reason ncr, trip_end_dt, (reg_dt<=upd_dt) regok from ruc.prim_chargehand where trip_id IN ($LIST) or trip_id like '98%' order by trip_id, trip_seq::int" > "$OUT"
$Q -A -F'|' -c "select match_status, count(*) from ruc.prim_rawgps where trip_id IN ($LIST) or trip_id like '98%' group by 1 order by 1" >> "$OUT"
wc -l < "$OUT"
echo "※ 엔진이 내려간 상태다. 필요하면 MapMatchSvr/bin/run_svr.sh 로 다시 띄울 것."

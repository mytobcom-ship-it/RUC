# RUC 과금 DB 스키마 기준선 v1.0

- **기준일**: 2026-08-07
- **원본 출처**: 운영 DB `ruc` 스키마(`59.11.91.162`) — `\d` + `pg_description`(테이블·컬럼 코멘트) + `information_schema`(FK 제약조건) 전수 조회 결과. 상세는 [`web/docs/charge-tables.html`](../web/docs/charge-tables.html), 논의 경과는 [`README.txt`](README.txt) 참고.
- **성격**: 이 문서는 **문서 스냅샷**입니다. 원격 DB(`59.11.91.162`) 접속이 되지 않아 `pg_dump --schema-only`로 직접 뜬 DDL이 아니라, 앞서 조사·확인된 `web/docs/charge-tables.html`(2026-08-04 스키마 조사 + 2026-08-06 실데이터 조사) 내용을 기준선으로 고정한 것입니다. **DB 접속이 복구되면 실제 DDL과 대조 검증할 것.**
- **용도**: 도로 요금 과금(개방형/폐쇄형/구간단속/주정차) 기능을 이 구조 위에 구현할 예정이며, 이후 스키마 변경은 이 문서를 v1.0 기준선으로 삼아 `v1.1`, `v1.2` … 형태의 버전업 문서로 델타를 기록한다.

---

## 0. 개요 · ZONEKIND 분류

`base_codesc`의 `ZONEKIND` 코드가 과금 유형 분류 그 자체.

| 코드 | 명칭 | note | 데이터 | 대응 테이블 |
|---|---|---|---|---|
| 01 | 일반도로(거리기반과금) | dist | 1,525,177건 | base_mapinfo |
| 02 | 유료도로(개방식) | tollOpen | 31,383건 | base_mapinfo |
| 03 | 유료도로(폐쇄식) | tollClosed | 0건 ← 테스트 대상 | base_roadlink+base_tollgate+base_tollsection |
| 04 | 과속·구간단속 | section | 0건 ← 테스트 대상 | base_mapinfo |
| 05 | 주정차단속 | parking | 0건 ← 테스트 대상 | base_mapinfo |

---

## 1. 마스터 테이블

### base_road — ⚠️ 은퇴 예정 (신규 작업에서 사용 금지)

테이블 코멘트: "(은퇴) 2026-08-04 base_roadlink로 대체 — 참조 코드·FK 없음. 확인 후 삭제 예정". 실제 참조 FK 0건.

| 컬럼 | 타입 | 설명 |
|---|---|---|
| road_id (PK) | varchar(20) | 노선 ID (RD+3자리, seq_road_id 채번) |
| road_nm | varchar(100) | 노선명(표시용) |
| toll_type | varchar(1) | 과금 유형(O 개방식/C 폐쇄식/N 무료) |
| use_yn | varchar(1) | 사용 여부(Y/N) |
| reg_dt / upd_dt | timestamp | 등록/수정일시 |

> `base_mapinfo.road_id` 컬럼 코멘트가 여전히 이 테이블을 가리키고 있음 — 은퇴 공지와 모순되는 stale 코멘트로 확인됨.

### base_roadlink — 현재 진짜 도로/구역 마스터

테이블 코멘트: "운영 설정 도로/구역 — 지도관리에서 직접 그려 등록. road_kind: ZONEKIND(01 일반/02 유료 개방식/03 유료 폐쇄식/04 과속·구간단속/05 주정차단속)"

| 컬럼 | 타입 | 설명 |
|---|---|---|
| road_id (PK) | varchar(20) | 도로 ID — RL-Znnn 자동 채번 |
| road_kind | varchar(2) | ROADKIND(0-base): 0 일반/1 유료(개방식)/2 유료(폐쇄식)/3 과속·구간단속/4 주정차단속 |
| road_nm | varchar(100) | 도로명(표시용) |
| geom_type | varchar(4) | LINE(선: 도로/구간) / POLY(면: 단속범위) |
| coords | jsonb | 좌표열 [[경도,위도],...] WGS84 |
| speed_limit_kmh | numeric | 제한속도(km/h) — road_kind=3 판정 기준 |
| use_yn | char(1) | Y 사용 / N 미사용 |
| reg_dt / upd_dt | varchar(14) | YYYYMMDDHH24MISS |
| admin_id | varchar(20) | 최종 수정 관리자(base_workerinfo.worker_no) |

> `base_tollgate.road_id`가 이 테이블을 하드 FK로 참조. 폐쇄형 테스트는 여기 1건 등록 후 게이트를 붙여야 함.

> **`road_link`(표준노드링크)와의 관계 (사용자 논의, 2026-08-08)**: `base_roadlink`는 표준노드링크와 연계하는 컬럼이
> 전혀 없는 **완전히 독립적인 "가상 링크"**로 보는 게 정확하다. 실제 도로망 위상(표준노드링크 그래프)과 무관하게
> 관리자가 지도에 그린 `coords`로 "도로 전체"를 하나의 논리적 단위로 등록하는 것이며, `f_node`/`t_node` 같은
> 위상(시작/끝) 컬럼 자체가 없다. 폐쇄형(03) 과금에서 이 "가상 링크"의 양 끝(시작/끝) 역할은
> `base_roadlink`가 아니라 `base_tollgate.gate_div`(I=진입/O=진출)가 대신한다 — 실제 요금도 이동거리 계산이
> 아니라 `base_tollsection`의 고정요금표(진입×출구×차종)를 그대로 쓰는 것이 이와 같은 맥락. 즉
> `base_roadlink` = 가상 링크의 "몸통"(경로 표시용, 위상 없음), `base_tollgate`(I/O) = 그 양 끝 노드 역할.

### base_tollgate — 영업소(게이트) 마스터 (3건)

테이블 코멘트: "영업소 마스터 - 개방식 본선(M)/폐쇄식 입구(I)·출구(O)·겸용(B)"

| 컬럼 | 타입 | 설명 |
|---|---|---|
| tollgate_id (PK) | varchar(20) | 영업소 ID (TG+4자리, seq_tollgate_id 채번) |
| road_id | varchar(20) | FK → base_roadlink.road_id |
| tollgate_nm | varchar(100) | 영업소명 |
| gate_div | varchar(1) | M 개방식 본선 / I 폐쇄식 입구 / O 폐쇄식 출구 / B 겸용 (현재 데이터는 M/I/O만 관측) |
| lon / lat | numeric | 경도/위도(WGS84) |
| link_id | varchar(20) | 기준 링크 — **소프트 참조**, `base_mapinfo.link_id` (road_link.link_id 아님) |
| use_yn | varchar(1) | 사용 여부 |
| reg_dt / upd_dt | timestamp | 등록/수정일시 |

> ⚠️ 실데이터 3건 전부 `link_id` NULL — 링크ID 매칭 불가, 좌표거리 임계값 방식 필요(추정).

### base_tollfare — 개방식 요금표

테이블 코멘트: "개방식 요금표 - 본선 영업소x차종x적용일. 개정 = 새 fare_date 행 추가(행 자체가 이력)"

| 컬럼 | 타입 | 설명 |
|---|---|---|
| tollgate_id (PK-1) | varchar(20) | FK → base_tollgate (gate_div=M) |
| car_type (PK-2) | varchar(1) | 차종(CARTYPE 1~6) |
| fare_date (PK-3) | varchar(8) | 적용 시작일(YYYYMMDD) |
| toll_fare | integer | 통행요금(원), NULL=미정 |
| admin_id | varchar(20) | 저장 관리자 |
| reg_dt / upd_dt | varchar(14) | 등록/수정 일시 |

### base_tollsection — 폐쇄식 구간요금표

테이블 코멘트: "폐쇄식 구간요금표 - 입구x출구 영업소x차종x적용일. 개정 = 새 fare_date 행 추가"

| 컬럼 | 타입 | 설명 |
|---|---|---|
| entry_tollgate_id (PK-1) | varchar(20) | FK → base_tollgate (gate_div I/B) |
| exit_tollgate_id (PK-2) | varchar(20) | FK → base_tollgate (gate_div O/B) |
| car_type (PK-3) | varchar(1) | 차종(CARTYPE 1~6) |
| fare_date (PK-4) | varchar(8) | 적용 시작일 |
| toll_fare | integer | 구간 통행요금(원), NULL=미정 |
| admin_id | varchar(20) | 저장 관리자 |
| reg_dt / upd_dt | varchar(14) | 등록/수정 일시 |

> (진입ID, 출구ID) 조합 자체가 PK — 별도 짝짓기 키 불필요. 없는 조합 발생 시 처리정책은 미정.

### base_mapinfo — 지도 구역/도로 마스터 (강릉시)

테이블 코멘트: "지도 구역/도로 마스터(강릉시) - 일반도로/유료도로/과속·구간단속/주정차단속 구역을 위경도 지오메트리로 관리"

| 컬럼 | 타입 | 설명 |
|---|---|---|
| zone_id (PK) | varchar(20) | 구역 ID (예: GN-Z001) |
| zone_kind | varchar(2) | ZONEKIND 코드. ⚠️ 실측: 01·02는 앞자리 0 없는 '1'·'2' 1글자로 저장(2026-08-06 확인) |
| zone_nm | varchar(100) | 구역명(예: 정동진 해안도로) |
| geom_type | varchar(4) | LINE(선=도로/구간) / POLY(면=단속범위) |
| coords | jsonb | 좌표열 [[경도,위도],...] WGS84. POLY는 닫힌 링 |
| speed_limit_kmh | numeric(6,1) | 제한속도(km/h) — 과속·구간단속용, 없으면 NULL |
| link_id | varchar(20) | 표준노드링크 LINK_ID 연계(선택). UNIQUE(NULL 제외) |
| color | varchar(10) | 표시색 override(#RRGGBB) |
| use_yn | char(1) | 사용여부 |
| reg_dt / upd_dt | timestamp | 등록/최종 수정일시 |
| admin_id | varchar(20) | 수정 관리자(worker_no) |
| f_node / t_node | varchar(20) | 표준노드링크 시작/종료노드 — 구간 입/출구 판정용 |
| road_id | varchar(20) | 소속 노선 — 코멘트상 `base_road.road_id`(⚠️ 은퇴 테이블 참조, stale 가능성). NULL=미배정 |
| min/max lon/lat | numeric | 경계상자(영역 조회용) |

> ⚠️ 주정차(zone_kind=05) 유예시간·과태료 컬럼이 이 테이블에 없음. 범칙금은 `base_fare`에서 가져오는 것으로 역산 확인됐으나, "유예시간(10분)" 값 자체를 저장하는 컬럼은 어디에도 없음 — 코드 하드코딩 여부 결정 필요.

> **`road_link`(표준노드링크)와의 관계 (사용자 논의, 2026-08-08)**: `base_mapinfo`는 `road_link`와 동일 테이블이
> 아니라(`road_link` 코멘트: "지도 표출(base_mapinfo)과 분리"), `link_id`/`f_node`/`t_node`로 **선택적**으로만
> 연계되는 **별개** 테이블이다. 다만 `zone_kind`에 따라 정밀도(粒度)가 다르다 — `01`(일반)/`02`(개방식)는
> 152만+3.1만 건으로 표준노드링크급으로 잘게 쪼갠 것으로 추정되는 반면, `04`(구간단속)/`05`(주정차)는 예시
> `zone_id`(ZS01/ZP01류)로 볼 때 훨씬 굵은 "구역/구간" 단위다. 즉 "base_mapinfo = 표준노드링크"가 아니라
> "표준노드링크와 별개이되, 01/02는 우연히 비슷한 세밀도, 04/05는 훨씬 굵은 단위"로 이해하는 게 정확하다.
> (`base_roadlink`와 대비: `base_roadlink`는 표준노드링크 연계 컬럼 자체가 없는 완전 독립적 "가상 링크" — 위 1절 base_roadlink 설명 참고.)

### base_fare — 차종별 통행요금·범칙금 기준표 (6건, 차종당 1행)

테이블 코멘트: "금액정보(차종별, 과태료)". PK=car_type 단일 컬럼(신/구 요금 한 행 보관, 개정 이력은 base_farehist).

| 컬럼 | 타입 | 설명 |
|---|---|---|
| car_type (PK) | varchar(1) | 차종구분. ⚠️ 컬럼 코멘트는 1~4(경차/소형/중형/대형)뿐이지만 실데이터는 1~6행 존재 — CARTYPE 라벨과 다르게 적혀있어 재확인 필요(조인 키 자체는 호환) |
| fare_date | varchar(8) | 요금정보 적용일자 |
| new_fare / old_fare | numeric(6,0) | 신/구 요금 — charge_type=0(NODE_STEP, ZONEKIND=01) 정산에 사용되는 것으로 확인 |
| new/old_parking_fine_10min_down | numeric(6,0) | 주정차 과태료, 체류 10분 미만 |
| new/old_parking_fine_10min_up | numeric(6,0) | 주정차 과태료, 체류 10분 이상 |
| new/old_speeding_fine_10km_down | numeric(6,0) | 과속 과태료, 초과속도 10km/h 미만 |
| new/old_speeding_fine_20km_down | numeric(6,0) | 과속 과태료, 초과속도 10~20km/h |
| new/old_speeding_fine_30km_down | numeric(6,0) | 과속 과태료, 초과속도 20~30km/h |
| new/old_speeding_fine_30km_up | numeric(6,0) | 과속 과태료, 초과속도 30km/h 이상 |
| inst_dt / admin_id | varchar(14)/varchar(4) | 등록일시/등록관리자 ID |

> ⚠️ 범칙금 산정의 유일한 기준인지 미확정. `prim_vltncharge` 111건 중 이 표와 일치하는 소액(500~3,200원)과, 표에 없는 고액(30,000~90,000원) 패턴이 동일 테이블에 혼재 — 담당자 재확인 필요.

### base_carinfo — 차량 기본정보 (150건, car_type 출처)

테이블 코멘트: "차량 기본정보 - 회원(cust_no)별 차량, 클라이언트키(device_key)는 차량 소속"

| 컬럼 | 타입 | 설명 |
|---|---|---|
| cust_no / car_seq (PK) | varchar(10)/smallint | 회원번호(MBR+6자리) + 회원별 차량 순번 |
| device_key | varchar(50) | 단말키(CAR+6자리), 발급 후 불변. `prim_chargehand`/`prim_tollcharge`/`prim_vltncharge`의 device_key와 동일값. UNIQUE 1:1 조인 |
| car_type | varchar(8) | 차종(CARTYPE 1~6), 컬럼 폭 8이나 실제 저장값은 1~6 단일 숫자 |
| car_no | varchar(12) | 차량번호 |
| car_status | smallint | 0 비활성/1 활성/2 정지 |
| reg_dt / mdft_dt | varchar(14) | 등록/수정일시 |

> car_type 조인 경로: `prim_chargehand.device_key` = `base_carinfo.device_key` → `car_type`. MapMatchSvr 소스엔 이 조회 코드가 없음.
> ⚠️ 혼동 주의: `tb_vhcl_info`(회원 앱쪽, PK=mbr_id+vhcl_sn, carmdl_type 컬럼)는 이름은 비슷하나 코드체계가 다른 별개 테이블 — 과금 조인에 쓰면 안 됨.

### road_link — 표준노드링크 (과금 판정 원천용, 지도 표출과 분리)

테이블 코멘트: "표준노드링크 LINK(전국, 2025-07-16판) — 과금 판정/원천용. 지도 표출(base_mapinfo)과 분리"

| 컬럼 | 타입 | 설명 |
|---|---|---|
| link_id (PK) | varchar(10) | 링크 ID |
| f_node / t_node | varchar(10) | 시작/종료 노드 ID |
| lanes | smallint | 차로 수 |
| road_rank | varchar(3) | 도로 등급(101 고속국도~107 기타) |
| road_type | varchar(3) | 도로 유형 |
| road_no / road_name | varchar | 노선번호/도로명 |
| road_use | varchar(1) | 사용 여부(0 사용) |
| max_spd | smallint | 제한속도(km/h) |
| length_m | numeric(12,3) | 링크 연장(m) |
| update_date | varchar(8) | 원천 갱신일 |
| min/max lon/lat | numeric | 경계상자 |
| coords | jsonb | 좌표열 (MultiLineString) |

> "과금 판정/원천용"이 명시된 테이블 — 실제 판정 로직은 이 테이블을 기준으로 하고, `base_mapinfo`는 화면 "표출"용으로 분리되어 있음.

---

## 2. 공통코드

### base_codelc / base_codesc

| 테이블 | 컬럼 | 설명 |
|---|---|---|
| base_codelc | codelc (PK) | 대분류 코드 |
| | codelc_nm | 대분류 코드명 |
| | codelc_note | 대분류 코드 설명 |
| | codelc_use_yn | 사용 여부 |
| base_codesc | codelc, codesc (복합 PK) | 대분류/소분류 코드 |
| | codesc_nm | 소분류 코드명 |
| | codesc_note | 소분류 코드 설명(영문 별칭) |
| | ary_ordr | 정렬순서 |
| | codesc_use_yn | 사용 여부 |

### ZONEKIND (1-base, base_mapinfo.zone_kind가 참조)

| codesc | codesc_nm | codesc_note |
|---|---|---|
| 01 | 일반도로(거리기반과금) | dist |
| 02 | 유료도로(개방식) | tollOpen |
| 03 | 유료도로(폐쇄식) | tollClosed |
| 04 | 과속·구간단속 | section |
| 05 | 주정차단속 | parking |

### ROADKIND (0-base, base_roadlink.road_kind가 참조 — base_codelc 부모행 누락)

| codesc | codesc_nm | codesc_note |
|---|---|---|
| 0 | 일반도로 | dist |
| 1 | 유료도로(개방식) | tollOpen |
| 2 | 유료도로(폐쇄식) | tollClosed |
| 3 | 과속·구간단속 | section |
| 4 | 주정차단속 | parking |

### CARTYPE (base_tollfare/base_tollsection.car_type, prim_tollcharge.vehicle_type이 참조)

| codesc | codesc_nm |
|---|---|
| 1 | 1종(소형) |
| 2 | 2종(중형) |
| 3 | 3종(대형) |
| 4 | 4종(대형) |
| 5 | 5종(대형) |
| 6 | 6종(경차) |

> ⚠️ **코드체계 불일치 주의**: ZONEKIND(1-base) / ROADKIND(0-base) / `prim_chargehand.charge_type`(0~4) / `prim_tollcharge.charge_type`(0~2) / `prim_vltncharge.violation_type`(0~1)이 전부 다른 숫자 체계. 변환표 없이 매핑하면 버그.

---

## 3. 결과(판정 로그) 테이블

### prim_chargehand — 과금 대상 통합 테이블 (검증서버가 Insert)

| 컬럼 | 타입 | 설명 |
|---|---|---|
| trip_id (PK-1) | varchar(60) | {차량 고유번호}_{YYYYMMDDHH24MISS} |
| device_key (PK-2) | varchar(36) | 모바일 앱 인증키 |
| trip_seq (PK-3) | integer | 과금부과 순번(1~N) |
| charge_type | smallint | 0 NODE_STEP / 1 OPEN_ROAD / 2 CLOSED_ROAD / 3 SPEED / 4 PARKING |
| charge_unit | smallint | 0 NODE / 1 LINK |
| link_id | varchar(20) | charge_unit=LINK일 때만 |
| from_id / to_id | varchar(20) | 출발/도착 노드 또는 링크 시작/종료 노드 |
| from_lat/lon, to_lat/lon | numeric(10,6) | 출발/도착 좌표 |
| dist_m | integer | 구간 거리(m) — 폐쇄식 요금 산출 기준 |
| speed_kmh | smallint | 구간 실측/평균 속도 |
| speed_limit_kmh | smallint | 제한속도, 0=해당없음 |
| stay_seconds | integer | 체류 시간(초) — 주정차 위반 판단 |
| zone_id / zone_name | varchar | 관련 구역 ID/명(비정규화) |
| occur_dt | varchar(14) | 과금/위반 발생 시각 |
| trip_start_dt / trip_end_dt | varchar(14) | 운행 시작/종료 시각 |
| charge_status | smallint | 0 PENDING / 1 PROCESSING / 2 CHARGED / 3 SKIP |
| reg_dt / upd_dt | varchar(14) | 등록/수정 일시 |
| charge_yn | varchar(1) | 과금 대상 여부 — 위치검증서버 적재 |
| non_charge_reason | smallint | 비과금 사유 — 위치검증서버 적재 |

#### 레코드 단위 설계 의도 (사용자 확인, 2026-08-07)

DB 코멘트가 아니라 **사용자가 직접 확인해준 설계 의도**입니다 — 향후 구현의 기준이 되므로 우선 참조용으로 기재.

> 운행 시작부터, **비과금 도로 구간**(시작~종료)을 한 레코드로, 그다음 **과금 부과 도로 구간**(시작~종료)을 한 레코드로 — 이런 식으로 **동일 과금 상태(과금/비과금)가 이어지는 구간 하나당 레코드 1건**을 적재하려는 테이블. 즉 GPS 포인트 단위나 링크(link) 단위가 아니라, **과금 상태가 바뀌는 지점마다 구간을 끊어서** 기록하는 구조.

**적재 시점 (사용자 확인, 2026-08-08):** 트립 종료 후 일괄 처리가 아니라, **주행을 시작하면서 맵매칭을 진행하는 동시에 과금정보 테이블을 실시간으로 확인해가며** 구간(비과금/과금)이 끝날 때마다 그때그때 1건씩 적재하는 테이블입니다. 즉 검증서버가 GPS를 처리하는 중간중간 레코드가 순차적으로 쌓입니다.

**레코드 개수 예시 (사용자 확인, 2026-08-08):**
- 한 트립 안에서 비과금/과금 구간이 여러 번 번갈아 나오면, 상태가 바뀔 때마다 레코드가 추가되어 여러 건이 적재될 수 있음
- 한 트립 전체에 과금 도로가 전혀 없으면 → 시작~종료 전 구간이 비과금 1레코드만 적재
- (대칭적으로) 트립 전체가 과금 도로면 → 과금 1레코드만 적재

기존에 확인된 컬럼으로 이 의도를 매핑해보면:

| 필요 개념 | 매핑 컬럼 | 비고 |
|---|---|---|
| 이 구간이 과금/비과금인지 | `charge_yn` | Y/N |
| 비과금이면 사유 | `non_charge_reason` | smallint 코드 |
| 구간 시작 지점 | `from_id`, `from_lat/lon` | |
| 구간 종료 지점 | `to_id`, `to_lat/lon` | |
| 구간 길이 | `dist_m` | 컬럼 설명엔 "폐쇄식 요금 산출 기준"이라고만 돼 있으나, 과금/비과금 구분 없이 모든 세그먼트 레코드에 공통으로 쓰이는 것으로 추정 |
| 트립 내 구간 순번 | `trip_seq` | "과금부과 순번(1~N)" — 과금·비과금 구간이 섞여도 순서대로 채번되는 것으로 추정 |
| 구간 발생 시각 | `occur_dt` | 단일 시각 컬럼 — 구간의 시작/종료 각각의 시각은 별도 컬럼 없음(아래 미확인 항목 참고) |

⚠️ **미확인 항목 — 실 DB 대조검증 시 확인 필요**
1. `occur_dt`가 구간 시작 시각인지 종료(확정) 시각인지 불명확 — 구간 자체의 시작·종료 시각을 각각 담는 컬럼이 현재 문서화된 스키마엔 안 보임(`trip_start_dt`/`trip_end_dt`는 트립 전체 기준). `speed_kmh`(구간 평균속도)를 구하려면 구간 소요시간이 필요한데, 어디서 계산하는지 확인 필요. (2026-08-08 사용자 확인으로 부분 해소: INSERT 자체는 트립 종료 후 일괄이 아니라 구간이 끝나는 시점마다 실시간으로 발생 — 따라서 `occur_dt`는 구간 종료(확정) 시각일 가능성이 높음. 다만 구간 "시작" 시각을 별도로 담는 컬럼 부재 문제는 여전히 남아있어, 구간 소요시간·평균속도 계산 방식은 실 데이터 대조 필요.)
2. `charge_type`(0 NODE_STEP 등)이 과금 구간과 비과금 구간 레코드 모두에 동일하게 쓰이는지, 아니면 비과금 구간은 다른 값(혹은 NULL)을 쓰는지 확인 필요.
3. 비과금 구간 레코드도 `prim_tollcharge`/`prim_vltncharge`로 이어지지 않고 `prim_chargehand`에만 남는 것으로 보이는데(과금서버가 charge_yn=N 은 건너뛸 것으로 추정), 이 필터링 지점이 어디인지(과금서버 쿼리 조건) 확인 필요.

### prim_tollcharge — 통행료 과금 내역 (과금서버가 Insert)

| 컬럼 | 타입 | 설명 |
|---|---|---|
| trip_id / device_key / trip_seq (PK) | varchar/varchar/int | CHARGE_TARGET 참조 |
| vehicle_type | smallint | 차종(1~5종 — CARTYPE 1~6종과 범위 불일치, 확인 필요) |
| charge_type | smallint | 0 NODE_STEP / 1 OPEN_ROAD / 2 CLOSED_ROAD (SPEED/PARKING 없음) |
| from_id / to_id | varchar(20) | 출발/도착 노드 또는 링크 ID |
| fare_amount | integer | 기본 요금(원) |
| vat_amount | integer | 부가세(원) = ROUND(fare_amount × 0.1) — 361건 중 352건(97.5%) 일치, 75건 예외(=0) |
| final_amount | integer | 최종 과금액 = fare_amount + vat_amount |
| charge_status | smallint | 0 PENDING_AUDIT / 1 CONFIRMED / 2 CANCELLED / 3 MANUAL_AUDIT |
| trip_start_dt / trip_end_dt | varchar(14) | 운행 시작/종료 시각 |
| reg_dt | varchar(14) | 등록 일시 |
| app_send_yn / app_send_dt | varchar | 앱서버 전송 여부/일시 |

### prim_vltncharge — 위반 과금 내역 (과금서버가 Insert)

| 컬럼 | 타입 | 설명 |
|---|---|---|
| trip_id / device_key / trip_seq (PK) | varchar/varchar/int | CHARGE_TARGET 참조 |
| violation_type | smallint | 0 SPEED / 1 PARKING |
| from_id / to_id | varchar(20) | 위반 구간 출발/도착 노드 또는 링크 ID |
| lat / lon | numeric(10,6) | 위반 발생 좌표 |
| zone_id | varchar(20) | 위반 구역 ID |
| speed_limit_kmh / measured_speed_kmh | smallint | 제한/실측 속도(과속 위반 시) |
| stay_minutes | integer | 체류 시간(분) — prim_chargehand.stay_seconds(초)와 단위 다름 |
| violation_dt | varchar(14) | 위반 발생 일시 |
| fine_amount | integer | 범칙금(원) |
| reg_dt | varchar(14) | 등록 일시 |
| app_send_yn / app_send_dt | varchar | 앱서버 전송 여부/일시 |

---

## 4. 기준선 대비 알려진 미해결/불확정 사항

구현 착수 전 확인이 필요한 항목만 압축. 전체 15건 목록은 [`web/docs/charge-issues.html`](../web/docs/charge-issues.html) 참고.

1. **OPEN 모델 확정 필요(최우선)** — "게이트 통과 즉시 정액" vs "링크 hop 누적거리" 중 무엇이 맞는지 담당자 확인 없이는 착수 불가.
2. **CLOSED_ROAD durable 기록 설계** — 세션(TTL 1h)이 장시간 정차 시 만료되어 진출해도 무과금되는 문제, 대응 설계만 완료.
3. **base_mapinfo point-in-polygon 판정 로직** — 코드에 없음. 신규 구현 필요.
4. **범칙금 기준 이원화** — base_fare 소액 패턴과 출처불명 고액 패턴이 prim_vltncharge에 혼재.
5. **HYBRID 게이트 미반영** — gate_div가 지점당 값 1개뿐이라 "진입 정액+진출 거리요금 동시부과" 표현 불가.
6. **base_tollsection 미등록 조합 처리정책** 없음.
7. **정차 감지 로직 자체 부재** — MapMatchSvr의 nDriveStatus/DRIVE_STATUS_PARKED 미사용.
8. **base_tollgate.link_id 전부 NULL** — 링크ID 매칭 불가.

---

## 5. 다음 단계

- [ ] 원격 DB 접속 복구 시 `pg_dump --schema-only -t 'base_road*' -t 'base_toll*' -t 'base_mapinfo' -t 'base_fare*' -t 'base_carinfo' -t 'road_link' -t 'prim_chargehand' -t 'prim_tollcharge' -t 'prim_vltncharge'`로 실제 DDL을 떠서 이 문서와 대조 검증
- [ ] 대조 검증 후 `RUC_과금DB_스키마_기준선_v1.1.md`로 델타 기록 (신규 컬럼/제약조건/코멘트 변경분만)
- [ ] 위 4절 미해결 사항 중 1~2번(OPEN 모델, CLOSED_ROAD durable 기록)은 실제 구현 착수 전 담당자 확인 필요

## 6. 관련 산출물 (2026-08-08 추가)

이 기준선을 근거로 개방식(OPEN) 임시 게이트 테스트용 재구성 DDL·샘플데이터를 만들었다 (`doc/README.txt` J절 참고).
`base_roadlink`/`base_tollgate`/`base_tollfare` 3개 테이블 한정이며, 위 5절의 pg_dump 대조검증이 아직 안 된
상태에서 만든 것이라 ⚠️ 표시된 세부 항목(시퀀스명·좌표 정밀도·인덱스 등)은 추정치다.

- `roadnet/sql/base_roadlink.sql`, `base_tollgate.sql`, `base_tollfare.sql` — 테이블 생성 DDL
- `roadnet/sql/base_open_gate_test_data.sql` — 임시 게이트 1건 + 차종별 요금 6건 샘플 INSERT
- `web/docs/charge-open-gate-test.html` — 위 내용 문서화

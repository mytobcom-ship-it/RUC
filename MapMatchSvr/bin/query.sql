-- MapMatchSvr query.sql (PostgreSQL / libpq: $1, $2, ...)
-- 근거: doc/RUC_위치검증서버_테이블설계서_v1.3.docx §2.1, §3.1
--
--
-- TRIP_EVENT   : 0=START, 1=NONE, 2=END
-- DRIVE_STATUS : 0=ON_ROAD, 1=IDLE, 2=PARKED, 3=TUNNELING, 4=OFF_ROAD(비과금 구역)
-- MATCH_STATUS : 0=PENDING, 1=MATCHED, 2=PROCESSING, 3=SKIP, 4=ERROR
--
-- ※※ 작성 규칙 (파서 CSQLAccessor::Load() 의 한계에서 나온 것 — 어기면 조용히 깨진다)
--   ① **SQL 본문 안에 `--` 주석을 쓰지 말 것.** 파서는 줄의 개행을 지우고 공백 하나로 이어
--      붙이기만 할 뿐 `--` 를 제거하지 않는다. 본문 중간에 `--` 가 있으면 그 뒤의 모든 줄이
--      한 줄짜리 주석 안으로 빨려 들어가 쿼리가 잘린다("구문 오류, 입력 끝부분").
--      2026-09-17 에 [trip_end]·[trip_abend] 두 SQL 이 실제로 이 이유로 실행 불능이었다.
--      설명은 지금 이 줄들처럼 `[key]` 헤더 **위**에 두면 된다(어느 섹션에도 담기지 않는다).
--   ② 각 SQL 은 세미콜론으로 끝낼 것. 없으면 다음 `[key]` 를 만나는 순간 조용히 버려진다.
--   ③ 한 섹션에 세미콜론으로 끝나는 문장을 2개 이상 두지 말 것 — 뒤엣것이 버려진다.
--      같은 섹션명을 두 번 쓰는 것도 마찬가지(2026-09-21 부터 그 경우 ERROR 로그가 남는다).
--   ④ `[key]` 는 줄 맨 앞에서 시작할 것(들여쓰면 SQL 본문으로 취급된다).
--   (2026-09-21 최정우 추가 — 규칙 자체는 SQLAccessor.cpp 의 @warning 과 동일 내용)
--
-- 처리 흐름
--   0) rawgps_recover     : 기동 시 PROCESSING(2) → PENDING(0) 복구
--   1) rawgps_select      : PENDING 예약(Reserve) + RETURNING
--   2) rawgps_update      : 맵매칭 결과 일괄 갱신 (MATCHED/SKIP/ERROR)
--   3) gate_select         : BASE_TOLLGATE 전량 조회 — CChargeDataLoader 기동 시(또는 주기 재조회 시)
--                           1회 실행, 결과를 인메모리 캐시 (2026-08-12 최정우 추가)
--   4) zone_select         : BASE_ROADLINK 전량 조회 — CChargeDataLoader::LoadZones() 가 gate_select 와
--                           동일 주기로 실행, 결과를 인메모리 캐시(road_id 키) (2026-08-12 최정우 추가)
--   5) charge_insert      : 6개 CHARGE_TYPE(0 일반도로·1 개방식·2 폐쇄식·3 구간단속·4 주정차·
--                           5 면제도로) 공용 bulk INSERT — CRawLogWorker::BulkInsertCharges() 가
--                           배치 종료 시(rawgps_update 성공 후) 실행
--                           (2026-08-13 최정우 수정 — 주정차 구현으로 게이트형 4유형 완료,
--                            2026-09-17 최정우 정정 — 그 뒤 추가된 일반도로(0)·면제도로(5)까지
--                            반영해 "4유형" 표기를 현행 6유형으로 고침)
--
-- DRIVE_STATUS=4 : 맵매칭·결과 저장은 0/2/3 과 동일, 과금 판별·CHARGE_TARGET 적재만 생략 (2026-07-10 최정우 수정)
--
-- rawgps_select RETURNING 컬럼 순서 (= RUC.PRIM_RAWGPS 컬럼 순서 — 로컬 ruc DB 전환, 2026-08-11 최정우 수정)
--   0:TRIP_ID 1:GPS_SEQ 2:DEVICE_KEY 3:GPS_DT 4:TRIP_EVENT 5:DRIVE_STATUS
--   6:GPS_LAT 7:MATCH_LAT 8:GPS_LON 9:MATCH_LON 10:INTERSECT_LEN(GPS↔세그먼트 교차점 거리,m) 11:RAW_VLD
--   12:SPEED_KMH 13:OBD_SPEED_KMH(차량 OBD 순간속도 — NULL 아니면 SPEED_KMH 보다 우선) 14:HEADING
--   15:ALTITUDE_M 16:ACCURACY_M 17:BATTERY 18:RECV_DT 19:MATCH_STATUS

-- ══ 이 파일의 SQL 이 전제하는 DB 스키마 ═══════════════════════════════════
-- 엔진 소스가 요구하는 컬럼·테이블인데 DB 에 없으면, 기동은 되지만 해당 SQL 만 런타임에
--   실패한다(과금 INSERT 전건 실패 등). 2026-08-29 실측: prim_chargehand 에 start_gps_seq 가
--   없어 charge bulk insert 가 전건 실패하고, 그 배치의 맵매칭 결과까지 되돌아갔다.
-- 새 환경에 DB 를 만들거나 스키마를 의심할 때 아래를 psql 로 실행할 것 (전부 재실행 안전).
-- ※ 아래 DDL 에는 끝의 세미콜론을 일부러 뺐다 — SQLAccessor 가 세미콜론을 SQL 종료로 보고
--   빈 키로 등록해버리기 때문이다. 복사해 실행할 때 각 문장 끝에 세미콜론을 붙일 것.
--
--   [charge_insert] / [charge_update] — 구역 진입·이탈 시점의 PRIM_RAWGPS.GPS_SEQ (웹뷰어 G순번)
--     ALTER TABLE ruc.prim_chargehand
--         ADD COLUMN IF NOT EXISTS start_gps_seq bigint NOT NULL DEFAULT 0,
--         ADD COLUMN IF NOT EXISTS end_gps_seq   bigint NOT NULL DEFAULT 0
--
--   [charge_insert] — 과금/비과금 사유 코드($31). 없으면 과금 bulk INSERT 가 전건 실패한다.
--     2026-09-15 수정으로 이 컬럼은 항상 채워진다(정상 과금 = 0, 더 이상 NULL 이 아니다).
--     ALTER TABLE ruc.prim_chargehand
--         ADD COLUMN IF NOT EXISTS non_charge_reason smallint
--     (2026-09-15 최정우 추가 — 2026-09-01 에 컬럼을 쓰기 시작했는데 이 목록에 빠져 있었다)
--
--   [rawgps_select] / [stale_recover] — PROCESSING(2) 예약 시각. 없으면 예약 UPDATE 자체가
--     실패해 맵매칭이 전혀 진행되지 않는다.
--     ALTER TABLE ruc.prim_rawgps
--         ADD COLUMN IF NOT EXISTS match_rsv_dt char(14)
--     CREATE INDEX IF NOT EXISTS ix_prim_rawgps_stale_proc
--         ON ruc.prim_rawgps (match_rsv_dt) WHERE match_status = 2
--
--   [parkfine_select] — 주정차 단속 개시 임계(분). 테이블이 없으면 임계 0(항상 등록)으로 동작.
--     CREATE TABLE IF NOT EXISTS ruc.base_parking_fine (
--         fine_id  varchar(20)  NOT NULL,
--         fine_nm  varchar(100) NOT NULL,
--         from_min integer      NOT NULL,
--         to_min   integer,
--         reg_dt   timestamp    NOT NULL DEFAULT now(),
--         upd_dt   timestamp,
--         admin_id varchar(4),
--         remark   varchar(200),
--         CONSTRAINT pk_base_parking_fine PRIMARY KEY (fine_id),
--         CONSTRAINT ck_base_parking_fine_from  CHECK (from_min >= 0),
--         CONSTRAINT ck_base_parking_fine_range CHECK (to_min IS NULL OR to_min > from_min))
--     INSERT INTO ruc.base_parking_fine (fine_id, fine_nm, from_min, remark)
--     VALUES ('PF-0001', '주정차 단속 개시(5분 이상)', 5, '기본 최소 단속시간 5분 = 300초')
--     ON CONFLICT (fine_id) DO NOTHING
--
-- 적용 확인
--   SELECT column_name FROM information_schema.columns
--    WHERE table_schema='ruc' AND ((table_name='prim_chargehand'
--          AND column_name IN ('start_gps_seq','end_gps_seq','non_charge_reason'))
--       OR (table_name='prim_rawgps' AND column_name='match_rsv_dt'))
--   SELECT MIN(from_min), MIN(from_min)*60 AS 임계초 FROM ruc.base_parking_fine
--
--   ※ 위 6가지(컬럼 4 · 인덱스 1 · 테이블 1)를 한 번에 점검하려면
--     doc/deploy_2026-09-15.sql 의 0절을 실행할 것 — 빠진 항목을 모두 지목하고 중단한다.
--     (2026-09-15 최정우 추가)
--
-- ※ 이 블록은 섹션([대괄호]) 밖이라 SQLAccessor 가 읽지 않는다 — 실행되지 않는 기록이므로
--   위 DDL 은 psql 로 직접 실행해야 한다. 섹션 "안"에는 절대 -- 주석을 쓰지 말 것
--   (로더가 SQL 을 한 줄로 이어붙여 뒤가 통째로 잘린다). (2026-08-29 최정우 추가)
-- ═════════════════════════════════════════════════════════════════════════

-- ── 0. 좀비 PROCESSING 복구 ────────────────────────────────────────────────
[rawgps_recover]
UPDATE RUC.PRIM_RAWGPS
SET
	MATCH_STATUS = 0
WHERE
	MATCH_STATUS = 2;

-- ── 0-1. 좀비 PROCESSING 운영 중 회수 ──────────────────────────────────────
-- [stale_recover] CServer::ProcessPeriodSec() 가 [server] stale_sec 주기로 실행.
--   rawgps_recover 는 "기동 시 1회"라 엔진이 계속 떠 있는 동안 2 에 갇힌 행을 못 푼다(워커 강제
--   종료·배치 실패 잔류분). 예약 시각(MATCH_RSV_DT)이 $1 초보다 오래된 행만 PENDING(0)으로 되돌려
--   다음 poll 에서 재처리되게 한다. 정상 처리 중인 행(예약 직후)은 임계 미만이라 건드리지 않는다.
--   MATCH_RSV_DT IS NULL 은 이 컬럼 도입 이전에 2 가 된 행 — 판정 근거가 없으므로 함께 회수한다.
--   $1 = 임계 초 (2026-08-29 최정우 추가)
[stale_recover]
UPDATE RUC.PRIM_RAWGPS
SET
	MATCH_STATUS = 0,
	MATCH_RSV_DT = NULL
WHERE
	MATCH_STATUS = 2
	AND (MATCH_RSV_DT IS NULL
		OR MATCH_RSV_DT < TO_CHAR(NOW() - ($1::INTEGER * INTERVAL '1 second'), 'YYYYMMDDHH24MISS'));

-- ── 1. 조회 + 예약(Reserve) ────────────────────────────────────────────────
-- [rawgps_select] PENDING(0) → PROCESSING(2) 예약, RETURNING 으로 행 반환
-- 제외 조건 없음 — RAW_VLD 도 값과 무관하게 전부 선택 대상(2026-08-19 최정우 수정). 이전엔
--   WHERE 에 RAW_VLD IS TRUE 를 둬서 RAW_VLD=false 행은 애초에 조회·예약이 안 됐음 — 그 결과
--   RawLogWorker::ShouldSkipGpsInput() 이 RAW_VLD=false 를 SKIP(3) 처리하는 로직이 이미
--   있는데도 그 행들이 이 함수까지 도달하지 못해 죽은 코드였고, 해당 행은 영원히 PENDING(0)에
--   갇혀 trip 이 완료돼도(END 도달) "진행중"으로 잘못 표시되는 원인이었음(실측: END 행 자체가
--   RAW_VLD=false 인 trip 은 세션 종료 처리조차 못 받음). 이제 RAW_VLD=false 행도 선택해와서
--   ShouldSkipGpsInput() 이 정상적으로 SKIP 처리하게 함.
-- DRIVE_STATUS 는 0(ON_ROAD)/1(IDLE)/2(PARKED)/3(TUNNELING)/4(OFF_ROAD) 전부 맵매칭 대상
--   (2026-08-13 최정우 수정 — 이전엔 DRIVE_STATUS=1(IDLE) 만 제외했었음. 서행 구간(IDLE)이
--   맵매칭·주정차 판정에서 통째로 안 보이는 문제가 있어 사용자 지시로 제외 조건 삭제 —
--   "모든 맵 매칭 시 DRIVE_STATUS 에서 0/1/2 는 포함되어야 한다")
-- $1 = LIMIT
-- 2026-08-18 최정우 수정 — 한때 TRIP_ID를 DEVICE_KEY_{시각} 형식으로 여기서 보정했었으나,
--   RawLogWorker::IsValidTripIdForDevice() 의 검증 기준 자체가 CAR_SEQ_NO(6자리 숫자)_{시각}
--   형식을 요구하도록 바뀌어(같은 날 수정) 방향이 반대가 됨 — 그 보정 로직을 남겨두면 유효한
--   CAR_SEQ_NO 형식을 오히려 무효한 DEVICE_KEY 형식으로 바꿔버리는 역효과가 나서 제거함.
--   원본(보정 없는) 형태로 되돌림.
-- 2026-08-23 최정우 수정 — 정렬에서 GPS_DT 제거. 트립 내 순서의 정본은 GPS_SEQ 다.
--   실측 000376_20260819140856: seq19(도착 이벤트 행)의 GPS_DT 가 직전 seq18 보다 3초 이르고
--   좌표도 seq17 과 동일했다. GPS_DT 우선 정렬이라 엔진에 17->19->18 순으로 들어갔고,
--   NeedsBeginReset() 이 seq18 을 역전(rollback)으로 판정해 세션 앵커를 버리고 BEGIN 으로
--   강등 -> BEGIN 은 heading 을 안 보고 거리만 봐서(bIgnoreHeading) 왕복분리 건너편 차로에
--   붙는 오매칭이 났다. GPS_SEQ 로만 정렬하면 17->18->19 가 되어 seq18 은 정상 처리되고,
--   뒤로 튄 중복 좌표인 seq19 는 기존 역행 판정으로 걸러진다.
-- ※ 이 블록 "안"에는 -- 주석을 쓰지 말 것 — 로더가 SQL 을 한 줄로 이어붙여 뒤가 통째로 잘린다.
[rawgps_select]
UPDATE RUC.PRIM_RAWGPS AS U
SET
	MATCH_STATUS = 2,
	MATCH_RSV_DT = TO_CHAR(NOW(), 'YYYYMMDDHH24MISS')
FROM (
	SELECT TRIP_ID, GPS_SEQ
	FROM RUC.PRIM_RAWGPS
	WHERE MATCH_STATUS = 0
	ORDER BY DEVICE_KEY ASC, TRIP_ID ASC, GPS_SEQ ASC
	LIMIT $1
	FOR UPDATE SKIP LOCKED
) AS S
WHERE U.TRIP_ID = S.TRIP_ID
  AND U.GPS_SEQ = S.GPS_SEQ
RETURNING
	U.TRIP_ID,
	U.GPS_SEQ,
	U.DEVICE_KEY,
	U.GPS_DT,
	U.TRIP_EVENT,
	U.DRIVE_STATUS,
	U.GPS_LAT,
	U.MATCH_LAT,
	U.GPS_LON,
	U.MATCH_LON,
	U.INTERSECT_LEN,
	U.RAW_VLD,
	U.SPEED_KMH,
	U.OBD_SPEED_KMH,
	U.HEADING,
	U.ALTITUDE_M,
	U.ACCURACY_M,
	U.BATTERY,
	U.RECV_DT,
	U.MATCH_STATUS;

-- ── 2. 결과 갱신 ──────────────────────────────────────────────────────────
-- [rawgps_update] PROCESSING(2) → MATCHED(1)/SKIP(3)/ERROR(4). DRIVE_STATUS=4 동일 갱신 (2026-07-10 최정우 수정)
-- 파라미터는 PRIM_RAWGPS 컬럼 순서대로 배열 (2026-07-15 최정우 재정렬)
-- $1=TRIP_ID[], $2=GPS_SEQ[], $3=MATCH_LAT[], $4=MATCH_LON[], $5=INTERSECT_LEN[](GPS↔세그먼트 교차점 거리,m), $6=MATCH_LINK_ID[], $7=MATCH_STATUS[]
[rawgps_update]
UPDATE RUC.PRIM_RAWGPS AS T
SET
	MATCH_LAT = CASE
		WHEN V.MATCH_LAT <> '' THEN V.MATCH_LAT::NUMERIC
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LAT
	END,
	MATCH_LON = CASE
		WHEN V.MATCH_LON <> '' THEN V.MATCH_LON::NUMERIC
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LON
	END,
	INTERSECT_LEN = CASE
		WHEN V.INTERSECT_LEN <> '' THEN V.INTERSECT_LEN::INTEGER
		WHEN V.MATCH_STATUS IN (3, 4) THEN 0
		ELSE T.INTERSECT_LEN
	END,
	MATCH_LINK_ID = CASE
		WHEN V.MATCH_LINK_ID <> '' THEN V.MATCH_LINK_ID
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LINK_ID
	END,
	MATCH_STATUS = V.MATCH_STATUS
FROM (
	SELECT
		U.TRIP_ID,
		U.GPS_SEQ::BIGINT AS GPS_SEQ,
		U.MATCH_LAT,
		U.MATCH_LON,
		U.INTERSECT_LEN,
		U.MATCH_LINK_ID,
		U.MATCH_STATUS::SMALLINT AS MATCH_STATUS
	FROM UNNEST(
		$1::TEXT[],
		$2::TEXT[],
		$3::TEXT[],
		$4::TEXT[],
		$5::TEXT[],
		$6::TEXT[],
		$7::TEXT[]
	) AS U(
		TRIP_ID,
		GPS_SEQ,
		MATCH_LAT,
		MATCH_LON,
		INTERSECT_LEN,
		MATCH_LINK_ID,
		MATCH_STATUS
	)
) AS V
WHERE T.TRIP_ID = V.TRIP_ID
  AND T.GPS_SEQ = V.GPS_SEQ
  AND T.MATCH_STATUS = 2;

-- ── 3. 과금 게이트 전량 조회 ─────────────────────────────────────────────
-- [gate_select] BASE_TOLLGATE 전량 — CChargeDataLoader::LoadGates() 가 실행 (2026-08-12 최정우 추가)
-- link_id 없는 행(좌표거리 폴백만 가능한 게이트)은 일단 제외 — CChargeDataLoader.cpp LoadGates() TODO 참고
-- 컬럼 순서(ChargeDataLoader.cpp LoadGates() 와 반드시 일치): 0:TOLLGATE_ID 1:ROAD_ID 2:GATE_DIV 3:LON 4:LAT 5:LINK_ID
[gate_select]
SELECT
	TOLLGATE_ID,
	ROAD_ID,
	GATE_DIV,
	LON,
	LAT,
	LINK_ID
FROM RUC.BASE_TOLLGATE
WHERE USE_YN = 'Y'
ORDER BY TOLLGATE_ID ASC;

-- ── 4. 과금 구역(도로/링크) 전량 조회 ────────────────────────────────────────
-- [zone_select] BASE_ROADLINK 전량 — CChargeDataLoader::LoadZones() 가 실행 (2026-08-12 최정우 추가)
-- 컬럼 순서(ChargeDataLoader.cpp LoadZones() 와 반드시 일치): 0:ROAD_ID 1:ROAD_KIND 2:ROAD_NM
--   3:GEOM_TYPE 4:SPEED_LIMIT_KMH 5:USE_YN 6:LINK_IDS 7:COORDS 8:LENGTH_M
--   9:FIRST_LON 10:FIRST_LAT 11:LAST_LON 12:LAST_LAT
-- LENGTH_M: coords 배열의 연속 정점 사이 하버사인 거리 합산(폴리라인 실거리) — 폐쇄형·구간단속
--   dist_m 산출에 사용(실측 운영 데이터와 거의 일치 확인됨, 2026-08-12 최정우 추가).
--   PostGIS 없어 직접 구현: 정점을 LEAD() 로 다음 정점과 짝지어 각 구간 하버사인 거리 계산 후 SUM.
--   주의: BASE_ROADLINK.coords 는 road_link.coords 와 중첩 구조가 다름 — [[lon,lat],...] 형태로
--   바로 좌표쌍 배열이라 coords->0 로 한 번 더 까면 안 됨(road_link 는 [[[lon,lat],...]] 라 ->0 필요.
--   이 차이를 못 보고 처음에 ->0 을 붙였다가 전부 대척점 거리(20037508m)로 잘못 나온 걸 실측으로 확인·수정함).
-- FIRST/LAST: coords 배열의 첫/마지막 정점 — 구간단속 from_lat/lon·to_lat/lon 에 사용
--   (실측 확인: 게이트 좌표가 아니라 구역 폴리라인 자체의 시작·끝점, 2026-08-12 최정우 추가)
[zone_select]
SELECT
	ROAD_ID,
	ROAD_KIND,
	ROAD_NM,
	GEOM_TYPE,
	SPEED_LIMIT_KMH,
	USE_YN,
	LINK_IDS,
	COORDS,
	(
		SELECT COALESCE(SUM(
			2 * 6378137.0 * asin(least(1.0, sqrt(
				sin(radians((nxt->>1)::float8 - (cur->>1)::float8) / 2) ^ 2 +
				cos(radians((cur->>1)::float8)) * cos(radians((nxt->>1)::float8)) *
				sin(radians((nxt->>0)::float8 - (cur->>0)::float8) / 2) ^ 2
			)))
		), 0)
		FROM (
			SELECT elem AS cur, LEAD(elem) OVER (ORDER BY ord) AS nxt
			FROM jsonb_array_elements(COORDS) WITH ORDINALITY AS t(elem, ord)
		) pairs
		WHERE nxt IS NOT NULL
	) AS LENGTH_M,
	COORDS->0->>0 AS FIRST_LON,
	COORDS->0->>1 AS FIRST_LAT,
	COORDS->-1->>0 AS LAST_LON,
	COORDS->-1->>1 AS LAST_LAT
FROM RUC.BASE_ROADLINK
WHERE USE_YN = 'Y'
ORDER BY ROAD_ID ASC;

-- ── 4-1. 주정차 과태료 최저 기준시간 조회 ─────────────────────────────────
-- [parkfine_select] BASE_PARKING_FINE 최소 FROM_MIN(분) — CChargeDataLoader::LoadParkingFine() 가 실행.
--   주정차 체류시간(STAY_SECONDS)이 이 값(초로 환산) 미만이면 PRIM_CHARGEHAND 에 등록하지 않음
--   (사용자 지시, 2026-08-24 최정우 추가). 테이블이 비어 있으면 NULL → 임계 비활성(항상 등록).
[parkfine_select]
SELECT MIN(FROM_MIN) FROM RUC.BASE_PARKING_FINE;

-- ── 5. 과금 판정 결과 bulk INSERT ─────────────────────────────────────
-- [charge_insert] PRIM_CHARGEHAND — CRawLogWorker::BulkInsertCharges() 가 실행 (2026-08-12 최정우 추가)
-- 6개 CHARGE_TYPE(0~5) 공용 — 폐쇄형은 DIST_M/ENTRY_TOLLGATE_ID/EXIT_TOLLGATE_ID 도 채움,
--   (2026-09-17 최정우 정정 — 종전 "4유형 공용" 표기는 일반도로·면제도로 추가 전 서술)
-- 주정차는 DIST_M(누적거리)/SPEED_KMH(평균속도) 채우고 CHARGE_YN='Y'/CHARGE_STATUS='0' 고정 (2026-08-13 최정우 수정)
-- 2026-08-20 최정우 수정 — FROM_ID/TO_ID/FROM_LAT/FROM_LON/TO_LAT/TO_LON 6개 컬럼 NOT NULL 해제
-- (사용자 지시 — 폐쇄형/구간단속 미완료(AUDIT) 건은 출구 쪽 아는 값이 없는데 억지로 ''/0 을 넣던
-- 것을 진짜 NULL로 저장하도록). RawLogWorker.cpp 는 모르는 값이면 빈 문자열("")을 넘기고, 여기서
-- NULLIF(...,'')로 그 빈 문자열을 NULL로 변환 — FROM_LAT 등 NUMERIC 컬럼은 캐스팅 전에 먼저
-- NULLIF 를 걸어야 함(빈 문자열을 바로 ::NUMERIC 캐스팅하면 에러가 남).
-- 파라미터 순서(RawLogWorker.cpp CHARGE_INSERT_ROW/BulkInsertCharges() 와 반드시 일치):
--   $1=TRIP_ID[] $2=DEVICE_KEY[] $3=TRIP_SEQ[] $4=CHARGE_TYPE[](1=OPEN_ROAD/2=CLOSED_ROAD) $5=CHARGE_UNIT[](0=NODE/1=LINK,실측 확인)
--   $6=LINK_ID[](실측상 항상 빈값) $7=FROM_ID[](모르면 NULL) $8=TO_ID[](모르면 NULL) $9=FROM_LAT[](모르면 NULL) $10=FROM_LON[](모르면 NULL)
--   $11=TO_LAT[](모르면 NULL) $12=TO_LON[](모르면 NULL) $13=ZONE_ID[] $14=ZONE_NAME[] $15=DIST_M[](개방형은 빈값) $16=SPEED_KMH[](계산 불가 시 빈값)
--   $17=SPEED_LIMIT_KMH[] $18=OCCUR_DT[] $19=TRIP_START_DT[] $20=TOLLGATE_ID[](실측상 항상 빈값)
--     ※ OCCUR_DT 기준은 과금유형별로 다르다 — 일반도로(CHARGE_TYPE=0)만 구간 **진출** 시각이고
--       나머지 5유형은 구간 **진입** 시각이다(2026-08-14 사용자 지시, RawLogWorker.cpp 의
--       "혼동해서 통일하지 말 것" 주석 참고). 조회측이 구간 시간범위를 구할 때 일반도로는
--       (OCCUR_DT-STAY_SECONDS ~ OCCUR_DT), 그 외는 (OCCUR_DT ~ OCCUR_DT+STAY_SECONDS) 다.
--       (2026-09-15 최정우 추가 — DB 컬럼 코멘트에도 동일 내용 반영)
--   $21=ENTRY_TOLLGATE_ID[](폐쇄형 전용, 개방형은 빈값) $22=EXIT_TOLLGATE_ID[](폐쇄형 전용) $23=REG_DT[] $24=UPD_DT[]
--   [2026-10-04 최정우 수정] $24=UPD_DT[] 는 6유형 모두 REG_DT 와 같은 값(최종 수정 일시 = INSERT 시점)이다.
--     DB 컬럼 정의(UPD_DT '최종 수정 일시')에 맞췄다. 아래 2026-09-17 정정의 유형별 차이는 그 이전 상태다.
--   [2026-09-17 최정우 정정] 종전 "$24=UPD_DT[](REG_DT와 항상 동일)" 는 사실이 아니다 —
--   게이트형(개방·폐쇄·구간단속)·주정차는 REG_DT 와 같은 값이지만, 일반도로·면제도로
--   (BuildNodeStepRow/BuildNodeStepRowFromLinkRange/BuildExemptRow)는 OCCUR_DT(진출 시각)를
--   넣는다. 트립 정상종료 시 [trip_end] 가 다시 GPS 종료시각으로 덮는다. 이 컬럼은 어떤
--   조건절에도 쓰이지 않아 과금 판정에는 영향이 없다(판정을 가르는 것은 TRIP_END_DT 다)
--   $25=CHARGE_YN[](빈값=DB기본 Y, 이상 시 "N" — 폐쇄형·구간단속 게이트 이상, 개방형 트립시작 run
--     게이트 미통과, TTL flush 등)
--   $26=CHARGE_STATUS[](빈값=DB기본 0. 이상 시 "3"=AUDIT(심사대상)이 기본이고, 면제도로(CHARGE_TYPE=5)
--     의 TTL 만료만 "4"=SKIP — 과금 대상이 아니라 심사 큐에 올리지 않는다) (2026-08-30 최정우 수정)
--   $27=STAY_SECONDS[](주정차 전용 — 체류시간 초. 컬럼 코멘트: "체류 시간(초). 주정차 위반 판단". 다른 3종은 빈값=DB기본 0) (2026-08-13 최정우 추가)
--   $28=TRIP_END_DT[](주정차 TTL 만료 강제마감 전용 — 더 이상 GPS 수신 불가로 판단한 시각. 그 외는
--     빈값=NULL 유지, 실제 TRIP_EVENT=2 수신 시 [trip_end] UPDATE 가 별도로 채움) (2026-08-13 최정우 추가)
--   $29=START_GPS_SEQ[] $30=END_GPS_SEQ[](진입/구역 안에서 실제로 마지막 확인된 PRIM_RAWGPS.GPS_SEQ —
--     웹뷰어 G순번과 동일 개념. 빈값=DB기본 0) (2026-08-28 최정우 추가)
--   $31=NON_CHARGE_REASON[](DataDefine.h 의 NCR_* 코드. 빈값=0=NCR_NORMAL(정상 과금) — 2026-09-15
--     최정우 수정, 사용자 지시: 종전엔 빈값을 NULL 로 넣어 Y/0 행이 전부 NULL 이었으나, 코드표에
--     0(정상 과금)이 이미 정의돼 있는데 DB 에만 안 들어가 일관성이 없었다. 이제 항상 채워지는
--     상태코드로 취급한다 — 조회측이 NULL 분기를 둘 필요가 없다)
-- ON CONFLICT: 배치 release 후 재시도되는 케이스의 중복 INSERT 방어(정상 흐름에서는 trip_seq 가 매번 새 값)
[charge_insert]
INSERT INTO RUC.PRIM_CHARGEHAND (
	TRIP_ID, DEVICE_KEY, TRIP_SEQ, CHARGE_TYPE, CHARGE_UNIT, LINK_ID, FROM_ID, TO_ID,
	FROM_LAT, FROM_LON, TO_LAT, TO_LON, ZONE_ID, ZONE_NAME, DIST_M, SPEED_KMH, SPEED_LIMIT_KMH,
	OCCUR_DT, TRIP_START_DT, TOLLGATE_ID, ENTRY_TOLLGATE_ID, EXIT_TOLLGATE_ID, REG_DT, UPD_DT,
	CHARGE_YN, CHARGE_STATUS, STAY_SECONDS, TRIP_END_DT, START_GPS_SEQ, END_GPS_SEQ, NON_CHARGE_REASON
)
SELECT
	U.TRIP_ID,
	U.DEVICE_KEY,
	U.TRIP_SEQ::INTEGER,
	U.CHARGE_TYPE::SMALLINT,
	U.CHARGE_UNIT::SMALLINT,
	NULLIF(U.LINK_ID, ''),
	NULLIF(U.FROM_ID, ''),
	NULLIF(U.TO_ID, ''),
	NULLIF(U.FROM_LAT, '')::NUMERIC,
	NULLIF(U.FROM_LON, '')::NUMERIC,
	NULLIF(U.TO_LAT, '')::NUMERIC,
	NULLIF(U.TO_LON, '')::NUMERIC,
	NULLIF(U.ZONE_ID, ''),
	NULLIF(U.ZONE_NAME, ''),
	CASE WHEN U.DIST_M <> '' THEN U.DIST_M::INTEGER ELSE 0 END,
	CASE WHEN U.SPEED_KMH <> '' THEN U.SPEED_KMH::SMALLINT ELSE 0 END,
	CASE WHEN U.SPEED_LIMIT_KMH <> '' THEN U.SPEED_LIMIT_KMH::SMALLINT ELSE 0 END,
	U.OCCUR_DT,
	U.TRIP_START_DT,
	NULLIF(U.TOLLGATE_ID, ''),
	NULLIF(U.ENTRY_TOLLGATE_ID, ''),
	NULLIF(U.EXIT_TOLLGATE_ID, ''),
	U.REG_DT,
	U.UPD_DT,
	CASE WHEN U.CHARGE_YN <> '' THEN U.CHARGE_YN ELSE 'Y' END,
	CASE WHEN U.CHARGE_STATUS <> '' THEN U.CHARGE_STATUS::SMALLINT ELSE 0 END,
	CASE WHEN U.STAY_SECONDS <> '' THEN U.STAY_SECONDS::INTEGER ELSE 0 END,
	NULLIF(U.TRIP_END_DT, ''),
	CASE WHEN U.START_GPS_SEQ <> '' THEN U.START_GPS_SEQ::BIGINT ELSE 0 END,
	CASE WHEN U.END_GPS_SEQ <> '' THEN U.END_GPS_SEQ::BIGINT ELSE 0 END,
	CASE WHEN U.NON_CHARGE_REASON <> '' THEN U.NON_CHARGE_REASON::SMALLINT ELSE 0 END
FROM UNNEST(
	$1::TEXT[], $2::TEXT[], $3::TEXT[], $4::TEXT[], $5::TEXT[], $6::TEXT[], $7::TEXT[],
	$8::TEXT[], $9::TEXT[], $10::TEXT[], $11::TEXT[], $12::TEXT[], $13::TEXT[], $14::TEXT[],
	$15::TEXT[], $16::TEXT[], $17::TEXT[], $18::TEXT[], $19::TEXT[], $20::TEXT[], $21::TEXT[],
	$22::TEXT[], $23::TEXT[], $24::TEXT[], $25::TEXT[], $26::TEXT[], $27::TEXT[], $28::TEXT[],
	$29::TEXT[], $30::TEXT[], $31::TEXT[]
) AS U(
	TRIP_ID, DEVICE_KEY, TRIP_SEQ, CHARGE_TYPE, CHARGE_UNIT, LINK_ID, FROM_ID, TO_ID,
	FROM_LAT, FROM_LON, TO_LAT, TO_LON, ZONE_ID, ZONE_NAME, DIST_M, SPEED_KMH, SPEED_LIMIT_KMH,
	OCCUR_DT, TRIP_START_DT, TOLLGATE_ID, ENTRY_TOLLGATE_ID, EXIT_TOLLGATE_ID, REG_DT, UPD_DT,
	CHARGE_YN, CHARGE_STATUS, STAY_SECONDS, TRIP_END_DT, START_GPS_SEQ, END_GPS_SEQ, NON_CHARGE_REASON
)
ON CONFLICT (trip_id, device_key, trip_seq) DO NOTHING;

-- ── 6. 트립 종료 시 trip_end_dt 반영 ───────────────────────────────────────
-- [trip_end] TRIP_EVENT=2 감지 시 CRawLogWorker::UpdateTripEndDt() 가 실행 (2026-08-12 최정우 추가)
-- 과금 INSERT 는 게이트 통과 즉시(트립 종료 전) 발생하므로 trip_end_dt 는 그 시점에 알 수
-- 없음 — 트립이 실제로 끝날 때 해당 trip_id 의 PRIM_CHARGEHAND 전 행에 반영.
-- 2026-08-20 최정우 수정 — 같은 trip_id 에 END 신호가 두 번 이상 와서 $1~$3 배열에 같은
-- TRIP_ID 가 여러 번 들어오면(원본 GPS에 END 이벤트가 중복·순서뒤섞임으로 두 번 온 실측 케이스로
-- 확인됨), 원래 쿼리는 UPDATE...FROM 에 같은 T 행에 매칭되는 V 행이 여러 개일 때 어느 게
-- 반영될지 명세상 정해져 있지 않아(PostgreSQL 공식 문서 — "unspecified which one is used"),
-- 배치 타이밍에 따라 더 이른 END 값이 남는 경우가 실측됨(나중 값이 이긴다는 보장이 없었음).
-- 아래 서브쿼리로 (1) 배열 안에서부터 trip_id 별 가장 늦은 TRIP_END_DT 만 골라 쓰고(DISTINCT ON),
-- (2) 이미 저장된 TRIP_END_DT 보다 늦을 때만 반영하도록(WHERE) 이중으로 보장 — 배치가 나뉘어
-- 여러 번 호출돼도, 어떤 순서로 오든 항상 "가장 늦은 종료 시각"만 최종 반영됨.
-- [버그 수정, 2026-09-17 최정우] 아래 설명 블록은 원래 이 SQL 의 SET 절 **안에** 있었다.
-- query.sql 파서(CSQLAccessor::Initialize)는 각 줄의 개행을 지우고 공백 하나로 이어붙이는데
-- '--' 주석을 제거하지 않는다 — 그래서 SQL 본문 안의 '--' 뒤가 전부 한 줄 주석으로 먹혀
-- 쿼리가 "UPDATE ... SET" 에서 잘렸고, 실행할 때마다 "구문 오류, 입력 끝부분" 으로 실패했다
-- (실측 000385_20260917140801: TRIP_END_DT 가 채워지지 않음. 커밋 ca547c1 에서 유입).
-- **SQL 본문에는 '--' 주석을 절대 넣지 말 것** — 설명은 이 자리(섹션 헤더 위)에 둔다.
-- [보강, 2026-09-17 최정우, 사용자 지시] 빈 문자열 방어.
--   TRIP_END_DT 는 NULLIF 로 NULL 이 되게 한다 — 이 컬럼은 nullable 이고, 무엇보다
--   [trip_abend] 의 판정 조건이 `TRIP_END_DT IS NULL` 이다. 빈 문자열('')은 NULL 이
--   아니므로 그대로 저장되면 그 행이 [trip_abend] 대상에서 **영구히 빠져**,
--   확정 못 한 채 끝난 트립이 N/3(심사대상)으로 강등되지 않고 Y/0(정상 과금)으로 남는다.
--   UPD_DT 는 NOT NULL 이라 NULL 을 못 넣으므로 값을 메워야 한다. 이때 **TRIP_END_DT 를
--   먼저 쓴다** — 이 UPDATE 에 넘어오는 두 값은 C++ 쪽에서 모두 같은 GPS 시각
--   (FormatDateTime14(stRawLogInfo.dtGPS))으로 만들어지므로(RawLogWorker.cpp
--   ProcessRawLog 트립종료 블록), 같은 값으로 맞추는 것이 원래 의미에 부합한다.
--   둘 다 비어 있을 때만 최후 수단으로 현재 시각을 쓴다.
-- [2026-10-04 최정우 수정 — 사용자 위임 결정] UPD_DT 는 TRIP_END_DT(GPS 시각)로 폴백하지 않고 벽시계로
--   메운다 — UPD_DT 는 '최종 수정 일시'(DB 컬럼 정의)이고, 트립 종료 시각은 TRIP_END_DT 에 이미 있다.
--   GPS 시각(과거)을 넣으면 수정 시각이 등록 시각(REG_DT)보다 앞서는 모순이 생기고, 수정분을 UPD_DT 로
--   찾는 쪽이 이 갱신을 놓친다. C++ 도 이제 벽시계를 넘긴다(RawLogWorker.cpp 트립종료 블록).
--   이 UPDATE 는 2026-10-04 부터 과금 INSERT 와 같은 배치 트랜잭션 안에서 실행된다.
-- $1=TRIP_ID[] $2=TRIP_END_DT[] $3=UPD_DT[]
[trip_end]
UPDATE RUC.PRIM_CHARGEHAND AS T
SET
	TRIP_END_DT = NULLIF(V.TRIP_END_DT, ''),
	UPD_DT = COALESCE(NULLIF(V.UPD_DT, ''), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'))
FROM (
	SELECT DISTINCT ON (TRIP_ID) TRIP_ID, TRIP_END_DT, UPD_DT
	FROM UNNEST($1::TEXT[], $2::TEXT[], $3::TEXT[]) AS U(TRIP_ID, TRIP_END_DT, UPD_DT)
	ORDER BY TRIP_ID, TRIP_END_DT DESC
) AS V
WHERE T.TRIP_ID = V.TRIP_ID
	AND (T.TRIP_END_DT IS NULL OR V.TRIP_END_DT > T.TRIP_END_DT);

-- ── 7. 트립의 기존 최대 TRIP_SEQ 조회 (읽기 전용) ─────────────────────────────────────
-- [trip_seqmax] CRawLogWorker::QueryTripSeqMax() 가 run() 배치 시작 때 실행(2026-10-04 최정우 추가, 사용자 확정).
--   엔진 재기동·TTL 마감 뒤 같은 trip_id 가 이어지면 새 세션이 TRIP_SEQ 를 1 부터 다시 매겨 기존 행과 PK 가
--   충돌, 새 행이 ON CONFLICT DO NOTHING 으로 조용히 버려졌다(재현 980015 seq26~35, 212m). 기존 최댓값을 읽어
--   다음 번호부터 잇게 한다. **TRIP_SEQ 를 바꾸는 SQL 이 아니다** — 번호 부여는 엔진만 한다.
-- [2026-10-04 같은 날 삭제] 종전 7번 섹션 [trip_abend](TTL 마감 시 TRIP_END_DT 빈 행을 N/3(61) 로 강등)는
--   이미 정상 확정된 행까지 강등·사유 덮어쓰기를 해서 삭제했다. TTL 마감은 [trip_end] 와 같은 UPDATE 를 쓴다.
-- $1=TRIP_ID $2=DEVICE_KEY
[trip_seqmax]
SELECT COALESCE(MAX(TRIP_SEQ), 0)
FROM RUC.PRIM_CHARGEHAND
WHERE TRIP_ID = $1 AND DEVICE_KEY = $2;

-- ── 8. (삭제) 트립 종료 시 TRIP_SEQ 재부여 [trip_seqoff]/[trip_seqfin] ─────────────
-- [2026-10-04 최정우 삭제 — 사용자 지시] TRIP_SEQ 는 SQL 로 처리하지 않는다. 엔진(RawLogWorker 워터마크
--   큐, ReleaseChargeQueue)이 확정할 때 번호를 붙여 INSERT 하며, 그 뒤로는 어떤 SQL 도 TRIP_SEQ 를
--   바꾸지 않는다(PK 일부 — 등록 후 변경 금지). 2026-09-22 부터 config 빈 값으로 비활성이던 섹션이다.

-- [server_status] CServer::UpdateServerStatus() 가 [server] status_interval(기본
--   600초=10분) 주기로 실행 — 이 서버(SERVER_ID=[server] id, 기본 "location") 1행만 갱신.
--   $1=SERVER_ID, $2=CPU_PCT, $3=MEM_PCT, $4=MEM_USED_MB, $5=MEM_TOTAL_MB
--   (2026-08-20 최정우 추가)
[server_status]
UPDATE RUC.PROC_SERVERSTATUS
SET
	CPU_PCT = $2::NUMERIC,
	MEM_PCT = $3::NUMERIC,
	MEM_USED_MB = $4::INTEGER,
	MEM_TOTAL_MB = $5::INTEGER,
	HB_DT = NOW(),
	UPD_DT = NOW()
WHERE SERVER_ID = $1;

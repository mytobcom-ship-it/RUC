★ 과금 등록 로직 정본(최신): doc/과금_등록_로직_정본.md  (2026-10-04~, git 추적)
  - 과금 유형별 대상·판정·과금/비과금 등록 여부, 주정차 폴리곤 안 일반도로 등록, 변경 이력·검증 절차
  - [2026-10-04 정정] doc/*.md 는 모두 git 추적된다(.gitignore 의 !*.md). 종전 "이 파일만 예외" 안내는 낡았다.
★ 주정차 단속구역 내 일반도로 등록 작업 문서: doc/주정차구역_일반도로_등록_로직.md  (2026-10-04~)
  - 현재 로직(기존 동일/변경), 결정 기록, 검증 절차·기준값, 미결 항목. 로직 변경 시 같은 커밋에서 갱신
  - 한글 파일명이 \352\263\274... 처럼 보이면: git config core.quotepath false

도로이용 과금(개방형/폐쇄형/구간단속/주정차) 설계 논의 — 이슈 및 처리방향 정리
상세 흐름도/mermaid는 web/docs/charge-flow.html 참조. 이 문서는 논의 중 나온 이슈와 결론만 압축 기재.
※ 원격 DB 접속정보(host/dbname/user/pw)는 보안상 이 파일(git 추적됨)엔 기재 안 함 — Claude 메모리에 별도 기록.

A. [중요] 기존 ruc DB에 이미 완성된 마스터 스키마 존재 (2026-08-04 확인)
개방형/폐쇄형/구간단속/주정차용으로 새 테이블(PRIM_GATE_INFO 등)을 설계했었으나, 실제 운영 DB(스키마 ruc, roadnet 스키마와는 별개)에
이미 훨씬 성숙한 버전이 구축돼 있음을 확인함. 아래 B~H절 중 "기반정보 저장 위치"·"세션 필드" 관련 설계 방향(캐시 전략 등)은
개념적으로 유효하지만, 실제 테이블명/컬럼명은 전부 이 기존 스키마를 따라야 함 — roadnet/sql/prim_charge_create.sql은 폐기 대상.

공통코드 ZONEKIND (base_codesc, codelc='ZONEKIND') — 우리가 논의해온 유형 분류가 이미 코드화돼 있음:
  01 일반도로(거리기반과금) — 데이터 있음 1,525,177건
  02 유료도로(개방식)       — 데이터 있음 31,383건
  03 유료도로(폐쇄식)       — 코드만 정의, 데이터 0건 ← 테스트 대상
  04 과속·구간단속          — 코드만 정의, 데이터 0건 ← 테스트 대상
  05 주정차단속             — 코드만 정의, 데이터 0건 ← 테스트 대상
※ "임의 지역에 테스트 구역 설정"은 결국 03/04/05에 해당하는 데이터를 base_mapinfo(04·05) / base_road+base_tollgate+base_tollsection(03)에
  새로 등록하는 작업임. 새 테이블 설계는 불필요.

실제 마스터/결과 테이블 구조:
  base_road          도로 마스터. toll_type CHECK(O=개방식,C=폐쇄식,N=무료) — 285건. FK 피대상: base_tollgate.road_id
  base_roadlink       도로-형상 연결 (road_id, coords jsonb)
  base_tollgate       게이트. gate_div CHECK(M=본선/개방식 게이트, I=진입, O=진출, B=미사용) — 3건. road_id FK
  base_tollfare       게이트별 CARTYPE(1~6종)×fare_date(요금개정 이력) 정액요금 — 개방식(M) 게이트용
  base_tollsection    (entry_tollgate_id, exit_tollgate_id, car_type, fare_date)가 PK — 폐쇄식 구간요금.
                       ※ 우리가 만들려 했던 GATE_PAIR_ID 불필요 — (진입ID,진출ID) 조합 자체가 짝짓기 키
  base_mapinfo        ZONE 마스터 (zone_id PK, zone_kind, f_node/t_node/road_id/link_id, speed_limit_kmh,
                       geom_type+coords jsonb, color(지도표시용), min/max lon/lat(bbox 인덱스))
  road_link           표준노드링크 슬림 버전 (PRIM_LINK_INFO와 동급, link_id PK, f_node/t_node, coords jsonb)
  base_codelc/codesc  공통코드 (ZONEKIND, CARTYPE, USEYN 등)
  prim_rawgps         원시 GPS (기존에 알던 것과 동일 성격)
  prim_chargehand     원시 감지 로그 — 3유형 공통 1테이블. from_id/to_id/zone_id/dist_m/speed_kmh/speed_limit_kmh/
                       stay_seconds 컬럼을 유형 불문 다 가짐(=charge_type/charge_unit으로 분기). PK(trip_id,device_key,trip_seq)
  prim_tollcharge     TOLL 확정청구. vehicle_type/fare_amount/vat_amount/final_amount/app_send_yn(앱 발송상태)
  prim_vltncharge     SPEED·PARKING 위반 확정. violation_type/speed_limit_kmh/measured_speed_kmh/stay_minutes/fine_amount
  prim_tollpasshist / prim_roadpasshist   통과 이력
  proc_tripaudit / proc_tripaudithist     미종결(AUDIT) 처리
  proc_drivestatus    운행상태
  proc_dayfin*         일별 정산/수정/납부 집계

기존 설계 대비 이미 반영된 실무 요소(우리가 처음 설계에서 놓쳤던 것): 차종별 차등요금(CARTYPE), 요금개정 이력(fare_date),
앱 발송상태(app_send_yn), 공통코드 테이블 패턴(신규 유형 추가 시 코드 INSERT만 하면 됨).
geom 저장 방식은 PostGIS가 아니라 coords(jsonb) — 폴리곤 판정(포함여부 등)을 어떻게 하는지는 애플리케이션단 확인 필요(미확인).

B. 과금 유형 개념 정리 (여전히 유효 — 용어만 ZONEKIND 코드로 치환해서 이해)
점 이벤트: 개방형(ZONEKIND=02) — 게이트 통과 즉시 정액요금(base_tollfare) 확정. 진입/진출 짝짓기·세션 보류 불필요.
구간 짝 이벤트: 폐쇄형(ZONEKIND=03, base_tollsection) · 구간단속(ZONEKIND=04) — 메커니즘 동일("진입 저장 → 진출에서 계산"),
  목적만 다름(거리×단가=통행료 vs 거리÷시간=평균속도 위반판정).
구역 체류 이벤트: 주정차(ZONEKIND=05) — 정차 시작-재개 짝, 판정기준은 거리가 아니라 체류시간.
※ 최초엔 구간단속을 "개방형"으로 오해했으나, 진입-진출 짝이 필요한 메커니즘이라 폐쇄형과 동일 상태머신으로 재분류함.

C. 개방형(ZONEKIND=02) 재정의 관련 — 결론: 정액요금이 맞음
"개방형도 이용거리만큼 과금해야 하는 것 아니냐"는 의문 제기됨 → 웹 검색으로 확인.
국내 개방식 요금소는 "요금소별 사전 산정된 최단이용거리 기준 고정 금액"을 부과 — 개별 차량의 실측 누적거리를 실시간 계산하지 않음.
실측거리 비례는 폐쇄식의 역할. base_tollfare(게이트별 정액) vs base_tollsection(구간별 거리요금) 구조가 이 결론과 정확히 일치.
스마트톨링(2024~ 대왕판교/남해선 시범, 2026 확대)은 요금 "산정방식"이 아니라 "수납방식"(무정차, 번호판인식 후불) 혁신일 뿐,
개방식/폐쇄식 산정 로직 자체는 안 바뀜.
※ 참고: ZONEKIND=01(일반도로, 거리기반과금)이 데이터 152만건으로 이미 존재 — 이건 톨게이트와 무관한 "일반도로 자체에 대한
  거리기반 도로이용료" 개념으로 보임(프로젝트명 RUC=Road User Charge 추정). 개방형/폐쇄형 논의와는 별도 트랙. 상세 미확인 — 필요시 추가 조사.

D. 혼합식(HYBRID) 게이트 — 미해결 이슈로 남김
일부 고속도로 IC는 진입 통과 즉시 소액 정액 과금 + 이후 최종 진출까지 거리요금 추가 정산을 동시에 수행 —
"게이트 1개=유형 1개" 가정이 깨지는 케이스. base_tollgate.gate_div가 지금 M/I/O 중 하나만 갖는 구조라 이 케이스를 못 담음.
반영하려면 한 도로(road_id)에 대해 같은 지점이 gate_div=M 행과 I 행을 동시에 가지도록 허용하거나 별도 설계 필요. 재작업 시 검토.

E. 명명(RawLogWorker.h C++ 구조체 한정 — DB 컬럼명은 기존 스키마 그대로 사용, 건드리지 않음)
"구간 짝짓기 키"를 C++ 세션 필드로 새로 만들 때 SectionID라고 이름 붙이면 기존 VEHICLE_TRIP_SESSION의 "세션"(TTL 트립 세션)
개념과 스펠링/발음이 헷갈림 → GatePairID(또는 GateGroupID) 식으로 명명할 것. DB 쪽은 base_tollsection이 (entry_id,exit_id) 조합을
그대로 키로 쓰므로 이 이슈 자체가 없음 — C++ 세션 구조체에서 그 조합을 임시로 들고 있을 때만 해당.
기존 규칙 재확인: 미터 단위 접미사(_M) 남용 금지, config.ini 파라미터는 최대 밑줄 1개+짧은 접두사(별도 memory 참조).

F. 기반정보 조회 전략 — link.psf 바이너리 vs DB 직접조회, 결론은 유효 (대상 테이블만 교체)
결론: DB 테이블(base_tollgate/base_tollsection/base_tollfare/base_mapinfo) 직접 SELECT가 아니라 인메모리 캐시 경유 권장. psf 통합은 기각.

기각 사유(psf 방식):
 - DATA_FILE_HEAD(CreateData/src/DataFormat.h:121)에 reserved/버전 필드가 전혀 없음 — 섹션 추가 시 헤더 포맷 변경, 전량 재생성 필요.
 - 게이트는 고정크기 레코드로 가능하나, base_mapinfo의 폴리곤형 구역(주정차 등)은 가변길이 설계가 추가로 필요해 복잡도 증가.
 - 테스트 중 구역을 자주 바꿀 목적과 안 맞음 — 매번 CreateData 재실행 + MapMatchSvr 재기동 필요.

채택 사유(DB+캐시 방식):
 - DB는 PostgreSQL(MVCC) — 순수 SELECT는 다른 SELECT/INSERT/UPDATE를 블로킹하지 않음. 우려했던 "Lock 경합"은 근본적으로 낮음.
 - 진짜 리스크는 락이 아니라 PostgrePool의 작은 커넥션 풀(PostgrePool.cpp: 기본 min 3/max 5). getConnection()(PostgrePool.cpp:162)은
   min→max까지는 on-demand 자동 증가하지만 max 초과 시 타임아웃 없이 블로킹 대기, keepPoolAlive()(PostgrePool.cpp:264)가 주기적으로
   다시 min까지 축소 — 매 GPS마다 SELECT를 태우면 기존 배치 INSERT/UPDATE와 커넥션 경쟁으로 처리량 병목.
 - 해법: link.psf의 m_mapLinkInfoList(DataLoader.h:73) 패턴과 동일하게, base_tollgate/base_tollsection/base_tollfare/base_mapinfo
   전체를 프로세스 시작 시 한 번에 SELECT → 인메모리 해시맵 캐시. 조회는 항상 인메모리(DB 왕복 0회). 규모상 부담 없음
   (전국 링크 수십만~백만 건 대비 게이트 3건·구역 03/04/05는 현재 0건 — 앞으로 늘어도 훨씬 작은 규모).
 - 레코드 추가/삭제/수정 시 재기동 불필요하게 하려면: 별도 백그라운드 스레드가 N초(예 30~60초)마다 테이블 재조회 → 새 맵 완성 후
   뮤텍스로 짧게 잠그고 포인터만 교체(old 맵 폐기). PostgrePool.cpp:264-297 keepPoolAlive()와 동일한 기존 패턴 재사용.
 - 참고: 세션맵(m_vtTripSessions, RawLogWorker.h:168)은 스레드별 소유권 분리로 락 자체를 안 씀(RawLogWorker.cpp:140 주석) —
   게이트 캐시도 같은 철학(핫패스에서 락/DB 왕복 최소화) 계승.
 - base_mapinfo.coords가 PostGIS GEOMETRY가 아니라 jsonb임 — 인메모리 캐시로 올릴 때 "포함 여부" 판정 로직(ST_Contains 대체)을
   애플리케이션 코드에서 직접 구현해야 할 가능성 있음. 기존에 이 판정을 어디서 하는지 미확인 — 재작업 시 우선 확인.

G. VEHICLE_TRIP_SESSION 확장 필드 (안) — RawLogWorker.h, C++ 세션 구조체. DB 컬럼명과 무관, 판정 중간상태 보관용
개방형: 해당 없음(점 이벤트, 세션 보류 불필요).
폐쇄형·구간단속(공통 상태머신): 진입중여부, 진입게이트ID, 진입~진출 짝 임시보관용 GatePairID(E절 명명 참고),
  목적(TOLL/SPEED), 진입시각, 누적거리.
주정차: 정차중여부, 정차시작시각, 정차시작좌표.

H. 재작업 시 TODO (우선순위 순, 2026-08-04 갱신 — 새 테이블 설계 불필요로 전면 수정)
1. [부분 확인, I절 참고] base_mapinfo.coords(jsonb) 포함여부 판정 로직이 기존에 있는지 확인 (F절 마지막 항목).
   geom_type='LINE'(구간단속용)은 실데이터로 형식 확인됨(I-4절). geom_type='POLY'(주정차용)는 실사용 사례 0건 —
   point-in-polygon 판정 로직은 여전히 신규 구현 필요.
2. ZONEKIND=03(폐쇄식)/04(구간단속)/05(주정차) 임의 지역 테스트 데이터 등록
   - 03: base_road(toll_type=C) 1건 + base_tollgate(gate_div=I/O 짝) 2건 + base_tollsection(entry/exit/car_type/fare_date) 등록
   - 04/05: base_mapinfo(zone_kind=04 또는 05, f_node/t_node 또는 link_id, speed_limit_kmh 또는 체류허용시간 상당 컬럼 확인) 등록
     ※ base_mapinfo에 주정차 유예시간 컬럼은 여전히 안 보임(범칙금은 I-3절 base_fare로 확인됨) — "유예시간(10분)" 자체는
       하드코딩 여부 결정 필요. ※ zone_kind 실제 저장은 '04'/'05'가 아니라 '4'/'5'(앞자리0 없음) 가능성 큼 — 등록 전 확인(I-4절).
3. VEHICLE_TRIP_SESSION(RawLogWorker.h) 확장 필드 추가 (G절).
4. 게이트/구역 인메모리 캐시 클래스 + 백그라운드 재조회 스레드 구현 (F절, PostgrePool.keepPoolAlive 패턴 참고).
   base_fare/base_carinfo(I절)도 이 캐시 대상에 포함 필요 — 특히 base_carinfo는 device_key 단위 룩업이라 해시맵 적합.
5. RawLogWorker.cpp에 판정 흐름(charge-flow.html 참고, 필요 시 base_* 실제 컬럼명에 맞게 갱신) 구현,
   strChargeInsertSQL(RawLogWorker.h:92) 스텁 해소 — 대상은 prim_chargehand(원시) → prim_tollcharge/prim_vltncharge(확정) 2단계.
6. D절 혼합식(HYBRID) 게이트 케이스 반영 여부 결정.
7. [해결됨, I-2절 참고] C절 ZONEKIND=01(일반도로 거리기반과금, 152만건 기존 데이터)의 실제 용도/판정로직 조사.
   → 별도 트랙 아님. charge_type=0(NODE_STEP)으로 같은 파이프라인에 속하며 base_fare.new_fare를 참조해
     prim_tollcharge로 확정됨을 기존 데이터로 확인.
8. [신규, I-3절] SPEED·PARKING 범칙금 산정 기준을 base_fare로 확정할지 결정 — prim_vltncharge 기존 데이터에
   base_fare 구간표와 다른 고액(3만~9만원대) 패턴도 섞여있어 과금서버 담당자 확인 필요.
9. [신규, I-7절, 최우선] 개방형(OPEN) 모델 확정 — "게이트 통과 즉시 정액"(1절 설계 가정) vs "링크 hop마다
   누적거리"(prim_chargehand 293건 실측 패턴) 중 무엇이 맞는지 담당자 확인 없이는 1절 코드 착수 불가.
   prim_chargehand·prim_tollcharge 간 ID 정합성도 안 맞아 기존 데이터를 참고 삼아 넘겨짚으면 안 됨.
10. [신규, I-8절] 폐쇄형(CLOSED_ROAD) 진입정보 durable 기록 설계 확정 — DB 테이블 구조(prim_chargehand
    선기록 vs 별도 경량 테이블), CLOSED_ROAD 전용 장기 TTL 값, 배치 스윕 주기·임계값 결정. 수집앱이
    시동 ON/OFF를 TRIP_EVENT END/START로 처리하는지도 확인 필요(3절과 연동).

폐기됨: roadnet/sql/prim_charge_create.sql (PRIM_GATE_INFO/PRIM_PARKING_ZONE/PRIM_CHARGEHAND) — 기존 ruc 스키마와 중복 설계.
참고용으로만 남겨두고 실제 작업 대상에서 제외.

I. [2026-08-06] 운영 DB(59.11.91.162/ruc) 실데이터 직접 조회로 확인·발견한 사항
2026-08-04 최초 조사는 pg_description(코멘트) 스캔 위주였음. 오늘은 실제 로우 데이터까지 조회해 아래를 확인·발견함
(상세 흐름도는 web/docs/charge-flow.html 5절, 스키마는 web/docs/charge-tables.html base_fare/base_carinfo 절 참고).

I-1. car_type(차종) 출처 확인됨
  prim_chargehand.device_key = base_carinfo.device_key(UNIQUE) 로 1:1 조인 → base_carinfo.car_type(1~6) 획득.
  주의: tb_vhcl_info(회원 앱쪽 차량정보, PK=mbr_id+vhcl_sn, carmdl_type 컬럼)는 별개 코드체계 — 절대 혼용 금지.

I-2. TOLL 계열(OPEN·CLOSED_ROAD·NODE_STEP) 금액 공식 확인됨
  vat_amount = ROUND(fare_amount × 0.1), final_amount = fare_amount + vat_amount
  — 기존 prim_tollcharge 361건 중 352건(97.5%) 일치. 단 75건은 vat_amount=0 예외로 남아있어 100% 확정 규칙은 아님.
  charge_type=0(NODE_STEP, ZONEKIND=01 일반도로)도 이 파이프라인을 타며 base_fare.new_fare를 요금원으로 사용하는
  것으로 보임(H-7 해결).

I-3. SPEED·PARKING 범칙금 기준 테이블 base_fare 신규 발견 — 단, 확정은 아님
  base_fare(PK=car_type, 6건): new_speeding_fine_10km_down/20km_down/30km_down/30km_up(초과속도 4단계),
  new_parking_fine_10min_down/up(체류시간 2단계), new_fare(일반요금). base_farehist는 개정 이력.
  ⚠ prim_vltncharge 기존 111건 중 base_fare 구간표와 일치하는 소액(500~3,200원대) 패턴과, 이 표 어디에도 없는
  고액(30,000~90,000원대, 실제 도로교통법 과속 범칙금 규모와 유사) 패턴이 동일 테이블에 섞여 있고 등록일시로도
  분리 안 됨 — base_fare가 진짜 유일한 산정 기준인지 과금서버 담당자에게 재확인 필요(H-8).

I-4. base_mapinfo 관련 확인
  - 실제 zone_kind 저장값은 '01'~'05'가 아니라 앞자리0 없는 '1'~'5'(1글자) — 컬럼 코멘트와 다름. 04/05 등록 시 주의.
  - coords(jsonb)는 이미 LineString류([[lon,lat],...])/Polygon류(닫힌 링) 좌표 배열을 담는 용도로 설계돼 있어
    구간단속용 신규 컬럼 불필요. 단, geom_type='POLY'(주정차 폴리곤) 실사용 사례가 현재 0건 — 검증 안 된 경로.
  - base_mapinfo(zone_kind=01/02, 152만+3.1만 건)는 전국 도로를 링크 단위로 잘게 쪼갠 지도 표출용 데이터로 보임
    (예: "경부고속도로"/"강변북로" 등 노선명 그대로). base_tollgate.link_id가 전부 NULL이라 base_tollgate와
    실제로 연결되지 않음 — 개방형·폐쇄형 판정에는 base_mapinfo를 쓰지 않고 base_tollgate 계열만 사용.
  - 결론: base_mapinfo는 구간단속(04)·주정차(05) 판정 전용 기반 테이블. 개방형·폐쇄형은 base_tollgate 계열로 완전히 분리됨.

I-5. 유형별 필요 테이블 요약
  개방형(OPEN):       base_tollgate(M) + base_tollfare           → prim_chargehand(1) → prim_tollcharge
  폐쇄형(CLOSED_ROAD): base_tollgate(I/O) + base_tollsection      → prim_chargehand(2) → prim_tollcharge
  구간단속(SPEED):     base_mapinfo(zone_kind=04) + base_fare     → prim_chargehand(3) → prim_vltncharge(0)
  주정차(PARKING):     base_mapinfo(zone_kind=05) + base_fare     → prim_chargehand(4) → prim_vltncharge(1)
  공통: base_carinfo(device_key→car_type)는 네 유형 모두 요금/범칙금 산정에 필요(I-1).

I-6. 최초 조사(2026-08-04) 누락분
  base_fare, base_carinfo 2개 테이블이 당시 pg_description 스캔에서 빠져 있었음(2026-08-06 실데이터 조회로 발견).
  스키마 문서화 시 코멘트 스캔만으로는 부족 — 실데이터 조회를 병행할 것(교훈).

I-7. [정정 + 신규 미해결] 개방형(OPEN) 모델 자체가 실측과 안 맞음
  I-2에서 "prim_chargehand OPEN 행이 from_id=to_id=게이트ID 패턴으로 실측 확인됨"이라 적었는데, 이는 소수 샘플만
  보고 성급히 일반화한 오류로 정정함. prim_chargehand(charge_type=1) 293건을 전수 확인한 결과:
    - 전부 dist_m이 채워져 있고 from_id≠to_id(road_link.link_id 형식 10자리 숫자)
    - 같은 트립 내에서 이전 행의 to_id = 다음 행의 from_id로 체인처럼 연결됨
      (예: CAR000030_20260724070948 trip_seq 1→2→3, 779m→949m→950m, to_id/from_id 순차 연결)
    - 즉 "게이트 통과 즉시 1건"이 아니라 "맵매칭 링크가 바뀔 때마다(hop) 매번 1건씩" 쌓이는 구조로 보임
  그런데 같은 트립의 prim_tollcharge(2단계 확정)는 from_id=to_id인 점(point) 형태 행을 갖는데, ID 자체가
  prim_chargehand 쪽 링크체인과 전혀 다름(예: 위 트립 prim_chargehand 링크ID군 vs prim_tollcharge의
  2520013913/2520013912/2520013918 — 서로 무관). 두 테이블이 같은 trip_id를 가리키면서도 정합성이 안 맞음.
  결론: prim_chargehand와 prim_tollcharge가 서로 다른 시점에 서로 다른 스크립트로 독립 시딩된 것으로 추정
  (I-3 범칙금 이중패턴, I-2 vat_amount 예외와 같은 종류의 데이터 비정합). 개방형이 "게이트 점 이벤트"인지
  "링크 hop 누적거리"인지 현재 데이터만으로 확정 불가 — 코드 착수 전 담당자 확인 필수(H절에 반영).
  문서 반영: web/docs/charge-flow.html 1절·7절, web/docs/charge-issues.html 개방형 섹션.

I-8. [대응방안 설계] 폐쇄형(CLOSED_ROAD) 휴게소 장시간 정차(시동 OFF) 시 진입정보 유실 위험
  시나리오: 고속도로 폐쇄구간 진입 후 휴게소에서 시동을 끄고 한참(수 시간) 있다가 재시동 → 진출 게이트
  통과 시 요금 미부과 가능성. 원인은 두 가지가 복합적:
    1. 진입 게이트 정보가 인메모리 세션(VEHICLE_TRIP_SESSION, TRIP_ID 단위, m_vtTripSessions)에만 있고 DB엔
       없음 — 기본 TTL(worker.ttl_sec, CFG_DEF_TTL=3600초=1시간, ConfigDefaults.h:29) 초과 정차 시 세션 소실.
    2. 시동 재시작 시 수집앱이 TRIP_EVENT=END/START로 새 TRIP_ID를 발급할 가능성 — 맞다면 TTL을 늘려도
       무의미(애초에 다른 세션이 됨). 수집앱이 실제로 이렇게 동작하는지는 미확인 — 확인 필요.
    3. AppMain.cpp의 rawlog_recover(Config.h:38, "GPS 좀비 PROCESSING 복구 SQL")는 원시 GPS 배치 복구용이지
       세션/진입정보 복구와는 무관 — 현재 이 문제에 대한 기존 대응 메커니즘 없음.
  대응방안(web/docs/charge-flow.html 2절 플로우차트에 반영):
    ① 진입 감지 시점에 세션뿐 아니라 DB에도 즉시 기록(durable) — 진입 이벤트당 1회뿐이라 볼륨 부담 적음
       (README F절 "핫패스 DB 왕복 최소화" 원칙과 충돌 안 됨, 매 GPS 아님)
    ② 진출 게이트 판정 시 TRIP_ID 세션뿐 아니라 DEVICE_KEY 기준 미종결 진입기록(DB)도 함께 조회 — TRIP_ID가
       바뀌어도 DEVICE_KEY로 찾아 정산 가능하게
    ③ CLOSED_ROAD 전용 TTL을 트립 전체 TTL과 분리해 훨씬 길게 설정(예: 12~24시간)
    ④ 그래도 못 찾는 영구 미종결 건은 일 단위 배치로 스윕해 AUDIT 처리(ExpireTtlSessions는 프로세스 인메모리
       기준이라 프로세스 재시작·트립 재시작을 못 버팀 — DB 기반 배치 별도 필요)
  미해결로 남는 것: DB durable 기록의 테이블 구조(prim_chargehand 선기록 vs 별도 경량 테이블), 배치 스윕
  주기, "N시간" 임계값 확정, 그리고 위 2번(트립 경계가 시동 ON/OFF와 연동되는지) 확인.

I-9. [정정, 2026-08-21] I-4/I-5의 "base_mapinfo=구간단속·주정차 기반 테이블" 결론은 폐기 — 실제 구현은 base_roadlink
  I-4/I-5(2026-08-06/08 작성) 결론과 달리, 실제 MapMatchSvr 구현(2026-08-11+, [[project_charge_unified_impl_design]])은
  구간단속(SPEED)·주정차(PARKING)를 포함한 5개 유형 전부 base_mapinfo가 아니라 base_roadlink(road_id=RL-Z0000N,
  road_kind 0~5)를 기반 테이블로 사용한다. 2026-08-21 재확인 결과:
    - base_mapinfo는 지금도 zone_kind='1'(152만건)·'2'(3.1만건)만 존재 — I-4가 말한 zone_kind='04'/'05' 데이터
      자체가 한 건도 없다(에초에 그 목적으로 채워진 적이 없었던 것으로 보임).
    - MapMatchSvr/src 전체·query.sql 어디에도 base_mapinfo 참조 없음(전수 grep 확인) — 완전히 미사용 테이블.
  결론: I-4/I-5는 실제 구현 이전 시점의 설계 구상이었고, base_roadlink로 전환된 뒤 문서가 갱신되지 않은 것.
  base_mapinfo(zone_kind=1/2, 전국 152만+3.1만건)는 SPEED/PARKING과 무관한 별도 목적(지도 표출용으로 추정,
  I-4 원문 참고) 데이터로 남겨두고, "구간단속·주정차 기반 테이블"이 필요하면 base_roadlink를 볼 것.

J. [2026-08-08] 개방식(OPEN) 임시 게이트 테스트 스캐폴드 생성
  I-7에서 개방형 모델 자체가 실측과 안 맞는 것이 확인됐지만, 담당자 확인 전까지 임시로라도 원래 설계 가정
  ("게이트 점 이벤트")을 그대로 따라 테스트할 수 있도록 최소 DDL·샘플데이터를 만들어둠:
    - roadnet/sql/base_roadlink.sql / base_tollgate.sql / base_tollfare.sql
      → base_roadlink·base_tollgate·base_tollfare 3개 테이블 재구성 DDL(⚠️ pg_dump 아님, 기준선 문서
        v1.0의 pg_description 조사 내용 기반 재구성 — 대조검증 전까지 참고용)
    - roadnet/sql/base_open_gate_test_data.sql
      → road_id='RL-Z900'(임시 개방식 도로 1건), tollgate_id='TG9000'(gate_div=M 게이트 1건),
        차종별(1~6) 임시 요금 6건(toll_fare는 placeholder, 실제 요금 아님)
    - web/docs/charge-open-gate-test.html
      → 위 DDL·샘플데이터 문서화 (charge-tables.html과 별개 문서, 사이드바에서 상호 링크)
  용도: 원격 DB 접속 없이 로컬에서 개방식 게이트 통과 판정 흐름 테스트. I-7 이슈(게이트 점 vs 링크 hop)의
  정식 해결과는 무관 — 임시 테스트 목적으로 원 설계안을 그대로 채택한 것뿐.

K. [2026-09-15] 전체 재매칭(백필) 표준 절차
  ※ 최초 작성분(같은 날 오전)은 "TTL 스윕이 안 돌아 잔여 세션이 유실된다"는 잘못된 전제와, END
    위치를 min(gps_seq)로만 조회해 두 번째 END를 놓친 사례표를 담고 있었다. 실제로 절차를 돌려
    검증한 결과 회수 0건이었고, 원인도 TTL이 아니었다. 아래가 정정본이다.

  K-1. 표준 절차
    1) MapMatchSvr 정지 — kill_svr.sh 실행 후 pgrep -x MapMatchSvr 로 실제 종료 확인
       ※ **반드시 정지 상태에서 리셋할 것.** 켜진 채 리셋하면 기동 시 stale 회수로 마지막 틱이
         먼저 처리되고 앞 틱이 뒤늦게 들어가 세션이 꼬인다 — 실측에서 stay_seconds 가 -7/-238 로
         기록됐다(지금은 BulkInsertCharges 가 0 으로 보정하고 WARN 을 남기지만, 애초에 안 만드는
         게 맞다).
    2) 리셋:
         BEGIN;
         DELETE FROM ruc.prim_chargehand;
         UPDATE ruc.prim_rawgps
            SET match_status=0, match_lat=NULL, match_lon=NULL, match_link_id=NULL,
                intersect_len=0, match_rsv_dt=NULL;
         COMMIT;
    3) 기동 후 match_status IN (0,2) 가 0 이 될 때까지 대기 = 재매칭 완료
       ※ ttl_sec 을 건드릴 필요 없다. K-2 참고.
    4) 검증 포인트
       - LOG_ERROR 0건
       - non_charge_reason NULL 0건 (Y/0 은 코드 0 이 적재된다)
       - stay_seconds / speed_kmh 음수 0건
       - 같은 charge_type=0 Y/0 행끼리 gps_seq 구간이 겹치지 않을 것
       - 두 번 연속 재매칭한 결과가 완전히 동일할 것(배치 구성에 따라 갈리면 안 됨)

  K-2. TTL 에 대한 사실관계 (오해 주의)
    - TTL 만료 판정은 GPS 시각이 아니라 **벽시계** 기준이다(stSession.dtLastSeen = time(nullptr)).
      과거 데이터를 넣어도 즉시 만료되지 않는다. TTL 분기를 일부러 태우려면 config.ini 의
      ttl_sec 을 낮춰야 한다(2026-09-15 개방형 TTL 거리계산 검증에 이 방법을 썼다).
    - 로그의 "ttl expiry" 문구는 트립종료 경로도 같은 함수를 쓰기 때문에 나온다 — **TTL 만료의
      증거가 아니다.**
    - 세션 맵의 키는 TRIP_ID 가 아니라 **DEVICE_KEY** 다(차량당 세션 1개). 같은 차량의 다음 운행이
      시작되면 이전 세션은 그 자리에서 초기화되므로, TTL 이 회수할 수 있는 건 사실상 "그 차량의
      마지막 운행" 뿐이다. 재매칭 뒤에 TTL 스윕을 유도해도 앞 트립들은 회수되지 않는다(실측 확인).
    - **[해결됨 2026-09-15] 종료 신호가 이상한 트립의 열린 run 마감.**
      (a) 최초 진단이 틀렸었다 — 유실 지점을 ResetTripSessionForBegin()(트립 전환)으로 적어 뒀으나,
          진단 로그로 확인하니 전체 재매칭에서 트립 전환은 단 1회(유령 트립)뿐이었다. 실제 유실은
          run() 이 배치 끝에서 `if (bTripEnded) mapSessions.erase(strDeviceKey)` 로 세션을 통째로
          버리는 지점이다. **END 가 스트림 중간에 찍힌 트립**은 그 뒤로도 tick 이 들어와
          stWorkSession 에 새 run 이 쌓이는데, 그게 과금 행 하나 없이 erase 와 함께 사라졌다.
          정상 트립은 END 가 마지막 tick 이라 이 경로를 안 탄다.
      (b) 조치 — ExpireTtlSessions() 안에 인라인이던 마감 블록(134줄)을
          **FlushOpenRunsAsAbnormalEnd()** 로 추출해 3곳에서 공용으로 쓴다:
            (1) TTL 스윕  (2) 배치 루프 직후(트립종료 이후 잔여 tick)  (3) 트립 전환 직전.
      (c) 코드 62(NON_CHARGE_REASON)는 **여전히 없다.** 2026-09-15 오전에 구현했다가 잡종 레코드
          (trip_id 는 새 운행, occur_dt 는 이전 운행) 때문에 전량 원복했고 이번 수정은 그 코드를
          되살리지 않았다 — 마감 사유는 기존 61(TTL/강제종료)을 그대로 쓴다. 원복 당시 잡종
          레코드의 원인은 szTripId 갱신 **뒤에** 마감한 것이었고, 이번에는 호출 위치를 szTripId
          대입 전으로 잡아 같은 문제가 없다.
      (d) 검증에서 자기 버그 2개를 잡았다(둘 다 안 잡았으면 회귀였다):
          - 정상 트립 중복 과금 — END tick 자신도 과금 판정을 거치므로, 트립종료 블록이
            bInClosedRoad 를 내린 직후 같은 tick 이 구역을 재진입시킨다. 그걸 또 마감해
            000376_20260819140532 의 폐쇄형이 두 행으로 중복 적재됐다 → 종료 tick 보다 뒤의 tick 이
            실제로 있을 때만(dwTripEndSeq 비교) 마감하도록 조건 추가.
          - stay_seconds 1,904,398초(약 22일) — AppendExpiredParkingCharge()/
            AppendExpiredOpenGateCharge() 가 체류 종료로 세션 dtLastSeen(**벽시계**)을 직접 읽는다.
            TTL 에서는 맞지만 트립종료 경로에서는 GPS 시각이어야 한다(과거 데이터 재매칭 시 수십 일
            차이) → 함수 진입부에서 dtEndTime 으로 임시 대입 후 복원. TTL 호출은 dtEndTime 에
            dtLastSeen 을 넘기므로 항등식이라 기존 동작 불변.
      (e) **현 데이터 기준 효과는 0이다.** 유일한 대상 000370_20260824140015 는 마감이 실제로
          실행되지만 생성 행 0건 — END 이후 35tick 이 주정차 run 을 이루는데 위반 임계(10분)
          미달이고, 매칭된 13tick 은 주정차 폴리곤 안이라 NODE_STEP 대상에서 제외된다. 즉 애초에
          청구할 과금이 없었다(종전에 "37m 유실"로 적은 건 매칭 tick 의 원시 주행거리였을 뿐
          과금 대상이 아니었다). 전체 재매칭 163행/180,219m 로 기준과 동일.
          메커니즘은 실재하고 이제 막혔으니, 다른 유형 도로에서 같은 일이 나면 그때 복구된다.

  K-5. [2026-09-15] 배포 시 주의 — MapMatchSvr/bin/config.ini 를 덮어쓰지 말 것
    config.ini 는 git 추적 대상인데 저장된 값이 **로컬 검증 환경 기준**이다.
      host=127.0.0.1 / userid=mytobcom / password=... / minconnect=11 / maxconnect=13
    실서버에서 git pull 또는 파일 전체 복사로 배포하면 DB 접속 정보가 로컬 값으로 덮여
    엔진이 엉뚱한 DB(또는 접속 실패)로 뜬다.
    - 배포 절차: 실서버 config.ini 를 먼저 백업하고, 배포 후 [database] 섹션을 원복할 것.
    - 반대로 query.sql 은 **반드시 갱신해야 한다** — 815b654 에서 [charge_insert] 에
      NON_CHARGE_REASON($31) 이 추가되어, 구 query.sql + 신 바이너리 조합은 동작하지 않는다.
      (bin/ 에서 이번에 갱신이 필요한 파일은 query.sql 하나뿐이다. run/kill/ps_svr.sh 는 불변)
    - 항구적 해결은 config.ini 를 추적 제외하고 config.ini.sample 만 커밋하는 것이나,
      이번 배포 범위 밖이라 손대지 않았다.

  K-4. [2026-09-15] 배포 동반 DB 변경 — **바이너리만 올리면 안 된다**
    실서버 배포 시 doc/deploy_2026-09-15.sql 을 함께 적용할 것. 로컬 검증 DB 는 반영 완료.
      PGPASSWORD=... psql -h 59.11.91.162 -U <user> -d ruc -v ON_ERROR_STOP=1 \
          -f doc/deploy_2026-09-15.sql
    내용(컬럼 추가·타입 변경 없음):
      1) base_codelc     : ROADKIND 대분류 행 1건 INSERT (부모 행 자체가 없었음)     ... DML
      2) base_codesc     : ROADKIND=5(면제도로) 행 1건 INSERT                         ... DML
      3) base_roadlink   : speed_limit_kmh CHECK(NULL 또는 0~255) 추가                ... DDL
      4) prim_chargehand : OCCUR_DT / NON_CHARGE_REASON 컬럼 코멘트 갱신              ... 메타데이터
                           (로컬에만 반영돼 있던 것을 실서버와 맞춘다. 데이터·구조 불변)
    - 멱등(재실행 안전), 전체가 단일 트랜잭션, 스크립트 끝에 검증 쿼리·롤백 SQL 포함.
      2026-09-15 로컬 DB 에 재실행하여 멱등성 실제 확인(INSERT 0 0 / 제약 건너뜀 / 종료코드 0).
    - 사전 점검 2건이 앞단에 있다. 두 경로(통과/중단) 모두 로컬에서 실제 실행해 확인했다.
      (0)   speed_limit_kmh 범위 밖 값이 있으면 문제 행을 지목하며 중단 — 데이터 정리 후 재실행.
      (0-2) 엔진 선행 스키마 6종(컬럼 4·인덱스 1·테이블 1)을 전수 점검, 빠진 것을 모두
            나열하며 중단. 이 6종은 이번 배포분이 아니라 2026-08-28~09-01 에 누적된 것으로,
            실서버에 이미 있으면 그냥 통과한다. 원본 DDL 은 query.sql 헤더에 있다.
              prim_chargehand.start_gps_seq / end_gps_seq / non_charge_reason
              prim_rawgps.match_rsv_dt + 인덱스 ix_prim_rawgps_stale_proc
              테이블 base_parking_fine
            ※ non_charge_reason 은 2026-09-01 부터 쓰는데 query.sql 헤더 선행요건 목록에
              빠져 있었다 — 2026-09-15 에 추가했다.
            ※ 없으면 "기동은 되는데 해당 SQL 만 런타임 실패"한다. 2026-08-29 에 start_gps_seq
              누락으로 과금 INSERT 전건 실패 + 같은 트랜잭션이라 맵매칭 결과까지 롤백된 전례.
    - 3) 적용 후 지도관리 화면에서 범위 밖 값 저장이 DB 레벨에서 거부된다. 그 화면의 오류 표시를
      배포 전에 한 번 확인할 것.

  K-3. 수집 데이터 TRIP_EVENT 이상 (엔진 밖, 확인 요청 필요)
    END(TRIP_EVENT=2) 틱은 직전 틱의 좌표·시각 복사본으로 스트림 맨 끝에 붙는 것이 정상인데,
    58개 트립 중 아래가 어긋난다. 근본 해결은 수집서버 쪽이다.
      END 0개 : 1건
      END 1개 : 55건 (정상)
      END 2개 : 2건  — 000370_20260824135458(4,96) / 000376_20260819140856(19,23).
                        둘 다 마지막 틱에도 END 가 있어 정상 마감된다.
      중간 END 후 미종료 : 000370_20260824140015 (END=6/총41틱, 이후 종료 신호 없음)

    ※ 2026-09-15 전수 재점검: 58트립 중 START 이상 0건, 종료 이상 4건(3유형). 상세 증거·재현 쿼리·
      수집서버 확인 요청 사항은 web/docs/tripevent-issues.html 로 정리했다(웹뷰어 문서 목록에서 접근).
      실제 과금 손실은 **0건**이다 — 유일한 후보였던 000370_20260824140015 도 END 이후 구간이
      과금 임계 미달이라 생성 행 0건이다(당초 "약 37m"로 적었던 값은 매칭 tick 의 원시 주행거리일
      뿐 과금 대상이 아니었다 — K-2 (e) 참고). 나머지는 엔진 방어로 흡수된다.

    엔진 방어는 두 갈래다 — END 중복(2개)은 IsTrustedTripEnd() 가, 종료신호 누락·중간 END 는
    FlushOpenRunsAsAbnormalEnd() 가 처리한다(2026-09-15 추가, K-2 해결됨 참고).
    마지막 항목("종료 신호 없이 끝난 운행")도 2026-09-15 부터 엔진이 마감한다 — K-2 (해결됨) 참고.
    다만 그 트립의 END 이후 구간은 과금 임계 미달이라 실제로 생성되는 행은 없다(효과 0). 근본
    해결은 여전히 수집서버 쪽이다.
    ※ [정정, 2026-09-15] 이 절에 원래 "엔진은 IsTrustedTripEnd()/코드 62 로 방어하고 있으나"
      라고 적혀 있었으나 **코드 62 는 존재하지 않는다.** 바로 위 K-2 에 적은 대로 구현했다가
      전량 원복했는데 이 줄만 안 고쳐 같은 문서 안에서 자기모순이 돼 있었다. 실제 NCR 코드는
      0·1·2·11·12·21·22·23·31·32·33·51·61 뿐이다(DataDefine.h 확인).

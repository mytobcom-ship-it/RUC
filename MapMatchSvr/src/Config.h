/**
 * @file Config.h
 * @brief 환경설정 구조체 정의 헤더 파일
*/
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <string>

using namespace std;

/**
 * @struct sConfig
 * @brief 프로세스 구동 환경
*/
typedef struct sConfig
{
	string							strLogPath;							// 로그 경로
	int								nLogLevel;							// 로그 레벨
	int								nLogKeepRunTime;					// 로그 삭제 시간 설정
	int								nLogKeepDay;						// 로그 보관일

	// DB 접속 정보
	string							strDBHost;							// 데이터베이 연결 Host
	int								nDBPort;							// 데이터베이스 연결 Port
	string							strDBName;							// 데이터베이스 이름
	string							strDBUserID;						// 데이터베이스 아이디
	string							strDBPasswd;						// 데이터베이스 비밀번호
	int								nDBMinConnect;						// DB 커넥션 풀 최소 연결 수
	int								nDBMaxConnect;						// DB 커넥션 풀 최대 연결 수
	int								nConnRetryMax;						// retrymax
	int								nConnRetryWait;						// retrywait

	// SQL 파일명
	string							strSQLFile;							// SQL 파일명

	// SQL 문 (쿼리.sql 세션 키)
	string							strRawLogRecoverSession;			// GPS 좀비 PROCESSING 복구 SQL
	string							strRawLogSelectSession;				// GPS 로그 조회·예약 SQL
	string							strRawLogUpdateSession;				// GPS 로그 갱신 SQL
	string							strChargeInsertSession;				// 과금 INSERT SQL (개방형 게이트 통과, 비어 있으면 비활성)
	string							strGateSelectSession;				// 과금 게이트(BASE_TOLLGATE) 전량 조회 SQL, 비어 있으면 CChargeDataLoader 게이트 캐시 비활성 (2026-08-12 최정우 추가)
	string							strZoneSelectSession;				// 과금 구역(BASE_ROADLINK) 전량 조회 SQL, 비어 있으면 CChargeDataLoader 구역 캐시 비활성 (2026-08-12 최정우 추가)
	string							strParkFineSelectSession;			// 주정차 과태료(BASE_PARKING_FINE) 최소 FROM_MIN 조회 SQL, 비어 있으면 체류시간 임계 비활성 (2026-08-24 최정우 추가)
	string							strTripEndUpdateSession;			// 트립 종료 시 trip_end_dt UPDATE SQL, 비어 있으면 비활성 (2026-08-12 최정우 추가)
	string							strTripSeqMaxSession;				// [sql] trip_seqmax — 트립의 기존 최대 TRIP_SEQ 조회(읽기 전용), 비어 있으면 이어 매기기 비활성 (2026-10-04 최정우, 사용자 확정 — TTL 마감 권장안)
	string							strServerStatusSession;				// 서버 상태(CPU/메모리) 하트비트 UPDATE SQL, 비어 있으면 비활성 (2026-08-20 최정우 추가)
	string							strStaleRecoverSession;				// 좀비 PROCESSING 운영 중 회수 SQL, 비어 있으면 비활성 (2026-08-29 최정우 추가)

	// 피더 (DB poll)
	int								nFetchLimit;						// 1회 조회·예약 최대 건수 (건)
	int								nFetchInterval;						// 큐 여유 시 DB 조회 간격 (ms)
	int								nQueuePauseCount;					// 큐 batch 수, 이상이면 DB 조회 중단 (건)
	int								nQueueMaxCount;						// 큐 더 차면 대기 최대 구간 (건)
	int								nQueueBusyMin;						// 큐 혼잡 시 조회 대기 최소 (ms)
	int								nQueueBusyMax;						// 큐 혼잡 시 조회 대기 최대 (ms)

	// 워커 (세션·종료)
	int								nTtlSec;							// trip_id 세션 유지 시간 (초)
	int								nShutdownWait;						// 종료 시 진행 중(활성) batch 완료 대기 (ms)
	int								nRetryMax;							// release 재시도 상한 (0=무제한)

	// 스레드 풀 개수
	int								nThreads;							// 스레드 풀 개수

	string							strDataFile;						// 데이터 바이너리 파일명 및 경로

	// 연속 맵매칭 정보
	int								nGeodetic;							// GPS 좌표 측지계
	int								nRadius;							// radius
	double							dfRadiusScale;					// radius_scale
	int								nRadiusMin;							// radius_min
	int								nRadiusMax;							// radius_max
	int								nRadiusSkip;						// radius_skip
	int								nMaxStep;							// maxstep
	double							dfHopLenRatio;						// [mapmatch] hoppenalty_lenratio — hop 벌점 링크길이 비례 상한, 0=비활성 (2026-08-23 최정우 추가)
	double							dfHopPenalty;						// [mapmatch] hoppenalty — depth 1단계당 가산 비용(m), 0=비활성 (2026-08-22 최정우 추가)
	int								nDistance;							// distance
	int								nMatchTimeout;						// timeout

	int								nAltGap;							// alt_gap
	int								nAltPenalty;						// alt_penalty (양수=페널티·음수=보너스)
	double							dfAltWeight;						// alt_weight
	double							dfAltSlope;							// alt_slope
	int								nReverseConfirm;					// reverse_confirm — 연속 역행 확정 포인트 수 (2026-07-21 최정우 추가)
	int								nOppStreakMax;						// opp_streakmax — 왕복분리 반대편 링크 연속 오매칭 허용 틱 수 (2026-08-24 최정우 추가)
	double							dfSpeedFactor;					// speed_factor (2026-07-20 최정우 추가)
	int								nSpeedMargin;					// speed_margin (km/h) (2026-07-20 최정우 추가)

	int								nGateReloadSec;						// [charge] gate_reload (단위: sec, 0=재조회 없음) (2026-08-12 최정우 추가)
	int								nStaleSec;				// [server] stale_sec (단위: sec, 0=비활성) — 이 시간 이상 PROCESSING(2) 인 행 회수 (2026-08-29 최정우 추가)
	int								nParkPad;							// [charge] park_pad (단위: m) — 구역판정 시 폴리곤 바깥으로 확장 허용하는 최대 거리 (2026-08-13 최정우 추가, 2026-09-03 park_buf 에서 개명)
	// [charge] park_accmax (단위: m) — 주정차 판정에 쓸 좌표의 ACCURACY_M 상한. 0=비활성
	//   측위에 실패한 단말은 셀 기반 대체 위치로 점프한 뒤 그 좌표에 얼어붙는다. 실측
	//   (실주행 11트립, analysis/rawvld_realcheck.py): accuracy_m>100 구간은 좌표 동결률
	//   64.6%, 보간 추정 참위치 대비 오차 중앙 189m(최대 486m)였다. 이 좌표로 폴리곤 포함을
	//   판정하면 51~100 구간 17건이 전부 허위 진입, 101~ 구간은 허위진입 20·누락 20 이었다.
	//   park_pad(확장 허용거리)로는 못 막는다 — 이 값은 "얼마나 여유를 줄까"이지 "이 좌표를
	//   믿을까"가 아니라서, 좌표 자체가 200m 틀리면 버퍼를 좁혀도 엉뚱한 자리에서 판정한다.
	//   16~50 구간은 오차 중앙 8.6m·폴리곤 오판정 0건이라 살린다 — RAW_VLD=false 도 주정차
	//   판정은 수행한다는 2026-08-22 확정은 그대로 유지된다 (2026-08-23 최정우 추가)
	int								nParkAccMax;
	// [mapmatch] ignore_rawvld — 1이면 RAW_VLD 검사를 건너뛰고 모든 좌표에 맵매칭을 시도한다.
	//   운영 데이터 전량 검증용 스위치다(기본 0=현행). 켜면 RAW_VLD=false 좌표(실주행의 15.5%)도
	//   매칭 대상이 되고, 그 좌표가 실제로 쓸 만한지는 엔진의 보완 로직(역행 확정·이상속도·
	//   클램프 저신뢰·hop 페널티)이 걸러내는지로 판별한다 (2026-08-23 최정우 추가)
	int								nIgnoreRawVld;
	int								nParkExitCnt;						// [charge] park_exitcnt — 구역 이탈 확정 연속 GPS 건수(디바운스) (2026-08-13 최정우 추가)
	int								nZoneExitCnt;						// [charge] zone_exitcnt — 게이트형 다중링크 구역 이탈 확정 연속 GPS 건수(디바운스), 0=비활성 (2026-09-21 최정우 추가)
	int								nNodeExitCnt;						// [charge] node_exitcnt — 일반도로(NODE_STEP) 이탈 확정 연속 GPS 건수(디바운스) (2026-08-24 최정우 추가)
	int								nParkSpeedMax;						// [charge] park_speedmax (단위: km/h) — 이 속도 이하에서만 주정차로 판정 (2026-08-22 최정우 추가)
	int								nParkEntryCnt;						// [charge] park_entrycnt — 세션 개시에 필요한 연속 충족 GPS 건수 (2026-08-22 최정우 추가)
	int								nParkRegraceSec;					// [charge] park_regrace (단위: sec) — 재진입 유예시간 (2026-08-14 최정우 추가)
	int								nParkTtlSec;						// [charge] park_ttl (단위: sec) — 마지막 신뢰(RAW_VLD=true) 확인 후 강제 마감까지의 시간 (2026-08-19 최정우 추가)
	int								nExemptRegraceSec;					// [charge] exempt_regrace (단위: sec) — 재진입 유예시간 (2026-08-14 최정우 추가)
	int								nExemptOutMax;						// [charge] exempt_outmax (단위: m) — 이 거리 이상 구역 밖을 주행하면 재진입 유예 무효 (2026-09-23 최정우 추가)

	string							strServerId;						// [server] id — PROC_SERVERSTATUS.SERVER_ID (2026-08-20 최정우 추가)
	int								nServerStatusIntervalSec;			// [server] status_interval (단위: sec, 0=비활성) (2026-08-20 최정우 추가)
} CONFIG, *PCONFIG;

#define CONFIG_SIZE												sizeof(CONFIG)

#endif //__CONFIG_H__

/**
 * @file Server.h
 * @brief 서버 클래스 헤더 파일
*/
#ifndef __SERVER_H__
#define __SERVER_H__

#include <stdio.h>
#include <string>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <memory>
#include "TypeDefine.h"
#include "Config.h"
#include "ConfigDefaults.h"
#include "log4z.h"
#include "Util.h"
#include "Thread.h"
#include "ThreadPool.h"
#include "Singleton.h"
#include "SingleThread.h"
#include "PostgrePool.h"
#include "SQLAccessor.h"
#include "LoggerManager.h"
#include "DataLoader.h"
#include "ChargeDataLoader.h"
#include "ProcessManager.h"
#include "RawLogFetcher.h"
#include "RawLogWorker.h"
#include "Mutex.h"
#include "Condition.h"

using namespace zsummer::log4z;
using namespace std;

// 단위: ms (2026-07-11 최정우 주석 추가)
#define SERVER_RUN_WAIT					CFG_DEF_RUN_WAIT
// 단위: sec (2026-07-11 최정우 주석 추가)
#define SERVER_MONITOR_INTERVAL			CFG_DEF_MONITOR
#define RECOVER_RETRY_MAX				CFG_DEF_RECOVER_MAX
// 단위: ms (2026-07-11 최정우 주석 추가)
#define RECOVER_RETRY_INTERVAL			CFG_DEF_RECOVER_WAIT

/**
 * @class CServer
 * @brief 서버 클래스
*/
class CServer : public CSingleThread
{
	DECLARE_SINGLETON(CServer)
public:
	CServer();
	virtual ~CServer();

	bool Initialize(const CONFIG& stConfig);
	void Uninitialize();
	void ProcessPeriodSec(time_t dtNow);
	void RequestShutdown();
	bool DrainPendingBatchesAndRelease();

	inline bool IsRun() { return m_bRun; }

private:
	virtual void run();
	void LogMonitorStatus(time_t dtNow);
	void WaitForNextCycle();
	// [2026-09-21 최정우 정정] 아래 하트비트 설명 주석이 RecoverStaleProcessing() 위에 붙어 있어
	//   마치 이 함수가 PROC_SERVERSTATUS 를 갱신하는 것처럼 읽혔다 — 제 선언 위로 옮긴다.
	// 좀비 PROCESSING(2) 운영 중 회수 — [stale_recover] (2026-08-29 최정우 추가)
	void RecoverStaleProcessing();
	// /proc/stat 1초 주기 샘플링 — m_dfLastCpuPct 갱신 (2026-08-20 최정우 추가)
	void UpdateCpuSample();
	// /proc/meminfo 기반 **시스템 전체** 메모리 사용량(엔진 자체 RSS 아님) (2026-08-20 최정우 추가)
	bool GetMemInfo(int& nUsedMB, int& nTotalMB, double& dfMemPct);
	// 서버 상태(CPU/메모리) 하트비트 — PROC_SERVERSTATUS 주기 UPDATE (2026-08-20 최정우 추가)
	void UpdateServerStatus();

private:
	CPostgrePool					*m_pcPostgrePool;					// PostgreSQL DB 커넥션 풀
	CSQLAccessor					*m_pcSQLAccessor;					// SQL 파일 읽기
	CLoggerManager					*m_pcLoggerManager;					// 로그 관리 클래스
	CDataLoader						*m_pcDataLoader;					// 기반 데이터 클래스
	CChargeDataLoader					*m_pcChargeDataLoader;				// 과금 게이트 데이터 클래스 (2026-08-12 최정우 추가)
	CThreadPool						*m_pcThreadPool;					// 스레드 풀
	CProcessManager					*m_pcProcessManager;				// GPS 정보 맵 매칭 처리 클래스
	CRawLogFetcher					*m_pcRawLogFetcher;					// 원시 GPS DB 폴링 클래스
	CRawLogWorker					*m_pcRawLogWorker;					// 원시 GPS batch 처리 워커

private:
	CMutex							m_cRunMutex;						// 서버 실행 플래그 mutex
	CCondition						m_cRunCondition;					// 서버 실행 플래그 condition

private:
	bool							m_bRun;								// 서버 실행 여부
	bool							m_bUninitialized;					// Uninitialize 중복 실행 가드 (2026-07-10 최정우 추가)
	// [버그 수정, 2026-09-11 최정우] 종료 시 ThreadPool 워커(detach, 운영 기본값)가 대기시간 안에
	//   전부 EWS_STOPPED 에 도달 못하면(느린 DB 호출 등으로 여전히 실행 중), 그 워커는 지금도
	//   m_pcRawLogWorker->run() 내부에서 m_pcProcessManager/m_pcDataLoader/m_pcChargeDataLoader/
	//   m_pcPostgrePool 을 참조 중일 수 있다 — 이 상태에서 그 객체들을 delete 하면 아직 살아있는
	//   네이티브 스레드가 해제된 메모리를 계속 참조하는 use-after-free 가 된다. WaitForAllStopped()
	//   가 실패로 보고하면 이 플래그를 세워 하위 객체 delete 를 전부 건너뛰고 의도적으로 누수시킨다
	//   (프로세스가 곧 종료되므로 누수보다 크래시가 훨씬 위험) — ThreadPool.cpp 의 동일 근거 수정과
	//   짝을 이룸.
	bool							m_bSkipDependentTeardown;
	string							m_strLogPath;						// 로그 경로
	int								m_nLogLevel;						// 로그 레벨
	int								m_nLogKeepRunTime;					// 로그 삭제 시간 설정
	int								m_nLogKeepDay;						// 로그 보관일
	int								m_nWorkerThread;					// 스레드 풀 개수
	pthread_t						m_hTimerThread;						// 로그 관리 Thread 핸들
	string							m_strSQLFile;						// SQL 파일
	string							m_strDBHost;						// DB 연결 Host
	int								m_nDBPort;							// DB 연결 Port
	string							m_strDBName;						// DB 이름
	string							m_strDBUserID;						// DB 아이디
	string							m_strDBPasswd;						// DB 비밀번호
	int								m_nDBMinConnect;					// DB pool 최소 연결 수
	int								m_nDBMaxConnect;					// DB pool 최대 연결 수
	string							m_strRawLogRecoverSQL;				// PROCESSING 복구 SQL
	string							m_strRawLogSelectSQL;				// 조회·예약 SQL (UPDATE RETURNING)
	string							m_strRawLogUpdateSQL;				// 결과 갱신 SQL
	string							m_strChargeInsertSQL;				// 개방형 게이트 통과 과금 INSERT SQL, 비어 있으면 비활성
	string							m_strTripEndUpdateSQL;				// 트립 종료 시 trip_end_dt UPDATE SQL, 비어 있으면 비활성 (2026-08-12 최정우 추가)
	string							m_strTripSeqMaxSQL;					// [trip_seqmax] 트립의 기존 최대 TRIP_SEQ 조회 (2026-10-04 최정우, 사용자 확정 — TTL 마감 권장안)
	string							m_strGateSelectSQL;				// 과금 게이트 전량 조회 SQL (2026-08-12 최정우 추가)
	string							m_strZoneSelectSQL;				// 과금 구역 전량 조회 SQL (2026-08-12 최정우 추가)
	string							m_strParkFineSelectSQL;				// 주정차 과태료 최소 FROM_MIN 조회 SQL, 비어 있으면 체류시간 임계 비활성 (2026-08-24 최정우 추가)
	int								m_nGateReloadSec;					// [charge] gate_reload — 게이트·구역 캐시 재조회 주기(sec, 0=재조회 없음) (2026-08-12 최정우 추가)
	time_t							m_dtLastGateReload;					// 마지막 게이트 캐시 재조회 시각 (2026-08-12 최정우 추가)
	int								m_nStaleSec;				// [server] stale_sec — 좀비 PROCESSING 회수 주기 겸 임계(sec, 0=비활성) (2026-08-29 최정우 추가)
	time_t							m_dtLastStaleRecover;				// 마지막 좀비 PROCESSING 회수 시각 (2026-08-29 최정우 추가)
	string							m_strStaleRecoverSQL;				// 좀비 PROCESSING 운영 중 회수 SQL (2026-08-29 최정우 추가)
	int								m_nParkPad;							// [charge] park_pad — 구역판정 시 폴리곤 바깥 확장 허용거리(m) (2026-08-13 최정우 추가, 2026-09-03 park_buf 에서 개명)
	int								m_nIgnoreRawVld;					// [mapmatch] ignore_rawvld — RAW_VLD 무시 전량 매칭(검증용) (2026-08-23 최정우 추가)
	int								m_nParkAccMax;						// [charge] park_accmax — 주정차 판정 좌표 정확도 상한(m), 0=비활성 (2026-08-23 최정우 추가)
	int								m_nParkExitCnt;						// [charge] park_exitcnt — 구역 이탈 확정 연속 GPS 건수(디바운스) (2026-08-13 최정우 추가)
	int								m_nZoneExitCnt;						// [charge] zone_exitcnt — 게이트형 다중링크 구역 이탈 확정 연속 GPS 건수(디바운스), 0=비활성 (2026-09-21 최정우 추가)
	int								m_nNodeExitCnt;						// [charge] node_exitcnt — 일반도로(NODE_STEP) 이탈 확정 연속 GPS 건수(디바운스) (2026-08-24 최정우 추가)
	int								m_nParkRegraceSec;					// [charge] park_regrace — 재진입 유예시간(초) (2026-08-14 최정우 추가)
	int								m_nParkTtlSec;						// [charge] park_ttl — 마지막 신뢰 확인 후 강제 마감까지의 시간(초) (2026-08-19 최정우 추가)
	int								m_nExemptRegraceSec;				// [charge] exempt_regrace — 재진입 유예시간(초) (2026-08-14 최정우 추가)
	int								m_nExemptOutMax;					// [charge] exempt_outmax — 이 거리(m) 이상 구역 밖을 주행하면 재진입 유예 무효 (2026-09-23 최정우 추가)
	string							m_strServerStatusSQL;				// 서버 상태 하트비트 UPDATE SQL, 비어 있으면 비활성 (2026-08-20 최정우 추가)
	string							m_strServerId;						// [server] id — PROC_SERVERSTATUS.SERVER_ID (2026-08-20 최정우 추가)
	int								m_nServerStatusIntervalSec;			// [server] status_interval — 하트비트 주기(초, 0=비활성) (2026-08-20 최정우 추가)
	time_t							m_dtLastServerStatusUpdate;			// 마지막 하트비트 반영 시각 (2026-08-20 최정우 추가)
	unsigned long long				m_qwPrevCpuTotal;					// 직전 /proc/stat 샘플 — CPU 총합 tick (2026-08-20 최정우 추가)
	unsigned long long				m_qwPrevCpuIdle;					// 직전 /proc/stat 샘플 — CPU idle tick (2026-08-20 최정우 추가)
	double							m_dfLastCpuPct;						// 가장 최근 계산된 CPU 사용률(%) — 1초 주기 샘플링, 하트비트 시점에 최신값 사용 (2026-08-20 최정우 추가)
	bool							m_bCpuSampleValid;					// m_qwPrevCpu* 유효 여부(최초 1회는 델타 계산 불가) (2026-08-20 최정우 추가)
	int								m_nFetchLimit;						// 1회 조회·예약 최대 건수 (건)
	int								m_nFetchInterval;					// 큐 여유 시 DB 조회 간격 (ms)
	int								m_nQueuePauseCount;					// 큐 batch 수, 이상이면 DB 조회 중단 (건)
	int								m_nQueueMaxCount;					// 큐 더 차면 대기 최대 구간 (건)
	int								m_nQueueBusyMin;					// 큐 혼잡 시 조회 대기 최소 (ms)
	int								m_nQueueBusyMax;					// 큐 혼잡 시 조회 대기 최대 (ms)
	int								m_nTtlSec;							// trip_id 세션 유지 시간 (초)
	int								m_nShutdownWait;					// 종료 시 워커 처리 완료 대기 (ms)
	int								m_nRetryMax;						// release 재시도 상한 (0=무제한)
	int								m_nThreads;							// 스레드 풀 개수
	string							m_strDataFile;						// 데이터 바이너리 파일명 및 경로
	uint8							m_nCoordinateType;					// GPS 좌표 측지계
	sint16							m_nRadius;							// 맵 매칭 유효 거리
	uint16							m_nMaxStep;							// 연속 맵매칭시 연결 링크 확인 최대 개수
	double							m_dfHopLenRatio;					// hoppenalty_lenratio (2026-08-23 최정우 추가)
	double							m_dfHopPenalty;						// hoppenalty — depth 1단계당 가산 비용(m) (2026-08-22 최정우 추가)
	int								m_nParkSpeedMax;					// park_speedmax — 주정차 판정 속도 상한(km/h) (2026-08-22 최정우 추가)
	int								m_nParkEntryCnt;					// park_entrycnt — 세션 개시 연속 GPS 건수 (2026-08-22 최정우 추가)
	uint32							m_dwMaxDistance;					// 연속 맵 매칭시 좌표간 최대 유효 거리
	int								m_nMatchTimeout;					// 1 GPS 맵매칭 처리 임계 (ms, 0=비활성, #16)
	double							m_dfRadiusScale;					// config radius_scale — 검색반경 = scale × ACCURACY_M (2026-07-08 최정우)
	int								m_nRadiusMin;						// config radius_min — 적응형 검색 반경 하한 (m) (2026-07-08 최정우)
	int								m_nRadiusMax;						// config radius_max — 적응형 검색 반경 상한 (m) (2026-07-08 최정우)
	int								m_nRadiusSkip;						// config radius_skip — ACCURACY_M 초과 시 SKIP (m). 0=비활성 (2026-07-08 최정우)
	int								m_nAltGap;							// config alt_gap — 직전 매칭 고도와 허용 차이(m)
	int								m_nAltPenalty;						// config alt_penalty — 양수=ROAD_TYPE 불일치 비용 가산·음수=같은 ROAD_TYPE 비용 감산(m)
	double							m_dfAltWeight;						// config alt_weight — 차이 초과 시 고도차 가중. 0=비활성
	double							m_dfAltSlope;						// config alt_slope
	int								m_nReverseConfirm;					// config reverse_confirm — 연속 역행 확정 포인트 수 (2026-07-21 최정우 추가)
	int								m_nOppStreakMax;					// config opp_streakmax — 왕복분리 반대편 링크 연속 오매칭 허용 틱 수 (2026-08-24 최정우 추가)
	double							m_dfSpeedFactor;				// config speed_factor (2026-07-20 최정우 추가)
	int								m_nSpeedMargin;					// config speed_margin (km/h) (2026-07-20 최정우 추가)

	time_t							m_dtLastMonitorLog;					// 마지막 모니터링 시각
	int								m_nLastQueueCount;					// 이전 큐 적재량
	bool							m_bQueueWarnActive;					// 큐 backlog WARN 상태
	bool							m_bAllBusyWarnActive;				// 워커 전원 사용 중 WARN 상태
};

#endif //__SERVER_H__

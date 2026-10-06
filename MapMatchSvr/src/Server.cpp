/**
 * @file Server.cpp
 * @brief 서버 클래스 소스 파일
*/
#include "Server.h"
#include "RawLogWorker.h"
#include "SingleInstanceLock.h"
#include <signal.h>

static CServer *g_pcServerInstance = nullptr;
// SIGINT/SIGTERM 수신 플래그 — 핸들러는 이 값만 세팅(async-시그널-safe) (2026-07-10 최정우 추가)
static volatile sig_atomic_t g_nShutdownRequested = 0;

/**
 * @brief SIGINT/SIGTERM 수신 시 서버 run 루프 종료
 * @param[in] nSignal 시그널 번호
 * @return void
 */
static void ServerSignalHandler(int nSignal)
{
	(void)nSignal;

	// 2026-07-10 최정우 주석 처리: 시그널 컨텍스트에서 뮤텍스/조건변수/pthread_kill 호출은
	// async-시그널-safe 위반. 특히 interrupt()가 run 스레드에 SIGUSR1→예외를 던져
	//   pthread_cond_timedwait(m_cRunCondition) 를 강제 언와인드 → 조건변수 내부 ref 오염 →
	//   종료 시 pthread_cond_destroy 무한 대기(hang) 유발.
	// [주석 보완, 2026-09-21 최정우] 위 서술은 **2026-07-10 당시의 상황**이다(이력으로 남긴다).
	//   지금은 CSingleThread::interruptHandler() 가 예외를 던지지 않고 플래그만 세우므로
	//   "SIGUSR1→예외→강제 언와인드" 경로 자체가 없어졌다. 그래도 아래 주석 처리는 그대로
	//   두는 것이 맞다 — 뮤텍스·조건변수 조작이 시그널 핸들러에서 안전하지 않다는 근본 이유는
	//   변하지 않았고, 플래그만 세우는 현재 방식이 이미 정석이기 때문이다.
	//if (g_pcServerInstance != nullptr)
	//{
	//	g_pcServerInstance->RequestShutdown();
	//g_pcServerInstance->interrupt();
	//}
	// 플래그만 세팅. run 루프가 이를 관찰하여 정상 경로로 종료 (2026-07-10 최정우 수정)
	g_nShutdownRequested = 1;
}

/**
 * @brief 로그 관리 검사 쓰레드
 * @param[in] context 호출 클래스 포인터
 * @return nullptr
 * @remark 로그 관리만 하는 스레드가 아니다 — 1초마다 CServer::ProcessPeriodSec() 을 불러 PID 파일
 *   잠금 무결성 확인·만료 로그 삭제·게이트/구역/주정차 과태료 캐시 주기 재조회(gate_reload)·
 *   좀비 PROCESSING 운영 중 회수(stale_sec)·CPU 샘플·서버 상태 하트비트(status_interval)를 처리한다.
 *   m_bRun 이 false 가 되면 종료하며, CServer::Uninitialize() 가 pthread_join 으로 회수한다
 *   (2026-10-06 최정우 주석 추가)
*/
extern "C" void *TimerThread(void *context)
{
	CServer *pcServer = reinterpret_cast<CServer *>(context);
	if (pcServer == nullptr)
		return nullptr;

	CUtil cUtil;

	time_t dtPre = time(nullptr);
	time_t dtNow = time(nullptr);
	while (pcServer->IsRun())
	{
		dtNow = time(nullptr);
		if (dtPre != dtNow)
		{
			// 매 초마다 로그 관리 시간 검사 및 처리
			//   [정정] 로그 관리뿐 아니라 캐시 재조회·stale 회수·하트비트 등 주기 작업 전체(위 @remark)
			//   (2026-10-06 최정우 주석 수정)
			pcServer->ProcessPeriodSec(dtNow);
		}

		dtPre = dtNow;
		// 타이머 쓰레드 10ms 슬립 (2026-07-08 최정우 주석 추가)
		cUtil.Sleep(0, 10000);
	}

	return nullptr;
}

/**
 * @brief 작업용 스레드 클래스 (CRawLogWorker 래퍼)
*/
class CWorkerManager : public virtual Runnable
{
public:
	CWorkerManager() : m_pcRawLogWorker(nullptr) {}

	/**
	 * @brief 위임 대상 RawLogWorker 연결 — 소유권은 넘기지 않는다(해제는 CServer::Uninitialize())
	 * @param[in] pcRawLogWorker 워커 스레드 전체가 공유하는 CRawLogWorker 인스턴스
	 * @return void
	 * @remark 이 래퍼 자체는 CThreadPool 이 소유해 ~CThreadPool() 에서 delete 한다 (2026-10-06 최정우 주석 추가)
	*/
	void SetWorker(CRawLogWorker *pcRawLogWorker)
	{
		m_pcRawLogWorker = pcRawLogWorker;
	}

	/**
	 * @brief 워커 1 배치 처리 — CThreadPoolWorker::run() 이 Dequeue 한 batch 를 넘겨 호출한다
	 * @param[in] nThreadId 워커(큐) 인덱스 — RawLogWorker 의 세션 슬롯 번호와 같다
	 * @param[in] context RAW_LOG_BATCH * (Dequeue 된 원시 GPS batch)
	 * @return void
	 * @remark 같은 워커 스레드에서만 그 슬롯의 세션을 만진다는 소유권 규칙으로 락 없이 운영된다
	 *   (2026-10-06 최정우 주석 추가)
	*/
	virtual void run(int nThreadId, void *context)
	{
		if (m_pcRawLogWorker != nullptr)
		{
			// ThreadPool Runnable → RawLogWorker 배치 처리 위임 (2026-07-08 최정우 주석 추가)
			m_pcRawLogWorker->run(nThreadId, context);
		}
	}

	// 워커 유휴 시 TTL 검사 위임 — 래퍼가 전달하지 않으면 Runnable 기본 구현(빈 함수)이 불려
	//   주기 TTL 마감이 동작하지 않는다 (2026-10-04 최정우 추가, TTL 마감 권장안 (다))
	virtual void idle(int nThreadId)
	{
		if (m_pcRawLogWorker != nullptr)
			m_pcRawLogWorker->idle(nThreadId);
	}

private:
	CRawLogWorker					*m_pcRawLogWorker;
};

/**
 * @brief 생성자
*/
CServer::CServer() : 
	m_pcPostgrePool(nullptr),
	m_pcSQLAccessor(nullptr), 
	m_pcLoggerManager(nullptr), 
	m_pcDataLoader(nullptr),
	m_pcChargeDataLoader(nullptr),
	m_pcThreadPool(nullptr),
	m_pcProcessManager(nullptr), 
	m_pcRawLogFetcher(nullptr),
	m_pcRawLogWorker(nullptr),
	m_bRun(false),
	m_bUninitialized(false),
	m_bSkipDependentTeardown(false),
	m_nWorkerThread(0), 
	m_hTimerThread(0),
	m_nDBMinConnect(CFG_DEF_MINCONNECT),
	m_nDBMaxConnect(5),
	m_nGateReloadSec(CFG_DEF_GATE_RELOAD),
	m_dtLastGateReload(0),
	m_nStaleSec(CFG_DEF_STALE_SEC),
	m_dtLastStaleRecover(0),
	m_nParkPad(CFG_DEF_PARK_PAD),
	m_nIgnoreRawVld(CFG_DEF_IGNORE_RAWVLD),
	m_nParkAccMax(CFG_DEF_PARK_ACCMAX),
	m_nParkExitCnt(CFG_DEF_PARK_EXITCNT),
	m_nZoneExitCnt(CFG_DEF_ZONE_EXITCNT),
	m_nNodeExitCnt(CFG_DEF_NODE_EXITCNT),
	m_nParkTtlSec(CFG_DEF_PARK_TTL),
	m_nExemptRegraceSec(CFG_DEF_EXEMPT_REGRACE),
	m_nExemptOutMax(CFG_DEF_EXEMPT_OUTMAX),
	m_strServerId(CFG_DEF_SERVER_ID),
	m_nServerStatusIntervalSec(CFG_DEF_STATUS_INTVL),
	m_dtLastServerStatusUpdate(0),
	m_qwPrevCpuTotal(0),
	m_qwPrevCpuIdle(0),
	m_dfLastCpuPct(0.0),
	m_bCpuSampleValid(false),
	m_nFetchLimit(CFG_DEF_LIMIT),
	m_nFetchInterval(CFG_DEF_FETCH_INTVL),
	m_nQueuePauseCount(CFG_DEF_Q_PAUSE_CNT),
	m_nQueueMaxCount(CFG_DEF_Q_MAX_CNT),
	m_nQueueBusyMin(CFG_DEF_Q_BUSY_MIN),
	m_nQueueBusyMax(CFG_DEF_Q_BUSY_MAX),
	m_nTtlSec(CFG_DEF_TTL),
	m_nShutdownWait(CFG_DEF_SHUTDOWN_WAIT),
	m_nRetryMax(CFG_DEF_RETRY_MAX),
	m_nCoordinateType(0),
	m_nRadius(0),
	m_nMaxStep(0),
	m_dwMaxDistance(0),
	m_nMatchTimeout(0),
	m_dfRadiusScale(CFG_DEF_RADIUS_SCALE),
	m_nRadiusMin(CFG_DEF_RADIUS_MIN),
	m_nRadiusMax(CFG_DEF_RADIUS),
	m_nRadiusSkip(CFG_DEF_RADIUS_SKIP),
	m_nAltGap(CFG_DEF_ALT_GAP),
	m_nAltPenalty(CFG_DEF_ALT_PENALTY),
	m_dfAltWeight(CFG_DEF_ALT_WEIGHT),
	m_dfAltSlope(CFG_DEF_ALT_SLOPE),
	m_nReverseConfirm(CFG_DEF_REVERSE_CONFIRM),
	m_nOppStreakMax(CFG_DEF_OPP_STREAKMAX),
	m_dfSpeedFactor(CFG_DEF_SPEED_FACTOR),
	m_nSpeedMargin(CFG_DEF_SPEED_MARGIN),
	m_dtLastMonitorLog(0),
	m_nLastQueueCount(0),
	m_bQueueWarnActive(false),
	m_bAllBusyWarnActive(false)
{
}

/**
 * @brief 소멸자
*/
CServer::~CServer()
{
	// 소멸 시 서버·리소스 정리 (2026-07-08 최정우 주석 추가)
	Uninitialize();
}

/**
 * @brief 서버 초기화
 * @param[in] stConfig 환경 설정
 * @return true(성공), false(실패)
 * @remark 기동 순서(2026-10-06 최정우 주석 추가): SQL 로드 → DB 풀 → link.psf 적재 → 과금 캐시(게이트·
 *   구역·주정차 과태료) 1회 로드 → ProcessManager·RawLogWorker·ThreadPool(워커 기동) → Fetcher 초기화
 *   → m_bRun=true → 타이머 스레드 → 시그널 핸들러 → rawgps_recover(최대 RECOVER_RETRY_MAX 회, 실패 시
 *   기동 중단) → Fetcher start(). 복구가 Fetcher 기동보다 먼저라 기동 직후 조회는 복구된 행을 포함한다.
 *   false 반환 경로는 모두 Uninitialize() 로 부분 초기화를 되돌린다. Fetcher start() 실패는 예외
 *   (IllegalThreadStateException)로 빠져 main 의 catch 가 Uninitialize() 를 부른다
*/
bool CServer::Initialize(const CONFIG& stConfig)
{
	m_strLogPath = stConfig.strLogPath;
	m_nLogLevel = stConfig.nLogLevel;
	m_nLogKeepRunTime = stConfig.nLogKeepRunTime;
	m_nLogKeepDay = stConfig.nLogKeepDay;
	m_nWorkerThread = stConfig.nThreads;
	m_strDBHost = stConfig.strDBHost;
	m_nDBPort = stConfig.nDBPort;
	m_strDBName = stConfig.strDBName;
	m_strDBUserID = stConfig.strDBUserID;
	m_strDBPasswd = stConfig.strDBPasswd;
	m_nDBMinConnect = stConfig.nDBMinConnect;
	m_nDBMaxConnect = stConfig.nDBMaxConnect;
	m_nThreads = stConfig.nThreads;
	m_strDataFile = stConfig.strDataFile;
	m_nCoordinateType = static_cast<uint8>(stConfig.nGeodetic);
	m_nRadius = static_cast<sint16>(stConfig.nRadius);
	m_nMaxStep = static_cast<uint16>(stConfig.nMaxStep);
	m_dfHopPenalty = stConfig.dfHopPenalty;					// (2026-08-22 최정우 추가)
	m_dfHopLenRatio = stConfig.dfHopLenRatio;				// (2026-08-23 최정우 추가)
	m_dwMaxDistance = static_cast<uint32>(stConfig.nDistance);
	m_nMatchTimeout = stConfig.nMatchTimeout;
	m_dfRadiusScale = stConfig.dfRadiusScale;
	m_nRadiusMin = stConfig.nRadiusMin;
	m_nRadiusMax = stConfig.nRadiusMax;
	m_nRadiusSkip = stConfig.nRadiusSkip;
	m_nAltGap = stConfig.nAltGap;
	m_nAltPenalty = stConfig.nAltPenalty;
	m_dfAltWeight = stConfig.dfAltWeight;
	m_dfAltSlope = stConfig.dfAltSlope;
	m_nReverseConfirm = stConfig.nReverseConfirm;
	m_nOppStreakMax = stConfig.nOppStreakMax;
	m_dfSpeedFactor = stConfig.dfSpeedFactor;
	m_nSpeedMargin = stConfig.nSpeedMargin;
	m_nFetchLimit = stConfig.nFetchLimit;
	m_nFetchInterval = stConfig.nFetchInterval;
	m_nQueuePauseCount = stConfig.nQueuePauseCount;
	m_nQueueMaxCount = stConfig.nQueueMaxCount;
	m_nQueueBusyMin = stConfig.nQueueBusyMin;
	m_nQueueBusyMax = stConfig.nQueueBusyMax;
	m_nTtlSec = stConfig.nTtlSec;
	m_nShutdownWait = stConfig.nShutdownWait;
	m_nRetryMax = stConfig.nRetryMax;
	m_nGateReloadSec = stConfig.nGateReloadSec;					// (2026-08-12 최정우 추가)
	m_nStaleSec = stConfig.nStaleSec;		// (2026-08-29 최정우 추가)
	m_nParkPad = stConfig.nParkPad;								// (2026-08-13 최정우 추가)
	m_nParkAccMax = stConfig.nParkAccMax;						// (2026-08-23 최정우 추가)
	m_nIgnoreRawVld = stConfig.nIgnoreRawVld;					// (2026-08-23 최정우 추가)
	m_nParkExitCnt = stConfig.nParkExitCnt;						// (2026-08-13 최정우 추가)
	m_nZoneExitCnt = stConfig.nZoneExitCnt;						// (2026-09-21 최정우 추가)
	m_nNodeExitCnt = stConfig.nNodeExitCnt;						// (2026-08-24 최정우 추가)
	m_nParkSpeedMax = stConfig.nParkSpeedMax;					// (2026-08-22 최정우 추가)
	m_nParkEntryCnt = stConfig.nParkEntryCnt;					// (2026-08-22 최정우 추가)
	m_nParkTtlSec = stConfig.nParkTtlSec;							// (2026-08-19 최정우 추가)
	m_nExemptRegraceSec = stConfig.nExemptRegraceSec;				// (2026-08-14 최정우 추가)
	m_nExemptOutMax = stConfig.nExemptOutMax;						// (2026-09-23 최정우 추가)
	m_strServerId = stConfig.strServerId;							// (2026-08-20 최정우 추가)
	m_nServerStatusIntervalSec = stConfig.nServerStatusIntervalSec;	// (2026-08-20 최정우 추가)

	// stale_sec 은 ttl_sec 이상이어야 한다 — 1틱 지연커밋으로 세션 보류(pending) 중인 행은
	//   아직 PROCESSING(2) 이고 메모리 버퍼에 남아 있다. ttl_sec 안에 회수해 PENDING(0)으로
	//   되돌리면 그 행이 새 행처럼 재조회되면서 보류 버퍼와 겹쳐 같은 GPS 를 두 번 처리한다
	//   (RawLogWorker 의 orphan release 가 세션 보류 행을 제외하는 이유와 동일).
	//   세션이 TTL 로 소멸한 뒤에 회수해야 안전하므로 미만이면 ttl_sec 으로 올린다 (2026-08-29 최정우 추가)
	//   [보완] 같은 값(stale_sec == ttl_sec, 현행 config 3600/3600)이면 여유가 0 이다 — TTL 마감은
	//   배치 처리 끝·워커 유휴 tick(60초)에만 돌아 경과 직후 즉시 마감된다는 보장이 없다.
	//   ttl_sec=0(TTL 비활성)이면 이 상향 자체가 걸리지 않는다 (2026-10-06 최정우 주석 추가)
	if ((m_nStaleSec > 0) && (m_nStaleSec < m_nTtlSec))
	{
		LOGFMTW("stale_sec[%d] < ttl_sec[%d] — 세션 보류 행 이중 처리 방지를 위해 ttl_sec 으로 올림",
			m_nStaleSec, m_nTtlSec);
		m_nStaleSec = m_nTtlSec;
	}

	LOGFMTI("please wait ....");

	// SQL 쿼리문 읽기
	m_pcSQLAccessor = new (std::nothrow)CSQLAccessor;
	if (m_pcSQLAccessor == nullptr)
	{
		LOGFMTE("sql read class start fail!");
		// SQL 접근자 할당 실패 시 부분 초기화 롤백 (2026-07-08 최정우 주석 추가)
		Uninitialize();
		return false;
	}

	// log 보관 관리 클래스
	m_pcLoggerManager = new (std::nothrow)CLoggerManager;
	if (!m_pcLoggerManager)
	{
		LOGFMTE("logger manager class memory allocate failed!");
		// 로거 매니저 할당 실패 시 부분 초기화 롤백 (2026-07-08 최정우 주석 추가)
		Uninitialize();
		return false;
	}

	// 로그 보관·삭제 스케줄 초기화 (2026-07-08 최정우 주석 추가)
	if (!m_pcLoggerManager->Initialize(m_strLogPath, m_nLogKeepRunTime, m_nLogKeepDay))
	{
		LOGFMTE("log manager initialize failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("logger manager initialize success!");

	// SQL load
	m_strSQLFile = stConfig.strSQLFile;
	// SQL 파일에서 쿼리문 로드 (2026-07-08 최정우 주석 추가)
	if (!m_pcSQLAccessor->Initialize(m_strSQLFile))
	{
		LOGFMTE("sql query read fail!");
		Uninitialize();
		return false;
	}

	// PROCESSING 복구 SQL (기동 시 1회)
	//   필수 — SQL 이 비면 기동 실패(config.ini 주석의 "선택" 표기와 다름) (2026-10-06 최정우 주석 추가)
	// 세션명으로 PROCESSING→PENDING 복구 SQL 조회 (2026-07-08 최정우 주석 추가)
	m_strRawLogRecoverSQL = m_pcSQLAccessor->GetSQL(stConfig.strRawLogRecoverSession);
	if (m_strRawLogRecoverSQL.empty())
	{
		LOGFMTE("raw gps recover query is empty!");
		Uninitialize();
		return false;
	}

	// 좀비 PROCESSING 운영 중 회수 SQL (선택) — 비어 있으면 기동 시 rawgps_recover 만 동작.
	//   [sql] stale_recover 미설정은 실패가 아니다 (2026-08-29 최정우 추가)
	if (!stConfig.strStaleRecoverSession.empty())
	{
		m_strStaleRecoverSQL = m_pcSQLAccessor->GetSQL(stConfig.strStaleRecoverSession);
		if (m_strStaleRecoverSQL.empty())
			LOGFMTW("stale processing recover query is empty! — 운영 중 좀비 PROCESSING 회수 비활성");
	}

	// 조회·예약 SQL (UPDATE RETURNING)
	// 세션명으로 rawgps_select SQL 조회 (2026-07-08 최정우 주석 추가)
	m_strRawLogSelectSQL = m_pcSQLAccessor->GetSQL(stConfig.strRawLogSelectSession);
	if (m_strRawLogSelectSQL.empty())
	{
		LOGFMTE("raw gps data select query is empty!");
		Uninitialize();
		return false;
	}

	// 결과 갱신 SQL
	// 세션명으로 rawgps_update SQL 조회 (2026-07-08 최정우 주석 추가)
	m_strRawLogUpdateSQL = m_pcSQLAccessor->GetSQL(stConfig.strRawLogUpdateSession);
	if (m_strRawLogUpdateSQL.empty())
	{
		LOGFMTE("raw gps data update query is empty!");
		Uninitialize();
		return false;
	}

	// 과금 대상 INSERT SQL (세션 미지정·SQL 없으면 스킵)
	if (!stConfig.strChargeInsertSession.empty())
	{
		m_strChargeInsertSQL = m_pcSQLAccessor->GetSQL(stConfig.strChargeInsertSession);
		if (m_strChargeInsertSQL.empty())
			LOGFMTW("charge_insert session=[%s] sql is empty — charge disabled",
				stConfig.strChargeInsertSession.c_str());
	}
	else
	{
		LOGFMTW("charge_insert session not configured — charge disabled");
	}

	// 트립 종료 시 trip_end_dt UPDATE SQL (세션 미지정·SQL 없으면 비활성) (2026-08-12 최정우 추가)
	if (!stConfig.strTripEndUpdateSession.empty())
	{
		m_strTripEndUpdateSQL = m_pcSQLAccessor->GetSQL(stConfig.strTripEndUpdateSession);
		if (m_strTripEndUpdateSQL.empty())
			LOGFMTW("trip_end session=[%s] sql is empty — trip_end_dt update disabled",
				stConfig.strTripEndUpdateSession.c_str());
	}
	else
	{
		LOGFMTW("trip_end session not configured — trip_end_dt update disabled");
	}

	// [trip_abend] 로드 삭제 — TTL 마감은 [trip_end] 로 통합 (2026-10-04 최정우, 사용자 확정 — TTL 마감 권장안)
	// 트립의 기존 최대 TRIP_SEQ 조회 SQL — trip_seq 이어 매기기 (2026-10-04 최정우, 사용자 확정 — TTL 마감 권장안)
	if (!stConfig.strTripSeqMaxSession.empty())
	{
		m_strTripSeqMaxSQL = m_pcSQLAccessor->GetSQL(stConfig.strTripSeqMaxSession);
		if (m_strTripSeqMaxSQL.empty())
			LOGFMTW("trip_seqmax session=[%s] sql is empty — trip_seq continuation disabled",
				stConfig.strTripSeqMaxSession.c_str());
	}
	else
	{
		LOGFMTW("trip_seqmax session not configured — trip_seq continuation disabled");
	}

	// [trip_seqoff]/[trip_seqfin] 로드 삭제 (2026-10-04 최정우 삭제 — 사용자 지시: trip_seq 는 SQL 로 처리하지 않고 엔진이 확정할 때 등록한다)

	// 서버 상태(CPU/메모리) 하트비트 UPDATE SQL (세션 미지정·SQL 없으면 비활성) (2026-08-20 최정우 추가)
	if (!stConfig.strServerStatusSession.empty())
	{
		m_strServerStatusSQL = m_pcSQLAccessor->GetSQL(stConfig.strServerStatusSession);
		if (m_strServerStatusSQL.empty())
			LOGFMTW("server_status session=[%s] sql is empty — server status heartbeat disabled",
				stConfig.strServerStatusSession.c_str());
	}
	else
	{
		LOGFMTW("server_status session not configured — server status heartbeat disabled");
	}

	// 과금 게이트 전량 조회 SQL (세션 미지정·SQL 없으면 CChargeDataLoader 게이트 캐시 비활성) (2026-08-12 최정우 추가)
	if (!stConfig.strGateSelectSession.empty())
	{
		m_strGateSelectSQL = m_pcSQLAccessor->GetSQL(stConfig.strGateSelectSession);
		if (m_strGateSelectSQL.empty())
			LOGFMTW("gate_select session=[%s] sql is empty — gate cache disabled",
				stConfig.strGateSelectSession.c_str());
	}
	else
	{
		LOGFMTW("gate_select session not configured — gate cache disabled");
	}

	// 과금 구역 전량 조회 SQL (세션 미지정·SQL 없으면 CChargeDataLoader 구역 캐시 비활성) (2026-08-12 최정우 추가)
	if (!stConfig.strZoneSelectSession.empty())
	{
		m_strZoneSelectSQL = m_pcSQLAccessor->GetSQL(stConfig.strZoneSelectSession);
		if (m_strZoneSelectSQL.empty())
			LOGFMTW("zone_select session=[%s] sql is empty — zone cache disabled",
				stConfig.strZoneSelectSession.c_str());
	}
	else
	{
		LOGFMTW("zone_select session not configured — zone cache disabled");
	}

	// 주정차 과태료 최소 FROM_MIN 조회 SQL (세션 미지정·SQL 없으면 체류시간 임계 비활성) (2026-08-24 최정우 추가)
	if (!stConfig.strParkFineSelectSession.empty())
	{
		m_strParkFineSelectSQL = m_pcSQLAccessor->GetSQL(stConfig.strParkFineSelectSession);
		if (m_strParkFineSelectSQL.empty())
			LOGFMTW("park_fine_select session=[%s] sql is empty — park fine minimum disabled",
				stConfig.strParkFineSelectSession.c_str());
	}
	else
	{
		LOGFMTW("park_fine_select session not configured — park fine minimum disabled");
	}

	LOGFMTI("sql accessor initialize success!");

	if (m_pcSQLAccessor != nullptr)
	{
		delete m_pcSQLAccessor;
		m_pcSQLAccessor = nullptr;
	}
	LOGFMTI("sql accessor uninitialize success!");

	// PostgreSQL DB 커넥션 풀
	m_pcPostgrePool = new (std::nothrow)CPostgrePool;
	if (m_pcPostgrePool == nullptr)
	{
		LOGFMTE("db connection pool memory allocate failed!");
		Uninitialize();
		return false;
	}
	// DB 커넥션 풀 min/max 연결 초기화 (2026-07-08 최정우 주석 추가)
	if (!m_pcPostgrePool->InitializePool(m_strDBUserID, m_strDBPasswd, m_strDBName, m_strDBHost,
			m_nDBPort, m_nDBMinConnect, m_nDBMaxConnect))
	{
		LOGFMTE("db connection pool initialize failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("db connection pool initialize success!min=[%d] max=[%d]",
		m_nDBMinConnect, m_nDBMaxConnect);

	// 기반 데이터 메모리 클래스
	m_pcDataLoader = new (std::nothrow)CDataLoader;
	if (m_pcDataLoader == nullptr)
	{
		LOGFMTE("data loader memory allocate failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("data loader memory allocate success!");

	// 맵매칭 바이너리 데이터 로더 경로·maxstep 설정 (2026-07-08 최정우 주석 추가)
	m_pcDataLoader->SetHopPenalty(m_dfHopPenalty);				// (2026-08-22 최정우 추가)
	m_pcDataLoader->SetHopLenRatio(m_dfHopLenRatio);			// (2026-08-23 최정우 추가)
	m_pcDataLoader->Initialize(m_strDataFile, m_nMaxStep);
	// link.psf 등 기반 데이터 메모리 적재 (2026-07-08 최정우 주석 추가)
	if (!m_pcDataLoader->SetDataUpdate())
	{
		LOGFMTE("binary data load failed!file=[%s]", m_strDataFile.c_str());
		Uninitialize();
		return false;
	}
	// 적재된 그리드·링크 통계 콘솔 출력 (2026-07-08 최정우 주석 추가)
	m_pcDataLoader->SetDataInfoDisplay();

	// 과금 게이트 데이터 클래스 — gate_select SQL 미설정 시 비활성(치명적 실패 아님) (2026-08-12 최정우 추가)
	//   [보완] gate_select 가 비면 CChargeDataLoader 자체를 만들지 않는다 — zone_select·parkfine_select
	//   가 설정돼 있어도 함께 꺼지고, RawLogWorker 는 pcChargeDataLoader==nullptr 이라 6개 과금유형
	//   판정·적재를 전부 건너뛴다(맵매칭 결과 갱신만 수행) (2026-10-06 최정우 주석 추가)
	if (!m_strGateSelectSQL.empty())
	{
		m_pcChargeDataLoader = new (std::nothrow)CChargeDataLoader;
		if (m_pcChargeDataLoader == nullptr)
		{
			LOGFMTE("charge data loader memory allocate failed!");
			Uninitialize();
			return false;
		}

		if (!m_pcChargeDataLoader->Initialize(m_pcPostgrePool, m_strGateSelectSQL, m_strZoneSelectSQL,
				m_strParkFineSelectSQL))
		{
			LOGFMTE("charge data loader initialize failed!");
			Uninitialize();
			return false;
		}

		// 기동 시 1회 게이트 캐시 로드 — 실패해도 서버 기동은 계속(과금 판정만 비활성) (2026-08-12 최정우 추가)
		if (!m_pcChargeDataLoader->LoadGates())
			LOGFMTW("initial gate cache load failed — charge gate matching disabled until next reload");
		else
			LOGFMTI("gate cache initial load success!count=[%zu]", m_pcChargeDataLoader->GetGateCount());

		// 기동 시 1회 구역 캐시 로드 — 실패해도 서버 기동은 계속(zone_name 조회만 비활성) (2026-08-12 최정우 추가)
		//   [정정] 지금은 zone_name 만이 아니다 — 구역 캐시·link_id 역인덱스가 비어 일반도로·개방형 구간·
		//   면제·주정차(폴리곤)·폐쇄형/구간단속 거리 산출과 구역 중간진입 판정이 다음 재조회 성공까지
		//   모두 동작하지 않는다 (2026-10-06 최정우 주석 수정)
		if (!m_pcChargeDataLoader->LoadZones())
			LOGFMTW("initial zone cache load failed — zone name lookup disabled until next reload");
		else
			LOGFMTI("zone cache initial load success!count=[%zu]", m_pcChargeDataLoader->GetZoneCount());

		// 기동 시 1회 주정차 과태료 최소 FROM_MIN 로드 — 실패해도 서버 기동은 계속(체류시간 임계만 비활성)
		//   (2026-08-24 최정우 추가)
		if (!m_pcChargeDataLoader->LoadParkingFine())
			LOGFMTW("initial park fine minimum load failed — stay-time threshold disabled until next reload");
		else
			LOGFMTI("park fine minimum initial load success!min_sec=[%d]",
				m_pcChargeDataLoader->GetParkFineMinSec());

		m_dtLastGateReload = time(nullptr);
	}

	m_pcProcessManager = new (std::nothrow)CProcessManager[m_nWorkerThread];
	if (m_pcProcessManager == nullptr)
	{
		LOGFMTE("map match process manager memory allocate failed!");
		Uninitialize();
		return false;
	}

	for (int i=0; i<m_nWorkerThread; ++i)
	{
		// config alt_* → 스레드별 ProcessManager·ContinueMapMatch 고도 보조 점수
		ALTITUDE_SCORE_CONFIG stAltitudeConfig;
		stAltitudeConfig.nGap = static_cast<sint16>(m_nAltGap);
		stAltitudeConfig.nAltPenalty = static_cast<sint16>(m_nAltPenalty);
		stAltitudeConfig.dfWeight = m_dfAltWeight;
		stAltitudeConfig.dfSlope = m_dfAltSlope;

		if (!m_pcProcessManager[i].Initialize(i, m_pcDataLoader,
				m_nCoordinateType, m_nRadius, m_dwMaxDistance,
				m_dfRadiusScale, static_cast<sint16>(m_nRadiusMin),
				static_cast<sint16>(m_nRadiusMax), stAltitudeConfig))
		{
			LOGFMTE("process manager[%d] initialize failed!", i);
			Uninitialize();
			return false;
		}
	}
	LOGFMTI("map match process manager initialize success!count=[%d]", m_nWorkerThread);

	m_pcRawLogWorker = new (std::nothrow)CRawLogWorker;
	if (m_pcRawLogWorker == nullptr)
	{
		LOGFMTE("raw log worker memory allocate failed!");
		Uninitialize();
		return false;
	}

	RAWLOG_WORKER_CONFIG stWorkerConfig;
	stWorkerConfig.pcPostgrePool = m_pcPostgrePool;
	stWorkerConfig.pcProcessManager = m_pcProcessManager;
	stWorkerConfig.pcChargeDataLoader = m_pcChargeDataLoader;			// nullptr 이면 개방형 과금 판정 비활성 (2026-08-12 최정우 추가)
	//   [정정] nullptr 이면 개방형만이 아니라 6개 과금유형 전부가 비활성이다 (2026-10-06 최정우 주석 수정)
	stWorkerConfig.pcDataLoader = m_pcDataLoader;						// LINK_INFO.qwOppositeLinkID 조회용 (2026-08-21 최정우 추가)
	stWorkerConfig.strUpdateSQL = m_strRawLogUpdateSQL;
	stWorkerConfig.strChargeInsertSQL = m_strChargeInsertSQL;
	stWorkerConfig.strTripEndUpdateSQL = m_strTripEndUpdateSQL;			// (2026-08-12 최정우 추가)
	stWorkerConfig.strTripSeqMaxSQL = m_strTripSeqMaxSQL;				// (2026-10-04 최정우 추가)
	stWorkerConfig.nWorkerThreads = m_nWorkerThread;
	stWorkerConfig.nTtlSec = m_nTtlSec;
	stWorkerConfig.nMatchTimeoutMs = m_nMatchTimeout;
	stWorkerConfig.nRetryMax = m_nRetryMax;
	stWorkerConfig.nConnRetryMax = stConfig.nConnRetryMax;
	stWorkerConfig.nConnRetryWait = stConfig.nConnRetryWait;
	stWorkerConfig.nRadiusSkip = m_nRadiusSkip;
	stWorkerConfig.dfSpeedFactor = m_dfSpeedFactor;			// (2026-07-20 최정우 추가)
	stWorkerConfig.nSpeedMargin = m_nSpeedMargin;				// (2026-07-20 최정우 추가)
	// 연속 역행 스트릭 판정 (2026-07-21 최정우 수정 — dip 판정 대체)
	stWorkerConfig.nReverseConfirm = m_nReverseConfirm;
	stWorkerConfig.nOppStreakMax = m_nOppStreakMax;
	stWorkerConfig.nHeadingMaxDist = static_cast<int>(m_dwMaxDistance);	// [mapmatch] distance → live heading 거리 상한 (2026-07-15 최정우 추가)
	stWorkerConfig.nParkPad = m_nParkPad;						// (2026-08-13 최정우 추가)
	stWorkerConfig.nParkAccMax = m_nParkAccMax;					// (2026-08-23 최정우 추가)
	stWorkerConfig.nIgnoreRawVld = m_nIgnoreRawVld;				// (2026-08-23 최정우 추가)
	stWorkerConfig.nParkExitCnt = m_nParkExitCnt;				// (2026-08-13 최정우 추가)
	stWorkerConfig.nZoneExitCnt = m_nZoneExitCnt;				// (2026-09-21 최정우 추가)
	stWorkerConfig.nNodeExitCnt = m_nNodeExitCnt;				// (2026-08-24 최정우 추가)
	stWorkerConfig.nParkSpeedMax = m_nParkSpeedMax;				// (2026-08-22 최정우 추가)
	stWorkerConfig.nParkEntryCnt = m_nParkEntryCnt;				// (2026-08-22 최정우 추가)
	stWorkerConfig.nParkTtlSec = m_nParkTtlSec;					// (2026-08-19 최정우 추가)
	stWorkerConfig.nExemptRegraceSec = m_nExemptRegraceSec;		// (2026-08-14 최정우 추가)
	stWorkerConfig.nExemptOutMax = m_nExemptOutMax;				// (2026-09-23 최정우 추가)
	// 워커에 DB pool·ProcessManager·SQL·TTL·conn_retry 등 공유 설정 전달 (2026-07-10 최정우 추가)
	m_pcRawLogWorker->SetConfig(stWorkerConfig);

	CWorkerManager *pcWorkerManager = new (std::nothrow)CWorkerManager;
	if (pcWorkerManager == nullptr)
	{
		LOGFMTE("worker manager memory allocate failed!");
		Uninitialize();
		return false;
	}
	// Runnable 래퍼에 RawLogWorker 인스턴스 연결 (2026-07-08 최정우 주석 추가)
	pcWorkerManager->SetWorker(m_pcRawLogWorker);

	m_pcThreadPool = new (std::nothrow)CThreadPool(m_nWorkerThread, pcWorkerManager);
	if (m_pcThreadPool == nullptr)
	{
		LOGFMTE("worker 스레드 풀 memory allocate failed!");
		delete pcWorkerManager;
		Uninitialize();
		return false;
	}
	LOGFMTI("worker 스레드 풀 memory allocate success!count=[%d]", m_nWorkerThread);

	m_pcRawLogFetcher = new (std::nothrow)CRawLogFetcher;
	if (m_pcRawLogFetcher == nullptr)
	{
		LOGFMTE("raw log fetcher memory allocate failed!");
		Uninitialize();
		return false;
	}

	// Feeder에 DB pool·ThreadPool·폴링/backpressure 파라미터 설정 (2026-07-08 최정우 주석 추가)
	if (!m_pcRawLogFetcher->Initialize(m_pcPostgrePool, m_pcThreadPool,
			m_strRawLogSelectSQL, m_strRawLogUpdateSQL,
			m_nWorkerThread, m_nCoordinateType, &m_bRun,
			m_nFetchLimit, m_nFetchInterval,
			m_nQueuePauseCount, m_nQueueMaxCount,
			m_nQueueBusyMin, m_nQueueBusyMax))
	{
		LOGFMTE("raw log fetcher initialize failed!");
		Uninitialize();
		return false;
	}

	m_bRun = true;

	// 1초 주기 로그 보관 검사 타이머 쓰레드 기동 — 반드시 m_bRun=true 이후에 생성할 것
	//   (2026-08-14 최정우 수정 — 기존엔 초기화 극초반(로거 초기화 직후)에 생성해서, DB pool·
	//   대용량 도로망 데이터 로딩·워커풀 구성 등 시간이 걸리는 나머지 초기화 도중 새 스레드가
	//   while(IsRun()) 최초 진입 체크에서 m_bRun=false 를 관측하고 즉시 종료해버리는 레이스가
	//   있었음 — 그 결과 로그 보관정리도 gate_reload/zone 주기 재조회도 사실상 한 번도 안 도는
	//   상태였음("소스상 문제" 검토 중 발견). 스레드 생성을 m_bRun=true 확정 뒤로 옮겨 원천 차단)
	if (pthread_create(&m_hTimerThread, nullptr, TimerThread, reinterpret_cast<void *>(this)) != 0)
	{
		LOGFMTE("timer thread create failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("timer thread create success!");

	m_dtLastMonitorLog = time(nullptr);
	g_pcServerInstance = this;
	// SIGINT/SIGTERM 수신 시 우아한 종료 핸들러 등록 (2026-07-08 최정우 주석 추가)
	signal(SIGINT, ServerSignalHandler);
	signal(SIGTERM, ServerSignalHandler);

	// 좀비 PROCESSING 복구는 기동 필수 안전망.
	// 실패 시 재시도 후에도 실패면 기동 중단(fail-fast)
	bool bRecovered = false;
	int nRecoverAttempt = 1;
	for (; nRecoverAttempt<=RECOVER_RETRY_MAX; ++nRecoverAttempt)
	{
		// 기동 시 PROCESSING→PENDING 좀비 행 일괄 복구 (2026-07-08 최정우 주석 추가)
		if (CRawLogFetcher::RunRecover(m_pcPostgrePool, m_strRawLogRecoverSQL))
		{
			bRecovered = true;
			break;
		}

		LOGFMTW("raw log recover failed! attempt=[%d/%d]", nRecoverAttempt, RECOVER_RETRY_MAX);
		if (nRecoverAttempt < RECOVER_RETRY_MAX)
		{
			// 복구 재시도 전 대기 (단위: ms) (2026-07-11 최정우 수정)
			usleep(RECOVER_RETRY_INTERVAL * 1000);
		}
	}

	if (!bRecovered)
	{
		LOGFMTE("raw log recover failed after [%d] attempts! abort startup!", RECOVER_RETRY_MAX);
		Uninitialize();
		return false;
	}
	LOGFMTI("raw log recover success!attempt=[%d/%d]", nRecoverAttempt, RECOVER_RETRY_MAX);

	// RawLogFetcher 폴링 쓰레드 기동 (2026-07-08 최정우 주석 추가)
	m_pcRawLogFetcher->start();
	LOGFMTI("raw log fetcher start!limit=[%d] fetch_interval=[%d]ms "
		"queue_pause=[%d] queue_max=[%d] queue_busymin=[%d]ms queue_busymax=[%d]ms "
		"ttl_sec=[%d]s shutdown_wait=[%d]ms",
		m_nFetchLimit, m_nFetchInterval,
		m_nQueuePauseCount, m_nQueueMaxCount,
		m_nQueueBusyMin, m_nQueueBusyMax,
		m_nTtlSec, m_nShutdownWait);

	return true;
}

/**
 * @brief 메모리 반환
 * @return void
 * @remark 종료 순서(2026-10-06 최정우 주석 추가) — 순서 자체가 안전 조건이다:
 *   ① RequestShutdown()(m_bRun=false) → ② Fetcher WakeUp()+join() → ③ ThreadPool RequestShutdown()
 *   → ④ WaitForActiveIdle(shutdown_wait, 0 이면 생략) → ⑤ DrainPendingBatchesAndRelease()(큐 잔여
 *   PROCESSING→PENDING) → ⑥ WaitForAllStopped(3000) → ⑦ 전부 멈췄을 때만 FlushAllSessionsOnShutdown()
 *   → ⑧ ThreadPool delete → ⑨ RawLogWorker delete → ⑩ 타이머 스레드 join → ⑪ DataLoader·
 *   ChargeDataLoader·PostgrePool·LoggerManager·ProcessManager 해제. ⑥ 이 false 면 ⑦ 을 건너뛰고
 *   워커가 참조하는 객체 delete 를 생략한다(m_bSkipDependentTeardown). 중복 호출은 m_bUninitialized 로 무시
*/
void CServer::Uninitialize()
{
	// 중복 호출 가드: main 명시 호출(AppMain) + 소멸자 호출 이중 실행 방지 (2026-07-10 최정우 추가)
	if (m_bUninitialized)
		return;
	m_bUninitialized = true;

	// run 루프·Fetcher·Worker 종료 플래그 설정 (2026-07-08 최정우 주석 추가)
	RequestShutdown();

	if (m_pcRawLogFetcher != nullptr)
	{
		// [버그 수정, 2026-09-11 최정우] interrupt()(SIGUSR1→예외 강제 언와인드)는 CServer 가
		//   2026-07-10 에 자기 자신의 run 루프에서 이미 겪고 제거한 것과 같은 클래스의 위험을
		//   여기서만 그대로 쓰고 있었다 — WakeUp()(조건변수 기반 안전한 즉시깨움)으로 교체.
		//   RequestShutdown() 이 이미 m_bRun=false 를 세팅했으므로 WakeUp() 은 남은 대기를
		//   끊어 즉시 반응하게 할 뿐, 없어도 다음 sleep 주기 안에 스스로 종료한다.
		m_pcRawLogFetcher->WakeUp();
		m_pcRawLogFetcher->join();
		delete m_pcRawLogFetcher;
		m_pcRawLogFetcher = nullptr;
	}
	LOGFMTI("raw log fetcher uninitialize!");

	// #8: 워커 종료 — 진행 중 batch 완료 대기 후 큐 잔여는 PENDING release
	if (m_pcThreadPool != nullptr)
	{
		// ThreadPool 워커 종료 요청: 신규 Dequeue 중단 (2026-07-08 최정우 주석 추가)
		m_pcThreadPool->RequestShutdown();

		if (m_nShutdownWait > 0)
		{
			// 진행 중(활성) batch 만 대기. 큐 잔여는 워커가 더 이상 꺼내지 않음 (2026-07-11 수정)
			bool bActiveIdle = m_pcThreadPool->WaitForActiveIdle(m_nShutdownWait);
			if (!bActiveIdle)
			{
				LOGFMTW("shutdown active drain timeout!active=[%d] queue=[%d] drain_ms=[%d]",
					m_pcThreadPool->GetActiveThreads(),
					m_pcThreadPool->GetQueueCount(),
					m_nShutdownWait);
			}
		}

		int nQueuedBatches = m_pcThreadPool->GetQueueCount();
		if (nQueuedBatches > 0)
		{
			LOGFMTI("shutdown: queued batches=[%d] releasing PROCESSING→PENDING (not matching)",
				nQueuedBatches);
		}

		// 큐 잔여 batch PROCESSING→PENDING release (#8) (2026-07-08 최정우 주석 추가)
		DrainPendingBatchesAndRelease();

		// [버그 수정, 2026-09-11 최정우] delete 전에 전체 워커가 실제로 run() 을 빠져나갔는지
		//   확인한다 — 위 WaitForActiveIdle() 은 "현재 batch 처리 중" 만 보고, 대기 루프를 완전히
		//   빠져나와 종료됐는지는 못 본다. detach(운영 기본값)된 워커가 타임아웃 안에 못 멈추면
		//   지금도 m_pcRawLogWorker->run() 내부에서 m_pcRawLogWorker/m_pcProcessManager/
		//   m_pcDataLoader/m_pcChargeDataLoader/m_pcPostgrePool 을 참조 중일 수 있어, 아래에서
		//   그 객체들을 delete 하면 use-after-free 가 된다(ThreadPool.cpp 의 동일 근거 수정과 짝).
		//   m_bSkipDependentTeardown 을 세워 이번 종료에서는 그 객체들 delete 를 전부 건너뛰고
		//   의도적으로 누수시킨다 — 프로세스가 곧 종료되므로 누수가 크래시보다 안전하다.
		bool bWorkersFullyStopped = m_pcThreadPool->WaitForAllStopped(3000);

		// [보완, 2026-09-16 최정우 추가] 워커가 전부 멈춘 것이 확인된 **이 시점에만** 열려 있는
		//   과금 구간을 마감한다. 세션(m_vtTripSessions)은 in-memory 라 여기서 안 비우면 프로세스와
		//   함께 사라지고, 그 구간은 해당 트립을 통째로 재매칭하지 않는 한 복구할 수 없다
		//   (CRawLogWorker::FlushAllSessionsOnShutdown 주석 참고).
		//   순서가 중요하다 — 워커 정지 확인 **후**(세션 맵 레이스 방지), RawLogWorker·PostgrePool
		//   delete **전**(둘 다 살아 있어야 마감·DB 반영이 가능)이라 이 자리뿐이다.
		//   못 멈춘 워커가 있으면 건너뛴다: 그 워커가 아직 세션을 만지고 있을 수 있어, 마감을
		//   시도하는 쪽이 use-after-free 보다 위험하다(종전대로 유실되지만 크래시는 없다).
		if (bWorkersFullyStopped && (m_pcRawLogWorker != nullptr))
			m_pcRawLogWorker->FlushAllSessionsOnShutdown();

		delete m_pcThreadPool;
		m_pcThreadPool = nullptr;

		if (!bWorkersFullyStopped)
		{
			m_bSkipDependentTeardown = true;
			LOGFMTE("threadpool shutdown incomplete!worker(s) still running after wait — "
				"skip freeing RawLogWorker/ProcessManager/DataLoader/ChargeDataLoader/PostgrePool "
				"to avoid use-after-free(leaking intentionally, process is exiting)");
		}
	}
	LOGFMTI("worker 스레드 풀 uninitialize!");

	if (m_pcRawLogWorker != nullptr)
	{
		if (!m_bSkipDependentTeardown)
			delete m_pcRawLogWorker;
		m_pcRawLogWorker = nullptr;
	}

	if (m_pcSQLAccessor != nullptr)
	{
		delete m_pcSQLAccessor;
		m_pcSQLAccessor = nullptr;
	}

	// timer thread join
	// 타이머 스레드 종료 대기.
	// [중요, 2026-09-21 최정우 주석 보완] **이 join 의 위치는 우연이 아니라 필수 조건이다.**
	//   TimerThread → ProcessPeriodSec() 은 m_pcLoggerManager · m_pcChargeDataLoader ·
	//   m_pcPostgrePool 을 직접 참조한다. 그 셋의 delete 는 모두 이 join **아래**에 있어야
	//   안전하다(현재 그렇다). 반대로 위쪽에서 먼저 지우는 ThreadPool·RawLogWorker 는
	//   ProcessPeriodSec() 이 건드리지 않는 객체들이라 문제가 없다.
	//   ProcessPeriodSec() 에 새 의존 객체를 추가한다면, 그 객체의 해제도 반드시 이 아래로 둘 것.
	//   (RequestShutdown() 이 m_bRun=false 를 세워도 이미 진입한 그 회차는 끝까지 실행되고,
	//    LoadGates/LoadZones 같은 DB 조회가 수백 ms 걸릴 수 있어 창이 짧지 않다.)
	if (m_hTimerThread)
	{
		// [2026-09-21 최정우 정리] 종전에는 `long int nStatus` 를 void** 로 캐스팅해 넘겼다 —
		//   64비트에서 크기가 같아 동작했을 뿐, 스레드 반환값은 void* 이므로 타입을 맞춘다.
		//   TimerThread 는 항상 nullptr 을 돌려주므로 값 자체는 쓰지 않는다.
		void *pvThreadResult = nullptr;
		pthread_join(m_hTimerThread, &pvThreadResult);
		m_hTimerThread = 0;
	}
	LOGFMTI("timer thread join success!");

	if (m_pcDataLoader != nullptr)
	{
		// [버그 수정, 2026-09-11 최정우] Uninitialize() 도 내부 데이터를 해제하므로 delete 와
		//   동일하게 건너뛴다 — 근거는 위 m_pcRawLogWorker 와 동일
		if (!m_bSkipDependentTeardown)
		{
			// 맵매칭 바이너리 데이터 메모리 해제 (2026-07-08 최정우 주석 추가)
			m_pcDataLoader->Uninitialize();
			delete m_pcDataLoader;
		}
		m_pcDataLoader = nullptr;
	}
	LOGFMTI("data loader uninitialize!");

	if (m_pcChargeDataLoader != nullptr)
	{
		if (!m_bSkipDependentTeardown)
		{
			// 과금 게이트 캐시 메모리 해제 (2026-08-12 최정우 추가)
			m_pcChargeDataLoader->Uninitialize();
			delete m_pcChargeDataLoader;
		}
		m_pcChargeDataLoader = nullptr;
	}
	LOGFMTI("charge data loader uninitialize!");

	if (m_pcPostgrePool != nullptr)
	{
		// DB 커넥션 풀 연결 전부 종료 (2026-07-08 최정우 주석 추가) — UninitializePool() 자체는
		//   대여 중(checked-out) 커넥션은 안 건드리고 자체 5초 타임아웃으로 안전하게 끝나므로
		//   m_bSkipDependentTeardown 여부와 무관하게 호출해도 된다. delete 만 건너뛴다.
		m_pcPostgrePool->UninitializePool();
		if (!m_bSkipDependentTeardown)
			delete m_pcPostgrePool;
		m_pcPostgrePool = nullptr;
	}
	LOGFMTI("db connection pool uninitialize!");

	// logger manager thread stop
	//   [정정] CLoggerManager 는 자체 스레드가 없다(타이머 스레드가 LogDeleteRun() 을 호출) — 여기서는
	//   객체 해제만 한다. 타이머 스레드 join 이 위에서 끝났으므로 안전하다 (2026-10-06 최정우 주석 수정)
	if (m_pcLoggerManager) delete m_pcLoggerManager;
	m_pcLoggerManager = nullptr;

	// GPS 정보 맵 매칭 처리 클래스
	// [버그 수정, 2026-09-11 최정우] m_stConfig.pcProcessManager 로 RawLogWorker 에 전달돼
	//   워커 스레드가 참조 — 근거는 위 m_pcRawLogWorker 와 동일
	if (m_pcProcessManager != nullptr)
	{
		if (!m_bSkipDependentTeardown)
			delete [] m_pcProcessManager;
		m_pcProcessManager = nullptr;
	}
	LOGFMTI("map match process manager uninitialize!");

	if (g_pcServerInstance == this)
		g_pcServerInstance = nullptr;

	LOGFMTI("server uninitialize!");
}

/**
 * @brief 서버 run 루프 종료 요청
 * @return void
 */
void CServer::RequestShutdown()
{
	m_cRunMutex.lock();
	m_bRun = false;
	m_cRunCondition.broadcast();
	m_cRunMutex.unlock();
}

/**
 * @brief 종료 시 워커 큐 잔여 batch 예약 해제 (#8)
 * @return true(release 시도 완료), false(pool/worker/conn 없음)
 *   [정정] true 는 잔여 batch 가 없거나 전 batch release 성공일 때만이다 — 한 batch 라도 release 에
 *   실패하면 false(그 행들은 PROCESSING 으로 남아 다음 기동의 rawgps_recover 가 회수) (2026-10-06 최정우 주석 수정)
*/
bool CServer::DrainPendingBatchesAndRelease()
{
	if (m_pcThreadPool == nullptr || m_pcRawLogWorker == nullptr || m_pcPostgrePool == nullptr)
		return false;

	vector<RAW_LOG_BATCH> vtPending;
	// 종료 시 ThreadPool 큐에 남은 batch 목록 추출 (2026-07-08 최정우 주석 추가)
	m_pcThreadPool->DrainQueuedBatches(&vtPending);

	if (vtPending.empty())
		return true;

	// drain용 DB 커넥션 풀에서 획득 (2026-07-08 최정우 주석 추가)
	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("shutdown drain: db connection is null!pending_batches=[%d]",
			static_cast<int>(vtPending.size()));
		return false;
	}

	int nReleased = 0;
	int nFailed = 0;

	for (size_t i=0; i<vtPending.size(); ++i)
	{
		// 미처리 batch PROCESSING→PENDING 일괄 release (2026-07-08 최정우 주석 추가)
		if (m_pcRawLogWorker->ReleaseReservedBatch(pcConn, vtPending[i], -1))
			++nReleased;
		else
			++nFailed;
	}

	// drain용 DB 커넥션 풀 반환 (2026-07-08 최정우 주석 추가)
	m_pcPostgrePool->releaseConnection(pcConn);

	LOGFMTW("shutdown drain release done!batches=[%d] ok=[%d] fail=[%d]",
		static_cast<int>(vtPending.size()), nReleased, nFailed);

	return (nFailed == 0);
}

/**
 * @brief run 루프 대기 (종료 시그널 또는 타임아웃)
 * @return void
 */
void CServer::WaitForNextCycle()
{
	m_cRunMutex.lock();
	if ((m_bRun) && (!IsInterrupted()))
	{
		// run 루프 주기 대기 또는 종료 시그널 수신 (단위: ms) (2026-07-11 최정우 수정)
		m_cRunCondition.waitTimed(m_cRunMutex, SERVER_RUN_WAIT);
	}
	m_cRunMutex.unlock();
}

/**
 * @brief 워커·큐·DB 풀 상태 모니터링 (주기 DEBUG, 임계치 WARN)
 * @param[in] dtNow 현재 시각 (초)
 * @return void
 */
void CServer::LogMonitorStatus(time_t dtNow)
{
	if (m_pcThreadPool == nullptr)
		return;

	int nQueue = m_pcThreadPool->GetQueueCount();
	int nActive = m_pcThreadPool->GetActiveThreads();
	int nWaiting = m_pcThreadPool->GetWaitingThreads();

	LOGFMTD("monitor queue=[%d] active=[%d/%d] waiting=[%d]",
		nQueue, nActive, m_nWorkerThread, nWaiting);

	if (m_pcPostgrePool != nullptr)
	{
		LOGFMTD("monitor db pooled=[%d] active=[%d] available=[%d]",
			m_pcPostgrePool->getPooledConnections(),
			m_pcPostgrePool->getActiveConnections(),
			m_pcPostgrePool->getAvailableConnections());
	}

	if (nQueue >= m_nQueuePauseCount)
	{
		if (!m_bQueueWarnActive)
		{
			LOGFMTW("monitor queue backlog threshold!queue=[%d] threshold=[%d]",
				nQueue, m_nQueuePauseCount);
			m_bQueueWarnActive = true;
		}
	}
	else if (m_bQueueWarnActive)
	{
		LOGFMTI("monitor queue backlog recovered!queue=[%d]", nQueue);
		m_bQueueWarnActive = false;
	}

	bool bAllBusy = (m_nWorkerThread > 0) && (nActive >= m_nWorkerThread) && (nQueue > 0);
	if (bAllBusy)
	{
		if (!m_bAllBusyWarnActive)
		{
			LOGFMTW("monitor all workers busy with pending queue!queue=[%d] active=[%d]",
				nQueue, nActive);
			m_bAllBusyWarnActive = true;
		}
	}
	else if (m_bAllBusyWarnActive)
	{
		LOGFMTI("monitor worker load normalized!queue=[%d] active=[%d]", nQueue, nActive);
		m_bAllBusyWarnActive = false;
	}

	// [2026-09-21 최정우 확인 — 미사용] m_nLastQueueCount 는 여기서 쓰기만 하고 읽는 곳이
	//   전 소스에 없다. "직전 주기 대비 증감" 을 보려던 흔적으로 보인다 — 되살린다면 현재
	//   임계 기반 경고(m_bQueueWarnActive)와 중복되지 않게 용도를 먼저 정할 것.
	m_nLastQueueCount = nQueue;
	(void)dtNow;
}

/**
 * @brief Thread 실행
 * @return void
*/
void CServer::run()
{
	while (m_bRun && !IsInterrupted())
	{
		// SIGINT/SIGTERM 플래그 관찰 시 정상 스레드 컨텍스트에서 종료 요청 후 루프 탈출 (2026-07-10 최정우 추가)
		// → run() 정상 return → threadHandler 가 m_cond 브로드캐스트 → main join() 정상 복귀
		//   → pthread_cond_timedwait 강제 언와인드 없음 → 조건변수 오염/destroy hang 방지
		if (g_nShutdownRequested)
		{
			RequestShutdown();
			break;
		}

		time_t dtNow = time(nullptr);
		// 모니터 로그 주기 (단위: sec) (2026-07-11 최정우 주석 추가)
		if ((dtNow - m_dtLastMonitorLog) >= SERVER_MONITOR_INTERVAL)
		{
			// 워커·큐·DB 풀 상태 주기 모니터링 (2026-07-08 최정우 주석 추가)
			LogMonitorStatus(dtNow);
			m_dtLastMonitorLog = dtNow;
		}

		// 종료 또는 타임아웃까지 run 루프 대기 (2026-07-08 최정우 주석 추가)
		WaitForNextCycle();
	}
}

/**
 * @brief 주기적 1 초마다 실행
 * @param[in] dtNow 현재 시각 (초)
 * @return void
*/
void CServer::ProcessPeriodSec(time_t dtNow)
{
	// 중복 기동 방지 잠금 무결성 재확인 — PID 파일이 기동 중 실수로 삭제되면(rm 등) 중복 기동
	//   방지가 뚫리는 구멍이 있어(AppMain.cpp::IsSingleInstanceLockIntact 주석 참고, 실측
	//   재현 확인됨) 매초 감지·즉시 재점유해 그 노출 창을 최대 1초로 좁힌다. stat 두 번뿐인
	//   가벼운 검사라 매초 호출해도 무방 (2026-09-04 최정우 추가, 사용자 지시)
	if (!IsSingleInstanceLockIntact())
	{
		LOGFMTE("single instance pid file was missing/replaced externally! re-acquired at "
			"same path — if this was not intentional (e.g. manual rm), check for a duplicate "
			"MapMatchSvr instance that may have started in the gap!");
	}

	// 설정 시각·보관일 기준 만료 로그 파일 삭제 (2026-07-08 최정우 주석 추가)
	// [2026-09-21 최정우 보완] 아래 다른 참조들과 달리 nullptr 검사가 없었다. 타이머 스레드는
	//   로거 매니저 초기화가 끝난 뒤에야 생성되므로 현재는 항상 유효하지만, 이 함수 안에서
	//   유일하게 무방어인 자리라 맞춰둔다(동작 변화 없음).
	if (m_pcLoggerManager != nullptr)
		m_pcLoggerManager->LogDeleteRun(dtNow);

	// 게이트·구역 캐시 주기 재조회 — gate_reload=0 이면 비활성. 기존 타이머 스레드(1초 주기) 재사용,
	// 별도 스레드 없음. 워커 처리량에 영향 없도록 best-effort — 실패해도 이전 캐시 유지 (2026-08-12 최정우 추가)
	if ((m_pcChargeDataLoader != nullptr) && (m_nGateReloadSec > 0) &&
		((dtNow - m_dtLastGateReload) >= m_nGateReloadSec))
	{
		if (!m_pcChargeDataLoader->LoadGates())
			LOGFMTW("gate cache reload failed — keeping previous cache");
		else
			LOGFMTI("gate cache reload success!count=[%zu]", m_pcChargeDataLoader->GetGateCount());

		if (!m_pcChargeDataLoader->LoadZones())
			LOGFMTW("zone cache reload failed — keeping previous cache");
		else
			LOGFMTI("zone cache reload success!count=[%zu]", m_pcChargeDataLoader->GetZoneCount());

		// 주정차 과태료 최소 FROM_MIN 도 게이트·구역과 같은 주기로 재조회 (2026-08-24 최정우 추가)
		if (!m_pcChargeDataLoader->LoadParkingFine())
			LOGFMTW("park fine minimum reload failed — keeping previous threshold");
		else
			LOGFMTI("park fine minimum reload success!min_sec=[%d]",
				m_pcChargeDataLoader->GetParkFineMinSec());

		m_dtLastGateReload = dtNow;
	}

	// 좀비 PROCESSING(MATCH_STATUS=2) 운영 중 회수 — stale_sec=0 이거나 SQL 미설정이면
	//   비활성. rawgps_recover 는 기동 시 1회뿐이라 엔진이 계속 떠 있는 동안 2 에 갇힌 행(워커
	//   강제 종료·배치 실패 잔류분)을 못 푼다. 예약 시각(MATCH_RSV_DT)이 임계보다 오래된 행만
	//   PENDING(0)으로 되돌려 다음 poll 에서 재처리시킨다. 처리 중인 행은 임계 미만이라 안전.
	//   best-effort — 실패해도 다음 주기 재시도 (2026-08-29 최정우 추가)
	if ((m_nStaleSec > 0) && (!m_strStaleRecoverSQL.empty()) &&
		((dtNow - m_dtLastStaleRecover) >= m_nStaleSec))
	{
		RecoverStaleProcessing();
		m_dtLastStaleRecover = dtNow;
	}

	// CPU 사용률 샘플 — 매초 누적(직전 /proc/stat 대비 델타), 하트비트 반영 시점엔 이 최신값을
	//   그대로 사용한다(하트비트 순간에만 샘플링하면 그 찰나의 값이라 대표성이 떨어짐) (2026-08-20 최정우 추가)
	UpdateCpuSample();

	// 서버 상태(CPU/메모리) 하트비트 — status_interval=0 이거나 SQL 미설정이면 비활성.
	//   best-effort(실패해도 다음 주기에 재시도, 워커 처리 방해 금지) (2026-08-20 최정우 추가)
	if ((m_nServerStatusIntervalSec > 0) && (!m_strServerStatusSQL.empty()) &&
		((dtNow - m_dtLastServerStatusUpdate) >= m_nServerStatusIntervalSec))
	{
		UpdateServerStatus();
		m_dtLastServerStatusUpdate = dtNow;
	}
}

/**
 * @brief 좀비 PROCESSING(MATCH_STATUS=2) 운영 중 회수 [stale_recover]
 * @return void
 * @remark 예약 시각(MATCH_RSV_DT)이 m_nStaleSec 보다 오래된 행만 PENDING(0)으로 되돌린다.
 *         기동 시 1회 도는 rawgps_recover 와 달리 엔진이 떠 있는 동안에도 회수한다 —
 *         워커 강제 종료·배치 실패로 2 에 갇힌 행이 재기동까지 미처리로 남는 것을 막는다.
 *         best-effort: 실패해도 로그만 남기고 다음 주기에 재시도 (2026-08-29 최정우 추가)
*/
void CServer::RecoverStaleProcessing()
{
	if (m_strStaleRecoverSQL.empty() || (m_pcPostgrePool == nullptr))
		return;

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("stale processing recover — get connection failed!");
		return;
	}

	char szThresholdSec[16];
	snprintf(szThresholdSec, sizeof(szThresholdSec), "%d", m_nStaleSec);

	const char *pszParams[1] = { szThresholdSec };
	PGresult *pcResult = PQexecParams(pcConn, m_strStaleRecoverSQL.c_str(),
		1, nullptr, pszParams, nullptr, nullptr, 0);

	if ((pcResult == nullptr) || (PQresultStatus(pcResult) != PGRES_COMMAND_OK))
	{
		LOGFMTW("stale processing recover failed!error=[%s]", PQerrorMessage(pcConn));
	}
	else
	{
		// 회수 0 건이 정상 — 1 건이라도 나오면 그 사이 배치 실패·강제 종료가 있었다는 뜻이라
		//   WARN 으로 남겨 원인 추적이 가능하게 한다
		const char *pszAffected = PQcmdTuples(pcResult);
		const int nAffected = ((pszAffected != nullptr) && (pszAffected[0] != '\0'))
			? atoi(pszAffected) : 0;
		if (nAffected > 0)
			LOGFMTW("stale processing recovered!PROCESSING→PENDING rows=[%d] threshold=[%d]s",
				nAffected, m_nStaleSec);
		else
			LOGFMTD("stale processing recover: none rows=[0] threshold=[%d]s", m_nStaleSec);
	}

	if (pcResult != nullptr)
		PQclear(pcResult);

	m_pcPostgrePool->releaseConnection(pcConn);
}

/**
 * @brief CPU 사용률 샘플링 — /proc/stat 델타 기반, 매초 호출
 * @return void
 * @remark
 *   "cpu  user nice system idle iowait irq softirq steal ..." 누적 tick 값을 읽어 직전
 *   샘플과의 차이로 사용률(%)을 계산한다. 최초 1회는 델타를 낼 이전 값이 없어 계산을
 *   건너뛴다(m_bCpuSampleValid=false) (2026-08-20 최정우 추가)
*/
void CServer::UpdateCpuSample()
{
	FILE *fp = fopen("/proc/stat", "r");
	if (fp == nullptr)
		return;

	char szLine[256];
	memset(szLine, 0, sizeof(szLine));
	bool bRead = (fgets(szLine, sizeof(szLine), fp) != nullptr);
	fclose(fp);
	if (!bRead)
		return;

	unsigned long long qwUser=0, qwNice=0, qwSystem=0, qwIdle=0, qwIowait=0, qwIrq=0, qwSoftirq=0, qwSteal=0;
	int nMatched = sscanf(szLine, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		&qwUser, &qwNice, &qwSystem, &qwIdle, &qwIowait, &qwIrq, &qwSoftirq, &qwSteal);
	if (nMatched < 4)						// 최소 user/nice/system/idle 은 있어야 함
		return;

	unsigned long long qwIdleAll = qwIdle + qwIowait;
	unsigned long long qwTotal = qwUser + qwNice + qwSystem + qwIdleAll + qwIrq + qwSoftirq + qwSteal;

	if (m_bCpuSampleValid && (qwTotal > m_qwPrevCpuTotal))
	{
		unsigned long long qwTotalDiff = qwTotal - m_qwPrevCpuTotal;
		unsigned long long qwIdleDiff = (qwIdleAll >= m_qwPrevCpuIdle) ? (qwIdleAll - m_qwPrevCpuIdle) : 0;
		m_dfLastCpuPct = 100.0 * static_cast<double>(qwTotalDiff - qwIdleDiff) / static_cast<double>(qwTotalDiff);
	}

	m_qwPrevCpuTotal = qwTotal;
	m_qwPrevCpuIdle = qwIdleAll;
	m_bCpuSampleValid = true;
}

/**
 * @brief 메모리 사용률·용량 조회 — /proc/meminfo 기준
 * @param[out] nUsedMB 사용 메모리(MB) — MemTotal-MemAvailable
 * @param[out] nTotalMB 전체 메모리(MB)
 * @param[out] dfMemPct 사용률(%)
 * @return true(성공), false(읽기 실패)
 * @warning **이 값은 이 프로세스가 아니라 장비 전체(시스템)의 메모리 사용량이다.**
 *   /proc/meminfo 는 호스트 전역 통계이므로, 다른 프로세스가 메모리를 먹어도 여기 수치가
 *   올라간다. 위 UpdateCpuSample() 의 CPU 사용률(/proc/stat)도 마찬가지로 전역 값이다.
 *   그래서 서버상태 로그의 mem/cpu 가 높다는 것만으로 **맵매칭 엔진이 메모리를 누수한다고
 *   판단하면 안 된다** — 2026-09-10 에 실제로 그렇게 오진단한 적이 있고, 진짜 원인은 다른
 *   프로세스가 유발한 OOM kill 이었다. 엔진 자신의 사용량이 필요하면 /proc/self/status 의
 *   VmRSS 를 따로 읽어야 한다 (2026-09-21 최정우 주석 보완)
*/
bool CServer::GetMemInfo(int& nUsedMB, int& nTotalMB, double& dfMemPct)
{
	FILE *fp = fopen("/proc/meminfo", "r");
	if (fp == nullptr)
		return false;

	long lMemTotalKB = -1, lMemAvailableKB = -1;
	char szLine[256];
	while (fgets(szLine, sizeof(szLine), fp) != nullptr)
	{
		if (strncmp(szLine, "MemTotal:", 9) == 0)
			sscanf(szLine + 9, "%ld", &lMemTotalKB);
		else if (strncmp(szLine, "MemAvailable:", 13) == 0)
			sscanf(szLine + 13, "%ld", &lMemAvailableKB);

		if ((lMemTotalKB >= 0) && (lMemAvailableKB >= 0))
			break;
	}
	fclose(fp);

	if ((lMemTotalKB <= 0) || (lMemAvailableKB < 0))
		return false;

	nTotalMB = static_cast<int>(lMemTotalKB / 1024);
	long lUsedKB = lMemTotalKB - lMemAvailableKB;
	if (lUsedKB < 0) lUsedKB = 0;
	nUsedMB = static_cast<int>(lUsedKB / 1024);
	dfMemPct = 100.0 * static_cast<double>(lUsedKB) / static_cast<double>(lMemTotalKB);
	return true;
}

/**
 * @brief 서버 상태(CPU/메모리) 하트비트 — PROC_SERVERSTATUS 1행 UPDATE
 * @return void
 * @remark best-effort — 연결/쿼리 실패해도 에러 로그만 남기고 다음 주기에 재시도 (2026-08-20 최정우 추가)
*/
void CServer::UpdateServerStatus()
{
	if (m_strServerStatusSQL.empty() || (m_pcPostgrePool == nullptr))
		return;

	int nUsedMB = 0, nTotalMB = 0;
	double dfMemPct = 0.0;
	if (!GetMemInfo(nUsedMB, nTotalMB, dfMemPct))
	{
		LOGFMTW("server status update skipped — meminfo read failed");
		return;
	}

	char szCpuPct[32], szMemPct[32], szUsedMB[16], szTotalMB[16];
	snprintf(szCpuPct, sizeof(szCpuPct), "%.1f", m_dfLastCpuPct);
	snprintf(szMemPct, sizeof(szMemPct), "%.1f", dfMemPct);
	snprintf(szUsedMB, sizeof(szUsedMB), "%d", nUsedMB);
	snprintf(szTotalMB, sizeof(szTotalMB), "%d", nTotalMB);

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("server status update — get connection failed!");
		return;
	}

	const char *pszParams[5] = { m_strServerId.c_str(), szCpuPct, szMemPct, szUsedMB, szTotalMB };
	PGresult *pcResult = PQexecParams(pcConn, m_strServerStatusSQL.c_str(),
		5, nullptr, pszParams, nullptr, nullptr, 0);

	if ((pcResult == nullptr) || (PQresultStatus(pcResult) != PGRES_COMMAND_OK))
	{
		LOGFMTW("server status update failed!error=[%s]", PQerrorMessage(pcConn));
	}
	else
	{
		LOGFMTI("server status update success!id=[%s] cpu=[%s%%] mem=[%s%%] used=[%sMB] total=[%sMB]",
			m_strServerId.c_str(), szCpuPct, szMemPct, szUsedMB, szTotalMB);
	}

	if (pcResult != nullptr)
		PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);
}
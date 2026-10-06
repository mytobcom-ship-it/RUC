/**
 * @file RawLogFetcher.cpp
 * @brief 원시 GPS 로그 DB 폴링 및 워커 큐 적재 클래스 소스 파일
*/
#include "RawLogFetcher.h"
#include "Util.h"
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <libpq-fe.h>

/**
 * @brief 생성자
*/
CRawLogFetcher::CRawLogFetcher() :
	m_pcPostgrePool(nullptr),
	m_pcThreadPool(nullptr),
	m_pbRun(nullptr),
	m_nWorkerThreads(0),
	m_nCoordinateType(WGS84GEO),
	m_nFetchLimit(CFG_DEF_LIMIT),
	m_nFetchInterval(CFG_DEF_FETCH_INTVL),
	m_nQueuePauseCount(CFG_DEF_Q_PAUSE_CNT),
	m_nQueueMaxCount(CFG_DEF_Q_MAX_CNT),
	m_nQueueBusyMin(CFG_DEF_Q_BUSY_MIN),
	m_nQueueBusyMax(CFG_DEF_Q_BUSY_MAX)
{
	setName("RawLogFetcher");
}

/**
 * @brief 소멸자
*/
CRawLogFetcher::~CRawLogFetcher()
{
}

/**
 * @brief Feeder 초기화 – DB pool, ThreadPool, poll/backpressure 설정
 * @param[in] pcPostgrePool DB 커넥션 풀
 * @param[in] pcThreadPool 워커 ThreadPool (Enqueue 대상)
 * @param[in] strSelectSQL [rawgps_select] SQL (UPDATE RETURNING)
 * @param[in] strUpdateSQL [rawgps_update] 파싱 실패 예약 해제용 (Worker 와 동일)
 * @param[in] nWorkerThreads 워커 스레드 수 (device_key hash % N)
 * @param[in] nCoordinateType GPS 측지계 (ParseRow 시 RAW_LOG_INFO 에 설정)
 * @param[in] pbRun 서버 실행 플래그 (false 시 run 루프 종료)
 * @param[in] nFetchLimit 1회 조회·예약 최대 건수 (건)
 * @param[in] nFetchInterval 큐 여유 시 DB 조회 간격 (ms)
 * @param[in] nQueuePauseCount 큐 batch 수, 이상이면 DB 조회 중단 (건)
 * @param[in] nQueueMaxCount 큐 더 차면 대기 최대 구간 (건)
 * @param[in] nQueueBusyMin 큐 혼잡 시 조회 대기 최소 (ms)
 * @param[in] nQueueBusyMax 큐 혼잡 시 조회 대기 최대 (ms)
 * @return true(성공), false(실패)
*/
bool CRawLogFetcher::Initialize(CPostgrePool *pcPostgrePool, CThreadPool *pcThreadPool,
		const string& strSelectSQL, const string& strUpdateSQL,
		int nWorkerThreads, uint8 nCoordinateType,
		volatile bool *pbRun,
		int nFetchLimit, int nFetchInterval,
		int nQueuePauseCount, int nQueueMaxCount,
		int nQueueBusyMin, int nQueueBusyMax)
{
	if (pcPostgrePool == nullptr || pcThreadPool == nullptr || pbRun == nullptr)
		return false;

	if (strSelectSQL.empty() || strUpdateSQL.empty())
		return false;

	if (nWorkerThreads <= 0)
		return false;

	if (nFetchLimit <= 0)
		nFetchLimit = CFG_DEF_LIMIT;

	if (nFetchInterval < 0)
		nFetchInterval = CFG_DEF_FETCH_INTVL;

	if (nQueuePauseCount <= 0)
		nQueuePauseCount = CFG_DEF_Q_PAUSE_CNT;

	if (nQueueMaxCount < nQueuePauseCount)
		nQueueMaxCount = nQueuePauseCount;

	if (nQueueBusyMin < nFetchInterval)
		nQueueBusyMin = nFetchInterval;

	if (nQueueBusyMax < nQueueBusyMin)
		nQueueBusyMax = nQueueBusyMin;

	m_pcPostgrePool = pcPostgrePool;
	m_pcThreadPool = pcThreadPool;
	m_strSelectSQL = strSelectSQL;
	m_strUpdateSQL = strUpdateSQL;
	m_nWorkerThreads = nWorkerThreads;
	m_nCoordinateType = nCoordinateType;
	m_pbRun = pbRun;
	m_nFetchLimit = nFetchLimit;
	m_nFetchInterval = nFetchInterval;
	m_nQueuePauseCount = nQueuePauseCount;
	m_nQueueMaxCount = nQueueMaxCount;
	m_nQueueBusyMin = nQueueBusyMin;
	m_nQueueBusyMax = nQueueBusyMax;

	return true;
}

/**
 * @brief 기동 시 1회 PROCESSING → PENDING 전량 복구 [rawgps_recover]
 * @param[in] pcPostgrePool DB 커넥션 풀
 * @param[in] strRecoverSQL 복구 SQL
 * @return true(UPDATE 성공), false(실패)
*/
bool CRawLogFetcher::RunRecover(CPostgrePool *pcPostgrePool,
		const string& strRecoverSQL)
{
	if (pcPostgrePool == nullptr || strRecoverSQL.empty())
		return false;

	// 복구 SQL 실행용 DB 커넥션 풀에서 획득 (2026-07-08 최정우 주석 추가)
	PGconn *pcConn = pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("raw log recover: db connection is null!");
		return false;
	}

	// PROCESSING→PENDING 복구 UPDATE 실행 (2026-07-08 최정우 주석 추가)
	PGresult *pcResult = PQexec(pcConn, strRecoverSQL.c_str());

	if (pcResult == nullptr)
	{
		LOGFMTE("raw log recover: exec failed! error=[%s]", PQerrorMessage(pcConn));
		// 복구 실패 시 DB 커넥션 풀 반환 (2026-07-08 최정우 주석 추가)
		pcPostgrePool->releaseConnection(pcConn);
		return false;
	}

	ExecStatusType nStatus = PQresultStatus(pcResult);
	bool bOk = (nStatus == PGRES_COMMAND_OK);
	if (!bOk)
	{
		LOGFMTE("raw log recover: error! msg=[%s]", PQresultErrorMessage(pcResult));
	}
	else
	{
		char *pszAffected = PQcmdTuples(pcResult);
		LOGFMTI("raw log recover: PROCESSING→PENDING rows=[%s]",
			(pszAffected != nullptr) ? pszAffected : "0");
	}

	// PQ 결과 메모리 해제 (2026-07-08 최정우 주석 추가)
	PQclear(pcResult);
	// 복구 완료 후 DB 커넥션 풀 반환 (2026-07-08 최정우 주석 추가)
	pcPostgrePool->releaseConnection(pcConn);
	return bOk;
}

/**
 * @brief Feeder 메인 루프 – poll / backpressure / 적응형 sleep
 * @return void
 * @remark
 *   - queue < queue_pause: FetchAndDispatch() 수행
 *   - queue >= queue_pause: DB 조회 건너뜀, sleep 만 증가
 *   - 매 사이클 끝 ComputeFetchSleepMs() 만큼 대기
*/
void CRawLogFetcher::run()
{
	while (m_pbRun != nullptr && *m_pbRun && !IsInterrupted())
	{
		int nQueueCount = 0;
		if (m_pcThreadPool != nullptr)
			// backpressure 판단용 워커 큐 batch 수 조회 (2026-07-08 최정우 주석 추가)
			nQueueCount = m_pcThreadPool->GetQueueCount();

		if (nQueueCount < m_nQueuePauseCount)
			// 1회 DB 예약·조회 후 워커 큐 적재 (2026-07-08 최정우 주석 추가)
			FetchAndDispatch();
		else
		{
			LOGFMTD("raw log fetcher: backpressure skip fetch!queue=[%d] queue_pause=[%d]",
				nQueueCount, m_nQueuePauseCount);
		}

		if (m_pbRun == nullptr || !(*m_pbRun) || IsInterrupted())
			break;

		int nSleepMs = ComputeFetchSleepMs(nQueueCount);
		// 큐 적재량에 따른 적응형 poll 대기 — 조건변수 기반 대기로 WakeUp() 이 즉시 깨울 수 있다
		//   (2026-07-08 최정우 주석 추가, 2026-09-11 최정우 수정 — CUtil::Sleep(select 기반) +
		//   CSingleThread::interrupt() 조합 대신. 자세한 배경은 헤더 WakeUp() 주석 참고)
		//   [주석 정정, 2026-09-21 최정우] 위 "(SIGUSR1→예외)" 표기를 지웠다 — 같은 날
		//   CSingleThread::interruptHandler() 가 **예외를 던지지 않고 플래그만 세우도록** 바뀌어
		//   더 이상 사실이 아니다(시그널 핸들러 안 throw 는 UB 였다). 다만 그 조합을 쓰지 않는
		//   이 결정 자체는 그대로 유효하다 — 조건변수 대기가 더 정확하고 즉시 깨울 수 있다.
		m_cSleepMutex.lock();
		if ((m_pbRun != nullptr) && *m_pbRun && !IsInterrupted())
			m_cSleepCondition.waitTimed(m_cSleepMutex, nSleepMs);
		m_cSleepMutex.unlock();
	}
}

/**
 * @brief run() 의 적응형 대기를 즉시 깨움 — 종료 시 남은 sleep 시간을 기다리지 않게 한다
 * @return void
*/
void CRawLogFetcher::WakeUp()
{
	m_cSleepMutex.lock();
	m_cSleepCondition.broadcast();
	m_cSleepMutex.unlock();
}

/**
 * @brief 큐 적재량에 따른 DB 조회 대기 시간 (적응형)
 * @param[in] nQueueCount 워커 큐 전체 적재 batch 수
 * @return sleep ms
 * @remark
 *   - queue < queue_pause : fetch_interval
 *   - queue_pause <= queue < queue_max : queue_busymin ~ queue_busymax 선형 증가
 *   - queue >= queue_max : queue_busymax
*/
int CRawLogFetcher::ComputeFetchSleepMs(int nQueueCount) const
{
	if (nQueueCount < m_nQueuePauseCount)
		return m_nFetchInterval;

	if (nQueueCount >= m_nQueueMaxCount)
		return m_nQueueBusyMax;

	int nRange = m_nQueueMaxCount - m_nQueuePauseCount;
	int nExcess = nQueueCount - m_nQueuePauseCount;
	int nSpan = m_nQueueBusyMax - m_nQueueBusyMin;

	if (nRange <= 0 || nSpan <= 0)
		return m_nQueueBusyMin;

	return m_nQueueBusyMin + (nExcess * nSpan / nRange);
}

/**
 * @brief 1회 poll – DB 예약·조회 후 trip_id batch 를 워커 큐에 적재
 * @return true(1건 이상 예약·Enqueue), false(없음·오류)
*/
bool CRawLogFetcher::FetchAndDispatch()
{
	if (m_pcPostgrePool == nullptr || m_pcThreadPool == nullptr)
		return false;

	// fetch·디스패치용 DB 커넥션 풀에서 획득 (2026-07-08 최정우 주석 추가)
	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("raw log fetcher: db connection is null!");
		return false;
	}

	vector<sRawLogInfo> vtRawLogInfos;
	// rawgps_select UPDATE RETURNING 으로 PENDING 예약·행 조회 (2026-07-08 최정우 주석 추가)
	if (!ReserveFetchBatch(pcConn, &vtRawLogInfos))
	{
		// 예약 실패 시 ROLLBACK 후 커넥션 반환 (2026-07-08 최정우 주석 추가)
		ReleaseConnection(pcConn);
		return false;
	}

	// fetch 완료 후 DB 커넥션 반환 (2026-07-08 최정우 주석 추가)
	ReleaseConnection(pcConn);

	if (vtRawLogInfos.empty())
		return false;

	// GroupByTripId 는 device_key/trip_id/gps_seq 순 정렬을 전제로 인접 구간만 묶는데,
	//   [rawgps_select]의 ORDER BY는 LIMIT 대상을 고르는 내부 서브쿼리에만 있어 바깥 UPDATE...
	//   RETURNING 출력 순서까지 보장하지 않음(PostgreSQL 실행계획이 서브쿼리 정렬을 그대로 안 지킬
	//   수 있음) — 여기서 직접 재정렬해 그 가정을 코드로 보장(2026-08-14 최정우 수정 — "소스상 문제"
	//   검토 중 발견). 실제 처리 순서를 정하는 곳은 SQL 이 아니라 여기다.
	//
	//   2026-08-23 최정우 수정 — 정렬 키에서 GPS_DT 제거. 트립 안에서 순서의 정본은 GPS_SEQ 다.
	//     실측 000376_20260819140856: seq19(도착 이벤트 행)의 GPS_DT 가 직전 seq18 보다 3초 이르고
	//     좌표도 seq17 과 같았다. GPS_DT 를 앞에 두는 바람에 워커에 17->19->18 순으로 들어갔고,
	//     NeedsBeginReset() 이 seq18 을 역전(rollback)으로 판정 -> 세션 앵커 폐기 -> BEGIN 강등.
	//     BEGIN 은 heading 을 안 보고 거리만 보므로(bIgnoreHeading) 왕복분리 도로에서 10m 옆
	//     건너편 차로(2.80m)가 정답 차로(7.12m)를 이겨 오매칭됐다.
	//     GPS_SEQ 로만 정렬하면 17->18->19 가 되어 seq18 은 정상 처리되고, 뒤로 튄 중복 좌표인
	//     seq19 는 기존 역행 판정(reverse_confirm)이 걸러낸다.
	//   ※ [rawgps_select] 의 ORDER BY 도 같이 맞춰뒀다 — 어느 쪽만 봐도 의도가 읽히게.
	std::stable_sort(vtRawLogInfos.begin(), vtRawLogInfos.end(),
		[](const sRawLogInfo& stLeft, const sRawLogInfo& stRight) -> bool
		{
			int nDeviceCmp = strcmp(stLeft.szDeviceKey, stRight.szDeviceKey);
			if (nDeviceCmp != 0) return nDeviceCmp < 0;
			int nTripCmp = strcmp(stLeft.szTripID, stRight.szTripID);
			if (nTripCmp != 0) return nTripCmp < 0;
			return stLeft.dwSeqNo < stRight.dwSeqNo;
		});

	vector<RAW_LOG_BATCH> vtBatches;
	// 동일 trip_id 연속 구간을 batch 로 묶음 (2026-07-08 최정우 주석 추가)
	GroupByTripId(vtRawLogInfos, &vtBatches);
	// device_key hash 기준 워커 큐에 Enqueue (2026-07-08 최정우 주석 추가)
	DispatchBatches(vtBatches);

	LOGFMTD("raw log fetcher: reserved=[%d], batches=[%d]",
		static_cast<int>(vtRawLogInfos.size()), static_cast<int>(vtBatches.size()));

	return true;
}

/**
 * @brief [rawgps_select] UPDATE RETURNING – PENDING 예약(Reserve) 및 행 조회
 * @param[in] pcConn DB 커넥션
 * @param[out] pvtRawLogInfos 예약된 RAW_LOG_INFO 목록 (**UPDATE ... RETURNING 이 돌려준 순서
 *             그대로** — 정렬은 호출측 FetchAndDispatch 가 따로 한다)
 *             [주석 정정, 2026-09-21 최정우] 종전에는 "SQL ORDER BY 순" 이라고 적혀 있었으나,
 *             [rawgps_select] 의 ORDER BY 는 LIMIT 대상을 고르는 내부 서브쿼리에만 있어 바깥
 *             RETURNING 의 출력 순서를 보장하지 않는다(그래서 호출측이 재정렬한다). 이 설명을
 *             믿고 호출측 정렬을 지우면 배치 묶음이 깨진다.
 * @return true(예약·파싱 완료), false(SQL 오류·release 실패)
 * @remark
 *   - $1=LIMIT
 *   - ParseRow 실패 시 해당 행만 건너뛰고 나머지 디스패치 (#4 orphan 방지)
 *   - 실패 행은 [rawgps_update] $4=0 으로 PROCESSING→PENDING 즉시 release
 *     [정정] MATCH_STATUS 는 $7 이다($4 는 MATCH_LON) — ReleaseReservedRows() 가 $7="0" 으로 보낸다
 *     (2026-10-06 최정우 주석 수정)
*/
bool CRawLogFetcher::ReserveFetchBatch(PGconn *pcConn, vector<sRawLogInfo> *pvtRawLogInfos)
{
	if (pcConn == nullptr || pvtRawLogInfos == nullptr)
		return false;

	pvtRawLogInfos->clear();

	char szLimit[16];
	snprintf(szLimit, sizeof(szLimit), "%d", m_nFetchLimit);

	const char *pszParams[1] = { szLimit };
	const int nParamLengths[1] = { static_cast<int>(strlen(szLimit)) };
	const int nParamFormats[1] = { 0 };

	// rawgps_select 파라미터 바인딩 실행 ($1=LIMIT) (2026-07-08 최정우 주석 추가)
	PGresult *pcResult = PQexecParams(pcConn, m_strSelectSQL.c_str(),
		1, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
	{
		LOGFMTE("raw log fetcher: rawgps_select failed! error=[%s]", PQerrorMessage(pcConn));
		return false;
	}

	ExecStatusType nStatus = PQresultStatus(pcResult);
	if (nStatus != PGRES_TUPLES_OK)
	{
		LOGFMTE("raw log fetcher: rawgps_select error! status=[%d], msg=[%s]",
			static_cast<int>(nStatus), PQresultErrorMessage(pcResult));
		PQclear(pcResult);
		return false;
	}

	int nRows = PQntuples(pcResult);
	vector<string> vtFailTripId;
	vector<string> vtFailGpsSeq;
	vtFailTripId.reserve(static_cast<size_t>(nRows));
	vtFailGpsSeq.reserve(static_cast<size_t>(nRows));

	for (int i=0; i<nRows; ++i)
	{
		sRawLogInfo stRawLogInfo;
		// RETURNING 1행 → sRawLogInfo 변환 (2026-07-08 최정우 주석 추가)
		if (!ParseRow(pcResult, i, &stRawLogInfo))
		{
			string strTripId;
			string strGpsSeq;
			// 파싱 실패 행 PK 추출 (release 용) (2026-07-08 최정우 주석 추가)
			if (ExtractRowPk(pcResult, i, &strTripId, &strGpsSeq))
			{
				LOGFMTW("raw log fetcher: parse fail!trip_id=[%s] gps_seq=[%s]",
					strTripId.c_str(), strGpsSeq.c_str());
				vtFailTripId.push_back(strTripId);
				vtFailGpsSeq.push_back(strGpsSeq);
			}
			else
			{
				LOGFMTW("raw log fetcher: parse fail!row=[%d] pk unavailable (PROCESSING orphan until recover)",
					i);
			}
			continue;
		}

		pvtRawLogInfos->push_back(stRawLogInfo);
	}

	PQclear(pcResult);

	if (!vtFailTripId.empty())
	{
		// 파싱 실패 행 PROCESSING→PENDING 즉시 release (2026-07-08 최정우 주석 추가)
		if (!ReleaseReservedRows(pcConn,
				vtFailTripId.data(), vtFailGpsSeq.data(),
				vtFailTripId.size()))
		{
			// release 실패해도 이미 파싱 성공한 나머지 행(pvtRawLogInfos)까지 통째로 버리면 안 됨 —
			//   여기서 false 를 반환하면 호출측(FetchAndDispatch)이 이미 정상 파싱된 행까지 전부
			//   디스패치 안 하고 버려서, PROCESSING 예약만 된 채 다음 재시작(RunRecover)까지 고아로
			//   남는 범위가 "파싱 실패한 몇 건"에서 "이번 배치 전체"로 커지는 문제였음(2026-08-14
			//   최정우 수정 — "소스상 문제" 검토 중 발견). 실패한 소수 행만 PK 미확보 케이스(바로
			//   위 분기)와 동일하게 고아로 남기고, 나머지는 정상 진행
			LOGFMTE("raw log fetcher: parse-fail release failed!count=[%d] (PROCESSING orphan until "
				"recover, parsed rows still dispatched)",
				static_cast<int>(vtFailTripId.size()));
		}
		else
		{
			LOGFMTW("raw log fetcher: parse-fail released!PROCESSING→PENDING count=[%d]",
				static_cast<int>(vtFailTripId.size()));
		}
	}

	return true;
}

/**
 * @brief 연속 동일 trip_id 구간을 RAW_LOG_BATCH 로 묶음
 * @param[in] vtRawLogInfos SQL 정렬 순 RAW_LOG_INFO 목록
 *            [정정] SQL 순서가 아니라 FetchAndDispatch() 의 stable_sort(device_key·trip_id·gps_seq)
 *            결과 순이다 — RETURNING 출력 순서는 보장되지 않는다 (2026-10-06 최정우 주석 수정)
 * @param[out] pvtBatches trip_id 별 batch 목록
 * @return void
 * @remark 입력은 device_key, trip_id, **gps_seq** 순 정렬 가정 — 인접한 같은 trip_id 구간만 묶으므로
 *   정렬이 깨지면 같은 트립이 여러 batch 로 쪼개져 서로 다른 처리 순서에 놓인다.
 *   [주석 정정, 2026-09-21 최정우] 종전에는 정렬 키에 gps_dt 가 들어 있다고 적혀 있었는데,
 *   **그 키는 2026-08-23 에 의도적으로 제거된 것**이다(FetchAndDispatch 의 stable_sort 주석 참고).
 *   GPS_DT 를 GPS_SEQ 앞에 두는 바람에 도착 이벤트 행이 앞으로 당겨져 seq 역전으로 오인되고,
 *   세션 앵커가 폐기되며 BEGIN 강등 → 왕복분리 도로 오매칭까지 간 실측 사례
 *   (000376_20260819140856)가 그 제거의 이유다. 이 주석만 옛 상태로 남아 있어, 읽고
 *   "그럼 SQL 이나 정렬에 gps_dt 를 다시 넣어야겠다" 로 오도될 수 있었다.
*/
void CRawLogFetcher::GroupByTripId(const vector<sRawLogInfo>& vtRawLogInfos, vector<RAW_LOG_BATCH> *pvtBatches)
{
	if ((pvtBatches == nullptr) || (vtRawLogInfos.empty()))
		return;

	pvtBatches->clear();

	auto isSameBatch = [](const sRawLogInfo& stLeft, const sRawLogInfo& stRight) -> bool
	{
		if (strcmp(stLeft.szDeviceKey, stRight.szDeviceKey) != 0)
			return false;

		return (strcmp(stLeft.szTripID, stRight.szTripID) == 0);
	};

	RAW_LOG_BATCH vtCurrentBatch;
	vtCurrentBatch.push_back(vtRawLogInfos[0]);

	for (size_t i=1; i<vtRawLogInfos.size(); ++i)
	{
		if (isSameBatch(vtCurrentBatch.back(), vtRawLogInfos[i]))
		{
			vtCurrentBatch.push_back(vtRawLogInfos[i]);
		}
		else
		{
			pvtBatches->push_back(vtCurrentBatch);
			vtCurrentBatch.clear();
			vtCurrentBatch.push_back(vtRawLogInfos[i]);
		}
	}

	if (!vtCurrentBatch.empty())
		pvtBatches->push_back(vtCurrentBatch);
}

/**
 * @brief batch 를 hash(device_key) % N 워커 큐에 Enqueue
 * @param[in] vtBatches trip_id batch 목록 (라우팅은 device_key 기준)
 * @return void
*/
void CRawLogFetcher::DispatchBatches(const vector<RAW_LOG_BATCH>& vtBatches)
{
	for (size_t i=0; i<vtBatches.size(); ++i)
	{
		if (vtBatches[i].empty())
			continue;

		int nWorkerId = GetWorkerId(vtBatches[i][0].szDeviceKey);
		// hash(device_key) % N 워커 큐에 batch 적재 (2026-07-08 최정우 주석 추가)
		m_pcThreadPool->Enqueue(nWorkerId, vtBatches[i]);
	}
}

/**
 * @brief device_key → 고정 워커 ID (hash % 워커 수)
 * @param[in] pszDeviceKey 디바이스 키
 * @return 워커(큐) 인덱스 (0 ~ nWorkerThreads-1)
*/
int CRawLogFetcher::GetWorkerId(const char *pszDeviceKey) const
{
	if ((pszDeviceKey == nullptr) || (pszDeviceKey[0] == '\0'))
		return 0;

	size_t nHash = hash<string>()(string(pszDeviceKey));
	return static_cast<int>(nHash % static_cast<size_t>(m_nWorkerThreads));
}

/**
 * @brief 커넥션 반환 전 미완료 트랜잭션 ROLLBACK
 * @param[in] pcConn DB 커넥션
 * @return void
*/
void CRawLogFetcher::ReleaseConnection(PGconn *pcConn)
{
	if ((pcConn == nullptr) || (m_pcPostgrePool == nullptr))
		return;

	PGTransactionStatusType nTxnStatus = PQtransactionStatus(pcConn);
	if (nTxnStatus == PQTRANS_INTRANS || nTxnStatus == PQTRANS_INERROR)
		// 미완료 트랜잭션 ROLLBACK (2026-07-08 최정우 주석 추가)
		PQexec(pcConn, "ROLLBACK");

	// DB 커넥션 풀 반환 (2026-07-08 최정우 주석 추가)
	m_pcPostgrePool->releaseConnection(pcConn);
}

/**
 * @brief rawgps_select RETURNING 컬럼 인덱스 (= PRIM_RAWGPS 컬럼 순서, 2026-07-10)
*/
namespace {
enum RawGpsReturningCol
{
	RGC_TRIP_ID						= 0,
	RGC_GPS_SEQ,
	RGC_DEVICE_KEY,
	RGC_GPS_DT,
	RGC_TRIP_EVENT,
	RGC_DRIVE_STATUS,
	RGC_GPS_LAT,
	RGC_MATCH_LAT,
	RGC_GPS_LON,
	RGC_MATCH_LON,
	RGC_INTERSECT_LEN,					// GPS↔세그먼트 교차점 거리(m)
	RGC_RAW_VLD,
	RGC_SPEED_KMH,
	RGC_OBD_SPEED_KMH,					// 차량 OBD 순간속도 — NULL 아니면 SPEED_KMH 보다 우선 (2026-08-11 최정우 추가)
	RGC_HEADING,
	RGC_ALTITUDE_M,
	RGC_ACCURACY_M,
	RGC_BATTERY,
	RGC_RECV_DT,
	RGC_MATCH_STATUS
};
} // namespace

/**
 * @brief PostgreSQL text[] 리터럴용 문자열 이스케이프 (rawgps_update 배열 파라미터)
 * @param[in] strValue 원본 문자열
 * @return 이스케이프된 문자열
*/
string CRawLogFetcher::EscapePgArrayText(const string& strValue)
{
	string strEscaped;
	strEscaped.reserve(strValue.size() + 4);

	for (size_t i=0; i<strValue.size(); ++i)
	{
		const char c = strValue[i];
		if (c == '\\' || c == '"')
			strEscaped += '\\';
		strEscaped += c;
	}

	return strEscaped;
}

/**
 * @brief PostgreSQL text[] 리터럴 생성
 * @param[in] vtValues text 배열 원소 목록
 * @return PostgreSQL text[] 리터럴 (예: {"a","b"})
*/
string CRawLogFetcher::BuildPgTextArray(const vector<string>& vtValues)
{
	string strArray = "{";
	for (size_t i=0; i<vtValues.size(); ++i)
	{
		if (i > 0)
			strArray += ",";
		strArray += "\"";
		// text[] 원소 PostgreSQL 이스케이프 (2026-07-08 최정우 주석 추가)
		strArray += EscapePgArrayText(vtValues[i]);
		strArray += "\"";
	}
	strArray += "}";
	return strArray;
}

/**
 * @brief RETURNING 행에서 PK 추출 (ParseRow 실패 시 release 용)
 * @param[in] pcResult PQ 결과
 * @param[in] nRow 행 인덱스
 * @param[out] pstrTripId TRIP_ID
 * @param[out] pstrGpsSeq GPS_SEQ
 * @return true(추출 성공), false(null·PK NULL)
 * @remark TRIP_ID·GPS_SEQ 가 NULL 이면 false — 복구 만 가능
*/
bool CRawLogFetcher::ExtractRowPk(PGresult *pcResult, int nRow,
		string *pstrTripId, string *pstrGpsSeq)
{
	if (pcResult == nullptr || pstrTripId == nullptr
		|| pstrGpsSeq == nullptr || nRow < 0)
		return false;

	if (PQgetisnull(pcResult, nRow, RGC_TRIP_ID)
		|| PQgetisnull(pcResult, nRow, RGC_GPS_SEQ))
		return false;

	*pstrTripId = PQgetvalue(pcResult, nRow, RGC_TRIP_ID);
	*pstrGpsSeq = PQgetvalue(pcResult, nRow, RGC_GPS_SEQ);
	return true;
}

/**
 * @brief PQexec UPDATE/COMMAND 영향 행 수
 * @param[in] pcResult PQ 실행 결과
 * @return 영향 받은 행 수 (없으면 0)
 * @remark PGRES_COMMAND_OK 여도 WHERE 불일치 시 0 가능 (#5)
*/
int CRawLogFetcher::GetPgCmdTuples(PGresult *pcResult)
{
	if (pcResult == nullptr)
		return 0;

	const char *pszAffected = PQcmdTuples(pcResult);
	if (pszAffected == nullptr || pszAffected[0] == '\0')
		return 0;

	return atoi(pszAffected);
}

/**
 * @brief UPDATE 영향 행 수가 기대값과 일치하는지 검증
 * @param[in] pcResult PQ 실행 결과
 * @param[in] nExpected 기대 갱신 행 수
 * @param[in] pszLogTag 로그 태그 (nullptr 이면 "raw log fetcher")
 * @return true(일치), false(불일치·pcResult null)
*/
bool CRawLogFetcher::CheckPgUpdateAffected(PGresult *pcResult, int nExpected,
		const char *pszLogTag)
{
	// PQcmdTuples 로 영향 행 수 추출 (2026-07-08 최정우 주석 추가)
	const int nAffected = GetPgCmdTuples(pcResult);
	if (nAffected == nExpected)
		return true;

	LOGFMTW("%s partial update! expected=[%d] affected=[%d]",
		(pszLogTag != nullptr) ? pszLogTag : "raw log fetcher",
		nExpected, nAffected);
	return false;
}

/**
 * @brief 파싱 실패·미처리 예약 행 PROCESSING 해제 [rawgps_update]
 * @param[in] pcConn DB 커넥션
 * @param[in] pstrTripIds TRIP_ID 배열 (길이 nCount)
 * @param[in] pstrGpsSeqs GPS_SEQ 배열
 * @param[in] nCount release 대상 행 수
 * @return true(전건 release), false(실행 오류·부분 release·인자 무효)
 * @remark Worker BulkReleaseRawLogs 와 동일: $7="0", $3~$6='', WHERE MATCH_STATUS=2
 *   - 파라미터 순서는 [rawgps_update] 기준 $1 TRIP_ID, $2 GPS_SEQ, $3 MATCH_LAT, $4 MATCH_LON,
 *     $5 INTERSECT_LEN, $6 MATCH_LINK_ID, $7 MATCH_STATUS (2026-07-18 최정우 수정 —
 *     이전 코드는 $3 자리에 MATCH_STATUS를 넣고 MATCH_LINK_ID를 아예 빠뜨려 현재 SQL과 어긋나 있었음)
 *   - PQcmdTuples == nCount 검증 (#5)
*/
bool CRawLogFetcher::ReleaseReservedRows(PGconn *pcConn,
		const string *pstrTripIds, const string *pstrGpsSeqs,
		size_t nCount)
{
	if (pcConn == nullptr || m_strUpdateSQL.empty()
		|| pstrTripIds == nullptr || pstrGpsSeqs == nullptr
		|| nCount == 0)
		return false;

	vector<string> vtTripId(pstrTripIds, pstrTripIds + nCount);
	vector<string> vtGpsSeq(pstrGpsSeqs, pstrGpsSeqs + nCount);
	vector<string> vtMatchStatus(nCount, "0");
	vector<string> vtEmpty(nCount, string());

	// rawgps_update text[] 파라미터 리터럴 생성 (2026-07-08 최정우 주석 추가)
	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strGpsSeqArray = BuildPgTextArray(vtGpsSeq);
	string strMatchStatusArray = BuildPgTextArray(vtMatchStatus);
	string strIntersectLenArray = BuildPgTextArray(vtEmpty);
	string strMatchLatArray = BuildPgTextArray(vtEmpty);
	string strMatchLonArray = BuildPgTextArray(vtEmpty);
	string strMatchLinkIdArray = BuildPgTextArray(vtEmpty);

	const char *pszParams[7] =
	{
		strTripIdArray.c_str(),
		strGpsSeqArray.c_str(),
		strMatchLatArray.c_str(),
		strMatchLonArray.c_str(),
		strIntersectLenArray.c_str(),
		strMatchLinkIdArray.c_str(),
		strMatchStatusArray.c_str()
	};

	const int nParamLengths[7] =
	{
		static_cast<int>(strTripIdArray.size()),
		static_cast<int>(strGpsSeqArray.size()),
		static_cast<int>(strMatchLatArray.size()),
		static_cast<int>(strMatchLonArray.size()),
		static_cast<int>(strIntersectLenArray.size()),
		static_cast<int>(strMatchLinkIdArray.size()),
		static_cast<int>(strMatchStatusArray.size())
	};
	const int nParamFormats[7] = { 0, 0, 0, 0, 0, 0, 0 };

	// rawgps_update bulk release ($7=0) 실행 (2026-07-08 최정우 주석 추가)
	PGresult *pcResult = PQexecParams(pcConn, m_strUpdateSQL.c_str(),
		7, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	const int nExpected = static_cast<int>(nCount);
	// [2026-09-21 최정우 정리] 종전에는 중간 분기에서 이미 false 인 bOk 에 false 를 다시 넣고
	//   있었다(초기값과 같은 값). 세 갈래가 각각 다른 일을 하는 것처럼 보였을 뿐이라 정리한다.
	bool bOk = false;

	if (nExecStatus != PGRES_COMMAND_OK)
	{
		LOGFMTE("raw log fetcher: release reserved rows error! count=[%d] msg=[%s]",
			nExpected, PQresultErrorMessage(pcResult));
	}
	else
	{
		bOk = CheckPgUpdateAffected(pcResult, nExpected, "raw log fetcher: release reserved rows");
	}

	PQclear(pcResult);
	return bOk;
}

/**
 * @brief rawgps_select RETURNING 1행 → sRawLogInfo 변환
 * @param[in] pcResult PQ 결과
 * @param[in] nRow 행 인덱스
 * @param[out] pstRawLogInfo 변환된 RAW_LOG_INFO
 * @return true(성공), false(실패)
 * @remark RETURNING 순서: RawGpsReturningCol (설계서 v1.3 §2.1)
 *   - PK NULL 등 변환 불가 시 false. ReserveFetchBatch 가 release 후 건너뜀 (#4)
*/
bool CRawLogFetcher::ParseRow(PGresult *pcResult, int nRow, sRawLogInfo *pstRawLogInfo)
{
	if (pcResult == nullptr || pstRawLogInfo == nullptr || nRow < 0)
		return false;

	if (PQgetisnull(pcResult, nRow, RGC_TRIP_ID)
		|| PQgetisnull(pcResult, nRow, RGC_GPS_SEQ)
		|| PQgetisnull(pcResult, nRow, RGC_DEVICE_KEY)
		|| PQgetisnull(pcResult, nRow, RGC_GPS_DT))
		return false;

	const char *pszTripId = PQgetvalue(pcResult, nRow, RGC_TRIP_ID);
	const char *pszSeq = PQgetvalue(pcResult, nRow, RGC_GPS_SEQ);
	const char *pszDeviceKey = PQgetvalue(pcResult, nRow, RGC_DEVICE_KEY);
	const char *pszGpsDt = PQgetvalue(pcResult, nRow, RGC_GPS_DT);
	// NULL 기본값 — TRIP_EVENT NULL → "1"(NONE, 시작·종료 아님), DRIVE_STATUS NULL → "0"(ON_ROAD),
	//   GPS_LAT/LON NULL → bGpsLat/LonNull 표시(좌표 0 유지), RECV_DT NULL → ""(dtRecv=0).
	//   RAW_VLD NULL 은 아래에서 false(신뢰 불가)로 본다 (2026-10-06 최정우 주석 추가)
	const char *pszTripEvent = PQgetisnull(pcResult, nRow, RGC_TRIP_EVENT)
		? "1" : PQgetvalue(pcResult, nRow, RGC_TRIP_EVENT);
	const char *pszDriveStatus = PQgetisnull(pcResult, nRow, RGC_DRIVE_STATUS)
		? "0" : PQgetvalue(pcResult, nRow, RGC_DRIVE_STATUS);
	const char *pszLat = PQgetisnull(pcResult, nRow, RGC_GPS_LAT)
		? nullptr : PQgetvalue(pcResult, nRow, RGC_GPS_LAT);
	const char *pszLon = PQgetisnull(pcResult, nRow, RGC_GPS_LON)
		? nullptr : PQgetvalue(pcResult, nRow, RGC_GPS_LON);
	// SPEED_KMH 가 NULL 이면 속도 미상 → NO_SPEED 로 표기(방위각 기본 가중치 적용) (2026-07-08 최정우 수정)
	const bool bSpeedNull = PQgetisnull(pcResult, nRow, RGC_SPEED_KMH);
	const char *pszSpeed = bSpeedNull ? nullptr : PQgetvalue(pcResult, nRow, RGC_SPEED_KMH);
	// OBD_SPEED_KMH(차량 OBD 순간속도) 가 있으면 SPEED_KMH(GPS 환산속도) 보다 우선 적용 (2026-08-11 최정우 추가)
	const bool bObdSpeedNull = PQgetisnull(pcResult, nRow, RGC_OBD_SPEED_KMH);
	const char *pszObdSpeed = bObdSpeedNull ? nullptr : PQgetvalue(pcResult, nRow, RGC_OBD_SPEED_KMH);
	const char *pszRecvDt = PQgetisnull(pcResult, nRow, RGC_RECV_DT)
		? "" : PQgetvalue(pcResult, nRow, RGC_RECV_DT);

	*pstRawLogInfo = sRawLogInfo();
	pstRawLogInfo->dwSeqNo = static_cast<uint32>(strtoul(pszSeq, nullptr, 10));
	strncpy(pstRawLogInfo->szDeviceKey, pszDeviceKey, sizeof(pstRawLogInfo->szDeviceKey) - 1);
	pstRawLogInfo->nTripEvent = static_cast<sint16>(atoi(pszTripEvent));
	pstRawLogInfo->nDriveStatus = static_cast<sint16>(atoi(pszDriveStatus));
	strncpy(pstRawLogInfo->szTripID, pszTripId, sizeof(pstRawLogInfo->szTripID) - 1);
	strncpy(pstRawLogInfo->szDeviceID, pszDeviceKey, sizeof(pstRawLogInfo->szDeviceID) - 1);

	pstRawLogInfo->bGpsLatNull = (pszLat == nullptr);
	pstRawLogInfo->bGpsLonNull = (pszLon == nullptr);
	if (!pstRawLogInfo->bGpsLatNull)
		pstRawLogInfo->dfY = atof(pszLat);
	if (!pstRawLogInfo->bGpsLonNull)
		pstRawLogInfo->dfX = atof(pszLon);

	pstRawLogInfo->bRawVldKnown = true;
	if (PQgetisnull(pcResult, nRow, RGC_RAW_VLD))
		pstRawLogInfo->bRawVld = false;
	else
	{
		const char *pszRawVld = PQgetvalue(pcResult, nRow, RGC_RAW_VLD);
		pstRawLogInfo->bRawVld = (pszRawVld != nullptr)
			&& (pszRawVld[0] == 't' || pszRawVld[0] == 'T' || pszRawVld[0] == '1');
	}

	// OBD_SPEED_KMH 우선, 없으면 SPEED_KMH, 둘 다 NULL 이면 NO_SPEED(-1) (2026-08-11 최정우 수정)
	if (!bObdSpeedNull)
		pstRawLogInfo->fSpeed = static_cast<float>(atof(pszObdSpeed));
	else if (!bSpeedNull)
		pstRawLogInfo->fSpeed = static_cast<float>(atof(pszSpeed));
	else
		pstRawLogInfo->fSpeed = static_cast<float>(NO_SPEED);
	// HEADING(방위각) 이 NULL 이면 맵매칭에 미적용(NO_ANGLE) — 방향 조건 스킵
	if (PQgetisnull(pcResult, nRow, RGC_HEADING))
		pstRawLogInfo->nAngle = static_cast<sint16>(NO_ANGLE);
	else
		pstRawLogInfo->nAngle = static_cast<sint16>(atoi(PQgetvalue(pcResult, nRow, RGC_HEADING)));
	// ACCURACY_M(수평 오차) NULL → NO_ACCURACY, 아니면 정수 m
	if (PQgetisnull(pcResult, nRow, RGC_ACCURACY_M))
		pstRawLogInfo->nAccuracyM = static_cast<sint16>(NO_ACCURACY);
	else
		pstRawLogInfo->nAccuracyM = static_cast<sint16>(
			atoi(PQgetvalue(pcResult, nRow, RGC_ACCURACY_M)));
	// ALTITUDE_M(고도,m) NULL → NO_ALTITUDE(−1). 연속 맵매칭 고도 보조 점수 입력용.
	//   · 매칭 좌표(MATCH_LAT/LON)에는 Z 없음 → GPS 고도만 사용
	//   · 직전·현재 모두 유효할 때만 Δalt 계산 (한쪽 NULL이면 고도 점수 스킵)
	if (PQgetisnull(pcResult, nRow, RGC_ALTITUDE_M))
		pstRawLogInfo->nAltitudeM = static_cast<sint16>(NO_ALTITUDE);
	else
		pstRawLogInfo->nAltitudeM = static_cast<sint16>(
			atoi(PQgetvalue(pcResult, nRow, RGC_ALTITUDE_M)));
	// GPS_DT YYYYMMDDHH24MISS → time_t 변환 (2026-07-08 최정우 주석 추가)
	pstRawLogInfo->dtGPS = ParseDateTime(pszGpsDt);
	// RECV_DT YYYYMMDDHH24MISS → time_t 변환 (2026-07-08 최정우 주석 추가)
	pstRawLogInfo->dtRecv = ParseDateTime(pszRecvDt);
	pstRawLogInfo->nCoordinateType = m_nCoordinateType;

	return true;
}

/**
 * @brief YYYYMMDDHH24MISS 문자열 → time_t (로컬 시각)
 * @param[in] pszDateTime 일시 문자열 (최소 14자)
 * @return time_t (실패·빈 값 0)
*/
time_t CRawLogFetcher::ParseDateTime(const char *pszDateTime)
{
	if (pszDateTime == nullptr || strlen(pszDateTime) < 14)
		return 0;

	struct tm stTm;
	memset(reinterpret_cast<void *>(&stTm), 0, sizeof(stTm));
	// [2026-09-21 최정우 보완] memset 으로 tm_isdst 가 0(=서머타임 아님)으로 고정된다. 한국은
	//   서머타임이 없어 현재는 결과가 같지만, 이는 "이 코드가 KST 전용" 이라는 숨은 전제다.
	//   -1 은 "모르니 mktime 이 알아서 판단하라" 는 뜻으로, 서머타임이 있는 지역에서 돌려도
	//   전환 시각 주변에서 1시간 어긋나지 않는다. 동작 변화 없음(KST 기준 동일 결과).
	stTm.tm_isdst = -1;

	char szBuf[5];

	memset(szBuf, 0, sizeof(szBuf));
	memcpy(szBuf, pszDateTime, 4);
	stTm.tm_year = atoi(szBuf) - 1900;

	memset(szBuf, 0, sizeof(szBuf));
	memcpy(szBuf, pszDateTime + 4, 2);
	stTm.tm_mon = atoi(szBuf) - 1;

	memset(szBuf, 0, sizeof(szBuf));
	memcpy(szBuf, pszDateTime + 6, 2);
	stTm.tm_mday = atoi(szBuf);

	memset(szBuf, 0, sizeof(szBuf));
	memcpy(szBuf, pszDateTime + 8, 2);
	stTm.tm_hour = atoi(szBuf);

	memset(szBuf, 0, sizeof(szBuf));
	memcpy(szBuf, pszDateTime + 10, 2);
	stTm.tm_min = atoi(szBuf);

	memset(szBuf, 0, sizeof(szBuf));
	memcpy(szBuf, pszDateTime + 12, 2);
	stTm.tm_sec = atoi(szBuf);

	// struct tm → time_t (로컬 시각) (2026-07-08 최정우 주석 추가)
	return mktime(&stTm);
}

/**
 * @file RawLogWorker.cpp
 * @brief 원시 GPS batch 맵매칭·DB 결과 갱신 워커 클래스 소스 파일
*/
#include "RawLogWorker.h"
#include "Clock.h"
#include "log4z.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <cctype>
#include <unistd.h>
#include <pthread.h>
#include <unordered_map>
#include <unordered_set>
#include <libpq-fe.h>

using namespace zsummer::log4z;

// [2026-10-04 최정우 추가, 사용자 확정 — 정책 E16] 주정차 폴리곤 안 일반도로 억제 여부. false = 폴리곤 안이라도
//   일반도로로 판정되면 등록한다(ProcessNodeStepCharge 의 폴리곤 억제 해제 + FillUncoveredLinkRows 의 폴리곤 안
//   링크 제외 해제). true 로 바꾸면 종전 동작(B2·B4)으로 돌아간다.
static const bool MM_PARK_SUPPRESS_NODESTEP = false;

// NON_CHARGE_REASON 코드별 메시지 — DataDefine.h 의 NCR_* 상수와 1:1 대응, MapMatch.cpp 의
//   ErrorCodeTable/m_cCodeMap 과 동일 패턴(CCodeMap::GetValue) 으로 로그에 사람이 읽을 수 있는
//   사유를 남긴다 (2026-09-11 최정우 추가)
CODE_ENTRY NonChargeReasonTable[] =
{
	{NCR_NORMAL,						"정상 과금"},			// 전 도로유형 공통 — 유형별로 나누지
																//   않는다(사용자 확정, 2026-09-15)
	{NCR_NODE_STEP_GAP_ANCHOR_LOST,	"일반도로 SKIP구간 브릿지 - 직전 확정위치 소실"},
	{NCR_NODE_STEP_GAP_APPROX,			"일반도로 SKIP구간 브릿지 - 직선거리로 추정"},
	{NCR_OPEN_ENTRY_GATE_MISSED,		"개방형 - 트립 중간시작으로 진입게이트 미통과"},
	{NCR_OPEN_GATE_NOT_ON_PATH,		"개방형 - GPS 간격 사이로 구역을 스쳐 지나 게이트 통과 미확정"},
	{NCR_CLOSED_ENTRY_UNOBSERVED,		"폐쇄형 - 진입게이트 미확인"},
	{NCR_CLOSED_ENTRY_EQUALS_EXIT,		"폐쇄형 - 입구==출구 동일 게이트"},
	{NCR_CLOSED_EXIT_UNCONFIRMED,		"폐쇄형 - 출구게이트 미확인"},
	{NCR_SPEED_ENTRY_UNOBSERVED,		"구간단속 - 진입게이트 미확인"},
	{NCR_SPEED_ENTRY_EQUALS_EXIT,		"구간단속 - 입구==출구 동일 게이트"},
	{NCR_SPEED_EXIT_UNCONFIRMED,		"구간단속 - 출구게이트 미확인"},
	{NCR_EXEMPT_TTL_FORCED_CLOSE,		"면제도로 - 종료 미확정 강제마감(TTL·잔여tick·트립종료)"},
	{NCR_EXEMPT_NO_TRIP_END,			"면제도로 - 종료신호 없이 다음 운행 시작으로 강제마감"},
	{NCR_TTL_FORCED_CLOSE,				"종료 미확정 강제마감(TTL·잔여tick·트립종료)"},
	{NCR_NO_TRIP_END_FORCED_CLOSE,		"종료신호 없이 다음 운행 시작으로 강제마감"}
};

namespace {

// bulk release 재시도 횟수 (PK: TRIP_ID|GPS_SEQ). 워커 스레드 간 공유 (2026-07-10 최정우 주석 추가)
pthread_mutex_t g_retryCountMutex = PTHREAD_MUTEX_INITIALIZER;
unordered_map<string, int> g_mapReleaseRetryCount;

/**
 * @brief release 재시도 카운트 맵 키 생성
 * @param[in] strTripId 운행 ID (PK-1)
 * @param[in] strGpsSeq GPS 순번 (PK-2, 문자열)
 * @return TRIP_ID|GPS_SEQ 형식 키
 * @remark BulkReleaseRawLogs() 재시도 상한(nRetryMax) 판별용
 */
static string MakeReleaseRetryKey(const string& strTripId, const string& strGpsSeq)
{
	return strTripId + "|" + strGpsSeq;
}

/**
 * @brief release 재시도 횟수 1 증가 (스레드 안전)
 * @param[in] strKey MakeReleaseRetryKey() 로 생성한 키
 * @return 증가 후 재시도 횟수
 * @remark BulkReleaseRawLogs() 에서 PROCESSING→PENDING 해제 실패 시 누적
 */
static int BumpReleaseRetryCount(const string& strKey)
{
	pthread_mutex_lock(&g_retryCountMutex);
	int nCount = ++g_mapReleaseRetryCount[strKey];
	pthread_mutex_unlock(&g_retryCountMutex);
	return nCount;
}

/**
 * @brief release 재시도 카운트 제거 (스레드 안전)
 * @param[in] strKey MakeReleaseRetryKey() 로 생성한 키
 * @return void
 * @remark BulkUpdateRawLogs() 정상 완료(MATCHED/SKIP/ERROR) 시 호출
 */
static void ClearReleaseRetryCount(const string& strKey)
{
	pthread_mutex_lock(&g_retryCountMutex);
	g_mapReleaseRetryCount.erase(strKey);
	pthread_mutex_unlock(&g_retryCountMutex);
}

/**
 * @brief 커넥션 풀에서 DB 연결 핸들 확보 (일시 고갈 시 재시도)
 * @param[in] pcPool PostgreSQL 커넥션 풀
 * @param[in] nMaxAttempt 재시도 최대 횟수 ([database] retrymax, 회)
 * @param[in] nWaitMs 재시도 사이 대기 ([database] retrywait, ms)
 * @return PGconn*(성공), nullptr(실패)
 * @remark getConnection() 실패 시 nWaitMs 간격으로 최대 nMaxAttempt 회 시도 (2026-07-10 최정우 주석 추가)
 */
static PGconn* AcquirePoolConnection(CPostgrePool *pcPool, int nMaxAttempt, int nWaitMs)
{
	if (pcPool == nullptr)
		return nullptr;

	if (nMaxAttempt < 1)
		nMaxAttempt = 1;
	if (nWaitMs < 0)
		nWaitMs = 0;

	for (int nAttempt=1; nAttempt<=nMaxAttempt; ++nAttempt)
	{
		PGconn *pcConn = pcPool->getConnection();
		if (pcConn != nullptr)
			return pcConn;

		if ((nAttempt < nMaxAttempt) && (nWaitMs > 0))
			usleep(static_cast<useconds_t>(nWaitMs) * 1000);
	}

	return nullptr;
}

} // namespace

/**
 * @brief 생성자
*/
CRawLogWorker::CRawLogWorker()
{
	// [버그 수정, 2026-09-17 최정우] 여기 있던 memset(&m_stConfig, 0, sizeof(m_stConfig)) 를
	//   제거했다 — m_stConfig 에는 std::string 이 6개 있어 memset 이 내부 포인터를 파괴했고,
	//   SetConfig() 의 대입에서 **빈 문자열을 받는 멤버**를 만나면 세그폴트했다(재현 확인).
	//   초기화는 멤버 선언부의 `{}` 값 초기화가 대신한다 — 상세 근거는 RawLogWorker.h 참고.
}

/**
 * @brief 소멸자
*/
CRawLogWorker::~CRawLogWorker()
{
}

/**
 * @brief 워커 공유 설정 및 스레드별 trip_id 세션 맵 초기화
 * @param[in] stConfig DB pool, SQL, ProcessManager, 워커 스레드 수
 * @return void
*/
void CRawLogWorker::SetConfig(const RAWLOG_WORKER_CONFIG& stConfig)
{
	m_stConfig = stConfig;

	if (m_stConfig.nWorkerThreads <= 0)
		m_stConfig.nWorkerThreads = 1;

	// [database] conn_retry — 기동 시 LoadConfig 값 보정 (2026-07-10 최정우 추가)
	if (m_stConfig.nConnRetryMax < 1)
		m_stConfig.nConnRetryMax = 1;
	if (m_stConfig.nConnRetryWait < 0)
		m_stConfig.nConnRetryWait = 0;

	m_vtTripSessions.clear();
	m_vtTripSessions.resize(static_cast<size_t>(m_stConfig.nWorkerThreads));
}

/**
 * @brief 진행 중이던 모든 과금 구간을 "비정상 종료"로 마감 (TTL 만료 / 트립종료 이후 잔여틱 / 트립전환 공용)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in,out] stSession 마감 대상 세션 — run 목록·carry 상태가 여기서 소비된다
 * @param[in] dtEndTime 마감 기준 시각(그 트립에서 마지막으로 확인된 시각)
 * @param[in] dwEndGpsSeq 마감 기준 GPS_SEQ
 * @param[in] dtNow UPD_DT 용 현재 시각(벽시계)
 * @param[out] pvtOut 생성된 과금 행이 추가된다
 * @param[out] pvtAbnormalEndUpdates TRIP_END_DT 미확정 행 마감 UPDATE 대상(nullptr 허용)
 * @param[in] bNoTripEnd true=종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로
 *   — non_charge_reason 을 62(면제도로는 52)로 남긴다. false=TTL·잔여tick·트립종료(61/51).
 *   판정(charge_yn/charge_status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
 * @return void
 * @remark [2026-09-15 최정우 추가] ExpireTtlSessions() 안에 인라인으로 있던 마감 블록을 로직 변경
 *   없이 함수로 뺀 것이다. 같은 처리가 **트립종료 tick 이후에도 tick 이 더 들어온 트립**과
 *   **종료신호 없이 다음 운행이 시작되는 경우**에도 필요해졌기 때문이다.
 * @warning 호출 시점에 stSession.szTripId 가 아직 "마감할 트립" 이어야 한다. 2026-09-15 오전에
 *   같은 취지의 코드 62 를 구현했다가 잡종 레코드(trip_id 는 새 운행, occur_dt 는 이전 운행)가
 *   나와 전량 원복했는데, 원인이 szTripId 갱신 **뒤에** 마감한 것이었다.
*/
void CRawLogWorker::FlushOpenRunsAsAbnormalEnd(int nThreadId, const string& strDeviceKey,
		VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, uint32 dwEndGpsSeq, time_t dtNow,
		vector<CHARGE_INSERT_ROW> *pvtOut,
		vector<TRIP_END_UPDATE_ROW> *pvtAbnormalEndUpdates, bool bNoTripEnd)
{
	if (pvtOut == nullptr)
		return;

	// [중요] AppendExpiredParkingCharge()/AppendExpiredOpenGateCharge() 는 체류 종료 시각으로
	//   세션의 dtLastSeen 을 **직접** 읽는데 그 값은 벽시계다. TTL 경로에서는 그게 맞지만
	//   (만료 시점이 곧 마지막 관측), 트립종료·트립전환 경로에서는 **그 트립의 마지막 GPS 시각**
	//   이어야 한다. 과거 데이터를 재매칭하면 둘이 수십 일 벌어져 체류시간이 터무니없어진다
	//   (실측: 2026-08-24 데이터를 2026-09-15 에 재매칭하니 stay_seconds=1,904,398 ≈ 22일).
	//   TTL 호출은 dtEndTime 에 dtLastSeen 을 그대로 넘기므로 아래 대입이 항등식이 되어 기존
	//   동작이 전혀 바뀌지 않는다 (2026-09-15 최정우 추가).
	// ※ 아래 본문 주석에 남은 "TTL" 표현은 이 블록이 ExpireTtlSessions() 안에 있던 시절의
	//   문맥이다. 2026-09-15 함수 추출 이후로는 **트립종료 이후 잔여 tick·종료신호 없는 트립
	//   전환 경로에도 그대로 적용**된다 — "TTL 만료 시"로 읽지 말고 "마감 시"로 읽을 것.
	const time_t dtSavedLastSeen = stSession.dtLastSeen;
	if (dtEndTime > 0)
		stSession.dtLastSeen = dtEndTime;

	// 한 trip 에서 여러 세션(예: 주정차 폴리곤·면제도로/일반도로/폐쇄형/구간단속 LINE)이
	//   좌표상 겹쳐 동시에 TTL 만료될 수 있음(실측으로 확인됨, 2026-08-13) — Append*들이
	//   같은 nChargeSeq(trip_seq)를 그대로 쓰면 PK(trip_id,device_key,trip_seq) 충돌로
	//   뒤 INSERT가 ON CONFLICT DO NOTHING 에 조용히 유실됨. 실제 INSERT 성공한 것만
	//   골라 seq 를 증가시켜야 하므로 호출 전후로 bIn*RoadZone 상태 변화를 확인 — 매번
	//   "호출 전 스냅샷 → 호출 → 증가" 패턴을 반복 (2026-08-14 최정우 수정 — 폐쇄형·구간단속
	//   TTL-flush 함수 신규 추가로 5개가 됨)
	// [버그 수정, 2026-09-10 최정우] 겹치는 구역이 2개 이상이면(함수 헤더 주석 "겹쳐
	//   진행 중이던 구역을 전부 마감한다"에 이미 명시된 상황) 이 Append 함수들이 한 번에
	//   여러 행을 push하는데, 고정 +1만 증가시키면 다음 유형 Append가 그 행들 중 마지막
	//   행과 같은 trip_seq부터 다시 시작해 PK 충돌로 조용히 유실된다 — 최소 재현으로 확인.
	//   SPEED(아래 bWasSpeedZone)는 2026-09-01/02에 이미 이 방식으로 고쳐져 있었는데,
	//   같은 근본 원인이 이 4곳(PARKING/EXEMPT/NODE_STEP/OPEN)에도 있다는 걸 그때 놓쳤다.
	bool bWasParking = !stSession.vtParkRuns.empty();
	size_t nSizeBeforeParking = (*pvtOut).size();
	AppendExpiredParkingCharge(nThreadId, strDeviceKey, stSession, &(*pvtOut), bNoTripEnd);
	if (bWasParking)
		stSession.nChargeSeq += static_cast<int>((*pvtOut).size() - nSizeBeforeParking);

	bool bWasExempt = !stSession.vtExemptRuns.empty();
	size_t nSizeBeforeExempt = (*pvtOut).size();
	AppendExpiredExemptZoneCharge(nThreadId, strDeviceKey, stSession, &(*pvtOut), bNoTripEnd);
	if (bWasExempt)
		stSession.nChargeSeq += static_cast<int>((*pvtOut).size() - nSizeBeforeExempt);

	// 구간단속 마감 시 보류해둔 일반도로 미러가 세션 TTL 소멸 전까지도 인수인계 구간을
	//   못 만나 소비 안 된 채 남아있으면, 원래 값 그대로 지금 등록한다(2026-09-03 최정우 추가)
	if (stSession.bHasHeldSpeedMirrorRun)
	{
		CHARGE_INSERT_ROW stMirrorRow;
		BuildNodeStepRow(stSession.stHeldSpeedMirrorRun, stSession.szTripId, strDeviceKey,
			stSession.nChargeSeq, stSession.stHeldSpeedMirrorRun.dtLastInZoneTime,
			stSession.stHeldSpeedMirrorRun.dwLastInZoneGpsSeq, "Y", "0", &stMirrorRow);
		(*pvtOut).push_back(stMirrorRow);
		stSession.nChargeSeq += 1;
		stSession.bHasHeldSpeedMirrorRun = false;
	}

	// 주정차 접촉이 확정 판정도 못 받고 세션이 TTL로 소멸하는 경우도 트립종료 경로와
	//   동일 기준으로 판정한다 — 확정 접촉이면 보류된 run을 그 경계 그대로 정상(Y/0)
	//   등록하고 접촉 구간은 버리며, 미확정이면 보류된 run과 접촉 구간을 합쳐 마지막
	//   확인 위치·시각 기준으로 정상(Y/0) 등록한다(2026-09-03 최정우 추가 — 위 트립종료
	//   경로들과 동일 근거)
	if (stSession.bHasParkTouchCarry)
	{
		if (stSession.bParkTouchEverMatchedInside)
		{
			// 접촉 직전까지 실제 이동거리가 0이면(접촉이 진입 직후 바로 시작돼 일반도로
			//   구간이 사실상 없었던 경우) 등록할 내용 자체가 없다 — 빈 레코드를 남기지
			//   않는다(사용자 지시, 2026-09-03 최정우 추가)
			if (stSession.bHasHeldNodeStepRun && (stSession.stHeldNodeStepRun.dfAccumDistM > 0.0))
			{
				CHARGE_INSERT_ROW stHeldRow;
				BuildNodeStepRow(stSession.stHeldNodeStepRun, stSession.szTripId, strDeviceKey,
					stSession.nChargeSeq, stSession.stHeldNodeStepRun.dtLastInZoneTime,
					stSession.stHeldNodeStepRun.dwLastInZoneGpsSeq, "Y", "0", &stHeldRow);
				(*pvtOut).push_back(stHeldRow);
				stSession.nChargeSeq += 1;
			}
		}
		else if (stSession.bHasHeldNodeStepRun)
		{
			ZONE_RUN_SESSION stFinal = stSession.stHeldNodeStepRun;
			stFinal.dfAccumDistM += stSession.stParkTouchCarry.dfAccumDistM;
			stFinal.dtLastInZoneTime = stSession.stParkTouchCarry.dtLastInZoneTime;
			stFinal.dwLastInZoneGpsSeq = stSession.stParkTouchCarry.dwLastInZoneGpsSeq;

			CHARGE_INSERT_ROW stFinalRow;
			BuildNodeStepRow(stFinal, stSession.szTripId, strDeviceKey,
				stSession.nChargeSeq, stFinal.dtLastInZoneTime, stFinal.dwLastInZoneGpsSeq,
				"Y", "0", &stFinalRow);
			(*pvtOut).push_back(stFinalRow);
			stSession.nChargeSeq += 1;
		}

		stSession.bHasParkTouchCarry = false;
		stSession.bHasHeldNodeStepRun = false;
		stSession.bParkTouchEverMatchedInside = false;
	}
	else if (stSession.bHasHeldNodeStepRun)
	{
		stSession.vtNodeStepRuns.push_back(stSession.stHeldNodeStepRun);
		stSession.bHasHeldNodeStepRun = false;
	}
	// [버그 수정, 2026-09-11 최정우] 일반도로 구간병합 이월값(stMergeCarry) — 받아줄 새 run이
	//   열리기를 기다리는 중에 세션이 TTL로 만료되면, 다른 carry 상태들과 달리 여기서
	//   소비되지 않아 그 구간의 거리·시간이 과금 레코드 없이 사라지고 있었다(전체 재검증으로
	//   발견, FlushNodeStepRunsAtTripEnd() 도 동일하게 누락돼 있어 같이 고침) — 남은 값을
	//   그대로 vtNodeStepRuns 에 편입시켜 마감한다.
	if (stSession.bHasMergeCarry)
	{
		stSession.vtNodeStepRuns.push_back(stSession.stMergeCarry);
		stSession.bHasMergeCarry = false;
	}
	bool bWasNodeStep = !stSession.vtNodeStepRuns.empty();
	size_t nSizeBeforeNodeStep = (*pvtOut).size();
	// 마감 경로 3종 모두 "신뢰할 트립종료 신호를 못 받은" 상태이므로 bTrustedTripEnd=false — 항상 N/3
	AppendExpiredNodeStepCharge(nThreadId, strDeviceKey, stSession,
		dtEndTime, dwEndGpsSeq, false, &(*pvtOut), bNoTripEnd);
	if (bWasNodeStep)
		stSession.nChargeSeq += static_cast<int>((*pvtOut).size() - nSizeBeforeNodeStep);

	bool bWasOpen = !stSession.vtOpenRuns.empty();
	size_t nSizeBeforeOpen = (*pvtOut).size();
	AppendExpiredOpenGateCharge(nThreadId, strDeviceKey, stSession, &(*pvtOut), bNoTripEnd);
	if (bWasOpen)
		stSession.nChargeSeq += static_cast<int>((*pvtOut).size() - nSizeBeforeOpen);

	// [2026-09-21 최정우 정리] 폐쇄형만 고정 +1 로 남아 있던 것을 다른 5곳과 같은 "실제로 늘어난
	//   행 수" 패턴으로 통일한다. AppendExpiredClosedRoadCharge() 의 push_back 은 현재 1곳뿐이라
	//   **동작은 완전히 동일**하다. 다만 2026-09-10 에 PARKING/EXEMPT/NODE_STEP/OPEN 4곳을 이
	//   패턴으로 고친 이유(한 번에 여러 행을 push 하면 고정 +1 이 PK 충돌을 낳는다)가 여기에도
	//   그대로 적용되므로, 나중에 이 함수가 행을 하나 더 추가하게 되면(SPEED 가 일반도로 미러를
	//   더하게 된 것처럼) 그 순간 조용히 유실이 시작된다. 미리 같은 규칙으로 맞춰둔다.
	bool bWasClosedRoad = stSession.bInClosedRoad;
	size_t nSizeBeforeClosed = (*pvtOut).size();
	AppendExpiredClosedRoadCharge(nThreadId, strDeviceKey, stSession, dtEndTime, &(*pvtOut), bNoTripEnd);
	if (bWasClosedRoad)
		stSession.nChargeSeq += static_cast<int>((*pvtOut).size() - nSizeBeforeClosed);

	bool bWasSpeedZone = stSession.bInSpeedZone;
	// NODE_STEP 일반도로 확장(케이스1)으로 이 함수가 SPEED row 에 더해 NODE_STEP row 까지
	//   최대 2건을 추가할 수 있게 돼, 고정 +1 대신 실제로 늘어난 행 수만큼 증가시킨다
	//   (2026-09-01 최정우 수정 — 안 그러면 다음 유형 TTL-flush 가 같은 trip_seq 를 재사용해
	//   PK 충돌로 유실됨)
	size_t nSizeBeforeSpeed = (*pvtOut).size();
	AppendExpiredSpeedZoneCharge(nThreadId, strDeviceKey, stSession, dtEndTime, &(*pvtOut));
	if (bWasSpeedZone)
		stSession.nChargeSeq += static_cast<int>((*pvtOut).size() - nSizeBeforeSpeed);

	// 미확정 레코드 마감(전 과금유형 공용) — trip_id 하나당 1행이면 충분(WHERE 절이
	//   TRIP_END_DT IS NULL 로 알아서 대상만 걸러줌, 없으면 0건 영향으로 조용히 끝남)
	//   (2026-08-13 최정우 추가, 2026-08-13 수정 — 개방형 한정 해제)
	if (stSession.szTripId[0] != '\0')
	{
		TRIP_END_UPDATE_ROW stAbnormalRow;
		stAbnormalRow.strTripId = stSession.szTripId;
		stAbnormalRow.strTripEndDt = FormatDateTime14(dtEndTime);
		stAbnormalRow.strUpdDt = FormatDateTime14(dtNow);
		if (pvtAbnormalEndUpdates != nullptr)
			pvtAbnormalEndUpdates->push_back(stAbnormalRow);
	}

	stSession.dtLastSeen = dtSavedLastSeen;			// 위 임시 대입 원복 (2026-09-15 최정우)
}

/**
 * @brief dtLastSeen 경과 trip_id 세션 만료 제거 (#6 TTL, 워커 스레드 self)
 * @param[in] nThreadId 워커 스레드 ID (자기 소유 세션 맵만 정리)
 * @param[in] nTtlSec TTL (초). 0 이하면 비활성
 * @param[in] pcConn TTL 만료 시점에 아직 주정차 세션이 열려 있으면 즉시 위반 INSERT, 전 유형 중
 *   미확정(TRIP_END_DT NULL) 레코드가 있으면 마감 UPDATE 하는 데 씀(nullptr 이면 둘 다 스킵)
 * @param[in] bForceAll true=경과시간 조건을 무시하고 **열려 있는 세션 전부**를 마감한다
 *   (서버 종료 시 FlushAllSessionsOnShutdown() 전용, nTtlSec 도 무시). false=종전대로
 *   nTtlSec 경과분만 (2026-09-16 최정우 추가)
 * @return 제거된 세션 수. **DB 반영이 실패해 롤백되면 세션을 전부 복원하고 0 을 반환한다**
 *   (다음 스윕에서 통째로 재시도) — 호출측이 이 값으로 "이번에 몇 개 만료됐나"를 판단하면
 *   롤백 시 0 을 정상값으로 오해할 수 있다 (2026-09-15 최정우)
 * @remark 각 워커가 자기 맵(m_vtTripSessions[nThreadId])만 정리 → 락 불필요(소유권 유지).
 *         run() 배치 처리 직후 호출되어 모니터 스레드와의 동시 접근(데이터 레이스)을 제거한다.
 *         TTL 만료 = 그 이후로 GPS 자체가 안 온 것 — END 이벤트를 놓쳤든 단말이 아예 전송을 멈췄든
 *         서버 입장에서 원인은 구분 불가. 주정차는 "계속 정차 중이었다"로 간주해 마지막으로 확인된
 *         위치·시각까지의 체류시간으로 위반을 확정(사용자 지시, 2026-08-13). 나머지(이미 INSERT돼
 *         있는데 트립이 안 끝나 TRIP_END_DT 가 아직 NULL인 레코드, 전 유형 해당 — 개방형은
 *         게이트 통과 즉시 Y/0 확정이라 항상 이 케이스)는 N/3(AUDIT) + TRIP_END_DT(마지막 확인
 *         시각)로 마감(사용자 지시, 2026-08-13 — `UpdateAbnormalTripEnd()`, 최초엔 개방형
 *         한정·status=4 였다가 전 유형·status=3(AUDIT)으로 확대·정정). 참고: 세션 자체가 다른
 *         이유로 유실되는 경우(서버 재시작 등, in-memory라 영속 안 됨)는 이 경로로도 못 잡음 —
 *         별도 한계로 남음.
*/
int CRawLogWorker::ExpireTtlSessions(int nThreadId, int nTtlSec, PGconn *pcConn, bool bForceAll)
{
	// bForceAll — 서버 종료 시 "경과시간과 무관하게 열려 있는 세션 전부"를 마감하는 모드
	//   (FlushAllSessionsOnShutdown 전용, 2026-09-16 최정우 추가). 평상시(false)는 종전대로
	//   nTtlSec 경과분만 대상으로 한다.
	if ((nTtlSec <= 0) && !bForceAll)
		return 0;

	if (nThreadId < 0 || nThreadId >= static_cast<int>(m_vtTripSessions.size()))
		return 0;

	unordered_map<string, VEHICLE_TRIP_SESSION>& mapSessions =
		m_vtTripSessions[static_cast<size_t>(nThreadId)];

	const time_t dtNow = time(nullptr);
	int nRemoved = 0;
	vector<CHARGE_INSERT_ROW> vtExpiredParkingCharges;
	vector<TRIP_END_UPDATE_ROW> vtAbnormalEndUpdates;
	vector<RAW_LOG_UPDATE_ROW> vtExpiredPendingUpdates;			// 세션 소멸 전 보류(pending) 행 확정분 (2026-08-21 최정우 추가)
	// DB 반영 실패 시 복원용 (key=DEVICE_KEY, 값=플러시 직전 세션) (2026-09-15 최정우 추가)
	vector<pair<string, VEHICLE_TRIP_SESSION> > vtFlushedSnapshots;

	for (unordered_map<string, VEHICLE_TRIP_SESSION>::iterator it=mapSessions.begin();
			it != mapSessions.end(); )
	{
		if (it->second.dtLastSeen > 0
			&& (bForceAll || ((dtNow - it->second.dtLastSeen) > static_cast<time_t>(nTtlSec))))
		{
			LOGFMTD("[#%02d] session ttl expired!trip_id=[%s] last_seen=[%ld] ttl=[%d]",
				nThreadId, it->first.c_str(),
				static_cast<long>(it->second.dtLastSeen), nTtlSec);

			// [버그 수정, 2026-09-15 최정우] DB 반영 실패 시 되돌리기 위한 스냅샷 — Append* 들이
			//   세션의 run 목록을 소비(clear)하기 **전에** 떠야 한다. 종전에는 아래에서 세션을
			//   먼저 erase 하고 DB 쓰기는 반환값도 안 보고 호출해, INSERT 가 실패하면 그 워커의
			//   TTL 마감 과금이 로그도 재시도도 없이 통째로 사라졌다(세션이 이미 메모리에서
			//   없어졌으므로 복구 불가). 실패 시 이 스냅샷으로 되살려 다음 스윕에서 재시도한다.
			//   구조체에 포인터 멤버가 없어 값 복사로 안전하다(vector/POD/char 배열만).
			vtFlushedSnapshots.push_back(make_pair(it->first, it->second));

			// 세션이 곧 지워지므로, 아직 보류(pending) 중인 1틱 지연 행이 있으면 먼저 확정
			//   (commit)한다 — 더 이상 "다음" GPS 가 안 올 것이므로 보정판단 없이 계산된 값 그대로.
			//   아래 bWasParking 등 스냅샷보다 먼저 실행해야 보류 행의 과금 반영이 상태에 반영됨
			//   (2026-08-21 최정우 추가)
			CommitPendingRow(nThreadId, &it->second, false, 0, &vtExpiredPendingUpdates, &vtExpiredParkingCharges);

			// [2026-10-04 최정우 수정, 사용자 확정 — TTL 마감 권장안 (나)] 마감 기준 시각을 **그 트립의 마지막
			//   GPS 시각**(dtLastGpsEventTime)으로 넘긴다. 종전에는 dtLastSeen(서버가 마지막으로 처리한 벽시계)
			//   이라 TRIP_END_DT 와 폐쇄식·구간단속 강제마감 행의 종료 시각에 벽시계가 들어갔다 — 시나리오
			//   980015: GPS 마지막 20261005010112 인데 TRIP_END_DT 20261004123120(트립 출발보다 앞). 정상 종료·
			//   트립 전환은 2026-09-15 부터 이미 GPS 시각이다. TTL 경과 판정(3600초)은 계속 dtLastSeen(벽시계).
			// 되돌리는 법: 아래 dtTtlEnd 대신 it->second.dtLastSeen 을 넘긴다.
			const time_t dtTtlEnd = (it->second.dtLastGpsEventTime > 0)
				? it->second.dtLastGpsEventTime : it->second.dtLastSeen;
			FlushOpenRunsAsAbnormalEnd(nThreadId, it->first, it->second,
				dtTtlEnd, it->second.dwLastGpsSeq, dtNow,
				&vtExpiredParkingCharges, &vtAbnormalEndUpdates);

			// [2026-09-22 최정우 추가] 워터마크 큐 전량 방출 — 세션이 곧 erase 되므로 **여기서
			//   꺼내지 않으면 이미 마감된 과금 행이 메모리와 함께 사라진다**. 위 Flush 가 만든 행도
			//   아직 번호가 없는 상태로 큐에 있으므로, 반드시 Flush **뒤에** 호출해야 전부 나간다.
			//   (DB 반영 실패 시에는 위 스냅샷으로 세션이 통째로 복원되어 큐·nEmitSeq 도 되돌아간다)
			EnqueueChargeRows(&(it->second), &vtExpiredParkingCharges);
			ReleaseChargeQueue(nThreadId, &(it->second), UINT32_MAX, &vtExpiredParkingCharges);

			mapSessions.erase(it++);
			++nRemoved;
		}
		else
		{
			// park_ttl — dtLastSeen(세션 전체)은 아직 신선해도(예: raw_vld=false GPS가 계속
			//   들어옴), 열려있는 주정차 세션의 마지막 신뢰(raw_vld=true) 확인으로부터 park_ttl
			//   초가 지났으면 좌표 확인 없이 그 시점 기준으로 강제 마감한다. 디바이스 세션 자체는
			//   지우지 않음 — GPS는 계속 들어오고 있어 다른 유형 세션·연속 매칭 컨텍스트는 그대로
			//   유지해야 함(위 TTL 만료 분기와 다른 점) (2026-08-19 최정우 추가)
			// 구역별 park_ttl 판정은 함수 안에서 한다(겹쳐 열린 구역마다 만료 시점이 다름)
			// [버그 수정, 2026-09-15 최정우] 이 분기도 스냅샷을 뜬다 — 세션을 지우지는 않지만
			//   AppendStaleParkingCharge() 가 열린 주정차 run 을 **소비**하므로, 아래 INSERT 가
			//   실패해 롤백되면 그 위반 기록이 영구 유실된다(run 은 이미 닫혔고 DB 에는 없음).
			//   erase 분기와 달리 살아있는 세션을 덮어쓰게 되는데, 스냅샷~복원 사이에 이 세션을
			//   건드리는 코드가 없어(같은 워커 스레드, 같은 함수) 안전하다.
			if (!it->second.vtParkRuns.empty())
			{
				vtFlushedSnapshots.push_back(make_pair(it->first, it->second));
				AppendStaleParkingCharge(nThreadId, it->first, &it->second, &vtExpiredParkingCharges);
			}
			++it;
		}
	}

	// [버그 수정, 2026-09-15 최정우] TTL 경로도 run() 과 동일하게 **하나의 트랜잭션**으로 묶고
	//   반환값을 검사한다. 종전에는 BEGIN/COMMIT 없이 세 함수를 반환값도 버리고 호출했다 —
	//   2026-08-29 에 run() 의 rawgps UPDATE + 과금 INSERT 원자성을 고쳤는데 이 경로가 그 수정을
	//   못 받았다. 구체적 실패 양상 2가지:
	//   (1) 과금 영구 유실 — 행 1건의 오류로 배치 전체가 죽는 것이 BulkInsertCharges 의 알려진
	//       실패 모드인데, 실패해도 로그·재시도가 없어 이번에 만료된 **모든** 세션의 마감 과금이
	//       사라진다.
	//   (2) 순서 역전 — 종전 순서가 "과금 INSERT 먼저, GPS 확정 나중" 이라, 뒤의 UPDATE 가 실패하면
	//       그 행들이 PROCESSING(2) 에 갇혔다가 stale 회수로 재처리되는데 과금은 이미 커밋된 뒤라
	//       같은 구간이 중복 과금될 수 있다. run() 과 동일하게 GPS 확정을 먼저 한다.
	//   주의: BulkReleaseRawLogs() 는 **자체 BEGIN/COMMIT 을 연다** — 중첩되지
	//   않도록 트랜잭션 밖에서만 호출한다.
	auto fnTtlTxn = [pcConn](const char *pszCmd) -> bool
	{
		PGresult *pcTxnResult = PQexec(pcConn, pszCmd);
		const bool bCmdOk = (pcTxnResult != nullptr)
			&& (PQresultStatus(pcTxnResult) == PGRES_COMMAND_OK);
		if (pcTxnResult != nullptr)
			PQclear(pcTxnResult);
		return bCmdOk;
	};

	const bool bNeedTtlTxn = (!vtExpiredPendingUpdates.empty()
		|| !vtExpiredParkingCharges.empty() || !vtAbnormalEndUpdates.empty());
	bool bTtlDbOk = true;

	if (bNeedTtlTxn && (pcConn != nullptr))
	{
		if (!fnTtlTxn("BEGIN"))
		{
			LOGFMTE("[#%02d] ttl flush txn begin failed!sessions=[%d]", nThreadId, nRemoved);
			bTtlDbOk = false;
		}
		else
		{
			// reserve(rawgps_select) 의 짝 — 보류 행 확정을 먼저(run() 동일 순서)
			if (bTtlDbOk && !vtExpiredPendingUpdates.empty()
				&& !BulkUpdateRawLogs(pcConn, vtExpiredPendingUpdates))
			{
				LOGFMTE("[#%02d] ttl flush bulk update failed!count=[%d]",
					nThreadId, static_cast<int>(vtExpiredPendingUpdates.size()));
				bTtlDbOk = false;
			}
			if (bTtlDbOk && !vtExpiredParkingCharges.empty()
				&& !BulkInsertCharges(pcConn, vtExpiredParkingCharges))
			{
				LOGFMTE("[#%02d] ttl flush charge insert failed!count=[%d]",
					nThreadId, static_cast<int>(vtExpiredParkingCharges.size()));
				bTtlDbOk = false;
			}
			// 마감 UPDATE 도 같은 트랜잭션에 넣는다 — 실패해도 되돌아와 고쳐줄 다음 tick 이 없다(세션 소멸).
			// [2026-10-04 최정우 수정, 사용자 확정 — TTL 마감 권장안 (가)] 종전 [trip_abend] 는 TRIP_END_DT 가
			//   빈 행을 **전부 N/3(61) 로 강등**했다 — 이미 정상 확정돼 INSERT 된 행(일반도로·구간단속 위반 등)
			//   까지 바뀌고 원래 사유(21·33 등)도 61 로 덮였다(시나리오 980015: 일반도로 2~6·구간단속 6~19
			//   Y/0 -> N/3, 폐쇄식 21 -> 61). 판정은 엔진이 INSERT 할 때 한 번만 정한다는 원칙에 맞게, 이제
			//   **[trip_end] 와 같은 UPDATE**(TRIP_END_DT·UPD_DT 만 기록)를 쓴다 — 종료신호 없는 트립 전환
			//   경로와 같은 방식이다. 열려 있던 구간은 위 FlushOpenRunsAsAbnormalEnd() 가 N/3(61) 행으로
			//   직접 INSERT 하므로 빠지는 것이 없다. [trip_abend] SQL·config 키·UpdateAbnormalTripEnd() 삭제.
			// 되돌리는 법: git 이력의 [trip_abend] 섹션·UpdateAbnormalTripEnd() 복원 후 이 호출을 되돌린다.
			if (bTtlDbOk && !vtAbnormalEndUpdates.empty()
				&& !UpdateTripEndDt(pcConn, vtAbnormalEndUpdates))
			{
				LOGFMTE("[#%02d] ttl flush abnormal trip end failed!count=[%d]",
					nThreadId, static_cast<int>(vtAbnormalEndUpdates.size()));
				bTtlDbOk = false;
			}

			if (bTtlDbOk)
			{
				if (!fnTtlTxn("COMMIT"))
				{
					LOGFMTE("[#%02d] ttl flush txn commit failed!sessions=[%d]", nThreadId, nRemoved);
					bTtlDbOk = false;
					fnTtlTxn("ROLLBACK");
				}
			}
			else if (!fnTtlTxn("ROLLBACK"))
			{
				// 롤백 실패 시 예약 행이 2 로 복원되지 않아 아래 release 도 affected=0 이 된다 —
				//   기동 시 rawgps_recover / 주기적 stale_recover 가 회수한다(run() 동일 근거)
				LOGFMTE("[#%02d] ttl flush txn rollback failed!sessions=[%d]", nThreadId, nRemoved);
			}
		}
	}

	if (!bTtlDbOk)
	{
		// 세션 복원 — 플러시 직전 상태로 되돌려 다음 스윕에서 통째로 재시도한다. dtLastSeen 도
		//   그대로라 곧바로 다시 만료 대상이 된다. nChargeSeq 도 함께 되돌아가므로 재시도 시
		//   같은 trip_seq 를 다시 쓴다(이번 INSERT 는 롤백돼 없다).
		for (size_t i = 0; i < vtFlushedSnapshots.size(); ++i)
		{
			VEHICLE_TRIP_SESSION& stRestored = (mapSessions[vtFlushedSnapshots[i].first]
				= vtFlushedSnapshots[i].second);
			// 보류(pending) 소유권은 포기한다 — 아래에서 그 행을 PENDING(0) 으로 반납하고
			//   정상 경로로 재처리시키므로, 복원된 세션이 같은 행을 또 확정하려 들면 안 된다
			//   (그러면 같은 GPS 가 두 번 반영된다). 반납한 행이 재처리되면서 그 tick 의
			//   기여분은 이 세션에 다시 쌓인다.
			stRestored.bHasPendingCommit = false;
		}

		LOGFMTW("[#%02d] ttl flush rolled back!sessions restored=[%d] charges=[%d] updates=[%d]",
			nThreadId, static_cast<int>(vtFlushedSnapshots.size()),
			static_cast<int>(vtExpiredParkingCharges.size()),
			static_cast<int>(vtExpiredPendingUpdates.size()));
		nRemoved = 0;

		// PROCESSING 좀비 방지 — 보류 행을 PENDING(0) 으로 반납해 정상 경로로 재처리시킨다.
		//   트랜잭션 밖에서 호출할 것(BulkReleaseRawLogs 는 자체 BEGIN/COMMIT 을 연다).
		//   [설계 근거, 2026-09-15 최정우 — 결함 주입 검증으로 두 대안을 직접 기각한 뒤 확정]
		//   (a) 반납하면서 세션의 보류 플래그를 안 지우면: 반납된 행이 재처리돼 MATCH_STATUS 가
		//       2 를 벗어나고, 다음 스윕에서 복원된 세션이 같은 행을 또 확정하려다 rawgps_update
		//       0건 매칭으로 실패한다(실측: "bulk update failed" → "bulk release failed" 반복).
		//   (b) 아예 반납하지 않고 MATCH_STATUS=2 로 남겨 세션이 소유를 유지하게 하면: 보류 행의
		//       MATCH_RSV_DT 와 세션의 dtLastSeen 이 사실상 같은 시점에 시작하므로 stale_sec 과
		//       ttl_sec 이 같은 값일 때(운영 기본값도 3600/3600) stale_recover 가 먼저 회수해
		//       같은 실패가 난다(실측 확인).
		//   → 위에서 보류 플래그를 지우고 여기서 반납한다. 행은 정상 경로로 재처리되어 그 tick 의
		//     기여분이 복원된 세션에 다시 쌓이고, 나머지 열린 run 은 다음 스윕에서 재시도된다.
		if (!vtExpiredPendingUpdates.empty() && (pcConn != nullptr)
			&& !BulkReleaseRawLogs(pcConn, vtExpiredPendingUpdates))
		{
			LOGFMTE("[#%02d] ttl flush bulk release failed!count=[%d]",
				nThreadId, static_cast<int>(vtExpiredPendingUpdates.size()));
		}
	}
	// TTL 마감 후 TRIP_SEQ 사후 재부여(UpdateTripSeqOrder) 호출 삭제 (2026-10-04 최정우 삭제 — 사용자 지시: trip_seq 는 SQL 로 처리하지 않고 엔진이 확정할 때 등록한다)

	if (nRemoved > 0)
	{
		LOGFMTD("[#%02d] session ttl expired removed!count=[%d] ttl_sec=[%d]",
			nThreadId, nRemoved, nTtlSec);
	}

	return nRemoved;
}

/**
 * @brief 주정차 구역 경계 통과 시각 보간 — 폴리곤 경계까지의 거리 비율로 실제 통과 시각을 추정
 *   (사용자 지시, 2026-08-24 최정우 추가)
 * @param[in] pstZone 대상 주정차 구역(폴리곤)
 * @param[in] dfInX/dfInY/dtIn 폴리곤 **내부**로 판정된 점의 좌표·시각
 * @param[in] dfOutX/dfOutY/dtOut 폴리곤 **외부**로 판정된 점의 좌표·시각
 * @return 추정된 경계 통과 시각. 보간 근거가 없으면 dtIn 을 그대로 반환(무보정)
 * @remark 등속 직선 이동을 가정하고, 두 점의 경계까지 거리 비율로 나눠 통과 시각을 추정한다.
 *   dfInX/Y 가 실제로는 폴리곤 내부가 아니거나(버퍼만으로 판정됐거나) dfOutX/Y 가 이미 내부이면
 *   근거가 없어 dtIn 을 그대로 돌려준다. dtOut 이 dtIn 보다 이전/이후 어느 쪽이든 무관 —
 *   진입(현재=In, 직전틱=Out, dtOut<dtIn)과 이탈(마지막 재실=In, 첫 이탈틱=Out, dtOut>dtIn)
 *   양쪽에 동일하게 쓴다.
 *   게이트라는 "점"까지의 직선거리로 재는 폐쇄형/구간단속용은 InterpolateGateCrossingTime() 이다.
 *   [2026-09-17 최정우 정정] 이 자리에 AppendExpiredParkingCharge() 설명이 잘못 붙어 있던 것을
 *   그 함수 위로 옮기고, 이 함수 자신의 설명을 제자리에 기재했다(동작 변화 없음).
*/
time_t CRawLogWorker::InterpolateZoneCrossingTime(PZONE_INFO pstZone,
		double dfInX, double dfInY, time_t dtIn, double dfOutX, double dfOutY, time_t dtOut,
		bool *pbInterpolated)
{
	// pbInterpolated — 경계 보간에 성공했는지(실패 시 dtIn 을 돌려주므로 반환값만으로는 "보간 결과가 dtIn 과 같음"과
	//   구분할 수 없다). 2026-10-04 최정우 추가(E20) — 실패 시 호출측이 두 tick 중간 시각을 쓴다.
	if (pbInterpolated != nullptr) *pbInterpolated = false;
	if (pstZone == nullptr) return dtIn;
	if (!CChargeDataLoader::IsPointInPolygon(dfInX, dfInY, pstZone->vtCoords)) return dtIn;
	if (CChargeDataLoader::IsPointInPolygon(dfOutX, dfOutY, pstZone->vtCoords)) return dtIn;

	double dfInDist = CChargeDataLoader::DistanceToPolygonBoundaryMeters(dfInX, dfInY, pstZone->vtCoords);
	double dfOutDist = CChargeDataLoader::DistanceToPolygonBoundaryMeters(dfOutX, dfOutY, pstZone->vtCoords);
	double dfTotal = dfInDist + dfOutDist;
	if (dfTotal <= 0.0) return dtIn;

	double dfFrac = dfInDist / dfTotal;					// In 이 경계에서 먼 비율만큼 Out 쪽으로 이동
	double dfDeltaSec = difftime(dtOut, dtIn) * dfFrac;
	if (pbInterpolated != nullptr) *pbInterpolated = true;
	return dtIn + static_cast<time_t>(dfDeltaSec + (dfDeltaSec >= 0.0 ? 0.5 : -0.5));
}

/**
 * @brief 구역 선(LINE) 위 두 지점 사이 거리를 **선을 따라** 잰다 (2026-10-04 최정우 추가, 사용자 확정)
 * @param[in] pstZone 폐쇄형·구간단속 구역(vtLineCoords 사용)
 * @param[in] dfInLon/dfInLat 시작점(진입 게이트, 진입 미확인이면 실제 출발 지점)
 * @param[in] dfOutLon/dfOutLat 끝점(진출 게이트)
 * @param[in] dfFallbackM 선을 못 쓸 때 돌려줄 종전 값
 * @return 두 점을 선에 투영한 진행거리의 차(m)
 * @remark 사용자 기준: "게이트가 있는 과금 도로는 진입 게이트 ~ 진출 게이트 사이 거리, 그 거리 ÷ (진출 시각 −
 *   진입 시각) 이 평균속도". 종전에는 구역 전체 등록 길이(dfLengthM)를 써서 게이트가 선 끝에 있지 않은 구역은
 *   과다했다 — RL-Z00008(강릉 폐쇄식) 등록 3,587.2m vs 게이트 간 3,487.3m(+99.9m). 진입 게이트를 못 잡은
 *   경우의 종전 값(출발 지점~출구 게이트 **직선**)도 같은 방식(선을 따라)으로 바꿨다.
 *   선 정점이 2개 미만이거나 어느 한 점이라도 선에서 MM_ZONE_SPAN_MAX_OFF_M 넘게 떨어지면 dfFallbackM.
 *   시각은 종전대로 게이트 통과 보간(InterpolateGateCrossingTime) — 게이트에 tick 이 없을 때도 같다.
 * 되돌리는 법: 호출 4곳(폐쇄형·구간단속 × 정상/SKIP 이탈)을 종전 dfLengthM·직선거리로 되돌린다.
*/
static double CalcZoneSpanM(const ZONE_INFO *pstZone, double dfInLon, double dfInLat,
	double dfOutLon, double dfOutLat, double dfFallbackM)
{
	static const double MM_ZONE_SPAN_MAX_OFF_M = 30.0;
	if ((pstZone == nullptr) || (pstZone->vtLineCoords.size() < 2))
		return dfFallbackM;
	const vector<POINT>& vtPts = pstZone->vtLineCoords;
	// 점 하나를 선에 투영해 시작점부터의 진행거리를 구한다(국소 평면 근사, 선분 길이는 하버사인)
	auto fnProgress = [&vtPts](double dfLon, double dfLat, double *pdfOff) -> double
	{
		double dfBestOff = 1e18, dfBestPos = 0.0, dfAcc = 0.0;
		const double dfCosLat = cos(dfLat * M_PI / 180.0);
		for (size_t i = 0; i + 1 < vtPts.size(); ++i)
		{
			const double ax = (vtPts[i].dfX - dfLon) * 111320.0 * dfCosLat, ay = (vtPts[i].dfY - dfLat) * 110574.0;
			const double bx = (vtPts[i + 1].dfX - dfLon) * 111320.0 * dfCosLat, by = (vtPts[i + 1].dfY - dfLat) * 110574.0;
			const double dx = bx - ax, dy = by - ay, dd = dx * dx + dy * dy;
			double t = (dd > 0.0) ? (-(ax * dx + ay * dy) / dd) : 0.0;
			if (t < 0.0) t = 0.0;
			if (t > 1.0) t = 1.0;
			const double qx = ax + t * dx, qy = ay + t * dy;
			const double dfOff = sqrt(qx * qx + qy * qy);
			// 선분 길이 — 하버사인(CRawLogWorker::HaversineMeters 는 private 라 같은 식·같은 반경(6378137, [zone_select] LENGTH_M 과 동일)을 직접 쓴다)
			const double p1 = vtPts[i].dfY * M_PI / 180.0, p2 = vtPts[i + 1].dfY * M_PI / 180.0;
			const double dp = p2 - p1, dl = (vtPts[i + 1].dfX - vtPts[i].dfX) * M_PI / 180.0;
			const double h = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
			const double dfSeg = 2.0 * 6378137.0 * asin(sqrt(h));
			if (dfOff < dfBestOff) { dfBestOff = dfOff; dfBestPos = dfAcc + t * dfSeg; }
			dfAcc += dfSeg;
		}
		*pdfOff = dfBestOff;
		return dfBestPos;
	};
	double dfOffIn = 0.0, dfOffOut = 0.0;
	const double dfPosIn = fnProgress(dfInLon, dfInLat, &dfOffIn);
	const double dfPosOut = fnProgress(dfOutLon, dfOutLat, &dfOffOut);
	if ((dfOffIn > MM_ZONE_SPAN_MAX_OFF_M) || (dfOffOut > MM_ZONE_SPAN_MAX_OFF_M))
		return dfFallbackM;
	return fabs(dfPosOut - dfPosIn);
}

/**
 * @brief 폐쇄형/구간단속 게이트 통과 시각 보간 — 직전 확정 tick~현재 tick 사이 등속 직선 이동을
 *   가정, 게이트까지의 직선거리 비율로 그 구간 안 실제 통과 시각을 추정한다 (2026-08-25 최정우 추가)
 * @remark InterpolateZoneCrossingTime() 과 원리는 같으나(비율 보간) 그쪽은 폴리곤 경계까지의
 *   거리(주정차 전용), 이쪽은 게이트라는 "점"까지의 직선거리(폐쇄형/구간단속 전용) 기준이라
 *   구분함. dfPrevX/Y~dfCurX/Y 거리가 0이면(같은 좌표 재수신 등) 보간 근거가 없어 dtCur 을 그대로
 *   반환한다. 비율은 0~1 로 클램프 — 게이트가 두 tick 의 이동 경로 밖(직선상 연장선)에 있는 것처럼
 *   계산되는 경우(맵매칭 곡선경로 vs 직선거리 오차)에도 dtPrev/dtCur 범위를 벗어나지 않게 한다.
 */
time_t CRawLogWorker::InterpolateGateCrossingTime(double dfPrevX, double dfPrevY, time_t dtPrev,
		double dfCurX, double dfCurY, time_t dtCur, double dfGateX, double dfGateY)
{
	POINT stPrev, stCur, stGate;
	stPrev.dfX = dfPrevX;  stPrev.dfY = dfPrevY;
	stCur.dfX = dfCurX;    stCur.dfY = dfCurY;
	stGate.dfX = dfGateX;  stGate.dfY = dfGateY;

	double dfPrevToCur = HaversineMeters(stPrev, stCur);
	if (dfPrevToCur <= 0.0) return dtCur;

	double dfPrevToGate = HaversineMeters(stPrev, stGate);
	double dfFrac = dfPrevToGate / dfPrevToCur;
	if (dfFrac < 0.0) dfFrac = 0.0;
	if (dfFrac > 1.0) dfFrac = 1.0;

	double dfDeltaSec = difftime(dtCur, dtPrev) * dfFrac;
	return dtPrev + static_cast<time_t>(dfDeltaSec + (dfDeltaSec >= 0.0 ? 0.5 : -0.5));
}

bool CRawLogWorker::BuildParkRow(const PARK_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, double dfEndX, double dfEndY,
		uint32 dwEndGpsSeq, const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow)
{
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);
	double dfDwellSec = difftime(dtEnd, stRun.dtEntryTime);

	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "4";							// PARKING
	pstRow->strChargeUnit = "2";							// POLYGON
	pstRow->strLinkId = "";
	pstRow->strFromId = stRun.szRoadID;
	pstRow->strToId = stRun.szRoadID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", dfEndY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", dfEndX);
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	pstRow->strZoneId = stRun.szRoadID;
	pstRow->strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stRun.dfAccumDistM + 0.5));
	pstRow->strDistM = szDistM;

	if (dfDwellSec > 0.0)
	{
		double dfAvgSpeedKmh = (stRun.dfAccumDistM / dfDwellSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", stRun.dwEntryGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwEndGpsSeq);
	pstRow->strStartGpsSeq = szStartGpsSeq;
	pstRow->strEndGpsSeq = szEndGpsSeq;

	// OCCUR_DT — 주정차는 "진입 시각"(RawLogWorker 관례). 일반도로만 진출 시각을 쓴다
	pstRow->strOccurDt = FormatDateTime14(stRun.dtEntryTime);
	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	pstRow->strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : pstRow->strOccurDt;

	pstRow->strTollgateId = "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strRegDt;
	pstRow->strChargeYn = pszChargeYn;
	pstRow->strChargeStatus = pszChargeStatus;

	// base_parking_fine 최소 from_min(분) 미만 체류는 등록 대상에서 제외 — min_sec=0 이면
	//   임계 비활성(테이블 비어있거나 미로드), 항상 등록 (사용자 지시, 2026-08-24 최정우 추가)
	int nMinSec = m_stConfig.pcChargeDataLoader->GetParkFineMinSec();
	return (nMinSec <= 0) || (dfDwellSec >= static_cast<double>(nMinSec));
}

/**
 * @brief 강제마감 시점에 아직 열려있는 주정차 세션을 위반 1건으로 마감 (2026-08-13 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 마감 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 위반 확정 시 CHARGE_INSERT_ROW 1건 추가(체류가 base_parking_fine 최소시간
 *   미만이면 추가하지 않는다 — BuildParkRow() 반환값)
 * @param[in] bNoTripEnd true=종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
 *   non_charge_reason 62. false=TTL·잔여tick·트립종료(61) (2026-09-16 최정우 추가)
 * @remark stRawLogInfo(현재 GPS 틱)가 없는 컨텍스트라 ProcessParkingCharge() 종료 블록과 로직은
 *   같지만 필드 출처가 다름(트립종료시각 대신 dtLastSeen, occur_dt는 세션의 진입시각 그대로).
 *   실제 TRIP_EVENT=2 를 받은 적이 없어 [trip_end] UPDATE 가 이 trip_id 를 앞으로도 채워줄 일이
 *   없으므로, trip_end_dt 는 **같은 배치의 [trip_end] UPDATE 가 채운다**(2026-10-04 정정 — 종전 [trip_abend])(이 함수도 BuildParkRow()
 *   도 TRIP_END_DT 를 직접 채우지 않는다 — 실제로 INSERT 시점에 채우는 건 폐쇄형 강제마감과
 *   구간단속 NODE_STEP 미러 두 곳뿐이다. 2026-09-17 최정우 정정: 종전 주석은 "여기서 직접
 *   dtLastSeen 으로 채운다"고 했으나 그런 코드가 없다).
 *   확정 데이터가 아님을 표시하기 위해 charge_yn='N'/charge_status='3'(AUDIT) 명시 — 정상 이탈/
 *   트립종료 경로의 'Y'/'0' 과 다르다.
 *   [2026-09-17 최정우 정정] 종전 주석은 이 블록이 InterpolateZoneCrossingTime() 위에 잘못 붙어
 *   있었고 charge_status 를 '4'(SKIP)로 적었으나, 실제 코드는 도입 시점부터 'N'/'3'(AUDIT)이다
 *   (SKIP(4)를 쓰는 유형은 면제도로뿐). 주석만 사실에 맞춰 정정 — 동작 변화 없음.
 *   [2026-09-15 적용범위 확대] 함수명은 "Expired" 지만 TTL 만료 전용이 아니다 —
 *   FlushOpenRunsAsAbnormalEnd() 를 통해 트립종료 이후 잔여 tick·종료신호 없는 트립 전환·
 *   서버 종료 경로에서도 호출된다.
*/
void CRawLogWorker::AppendExpiredParkingCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut, bool bNoTripEnd)
{
	// 마감 사유 코드 — 종료신호 없이 다음 운행이 시작된 경로면 62(면제도로 52), 그 밖의
	//   마감(TTL·잔여tick·트립종료)이면 61(51). 판정(charge_yn/status)은 동일하다
	//   (2026-09-16 최정우 추가)
	const int nReasonCode = bNoTripEnd ? NCR_NO_TRIP_END_FORCED_CLOSE : NCR_TTL_FORCED_CLOSE;

	if (stSession.vtParkRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 겹쳐 진행 중이던 구역을 전부 마감한다. 체류 종료는 dtLastSeen — TTL 만료는 그 이후로 GPS가
	//   안 왔다는 뜻이라 "그 시점까지는 확실히 있었다"만 서버가 아는 전부 (2026-08-23 최정우 수정)
	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtParkRuns.size(); ++i)
	{
		const PARK_RUN_SESSION& stRun = stSession.vtParkRuns[i];
		const int nThisSeq = nSeq;						// 이 run 에 배정한 순번 (로그·행 생성 공용)
		CHARGE_INSERT_ROW stRow;
		// [2026-10-04 최정우 추가 — 정책 대조표 B10, 사용자 확정] 체류 종료는 dtLastSeen(구역 밖 tick
		//   포함 — 이탈 디바운스 중이면 구역을 이미 나간 시각까지 들어간다)이 아니라 **구역 안에서
		//   마지막으로 관측된 tick** 이다. TTL 만료 시각은 서버가 늦게 알아챈 시각이라 쓰지 않는다.
		//   판정(N/3·61/62)은 그대로. 구역 안 tick 기록이 없으면(0) 종전 값으로 폴백.
		// 되돌리는 법: bInZoneEnd 를 false 로 고정하면 종전(dtLastSeen) 동작이다.
		const bool bInZoneEnd = (stRun.dtLastInZoneTime > 0);
		bool bMeetsFineMin = bInZoneEnd
			? BuildParkRow(stRun, stSession.szTripId, strDeviceKey, nThisSeq, stRun.dtLastInZoneTime,
				stRun.dfLastInZoneX, stRun.dfLastInZoneY, stRun.dwLastInZoneGpsSeq, "N", "3", &stRow)
			: BuildParkRow(stRun, stSession.szTripId, strDeviceKey, nThisSeq, stSession.dtLastSeen,
				stRun.dfLastX, stRun.dfLastY, stSession.dwLastGpsSeq, "N", "3", &stRow);
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(N/3)은 안 건드림
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", nReasonCode);
		stRow.strNonChargeReason = szReason;

		// [버그 수정, 2026-09-21 최정우] **등록한 행만 순번을 소비한다.** 종전에는 push 여부와
		//   무관하게 nSeq 를 올려서, 체류가 base_parking_fine 최소시간(현재 5분) 미만이라 등록되지
		//   않은 run 도 순번 하나를 먹었다. 그런데 호출측(FlushOpenRunsAsAbnormalEnd)은
		//   **실제로 늘어난 행 수**만큼만 세션의 nChargeSeq 를 올린다 — 둘이 어긋난다.
		//   겹친 주정차 구역 2개가 동시에 열려 있고 앞쪽이 최소시간 미달이면:
		//     run A(미달, seq 1 소비·미등록) → run B(등록, seq **2** 사용) → pvtOut 증가는 1건
		//     → 호출측 nChargeSeq = 1 + 1 = **2** → 다음 유형(면제·일반도로 등)이 seq 2 로 시작
		//     → run B 와 PK(trip_id, device_key, trip_seq) 충돌 → ON CONFLICT DO NOTHING 으로
		//        **그 행이 로그도 없이 사라진다.**
		//   나머지 5개 Append*(Exempt/NodeStep/Open/Closed/Speed)는 전부 "무조건 push 후 +1" 이라
		//   이 어긋남이 없다 — 조건부 push 는 이 함수뿐이라 여기만 규칙이 깨져 있었다.
		//   현재 로컬 기준정보에는 POLY 구역이 2개이고 서로 겹치지 않아 발현하지 않지만,
		//   겹침은 이 코드가 명시적으로 지원하는 상황이다(vtParkRuns 가 vector 인 이유,
		//   PARK_RUN_SESSION 선언부 주석 참고) — 기준정보에 겹친 구역이 등록되는 순간 발현한다.
		//   ※ 미등록 run 이 순번을 안 먹으므로 결번도 생기지 않는다(호출측 계산과 정확히 일치).
		if (bMeetsFineMin)
		{
			pvtOut->push_back(stRow);
			nSeq += 1;
		}

		LOGFMTW("[#%02d] parking expired!device=[%s] trip_id=[%s] seq=[%d] road=[%s] dwell=[%s]s "
			"registered=[%d] non_charge_reason=[%d:%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId, nThisSeq, stRun.szRoadID,
			stRow.strStaySeconds.c_str(), static_cast<int>(bMeetsFineMin), nReasonCode,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nReasonCode));
	}
}

/**
 * @brief park_ttl — 열려있는 주정차 세션의 마지막 신뢰(raw_vld=true) 확인 이후 일정 시간이
 *   지나면 좌표 확인 없이 그 시점 기준으로 강제 마감 (2026-08-19 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in,out] pstSession 대상 세션 — 마감 후 park_* 필드만 초기화(디바이스 세션 자체는 유지)
 * @param[out] pvtOut CHARGE_INSERT_ROW 1건 추가
 * @remark AppendExpiredParkingCharge() 와 다른 점: 그건 디바이스 세션 전체가 TTL로 사라질 때
 *   (GPS 자체가 끊김) 쓰는 경로라 trip_end_dt 를 직접 채우고 세션을 지운다. 이 함수는 GPS는
 *   계속 들어오는데(raw_vld=false 라도) 신뢰 가능한 위치 확인만 오래 끊긴 경우라 트립이 계속
 *   진행 중일 수 있음 — trip_end_dt 는 비워두고([trip_end] UPDATE 가 나중에 채움), 세션도
 *   지우지 않고 park_* 필드만 리셋해 다음 구역 진입을 정상적으로 받을 수 있게 한다. 체류시간·
 *   종료위치는 ProcessParkingCharge() 의 raw_vld=false 마감 경로와 동일하게 마지막 신뢰
 *   시각·좌표(dtParkLastConfirmedTime/dfParkLastConfirmedX/Y) 기준 — 그래서 charge_yn/status 도
 *   그 경로와 동일하게 정상값('Y'/'0')을 쓴다(AppendExpiredParkingCharge 의 'N'/'3' 과 다름 —
 *   거긴 트립 자체가 유실된 상황이라 심사대상으로 표시하지만, 여긴 마지막 신뢰 시점까지는
 *   확실한 근거가 있는 정상 기록이기 때문).
*/
void CRawLogWorker::AppendStaleParkingCharge(int nThreadId, const string& strDeviceKey,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if ((pstSession == nullptr) || pstSession->vtParkRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// park_ttl 초과 — 마지막 신뢰(raw_vld=true) 확인 시점까지만 체류로 인정하고 강제 마감한다.
	//   디바이스 세션 자체는 지우지 않는다(GPS 는 계속 들어오고 다른 유형 세션은 유효).
	//   정상 마감 경로와 동일하게 Y/0 로 기록 (2026-08-23 최정우 수정 — 구역별로 처리)
	// "지금"은 서버 벽시계가 아니라 이 세션이 처리해온 GPS 타임라인상 최신 시각(dtLastGpsEventTime)을
	//   쓴다 — 실시간 운영에서는 둘이 거의 같아 동작이 그대로지만, 과거 데이터를 벌크로 재처리할
	//   때는 벽시계(예: 지금 19시)가 재처리 중인 GPS 시각(예: 그날 15시)보다 몇 시간 앞서 있어
	//   park_ttl(600초)을 사실상 항상 즉시 초과한 것으로 오판, 연속된 주정차 1건이 여러 건으로
	//   쪼개지는 버그가 있었다(실측 000376_20260826150010 — 이동·이탈 없이 정지 상태로 6초 간격
	//   두 레코드로 분리됨). dtLastGpsEventTime==0(비정상 경로)이면 벽시계로 폴백 (2026-08-26 최정우 추가)
	const time_t dtNow = (pstSession->dtLastGpsEventTime > 0) ? pstSession->dtLastGpsEventTime : time(nullptr);
	for (size_t i = 0; i < pstSession->vtParkRuns.size(); )
	{
		PARK_RUN_SESSION& stRun = pstSession->vtParkRuns[i];
		if ((stRun.dtLastConfirmedTime <= 0) || (m_stConfig.nParkTtlSec <= 0)
			|| ((dtNow - stRun.dtLastConfirmedTime) <= static_cast<time_t>(m_stConfig.nParkTtlSec)))
		{ ++i; continue; }

		CHARGE_INSERT_ROW stRow;
		// [2026-10-04 최정우 수정, 사용자 확정(권장안) — E21] 종료 시각을 마지막 RAW_VLD=true tick 에서 **폴리곤 안으로
		//   판정된 마지막 tick(RAW_VLD 무관)** 으로 바꿨다 — "주정차 체류시간은 모든 GPS 수신 시각 기준" 정의와 맞추고,
		//   다른 마감 경로(TTL·B10: 구역 안 마지막 tick)와 기준을 같게 한다. 마감 시작 조건(마지막 RAW_VLD=true 확인 후
		//   park_ttl 초 경과)과 판정 값(Y/0)은 그대로다. 안쪽 마지막 tick 정보가 없거나 더 이르면 종전 값.
		// 되돌리는 법: 아래 세 값을 dtLastConfirmedTime·dfLastConfirmedX/Y·dwLastConfirmedGpsSeq 로 되돌린다.
		const bool bUseLastIn = (stRun.dtLastInZoneTime > 0) && (stRun.dtLastInZoneTime >= stRun.dtLastConfirmedTime);
		const time_t dtStaleEnd = bUseLastIn ? stRun.dtLastInZoneTime : stRun.dtLastConfirmedTime;
		const uint32 dwStaleEndSeq = bUseLastIn ? stRun.dwLastInZoneGpsSeq : stRun.dwLastConfirmedGpsSeq;
		bool bMeetsFineMin = BuildParkRow(stRun, pstSession->szTripId, strDeviceKey, pstSession->nChargeSeq,
			dtStaleEnd, bUseLastIn ? stRun.dfLastInZoneX : stRun.dfLastConfirmedX,
			bUseLastIn ? stRun.dfLastInZoneY : stRun.dfLastConfirmedY,
			dwStaleEndSeq, "Y", "0", &stRow);
		if (bMeetsFineMin) pvtOut->push_back(stRow);

		// [사용자 지시, 2026-09-11 최정우] Y/0(정상) 도 N/x 와 동일한 형식으로 INFO 로그에
		//   non_charge_reason=[0:정상 과금] 을 남긴다 — WARN 승격 없이 조회용 상수(NCR_NORMAL)만
		//   추가. DB non_charge_reason 컬럼에도 0 이 들어간다 — strNonChargeReason 을 빈 값으로 두면
		//   [charge_insert]가 0(NCR_NORMAL)으로 변환한다(2026-09-15 최정우 수정, 사용자 지시 —
		//   종전엔 NULL 이었음). 여기서 별도 대입은 불필요
		LOGFMTI("[#%02d] parking stale finalized!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"dwell=[%s]s registered=[%d] non_charge_reason=[%d:%s]",
			nThreadId, strDeviceKey.c_str(), pstSession->szTripId, pstSession->nChargeSeq, stRun.szRoadID,
			stRow.strStaySeconds.c_str(), static_cast<int>(bMeetsFineMin), NCR_NORMAL,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));

		pstSession->nChargeSeq += 1;
		pstSession->vtParkRuns.erase(pstSession->vtParkRuns.begin() + i);
	}
}

/**
 * @brief 면제도로(EXEMPT, CHARGE_TYPE=5) 과금 1행 조립 — 정상 이탈과 강제마감 공용
 *   (2026-08-23 최정우 추가)
 * @param[in] stRun 대상 run(진입점·누적거리·마지막 좌표를 담은 ZONE_RUN_SESSION)
 * @param[in] strTripId / strDeviceKey / nChargeSeq 행 식별자
 * @param[in] dtEnd 진출 시각 — 체류시간·평균속도의 분모 기준(경과 1초 하한)
 * @param[in] dwEndGpsSeq dtEnd 와 같은 tick 의 GPS_SEQ — END_GPS_SEQ
 * @param[in] pszChargeYn / pszChargeStatus 호출측 지정 — 정상 이탈은 Y/0, 강제마감은 N/4
 * @param[out] pstRow 조립된 과금 행
 * @remark charge_type="5"·from_id/to_id=zone road_id 는 2026-08-14 재설계를 세 차례 거쳐 확정된
 *   최종 형태다("모든 미등록 링크" 방식 폐기 → zone 기반 복귀, 한때 "0"+링크ID 였다가 사용자
 *   재지시로 원복). OCCUR_DT 는 **진입 시각**이다(2026-08-30 수정 — 이전에는 면제도로만 진출
 *   시각이었다). 일반도로만 진출 시각을 쓴다.
 *   charge_yn/status 를 호출측이 정하는 이유는 BuildParkRow()/BuildNodeStepRow() 와 같다 —
 *   종전에는 이 함수가 정상/비정상 구분 없이 항상 N/4 로 고정해 정상 통행까지 "확정 데이터
 *   아님"으로 남았다(사용자 지시, 2026-08-30 최정우 수정).
 *   [2026-09-17 최정우 정정] 이 자리에 AppendExpiredExemptZoneCharge() 설명이 잘못 붙어 있던
 *   것을 그 함수 위로 옮기고, 이 함수 자신의 설명을 제자리에 기재했다(동작 변화 없음).
*/
void CRawLogWorker::BuildExemptRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, uint32 dwEndGpsSeq,
		const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow)
{
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);

	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "5";							// 비과금도로 고유 charge_type(사용자 재지시, 2026-08-14)
	pstRow->strChargeUnit = "1";							// LINE 유형 — 폐쇄형·구간단속과 동일하게 LINK
	pstRow->strLinkId = "";
	pstRow->strFromId = stRun.szRoadID;					// base_roadlink 등록 road_id
	pstRow->strToId = stRun.szRoadID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", stRun.dfLastY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", stRun.dfLastX);
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	pstRow->strZoneId = stRun.szRoadID;
	pstRow->strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stRun.dfAccumDistM + 0.5));
	pstRow->strDistM = szDistM;

	// 경과시간 1초 하한 — 폐쇄식·구간단속과 동일 관례. 구역 안 tick 이 1개뿐이면 0초가 되어
	//   speed_kmh 가 아예 안 채워지던 문제를 막는다 (사용자 지시, 2026-08-30 최정우 추가)
	double dfElapsedSec = difftime(dtEnd, stRun.dtEntryTime);
	if (dfElapsedSec < 1.0)
		dfElapsedSec = 1.0;
	{
		double dfAvgSpeedKmh = (stRun.dfAccumDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", stRun.dwEntryGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwEndGpsSeq);
	pstRow->strStartGpsSeq = szStartGpsSeq;
	pstRow->strEndGpsSeq = szEndGpsSeq;

	// OCCUR_DT — 진입 시각. 개방형·폐쇄식·주정차와 동일 관례(일반도로만 진출 시각을 쓴다)
	//   (사용자 지시, 2026-08-30 최정우 수정 — 이전에는 면제도로만 진출 시각이었다)
	pstRow->strOccurDt = FormatDateTime14(stRun.dtEntryTime);
	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	pstRow->strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : pstRow->strOccurDt;

	pstRow->strTollgateId = "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strRegDt;		// UPD_DT=최종 수정 일시 — INSERT 시점엔 REG_DT 와 같다(2026-10-04 최정우 수정, 종전 OCCUR_DT)
	// charge_yn/status — 호출측 지정. 정상 이탈·트립종료는 Y/0, TTL 만료 강제마감은 N/4.
	//   다른 유형이 TTL flush 를 C++ 에서 직접 N/3 으로 세팅하는 것과 동일한 구조이며, 면제도로만
	//   AUDIT(3) 이 아니라 SKIP(4) 를 쓴다 — 과금 대상이 아니라 사람이 재확인할 필요가 없다는
	//   설계 의도. (종전 [trip_abend] 의 CHARGE_TYPE=5 분기와 값이 같았다 — 그 SQL 은 2026-10-04 삭제)
	//   (사용자 지시, 2026-08-30 최정우 수정 — 이전에는 여기서 항상 N/4 로 고정해 정상 통행까지
	//    "확정 데이터 아님"으로 남았다)
	pstRow->strChargeYn = pszChargeYn;
	pstRow->strChargeStatus = pszChargeStatus;
}

/**
 * @brief 강제마감 시점에 아직 열려있는 면제도로 세션을 N/4(SKIP)로 마감 (2026-08-13 최초 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 마감 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 진행 중이던 run 마다 CHARGE_INSERT_ROW 1건 추가
 * @param[in] bNoTripEnd true=종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감 —
 *   non_charge_reason 52. false=TTL·잔여tick·트립종료(51) (2026-09-16 최정우 추가)
 * @remark 정상 이탈·트립종료는 ProcessExemptZoneCharge() 가 그 자리에서 Y/0 으로 기록하므로,
 *   이 함수는 그 신호(구역 이탈/트립종료)가 영영 안 오고 GPS 자체가 끊긴 경우만 대신 마감해주는
 *   보완 경로다 — 여기서는 N/4 로 INSERT 한다. 다른 유형이 강제마감을 N/3(AUDIT)으로 세팅하는
 *   것과 같은 구조이며, 면제도로만 SKIP(4)를 쓴다(과금 대상이 아니라 사람이 재확인할 필요가
 *   없다는 설계 의도 — 종전 [trip_abend] 의 CHARGE_TYPE=5 분기와 값이 같았다, 그 SQL 은 2026-10-04 삭제).
 *   [2026-09-15 적용범위 확대] 함수명은 "Expired" 지만 TTL 만료 전용이 아니다 —
 *   FlushOpenRunsAsAbnormalEnd()/FlushOpenExemptRunsAtTripEnd() 를 통해 트립종료 이후 잔여 tick·
 *   종료신호 없는 트립 전환·서버 종료 경로에서도 호출된다.
 *   [2026-09-17 최정우 정정] 종전엔 이 설명 블록이 BuildExemptRow() 위에 붙어 있었다(동작 무관).
*/
void CRawLogWorker::AppendExpiredExemptZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut, bool bNoTripEnd)
{
	// 마감 사유 코드 — 종료신호 없이 다음 운행이 시작된 경로면 62(면제도로 52), 그 밖의
	//   마감(TTL·잔여tick·트립종료)이면 61(51). 판정(charge_yn/status)은 동일하다
	//   (2026-09-16 최정우 추가)
	const int nReasonCode = bNoTripEnd ? NCR_EXEMPT_NO_TRIP_END : NCR_EXEMPT_TTL_FORCED_CLOSE;

	if (stSession.vtExemptRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtExemptRuns.size(); ++i)
	{
		CHARGE_INSERT_ROW stRow;
		BuildExemptRow(stSession.vtExemptRuns[i], stSession.szTripId, strDeviceKey,
			nSeq, stSession.vtExemptRuns[i].dtLastInZoneTime,
			stSession.vtExemptRuns[i].dwLastInZoneGpsSeq, "N", "4", &stRow);
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(N/4)은 안 건드림
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", nReasonCode);
		stRow.strNonChargeReason = szReason;
		pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTW("[#%02d] exempt zone expired!device=[%s] trip_id=[%s] seq=[%d] road=[%s] dist_m=[%s] "
			"non_charge_reason=[%d:%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId, nSeq - 1,
			stSession.vtExemptRuns[i].szRoadID, stRow.strDistM.c_str(), nReasonCode,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nReasonCode));
	}
}

/**
 * @brief 일반도로(NODE_STEP, CHARGE_TYPE=0) 과금 1행 조립 — 모든 일반도로 레코드의 공통 출구
 * @param[in] stRun 대상 run(진입점·누적거리·마지막 링크/좌표를 담은 ZONE_RUN_SESSION)
 * @param[in] strTripId TRIP_ID
 * @param[in] strDeviceKey DEVICE_KEY
 * @param[in] nChargeSeq 행 생성 시점의 임시 순번 — DB 의 TRIP_SEQ 는 큐 방출 때 다시 매긴다
 *   ([2026-09-22 최정우 정정] 종전의 "미등록 이벤트도 카운터를 소비 — 결번이 정상" 은 폐기됐다.
 *    ReleaseChargeQueue() 가 **실제로 INSERT 되는 행에만** 번호를 주므로 결번이 생기지 않는다)
 * @param[in] dtEnd 진출 시각 — OCCUR_DT 및 평균속도의 분모(경과시간) 기준
 * @param[in] dwEndGpsSeq dtEnd 와 같은 tick 의 GPS_SEQ — END_GPS_SEQ
 * @param[in] pszChargeYn / pszChargeStatus 호출측이 정한 CHARGE_YN / CHARGE_STATUS
 * @param[out] pstRow 조립된 과금 행
 *
 * @remark ── 일반도로 레코드 적재 규칙 (2026-09-06 최정우 정리, 사용자 지시 — 로직 수정 시 참고) ──
 *   이 블록이 일반도로 규칙의 **정본**이다(doc/과금_맵매칭_로직_색인.md 는 색인일 뿐이다).
 *   [2026-09-17 최정우 정리] 항목이 1·2·3·6·4·7·5 순서로 섞여 있던 것을 번호순으로 재배열했다 —
 *   본문은 한 글자도 바꾸지 않았고 항목 간 상호참조("아래 6번의 억제" 등)도 번호 기준이라 그대로
 *   유효하다. 동작 변화 없음.
 *
 * \t**1. 대상 범위** — 과금 구역 테이블(BASE_ROADLINK)에 **등록되지 않은 도로도 일반도로로 과금
 * \t한다.** 등록 구역이 아니라는 이유로 빠지지 않으며, 하나의 run 이 미등록↔등록구역A↔등록구역B 를
 * \t넘나들며 이어질 수 있다. 그래서 ZONE_ID/ZONE_NAME 은 비우고 FROM_ID/TO_ID 에 **게이트가 아니라
 * \t링크 ID** 를 넣는다(게이트 개념이 없는 유형이므로 TOLLGATE_ID/ENTRY/EXIT 는 항상 빈 값).
 * \t  **게이트형 구역(1 개방식·2 폐쇄식·3 구간단속)에 등록된 링크도, 그 구역 run 이 열려 있지
 * \t  않은 동안은 일반도로다**(2026-09-07 추가, 사용자 지시). 게이트형은 "게이트를 통과한 구간"만
 * \t  그 유형으로 과금되므로, 게이트를 통과하지 않았거나 이미 진출한 뒤에도 같은 링크에 매칭되는
 * \t  tick 은 그 유형에 계상되지 않는다. 예전에는 "등록 링크"라는 이유만으로 미등록 pseudo-zone
 * \t  대상에서도 빠져(road_kind=0 정식구역도 아니므로 vtZones 에도 안 잡힘) 어떤 run 에도 못 들어간
 * \t  채 사라졌다 — 실측 000376_20260819140532 seq48·49(구간단속 RL-Z00003 의 유일 링크
 * \t  2040424301, seq47 뒤 진출게이트 TG00013 통과로 run 이 닫혔는데도 계속 그 링크에 매칭)가
 * \t  누락되어 일반도로가 23~47 / 51~53 으로 쪼개졌다. 수정 후 23~53 한 레코드다.
 * \t  판정은 IsLinkNodeStepEligible() 하나로 모았다 — 되돌리는 법은 그 함수 주석 참고.
 * \t  면제(5)·일반도로(0) 등록 링크는 흡수하지 않는다(면제를 흡수하면 면제 구간이 과금된다).
 *
 * \t**2. 산출 재료** — 게이트도 구역 등록 길이(ZONE_INFO.dfLengthM)도 없으므로 전부 GPS 실측이다.
 * \tDIST_M = run 누적 이동거리(dfAccumDistM), STAY_SECONDS = dtEnd − 진입시각, SPEED_KMH = 그 둘의
 * \t역산(평균속도). OCCUR_DT 는 **일반도로만 진출 시각**을 쓴다(다른 유형은 진입 시각).
 *
 * \t**3. 한 레코드로 합쳐지는 경로 3가지** — 일반도로는 "연속 주행 구간 하나 = 레코드 하나"가
 * \t원칙이라, 아래 상황에서 쪼개지지 않도록 병합한다. 쪼개면 같은 주행이 여러 건으로 과금된 것처럼
 * \t보이고 평균속도도 구간마다 따로 계산돼 왜곡된다.
 * \t  (a) **구간단속 미러 인수인계** — 구간단속(3) 마감 시 같은 구간의 일반도로 미러를 즉시
 * \t      등록하지 않고 stHeldSpeedMirrorRun 에 **보류**한다. 그 다음이 주정차 폴리곤과 만나면
 * \t      ProcessNodeStepCharge() 의 인수인계 로직이 보류분의 진입정보·누적거리를 그대로 이어받아
 * \t      하나로 합친다(미러의 마지막 링크 == 직전 확정 링크일 때).
 * \t  (b) **보류 1 tick 지연** — 미러를 보류한 그 tick 에서는 등록하지 않는다. 구간단속 출구 통과
 * \t      tick 과 폴리곤 진입 tick 이 한 칸 어긋나면(실측 000376_20260821095239 seq11/seq12) 같은
 * \t      tick 에 등록해버려 이어받을 미러가 사라지기 때문. 유실 위험은 없다 — TTL 만료·세션 종료·
 * \t      트립 종료 3경로가 모두 남은 보류분을 흘려보낸다.
 * \t  (c) **주정차 폴리곤 접촉 구간 소급** — 아래 6번의 억제로 표출이 막힌 접촉 tick 을 버퍼에
 * \t      쌓아뒀다가, 이탈이 확정되는 순간 그 구간 전체를 새 run 의 진입점으로 되돌린다.
 *
 * \t**4. 누락 링크 복구와 FROM_ID/TO_ID** — 직전 확정 링크와 이번 링크가 떨어져 있으면(또는 바로
 * \t인접이어도) FindLinkPathBounded() 로 그 사이 링크를 복구하고, FindLinkPolygonCrossing() 으로
 * \t주정차 폴리곤이 갈리는 지점을 찾아 **그 지점까지의 거리만** 누적한 뒤 그 링크를 TO_ID 에 기재한다.
 * \t폴리곤 안쪽은 포함하지 않는다. END_GPS_SEQ/OCCUR_DT 는 교차점을 찾아낸 tick 이 아니라 **마지막
 * \t확정 링크 tick** 을 쓴다 — 복구 링크와 교차점은 지오메트리로 보완한 값일 뿐 실측 경계는 그 tick
 * \t이기 때문이다.
 * \t  **FROM_ID 는 "거리 누적이 실제로 시작된 링크"** 다(사용자 지시, 2026-09-06). 누적은 경로의 두
 * \t  번째 링크부터 시작하고 직전 확정 링크(경로 첫 링크) 위의 남은 거리는 세지 않으므로, 직전 확정
 * \t  링크를 FROM_ID 로 적으면 거리 기여가 0인 링크가 출발 링크로 남아 그 링크를 통째로 지난 것처럼
 * \t  오해된다. 실측 000376_20260819093337 seq106 — 매칭점이 2040424801(63.7m)의 끝 노드에서 0.46m
 * \t  지점이라 실제 6m 는 전부 다음 링크 2040424803(9.0m, 이 링크 중간에서 폴리곤이 갈림) 위였는데
 * \t  FROM_ID 가 2040424801 로 찍혔다. 올바른 표기는 **2040424803 → 2040424803** 이다.
 * \t  FROM_LAT/LON 은 실제 관측된 마지막 매칭 좌표를 그대로 둔다 — 링크 시작 노드로 바꾸면 실측이
 * \t  아닌 값이 되고, 둘의 차이는 애초에 세지 않는 잔여 구간(위 실측 0.46m)뿐이다.
 * \t  **주정차 폴리곤이 없는 일반 진출~진입 gap 복구**(FindLinkPathBounded 만 쓰는 경로)도 복구 중간
 * \t  링크마다 IsCase3EligibleRoadKind() 로 걸러, 다른 과금유형(개방식·폐쇄식·면제)에 등록된 링크를
 * \t  만나면 그 직전에서 멈춘다 — 일반도로(0)·구간단속(3) 등록 또는 미등록 링크만 통과시킨다. 케이스3
 * \t  (SKIP 구간 브릿지)에 이미 확정해 쓰던 것과 같은 기준이다(사용자 지시, 2026-09-14 최정우 수정 —
 * \t  실측 000370_20260911141637 trip_seq=5: 개방식 RL-Z00004 등록 링크 2040424103 이 검사 없이
 * \t  흡수돼 TO_ID/DIST_M 이 그 링크만큼 과다·오기재됨. 구간단속은 예외 — 위반 없이 통과한 구간단속
 * \t  구역은 자체적으로 일반도로 미러(3a)를 만들어 결과적으로 일반도로 과금과 동등하므로 함께 흡수해도
 * \t  이중계상이 아니다).
 *
 * \t**5. CHARGE_YN/STATUS** — 정상 마감은 Y/0, TTL 만료 등 확정 못한 채 끝난 건 N/3(AUDIT=심사대상).
 * \t비과금도로의 N/4(SKIP)와 다르다 — 애초에 과금 대상이 아닌 비과금도로와 달리 일반도로는 확정만
 * \t못한 것이라 사람이 재확인해야 한다. 값 자체는 호출측이 정해 인자로 넘긴다.
 *
 * \t**6. 주정차 폴리곤 안에서는 일반도로를 표출하지 않는다** — 구역 등록/미등록 여부와 무관하다.
 * \t[2026-10-04 변경, 사용자 확정] 주행 중 폴리곤을 **빠져나온** 경우 폴리곤 안 구간도 일반도로로
 * \t등록한다(정책 대조표 B2·E1, ProcessParkingCharge() 위). 아래 억제는 이제 "접촉 중에는 run 을
 * \t보류한다"는 뜻이고, 폴리곤 안에서 트립이 끝날 때(B4)만 안쪽 구간이 일반도로에서 빠진다.
 * \t**판정은 매칭좌표(bMatchInParkingZoneNow) 단독 기준**이다(사용자 지시, 2026-09-06):
 * \t  | 원시 | 매칭 | 판정 |
 * \t  | 안 | 안 | 주정차 — 억제 |
 * \t  | 밖 | 안 | **주정차로 인정** — 억제 (원시가 폴리곤을 살짝 벗어나도 실제 달린 도로가 안이면 주정차) |
 * \t  ※ [2026-09-16 사용자 재확인] **속도는 이 판정에 들어가지 않는다.** "폴리곤 안이라도 주행
 * \t    중이면 일반도로로 표출"하는 속도 예외를 넣었다가 되돌린 이력이 있다 — 사유는
 * \t    bInParkingZone 계산부 주석 참고(폴리곤에 진입해 주차하는 차도 진입 순간엔 주행 속도라
 * \t    통과와 구분 불가, 실측에서 Y/0 구간 중복 5쌍 발생). 매칭좌표가 폴리곤 안이면 속도와
 * \t    무관하게 주정차이며, 예외는 아래 "안/밖"(실제 달린 도로가 폴리곤 밖) 한 줄뿐이다.
 * \t  | 안 | 밖 | **일반도로로 포함** (실제 달린 도로가 폴리곤 밖이면 주정차가 아님) |
 * \t  | 밖 | 밖 | 일반도로 |
 * \t  즉 매칭이 안이면 억제, 밖이면 포함이며 원시좌표는 판정에서 빠진다. 이는 억제를 원시좌표
 * \t  기준으로 하던 2026-09-02 지시를 **대체**한다 — 다만 그 지시의 취지인 "park_pad 버퍼를 쓰지
 * \t  않고 순수 폴리곤 경계로만 판단"(버퍼 적용 시 14개 주정차 구역 인근 도로가 대거 영향받음)은
 * \t  그대로다. 버퍼는 여전히 0.0 이고 바뀐 것은 어느 좌표를 폴리곤에 넣어보는가뿐이다.
 * \t  매칭좌표를 쓰는 근거는 2026-09-03 에 "접촉 확정" 판정을 매칭 기준으로 바꾼 것과 같다 —
 * \t  원시로 판정하면 접근로만 스친 오검출이 섞인다(실측 000376_20260819094414 seq55·56).
 * \t  실측 000376_20260821094609: seq14·17·18(원시 밖·매칭 안)이 주정차로 인정돼 종전의 일반도로
 * \t  28m 레코드가 사라지고, seq10(원시 안·매칭 밖)은 일반도로로 포함된다.
 * \t  이탈은 즉시 판정하지 않고 park_exitcnt 만큼 디바운스한다 — 폴리곤 밖으로 짧게 튄 tick 을
 * \t  PARKING 은 노이즈로 흡수하는데 NODE_STEP 만 별도 레코드로 쪼개는 불일치를 막기 위함이다.
 * \t  접촉 구역 ID(szParkTouchZoneRoadId)도 매칭쪽을 1순위로 채운다 — 원시가 밖인 tick 은 원시쪽
 * \t  구역 ID 가 비어 있어 그대로 두면 인수인계 로직이 구역을 못 찾는다.
 *
 * \t**7. 주정차 폴리곤을 나가며 시작하는 run — 거리와 시간을 반드시 같은 기준점에서 잰다**
 * \t(2026-09-06 최정우 정리, 사용자 지시 — 실측 000376_20260819093337 seq52~62 로 전 구간 검증)
 * \t  진출은 진입(4번)과 대칭이다. 폴리곤 안 tick 은 6번으로 억제되므로, 나가는 순간부터 새 run 이
 * \t  열리는데 그 **시작점은 "이탈이 확정된 tick"이 아니라 "폴리곤 경계 교차점"**이다
 * \t  (`FindLinkPolygonExitCrossing`). 확정 tick 부터 시작하면 park_exitcnt 디바운스만큼의 구간이
 * \t  어느 레코드에도 안 들어가 통째로 누락된다.
 * \t    · FROM_ID / FROM_LAT/LON = **경계가 놓인 링크와 그 교차점**. 이 링크에는 GPS 점이 하나도
 * \t      없을 수 있다(짧은 링크를 한 tick 에 통과) — 매칭 좌표와 일치하지 않는 게 정상이다.
 * \t    · START_GPS_SEQ = **경계를 넘은 첫 실측 tick**. 거리 산출의 종점(이탈 확정 tick)과는 다를
 * \t      수 있고, 그 사이 tick 들은 거리에만 반영된다.
 * \t    · DIST_M = 경계~확정 tick(누락 링크 복구 포함) + 이후 주행분 + 진출 쪽 누락 링크 복구
 * \t    · STAY_SECONDS = **경계 교차 시각**(`InterpolateGateCrossingTime` 로 보간) ~ 진출 시각
 * \t  **거리만 경계부터 재고 시간은 첫 tick 부터 재면 평균속도가 부풀려진다.** 둘 다 경계 기준이어야
 * \t  한다 — 구간단속에서 실제로 그 불일치가 있었다(거리는 구역 전체, 시간은 실측분만 써서 34.5 대신
 * \t  39.7km/h 로 계산, 2026-09-05 수정). 일반도로는 이 원칙이 지켜지고 있음을 실측으로 확인했다:
 * \t    경계 093738 ~ seq62 093810 = 32초, 362m, 362/32*3.6 = 40.7 -> 41km/h (DB 일치)
 * \t  판정 기준(6번)을 바꿀 때는 **경계 스냅샷 기준도 같이 바꿔야 한다** — 2026-09-06 억제를
 * \t  원시->매칭 으로 바꾸면서 스냅샷만 원시로 남겨, seq51(원시 밖·매칭 안)이 억제 대상이면서
 * \t  동시에 "이탈 첫 tick"으로 기록돼 run 이 51 부터 열린 회귀가 있었다(올바른 값 52).
*/
void CRawLogWorker::BuildNodeStepRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, uint32 dwEndGpsSeq,
		const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow)
{
	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "0";							// NODE_STEP(일반도로)
	pstRow->strChargeUnit = "0";							// NODE(사용자 지시, 2026-08-14 — 개방형과 동일 관례)
	pstRow->strLinkId = "";

	// FROM_ID/TO_ID — road_kind=0 정식 구역이든 미등록 pseudo-zone(케이스2, szRoadID=="")이든
	//   과금유형 등록 여부와 무관하게 항상 LINK_ID(진입/이탈 링크) 사용(사용자 지시, 2026-09-01
	//   최정우 수정 — 기존엔 정식 구역만 road_id를 썼으나 일반도로는 등록 여부 상관없이 통일)
	{
		// [2026-09-23 최정우 수정 — 사용자 지적] 경계가 **타 과금유형 등록 링크**면 이 run 이 덮은
		//   링크 중 가장 바깥의 **미등록 링크**로 되돌린다.
		//   진입/이탈 tick 의 매칭 링크를 그대로 쓰면, 타 유형 구역에 막 들어간(혹은 막 나온) tick 이
		//   경계가 되어 그 유형 행과 구간이 겹친다 — 실측 000998_20260917090000 trip_seq3:
		//   seq8 이 면제구역 RL-Z00007 링크 2040005903 에 매칭되며 일반도로 run 이 닫혀 TO_ID 가
		//   그 링크가 됐다. 실제 일반도로 구간은 면제 진입 **직전** 링크 2040006402 까지다
		//   (경로: 2040424001 → 2040424002 → 2040006402 → 2040005903).
		//   vtRunLinks 는 tick 이 안 찍힌 경유 링크까지 담고 있고(2026-09-22 추가), 이 tick 의
		//   경로가 ProcessNodeStepCharge() 호출 **전에** 이미 쌓이므로 그 직전 링크를 여기서 찾을 수 있다.
		//   ※ START/END_GPS_SEQ 는 건드리지 않는다 — 넓히면 같은 유형 구간중복이 생긴다
		//     (FillUncoveredLinkRows 안전규칙 3번과 같은 이유). 경계는 "어디까지 달렸나"를 가리키는
		//     링크값이라 tick 범위와 독립적으로 고칠 수 있다.
		//   [2026-09-23 2차 수정] vtRunLinks 만 뒤지는 것으로는 부족하다 — **경로 재구성 자체가
		//   불완전**해서 tick 사이 링크가 통째로 빠져 있을 수 있다(실측 000998 seq7->8: vtRunLinks 에
		//   2040424002 는 있는데 그 다음 2040006402 가 없어, 트립 마감 때 FillUncoveredLinkRows 가
		//   "미덮임"으로 따로 주웠다). 그래서 run 이 들고 있는 마지막 미등록 링크에서 경계 링크까지
		//   **FindLinkPathBounded 로 직접 탐색**해 그 사이 링크까지 본다. 이 탐색은 TURN_INFO 기반
		//   방향성 그래프라 역방향 링크로는 뻗지 않는다(사용자 지적 "주행 방향 참조").
		// 경계 보정은 "구역 진입/진출 직전후" 몇 링크만 보면 되므로 홉수를 짧게 잡는다
		//   (SKIP 갭 복구 MM_SKIPGAP_MAX_HOPS 와 같은 6)
		static const int MM_EDGE_FIX_MAX_HOPS = 6;
		uint64 qwFromLink = stRun.qwEntryLinkID;
		uint64 qwToLink   = stRun.qwLastLinkID;

		if (m_stConfig.pcChargeDataLoader != nullptr)
		{
			// TO — run 안의 마지막 미등록 링크를 찾고, 거기서 경계 링크까지의 경로에서 더 바깥의
			//   미등록 링크가 있으면 그쪽을 쓴다
			if ((qwToLink != 0)
				&& !m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(qwToLink))
			{
				uint64 qwAnchor = 0;
				for (size_t i = stRun.vtRunLinks.size(); i-- > 0; )
				{
					if (m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(stRun.vtRunLinks[i]))
					{ qwAnchor = stRun.vtRunLinks[i]; break; }
				}
				uint64 qwPicked = qwAnchor;
				if (qwAnchor != 0)
				{
					vector<uint64> vtPath;
					if (FindLinkPathBounded(qwAnchor, qwToLink, MM_EDGE_FIX_MAX_HOPS, &vtPath))
					{
						for (size_t i = vtPath.size(); i-- > 0; )
						{
							if (m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtPath[i]))
							{ qwPicked = vtPath[i]; break; }
						}
					}
				}
				if (qwPicked != 0) qwToLink = qwPicked;
			}

			// FROM — 대칭. 경계 링크에서 run 안의 첫 미등록 링크까지의 경로에서 가장 앞선 미등록 링크
			if ((qwFromLink != 0)
				&& !m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(qwFromLink))
			{
				uint64 qwAnchor = 0;
				for (size_t i = 0; i < stRun.vtRunLinks.size(); ++i)
				{
					if (m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(stRun.vtRunLinks[i]))
					{ qwAnchor = stRun.vtRunLinks[i]; break; }
				}
				uint64 qwPicked = qwAnchor;
				if (qwAnchor != 0)
				{
					vector<uint64> vtPath;
					if (FindLinkPathBounded(qwFromLink, qwAnchor, MM_EDGE_FIX_MAX_HOPS, &vtPath))
					{
						for (size_t i = 0; i < vtPath.size(); ++i)
						{
							if (m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtPath[i]))
							{ qwPicked = vtPath[i]; break; }
						}
					}
				}
				if (qwPicked != 0) qwFromLink = qwPicked;
			}
		}

		char szFromId[24], szToId[24];
		snprintf(szFromId, sizeof(szFromId), "%llu", static_cast<unsigned long long>(qwFromLink));
		snprintf(szToId, sizeof(szToId), "%llu", static_cast<unsigned long long>(qwToLink));
		pstRow->strFromId = szFromId;
		pstRow->strToId = szToId;
	}

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", stRun.dfLastY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", stRun.dfLastX);
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	// zone_id/zone_name — 일반도로 run 이 여러 zone_id(미등록↔등록구역A↔등록구역B)를 넘나들며
	//   하나로 병합되므로, 마지막에 머물던 zone_id(stRun.szRoadID) 하나만 남기면 오히려
	//   오해를 준다. 과금형 테이블 등록 여부 자체를 무시하는 일반도로 특성상 zone 식별은
	//   FROM_ID/TO_ID(링크ID)로 충분해 아예 비워둔다(사용자 지시, 2026-09-01 최정우 추가)
	pstRow->strZoneId = "";
	pstRow->strZoneName = "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stRun.dfAccumDistM + 0.5));
	pstRow->strDistM = szDistM;

	double dfElapsedSec = difftime(dtEnd, stRun.dtEntryTime);
	if (dfElapsedSec > 0.0)
	{
		double dfAvgSpeedKmh = (stRun.dfAccumDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";							// 일반도로는 구역 제한속도 개념 해당없음

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", stRun.dwEntryGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwEndGpsSeq);
	pstRow->strStartGpsSeq = szStartGpsSeq;
	pstRow->strEndGpsSeq = szEndGpsSeq;

	// OCCUR_DT — 다른 4유형은 "진입 시각"이지만 일반도로만 사용자 지시로 "진출 시각"
	//   (2026-08-14) — 혼동해서 통일하지 말 것
	pstRow->strOccurDt = FormatDateTime14(dtEnd);

	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	if (pszTripStartDt != nullptr)
		pstRow->strTripStartDt = pszTripStartDt;
	else
		pstRow->strTripStartDt = pstRow->strOccurDt;

	pstRow->strTollgateId = "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strRegDt;		// UPD_DT=최종 수정 일시 — INSERT 시점엔 REG_DT 와 같다(2026-10-04 최정우 수정, 종전 OCCUR_DT)
	pstRow->strChargeYn = pszChargeYn;
	pstRow->strChargeStatus = pszChargeStatus;

	// [2026-09-22 최정우 추가] 이 행이 덮은 링크 목록을 run 에서 그대로 넘긴다 — 트립 마감 시
	//   "실제 경유한 링크" 와 대조해 **어느 행에도 안 덮인 링크**를 찾는 데 쓴다. DB 에는 안 실린다
	//   (ZONE_RUN_SESSION::vtRunLinks 주석 참고).
	pstRow->vtCoveredLinks = stRun.vtRunLinks;
}

/**
 * @brief TTL 만료 세션 중 아직 열려있는 일반도로(ROAD_KIND=0) 세션을 N/3(AUDIT) 로 마감 (2026-08-14 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 만료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 세션이 진행 중이었으면 CHARGE_INSERT_ROW 1건 추가
 * @remark 정상 이탈·트립종료는 ProcessNodeStepCharge() 가 이미 그 자리에서 Y/0 로 기록하므로,
 *   이 함수는 그 신호(링크 변경/트립종료)가 영영 안 오고 GPS 자체가 끊긴 경우(TTL 만료)만 대신
 *   마감해주는 보완 경로. 일반도로는 실제 과금 대상이라 폐쇄형/구간단속/주정차와 동일하게
 *   charge_status=3(AUDIT) — 비과금도로의 SKIP(4)과 다름(애초에 과금 대상이 아니었던 비과금도로와
 *   달리, 일반도로는 확정 못한 채 끝난 것뿐이라 "심사대상"이 맞음).
 *   레코드 조립 규칙 자체는 BuildNodeStepRow() 헤더 주석 참고 (2026-09-06 최정우 — 원래 이 블록이
 *   BuildNodeStepRow() 위에 잘못 붙어 있던 것을 제자리로 옮김)
 * @param[in] bNoTripEnd true=종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
 *   non_charge_reason 을 62(면제도로는 52)로 남긴다. false=TTL·잔여tick·트립종료(61/51).
 *   판정(charge_yn/charge_status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
*/
void CRawLogWorker::AppendExpiredNodeStepCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEnd, uint32 dwEndGpsSeq,
		bool bStationaryTripEnd, vector<CHARGE_INSERT_ROW> *pvtOut, bool bNoTripEnd)
{
	// 마감 사유 코드 — 종료신호 없이 다음 운행이 시작된 경로면 62, 그 밖의 마감
	//   (TTL·잔여tick·트립종료)이면 61. 판정(charge_yn/status)은 동일하다
	//   (2026-09-16 최정우 추가)
	const int nReasonCode = bNoTripEnd ? NCR_NO_TRIP_END_FORCED_CLOSE : NCR_TTL_FORCED_CLOSE;

	if (stSession.vtNodeStepRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 겹쳐 진행 중이던 구역이 여럿일 수 있어 전부 마감한다 (2026-08-23 최정우 수정)
	// dtEnd/dwEndGpsSeq 파라미터화(2026-09-03 최정우 수정, ClosedRoad/SpeedZone과 동일 패턴) — TTL
	//   만료 전용이던 걸 트립종료 안전망(FlushNodeStepRunsAtTripEnd)에서도 재사용하기 위함. TO_LAT/
	//   LON·DIST_M 은 여전히 stRun.dfLastX/Y·dfAccumDistM(run이 매 틱 정확히 누적한 값)을 그대로
	//   써서 dtEnd 시점의 매칭 신뢰 여부와 무관하게 정확하다.
	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtNodeStepRuns.size(); ++i)
	{
		CHARGE_INSERT_ROW stRow;
		// OCCUR_DT/END_GPS_SEQ 는 dtEnd/dwEndGpsSeq(호출측이 넘긴 "이번 tick", TRIP_EVENT=2가
		//   왔지만 매칭 실패한 tick이거나 TTL 스윕 시각일 수 있음)가 아니라, run 이 실제로 마지막
		//   확정매칭됐던 시각(dtLastInZoneTime/dwLastInZoneGpsSeq)을 우선 써야 한다 — 안 그러면
		//   "마지막 확정 위치는 seq102인데 도착시각·stay_seconds는 신호 끊긴 seq129 기준"으로
		//   나와, 실제로 확인 안 된 공백 시간(예: seq103~129, 여기선 2분11초)이 그대로 stay_seconds에
		//   얹혀 평균속도가 실제보다 낮게 계산된다(사용자 지적, 2026-09-14 최정우 수정 — 실측
		//   000376_20260819141002: TO_LAT/LON은 seq102 위치가 정확히 나오는데 OCCUR_DT/END_GPS_SEQ만
		//   seq129 기준이라 stay_seconds가 186초가 아니라 316초로 부풀려짐). run 이 한 번도 확정
		//   매칭을 못 받은 예외적인 경우(dtLastInZoneTime==0)만 넘겨받은 값으로 방어적 폴백한다.
		const ZONE_RUN_SESSION& stExpiredRun = stSession.vtNodeStepRuns[i];
		const time_t dtRowEnd = (stExpiredRun.dtLastInZoneTime != 0) ? stExpiredRun.dtLastInZoneTime : dtEnd;
		const uint32 dwRowEndGpsSeq =
			(stExpiredRun.dtLastInZoneTime != 0) ? stExpiredRun.dwLastInZoneGpsSeq : dwEndGpsSeq;
		// N/3(AUDIT) — 다른 등록 과금형 도로(면제 제외)의 TTL·비정상종료 관례와 동일
		//   (사용자 지시, 2026-09-01 최정우 수정 — 기존엔 정상 이탈과 동일하게 Y/0로 나가던 버그)
		//   단, bStationaryTripEnd(신뢰 가능한 트립종료 + 그 시점 정차·주차/일시정지 상태)면 "확정
		//   못 한 채 끝남"이 아니라 "정차한 채로 정상 종료"이므로 Y/0 정상마감 (사용자 지시, 2026-09-14
		//   최정우 추가)
		const char *pszChargeYn = bStationaryTripEnd ? "Y" : "N";
		const char *pszChargeStatus = bStationaryTripEnd ? "0" : "3";
		BuildNodeStepRow(stExpiredRun, stSession.szTripId, strDeviceKey,
			nSeq, dtRowEnd, dwRowEndGpsSeq, pszChargeYn, pszChargeStatus, &stRow);
		if (!bStationaryTripEnd)
		{
			// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(N/3)은 안 건드림
			char szReason[8];
			snprintf(szReason, sizeof(szReason), "%d", nReasonCode);
			stRow.strNonChargeReason = szReason;
		}
		pvtOut->push_back(stRow);
		nSeq += 1;

		if (bStationaryTripEnd)
		{
			// [버그 수정, 2026-09-15 최정우] Y/0 도 N/x 와 동일 형식으로 non_charge_reason=[0:정상 과금]
			//   을 남긴다(2026-09-11 관례) — 이 경로만 필드가 통째로 빠져 있었다(사용자 지적)
			LOGFMTI("[#%02d] node step closed normally(stationary trip end)!device=[%s] trip_id=[%s] "
				"seq=[%d] road=[%s] dist_m=[%s] non_charge_reason=[%d:%s]",
				nThreadId, strDeviceKey.c_str(), stSession.szTripId, nSeq - 1,
				stSession.vtNodeStepRuns[i].szRoadID, stRow.strDistM.c_str(), NCR_NORMAL,
				m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
		}
		else
		{
			LOGFMTW("[#%02d] node step expired!device=[%s] trip_id=[%s] seq=[%d] road=[%s] dist_m=[%s] "
				"non_charge_reason=[%d:%s]",
				nThreadId, strDeviceKey.c_str(), stSession.szTripId, nSeq - 1,
				stSession.vtNodeStepRuns[i].szRoadID, stRow.strDistM.c_str(), nReasonCode,
				m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nReasonCode));
		}
	}
}

/**
 * @brief 트립종료 시 "이번 틱이 신뢰 못할 매칭이거나 매칭 자체를 못 한" 조기 반환 경로 전용 —
 *   AppendExpiredNodeStepCharge() 를 이번 틱의 실제 GPS 시각/순번으로 재사용하는 얇은 래퍼
 * @remark 트립이 정상 확정 경로(bMatched && !bUntrustedMatch)로 끝나는 경우는 CommitPendingRow() 가
 *   이미 그 자리에서 ProcessNodeStepCharge() 로 Y/0 정상 마감하므로 이 함수를 호출하면 안 된다 —
 *   호출측에서 그 경로에는 넣지 않았다(2026-09-03). 호출 전후로 vtNodeStepRuns 크기 변화만큼만
 *   nChargeSeq 를 늘려 PK(trip_id,device_key,trip_seq) 충돌을 피한다(SpeedZone과 동일 패턴).
*/
void CRawLogWorker::FlushNodeStepRunsAtTripEnd(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if (pstSession == nullptr)
		return;

	// 구간단속 마감 시 보류해둔 일반도로 미러가 이 조기반환 경로로 트립이 끝날 때까지도 인수인계
	//   구간을 못 만나 소비 안 된 채 남아있으면, 원래 값 그대로 지금 등록한다(2026-09-03 최정우 추가)
	if (pstSession->bHasHeldSpeedMirrorRun)
	{
		CHARGE_INSERT_ROW stMirrorRow;
		BuildNodeStepRow(pstSession->stHeldSpeedMirrorRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, pstSession->stHeldSpeedMirrorRun.dtLastInZoneTime,
			pstSession->stHeldSpeedMirrorRun.dwLastInZoneGpsSeq, "Y", "0", &stMirrorRow);
		pvtChargeInserts->push_back(stMirrorRow);
		pstSession->nChargeSeq += 1;
		pstSession->bHasHeldSpeedMirrorRun = false;
	}

	// 주정차 접촉이 확정 판정도 못 받고(이탈이 안 옴) 트립이 이 조기반환 경로로 끝나는 경우도
	//   ProcessNodeStepCharge() 트립종료 분기와 동일 기준으로 판정한다 — 확정 접촉이면 보류된
	//   run을 그 경계 그대로 정상(Y/0) 등록하고 접촉 구간은 버리며, 미확정이면 보류된 run과
	//   접촉 구간을 합쳐 마지막 확인 위치·시각 기준으로 정상(Y/0) 등록한다. held run이 없는데
	//   접촉만 있던 경우(대기 중이던 run 자체가 없음)는 없던 일로 버린다(사용자 지시, 2026-09-03
	//   최정우 추가 — ProcessNodeStepCharge를 거치지 않는 5개 조기반환 경로도 동일 유실 방지)
	if (pstSession->bHasParkTouchCarry)
	{
		if (pstSession->bParkTouchEverMatchedInside)
		{
			// 접촉 직전까지 실제 이동거리가 0이면 등록할 내용 자체가 없다 — 빈 레코드를 남기지
			//   않는다(사용자 지시, 2026-09-03 최정우 추가)
			if (pstSession->bHasHeldNodeStepRun && (pstSession->stHeldNodeStepRun.dfAccumDistM > 0.0))
			{
				CHARGE_INSERT_ROW stHeldRow;
				BuildNodeStepRow(pstSession->stHeldNodeStepRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
					pstSession->nChargeSeq, pstSession->stHeldNodeStepRun.dtLastInZoneTime,
					pstSession->stHeldNodeStepRun.dwLastInZoneGpsSeq, "Y", "0", &stHeldRow);
				pvtChargeInserts->push_back(stHeldRow);
				pstSession->nChargeSeq += 1;
			}
		}
		else if (pstSession->bHasHeldNodeStepRun)
		{
			ZONE_RUN_SESSION stFinal = pstSession->stHeldNodeStepRun;
			stFinal.dfAccumDistM += pstSession->stParkTouchCarry.dfAccumDistM;
			stFinal.dtLastInZoneTime = pstSession->stParkTouchCarry.dtLastInZoneTime;
			stFinal.dwLastInZoneGpsSeq = pstSession->stParkTouchCarry.dwLastInZoneGpsSeq;

			CHARGE_INSERT_ROW stFinalRow;
			BuildNodeStepRow(stFinal, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
				pstSession->nChargeSeq, stFinal.dtLastInZoneTime, stFinal.dwLastInZoneGpsSeq,
				"Y", "0", &stFinalRow);
			pvtChargeInserts->push_back(stFinalRow);
			pstSession->nChargeSeq += 1;
		}

		pstSession->bHasParkTouchCarry = false;
		pstSession->bHasHeldNodeStepRun = false;
		pstSession->bParkTouchEverMatchedInside = false;
	}
	else if (pstSession->bHasHeldNodeStepRun)
	{
		// 접촉 자체는 이미 끝났는데 그 안에서 조기마감된 run만 아직 대기 중인 상태는 정상적으론
		//   있을 수 없다(대기 중이면 항상 bHasParkTouchCarry=true) — 방어적으로 나머지 run들과
		//   동일하게(TTL과 동일 근거로 N/3) 마감한다(2026-09-03 최정우 추가)
		pstSession->vtNodeStepRuns.push_back(pstSession->stHeldNodeStepRun);
		pstSession->bHasHeldNodeStepRun = false;
	}

	// [버그 수정, 2026-09-11 최정우] 일반도로 구간병합 이월값(stMergeCarry) — 받아줄 새 run이
	//   열리기를 기다리는 중에 이 조기반환 경로로 트립이 끝나면, 다른 carry 상태들(bHasParkTouchCarry/
	//   bHasHeldNodeStepRun/bHasHeldSpeedMirrorRun)과 달리 여기서 소비되지 않아 그 구간의 거리·시간이
	//   과금 레코드 없이 사라지고 있었다(전체 재검증으로 발견, ExpireTtlSessions() 도 동일하게
	//   누락돼 있어 같이 고침) — 남은 값을 그대로 vtNodeStepRuns 에 편입시켜 마감한다.
	if (pstSession->bHasMergeCarry)
	{
		pstSession->vtNodeStepRuns.push_back(pstSession->stMergeCarry);
		pstSession->bHasMergeCarry = false;
	}

	if (pstSession->vtNodeStepRuns.empty())
		return;

	// 신뢰 가능한 트립종료(bTrustedTripEnd, 호출측에서 이미 보장됨) 시점의 DRIVE_STATUS 가
	//   정차·주차/일시정지면 "확정 못 한 채 끝남"이 아니라 "정차한 채로 정상 종료"로 본다
	//   (사용자 지시, 2026-09-14 최정우 추가)
	const bool bStationaryTripEnd = (stRawLogInfo.nDriveStatus == DRIVE_STATUS_PARKED)
		|| (stRawLogInfo.nDriveStatus == DRIVE_STATUS_IDLE);
	size_t nSizeBefore = pvtChargeInserts->size();
	AppendExpiredNodeStepCharge(nThreadId, stRawLogInfo.szDeviceKey, *pstSession,
		stRawLogInfo.dtGPS, stRawLogInfo.dwSeqNo, bStationaryTripEnd, pvtChargeInserts);
	pstSession->nChargeSeq += static_cast<int>(pvtChargeInserts->size() - nSizeBefore);
	pstSession->vtNodeStepRuns.clear();
}

/**
 * @brief 개방형·면제구역판 FlushNodeStepRunsAtTripEnd() — 이번 tick 자체의 맵매칭 실패로 bMatched
 *   블록(ProcessOpenGateCharge/ProcessExemptZoneCharge)이 안 불리는 조기반환 경로 전용 안전망
 * @remark [버그 수정, 2026-09-11 최정우] 트립 종료 시 마지막 GPS tick 자체의 맵매칭이 실패하면
 *   ProcessOpenGateCharge()/ProcessExemptZoneCharge() 가 bMatched 블록 안에서만 호출되는 탓에 전혀
 *   안 불려, 그 시점에 열려있던 개방형·면제구역 run 이 과금 레코드 없이 세션과 함께 사라지던 버그의
 *   수정. 반드시 FlushNodeStepRunsAtTripEnd() 와 정확히 같은 호출 지점(이번 tick이 매칭실패로
 *   곧장 return 하는 분기)에서만 호출할 것 — 처음엔 CLOSED/SPEED 의 "매칭 시도 전 무조건 flush"
 *   패턴을 그대로 따라 ProcessRawLog 의 bTrustedTripEnd 블록 최상단에서 무조건 호출하도록 짰었는데,
 *   재매칭 결과를 수정 전과 대조하다가 000376_20260821095239·000376_20260826152113 두 트립에서
 *   회귀를 발견했다 — 이 두 트립은 원래 마지막 tick이 정상 매칭돼 bMatched 블록에서 Y/0 으로 정상
 *   종료되고 있었는데, 무조건-먼저-flush 가 vtOpenRuns 를 먼저 비워버려서 뒤이은 정상 Y/0 종료
 *   로직이 아예 실행되지 못하고 이 함수의 N/3(AUDIT) 값으로 잘못 대체됐다. CLOSED/SPEED 는 정상
 *   진출이 게이트 통과 tick(트립 마지막 tick보다 보통 이전)에서 이미 끝나 있어 무조건-먼저-flush 가
 *   안전하지만, OPEN/EXEMPT 는 "트립 마지막 tick 자체가 정상 매칭되며 끝나는" 케이스가 흔해 그
 *   전제가 성립하지 않는다 — FlushNodeStepRunsAtTripEnd() 의 2026-09-03 회귀 사례(선언부 주석
 *   참고)와 근본원인이 동일하다.
*/
void CRawLogWorker::FlushOpenExemptRunsAtTripEnd(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if (pstSession == nullptr)
		return;

	if (!pstSession->vtOpenRuns.empty())
	{
		size_t nSizeBefore = pvtChargeInserts->size();
		AppendTripEndOpenGateCharge(nThreadId, stRawLogInfo.szDeviceKey, *pstSession, pvtChargeInserts);
		pstSession->nChargeSeq += static_cast<int>(pvtChargeInserts->size() - nSizeBefore);
		pstSession->vtOpenRuns.clear();
	}
	if (!pstSession->vtExemptRuns.empty())
	{
		size_t nSizeBefore = pvtChargeInserts->size();
		AppendExpiredExemptZoneCharge(nThreadId, stRawLogInfo.szDeviceKey, *pstSession, pvtChargeInserts);
		pstSession->nChargeSeq += static_cast<int>(pvtChargeInserts->size() - nSizeBefore);
		pstSession->vtExemptRuns.clear();
	}
}

/**
 * @brief NODE_STEP 일반도로 등록 확장(2026-09-01 최정우 추가) — LINK_ID 를 FROM_ID~TO_ID 로 삼는
 *   공용 row 생성기
 * @remark BuildNodeStepRow() 는 road_kind=0 정식 구역(ZONE_RUN_SESSION, road_id 기반) 전용이라
 *   그대로 못 씀 — 구간단속 위반 추가분(케이스1)·SKIP 구간 브릿지(케이스3)는 실제 zone이 없거나
 *   (SKIP은 아예 zone 개념 자체가 없음) from_id/to_id 가 road_id 가 아니라 링크ID라 별도 함수로
 *   분리. charge_type/charge_unit 은 기존 NODE_STEP 컨벤션(0/0) 그대로 유지해 하위호환.
 *   pszZoneId/pszZoneName 이 nullptr 이면 zone_id/zone_name 빈값(케이스3 — 실제 zone 없음),
 *   있으면 그대로 채움(케이스1 — 구간단속 구역 road_id/road_nm 재사용)
*/
void CRawLogWorker::BuildNodeStepRowFromLinkRange(const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, uint64 qwFromLink, uint64 qwToLink,
		double dfFromLat, double dfFromLon, double dfToLat, double dfToLon, double dfDistM,
		time_t dtStart, time_t dtEnd, uint32 dwStartGpsSeq, uint32 dwEndGpsSeq,
		const char *pszChargeYn, const char *pszChargeStatus,
		const char *pszZoneId, const char *pszZoneName, CHARGE_INSERT_ROW *pstRow)
{
	CHARGE_INSERT_ROW& stRow = *pstRow;
	stRow.strTripId = strTripId;
	stRow.strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "0";									// NODE_STEP(일반도로) — 기존 컨벤션 유지
	stRow.strChargeUnit = "0";									// NODE(개방형·기존 NODE_STEP과 동일 관례)
	stRow.strLinkId = "";

	char szFromId[24], szToId[24];
	snprintf(szFromId, sizeof(szFromId), "%llu", static_cast<unsigned long long>(qwFromLink));
	snprintf(szToId, sizeof(szToId), "%llu", static_cast<unsigned long long>(qwToLink));
	stRow.strFromId = szFromId;
	stRow.strToId = szToId;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", dfFromLat);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", dfFromLon);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", dfToLat);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", dfToLon);
	stRow.strFromLat = szFromLat;
	stRow.strFromLon = szFromLon;
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = (pszZoneId != nullptr) ? pszZoneId : "";
	stRow.strZoneName = (pszZoneName != nullptr) ? pszZoneName : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfDistM + 0.5));
	stRow.strDistM = szDistM;

	double dfElapsedSec = difftime(dtEnd, dtStart);
	if (dfElapsedSec < 1.0) dfElapsedSec = 1.0;					// 최소 1초 보정 — 구간단속과 동일 근거
	{
		double dfAvgSpeedKmh = (dfDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}
	stRow.strSpeedLimitKmh = "";								// 일반도로는 구역 제한속도 개념 해당없음(BuildNodeStepRow와 동일)

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", dwStartGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwEndGpsSeq);
	stRow.strStartGpsSeq = szStartGpsSeq;
	stRow.strEndGpsSeq = szEndGpsSeq;

	// OCCUR_DT — 기존 NODE_STEP 관례(다른 4유형과 달리 "진출 시각") 그대로 유지
	stRow.strOccurDt = FormatDateTime14(dtEnd);

	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = "";
	stRow.strExitTollgateId = "";
	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;		// UPD_DT=최종 수정 일시(2026-10-04 최정우 수정, 종전 OCCUR_DT)
	stRow.strChargeYn = pszChargeYn;
	stRow.strChargeStatus = pszChargeStatus;

	// [2026-09-22 최정우 추가] 이 행이 덮은 링크를 기록한다 — 이 함수는 run 이 아니라 **링크 범위**로
	//   행을 만드는 경로(구간단속 미러, SKIP-gap 브리지)라 vtRunLinks 를 못 받는다. 그래서 FROM~TO
	//   사이 경로를 직접 채운다. 기록이 없으면 커버리지 대조에서 **덮었는데 미덮임으로 잘못 집계**된다
	//   (실측 000994_20250903152350: 구간단속 RL-Z00006 미러가 덮은 554m 가 오탐으로 잡혔다).
	pstRow->vtCoveredLinks.clear();
	if ((qwFromLink != 0) && (qwToLink != 0))
	{
		if (qwFromLink == qwToLink)
		{
			pstRow->vtCoveredLinks.push_back(qwFromLink);
		}
		else
		{
			static const int MM_COVER_RANGE_MAX_HOPS = 8;
			vector<uint64> vtRangePath;
			if (FindLinkPathBounded(qwFromLink, qwToLink, MM_COVER_RANGE_MAX_HOPS, &vtRangePath))
				pstRow->vtCoveredLinks = vtRangePath;
			else
			{									// 경로를 못 찾으면 양 끝만이라도 기록
				pstRow->vtCoveredLinks.push_back(qwFromLink);
				pstRow->vtCoveredLinks.push_back(qwToLink);
			}
		}
	}
	else if (qwFromLink != 0)
		pstRow->vtCoveredLinks.push_back(qwFromLink);
	else if (qwToLink != 0)
		pstRow->vtCoveredLinks.push_back(qwToLink);
}

/**
 * @brief 개방형 과금 1행 생성 — 정상 이탈과 TTL 만료 공용 (2026-08-25 최정우 추가)
 * @remark dist_m·from/to 좌표·charge_yn/status 산출 방식이 stRun.bStartedByTrip 으로 갈린다:
 *   - false(구역 밖에서 정상 진입) — dist_m=구역 전체길이(ZONE_INFO.dfLengthM), from/to 좌표=
 *     구역 자체 등록 시작/끝점(dfFirstLat/Lon·dfLastLat/Lon, SPEED/CLOSED와 동일 관례),
 *     charge_yn/status=Y/0 고정(개방형 구역을 물리적으로 통과하려면 게이트를 지날 수밖에 없음)
 *   - true(트립이 이 구역 도로 위에서 시작) — dist_m=stRun.dfAccumDistM(출발~이탈 실관측 거리),
 *     from/to 좌표=실제 진입/이탈 매칭 좌표(NODE_STEP과 동일 관례), charge_yn/status 는
 *     stRun.bGateCrossed(이 run 동안 게이트를 실제로 지났는가)로 결정 — Y/0(지남) vs N/3(못 지남,
 *     이미 게이트를 지난 뒤 시작했다는 뜻)
*/
void CRawLogWorker::BuildOpenZoneRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, uint32 dwEndGpsSeq, CHARGE_INSERT_ROW *pstRow)
{
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);

	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "1";							// OPEN_ROAD
	pstRow->strChargeUnit = "0";							// NODE(실측 관례 유지)
	pstRow->strLinkId = "";
	pstRow->strFromId = stRun.szRoadID;					// 2026-08-25 변경 — 게이트ID→구역road_id(NODE_STEP과 동일 관례)
	pstRow->strToId = stRun.szRoadID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	double dfDistM;
	if (stRun.bStartedByTrip)
	{
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
		snprintf(szToLat, sizeof(szToLat), "%.06lf", stRun.dfLastY);
		snprintf(szToLon, sizeof(szToLon), "%.06lf", stRun.dfLastX);
		dfDistM = stRun.dfAccumDistM;
	}
	else if (pstZone != nullptr)
	{
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZone->dfFirstLat);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZone->dfFirstLon);
		snprintf(szToLat, sizeof(szToLat), "%.06lf", pstZone->dfLastLat);
		snprintf(szToLon, sizeof(szToLon), "%.06lf", pstZone->dfLastLon);
		dfDistM = pstZone->dfLengthM;
	}
	else
	{
		szFromLat[0] = szFromLon[0] = szToLat[0] = szToLon[0] = '\0';
		dfDistM = 0.0;
	}
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	pstRow->strZoneId = stRun.szRoadID;
	pstRow->strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfDistM + 0.5));
	pstRow->strDistM = szDistM;

	double dfElapsedSec = difftime(dtEnd, stRun.dtEntryTime);

	// [버그 수정, 2026-09-21 최정우, 사용자 확정 — 이슈 33①] 구역 안 실측 tick 이 **하나뿐**일 때의
	//   체류시간 보정. 그 경우 진입·진출 보간이 통행 전체를 감쌀 수 없어, DIST_M 은 구역 등록길이
	//   (전 구간)인데 STAY_SECONDS 는 보간된 두 경계 사이(수 초)만 남는다 — 실측
	//   000994_20250903152350 RL-Z00004(141.6m): seq87 한 tick 만 구역 안에 걸려
	//   **142m/1초/512km/h**(그 tick 의 보고 속도는 50km/h). 구역 길이가 GPS 수신 간격 동안의
	//   이동거리보다 짧으면 구조적으로 발생한다(50km/h x 5초 = 약 70m < 141.6m).
	//   보정 근거는 물리 한계 하나다 — **구간 평균속도는 그 구간에서 관측된 최대 순간속도를 넘을
	//   수 없다.** 넘으면 경과시간이 부족한 것이므로 "거리 / 관측최대속도" 로 올린다.
	//   **tick 이 2개 이상이면 건드리지 않는다** — 그때는 보간이 실제 구간을 감싸고 있고, GPS 속도
	//   표본 사이의 진짜 최고속도가 표본 최대를 넘을 수 있어 이 한계가 성립하지 않는다.
	//   실측 분포상 tick 2개 이상인 행은 평균/관측최대 비가 최대 1.43 배로 측정 오차 범위이고,
	//   tick 1개인 행은 10.2 배라 둘이 뚜렷이 갈린다.
	// 되돌리는 법: 아래 if 블록만 지우면 종전 동작이다.
	if ((stRun.nInZoneTicks <= 1) && (stRun.fMaxSpeed > 0.0f) && (dfDistM > 0.0))
	{
		const double dfNeedSec = dfDistM / (static_cast<double>(stRun.fMaxSpeed) / 3.6);
		if (dfElapsedSec < dfNeedSec)
			dfElapsedSec = dfNeedSec;
	}

	if (dfElapsedSec > 0.0)
	{
		double dfAvgSpeedKmh = (dfDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";							// 개방형은 구역 제한속도 개념 해당없음(기존 관례 유지)

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", stRun.dwEntryGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwEndGpsSeq);
	pstRow->strStartGpsSeq = szStartGpsSeq;
	pstRow->strEndGpsSeq = szEndGpsSeq;

	pstRow->strOccurDt = FormatDateTime14(stRun.dtEntryTime);	// 진입 시각(다른 유형과 동일 관례)

	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	pstRow->strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : pstRow->strOccurDt;

	// tollgate_id — 이 run 동안 실제로 지난 게이트 ID(못 지났으면 빈 값)
	PGATE_INFO pstGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(stRun.szRoadID, 'M');
	pstRow->strTollgateId = (stRun.bGateCrossed && (pstGate != nullptr)) ? pstGate->szTollgateID : "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strRegDt;

	// charge_yn/status — 정상진입 run 은 Y/0 고정, 트립시작 run 은 게이트 통과 여부로 결정
	//   (사용자 지시, 2026-08-25)
	if (!stRun.bStartedByTrip || stRun.bGateCrossed)
	{
		pstRow->strChargeYn = "Y";
		pstRow->strChargeStatus = "0";
	}
	else
	{
		pstRow->strChargeYn = "N";
		pstRow->strChargeStatus = "3";
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(N/3) 자체는
		//   위에서 이미 끝났고 사유만 부가. AppendExpiredOpenGateCharge/AppendTripEndOpenGateCharge
		//   는 이 함수 호출 뒤 charge_yn/status 를 TTL/트립종료 사유(코드 61)로 다시 덮어쓰므로
		//   그쪽에서 이 값도 같이 덮어쓴다 — 여기서 세팅되는 11번은 그 두 곳을 거치지 않는
		//   "정상 진행 중 게이트 미통과로 마감되는" 살아있는 경로(ProcessOpenGateCharge)에만 남는다.
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", NCR_OPEN_ENTRY_GATE_MISSED);
		pstRow->strNonChargeReason = szReason;
	}
}

/**
 * @brief TTL 만료로 세션이 지워지기 직전, 아직 열려있는 개방형 run 이면 N/3(AUDIT)로 1건 기록 —
 *   [2026-10-04 정정] 판정은 이 함수가 직접 N/3 으로 넣는다. [trip_abend] 는 삭제됐고 TTL 마감은 [trip_end]
 *   (TRIP_END_DT·UPD_DT 만 기록)로 통합됐다. 아래 "사후 정정" 서술은 그 이전 상태다.
 *   (종전) [trip_abend] UPDATE(query.sql)가 뒤이어 TRIP_END_DT IS NULL 인 이 행을 찾아 다시 N/3로 정정
 *   하므로(2단계 처리, AppendExpiredNodeStepCharge와 동일 패턴) 이 시점에 N/3를 안 넣어도 최종
 *   DB 값은 같았지만, "면제도로인 경우를 제외한 과금형 도로는 TTL·비정상종료 시 N/3" 정책(사용자
 *   확정, 2026-09-01)을 게이트 통과 여부(bGateCrossed)와 무관하게 이 함수 자체에서도 명시적으로
 *   강제 — BuildOpenZoneRow() 는 게이트 확실히 통과한 정상 진행 run 도 Y/0 로 계산할 수 있는데,
 *   TTL로 끊긴 이상 트립 전체 데이터가 불완전할 수 있어 심사 큐로 보낸다 (2026-09-01 최정우 수정)
 * @param[in] bNoTripEnd true=종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
 *   non_charge_reason 을 62(면제도로는 52)로 남긴다. false=TTL·잔여tick·트립종료(61/51).
 *   판정(charge_yn/charge_status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
*/
void CRawLogWorker::AppendExpiredOpenGateCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut, bool bNoTripEnd)
{
	// 마감 사유 코드 — 종료신호 없이 다음 운행이 시작된 경로면 62, 그 밖의 마감
	//   (TTL·잔여tick·트립종료)이면 61. 판정(charge_yn/status)은 동일하다
	//   (2026-09-16 최정우 추가)
	const int nReasonCode = bNoTripEnd ? NCR_NO_TRIP_END_FORCED_CLOSE : NCR_TTL_FORCED_CLOSE;

	if (stSession.vtOpenRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtOpenRuns.size(); ++i)
	{
		// OCCUR_DT/END_GPS_SEQ — 세션의 마지막 수신 tick(dtLastSeen/dwLastGpsSeq, 매칭 여부 무관)이
		//   아니라 run 이 실제로 마지막 확정매칭됐던 시각(dtLastInZoneTime/dwLastInZoneGpsSeq)을
		//   우선 쓴다 — NODE_STEP 의 AppendExpiredNodeStepCharge() 동일 수정과 같은 근거(사용자
		//   지적, 2026-09-14 최정우 수정). run 이 한 번도 확정매칭을 못 받은 예외적인 경우만
		//   세션값으로 방어적 폴백.
		const ZONE_RUN_SESSION& stOrigRun = stSession.vtOpenRuns[i];
		const time_t dtRowEnd = (stOrigRun.dtLastInZoneTime != 0) ? stOrigRun.dtLastInZoneTime : stSession.dtLastSeen;
		const uint32 dwRowEndGpsSeq =
			(stOrigRun.dtLastInZoneTime != 0) ? stOrigRun.dwLastInZoneGpsSeq : stSession.dwLastGpsSeq;

		// DIST_M — "정상진입"(bStartedByTrip=false) run 은 BuildOpenZoneRow() 가 실측과 무관하게
		//   구역 등록 전체길이(pstZone->dfLengthM)를 그대로 쓴다(정상 진출 시엔 구역을 다 지났다는
		//   뜻이라 맞지만), TTL 강제종료는 구역을 다 지났는지조차 확실치 않다 — 신호가 구역 중간에서
		//   끊겼을 수도 있는데 전체길이를 청구하면 실측보다 부풀려질 위험이 있다. run 을 복사해
		//   bStartedByTrip=true 로 강제해, 어떤 진입 방식이었든 TTL 로 닫힐 땐 항상 실측 누적거리
		//   (dfAccumDistM)·실측 좌표(dfEntryX/Y, dfLastX/Y)를 쓰게 한다(사용자 지적, 2026-09-14
		//   최정우 수정 — dfAccumDistM 자체는 정상진입 run 도 매 틱 정확히 누적해두고 안 쓸 뿐이라
		//   그대로 재사용 가능).
		ZONE_RUN_SESSION stTtlRun = stOrigRun;
		stTtlRun.bStartedByTrip = true;

		CHARGE_INSERT_ROW stRow;
		BuildOpenZoneRow(stTtlRun, stSession.szTripId, strDeviceKey,
			nSeq, dtRowEnd, dwRowEndGpsSeq, &stRow);
		stRow.strChargeYn = "N";
		stRow.strChargeStatus = "3";
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록(공통 TTL사유) — 위
		//   BuildOpenZoneRow() 가 내부적으로 11번(진입게이트 미통과)을 세팅했을 수 있으나, 이 함수는
		//   그 사유와 무관하게 TTL 만료로 강제마감하는 경로라 61로 덮어쓴다(charge_yn/status 를
		//   덮어쓰는 것과 동일한 이유)
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", nReasonCode);
		stRow.strNonChargeReason = szReason;
		pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTW("[#%02d] open zone expired!device=[%s] trip_id=[%s] seq=[%d] road=[%s] dist_m=[%s] "
			"non_charge_reason=[%d:%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId, nSeq - 1,
			stSession.vtOpenRuns[i].szRoadID, stRow.strDistM.c_str(), nReasonCode,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nReasonCode));
	}
}

/**
 * @brief 트립 정상종료(TRIP_EVENT=2) 시점, 아직 열려있는 개방형 run 을 N/3(AUDIT)로 마감
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 종료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 열려있던 run 마다 CHARGE_INSERT_ROW 1건 추가
 * @remark [버그 수정, 2026-09-11 최정우] CLOSED/SPEED 는 트립종료 시 AppendExpiredClosedRoadCharge/
 *   AppendExpiredSpeedZoneCharge 가 매칭 성공 여부와 무관하게 무조건 호출돼 마감되는데(ProcessRawLog
 *   bTrustedTripEnd 블록), OPEN·EXEMPT 는 ProcessOpenGateCharge/ProcessExemptZoneCharge 가 그 자리에서
 *   Y/0 으로 마감하는 경로 하나뿐이었다 — 그런데 이 두 함수는 bMatched 블록 안에서만 호출된다. 트립이
 *   끝나는 마지막 tick 자체의 맵매칭이 실패하면(정확도 나쁨·이상속도·좌표무효 등, 트립 막바지에 흔함)
 *   그 시점에 열려있던 개방형 run 이 과금 레코드 없이 세션과 함께 사라졌다(최소 재현으로 확인).
 *   AppendExpiredOpenGateCharge() 는 TTL 만료 전용(세션 전체가 wall-clock 기준으로 유휴 판정됐을 때)
 *   이라 stSession.dtLastSeen(=time(nullptr))·dwLastGpsSeq(세션 공통값)를 쓰는데, 트립종료는 "이번
 *   tick 이 마지막"이라는 정확한 GPS 시각을 아는 경우라 CLOSED/SPEED·EXEMPT(AppendExpiredExemptZoneCharge,
 *   run 별 dtLastInZoneTime 사용)와 동일하게 run 별 마지막 확정 시각을 그대로 쓴다.
*/
void CRawLogWorker::AppendTripEndOpenGateCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if (stSession.vtOpenRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtOpenRuns.size(); ++i)
	{
		// [버그 수정, 2026-09-21 최정우] DIST_M — 짝 함수 AppendExpiredOpenGateCharge() 가
		//   2026-09-14 에 받은 수정(사용자 지적)이 **이 함수에는 적용되지 않은 채 남아 있었다.**
		//   그쪽 근거를 그대로 옮기면: "정상진입(bStartedByTrip=false) run 은 BuildOpenZoneRow()
		//   가 실측과 무관하게 구역 등록 전체길이(pstZone->dfLengthM)를 쓰는데, 정상 **진출**이
		//   확인됐을 때나 맞는 값이다. 진출을 못 본 채 마감하는 경우엔 구역을 다 지났는지조차
		//   확실치 않으므로 전체길이를 청구하면 실측보다 부풀려진다."
		//   이 함수가 불리는 상황이 정확히 그 경우다 — **트립의 마지막 tick 자체가 맵매칭에
		//   실패해** 정상 Y/0 진출 처리를 못 하고 N/3(AUDIT)로 마감하는 경로다(함수 @remark 참고).
		//   즉 "구역 안에서 끝났다" 는 점에서 TTL 마감과 성질이 같은데, 한쪽만 고쳐져 있었다.
		//   구역 전체길이는 최대 2,621m(현 기준정보)라 과다청구 방향의 차이다.
		//   run 을 복사해 bStartedByTrip=true 로 강제하면, 어떤 진입 방식이었든 실측 누적거리
		//   (dfAccumDistM)와 실측 좌표(dfEntryX/Y, dfLastX/Y)를 쓰게 된다 — dfAccumDistM 은
		//   정상진입 run 도 매 tick 정확히 누적해두고 쓰지 않을 뿐이라 그대로 재사용 가능하다.
		ZONE_RUN_SESSION stEndRun = stSession.vtOpenRuns[i];
		stEndRun.bStartedByTrip = true;

		CHARGE_INSERT_ROW stRow;
		BuildOpenZoneRow(stEndRun, stSession.szTripId, strDeviceKey,
			nSeq, stSession.vtOpenRuns[i].dtLastInZoneTime,
			stSession.vtOpenRuns[i].dwLastInZoneGpsSeq, &stRow);
		stRow.strChargeYn = "N";
		stRow.strChargeStatus = "3";
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — AppendExpiredOpenGateCharge
		//   와 동일 근거(BuildOpenZoneRow 내부 11번을 트립종료 공통사유 61로 덮어씀)
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", NCR_TTL_FORCED_CLOSE);
		stRow.strNonChargeReason = szReason;
		pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTW("[#%02d] open zone trip-end flush!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"dist_m=[%s] non_charge_reason=[%d:%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId, nSeq - 1,
			stSession.vtOpenRuns[i].szRoadID, stRow.strDistM.c_str(), NCR_TTL_FORCED_CLOSE,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_TTL_FORCED_CLOSE));
	}
}

/**
 * @brief 구역 이탈 경계 보정 — 구역 안 마지막 확인 위치에서 그 링크의 종료 노드까지 거리를 더한다
 * @param[in] qwLastZoneLinkID 구역 안에서 마지막으로 확인된 링크 ID (0이면 아무것도 안 함)
 * @param[in,out] pdfLastZoneX 구역 안 마지막 확인 위치 경도 — 보정 성공 시 종료 노드로 이동
 * @param[in,out] pdfLastZoneY 구역 안 마지막 확인 위치 위도
 * @param[in,out] pdfAccumDistM 누적 이동거리(m) — 보정분이 가산된다
 * @return 가산된 tail 거리(m). 0이면 보정 안 함
 * @remark [2026-09-15 최정우 추가, 소스 재검토 지적] 거리 누적을 "구역 안 tick 만"으로 게이팅한
 *   뒤로는, 마지막 구역 안 tick ~ 실제 구역 경계 구간이 통째로 빠진다(그 사이 tick 은 이미 구역
 *   밖이라 누적에서 제외되므로). GPS 3초 간격·100km/h 면 최대 83m 손실이고, 구간단속에서는
 *   누적거리가 짧아지면 평균속도가 낮게 나와 **위반 판정이 뒤집혀 레코드 자체가 사라질 수 있다.**
 *   면제도로 ProcessExemptZoneCharge() 가 이미 쓰고 있는 이탈 경계 보정과 동일 원리를 구간단속·
 *   폐쇄형에도 적용하기 위해 공용 함수로 뺐다. 링크 길이+1m 를 넘는 tail 은 매칭 오류로 보고 버린다.
 * @warning **"차가 실제로 구역을 벗어난" 경로에서만 호출할 것.** TTL 만료·트립종료처럼 구역 안에서
 *   끝난 경우(AppendExpiredClosedRoadCharge/AppendExpiredSpeedZoneCharge/AppendExpiredExemptZoneCharge)
 *   에는 차가 링크 끝까지 갔다는 증거가 없어 주행하지 않은 거리를 청구하게 된다 — 2026-09-15 오전에
 *   그 두 곳에 잘못 넣었다가 같은 날 오후 재검토에서 되돌렸다. 면제도로가 `!bSameZone` 으로 분기하는
 *   이유가 이것이다(사용자 지시, 2026-08-30). 현재 정당한 호출부는 "구역 이탈 확정" 2곳뿐이다.
*/
double CRawLogWorker::ApplyZoneExitTailDist(uint64 qwLastZoneLinkID,
		double *pdfLastZoneX, double *pdfLastZoneY, double *pdfAccumDistM)
{
	if ((qwLastZoneLinkID == 0) || (pdfLastZoneX == nullptr) || (pdfLastZoneY == nullptr)
		|| (pdfAccumDistM == nullptr) || (m_stConfig.pcDataLoader == nullptr))
		return 0.0;
	if ((*pdfLastZoneX == 0.0) && (*pdfLastZoneY == 0.0))
		return 0.0;											// 구역 안 위치 자체가 없음

	PLINK_INFO pstLastLink = m_stConfig.pcDataLoader->GetLinkInfo(qwLastZoneLinkID);
	if (pstLastLink == nullptr)
		return 0.0;

	POINT stFrom, stNode;
	stFrom.dfX = *pdfLastZoneX;  stFrom.dfY = *pdfLastZoneY;
	stNode.dfX = static_cast<double>(pstLastLink->dwEdNodeX) / 360000.0;
	stNode.dfY = static_cast<double>(pstLastLink->dwEdNodeY) / 360000.0;

	double dfTail = HaversineMeters(stFrom, stNode);
	if ((dfTail <= 0.0) || (dfTail > pstLastLink->dfLen + 1.0))
		return 0.0;

	*pdfAccumDistM += dfTail;
	*pdfLastZoneX = stNode.dfX;
	*pdfLastZoneY = stNode.dfY;
	return dfTail;
}

/**
 * @brief 강제마감 시점에 입구만 통과하고 출구를 못 찾은 폐쇄형 세션을 N/3(AUDIT)로 마감
 *   (2026-08-14 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 마감 직전 세션(제거 전 스냅샷)
 * @param[in] dtEndTime 마감 기준 시각 — TTL 경로는 dtLastSeen(벽시계), 트립종료·트립전환 경로는
 *   그 트립의 마지막 GPS 시각(FlushOpenRunsAsAbnormalEnd() 가 맞춰 넘긴다)
 * @param[out] pvtOut 세션이 입구 통과 상태였으면 CHARGE_INSERT_ROW 1건 추가
 * @param[in] bNoTripEnd true=종료신호 없이 다음 운행이 시작돼 마감(non_charge_reason 62),
 *   false=TTL·잔여tick·트립종료(61) (2026-09-16 최정우 추가)
 * @remark 기존엔 이 함수 자체가 없어 "입구는 확인했지만 출구 신호가 영영 안 온" 세션이 조용히
 *   사라지면 그 진입 사실이 통째로 유실됐다(휴게소 장시간 정차 등). to_id/출구게이트는 여전히
 *   못 봤으니 지어내지 않고 비워두지만, dist_m/speed_kmh/to_lat·lon 은 2026-08-25부터
 *   dfClosedAccumDistM 과 dfClosedLastZoneX/Y(구역 **안** 마지막 확인 위치)로 채운다.
 *   꼬리 보정(ApplyZoneExitTailDist)은 여기서 쓰면 안 된다 — 함수 본문 주석 참고.
 *   [2026-09-15 적용범위 확대] 함수명은 "Expired" 지만 TTL 만료 전용이 아니다 — 트립종료·
 *   트립전환·서버 종료 경로에서도 호출된다.
 *   [2026-09-17 최정우 정정] 종전엔 이 설명 블록이 ApplyZoneExitTailDist() 위에 붙어 있었다
 *   (동작 무관). to_lat/lon 출처도 실제 코드(dfClosedLastZoneX/Y, 2026-09-15 수정분)에 맞췄다.
*/
void CRawLogWorker::AppendExpiredClosedRoadCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut, bool bNoTripEnd)
{
	// 마감 사유 코드 — 종료신호 없이 다음 운행이 시작된 경로면 62, 그 밖의 마감
	//   (TTL·잔여tick·트립종료)이면 61. 판정(charge_yn/status)은 동일하다
	//   (2026-09-16 최정우 추가)
	const int nReasonCode = bNoTripEnd ? NCR_NO_TRIP_END_FORCED_CLOSE : NCR_TTL_FORCED_CLOSE;

	if (!stSession.bInClosedRoad || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stSession.szClosedRoadId);

	// [버그 수정, 2026-09-15 최정우 — 같은 날 오전 수정의 회귀를 오후 재검토에서 되돌림]
	//   오전에 여기에 ApplyZoneExitTailDist()(구역 안 마지막 위치 → 링크 종료노드까지 가산)를
	//   넣었는데 **이 함수에는 적용하면 안 된다.** 꼬리 보정은 "차가 실제로 구역을 벗어났고,
	//   벗어난 뒤 tick 들은 이미 구역 밖이라 누적에서 빠졌다"는 전제에서만 근거가 있다. 반면 이
	//   함수는 이 함수 헤더 주석대로 **출구를 못 본 채 강제마감으로 끝난** 경우에만 호출되므로
	//   (진입부 !bInClosedRoad 조기반환), 차가 링크 끝까지 갔다는 증거가 없다. 300m 링크 20m
	//   지점에서 정차·종료하면 280m 가 그대로 과다청구된다.
	//   근거 2건: (a) 기준 구현인 면제도로 ProcessExemptZoneCharge() 의 이탈 경계 보정은 `!bSameZone` 으로
	//   명시 분기하며 "구역 안에서 끝난 경우는 그 지점이 곧 진출점이므로 보정하지 않는다
	//   (사용자 지시, 2026-08-30)" 주석이 있고, AppendExpiredExemptZoneCharge() 는 아예 보정을
	//   안 한다. (b) 아래 TO_LAT/LON 주석의 "구역 안 마지막 확인 좌표 + 실측 누적거리(사용자 확인)".
	//   → 보정 없이 세션 값을 그대로 쓴다. stSession 이 const& 라 지역 복사본으로 받는다.
	const double dfClosedEndX = stSession.dfClosedLastZoneX;
	const double dfClosedEndY = stSession.dfClosedLastZoneY;
	const double dfClosedEndDistM = stSession.dfClosedAccumDistM;

	CHARGE_INSERT_ROW stRow;
	stRow.strTripId = stSession.szTripId;
	stRow.strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", stSession.nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "2";								// CLOSED_ROAD
	stRow.strChargeUnit = "1";								// LINK (실측 확인)
	stRow.strLinkId = "";

	stRow.strFromId = stSession.szEntryTollgateId;			// 입구는 확인됨
	stRow.strToId = "";										// 출구 미확인 — 지어내지 않음

	char szFromLat[32], szFromLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stSession.dfEntryFromLat);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stSession.dfEntryFromLon);
	stRow.strFromLat = szFromLat;
	stRow.strFromLon = szFromLon;
	// 출구 미확인이라도 실시간 누적거리·마지막 확인 위치는 이제 세션에 있음(2026-08-25 최정우
	//   추가 — ProcessClosedRoadCharge() 의 "구역 이탈" 보조판정과 같은 필드 재사용). dfClosedLastX/Y
	//   초기값이 진입 위치라 세션이 TTL로 지워질 때까지 한 틱도 못 받았어도 0m·진입 위치 그대로 나옴
	char szToLat[32], szToLon[32];
	// [버그 수정, 2026-09-15 최정우] 구역 **안** 마지막 위치를 쓴다 — dfClosedLastX/Y 는 구역
	//   밖에서도 갱신되는 세션 위치라, 거리 누적을 구역 내부로 게이팅한 뒤로는 dist_m 과
	//   TO_LAT/LON 이 서로 다른 구간을 가리키게 된다
	snprintf(szToLat, sizeof(szToLat), "%.06lf", dfClosedEndY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", dfClosedEndX);
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = stSession.szClosedRoadId;
	stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfClosedEndDistM + 0.5));
	stRow.strDistM = szDistM;

	double dfDwellSec = difftime(dtEndTime, stSession.dtEntryTime);
	if (dfDwellSec > 0.0)
	{
		double dfAvgSpeedKmh = (dfClosedEndDistM / dfDwellSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}
	stRow.strSpeedLimitKmh = "";							// 출구 링크를 특정 못 함 — 지어내지 않음

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", stSession.dwEntryGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", stSession.dwClosedLastGpsSeq);
	stRow.strStartGpsSeq = szStartGpsSeq;
	stRow.strEndGpsSeq = szEndGpsSeq;

	stRow.strOccurDt = FormatDateTime14(stSession.dtEntryTime);

	const char *pszTripStartDt = ExtractTripStartDt(stSession.szTripId);
	if (pszTripStartDt != nullptr)
		stRow.strTripStartDt = pszTripStartDt;
	else
		stRow.strTripStartDt = stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = stSession.szEntryTollgateId;
	stRow.strExitTollgateId = "";

	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;

	stRow.strTripEndDt = FormatDateTime14(dtEndTime);
	stRow.strChargeYn = "N";
	stRow.strChargeStatus = "3";							// AUDIT — 다른 TTL-flush 유형과 동일 관례
	// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(N/3)은 안 건드림
	char szReason[8];
	snprintf(szReason, sizeof(szReason), "%d", nReasonCode);
	stRow.strNonChargeReason = szReason;

	pvtOut->push_back(stRow);

	// [문구 정정, 2026-09-16 최정우] "(ttl expiry)" → "(forced close)" — 이 함수들은
	//   2026-09-15 부터 TTL 뿐 아니라 잔여tick·트립종료·트립전환·서버종료에서도 불린다.
	//   TTL 이 아닌 경로에서도 "ttl expiry" 로 찍혀 로그만 보고 원인을 오판하기 쉬웠다.
	//   같은 맥락으로 "seq=" 라벨도 "charge_seq=" 로 바꿨다 — 찍히는 값이 GPS 순번이
	//   아니라 nChargeSeq(과금 순번)인데 GPS seq 로 오해할 수 있었다.
	LOGFMTW("[#%02d] closed road recorded (forced close)!device=[%s] trip_id=[%s] charge_seq=[%d] road=[%s] "
		"entry=[%s] exit=[unknown] non_charge_reason=[%d:%s]",
		nThreadId, strDeviceKey.c_str(), stSession.szTripId, stSession.nChargeSeq,
		stSession.szClosedRoadId, stSession.szEntryTollgateId, nReasonCode,
		m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nReasonCode));
}

/**
 * @brief 강제마감 시점에 입구만 통과하고 출구를 못 찾은 구간단속 세션을 마감 —
 *   **일반도로(NODE_STEP) 미러 1건만 Y/0 으로 적재**한다 (2026-08-14 최정우 추가,
 *   2026-09-06 적재 정책 변경)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 마감 직전 세션(제거 전 스냅샷)
 * @param[in] dtEndTime 마감 기준 시각(호출측이 상황에 맞는 값을 넘긴다)
 * @param[out] pvtOut NODE_STEP 미러 1건 추가
 * @param[in] dwEndGpsSeq 종료 tick 의 GPS_SEQ(0=미지정, 세션값 사용). 트립 종료 이벤트 tick 은
 *   직전 tick 의 복사본이라 구역 갱신 경로를 안 타서 세션의 dwSpeedLastGpsSeq 가 그 앞에서
 *   멈춘다 — 호출측이 실제 종료 seq 를 넘겨 구간 표기를 트립 끝에 맞춘다(2 tick 이내일 때만
 *   확장, 본문 주석 참고)
 * @remark **이 함수는 SPEED(CHARGE_TYPE=3) 행을 만들지 않는다.** 2026-09-06 에 "구간단속은
 *   진입·진출 게이트 통과가 모두 확인되고 평균속도가 제한속도 이상일 때만 적재" 로 바뀌었는데,
 *   강제마감은 정의상 진출 게이트를 못 지난 경우라 그 조건을 채울 수 없기 때문이다. 그 구간의
 *   주행 자체는 실재하므로 일반도로 미러가 Y/0 으로 이어받는다.
 *   (2026-09-17 최정우 — 그 결정 뒤 남아 있던 SPEED 행 조립 코드를 삭제. 본문 주석 참고)
 *   to_lat/lon·dist_m 은 dfSpeedLastZoneX/Y·dfSpeedAccumDistM(구역 **안** 마지막 확인 위치와
 *   실측 누적거리, 2026-09-15 수정분)을 쓴다. from_lat/lon 은 구역 등록 좌표(ZONE_INFO.
 *   dfFirstLat/Lon) 그대로다. 꼬리 보정(ApplyZoneExitTailDist)은 쓰지 않는다 — 본문 주석 참고.
 *   [2026-09-15 적용범위 확대] 함수명은 "Expired" 지만 TTL 만료 전용이 아니다 — 트립종료·
 *   트립전환·서버 종료 경로에서도 호출된다. 다만 다른 Append* 와 달리 bNoTripEnd 파라미터가
 *   없다 — 유일한 출력인 NODE_STEP 미러가 Y/0(non_charge_reason=0)이라 62/52 를 붙일 행이
 *   없기 때문이다(2026-09-17 최정우 확인).
*/
void CRawLogWorker::AppendExpiredSpeedZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut,
		uint32 dwEndGpsSeq)
{
	if (!stSession.bInSpeedZone || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stSession.szSpeedZoneRoadId);

	// [버그 수정, 2026-09-15 최정우] 오전에 넣은 ApplyZoneExitTailDist() 를 되돌린다 — 근거는
	//   AppendExpiredClosedRoadCharge() 주석 참고(출구를 못 본 채 끝난 경우라 꼬리 보정의 전제가
	//   성립하지 않음). 구간단속은 오히려 더 위험하다: 부풀려진 거리가 평균속도를 올려 **위반
	//   판정을 없는 위반으로 뒤집을 수 있고**, 아래 일반도로 미러 행은 charge_yn=Y/0 실과금이라
	//   그 거리가 그대로 청구된다.
	const double dfSpeedEndX = stSession.dfSpeedLastZoneX;
	const double dfSpeedEndY = stSession.dfSpeedLastZoneY;
	const double dfSpeedEndDistM = stSession.dfSpeedAccumDistM;

	// 구간단속은 **진입·진출 게이트 통과가 모두 확인되고 평균속도가 제한속도 이상일 때만** 적재한다
	//   (2026-09-06 최정우 수정, 사용자 지시). TTL 강제마감·비정상종료는 정의상 진출 게이트를 못
	//   지난 것이므로 이 경로에서는 레코드를 만들지 않는다. 그 구간은 아래 NODE_STEP(일반도로)이
	//   그대로 이어받는다 — 실측 000376_20260819140856: 진입 TG00012 만 있고 진출이 없는데도
	//   171m 가 구간단속 N/3 으로 적재됐다. 종전 정책(게이트 이상 시 AUDIT 로 남겨 사람이 확인)을
	//   대체한다 — 구간단속은 게이트 통과가 성립 요건이라 미통과 건을 심사 큐에 올릴 근거가 없다.
	//   NODE_STEP 등록은 종전대로 계속한다(아래 블록) — 주행 자체는 실재하므로 일반도로로 남는다.
	//   [2026-09-17 최정우 정리] 이 결정 뒤에도 push 되지 않은 채 남아 있던 SPEED 행 조립 89줄을
	//   삭제했다. 되살릴 일이 생기면 그 옛 코드가 아니라 **ProcessSpeedZoneCharge() 의 "출구 미확인"
	//   분기**를 본뜰 것 — 옛 코드는 2026-09-10 FROM 보정과 2026-09-11 non_charge_reason 배선을
	//   둘 다 못 받은 낡은 사본이었다(상세: doc/과금_맵매칭_로직_색인.md).
	// [문구 정정, 2026-09-16 최정우] "(ttl expiry)" → "(forced close)" — 이 함수들은
	//   2026-09-15 부터 TTL 뿐 아니라 잔여tick·트립종료·트립전환·서버종료에서도 불린다.
	//   TTL 이 아닌 경로에서도 "ttl expiry" 로 찍혀 로그만 보고 원인을 오판하기 쉬웠다.
	LOGFMTI("[#%02d] speed zone skipped (forced close, no exit gate)!device=[%s] trip_id=[%s] road=[%s]",
		nThreadId, strDeviceKey.c_str(), stSession.szTripId, stSession.szSpeedZoneRoadId);

	// NODE_STEP 일반도로 확장(케이스1) — TTL 강제마감·비정상종료 시에는 출구 자체가 없어 위반
	//   여부를 판정할 수 없으므로(정상종료 전제인 "평균속도 vs 제한속도" 판정 불가) NODE_STEP 만
	//   등록한다. TRIP_END_DT를 직접 채워 INSERT하므로(사후정정 없음 — 종전 [trip_abend] 는 2026-10-04 삭제)
	//   여기서 정한 값이 최종값 (2026-09-01 최정우 추가)
	//   **charge_yn/status 는 N/3 -> Y/0** (2026-09-06 최정우 수정, 사용자 지시). 같은 날 구간단속
	//   자체를 "게이트 양쪽 통과 + 제한속도 이상"일 때만 적재하도록 바꾸면서, 그 조건을 못 채운
	//   구간은 구간단속 레코드 없이 **일반도로로만** 남는다. 그 주행은 실제로 있었고 일반도로로서는
	//   정상이므로 심사대상(N/3)이 아니라 정상 과금(Y/0)이 맞다 — 종전 N/3 은 "구간단속을 확정
	//   못한 채 끝났다"는 표시였는데 이제 구간단속 레코드 자체가 없으므로 그 의미가 사라졌다.
	//   from/to 좌표와 거리는 종전대로 **구역 시작 좌표 ~ 구역 안 마지막 확인 좌표**와 실측 누적
	//   거리(dfSpeedAccumDistM)를 쓴다 — 등록 구역 전체 길이가 아니다(사용자 확인).
	{
		// [버그 수정, 2026-09-15 최정우] charge_seq 는 nChargeSeq+1 이 아니라 nChargeSeq 를 그대로
		//   써야 한다 — 다른 모든 마감 경로(주정차·폐쇄형·일반도로·면제)의 공통 관례이고, 호출측
		//   (ProcessRawLog/ExpireTtlSessions)이 "실제로 push 된 행 수"만큼만 nChargeSeq 를 올리기
		//   때문이다. 원래는 이 함수가 구간단속 행(nChargeSeq)과 일반도로 미러(nChargeSeq+1) 2건을
		//   넣었는데, 2026-09-06 에 구간단속 행 등록이 폐지되면서 push 는 1건인데 번호만 +1 로
		//   남아 (a) nChargeSeq 번이 비고 (b) 호출측 +1 뒤 다음 레코드가 같은 번호를 재사용하는
		//   충돌이 생겼다 — 실측 000376_20260826160622 에서 이 행과 뒤따르는 일반도로 마감행이
		//   둘 다 charge_seq=3 이었다(사용자 지적). 그때 남아 있던 구간단속 행 조립 코드는
		//   2026-09-17 에 삭제했고, 이 함수가 넣는 행은 이제 이 미러 1건뿐이다.
		CHARGE_INSERT_ROW stNodeStepRow;
		char szNodeStepSeq[16];
		snprintf(szNodeStepSeq, sizeof(szNodeStepSeq), "%d", stSession.nChargeSeq);
		BuildNodeStepRowFromLinkRange(stSession.szTripId, strDeviceKey, stSession.nChargeSeq,
			stSession.qwSpeedEntryLinkID, stSession.qwSpeedLastZoneLinkID,
			(pstZone != nullptr) ? pstZone->dfFirstLat : 0.0, (pstZone != nullptr) ? pstZone->dfFirstLon : 0.0,
			dfSpeedEndY, dfSpeedEndX,	// 구역 **안** 마지막 확인 위치(보정 없음 — 위 ApplyZoneExitTailDist 미적용 주석)
			dfSpeedEndDistM, stSession.dtSpeedEntryTime, dtEndTime,
			stSession.dwSpeedEntryGpsSeq,
			// 종료 seq — 구역 안 마지막 확인 tick 과 트립 마지막 확정 tick 중 큰 쪽. 트립 종료
			//   이벤트(TRIP_EVENT=2) tick 은 직전 tick 과 좌표·시각이 같은 복사본이라 구역 갱신
			//   경로를 안 타서 dwSpeedLastGpsSeq 가 그 앞에서 멈춘다 — 실측
			//   000376_20260819140856: 마지막 tick 이 seq23 인데 22 로 기록됐다. 거리·시간에는
			//   기여가 없고 구간 표기만 어긋나므로 큰 쪽을 쓴다 (2026-09-06 최정우 수정, 사용자 지시)
			// [버그 수정, 2026-09-15 최정우] 무조건 큰 쪽이 아니라 **2 tick 이내로 붙어 있을 때만**
			//   확장한다. 위 수정으로 dwSpeedLastGpsSeq 가 "구역 안" 마지막 tick 이 되면서, 게이트를
			//   안 거치고 구역을 벗어난 뒤 한참 더 달린 트립에서 이 max 가 구간을 트립 끝까지
			//   늘려버린다(실측 000376_20260826160622: 구역은 31~103 인데 31~242 로 기록됨).
			//   원래 의도였던 "TRIP_EVENT=2 복사본 tick 한 칸" 보정만 남긴다.
			((dwEndGpsSeq > stSession.dwSpeedLastGpsSeq)
				&& ((dwEndGpsSeq - stSession.dwSpeedLastGpsSeq) <= 2))
				? dwEndGpsSeq : stSession.dwSpeedLastGpsSeq,
			"Y", "0",
			nullptr, nullptr, &stNodeStepRow);
		stNodeStepRow.strTripEndDt = FormatDateTime14(dtEndTime);
		pvtOut->push_back(stNodeStepRow);

		// [문구 정정, 2026-09-16 최정우] "(ttl expiry)" → "(forced close)" — 이 함수들은
		//   2026-09-15 부터 TTL 뿐 아니라 잔여tick·트립종료·트립전환·서버종료에서도 불린다.
		//   TTL 이 아닌 경로에서도 "ttl expiry" 로 찍혀 로그만 보고 원인을 오판하기 쉬웠다.
		//   같은 맥락으로 "seq=" 라벨도 "charge_seq=" 로 바꿨다 — 찍히는 값이 GPS 순번이
		//   아니라 nChargeSeq(과금 순번)인데 GPS seq 로 오해할 수 있었다.
		LOGFMTI("[#%02d] node step recorded (forced close, from speed zone)!device=[%s] trip_id=[%s] charge_seq=[%s] "
			"road=[%s] non_charge_reason=[%d:%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId, szNodeStepSeq, stSession.szSpeedZoneRoadId,
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
	}
}

/**
 * @brief 예약된 batch 전건 PROCESSING→PENDING release (#7/#8)
 * @param[in] pcConn DB 커넥션
 * @param[in] vtBatch 예약된 GPS batch
 * @param[in] nThreadId 로그용 워커 ID (-1 이면 생략)
 * @return true(전건 release), false(실패·인자 무효)
*/
bool CRawLogWorker::ReleaseReservedBatch(PGconn *pcConn, const RAW_LOG_BATCH& vtBatch, int nThreadId)
{
	if (pcConn == nullptr || vtBatch.empty() || m_stConfig.strUpdateSQL.empty())
		return false;

	vector<RAW_LOG_UPDATE_ROW> vtRelease;
	vtRelease.reserve(vtBatch.size());

	for (size_t i=0; i<vtBatch.size(); ++i)
	{
		// batch 1건씩 release 행 목록 적재 (2026-07-08 최정우 주석 추가)
		AppendReleaseRowFromRawLog(&vtRelease, vtBatch[i]);
	}

	if (vtRelease.empty())
		return false;

	// 예약 batch PROCESSING→PENDING bulk release (2026-07-08 최정우 주석 추가)
	if (!BulkReleaseRawLogs(pcConn, vtRelease))
	{
		if (nThreadId >= 0)
		{
			LOGFMTE("[#%02d] batch reserve release failed!device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
				static_cast<int>(vtRelease.size()));
		}
		else
		{
			LOGFMTE("batch reserve release failed!device=[%s] trip_id=[%s] count=[%d]",
				vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
				static_cast<int>(vtRelease.size()));
		}
		return false;
	}

	if (nThreadId >= 0)
	{
		LOGFMTW("[#%02d] batch reserve released!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
			nThreadId, vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
			static_cast<int>(vtRelease.size()));
	}
	else
	{
		LOGFMTW("batch reserve released!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
			vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
			static_cast<int>(vtRelease.size()));
	}

	return true;
}

/**
 * @brief TRIP_EVENT 값 유효 여부 (0/1/2)
 * @param[in] nTripEvent TRIP_EVENT SMALLINT
 * @return true(유효), false(실패)
*/
bool CRawLogWorker::IsValidTripEvent(sint16 nTripEvent)
{
	return (nTripEvent == TRIP_EVENT_START)
		|| (nTripEvent == TRIP_EVENT_NONE)
		|| (nTripEvent == TRIP_EVENT_END);
}

/**
 * @brief TRIP_ID 가 CAR_SEQ_NO 기반 형식인지 검사
 * @param[in] stRawLogInfo 원시 GPS
 * @return true(유효), false(실패)
 * @remark 형식: {CAR_SEQ_NO 6자리 숫자}_{YYYYMMDDHH24MISS} (2026-08-18 최정우 수정 —
 *   DEVICE_KEY 접두사 검사에서 CAR_SEQ_NO 6자리 숫자 접두사 검사로 변경. 클라이언트가
 *   BASE_CARINFO.CAR_SEQ_NO 를 6자리로 0-패딩해 trip_id 를 생성하는 방식으로 확인됨에
 *   따라, 실제 발급 방식에 맞춰 형식 검증을 수정. 특정 device 의 CAR_SEQ_NO 와
 *   일치하는지까지는 대조하지 않는 단순 패턴 검사(숫자 6자리 + '_')다.)
*/
bool CRawLogWorker::IsValidTripIdForDevice(const sRawLogInfo& stRawLogInfo)
{
	if (stRawLogInfo.szDeviceKey[0] == '\0' || stRawLogInfo.szTripID[0] == '\0')
		return false;

	for (int i = 0; i < 6; ++i)
	{
		if (!isdigit(static_cast<unsigned char>(stRawLogInfo.szTripID[i])))
			return false;
	}

	return (stRawLogInfo.szTripID[6] == '_');
}

/**
 * @brief 수집 데이터 2차 검증 (위치검증서버 방어)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[out] pnRejectStatus 거부 시 MATCH_STATUS (SKIP)
 * @return true(맵매칭 진행 가능), false(거부)
*/
bool CRawLogWorker::ValidateRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo,
		sint16 *pnRejectStatus)
{
	if (pnRejectStatus == nullptr)
		return false;

	*pnRejectStatus = MATCH_STATUS_SKIP;

	if (stRawLogInfo.szDeviceKey[0] == '\0')
	{
		LOGFMTW("[#%02d] reject empty device_key!seq=[%u]",
			nThreadId, stRawLogInfo.dwSeqNo);
		return false;
	}

	if (stRawLogInfo.szTripID[0] == '\0')
	{
		LOGFMTW("[#%02d] reject empty trip_id!device=[%s] seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo);
		return false;
	}

	// trip_id 가 CAR_SEQ_NO_{ts} 형식인지 검사 (2026-07-08 최정우 주석 추가, 2026-08-18 최정우 수정)
	if (!IsValidTripIdForDevice(stRawLogInfo))
	{
		LOGFMTW("[#%02d] reject invalid trip_id format!device=[%s] trip_id=[%s] seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
		return false;
	}

	// TRIP_EVENT 0/1/2 유효값 검사 (2026-07-08 최정우 주석 추가)
	if (!IsValidTripEvent(stRawLogInfo.nTripEvent))
	{
		LOGFMTW("[#%02d] reject invalid trip_event=[%d]!device=[%s] trip_id=[%s] seq=[%u]",
			nThreadId, static_cast<int>(stRawLogInfo.nTripEvent),
			stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
		return false;
	}

	return true;
}

/**
 * @brief GPS 좌표·RAW_VLD 유효성 검사 (맵매칭 제외 대상)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @return true(SKIP=3 처리), false(맵매칭 진행 가능)
 * @remark
 *   - GPS_LAT 또는 GPS_LON 이 NULL
 *   - RAW_VLD 가 FALSE 또는 NULL
*/
bool CRawLogWorker::ShouldSkipGpsInput(int nThreadId, const sRawLogInfo& stRawLogInfo, bool bIgnoreRawVld)
{
	if ((stRawLogInfo.bGpsLatNull) || (stRawLogInfo.bGpsLonNull))
	{
		LOGFMTW("[#%02d] reject null gps coord!device=[%s] trip_id=[%s] seq=[%u] lat_null=[%d] lon_null=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<int>(stRawLogInfo.bGpsLatNull),
			static_cast<int>(stRawLogInfo.bGpsLonNull));
		return true;
	}

	// RAW_VLD=false 는 주정차 판정에도 태우지 않는다(2026-08-22 사용자 확정). 실측에서 이런 좌표의
	//   ACCURACY_M 이 78~362m(평균 149m)였는데, RL-Z00001 폴리곤은 면적 12,624m^2 로 대각선이 약 110m
	//   라 "폴리곤 안에 있었다"는 사실 자체를 신뢰할 수 없다. 그 결과 실제 정차 2건(178초·75초)이
	//   기록되지 않지만, 이건 이 검사가 도입된 시점부터의 동작이고 규칙 변경과 무관하다.
	//   완화하려면 폴리곤 크기 대비 ACCURACY_M 임계값을 구역별로 둬야 해서 관리비용이 크다고 판단.
	//   ignore_rawvld=1 이면 이 검사를 건너뛴다 — 운영 데이터 전량 매칭 검증용 (2026-08-23 최정우 추가)
	if (!bIgnoreRawVld && ((!stRawLogInfo.bRawVldKnown) || (!stRawLogInfo.bRawVld)))
	{
		LOGFMTW("[#%02d] reject invalid raw_vld!device=[%s] trip_id=[%s] seq=[%u] known=[%d] raw_vld=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<int>(stRawLogInfo.bRawVldKnown),
			static_cast<int>(stRawLogInfo.bRawVld));
		return true;
	}

	return false;
}

/**
 * @brief 이동거리 환산속도 vs SPEED_KMH 정합성 검사 (2026-07-20 최정우 추가)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stSession 현재 trip_id 세션 (직전 매칭 성공 위치·시각 기준)
 * @param[out] pnImpliedSpeedKmh 이동거리 환산속도(km/h) — 로그·참고용, nullable
 * @return true(SKIP 대상 — 이동거리가 SPEED_KMH 대비 비정상), false(정상 또는 판단 불가)
 * @remark
 *   전제: config speed_factor>0, 직전 매칭 성공 위치 보유(bHasLastMatch), 직전 포인트가 정상 매칭
 *         (bLastPointOk — 아니면 갭이 정상 1구간보다 넓어져 평균/순간 비교 신뢰 못함, 2026-07-21 최정우 추가),
 *         SPEED_KMH 유효(NULL 아님), 직전 매칭 시각과의 간격이 (0, MM_CALC_MAX_GAP_SEC] 이내
 *   판정: 환산속도(직전 매칭 위치→현재 GPS 하버사인 거리 / 시간간격) > SPEED_KMH × speed_factor + speed_margin
 *   예) SPEED_KMH=37, factor=2.0, margin=25 → 상한 99km/h. 환산속도 187km/h → SKIP
*/
bool CRawLogWorker::ShouldSkipImplausibleSpeed(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, int *pnImpliedSpeedKmh)
{
	if (pnImpliedSpeedKmh != nullptr)
		*pnImpliedSpeedKmh = -1;

	if (m_stConfig.dfSpeedFactor <= 0.0)
		return false;			// 비활성
	if (!stSession.bHasLastMatch)
		return false;			// 비교 기준(직전 매칭 위치) 없음 — START 등
	if (!stSession.bLastPointOk)
		return false;			// 직전 포인트가 매칭 실패 — 갭이 정상 1구간보다 넓어져 평균/순간 비교 신뢰 못함 (2026-07-21 최정우 추가)
	if (stRawLogInfo.fSpeed < 0.0f)
		return false;			// SPEED_KMH NULL — 비교 불가

	double dfGapSec = difftime(stRawLogInfo.dtGPS, stSession.dtLastMatchGps);
	if (dfGapSec <= 0.0 || dfGapSec > static_cast<double>(MM_CALC_MAX_GAP_SEC))
		return false;			// 공백·역전 구간 — 판단 불신

	POINT stPrev; stPrev.dfX = stSession.dfLastMatchX; stPrev.dfY = stSession.dfLastMatchY;
	POINT stCur;  stCur.dfX = stRawLogInfo.dfX;         stCur.dfY = stRawLogInfo.dfY;
	// 직전 매칭 위치→현재 GPS 하버사인 거리(m) (2026-07-20 최정우 추가)
	double dfMoveM = HaversineMeters(stPrev, stCur);
	double dfImpliedKmh = (dfMoveM / dfGapSec) * 3.6;

	if (pnImpliedSpeedKmh != nullptr)
		*pnImpliedSpeedKmh = static_cast<int>(dfImpliedKmh + 0.5);

	double dfLimitKmh = static_cast<double>(stRawLogInfo.fSpeed) * m_stConfig.dfSpeedFactor
		+ static_cast<double>(m_stConfig.nSpeedMargin);
	if (dfImpliedKmh <= dfLimitKmh)
		return false;

	LOGFMTW("[#%02d] reject implausible speed!device=[%s] trip_id=[%s] seq=[%u] "
		"move=[%.1fm] gap=[%.1fs] implied=[%.1fkm/h] reported=[%.1fkm/h] limit=[%.1fkm/h]",
		nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
		dfMoveM, dfGapSec, dfImpliedKmh, static_cast<double>(stRawLogInfo.fSpeed), dfLimitKmh);
	return true;
}

/**
 * @brief Begin 폴백(위상 연결 미검증) 확정 결과의 이동거리 타당성 검사
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stSession 현재 trip_id 세션
 * @param[in] stMatchLinkInfo 이번 틱 맵매칭 결과
 * @return true(비현실적 — SKIP 처리 필요), false(정상)
 * @remark
 *   ShouldSkipImplausibleSpeed()가 SPEED_KMH 배율(speed_factor/speed_margin, 노이즈 허용폭 넓음)로
 *   raw GPS 튐을 잡는 것과 달리, 이 함수는 Continue(위상 그래프 연속매칭)가 직전 확정 링크와의
 *   연결을 못 찾아 Begin(반경 최근접) 폴백으로 떨어진 결과에 한해(stMatchLinkInfo.bContinueFallback)
 *   "직전 확정 위치 → 신규 매칭 위치" 거리를 raw GPS 이동거리 기준 배율(MM_PATH_PLAUSIBLE_SCALE)+
 *   바닥값(MM_PATH_PLAUSIBLE_FLOOR_M)과 비교한다 — FindLinkPathBounded 재구성 경로 타당성 검사
 *   (RawLogWorker.cpp 상단 MM_PATH_PLAUSIBLE_SCALE 주석 참고)와 동일 기준선을, 위상 연결이 아예
 *   끊긴 단일 틱 전이에도 적용한 것. (실측 000376_20260826150010 seq404→405: 강릉 주정차구역
 *   이탈 직후 매칭점간 44m/3초 점프, 직전 링크와 공유 노드 없음·최단 우회조차 15m 브릿지 기준
 *   초과 — Continue는 정상적으로 거부했으나 Begin 폴백이 연결성 검증 없이 그대로 채택했다)
 *   (2026-09-04 최정우 추가)
*/
bool CRawLogWorker::IsFallbackJumpImplausible(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, const MATCH_LINK_INFO& stMatchLinkInfo)
{
	if (!stMatchLinkInfo.bContinueFallback)
		return false;			// Continue 정상 연결 — 대상 아님
	if (!stSession.bHasLastMatch)
		return false;			// 비교 기준(직전 매칭 위치) 없음 — START 등
	if (!stSession.bLastPointOk)
		return false;			// 직전 포인트가 매칭 실패 — 갭이 정상 1구간보다 넓어져 판단 불신 (ShouldSkipImplausibleSpeed 와 동일 가드)

	double dfGapSec = difftime(stRawLogInfo.dtGPS, stSession.dtLastMatchGps);
	if (dfGapSec <= 0.0 || dfGapSec > static_cast<double>(MM_CALC_MAX_GAP_SEC))
		return false;			// 공백·역전 구간 — 판단 불신

	POINT stPrevMatch; stPrevMatch.dfX = stSession.dfLastMatchX; stPrevMatch.dfY = stSession.dfLastMatchY;
	POINT stCurRaw;    stCurRaw.dfX = stRawLogInfo.dfX;          stCurRaw.dfY = stRawLogInfo.dfY;
	POINT stNewMatch;  stNewMatch.dfX = stMatchLinkInfo.dfMatchX; stNewMatch.dfY = stMatchLinkInfo.dfMatchY;

	// 판정 기준값(raw GPS 실이동) — 직전 확정 위치→이번 raw GPS 하버사인 거리
	double dfRawMoveM = HaversineMeters(stPrevMatch, stCurRaw);
	// 실제 검사 대상 — 직전 확정 위치→이번 신규 매칭 위치 거리(도로망 스냅 결과)
	double dfMatchJumpM = HaversineMeters(stPrevMatch, stNewMatch);

	double dfPlausibleMaxM = (dfRawMoveM * MM_PATH_PLAUSIBLE_SCALE) + MM_PATH_PLAUSIBLE_FLOOR_M;
	if (dfMatchJumpM <= dfPlausibleMaxM)
		return false;

	LOGFMTW("[#%02d] reject implausible fallback jump!device=[%s] trip_id=[%s] seq=[%u] "
		"link=[%llu] match_jump=[%.1fm] raw_move=[%.1fm] gap=[%.1fs] limit=[%.1fm]",
		nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
		static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
		dfMatchJumpM, dfRawMoveM, dfGapSec, dfPlausibleMaxM);
	return true;
}

/**
 * @brief raw GPS가 등록된 주정차구역 폴리곤 안인데 매칭 좌표는 그 밖으로 나온 경우 판정
 * @param[in] nThreadId 워커 스레드 ID(로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 맵매칭 결과(bMatched 확인 후 호출)
 * @return true(비현실적 — SKIP 판정), false(정상 또는 판단 대상 아님)
 * @remark 그래프탐색(ContinueMapMatch/BeginMapMatch)은 전혀 건드리지 않고, 이미 나온 매칭 결과를
 *   등록된 주정차구역 폴리곤과 사후 비교만 한다 — ProcessParkingCharge 규칙4("이동 중엔 매칭좌표가
 *   raw보다 신뢰할 만하다")와 정확히 대칭: 이 함수는 그 전제가 성립 안 하는 저속 구간에서 "raw가
 *   매칭보다 신뢰할 만하다"고 보고, 매칭이 raw가 속한 구역 밖으로 나가면 그 매칭을 못 믿는다.
 *   MM_ZONE_OUTSIDE_SPEED_MAX_KMH 이하 속도에서만 적용 — 이동 중 정상 도로 통과까지 오탐하지
 *   않기 위함(2026-09-04 최정우 추가, 사용자 지시)
*/
bool CRawLogWorker::IsMatchOutsideRawZonePolygon(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo)
{
	if (m_stConfig.pcChargeDataLoader == nullptr)
		return false;

	vector<PZONE_INFO> vtRawZones;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(stRawLogInfo.dfX, stRawLogInfo.dfY, 0.0, &vtRawZones);
	if (vtRawZones.empty())
		return false;			// raw 자체가 등록 구역 밖 — 대상 아님

	vector<PZONE_INFO> vtMatchZones;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, 0.0, &vtMatchZones);

	for (size_t r = 0; r < vtRawZones.size(); ++r)
	{
		for (size_t m = 0; m < vtMatchZones.size(); ++m)
		{
			if (strcmp(vtRawZones[r]->szRoadID, vtMatchZones[m]->szRoadID) == 0)
				return false;	// raw가 속한 구역에 매칭좌표도 속함 — 정상
		}
	}

	// (F-2)에서 수선의 발 거리(intersect_len)로 속도 게이트를 대체해봤으나, 판교/강릉 실측 결과
	//   SKIP 구간이 넓어지며 ResolveSkipGapNodeStep/handoff-gap 등 "SKIP 구간을 사후에 메우는"
	//   로직들이 감당 못 하고 실패 — 이미 확정(Y/0, 경로기반 거리)돼 있던 과금 레코드가 감사대상
	//   (N/3, 직선거리)으로 다운그레이드되거나(000370 실측) 아예 소실되는(000376 실측) 부작용을
	//   확인해 롤백. 국소적 매칭 개선보다 확정 과금 훼손이 더 큰 비용이라 판단 (2026-09-04
	//   최정우 확인, 사용자 지시로 검증 후 원복) — ① speed<=1.0km/h 게이트로 복귀
	if ((stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed > static_cast<float>(MM_ZONE_OUTSIDE_SPEED_MAX_KMH)))
		return false;

	LOGFMTW("[#%02d] match outside raw zone polygon!device=[%s] trip_id=[%s] seq=[%u] "
		"rawZone=[%s] link=[%llu] speed=[%.1f]km/h intersect_len=[%.1f]m -> SKIP",
		nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
		vtRawZones[0]->szRoadID, static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
		static_cast<double>(stRawLogInfo.fSpeed), stMatchLinkInfo.dfIntersectLenSgmt);
	return true;
}

/**
 * @brief Begin 맵매칭(초기 맵매칭) 필요 여부 판단
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stSession 현재 trip_id 세션
 * @param[out] pbFullReset true 이면 START 에 의한 전체 세션 초기화
 * @param[out] pbSeqRollback true 이면 GPS_SEQ 역전(과거·중복 seq) — 반환값은 false 이고
 *   호출측(ProcessRawLog)이 세션을 건드리지 않은 채 그 행만 SKIP 한다 (2026-08-23 최정우 추가)
 * @remark
 *   - TRIP_EVENT=0(START) 또는 TRIP_ID 변경 → 시작(BEGIN 강등)
 *   - GPS_SEQ<=dwLastGpsSeq(역전·동일 seq 재처리)는 더 이상 BEGIN 강등이 아니다 — pbSeqRollback 로 알린다
*/
bool CRawLogWorker::NeedsBeginReset(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, bool *pbFullReset, bool *pbSeqRollback)
{
	if ((pbFullReset == nullptr) || (pbSeqRollback == nullptr))
		return false;

	*pbFullReset = false;
	*pbSeqRollback = false;

	if (stRawLogInfo.nTripEvent == TRIP_EVENT_START)
	{
		*pbFullReset = true;
		return true;
	}

	// TRIP_ID 변경 = 새 주행 (이전 trip END 누락으로 세션 잔류 또는 START 누락) → 전체 리셋(갱신) (2026-07-08 최정우 추가)
	if ((stSession.szTripId[0] != '\0') && 
		(strcmp(stSession.szTripId, stRawLogInfo.szTripID) != 0) && 
		(stRawLogInfo.szTripID[0] != '\0'))
	{
		LOGFMTW("[#%02d] trip_id changed (missing END/START)!device=[%s] old=[%s] new=[%s] seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stSession.szTripId,
			stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
		*pbFullReset = true;
		return true;
	}

	// GPS_SEQ 역전·중복 — 이미 지나온 시점의 행이다. 세션은 건드리지 않고 신호만 올린다.
	//   (2026-08-23 최정우 수정 — 이전엔 return true 로 BEGIN 강등했으나 그게 오매칭 원인이었다.
	//    아래 ProcessRawLog 의 bSeqRollback 처리 주석 참고)
	if ((stSession.dwLastGpsSeq > 0) && 
		(stRawLogInfo.dwSeqNo <= stSession.dwLastGpsSeq))
	{
		LOGFMTW("[#%02d] gps_seq rollback!device=[%s] trip_id=[%s] seq=[%u] last_seq=[%u] -> SKIP(세션 유지)",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			stRawLogInfo.dwSeqNo, stSession.dwLastGpsSeq);
		*pbSeqRollback = true;
		return false;
	}

	return false;
}

/**
 * @brief 연속 맵매칭 세션을 시작 상태로 초기화
 * @param[in,out] stSession trip_id 세션
 * @param[in] bFullReset true 이면 START 누락 경고 플래그도 초기화
 * @return void
*/
void CRawLogWorker::ResetTripSessionForBegin(VEHICLE_TRIP_SESSION& stSession, bool bFullReset)
{
	stSession.qwLinkID = 0;
	stSession.dwLastGpsSeq = 0;

	// 시작 전환 시 직전 매칭·고도 앵커 폐기 → 끊긴/역전 구간 오계산 방지 (2026-07-08 최정우 추가)
	stSession.dfLastMatchX = 0.0;
	stSession.dfLastMatchY = 0.0;
	stSession.dtLastMatchGps = 0;
	stSession.dwLastMatchGpsSeq = 0;
	stSession.bHasLastMatch = false;
	stSession.nPrevAltitude = NO_ALTITUDE;
	stSession.nPrevRoadType = ROAD_TYPE_NORMAL;
	stSession.bHasPrevAlt = false;
	stSession.dfLastMatchLinkPos = 0.0;
	stSession.bHasPrevLinkPos = false;
	stSession.nReverseStreak = 0;
	stSession.bLastPointOk = true;

	if (bFullReset)
	{
		stSession.bStartWarned = false;
		// 실제 신규 trip 시작(START/trip_id 변경)일 때만 과금 트랙 리셋 — GPS_SEQ 역전 재처리(bFullReset=false)는
		//   같은 trip 이 계속되는 것이라 리셋하면 안 됨(이미 지난 게이트·구역 재부과 위험) (2026-08-12 최정우 추가)
		stSession.nChargeSeq = 1;
		// [2026-09-22 최정우 추가] 워터마크 큐도 트립 단위 상태다. **호출측이 직전에 전량
		//   방출했다는 전제**로 비운다(ProcessRawLog 의 ReleaseChargeQueue 참고) — 새 트립 상태를
		//   추가할 때 이 리셋을 빠뜨리면 이전 트립 값이 새 트립으로 샌다는 이 함수의 기존 주의사항이
		//   그대로 적용된다.
		stSession.vtPendingEmit.clear();
		stSession.nEmitSeq = 0;
		// 링크 커버리지 계측도 트립 단위 (2026-09-22 최정우 추가)
		stSession.vtTripPathLinks.clear();
		stSession.dfChargedNodeStepM = 0.0;
		stSession.vtCoveredAll.clear();
		stSession.dwLastEmittedStartSeq = 0;
		stSession.vtEmittedNodeStepRanges.clear();
		stSession.dtLastGpsEventTime = 0;						// 신규 trip 은 이전 trip 최댓값을 이어받으면 안 됨 (2026-08-25 최정우 추가)

		// 폐쇄형 진입 상태도 동일하게 신규 trip 시작 시에만 리셋 (2026-08-12 최정우 추가)
		stSession.bInClosedRoad = false;
		stSession.szEntryTollgateId[0] = '\0';
		stSession.szClosedRoadId[0] = '\0';
		stSession.dfClosedAccumDistM = 0.0;						// (2026-08-25 최정우 추가)
		stSession.qwClosedLastZoneLinkID = 0;						// (2026-08-25 최정우 추가)
		stSession.dtClosedLastZoneTime = 0;						// (2026-08-25 최정우 추가)
		stSession.bClosedEntryAmbiguous = false;					// (2026-08-25 최정우 추가)
		// [버그 수정, 2026-09-21 최정우] 게이트 확정 이탈 상태(이탈 디바운스 스트릭 + 재진입
		//   차단용 링크/구역 기억)는 **트립 단위 상태**인데 여기서 리셋이 빠져 있었다. 직전 트립이
		//   진출게이트를 지난 그 링크 위에서 끝나고 같은 차량의 다음 트립이 같은 링크에서 시작하면,
		//   진입 루프(ProcessClosedRoadCharge)의 재진입 차단 분기가 **새 트립의 정상 진입까지**
		//   막아 폐쇄형 과금 1건이 통째로 누락된다. 함수 선두의 "링크가 바뀌면 해제" 로직은
		//   링크가 같으면 발동하지 않으므로 이 경로를 못 막는다 — 세션 키가 DEVICE_KEY 라
		//   트립이 바뀌어도 세션 자체는 그대로 살아있다는 점이 핵심
		stSession.nClosedExitTicks = 0;
		stSession.qwClosedExitedLinkID = 0;
		stSession.szClosedExitedRoadId[0] = '\0';

		// 구간단속 진입 상태도 동일하게 신규 trip 시작 시에만 리셋 (2026-08-12 최정우 추가)
		stSession.bInSpeedZone = false;
		stSession.szSpeedZoneRoadId[0] = '\0';
		stSession.szSpeedEntryTollgateId[0] = '\0';
		stSession.dfSpeedAccumDistM = 0.0;						// (2026-08-25 최정우 추가)
		stSession.qwSpeedLastZoneLinkID = 0;						// (2026-08-25 최정우 추가)
		stSession.dtSpeedLastZoneTime = 0;						// (2026-08-25 최정우 추가)
		stSession.bSpeedEntryAmbiguous = false;					// (2026-08-25 최정우 추가)
		// [버그 수정, 2026-09-21 최정우] 위 폐쇄형(nClosedExitTicks 블록)과 동일 사유 — 구간단속판
		stSession.nSpeedExitTicks = 0;
		stSession.qwSpeedExitedLinkID = 0;
		stSession.szSpeedExitedRoadId[0] = '\0';

		// 면제도로·일반도로 진행 세션도 신규 trip 시작 시에만 리셋 (2026-08-23 최정우 수정 — 벡터화)
		stSession.vtExemptRuns.clear();
		stSession.vtNodeStepRuns.clear();
		stSession.vtOpenRuns.clear();							// (2026-08-25 최정우 추가)

		// [버그 수정, 2026-09-11 최정우] 주정차(PARKING) 진행 세션·후보는 위 3종과 동일하게 trip
		//   스코프인데 여기서만 빠져 있었다 — TRIP_ID 변경(이전 END 누락) 시 옛 trip 에서 열려있던
		//   vtParkRuns 가 새 trip 세션으로 그대로 이월되고, 나중에 그 run 이 마감될 때 BuildParkRow()
		//   가 새 TRIP_ID + 옛 trip 의 GPS_SEQ 조합으로 START_GPS_SEQ 를 기록해 PK 정합성이
		//   깨졌다(PRIM_RAWGPS PK 는 TRIP_ID+GPS_SEQ). NODE_STEP 접촉/병합 이월 상태도 같은 이유로
		//   같이 리셋 — 안 하면 옛 trip 의 이월값(위치·시각·링크ID)이 새 trip 첫 tick 과 잘못
		//   이어붙는다.
		stSession.vtParkRuns.clear();
		stSession.vtParkCands.clear();
		stSession.dwParkTripFirstSeq = 0;				// (2026-10-04 최정우 추가 — E19)
		stSession.dtParkTripFirstTime = 0;
		stSession.bParkAccOkSeen = false;
		stSession.dwParkLastAccOkSeq = 0;
		stSession.bParkHeadNear = true;
		stSession.bHasParkTouchCarry = false;
		stSession.bHasMergeCarry = false;
		stSession.dwMergeCarrySeq = 0;
		stSession.bHasHeldNodeStepRun = false;
		stSession.bHasHeldSpeedMirrorRun = false;
		// [버그 수정, 2026-09-15 최정우] 게이트 진출 이월도 같이 리셋 — 바로 위 이월값들과 완전히
		//   같은 근거인데 이것만 빠져 있었다. 안 지우면 옛 trip 의 게이트 진출 지점(시각·좌표·
		//   GPS_SEQ)이 새 trip 의 첫 일반도로 run 진입정보로 그대로 소비돼, **새 trip 의 행에
		//   옛 trip 의 occur_dt·start_gps_seq 가 박힌 잡종 레코드**가 만들어진다 — 실측 합성
		//   시나리오(동일 차량 연속 2운행)에서 새 trip 행에 occur_dt=이전운행시각,
		//   start_gps_seq=이전운행 마지막 순번(41)이 찍히는 걸 확인했다.
		stSession.bHasGateExitCarry = false;
		stSession.bGateExitAtTick = false;
		// [버그 수정, 2026-09-15 최정우, 소스 재검토 지적] SKIP 갭 흡수용 이월 3필드도 같이 리셋 —
		//   GPS_SEQ 는 트립마다 1부터 다시 시작하므로, 안 지우면 이전 트립의 (순번, run entry, 거리)가
		//   새 트립의 같은 순번 tick 에서 가드를 통과해 **엉뚱한 run 의 누적거리를 깎는다**
		//   (음수 클램프에 걸리면 그 run 이 쌓은 거리 전체가 소실). 위 게이트 진출 이월과 동일 부류.
		stSession.dfSkipGapTickDistM = 0.0;
		stSession.dwSkipGapTickGpsSeq = 0;
		stSession.dwSkipGapTickRunEntry = 0;
		stSession.dwSkipInParkSeq = 0;				// (2026-10-04 최정우 추가 — E18)
		stSession.dwSkipParkGapCutSeq = 0;
		// [버그 수정, 2026-09-15 최정우, 소스 재검토 지적] 주정차 접촉(park-touch) 상태기계 전체도
		//   리셋한다 — 2026-09-11 에 bHasParkTouchCarry 만 지우고 **그 carry 를 만들어내는 상태**는
		//   그대로 뒀다. 옛 trip 이 폴리곤 접촉 중 끝나면 그 앵커(링크·좌표·시각·순번)가 새 trip 으로
		//   넘어가, 새 trip 첫 tick 이 폴리곤 밖이어도 디바운스만 돌다가 이탈 확정 시점에 **옛 trip 의
		//   좌표·GPS_SEQ 로 행이 만들어진다**(새 trip_id + 옛 순번 = PRIM_RAWGPS PK 와 어긋나는
		//   잡종 레코드). 위 게이트 진출 이월·SKIP 갭 이월과 완전히 같은 부류.
		stSession.bNodeStepParkTouch = false;
		stSession.nNodeStepParkExitTicks = 0;
		stSession.bParkTouchEverMatchedInside = false;
		stSession.bParkTouchHasFirstOut = false;
		stSession.qwParkTouchLastInLinkID = 0;
		stSession.dfParkTouchAccumAtLastIn = 0.0;		// (2026-10-04 최정우 추가)
		stSession.dfParkTouchLastInX = 0.0;
		stSession.dfParkTouchLastInY = 0.0;
		stSession.dtParkTouchLastIn = 0;
		stSession.dwParkTouchLastInGpsSeq = 0;
		stSession.dfParkTouchFirstOutX = 0.0;
		stSession.dfParkTouchFirstOutY = 0.0;
		stSession.dtParkTouchFirstOut = 0;
		stSession.szParkTouchZoneRoadId[0] = '\0';
		// [버그 수정, 2026-09-15 최정우, 소스 재검토 지적] "이 trip 의 마지막 확정 링크" 앵커도 리셋.
		//   맵매칭 앵커(qwLinkID/dfLastMatchX·Y/bHasLastMatch/dtLastMatchGps)는 이 함수 앞부분에서
		//   이미 모든 리셋 경로에서 지워지는데 이 4개만 빠져 있었다. qwLastConfirmedLinkID 는
		//   "이 trip 의 첫 확정매칭인가"(==0) 센티널로 쓰여서, 옛 trip 값이 남으면 새 trip 이
		//   폐쇄형/구간단속 구역 **안에서 시작**해도 bAmbiguousStart 가 안 붙는다 → "정상 게이트
		//   통과"로 처리돼 dist_m 에 실측이 아닌 **구역 등록 전체길이**가 들어가는 과다청구가 된다.
		stSession.qwLastConfirmedLinkID = 0;
		stSession.dtLastConfirmedLinkTime = 0;
		stSession.dwLastConfirmedLinkGpsSeq = 0;
		stSession.fLastConfirmedLinkSpeed = 0.0f;
	}
}

/**
 * @brief TRIP_EVENT=END 신뢰 여부 판정(스퓨리어스 순서역전 검사) (2026-08-25 최정우 추가)
 * @remark gps_dt 가 이 세션에서 지금까지 확인된 최대 gps_dt(dtLastGpsEventTime)보다 과거면
 *   신뢰하지 않는다 — 단말이 보낸 순서역전·중복 도착 이벤트로 추정(실측 근거·오판 시 안전장치는
 *   [[project_duplicate_trip_end_event_speed_charge]] 참고). ProcessRawLog() 의 "현재 틱" 판정과
 *   CommitPendingRow() 의 "보류 중이던 옛 틱을 나중에 flush 할 때" 판정에 공용으로 쓴다 — 후자를
 *   빠뜨리면, 스퓨리어스 END 로 판정돼 보류 상태로 남아있던 행이 나중에 다른 이유로 flush 될 때
 *   그 안에서 또 nTripEvent==END 를 그대로 믿어버려 원래 막으려던 버그가 재발한다(2026-08-25
 *   OPEN/EXEMPT/NODE_STEP/PARKING 에 이 판정을 배선하던 중 발견). dtLastGpsEventTime 갱신(상태
 *   변경)은 이 함수가 아니라 ProcessRawLog() 가 매 틱 1회만 수행 — 이 함수는 순수 판정만 한다.
*/
bool CRawLogWorker::IsTrustedTripEnd(const sRawLogInfo& stRawLogInfo, const VEHICLE_TRIP_SESSION& stSession)
{
	if (stRawLogInfo.nTripEvent != TRIP_EVENT_END)
		return false;
	if ((stSession.dtLastGpsEventTime > 0) && (stRawLogInfo.dtGPS < stSession.dtLastGpsEventTime))
		return false;
	return true;
}

/**
 * @brief ThreadPool Runnable – trip_id batch 1건 처리
 * @param[in] nThreadId 워커 스레드 ID (세션 맵 인덱스)
 * @param[in] context RAW_LOG_BATCH 포인터 (동일 trip_id GPS 묶음)
 * @return void
 * @remark
 *   - 세션은 배치 임시(stWorkSession)로 맵매칭 후 bulk 성공 시에만 m_vtTripSessions 에 커밋
 *   - #7: 조기 종료 시 ReleaseReservedBatch() 로 PROCESSING 해제
 *   - [2026-09-15] 배치 루프 직후, 신뢰 END tick 보다 **뒤의 tick 이 실제로 있었던** 경우에만
 *     열린 과금 구간을 비정상 종료로 마감한다(FlushOpenRunsAsAbnormalEnd). END 가 스트림
 *     중간에 찍힌 트립의 잔여 구간이 세션 erase 와 함께 사라지던 것을 막는다 — 세션 erase
 *     지점은 트랜잭션 뒤라 거기서 만들면 이번 배치 INSERT 에 못 실린다.
*/
void CRawLogWorker::run(int nThreadId, void *context)
{
	RAW_LOG_BATCH *pvtBatch = reinterpret_cast<RAW_LOG_BATCH *>(context);
	if ((pvtBatch == nullptr) || (pvtBatch->empty()))
		return;

	PGconn *pcConn = nullptr;

	if (m_stConfig.pcPostgrePool == nullptr)
	{
		LOGFMTE("[#%02d] db pool is null!batch orphan until recover!device=[%s] count=[%d]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, static_cast<int>(pvtBatch->size()));
		return;
	}

	if (m_stConfig.strUpdateSQL.empty())
	{
		LOGFMTE("[#%02d] update sql is empty!batch orphan until recover!device=[%s] count=[%d]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, static_cast<int>(pvtBatch->size()));
		return;
	}

	// batch 처리용 DB 커넥션 획득 (#E-1: [database] retrymax/wait 재시도) (2026-07-10 최정우 추가)
	pcConn = AcquirePoolConnection(m_stConfig.pcPostgrePool,
		m_stConfig.nConnRetryMax, m_stConfig.nConnRetryWait);
	if (pcConn == nullptr)
	{
		LOGFMTE("[#%02d] db connection is null after retry!batch orphan until recover!device=[%s] count=[%d]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, static_cast<int>(pvtBatch->size()));
		return;
	}

	if ((nThreadId < 0) || (nThreadId >= static_cast<int>(m_vtTripSessions.size())))
	{
		LOGFMTE("[#%02d] session index out of range!", nThreadId);
		// 세션 인덱스 오류 시 batch 예약 release (2026-07-08 최정우 주석 추가)
		ReleaseReservedBatch(pcConn, *pvtBatch, nThreadId);
		// DB 커넥션 반환 (2026-07-08 최정우 주석 추가)
		ReleaseConnection(pcConn);
		return;
	}

	LOGFMTD("[#%02d] batch start!device=[%s] trip_id=[%s] count=[%d]",
		nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
		static_cast<int>(pvtBatch->size()));

	vector<RAW_LOG_UPDATE_ROW> vtUpdates;
	vtUpdates.reserve(pvtBatch->size());
	vector<CHARGE_INSERT_ROW> vtChargeInserts;					// 개방형 게이트 통과 — 배치 종료 시 일괄 INSERT (2026-08-12 최정우 추가)
	vector<TRIP_END_UPDATE_ROW> vtTripEndUpdates;				// 트립 종료 — 배치 종료 시 trip_end_dt UPDATE (2026-08-12 최정우 추가)

	// bulk 성공 전까지 m_vtTripSessions 미갱신: 커밋된 세션을 복사해 배치 임시 세션으로 사용
	// 세션 맵 키 = DEVICE_KEY (2026-07-08 최정우 수정): device 당 1세션 → END/START 누락 고아 세션 누적 방지.
	//   신규 TRIP_ID 는 ProcessRawLog/NeedsBeginReset 의 trip_id 변경 감지로 세션을 리셋(갱신)한다.
	unordered_map<string, VEHICLE_TRIP_SESSION>& mapSessions =
		m_vtTripSessions[static_cast<size_t>(nThreadId)];
	const string strDeviceKey = (*pvtBatch)[0].szDeviceKey;

	VEHICLE_TRIP_SESSION stWorkSession;
	unordered_map<string, VEHICLE_TRIP_SESSION>::iterator itSession = mapSessions.find(strDeviceKey);
	if (itSession != mapSessions.end())
		stWorkSession = itSession->second;

	// [2026-10-04 최정우 추가, 사용자 확정 — trip_seq 이어 매기기] 이 배치의 트립이 세션의 트립과 다르면
	//   (새 트립·엔진 재기동·TTL 마감 뒤 같은 trip_id 재개) 그 트립에 이미 등록된 TRIP_SEQ 최댓값을 읽어
	//   둔다. ProcessRawLog() 가 이 트립을 세션에 세팅할 때 nEmitSeq 를 이 값으로 맞춰 다음 번호부터 매긴다
	//   (QueryTripSeqMax() 주석 참고). 새 트립이면 0 이라 영향이 없다. 조회 실패(-1)면 종전대로 0 부터.
	if (strcmp(stWorkSession.szTripId, (*pvtBatch)[0].szTripID) != 0)
	{
		const int nSeqMax = QueryTripSeqMax(pcConn, (*pvtBatch)[0].szTripID, strDeviceKey);
		strncpy(stWorkSession.szSeedTripId, (*pvtBatch)[0].szTripID, sizeof(stWorkSession.szSeedTripId) - 1);
		stWorkSession.szSeedTripId[sizeof(stWorkSession.szSeedTripId) - 1] = '\0';
		stWorkSession.nSeedEmitSeq = (nSeqMax > 0) ? nSeqMax : 0;
	}

	bool bTripEnded = false;
	bool bProcessOk = true;
	// 신뢰 트립종료(TRIP_EVENT=2)가 찍힌 tick 의 GPS_SEQ — 그 **뒤로도** tick 이 더 들어왔는지
	//   판별하는 데만 쓴다. 0 이면 이번 배치에 트립종료가 없었다.
	//   END 가 여러 번 찍히는 트립(README K-3)에서는 **첫** END 를 기준으로 잡는다 — 중간 END
	//   이후 구간이 곧 마감 대상이고, 마지막 END 를 기준으로 잡으면 바로 그 구간을 놓친다.
	//   (2026-09-15 최정우 추가)
	uint32 dwTripEndSeq = 0;
	for (size_t i=0; i<pvtBatch->size(); ++i)
	{
		const bool bEndedBefore = bTripEnded;
		// GPS 1건 검증·맵매칭·UPDATE 행 적재 (2026-07-08 최정우 주석 추가)
		if (!ProcessRawLog(nThreadId, (*pvtBatch)[i], &vtUpdates, &vtChargeInserts, &vtTripEndUpdates, &stWorkSession, &bTripEnded))
			bProcessOk = false;
		if (!bEndedBefore && bTripEnded)
			dwTripEndSeq = (*pvtBatch)[i].dwSeqNo;
	}

	// vtUpdates 에 없는 예약 행 release (AppendUpdateRow 실패 등 #4 배치 내 orphan)
	vector<RAW_LOG_UPDATE_ROW> vtOrphanRelease;
	for (size_t i=0; i<pvtBatch->size(); ++i)
	{
		char szGpsSeq[16];
		snprintf(szGpsSeq, sizeof(szGpsSeq), "%u", (*pvtBatch)[i].dwSeqNo);

		if ((*pvtBatch)[i].szTripID[0] == '\0')
		{
			LOGFMTE("[#%02d] orphan release skipped!invalid trip_id device=[%s] seq=[%u]",
				nThreadId, (*pvtBatch)[i].szDeviceKey, (*pvtBatch)[i].dwSeqNo);
			bProcessOk = false;
			continue;
		}

		// 1틱 지연커밋으로 세션에 정당하게 보류(pending) 중인 행은 orphan 이 아님 — vtUpdates 에는
		//   아직 없지만(다음 배치에서 확정) 유실된 게 아니므로 release 대상에서 제외해야 한다.
		//   그렇지 않으면 이 행이 PENDING(0)으로 되돌아가 나중에 새 행처럼 재조회되면서, 메모리에
		//   남아있는 보류 버퍼와 겹쳐 같은 GPS 를 두 번 처리하는 버그가 생김 (2026-08-21 최정우 추가)
		const bool bIsSessionPending = stWorkSession.bHasPendingCommit
			&& (strcmp(stWorkSession.stPendingRawLogInfo.szTripID, (*pvtBatch)[i].szTripID) == 0)
			&& (stWorkSession.stPendingRawLogInfo.dwSeqNo == (*pvtBatch)[i].dwSeqNo);

		if (!IsRowInUpdates(vtUpdates, (*pvtBatch)[i].szTripID, szGpsSeq) && !bIsSessionPending)
		{
			// vtUpdates 미포함 orphan 행 release 목록 적재 (2026-07-08 최정우 주석 추가)
			AppendReleaseRowFromRawLog(&vtOrphanRelease, (*pvtBatch)[i]);
		}
	}

	if (!vtOrphanRelease.empty())
	{
		// orphan 예약 행 PROCESSING→PENDING release (2026-07-08 최정우 주석 추가)
		if (!BulkReleaseRawLogs(pcConn, vtOrphanRelease))
		{
			LOGFMTE("[#%02d] orphan release failed!device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
				static_cast<int>(vtOrphanRelease.size()));
			bProcessOk = false;
		}
		else
		{
			LOGFMTW("[#%02d] orphan released!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
				static_cast<int>(vtOrphanRelease.size()));
		}
	}

	// [버그 수정, 2026-09-15 최정우] **트립종료 tick 이후에 더 들어온 tick 의 과금 구간을 마감**한다.
	//   bTripEnded 이면 아래에서 mapSessions.erase(strDeviceKey) 로 세션을 통째로 버리는데, END 가
	//   스트림 중간에 찍힌 트립에서는 그 뒤로도 tick 이 들어와 stWorkSession 에 새 run 이 쌓인다.
	//   종전에는 그 run 들이 **과금 행 하나 없이 erase 와 함께 사라졌다**.
	//   위치가 중요하다 — 배치 루프 직후·트랜잭션 직전이라 생성된 행이 이번 배치의 병합과 INSERT 를
	//   그대로 탄다(erase 지점은 트랜잭션 뒤라 늦는다).
	//   [중요] **트립종료 tick 보다 뒤의 tick 이 실제로 있었을 때만** 마감한다. 정상 트립은 END 가
	//   마지막 tick 인데 그 tick 자신도 맵매칭·과금 판정을 거치므로, 트립종료 블록이
	//   bInClosedRoad/bInSpeedZone 을 내린 **직후 같은 tick 이 구역을 다시 진입**시켜 상태를 켜 놓는다.
	//   이 조건이 없으면 그 재진입 상태를 또 마감해 **정상 트립에 중복 과금 행**이 생긴다
	//   (실측 000376_20260819140532: 폐쇄형 RL-Z00005 가 seq58~66 과 seq67~67 두 행으로 중복 적재).
	if (bTripEnded && (dwTripEndSeq > 0) && (stWorkSession.dwLastGpsSeq > dwTripEndSeq))
	{
		const size_t nBeforeEndFlush = vtChargeInserts.size();
		FlushOpenRunsAsAbnormalEnd(nThreadId, strDeviceKey, stWorkSession,
			stWorkSession.dtLastGpsEventTime, stWorkSession.dwLastGpsSeq, time(nullptr),
			&vtChargeInserts, &vtTripEndUpdates);
		const size_t nEndFlushed = vtChargeInserts.size() - nBeforeEndFlush;
		if (nEndFlushed > 0)
		{
			LOGFMTW("[#%02d] post-trip-end open runs flushed!device=[%s] trip_id=[%s] "
				"rows=[%d] end_seq=[%u] last_seq=[%u]",
				nThreadId, strDeviceKey.c_str(), stWorkSession.szTripId,
				static_cast<int>(nEndFlushed), dwTripEndSeq, stWorkSession.dwLastGpsSeq);
		}
	}

	// [버그 수정, 2026-09-16 최정우] **세션을 버리기 전에 1틱 지연커밋 보류행을 확정**한다.
	//   아래에서 bTripEnded 면 mapSessions.erase() 로 세션을 통째로 버리는데, 그때 보류행이 남아
	//   있으면 그 GPS 행은 **PROCESSING(2) 인 채로 DB 에 영구히 남는다**(세션이 없어져 확정해줄
	//   주체가 사라진다). stale_recover 가 stale_sec 마다 PENDING 으로 되돌리지만, 그 트립은 이미
	//   끝나 "다음 tick" 이 영영 오지 않으므로 재처리해도 같은 자리에 다시 보류돼 **주기적으로
	//   무한 반복**한다.
	//   정상 트립은 END 가 마지막 tick 이라 그 tick 처리 중에 보류가 확정돼 여기 걸리지 않는다.
	//   문제가 되는 건 **END 가 스트림 중간에 찍힌 트립이 배치 경계에서 잘릴 때**다 — 실측
	//   000370_20260824140015(END 가 seq6, 이후 35 tick 더 수신): 41 tick 이 14+27 로 쪼개져
	//   첫 배치가 seq14 를 보류한 채 끝났고, bTripEnded 로 세션이 지워지면서 seq14 가 PROCESSING
	//   으로 남았다(두 번째 배치의 seq15 가 Begin 모드로 시작한 것이 세션 소실의 증거).
	//   위치는 이 자리여야 한다 — 배치 루프 직후·트랜잭션 직전이라 여기서 만든 UPDATE 가 이번
	//   배치의 bulk 반영을 그대로 탄다(erase 지점은 트랜잭션 뒤라 늦다). ExpireTtlSessions() 가
	//   세션 소멸 전에 CommitPendingRow() 를 부르는 것과 같은 이유·같은 패턴이다.
	if (bTripEnded && stWorkSession.bHasPendingCommit)
	{
		const size_t nBeforePendingCommit = vtUpdates.size();
		CommitPendingRow(nThreadId, &stWorkSession, false, 0, &vtUpdates, &vtChargeInserts);
		if (vtUpdates.size() > nBeforePendingCommit)
		{
			LOGFMTW("[#%02d] pending row committed before session drop!device=[%s] trip_id=[%s] "
				"end_seq=[%u]",
				nThreadId, strDeviceKey.c_str(), stWorkSession.szTripId, dwTripEndSeq);
		}
	}

	// 1틱 지연커밋 도입으로 이번 배치의 모든 행이 보류(pending)됐다면 vtUpdates 가 비어있을 수
	//   있음 — 그 경우 bulk UPDATE 자체는 할 게 없어 자동 성공 취급하고, 아래 세션 커밋(보류
	//   버퍼 포함)은 그대로 진행해야 다음 배치에서 이어서 확정(commit)된다 (2026-08-21 최정우 추가)
	// ── 맵매칭 결과 UPDATE + 과금 INSERT 를 한 트랜잭션으로 (#10, 2026-08-29 최정우 추가) ──
	//   이전에는 autocommit 단일 문장이라 rawgps_update 가 먼저 커밋된 뒤 과금 INSERT 가 실패하면,
	//   보상 release 의 WHERE MATCH_STATUS=2 조건이 이미 ①에서 소진돼 affected=0 으로 실패했다.
	//   그 결과 과금은 유실되고 매칭 결과만 남았으며, 세션도 미커밋이라 해당 구간 과금이 조용히
	//   비었다(2026-08-29 실측: prim_chargehand 컬럼 누락으로 expected=[116] affected=[0]).
	//   이제 실패 시 ROLLBACK 으로 MATCH_STATUS 를 2 로 되돌린 뒤 release 하므로 조건이 다시
	//   성립해 PENDING(0)으로 재큐잉되고, 재기동 없이 다음 poll 에서 정상 재처리된다.
	//   trip_end_dt 는 금액 무관 참고 컬럼이라 종전대로 트랜잭션 밖 best-effort 로 둔다.
	//   ReleaseConnection() 의 PQTRANS_INERROR 롤백 가드가 이 도입을 대비해 이미 들어와 있다.
	auto fnTxn = [pcConn](const char *pszCmd) -> bool
	{
		PGresult *pcTxnResult = PQexec(pcConn, pszCmd);
		const bool bCmdOk = (pcTxnResult != nullptr)
			&& (PQresultStatus(pcTxnResult) == PGRES_COMMAND_OK);
		if (pcTxnResult != nullptr)
			PQclear(pcTxnResult);
		return bCmdOk;
	};

	// 연속된 일반도로 행 병합 + 거리·체류·평균속도 재계산 — INSERT 직전 단일 병목에서
	//   일괄 처리한다 (2026-09-06 최정우 추가, 사용자 지시)
	// 매칭 tick 이 없는 일반도로 행 제거 — **병합보다 먼저** 돌린다. 나중에 돌리면 그런 구간이
	//   이미 정상 구간에 흡수돼 걸러낼 수 없다 (2026-09-07 최정우 추가, 사용자 지시)
	DropNodeStepRowsWithoutMatch(&vtChargeInserts, vtUpdates);
	MergeAdjacentNodeStepRows(&vtChargeInserts);

	// ── 과금 행 워터마크 큐 (2026-09-22 최정우 추가 — 사용자 확정 요구) ─────────────────────
	//   TRIP_SEQ 를 **적재 시점에 최종 확정**한다(등록 후 변경 금지). 이번 배치가 만든 행을 세션
	//   큐에 넣고, GPS_SEQ 순서가 확정된 것만 꺼내 번호를 부여해 INSERT 대상으로 되돌린다.
	//   트립이 끝났으면(bTripEnded) 더 생길 행이 없으므로 전량 방출한다.
	//   ※ 세션(stWorkSession)은 배치 성공 후에만 mapSessions 에 커밋되므로, 배치가 실패하면 큐와
	//     nEmitSeq 도 함께 롤백되어 재처리 시 동일한 결과가 나온다(기존 vtOpenRuns 등과 같은 성질).
	//   ※ 위 Drop/Merge 후처리는 **큐에 넣기 전에** 끝낸다 — 큐에 남는 행은 이미 그 검사를 통과한
	//     상태이고, 다음 배치의 vtUpdates 로 다시 검사하면 무관한 기준으로 지워질 수 있다.
	EnqueueChargeRows(&stWorkSession, &vtChargeInserts);
	{
		const uint32 dwBatchLastSeq = pvtBatch->empty() ? 0 : pvtBatch->back().dwSeqNo;
		const uint32 dwWatermark = bTripEnded ? UINT32_MAX
			: CalcChargeWatermark(stWorkSession, dwBatchLastSeq);
		ReleaseChargeQueue(nThreadId, &stWorkSession, dwWatermark, &vtChargeInserts);
	}

	const bool bNeedTxn = (!vtUpdates.empty() || !vtChargeInserts.empty() || !vtTripEndUpdates.empty());
	bool bTxnOpen = false;
	if (bNeedTxn)
	{
		if (fnTxn("BEGIN"))
		{
			bTxnOpen = true;
		}
		else
		{
			LOGFMTE("[#%02d] batch txn begin failed!device=[%s] trip_id=[%s]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID);
			bProcessOk = false;
		}
	}

	bool bUpdateOk = true;
	bool bChargeOk = true;
	if (bTxnOpen)
	{
		// reserve(rawgps_select) 의 짝: 완료는 rawgps_update(1/3/4), 실패 시 release(0) 동일 SQL
		// 맵매칭 결과 bulk UPDATE (2026-07-08 최정우 주석 추가)
		if (!vtUpdates.empty() && !BulkUpdateRawLogs(pcConn, vtUpdates))
		{
			LOGFMTE("[#%02d] bulk update failed!device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
				static_cast<int>(vtUpdates.size()));
			bUpdateOk = false;
		}

		// 과금 bulk INSERT(개방형·폐쇄형·구간단속·주정차·면제도로·일반도로 공용) — rawgps_update
		//   성공 후에만 시도. 실패 시 map-match bulk update 실패와 동일하게 취급(배치 release·
		//   세션 미커밋) → 다음 poll 에서 재처리되며 세션의 진행 중 트랙(vtOpenRuns 등)도 커밋되지
		//   않아 재통과 시 정상 재부과됨 (2026-08-12 최정우 추가)
		if (bUpdateOk && !vtChargeInserts.empty())
		{
			bChargeOk = BulkInsertCharges(pcConn, vtChargeInserts);
			if (!bChargeOk)
			{
				LOGFMTE("[#%02d] charge bulk insert failed!device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtChargeInserts.size()));
			}
		}

		// [2026-10-04 최정우 수정 — 사용자 위임 결정] 트립 종료 TRIP_END_DT/UPD_DT 갱신을 **같은 트랜잭션**
		//   안으로 옮겼다. 종전에는 COMMIT 뒤 best-effort 라 실패하면 로그만 남고, 세션은 이미 지워져
		//   TRIP_END_DT 가 영구히 NULL 인 트립이 생길 수 있었다. 과금서버는 PRIM_CHARGEHAND 를
		//   CHARGE_STATUS(0 PENDING -> 1 PROCESSING -> 2 CHARGED) 로 소비하므로, 트립 확정 표시
		//   (TRIP_END_DT)가 과금 행과 함께 원자적으로 들어가야 한다. 실패하면 배치 전체를 롤백·반납해
		//   다음 poll 에서 재처리한다(맵매칭·과금 INSERT 실패와 같은 취급).
		// 되돌리는 법: 이 블록을 지우고 아래 성공 분기의 종전 UpdateTripEndDt() 호출을 복원.
		if (bUpdateOk && bChargeOk && !vtTripEndUpdates.empty())
		{
			if (!UpdateTripEndDt(pcConn, vtTripEndUpdates))
			{
				LOGFMTE("[#%02d] trip_end update failed (rolled back)!device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtTripEndUpdates.size()));
				bChargeOk = false;
			}
		}

		if (bUpdateOk && bChargeOk)
		{
			// 커밋 실패도 "DB 미반영" 이므로 롤백 후 release 경로로 보낸다
			if (!fnTxn("COMMIT"))
			{
				LOGFMTE("[#%02d] batch txn commit failed!device=[%s] trip_id=[%s]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID);
				bUpdateOk = false;
				fnTxn("ROLLBACK");
			}
		}
		else if (!fnTxn("ROLLBACK"))
		{
			// 롤백 실패 시 MATCH_STATUS 가 2 로 복원되지 않아 아래 release 도 affected=0 이 된다.
			//   이 경우는 기동 시 rawgps_recover 가 회수한다 (2026-08-29 최정우 추가)
			LOGFMTE("[#%02d] batch txn rollback failed!device=[%s] trip_id=[%s]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID);
		}
	}

	// bNeedTxn 이 false 면 반영할 게 없어 성공 취급 — 1틱 지연커밋으로 이번 배치 전 행이 보류(pending)
	//   됐을 때가 이 경우이며, 세션 커밋은 그대로 진행해야 다음 배치에서 확정된다 (2026-08-21 최정우 추가)
	const bool bDbOk = bTxnOpen ? (bUpdateOk && bChargeOk) : (!bNeedTxn);
	if (!bDbOk)
	{
		bProcessOk = false;

		// PROCESSING 좀비 방지: ROLLBACK 으로 2 로 되돌아온 예약 행을 PENDING(0)으로 반납
		//   match_status=0, INTERSECT_LEN/MATCH_* '' → 기존 컬럼 유지 (2026-07-08 최정우 주석 추가)
		if (!vtUpdates.empty())
		{
			if (!BulkReleaseRawLogs(pcConn, vtUpdates))
			{
				LOGFMTE("[#%02d] bulk release failed!device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtUpdates.size()));
			}
			else
			{
				LOGFMTW("[#%02d] bulk release ok!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtUpdates.size()));
			}
		}
		// stWorkSession 폐기 — 커밋된 세션(mapSessions) 유지
	}
	else
	{
		// 트립 종료 trip_end_dt UPDATE 는 2026-10-04 부터 위 배치 트랜잭션 안에서 처리한다(종전: 여기서
		//   COMMIT 뒤 best-effort, 2026-08-12 최정우 추가).
		// 정상 종료 후 TRIP_SEQ 사후 재부여(UpdateTripSeqOrder) 호출 삭제 (2026-10-04 최정우 삭제 — 사용자 지시: trip_seq 는 SQL 로 처리하지 않고 엔진이 확정할 때 등록한다)

		// [버그 수정, 2026-09-10 최정우] 소급 재기록용 버퍼(vtOppStreakUpdateIdx 등 6종)가
		//   "이번 배치의 vtUpdates 안 인덱스"를 세션(배치 간 영속)에 저장해두는데, vtUpdates 는
		//   run() 호출마다(=배치마다) 새로 만들어지고 끝나면 파괴된다. 스트릭/런이 이번 배치
		//   안에서 못 끝나고 다음 배치로 넘어가면, 남아있는 인덱스가 다음 배치의 새 vtUpdates
		//   에서는 완전히 무관한 행을 가리키게 되어 그 행의 MATCH_STATUS/좌표를 잘못 덮어쓴다
		//   (실측 대신 최소 재현으로 확인 — 무관한 행이 SKIP으로 오염됨). 이미 배치 안에서
		//   해소된 경우는 각 소비 지점이 스스로 비워두므로 여기선 원래 비어있어 영향 없고,
		//   못 끝난 채로 배치 경계를 넘으려는 경우만 여기서 걸러진다 — 그 배치·다음 배치 각각의
		//   범위 안에서 새로 발생하는 스트릭/런은 정상적으로 계속 잡힌다(기능 자체를 끄는 게
		//   아니라 배치 경계를 넘어가는 예외적인 경우만 안전하게 포기).
		stWorkSession.qwOppStreakAnchorLinkID = 0;
		stWorkSession.vtOppStreakUpdateIdx.clear();
		stWorkSession.qwAmbigReverseRunLinkID = 0;
		stWorkSession.vtAmbigReverseRunIdx.clear();
		stWorkSession.qwClampRunLinkID = 0;
		stWorkSession.vtClampRunUpdateIdx.clear();
		stWorkSession.vtClampRunRawLogInfo.clear();
		stWorkSession.qwReverseSkipRunAnchorLinkID = 0;
		stWorkSession.vtReverseSkipRunUpdateIdx.clear();
		stWorkSession.vtReverseSkipRunRawLogInfo.clear();
		stWorkSession.qwSkipRunAnchorLinkID = 0;
		stWorkSession.vtSkipRunUpdateIdx.clear();
		stWorkSession.vtSkipRunRawLogInfo.clear();
		stWorkSession.qwStartCandLinkA = 0;
		stWorkSession.qwStartCandLinkB = 0;
		stWorkSession.vtStartCandIdxA.clear();
		stWorkSession.vtStartCandIdxB.clear();

		// DB 반영 성공 후에만 세션 커밋 (bulk 실패·release 시 연속 맵매칭 맥락 보존)
		// bTripEnded 이면 MATCHED/ERROR/SKIP 무관 trip_id 세션 제거
		if (bTripEnded)
			mapSessions.erase(strDeviceKey);					// (2026-07-08 최정우 수정) 키 = DEVICE_KEY
		else
			mapSessions[strDeviceKey] = stWorkSession;			// (2026-07-08 최정우 수정) 키 = DEVICE_KEY
	}

	if (!bProcessOk)
	{
		LOGFMTE("[#%02d] batch process failed!device=[%s] trip_id=[%s]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID);
	}

	// 자기 스레드 세션만 TTL 만료 정리 (락 불필요, 모니터 스레드 레이스 제거)
	// trip_id 세션 TTL 만료 제거 (2026-07-08 최정우 주석 추가)
	ExpireTtlSessions(nThreadId, m_stConfig.nTtlSec, pcConn);

	// batch 처리 후 DB 커넥션 반환 (2026-07-08 최정우 주석 추가)
	ReleaseConnection(pcConn);
}

/**
 * @brief 서버 종료 시 모든 워커 슬롯의 열린 과금 구간을 마감 (2026-09-16 최정우 추가)
 * @return 마감된 세션 수
 * @remark 세션(m_vtTripSessions)은 in-memory 라 프로세스가 내려가면 그대로 사라진다 —
 *   ExpireTtlSessions() 주석이 "서버 재시작 등, in-memory라 영속 안 됨 … 별도 한계로 남음"
 *   이라고 적어둔 그 한계다. GPS 행(RunRecover)·이미 적재된 과금 행(trip_seq 는 [trip_seqmax] 로 이어 매김, 2026-10-04)은 재기동 후
 *   복구되지만, **아직 DB 에 안 나간 진행 중 구간**은 복구 경로가 없어 그 트립을 통째로
 *   재매칭하지 않는 한 되살릴 수 없었다. 종료 직전에 마감해 최소한 기록으로 남긴다.
 * @warning **워커 스레드가 전부 멈춘 뒤에만 호출해야 한다.** 세션 맵은 "워커가 자기 슬롯만
 *   만진다"는 소유권 규칙으로 락 없이 운영되므로, 워커가 도는 중에 부르면 데이터 레이스다.
 *   호출부(Server::Uninitialize)는 ThreadPool::WaitForAllStopped() 가 true 를 돌려준 경우에만
 *   부른다 — 못 멈춘 워커가 있으면 건너뛴다(마감 못 한 구간은 종전대로 유실).
 * @remark 마감 자체는 ExpireTtlSessions(bForceAll=true) 에 위임한다 — 경과시간만 무시할 뿐
 *   마감 로직·사유코드(61)·DB 반영·실패 시 세션 복원까지 전부 검증된 기존 경로 그대로다.
*/
int CRawLogWorker::FlushAllSessionsOnShutdown()
{
	if (m_stConfig.pcPostgrePool == nullptr)
		return 0;

	int nTotal = 0;
	for (size_t i = 0; i < m_vtTripSessions.size(); ++i)
	{
		if (m_vtTripSessions[i].empty())
			continue;

		PGconn *pcConn = AcquirePoolConnection(m_stConfig.pcPostgrePool,
			m_stConfig.nConnRetryMax, m_stConfig.nConnRetryWait);
		if (pcConn == nullptr)
		{
			LOGFMTE("[#%02d] shutdown flush skipped!db connection is null!sessions=[%zu]",
				static_cast<int>(i), m_vtTripSessions[i].size());
			continue;
		}

		const int nFlushed = ExpireTtlSessions(static_cast<int>(i), 0, pcConn, true);
		if (nFlushed > 0)
		{
			LOGFMTW("[#%02d] open charge runs flushed on shutdown!sessions=[%d]",
				static_cast<int>(i), nFlushed);
		}
		nTotal += nFlushed;

		ReleaseConnection(pcConn);
	}

	if (nTotal > 0)
		LOGFMTW("shutdown flush done!total_sessions=[%d]", nTotal);

	return nTotal;
}

/**
 * @brief 커넥션 반환 전 미완료 트랜잭션 ROLLBACK 가드 후 pool 반환 (#14)
 * @param[in] pcConn DB 커넥션
 * @return void
 * @remark 현재 워커는 autocommit 단일 문장이라 in-트랜잭션 상태가 되지 않지만,
 *         향후 명시적 BEGIN/COMMIT(예: #10 과금 INSERT 동시 커밋) 도입 대비 방어 가드.
 *         Fetcher::ReleaseConnection 과 동일 패턴.
*/
/**
 * @brief 워커가 일감 없이 대기 상한(60초)을 넘겼을 때 그 워커 스레드에서 호출 — 자기 세션의 TTL 검사
 *   (2026-10-04 최정우 추가, 사용자 확정 — TTL 마감 권장안 (다))
 * @remark 종전에는 TTL 검사가 run() 끝에서만 돌아, 그 워커에 다음 배치가 오지 않으면 끊긴 트립이 재기동
 *   전까지 마감되지 않았다(열린 구간이 INSERT 안 되고 TRIP_END_DT 도 비어 있음). 세션이 없으면 DB 커넥션을
 *   잡지 않는다. 경과 판정·마감은 ExpireTtlSessions() 그대로다.
*/
void CRawLogWorker::idle(int nThreadId)
{
	if ((nThreadId < 0) || (nThreadId >= static_cast<int>(m_vtTripSessions.size())))
		return;
	if (m_vtTripSessions[static_cast<size_t>(nThreadId)].empty() || (m_stConfig.nTtlSec <= 0))
		return;
	PGconn *pcConn = AcquirePoolConnection(m_stConfig.pcPostgrePool,
		m_stConfig.nConnRetryMax, m_stConfig.nConnRetryWait);
	if (pcConn == nullptr)
		return;
	const int nExpired = ExpireTtlSessions(nThreadId, m_stConfig.nTtlSec, pcConn);
	if (nExpired > 0)
		LOGFMTI("[#%02d] idle ttl sweep!expired=[%d]", nThreadId, nExpired);
	ReleaseConnection(pcConn);
}

void CRawLogWorker::ReleaseConnection(PGconn *pcConn)
{
	if ((pcConn == nullptr) || (m_stConfig.pcPostgrePool == nullptr))
		return;

	PGTransactionStatusType nTxnStatus = PQtransactionStatus(pcConn);
	if ((nTxnStatus == PQTRANS_INTRANS) || (nTxnStatus == PQTRANS_INERROR))
	{
		// 미완료 트랜잭션 ROLLBACK (2026-07-08 최정우 주석 추가)
		PQexec(pcConn, "ROLLBACK");
	}

	// DB 커넥션 풀 반환 (2026-07-08 최정우 주석 추가)
	m_stConfig.pcPostgrePool->releaseConnection(pcConn);
}

/**
 * @brief Runnable 종료 콜백 (ThreadPool stop 시 호출)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] context 호출 컨텍스트 (미사용)
 * @return void
 * @remark #8: 진행 중 batch 는 run() 완료 시점까지 처리. 큐 잔여는 Server drain 이 release
*/
void CRawLogWorker::stop(int nThreadId, void *context)
{
	(void)nThreadId;
	(void)context;
}

/**
 * @brief FROM~TO 링크 사이 방향성 링크그래프(TURN_INFO 기반) bounded BFS (2026-09-01 최정우 추가)
 * @param[in] qwFromLink 시작 링크 ID
 * @param[in] qwToLink 목표 링크 ID
 * @param[in] nMaxHops 최대 탐색 홉 수 — 이 안에 못 찾으면 실패
 * @param[out] pvtPathOut 찾은 경로(qwFromLink~qwToLink 포함, 순서대로) — 실패 시 비움
 * @return true=경로 발견
 * @remark NODE_STEP 케이스3(SKIP 구간) 2순위 fallback 전용. ContinueMapMatch::GetLinkDepthInfo() 와
 *   동일하게 LINK_INFO.dwTurnOffset/nTurnCount → TURN_INFO.qwOutLinkID 로 진출링크를 순회하되,
 *   맵매칭 스코어링(세그먼트 비교·방위각·지리적 브릿지)은 전혀 하지 않는 순수 그래프 존재 여부
 *   탐색이라 훨씬 가볍다 — "경로가 있는가"만 확인하면 충분하기 때문
*/
bool CRawLogWorker::FindLinkPathBounded(uint64 qwFromLink, uint64 qwToLink, int nMaxHops,
		vector<uint64> *pvtPathOut)
{
	pvtPathOut->clear();
	if ((m_stConfig.pcDataLoader == nullptr) || (qwFromLink == 0) || (qwToLink == 0))
		return false;
	if (qwFromLink == qwToLink)
	{
		pvtPathOut->push_back(qwFromLink);
		return true;
	}

	unordered_map<uint64, uint64> mapParent;					// 링크ID → 그 직전 링크ID(경로 역추적용)
	unordered_set<uint64> setVisited;
	setVisited.insert(qwFromLink);
	vector<uint64> vtFrontier;
	vtFrontier.push_back(qwFromLink);

	bool bFound = false;
	for (int nHop = 0; (nHop < nMaxHops) && !bFound && !vtFrontier.empty(); ++nHop)
	{
		vector<uint64> vtNextFrontier;
		for (size_t f = 0; (f < vtFrontier.size()) && !bFound; ++f)
		{
			PLINK_INFO pstCur = m_stConfig.pcDataLoader->GetLinkInfo(vtFrontier[f]);
			if (pstCur == nullptr) continue;

			for (uint32 t = pstCur->dwTurnOffset; t < (pstCur->dwTurnOffset + pstCur->nTurnCount); ++t)
			{
				PTURN_INFO pstTurn = m_stConfig.pcDataLoader->GetTurnInfo(t);
				if (pstTurn == nullptr) continue;
				uint64 qwNext = pstTurn->qwOutLinkID;
				if (setVisited.find(qwNext) != setVisited.end()) continue;

				setVisited.insert(qwNext);
				mapParent[qwNext] = vtFrontier[f];
				if (qwNext == qwToLink) { bFound = true; break; }
				vtNextFrontier.push_back(qwNext);
			}
		}
		vtFrontier.swap(vtNextFrontier);
	}

	if (!bFound) return false;

	vector<uint64> vtRev;
	uint64 qwWalk = qwToLink;
	vtRev.push_back(qwWalk);
	while (qwWalk != qwFromLink)
	{
		unordered_map<uint64, uint64>::iterator it = mapParent.find(qwWalk);
		if (it == mapParent.end()) { pvtPathOut->clear(); return false; }		// 방어적 — 이론상 도달 불가
		qwWalk = it->second;
		vtRev.push_back(qwWalk);
	}
	pvtPathOut->assign(vtRev.rbegin(), vtRev.rend());
	return true;
}

/**
 * @brief 게이트 좌표의 "링크 진행거리" 산출 — 링크 시작노드부터 **폴리라인을 따라** 잰 거리(m)
 *   (2026-09-07 최정우 추가, 사용자 지적)
 * @param[in] qwLinkID 게이트가 놓인 링크 ID
 * @param[in] dfLon/dfLat 게이트 좌표
 * @return 링크 진행거리(m). 링크 형상을 못 얻으면 -1.0 — 호출측이 종전 직선거리로 대체한다
 *
 * @remark 왜 필요한가 — 게이트 통과 판정의 단위 불일치
 * \t 게이트 통과는 "이번 tick 의 링크 진행거리(wLenFromLink + dfSgmtMatchLen)"와 "게이트의 링크
 * \t 진행거리"를 비교해 정한다. 그런데 후자를 HaversineMeters(링크 시작노드, 게이트) 즉 **직선
 * \t 거리**로 재고 있었다. 링크가 굽으면 직선이 폴리라인보다 짧으므로 게이트가 실제보다 앞에
 * \t 있는 것처럼 계산되고, 게이트에 닿기 전 tick 이 통과로 오판된다.
 * \t 실측 000376_20260819140532 — 구간단속 RL-Z00003 진출게이트 TG00013, 링크 2040424301:
 * \t   · 폴리라인 길이 331.0m, 게이트의 실제 링크 진행거리 330.7m
 * \t   · 직선 시작노드→게이트 = 322.1m  (**8.6m 앞당겨짐**)
 * \t   · seq47 진행거리 322.7m → 322.7 < 322.1-3.0 이 거짓이라 통과로 확정.
 * \t     실제로는 게이트까지 8.0m 남아 있었고, 게이트에 닿은 tick 은 seq49(이격 0.42m)다.
 * \t   그 결과 구간단속이 35~47 로 3 tick 일찍 마감됐다(정답 35~49).
 * \t 링크가 곧을수록 오차가 0 에 수렴하므로 종전에도 대부분은 맞았다 — 굽은 링크에서만 틀린다.
 *
 * @remark 되돌리는 법 — 호출부의 이 함수 호출을 HaversineMeters(링크 시작노드, 게이트) 로
 * \t 되돌리면 2026-09-07 이전 판정으로 복귀한다.
 * \t 호출부는 **8곳**이다(2026-09-17 최정우 전수 확인, 종전 주석의 "6곳"은 오기):
 * \t   · UpdateOpenGateCrossed()                1곳 — 개방식 M게이트 통과 래치
 * \t   · ProcessOpenGateCharge() 트립시작 run   1곳 — case B/C 구분(±3m)
 * \t   · ProcessClosedRoadCharge()              3곳 — 진출 판정 / 같은 링크 출구 / 진입 게이트
 * \t   · ProcessSpeedZoneCharge()               3곳 — 진출 판정 / 같은 링크 출구 / 진입 게이트
*/
double CRawLogWorker::GatePosOnLink(uint64 qwLinkID, double dfLon, double dfLat)
{
	if (m_stConfig.pcDataLoader == nullptr)
		return -1.0;

	PLINK_INFO pstLink = m_stConfig.pcDataLoader->GetLinkInfo(qwLinkID);
	if (pstLink == nullptr)
		return -1.0;

	struct SEG_VERTEX { double dfX, dfY, dfLenFromStart; };
	vector<SEG_VERTEX> vtPts;
	if (pstLink->wSgmtCount == 0)
	{
		vtPts.push_back({ static_cast<double>(pstLink->dwStNodeX) / 360000.0,
			static_cast<double>(pstLink->dwStNodeY) / 360000.0, 0.0 });
	}
	else
	{
		for (uint32 i = 0; i < pstLink->wSgmtCount; ++i)
		{
			PLINK_SGMT_INFO pstSgmt = m_stConfig.pcDataLoader->GetLinkSgmtInfo(pstLink->dwSgmtOffset + i);
			if (pstSgmt == nullptr) continue;
			vtPts.push_back({ static_cast<double>(pstSgmt->dwX) / 360000.0,
				static_cast<double>(pstSgmt->dwY) / 360000.0,
				static_cast<double>(pstSgmt->wLenFromLink) });
		}
	}
	vtPts.push_back({ static_cast<double>(pstLink->dwEdNodeX) / 360000.0,
		static_cast<double>(pstLink->dwEdNodeY) / 360000.0, pstLink->dfLen });

	if (vtPts.size() < 2)
		return -1.0;

	// 게이트를 각 세그먼트에 정사영해 가장 가까운 세그먼트를 고르고, 그 세그먼트 시작점까지의
	//   누적거리에 세그먼트 내 진행분을 더한다. 위경도를 그대로 쓰면 경도 1도가 위도 1도보다
	//   짧아 투영이 틀어지므로 미터 스케일로 환산해 계산한다
	double dfBestDist = -1.0;
	double dfBestPos = 0.0;
	for (size_t i = 0; i + 1 < vtPts.size(); ++i)
	{
		const double dfKx = cos(vtPts[i].dfY * M_PI / 180.0) * 111320.0;
		const double dfKy = 111132.0;
		const double dfBx = (vtPts[i + 1].dfX - vtPts[i].dfX) * dfKx;
		const double dfBy = (vtPts[i + 1].dfY - vtPts[i].dfY) * dfKy;
		const double dfPx = (dfLon - vtPts[i].dfX) * dfKx;
		const double dfPy = (dfLat - vtPts[i].dfY) * dfKy;
		const double dfL2 = (dfBx * dfBx) + (dfBy * dfBy);

		double dfT = 0.0;
		if (dfL2 > 0.0)
		{
			dfT = ((dfPx * dfBx) + (dfPy * dfBy)) / dfL2;
			if (dfT < 0.0) dfT = 0.0;
			else if (dfT > 1.0) dfT = 1.0;
		}
		const double dfDx = dfPx - (dfBx * dfT);
		const double dfDy = dfPy - (dfBy * dfT);
		const double dfDist = sqrt((dfDx * dfDx) + (dfDy * dfDy));
		if ((dfBestDist < 0.0) || (dfDist < dfBestDist))
		{
			dfBestDist = dfDist;
			dfBestPos = vtPts[i].dfLenFromStart
				+ ((vtPts[i + 1].dfLenFromStart - vtPts[i].dfLenFromStart) * dfT);
		}
	}

	return (dfBestDist < 0.0) ? -1.0 : dfBestPos;
}

/**
 * @brief 링크 폴리라인이 폴리곤 안에 들어가 있는 **모든** 구간을 링크 진행순으로 구한다
 *   (2026-09-21 최정우 추가, 사용자 지시 — 이슈 31)
 * @param[in] qwLinkID 대상 링크
 * @param[in] vtPolyCoords 폴리곤 정점(평문 경위도, base_roadlink.coords 파싱 결과)
 * @param[out] pvtSpans 안쪽 구간 목록(호출 시 비워진다). LINK_POLY_SPAN 주석 참고
 * @return true: 안쪽 구간을 하나 이상 찾음 / false: 링크 전체가 폴리곤 밖이거나 정보 없음
 * @remark 세그먼트 정점마다 CChargeDataLoader::IsPointInPolygon() 으로 안/밖을 판정하고,
 *   부호가 바뀌는 세그먼트는 이진탐색(24회, 세그먼트 최대 길이 기준 서브미터 정밀도)으로
 *   경계점을 근사한다 — 폴리곤 변과의 직접 교차식 대신 기존 판정 함수를 재사용해 오목
 *   폴리곤에도 안전하다.
 * @remark **왜 목록인가** — 종전에는 진입(FindLinkPolygonCrossing)·이탈
 *   (FindLinkPolygonExitCrossing) 각각이 **첫 교차에서 곧바로 return** 했다. 그래서 한 링크가
 *   폴리곤 모서리를 관통(밖→안→밖)하면 진입 쪽은 진입점까지만 보고, 이탈 쪽은 첫 이탈 이후를
 *   전부 "밖"으로 세어 꼬리 구간을 잃었다. 주정차 영역을 링크의 폴리곤 경계 기준으로 안/밖으로
 *   나누려면(이슈 18) 교차점을 전부 알아야 해서 일반형으로 분리했다.
 * @remark 아래 두 함수는 이 함수 결과의 **첫 구간**만 읽어 종전과 동일한 값을 만든다 —
 *   동작 보존 리팩터링이므로 재매칭 결과가 달라지면 안 된다.
*/
bool CRawLogWorker::FindLinkPolygonSpans(uint64 qwLinkID, const vector<POINT>& vtPolyCoords,
		vector<LINK_POLY_SPAN> *pvtSpans)
{
	if (pvtSpans == nullptr)
		return false;
	pvtSpans->clear();

	if ((m_stConfig.pcDataLoader == nullptr) || (vtPolyCoords.size() < 3))
		return false;

	PLINK_INFO pstLink = m_stConfig.pcDataLoader->GetLinkInfo(qwLinkID);
	if (pstLink == nullptr)
		return false;

	// 링크 형상 정점 목록 — 시작노드 + 세그먼트 정점들 + 종료노드. wSgmtCount==0 이면 두 노드를
	//   잇는 직선으로 본다(종전 두 함수가 각자 갖고 있던 동일 코드를 여기로 일원화)
	struct SEG_VERTEX { double dfX, dfY, dfLenFromStart; };
	vector<SEG_VERTEX> vtPts;
	if (pstLink->wSgmtCount == 0)
	{
		vtPts.push_back({ static_cast<double>(pstLink->dwStNodeX) / 360000.0,
			static_cast<double>(pstLink->dwStNodeY) / 360000.0, 0.0 });
	}
	else
	{
		for (uint32 i = 0; i < pstLink->wSgmtCount; ++i)
		{
			PLINK_SGMT_INFO pstSgmt = m_stConfig.pcDataLoader->GetLinkSgmtInfo(pstLink->dwSgmtOffset + i);
			if (pstSgmt == nullptr) continue;
			vtPts.push_back({ static_cast<double>(pstSgmt->dwX) / 360000.0,
				static_cast<double>(pstSgmt->dwY) / 360000.0,
				static_cast<double>(pstSgmt->wLenFromLink) });
		}
	}
	vtPts.push_back({ static_cast<double>(pstLink->dwEdNodeX) / 360000.0,
		static_cast<double>(pstLink->dwEdNodeY) / 360000.0, pstLink->dfLen });

	if (vtPts.size() < 2)
		return false;

	// 세그먼트 [i, i+1] 사이에서 안/밖이 바뀌는 지점을 이진탐색으로 근사 — bWantInside=true 면
	//   "밖→안"(경계를 넘어 안이 되는 첫 점), false 면 "안→밖"(밖이 되는 첫 점)을 찾는다
	auto fnBisect = [&](const SEG_VERTEX& stA, const SEG_VERTEX& stB, bool bWantInside,
			double *pdfDistM, double *pdfX, double *pdfY)
	{
		double dfLo = 0.0, dfHi = 1.0;			// dfLo=시작쪽 상태, dfHi=끝쪽 상태
		for (int nIter = 0; nIter < 24; ++nIter)
		{
			const double dfMid = (dfLo + dfHi) * 0.5;
			const double dfMidX = stA.dfX + (stB.dfX - stA.dfX) * dfMid;
			const double dfMidY = stA.dfY + (stB.dfY - stA.dfY) * dfMid;
			const bool bMidIn = CChargeDataLoader::IsPointInPolygon(dfMidX, dfMidY, vtPolyCoords);
			// 찾는 상태(bWantInside)에 도달했으면 경계는 이 앞이므로 상한을 당긴다
			if (bMidIn == bWantInside) dfHi = dfMid;
			else                       dfLo = dfMid;
		}

		POINT stFrom, stCross;
		stFrom.dfX = stA.dfX;  stFrom.dfY = stA.dfY;
		stCross.dfX = stA.dfX + (stB.dfX - stA.dfX) * dfHi;
		stCross.dfY = stA.dfY + (stB.dfY - stA.dfY) * dfHi;

		*pdfDistM = stA.dfLenFromStart + HaversineMeters(stFrom, stCross);
		*pdfX = stCross.dfX;
		*pdfY = stCross.dfY;
	};

	bool bPrevIn = CChargeDataLoader::IsPointInPolygon(vtPts[0].dfX, vtPts[0].dfY, vtPolyCoords);
	LINK_POLY_SPAN stCur;
	bool bOpen = false;

	if (bPrevIn)
	{
		// 링크가 폴리곤 안에서 시작 — 구간 시작은 경계가 아니라 링크 시작 노드 자체다
		stCur = LINK_POLY_SPAN();
		stCur.dfStartDistM = vtPts[0].dfLenFromStart;
		stCur.dfStartX = vtPts[0].dfX;
		stCur.dfStartY = vtPts[0].dfY;
		stCur.bEntryIsBoundary = false;
		bOpen = true;
	}

	for (size_t i = 0; i + 1 < vtPts.size(); ++i)
	{
		const bool bNextIn = CChargeDataLoader::IsPointInPolygon(vtPts[i + 1].dfX, vtPts[i + 1].dfY, vtPolyCoords);
		if (bNextIn == bPrevIn)
		{
			bPrevIn = bNextIn;
			continue;								// 이 세그먼트 안에서는 상태가 안 바뀜
		}

		double dfCrossDistM = 0.0, dfCrossX = 0.0, dfCrossY = 0.0;
		fnBisect(vtPts[i], vtPts[i + 1], bNextIn, &dfCrossDistM, &dfCrossX, &dfCrossY);

		if (bNextIn)
		{
			// 밖 → 안 : 새 구간 개시
			stCur = LINK_POLY_SPAN();
			stCur.dfStartDistM = dfCrossDistM;
			stCur.dfStartX = dfCrossX;
			stCur.dfStartY = dfCrossY;
			stCur.bEntryIsBoundary = true;
			bOpen = true;
		}
		else if (bOpen)
		{
			// 안 → 밖 : 진행 중이던 구간 마감
			stCur.dfEndDistM = dfCrossDistM;
			stCur.dfEndX = dfCrossX;
			stCur.dfEndY = dfCrossY;
			stCur.bExitIsBoundary = true;
			pvtSpans->push_back(stCur);
			bOpen = false;
		}

		bPrevIn = bNextIn;
	}

	if (bOpen)
	{
		// 링크가 폴리곤 안에서 끝남 — 구간 끝은 경계가 아니라 링크 종료 노드 자체다
		const SEG_VERTEX& stLast = vtPts[vtPts.size() - 1];
		stCur.dfEndDistM = stLast.dfLenFromStart;
		stCur.dfEndX = stLast.dfX;
		stCur.dfEndY = stLast.dfY;
		stCur.bExitIsBoundary = false;
		pvtSpans->push_back(stCur);
	}

	return !pvtSpans->empty();
}

/**
 * @brief 링크의 시작 노드부터 세그먼트를 순서대로 훑어, 폴리곤과 처음 교차하는 지점까지의
 *   부분 거리·좌표를 구한다 — 주정차 접촉으로 NODE_STEP run이 마감될 때 "이 링크를 끝까지
 *   달렸다"고 가정하는 기존 이탈 지점 보정 대신, 실제로 폴리곤에 들어가는 지점까지만 정확히
 *   계산하기 위함(사용자 지시, 2026-09-03 최정우 추가)
 * @param[in] qwLinkID 대상 링크
 * @param[in] vtPolyCoords 폴리곤 정점(평문 경위도, base_roadlink.coords 파싱 결과)
 * @param[out] pdfPartialDistM 링크 시작 노드~교차점까지의 거리(m)
 * @param[out] pdfCrossX/Y 교차점 좌표(평문 경위도)
 * @return true: 링크 도중에 폴리곤 진입 확인(출력 채움) / false: 링크 전체가 폴리곤 밖(교차 없음,
 *   호출측이 이 링크 전체 길이를 더하고 다음 링크로 계속 진행)
 * @remark [2026-09-21 최정우 수정] 본문을 FindLinkPolygonSpans() 위임으로 교체했다(동작 동일).
 *   종전에는 링크 형상 구성과 이진탐색을 이 함수와 FindLinkPolygonExitCrossing() 이 각각
 *   복제해 갖고 있었다. **첫 구간의 시작**이 곧 종전의 "처음 교차하는 지점"이다 — 링크가
 *   폴리곤 안에서 시작하면 첫 구간 시작이 0.0(시작 노드)이라 종전 조기반환과 같은 값이 된다.
*/
bool CRawLogWorker::FindLinkPolygonCrossing(uint64 qwLinkID, const vector<POINT>& vtPolyCoords,
		double *pdfPartialDistM, double *pdfCrossX, double *pdfCrossY)
{
	vector<LINK_POLY_SPAN> vtSpans;
	if (!FindLinkPolygonSpans(qwLinkID, vtPolyCoords, &vtSpans) || vtSpans.empty())
		return false;			// 링크 전체가 폴리곤 밖 — 교차 없음

	*pdfPartialDistM = vtSpans[0].dfStartDistM;
	*pdfCrossX = vtSpans[0].dfStartX;
	*pdfCrossY = vtSpans[0].dfStartY;
	return true;
}

/**
 * @brief 링크를 시작 노드부터 따라가며 폴리곤 "안→밖"으로 벗어나는 지점을 찾는다
 * @param[in] qwLinkID 대상 링크 ID
 * @param[in] vtPolyCoords 폴리곤 좌표 목록
 * @param[out] pdfExitDistM 링크 시작부터 이탈 지점까지의 거리(m)
 * @param[out] pdfCrossX/pdfCrossY 이탈 지점 좌표
 * @return true(이탈 지점 찾음), false(링크 전체가 폴리곤 안이거나 전체가 밖 — 교차 없음)
 * @remark FindLinkPolygonCrossing() 의 이탈 방향 대칭. 그쪽은 "밖에서 들어오는" 첫 교차를 찾고
 *   이쪽은 "안에서 나가는" 첫 교차를 찾는다. 확정 접촉이 이탈될 때 그 경계부터 NODE_STEP run 을
 *   여는 용도 — 폴리곤 안 구간은 여전히 일반도로로 세지 않고, 경계 이후만 센다
 *   (2026-09-05 최정우 추가, 사용자 지시)
 * @remark [2026-09-21 최정우 수정] 본문을 FindLinkPolygonSpans() 위임으로 교체했다(동작 동일).
 *   **첫 구간의 끝이 실제 경계 교차일 때**(bExitIsBoundary) 그 값이 곧 종전의 "안→밖 첫 교차"다.
 *   링크 전체가 폴리곤 안이면 그 끝은 링크 종료 노드라 bExitIsBoundary=false 가 되고, 이때
 *   false 를 돌려주는 것이 종전 동작과 같다.
*/
bool CRawLogWorker::FindLinkPolygonExitCrossing(uint64 qwLinkID, const vector<POINT>& vtPolyCoords,
		double *pdfExitDistM, double *pdfCrossX, double *pdfCrossY)
{
	vector<LINK_POLY_SPAN> vtSpans;
	if (!FindLinkPolygonSpans(qwLinkID, vtPolyCoords, &vtSpans) || vtSpans.empty())
		return false;			// 링크 전체가 폴리곤 밖 — 이탈 경계 없음

	if (!vtSpans[0].bExitIsBoundary)
		return false;			// 링크 전체(또는 남은 전부)가 폴리곤 안 — 이탈 경계 없음

	*pdfExitDistM = vtSpans[0].dfEndDistM;
	*pdfCrossX = vtSpans[0].dfEndX;
	*pdfCrossY = vtSpans[0].dfEndY;
	return true;
}

/**
 * @brief NODE_STEP 케이스3(SKIP 구간) 브릿지 — CommitPendingRow() 가 새 신뢰매칭을 확정하는 순간,
 *   그 직전까지의 SKIP(완전 매칭실패) 구간이 있었으면 그 구간을 NODE_STEP 으로 등록 시도 (2026-09-01 최정우 추가)
 * @param[in] nThreadId 워커 스레드 ID(로그용)
 * @param[in,out] pstSession vtSkipRunRawLogInfo(SKIP 구간 raw tick 버퍼) 사용 후 호출측이 clear
 * @param[in] qwFromLink SKIP 시작 전 마지막 확정 링크(FROM_ID)
 * @param[in] qwToLink 이번에 새로 확정된 링크(TO_ID)
 * @param[in] stToRawLogInfo/stToMatchLinkInfo TO 확정 tick 의 raw GPS·매칭 결과
 * @param[out] pvtChargeInserts 브릿지 성공 시 1행 적재
 * @remark 호출 전제: qwFromLink!=qwToLink 이고 실제 SKIP 구간(vtSkipRunRawLogInfo 비어있지 않음)이
 *   있었을 때만 호출됨(CommitPendingRow 쪽 가드). 사용자 확정 3단계 fallback:
 *   1순위 재매칭 — 버퍼된 각 raw tick 을 qwToLink 로 편향 재매칭(RematchBeginBiased, 클램프
 *     브릿지와 동일 메커니즘) 시도. 이 API 는 "특정 링크로의 편향" 만 지원해 진짜 여러 링크를
 *     거친 긴 SKIP 구간의 중간 링크ID 자체를 알아낼 수는 없다 — 그래서 "모든 tick 이 qwToLink 로
 *     재매칭 성공"하는 경우만 성공으로 본다(=사실상 그 구간 내내 이미 TO 링크 위였던 짧은 SKIP).
 *     일부만 성공하거나 전부 실패하면 2순위로 넘어간다. 성공 시 재매칭된 첫/끝 tick 위치·시각으로
 *     dist_m/stay_seconds 산출, charge_yn/status=Y/0.
 *   2순위 그래프탐색 — FindLinkPathBounded() 로 FROM~TO 사이 방향성 링크그래프 경로를 최대
 *     MM_SKIPGAP_MAX_HOPS 홉 이내에서 탐색 + GPS 경과시간 대비 비현실적으로 긴 경로 배제
 *     (ContinueMapMatch 의 MM_PATH_PLAUSIBLE_SCALE/FLOOR_M 재사용, 기준값은 버퍼된 raw tick들의
 *     실측 이동거리 합). 찾으면 경로 링크들의 실제 등록 길이(link.psf dfLen) 합산=dist_m,
 *     charge_yn/status=Y/0.
 *   3순위 직선거리 — 위 둘 다 실패하면 FROM 마지막 신뢰좌표~TO 확정좌표 haversine 직선거리로
 *     대체, charge_yn/status=N/3(AUDIT) — 근사값임을 표시(사용자 확정 답변). 단 FROM 신뢰좌표
 *     자체가 무효(세션갭 30초 초과 리셋 등, bHasLastMatch=false)면 haversine 을 구하지 않고
 *     dfRawAccumDistM(SKIP 버퍼 tick 간 실측 누적거리)로 대체, speed_kmh/stay_seconds=0,
 *     non_charge_reason=NCR_NODE_STEP_GAP_ANCHOR_LOST 기록(사용자 지시, 2026-09-01 최정우 추가 —
 *     실측 000093_20260817102000 seq9~19 에서 FROM 이 (0,0)으로 읽혀 dist_m 13,199km·avg speed
 *     smallint 오버플로로 charge INSERT 전체 실패한 장애의 재발 방지)
*/
void CRawLogWorker::ResolveSkipGapNodeStep(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		uint64 qwFromLink, uint64 qwToLink, const sRawLogInfo& stToRawLogInfo,
		const MATCH_LINK_INFO& stToMatchLinkInfo, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	static const int MM_SKIPGAP_MAX_HOPS = 6;					// 2순위 그래프탐색 최대 홉 수

	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;
	if (!m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(qwFromLink)
		|| !m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(qwToLink))
		return;													// 케이스3 범위 밖 — 그 유형 자체 로직에 맡김(질문2 확정 답변)
	// [2026-10-04 최정우 추가, 사용자 확정 — E18] 주정차 폴리곤 안 SKIP 좌표가 있는 구간은 일반도로로 잇지 않는다
	//   (주정차 단속 정보에만 쓴다). 열린 run 은 ProcessNodeStepCharge() 가 이미 SKIP 직전 tick 에서 끊었다.
	// 되돌리는 법: 이 if 블록을 지운다(ProcessNodeStepCharge 의 E18 분기와 함께).
	if (IsSkipGapInParkingZone(pstSession) || (pstSession->dwSkipParkGapCutSeq == stToRawLogInfo.dwSeqNo))
	{
		LOGFMTI("[#%02d] SKIP-gap in parking zone -> not bridged as general road!device=[%s] trip_id=[%s] "
			"gps_seq=[%u~%u] skip_ticks=[%zu]",
			nThreadId, stToRawLogInfo.szDeviceKey, stToRawLogInfo.szTripID,
			pstSession->dwLastConfirmedLinkGpsSeq, stToRawLogInfo.dwSeqNo, pstSession->vtSkipRunRawLogInfo.size());
		pstSession->dwSkipGapTickGpsSeq = 0;
		return;
	}

	const time_t dtFrom = pstSession->dtLastConfirmedLinkTime;
	const time_t dtTo = stToRawLogInfo.dtGPS;
	const uint32 dwFromGpsSeq = pstSession->dwLastConfirmedLinkGpsSeq;
	const uint32 dwToGpsSeq = stToRawLogInfo.dwSeqNo;

	// [버그 수정, 2026-09-15 최정우, 사용자 지시] 이 갭을 방금 ProcessNodeStepCharge() 가 열려있는
	//   일반도로 run 에 "직선 한 방"으로 이미 더했다면, 여기서 별도 과금행을 또 만들면 같은 구간이
	//   두 번 청구된다(실측 000370_20260826143912: run 이 270~278 을 561m 로 흡수했는데 브릿지가
	//   872m 짜리 Y/0 행을 따로 만들어 합 1,433m). 그런 경우엔 행을 만들지 않고 run 의 그 구간
	//   거리만 더 정확한 브릿지 값으로 **교체**한다 — 주행 1구간 = 1레코드 원칙 유지 + 그래프
	//   경로거리의 정확도도 유지. run 이 안 열려 있거나(세션 리셋·구역 진입 등) 다른 tick 의
	//   값이면 교체 대상이 없으므로 종전대로 별도 행을 만든다.
	//   N/3(AUDIT) 브릿지는 과금 대상이 아니라 심사 증거이므로 이 교체를 적용하지 않는다.
	auto AbsorbGapIntoOpenRun = [&](double dfBridgeDistM) -> bool
	{
		if (pstSession->dwSkipGapTickGpsSeq != dwToGpsSeq)
			return false;
		// [보강, 2026-09-15 최정우, 소스 재검토 지적] dwEntryGpsSeq 는 run 의 유일 식별자가 아니다 —
		//   같은 tick 에 등록구역 run 과 미등록 pseudo run 이 함께 열리거나, 게이트 진출 이월 경로가
		//   둘 다 dwGateExitGpsSeq 를 쓰면 값이 겹친다. 후보가 2개 이상이면 어느 run 이 그 거리를
		//   누적했는지 특정할 수 없으므로 **흡수하지 않고 종전대로 별도 행을 만든다**(이중 계상은
		//   남지만, 엉뚱한 run 의 거리를 깎는 것보다 낫다 — 정확도 우선).
		size_t nMatchIdx = pstSession->vtNodeStepRuns.size();
		size_t nMatchCnt = 0;
		for (size_t i = 0; i < pstSession->vtNodeStepRuns.size(); ++i)
		{
			if (pstSession->vtNodeStepRuns[i].dwEntryGpsSeq == pstSession->dwSkipGapTickRunEntry)
			{ nMatchIdx = i; ++nMatchCnt; }
		}
		if (nMatchCnt != 1)
		{
			if (nMatchCnt > 1)
			{
				LOGFMTW("[#%02d] SKIP-gap absorb skipped(ambiguous run)!device=[%s] trip_id=[%s] "
					"gps_seq=[%u] run_entry=[%u] candidates=[%zu]",
					nThreadId, stToRawLogInfo.szDeviceKey, stToRawLogInfo.szTripID, dwToGpsSeq,
					pstSession->dwSkipGapTickRunEntry, nMatchCnt);
			}
			return false;
		}
		{
			ZONE_RUN_SESSION& stRun = pstSession->vtNodeStepRuns[nMatchIdx];
			stRun.dfAccumDistM += (dfBridgeDistM - pstSession->dfSkipGapTickDistM);
			if (stRun.dfAccumDistM < 0.0) stRun.dfAccumDistM = 0.0;
			LOGFMTI("[#%02d] node step SKIP-gap absorbed into open run!device=[%s] trip_id=[%s] "
				"gps_seq=[%u] run_entry=[%u] straight=[%.1f]m -> bridge=[%.1f]m (no separate row)",
				nThreadId, stToRawLogInfo.szDeviceKey, stToRawLogInfo.szTripID, dwToGpsSeq,
				stRun.dwEntryGpsSeq, pstSession->dfSkipGapTickDistM, dfBridgeDistM);
			pstSession->dwSkipGapTickGpsSeq = 0;			// 1회성 — 재사용 방지
			return true;
		}
	};

	// 버퍼된 raw tick들의 실측 이동거리 합 — 2순위 비현실성 체크 기준값(dfHorizMove 대체) +
	//   3순위 직선거리의 대체 재료로도 못 씀(직선거리는 아래에서 FROM/TO 좌표로 별도 계산)
	double dfRawAccumDistM = 0.0;
	{
		double dfPrevX = pstSession->dfLastMatchX, dfPrevY = pstSession->dfLastMatchY;
		bool bHasPrev = pstSession->bHasLastMatch;
		for (size_t i = 0; i < pstSession->vtSkipRunRawLogInfo.size(); ++i)
		{
			const RAW_LOG_INFO& stTick = pstSession->vtSkipRunRawLogInfo[i];
			if (bHasPrev)
			{
				POINT stPrev, stCur;
				stPrev.dfX = dfPrevX;  stPrev.dfY = dfPrevY;
				stCur.dfX = stTick.dfX;  stCur.dfY = stTick.dfY;
				dfRawAccumDistM += HaversineMeters(stPrev, stCur);
			}
			dfPrevX = stTick.dfX;  dfPrevY = stTick.dfY;
			bHasPrev = true;
		}
		if (bHasPrev)
		{
			POINT stPrev, stCur;
			stPrev.dfX = dfPrevX;  stPrev.dfY = dfPrevY;
			stCur.dfX = stToRawLogInfo.dfX;  stCur.dfY = stToRawLogInfo.dfY;
			dfRawAccumDistM += HaversineMeters(stPrev, stCur);
		}
	}

	// ── 1순위: 재매칭 시도 ──
	if ((m_stConfig.pcProcessManager != nullptr) && !pstSession->vtSkipRunRawLogInfo.empty())
	{
		CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
		bool bAllRematched = true;
		time_t dtFirstRematch = 0;
		for (size_t i = 0; (i < pstSession->vtSkipRunRawLogInfo.size()) && bAllRematched; ++i)
		{
			MATCH_LINK_INFO stRematched;
			if (!cPM.RematchBeginBiased(pstSession->vtSkipRunRawLogInfo[i], qwToLink, &stRematched)
				|| (stRematched.qwLinkID != qwToLink))
			{
				bAllRematched = false;
				break;
			}
			if (dtFirstRematch == 0) dtFirstRematch = pstSession->vtSkipRunRawLogInfo[i].dtGPS;
		}

		if (bAllRematched)
		{
			PLINK_INFO pstToLink = (m_stConfig.pcDataLoader != nullptr)
				? m_stConfig.pcDataLoader->GetLinkInfo(qwToLink) : nullptr;
			double dfDistM = (pstToLink != nullptr) ? pstToLink->dfLen : dfRawAccumDistM;
			time_t dtStart = (dtFirstRematch != 0) ? dtFirstRematch : dtFrom;
			time_t dtEnd = dtTo;

			// 열려있는 일반도로 run 이 이 갭을 이미 직선으로 흡수했으면 별도 행 대신 그 run 의
			//   거리만 교체한다(이중 계상 방지) — 위 AbsorbGapIntoOpenRun 주석 참고 (2026-09-15 최정우)
			if (AbsorbGapIntoOpenRun(dfDistM))
				return;

			CHARGE_INSERT_ROW stRow;
			BuildNodeStepRowFromLinkRange(
				stToRawLogInfo.szTripID, stToRawLogInfo.szDeviceKey, pstSession->nChargeSeq,
				qwFromLink, qwToLink,
				pstSession->dfLastMatchX, pstSession->dfLastMatchY,
				stToMatchLinkInfo.dfMatchX, stToMatchLinkInfo.dfMatchY,
				dfDistM, dtStart, dtEnd, dwFromGpsSeq, dwToGpsSeq, "Y", "0", nullptr, nullptr, &stRow);
			pvtChargeInserts->push_back(stRow);
			pstSession->nChargeSeq += 1;

			LOGFMTI("[#%02d] node step SKIP-gap bridged(rematch)!device=[%s] trip_id=[%s] from=[%llu] to=[%llu] "
				"dist_m=[%.1f] non_charge_reason=[%d:%s]", nThreadId, stToRawLogInfo.szDeviceKey,
				stToRawLogInfo.szTripID, static_cast<unsigned long long>(qwFromLink),
				static_cast<unsigned long long>(qwToLink), dfDistM, NCR_NORMAL,
				m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
			return;
		}
	}

	// ── 2순위: 그래프 경로탐색 ──
	{
		vector<uint64> vtPath;
		if (FindLinkPathBounded(qwFromLink, qwToLink, MM_SKIPGAP_MAX_HOPS, &vtPath) && (m_stConfig.pcDataLoader != nullptr))
		{
			double dfPathDistM = 0.0;
			bool bAllLenOk = true;
			for (size_t i = 0; i < vtPath.size(); ++i)
			{
				PLINK_INFO pstLink = m_stConfig.pcDataLoader->GetLinkInfo(vtPath[i]);
				if (pstLink == nullptr) { bAllLenOk = false; break; }
				dfPathDistM += pstLink->dfLen;
			}

			// 비현실성 체크 — GPS 경과시간 대비 과도하게 긴 경로 배제(ContinueMapMatch 의
			//   MM_PATH_PLAUSIBLE_SCALE/FLOOR_M 과 동일 기준, dfHorizMove 대신 버퍼된 raw tick
			//   실측 이동거리 합을 씀)
			double dfPlausibleMaxM = (dfRawAccumDistM * MM_PATH_PLAUSIBLE_SCALE) + MM_PATH_PLAUSIBLE_FLOOR_M;

			if (bAllLenOk && (dfPathDistM <= dfPlausibleMaxM))
			{
				// 위 1순위(rematch) 분기와 동일 근거 (2026-09-15 최정우)
				if (AbsorbGapIntoOpenRun(dfPathDistM))
					return;

				CHARGE_INSERT_ROW stRow;
				BuildNodeStepRowFromLinkRange(
					stToRawLogInfo.szTripID, stToRawLogInfo.szDeviceKey, pstSession->nChargeSeq,
					qwFromLink, qwToLink,
					pstSession->dfLastMatchX, pstSession->dfLastMatchY,
					stToMatchLinkInfo.dfMatchX, stToMatchLinkInfo.dfMatchY,
					dfPathDistM, dtFrom, dtTo, dwFromGpsSeq, dwToGpsSeq, "Y", "0", nullptr, nullptr, &stRow);
				pvtChargeInserts->push_back(stRow);
				pstSession->nChargeSeq += 1;

				LOGFMTI("[#%02d] node step SKIP-gap bridged(graph path)!device=[%s] trip_id=[%s] from=[%llu] "
					"to=[%llu] hops=[%zu] dist_m=[%.1f] non_charge_reason=[%d:%s]", nThreadId,
					stToRawLogInfo.szDeviceKey, stToRawLogInfo.szTripID,
					static_cast<unsigned long long>(qwFromLink), static_cast<unsigned long long>(qwToLink),
					vtPath.size(), dfPathDistM, NCR_NORMAL,
					m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
				return;
			}
		}
	}

	// ── 3순위: 직선거리 + AUDIT ──
	{
		// FROM 위치(dfLastMatchX/Y)가 실제로 유효한지 먼저 확인 — 세션갭(MM_SESSION_RESET_GAP_SEC=30초)
		//   초과로 세션 앵커가 리셋되면(ProcessRawLog (D), 2026-07-15) bHasLastMatch=false 이면서
		//   dfLastMatchX/Y 는 (0,0) 그대로다. 이 값을 실좌표로 오인해 haversine 거리를 구하면
		//   비현실적으로 커진다(실측 13,199~13,209km, speed_kmh smallint 오버플로로 charge INSERT
		//   전체가 실패해 재시도 폭주까지 유발한 실제 장애 원인). FROM 이 무효하면 직선거리 대신 이미
		//   구해둔 실측 누적이동거리(dfRawAccumDistM, 위 SKIP 버퍼 tick 간 haversine 합 — 첫 구간은
		//   bHasLastMatch 로 이미 걸러져 있어 이 경우에도 오염되지 않는다)를 쓰고, 속도·체류시간은
		//   산출 근거가 없어 0 으로 기록, NON_CHARGE_REASON 에 사유를 남긴다(사용자 지시, 2026-09-01
		//   최정우 추가) — 에러코드 값은 임시, 추후 정식 체계 정리 시 재배정 예정
		bool bFromValid = pstSession->bHasLastMatch;

		POINT stTo;
		stTo.dfX = stToMatchLinkInfo.dfMatchX;  stTo.dfY = stToMatchLinkInfo.dfMatchY;

		double dfFromX = pstSession->dfLastMatchX;
		double dfFromY = pstSession->dfLastMatchY;
		double dfDistM;
		if (bFromValid)
		{
			POINT stFrom;
			stFrom.dfX = dfFromX;  stFrom.dfY = dfFromY;
			dfDistM = HaversineMeters(stFrom, stTo);
		}
		else if (!pstSession->vtSkipRunRawLogInfo.empty())
		{
			dfFromX = pstSession->vtSkipRunRawLogInfo.front().dfX;
			dfFromY = pstSession->vtSkipRunRawLogInfo.front().dfY;
			dfDistM = dfRawAccumDistM;
		}
		else
		{
			dfFromX = stTo.dfX;
			dfFromY = stTo.dfY;
			dfDistM = dfRawAccumDistM;
		}

		CHARGE_INSERT_ROW stRow;
		BuildNodeStepRowFromLinkRange(
			stToRawLogInfo.szTripID, stToRawLogInfo.szDeviceKey, pstSession->nChargeSeq,
			qwFromLink, qwToLink,
			dfFromX, dfFromY,
			stToMatchLinkInfo.dfMatchX, stToMatchLinkInfo.dfMatchY,
			dfDistM, dtFrom, dtTo, dwFromGpsSeq, dwToGpsSeq, "N", "3", nullptr, nullptr, &stRow);

		// [버그 수정, 2026-09-15 최정우] 종전엔 !bFromValid 면 속도·체류시간을 무조건 0 으로 지웠으나,
		//   세션갭 리셋(ProcessRawLog (D))이 버리는 건 위치 앵커(dfLastMatchX/Y·dtLastMatchGps·
		//   bHasLastMatch)뿐이고 dtLastConfirmedLinkTime/dwLastConfirmedLinkGpsSeq 는 그대로 살아있다
		//   — 즉 시각은 알 수 있는데 버려서 "569m 를 0 초에 이동, 평균 0km/h" 라는 모순된 AUDIT 행이
		//   남았다(사용자 지적, 실측 000376_20260826152113 trip_seq=2 등 5건). dtFrom 이 유효하면
		//   BuildNodeStepRowFromLinkRange 가 이미 산출해 둔 값을 그대로 쓴다. dist_m 이 실측이 아닌
		//   추정치라는 경고는 non_charge_reason=1 이 계속 담당하므로 판정 근거는 그대로다.
		//   한 번도 확정매칭을 못 받아 dtFrom 이 0 인 경우만 종전대로 0 — 1970 기준 경과초가
		//   speed_kmh(smallint)를 넘겨 배치 INSERT 전체를 실패시키는 걸 막는 방어.
		if (!bFromValid && (dtFrom <= 0))
		{
			stRow.strSpeedKmh = "0";
			stRow.strStaySeconds = "0";
		}
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — CHARGE_YN/STATUS(N/3)
		//   판정 자체는 위 BuildNodeStepRowFromLinkRange 호출에서 이미 끝났고 여기선 안 건드림,
		//   사유만 부가로 남긴다(bFromValid 로 이미 갈라놓은 두 케이스 그대로 사용)
		const int nThisChargeSeq = pstSession->nChargeSeq;
		const int nNonChargeReason = bFromValid ? NCR_NODE_STEP_GAP_APPROX : NCR_NODE_STEP_GAP_ANCHOR_LOST;
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", nNonChargeReason);
		stRow.strNonChargeReason = szReason;

		pvtChargeInserts->push_back(stRow);
		pstSession->nChargeSeq += 1;

		LOGFMTW("[#%02d] node step SKIP-gap bridged(straight-line fallback, AUDIT)!device=[%s] trip_id=[%s] "
			"charge_seq=[%d] from=[%llu] to=[%llu] dist_m=[%.1f] stay=[%s]s avg_speed=[%s] from_valid=[%d] "
			"non_charge_reason=[%d:%s]", nThreadId, stToRawLogInfo.szDeviceKey,
			stToRawLogInfo.szTripID, nThisChargeSeq, static_cast<unsigned long long>(qwFromLink),
			static_cast<unsigned long long>(qwToLink), dfDistM, stRow.strStaySeconds.c_str(),
			stRow.strSpeedKmh.c_str(), static_cast<int>(bFromValid),
			nNonChargeReason,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nNonChargeReason));
	}
}

/**
 * @brief 클램프 저신뢰 SKIP 런을 "전·후 확정좌표 궤적 방향"으로 검증해 MATCH_STATUS 만 복원
 * @param[in] nThreadId 워커 스레드 ID(로그용)
 * @param[in,out] pstSession 클램프 런 버퍼(vtClampRunUpdateIdx/vtClampRunRawLogInfo) 사용 — 정리는 호출측
 * @param[in] dfNextMatchX/dfNextMatchY 이번에 확정된 "다음" 매칭 좌표(後)
 * @param[in] stRawLogInfo 로그용 device_key/trip_id 참조
 * @param[out] pvtUpdates 신뢰 회복된 tick 의 MATCH_STATUS 를 "1" 로 재기록
 * @return 복원한 tick 수
 * @remark "전(前, 클램프 런 시작 직전 신뢰 매칭좌표) → 후(後, 이번에 확정된 매칭좌표)"를 이은 실측
 *   이동방향을 클램프 런 각 tick 의 raw heading 과 비교한다. 도로 세그먼트 방향(회전 중이라 애매)이나
 *   기기 순간속도 게이트(MM_SPEED_HIGH_KMH=20km/h 이상 요구, bClampTrustedByHeading)에 기대지 않고
 *   "확정된 두 지점을 잇는 실제 궤적"이라는 더 안정적인 신호를 쓰므로 저속에서도 유효하다.
 *   신뢰되면 링크(qwClampRunLinkID)·좌표·INTERSECT_LEN 은 이미 SgmtMatch 가 계산해 DB 에 참고용으로
 *   남겨둔 값 그대로 두고 MATCH_STATUS 만 복원한다 — "다음 링크로 넘기는" bConnected 재매칭 경로와
 *   달리 원래 위치를 그대로 신뢰 회복시키는 것.
 *   실측 검증: 000376_20260819094414 seq20(전·후 방향 220.0° vs heading 244° → 24°),
 *   000376_20260821095239 seq44(239.8° vs 242° → 2.2°)·seq49(253.5° vs 258° → 4.5°)
 *   (2026-09-04 최정우 추가, 2026-09-05 최정우 수정 — 두 호출부 공용 헬퍼로 분리, 사용자 지시)
*/
size_t CRawLogWorker::BridgeClampRunByTrajectory(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		double dfNextMatchX, double dfNextMatchY, const sRawLogInfo& stRawLogInfo,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates)
{
	if ((pstSession == nullptr) || (pvtUpdates == nullptr) || !pstSession->bClampRunEntryValid)
		return 0;
	if (pstSession->vtClampRunUpdateIdx.empty()
		|| (pstSession->vtClampRunUpdateIdx.size() != pstSession->vtClampRunRawLogInfo.size()))
		return 0;

	POINT stEntryPoint, stNextPoint;
	stEntryPoint.dfX = pstSession->dfClampRunEntryX;  stEntryPoint.dfY = pstSession->dfClampRunEntryY;
	stNextPoint.dfX = dfNextMatchX;  stNextPoint.dfY = dfNextMatchY;

	// 전·후가 사실상 같은 점(정지에 가까움)이면 방향 자체가 노이즈라 판단 보류
	if (HaversineMeters(stEntryPoint, stNextPoint) < MM_CALC_MIN_DIST)
		return 0;

	sint16 nTrajBearing = m_cGISUtil.GetDirAngleDegree(stEntryPoint, stNextPoint);
	size_t nBridged = 0;
	for (size_t i = 0; i < pstSession->vtClampRunUpdateIdx.size(); ++i)
	{
		size_t idx = pstSession->vtClampRunUpdateIdx[i];
		if (idx >= pvtUpdates->size()) continue;

		sint16 nRawHeading = pstSession->vtClampRunRawLogInfo[i].nAngle;
		if (nRawHeading < 0) continue;					// heading 없음 — 검증 불가, 건드리지 않음(SKIP 유지)

		sint16 nDiff = m_cGISUtil.GetAngleDiff(nTrajBearing, nRawHeading);
		if (abs(nDiff) > MM_CLAMP_HEADING_MAX_DIFF) continue;	// 방향 안 맞음 — SKIP 유지

		(*pvtUpdates)[idx].strMatchStatus = "1";
		++nBridged;
	}

	if (nBridged > 0)
	{
		LOGFMTW("[#%02d] clamp-low-conf %zu/%zu-tick trajectory-direction bridge!"
			"device=[%s] trip_id=[%s] link=[%llu] traj_bearing=[%d] "
			"(original position restored, charge not retroactively processed)",
			nThreadId, nBridged, pstSession->vtClampRunUpdateIdx.size(), stRawLogInfo.szDeviceKey,
			stRawLogInfo.szTripID, static_cast<unsigned long long>(pstSession->qwClampRunLinkID),
			static_cast<int>(nTrajBearing));
	}
	return nBridged;
}

/**
 * @brief 보류(pending) 중인 1틱 지연 행을 확정(commit) — 반대편 짝 링크 1틱 오매칭 보정 + 과금
 *   함수 호출 + rawgps_update 큐잉
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in,out] pstSession 배치 임시 세션 — bHasPendingCommit=false 로 소비
 * @param[in] bHasNextLinkID 보정판단용 "다음" 확정 링크 존재 여부(false=보정 시도 안 함)
 * @param[in] qwNextLinkID 보정판단용 "다음" 확정 링크 ID
 * @param[out] pvtUpdates rawgps_update bulk UPDATE 대상 행 목록
 * @param[out] pvtChargeInserts charge_insert bulk INSERT 대상 행 목록
 * @return void
 * @remark 보류 행이 없으면(bHasPendingCommit=false) 아무 것도 안 하고 반환.
 *   보정 조건: 보류 행이 bReverseSuspect(역행의심)이고, 보류 행의 링크가 마지막으로 신뢰
 *   커밋된 링크(qwLastConfirmedLinkID)와 다르며, "다음" 확정 링크가 다시 그 마지막 신뢰
 *   링크로 돌아왔을 때 — 즉 "역행의심으로 다른 링크에 1틱 튀었다가 바로 다음 GPS에서 직전
 *   링크로 복귀"하는 패턴이면 GPS 노이즈로 판단해 MATCH_STATUS=SKIP(미과금) 처리한다.
 *   좌표·MATCH_LINK_ID 자체는 다른 저신뢰 SKIP(bClampLowConf 등)과 동일하게 참고용으로 DB에
 *   그대로 남긴다(무엇으로 오매칭됐었는지 추적 가능하도록, 값을 지어내 덮어쓰지 않음).
 *   과금 함수(ProcessOpenGateCharge 등)는 "직전 매칭 위치·시각"을 세션에서 읽어 이동거리·
 *   속도를 계산하는데, 그 값(dfLastMatchX/Y 등)은 RunMapMatch 가 매 행마다 실시간으로 이미
 *   최신 위치로 전진시켜놨으므로, 보류 행 처리 "당시" 스냅샷(dfPendingPrevMatchX/Y 등)으로
 *   잠깐 바꿔치기한 후 호출하고 끝나면 즉시 원복한다 — 그렇지 않으면 몇 틱 지난 최신 위치를
 *   "직전 위치"로 오인해 이동거리·속도가 틀어진다. 세션의 다른 과금 상태(bInClosedRoad 등)는
 *   건드리지 않음 (2026-08-21 최정우 추가)
*/
/**
 * @brief 트립 시작 판정 구간 동안 run 에 이번 tick 을 기록한다 (2026-10-04 최정우 추가, 사용자 확정)
 * @remark 같은 tick 이 다시 오면 누적값만 갱신한다. 8 tick 을 넘으면 더 쌓지 않는다(판정 구간은
 *   nOppStreakMax tick 남짓이라 충분하다).
*/
static void RecordRunHeadTrail(ZONE_RUN_SESSION& stRun, uint32 dwSeq, time_t dtGps,
	double dfX, double dfY, uint64 qwLinkID, bool bCoveredOnly)
{
	if ((stRun.dwEntryGpsSeq == 0) || (dwSeq < stRun.dwEntryGpsSeq))
		return;
	// bCoveredOnly — 이 run 이 실제로 덮은 tick(dwLastInZoneGpsSeq 이하)만 기록한다. 보류 run·병합 이월은
	//   더 나아가지 않는데, 이후 tick 을 기록하면 그 tick 을 "유효한 새 출발"로 오인해 강등 tick 으로만 된
	//   run 을 버리지 못한다(실측 000376_20260819140532: 진 쪽 링크 run 이 보류된 채 남아, 경계 교차 보정이
	//   만든 엉뚱한 교차점 ~ 접촉 시작점 직선 87.9m 가 폴리곤 안 거리에 더해짐). 접촉 이월은 매 tick 전진하므로
	//   false 로 부른다.
	if (bCoveredOnly && (dwSeq > stRun.dwLastInZoneGpsSeq))
		return;
	const int n = stRun.nHeadTrail;
	if ((n > 0) && (stRun.adwHeadSeq[n - 1] == dwSeq))
	{
		stRun.adfHeadAccum[n - 1] = stRun.dfAccumDistM;
		return;
	}
	if (n >= 8)
		return;
	stRun.adwHeadSeq[n] = dwSeq;
	stRun.adfHeadAccum[n] = stRun.dfAccumDistM;
	stRun.adtHeadTime[n] = dtGps;
	stRun.adfHeadX[n] = dfX;
	stRun.adfHeadY[n] = dfY;
	stRun.aqwHeadLink[n] = qwLinkID;
	stRun.nHeadTrail = n + 1;
}

/**
 * @brief 출발 후보 해소로 SKIP 강등된 tick 에서 시작한 run 의 출발점을 첫 유효 tick 으로 다시 잡는다
 *   (2026-10-04 최정우 추가, 사용자 확정 "일반도로 출발 = 일반도로·구간단속에 매칭된 첫 tick")
 * @param[in,out] stRun 대상 run
 * @param[in] setLoserSeq 강등된 tick 의 GPS_SEQ
 * @param[in] qwLoserLink 진 쪽 링크(경유 링크 목록에서 뺀다)
 * @param[out] pdfCut 잘라낸 누적거리(m)
 * @return 1=다시 잡음, 0=대상 아님(출발 tick 이 강등 대상이 아님), -1=유효 tick 없음(호출측이 버림)
 * @remark 새 출발 = 기록 중 강등되지 않은 첫 tick. 거리는 그 tick 처리 직후 누적값을 뺀다 — 강등
 *   tick 들과 그 tick 까지의 구간(튄 좌표 포함)이 통째로 빠진다. 진입 시각·좌표·링크·GPS_SEQ 도 그
 *   tick 값으로 바꾼다. 실측 000376_20260819140532: seq4·5(2040425401, 정확도 53~180m 출발 표류)가
 *   후보 해소로 SKIP 됐는데 run·보류·접촉 이월이 seq4 부터 남아 일반도로 4~53 이 1,090m 로 잡혔다
 *   (첫 매칭 tick seq6 기준 재계산 978.7m).
*/
static int ReanchorRunHead(ZONE_RUN_SESSION& stRun, const set<uint32>& setLoserSeq, uint64 qwLoserLink,
	double *pdfCut)
{
	if (pdfCut != nullptr) *pdfCut = 0.0;
	if (setLoserSeq.find(stRun.dwEntryGpsSeq) == setLoserSeq.end())
		return 0;
	for (int i = 0; i < stRun.nHeadTrail; ++i)
	{
		if (setLoserSeq.find(stRun.adwHeadSeq[i]) != setLoserSeq.end())
			continue;
		const double dfCut = stRun.adfHeadAccum[i];
		stRun.dfAccumDistM -= dfCut;
		if (stRun.dfAccumDistM < 0.0) stRun.dfAccumDistM = 0.0;
		stRun.dwEntryGpsSeq = stRun.adwHeadSeq[i];
		stRun.dtEntryTime = stRun.adtHeadTime[i];
		stRun.dfEntryX = stRun.adfHeadX[i];
		stRun.dfEntryY = stRun.adfHeadY[i];
		stRun.qwEntryLinkID = stRun.aqwHeadLink[i];
		if (stRun.dtLastInZoneTime < stRun.dtEntryTime) stRun.dtLastInZoneTime = stRun.dtEntryTime;
		if (stRun.dwLastInZoneGpsSeq < stRun.dwEntryGpsSeq) stRun.dwLastInZoneGpsSeq = stRun.dwEntryGpsSeq;
		// 남은 기록도 새 출발 기준으로 당긴다(같은 run 이 다시 해소될 일은 없지만 값 일관성 유지)
		int k = 0;
		for (int j = i; j < stRun.nHeadTrail; ++j, ++k)
		{
			stRun.adwHeadSeq[k] = stRun.adwHeadSeq[j];
			stRun.adfHeadAccum[k] = stRun.adfHeadAccum[j] - dfCut;
			stRun.adtHeadTime[k] = stRun.adtHeadTime[j];
			stRun.adfHeadX[k] = stRun.adfHeadX[j];
			stRun.adfHeadY[k] = stRun.adfHeadY[j];
			stRun.aqwHeadLink[k] = stRun.aqwHeadLink[j];
		}
		stRun.nHeadTrail = k;
		if (qwLoserLink != 0)
			stRun.vtRunLinks.erase(remove(stRun.vtRunLinks.begin(), stRun.vtRunLinks.end(), qwLoserLink),
				stRun.vtRunLinks.end());
		if (pdfCut != nullptr) *pdfCut = dfCut;
		return 1;
	}
	return -1;
}

void CRawLogWorker::CommitPendingRow(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		bool bHasNextLinkID, uint64 qwNextLinkID,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		double dfNextMatchX, double dfNextMatchY)
{
	if ((pstSession == nullptr) || !pstSession->bHasPendingCommit)
		return;

	const sRawLogInfo stRawLogInfo = pstSession->stPendingRawLogInfo;			// 지역 복사(commit 중 세션 필드 재사용 대비)
	MATCH_LINK_INFO stMatchLinkInfo = pstSession->stPendingMatchLinkInfo;		// 지역 복사(보정 시 nFinalStatus 만 별도 변수로 바꿈)
	// 보류 중이던 이 행 자체의 TRIP_EVENT=END 신뢰 여부 — ProcessRawLog() 가 처음 이 행을 봤을 때
	//   스퓨리어스로 판정해 보류 상태로 남겨뒀을 수 있는데, 여기서 다시 stRawLogInfo.nTripEvent 를
	//   직접 보면 그 판정이 무시되고 원래 막으려던 버그가 재발한다 — IsTrustedTripEnd() 로 동일하게
	//   재판정(dtLastGpsEventTime 은 이미 더 이후 틱까지 반영돼 있어 오히려 더 안전) (2026-08-25 최정우 추가)
	const bool bTrustedTripEnd = IsTrustedTripEnd(stRawLogInfo, *pstSession);
	sint16 nFinalStatus = pstSession->nPendingFinalStatus;
	bool bMatched = (nFinalStatus == MATCH_STATUS_MATCHED);

	// ── 트립 첫 점(BEGIN) 반대방향 오매칭 보정 (2026-08-22 최정우 추가) ──
	//   BEGIN 은 heading 을 무시하고 거리만으로 판정한다(bIgnoreHeading, 2026-08-19). 왕복분리
	//   도로는 짝 링크가 10m 남짓 옆에 나란히 있어 거리차가 무의미한데도 더 가까운 쪽이 뽑힌다.
	//   실측 trip 000376_20260819094414 seq1 — 정답 2040426801 이 7.68m, 반대방향 2040426701 이
	//   4.16m 라 반대방향이 채택됐다(heading 276° vs 채택 링크 방위각 99°, 177° 어긋남).
	//
	//   첫 점 heading 으로 고치려던 접근은 버렸다 — 전국 21트립 실측에서 첫 점 heading 이
	//   이후 점들의 평균 방향과 41° 어긋나 신뢰할 수 없고, 애초에 BEGIN 후보 목록은
	//   그리드 셀당 최선 1건만 담아(GridSgmtMapMatch) 짝 링크가 목록에 오르지도 않는다.
	//
	//   대신 "이미 확정된 다음 점의 링크"를 편향 기준으로 BEGIN 을 다시 태운다. 그 링크가
	//   보류 행 링크의 왕복분리 짝(qwOppositeLinkID)일 때만 — 즉 첫 점이 건너편에 붙은 것이
	//   분명할 때만 — 재매칭한다. 진입 가능한 다른 링크로 넘어간 정상 전이는 건드리지 않는다.
	//   (전국 21트립 검증: 보정 대상 1건, 정상 전이 9건은 미개입)
	if (bMatched && (pstSession->qwLastConfirmedLinkID == 0)
		&& bHasNextLinkID && (qwNextLinkID != stMatchLinkInfo.qwLinkID)
		&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcProcessManager != nullptr))
	{
		PLINK_INFO pstPendingLink = m_stConfig.pcDataLoader->GetLinkInfo(stMatchLinkInfo.qwLinkID);
		const bool bOppositeSlip = (pstPendingLink != nullptr) && (pstPendingLink->qwOppositeLinkID == qwNextLinkID);
		// [2026-10-04 최정우 추가 — 사용자 확정(권장안)] **노드 부근 인접 링크 오매칭** 도 같은 방식으로
		//   고친다. 첫 점이 두 링크가 만나는 노드 근처에 찍히면 BEGIN(거리만 봄)이 실제 출발 링크가
		//   아니라 그 노드에 붙은 **앞 링크의 끝**(또는 같은 노드에서 갈라지는 다른 링크의 시작)을
		//   고른다. 다음 확정 링크가 바로 그 노드에서 시작하면 첫 점은 그 링크 위로 보는 것이 맞다.
		//   조건: 보류 링크의 시작·끝 노드 중 하나 == 다음 확정 링크의 시작 노드, 첫 점 매칭좌표가 그
		//   노드에서 MM_BEGIN_NODE_SLIP_M 이내. 결과 링크가 다음 확정 링크일 때만 채택한다.
		//   실측(시나리오, sim_truth 대조):
		//     980013_20261004230000 seq1 2040426501(끝) -> 정답 2040426101 — FROM_ID·과다 거리
		//     980003/980012 seq1 2040423802(시작) -> 정답 2040423501 — 첫 행 442~447m/6tick(88km/h)
		//     980008_20261004180000 seq1 2040423801(끝, 폐쇄식) -> 정답 2040423501 — 폐쇄식 N/3(21) 1tick
		// 되돌리는 법: bNodeSlip 을 false 로 고정
		static const double MM_BEGIN_NODE_SLIP_M = 5.0;
		bool bNodeSlip = false;
		uint64 qwSlipNodeID = 0;
		// 트립의 **첫 매칭 tick** 에만 적용한다(경로 기록이 아직 비어 있을 때). 위 공통 조건
		//   qwLastConfirmedLinkID == 0 은 트립 시작 후보 판정 동안 몇 tick 더 이어지는데, 그 사이 tick 은
		//   이미 주행 중이라 노드 부근이어도 정답일 수 있다 — 첫 적용에서 seq4 가 정답 2040425302(경계
		//   링크 끝)에서 2040425301 로 잘못 옮겨졌다(sim_truth 대조, 980002·980004·980013)
		if (!bOppositeSlip && (pstPendingLink != nullptr) && pstSession->vtTripPathLinks.empty())
		{
			PLINK_INFO pstNextLink = m_stConfig.pcDataLoader->GetLinkInfo(qwNextLinkID);
			if (pstNextLink != nullptr)
			{
				const uint64 qwN = pstNextLink->qwStNodeID;
				const bool bAtEnd = (pstPendingLink->qwEdNodeID == qwN);
				const bool bAtStart = (pstPendingLink->qwStNodeID == qwN);
				if ((qwN != 0) && (bAtEnd || bAtStart))
				{
					POINT stNode, stCur;
					stNode.dfX = static_cast<double>(pstNextLink->dwStNodeX) / 360000.0;
					stNode.dfY = static_cast<double>(pstNextLink->dwStNodeY) / 360000.0;
					stCur.dfX = stMatchLinkInfo.dfMatchX;
					stCur.dfY = stMatchLinkInfo.dfMatchY;
					// [2026-10-04 최정우 추가, 사용자 확정(권장안)] 첫 점 링크가 **폐쇄식·구간단속 구역 링크**면 기준을
					//   MM_BEGIN_NODE_SLIP_GATE_M(10m)까지 넓힌다. 그 오매칭은 실제로 지나지 않은 게이트형 구역에 1tick
					//   가짜 행(진입게이트 미확인 N/3)을 만들기 때문이다 — 실측(시나리오, sim_truth 대조)
					//   980015_20261005010000 seq1: 2040423801(폐쇄식 RL-Z00005 끝) 매칭점이 노드에서 6.7m 라 5m 기준
					//   밖 → 폐쇄 1~1 6m N/3(21). 일반 링크(980001·980007 seq1, 6.7~7.2m)는 종전 5m 그대로 둔다
					//   (넓혔을 때 오교정 이력, 정본 10절). 다음 확정 링크가 그 노드에서 시작하는 조건은 위와 같다.
					// 되돌리는 법: dfSlipLimitM 을 MM_BEGIN_NODE_SLIP_M 으로 고정
					static const double MM_BEGIN_NODE_SLIP_GATE_M = 10.0;
					double dfSlipLimitM = MM_BEGIN_NODE_SLIP_M;
					if (m_stConfig.pcChargeDataLoader != nullptr)
					{
						char szGateZone[20 + 1];
						if (m_stConfig.pcChargeDataLoader->GetClosedZoneRoadIdByLinkId(stMatchLinkInfo.qwLinkID, szGateZone, sizeof(szGateZone))
							|| m_stConfig.pcChargeDataLoader->GetSpeedZoneRoadIdByLinkId(stMatchLinkInfo.qwLinkID, szGateZone, sizeof(szGateZone)))
							dfSlipLimitM = MM_BEGIN_NODE_SLIP_GATE_M;
					}
					if (HaversineMeters(stNode, stCur) <= dfSlipLimitM)
					{
						bNodeSlip = true;
						qwSlipNodeID = qwN;
					}
				}
			}
		}
		if (bOppositeSlip || bNodeSlip)
		{
			MATCH_LINK_INFO stRematched;
			CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
			if (cPM.RematchBeginBiased(stRawLogInfo, qwNextLinkID, &stRematched)
				&& (stRematched.qwLinkID == qwNextLinkID))
			{
				LOGFMTW("[#%02d] begin %s corrected!device=[%s] trip_id=[%s] seq=[%u] "
					"link=[%llu] -> [%llu] (next_confirmed=[%llu] node=[%llu])",
					nThreadId, bOppositeSlip ? "opposite-link" : "node-slip",
					stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
					static_cast<unsigned long long>(stRematched.qwLinkID),
					static_cast<unsigned long long>(qwNextLinkID),
					static_cast<unsigned long long>(qwSlipNodeID));

				// [2026-09-23 최정우 추가 — 사용자 지적] **경로 누적에 이미 들어간 폐기 링크도 교체한다.**
				//   보정은 "그 링크가 틀렸다"는 판정인데, 종전에는 stMatchLinkInfo 만 고치고
				//   vtTripPathLinks 는 그대로 뒀다. 그러면 트립 마감 때 FillUncoveredLinkRows 가
				//   그 폐기 링크를 "아무도 안 덮은 링크"로 보고 **일반도로 행으로 복구**한다 —
				//   실측 000998_20260917090000 / 000983_20250903153213 trip_seq1:
				//   역방향 링크 2040424101(89m)이 정차 중(실측 0km/h)인데 과금됐다.
				//   실제 주행 링크는 보정 결과(2040424201)이고 그쪽은 면제 행이 이미 덮는다.
				const uint64 qwDropped = stMatchLinkInfo.qwLinkID;
				if (qwDropped != stRematched.qwLinkID)
				{
					for (size_t ti = pstSession->vtTripPathLinks.size(); ti-- > 0; )
					{
						if (pstSession->vtTripPathLinks[ti].qwLinkID != qwDropped) continue;
						pstSession->vtTripPathLinks[ti].qwLinkID = stRematched.qwLinkID;
						LOGFMTI("[#%02d] begin opposite-link path entry fixed!device=[%s] trip_id=[%s] "
							"idx=[%d] link=[%llu] -> [%llu]",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
							static_cast<int>(ti),
							static_cast<unsigned long long>(qwDropped),
							static_cast<unsigned long long>(stRematched.qwLinkID));
						break;
					}
				}
				stMatchLinkInfo = stRematched;
			}
		}
	}

	// ── 왕복분리 반대편 링크 N틱 연속 오매칭 보정 (2026-08-24 최정우 확장) ──
	//   기존엔 "짝 링크로 1틱만 튀었다가 바로 다음 틱에 복귀"만 잡았다. 짝 링크로 튀었다가
	//   opp_streakmax 틱 이내에 원래 링크로 돌아오면, 그 사이 커밋된 MATCH_STATUS 를 한꺼번에
	//   SKIP 으로 재기록한다(실측 000376_20260819140532 G5·G6, 2틱 연속 — 저속 구간은 방위각을
	//   안 써서(가중치 0) 왕복분리 반대편이 물리적으로 붙어있으면 GPS 튐만으로 여러 틱 연속
	//   반대편에 붙을 수 있음). qwLastConfirmedLinkID 는 매 틱 갱신돼버려 스트릭 기준으로 못 쓰므로,
	//   스트릭 시작 시점의 진짜 확정 링크를 qwOppStreakAnchorLinkID 에 별도로 고정해둔다.
	//   과금 함수는 아래 공통 흐름에서 매 틱 정상 호출한다(보정 여부와 무관) — 확정 전에 과금을
	//   보류하는 방식도 시도했으나, 정상 주행 중 반대편을 스치기만 하고 다른 링크로 넘어가는(오매칭
	//   아닌) 흔한 경우까지 매번 과금을 놓치는 것으로 실측 확인돼(트립 1개에 4건) 기존 정책(원래
	//   1틱 보정도 항상 즉시 과금을 확정해왔음)을 그대로 유지 — DB 표시(match_status)만 사후
	//   재기록한다.
	if (bMatched && (m_stConfig.pcDataLoader != nullptr))
	{
		uint64 qwAnchor = (pstSession->qwOppStreakAnchorLinkID != 0)
			? pstSession->qwOppStreakAnchorLinkID : pstSession->qwLastConfirmedLinkID;

		if ((qwAnchor != 0) && (stMatchLinkInfo.qwLinkID == qwAnchor) && !pstSession->vtOppStreakUpdateIdx.empty())
		{
			// 원래 링크로 복귀 확정 — 스트릭에 쌓인 틱들의 DB 기록만 SKIP 으로 재기록
			//   (과금은 이미 그 시점 매칭대로 처리됐으므로 되돌리지 않는다)
			for (size_t i = 0; i < pstSession->vtOppStreakUpdateIdx.size(); ++i)
			{
				size_t idx = pstSession->vtOppStreakUpdateIdx[i];
				if (idx >= pvtUpdates->size()) continue;
				(*pvtUpdates)[idx].strMatchStatus = "3";
				(*pvtUpdates)[idx].strMatchLat.clear();
				(*pvtUpdates)[idx].strMatchLon.clear();
				(*pvtUpdates)[idx].strMatchLinkId.clear();
				(*pvtUpdates)[idx].strIntersectLen.clear();
			}
			LOGFMTW("[#%02d] opposite-link %zu-tick flip DB-corrected!device=[%s] trip_id=[%s] "
				"anchor_link=[%llu] (charge already processed as-matched, not reverted)",
				nThreadId, pstSession->vtOppStreakUpdateIdx.size(), stRawLogInfo.szDeviceKey,
				stRawLogInfo.szTripID, static_cast<unsigned long long>(qwAnchor));
			pstSession->qwOppStreakAnchorLinkID = 0;
			pstSession->vtOppStreakUpdateIdx.clear();
		}
		else if ((qwAnchor != 0) && (stMatchLinkInfo.qwLinkID != qwAnchor))
		{
			PLINK_INFO pstAnchorLink = m_stConfig.pcDataLoader->GetLinkInfo(qwAnchor);
			bool bIsOpposite = (pstAnchorLink != nullptr) && (pstAnchorLink->qwOppositeLinkID != 0)
				&& (pstAnchorLink->qwOppositeLinkID == stMatchLinkInfo.qwLinkID);

			if (bIsOpposite && (static_cast<int>(pstSession->vtOppStreakUpdateIdx.size()) < m_stConfig.nOppStreakMax))
			{
				if (pstSession->qwOppStreakAnchorLinkID == 0)
					pstSession->qwOppStreakAnchorLinkID = qwAnchor;
				// 이번 틱의 pvtUpdates 인덱스는 AppendUpdateRow 호출 직후에 기록한다
			}
			else
			{
				// 반대편이 아니거나 스트릭 한도 초과 — 진짜 전이로 인정, 스트릭만 해제(DB 재기록 없음)
				pstSession->qwOppStreakAnchorLinkID = 0;
				pstSession->vtOppStreakUpdateIdx.clear();
			}
		}
	}

	// ── 같은 링크 ambiguous-reverse SKIP 브릿지 해소 (2026-08-28 최정우 추가) ──
	//   "다음"(bHasNextLinkID) 확정 링크가 진행 중인 런의 링크와 같으면 링크 이탈이 없었다는
	//   뜻이므로 그 사이 쌓인 SKIP 을 MATCHED 로 소급 재기록한다. 다르면 진짜 이탈로 보고 런만
	//   버린다(SKIP 유지, DB 재기록 없음) — pstSession 자체(보류 중이던 행)의 매칭 성공 여부와는
	//   무관하게 "다음" 링크 정보만으로 판정
	if (bHasNextLinkID && (pstSession->qwAmbigReverseRunLinkID != 0) && !pstSession->vtAmbigReverseRunIdx.empty())
	{
		if (qwNextLinkID == pstSession->qwAmbigReverseRunLinkID)
		{
			for (size_t i = 0; i < pstSession->vtAmbigReverseRunIdx.size(); ++i)
			{
				size_t idx = pstSession->vtAmbigReverseRunIdx[i];
				if (idx >= pvtUpdates->size()) continue;
				(*pvtUpdates)[idx].strMatchStatus = "1";
			}
			LOGFMTW("[#%02d] ambiguous-reverse %zu-tick same-link bridge!device=[%s] trip_id=[%s] "
				"link=[%llu] -> MATCHED (charge not retroactively processed)",
				nThreadId, pstSession->vtAmbigReverseRunIdx.size(), stRawLogInfo.szDeviceKey,
				stRawLogInfo.szTripID, static_cast<unsigned long long>(pstSession->qwAmbigReverseRunLinkID));
		}
		pstSession->qwAmbigReverseRunLinkID = 0;
		pstSession->vtAmbigReverseRunIdx.clear();
	}

	// ── 경계 클램프(bClampLowConf) SKIP 브릿지 해소 (2026-08-28 최정우 추가) ──
	//   "다음" 확정 링크가 클램프 런의 링크와 다르지만 실제로 인접(1-hop 직결)하면, 각 tick 을
	//   그 링크로 편향 재매칭(RematchBeginBiased — BEGIN 반대방향 오매칭 보정과 동일 메커니즘)해
	//   재매칭 결과가 정말 그 링크로 떨어지는 경우만 링크·좌표·INTERSECT_LEN까지 소급 재기록한다.
	//   다음 링크가 클램프 런과 같으면(원래 링크로 복귀 확정) 브릿지 대상이 아니므로 그대로 SKIP
	//   유지 — bAmbiguousReverse 브릿지가 이미 그 경우를 커버함. 인접하지 않으면(진짜 별개 구간)
	//   재매칭 시도 없이 런만 버린다
	if (bHasNextLinkID && (pstSession->qwClampRunLinkID != 0) && !pstSession->vtClampRunUpdateIdx.empty()
		&& (qwNextLinkID != pstSession->qwClampRunLinkID)
		&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcProcessManager != nullptr))
	{
		PLINK_INFO pstClampLink = m_stConfig.pcDataLoader->GetLinkInfo(pstSession->qwClampRunLinkID);
		PLINK_INFO pstNextLink = m_stConfig.pcDataLoader->GetLinkInfo(qwNextLinkID);
		bool bConnected = (pstClampLink != nullptr) && (pstNextLink != nullptr)
			&& ((pstClampLink->qwEdNodeID == pstNextLink->qwStNodeID)
				|| (pstClampLink->qwEdNodeID == pstNextLink->qwEdNodeID)
				|| (pstClampLink->qwStNodeID == pstNextLink->qwStNodeID)
				|| (pstClampLink->qwStNodeID == pstNextLink->qwEdNodeID));

		// ── 경로 기반 확장 (2026-09-06 최정우 추가, 사용자 지시) ──
		//   기존엔 "클램프 링크와 다음 확정 링크가 1-hop 직결"일 때만, 그리고 재매칭 결과가 다음
		//   확정 링크와 "정확히 일치"할 때만 인정했다. 그러면 짧은 링크가 사이에 끼어 여러 홉
		//   떨어진 경우를 통째로 놓친다 — 실측 000376_20260819093337 seq101·102: 클램프 링크
		//   2040423701(374.6m) 에서 다음 확정 2040424801 까지 2040423702(7.9m)·2040424802(9.5m)
		//   두 짧은 링크를 거쳐 3홉이고, 정답은 그 "중간" 링크 2040424802 였다. 1-hop 조건에서
		//   걸러지고, 통과했더라도 결과가 다음 확정 링크가 아니라 중간 링크라 거부됐을 것이다.
		//   그래서 (1) 연결 판정을 경로탐색(FindLinkPathBounded)으로 넓히고 (2) 재매칭 결과가
		//   그 경로 위 어느 링크든이면 인정하도록 바꾼다. 경로 위 링크만 허용하므로 "실제로 지나간
		//   길"이라는 근거는 그대로 유지된다 — 다음 확정 링크가 그쪽으로 이어지지 않으면 애초에
		//   경로가 안 나온다. 대상은 이미 SKIP 으로 판정된 클램프 tick 뿐이라 정상 매칭은 무영향.
		static const int MM_CLAMP_BRIDGE_MAX_HOPS = 6;
		vector<uint64> vtClampPath;
		bool bPathFound = false;
		if (!bConnected && (pstClampLink != nullptr) && (pstNextLink != nullptr))
		{
			bPathFound = FindLinkPathBounded(pstSession->qwClampRunLinkID, qwNextLinkID,
				MM_CLAMP_BRIDGE_MAX_HOPS, &vtClampPath) && (vtClampPath.size() > 2);
		}

		if (bConnected || bPathFound)
		{
			CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
			size_t nBridged = 0;
			for (size_t i = 0; i < pstSession->vtClampRunUpdateIdx.size(); ++i)
			{
				size_t idx = pstSession->vtClampRunUpdateIdx[i];
				if (idx >= pvtUpdates->size()) continue;

				MATCH_LINK_INFO stRematched;
				if (!cPM.RematchBeginBiased(pstSession->vtClampRunRawLogInfo[i], qwNextLinkID, &stRematched))
					continue;			// 재매칭 자체가 실패 — 이 tick 은 건드리지 않음(SKIP 유지)

				// 결과가 다음 확정 링크이거나, 경로탐색으로 찾은 그 사이 링크 중 하나여야 인정한다.
				//   경로 밖으로 떨어지면 "지나간 길"이라는 근거가 없으므로 종전대로 SKIP 유지
				//   (2026-09-06 최정우 수정, 사용자 지시)
				bool bOnPath = (stRematched.qwLinkID == qwNextLinkID);
				if (!bOnPath && bPathFound)
				{
					for (size_t g = 1; g < vtClampPath.size(); ++g)
					{
						if (vtClampPath[g] == stRematched.qwLinkID) { bOnPath = true; break; }
					}
				}
				if (!bOnPath)
					continue;

				int nNewIntersectLen = CalcIntersectLen(pstSession->vtClampRunRawLogInfo[i],
					stRematched.dfMatchX, stRematched.dfMatchY);
				char szMatchLat[32], szMatchLon[32], szIntersectLen[16], szMatchLinkId[24];
				snprintf(szMatchLat, sizeof(szMatchLat), "%.06lf", stRematched.dfMatchY);
				snprintf(szMatchLon, sizeof(szMatchLon), "%.06lf", stRematched.dfMatchX);
				snprintf(szIntersectLen, sizeof(szIntersectLen), "%d", nNewIntersectLen);
				// 다음 확정 링크가 아니라 **실제 재매칭된 링크**를 기록한다 — 경로 중간 링크로
				//   떨어질 수 있으므로 (2026-09-06 최정우 수정, 사용자 지시)
				snprintf(szMatchLinkId, sizeof(szMatchLinkId), "%llu",
					static_cast<unsigned long long>(stRematched.qwLinkID));

				(*pvtUpdates)[idx].strMatchStatus = "1";
				(*pvtUpdates)[idx].strMatchLat = szMatchLat;
				(*pvtUpdates)[idx].strMatchLon = szMatchLon;
				(*pvtUpdates)[idx].strIntersectLen = szIntersectLen;
				(*pvtUpdates)[idx].strMatchLinkId = szMatchLinkId;
				++nBridged;
			}
			if (nBridged > 0)
			{
				LOGFMTW("[#%02d] clamp-low-conf %zu/%zu-tick adjacent-link bridge!device=[%s] trip_id=[%s] "
					"link=[%llu] -> [%llu] (rematched, charge not retroactively processed)",
					nThreadId, nBridged, pstSession->vtClampRunUpdateIdx.size(), stRawLogInfo.szDeviceKey,
					stRawLogInfo.szTripID, static_cast<unsigned long long>(pstSession->qwClampRunLinkID),
					static_cast<unsigned long long>(qwNextLinkID));
			}
		}
		else
		{
			// bConnected 가 아니어도 "전·후 확정좌표 궤적 방향"으로는 신뢰 회복이 가능하다 —
			//   BridgeClampRunByTrajectory() 주석 참고 (2026-09-04 최정우 추가, 사용자 지시.
			//   2026-09-05 최정우 수정 — 같은 링크 복귀 분기와 공용이라 헬퍼로 분리)
			BridgeClampRunByTrajectory(nThreadId, pstSession, dfNextMatchX, dfNextMatchY,
				stRawLogInfo, pvtUpdates);
		}
		pstSession->qwClampRunLinkID = 0;
		pstSession->vtClampRunUpdateIdx.clear();
		pstSession->vtClampRunRawLogInfo.clear();
		pstSession->bClampRunEntryValid = false;
	}
	else if (bHasNextLinkID && (pstSession->qwClampRunLinkID != 0) && (qwNextLinkID == pstSession->qwClampRunLinkID))
	{
		// 원래 링크로 복귀 확정 — 링크 이탈이 없었다는 뜻이므로 링크를 바꿀 이유는 없다. 다만
		//   "그러니 SKIP 그대로 둔다"는 종전 처리는 틀렸다: 애초에 이 SKIP 은 링크를 잘못 골라서가
		//   아니라 GPS↔매칭점 거리(INTERSECT_LEN)가 MM_CLAMP_SKIP_LEN 을 넘어서 붙은 것이라,
		//   앞뒤가 같은 링크로 확정됐다는 사실 자체가 오히려 그 사이 tick 도 그 링크 위였다는
		//   방증이다. 종전 주석은 "bAmbiguousReverse 브릿지가 이 경우를 커버한다"고 봤지만 그쪽은
		//   qwAmbigReverseRunLinkID 런만 보므로 클램프 런은 커버하지 않아, 같은 링크 위의 클램프
		//   SKIP 이 어느 브릿지에도 안 걸리는 사각지대였다.
		//   실측 000376_20260821095239 seq44·49 — 앞뒤 모두 2040424401 MATCHED 인데 그 사이만
		//   INTERSECT_LEN 10.7m·11.6m(임계 10.0m)로 SKIP. heading 구제(bClampTrustedByHeading)는
		//   속도 20km/h 이상을 요구해 18·19km/h 인 이 두 tick 을 아깝게 배제했다. 전·후 궤적
		//   방향으로 재보면 차이가 2.2°·4.5°(임계 30°)로 명백한 정상 주행이다.
		//   bConnected 실패 경로와 같은 검증을 쓴다 — 속도 게이트에 기대지 않아 저속에서도 유효
		//   (2026-09-05 최정우 수정, 사용자 지시)
		BridgeClampRunByTrajectory(nThreadId, pstSession, dfNextMatchX, dfNextMatchY,
			stRawLogInfo, pvtUpdates);
		pstSession->qwClampRunLinkID = 0;
		pstSession->vtClampRunUpdateIdx.clear();
		pstSession->vtClampRunRawLogInfo.clear();
		pstSession->bClampRunEntryValid = false;
	}

	// 이번 틱이 스트릭에 새로 편입되는지 — 위 블록 판정 직후, 과금 호출로 qwLastConfirmedLinkID 가
	//   갱신되기 "전"에 미리 계산해둔다 (2026-08-24 최정우 추가)
	const bool bJoinedOppStreak = (pstSession->qwOppStreakAnchorLinkID != 0)
		&& (stMatchLinkInfo.qwLinkID != pstSession->qwOppStreakAnchorLinkID);

	// ── 트립 시작(또는 장시간 SKIP 후) 첫 매칭이 왕복분리 어느 쪽인지 불확실한 구간 보정
	//   (2026-08-24 최정우 추가, 실측 21트립 중 3건꼴로 재현 확인: 000376_20260819140532 G5·G6 등) ──
	//   진짜 앵커가 아직 없을 때 첫 매칭 성공 링크(A)를 곧바로 신뢰하지 않고 "잠정 후보"로만 잡는다.
	//   왕복분리 반대편(B)이 뒤이어 나타나면 둘 다 후보로 계속 추적하다가, ①한쪽이 opp_streakmax
	//   틱에 먼저 도달하거나 ②A/B 어느 쪽도 아닌 제3의 링크가 나타나면 그 시점까지 더 많이 쌓인
	//   쪽을 승자로 확정하고, 진 쪽의 DB 기록만 SKIP 으로 재기록한다(과금은 그 시점 매칭대로 항상
	//   정상 처리 — 위 반대편 스트릭 보정과 동일 정책). 승자가 정해지면 qwLastConfirmedLinkID 는
	//   아래 공통 흐름(과금 처리 뒤 무조건 대입)에서 자연히 이번 틱 링크로 앵커가 되므로 여기서
	//   별도로 세팅하지 않는다.
	int nJoinedStartCand = 0;			// 0=해당없음, 1=후보A 편입, 2=후보B 편입
	if (bMatched && (m_stConfig.pcDataLoader != nullptr)
		&& (pstSession->qwLastConfirmedLinkID == 0) && (pstSession->qwOppStreakAnchorLinkID == 0))
	{
		auto ResolveStartAmbiguity = [&](vector<size_t>& vtLoserIdx, uint64 qwLoserLink)
		{
			for (size_t i = 0; i < vtLoserIdx.size(); ++i)
			{
				size_t idx = vtLoserIdx[i];
				if (idx >= pvtUpdates->size()) continue;
				(*pvtUpdates)[idx].strMatchStatus = "3";
				(*pvtUpdates)[idx].strMatchLat.clear();
				(*pvtUpdates)[idx].strMatchLon.clear();
				(*pvtUpdates)[idx].strMatchLinkId.clear();
				(*pvtUpdates)[idx].strIntersectLen.clear();
			}
			// 패자 링크로 만들어진 **일반도로 과금 레코드도 함께 취소한다** (2026-09-07 최정우 수정,
			//   사용자 지시). 종전 정책은 "DB 기록만 SKIP 으로 재기록하고 과금은 그 시점 매칭대로
			//   유지"였는데, 그러면 **DB 상 매칭 실패인 좌표가 일반도로 과금 이력에 남는다** —
			//   실측 000376_20260819140532: seq4·5 가 2040425401 로 매칭돼 run 을 열었다가(로그
			//   node step entry(unregistered) seq=[4] trusted=[1]) 이 판정에서 그 링크가 패자로
			//   확정돼 DB 는 SKIP·좌표 삭제로 바뀌었는데, 이미 만들어진 27m 일반도로 레코드(4~5)는
			//   그대로 남았다. 주정차 폴리곤 안도 아니고 맵매칭도 실패이며 복구 대상도 아닌 좌표는
			//   과금 이력에 올라올 수 없다는 규칙에 어긋난다.
			//   취소 대상은 **이 배치에서 아직 INSERT 되지 않은** 행 중 CHARGE_TYPE=0 이고 FROM_ID 가
			//   패자 링크인 것으로 한정한다 — 다른 유형(게이트형·면제·주정차)은 구역 판정이 별도
			//   근거로 서므로 건드리지 않는다.
			//   레코드는 run 이 닫힐 때 만들어지므로 이 시점엔 아직 없다(실측 확인: 취소 시도
			//   dropped=0). 그래서 **아직 열려 있는 run 자체를 버린다** — 패자 링크에서 출발한
			//   일반도로 run 은 근거가 사라졌기 때문이다.
			// [2026-10-04 최정우 수정 — 사용자 확정 "일반도로 출발 = 일반도로·구간단속에 매칭된 첫 tick"]
			//   ① 대상 판정을 "진입 링크 == 패자 링크" 에서 **"진입 tick 이 강등된 tick"** 으로 바꿨다 — 진입
			//      링크는 begin 보정 등으로 바뀔 수 있다. ② 버리지 않고 **첫 유효 tick 으로 출발점을 다시
			//      잡는다**(ReanchorRunHead). 종전에는 버려서 승자 tick 부터 해소 시점까지의 주행이 빠졌다.
			//      유효 tick 이 하나도 없으면(강등 tick 만으로 된 run) 종전대로 버린다. ③ 열린 run 만이
			//      아니라 **보류 run·주정차 접촉 이월·병합 이월**도 같은 처리를 한다 — 실측
			//      000376_20260819140532 는 해소 시점에 run 이 이미 보류·접촉 이월로 넘어가 있어
			//      dropped=0 으로 빠져나갔고, 출발 표류(seq4·5)가 일반도로 4~53 에 그대로 남았다.
			// 되돌리는 법: 이 블록을 종전 "qwEntryLinkID == qwLoserLink 인 vtNodeStepRuns 삭제" 로 되돌린다.
			size_t nDropped = 0, nReanchored = 0;
			if ((pstSession != nullptr) && !vtLoserIdx.empty())
			{
				set<uint32> setLoserSeq;
				for (size_t i = 0; i < vtLoserIdx.size(); ++i)
				{
					const size_t idx = vtLoserIdx[i];
					if ((idx < pvtUpdates->size()) && ((*pvtUpdates)[idx].strTripId == stRawLogInfo.szTripID))
						setLoserSeq.insert(static_cast<uint32>(strtoul((*pvtUpdates)[idx].strGpsSeq.c_str(), nullptr, 10)));
				}
				double dfCut = 0.0;
				for (size_t c = pstSession->vtNodeStepRuns.size(); c > 0; --c)
				{
					const int nRet = ReanchorRunHead(pstSession->vtNodeStepRuns[c - 1], setLoserSeq, qwLoserLink, &dfCut);
					if (nRet < 0)
					{
						pstSession->vtNodeStepRuns.erase(pstSession->vtNodeStepRuns.begin() + (c - 1));
						++nDropped;
					}
					else if (nRet > 0) ++nReanchored;
				}
				if (pstSession->bHasHeldNodeStepRun)
				{
					const int nRet = ReanchorRunHead(pstSession->stHeldNodeStepRun, setLoserSeq, qwLoserLink, &dfCut);
					if (nRet < 0) { pstSession->bHasHeldNodeStepRun = false; ++nDropped; }
					else if (nRet > 0) ++nReanchored;
				}
				if (pstSession->bHasMergeCarry)
				{
					const int nRet = ReanchorRunHead(pstSession->stMergeCarry, setLoserSeq, qwLoserLink, &dfCut);
					if (nRet < 0) { pstSession->bHasMergeCarry = false; ++nDropped; }
					else if (nRet > 0) ++nReanchored;
				}
				if (pstSession->bHasParkTouchCarry)
				{
					const int nRet = ReanchorRunHead(pstSession->stParkTouchCarry, setLoserSeq, qwLoserLink, &dfCut);
					if (nRet < 0)
					{
						// 접촉 버퍼가 강등 tick 뿐이면 버퍼만 비운다 — 다음 접촉 tick 이 새로 연다
						pstSession->bHasParkTouchCarry = false;
						pstSession->dfParkTouchAccumAtLastIn = 0.0;
						++nDropped;
					}
					else if (nRet > 0)
					{
						// 마지막 안쪽 tick 시점의 누적 스냅샷도 같은 만큼 당긴다(폴리곤 안 거리 산출에 쓰임)
						pstSession->dfParkTouchAccumAtLastIn -= dfCut;
						if (pstSession->dfParkTouchAccumAtLastIn < 0.0) pstSession->dfParkTouchAccumAtLastIn = 0.0;
						++nReanchored;
					}
				}
			}
			if (!vtLoserIdx.empty())
				LOGFMTW("[#%02d] trip-start ambiguous link resolved!device=[%s] trip_id=[%s] "
					"loser_link=[%llu] loser_streak=[%zu] (DB-corrected, node_step runs dropped=[%zu] reanchored=[%zu])",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					static_cast<unsigned long long>(qwLoserLink), vtLoserIdx.size(), nDropped, nReanchored);
			pstSession->qwStartCandLinkA = 0;
			pstSession->qwStartCandLinkB = 0;
			pstSession->vtStartCandIdxA.clear();
			pstSession->vtStartCandIdxB.clear();
		};

		if (pstSession->qwStartCandLinkA == 0)
		{
			pstSession->qwStartCandLinkA = stMatchLinkInfo.qwLinkID;
			nJoinedStartCand = 1;
		}
		else if (stMatchLinkInfo.qwLinkID == pstSession->qwStartCandLinkA)
		{
			if (static_cast<int>(pstSession->vtStartCandIdxA.size()) >= m_stConfig.nOppStreakMax)
				ResolveStartAmbiguity(pstSession->vtStartCandIdxB, pstSession->qwStartCandLinkB);
			else
				nJoinedStartCand = 1;
		}
		else if (pstSession->qwStartCandLinkB == 0)
		{
			PLINK_INFO pstCandA = m_stConfig.pcDataLoader->GetLinkInfo(pstSession->qwStartCandLinkA);
			if ((pstCandA != nullptr) && (pstCandA->qwOppositeLinkID != 0)
				&& (pstCandA->qwOppositeLinkID == stMatchLinkInfo.qwLinkID))
			{
				pstSession->qwStartCandLinkB = stMatchLinkInfo.qwLinkID;
				nJoinedStartCand = 2;
			}
			else
			{
				// A도 아니고 반대편도 아닌 제3의 링크 — B가 없었으니 A가 그냥 승리(정리만)
				ResolveStartAmbiguity(pstSession->vtStartCandIdxB, 0);
			}
		}
		else if (stMatchLinkInfo.qwLinkID == pstSession->qwStartCandLinkB)
		{
			if (static_cast<int>(pstSession->vtStartCandIdxB.size()) >= m_stConfig.nOppStreakMax)
				ResolveStartAmbiguity(pstSession->vtStartCandIdxA, pstSession->qwStartCandLinkA);
			else
				nJoinedStartCand = 2;
		}
		else
		{
			// 제3의 링크 등장 — A/B 중 더 많이 쌓인 쪽이 승리
			if (pstSession->vtStartCandIdxA.size() >= pstSession->vtStartCandIdxB.size())
				ResolveStartAmbiguity(pstSession->vtStartCandIdxB, pstSession->qwStartCandLinkB);
			else
				ResolveStartAmbiguity(pstSession->vtStartCandIdxA, pstSession->qwStartCandLinkA);
		}
	}

	// ── 경로 불연속 보류행 보정 (2026-08-26 최정우 추가) ──
	//   보류 행의 링크가 직전 신뢰 링크와 위상적으로(같은 노드 공유) 연결 안 됐는데, "다음"
	//   확정 링크는 직전 신뢰 링크와 곧바로 연결된다면 — 보류 행이 경로에서 뜬 오매칭이라는
	//   신호. 위 왕복분리 보정(qwOppositeLinkID 공식 짝만 인정)과 달리 원인을 가리지 않고 순수
	//   노드 연결로만 판단해 더 넓게 잡는다. 좌표/링크는 다른 저신뢰 SKIP과 동일하게 참고용으로
	//   DB에 그대로 남기고 MATCH_STATUS만 SKIP 재기록 — bMatched(과금 처리 여부)는 이미 위에서
	//   확정된 값 그대로 두어 과금은 되돌리지 않는다(기존 정책과 동일). bJoinedOppStreak 인
	//   틱은 저 위 로직이 이미 처리 중이라 건드리지 않는다 (실측 000376_20260826152113
	//   M260/M261/M262 — M261 도 SKIP 이라 기존 1틱 지연만으로는 M262 까지 못 보고 놓쳤었음,
	//   ProcessRawLog 의 SKIP 보류연장(MM_PENDING_MAX_HOLD_TICKS)과 짝을 이룸)
	if (bMatched && bHasNextLinkID && (qwNextLinkID != stMatchLinkInfo.qwLinkID) && !bJoinedOppStreak
		&& (pstSession->qwLastConfirmedLinkID != 0) && (pstSession->qwLastConfirmedLinkID != stMatchLinkInfo.qwLinkID)
		&& (m_stConfig.pcDataLoader != nullptr))
	{
		PLINK_INFO pstLastLink = m_stConfig.pcDataLoader->GetLinkInfo(pstSession->qwLastConfirmedLinkID);
		PLINK_INFO pstPendingLinkChk = m_stConfig.pcDataLoader->GetLinkInfo(stMatchLinkInfo.qwLinkID);
		PLINK_INFO pstNextLinkChk = m_stConfig.pcDataLoader->GetLinkInfo(qwNextLinkID);

		if ((pstLastLink != nullptr) && (pstPendingLinkChk != nullptr) && (pstNextLinkChk != nullptr))
		{
			bool bPendingConnected =
				(pstPendingLinkChk->qwStNodeID == pstLastLink->qwEdNodeID)
				|| (pstPendingLinkChk->qwStNodeID == pstLastLink->qwStNodeID)
				|| (pstPendingLinkChk->qwEdNodeID == pstLastLink->qwEdNodeID)
				|| (pstPendingLinkChk->qwEdNodeID == pstLastLink->qwStNodeID);
			bool bNextConnectedToLast =
				(pstNextLinkChk->qwStNodeID == pstLastLink->qwEdNodeID)
				|| (pstNextLinkChk->qwEdNodeID == pstLastLink->qwEdNodeID);

			if (!bPendingConnected && bNextConnectedToLast)
			{
				nFinalStatus = MATCH_STATUS_SKIP;
				LOGFMTW("[#%02d] path discontinuity corrected!device=[%s] trip_id=[%s] seq=[%u] "
					"pending_link=[%llu] last_confirmed=[%llu] next=[%llu] -> SKIP",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
					static_cast<unsigned long long>(pstSession->qwLastConfirmedLinkID),
					static_cast<unsigned long long>(qwNextLinkID));
			}
			// ── 교차로 짧은 지선 1틱 튐 보정 (2026-09-06 최정우 추가, 사용자 지시) ──
			//   위 "경로 불연속"은 보류 행이 직전 링크와 **미연결**인 경우를 잡는다. 그런데 교차로
			//   한복판에서는 지선도 직전 링크와 같은 노드를 공유해 "연결됨"으로 나오므로 그 조건에
			//   안 걸린다. 실측 000376_20260819093337 seq119 — 원시좌표가 지선 2040426202(12.6m)와
			//   본선 2040426701 양쪽에서 **정확히 3.5m 등거리**이고 heading(86°)으로도 안 갈려,
			//   그 시점 정보만으로는 구분이 불가능했다(2026-08-22 주석의 "다음 점을 봐야 한다" 유형).
			//   다음 확정 링크가 나온 뒤에는 위상으로 갈린다:
			//     · 직전 A -> 보류 B 정방향 가능  (A 종료노드 == B 시작노드)
			//     · 직전 A -> 다음 C 정방향 가능  (A 종료노드 == C 시작노드)  = A·C 가 직결
			//     · 보류 B -> 다음 C 정방향 **불가** (B 종료노드 != C 시작노드)
			//   이면 B 로 들어갔다가 C 로 가려면 B 를 **되돌아 나와야** 한다 — 3초 tick 에 12.6m
			//   지선을 왕복하는 건 물리적으로 불가능하므로 B 는 지선 튐이다. 짧은 링크(MM_SPUR_MAX_LEN)
			//   로 한정해 본선 우회까지 잡지 않는다.
			//   처리는 위 경로 불연속과 동일하게 **MATCH_STATUS 만 SKIP 재기록**한다 — 좌표·링크는
			//   참고용으로 남기고 bMatched(과금 반영 여부)는 건드리지 않아 과금 로직에 영향이 없다.
			else
			{
				static const double MM_SPUR_MAX_LEN = 30.0;		// (m) 지선으로 볼 최대 링크 길이
				// 이격이 아주 작으면 차량이 실제로 그 지선 위였을 가능성이 높아 판정에서 뺀다 —
				//   실측 000376_20260821095603 seq35: 2040425802(8.9m)에 **이격 1.6m** 로 잘 맞던
				//   점이 지선 판정에 걸려 SKIP 됐다(재매칭도 실패). 교차로 오탐(seq119 은 3.5m,
				//   seq17 은 7.7m)과 달리 1.6m 는 그 링크 위에 있었다고 봐야 한다
				//   (2026-09-06 최정우 추가, 사용자 지시)
				static const double MM_SPUR_MIN_ILEN = 2.0;		// (m) 이 이하 이격이면 지선 판정 제외
				const bool bLastToPendingFwd =
					(pstLastLink->qwEdNodeID == pstPendingLinkChk->qwStNodeID);
				const bool bLastToNextFwd =
					(pstLastLink->qwEdNodeID == pstNextLinkChk->qwStNodeID);
				const bool bPendingToNextFwd =
					(pstPendingLinkChk->qwEdNodeID == pstNextLinkChk->qwStNodeID);

				if (bLastToPendingFwd && bLastToNextFwd && !bPendingToNextFwd
					&& (pstPendingLinkChk->dfLen > 0.0)
					&& (pstPendingLinkChk->dfLen <= MM_SPUR_MAX_LEN)
					&& (static_cast<double>(pstSession->nPendingIntersectLen) > MM_SPUR_MIN_ILEN))
				{
					// 지선 튐이 확인됐다 — SKIP 으로 버리지 않고 **본선으로 재매칭해 MATCHED 를
					//   유지한다**(사용자 지시, 2026-09-06). 차량은 실제로 그 지점을 지났으므로
					//   버리면 매칭율만 잃는다. 다음 확정 링크(C)로 편향 재매칭하고, 결과가 본선
					//   A 나 C 로 떨어질 때만 채택한다 — 다시 지선이나 엉뚱한 링크로 떨어지면
					//   근거가 없으므로 종전 계획대로 SKIP 으로 표시한다.
					//   stMatchLinkInfo 는 이 함수의 지역 복사본이라, 여기서 덮어쓰면 아래 과금
					//   함수들과 AppendUpdateRow 가 모두 교정된 링크·좌표를 쓴다 — 표시만 바꾸는
					//   다른 보정과 달리 과금 산출에도 반영된다(그게 맞다: 링크가 바뀌었으므로).
					bool bFixed = false;
					if (m_stConfig.pcProcessManager != nullptr)
					{
						CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
						MATCH_LINK_INFO stRematched;
						if (cPM.RematchBeginBiased(stRawLogInfo, qwNextLinkID, &stRematched)
							&& ((stRematched.qwLinkID == qwNextLinkID)
								|| (stRematched.qwLinkID == pstSession->qwLastConfirmedLinkID)))
						{
							LOGFMTW("[#%02d] junction spur excursion corrected!device=[%s] trip_id=[%s] "
								"seq=[%u] spur_link=[%llu] len=[%.1f]m -> rematched=[%llu] "
								"(last=[%llu] next=[%llu])",
								nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
								stRawLogInfo.dwSeqNo,
								static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
								pstPendingLinkChk->dfLen,
								static_cast<unsigned long long>(stRematched.qwLinkID),
								static_cast<unsigned long long>(pstSession->qwLastConfirmedLinkID),
								static_cast<unsigned long long>(qwNextLinkID));
							stMatchLinkInfo = stRematched;
							pstSession->nPendingIntersectLen =
								CalcIntersectLen(stRawLogInfo, stRematched.dfMatchX, stRematched.dfMatchY);
							bFixed = true;
						}
					}
					if (!bFixed)
					{
						nFinalStatus = MATCH_STATUS_SKIP;
						LOGFMTW("[#%02d] junction spur excursion corrected!device=[%s] trip_id=[%s] seq=[%u] "
							"spur_link=[%llu] len=[%.1f]m last_confirmed=[%llu] next=[%llu] -> SKIP"
							" (rematch failed)",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
							static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
							pstPendingLinkChk->dfLen,
							static_cast<unsigned long long>(pstSession->qwLastConfirmedLinkID),
							static_cast<unsigned long long>(qwNextLinkID));
					}
				}
			}
		}
	}

	if (bMatched)
	{
		const double dfCurX = pstSession->dfLastMatchX;
		const double dfCurY = pstSession->dfLastMatchY;
		const time_t dtCurGps = pstSession->dtLastMatchGps;
		const uint32 dwCurGpsSeq = pstSession->dwLastMatchGpsSeq;
		const bool bCurHas = pstSession->bHasLastMatch;

		pstSession->dfLastMatchX = pstSession->dfPendingPrevMatchX;
		pstSession->dfLastMatchY = pstSession->dfPendingPrevMatchY;
		pstSession->dtLastMatchGps = pstSession->dtPendingPrevMatchGps;
		pstSession->dwLastMatchGpsSeq = pstSession->dwPendingPrevMatchGpsSeq;
		pstSession->bHasLastMatch = pstSession->bPendingHadLastMatch;

		// 세션에 DEVICE_KEY 보관 — 커버리지 복구 행을 만들 때 필요 (2026-09-22 최정우 추가)
		if (pstSession->szDeviceKey[0] == '\0')
		{
			strncpy(pstSession->szDeviceKey, stRawLogInfo.szDeviceKey, sizeof(pstSession->szDeviceKey) - 1);
			pstSession->szDeviceKey[sizeof(pstSession->szDeviceKey) - 1] = '\0';
		}

		// [2026-09-22 최정우 추가 — 계측 전용, 판정 무변경] 이번 tick 이 실제로 경유한 링크를
		//   트립 경로에 누적한다. aqwPathLinkIDs 는 직전 확정 링크 다음부터 이번 확정 링크까지의
		//   경유 링크 전부이므로(2026-08-20 도입), tick 이 안 찍힌 링크까지 들어 있다.
		//   연속 중복만 걸러 시퀀스를 유지한다 — 같은 링크를 되돌아오는 주행도 있으므로 set 은 쓰지 않는다.
		{
			const uint8 nPathCnt = (stMatchLinkInfo.nPathLinkCount > 0)
				? stMatchLinkInfo.nPathLinkCount : 0;
			if (nPathCnt > 0)
			{
				for (uint8 pi = 0; pi < nPathCnt; ++pi)
				{
					const uint64 qwPathLink = stMatchLinkInfo.aqwPathLinkIDs[pi];
					if (qwPathLink == 0) continue;
					if (!pstSession->vtTripPathLinks.empty()
						&& (pstSession->vtTripPathLinks.back().qwLinkID == qwPathLink)) continue;
					pstSession->vtTripPathLinks.push_back(VEHICLE_TRIP_SESSION::sTripPathLink(
						qwPathLink, stRawLogInfo.dwSeqNo, stRawLogInfo.dtGPS,
						stRawLogInfo.fSpeed));
				}
			}
			else if (stMatchLinkInfo.qwLinkID != 0)
			{
				// 경로 정보가 없으면(그럴듯함 검증에서 버려진 경우 등) 확정 링크 1개만
				if (pstSession->vtTripPathLinks.empty()
					|| (pstSession->vtTripPathLinks.back().qwLinkID != stMatchLinkInfo.qwLinkID))
					pstSession->vtTripPathLinks.push_back(VEHICLE_TRIP_SESSION::sTripPathLink(
						stMatchLinkInfo.qwLinkID, stRawLogInfo.dwSeqNo, stRawLogInfo.dtGPS,
						stRawLogInfo.fSpeed));
			}
	
			// [2026-09-22 최정우 추가] **열려 있는 일반도로 run 에도 같은 링크를 쌓는다.**
			//   행을 만들 때 이 목록이 CHARGE_INSERT_ROW.vtCoveredLinks 가 되어, 트립 마감 시
			//   "실제 경유한 링크" 와 대조된다. 타 과금유형 run 은 대조 대상이 아니라(그 유형이
			//   자기 구역을 책임진다) 일반도로 run 만 쌓는다.
			for (size_t ri = 0; ri < pstSession->vtNodeStepRuns.size(); ++ri)
			{
				ZONE_RUN_SESSION& stNsRun = pstSession->vtNodeStepRuns[ri];
				const uint8 nCnt = (stMatchLinkInfo.nPathLinkCount > 0)
					? stMatchLinkInfo.nPathLinkCount : 0;
				if (nCnt > 0)
				{
					for (uint8 pi = 0; pi < nCnt; ++pi)
					{
						const uint64 qwL = stMatchLinkInfo.aqwPathLinkIDs[pi];
						if (qwL == 0) continue;
						if (!stNsRun.vtRunLinks.empty() && (stNsRun.vtRunLinks.back() == qwL)) continue;
						stNsRun.vtRunLinks.push_back(qwL);
					}
				}
				else if (stMatchLinkInfo.qwLinkID != 0)
				{
					if (stNsRun.vtRunLinks.empty()
						|| (stNsRun.vtRunLinks.back() != stMatchLinkInfo.qwLinkID))
						stNsRun.vtRunLinks.push_back(stMatchLinkInfo.qwLinkID);
				}
			}
		}

		ProcessOpenGateCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts, bTrustedTripEnd);
		ProcessClosedRoadCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessSpeedZoneCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessExemptZoneCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts, bTrustedTripEnd);
		ProcessNodeStepCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts, bTrustedTripEnd,
			(nFinalStatus == MATCH_STATUS_MATCHED) && pstSession->bPendingHasCoords);

		// 트립 시작 판정 구간(첫 확정 링크 전)이면 일반도로 계열 run 들에 이 tick 을 기록한다 — 출발 후보
		//   해소 시 출발점 재기준용(ReanchorRunHead, 2026-10-04 최정우 추가, 사용자 확정)
		if (pstSession->qwLastConfirmedLinkID == 0)
		{
			for (size_t r = 0; r < pstSession->vtNodeStepRuns.size(); ++r)
				RecordRunHeadTrail(pstSession->vtNodeStepRuns[r], stRawLogInfo.dwSeqNo, stRawLogInfo.dtGPS,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stMatchLinkInfo.qwLinkID, true);
			if (pstSession->bHasHeldNodeStepRun)
				RecordRunHeadTrail(pstSession->stHeldNodeStepRun, stRawLogInfo.dwSeqNo, stRawLogInfo.dtGPS,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stMatchLinkInfo.qwLinkID, true);
			if (pstSession->bHasMergeCarry)
				RecordRunHeadTrail(pstSession->stMergeCarry, stRawLogInfo.dwSeqNo, stRawLogInfo.dtGPS,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stMatchLinkInfo.qwLinkID, true);
			if (pstSession->bHasParkTouchCarry)
				RecordRunHeadTrail(pstSession->stParkTouchCarry, stRawLogInfo.dwSeqNo, stRawLogInfo.dtGPS,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stMatchLinkInfo.qwLinkID, false);
		}

		// NODE_STEP 케이스3(SKIP 구간 브릿지) — pstSession->dfLastMatchX/Y 가 아직 "보류 시점 스냅샷"
		//   (=FROM, 갭 이전 마지막 신뢰위치)으로 바꿔치기된 상태일 때(위 charge 함수들과 동일 근거)
		//   호출해야 ResolveSkipGapNodeStep() 내부에서 올바른 FROM 좌표를 읽는다 — 아래 restore
		//   이후엔 이미 TO(현재 커밋 대상) 위치로 덮여써 있어 늦다. qwLastConfirmedLinkID 를
		//   덮어쓰기 "전"에 옛 값을 FROM 링크ID로 써야 함(==0 이면 트립의 첫 확정 링크조차 없던 것 —
		//   조건에서 자동으로 걸러짐) (2026-09-01 최정우 추가)
		if ((pstSession->qwLastConfirmedLinkID != 0)
			&& (pstSession->qwLastConfirmedLinkID != stMatchLinkInfo.qwLinkID)
			&& !pstSession->vtSkipRunRawLogInfo.empty())
		{
			// ACCURACY_M SKIP 개별 틱 소급 MATCHED 승격 — NODE_STEP 브릿지(구간 단위 과금)와 별개로,
			//   다음 확정 링크(TO)가 실제로 알려진 지금 시점에 각 틱을 TO 쪽으로 편향 재매칭한다.
			//   heading 신뢰성 검증까지 통과(RematchBeginBiasedDirectional)하고 결과가 정확히 TO
			//   링크로 떨어지는 틱만 MATCH_STATUS 를 소급 재기록 — 클램프 브릿지(vtClampRunUpdateIdx)
			//   와 동일 패턴이나, "1-hop 인접" 사전조건 대신 재매칭 결과 자체(및 반대편 heading 검증)로
			//   신뢰도를 확보한다. 실패한 틱은 SKIP 유지 — NODE_STEP 브릿지가 구간 전체를 별도로 커버
			//   (2026-09-04 최정우 추가, 사용자 지시)
			if ((m_stConfig.pcProcessManager != nullptr)
				&& (pstSession->vtSkipRunUpdateIdx.size() == pstSession->vtSkipRunRawLogInfo.size()))
			{
				CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
				size_t nBridged = 0;
				for (size_t i = 0; i < pstSession->vtSkipRunRawLogInfo.size(); ++i)
				{
					size_t idx = pstSession->vtSkipRunUpdateIdx[i];
					if (idx >= pvtUpdates->size()) continue;

					MATCH_LINK_INFO stRematched;
					if (!cPM.RematchBeginBiasedDirectional(pstSession->vtSkipRunRawLogInfo[i],
							stMatchLinkInfo.qwLinkID, &stRematched)
						|| (stRematched.qwLinkID != stMatchLinkInfo.qwLinkID))
						continue;		// 재매칭 실패·TO 불일치·heading 신뢰 불가 — SKIP 유지

					int nNewIntersectLen = CalcIntersectLen(pstSession->vtSkipRunRawLogInfo[i],
						stRematched.dfMatchX, stRematched.dfMatchY);
					char szMatchLat[32], szMatchLon[32], szIntersectLen[16], szMatchLinkId[24];
					snprintf(szMatchLat, sizeof(szMatchLat), "%.06lf", stRematched.dfMatchY);
					snprintf(szMatchLon, sizeof(szMatchLon), "%.06lf", stRematched.dfMatchX);
					snprintf(szIntersectLen, sizeof(szIntersectLen), "%d", nNewIntersectLen);
					snprintf(szMatchLinkId, sizeof(szMatchLinkId), "%llu",
						static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));

					(*pvtUpdates)[idx].strMatchStatus = "1";
					(*pvtUpdates)[idx].strMatchLat = szMatchLat;
					(*pvtUpdates)[idx].strMatchLon = szMatchLon;
					(*pvtUpdates)[idx].strIntersectLen = szIntersectLen;
					(*pvtUpdates)[idx].strMatchLinkId = szMatchLinkId;
					++nBridged;
				}
				if (nBridged > 0)
				{
					LOGFMTW("[#%02d] accuracy-skip %zu/%zu-tick directional bridge!device=[%s] trip_id=[%s] "
						"link=[%llu] (rematched, charge not retroactively processed)",
						nThreadId, nBridged, pstSession->vtSkipRunRawLogInfo.size(),
						stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
				}
			}

			ResolveSkipGapNodeStep(nThreadId, pstSession, pstSession->qwLastConfirmedLinkID,
				stMatchLinkInfo.qwLinkID, stRawLogInfo, stMatchLinkInfo, pvtChargeInserts);
		}
		pstSession->vtSkipRunRawLogInfo.clear();
		pstSession->vtSkipRunUpdateIdx.clear();

		pstSession->dfLastMatchX = dfCurX;
		pstSession->dfLastMatchY = dfCurY;
		pstSession->dtLastMatchGps = dtCurGps;
		pstSession->dwLastMatchGpsSeq = dwCurGpsSeq;
		pstSession->bHasLastMatch = bCurHas;

		// 트립 시작 모호구간 후보(A/B)에 아직 편입 중이면(nJoinedStartCand!=0) 앵커를 확정하지
		//   않는다 — 여기서 그냥 대입해버리면 qwLastConfirmedLinkID 가 0이 아니게 돼 바로 다음
		//   틱부터 "앵커 없음" 조건이 깨져 위 블록이 다시는 못 돈다 (2026-08-24 최정우 추가)
		if (nJoinedStartCand == 0)
		{
			pstSession->qwLastConfirmedLinkID = stMatchLinkInfo.qwLinkID;
			pstSession->dtLastConfirmedLinkTime = stRawLogInfo.dtGPS;
			pstSession->dwLastConfirmedLinkGpsSeq = stRawLogInfo.dwSeqNo;
			pstSession->fLastConfirmedLinkSpeed = stRawLogInfo.fSpeed;
		}
	}
	else
	{
		// SKIP 틱 — 확정매칭 전이로 못 잡은 잔여 출구 케이스를 raw GPS 좌표로 한 번 더 확인
		//   (사용자 지시, 2026-08-24 최정우 추가)
		CheckClosedRoadExitByRawGps(nThreadId, stRawLogInfo, pstSession, pvtChargeInserts);
		// [2026-10-04 최정우 추가, 사용자 확정 — E18] 주정차 폴리곤 안 SKIP 좌표는 구간단속 판정에 쓰지 않는다
		if (!IsRawInParkingZone(stRawLogInfo.dfX, stRawLogInfo.dfY, stRawLogInfo.nAccuracyM))
			CheckSpeedZoneExitByRawGps(nThreadId, stRawLogInfo, pstSession, pvtChargeInserts);
	}

	bool bAppended = AppendUpdateRow(pvtUpdates, stRawLogInfo, nFinalStatus, pstSession->nPendingIntersectLen,
		pstSession->bPendingHasCoords ? &stMatchLinkInfo.dfMatchY : nullptr,
		pstSession->bPendingHasCoords ? &stMatchLinkInfo.dfMatchX : nullptr,
		pstSession->bPendingHasCoords ? stMatchLinkInfo.qwLinkID : 0);

	if (bJoinedOppStreak && bAppended)
		pstSession->vtOppStreakUpdateIdx.push_back(pvtUpdates->size() - 1);

	if (bAppended)
	{
		if (nJoinedStartCand == 1)
			pstSession->vtStartCandIdxA.push_back(pvtUpdates->size() - 1);
		else if (nJoinedStartCand == 2)
			pstSession->vtStartCandIdxB.push_back(pvtUpdates->size() - 1);
	}

	pstSession->bHasPendingCommit = false;
}

/**
 * @brief GPS 1건 처리 – 검증·맵매칭·결과 행 적재 (배치 종료 시 rawgps_update)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS 정보 (TRIP_ID 는 수집서버 적재분)
 * @param[out] pvtUpdates bulk UPDATE 대상 행 목록
 * @param[in,out] pstSession 배치 임시 세션 (bulk 성공 전까지 m_vtTripSessions 미반영)
 * @param[out] pvtChargeInserts 과금 INSERT 대상 행 — 이 tick 이 만든 행뿐 아니라, 트립 전환 시
 *   **이전 트립**의 보류행·미마감 구간(FlushOpenRunsAsAbnormalEnd)도 여기 실린다 (2026-09-15 추가)
 * @param[out] pvtTripEndUpdates TRIP_END_DT 미확정 행 마감 UPDATE 대상 — 트립 전환 강제마감 경로에서도 적재된다
 * @param[out] pbTripEnded TRIP END(2) 시 true 설정 — 일치 결과 무관, bulk 성공 후 세션 제거 (#9)
 * @return true(처리·적재 성공), false(인자 null·적재 실패)
 * @remark 2026-07-08 최정우 추가
 *   - TRIP_ID 없음/불일치, TRIP_EVENT 비정상 → SKIP
 *   - TRIP_EVENT=0(START) 또는 GPS_SEQ<=dwLastGpsSeq → 세션 초기화 후 시작
 *   - 맵매칭 실패 → ERROR, 성공 → MATCHED
 *   - TRIP_EVENT=2(END) 이면 MATCHED/ERROR/SKIP 무관 pbTripEnded=true (#9)
 * @remark 세션 갱신은 pstSession(배치 임시)에만 적용. run() 이 bulk 성공 시 커밋.
 * @remark 2026-08-21 최정우 수정 — 정상 매칭(bMatched && !bUntrustedMatch)된 행은 즉시
 *   커밋하지 않고 세션에 1틱 보류(CommitPendingRow 참고), 반대편 짝 링크 1틱 오매칭 보정 도입
*/
bool CRawLogWorker::ProcessRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		vector<TRIP_END_UPDATE_ROW> *pvtTripEndUpdates,
		VEHICLE_TRIP_SESSION *pstSession, bool *pbTripEnded)
{
	if ((pvtUpdates == nullptr) || (pvtChargeInserts == nullptr) || (pvtTripEndUpdates == nullptr)
		|| (pstSession == nullptr) || (pbTripEnded == nullptr))
		return false;

	sint16 nRejectStatus = MATCH_STATUS_SKIP;
	// device_key·trip_id·trip_event 2차 검증 (2026-07-08 최정우 주석 추가)
	if (!ValidateRawLog(nThreadId, stRawLogInfo, &nRejectStatus))
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, nRejectStatus);

	VEHICLE_TRIP_SESSION& stSession = *pstSession;
	stSession.dtLastSeen = time(nullptr);

	bool bFullReset = false;
	bool bSeqRollback = false;			// GPS_SEQ 역전 — 아래에서 이 행만 SKIP (2026-08-23 최정우 추가)
	// 시작(세션 초기화) 필요 여부 판단 (2026-07-08 최정우 주석 추가)
	if (NeedsBeginReset(nThreadId, stRawLogInfo, stSession, &bFullReset, &bSeqRollback))
	{
		if (bFullReset)
		{
			LOGFMTD("[#%02d] trip START reset!device=[%s] trip_id=[%s] seq=[%u]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRawLogInfo.dwSeqNo);

			// [버그 수정, 2026-09-15 최정우, 가상 시나리오 검증으로 재현] 이전 운행의 **보류행
			//   (1틱 지연커밋)을 리셋 전에 먼저 확정**한다. 세션 키가 DEVICE_KEY 라 옛 trip 의
			//   마지막 tick 이 보류 상태로 남은 채 다음 운행이 시작되는데, 종전에는 그 보류행이
			//   trip B 의 첫 매칭 tick 에서 CommitPendingRow() 로 확정됐다 — 그 시점엔 아래
			//   szTripId 갱신이 이미 끝나 **옛 tick 의 좌표·시각·GPS_SEQ 가 새 trip_id 로 기록**
			//   된다(실측 재현: trip B 행 3건이 trip A 의 seq35·occur_dt=141147 을 달고 나옴).
			//   덤으로 새 run 의 진입시각도 옛 trip 것으로 잡혀 두 운행 사이 공백이 통째로
			//   stay_seconds 에 들어간다(같은 재현에서 7,400초).
			//   여기서 커밋하면 szTripId 가 아직 옛 trip 이라 올바른 trip_id 로 적재된다.
			//   보정판단용 "다음" 링크는 없다(다음 tick 은 다른 운행이므로 이어붙이면 안 됨) —
			//   RAW_VLD/정확도 SKIP 분기와 동일하게 계산된 값 그대로 커밋한다.
			const bool bTripSwitched = (stSession.szTripId[0] != '\0')
				&& (strcmp(stSession.szTripId, stRawLogInfo.szTripID) != 0);

			if (stSession.bHasPendingCommit && bTripSwitched)
			{
				LOGFMTW("[#%02d] pending row committed before trip switch!device=[%s] "
					"prev_trip_id=[%s] new_trip_id=[%s] new_seq=[%u]",
					nThreadId, stRawLogInfo.szDeviceKey, stSession.szTripId,
					stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
				CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
			}

			// [버그 수정, 2026-09-15 최정우] 종료 신호 없이 끝난 트립의 **열린 과금 구간을 마감**한다.
			//   여기 도달했다는 건 이전 트립이 신뢰할 END 를 못 받았다는 뜻이다 — 정상 종료된 트립은
			//   run() 이 배치 끝에서 세션을 erase 하므로 살아남지 못한다. 종전에는 바로 아래
			//   ResetTripSessionForBegin() 이 vtNodeStepRuns/vtOpenRuns/vtExemptRuns/vtParkRuns 를
			//   **과금 행 하나 없이 clear()** 하고 bInClosedRoad/bInSpeedZone 도 플래그만 내려 그
			//   구간이 통째로 사라졌다. TTL 도 이 경우를 못 잡는다 — 세션 키가 DEVICE_KEY 라 다음
			//   운행이 세션을 그 자리에서 재사용하므로 만료 대상이 되지 않는다.
			//   **호출 위치가 핵심이다.** szTripId 는 아래에서 새 trip 으로 바뀌므로 지금은 아직
			//   "마감할 트립"이다(원복했던 코드 62 의 잡종 레코드 원인이 정확히 그 순서였다).
			//   마감 기준 시각·순번(dtLastGpsEventTime/dwLastGpsSeq) 갱신도 전부 이 지점보다 뒤다.
			if (bTripSwitched)
			{
				const size_t nBeforeFlush = pvtChargeInserts->size();
				FlushOpenRunsAsAbnormalEnd(nThreadId, stRawLogInfo.szDeviceKey, stSession,
					stSession.dtLastGpsEventTime, stSession.dwLastGpsSeq, time(nullptr),
					pvtChargeInserts, pvtTripEndUpdates,
					true);						// 종료신호 없는 트립 전환 — non_charge_reason 62/52 (2026-09-16 최정우 추가)
				const size_t nFlushed = pvtChargeInserts->size() - nBeforeFlush;
				if (nFlushed > 0)
				{
					// 다른 non_charge_reason 로그와 같은 형식으로 사유 코드·설명을 함께 남긴다
					//   — 면제도로 run 이 섞여 있으면 그 행만 52 로 적재되므로 로그의 62 는
					//   "이 마감의 사유"를 뜻한다(행별 실제 코드는 각 Append 로그 참고)
					//   (2026-09-16 최정우 추가)
					LOGFMTW("[#%02d] open runs flushed before trip switch!device=[%s] "
						"prev_trip_id=[%s] new_trip_id=[%s] rows=[%d] end_seq=[%u] "
						"non_charge_reason=[%d:%s]",
						nThreadId, stRawLogInfo.szDeviceKey, stSession.szTripId,
						stRawLogInfo.szTripID, static_cast<int>(nFlushed), stSession.dwLastGpsSeq,
						NCR_NO_TRIP_END_FORCED_CLOSE,
						m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable),
							NCR_NO_TRIP_END_FORCED_CLOSE));
				}
			}
		}

		// [2026-09-22 최정우 추가] 워터마크 큐 전량 방출 — **ResetTripSessionForBegin() 직전**이
		//   유일한 자리다. 아래 리셋이 큐를 비우므로 여기서 안 꺼내면 이전 트립의 **이미 마감된**
		//   과금 행이 통째로 사라진다(2026-09-15 에 vtOpenRuns 가 같은 자리에서 사라졌던 것과
		//   동일한 함정). 새 트립이 시작되는 경우(bFullReset)에만 해당 — GPS_SEQ 역전 재처리
		//   (bFullReset=false)는 같은 트립이 계속되는 것이라 큐를 그대로 둬야 순서가 이어진다.
		// [버그 수정, 2026-10-04 최정우 — 사용자 지시 "다른 과금 경로의 TRIP_SEQ 확정 방법과 동일하게"]
		//   방출 전에 **이 배치에서 아직 번호를 받지 않은 행을 먼저 이 트립 큐에 넣는다** —
		//   ExpireTtlSessions() 의 TTL 마감(Flush -> EnqueueChargeRows -> ReleaseChargeQueue)과 같은
		//   순서다. 종전에는 이 Enqueue 가 빠져, 바로 위 FlushOpenRunsAsAbnormalEnd() 가 만든 이전
		//   트립의 마감 행(과 이 배치에서 앞서 마감된 이전 트립 행)이 pvtChargeInserts 에 번호 없이
		//   남았다가 배치 끝 run() 의 EnqueueChargeRows(&stWorkSession) 에서 **이미 새 트립이 된
		//   세션 큐**에 들어가 새 트립 카운터로 번호를 받았다 — 두 트립의 TRIP_SEQ 가 섞이고
		//   (시나리오 980011 이 1·3, 980012 가 2·4·5), 전환이 배치 경계에 걸리면 이전 트립 행이 1번부터
		//   다시 번호를 받아 기존 행과 PK 충돌 -> ON CONFLICT DO NOTHING 으로 유실될 수 있었다.
		//   EnqueueChargeRows 는 이미 번호를 받은(bEmitted) 행은 건드리지 않는다.
		// 되돌리는 법: 아래 EnqueueChargeRows 한 줄 삭제
		if (bFullReset)
		{
			EnqueueChargeRows(&stSession, pvtChargeInserts);
			ReleaseChargeQueue(nThreadId, &stSession, UINT32_MAX, pvtChargeInserts);
		}

		// 연속 맵매칭 세션 시작 상태로 초기화 (2026-07-08 최정우 주석 추가)
		ResetTripSessionForBegin(stSession, bFullReset);
	}
	else if (!bSeqRollback && !stSession.bStartWarned && stRawLogInfo.nTripEvent != TRIP_EVENT_START)
	{
		LOGFMTW("[#%02d] trip missing START!device=[%s] trip_id=[%s] seq=[%u] event=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			stRawLogInfo.dwSeqNo, static_cast<int>(stRawLogInfo.nTripEvent));
		stSession.bStartWarned = true;
	}

	// 현재 배치 TRIP_ID 를 세션에 기록(다음 trip 변경 감지 기준). 키는 DEVICE_KEY 라 trip 이 바뀌면 위에서 리셋됨 (2026-07-08 최정우 추가)
	strncpy(stSession.szTripId, stRawLogInfo.szTripID, sizeof(stSession.szTripId) - 1);
	stSession.szTripId[sizeof(stSession.szTripId) - 1] = '\0';
	// trip_seq 이어 매기기 — run() 이 읽어 둔 그 트립의 기존 최대 TRIP_SEQ 부터 잇는다(1회만 적용)
	//   (2026-10-04 최정우 추가, 사용자 확정 — QueryTripSeqMax() 주석 참고)
	if ((stSession.szSeedTripId[0] != '\0') && (strcmp(stSession.szSeedTripId, stSession.szTripId) == 0))
	{
		if (stSession.nSeedEmitSeq > stSession.nEmitSeq)
		{
			LOGFMTW("[#%02d] trip_seq continued from db!device=[%s] trip_id=[%s] seq=[%u] emit_seq=[%d] -> [%d]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				stSession.nEmitSeq, stSession.nSeedEmitSeq);
			stSession.nEmitSeq = stSession.nSeedEmitSeq;
		}
		stSession.szSeedTripId[0] = '\0';
		stSession.nSeedEmitSeq = 0;
	}

	// TRIP_EVENT=END 스퓨리어스(순서역전) 검사 — 이 END 의 gps_dt 가 이 trip 에서 지금까지 확인된
	//   최대 gps_dt(dtLastGpsEventTime)보다 과거면 신뢰하지 않는다(단말이 보낸 순서역전·중복 도착
	//   이벤트로 추정). 실측 000376_20260819140856 seq19(gps_dt 가 직전 seq18보다 과거인 END) —
	//   이 스퓨리어스 END 가 그대로 처리되면서 열려있던 구간단속 세션이 강제마감→리셋되고, 뒤이은
	//   정상 주행이 같은 구역 재진입으로 오판돼 AUDIT 행이 중복 적재됐다. 오판(진짜 END를 스퓨리어스로
	//   착각)해도 세션이 안 지워질 뿐 데이터 유실은 없음 — 결국 TTL 만료로 안전하게 마감된다(사용자
	//   확정, 2026-08-25). dtLastGpsEventTime==0(신규 trip 첫 행)이면 비교 대상이 없어 항상 신뢰한다.
	//   (2026-08-25 최정우 추가)
	bool bTrustedTripEnd = IsTrustedTripEnd(stRawLogInfo, stSession);
	if (!bTrustedTripEnd && (stRawLogInfo.nTripEvent == TRIP_EVENT_END))
	{
		LOGFMTW("[#%02d] spurious END skip!device=[%s] trip_id=[%s] seq=[%u] gps_dt=[%ld] last_gps_dt=[%ld]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<long>(stRawLogInfo.dtGPS), static_cast<long>(stSession.dtLastGpsEventTime));
	}
	if (stRawLogInfo.dtGPS > stSession.dtLastGpsEventTime)
		stSession.dtLastGpsEventTime = stRawLogInfo.dtGPS;

	// 트립종료 직전, 아직 보류(pending) 중인 1틱 지연 행이 있으면 먼저 확정(commit)한다 — 바로
	//   아래 AppendExpiredClosedRoadCharge/AppendExpiredSpeedZoneCharge 가 세션의 bInClosedRoad/
	//   bInSpeedZone 을 읽기 "전"에 보류 행의 과금 반영이 먼저 끝나 있어야 정확하다. 이 시점엔
	//   이번 행(TRIP_EVENT=END) 자신의 매칭 결과를 아직 몰라 보정판단용 "다음" 링크가 없음(보정
	//   시도 안 함, 보류 행을 계산된 값 그대로 커밋) (2026-08-21 최정우 추가)
	if (bTrustedTripEnd)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);

	// 트립 종료(TRIP_EVENT=2) — 매칭 성공/실패 무관하게 그 trip_id 의 PRIM_CHARGEHAND 전 행에
	//   trip_end_dt 를 나중에(run() 이 배치 종료 시) UPDATE 하도록 적재 (2026-08-12 최정우 추가)
	//   UPD_DT 도 TRIP_END_DT 와 동일하게 GPS 시각(wall-clock 아님) 사용 — 사용자 지시(2026-08-14):
	//   "구역 이탈"과 "트립 정상종료"가 같은 tick 에 겹치는 경우(예: 일반도로 구간 안에서 트립이
	//   끝나는 경우) 진출시각과 종료시각이 같은 값이니 UPD_DT 도 그 값으로 맞추는 게 자연스러움 —
	//   5유형 공용 UPDATE라 전부에 동일 적용됨. bTrustedTripEnd 로 스퓨리어스 END 는 제외(2026-08-25)
	if (bTrustedTripEnd)
	{
		TRIP_END_UPDATE_ROW stEndRow;
		stEndRow.strTripId = stRawLogInfo.szTripID;
		stEndRow.strTripEndDt = FormatDateTime14(stRawLogInfo.dtGPS);
		stEndRow.strUpdDt = FormatDateTime14(time(nullptr));	// UPD_DT=최종 수정 일시(벽시계) — DB 컬럼 정의대로 (2026-10-04 최정우 수정, 종전 GPS 종료시각)
		pvtTripEndUpdates->push_back(stEndRow);

		// 트립이 끝나는 시점에 아직 입구만 통과하고 출구를 못 찾은 폐쇄형/구간단속 세션이 열려
		//   있으면, 곧이어 세션 자체가 삭제(TRIP_EVENT=2 → pbTripEnded=true → run()이 배치 성공 후
		//   mapSessions.erase)되면서 그 진행 중이던 통행 기록이 통째로 사라진다 — TTL 만료 때와
		//   동일한 형태(N/3 AUDIT, 못 본 출구게이트는 NULL로 저장 — NULLIF(...,'') 처리)로 여기서
		//   먼저 기록해둔다(2026-08-20 최정우 추가, 사용자 지시 — 이슈②·③이 공유하던 근본원인 해결).
		//   snapshot-후-increment 패턴은 ExpireTtlSessions()와 동일 이유(한 tick에 폐쇄형·구간단속이
		//   동시에 열려있을 수 있어 trip_seq PK 충돌 방지)
		//   마감 후에는 진행 플래그를 내린다. 두 Append 함수는 세션을 const 참조로 받아 스스로
		//   플래그를 못 내리는데, 한 트립에 TRIP_EVENT=END 행이 둘 이상 오면(실측
		//   000376_20260819140856 의 seq19·seq23 — seq19 는 시각이 뒤로 간 도착 이벤트 행이다)
		//   같은 진입이 두 번 마감돼 AUDIT 행이 중복 적재된다. 실측에서 구간단속 RL-Z00003 이
		//   진입시각 20260819140940 으로 2건(체류 9초·21초) 쌓였다. 둘 다 charge_yn=N 이라
		//   요금이 이중으로 나가지는 않았지만, 출구를 봤다면 이중 과금이 될 수 있는 구조다.
		//   트립 종료 시 세션은 어차피 배치 끝에 삭제되므로 여기서 미리 내려도 안전하다
		//   (2026-08-23 최정우 수정)
		bool bWasClosedRoad = stSession.bInClosedRoad;
		AppendExpiredClosedRoadCharge(nThreadId, stRawLogInfo.szDeviceKey, stSession, stRawLogInfo.dtGPS, pvtChargeInserts);
		if (bWasClosedRoad)
		{
			stSession.nChargeSeq += 1;
			stSession.bInClosedRoad = false;
		}

		bool bWasSpeedZone = stSession.bInSpeedZone;
		// NODE_STEP 일반도로 확장(케이스1)으로 최대 2건(SPEED+NODE_STEP) 추가될 수 있어 고정 +1
		//   대신 실제 증가분만큼 — ExpireTtlSessions() 동일 근거 (2026-09-01 최정우 수정)
		size_t nSizeBeforeSpeed2 = pvtChargeInserts->size();
		AppendExpiredSpeedZoneCharge(nThreadId, stRawLogInfo.szDeviceKey, stSession, stRawLogInfo.dtGPS, pvtChargeInserts, stRawLogInfo.dwSeqNo);
		if (bWasSpeedZone)
		{
			stSession.nChargeSeq += static_cast<int>(pvtChargeInserts->size() - nSizeBeforeSpeed2);
			stSession.bInSpeedZone = false;
		}
	}

	// ── GPS_SEQ 역전 행 — 세션 앵커를 유지한 채 이 행만 SKIP (2026-08-23 최정우 추가) ──
	//   예전에는 NeedsBeginReset() 이 여기서 true 를 돌려 세션 앵커(직전 링크·위치·고도)를 통째로
	//   버리고 BEGIN 으로 강등했다. 그런데 BEGIN 은 heading 을 아예 안 보고 거리만으로 판정하므로
	//   (bIgnoreHeading=true, BeginMapMatch.cpp), 왕복분리 도로에서는 10m 옆 건너편 차로가 그냥
	//   더 가깝다는 이유로 채택된다 — 실측 000376_20260819140856 seq18 에서 반대차로 2.80m 가
	//   정답차로 7.12m 를 이겨 오매칭됐고, 그 잘못된 앵커가 seq20 까지 번졌다.
	//   방위각으로는 막을 수 없다: GISUtil::SgmtMatch() 가 양방향 단일 링크 지원을 위해 세그먼트
	//   방위각의 정·역(+180°) 중 더 잘 맞는 쪽을 채택해서, 실제 167° 어긋난 건너편 차로가 13° 로
	//   접혀 하드컷(MM_DIR_MAX_DEG=120°)을 그냥 통과한다. 즉 이 쌍에서 유일한 방어선은 위상
	//   (직전 링크에서 도달 가능한가)인데 BEGIN 에는 그 제약이 없다.
	//   과거 행 하나 때문에 뒤따르는 정상 행이 그 방어선을 잃을 이유가 없다. 역전 행 자신은
	//   이미 지나온 시점이라 재매칭 가치가 없으므로 SKIP(3) 로 남긴다.
	//   트립종료(TRIP_EVENT=END) 기록·보류행 커밋은 바로 위에서 이미 끝냈다.
	if (bSeqRollback)
	{
		if (bTrustedTripEnd)
		{
			*pbTripEnded = true;
			FlushNodeStepRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
			FlushOpenExemptRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
		}
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
	}

	// GPS 좌표·RAW_VLD 유효성 검사 — SKIP(3). 세션·DB 좌표 미저장 (2026-07-10 최정우 수정)
	if (ShouldSkipGpsInput(nThreadId, stRawLogInfo, m_stConfig.nIgnoreRawVld != 0))
	{
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bLastPointOk = false;			// (2026-07-21 최정우 추가)
		if (bTrustedTripEnd)
		{
			*pbTripEnded = true;
			FlushNodeStepRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
			FlushOpenExemptRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
		}

		// 주정차 판정은 RAW_VLD 와 무관하게 원시 GPS 좌표로 수행한다 (2026-08-22 사용자 확정).
		//   근거: 차량이 멈추면 GPS 가 측위를 놓쳐 ACCURACY_M 이 급격히 나빠지고 RAW_VLD=false 가
		//   되는데(실측: DRIVE_STATUS=PARKED 497건의 평균 정확도 64m, ON_ROAD 는 7m), 하필 주정차
		//   판정이 가장 필요한 순간이 그때다. 이 행들을 버리면 도착 정차가 통째로 누락된다
		//   (실측: 000376_20260819094414 의 도착 정차 17점·79초가 전부 RAW_VLD=false 였음).
		//   맵매칭(다른 3종 과금)은 종전대로 SKIP — 좌표를 링크에 붙이는 일은 정확도가 필요하지만,
		//   "폴리곤 안에 있었나"는 그보다 훨씬 큰 공간 판정이라 성격이 다르다.
		//   GPS_LAT/LON 자체가 NULL 이면 판정 불가라 제외.
		if (!stRawLogInfo.bGpsLatNull && !stRawLogInfo.bGpsLonNull)
		{
			// [2026-10-04 최정우 추가, 사용자 확정 — E18] 이 SKIP tick(RAW_VLD=false·정확도 초과·매칭 실패)이 주정차
			//   폴리곤 안이면 순번을 남긴다 — ProcessNodeStepCharge() 가 이 구간을 일반도로로 잇지 않는다(주정차 정보에만).
			//   매칭 신뢰도 보정으로 잠깐 SKIP 됐다 복원되는 tick(clamp low-confidence 등)은 이 경로가 아니라 대상이 아니다.
			if (IsRawInParkingZone(stRawLogInfo.dfX, stRawLogInfo.dfY, stRawLogInfo.nAccuracyM))
				stSession.dwSkipInParkSeq = stRawLogInfo.dwSeqNo;
			ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts, bTrustedTripEnd);
		}
		// 보정판단용 "다음" 링크 없음(이 행은 raw_vld=false라 신뢰 못함) — 보류 행 계산된 값 그대로 커밋 (2026-08-21 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
	}

	// config radius_skip — ACCURACY_M 초과 시 SKIP. 세션 앵커 미갱신 (2026-07-10 최정우 수정)
	// 검색반경 아님. 0=비활성 (2026-07-08 최정우)
	// [2026-09-17 최정우 정리] 여기 있던 #if 0 블록(존재하지 않는 멤버 nRadiusSkipM 을 참조하던
	//   구 코드)을 제거했다 — 컴파일에 포함되지 않아 동작 변화 없음. 현행 필드명은 nRadiusSkip.
	if ((m_stConfig.nRadiusSkip > 0) &&
		(stRawLogInfo.nAccuracyM >= 0) && 
		(stRawLogInfo.nAccuracyM > m_stConfig.nRadiusSkip))
	{
		LOGFMTW("[#%02d] reject accuracy_m over skip!device=[%s] trip_id=[%s] seq=[%u] accuracy_m=[%d] radius_skip=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<int>(stRawLogInfo.nAccuracyM), m_stConfig.nRadiusSkip);
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bLastPointOk = false;			// (2026-07-21 최정우 추가)
		if (bTrustedTripEnd)
		{
			*pbTripEnded = true;
			FlushNodeStepRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
			FlushOpenExemptRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
		}

		// [버그 수정, 2026-09-15 최정우, 사용자 지적] 주정차 판정을 이 경로에도 태운다 — 위
		//   ShouldSkipGpsInput(RAW_VLD) 분기가 2026-08-22 에 받은 처리와 **완전히 같은 근거**인데
		//   정확도 초과 SKIP 분기에만 빠져 있었다. 차가 멈추면 GPS 가 측위를 놓쳐 ACCURACY_M 이
		//   커지는데, 하필 그때가 주정차 판정이 가장 필요한 순간이다. 빠져 있으면 열린 주정차
		//   세션이 도착 정차 구간 내내 갱신·마감되지 못해 통째로 유실된다 — 실측
		//   000376_20260819094414: seq93 에 RL-Z00001 진입 후 seq94~150(정확도 78~321m)이 전부
		//   이 분기로 빠져 dwell 기록이 한 건도 남지 않았다(도착 정차 전량 누락).
		//   좌표를 링크에 붙이는 맵매칭과 달리 "폴리곤 안에 있었나"는 훨씬 큰 공간 판정이라
		//   정확도 요구 수준이 다르다는 근거도 동일하다. GPS_LAT/LON 이 NULL 이면 판정 불가라 제외.
		if (!stRawLogInfo.bGpsLatNull && !stRawLogInfo.bGpsLonNull)
		{
			// [2026-10-04 최정우 추가, 사용자 확정 — E18] 이 SKIP tick(RAW_VLD=false·정확도 초과·매칭 실패)이 주정차
			//   폴리곤 안이면 순번을 남긴다 — ProcessNodeStepCharge() 가 이 구간을 일반도로로 잇지 않는다(주정차 정보에만).
			//   매칭 신뢰도 보정으로 잠깐 SKIP 됐다 복원되는 tick(clamp low-confidence 등)은 이 경로가 아니라 대상이 아니다.
			if (IsRawInParkingZone(stRawLogInfo.dfX, stRawLogInfo.dfY, stRawLogInfo.nAccuracyM))
				stSession.dwSkipInParkSeq = stRawLogInfo.dwSeqNo;
			ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts, bTrustedTripEnd);
		}

		// 정확도 SKIP — 최근접 있으면 참고용 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장.
		//   ACCURACY_M 자체가 나빠 매칭 확정(MATCH_STATUS=MATCHED)은 여전히 못 하지만(정확도 우선
		//   원칙 유지, 사용자 지시), heading 을 무시한 순수 기하 최근접 대신 heading 을 살려 방향이
		//   맞는 후보를 우선하면 참고 좌표 품질이 개선된다 — "완전히 못 믿진 않되 전적으로 믿지도
		//   않는" 참고용 활용 (2026-09-04 최정우 추가, 사용자 지시)
		MATCH_LINK_INFO stNear;
		memset(reinterpret_cast<void *>(&stNear), 0, MATCH_LINK_INFO_SIZE);
		stNear.dfIntersectLenSgmt = -1.0;
		CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
		// 보정판단용 "다음" 링크 없음(정확도 SKIP이라 신뢰 못함) — 보류 행 계산된 값 그대로 커밋 (2026-08-21 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
		// NODE_STEP 케이스3(SKIP 구간 브릿지)용 raw tick 버퍼 적립 — 완전 매칭실패(!bMatched)와 동일
		//   메커니즘을 ACCURACY_M SKIP(매칭 시도 자체를 안 함)에도 확장 적용. 다음 신뢰매칭이 확정되면
		//   (CommitPendingRow) 이 구간이 "이전 확정 링크(FROM)→다음 확정 링크(TO)" 를 근거로 재매칭·
		//   그래프탐색·직선거리 3단계 fallback(ResolveSkipGapNodeStep)을 거쳐 일반도로 과금으로
		//   등록 시도된다 — MATCH_STATUS 자체는 여전히 SKIP 이지만(정확도 우선 원칙 유지), 과금
		//   판정에는 그 구간이 반영된다 (2026-09-04 최정우 추가, 사용자 지시)
		static const size_t MM_SKIPGAP_MAX_BUFFER_TICKS = 300;
		// **원시 GPS 가 주정차 폴리곤 안이면 버퍼에 넣지 않는다** — 그 tick 은 일반도로가 아니라
		//   주정차에 속한다(사용자 지시, 2026-09-06). 정확도 초과로 거부된 tick 은 매칭좌표를 믿을
		//   근거가 없으므로 원시좌표로 판정한다("MATCHED tick 은 매칭좌표, 신뢰 못 하는 tick 은
		//   원시좌표"). 넣어두면 다음 신뢰매칭이 확정될 때 이 tick 의 참고용 링크가 일반도로 run 의
		//   출발점이 되어, 실측 tick 이 하나도 없는 구간에 지오메트리 추정치만으로 과금이 생긴다 —
		//   실측 000376_20260819140532: seq1~3(정확도 100·180·53m 로 전부 거부, 원시좌표는 폴리곤
		//   안)이 버퍼에 들어가 seq3 의 참고 링크 2040425401 에서 출발한 27m 일반도로 레코드가
		//   seq4~5(매칭 자체가 없는 구간)에 얹혔다. 누락 링크 복구 로직 자체는 그대로 둔다 —
		//   출발점이 신뢰할 수 있는 tick 일 때는 계속 동작해야 한다.
		bool bRawInParkForSkipBuf = false;
		if (m_stConfig.pcChargeDataLoader != nullptr)
		{
			vector<PZONE_INFO> vtParkChk;
			m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(
				stRawLogInfo.dfX, stRawLogInfo.dfY, 0.0, &vtParkChk);
			bRawInParkForSkipBuf = !vtParkChk.empty();
		}
		if (!bRawInParkForSkipBuf
			&& (stSession.vtSkipRunRawLogInfo.size() < MM_SKIPGAP_MAX_BUFFER_TICKS))
			stSession.vtSkipRunRawLogInfo.push_back(stRawLogInfo);
		bool bAccSkipAppended;
		if (cPM.FindNearestSegment(stRawLogInfo, &stNear, false))
		{
			int nNearLen = CalcIntersectLen(stRawLogInfo, stNear.dfMatchX, stNear.dfMatchY);
			bAccSkipAppended = AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP, nNearLen,
				&stNear.dfMatchY, &stNear.dfMatchX, stNear.qwLinkID);
		}
		else
		{
			bAccSkipAppended = AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
		}
		// vtSkipRunRawLogInfo 와 1:1 대응 인덱스 — 소급 MATCHED 승격 시 이 행을 직접 찾아 고쳐씀
		//   (2026-09-04 최정우 추가)
		// [버그 수정, 2026-09-10 최정우] 위 vtSkipRunRawLogInfo push 조건엔
		//   !bRawInParkForSkipBuf 가 있는데 여기는 없어서, 주정차 폴리곤 안 tick이 섞인 SKIP
		//   런에서 두 벡터 크기가 어긋났다 — 소비부(CommitPendingRow() 의 accuracy-skip directional bridge)의 size-일치 가드가 그 어긋남
		//   자체로 인한 오염은 막아주지만, 대신 그 SKIP 런 전체의 소급 MATCHED 승격 기능이
		//   조용히(로그도 없이) 무력화됐다(최소 재현으로 확인). 두 조건을 동일하게 맞춘다.
		if (!bRawInParkForSkipBuf && bAccSkipAppended
			&& (stSession.vtSkipRunUpdateIdx.size() < MM_SKIPGAP_MAX_BUFFER_TICKS))
			stSession.vtSkipRunUpdateIdx.push_back(pvtUpdates->size() - 1);
		return bAccSkipAppended;
	}

	// 이동거리 환산속도 vs SPEED_KMH 정합성 검사 — 이상치 GPS SKIP. 세션 앵커 미갱신 (2026-07-20 최정우 추가)
	int nImpliedSpeedKmh = -1;
	if (ShouldSkipImplausibleSpeed(nThreadId, stRawLogInfo, stSession, &nImpliedSpeedKmh))
	{
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bLastPointOk = false;			// (2026-07-21 최정우 추가)
		if (bTrustedTripEnd)
		{
			*pbTripEnded = true;
			FlushNodeStepRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
			FlushOpenExemptRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
		}

		// [버그 수정, 2026-09-17 최정우] 이 분기에도 주정차 판정을 태운다 — 나머지 SKIP 분기
		//   (RAW_VLD·정확도)엔 이미 있는데 여기만 빠져 있었다. 주정차는 맵매칭 결과를 전혀 쓰지
		//   않고 원시 GPS 좌표로만 판정하므로(ProcessParkingCharge 헤더 주석 참고), 이 tick 이
		//   매칭에서 제외되는 것과 무관하게 판정은 계속돼야 한다.
		//   빠져 있으면 **이 tick 이 트립종료(TRIP_EVENT=2)일 때 열려 있던 주정차 구간이 과금 행
		//   하나 없이 사라진다** — 위 bTrustedTripEnd 블록이 *pbTripEnded 를 세워 run() 이 세션을
		//   erase 하는데, 정작 마감해줄 주체가 없기 때문이다. run() 의 잔여 tick 강제마감 가드는
		//   "종료 tick 보다 뒤의 tick 이 있을 때"만 도는 터라 종료 tick 이 마지막이면 발동하지 않는다.
		//   실측 000385_20260917140801(CAR000447, 제2판교 RL-Z00001 폴리곤 안 363초 정차):
		//   종료 tick(seq60)이 정차 중 매칭점 오프셋(raw↔매칭점 32m — 도로 밖 주차 구역이라
		//   가장 가까운 도로로 스냅된 정상 동작) 때문에 환산속도 30.8km/h 로 걸러지면서
		//   prim_chargehand 가 0 건이 됐다. 원격 실서버 실데이터로 확인 후 로컬 재현까지 완료.
		//   나머지 5유형은 위 두 Flush 함수와 상단 트립종료 공통 블록이 이미 커버하므로
		//   이 분기에서 손볼 것은 주정차뿐이다.
		if (!stRawLogInfo.bGpsLatNull && !stRawLogInfo.bGpsLonNull)
		{
			// [2026-10-04 최정우 추가, 사용자 확정 — E18] 이 SKIP tick(RAW_VLD=false·정확도 초과·매칭 실패)이 주정차
			//   폴리곤 안이면 순번을 남긴다 — ProcessNodeStepCharge() 가 이 구간을 일반도로로 잇지 않는다(주정차 정보에만).
			//   매칭 신뢰도 보정으로 잠깐 SKIP 됐다 복원되는 tick(clamp low-confidence 등)은 이 경로가 아니라 대상이 아니다.
			if (IsRawInParkingZone(stRawLogInfo.dfX, stRawLogInfo.dfY, stRawLogInfo.nAccuracyM))
				stSession.dwSkipInParkSeq = stRawLogInfo.dwSeqNo;
			ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts, bTrustedTripEnd);
		}

		// 정합성 SKIP — 최근접 있으면 참고용 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
		MATCH_LINK_INFO stNear;
		memset(reinterpret_cast<void *>(&stNear), 0, MATCH_LINK_INFO_SIZE);
		stNear.dfIntersectLenSgmt = -1.0;
		CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
		// 보정판단용 "다음" 링크 없음(정합성 SKIP이라 신뢰 못함) — 보류 행 계산된 값 그대로 커밋 (2026-08-21 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
		if (cPM.FindNearestSegment(stRawLogInfo, &stNear))
		{
			int nNearLen = CalcIntersectLen(stRawLogInfo, stNear.dfMatchX, stNear.dfMatchY);
			return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP, nNearLen,
				&stNear.dfMatchY, &stNear.dfMatchX, stNear.qwLinkID);
		}
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
	}

	// (D) 장시간 공백 시 세션 앵커 폐기 → 연속성 끊고 초기(Begin) 재획득 (2026-07-15 최정우 추가)
	//   직전 "매칭 성공" 이후 gap 이 MM_SESSION_RESET_GAP_SEC 초과면 위치 불확실 → 앵커·링크 리셋
	if (stSession.bHasLastMatch && (stSession.dtLastMatchGps > 0))
	{
		double dfSessGapSec = difftime(stRawLogInfo.dtGPS, stSession.dtLastMatchGps);
		if (dfSessGapSec > static_cast<double>(MM_SESSION_RESET_GAP_SEC))
		{
			LOGFMTD("[#%02d] session gap reset! device=[%s] trip_id=[%s] seq=[%u] gap=[%.0fs]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRawLogInfo.dwSeqNo, dfSessGapSec);
			stSession.qwLinkID = 0;
			stSession.dfLastMatchX = 0.0;
			stSession.dfLastMatchY = 0.0;
			stSession.dtLastMatchGps = 0;
			stSession.dwLastMatchGpsSeq = 0;
			stSession.bHasLastMatch = false;
			stSession.bHasPrevAlt = false;
		}
	}

	sint16 nFinalStatus = MATCH_STATUS_MATCHED;
	MATCH_LINK_INFO stMatchLinkInfo;
	memset(reinterpret_cast<void *>(&stMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);
	stMatchLinkInfo.dfIntersectLenSgmt = -1.0;

	// 1 GPS 맵매칭 처리시간 측정 (nMatchTimeoutMs>0 시 초과 포인트 ERROR 격리)
	// 맵매칭 처리 시간 측정 시작 (2026-07-08 최정우 주석 추가)
	CClock cMatchClock;
	cMatchClock.Start();
	// 역행 스트릭 종료(=노이즈였음) 감지용 — RunMapMatch 가 내부에서 nReverseStreak 를 갱신해버리므로
	//   호출 "전" 값을 미리 스냅샷해둔다 (2026-09-04 최정우 추가)
	const int nPrevReverseStreak = stSession.nReverseStreak;
	// ProcessManager 경유 시작/Continue 맵매칭 (2026-07-08 최정우 주석 추가)
	bool bMatched = RunMapMatch(nThreadId, stRawLogInfo, &stSession, &stMatchLinkInfo);
	// 맵매칭 처리 시간 측정 종료 (2026-07-08 최정우 주석 추가)
	cMatchClock.Stop();

	// 반경 밖·진단반경 초과 최근접 — MATCHED 아님, SKIP(3)·세션 미갱신·MATCH_LAT/LON·INTERSECT_LEN 저장 (2026-07-10 최정우 수정)
	const bool bOut = (!bMatched) && stMatchLinkInfo.bOutOfRadius;
	// 연속 역행 미확정(reverse_confirm 미만) — SKIP·세션 앵커 고정. RunMapMatch 가 이미 nReverseStreak 갱신.
	//   bReverseSuspect(위치+heading 둘 다 역행) 기준으로 GPS 노이즈로 인한 오탐을 줄임 (2026-07-21 최정우 수정)
	const bool bReverseSkip = bMatched && stMatchLinkInfo.bReverseSuspect
		&& (stSession.nReverseStreak < m_stConfig.nReverseConfirm);
	// Continue 실패 후 Begin 폴백(위상 연결 미검증)으로 확정된 결과의 이동거리 타당성 — SKIP 판정용
	//   (2026-09-04 최정우 추가)
	const bool bFallbackJumpImplausible = bMatched
		&& IsFallbackJumpImplausible(nThreadId, stRawLogInfo, stSession, stMatchLinkInfo);
	// raw GPS가 등록 주정차구역 폴리곤 안인데 매칭 좌표는 그 밖 — SKIP 판정용 (2026-09-04 최정우 추가)
	const bool bOutsideRawZone = bMatched
		&& IsMatchOutsideRawZonePolygon(nThreadId, stRawLogInfo, stMatchLinkInfo);

	// 역행 스트릭이 reverse_confirm 미달로 끊기고 정상(비역행) 매칭으로 복귀 — 그 사이 SKIP됐던
	//   틱들은 결국 노이즈였다는 뜻이므로, 스트릭 시작 전 마지막 확정 링크(FROM, 아직 이번 틱으로
	//   덮어써지기 전)→이번 복귀 링크(TO)로 방향검증 재매칭(RematchBeginBiasedDirectional, heading
	//   신뢰 불가 시 자동 포기)해 성공한 틱만 소급 MATCHED. 클램프 브릿지와 달리 "인접" 사전조건이
	//   없다 — 스트릭 종료 자체가 이미 "노이즈였다"는 판정이고, 재매칭 결과가 TO와 정확히 일치할
	//   때만 채택하는 이중 검증으로 신뢰도를 확보한다 (2026-09-04 최정우 추가, 사용자 지시)
	//   nPrevReverseStreak 가 버퍼 크기와 정확히 같아야 한다 — 스트릭이 중간에 reverse_confirm 에
	//   도달해 "진짜 역행"으로 한 번이라도 확정된 적이 있으면(그 확정 틱 이후로도 bReverseSuspect
	//   가 이어지면 스트릭은 계속 증가하지만 확정 틱 자체는 버퍼링되지 않아 크기가 안 맞음) 앞의
	//   버퍼된 틱들도 진짜 역행이었을 가능성이 높으므로 소급재기록 대상에서 제외한다
	if (bMatched && !stMatchLinkInfo.bReverseSuspect && (nPrevReverseStreak > 0)
		&& (stSession.nReverseStreak == 0) && !stSession.vtReverseSkipRunRawLogInfo.empty()
		&& (nPrevReverseStreak == static_cast<int>(stSession.vtReverseSkipRunRawLogInfo.size()))
		&& (m_stConfig.pcProcessManager != nullptr)
		&& (stSession.vtReverseSkipRunUpdateIdx.size() == stSession.vtReverseSkipRunRawLogInfo.size()))
	{
		CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
		size_t nBridged = 0;
		for (size_t i = 0; i < stSession.vtReverseSkipRunRawLogInfo.size(); ++i)
		{
			size_t idx = stSession.vtReverseSkipRunUpdateIdx[i];
			if (idx >= pvtUpdates->size()) continue;

			MATCH_LINK_INFO stRematched;
			if (!cPM.RematchBeginBiasedDirectional(stSession.vtReverseSkipRunRawLogInfo[i],
					stMatchLinkInfo.qwLinkID, &stRematched)
				|| (stRematched.qwLinkID != stMatchLinkInfo.qwLinkID))
				continue;		// 재매칭 실패·TO 불일치·heading 신뢰 불가 — SKIP 유지

			int nNewIntersectLen = CalcIntersectLen(stSession.vtReverseSkipRunRawLogInfo[i],
				stRematched.dfMatchX, stRematched.dfMatchY);
			char szMatchLat[32], szMatchLon[32], szIntersectLen[16], szMatchLinkId[24];
			snprintf(szMatchLat, sizeof(szMatchLat), "%.06lf", stRematched.dfMatchY);
			snprintf(szMatchLon, sizeof(szMatchLon), "%.06lf", stRematched.dfMatchX);
			snprintf(szIntersectLen, sizeof(szIntersectLen), "%d", nNewIntersectLen);
			snprintf(szMatchLinkId, sizeof(szMatchLinkId), "%llu",
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));

			(*pvtUpdates)[idx].strMatchStatus = "1";
			(*pvtUpdates)[idx].strMatchLat = szMatchLat;
			(*pvtUpdates)[idx].strMatchLon = szMatchLon;
			(*pvtUpdates)[idx].strIntersectLen = szIntersectLen;
			(*pvtUpdates)[idx].strMatchLinkId = szMatchLinkId;
			++nBridged;
		}
		if (nBridged > 0)
		{
			LOGFMTW("[#%02d] reverse-skip %zu/%zu-tick directional bridge!device=[%s] trip_id=[%s] "
				"link=[%llu] (rematched, charge not retroactively processed)",
				nThreadId, nBridged, stSession.vtReverseSkipRunRawLogInfo.size(),
				stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
		}
		stSession.qwReverseSkipRunAnchorLinkID = 0;
		stSession.vtReverseSkipRunUpdateIdx.clear();
		stSession.vtReverseSkipRunRawLogInfo.clear();
	}

	if (!bMatched && bOut)
	{
		// SKIP: MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)만 DB 저장, 세션 앵커 미갱신
		LOGFMTW("[#%02d] out-of-radius skip! device=[%s] trip_id=[%s] seq=[%u] "
			"intersect_len=[%.1fm] match_lat=[%.06lf] match_lon=[%.06lf]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			stMatchLinkInfo.dfIntersectLenSgmt, stMatchLinkInfo.dfMatchY, stMatchLinkInfo.dfMatchX);
		nFinalStatus = MATCH_STATUS_SKIP;
	}
	else if (!bMatched)
	{
		// 실패: DEVICE_KEY·좌표(위경도)·에러코드·에러메시지(CodeMap 변환값) 로그
		const char *pszErrMsg = (stMatchLinkInfo.szErrorMsg[0] != '\0')
			? stMatchLinkInfo.szErrorMsg : "unknown";
		LOGFMTW("[#%02d] map match failed! device=[%s] trip_id=[%s] seq=[%u] "
			"lat=[%.06lf] lon=[%.06lf] err=[%d] msg=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			stRawLogInfo.dfY, stRawLogInfo.dfX,
			static_cast<int>(stMatchLinkInfo.wErrorCode), pszErrMsg);
		// "탐색은 정상적으로 다 했는데 주변에 후보 자체가 없는" 경우(진단반경(MM_DIAG_RADIUS)·
		//   기하 최근접까지 다 실패한 뒤 도달 — ProcessManager::AttemptMatch()/FindGeomNearestSegment()
		//   참고)는 시스템
		//   결함이 아니라 도로망 데이터 공백(예: 실측 000093_20260818074500 seq9, 한강 교량 구간
		//   누락)이므로 ERROR(4) 대신 SKIP(3)으로 기록한다. INVALID_COORDTYPE 등(1~8) 입력·설정
		//   오류는 여전히 ERROR로 남겨 실제 결함과 구분한다 (2026-08-27 최정우 추가, 사용자 지시)
		nFinalStatus = ((stMatchLinkInfo.wErrorCode == MAP_MATCH_FAIL)
			|| (stMatchLinkInfo.wErrorCode == NOT_FOUND_GRIDINFO)
			|| (stMatchLinkInfo.wErrorCode == NOT_FOUND_LINKID))
			? MATCH_STATUS_SKIP : MATCH_STATUS_ERROR;
	}
	else
	{
		// bMatched == true 인 경우, 아래 두 검사(타임아웃/역행 SKIP)는 각각 독립적으로 실행되어야 한다.
		//
		// [버그였던 예전 코드]                          [문제]
		//   else if (nMatchTimeoutMs > 0) { ... }         "타임아웃 검사 기능이 켜져 있다"는 조건만으로
		//   else if (bReverseSkip)        { ... }         이 가지로 들어가 버려서, 실제로는 200ms를
		//                                                  안 넘겨 안에서 아무 것도 안 해도 바로 아래
		//                                                  else if(bReverseSkip) 는 검사조차 못 받고
		//                                                  건너뛰어짐 → reverse_confirm 기반 SKIP이
		//                                                  timeout 기능이 켜져 있는 한 항상 무력화됨.
		//
		// [수정] else if 로 나란히 두지 않고, if 두 개로 분리해서 둘 다 매번 검사되게 함
		//   (2026-07-21 최정우 수정 — 인위적 역행 테스트로 발견)
		if (m_stConfig.nMatchTimeoutMs > 0)
		{
			const double dfElapsedMs = cMatchClock.GetElapsedTime() * 1000.0;
			if (dfElapsedMs > static_cast<double>(m_stConfig.nMatchTimeoutMs))
			{
				LOGFMTW("[#%02d] map match timeout! elapsed=[%.1fms] threshold=[%dms] seq=[%u] device=[%s] trip_id=[%s]",
					nThreadId, dfElapsedMs, m_stConfig.nMatchTimeoutMs,
					stRawLogInfo.dwSeqNo, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID);
				nFinalStatus = MATCH_STATUS_ERROR;
				bMatched = false;   // MATCH_LAT/LON 미기록(격리)
			}
		}

		if (bMatched && bReverseSkip)
		{
			// 연속 역행이 reverse_confirm 미만 — 노이즈 취급, SKIP 저장(좌표는 원본 그대로)·세션 앵커 미갱신 (2026-07-21 최정우 추가)
			LOGFMTW("[#%02d] reverse hit! device=[%s] trip_id=[%s] seq=[%u] streak=[%d/%d] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				stSession.nReverseStreak, m_stConfig.nReverseConfirm,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// 경계 클램프 + GPS와 거리가 먼 저신뢰 매칭 — SKIP 처리. 여러 GPS_SEQ 가 같은 꺾임점으로
		//   뭉개져 실제로는 계속 이동 중인데도 MATCH_LAT/LON 이 정지한 것처럼 보이는 오탐(예: 주정차
		//   오판) 을 다운스트림에서 MATCHED 로 신뢰하지 않도록 함. 세션 앵커(qwLinkID 등)는 그대로
		//   갱신 — 엔진 내부 연속 매칭 추적은 방해하지 않고, DB 저장값만 SKIP 으로 표시 (2026-07-21 최정우 추가)
		if (bMatched && stMatchLinkInfo.bClampLowConf)
		{
			LOGFMTW("[#%02d] clamp low-confidence! device=[%s] trip_id=[%s] seq=[%u] "
				"intersect_len=[%.1fm] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				stMatchLinkInfo.dfIntersectLenSgmt,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// 같은 링크 역행인데 heading 없음/각도 애매해 "확실한 노이즈"로 단정 못 하는 경우 — SKIP 처리.
		//   좌표는 계산된 값 그대로 저장(원본 GPS 대비 위치는 정상), 신뢰도만 낮게 표시 (2026-07-22 최정우 추가)
		if (bMatched && stMatchLinkInfo.bAmbiguousReverse)
		{
			LOGFMTW("[#%02d] ambiguous reverse! device=[%s] trip_id=[%s] seq=[%u] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// Continue(위상 그래프 연속매칭)가 직전 링크와의 연결을 못 찾아 Begin(반경 최근접) 폴백으로
		//   확정된 결과인데, 직전 확정 위치 대비 이동거리가 비현실적이면 SKIP 처리 — "확신 없는
		//   매칭보다 SKIP" 원칙 (2026-09-04 최정우 추가)
		if (bMatched && bFallbackJumpImplausible)
			nFinalStatus = MATCH_STATUS_SKIP;

		// depth 탐색 재구성 경로가 경과시간 대비 절대 물리속도로 불가능한 경우 — 최종 링크 자체를
		//   신뢰 못 함. SKIP 처리 (2026-09-04 최정우 추가, MM_PATH_ABS_MAX_KMH 주석 참고)
		if (bMatched && stMatchLinkInfo.bImplausiblePath)
		{
			LOGFMTW("[#%02d] implausible path abs speed! device=[%s] trip_id=[%s] seq=[%u] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// 재구성 경로 전체 방향이 현재 heading 과 크게 어긋나는 경우 — 시간·거리는 타당해도 실제
		//   주행 경로로 보기 어려움. SKIP 처리 (2026-09-04 최정우 추가, 사용자 지시)
		if (bMatched && stMatchLinkInfo.bImplausibleDirection)
		{
			LOGFMTW("[#%02d] implausible path direction! device=[%s] trip_id=[%s] seq=[%u] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// 재구성 경로 구간(hop)별 등록 제한속도 기준 최소 소요시간이 실제 경과시간보다 큰 경우 —
		//   평균속도는 상한 이내여도 유독 느린 구간 하나만으로 물리적으로 불가능함. SKIP 처리
		//   (2026-09-04 최정우 추가, 사용자 지시)
		if (bMatched && stMatchLinkInfo.bImplausibleSpeedLimit)
		{
			LOGFMTW("[#%02d] implausible path speed limit! device=[%s] trip_id=[%s] seq=[%u] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// raw GPS가 등록 주정차구역 폴리곤 안인데 매칭 좌표는 그 밖 — 저속에서 raw가 매칭보다
		//   신뢰할 만함. SKIP 처리. 세션 앵커(qwLinkID)는 bClampLowConf 와 동일 관례로 그대로 전진
		//   (2026-09-04 최정우 추가, 사용자 지시)
		if (bOutsideRawZone)
			nFinalStatus = MATCH_STATUS_SKIP;
	}

	// 신뢰 못 하는 좌표(역행 미확정·클램프 저신뢰·역행 판단불가·연결성 미검증 이동거리 비현실·
	//   재구성경로 절대속도·방향·구간제한속도 비현실·raw구역폴리곤 이탈)는 다음 포인트의
	//   HEADING/SPEED/이상속도 검사 기준으로 쓰지 않음 (2026-07-22 최정우 수정 — 역행 판단불가
	//   케이스 추가, 2026-09-04 최정우 수정 — 폴백 점프·재구성경로 절대속도·방향·구간제한속도·
	//   raw구역폴리곤 이탈 케이스 추가)
	const bool bUntrustedMatch = bReverseSkip || stMatchLinkInfo.bClampLowConf
		|| stMatchLinkInfo.bImplausiblePath || stMatchLinkInfo.bImplausibleDirection
		|| stMatchLinkInfo.bImplausibleSpeedLimit || bOutsideRawZone
		|| stMatchLinkInfo.bAmbiguousReverse || bFallbackJumpImplausible;

	stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
	// 다음 포인트의 이상속도 검사 신뢰도 판단용 — 앵커 갱신 여부와 동일 조건 (2026-07-21 최정우 추가)
	stSession.bLastPointOk = (bMatched && !bUntrustedMatch);

	// 주정차 판정 — 원시 GPS·속도가 기본이지만, 규칙2(매칭 좌표도 폴리곤 내)·규칙4(매칭 좌표가
	//   폴리곤 밖이면 통과로 보고 해제)를 위해 매칭 결과가 필요해 맵매칭 "이후"로 옮겼다.
	//   맵매칭 성공/실패와 무관하게 항상 평가하는 성질은 그대로 — 실패 시 bMatchTrusted=false 로
	//   넘겨 원시 좌표만으로 규칙1 판정한다 (2026-08-22 최정우 수정, 원래는 맵매칭 전 호출)
	{
		const bool bParkMatchOk = (bMatched && !bUntrustedMatch);
		ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts, bTrustedTripEnd,
			bParkMatchOk, bParkMatchOk ? stMatchLinkInfo.dfMatchX : 0.0,
			bParkMatchOk ? stMatchLinkInfo.dfMatchY : 0.0);
	}

	// ── 매칭 성공(MATCHED) 시에만 세션 앵커 갱신 — SKIP/ERROR 는 직전 성공 앵커 유지 (2026-07-10 최정우 수정) ──
	//   · XY·시각: HEADING/SPEED 보정용 (ALTITUDE_M NULL이어도 갱신)
	//   · 고도: ALTITUDE_M 유효 시에만 nPrevAltitude·nPrevRoadType 저장
	//     (직전 매칭 좌표에 Z 없음 — GPS 고도를 앵커로 기억)
	//   · 직전 고도 없이 현재만 있으면 bHasPrevAlt=false → 고도 점수 스킵
	//   · 역행 미확정(bReverseSkip)·클램프 저신뢰 시에도 이 앵커는 갱신하지 않음 — 다음 포인트가
	//     오염된 좌표를 기준으로 HEADING/SPEED 를 잘못 계산하지 않도록 (2026-07-21 최정우 수정)
	//   · 이 앵커는 맵매칭 엔진(RunMapMatch, 다음 GPS 의 고도보조 점수)이 실시간으로 계속 써야
	//     해서 절대 지연 불가 — 과금 함수 호출·DB 반영만 아래에서 1틱 보류한다 (2026-08-21 최정우 수정)
	if (bMatched && !bUntrustedMatch)
	{
		// 과금 함수용 "직전 매칭 위치·시각" 스냅샷 — 바로 아래서 최신값으로 덮어쓰기 전에, 이번
		//   행을 보류(pending) 커밋할 때 쓸 "그 당시" 값을 미리 저장해둔다. 기존엔 이 스냅샷이
		//   필요 없었다(과금 함수를 그 자리에서 곧바로 호출했으므로) — 1틱 지연커밋 도입으로
		//   보류 시점엔 세션 앵커가 이미 몇 틱 전진해있어 스냅샷이 꼭 필요함 (2026-08-21 최정우 추가)
		const double dfPrevMatchX = stSession.dfLastMatchX;
		const double dfPrevMatchY = stSession.dfLastMatchY;
		const time_t dtPrevMatchGps = stSession.dtLastMatchGps;
		const uint32 dwPrevMatchGpsSeq = stSession.dwLastMatchGpsSeq;
		const bool bPrevHasMatch = stSession.bHasLastMatch;

		stSession.dfLastMatchX = stMatchLinkInfo.dfMatchX;
		stSession.dfLastMatchY = stMatchLinkInfo.dfMatchY;
		stSession.dtLastMatchGps = stRawLogInfo.dtGPS;
		stSession.dwLastMatchGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bHasLastMatch = true;
		if (stRawLogInfo.nAltitudeM >= 0)
		{
			stSession.nPrevAltitude = stRawLogInfo.nAltitudeM;
			stSession.nPrevRoadType = stMatchLinkInfo.nRoadType;
			stSession.bHasPrevAlt = true;
		}

		// 기존에 보류돼 있던 행을 먼저 확정(commit) — 이번 행의 확정 링크를 "다음" 참고로 보정
		//   판단(반대편 짝 링크 1틱 오매칭이면 SKIP 처리) (2026-08-21 최정우 추가). 매칭좌표도 함께
		//   넘겨 클램프 브릿지의 전.후 확정좌표 방향검증 대안 경로가 쓸 수 있게 한다 (2026-09-04 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, true, stMatchLinkInfo.qwLinkID, pvtUpdates, pvtChargeInserts,
			stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY);

		// 이번 행은 즉시 과금 처리·DB 반영하지 않고 세션에 1틱 보류 — 다음 GPS 가 오면 그때 위
		//   로직으로 확정된다(반대편 짝 링크 1틱 오매칭 보정, [[project_mapmatch_opposite_link_and_begin_heading]]
		//   과 별개 후속 조치) (2026-08-21 최정우 추가)
		stSession.bHasPendingCommit = true;
		stSession.nPendingHoldTicks = 0;						// 새 보류 행 시작 — 연장 카운트 초기화 (2026-08-26 최정우 추가)
		stSession.stPendingRawLogInfo = stRawLogInfo;
		stSession.stPendingMatchLinkInfo = stMatchLinkInfo;
		stSession.nPendingFinalStatus = nFinalStatus;
		stSession.nPendingIntersectLen = CalcIntersectLen(stRawLogInfo,
			stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY);
		stSession.bPendingHasCoords = true;
		stSession.dfPendingPrevMatchX = dfPrevMatchX;
		stSession.dfPendingPrevMatchY = dfPrevMatchY;
		stSession.dtPendingPrevMatchGps = dtPrevMatchGps;
		stSession.dwPendingPrevMatchGpsSeq = dwPrevMatchGpsSeq;
		stSession.bPendingHadLastMatch = bPrevHasMatch;

		// 트립 종료(TRIP_EVENT=2) — 더 이상 "다음" GPS 가 안 올 수 있으므로 보정판단 없이 즉시 확정 (2026-08-21 최정우 추가)
		//   bTrustedTripEnd 로 스퓨리어스 END 는 제외(2026-08-25)
		if (bTrustedTripEnd)
		{
			CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
			*pbTripEnded = true;
		}

		return true;
	}

	// SKIP/ERROR(또는 bUntrustedMatch) — 이 행 자체는 보정판단에 못 쓰지만(신뢰 못하는 매칭),
	//   보류 중이던 "다른" 행이 있으면 곧바로 포기하지 않고 MM_PENDING_MAX_HOLD_TICKS 틱까지는
	//   계속 보류를 연장해 그 다음 정상 매칭이 나올 때까지 기다린다 — SKIP 한 틱 때문에 경로
	//   일관성 보정(CommitPendingRow 의 "경로 불연속 보류행 보정") 기회를 놓치던 사각지대 보완
	//   (2026-08-21 최정우 1틱 지연 도입, 2026-08-26 최정우 보류연장 추가 — 실측
	//   000376_20260826152113 M260/M261/M262)
	//   트립종료(bTrustedTripEnd)면 더 이상 "다음" GPS 가 안 오므로 보류 연장 없이 즉시 확정한다
	//   (안 그러면 보류 행이 커밋 한 번 못 받고 세션과 함께 유실됨) (2026-08-26 최정우 추가)
	if (stSession.bHasPendingCommit && !bTrustedTripEnd
		&& (stSession.nPendingHoldTicks < MM_PENDING_MAX_HOLD_TICKS))
	{
		stSession.nPendingHoldTicks += 1;
	}
	else
	{
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
	}

	// INTERSECT_LEN: GPS↔세그먼트 교차점(MATCH_LAT/LON) 하버사인 거리(m) → 정수 반올림
	const bool bHasCoords = (bMatched || bOut);
	int nIntersectLen = -1;
	if (bHasCoords)
		nIntersectLen = CalcIntersectLen(stRawLogInfo,
			stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY);

	// DB 저장: MATCHED/SKIP 시 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)·MATCH_LINK_ID. ERROR 는 미저장
	if (!AppendUpdateRow(pvtUpdates, stRawLogInfo, nFinalStatus, nIntersectLen,
		bHasCoords ? &stMatchLinkInfo.dfMatchY : nullptr,
		bHasCoords ? &stMatchLinkInfo.dfMatchX : nullptr,
		bHasCoords ? stMatchLinkInfo.qwLinkID : 0))
		return false;

	// 같은 링크 ambiguous-reverse SKIP 브릿지 후보 적립 — 해소/폐기는 CommitPendingRow 가 "다음"
	//   확정 링크를 볼 때 처리한다(사용자 지시, 2026-08-28 최정우 추가). bAmbiguousReverse 는 정의상
	//   항상 세션 연속매칭 앵커와 같은 링크에서만 발생하므로 링크 불일치 검사 없이 바로 적립해도 안전
	if (stMatchLinkInfo.bAmbiguousReverse)
	{
		if (stSession.qwAmbigReverseRunLinkID != stMatchLinkInfo.qwLinkID)
		{
			stSession.qwAmbigReverseRunLinkID = stMatchLinkInfo.qwLinkID;
			stSession.vtAmbigReverseRunIdx.clear();
		}
		stSession.vtAmbigReverseRunIdx.push_back(pvtUpdates->size() - 1);
	}

	// 경계 클램프(bClampLowConf) SKIP 브릿지 후보 적립 — 해소는 CommitPendingRow 가 "다음" 확정
	//   링크가 이 클램프 링크와 실제로 인접(1-hop 직결)한지 확인한 뒤 RematchBeginBiased 로
	//   재매칭한다. 원본 GPS 를 같이 보관해야 재매칭이 가능함 (사용자 지시, 2026-08-28 최정우 추가)
	if (stMatchLinkInfo.bClampLowConf)
	{
		if (stSession.qwClampRunLinkID != stMatchLinkInfo.qwLinkID)
		{
			stSession.qwClampRunLinkID = stMatchLinkInfo.qwLinkID;
			stSession.vtClampRunUpdateIdx.clear();
			stSession.vtClampRunRawLogInfo.clear();
			// 런 시작 시점의 직전 신뢰 매칭 좌표 스냅샷 — bClampLowConf 는 bUntrustedMatch 구성요소라
			//   dfLastMatchX/Y 가 이 tick 으로는 전진하지 않으므로, 지금 읽는 값이 곧 "런 시작 전
			//   마지막으로 신뢰됐던 위치"다. 트립 첫 tick부터 바로 클램프면 아직 신뢰 매칭 자체가
			//   없어(bHasLastMatch=false) 무효로 표시 (2026-09-04 최정우 추가)
			stSession.dfClampRunEntryX = stSession.dfLastMatchX;
			stSession.dfClampRunEntryY = stSession.dfLastMatchY;
			stSession.bClampRunEntryValid = stSession.bHasLastMatch;
		}
		stSession.vtClampRunUpdateIdx.push_back(pvtUpdates->size() - 1);
		stSession.vtClampRunRawLogInfo.push_back(stRawLogInfo);
	}

	// 역행 의심(bReverseSkip) SKIP 브릿지 후보 적립 — 스트릭이 reverse_confirm 미달로 끊기고
	//   정상 매칭으로 복귀하는 순간(아래 RunMapMatch 직후 nPrevReverseStreak 비교부) 소급 재매칭한다.
	//   qwLastConfirmedLinkID(신뢰 매칭에서만 갱신)를 앵커로 써 스트릭 시작 전 마지막 확정 링크를
	//   자연히 유지한다 (2026-09-04 최정우 추가, 사용자 지시)
	if (bReverseSkip)
	{
		if (stSession.qwReverseSkipRunAnchorLinkID != stSession.qwLastConfirmedLinkID)
		{
			stSession.qwReverseSkipRunAnchorLinkID = stSession.qwLastConfirmedLinkID;
			stSession.vtReverseSkipRunUpdateIdx.clear();
			stSession.vtReverseSkipRunRawLogInfo.clear();
		}
		stSession.vtReverseSkipRunUpdateIdx.push_back(pvtUpdates->size() - 1);
		stSession.vtReverseSkipRunRawLogInfo.push_back(stRawLogInfo);
	}

	// NODE_STEP 케이스3(SKIP 구간 브릿지)용 raw tick 버퍼 적립 — 완전 매칭실패(!bMatched)만 대상,
	//   위 두 브릿지(ambiguous-reverse/clamp)는 bMatched==true(매칭은 됐으나 저신뢰)라 겹치지 않음.
	//   qwLastConfirmedLinkID 가 바뀔 때(=갭 해소)까지 계속 이어붙임 — ResolveSkipGapNodeStep() 이
	//   소비 후 clear. MM_SKIPGAP_MAX_BUFFER_TICKS 넘으면 더 안 쌓음(끝없는 SKIP 트립의 메모리 상한,
	//   1순위 재매칭이 못 미치는 나머지는 2·3순위 fallback이 처리하므로 정확도엔 영향 없음)
	//   (2026-09-01 최정우 추가)
	if (!bMatched)
	{
		static const size_t MM_SKIPGAP_MAX_BUFFER_TICKS = 300;
		if (stSession.vtSkipRunRawLogInfo.size() < MM_SKIPGAP_MAX_BUFFER_TICKS)
		{
			stSession.vtSkipRunRawLogInfo.push_back(stRawLogInfo);
			// ACCURACY_M SKIP 분기와 동일하게 인덱스도 같이 적립 — 소급 MATCHED 승격
			//   (RematchBeginBiasedDirectional) 대상 인덱싱용 (2026-09-04 최정우 추가)
			stSession.vtSkipRunUpdateIdx.push_back(pvtUpdates->size() - 1);
		}
	}

	// END 이벤트면 MATCHED/ERROR/SKIP 무관 세션 종료 (bulk 성공 후 mapSessions.erase)
	//   bTrustedTripEnd 로 스퓨리어스 END 는 제외(2026-08-25)
	if (bTrustedTripEnd)
	{
		*pbTripEnded = true;
		// 이번 틱이 신뢰 못할 매칭(bUntrustedMatch)이거나 매칭 자체를 못 했는데(!bMatched) 트립이
		//   여기서 끝나는 경우 — 위 CommitPendingRow(line ~3084)가 커밋한 건 이번 틱이 아니라 훨씬
		//   전에 보류돼있던 행이라 ProcessNodeStepCharge가 "트립종료"를 못 보고 지나갔을 수 있다.
		//   실측 000376_20260826150010(reverse_confirm=4 로 검증 중 발견) — 마지막 틱이 역행의심
		//   미확정으로 seq408~421 NODE_STEP 구간(220m/36초) 전체가 유실됐다. bMatched && !bUntrustedMatch
		//   인 정상 경로(위쪽 3016번째 줄 근방)는 CommitPendingRow가 이미 이번 틱 자체로 Y/0 정상
		//   마감하므로 여기서 또 부르면 그 정상 결과를 N/3(AUDIT)로 덮어써버린다 — 그래서 이 안전망은
		//   반드시 이 fallback 경로(else 이후 도달)에서만 호출한다 (2026-09-03 최정우 추가)
		FlushNodeStepRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
		FlushOpenExemptRunsAtTripEnd(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
	}

	return true;
}

/**
 * @brief vtUpdates 에 PK 행 존재 여부 (배치 orphan 판별)
 * @param[in] vtUpdates bulk UPDATE 대상 목록
 * @param[in] strTripId 운행 ID (PK-1)
 * @param[in] strGpsSeq GPS 순번 (PK-2, 문자열)
 * @return true(포함), false(미포함)
*/
bool CRawLogWorker::IsRowInUpdates(const vector<RAW_LOG_UPDATE_ROW>& vtUpdates,
		const string& strTripId, const string& strGpsSeq)
{
	for (size_t i=0; i<vtUpdates.size(); ++i)
	{
		if (vtUpdates[i].strTripId == strTripId
			&& vtUpdates[i].strGpsSeq == strGpsSeq)
			return true;
	}
	return false;
}

/**
 * @brief 미처리 예약 행 release 1건 적재 [rawgps_update] $3=0
 * @param[out] pvtRelease release 대상 행 목록
 * @param[in] stRawLogInfo 원시 GPS (PK 추출용)
 * @return true(적재 성공), false(pvtRelease null·trip_id 무효)
 * @remark AppendUpdateRow 실패 등 vtUpdates 미포함 행의 PROCESSING 해제용 (#4)
*/
bool CRawLogWorker::AppendReleaseRowFromRawLog(vector<RAW_LOG_UPDATE_ROW> *pvtRelease,
		const sRawLogInfo& stRawLogInfo)
{
	if (pvtRelease == nullptr)
		return false;

	if (stRawLogInfo.szTripID[0] == '\0')
		return false;

	char szSeqNo[16];
	snprintf(szSeqNo, sizeof(szSeqNo), "%u", stRawLogInfo.dwSeqNo);

	RAW_LOG_UPDATE_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strGpsSeq = szSeqNo;
	stRow.strMatchStatus = "0";
	pvtRelease->push_back(stRow);
	return true;
}

/**
 * @brief RAW_LOG_INFO → MAP_MATCH_INPUT 변환 후 스레드별 ProcessManager 맵매칭
 * @param[in] nThreadId 워커 스레드 ID (ProcessManager 인덱스)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in,out] pstSession trip_id 세션 (qwLinkID 연속 맵매칭용)
 * @return true(매칭 성공), false(실패)
*/
bool CRawLogWorker::RunMapMatch(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, MATCH_LINK_INFO *pstMatchLinkInfo)
{
	if (pstSession == nullptr || pstMatchLinkInfo == nullptr
		|| m_stConfig.pcProcessManager == nullptr)
		return false;

	if (nThreadId < 0 || nThreadId >= m_stConfig.nWorkerThreads)
		return false;

	memset(reinterpret_cast<void *>(pstMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);
	pstMatchLinkInfo->dfIntersectLenSgmt = -1.0;
	CProcessManager& cProcessManager = m_stConfig.pcProcessManager[nThreadId];

	// 같은 링크 노이즈 보정(1m 강제전진) 억제 판별용 "직전 tick 원시좌표" 스냅샷 — 비교 자체는
	//   아래(stAdjusted.fSpeed 확정 후)에서 한다. 여기서는 세션 갱신 전 값만 먼저 떠둔다
	//   (2026-09-11 최정우 수정 — 아래 이유로 위치 이동)
	const double dfPrevTickRawX = pstSession->dfPrevTickRawX;
	const double dfPrevTickRawY = pstSession->dfPrevTickRawY;
	const sint16 nPrevTickAngle = pstSession->nPrevTickAngle;
	const bool bHadPrevTickRaw = pstSession->bHasPrevTickRaw;
	pstSession->dfPrevTickRawX = stRawLogInfo.dfX;
	pstSession->dfPrevTickRawY = stRawLogInfo.dfY;
	pstSession->nPrevTickAngle = stRawLogInfo.nAngle;
	pstSession->bHasPrevTickRaw = true;

	// ── HEADING/SPEED 보정: DB 적재값 우선, NULL(미적재) 이면 직전 매칭좌표로 계산 (2026-07-08 최정우 주석 추가) ──
	//   · 전제 : 세션에 직전 "매칭 성공" 좌표(dfLastMatchX/Y)와 그 GPS 시각(dtLastMatchGps) 보유 시에만
	//   · nAngle < 0(NO_ANGLE) / fSpeed < 0(NO_SPEED) 가 곧 DB NULL 을 의미(파서에서 그렇게 세팅)
	// · 차이 가드: 직전 매칭점과의 시간간격이 (0, MM_CALC_MAX_GAP_SEC] 일 때만 계산(끊긴 구간 오계산 방지)
	//   · 원본 stRawLogInfo 는 불변 유지, 보정본(stAdjusted)으로 맵매칭 입력
	sRawLogInfo stAdjusted = stRawLogInfo;
	ALT_MATCH_CTX stAltCtx;
	if ((pstSession->bHasLastMatch) && 
		((stAdjusted.nAngle < 0) || (stAdjusted.fSpeed < 0.0f)))
	{
		double dfGapSec = difftime(stAdjusted.dtGPS, pstSession->dtLastMatchGps);
		if (dfGapSec > 0.0 && dfGapSec <= static_cast<double>(MM_CALC_MAX_GAP_SEC))
		{
			POINT stPrev; stPrev.dfX = pstSession->dfLastMatchX; stPrev.dfY = pstSession->dfLastMatchY;
			POINT stCur;  stCur.dfX  = stAdjusted.dfX;           stCur.dfY  = stAdjusted.dfY;
			// 직전·현재 GPS 하버사인 수평 이동거리(m) (2026-07-08 최정우 주석 추가)
			double dfMoveM = HaversineMeters(stPrev, stCur);
			stAltCtx.dfHorizMove = dfMoveM;
			stAltCtx.dfGapSec = dfGapSec;	// (2026-09-04 최정우 추가)

			if (stAdjusted.fSpeed < 0.0f)
				stAdjusted.fSpeed = static_cast<float>(dfMoveM / dfGapSec * 3.6);

			// 방위각 계산: 하한(MM_CALC_MIN_DIST) ≤ 이동거리, 상한([mapmatch] distance) ≥ 이동거리 일 때만 (2026-07-15 최정우 수정)
			//   상한 초과(예: 터널·수신두절 후 큰 점프)면 직선 이동방향이 실제 진행방향과 달라 heading 미사용
			if (stAdjusted.nAngle < 0 && dfMoveM >= MM_CALC_MIN_DIST &&
				((m_stConfig.nHeadingMaxDist <= 0) || (dfMoveM <= static_cast<double>(m_stConfig.nHeadingMaxDist))))
				// 직전·현재 좌표로 방위각(degree) 보정 (2026-07-08 최정우 주석 추가)
				stAdjusted.nAngle = m_cGISUtil.GetDirAngleDegree(stPrev, stCur);
		}
	}
	else if (pstSession->bHasLastMatch)
	{
		POINT stPrev; stPrev.dfX = pstSession->dfLastMatchX; stPrev.dfY = pstSession->dfLastMatchY;
		POINT stCur;  stCur.dfX  = stAdjusted.dfX;           stCur.dfY  = stAdjusted.dfY;
		// 고도 점수용 수평 이동거리(m) (2026-07-08 최정우 주석 추가)
		stAltCtx.dfHorizMove = HaversineMeters(stPrev, stCur);
		// (2026-09-04 최정우 추가) — 이 분기는 SPEED_KMH/HEADING 이 DB 값 그대로라 위 분기처럼
		//   경과초를 계산할 필요가 없었으나, 재구성 경로 절대속도 판정에 필요해 동일하게 계산
		double dfGapSec2 = difftime(stAdjusted.dtGPS, pstSession->dtLastMatchGps);
		if (dfGapSec2 > 0.0 && dfGapSec2 <= static_cast<double>(MM_CALC_MAX_GAP_SEC))
			stAltCtx.dfGapSec = dfGapSec2;
	}

	// 같은 링크 노이즈 보정(1m 강제전진) 억제 판별 — 원시 GPS 좌표·방향(heading)이 직전 tick과
	//   완전히 같으면(=실제로 정지) 강제전진 대신 세그먼트 매칭이 계산한 실제 좌표를 그대로 쓴다.
	//   좌표·방향 비교는 원본 stRawLogInfo(장비 보고값) 기준 — stAdjusted 는 heading 계산/보정이
	//   들어가 같은 값이어도 달라질 수 있어 판별 기준으로 부적합하다 (사용자 지시, 2026-09-02 최정우 추가)
	// [버그 수정, 2026-09-11 최정우] 비트 단위 완전동일만 잡던 위 기준은 정차 중 GPS 수신기 자체
	//   지터(수 m)를 "다른 위치로 이동"으로 오판해 노이즈 전진보정이 매 tick 누적, 실측 최대 20m
	//   드리프트가 확인됐다(재매칭 검증). 완전동일이 아니어도 저속(MM_SPEED_LOW_KMH 이하)이고 원시
	//   이동거리가 MM_NOISE_SAME_POS_TOL_M 이내면 "사실상 정지"로 같이 인정한다 — heading 은 정차
	//   중 노이즈로 요동치는 경우가 흔해 이 경우엔 비교하지 않는다(저속 자체가 "역주행일 리 없다"는
	//   더 강한 근거, MM_SPEED_LOW_KMH 재사용 근거는 위 ContinueMapMatch.cpp bLowSpeedStationary와 동일).
	//   속도 판정은 stRawLogInfo.fSpeed(DB 원본, NULL 이면 -1)가 아니라 위에서 NULL 폴백까지 끝난
	//   stAdjusted.fSpeed 를 써야 한다 — 원본만 보면 SPEED_KMH 가 DB NULL 인 tick(흔함)마다 이 예외가
	//   전혀 안 걸려 고쳐지지 않은 것처럼 보인다(최초 배포 후 재매칭 대조로 발견 — 000370_20260826143912
	//   등 여러 정지구간이 수정 후에도 그대로였음).
	POINT stPrevTickRaw, stCurTickRaw;
	stPrevTickRaw.dfX = dfPrevTickRawX;  stPrevTickRaw.dfY = dfPrevTickRawY;
	stCurTickRaw.dfX = stRawLogInfo.dfX; stCurTickRaw.dfY = stRawLogInfo.dfY;
	const bool bExactSameRawAndHeading = bHadPrevTickRaw
		&& (stRawLogInfo.dfX == dfPrevTickRawX)
		&& (stRawLogInfo.dfY == dfPrevTickRawY)
		&& (stRawLogInfo.nAngle == nPrevTickAngle);
	const bool bNearSameLowSpeed = bHadPrevTickRaw
		&& (stAdjusted.fSpeed >= 0.0f) && (stAdjusted.fSpeed <= static_cast<float>(MM_SPEED_LOW_KMH))
		&& (HaversineMeters(stPrevTickRaw, stCurTickRaw) <= MM_NOISE_SAME_POS_TOL_M);
	const bool bSameRawAndHeadingAsPrev = bExactSameRawAndHeading || bNearSameLowSpeed;

	// ── 고도 앵커 → 연속 맵매칭 컨텍스트 (Begin 미적용) ──
	//   · 전제: bHasPrevAlt (직전 매칭 성공 시 ALTITUDE_M 있었음)
	//   · dfHorizMove: 직전 매칭 XY → 현재 GPS XY 하버사인(m) — 경사 판정용
	//   · Δalt = 현재 ALTITUDE_M − nPrevAltitude (ProcessManager/ContinueMapMatch에서 사용)
	// · 예) seq10 매칭·고도100m 저장 → seq11 고도106m·같은 고가 → 차이=8 이내 보너스 −3
	if (pstSession->bHasPrevAlt)
	{
		stAltCtx.nPrevAltitude = pstSession->nPrevAltitude;
		stAltCtx.nPrevRoadType = pstSession->nPrevRoadType;
		stAltCtx.bHasPrevAlt = true;
	}

	// 직전 매칭 위치(같은 링크 내 역행 페널티용) — 고도 앵커와 무관하게 독립 전달 (2026-07-20 최정우 추가)
	if (pstSession->bHasPrevLinkPos)
	{
		stAltCtx.dfPrevLinkPos = pstSession->dfLastMatchLinkPos;
		stAltCtx.bHasPrevLinkPos = true;
		// 같은 링크 노이즈 보정(1m 전진) 시, 이번 후보 자신의 계산값이 아니라 마지막으로
		//   신뢰했던 실제 매칭 좌표를 기준점으로 삼기 위해 함께 전달 (2026-07-22 최정우 추가)
		//   단 "신뢰 가능한 매칭"이 한 번이라도 있었을 때만이다. bHasPrevLinkPos 는 매칭 성공이면
		//   무조건 서지만 dfLastMatchX/Y 는 신뢰 매칭에서만 갱신되므로, 트립 앞부분이 전부
		//   SKIP 이면 0 인 채로 넘어가 보정 좌표가 (0,0) 이 된다 (2026-08-23 최정우 수정)
		stAltCtx.bHasPrevMatchPos = pstSession->bHasLastMatch;
		if (pstSession->bHasLastMatch)
		{
			stAltCtx.dfPrevMatchX = pstSession->dfLastMatchX;
			stAltCtx.dfPrevMatchY = pstSession->dfLastMatchY;
		}
		// 원시좌표·방향 동일 여부 함께 전달 — 같은 링크 노이즈 보정 적용 여부 판단용 (2026-09-02 최정우 추가)
		stAltCtx.bSameRawAndHeadingAsPrev = bSameRawAndHeadingAsPrev;
	}

	// 연속 맵매칭 링크는 "맵매칭 성공(반경 내 MATCHED)" 시에만 세션에 반영한다.
	//   SKIP(정확도/반경 밖)·ERROR 시 직전 성공 링크를 그대로 유지 → 다음 GPS 는 마지막 성공 링크
	//   기준으로 연속 맵매칭을 이어간다. (ProcessRawLog 는 실패 시 로컬 링크를 0으로 리셋하므로
	//   세션 링크에 반영되지 않도록 로컬 복사본으로 호출) (2026-07-10 최정우 수정)
	uint64 qwLinkID = pstSession->qwLinkID;
	bool bMatched = cProcessManager.ProcessRawLog(stAdjusted, qwLinkID, pstMatchLinkInfo,
		(stAltCtx.bHasPrevAlt || stAltCtx.bHasPrevLinkPos) ? &stAltCtx : nullptr);
	if (bMatched)
	{
		double dfNewLinkPos = static_cast<double>(pstMatchLinkInfo->wLenFromLink)
			+ pstMatchLinkInfo->dfSgmtMatchLen;

		// [버그 수정, 2026-09-11 최정우 — 사용자 지시] 정지 중(bSameRawAndHeadingAsPrev)인데도
		//   SgmtMatch() 가 매 tick 새로 세그먼트 위 수선의 발을 계산해, GPS 저주파 위치표류를 그대로
		//   반영하는 현상(실측 최대 20m) — 매칭좌표(dfMatchX/Y)를 여기서 직접 덮어쓰는 첫 시도는
		//   바로 아래 nPendingIntersectLen 계산(CalcIntersectLen, 매칭 신뢰도 지표)도 오염시켜
		//   MATCH_STATUS 2건 회귀로 이어져 되돌렸다. 좌표 자체는 그대로 두고, 5개 과금함수가 거리
		//   누적(dfAccumDistM) 여부를 판단할 수 있게 신호만 실어 보낸다 — 매칭 판정(SKIP/MATCHED,
		//   intersect_len, 경계클램프 등)에는 전혀 영향 없음. bReverseSuspect 와 동일한 원리로
		//   pstSession->stPendingMatchLinkInfo 에 그대로 복사돼 CommitPendingRow 경유 과금함수까지
		//   전달된다.
		pstMatchLinkInfo->bSameRawAndHeadingAsPrev = bSameRawAndHeadingAsPrev;

		// 연속 역행 스트릭 갱신 — bReverseSuspect(위치 역행 + heading 도 역방향 일치) 연속 횟수를 센다.
		//   GPS 노이즈성 흔들림(heading은 여전히 정방향)은 스트릭에 안 잡히고 실제 역행(heading도 반대)만
		//   잡히게 함 (2026-07-21 최정우 수정 — heading 대조 결합)
		//   reverse_confirm 미만이면 노이즈로 보고 앵커(dfLastMatchLinkPos) 고정 — 다음 포인트도
		//   같은 기준점과 비교돼 판정이 안 흔들린다. reverse_confirm 이상이면 실제 이동으로 확정하고
		//   앵커를 지금 위치로 재설정해 정상 추적을 재개한다 (2026-07-21 최정우 추가 — dip 판정 대체)
		if (pstMatchLinkInfo->bReverseSuspect)
			pstSession->nReverseStreak += 1;
		else
			pstSession->nReverseStreak = 0;

		const bool bConfirmed = (pstSession->nReverseStreak >= m_stConfig.nReverseConfirm);

		// bImplausiblePath(재구성 경로 절대속도 비현실) — 이 링크 자체를 신뢰 못 하는 것이므로
		//   qwLinkID(다음 틱 Continue 탐색 앵커)도 전진시키지 않는다 — bClampLowConf·bAmbiguousReverse·
		//   bReverseSkip 등 "링크는 맞는데 위치·방향만 애매한" 경우는 여전히 무조건 전진(기존 관례,
		//   세션이 실제 위치 근처에 계속 앵커링되도록 유지하려는 의도)하지만, bImplausiblePath는
		//   "이 링크에 도달했다는 주장 자체"가 의심스러운 유일한 경우라 성격이 다르다. 전진시키지
		//   않으면 다음 신뢰 틱은 "마지막 진짜 신뢰 지점"부터 다시 그래프 탐색하게 되고, 그 경로가
		//   실제 누적 경과시간(여러 틱에 걸친 진짜 gap) 기준으로 재검증된다 — 링크 내 위치 기준점도
		//   같은 이유로 무효화(실측 000376_20260826150010 seq133→134: 132에서 134까지 6초로 재계산하면
		//   207.5km/h로 타당한데, 133이 이미 세션 링크·위치를 전진시켜놔서 134가 그 검증 자체를
		//   건너뛰고 통과했었다 — 결과는 우연히 맞았지만 검증을 안 거친 것) (2026-09-04 최정우 추가, 사용자 지시)
		//   bImplausibleDirection·bImplausibleSpeedLimit(재구성 경로 방향·구간제한속도 비현실)도
		//   같은 이유로 동일 처리 (2026-09-04 최정우 추가)
		if (pstMatchLinkInfo->bImplausiblePath || pstMatchLinkInfo->bImplausibleDirection
			|| pstMatchLinkInfo->bImplausibleSpeedLimit)
		{
			pstSession->bHasPrevLinkPos = false;
		}
		else
		{
			pstSession->qwLinkID = qwLinkID;		// 성공 시에만 링크 전진(다음 점 연속 매칭 기준)
			if (!pstMatchLinkInfo->bReverseSuspect || bConfirmed)
				pstSession->dfLastMatchLinkPos = dfNewLinkPos;
			pstSession->bHasPrevLinkPos = true;
		}
	}
	return bMatched;
}

/**
 * @brief YYYYMMDDHH24MISS 문자열 생성 (로컬 시각) (2026-08-12 최정우 추가)
 * @param[in] dtValue time_t
 * @return 14자 일시 문자열 (dtValue<=0 이면 빈 문자열)
 * @remark CRawLogFetcher::ParseDateTime() 의 역변환 — PRIM_CHARGEHAND occur_dt/trip_start_dt 저장용
*/
/**
 * @brief "YYYYMMDDHH24MISS" 14자리 문자열을 time_t 로 되돌린다 — FormatDateTime14() 의 역함수
 *   (2026-09-22 최정우 추가)
 * @param[in] strValue 14자리 날짜시각 문자열(그 외 길이는 무효)
 * @return 변환된 time_t. 형식이 어긋나면 0
 * @remark FormatDateTime14() 가 localtime_r 로 만들었으므로 여기서도 mktime(지역시각)으로 되돌려
 *   왕복이 일치하게 한다. CRawLogFetcher::ParseDateTime() 이 같은 일을 하지만 private 이라
 *   여기서 쓸 수 없어 대칭 구현을 따로 둔다.
*/
time_t CRawLogWorker::ParseDateTime14(const string& strValue)
{
	if (strValue.length() != 14)
		return 0;
	for (size_t i = 0; i < 14; ++i)
	{
		if ((strValue[i] < '0') || (strValue[i] > '9')) return 0;
	}

	struct tm stTm;
	memset(&stTm, 0, sizeof(stTm));
	stTm.tm_year = atoi(strValue.substr(0, 4).c_str()) - 1900;
	stTm.tm_mon  = atoi(strValue.substr(4, 2).c_str()) - 1;
	stTm.tm_mday = atoi(strValue.substr(6, 2).c_str());
	stTm.tm_hour = atoi(strValue.substr(8, 2).c_str());
	stTm.tm_min  = atoi(strValue.substr(10, 2).c_str());
	stTm.tm_sec  = atoi(strValue.substr(12, 2).c_str());
	stTm.tm_isdst = -1;
	return mktime(&stTm);
}

string CRawLogWorker::FormatDateTime14(time_t dtValue)
{
	if (dtValue <= 0)
		return string();

	struct tm stTm;
	localtime_r(&dtValue, &stTm);

	char szBuf[32];
	snprintf(szBuf, sizeof(szBuf), "%04d%02d%02d%02d%02d%02d",
		stTm.tm_year + 1900, stTm.tm_mon + 1, stTm.tm_mday,
		stTm.tm_hour, stTm.tm_min, stTm.tm_sec);
	return string(szBuf);
}

/**
 * @brief TRIP_ID({6자리 숫자}_{YYYYMMDDHH24MISS}) 에서 시각 부분만 추출 (2026-08-19 최정우 추가)
 * @param[in] szTripId TRIP_ID 문자열
 * @return 성공 시 첫 '_' 다음 문자열 포인터(szTripId 내부를 가리킴 — 수명은 szTripId 와 동일),
 *   '_'가 없거나 그 뒤에 아무것도 없으면 nullptr
 * @remark 기존엔 DEVICE_KEY 길이만큼 건너뛰는 방식(TRIP_ID = DEVICE_KEY_시각 가정)이었으나,
 *   TRIP_ID 포맷이 CAR_SEQ_NO(6자리)_시각 로 바뀌면서 DEVICE_KEY 와 길이가 달라져 엉뚱한
 *   위치부터 잘리는 버그가 있었음(원격 DB 실측으로 확인 — 예: trip_id=000376_20260819094414,
 *   device_key=CAR000434(9자) 인데 trip_start_dt=60819094414 로 저장됨). TRIP_ID 안의 첫
 *   '_' 위치를 직접 찾는 방식으로 교체 — 포맷이 다시 바뀌어도(접두어 길이 변경) 안전.
*/
const char* CRawLogWorker::ExtractTripStartDt(const char *szTripId)
{
	if ((szTripId == nullptr) || (szTripId[0] == '\0'))
		return nullptr;

	const char *pszUnderscore = strchr(szTripId, '_');
	if ((pszUnderscore == nullptr) || (pszUnderscore[1] == '\0'))
		return nullptr;

	return pszUnderscore + 1;
}

/**
 * @brief 개방형 구역의 M게이트를 이 진행(run) 동안 실제로 지났는지 갱신(래치 — 한 번 true 면
 *   재확인 안 함) (2026-08-25 최정우 추가)
 * @remark 단순히 "지금 위치가 게이트 지점 이후인가"만 보면 안 된다 — case C(트립이 이미 게이트를
 *   지난 뒤 시작)는 시작하자마자 그 조건을 항상 만족해버려서 첫 틱부터 "통과함"으로 오판된다
 *   (실측 000376/900202 합성테스트로 발견, 2026-08-25). 그래서 "이 run 안에서 게이트 이전
 *   상태를 실제로 거쳐왔는가"(bSeenBeforeGate)를 먼저 확인하고, 그 상태에서 게이트 위치에
 *   도달했을 때만 "진짜 통과"로 확정한다. 판정 순서:
 *   (1) 경유 링크(aqwPathLinkIDs)에 게이트 링크가 있으면 이미 완전히 지나온 것으로 확정
 *       (그 경유 링크에 도달했다는 것 자체가 그 이전엔 게이트 이전 위치였다는 뜻이라 안전)
 *   (2) 이번 확정 링크가 게이트 링크가 아닌 이 구역의 다른 링크면 — 개방형 구역은 단방향
 *       코리도라는 전제로 "게이트 이전" 상태로 기록만 해둔다(RL-Z00004 등 실측 구조상 게이트
 *       앞뒤로 링크가 나뉘어 있음)
 *   (3) 이번 확정 링크가 게이트 자신의 링크면 "링크 시작점부터 게이트까지 거리" vs "링크
 *       시작점부터 현재 매칭 위치까지 거리"를 비교(-3m 오차 허용) — 아직 못 미쳤으면 "게이트
 *       이전" 기록만, 이미 도달/통과했으면 bSeenBeforeGate 가 true 일 때만 "진짜 통과"로 확정
 *   HaversineMeters() 가 private static 이라 CollectGateCandidates() 처럼 파일지역 static 함수로
 *   못 두고 멤버 함수로 둠
*/
void CRawLogWorker::UpdateOpenGateCrossed(const MATCH_LINK_INFO& stMatchLinkInfo, ZONE_RUN_SESSION *pstRun)
{
	if (pstRun->bGateCrossed) return;
	if (m_stConfig.pcChargeDataLoader == nullptr) return;

	PGATE_INFO pstGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(pstRun->szRoadID, 'M');
	if (pstGate == nullptr) return;			// 이 구역엔 등록된 M게이트가 없음(방어적)

	// aqwPathLinkIDs 의 마지막 원소는 이번 확정 링크(stMatchLinkInfo.qwLinkID)와 항상 같다
	//   (CollectGateCandidatesOnIntermediateLinks() 주석 참고) — 그래서 "이미 완전히 지나온
	//   경유 링크"만 보려면 마지막 하나는 반드시 제외해야 한다. 안 그러면 이번 확정 링크가
	//   게이트 링크 자신인 매 틱마다 이 루프가 자기 자신과 매치돼 무조건 true 가 되어 버려서,
	//   case C(이미 게이트 지난 뒤 출발) 의 두 번째 틱부터 곧바로 "통과함"으로 오판된다(실측
	//   000376/900202 합성테스트로 발견, 2026-08-25)
	for (uint8 i = 0; (i + 1) < stMatchLinkInfo.nPathLinkCount; ++i)
	{
		if (stMatchLinkInfo.aqwPathLinkIDs[i] == pstGate->qwLinkID)
		{ pstRun->bGateCrossed = true; return; }
	}

	if (stMatchLinkInfo.qwLinkID != pstGate->qwLinkID)
	{
		pstRun->bSeenBeforeGate = true;			// 게이트 아닌 구역 내 다른(=상류) 링크 위
		return;
	}

	POINT stLinkStart, stGatePos;
	stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
	stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
	stGatePos.dfX = pstGate->dfLon;
	stGatePos.dfY = pstGate->dfLat;
	// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
	//   (2026-09-07 최정우 수정, 사용자 지적)
	double dfGatePosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stGatePos.dfX, stGatePos.dfY);
	if (dfGatePosOnLink < 0.0)
		dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);		// 형상 없음 — 종전 직선거리
	double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

	if (dfCurPosOnLink < (dfGatePosOnLink - 3.0))
	{
		pstRun->bSeenBeforeGate = true;			// 아직 게이트 전 — 나중에 지나면 확정할 근거
		return;
	}

	if (pstRun->bSeenBeforeGate)
		pstRun->bGateCrossed = true;				// 게이트 전이었다가 지금 지남 확인 — 진짜 통과
	// bSeenBeforeGate 가 false 면(=이 run 안에서 게이트 전 상태를 본 적이 없음) 지금 위치가
	//   게이트 지난 곳이어도 확정 안 함 — 트립이 이미 게이트를 지난 뒤 시작했을 가능성(case C)
}

/**
 * @brief 개방형(ROAD_KIND=1) 구역 진입/이탈 판정 (2026-08-25 최정우 재작성)
 * @param[in] nThreadId 워커 스레드 ID (로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 신뢰 가능한 맵매칭 결과(호출측이 bMatched && !bUntrustedMatch 확인 후 호출)
 * @param[in,out] pstSession 배치 임시 세션 — vtOpenRuns/nChargeSeq 갱신
 * @param[out] pvtChargeInserts 정상 이탈·트립종료 시 1행씩 적재 (일반도로 NODE_STEP과 동일 구조)
 * @return void
 * @remark 원래는 게이트 통과 순간의 점 이벤트였으나, 주행거리(dist_m)·주행시간(stay_seconds)을
 *   함께 적재하기 위해 일반도로와 동일한 "구역 진입~이탈" 구조로 전환(사용자 지시). 게이트는
 *   여전히 있고 UpdateOpenGateCrossed()가 이 진행(run) 동안 실제로 지났는지를 매 틱 갱신 —
 *   BuildOpenZoneRow()가 이 값과 bStartedByTrip 으로 dist_m 산출 방식·charge_yn/status 를 정한다.
 *   트립종료(TRIP_EVENT=2)는 일반도로와 동일하게 디바운스 없이 즉시 마감(다음 GPS가 안 올 수 있음)
*/
void CRawLogWorker::ProcessOpenGateCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경유 링크 포함 전체 경로에서 개방형 구역을 "전부" 수집(NODE_STEP과 동일 패턴, 2026-08-25 최정우 추가)
	vector<PZONE_INFO> vtZones;
	{
		vector<PZONE_INFO> vtOne;
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
			m_stConfig.pcChargeDataLoader->GetOpenZonesByLinkId(stMatchLinkInfo.qwLinkID, &vtOne);
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
				m_stConfig.pcChargeDataLoader->GetOpenZonesByLinkId(
					stMatchLinkInfo.aqwPathLinkIDs[i], &vtOne);
		}
		for (size_t i = 0; i < vtOne.size(); ++i)			// road_id 기준 중복 제거
		{
			bool bDup = false;
			for (size_t e = 0; e < vtZones.size(); ++e)
			{
				if (strcmp(vtZones[e]->szRoadID, vtOne[i]->szRoadID) == 0) { bDup = true; break; }
			}
			if (!bDup) vtZones.push_back(vtOne[i]);
		}
	}

	const bool bTripEnding = bTrustedTripEnd;

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtOpenRuns.size(); )
	{
		ZONE_RUN_SESSION& stRun = pstSession->vtOpenRuns[si];

		UpdateOpenGateCrossed(stMatchLinkInfo, &stRun);

		bool bSameZone = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0) { bSameZone = true; break; }
		}

		// 누적 이동거리 — bStartedByTrip(부분거리) run 에만 의미 있지만, 정상진입 run 도 계산 자체는
		//   해둔다(사용 안 하고 버릴 뿐, 분기 단순화)
		if (bSameZone)
		{
			stRun.nExitTicks = 0;
			// [버그 수정, 2026-09-11 최정우 — 1차 수정 자체 발견+재수정] 정지 중(bSameRawAndHeadingAsPrev)
			//   GPS 저주파 위치표류가 매 tick 세그먼트 재투영에 반영돼 dist_m 이 과다 계상되는 걸
			//   막는다(NODE_STEP/EXEMPT 동일 수정과 같은 근거). ProcessClosedRoadCharge()/
			//   ProcessSpeedZoneCharge() 와 동일하게, "거리누적·누적기준점"(dfAccumDistM/dfLastX/Y)만
			//   가드 안에 넣고, "구역 안에서 마지막으로 확인된 링크/시각"(qwLastLinkID/dtLastInZoneTime/
			//   dwLastInZoneGpsSeq — stay_seconds·경계통과 시각보간·end_gps_seq 의 근거)은 정지 중에도
			//   매 tick 갱신돼야 한다 — 최초 배선 시 이 셋까지 같은 가드에 넣어버려서, 정지 상태로
			//   구역이 끝나면 stay_seconds 가 짧게 계산되고 end_gps_seq 도 부정확해지는 회귀가 있었다
			//   (전체 재검증으로 자체 발견).
			if (!stMatchLinkInfo.bSameRawAndHeadingAsPrev)
			{
				POINT stPrev, stCur;
				stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
				stCur.dfX = stMatchLinkInfo.dfMatchX;  stCur.dfY = stMatchLinkInfo.dfMatchY;
				stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
				stRun.dfLastX = stMatchLinkInfo.dfMatchX;
				stRun.dfLastY = stMatchLinkInfo.dfMatchY;
			}
			stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
			stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
			stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;

			// 구역 안 실측 tick 수·최대 순간속도 — BuildOpenZoneRow() 의 체류시간 물리 한계
			//   보정용(필드 주석 참고). 정지·역행 여부와 무관하게 "구역 안에서 관측된 tick"
			//   자체를 세므로 위 가드 밖에 둔다 (2026-09-21 최정우 추가)
			stRun.nInZoneTicks += 1;
			if (stRawLogInfo.fSpeed > stRun.fMaxSpeed)
				stRun.fMaxSpeed = stRawLogInfo.fSpeed;
		}
		else if (stRun.nExitTicks == 0)
		{
			// 이탈 디바운스 스트릭의 첫 "밖" tick — ProcessNodeStepCharge() 동일 근거(dfFirstOutX/Y
			//   필드 주석 참고, 2026-08-25 최정우 추가)
			stRun.dfFirstOutX = stMatchLinkInfo.dfMatchX;
			stRun.dfFirstOutY = stMatchLinkInfo.dfMatchY;
			stRun.dtFirstOut = stRawLogInfo.dtGPS;
			stRun.qwFirstOutLinkID = stMatchLinkInfo.qwLinkID;

			// 개방식 진출 지점 이월(bHasGateExitCarry) — 폐쇄식·구간단속·면제도 진출 확정 시 이 이월을
			//   하는데 개방식만 빠져 있었다(버그 수정, 2026-09-14 최정우 — 실측
			//   000370_20260911141637 seq89~91: 개방식 종료좌표와 바로 다음 일반도로 시작좌표 사이
			//   실측 27m 가 이월이 없어 어느 기록에도 안 잡힘). 개방식은 진출 판정 기준이 게이트
			//   (측정용 M게이트일 뿐 경계 아님)가 아니므로, 구역 안 마지막 링크의 종료 노드를 진출
			//   경계로 삼는다. **여기(첫 밖 tick)에서 바로 이월해야 한다** — 개방식은 node_exitcnt
			//   디바운스를 거쳐야 진출이 최종 확정되는데(아래), NODE_STEP은 그 확정을 기다리지 않고
			//   같은 tick에 곧바로 새 run을 연다. 이월을 디바운스 확정 시점(기존 위치)에 해뒀더니
			//   NODE_STEP이 이미 그 전에 원시좌표로 진입점을 확정해버려 이월이 항상 한 박자 늦었다
			//   (재검증으로 확인 — 폐쇄식/구간단속은 게이트 위치 기반이라 확정이 즉시라 이 문제가 없음).
			if ((stRun.qwLastLinkID != 0) && (m_stConfig.pcDataLoader != nullptr))
			{
				PLINK_INFO pstExitLink = m_stConfig.pcDataLoader->GetLinkInfo(stRun.qwLastLinkID);
				if (pstExitLink != nullptr)
				{
					POINT stFrom, stNode;
					stFrom.dfX = stRun.dfLastX;  stFrom.dfY = stRun.dfLastY;
					stNode.dfX = static_cast<double>(pstExitLink->dwEdNodeX) / 360000.0;
					stNode.dfY = static_cast<double>(pstExitLink->dwEdNodeY) / 360000.0;

					pstSession->bHasGateExitCarry = true;
					pstSession->dfGateExitX = stNode.dfX;
					pstSession->dfGateExitY = stNode.dfY;
					pstSession->dtGateExit = InterpolateGateCrossingTime(
						stFrom.dfX, stFrom.dfY, stRun.dtLastInZoneTime,
						stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut,
						stNode.dfX, stNode.dfY);
					pstSession->dwGateExitGpsSeq = stRun.dwLastInZoneGpsSeq;
					pstSession->qwGateExitLinkID = stRun.qwLastLinkID;
					pstSession->bGateExitAtTick =
						(HaversineMeters(stNode, stFrom) <= 2.0);

					LOGFMTI("[#%02d] open gate exit carry armed!device=[%s] trip_id=[%s] seq=[%u] "
						"road=[%s] last_link=[%llu] node=[%.6f,%.6f]",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
						stRun.szRoadID, static_cast<unsigned long long>(stRun.qwLastLinkID),
						stNode.dfY, stNode.dfX);
				}
			}
		}

		if (bSameZone && !bTripEnding) { ++si; continue; }		// 계속 진행 중

		// node_exitcnt 재사용 — 순간 오매칭 1틱으로 세션이 쪼개지는 것 방지(NODE_STEP과 동일 원리).
		//   트립종료는 디바운스 없이 즉시 마감(다음 틱이 안 옴)
		if (!bTripEnding)
		{
			stRun.nExitTicks += 1;
			if (stRun.nExitTicks < m_stConfig.nNodeExitCnt) { ++si; continue; }
		}

		// 이탈 지점 보정 — 구역 안에서 마지막으로 달린 링크의 종료 노드까지 거리를 채운다(NODE_STEP과
		//   동일 원리). 거리 보정 자체는 bStartedByTrip(부분거리) run 에만 의미 있음 — 정상진입 run 은
		//   dist_m 이 구역 전체길이 고정값이라 무관. 단 "시각" 보간(dtOpenExitTime)은 두 run 유형
		//   모두에 적용한다 — 정상진입 run 도 stay_seconds/speed_kmh 는 dtEnd 기준으로 계산되므로
		//   똑같이 부정확할 수 있음(사용자 지시, 2026-08-25 최정우 추가 — CLOSED/SPEED 의
		//   InterpolateGateCrossingTime() 동일 로직을 "구역 경계 노드"를 목표점 삼아 재사용)
		time_t dtOpenExitTime = (stRun.dtLastInZoneTime != 0) ? stRun.dtLastInZoneTime : stRawLogInfo.dtGPS;
		if (!bSameZone && (stRun.qwLastLinkID != 0) && (m_stConfig.pcDataLoader != nullptr))
		{
			PLINK_INFO pstLastLink = m_stConfig.pcDataLoader->GetLinkInfo(stRun.qwLastLinkID);
			if (pstLastLink != nullptr)
			{
				POINT stFrom, stNode;
				stFrom.dfX = stRun.dfLastX;  stFrom.dfY = stRun.dfLastY;
				stNode.dfX = static_cast<double>(pstLastLink->dwEdNodeX) / 360000.0;
				stNode.dfY = static_cast<double>(pstLastLink->dwEdNodeY) / 360000.0;

				if (stRun.bStartedByTrip)
				{
					double dfTail = HaversineMeters(stFrom, stNode);
					if ((dfTail > 0.0) && (dfTail <= pstLastLink->dfLen + 1.0))
					{
						stRun.dfAccumDistM += dfTail;
						stRun.dfLastX = stNode.dfX;
						stRun.dfLastY = stNode.dfY;
					}
				}

				// "밖" 기준점은 세션 범용 직전tick이 아니라 dfFirstOutX/Y 를 써야 한다 —
				//   ProcessNodeStepCharge() 동일 근거(node_exitcnt 디바운스 구간 오염 방지,
				//   2026-08-25 최정우 추가)
				if (stRun.dtFirstOut != 0)
				{
					dtOpenExitTime = InterpolateGateCrossingTime(
						stFrom.dfX, stFrom.dfY, stRun.dtLastInZoneTime,
						stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut,
						stNode.dfX, stNode.dfY);
				}
			}
		}

		CHARGE_INSERT_ROW stRow;
		BuildOpenZoneRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, dtOpenExitTime, stRun.dwLastInZoneGpsSeq, &stRow);

		// [보완, 2026-09-16 최정우 — 사용자 지시] **같은 구역의 연속 구간이면 한 행으로 합친다.**
		//   게이트 진출 후 경로복구(EmitForeignSpan)가 만든 행이 이 정규 마감보다 **먼저** 벡터에
		//   들어간다(실측 로그 순서: gap-recovered → open zone exit). 구역 자신의 마지막 링크가
		//   짧아(RL-Z00004 의 2040423602, 8.8m) tick 이 하나도 안 찍히면 그 링크가 복구 대상이 되어,
		//   한 번의 주행이 142m(Y/0) + 9m(N/3, 코드 12) 두 행으로 쪼개졌다 — 실측 7건 전부 이 패턴.
		//   뒤 행은 그 링크 위에 게이트가 없어 미확정이라 심사 큐까지 만든다.
		//   같은 트립·같은 구역의 복구 행만 흡수하며(다른 구역이면 종전대로 둘 다 남긴다),
		//   **구간 범위(START/END_GPS_SEQ)만** 합친다 — 거리는 더하지 않는다(아래 2026-09-16 수정 참고).
		// [2026-09-22 최정우] 이 인라인 흡수는 **그 배치 벡터 안에 두 행이 같이 있을 때만** 동작한다.
		//   배치가 갈리면 9m 조각이 그대로 남아(실측 000994_20250903152350 trip_seq=14) 심사 큐까지
		//   만들었다. 그래서 같은 판정을 MergeGapRecoveredOpenRows() 로 떼어내 **큐 방출 직전에도**
		//   돌린다. 여기 인라인 판정은 배치 안에서 조기 해소되는 흔한 경우를 그대로 처리한다.
		for (size_t pi = pvtChargeInserts->size(); pi > 0; --pi)
		{
			const CHARGE_INSERT_ROW& stPrev = (*pvtChargeInserts)[pi - 1];
			if (stPrev.strTripId != stRawLogInfo.szTripID) continue;
			if (stPrev.strChargeType != "1") continue;			// 개방형 행만 대상
			if (stPrev.strFromId != stRun.szRoadID) break;		// 다른 구역 — 합치지 않음
			if (stPrev.strNonChargeReason != to_string(static_cast<int>(NCR_OPEN_GATE_NOT_ON_PATH)))
				break;											// 복구 행이 아니면 대상 아님

			// [버그 수정, 2026-09-16 최정우 — 사용자 지적] **거리를 더하지 않는다.** 오전에 이 병합을
			//   넣을 때 두 행이 서로 다른 구간이라 보고 합산했는데, 개방형 DIST_M 은 실측 누적이 아니라
			//   **구역 전체 길이 고정값**이다(BuildOpenZoneRow 주석 참고). 즉 복구된 링크는 이미 그
			//   값 안에 들어 있어, 더하면 구역보다 긴 거리가 청구된다 — 실측 RL-Z00004 는 기준
			//   142.1m 인데 151m 로 10건, RL-Z00010/11 은 2618m 기준에 2621m 로 5건. 게다가 복구 행은
			//   N/3(과금 제외, 코드 12)인데 합치면서 그 거리가 Y/0(과금 대상)으로 옮겨갔다.
			//   병합 자체는 유효하다(한 번의 주행을 한 행으로) — 합칠 것은 거리가 아니라 **구간 범위**다.
			if (!stPrev.strStartGpsSeq.empty() && !stRow.strStartGpsSeq.empty()
				&& (atol(stPrev.strStartGpsSeq.c_str()) < atol(stRow.strStartGpsSeq.c_str())))
				stRow.strStartGpsSeq = stPrev.strStartGpsSeq;
			if (!stPrev.strEndGpsSeq.empty() && !stRow.strEndGpsSeq.empty()
				&& (atol(stPrev.strEndGpsSeq.c_str()) > atol(stRow.strEndGpsSeq.c_str())))
				stRow.strEndGpsSeq = stPrev.strEndGpsSeq;

			LOGFMTI("[#%02d] gap-recovered open zone merged into exit row!device=[%s] trip_id=[%s] "
				"road=[%s] recovered_dist=[%s]m exit_dist=[%s]m dist_not_summed=[1] "
				"gps_seq=[%s~%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
				stPrev.strDistM.c_str(), stRow.strDistM.c_str(),
				stRow.strStartGpsSeq.c_str(), stRow.strEndGpsSeq.c_str());

			// [2026-09-22 최정우 제거] 종전에는 여기서 복구 행의 trip_seq 를 승계하고
			//   nChargeSeq 를 되돌려 "비는 번호"를 막았다. 워터마크 큐 도입으로 **죽은 코드**가 됐다 —
			//   DB 에 실리는 TRIP_SEQ 는 ReleaseChargeQueue() 가 방출 시점에 새로 매기므로 여기서
			//   무엇을 넣든 덮어써지고, 실제로 INSERT 되는 행에만 번호를 주므로 결번도 안 생긴다.
			pvtChargeInserts->erase(pvtChargeInserts->begin() + (pi - 1));
			break;
		}

		pvtChargeInserts->push_back(stRow);

		if (stRow.strNonChargeReason.empty())
		{
			LOGFMTI("[#%02d] open zone exit recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
				"started_by_trip=[%d] gate_crossed=[%d] dist_m=[%s] avg_speed=[%s] trip_ending=[%d] "
				"non_charge_reason=[%d:%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
				stRun.szRoadID, static_cast<int>(stRun.bStartedByTrip), static_cast<int>(stRun.bGateCrossed),
				stRow.strDistM.c_str(), stRow.strSpeedKmh.c_str(), static_cast<int>(bTripEnding),
				NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
		}
		else
		{
			// [버그 수정, 2026-09-11 최정우] non_charge_reason 이 채워진(=N/3) 케이스는 WARN 으로
			//   승격 + 코드·메시지를 남겨 trip_id/charge_seq 로 원인 추적 가능하게 한다
			const int nNonChargeReason = atoi(stRow.strNonChargeReason.c_str());
			LOGFMTW("[#%02d] open zone exit recorded(AUDIT)!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
				"started_by_trip=[%d] gate_crossed=[%d] dist_m=[%s] avg_speed=[%s] trip_ending=[%d] "
				"non_charge_reason=[%d:%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
				stRun.szRoadID, static_cast<int>(stRun.bStartedByTrip), static_cast<int>(stRun.bGateCrossed),
				stRow.strDistM.c_str(), stRow.strSpeedKmh.c_str(), static_cast<int>(bTripEnding),
				nNonChargeReason,
				m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nNonChargeReason));
		}

		pstSession->nChargeSeq += 1;
		pstSession->vtOpenRuns.erase(pstSession->vtOpenRuns.begin() + si);
	}

	// ── ② 새로 진입한 구역 세션 개시 ─────────────────────────────────────────
	if (bTripEnding)
		return;											// 종료 틱에서는 새로 열지 않는다

	// 트립 자체가 이 구역 도로 위에서 시작했는지 — TRIP_EVENT=START 행의 매칭 링크가 이미 구역
	//   안이면 게이트를 지나온 적 없는 상태로 진행이 시작된 것(사용자 지시, 2026-08-25 추가)
	const bool bTripStarting = (stRawLogInfo.nTripEvent == TRIP_EVENT_START);

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtOpenRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtOpenRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;

		ZONE_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';
		// 진입 경계(구역에 들어온 링크의 시작 노드) 통과 시각 보간 — 위 dtOpenExitTime 동일 근거
		//   참고. 트립시작(bTripStarting) run 은 겨냥할 "직전(구역 밖) tick" 자체가 없어 대상 아님
		//   (2026-08-25 최정우 추가, 사용자 지시)
		if (!bTripStarting && pstSession->bHasLastMatch)
		{
			stRun.dtEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				stMatchLinkInfo.dfStNodeX, stMatchLinkInfo.dfStNodeY);
		}
		else
		{
			stRun.dtEntryTime = stRawLogInfo.dtGPS;
		}
		stRun.dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
		stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
		stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
		stRun.dfAccumDistM = 0.0;
		stRun.dfLastX = stMatchLinkInfo.dfMatchX;
		stRun.dfLastY = stMatchLinkInfo.dfMatchY;
		stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
		stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
		stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
		stRun.bStartedByTrip = bTripStarting;

		if (bTripStarting)
		{
			// 트립시작 run 은 "이미 지나온 뒤 시작"(case C)과 "지금 막 그 지점에서 시작"(case B)을
			//   구분해야 한다 — 이전 틱이 없어 UpdateOpenGateCrossed()의 "reached-or-passed"(한쪽
			//   방향) 판정을 그대로 쓰면 게이트보다 훨씬 뒤에서 시작해도 무조건 "지났다"로 오판된다.
			//   대칭 허용오차(±3m)로 "지금 막 그 지점"만 인정 — 그보다 뚜렷이 앞서면(이미 지남)
			//   미인정, 뚜렷이 뒤면(아직 못 지남) 당연히 미인정(사용자 지시, 2026-08-25 추가)
			PGATE_INFO pstGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(stRun.szRoadID, 'M');
			if ((pstGate != nullptr) && (pstGate->qwLinkID == stMatchLinkInfo.qwLinkID))
			{
				POINT stLinkStart, stGatePos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stGatePos.dfX = pstGate->dfLon;
				stGatePos.dfY = pstGate->dfLat;
				// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
				//   (2026-09-07 최정우 수정, 사용자 지적)
				double dfGatePosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stGatePos.dfX, stGatePos.dfY);
				if (dfGatePosOnLink < 0.0)
					dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);		// 형상 없음 — 종전 직선거리
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;
				if (fabs(dfCurPosOnLink - dfGatePosOnLink) <= 3.0)
					stRun.bGateCrossed = true;
			}
			// 게이트가 다른 링크면(=이 링크엔 아예 없음) 이 시작 틱만으론 판단 불가 — false 유지,
			//   뒤이은 틱에서 UpdateOpenGateCrossed()가 정상 갱신(그 시점부턴 "이 run 안에서 계속
			//   있었다"는 전제가 성립하므로 한쪽 방향 판정이 맞다)
		}
		else
		{
			// 정상진입(case A) run — 직전 틱까지 구역 밖에 있었다는 게 보장되므로 한쪽 방향
			//   판정(경유 링크 포함)을 그대로 써도 안전
			UpdateOpenGateCrossed(stMatchLinkInfo, &stRun);
		}
		// [2026-09-22 최정우 추가 — 사용자 지시] **역방향 진입 차단.**
		//   왕복분리 도로에서 반대편 차선 링크에 붙는 오매칭이 나면 주행하지 않은 구간에 요금이
		//   부과된다(실측 3건·426m). 차량 heading 이 구역 진행 방향과 150° 이상 어긋나면 진입으로
		//   인정하지 않는다 — 그 구간은 일반도로로 청구되므로 거리가 빠지지는 않는다.
		//   근거·임계값은 IsZoneDirectionOpposite() 주석 참고(실측 70건 전수 분석).
		if (IsZoneDirectionOpposite(string(stRun.szRoadID), stRawLogInfo))
		{
			LOGFMTW("[#%02d] zone entry rejected(opposite direction)!device=[%s] trip_id=[%s] "
				"seq=[%u] road=[%s] heading=[%d] speed=[%.0f]km/h",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				stRun.szRoadID, static_cast<int>(stRawLogInfo.nAngle), stRawLogInfo.fSpeed);
			continue;
		}

		// 진입 tick 도 구역 안 실측 tick 이다 (2026-09-21 최정우 추가)
		stRun.nInZoneTicks = 1;
		stRun.fMaxSpeed = (stRawLogInfo.fSpeed > 0.0f) ? stRawLogInfo.fSpeed : 0.0f;
		pstSession->vtOpenRuns.push_back(stRun);

		LOGFMTI("[#%02d] open zone entry!device=[%s] trip_id=[%s] road=[%s] started_by_trip=[%d] open=[%zu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
			static_cast<int>(bTripStarting), pstSession->vtOpenRuns.size());
	}
}

/**
 * @brief link_id+gate_div 로 게이트 후보 수집 — 'B'(양방향 겸용) 게이트도 함께 포함 (2026-08-13 최정우 추가)
 * @remark base_tollgate.gate_div CHECK 제약이 M/I/O/B 를 전부 허용하는데, 코드에선 I/O만 취급하고
 *   있었음 — 'B' 는 "이 한 게이트가 자기 road_id 기준으로 입구·출구 양쪽 다 될 수 있다"는 의미로
 *   해석해 cGateDiv 조회에 항상 같이 포함시킴. 폐쇄형/구간단속 둘 다 사용
 * @param[in] qwLinkID 매칭 링크 ID
 * @param[in] cGateDiv 찾는 방향('I' 또는 'O')
 * @param[out] pvtOut 일치하는 게이트 포인터 목록(cGateDiv 일치 + 'B' 전부)
*/
static void CollectGateCandidates(CChargeDataLoader *pcLoader, const uint64 qwLinkID,
		const char cGateDiv, vector<PGATE_INFO> *pvtOut)
{
	pvtOut->clear();
	vector<PGATE_INFO> vtExact, vtBoth;
	pcLoader->GetGatesByLinkId(qwLinkID, cGateDiv, &vtExact);
	pcLoader->GetGatesByLinkId(qwLinkID, 'B', &vtBoth);
	pvtOut->insert(pvtOut->end(), vtExact.begin(), vtExact.end());
	pvtOut->insert(pvtOut->end(), vtBoth.begin(), vtBoth.end());
}

/**
 * @brief 경유(이미 완전히 통과한) 링크에서만 게이트 후보 수집 — stMatchLinkInfo.aqwPathLinkIDs 중
 *   마지막(=이번 확정 링크, stMatchLinkInfo.qwLinkID 와 동일) 하나를 제외한 앞쪽 링크들만 순회
 *   (2026-08-20 최정우 추가)
 * @remark 진출(exit) 판정은 원래 "이번 확정 링크 안에서 게이트 지점을 실제로 지났는지" 링크 내
 *   위치(wLenFromLink+dfSgmtMatchLen) 비교가 필요했는데, 경유 링크는 정의상 이미 링크 전체를
 *   통과 완료한 구간이라 그 위의 게이트는 위치 비교 없이 무조건 "통과"로 확정할 수 있음 — GPS
 *   수신 주기가 늘어나 짧은 게이트 링크가 두 GPS 포인트 사이에 통째로 끼어버리는 경우 보완
 * @param[in] nPathCount stMatchLinkInfo.nPathLinkCount(0 또는 1이면 경유 링크 없음 — 결과 항상 빈 목록)
*/
static void CollectGateCandidatesOnIntermediateLinks(CChargeDataLoader *pcLoader,
		const uint64 *paqwPathLinkIDs, const uint8 nPathCount, const char cGateDiv,
		vector<PGATE_INFO> *pvtOut)
{
	pvtOut->clear();
	if (nPathCount < 2)
		return;
	for (uint8 i = 0; i < static_cast<uint8>(nPathCount - 1); ++i)
	{
		vector<PGATE_INFO> vtOne;
		CollectGateCandidates(pcLoader, paqwPathLinkIDs[i], cGateDiv, &vtOne);
		pvtOut->insert(pvtOut->end(), vtOne.begin(), vtOne.end());
	}
}

/**
 * @brief 이번 확정 링크(경유 링크 포함)가 그 구역의 link_ids 안에 있는지 — 게이트를 못 찾은 채
 *   구역을 "확정 이탈"했는지 판정하는 보조 수단 (2026-08-25 최정우 추가)
 * @remark CLOSED_ROAD/SPEED 전용 — 진행 중인 구역의 road_id 를 이미 알고 있는 상태에서 "그 구역
 *   안에 계속 있는가"만 확인하면 되므로, NODE_STEP/EXEMPT/OPEN처럼 link_id→구역 전체 역인덱스를
 *   따로 만들 필요 없이 GetZoneByRoadId() 로 얻은 그 zone 하나의 vtLinkIds 만 선형 탐색한다.
*/
static bool IsMatchedLinkInZone(PZONE_INFO pstZone, const MATCH_LINK_INFO& stMatchLinkInfo)
{
	if ((pstZone == nullptr) || pstZone->vtLinkIds.empty())
		return false;

	for (size_t z = 0; z < pstZone->vtLinkIds.size(); ++z)
	{
		if (pstZone->vtLinkIds[z] == stMatchLinkInfo.qwLinkID)
			return true;
		for (uint8 p = 0; p < stMatchLinkInfo.nPathLinkCount; ++p)
		{
			if (pstZone->vtLinkIds[z] == stMatchLinkInfo.aqwPathLinkIDs[p])
				return true;
		}
	}
	return false;
}

/**
 * @brief 폐쇄식(2)·구간단속(3) 출구 게이트를 **누락 링크 복구 경로**에서 찾는다
 *   (2026-10-04 최정우 추가 — 사용자 지시, 권장안 "대비 차원의 보완")
 * @param[in] qwLastZoneLinkID 구역 안에서 마지막으로 확인된 링크(qwClosedLastZoneLinkID / qwSpeedLastZoneLinkID)
 * @param[in] pszZoneRoadId 진행 중인 구역 road_id — 이 구역의 출구(O, B 겸용 포함) 게이트만 인정
 * @return 찾은 출구 게이트, 없으면 nullptr
 * @remark 출구 게이트는 ① 맵매칭이 복구한 경유 링크(aqwPathLinkIDs) ② 현재 링크(진행거리) ③ 직전 링크
 *   순으로 찾는다. 셋 다 실패하면 "출구 미확인"(구역 확정 이탈 시 N/3 23/33)으로 넘어가는데, ①은
 *   맵매칭이 만든 경로에만 기대므로 그 경로가 버려졌거나(재구성 경로가 이동거리의 3배 초과 등 그럴듯함
 *   검증 탈락) SKIP tick 을 건너뛴 경우에는 출구 게이트 링크가 통째로 빠진다. 일반도로·주정차가 쓰는
 *   것과 같은 FindLinkPathBounded(6홉)로 **마지막 구역 링크 -> 현재 링크** 사이를 직접 복구해, 그 중간
 *   링크(양 끝 제외 — 끝 링크들은 ②③에서 이미 봤다)에 이 구역 출구 게이트가 있으면 "경유 링크 통과"로
 *   확정한다. 현재 링크가 아직 구역 링크면(구역 안 주행 중) 시도하지 않는다.
 *   발생 건수: 2026-10-04 기준 실주행 21트립·시나리오 13트립 모두 "출구 미확인" 0건 — 결과 무변화가
 *   정상이며, 맵매칭 경로가 빠지는 경우에 대비한 보완이다.
 * 되돌리는 법: 두 호출부(ProcessClosedRoadCharge / ProcessSpeedZoneCharge 의 ③ 직후)를 지운다.
*/
PGATE_INFO CRawLogWorker::FindExitGateOnRecoveredPath(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, uint64 qwLastZoneLinkID, const char *pszZoneRoadId)
{
	if ((qwLastZoneLinkID == 0) || (pszZoneRoadId == nullptr) || (pszZoneRoadId[0] == '\0')
		|| (qwLastZoneLinkID == stMatchLinkInfo.qwLinkID) || (m_stConfig.pcChargeDataLoader == nullptr))
		return nullptr;
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pszZoneRoadId);
	if ((pstZone == nullptr) || IsMatchedLinkInZone(pstZone, stMatchLinkInfo))
		return nullptr;											// 아직 구역 안 — 출구를 지났다고 볼 근거 없음
	static const int MM_EXIT_GATE_RECOVER_MAX_HOPS = 6;			// 일반도로·주정차 누락 링크 복구와 같은 상한
	vector<uint64> vtPath;
	if (!FindLinkPathBounded(qwLastZoneLinkID, stMatchLinkInfo.qwLinkID, MM_EXIT_GATE_RECOVER_MAX_HOPS, &vtPath)
		|| (vtPath.size() < 3))
		return nullptr;
	for (size_t p = 1; (p + 1) < vtPath.size(); ++p)
	{
		vector<PGATE_INFO> vtGates;
		CollectGateCandidates(m_stConfig.pcChargeDataLoader, vtPath[p], 'O', &vtGates);
		for (size_t g = 0; g < vtGates.size(); ++g)
		{
			if (strcmp(vtGates[g]->szRoadID, pszZoneRoadId) != 0) continue;
			LOGFMTI("[#%02d] exit gate found on recovered path!device=[%s] trip_id=[%s] seq=[%u] road=[%s] "
				"gate=[%s] gate_link=[%llu] from=[%llu] to=[%llu] hops=[%zu]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				pszZoneRoadId, vtGates[g]->szTollgateID, static_cast<unsigned long long>(vtPath[p]),
				static_cast<unsigned long long>(qwLastZoneLinkID),
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID), vtPath.size());
			return vtGates[g];
		}
	}
	return nullptr;
}

/**
 * @brief 폐쇄형(CLOSED_ROAD) 입/출구 게이트 판정 (2026-08-12 최정우 추가, 2026-08-13 재작성)
 * @param[in] nThreadId 워커 스레드 ID (로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 신뢰 가능한 맵매칭 결과(호출측이 bMatched && !bUntrustedMatch 확인 후 호출)
 * @param[in,out] pstSession 배치 임시 세션 — 진입 상태(bInClosedRoad 등) 갱신
 * @param[out] pvtChargeInserts 출구 통과 시 1행 적재 (bulk INSERT 는 run() 이 배치 종료 시 일괄 실행)
 * @return void
 * @remark 2026-08-13 재작성 — 사용자 질문으로 발견된 3가지 한계 대응:
 *   (1) 한 링크에 같은 방향(I 또는 O) 게이트가 2개 이상 있는 경우 — 기존 GetGateByLinkId(첫 매치만
 *       반환)를 GetGatesByLinkId(전부 수집) 기반 CollectGateCandidates() 로 교체, 활성 세션의
 *       road_id 와 실제로 일치하는 게이트를 후보 중에서 직접 찾음(개방형 멀티게이트 수정과 동일 원리)
 *   (2) 링크는 다르지만 출구→도로→입구 패턴이 연속으로 여러 번 존재 — 원래도 지원됨(세션이 매
 *       구간마다 완전히 리셋되므로), 이번 재작성으로도 유지
 *   (3) 한 게이트가 출구+입구를 겸하는 경우(gate_div='B', base_tollgate CHECK 제약엔 이미 있었지만
 *       코드가 안 씀) — CollectGateCandidates() 가 'B' 게이트를 I/O 조회 양쪽에 항상 포함
 *   추가로 "방금 출구 처리한 link_id 에서 즉시 재진입 금지" 가드를 link_id 만이 아니라 **road_id
 *   까지 같이 비교**하도록 정정 — 기존엔 link_id 만 비교해서 "같은 링크의 다른 구역 입구 게이트"까지
 *   같이 막아버리는 문제가 있었음(예: A구역 출구와 B구역 입구가 같은 링크에 있는 경우, A 출구 직후
 *   B 입구까지 막힘). road_id 도 같아야만(=진짜 자기 자신 재진입) 막도록 수정.
 *   이 가드 덕분에 이제 "같은 tick 에서 A구역 출구 처리 직후 곧바로 B구역 입구 처리"까지 자연스럽게
 *   이어짐(진출 처리 후 return 하지 않고 바로 진입 후보 검사로 넘어가는 구조로 변경).
 *
 * @remark **진입 경로는 둘이다** (2026-09-23 최정우 추가, 사용자 지시 — 함수 끝 "구역 중간 진입" 블록)
 *   ① 입구게이트 통과 — 입구(I/B) 게이트가 달린 링크에 매칭됐을 때. 게이트ID를 확정해 FROM_ID 로 쓴다.
 *      그 링크 위에서 **게이트 지점을 이미 지난 채** 트립이 시작됐으면 bAmbiguousStart 로 보고
 *      게이트ID를 비운다(그 트립에서 통과 장면을 관측한 적이 없으므로 귀속시키지 않는다).
 *   ② 구역 중간 진입 — 입구게이트 링크가 아닌 곳(교차로·램프)에서 구역 링크에 올라탄 경우.
 *      ①이 성립하지 않으면 m_mapClosedZoneLinkToRoadId 로 그 링크의 구역을 찾아 게이트ID 공란으로
 *      run 을 연다. 상태 세팅은 bAmbiguousStart 와 **완전히 동일**하다.
 *   두 경우 모두 szEntryTollgateId 가 비므로, 마감 시 bEntryMissing 판정이 자동으로 N/3 +
 *   NCR_CLOSED_ENTRY_UNOBSERVED(21) 를 붙인다 — 새 판정을 만들지 않고 기존 분기에 얹는 것이 요점이다.
 *   **왜 등록하는가**: 개방형(OPEN)이 같은 상황을 "구역 진입만으로 run 을 열고 게이트 미통과는
 *   N/3 + 코드11" 로 처리하는 것이 이 엔진의 확정 관례다(완전 배제 SKIP=4 가 아니라 심사대상 3 —
 *   2026-08-13 사용자 지시로 4→3 정정). 폐쇄형만 그 관례를 구현하지 못해 중간 진입 구간이
 *   일반도로로 흡수되고 있었다(실측 000370_20260826143912 RL-Z00009 본선 3,337m).
 *
 * @remark **게이트 근거가 없는 run(bClosedEntryAmbiguous)의 거리 산정** (2026-09-23 최정우)
 *   진입 쪽은 게이트가 아니라 **실측 tick** 부터 세고(dfClosedAccumDistM=0 시작), 이탈 쪽도 같은
 *   기준으로 맞춰 ApplyZoneExitTailDist(링크 종료 노드까지 연장)를 **걸지 않는다**. 한쪽만 늘리면
 *   한 행 안에서 거리·시각·좌표가 어긋난다 — 실측 000370 seq11 은 1 tick 스쳤을 뿐인데 링크 잔여
 *   326m 가 실려 326m/1초/1,174km/h 가 됐다.
*/
void CRawLogWorker::ProcessClosedRoadCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 같은 링크 재진입 재발화 차단 해제 — 진출했던 링크를 벗어나면 표시를 지운다.
	//   (세션 필드 qwClosedExitedLinkID 주석 참고, 2026-09-21 최정우 추가)
	if ((pstSession->qwClosedExitedLinkID != 0)
		&& (stMatchLinkInfo.qwLinkID != pstSession->qwClosedExitedLinkID))
	{
		pstSession->qwClosedExitedLinkID = 0;
		pstSession->szClosedExitedRoadId[0] = '\0';
	}

	// 진출 처리 먼저 시도 — 이미 진입해 있는 상태일 때만. 이 링크의 출구(O/B) 게이트 후보 전부를
	//   모아 활성 세션의 road_id 와 실제로 일치하는 것을 찾음(같은 링크에 다른 구역 출구 게이트가
	//   섞여 있어도 정확히 골라냄) (2026-08-13 최정우 재작성)
	if (pstSession->bInClosedRoad)
	{
		// 실시간 누적거리·마지막 위치 갱신 — 게이트 확정 여부와 무관하게 매 틱 항상 최신으로
		//   유지한다. 게이트 기반 정상 종료는 여전히 ZONE_INFO.dfLengthM(고정값)을 쓰지만, 게이트를
		//   못 찾고 구역을 벗어나거나(아래 else 분기) TTL/트립종료로 강제마감되는 경우
		//   (AppendExpiredClosedRoadCharge)엔 이 실측값이 유일한 근거라 "게이트 대기 중"이었던
		//   틱들도 빠짐없이 누적돼 있어야 한다(실측 000376_20260819140856 RL-Z00003 — 게이트를
		//   찾긴 했지만 위치 미도달로 대기만 하다 트립이 끝난 케이스에서 처음엔 이 갱신이 빠져
		//   dist_m=0으로 남는 문제 발견, 2026-08-25 최정우 추가)
		// [버그 수정, 2026-09-11 최정우] 정지 중(bSameRawAndHeadingAsPrev) GPS 저주파 위치표류가
		//   매 tick 세그먼트 재투영에 반영돼 dist_m 이 과다 계상되는 걸 막는다(OPEN/EXEMPT/NODE_STEP
		//   동일 수정과 같은 근거) — "게이트 확정 여부와 무관하게 매 틱 갱신"이라는 위 의도는
		//   그대로 유지하면서, 진짜 정지 tick만 추가로 제외한다.
		// [버그 수정, 2026-09-15 최정우] **구역 안 tick 만 누적** — ProcessSpeedZoneCharge() 동일
		//   수정과 같은 근거(면제도로가 2026-08-30 에 받은 규칙). 게이트를 안 거치고 구역을 벗어나면
		//   bInClosedRoad 가 안 풀려 구역 밖 주행거리가 폐쇄형 거리로 쌓인다. 이 누적값은 TTL·
		//   트립종료 강제마감과 "게이트 못 찾고 구역 이탈" 분기의 dist_m 근거라 그대로 과금에 반영된다
		//   (현재 실데이터엔 해당 케이스가 없어 피해는 없지만 구조가 구간단속과 동일해 같이 고친다).
		//   [재수정, 2026-09-15 최정우] ProcessSpeedZoneCharge() 와 동일하게 면제도로 구조로 맞춤 —
		//   구역 안 위치(dfClosedLastZoneX/Y)만 누적 기준점으로 쓰고 누적·위치·링크·시각·순번을
		//   한 덩어리로 갱신한다. 처음엔 dfClosedLastX/Y 를 구역 밖에서도 갱신했는데, 그러면 거리는
		//   구역 안으로 잘리면서 TO_LAT/LON 은 구역 밖을 가리켜 한 행 안에서 어긋난다.
		PZONE_INFO pstTrackingZoneForUpdate = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szClosedRoadId);
		// [2026-09-15 최정우] fail-open — ProcessSpeedZoneCharge() 동일 근거
		const bool bClosedZoneKnown = (pstTrackingZoneForUpdate != nullptr);
		const bool bMatchedInClosedZone = !bClosedZoneKnown
			|| IsMatchedLinkInZone(pstTrackingZoneForUpdate, stMatchLinkInfo);
		if (!bClosedZoneKnown)
		{
			LOGFMTW("[#%02d] closed road zone info missing!device=[%s] trip_id=[%s] seq=[%u] road=[%s] "
				"-> 구역 판정 불가, 누적 계속(fail-open)",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRawLogInfo.dwSeqNo, pstSession->szClosedRoadId);
		}

		// "구역 안에서 마지막으로 확인된 링크/시각" 갱신 — qwLastConfirmedLinkID 가 트립 시작
		//   후보(A/B) 판정 중이라 아직 0인 상태로 진입~이탈이 한 tick 만에 벌어지면(실측
		//   000376_20260819140856) 아래 bExitOnPrevLink 판정이 qwLastConfirmedLinkID 만으론
		//   출구 게이트를 못 찾는다 — 그 대체 근거로 매 틱 갱신(2026-08-25 최정우 추가)
		//   [재수정2, 2026-09-15 최정우] 정지틱 가드는 거리·위치에만 — ProcessSpeedZoneCharge()
		//   동일 근거(2026-09-11 OPEN 회귀와 같은 배선 실수 방지)
		if (bMatchedInClosedZone)
		{
			if (!stMatchLinkInfo.bSameRawAndHeadingAsPrev)
			{
				POINT stPrevPos, stCurPos;
				stPrevPos.dfX = pstSession->dfClosedLastZoneX;  stPrevPos.dfY = pstSession->dfClosedLastZoneY;
				stCurPos.dfX = stMatchLinkInfo.dfMatchX;        stCurPos.dfY = stMatchLinkInfo.dfMatchY;
				if ((pstSession->dfClosedLastZoneX != 0.0) || (pstSession->dfClosedLastZoneY != 0.0))
					pstSession->dfClosedAccumDistM += HaversineMeters(stPrevPos, stCurPos);
				pstSession->dfClosedLastZoneX = stMatchLinkInfo.dfMatchX;
				pstSession->dfClosedLastZoneY = stMatchLinkInfo.dfMatchY;
				pstSession->dwClosedLastGpsSeq = stRawLogInfo.dwSeqNo;
			}
			// 구역 안 마지막 확인 링크/시각/순번 — 정지 여부와 무관하게 매 tick 갱신
			pstSession->qwClosedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
			pstSession->dtClosedLastZoneTime = stRawLogInfo.dtGPS;
			pstSession->dwClosedLastZoneGpsSeq = stRawLogInfo.dwSeqNo;
		}

		// 세션 마지막 매칭 위치 — 구역 소속과 무관하게 매 tick 갱신(기존 의미 유지)
		if (!stMatchLinkInfo.bSameRawAndHeadingAsPrev)
		{
			pstSession->dfClosedLastX = stMatchLinkInfo.dfMatchX;
			pstSession->dfClosedLastY = stMatchLinkInfo.dfMatchY;
		}

		// 경유(이미 완전 통과) 링크들을 먼저 확인 — 발견되면 위치 판정 없이 무조건 통과로 확정
		//   (2026-08-20 최정우 추가, 근거는 CollectGateCandidatesOnIntermediateLinks() 주석 참고)
		PGATE_INFO pstExitGate = nullptr;
		bool bExitOnIntermediate = false;

		vector<PGATE_INFO> vtIntermediateExit;
		CollectGateCandidatesOnIntermediateLinks(m_stConfig.pcChargeDataLoader,
			stMatchLinkInfo.aqwPathLinkIDs, stMatchLinkInfo.nPathLinkCount, 'O', &vtIntermediateExit);
		for (size_t i = 0; i < vtIntermediateExit.size(); ++i)
		{
			if (strcmp(pstSession->szClosedRoadId, vtIntermediateExit[i]->szRoadID) == 0)
			{
				pstExitGate = vtIntermediateExit[i];
				bExitOnIntermediate = true;
				break;
			}
		}

		if (pstExitGate == nullptr)
		{
			vector<PGATE_INFO> vtExitCandidates;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtExitCandidates);

			for (size_t i = 0; i < vtExitCandidates.size(); ++i)
			{
				if (strcmp(pstSession->szClosedRoadId, vtExitCandidates[i]->szRoadID) == 0)
				{
					pstExitGate = vtExitCandidates[i];
					break;
				}
			}
		}

		// 직전 확정 링크에 출구 게이트가 있었는데 그 안에서 위치 3m 이내로 못 잡은 경우 — 다음
		//   틱이 아예 다른 링크로 확정됐다는 것 자체가 그 링크를 끝까지 지나왔다는 뜻이므로 위치
		//   판정 없이 확정한다. 자세한 배경은 ProcessSpeedZoneCharge() 동일 로직 주석 참고
		//   (2026-08-24 최정우 추가)
		// 후보 링크 두 개를 순서대로 시도 — (1) qwLastConfirmedLinkID, (2) qwClosedLastZoneLinkID.
		//   qwLastConfirmedLinkID 는 트립 시작 후보(A/B) 판정이 아직 안 끝났으면 0에 머물러 있어
		//   못 쓰는 경우가 있다(실측 000376_20260819140856 — 트립 첫 tick 이 이미 구역 안이라
		//   그 tick 이 바로 진입+구역 내 유일한 링크였는데, 다음 tick 에 완전히 다른 링크로 넘어가며
		//   출구를 못 잡고 "게이트 미확인 이탈"로 빠짐). qwClosedLastZoneLinkID 는 그 대체용으로
		//   "실제로 이 구역 안에서 마지막으로 확인됐던 링크"를 추적한 값이라 이 경우에도 유효
		//   (2026-08-25 최정우 추가, 사용자 지시 — "다음 맵매칭 좌표/링크로 진출 확인 가능")
		bool bExitOnPrevLink = false;
		uint64 qwExitPrevLinkID = 0;
		time_t dtExitPrevLinkTime = 0;
		if (pstExitGate == nullptr)
		{
			const uint64 aqwPrevCand[2] = { pstSession->qwLastConfirmedLinkID, pstSession->qwClosedLastZoneLinkID };
			const time_t adtPrevCandTime[2] = { pstSession->dtLastConfirmedLinkTime, pstSession->dtClosedLastZoneTime };
			for (int c = 0; (c < 2) && (pstExitGate == nullptr); ++c)
			{
				if ((aqwPrevCand[c] == 0) || (aqwPrevCand[c] == stMatchLinkInfo.qwLinkID))
					continue;

				vector<PGATE_INFO> vtPrevExit;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, aqwPrevCand[c], 'O', &vtPrevExit);
				for (size_t i = 0; i < vtPrevExit.size(); ++i)
				{
					if (strcmp(pstSession->szClosedRoadId, vtPrevExit[i]->szRoadID) == 0)
					{
						pstExitGate = vtPrevExit[i];
						bExitOnPrevLink = true;
						qwExitPrevLinkID = aqwPrevCand[c];
						dtExitPrevLinkTime = adtPrevCandTime[c];
						break;
					}
				}
			}
		}
		// ④ 누락 링크 복구 경로에서 출구 게이트 찾기 — FindExitGateOnRecoveredPath() 주석 참고
		//   (2026-10-04 최정우 추가). 찾으면 "경유 링크 통과"와 같이 위치 비교 없이 확정한다.
		if (pstExitGate == nullptr)
		{
			pstExitGate = FindExitGateOnRecoveredPath(nThreadId, stRawLogInfo, stMatchLinkInfo,
				pstSession->qwClosedLastZoneLinkID, pstSession->szClosedRoadId);
			if (pstExitGate != nullptr)
				bExitOnIntermediate = true;
		}

		if (pstExitGate != nullptr)
		{
			// 위치(구간 내 지점) 비교는 "이번 확정 링크 안에서 아직 진행 중"인 경우에만 필요 —
			//   경유 링크에서 찾았으면 이미 링크 전체를 지났으므로 생략 (2026-08-20 최정우 추가)
			if (!bExitOnIntermediate && !bExitOnPrevLink)
			{
				POINT stLinkStart, stGatePos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stGatePos.dfX = pstExitGate->dfLon;
				stGatePos.dfY = pstExitGate->dfLat;
				// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
				//   (2026-09-07 최정우 수정, 사용자 지적)
				double dfGatePosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stGatePos.dfX, stGatePos.dfY);
				if (dfGatePosOnLink < 0.0)
					dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);		// 형상 없음 — 종전 직선거리
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				// 출구 게이트 지점을 아직 안 지났으면(약간의 오차 허용 -3m) 이번 tick 은 통과 전 —
				//   누적거리만 반영하고 종료 대기, 진입 후보 검사도 아직은 무의미하므로 여기서 끝 (2026-08-12 최정우 추가)
				if (dfCurPosOnLink < (dfGatePosOnLink - 3.0))
					return;
			}

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "2";							// CLOSED_ROAD
			stRow.strChargeUnit = "1";							// LINK (실측 확인)
			stRow.strLinkId = "";

			stRow.strFromId = pstSession->szEntryTollgateId;
			stRow.strToId = pstExitGate->szTollgateID;

			// 입/출구 게이트 이상(둘이 같거나 하나라도 비어있음) — 정상 과금 대상 아님으로 표시.
			//   charge_status=3(AUDIT=심사대상) — 완전 배제(SKIP=4)가 아니라 사람이 재확인하도록 표시
			//   (사용자 지시, 2026-08-13 — 원래 4(SKIP)였다가 3(AUDIT)으로 정정)
			const bool bEntryMissing = (pstSession->szEntryTollgateId[0] == '\0');
			const bool bExitMissing = (pstExitGate->szTollgateID[0] == '\0');
			const bool bEntryEqualsExit = !bEntryMissing && !bExitMissing
				&& (strcmp(pstSession->szEntryTollgateId, pstExitGate->szTollgateID) == 0);
			bool bGateAnomaly = bEntryMissing || bExitMissing || bEntryEqualsExit;
			stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
			stRow.strChargeStatus = bGateAnomaly ? "3" : "0";
			if (bGateAnomaly)
			{
				// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(bGateAnomaly,
				//   N/3) 자체는 안 건드리고 세 사유 중 어느 것인지만 구분해서 남긴다. 우선순위는
				//   원래 OR 조건 순서(입구 미확인→출구 미확인→입구==출구)와 동일
				const int nNonChargeReason = bEntryMissing ? NCR_CLOSED_ENTRY_UNOBSERVED
					: bExitMissing ? NCR_CLOSED_EXIT_UNCONFIRMED : NCR_CLOSED_ENTRY_EQUALS_EXIT;
				char szReason[8];
				snprintf(szReason, sizeof(szReason), "%d", nNonChargeReason);
				stRow.strNonChargeReason = szReason;
				// 로그는 아래 push_back 직후 한 곳에서 Y/N 공통 형식으로 남긴다(중복 로그 방지,
				//   2026-09-11 최정우 — open zone exit recorded() 와 동일 패턴)
			}

			// 출구 게이트 통과 시각 보간 — 직전 확정 tick~이번 tick 사이에서 게이트를 지났을 시각을
			//   추정한다(사용자 지시, 2026-08-25 최정우 추가). bExitOnPrevLink 로 확정된 경우 이번
			//   틱의 stMatchLinkInfo 는 이미 다른(무관한) 링크를 가리키지만, 보간 기준으로는 오히려
			//   "그 무관한 링크까지 이동한 시각"이 곧 게이트를 확실히 지난 시점이라 유효한 종점이 됨.
			//   직전 tick 이 없으면(트립 첫 tick) 예전처럼 dtExitPrevLinkTime/현재시각 그대로 사용
			//   (원래 방식: 2026-08-24 최정우 추가, dtExitPrevLinkTime 로 일반화 2026-08-25)
			time_t dtExitTime;
			if (pstSession->bHasLastMatch)
			{
				dtExitTime = InterpolateGateCrossingTime(
					pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
					pstExitGate->dfLon, pstExitGate->dfLat);
			}
			else
			{
				dtExitTime = bExitOnPrevLink ? dtExitPrevLinkTime : stRawLogInfo.dtGPS;
			}
			// end_gps_seq — 게이트를 이번 tick에서 확정했으면 이번 tick, 직전 링크 기준(bExitOnPrevLink)이면
			//   구역 안에서 마지막으로 확인됐던 tick(dtExitPrevLinkTime과 동일 소스) (2026-08-28 최정우 추가)
			uint32 dwExitGpsSeq = bExitOnPrevLink ? pstSession->dwClosedLastZoneGpsSeq : stRawLogInfo.dwSeqNo;

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfEntryFromLat);
			snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfEntryFromLon);
			if (bExitOnPrevLink)
			{
				snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);
			}
			else
			{
				snprintf(szToLat, sizeof(szToLat), "%.06lf", stMatchLinkInfo.dfEdNodeY);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", stMatchLinkInfo.dfEdNodeX);
			}
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szClosedRoadId;
			// [2026-09-15 최정우] 같은 tick 에 이미 조회한 포인터를 재사용 — 중복 조회(뮤텍스 2회)를
			//   없애고, 두 조회 사이에 기준정보 캐시 세대가 바뀌어 서로 다른 세대를 보는 일도 막는다
			PZONE_INFO pstZone = pstTrackingZoneForUpdate;
			stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

			// 구간거리 — 원래는 실시간 GPS 누적이 아니라 base_roadlink.coords 폴리라인 실거리
			//   (ZONE_INFO.dfLengthM, [zone_select] SQL 에서 하버사인 합산으로 미리 계산됨) 사용
			//   (2026-08-12 최정우 수정). 단, 진입 자체가 애매(bClosedEntryAmbiguous — 트립이 이미
			//   폐쇄형 도로 위에서 시작해 진입 게이트를 못 잡은 경우)했으면 "구역 전체를 다 지났다"는
			//   전제가 성립하지 않으므로, 대신 실제 출발 지점(dfEntryFromLat/Lon)~출구 게이트 간
			//   실거리를 쓴다(사용자 지시, 2026-08-25 최정우 추가)
			double dfLengthM;
			if (pstSession->bClosedEntryAmbiguous)
			{
				POINT stStartPos, stGatePos;
				stStartPos.dfX = pstSession->dfEntryFromLon;  stStartPos.dfY = pstSession->dfEntryFromLat;
				stGatePos.dfX = pstExitGate->dfLon;           stGatePos.dfY = pstExitGate->dfLat;
				dfLengthM = CalcZoneSpanM(pstZone, stStartPos.dfX, stStartPos.dfY, stGatePos.dfX, stGatePos.dfY,
					HaversineMeters(stStartPos, stGatePos));		// 2026-10-04: 직선 -> 구역 선을 따라
			}
			else
			{
				// 2026-10-04 최정우 수정(사용자 확정): 구역 전체 길이 -> 진입·진출 게이트 사이 선 길이
				PGATE_INFO pstEntryGateForLen = (m_stConfig.pcChargeDataLoader != nullptr)
					? m_stConfig.pcChargeDataLoader->GetGateByTollgateId(pstSession->szEntryTollgateId) : nullptr;
				const double dfZoneLen = (pstZone != nullptr) ? pstZone->dfLengthM : 0.0;
				dfLengthM = (pstEntryGateForLen != nullptr)
					? CalcZoneSpanM(pstZone, pstEntryGateForLen->dfLon, pstEntryGateForLen->dfLat,
						pstExitGate->dfLon, pstExitGate->dfLat, dfZoneLen)
					: dfZoneLen;
			}
			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
			stRow.strDistM = szDistM;

			double dfDwellSec = difftime(dtExitTime, pstSession->dtEntryTime);
			// 주행은 실제로 있었으므로 경과시간이 0(또는 반올림 시 0이 되는 소수)으로 남으면 안 됨 —
			//   최소 1초로 보정(사용자 지시, 2026-08-25 최정우 추가)
			if (dfDwellSec < 1.0)
				dfDwellSec = 1.0;

			// 평균속도 — 구역 실거리 ÷ 입구~출구 경과시간(사용자 지시, 2026-08-14 — 개방형 제외 전
			//   유형 평균속도로 통일. 기존엔 개방형과 동일하게 순간속도를 썼으나, 구역 개념이 있는
			//   유형은 구간단속과 동일하게 평균속도가 맞다는 지시로 변경)
			{
				double dfAvgSpeedKmh = (dfLengthM / dfDwellSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}

			// 제한속도 — 진입 애매(bClosedEntryAmbiguous) 시엔 이 레코드가 어느 구간의 제한속도를
			//   대표하는지 정의할 근거 자체가 없음(입구 링크를 특정 못 함) — 0으로 둔다(사용자 지시,
			//   2026-08-25 최정우 추가). 그 외엔 기존과 동일하게 매칭 링크(또는 bExitOnPrevLink 시
			//   직전 링크)의 실제 도로 제한속도 사용
			if (pstSession->bClosedEntryAmbiguous)
			{
				stRow.strSpeedLimitKmh = "0";
			}
			else
			{
				uint8 nSpeedLimitForRow = stMatchLinkInfo.nMaxSpeed;
				if (bExitOnPrevLink && (m_stConfig.pcDataLoader != nullptr))
				{
					PLINK_INFO pstPrevLink = m_stConfig.pcDataLoader->GetLinkInfo(qwExitPrevLinkID);
					if (pstPrevLink != nullptr)
						nSpeedLimitForRow = pstPrevLink->nMaxSpeed;
				}
				char szSpeedLimit[16];					// (2026-09-15 최정우 수정) 8 → 16: 8 이면 7자리 초과 값이 잘려
											//   엉뚱한 수가 된다(제한속도는 실무상 3자리지만 기준정보 오류 시
											//   큰 값이 들어올 수 있다). [2026-09-17 정정] 종전 주석의 "클램프
											//   WARN 로그" 는 이 함수들에 존재하지 않는 내용이라 삭제
				snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(nSpeedLimitForRow));
				stRow.strSpeedLimitKmh = szSpeedLimit;
			}

			// stay_seconds — 입구~출구 체류시간(초), 사용자 지시(2026-08-14 — 개방형 제외 전 유형 공통화).
			//   위 dfDwellSec(평균속도 계산에 이미 씀)과 동일한 경과시간 재사용
			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			char szStartGpsSeq[16], szEndGpsSeq[16];
			snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", pstSession->dwEntryGpsSeq);
			snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwExitGpsSeq);
			stRow.strStartGpsSeq = szStartGpsSeq;
			stRow.strEndGpsSeq = szEndGpsSeq;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtEntryTime);		// 입구 통과 시각(실측 패턴과 일치)

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			if (pszTripStartDt != nullptr)
				stRow.strTripStartDt = pszTripStartDt;
			else
				stRow.strTripStartDt = stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szEntryTollgateId;
			stRow.strExitTollgateId = pstExitGate->szTollgateID;

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			pvtChargeInserts->push_back(stRow);

			// [사용자 지시, 2026-09-11 최정우] Y/0·N/x 공통 형식 — 사유가 있으면(=N/3) WARN 승격,
			//   없으면(=Y/0) INFO 로 코드 0(정상 과금) 표시. trip_id/seq 로 동일하게 추적 가능
			if (stRow.strNonChargeReason.empty())
			{
				LOGFMTI("[#%02d] closed road exit charge queued!device=[%s] trip_id=[%s] seq=[%d] entry=[%s] "
					"exit=[%s] dist_m=[%s] non_charge_reason=[%d:%s]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
					pstSession->szEntryTollgateId, pstExitGate->szTollgateID, szDistM,
					NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
			}
			else
			{
				const int nNonChargeReason = atoi(stRow.strNonChargeReason.c_str());
				LOGFMTW("[#%02d] closed road exit charge queued(AUDIT)!device=[%s] trip_id=[%s] seq=[%d] "
					"entry=[%s] exit=[%s] dist_m=[%s] non_charge_reason=[%d:%s]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
					pstSession->szEntryTollgateId, pstExitGate->szTollgateID, szDistM, nNonChargeReason,
					m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nNonChargeReason));
			}

			pstSession->nChargeSeq += 1;
			// 진출게이트 지점을 이월해 둔다 — 다음 일반도로 run 이 이 지점부터 시작하도록
			//   (세션 필드 주석 참고). 게이트가 매칭 tick 과 떨어져 있어도 게이트가 곧 구역의
			//   끝이므로 그 지점부터가 일반도로다 (2026-09-06 최정우 추가, 사용자 지시)
			pstSession->bHasGateExitCarry = true;
			pstSession->dfGateExitX = pstExitGate->dfLon;
			pstSession->dfGateExitY = pstExitGate->dfLat;
			pstSession->dtGateExit = dtExitTime;
			pstSession->dwGateExitGpsSeq = dwExitGpsSeq;
			pstSession->qwGateExitLinkID = pstExitGate->qwLinkID;
			{
				POINT stGateP, stTickP;
				stGateP.dfX = pstExitGate->dfLon;      stGateP.dfY = pstExitGate->dfLat;
				stTickP.dfX = pstSession->dfClosedLastX; stTickP.dfY = pstSession->dfClosedLastY;
				pstSession->bGateExitAtTick = (HaversineMeters(stGateP, stTickP) <= 2.0);
			}
			// 이 링크에서 진출했음을 표시 — 정차 중 GPS 드리프트로 같은 링크에서 재진입이
			//   재발화하는 것을 막는다(세션 필드 주석 참고, 2026-09-21 최정우 추가)
			pstSession->qwClosedExitedLinkID = stMatchLinkInfo.qwLinkID;
			strncpy(pstSession->szClosedExitedRoadId, pstSession->szClosedRoadId,
				sizeof(pstSession->szClosedExitedRoadId) - 1);
			pstSession->szClosedExitedRoadId[sizeof(pstSession->szClosedExitedRoadId) - 1] = '\0';
			pstSession->bInClosedRoad = false;
			pstSession->szEntryTollgateId[0] = '\0';
			pstSession->szClosedRoadId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 — 이 링크가 바로 다음 구역의
			//   입구 게이트도 겸하는 경우(사용자 질문 케이스) 같은 tick 에 바로 이어서 처리 (2026-08-13 최정우 추가)
		}
		else
		{
			// 게이트를 못 찾음 — 이 구역(link_ids)을 완전히 벗어났는지로 "확정 이탈" 여부만 보조
			//   판정한다(실측 누적거리·위치는 위에서 이미 매 틱 갱신됨). 아직 구역 안이면 계속
			//   대기, 완전히 벗어났으면 게이트 미확인이라 여전히 N/3(AUDIT)이지만 dist_m/speed_kmh/
			//   stay_seconds는 0 대신 실측값으로 채워 마감한다(실측 000376_20260819140856 — 게이트를
			//   못 찾고 구역을 나갔는데 전부 0으로 비어있던 문제, 2026-08-25 최정우 추가)
			// [2026-09-15 최정우] 같은 tick 에 이미 조회한 포인터를 재사용 — 중복 조회(뮤텍스 2회)를
			//   없애고, 두 조회 사이에 기준정보 캐시 세대가 바뀌어 서로 다른 세대를 보는 일도 막는다
			PZONE_INFO pstTrackingZone = pstTrackingZoneForUpdate;

			if (IsMatchedLinkInZone(pstTrackingZone, stMatchLinkInfo))
			{
				pstSession->nClosedExitTicks = 0;	// 구역 안 확인 — 이탈 스트릭 리셋 (2026-09-21 최정우 추가)
				return;			// 아직 구역 안 — 다음 틱 대기
			}

			// 링크가 2개 이상인 구역은 "지금 이 링크가 link_ids 에 없다"만으로 확정 이탈 처리하면
			//   안 된다 — 교차로 부근에서 잠깐 인접한 비구성원 링크로 튀었다가 같은 구역의 남은
			//   링크로 되돌아오는 매칭 흔들림이 실측으로 확인됨(000376_20260819141002 RL-Z00006,
			//   6개 링크 중 3번째 링크 대신 잠깐 옆 링크로 튀었다가 복귀 — 이걸 확정 이탈로 오판해
			//   실제 출구 게이트(TG00011)에 도달하기 훨씬 전에 1266m/144s 짜리 정상 통과를
			//   459m/24s 로 조기 마감시켜버림). 링크 1개짜리 구역(RL-Z00005 등)은 "그 링크를
			//   벗어남=구역을 벗어남"이 유일한 해석이라 안전하지만, 여러 링크로 구성된 구역은
			//   TTL/트립종료 마감(AppendExpiredClosedRoadCharge)에 맡기는 게 더 안전함 — 그쪽도
			//   이미 실측 누적거리를 쓰므로 dist_m=0 으로 비는 문제는 없다 (2026-08-25 최정우 추가,
			//   전체 재맵매칭 회귀검증 중 발견)
			// [버그 수정, 2026-09-21 최정우, 사용자 확정 — 이슈 36] 이 대기에 **상한이 없었다.**
			//   원래 목적은 교차로 부근 매칭 흔들림(1~2 tick) 흡수인데, 상한이 없어 **실제 우회
			//   주행까지 구역 체류로 흡수**했다 — 실측 000984_20250903153702 RL-Z00006(3링크):
			//   진입게이트 통과(seq7~10) 직후 구역을 벗어나 seq11~30(90초·약 500m, 폐쇄식
			//   RL-Z00005 포함)을 우회한 뒤 seq31 에 구역으로 복귀해 seq45~46 진출게이트 통과.
			//   그런데 세션은 그 90초 내내 열려 있어 체류 165초·평균 16km/h 로 기록됐다
			//   (실제 구역 통과는 63초·41km/h). **우회가 평균속도를 낮춰 구간단속 위반 판정을
			//   가려주는 방향**이라 그대로 둘 수 없다.
			//   NODE_STEP(node_exitcnt)·PARKING(park_exitcnt)이 같은 문제를 이미 연속 tick
			//   디바운스로 풀고 있어 같은 방식을 쓴다 — config zone_exitcnt(기본 3).
			//   거리·시간 기준을 쓰지 않은 이유: 고속 주행 중에는 1 tick 만으로도 수십 m·수 초가
			//   지나 매칭 흔들림에서 오탐한다.
			// 되돌리는 법: config zone_exitcnt=0 이면 종전(무제한 대기)으로 폴백한다.
			if ((pstTrackingZone != nullptr) && (pstTrackingZone->vtLinkIds.size() > 1))
			{
				pstSession->nClosedExitTicks += 1;
				if ((m_stConfig.nZoneExitCnt <= 0)
					|| (pstSession->nClosedExitTicks < m_stConfig.nZoneExitCnt))
					return;		// 아직 일시적 이탈일 수 있음 — 확정 짓지 않고 대기

				LOGFMTI("[#%02d] closed road zone exit debounced!device=[%s] trip_id=[%s] seq=[%u] "
					"road=[%s] out_ticks=[%d/%d] -> 확정 이탈",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					pstSession->szClosedRoadId, pstSession->nClosedExitTicks, m_stConfig.nZoneExitCnt);
			}

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "2";							// CLOSED_ROAD
			stRow.strChargeUnit = "1";
			stRow.strLinkId = "";
			stRow.strFromId = pstSession->szEntryTollgateId;
			stRow.strToId = "";								// 출구 미확인 — 지어내지 않음

			// [2026-09-15 최정우 추가] 이탈 경계 보정 — 구역을 확정 이탈한 시점이라 세션에 직접
			//   적용해도 안전하다(아래에서 마감되고 상태가 리셋된다)
			// [버그 수정, 2026-09-15 최정우 — 오후 재검토] TO 좌표를 만들기 **전에** 돌도록 앞으로
			//   옮겼다(종전엔 TO 를 찍은 뒤에 호출돼 보정 결과가 TO 에 반영되지 못했다).
			// [2026-09-23 최정우 추가, 사용자 확정] 진입게이트 근거가 없는 run(구역 중간 진입·
			//   구역 안에서 트립 시작)에는 이 보정을 걸지 않는다. 보정의 전제는 "구역 경계까지
			//   달리고 나갔다"인데, 그런 run 은 진입 쪽도 게이트가 아니라 **실측 tick** 부터 세므로
			//   이탈 쪽만 링크 종료 노드까지 늘리면 한쪽만 부풀어 한 행 안에서 어긋난다.
			//   실측 000370_20260826143912 seq11: RL-Z00009 를 1 tick 스쳤을 뿐인데 그 링크의
			//   잔여 326m 가 통째로 실려 326m/1초/1,174km/h 가 됐다.
			//   근거 없는 거리를 지어내지 않는다는 이 함수 주변의 기존 관례(TO_ID·SPEED_LIMIT 를
			//   "지어내지 않음" 으로 비워두는 것)와 같은 방향이다.
			if (!pstSession->bClosedEntryAmbiguous)
			{
				ApplyZoneExitTailDist(pstSession->qwClosedLastZoneLinkID,
					&pstSession->dfClosedLastZoneX, &pstSession->dfClosedLastZoneY,
					&pstSession->dfClosedAccumDistM);
			}

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfEntryFromLat);
			snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfEntryFromLon);
			// [버그 수정, 2026-09-15 최정우] TO 는 "구역 안 마지막 위치 + 이탈 경계 보정"(= 구역
			//   경계)을 쓴다. 종전엔 이번 tick 의 매칭 좌표였는데, 이탈 디바운스가 확정되는 tick 은
			//   이미 구역에서 여러 tick 벗어난 지점이라 dist_m(구역 내부로 게이팅된 누적거리)과
			//   한 행 안에서 서로 다른 구간을 가리켰다. 구간단속 쪽은 같은 tick 이 만드는 NODE_STEP
			//   미러 행이 이미 df*LastZoneY/X 를 쓰고 있어 두 행이 다른 지점을 가리키기까지 했다.
			//   구역 안 tick 이 한 번도 없었으면(0,0) 근거가 없으므로 종전 동작으로 폴백한다.
			const bool bClosedZonePosKnown = ((pstSession->dfClosedLastZoneX != 0.0)
				|| (pstSession->dfClosedLastZoneY != 0.0));
			snprintf(szToLat, sizeof(szToLat), "%.06lf",
				bClosedZonePosKnown ? pstSession->dfClosedLastZoneY : stMatchLinkInfo.dfMatchY);
			snprintf(szToLon, sizeof(szToLon), "%.06lf",
				bClosedZonePosKnown ? pstSession->dfClosedLastZoneX : stMatchLinkInfo.dfMatchX);
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szClosedRoadId;
			stRow.strZoneName = (pstTrackingZone != nullptr) ? pstTrackingZone->szRoadNm : "";

			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(pstSession->dfClosedAccumDistM + 0.5));
			stRow.strDistM = szDistM;

			// [버그 수정, 2026-09-23 최정우, 사용자 확정] 종료 시각·순번도 **구역 안 마지막 tick**
			//   기준으로 맞춘다. 2026-09-15 에 같은 근거로 TO 좌표만 고치고 이 둘을 놓쳤다 —
			//   이탈 디바운스가 확정되는 tick 은 이미 구역을 여러 tick 벗어난 지점이라,
			//   dist_m(구역 내부로 게이팅된 누적거리)·TO 좌표와 한 행 안에서 서로 다른 구간을
			//   가리킨다. 그 결과 체류시간이 구역 밖 주행분까지 포함해 부풀고(평균속도는 반대로
			//   낮아짐), END_GPS_SEQ 는 **이미 다음 구역에 들어간 tick**을 가리켜 앞뒤 폐쇄형 행의
			//   구간이 1 tick 씩 겹쳤다(실측 000370_20260826143912: 11~18 / 18~79 / 79~186).
			//   구역 안 tick 이 한 번도 없었으면 근거가 없으므로 종전 동작으로 폴백한다 —
			//   바로 위 TO 좌표의 bClosedZonePosKnown 과 같은 관례.
			const bool bClosedZoneTickKnown = (pstSession->dwClosedLastZoneGpsSeq != 0)
				&& (pstSession->dtClosedLastZoneTime != 0);
			const time_t dtZoneEndTime = bClosedZoneTickKnown
				? pstSession->dtClosedLastZoneTime : stRawLogInfo.dtGPS;
			const uint32 dwZoneEndGpsSeq = bClosedZoneTickKnown
				? pstSession->dwClosedLastZoneGpsSeq : stRawLogInfo.dwSeqNo;

			double dfDwellSec = difftime(dtZoneEndTime, pstSession->dtEntryTime);
			if (dfDwellSec > 0.0)
			{
				double dfAvgSpeedKmh = (pstSession->dfClosedAccumDistM / dfDwellSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}
			stRow.strSpeedLimitKmh = "";						// 출구 링크를 특정 못 함 — 지어내지 않음

			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			char szStartGpsSeq[16], szEndGpsSeq[16];
			snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", pstSession->dwEntryGpsSeq);
			snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwZoneEndGpsSeq);
			stRow.strStartGpsSeq = szStartGpsSeq;
			stRow.strEndGpsSeq = szEndGpsSeq;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtEntryTime);

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szEntryTollgateId;
			stRow.strExitTollgateId = "";

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			stRow.strChargeYn = "N";							// 출구 게이트 미확인 — 항상 AUDIT
			stRow.strChargeStatus = "3";
			// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(N/3)은 안 건드림.
			//   이 분기는 경유경로 재구성으로도 출구게이트를 못 찾고 구역만 확정 이탈한 케이스라
			//   bGateAnomaly 분기(21/22/23)와 동일한 사유(23, 출구 미확인)
			// [2026-09-23 최정우 보완] 입구게이트마저 없으면 21(진입게이트 미확인)이 먼저다 —
			//   bGateAnomaly 분기의 우선순위(입구 미확인 → 출구 미확인 → 입구==출구)와 맞춘다.
			//   구역 중간 진입(입구게이트 미통과)으로 열린 run 이 이 경로로 마감되는데, 그때
			//   "출구를 못 찾았다"고만 적으면 진짜 사유인 "입구를 안 지났다"가 가려진다.
			const int nZoneLeftReason = (pstSession->szEntryTollgateId[0] == '\0')
				? NCR_CLOSED_ENTRY_UNOBSERVED : NCR_CLOSED_EXIT_UNCONFIRMED;
			char szReason[8];
			snprintf(szReason, sizeof(szReason), "%d", nZoneLeftReason);
			stRow.strNonChargeReason = szReason;

			pvtChargeInserts->push_back(stRow);

			LOGFMTW("[#%02d] closed road exit unconfirmed(zone left)!device=[%s] trip_id=[%s] seq=[%d] "
				"entry=[%s] road=[%s] dist_m=[%s] non_charge_reason=[%d:%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
				pstSession->szEntryTollgateId, pstSession->szClosedRoadId, szDistM,
				nZoneLeftReason, m_cCodeMap.GetValue(NonChargeReasonTable,
				NOE(NonChargeReasonTable), nZoneLeftReason));

			pstSession->nChargeSeq += 1;
			// 이 링크에서 진출했음을 표시 — 정차 중 GPS 드리프트로 같은 링크에서 재진입이
			//   재발화하는 것을 막는다(세션 필드 주석 참고, 2026-09-21 최정우 추가)
			pstSession->qwClosedExitedLinkID = stMatchLinkInfo.qwLinkID;
			strncpy(pstSession->szClosedExitedRoadId, pstSession->szClosedRoadId,
				sizeof(pstSession->szClosedExitedRoadId) - 1);
			pstSession->szClosedExitedRoadId[sizeof(pstSession->szClosedExitedRoadId) - 1] = '\0';
			pstSession->bInClosedRoad = false;
			pstSession->szEntryTollgateId[0] = '\0';
			pstSession->szClosedRoadId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 — 게이트 확정 이탈과 동일 관례
		}
	}

	// 진입 처리 — 이미 다른 구역에 진입해 있는 상태면(bInClosedRoad=true 로 위 블록에서 못 닫혔으면)
	//   새 진입을 받지 않음(한 번에 하나의 폐쇄형 구역만 추적 가능) (2026-08-13 최정우 재작성)
	if (pstSession->bInClosedRoad)
		return;

	// 경유 링크 포함 전체 경로에서 입구 후보 수집 — 진입은 위치 판정이 필요 없어(어느 지점에서
	//   발견되든 그 즉시 진입 확정) 경유/최종 구분 없이 단순 병합 (2026-08-20 최정우 추가)
	vector<PGATE_INFO> vtEntryCandidates;
	{
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
		{
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'I', &vtEntryCandidates);
		}
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
			{
				vector<PGATE_INFO> vtOne;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.aqwPathLinkIDs[i], 'I', &vtOne);
				vtEntryCandidates.insert(vtEntryCandidates.end(), vtOne.begin(), vtOne.end());
			}
		}
	}

	for (size_t i = 0; i < vtEntryCandidates.size(); ++i)
	{
		PGATE_INFO pstEntryGate = vtEntryCandidates[i];
		bool bAmbiguousStart = false;			// 아래 참고 (2026-08-25 최정우 추가)

		// 이 링크에서 방금 진출한 구역이면 재진입을 받지 않는다 — 정차 중 GPS 드리프트가
		//   링크 진행거리를 출구 임계 안팎으로 오가게 만들어 진출~재진입이 반복되는 것을
		//   막는다. 링크를 벗어나면 위 함수 진입부에서 해제되므로 다음 통행은 정상 진입한다
		//   (2026-09-21 최정우 추가 — 실측 000994_20250903152350 RL-Z00003 398km/h)
		if ((pstSession->qwClosedExitedLinkID != 0)
			&& (stMatchLinkInfo.qwLinkID == pstSession->qwClosedExitedLinkID)
			&& (strcmp(pstEntryGate->szRoadID, pstSession->szClosedExitedRoadId) == 0))
			continue;
		// [2026-09-21 최정우 수정] 이 구역이 내가 다룰 유형인지부터 확인한다 — 종전에는 이 검사가
		//   아래 위치 판정 **뒤**에 있어, 다른 유형 구역(예: 구간단속 RL-Z00006)의 게이트 후보에도
		//   "closed road entry ambiguous" WARN 이 먼저 찍힌 뒤 여기서 조용히 버려졌다. 판정이 5개
		//   구역으로 확대되면서 그 교차 노이즈가 트립당 여러 줄로 늘어 장애 분석을 방해한다.
		//   동작은 동일하다 — 어느 쪽이든 이 후보는 continue 로 건너뛴다.
		PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstEntryGate->szRoadID);
		if ((pstZone == nullptr) || (strcmp(pstZone->szRoadKind, "2") != 0))
			continue;

		// 이번 확정(최종) 링크에서 찾은 후보만 위치 검사 — 경유(이미 완전 통과) 링크에서 찾은
		//   후보는 무조건 확정(위치를 잴 의미가 없음, 위 출구 판정과 동일 원리).
		//   같은 링크에 같은 road_id 출구(O/B) 게이트가 있으면(TG00007/08 처럼 입/출구가 한 링크에
		//   같이 있는 구조) 그 출구 지점을 이미 지났으면 진입 후보에서 제외 — 안 그러면 출구 처리
		//   직후에도 그 링크에 계속 머무는 동안(다음 tick들) 매번 진입~진출이 반복 재발화됨.
		//   근본원인: 출구 판정엔 "아직 도달 전인가" 위치 확인이 있는데 진입 판정엔 전혀 없었음 —
		//   진입 쪽엔 "이미 지나쳤는가"를 확인해야 재발화를 막을 수 있음(2026-08-20 최정우 수정 —
		//   세션에 남기는 방식은 트립 끝까지 재진입 자체를 막아버리는 별개 버그였음, 지역변수로
		//   1tick만 막는 방식도 이 재발화(여러 tick 동안 반복)까진 못 막아 폐기)
		if (pstEntryGate->qwLinkID == stMatchLinkInfo.qwLinkID)
		{
			vector<PGATE_INFO> vtSameLinkExit;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtSameLinkExit);

			PGATE_INFO pstSameLinkExit = nullptr;
			for (size_t e = 0; e < vtSameLinkExit.size(); ++e)
			{
				if (strcmp(vtSameLinkExit[e]->szRoadID, pstEntryGate->szRoadID) == 0)
				{
					pstSameLinkExit = vtSameLinkExit[e];
					break;
				}
			}

			POINT stLinkStart, stExitPos, stEntryPos;
			stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
			stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
			double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

			// 재발화 가드 — 같은 링크에 같은 road_id 출구 게이트가 **있을 때만** 성립한다
			double dfExitPosOnLink = -1.0;			// -1 = 같은 링크에 출구 게이트 없음(아래 로그 표기용)
			if (pstSameLinkExit != nullptr)
			{
				stExitPos.dfX = pstSameLinkExit->dfLon;
				stExitPos.dfY = pstSameLinkExit->dfLat;
				// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
				//   (2026-09-07 최정우 수정, 사용자 지적)
				dfExitPosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stExitPos.dfX, stExitPos.dfY);
				if (dfExitPosOnLink < 0.0)
					dfExitPosOnLink = HaversineMeters(stLinkStart, stExitPos);		// 형상 없음 — 종전 직선거리

				if (dfCurPosOnLink >= (dfExitPosOnLink - 3.0))
					continue;			// 이미 이 링크의 출구 지점을 지났음 — 재진입 아님
			}

			// [버그 수정, 2026-09-21 최정우] 아래 진입 애매 판정을 위 `pstSameLinkExit != nullptr`
			//   블록 **밖으로** 꺼냈다. 이 판정은 입구 게이트 위치와 현재 위치만 쓸 뿐 출구 게이트와
			//   아무 관계가 없는데(출구 위치는 로그 표기용일 뿐이다), 종전에는 그 블록 안에 들어 있어
			//   "입구 게이트 링크에 같은 구역 출구 게이트가 함께 있는 구역"에서만 동작했다.
			//   실측 기준정보(base_tollgate × base_roadlink) — road_kind 2·3 구역 7개 중 그 조건을
			//   만족하는 건 RL-Z00005(폐쇄식)·RL-Z00003(구간단속) 둘뿐이고, 나머지 5개
			//   (RL-Z00008·RL-Z00009 / RL-Z00006·RL-Z00012·RL-Z00013)에서는 판정이 아예 실행되지
			//   않았다. 그 5개 구역에서 트립이 "입구 링크 위·게이트를 이미 지난 지점"에서 시작하면
			//   게이트를 통과하는 장면을 한 번도 관측하지 않았는데도 정상 진입으로 확정돼, dist_m 에
			//   구역 전체 길이가 들어가고 charge_yn=Y/0 로 입구 게이트까지 귀속됐다.
			// 되돌리는 법: 아래 블록을 다시 위 if 안(재발화 가드 바로 뒤)으로 옮기면 종전 동작이다.
			// 세션에 확정된 직전 링크가 전혀 없는(=이 트립의 첫 매칭 좌표) 상태에서, 그 좌표가
			//   입구 게이트 지점을 이미 지나 있으면 — 이 트립에서 게이트를 통과하는 장면을 관측한
			//   적이 없다는 뜻이다. 입구 게이트ID를 이 게이트로 확정해 잘못 귀속시키지 말고
			//   비워둔다 — 아래 bGateAnomaly 로직이 자동으로 AUDIT(N/3) 처리해준다.
			//   판정 기준은 "첫 매칭 좌표가 게이트 좌표 이전이거나 같으면 진입, 이후면 미통과"이며
			//   거리 여유를 두지 않는다(사용자 지시, 2026-09-05 최정우 수정). qwLastConfirmedLinkID
			//   는 매칭 성공 tick 에서만 갱신되므로 미매칭(SKIP) 좌표는 이 판정에서 자동 제외된다.
			//   원래는 입·출구 중간지점을 넘었을 때만 미통과로 봤는데(2026-08-25), 그 기준은
			//   당시 실측 사례(000376_20260819140856 — 입구에서 343m, 출구까지 23m)만 겨우 잡는
			//   느슨한 값이라 구역 초입에서 시작한 트립을 정상 진입으로 오인했다. 실측
			//   000376_20260821095239: 첫 매칭이 입구 TG00012 로부터 35.3m 안쪽(구역 331.4m 의
			//   10.6%)이었는데 중간지점(165.7m)에 못 미쳐 통과로 오인 → dist_m 에 구역 전체
			//   331m 가 들어가고 체류시간은 실제 관측분 30초라, 평균속도가 34.5 대신 39.7km/h 로
			//   부풀려졌다(제한 20km/h 구간단속이라 위반 판정 기준값 자체가 틀어짐)
			stEntryPos.dfX = pstEntryGate->dfLon;
			stEntryPos.dfY = pstEntryGate->dfLat;
			// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
			//   (2026-09-07 최정우 수정, 사용자 지적)
			double dfEntryPosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stEntryPos.dfX, stEntryPos.dfY);
			if (dfEntryPosOnLink < 0.0)
				dfEntryPosOnLink = HaversineMeters(stLinkStart, stEntryPos);		// 형상 없음 — 종전 직선거리
			if ((pstSession->qwLastConfirmedLinkID == 0)
				&& (dfCurPosOnLink > dfEntryPosOnLink))
			{
				bAmbiguousStart = true;
				LOGFMTW("[#%02d] closed road entry ambiguous!device=[%s] trip_id=[%s] road=[%s] "
					"pos=[%.1fm] entry_gate_pos=[%.1fm] exit_gate_pos=[%.1fm] -> entry left blank",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					pstEntryGate->szRoadID, dfCurPosOnLink, dfEntryPosOnLink, dfExitPosOnLink);
			}
		}

		// [2026-09-22 최정우 추가 — 사용자 지시] **역방향 진입 차단.** 반대편 차선 오매칭으로
		//   주행하지 않은 구역에 요금이 붙는 것을 막는다(실측 근거·임계값은 IsZoneDirectionOpposite()
		//   주석 참고). **세션 상태를 하나라도 세팅하기 전에** 판정해야 한다 — 뒤에 두면 진입을
		//   거부해도 szClosedRoadId·dtEntryTime 같은 값이 오염된 채 남는다(첫 배선에서 실제로 그랬다).
		if (IsZoneDirectionOpposite(string(pstEntryGate->szRoadID), stRawLogInfo))
		{
			LOGFMTW("[#%02d] zone entry rejected(opposite direction)!device=[%s] trip_id=[%s] "
				"seq=[%u] road=[%s] heading=[%d] speed=[%.0f]km/h",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				pstEntryGate->szRoadID, static_cast<int>(stRawLogInfo.nAngle), stRawLogInfo.fSpeed);
			return;
		}

		pstSession->bInClosedRoad = true;
		pstSession->nClosedExitTicks = 0;		// 이탈 스트릭 초기화 (2026-09-21 최정우 추가)
		pstSession->bClosedEntryAmbiguous = bAmbiguousStart;
		if (bAmbiguousStart)
		{
			pstSession->szEntryTollgateId[0] = '\0';
			// 진입 게이트 자체가 없으니 "입구 게이트 링크의 from_node"라는 기존 의미가 성립하지
			//   않음 — 실제 시작 지점(이 트립이 관측된 첫 매칭 좌표)을 대신 담아 나중에 dist_m
			//   실거리 계산 기준으로 쓴다(사용자 지시, 2026-08-25 최정우 추가)
			pstSession->dfEntryFromLat = stMatchLinkInfo.dfMatchY;
			pstSession->dfEntryFromLon = stMatchLinkInfo.dfMatchX;
		}
		else
		{
			strncpy(pstSession->szEntryTollgateId, pstEntryGate->szTollgateID, sizeof(pstSession->szEntryTollgateId) - 1);
			pstSession->szEntryTollgateId[sizeof(pstSession->szEntryTollgateId) - 1] = '\0';
			pstSession->dfEntryFromLat = stMatchLinkInfo.dfStNodeY;
			pstSession->dfEntryFromLon = stMatchLinkInfo.dfStNodeX;
		}
		strncpy(pstSession->szClosedRoadId, pstEntryGate->szRoadID, sizeof(pstSession->szClosedRoadId) - 1);
		pstSession->szClosedRoadId[sizeof(pstSession->szClosedRoadId) - 1] = '\0';
		// 진입 게이트 통과 시각 보간 — 진입이 애매하지 않을 때만(실제 게이트 위치가 있어야 보간
		//   근거가 있음) 직전 확정 tick~이번 tick 사이에서 게이트를 지났을 시각을 추정한다. 직전
		//   tick 이 없으면(트립 첫 tick) 지금 tick 시각을 그대로 씀(사용자 지시, 2026-08-25 최정우 추가)
		if (!bAmbiguousStart && pstSession->bHasLastMatch)
		{
			pstSession->dtEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				pstEntryGate->dfLon, pstEntryGate->dfLat);
		}
		else
		{
			pstSession->dtEntryTime = stRawLogInfo.dtGPS;
		}
		// ProcessSpeedZoneCharge() 의 동일 보정 참고 (2026-09-06 최정우 추가, 사용자 지시)
		// 순번 공유는 **게이트가 직전 tick 매칭점과 사실상 같은 지점일 때만** 한다(2m — 게이트형
		//   진출 이월 bGateExitAtTick 과 동일 임계, 사용자 지시 "좌표가 동일할 때 순번이 동일할
		//   수 있음"). 종전엔 시각 조건(보간된 게이트 통과 시각 <= 직전 tick 시각)만 봤는데, GPS
		//   가 3초 간격이라 게이트가 직전 tick 에서 수 m 떨어져 있어도 초 단위로는 같은 값이 나와
		//   앞 레코드의 tick 을 가져다 썼다 — 실측 000376_20260819140532 폐쇄식 RL-Z00005:
		//   진입게이트 TG00007 이 seq57 매칭점(면제 링크 위)에서 3.12m 떨어져 있는데 57 을 공유해
		//   57~66 이 됐다. 구역 안에서 실제로 관측된 첫 tick 은 seq58(게이트에서 30.17m) 이므로
		//   정답은 58~66 이다 (2026-09-07 최정우 수정, 사용자 지적)
		// 되돌리는 법: 아래 HaversineMeters(...) <= 2.0 조건 한 줄을 지우면 종전 판정으로 복귀
		pstSession->dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
		POINT stEntryGateP, stPrevTickP;
		stEntryGateP.dfX = pstEntryGate->dfLon;        stEntryGateP.dfY = pstEntryGate->dfLat;
		stPrevTickP.dfX = pstSession->dfLastMatchX;    stPrevTickP.dfY = pstSession->dfLastMatchY;
		if (pstSession->bHasLastMatch && (pstSession->dwLastConfirmedLinkGpsSeq != 0)
			&& (pstSession->dtLastConfirmedLinkTime == pstSession->dtLastMatchGps)
			&& (pstSession->dtEntryTime <= pstSession->dtLastMatchGps)
			&& (HaversineMeters(stEntryGateP, stPrevTickP) <= 2.0))
		{
			pstSession->dwEntryGpsSeq = pstSession->dwLastConfirmedLinkGpsSeq;
		}
		// 게이트 미확인 이탈 시 실측 dist_m 산출용 — 진입 시점 위치부터 누적 시작 (2026-08-25 최정우 추가)
		pstSession->dfClosedLastX = stMatchLinkInfo.dfMatchX;
		pstSession->dfClosedLastY = stMatchLinkInfo.dfMatchY;
		pstSession->dwClosedLastGpsSeq = stRawLogInfo.dwSeqNo;
		// 누적 시작점은 **진입 게이트(구역 경계)** 다 — 진입 tick 매칭점부터 세면 게이트~그 tick
		//   구간이 통째로 빠진다. FROM_LAT/LON 은 이미 게이트(진입 링크 시작노드)를 쓰고 있어
		//   좌표와 거리의 기준이 어긋나 있었다. 실측 000376_20260819140532 폐쇄식 RL-Z00005:
		//   진입게이트 TG00007(링크 진행거리 0.0m) ~ 도착 seq66(325.4m) 인데 seq58(30.0m)부터
		//   세어 296m 로 30m 부족했다. 진출게이트를 통과하지 못한 채 마감되는 경우의 종점은
		//   사용자 지시대로 "도착 좌표"(마지막 확인 tick)이며 그건 종전과 같다.
		//   진입이 애매(bAmbiguousStart — 트립이 구역 안에서 시작해 게이트 통과 근거가 없음)하면
		//   겨냥할 게이트가 없으므로 종전대로 이번 tick 부터 센다
		//   (2026-09-07 최정우 수정, 사용자 지적)
		// 되돌리는 법: 아래 초기값을 0.0 으로 되돌리면 종전 산출로 복귀
		pstSession->dfClosedAccumDistM = 0.0;
		if (!bAmbiguousStart)
		{
			POINT stGateP, stCurP;
			stGateP.dfX = pstEntryGate->dfLon;         stGateP.dfY = pstEntryGate->dfLat;
			stCurP.dfX = stMatchLinkInfo.dfMatchX;     stCurP.dfY = stMatchLinkInfo.dfMatchY;
			pstSession->dfClosedAccumDistM = HaversineMeters(stGateP, stCurP);
		}
		// 진입 tick 의 링크 자체가 "구역 안에서 마지막으로 확인된 링크"의 첫 값 — 위 qwClosedLastZoneLinkID
		//   주석 참고(2026-08-25 최정우 추가)
		pstSession->qwClosedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
		pstSession->dtClosedLastZoneTime = stRawLogInfo.dtGPS;
		pstSession->dwClosedLastZoneGpsSeq = stRawLogInfo.dwSeqNo;
		// 구역 안 누적 기준점 초기화 (2026-09-15 최정우 추가) — ProcessSpeedZoneCharge() 동일 근거
		pstSession->dfClosedLastZoneX = stMatchLinkInfo.dfMatchX;
		pstSession->dfClosedLastZoneY = stMatchLinkInfo.dfMatchY;

		LOGFMTI("[#%02d] closed road entry!device=[%s] trip_id=[%s] gate=[%s] road=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			bAmbiguousStart ? "unknown" : pstEntryGate->szTollgateID, pstEntryGate->szRoadID);
		break;			// 한 tick 엔 하나만 진입
	}

	// ── 구역 중간 진입(입구게이트 미통과) ───────────────────────────────────────
	// [2026-09-23 최정우 추가, 사용자 지시] 위 루프는 **입구게이트가 달린 링크**에 매칭돼야만
	//   진입을 인정한다. 그래서 입구게이트를 거치지 않고 중간 교차로·램프로 폐쇄식 도로에 올라타면
	//   진입이 아예 성립하지 않아, 그 구간이 어떤 폐쇄형 행도 남기지 못한 채 일반도로로 흡수됐다
	//   (IsLinkNodeStepEligible() ③ 분기). 실측 000995_20260904162440 seq521~523(RL-Z00008 강문IC
	//   교차로 진입 71m), 000370_20260826143912 seq76~186(RL-Z00009 본선 **3,380m**) 등 8건.
	//
	// **기본 로직이 이미 이 상황을 상정하고 있다** — NCR_CLOSED_ENTRY_UNOBSERVED(21) "진입게이트
	//   미확인" 이 그것이고, 개방형(OPEN)은 같은 상황을 이렇게 처리한다: 구역 링크에 들어서면
	//   게이트와 무관하게 run 을 열고, 게이트 통과를 확인하지 못하면 charge_yn=N/charge_status=3
	//   (AUDIT=심사대상) + 사유코드로 남긴다. 완전 배제(SKIP=4)가 아니라 사람이 재확인하도록
	//   표시하는 것이 이 엔진의 확정 관례다(2026-08-13 사용자 지시로 4 에서 3 으로 정정한 이력).
	//   폐쇄형만 그 관례를 구현하지 못하고 있었다 — 코드 21 이 정의돼 있으면서도 실제로는
	//   "입구게이트 링크 위에서 게이트를 지나친 채 트립이 시작된 경우"(bAmbiguousStart)만
	//   도달할 수 있었다. 여기서 그 나머지 경로를 연다.
	//
	// 진입 정보는 bAmbiguousStart 와 **완전히 동일하게** 다룬다(게이트ID 공란 → 마감 시
	//   bEntryMissing → N/3 + 코드21). 새 판정을 만들지 않고 기존 분기에 얹는 것이 요점이다.
	if (!pstSession->bInClosedRoad)
	{
		char szMidZoneRoadId[20+1];
		szMidZoneRoadId[0] = '\0';
		if (m_stConfig.pcChargeDataLoader->GetClosedZoneRoadIdByLinkId(
				stMatchLinkInfo.qwLinkID, szMidZoneRoadId, sizeof(szMidZoneRoadId))
			&& (szMidZoneRoadId[0] != '\0'))
		{
			// 방금 이 링크에서 진출한 구역이면 재진입을 받지 않는다 — 위 게이트 진입 루프와 동일
			//   근거(정차 중 GPS 드리프트로 진출~재진입이 반복되는 것을 막는다)
			const bool bJustExitedHere = (pstSession->qwClosedExitedLinkID != 0)
				&& (stMatchLinkInfo.qwLinkID == pstSession->qwClosedExitedLinkID)
				&& (strcmp(szMidZoneRoadId, pstSession->szClosedExitedRoadId) == 0);
			// 역방향 진입 차단 — 반대편 차선 오매칭으로 주행하지 않은 구역에 행이 생기는 것을
			//   막는다(2026-09-22 게이트 진입에 넣은 것과 같은 판정·같은 이유)
			if (!bJustExitedHere && !IsZoneDirectionOpposite(string(szMidZoneRoadId), stRawLogInfo))
			{
				pstSession->bInClosedRoad = true;
				pstSession->nClosedExitTicks = 0;
				pstSession->bClosedEntryAmbiguous = true;		// 진입게이트 근거 없음
				pstSession->szEntryTollgateId[0] = '\0';
				strncpy(pstSession->szClosedRoadId, szMidZoneRoadId,
					sizeof(pstSession->szClosedRoadId) - 1);
				pstSession->szClosedRoadId[sizeof(pstSession->szClosedRoadId) - 1] = '\0';
				// 게이트가 없으므로 진입 시각·좌표는 "구역 안에서 실제로 관측된 첫 tick" 이다 —
				//   bAmbiguousStart 경로가 쓰는 기준과 동일하며, 보간할 게이트가 없으니 tick 값을
				//   그대로 쓴다. 거리도 이 지점부터 센다(게이트~tick 구간을 지어내지 않는다)
				pstSession->dtEntryTime = stRawLogInfo.dtGPS;
				pstSession->dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
				pstSession->dfEntryFromLat = stMatchLinkInfo.dfMatchY;
				pstSession->dfEntryFromLon = stMatchLinkInfo.dfMatchX;
				pstSession->dfClosedLastX = stMatchLinkInfo.dfMatchX;
				pstSession->dfClosedLastY = stMatchLinkInfo.dfMatchY;
				pstSession->dwClosedLastGpsSeq = stRawLogInfo.dwSeqNo;
				pstSession->dfClosedAccumDistM = 0.0;
				pstSession->qwClosedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
				pstSession->dtClosedLastZoneTime = stRawLogInfo.dtGPS;
				pstSession->dwClosedLastZoneGpsSeq = stRawLogInfo.dwSeqNo;
				pstSession->dfClosedLastZoneX = stMatchLinkInfo.dfMatchX;
				pstSession->dfClosedLastZoneY = stMatchLinkInfo.dfMatchY;

				LOGFMTW("[#%02d] closed road entry(mid-zone, no entry gate)!device=[%s] trip_id=[%s] "
					"seq=[%u] road=[%s] link=[%llu]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					szMidZoneRoadId, static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			}
		}
	}
}

/**
 * @brief 구간단속(SPEED) 입/출구 게이트 판정 (2026-08-12 최정우 추가, 2026-08-13 재작성)
 * @param[in] nThreadId 워커 스레드 ID (로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 신뢰 가능한 맵매칭 결과(호출측이 bMatched && !bUntrustedMatch 확인 후 호출)
 * @param[in,out] pstSession 배치 임시 세션 — 진입 상태(bInSpeedZone 등) 갱신. 폐쇄형과 별도
 *   독립 트랙(같은 도로 위에 겹쳐 동시 진행 가능)
 * @param[out] pvtChargeInserts 출구 통과 시 1행 적재
 * @return void
 * @remark
 *   - 이 링크에 입구(I) 게이트가 있고 아직 구간단속 구역에 안 들어가 있으면, 그 게이트 road_id 의
 *     구역이 road_kind='3'(구간단속)일 때만 진입 처리(폐쇄형 road_kind='2' 등 제외)
 *   - 진출 시 from_id=to_id=구역 road_id(게이트ID 아님, 실측 확인), from/to_lat·lon=구역
 *     coords 폴리라인의 첫/마지막 정점(ZONE_INFO.dfFirstLat/Lon·dfLastLat/Lon)
 *   - speed_kmh 는 순간속도가 아니라 "구역 실거리 ÷ 입구~출구 경과시간" 평균속도
 *   - speed_limit_kmh 는 구역 자체 등록값(base_roadlink.speed_limit_kmh) 사용
 *   - charge_yn/charge_status 는 기본 Y/0 — 다른 유형과 동일하게 처리(사용자 지시, 2026-08-13).
 *     원래는 위반 여부와 무관하게 항상 N/4 고정이었음(실측 2건 전부 위반(78/84>60)인데도 N/4로
 *     확인됨 — 통행료 파이프라인 비대상이라는 근거였음). 이번 변경으로 그 실측 선례와는 달라짐.
 *     TTL 만료·세션유실은 이후 AppendExpiredSpeedZoneCharge() 가 N/3(AUDIT) 으로 처리하도록
 *     구현됨 — 다른 유형과 동일 관례 (2026-08-30 최정우 주석 갱신)
 *   - 2026-08-13 재작성: 폐쇄형과 동일한 3가지 한계 대응(한 링크 내 동일방향 게이트 2개 이상,
 *     road_id 까지 비교하는 재진입 가드로 "같은 링크의 다른 구역" 통과 허용, gate_div='B' 겸용
 *     게이트 지원) — CollectGateCandidates() 재사용, 상세 근거는 ProcessClosedRoadCharge() 참고
 *
 * @remark **진출 시 게이트 지점 이월**(bHasGateExitCarry) — 2026-09-23 최정우 추가, 사용자 지시.
 *   개방형·폐쇄형·면제는 진출 확정 시 이 이월을 걸어 **진출게이트 ~ 다음 일반도로 첫 tick** 사이
 *   구간을 주행방향 경유 링크 길이로 채우는데(ApplyGateExitCarryDist), 구간단속만 이 처리가 빠져
 *   있어 그 구간이 어느 레코드에도 안 들어갔다(실측 000993 seq503→504 37.4m, 000992 seq20→21 31.4m).
 *   이월은 두 가지로 쓰인다:
 *     ① 다음 tick 이 일반도로면 — 그 run 의 진입 지점이 게이트(구역 경계)로 정정되고 거리가 더해진다.
 *     ② 다음 tick 이 **또 다른 과금유형**이면 — 이월은 폐기되는데(진입시각 역전 방지, 2026-09-11),
 *        폐기 직전에 그 사이 미등록 링크를 독립 일반도로 행으로 건져낸다
 *        (ProcessNodeStepCharge 의 "gate exit orphan span recovered").
 *   **구간단속 행 자체의 판정(위반 여부·게이트 조건에 따른 등록)은 이 변경과 무관하다** — 실측에서도
 *   구간단속 행수·거리가 완전히 불변이었다.
 *   ※ 구간단속은 위반 여부와 무관하게 같은 구간을 일반도로 미러 행으로도 등록하는데(아래
 *     stHeldSpeedMirrorRun 블록), 그 미러 run 은 진출 후에도 일반도로로 이어지는 경우가 있어
 *     이월 구간과 맞닿는다(실측 000993_20250903152139: 구간단속 408~415, 미러 408~416).
 *     **이중 계상 여부는 추정하지 말고 구간중복으로 검증할 것** — 2026-09-23 전체 재매칭에서
 *     같은 유형 구간중복(verify_charge.py Z3)은 기준선과 동일한 2건이었다.
*/
void CRawLogWorker::ProcessSpeedZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 같은 링크 재진입 재발화 차단 해제 — 진출했던 링크를 벗어나면 표시를 지운다.
	//   (세션 필드 qwSpeedExitedLinkID 주석 참고, 2026-09-21 최정우 추가)
	if ((pstSession->qwSpeedExitedLinkID != 0)
		&& (stMatchLinkInfo.qwLinkID != pstSession->qwSpeedExitedLinkID))
	{
		pstSession->qwSpeedExitedLinkID = 0;
		pstSession->szSpeedExitedRoadId[0] = '\0';
	}

	// 진출 처리 먼저 시도 — ProcessClosedRoadCharge() 와 동일 패턴 (2026-08-13 최정우 재작성)
	if (pstSession->bInSpeedZone)
	{
		PZONE_INFO pstTrackingZoneForUpdate = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szSpeedZoneRoadId);
		// [2026-09-15 최정우] 구역 조회 실패(운영 중 기준정보 재조회로 road_id 가 사라진 경우 등)는
		//   fail-open — "구역 안"으로 간주해 종전처럼 누적한다. fail-closed 로 두면 누적이 통째로
		//   멈춰 dist_m≈0·speed_kmh≈0 인 행으로 마감되는데, 이는 실측 누적이라도 남던 종전보다
		//   나쁘다. 판정 근거가 없을 때 과금을 0으로 만들지 않는다(정확도 우선 원칙).
		const bool bSpeedZoneKnown = (pstTrackingZoneForUpdate != nullptr);
		const bool bMatchedInZone = !bSpeedZoneKnown
			|| IsMatchedLinkInZone(pstTrackingZoneForUpdate, stMatchLinkInfo);
		if (!bSpeedZoneKnown)
		{
			LOGFMTW("[#%02d] speed zone info missing!device=[%s] trip_id=[%s] seq=[%u] road=[%s] "
				"-> 구역 판정 불가, 누적 계속(fail-open)",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRawLogInfo.dwSeqNo, pstSession->szSpeedZoneRoadId);
		}

		// 실시간 누적거리·마지막 위치 갱신 — ProcessClosedRoadCharge() 와 동일 근거·위치(게이트
		//   확정 여부와 무관하게 매 틱 항상 최신 유지) (2026-08-25 최정우 추가)
		// [버그 수정, 2026-09-11 최정우] 정지 중(bSameRawAndHeadingAsPrev) GPS 저주파 위치표류 제외 —
		//   ProcessClosedRoadCharge() 동일 수정과 같은 근거.
		// [버그 수정, 2026-09-15 최정우] **구역 안 tick 만 누적**한다(면제도로가 2026-08-30 에 받은
		//   수정과 동일 규칙). 종전에는 bInSpeedZone 이기만 하면 매칭 링크가 구역 소속인지와 무관하게
		//   더했는데, 게이트를 안 거치고 구역을 벗어나면(특히 구역 링크의 반대방향 짝 링크로 옮겨갈 때)
		//   bInSpeedZone 이 안 풀려 구역 밖 주행거리가 통째로 구간단속 거리로 쌓였다 — 실측
		//   000376_20260826160622(구역 31~103, 이후 139 tick 2,083m)·000370_20260826143912(3,143m).
		//   게다가 같은 구간을 일반도로 run 이 따로 기록해 이중 계상까지 났다(사용자 지적).
		//   [재수정, 2026-09-15 최정우] 처음엔 dfSpeedLastX/Y 를 구역 밖에서도 갱신해 "재진입 시
		//   구역 밖 이동분이 얹히는 것"을 막으려 했으나, 그러면 거리는 구역 안으로 잘리는데
		//   TO_LAT/LON 은 구역 밖을 가리켜 한 행 안에서 거리·좌표가 서로 다른 구간을 가리키게
		//   된다(소스 재검토에서 발견). 면제도로(ProcessExemptZoneCharge)가 이미 검증된 정답이라
		//   그 구조를 그대로 따른다 — **구역 안 위치(dfSpeedLastZoneX/Y)만 누적 기준점으로 쓰고,
		//   누적·위치·링크·시각·순번을 한 덩어리로 갱신**한다. 재진입 시 첫 구간에 이탈분이
		//   얹히는 건 면제도로도 동일하게 감수하는 트레이드오프다(그쪽은 regrace 로 흡수).
		//   dfSpeedLastX/Y 는 구역 소속과 무관한 "세션 마지막 매칭 위치"로 남겨 다른 소비처의
		//   기존 의미를 바꾸지 않는다.
		//   [재수정2, 2026-09-15 최정우] 정지틱 가드를 거리·위치에만 걸고 링크/시각/순번은
		//   구역 안이면 **무조건** 갱신한다 — 2026-09-11 에 ProcessOpenGateCharge() 에서 똑같이
		//   한 덩어리로 묶었다가 "정지 상태로 구역이 끝나면 stay_seconds 가 짧게 계산되고
		//   end_gps_seq 도 부정확"해지는 회귀를 냈던 그 배선 실수를 그대로 반복한 것이었다.
		if (bMatchedInZone)
		{
			if (!stMatchLinkInfo.bSameRawAndHeadingAsPrev)
			{
				POINT stPrevPos, stCurPos;
				stPrevPos.dfX = pstSession->dfSpeedLastZoneX;  stPrevPos.dfY = pstSession->dfSpeedLastZoneY;
				stCurPos.dfX = stMatchLinkInfo.dfMatchX;       stCurPos.dfY = stMatchLinkInfo.dfMatchY;
				// 구역 진입 직후 첫 tick 은 기준점이 아직 없다(0,0) — 그 상태로 haversine 을 돌리면
				//   지구 반대편 거리가 나온다. 진입 처리부가 채워주지만 방어적으로 한 번 더 확인한다.
				if ((pstSession->dfSpeedLastZoneX != 0.0) || (pstSession->dfSpeedLastZoneY != 0.0))
					pstSession->dfSpeedAccumDistM += HaversineMeters(stPrevPos, stCurPos);
				pstSession->dfSpeedLastZoneX = stMatchLinkInfo.dfMatchX;
				pstSession->dfSpeedLastZoneY = stMatchLinkInfo.dfMatchY;
				pstSession->dwSpeedLastGpsSeq = stRawLogInfo.dwSeqNo;
			}
			// 구역 안에서 마지막으로 확인된 링크/시각/순번 — 정지 여부와 무관하게 매 tick 갱신
			pstSession->qwSpeedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
			pstSession->dtSpeedLastZoneTime = stRawLogInfo.dtGPS;
			pstSession->dwSpeedLastZoneGpsSeq = stRawLogInfo.dwSeqNo;
		}

		// 세션 마지막 매칭 위치 — 구역 소속과 무관하게 매 tick 갱신(기존 의미 유지)
		if (!stMatchLinkInfo.bSameRawAndHeadingAsPrev)
		{
			pstSession->dfSpeedLastX = stMatchLinkInfo.dfMatchX;
			pstSession->dfSpeedLastY = stMatchLinkInfo.dfMatchY;
		}

		// 경유(이미 완전 통과) 링크들을 먼저 확인 — ProcessClosedRoadCharge() 와 동일 원리
		//   (2026-08-20 최정우 추가)
		PGATE_INFO pstExitGate = nullptr;
		bool bExitOnIntermediate = false;

		vector<PGATE_INFO> vtIntermediateExit;
		CollectGateCandidatesOnIntermediateLinks(m_stConfig.pcChargeDataLoader,
			stMatchLinkInfo.aqwPathLinkIDs, stMatchLinkInfo.nPathLinkCount, 'O', &vtIntermediateExit);
		for (size_t i = 0; i < vtIntermediateExit.size(); ++i)
		{
			if (strcmp(pstSession->szSpeedZoneRoadId, vtIntermediateExit[i]->szRoadID) == 0)
			{
				pstExitGate = vtIntermediateExit[i];
				bExitOnIntermediate = true;
				break;
			}
		}

		if (pstExitGate == nullptr)
		{
			vector<PGATE_INFO> vtExitCandidates;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtExitCandidates);

			for (size_t i = 0; i < vtExitCandidates.size(); ++i)
			{
				if (strcmp(pstSession->szSpeedZoneRoadId, vtExitCandidates[i]->szRoadID) == 0)
				{
					pstExitGate = vtExitCandidates[i];
					break;
				}
			}
		}

		// 직전 확정 링크(이번 틱에 이미 완전히 벗어난 링크)에 출구 게이트가 있었는데, 그 안에서
		//   GPS 틱이 게이트 위치 3m 이내로 못 들어와 못 잡은 경우 — 다음 틱이 아예 다른 링크로
		//   확정됐다는 것 자체가 그 링크를 끝까지 지나왔다는 뜻이므로 위치 판정 없이 확정한다.
		//   경유경로(aqwPathLinkIDs)로도 못 잡는 이유: 두 확정 링크가 그래프상 1-hop 인접이면
		//   재구성 경로가 생기지 않아(경유 링크 개념 자체가 없음) 직전 링크가 경로에 아예 안 실린다
		//   (실측 000370_20260824103155 RL-Z00003 — 출구 TG00013 이 링크 끝단(길이 331.7m 중
		//   310m 지점)에 있는데 마지막 GPS 는 그보다 21.6m 못 미친 289m 지점, 3초 뒤 다음 틱은
		//   이미 다음 링크로 확정 — 게이트를 "지나간 순간"을 찍은 GPS 가 그냥 없었을 뿐 실제로는
		//   통과함. 이걸 못 잡아 트립종료까지 세션이 안 닫히고 TTL 로 강제마감돼 stay_seconds 에
		//   이탈 이후 시간까지 섞여 들어감) (2026-08-24 최정우 추가)
		// 후보 링크 두 개를 순서대로 시도 — ProcessClosedRoadCharge() 동일 근거 참고. qwLastConfirmedLinkID
		//   가 트립 시작 후보 판정 중이라 0에 머물러 있으면 qwSpeedLastZoneLinkID 로 대체
		//   (2026-08-25 최정우 추가, 사용자 지시 — "다음 맵매칭 좌표/링크로 진출 확인 가능")
		bool bExitOnPrevLink = false;
		uint64 qwExitPrevLinkID = 0;
		time_t dtExitPrevLinkTime = 0;
		if (pstExitGate == nullptr)
		{
			const uint64 aqwPrevCand[2] = { pstSession->qwLastConfirmedLinkID, pstSession->qwSpeedLastZoneLinkID };
			const time_t adtPrevCandTime[2] = { pstSession->dtLastConfirmedLinkTime, pstSession->dtSpeedLastZoneTime };
			for (int c = 0; (c < 2) && (pstExitGate == nullptr); ++c)
			{
				if ((aqwPrevCand[c] == 0) || (aqwPrevCand[c] == stMatchLinkInfo.qwLinkID))
					continue;

				vector<PGATE_INFO> vtPrevExit;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, aqwPrevCand[c], 'O', &vtPrevExit);
				for (size_t i = 0; i < vtPrevExit.size(); ++i)
				{
					if (strcmp(pstSession->szSpeedZoneRoadId, vtPrevExit[i]->szRoadID) == 0)
					{
						pstExitGate = vtPrevExit[i];
						bExitOnPrevLink = true;
						qwExitPrevLinkID = aqwPrevCand[c];
						dtExitPrevLinkTime = adtPrevCandTime[c];
						break;
					}
				}
			}
		}
		// ④ 누락 링크 복구 경로에서 출구 게이트 찾기 — FindExitGateOnRecoveredPath() 주석 참고
		//   (2026-10-04 최정우 추가). 찾으면 "경유 링크 통과"와 같이 위치 비교 없이 확정한다.
		if (pstExitGate == nullptr)
		{
			pstExitGate = FindExitGateOnRecoveredPath(nThreadId, stRawLogInfo, stMatchLinkInfo,
				pstSession->qwSpeedLastZoneLinkID, pstSession->szSpeedZoneRoadId);
			if (pstExitGate != nullptr)
				bExitOnIntermediate = true;
		}

		if (pstExitGate != nullptr)
		{
			if (!bExitOnIntermediate && !bExitOnPrevLink)
			{
				POINT stLinkStart, stGatePos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stGatePos.dfX = pstExitGate->dfLon;
				stGatePos.dfY = pstExitGate->dfLat;
				// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
				//   (2026-09-07 최정우 수정, 사용자 지적)
				double dfGatePosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stGatePos.dfX, stGatePos.dfY);
				if (dfGatePosOnLink < 0.0)
					dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);		// 형상 없음 — 종전 직선거리
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				if (dfCurPosOnLink < (dfGatePosOnLink - 3.0))
					return;
			}

			// [2026-09-15 최정우] 같은 tick 에 이미 조회한 포인터를 재사용 — 중복 조회(뮤텍스 2회)를
			//   없애고, 두 조회 사이에 기준정보 캐시 세대가 바뀌어 서로 다른 세대를 보는 일도 막는다
			PZONE_INFO pstZone = pstTrackingZoneForUpdate;

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "3";							// SPEED
			stRow.strChargeUnit = "1";							// LINK (실측 확인)
			stRow.strLinkId = "";

			// from_id/to_id — 입/출구 게이트ID (2026-08-20 최정우 수정, 사용자 지시 — 기존엔 구역
			//   road_id를 넣었으나(2026-08-12 실측 확인 기반) 게이트ID 자체로 변경. 구역 road_id는
			//   zone_id 컬럼에 계속 남음
			stRow.strFromId = pstSession->szSpeedEntryTollgateId;
			stRow.strToId = pstExitGate->szTollgateID;

			// 진입 애매(bSpeedEntryAmbiguous — 트립이 이미 구간단속 구역 위에서 시작해 진입 게이트를
			//   못 잡은 경우)했으면 from_lat/lon·dist_m 모두 "구역 전체를 지났다"는 전제인 구역
			//   등록값(dfFirstLat/Lon, dfLengthM) 대신 실제 출발 지점 기준으로 계산한다(사용자 지시,
			//   2026-08-25 최정우 추가) — ProcessClosedRoadCharge() bClosedEntryAmbiguous 동일 근거
			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			double dfLengthM;
			if (pstSession->bSpeedEntryAmbiguous)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfSpeedEntryFromLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfSpeedEntryFromLon);
				snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);

				POINT stStartPos, stGatePos;
				stStartPos.dfX = pstSession->dfSpeedEntryFromLon;  stStartPos.dfY = pstSession->dfSpeedEntryFromLat;
				stGatePos.dfX = pstExitGate->dfLon;                stGatePos.dfY = pstExitGate->dfLat;
				dfLengthM = CalcZoneSpanM(pstZone, stStartPos.dfX, stStartPos.dfY, stGatePos.dfX, stGatePos.dfY,
					HaversineMeters(stStartPos, stGatePos));		// 2026-10-04: 직선 -> 구역 선을 따라
			}
			else if (pstZone != nullptr)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZone->dfFirstLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZone->dfFirstLon);
				snprintf(szToLat, sizeof(szToLat), "%.06lf", pstZone->dfLastLat);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", pstZone->dfLastLon);
				// 2026-10-04 최정우 수정(사용자 확정): 구역 전체 길이 -> 진입·진출 게이트 사이 선 길이.
				//   이 값은 아래 일반도로 미러 거리에도 그대로 쓰인다
				PGATE_INFO pstEntryGateForLen = (m_stConfig.pcChargeDataLoader != nullptr)
					? m_stConfig.pcChargeDataLoader->GetGateByTollgateId(pstSession->szSpeedEntryTollgateId) : nullptr;
				dfLengthM = (pstEntryGateForLen != nullptr)
					? CalcZoneSpanM(pstZone, pstEntryGateForLen->dfLon, pstEntryGateForLen->dfLat,
						pstExitGate->dfLon, pstExitGate->dfLat, pstZone->dfLengthM)
					: pstZone->dfLengthM;
			}
			else
			{
				szFromLat[0] = szFromLon[0] = szToLat[0] = szToLon[0] = '\0';
				dfLengthM = 0.0;
			}
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szSpeedZoneRoadId;
			stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
			stRow.strDistM = szDistM;

			// 출구 게이트 통과 시각 보간 — ProcessClosedRoadCharge() 동일 근거·방식 참고
			//   (2026-08-25 최정우 추가). 직전 tick 이 없으면 예전 방식 그대로(2026-08-24 최정우
			//   추가, dtExitPrevLinkTime 로 일반화 2026-08-25)
			time_t dtExitTime;
			if (pstSession->bHasLastMatch)
			{
				dtExitTime = InterpolateGateCrossingTime(
					pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
					pstExitGate->dfLon, pstExitGate->dfLat);
			}
			else
			{
				dtExitTime = bExitOnPrevLink ? dtExitPrevLinkTime : stRawLogInfo.dtGPS;
			}
			// end_gps_seq — ProcessClosedRoadCharge() 동일 근거 (2026-08-28 최정우 추가)
			uint32 dwExitGpsSeq = bExitOnPrevLink ? pstSession->dwSpeedLastZoneGpsSeq : stRawLogInfo.dwSeqNo;
			(void)qwExitPrevLinkID;		// 구간단속은 제한속도를 구역 등록값으로만 쓰므로 링크ID 자체는 미사용

			double dfElapsedSec = difftime(dtExitTime, pstSession->dtSpeedEntryTime);
			// 주행은 실제로 있었으므로 경과시간이 0(또는 반올림 시 0이 되는 소수)으로 남으면 안 됨 —
			//   최소 1초로 보정(사용자 지시, 2026-08-25 최정우 추가)
			if (dfElapsedSec < 1.0)
				dfElapsedSec = 1.0;

			// 평균속도 — 구역 실거리 ÷ 입구~출구 경과시간. 구간단속은 순간속도가 아니라 구간 전체
			//   평균속도가 제한속도 초과 여부(위반 판정) 기준이기 때문 (2026-08-12 최정우 추가)
			{
				double dfAvgSpeedKmh = (dfLengthM / dfElapsedSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}

			// 제한속도 — 구역 자체 등록값(base_roadlink.speed_limit_kmh, 실측 확인) 사용. 개방형·폐쇄형은
			//   매칭 링크의 값을 썼지만, 구간단속은 구역 전체 제한속도가 별도 등록돼 있어 이쪽을 씀 (2026-08-12 최정우 추가)
			if (pstZone != nullptr)
			{
				char szSpeedLimit[16];					// (2026-09-15 최정우 수정) 8 → 16: 8 이면 7자리 초과 값이 잘려
											//   엉뚱한 수가 된다(제한속도는 실무상 3자리지만 기준정보 오류 시
											//   큰 값이 들어올 수 있다). [2026-09-17 정정] 종전 주석의 "클램프
											//   WARN 로그" 는 이 함수들에 존재하지 않는 내용이라 삭제
				snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZone->dfSpeedLimitKmh + 0.5));
				stRow.strSpeedLimitKmh = szSpeedLimit;
			}

			// stay_seconds — 입구~출구 체류시간(초), 사용자 지시(2026-08-14 — 개방형 제외 전 유형 공통화).
			//   위 dfElapsedSec(평균속도 계산에 이미 씀)과 동일한 경과시간 재사용
			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			char szStartGpsSeq[16], szEndGpsSeq[16];
			snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", pstSession->dwSpeedEntryGpsSeq);
			snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", dwExitGpsSeq);
			stRow.strStartGpsSeq = szStartGpsSeq;
			stRow.strEndGpsSeq = szEndGpsSeq;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtSpeedEntryTime);

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			if (pszTripStartDt != nullptr)
				stRow.strTripStartDt = pszTripStartDt;
			else
				stRow.strTripStartDt = stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szSpeedEntryTollgateId;	// 2026-08-20 최정우 수정 — 게이트ID 기록으로 변경
			stRow.strExitTollgateId = pstExitGate->szTollgateID;

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			// charge_yn/charge_status — 다른 유형과 동일하게 기본 Y/0 (사용자 지시, 2026-08-13 —
			//   원래는 통행료 파이프라인 비대상이라 위반 여부 무관 항상 N/4 고정이었음, 실측 선례와 달라짐).
			//   단, 입구 게이트를 확정 못 한(진입 판정이 애매해 비워둔) 경우는 CLOSED_ROAD와 동일하게
			//   AUDIT(N/3) — "다른 유형과 동일하게"라는 원 지시 취지에 맞춤(2026-08-25 최정우 추가,
			//   ProcessClosedRoadCharge() bGateAnomaly 참고)
			const bool bEntryMissing = (pstSession->szSpeedEntryTollgateId[0] == '\0');
			const bool bExitMissing = (pstExitGate->szTollgateID[0] == '\0');
			const bool bEntryEqualsExit = !bEntryMissing && !bExitMissing
				&& (strcmp(pstSession->szSpeedEntryTollgateId, pstExitGate->szTollgateID) == 0);
			bool bGateAnomaly = bEntryMissing || bExitMissing || bEntryEqualsExit;
			stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
			stRow.strChargeStatus = bGateAnomaly ? "3" : "0";
			if (bGateAnomaly)
			{
				// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — CLOSED_ROAD와
				//   동일 근거·우선순위(입구 미확인→출구 미확인→입구==출구)
				const int nNonChargeReason = bEntryMissing ? NCR_SPEED_ENTRY_UNOBSERVED
					: bExitMissing ? NCR_SPEED_EXIT_UNCONFIRMED : NCR_SPEED_ENTRY_EQUALS_EXIT;
				char szReason[8];
				snprintf(szReason, sizeof(szReason), "%d", nNonChargeReason);
				stRow.strNonChargeReason = szReason;

				LOGFMTW("[#%02d] speed zone gate anomaly!device=[%s] trip_id=[%s] seq=[%d] entry=[%s] "
					"exit=[%s] -> charge_yn=N non_charge_reason=[%d:%s]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
					pstSession->szSpeedEntryTollgateId, pstExitGate->szTollgateID, nNonChargeReason,
					m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nNonChargeReason));
			}

			// NODE_STEP 일반도로 확장(케이스1) — 평균속도 vs 제한속도로 위반 여부 직접 판정.
			//   제한속도를 모르면(pstZone==nullptr 이거나 등록값 0) 판정 불가라 기존 그대로 SPEED만
			//   등록(사용자 확정 스펙은 "제한속도 이상/이내" 전제라 이 경우는 범위 밖) (2026-09-01 최정우 추가)
			double dfAvgSpeedKmhForJudge = (dfLengthM / dfElapsedSec) * 3.6;
			bool bSpeedLimitKnown = (pstZone != nullptr) && (pstZone->dfSpeedLimitKmh > 0.0);
			bool bViolated = bSpeedLimitKnown && (dfAvgSpeedKmhForJudge >= pstZone->dfSpeedLimitKmh);

			// 적재 조건 (2026-09-06 최정우 수정, 사용자 지시) — 아래를 모두 충족해야 레코드를 만든다.
			//   · 진입 게이트 통과 확인
			//   · 진출 게이트 통과 확인
			//   · 평균속도 >= 제한속도 (위반)
			//   하나라도 어긋나면 **레코드 자체를 만들지 않는다**. 종전에는 게이트 이상 시
			//   charge_yn=N/status=3(AUDIT)로 남겼으나, 구간단속은 게이트 통과가 확인돼야 성립하는
			//   유형이라 미통과 건을 심사 큐에 올릴 근거가 없다는 판단이다. 그 구간은 일반도로가
			//   관통해서 이어진다. 실측 000376_20260819140856 trip_seq=3 — 진입 TG00012 는 있으나
			//   진출게이트가 없는데도 171m 가 구간단속으로 적재됐다.
			//   제한속도 미등록(bSpeedLimitKnown=false)은 위반 여부를 판정할 수 없어 종전대로
			//   적재를 유지한다 — 데이터 결함이지 비위반이 아니므로 조용히 버리면 안 된다.
			if (!bGateAnomaly && (!bSpeedLimitKnown || bViolated))
			{
				pvtChargeInserts->push_back(stRow);
				LOGFMTI("[#%02d] speed zone exit charge queued!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
					"dist_m=[%s] avg_speed=[%s] non_charge_reason=[%d:%s]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
					pstSession->szSpeedZoneRoadId, szDistM, stRow.strSpeedKmh.c_str(),
					NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
				pstSession->nChargeSeq += 1;
			}
			if (bSpeedLimitKnown)
			{
				// FROM/TO 는 LINK_ID(구역 road_id/게이트ID 아님) — 위반 시 SPEED 와 별도로 추가,
				//   비위반 시 이것만 등록. 누적거리·평균속도·체류시간은 SPEED 와 동일 값 재사용,
				//   from/to 좌표도 동일(위 szFromLat/Lon 등 계산과 같은 소스)
				double dfFromLatRaw = pstSession->bSpeedEntryAmbiguous ? pstSession->dfSpeedEntryFromLat
					: ((pstZone != nullptr) ? pstZone->dfFirstLat : 0.0);
				double dfFromLonRaw = pstSession->bSpeedEntryAmbiguous ? pstSession->dfSpeedEntryFromLon
					: ((pstZone != nullptr) ? pstZone->dfFirstLon : 0.0);

				// zone_id/zone_name — 일반도로 레코드는 과금유형 등록 여부를 무시하므로 구간단속
				//   구역 road_id 를 넣지 않는다(사용자 지시, 2026-09-01 최정우 추가 — BuildNodeStepRow
				//   와 동일 근거)
				// TO 링크 — stMatchLinkInfo.qwLinkID(이번 확정 tick의 링크)가 아니라
				//   qwSpeedLastZoneLinkID(구역 안에서 마지막으로 확인된 링크)를 써야 한다. 출구
				//   게이트를 GPS가 3m 이내로 못 잡아 "다음 틱이 이미 다른 링크로 확정"되는
				//   대체판정(bExitOnPrevLink)일 때 stMatchLinkInfo 는 이미 구역 밖 링크라 TO_ID가
				//   틀어진다(실측 000376_20260826155015 RL-Z00013 출구 — TG00029 링크 2520196805 대신
				//   구역 밖 2520196803 이 찍힘). dwExitGpsSeq 가 이미 이 값으로 갈라 쓰는 것과 동일
				//   근거(2026-09-01 최정우 수정)
				// 곧바로 등록하지 않고 보류한다 — 이 구역단속 마지막 링크 바로 다음이 누락 링크를
				//   거쳐 주정차 폴리곤과 곧장 만나는 경우, ProcessNodeStepCharge() 의 인수인계 구간
				//   로직이 이 보류분을 그대로 이어받아 하나의 레코드로 합친다. 접촉이 없으면(또는
				//   인수인계 탐색이 실패하면) 그 로직이 원래 값 그대로 등록한다(사용자 지시,
				//   2026-09-03 최정우 추가 — trip_seq 를 별도로 쪼개지 않고 82~93 + 인수인계 구간을
				//   하나의 일반도로 레코드로)
				pstSession->stHeldSpeedMirrorRun.dtEntryTime = pstSession->dtSpeedEntryTime;
				pstSession->stHeldSpeedMirrorRun.dwEntryGpsSeq = pstSession->dwSpeedEntryGpsSeq;
				pstSession->stHeldSpeedMirrorRun.dfEntryX = dfFromLonRaw;
				pstSession->stHeldSpeedMirrorRun.dfEntryY = dfFromLatRaw;
				pstSession->stHeldSpeedMirrorRun.qwEntryLinkID = pstSession->qwSpeedEntryLinkID;
				pstSession->stHeldSpeedMirrorRun.dfAccumDistM = dfLengthM;
				pstSession->stHeldSpeedMirrorRun.qwLastLinkID = pstSession->qwSpeedLastZoneLinkID;
				pstSession->stHeldSpeedMirrorRun.dfLastX = pstExitGate->dfLon;
				pstSession->stHeldSpeedMirrorRun.dfLastY = pstExitGate->dfLat;
				pstSession->stHeldSpeedMirrorRun.dtLastInZoneTime = dtExitTime;
				pstSession->stHeldSpeedMirrorRun.dwLastInZoneGpsSeq = dwExitGpsSeq;
				pstSession->bHasHeldSpeedMirrorRun = true;
				// 보류한 tick 을 남겨, 같은 tick 안에서 곧바로 등록되지 않게 한다 — 아래
				//   ProcessNodeStepCharge() 의 "접촉 없음" 분기가 이 tick 에서 바로 실행되면
				//   다음 tick 의 폴리곤 접촉이 이어받을 미러가 사라진다 (2026-09-06 최정우 추가)
				pstSession->dwHeldSpeedMirrorSeq = stRawLogInfo.dwSeqNo;

				LOGFMTI("[#%02d] node step (from speed zone %s) held for handoff merge!device=[%s] "
					"trip_id=[%s] road=[%s] dist_m=[%s] avg_speed=[%s] non_charge_reason=[%d:%s]", nThreadId,
					bViolated ? "violation" : "non-violation",
					stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					pstSession->szSpeedZoneRoadId, szDistM, stRow.strSpeedKmh.c_str(),
					NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
			}

			// [2026-09-23 최정우 추가, 사용자 지시] 진출게이트 지점 이월 — 폐쇄형·개방형·면제는
			//   진출 시 이 이월을 걸어 **게이트~다음 일반도로 첫 tick 사이 구간**을 주행방향 경유
			//   링크 길이로 채우는데(ApplyGateExitCarryDist), 구간단속만 이 처리가 없어 그 구간이
			//   어느 레코드에도 안 들어갔다(실측 000993 seq503→504 37.4m, 000992 seq20→21 31.4m).
			//   폐쇄형 CheckClosedRoadExitByRawGps() 주석이 지적한 것과 **같은 종류의 누락**이다.
			//   이월은 다음 tick 이 또 다른 과금유형이면 폐기되는데, 그때 그 사이 미등록 링크를
			//   일반도로 행으로 건져내는 경로(ProcessNodeStepCharge 의 "gate exit orphan span
			//   recovered")도 이 이월이 있어야 도달한다 — 구간단속 진출 직후 다른 구역에 진입하는
			//   경우의 누락 링크가 그 경로로 복구된다.
			//   **구간단속 행 자체의 판정(위반 여부·게이트 조건에 따른 등록)은 건드리지 않는다.**
			pstSession->bHasGateExitCarry = true;
			pstSession->dfGateExitX = pstExitGate->dfLon;
			pstSession->dfGateExitY = pstExitGate->dfLat;
			pstSession->dtGateExit = dtExitTime;
			pstSession->dwGateExitGpsSeq = dwExitGpsSeq;
			pstSession->qwGateExitLinkID = pstExitGate->qwLinkID;
			{
				POINT stGateP, stTickP;
				stGateP.dfX = pstExitGate->dfLon;          stGateP.dfY = pstExitGate->dfLat;
				stTickP.dfX = pstSession->dfSpeedLastX;    stTickP.dfY = pstSession->dfSpeedLastY;
				pstSession->bGateExitAtTick = (HaversineMeters(stGateP, stTickP) <= 2.0);
			}

			// 이 링크에서 진출했음을 표시 — 정차 중 GPS 드리프트로 같은 링크에서 재진입이
			//   재발화하는 것을 막는다(세션 필드 주석 참고, 2026-09-21 최정우 추가)
			pstSession->qwSpeedExitedLinkID = stMatchLinkInfo.qwLinkID;
			strncpy(pstSession->szSpeedExitedRoadId, pstSession->szSpeedZoneRoadId,
				sizeof(pstSession->szSpeedExitedRoadId) - 1);
			pstSession->szSpeedExitedRoadId[sizeof(pstSession->szSpeedExitedRoadId) - 1] = '\0';
			pstSession->bInSpeedZone = false;
			pstSession->szSpeedZoneRoadId[0] = '\0';
			pstSession->szSpeedEntryTollgateId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 (2026-08-13 최정우 추가, 폐쇄형과 동일 이유)
		}
		else
		{
			// 게이트를 못 찾음 — ProcessClosedRoadCharge() 동일 로직·근거 참고: 이 구역(link_ids)을
			//   완전히 벗어났는지로 "확정 이탈" 판정(실측 누적거리·위치는 위에서 이미 매 틱 갱신됨).
			//   아직 구역 안이면 계속 대기, 벗어났으면 게이트 미확인 AUDIT(N/3)이지만 dist_m/
			//   speed_kmh/stay_seconds는 실측값으로 채운다 (2026-08-25 최정우 추가)
			// [2026-09-15 최정우] 같은 tick 에 이미 조회한 포인터를 재사용 — 중복 조회(뮤텍스 2회)를
			//   없애고, 두 조회 사이에 기준정보 캐시 세대가 바뀌어 서로 다른 세대를 보는 일도 막는다
			PZONE_INFO pstTrackingZone = pstTrackingZoneForUpdate;

			if (IsMatchedLinkInZone(pstTrackingZone, stMatchLinkInfo))
			{
				pstSession->nSpeedExitTicks = 0;	// 구역 안 확인 — 이탈 스트릭 리셋 (2026-09-21 최정우 추가)
				return;			// 아직 구역 안 — 다음 틱 대기
			}

			// 여러 링크 구역은 일시적 이탈로 확정 짓지 않음 — ProcessClosedRoadCharge() 동일 근거
			//   참고(2026-08-25 최정우 추가, 전체 재맵매칭 회귀검증 중 RL-Z00006 에서 발견)
			// [버그 수정, 2026-09-21 최정우 — 이슈 36] 이탈 디바운스(config zone_exitcnt) 적용.
			//   상세 근거·실측 사례는 ProcessClosedRoadCharge() 의 동일 블록 주석 참고 —
			//   실측 사례 000984_20250903153702 가 바로 이 구간단속 경로에서 나왔다.
			// 되돌리는 법: config zone_exitcnt=0
			if ((pstTrackingZone != nullptr) && (pstTrackingZone->vtLinkIds.size() > 1))
			{
				pstSession->nSpeedExitTicks += 1;
				if ((m_stConfig.nZoneExitCnt <= 0)
					|| (pstSession->nSpeedExitTicks < m_stConfig.nZoneExitCnt))
					return;		// 아직 일시적 이탈일 수 있음 — 확정 짓지 않고 대기

				LOGFMTI("[#%02d] speed zone exit debounced!device=[%s] trip_id=[%s] seq=[%u] "
					"road=[%s] out_ticks=[%d/%d] -> 확정 이탈",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					pstSession->szSpeedZoneRoadId, pstSession->nSpeedExitTicks, m_stConfig.nZoneExitCnt);
			}

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "3";							// SPEED
			stRow.strChargeUnit = "1";
			stRow.strLinkId = "";
			stRow.strFromId = pstSession->szSpeedEntryTollgateId;
			stRow.strToId = "";								// 출구 미확인 — 지어내지 않음

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			// [버그 수정, 2026-09-10 최정우] 게이트 확정 진출 경로(위 bSpeedEntryAmbiguous 분기,
			// ProcessClosedRoadCharge() bClosedEntryAmbiguous 와 동일 근거)는 진입이 애매(트립이
			// 구역 중간에서 시작)했으면 FROM을 구역 등록 시작점 대신 실제 관측 시작점으로 쓰는데,
			// 이 "출구 미확인" 분기는 그 처리가 빠져 있었다 — DIST_M(아래 dfSpeedAccumDistM, 실제
			// 관측 시작점 기준 실측 누적거리)은 이미 맞는데 FROM 좌표만 구역 전체 시작점이라, 두
			// 값이 서로 다른 구간을 가리키는 모순 데이터가 됐다.
			if (pstSession->bSpeedEntryAmbiguous)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfSpeedEntryFromLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfSpeedEntryFromLon);
			}
			else if (pstTrackingZone != nullptr)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstTrackingZone->dfFirstLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstTrackingZone->dfFirstLon);
			}
			else
			{
				szFromLat[0] = szFromLon[0] = '\0';
			}
			// [2026-09-15 최정우 추가] 이탈 경계 보정 — **아래 bViolated2(평균속도 vs 제한속도)
			//   판정보다 반드시 먼저** 적용해야 한다. 경계 구간이 빠진 짧은 거리로 평균속도를
			//   내면 위반이 비위반으로 뒤집혀 SPEED 행 자체가 등록되지 않는다.
			// [버그 수정, 2026-09-15 최정우 — 오후 재검토] TO 좌표보다도 앞으로 옮겼다(bViolated2
			//   보다 먼저라는 조건은 그대로 유지된다). 근거는 아래 TO 주석·CLOSED 동일 수정 참고.
			ApplyZoneExitTailDist(pstSession->qwSpeedLastZoneLinkID,
				&pstSession->dfSpeedLastZoneX, &pstSession->dfSpeedLastZoneY,
				&pstSession->dfSpeedAccumDistM);

			// [버그 수정, 2026-09-15 최정우] TO 는 구역 안 마지막 위치 + 이탈 경계 보정(= 구역
			//   경계). 종전엔 이번 tick 매칭 좌표라, 바로 아래에서 같은 tick 이 만드는 NODE_STEP
			//   미러 행(dfSpeedLastZoneY/X 사용)과 **두 행이 서로 다른 지점**을 가리켰다.
			//   CLOSED 동일 수정 참고. 구역 안 tick 이 없었으면 종전 동작으로 폴백.
			const bool bSpeedZonePosKnown = ((pstSession->dfSpeedLastZoneX != 0.0)
				|| (pstSession->dfSpeedLastZoneY != 0.0));
			snprintf(szToLat, sizeof(szToLat), "%.06lf",
				bSpeedZonePosKnown ? pstSession->dfSpeedLastZoneY : stMatchLinkInfo.dfMatchY);
			snprintf(szToLon, sizeof(szToLon), "%.06lf",
				bSpeedZonePosKnown ? pstSession->dfSpeedLastZoneX : stMatchLinkInfo.dfMatchX);
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szSpeedZoneRoadId;
			stRow.strZoneName = (pstTrackingZone != nullptr) ? pstTrackingZone->szRoadNm : "";

			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(pstSession->dfSpeedAccumDistM + 0.5));
			stRow.strDistM = szDistM;

			double dfElapsedSec = difftime(stRawLogInfo.dtGPS, pstSession->dtSpeedEntryTime);
			if (dfElapsedSec > 0.0)
			{
				double dfAvgSpeedKmh = (pstSession->dfSpeedAccumDistM / dfElapsedSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}
			if (pstTrackingZone != nullptr)
			{
				char szSpeedLimit[16];					// (2026-09-15 최정우 수정) 8 → 16: 8 이면 7자리 초과 값이 잘려
											//   엉뚱한 수가 된다(제한속도는 실무상 3자리지만 기준정보 오류 시
											//   큰 값이 들어올 수 있다). [2026-09-17 정정] 종전 주석의 "클램프
											//   WARN 로그" 는 이 함수들에 존재하지 않는 내용이라 삭제
				snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstTrackingZone->dfSpeedLimitKmh + 0.5));
				stRow.strSpeedLimitKmh = szSpeedLimit;
			}

			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			char szStartGpsSeq[16], szEndGpsSeq[16];
			snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", pstSession->dwSpeedEntryGpsSeq);
			snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", stRawLogInfo.dwSeqNo);
			stRow.strStartGpsSeq = szStartGpsSeq;
			stRow.strEndGpsSeq = szEndGpsSeq;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtSpeedEntryTime);

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szSpeedEntryTollgateId;
			stRow.strExitTollgateId = "";

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			stRow.strChargeYn = "N";							// 출구 게이트 미확인 — 항상 AUDIT
			stRow.strChargeStatus = "3";
			// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — 판정(N/3)은 안 건드림.
			//   ProcessClosedRoadCharge() 의 동일 패턴(출구 미확인, 구역 확정 이탈)과 같은 사유
			{
				char szReason[8];
				snprintf(szReason, sizeof(szReason), "%d", NCR_SPEED_EXIT_UNCONFIRMED);
				stRow.strNonChargeReason = szReason;
			}

			// NODE_STEP 일반도로 확장(케이스1) — 위 게이트확정 이탈과 동일 근거로 위반 여부를
			//   먼저 판정해 SPEED row 를 등록할지(위반/미상) NODE_STEP만 등록할지(비위반) 가른다.
			//   TO_ID/to_lat·lon 은 SPEED row(out-of-zone 위치)와 달리 qwSpeedLastZoneLinkID/
			//   dfSpeedLastX·Y(구역 안에서 마지막으로 확인된 링크·위치)를 씀 — "같은 구간" 범위를
			//   구역 밖으로 넘기지 않기 위함 (2026-09-01 최정우 추가)
			bool bSpeedLimitKnown2 = (pstTrackingZone != nullptr) && (pstTrackingZone->dfSpeedLimitKmh > 0.0);
			bool bViolated2 = bSpeedLimitKnown2 && (dfElapsedSec > 0.0)
				&& (((pstSession->dfSpeedAccumDistM / dfElapsedSec) * 3.6) >= pstTrackingZone->dfSpeedLimitKmh);

			// [정책 정합, 2026-10-04 최정우 — 사용자 지시(권장안)] **출구 미확인이면 SPEED 행을 만들지 않는다.**
			//   2026-09-06 적재 정책("진입·진출 게이트 모두 확인 + 평균속도 >= 제한속도일 때만 SPEED 행")과
			//   맞춘다 — 종전에는 이 분기만 위반·제한속도 미등록이면 N/3(33) SPEED 행을 적재했다(게이트 확정
			//   경로·SKIP tick 원시좌표 경로·TTL 마감은 이미 미적재). 이 분기에 오기 전에 누락 링크 복구로
			//   출구 게이트를 한 번 더 찾는다(FindExitGateOnRecoveredPath). 그래도 못 찾은 구간은
			//   **일반도로 미러(Y/0)** 로만 남기며, 제한속도가 미등록이어도 미러는 만든다 — 구간단속 run 이
			//   열려 있는 동안 일반도로가 억제되므로 미러가 없으면 그 구간이 어느 행에도 안 남는다.
			//   발생 건수: 2026-10-04 기준 실주행·시나리오 0건(결과 무변화).
			// 되돌리는 법: 종전 블록(if (!bSpeedLimitKnown2 || bViolated2) { push_back(stRow) ... }) 복원,
			//   아래 미러 조건을 if (bSpeedLimitKnown2) 로.
			LOGFMTW("[#%02d] speed zone exit unconfirmed(zone left) -> SPEED row not registered (mirror only)!"
				"device=[%s] trip_id=[%s] seq=[%u] entry=[%s] road=[%s] dist_m=[%s] violated=[%d] limit_known=[%d]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				pstSession->szSpeedEntryTollgateId, pstSession->szSpeedZoneRoadId, szDistM,
				bViolated2 ? 1 : 0, bSpeedLimitKnown2 ? 1 : 0);
			(void)stRow;
			if (pstTrackingZone != nullptr)		// 미러는 구역 등록 좌표를 쓴다 — 기준정보 재조회로 구역이 사라졌으면 생략
			{
				CHARGE_INSERT_ROW stNodeStepRow;
				// zone_id/zone_name 미사용 — 위 게이트확정 이탈 경로와 동일 근거(2026-09-01 최정우 추가)
				BuildNodeStepRowFromLinkRange(stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
					pstSession->nChargeSeq, pstSession->qwSpeedEntryLinkID, pstSession->qwSpeedLastZoneLinkID,
					pstTrackingZone->dfFirstLat, pstTrackingZone->dfFirstLon,
					pstSession->dfSpeedLastZoneY, pstSession->dfSpeedLastZoneX,	// (2026-09-15 최정우 수정)
					pstSession->dfSpeedAccumDistM, pstSession->dtSpeedEntryTime, stRawLogInfo.dtGPS,
					pstSession->dwSpeedEntryGpsSeq, pstSession->dwSpeedLastZoneGpsSeq, "Y", "0",
					nullptr, nullptr, &stNodeStepRow);
				pvtChargeInserts->push_back(stNodeStepRow);

				LOGFMTI("[#%02d] node step (from speed zone %s, unconfirmed exit)!device=[%s] trip_id=[%s] "
					"seq=[%d] road=[%s] dist_m=[%s] non_charge_reason=[%d:%s]", nThreadId,
					bViolated2 ? "violation" : "non-violation",
					stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
					pstSession->szSpeedZoneRoadId, szDistM,
					NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
				pstSession->nChargeSeq += 1;
			}

			// 이 링크에서 진출했음을 표시 — 정차 중 GPS 드리프트로 같은 링크에서 재진입이
			//   재발화하는 것을 막는다(세션 필드 주석 참고, 2026-09-21 최정우 추가)
			pstSession->qwSpeedExitedLinkID = stMatchLinkInfo.qwLinkID;
			strncpy(pstSession->szSpeedExitedRoadId, pstSession->szSpeedZoneRoadId,
				sizeof(pstSession->szSpeedExitedRoadId) - 1);
			pstSession->szSpeedExitedRoadId[sizeof(pstSession->szSpeedExitedRoadId) - 1] = '\0';
			pstSession->bInSpeedZone = false;
			pstSession->szSpeedZoneRoadId[0] = '\0';
			pstSession->szSpeedEntryTollgateId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 — 게이트 확정 이탈과 동일 관례
		}
	}

	// 진입 처리 — 위 블록에서 못 닫혔으면(다른 구역에 여전히 진입 중) 새 진입 안 받음 (2026-08-13 최정우 재작성)
	if (pstSession->bInSpeedZone)
		return;

	// 경유 링크 포함 전체 경로에서 입구 후보 수집 (2026-08-20 최정우 추가, 폐쇄형과 동일 원리)
	vector<PGATE_INFO> vtEntryCandidates;
	{
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
		{
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'I', &vtEntryCandidates);
		}
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
			{
				vector<PGATE_INFO> vtOne;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.aqwPathLinkIDs[i], 'I', &vtOne);
				vtEntryCandidates.insert(vtEntryCandidates.end(), vtOne.begin(), vtOne.end());
			}
		}
	}

	for (size_t i = 0; i < vtEntryCandidates.size(); ++i)
	{
		PGATE_INFO pstEntryGate = vtEntryCandidates[i];
		bool bAmbiguousStart = false;			// ProcessClosedRoadCharge() 동일 로직 참고 (2026-08-25 최정우 추가)

		// 이 링크에서 방금 진출한 구역이면 재진입을 받지 않는다 — 정차 중 GPS 드리프트가
		//   링크 진행거리를 출구 임계 안팎으로 오가게 만들어 진출~재진입이 반복되는 것을
		//   막는다. 링크를 벗어나면 위 함수 진입부에서 해제되므로 다음 통행은 정상 진입한다
		//   (2026-09-21 최정우 추가 — 실측 000994_20250903152350 RL-Z00003 398km/h)
		if ((pstSession->qwSpeedExitedLinkID != 0)
			&& (stMatchLinkInfo.qwLinkID == pstSession->qwSpeedExitedLinkID)
			&& (strcmp(pstEntryGate->szRoadID, pstSession->szSpeedExitedRoadId) == 0))
			continue;
		// [2026-09-21 최정우 수정] 이 구역이 내가 다룰 유형인지부터 확인한다 — 종전에는 이 검사가
		//   아래 위치 판정 **뒤**에 있어, 다른 유형 구역(예: 폐쇄식 RL-Z00008)의 게이트 후보에도
		//   "speed zone entry ambiguous" WARN 이 먼저 찍힌 뒤 여기서 조용히 버려졌다. 판정이 5개
		//   구역으로 확대되면서 그 교차 노이즈가 트립당 여러 줄로 늘어 장애 분석을 방해한다.
		//   동작은 동일하다 — 어느 쪽이든 이 후보는 continue 로 건너뛴다.
		PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstEntryGate->szRoadID);
		if ((pstZone == nullptr) || (strcmp(pstZone->szRoadKind, "3") != 0))
			continue;

		// 이번 확정(최종) 링크에서 찾은 후보만 위치 검사 — ProcessClosedRoadCharge() 와 동일 원리
		//   (2026-08-20 최정우 수정, 상세 근거는 그 함수의 동일 블록 주석 참고)
		if (pstEntryGate->qwLinkID == stMatchLinkInfo.qwLinkID)
		{
			vector<PGATE_INFO> vtSameLinkExit;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtSameLinkExit);

			PGATE_INFO pstSameLinkExit = nullptr;
			for (size_t e = 0; e < vtSameLinkExit.size(); ++e)
			{
				if (strcmp(vtSameLinkExit[e]->szRoadID, pstEntryGate->szRoadID) == 0)
				{
					pstSameLinkExit = vtSameLinkExit[e];
					break;
				}
			}

			POINT stLinkStart, stExitPos, stEntryPos;
			stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
			stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
			double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

			// 재발화 가드 — 같은 링크에 같은 road_id 출구 게이트가 **있을 때만** 성립한다
			double dfExitPosOnLink = -1.0;			// -1 = 같은 링크에 출구 게이트 없음(아래 로그 표기용)
			if (pstSameLinkExit != nullptr)
			{
				stExitPos.dfX = pstSameLinkExit->dfLon;
				stExitPos.dfY = pstSameLinkExit->dfLat;
				// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
				//   (2026-09-07 최정우 수정, 사용자 지적)
				dfExitPosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stExitPos.dfX, stExitPos.dfY);
				if (dfExitPosOnLink < 0.0)
					dfExitPosOnLink = HaversineMeters(stLinkStart, stExitPos);		// 형상 없음 — 종전 직선거리

				if (dfCurPosOnLink >= (dfExitPosOnLink - 3.0))
					continue;			// 이미 이 링크의 출구 지점을 지났음 — 재진입 아님
			}

			// [버그 수정, 2026-09-21 최정우] 아래 진입 애매 판정을 위 `pstSameLinkExit != nullptr`
			//   블록 **밖으로** 꺼냈다. 이 판정은 입구 게이트 위치와 현재 위치만 쓸 뿐 출구 게이트와
			//   아무 관계가 없는데(출구 위치는 로그 표기용일 뿐이다), 종전에는 그 블록 안에 들어 있어
			//   "입구 게이트 링크에 같은 구역 출구 게이트가 함께 있는 구역"에서만 동작했다.
			//   실측 기준정보(base_tollgate × base_roadlink) — road_kind 2·3 구역 7개 중 그 조건을
			//   만족하는 건 RL-Z00005(폐쇄식)·RL-Z00003(구간단속) 둘뿐이고, 나머지 5개
			//   (RL-Z00008·RL-Z00009 / RL-Z00006·RL-Z00012·RL-Z00013)에서는 판정이 아예 실행되지
			//   않았다. 그 5개 구역에서 트립이 "입구 링크 위·게이트를 이미 지난 지점"에서 시작하면
			//   게이트를 통과하는 장면을 한 번도 관측하지 않았는데도 정상 진입으로 확정돼, dist_m 에
			//   구역 전체 길이가 들어가고 charge_yn=Y/0 로 입구 게이트까지 귀속됐다.
			// 되돌리는 법: 아래 블록을 다시 위 if 안(재발화 가드 바로 뒤)으로 옮기면 종전 동작이다.
			// ProcessClosedRoadCharge() 동일 로직·근거 참고 (2026-08-25 최정우 추가,
			//   2026-09-05 최정우 수정 — 중간지점 기준을 "게이트 지점 초과"로 강화, 사용자 지시)
			stEntryPos.dfX = pstEntryGate->dfLon;
			stEntryPos.dfY = pstEntryGate->dfLat;
			// 게이트 진행거리는 직선이 아니라 링크 폴리라인을 따라 잰다 — GatePosOnLink() 주석 참고
			//   (2026-09-07 최정우 수정, 사용자 지적)
			double dfEntryPosOnLink = GatePosOnLink(stMatchLinkInfo.qwLinkID, stEntryPos.dfX, stEntryPos.dfY);
			if (dfEntryPosOnLink < 0.0)
				dfEntryPosOnLink = HaversineMeters(stLinkStart, stEntryPos);		// 형상 없음 — 종전 직선거리
			if ((pstSession->qwLastConfirmedLinkID == 0)
				&& (dfCurPosOnLink > dfEntryPosOnLink))
			{
				bAmbiguousStart = true;
				LOGFMTW("[#%02d] speed zone entry ambiguous!device=[%s] trip_id=[%s] road=[%s] "
					"pos=[%.1fm] entry_gate_pos=[%.1fm] exit_gate_pos=[%.1fm] -> entry left blank",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					pstEntryGate->szRoadID, dfCurPosOnLink, dfEntryPosOnLink, dfExitPosOnLink);
			}
		}

		// [2026-09-22 최정우 추가 — 사용자 지시] **역방향 진입 차단.** 반대편 차선 오매칭으로
		//   주행하지 않은 구역에 요금이 붙는 것을 막는다(실측 근거·임계값은 IsZoneDirectionOpposite()
		//   주석 참고). **세션 상태를 하나라도 세팅하기 전에** 판정해야 한다 — 뒤에 두면 진입을
		//   거부해도 szClosedRoadId·dtEntryTime 같은 값이 오염된 채 남는다(첫 배선에서 실제로 그랬다).
		if (IsZoneDirectionOpposite(string(pstEntryGate->szRoadID), stRawLogInfo))
		{
			LOGFMTW("[#%02d] zone entry rejected(opposite direction)!device=[%s] trip_id=[%s] "
				"seq=[%u] road=[%s] heading=[%d] speed=[%.0f]km/h",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				pstEntryGate->szRoadID, static_cast<int>(stRawLogInfo.nAngle), stRawLogInfo.fSpeed);
			return;
		}

		pstSession->bInSpeedZone = true;
		pstSession->nSpeedExitTicks = 0;		// 이탈 스트릭 초기화 (2026-09-21 최정우 추가)
		pstSession->bSpeedEntryAmbiguous = bAmbiguousStart;
		// NODE_STEP 일반도로 확장(케이스1) FROM_ID(링크ID) 용 — 게이트ID(szSpeedEntryTollgateId)와
		//   별개로 항상 기록(진입 애매 여부 무관, 애매해도 이 링크가 실제 진입 링크임) (2026-09-01 최정우 추가)
		pstSession->qwSpeedEntryLinkID = stMatchLinkInfo.qwLinkID;
		if (bAmbiguousStart)
		{
			pstSession->szSpeedEntryTollgateId[0] = '\0';
			// ProcessClosedRoadCharge() bClosedEntryAmbiguous 동일 근거 참고 — 실제 시작 지점을
			//   담아 나중에 dist_m 실거리 계산 기준으로 씀(2026-08-25 최정우 추가)
			pstSession->dfSpeedEntryFromLat = stMatchLinkInfo.dfMatchY;
			pstSession->dfSpeedEntryFromLon = stMatchLinkInfo.dfMatchX;
		}
		else
		{
			strncpy(pstSession->szSpeedEntryTollgateId, pstEntryGate->szTollgateID, sizeof(pstSession->szSpeedEntryTollgateId) - 1);
			pstSession->szSpeedEntryTollgateId[sizeof(pstSession->szSpeedEntryTollgateId) - 1] = '\0';
		}
		strncpy(pstSession->szSpeedZoneRoadId, pstEntryGate->szRoadID, sizeof(pstSession->szSpeedZoneRoadId) - 1);
		pstSession->szSpeedZoneRoadId[sizeof(pstSession->szSpeedZoneRoadId) - 1] = '\0';
		// ProcessClosedRoadCharge() 동일 근거 참고 — 진입 게이트 통과 시각 보간 (2026-08-25 최정우 추가)
		if (!bAmbiguousStart && pstSession->bHasLastMatch)
		{
			pstSession->dtSpeedEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				pstEntryGate->dfLon, pstEntryGate->dfLat);
		}
		else
		{
			pstSession->dtSpeedEntryTime = stRawLogInfo.dtGPS;
		}
		// 보간된 진입 시각이 직전 매칭 tick 의 시각과 같거나 그보다 앞이면, 게이트를 지난 시점이 곧
		//   그 tick 이므로 START_GPS_SEQ 도 이번 tick 이 아니라 **그 tick** 으로 잡는다. 게이트 좌표가
		//   링크 경계에 놓여 직전 tick 의 매칭좌표와 사실상 일치하는 경우가 그렇다 — 실측
		//   000376_20260821094609: 진입게이트 TG00012 와 seq54 매칭좌표의 거리가 0.14m 이고, 보간된
		//   진입 시각도 seq54 시각(094930)과 같은데 START_GPS_SEQ 만 seq55 로 기록돼 stay_seconds(33초,
		//   seq54~65)와 구간 표기가 어긋났다. 이때 직전 일반도로 레코드의 END_GPS_SEQ 와 이 레코드의
		//   START_GPS_SEQ 가 같은 번호가 되는데, **게이트 지점을 두 레코드가 공유하는 것이므로 정상**
		//   이다(사용자 지시, 2026-09-06 최정우 추가). dtLastConfirmedLinkTime == dtLastMatchGps 를
		//   함께 확인해 두 값이 같은 tick 것임을 보장한다(트립 시작 모호구간에서는 앵커가 갱신되지
		//   않아 서로 다른 tick 을 가리킬 수 있음).
		// 순번 공유는 **게이트가 직전 tick 매칭점과 사실상 같은 지점일 때만** 한다(2m — 게이트형
		//   진출 이월 bGateExitAtTick 과 동일 임계, 사용자 지시 "좌표가 동일할 때 순번이 동일할
		//   수 있음"). 종전엔 시각 조건(보간된 게이트 통과 시각 <= 직전 tick 시각)만 봤는데, GPS
		//   가 3초 간격이라 게이트가 직전 tick 에서 수 m 떨어져 있어도 초 단위로는 같은 값이 나와
		//   앞 레코드의 tick 을 가져다 썼다 — 실측 000376_20260819140532 폐쇄식 RL-Z00005:
		//   진입게이트 TG00007 이 seq57 매칭점(면제 링크 위)에서 3.12m 떨어져 있는데 57 을 공유해
		//   57~66 이 됐다. 구역 안에서 실제로 관측된 첫 tick 은 seq58(게이트에서 30.17m) 이므로
		//   정답은 58~66 이다 (2026-09-07 최정우 수정, 사용자 지적)
		// 되돌리는 법: 아래 HaversineMeters(...) <= 2.0 조건 한 줄을 지우면 종전 판정으로 복귀
		pstSession->dwSpeedEntryGpsSeq = stRawLogInfo.dwSeqNo;
		POINT stEntryGateP, stPrevTickP;
		stEntryGateP.dfX = pstEntryGate->dfLon;        stEntryGateP.dfY = pstEntryGate->dfLat;
		stPrevTickP.dfX = pstSession->dfLastMatchX;    stPrevTickP.dfY = pstSession->dfLastMatchY;
		if (pstSession->bHasLastMatch && (pstSession->dwLastConfirmedLinkGpsSeq != 0)
			&& (pstSession->dtLastConfirmedLinkTime == pstSession->dtLastMatchGps)
			&& (pstSession->dtSpeedEntryTime <= pstSession->dtLastMatchGps)
			&& (HaversineMeters(stEntryGateP, stPrevTickP) <= 2.0))
		{
			pstSession->dwSpeedEntryGpsSeq = pstSession->dwLastConfirmedLinkGpsSeq;
		}
		// 게이트 미확인 이탈 시 실측 dist_m 산출용 (2026-08-25 최정우 추가, ProcessClosedRoadCharge() 동일)
		pstSession->dfSpeedLastX = stMatchLinkInfo.dfMatchX;
		pstSession->dfSpeedLastY = stMatchLinkInfo.dfMatchY;
		pstSession->dwSpeedLastGpsSeq = stRawLogInfo.dwSeqNo;
		// 누적 시작점은 **진입 게이트(구역 경계)** 다 — 진입 tick 매칭점부터 세면 게이트~그 tick
		//   구간이 통째로 빠진다. FROM_LAT/LON 은 이미 게이트(진입 링크 시작노드)를 쓰고 있어
		//   좌표와 거리의 기준이 어긋나 있었다. 실측 000376_20260819140532 폐쇄식 RL-Z00005:
		//   진입게이트 TG00007(링크 진행거리 0.0m) ~ 도착 seq66(325.4m) 인데 seq58(30.0m)부터
		//   세어 296m 로 30m 부족했다. 진출게이트를 통과하지 못한 채 마감되는 경우의 종점은
		//   사용자 지시대로 "도착 좌표"(마지막 확인 tick)이며 그건 종전과 같다.
		//   진입이 애매(bAmbiguousStart — 트립이 구역 안에서 시작해 게이트 통과 근거가 없음)하면
		//   겨냥할 게이트가 없으므로 종전대로 이번 tick 부터 센다
		//   (2026-09-07 최정우 수정, 사용자 지적)
		// 되돌리는 법: 아래 초기값을 0.0 으로 되돌리면 종전 산출로 복귀
		pstSession->dfSpeedAccumDistM = 0.0;
		if (!bAmbiguousStart)
		{
			POINT stGateP, stCurP;
			stGateP.dfX = pstEntryGate->dfLon;         stGateP.dfY = pstEntryGate->dfLat;
			stCurP.dfX = stMatchLinkInfo.dfMatchX;     stCurP.dfY = stMatchLinkInfo.dfMatchY;
			pstSession->dfSpeedAccumDistM = HaversineMeters(stGateP, stCurP);
		}
		// ProcessClosedRoadCharge() 동일 근거 참고 (2026-08-25 최정우 추가)
		pstSession->qwSpeedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
		pstSession->dtSpeedLastZoneTime = stRawLogInfo.dtGPS;
		pstSession->dwSpeedLastZoneGpsSeq = stRawLogInfo.dwSeqNo;
		// 구역 안 누적 기준점 초기화 (2026-09-15 최정우 추가) — 이걸 안 채우면 다음 tick 의
		//   haversine 이 (0,0) 과의 거리를 재서 지구 반대편 값이 나온다
		pstSession->dfSpeedLastZoneX = stMatchLinkInfo.dfMatchX;
		pstSession->dfSpeedLastZoneY = stMatchLinkInfo.dfMatchY;

		LOGFMTI("[#%02d] speed zone entry!device=[%s] trip_id=[%s] gate=[%s] road=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			bAmbiguousStart ? "unknown" : pstEntryGate->szTollgateID, pstEntryGate->szRoadID);
		break;			// 한 tick 엔 하나만 진입
	}

	// [2026-09-23 최정우 — 시도했다가 **원복**. 같은 방식을 다시 제안하지 말 것]
	//   폐쇄형과 똑같이 "구역 중간 진입(진입게이트 미통과)" 으로 run 을 여는 블록을 여기에 넣었다가
	//   되돌렸다. 목적은 실측 000995_20260904162440 seq629~691 — RL-Z00012 의 **진출게이트 TG00027
	//   이 달린 링크** 2520197600 으로 올라타(진입게이트 TG00026 은 링크 2520215000) 구간단속이
	//   전혀 기록되지 않고 일반도로로 흡수되던 것이다. 구조는 폐쇄형 RL-Z00008 seq521~523 과 같다.
	//
	//   **왜 안 되는가 — 미러 정책과 충돌해 거리가 이중 계상된다.**
	//   구간단속은 폐쇄형과 마감 규칙이 다르다: 게이트 이상(bGateAnomaly)이면 **SPEED 행 자체를
	//   적재하지 않고**(위 `!bGateAnomaly && ...` 조건), 제한속도만 알면 위반 여부와 무관하게
	//   일반도로 미러 행은 그대로 만든다. 그래서 중간 진입으로 run 만 열리면
	//     · SPEED 행은 안 생기고(기본 로직 그대로),
	//     · 미러 행이 새로 생기는데 그 구간은 이미 일반도로 run 이 실측으로 세고 있던 곳이라
	//       같은 구간이 두 번 들어간다.
	//   실측(2026-09-23 전체 재매칭): 일반도로 +737m, 000995 순번6 이 9,572 -> 10,982m(**+1,410m**).
	//   병합(MergeAdjacentNodeStepRows)이 미러를 인접 일반도로 행에 흡수해 버려서 **같은 유형
	//   구간중복(Z3) 검사에는 잡히지도 않았다** — 행수·Z3 는 그대로인데 거리만 늘었다.
	//   거리 증가를 "복구" 로 오인하지 말 것.
	//
	//   고치려면 미러 정책까지 같이 설계해야 한다(셋 중 하나를 골라야 한다):
	//     ① 게이트 근거 없는 run 은 SPEED 행을 N/3 + 코드31 로 **적재**하고 미러는 만들지 않는다
	//        (폐쇄형과 같은 형태. 구간단속의 "게이트 이상이면 미적재" 정책을 바꾸는 것이다)
	//     ② 미러만 만들되 그 구간을 일반도로 run 에서 확실히 빼낸다(IsLinkNodeStepEligible 연동)
	//     ③ 현행 유지 — 게이트를 못 지난 구간단속 구역은 일반도로로만 청구한다
	//   **③ 으로 확정(2026-10-02 사용자 결정).** 구간단속은 정상 통과 때도 일반도로 미러를 만드는
	//   것이 정책이라 일반도로 청구 자체는 틀리지 않고, 진입게이트 시각이 없으면 평균속도(위반 판정)
	//   근거가 없다. ①·② 는 사용자가 정책을 바꾸라고 할 때만 재착수할 것.
	//   링크→구간단속 구역 조회가 필요하면
	//   CChargeDataLoader::GetSpeedZoneRoadIdByLinkId() 가 남아 있다.
}

/**
 * @brief 폐쇄형 — SKIP 틱(맵매칭 실패) raw GPS 기준 출구 판정
 * @remark ProcessClosedRoadCharge() 의 확정매칭 전이 기반 출구판정으로도 못 잡는 잔여 케이스 보완 —
 *   구역 시작점 기준 raw GPS 거리가 출구 게이트 거리보다 MM_RAWGPS_EXIT_MARGIN_M 이상 크면
 *   확정 링크 없이도 출구로 확정한다. 확정매칭이 있었을 때 쓰는 stMatchLinkInfo 기반 값(매칭
 *   링크 종점 좌표·제한속도 등)이 없어, 그 대신 구역 등록값(ZONE_INFO)으로 대체한다
 *   (2026-08-24 최정우 추가, 사용자 지시)
*/
void CRawLogWorker::CheckClosedRoadExitByRawGps(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if (!pstSession->bInClosedRoad || (m_stConfig.pcChargeDataLoader == nullptr)
		|| m_stConfig.strChargeInsertSQL.empty() || stRawLogInfo.bGpsLatNull || stRawLogInfo.bGpsLonNull)
		return;

	PGATE_INFO pstExitGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(pstSession->szClosedRoadId, 'O');
	if (pstExitGate == nullptr)
		return;
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szClosedRoadId);
	if (pstZone == nullptr)
		return;

	POINT stZoneStart, stGatePos, stRaw;
	stZoneStart.dfX = pstZone->dfFirstLon;  stZoneStart.dfY = pstZone->dfFirstLat;
	stGatePos.dfX = pstExitGate->dfLon;     stGatePos.dfY = pstExitGate->dfLat;
	stRaw.dfX = stRawLogInfo.dfX;           stRaw.dfY = stRawLogInfo.dfY;

	double dfGateFromStart = HaversineMeters(stZoneStart, stGatePos);
	double dfRawFromStart = HaversineMeters(stZoneStart, stRaw);
	if (dfRawFromStart < (dfGateFromStart + MM_RAWGPS_EXIT_MARGIN_M))
		return;

	CHARGE_INSERT_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "2";								// CLOSED_ROAD
	stRow.strChargeUnit = "1";
	stRow.strLinkId = "";

	stRow.strFromId = pstSession->szEntryTollgateId;
	stRow.strToId = pstExitGate->szTollgateID;

	const bool bEntryMissing = (pstSession->szEntryTollgateId[0] == '\0');
	const bool bExitMissing = (pstExitGate->szTollgateID[0] == '\0');
	const bool bEntryEqualsExit = !bEntryMissing && !bExitMissing
		&& (strcmp(pstSession->szEntryTollgateId, pstExitGate->szTollgateID) == 0);
	bool bGateAnomaly = bEntryMissing || bExitMissing || bEntryEqualsExit;
	stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
	stRow.strChargeStatus = bGateAnomaly ? "3" : "0";
	if (bGateAnomaly)
	{
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — ProcessClosedRoadCharge()
		//   와 동일 근거·우선순위(입구 미확인→출구 미확인→입구==출구)
		const int nNonChargeReason = bEntryMissing ? NCR_CLOSED_ENTRY_UNOBSERVED
			: bExitMissing ? NCR_CLOSED_EXIT_UNCONFIRMED : NCR_CLOSED_ENTRY_EQUALS_EXIT;
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", nNonChargeReason);
		stRow.strNonChargeReason = szReason;
		// 로그는 아래 push_back 직후 한 곳에서 Y/N 공통 형식으로 남긴다(중복 로그 방지)
	}

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfEntryFromLat);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfEntryFromLon);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);
	stRow.strFromLat = szFromLat;
	stRow.strFromLon = szFromLon;
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = pstSession->szClosedRoadId;
	stRow.strZoneName = pstZone->szRoadNm;

	// 진입 자체가 애매(bClosedEntryAmbiguous)했으면 "구역 전체를 다 지났다"는 전제가 성립하지
	//   않으므로 실제 출발지점~출구 게이트 실거리를 쓴다 — ProcessClosedRoadCharge()의 정상매칭
	//   경로(2026-08-25)와 동일 처리를 raw GPS 기준 SKIP 이탈 경로에도 적용(2026-09-11 최정우 추가)
	double dfLengthM;
	if (pstSession->bClosedEntryAmbiguous)
	{
		POINT stStartPos, stGatePos2;
		stStartPos.dfX = pstSession->dfEntryFromLon;  stStartPos.dfY = pstSession->dfEntryFromLat;
		stGatePos2.dfX = pstExitGate->dfLon;          stGatePos2.dfY = pstExitGate->dfLat;
		dfLengthM = CalcZoneSpanM(pstZone, stStartPos.dfX, stStartPos.dfY, stGatePos2.dfX, stGatePos2.dfY,
			HaversineMeters(stStartPos, stGatePos2));		// 2026-10-04: 직선 -> 구역 선을 따라
	}
	else
	{
		// 2026-10-04 최정우 수정(사용자 확정): 구역 전체 길이 -> 진입·진출 게이트 사이 선 길이(CalcZoneSpanM)
		PGATE_INFO pstEntryGateForLen = (m_stConfig.pcChargeDataLoader != nullptr)
			? m_stConfig.pcChargeDataLoader->GetGateByTollgateId(pstSession->szEntryTollgateId) : nullptr;
		dfLengthM = (pstEntryGateForLen != nullptr)
			? CalcZoneSpanM(pstZone, pstEntryGateForLen->dfLon, pstEntryGateForLen->dfLat,
				pstExitGate->dfLon, pstExitGate->dfLat, pstZone->dfLengthM)
			: pstZone->dfLengthM;
	}
	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
	stRow.strDistM = szDistM;

	double dfDwellSec = difftime(stRawLogInfo.dtGPS, pstSession->dtEntryTime);
	if (dfDwellSec > 0.0)
	{
		double dfAvgSpeedKmh = (dfLengthM / dfDwellSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}

	char szSpeedLimit[16];					// (2026-09-15 최정우 수정) 8 → 16: 8 이면 7자리 초과 값이 잘려
											//   엉뚱한 수가 된다(제한속도는 실무상 3자리지만 기준정보 오류 시
											//   큰 값이 들어올 수 있다). [2026-09-17 정정] 종전 주석의 "클램프
											//   WARN 로그" 는 이 함수들에 존재하지 않는 내용이라 삭제
	snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZone->dfSpeedLimitKmh + 0.5));
	stRow.strSpeedLimitKmh = szSpeedLimit;

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", pstSession->dwEntryGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", stRawLogInfo.dwSeqNo);
	stRow.strStartGpsSeq = szStartGpsSeq;
	stRow.strEndGpsSeq = szEndGpsSeq;

	stRow.strOccurDt = FormatDateTime14(pstSession->dtEntryTime);

	const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
	stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = pstSession->szEntryTollgateId;
	stRow.strExitTollgateId = pstExitGate->szTollgateID;

	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;

	pvtChargeInserts->push_back(stRow);

	if (stRow.strNonChargeReason.empty())
	{
		LOGFMTI("[#%02d] closed road exit charge queued (raw gps)!device=[%s] trip_id=[%s] seq=[%d] "
			"entry=[%s] exit=[%s] dist_m=[%s] margin_over=[%.1f]m non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			pstSession->szEntryTollgateId, pstExitGate->szTollgateID, szDistM,
			dfRawFromStart - dfGateFromStart,
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
	}
	else
	{
		const int nNonChargeReason = atoi(stRow.strNonChargeReason.c_str());
		LOGFMTW("[#%02d] closed road exit charge queued(AUDIT, raw gps)!device=[%s] trip_id=[%s] seq=[%d] "
			"entry=[%s] exit=[%s] dist_m=[%s] margin_over=[%.1f]m non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			pstSession->szEntryTollgateId, pstExitGate->szTollgateID, szDistM,
			dfRawFromStart - dfGateFromStart, nNonChargeReason,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nNonChargeReason));
	}

	pstSession->nChargeSeq += 1;

	// [버그 수정, 2026-09-10 최정우] 정상 매칭 진출 경로(ProcessClosedRoadCharge, 2026-09-06 추가)는
	// 여기서 게이트 진출 지점을 이월해(bHasGateExitCarry) 다음 NODE_STEP run이 게이트 경계부터
	// 시작하게 하는데, 이 SKIP 틱 경로엔 그 처리가 빠져 있었다 — 그 결과 여기로 마감되는 통행은
	// 다음 일반도로 run 진입점이 게이트가 아니라 "다음 확정 매칭 tick 좌표"가 되어, 게이트~그
	// tick 사이 실제 구간(실측 사례 기준 약 20m대)이 어느 레코드에도 안 들어가고 누락됐다(최소
	// 재현으로 확인). dfClosedLastX/Y(마지막 확인 매칭 위치, ProcessClosedRoadCharge가 매 틱
	// 갱신)를 정상 경로와 동일하게 "게이트와 일치하는 tick인지" 판정 기준으로 재사용한다.
	pstSession->bHasGateExitCarry = true;
	pstSession->dfGateExitX = pstExitGate->dfLon;
	pstSession->dfGateExitY = pstExitGate->dfLat;
	pstSession->dtGateExit = stRawLogInfo.dtGPS;
	pstSession->dwGateExitGpsSeq = stRawLogInfo.dwSeqNo;
	pstSession->qwGateExitLinkID = pstExitGate->qwLinkID;
	{
		POINT stGateP, stTickP;
		stGateP.dfX = pstExitGate->dfLon;         stGateP.dfY = pstExitGate->dfLat;
		stTickP.dfX = pstSession->dfClosedLastX;  stTickP.dfY = pstSession->dfClosedLastY;
		pstSession->bGateExitAtTick = (HaversineMeters(stGateP, stTickP) <= 2.0);
	}

	pstSession->bInClosedRoad = false;
}

/**
 * @brief 구간단속 — SKIP 틱(맵매칭 실패) raw GPS 기준 출구 판정
 * @remark CheckClosedRoadExitByRawGps() 와 동일 원리, 자세한 배경은 그쪽 주석 참고 (2026-08-24 최정우 추가)
*/
void CRawLogWorker::CheckSpeedZoneExitByRawGps(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if (!pstSession->bInSpeedZone || (m_stConfig.pcChargeDataLoader == nullptr)
		|| m_stConfig.strChargeInsertSQL.empty() || stRawLogInfo.bGpsLatNull || stRawLogInfo.bGpsLonNull)
		return;

	PGATE_INFO pstExitGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(pstSession->szSpeedZoneRoadId, 'O');
	if (pstExitGate == nullptr)
		return;
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szSpeedZoneRoadId);
	if (pstZone == nullptr)
		return;

	POINT stZoneStart, stGatePos, stRaw;
	stZoneStart.dfX = pstZone->dfFirstLon;  stZoneStart.dfY = pstZone->dfFirstLat;
	stGatePos.dfX = pstExitGate->dfLon;     stGatePos.dfY = pstExitGate->dfLat;
	stRaw.dfX = stRawLogInfo.dfX;           stRaw.dfY = stRawLogInfo.dfY;

	double dfGateFromStart = HaversineMeters(stZoneStart, stGatePos);
	double dfRawFromStart = HaversineMeters(stZoneStart, stRaw);
	if (dfRawFromStart < (dfGateFromStart + MM_RAWGPS_EXIT_MARGIN_M))
		return;

	PZONE_INFO pstZoneForRow = pstZone;
	CHARGE_INSERT_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "3";								// SPEED
	stRow.strChargeUnit = "1";
	stRow.strLinkId = "";

	stRow.strFromId = pstSession->szSpeedEntryTollgateId;
	stRow.strToId = pstExitGate->szTollgateID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	// [버그 수정, 2026-09-10 최정우] ProcessSpeedZoneCharge() 의 게이트 확정 진출 경로와 동일 근거
	// (bClosedEntryAmbiguous 동일 패턴) — 진입이 애매했으면 FROM을 구역 등록 시작점 대신 실제
	// 관측 시작점으로 써야 DIST_M(실측 누적거리)과 같은 구간을 가리킨다. 이 SKIP 틱 경로만 빠져
	// 있었다.
	if (pstSession->bSpeedEntryAmbiguous)
	{
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfSpeedEntryFromLat);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfSpeedEntryFromLon);
	}
	else
	{
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZoneForRow->dfFirstLat);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZoneForRow->dfFirstLon);
	}
	snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);
	stRow.strFromLat = szFromLat;
	stRow.strFromLon = szFromLon;
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = pstSession->szSpeedZoneRoadId;
	stRow.strZoneName = pstZoneForRow->szRoadNm;

	// 진입 애매(bSpeedEntryAmbiguous)했으면 "구역 전체를 다 지났다"는 전제가 성립하지 않으므로
	//   실제 관측 출발지점~출구 게이트 실거리를 쓴다 — 위 FROM_LAT/LON(2026-09-10 수정)과 같은
	//   근거인데 dfLengthM 만 빠져 있었다. ProcessSpeedZoneCharge() 정상매칭 경로(2026-08-25)와
	//   동일 처리를 raw GPS 기준 SKIP 이탈 경로에도 적용(2026-09-11 최정우 추가)
	double dfLengthM;
	if (pstSession->bSpeedEntryAmbiguous)
	{
		POINT stStartPos, stGatePos2;
		stStartPos.dfX = pstSession->dfSpeedEntryFromLon;  stStartPos.dfY = pstSession->dfSpeedEntryFromLat;
		stGatePos2.dfX = pstExitGate->dfLon;               stGatePos2.dfY = pstExitGate->dfLat;
		dfLengthM = CalcZoneSpanM(pstZoneForRow, stStartPos.dfX, stStartPos.dfY, stGatePos2.dfX, stGatePos2.dfY,
			HaversineMeters(stStartPos, stGatePos2));		// 2026-10-04: 직선 -> 구역 선을 따라
	}
	else
	{
		// 2026-10-04 최정우 수정(사용자 확정): 구역 전체 길이 -> 진입·진출 게이트 사이 선 길이(CalcZoneSpanM)
		PGATE_INFO pstEntryGateForLen = (m_stConfig.pcChargeDataLoader != nullptr)
			? m_stConfig.pcChargeDataLoader->GetGateByTollgateId(pstSession->szSpeedEntryTollgateId) : nullptr;
		dfLengthM = (pstEntryGateForLen != nullptr)
			? CalcZoneSpanM(pstZoneForRow, pstEntryGateForLen->dfLon, pstEntryGateForLen->dfLat,
				pstExitGate->dfLon, pstExitGate->dfLat, pstZoneForRow->dfLengthM)
			: pstZoneForRow->dfLengthM;
	}
	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
	stRow.strDistM = szDistM;

	double dfElapsedSec = difftime(stRawLogInfo.dtGPS, pstSession->dtSpeedEntryTime);
	// [버그 수정, 2026-09-11 최정우] 진입~진출이 같은 초(또는 시각 역행)면 dfElapsedSec<=0 이 되어
	//   평균속도 계산이 통째로 건너뛰어지고 strSpeedKmh 가 빈 값(→query.sql CASE 문으로 0 저장)으로
	//   남아 실제로는 빠르게 지나간 구간이 speed_kmh=0 으로 잘못 기록됐다. 다른 형제 함수들
	//   (BuildExemptRow()/BuildNodeStepRowFromLinkRange()/ProcessSpeedZoneCharge() 등)과 동일하게 최소 1초로 클램프.
	if (dfElapsedSec < 1.0)
		dfElapsedSec = 1.0;
	{
		double dfAvgSpeedKmh = (dfLengthM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}

	char szSpeedLimit[16];					// (2026-09-15 최정우 수정) 8 → 16: 8 이면 7자리 초과 값이 잘려
											//   엉뚱한 수가 된다(제한속도는 실무상 3자리지만 기준정보 오류 시
											//   큰 값이 들어올 수 있다). [2026-09-17 정정] 종전 주석의 "클램프
											//   WARN 로그" 는 이 함수들에 존재하지 않는 내용이라 삭제
	snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZoneForRow->dfSpeedLimitKmh + 0.5));
	stRow.strSpeedLimitKmh = szSpeedLimit;

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	char szStartGpsSeq[16], szEndGpsSeq[16];
	snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", pstSession->dwSpeedEntryGpsSeq);
	snprintf(szEndGpsSeq, sizeof(szEndGpsSeq), "%u", stRawLogInfo.dwSeqNo);
	stRow.strStartGpsSeq = szStartGpsSeq;
	stRow.strEndGpsSeq = szEndGpsSeq;

	stRow.strOccurDt = FormatDateTime14(pstSession->dtSpeedEntryTime);

	const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
	stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = pstSession->szSpeedEntryTollgateId;
	stRow.strExitTollgateId = pstExitGate->szTollgateID;

	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;

	// ProcessSpeedZoneCharge() 와 동일 근거 — 입구 게이트 미확정 시 AUDIT(N/3) (2026-08-25 최정우 추가)
	const bool bEntryMissing = (pstSession->szSpeedEntryTollgateId[0] == '\0');
	const bool bExitMissing = (pstExitGate->szTollgateID[0] == '\0');
	const bool bEntryEqualsExit = !bEntryMissing && !bExitMissing
		&& (strcmp(pstSession->szSpeedEntryTollgateId, pstExitGate->szTollgateID) == 0);
	bool bGateAnomaly = bEntryMissing || bExitMissing || bEntryEqualsExit;
	stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
	stRow.strChargeStatus = bGateAnomaly ? "3" : "0";
	if (bGateAnomaly)
	{
		// [버그 수정, 2026-09-11 최정우] non_charge_reason 정식 코드 기록 — ProcessSpeedZoneCharge()
		//   와 동일 근거·우선순위(입구 미확인→출구 미확인→입구==출구)
		const int nNonChargeReason = bEntryMissing ? NCR_SPEED_ENTRY_UNOBSERVED
			: bExitMissing ? NCR_SPEED_EXIT_UNCONFIRMED : NCR_SPEED_ENTRY_EQUALS_EXIT;
		char szReason[8];
		snprintf(szReason, sizeof(szReason), "%d", nNonChargeReason);
		stRow.strNonChargeReason = szReason;

		LOGFMTW("[#%02d] speed zone gate anomaly(raw-gps exit)!device=[%s] trip_id=[%s] seq=[%d] "
			"entry=[%s] exit=[%s] -> charge_yn=N non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			pstSession->szSpeedEntryTollgateId, pstExitGate->szTollgateID, nNonChargeReason,
			m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nNonChargeReason));
	}

	// [버그 수정, 2026-09-10 최정우] ProcessSpeedZoneCharge() 는 2026-09-06부터 "진입게이트 확인 +
	// 진출게이트 확인 + 평균속도>=제한속도(위반)" 를 모두 충족해야만 SPEED 레코드를 만드는데(적재
	// 조건, 원 근거는 그 함수 주석 참고), 이 SKIP 틱 경로는 그 정책이 반영 안 된 채 항상
	// push_back 해서, 비위반 통행에도 SPEED 과금 레코드가 생성되고 있었다(실측 대신 최소 재현
	// 으로 확인 — 정상경로/SKIP경로 push 여부가 갈림). 같은 판정을 여기도 적용한다.
	double dfAvgSpeedKmhForJudge = (dfLengthM / dfElapsedSec) * 3.6;
	bool bSpeedLimitKnown = (pstZone != nullptr) && (pstZone->dfSpeedLimitKmh > 0.0);
	bool bViolated = bSpeedLimitKnown && (dfAvgSpeedKmhForJudge >= pstZone->dfSpeedLimitKmh);

	if (!bGateAnomaly && (!bSpeedLimitKnown || bViolated))
	{
		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] speed zone exit charge queued (raw gps)!device=[%s] trip_id=[%s] seq=[%d] "
			"road=[%s] dist_m=[%s] margin_over=[%.1f]m non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			pstSession->szSpeedZoneRoadId, szDistM, dfRawFromStart - dfGateFromStart,
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));

		pstSession->nChargeSeq += 1;
	}
	// [버그 수정, 2026-09-10 최정우] NODE_STEP 일반도로 확장(케이스1) 미러 레코드 — 위반 여부와
	// 무관하게 제한속도만 알면 항상 만든다(ProcessSpeedZoneCharge() 동일 로직 그대로 재사용,
	// 그 함수의 stHeldSpeedMirrorRun 세팅 블록 참고). 이 SKIP 틱 경로는 이 미러 자체가 통째로 빠져 있어, 여기로 마감되는
	// 구간단속 통행은 NODE_STEP 일반도로 과금도 같이 누락되고 있었다.
	if (bSpeedLimitKnown)
	{
		double dfFromLatRaw = pstSession->bSpeedEntryAmbiguous ? pstSession->dfSpeedEntryFromLat : pstZoneForRow->dfFirstLat;
		double dfFromLonRaw = pstSession->bSpeedEntryAmbiguous ? pstSession->dfSpeedEntryFromLon : pstZoneForRow->dfFirstLon;

		pstSession->stHeldSpeedMirrorRun.dtEntryTime = pstSession->dtSpeedEntryTime;
		pstSession->stHeldSpeedMirrorRun.dwEntryGpsSeq = pstSession->dwSpeedEntryGpsSeq;
		pstSession->stHeldSpeedMirrorRun.dfEntryX = dfFromLonRaw;
		pstSession->stHeldSpeedMirrorRun.dfEntryY = dfFromLatRaw;
		pstSession->stHeldSpeedMirrorRun.qwEntryLinkID = pstSession->qwSpeedEntryLinkID;
		pstSession->stHeldSpeedMirrorRun.dfAccumDistM = dfLengthM;
		pstSession->stHeldSpeedMirrorRun.qwLastLinkID = pstSession->qwSpeedLastZoneLinkID;
		pstSession->stHeldSpeedMirrorRun.dfLastX = pstExitGate->dfLon;
		pstSession->stHeldSpeedMirrorRun.dfLastY = pstExitGate->dfLat;
		pstSession->stHeldSpeedMirrorRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
		pstSession->stHeldSpeedMirrorRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
		pstSession->bHasHeldSpeedMirrorRun = true;
		pstSession->dwHeldSpeedMirrorSeq = stRawLogInfo.dwSeqNo;

		LOGFMTI("[#%02d] node step (from speed zone %s, raw gps) held for handoff merge!device=[%s] "
			"trip_id=[%s] road=[%s] dist_m=[%s] non_charge_reason=[%d:%s]", nThreadId,
			bViolated ? "violation" : "non-violation",
			stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			pstSession->szSpeedZoneRoadId, szDistM,
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
	}

	// [2026-09-23 최정우 추가, 사용자 지시 — 전체 소스 재검토] 정상 매칭 진출 경로
	//   (ProcessSpeedZoneCharge, 같은 날 추가)와 동일하게 게이트 진출 지점을 이월한다.
	//   폐쇄형은 이 대칭을 2026-09-10 에 맞췄는데(CheckClosedRoadExitByRawGps 의 같은 블록)
	//   구간단속만 raw GPS(SKIP 틱) 경로에 처리가 빠져 있었다 — 그러면 여기로 마감되는 통행은
	//   다음 일반도로 run 진입점이 게이트가 아니라 "다음 확정 매칭 tick 좌표"가 되어 게이트~그
	//   tick 사이 구간이 누락되고, 그 직후 다른 과금유형에 진입하는 경우의 미등록 링크 복구
	//   (gate exit orphan span)도 이월이 없어 아예 돌지 않는다.
	//   dfSpeedLastX/Y(마지막 확인 매칭 위치, ProcessSpeedZoneCharge 가 매 틱 갱신)를 정상 경로와
	//   동일하게 "게이트와 일치하는 tick 인지" 판정 기준으로 재사용한다.
	pstSession->bHasGateExitCarry = true;
	pstSession->dfGateExitX = pstExitGate->dfLon;
	pstSession->dfGateExitY = pstExitGate->dfLat;
	pstSession->dtGateExit = stRawLogInfo.dtGPS;
	pstSession->dwGateExitGpsSeq = stRawLogInfo.dwSeqNo;
	pstSession->qwGateExitLinkID = pstExitGate->qwLinkID;
	{
		POINT stGateP, stTickP;
		stGateP.dfX = pstExitGate->dfLon;         stGateP.dfY = pstExitGate->dfLat;
		stTickP.dfX = pstSession->dfSpeedLastX;   stTickP.dfY = pstSession->dfSpeedLastY;
		pstSession->bGateExitAtTick = (HaversineMeters(stGateP, stTickP) <= 2.0);
	}

	pstSession->bInSpeedZone = false;
}

/**
 * @brief 면제도로 진입/이탈 판정 (2026-08-13 최초 추가, 2026-08-14 세 차례 재설계)
 * @remark 2026-08-14 사용자 재지시로 zone 기반 판정으로 복귀 — base_roadlink 에 등록된 면제도로
 *   구역(road_kind=5, link_ids)에 매칭 링크가 속하는지로 판정("모든 미등록 링크" 방식은 폐기).
 *   charge_type="5"(비과금도로 고유값 — 한때 "0"(일반도로와 통합)으로 바꿨다가 사용자 재지시로
 *   원복됨). from_id/to_id는 zone의 road_id(base_roadlink 등록값, 링크 ID 아님 — 이것도 한때
 *   링크ID였다가 재지시로 원복). zone_id/zone_name도 동일하게 실제 매칭된 구역의 road_id/road_nm.
 *   from/to_lat·lon은 진입/진출 시점의 매칭 위치. stay_seconds는 진출-진입 체류시간(초), 평균속도는
 *   speed_kmh로 분리 기록.
 *   이탈(다른 구역/미등록 링크로 이동) 또는 트립 종료 시 `charge_yn='Y'`/`charge_status='0'`
 *   (정상)으로 1건 기록한다 — 다른 과금 유형의 정상 통행과 동일하며, 과금 제외는 charge_yn 이
 *   아니라 charge_type=5 로 구분된다. 이상 건만 N/4(SKIP): 강제마감은
 *   AppendExpiredExemptZoneCharge(), 트립 미종료(TTL)는 같은 함수가 직접 N/4 로 넣는다(종전 [trip_abend] 는 2026-10-04 삭제).
 *   [2026-09-17 최정우 정정] 종전 주석은 "이탈·트립종료 시 **항상** N/4(SKIP)" 라고 적혀 있었는데,
 *   이는 2026-08-30 에 "정상 이탈은 Y/0, 이상 건만 N/4" 로 바꾸기 전의 서술이다(실제 코드는
 *   그때부터 BuildExemptRow() 에 "Y","0" 을 넘긴다). 주석만 현행에 맞춤 — 동작 변화 없음.
*/

void CRawLogWorker::ProcessExemptZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경유 링크 포함 전체 경로에서 면제구역을 "전부" 수집 — 짧은 면제구역 링크가 두 GPS 사이에
	//   통째로 끼어 안 잡히는 경우 보완 + 한 링크가 여러 구역에 속하는 경우 대응
	//   (2026-08-20 추가 → 2026-08-23 복수 구역 지원)
	//   [2026-09-23 최정우 수정 — 사용자 지적] **구역과 함께 "그 구역이었던 경로 링크"도 보관한다.**
	//   종전에는 구역 포인터만 모으고 링크를 버려서, 판정은 경로로 하고 기록(qwLastLinkID)은
	//   **매칭 링크**로 했다. 매칭 링크가 구역 밖이면 이탈 경계 보정이 **구역 밖 링크의 종료
	//   노드까지** 거리를 더한다 — 실측 000993_20250903152139 seq417: RL-Z00018 행의 dist_m 102m 가
	//   미등록 링크 2040425801 의 전체 길이(102m)였다. 같은 링크를 쓰면 그 오차가 사라진다.
	vector<PZONE_INFO> vtZones;
	vector<uint64>     vtZoneLinks;					// vtZones[i] 가 발견된 경로 링크(뒤엣것으로 갱신)
	{
		vector<PZONE_INFO> vtOne;
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;

		// 한 링크에서 찾은 구역들을 그 링크와 짝지어 담는다 — 같은 구역이 여러 경로 링크에
		//   걸치면 **진행 방향상 마지막 링크**가 남아야 이탈 보정의 기준이 맞는다
		auto fnCollect = [&](uint64 qwLink)
		{
			if (qwLink == 0) return;
			vtOne.clear();
			m_stConfig.pcChargeDataLoader->GetExemptZonesByLinkId(qwLink, &vtOne);
			for (size_t i = 0; i < vtOne.size(); ++i)
			{
				bool bDup = false;
				for (size_t e = 0; e < vtZones.size(); ++e)
				{
					if (strcmp(vtZones[e]->szRoadID, vtOne[i]->szRoadID) == 0)
					{ vtZoneLinks[e] = qwLink; bDup = true; break; }
				}
				if (!bDup) { vtZones.push_back(vtOne[i]); vtZoneLinks.push_back(qwLink); }
			}
		};

		if (nPathCount == 0)
			fnCollect(stMatchLinkInfo.qwLinkID);
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
				fnCollect(stMatchLinkInfo.aqwPathLinkIDs[i]);
		}
	}

	const bool bTripEnding = bTrustedTripEnd;

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtExemptRuns.size(); )
	{
		ZONE_RUN_SESSION& stRun = pstSession->vtExemptRuns[si];

		bool bSameZone = false;
		uint64 qwZoneLink = 0;						// 이번 tick 에서 그 구역이었던 경로 링크
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0)
			{ bSameZone = true; qwZoneLink = vtZoneLinks[e]; break; }
		}

		// dist_m·stay_seconds 는 "면제도로(base_roadlink 등록 링크) 위를 실제로 달린 만큼"이다.
		//   그래서 이 tick 이 그 구역에 매칭됐을 때만 누적·갱신한다. 구역을 벗어난 뒤 재진입
		//   유예(exempt_regrace) 를 기다리는 동안의 주행은 면제도로 주행이 아니므로 제외한다.
		//   (사용자 지시, 2026-08-30 최정우 수정 — 이전에는 유예 구간까지 무조건 가산해
		//    64m 구역에 719m 가 기록되는 문제가 있었다. 2026-08-14 의 "유예 구간 포함" 지시를 대체)
		if (bSameZone)
		{
			stRun.dtFirstOut = 0;								// 재진입 — 이탈 보간 기준점 리셋
			stRun.dfOutDist = 0.0;								// 재진입 — 구역 밖 주행거리 리셋 (2026-09-23 최정우 추가)

			// 역행의심(bReverseSuspect) tick — reverse_confirm 스트릭이 확정돼 맵매칭 자체는
			//   MATCHED로 인정되더라도, EXEMPT 누적 관점에서는 여전히 "직전 위치보다 뒤로 간 것"이라
			//   run의 마지막 위치·누적거리를 갱신하지 않고 그대로 대기한다. NODE_STEP의 동일 처리
			//   (2026-09-03) 와 같은 근거 (2026-09-11 최정우 추가)
			// [버그 수정, 2026-09-11 최정우] 정지 중(bSameRawAndHeadingAsPrev) GPS 저주파 위치표류도
			//   같은 이유로 제외 — 차가 안 움직이는데 세그먼트 재투영이 매 tick 완만히 다른 점으로
			//   튀어 dist_m 이 과다 계상되는 걸 막는다(재매칭 검증으로 최대 20m 확인).
			if (!stMatchLinkInfo.bReverseSuspect && !stMatchLinkInfo.bSameRawAndHeadingAsPrev)
			{
				POINT stPrev, stCur;
				stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
				stCur.dfX = stMatchLinkInfo.dfMatchX;  stCur.dfY = stMatchLinkInfo.dfMatchY;
				stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
				stRun.dfLastX = stMatchLinkInfo.dfMatchX;
				stRun.dfLastY = stMatchLinkInfo.dfMatchY;
			}
			// [버그 수정, 2026-09-15 최정우] ProcessNodeStepCharge() 와 동일 수정 — 근거·역행의심
			//   취급 차이는 그쪽 주석 참고. 여기서는 dtLastInZoneTime 이 아래 이탈 경계 보정(InterpolateGateCrossingTime)의
			//   "안쪽" 기준시각으로도 쓰이는데, 정지 중 시각을 얼려두면 보간 구간 자체가 왜곡된다.
			//   [2026-09-23 수정] 매칭 링크가 아니라 **그 구역에 속한 경로 링크**를 남긴다 —
			//   이 값이 이탈 경계 보정의 기준 링크가 되므로, 구역 밖 링크가 들어가면 그 링크
			//   길이만큼 구역 거리가 부풀려진다
			if (!stMatchLinkInfo.bReverseSuspect)
				stRun.qwLastLinkID = (qwZoneLink != 0) ? qwZoneLink : stMatchLinkInfo.qwLinkID;
			stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;		// to_lat/lon·stay_seconds 기준 tick
			stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;	// end_gps_seq
		}
		else
		{
			// [2026-09-23 최정우 추가 — 사용자 지시] 구역 밖 주행거리 누적.
			//   첫 이탈 tick 은 **구역 안 마지막 위치**(dfLastX/Y)에서부터 재고, 이후 tick 은
			//   직전 구역 밖 위치에서 잰다. 아래 유예 게이트가 이 값으로 "경계 흔들림"과
			//   "실제 이탈 후 재진입"을 가른다. 이 함수는 bMatched 블록에서만 불리므로
			//   dfMatchX/Y 는 항상 유효하다(SKIP tick 은 애초에 들어오지 않는다).
			POINT stOutPrev, stOutCur;
			stOutCur.dfX = stMatchLinkInfo.dfMatchX;
			stOutCur.dfY = stMatchLinkInfo.dfMatchY;

			if (stRun.dtFirstOut == 0)
			{
				// 구역 밖 "첫" tick — 이탈 경계 보간의 바깥쪽 기준점. 이후 tick 으로 덮어쓰면 유예
				//   구간 끝점과 보간하게 돼 엉뚱한 결과가 나온다(개방형 dfFirstOut* 동일 근거)
				stRun.dfFirstOutX = stMatchLinkInfo.dfMatchX;
				stRun.dfFirstOutY = stMatchLinkInfo.dfMatchY;
				stRun.dtFirstOut = stRawLogInfo.dtGPS;
				stRun.qwFirstOutLinkID = stMatchLinkInfo.qwLinkID;

				stOutPrev.dfX = stRun.dfLastX;
				stOutPrev.dfY = stRun.dfLastY;
			}
			else
			{
				stOutPrev.dfX = stRun.dfOutLastX;
				stOutPrev.dfY = stRun.dfOutLastY;
			}

			if ((stOutPrev.dfX != 0.0) || (stOutPrev.dfY != 0.0))
				stRun.dfOutDist += HaversineMeters(stOutPrev, stOutCur);
			stRun.dfOutLastX = stOutCur.dfX;
			stRun.dfOutLastY = stOutCur.dfY;
		}

		if (bSameZone && !bTripEnding)
		{
			stRun.dtExitCandidateTime = 0;				// 재진입 확인 — 유예 대기 해제
			// [2026-09-22 최정우 추가] 먼저 걸어둔 진출 이월도 취소한다 — 아직 안 나간 것이 되므로
			if (stRun.bExitCarryArmed)
			{
				pstSession->bHasGateExitCarry = false;
				stRun.bExitCarryArmed = false;
			}
			++si; continue;
		}

		// [2026-09-21 최정우 — 시도했다가 **원복**. 같은 수정을 다시 하지 말 것 — 이슈 21]
		//   유예 게이트에서 `vtZones.empty()` 조건을 빼 봤다가 되돌렸다.
		//   **문제 자체는 실재한다(실증 확보)**: RL-Z00018 을 벗어나 17초 만에 복귀하는데
		//   (exempt_regrace 60초 안) 그 사이 바로 옆 면제구역 RL-Z00007 을 2 tick 스치면
		//   vtZones 가 비지 않아 유예가 통째로 건너뛰어져 즉시 마감된다 — 실측
		//   000998_20260917090000 seq3~19(89m + 342m 로 분할), 000983·000982·000993 동일 패턴.
		//   종전 기록의 "실증 0건" 은 **틀렸다**(같은 유형의 옆 구역이라 못 찾았던 것).
		//   종전 기록의 수정 방향 "이탈 tick 디바운스 추가" 도 **맞지 않는다** — 이 사례는 구역
		//   밖이 5 tick(17초)이라 디바운스 3 으로도 마감된다.
		//   **왜 원복했나**: 유예를 켜면 레코드 확정이 늦어지는데, 같은 지점에 묶여 있는
		//   **이탈 경계 보정과 진출 이월(bHasGateExitCarry)까지 함께 늦어진다.** 그러면 바로 뒤
		//   일반도로 run 이 받던 이월 거리를 잃는다 — 실측 000998 seq7~8 60m→37m, seq10~11
		//   75m→42m. 전체 재매칭에서 **관계없는 7 트립의 거리가 ±239m(일반도로 +156·면제 +83)**
		//   움직였고 방향도 일관되지 않았다(과금합/경로길이 0.92→0.84, 1.06→1.13). 면제는
		//   charge_type=5 비과금이라 원 문제는 **요금 영향 0 인 표출 분할**인데, 고치려다
		//   **실제 과금 대상인 일반도로 거리**를 건드린 셈이라 되돌렸다.
		//   **근본 해결책은 있다** — 한 지점에 묶인 세 가지(① 이탈 경계 보정 ② 레코드 확정
		//   ③ 진출 이월) 중 ①·③ 은 "구역을 벗어난 순간"의 사실이고 ② 만 "여기서 끊는다" 는
		//   결정이다. ①·③ 을 **첫 이탈 tick**(stRun.dtFirstOut/dfFirstOutX/Y 가 이미 그 시점에
		//   잡혀 있다)으로 옮기고 ② 만 유예하면 분할과 이월 손실이 동시에 풀린다. 다만 이월은
		//   ApplyGateExitCarryDist·섬 분할·bGateExitAtTick 과 얽혀 영향 범위가 넓어, 실서버 배포를
		//   한 번 거친 뒤 별건으로 할 것.
		// [2026-09-22 최정우 수정 — 사용자 지시, 이슈 21 근본책 적용]
		//   한 지점에 묶여 있던 세 가지 중 **③ 진출 이월을 유예보다 앞으로** 뺀다.
		//   이월은 "구역을 벗어났다"는 **사실**이지 "여기서 끊는다"는 **결정**이 아니다. 종전에는
		//   유예를 통과해야 이월이 걸려서, 유예를 켜면 바로 뒤 일반도로 run 이 받을 이월 거리를
		//   통째로 잃었다(실측 000998 seq7~8 60m→37m) — 그게 2026-09-21 원복의 직접 원인이었다.
		//   먼저 걸어두고 **재진입하면 취소**하면 둘 다 성립한다(위 bSameZone 분기 참고).
		if (!bTripEnding && !stRun.bExitCarryArmed)
		{
			pstSession->bHasGateExitCarry = true;
			pstSession->dfGateExitX = stRun.dfLastX;
			pstSession->dfGateExitY = stRun.dfLastY;
			pstSession->dtGateExit = stRun.dtLastInZoneTime;	// 마감 때 경계보정값으로 갱신된다
			pstSession->dwGateExitGpsSeq = stRun.dwLastInZoneGpsSeq;
			pstSession->qwGateExitLinkID = stRun.qwLastLinkID;
			pstSession->bGateExitAtTick = false;
			stRun.bExitCarryArmed = true;
		}

		// [2026-09-22 최정우 수정] **vtZones.empty() 조건을 뺀다.** 종전에는 옆 면제구역을 1 tick
		//   스치기만 해도 vtZones 가 비지 않아 유예가 통째로 건너뛰어져 같은 구역이 둘로 쪼개졌다
		//   (실측 000993 RL-Z00018 98m+342m, 000998·000983·000982 동일). 2026-09-21 에 같은 수정을
		//   했다가 원복한 이유는 **이월 손실**이었는데, 위에서 이월을 앞으로 뺐으므로 해소된다.
		//   ※ 디바운스(exempt_exitcnt) 추가는 **답이 아니다** — 이 사례들은 구역 밖이 4~5 tick 이라
		//     디바운스 3 으로도 그대로 마감된다(이 블록 위 2026-09-21 기록 참고).
		// [2026-09-23 최정우 추가 — 사용자 지시] **구역 밖 주행거리 상한(exempt_outmax).**
		//   유예의 취지는 config.ini 주석대로 "잠깐 버퍼 주행 후 복귀"인데, 시간(60초)만으로는
		//   경계 흔들림과 실제 이탈을 못 가른다. 실측 000998_20260917090000 은 구역을 벗어나
		//   5 tick·17초 동안 220m 를 41~53km/h 로 달리고 U턴해 복귀하는데도 유예에 흡수돼
		//   면제 run 이 seq3~19 로 잡혀, 그 안의 일반도로·타 면제구역 레코드를 감쌌다.
		//   전체 재매칭 실측에서 유예가 흡수 중이던 구역 밖 구간 13건의 주행거리는
		//   1 tick 6~57m / 2 tick 75m 와 4~15 tick 165~239m 로 **뚜렷이 갈린다** —
		//   그 사이인 100m 를 기본값으로 둔다.
		const bool bOutTooFar = (stRun.dfOutDist >= static_cast<double>(m_stConfig.nExemptOutMax));

		if (!bTripEnding && !bOutTooFar)
		{
			// "무존"(다음 행선지 미확인) — exempt_regrace 초 동안 확정 마감을 보류하고 재진입을 기다림
			if (stRun.dtExitCandidateTime == 0)
				stRun.dtExitCandidateTime = stRawLogInfo.dtGPS;

			double dfGraceElapsedSec = difftime(stRawLogInfo.dtGPS, stRun.dtExitCandidateTime);
			if (dfGraceElapsedSec < static_cast<double>(m_stConfig.nExemptRegraceSec))
			{ ++si; continue; }
		}

		// 이탈 경계 보정 — 구역 안 마지막 링크의 종료 노드까지 거리·시각을 채운다. 개방형
		//   ProcessOpenGateCharge() 의 dtOpenExitTime 처리와 동일 원리(InterpolateGateCrossingTime
		//   을 "구역 경계 노드"에 겨냥). 트립이 구역 안에서 끝난 경우(bSameZone)는 그 지점이 곧
		//   진출점이므로 보정하지 않는다 (사용자 지시, 2026-08-30 최정우 추가)
		time_t dtExemptEnd = (stRun.dtLastInZoneTime != 0) ? stRun.dtLastInZoneTime : stRawLogInfo.dtGPS;
		if (!bSameZone && (stRun.qwLastLinkID != 0) && (m_stConfig.pcDataLoader != nullptr))
		{
			PLINK_INFO pstLastLink = m_stConfig.pcDataLoader->GetLinkInfo(stRun.qwLastLinkID);
			if (pstLastLink != nullptr)
			{
				POINT stFrom, stNode;
				stFrom.dfX = stRun.dfLastX;  stFrom.dfY = stRun.dfLastY;
				stNode.dfX = static_cast<double>(pstLastLink->dwEdNodeX) / 360000.0;
				stNode.dfY = static_cast<double>(pstLastLink->dwEdNodeY) / 360000.0;

				double dfTail = HaversineMeters(stFrom, stNode);
				if ((dfTail > 0.0) && (dfTail <= pstLastLink->dfLen + 1.0))
				{
					stRun.dfAccumDistM += dfTail;
					stRun.dfLastX = stNode.dfX;
					stRun.dfLastY = stNode.dfY;
				}

				if (stRun.dtFirstOut != 0)
				{
					dtExemptEnd = InterpolateGateCrossingTime(
						stFrom.dfX, stFrom.dfY, stRun.dtLastInZoneTime,
						stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut,
						stNode.dfX, stNode.dfY);
				}
			}
		}

		CHARGE_INSERT_ROW stRow;
		BuildExemptRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, dtExemptEnd, stRun.dwLastInZoneGpsSeq,
			"Y", "0", &stRow);
		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] exempt zone exit recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"dist_m=[%s] trip_ending=[%d] out_dist_m=[%.0f] grace_skipped=[%d] non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, stRow.strDistM.c_str(), static_cast<int>(bTripEnding),
			stRun.dfOutDist, static_cast<int>(bOutTooFar),		// (2026-09-23 최정우 추가 — exempt_outmax 판정 근거)
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));

		pstSession->nChargeSeq += 1;

		// 면제구역 진출 지점을 이월해 둔다 — 다음 일반도로 run 이 이 지점부터 시작하도록.
		//   폐쇄식·개방식·구간단속은 진출게이트가 그 역할을 하지만(bHasGateExitCarry —
		//   ProcessClosedRoadCharge()/ProcessOpenGateCharge()/ProcessSpeedZoneCharge()/
		//   Check{ClosedRoad,SpeedZone}ExitByRawGps() 의 이월 세팅부)
		//   면제구역은 게이트가 없어 이 장치가 없었고, 그래서 "면제 진출점 ~ 일반도로 첫 매칭점"
		//   구간이 어느 레코드에도 안 들어갔다 — 실측 000983_20250903153213: 면제 RL-Z00018
		//   진출점(37.407089,127.085892)과 바로 뒤 일반도로 시작점(37.407050,127.085256) 사이
		//   56.3m 가 통째로 누락(사용자 지적). 바로 위 "이탈 경계 보정"이 dfLastX/Y 를 이미 구역
		//   마지막 링크의 종료노드(=진출점)로 옮겨놨으므로 그 값을 그대로 넘긴다. 소비 쪽
		//   (ApplyGateExitCarryDist)이 이월지점~다음 tick 사이 누락 링크까지 복구하므로 게이트형과
		//   똑같이 동작한다. 구역 안에서 끝난 경우(bSameZone)·트립종료는 진출이 아니라 이월하지
		//   않는다. 게이트와 달리 진출점은 구역 안쪽 경계라 이번 tick 과 순번을 공유하지 않는다
		//   (bGateExitAtTick = false) (2026-09-16 최정우 추가, 사용자 지시)
		if (!bSameZone && !bTripEnding && (stRun.qwLastLinkID != 0))
		{
			// 이월을 세팅하기 전에, **이미 열려 있는** 미등록 일반도로 run 이 있으면 그 자리에서
			//   소급 적용한다 — 면제는 exempt_regrace 유예 때문에 이탈 확정이 1틱 이상 늦어, 그
			//   사이 일반도로 run 이 먼저 열려버려 "새 run 개시" 시점의 이월 소비를 영영 못 만난다
			//   (실측 000983_20250903153213: seq7 에 run 이 열리고 seq8 에 면제가 마감돼 이월이
			//   그대로 방치되다 seq11 에서 타 유형 tick 으로 폐기됐다). 게이트형(폐쇄·개방)은 이탈이
			//   그 tick 에 즉시 확정돼 이 문제가 없다. 구간단속 미러의 "absorbed into already-open
			//   run" 과 같은 패턴이다.
			//   진입 순번(dwEntryGpsSeq)·FROM_ID 는 건드리지 않는다 — 진출점은 구역 안쪽 경계라
			//   그 tick 을 공유하지 않고(게이트형 bGateExitAtTick=false 와 동일), FROM_ID 를 구역
			//   링크로 덮으면 같은 구간이 두 유형으로 중복 계상된다(게이트 이월 쪽 주석과 동일 근거)
			//   (2026-09-16 최정우 추가, 사용자 지적)
			bool bAppliedToOpenRun = false;
			for (size_t ri = 0; ri < pstSession->vtNodeStepRuns.size(); ++ri)
			{
				ZONE_RUN_SESSION& stNsRun = pstSession->vtNodeStepRuns[ri];
				if (stNsRun.szRoadID[0] != '\0') continue;		// 미등록 pseudo-zone 만 — 정식
																//   등록구역은 자기 진입 경계가 따로 있다
				// 면제 진출보다 먼저 시작된 run 은 이 구역보다 앞 구간이라 대상이 아니다
				if ((stNsRun.dtEntryTime == 0) || (stNsRun.dtEntryTime < dtExemptEnd)) continue;

				// 진출점~run 진입점 사이의 누락 링크를 복구한다 — GPS 3초 간격이면 그 사이 링크가
				//   어떤 tick 에도 안 찍혀 맵매칭 결과에 아예 안 남는다. 직선거리로 때우면 거리는
				//   메워지지만 FROM_ID 가 여전히 run 이 실제로 열린 링크(구역에서 한 링크 더 간
				//   지점)로 남아, "면제 진출 직후 어느 링크에서 일반도로가 시작됐는지"를 못 나타낸다
				//   — 실측 000983_20250903153213: 면제 2040424201 진출 뒤 2040424202(미등록)부터
				//   일반도로인데 FROM_ID 가 2040424001 로 찍혔다(사용자 지적). NODE_STEP 진출 쪽
				//   누락링크 보정(FindLinkPathBounded + IsCase3EligibleRoadKind)과 같은 방식·같은
				//   상한을 쓰고, 복구 경로 첫 링크를 FROM_ID 로 삼는다 (2026-09-16 최정우 추가)
				double dfCarryDistM = 0.0;
				uint64 qwCarryFromLinkID = 0;
				if ((stNsRun.qwEntryLinkID != 0) && (m_stConfig.pcDataLoader != nullptr)
					&& (m_stConfig.pcChargeDataLoader != nullptr))
				{
					static const int MM_EXEMPT_EXIT_CARRY_MAX_HOPS = 6;
					vector<uint64> vtCarryPath;
					if (FindLinkPathBounded(stRun.qwLastLinkID, stNsRun.qwEntryLinkID,
							MM_EXEMPT_EXIT_CARRY_MAX_HOPS, &vtCarryPath) && (vtCarryPath.size() > 2))
					{
						bool bAllLenOk = true;
						for (size_t g = 1; g + 1 < vtCarryPath.size(); ++g)
						{
							// 복구 경로 중간에 다른 과금유형 등록 링크가 끼면 그 직전에서 멈춘다 —
							//   그 링크부터는 그 유형 자신의 몫이다(NODE_STEP 진출 보정과 동일 기준)
							if (!m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtCarryPath[g]))
							{ bAllLenOk = false; break; }

							PLINK_INFO pstCarryLink = m_stConfig.pcDataLoader->GetLinkInfo(vtCarryPath[g]);
							if (pstCarryLink == nullptr) { bAllLenOk = false; break; }
							dfCarryDistM += pstCarryLink->dfLen;
							if (qwCarryFromLinkID == 0) qwCarryFromLinkID = vtCarryPath[g];
						}
						if (!bAllLenOk) { dfCarryDistM = 0.0; qwCarryFromLinkID = 0; }
					}
				}

				// 경로를 못 찾으면(끊긴 그래프·홉 초과) 직선거리로 폴백한다 — FROM_ID 는 그대로 둔다
				POINT stExitP, stEntryP;
				stExitP.dfX = stRun.dfLastX;     stExitP.dfY = stRun.dfLastY;
				stEntryP.dfX = stNsRun.dfEntryX; stEntryP.dfY = stNsRun.dfEntryY;
				if (qwCarryFromLinkID == 0) dfCarryDistM = HaversineMeters(stExitP, stEntryP);

				// 상한 — GPS 수신 간격 동안 상식적으로 나올 수 있는 거리를 넘으면 연속 구간으로
				//   보지 않는다(중간에 SKIP 이 길게 끼었거나 매칭이 튄 경우)
				static const double MM_EXEMPT_EXIT_CARRY_MAX_GAP_M = 200.0;
				if ((dfCarryDistM <= 0.0) || (dfCarryDistM > MM_EXEMPT_EXIT_CARRY_MAX_GAP_M)) break;

				stNsRun.dfAccumDistM += dfCarryDistM;
				// [2026-09-22 최정우 추가 — 사용자 지적] **주정차 폴리곤 이탈 보정이 이미 진입을 확정했으면
				//   진입 정보를 덮지 않는다.** 폴리곤 경계가 그 구간의 물리적 시작점이고, 면제 진출은 그보다
				//   앞선 사건이다. 덮으면 FROM_ID 가 폴리곤 이탈 링크가 아닌 엉뚱한 링크를 가리킨다
				//   (실측 000993_20250903152139 seq28: 2040426201 이어야 하는데 2040425502 로 찍혔다).
				//   거리는 그대로 더한다 — 그 구간을 실제로 주행했다는 사실은 변하지 않는다.
				if (!stNsRun.bEntryFixedByParkExit)
				{
					stNsRun.dtEntryTime = dtExemptEnd;
					stNsRun.dfEntryX = stRun.dfLastX;
					stNsRun.dfEntryY = stRun.dfLastY;
					if (qwCarryFromLinkID != 0) stNsRun.qwEntryLinkID = qwCarryFromLinkID;
				}
				bAppliedToOpenRun = true;

				LOGFMTI("[#%02d] exempt exit carry applied to open run!device=[%s] trip_id=[%s] "
					"road=[%s] run_entry_seq=[%u] from_link=[%llu] gap=[%.1f]m total=[%.1f]m",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
					stNsRun.dwEntryGpsSeq, static_cast<unsigned long long>(qwCarryFromLinkID),
					dfCarryDistM, stNsRun.dfAccumDistM);
				break;
			}

			// [2026-09-22 최정우 추가 — 사용자 지시] **열린 run 이 없으면 이미 만들어진 행에 소급 적용.**
			//   위 루프는 vtNodeStepRuns(= 아직 **열려 있는** run)만 본다. 면제는 exempt_regrace(60초)
			//   유예 때문에 이탈 확정이 크게 늦어, 그 사이 일반도로 run 이 열렸다가 **이미 마감돼**
			//   목록에서 빠지는 경우가 있다 — 그러면 이월은 적용될 곳을 못 찾고 방치되다 타 유형
			//   tick 을 만나 폐기되고, 진출점~다음 run 시작 사이 구간이 **어느 행에도 안 들어간다**
			//   (실측 000994_20250903152350: 면제 RL-Z00018 진출(gps32, 2040425501) 뒤 gps33 에
			//    2040425101 로 run 이 열렸다가 마감돼 약 50m 가 누락. 전체 136건 중 36건 미적용).
			//   워터마크 큐 도입으로 그 마감된 행이 **아직 INSERT 전** 상태로 남아 있다(이번 배치 벡터 또는
			//   세션 큐 — 두 곳을 모두 봐야 한다. 큐만 뒤졌을 때 적용 0건이었다) —
			//   열린 면제 run 이 워터마크 상한을 낮춰 잡고 있으므로 구조적으로 보장된다.
			//   보정 규칙은 위 "열린 run" 경로와 **완전히 동일**하다(FindLinkPathBounded +
			//   IsCase3EligibleRoadKind + 200m 상한, FROM_ID 는 복구 경로 첫 링크로).
			if (!bAppliedToOpenRun)
			{
				// 대상은 **두 곳**을 본다 — 이번 배치가 방금 만든 행은 아직 큐로 옮겨지기 전이라
				//   pvtChargeInserts 에 있고(큐 이동은 배치 끝의 EnqueueChargeRows), 앞선 배치에서
				//   넘어와 보류 중인 행은 vtPendingEmit 에 있다. 한쪽만 보면 대부분을 놓친다
				//   (실측: 큐만 봤을 때 적용 0건).
				CHARGE_INSERT_ROW *pstQRow = nullptr;
				uint32 dwQBestStart = 0;
				vector<CHARGE_INSERT_ROW> *pvtCandLists[2] =
					{ pvtChargeInserts, &pstSession->vtPendingEmit };
				for (int nList = 0; nList < 2; ++nList)
				{
					if (pvtCandLists[nList] == nullptr) continue;
					vector<CHARGE_INSERT_ROW>& vtCand = *(pvtCandLists[nList]);
					for (size_t qi = 0; qi < vtCand.size(); ++qi)
					{
						CHARGE_INSERT_ROW& stQ = vtCand[qi];
						if (stQ.strChargeType != "0") continue;			// 일반도로 행만 대상
						if (stQ.strTripId != stRawLogInfo.szTripID) continue;
						const uint32 dwQStart = static_cast<uint32>(
							strtoul(stQ.strStartGpsSeq.c_str(), nullptr, 10));
						// 면제 진출보다 뒤에 시작한 행만 — 앞 구간 행에 붙이면 구간이 역전된다
						if (dwQStart <= stRun.dwLastInZoneGpsSeq) continue;
						if ((pstQRow == nullptr) || (dwQStart < dwQBestStart))
						{ pstQRow = &stQ; dwQBestStart = dwQStart; }
					}
				}

				if (pstQRow != nullptr)
				{
					const uint64 qwQEntryLink = strtoull(pstQRow->strFromId.c_str(), nullptr, 10);
					double dfQCarryDistM = 0.0;
					uint64 qwQCarryFromLinkID = 0;
					if ((qwQEntryLink != 0) && (stRun.qwLastLinkID != 0)
						&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr))
					{
						static const int MM_EXEMPT_EXIT_CARRY_MAX_HOPS2 = 6;
						vector<uint64> vtQPath;
						if (FindLinkPathBounded(stRun.qwLastLinkID, qwQEntryLink,
								MM_EXEMPT_EXIT_CARRY_MAX_HOPS2, &vtQPath) && (vtQPath.size() > 2))
						{
							bool bQLenOk = true;
							for (size_t g = 1; g + 1 < vtQPath.size(); ++g)
							{
								if (!m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtQPath[g]))
								{ bQLenOk = false; break; }
								PLINK_INFO pstQLink = m_stConfig.pcDataLoader->GetLinkInfo(vtQPath[g]);
								if (pstQLink == nullptr) { bQLenOk = false; break; }
								dfQCarryDistM += pstQLink->dfLen;
								if (qwQCarryFromLinkID == 0) qwQCarryFromLinkID = vtQPath[g];
							}
							if (!bQLenOk) { dfQCarryDistM = 0.0; qwQCarryFromLinkID = 0; }
						}
					}

					static const double MM_EXEMPT_EXIT_CARRY_MAX_GAP_M2 = 200.0;
					if ((dfQCarryDistM > 0.0) && (dfQCarryDistM <= MM_EXEMPT_EXIT_CARRY_MAX_GAP_M2))
					{
						// 거리·구간 시작 시각을 함께 늘린다 — 거리만 더하면 평균속도가 부풀려진다.
						//   OCCUR_DT 를 면제 진출 시각으로 앞당기고 STAY_SECONDS 를 그만큼 늘린 뒤
						//   SPEED_KMH 를 재계산한다(열린 run 경로가 dtEntryTime 을 앞당기는 것과 동일 효과).
						const long nQOldDist = atol(pstQRow->strDistM.c_str());
						const long nQOldStay = atol(pstQRow->strStaySeconds.c_str());
						const time_t dtQOldOccur = ParseDateTime14(pstQRow->strOccurDt);
						long nQNewStay = nQOldStay;
						if ((dtQOldOccur > 0) && (dtExemptEnd > 0) && (dtQOldOccur > dtExemptEnd))
							nQNewStay += static_cast<long>(dtQOldOccur - dtExemptEnd);

						const long nQNewDist = nQOldDist + static_cast<long>(dfQCarryDistM + 0.5);
						char szQBuf[32];
						snprintf(szQBuf, sizeof(szQBuf), "%ld", nQNewDist);
						pstQRow->strDistM = szQBuf;
						snprintf(szQBuf, sizeof(szQBuf), "%ld", nQNewStay);
						pstQRow->strStaySeconds = szQBuf;
						if (nQNewStay > 0)
						{
							snprintf(szQBuf, sizeof(szQBuf), "%d", static_cast<int>(
								(static_cast<double>(nQNewDist) / static_cast<double>(nQNewStay)) * 3.6 + 0.5));
							pstQRow->strSpeedKmh = szQBuf;
						}
						if (dtExemptEnd > 0)
							pstQRow->strOccurDt = FormatDateTime14(dtExemptEnd);
						// FROM_ID 는 복구 경로 첫 링크로 — 구역 링크로 덮지 않는다(중복 계상 방지,
						//   위 "열린 run" 경로와 동일 근거). START_GPS_SEQ 는 그대로 둔다.
						if (qwQCarryFromLinkID != 0)
						{
							snprintf(szQBuf, sizeof(szQBuf), "%llu",
								static_cast<unsigned long long>(qwQCarryFromLinkID));
							pstQRow->strFromId = szQBuf;
						}
						bAppliedToOpenRun = true;

						LOGFMTI("[#%02d] exempt exit carry applied to queued row!device=[%s] trip_id=[%s] "
							"road=[%s] row_start_seq=[%u] from_link=[%llu] gap=[%.1f]m dist=[%ld->%ld]m",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
							dwQBestStart, static_cast<unsigned long long>(qwQCarryFromLinkID),
							dfQCarryDistM, nQOldDist, nQNewDist);
					}
				}
			}

			if (bAppliedToOpenRun)
			{
				pstSession->vtExemptRuns.erase(pstSession->vtExemptRuns.begin() + si);
				continue;
			}

			pstSession->bHasGateExitCarry = true;
			pstSession->dfGateExitX = stRun.dfLastX;
			pstSession->dfGateExitY = stRun.dfLastY;
			pstSession->dtGateExit = dtExemptEnd;
			pstSession->dwGateExitGpsSeq = stRun.dwLastInZoneGpsSeq;
			pstSession->qwGateExitLinkID = stRun.qwLastLinkID;
			pstSession->bGateExitAtTick = false;

			LOGFMTI("[#%02d] exempt exit carry armed!device=[%s] trip_id=[%s] seq=[%u] road=[%s] "
				"last_link=[%llu] node=[%f,%f]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRun.dwLastInZoneGpsSeq, stRun.szRoadID,
				static_cast<unsigned long long>(stRun.qwLastLinkID), stRun.dfLastY, stRun.dfLastX);
		}

		pstSession->vtExemptRuns.erase(pstSession->vtExemptRuns.begin() + si);
	}

	// ── ② 새로 진입한 구역 세션 개시 ─────────────────────────────────────────
	if (bTripEnding)
		return;

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtExemptRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtExemptRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;

		ZONE_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';

		// 진입 경계 보정 — 구역에 들어온 링크의 시작 노드까지 거리·시각을 채운다. 개방형
		//   ProcessOpenGateCharge() 의 진입 보간과 동일 원리. 트립이 구역 안에서 시작한 경우
		//   (bTripStarting)는 겨냥할 "직전 구역 밖 tick" 자체가 없어 보정 대상이 아니며,
		//   그 출발좌표가 곧 진입좌표다 (사용자 지시, 2026-08-30 최정우 추가)
		const bool bTripStarting = (stRawLogInfo.nTripEvent == TRIP_EVENT_START);
		bool bHeadDone = false;
		if (!bTripStarting && pstSession->bHasLastMatch)
		{
			POINT stNode, stCur;
			stNode.dfX = stMatchLinkInfo.dfStNodeX;  stNode.dfY = stMatchLinkInfo.dfStNodeY;
			stCur.dfX = stMatchLinkInfo.dfMatchX;    stCur.dfY = stMatchLinkInfo.dfMatchY;

			double dfHead = HaversineMeters(stNode, stCur);
			if ((dfHead >= 0.0) && (dfHead <= stMatchLinkInfo.dfLen + 1.0))
			{
				stRun.dtEntryTime = InterpolateGateCrossingTime(
					pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
					stCur.dfX, stCur.dfY, stRawLogInfo.dtGPS,
					stNode.dfX, stNode.dfY);
				stRun.dfEntryX = stNode.dfX;
				stRun.dfEntryY = stNode.dfY;
				stRun.dfAccumDistM = dfHead;
				bHeadDone = true;
			}
		}
		if (!bHeadDone)
		{
			stRun.dtEntryTime = stRawLogInfo.dtGPS;
			stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
			stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
			stRun.dfAccumDistM = 0.0;
		}
		// 진입 경계와 직전 tick 이 사실상 같은 지점이면 순번을 공유한다 — 게이트형 진출 이월의
		//   bGateExitAtTick 과 같은 규칙(2m 이내), 사용자 지시("좌표가 진입·진출 동일하고 맵매칭
		//   좌표도 동일하면 진출·진입 순번이 동일할 수 있음")의 면제도로 적용분.
		//   면제는 진입 좌표·시각·거리를 모두 "구역 링크 시작노드"로 보정(위 bHeadDone)하므로,
		//   그 경계가 직전 tick 매칭점 위라면 레코드는 실제로 그 tick 에서 시작한 것이다. 그런데
		//   START_GPS_SEQ 만 이번 tick 으로 남아 앞 레코드와 한 칸 벌어졌다 — 실측
		//   000376_20260819140532: 일반도로가 링크 2040425102 끝까지(tail 0.3m) 세고 seq53 에서
		//   끝나는데 면제 RL-Z00015(링크 2040424701)는 54 로 시작해, 경계가 같은 점인데도
		//   53/54 로 갈라졌다. 진입 시각은 이미 보간으로 seq53 시각과 같았으므로 순번만 어긋난
		//   상태였다(체류 12초 = 14:08:15~14:08:27 로 검산). 정답은 53~57
		//   (2026-09-07 최정우 추가, 사용자 지적)
		// 되돌리는 법: 아래 if 블록을 지우면 종전대로 항상 이번 tick 순번을 쓴다
		stRun.dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
		if (bHeadDone && pstSession->bHasLastMatch && (pstSession->dwLastMatchGpsSeq != 0))
		{
			POINT stBoundP, stPrevP;
			stBoundP.dfX = stRun.dfEntryX;            stBoundP.dfY = stRun.dfEntryY;
			stPrevP.dfX = pstSession->dfLastMatchX;   stPrevP.dfY = pstSession->dfLastMatchY;
			if (HaversineMeters(stBoundP, stPrevP) <= 2.0)
				stRun.dwEntryGpsSeq = pstSession->dwLastMatchGpsSeq;
		}
		stRun.dfLastX = stMatchLinkInfo.dfMatchX;
		stRun.dfLastY = stMatchLinkInfo.dfMatchY;
		stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;			// 진입 tick 이 곧 구역 안 첫 tick
		stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
		// [2026-09-23 수정] 진입 tick 도 같은 원칙 — 매칭 링크가 아니라 **그 구역에 속한
		//   경로 링크**를 남긴다. 구역이 두 GPS 사이에 통째로 끼어 경로로만 잡힌 경우, 매칭 링크는
		//   구역 밖이라 이탈 보정이 엉뚱한 링크 길이를 더한다(실측 000993 seq417: 매칭 2040425801
		//   102m 가 그대로 RL-Z00018 거리가 됐다)
		stRun.qwLastLinkID = (vtZoneLinks[e] != 0) ? vtZoneLinks[e] : stMatchLinkInfo.qwLinkID;
		pstSession->vtExemptRuns.push_back(stRun);

		LOGFMTI("[#%02d] exempt zone entry!device=[%s] trip_id=[%s] road=[%s] open=[%zu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
			pstSession->vtExemptRuns.size());
	}
}

/**
 * @brief 일반도로 run 을 여는 tick 의 경로 링크를 그 run 의 커버 목록(vtRunLinks)에 넣는다
 *   (2026-10-04 최정우 추가 — 사용자 지적, 시나리오 980013_20261004230000)
 * @remark CommitPendingRow() 의 "열려 있는 일반도로 run 에도 같은 링크를 쌓는다" 블록은 과금 함수보다
 *   **먼저** 돌기 때문에, 이번 tick 에 새로 열린 run 은 이 tick 의 링크를 받지 못한다. 그러면 그 링크가
 *   트립 마감 때 FillUncoveredLinkRows() 에서 "어느 행도 안 덮은 링크"로 잡혀 **전체 길이**가 다시
 *   더해진다 — run 거리는 이미 이 tick 위치부터 누적하므로 링크 앞부분(실제로 안 달린 구간)이 과다
 *   계상된다. 실측: 실주행 미커버 흡수 13건 중 10건(2,172m)이 run 시작 tick 이었고,
 *   000376_20260819141002 trip_seq=1 은 seq1~4(12초)에 573m(172km/h)였다(+444.8m, 링크 3개).
 *   트립 첫 tick 은 직전 트립 앵커에서 이어진 경로 링크까지 담을 수 있는데, 그 구간도 이 트립에서
 *   달린 것이 아니므로 덮인 것으로 처리하는 것이 맞다.
 * 되돌리는 법: 두 호출부(run 생성 직전)를 지우면 종전 동작이다.
*/
static void AppendTickLinksToRun(vector<uint64> *pvtRunLinks, const MATCH_LINK_INFO& stMatchLinkInfo)
{
	if (pvtRunLinks == nullptr) return;
	if (stMatchLinkInfo.nPathLinkCount > 0)
	{
		for (uint8 pi = 0; pi < stMatchLinkInfo.nPathLinkCount; ++pi)
		{
			const uint64 qwL = stMatchLinkInfo.aqwPathLinkIDs[pi];
			if (qwL == 0) continue;
			if (!pvtRunLinks->empty() && (pvtRunLinks->back() == qwL)) continue;
			pvtRunLinks->push_back(qwL);
		}
	}
	else if (stMatchLinkInfo.qwLinkID != 0)
	{
		if (pvtRunLinks->empty() || (pvtRunLinks->back() != stMatchLinkInfo.qwLinkID))
			pvtRunLinks->push_back(stMatchLinkInfo.qwLinkID);
	}
}

/**
 * @brief 일반도로(ROAD_KIND=0, NODE_STEP) 진입/이탈 판정 (2026-08-14 최정우 추가)
 * @remark 비과금도로와 동일 구조 — 게이트가 없어 매칭 링크 ID로 구역 소속 여부를 직접 조회
 *   (GetNodeStepZoneByLinkId, ChargeDataLoader가 link_ids로 미리 구성해둔 역인덱스). 비과금도로와
 *   달리 실제 과금 대상이라, 이탈(다른 링크로 이동) 또는 트립 종료 시 항상 `charge_yn='Y'`/
 *   `charge_status='0'`(정상)으로 1건 기록 — 구역 진입 시점부터 이탈까지 누적거리를 dist_m 으로
 *   적재(사용자 지시, 2026-08-14 — 다른 유형과 겹쳐도 무조건 별도 부과, 폐쇄형/비과금도로와 동일
 *   패턴 재사용). 진행 중 GPS가 끊겨 세션이 TTL로 강제 마감되는 경우는
 *   AppendExpiredNodeStepCharge() 가 별도 INSERT 처리.
 * @remark 컬럼 매핑(사용자 지시, 2026-08-14) — 다른 4유형과 다른 점 위주:
 *   `charge_unit='0'`(NODE, 개방형과 동일 관례 — LINE 판정이지만 단위는 NODE로 지시받음),
 *   `occur_dt`=**진출 시각**(다른 5유형은 전부 "진입 시각"이라 헷갈리기 쉬움 — 일반도로만 예외),
 *   `upd_dt`=occur_dt와 동일(진출 시각, `reg_dt`=INSERT 실행 wall-clock 시각과는 다름),
 *   `trip_end_dt`는 이 함수에서 직접 안 채움 — TRIP_EVENT=2(트립 정상종료) 시 전 유형 공용
 *   `[trip_end]` UPDATE 가 별도로 채움(사용자 지시의 "운행 정상 종료 시 종료 시각"과 동일 결과,
 *   기존 공용 메커니즘 재사용).
 *
 * @remark **이 함수가 만드는 일반도로 행은 세 갈래다** (2026-09-23 최정우 정리 — 읽는 순서 안내)
 *   ① 정규 run 마감 — vtNodeStepRuns 의 run 이 이탈·트립종료로 닫힐 때(BuildNodeStepRow).
 *      대부분의 행이 여기서 나온다.
 *   ② 이월 확정 — 병합 이월(stMergeCarry)이 다른 과금유형 tick 을 만나 더는 이어붙일 수 없을 때
 *      그 자리에서 단독 행으로 확정한다("merge carry recorded(other charge type)").
 *   ③ **게이트 진출 orphan span 복구** — 게이트형 구역을 진출한 직후(2 tick 이내) 곧바로 다른
 *      과금유형에 진입해 게이트 진출 이월이 폐기될 때, 그 사이의 **어느 구역에도 속하지 않는
 *      연결 링크**를 FindLinkPathBounded(주행방향 그래프)로 찾아 독립 행으로 만든다
 *      (BuildNodeStepRowFromLinkRange). 이 구간에는 실측 tick 이 하나도 없으므로 —
 *        · 거리는 링크 기하, 체류시간은 **실측 속도로 역산**(게이트 통과 시각이 보간값이라
 *          그대로 쓰면 37m/1초=133km/h 같은 값이 나온다),
 *        · GPS_SEQ 는 **이번 tick 하나로 고정**(범위를 넓히면 앞 일반도로 행과 같은 유형끼리
 *          겹친다 — 실측 +11쌍), 달린 구간은 FROM_ID~TO_ID 로 표현한다,
 *        · bNoMergeAfter 로 뒤 행과의 병합을 막는다.
 *      실측 000995_20260904162440 seq523→524 의 36.9m(2520231403→2520655702→2520231304).
*/
void CRawLogWorker::ProcessNodeStepCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd, bool bTrustedMatch)
{
	// E18 — SKIP 복구 버퍼(매칭 실패 SKIP)에 폴리곤 안 좌표가 있는지. RAW_VLD·정확도 SKIP 은 dwSkipInParkSeq 로 본다.
	const bool bSkipBufInPark = !pstSession->vtSkipRunRawLogInfo.empty() && IsSkipGapInParkingZone(pstSession);
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경유 링크 포함 전체 경로에서 일반도로 구역을 "전부" 수집한다. 한 링크가 여러 구역에 속할 수
	//   있어(인접 구역의 경계 링크 등) 복수 조회 API 를 쓴다 (2026-08-23 최정우 수정)
	vector<PZONE_INFO> vtZones;
	{
		vector<PZONE_INFO> vtOne;
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
			m_stConfig.pcChargeDataLoader->GetNodeStepZonesByLinkId(stMatchLinkInfo.qwLinkID, &vtOne);
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
				m_stConfig.pcChargeDataLoader->GetNodeStepZonesByLinkId(
					stMatchLinkInfo.aqwPathLinkIDs[i], &vtOne);
		}
		for (size_t i = 0; i < vtOne.size(); ++i)			// road_id 기준 중복 제거
		{
			bool bDup = false;
			for (size_t e = 0; e < vtZones.size(); ++e)
			{
				if (strcmp(vtZones[e]->szRoadID, vtOne[i]->szRoadID) == 0) { bDup = true; break; }
			}
			if (!bDup) vtZones.push_back(vtOne[i]);
		}
	}

	// 일반도로 연속 구간 병합용 이월 슬롯 — ①에서 run이 닫힐 때 "받아줄" 다른 run이 아직
	//   없으면(등록 없이 새로 열리는 ②보다 먼저 닫히는 극단적 타이밍) 여기 잠깐 담아뒀다가
	//   ②에서 새 run을 열 때 그대로 이어받는다(사용자 지시, 2026-09-01 최정우 추가).
	//   아래 주정차 유예(park_exitcnt) 확정 시 소급 진입점도 동일 메커니즘으로 흘려보내므로
	//   선언을 여기로 앞당김(2026-09-03 최정우 수정 — 원래 ①·② 사이에 있었음)
	// [버그 수정, 2026-09-10 최정우] 원래 이 함수의 지역변수였는데, 2026-09-07에 추가된
	//   "!bTrustedMatch면 새 run을 안 연다" 가드(미등록 도로 개시 분기)가 이 지역변수의 유일한
	//   소비 지점(② 새 run 개시부)보다 먼저 return 해버려, 신뢰 못하는 tick을 만나면 이미 쌓아둔
	//   이월값이 그대로 소멸했다(함수가 매 tick 새로 호출되므로 다음 tick엔 false로 초기화된
	//   새 지역변수만 존재) — 최소 재현으로 87.3m 규모 이월값 유실을 확인. 게이트 이월
	//   (pstSession->bHasGateExitCarry, 바로 아래)과 똑같이 세션 필드로 승격해, 신뢰 못하는
	//   tick을 만나도 없어지지 않고 나중에 신뢰 가능한 tick이 올 때까지 남아있게 한다. 세션
	//   필드라 여기서 매 tick 초기화하면 안 됨 — 생성자에서만 초기화(RawLogWorker.h 참고),
	//   이 함수 안에서는 그대로 이어받아 쓴다.

	// 주정차(PARKING, road_kind=4) 폴리곤 안이면 등록/미등록 여부와 무관하게 일반도로 자체를
	//   표출하지 않는다 — 원시좌표 기준, 버퍼 없이 순수 폴리곤 경계로만 판단한다(사용자 지시,
	//   2026-09-02 최정우 수정 — park_pad 적용판은 시스템 전체 14개 주정차 구역 인근 도로가
	//   대거 영향받아 의도한 범위를 벗어남 확인, 버퍼 없는 순수 좌표 판정으로 축소).
	//   PARKING 자체의 이탈 디바운스(park_exitcnt)와 동일 기준을 적용 — 폴리곤 밖으로 짧게
	//   튄 tick(park_exitcnt 미만 연속)은 여전히 "주정차 취급" 유지. 디바운스 없이 즉시
	//   판정하면 PARKING은 노이즈로 흡수해 세션을 안 끊는데 NODE_STEP만 그 사이를 별도
	//   레코드로 표출해버리는 불일치가 생김(사용자 지시, 2026-09-02 최정우 추가 — 실측
	//   000376_20260826150010 seq158~159)
	bool bRawInParkingZoneNow = false;
	const char *pszRawParkZoneRoadId = "";		// 접촉 시작 시 szParkTouchZoneRoadId 채우는 용도 — 이번 tick에만 유효
	{
		vector<PZONE_INFO> vtParkCheck;
		m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(
			stRawLogInfo.dfX, stRawLogInfo.dfY, 0.0, &vtParkCheck);
		bRawInParkingZoneNow = !vtParkCheck.empty();
		if (bRawInParkingZoneNow)
			pszRawParkZoneRoadId = vtParkCheck[0]->szRoadID;
	}
	// 접촉 확정 판정용 — 반드시 매칭좌표 기준(원시좌표 기준 아님). 판교 실측
	//   000376_20260819094414 seq55,56은 원시좌표는 폴리곤 안이지만 매칭좌표는 계속 밖이라,
	//   원시좌표로 판정하면 접근로만 스친 오검출까지 "확정 접촉"으로 잘못 분류된다
	//   (사용자 지시, 2026-09-03 최정우 추가)
	bool bMatchInParkingZoneNow = false;
	const char *pszMatchParkZoneRoadId = "";	// 억제가 매칭 기준이 되면서 szParkTouchZoneRoadId 의
												//   1순위 출처 — 원시가 밖인 tick 은 원시쪽이 비어 있다
												//   (2026-09-06 최정우 추가)
	{
		vector<PZONE_INFO> vtParkCheckMatch;
		m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(
			stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, 0.0, &vtParkCheckMatch);
		bMatchInParkingZoneNow = !vtParkCheckMatch.empty();
		if (bMatchInParkingZoneNow)
			pszMatchParkZoneRoadId = vtParkCheckMatch[0]->szRoadID;
	}
	// ── 주정차 폴리곤 판정: 매칭좌표 단독 기준 (사용자 지시, 2026-09-06 최정우 수정) ──
	//   두 지시를 합치면 결과적으로 매칭좌표 하나가 판정을 결정한다:
	//     · 원시 안 + 매칭 안 -> 주정차(억제)
	//     · 원시 밖 + 매칭 안 -> **주정차로 인정**(억제) — 원시가 폴리곤을 살짝 벗어나도 실제로
	//       달린(매칭된) 도로가 폴리곤 안이면 그건 폴리곤 안 주정차 도로다
	//     · 원시 안 + 매칭 밖 -> 일반도로로 포함 — 실제 달린 도로가 폴리곤 밖이면 주정차가 아니다
	//     · 원시 밖 + 매칭 밖 -> 일반도로
	//   즉 "매칭좌표가 폴리곤 안이면 억제, 밖이면 포함"이며 원시좌표는 판정에서 빠진다.
	//   이는 원시좌표를 기준으로 하던 2026-09-02 지시를 **대체**한다 — 그 지시의 취지(park_pad
	//   버퍼를 적용하면 14개 주정차 구역 인근 도로가 대거 영향받으므로 버퍼 없이 순수 폴리곤
	//   경계로만 판단)는 그대로 유지된다. 버퍼는 여전히 0.0 이고, 바뀐 것은 "어느 좌표를 폴리곤에
	//   넣어보는가"뿐이다. 실측 000376_20260821094609 — seq14·17·18(원시 밖·매칭 안)이 이제
	//   주정차로 인정돼 일반도로 28m 레코드가 사라지고, seq10(원시 안·매칭 밖)은 일반도로로 포함된다.
	//   bMatchInParkingZoneNow 는 원래 "접촉 확정" 판정용으로 도입된 값이라(2026-09-03, 접근로만
	//   스친 오검출 방지) 같은 근거가 억제 판정에도 그대로 적용된다.
	// [2026-09-16 검토·원복 — 같은 제안을 다시 하지 말 것] "폴리곤이 일반 도로를 가로질러 덮고
	//   있으니 5km/h 초과 통과 주행은 일반도로로 표출하자"는 속도 예외를 넣었다가 **되돌렸다**.
	//   ① 규칙은 "매칭좌표가 폴리곤 안이면 주정차"이고 예외는 아래 표의 "원시 안 + 매칭 밖" 한 줄
	//      뿐이다(사용자 재확인, 2026-09-16). 속도는 이 판정에 들어가지 않는다.
	//   ② 실제로도 깨진다 — 폴리곤에 **진입해서 주차하는 차도 진입 순간에는 주행 속도**라
	//      (실측 000376_20260826150010 seq121·128: 19km/h·13km/h) 그 tick 을 통과로 보면 열려 있던
	//      일반도로 run 이 닫히지 않고, 체류(seq131~405) 뒤 새 run 이 또 열려 **Y/0 구간이 겹친다**.
	//      A/B 실측: 예외 OFF 163행/180,219m/중복 0쌍 vs 예외 ON 171행/184,452m/**중복 5쌍**.
	//      진입 tick 하나만 보고 "통과"와 "주차"를 구분할 방법이 없다(이후 tick 을 봐야 안다).
	bool bInParkingZone;
	if (bMatchInParkingZoneNow)
	{
		bInParkingZone = true;
		pstSession->nNodeStepParkExitTicks = 0;
		pstSession->bNodeStepParkTouch = true;
	}
	else if (pstSession->bNodeStepParkTouch)
	{
		pstSession->nNodeStepParkExitTicks += 1;
		if (pstSession->nNodeStepParkExitTicks < m_stConfig.nParkExitCnt)
			bInParkingZone = true;					// 디바운스 유예 중 — 여전히 주정차 취급
		else
		{
			bInParkingZone = false;
			pstSession->bNodeStepParkTouch = false;	// park_exitcnt 회 연속 확인 — 이탈 확정
		}
	}
	else
	{
		bInParkingZone = false;
	}
	// [2026-10-04 최정우 수정, 사용자 확정 — 정책 E16] **주정차 폴리곤 안이라도 일반도로로 판정되면 일반도로로
	//   등록한다.** 종전에는 폴리곤 안(매칭좌표 기준) 동안 일반도로 run 을 억제·보류하고, 주행해서 빠져나온
	//   경우만(B2) 안쪽을 마지막 안쪽 tick 까지 따로 등록했으며, 폴리곤 안에서 트립이 끝나면(B4) 경계까지만
	//   등록했다. 이제 억제 자체를 끄므로 일반도로 run 은 폴리곤을 의식하지 않고 다른 도로와 똑같이 이어진다
	//   (안에서 끝나는 트립도 마지막 tick 까지). 주정차 판정·등록(ProcessParkingCharge)은 그대로이고 두 행은
	//   GPS_SEQ 범위가 겹쳐도 된다(B6). 접촉 버퍼·폴리곤 통과(B2)·진출 경계 이월 블록은 bInParkingZone 이
	//   항상 false 라 동작하지 않는다(코드는 되돌리기용으로 남김). 정지 tick 표류 제외는 아래 누적부에 옮겼다.
	// 되돌리는 법: MM_PARK_SUPPRESS_NODESTEP 을 true 로(FillUncoveredLinkRows 의 폴리곤 제외도 같이 살아난다).
	if (!MM_PARK_SUPPRESS_NODESTEP)
	{
		bInParkingZone = false;
		pstSession->bNodeStepParkTouch = false;
		pstSession->nNodeStepParkExitTicks = 0;
	}

	// 구간단속 마감 시 보류해둔 일반도로 미러 — 이번 tick에 접촉 자체가 없으면(또는 접촉이 이미
	//   끝났으면) 더는 인수인계 구간을 기다릴 이유가 없다. 아래 인수인계 로직은 접촉 중일 때만
	//   실행되므로, 접촉이 아예 없는 tick에서 여기서 그대로 원래 값으로 등록해 흘려버린다
	//   (2026-09-03 최정우 추가)
	// 미러를 보류한 그 tick 에서는 등록하지 않는다 — 구간단속 출구 통과 tick 과 주정차 폴리곤
	//   진입 tick 이 한 칸 어긋나는 경우(실측 000376_20260821095239: seq11 에서 출구 TG00013
	//   통과·미러 생성, seq12 에서 폴리곤 진입), 같은 tick 에 등록해버리면 다음 tick 의
	//   인수인계 로직이 이어받을 미러가 없어 누락 링크 구간이 별도 레코드로 쪼개진다.
	//   최소 1 tick 을 들고 있다가, 접촉이 없으면 그때 원래 값 그대로 등록한다. 보류분이
	//   유실될 일은 없다 — TTL 만료·세션 종료·트립 종료 3경로가 모두 흘려보낸다
	//   (2026-09-06 최정우 수정, 사용자 지시)
	// [버그 수정, 2026-09-11 최정우 — 사용자 지시] 여기서 곧장 bHasMergeCarry 로 변환하면, 그
	//   변환이 "② 새 run 개시"(같은 tick, 이 지점보다 한참 아래)보다 먼저 실행되긴 하지만, 1틱
	//   지연 커밋 + 사후 브릿지 보정이 겹치면 실제로는 ②가 이미 다른 tick에서 먼저 새 run을 열어
	//   버린 뒤에야 이 변환이 뒤늦게 일어나는 순서가 실측으로 확인됐다(000370_20260911141637 —
	//   미러가 진입정보를 못 넘기고 새 run 자신의(더 늦은) 진입값으로 굳어짐). 타이밍에 기대지
	//   않도록 이 지점에서는 상태만 유지하고, 실제 변환·소비는 "② 새 run 개시" 바로 앞(같은 호출
	//   안에서 새 run 생성 직전)으로 옮겨 타이밍 격차 자체를 없앤다 — 아래 참고.

	// 주정차 접촉(폴리곤 직접 접촉이든 이탈 디바운스 유예든 사유 불문) 구간의 소급 버퍼링 —
	//   "구역과 접촉해 NODE_STEP이 억제된 tick"을 전부 이 버퍼에 쌓아뒀다가, 이탈이 "확정"되는
	//   순간(bInParkingZone이 true→false로 바뀌는 tick) 그 구간 전체를 소급해서 새 NODE_STEP
	//   run의 진입점으로 되돌린다. 최초 구현 시 "폴리곤 직접 접촉"(bRawInParkingZoneNow=true)
	//   tick은 버퍼링 없이 그냥 매번 폐기(bHasParkTouchCarry=false)하고, "이탈 디바운스 유예"
	//   tick만 버퍼링했었는데 — 그러면 직접 접촉 구간(예: 폴리곤 5m 옆을 스쳐 지나가며 raw가
	//   버퍼에 바로 걸리는 tick들)이 통째로 유실되고, 그 뒤에 이어지는 유예 tick부터만 살아남아
	//   NODE_STEP 레코드가 엉뚱하게 쪼개졌다(실측 000376_20260819094414 seq54~61 — 54는 단독
	//   1틱 레코드, 55·56 유실, 57부터 별도 레코드로 소급). 상태(접촉/유예)와 무관하게
	//   bInParkingZone인 매 tick을 균일하게 버퍼링하도록 수정 (2026-09-03 최정우 수정)
	if (bInParkingZone)
	{
		if (!pstSession->bHasParkTouchCarry)
		{
			pstSession->stParkTouchCarry.dtEntryTime = stRawLogInfo.dtGPS;
			pstSession->stParkTouchCarry.dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
			pstSession->stParkTouchCarry.dfEntryX = stMatchLinkInfo.dfMatchX;
			pstSession->stParkTouchCarry.dfEntryY = stMatchLinkInfo.dfMatchY;
			pstSession->stParkTouchCarry.qwEntryLinkID = stMatchLinkInfo.qwLinkID;
			pstSession->stParkTouchCarry.dfAccumDistM = 0.0;
			pstSession->stParkTouchCarry.dfLastX = stMatchLinkInfo.dfMatchX;
			pstSession->stParkTouchCarry.dfLastY = stMatchLinkInfo.dfMatchY;
			pstSession->bHasParkTouchCarry = true;
			pstSession->bHasHandoffGapChecked = false;
			// 억제가 매칭좌표 기준이 됐으므로 구역 ID도 매칭쪽을 1순위로 쓴다 — 원시가 폴리곤
			//   밖인 tick 은 pszRawParkZoneRoadId 가 비어 있어, 그대로 두면 인수인계 로직이
			//   GetZoneByRoadId("") 로 구역을 못 찾는다 (2026-09-06 최정우 수정)
			const char *pszTouchZoneRoadId = (pszMatchParkZoneRoadId[0] != '\0')
				? pszMatchParkZoneRoadId : pszRawParkZoneRoadId;
			strncpy(pstSession->szParkTouchZoneRoadId, pszTouchZoneRoadId,
				sizeof(pstSession->szParkTouchZoneRoadId) - 1);
			pstSession->szParkTouchZoneRoadId[sizeof(pstSession->szParkTouchZoneRoadId) - 1] = '\0';
		}
		else
		{
			POINT stPrev, stCur;
			stPrev.dfX = pstSession->stParkTouchCarry.dfLastX;
			stPrev.dfY = pstSession->stParkTouchCarry.dfLastY;
			stCur.dfX = stMatchLinkInfo.dfMatchX;
			stCur.dfY = stMatchLinkInfo.dfMatchY;
			// [2026-10-04 최정우 추가 — 정책 대조표 C3] 이 접촉 구간 거리는 이탈 시 일반도로로 넘어가므로
			//   (아래 "확정 이탈" 블록), 정지 tick 의 GPS 표류가 거리로 둔갑하지 않게 막는다 — 속도<1
			//   이거나 직전과 원시좌표·방향이 같으면 누적하지 않고 기준점만 옮긴다.
			const bool bTouchStill = stMatchLinkInfo.bSameRawAndHeadingAsPrev
				|| ((stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed < 1.0f));
			if (!bTouchStill)
				pstSession->stParkTouchCarry.dfAccumDistM += HaversineMeters(stPrev, stCur);
			pstSession->stParkTouchCarry.dfLastX = stMatchLinkInfo.dfMatchX;
			pstSession->stParkTouchCarry.dfLastY = stMatchLinkInfo.dfMatchY;
		}

		// 인수인계 구간(handoff gap) — 이 접촉을 받아줄 일반도로 run이 하나도 열려있지 않은 채
		//   시작되는 경우(예: 구간단속처럼 다른 과금유형이 이 링크까지 등록하다가 끝나고, 바로
		//   다음 링크에서 곧장 주정차 폴리곤에 닿는 경우 — 그 사이엔 "미등록" 판정이 성립하는
		//   틱이 단 한 번도 없어 run 자체가 열릴 기회가 없었음). 마지막으로 신뢰 확정됐던 링크
		//   (qwLastConfirmedLinkID, 과금유형과 무관하게 매 tick 갱신되는 범용 앵커)부터 누락 링크를
		//   복구해, 폴리곤과 교차하는 지점까지만 별도의 작은 일반도로 레코드로 즉시 등록한다.
		//   접촉 시작 tick 자체엔 아직 링크가 안 바뀌어 있을 수 있어(예: 구간단속 마지막 tick과
		//   접촉 시작 tick이 같은 링크) 접촉이 이어지는 동안 매 tick 재시도하다가, 링크가 실제로
		//   바뀌는 tick에서 딱 한 번만 시도한다(사용자 지시, 2026-09-03 최정우 추가 — 실측
		//   000376_20260819094414 seq93(구간단속 마지막 링크 2040424301, 아직 미분기)→94(링크
		//   2040425801 로 전환, 이때 비로소 누락 링크 2040424302 에서 폴리곤이 갈림))
		if (!pstSession->bHasHandoffGapChecked && pstSession->vtNodeStepRuns.empty()
			&& pstSession->bHasLastMatch && (pstSession->qwLastConfirmedLinkID != 0)
			&& (pstSession->qwLastConfirmedLinkID != stMatchLinkInfo.qwLinkID)
			&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr))
		{
			pstSession->bHasHandoffGapChecked = true;
			PZONE_INFO pstTouchZone =
				m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szParkTouchZoneRoadId);
			if (pstTouchZone != nullptr)
			{
				static const int MM_NODE_STEP_HANDOFF_GAP_MAX_HOPS = 6;
				vector<uint64> vtGapPath;
				// 경로 길이 2(=직전 링크와 이번 링크가 바로 인접, 중간 누락 링크 없음)도 대상에 넣고,
				//   탐색 범위도 마지막 링크(=이번 tick 의 매칭 링크)까지 넓힌다 — 폴리곤이 갈리는
				//   지점이 "누락된 중간 링크" 위에 있을 때만 잡던 것을, 바로 다음 링크 위에서 갈리는
				//   경우까지 포함하기 위함이다. 중간 링크를 먼저 훑고 첫 교차에서 멈추는 순서는 그대로라
				//   기존에 잡히던 케이스의 결과는 바뀌지 않는다(실측 000376_20260819094414 seq82~93 —
				//   2040424301→[2040424302]→2040425801 은 종전대로 중간 링크에서 교차 확정).
				//   실측 000376_20260821095239 seq11(2040424301)→seq12(2040424302): 두 링크가 인접해
				//   경로 길이가 2 라 종전 조건(>2)에서 통째로 걸러졌고, 그 결과 폴리곤 안 seq12~41
				//   30점이 주정차(park_entrycnt 미충족으로 미개시)·일반도로(폴리곤 안이라 억제) 어느
				//   쪽에도 안 잡혀 과금에서 사라졌다 (2026-09-06 최정우 수정, 사용자 지시)
				if (FindLinkPathBounded(pstSession->qwLastConfirmedLinkID, stMatchLinkInfo.qwLinkID,
						MM_NODE_STEP_HANDOFF_GAP_MAX_HOPS, &vtGapPath) && (vtGapPath.size() >= 2))
				{
					double dfGapDistM = 0.0;
					double dfCrossX = 0.0, dfCrossY = 0.0;
					uint64 qwCrossLinkID = 0;
					// 이 구간에서 "거리가 실제로 발생하기 시작한" 첫 링크 — FROM_ID 용.
					//   누적은 vtGapPath[1] 부터 시작한다(vtGapPath[0] 는 직전 확정 링크로,
					//   그 위의 남은 거리는 세지 않는다). 그래서 qwLastConfirmedLinkID 를 그대로
					//   FROM_ID 로 쓰면 "거리 기여가 0인 링크"가 출발 링크로 적혀, 그 링크를
					//   통째로 지난 것처럼 오해된다 — 실측 000376_20260819093337 seq106:
					//   매칭점이 2040424801(63.7m) 의 끝 노드에서 0.46m 지점이라 실제 6m 는 전부
					//   다음 링크 2040424803(9.0m, 이 링크 중간에서 주정차 폴리곤이 갈림) 위인데
					//   FROM_ID 가 2040424801 로 찍혀 63.7m 링크를 지난 것처럼 보였다. 올바른
					//   표기는 2040424803 → 2040424803 이다 (2026-09-06 최정우 수정, 사용자 지시)
					uint64 qwFirstDistLinkID = 0;
					bool bAllLenOk = true;
					for (size_t g = 1; (g < vtGapPath.size()) && (qwCrossLinkID == 0); ++g)
					{
						double dfPartial = 0.0;
						if (FindLinkPolygonCrossing(vtGapPath[g], pstTouchZone->vtCoords,
								&dfPartial, &dfCrossX, &dfCrossY))
						{
							dfGapDistM += dfPartial;
							qwCrossLinkID = vtGapPath[g];
							if ((qwFirstDistLinkID == 0) && (dfPartial > 0.0))
								qwFirstDistLinkID = vtGapPath[g];
						}
						else
						{
							PLINK_INFO pstGapLink = m_stConfig.pcDataLoader->GetLinkInfo(vtGapPath[g]);
							if (pstGapLink == nullptr) { bAllLenOk = false; break; }
							dfGapDistM += pstGapLink->dfLen;
							if ((qwFirstDistLinkID == 0) && (pstGapLink->dfLen > 0.0))
								qwFirstDistLinkID = vtGapPath[g];
						}
					}

					if (bAllLenOk && (qwCrossLinkID != 0) && (dfGapDistM > 0.0))
					{
						ZONE_RUN_SESSION stHandoff;
						stHandoff.dtEntryTime = pstSession->dtLastConfirmedLinkTime;
						stHandoff.dwEntryGpsSeq = pstSession->dwLastConfirmedLinkGpsSeq;
						stHandoff.dfEntryX = pstSession->dfLastMatchX;
						stHandoff.dfEntryY = pstSession->dfLastMatchY;
						// FROM_ID — 거리 누적이 실제로 시작된 링크를 쓴다(위 qwFirstDistLinkID 주석 참고).
						//   못 찾았으면 종전대로 직전 확정 링크로 폴백한다. FROM_LAT/LON 은 실제로
						//   관측된 마지막 매칭 좌표를 그대로 둔다 — 링크 시작 노드로 바꾸면 실측이
						//   아닌 값이 되고, 둘의 차이는 세지 않는 잔여 구간(이번 실측 0.46m)뿐이다
						//   (2026-09-06 최정우 수정, 사용자 지시)
						stHandoff.qwEntryLinkID = (qwFirstDistLinkID != 0)
							? qwFirstDistLinkID : pstSession->qwLastConfirmedLinkID;
						stHandoff.dfAccumDistM = dfGapDistM;
						stHandoff.qwLastLinkID = qwCrossLinkID;
						stHandoff.dfLastX = dfCrossX;
						stHandoff.dfLastY = dfCrossY;
						// end_gps_seq/occur_dt — 교차점을 실제로 찾아낸 이번 tick(예: seq94, 이미
						//   폴리곤 안쪽으로 확정된 tick) 이 아니라 "마지막 확정 링크" tick(예: seq93,
						//   실제 GPS로 확인된 마지막 일반도로 지점)을 써야 한다 — 누락 링크와 교차점은
						//   그 사이를 지오메트리로 보완한 결과일 뿐, 실측 경계는 여전히 93이다
						//   (사용자 지시, 2026-09-03 최정우 수정)
						stHandoff.dtLastInZoneTime = pstSession->dtLastConfirmedLinkTime;
						stHandoff.dwLastInZoneGpsSeq = pstSession->dwLastConfirmedLinkGpsSeq;

						// 구간단속 등록은 그대로(현재처럼) — 그 미러로 보류해둔 일반도로 레코드가
						//   있고 이 인수인계 구간과 바로 이어지면(미러의 마지막 링크 == 이번 탐색의
						//   출발 링크), 별도 레코드로 쪼개지 않고 미러의 진입정보·누적거리를 그대로
						//   이어받아 하나로 합친다(사용자 지시, 2026-09-03 최정우 추가)
						if (pstSession->bHasHeldSpeedMirrorRun
							&& (pstSession->stHeldSpeedMirrorRun.qwLastLinkID == pstSession->qwLastConfirmedLinkID))
						{
							stHandoff.dtEntryTime = pstSession->stHeldSpeedMirrorRun.dtEntryTime;
							stHandoff.dwEntryGpsSeq = pstSession->stHeldSpeedMirrorRun.dwEntryGpsSeq;
							stHandoff.dfEntryX = pstSession->stHeldSpeedMirrorRun.dfEntryX;
							stHandoff.dfEntryY = pstSession->stHeldSpeedMirrorRun.dfEntryY;
							stHandoff.qwEntryLinkID = pstSession->stHeldSpeedMirrorRun.qwEntryLinkID;
							stHandoff.dfAccumDistM += pstSession->stHeldSpeedMirrorRun.dfAccumDistM;
							pstSession->bHasHeldSpeedMirrorRun = false;
						}

						CHARGE_INSERT_ROW stHandoffRow;
						BuildNodeStepRow(stHandoff, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
							pstSession->nChargeSeq, stHandoff.dtLastInZoneTime,
							stHandoff.dwLastInZoneGpsSeq, "Y", "0", &stHandoffRow);

						// speed_kmh — 이 레코드는 dtEnd를 dtEntryTime과 동일값(마지막 실측 tick, 158 등
						//   qwLastConfirmedLinkID 확정 시점)으로 넘기므로 BuildNodeStepRow 내부의
						//   dist/elapsedSec 평균속도 계산이 elapsedSec=0 으로 항상 성립하지 않는다
						//   (공백으로 남음). 대신 "그 확정 tick(158) 자체가" 기기로부터 보고받은
						//   순간속도(fLastConfirmedLinkSpeed)를 쓴다 — stRawLogInfo.fSpeed 를 그대로
						//   쓰면 안 됨: 이 코드가 실행되는 시점의 stRawLogInfo 는 158이 아니라 그 뒤
						//   역행의심 스트릭이 풀리며 지금 막 커밋되는 더 나중 tick 이라 값이 다른
						//   tick 걸로 새 버린다(실측 000376/000382 강릉 두 트립 모두 158/435 이 아닌
						//   160/437(둘 다 24km/h)로 잘못 채워지는 걸로 확인). 0으로 보고됐거나(정차 등)
						//   값이 없는 경우(-1)만 나눗셈 목적의 최소값 1로 보정한다(사용자 지시,
						//   2026-09-04 최정우 추가)
						{
							int nAnchorSpeed = (pstSession->fLastConfirmedLinkSpeed > 0.0f)
								? static_cast<int>(pstSession->fLastConfirmedLinkSpeed + 0.5f) : 1;
							char szHandoffSpeedKmh[16];
							snprintf(szHandoffSpeedKmh, sizeof(szHandoffSpeedKmh), "%d", nAnchorSpeed);
							stHandoffRow.strSpeedKmh = szHandoffSpeedKmh;
						}

						// 시작·종료 tick 이 같아 경과시간이 0 인 경우, 위에서 채운 순간속도로
						//   소요시간을 역산해 거리·시간·속도를 정합시킨다 — 폴리곤 경계 절단
						//   레코드와 동일 규칙(그쪽 주석 참고). 실측 000376_20260819093337
						//   trip_seq=5: 6m·27km/h 인데 체류가 0초로 비어 있었다
						//   (2026-09-06 최정우 추가, 사용자 지시)
						if (stHandoffRow.strStaySeconds.empty() || (atol(stHandoffRow.strStaySeconds.c_str()) <= 0))
						{
							const double dfSpdKmh = atof(stHandoffRow.strSpeedKmh.c_str());
							const double dfDistM = atof(stHandoffRow.strDistM.c_str());
							if ((dfSpdKmh > 0.0) && (dfDistM > 0.0))
							{
								int nStaySec = static_cast<int>((dfDistM / (dfSpdKmh / 3.6)) + 0.5);
								if (nStaySec < 1) nStaySec = 1;
								char szStay[16];
								snprintf(szStay, sizeof(szStay), "%d", nStaySec);
								stHandoffRow.strStaySeconds = szStay;
							}
						}
						pvtChargeInserts->push_back(stHandoffRow);
						pstSession->nChargeSeq += 1;

						LOGFMTI("[#%02d] node step handoff gap crossing corrected!device=[%s] "
							"trip_id=[%s] zone=[%s] cross_link=[%llu] gap_dist=[%.1f]m",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
							pstSession->szParkTouchZoneRoadId,
							static_cast<unsigned long long>(qwCrossLinkID), dfGapDistM);
					}
				}
			}

			// 위에서 인수인계 탐색을 시도했지만(bHasHandoffGapChecked=true) 병합에 실패했으면(경로를
			//   못 찾았거나 폴리곤 교차점을 못 찾음), 더는 재시도 기회가 없으므로 보류된 미러를
			//   원래 값 그대로 지금 등록한다(2026-09-03 최정우 추가)
			if (pstSession->bHasHandoffGapChecked && pstSession->bHasHeldSpeedMirrorRun)
			{
				CHARGE_INSERT_ROW stMirrorRow;
				BuildNodeStepRow(pstSession->stHeldSpeedMirrorRun, stRawLogInfo.szTripID,
					stRawLogInfo.szDeviceKey, pstSession->nChargeSeq,
					pstSession->stHeldSpeedMirrorRun.dtLastInZoneTime,
					pstSession->stHeldSpeedMirrorRun.dwLastInZoneGpsSeq, "Y", "0", &stMirrorRow);
				pvtChargeInserts->push_back(stMirrorRow);
				pstSession->nChargeSeq += 1;
				pstSession->bHasHeldSpeedMirrorRun = false;
			}
		}

		// dtLastInZoneTime/dwLastInZoneGpsSeq는 접촉 중인 매 tick마다 전진시킨다 — 초기화
		//   시점 값에 고정해두면 end_gps_seq가 실제보다 앞선 값으로 잘못 나온다(2026-09-03
		//   최정우 수정 — 최초 구현 시 누락됐던 버그)
		pstSession->stParkTouchCarry.dtLastInZoneTime = stRawLogInfo.dtGPS;
		pstSession->stParkTouchCarry.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;

		// ── 폴리곤 경계 소급용 기준점 스냅샷 (2026-09-05 최정우 추가, 사용자 지시) ──
		//   접촉 tick 은 "실제로 폴리곤 안"인 것과 "이탈 디바운스 유예 중(이미 밖)"인 것이 섞여
		//   있다. 경계를 찾으려면 이 둘을 갈라 각각의 마지막/첫 tick 을 잡아둬야 한다.
		//   **판정 기준은 위 억제 판정과 반드시 같아야 한다**(bMatchInParkingZoneNow).
		//   2026-09-06 억제를 원시→매칭 기준으로 바꾸면서 이 블록만 원시(bRawInParkingZoneNow)로
		//   남아, "매칭은 안·원시는 밖"인 tick 이 억제되면서 동시에 "폴리곤 밖 첫 tick"으로도
		//   기록되는 모순이 생겼다 — 실측 000376_20260819093337 seq51(원시 밖·매칭 안): 억제
		//   대상인데 이탈 기준점이 돼서 일반도로 run 이 seq51 부터 열렸다(올바른 값은 52~62).
		//   FROM_ID 도 seq51·52 어느 매칭 링크도 아닌 값(2040425202)이 찍혔다
		//   (2026-09-06 최정우 수정, 사용자 지시)
		if (bMatchInParkingZoneNow)
		{
			pstSession->qwParkTouchLastInLinkID = stMatchLinkInfo.qwLinkID;
			pstSession->dfParkTouchLastInX = stMatchLinkInfo.dfMatchX;
			pstSession->dfParkTouchLastInY = stMatchLinkInfo.dfMatchY;
			pstSession->dtParkTouchLastIn = stRawLogInfo.dtGPS;
			pstSession->dwParkTouchLastInGpsSeq = stRawLogInfo.dwSeqNo;	// (2026-09-07 최정우 추가)
			pstSession->dfParkTouchAccumAtLastIn = pstSession->stParkTouchCarry.dfAccumDistM;	// (2026-10-04 최정우 추가)
			pstSession->bParkTouchHasFirstOut = false;		// 다시 안으로 복귀 — 첫 밖 tick 재수집
		}
		else if (!pstSession->bParkTouchHasFirstOut)
		{
			pstSession->dfParkTouchFirstOutX = stMatchLinkInfo.dfMatchX;
			pstSession->dfParkTouchFirstOutY = stMatchLinkInfo.dfMatchY;
			pstSession->dtParkTouchFirstOut = stRawLogInfo.dtGPS;
			pstSession->dwParkTouchFirstOutGpsSeq = stRawLogInfo.dwSeqNo;
			pstSession->fParkTouchFirstOutSpeed = stRawLogInfo.fSpeed;	// (2026-09-06 최정우 추가)
			pstSession->bParkTouchHasFirstOut = true;
		}

		// 접촉 구간 전체에서 매칭좌표 기준 안쪽이 한 번이라도 확인되면 이 접촉 전체를 "확정"으로
		//   기록해둔다 — 확정 여부는 접촉이 끝나는 tick에서 판정한다(사용자 지시, 2026-09-03 추가)
		if (bMatchInParkingZoneNow)
			pstSession->bParkTouchEverMatchedInside = true;
	}
	else if (pstSession->bHasParkTouchCarry)
	{
		// 이번 tick에서 방금 확정 이탈됨 — 접촉 구간 마지막 레그까지 마저 더한다 (2026-09-03 최정우 추가)
		POINT stPrev, stCur;
		stPrev.dfX = pstSession->stParkTouchCarry.dfLastX;
		stPrev.dfY = pstSession->stParkTouchCarry.dfLastY;
		stCur.dfX = stMatchLinkInfo.dfMatchX;
		stCur.dfY = stMatchLinkInfo.dfMatchY;
		pstSession->stParkTouchCarry.dfAccumDistM += HaversineMeters(stPrev, stCur);
		pstSession->stParkTouchCarry.dtLastInZoneTime = stRawLogInfo.dtGPS;
		pstSession->stParkTouchCarry.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;

		if (pstSession->bParkTouchEverMatchedInside)
		{
			// 확정 접촉 — 접촉 구간 자체(폴리곤 안에서 이동한 거리·시간)는 기존 원칙(주정차
			//   폴리곤 안은 일반도로 미표출)대로 절대 넘기지 않는다 — stParkTouchCarry를
			//   병합-이월 변수로 흘려보내면 폴리곤 내부 이동거리가 다음 run의 거리로 둔갑한다
			//   (실측 000376_20260819094414 seq93~105 — 접촉이 seq93~94에서 이미 확정됐는데도
			//   당시 열려있던 run이 없어 stMergeCarry가 그대로 ②에서 새 run으로 소비되며
			//   폴리곤 내부(seq94~104) 이동거리 238.8m가 고스란히 일반도로 거리로 등록되던
			//   버그. 접촉 중 이탈 디바운스로 보류돼있던 run(있다면, 접촉 시작 이전의 정당한
			//   진행분)만 그 경계 그대로 확정 등록한다(사용자 지시, 2026-09-03 최정우 수정)
			// 접촉 직전까지 실제 이동거리가 0이면 등록할 내용 자체가 없다 — 빈 레코드를 남기지
			//   않는다(사용자 지시, 2026-09-03 최정우 추가)
			// [2026-10-04 최정우 추가 — 정책 대조표 B2·B8·C3, 사용자 확정] 주행 중 폴리곤을 **빠져나온**
			//   경우 폴리곤 안 구간도 일반도로로 등록한다(종전: 보류 run 만 경계까지 등록하고 폴리곤 안
			//   구간은 버림 = 아래 else-if 분기). 보류 run(경계
			//   까지) + 경계~첫 안쪽 tick + 폴리곤 안 이동분을 **마지막 안쪽 tick 까지만** 이어 한 행으로
			//   등록하고, 진출 쪽(마지막 안쪽 tick 이후)은 아래 종전 "경계부터 새 run 소급" 로직이 그대로
			//   맡는다 — 그쪽이 면제 링크 절단 등을 이미 처리하므로, 이탈 디바운스 tick 을 접촉 버퍼째
			//   넘기면 생기는 이중 계상(시나리오 980007 seq15~16 면제 64m)을 피한다. 두 행은 순번이
			//   맞닿아 MergeAdjacentNodeStepRows 가 하나로 합친다.
			//   트립이 폴리곤 안에서 끝나는 경우(B4)는 이 블록이 아니라 트립종료·TTL 경로가 처리하며,
			//   그쪽은 모드와 무관하게 "보류 run 을 경계까지만 등록" 그대로다.
			//   정지 tick 거리는 접촉 버퍼 누적 단계에서 이미 빠져 있다(bTouchStill).
			//   검증: web/tools/zone_check/analysis/park_scenario.py (모드 0/1 대조)
			// 되돌리는 법: 아래 if 블록을 지우면 종전(else-if 분기만) 동작이다.
			if (pstSession->qwParkTouchLastInLinkID != 0)
			{
				ZONE_RUN_SESSION stInRun = pstSession->bHasHeldNodeStepRun
					? pstSession->stHeldNodeStepRun : pstSession->stParkTouchCarry;
				double dfInsideM = pstSession->dfParkTouchAccumAtLastIn;
				if (pstSession->bHasHeldNodeStepRun)
				{
					POINT stHeldEnd, stTouchStart;
					stHeldEnd.dfX = stInRun.dfLastX;  stHeldEnd.dfY = stInRun.dfLastY;
					stTouchStart.dfX = pstSession->stParkTouchCarry.dfEntryX;
					stTouchStart.dfY = pstSession->stParkTouchCarry.dfEntryY;
					if ((stHeldEnd.dfX != 0.0) && (stTouchStart.dfX != 0.0))
						dfInsideM += HaversineMeters(stHeldEnd, stTouchStart);
					stInRun.dfAccumDistM += dfInsideM;
				}
				else
				{
					stInRun.dfAccumDistM = dfInsideM;
				}
				stInRun.qwLastLinkID = pstSession->qwParkTouchLastInLinkID;
				stInRun.dfLastX = pstSession->dfParkTouchLastInX;
				stInRun.dfLastY = pstSession->dfParkTouchLastInY;
				stInRun.dtLastInZoneTime = pstSession->dtParkTouchLastIn;
				stInRun.dwLastInZoneGpsSeq = pstSession->dwParkTouchLastInGpsSeq;
				if (stInRun.dfAccumDistM > 0.0)
				{
					CHARGE_INSERT_ROW stInRow;
					BuildNodeStepRow(stInRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
						pstSession->nChargeSeq, stInRun.dtLastInZoneTime, stInRun.dwLastInZoneGpsSeq,
						"Y", "0", &stInRow);
					pvtChargeInserts->push_back(stInRow);
					pstSession->nChargeSeq += 1;
					LOGFMTI("[#%02d] node step through park zone (drivemode=1)!device=[%s] trip_id=[%s] "
						"zone=[%s] seq=[%s~%s] inside=[%.1f]m total=[%.1f]m",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						pstSession->szParkTouchZoneRoadId, stInRow.strStartGpsSeq.c_str(),
						stInRow.strEndGpsSeq.c_str(), dfInsideM, stInRun.dfAccumDistM);
				}
			}
			else if (pstSession->bHasHeldNodeStepRun && (pstSession->stHeldNodeStepRun.dfAccumDistM > 0.0))
			{
				CHARGE_INSERT_ROW stHeldRow;
				BuildNodeStepRow(pstSession->stHeldNodeStepRun, stRawLogInfo.szTripID,
					stRawLogInfo.szDeviceKey, pstSession->nChargeSeq,
					pstSession->stHeldNodeStepRun.dtLastInZoneTime,
					pstSession->stHeldNodeStepRun.dwLastInZoneGpsSeq, "Y", "0", &stHeldRow);
				pvtChargeInserts->push_back(stHeldRow);
				pstSession->nChargeSeq += 1;
			}
			pstSession->bHasHeldNodeStepRun = false;

			// ── 폴리곤 경계부터 새 run 소급 개시 (2026-09-05 최정우 추가, 사용자 지시) ──
			//   위 원칙("폴리곤 안 구간은 넘기지 않는다")은 그대로다. 문제는 그 원칙이 적용되는
			//   범위가 "폴리곤 안"이 아니라 "이탈 디바운스가 확정될 때까지"였다는 것 — 실제 경계를
			//   지난 뒤 park_exitcnt 만큼의 tick 이 어느 레코드에도 안 들어갔다(실측
			//   000376_20260821095239: 경계는 seq41~42 사이인데 run 은 seq45 부터 열려 81.0m·
			//   10.3초 누락. 디바운스 3틱에 더해 seq44 가 클램프 저신뢰 SKIP 이라 과금 함수 자체가
			//   호출되지 않아 확정이 한 틱 더 밀린 것도 겹쳤다).
			//   진입 방향의 인수인계(handoff gap)와 완전히 대칭으로 처리한다 — 마지막 "폴리곤 안"
			//   링크부터 이번 링크까지 FindLinkPathBounded 로 누락 링크를 복구하고,
			//   FindLinkPolygonExitCrossing 으로 폴리곤을 벗어나는 지점을 찾아 그 지점을 진입점으로
			//   하는 이월(stMergeCarry)을 만든다. 아래 ②에서 새 run 이 그대로 이어받는다.
			//   경로를 못 찾거나 교차점을 못 찾으면 아무 것도 하지 않는다 — 종전 동작(확정 tick 부터
			//   시작) 그대로 폴백.
			if ((pstSession->qwParkTouchLastInLinkID != 0) && pstSession->bParkTouchHasFirstOut
				&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr))
			{
				PZONE_INFO pstExitZone =
					m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szParkTouchZoneRoadId);
				if (pstExitZone != nullptr)
				{
					static const int MM_NODE_STEP_PARK_EXIT_MAX_HOPS = 6;		// handoff gap 과 동일 상한
					vector<uint64> vtExitPath;
					if (FindLinkPathBounded(pstSession->qwParkTouchLastInLinkID,
							stMatchLinkInfo.qwLinkID, MM_NODE_STEP_PARK_EXIT_MAX_HOPS, &vtExitPath)
						&& !vtExitPath.empty())
					{
						// 경로를 앞에서부터 훑어 폴리곤을 벗어나는 링크를 찾고, 그 지점 이후의
						//   거리만 누적한다. 마지막 원소(=이번 확정 링크)는 링크 전체가 아니라
						//   "시작~이번 매칭점"까지만 이동한 것이므로 매칭 위치를 쓴다
						//   (MapMatch.cpp 재구성 경로 길이 산출과 동일 근거)
						double dfCrossX = 0.0, dfCrossY = 0.0;
						double dfAfterM = 0.0;
						uint64 qwCrossLinkID = 0;		// 폴리곤 경계가 놓인 링크 — FROM_ID 용
						uint64 qwAfterLastLinkID = 0;	// 실제로 거리를 더한 마지막 링크 — TO_ID 용
						bool bTruncatedByZone = false;	// 면제도로 링크를 만나 중간에서 끊었는지
						bool bFoundExit = false;
						bool bLenOk = true;
						const size_t nLastIdx = vtExitPath.size() - 1;
						const double dfCurLinkPos = static_cast<double>(stMatchLinkInfo.wLenFromLink)
							+ ((stMatchLinkInfo.dfSgmtMatchLen > 0.0) ? stMatchLinkInfo.dfSgmtMatchLen : 0.0);

						for (size_t p = 0; p < vtExitPath.size(); ++p)
						{
							const bool bIsLast = (p == nLastIdx);
							// 이 링크에서 실제로 진행한 거리 — 마지막 링크는 매칭점까지만
							double dfLinkRunM = 0.0;
							if (bIsLast)
								dfLinkRunM = dfCurLinkPos;
							else
							{
								PLINK_INFO pstPathLink = m_stConfig.pcDataLoader->GetLinkInfo(vtExitPath[p]);
								if (pstPathLink == nullptr) { bLenOk = false; break; }
								dfLinkRunM = pstPathLink->dfLen;
							}

							// 다른 과금 구역(면제도로)에 속한 링크를 만나면 거기서 끊는다 — 일반도로
							//   run 이 그 구역 안으로 이어지면 같은 구간이 두 유형으로 중복 계상되고,
							//   면제도로 레코드가 열리며 일반도로 run 이 설 자리를 잃는다. 실측
							//   000376_20260821094609: 폴리곤 경계(2040425303 위)에서 나온 뒤 곧바로
							//   면제도로 RL-Z00015(2040424701)로 들어가는데, 경계 이월이 그 면제 링크
							//   까지 삼켜 entry_seq 가 25(이미 면제도로 안)로 잡혔다. 그 결과 경계~면제
							//   진입 사이의 2040425303 잔여분과 2040425102(8.3m) 전체가 어느 과금
							//   이력에도 안 들어갔다. 여기서 끊으면 그 구간이 일반도로로 남는다.
							//   게이트형(폐쇄식·구간단속·개방식)은 진입 판정이 게이트로 따로 관리되므로
							//   대상에서 뺀다 (2026-09-06 최정우 추가, 사용자 지시)
							if (bFoundExit && (m_stConfig.pcChargeDataLoader != nullptr)
								&& (m_stConfig.pcChargeDataLoader->GetExemptZoneByLinkId(vtExitPath[p]) != nullptr))
							{
								bTruncatedByZone = true;
								break;
							}

							if (!bFoundExit)
							{
								double dfExitDistM = 0.0;
								if (FindLinkPolygonExitCrossing(vtExitPath[p], pstExitZone->vtCoords,
										&dfExitDistM, &dfCrossX, &dfCrossY))
								{
									bFoundExit = true;
									qwCrossLinkID = vtExitPath[p];
									// 이 링크에서 경계 이후로 진행한 몫만 더한다
									if (dfLinkRunM > dfExitDistM)
										dfAfterM += (dfLinkRunM - dfExitDistM);
									qwAfterLastLinkID = vtExitPath[p];
								}
								// 아직 폴리곤 안(교차 없음) — 이 링크는 통째로 제외
							}
							else
							{
								dfAfterM += dfLinkRunM;			// 경계 이후 링크는 전부 포함
								qwAfterLastLinkID = vtExitPath[p];
							}
						}

						if (bLenOk && bFoundExit && (dfAfterM > 0.0))
						{
							// 경계 통과 시각 — "마지막 안" tick 과 "첫 밖" tick 사이에서, 경계
							//   좌표까지의 거리 비율로 선형보간한다(ZONE_RUN_SESSION dfFirstOut* 을
							//   쓰는 기존 경계 보간들과 동일 원리). 확정 tick 을 쓰면 이미 구역에서
							//   한참 멀어진 지점과 보간하게 된다
							ZONE_RUN_SESSION stExitCarry;
							stExitCarry.szRoadID[0] = '\0';			// 미등록 pseudo-zone 으로 이어받음
							stExitCarry.dtEntryTime = InterpolateGateCrossingTime(
								pstSession->dfParkTouchLastInX, pstSession->dfParkTouchLastInY,
								pstSession->dtParkTouchLastIn,
								pstSession->dfParkTouchFirstOutX, pstSession->dfParkTouchFirstOutY,
								pstSession->dtParkTouchFirstOut, dfCrossX, dfCrossY);
							stExitCarry.dwEntryGpsSeq = pstSession->dwParkTouchFirstOutGpsSeq;
							stExitCarry.dfEntryX = dfCrossX;
							stExitCarry.dfEntryY = dfCrossY;
							// FROM_ID 는 "경계가 놓인 링크"여야 한다 — 이번 확정 링크를 쓰면 진입
							//   좌표(경계점)와 링크ID가 서로 다른 링크를 가리켜, 이력만 보고는 어디서
							//   경계를 넘었는지 알 수 없다(실측 000376_20260821095239: 경계는
							//   2040425202 위인데 FROM_ID 에 2040424401 이 들어갔다)
							//   (2026-09-05 최정우 수정, 사용자 지시)
							stExitCarry.qwEntryLinkID = qwCrossLinkID;
							// 위에서 폴리곤 안 구간을 마지막 안쪽 tick 까지 등록했으므로
							//   그 tick ~ 경계 교차점 사이를 이 진출 구간에 더해 빈틈을 없앤다
							//   (2026-10-04 최정우 추가, 정책 대조표 B2)
							if (pstSession->dfParkTouchLastInX != 0.0)
							{
								POINT stLastIn, stCross;
								stLastIn.dfX = pstSession->dfParkTouchLastInX;
								stLastIn.dfY = pstSession->dfParkTouchLastInY;
								stCross.dfX = dfCrossX;  stCross.dfY = dfCrossY;
								dfAfterM += HaversineMeters(stLastIn, stCross);
							}
							stExitCarry.dfAccumDistM = dfAfterM;
							// 면제도로에서 끊었으면 종점은 이번 매칭점이 아니라 **거리를 더한 마지막
							//   링크의 종료노드**다 — 이번 매칭점은 이미 면제 구역 안이라 쓰면 안 된다
							//   (2026-09-06 최정우 추가, 사용자 지시)
							PLINK_INFO pstAfterLast = (bTruncatedByZone && (qwAfterLastLinkID != 0))
								? m_stConfig.pcDataLoader->GetLinkInfo(qwAfterLastLinkID) : nullptr;
							if (pstAfterLast != nullptr)
							{
								stExitCarry.dfLastX = static_cast<double>(pstAfterLast->dwEdNodeX) / 360000.0;
								stExitCarry.dfLastY = static_cast<double>(pstAfterLast->dwEdNodeY) / 360000.0;
								stExitCarry.qwLastLinkID = qwAfterLastLinkID;
							}
							else
							{
								stExitCarry.dfLastX = stMatchLinkInfo.dfMatchX;
								stExitCarry.dfLastY = stMatchLinkInfo.dfMatchY;
								stExitCarry.qwLastLinkID = stMatchLinkInfo.qwLinkID;
							}
							stExitCarry.dtLastInZoneTime = stRawLogInfo.dtGPS;
							stExitCarry.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
							if (bTruncatedByZone)
							{
								// 면제도로에서 끊긴 구간은 **그 자리에서 독립 레코드로 등록**한다.
								//   이월(stMergeCarry)은 "다음에 새 일반도로 run 이 열릴 때 이어받는"
								//   구조인데, 폴리곤을 나오자마자 다른 구역이 시작되면 그 run 이 한참
								//   뒤에나 열려(실측 000376_20260821094609: 이월은 seq25 인데 일반도로
								//   run 은 seq48) 엉뚱한 구간에 붙거나 그냥 폐기된다. 이 구간은 이미
								//   "폴리곤 경계 ~ 다음 구역 진입 직전"으로 완결돼 있으므로 기다릴
								//   이유가 없다. GPS tick 이 하나도 없는 구간이라(짧은 링크를 한 tick
								//   에 통과) 시작·종료 seq 가 같은 1틱 레코드가 되며, 이는
								//   000376_20260819093337 trip_seq=5(2040424803, 6m, 106~106)와 동일
								//   형태다 (2026-09-06 최정우 추가, 사용자 지시)
								// 순번은 **가장 가까운 매칭 tick** 으로 정한다 (2026-09-07 최정우 추가, 사용자 지시).
								//   이 레코드는 구간 안에 GPS tick 이 하나도 없는 "복구 링크만으로 구성된"
								//   행이라, 어느 tick 의 순번을 쓸지는 규칙으로 정해야 한다. 후보는 둘뿐이다 —
								//   폴리곤 안 마지막 매칭 tick(구간 진입점 쪽)과 이탈 첫 매칭 tick(진출점 쪽).
								//   각자 자기 쪽 끝점까지의 거리를 재서 **더 가까운 쪽**을 쓴다.
								//   종전에는 무조건 이탈 첫 tick 이었다 — 실측 000376_20260821094609:
								//   진입점(2040425303 위 폴리곤 경계)까지 seq24 가 4.77m, 진출점
								//   (2040425102 종료노드)까지 seq25 가 16.3m 라 seq24 가 3배 이상 가까운데도
								//   25~25 로 찍혔다. 정답은 24~24 다.
								// 되돌리는 법: 아래 dwRowGpsSeq 선택 블록을 지우고
								//   stExitCarry.dwEntryGpsSeq 를 그대로 넘기면 종전 동작이다
								uint32 dwRowGpsSeq = stExitCarry.dwEntryGpsSeq;
								if (pstSession->dwParkTouchLastInGpsSeq != 0)
								{
									POINT stInTick, stEntryPt, stOutTick, stExitPt;
									stInTick.dfX  = pstSession->dfParkTouchLastInX;
									stInTick.dfY  = pstSession->dfParkTouchLastInY;
									stEntryPt.dfX = dfCrossX;                stEntryPt.dfY = dfCrossY;
									stOutTick.dfX = pstSession->dfParkTouchFirstOutX;
									stOutTick.dfY = pstSession->dfParkTouchFirstOutY;
									stExitPt.dfX  = stExitCarry.dfLastX;     stExitPt.dfY  = stExitCarry.dfLastY;

									const double dfToEntry = HaversineMeters(stInTick, stEntryPt);
									const double dfToExit  = HaversineMeters(stOutTick, stExitPt);
									if (dfToEntry < dfToExit)
										dwRowGpsSeq = pstSession->dwParkTouchLastInGpsSeq;

									LOGFMTI("[#%02d] park exit boundary seq pick!device=[%s] trip_id=[%s] "
										"in_seq=[%u] to_entry=[%.2f]m out_seq=[%u] to_exit=[%.2f]m -> seq=[%u]",
										nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
										pstSession->dwParkTouchLastInGpsSeq, dfToEntry,
										stExitCarry.dwEntryGpsSeq, dfToExit, dwRowGpsSeq);
								}

								CHARGE_INSERT_ROW stCutRow;
								BuildNodeStepRow(stExitCarry, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
									pstSession->nChargeSeq, stExitCarry.dtEntryTime,
									dwRowGpsSeq, "Y", "0", &stCutRow);
								// START_GPS_SEQ 도 같은 순번으로 맞춘다 — BuildNodeStepRow 는 시작 순번을
								//   stRun.dwEntryGpsSeq 에서 읽으므로 여기서 함께 정렬해 준다
								{
									char szPickSeq[16];
									snprintf(szPickSeq, sizeof(szPickSeq), "%u", dwRowGpsSeq);
									stCutRow.strStartGpsSeq = szPickSeq;
								}
								// 시작·종료 tick 이 같아 경과시간이 0 이므로 BuildNodeStepRow 안의
								//   평균속도 계산(거리÷경과시간)이 성립하지 않는다. 그렇다고 0 으로 두면
								//   주행 중 통과한 구간이 "0초·0km/h"(=정차)로 읽혀 속도 기반 판정을
								//   왜곡한다. 그 tick 의 **순간속도**로 소요시간을 역산해 거리·시간·속도
								//   세 값이 서로 모순되지 않게 맞춘다 — 실측 000376_20260821094609:
								//   13m ÷ 17km/h = 2.75초 로 GPS 간격 3초와 정합한다.
								//   (2026-09-06 최정우 추가, 사용자 지시)
								{
									// 경계를 실제로 지난 시점의 속도(이탈 첫 tick)를 1순위로 쓴다 —
									//   확정 tick 은 디바운스만큼 뒤라 값이 어긋난다
									const double dfSpdKmh = (pstSession->fParkTouchFirstOutSpeed > 0.0f)
										? static_cast<double>(pstSession->fParkTouchFirstOutSpeed)
										: ((pstSession->fLastConfirmedLinkSpeed > 0.0f)
											? static_cast<double>(pstSession->fLastConfirmedLinkSpeed)
											: ((stRawLogInfo.fSpeed > 0.0f) ? static_cast<double>(stRawLogInfo.fSpeed) : 0.0));
									if ((dfSpdKmh > 0.0) && (dfAfterM > 0.0))
									{
										int nStaySec = static_cast<int>((dfAfterM / (dfSpdKmh / 3.6)) + 0.5);
										if (nStaySec < 1) nStaySec = 1;
										char szBuf[16];
										snprintf(szBuf, sizeof(szBuf), "%d", nStaySec);
										stCutRow.strStaySeconds = szBuf;
										snprintf(szBuf, sizeof(szBuf), "%d", static_cast<int>(dfSpdKmh + 0.5));
										stCutRow.strSpeedKmh = szBuf;
									}
								}
								pvtChargeInserts->push_back(stCutRow);
								pstSession->nChargeSeq += 1;

								LOGFMTI("[#%02d] park exit boundary recorded (zone-truncated)!device=[%s] "
									"trip_id=[%s] zone=[%s] seq=[%u] from=[%llu] to=[%llu] dist=[%.1f]m",
									nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
									pstSession->szParkTouchZoneRoadId, stExitCarry.dwEntryGpsSeq,
									static_cast<unsigned long long>(stExitCarry.qwEntryLinkID),
									static_cast<unsigned long long>(stExitCarry.qwLastLinkID), dfAfterM);
							}
							else
							{
								stExitCarry.bEntryFixedByParkExit = true;	// 폴리곤 경계가 이 구간의 시작점 (2026-09-22 최정우 추가)
								pstSession->stMergeCarry = stExitCarry;
								pstSession->bHasMergeCarry = true;
								pstSession->dwMergeCarrySeq = stRawLogInfo.dwSeqNo;

								LOGFMTI("[#%02d] park exit boundary carry!device=[%s] trip_id=[%s] zone=[%s] "
									"entry_seq=[%u] cur_seq=[%u] after=[%.1f]m hops=[%zu]",
									nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
									pstSession->szParkTouchZoneRoadId, stExitCarry.dwEntryGpsSeq,
									stRawLogInfo.dwSeqNo, dfAfterM, vtExitPath.size());
							}
						}
					}
				}
			}
		}
		else
		{
			// 미확정 — 이 접촉은 진짜 주정차가 아니었던 것으로 소급 취소한다. 접촉 중 이탈
			//   디바운스로 보류돼있던 run(있다면)의 진입정보를 그대로 살리고, 그 누적거리와
			//   접촉 구간 누적거리를 합쳐 이월한다 — 새 run이 아니라 원래 run이 끊긴 적
			//   없었던 것처럼 이어붙인다(사용자 지시, 2026-09-03 최정우 추가 — 실측
			//   000376_20260819094414 seq54~61)
			if (pstSession->bHasHeldNodeStepRun)
			{
				pstSession->stMergeCarry = pstSession->stHeldNodeStepRun;
				pstSession->stMergeCarry.dfAccumDistM += pstSession->stParkTouchCarry.dfAccumDistM;
				pstSession->stMergeCarry.dtLastInZoneTime = pstSession->stParkTouchCarry.dtLastInZoneTime;
				pstSession->stMergeCarry.dwLastInZoneGpsSeq = pstSession->stParkTouchCarry.dwLastInZoneGpsSeq;
				pstSession->bHasHeldNodeStepRun = false;
			}
			else
			{
				pstSession->stMergeCarry = pstSession->stParkTouchCarry;
			}
			pstSession->bHasMergeCarry = true;
			pstSession->dwMergeCarrySeq = stRawLogInfo.dwSeqNo;
		}

		pstSession->bHasParkTouchCarry = false;
		pstSession->bParkTouchEverMatchedInside = false;
		// 경계 소급 스냅샷도 함께 비운다 — 다음 접촉이 이전 접촉의 기준점을 물려받으면
		//   엉뚱한 구역의 경계로 소급된다 (2026-09-05 최정우 추가)
		pstSession->qwParkTouchLastInLinkID = 0;
		pstSession->dfParkTouchAccumAtLastIn = 0.0;	// (2026-10-04 최정우 추가)
		pstSession->bParkTouchHasFirstOut = false;
	}

	if (bInParkingZone)
		vtZones.clear();

	// NODE_STEP 일반도로 확장(케이스2) — 경로상 어떤 링크든 하나라도 "어떤 과금유형에도 등록 안
	//   됨"이면 이 tick은 미등록 pseudo-zone(szRoadID=="") 대상. road_kind=0 정식구역과 별개 트랙 —
	//   두 상태가 같은 tick에 동시에 성립할 순 없음(링크가 road_kind=0 이면 이미 등록된 것이라
	//   IsLinkChargeRegistered 가 true) (2026-09-01 최정우 추가)
	// 게이트형 구역(1·2·3) 링크는 "그 구역 run 이 열려 있는 동안"만 등록으로 본다 — 판정을
	//   IsLinkChargeRegistered() 단독에서 IsLinkNodeStepEligible() 로 옮겼다. 근거·되돌리는 법은
	//   그 함수 주석 참고 (2026-09-07 최정우 수정, 사용자 지시)
	bool bTouchesUnregistered = false;
	if (!bInParkingZone)
	{
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
			bTouchesUnregistered = IsLinkNodeStepEligible(stMatchLinkInfo.qwLinkID, pstSession);
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
			{
				if (IsLinkNodeStepEligible(stMatchLinkInfo.aqwPathLinkIDs[i], pstSession))
				{ bTouchesUnregistered = true; break; }
			}
		}
	}

	// [2026-09-16 최정우 — 시도했다가 되돌림] 면제도로(5) 위 tick 에서 이탈 디바운스(node_exitcnt)를
	//   건너뛰고 일반도로 run 을 즉시 마감하는 예외를 넣어 봤다. 목적은 면제 구간이 1~2 tick 일 때
	//   일반도로 run 이 그 구간을 통째로 감싸는 것(실서버 면제 535행 중 130행)을 막는 것이었다.
	//   **결과는 정반대였다** — 합성 재현(000297_20260911131815 복제, CAR000996) 기준
	//   수정 전 `1:0(1~488, 22765m) 2:5(10) 3:0(490~493) 4:5(494~495) 5:0(499~871)` 이
	//   수정 후 `1:0(1~495, 22887m) 2:5(10) 3:5(494~495) 4:0(499~871)` 로 **더 나빠졌다**:
	//   일반도로가 면제 구간까지 삼키고 거리도 +122m 늘었으며 중간 일반도로 run(490~493)은 사라졌다.
	//   당시엔 원인을 **이탈 경계 보정**(clipped to node / ApplyZoneExitTailDist)으로 짚었다.
	//   [2026-09-16 오후 정정 — 같은 날 A/B 재측정] 그 진단은 틀렸다. 진짜 선행 조건은 **병합 이월
	//   무효화**(bHasMergeCarry, 아래 블록)였다 — 이월값이 타 유형 구간을 건너뛰어 살아남는 한
	//   디바운스를 일찍 끊어도 그 이월이 다음 run 의 진입점을 앞으로 되돌려 감싸기가 남는다.
	//   이월 무효화를 먼저 넣고 재측정하니 경계 보정을 그대로 둬도 감싸기가 해소됐고(감싸기 2건으로
	//   동일), 오히려 보정을 억제하면 실주행 거리만 1,211m 깎였다. 그래서 **디바운스 예외는 현재
	//   적용돼 있고**(아래 "[디바운스 예외]" 블록) 경계 보정은 건드리지 않는다.


	// 게이트 진출 이월(bHasGateExitCarry) 무효화 — 이월값은 "게이트 진출 직후 곧바로 일반도로가
	//   이어짐"을 전제로 다음 run 진입 시각을 그 게이트 통과시각으로 앞당겨 잡는다. 그런데 진출
	//   직후 이 tick이 일반도로가 아니라 다른 과금유형(EXEMPT/SPEED/PARKING/CLOSED/OPEN)이면, 그
	//   이월값은 소비되지 못한 채 세션에 계속 남아있다가 한참 뒤 처음 열리는 엉뚱한 일반도로 run에
	//   흡수돼 진입시각이 그 run 자신의 실제 시각보다 훨씬 앞선 값으로 역전된다 — 실측
	//   000370_20260911141637: CLOSED(seq9~20) 진출로 세팅된 이월값이 EXEMPT(22~27)·SPEED(28~36)를
	//   거치는 18tick(54초) 동안 방치되다 seq38에 새로 여는 미등록run에 잘못 흡수돼 entry_time이
	//   실제 seq38 시각보다 49초나 이전으로 나왔다(run 병합 로그의 진입시각 역전으로 발견). 게이트
	//   ~이 tick 사이는 이미 다른 과금유형이 점유했으므로 일반도로로 이어붙일 근거가 없다 — 이번
	//   tick이 일반도로 카테고리가 전혀 아니면(등록구역도 미등록도 아니면) 이월값을 폐기한다. 단
	//   이월값이 방금 이번 tick에 세팅됐다면(게이트 진출과 동시에 일반도로 판정이 같은 tick에 나온
	//   경우) 건드리지 않는다 (2026-09-11 최정우 추가, 사용자 지시 — 근본 해결)
	if (!bInParkingZone && vtZones.empty() && !bTouchesUnregistered
		&& pstSession->bHasGateExitCarry && (pstSession->dwGateExitGpsSeq != stRawLogInfo.dwSeqNo))
	{
		// [2026-09-23 최정우 추가, 사용자 지적] **폐기하기 전에 미등록 구간을 독립 레코드로 건져낸다.**
		//   이 분기의 근거는 "게이트~이 tick 사이는 이미 다른 과금유형이 점유했다" 인데, 그 사이에
		//   **어느 구역에도 속하지 않는 연결 링크**가 있으면 그 전제가 성립하지 않는다. 실측
		//   000995_20260904162440: 폐쇄식 RL-Z00008 진출(seq523) 직후 seq524 가 곧바로 RL-Z00009
		//   진입이라, 그 사이 미등록 링크 3개(2520231403 12.8m → 2520655702 11.4m →
		//   2520231304 12.8m, 합 36.9m)가 통째로 사라졌다.
		//   GPS 3초 간격 사이로 지나가 tick 이 하나도 안 찍힌 구간이라 경로 재구성(aqwPathLinkIDs)
		//   에도 안 잡혀 트립 마감의 FillUncoveredLinkRows() 도 못 건진다(커버리지 로그가
		//   uncovered=0 으로 나와 정상처럼 보인다) — ApplyGateExitCarryDist() 가 쓰는 것과 같은
		//   주행방향 링크 그래프 탐색(FindLinkPathBounded, TURN_INFO 기반이라 역방향으로 뻗지 않음)
		//   으로 여기서 직접 찾는다.
		//   **거리만 다음 run 에 얹지 않는다** — 그러면 실제로는 진출~진입 사이인 구간이 한참 뒤
		//   레코드에 들어가 위치도 경계 링크(FROM_ID/TO_ID)도 드러나지 않는다(사용자 지적).
		//   그 구간 자체를 FROM~TO 가 실제 경유 링크인 일반도로 행으로 만든다.
		//   **GPS_SEQ 는 이번 tick 하나로 고정한다**(게이트 진출 tick ~ 이번 tick 으로 넓혔다가
		//   되돌렸다 — 실측 같은 유형 구간중복 +11). 게이트 진출 tick 은 그 앞 일반도로 행의
		//   마지막 tick 이기도 해서, 범위에 넣으면 그 행과 같은 유형끼리 겹친다. 이 구간에는
		//   고유한 실측 tick 이 없으므로 "달린 구간"은 FROM_ID~TO_ID(실제 경유 링크)로 표현하고
		//   순번은 한 점으로 둔다 — FillUncoveredLinkRows() 가 tick 없는 구간을 다룰 때 쓰는
		//   "gps_seq 범위를 넓히지 않는다" 는 안전규칙과 같다. 이번 tick 은 다른 유형 행에
		//   속하므로 **유형 간** 겹침(Z4·WARN)은 남는데, 이는 tick 이 없는 구간을 기록하는 한
		//   불가피하고 같은 유형 이중과금과는 성격이 다르다.
		// **진출 직후일 때만 복구한다.** 이월의 전제 자체가 "게이트를 나오자마자 이어진다" 이므로,
		//   이월이 오래 방치된 뒤 폐기되는 경우(다른 유형 구간을 여러 tick 지난 뒤)는 그 사이가
		//   이미 그 유형 몫이라 근거가 없다 — 실측 000994_20250903152350: 게이트 진출 seq440 의
		//   이월이 seq493 까지 53 tick 살아남아 124m 를 일반도로로 청구했다(그 사이는 다른 유형
		//   구간이다). 2026-09-11 에 이월 폐기 로직을 넣은 이유("54초 방치되다 엉뚱한 run 에
		//   흡수돼 진입시각 역전")와 같은 함정이다. SKIP 한두 tick 이 끼는 경우까지만 허용한다.
		static const uint32 MM_GATE_ORPHAN_MAX_TICK_GAP = 2;
		const bool bOrphanJustExited = (stRawLogInfo.dwSeqNo > pstSession->dwGateExitGpsSeq)
			&& ((stRawLogInfo.dwSeqNo - pstSession->dwGateExitGpsSeq) <= MM_GATE_ORPHAN_MAX_TICK_GAP);

		if (bOrphanJustExited && (pstSession->qwGateExitLinkID != 0) && (stMatchLinkInfo.qwLinkID != 0)
			&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr))
		{
			static const int MM_GATE_ORPHAN_MAX_HOPS = 6;			// ApplyGateExitCarryDist 동일
			vector<uint64> vtOrphanPath;
			if (FindLinkPathBounded(pstSession->qwGateExitLinkID, stMatchLinkInfo.qwLinkID,
					MM_GATE_ORPHAN_MAX_HOPS, &vtOrphanPath) && (vtOrphanPath.size() > 2))
			{
				double dfOrphanM = 0.0;
				vector<uint64> vtOrphanLinks;
				// 양 끝(게이트 링크·이번 tick 링크)은 각자 유형이 책임지므로 중간 링크만 본다.
				//   등록 링크가 섞여 있으면 그 링크는 건너뛴다 — 그 유형 몫이다
				for (size_t oi = 1; (oi + 1) < vtOrphanPath.size(); ++oi)
				{
					if (m_stConfig.pcChargeDataLoader->IsLinkChargeRegistered(vtOrphanPath[oi]))
						continue;
					PLINK_INFO pstOrphanLink = m_stConfig.pcDataLoader->GetLinkInfo(vtOrphanPath[oi]);
					if (pstOrphanLink == nullptr) continue;
					dfOrphanM += pstOrphanLink->dfLen;
					vtOrphanLinks.push_back(vtOrphanPath[oi]);
				}

				if ((dfOrphanM > 0.0) && !vtOrphanLinks.empty())
				{
					// 좌표는 링크 기하로 채운다 — 이 구간엔 매칭 좌표가 없다
					PLINK_INFO pstFirstLink = m_stConfig.pcDataLoader->GetLinkInfo(vtOrphanLinks.front());
					PLINK_INFO pstLastLink  = m_stConfig.pcDataLoader->GetLinkInfo(vtOrphanLinks.back());
					const double dfOrpFromLon = (pstFirstLink != nullptr)
						? (static_cast<double>(pstFirstLink->dwStNodeX) / 360000.0) : 0.0;
					const double dfOrpFromLat = (pstFirstLink != nullptr)
						? (static_cast<double>(pstFirstLink->dwStNodeY) / 360000.0) : 0.0;
					const double dfOrpToLon = (pstLastLink != nullptr)
						? (static_cast<double>(pstLastLink->dwEdNodeX) / 360000.0) : 0.0;
					const double dfOrpToLat = (pstLastLink != nullptr)
						? (static_cast<double>(pstLastLink->dwEdNodeY) / 360000.0) : 0.0;

					// 체류시간 — 게이트 통과 시각은 **보간값**이라 이번 tick 과 1초 차로 붙는 일이
					//   흔한데, 거리는 링크 전체를 넣으므로 평균속도가 폭주한다(실측 37m/1초 =
					//   133km/h). sTripPathLink::fSpeedKmh 주석에 적힌 것과 같은 상황이고 해법도
					//   같다 — **실측 속도로 소요시간을 역산해** 그만큼 시작 시각을 앞당긴다.
					//   거리(링크 기하)와 속도(기기 보고)가 둘 다 실측이므로 보간 시각보다 믿을 만하다.
					//   역산값이 더 짧으면 건드리지 않는다(실제로 그만큼 걸린 것이다).
					time_t dtOrphanStart = pstSession->dtGateExit;
					{
						double dfOrphanSpdKmh = (stRawLogInfo.fSpeed > 0.0f)
							? static_cast<double>(stRawLogInfo.fSpeed)
							: static_cast<double>(pstSession->fLastConfirmedLinkSpeed);
						if (dfOrphanSpdKmh > 0.0)
						{
							int nNeedSec = static_cast<int>((dfOrphanM / (dfOrphanSpdKmh / 3.6)) + 0.5);
							if (nNeedSec < 1) nNeedSec = 1;
							if (difftime(stRawLogInfo.dtGPS, dtOrphanStart) < nNeedSec)
								dtOrphanStart = stRawLogInfo.dtGPS - nNeedSec;
						}
					}

					CHARGE_INSERT_ROW stOrphanRow;
					BuildNodeStepRowFromLinkRange(stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
						pstSession->nChargeSeq, vtOrphanLinks.front(), vtOrphanLinks.back(),
						dfOrpFromLat, dfOrpFromLon, dfOrpToLat, dfOrpToLon, dfOrphanM,
						dtOrphanStart, stRawLogInfo.dtGPS,
						stRawLogInfo.dwSeqNo, stRawLogInfo.dwSeqNo,
						"Y", "0", nullptr, nullptr, &stOrphanRow);
					stOrphanRow.vtCoveredLinks = vtOrphanLinks;		// 자기가 덮은 링크를 스스로 기록
					// 앞뒤로 붙는 일반도로 행과 합쳐지면 이 구간의 위치가 다시 흐려진다
					stOrphanRow.bNoMergeAfter = true;
					pvtChargeInserts->push_back(stOrphanRow);

					LOGFMTI("[#%02d] gate exit orphan span recovered!device=[%s] trip_id=[%s] seq=[%d] "
						"gps_seq=[%u] gate_exit_seq=[%u] links=[%d] dist=[%.1f]m from=[%llu] to=[%llu]",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						pstSession->nChargeSeq, stRawLogInfo.dwSeqNo, pstSession->dwGateExitGpsSeq,
						static_cast<int>(vtOrphanLinks.size()), dfOrphanM,
						static_cast<unsigned long long>(vtOrphanLinks.front()),
						static_cast<unsigned long long>(vtOrphanLinks.back()));

					pstSession->nChargeSeq += 1;
				}
			}
		}

		pstSession->bHasGateExitCarry = false;
		pstSession->bGateExitAtTick = false;
		LOGFMTI("[#%02d] gate exit carry invalidated(other charge type)!device=[%s] trip_id=[%s] seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
	}

	// 이번 tick 이 일반도로 카테고리가 전혀 아닌지 — 등록된 일반도로 구역도 아니고 미등록 링크도
	//   아니며 주정차 구역도 아니면, 남는 것은 다른 과금유형(면제·폐쇄식·개방식·구간단속)뿐이다.
	//   아래 이월값 무효화와 이탈 디바운스 예외가 같은 기준을 써야 해서 한 번만 계산해 공유한다
	//   (주정차는 제외 — 접촉 보류(bHasParkTouchCarry) 경로가 자기 기준으로 따로 마감한다)
	//   (2026-09-16 최정우 추가)
	const bool bOtherTypeTick = (!bInParkingZone && vtZones.empty() && !bTouchesUnregistered);

	// 일반도로 병합 이월(bHasMergeCarry) 무효화 — 바로 위 게이트 진출 이월과 똑같은 함정이 여기에도
	//   있다. 이월값은 "곧 다음 일반도로 run 이 열려 그대로 이어진다"를 전제로 진입정보(순번·시각·
	//   링크)를 들고 대기하는데, 그 사이 tick 이 다른 과금유형(면제·폐쇄식·개방식·구간단속·주정차)
	//   이면 그 구간은 이미 그 유형이 점유했으므로 일반도로로 이어붙일 근거가 없다. 그런데도
	//   이월값이 계속 남아 한참 뒤 처음 열리는 일반도로 run 에 흡수되면, 그 run 의 START_GPS_SEQ·
	//   FROM_ID·진입시각이 타 유형 구간보다 앞으로 되돌아가 그 구간을 통째로 감싼다 — 실측
	//   000994_20250903152350: 구간단속 미러(seq1~6)가 seq8 에 이월된 뒤 면제 RL-Z00018(seq14~32)을
	//   건너뛰어 seq33 에 열린 run 에 흡수돼 `1~35` 한 행이 면제 19tick 을 감쌌고, 실제 25초 구간의
	//   STAY_SECONDS 가 136초·평균속도가 45km/h→7km/h 로 왜곡됐다.
	//   게이트 진출 이월과 달리 **폐기하면 안 된다** — 이쪽은 좌표뿐 아니라 실주행 누적거리를 들고
	//   있어서 버리면 그만큼 과금 구간이 통째로 사라진다. 그 자리에서 이월값 자신의 구간(진입~마지막
	//   확정 tick)으로 단독 레코드를 확정하고 비운다. 같은 함수 뒤쪽 구간단속 미러 변환에서 "이미
	//   다른 이월값이 대기 중이면 단독 레코드로 폴백"하는 것과 동일한 처리다. 방금 이번 tick 에
	//   담긴 이월값은 아직 소비 기회를 못 가졌으므로 건드리지 않는다(설정 지점이 이 지점보다 앞뒤로
	//   흩어져 있어 dwMergeCarrySeq 로 구분한다 — 필드 주석 참고)
	//   (2026-09-16 최정우 추가, 사용자 지시 — 근본 해결)
	if (bOtherTypeTick
		&& pstSession->bHasMergeCarry && (pstSession->dwMergeCarrySeq != stRawLogInfo.dwSeqNo))
	{
		CHARGE_INSERT_ROW stCarryRow;
		BuildNodeStepRow(pstSession->stMergeCarry, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, pstSession->stMergeCarry.dtLastInZoneTime,
			pstSession->stMergeCarry.dwLastInZoneGpsSeq, "Y", "0", &stCarryRow);
		pvtChargeInserts->push_back(stCarryRow);

		LOGFMTI("[#%02d] merge carry recorded(other charge type)!device=[%s] trip_id=[%s] seq=[%d] "
			"carry_seq=[%u] gps_seq=[%u~%u] dist_m=[%s] non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			pstSession->dwMergeCarrySeq, pstSession->stMergeCarry.dwEntryGpsSeq,
			pstSession->stMergeCarry.dwLastInZoneGpsSeq, stCarryRow.strDistM.c_str(),
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));

		pstSession->nChargeSeq += 1;
		pstSession->bHasMergeCarry = false;
	}

	// 트립 종료(TRIP_EVENT=2) — 구역 위인 채로 끝나면 "이탈" 신호가 영영 안 옴, 즉시 강제 마감.
	//   ProcessRawLog() 가 스퓨리어스(순서역전) END 검사까지 마친 bTrustedTripEnd 를 그대로 씀
	//   (2026-08-25 최정우 수정)
	const bool bTripEnding = bTrustedTripEnd;

	// 구간단속 마감 시 보류해둔 일반도로 미러가 트립종료 tick까지도(접촉이 계속 진행 중이라 위
	//   "접촉 없음" 분기를 못 거쳐) 소비 안 된 채 남아있으면, 원래 값 그대로 지금 등록한다
	//   (2026-09-03 최정우 추가)
	if (bTripEnding && pstSession->bHasHeldSpeedMirrorRun)
	{
		CHARGE_INSERT_ROW stMirrorRow;
		BuildNodeStepRow(pstSession->stHeldSpeedMirrorRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, pstSession->stHeldSpeedMirrorRun.dtLastInZoneTime,
			pstSession->stHeldSpeedMirrorRun.dwLastInZoneGpsSeq, "Y", "0", &stMirrorRow);
		pvtChargeInserts->push_back(stMirrorRow);
		pstSession->nChargeSeq += 1;
		pstSession->bHasHeldSpeedMirrorRun = false;
	}

	// 주정차 접촉이 확정 판정도 못 받고(이탈이 안 옴) 트립이 그대로 끝나는 경우 — 대기할 다음
	//   틱이 더는 없으므로 지금 판정한다. 확정 접촉(bParkTouchEverMatchedInside)이면 보류돼있던
	//   run(있다면)을 그 경계 그대로 정상 등록하고, 접촉 구간 자체(폴리곤 안에서 보낸 시간)는
	//   기존 원칙(주정차 폴리곤 안은 일반도로 미표출)대로 버린다 — 트립종료 시각까지 늘여서
	//   심사대상으로 부풀리지 않는다. 미확정이면 보류된 run과 접촉 구간 누적거리를 합쳐, 마지막
	//   으로 확인된 위치·시각(접촉 중 매 tick 갱신되는 stParkTouchCarry.dtLastInZoneTime — SKIP
	//   구간 진입 직전에 멈춰있음) 기준으로 하나의 run으로 정상 등록한다(사용자 지시, 2026-09-03
	//   최정우 추가 — 실측 000376_20260819094414 seq93~150: RL-Z00001 재접촉이 seq93부터 원시좌표
	//   기준 확정된 채 SKIP·트립종료로 이어져, 판정을 못 받고 트립종료 시각까지 202초짜리 N/3 로
	//   부풀려지던 문제)
	if (bTripEnding && pstSession->bHasParkTouchCarry)
	{
		if (pstSession->bParkTouchEverMatchedInside)
		{
			// 접촉 직전까지 실제 이동거리가 0이면 등록할 내용 자체가 없다 — 빈 레코드를 남기지
			//   않는다(사용자 지시, 2026-09-03 최정우 추가)
			if (pstSession->bHasHeldNodeStepRun && (pstSession->stHeldNodeStepRun.dfAccumDistM > 0.0))
			{
				CHARGE_INSERT_ROW stHeldRow;
				BuildNodeStepRow(pstSession->stHeldNodeStepRun, stRawLogInfo.szTripID,
					stRawLogInfo.szDeviceKey, pstSession->nChargeSeq,
					pstSession->stHeldNodeStepRun.dtLastInZoneTime,
					pstSession->stHeldNodeStepRun.dwLastInZoneGpsSeq, "Y", "0", &stHeldRow);
				pvtChargeInserts->push_back(stHeldRow);
				pstSession->nChargeSeq += 1;
			}
		}
		else
		{
			ZONE_RUN_SESSION stFinal;
			if (pstSession->bHasHeldNodeStepRun)
			{
				stFinal = pstSession->stHeldNodeStepRun;
				stFinal.dfAccumDistM += pstSession->stParkTouchCarry.dfAccumDistM;
			}
			else
			{
				stFinal = pstSession->stParkTouchCarry;
			}
			stFinal.dtLastInZoneTime = pstSession->stParkTouchCarry.dtLastInZoneTime;
			stFinal.dwLastInZoneGpsSeq = pstSession->stParkTouchCarry.dwLastInZoneGpsSeq;

			CHARGE_INSERT_ROW stFinalRow;
			BuildNodeStepRow(stFinal, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
				pstSession->nChargeSeq, stFinal.dtLastInZoneTime, stFinal.dwLastInZoneGpsSeq,
				"Y", "0", &stFinalRow);
			pvtChargeInserts->push_back(stFinalRow);
			pstSession->nChargeSeq += 1;
		}

		pstSession->bHasParkTouchCarry = false;
		pstSession->bHasHeldNodeStepRun = false;
		pstSession->bParkTouchEverMatchedInside = false;
	}
	else if (bTripEnding && pstSession->bHasHeldNodeStepRun)
	{
		// 접촉 자체는 이미 끝났는데 그 안에서 조기마감된 run만 아직 대기 중인 상태는 정상적으론
		//   있을 수 없다(대기 중이면 항상 bHasParkTouchCarry=true) — 방어적으로 유지
		//   (2026-09-03 최정우 추가 — 보류 도입으로 새로 생긴 트립종료 유실 경로 방지)
		pstSession->vtNodeStepRuns.push_back(pstSession->stHeldNodeStepRun);
		pstSession->bHasHeldNodeStepRun = false;
	}

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtNodeStepRuns.size(); )
	{
		ZONE_RUN_SESSION& stRun = pstSession->vtNodeStepRuns[si];

		// 미등록 pseudo-zone run(szRoadID=="")은 road_id 비교 대상이 없어 bTouchesUnregistered 로
		//   직접 판정 — 서로 다른 미등록 링크를 넘나들어도 "미등록 상태 지속"이면 같은 run 으로
		//   본다(연속 추적) (2026-09-01 최정우 추가)
		bool bSameZone = false;
		if (stRun.szRoadID[0] == '\0')
		{
			bSameZone = bTouchesUnregistered;
		}
		else
		{
			for (size_t e = 0; e < vtZones.size(); ++e)
			{
				if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0) { bSameZone = true; break; }
			}
		}

		// 누적 이동거리는 구역 안에 있을 때만 더한다 — 이탈한 틱의 구역 밖 매칭점을 포함하지 않기 위함
		if (bSameZone)
		{
			stRun.nExitTicks = 0;						// 정상 유지 — 디바운스 해제 (2026-08-24 최정우 추가)
			// 역행의심(bReverseSuspect) tick — reverse_confirm 스트릭이 확정돼 맵매칭 자체는
			//   MATCHED로 인정되더라도(다음 매칭·세션 앵커 갱신에 필요), NODE_STEP 누적 관점에서는
			//   여전히 "직전 위치보다 뒤로 간 것"이라 run의 마지막 위치·누적거리를 갱신하지 않고
			//   그대로 대기한다. 다음 tick이 진짜 전진이면 그때 정상적으로 반영되고, 링크 자체가
			//   바뀌면 이탈 디바운스(위 else 분기)로 자연히 넘어간다 — reverse_confirm 메커니즘
			//   자체(맵매칭 신뢰 판정)는 전혀 건드리지 않고, NODE_STEP 통계 산출에서만 별도로
			//   보수적으로 처리 (사용자 지시, 2026-09-03 최정우 추가 — 실측
			//   000376_20260826150010 seq128->131)
			// [버그 수정, 2026-09-11 최정우] 정지 중(bSameRawAndHeadingAsPrev) GPS 저주파 위치표류도
			//   같은 이유로 제외 — 차가 안 움직이는데 세그먼트 재투영이 매 tick 완만히 다른 점으로
			//   튀어 dist_m 이 과다 계상되는 걸 막는다(재매칭 검증으로 최대 20m 확인).
			// [2026-10-04 최정우 추가 — E16] 폴리곤 안(매칭좌표) 정지 tick(속도<1)은 거리를 누적하지 않는다 — 종전
			//   접촉 버퍼(bTouchStill)가 하던 주차 중 GPS 표류 제외를, 억제를 끈 뒤 일반도로 run 에 그대로 옮긴 것.
			//   폴리곤 밖은 종전 동작 그대로다.
			// [2026-10-04 최정우 추가, 사용자 확정 — E18] **SKIP 좌표는 주정차 단속 정보에만** — 직전 SKIP 구간에
			//   주정차 폴리곤 안 SKIP 좌표가 있으면 그 구간을 일반도로로 잇지 않는다. 이 run 을 SKIP 직전 tick 에서
			//   끊어 행으로 내보내고(병합 금지 표시), 이번 tick 부터 새로 시작한다. 이 tick 의 직선 누적(갭 전체를
			//   건너뛴 거리)도 하지 않으며, 뒤이은 ResolveSkipGapNodeStep() 도 이 구간에 행을 만들지 않는다.
			//   실측(시나리오) 980005_20261004150000(S5, 폴리곤 안 전부 미매칭): 종전 일반도로 1~28 550m 한 행.
			// 되돌리는 법: 이 if 블록을 지운다(ResolveSkipGapNodeStep 의 E18 분기와 함께).
			if ((stRun.dwLastInZoneGpsSeq > 0) && (stRun.dwLastInZoneGpsSeq + 1 < stRawLogInfo.dwSeqNo)
				&& (bSkipBufInPark
					|| ((pstSession->dwSkipInParkSeq > stRun.dwLastInZoneGpsSeq)
						&& (pstSession->dwSkipInParkSeq < stRawLogInfo.dwSeqNo))))
			{
				if (stRun.dfAccumDistM > 0.0)
				{
					CHARGE_INSERT_ROW stCutRow;
					BuildNodeStepRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
						pstSession->nChargeSeq, stRun.dtLastInZoneTime, stRun.dwLastInZoneGpsSeq, "Y", "0", &stCutRow);
					stCutRow.bNoMergeAfter = true;
					pvtChargeInserts->push_back(stCutRow);
					pstSession->nChargeSeq += 1;
					LOGFMTI("[#%02d] node step cut before SKIP-gap in parking zone!device=[%s] trip_id=[%s] "
						"gps_seq=[%s~%s] dist_m=[%s] next_entry=[%u]",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						stCutRow.strStartGpsSeq.c_str(), stCutRow.strEndGpsSeq.c_str(), stCutRow.strDistM.c_str(),
						stRawLogInfo.dwSeqNo);
				}
				stRun.dtEntryTime = stRawLogInfo.dtGPS;
				stRun.dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
				stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
				stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
				stRun.qwEntryLinkID = stMatchLinkInfo.qwLinkID;
				stRun.qwPendingEntryFromLinkID = 0;
				stRun.bEntryFixedByParkExit = false;
				stRun.dfAccumDistM = 0.0;
				stRun.dfLastX = stMatchLinkInfo.dfMatchX;
				stRun.dfLastY = stMatchLinkInfo.dfMatchY;
				stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
				stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
				stRun.vtRunLinks.clear();
				stRun.vtRunLinks.push_back(stMatchLinkInfo.qwLinkID);
				stRun.nHeadTrail = 0;
				pstSession->dwSkipGapTickGpsSeq = 0;
				pstSession->dwSkipParkGapCutSeq = stRawLogInfo.dwSeqNo;
			}
			const bool bParkStillTick = bMatchInParkingZoneNow
				&& (stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed < 1.0f);
			if (!stMatchLinkInfo.bReverseSuspect && !stMatchLinkInfo.bSameRawAndHeadingAsPrev && !bParkStillTick)
			{
				POINT stPrev, stCur;
				stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
				stCur.dfX = stMatchLinkInfo.dfMatchX;  stCur.dfY = stMatchLinkInfo.dfMatchY;

				// [2026-09-21 최정우 — 시도했다가 **원복**. 같은 방식을 다시 제안하지 말 것 — 이슈 34]
				//   "매칭 링크가 타 과금유형 등록 링크(개방식1·폐쇄식2·면제5)면 그 링크 **시작 노드
				//   까지만** 세고 dfLastX/Y 도 그 노드로 둔다" 를 넣었다가 되돌렸다.
				//   목적은 (b) 이중계상 제거였다 — bTouchesUnregistered 가 "경로 링크 중 하나라도
				//   일반도로 계열이면 true" 인 OR 판정이라, 매칭 링크가 타 유형이어도 run 이 그 링크
				//   매칭점까지 따라가고, 같은 구간을 그 유형 처리기가 링크 시작 노드부터 또 센다.
				//   **왜 안 되는가** — 연속 추적 구조와 맞지 않는다. 타 유형 링크 위를 여러 tick 달리면
				//   매 tick dfLastX/Y 가 그 링크 시작 노드로 되돌려져 거리가 0 으로 세어지고, 다시
				//   일반도로로 돌아오는 순간 그 시작 노드부터 재어져 **타 유형 구간이 통째로 다시
				//   들어온다**. 실측(2026-09-21 전체 재매칭): 절단 **329건·59,908m**(평균 182m,
				//   50m 초과 290건, 상위는 raw 400m -> clipped 0.0m)가 잘렸는데 최종 일반도로 거리는
				//   **-34m** 뿐이었다 — 잘린 만큼 다음 leg 에서 복구돼 상쇄된 것이다.
				//   사전 측정(이슈 23 가드가 막은 8건에 남은 242.9m)은 발생 빈도를 크게 과소평가했다.
				//   dfLastX/Y 를 매칭점에 그대로 두고 거리만 깎는 변형도 답이 아니다 — 다음 leg 이
				//   타 유형 링크의 남은 구간을 그대로 포함한다. 결국 **leg 을 링크 단위로 잘라
				//   eligible 구간만 합산**해야 하고, 그러려면 run 의 거리·위치·시각·순번 갱신 축
				//   자체를 바꿔야 한다(END_GPS_SEQ 가 바뀌면 MergeAdjacentNodeStepRows() 의 인접
				//   판정까지 연쇄) — "타 로직 무영향" 과 양립하지 않아 미해결로 남긴다.
				//   ※ 이로 인한 (a) "이탈 보정이 타 유형 링크 종료 노드까지 연장" 은 이슈 23 가드로
				//     이미 막혀 있다(2026-09-21, 88->80건·-375m).

				const double dfTickDistM = HaversineMeters(stPrev, stCur);
				stRun.dfAccumDistM += dfTickDistM;
				// SKIP 갭 해소 tick 이면 이 값이 "갭 전체를 직선으로 건너뛴 거리"다 — 뒤이어 도는
				//   ResolveSkipGapNodeStep() 이 같은 구간을 경로거리로 다시 구하므로, 그쪽에서
				//   이 값을 빼고 자기 값으로 교체할 수 있도록 남겨둔다 (2026-09-15 최정우 추가)
				pstSession->dfSkipGapTickDistM = dfTickDistM;
				pstSession->dwSkipGapTickGpsSeq = stRawLogInfo.dwSeqNo;
				pstSession->dwSkipGapTickRunEntry = stRun.dwEntryGpsSeq;
				stRun.dfLastX = stMatchLinkInfo.dfMatchX;
				stRun.dfLastY = stMatchLinkInfo.dfMatchY;
			}
			// [버그 수정, 2026-09-15 최정우] 링크/시각/순번을 위 가드 밖으로 뺀다 — ProcessOpenGateCharge()
			//   의 "정지 중에도 매 tick 갱신" 주석에 기록된 회귀(정지 상태로 구역이 끝나면 stay_seconds 가 짧게 계산되고
			//   end_gps_seq 도 이르게 찍힘)가 이 함수에는 그대로 남아 있었다. 2026-09-15 오전에
			//   ProcessClosedRoadCharge()/ProcessSpeedZoneCharge() 만 고치고 여기와 EXEMPT 를 놓쳤다.
			//   단 셋을 한 덩어리로 빼면 안 된다 — 역행의심 tick 에서의 취급이 서로 다르다.
			// qwLastLinkID: TO_ID(BuildNodeStepRow())·이탈 경계 보정(아래 "이탈 지점 보정" 블록)·
			//   누락링크/SKIP갭 경로탐색(FindLinkPathBounded 호출부)에서 **항상
			//   dfLastX/Y 와 짝을 이뤄** 소비되므로 위치와 같은 tick 을 가리켜야 한다. 역행의심 tick 은
			//   위치를 갱신하지 않으므로 링크도 갱신하지 않는다. 반면 정지 tick 은 차가 안 움직여
			//   위치가 그대로라 짝이 유지되고, 같은 자리에서 매칭 링크만 보정된 것이므로 갱신이 맞다.
			if (!stMatchLinkInfo.bReverseSuspect)
				stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;

			// 보류해둔 진입 링크 확정 — 게이트 진출 tick 에는 아직 구역 링크에 매칭돼 있어 FROM_ID 를
			//   정하지 못했다(qwPendingEntryFromLinkID 필드 주석 참고). 구역 밖 링크가 처음 나온 이
			//   tick 에서 그 사이 누락 링크를 복구해 **첫 링크**를 FROM_ID 로 삼는다 — 실측
			//   000972_20260916100000: 2040423801(폐쇄식) → [2040423802] → 2040423401 이므로
			//   2040423802 가 일반도로 진입이다(사용자 지적). 거리는 건드리지 않는다 — 게이트~이 tick
			//   구간은 게이트 이월 보정이 이미 계상했다. 복구에 실패하면(경로 없음·홉 초과·중간에 타
			//   과금유형) 이번 tick 링크를 그대로 FROM_ID 로 확정한다 (2026-09-16 최정우 추가)
			if ((stRun.qwPendingEntryFromLinkID != 0) && (stMatchLinkInfo.qwLinkID != 0)
				&& (stMatchLinkInfo.qwLinkID != stRun.qwPendingEntryFromLinkID))
			{
				uint64 qwResolvedFromLinkID = stMatchLinkInfo.qwLinkID;
				if ((m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr))
				{
					static const int MM_PENDING_ENTRY_MAX_HOPS = 6;
					vector<uint64> vtEntryPath;
					if (FindLinkPathBounded(stRun.qwPendingEntryFromLinkID, stMatchLinkInfo.qwLinkID,
							MM_PENDING_ENTRY_MAX_HOPS, &vtEntryPath) && (vtEntryPath.size() > 2)
						&& m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtEntryPath[1]))
					{
						qwResolvedFromLinkID = vtEntryPath[1];		// 첫 중간 링크가 곧 일반도로의 시작
					}
				}

				LOGFMTI("[#%02d] node step entry link resolved(after gate exit)!device=[%s] trip_id=[%s] "
					"seq=[%u] gate_link=[%llu] from_link=[%llu] tick_link=[%llu]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					static_cast<unsigned long long>(stRun.qwPendingEntryFromLinkID),
					static_cast<unsigned long long>(qwResolvedFromLinkID),
					static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));

				stRun.qwEntryLinkID = qwResolvedFromLinkID;
				stRun.qwPendingEntryFromLinkID = 0;
			}
			// dtLastInZoneTime/dwLastInZoneGpsSeq: occur_dt(일반도로는 진출시각)·stay_seconds·
			//   end_gps_seq 의 근거. 차가 구역 안에 있었다는 사실 자체는 정지·역행의심과 무관하므로
			//   매 tick 갱신한다.
			stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
			stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
		}
		else if (stRun.nExitTicks == 0)
		{
			// 이탈 디바운스 스트릭의 첫 "밖" tick — 디바운스가 확정되는 마지막 tick(이미 구역에서
			//   한참 멀어진 지점)이 아니라 이 tick을 나중에 경계 노드 통과시각 보간의 "밖" 기준점으로
			//   써야 한다(위 dfFirstOutX/Y 필드 주석 참고, 2026-08-25 최정우 추가)
			stRun.dfFirstOutX = stMatchLinkInfo.dfMatchX;
			stRun.dfFirstOutY = stMatchLinkInfo.dfMatchY;
			stRun.dtFirstOut = stRawLogInfo.dtGPS;
			stRun.qwFirstOutLinkID = stMatchLinkInfo.qwLinkID;
		}

		if (bSameZone && !bTripEnding) { ++si; continue; }	// 계속 진행 중

		// node_exitcnt 회 연속 확인 후에만 이탈 확정 — 순간 오매칭 1틱으로 세션이 쪼개지는 것 방지
		//   (실측 000376_20260819094414 M45, 왕복분리 반대편 오매칭으로 RL-Z00002 세션이 40~44/
		//   46~ 로 쪼개짐). 트립종료는 디바운스 없이 즉시 마감(다음 틱이 안 옴) (2026-08-24 최정우 추가)
		// [디바운스 예외] 이번 tick 이 이미 다른 과금유형 구역이면 node_exitcnt 를 기다리지 않고
		//   즉시 마감한다. 디바운스는 "순간 오매칭 1틱으로 세션이 쪼개지는 것"을 막으려는 장치인데,
		//   다른 과금유형 구역에 들어간 것은 오매칭이 아니라 확정된 상태 전이라 기다릴 이유가 없다.
		//   기다리는 동안 END_GPS_SEQ 가 그 구역 너머까지 늘어나 일반도로 한 행이 타 유형 구간을
		//   통째로 감싼다(실측 000993/000994 면제·폐쇄식 1~2tick 구간 다수).
		//   [선행 조건] 이 예외만 단독으로 넣으면 안 된다 — 2026-09-16 오전에 그렇게 했다가 일반도로가
		//   오히려 면제 구간까지 삼키고 거리가 +122m 늘었다(같은 함수 위쪽 "시도했다가 되돌림" 주석).
		//   당시엔 원인을 이탈 지점 보정(clipped to node)으로 짚었으나, 오후에 A/B 로 다시 재보니
		//   **진짜 선행 조건은 위 이월값 무효화(bHasMergeCarry)** 였다. 이월값이 타 유형 구간을
		//   건너뛰어 살아남는 한, 디바운스를 아무리 일찍 끊어도 그 이월이 다음 run 의 진입점을 앞으로
		//   되돌려 감싸기가 그대로 남는다. 이월 무효화를 먼저 넣은 상태에서는 이탈 지점 보정을
		//   그대로 둬도 감싸기가 해소되고(실측 감싸기 2건으로 동일), 오히려 억제하면 실주행 거리만
		//   1,211m 깎인다 — 그래서 보정은 건드리지 않는다
		//   (2026-09-16 최정우 추가, 사용자 지시 — 근본 해결)
		if (!bTripEnding && !bOtherTypeTick)
		{
			stRun.nExitTicks += 1;
			if (stRun.nExitTicks < m_stConfig.nNodeExitCnt) { ++si; continue; }
		}

		// 주정차 접촉 중 이탈 디바운스로 마감되는 run — 아래 "이탈 지점 보정"/"누락 링크 보정"을
		//   거치기 *전* 원본 상태 그대로 보류한다. 그 보정들은 "이 링크를 끝까지 달리고 나갔다"는
		//   전제로 링크 종료 노드까지 거리를 채우는데, 주정차 접촉으로 인한 마감은 실제로 링크를
		//   끝까지 달린 게 아니라 폴리곤에 걸려 멈춘 것이라 이 전제가 성립하지 않는다 — 그대로
		//   적용하면 링크의 나머지 길이(흔히 폴리곤 안쪽 구간)가 통째로 일반도로 거리로 둔갑한다
		//   (사용자 지시, 2026-09-03 최정우 수정 — 실측 000376_20260819094414 seq105: 확정 접촉
		//   경계로 쓰인 링크(2040425301) 자체가 매칭좌표 기준 폴리곤 안쪽으로 확인된 링크였는데도
		//   이탈 보정이 그 링크 끝까지 거리를 채워 114m가 일반도로로 등록되던 문제. 트립종료는
		//   대기할 다음 틱이 없으므로 예외 — 기존 그대로 보정 적용 후 즉시 등록)
		if (!bTripEnding && pstSession->bHasParkTouchCarry)
		{
			stRun.dtLastInZoneTime = (stRun.dtLastInZoneTime != 0) ? stRun.dtLastInZoneTime : stRawLogInfo.dtGPS;
			stRun.dwLastInZoneGpsSeq = (stRun.dtLastInZoneTime != 0) ? stRun.dwLastInZoneGpsSeq : stRawLogInfo.dwSeqNo;

			// 누락 링크를 복구해, "링크 끝까지"가 아니라 그 링크가 접촉 시작 구역 폴리곤과 실제로
			//   교차하는 지점까지만 거리를 채운다(FindLinkPolygonCrossing) — 폴리곤 진입 직전 경계가
			//   진짜 일반도로 종점이다. 교차를 못 찾은 중간 링크는 전체 길이를 더하고 계속 진행,
			//   경로 전체에서 교차를 못 찾으면(폴리곤이 그 경로상에 없음 등) 원래대로 보정 없이
			//   보류한다(사용자 지시, 2026-09-03 최정우 추가 — 실측 000376_20260819094414 seq93~94
			//   사이 누락 링크 2040424302)
			bool bParkGapCorrected = false;			// 아래 같은 링크 보정과 배타 (2026-09-21 최정우 추가)
			uint64 qwGapSearchTo = (stRun.qwFirstOutLinkID != 0) ? stRun.qwFirstOutLinkID : stMatchLinkInfo.qwLinkID;
			if ((stRun.qwLastLinkID != 0) && (qwGapSearchTo != 0) && (stRun.qwLastLinkID != qwGapSearchTo)
				&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr)
				&& (pstSession->szParkTouchZoneRoadId[0] != '\0'))
			{
				PZONE_INFO pstTouchZone =
					m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szParkTouchZoneRoadId);
				if (pstTouchZone != nullptr)
				{
					static const int MM_NODE_STEP_PARKGAP_MAX_HOPS = 6;
					vector<uint64> vtGapPath;
					if (FindLinkPathBounded(stRun.qwLastLinkID, qwGapSearchTo,
							MM_NODE_STEP_PARKGAP_MAX_HOPS, &vtGapPath) && (vtGapPath.size() >= 2))
					{
						double dfGapDistM = 0.0;
						double dfCrossX = 0.0, dfCrossY = 0.0;
						uint64 qwCrossLinkID = 0;
						bool bAllLenOk = true;
						// [버그 수정, 2026-10-04 최정우 — 사용자 지적] 탐색 범위를 **경로 마지막 링크(첫 접촉
						//   tick 의 링크)까지** 넓히고 경로 길이 2(바로 인접)도 대상에 넣는다. 종전에는 중간
						//   링크만 봐서, 경계가 첫 접촉 tick 의 링크 위에 있으면(폴리곤 경계를 걸친 링크로
						//   바로 진입) 교차를 못 찾아 보정 전체가 건너뛰어졌다 — TO_ID 가 경계 링크가 아니라
						//   마지막 밖 tick 의 링크로 남고 그 사이 누락 링크 거리도 빠졌다(시나리오
						//   980013_20261004230000 trip_seq=1: TO 2040426101, 누락 2040426103·2040425302 앞부분).
						//   2026-09-06 인수인계(handoff) 블록에 적용한 "경로 길이 2 포함·마지막 링크까지 탐색"
						//   과 같은 규칙이다. 마지막 링크에서 교차를 못 찾으면 종전대로 보정하지 않는다
						//   (qwCrossLinkID == 0 -> 아래 조건에서 걸러짐).
						// 되돌리는 법: 루프 조건을 (g + 1 < vtGapPath.size()), 위 경로 조건을 (> 2) 로
						for (size_t g = 1; (g < vtGapPath.size()) && (qwCrossLinkID == 0); ++g)
						{
							const bool bLastGap = ((g + 1) == vtGapPath.size());
							if (bLastGap)
							{
								// 마지막 링크(첫 접촉 tick 의 링크)는 **실제 경계 교차**일 때만 인정한다.
								//   FindLinkPolygonCrossing() 은 링크가 폴리곤 안에서 시작하면 시작 노드를
								//   교차점으로 돌려주는데, 그건 경계가 아니라 앞 링크(run 의 마지막 링크) 위에
								//   경계가 있다는 뜻이다 — 그 경우는 아래 "같은 링크 경계 보정"(2026-09-21)이
								//   맡아야 한다. 이 확인 없이 인정하면 그 보정을 선점해 TO_ID 가 폴리곤 안
								//   링크로 바뀐다(시나리오 980007·980010: 2040425302 -> 2040425301)
								//   (2026-10-04 최정우 추가)
								vector<LINK_POLY_SPAN> vtLastSpans;
								if (FindLinkPolygonSpans(vtGapPath[g], pstTouchZone->vtCoords, &vtLastSpans)
									&& !vtLastSpans.empty() && vtLastSpans[0].bEntryIsBoundary)
								{
									dfGapDistM += vtLastSpans[0].dfStartDistM;
									dfCrossX = vtLastSpans[0].dfStartX;
									dfCrossY = vtLastSpans[0].dfStartY;
									qwCrossLinkID = vtGapPath[g];
								}
								break;				// 마지막 링크 전체 길이는 더하지 않는다(접촉 tick 이 그 위)
							}
							double dfPartial = 0.0;
							if (FindLinkPolygonCrossing(vtGapPath[g], pstTouchZone->vtCoords,
									&dfPartial, &dfCrossX, &dfCrossY))
							{
								dfGapDistM += dfPartial;
								qwCrossLinkID = vtGapPath[g];
							}
							else
							{
								PLINK_INFO pstGapLink = m_stConfig.pcDataLoader->GetLinkInfo(vtGapPath[g]);
								if (pstGapLink == nullptr) { bAllLenOk = false; break; }
								dfGapDistM += pstGapLink->dfLen;
							}
						}

						if (bAllLenOk && (qwCrossLinkID != 0))
						{
							// [2026-09-21 최정우 추가, 사용자 확정 — 이슈 27 권장안] 거리를 늘리면
							//   **경과시간도 같이 늘린다.** 이 보정 구간에는 실측 tick 이 하나도 없어
							//   (GPS 간격 사이로 지나간 누락 링크들) run 의 시작·종료 시각이 같은 tick 이
							//   되고, 그러면 STAY_SECONDS=0 → SPEED_KMH=0 인 "0초·0km/h" 행이 나온다 —
							//   실제로는 주행해서 지나간 구간인데 정차로 읽힌다(실측 000991_20260916133220
							//   seq34: 29m·0초·0km/h).
							//   1초 하한 같은 인공값은 쓰지 않는다 — 29m÷1s = 104km/h 가 되어 위반으로
							//   오독될 수 있다. 대신 **이미 알고 있는 실제 시간 창**(run 의 마지막 실측
							//   tick ~ 첫 접촉 tick)을 이 구간이 차지하는 거리 비율로 나눈다. 이슈 18 의
							//   같은 링크 경계 보정과 동일한 방식이며, 병합(MergeAdjacentNodeStepRows)이
							//   STAY_SECONDS 를 합산해도 실제 경과시간이라 중복 적용 문제가 없다
							//   (종전 1초 하한 시도가 원복된 근본 원인이 그 중복이었다).
							// 되돌리는 법: 아래 dtLastInZoneTime 보정 블록만 지우면 종전 동작이다.
							const double dfPrevLastX = stRun.dfLastX;
							const double dfPrevLastY = stRun.dfLastY;

							stRun.dfAccumDistM += dfGapDistM;
							stRun.qwLastLinkID = qwCrossLinkID;
							stRun.dfLastX = dfCrossX;
							stRun.dfLastY = dfCrossY;
							bParkGapCorrected = true;

							const time_t dtInTick = pstSession->stParkTouchCarry.dtEntryTime;
							if (dtInTick > stRun.dtLastInZoneTime)
							{
								// 분모는 "run 마지막 실측 위치 ~ 첫 접촉 tick 위치" 직선거리 — 복구
								//   경로는 직선보다 길 수 있으므로 비율을 1.0 으로 클램프한다
								POINT stFrom, stTo;
								stFrom.dfX = dfPrevLastX;
								stFrom.dfY = dfPrevLastY;
								stTo.dfX = pstSession->stParkTouchCarry.dfEntryX;
								stTo.dfY = pstSession->stParkTouchCarry.dfEntryY;
								const double dfTotalM = HaversineMeters(stFrom, stTo);

								double dfFrac = (dfTotalM > 0.0) ? (dfGapDistM / dfTotalM) : 1.0;
								if (dfFrac > 1.0) dfFrac = 1.0;
								const double dfAddSec = difftime(dtInTick, stRun.dtLastInZoneTime) * dfFrac;
								stRun.dtLastInZoneTime += static_cast<time_t>(dfAddSec + 0.5);
							}

							LOGFMTI("[#%02d] node step park-touch gap crossing corrected!device=[%s] "
								"trip_id=[%s] zone=[%s] cross_link=[%llu] gap_dist=[%.1f]m stay_to=[%s]",
								nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
								pstSession->szParkTouchZoneRoadId,
								static_cast<unsigned long long>(qwCrossLinkID), dfGapDistM,
								FormatDateTime14(stRun.dtLastInZoneTime).c_str());
						}
					}
				}
			}

			// [2026-09-21 최정우 추가, 사용자 지시 — 이슈 18] 폴리곤 경계가 **같은 링크 중간**에
			//   있는 경우 보정. 위 누락 링크 보정은 `stRun.qwLastLinkID != qwGapSearchTo`(= 링크가
			//   실제로 바뀐 경우)만 다루므로, 경계가 run 의 마지막 링크 한가운데 있고 그 앞뒤 tick 이
			//   같은 링크에 매칭되면 조건 자체가 성립하지 않아 아무 보정도 하지 못했다. 그 결과
			//   일반도로 run 이 "폴리곤 경계"가 아니라 "경계 직전 마지막 tick"에서 끝나, 경계까지의
			//   나머지 구간이 어느 과금유형에도 들어가지 않았다(주정차는 실측 tick 기준이라 그쪽도
			//   안 센다).
			//   실측 기준정보 대조(엔진 IsPointInPolygon 과 동일 알고리즘으로 road_link.coords 전수):
			//   주정차 폴리곤 2개(RL-Z00001·RL-Z00014)와 교차하는 링크 16개 중 **12개가 경계를 링크
			//   중간에서 정확히 한 번** 넘는다(모서리 관통 0건).
			//   **판정 규칙은 건드리지 않는다** — 어느 tick 이 주정차인지는 종전 그대로(매칭좌표가
			//   폴리곤 안이면 속도 무관 주정차)이고, 여기서 바뀌는 것은 일반도로 run 의 **종점 위치와
			//   거리**뿐이다(사용자 확정, 2026-09-21 — 주정차쪽 거리·좌표는 실측 tick 기준 유지).
			//
			// **방향을 가정하지 않는다** — 처음엔 "링크 진행방향 앞쪽 경계"만 찾았는데 실데이터
			//   유일 사례가 정반대였다(000376_20260826150010 링크 2520164100: seq128~130 이 링크
			//   131.3m 지점까지 전진했다가 seq132 에 103m 부근으로 **되돌아오며** 폴리곤에 진입 —
			//   주차하려고 기동하는 차량이라 링크를 역행한다). 그래서 "구역 밖 마지막 tick 위치"와
			//   "구역 안 첫 tick 위치" **사이**에 있는 경계를 방향 무관하게 고르고, 거리는 그 둘
			//   사이의 절댓값으로 더한다. 이 범위 조건이 곧 안전장치다 — 매칭이 튀어 엉뚱한 경계를
			//   집는 일을 막는다.
			// 되돌리는 법: 이 블록을 통째로 지우면 종전 동작으로 복귀한다.
			if (!bParkGapCorrected && (stRun.qwLastLinkID != 0) && pstSession->bHasParkTouchCarry
				&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr)
				&& (pstSession->szParkTouchZoneRoadId[0] != '\0'))
			{
				PZONE_INFO pstTouchZone =
					m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szParkTouchZoneRoadId);
				vector<LINK_POLY_SPAN> vtSpans;
				if ((pstTouchZone != nullptr)
					&& FindLinkPolygonSpans(stRun.qwLastLinkID, pstTouchZone->vtCoords, &vtSpans))
				{
					// 두 지점이 이 링크의 어디인지 — 게이트 판정과 동일하게 링크 폴리라인을 따라
					//   잰다(GatePosOnLink). 형상을 못 찾으면(-1) 보정 근거가 없어 건너뛴다
					const double dfOutPosOnLink = GatePosOnLink(stRun.qwLastLinkID,
						stRun.dfLastX, stRun.dfLastY);							// 구역 밖 마지막 tick
					const double dfInPosOnLink = GatePosOnLink(stRun.qwLastLinkID,
						pstSession->stParkTouchCarry.dfEntryX,
						pstSession->stParkTouchCarry.dfEntryY);					// 구역 안 첫 tick

					if ((dfOutPosOnLink >= 0.0) && (dfInPosOnLink >= 0.0))
					{
						const double dfLo = (dfOutPosOnLink < dfInPosOnLink) ? dfOutPosOnLink : dfInPosOnLink;
						const double dfHi = (dfOutPosOnLink < dfInPosOnLink) ? dfInPosOnLink : dfOutPosOnLink;

						// 두 tick 사이에 놓인 **실제 경계 교차** 중 밖 tick 에 가장 가까운 것을 고른다.
						//   구간의 시작/끝 가운데 링크 자체의 끝(bEntry/bExitIsBoundary=false)은 경계가
						//   아니므로 제외한다
						bool bFound = false;
						double dfBestPos = 0.0, dfBestX = 0.0, dfBestY = 0.0;
						for (size_t sp = 0; sp < vtSpans.size(); ++sp)
						{
							const double adfPos[2] = { vtSpans[sp].dfStartDistM, vtSpans[sp].dfEndDistM };
							const double adfX[2]   = { vtSpans[sp].dfStartX,     vtSpans[sp].dfEndX };
							const double adfY[2]   = { vtSpans[sp].dfStartY,     vtSpans[sp].dfEndY };
							const bool   abReal[2] = { vtSpans[sp].bEntryIsBoundary, vtSpans[sp].bExitIsBoundary };

							for (int e = 0; e < 2; ++e)
							{
								if (!abReal[e]) continue;
								if ((adfPos[e] < dfLo) || (adfPos[e] > dfHi)) continue;
								const double dfCand = (adfPos[e] > dfOutPosOnLink)
									? (adfPos[e] - dfOutPosOnLink) : (dfOutPosOnLink - adfPos[e]);
								const double dfBest = (dfBestPos > dfOutPosOnLink)
									? (dfBestPos - dfOutPosOnLink) : (dfOutPosOnLink - dfBestPos);
								if (!bFound || (dfCand < dfBest))
								{
									bFound = true;
									dfBestPos = adfPos[e]; dfBestX = adfX[e]; dfBestY = adfY[e];
								}
							}
						}

						if (bFound)
						{
							const double dfDeltaM = (dfBestPos > dfOutPosOnLink)
								? (dfBestPos - dfOutPosOnLink) : (dfOutPosOnLink - dfBestPos);
							if (dfDeltaM > 0.0)
							{
								stRun.dfAccumDistM += dfDeltaM;
								stRun.dfLastX = dfBestX;
								stRun.dfLastY = dfBestY;

								// **시각도 함께 경계로 옮긴다** — 거리만 늘리면 STAY_SECONDS 가 마지막
								//   실측 tick 시각에 멈춰 있어 SPEED_KMH 가 그만큼 부풀고, 한 행 안에서
								//   거리·시간·속도가 서로 모순된다(실측 000376_20260826150010: 29m/6s/
								//   17km/h 였던 행이 보정 후 57m/6s/34km/h 가 됐다. 실제로는 되돌아오는
								//   구간까지 약 15초가 걸렸으므로 14km/h 가 맞다).
								//   보간은 링크 진행거리 비율로 한다 — 두 지점의 링크상 위치를 이미
								//   구해뒀고, 그 사이를 등속으로 이동했다고 보는 것이 폴리곤 경계까지의
								//   직선거리 비율(InterpolateZoneCrossingTime)보다 이 상황에 정확하다
								//   (차량이 링크를 따라 되돌아오는 중이라 직선거리가 단조롭지 않다).
								//   END_GPS_SEQ 는 그대로 둔다 — 경계는 tick 사이에 있어 대응하는 순번이
								//   없고, 진입측 보정(park exit boundary carry)도 시각만 보간하고 순번은
								//   실측 tick 을 쓰는 동일 관례다 (2026-09-21 최정우 추가)
								const double dfSpanM = (dfInPosOnLink > dfOutPosOnLink)
									? (dfInPosOnLink - dfOutPosOnLink) : (dfOutPosOnLink - dfInPosOnLink);
								const time_t dtInTick = pstSession->stParkTouchCarry.dtEntryTime;
								if ((dfSpanM > 0.0) && (dtInTick > stRun.dtLastInZoneTime))
								{
									const double dfFrac = (dfDeltaM < dfSpanM) ? (dfDeltaM / dfSpanM) : 1.0;
									const double dfAddSec = difftime(dtInTick, stRun.dtLastInZoneTime) * dfFrac;
									stRun.dtLastInZoneTime += static_cast<time_t>(dfAddSec + 0.5);
								}

								LOGFMTI("[#%02d] node step park boundary clipped(same link)!device=[%s] "
									"trip_id=[%s] zone=[%s] link=[%llu] out_pos=[%.1f]m in_pos=[%.1f]m "
									"boundary=[%.1f]m added=[%.1f]m total=[%.1f]m stay_to=[%s]",
									nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
									pstSession->szParkTouchZoneRoadId,
									static_cast<unsigned long long>(stRun.qwLastLinkID),
									dfOutPosOnLink, dfInPosOnLink, dfBestPos, dfDeltaM,
									stRun.dfAccumDistM, FormatDateTime14(stRun.dtLastInZoneTime).c_str());
							}
						}
					}
				}
			}

			pstSession->stHeldNodeStepRun = stRun;
			pstSession->bHasHeldNodeStepRun = true;
			pstSession->vtNodeStepRuns.erase(pstSession->vtNodeStepRuns.begin() + si);
			continue;
		}

		// ── 일반도로 연속 구간 병합 ──
		//   미등록↔등록구역, 등록구역A↔등록구역B 로 zone_id 가 바뀌어도 여전히 일반도로
		//   범주(road_kind=0 등록 또는 미등록)에 있으면 별개 레코드로 끊지 않고 하나로 이어간다
		//   — 실제로 다른 과금유형(OPEN/CLOSED/SPEED/PARKING/EXEMPT)으로 전이할 때만 레코드를
		//   emit 한다. 이번 tick 이 여전히 일반도로 범주(vtZones 비어있지 않거나
		//   bTouchesUnregistered)이면, 그 범주를 이미 진행 중인 "다른" run 으로 이 run 의 진입
		//   정보(더 이른 시각 우선)·누적거리를 넘기고 이 run 은 emit 없이 제거한다. 받아줄 run 이
		//   아직 없으면(디바운스 타이밍상 ②가 아직 안 열었을 때) stMergeCarry 에 잠깐 담아 ②에서
		//   새 run 을 열 때 이어받는다. TO_ID 는 최종적으로 다른 과금유형에 진입할 때 그 시점
		//   run 이 [[누락 링크 보정]](FindLinkPathBounded)을 그대로 거치므로 별도 처리 불필요
		//   (사용자 지시, 2026-09-01 최정우 추가 — 실측 000376_20260819094414 unregistered(3~24)+
		//   RL-Z00002(25~42) 를 하나의 3~42 레코드로)
		if (!bTripEnding && (!vtZones.empty() || bTouchesUnregistered))
		{
			bool bMerged = false;
			for (size_t oi = 0; oi < pstSession->vtNodeStepRuns.size(); ++oi)
			{
				if (oi == si) continue;
				ZONE_RUN_SESSION& stOther = pstSession->vtNodeStepRuns[oi];
				bool bOtherActive = false;
				if (stOther.szRoadID[0] == '\0')
					bOtherActive = bTouchesUnregistered;
				else
				{
					for (size_t e = 0; e < vtZones.size(); ++e)
					{
						if (strcmp(stOther.szRoadID, vtZones[e]->szRoadID) == 0) { bOtherActive = true; break; }
					}
				}
				if (!bOtherActive) continue;

				// [버그 수정, 2026-09-11 최정우] 진입정보는 "닫히는 쪽(stRun)이 항상 더 이르다"고
				//   가정하고 무조건 stRun 걸로 덮어썼었는데, 이 가정이 항상 성립하지 않는다 — 예를 들어
				//   막 새로 연(진입시각이 늦은) run이 이미 한참 진행 중이던(진입시각이 이른) run 쪽으로
				//   병합되는 순서도 실제로 나온다(실측 000370_20260911141637 trip_seq=6, RL-Z00002가
				//   seq38부터 열려 있었는데 seq66에 새로 연 미등록 run과 병합되며 진입점이 66으로
				//   퇴보). 두 run 중 실제로 더 이른 진입시각을 가진 쪽의 진입정보를 그대로 보존한다.
				const uint32 dwOtherEntrySeqBefore = stOther.dwEntryGpsSeq;
				const time_t dtOtherEntryTimeBefore = stOther.dtEntryTime;
				const bool bRunIsEarlier = (stOther.dtEntryTime == 0)
					|| (stRun.dtEntryTime != 0 && stRun.dtEntryTime < stOther.dtEntryTime);

				// [2026-10-04 최정우 추가, 사용자 확정(권장안)] **전환 구간을 잇는다.** 종전에는 누적거리만 넘겨,
				//   이 run 의 마지막 tick ~ 상대 run 의 진입 tick 사이 구간이 어느 행에도 들어가지 않았다
				//   (상대 run 은 진입 tick 위치에서 거리 0 으로 시작한다). 실측 000376_20260819140532 seq22
				//   (미등록 2040425201) -> seq23(RL-Z00002 2040424401) 직선 14.4m 누락 — 같은 전환이
				//   000376_20260819093337 seq51->52, 000376_20260821095239 seq41->42, 000376_20260819094414
				//   seq24->25 에도 있다. E16 이전에는 폴리곤 진출 이월이 이 구간을 우연히 대신 셌다.
				//   조건: 이 run 이 먼저 시작했고 상대 run 의 진입 tick 이 이 run 마지막 tick 바로 뒤(4 tick 이내).
				//   거리: 같은 링크면 진행거리 차, 다르면 (마지막 링크 남은 길이 + 중간 링크 + 진입 링크 진입 위치)
				//   — 중간 링크는 FindLinkPathBounded(6홉), 모두 일반도로 계열(IsCase3EligibleRoadKind)이어야 한다.
				//   경로를 못 찾거나 결과가 직선의 2배+50m 를 넘으면 직선거리. 시각은 병합 행이 이 run 진입~상대
				//   run 마감이라 따로 손대지 않는다.
				// 되돌리는 법: dfJoinM 을 0 으로 고정
				double dfJoinM = 0.0;
				if (bRunIsEarlier && (stRun.dwLastInZoneGpsSeq > 0)
					&& (stOther.dwEntryGpsSeq > stRun.dwLastInZoneGpsSeq)
					&& (stOther.dwEntryGpsSeq <= stRun.dwLastInZoneGpsSeq + 4)
					&& (stRun.dfLastX != 0.0) && (stOther.dfEntryX != 0.0))
				{
					POINT stFromPt, stToPt;
					stFromPt.dfX = stRun.dfLastX;    stFromPt.dfY = stRun.dfLastY;
					stToPt.dfX = stOther.dfEntryX;   stToPt.dfY = stOther.dfEntryY;
					const double dfChordM = HaversineMeters(stFromPt, stToPt);
					dfJoinM = dfChordM;
					const uint64 qwFromL = stRun.qwLastLinkID, qwToL = stOther.qwEntryLinkID;
					if ((qwFromL != 0) && (qwToL != 0) && (m_stConfig.pcDataLoader != nullptr))
					{
						const double dfPosFrom = GatePosOnLink(qwFromL, stFromPt.dfX, stFromPt.dfY);
						const double dfPosTo = GatePosOnLink(qwToL, stToPt.dfX, stToPt.dfY);
						double dfPathM = -1.0;
						if ((dfPosFrom >= 0.0) && (dfPosTo >= 0.0))
						{
							if (qwFromL == qwToL)
							{
								if (dfPosTo >= dfPosFrom) dfPathM = dfPosTo - dfPosFrom;
							}
							else
							{
								vector<uint64> vtJoinPath;
								PLINK_INFO pstFromL = m_stConfig.pcDataLoader->GetLinkInfo(qwFromL);
								if ((pstFromL != nullptr) && FindLinkPathBounded(qwFromL, qwToL, 6, &vtJoinPath)
									&& (vtJoinPath.size() >= 2))
								{
									double dfMid = 0.0;
									bool bMidOk = true;
									for (size_t pi = 1; pi + 1 < vtJoinPath.size(); ++pi)
									{
										PLINK_INFO pstMid = m_stConfig.pcDataLoader->GetLinkInfo(vtJoinPath[pi]);
										if ((pstMid == nullptr) || ((m_stConfig.pcChargeDataLoader != nullptr)
											&& !m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtJoinPath[pi])))
										{ bMidOk = false; break; }
										dfMid += pstMid->dfLen;
									}
									const double dfFromRemain = pstFromL->dfLen - dfPosFrom;
									if (bMidOk)
										dfPathM = ((dfFromRemain > 0.0) ? dfFromRemain : 0.0) + dfMid + dfPosTo;
								}
							}
						}
						if ((dfPathM >= 0.0) && (dfPathM <= (dfChordM * 2.0 + 50.0)))
							dfJoinM = dfPathM;
					}
					LOGFMTI("[#%02d] node step run join leg!device=[%s] trip_id=[%s] from_seq=[%u] to_seq=[%u] "
						"from_link=[%llu] to_link=[%llu] chord=[%.1f]m join=[%.1f]m",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						stRun.dwLastInZoneGpsSeq, stOther.dwEntryGpsSeq,
						static_cast<unsigned long long>(qwFromL), static_cast<unsigned long long>(qwToL),
						dfChordM, dfJoinM);
				}
				if (bRunIsEarlier)
				{
					stOther.dtEntryTime = stRun.dtEntryTime;
					stOther.dfEntryX = stRun.dfEntryX;
					stOther.dfEntryY = stRun.dfEntryY;
					stOther.dwEntryGpsSeq = stRun.dwEntryGpsSeq;
					stOther.qwEntryLinkID = stRun.qwEntryLinkID;
				}
				stOther.dfAccumDistM += stRun.dfAccumDistM + dfJoinM;		// + 전환 구간(2026-10-04)
				// [2026-09-22 최정우 추가] 덮은 링크도 넘긴다 — 안 넘기면 흡수된 run 이 덮던 링크가
				//   커버리지 대조에서 "덮였는데 미덮임" 으로 잘못 잡힌다
				for (size_t li = 0; li < stRun.vtRunLinks.size(); ++li)
					stOther.vtRunLinks.push_back(stRun.vtRunLinks[li]);
				bMerged = true;

				LOGFMTI("[#%02d] node step run merged(continuous general road)!device=[%s] trip_id=[%s] "
					"from_road=[%s] into_road=[%s] carried_dist=[%.1f]m entry_kept=[%s] "
					"from_entry_seq=[%u] from_entry_time=[%ld] into_entry_seq_before=[%u] "
					"into_entry_time_before=[%ld]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					stRun.szRoadID, stOther.szRoadID, stRun.dfAccumDistM,
					bRunIsEarlier ? "from_road" : "into_road",
					stRun.dwEntryGpsSeq, static_cast<long>(stRun.dtEntryTime),
					dwOtherEntrySeqBefore, static_cast<long>(dtOtherEntryTimeBefore));
				break;
			}

			if (!bMerged && !pstSession->bHasMergeCarry)
			{
				pstSession->stMergeCarry = stRun;
				pstSession->bHasMergeCarry = true;
				pstSession->dwMergeCarrySeq = stRawLogInfo.dwSeqNo;
				bMerged = true;

				LOGFMTI("[#%02d] node step run carried over(continuous general road, pending open)!"
					"device=[%s] trip_id=[%s] from_road=[%s] dist=[%.1f]m",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					stRun.szRoadID, stRun.dfAccumDistM);
			}

			if (bMerged)
			{
				pstSession->vtNodeStepRuns.erase(pstSession->vtNodeStepRuns.begin() + si);
				continue;
			}
		}

		// ── 이탈 지점 보정 ──
		//   GPS 표본은 구역 경계에 맞춰 찍히지 않아, 마지막 매칭점에서 끊으면 통행거리가 짧게
		//   계산된다. 구역 안에서 마지막으로 달린 링크의 종료 노드까지 채운다(구역 끝 좌표든
		//   중간 교차로든 결국 그 노드로 수렴). 트립종료·TTL 로 구역 안에서 끝난 경우는
		//   실제로 거기까지만 달린 것이라 보정하지 않는다(bSameZone 으로 걸러진다).
		//   [2026-09-17 최정우 정정 — 주석이 코드와 달랐다] 종전 이 자리에는 "타 과금유형 구역
		//   진입으로 즉시 마감하는 경우도 보정하지 않는다" 고 적혀 있었으나, **아래 조건식에 그
		//   판정이 없다** — 2026-09-16 에 주석만 들어가고 구현이 빠졌다(git 이력 확인). 실제로는
		//   타 유형 tick 으로 즉시 마감할 때도 보정이 그대로 돌고, 그때 stRun.qwLastLinkID 가
		//   이미 타 유형 링크로 갱신돼 있으면 **그 링크의 잔여 길이가 일반도로 거리로 편입된다**
		//   (실측: 면제 2tick 합성 000998 에서 2040005903 의 13.8m 가 면제 행 14m 와 이중 계상).
		//
		// [버그 수정, 2026-09-21 최정우, 사용자 확정 — 이슈 23] 가드를 **보정 전체가 아니라
		//   "연장 대상 링크 하나"** 에 건다. 바로 아래 "진출~진입 사이 누락 링크 보정" 이 같은
		//   함정을 막을 때 쓰는 IsCase3EligibleRoadKind() 를 그대로 재사용한다(2026-09-14) —
		//   그 함수가 false 를 돌려주는 건 개방식(1)·폐쇄식(2)·면제(5) 등록 링크뿐이고,
		//   일반도로(0)·구간단속(3)·미등록은 전부 통과한다.
		//   **보정을 통째로 끄면 안 된다**는 종전 우려는 이 방식으로 해소된다 — 정상 건은 조건에
		//   걸리지 않으므로 값이 그대로다.
		//   실측(2026-09-21 재매칭, 종전 주석의 "85건 중 12건·696m" 는 그 뒤 코드 변경으로 낡음):
		//     · 발동 88건 중 **통과 80건(tail 합 1,070.7m) — 변화 없음**
		//     · **차단 8건(tail 합 374.5m)** = 타 유형 링크 잔여길이를 일반도로가 먹던 것
		//       2040424501 223.0m(면제 RL-Z00018) · 2520655700 120.9m(개방식 RL-Z00011) ·
		//       2520231401 16.8m(폐쇄식 RL-Z00008) · 2040005903 13.8m(면제 RL-Z00007) ·
		//       2040423801 x4 0.0m(폐쇄식 RL-Z00005, tail 0 이라 값 변화 없음)
		//   블록을 통째로 건너뛰므로 아래 "종료 노드 통과 시각 보간" 도 함께 건너뛴다 — 의도한
		//   것이다. 거리를 노드까지 늘리지 않는데 시각만 노드까지 늘리면 한 행 안에서
		//   dist·stay·speed 가 어긋난다(이슈 27 과 같은 종류의 모순).
		// [남은 근본 원인 — 이슈 34] 애초에 stRun.qwLastLinkID 가 타 유형 링크가 되는 이유는
		//   bTouchesUnregistered 가 "경로 링크 중 **하나라도** 일반도로 계열이면 true" 인 OR 판정
		//   이라, 정작 매칭된 링크가 면제·개방식이어도 run 이 그 링크까지 따라가기 때문이다.
		//   거기까지 고치려면 run 의 거리·위치·시각·순번 갱신 축을 바꿔야 하고, END_GPS_SEQ 가
		//   바뀌면 MergeAdjacentNodeStepRows() 의 인접 판정(nCurStart <= nPrevEnd + 2)까지
		//   흔들린다 — "타 로직 무영향" 과 양립하지 않아 별건으로 분리했다.
		// 되돌리는 법: 아래 조건에서 IsCase3EligibleRoadKind(...) 한 줄만 지우면 종전 동작이다.
		if (!bSameZone && (stRun.qwLastLinkID != 0) && (m_stConfig.pcDataLoader != nullptr)
			&& (m_stConfig.pcChargeDataLoader != nullptr)
			&& m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(stRun.qwLastLinkID))
		{
			PLINK_INFO pstLastLink = m_stConfig.pcDataLoader->GetLinkInfo(stRun.qwLastLinkID);
			if (pstLastLink != nullptr)
			{
				POINT stFrom, stNode;
				stFrom.dfX = stRun.dfLastX;  stFrom.dfY = stRun.dfLastY;
				stNode.dfX = static_cast<double>(pstLastLink->dwEdNodeX) / 360000.0;
				stNode.dfY = static_cast<double>(pstLastLink->dwEdNodeY) / 360000.0;

				double dfTail = HaversineMeters(stFrom, stNode);
				// [버그 수정, 2026-10-04 최정우 — 사용자 지시(권장안)] **구간단속 진입으로 마감되는 run 은
				//   끝 노드가 아니라 진입 게이트까지만 채운다.** 위 가드(IsCase3EligibleRoadKind)는 구간단속(3)
				//   링크를 통과시키므로(게이트 미통과 구간은 일반도로라는 원칙), 구간단속 진입 tick 에 run 이
				//   그 구역 링크를 마지막 링크로 들고 끝나면 **게이트 뒤 구역 구간까지** 일반도로로 연장됐다 —
				//   그 구간은 SPEED 행과 미러 행이 이미 센다. 실측(시나리오): 980003_20261004130000 trip_seq=1
				//   seq1~6 442m/18s(88km/h, tail 317.1m = 링크 2040424301 331.7m 중 게이트 TG00012 이후),
				//   980012_20261004212642 동일. 정답은 출발~게이트 약 125m.
				//   조건: 구간단속 run 이 열려 있고, 마지막 링크에 그 구역의 진입(I) 게이트가 있을 때.
				//   현재 위치가 이미 게이트를 지났으면 tail=0(더 채우지 않음). 시각 보간도 게이트 기준.
				// 되돌리는 법: 아래 if 블록을 지운다.
				if (pstSession->bInSpeedZone && (pstSession->szSpeedZoneRoadId[0] != '\0'))
				{
					vector<PGATE_INFO> vtEntryGates;
					CollectGateCandidates(m_stConfig.pcChargeDataLoader, stRun.qwLastLinkID, 'I', &vtEntryGates);
					for (size_t gi = 0; gi < vtEntryGates.size(); ++gi)
					{
						if (strcmp(vtEntryGates[gi]->szRoadID, pstSession->szSpeedZoneRoadId) != 0) continue;
						const double dfGatePos = GatePosOnLink(stRun.qwLastLinkID, vtEntryGates[gi]->dfLon, vtEntryGates[gi]->dfLat);
						const double dfCurPos = GatePosOnLink(stRun.qwLastLinkID, stRun.dfLastX, stRun.dfLastY);
						if ((dfGatePos < 0.0) || (dfCurPos < 0.0)) break;			// 형상 없음 — 종전 동작
						dfTail = (dfGatePos > dfCurPos) ? (dfGatePos - dfCurPos) : 0.0;
						stNode.dfX = vtEntryGates[gi]->dfLon;
						stNode.dfY = vtEntryGates[gi]->dfLat;
						LOGFMTI("[#%02d] node step exit clipped to speed zone entry gate!device=[%s] trip_id=[%s] "
							"link=[%llu] gate=[%s] cur_pos=[%.1f]m gate_pos=[%.1f]m tail=[%.1f]m",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
							static_cast<unsigned long long>(stRun.qwLastLinkID),
							vtEntryGates[gi]->szTollgateID, dfCurPos, dfGatePos, dfTail);
						break;
					}
				}
				// 링크 길이를 넘으면 매칭이 튄 것으로 보고 버린다(과다 계상 방지)
				if ((dfTail > 0.0) && (dfTail <= pstLastLink->dfLen + 1.0))
				{
					stRun.dfAccumDistM += dfTail;
					stRun.dfLastX = stNode.dfX;
					stRun.dfLastY = stNode.dfY;

					LOGFMTI("[#%02d] node step exit clipped to node!device=[%s] trip_id=[%s] "
						"road=[%s] link=[%llu] tail=[%.1f]m total=[%.1f]m",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
						static_cast<unsigned long long>(stRun.qwLastLinkID),
						dfTail, stRun.dfAccumDistM);
				}

				// 종료 노드 통과 시각 보간 — ProcessOpenGateCharge()/ProcessClosedRoadCharge() 동일
				//   근거(InterpolateGateCrossingTime() 참고, 목표점만 게이트 대신 이 노드). "밖" 기준점은
				//   세션 범용 직전tick(dfLastMatchX/Y)이 아니라 dfFirstOutX/Y 를 써야 한다 — node_exitcnt
				//   디바운스로 이탈 확정이 몇 틱 뒤에 일어나므로, 세션 범용 직전tick은 이미 그 디바운스
				//   구간 안의 tick(구역에서 한참 멀어진 지점)이라 엉뚱한 보간이 나온다(2026-08-25
				//   최정우 추가, 사용자 지시 — dfFirstOutX/Y 필드 주석 참고)
				if (stRun.dtFirstOut != 0)
				{
					stRun.dtLastInZoneTime = InterpolateGateCrossingTime(
						stFrom.dfX, stFrom.dfY, stRun.dtLastInZoneTime,
						stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut,
						stNode.dfX, stNode.dfY);
				}
			}
		}

		// ── 진출~진입 사이 누락 링크 보정 ──
		//   GPS 3초 간격이 짧은 연결 링크를 건너뛰면, 그 링크는 어떤 GPS 틱에도 매칭되지 않아
		//   맵매칭 결과 자체에 전혀 안 남는다. TO_ID를 마지막 매칭 링크(qwLastLinkID)로 그대로
		//   두면 실제 진출 지점보다 한 링크 앞선 곳으로 표시된다. 탐색 목표는 "디바운스 확정" tick
		//   (stMatchLinkInfo, 이미 구역에서 node_exitcnt-1 틱 더 간 지점)이 아니라 "첫 밖" tick
		//   (qwFirstOutLinkID)이어야 한다 — 확정 tick을 쓰면 실제 경계보다 몇 링크 안쪽까지
		//   보정돼버린다(실측 000376_20260826155015 RL-Z00013 — 확정tick 기준으로 찾다 구역 안쪽
		//   2번째 링크가 잘못 나옴, dfFirstOutX/Y 와 동일 근거로 2026-09-01 최정우 수정).
		//   방향성 그래프로 짧게(최대 6홉) 탐색해 중간 링크가 있으면, 그 목표 링크 바로 앞 링크로
		//   qwLastLinkID를 보정하고 그 구간 길이도 누적거리에 더한다. 직접 연결(경로 2개=FROM,TO뿐)
		//   이면 보정하지 않는다(사용자 지시, 2026-09-01 최정우 추가 — 실측 000376_20260819094414 유사 패턴)
		uint64 qwGapSearchTo = (stRun.qwFirstOutLinkID != 0) ? stRun.qwFirstOutLinkID : stMatchLinkInfo.qwLinkID;
		if (!bSameZone && !bTripEnding && (stRun.qwLastLinkID != 0)
			&& (qwGapSearchTo != 0) && (stRun.qwLastLinkID != qwGapSearchTo)
			&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcChargeDataLoader != nullptr))
		{
			static const int MM_NODE_STEP_EXIT_GAP_MAX_HOPS = 6;			// 2026-09-01 최정우 수정 —
				// 3으로는 부족했음(실측 000376_20260819094414: 2040424401→2040424301 사이 실제
				// 4홉/3개 중간링크, 2순위 그래프탐색과 동일하게 6으로 확장)
			vector<uint64> vtGapPath;
			if (FindLinkPathBounded(stRun.qwLastLinkID, qwGapSearchTo,
					MM_NODE_STEP_EXIT_GAP_MAX_HOPS, &vtGapPath) && (vtGapPath.size() > 2))
			{
				double dfGapDistM = 0.0;
				bool bAllLenOk = true;
				uint64 qwNewLastLink = 0;
				for (size_t g = 1; g + 1 < vtGapPath.size(); ++g)
				{
					// 복구 경로 중간 링크가 다른 과금유형(개방식·폐쇄식·면제)에 등록돼 있으면 그
					//   직전에서 멈춘다 — 그 링크부터는 그 유형 자신의 로직이 처리할 몫이라 일반도로가
					//   흡수하면 안 된다. 케이스3(SKIP 구간 브릿지)에 이미 쓰던 것과 동일 판정 기준을
					//   재사용(IsCase3EligibleRoadKind — 일반도로(0)·구간단속(3) 등록 또는 미등록만
					//   통과, 그 외는 제외)(사용자 지시, 2026-09-14 최정우 수정 — 실측
					//   000370_20260911141637 trip_seq=5: 개방식 RL-Z00004 등록 링크 2040424103 이
					//   여기서 무조건 흡수돼 TO_ID/DIST_M 이 그 링크만큼 과다·오기재됨)
					if (!m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtGapPath[g]))
						break;

					PLINK_INFO pstGapLink = m_stConfig.pcDataLoader->GetLinkInfo(vtGapPath[g]);
					if (pstGapLink == nullptr) { bAllLenOk = false; break; }
					dfGapDistM += pstGapLink->dfLen;
					qwNewLastLink = vtGapPath[g];
				}

				PLINK_INFO pstNewLastLink = (bAllLenOk && (qwNewLastLink != 0))
					? m_stConfig.pcDataLoader->GetLinkInfo(qwNewLastLink) : nullptr;
				if (pstNewLastLink != nullptr)
				{
					stRun.dfAccumDistM += dfGapDistM;
					stRun.qwLastLinkID = qwNewLastLink;
					stRun.dfLastX = static_cast<double>(pstNewLastLink->dwEdNodeX) / 360000.0;
					stRun.dfLastY = static_cast<double>(pstNewLastLink->dwEdNodeY) / 360000.0;

					LOGFMTI("[#%02d] node step exit gap-link corrected!device=[%s] trip_id=[%s] "
						"road=[%s] to_link=[%llu] gap_dist=[%.1f]m",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
						static_cast<unsigned long long>(qwNewLastLink), dfGapDistM);
				}
			}
		}

		// node_exitcnt 디바운스로 이탈 확정이 몇 틱 늦게 일어나므로(이미 다른 구역에 들어가 있을 수
		//   있음), occur_dt·stay_seconds 는 이번 틱(dtGPS)이 아니라 구역 안에서 실제로 마지막 확정
		//   매칭됐던 시각(dtLastInZoneTime, 위에서 보간됐으면 그 값)을 써야 한다 — 안 그러면 디바운스
		//   대기 시간이 stay_seconds 에 섞여 다음 구역 범위와 겹친다(실측 000376_20260819094414 —
		//   실제 마지막 확정은 seq38인데 디바운스 대기 중 seq42~44 는 이미 RL-Z00003 로 넘어가 있어
		//   occur_dt 를 그대로 쓰면 두 구역 G범위가 겹쳐 보임). 세션이 진입 즉시 이 값을 채우므로
		//   0 은 실질적으로 없지만 방어적으로 폴백 유지 (2026-08-24 최정우 추가)
		stRun.dtLastInZoneTime = (stRun.dtLastInZoneTime != 0) ? stRun.dtLastInZoneTime : stRawLogInfo.dtGPS;
		stRun.dwLastInZoneGpsSeq = (stRun.dtLastInZoneTime != 0) ? stRun.dwLastInZoneGpsSeq : stRawLogInfo.dwSeqNo;

		// 진입 링크를 끝내 확정 못 한 채 거리 0 으로 끝난 run 은 등록하지 않는다 — 게이트형 구역
		//   진출이 확정되는 tick 은 매칭점이 게이트로 클램프될 뿐 아직 구역 링크 위라, 그 tick 에 열린
		//   run 은 진입 링크를 다음 tick 으로 미뤄둔다(qwPendingEntryFromLinkID). 그런데 바로 다음이
		//   또 다른 과금유형이면 run 이 1tick·0m 로 끝나 미뤄둔 확정 기회가 영영 오지 않고, FROM_ID 가
		//   진출한 구역 링크 그대로 남은 0m 레코드가 생긴다 — 실측 4건이 모두 같은 모양이다(폐쇄식
		//   진출 직후 곧바로 면제 진입: 000370_20260824170123 seq36, 000370_20260819093236 seq102,
		//   000376_20260821094609 seq47, 000994_20250903152350 seq84). 주행거리가 0 이라 과금 손실은
		//   없고, 그 구간은 앞뒤 두 과금유형이 이미 자기 경계까지 계상했다.
		//   거리 조건을 함께 보는 이유는 "정차한 채 트립이 정상 종료된 0m run"(Y/0 로 남기기로 확정,
		//   실측 000993_20250903152139 seq504)을 건드리지 않기 위해서다 — 그쪽은 보류 자체가 없다
		//   (2026-09-16 최정우 추가, 사용자 지시)
		if ((stRun.qwPendingEntryFromLinkID != 0) && (stRun.dfAccumDistM < 0.5))
		{
			LOGFMTI("[#%02d] node step exit dropped(entry link unresolved, zero dist)!device=[%s] "
				"trip_id=[%s] seq=[%u] gate_link=[%llu] gps_seq=[%u~%u]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stRun.qwPendingEntryFromLinkID),
				stRun.dwEntryGpsSeq, stRun.dwLastInZoneGpsSeq);

			pstSession->vtNodeStepRuns.erase(pstSession->vtNodeStepRuns.begin() + si);
			continue;
		}

		CHARGE_INSERT_ROW stRow;
		// 정상 이탈(mid-route)·정상 트립종료(TRIP_EVENT=2) 모두 실제 종료지점을 아는 "정상" 케이스라
		//   Y/0 그대로 — TTL(AppendExpiredNodeStepCharge) 만 N/3 (2026-09-01 최정우 명시화)
		BuildNodeStepRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, stRun.dtLastInZoneTime, stRun.dwLastInZoneGpsSeq, "Y", "0", &stRow);
		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] node step exit recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"gps_seq=[%u~%u] dist_m=[%s] avg_speed=[%s] trip_ending=[%d] non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, stRun.dwEntryGpsSeq, stRun.dwLastInZoneGpsSeq,
			stRow.strDistM.c_str(), stRow.strSpeedKmh.c_str(),
			static_cast<int>(bTripEnding),
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));

		pstSession->nChargeSeq += 1;
		pstSession->vtNodeStepRuns.erase(pstSession->vtNodeStepRuns.begin() + si);
	}

	// ── ② 새로 진입한 구역 세션 개시 ─────────────────────────────────────────
	if (bTripEnding)
		return;											// 종료 틱에서는 새로 열지 않는다

	// [버그 수정, 2026-09-11 최정우 — 사용자 지시, 근본 해결] 구간단속 마감 시 보류해둔 일반도로
	//   미러(stHeldSpeedMirrorRun)를 여기, 새 run 을 실제로 여는 지점 바로 앞에서 bHasMergeCarry 로
	//   변환한다 — 이 함수 앞쪽(위 ①보다도 전)에서 미리 변환해뒀다가 여기서 소비되길 "기다리는"
	//   기존 설계는, 1틱 지연 커밋 + 사후 브릿지 보정이 겹치면 변환 시점과 소비 시점(이 지점) 사이에
	//   실제로는 다른 tick 의 호출이 끼어들어 순서가 어긋날 수 있었다(실측 000370_20260911141637 —
	//   미러가 새 run 에 진입정보를 못 넘기고 새 run 자신의 더 늦은 진입값으로 굳어짐). 변환과
	//   소비를 같은 호출·같은 지점으로 합쳐 타이밍 격차 자체를 없앤다. 이미 다른 이월값
	//   (bHasMergeCarry)이 대기 중이면 덮어쓰지 않고 단독 레코드로 안전하게 폴백한다(드문 동시발생,
	//   기존 폴백 원칙과 동일).
	if (pstSession->bHasHeldSpeedMirrorRun && !bInParkingZone
		&& (stRawLogInfo.dwSeqNo != pstSession->dwHeldSpeedMirrorSeq))
	{
		// 1틱 지연 설계의 사각지대 — 미러를 보류한 바로 다음 tick(여기)에서야 인수인계
		//   가능해지는데, 그 사이(보류 tick 자체)에 미등록 pseudo-zone run이 이미 열려버리면
		//   ("구간단속 종료와 동시에 미등록 링크로 넘어감") 그 run은 다시는 "새 run 개시"
		//   이벤트를 발생시키지 않아 bHasMergeCarry가 영원히 미소비로 남는다 — 실측
		//   000376_20260819141002 seq87(pseudo-zone run 오픈)/88(미러 변환), TTL까지 별개
		//   레코드 두 개(714m/357m, 서로 겹치는 gps_seq)로 남았다. 그러니 미루기 전에 **이미
		//   열려있는 미등록 pseudo-zone run**이 있으면 새 run 개시를 기다리지 않고 바로
		//   흡수한다(사용자 지적, 2026-09-14 최정우 수정).
		bool bAbsorbedNow = false;
		for (size_t si = 0; si < pstSession->vtNodeStepRuns.size(); ++si)
		{
			ZONE_RUN_SESSION& stExisting = pstSession->vtNodeStepRuns[si];
			if (stExisting.szRoadID[0] != '\0') continue;	// 미등록 pseudo-zone만 대상 —
															//   정식 등록구역 병합은 기존 로직이 처리
			// 진입정보는 더 이른 쪽(미러)을 채택 — "일반도로 연속 구간 병합"과 동일 원칙
			stExisting.dtEntryTime = pstSession->stHeldSpeedMirrorRun.dtEntryTime;
			stExisting.dfEntryX = pstSession->stHeldSpeedMirrorRun.dfEntryX;
			stExisting.dfEntryY = pstSession->stHeldSpeedMirrorRun.dfEntryY;
			stExisting.dwEntryGpsSeq = pstSession->stHeldSpeedMirrorRun.dwEntryGpsSeq;
			stExisting.qwEntryLinkID = pstSession->stHeldSpeedMirrorRun.qwEntryLinkID;
			stExisting.dfAccumDistM += pstSession->stHeldSpeedMirrorRun.dfAccumDistM;
			// [2026-09-22 최정우 추가] 미러가 덮은 링크도 함께 넘긴다 — 안 넘기면 그 구간이
			//   커버리지 대조에서 "덮였는데 미덮임" 으로 잘못 잡힌다(실측 000994 의 554m)
			{
				const uint64 qwMFrom = pstSession->stHeldSpeedMirrorRun.qwEntryLinkID;
				const uint64 qwMTo   = pstSession->stHeldSpeedMirrorRun.qwLastLinkID;
				vector<uint64> vtMPath;
				if ((qwMFrom != 0) && (qwMTo != 0) && (qwMFrom != qwMTo)
					&& FindLinkPathBounded(qwMFrom, qwMTo, 8, &vtMPath))
				{
					for (size_t mi = 0; mi < vtMPath.size(); ++mi)
						stExisting.vtRunLinks.push_back(vtMPath[mi]);
				}
				else
				{
					if (qwMFrom != 0) stExisting.vtRunLinks.push_back(qwMFrom);
					if ((qwMTo != 0) && (qwMTo != qwMFrom)) stExisting.vtRunLinks.push_back(qwMTo);
				}
			}
			bAbsorbedNow = true;

			LOGFMTI("[#%02d] node step (from speed zone) absorbed into already-open run!"
				"device=[%s] trip_id=[%s] seq=[%u] held_seq=[%u] dist=[%.1f]m",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				pstSession->dwHeldSpeedMirrorSeq, pstSession->stHeldSpeedMirrorRun.dfAccumDistM);
			break;
		}

		if (!bAbsorbedNow)
		{
			if (!pstSession->bHasMergeCarry)
			{
				pstSession->stMergeCarry = pstSession->stHeldSpeedMirrorRun;
				pstSession->bHasMergeCarry = true;
				pstSession->dwMergeCarrySeq = stRawLogInfo.dwSeqNo;

				LOGFMTI("[#%02d] node step (from speed zone) carried over(continuous general road, "
					"pending open)!device=[%s] trip_id=[%s] seq=[%u] held_seq=[%u] dist=[%.1f]m",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					pstSession->dwHeldSpeedMirrorSeq, pstSession->stHeldSpeedMirrorRun.dfAccumDistM);
			}
			else
			{
				CHARGE_INSERT_ROW stMirrorRow;
				BuildNodeStepRow(pstSession->stHeldSpeedMirrorRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
					pstSession->nChargeSeq, pstSession->stHeldSpeedMirrorRun.dtLastInZoneTime,
					pstSession->stHeldSpeedMirrorRun.dwLastInZoneGpsSeq, "Y", "0", &stMirrorRow);
				pvtChargeInserts->push_back(stMirrorRow);
				pstSession->nChargeSeq += 1;
			}
		}
		pstSession->bHasHeldSpeedMirrorRun = false;
	}

	// 게이트형 구역(개방식·폐쇄식·구간단속)·면제 진출 지점 이월(bHasGateExitCarry) 소비 — 케이스1(정식 등록구역
	//   진입, 바로 아래)·케이스2(미등록 pseudo-zone 진입, 이 함수 뒤쪽) 양쪽에서 똑같이 써야 한다.
	//   원래 케이스2에만 있었는데, 복구/현재 tick 링크가 정식 등록된 일반도로(0) 구역에 바로 떨어지면
	//   케이스1로 진입해 이 이월이 아예 소비되지 않고, "게이트 진출 이월 무효화"(vtZones 가 비어있을
	//   때만 발동)도 안 걸려 이월값이 세션에 계속 남는 문제가 있었다 — 최악의 경우 한참 뒤 엉뚱한
	//   run이 잘못 흡수(2026-09-11 고친 stale 이월 버그와 동일 유형). 두 곳에서 중복 구현하면 어긋날
	//   위험이 있어 지역 람다로 한 번만 구현한다(사용자 지적, 2026-09-14 최정우 추가).
	//   FindLinkPathBounded 로 이월 지점~이번 tick 사이 누락 링크를 복구해, 중간 링크마다
	//   IsCase3EligibleRoadKind() 로 걸러(케이스3 SKIP 브릿지와 동일 기준) 방금 닫힌 구역 자신의
	//   잔여 등록 링크는 제외하고 일반도로(0)·구간단속(3) 등록 또는 미등록 링크만 거리에 반영한다.
	//   경로를 못 찾거나 직접 인접이면 직선거리로 폴백. FROM_ID(qwEntryLinkID)는 복구 경로에서
	//   처음으로 거리 기여가 있었던 링크로 남긴다("거리 누적이 실제로 시작된 링크" 원칙, 2026-09-06
	//   확정 규칙).
	auto ApplyGateExitCarryDist = [&](ZONE_RUN_SESSION *pstRunForCarry) -> double
	{
		double dfCarryDistM = 0.0;
		bool bCarryPathApplied = false;
		uint64 qwCarryFromLinkID = 0;
		if ((pstSession->qwGateExitLinkID != 0) && (m_stConfig.pcDataLoader != nullptr)
			&& (m_stConfig.pcChargeDataLoader != nullptr))
		{
			static const int MM_GATE_CARRY_MAX_HOPS = 6;
			vector<uint64> vtCarryPath;
			if (FindLinkPathBounded(pstSession->qwGateExitLinkID, stMatchLinkInfo.qwLinkID,
					MM_GATE_CARRY_MAX_HOPS, &vtCarryPath) && (vtCarryPath.size() > 2))
			{
				bool bAllLenOk = true;
				// "섬" 단위로 걷는다 — 복구 경로 중간에 다른 과금유형(개방식·폐쇄식·면제) 등록
				//   링크가 끼어 있으면, 그 직전까지 쌓인 일반도로 구간을 독립된 레코드로 즉시
				//   마감하고 그 링크 다음부터 새 섬을 시작한다. 마지막 섬만 호출측이 계속 진행 중인
				//   run 으로 이어받는다. 예전엔 중간 등록 링크를 그냥 건너뛰기만 해서 앞뒤 일반도로가
				//   하나로 합쳐졌다(사용자 지적, 2026-09-14 최정우 수정 — 실측
				//   000376_20260819141002: 폐쇄식 진출 뒤 일반도로(2040423802)→개방식 등록
				//   (2040423602)→일반도로(2040423302) 순서인데 세 구간이 하나로 병합돼 있었다).
				//   섬 사이 경계(등록 링크 자체)는 실측 tick이 없어 정확한 시각을 모르므로, 편의상
				//   게이트 진출 tick 시각/순번을 그대로 쓴다(다른 지오메트리 전용 보정과 동일 관례).
				double dfIslandDistM = 0.0;
				uint64 qwIslandFromLinkID = 0;
				uint64 qwIslandLastLinkID = 0;
				double dfIslandEntryX = pstSession->dfGateExitX;
				double dfIslandEntryY = pstSession->dfGateExitY;

				// 타 과금유형(개방식·폐쇄식·면제) 구간 누적 — 일반도로 섬과 별개로, 연속된
				//   등록 링크를 모았다가 "복귀"하는 순간(다시 일반도로 계열을 만나거나 경로 끝에
				//   도달할 때) 그 유형 자신의 등록 규칙을 적용해 정식 레코드로 등록한다(사용자 지시,
				//   2026-09-14 최정우 추가 — 실측 링크 위 게이트 유무로 통과 확정 여부를 판단).
				//   지금까지는 이 구간을 그냥 건너뛰기만 했다(소액 손실 감수).
				// [도달 조건, 2026-09-15 최정우 확인 — 합성 시나리오 검증 결과]
				//   아래 EmitForeignSpan 은 **그 구역 링크 위에 실측 tick 이 단 하나도 안 찍혔을 때만**
				//   도달한다. tick 이 하나라도 있으면 그 유형의 정규 처리기(ProcessExemptZoneCharge /
				//   ProcessClosedRoadCharge 등)가 먼저 진입을 감지하면서 게이트 진출 carry 를 무효화
				//   하고("gate exit carry invalidated(other charge type)" 로그), 이 경로 자체가 안 돈다.
				//   즉 **구역 링크 전장이 GPS 수신 간격 동안의 이동거리보다 짧아야** 한다.
				//   현재 기준정보에서 그 조건을 만족하는 건 개방형 RL-Z00004 의 브리지 링크
				//   2040423602(8.8m) 하나뿐이라, 실데이터에서는 open 분기만 동작한다(실측 7건).
				//   exempt(RL-Z00016, 링크 2개·약 125m)·closed 분기는 검증용 합성 트립
				//   (000370_20260911141637 의 면제구역 tick 제거)으로 조건을 만들어도 도달하지
				//   못함을 확인했다 — **미구현·미검증이 아니라 현재 기준정보에서 도달 불가**다.
				//   짧은 면제·폐쇄 링크가 등록되면 그때 동작해야 하므로 방어적으로 남겨둔다.
				vector<uint64> vtForeignLinks;
				double dfForeignEntryX = 0.0, dfForeignEntryY = 0.0;

				// [2026-09-21 최정우 추가, 사용자 확정 — 이슈 27 권장안] 복구 구간 경과시간 배분.
				//   이 경로가 만드는 레코드(섬 분할·EmitForeignSpan)는 **실측 tick 이 하나도 없는**
				//   구간이라, 지금까지 시작·종료 시각을 모두 게이트 진출 tick 으로 찍었다. 그 결과
				//   STAY_SECONDS=0 -> SPEED_KMH=0 인 "0초·0km/h" 행이 나왔다(실측 9건, 전부 9m) —
				//   주행해서 지나간 구간인데 정차로 읽힌다.
				//   1초 하한 같은 인공값은 쓰지 않는다(9m/1s=32km/h, 29m/1s=104km/h 가 되어 위반으로
				//   오독될 수 있다). 대신 **이미 알고 있는 실제 시간 창**(게이트 진출 tick ~ 이번 tick)을
				//   각 조각이 차지하는 거리 비율로 나눈다 — 등속 가정이며, 이 파일의 다른 경계 보정
				//   (InterpolateGateCrossingTime 계열)과 같은 원리다. 병합(MergeAdjacentNodeStepRows)이
				//   STAY_SECONDS 를 합산해도 실제 경과시간이라 중복 적용 문제가 없다(종전 1초 하한
				//   시도가 원복된 근본 원인이 그 중복이었다).
				//   비율을 내려면 총거리를 먼저 알아야 해서 사전 패스를 둔다. 총거리를 못 구하거나
				//   (링크 정보 결손) 경과시간이 0 이하면 배분을 포기하고 종전대로 게이트 시각을 쓴다.
				// 되돌리는 법: bCarryTimeOk 를 false 로 고정하면 종전 동작으로 복귀한다.
				double dfTotalCarryM = 0.0;
				bool bCarryTimeOk = false;
				{
					bool bTotalOk = true;
					for (size_t t = 1; (t + 1 < vtCarryPath.size()) && bTotalOk; ++t)
					{
						PLINK_INFO pstTotLink = m_stConfig.pcDataLoader->GetLinkInfo(vtCarryPath[t]);
						if (pstTotLink == nullptr) { bTotalOk = false; break; }
						dfTotalCarryM += pstTotLink->dfLen;
					}
					POINT stTotStart, stTotCur;
					stTotStart.dfX = stMatchLinkInfo.dfStNodeX;  stTotStart.dfY = stMatchLinkInfo.dfStNodeY;
					stTotCur.dfX = stMatchLinkInfo.dfMatchX;     stTotCur.dfY = stMatchLinkInfo.dfMatchY;
					dfTotalCarryM += HaversineMeters(stTotStart, stTotCur);
					bCarryTimeOk = bTotalOk && (dfTotalCarryM > 0.0)
						&& (difftime(stRawLogInfo.dtGPS, pstSession->dtGateExit) > 0.0);
				}
				const double dfCarryElapsedSec = difftime(stRawLogInfo.dtGPS, pstSession->dtGateExit);

				// [2026-09-21 최정우 추가 — 이슈 27 보완] 시간 창이 0 인 경우의 폴백.
				//   게이트 통과 시각은 직전 tick~이번 tick 사이를 거리 비율로 보간한 값인데, time_t
				//   가 초 단위라 게이트가 이번 tick 쪽에 가까우면 **이번 tick 시각으로 반올림**된다.
				//   그러면 dfCarryElapsedSec=0 이 되어 비율 배분이 통째로 무력화된다 — 실측
				//   000376_20260819141002: seq39(14:11:59) 다음 seq40(14:12:02) 사이에 복구 링크
				//   2개(8.9m+8.8m)를 지났는데 게이트 시각이 14:12:02 로 반올림돼 창이 0 이었다.
				//   직전 tick 까지 창을 넓히는 방법은 쓰지 않는다 — 그러면 앞선 폐쇄식 행
				//   (14:11:24~14:12:02)과 시간이 3초 겹친다.
				//   대신 이 파일이 같은 상황에서 이미 쓰는 방식을 따른다: "시작·종료 tick 이 같아
				//   경과시간이 0 이면 **순간속도로 소요시간을 역산**한다"(인수인계 구간·폴리곤 경계
				//   절단 레코드의 동일 처리). 직전 확정 tick 의 보고 속도를 1순위, 이번 tick 속도를
				//   2순위로 쓰고, 둘 다 없으면 배분을 포기한다(그 경우 BulkInsertCharges 의 최후
				//   방어가 받는다).
				double dfCarrySpeedMps = 0.0;
				if (!bCarryTimeOk)
				{
					const float fAnchorKmh = (pstSession->fLastConfirmedLinkSpeed > 0.0f)
						? pstSession->fLastConfirmedLinkSpeed
						: ((stRawLogInfo.fSpeed > 0.0f) ? stRawLogInfo.fSpeed : 0.0f);
					if (fAnchorKmh > 0.0f)
						dfCarrySpeedMps = static_cast<double>(fAnchorKmh) / 3.6;
				}

				// 게이트 진출점에서 누적거리 dfCumM 만큼 진행한 지점의 추정 시각
				auto fnCarryTimeAt = [&](double dfCumM) -> time_t
				{
					if (bCarryTimeOk)
					{
						double dfFrac = dfCumM / dfTotalCarryM;
						if (dfFrac < 0.0) dfFrac = 0.0;
						if (dfFrac > 1.0) dfFrac = 1.0;
						return pstSession->dtGateExit + static_cast<time_t>((dfCarryElapsedSec * dfFrac) + 0.5);
					}
					if (dfCarrySpeedMps > 0.0)
						return pstSession->dtGateExit + static_cast<time_t>((dfCumM / dfCarrySpeedMps) + 0.5);
					return pstSession->dtGateExit;
				};

				auto EmitForeignSpan = [&](double dfExitX, double dfExitY,
						time_t dtSpanStart, time_t dtSpanEnd)
				{
					if (vtForeignLinks.empty()) return;

					double dfForeignDistM = 0.0;
					for (size_t f = 0; f < vtForeignLinks.size(); ++f)
					{
						PLINK_INFO pstFLink = m_stConfig.pcDataLoader->GetLinkInfo(vtForeignLinks[f]);
						if (pstFLink != nullptr) dfForeignDistM += pstFLink->dfLen;
					}

					// ① 개방식 — 등록 여부는 첫 링크로 판정(같은 span 안에서 구역이 바뀌는 경우는
					//   드물다고 보고 미지원). M게이트가 이 span 안의 링크 위에 있으면 "경유 링크는
					//   정의상 전체 통과"라는 기존 원칙(CollectGateCandidatesOnIntermediateLinks와
					//   동일 근거)으로 통과 확정 — bStartedByTrip=true로 지어 BuildOpenZoneRow가
					//   구역 등록길이 대신 이 실측(추정) 거리를 쓰게 한다.
					vector<PZONE_INFO> vtOpenZones;
					m_stConfig.pcChargeDataLoader->GetOpenZonesByLinkId(vtForeignLinks[0], &vtOpenZones);
					if (!vtOpenZones.empty())
					{
						ZONE_RUN_SESSION stForeignRun;
						strncpy(stForeignRun.szRoadID, vtOpenZones[0]->szRoadID,
							sizeof(stForeignRun.szRoadID) - 1);
						stForeignRun.szRoadID[sizeof(stForeignRun.szRoadID) - 1] = '\0';
						stForeignRun.bStartedByTrip = true;
						PGATE_INFO pstMGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(
							stForeignRun.szRoadID, 'M');
						stForeignRun.bGateCrossed = false;
						if (pstMGate != nullptr)
						{
							for (size_t f = 0; f < vtForeignLinks.size(); ++f)
							{
								if (vtForeignLinks[f] == pstMGate->qwLinkID)
								{ stForeignRun.bGateCrossed = true; break; }
							}
						}
						stForeignRun.dtEntryTime = dtSpanStart;
						stForeignRun.dwEntryGpsSeq = pstSession->dwGateExitGpsSeq;
						stForeignRun.dfEntryX = dfForeignEntryX;
						stForeignRun.dfEntryY = dfForeignEntryY;
						stForeignRun.dfAccumDistM = dfForeignDistM;
						stForeignRun.dfLastX = dfExitX;
						stForeignRun.dfLastY = dfExitY;
						stForeignRun.dtLastInZoneTime = dtSpanEnd;
						stForeignRun.dwLastInZoneGpsSeq = pstSession->dwGateExitGpsSeq;

						CHARGE_INSERT_ROW stForeignRow;
						BuildOpenZoneRow(stForeignRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
							pstSession->nChargeSeq, dtSpanEnd, pstSession->dwGateExitGpsSeq,
							&stForeignRow);
						// [버그 수정, 2026-09-15 최정우, 사용자 지적] 사유코드 11 -> 12 교체.
						//   위에서 bStartedByTrip=true 로 지은 건 "구역 등록길이 대신 실측거리를 쓰게
						//   하려는 수단"일 뿐 실제로 트립이 구역 안에서 시작한 게 아니다. 그런데
						//   BuildOpenZoneRow()는 그 플래그만 보고 11번(트립 중간시작으로 진입게이트
						//   미통과)을 붙여, 실제 원인(GPS 간격 사이로 게이트 링크를 스쳐 지나감)과
						//   전혀 다른 문구가 남았다. N/3 판정 자체는 타당하므로 사유만 바로잡는다.
						if (stForeignRow.strNonChargeReason
								== to_string(static_cast<int>(NCR_OPEN_ENTRY_GATE_MISSED)))
						{
							char szForeignReason[8];
							snprintf(szForeignReason, sizeof(szForeignReason), "%d", NCR_OPEN_GATE_NOT_ON_PATH);
							stForeignRow.strNonChargeReason = szForeignReason;
						}
						stForeignRow.bNoMergeAfter = true;
						pvtChargeInserts->push_back(stForeignRow);

						// [버그 수정, 2026-09-15 최정우] non_charge_reason 필드 누락 보완 + N/x 는 WARN
						//   승격(2026-09-11 관례와 동일). 이 경로만 사유코드가 로그에 안 찍혀 DB 에만
						//   남던 상태였다(사용자 지적)
						const int nForeignReason = stForeignRow.strNonChargeReason.empty()
							? NCR_NORMAL : atoi(stForeignRow.strNonChargeReason.c_str());
						if (nForeignReason == NCR_NORMAL)
						{
							LOGFMTI("[#%02d] gap-recovered open zone registered!device=[%s] trip_id=[%s] "
								"seq=[%d] road=[%s] gate_crossed=[%d] dist_m=[%.1f]m non_charge_reason=[%d:%s]",
								nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
								pstSession->nChargeSeq, stForeignRun.szRoadID,
								static_cast<int>(stForeignRun.bGateCrossed), dfForeignDistM, nForeignReason,
								m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nForeignReason));
						}
						else
						{
							LOGFMTW("[#%02d] gap-recovered open zone registered(AUDIT)!device=[%s] trip_id=[%s] "
								"seq=[%d] road=[%s] gate_crossed=[%d] dist_m=[%.1f]m non_charge_reason=[%d:%s]",
								nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
								pstSession->nChargeSeq, stForeignRun.szRoadID,
								static_cast<int>(stForeignRun.bGateCrossed), dfForeignDistM, nForeignReason,
								m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), nForeignReason));
						}
						pstSession->nChargeSeq += 1;
						vtForeignLinks.clear();
						return;
					}

					// ② 면제도로 — 게이트 개념이 없어 등록만 되면 그대로(항상 Y/0, 사용자 확정 정책)
					vector<PZONE_INFO> vtExemptZones;
					m_stConfig.pcChargeDataLoader->GetExemptZonesByLinkId(vtForeignLinks[0], &vtExemptZones);
					// [2026-09-23 최정우 추가 — 사용자 지적, 같은유형 구간중복]
					//   **그 구역의 run 이 이미 열려 있으면 복구 행을 만들지 않는다.**
					//   엔진은 구역을 경로(aqwPathLinkIDs)로 판정하는데, 같은 tick 에서
					//   ① 여기(경로에 끼인 타 구역 복구)와 ② ProcessExemptZoneCharge 의 run 진입이
					//   **같은 구역을 각각 기록**해 한 구간이 두 행으로 쪼개졌다 — 실측 6건
					//   (000992 seq1 RL-Z00007 이 14m + 5m 로 분리, 전부 Y/0 이라 구간중복이 된다).
					//   사후 병합으로는 못 푼다 — 이 행은 아래에서 bNoMergeAfter=true 로 병합을
					//   **명시적으로 금지**하고 있어(경로 복구분이 뒤 구간에 흡수되면 위치가 틀어진다),
					//   중복을 만들고 합치는 게 아니라 **애초에 만들지 않는 것**이 맞다.
					//   run 이 열려 있다는 건 그 구역을 지금 정규 경로로 기록 중이라는 뜻이고,
					//   그 run 이 마감될 때 거리·구간이 온전히 들어간다.
					if (!vtExemptZones.empty())
					{
						//   [보완] 행만 안 만들고 끝내면 그 구간 거리가 **어디에도 안 들어간다**
						//   (실측: 억제만 했더니 13.8m x 6건 = 84m 손실). 열린 run 에 거리를 가산하고
						//   진입 시각도 복구 구간 시작으로 앞당긴다 — 거리만 더하면 분모(체류시간)가
						//   그대로라 평균속도가 부풀려진다(면제 이월 소급 적용 A-1 과 같은 근거).
						//   **START/END_GPS_SEQ 는 건드리지 않는다** — 넓히면 구간중복이 생긴다.
						ZONE_RUN_SESSION *pstOpenRun = nullptr;
						for (size_t ri = 0; ri < pstSession->vtExemptRuns.size(); ++ri)
						{
							if (strcmp(pstSession->vtExemptRuns[ri].szRoadID,
									vtExemptZones[0]->szRoadID) == 0)
							{ pstOpenRun = &pstSession->vtExemptRuns[ri]; break; }
						}
						if (pstOpenRun != nullptr)
						{
							pstOpenRun->dfAccumDistM += dfForeignDistM;
							const bool bEarlier = (dtSpanStart > 0)
								&& ((pstOpenRun->dtEntryTime == 0) || (dtSpanStart < pstOpenRun->dtEntryTime));
							if (bEarlier)
							{
								pstOpenRun->dtEntryTime = dtSpanStart;
								pstOpenRun->dfEntryX = dfForeignEntryX;
								pstOpenRun->dfEntryY = dfForeignEntryY;
							}
							// [2026-09-23 최정우 추가] **거리를 더했으면 시간도 늘려야 한다.**
							//   dtSpanStart 는 이월 기준 보간값이라 트립 첫 tick 에서는 유효하지
							//   않아(entry_moved=0) 위 분기가 안 걸린다. 그러면 분모(체류시간)가
							//   그대로라 평균속도가 부풀려진다 — 실측 58m/2초 = 104km/h(tick 실측 44).
							//   복구 행 체류시간 산정(FillUncoveredLinkRows)과 같은 원리로,
							//   **그 tick 의 실측 속도**로 더한 거리만큼의 소요시간을 역산해
							//   진입 시각을 앞당긴다. 실측 속도가 0 이면 근거가 없으므로 두지 않는다.
							else if ((stRawLogInfo.fSpeed > 0.1f) && (pstOpenRun->dtEntryTime > 0))
							{
								const time_t dtBack = static_cast<time_t>(
									(dfForeignDistM / (stRawLogInfo.fSpeed / 3.6)) + 0.5);
								if (dtBack > 0) pstOpenRun->dtEntryTime -= dtBack;
							}
							LOGFMTI("[#%02d] gap-recovered exempt zone merged into open run!device=[%s] "
								"trip_id=[%s] road=[%s] dist_m=[+%.1f]m total=[%.1f]m entry_moved=[%d]",
								nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
								vtExemptZones[0]->szRoadID, dfForeignDistM,
								pstOpenRun->dfAccumDistM, static_cast<int>(bEarlier));
							vtForeignLinks.clear();
							return;
						}

						ZONE_RUN_SESSION stForeignRun;
						strncpy(stForeignRun.szRoadID, vtExemptZones[0]->szRoadID,
							sizeof(stForeignRun.szRoadID) - 1);
						stForeignRun.szRoadID[sizeof(stForeignRun.szRoadID) - 1] = '\0';
						stForeignRun.dtEntryTime = dtSpanStart;
						stForeignRun.dwEntryGpsSeq = pstSession->dwGateExitGpsSeq;
						stForeignRun.dfEntryX = dfForeignEntryX;
						stForeignRun.dfEntryY = dfForeignEntryY;
						stForeignRun.dfAccumDistM = dfForeignDistM;
						stForeignRun.dfLastX = dfExitX;
						stForeignRun.dfLastY = dfExitY;
						stForeignRun.dtLastInZoneTime = dtSpanEnd;
						stForeignRun.dwLastInZoneGpsSeq = pstSession->dwGateExitGpsSeq;

						CHARGE_INSERT_ROW stForeignRow;
						BuildExemptRow(stForeignRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
							pstSession->nChargeSeq, dtSpanEnd, pstSession->dwGateExitGpsSeq,
							"Y", "0", &stForeignRow);
						stForeignRow.bNoMergeAfter = true;
						pvtChargeInserts->push_back(stForeignRow);

						LOGFMTI("[#%02d] gap-recovered exempt zone registered!device=[%s] trip_id=[%s] "
							"seq=[%d] road=[%s] dist_m=[%.1f]m non_charge_reason=[%d:%s]",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
							pstSession->nChargeSeq, stForeignRun.szRoadID, dfForeignDistM, NCR_NORMAL,
							m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
						pstSession->nChargeSeq += 1;
						vtForeignLinks.clear();
						return;
					}

					// ③ 폐쇄식 — link_id→road_id 역인덱스가 없어(게이트 기반) 이 span 의 링크마다
					//   게이트 유무를 직접 조회한다. 입구('I'/'B')·출구('O'/'B') 게이트가 같은
					//   road_id 로 이 span 안에서 둘 다 발견돼야만 정식 등록 — 한쪽만 있으면 나머지
					//   경계를 모르므로 소급 확정하지 않는다(과다청구 방지가 우선).
					PGATE_INFO pstEntryGate = nullptr;
					PGATE_INFO pstExitGate = nullptr;
					for (size_t f = 0; f < vtForeignLinks.size(); ++f)
					{
						PGATE_INFO pstAnyGate = m_stConfig.pcChargeDataLoader->GetGateByLinkId(
							vtForeignLinks[f], 0);
						if (pstAnyGate == nullptr) continue;
						vector<PGATE_INFO> vtLinkGates;
						m_stConfig.pcChargeDataLoader->GetGatesByLinkId(vtForeignLinks[f], 'I', &vtLinkGates);
						vector<PGATE_INFO> vtLinkGatesB;
						m_stConfig.pcChargeDataLoader->GetGatesByLinkId(vtForeignLinks[f], 'B', &vtLinkGatesB);
						vtLinkGates.insert(vtLinkGates.end(), vtLinkGatesB.begin(), vtLinkGatesB.end());
						if (!vtLinkGates.empty() && (pstEntryGate == nullptr))
							pstEntryGate = vtLinkGates[0];

						vector<PGATE_INFO> vtLinkGatesO;
						m_stConfig.pcChargeDataLoader->GetGatesByLinkId(vtForeignLinks[f], 'O', &vtLinkGatesO);
						vector<PGATE_INFO> vtLinkGatesB2;
						m_stConfig.pcChargeDataLoader->GetGatesByLinkId(vtForeignLinks[f], 'B', &vtLinkGatesB2);
						vtLinkGatesO.insert(vtLinkGatesO.end(), vtLinkGatesB2.begin(), vtLinkGatesB2.end());
						if (!vtLinkGatesO.empty() && (pstExitGate == nullptr))
							pstExitGate = vtLinkGatesO[0];
					}
					if ((pstEntryGate != nullptr) && (pstExitGate != nullptr)
						&& (strcmp(pstEntryGate->szRoadID, pstExitGate->szRoadID) == 0))
					{
						PZONE_INFO pstClosedZone =
							m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstEntryGate->szRoadID);
						CHARGE_INSERT_ROW stForeignRow;
						stForeignRow.strTripId = stRawLogInfo.szTripID;
						stForeignRow.strDeviceKey = stRawLogInfo.szDeviceKey;
						char szSeq[16];
						snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
						stForeignRow.strChargeSeq = szSeq;
						stForeignRow.strChargeType = "2";
						stForeignRow.strChargeUnit = "1";
						stForeignRow.strLinkId = "";
						stForeignRow.strFromId = pstEntryGate->szTollgateID;
						stForeignRow.strToId = pstExitGate->szTollgateID;
						char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
						snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstEntryGate->dfLat);
						snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstEntryGate->dfLon);
						snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
						snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);
						stForeignRow.strFromLat = szFromLat;
						stForeignRow.strFromLon = szFromLon;
						stForeignRow.strToLat = szToLat;
						stForeignRow.strToLon = szToLon;
						stForeignRow.strZoneId = pstEntryGate->szRoadID;
						stForeignRow.strZoneName = (pstClosedZone != nullptr) ? pstClosedZone->szRoadNm : "";
						char szDistM[16];
						snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfForeignDistM + 0.5));
						stForeignRow.strDistM = szDistM;
						// [2026-09-21 최정우 수정 — 이슈 27] 종전엔 무조건 1초였다(실측 tick 이 없어 경과시간
						//   산출 불가라는 이유). 이제는 게이트 진출 tick ~ 이번 tick 시간 창을 거리 비율로
						//   배분한 값을 쓰고, 배분이 불가능할 때만 종전대로 1초 하한으로 떨어진다
						double dfElapsedSec = difftime(dtSpanEnd, dtSpanStart);
						if (dfElapsedSec < 1.0) dfElapsedSec = 1.0;
						char szSpeedKmh[16];
						snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d",
							static_cast<int>((dfForeignDistM / dfElapsedSec) * 3.6 + 0.5));
						stForeignRow.strSpeedKmh = szSpeedKmh;
						stForeignRow.strSpeedLimitKmh = "";
						stForeignRow.strOccurDt = FormatDateTime14(dtSpanStart);
						const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
						stForeignRow.strTripStartDt =
							(pszTripStartDt != nullptr) ? pszTripStartDt : stForeignRow.strOccurDt;
						stForeignRow.strTollgateId = "";
						stForeignRow.strEntryTollgateId = pstEntryGate->szTollgateID;
						stForeignRow.strExitTollgateId = pstExitGate->szTollgateID;
						stForeignRow.strRegDt = FormatDateTime14(time(nullptr));
						stForeignRow.strUpdDt = stForeignRow.strRegDt;
						stForeignRow.strChargeYn = "Y";
						stForeignRow.strChargeStatus = "0";
						char szStaySeconds[16];
						snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec));
						stForeignRow.strStaySeconds = szStaySeconds;
						char szStartGpsSeq[16];
						snprintf(szStartGpsSeq, sizeof(szStartGpsSeq), "%u", pstSession->dwGateExitGpsSeq);
						stForeignRow.strStartGpsSeq = szStartGpsSeq;
						stForeignRow.strEndGpsSeq = szStartGpsSeq;
						stForeignRow.bNoMergeAfter = true;
						pvtChargeInserts->push_back(stForeignRow);

						LOGFMTI("[#%02d] gap-recovered closed zone registered!device=[%s] trip_id=[%s] "
							"seq=[%d] road=[%s] entry=[%s] exit=[%s] dist_m=[%.1f]m non_charge_reason=[%d:%s]",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
							pstSession->nChargeSeq, pstEntryGate->szRoadID, pstEntryGate->szTollgateID,
							pstExitGate->szTollgateID, dfForeignDistM, NCR_NORMAL,
							m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));
						pstSession->nChargeSeq += 1;
						vtForeignLinks.clear();
						return;
					}

					// 어느 쪽으로도 확정 못 함 — 기존 정책대로 제외(소액 손실 감수, 과다청구 방지 우선)
					LOGFMTW("[#%02d] gap-recovered foreign span unconfirmed(dropped)!device=[%s] "
						"trip_id=[%s] seq=[%d] first_link=[%llu] dist_m=[%.1f]m",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						pstSession->nChargeSeq, static_cast<unsigned long long>(vtForeignLinks[0]),
						dfForeignDistM);
					vtForeignLinks.clear();
				};

				// 누적거리 커서 — 각 조각의 시작·끝 시각을 fnCarryTimeAt() 으로 내기 위한 기준
				//   (2026-09-21 최정우 추가, 이슈 27)
				double dfCumCarryM = 0.0;			// 게이트 진출점 ~ 지금 보는 링크의 시작까지
				double dfIslandStartCumM = 0.0;		// 진행 중인 일반도로 섬이 시작된 누적거리
				double dfForeignStartCumM = 0.0;	// 진행 중인 타 과금유형 span 이 시작된 누적거리

				for (size_t g = 1; (g + 1 < vtCarryPath.size()) && bAllLenOk; ++g)
				{
					PLINK_INFO pstCarryLink = m_stConfig.pcDataLoader->GetLinkInfo(vtCarryPath[g]);
					if (pstCarryLink == nullptr) { bAllLenOk = false; break; }

					if (m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(vtCarryPath[g]))
					{
						if (!vtForeignLinks.empty())
						{
							double dfExitX = static_cast<double>(pstCarryLink->dwStNodeX) / 360000.0;
							double dfExitY = static_cast<double>(pstCarryLink->dwStNodeY) / 360000.0;
							EmitForeignSpan(dfExitX, dfExitY,
								fnCarryTimeAt(dfForeignStartCumM), fnCarryTimeAt(dfCumCarryM));
							dfIslandEntryX = dfExitX;
							dfIslandEntryY = dfExitY;
						}
						if (qwIslandFromLinkID == 0)
						{
							qwIslandFromLinkID = vtCarryPath[g];
							dfIslandStartCumM = dfCumCarryM;		// 이 섬의 시작 지점
						}
						dfIslandDistM += pstCarryLink->dfLen;
						qwIslandLastLinkID = vtCarryPath[g];
						dfCumCarryM += pstCarryLink->dfLen;
						continue;
					}

					// 다른 과금유형 등록 링크 — 지금까지 쌓인 섬이 있으면 독립 레코드로 즉시 마감
					if (qwIslandFromLinkID != 0)
					{
						double dfIslandExitX = static_cast<double>(pstCarryLink->dwStNodeX) / 360000.0;
						double dfIslandExitY = static_cast<double>(pstCarryLink->dwStNodeY) / 360000.0;

						// 이 섬이 차지한 시간 창 — 누적거리 비율로 배분 (2026-09-21 최정우 수정, 이슈 27).
						//   종전엔 시작·끝 모두 게이트 진출 tick 이라 STAY_SECONDS 가 항상 0 이었다
						const time_t dtIslandStart = fnCarryTimeAt(dfIslandStartCumM);
						const time_t dtIslandEnd = fnCarryTimeAt(dfCumCarryM);

						ZONE_RUN_SESSION stIslandRun;
						stIslandRun.dtEntryTime = dtIslandStart;
						stIslandRun.dwEntryGpsSeq = pstSession->dwGateExitGpsSeq;
						stIslandRun.dfEntryX = dfIslandEntryX;
						stIslandRun.dfEntryY = dfIslandEntryY;
						stIslandRun.qwEntryLinkID = qwIslandFromLinkID;
						stIslandRun.dfAccumDistM = dfIslandDistM;
						stIslandRun.qwLastLinkID = qwIslandLastLinkID;
						stIslandRun.dfLastX = dfIslandExitX;
						stIslandRun.dfLastY = dfIslandExitY;
						stIslandRun.dtLastInZoneTime = dtIslandEnd;
						stIslandRun.dwLastInZoneGpsSeq = pstSession->dwGateExitGpsSeq;

						CHARGE_INSERT_ROW stIslandRow;
						BuildNodeStepRow(stIslandRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
							pstSession->nChargeSeq, dtIslandEnd, pstSession->dwGateExitGpsSeq,
							"Y", "0", &stIslandRow);
						// gps_seq가 게이트 진출 tick과 겹치거나 붙어 있어 MergeAdjacentNodeStepRows()가
						//   바로 뒤 일반도로 행과 다시 합쳐버릴 수 있다 — 사이에 실제로 다른 과금유형
						//   등록 링크가 껴 있어 진짜로 끊긴 구간이므로 병합 금지 표시 (2026-09-14 최정우 추가)
						stIslandRow.bNoMergeAfter = true;
						pvtChargeInserts->push_back(stIslandRow);

						LOGFMTI("[#%02d] node step island split(mid-path other charge type)!device=[%s] "
							"trip_id=[%s] seq=[%d] from_link=[%llu] dist_m=[%.1f]m skip_link=[%llu] "
							"stay=[%.0f]s",
							nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
							pstSession->nChargeSeq, static_cast<unsigned long long>(qwIslandFromLinkID),
							dfIslandDistM, static_cast<unsigned long long>(vtCarryPath[g]),
							difftime(dtIslandEnd, dtIslandStart));

						pstSession->nChargeSeq += 1;
						qwIslandFromLinkID = 0;
						qwIslandLastLinkID = 0;
						dfIslandDistM = 0.0;
					}

					if (vtForeignLinks.empty())
					{
						dfForeignEntryX = dfIslandEntryX;
						dfForeignEntryY = dfIslandEntryY;
						dfForeignStartCumM = dfCumCarryM;			// 이 span 의 시작 지점
					}
					vtForeignLinks.push_back(vtCarryPath[g]);
					dfCumCarryM += pstCarryLink->dfLen;

					// 다음 섬은(타 과금유형 span 이 끝나면) 이 등록 링크의 끝 노드부터 시작 —
					//   span 이 더 이어지면 아래에서 다시 갱신되므로 무해하다
					dfIslandEntryX = static_cast<double>(pstCarryLink->dwEdNodeX) / 360000.0;
					dfIslandEntryY = static_cast<double>(pstCarryLink->dwEdNodeY) / 360000.0;
					dfIslandStartCumM = dfCumCarryM;				// 다음 섬 후보 시작점
				}
				if (bAllLenOk)
				{
					POINT stLinkStart, stCurPt2;
					stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
					stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
					stCurPt2.dfX = stMatchLinkInfo.dfMatchX;
					stCurPt2.dfY = stMatchLinkInfo.dfMatchY;

					// 경로 끝까지 타 과금유형 span 이 안 닫힌 채 목적지에 도달 — 목적지 링크의
					//   시작 노드를 그 span 의 종료 지점으로 보고 마감한다
					if (!vtForeignLinks.empty())
					{
						// 남은 span 의 종료 시각은 이 지점까지의 누적거리 기준 (2026-09-21 최정우 수정)
						EmitForeignSpan(stLinkStart.dfX, stLinkStart.dfY,
							fnCarryTimeAt(dfForeignStartCumM), fnCarryTimeAt(dfCumCarryM));
						dfIslandEntryX = stLinkStart.dfX;
						dfIslandEntryY = stLinkStart.dfY;
					}

					dfIslandDistM += HaversineMeters(stLinkStart, stCurPt2);

					dfCarryDistM = dfIslandDistM;
					qwCarryFromLinkID = qwIslandFromLinkID;
					// 마지막 섬이 원래 게이트 지점과 다른 곳에서 시작하면(중간에 분리됐으면) 호출측
					//   run 의 진입좌표도 이 마지막 섬 기준으로 갱신한다 — 분리가 없었으면 게이트
					//   지점과 동일해 변화 없음
					pstRunForCarry->dfEntryX = dfIslandEntryX;
					pstRunForCarry->dfEntryY = dfIslandEntryY;
					// 마지막 섬의 **진입 시각**도 같은 근거로 갱신한다 — 분리가 없었으면
					//   누적거리 0 이라 게이트 진출 시각 그대로다 (2026-09-21 최정우 추가, 이슈 27)
					// **이번 tick 시각을 넘지 않게 자른다** — 위 순간속도 폴백(시간 창이 0 인 경우)은
					//   복구 거리를 속도로 나눠 시각을 만들기 때문에 합이 실제 tick 간격을 넘어설 수
					//   있다. 그러면 호출측 run 의 진입 시각이 **자기 첫 실측 tick 보다 뒤**가 되는
					//   모순이 생긴다(실측 000376_20260819141002: run 첫 tick 은 seq40=14:12:02 인데
					//   진입 시각이 14:12:11 로 계산됐다). 작은 복구 조각들이 tick 을 조금 넘어서는
					//   것은 감수하되, **실측 tick 이 있는 run 의 경계는 실측을 우선**한다
					//   (2026-09-21 최정우 추가)
					time_t dtLastIslandStart = fnCarryTimeAt(dfIslandStartCumM);
					if (dtLastIslandStart > stRawLogInfo.dtGPS)
						dtLastIslandStart = stRawLogInfo.dtGPS;
					pstRunForCarry->dtEntryTime = dtLastIslandStart;
					bCarryPathApplied = true;
				}
			}
		}
		if (!bCarryPathApplied)
		{
			POINT stGatePt, stCurPt;
			stGatePt.dfX = pstSession->dfGateExitX;  stGatePt.dfY = pstSession->dfGateExitY;
			stCurPt.dfX = stMatchLinkInfo.dfMatchX;  stCurPt.dfY = stMatchLinkInfo.dfMatchY;
			dfCarryDistM = HaversineMeters(stGatePt, stCurPt);
		}
		if (qwCarryFromLinkID != 0)
			pstRunForCarry->qwEntryLinkID = qwCarryFromLinkID;
		return dfCarryDistM;
	};

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtNodeStepRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtNodeStepRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;

		// 신뢰할 수 없는 tick(사후 SKIP 재판정 등)은 새 run 을 열지 않는다 — 아래 미등록
		//   pseudo-zone 개시 분기(2026-09-07 추가)와 동일 가드를 등록구역에도 적용. 이게 없으면
		//   CommitPendingRow 사후보정으로 SKIP 재판정된 링크가 등록 NODE_STEP 구역일 때 실측되지
		//   않은 좌표로 run 이 개시된다(2026-09-11 최정우 추가)
		if (!bTrustedMatch) continue;

		ZONE_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';
		// 진입 경계(구역에 들어온 링크의 시작 노드) 통과 시각 보간 — ProcessOpenGateCharge() 동일
		//   근거 참고. 직전 tick 이 없으면(트립 첫 tick) 그대로 원시 시각 사용
		//   (2026-08-25 최정우 추가, 사용자 지시)
		if (pstSession->bHasLastMatch)
		{
			stRun.dtEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				stMatchLinkInfo.dfStNodeX, stMatchLinkInfo.dfStNodeY);
		}
		else
		{
			stRun.dtEntryTime = stRawLogInfo.dtGPS;
		}
		stRun.dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
		stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
		stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
		stRun.dfAccumDistM = 0.0;
		stRun.dfLastX = stMatchLinkInfo.dfMatchX;
		stRun.dfLastY = stMatchLinkInfo.dfMatchY;
		stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
		stRun.qwEntryLinkID = stMatchLinkInfo.qwLinkID;			// 2026-09-01 최정우 추가 — 미등록 pseudo-zone FROM_ID용
		// 진입 시각으로 미리 채워둔다 — 진입 후 단 1tick만 존재하고 바로 이탈하는 run(다음 tick에서
		//   bSameZone=true 를 한 번도 못 만남)은 이 값이 기본값(0)인 채로 남아, 이탈 처리부의
		//   InterpolateGateCrossingTime()에 "구역 안 마지막 확정시각"으로 0(1970년)이 그대로
		//   들어가 엉뚱한 과거 시각(occur_dt)·end_gps_seq=0 이 나오는 버그였다(사용자 지시,
		//   2026-09-02 최정우 추가 — 실측 000370_20260824135458 trip_seq=4, occur_dt=19850104...,
		//   stay_seconds=-1313903640)
		stRun.dtLastInZoneTime = stRun.dtEntryTime;
		stRun.dwLastInZoneGpsSeq = stRun.dwEntryGpsSeq;

		// 게이트형 구역(개방식·폐쇄식) 진출 지점 이어받기 — 케이스2와 동일 근거(위 공용 람다
		//   ApplyGateExitCarryDist 주석 참고). 병합 이월(stMergeCarry)이 있으면 그쪽이 우선한다
		//   (2026-09-14 최정우 추가 — 정식 등록구역 진입에도 이 이월이 빠져 있던 문제 수정)
		if (!pstSession->bHasMergeCarry && pstSession->bHasGateExitCarry)
		{
			stRun.dtEntryTime = pstSession->dtGateExit;
			stRun.dfEntryX = pstSession->dfGateExitX;
			stRun.dfEntryY = pstSession->dfGateExitY;
			if (pstSession->bGateExitAtTick)
				stRun.dwEntryGpsSeq = pstSession->dwGateExitGpsSeq;
			stRun.dfAccumDistM += ApplyGateExitCarryDist(&stRun);
			// 이 tick 이 아직 진출한 구역 링크 위면 FROM_ID 확정을 다음 tick 으로 미룬다
			//   (qwPendingEntryFromLinkID 필드 주석 참고, 2026-09-16 최정우 추가)
			if ((pstSession->qwGateExitLinkID != 0)
				&& (stRun.qwEntryLinkID == pstSession->qwGateExitLinkID))
				stRun.qwPendingEntryFromLinkID = pstSession->qwGateExitLinkID;
			pstSession->bHasGateExitCarry = false;
			pstSession->bGateExitAtTick = false;
		}

		// 위 ①에서 병합 상대를 못 찾고 이월된 run 이 있으면 이 새 run 이 이어받는다 — 진입정보는
		//   더 이른 시각(이월분)으로 덮어쓰고 누적거리를 더한다 (2026-09-01 최정우 추가)
		if (pstSession->bHasMergeCarry)
		{
			stRun.dtEntryTime = pstSession->stMergeCarry.dtEntryTime;
			stRun.dfEntryX = pstSession->stMergeCarry.dfEntryX;
			stRun.dfEntryY = pstSession->stMergeCarry.dfEntryY;
			stRun.dwEntryGpsSeq = pstSession->stMergeCarry.dwEntryGpsSeq;
			stRun.qwEntryLinkID = pstSession->stMergeCarry.qwEntryLinkID;
			stRun.dfAccumDistM += pstSession->stMergeCarry.dfAccumDistM;
			// [2026-09-22 최정우 추가] 이월이 덮던 링크도 새 run 으로 넘긴다
			for (size_t li = 0; li < pstSession->stMergeCarry.vtRunLinks.size(); ++li)
				stRun.vtRunLinks.push_back(pstSession->stMergeCarry.vtRunLinks[li]);
			// 주정차 폴리곤 이탈이 확정한 진입 정보라는 표시도 함께 넘긴다 — 안 넘기면 뒤이은
			//   면제 이월이 FROM_ID 를 덮어쓴다(사용자 지적, 2026-09-22 최정우 추가)
			if (pstSession->stMergeCarry.bEntryFixedByParkExit)
				stRun.bEntryFixedByParkExit = true;
			// 이월분이 이미 갖고 있던 "구역 안 마지막 확정시각"도 같이 이어받는다(2026-09-02 최정우 추가)
			stRun.dtLastInZoneTime = pstSession->stMergeCarry.dtLastInZoneTime;
			stRun.dwLastInZoneGpsSeq = pstSession->stMergeCarry.dwLastInZoneGpsSeq;
			pstSession->bHasMergeCarry = false;
		}
		AppendTickLinksToRun(&stRun.vtRunLinks, stMatchLinkInfo);	// run 개시 tick 링크 커버 (2026-10-04 최정우 추가)
		pstSession->vtNodeStepRuns.push_back(stRun);

		LOGFMTI("[#%02d] node step entry!device=[%s] trip_id=[%s] seq=[%u] road=[%s] open=[%zu] "
			"entry_gps_seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			stRun.szRoadID, pstSession->vtNodeStepRuns.size(), stRun.dwEntryGpsSeq);
	}

	// NODE_STEP 일반도로 확장(케이스2) — 미등록 pseudo-zone(szRoadID=="") 진입. 위 정식구역 루프와
	//   달리 vtZones 에 대응 항목이 없어(실제 ZONE_INFO가 없음) 별도 처리 (2026-09-01 최정우 추가)
	if (bTouchesUnregistered)
	{
		bool bPseudoOpen = false;
		for (size_t si = 0; si < pstSession->vtNodeStepRuns.size(); ++si)
		{
			if (pstSession->vtNodeStepRuns[si].szRoadID[0] == '\0') { bPseudoOpen = true; break; }
		}

		if (!bPseudoOpen)
		{
			ZONE_RUN_SESSION stRun;
			stRun.szRoadID[0] = '\0';
			if (pstSession->bHasLastMatch)
			{
				stRun.dtEntryTime = InterpolateGateCrossingTime(
					pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
					stMatchLinkInfo.dfStNodeX, stMatchLinkInfo.dfStNodeY);
			}
			else
			{
				stRun.dtEntryTime = stRawLogInfo.dtGPS;
			}
			stRun.dwEntryGpsSeq = stRawLogInfo.dwSeqNo;
			stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
			stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
			stRun.dfAccumDistM = 0.0;
			stRun.dfLastX = stMatchLinkInfo.dfMatchX;
			stRun.dfLastY = stMatchLinkInfo.dfMatchY;
			if (!bTrustedMatch)
				return;					// 신뢰할 수 없는 tick 은 새 run 을 열지 않는다(헤더 주석 참고)
			stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
			stRun.qwEntryLinkID = stMatchLinkInfo.qwLinkID;
			// 진입 시각으로 미리 채워둔다 — 위 등록구역 루프와 동일 근거(1tick-only run 버그 방지,
			//   2026-09-02 최정우 추가)
			stRun.dtLastInZoneTime = stRun.dtEntryTime;
			stRun.dwLastInZoneGpsSeq = stRun.dwEntryGpsSeq;

			// 게이트형 구역 진출 지점 이어받기 — 진출게이트가 곧 일반도로의 시작이다.
			//   병합 이월(stMergeCarry)이 있으면 그쪽이 우선한다(더 앞선 구간이므로).
			//   (2026-09-06 최정우 추가, 사용자 지시)
			if (!pstSession->bHasMergeCarry && pstSession->bHasGateExitCarry)
			{
				stRun.dtEntryTime = pstSession->dtGateExit;
				stRun.dfEntryX = pstSession->dfGateExitX;
				stRun.dfEntryY = pstSession->dfGateExitY;
				// 게이트가 그 tick 매칭점과 일치할 때만 순번을 공유한다(세션 필드 주석 참고).
				//   아니면 게이트는 구역 안쪽이므로 일반도로의 첫 실측 tick 은 이번 tick 이다
				if (pstSession->bGateExitAtTick)
					stRun.dwEntryGpsSeq = pstSession->dwGateExitGpsSeq;
				// FROM_ID 는 게이트가 놓인 링크(=구역 링크)로 덮어쓰지 않는다 — 그러면 일반도로
				//   레코드가 구역 링크에서 시작한 것처럼 보여 같은 구간이 두 유형으로 중복 계상된다
				//   (실측 000376_20260819140856: 폐쇄식 RL-Z00005 링크 2040423801 이 일반도로
				//   FROM_ID 로 찍혔다). 좌표는 게이트(구역 경계)를 쓰되 링크 ID 는 이 run 이 실제로
				//   열린 구역 밖 링크를 유지한다 (2026-09-06 최정우 수정, 사용자 지시). 이월 지점~이번
				//   tick 사이 누락 링크 복구는 ApplyGateExitCarryDist() 공용 람다 참고(위 주석).
				stRun.dfAccumDistM += ApplyGateExitCarryDist(&stRun);
				// 위 케이스1 과 동일 근거 (2026-09-16 최정우 추가)
				if ((pstSession->qwGateExitLinkID != 0)
					&& (stRun.qwEntryLinkID == pstSession->qwGateExitLinkID))
					stRun.qwPendingEntryFromLinkID = pstSession->qwGateExitLinkID;
				pstSession->bHasGateExitCarry = false;
				pstSession->bGateExitAtTick = false;
			}

			// 일반도로 연속 구간 병합 이월분 이어받기 — 위 등록구역 루프와 동일 근거
			//   (2026-09-01 최정우 추가)
			if (pstSession->bHasMergeCarry)
			{
				stRun.dtEntryTime = pstSession->stMergeCarry.dtEntryTime;
				stRun.dfEntryX = pstSession->stMergeCarry.dfEntryX;
				stRun.dfEntryY = pstSession->stMergeCarry.dfEntryY;
				stRun.dwEntryGpsSeq = pstSession->stMergeCarry.dwEntryGpsSeq;
				stRun.qwEntryLinkID = pstSession->stMergeCarry.qwEntryLinkID;
				stRun.dfAccumDistM += pstSession->stMergeCarry.dfAccumDistM;
				// [2026-09-22 최정우 추가] 이월이 덮던 링크도 새 run 으로 넘긴다
				for (size_t li = 0; li < pstSession->stMergeCarry.vtRunLinks.size(); ++li)
					stRun.vtRunLinks.push_back(pstSession->stMergeCarry.vtRunLinks[li]);
				// 주정차 폴리곤 이탈 확정 표시도 함께 (2026-09-22 최정우 추가)
				if (pstSession->stMergeCarry.bEntryFixedByParkExit)
					stRun.bEntryFixedByParkExit = true;
				// 이월분이 이미 갖고 있던 "구역 안 마지막 확정시각"도 같이 이어받는다 — 안 그러면
				//   위에서 이번 tick 시각으로 채운 값이 이월된 더 이른 dtEntryTime보다 앞서는
				//   모순이 생긴다(2026-09-02 최정우 추가)
				stRun.dtLastInZoneTime = pstSession->stMergeCarry.dtLastInZoneTime;
				stRun.dwLastInZoneGpsSeq = pstSession->stMergeCarry.dwLastInZoneGpsSeq;
				pstSession->bHasMergeCarry = false;
			}
			AppendTickLinksToRun(&stRun.vtRunLinks, stMatchLinkInfo);	// run 개시 tick 링크 커버 (2026-10-04 최정우 추가)
			pstSession->vtNodeStepRuns.push_back(stRun);

			// seq·신뢰여부를 함께 남긴다 — 어느 tick 이 run 을 열었는지 로그만으로 추적할 수 있어야
			//   한다(2026-09-07 최정우 추가, 실측 000376_20260819140532 추적 중 로그에 seq 가 없어
			//   어느 tick 이 여는지 못 짚었다)
			LOGFMTI("[#%02d] node step entry(unregistered)!device=[%s] trip_id=[%s] seq=[%u] link=[%llu] "
				"trusted=[%d] open=[%zu]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
				bTrustedMatch ? 1 : 0, pstSession->vtNodeStepRuns.size());
		}
	}
}

/*
 * ════════════════════════════════════════════════════════════════════════════════════════════
 *  [정책 대조표] 주정차 단속구역(폴리곤) 안 주행의 주정차·일반도로·구간단속 판정
 *  2026-10-04 최초 작성(사용자 요청). **정책이나 구현이 바뀌면 이 표를 먼저 갱신할 것.**
 *  다른 PC 공유용 요약(git 추적): doc/과금_등록_로직_정본.md — 이 표를 고치면 그 문서 5·7절도 함께 고친다.
 *  상태 표기: [현행]=지금 코드 동작 / [유지]=목표에서도 그대로 / [신규]=목표, 미구현 / [미정]=권장안
 * ════════════════════════════════════════════════════════════════════════════════════════════
 *  A. 현행 (2026-10-04 코드 기준)
 *   A1 [현행] 구역 안 원시좌표 + 매칭 실패(SKIP) -> 주정차(규칙1). 여기서 "안"은 폴리곤 경계 밖
 *        min(ACCURACY_M, park_pad=10)m 까지 포함한다(주정차 쪽만의 여유, 일반도로 억제는 여유 0).
 *        규칙5: 매칭이 실패(또는 저신뢰)하고 10 < ACCURACY_M <= 50 이면 그 여유를 ACCURACY_M 까지
 *        넓힌다 — 경계 밖 14m 에 찍힌 ACCURACY_M=18 tick 도 주정차(세션 개시·유지 공통).
 *        ACCURACY_M > park_accmax(50) 좌표는 판정에서 뺀다(정보 없음).
 *        일반도로 함수는 SKIP tick 에서 호출되지 않는다.
 *   A2 [현행] 매칭좌표가 폴리곤 안 -> 일반도로 억제(BuildNodeStepRow 헤더 규칙6, 속도 무관).
 *        **구간단속은 억제하지 않는다** — 폴리곤 안에서도 거리 누적·SPEED 행·일반도로 미러에 포함된다.
 *        개방·폐쇄·면제도 폴리곤을 참조하지 않는다.
 *   A3 [현행] 원시 안 + 매칭 밖(주행 중, 신뢰 매칭) -> 주정차 아님(규칙4). 정지(<1km/h)면 규칙4를
 *        적용하지 않아 주정차. **매칭 링크가 직전 링크에서 주행 가능한지(누락 링크 복구 포함)를 따로
 *        검사하지 않는다** — 맵매칭 자체의 연결성(CONTINUE 의 TURN_INFO 탐색)에만 기댄다. BEGIN
 *        폴백 매칭은 비연결 링크도 나올 수 있어 이 경우 검증이 비어 있다.
 *   A4 [현행] 원시 밖 + 매칭 안(주행 중, 신뢰 매칭) -> 주정차(규칙3). 일반도로는 A2 로 억제된다.
 *   A5 [현행] 원시 안 + 매칭 안 -> 주정차(규칙2).
 *   A6 [현행] 체류시간 = 진입 경계 보간 시각 ~ 진출 경계 보간 시각(InterpolateZoneCrossingTime).
 *        진입 park_entrycnt(3) 연속, 이탈 park_exitcnt(3) 연속. 구역 안에서 트립이 끝나면 종료는
 *        마지막 구역 안 tick. base_parking_fine 최소시간 미만이면 행을 만들지 않는다.
 *   A7 [현행, 사각지대] 폴리곤 통과 주행: 일반도로는 A2 로 억제, 체류가 최소시간 미만이면 주정차
 *        행도 없음 -> **통과 거리가 어느 행에도 들어가지 않는다.**
 *
 *  B. 목표 정책 (사용자 정책안, 2026-10-04) — **현행 A 는 모두 유지하고 보완·추가하는 것**이다(사용자
 *     확정). 주정차 판정(규칙1~5)은 바꾸지 않고, 폴리곤 안 주행을 일반도로로도 등록하는 기능을 더한다.
 *   B1 [유지] A6 체류시간 산출.
 *   B2 [신규] 폴리곤 안이라도 매칭 링크가 다른 과금유형(개방·폐쇄·면제)에 등록된 링크가 아니면
 *        일반도로로 등록한다(A2 억제 폐지). 구간단속 링크는 지금처럼 구간단속 + 미러.
 *   B3 [신규] 진출 쪽 매칭이 직전 신뢰 tick 에서 경과시간 안에 도달할 수 없는 위치면 SKIP 하고,
 *        그 tick 은 주정차 체류에 포함한다.
 *   B4 [**E16 으로 대체, 2026-10-04** — 이제 안쪽도 도착 tick 까지 일반도로] 트립이 구역 안에서 끝나면 구역 안 구간은 일반도로에 넣지 않는다 —
 *        그 구간은 **주정차 정체시간**이다: 진입 경계(출발이 같은 폴리곤 안이면 출발 tick)부터 도착
 *        tick 까지. 현행 코드가 이미 이렇게 동작한다(시나리오 S4 진입 경계~seq128 373s, S9 출발~도착
 *        372s). 체류가 base_parking_fine 최소시간 미만이면 주정차 행도 없으므로 그 안쪽 주행은 어디에도
 *        청구되지 않는다(정책상 귀결).
 *   B5 [유지] 진입 전·진출 후의 일반도로·구간단속 미러·복구 링크(0·3·미등록)는 하나의 연속
 *        일반도로로 병합(MergeAdjacentNodeStepRows, IsCase3EligibleRoadKind).
 *   B6 [확정] 주정차 행과 일반도로 행의 GPS_SEQ 범위가 겹쳐도 된다 — 구간단속 행과 그 일반도로 미러가
 *        겹치는 것과 같은 규칙(유형이 달라 이중과금 아님). (구 C6, 2026-10-04 사용자 확정)
 *   B7 [확정, 현행 유지] 폴리곤 안 구간단속은 지금 그대로 등록한다. 주정차로 단속된 경우에도 구간단속
 *        위반 여부·등록 여부는 현행 로직 그대로이고, 그 구간은 미러로 일반도로에도 등록된다.
 *   B8 [신규] 폴리곤 진입 전·진출 후·전후 모두가 구간단속 도로인 경우, 폴리곤 안 일반도로는 구간단속
 *        미러와 **같은 일반도로 판정 로직**으로 이어 붙인다 — 미러(전) + 폴리곤 안(B2) + 미러(후)를
 *        하나의 연속 일반도로로(B5 와 같은 병합 규칙). 구간단속 run 이 열려 있는 동안은
 *        IsLinkNodeStepEligible 이 이미 일반도로를 빼므로 미러와 B2 가 이중 계상되지 않는다.
 *   B9 [확정·보완 필요] 주정차 체류시간은 **폴리곤 경계라인** 통과 시각 기준. 현행 보간
 *        (InterpolateZoneCrossingTime)은 원시좌표가 경계라인 안일 때만 동작하고, 여유(<=10m)·규칙3
 *        (매칭만 안)·규칙5(오차 구제)로 "안" 판정된 tick 은 보정 없이 tick 시각을 써서 체류가 길게
 *        잡힌다. 보완: "안" 판정에 실제로 쓴 좌표(규칙3이면 매칭좌표)로 경계라인 교차를 찾아 보간하고,
 *        궤적에 경계라인 교차가 아예 없으면(전부 여유 안쪽) tick 시각을 그대로 쓴다.
 *   B10 [확정, 2026-10-04 사용자] 체류 시작·종료 — 출발이 구역 안이면 출발 tick 부터(현행), 도착이
 *        구역 안이면 도착 tick 까지(현행). TTL·비정상 종료는 TTL 만료 시각이 아니라 **구역 안에서
 *        마지막으로 관측된 tick** 까지로 하고 N/3(사유 61)로 표시한다(현행은 dtLastSeen — 구역 밖
 *        tick 까지 들어갈 수 있어 dtLastInZoneTime 으로 보완). TTL 만료 시각은 차량 행동이 아니라
 *        서버가 늦게 알아챈 시각이라(배치가 와야 검사됨, 서버 종료 플러시도 같은 경로) 쓰지 않는다.
 *
 *  C. 미정(?) 항목 권장안 — 2026-10-04 사용자 확정: 권장안대로 진행(C2 는 D3 조건 포함)
 *   C1 도달성 판정 기준점 = 구역 안 마지막 신뢰 매칭 tick 의 좌표·GPS 시각(경계 보간값 아님 — 실측이
 *        아니므로). 판정식은 새 임계를 만들지 않고 ShouldSkipImplausibleSpeed 와 같게:
 *        경로거리(FindLinkPathBounded) ÷ 경과시간 > 신고속도 × speed_factor + speed_margin 이면 도달 불가.
 *   C2 누락 링크 복구 상한: 지금은 MM_NODE_STEP_PARK_EXIT_MAX_HOPS=6 하드코딩. config 로 승격 —
 *        park_rcvhop=6(경로 홉), park_rcvtick=5(복구 대기 tick, MM_PENDING_MAX_HOLD_TICKS 와 같은 값).
 *        상한을 넘겨 복구 못 한 tick 은 주정차 체류에 넣는다(거리에는 넣지 않음).
 *   C3 [확정·변경, 2026-10-04 사용자] 구역 안에서 정차가 끼어도, 주정차 행이 생기더라도 **일반도로를
 *        정차 앞뒤로 나누지 않는다** — 한 run 으로 잇는다(체류시간만큼 평균속도가 낮아지는 것은 감수).
 *        정지 tick(속도<1 또는 bSameRawAndHeadingAsPrev) 거리는 누적하지 않는다(GPS 표류가 거리로
 *        둔갑하지 않게). TRIP_SEQ 는 기존 방식 그대로 START_GPS_SEQ 순 부여(ReleaseChargeQueue) —
 *        일반도로 행이 먼저 시작하므로 그 안에 겹친 주정차 행은 그 다음 순번이 된다.
 *        (종전 권장안 "주정차 행이 생기면 정차 앞뒤로 분할"은 채택하지 않음)
 *   C4 구역 안 tick 이 모두 미매칭이고 진입·진출은 구역 밖: ResolveSkipGapNodeStep 3단계를 그대로 쓴다
 *        (재매칭 Y/0 -> 경로 길이 Y/0 -> 직선거리 N/3). 체류시간은 규칙1로 주정차에 들어간다.
 *   C5 출발·도착이 모두 구역 안이고 모두 미매칭: 일반도로 행을 만들지 않는다(매칭 근거 없음 —
 *        DropNodeStepRowsWithoutMatch 와 같은 원칙). 주정차만 판정한다.
 *   C6 (B6 로 확정·이동)
 *
 *  D. 적용 방식·주변 영향 검토 (2026-10-04)
 *   D1 스위치: (2026-10-04 검증 단계에서 config park_drivemode 로 0/1 을 대조했고, 사용자 확정 후
 *        **설정을 제거하고 1 의 동작으로 고정**했다 — E10 참고.)
 *   D2 주변 실측(ruc DB): RL-Z00001(판교)은 일반도로 RL-Z00002 4.5m·구간단속 RL-Z00003 5.0m·
 *        면제 RL-Z00016 5.0m·RL-Z00015 9.2m 거리에 붙어 있다. 폴리곤 경계 관통 링크 RL-Z00001 6개·
 *        RL-Z00014 11개, 밖 0~10m 링크 15개·11개.
 *   D3 C2 는 그대로 쓰면 위험하다 — 옆 구간단속·일반도로를 달리는 tick 이 복구 실패로 주정차 체류에
 *        들어가면 "주행 중 주차" 모순이 생긴다. 조건: 주정차 세션이 이미 열림 + 원시가 폴리곤 안
 *        (여유 0) + 다른 과금유형 run 미진행일 때만 체류에 넣는다.
 *   D4 C1 은 기존 ShouldSkipImplausibleSpeed 와 같이 "직전 정상 + 간격 10초 이하"일 때만 판정한다.
 *        경로를 못 찾으면 직선거리로 대신하지 말고 판정을 보류한다(옆 도로 정상 진출을 SKIP 하지 않게).
 *   D5 검증 표본 부족: 폴리곤 50m 이내 실주행은 RL-Z00001 8트립뿐, RL-Z00014 는 0. 경계 모호 tick
 *        (원시 밖0~10m+매칭 안 78, 원시 안+매칭 밖 10). A/B 전체 재매칭 외에 시뮬레이터(sim_truth)로
 *        통과·정차후출차·옆 구간단속 주행·구역 안 종료·전부 미매칭 5시나리오를 만들어 검증할 것.
 *        (2026-10-04 사용자 확정 — 진행. B8 의 구간단속 전·후·전후 3경우를 시나리오에 추가)
 *   D6 시나리오 기준선(모드 0 = 현행, 2026-10-04) — web/tools/zone_check/analysis/park_scenario.py,
 *        트립 980001~980009. 목표 충족 4/9: S3 옆 구간단속(주정차 없음)·S4 구역 안 종료·S5 전부 미매칭
 *        (SKIP 구간 브릿지로 이미 연속 1행)·S9 출도착 안+미매칭(주정차만). 미충족 5/9 는 모두 같은 원인 —
 *        **매칭된 폴리곤 안 tick 의 일반도로 억제(A2)**: S1 통과 seq4~13, S2 정차 전후 주행분,
 *        S6 seq20~23, S7 seq4~13, S8 seq20~32 가 어느 일반도로 행에도 없다. 즉 같은 폴리곤 통과라도
 *        매칭에 실패하면(S5) 청구되고 성공하면(S1) 청구되지 않는 비일관이 현행에 있다.
 *        TTL 마감의 체류 종료는 현재 dtLastSeen(구역 밖 tick 포함)이다 — B10 참고.
 *
 *  E. 구현 상태 (2026-10-04 — 아래 "모드 0"은 종전 동작, "모드 1"은 현재 고정된 동작)
 *   E1 [구현] B2·B8·C3 — ProcessNodeStepCharge() 확정 이탈 분기: 보류 run + 경계~첫 안쪽 tick + 폴리곤
 *        안 이동분을 "마지막 안쪽 tick"까지 한 행으로 등록, 진출 쪽은 종전 경계 소급 로직 그대로(+ 마지막
 *        안쪽 tick~경계 거리). 정지 tick 거리 제외(bTouchStill). 병합 예외 ②에서 주정차 행 제외
 *        (MergeAdjacentNodeStepRows). B4(구역 안 종료)는 종전 트립종료·TTL 경로가 그대로 충족.
 *   E2 [구현] B9 — ProcessParkingCharge() 진입·진출 보간에 매칭좌표 쌍 재시도(dfLastInZoneMX 등).
 *   E3 [구현] B10 — AppendExpiredParkingCharge() 체류 종료 = 구역 안 마지막 tick.
 *   E4 [보류 — 시험 기준 판단, 2026-10-04] C1(진출 도달성 SKIP)·C2(park_rcvhop/park_rcvtick).
 *        시나리오 980013(S12: 출차 직후 1 tick 을 진행방향 직각 45m 로 튀김)에서 그 tick(seq130)은
 *        이미 CONTINUE 탐색이 참 링크 2040425301 에 오차 0.0m 로 매칭했다 — 도달성 SKIP 을 더해도
 *        바뀔 것이 없다. 남은 영향은 주정차 DIST_M(원시좌표 누적) 168 -> 211m 뿐이다. 복구 실패(C2)
 *        를 재현하는 사례도 실측·시나리오 모두 없어, 둘 다 실제 결함 사례가 나올 때 착수한다.
 *   E6 검증 2차(980010~980013): B9 — S10(경계 tick 원시만 밖) 체류 384s(모드0) -> 388s(모드1),
 *        같은 움직임의 S2 와 일치. B10 — S11(정차 후 2 tick 이탈 중 종료신호 없이 트립 전환) 체류
 *        391s(이탈 tick seq135 까지) -> 388s(구역 안 seq134 까지), N/3·62. 단, 이탈 디바운스 중
 *        비정상 종료라 폴리곤 안 주행분은 B4(구역 안 종료)와 같이 일반도로에 넣지 않는다.
 *   E5 검증(park_scenario.py, 980001~980009): 모드 0 은 구현 전 기준선과 9건 전 행 동일(회귀 0).
 *        모드 1 목표 충족 4/9 -> 9/9 기준(아래 단서):
 *          S1 통과 2행 425m -> 1행 692m / S2 정차후출차 일반 2행 526m -> 1행 773m(정차 표류 0, 주정차
 *          388s 와 겹침) / S6 폴리곤 분할 제거(1~19 + 19~27) / S7 1~14 363m(면제 15~16 과 겹침 없음) /
 *          S8 19~33 366m / S3·S4·S5·S9 불변.
 *        단서: S6 처럼 구간단속 앞뒤가 2행으로 나뉘는 것은 폴리곤과 무관한 기존 병합 규칙(병합 결과가
 *        구간단속 행을 감싸면 금지, 2026-09-16)이며 S3(폴리곤 없음)도 같다. B9·B10 은 이 시나리오로
 *        발동하지 않았다(원시좌표가 경계라인 안 / TTL 경로 없음) — 별도 검증 필요.
 *   E7 실주행 검증(2026-10-04, 폴리곤 50m 이내 8트립, 모드 0 -> 1): 일반도로 22행 8,024m -> 19행
 *        9,878m(+1,854m, +23.1%). 다른 유형(개방·폐쇄·구간·주정차·면제) 행·거리 불변, MATCH_STATUS 불변
 *        (MATCHED 732 / SKIP 219). 변화 트립 6개: 000370_20260819093236 476->740m,
 *        000376_20260819093337 1,170->1,526m, 000376_20260819140532 762->1,090m,
 *        000376_20260819141002 2,076->2,430m, 000376_20260821094609 478->630m,
 *        000376_20260821095239 1,146->1,546m(3행->2행).
 *   E9 [적용, 2026-10-04 사용자 확인] config.ini park_drivemode=1 — 시나리오 980013(정차 후 출차)이
 *        "일반도로 1~148(657m·439s·5km/h) + 주정차 4~133" 2행으로 등록되는 것이 맞다고 확인. 실주행
 *        21트립·시나리오 전체를 모드 1 로 재처리했다. verify_charge.py: S1·S2(trip_seq 결번·순서) 0건,
 *        K1·K2(주정차) 0건. Z1·Z3 ERROR 5건은 모두 시나리오 트립의 구간단속 게이트 공유 tick(모드 0 의
 *        S3 에서도 동일한 기존 패턴), P1 1건은 의도된 S9(전부 SKIP), P6 은 000093 합성 트립과 트립 첫 점
 *        오매칭(980003·980012 seq1 -> 2040423802) 계열로 이번 변경과 무관.
 *   E10 [고정, 2026-10-04 사용자 지시] 별도 환경설정이 필요 없으므로 config park_drivemode 와 그 읽기·
 *        전달 소스(ConfigDefaults/Config/AppMain/Server/RawLogWorker.h)를 제거하고 모드 1 동작만 남겼다.
 *        제거 후 재처리 결과가 모드 1 과 동일함을 확인(시나리오 13트립·실주행 21트립). 종전 동작으로
 *        되돌리려면 각 블록의 "되돌리는 법" 주석을 따르거나 git 이력을 쓴다.
 *   E11 [확정, 2026-10-04 사용자 지시] **TRIP_SEQ 는 SQL 로 처리하지 않고 엔진이 확정할 때 등록한다.**
 *        폴리곤 안 일반도로 행(E1)도 다른 모든 과금 행과 같은 경로다: pvtChargeInserts -> EnqueueChargeRows
 *        -> ReleaseChargeQueue 에서 번호(++nEmitSeq)를 붙여 INSERT, 이후 어떤 SQL 도 TRIP_SEQ 를 바꾸지
 *        않는다. 사후 재부여 [trip_seqoff]/[trip_seqfin]·UpdateTripSeqOrder()·config 키는 삭제했다.
 *        현행 번호 순서 = START_GPS_SEQ 순(워터마크 큐). 그래서 정차 후 출차(980002)는 일반도로 1~148 이
 *        1번, 그 안의 주정차 4~133 이 2번이며, 주정차 행은 일반도로 행이 확정될 때까지 INSERT 가 늦어진다.
 *        [2026-10-04 사용자 결정: **현행(START_GPS_SEQ 순 + 워터마크 큐) 유지**. 아래는 채택하지 않은 대안 기록]
 *        [대안 — 미채택] "확정 순 + 일반도로만 짧은 보류(최대 60초)" 로 바꾸면 주정차·
 *        구간단속 행은 확정 즉시 INSERT 된다. 이 방식을 채택하면 폴리곤 안 일반도로 행도 같은 규칙을
 *        따른다(일반도로이므로 짧은 보류 대상). 변경 지점: ReleaseChargeQueue() 정렬 기준(START -> 큐 적재
 *        순)과 CalcChargeWatermark()(유형별 준비 판정), FillUncoveredLinkRows() 흡수 대상(미방출 행 한정),
 *        verify_charge.py S2 규칙. 정본 문서 doc/과금_등록_로직_정본.md 6·10절.
 *   E21 [구현, 2026-10-04 사용자 확정(권장안)] park_ttl 강제 마감 종료 = 폴리곤 안 마지막 tick(RAW_VLD 무관). 시작 조건·Y/0 유지.
 *   E20 [구현, 2026-10-04 사용자 확정(권장안)] 주정차 진입·진출 경계 보간 실패 시 밖·안 두 tick 의 중간 시각(gps_dt) 사용.
 *   E19 [구현, 2026-10-04 사용자 확정(권장안)] 주정차 출발·도착 시각에 정확도 초과 tick 의 GPS 시각도 쓴다(위치 판정은
 *        그대로). 출발=트립 첫 GPS(앞머리 전부 정확도 초과 + 폴리곤 근접), 도착=종료 tick GPS(마지막 안쪽 이후 전부 정확도 초과).
 *   E18 [구현, 2026-10-04 사용자 확정 — 정리안 U1+U2] 주정차 폴리곤 안 SKIP 좌표(RAW_VLD=false·정확도 초과·매칭 실패)는
 *        주정차 판정에만 쓰고 일반도로·구간단속에는 쓰지 않는다 — 일반도로 run 을 SKIP 직전에서 끊고 SKIP 뒤부터 새로
 *        시작, SKIP 복구 행 미생성, 구간단속 원시좌표 출구 확인 생략, 0m·0s 일반도로 행 미등록.
 *   E17 [구현, 2026-10-04 사용자 확정(권장안)] 일반도로 구역 전환 병합 시 전환 구간(이전 run 마지막 tick~상대 run
 *        진입 tick)을 링크를 따라 더한다(로그 node step run join leg). 4트립 오차 합 76→32m.
 *   E16 [구현, 2026-10-04 사용자 확정] **폴리곤 안이라도 일반도로로 판정되면 항상 일반도로로 등록**(B2·B4 대체).
 *        MM_PARK_SUPPRESS_NODESTEP=false — 일반도로 폴리곤 억제 해제, 커버리지 복구의 폴리곤 안 링크 제외 해제,
 *        폴리곤 안 정지 tick(속도<1) 거리 누적 제외. 주정차 행은 그대로(B6 겹침 허용). 실측은 정본 7절.
 *   E15 [구현, 2026-10-04 사용자 확정] 게이트형(폐쇄식·구간단속) 거리 = 진입~진출 게이트 사이 구역 선 길이(CalcZoneSpanM),
 *        체류 = 진출 시각 − 진입 시각(게이트 통과 보간, 종전 유지), 평균속도 = 거리 ÷ 체류. 폴리곤 안 구간단속(B7)과
 *        그 미러에도 같은 값이 쓰인다. 일반도로 링크 형상 측정은 실주행 +0.00% 라 보류(정본 10절).
 *   E14 [구현, 2026-10-04 사용자 확정] **일반도로 출발 = 일반도로·구간단속 링크에 매칭된 첫 tick.** 트립 시작
 *        출발 후보 해소(ResolveStartAmbiguity)로 SKIP 강등된 tick 에서 시작한 run·보류 run·병합 이월·접촉 이월을
 *        첫 유효 tick 으로 다시 잡는다(ReanchorRunHead, 판정 구간 tick 기록 RecordRunHeadTrail). 유효 tick 이
 *        없으면 버린다. 링크 진입(다른 구역 진출) 출발은 종전대로 게이트·링크 시작부터, 다른 구역 진입으로
 *        끝나면 종전대로 경계까지. 실측 000376_20260819140532 4~53 1,090m/146s -> 6~53 975m/140s(사용자
 *        기준 재계산 978.7m/141s), 000370_20260819093236 25~82 277m/174s -> 28~82 147m/164s(재계산 136.1m +
 *        면제 경계까지 채움). 원인은 진 쪽 링크 보류 run 을 경계 교차 보정이 엉뚱한 교차점으로 끌고 가 그
 *        점~접촉 시작점 직선 87.9m 가 폴리곤 안 거리에 더해진 것.
 *        [확인된 사실 — 보류] 폴리곤 안 행(마지막 안쪽 tick 까지)과 진출 행(경계부터) 사이 구간은
 *        FillUncoveredLinkRows 가 메운다(980001 seq14 +8.3m). 접촉 이월에 링크 목록을 모으면 이 보완이 멈춰
 *        정답에서 멀어져(980001 677->669, 정답 679.3) 원복했다 — 그 대가로 verify Z6 에 폴리곤 안 링크가
 *        "미덮임"으로 남는다(거리 영향 없음).
 *   E13 [구현, 2026-10-04 사용자 확정] 구간단속 앞뒤 일반도로를 한 행으로 — 진입 앞 일반도로 + 미러(구간단속
 *        구간 + 진출 후). MergeAdjacentNodeStepRows() 감싸기 예외에 구간단속(3) 추가(2026-09-16 금지를 구간단속에
 *        한해 되돌림), CalcChargeWatermark() ⑤ 를 구간단속 통과·미러 보류 중에도 적용. B8 과 같은 "연속 주행 = 1행".
 *        실측 980015 2~6+6~25 -> 2~25(594m), 000376_20260819094414 1~42+43~57 -> 1~57(1,006m). 같은 날 REG_DT/UPD_DT
 *        를 행 생성 시각이 아니라 trip_seq 부여(ReleaseChargeQueue) 시각으로 찍도록 수정(보류가 길어져 드러남).
 *   E12 [구현, 2026-10-04 사용자 확정 — TTL 마감 권장안 (가)(나)(다) + 이어 매기기] B10 의 TTL 경로 정리.
 *        (가) TTL 마감은 [trip_end] 와 같은 UPDATE(trip_end_dt·upd_dt)만 — 확정 행 판정 불변, [trip_abend] 삭제.
 *        (나) 마감 종료시각 = 마지막 GPS 시각(dtLastGpsEventTime). 주정차 체류 종료(B10)와 같은 기준.
 *        (다) 워커 유휴 60초마다 idle() -> ExpireTtlSessions()(워커 자기 스레드).
 *        이어 매기기: 세션에 없는 trip_id 배치는 [trip_seqmax] 로 기존 MAX(TRIP_SEQ) 를 읽어 nEmitSeq 시드.
 *        충돌 감지: INSERT 적재 수 < 요청 수 -> [CHARGE_SEQ_CONFLICT] ERROR.
 *        검증 S15(980015): 일반 2~6·구간단속 6~19 Y/0 유지(종전 N/3·61), 폐쇄 1~1 사유 21 유지(종전 61),
 *        trip_end_dt 20261005010112(GPS, 종전 벽시계 20261004123120), 이어진 seq26~35 -> trip_seq 5 212m
 *        (종전 PK 충돌로 미적재). 실주행 21트립·S1~S13 무변화.
 *   E8 진출 쪽 누락 링크 복구(park exit boundary)는 경로에서 **처음** 폴리곤을 벗어나는 지점만 찾고 그
 *        뒤 링크는 다시 폴리곤에 들어가도 전부 더한다(재진입 미검사). 모드 1 에서는 폴리곤 안 주행도
 *        일반도로라 정책과 맞고, 모드 0 에서만 억제 원칙과 어긋난다. 주정차에는 게이트가 없어(폴리곤
 *        안 여러 도로) 출구를 미리 정할 수 없으므로, 진출은 계속 "경계 교차 + park_exitcnt 디바운스"로
 *        판정한다.
 *  구현 메모: ProcessNodeStepCharge() 의 접촉 소급 버퍼(stParkTouchCarry) "미확정" 분기는 2026-09-06
 *   억제 기준을 매칭좌표로 바꾼 뒤 도달하지 않는 코드가 됐는데, 그 동작("접촉 구간을 원래 run 에
 *   이어붙임")이 B2 의 통과 처리와 같다 — B2 구현 시 재사용 후보.
 * ════════════════════════════════════════════════════════════════════════════════════════════
*/

/**
 * @brief 주정차 판정 — 원시좌표 + (신뢰 시) 매칭좌표로 구역 진입·이탈·체류 판단 (2026-08-13 최정우 추가)
 * @remark [2026-10-04 정정] 종전 헤더는 "매칭 결과를 전혀 안 쓰고 맵매칭 전에 호출, 임계값 없음"이라고
 *   적혀 있었으나 현재와 다르다. 2026-08-22 부터 맵매칭 **이후**에 호출되어 규칙2~4가 매칭좌표를 쓰고,
 *   2026-08-24 부터 base_parking_fine 최소시간 미만 체류는 행을 만들지 않는다(BuildParkRow).
 *   판정 규칙 1~5는 본문 "판정 규칙" 주석, 일반도로·구간단속과의 관계는 바로 위 [정책 대조표] 참고.
 *   DRIVE_STATUS 는 진입 판정에 쓰지 않고, PARKED+정지일 때 거리 누적만 막는다(bParkedStill).
 *   구역 이탈은 park_exitcnt 회 연속 확인 후에만 확정(디바운스) — GPS 튐으로 인한 세션 오종료 방지.
*/
void CRawLogWorker::ProcessParkingCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		bool bTrustedTripEnd, bool bMatchTrusted, double dfMatchX, double dfMatchY)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경계 통과 시각 보간용 "직전 틱" 스냅샷 — 이번 틱으로 갱신하기 "전" 값을 먼저 떼어간다.
	//   early return 경로가 아래에 여럿 있어(bTripEnding 등) 함수 맨 앞에서 한 번만 갱신해
	//   호출마다 반드시 실행되게 한다 (사용자 지시, 2026-08-24 최정우 추가)
	const bool bPrevRawKnown = pstSession->bHasLastRawTick;
	const double dfPrevRawX = pstSession->dfLastRawTickX;
	const double dfPrevRawY = pstSession->dfLastRawTickY;
	const time_t dtPrevRaw = pstSession->dtLastRawTick;
	pstSession->dfLastRawTickX = stRawLogInfo.dfX;
	pstSession->dfLastRawTickY = stRawLogInfo.dfY;
	pstSession->dtLastRawTick = stRawLogInfo.dtGPS;
	pstSession->bHasLastRawTick = true;
	// 같은 스냅샷의 매칭좌표판 — 정책 대조표 B9 경계라인 보간용. 신뢰 매칭이 아닌
	//   tick 은 bHas=false 로 끊는다(보간 쌍은 바로 직전 tick 이어야 한다) (2026-10-04 최정우 추가)
	const bool bPrevMatchKnown = pstSession->bHasLastParkMatch;
	const double dfPrevMatchX = pstSession->dfLastParkMatchX;
	const double dfPrevMatchY = pstSession->dfLastParkMatchY;
	const time_t dtPrevMatch = pstSession->dtLastParkMatch;
	pstSession->bHasLastParkMatch = bMatchTrusted;
	pstSession->dfLastParkMatchX = bMatchTrusted ? dfMatchX : 0.0;
	pstSession->dfLastParkMatchY = bMatchTrusted ? dfMatchY : 0.0;
	pstSession->dtLastParkMatch = stRawLogInfo.dtGPS;

	// ── 좌표 정확도 상한 (park_accmax, 0=비활성) ─────────────────────────────────
	//   측위에 실패한 단말은 셀 기반 대체 위치로 수백 m 점프한 뒤 그 좌표에 얼어붙는다.
	//   실측(실주행 11트립, analysis/rawvld_realcheck.py — 앞뒤 신뢰점 사이를 도로 형상을
	//   따라 보간해 참위치를 추정하는 방식, leave-one-out 검증 링크일치 93.9%):
	//     accuracy_m 51~100  추정오차 중앙 34.7m — 폴리곤 판정 17건 전부 허위 진입(일치 0)
	//     accuracy_m 101~    추정오차 중앙 189m·최대 486m, 좌표 동결률 64.6%
	//                        — 허위진입 20건·누락 20건
	//     accuracy_m 16~50   추정오차 중앙 8.6m — 폴리곤 오판정 0건이라 살린다
	//   park_pad(확장 허용거리)로는 못 막는다. 이 값은 "얼마나 여유를 줄까"이지 "이 좌표를 믿을까"가
	//   아니라서, 좌표 자체가 200m 틀리면 버퍼를 좁혀도 엉뚱한 자리에서 판정한다.
	//
	//   맵매칭에는 영향이 없다 — 걸러지는 행(accuracy_m>50)은 전부 RAW_VLD=false 이고
	//   (경계가 15라 구조적으로 그렇다) 이미 ShouldSkipGpsInput() 에서 맵매칭을 SKIP 한다.
	//   반대로 매칭을 타는 RAW_VLD=true 행은 accuracy_m<=15 라 이 게이트에 걸릴 수 없다.
	//   "RAW_VLD=false 도 주정차 판정은 수행한다"(2026-08-22 사용자 확정)는 유지된다 —
	//   그 근거였던 "멈추면 GPS 가 나빠지는데 하필 그때 판정이 필요하다"는 16~50 구간에서
	//   여전히 유효하고, 못 쓸 좌표만 걷어낸다.
	//   ACCURACY_M 이 NULL(-1)이면 판단 근거가 없으므로 걸러내지 않는다.
	//
	//   적용 범위 — "세션 개시"에만 건다. 처음엔 함수 진입부에서 통째로 return 했는데
	//   실측에서 반례가 나왔다(000376_20260819094414): 정상 좌표로 폴리곤에 들어가 정차한
	//   진짜 도착 정차인데(seq 133 까지 정확도 12·폴리곤 내), 정차 후 정확도가 78~346 으로
	//   나빠졌고 트립 종료(TRIP_EVENT=END) 행도 정확도 321 이었다. 그 행에서 return 하는 바람에
	//   ①의 세션 마감이 실행되지 못해 199초짜리 진짜 체류가 통째로 사라졌다.
	//   그래서 아래처럼 나눈다.
	//     ① 진행 중 세션 — 갱신은 건너뛰되(위치·거리 오염·허위 이탈틱 방지) 마감은 그대로 한다
	//     ② 세션 개시   — 나쁜 좌표로는 새 세션을 열지 않는다 (허위 진입 차단)
	//   즉 나쁜 좌표를 "정보 없음"으로 다루는 것이지 "구역 밖"으로 다루는 게 아니다.
	//   (2026-08-23 최정우 추가)
	const bool bAccTrusted = (m_stConfig.nParkAccMax <= 0) || (stRawLogInfo.nAccuracyM < 0)
		|| (stRawLogInfo.nAccuracyM <= m_stConfig.nParkAccMax);
	// [2026-10-04 최정우 추가, 사용자 확정 — E19] 주정차 체류시간은 **모든 GPS 수신 시각(gps_dt)** 으로 계산한다.
	//   정확도 초과 tick 은 위치 판정에는 여전히 쓰지 않지만(오탐 방지), 출발·도착 시각에는 쓴다:
	//   · 출발 — run 이 트립의 첫 정확도 정상 tick 에서 시작했으면(그 앞이 전부 정확도 초과) 트립 첫 GPS 시각으로 당긴다
	//   · 도착 — 트립 종료 때 마지막 안쪽 tick 이후가 전부 정확도 초과였으면 종료 tick 의 GPS 시각까지 늘린다
	//   실측 000376_20260819140532: 폴리곤 안 출발인데 seq1~3(정확도 53~180m)이 빠져 주정차가 seq7 부터 시작.
	// 되돌리는 법: 아래 bTripHead 사용부(진입)와 bExtendEnd(트립 종료) 두 곳을 지운다.
	if (pstSession->dwParkTripFirstSeq == 0)
	{
		pstSession->dwParkTripFirstSeq = stRawLogInfo.dwSeqNo;
		pstSession->dtParkTripFirstTime = stRawLogInfo.dtGPS;
	}
	const bool bAccOkSeenBefore = pstSession->bParkAccOkSeen;
	// 트립 앞머리의 정확도 초과 tick 은 위치를 믿을 수 없으므로, 정확도 반경 안에 주정차 폴리곤이 없는(=분명히 밖)
	//   tick 이 하나라도 있으면 출발 시각을 당기지 않는다(밖에서 출발해 들어온 경우를 과다 계상하지 않도록)
	if (!bAccOkSeenBefore && !bAccTrusted && pstSession->bParkHeadNear)
	{
		vector<PZONE_INFO> vtHeadNear;
		const double dfHeadRadiusM = (stRawLogInfo.nAccuracyM > 0) ? static_cast<double>(stRawLogInfo.nAccuracyM) : 0.0;
		m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(stRawLogInfo.dfX, stRawLogInfo.dfY, dfHeadRadiusM, &vtHeadNear);
		if (vtHeadNear.empty())
			pstSession->bParkHeadNear = false;
	}
	const uint32 dwLastAccOkSeqBefore = pstSession->dwParkLastAccOkSeq;
	if (bAccTrusted)
	{
		pstSession->bParkAccOkSeen = true;
		pstSession->dwParkLastAccOkSeq = stRawLogInfo.dwSeqNo;
	}

	// 구역판정 바깥쪽 확장 허용거리 — ACCURACY_M 적응형(포인트마다 오차만큼), 상한은 park_pad 로 캡(이상치 방지)
	double dfPadM = static_cast<double>(m_stConfig.nParkPad);
	if ((stRawLogInfo.nAccuracyM >= 0) && (static_cast<double>(stRawLogInfo.nAccuracyM) < dfPadM))
		dfPadM = static_cast<double>(stRawLogInfo.nAccuracyM);

	// ── 판정 규칙 (2026-08-22 재작성 → 2026-08-23 복수 구역 지원 → 2026-08-25 규칙4 정지 예외) ──
	//   규칙1  원시 좌표가 폴리곤 내 + 맵매칭 실패                    → 주정차
	//   규칙2  원시 좌표가 폴리곤 내 + 매칭 성공 + 매칭 좌표도 같은 폴리곤 내 → 주정차
	//   규칙3  원시 좌표는 폴리곤 밖인데 매칭 좌표가 폴리곤 내         → 주정차 (2026-09-07 신설)
	//   규칙4  원시 좌표는 폴리곤 내인데 매칭 좌표는 그 폴리곤 밖     → 통과 중이므로 제외
	//   규칙5  매칭 실패 + 원시 좌표가 폴리곤 경계에서 ACCURACY_M 이내(세션 개시·유지 공통)
	//                                                                 → 이탈로 치지 않음 (2026-09-07 신설)
	//   폴리곤이 겹쳐 설정될 수 있어(시간대별 규제가 다른 구역 등) 포함하는 구역을 전부 다룬다.
	//
	//   규칙4는 "매칭 좌표가 raw 좌표보다 신뢰할 만하다"는 전제인데, 이 전제는 차량이 실제로
	//   움직여야(방위각·궤적으로 후보 링크를 구분) 성립한다. 정지 상태(SPEED_KMH<1)에서는 그
	//   구분 신호 자체가 없어 근처 아무 링크에나 확신 있게(bReverseSkip/bClampLowConf/
	//   bAmbiguousReverse 어디에도 안 걸리는 채로) 잘못 스냅될 수 있다 — 실측
	//   000370_20260819093236 seq29~71(2분 9초, DRIVE_STATUS=PARKED·SPEED_KMH=0 내내, raw
	//   좌표는 RL-Z00001 폴리곤 안 그대로)에서 매칭이 인접 링크에 확신 있게 스냅되는 바람에
	//   규칙4가 전체 구간을 "통과 중"으로 오판 → PARKING 레코드가 한 건도 안 남음(진입
	//   후보(park_entrycnt)조차 못 쌓임) 확인. 정지 중엔 규칙4를 적용하지 않고 규칙1/2(raw
	//   기준)로만 판정한다 (2026-08-25 최정우 추가).
	const bool bLikelyStationary = (stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed < 1.0f);

	vector<PZONE_INFO> vtZones;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(
		stRawLogInfo.dfX, stRawLogInfo.dfY, dfPadM, &vtZones);

	// 규칙3 — 원시 좌표가 폴리곤 밖이라도 **실제로 달린 도로(매칭 좌표)가 폴리곤 안이면 주정차**다
	//   (2026-09-07 최정우 추가, 사용자 지시). 종전에는 판정의 출발점이 언제나
	//   GetParkingZonesContaining(원시좌표) 하나뿐이어서, 원시가 폴리곤을 살짝 벗어난 tick 은
	//   매칭이 폴리곤 안이어도 vtZones 가 비어 무조건 "밖"이었다. 같은 규칙이 NODE_STEP 억제
	//   쪽(bMatchInParkingZoneNow, 2026-09-06)에는 이미 들어가 있어, 그 tick 들이 일반도로에서도
	//   빠지고 주정차에도 안 들어가는 사각지대가 생겼다 — 실측 000376_20260821094609
	//   seq14·17·18·24(원시 밖·매칭 RL-Z00001 안). seq14~18 이 원시 기준으로 5회 연속 "밖"이라
	//   park_exitcnt(3) 디바운스를 넘겨 주정차 세션이 둘로 쪼개졌다(dwell 18초 + 37초).
	//   정지 중(bLikelyStationary)에는 적용하지 않는다 — 규칙4 예외와 같은 근거로, 멈춰 있으면
	//   매칭이 근처 아무 링크에나 스냅될 수 있어 매칭 좌표를 믿을 수 없다.
	// 되돌리는 법: 아래 규칙3 블록을 지우면 종전 판정으로 복귀
	if (bMatchTrusted && !bLikelyStationary)
	{
		vector<PZONE_INFO> vtMatchOnly;
		m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(dfMatchX, dfMatchY, 0.0, &vtMatchOnly);
		for (size_t m = 0; m < vtMatchOnly.size(); ++m)
		{
			bool bAlready = false;
			for (size_t e = 0; e < vtZones.size(); ++e)
			{
				if (strcmp(vtZones[e]->szRoadID, vtMatchOnly[m]->szRoadID) == 0)
				{ bAlready = true; break; }
			}
			if (!bAlready)
				vtZones.push_back(vtMatchOnly[m]);
		}
	}

	if (bMatchTrusted && !bLikelyStationary && !vtZones.empty())
	{
		vector<PZONE_INFO> vtMatch;
		m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(dfMatchX, dfMatchY, 0.0, &vtMatch);
		vector<PZONE_INFO> vtKeep;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			for (size_t m = 0; m < vtMatch.size(); ++m)
			{
				if (strcmp(vtZones[e]->szRoadID, vtMatch[m]->szRoadID) == 0)
				{ vtKeep.push_back(vtZones[e]); break; }
			}
		}
		vtZones.swap(vtKeep);							// 규칙4 — 매칭 좌표가 그 구역 밖이면 제외
	}

	// 규칙5 — "원시 폴리곤 밖 + 맵매칭 실패" tick 구제 (2026-09-07 최정우 추가, 사용자 지시)
	//   매칭이 실패하면 규칙3으로 구제할 근거가 없고 원시 좌표만 남는데, 그 좌표가 폴리곤
	//   경계에서 ACCURACY_M(평면 오차) 이내라면 "실제로 나갔다"가 아니라 "오차로 밖에 찍혔다"로
	//   보는 것이 맞다. 평상시 판정 여유(dfPadM)는 park_pad(10m)로 캡이 걸려 있는데, 이 구제에
	//   한해 그 캡을 풀고 ACCURACY_M 까지 허용한다.
	//
	//   적용 범위 — **진행 중 세션 유지와 세션 개시 양쪽 모두**다(2026-09-07 사용자 확인).
	//   처음엔 진행 중 세션에만 걸었는데(허위 진입 우려), 그러면 실측 000376_20260821094609 의
	//   seq16·17 이 아직 세션 개시 전이라 구제되지 않아 세션이 seq18 부터 열렸다. 이 구간은
	//   폴리곤 경계 14.4m·10.2m 지점에 ACCURACY_M 18·24 로 찍힌 tick 이고 앞뒤가 모두 주정차라,
	//   체류 계산에서 빠질 이유가 없다 — 정답은 16~24 다.
	//   허위 진입은 park_entrycnt(3회 연속)·park_accmax(50m 초과 좌표 배제)가 이미 막는다.
	//   맵매칭에 성공한 tick 은 대상이 아니다 — 그쪽은 규칙2~4가 매칭 좌표로 정확히 판정한다.
	//   ACCURACY_M 이 평상시 여유(dfPadM)보다 클 때만 의미가 있으므로 그 조건을 함께 건다.
	// 되돌리는 법: 아래 규칙5 블록을 지우면 종전 판정으로 복귀
	if (!bMatchTrusted && bAccTrusted && (stRawLogInfo.nAccuracyM > 0)
		&& (static_cast<double>(stRawLogInfo.nAccuracyM) > dfPadM))
	{
		vector<PZONE_INFO> vtNear;
		m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(
			stRawLogInfo.dfX, stRawLogInfo.dfY,
			static_cast<double>(stRawLogInfo.nAccuracyM), &vtNear);
		for (size_t e = 0; e < vtNear.size(); ++e)
		{
			bool bAlready = false;
			for (size_t z = 0; z < vtZones.size(); ++z)
			{
				if (strcmp(vtZones[z]->szRoadID, vtNear[e]->szRoadID) == 0)
				{ bAlready = true; break; }
			}
			if (!bAlready)
				vtZones.push_back(vtNear[e]);
		}
	}

	// 속도 상한 — park_speedmax=0 이면 비활성(기본). 위반 판정 정책은 과금서버 몫이라 엔진은
	//   구역 진입 사실만 등록한다 (2026-08-22 사용자 확정)
	const bool bSpeedGate = (m_stConfig.nParkSpeedMax > 0);
	const bool bSlowEnough = (!bSpeedGate) || (stRawLogInfo.fSpeed < 0.0f)
		|| (stRawLogInfo.fSpeed <= static_cast<float>(m_stConfig.nParkSpeedMax));
	if (!bSlowEnough)
		vtZones.clear();

	// ProcessRawLog() 가 스퓨리어스(순서역전) END 검사까지 마친 bTrustedTripEnd 를 그대로 씀
	//   (2026-08-25 최정우 수정)
	const bool bTripEnding = bTrustedTripEnd;
	const bool bThisRowTrusted = stRawLogInfo.bRawVldKnown && stRawLogInfo.bRawVld;
	// DRIVE_STATUS=PARKED + 속도 0 이면 좌표 변화는 GPS 튐이다 — 거리로 세지 않고 체류만 연장
	const bool bParkedStill = (stRawLogInfo.nDriveStatus == DRIVE_STATUS_PARKED)
		&& (stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed < 1.0f);

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtParkRuns.size(); )
	{
		PARK_RUN_SESSION& stRun = pstSession->vtParkRuns[si];

		// 정확도 미달 좌표는 "정보 없음" — 위치·누적거리를 오염시키지 않고 이탈 디바운스도
		//   올리지 않는다. 다만 트립 종료 행이면 마감은 해야 하므로 통과시킨다 (2026-08-23 최정우 추가)
		if (!bAccTrusted && !bTripEnding) { ++si; continue; }

		bool bSameZone = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0) { bSameZone = true; break; }
		}

		if (bSameZone)
		{
			POINT stPrev, stCur;
			stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
			stCur.dfX = stRawLogInfo.dfX;  stCur.dfY = stRawLogInfo.dfY;
			if (!bParkedStill)
				stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
			stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
			stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
			stRun.dfLastInZoneX = stRawLogInfo.dfX;
			stRun.dfLastInZoneY = stRawLogInfo.dfY;
			stRun.dfLastInZoneMX = bMatchTrusted ? dfMatchX : 0.0;		// B9 (2026-10-04 최정우 추가)
			stRun.dfLastInZoneMY = bMatchTrusted ? dfMatchY : 0.0;
		}
		stRun.dfLastX = stRawLogInfo.dfX;				// 하버사인 기준점은 항상 최신 좌표
		stRun.dfLastY = stRawLogInfo.dfY;
		if (bThisRowTrusted)
		{
			stRun.dtLastConfirmedTime = stRawLogInfo.dtGPS;
			stRun.dwLastConfirmedGpsSeq = stRawLogInfo.dwSeqNo;
			stRun.dfLastConfirmedX = stRawLogInfo.dfX;
			stRun.dfLastConfirmedY = stRawLogInfo.dfY;
		}

		if (bSameZone && !bTripEnding)
		{
			stRun.nExitTicks = 0;						// 정상 유지 — 디바운스 해제
			++si; continue;
		}

		if (!bTripEnding)
		{
			// 디바운스·유예 시작 "전", 구역 밖으로 처음 찍힌 원시좌표 — 경계 통과 시각 보간의
			//   Out 쪽 앵커. 이후 재확인 없이 그대로 유지(디바운스 도중 다시 잠깐 원존이어도
			//   nExitTicks 는 0으로 안 돌아가는 한 갱신 안 함) (2026-08-24 최정우 추가)
			if (stRun.nExitTicks == 0)
			{
				stRun.dfFirstOutX = stRawLogInfo.dfX;
				stRun.dfFirstOutY = stRawLogInfo.dfY;
				stRun.dtFirstOut = stRawLogInfo.dtGPS;
				stRun.dfFirstOutMX = bMatchTrusted ? dfMatchX : 0.0;		// B9 (2026-10-04 최정우 추가)
				stRun.dfFirstOutMY = bMatchTrusted ? dfMatchY : 0.0;
			}
			stRun.nExitTicks += 1;						// park_exitcnt 회 연속 확인 후에만 이탈 확정
			if (stRun.nExitTicks < m_stConfig.nParkExitCnt) { ++si; continue; }

			// 이탈이 park_exitcnt 회 연속으로 확정되면 재진입 유예(park_regrace) 없이 즉시 마감한다.
			//   재진입 시 같은 세션으로 이어붙이지 않고 별도 레코드로 새로 연다(사용자 지시,
			//   2026-09-02 최정우 추가 — 실측 000376_20260826150010 seq121~404, 재진입 유예로
			//   병합된 주정차 세션 안에 실제 주행 구간(일반도로 seq126~133)이 끼어들어 GPS_SEQ
			//   범위가 실제 "구역 내 체류"보다 넓게 표시되던 문제. park_regrace 설정값은 더 이상
			//   쓰이지 않는다 — 원복 시 이 블록만 되돌리면 된다)
			//   [2026-10-04 최정우] park_regrace 설정·전달 경로·PARK_RUN_SESSION.dtExitCandidateTime 삭제(사용자 확정).
			//   원복하려면 설정 키와 그 필드도 함께 되살려야 한다(git 이력 참고).
		}

		// 체류 종료는 "마지막으로 조건을 만족한 시각" — 디바운스·유예에 쓴 시간을 위반에 넣지 않는다.
		//   트립종료(bTripEnding)로 강제마감된 경우는 그 뒤에 관측된 "밖" 좌표 자체가 없어 보간
		//   근거가 없으므로 무보정(dtLastInZoneTime 그대로) (2026-08-24 최정우 추가)
		time_t dtParkEnd = stRun.dtLastInZoneTime;
		uint32 dwParkEndSeq = stRun.dwLastInZoneGpsSeq;
		// E19 도착 — 트립 종료 tick 자체가 정확도 초과이고, 마지막 안쪽 tick 이후 정확도 정상 tick 이 없었으면
		//   (= 밖이라는 믿을 만한 근거가 없으면) 도착 시각을 종료 tick 의 GPS 시각으로 늘린다
		const bool bExtendEnd = bTripEnding && !bAccTrusted && !bSameZone
			&& (dwLastAccOkSeqBefore <= stRun.dwLastInZoneGpsSeq)
			&& (stRawLogInfo.dwSeqNo > stRun.dwLastInZoneGpsSeq);
		if (bExtendEnd)
		{
			LOGFMTI("[#%02d] parking end extended to trip end gps time!device=[%s] trip_id=[%s] road=[%s] "
				"last_in_seq=[%u] end_seq=[%u]", nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRun.szRoadID, stRun.dwLastInZoneGpsSeq, stRawLogInfo.dwSeqNo);
			dtParkEnd = stRawLogInfo.dtGPS;
			dwParkEndSeq = stRawLogInfo.dwSeqNo;
		}
		if (!bTripEnding)
		{
			PZONE_INFO pstExitZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);
			bool bExitInterp = false;
			dtParkEnd = InterpolateZoneCrossingTime(pstExitZone,
				stRun.dfLastInZoneX, stRun.dfLastInZoneY, stRun.dtLastInZoneTime,
				stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut, &bExitInterp);
			// [2026-10-04 최정우 추가 — 정책 대조표 B9, 사용자 확정] 체류 종료는
			//   **경계라인** 통과 시각. 원시 쌍으로 보간이 안 됐으면(마지막 안쪽 tick 의 원시좌표가
			//   경계라인 밖 — 규칙3·여유로 "안" 판정된 경우) "안" 판정에 실제로 쓴 매칭좌표 쌍으로
			//   다시 보간한다. 매칭 쌍도 경계라인을 넘지 않으면 종전대로 tick 시각.
			// 되돌리는 법: 이 if 블록을 지우면 종전(원시 쌍만) 동작이다.
			if ((dtParkEnd == stRun.dtLastInZoneTime)
				&& (stRun.dfLastInZoneMX != 0.0) && (stRun.dfFirstOutMX != 0.0))
			{
				dtParkEnd = InterpolateZoneCrossingTime(pstExitZone,
					stRun.dfLastInZoneMX, stRun.dfLastInZoneMY, stRun.dtLastInZoneTime,
					stRun.dfFirstOutMX, stRun.dfFirstOutMY, stRun.dtFirstOut, &bExitInterp);
			}
			// E20 — 진출 경계 보간이 원시·매칭 쌍 모두 실패하면 마지막 안쪽 tick 과 첫 바깥 tick 의 중간 시각(초 단위 내림)
			if (!bExitInterp && (stRun.dtFirstOut > stRun.dtLastInZoneTime) && (stRun.dtLastInZoneTime > 0))
			{
				dtParkEnd = stRun.dtLastInZoneTime + (stRun.dtFirstOut - stRun.dtLastInZoneTime) / 2;
				LOGFMTI("[#%02d] parking exit boundary interpolation failed -> midpoint!device=[%s] trip_id=[%s] road=[%s] "
					"last_in=[%s] first_out=[%s] used=[%s]", nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					stRun.szRoadID, FormatDateTime14(stRun.dtLastInZoneTime).c_str(),
					FormatDateTime14(stRun.dtFirstOut).c_str(), FormatDateTime14(dtParkEnd).c_str());
			}
		}
		CHARGE_INSERT_ROW stRow;
		bool bMeetsFineMin = BuildParkRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, dtParkEnd,
			stRun.dfLastInZoneX, stRun.dfLastInZoneY, dwParkEndSeq, "Y", "0", &stRow);
		if (bMeetsFineMin) pvtChargeInserts->push_back(stRow);

		// gps_seq 범위를 함께 남긴다 — 미적재(registered=0)면 DB 에 흔적이 없어 어느 구간이
		//   한 세션이었는지 로그만으로 추적할 수 있어야 한다 (2026-09-07 최정우 추가, 사용자 지시)
		LOGFMTI("[#%02d] parking dwell recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"gps_seq=[%s~%s] dwell=[%s]s dist_m=[%s] avg_speed=[%s] trip_ending=[%d] registered=[%d] "
			"non_charge_reason=[%d:%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, stRow.strStartGpsSeq.c_str(), stRow.strEndGpsSeq.c_str(),
			stRow.strStaySeconds.c_str(), stRow.strDistM.c_str(),
			stRow.strSpeedKmh.c_str(), static_cast<int>(bTripEnding), static_cast<int>(bMeetsFineMin),
			NCR_NORMAL, m_cCodeMap.GetValue(NonChargeReasonTable, NOE(NonChargeReasonTable), NCR_NORMAL));

		pstSession->nChargeSeq += 1;
		pstSession->vtParkRuns.erase(pstSession->vtParkRuns.begin() + si);
	}

	if (bTripEnding)
	{
		pstSession->vtParkCands.clear();
		return;
	}

	// 정확도 미달 좌표로는 새 세션을 열지 않는다. 후보 카운터도 건드리지 않는다 —
	//   "정보 없음"이므로 좋은 좌표의 연속을 끊지도, 늘리지도 않는다 (2026-08-23 최정우 추가)
	if (!bAccTrusted)
		return;

	// ── ② 세션 개시 — 구역별로 park_entrycnt 회 연속 충족해야 연다 ────────────
	//   1~2점(0~3초)짜리는 체류시간 산출이 불가능하고 GPS 튐과 구분되지 않는다
	for (size_t ci = 0; ci < pstSession->vtParkCands.size(); )
	{
		bool bStill = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(pstSession->vtParkCands[ci].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bStill = true; break; }
		}
		if (bStill) ++ci;
		else pstSession->vtParkCands.erase(pstSession->vtParkCands.begin() + ci);   // 연속 끊김
	}

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtParkRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtParkRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;							// 이미 진행 중

		PARK_CANDIDATE *pstCand = nullptr;
		for (size_t ci = 0; ci < pstSession->vtParkCands.size(); ++ci)
		{
			if (strcmp(pstSession->vtParkCands[ci].szRoadID, vtZones[e]->szRoadID) == 0)
			{ pstCand = &pstSession->vtParkCands[ci]; break; }
		}
		if (pstCand == nullptr)
		{
			PARK_CANDIDATE stNew;
			strncpy(stNew.szRoadID, vtZones[e]->szRoadID, sizeof(stNew.szRoadID) - 1);
			stNew.szRoadID[sizeof(stNew.szRoadID) - 1] = '\0';
			// 연속의 첫 좌표 — 세션 진입 시각이 된다. 직전 틱(밖)이 있으면 그 사이에서 실제 경계
			//   통과 시각을 보간 — 버퍼만으로 판정됐거나(원시좌표는 폴리곤 밖) 직전 틱이 없으면
			//   (트립 첫 틱 등) 보정 근거가 없어 원시 GPS 시각 그대로 (2026-08-24 최정우 추가)
			bool bEntryInterp = false;
			stNew.dtTime = bPrevRawKnown
				? InterpolateZoneCrossingTime(vtZones[e], stRawLogInfo.dfX, stRawLogInfo.dfY, stRawLogInfo.dtGPS,
					dfPrevRawX, dfPrevRawY, dtPrevRaw, &bEntryInterp)
				: stRawLogInfo.dtGPS;
			// 정책 대조표 B9 — 진입도 경계라인 기준. 원시 쌍으로 보간이 안 됐으면
			//   매칭 쌍(이번·직전 tick 모두 신뢰 매칭일 때)으로 다시 보간 (2026-10-04 최정우 추가)
			if ((stNew.dtTime == stRawLogInfo.dtGPS) && bMatchTrusted && bPrevMatchKnown
				&& (dtPrevMatch == dtPrevRaw))
			{
				stNew.dtTime = InterpolateZoneCrossingTime(vtZones[e], dfMatchX, dfMatchY, stRawLogInfo.dtGPS,
					dfPrevMatchX, dfPrevMatchY, dtPrevMatch, &bEntryInterp);
			}
			// [2026-10-04 최정우 추가, 사용자 확정(권장안) — E20] 경계 보간이 원시·매칭 쌍 모두 실패하면 안쪽 첫 tick 시각
			//   대신 **직전(밖) tick 과 이 tick 의 중간 시각**을 쓴다. 두 tick 의 gps_dt 만으로 계산하며(간격 가정 없음),
			//   최대 오차가 tick 간격의 절반으로 준다. 초 단위 내림. 직전 tick 이 없으면(트립 첫 tick) 종전대로 이 tick 시각.
			//   (정정: 000376_20260821095603 seq19→20 은 보간 실패가 아니라 성공 — 경계 통과 09:56:59.55 를 반올림해 09:57:00.
			//    현 데이터에 보간 실패 사례는 없다. 발생 시 로그 "parking entry/exit boundary interpolation failed")
			// 되돌리는 법: 이 if 블록을 지운다.
			if (bPrevRawKnown && !bEntryInterp && (dtPrevRaw > 0) && (dtPrevRaw < stRawLogInfo.dtGPS))
			{
				stNew.dtTime = dtPrevRaw + (stRawLogInfo.dtGPS - dtPrevRaw) / 2;
				LOGFMTI("[#%02d] parking entry boundary interpolation failed -> midpoint!device=[%s] trip_id=[%s] road=[%s] "
					"seq=[%u] out=[%s] in=[%s] used=[%s]", nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					vtZones[e]->szRoadID, stRawLogInfo.dwSeqNo, FormatDateTime14(dtPrevRaw).c_str(),
					FormatDateTime14(stRawLogInfo.dtGPS).c_str(), FormatDateTime14(stNew.dtTime).c_str());
			}
			stNew.dwGpsSeq = stRawLogInfo.dwSeqNo;
			stNew.dfX = stRawLogInfo.dfX;
			stNew.dfY = stRawLogInfo.dfY;
			// E19 출발 — 이 tick 앞의 같은 트립 tick 이 모두 정확도 초과였는지
			stNew.bTripHead = !bAccOkSeenBefore && pstSession->bParkHeadNear && (pstSession->dwParkTripFirstSeq != 0)
				&& (pstSession->dwParkTripFirstSeq < stRawLogInfo.dwSeqNo);
			pstSession->vtParkCands.push_back(stNew);
			pstCand = &pstSession->vtParkCands.back();
		}
		pstCand->nTicks += 1;
		if (pstCand->nTicks < m_stConfig.nParkEntryCnt) continue;

		PARK_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';
		stRun.dtEntryTime = pstCand->dtTime;			// 연속의 첫 좌표부터 체류 시작
		stRun.dwEntryGpsSeq = pstCand->dwGpsSeq;
		if (pstCand->bTripHead)							// E19 출발 — 트립 첫 GPS 시각으로(위치는 첫 정상 tick 그대로)
		{
			LOGFMTI("[#%02d] parking start pulled to trip first gps time!device=[%s] trip_id=[%s] road=[%s] "
				"cand_seq=[%u] trip_first_seq=[%u]", nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				vtZones[e]->szRoadID, pstCand->dwGpsSeq, pstSession->dwParkTripFirstSeq);
			stRun.dtEntryTime = pstSession->dtParkTripFirstTime;
			stRun.dwEntryGpsSeq = pstSession->dwParkTripFirstSeq;
		}
		stRun.dfEntryX = pstCand->dfX;
		stRun.dfEntryY = pstCand->dfY;
		stRun.dfLastX = stRawLogInfo.dfX;
		stRun.dfLastY = stRawLogInfo.dfY;
		stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
		stRun.dwLastInZoneGpsSeq = stRawLogInfo.dwSeqNo;
		stRun.dfLastInZoneX = stRawLogInfo.dfX;
		stRun.dfLastInZoneY = stRawLogInfo.dfY;
		stRun.dtLastConfirmedTime = stRawLogInfo.dtGPS;
		stRun.dwLastConfirmedGpsSeq = stRawLogInfo.dwSeqNo;
		stRun.dfLastConfirmedX = stRawLogInfo.dfX;
		stRun.dfLastConfirmedY = stRawLogInfo.dfY;
		pstSession->vtParkRuns.push_back(stRun);

		LOGFMTI("[#%02d] parking zone entry!device=[%s] trip_id=[%s] road=[%s] cnt=[%d] speed=[%d] open=[%zu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
			pstCand->nTicks, static_cast<int>(stRawLogInfo.fSpeed), pstSession->vtParkRuns.size());

		for (size_t ci = 0; ci < pstSession->vtParkCands.size(); ++ci)
		{
			if (strcmp(pstSession->vtParkCands[ci].szRoadID, stRun.szRoadID) == 0)
			{ pstSession->vtParkCands.erase(pstSession->vtParkCands.begin() + ci); break; }
		}
	}
}

/**
 * @brief 하버사인 거리 계산 (WGS84 경위도 → m) (2026-07-08 최정우 추가)
 * @param[in] stA 좌표 A (dfX=경도, dfY=위도, 단위 도)
 * @param[in] stB 좌표 B
 * @return 두 점 사이 지표 거리(m)
 * @remark a = sin²(Δlat/2) + cos(lat1)·cos(lat2)·sin²(Δlon/2), d = 2R·asin(√a)
*/
double CRawLogWorker::HaversineMeters(const POINT& stA, const POINT& stB)
{
	const double dfR = 6378137.0;								// WGS84 장반경(m)
	double dfLat1 = RAD(stA.dfY);
	double dfLat2 = RAD(stB.dfY);
	double dfDLat = RAD(stB.dfY - stA.dfY);
	double dfDLon = RAD(stB.dfX - stA.dfX);

	double dfA = sin(dfDLat / 2.0) * sin(dfDLat / 2.0)
		+ cos(dfLat1) * cos(dfLat2) * sin(dfDLon / 2.0) * sin(dfDLon / 2.0);
	if (dfA > 1.0) dfA = 1.0;									// 부동소수 오차 클램프
	return 2.0 * dfR * asin(sqrt(dfA));
}

/**
 * @brief INTERSECT_LEN 산출 — GPS 좌표와 세그먼트 교차점(MATCH) 사이 거리(m)
 * @param[in] stRawLogInfo 원시 GPS (dfX=경도, dfY=위도, 도)
 * @param[in] dfMatchLon 세그먼트 교차점 경도 (MATCH_LON)
 * @param[in] dfMatchLat 세그먼트 교차점 위도 (MATCH_LAT)
 * @return 반올림 정수 거리(m), GPS 무효 시 -1
*/
int CRawLogWorker::CalcIntersectLen(const sRawLogInfo& stRawLogInfo,
		double dfMatchLon, double dfMatchLat)
{
	if (stRawLogInfo.bGpsLatNull || stRawLogInfo.bGpsLonNull)
		return -1;

	POINT stGps;
	POINT stMatch;
	stGps.dfX = stRawLogInfo.dfX;
	stGps.dfY = stRawLogInfo.dfY;
	stMatch.dfX = dfMatchLon;
	stMatch.dfY = dfMatchLat;
	return static_cast<int>(HaversineMeters(stGps, stMatch) + 0.5);
}

/**
 * @brief PostgreSQL text[] 리터럴용 문자열 이스케이프
 * @param[in] strValue 원본 문자열
 * @return 이스케이프된 문자열
*/
string CRawLogWorker::EscapePgArrayText(const string& strValue)
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
string CRawLogWorker::BuildPgTextArray(const vector<string>& vtValues)
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
 * @brief bulk UPDATE 1행 적재
 * @param[out] pvtUpdates bulk UPDATE 대상 목록
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] nStatus MATCH_STATUS (1/3/4/0)
 * @param[in] nIntersectLen GPS↔세그먼트 교차점 거리(m, INTERSECT_LEN), -1 이면 미갱신
 * @param[in] pdfMatchLat 매칭 위도 (MATCHED 시), nullptr 이면 미갱신
 * @param[in] pdfMatchLon 매칭 경도 (MATCHED 시), nullptr 이면 미갱신
 * @return true(적재 성공), false(pvtUpdates null·trip_id 무효)
 * @remark invalid trip_id 시 false — run() orphan release 가 PK 없으면 복구 대기
*/
bool CRawLogWorker::AppendUpdateRow(vector<RAW_LOG_UPDATE_ROW> *pvtUpdates,
		const sRawLogInfo& stRawLogInfo, sint16 nStatus, int nIntersectLen,
		const double *pdfMatchLat, const double *pdfMatchLon, uint64 qwMatchLinkId)
{
	if (pvtUpdates == nullptr)
		return false;

	if (stRawLogInfo.szTripID[0] == '\0')
	{
		LOGFMTE("worker update error! invalid trip_id! seq=[%u] device=[%s]",
			stRawLogInfo.dwSeqNo, stRawLogInfo.szDeviceKey);
		return false;
	}

	char szSeqNo[16];
	char szStatus[8];
	char szIntersectLen[16];
	char szMatchLat[32];
	char szMatchLon[32];
	char szMatchLinkId[24];

	snprintf(szSeqNo, sizeof(szSeqNo), "%u", stRawLogInfo.dwSeqNo);
	snprintf(szStatus, sizeof(szStatus), "%d", static_cast<int>(nStatus));
	if (nIntersectLen >= 0)
		snprintf(szIntersectLen, sizeof(szIntersectLen), "%d", nIntersectLen);
	else
		szIntersectLen[0] = '\0';

	// 좌표가 제공되면 상태(MATCHED/SKIP) 무관 저장. 반경 밖 SKIP 도 최근접 좌표 기록 (2026-07-10 최정우 수정)
	if ((pdfMatchLat != nullptr) && (pdfMatchLon != nullptr))
	{
		snprintf(szMatchLat, sizeof(szMatchLat), "%.06lf", *pdfMatchLat);
		snprintf(szMatchLon, sizeof(szMatchLon), "%.06lf", *pdfMatchLon);
	}
	else
	{
		szMatchLat[0] = '\0';
		szMatchLon[0] = '\0';
	}

	// 매칭 링크 ID (0=미제공 → 빈 문자열, SQL CASE 에서 상태별 처리) (2026-07-15 최정우 추가)
	if (qwMatchLinkId != 0)
		snprintf(szMatchLinkId, sizeof(szMatchLinkId), "%llu",
			static_cast<unsigned long long>(qwMatchLinkId));
	else
		szMatchLinkId[0] = '\0';

	RAW_LOG_UPDATE_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strGpsSeq = szSeqNo;
	stRow.strMatchStatus = szStatus;
	stRow.strIntersectLen = szIntersectLen;
	stRow.strMatchLat = szMatchLat;
	stRow.strMatchLon = szMatchLon;
	stRow.strMatchLinkId = szMatchLinkId;
	pvtUpdates->push_back(stRow);
	return true;
}

/**
 * @brief PQexec UPDATE/COMMAND 영향 행 수
 * @param[in] pcResult PQ 실행 결과
 * @return 영향 받은 행 수 (없으면 0)
 * @remark PGRES_COMMAND_OK 여도 WHERE 불일치 시 0 가능 (#5)
*/
int CRawLogWorker::GetPgCmdTuples(PGresult *pcResult)
{
	if (pcResult == nullptr)
		return 0;

	const char *pszAffected = PQcmdTuples(pcResult);
	if ((pszAffected == nullptr) || (pszAffected[0] == '\0'))
		return 0;

	return atoi(pszAffected);
}

/**
 * @brief UPDATE 영향 행 수가 기대값과 일치하는지 검증
 * @param[in] pcResult PQ 실행 결과
 * @param[in] nExpected 기대 갱신 행 수
 * @param[in] pszLogTag 로그 태그 (nullptr 이면 "워커")
 * @return true(일치), false(불일치·pcResult null)
*/
bool CRawLogWorker::CheckPgUpdateAffected(PGresult *pcResult, int nExpected,
		const char *pszLogTag)
{
	// PQcmdTuples 로 영향 행 수 추출 (2026-07-08 최정우 주석 추가)
	const int nAffected = GetPgCmdTuples(pcResult);
	if (nAffected == nExpected)
		return true;

	LOGFMTW("%s partial update! expected=[%d] affected=[%d]",
		(pszLogTag != nullptr) ? pszLogTag : "worker",
		nExpected, nAffected);
	return false;
}

/**
 * @brief prim_rawgps 처리 결과 일괄 갱신 [rawgps_update]
 * @param[in] pcConn DB 커넥션
 * @param[in] vtUpdates bulk UPDATE 대상 행 목록
 * @return true(전건 갱신), false(실행 오류·부분 갱신·인자 무효)
 * @remark
 *   - WHERE MATCH_STATUS=2 인 행만 갱신 (예약된 batch)
 *   - $4=INTERSECT_LEN[] : GPS↔세그먼트 교차점 거리(m). MATCH_LAT/LON 과 함께 AppendUpdateRow 값 사용
 *   - 별도 release SQL 없음. 실패 복구는 BulkReleaseRawLogs() 가 $4=0 으로 동일 SQL 호출
 *   - PGRES_COMMAND_OK 뿐 아니라 PQcmdTuples == vtUpdates.size() 검증 (#5)
 */
bool CRawLogWorker::BulkUpdateRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates)
{
	if (pcConn == nullptr || vtUpdates.empty())
		return false;

	vector<string> vtTripId;
	vector<string> vtGpsSeq;
	vector<string> vtMatchStatus;
	vector<string> vtIntersectLen;
	vector<string> vtMatchLat;
	vector<string> vtMatchLon;
	vector<string> vtMatchLinkId;

	vtTripId.reserve(vtUpdates.size());
	vtGpsSeq.reserve(vtUpdates.size());
	vtMatchStatus.reserve(vtUpdates.size());
	vtIntersectLen.reserve(vtUpdates.size());
	vtMatchLat.reserve(vtUpdates.size());
	vtMatchLon.reserve(vtUpdates.size());
	vtMatchLinkId.reserve(vtUpdates.size());

	for (size_t i=0; i<vtUpdates.size(); ++i)
	{
		const RAW_LOG_UPDATE_ROW& stRow = vtUpdates[i];
		vtTripId.push_back(stRow.strTripId);
		vtGpsSeq.push_back(stRow.strGpsSeq);
		vtMatchStatus.push_back(stRow.strMatchStatus);
		vtIntersectLen.push_back(stRow.strIntersectLen);
		vtMatchLat.push_back(stRow.strMatchLat);
		vtMatchLon.push_back(stRow.strMatchLon);
		vtMatchLinkId.push_back(stRow.strMatchLinkId);
	}

	// rawgps_update text[] 파라미터 리터럴 생성 (2026-07-08 최정우 주석 추가)
	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strGpsSeqArray = BuildPgTextArray(vtGpsSeq);
	string strMatchStatusArray = BuildPgTextArray(vtMatchStatus);
	string strIntersectLenArray = BuildPgTextArray(vtIntersectLen);
	string strMatchLatArray = BuildPgTextArray(vtMatchLat);
	string strMatchLonArray = BuildPgTextArray(vtMatchLon);
	string strMatchLinkIdArray = BuildPgTextArray(vtMatchLinkId);

	// 파라미터 순서 = PRIM_RAWGPS 컬럼 순서 ($1 TRIP_ID, $2 GPS_SEQ, $3 MATCH_LAT,
	//   $4 MATCH_LON, $5 INTERSECT_LEN, $6 MATCH_LINK_ID, $7 MATCH_STATUS)
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

	// rawgps_update bulk UPDATE 실행 (2026-07-08 최정우 주석 추가)
	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strUpdateSQL.c_str(),
		7, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	const int nExpected = static_cast<int>(vtUpdates.size());
	bool bOk = false;

	if (nExecStatus != PGRES_COMMAND_OK)
	{
		LOGFMTE("worker bulk update error! count=[%d] msg=[%s]",
			nExpected, PQresultErrorMessage(pcResult));
	}
	else if (!CheckPgUpdateAffected(pcResult, nExpected, "worker bulk update"))
		bOk = false;
	else
	{
		bOk = true;
		for (size_t i=0; i<vtUpdates.size(); ++i)
		{
			const string& strStatus = vtUpdates[i].strMatchStatus;
			if ((strStatus == "1") || (strStatus == "3") || (strStatus == "4"))
			{
				ClearReleaseRetryCount(MakeReleaseRetryKey(vtUpdates[i].strTripId,
					vtUpdates[i].strGpsSeq));
			}
		}
	}

	PQclear(pcResult);
	return bOk;
}

/**
 * @brief bulk update 실패 시 예약 해제 [rawgps_update] — reserve 의 release 경로
 * @param[in] pcConn DB 커넥션
 * @param[in] vtUpdates release 대상 PK 목록 (match_status 등은 내부에서 0으로 치환)
 * @return true(전건 release), false(실행 오류·부분 release·인자 무효)
 * @remark
 *   - rawgps_select 가 PROCESSING(2) 로 예약한 PK 목록을 PENDING(0) 으로 되돌린다.
 *   - 동일 [rawgps_update] SQL: $4 전부 '', $5~$7 전부 '' (MATCH_*·INTERSECT_LEN 미변경)
 *   - SQL CASE: status 0 은 MATCH_LAT/LON ELSE 분기 → 기존 DB 값 유지
 *   - 다음 poll 에서 PENDING 으로 재예약·재맵매칭 (기동 복구 없이 런타임 복구)
 *   - BulkUpdateRawLogs() 경유 — PQcmdTuples 전건 검증 (#5)
 */
bool CRawLogWorker::BulkReleaseRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates)
{
	if ((pcConn == nullptr) || (vtUpdates.empty()))
		return false;

	vector<RAW_LOG_UPDATE_ROW> vtPending;
	vector<RAW_LOG_UPDATE_ROW> vtError;
	vtPending.reserve(vtUpdates.size());
	vtError.reserve(vtUpdates.size());

	for (size_t i=0; i<vtUpdates.size(); ++i)
	{
		RAW_LOG_UPDATE_ROW stRow = vtUpdates[i];
		stRow.strIntersectLen.clear();
		stRow.strMatchLat.clear();
		stRow.strMatchLon.clear();
		stRow.strMatchLinkId.clear();

		const string strRetryKey = MakeReleaseRetryKey(stRow.strTripId, stRow.strGpsSeq);
		const int nRetryMax = m_stConfig.nRetryMax;
		const int nRetryCount = (nRetryMax > 0)
			? BumpReleaseRetryCount(strRetryKey) : 0;

		if ((nRetryMax > 0) && (nRetryCount >= nRetryMax))
		{
			stRow.strMatchStatus = "4";
			vtError.push_back(stRow);
			LOGFMTW("release retry exhausted!→ERROR trip_id=[%s] seq=[%s] count=[%d/%d]",
				stRow.strTripId.c_str(), stRow.strGpsSeq.c_str(),
				nRetryCount, nRetryMax);
		}
		else
		{
			stRow.strMatchStatus = "0";
			vtPending.push_back(stRow);
		}
	}

	// [버그 수정, 2026-09-11 최정우] vtPending/vtError 를 트랜잭션 없이 독립된 UPDATE 두 번으로
	//   나눠 실행했다 — UpdateTripSeqOrder() 의 2026-09-10 수정과 동일 클래스 버그. 앞(vtPending)
	//   UPDATE 가 성공한 직후 뒤(vtError) UPDATE 가 실패(DB 순단 등)하면, 재시도 소진으로 마땅히
	//   ERROR(4) 로 마감돼야 할 행들이 PROCESSING(2) 에 영구히 남는다(결국 RecoverStaleProcessing()
	//   이 stale_sec 뒤에 회수하므로 데이터 유실은 아니지만, 그동안 재시도 카운터·DB 상태가
	//   불일치한다). UpdateTripSeqOrder() 의 동일한 BEGIN/COMMIT/ROLLBACK 패턴을 그대로 재사용.
	auto fnTxn = [pcConn](const char *pszCmd) -> bool
	{
		PGresult *pcTxnResult = PQexec(pcConn, pszCmd);
		const bool bCmdOk = (pcTxnResult != nullptr)
			&& (PQresultStatus(pcTxnResult) == PGRES_COMMAND_OK);
		if (pcTxnResult != nullptr)
			PQclear(pcTxnResult);
		return bCmdOk;
	};

	if (!fnTxn("BEGIN"))
	{
		LOGFMTE("worker bulk release begin failed! pending=[%zu] error=[%zu]",
			vtPending.size(), vtError.size());
		return false;
	}

	bool bOk = true;
	if (!vtPending.empty())
		bOk = BulkUpdateRawLogs(pcConn, vtPending) && bOk;
	if (bOk && !vtError.empty())
		bOk = BulkUpdateRawLogs(pcConn, vtError) && bOk;

	if (!bOk)
	{
		fnTxn("ROLLBACK");
		return false;
	}

	if (!fnTxn("COMMIT"))
	{
		LOGFMTE("worker bulk release commit failed! pending=[%zu] error=[%zu]",
			vtPending.size(), vtError.size());
		fnTxn("ROLLBACK");
		return false;
	}

	return true;
}

/**
 * @brief 이 매칭 링크를 일반도로(CHARGE_TYPE=0) run 으로 계상해도 되는지 판정
 *   (2026-09-07 최정우 추가, 사용자 지시)
 * @param[in] qwLinkID     이번 tick 의 매칭 링크(또는 경로 링크) ID
 * @param[in] pstSession   진행 중인 트립 세션 — 게이트형 구역 run 이 열려 있는지 확인용
 * @return true = 이 링크는 이번 tick 에 일반도로로 계상 대상(기존 bTouchesUnregistered 와 같은 뜻)
 *
 * @remark BuildNodeStepRow() 헤더 주석 "1. 대상 범위" 가 가리키는 판정 본체다.
 * @remark 왜 필요한가 — 실측 000376_20260819140532 seq48·49
 * \t 링크 2040424301 은 구간단속 RL-Z00003 의 유일한 등록 링크다. 차량은 seq47 뒤 진출게이트
 * \t TG00013 를 통과해 구간단속 run 이 닫혔는데도, seq48·49 는 여전히 그 링크에 매칭됐다.
 * \t 예전 판정(IsLinkChargeRegistered 단독)은 "등록 링크"라는 이유로 미등록 pseudo-zone 대상에서
 * \t 빼버렸고, road_kind=0 정식구역도 아니어서 vtZones 에도 안 잡혔다. 결과적으로 seq48·49 는
 * \t 어떤 run 에도 속하지 못해 과금 이력에서 통째로 사라졌다(일반도로가 23~47 / 51~53 으로 쪼개짐).
 *
 * @remark 판정 규칙 3단
 * \t ① 어디에도 미등록 링크          -> true  (2026-09-01 이래의 케이스2, 그대로)
 * \t ② 일반도로(0)·면제(5) 등록 링크 -> false (각 유형 트랙이 처리한다. 면제를 일반도로로
 * \t                                           흡수하면 면제 구간이 과금돼 버린다)
 * \t ③ 게이트형(1 개방식·2 폐쇄식·3 구간단속)에만 등록된 링크
 * \t     - 그 유형 run 이 하나라도 열려 있으면 -> false (그 유형이 계상 중 — 중복 계상 금지.
 * \t       구간단속은 진행 중 구간을 진출 시 일반도로 미러로 따로 만들어 준다)
 * \t     - 열린 run 이 없으면                  -> true  (게이트 미통과 구간·진출 후 잔여 tick.
 * \t       사용자 원칙 "게이트 조건 미충족 구간은 일반도로 Y/0" 그대로)
 *
 * @remark [2026-09-23 최정우 보완 — ③ 의 적용 범위가 줄었다] 폐쇄식(2)은 이제 **입구게이트를
 * \t 통과하지 않아도 구역 링크에 올라타면 run 이 열린다**(ProcessClosedRoadCharge 의 "구역 중간
 * \t 진입"). 그래서 폐쇄식 링크에서 ③ 이 true 가 되는 경우는 사실상 **진출 후 잔여 tick** 뿐이고,
 * \t "게이트 미통과 구간" 은 그쪽에서 N/3 + 코드21 폐쇄형 행으로 등록된다. 이 함수는 고치지
 * \t 않았다 — 판정 순서상 run 이 먼저 열리므로 ③ 이 자연히 false 가 된다.
 * \t 개방식(1)·구간단속(3)은 종전 그대로다.
 *
 * @remark 게이트형 run 진행 여부는 유형별이 아니라 세션 전체로 본다(bInSpeedZone / bInClosedRoad /
 * \t vtOpenRuns 중 하나라도 열려 있으면 false). 다른 유형 구역 안에 있으면서 이 링크에 매칭되는
 * \t 교차 상황은 보수적으로 "억제"쪽에 두는 편이 안전하다 — 억제는 종전 동작과 같아 회귀가 없다.
 *
 * @remark 영향 범위 실측(2026-09-07, 11개 실주행 트립 1,355점) — 이 함수만 옛 판정으로 되돌린
 * \t 빌드로 전체 재맵매칭해 과금 63행을 대조한 결과 **3행만** 달라졌고 건수는 변하지 않았다.
 * \t   · 000376_20260819140532 trip_seq=1  일반도로 거리 731 -> 761m (빠져 있던 seq48~50 의 30m)
 * \t   · 000370_20260819093236 trip_seq=4  FROM_ID 2040423501 -> 2040423801, 462 -> 463m
 * \t   · 000376_20260821094609 trip_seq=4  FROM_ID 2040423501 -> 2040423801, 462 -> 463m
 * \t 뒤 2건은 폐쇄식 RL-Z00005 링크 2040423801 을 게이트 통과 없이 지난 tick 이 일반도로에
 * \t 편입되면서 시작 링크가 실제 주행 링크로 정정된 것이다. 맵매칭 지표(매칭률·정확도)는 이
 * \t 함수가 과금 단계에서만 쓰이므로 구조적으로 무관하고, 실측에서도 불변이었다.
 *
 * @remark 되돌리는 법 — 이 함수 본문을
 * \t   return !m_stConfig.pcChargeDataLoader->IsLinkChargeRegistered(qwLinkID);
 * \t 한 줄로 바꾸면 2026-09-06 이전 판정으로 정확히 복귀한다.
*/
bool CRawLogWorker::IsLinkNodeStepEligible(const uint64 qwLinkID, const VEHICLE_TRIP_SESSION *pstSession)
{
	if (!m_stConfig.pcChargeDataLoader->IsLinkChargeRegistered(qwLinkID))
		return true;											// ① 미등록
	if (!m_stConfig.pcChargeDataLoader->IsLinkGateZoneOnly(qwLinkID))
		return false;											// ② 일반도로(0)·면제(5)
	if (pstSession == nullptr)
		return false;
	if (pstSession->bInSpeedZone || pstSession->bInClosedRoad || !pstSession->vtOpenRuns.empty())
		return false;											// ③ 그 유형 run 진행 중
	return true;												// ③ 게이트 조건 미충족 구간
}

/**
 * @brief 매칭된 tick 이 하나도 없는 일반도로(CHARGE_TYPE=0) 행을 INSERT 직전에 제거
 * @param[in,out] pvtCharges 이 배치의 과금 행 목록
 * @param[in] vtUpdates 같은 배치의 tick 별 최종 MATCH_STATUS
 * @remark
 * \t**주정차 폴리곤 안도 아니고 맵매칭도 실패이며 복구 대상도 아닌 좌표는 일반도로 과금 이력에
 * \t올라올 수 없다**(사용자 지시, 2026-09-07). 그런 구간이 레코드로 남는 경로가 실제로 있었다 —
 * \t실측 000376_20260819140532: seq4·5 가 2040425401 로 매칭돼 일반도로 run 을 열었는데(로그
 * \t`node step entry(unregistered) seq=[4] trusted=[1]`), 뒤이어 "트립 시작 모호구간 A/B 판정"이
 * \t그 링크를 패자로 확정해 **DB 기록만** SKIP·좌표 삭제로 정정했다(그 로직의 종전 정책이
 * \t"과금은 되돌리지 않는다"였다). 결과적으로 DB 상 매칭 실패인 두 tick 에 27m 일반도로가 남았다.
 *
 * \t run 이 생성~마감 사이에 여러 컨테이너(vtNodeStepRuns / stHeldNodeStepRun / stParkTouchCarry /
 * \t stMergeCarry)를 옮겨다녀 중간에서 취소하려는 시도가 번번이 빗나갔다. 반면 **최종 결과물은
 * \t 이 한 곳에 모이므로**, 경로를 몰라도 결과만 검사하면 된다. 그래서 run 생성·이월·마감·누락
 * \t 링크 복구 등 기존 로직에는 일절 손대지 않고 여기서만 거른다.
 *
 * \t판정: 행의 [START_GPS_SEQ ~ END_GPS_SEQ] 안에 MATCH_STATUS=1 인 tick 이 하나라도 있으면 남기고,
 * \t하나도 없으면 버린다. **그 구간 tick 이 이 배치에 아예 없으면(트립이 배치 경계에 걸침) 판단
 * \t불가로 보고 남긴다** — 안 보인다고 버리면 정상 레코드를 잃는다.
 * \tCHARGE_TYPE=0 만 대상이다. 게이트형·면제·주정차는 구역 판정이 별도 근거로 서므로 건드리지 않는다.
 *
 * \t[2026-09-23 최정우 확장, 사용자 확정] **예외 하나 — 폐쇄형(2) 중 코드21 행**도 대상에 넣는다.
 * \t위 "별도 근거" 란 게이트 통과 관측을 말하는데, 구역 중간 진입으로 열린 run
 * \t(non_charge_reason=NCR_CLOSED_ENTRY_UNOBSERVED)에는 그 근거가 없어 판정이 전적으로 매칭 링크에
 * \t달려 있다. 그래서 같은 검사를 받아야 한다 — 실측 000370_20260826143912 seq11 은 그 tick 이
 * \t최종적으로 SKIP 이었는데도 1 tick 짜리 폐쇄형 행이 남았다(어디를 달렸는지 모르는 구간을 청구).
 * \t함수 이름은 종전대로 두되(호출부·관련 주석 다수), 실제 대상은 "매칭 링크 외에 근거가 없는 행"
 * \t으로 읽을 것. 버릴 때 찍는 로그도 charge_type 을 함께 남긴다.
*/
void CRawLogWorker::DropNodeStepRowsWithoutMatch(vector<CHARGE_INSERT_ROW> *pvtCharges,
		const vector<RAW_LOG_UPDATE_ROW>& vtUpdates)
{
	if ((pvtCharges == nullptr) || pvtCharges->empty() || vtUpdates.empty())
		return;

	// (trip_id, gps_seq) -> MATCHED 여부
	map<string, bool> mapTickMatched;
	for (size_t i = 0; i < vtUpdates.size(); ++i)
		mapTickMatched[vtUpdates[i].strTripId + "|" + vtUpdates[i].strGpsSeq]
			= (vtUpdates[i].strMatchStatus == "1");

	vector<CHARGE_INSERT_ROW> vtKept;
	vtKept.reserve(pvtCharges->size());
	size_t nDropped = 0;
	for (size_t i = 0; i < pvtCharges->size(); ++i)
	{
		const CHARGE_INSERT_ROW& stRow = (*pvtCharges)[i];
		bool bKeep = true;
		// [2026-10-04 최정우 추가 — E18 부수] 거리 0·체류 0초 일반도로 행은 담긴 주행이 없으므로 등록하지 않는다.
		//   [2026-10-04 사용자 확정(권장안 가) — E18-1] 체류 0초는 같은 gps_dt tick 만 남은 경우다(예: 마지막 tick 과 같은 초의
		//   트립 종료 이벤트). 체류 1초 이상인 0m 행(정차 구간)은 그대로 등록한다.
		//   E18(폴리곤 안 SKIP 뒤 run 재시작) 이후 주차 상태로 끝나는 tick 들이 0m/0s 행이 되는 것을 막는다
		//   (실측 000376_20260821095603 167~168). 체류가 있는 0m 행(정차만 한 구간)은 그대로 둔다.
		// 되돌리는 법: 이 if 블록을 지운다.
		if ((stRow.strChargeType == "0") && (atol(stRow.strDistM.c_str()) == 0)
			&& (atol(stRow.strStaySeconds.c_str()) == 0))
		{
			++nDropped;
			LOGFMTI("charge row dropped (zero distance and stay)!trip_id=[%s] seq=[%s~%s] from=[%s] to=[%s]",
				stRow.strTripId.c_str(), stRow.strStartGpsSeq.c_str(), stRow.strEndGpsSeq.c_str(),
				stRow.strFromId.c_str(), stRow.strToId.c_str());
			continue;
		}
		// [2026-09-23 최정우 추가, 사용자 확정] 폐쇄형(2) 중에서도 **진입게이트 근거 없이 매칭
		//   링크만으로 열린 행**(구역 중간 진입, non_charge_reason=21)은 같은 검사를 받는다.
		//   위 주석이 "게이트형은 구역 판정이 별도 근거로 서므로 건드리지 않는다"고 한 그 별도
		//   근거(게이트 통과 관측)가 이 행에는 없어서, 판정이 전적으로 매칭 링크에 달려 있기
		//   때문이다. 실측 000370_20260826143912 seq11 은 그 tick 이 최종적으로 SKIP 이었는데도
		//   1 tick 짜리 폐쇄형 행이 남았다 — 어디를 달렸는지 모르는 구간을 청구한 셈이다.
		const bool bGatelessClosedRow = (stRow.strChargeType == "2")
			&& (stRow.strNonChargeReason == to_string(static_cast<int>(NCR_CLOSED_ENTRY_UNOBSERVED)));
		if ((stRow.strChargeType == "0") || bGatelessClosedRow)
		{
			const long nStart = atol(stRow.strStartGpsSeq.c_str());
			const long nEnd = atol(stRow.strEndGpsSeq.c_str());
			if ((nStart > 0) && (nEnd >= nStart))
			{
				bool bSeen = false, bMatchedAny = false;
				for (long q = nStart; (q <= nEnd) && !bMatchedAny; ++q)
				{
					// [버그 수정, 2026-09-11 최정우] GPS_SEQ 는 트립 내 순번이라 실제로는 항상 작은
					//   값이지만, q(long)는 문자열에서 런타임에 읽은 값이라 컴파일러가 상한을 증명
					//   못해 long 이론상 최댓값(부호+19자리+NUL=21바이트) 기준으로 -Wformat-truncation
					//   경고를 낸다(16바이트는 부족) — 무해한 경고였지만 여유 있게 24바이트로 확장
					char szSeq[24];
					snprintf(szSeq, sizeof(szSeq), "%ld", q);
					map<string, bool>::const_iterator it =
						mapTickMatched.find(stRow.strTripId + "|" + szSeq);
					if (it == mapTickMatched.end()) continue;	// 이 배치에 없는 tick
					bSeen = true;
					if (it->second) bMatchedAny = true;
				}
				if (bSeen && !bMatchedAny)
				{
					bKeep = false;
					++nDropped;
					LOGFMTW("charge row dropped (no matched tick)!trip_id=[%s] charge_type=[%s] "
						"seq=[%s~%s] dist_m=[%s] from=[%s] to=[%s]",
						stRow.strTripId.c_str(), stRow.strChargeType.c_str(),
						stRow.strStartGpsSeq.c_str(), stRow.strEndGpsSeq.c_str(),
						stRow.strDistM.c_str(), stRow.strFromId.c_str(), stRow.strToId.c_str());
				}
			}
		}
		if (bKeep) vtKept.push_back(stRow);
	}
	if (nDropped > 0)
		pvtCharges->swap(vtKept);
}

/**
 * @brief 배치 INSERT 직전, 같은 트립에서 연속으로 이어지는 일반도로(CHARGE_TYPE=0) 행을 하나로
 *   합치고 거리·체류시간·평균속도를 다시 계산한다 (2026-09-06 최정우 추가, 사용자 지시)
 * @param[in,out] pvtCharges 이 배치의 과금 행 목록 — 흡수된 행은 제거된다
 * @remark 일반도로는 "연속 주행 구간 하나 = 레코드 하나"가 원칙이다. run 이 구역 진입 tick 소비·
 *   디바운스·보류 등으로 쪼개져도 여기서 다시 붙인다. 규칙은 넷이다:
 * \t  · **순서** — 트립별 START_GPS_SEQ 오름차순으로 훑는다(적재 순서가 아니다). 적재 순서로
 * \t    돌면 구간이 겹친 채 둘 다 남아 이중 계상된다(본문 2026-09-15 주석 참고).
 * \t  · **이어짐 판정** — 같은 tick 공유, 바로 다음 tick, 또는 한 tick 만 비는 경우까지(구역
 * \t    진입 tick 이 그 구역 세션에 소비된 상황). 두 tick 이상 비면 합치지 않는다.
 * \t  · **합치지 않는 예외** — ① 앞 행의 bNoMergeAfter(사이에 실제 다른 과금유형 등록 링크가
 * \t    껴 진짜로 끊긴 "섬") ② 병합 결과가 다른 과금유형 행을 **진부분집합으로** 감싸는 경우
 * \t    (구간단속 미러가 병합을 타고 앞 구간까지 끌어당기던 문제, 2026-09-16).
 * \t  · **판정 승계** — charge_yn/status/non_charge_reason 은 완전 일치를 요구하지 않는다.
 * \t    둘 중 하나라도 비정상이면 전체를 비정상으로 통일한다("정확도 우선").
 *   DropNodeStepRowsWithoutMatch() 를 **먼저** 돌린 뒤에 호출해야 한다 — 순서가 바뀌면 매칭
 *   tick 이 없는 구간이 정상 구간에 이미 흡수돼 걸러낼 수 없다(run() 호출부 주석 참고).
 *   (2026-09-17 최정우 — 헤더 선언부가 "상세 규칙은 구현부 주석 참고" 라고 가리키는데 정작
 *    구현부에 설명이 없어 보완. 동작 변화 없음)
*/
void CRawLogWorker::MergeAdjacentNodeStepRows(vector<CHARGE_INSERT_ROW> *pvtCharges)
{
	if ((pvtCharges == nullptr) || (pvtCharges->size() < 2))
		return;

	// 트립·기기별 "직전 일반도로 행"의 인덱스 — 배치 벡터에는 여러 트립이 섞여 있어 벡터상
	//   바로 앞 행이 같은 트립이라는 보장이 없다
	map<string, size_t> mapLastNodeStep;
	vector<bool> vtDropped(pvtCharges->size(), false);
	size_t nMerged = 0;

	// [버그 수정, 2026-09-15 최정우] 병합은 반드시 트립별 START_GPS_SEQ 오름차순으로 돌아야 한다 —
	//   pvtCharges 의 순서는 "마감·적재된 순서"라 실제 주행 순서와 다를 수 있다(트립종료·TTL 일괄
	//   마감분이, 그보다 먼저 열려 아직 안 닫힌 run 보다 앞서 들어오는 등). 순서가 어긋나면 이어짐
	//   판정(nCurStart >= nPrevEnd)이 거짓이 되어 병합이 끊기는데, 이때 앞 행은 이미 뒤 구간까지
	//   흡수한 뒤라 두 행의 gps_seq 구간이 겹친 채 둘 다 남는다 = 그만큼 이중 계상.
	//   실측 000376_20260826160622: 적재순서가 [1~103] → [104~242(구간단속 미러)] → [104~241] 이라
	//   앞 둘이 1~242(5617m)로 합쳐진 뒤 104~241(2069m)이 따로 남아 2069m 가 두 번 잡혔다.
	//   한 배치에 트립이 몇 개 섞이느냐에 따라 적재순서가 달라져 전체 재매칭과 단독 재매칭의 결과가
	//   갈리던 원인이기도 하다(사용자 지적).
	vector<size_t> vtOrder;
	for (size_t i = 0; i < pvtCharges->size(); ++i)
	{
		if ((*pvtCharges)[i].strChargeType == "0")
			vtOrder.push_back(i);
	}
	stable_sort(vtOrder.begin(), vtOrder.end(),
		[pvtCharges](size_t nIdxA, size_t nIdxB) -> bool
		{
			const CHARGE_INSERT_ROW& stA = (*pvtCharges)[nIdxA];
			const CHARGE_INSERT_ROW& stB = (*pvtCharges)[nIdxB];
			const string strKeyA = stA.strTripId + "|" + stA.strDeviceKey;
			const string strKeyB = stB.strTripId + "|" + stB.strDeviceKey;
			if (strKeyA != strKeyB) return strKeyA < strKeyB;
			return atol(stA.strStartGpsSeq.c_str()) < atol(stB.strStartGpsSeq.c_str());
		});

	for (size_t k = 0; k < vtOrder.size(); ++k)
	{
		const size_t i = vtOrder[k];
		CHARGE_INSERT_ROW& stCur = (*pvtCharges)[i];

		const string strKey = stCur.strTripId + "|" + stCur.strDeviceKey;
		map<string, size_t>::iterator it = mapLastNodeStep.find(strKey);
		if (it != mapLastNodeStep.end())
		{
			CHARGE_INSERT_ROW& stPrev = (*pvtCharges)[it->second];
			const long nPrevEnd = atol(stPrev.strEndGpsSeq.c_str());
			const long nCurStart = atol(stCur.strStartGpsSeq.c_str());
			// 이어짐 판정 — 같은 tick 공유(게이트·경계 지점), 바로 다음 tick, 또는 **한 tick 만
			//   비는 경우**까지 포함한다. 마지막 경우는 구역 진입 tick 이 그 구역 세션에 소비돼
			//   어느 일반도로 레코드에도 안 들어가는 상황이다 — 실측 000376_20260819140856:
			//   일반도로 2~12 와 13~23 사이의 seq13 이 구간단속 진입 tick 으로 소비돼 12↔14 로
			//   벌어졌고, 연속 주행인데도 두 레코드로 쪼개졌다. 두 tick 이상 비면 별개 구간일
			//   가능성이 커지므로 합치지 않는다 (2026-09-06 최정우 수정, 사용자 지시)
			const bool bContiguous = (nPrevEnd > 0) && (nCurStart > 0)
				&& (nCurStart >= nPrevEnd) && (nCurStart <= nPrevEnd + 2);

			// bNoMergeAfter — gps_seq는 이어 보여도 사이에 실제 다른 과금유형 등록 링크가 껴
			//   있어 진짜로 끊긴 구간(ApplyGateExitCarryDist()의 섬 분리 산출물)이면 병합 금지
			//   (2026-09-14 최정우 추가)

			// [버그 수정, 2026-09-16 최정우 — 사용자 지적] **병합 결과가 다른 과금유형 구간을 통째로
			//   감싸게 되면 병합하지 않는다.**
			//   실측 000376_20260819094414: 앞 일반도로(1~42) 마감 → 구간단속(43~53) 기록 + 위반
			//   미러 생성(진입 seq43) → 뒤 일반도로 run(54~61)이 그 미러를 흡수하며 **진입 정보를
			//   미러 것(43)으로 앞당겨** 43~61 이 됐고, 앞 행 끝 42 와 인접 판정(43 <= 42+2)에 걸려
			//   1~61 로 병합됐다. 그 결과 구간단속 43~53 이 일반도로 한 행 안에 통째로 들어가,
			//   dist_m(구간단속분이 미러로 더해진 값)과 START/END_GPS_SEQ 가 가리키는 구간이 어긋난다.
			//   **미러 자체는 정책이다** — 위반 시 SPEED 와 같은 구간을 공유하는 일반도로 행을
			//   의도적으로 함께 등록한다(ProcessSpeedZoneCharge 주석). 여기서 막는 건 그 미러가
			//   **병합을 타고 앞 구간까지 끌어당기는 것**뿐이라, 미러 행(43~61)은 그대로 남는다.
			//   판정은 "병합 후 범위가 타 유형 행을 **진부분집합으로** 포함하는가"다 — 같은 구간을
			//   공유하는 미러(시작·끝이 같음)는 이 조건에 걸리지 않는다.
			bool bWouldWrapOtherType = false;
			if (bContiguous)
			{
				const long nMergedStart = atol(stPrev.strStartGpsSeq.c_str());
				const long nMergedEnd   = atol(stCur.strEndGpsSeq.c_str());
				for (size_t m = 0; (m < pvtCharges->size()) && !bWouldWrapOtherType; ++m)
				{
					const CHARGE_INSERT_ROW& stOther = (*pvtCharges)[m];
					if (stOther.strChargeType == "0") continue;			// 일반도로끼리는 대상 아님
					// 주정차 행은 일반도로와 범위가 겹쳐도 되고(정책 대조표 B6), 정차가 끼어도 일반도로를
					//   나누지 않는다(C3, 사용자 확정). 그래서 주정차 행을 감싸는 병합은 막지 않는다 —
					//   시나리오 980002(정차 후 출차)가 1~133/134~148 로 쪼개지던 것 (2026-10-04 최정우 추가).
					// 되돌리는 법: 이 한 줄을 지운다.
					if (stOther.strChargeType == "4") continue;
					// [2026-10-04 최정우 수정, 사용자 확정] **구간단속(3) 행도 감싸도 된다** — 구간단속 앞 일반도로와
					//   미러(구간단속 구간 + 진출 후 일반도로)를 하나의 연속 일반도로 행으로 합친다. 2026-09-16 의
					//   감싸기 금지(000376_20260819094414 1~61)를 구간단속에 한해 되돌린 것이다. 근거: 미러가 이미
					//   구간단속을 감싸고 있어(6~25 ⊃ 6~19) 진입 앞쪽만 끊기는 비대칭이었고, 주정차(B6·C3)와 같은
					//   "연속 주행 = 1 레코드" 원칙. 거리 합계 불변(이중계상 없음). 판정은 아래 병합 규칙(한쪽이라도
					//   비정상이면 전체 비정상)을 그대로 따른다.
					//   실측(시나리오): 980015_20261005010000 일반 2~6(125m)+6~25(469m) -> 2~25(594m).
					//   짝 변경: CalcChargeWatermark() ⑤ — 구간단속 통과 중에도 앞 일반도로 행을 보류.
					// 되돌리는 법: 이 한 줄과 ⑤ 의 bInSpeedZone/bHasHeldSpeedMirrorRun 조건을 지운다.
					if (stOther.strChargeType == "3") continue;
					if (stOther.strTripId != stCur.strTripId) continue;
					if (stOther.strDeviceKey != stCur.strDeviceKey) continue;
					const long nOtherStart = atol(stOther.strStartGpsSeq.c_str());
					const long nOtherEnd   = atol(stOther.strEndGpsSeq.c_str());
					if ((nOtherStart <= 0) || (nOtherEnd <= 0)) continue;
					if ((nMergedStart < nOtherStart) && (nOtherEnd < nMergedEnd))
						bWouldWrapOtherType = true;
				}
			}

			if (bContiguous && !stPrev.bNoMergeAfter && !bWouldWrapOtherType)
			{
				// [2026-09-22 최정우 추가] 덮은 링크 목록도 합친다 — 안 합치면 뒤 행이 덮던 링크가
				//   커버리지 대조에서 "미덮임" 으로 잘못 잡힌다
				for (size_t ci = 0; ci < stCur.vtCoveredLinks.size(); ++ci)
					stPrev.vtCoveredLinks.push_back(stCur.vtCoveredLinks[ci]);

				const long nDist = atol(stPrev.strDistM.c_str()) + atol(stCur.strDistM.c_str());
				const long nStay = atol(stPrev.strStaySeconds.c_str()) + atol(stCur.strStaySeconds.c_str());

				char szBuf[32];
				snprintf(szBuf, sizeof(szBuf), "%ld", nDist);
				stPrev.strDistM = szBuf;
				snprintf(szBuf, sizeof(szBuf), "%ld", nStay);
				stPrev.strStaySeconds = szBuf;
				if (nStay > 0)
				{
					snprintf(szBuf, sizeof(szBuf), "%d",
						static_cast<int>((static_cast<double>(nDist) / static_cast<double>(nStay)) * 3.6 + 0.5));
					stPrev.strSpeedKmh = szBuf;
				}

				stPrev.strEndGpsSeq = stCur.strEndGpsSeq;
				// [버그 수정, 2026-09-15 최정우, 소스 재검토 지적] bNoMergeAfter 도 같이 옮긴다 —
				//   stCur 가 "섬 분리"(사이에 다른 과금유형 등록 링크가 껴 진짜로 끊긴 구간) 행이면
				//   흡수한 stPrev 가 그 표시를 물려받아야 한다. 안 옮기면 stPrev 가 false 로 남아
				//   다음 행까지 연달아 병합돼 2026-09-14 섬 분리 취지가 무효화된다.
				stPrev.bNoMergeAfter = stCur.bNoMergeAfter;
				stPrev.strToId = stCur.strToId;
				stPrev.strToLat = stCur.strToLat;
				stPrev.strToLon = stCur.strToLon;
				stPrev.strOccurDt = stCur.strOccurDt;
				// UPD_DT 는 그대로 둔다 — 최종 수정 일시(REG_DT 와 같음)라 병합으로 바뀌지 않는다(2026-10-04 최정우 수정, 종전 OCCUR_DT)

				// charge_yn/status/non_charge_reason 은 더 이상 완전 일치를 요구하지 않는다 —
				//   구간단속(3)도 일반도로 계열이라 그 구간을 지나며 이어지는 게 정상인데, 뒤쪽이
				//   TTL 등으로 우연히 N/3 강제종료되면 앞쪽(Y/0)과 값이 달라 병합이 막혔다(사용자
				//   지적, 2026-09-14 최정우 수정 — 실측 000376_20260819141002 seq40~49(Y/0)+
				//   50~129(N/3, 구간단속 통과 뒤 TTL 강제종료)가 하나여야 할 연속 주행인데 둘로
				//   남았다). 둘 중 하나라도 비정상이면 전체를 보수적으로 비정상 쪽으로 통일한다
				//   ("정확도 우선" 원칙 — 일부 구간이 불확실하면 전체를 정상으로 볼 근거가 없다).
				//   둘 다 비정상이면 뒤 행(stCur) 사유를 우선한다(보통 더 나중에 확정된 원인).
				const bool bPrevNormal = (stPrev.strChargeYn == "Y") && (stPrev.strChargeStatus == "0");
				const bool bCurNormal = (stCur.strChargeYn == "Y") && (stCur.strChargeStatus == "0");
				if (!bCurNormal)
				{
					stPrev.strChargeYn = stCur.strChargeYn;
					stPrev.strChargeStatus = stCur.strChargeStatus;
					stPrev.strNonChargeReason = stCur.strNonChargeReason;
				}
				else if (!bPrevNormal)
				{
					// stPrev 가 이미 비정상 — 그대로 유지(덮어쓰지 않음). 필드는 이미 stPrev 값
				}

				vtDropped[i] = true;
				nMerged += 1;
				continue;								// 앞 행이 계속 "직전 행" 역할을 이어간다
			}
		}
		mapLastNodeStep[strKey] = i;
	}

	if (nMerged == 0)
		return;

	vector<CHARGE_INSERT_ROW> vtKept;
	vtKept.reserve(pvtCharges->size() - nMerged);
	for (size_t i = 0; i < pvtCharges->size(); ++i)
	{
		if (!vtDropped[i])
			vtKept.push_back((*pvtCharges)[i]);
	}
	pvtCharges->swap(vtKept);

	LOGFMTI("node step rows merged!merged=[%zu] remain=[%zu]", nMerged, pvtCharges->size());
}

/**
 * @brief 과금 이력 bulk INSERT [charge_insert] — 6개 과금유형(일반도로·개방식·폐쇄식·
 *   구간단속·주정차·면제도로) 행을 한 번에 적재한다 (2026-08-12 최정우 추가,
 *   제목 정정 2026-09-17 — "개방형 게이트 통과" 는 도입 당시 용도라 현행과 맞지 않았다)
 * @param[in] pcConn DB 커넥션
 * @param[in] vtCharges bulk INSERT 대상 행 목록 (각 Process*Charge() 가 적재)
 * @return true(성공), false(실행 오류·인자 무효)
 * @remark
 *   - rawgps_update 와 동일한 UNNEST text[] 파라미터 패턴 (BuildPgTextArray)
 *   - PK(trip_id, device_key, trip_seq) 충돌은 ON CONFLICT DO NOTHING(query.sql) — 재시도 배치의
 *     중복 INSERT 방어용
 *   - 호출측(run())이 실패 시 rawgps_update 와 동일하게 배치 release·세션 미커밋 처리 —
 *     과금 행이 누락 없이 다음 poll 에서 재시도되도록 함
*/
bool CRawLogWorker::BulkInsertCharges(PGconn *pcConn, const vector<CHARGE_INSERT_ROW>& vtCharges)
{
	if ((pcConn == nullptr) || vtCharges.empty() || m_stConfig.strChargeInsertSQL.empty())
		return false;

	vector<string> vtTripId, vtDeviceKey, vtChargeSeq, vtChargeType, vtChargeUnit, vtLinkId,
		vtFromId, vtToId, vtFromLat, vtFromLon, vtToLat, vtToLon, vtZoneId, vtZoneName,
		vtDistM, vtSpeedKmh, vtSpeedLimitKmh, vtOccurDt, vtTripStartDt, vtTollgateId,
		vtEntryTollgateId, vtExitTollgateId, vtRegDt, vtUpdDt, vtChargeYn, vtChargeStatus,
		vtStaySeconds, vtTripEndDt, vtStartGpsSeq, vtEndGpsSeq, vtNonChargeReason;

	for (size_t i=0; i<vtCharges.size(); ++i)
	{
		const CHARGE_INSERT_ROW& stRow = vtCharges[i];

		// [버그 수정, 2026-09-15 최정우, 사용자 지적] STAY_SECONDS/SPEED_KMH 음수 방어 —
		//   두 값을 만드는 지점이 16곳인데 하한 보정이 있는 곳은 3곳뿐이라, 진입시각 > 종료시각이
		//   되는 경로가 하나라도 생기면 음수가 그대로 적재된다(전례: 2026-09-02
		//   000370_20260824135458 trip_seq=4 의 stay_seconds=-1313903640). SPEED_KMH 는 SMALLINT 라
		//   범위를 벗어나면 배치 INSERT 자체가 실패해 재시도 폭주까지 간다. 16곳을 각각 고치는 대신
		//   적재 직전 한 곳에서 0 으로 보정하되, 조용히 감추지 않도록 WARN 으로 원본값을 남겨
		//   trip_id/charge_seq 로 원인을 추적할 수 있게 한다.
		string strStayFixed = stRow.strStaySeconds;
		string strSpeedFixed = stRow.strSpeedKmh;
		// [보강, 2026-09-15 최정우, 소스 재검토 지적] **양수 오버플로도 막는다.** 재발 방지 근거로
		//   든 실제 장애(13,199km -> speed_kmh 오버플로 -> 배치 INSERT 전체 실패 -> 재시도 폭주)는
		//   음수가 아니라 양수였다. SPEED_KMH 는 query.sql 에서 ::SMALLINT 캐스팅이라 32767 을 넘는
		//   값 1건이 그 배치의 정상 과금행까지 전부 죽인다. STAY_SECONDS/DIST_M 은 INTEGER 라
		//   상한 위험이 낮아 음수만 본다.
		static const long MM_SMALLINT_MAX = 32767;
		if (atol(strSpeedFixed.c_str()) > MM_SMALLINT_MAX)
		{
			LOGFMTW("speed_kmh over smallint clamped!device=[%s] trip_id=[%s] seq=[%s] "
				"charge_type=[%s] speed_kmh=[%s] dist_m=[%s] stay_seconds=[%s]",
				stRow.strDeviceKey.c_str(), stRow.strTripId.c_str(), stRow.strChargeSeq.c_str(),
				stRow.strChargeType.c_str(), strSpeedFixed.c_str(),
				stRow.strDistM.c_str(), strStayFixed.c_str());
			strSpeedFixed = "32767";
		}
		if ((atol(strStayFixed.c_str()) < 0) || (atol(strSpeedFixed.c_str()) < 0))
		{
			LOGFMTW("negative charge value clamped to 0!device=[%s] trip_id=[%s] seq=[%s] "
				"charge_type=[%s] stay_seconds=[%s] speed_kmh=[%s]",
				stRow.strDeviceKey.c_str(), stRow.strTripId.c_str(),
				stRow.strChargeSeq.c_str(), stRow.strChargeType.c_str(),
				strStayFixed.c_str(), strSpeedFixed.c_str());
			if (atol(strStayFixed.c_str()) < 0) strStayFixed = "0";
			if (atol(strSpeedFixed.c_str()) < 0) strSpeedFixed = "0";
		}

		// [2026-09-21 최정우 추가, 사용자 확정 — 이슈 27 권장안 ③] STAY_SECONDS=0 최후 방어.
		//   거리는 있는데 체류시간이 0 이면 SPEED_KMH 도 0 이 되어 **주행해서 지나간 구간이
		//   "정차"로 읽힌다.** 정상 경로들은 이제 실제 시간 창을 거리 비율로 배분하므로
		//   (ApplyGateExitCarryDist 사전 패스 / park-touch gap 보정 / 같은 링크 경계 보정)
		//   여기 걸리면 **아직 배분이 배선되지 않은 생성 경로가 남아 있다는 신호**다.
		//   값은 최소한으로만 메운다 — 1초로 올리고 속도를 재계산해 행 자체의 정합성만 맞추되,
		//   WARN 으로 원본을 남겨 어느 경로인지 추적할 수 있게 한다(음수·오버플로 클램프와 동일 관례).
		//   DIST_M=0 인 행은 건드리지 않는다 — 실제로 0m 인 정상 레코드다(정차 중 트립종료 등).
		//   실측: 배분 배선 후 도달 0건이 정상이다.
		if ((atol(strStayFixed.c_str()) <= 0) && (atol(stRow.strDistM.c_str()) > 0))
		{
			const long nDistM = atol(stRow.strDistM.c_str());
			LOGFMTW("zero stay_seconds with distance!device=[%s] trip_id=[%s] seq=[%s] charge_type=[%s] "
				"dist_m=[%ld] speed_kmh=[%s] -> stay_seconds=1 (경과시간 배분 미배선 경로)",
				stRow.strDeviceKey.c_str(), stRow.strTripId.c_str(), stRow.strChargeSeq.c_str(),
				stRow.strChargeType.c_str(), nDistM, strSpeedFixed.c_str());

			strStayFixed = "1";
			char szSpeedFix[16];
			snprintf(szSpeedFix, sizeof(szSpeedFix), "%ld",
				static_cast<long>((static_cast<double>(nDistM) * 3.6) + 0.5));
			strSpeedFixed = szSpeedFix;
		}

		// [보강, 2026-09-15 최정우, 소스 재검토 지적] SPEED_LIMIT_KMH 도 같은 이유로 클램프한다.
		//   이 값은 SPEED_KMH 와 달리 우리가 계산한 게 아니라 **기준정보에서 그대로 읽어온 값**
		//   (pstZone->dfSpeedLimitKmh ← ruc.base_roadlink.speed_limit_kmh)인데, 그 컬럼은
		//   precision 도 CHECK 제약도 없는 numeric 이라 운영자가 지도관리 화면에서 아무 값이나
		//   넣을 수 있다(2026-09-15 doc/deploy_2026-09-15.sql 3절이 DB 측 CHECK(0~255)도 추가한다 —
		//   그건 입력 차단, 이 클램프는 배치 전멸을 막는 소비자 측 방어라 역할이 다르고 둘 다 유지한다.
		//   실서버 적용 전에는 CHECK 이 없으므로 이 클램프가 유일한 방어다).
		//   query.sql 은 이 값을 ::SMALLINT 로 캐스팅하므로 32767 초과 1건이
		//   그 배치의 정상 과금행까지 전부 죽인다(SPEED_KMH 오버플로 장애와 동일 파급 경로).
		//   현재 실데이터는 20~90 이라 당장 발현되지는 않는다.
		string strSpeedLimitFixed = stRow.strSpeedLimitKmh;
		if (!strSpeedLimitFixed.empty())
		{
			const long nSpeedLimit = atol(strSpeedLimitFixed.c_str());
			if ((nSpeedLimit > MM_SMALLINT_MAX) || (nSpeedLimit < 0))
			{
				LOGFMTW("speed_limit_kmh out of smallint range clamped!device=[%s] trip_id=[%s] "
					"seq=[%s] charge_type=[%s] zone_id=[%s] speed_limit_kmh=[%s]",
					stRow.strDeviceKey.c_str(), stRow.strTripId.c_str(), stRow.strChargeSeq.c_str(),
					stRow.strChargeType.c_str(), stRow.strZoneId.c_str(), strSpeedLimitFixed.c_str());
				strSpeedLimitFixed = (nSpeedLimit < 0) ? "0" : "32767";
			}
		}

		// [보강, 2026-09-17 최정우, 사용자 지시] 시각 컬럼 빈 값 방어 — OCCUR_DT/TRIP_START_DT/
		//   REG_DT/UPD_DT 는 DB 에서 **varchar NOT NULL** 이다. varchar 라 빈 문자열이 타입 오류를
		//   내지 않고, NOT NULL 도 ''(NULL 아님)을 막지 못하며, 컬럼 DEFAULT 는 [charge_insert] 가
		//   값을 명시하므로 발동하지 않는다 — 즉 **빈 값이 아무 경고 없이 그대로 적재된다**.
		//   빌 수 있는 경로: FormatDateTime14() 는 시각이 0 이하면 빈 문자열을 반환하는데,
		//   UPD_DT 를 OCCUR_DT 에서 복사하는 일반도로·면제도로 계열(BuildNodeStepRow/
		//   BuildNodeStepRowFromLinkRange/BuildExemptRow)이 여기에 노출된다. REG_DT 계열은
		//   time(nullptr) 이라 구조적으로 빌 수 없지만 방어는 같이 건다.
		//   보정 순서는 "덜 임의적인 값 우선" — OCCUR_DT 는 REG_DT(그 행이 만들어진 시각)로,
		//   TRIP_START_DT 는 보정된 OCCUR_DT 로, UPD_DT 는 보정된 REG_DT 로 메운다.
		//   SQL 에 COALESCE 를 넣지 않고 여기서 처리하는 이유는 **흔적을 남기기 위해서**다 —
		//   SQL 로 덮으면 조용히 메워져 원인을 추적할 수 없다. 기존 STAY_SECONDS/SPEED_KMH
		//   음수 보정과 동일한 자리·동일한 방식(WARN + trip_id/charge_seq)이다.
		//   TRIP_END_DT 는 nullable 이라 대상이 아니다 — 빈 값은 [charge_insert] 의
		//   NULLIF(...,'') 가 NULL 로 바꿔 주는 것이 정답이다(그쪽이 [trip_end] 의
		//   `TRIP_END_DT IS NULL` 조건과 맞물린다 — 종전 [trip_abend] 는 2026-10-04 삭제).
		//   실측: 현재 적재분에 빈 값 0건 — 발생 이력 없는 예방 조치다.
		string strRegFixed = stRow.strRegDt;
		string strOccurFixed = stRow.strOccurDt;
		string strTripStartFixed = stRow.strTripStartDt;
		string strUpdFixed = stRow.strUpdDt;
		if (strRegFixed.empty() || strOccurFixed.empty()
			|| strTripStartFixed.empty() || strUpdFixed.empty())
		{
			LOGFMTW("empty datetime column filled!device=[%s] trip_id=[%s] seq=[%s] charge_type=[%s] "
				"occur_dt=[%s] trip_start_dt=[%s] reg_dt=[%s] upd_dt=[%s]",
				stRow.strDeviceKey.c_str(), stRow.strTripId.c_str(), stRow.strChargeSeq.c_str(),
				stRow.strChargeType.c_str(), strOccurFixed.c_str(), strTripStartFixed.c_str(),
				strRegFixed.c_str(), strUpdFixed.c_str());

			if (strRegFixed.empty())
				strRegFixed = FormatDateTime14(time(nullptr));
			if (strOccurFixed.empty())
				strOccurFixed = strRegFixed;
			if (strTripStartFixed.empty())
				strTripStartFixed = strOccurFixed;
			if (strUpdFixed.empty())
				strUpdFixed = strRegFixed;
		}

		vtTripId.push_back(stRow.strTripId);
		vtDeviceKey.push_back(stRow.strDeviceKey);
		vtChargeSeq.push_back(stRow.strChargeSeq);
		vtChargeType.push_back(stRow.strChargeType);
		vtChargeUnit.push_back(stRow.strChargeUnit);
		vtLinkId.push_back(stRow.strLinkId);
		vtFromId.push_back(stRow.strFromId);
		vtToId.push_back(stRow.strToId);
		vtFromLat.push_back(stRow.strFromLat);
		vtFromLon.push_back(stRow.strFromLon);
		vtToLat.push_back(stRow.strToLat);
		vtToLon.push_back(stRow.strToLon);
		vtZoneId.push_back(stRow.strZoneId);
		vtZoneName.push_back(stRow.strZoneName);
		vtDistM.push_back(stRow.strDistM);
		vtSpeedKmh.push_back(strSpeedFixed);				// 원본 아닌 보정값 (2026-09-15 최정우 수정)
		vtSpeedLimitKmh.push_back(strSpeedLimitFixed);		// 원본 아닌 보정값 (2026-09-15 최정우 수정)
		vtOccurDt.push_back(strOccurFixed);					// 원본 아닌 보정값 (2026-09-17 최정우 추가)
		vtTripStartDt.push_back(strTripStartFixed);
		vtTollgateId.push_back(stRow.strTollgateId);
		vtEntryTollgateId.push_back(stRow.strEntryTollgateId);
		vtExitTollgateId.push_back(stRow.strExitTollgateId);
		vtRegDt.push_back(strRegFixed);						// 원본 아닌 보정값 (2026-09-17 최정우 추가)
		vtUpdDt.push_back(strUpdFixed);
		vtChargeYn.push_back(stRow.strChargeYn);
		vtChargeStatus.push_back(stRow.strChargeStatus);
		vtStaySeconds.push_back(strStayFixed);			// 원본 아닌 보정값 (2026-09-15 최정우 수정)
		vtTripEndDt.push_back(stRow.strTripEndDt);
		vtStartGpsSeq.push_back(stRow.strStartGpsSeq);
		vtEndGpsSeq.push_back(stRow.strEndGpsSeq);
		vtNonChargeReason.push_back(stRow.strNonChargeReason);
	}

	// 파라미터 순서(query.sql [charge_insert] UNNEST 컬럼 순서와 반드시 일치)
	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strDeviceKeyArray = BuildPgTextArray(vtDeviceKey);
	string strChargeSeqArray = BuildPgTextArray(vtChargeSeq);
	string strChargeTypeArray = BuildPgTextArray(vtChargeType);
	string strChargeUnitArray = BuildPgTextArray(vtChargeUnit);
	string strLinkIdArray = BuildPgTextArray(vtLinkId);
	string strFromIdArray = BuildPgTextArray(vtFromId);
	string strToIdArray = BuildPgTextArray(vtToId);
	string strFromLatArray = BuildPgTextArray(vtFromLat);
	string strFromLonArray = BuildPgTextArray(vtFromLon);
	string strToLatArray = BuildPgTextArray(vtToLat);
	string strToLonArray = BuildPgTextArray(vtToLon);
	string strZoneIdArray = BuildPgTextArray(vtZoneId);
	string strZoneNameArray = BuildPgTextArray(vtZoneName);
	string strDistMArray = BuildPgTextArray(vtDistM);
	string strSpeedKmhArray = BuildPgTextArray(vtSpeedKmh);
	string strSpeedLimitKmhArray = BuildPgTextArray(vtSpeedLimitKmh);
	string strOccurDtArray = BuildPgTextArray(vtOccurDt);
	string strTripStartDtArray = BuildPgTextArray(vtTripStartDt);
	string strTollgateIdArray = BuildPgTextArray(vtTollgateId);
	string strEntryTollgateIdArray = BuildPgTextArray(vtEntryTollgateId);
	string strExitTollgateIdArray = BuildPgTextArray(vtExitTollgateId);
	string strRegDtArray = BuildPgTextArray(vtRegDt);
	string strUpdDtArray = BuildPgTextArray(vtUpdDt);
	string strChargeYnArray = BuildPgTextArray(vtChargeYn);
	string strChargeStatusArray = BuildPgTextArray(vtChargeStatus);
	string strStaySecondsArray = BuildPgTextArray(vtStaySeconds);				// (2026-08-13 최정우 추가)
	string strTripEndDtArray = BuildPgTextArray(vtTripEndDt);					// (2026-08-13 최정우 추가)
	string strStartGpsSeqArray = BuildPgTextArray(vtStartGpsSeq);				// (2026-08-28 최정우 추가)
	string strEndGpsSeqArray = BuildPgTextArray(vtEndGpsSeq);					// (2026-08-28 최정우 추가)
	string strNonChargeReasonArray = BuildPgTextArray(vtNonChargeReason);		// (2026-09-01 최정우 추가)

	const char *pszParams[31] =
	{
		strTripIdArray.c_str(), strDeviceKeyArray.c_str(), strChargeSeqArray.c_str(),
		strChargeTypeArray.c_str(), strChargeUnitArray.c_str(), strLinkIdArray.c_str(),
		strFromIdArray.c_str(), strToIdArray.c_str(), strFromLatArray.c_str(),
		strFromLonArray.c_str(), strToLatArray.c_str(), strToLonArray.c_str(),
		strZoneIdArray.c_str(), strZoneNameArray.c_str(), strDistMArray.c_str(),
		strSpeedKmhArray.c_str(), strSpeedLimitKmhArray.c_str(), strOccurDtArray.c_str(),
		strTripStartDtArray.c_str(), strTollgateIdArray.c_str(), strEntryTollgateIdArray.c_str(),
		strExitTollgateIdArray.c_str(), strRegDtArray.c_str(), strUpdDtArray.c_str(),
		strChargeYnArray.c_str(), strChargeStatusArray.c_str(), strStaySecondsArray.c_str(),
		strTripEndDtArray.c_str(), strStartGpsSeqArray.c_str(), strEndGpsSeqArray.c_str(),
		strNonChargeReasonArray.c_str()
	};

	const int nParamLengths[31] =
	{
		static_cast<int>(strTripIdArray.size()), static_cast<int>(strDeviceKeyArray.size()),
		static_cast<int>(strChargeSeqArray.size()), static_cast<int>(strChargeTypeArray.size()),
		static_cast<int>(strChargeUnitArray.size()), static_cast<int>(strLinkIdArray.size()),
		static_cast<int>(strFromIdArray.size()), static_cast<int>(strToIdArray.size()),
		static_cast<int>(strFromLatArray.size()), static_cast<int>(strFromLonArray.size()),
		static_cast<int>(strToLatArray.size()), static_cast<int>(strToLonArray.size()),
		static_cast<int>(strZoneIdArray.size()), static_cast<int>(strZoneNameArray.size()),
		static_cast<int>(strDistMArray.size()), static_cast<int>(strSpeedKmhArray.size()),
		static_cast<int>(strSpeedLimitKmhArray.size()), static_cast<int>(strOccurDtArray.size()),
		static_cast<int>(strTripStartDtArray.size()), static_cast<int>(strTollgateIdArray.size()),
		static_cast<int>(strEntryTollgateIdArray.size()), static_cast<int>(strExitTollgateIdArray.size()),
		static_cast<int>(strRegDtArray.size()), static_cast<int>(strUpdDtArray.size()),
		static_cast<int>(strChargeYnArray.size()), static_cast<int>(strChargeStatusArray.size()),
		static_cast<int>(strStaySecondsArray.size()), static_cast<int>(strTripEndDtArray.size()),
		static_cast<int>(strStartGpsSeqArray.size()), static_cast<int>(strEndGpsSeqArray.size()),
		static_cast<int>(strNonChargeReasonArray.size())
	};
	const int nParamFormats[31] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strChargeInsertSQL.c_str(),
		31, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	bool bOk = (nExecStatus == PGRES_COMMAND_OK) || (nExecStatus == PGRES_TUPLES_OK);
	if (!bOk)
	{
		LOGFMTE("worker charge bulk insert error! count=[%d] msg=[%s]",
			static_cast<int>(vtCharges.size()), PQresultErrorMessage(pcResult));
	}
	else
	{
		// [CHARGE_SEQ_CONFLICT] 2026-10-04 최정우 추가, 사용자 확정 — 실제로 들어간 행 수가 요청보다 적으면
		//   (trip_id, device_key, trip_seq) PK 충돌로 ON CONFLICT DO NOTHING 이 행을 버린 것이다. 종전에는
		//   아무 로그도 없어 과금 행이 조용히 사라졌다(재현 980015 seq26~35). trip_seq 이어 매기기
		//   (QueryTripSeqMax) 이후에는 정상 흐름에서 생기지 않아야 하므로 ERROR 로 남긴다. 배치 실패로는
		//   보지 않는다 — 배치 재시도로 같은 행을 다시 넣는 정상 중복도 같은 결과라 구분할 수 없다.
		//   로그 코드 CHARGE_SEQ_CONFLICT 로 검색할 것. 버려진 행의 trip_id·trip_seq 범위를 함께 남긴다.
		const int nInserted = GetPgCmdTuples(pcResult);
		if (nInserted < static_cast<int>(vtCharges.size()))
		{
			LOGFMTE("[CHARGE_SEQ_CONFLICT] charge insert conflicted!requested=[%d] inserted=[%d] "
				"first_trip_id=[%s] trip_seq=[%s~%s]",
				static_cast<int>(vtCharges.size()), nInserted, vtCharges.front().strTripId.c_str(),
				vtCharges.front().strChargeSeq.c_str(), vtCharges.back().strChargeSeq.c_str());
		}
	}

	PQclear(pcResult);
	return bOk;
}

/**
 * @brief 그 트립에 이미 등록된 TRIP_SEQ 최댓값 조회 [trip_seqmax] — 읽기 전용
 *   (2026-10-04 최정우 추가, 사용자 확정 — TTL 마감 권장안 (다)와 함께 적용하는 "trip_seq 이어 매기기")
 * @param[in] pcConn DB 커넥션
 * @param[in] strTripId / strDeviceKey 대상 트립
 * @return 최댓값(없으면 0), 조회 실패·SQL 미설정이면 -1
 * @remark 새 세션(엔진 재기동·TTL 마감 뒤 같은 trip_id 가 이어짐)이 TRIP_SEQ 를 1 부터 다시 매기면 이미
 *   등록된 번호와 PK 가 충돌해 새 행이 ON CONFLICT DO NOTHING 으로 **조용히 버려졌다**(재현: 980015
 *   seq26~35 일반도로 212m 미적재). run() 이 배치 시작 때 "세션의 트립 != 배치의 트립"이면 이 값을 읽어
 *   두고, ProcessRawLog() 가 그 트립을 세션에 세팅하는 순간 nEmitSeq 를 이 값으로 맞춘다 — 번호 부여는
 *   여전히 엔진(ReleaseChargeQueue)만 하고, SQL 은 기존 최댓값을 읽기만 한다(TRIP_SEQ 를 바꾸지 않음).
*/
int CRawLogWorker::QueryTripSeqMax(PGconn *pcConn, const string& strTripId, const string& strDeviceKey)
{
	if ((pcConn == nullptr) || strTripId.empty() || m_stConfig.strTripSeqMaxSQL.empty())
		return -1;
	const char *pszParams[2] = { strTripId.c_str(), strDeviceKey.c_str() };
	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strTripSeqMaxSQL.c_str(),
		2, nullptr, pszParams, nullptr, nullptr, 0);
	if (pcResult == nullptr)
		return -1;
	int nMax = -1;
	if ((PQresultStatus(pcResult) == PGRES_TUPLES_OK) && (PQntuples(pcResult) > 0))
		nMax = atoi(PQgetvalue(pcResult, 0, 0));
	else
		LOGFMTE("worker trip_seqmax query error! trip_id=[%s] msg=[%s]",
			strTripId.c_str(), PQresultErrorMessage(pcResult));
	PQclear(pcResult);
	return nMax;
}

/**
 * @brief 트립 종료 시 trip_end_dt bulk UPDATE [trip_end] (2026-08-12 최정우 추가)
 * @param[in] pcConn DB 커넥션
 * @param[in] vtRows UPDATE 대상 행 목록(ProcessRawLog 가 TRIP_EVENT=2 감지 시 적재)
 * @return true(성공), false(실행 오류·인자 무효)
 * @remark WHERE TRIP_ID 매칭 — 해당 trip_id 로 적재된 PRIM_CHARGEHAND 행이 없으면 0건
 *   영향으로 조용히 끝남(과금 없는 트립도 정상 케이스라 오류 아님)
*/
bool CRawLogWorker::UpdateTripEndDt(PGconn *pcConn, const vector<TRIP_END_UPDATE_ROW>& vtRows)
{
	if ((pcConn == nullptr) || vtRows.empty() || m_stConfig.strTripEndUpdateSQL.empty())
		return false;

	vector<string> vtTripId, vtTripEndDt, vtUpdDt;
	for (size_t i=0; i<vtRows.size(); ++i)
	{
		vtTripId.push_back(vtRows[i].strTripId);
		vtTripEndDt.push_back(vtRows[i].strTripEndDt);
		vtUpdDt.push_back(vtRows[i].strUpdDt);
	}

	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strTripEndDtArray = BuildPgTextArray(vtTripEndDt);
	string strUpdDtArray = BuildPgTextArray(vtUpdDt);

	const char *pszParams[3] = { strTripIdArray.c_str(), strTripEndDtArray.c_str(), strUpdDtArray.c_str() };
	const int nParamLengths[3] =
	{
		static_cast<int>(strTripIdArray.size()),
		static_cast<int>(strTripEndDtArray.size()),
		static_cast<int>(strUpdDtArray.size())
	};
	const int nParamFormats[3] = { 0, 0, 0 };

	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strTripEndUpdateSQL.c_str(),
		3, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	bool bOk = (nExecStatus == PGRES_COMMAND_OK) || (nExecStatus == PGRES_TUPLES_OK);
	if (!bOk)
	{
		LOGFMTE("worker trip_end update error! count=[%d] msg=[%s]",
			static_cast<int>(vtRows.size()), PQresultErrorMessage(pcResult));
	}

	PQclear(pcResult);
	return bOk;
}

// CRawLogWorker::UpdateTripSeqOrder() 삭제 — [trip_seqoff]/[trip_seqfin] 로 TRIP_SEQ 를 사후 재부여하던 함수.
//   2026-09-22 부터 비활성이었고, TRIP_SEQ 는 ReleaseChargeQueue() 가 방출할 때 한 번 정해 INSERT 하면
//   이후 바뀌지 않는다 (2026-10-04 최정우 삭제 — 사용자 지시: trip_seq 는 SQL 로 처리하지 않고 엔진이 확정할 때 등록한다)

/**
 * @brief 지금 시점에 "GPS_SEQ 순서가 확정된" 상한을 구한다 (과금 행 워터마크)
 *   (2026-09-22 최정우 추가 — 사용자 확정 요구)
 * @param[in] stSession 대상 세션
 * @param[in] dwCurGpsSeq 이번 배치에서 마지막으로 처리한 GPS_SEQ
 * @return 확정 상한(exclusive) — start_gps_seq 가 이 값보다 **작은** 행은 더 생길 수 없다
 * @remark 반환값이 곧 "여기부터 앞쪽은 아직 행이 더 생길 수 있다"는 경계다. 아래 5종 중
 *   하나라도 빠뜨리면 이미 번호를 준 뒤에 그보다 앞선 행이 생겨 순서가 깨진다 — 새로운
 *   보류·이월 상태를 추가할 때는 **여기에도 반드시 같이** 넣을 것.
 *   현재 tick 자체를 상한으로 두는 이유: 경로 복구(EmitForeignSpan)가 만드는 행은
 *   **직전 tick 자리**에 뒤늦게 생긴다(실측 4건 전부 start_gps_seq=end_gps_seq=직전 tick).
 *   따라서 이번 tick 을 다 처리하기 전에는 직전 tick 도 확정할 수 없다.
*/
uint32 CRawLogWorker::CalcChargeWatermark(const VEHICLE_TRIP_SESSION& stSession, uint32 dwCurGpsSeq)
{
	uint32 dwMark = dwCurGpsSeq;
	// 0 은 "값 없음"이므로 후보에서 제외한다(미설정 필드가 상한을 0 으로 끌어내리면 영구 보류)
	auto fnMin = [&dwMark](uint32 dwSeq)
	{
		if ((dwSeq > 0) && (dwSeq < dwMark)) dwMark = dwSeq;
	};

	// ① 아직 열려 있는 구역 run — 닫히는 순간 그 **진입 자리**에 행이 생긴다
	for (size_t i = 0; i < stSession.vtParkRuns.size(); ++i)
		fnMin(stSession.vtParkRuns[i].dwEntryGpsSeq);
	for (size_t i = 0; i < stSession.vtOpenRuns.size(); ++i)
		fnMin(stSession.vtOpenRuns[i].dwEntryGpsSeq);
	for (size_t i = 0; i < stSession.vtExemptRuns.size(); ++i)
		fnMin(stSession.vtExemptRuns[i].dwEntryGpsSeq);
	for (size_t i = 0; i < stSession.vtNodeStepRuns.size(); ++i)
		fnMin(stSession.vtNodeStepRuns[i].dwEntryGpsSeq);
	if (stSession.bInClosedRoad)
		fnMin(stSession.dwEntryGpsSeq);
	if (stSession.bInSpeedZone)
		fnMin(stSession.dwSpeedEntryGpsSeq);

	// ② 이월(carry) — 다음 tick 의 경로 복구가 **과거 자리**에 행을 만든다.
	//   게이트 진출 이월은 개방형·폐쇄형·면제·구간단속이 bHasGateExitCarry/dwGateExitGpsSeq 를
	//   공용으로 쓴다(구간단속은 2026-09-23 합류 — ProcessSpeedZoneCharge 헤더 주석 참고).
	//   이 이월이 폐기될 때 그 자리에 독립 일반도로 행(orphan span)이 생길 수 있으므로,
	//   진출 순번을 상한으로 잡아 그보다 앞선 행이 먼저 번호를 가져가지 않게 한다.
	if (stSession.bHasGateExitCarry)
		fnMin(stSession.dwGateExitGpsSeq);
	if (stSession.bHasMergeCarry)
		fnMin(stSession.stMergeCarry.dwEntryGpsSeq);
	if (stSession.bHasParkTouchCarry)
		fnMin(stSession.stParkTouchCarry.dwEntryGpsSeq);

	// ③ 보류(held) — 인수인계 구간을 만날 때까지 들고 있다가 **원래 진입 자리**로 등록된다
	if (stSession.bHasHeldNodeStepRun)
		fnMin(stSession.stHeldNodeStepRun.dwEntryGpsSeq);
	if (stSession.bHasHeldSpeedMirrorRun)
		fnMin(stSession.stHeldSpeedMirrorRun.dwEntryGpsSeq);

	// ④ 1틱 지연커밋 보류행 — 확정되어야 그 tick 의 과금 판정이 비로소 돈다
	if (stSession.bHasPendingCommit)
		fnMin(stSession.stPendingRawLogInfo.dwSeqNo);

	// ⑤ [2026-09-22 최정우 추가 — 사용자 지시] **연속 일반도로 병합 기회 보존.**
	//   일반도로 run 이 열려 있으면, 큐에 남아 있는 **마지막 일반도로 행**부터 보류한다.
	//   그 행은 지금 열려 있는 run 이 닫히면 MergeAdjacentNodeStepRows() 의 인접 판정
	//   (nCurStart <= nPrevEnd + 2)에 걸려 하나로 합쳐질 수 있는데, 먼저 방출해 버리면 큐에서
	//   사라져 그 기회를 잃는다 — 실측 000376_20260826160622 이 정확히 그 경우다(1~103 과
	//   104~242 가 병합 조건을 모두 만족하는데도 배치가 갈려 두 행으로 남았다).
	//   그 행의 start 를 상한으로 쓰면 **그보다 앞선 행은 그대로 방출**되고(불필요한 지연 없음),
	//   그 행과 그 이후 행만 보류된다. 후자가 중요한 이유는 병합의 감싸기 금지 판정
	//   (bWouldWrapOtherType)이 **같은 벡터 안의 타 유형 행만** 보기 때문이다 — 그 구간의 타 유형
	//   행이 먼저 빠져나가면 검사가 헐거워진다.
	//   트립종료·TTL·트립전환은 dwWatermark=UINT32_MAX 로 전량 방출하므로 유실되지 않는다.
	//   [2026-10-04 최정우 추가, 사용자 확정] 구간단속 run 이 열려 있거나(통과 중) 미러가 보류 중일 때도
	//   같은 보류를 건다 — 그 사이 일반도로 run 은 닫혀 있어(구간단속 링크는 일반도로에서 빠짐) 진입 앞
	//   일반도로 행이 먼저 방출돼 미러와 병합할 기회를 잃었다(980015 2~6 이 단독 INSERT). 대가로 앞 행의
	//   INSERT 가 진출 뒤 일반도로가 끝날 때까지 늦어진다(구간단속 행은 원래 그만큼 대기 중이었다).
	if (!stSession.vtNodeStepRuns.empty() || stSession.bInSpeedZone || stSession.bHasHeldSpeedMirrorRun)
	{
		uint32 dwLastNodeStepStart = 0;
		for (size_t i = 0; i < stSession.vtPendingEmit.size(); ++i)
		{
			if (stSession.vtPendingEmit[i].strChargeType != "0") continue;
			const uint32 dwStart = static_cast<uint32>(
				strtoul(stSession.vtPendingEmit[i].strStartGpsSeq.c_str(), nullptr, 10));
			if (dwStart > dwLastNodeStepStart) dwLastNodeStepStart = dwStart;
		}
		fnMin(dwLastNodeStepStart);
	}

	// ※ ZONE_RUN_SESSION.qwPendingEntryFromLinkID(진입 링크 확정 보류)는 그 run 이 열려 있는
	//   동안만 유효하므로 ① 에서 이미 커버된다 — 별도 항이 필요 없다.
	return dwMark;
}

/**
 * @brief 이번 배치가 만든 과금 행을 세션의 방출 대기 큐로 옮긴다
 *   (2026-09-22 최정우 추가)
 * @param[in,out] pstSession 대상 세션 — vtPendingEmit 에 append
 * @param[in,out] pvtRows 배치가 만든 행 목록. 큐로 옮긴 행은 여기서 제거된다
 * @remark bEmitted=true 인 행(배치 도중 트립 전환 등으로 **이미 방출된** 행)은 큐로 돌리지
 *   않고 그대로 남긴다 — 그러지 않으면 같은 행이 두 번 번호를 받는다.
*/
void CRawLogWorker::EnqueueChargeRows(VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtRows)
{
	if ((pstSession == nullptr) || (pvtRows == nullptr)) return;

	vector<CHARGE_INSERT_ROW> vtKeep;
	for (size_t i = 0; i < pvtRows->size(); ++i)
	{
		if ((*pvtRows)[i].bEmitted)
			vtKeep.push_back((*pvtRows)[i]);
		else
			pstSession->vtPendingEmit.push_back((*pvtRows)[i]);
	}
	pvtRows->swap(vtKeep);
}

/**
 * @brief 큐에서 순서가 확정된 행을 꺼내 TRIP_SEQ 를 부여한다 (2026-09-22 최정우 추가)
 * @param[in] nThreadId 로그용 워커 번호
 * @param[in,out] pstSession 대상 세션 — vtPendingEmit / nEmitSeq / vtCoveredAll 갱신
 * @param[in] dwWatermark CalcChargeWatermark() 결과. UINT32_MAX 면 전량 방출(트립 마감)
 * @param[out] pvtOut 방출된 행이 append 된다(INSERT 대상)
 * @remark [수행 순서] 아래 차례가 중요하다 — 바꾸면 결과가 달라진다.
 *   ① (전량 방출일 때만) FillUncoveredLinkRows() — 어느 행에도 안 덮인 구간을 복구해 큐에 넣는다.
 *      **병합보다 앞**에 둬야 복구분이 인접 행과 자연히 합쳐진다.
 *   ② MergeAdjacentNodeStepRows() — 연속 일반도로 행 병합. 배치가 아니라 **큐**를 대상으로 돌려
 *      poll 주기 경계로 병합을 놓치던 문제를 없앴다.
 *   ③ MergeGapRecoveredOpenRows() — 개방형 경로복구 조각을 정규 마감 행에 흡수.
 *   ④ 정렬 → 방출 → (전량 방출일 때만) LogTripLinkCoverage() 로 남은 미덮임 계측.
 * @remark 정렬은 start_gps_seq 오름차순, **동점은 큐 삽입 순**(= 경로 진행 순)이라 stable_sort 를
 *   쓴다. 실측으로 같은 GPS_SEQ 에 2~3행이 몰린다(경로 복구 행은 진입·진출이 같은 tick 이라 start=end).
 *   번호는 **여기서 처음** 부여하므로, 조건 미달로 등록되지 않은 행은 번호를 먹지 않는다
 *   → 결번이 생기지 않는다.
 * @remark 방출할 때 일반도로 행의 dist_m·덮은 링크·최대 START_GPS_SEQ 를 세션에 누적한다
 *   (dfChargedNodeStepM / vtCoveredAll / dwLastEmittedStartSeq). 앞의 둘은 커버리지 대조용,
 *   마지막은 복구 행이 **이미 나간 행보다 앞 위치**에 생기는 것을 막는 순서 가드용이다.
*/
void CRawLogWorker::ReleaseChargeQueue(int nThreadId, VEHICLE_TRIP_SESSION *pstSession, uint32 dwWatermark,
	vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if ((pstSession == nullptr) || (pvtOut == nullptr) || pstSession->vtPendingEmit.empty())
		return;

	// [2026-09-22 최정우 추가 — 사용자 지시, 커버리지 2단계] 전량 방출(트립 마감)이면, 실제 지나간
	//   링크 중 **어느 행에도 안 덮인 구간**을 일반도로 행으로 복구해 큐에 먼저 넣는다.
	//   아래 병합보다 앞에 두어, 복구된 행이 인접 일반도로 행과 자연히 합쳐지게 한다.
	if (dwWatermark == UINT32_MAX)
		FillUncoveredLinkRows(nThreadId, pstSession);

	// [2026-09-22 최정우 추가 — 사용자 지시] **배치 경계를 넘는 연속 일반도로 병합.**
	//   판정 규칙(bContiguous / bNoMergeAfter / bWouldWrapOtherType)은 한 글자도 바꾸지 않고,
	//   **대상 벡터만 배치 → 큐로** 넓힌다. 종전에는 run() 의 배치 벡터에서만 돌아서, 앞뒤 행이
	//   다른 배치로 갈리면(poll 주기 경계) 병합 조건을 전부 만족해도 두 행으로 남았다.
	//   큐는 배치를 넘어 유지되므로 그 제약이 사라진다.
	//   ※ DropNodeStepRowsWithoutMatch() 는 그 배치의 vtUpdates 가 있어야 하므로 여기서 부르지
	//     않는다 — 큐에 들어온 행은 이미 run() 에서 그 검사를 통과한 것이라 순서 전제도 지켜진다.
	MergeAdjacentNodeStepRows(&pstSession->vtPendingEmit);

	// [2026-09-22 최정우 추가 — 사용자 지시] 개방형 경로복구 행 흡수도 같은 이유로 큐에서 한 번 더
	//   돌린다. ProcessOpenGateCharge() 의 인라인 흡수는 배치 벡터만 보므로, 복구 행과 정규 마감
	//   행이 다른 배치로 갈리면 9m 조각이 그대로 남았다(실측 000994_20250903152350 trip_seq=14,
	//   RL-Z00004 8.8m 브리지 링크). 판정 규칙은 그대로 옮겼다.
	MergeGapRecoveredOpenRows(&pstSession->vtPendingEmit);

	// [2026-09-22 최정우 추가 — 사용자 지시] 과금 대상 일반도로 구간에 **완전히 포함**되는
	//   N/3 일반도로 행(SKIP 구간 브릿지)을 제거한다. 병합 뒤에 둬야 병합으로 넓어진 정규 구간까지
	//   기준으로 삼는다. 번호 부여(아래 방출 루프)보다 앞이라 결번도 생기지 않는다.
	DropContainedAuditNodeStepRows(nThreadId, pstSession);

	stable_sort(pstSession->vtPendingEmit.begin(), pstSession->vtPendingEmit.end(),
		[](const CHARGE_INSERT_ROW& stA, const CHARGE_INSERT_ROW& stB) -> bool
		{
			return strtoul(stA.strStartGpsSeq.c_str(), nullptr, 10)
				 < strtoul(stB.strStartGpsSeq.c_str(), nullptr, 10);
		});

	size_t nCut = 0;
	while (nCut < pstSession->vtPendingEmit.size())
	{
		const uint32 dwStart = static_cast<uint32>(
			strtoul(pstSession->vtPendingEmit[nCut].strStartGpsSeq.c_str(), nullptr, 10));
		// 정렬돼 있으므로 하나라도 조건을 못 넘기면 그 뒤는 볼 필요가 없다
		if ((dwWatermark != UINT32_MAX) && (dwStart >= dwWatermark))
			break;

		CHARGE_INSERT_ROW stRow = pstSession->vtPendingEmit[nCut];
		char szSeq[16];
		snprintf(szSeq, sizeof(szSeq), "%d", ++(pstSession->nEmitSeq));
		stRow.strChargeSeq = szSeq;
		stRow.bEmitted = true;
		// [2026-10-04 최정우 수정, 사용자 확정 규칙 "trip_seq 확정·DB 적재 시점에 reg_dt = upd_dt"] REG_DT/UPD_DT 를
		//   **번호를 붙이는 지금(INSERT 직전)** 으로 다시 찍는다. 종전에는 행을 만든 시각(Build* 13곳)이 그대로
		//   남아, 워터마크 큐에 보류됐다 나중에 INSERT 된 행의 REG_DT 가 실제 적재보다 앞섰다 — 과금서버가
		//   REG_DT 를 기준으로 새 행을 가져가면 놓칠 수 있다. 실측 980015 trip_seq 2·3: 생성 13:13:34, INSERT
		//   13:15:34(TTL 마감)인데 REG_DT 13:13:34. 구간단속 앞뒤 일반도로 병합(같은 날)으로 보류가 길어져 드러남.
		// 되돌리는 법: 아래 두 줄을 지운다.
		stRow.strRegDt = FormatDateTime14(time(nullptr));
		stRow.strUpdDt = stRow.strRegDt;
		// [2026-09-22 최정우 추가 — 계측 전용] 일반도로로 실제 청구한 거리를 누적한다.
		//   방출 시점에 한 곳에서 세므로 행 생성 경로가 몇 개든 빠짐없이 잡힌다.
		{
			const uint32 dwRowStart = static_cast<uint32>(strtoul(stRow.strStartGpsSeq.c_str(), nullptr, 10));
			if (dwRowStart > pstSession->dwLastEmittedStartSeq)
				pstSession->dwLastEmittedStartSeq = dwRowStart;
		}
		if (stRow.strChargeType == "0")
		{
			pstSession->dfChargedNodeStepM += atof(stRow.strDistM.c_str());
			for (size_t ci = 0; ci < stRow.vtCoveredLinks.size(); ++ci)
				pstSession->vtCoveredAll.push_back(stRow.vtCoveredLinks[ci]);
			if (stRow.strChargeYn == "Y")
			{
				const uint32 dwS = static_cast<uint32>(strtoul(stRow.strStartGpsSeq.c_str(), nullptr, 10));
				const uint32 dwE = static_cast<uint32>(strtoul(stRow.strEndGpsSeq.c_str(), nullptr, 10));
				if ((dwS > 0) && (dwE >= dwS))
					pstSession->vtEmittedNodeStepRanges.push_back(make_pair(dwS, dwE));
			}
		}
		pvtOut->push_back(stRow);
		++nCut;
	}

	if (nCut > 0)
	{
		pstSession->vtPendingEmit.erase(pstSession->vtPendingEmit.begin(),
			pstSession->vtPendingEmit.begin() + nCut);
	}

	// [2026-09-22 최정우 추가 — 계측 전용] 전량 방출(트립종료·전환·TTL·서버종료)이면 그 트립의
	//   링크 커버리지를 남긴다. 이 시점이 "그 트립의 모든 행이 확정된" 유일한 지점이다.
	if ((dwWatermark == UINT32_MAX) && !pstSession->vtTripPathLinks.empty())
		LogTripLinkCoverage(nThreadId, *pstSession);
}

/**
 * @brief 개방형 경로복구 행을 같은 구역 정규 마감 행에 흡수 (2026-09-22 최정우 추가)
 * @param[in,out] pvtCharges 대상 행 목록 — 흡수된 복구 행은 제거된다
 * @remark ProcessOpenGateCharge() 안에 있던 흡수 판정을 그대로 떼어낸 독립 패스다.
 *   종전 인라인 판정은 "정규 행을 만드는 그 순간, 배치 벡터 끝에서 역순으로 직전 개방형 행을
 *   찾아 같은 구역 복구 행이면 흡수" 였다 — 그래서 **같은 배치 안에 둘 다 있을 때만** 동작했고,
 *   배치가 갈리면 9m 짜리 N/3 조각이 심사 큐까지 만들며 남았다.
 *   여기서는 벡터 전체를 훑어 같은 트립·기기·구역(from_id)의 정규 행(NCR 12 아님)과 복구 행
 *   (NCR 12)을 gps_seq 인접성으로 짝지어 흡수한다 — 한 트립에서 같은 구역을 여러 번 통과해도
 *   (실측 000994 는 RL-Z00004 를 4회 통과) 엉뚱한 짝이 생기지 않는다.
 *   **거리는 더하지 않는다.** 개방형 DIST_M 은 실측 누적이 아니라 구역 전체 길이 고정값이므로,
 *   복구된 링크는 이미 그 값 안에 들어 있다(2026-09-16 실측 15건으로 확인된 함정).
 *   합치는 것은 거리가 아니라 **구간 범위(START/END_GPS_SEQ)** 다.
*/
void CRawLogWorker::MergeGapRecoveredOpenRows(vector<CHARGE_INSERT_ROW> *pvtCharges)
{
	if ((pvtCharges == nullptr) || (pvtCharges->size() < 2))
		return;

	const string strRecoverReason = to_string(static_cast<int>(NCR_OPEN_GATE_NOT_ON_PATH));
	vector<bool> vtDropped(pvtCharges->size(), false);
	int nMerged = 0;

	for (size_t i = 0; i < pvtCharges->size(); ++i)
	{
		CHARGE_INSERT_ROW& stMain = (*pvtCharges)[i];
		if (vtDropped[i]) continue;
		if (stMain.strChargeType != "1") continue;					// 개방형만 대상
		if (stMain.strNonChargeReason == strRecoverReason) continue;	// 정규 마감 행만 흡수 주체

		const long nMainStart = atol(stMain.strStartGpsSeq.c_str());
		const long nMainEnd   = atol(stMain.strEndGpsSeq.c_str());
		if ((nMainStart <= 0) || (nMainEnd <= 0)) continue;

		for (size_t j = 0; j < pvtCharges->size(); ++j)
		{
			if ((i == j) || vtDropped[j]) continue;
			const CHARGE_INSERT_ROW& stFrag = (*pvtCharges)[j];
			if (stFrag.strChargeType != "1") continue;
			if (stFrag.strNonChargeReason != strRecoverReason) continue;	// 복구 행만 흡수 대상
			if (stFrag.strTripId != stMain.strTripId) continue;
			if (stFrag.strDeviceKey != stMain.strDeviceKey) continue;
			if (stFrag.strFromId != stMain.strFromId) continue;			// 같은 구역만 — 다른 구역이면 둘 다 남긴다

			const long nFragStart = atol(stFrag.strStartGpsSeq.c_str());
			const long nFragEnd   = atol(stFrag.strEndGpsSeq.c_str());
			if ((nFragStart <= 0) || (nFragEnd <= 0)) continue;

			// 인접성 — 같은 구역을 여러 번 통과하는 트립에서 엉뚱한 통과분끼리 붙지 않게 한다.
			//   복구 행은 정규 구간의 끝(또는 시작) 링크가 tick 없이 지나간 조각이라 범위가 겹치거나
			//   한 tick 안에서 맞닿는다(실측: 정규 130~133 / 복구 133~133).
			const bool bAdjacent = (nFragStart >= nMainStart - 1) && (nFragStart <= nMainEnd + 1);
			if (!bAdjacent) continue;

			// 구간 범위만 넓힌다 — 거리(DIST_M)는 더하지 않는다(위 @remark 참고)
			if (nFragStart < nMainStart)
				stMain.strStartGpsSeq = stFrag.strStartGpsSeq;
			if (nFragEnd > nMainEnd)
				stMain.strEndGpsSeq = stFrag.strEndGpsSeq;

			vtDropped[j] = true;
			++nMerged;
			break;													// 정규 행 하나당 복구 행 하나
		}
	}

	if (nMerged > 0)
	{
		vector<CHARGE_INSERT_ROW> vtKeep;
		for (size_t i = 0; i < pvtCharges->size(); ++i)
		{
			if (!vtDropped[i]) vtKeep.push_back((*pvtCharges)[i]);
		}
		pvtCharges->swap(vtKeep);
	}
}

/**
 * @brief 트립 경유 링크 대비 일반도로 청구 거리 커버리지를 로그로 남긴다 (2026-09-22 최정우 추가)
 * @param[in] nThreadId 로그용 워커 번호
 * @param[in] stSession 대상 세션(전량 방출 직후 — 그 트립의 모든 행이 확정된 시점)
 * @remark **이 함수 자체는 계측 전용이다 — 어떤 값도 바꾸지 않는다.** 복구는 같은 자리에서 먼저
 *   도는 FillUncoveredLinkRows() 가 담당하고, 이 함수는 **그러고도 남은** 미덮임을 기록한다.
 *   누락 링크 문제는 유형별 복구 경로가 제각각이라 전이 조합마다 구멍이 남는 구조에서 나온다.
 *   개별 구멍을 계속 막는 대신, "실제 지나간 링크 중 일반도로로 청구되지 않은 길이"를 트립 단위로
 *   재서 규모와 패턴을 먼저 본다. 이 수치로 2단계(커버리지 채우기) 적용 여부를 판단한다.
 *   비교 대상을 **일반도로(type 0)로 한정**하는 이유: 개방형 DIST_M 은 구역 전체 길이 고정값,
 *   면제는 구역 내부만 누적이라 링크 길이 합과 1:1로 대응하지 않는다. 미등록(일반도로) 링크만이
 *   "지나간 만큼 청구" 가 성립한다.
 *   gap > 0 이면 청구 누락, gap < 0 이면 과다 청구 의심이다.
*/
void CRawLogWorker::LogTripLinkCoverage(int nThreadId, const VEHICLE_TRIP_SESSION& stSession)
{
	if ((m_stConfig.pcDataLoader == nullptr) || (m_stConfig.pcChargeDataLoader == nullptr))
		return;

	// 덮인 링크 집합 — 행들이 자기가 덮은 링크를 직접 들고 오므로 추정이 필요 없다
	set<uint64> setCovered(stSession.vtCoveredAll.begin(), stSession.vtCoveredAll.end());

	int nUnreg = 0, nUncovered = 0;
	double dfUncoveredM = 0.0;
	string strSample;
	for (size_t i = 0; i < stSession.vtTripPathLinks.size(); ++i)
	{
		const uint64 qwLink = stSession.vtTripPathLinks[i].qwLinkID;
		// 타 과금유형에 등록된 링크는 그 유형 행이 담당하므로 대상이 아니다 —
		//   일반도로로 청구되어야 할 링크(미등록)만 본다
		if (!m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(qwLink)) continue;
		++nUnreg;
		if (setCovered.find(qwLink) != setCovered.end()) continue;
		PLINK_INFO pstLink = m_stConfig.pcDataLoader->GetLinkInfo(qwLink);
		if (pstLink == nullptr) continue;
		++nUncovered;
		dfUncoveredM += pstLink->dfLen;
		if (strSample.length() < 90)
		{
			char szOne[32];
			snprintf(szOne, sizeof(szOne), "%llu(%.0fm) ",
				static_cast<unsigned long long>(qwLink), pstLink->dfLen);
			strSample += szOne;
		}
	}

	LOGFMTI("[#%02d] trip link coverage!trip_id=[%s] path=[%d] unreg=[%d] uncovered=[%d] "
		"uncovered_m=[%.1f] charged_nodestep_m=[%.1f] links=[%s]",
		nThreadId, stSession.szTripId, static_cast<int>(stSession.vtTripPathLinks.size()),
		nUnreg, nUncovered, dfUncoveredM, stSession.dfChargedNodeStepM, strSample.c_str());
}

/**
 * @brief 어느 행에도 덮이지 않은 링크 구간을 일반도로 행으로 복구 (2026-09-22 최정우 추가)
 * @param[in] nThreadId 로그용 워커 번호
 * @param[in,out] pstSession 대상 세션 — 생성된 행을 vtPendingEmit 에 추가
 * @return 생성된 행 수
 * @remark GPS 3초 간격이면 tick 이 하나도 안 찍힌 링크가 생긴다. 그 복구를 유형별 전이 경로에서
 *   제각각 하다 보니 조합마다 구멍이 남았고(면제→타 유형, 구간단속·주정차 진출 후 …), 막을 때마다
 *   다음 조합에서 또 나왔다. 여기서는 **조합을 가리지 않고** "실제 지나간 링크 중 아무도 안 덮은 것"을
 *   찾아 채운다.
 *   [안전 규칙 — 넷 다 실측으로 필요성이 확인된 것이다]
 *   1. **미등록(일반도로) 링크만** 대상이다. 타 과금유형 등록 링크는 그 유형 행이 담당한다.
 *   2. **주정차 폴리곤 안의 링크는 제외한다**(IsLinkInsideParkingPolygon). 주정차는 폴리곤 기반이라
 *      IsCase3EligibleRoadKind() 가 그 안의 링크도 "미등록" 으로 돌려주는데, 그대로 믿고 복구하면
 *      **"매칭좌표가 폴리곤 안이면 속도 무관 주정차" 라는 확정 규칙을 우회**하게 된다.
 *      실측 000376_20260819094414 trip_seq=8(gps 99~115, 301m)이 그렇게 잘못 생겼다(사용자 지적).
 *      폴리곤 안을 짧게 통과해 최소체류 미달로 주정차가 미등록된 구간은 **청구 대상이 아니다.**
 *   3. **흡수 우선, 새 행은 최후수단.** 인접 일반도로 행이 있으면 그 행의 거리만 늘린다 —
 *      START/END_GPS_SEQ 를 넓히지 않으므로 구간중복이 생기지 않는다. 처음에 새 행만 만들었더니
 *      같은 유형 구간중복이 12→26 쌍으로 늘었다(실측).
 *   4. **이미 방출된 행보다 앞 위치에는 새 행을 만들지 않는다.** TRIP_SEQ 는 방출 순서로 매겨지므로
 *      과거 위치에 행을 더하면 gps_seq 순과 어긋난다(가드 없이 19행 불일치, 실측).
 *      그래서 일부 구간은 복구되지 않고 남는다 — 커버리지 로그의 잔여분이 이것이다.
 *   [적용 결과] 미덮임 링크 147개(13,667m) → 46개(8,301m), 일반도로 +4,419m. 과금 구역(1~5)의
 *     거리는 1m도 바뀌지 않았고 구간중복·trip_seq 순서도 그대로다.
*/
int CRawLogWorker::FillUncoveredLinkRows(int nThreadId, VEHICLE_TRIP_SESSION *pstSession)
{
	if ((pstSession == nullptr) || (m_stConfig.pcDataLoader == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || pstSession->vtTripPathLinks.empty())
		return 0;

	// 덮인 링크 = 이미 방출된 행 + 아직 큐에 있는 행
	set<uint64> setCovered(pstSession->vtCoveredAll.begin(), pstSession->vtCoveredAll.end());
	for (size_t i = 0; i < pstSession->vtPendingEmit.size(); ++i)
	{
		const CHARGE_INSERT_ROW& stQ = pstSession->vtPendingEmit[i];
		for (size_t ci = 0; ci < stQ.vtCoveredLinks.size(); ++ci)
			setCovered.insert(stQ.vtCoveredLinks[ci]);
	}

	int nMade = 0;
	size_t i = 0;
	const size_t nCnt = pstSession->vtTripPathLinks.size();
	// [버그 수정, 2026-10-04 최정우 — 사용자 지적] **트립 첫 tick 의 경로 링크는 복구하지 않는다.**
	//   차량은 그 링크의 어딘가(대개 중간)에서 출발했으므로 링크 전체 길이를 더할 근거가 없고, 첫 tick
	//   이 직전 트립 앵커에서 이어진 경로를 담으면 이 트립에서 달리지 않은 링크까지 들어 있다. 출발
	//   지점 이후 거리는 그 tick 에서 열린 행(일반도로 run·구간단속 미러 등)이 이미 센다.
	//   실측: 미커버 흡수 13건 중 10건(2,172m)이 트립 첫 tick — 000376_20260819141002 trip_seq=1 은
	//   12초에 573m(172km/h, 구간단속 링크 2040424301 전체 331.7m 포함), 시나리오
	//   980013_20261004230000 trip_seq=1 은 3 tick 에 179m(첫 점이 앞 링크 2040426501 125.5m 로 매칭).
	// 되돌리는 법: 아래 dwTripFirstSeq 조건을 지우면 종전 동작이다.
	const uint32 dwTripFirstSeq = pstSession->vtTripPathLinks[0].dwGpsSeq;
	while ((i < nCnt) && (pstSession->vtTripPathLinks[i].dwGpsSeq == dwTripFirstSeq))
		++i;
	while (i < nCnt)
	{
		const uint64 qwLink = pstSession->vtTripPathLinks[i].qwLinkID;
		const bool bTarget = m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(qwLink)
			&& (setCovered.find(qwLink) == setCovered.end())
			&& !(MM_PARK_SUPPRESS_NODESTEP && IsLinkInsideParkingPolygon(qwLink));
		if (!bTarget) { ++i; continue; }

		// 연속한 미덮임 구간을 한 행으로 묶는다
		const size_t nStart = i;
		double dfDistM = 0.0;
		vector<uint64> vtSpanLinks;
		while (i < nCnt)
		{
			const uint64 qwCur = pstSession->vtTripPathLinks[i].qwLinkID;
			if (!m_stConfig.pcChargeDataLoader->IsCase3EligibleRoadKind(qwCur)) break;
			if (setCovered.find(qwCur) != setCovered.end()) break;
			if (MM_PARK_SUPPRESS_NODESTEP && IsLinkInsideParkingPolygon(qwCur)) break;
			PLINK_INFO pstLink = m_stConfig.pcDataLoader->GetLinkInfo(qwCur);
			if (pstLink == nullptr) break;
			dfDistM += pstLink->dfLen;
			vtSpanLinks.push_back(qwCur);
			setCovered.insert(qwCur);						// 같은 링크를 두 번 채우지 않는다
			++i;
		}
		if (vtSpanLinks.empty() || (dfDistM <= 0.0)) continue;

		const size_t nEnd = (i > nStart) ? (i - 1) : nStart;
		const uint32 dwStartSeq = pstSession->vtTripPathLinks[nStart].dwGpsSeq;
		const uint32 dwEndSeq   = pstSession->vtTripPathLinks[nEnd].dwGpsSeq;
		const time_t dtStart    = pstSession->vtTripPathLinks[nStart].dtGps;
		time_t       dtEnd      = pstSession->vtTripPathLinks[nEnd].dtGps;

		// [2026-09-23 최정우 추가 — 사용자 지시] **복구 행의 체류시간 산정.**
		//   tick 이 안 찍힌 링크는 vtTripPathLinks 에 항목이 하나뿐이라 start==end 가 되고,
		//   거리는 링크 전체를 넣으면서 시간은 0~1초가 돼 평균속도가 폭주한다
		//   (실측 000370_20260824135458 seq80: 102m/1초 = 369km/h, tick 실측은 16km/h).
		//   ① 다음 경로 링크로 넘어간 시각이 있으면 그것이 이 구간을 다 지난 시점이다.
		//   ② 없으면(트립 마지막 구간) 그 tick 의 실측 속도로 역산한다.
		//   **거리와 START/END_GPS_SEQ 는 건드리지 않는다** — 거리는 링크 길이로 이미 정확하고,
		//   gps_seq 를 넓히면 같은 유형 구간중복이 생긴다(안전규칙 3번과 같은 이유).
		{
			if ((nEnd + 1) < nCnt)
			{
				const time_t dtNext = pstSession->vtTripPathLinks[nEnd + 1].dtGps;
				if (dtNext > dtEnd) dtEnd = dtNext;						// ① 다음 링크 진입 시각
			}
			if (difftime(dtEnd, dtStart) <= 0.0)						// ② 실측 속도 역산
			{
				const float fSpd = pstSession->vtTripPathLinks[nStart].fSpeedKmh;
				if (fSpd > 0.1f)
					dtEnd = dtStart + static_cast<time_t>((dfDistM / (fSpd / 3.6)) + 0.5);
			}
		}

		// [2026-09-22 최정우 수정] **새 행을 만들기 전에 인접 일반도로 행에 흡수부터 시도한다.**
		//   새 행으로 만들었더니 두 기준을 어겼다(실측): ① 복구 행의 gps_seq 범위가 기존 행과 겹쳐
		//   **같은 유형 구간중복이 12→26 쌍**으로 늘고, ② 복구 행은 과거 위치에 생기는데 이미 방출된
		//   행이 번호를 가져간 뒤라 **trip_seq 순이 gps_seq 순과 19행 어긋났다**.
		//   흡수 방식은 거리만 더하고 **gps_seq 범위를 넓히지 않으므로** 둘 다 생기지 않는다
		//   (A-1 면제 이월 소급 적용에서 검증된 방식과 동일).
		{
			CHARGE_INSERT_ROW *pstAdj = nullptr;
			long nBestGap = 0;
			for (size_t qi = 0; qi < pstSession->vtPendingEmit.size(); ++qi)
			{
				CHARGE_INSERT_ROW& stQ = pstSession->vtPendingEmit[qi];
				if (stQ.strChargeType != "0") continue;
				if (stQ.strTripId != pstSession->szTripId) continue;
				const long nQS = atol(stQ.strStartGpsSeq.c_str());
				const long nQE = atol(stQ.strEndGpsSeq.c_str());
				if ((nQS <= 0) || (nQE <= 0)) continue;
				// 구간이 그 행 안에 들어가거나(대부분), 양끝에서 2 tick 이내로 맞닿는 경우
				long nGap = 0;
				if ((static_cast<long>(dwStartSeq) >= nQS) && (static_cast<long>(dwEndSeq) <= nQE))
					nGap = 0;
				else if (static_cast<long>(dwStartSeq) > nQE)
					nGap = static_cast<long>(dwStartSeq) - nQE;
				else if (static_cast<long>(dwEndSeq) < nQS)
					nGap = nQS - static_cast<long>(dwEndSeq);
				else
					nGap = 0;						// 부분 겹침
				if (nGap > 2) continue;
				if ((pstAdj == nullptr) || (nGap < nBestGap)) { pstAdj = &stQ; nBestGap = nGap; }
			}

			if (pstAdj != nullptr)
			{
				// 거리만 더한다 — START/END_GPS_SEQ 는 건드리지 않아야 구간중복이 안 생긴다.
				//   SPEED_KMH 도 재계산하지 않는다(체류 범위를 안 넓혔으므로 분모가 그대로다).
				const long nNewDist = atol(pstAdj->strDistM.c_str()) + static_cast<long>(dfDistM + 0.5);
				char szAdjBuf[32];
				snprintf(szAdjBuf, sizeof(szAdjBuf), "%ld", nNewDist);
				pstAdj->strDistM = szAdjBuf;
				for (size_t ci = 0; ci < vtSpanLinks.size(); ++ci)
					pstAdj->vtCoveredLinks.push_back(vtSpanLinks[ci]);
				++nMade;
				LOGFMTI("[#%02d] uncovered span absorbed!trip_id=[%s] gps=[%u~%u] links=[%d] dist=[+%.1f]m "
					"into_row=[%s~%s] gap=[%ld]tick",
					nThreadId, pstSession->szTripId, dwStartSeq, dwEndSeq,
					static_cast<int>(vtSpanLinks.size()), dfDistM,
					pstAdj->strStartGpsSeq.c_str(), pstAdj->strEndGpsSeq.c_str(), nBestGap);
				continue;						// 흡수했으므로 새 행을 만들지 않는다
			}
		}

		// [2026-09-22 최정우 추가] 인접 행이 없어 **새 행**을 만들어야 하는데 그 위치가 이미 방출된
		//   행보다 앞이면 만들지 않는다 — trip_seq 는 방출 순서로 매겨지므로 과거 위치에 새 행을
		//   더하면 gps_seq 순과 어긋난다(실측: 이 가드 없이 19행 불일치). 흡수는 기존 행의 거리만
		//   늘려 순서를 건드리지 않으므로 이 제한을 받지 않는다.
		if ((pstSession->dwLastEmittedStartSeq > 0) && (dwStartSeq < pstSession->dwLastEmittedStartSeq))
		{
			LOGFMTW("[#%02d] uncovered span skipped(order)!trip_id=[%s] gps=[%u~%u] dist=[%.1f]m "
				"last_emitted_start=[%u]",
				nThreadId, pstSession->szTripId, dwStartSeq, dwEndSeq, dfDistM,
				pstSession->dwLastEmittedStartSeq);
			continue;
		}

		CHARGE_INSERT_ROW stRow;
		// 좌표는 링크 기하로 채운다 — 이 구간은 tick 이 없어 매칭 좌표가 없다
		PLINK_INFO pstFirst = m_stConfig.pcDataLoader->GetLinkInfo(vtSpanLinks.front());
		PLINK_INFO pstLast  = m_stConfig.pcDataLoader->GetLinkInfo(vtSpanLinks.back());
		const double dfFromLon = (pstFirst != nullptr) ? (static_cast<double>(pstFirst->dwStNodeX) / 360000.0) : 0.0;
		const double dfFromLat = (pstFirst != nullptr) ? (static_cast<double>(pstFirst->dwStNodeY) / 360000.0) : 0.0;
		const double dfToLon   = (pstLast  != nullptr) ? (static_cast<double>(pstLast->dwEdNodeX)  / 360000.0) : 0.0;
		const double dfToLat   = (pstLast  != nullptr) ? (static_cast<double>(pstLast->dwEdNodeY)  / 360000.0) : 0.0;

		BuildNodeStepRowFromLinkRange(pstSession->szTripId, pstSession->szDeviceKey,
			pstSession->nChargeSeq, vtSpanLinks.front(), vtSpanLinks.back(),
			dfFromLat, dfFromLon, dfToLat, dfToLon, dfDistM,
			dtStart, dtEnd, dwStartSeq, dwEndSeq, "Y", "0", nullptr, nullptr, &stRow);
		stRow.vtCoveredLinks = vtSpanLinks;				// 자기가 덮은 링크를 스스로 기록
		pstSession->vtPendingEmit.push_back(stRow);
		++nMade;

		LOGFMTI("[#%02d] uncovered span recovered!trip_id=[%s] gps=[%u~%u] links=[%d] dist=[%.1f]m "
			"from=[%llu] to=[%llu]",
			nThreadId, pstSession->szTripId, dwStartSeq, dwEndSeq,
			static_cast<int>(vtSpanLinks.size()), dfDistM,
			static_cast<unsigned long long>(vtSpanLinks.front()),
			static_cast<unsigned long long>(vtSpanLinks.back()));
	}
	return nMade;
}

/**
 * @brief 차량 진행 방향이 구역 진행 방향과 정반대인지 판정 (2026-09-22 최정우 추가 — 사용자 지시)
 * @param[in] strRoadId 대상 구역 road_id
 * @param[in] stRawLogInfo 진입 판정 중인 tick
 * @return true = 역방향(진입 불인정)
 * @remark 왕복분리 도로는 상·하행 링크가 3~4m 거리라 GPS 오차(3~10m) 안에 들어온다. 그래서
 *   반대편 차선 링크에 붙는 오매칭이 나고, 그 링크가 과금구역이면 **주행하지 않은 구간에 요금이
 *   부과된다**(실측 000994_20250903152350 gps87: 북행 중 남행 RL-Z00004 에 4m 차이로 붙어
 *   개방형 142m 가 Y/0 로 청구됐다. 전수 분석 결과 **7개 지점**에서 같은 패턴 — RL-Z00004 3건,
 *   RL-Z00012 4건).
 *   맵매칭 자체를 고치는 대신(과거에 반대편 링크 교정을 넓히려다 근거 부족으로 원복한 이력이 있다)
 *   **요금이 붙는 지점에서만** 거른다.
 *   [임계 150°의 근거 — 실측 70건 전수 분석]
 *     · 정상 통행의 heading↔구역방위 차이는 **최대 90° 미만**
 *     · 역방향 3건은 **173~179°**
 *     · 사이가 비어 있어 150° 로 자르면 정상 통행은 **한 건도 걸리지 않는다**
 *   [전체 방위각(첫점→끝점)을 쓰는 이유]
 *     곡선 구역에서 세그먼트별 편차가 최대 67°(RL-Z00008) 나지만, 임계 150° 에서는 세그먼트
 *     방식과 **판정 결과가 동일했다**(둘 다 3건). 게다가 LINE 구역은 vtCoords 를 파싱하지 않아
 *     (ZONE_INFO 주석 참고) 세그먼트를 쓰려면 로더까지 고쳐야 한다 — 이득 없이 범위만 넓어진다.
 *   [**호출 위치 주의**] 반드시 **세션 상태를 하나라도 세팅하기 전에** 불러야 한다. 처음에는
 *     szClosedRoadId·dtEntryTime·dfSpeedAccumDistM 을 채운 뒤에 판정했는데, 진입을 거부해도 그
 *     값들이 오염된 채 남아 **뒤이은 일반도로 누적에 섞여 들어갔다**(실측 4트립 504m 과다 계상).
 *     판정 인자도 세션 필드가 아니라 pstEntryGate->szRoadID 를 쓴다.
 *   [속도 예외] GPS heading 은 이동 방향에서 산출되므로 정차·저속에서 의미가 없다.
 *     5km/h 미만이면 검증하지 않고 **종전대로 진입을 허용**한다(막아서 생기는 과소 청구 회피).
*/
bool CRawLogWorker::IsZoneDirectionOpposite(const string& strRoadId, const RAW_LOG_INFO& stRawLogInfo)
{
	static const double MM_ZONE_DIR_OPPOSITE_DEG = 150.0;	// 역방향 판정 임계
	static const double MM_ZONE_DIR_MIN_SPEED_KMH = 5.0;	// heading 을 믿을 수 있는 최저 속도

	if ((m_stConfig.pcChargeDataLoader == nullptr) || strRoadId.empty()) return false;
	if (stRawLogInfo.fSpeed < MM_ZONE_DIR_MIN_SPEED_KMH) return false;	// 저속·정차 — 검증 생략

	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(strRoadId);
	if (pstZone == nullptr) return false;
	if ((pstZone->dfFirstLat == 0.0) && (pstZone->dfFirstLon == 0.0)) return false;
	if ((pstZone->dfLastLat == 0.0) && (pstZone->dfLastLon == 0.0)) return false;

	// 구역 진행 방위각 (첫 정점 → 마지막 정점)
	const double dfLon1 = pstZone->dfFirstLon * M_PI / 180.0;
	const double dfLat1 = pstZone->dfFirstLat * M_PI / 180.0;
	const double dfLon2 = pstZone->dfLastLon  * M_PI / 180.0;
	const double dfLat2 = pstZone->dfLastLat  * M_PI / 180.0;
	const double dfDLon = dfLon2 - dfLon1;
	double dfZoneBrg = atan2(sin(dfDLon) * cos(dfLat2),
		cos(dfLat1) * sin(dfLat2) - sin(dfLat1) * cos(dfLat2) * cos(dfDLon)) * 180.0 / M_PI;
	dfZoneBrg = fmod(dfZoneBrg + 360.0, 360.0);

	if (stRawLogInfo.nAngle < 0) return false;				// 방위각 미제공(-1) — 검증 생략
	double dfDiff = fabs(static_cast<double>(stRawLogInfo.nAngle) - dfZoneBrg);
	dfDiff = fmod(dfDiff, 360.0);
	if (dfDiff > 180.0) dfDiff = 360.0 - dfDiff;

	return (dfDiff >= MM_ZONE_DIR_OPPOSITE_DEG);
}

/**
 * @brief 원시좌표가 주정차 폴리곤 안(경계 park_pad 이내 포함, 정확도가 pad 보다 작으면 정확도까지)인지
 *   (2026-10-04 최정우 추가, 사용자 확정 — 정책 E18) — ProcessParkingCharge() 의 기본 판정과 같은 반경
*/
bool CRawLogWorker::IsRawInParkingZone(double dfLon, double dfLat, int nAccuracyM)
{
	if (m_stConfig.pcChargeDataLoader == nullptr)
		return false;
	double dfPadM = static_cast<double>(m_stConfig.nParkPad);
	if ((nAccuracyM >= 0) && (static_cast<double>(nAccuracyM) < dfPadM))
		dfPadM = static_cast<double>(nAccuracyM);
	vector<PZONE_INFO> vtZones;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(dfLon, dfLat, dfPadM, &vtZones);
	return !vtZones.empty();
}

/**
 * @brief SKIP 버퍼에 주정차 폴리곤 안 SKIP 좌표가 하나라도 있는지 (2026-10-04 최정우 추가, 사용자 확정 — E18)
 * @remark 사용자 규칙: "GPS 좌표가 SKIP 좌표이면 주정차 단속구역 내 일반도로 주행과 관계없이 주정차 단속
 *   정보에만 포함". 이 구간은 일반도로(SKIP 구간 복구 포함)·구간단속 판정에 쓰지 않는다.
*/
bool CRawLogWorker::IsSkipGapInParkingZone(const VEHICLE_TRIP_SESSION *pstSession)
{
	if (pstSession == nullptr)
		return false;
	for (size_t i = 0; i < pstSession->vtSkipRunRawLogInfo.size(); ++i)
	{
		const RAW_LOG_INFO& stTick = pstSession->vtSkipRunRawLogInfo[i];
		if (IsRawInParkingZone(stTick.dfX, stTick.dfY, stTick.nAccuracyM))
			return true;
	}
	return false;
}

/**
 * @brief 링크가 주정차 단속 폴리곤 안에 있는지 (2026-09-22 최정우 추가 — 사용자 지적)
 * @param[in] qwLinkID 대상 링크
 * @return true = 폴리곤 안 (커버리지 복구 대상이 아님)
 * @remark **주정차는 폴리곤 기반이라 링크 등록 개념이 없다.** 그래서 IsCase3EligibleRoadKind() 는
 *   주정차 폴리곤 안의 링크도 "미등록(=일반도로)" 으로 돌려준다(그 함수 주석에 명시돼 있다).
 *   커버리지 복구가 그 값을 그대로 믿으면, **폴리곤 안을 짧게 통과해 주정차가 최소체류 미달로
 *   미등록된 구간을 일반도로로 청구**하게 된다 — 실측 000376_20260819094414 trip_seq=8
 *   (gps 99~115, 301m)이 그렇게 생겼다(사용자 지적).
 *   "매칭좌표가 폴리곤 안이면 속도 무관 주정차" 는 확정 규칙이고, 통과 주행을 일반도로로 돌리려는
 *   시도는 2026-09-16 에 Y/0 구간중복 회귀로 **전량 원복된 바 있다**. 그 규칙을 우회하지 않도록
 *   복구 대상에서 제외한다.
*/
bool CRawLogWorker::IsLinkInsideParkingPolygon(uint64 qwLinkID)
{
	if ((qwLinkID == 0) || (m_stConfig.pcDataLoader == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr)) return false;

	PLINK_INFO pstLink = m_stConfig.pcDataLoader->GetLinkInfo(qwLinkID);
	if (pstLink == nullptr) return false;

	// 링크 중점으로 판정한다 — 양 끝 노드만 보면 경계에 걸친 링크를 놓친다
	const double dfMidX = ((static_cast<double>(pstLink->dwStNodeX)
		+ static_cast<double>(pstLink->dwEdNodeX)) / 2.0) / 360000.0;
	const double dfMidY = ((static_cast<double>(pstLink->dwStNodeY)
		+ static_cast<double>(pstLink->dwEdNodeY)) / 2.0) / 360000.0;

	vector<PZONE_INFO> vtPark;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(dfMidX, dfMidY, 0.0, &vtPark);
	if (!vtPark.empty())
		return true;

	// [버그 수정, 2026-10-04 최정우 — 사용자 지적 계기] 위 "중점"은 양 끝 노드를 **직선으로 이은**
	//   중점이라, 굽은 링크는 형상 전체가 폴리곤 안이어도 그 점이 밖에 떨어져 이 판정을 빠져나갔다 —
	//   실측 000376_20260819093337: 폴리곤 안 링크 2040425801(102.4m)이 커버리지 복구로 일반도로에
	//   청구됐다(모드 0 에서도). 직선 중점이 밖이면 **링크 형상 기준**으로 다시 본다: 링크 길이의
	//   절반 이상이 폴리곤 안이면 "안". 후보 구역은 중점 주변(링크 길이의 절반 반경)에서만 찾는다.
	// 되돌리는 법: 이 블록을 지우면 종전(직선 중점) 판정이다.
	vector<PZONE_INFO> vtNear;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(dfMidX, dfMidY, pstLink->dfLen / 2.0 + 1.0, &vtNear);
	for (size_t z = 0; z < vtNear.size(); ++z)
	{
		vector<LINK_POLY_SPAN> vtSpans;
		if (!FindLinkPolygonSpans(qwLinkID, vtNear[z]->vtCoords, &vtSpans)) continue;
		double dfInsideM = 0.0;
		for (size_t sp = 0; sp < vtSpans.size(); ++sp)
			dfInsideM += (vtSpans[sp].dfEndDistM - vtSpans[sp].dfStartDistM);
		if ((pstLink->dfLen > 0.0) && (dfInsideM >= (pstLink->dfLen * 0.5)))
			return true;
	}
	return false;
}

/**
 * @brief 과금 대상 일반도로 구간에 완전히 포함되는 N/3 일반도로 행을 제거
 *   (2026-09-22 최정우 추가 — 사용자 지시)
 * @param[in] nThreadId 로그용 워커 번호
 * @param[in,out] pstSession 대상 세션 — vtPendingEmit 에서 해당 행을 뺀다
 * @return 제거한 행 수
 * @remark SKIP 구간 브릿지(NCR 1·2)는 "이 구간은 매칭이 끊겨 직선으로 추정했다"는 **심사 증거**로
 *   N/3 행을 남긴다. 그런데 그 구간을 정규 일반도로 행이 이미 덮고 있으면, 같은 유형끼리 GPS_SEQ
 *   범위가 겹쳐 **거리가 이중 계상**된다(실측 10행 전부 그랬다 — 000376_20260825170744 seq2 는
 *   167m 가 seq1 의 2~140 안에 통째로 들어간다, 사용자 지적).
 *   종전 코드는 이를 알고도 남겼다("이중 계상은 남지만 엉뚱한 run 의 거리를 깎는 것보다 낫다 —
 *   정확도 우선", AbsorbGapIntoOpenRun 주석). 그 판단은 **흡수 대상 run 을 특정할 수 없을 때**의
 *   이야기이고, 여기처럼 **같은 트립·같은 유형·완전 포함**이 확인되면 애매함이 없다.
 *   [안전 규칙]
 *   - **일반도로(charge_type=0)끼리만** 본다 — 타 과금유형은 건드리지 않는다.
 *   - 포함하는 쪽은 **CHARGE_YN='Y'(과금 대상)** 여야 한다. N/3 끼리는 서로 지우지 않는다.
 *   - **완전 포함**만 제거한다(부분 겹침은 그대로) — 일부라도 밖으로 나가면 그만큼은 정규 행이
 *     덮지 않은 구간이므로 증거를 남겨야 한다.
 *   - 비교 대상은 큐에 남은 행 **+ 이미 방출된 Y 행의 범위**(vtEmittedNodeStepRanges) 둘 다다.
 *     큐만 보면 먼저 나간 행을 못 찾아 놓친다.
*/
int CRawLogWorker::DropContainedAuditNodeStepRows(int nThreadId, VEHICLE_TRIP_SESSION *pstSession)
{
	if ((pstSession == nullptr) || pstSession->vtPendingEmit.empty()) return 0;

	vector<CHARGE_INSERT_ROW> vtKeep;
	int nDropped = 0;
	for (size_t i = 0; i < pstSession->vtPendingEmit.size(); ++i)
	{
		const CHARGE_INSERT_ROW& stCur = pstSession->vtPendingEmit[i];
		bool bDrop = false;
		if ((stCur.strChargeType == "0") && (stCur.strChargeYn == "N"))
		{
			const long nCurS = atol(stCur.strStartGpsSeq.c_str());
			const long nCurE = atol(stCur.strEndGpsSeq.c_str());
			if ((nCurS > 0) && (nCurE >= nCurS))
			{
				// ① 큐에 남은 과금 대상 일반도로 행
				for (size_t j = 0; (j < pstSession->vtPendingEmit.size()) && !bDrop; ++j)
				{
					if (i == j) continue;
					const CHARGE_INSERT_ROW& stOther = pstSession->vtPendingEmit[j];
					if (stOther.strChargeType != "0") continue;
					if (stOther.strChargeYn != "Y") continue;
					if (stOther.strTripId != stCur.strTripId) continue;
					const long nOS = atol(stOther.strStartGpsSeq.c_str());
					const long nOE = atol(stOther.strEndGpsSeq.c_str());
					if ((nOS <= 0) || (nOE < nOS)) continue;
					if ((nOS <= nCurS) && (nCurE <= nOE)) bDrop = true;
				}
				// ② 이미 방출된 과금 대상 일반도로 행의 범위
				for (size_t k = 0; (k < pstSession->vtEmittedNodeStepRanges.size()) && !bDrop; ++k)
				{
					const long nES = static_cast<long>(pstSession->vtEmittedNodeStepRanges[k].first);
					const long nEE = static_cast<long>(pstSession->vtEmittedNodeStepRanges[k].second);
					if ((nES <= nCurS) && (nCurE <= nEE)) bDrop = true;
				}
			}
		}

		if (bDrop)
		{
			++nDropped;
			LOGFMTI("[#%02d] audit node step row dropped(contained)!trip_id=[%s] gps=[%s~%s] "
				"dist=[%s]m ncr=[%s] — 과금 대상 일반도로 구간에 완전히 포함",
				nThreadId, stCur.strTripId.c_str(), stCur.strStartGpsSeq.c_str(),
				stCur.strEndGpsSeq.c_str(), stCur.strDistM.c_str(), stCur.strNonChargeReason.c_str());
			continue;
		}
		vtKeep.push_back(stCur);
	}

	if (nDropped > 0) pstSession->vtPendingEmit.swap(vtKeep);
	return nDropped;
}

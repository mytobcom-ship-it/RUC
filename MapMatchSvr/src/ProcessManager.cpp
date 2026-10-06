/**
 * @file ProcessManager.cpp
 * @brief 작업용 클래스 소스 파일
*/
#include "ProcessManager.h"

namespace {

/**
 * @brief 맵매칭 트레이스(MatchTrace) 컨텍스트 채우기 — 원시 GPS·맵매칭 입력·직전 링크를 진단 로그용으로 복사
 * @param[out] stTraceCtx 채울 트레이스 컨텍스트(먼저 0 으로 초기화)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMapMatchInput 이번 시도에 쓰는 맵매칭 입력(반경·속도·heading·고도)
 * @param[in] qwPrevLinkId 직전 링크 ID(0=없음)
 * @param[in] bContinue true=연속(Continue) 시도, false=시작(Begin) 시도
 * @param[in] pstAltCtx 고도 컨텍스트(nullable) — bHasPrevAlt 일 때만 직전 고도·ROAD_TYPE 복사
 * @return void
 * @remark (2026-10-06 최정우 주석 추가)
*/
void FillMatchTraceCtx(MATCH_TRACE_CTX& stTraceCtx, int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MAP_MATCH_INPUT& stMapMatchInput, uint64 qwPrevLinkId, bool bContinue,
		const ALT_MATCH_CTX *pstAltCtx)
{
	memset(reinterpret_cast<void *>(&stTraceCtx), 0, sizeof(stTraceCtx));
	stTraceCtx.nThreadId = nThreadId;
	strncpy(stTraceCtx.szDeviceKey, stRawLogInfo.szDeviceKey, sizeof(stTraceCtx.szDeviceKey) - 1);
	strncpy(stTraceCtx.szTripId, stRawLogInfo.szTripID, sizeof(stTraceCtx.szTripId) - 1);
	stTraceCtx.dwSeqNo = stRawLogInfo.dwSeqNo;
	stTraceCtx.dfGpsLat = stRawLogInfo.dfY;
	stTraceCtx.dfGpsLon = stRawLogInfo.dfX;
	stTraceCtx.nRadius = stMapMatchInput.nRadius;
	stTraceCtx.nSpeed = stMapMatchInput.nSpeed;
	stTraceCtx.nHeading = stMapMatchInput.nAngle;
	stTraceCtx.nAltitudeM = stMapMatchInput.nAltitudeM;
	stTraceCtx.bContinue = bContinue;
	stTraceCtx.qwPrevLinkId = qwPrevLinkId;
	stTraceCtx.nMatchedStep = 0;
	if (pstAltCtx != nullptr && pstAltCtx->bHasPrevAlt)
	{
		stTraceCtx.nPrevAltitude = pstAltCtx->nPrevAltitude;
		stTraceCtx.nPrevRoadType = pstAltCtx->nPrevRoadType;
	}
}

} // namespace

/**
 * @brief 생성자
*/
CProcessManager::CProcessManager() : 
	m_pcDataLoader(nullptr), 
	m_pcMapMatch(nullptr),
	m_nThreadId(0), 
	m_nCoordinateType(0), 
	m_nRadius(0), 
	m_dfRadiusScale(2.5),
	m_nRadiusMin(20),
	m_nRadiusMax(50),
	m_qwLinkID(0), 
	m_dwMaxDistance(0)
{
	// init
	memset(reinterpret_cast<void *>(m_szStartDate), 0, sizeof(m_szStartDate));
	memset(reinterpret_cast<void *>(m_szDriveID), 0, sizeof(m_szDriveID));
	memset(reinterpret_cast<void *>(m_szOperID), 0, sizeof(m_szOperID));
	m_vtMapMatchInfoList.clear();
}

/**
 * @brief 소멸자
*/
CProcessManager::~CProcessManager()
{
	if (m_pcMapMatch != nullptr)
	{
		delete m_pcMapMatch;
		m_pcMapMatch = nullptr;
	}
}

/**
 * @brief 작업 초기화 및 맵 매칭 작업
 * @param[in] nThreadId 쓰레드 ID
 * @param[in] pcDataLoader 형상정보 데이터 클래스
 * @param[in] nCoordinateType GPS 좌표 측지계
 * @param[in] nRadius config radius — ACCURACY_M NULL 시 검색 반경 폴백 (m) (2026-07-08 최정우)
 * @param[in] dwMaxDistance 연속 맵 매칭시 Heading 유효거리
 * @param[in] dfRadiusScale config radius_scale — 검색반경 = scale × ACCURACY_M (2026-07-08 최정우)
 * @param[in] nRadiusMin config radius_min — 적응형 검색 반경 하한 (m) (2026-07-08 최정우)
 * @param[in] nRadiusMax config radius_max — 적응형 검색 반경 상한 (m) (2026-07-08 최정우)
 * @param[in] stAltitudeConfig config altitude_* — 연속 맵매칭 고도 보조 점수
 *            [정정] 키 이름은 2026-07-21 에 alt_gap·alt_penalty·alt_weight·alt_slope 로 바뀌었다
 *            (2026-10-06 최정우 주석 수정)
 * @return true(성공), false(실패)
*/
bool CProcessManager::Initialize(const int nThreadId, CDataLoader *pcDataLoader,
		const uint8& nCoordinateType, const sint16& nRadius, const uint32& dwMaxDistance,
		const double& dfRadiusScale, const sint16& nRadiusMin, const sint16& nRadiusMax,
		const ALTITUDE_SCORE_CONFIG& stAltitudeConfig)
{
	m_nThreadId = nThreadId;					// 쓰레드 ID

	m_pcDataLoader = pcDataLoader;
	if (m_pcDataLoader == nullptr)
	{
		LOGFMTE("[#%02d] data loader is null!", m_nThreadId);
		return false;
	}

	m_nCoordinateType = nCoordinateType;		// GPS 좌표 측지계
	m_nRadius = nRadius;						// 검색 반경 폴백 (ACCURACY_M NULL)
	m_dwMaxDistance = dwMaxDistance;			// 연속 맵매칭시 Heading 유효거리
	m_dfRadiusScale = (dfRadiusScale > 0.0) ? dfRadiusScale : 2.5;
	m_nRadiusMin = (nRadiusMin > 0) ? nRadiusMin : 20;
	m_nRadiusMax = (nRadiusMax >= m_nRadiusMin) ? nRadiusMax : m_nRadiusMin;
	m_stAltitudeConfig = stAltitudeConfig;

	if (m_pcMapMatch != nullptr)
	{
		delete m_pcMapMatch;
		m_pcMapMatch = nullptr;
	}

	m_pcMapMatch = new (std::nothrow)CMapMatch;
	if (m_pcMapMatch == nullptr)
	{
		LOGFMTE("[#%02d] map match instance is null!", m_nThreadId);
		return false;
	}

	// MapMatch 인스턴스에 형상 데이터 연결 (2026-07-08 최정우 주석 추가)
	if (!m_pcMapMatch->Initialize(m_pcDataLoader))
	{
		delete m_pcMapMatch;
		m_pcMapMatch = nullptr;
		LOGFMTE("[#%02d] map match initialize failed!", m_nThreadId);
		return false;
	}

	// 연속 맵매칭 고도 보조 점수 config 적용 (2026-07-08 최정우 주석 추가)
	m_pcMapMatch->SetAltitudeConfig(m_stAltitudeConfig);

	return true;
}

/**
 * @brief ACCURACY_M 기반 적응형 맵매칭 검색 반경 (m)
 * @param[in] nAccuracyM 수평 오차 ACCURACY_M (m). NO_ACCURACY 이면 config radius 폴백
 * @return 검색 반경 (m)
 * @remark 2026-07-08 최정우 추가
 *
 * 공식:
 *   ACCURACY_M NULL  → 검색반경 = radius
 *   ACCURACY_M 있음  → 검색반경 = max(radius_min, min(round(radius_scale × ACCURACY_M), radius_max))
 *
 * 예) radius=50, radius_scale=2.5, radius_min=20, radius_max=50
 *     ACCURACY_M=10  → round(2.5×10)=25  → 25m
 *     ACCURACY_M=30  → round(2.5×30)=75  → min(75,50)=50m
 *     ACCURACY_M=NULL → 50m (radius 폴백)
 *   [보완] 위 예의 radius_max=50 은 작성 당시 값이다 — 현행 config.ini·기본값(CFG_DEF_RADIUS_MAX)은
 *     75 라 ACCURACY_M=30 이면 75m 가 된다 (2026-10-06 최정우 주석 추가)
*/
sint16 CProcessManager::CalcAdaptiveRadius(sint16 nAccuracyM) const
{
	if (nAccuracyM < 0)
		return m_nRadius;

	int nAdaptive = static_cast<int>(m_dfRadiusScale * static_cast<double>(nAccuracyM) + 0.5);
	nAdaptive = std::max(static_cast<int>(m_nRadiusMin),
			std::min(nAdaptive, static_cast<int>(m_nRadiusMax)));
	return static_cast<sint16>(nAdaptive);
}

/**
 * @brief RAW_LOG_INFO → MAP_MATCH_INPUT 변환
 * @param[in] stRawLogInfo 원시 GPS
 * @param[out] pstMapMatchInput 맵매칭 입력
 * @param[in] qwLinkID 직전 링크 ID (0이면 시작 후보)
 * @param[in] pstAltCtx 연속 맵매칭 고도 컨텍스트 (nullable)
 * @return void
 * @remark
 *   고도 보조 점수(bUseAltScore) 활성 조건 (모두 충족):
 *     1) qwLinkID ≠ 0 (연속 맵매칭)
 *     2) pstAltCtx·bHasPrevAlt (직전 성공 시 GPS 고도 앵커 있음)
 *     3) alt_weight > 0
 *   예) 직전100m·현재106m·같은 ROAD_TYPE → ContinueMapMatch에서 dfCost −10 가산
*/
void CProcessManager::BuildMapMatchInput(const sRawLogInfo& stRawLogInfo,
		MAP_MATCH_INPUT *pstMapMatchInput, uint64 qwLinkID,
		const ALT_MATCH_CTX *pstAltCtx) const
{
	if (pstMapMatchInput == nullptr)
		return;

	*pstMapMatchInput = MAP_MATCH_INPUT();
	pstMapMatchInput->nCoordinateType = (stRawLogInfo.nCoordinateType != 0)
		? stRawLogInfo.nCoordinateType : m_nCoordinateType;
	// ACCURACY_M 기반 적응형 검색 반경 산출 (2026-07-08 최정우 주석 추가)
	pstMapMatchInput->nRadius = CalcAdaptiveRadius(stRawLogInfo.nAccuracyM);
	pstMapMatchInput->dfX = stRawLogInfo.dfX;
	pstMapMatchInput->dfY = stRawLogInfo.dfY;
	pstMapMatchInput->nAngle = (stRawLogInfo.nAngle >= 0) ? stRawLogInfo.nAngle : static_cast<sint16>(NO_ANGLE);
	pstMapMatchInput->nSpeed = (stRawLogInfo.fSpeed >= 0.0f)
		? static_cast<sint16>(stRawLogInfo.fSpeed + 0.5f) : static_cast<sint16>(NO_SPEED);
	pstMapMatchInput->qwLinkID = 0;
	pstMapMatchInput->nAltitudeM = stRawLogInfo.nAltitudeM;
	pstMapMatchInput->nDriveStatus = stRawLogInfo.nDriveStatus;

	// 연속 맵매칭 + 직전 고도 앵커 + config 활성 시 고도 보조 점수 ON
	if (qwLinkID != 0 && pstAltCtx != nullptr && pstAltCtx->bHasPrevAlt
		&& m_stAltitudeConfig.dfWeight > 0.0)
	{
		pstMapMatchInput->bUseAltScore = true;
		pstMapMatchInput->nPrevAltitude = pstAltCtx->nPrevAltitude;
		pstMapMatchInput->nPrevRoadType = pstAltCtx->nPrevRoadType;
		pstMapMatchInput->dfHorizMove = pstAltCtx->dfHorizMove;
		pstMapMatchInput->dfGapSec = pstAltCtx->dfGapSec;	// (2026-09-04 최정우 추가)
	}

	// 연속 맵매칭 + 직전 매칭 위치 보유 시 역행 페널티용 위치 전달 (2026-07-20 최정우 추가)
	if (qwLinkID != 0 && pstAltCtx != nullptr && pstAltCtx->bHasPrevLinkPos)
	{
		pstMapMatchInput->dfPrevLinkPos = pstAltCtx->dfPrevLinkPos;
		pstMapMatchInput->bHasPrevLinkPos = true;
		// 같은 링크 노이즈 보정 기준점(직전 신뢰 매칭 좌표) 함께 전달 (2026-07-22 최정우 추가)
		pstMapMatchInput->dfPrevMatchX = pstAltCtx->dfPrevMatchX;
		pstMapMatchInput->dfPrevMatchY = pstAltCtx->dfPrevMatchY;
		// [2026-09-16 최정우 — 판정 완료, 배선하지 않기로 확정] 아래 "미수정" 은 유지가 맞다.
		//   임시로 배선해 전체 재매칭한 결과: 검사(MapMatch.cpp 재구성 경로 방향 타당성)가 6회
		//   발동했고 **6회 전부 hops=[1]**, 즉 이 검사가 잡으려던 "여러 홉을 건너뛴 먼 우회 경로"는
		//   한 건도 없었다. 실제로 걸린 건 교차로 회전·정차 후 출발처럼 **한 tick 안에서 heading 이
		//   크게 바뀌는 정상 주행**뿐이다(실측: 000376_20260819140532 seq17 — 좌회전 진입 tick,
		//   heading 250→34, 직후 seq18 이 2040425201 에 intersect_len=1m 로 확실히 매칭됨).
		//   MATCH_STATUS 는 2건이 MATCHED→SKIP 으로 바뀌고(나머지 4건은 원래도 SKIP), 과금은 불변.
		//   살리려면 "직전 신뢰 위치가 N tick 이내" 또는 "hops>=2" 가드가 선행돼야 한다.
		// [참고, 2026-09-11 최정우 확인 — 미수정] dfPrevMatchX/Y 유효 여부 플래그 bHasPrevMatchPos
		//   (2026-08-23 추가)가 여기서 전달이 빠져 있어 항상 기본값 false 로 넘어간다 — 이 플래그를
		//   보는 소비측 중 MapMatch.cpp:432 의 "재구성 경로 방향 타당성 검사"(2026-09-04 추가)가
		//   그 때문에 지금까지 비활성 상태다. 전달을 고치면 그 검사가 깨어나 MATCH_STATUS 가
		//   재매칭 전체에서 29건(매칭→SKIP) 바뀌는 게 실측 확인됨 — 의도한 동작인지 별도 검토가
		//   필요해 일부러 고치지 않고 남겨둠(memory: project_full_source_review 계열 참고).
		//   [정정] "MapMatch.cpp:432" 는 행 번호가 바뀌었다 — CMapMatch::ContinueMapMatch() 안의
		//   "재구성 경로 방향 타당성" 검사(bHasPrevMatchPos 조건 분기)를 가리킨다 (2026-10-06 최정우 주석 수정)
		// 원시좌표·방향 동일 여부 함께 전달 — 같은 링크 노이즈 보정 적용 여부 판단용 (2026-09-02 최정우 추가)
		pstMapMatchInput->bSameRawAndHeadingAsPrev = pstAltCtx->bSameRawAndHeadingAsPrev;
	}
}

/**
 * @brief 실시간 GPS 1건 맵매칭 (시작 / Continue)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in,out] qwInOutLinkID 직전 링크 ID (device_key 세션). 성공 시 갱신
 * @param[out] pstMatchLinkInfo 맵매칭 결과
 * @param[in] pstAltCtx 연속 맵매칭 보조 컨텍스트 (고도 앵커·직전 매칭 위치, nullable)
 *                      (2026-09-21 최정우 주석 보완 — 파라미터가 있는데 설명이 빠져 있었다)
 * @return true(매칭 성공), false(실패)
*/
bool CProcessManager::ProcessRawLog(const sRawLogInfo& stRawLogInfo, uint64& qwInOutLinkID,
		MATCH_LINK_INFO *pstMatchLinkInfo, const ALT_MATCH_CTX *pstAltCtx)
{
	if (m_pcMapMatch == nullptr || pstMatchLinkInfo == nullptr)
		return false;

	memset(reinterpret_cast<void *>(pstMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);
	pstMatchLinkInfo->dfIntersectLenSgmt = -1.0;

	const uint64 qwPrevLinkId = qwInOutLinkID;

	MAP_MATCH_INPUT stMapMatchInput;
	// RAW GPS → MAP_MATCH_INPUT 변환 (적응형 검색 반경 포함) (2026-07-08 최정우 주석 추가)
	BuildMapMatchInput(stRawLogInfo, &stMapMatchInput, qwInOutLinkID, pstAltCtx);

	// 1차: 적응형 반경으로 Continue→시작 시도 (2026-07-10 최정우 수정)
	if (AttemptMatch(stRawLogInfo, stMapMatchInput, qwInOutLinkID, qwPrevLinkId, pstMatchLinkInfo, pstAltCtx))
		return true;

	// widen-on-miss: 적응형 반경이 상한보다 작으면 넓은 반경으로 1회 확장 재시도 (2026-07-10 최정우 추가)
	//   경계 밖(예: 반경 1m 초과)에 링크가 있어 놓친 경우 구제. 정밀(좁은 반경) 우선 + 실패 시에만 확장.
	const sint16 nWideRadius = std::max(m_nRadius, m_nRadiusMax);
	if (stMapMatchInput.nRadius < nWideRadius)
	{
		qwInOutLinkID = qwPrevLinkId;					// 연속 재시도 위해 직전 링크 복원
		// 입력 재구성(반경 외 nAngle/고도 컨텍스트 원복) 후 검색 반경만 확장 (2026-07-10 최정우 추가)
		BuildMapMatchInput(stRawLogInfo, &stMapMatchInput, qwInOutLinkID, pstAltCtx);
		stMapMatchInput.nRadius = nWideRadius;
		LOGFMTD("[#%02d] widen-on-miss retry!device=[%s] seq=[%u] radius=[%d]",
			m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
			static_cast<int>(nWideRadius));
		if (AttemptMatch(stRawLogInfo, stMapMatchInput, qwInOutLinkID, qwPrevLinkId, pstMatchLinkInfo, pstAltCtx))
			return true;
	}

	qwInOutLinkID = 0;

	// 정식 매칭 실패 — ① 진단반경(MM_DIAG_RADIUS) 이내 최근접 (2026-07-10 최정우 수정)
	//   MATCHED 아님 → SKIP·세션 미갱신, DB에 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
	if (FindNearestSegment(stRawLogInfo, pstMatchLinkInfo))
	{
		pstMatchLinkInfo->bOutOfRadius = true;
		LOGFMTD("[#%02d] out-of-radius nearest captured!device=[%s] seq=[%u] intersect_len=[%.1fm]",
			m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
			pstMatchLinkInfo->dfIntersectLenSgmt);
		return false;
	}

	// ② 그리드에 후보 있으나 진단반경 초과 — 반경 무시 기하 최근접 (2026-07-10 최정우 수정)
	//   동일하게 SKIP·세션 미갱신·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
	if (FindGeomNearestSegment(stRawLogInfo, pstMatchLinkInfo))
	{
		pstMatchLinkInfo->bOutOfRadius = true;
		LOGFMTW("[#%02d] beyond-diag nearest captured!device=[%s] seq=[%u] intersect_len=[%.1fm] diag_max=[%dm]",
			m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
			pstMatchLinkInfo->dfIntersectLenSgmt, MM_DIAG_RADIUS);
		return false;
	}

	LOGFMTW("[#%02d] map match failed!device=[%s] seq=[%u] err=[%u]",
		m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
		pstMatchLinkInfo->wErrorCode);
	return false;
}

/**
 * @brief 반경 밖 최근접 세그먼트 탐색(진단용) — MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 확보
 * @param[in] stRawLogInfo 원시 GPS
 * @param[out] pstMatchLinkInfo 최근접 세그먼트 결과(좌표·INTERSECT_LEN·링크정보)
 * @param[in] bIgnoreHeading true(기본)면 방위각을 지우고 순수 기하 최근접으로 찾는다.
 *                           false 면 원본 heading 을 살려 방향이 맞는 후보를 우선한다
 *                           (2026-09-21 최정우 주석 보완 — 기본값이 있는 파라미터라 설명이
 *                           빠져 있었고, 아래 @remark 는 "방위각 무시" 만 적어 마치 항상
 *                           무시하는 것처럼 읽혔다)
 * @return true(최근접 후보 발견), false(진단 반경 내 후보 없음)
 * @remark 시작(연속 아님), 진단반경 MM_DIAG_RADIUS 이내. 방위각은 bIgnoreHeading 에 따름.
 *         MATCHED 아님 — SKIP·세션 미갱신·DB 참고용만 (2026-07-10 최정우 수정)
*/
bool CProcessManager::FindNearestSegment(const sRawLogInfo& stRawLogInfo,
		MATCH_LINK_INFO *pstMatchLinkInfo, bool bIgnoreHeading)
{
	if ((m_pcMapMatch == nullptr) || (pstMatchLinkInfo == nullptr))
		return false;

	MAP_MATCH_INPUT stDiagInput;
	// 입력 재구성(고도 컨텍스트 없음, 시작) 후 방위각 무시·최대 반경으로 최근접 탐색 (2026-07-10 최정우 추가)
	BuildMapMatchInput(stRawLogInfo, &stDiagInput, 0, nullptr);
	stDiagInput.qwLinkID = 0;
	if (bIgnoreHeading)
		stDiagInput.nAngle = NO_ANGLE;				// 순수 기하 최근접(방위각 필터 미적용)
	stDiagInput.nRadius = MM_DIAG_RADIUS;			// 반경 밖 후보까지 포함하도록 최대 반경

	MATCH_TRACE_CTX stTraceCtx;
	FillMatchTraceCtx(stTraceCtx, m_nThreadId, stRawLogInfo, stDiagInput, 0, false, nullptr);
	// GRID 기반 시작 으로 최근접 세그먼트 1개 획득 (좌표는 /360000 역스케일되어 채워짐) (2026-07-10 최정우 수정)
	return m_pcMapMatch->BeginMapMatch(stDiagInput, pstMatchLinkInfo, &stTraceCtx);
}

/**
 * @brief 특정 링크 쪽으로 편향해 초기(BEGIN) 재매칭
 * @param[in] stRawLogInfo 원시 GPS 로그
 * @param[in] qwBiasLinkID 편향 기준 링크 (보통 "다음 확정 링크")
 * @param[out] pstMatchLinkInfo 재매칭 결과
 * @return true(성공), false(실패)
 * @remark
 * \tBEGIN 매칭은 heading 을 무시하고 거리만으로 판정한다(bIgnoreHeading, 2026-08-19).
 * \t왕복분리 도로는 짝 링크가 10m 남짓 옆에 나란히 있어 거리 차가 무의미한데도 더 가까운
 * \t쪽이 뽑히므로, 트립 첫 점이 반대방향 링크에 붙는 일이 생긴다.
 * \t(실측 trip 000376_20260819094414 seq1 — 정답 7.68m, 반대방향 4.16m)
 *
 * \t첫 점 heading 은 신뢰할 수 없다(전국 21트립 실측: 이후 점들의 평균 방향과 41° 어긋남).
 * \t그래서 heading 이 아니라 "이미 확정된 다음 점의 링크"를 편향 기준으로 삼아 다시 고른다.
 * \tqwBiasLinkID 는 BuildConnectedSet 으로 연결 집합이 되고, 그 밖의 후보는 cost 에
 * \tMM_CONNECT_PENALTY(30m)가 붙는다 — 위 사례의 거리차 3.5m 를 충분히 뒤집는다.
 * \t(2026-08-22 최정우 추가)
*/
bool CProcessManager::RematchBeginBiased(const sRawLogInfo& stRawLogInfo,
		uint64 qwBiasLinkID, MATCH_LINK_INFO *pstMatchLinkInfo)
{
	if ((m_pcMapMatch == nullptr) || (pstMatchLinkInfo == nullptr) || (qwBiasLinkID == 0))
		return false;

	MAP_MATCH_INPUT stInput;
	BuildMapMatchInput(stRawLogInfo, &stInput, 0, nullptr);
	stInput.qwLinkID = 0;					// 연속 아님 — BEGIN 경로로 태운다
	stInput.qwBiasLinkID = qwBiasLinkID;	// 다음 확정 링크 쪽으로 편향

	MATCH_TRACE_CTX stTraceCtx;
	FillMatchTraceCtx(stTraceCtx, m_nThreadId, stRawLogInfo, stInput, 0, false, nullptr);
	return m_pcMapMatch->BeginMapMatch(stInput, pstMatchLinkInfo, &stTraceCtx);
}

/**
 * @brief RematchBeginBiased + heading 신뢰성 검증 — SKIP 구간 개별 틱 소급 MATCHED 승격 전용
 * @param[in] stRawLogInfo 원시 GPS 로그 (SKIP 런 버퍼에 보관돼 있던 tick)
 * @param[in] qwBiasLinkID 편향 기준 링크 (다음 확정 링크)
 * @param[out] pstMatchLinkInfo 재매칭 결과
 * @return true(성공 — heading 검증까지 통과), false(실패 또는 heading 신뢰 불가로 시도 자체를 포기)
 * @remark
 * \tRematchBeginBiased 는 BEGIN 매칭 특성상 후보 정렬에 heading 을 안 써서(순수 거리+연결성 편향),
 * \t10m 옆 반대방향(왕복분리) 링크가 채택될 수 있다(BeginMapMatch.cpp FixOppositePairByHeading
 * \t주석, 실측 000376_20260819094414 M79 — 개방형 톨게이트 오과금까지 발생한 전례).
 * \tACCURACY_M SKIP 틱은 정지·저속 구간이 많아 heading 자체가 무의미(예: 0)한 경우가 흔하다 —
 * \t그런 틱까지 재매칭을 강행하면 이 위험이 가장 커지므로, heading·속도가 신뢰할 만할 때만
 * \t(IsAntiHeadingOpposite 와 동일 기준 MM_OPP_FIX_MIN_SPEED 재사용) 시도하고, 결과가 짝 링크
 * \t반대방향이면 실패 처리해 SKIP 을 유지한다 (2026-09-04 최정우 추가, 사용자 지시)
*/
bool CProcessManager::RematchBeginBiasedDirectional(const sRawLogInfo& stRawLogInfo,
		uint64 qwBiasLinkID, MATCH_LINK_INFO *pstMatchLinkInfo)
{
	if ((m_pcMapMatch == nullptr) || (pstMatchLinkInfo == nullptr) || (qwBiasLinkID == 0))
		return false;

	// heading 신뢰 전제 — NO_ANGLE·SPEED 없음·저속(MM_OPP_FIX_MIN_SPEED 미만)이면 반대편 판별
	//   근거가 없어 안전하게 포기(SKIP 유지)
	if ((stRawLogInfo.nAngle < 0) || (stRawLogInfo.fSpeed < 0.0f)
		|| (stRawLogInfo.fSpeed < static_cast<float>(MM_OPP_FIX_MIN_SPEED)))
		return false;

	if (!RematchBeginBiased(stRawLogInfo, qwBiasLinkID, pstMatchLinkInfo))
		return false;

	const sint16 nSpeedRounded = static_cast<sint16>(stRawLogInfo.fSpeed + 0.5f);
	if (m_pcMapMatch->IsAntiHeadingOpposite(pstMatchLinkInfo->qwLinkID, stRawLogInfo.nAngle, nSpeedRounded))
		return false;			// 짝 링크 반대방향 채택 — 신뢰 불가, SKIP 유지

	return true;
}

/**
 * @brief 진단반경 초과여도 기하 최근접 세그먼트 1건 (SKIP 참고용) (2026-07-10 최정우 수정)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[out] pstMatchLinkInfo 기하 최근접 세그먼트 결과(좌표·INTERSECT_LEN·링크정보)
 * @return true(최근접 후보 발견), false(그리드에 후보 없음·인자 무효) (2026-10-06 최정우 주석 추가)
 * @remark 정식·진단반경 매칭 실패 후 호출. MATCHED 아님, 세션 링크·앵커 미갱신.
*/
bool CProcessManager::FindGeomNearestSegment(const sRawLogInfo& stRawLogInfo,
		MATCH_LINK_INFO *pstMatchLinkInfo)
{
	if ((m_pcMapMatch == nullptr) || (pstMatchLinkInfo == nullptr))
		return false;

	MAP_MATCH_INPUT stDiagInput;
	BuildMapMatchInput(stRawLogInfo, &stDiagInput, 0, nullptr);
	stDiagInput.qwLinkID = 0;
	stDiagInput.nAngle = NO_ANGLE;
	stDiagInput.nRadius = MM_DIAG_RADIUS;

	return m_pcMapMatch->BeginGeomNearest(stDiagInput, pstMatchLinkInfo);
}

/**
 * @brief 지정 반경(stMapMatchInput.nRadius)으로 연속(Continue)→초기(시작) 1회 시도 (2026-07-10 최정우 추가)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in,out] stMapMatchInput 맵매칭 입력 (nRadius 등 호출측에서 세팅). 내부에서 qwLinkID/nAngle 갱신
 * @param[in,out] qwInOutLinkID 직전 링크 ID. 연속 실패 시 0, 성공 시 매칭 링크로 갱신
 * @param[in] qwPrevLinkId 트레이스용 직전 링크 ID
 * @param[out] pstMatchLinkInfo 맵매칭 결과
 * @param[in] pstAltCtx 고도 컨텍스트 (nullable)
 * @return true(매칭 성공), false(실패)
 * @remark widen-on-miss 재시도에서 동일 로직을 반경만 바꿔 재사용하기 위한 분리
*/
bool CProcessManager::AttemptMatch(const sRawLogInfo& stRawLogInfo, MAP_MATCH_INPUT& stMapMatchInput,
		uint64& qwInOutLinkID, uint64 qwPrevLinkId, MATCH_LINK_INFO *pstMatchLinkInfo,
		const ALT_MATCH_CTX *pstAltCtx)
{
	MATCH_TRACE_CTX stTraceCtx;
	// Continue 완전실패 후 Begin 재검색에도 실제 heading 을 넘기기 위해 지워지기 전에 보존
	//   (2026-08-24 최정우 추가 — 아래 IsAntiHeadingOpposite 거부권 판정용)
	const sint16 nOrigAngle = stMapMatchInput.nAngle;

	if (qwInOutLinkID != 0)
	{
		stMapMatchInput.qwLinkID = qwInOutLinkID;
		FillMatchTraceCtx(stTraceCtx, m_nThreadId, stRawLogInfo, stMapMatchInput, qwPrevLinkId, true, pstAltCtx);
		// 직전 링크 기준 연속 맵매칭 시도 (2026-07-08 최정우 주석 추가)
		if (m_pcMapMatch->ContinueMapMatch(stMapMatchInput, pstMatchLinkInfo, &stTraceCtx))
		{
			qwInOutLinkID = pstMatchLinkInfo->qwLinkID;
			LOGFMTD("[#%02d] continue map match ok!device=[%s] link=[%llu]",
				m_nThreadId, stRawLogInfo.szDeviceKey,
				static_cast<unsigned long long>(qwInOutLinkID));
			return true;
		}

		qwInOutLinkID = 0;
		stMapMatchInput.qwLinkID = 0;
		stMapMatchInput.nAngle = NO_ANGLE;
	}

	// 연속 실패·초기 세션 — GRID 기반 Begin 맵매칭 (2026-07-08 최정우 주석 추가)
	// 직전 성공 링크가 있으면 연결성 편향 전달 → 나란한 도로 오매칭 억제 (2026-07-15 최정우 추가)
	stMapMatchInput.qwBiasLinkID = qwPrevLinkId;
	// heading 은 위에서 지워졌지만(Begin 자체 후보 정렬은 원래 heading 무시), CBeginMapMatch 내부
	//   FixOppositePairByHeading(왕복분리 짝 링크 heading 교정)은 이 값을 필요로 한다 — 지워진 채
	//   넘기면 그 교정 자체가 통째로 비활성화된다. 지우기 전에 저장해둔 nOrigAngle 로 되살려 넘긴다
	//   (2026-08-24 최정우 추가 — 실측 000376_20260819094414 M79: Continue 그래프탐색이 반경 안에서
	//   후보를 못 찾아 완전실패 → 여기 Begin 폴백으로 떨어졌는데, heading 이 지워진 채 호출되는
	//   바람에 짝 링크(2040423603↔2040423501) heading 역방향 교정이 아예 시도조차 안 되고 순수
	//   거리(9m)만으로 반대편이 채택돼 개방형 톨게이트 오과금까지 발생)
	stMapMatchInput.nAngle = nOrigAngle;
	FillMatchTraceCtx(stTraceCtx, m_nThreadId, stRawLogInfo, stMapMatchInput, qwPrevLinkId, false, pstAltCtx);
	if (m_pcMapMatch->BeginMapMatch(stMapMatchInput, pstMatchLinkInfo, &stTraceCtx)
		// FixOppositePairByHeading 은 짝 링크가 반경 안 후보 목록에 있을 때만 교정한다 — 짝 링크가
		//   반경 밖(구간이 이미 끝난 지점 등)이라 목록에 없으면 교정이 안 먹고 역방향 링크가 그대로
		//   채택된다. 이 잔여 케이스는 아예 매칭 실패로 처리해 SKIP·widen-retry 로 넘긴다 —
		//   "확신 없는 매칭보다 SKIP" 원칙 (2026-08-24 최정우 추가)
		&& !m_pcMapMatch->IsAntiHeadingOpposite(pstMatchLinkInfo->qwLinkID, nOrigAngle, stMapMatchInput.nSpeed))
	{
		qwInOutLinkID = pstMatchLinkInfo->qwLinkID;
		// 직전 세션 링크가 있었는데도(Continue 시도 대상) 여기(Begin 폴백)로 확정됐다는 것은
		//   위상 그래프상 직전 링크와의 연결을 못 찾았다는 뜻 — 호출측 이동거리 타당성 검사 대상 표시
		//   (2026-09-04 최정우 추가)
		pstMatchLinkInfo->bContinueFallback = (qwPrevLinkId != 0);
		LOGFMTD("[#%02d] begin map match ok!device=[%s] link=[%llu]",
			m_nThreadId, stRawLogInfo.szDeviceKey,
			static_cast<unsigned long long>(qwInOutLinkID));
		return true;
	}

	return false;
}

/**
 * @brief 두 좌표를 잇는 진행 방위각 계산 (0=북, 시계방향 0~359도)
 * @param[in] stMatchPt 기준(매칭) X,Y 좌표 — 좌표정수(도×360000)
 * @param[in] stPoint 대상(요청) X,Y 좌표 — 좌표정수(도×360000)
 * @param[out] pnHeading 진행 각도(방위각)
 * @return true(성공), false(두 좌표가 같아 방향을 정의할 수 없음)
 * @remark
 * \t[미사용 경고, 2026-09-21 최정우 확인] 이 함수는 전 소스에서 호출부가 0 이다(정의만 존재).
 * \t실제로 쓰이는 방위각 계산은 CGISUtil::GetDirAngle() / GetDirAngleDegree() 이며,
 * \t둘 다 2026-08-26 에 BearingDegScaled() 로 교체돼 cos(위도) 보정이 들어가 있다.
 * \t삭제하지 않고 남기되(이력 추적 방침) **되살릴 거면 아래 수정 내역을 반드시 볼 것.**
 *
 * \t[버그 수정, 2026-09-21 최정우] 종전 구현에는 결함이 2 개 있어 되살리는 즉시 오작동했다.
 * \t  (1) **방위각이 대각선 반사돼 나왔다.** 종전 코드의 분기별 계산 결과는 이미 방위각
 * \t      (atan(Δx/Δy) = 북쪽 기준 시계방향)인데, 마지막에 "수학각도→방위각" 변환인
 * \t      `nHeading = 90 - nHeading;` 을 한 번 더 걸었다. 그 결과 정북(0)이 90(정동)으로,
 * \t      정동(90)이 0(정북)으로 뒤집혔다 — 45·135·225·315도에서만 우연히 값이 일치해
 * \t      눈으로 검산하면 맞는 것처럼 보인다.
 * \t  (2) **경도축 cos(위도) 보정 누락.** 좌표가 위·경도(도×360000)라 Δx 를 그대로 쓰면
 * \t      위도 37°대에서 동서 성분이 약 1.26배 부풀려진다(GISUtil.cpp BearingDegScaled 주석의
 * \t      실측 사례: 오차 4.74도가 154m 세그먼트에서 11.9m 옆으로 밀림).
 * \t두 결함 모두 검증된 공용 구현으로 위임해 해소한다. 호출부가 없어 동작 변화는 없다.
*/
bool CProcessManager::GetDirAzimuth(POINT& stMatchPt, POINT& stPoint, sint16 *pnHeading)
{
	if (pnHeading == nullptr)
		return false;

	// 두 점이 같으면 방향이 정의되지 않는다 — 종전 구현의 마지막 else(return false) 와 동일 계약
	if ((stPoint.dfX == stMatchPt.dfX) && (stPoint.dfY == stMatchPt.dfY))
		return false;

	*pnHeading = m_cGISUtil.GetDirAngleDegree(stMatchPt, stPoint);
	return true;
}

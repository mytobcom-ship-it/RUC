/**
 * @file MapMatch.cpp
 * @brief 맵매칭 클래스 소스 파일
*/

#include "MapMatch.h"

// 에러 코드별 메시지
CODE_ENTRY ErrorCodeTable[] = 
{
	{NO_ERROR,					"오류 없음"}, 
	{INVALID_COORDTYPE,			"측지계 코드 오류"}, 
	{INVALID_COORDINATE,		"좌표범위 초과"}, 
	{INVALID_ANGLE,				"방위각 오류"}, 
	{INVALID_ROADTYPE,			"도로등급 오류"}, 
	{INVALID_SEARCHRADIUS,		"검색 반경 오류"}, 
	{INVALID_LINKID,			"LinkID 오류"}, 
	{INVALID_RESULTCOUNT,		"응답 개수 오류"}, 
	{INVALID_SEARCHSTEP,		"탐색 단계 오류"}, 
	{NOT_FOUND_GRIDINFO,		"GRID 검색 실패"}, 
	{NOT_FOUND_LINKID,			"LinkID 검색 실패"}, 
	{MAP_MATCH_FAIL,			"맵매칭 실패"},
	{NOT_LOADED_MAPDATA,		"지도 데이터 미로드"}		// (2026-09-15 최정우 추가)
};

/**
 * @brief 생성자
*/
CMapMatch::CMapMatch() : 
	m_pcDataLoader(nullptr)
{
}

/**
 * @brief 소멸자
*/
CMapMatch::~CMapMatch()
{
}

/**
 * @brief 데이터 초기화
 * @param[in] pcDataLoader 형상 데이터(link.psf) 로더. nullptr 이거나 아직 로드 전이면 실패
 *            (2026-10-06 최정우 주석 수정 — 파라미터 이름·설명이 비어 있었다)
 * @return true(성공), false(실패)
*/
bool CMapMatch::Initialize(CDataLoader *pcDataLoader)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((pcDataLoader == nullptr) || (!pcDataLoader->IsLoad()))
		return false;

	m_pcDataLoader = pcDataLoader;
	return true;
}

/**
 * @brief 연속 맵매칭 고도 보조 점수 설정 (config [mapmatch] alt_gap/alt_penalty/alt_weight/alt_slope)
 *        (2026-10-06 최정우 주석 수정 — 종전 "altitude_*" 는 2026-07-21 키 이름 변경 전 표기)
 * @param[in] stAltConfig 고도 점수 설정
 * @return void
*/
void CMapMatch::SetAltitudeConfig(const ALTITUDE_SCORE_CONFIG& stAltConfig)
{
	// 연속 맵매칭 모듈에 고도 보조 점수 설정 전달 (2026-07-08 최정우 주석 추가)
	m_cContinueMapMatch.SetAltitudeConfig(stAltConfig);
}

/**
 * @brief 초기 맵매칭
 * @param[in] stMapMatchInput 초기 맵매칭 입력 정보 (값 전달 — 내부 좌표 변환이 호출측에 안 번진다)
 * @param[out] pstMatchLinkInfo 초기 맵매칭 응답 정보
 * @param[in] pstTraceCtx 매칭 트레이스 컨텍스트 (nullptr=미기록)
 *            (2026-09-21 최정우 주석 보완 — 설명이 없던 파라미터)
 * @return true(성공), false(실패)
*/
bool CMapMatch::BeginMapMatch(MAP_MATCH_INPUT stMapMatchInput, 
		PMATCH_LINK_INFO pstMatchLinkInfo, PMATCH_TRACE_CTX pstTraceCtx)
{
	uint16 wErrorCode = NO_ERROR;
	enum eCoordinateType eCoordType = static_cast<enum eCoordinateType>(stMapMatchInput.nCoordinateType);
	sint16 nRadius = stMapMatchInput.nRadius;
	double dfX = stMapMatchInput.dfX;
	double dfY = stMapMatchInput.dfY;
	sint16 nAngle = stMapMatchInput.nAngle;

	// 공통 맵 매칭 요청 입력 정보 유효성 검사
	pstMatchLinkInfo->wErrorCode = wErrorCode;
	if (!IsValidCommonRequestValue(eCoordType, nRadius, dfX, dfY, nAngle, pstMatchLinkInfo))
		return false;

	// 초기 맵매칭 시작
	SGMT_MATCH_INPUT stSgmtMatchInput;
	MATCH_ENTRY stMatchEntry;
	
	stSgmtMatchInput.stPoint.dfX = dfX;
	stSgmtMatchInput.stPoint.dfY = dfY;
	stSgmtMatchInput.nRadius = nRadius;
	stSgmtMatchInput.nDirAng = nAngle;
	stSgmtMatchInput.nSpeed = stMapMatchInput.nSpeed;		// 방위각 가중치 적응용(속도) (2026-07-08 최정우 추가)

	// 초기(GRID 기반) 맵매칭 엔진 호출 — 연속실패 재검색 시 직전 성공 링크 연결성 편향 전달 (2026-07-15 최정우 수정)
	if (!m_cBeginMapMatch.StartMapMatch(m_pcDataLoader, stSgmtMatchInput, &wErrorCode, &stMatchEntry, pstTraceCtx, stMapMatchInput.qwBiasLinkID))
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		if (stMatchEntry.dfIntersectLenSgmt >= 0.0)
			pstMatchLinkInfo->dfIntersectLenSgmt = stMatchEntry.dfIntersectLenSgmt;
		return false;
	}

	// 매칭 결과를 응답 구조체로 변환·좌표 역스케일 (2026-07-08 최정우 주석 추가)
	return SetResponseValue(wErrorCode, stMatchEntry, pstMatchLinkInfo);
}

/**
 * @brief 짝 링크(qwOppositeLinkID)가 있는 링크가 heading 과 거의 정반대(MM_OPP_FIX_REV_DEG 이상)로
 *        채택됐는지 판정 — CBeginMapMatch::IsAntiHeadingOpposite() 위임
 * @param[in] qwLinkID 판정 대상 링크 ID
 * @param[in] nHeading GPS 방위각(도, NO_ANGLE=판정 안 함)
 * @param[in] nSpeed GPS 속도(km/h, NO_SPEED 또는 MM_OPP_FIX_MIN_SPEED 미만이면 판정 안 함)
 * @return true(짝 링크가 있는데 heading 역방향으로 채택 — 신뢰 불가), false(그 외)
 * @remark 링크 방위각은 시작 노드→종료 노드 직선 방향이다(곡선 링크의 중간 형상은 보지 않음)
 *         (2026-10-06 최정우 주석 추가)
*/
bool CMapMatch::IsAntiHeadingOpposite(uint64 qwLinkID, sint16 nHeading, sint16 nSpeed)
{
	return m_cBeginMapMatch.IsAntiHeadingOpposite(qwLinkID, nHeading, nSpeed);
}

/**
 * @brief 반경 무시 기하 최근접 Begin — 진단반경 초과 SKIP 참고용 (2026-07-10 최정우 수정)
 * @remark MATCHED 아님. MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)만 확보.
 * @param[in] stMapMatchInput 맵매칭 입력 (값 전달). nRadius·nAngle 은 무시되고 각각
 *            MM_DIAG_RADIUS·NO_ANGLE 로 대체된다
 * @param[out] pstMatchLinkInfo 최근접 결과(좌표는 도 단위로 역스케일) 또는 에러 코드·메시지
 * @return true(최근접 1건 확보), false(입력 오류·지도 미적재·후보 없음)
 *         (2026-10-06 최정우 주석 추가 — @param/@return 이 없었다)
*/
bool CMapMatch::BeginGeomNearest(MAP_MATCH_INPUT stMapMatchInput, PMATCH_LINK_INFO pstMatchLinkInfo)
{
	uint16 wErrorCode = NO_ERROR;
	enum eCoordinateType eCoordType = static_cast<enum eCoordinateType>(stMapMatchInput.nCoordinateType);
	sint16 nRadius = static_cast<sint16>(MM_DIAG_RADIUS);
	double dfX = stMapMatchInput.dfX;
	double dfY = stMapMatchInput.dfY;
	const sint16 nAngle = NO_ANGLE;

	pstMatchLinkInfo->wErrorCode = wErrorCode;
	if (!IsValidCommonRequestValue(eCoordType, nRadius, dfX, dfY, nAngle, pstMatchLinkInfo))
		return false;

	SGMT_MATCH_INPUT stSgmtMatchInput;
	MATCH_ENTRY stMatchEntry;

	stSgmtMatchInput.stPoint.dfX = dfX;
	stSgmtMatchInput.stPoint.dfY = dfY;
	stSgmtMatchInput.nRadius = nRadius;
	stSgmtMatchInput.nDirAng = NO_ANGLE;
	stSgmtMatchInput.nSpeed = stMapMatchInput.nSpeed;

	if (!m_cBeginMapMatch.FindGeomNearest(m_pcDataLoader, stSgmtMatchInput, &wErrorCode, &stMatchEntry))
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		strcpy(pstMatchLinkInfo->szErrorMsg,
			m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		if (stMatchEntry.dfIntersectLenSgmt >= 0.0)
			pstMatchLinkInfo->dfIntersectLenSgmt = stMatchEntry.dfIntersectLenSgmt;
		return false;
	}

	return SetResponseValue(wErrorCode, stMatchEntry, pstMatchLinkInfo);
}

/**
 * @brief 연속 맵매칭
 * @param[in] stMapMatchInput 연속 맵매칭 입력 정보 (값 전달)
 * @param[out] pstMatchLinkInfo 연속 맵매칭 응답 정보
 * @param[in] pstTraceCtx 매칭 트레이스 컨텍스트 (nullptr=미기록)
 *            (2026-09-21 최정우 주석 보완 — 설명이 없던 파라미터)
 * @return true(성공), false(실패)
*/
bool CMapMatch::ContinueMapMatch(MAP_MATCH_INPUT stMapMatchInput, 
		PMATCH_LINK_INFO pstMatchLinkInfo, PMATCH_TRACE_CTX pstTraceCtx)
{
	uint16 wErrorCode = NO_ERROR;
	enum eCoordinateType eCoordType = static_cast<enum eCoordinateType>(stMapMatchInput.nCoordinateType);
	sint16 nRadius = stMapMatchInput.nRadius;
	double dfX = stMapMatchInput.dfX;
	double dfY = stMapMatchInput.dfY;
	sint16 nAngle = stMapMatchInput.nAngle;
	uint64 qwLinkID = stMapMatchInput.qwLinkID;
	// [2026-09-21 최정우 보완] m_pcDataLoader 를 검사 없이 역참조하고 있었다 — 같은 클래스의
	//   BeginMapMatch()/BeginGeomNearest() 는 하위 StartMapMatch()/FindGeomNearest() 안에서
	//   nullptr 검사에 걸려 NOT_LOADED_MAPDATA 로 정상 실패하는데, 이 경로만 그 전에 먼저
	//   GetSearchStep() 을 부르기 때문에 **검사에 닿기도 전에 죽는다**. Initialize() 가 실패하면
	//   CProcessManager 가 인스턴스를 폐기하므로 현재 호출 경로에서는 발생하지 않지만,
	//   같은 클래스 안에서 방어 수준이 갈리는 것은 그 자체로 함정이다.
	if (m_pcDataLoader == nullptr)
	{
		memset(pstMatchLinkInfo, 0, MATCH_LINK_INFO_SIZE);
		pstMatchLinkInfo->wErrorCode = NOT_LOADED_MAPDATA;
		strcpy(pstMatchLinkInfo->szErrorMsg,
			m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		LOGFMTE("continue map match called before map data loaded!");
		return false;
	}
	// config에서 연속 탐색 depth(검색 단계) 조회 (2026-07-08 최정우 주석 추가)
	sint16 nSearchStep = m_pcDataLoader->GetSearchStep();

	// (B) 공백 적응: 직전 매칭점→현재 이동거리 클수록 탐색 depth 확대 (2026-07-15 최정우 추가)
	//   공백(SKIP 연속 등)으로 차량이 여러 링크 전진했을 수 있어, 이동거리 MM_STEP_EXTEND_DIST 마다 depth +1 (최대 +MM_STEP_EXTEND_MAX)
	if (stMapMatchInput.dfHorizMove > 0.0)
	{
		int nExtraStep = static_cast<int>(stMapMatchInput.dfHorizMove / MM_STEP_EXTEND_DIST);
		if (nExtraStep > MM_STEP_EXTEND_MAX)
			nExtraStep = MM_STEP_EXTEND_MAX;
		nSearchStep = static_cast<sint16>(nSearchStep + nExtraStep);
	}

	// 공통 맵 매칭 요청 입력 정보 유효성 검사
	pstMatchLinkInfo->wErrorCode = wErrorCode;
	if (!IsValidCommonRequestValue(eCoordType, nRadius, dfX, dfY, nAngle, pstMatchLinkInfo))
		return false;
	
	// 연속 탐색 단계 유효성 검사 (2026-07-08 최정우 주석 추가)
	if (!IsValidSearchStep(nSearchStep))
		return false;

	// 연속 맵매칭 시작
	SGMT_MATCH_INPUT stSgmtMatchInput;
	MATCH_ENTRY stMatchEntry;
	// 직전 확정 링크부터 이번 확정 링크까지 실제 경유한 링크 목록 — 게이트/구역 판정용
	//   (2026-08-20 최정우 추가). 아래 Begin 대체 분기에서 Begin이 채택되면 무효화됨
	//   (Begin은 그래프 경로 개념이 없어 경유 링크를 모름 — bUsedContinuePath 로 표시)
	vector<uint64> vtContinuePath;
	bool bUsedContinuePath = true;
	// Continue 결과가 아래 병행 Begin 재평가로 덮어써졌는지 — 덮어써지면 위상(그래프) 검증 없이
	//   순수 근접 거리만으로 채택된 것이므로, 호출측 이동거리 타당성 검사(RawLogWorker
	//   IsFallbackJumpImplausible) 대상 표시용 (2026-09-04 최정우 추가)
	bool bBeginOverrode = false;
	// Continue depth 탐색(maxstep 이내)으로 실제 방문한 링크 UID 집합 — 아래 Begin 병행폴백에서
	//   "진짜 갈림길(형제) 링크"와 "그래프상 전혀 무관한 링크"를 구분하는 데 재사용한다.
	//   새 거리 임계값을 따로 두지 않고 기존 maxstep 탐색 결과를 그대로 활용 (2026-08-21 최정우 추가)
	set<uint64> setContinueSearchHistory;

	stSgmtMatchInput.stPoint.dfX = dfX;
	stSgmtMatchInput.stPoint.dfY = dfY;
	stSgmtMatchInput.nRadius = nRadius;
	stSgmtMatchInput.nDirAng = nAngle;
	stSgmtMatchInput.nSpeed = stMapMatchInput.nSpeed;		// 방위각 가중치 적응용(속도) (2026-07-08 최정우 추가)
	// 연속 맵매칭 고도 컨텍스트 — 세션 앵커·현재 GPS 고도 (시작 미전달)
	stSgmtMatchInput.nAltitudeM = stMapMatchInput.nAltitudeM;
	stSgmtMatchInput.nPrevAltitude = stMapMatchInput.nPrevAltitude;
	stSgmtMatchInput.nPrevRoadType = stMapMatchInput.nPrevRoadType;
	stSgmtMatchInput.nDriveStatus = stMapMatchInput.nDriveStatus;
	stSgmtMatchInput.dfHorizMove = stMapMatchInput.dfHorizMove;
	stSgmtMatchInput.bUseAltScore = stMapMatchInput.bUseAltScore;
	// 역행 페널티 컨텍스트 — qwPrevLinkID 는 ContinueMapMatch::StartMapMatch 내부에서 세팅 (2026-07-20 최정우 추가)
	stSgmtMatchInput.dfPrevLinkPos = stMapMatchInput.dfPrevLinkPos;
	stSgmtMatchInput.bHasPrevLinkPos = stMapMatchInput.bHasPrevLinkPos;
	// 같은 링크 노이즈 보정 기준점(직전 신뢰 매칭 좌표, WGS84) — StartMapMatch 내부에서
	//   내부 스케일(*360000)로 변환됨 (2026-07-22 최정우 추가)
	stSgmtMatchInput.bHasPrevMatchPos = stMapMatchInput.bHasPrevMatchPos;	// (2026-08-23 최정우 추가)
	stSgmtMatchInput.dfPrevMatchX = stMapMatchInput.dfPrevMatchX;
	stSgmtMatchInput.dfPrevMatchY = stMapMatchInput.dfPrevMatchY;
	stSgmtMatchInput.bSameRawAndHeadingAsPrev = stMapMatchInput.bSameRawAndHeadingAsPrev;	// (2026-09-02 최정우 추가)

	// 연속(링크 그래프) 맵매칭 엔진 호출 (2026-07-08 최정우 주석 추가)
	if (!m_cContinueMapMatch.StartMapMatch(m_pcDataLoader, stSgmtMatchInput, qwLinkID, nSearchStep, &wErrorCode, &stMatchEntry, pstTraceCtx, &vtContinuePath, &setContinueSearchHistory))
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		if (stMatchEntry.dfIntersectLenSgmt >= 0.0)
			pstMatchLinkInfo->dfIntersectLenSgmt = stMatchEntry.dfIntersectLenSgmt;
		return false;
	}

	// Continue 결과 품질이 나쁘면(방위각 비용이 상한 도달) Begin도 병행 실행해 더 나은 쪽 채택 (2026-07-18 최정우 추가)
	//   갈림길(직전 링크와 같은 시작 노드에서 갈라지는 형제 링크)은 depth 그래프가 직전 링크의
	//   끝(t_node) 이후로만 확장돼 구조적으로 도달 불가 — Begin은 반경 기반이라 그래프 제약이 없어 보완됨.
	//   연결성 편향(qwBiasLinkID)은 일부러 안 줌: 그 편향 자체가 forward-only 그래프와 같은 가정이라
	//   갈림길 형제 링크에 다시 페널티(MM_CONNECT_PENALTY)를 물려 이 보완 목적을 무력화하기 때문.
	//   120° 방위각 하드컷(MM_DIR_MAX_DEG)이 나란한/반대방향 도로 오매칭은 이미 차단.
	//   [정정] 위 한 줄은 사실과 다르다. ① GISUtil::SgmtMatch 는 세그먼트 정·역 두 방향 중 더 가까운
	//   쪽 각도차를 쓰므로(2026-07-16) 그 값은 최대 90° 라 120° 하드컷이 구조적으로 발동하지 않고,
	//   ② 여기서 부르는 Begin 은 bIgnoreHeading=true(2026-08-19)라 각도 판정 자체를 하지 않는다.
	//   반대방향 차단은 실제로는 아래 IsAntiHeadingOpposite 거부권과 Continue 쪽 역방향 적합
	//   후보 제외(ContinueMapMatch.cpp LinkSgmtMapMatch, 짝 링크 보유 시)가 맡는다
	//   (2026-10-06 최정우 주석 수정)
	//   dfAngleCost는 (거리+cap)-거리 형태로 역산되어 부동소수점 반올림 오차가 있을 수 있어
	//   허용오차(0.01m) 적용 (2026-07-18 최정우 수정)
	//   2026-08-21 수정 — 편향을 안 주다 보니 그래프상 전혀 무관한(TURN_INFO로 연결 안 된) 링크도
	//   순수 거리만으로 이겨서 채택되는 사례 실측 확인(trip 000376_20260819094414 seq54,
	//   2040424301->2040425201 — 두 링크는 52m 떨어진 별개 노드라 TURNINFO 연결이 원래 없음).
	//   Begin 후보가 setContinueSearchHistory(Continue가 이미 maxstep 이내에서 탐색해본 링크
	//   집합)에 있을 때만 채택하도록 제한 — "진짜 갈림길 형제"는 애초에 이 집합 안에 있어야
	//   Continue 후보로도 한 번은 걸러졌을 것이므로 기존 구제 목적은 그대로 유지되고, 탐색
	//   범위 밖의 무관한 링크만 차단된다. 새 거리 임계값을 따로 두지 않고 기존 maxstep 탐색
	//   결과를 그대로 재사용.
	//   2026-09-04 수정 — 이 구제는 "짧은 크로스오버 지선으로 1틱 튐"(교차로 한복판) 전용인데
	//   실제 발동 조건엔 이동거리가 전혀 없어, 실측 000376_20260826150010 seq405(직전 확정
	//   위치 대비 raw GPS 40m/3초 이동 — 실제로는 멀리 이동한 상황)에서도 그대로 발동해 Continue가
	//   위상 그래프로 4-hop 확장해 찾은 후보(2520216300, 방위각만 안 맞음)를, 위상 검증이 전혀
	//   없는 순수 거리 기반 Begin 후보가 덮어써버렸다. dfHorizMove(직전 확정 위치→현재 GPS 이동거리)가
	//   MM_NODE_BRIDGE_MAX_M(15m, 교차로 근접 브릿지 인정 거리와 동일 척도) 이내일 때만 — 즉 원래
	//   설계 의도대로 "짧게 튄" 경우에만 — 이 구제를 적용한다.
	if ((stMatchEntry.dfAngleCost >= (MM_DIR_MAX_PENALTY - 0.01))
		&& (stMapMatchInput.dfHorizMove <= MM_NODE_BRIDGE_MAX_M))
	{
		SGMT_MATCH_INPUT stBeginSgmtMatchInput;
		MATCH_ENTRY stBeginMatchEntry;
		uint16 wBeginErrorCode = NO_ERROR;
		// Begin은 bIgnoreHeading이라 bReverseSuspect를 절대 계산하지 않는다(항상 false) — 아래서
		//   Begin이 채택되면 Continue가 이미 정확히 감지한 역행 판정을 무조건 지워버리는 문제가
		//   있었다(실측 000376_20260826150010 seq131 — Continue가 suspect=1로 정확히 감지했는데,
		//   같은 링크를 heading 없이 재평가한 Begin이 더 싸다는 이유로(각도비용이 없으니까) 통째로
		//   대체해 역행이 사라짐). Begin이 채택한 링크가 Continue와 "다른" 링크(진짜 갈림길 구제
		//   목적)라면 원래 의도대로 그대로 두고, "같은" 링크를 재확인한 것뿐이라면 좌표/비용만
		//   Begin 걸 쓰고 역행 판정은 Continue 걸 보존한다 (2026-09-03 최정우 추가)
		const uint64 qwContinueLinkID = stMatchEntry.qwLinkID;
		const bool bContinueReverseSuspect = stMatchEntry.bReverseSuspect;

		stBeginSgmtMatchInput.stPoint.dfX = dfX;
		stBeginSgmtMatchInput.stPoint.dfY = dfY;
		stBeginSgmtMatchInput.nRadius = nRadius;
		stBeginSgmtMatchInput.nDirAng = nAngle;
		stBeginSgmtMatchInput.nSpeed = stMapMatchInput.nSpeed;

		// Begin 은 bIgnoreHeading 으로 순수 거리만 보므로, 짝 링크(qwOppositeLinkID)가 있는 링크를
		//   heading 역방향으로 채택했을 수 있다 — FixOppositePairByHeading 이 후보 목록에서 짝 링크를
		//   찾아 앞으로 당겨주지만, 짝 링크가 반경 밖(구간이 이미 끝난 지점 등)이라 목록에 없으면
		//   교정이 안 먹고 역방향 링크가 그대로 채택된 채 여기로 온다. 이 경우 Continue 결과를
		//   대체하지 않는다 — Continue 쪽엔 이미 동일한 반대편 역방향 차단(ContinueMapMatch.cpp)이
		//   있는데, Begin 이 병행폴백으로 그걸 우회해 덮어써버리는 구멍이었다(실측
		//   000376_20260819094414 M79 — 2040423501→2040423603[반대편, heading 역방향]→2040423503 순
		//   주행인데 Continue 각도비용 상한으로 Begin 폴백이 발동, 짝 링크 2040423501 은 이미 그
		//   구간을 지나 반경 밖이라 FixOppositePairByHeading 도 못 잡고 2040423603 이 그대로 채택돼
		//   개방형 톨게이트 오과금까지 발생) (2026-08-24 최정우 추가)
		// pstTraceCtx 를 Continue 와 같은 것으로 넘기므로, 트레이스 로그(LOGFMTD)에 이 병행 Begin 의
		//   후보 블록이 mode=[Continue] 로 한 번 더 찍히고 nMatchedStep 이 0 으로 덮어써진다 —
		//   채택 여부와 무관하다. 로그 해석 시 주의(매칭 결과에는 영향 없음) (2026-10-06 최정우 주석 추가)
		if (m_cBeginMapMatch.StartMapMatch(m_pcDataLoader, stBeginSgmtMatchInput, &wBeginErrorCode,
				&stBeginMatchEntry, pstTraceCtx, 0)
			&& (stBeginMatchEntry.dfCost < stMatchEntry.dfCost)
			&& (setContinueSearchHistory.find(stBeginMatchEntry.qwLinkID) != setContinueSearchHistory.end())
			&& !m_cBeginMapMatch.IsAntiHeadingOpposite(stBeginMatchEntry.qwLinkID, nAngle, stMapMatchInput.nSpeed))
		{
			stMatchEntry = stBeginMatchEntry;
			bUsedContinuePath = false;		// Begin 채택 — Continue 경유 경로는 무효 (2026-08-20 최정우 추가)
			bBeginOverrode = true;			// (2026-09-04 최정우 추가)
			if (stBeginMatchEntry.qwLinkID == qwContinueLinkID)
				stMatchEntry.bReverseSuspect = bContinueReverseSuspect;
		}
	}

	// 매칭 결과를 응답 구조체로 변환·좌표 역스케일 (2026-07-08 최정우 주석 추가)
	if (!SetResponseValue(wErrorCode, stMatchEntry, pstMatchLinkInfo))
		return false;

	// Begin 재평가로 덮어써진 결과 표시 — 위상 그래프 검증이 아니라 순수 근접 거리로 채택됐다는 뜻.
	//   실측 000376_20260826150010 seq405: Continue가 depth 4까지 확장해 2520216300 을 찾았지만
	//   방위각 비용 상한(MM_DIR_MAX_PENALTY) 도달로 병행 Begin이 heading 무시·순수 거리(18m)로
	//   재평가해 더 싼 비용으로 덮어썼다. 그 결과 직전 확정 위치(강릉 주정차구역 이탈 직후) 대비
	//   3초/44m(≈53km/h, 신고속도 11~14km/h) 점프가 위상 검증도 거리 타당성 검증도 없이 그대로
	//   MATCHED 확정되는 구멍이었다 — ProcessManager::AttemptMatch 의 Continue 완전실패→Begin
	//   폴백(bContinueFallback)과 위상은 다르지만 "근접 거리만으로 채택" 이라는 성질은 동일해
	//   같은 플래그를 재사용한다 (2026-09-04 최정우 추가)
	if (bBeginOverrode)
		pstMatchLinkInfo->bContinueFallback = true;

	// SetResponseValue가 채운 기본 경로(최종 링크 1개)를, Continue가 실제 경유한 전체 경로로 교체
	//   (2026-08-20 최정우 추가) — 배열 크기(MATCH_LINK_INFO_MAX_PATH) 초과분은 자름
	//   교체 전 "그럴듯함" 검증 — 경로 링크 길이 합이 실제 이동거리(dfHorizMove) 대비 비정상적으로
	//   크면(MM_PATH_PLAUSIBLE_* 참고) 신뢰하지 않고 기본값(최종 링크 1개)을 그대로 둔다. 그래프
	//   탐색은 두 확정 링크를 잇는 "어떤" 경로만 찾을 뿐 실제 주행 여부는 검증하지 않아, 복잡한
	//   교차로에서 실제로 가지 않은 갈림길이 경유 링크로 잘못 포함될 수 있다 — 그 갈림길이 우연히
	//   과금구역 링크면 오등록으로 이어진다(2026-08-24 최정우 추가)
	//   2026-09-04 수정 — vtContinuePath 가 크기 1이어도 그 유일한 원소가 직전 세션 링크(qwLinkID)와
	//   같으면(=ReconstructPath 가 "확장 없이 그대로 확정"으로 기록한 경우, ContinueMapMatch.cpp
	//   ReconstructPath 주석 참고) 이건 "여러 링크를 거친 재구성 경로"가 아니라 "긴 링크 위를
	//   계속 달리는 중 링크 시작점부터 지금까지의 누적거리"일 뿐이다. 실측 000376_20260826150010
	//   seq46·seq77: 289m대 긴 링크 위를 정상 주행(raw 16.5~40.8m/3초) 중이었는데, 누적거리
	//   (235.5m·289.0m)를 그 틱의 이동거리로 오인해 절대속도 280~350km/h대로 오판정, SKIP시켰다.
	//   최초 시도(pstTraceCtx->nMatchedStep>0 가드)는 부정확했다 — StartMapMatch 의 nBestStep 은
	//   "실제 채택된 후보의 depth"가 아니라 "마지막으로 depth 확장을 시도해 후보가 발견된 depth"라,
	//   depth=0 후보가 그대로 채택돼도(경계클램프·각도부적합으로 확장은 했지만 depth=0이 그대로 이김)
	//   nMatchedStep 이 >0 으로 남아 위 오탐을 못 걸렀다. 대신 "재구성 경로 시작 링크가 직전 세션
	//   링크와 다른가"(=진짜 다른 링크로 그래프 확장됐는가)로 직접 판정한다.
	if (bUsedContinuePath && !vtContinuePath.empty() && (vtContinuePath.front() != qwLinkID))
	{
		double dfPathLenSum = 0.0;
		// 구간(hop)별 등록 제한속도 기준 최소 소요시간 합산 — MM_PATH_SPEEDLIMIT_MARGIN 주석 참고
		//   (2026-09-04 최정우 추가)
		double dfMinTimeSec = 0.0;
		const size_t nLastIdx = vtContinuePath.size() - 1;
		for (size_t i = 0; i < vtContinuePath.size(); ++i)
		{
			double dfHopLenM = 0.0;
			uint8 nHopMaxSpeed = 0;
			// 최종 확정 링크(경로의 마지막 원소)는 링크 시작점부터 끝까지가 아니라 "시작점부터
			//   실제 매핑된 지점까지"만 이동했다 — 전체 링크 길이(dfLen)를 그대로 더하면 링크가
			//   길수록(예: 400m대) 실제 이동거리와 무관하게 과대평가된다. 실측 확인: hops=1인데
			//   pathLen=416.3m로 나와 절대속도 500km/h로 오판정된 사례 다수(2026-09-04). 중간
			//   경유 링크는 처음부터 끝까지 통과한 게 맞아 전체 길이 그대로 쓴다 (2026-09-04 최정우 추가)
			if ((i == nLastIdx) && (vtContinuePath[i] == stMatchEntry.qwLinkID))
			{
				dfHopLenM = static_cast<double>(stMatchEntry.wLenFromLink) + stMatchEntry.dfSgmtMatchLen;
				nHopMaxSpeed = stMatchEntry.nMaxSpeed;
			}
			else
			{
				PLINK_INFO pstPathLink = m_pcDataLoader->GetLinkInfo(vtContinuePath[i]);
				if (pstPathLink != nullptr)
				{
					dfHopLenM = pstPathLink->dfLen;
					nHopMaxSpeed = pstPathLink->nMaxSpeed;
				}
			}
			dfPathLenSum += dfHopLenM;
			const double dfHopSpeedKmh = (nHopMaxSpeed > 0)
				? (static_cast<double>(nHopMaxSpeed) * MM_PATH_SPEEDLIMIT_MARGIN)
				: MM_PATH_SPEEDLIMIT_DEFAULT_KMH;
			dfMinTimeSec += (dfHopLenM / dfHopSpeedKmh) * 3.6;
		}
		double dfPlausibleMax = stMapMatchInput.dfHorizMove * MM_PATH_PLAUSIBLE_SCALE;
		if (dfPlausibleMax < MM_PATH_PLAUSIBLE_FLOOR_M)
			dfPlausibleMax = MM_PATH_PLAUSIBLE_FLOOR_M;

		if (dfPathLenSum <= dfPlausibleMax)
		{
			uint8 nCount = static_cast<uint8>(vtContinuePath.size() < MATCH_LINK_INFO_MAX_PATH ? vtContinuePath.size() : MATCH_LINK_INFO_MAX_PATH);
			for (uint8 i = 0; i < nCount; i++)
				pstMatchLinkInfo->aqwPathLinkIDs[i] = vtContinuePath[i];
			pstMatchLinkInfo->nPathLinkCount = nCount;
		}
		else
		{
			LOGFMTW("implausible reconstructed path discarded!pathLen=[%.1f]m horizMove=[%.1f]m plausibleMax=[%.1f]m hops=[%zu]",
				dfPathLenSum, stMapMatchInput.dfHorizMove, dfPlausibleMax, vtContinuePath.size());
		}

		// 위 배율식(dfHorizMove 상대)과 별개로 dfGapSec(경과초) 기준 절대 물리속도 검증 —
		//   MM_PATH_ABS_MAX_KMH 주석 참고. dfGapSec 계산 불가(직전 매칭 없음·역전 등, 음수)면
		//   판단 보류(2026-09-04 최정우 추가)
		if (stMapMatchInput.dfGapSec > 0.0)
		{
			double dfImpliedKmh = (dfPathLenSum / stMapMatchInput.dfGapSec) * 3.6;
			if (dfImpliedKmh > MM_PATH_ABS_MAX_KMH)
			{
				LOGFMTW("implausible reconstructed path abs speed!pathLen=[%.1f]m gapSec=[%.1f]s implied=[%.1f]km/h limit=[%.1f]km/h hops=[%zu]",
					dfPathLenSum, stMapMatchInput.dfGapSec, dfImpliedKmh, MM_PATH_ABS_MAX_KMH, vtContinuePath.size());
				pstMatchLinkInfo->bImplausiblePath = true;
			}

			// 위 평균속도 상한과 별개로, 구간(hop)별 등록 제한속도 기준 최소 소요시간 합(dfMinTimeSec)이
			//   실제 경과시간보다 크면 비현실적 — 평균은 상한 이내여도 유독 느린 구간(좁은 길 등)
			//   하나만으로 물리적으로 불가능한 경우를 잡는다. MM_PATH_SPEEDLIMIT_MARGIN 주석 참고
			//   (2026-09-04 최정우 추가, 사용자 지시)
			if (dfMinTimeSec > stMapMatchInput.dfGapSec)
			{
				LOGFMTW("implausible reconstructed path speed limit!pathLen=[%.1f]m gapSec=[%.1f]s minTimeSec=[%.1f]s hops=[%zu]",
					dfPathLenSum, stMapMatchInput.dfGapSec, dfMinTimeSec, vtContinuePath.size());
				pstMatchLinkInfo->bImplausibleSpeedLimit = true;
			}
		}

		// 재구성 경로(직전 신뢰 위치→이번 확정 위치)의 전체 진행방향이 현재 heading 과 크게 어긋나면
		//   비현실적 — 시간·거리(위 절대속도 검사)만으로는 "실제 주행 경로인가"를 못 잡는다. 예:
		//   4-hop 경로가 시간상으로는 가능해도, 그 경로의 전체 방향이 지금 차량이 향하는 방향과
		//   반대/직각이면 실제로 그 길을 달렸다고 보기 어렵다. heading·속도가 무의미한 저속/정지
		//   구간(예: 주정차구역 내 공터 이동)은 판단 근거가 없어 제외 — MM_OPP_FIX_MIN_SPEED 재사용
		//   (실측 기준: 시속 3km 미만은 heading 노이즈로 취급, IsAntiHeadingOpposite 와 동일 전제)
		//   (2026-09-04 최정우 추가, 사용자 지시)
		// ※ 현재 이 검사는 **발동하지 않는다** — 조건의 bHasPrevMatchPos 를 ProcessManager 가 전달하지
		//   않아 항상 false 다. 배선 시 교차로 회전·정차 후 출발 tick 만 SKIP 되는 오탐(발동 6회 전부
		//   hops=1)이 실측돼 미배선 유지로 확정됐다(ProcessManager.cpp 2026-09-16 주석 참고).
		//   살리려면 "hops>=2" 같은 가드가 먼저 필요하다 (2026-10-06 최정우 주석 추가)
		if ((stMapMatchInput.nAngle >= 0) && (stMapMatchInput.nSpeed >= 0)
			&& (stMapMatchInput.nSpeed >= MM_OPP_FIX_MIN_SPEED) && stMapMatchInput.bHasPrevMatchPos)
		{
			POINT stPathStart; stPathStart.dfX = stMapMatchInput.dfPrevMatchX; stPathStart.dfY = stMapMatchInput.dfPrevMatchY;
			POINT stPathEnd;   stPathEnd.dfX = pstMatchLinkInfo->dfMatchX;     stPathEnd.dfY = pstMatchLinkInfo->dfMatchY;
			// 시작·끝이 사실상 같은 점(정지에 가까움)이면 방향 자체가 노이즈라 판단 보류.
			//   RawLogWorker::HaversineMeters 와 동일한 표준 하버사인 공식(WGS84 도 단위 좌표 그대로,
			//   내부 스케일(*360000) 미적용) — 별도 인라인 계산(2026-09-04 최정우 추가)
			const double dfEarthR = 6378137.0;
			const double dfLat1 = stPathStart.dfY * M_PI / 180.0, dfLat2 = stPathEnd.dfY * M_PI / 180.0;
			const double dfDLat = (stPathEnd.dfY - stPathStart.dfY) * M_PI / 180.0;
			const double dfDLon = (stPathEnd.dfX - stPathStart.dfX) * M_PI / 180.0;
			const double dfA = sin(dfDLat/2)*sin(dfDLat/2) + cos(dfLat1)*cos(dfLat2)*sin(dfDLon/2)*sin(dfDLon/2);
			const double dfPathMoveM = 2.0 * dfEarthR * asin(sqrt(dfA));
			if (dfPathMoveM >= MM_CALC_MIN_DIST)
			{
				sint16 nPathBearing = m_cGISUtil.GetDirAngleDegree(stPathStart, stPathEnd);
				sint16 nHeading = stMapMatchInput.nAngle;
				sint16 nDirDiff = m_cGISUtil.GetAngleDiff(nPathBearing, nHeading);
				if (abs(nDirDiff) > MM_DIR_MAX_DEG)
				{
					LOGFMTW("implausible reconstructed path direction!pathBearing=[%d] heading=[%d] diff=[%d] limit=[%d] hops=[%zu]",
						static_cast<int>(nPathBearing), static_cast<int>(nHeading), static_cast<int>(nDirDiff),
						MM_DIR_MAX_DEG, vtContinuePath.size());
					pstMatchLinkInfo->bImplausibleDirection = true;
				}
			}
		}
	}

	return true;
}

/**
 * @brief 공통 맵 매칭 요청 입력 정보 유효성 검사
 * @param[in] eCoordType 측지계 코드
 * @param[in] nRadius 맵매칭 유효거리
 * @param[in,out] dfX X 좌표
 * @param[in,out] dfY Y 좌표
 * @param[in] nAngle 방위각
 * @param[out] pstMatchLinkInfo 에러 코드 및 에러 메시지. **진입 즉시 memset 으로 전부 지운다**
 *             (2026-09-21 최정우 주석 보완 — in/out 표기가 없었고, 호출 전에 채워둔 값이
 *             날아간다는 점이 드러나지 않았다)
 * @return true(성공), false(실패)
*/
bool CMapMatch::IsValidCommonRequestValue(enum eCoordinateType& eCoordType, 
		sint16& nRadius, double& dfX, double& dfY, sint16 nAngle, 
		PMATCH_LINK_INFO pstMatchLinkInfo)
{
	// 초기화
	memset(pstMatchLinkInfo, 0, MATCH_LINK_INFO_SIZE);

	// 측지계 코드 유효성 검사
	if (!IsValidCoordinateType(eCoordType))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_COORDTYPE;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	// 좌표 변환 및 유효성 검사
	if (!IsValidCoordinate(eCoordType, &dfX, &dfY))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_COORDINATE;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	// 맵매칭 유효 거리
	if (!IsValidSearchRadius(nRadius))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_SEARCHRADIUS;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	// 방위각 유효성 검사
	if (!IsValidAngle(nAngle))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_ANGLE;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	pstMatchLinkInfo->wErrorCode = NO_ERROR;
	// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
	strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
	return true;
}

/**
 * @brief 맵 매칭 결과값
 * @param[in] wErrorCode 에러 코드
 * @param[in] stMatchEntry 매칭 링크 정보
 * @param[out] pstMatchLinkInfo 결과 정보
 * @return true(성공), false(실패)
*/
bool CMapMatch::SetResponseValue(uint16 wErrorCode, MATCH_ENTRY stMatchEntry, 
		PMATCH_LINK_INFO pstMatchLinkInfo)
{
	memset(pstMatchLinkInfo, 0, MATCH_LINK_INFO_SIZE);

	if (wErrorCode != NO_ERROR)
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
	}
	else
	{
		// [2026-09-21 최정우 정리] 종전에는 여기서 똑같은 "코드 대입 + 메시지 strcpy" 를 두 번
		//   연달아 했다 — 이 else 는 wErrorCode == NO_ERROR 인 경우이므로 두 블록이 글자 그대로
		//   같은 일을 했다(둘째 블록의 `= NO_ERROR` 도 이미 들어 있는 값을 다시 넣는 것).
		//   읽는 사람이 "왜 두 번 하지? 사이에 뭔가 바뀌나?" 하고 멈추게 만들 뿐이라 한 번으로
		//   줄인다. 동작 변화 없음.
		pstMatchLinkInfo->wErrorCode = NO_ERROR;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));

		// 2026-07-10 최정우 주석 처리: MATCH_ENTRY 에만 존재하는 dfCost/dfAngleCost/dfAltAdj(24B) 때문에
		//   두 구조체 레이아웃이 어긋나, 통복사 시 qwLinkID 이후 필드(링크ID·도로유형·노드 등)가 전부 밀려
		//   쓰레기값이 됨 → 연속(Continue) 맵매칭이 항상 GetLinkInfo 실패로 무력화. 명시적 필드 복사로 교체.
		//memcpy(&pstMatchLinkInfo->dfMatchX, &stMatchEntry, MATCH_ENTRY_SIZE);
		//pstMatchLinkInfo->dfMatchX /= 360000.0;
		//pstMatchLinkInfo->dfMatchY /= 360000.0;
		//pstMatchLinkInfo->dfStNodeX /= 360000.0;
		//pstMatchLinkInfo->dfStNodeY /= 360000.0;
		//pstMatchLinkInfo->dfEdNodeX /= 360000.0;
		//pstMatchLinkInfo->dfEdNodeY /= 360000.0;

		// MATCH_ENTRY → MATCH_LINK_INFO 필드별 복사 (좌표·노드좌표는 /360000 역스케일) (2026-07-10 최정우 수정)
		pstMatchLinkInfo->dfMatchX			= stMatchEntry.dfMatchX / 360000.0;
		pstMatchLinkInfo->dfMatchY			= stMatchEntry.dfMatchY / 360000.0;
		pstMatchLinkInfo->dfSgmtMatchLen	= stMatchEntry.dfSgmtMatchLen;
		pstMatchLinkInfo->dfIntersectLenSgmt = stMatchEntry.dfIntersectLenSgmt;
		pstMatchLinkInfo->nDirAngleDiff		= stMatchEntry.nDirAngleDiff;
		pstMatchLinkInfo->qwLinkID			= stMatchEntry.qwLinkID;
		pstMatchLinkInfo->wLenFromLink		= stMatchEntry.wLenFromLink;
		pstMatchLinkInfo->nMaxSpeed			= stMatchEntry.nMaxSpeed;
		pstMatchLinkInfo->dfLen				= stMatchEntry.dfLen;
		pstMatchLinkInfo->nRoadRank			= stMatchEntry.nRoadRank;
		pstMatchLinkInfo->nConnect			= stMatchEntry.nConnect;
		pstMatchLinkInfo->nRoadType			= stMatchEntry.nRoadType;
		pstMatchLinkInfo->nLanes			= stMatchEntry.nLanes;
		memcpy(pstMatchLinkInfo->szRoadName, stMatchEntry.szRoadName, sizeof(pstMatchLinkInfo->szRoadName));
		pstMatchLinkInfo->qwStNodeID		= stMatchEntry.qwStNodeID;
		pstMatchLinkInfo->dfStNodeX			= stMatchEntry.dfStNodeX / 360000.0;
		pstMatchLinkInfo->dfStNodeY			= stMatchEntry.dfStNodeY / 360000.0;
		pstMatchLinkInfo->nStNodeType		= stMatchEntry.nStNodeType;
		pstMatchLinkInfo->qwEdNodeID		= stMatchEntry.qwEdNodeID;
		pstMatchLinkInfo->dfEdNodeX			= stMatchEntry.dfEdNodeX / 360000.0;
		pstMatchLinkInfo->dfEdNodeY			= stMatchEntry.dfEdNodeY / 360000.0;
		pstMatchLinkInfo->nEdNodeType		= stMatchEntry.nEdNodeType;
		// 경로(경유 링크) 기본값 — 링크 1개(qwLinkID) 뿐인 것으로 설정. Continue 매칭이 실제
		//   경유 링크가 더 있으면 CMapMatch::ContinueMapMatch() 가 호출 후 이 값을 덮어씀.
		//   Begin 매칭·경계선 경유가 없는 Continue 매칭·Begin으로 대체된 경우는 이 기본값 그대로
		//   사용(항상 최소 1개는 채워짐) (2026-08-20 최정우 추가)
		pstMatchLinkInfo->aqwPathLinkIDs[0] = stMatchEntry.qwLinkID;
		pstMatchLinkInfo->nPathLinkCount = 1;
		// 위치 역행 + heading 역방향 일치 — 연속역행(reverse_confirm) 스트릭 판정 전용 (2026-07-21 최정우 추가)
		pstMatchLinkInfo->bReverseSuspect	= stMatchEntry.bReverseSuspect;
		// 최종 확정 후보가 세그먼트 끝점(꺾임점)에 스냅됐고 GPS와 거리가 먼 경우 — 여러 GPS_SEQ 가
		//   같은 꺾임점으로 뭉개져 MATCH_LAT/LON 이 실제로는 계속 이동 중인데도 정지한 것처럼 보이는
		//   오탐(예: 주정차 오판) 방지용. bSgmtClamped 는 GISUtil::SgmtMatch 가 세그먼트 단위로
		//   판정한 신호라, 링크 전체 시작/끝이 아니라도(링크 중간 꺾임점) 잡힌다 — IsBoundaryClamped
		//   (ContinueMapMatch, 링크 전체 기준)와는 다른 신호이니 혼동 주의 (2026-07-21 최정우 추가)
		pstMatchLinkInfo->bClampLowConf = stMatchEntry.bSgmtClamped
			&& (stMatchEntry.dfIntersectLenSgmt > MM_CLAMP_SKIP_LEN)
			&& !stMatchEntry.bClampTrustedByHeading;
		// bClampTrustedByHeading — heading이 도로 방향과 잘 맞고 고속이면(GISUtil::SgmtMatch 판정)
		//   위 거리 초과여도 SKIP 강등을 취소 — 도로가 꺾이는 구간에서 GPS 궤적은 거의 직선인데
		//   도로만 꺾여 INTERSECT_LEN이 커지는 정상 케이스 구제 (2026-08-20 최정우 추가)
		// 같은 링크 역행인데 heading 없음/애매해 노이즈 단정 불가 — SKIP 격리용 (2026-07-22 최정우 추가)
		pstMatchLinkInfo->bAmbiguousReverse = stMatchEntry.bAmbiguousReverse;
	}

	return (wErrorCode == NO_ERROR) ? true : false;
}

/**
 * @brief 측지계 코드 유효성 검사
 * @param[in] eCoordType 측지계 코드
 * @return true(성공), false(실패)
*/
bool CMapMatch::IsValidCoordinateType(enum eCoordinateType& eCoordType)
{
	if ((eCoordType < EPSG3857) || (eCoordType > BESSELGEO))
		return false;

	return true;
}

/**
 * @brief 좌표 변환 및 유효성 검사
 * @param[in] eCoordType 측지계 코드
 * @param[in,out] dfX X 좌표
 * @param[in,out] dfY Y 좌표
 * @return true(성공), false(실패)
*/
bool CMapMatch::IsValidCoordinate(enum eCoordinateType& eCoordType, 
		double *dfX, double *dfY)
{
	// 좌표 변환
	if (!m_cCoordinate.ConvertCoordinateToWGS84GEO(eCoordType, dfX, dfY))
		return false;

	// 좌표 유효성 검사
	if (!m_cCoordinate.IsValidWGS84GEO(*dfX, *dfY))
		return false;

	return true;
}

/**
 * @brief 검색 반경 유효성 검사
 * @param[in] nRadius 검색 반경
 * @return true(성공), false(실패)
*/
bool CMapMatch::IsValidSearchRadius(sint16& nRadius)
{
	// [2026-09-21 최정우 수정] 상한이 리터럴 250 으로 박혀 있었는데, 이 값은 진단용 탐색 반경
	//   MM_DIAG_RADIUS(250m)와 **같아야만** 한다 — BeginGeomNearest() 는 nRadius 에
	//   MM_DIAG_RADIUS 를 그대로 넣고 이 검사를 통과해야 하기 때문이다. 상수를 251 이상으로
	//   올리는 순간 그 경로가 전부 INVALID_SEARCHRADIUS 로 조용히 실패한다(진단용 최근접
	//   좌표를 못 남겨 SKIP 행의 MATCH_LAT/LON·INTERSECT_LEN 이 통째로 비게 된다).
	//   현재 값이 서로 같으므로 동작 변화는 없고, 앞으로 함께 움직이도록 상수로 묶는다.
	if ((nRadius < 0) || (nRadius > MM_DIAG_RADIUS))
		return false;

	return true;
}

/**
 * @brief 방위각 유효성 검사
 * @param[in] nAngle 방위각
 * @return true(성공), false(실패)
*/
bool CMapMatch::IsValidAngle(sint16& nAngle)
{
	if (nAngle == NO_ANGLE) return true;
	if ((nAngle < NO_ANGLE) || (nAngle > 359))
		return false;

	return true;
}

/**
 * @brief 연속 맵매칭 탐색 depth(nSearchStep) 유효성 검사
 *        (2026-10-06 최정우 주석 수정 — 종전 "최대 연속 측위" 는 이 값의 의미와 달랐다. config
 *        [mapmatch] maxstep 에 공백 적응 확장(MM_STEP_EXTEND_*)을 더한 링크 그래프 탐색 깊이다)
 * @param[in] nSearchStep 연속 탐색 depth
 * @return true(성공), false(실패)
*/
bool CMapMatch::IsValidSearchStep(sint16& nSearchStep)
{
	// [경고 정리, 2026-09-15 최정우] 상한 검사(> 32767)는 sint16 의 최댓값이 32767 이라 절대
	//   성립하지 않는다(-Wtype-limits). 타입이 보장하는 조건이므로 제거하고 하한만 남긴다 —
	//   인자 타입을 더 넓은 정수로 바꾸게 되면 상한 검사를 다시 넣을 것.
	if (nSearchStep < 0)
		return false;

	return true;
}

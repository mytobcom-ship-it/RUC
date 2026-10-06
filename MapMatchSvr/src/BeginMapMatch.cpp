/**
 * @file BeginMapMatch.cpp
 * @brief 초기 맵매칭 클래스 소스 파일
*/
#include "BeginMapMatch.h"

/**
 * @brief 생성자
*/
CBeginMapMatch::CBeginMapMatch() : 
	m_pcDataLoader(nullptr)
{
}

/**
 * @brief 소멸자
*/
CBeginMapMatch::~CBeginMapMatch()
{
}

/**
 * @brief 링크 진행 방위각 (시작 노드 → 종료 노드)
 * @param[in] pstLinkInfo 링크 정보 (노드 좌표는 도×360000 스케일 정수)
 * @return 방위각(도, 0=북 시계방향, 0~359)
 * @remark 두 노드를 잇는 **직선** 방향이다 — 곡선·U자형 링크는 중간 형상과 크게 다를 수 있다.
 *         FixOppositePairByHeading·FixReverseLinkByAzimuth·IsAntiHeadingOpposite 의 판정 기준
 *         (2026-10-06 최정우 주석 추가)
*/
sint16 CBeginMapMatch::GetLinkAzimuth(PLINK_INFO pstLinkInfo)
{
	POINT stFrom;
	POINT stTo;
	stFrom.dfX = static_cast<double>(pstLinkInfo->dwStNodeX);
	stFrom.dfY = static_cast<double>(pstLinkInfo->dwStNodeY);
	stTo.dfX   = static_cast<double>(pstLinkInfo->dwEdNodeX);
	stTo.dfY   = static_cast<double>(pstLinkInfo->dwEdNodeY);
	return m_cGISUtil.GetDirAngleDegree(stFrom, stTo);
}

/**
 * @brief 왕복분리 짝 링크 heading 교정
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 (heading·속도)
 * @param[in,out] listMatchEntryList 비용순 정렬된 후보 목록 (교정 시 짝 링크를 맨 앞으로)
 * @remark
 * \t자세한 배경은 DataDefine.h 의 MM_OPP_FIX_* 주석 참고.
 * \t확신이 없으면(채택 링크·짝 링크 둘 다 애매) 아무것도 하지 않는다.
 * \t(2026-08-22 최정우 추가)
*/
void CBeginMapMatch::FixOppositePairByHeading(const SGMT_MATCH_INPUT& stSgmtMatchInput,
		list<MATCH_ENTRY>& listMatchEntryList)
{
	if (listMatchEntryList.empty())
		return;
	if (stSgmtMatchInput.nDirAng == NO_ANGLE)
		return;
	// 정지·저속 구간의 heading 은 신뢰할 수 없다
	if ((stSgmtMatchInput.nSpeed == NO_SPEED) || (stSgmtMatchInput.nSpeed < MM_OPP_FIX_MIN_SPEED))
		return;
	if (m_pcDataLoader == nullptr)
		return;

	const uint64 qwTopLinkID = listMatchEntryList.begin()->qwLinkID;
	PLINK_INFO pstTop = m_pcDataLoader->GetLinkInfo(qwTopLinkID);
	if ((pstTop == nullptr) || (pstTop->qwOppositeLinkID == 0))
		return;

	PLINK_INFO pstOpp = m_pcDataLoader->GetLinkInfo(pstTop->qwOppositeLinkID);
	if (pstOpp == nullptr)
		return;

	sint16 nHeading = stSgmtMatchInput.nDirAng;
	sint16 nTopAz = GetLinkAzimuth(pstTop);
	sint16 nOppAz = GetLinkAzimuth(pstOpp);

	// 채택 링크가 heading 과 거의 정반대가 아니면 교정 대상이 아니다
	if (abs(m_cGISUtil.GetAngleDiff(nTopAz, nHeading)) < MM_OPP_FIX_REV_DEG)
		return;
	// 짝 링크도 heading 과 안 맞으면 확신이 없으므로 건드리지 않는다
	if (abs(m_cGISUtil.GetAngleDiff(nOppAz, nHeading)) > MM_OPP_FIX_FWD_DEG)
		return;

	// 후보 목록에 짝 링크가 있으면 맨 앞으로 (반경 밖이면 후보에 없을 수 있다)
	for (list<MATCH_ENTRY>::iterator it = listMatchEntryList.begin();
			it != listMatchEntryList.end(); ++it)
	{
		if (it->qwLinkID != pstTop->qwOppositeLinkID)
			continue;

		LOGFMTI("begin opposite pair fix!link=[%lu]->[%lu], heading=[%d], az=[%d]->[%d], speed=[%d]",
			qwTopLinkID, pstTop->qwOppositeLinkID, nHeading, nTopAz, nOppAz, stSgmtMatchInput.nSpeed);
		MATCH_ENTRY stFixed = *it;
		listMatchEntryList.erase(it);
		listMatchEntryList.push_front(stFixed);
		return;
	}
}

/**
 * @brief 채택 링크가 heading 과 역방향이면, 후보 목록에서 방향이 맞는 최선 후보를 앞으로 당긴다
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력(heading·속도)
 * @param[in,out] listMatchEntryList 비용 오름차순 정렬된 후보 목록
 * @return void
 * @remark FixOppositePairByHeading 의 일반화 경로 — 그쪽은 psf 의 qwOppositeLinkID(짝 링크)에
 *   의존하는데, 짝이 등록돼 있지 않거나 후보 목록 밖이면 아무 것도 못 한다(실측: 실주행 21트립
 *   재매칭에서 그 교정이 한 번도 발동하지 않았다). 이쪽은 짝 관계와 무관하게 "후보 목록 안에서
 *   링크 진행방향(GetLinkAzimuth)이 heading 과 맞는 것"을 찾는다.
 *
 *   왜 SgmtMatch 의 각도 비용으로는 안 되는가 — 그쪽은 세그먼트 방위각을 정·역 양방향으로 보고
 *   더 잘 맞는 쪽을 채택한다(2026-07-16, 양방향 단일 링크의 120° 오배제 방지). 그래서 역주행
 *   링크도 "각도가 잘 맞는" 것으로 계산돼 걸러지지 않는다. 실측 000376_20260821095239 seq1:
 *   2040424401 은 세그먼트 역방향차가 44° 라 각도를 켜도 비용 10.6 으로 정답(2040424301, 15.0)을
 *   이긴다. 링크 방위각(254° vs heading 118° = 136°)으로 봐야 비로소 역주행임이 드러난다.
 *
 *   임계값은 FixOppositePairByHeading 과 동일하게 재사용한다 — 채택 링크가 MM_OPP_FIX_REV_DEG
 *   이상 어긋나야 검토하고, 교체 후보는 MM_OPP_FIX_FWD_DEG 이내여야 한다. 둘 다 애매하면
 *   건드리지 않는다. 목록이 비용 오름차순이므로 조건을 만족하는 첫 후보가 곧 최선이다.
 *   (2026-09-05 최정우 추가, 사용자 지시)
*/
void CBeginMapMatch::FixReverseLinkByAzimuth(const SGMT_MATCH_INPUT& stSgmtMatchInput,
		list<MATCH_ENTRY>& listMatchEntryList)
{
	if (listMatchEntryList.empty())
		return;
	if (stSgmtMatchInput.nDirAng == NO_ANGLE)
		return;
	// 정지·저속 구간의 heading 은 신뢰할 수 없다 (FixOppositePairByHeading 과 동일 기준)
	if ((stSgmtMatchInput.nSpeed == NO_SPEED) || (stSgmtMatchInput.nSpeed < MM_OPP_FIX_MIN_SPEED))
		return;
	if (m_pcDataLoader == nullptr)
		return;

	sint16 nHeading = stSgmtMatchInput.nDirAng;
	PLINK_INFO pstTop = m_pcDataLoader->GetLinkInfo(listMatchEntryList.begin()->qwLinkID);
	if (pstTop == nullptr)
		return;

	// 채택 링크가 heading 과 거의 정반대가 아니면 교정 대상이 아니다
	sint16 nTopAz = GetLinkAzimuth(pstTop);
	if (abs(m_cGISUtil.GetAngleDiff(nTopAz, nHeading)) < MM_OPP_FIX_REV_DEG)
		return;

	for (list<MATCH_ENTRY>::iterator it = listMatchEntryList.begin();
			it != listMatchEntryList.end(); ++it)
	{
		if (it == listMatchEntryList.begin())
			continue;

		PLINK_INFO pstCand = m_pcDataLoader->GetLinkInfo(it->qwLinkID);
		if (pstCand == nullptr)
			continue;

		sint16 nCandAz = GetLinkAzimuth(pstCand);
		if (abs(m_cGISUtil.GetAngleDiff(nCandAz, nHeading)) > MM_OPP_FIX_FWD_DEG)
			continue;					// 이 후보도 방향이 안 맞음 — 다음 후보

		LOGFMTI("begin reverse link fix!link=[%llu]->[%llu], heading=[%d], az=[%d]->[%d], "
			"cost=[%.1f]->[%.1f], speed=[%d]",
			static_cast<unsigned long long>(listMatchEntryList.begin()->qwLinkID),
			static_cast<unsigned long long>(it->qwLinkID), nHeading, nTopAz, nCandAz,
			listMatchEntryList.begin()->dfCost, it->dfCost, stSgmtMatchInput.nSpeed);

		MATCH_ENTRY stFixed = *it;
		listMatchEntryList.erase(it);
		listMatchEntryList.push_front(stFixed);
		return;
	}
	// 방향이 맞는 후보가 하나도 없으면 그대로 둔다 — 종전대로 IsAntiHeadingOpposite 거부권이
	//   받아 SKIP 처리한다("확신 없는 매칭보다 SKIP")
}

/**
 * @brief 짝 링크(qwOppositeLinkID)가 있는 링크가 heading 과 거의 정반대로 채택됐는지 판정
 * @param[in] qwLinkID 판정 대상 링크 ID
 * @param[in] nHeading GPS 방위각
 * @param[in] nSpeed GPS 속도(km/h)
 * @return true(짝 링크가 있는데 이번 채택이 heading 역방향 — 신뢰 불가)
 * @remark
 * \tMapMatch.cpp 의 Continue→Begin 병행폴백 대체 직전 거부권으로 쓴다. FixOppositePairByHeading 과
 * \t같은 임계값(MM_OPP_FIX_REV_DEG)을 재사용 — 그쪽은 "짝 링크가 후보 목록에 있으면 앞으로 당기고",
 * \t이쪽은 "그래도 여전히(짝을 못 찾아) 역방향 링크가 채택돼 있으면 아예 대체를 무효화"하는 역할이라
 * \t상호 보완 관계다(실측 000376_20260819094414 M79 — 2040423603 이 짝 2040423501 의 역방향으로
 * \t채택됐는데, 실제 정답 링크는 2040423503 이라 짝 교정만으론 못 잡음). (2026-08-24 최정우 추가)
*/
bool CBeginMapMatch::IsAntiHeadingOpposite(uint64 qwLinkID, sint16 nHeading, sint16 nSpeed)
{
	if ((nHeading == NO_ANGLE) || (nSpeed == NO_SPEED) || (nSpeed < MM_OPP_FIX_MIN_SPEED))
		return false;
	if (m_pcDataLoader == nullptr)
		return false;

	PLINK_INFO pstLink = m_pcDataLoader->GetLinkInfo(qwLinkID);
	if ((pstLink == nullptr) || (pstLink->qwOppositeLinkID == 0))
		return false;

	sint16 nAz = GetLinkAzimuth(pstLink);
	return (abs(m_cGISUtil.GetAngleDiff(nAz, nHeading)) >= MM_OPP_FIX_REV_DEG);
}

/**
 * @brief 초기 맵매칭 시작
 * @param[in] pcDataLoader 데이터 로딩 클래스
 * @param[in,out] stSgmtMatchInput 세그먼트 매칭 입력 정보.
 *                **주의: 좌표(stPoint)가 내부에서 ×360000 으로 덮어써진다** — 아래 본문의
 *                ※※ 주의 주석 참고 (2026-09-21 최정우 주석 보완 — in 으로만 적혀 있어
 *                호출측이 값이 보존되는 것으로 오해할 수 있었다)
 * @param[out] pwErrorCode 에러 코드
 * @param[out] pstMatchEntry 검색 정보
 * @param[in] pstTraceCtx 매칭 트레이스 컨텍스트 (nullptr=트레이스 미기록)
 *                (2026-09-21 최정우 주석 보완 — 설명이 없던 파라미터)
 * @param[in] qwBiasLinkID 연결성 편향 기준 링크(직전 성공 링크). 0=편향 미적용
 *                (2026-09-21 최정우 주석 보완 — 설명이 없던 파라미터)
 * @return true(성공), false(실패)
*/
bool CBeginMapMatch::StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry, PMATCH_TRACE_CTX pstTraceCtx,
		uint64 qwBiasLinkID)
{
	// [2026-09-21 최정우 보완] 아래 지도 로드 검사만 `if (pwErrorCode != nullptr)` 로 방어하고,
	//   그 뒤의 NOT_FOUND_GRIDINFO·MAP_MATCH_FAIL·NO_ERROR 대입은 전부 무방어 역참조였다.
	//   이 함수는 에러코드 없이는 결과를 해석할 수 없으므로 입구에서 한 번 막는 편이 맞다.
	//   현재 호출부(MapMatch.cpp)는 항상 유효 포인터를 넘기므로 동작 변화는 없다.
	if (pwErrorCode == nullptr)
	{
		LOGFMTE("begin map match called with null error code!");
		return false;
	}

	m_pcDataLoader = pcDataLoader;

	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		// [버그 수정, 2026-09-15 최정우] 에러코드를 세팅하고 반환한다. 종전에는 pwErrorCode 를
		//   건드리지 않고 false 만 돌려줘, 호출부(MapMatch.cpp)가 초기값 NO_ERROR 를 그대로
		//   pstMatchLinkInfo->wErrorCode 에 넣고 szErrorMsg 에 "오류 없음" 을 복사했다 —
		//   **지도 로드 실패라는 치명적 상태가 정상 코드로 기록**된다(link.psf 재로드 실패 직후
		//   들어온 GPS 가 이 경로를 탄다).
		if (pwErrorCode != nullptr)
			*pwErrorCode = NOT_LOADED_MAPDATA;
		LOGFMTE("data loading fail!");
		return false;
	}

	// GRID 구하기
	uint32 dwGridID = m_cGISUtil.GetGridID(stSgmtMatchInput.stPoint.dfX, stSgmtMatchInput.stPoint.dfY);

	// GRID 별 세그먼트 범위
	PGRID_INFO pstGridInfo = m_pcDataLoader->GetGridInfo(dwGridID);
	if (!pstGridInfo)
	{
		*pwErrorCode = NOT_FOUND_GRIDINFO;
		return false;
	}

	list<MATCH_ENTRY> listMatchEntryList;
	vector<uint32> vtNearGridIDList;

	// 연속실패 Begin 재검색: 직전 성공 링크와 연결(회전 가능)된 링크 집합 구성 (2026-07-15 최정우 추가)
	//   비어있지 않으면 GridSgmtMapMatch 에서 미연결 후보에 소프트 페널티 → 나란한 도로 오매칭 억제
	set<uint64> setConnected;
	if (qwBiasLinkID != 0)
		BuildConnectedSet(qwBiasLinkID, setConnected);
	const set<uint64>* psetConnected = setConnected.empty() ? nullptr : &setConnected;
	
	// 현재 소속된 GRID ID
	vtNearGridIDList.push_back(dwGridID);

	// 인접 GRID 목록 검색
	m_cGISUtil.GetNearGridID(dwGridID, stSgmtMatchInput, vtNearGridIDList);

	// ※※ [주의, 2026-09-16 최정우 추가] 이 함수는 **참조로 받은 stSgmtMatchInput 을 그 자리에서
	//   변환**한다(도(度) → 내부 좌표 스케일 ×360000) — 원래 값으로 되돌리지 않는다.
	//   도로망(link.psf)은 CreateData 가 이미 스케일 정수로 저장하므로(BinaryMaker.cpp:928) 비교
	//   대상과 단위를 맞추려면 GPS 입력 쪽을 올려야 하고, 그 변환이 여기다.
	//   **그래서 호출측이 지켜야 할 두 가지:**
	//     ① 호출 후 그 변수를 다시 읽지 말 것 — 도 단위로 믿고 쓰면 36만 배 값을 얻는다
	//     ② 같은 변수를 다시 넘기지 말 것 — ×360000 이 두 번 걸려 약 1,300억 배가 된다
	//   둘 다 컴파일 경고도 크래시도 없이 조용히 틀린다. 재호출이 필요하면 MapMatch.cpp 의
	//   Begin 병행폴백처럼(stBeginSgmtMatchInput, MapMatch.cpp:271) **새 변수에 원본 도 좌표를
	//   다시 채워** 넘길 것. 현재 호출부는 전부 이 규칙을 지키고 있어 무해하다.
	//   참조 대신 값 전달로 바꾸는 근본 수정은 하위 호출 전부가 "이미 스케일된 상태"를 전제로
	//   짜여 있어 핵심 매칭 함수 전체 재설계가 필요 — 보류 중인 별도 과제다.
	//   [정정] 위 "MapMatch.cpp:271" 은 작성 당시 줄 번호라 지금은 어긋난다 — CMapMatch::ContinueMapMatch()
	//   안의 Begin 병행폴백 블록(stBeginSgmtMatchInput 선언부)을 가리킨다 (2026-10-06 최정우 주석 수정)
	stSgmtMatchInput.stPoint.dfX *= 360000.0;
	stSgmtMatchInput.stPoint.dfY *= 360000.0;

	uint32 dwStartSgmtOffset = pstGridInfo->dwSgmtOffset;
	uint32 dwEndSgmtOffset = dwStartSgmtOffset + pstGridInfo->wSgmtCount;

	if (!vtNearGridIDList.empty())
	{
		vector<uint32>::iterator it = vtNearGridIDList.begin();
		for (; it != vtNearGridIDList.end(); ++it)
		{
			// GRID 별 세그먼트 범위
			pstGridInfo = m_pcDataLoader->GetGridInfo(*it);
			if (!pstGridInfo)
			{
				*pwErrorCode = NOT_FOUND_GRIDINFO;
				continue;
			}

			dwStartSgmtOffset = pstGridInfo->dwSgmtOffset;
			dwEndSgmtOffset = dwStartSgmtOffset + pstGridInfo->wSgmtCount;

			// GRID 내 세그먼트 범위별 맵매칭 수행 (연결성 편향 집합 전달) (2026-07-08 최정우 주석 추가)
			//   셀당 상위 MM_BEGIN_CELL_TOPN 건을 목록에 직접 append (2026-09-05 최정우 수정)
			GridSgmtMapMatch(stSgmtMatchInput, dwStartSgmtOffset, dwEndSgmtOffset, pwErrorCode,
				listMatchEntryList, psetConnected);
		}
	}

	if (listMatchEntryList.empty())
	{
		*pwErrorCode = MAP_MATCH_FAIL;
		return false;
	}

	*pwErrorCode = NO_ERROR;
	listMatchEntryList.sort();
	// 왕복분리 짝 링크 오매칭 교정 — BEGIN 은 heading 을 무시하므로 거리로만 고르면
	//   10m 옆 반대방향 링크가 채택될 수 있다 (2026-08-22 최정우 추가)
	FixOppositePairByHeading(stSgmtMatchInput, listMatchEntryList);
	// 위 교정은 qwOppositeLinkID(짝 링크)가 psf 에 등록돼 있어야만 동작한다 — 등록이 없거나
	//   짝이 후보 목록 밖이면 역방향 링크가 그대로 채택돼 ProcessManager 의 IsAntiHeadingOpposite
	//   거부권에 걸려 통째로 SKIP 된다. 짝에 의존하지 않고 후보 목록 자체에서 방향이 맞는 링크를
	//   찾는 일반 경로를 뒤에 둔다 (2026-09-05 최정우 추가, 사용자 지시)
	FixReverseLinkByAzimuth(stSgmtMatchInput, listMatchEntryList);
	if (pstTraceCtx != nullptr)
	{
		pstTraceCtx->nMatchedStep = 0;
		CMatchTrace::LogResult(pstTraceCtx, stSgmtMatchInput, listMatchEntryList, listMatchEntryList.front());
	}
	*pstMatchEntry = *listMatchEntryList.begin();
	return true;
}

/**
 * @brief GRID 별 세그먼트 맵매칭
 * @param[in] stSgmtMatchInput 세그먼트 입력 정보
 * @param[in] dwStartSgmtOffset 세그먼트 시작
 * @param[in] dwEndSgmtOffset 세그먼트 종료
 * @param[out] pwErrorCode 에러 코드
 * @param[out] listOutEntryList 검색 정보 — 이 셀의 상위 MM_BEGIN_CELL_TOPN 건을 append
 * @return true(1건 이상 담음), false(이 셀에 후보 없음)
 * @remark 2026-09-05 최정우 수정(사용자 지시) — 기존엔 셀 안에서 1등 하나만 반환했다. 그래서
 *   후보 목록 크기가 곧 "매칭된 그리드 셀 수"였고, 같은 셀 안의 정답 링크는 더 가까운 링크에
 *   가려 목록에 오르지조차 못했다(MM_BEGIN_CELL_TOPN 주석의 seq1 실측 참고). 상위 N건을 담아
 *   뒤따르는 교정 계층(FixOppositePairByHeading·FixReverseLinkByAzimuth)이 실제로 고를 수
 *   있는 재료를 준다. 정렬·최종 선택은 호출측(StartMapMatch)이 전체 목록을 다시 sort 해서 한다.
*/
bool CBeginMapMatch::GridSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset, 
		uint32 dwEndSgmtOffset, uint16 *pwErrorCode, list<MATCH_ENTRY>& listOutEntryList,
		const std::set<uint64>* psetConnected)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		// [버그 수정, 2026-09-15 최정우] 에러코드를 세팅하고 반환한다. 종전에는 pwErrorCode 를
		//   건드리지 않고 false 만 돌려줘, 호출부(MapMatch.cpp)가 초기값 NO_ERROR 를 그대로
		//   pstMatchLinkInfo->wErrorCode 에 넣고 szErrorMsg 에 "오류 없음" 을 복사했다 —
		//   **지도 로드 실패라는 치명적 상태가 정상 코드로 기록**된다(link.psf 재로드 실패 직후
		//   들어온 GPS 가 이 경로를 탄다).
		if (pwErrorCode != nullptr)
			*pwErrorCode = NOT_LOADED_MAPDATA;
		LOGFMTE("data loading fail!");
		return false;
	}

	list<MATCH_ENTRY> listMatchEntryList;
	listMatchEntryList.clear();

	for (uint32 i=dwStartSgmtOffset; i<dwEndSgmtOffset; ++i)
	{
		SGMT_MATCH_RES stSgmtMatchRes;

		// GRID 세그먼트 정보
		PGRID_SGMT_INFO pstGridSgmtInfo = m_pcDataLoader->GetGridSgmtInfo(i);
		if (!pstGridSgmtInfo)
			continue;

		// 세그먼트 정보 구조체
		SGMT_INFO stSgmtInfo;

		// 세그먼트 X,Y 좌표
		stSgmtInfo.stPoint.dfX = static_cast<double>(pstGridSgmtInfo->dwX);
		stSgmtInfo.stPoint.dfY = static_cast<double>(pstGridSgmtInfo->dwY);

		// 세그먼트 진행 방향(방위각)
		stSgmtInfo.nDirAng = static_cast<sint16>(pstGridSgmtInfo->wDirAng);

		// 세그먼트 길이 형변환
		stSgmtInfo.dfLen = static_cast<double>(pstGridSgmtInfo->wLenSgmt);

		// 링크 ID
		stSgmtInfo.qwLinkID = pstGridSgmtInfo->qwLinkID;

		// INTERSECT_LEN(GPS↔세그먼트 교차점 거리)·방위 비용 매칭 (2026-07-08 최정우 주석 추가)
		// BEGIN 매칭은 heading 을 후보 판정에 쓰지 않는다 — bIgnoreHeading=true 로 heading
		//   하드컷·소프트 비용 전부 미적용, 거리(INTERSECT_LEN)만으로 후보 판정
		// [주석 정정, 2026-09-21 최정우] 종전 주석은 이를 "실험(2026-08-19 임시)" 이라고 적었으나
		//   임시가 아니라 **확정된 설계**다. 트립 첫 점의 heading 은 신뢰할 수 없다는 실측
		//   (전국 21트립: 이후 점들의 평균 방향과 41° 어긋남)에 따른 것이고, 그 때문에 방향
		//   보정은 후보를 다 모은 뒤 FixOppositePairByHeading·FixReverseLinkByAzimuth 두
		//   교정 계층에서 따로 한다. "임시" 로 읽고 되돌리면 그 두 교정의 전제가 깨진다.
		if (!m_cGISUtil.SgmtMatch(stSgmtMatchInput, stSgmtInfo, &stSgmtMatchRes, false, true))
			continue;

		// 매칭 성공이면 링크 정보
		PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(stSgmtInfo.qwLinkID);
		if (!pstLinkInfo) continue;

		MATCH_ENTRY stMatchEntry;

		stMatchEntry.dfMatchX = stSgmtMatchRes.stMatchPoint.dfX;
		stMatchEntry.dfMatchY = stSgmtMatchRes.stMatchPoint.dfY;
		stMatchEntry.dfSgmtMatchLen = stSgmtMatchRes.dfSgmtMatchLen;
		stMatchEntry.dfIntersectLenSgmt = stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.bSgmtClamped = stSgmtMatchRes.bSgmtClamped;			// 세그먼트 끝점 스냅 여부 (2026-07-21 최정우 추가)
		stMatchEntry.bHasHeading = stSgmtMatchRes.bHasHeading;				// heading 값 존재 여부 (2026-07-22 최정우 추가)
		stMatchEntry.bClampTrustedByHeading = stSgmtMatchRes.bClampTrustedByHeading;	// 클램프+heading 신뢰 구제 신호 (2026-08-20 최정우 추가)
		stMatchEntry.dfCost = stSgmtMatchRes.dfCost;		// 소프트 비용(INTERSECT_LEN+방위각) → sort 선택 기준 (2026-07-08 최정우 추가)
		stMatchEntry.dfAngleCost = stSgmtMatchRes.dfCost - stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfAltAdj = 0.0;

		// 연속실패 Begin 재검색: 직전 성공 링크와 미연결(회전 불가) 후보에 소프트 페널티 (2026-07-15 최정우 추가)
		//   연결 집합에 없는 링크만 cost 가산 → 나란한 도로/역주행 링크로 튀는 오매칭 억제.
		//   소프트 페널티라 페널티(m)보다 명백히 더 가까운 도로는 그대로 선택됨.
		if ((psetConnected != nullptr) &&
			(psetConnected->find(stSgmtMatchRes.qwLinkID) == psetConnected->end()))
		{
			stMatchEntry.dfCost += MM_CONNECT_PENALTY;
		}

		stMatchEntry.nDirAngleDiff = stSgmtMatchRes.nDirAngleDiff;
		stMatchEntry.qwLinkID = stSgmtMatchRes.qwLinkID;
		stMatchEntry.wLenFromLink = pstGridSgmtInfo->wLenFromLink;
		stMatchEntry.nMaxSpeed = pstLinkInfo->nMaxSpeed;
		stMatchEntry.dfLen = pstLinkInfo->dfLen;
		stMatchEntry.nRoadRank = pstLinkInfo->nRoadRank;
		stMatchEntry.nConnect = pstLinkInfo->nConnect;
		stMatchEntry.nRoadType = pstLinkInfo->nRoadType;
		stMatchEntry.nLanes = pstLinkInfo->nLanes;
		memcpy(stMatchEntry.szRoadName, pstLinkInfo->szRoadName, sizeof(stMatchEntry.szRoadName));
		stMatchEntry.qwStNodeID = pstLinkInfo->qwStNodeID;
		stMatchEntry.dfStNodeX = static_cast<double>(pstLinkInfo->dwStNodeX);
		stMatchEntry.dfStNodeY = static_cast<double>(pstLinkInfo->dwStNodeY);
		stMatchEntry.nStNodeType = pstLinkInfo->nStNodeType;
		stMatchEntry.qwEdNodeID = pstLinkInfo->qwEdNodeID;
		stMatchEntry.dfEdNodeX = static_cast<double>(pstLinkInfo->dwEdNodeX);
		stMatchEntry.dfEdNodeY = static_cast<double>(pstLinkInfo->dwEdNodeY);
		stMatchEntry.nEdNodeType = pstLinkInfo->nEdNodeType;

		listMatchEntryList.push_back(stMatchEntry);
	}

	if (listMatchEntryList.empty())								// 매칭된 결과가 없으면
		*pwErrorCode = MAP_MATCH_FAIL;
	else														// 매칭된 결과가 있으면
	{
		// 매칭 거리순 정렬
		// [정정] 거리순이 아니라 MATCH_ENTRY::operator< 기준 **dfCost 오름차순**(동률 시 INTERSECT_LEN)이다.
		//   Begin 은 heading 을 안 써 각도비용이 0 이지만, 연속실패 재검색이면 위 MM_CONNECT_PENALTY 가
		//   dfCost 에 더해져 있어 거리순과 달라질 수 있다 (2026-10-06 최정우 주석 수정)
		listMatchEntryList.sort();

		*pwErrorCode = NO_ERROR;
		// 셀당 1등만 담던 것을 상위 MM_BEGIN_CELL_TOPN 건까지 담는다 (2026-09-05 최정우 수정)
		//   단 "같은 링크의 다른 세그먼트"는 건너뛴다 — 링크 하나가 여러 세그먼트로 잘려 있어,
		//   GPS 가 그 링크 위에 있으면 최근접 N 건이 전부 같은 링크 조각으로 채워진다. 그러면
		//   자리만 쓰고 링크 다양성은 늘지 않아 이 확장의 목적(정답 링크를 목록에 올리기)이
		//   무산된다 — 실측: 후보 2건 이상인 Begin 호출 430건 중 391건(90.9%)에서 같은 링크가
		//   섞였고, 35건(8.1%)은 전부 같은 링크 하나였다. 같은 링크의 두 번째 조각은 어차피
		//   같은 링크라 뒤따르는 교정 계층에 새 정보를 주지 않으므로 버려도 손실이 없다.
		//   셀 간 중복은 건드리지 않는다 — 셀 처리 순서는 거리순이 아니라서, 먼저 처리된 셀의
		//   더 먼 조각이 살아남고 나중 셀의 더 가까운 조각이 버려질 수 있다(기존 동작 유지).
		//   (2026-09-05 최정우 수정, 사용자 지시)
		int nTaken = 0;
		set<uint64> setTakenLinkID;
		for (list<MATCH_ENTRY>::const_iterator it = listMatchEntryList.begin();
				(it != listMatchEntryList.end()) && (nTaken < MM_BEGIN_CELL_TOPN); ++it)
		{
			if (!setTakenLinkID.insert(it->qwLinkID).second)
				continue;						// 이 셀에서 이미 담은 링크 — 다음 링크를 찾는다
			listOutEntryList.push_back(*it);
			++nTaken;
		}
	}

	return (*pwErrorCode == NO_ERROR) ? true : false;
}

/**
 * @brief GRID 내 세그먼트 기하 최근접(반경 무시) — 진단반경 초과 SKIP 참고용 (2026-07-10 최정우 수정)
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 (좌표는 호출측에서 이미 ×360000 된 상태)
 * @param[in] dwStartSgmtOffset 이 그리드의 세그먼트 시작 offset
 * @param[in] dwEndSgmtOffset 이 그리드의 세그먼트 끝 offset(미포함)
 * @param[in,out] stBest 지금까지의 최근접 후보. 더 가까운 것을 만나면 갱신된다
 * @param[in,out] dfBestDist 위 후보의 INTERSECT_LEN. 갱신 시 함께 바뀐다
 * @param[in,out] bFound 후보를 한 번이라도 찾았는지. **여러 그리드에 걸쳐 누적**되므로
 *                호출측이 루프 밖에서 초기화하고 그대로 넘겨야 한다
 * @return bFound 와 같은 값(누적 결과)
 * @remark 2026-09-21 최정우 주석 보완 — @param 이 하나도 없어, 특히 뒤 3개가 "이 그리드만의
 *   결과" 가 아니라 그리드 루프 전체에 걸친 누적 상태라는 점이 드러나지 않았다.
*/
bool CBeginMapMatch::GridSgmtGeomNearest(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset,
		uint32 dwEndSgmtOffset, MATCH_ENTRY& stBest, double& dfBestDist, bool& bFound)
{
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
		return false;

	for (uint32 i = dwStartSgmtOffset; i < dwEndSgmtOffset; ++i)
	{
		PGRID_SGMT_INFO pstGridSgmtInfo = m_pcDataLoader->GetGridSgmtInfo(i);
		if (!pstGridSgmtInfo)
			continue;

		SGMT_INFO stSgmtInfo;
		stSgmtInfo.stPoint.dfX = static_cast<double>(pstGridSgmtInfo->dwX);
		stSgmtInfo.stPoint.dfY = static_cast<double>(pstGridSgmtInfo->dwY);
		stSgmtInfo.nDirAng = static_cast<sint16>(pstGridSgmtInfo->wDirAng);
		stSgmtInfo.dfLen = static_cast<double>(pstGridSgmtInfo->wLenSgmt);
		stSgmtInfo.qwLinkID = pstGridSgmtInfo->qwLinkID;

		SGMT_MATCH_RES stSgmtMatchRes;
		// 반경(nRadius) 초과여도 INTERSECT_LEN·스냅 좌표만 계산 (정식 매칭 아님) (2026-07-10 최정우 수정)
		if (!m_cGISUtil.SgmtMatch(stSgmtMatchInput, stSgmtInfo, &stSgmtMatchRes, true))
			continue;

		PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(stSgmtInfo.qwLinkID);
		if (!pstLinkInfo)
			continue;

		if (bFound && (stSgmtMatchRes.dfIntersectLenSgmt >= dfBestDist))
			continue;

		MATCH_ENTRY stMatchEntry;
		stMatchEntry.dfMatchX = stSgmtMatchRes.stMatchPoint.dfX;
		stMatchEntry.dfMatchY = stSgmtMatchRes.stMatchPoint.dfY;
		stMatchEntry.dfSgmtMatchLen = stSgmtMatchRes.dfSgmtMatchLen;
		stMatchEntry.dfIntersectLenSgmt = stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.bSgmtClamped = stSgmtMatchRes.bSgmtClamped;			// 세그먼트 끝점 스냅 여부 (2026-07-21 최정우 추가)
		stMatchEntry.bHasHeading = stSgmtMatchRes.bHasHeading;				// heading 값 존재 여부 (2026-07-22 최정우 추가)
		stMatchEntry.bClampTrustedByHeading = stSgmtMatchRes.bClampTrustedByHeading;	// 클램프+heading 신뢰 구제 신호 (2026-08-20 최정우 추가)
		stMatchEntry.dfCost = stSgmtMatchRes.dfCost;
		stMatchEntry.dfAngleCost = stSgmtMatchRes.dfCost - stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfAltAdj = 0.0;
		stMatchEntry.nDirAngleDiff = stSgmtMatchRes.nDirAngleDiff;
		stMatchEntry.qwLinkID = stSgmtMatchRes.qwLinkID;
		stMatchEntry.wLenFromLink = pstGridSgmtInfo->wLenFromLink;
		stMatchEntry.nMaxSpeed = pstLinkInfo->nMaxSpeed;
		stMatchEntry.dfLen = pstLinkInfo->dfLen;
		stMatchEntry.nRoadRank = pstLinkInfo->nRoadRank;
		stMatchEntry.nConnect = pstLinkInfo->nConnect;
		stMatchEntry.nRoadType = pstLinkInfo->nRoadType;
		stMatchEntry.nLanes = pstLinkInfo->nLanes;
		memcpy(stMatchEntry.szRoadName, pstLinkInfo->szRoadName, sizeof(stMatchEntry.szRoadName));
		stMatchEntry.qwStNodeID = pstLinkInfo->qwStNodeID;
		stMatchEntry.dfStNodeX = static_cast<double>(pstLinkInfo->dwStNodeX);
		stMatchEntry.dfStNodeY = static_cast<double>(pstLinkInfo->dwStNodeY);
		stMatchEntry.nStNodeType = pstLinkInfo->nStNodeType;
		stMatchEntry.qwEdNodeID = pstLinkInfo->qwEdNodeID;
		stMatchEntry.dfEdNodeX = static_cast<double>(pstLinkInfo->dwEdNodeX);
		stMatchEntry.dfEdNodeY = static_cast<double>(pstLinkInfo->dwEdNodeY);
		stMatchEntry.nEdNodeType = pstLinkInfo->nEdNodeType;

		stBest = stMatchEntry;
		dfBestDist = stSgmtMatchRes.dfIntersectLenSgmt;
		bFound = true;
	}

	return bFound;
}

/**
 * @brief 소속·인접 GRID 에서 반경 무시 기하 최근접 세그먼트 1건 (2026-07-10 최정우 수정)
 * @param[in] pcDataLoader 데이터 로딩 클래스
 * @param[in,out] stSgmtMatchInput 세그먼트 매칭 입력. **좌표가 ×360000 으로 덮어써진다**
 * @param[out] pwErrorCode 에러 코드
 * @param[out] pstMatchEntry 최근접 1건
 * @return true(1건 확보), false(후보 없음·지도 미적재)
 * @remark 정식 매칭 실패·진단반경(MM_DIAG_RADIUS) 내 후보도 없을 때 호출.
 *         그리드에 링크가 있으나 거리만 먼 경우 SKIP용 MATCH_LAT/LON·INTERSECT_LEN 확보.
 *         (2026-09-21 최정우 주석 보완 — @param 이 하나도 없었다)
*/
bool CBeginMapMatch::FindGeomNearest(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry)
{
	// StartMapMatch() 와 동일 근거 — 아래 MAP_MATCH_FAIL·NO_ERROR 대입이 무방어였다
	//   (2026-09-21 최정우 보완)
	if (pwErrorCode == nullptr)
	{
		LOGFMTE("begin geom nearest called with null error code!");
		return false;
	}

	m_pcDataLoader = pcDataLoader;

	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		// [버그 수정, 2026-09-15 최정우] 에러코드를 세팅하고 반환한다. 종전에는 pwErrorCode 를
		//   건드리지 않고 false 만 돌려줘, 호출부(MapMatch.cpp)가 초기값 NO_ERROR 를 그대로
		//   pstMatchLinkInfo->wErrorCode 에 넣고 szErrorMsg 에 "오류 없음" 을 복사했다 —
		//   **지도 로드 실패라는 치명적 상태가 정상 코드로 기록**된다(link.psf 재로드 실패 직후
		//   들어온 GPS 가 이 경로를 탄다).
		if (pwErrorCode != nullptr)
			*pwErrorCode = NOT_LOADED_MAPDATA;
		LOGFMTE("data loading fail!");
		return false;
	}

	uint32 dwGridID = m_cGISUtil.GetGridID(stSgmtMatchInput.stPoint.dfX, stSgmtMatchInput.stPoint.dfY);
	vector<uint32> vtNearGridIDList;
	vtNearGridIDList.push_back(dwGridID);
	// 인접 GRID 탐색 반경은 MM_DIAG_RADIUS 과 동일(그리드 선정용) (2026-07-10 최정우 수정)
	m_cGISUtil.GetNearGridID(dwGridID, stSgmtMatchInput, vtNearGridIDList);

	// ※※ [주의, 2026-09-16 최정우 추가] 이 함수는 **참조로 받은 stSgmtMatchInput 을 그 자리에서
	//   변환**한다(도(度) → 내부 좌표 스케일 ×360000) — 원래 값으로 되돌리지 않는다.
	//   도로망(link.psf)은 CreateData 가 이미 스케일 정수로 저장하므로(BinaryMaker.cpp:928) 비교
	//   대상과 단위를 맞추려면 GPS 입력 쪽을 올려야 하고, 그 변환이 여기다.
	//   **그래서 호출측이 지켜야 할 두 가지:**
	//     ① 호출 후 그 변수를 다시 읽지 말 것 — 도 단위로 믿고 쓰면 36만 배 값을 얻는다
	//     ② 같은 변수를 다시 넘기지 말 것 — ×360000 이 두 번 걸려 약 1,300억 배가 된다
	//   둘 다 컴파일 경고도 크래시도 없이 조용히 틀린다. 재호출이 필요하면 MapMatch.cpp 의
	//   Begin 병행폴백처럼(stBeginSgmtMatchInput, MapMatch.cpp:271) **새 변수에 원본 도 좌표를
	//   다시 채워** 넘길 것. 현재 호출부는 전부 이 규칙을 지키고 있어 무해하다.
	//   참조 대신 값 전달로 바꾸는 근본 수정은 하위 호출 전부가 "이미 스케일된 상태"를 전제로
	//   짜여 있어 핵심 매칭 함수 전체 재설계가 필요 — 보류 중인 별도 과제다.
	//   [정정] 위 "MapMatch.cpp:271" 은 작성 당시 줄 번호라 지금은 어긋난다 — CMapMatch::ContinueMapMatch()
	//   안의 Begin 병행폴백 블록(stBeginSgmtMatchInput 선언부)을 가리킨다 (2026-10-06 최정우 주석 수정)
	stSgmtMatchInput.stPoint.dfX *= 360000.0;
	stSgmtMatchInput.stPoint.dfY *= 360000.0;

	bool bFound = false;
	double dfBestDist = -1.0;
	MATCH_ENTRY stBest;

	for (size_t i = 0; i < vtNearGridIDList.size(); ++i)
	{
		PGRID_INFO pstGridInfo = m_pcDataLoader->GetGridInfo(vtNearGridIDList[i]);
		if (!pstGridInfo)
			continue;

		uint32 dwStartSgmtOffset = pstGridInfo->dwSgmtOffset;
		uint32 dwEndSgmtOffset = dwStartSgmtOffset + pstGridInfo->wSgmtCount;
		GridSgmtGeomNearest(stSgmtMatchInput, dwStartSgmtOffset, dwEndSgmtOffset,
			stBest, dfBestDist, bFound);
	}

	if (!bFound)
	{
		*pwErrorCode = MAP_MATCH_FAIL;
		return false;
	}

	*pwErrorCode = NO_ERROR;
	*pstMatchEntry = stBest;
	return true;
}

/**
 * @brief 직전 성공 링크 기준 연결(회전 가능) 링크 집합 구성 (2026-07-15 최정우 추가)
 * @param[in] qwBiasLinkID 직전 성공 링크 ID
 * @param[out] setConnected {bias link} ∪ {bias link 의 진출(회전) 링크}
 * @remark
 *   TURN_INFO 는 진입 링크(qwInLinkID) 기준 진출 링크(qwOutLinkID)를 담고 있어
 *   실제 주행 가능한 1-스텝 연속 링크를 그대로 사용. 노드만 공유하는(수렴) 링크는
 *   회전 정보에 없으므로 제외 → 나란한 도로 오매칭 억제에 적합.
*/
void CBeginMapMatch::BuildConnectedSet(uint64 qwBiasLinkID, std::set<uint64>& setConnected)
{
	if ((m_pcDataLoader == nullptr) || (qwBiasLinkID == 0))
		return;

	// 직전 성공 링크 자체는 항상 연결 집합에 포함(같은 링크 재매칭 허용)
	setConnected.insert(qwBiasLinkID);

	PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(qwBiasLinkID);
	if (!pstLinkInfo)
		return;

	// 진출 링크(회전 가능) 수집
	uint32 dwStartTurnOffset = pstLinkInfo->dwTurnOffset;
	uint32 dwEndTurnOffset = dwStartTurnOffset + pstLinkInfo->nTurnCount;
	for (uint32 i = dwStartTurnOffset; i < dwEndTurnOffset; ++i)
	{
		PTURN_INFO pstTurnInfo = m_pcDataLoader->GetTurnInfo(i);
		if (!pstTurnInfo)
			continue;

		setConnected.insert(pstTurnInfo->qwOutLinkID);
	}
}

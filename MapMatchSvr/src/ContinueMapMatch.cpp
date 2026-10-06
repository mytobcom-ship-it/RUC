/**
 * @file ContinueMapMatch.cpp
 * @brief 연속 맵매칭 클래스 소스 파일
*/
#include "ContinueMapMatch.h"

/**
 * @brief 생성자
*/
CContinueMapMatch::CContinueMapMatch() :
	m_pcDataLoader(nullptr)
{
}

/**
 * @brief 소멸자
*/
CContinueMapMatch::~CContinueMapMatch()
{
}

/**
 * @brief 연속 맵매칭 고도 보조 점수 설정 (config [mapmatch] alt_gap/alt_penalty/alt_weight/alt_slope)
 *        (2026-10-06 최정우 주석 수정 — 종전 "altitude_*" 는 2026-07-21 키 이름 변경 전 표기)
 * @param[in] stAltConfig 고도 점수 설정 — Server→ProcessManager→MapMatch→ContinueMapMatch 전달
 * @return void
*/
void CContinueMapMatch::SetAltitudeConfig(const ALTITUDE_SCORE_CONFIG& stAltConfig)
{
	m_stAltitudeConfig = stAltConfig;
}

/**
 * @brief 연속 맵매칭 시작
 * @param[in] pcDataLoader 데이터 로딩 클래스
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] qwLinkID 직전 확정 링크 ID — depth 탐색의 출발점
 * @param[in] nSearchStep 탐색할 최대 depth. config [mapmatch] maxstep 에 이동거리 기반 확장
 *   (MM_STEP_EXTEND_*)이 더해진 값이 들어온다
 *   (2026-09-21 최정우 주석 정정 — 종전 "(0~5)" 는 근거 없는 표기였다. 상한을 강제하는 코드는
 *   어디에도 없고, 검증은 CMapMatch::IsValidSearchStep() 의 "음수 아님" 뿐이다. 이 범위를
 *   믿고 5 기준으로 배열·루프를 짜면 어긋난다)
 * @param[out] pwErrorCode 에러 코드
 * @param[out] pstMatchEntry 검색 정보
 * @param[in] pstTraceCtx 매칭 트레이스 컨텍스트 (nullptr=미기록)
 *   (2026-09-21 최정우 주석 보완 — 설명이 없던 파라미터)
 * @param[out] pvtPathLinkIDs (선택) 직전 확정 링크부터 이번 확정 링크까지 실제 경유한 링크 목록.
 *   게이트/구역 판정이 "지나간 링크 전부"를 봐야 해서 필요하다 — ReconstructPath() 참고
 *   (2026-09-21 최정우 주석 보완 — 설명이 없던 파라미터)
 * @param[out] psetSearchHistoryLinkID (선택) depth 탐색(maxstep 이내)으로 실제 방문한 전체
 *   링크 UID 집합 — CMapMatch::ContinueMapMatch()가 Begin 병행폴백 후보의 "진짜 갈림길" 여부
 *   판별에 사용 (2026-08-21 최정우 추가)
 * @return true(성공), false(실패)
 * @warning **stSgmtMatchInput 은 이 함수 안에서 비가역적으로 변조된다**(좌표가 검색 스케일로
 *   변환되고 qwPrevLinkID 등 내부 필드가 채워진다). 따라서 **호출 후 그 구조체를 다시 읽거나
 *   다른 매칭 호출에 재사용하면 안 된다** — "아직 변환 전"을 전제로 한 코드가 이미 변환된 값을
 *   받아 조용히 틀린 결과를 낸다(컴파일 경고도 없다).
 *   [2026-09-15 최정우 확인] 현재 호출부는 안전하다: CMapMatch::ContinueMapMatch()가 매번 새
 *   로컬을 선언해 넘기고(MapMatch.cpp), 호출 후 그 구조체를 읽지 않으며, 같은 함수 안의 Begin
 *   병행폴백은 **별도 구조체(stBeginSgmtMatchInput)를 원본 stMapMatchInput 에서 새로 채운다**.
 *   2026-09-11 소스리뷰에서 "값 전달로 바꾸려면 StartMapMatch 하위 호출 전체를 재설계해야 한다"며
 *   보류했던 항목 — 재확인 결과 현재는 무해하므로 고치지 않고 이 경고만 남긴다.
 *   **새 호출부를 추가할 때는 반드시 새 구조체를 원본에서 채워 넘길 것.**
*/
bool CContinueMapMatch::StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint64& qwLinkID, sint16& nSearchStep, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx, vector<uint64> *pvtPathLinkIDs,
		set<uint64> *psetSearchHistoryLinkID)
{
	// [2026-09-21 최정우 보완] 아래 지도 로드 검사만 nullptr 을 방어하고, 그 뒤의
	//   NOT_FOUND_LINKID·MAP_MATCH_FAIL·NO_ERROR 대입(총 6곳)은 전부 무방어 역참조였다.
	//   CBeginMapMatch::StartMapMatch() 에 넣은 것과 같은 근거로 입구에서 한 번 막는다.
	//   현재 호출부(MapMatch.cpp)는 항상 유효 포인터를 넘기므로 동작 변화는 없다.
	if (pwErrorCode == nullptr)
	{
		LOGFMTE("continue map match called with null error code!");
		return false;
	}

	// 경로 역추적용 — 새로 발견된 링크가 어느 링크를 거쳐 도달했는지 기록 (2026-08-20 최정우 추가)
	unordered_map<uint64, uint64> mapParentLink;
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

	// 검색된 목록 저장 (중복 검사 처리용) — 링크 ID는 uint64(2026-08-14 최정우 수정, set<uint32>였던
	//   걸 원복 — "소스상 문제" 검토 중 발견, BeginMapMatch 의 동일 목적 집합은 원래부터 set<uint64>)
	set<uint64> setSearchHistoryLinkList;
	// psetSearchHistoryLinkID 로 밖에 노출하는 목록 — setSearchHistoryLinkList 와 달리 BridgeNearbyLinkStarts
	//   로 찾은(TURNINFO 아닌 좌표 근접) 링크는 제외한다. Begin 폴백 화이트리스트 목적(진짜 갈림길
	//   형제 판별)에는 좌표만 근접한 링크까지 "탐색해봤다"고 인정하면 안 되기 때문 — 실측 확인
	//   (trip 000376_20260819094414 seq54, 2040425201 이 브릿지로 setSearchHistoryLinkList 에는
	//   들어가 화이트리스트를 통과, Begin 폴백에서 다시 채택되던 문제) (2026-08-21 최정우 추가)
	set<uint64> setGenuineSearchHistoryLinkList;

	// 연결 링크 목록
	listDepthLinkInfo listDepthLinkInfoList;

	// 직전 링크 ID로 링크 메타 정보 조회 (2026-07-08 최정우 주석 추가)
	PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(qwLinkID);
	if (!pstLinkInfo)
	{
		*pwErrorCode = NOT_FOUND_LINKID;
		return false;
	}

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
	// 같은 링크 노이즈 보정 기준점도 동일 내부 스케일로 변환 (2026-07-22 최정우 추가)
	stSgmtMatchInput.dfPrevMatchX *= 360000.0;
	stSgmtMatchInput.dfPrevMatchY *= 360000.0;
	// 역행 페널티 판정용 직전 링크 ID — 이 링크 위 후보만 dfPrevLinkPos 와 비교 (2026-07-20 최정우 추가)
	stSgmtMatchInput.qwPrevLinkID = qwLinkID;
	// 링크가 바뀌는 후보의 진입/진출 노드 판정용 — 직전 링크의 종료 노드 (2026-07-21 최정우 추가)
	stSgmtMatchInput.qwPrevEdNodeID = pstLinkInfo->qwEdNodeID;
	// 진입링크 후보의 확장 위치(역행 거리) 계산용 — 직전 링크 길이 (2026-07-22 최정우 추가)
	stSgmtMatchInput.dfPrevLinkLen = pstLinkInfo->dfLen;

	// 0 depth 세팅 
	// 링크 세그먼트 정보
	DEPTH_LINK_INFO_DATA stDepthLinkInfoData;

	stDepthLinkInfoData.qwLinkID = qwLinkID;
	stDepthLinkInfoData.dwStartTurnOffset = pstLinkInfo->dwTurnOffset;
	stDepthLinkInfoData.dwEndTurnOffset = stDepthLinkInfoData.dwStartTurnOffset + pstLinkInfo->nTurnCount;
	stDepthLinkInfoData.dwStartSgmtOffset = pstLinkInfo->dwSgmtOffset;
	stDepthLinkInfoData.dwEndSgmtOffset = stDepthLinkInfoData.dwStartSgmtOffset + pstLinkInfo->wSgmtCount;

	listDepthLinkInfoList.push_back(stDepthLinkInfoData);

	// depth 이내의 맵 매칭 유효 거리 이내 링크 목록
	list<MATCH_ENTRY> listMatchEntryList;			// 현재 depth 후보
	// 누적 후보 목록 — 최적 후보가 링크 경계 클램프면 다음 depth 를 확장해 함께 비교 (2026-07-15 최정우 추가)
	list<MATCH_ENTRY> listAllEntryList;
	sint16 nBestStep = 0;

	// 요청 depth 이내에서 검색
	for (uint16 i=0; i<=nSearchStep; ++i)
	{
		// 같은 depth 내 맵 매칭 유효 거리 이내 링크 목록 초기화
		listMatchEntryList.clear();

		// 같은 depth 링크 UID 목록 맵 매칭
		listDepthLinkInfo::iterator it = listDepthLinkInfoList.begin();
		for (; it != listDepthLinkInfoList.end(); ++it)
		{
			// 링크별 세그컨트 맵매칭
			LinkSgmtMapMatch(stSgmtMatchInput, *it, &listMatchEntryList);

			// 검색된 목록 저장 (중복 검사 처리용)
			setSearchHistoryLinkList.insert(it->qwLinkID);
			// 화이트리스트 노출용 — 좌표 근접 브릿지 링크는 제외 (2026-08-21 최정우 추가)
			if (!it->bGeometricBridge)
				setGenuineSearchHistoryLinkList.insert(it->qwLinkID);
		}

		// 이번 depth 후보를 누적 목록에 병합 (2026-07-15 최정우 수정)
		if (!listMatchEntryList.empty())
		{
			// depth(hop) 비용 가산 — 직전 링크에서 멀리 돌아온 후보일수록 불리하게 만든다.
			//   이 가산이 없으면 3 hop 떨어진 2.3m 후보가 1 hop 5.2m 후보를 이겨, 물리적으로
			//   불가능한 우회 경로가 선택된다(실측 연속 3점 42건 중 8건, 19%). (2026-08-22 최정우 추가)
			//
			//   [보류] 이 가산으로 8건 중 4건이 잡혔고, 남은 4건은 A→B 가 1 hop 이라 hop 비용이
			//   거의 안 걸리는 유형이다(B 에서 C 로 가는 길이 막힌다는 사실은 B 를 고를 때 알 수 없음).
			//   이를 잡으려면 "다음 점까지 본 경로 일관성 검사"가 필요하다 —
			//     조건    : hop(A→B) + hop(B→C) - hop(A→C) >= 4
			//     위치    : CommitPendingRow() (1틱 지연 커밋 구조 재사용, 추가 지연 없음)
			//     안전장치: 재매칭 후보가 원래보다 15m 이상 멀면 원래 값 유지
			//     현재 해당: 3건 / 전이 104건 (2.9%) — 2026-08-23 정답 기준선을 실주행 11트립
			//     전량(1,355점)으로 넓혀 다시 측정한 값. 3건 모두 같은 형태였다:
			//       · 직전 링크 A 의 종료 노드에서 지선 B 와 본선 C 가 함께 갈라진다
			//       · B 는 반대 차로로 건너가는 중앙 크로스오버 링크로 8.9~12.6m 로 매우 짧다
			//       · 차량은 A -> C 로 직진(A 와 C 는 직결)했는데 그 한 틱만 B 에 붙었다
			//     즉 "짧은 크로스오버 지선으로 1틱 튐"이 이 유형의 정체다. 교차로 한복판이라
			//     이격 차이가 0.06~2.4m 수준이고 방위각도 갈리지 않아 그 시점 정보만으로는
			//     못 가른다 — 다음 점을 봐야 한다는 위 설계의 근거가 된다.
			//   표본 3건으로 임계값을 정하면 과적합이라 실차 데이터가 쌓인 뒤 착수한다 (2026-08-22 판단).
			//   [2026-08-23 추가] hoppenalty_lenratio>0 이면 벌점을 링크 길이에 맞춰 깎는다.
			//     7m 지선에 8m 벌점이 붙으면 링크보다 벌점이 커서, 차가 그 위에 있어도 직전
			//     링크가 이긴다(합성 실측: 짧은 지선 진입 7건 중 3건 누락, 전부 직전 링크에 잔류).
			//     긴 링크로 우회하는 것은 종전대로 막고 짧은 조각만 통과시키는 게 목적이라
			//     후보별 링크 길이(dfLen)를 쓴다. 0 이면 종전 동작 그대로.
			if (i > 0)
			{
				const double dfHopBase = m_pcDataLoader->GetHopPenalty();
				const double dfLenRatio = m_pcDataLoader->GetHopLenRatio();
				// [가독성, 2026-09-15 최정우] 반복자 이름을 it -> itEntry 로 분리한다. 바깥
				//   136행에 같은 이름의 depth 목록 반복자가 있어 -Wshadow 경고가 났다. 지금은
				//   [정정] "136행" 은 당시 줄 번호 — 바깥 반복자는 이 함수의 "같은 depth 링크 UID 목록 맵 매칭"
				//   루프의 listDepthLinkInfo::iterator it 이다 (2026-10-06 최정우 주석 수정)
				//   안쪽이 이 블록 안에서만 살아 동작은 정상이지만, 블록 경계가 바뀌거나 코드가
				//   옮겨지면 조용히 바깥 반복자를 건드리게 되는 자리다.
				for (list<MATCH_ENTRY>::iterator itEntry = listMatchEntryList.begin();
					itEntry != listMatchEntryList.end(); ++itEntry)
				{
					double dfUnit = dfHopBase;
					if ((dfLenRatio > 0.0) && (itEntry->dfLen > 0.0))
					{
						const double dfCap = itEntry->dfLen * dfLenRatio;
						if (dfCap < dfUnit) dfUnit = dfCap;
					}
					itEntry->dfCost += static_cast<double>(i) * dfUnit;
				}
			}
			listAllEntryList.insert(listAllEntryList.end(),
				listMatchEntryList.begin(), listMatchEntryList.end());
			nBestStep = static_cast<sint16>(i);
		}

		// 누적 후보가 있으면 최적 후보 판정
		if (!listAllEntryList.empty())
		{
			listAllEntryList.sort();
			// 최적 후보가 링크 경계 클램프가 아니고(정상 내부 수선발) 방위각도 잘 맞으면, 더 깊이 갈 수 없으면 확정.
			//   경계 클램프면 차량이 링크 끝을 지난 것 → 연결 다음 링크에 더 나은 후보가 있을 수 있어 depth 확장.
			//   방위각 부적합(비용이 상한 도달)이면 직전 링크 위 내부 수선발이어도 회전·교차로일 수 있어
			//   depth 확장해 연결 링크와 비교(2026-07-18 최정우 추가) — 직전 링크에 계속 고정되는 것 방지.
			//   (두 경우 모두, 확장해도 다음 depth 후보가 더 나쁘면 sort 후 그대로 이 후보가 선택되므로 안전) (2026-07-15 최정우 추가)
			//   정리하면 확정 조건은 "(경계 클램프 아님 AND 방위각 부적합 아님) OR 최대 depth 도달" 이다.
			//   첫 줄의 "더 깊이 갈 수 없으면" 은 뒤쪽 OR 조건(i == nSearchStep)을 가리킨다 (2026-10-06 최정우 주석 추가)
			if ((!IsBoundaryClamped(listAllEntryList.front())
					&& !IsPoorAngleFit(listAllEntryList.front())) || (i == nSearchStep))
			{
				*pwErrorCode = NO_ERROR;
				if (pstTraceCtx != nullptr)
					pstTraceCtx->nMatchedStep = nBestStep;
				GetMatchEntry(&listAllEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
				if (pvtPathLinkIDs != nullptr)
					ReconstructPath(pstMatchEntry->qwLinkID, mapParentLink, pvtPathLinkIDs);
				if (psetSearchHistoryLinkID != nullptr)
					*psetSearchHistoryLinkID = setGenuineSearchHistoryLinkList;
				return true;
			}
		}

		// 현재 검색 단계가 최대 설정 검색 단계이면 다음 depth 링크 목록 정보를 가져오지 않음
		if (i == nSearchStep) break;

		// 현재 depth 링크 목록이 맵 매칭에 실패시 다음 depth 링크 목록 정보
		if (!GetLinkDepthInfo(&setSearchHistoryLinkList, &listDepthLinkInfoList, &mapParentLink, stSgmtMatchInput.nSpeed))
		{
			// 더 확장할 연결 링크가 없으면, 지금까지 누적 후보가 있으면 그걸로 확정 (2026-07-15 최정우 수정)
			if (!listAllEntryList.empty())
			{
				*pwErrorCode = NO_ERROR;
				if (pstTraceCtx != nullptr)
					pstTraceCtx->nMatchedStep = nBestStep;
				GetMatchEntry(&listAllEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
				if (pvtPathLinkIDs != nullptr)
					ReconstructPath(pstMatchEntry->qwLinkID, mapParentLink, pvtPathLinkIDs);
				if (psetSearchHistoryLinkID != nullptr)
					*psetSearchHistoryLinkID = setGenuineSearchHistoryLinkList;
				return true;
			}
			*pwErrorCode = MAP_MATCH_FAIL;
			return false;
		}
	}

	// 루프 종료 후 누적 후보가 있으면 확정 (경계 클램프만 있었던 경우)
	if (!listAllEntryList.empty())
	{
		*pwErrorCode = NO_ERROR;
		if (pstTraceCtx != nullptr)
			pstTraceCtx->nMatchedStep = nBestStep;
		GetMatchEntry(&listAllEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
		if (pvtPathLinkIDs != nullptr)
			ReconstructPath(pstMatchEntry->qwLinkID, mapParentLink, pvtPathLinkIDs);
		if (psetSearchHistoryLinkID != nullptr)
			*psetSearchHistoryLinkID = setGenuineSearchHistoryLinkList;
		return true;
	}

	*pwErrorCode = MAP_MATCH_FAIL;
	return false;
}

/**
 * @brief 최적 후보가 링크 경계(시작/끝)에 스냅(클램프)됐는지 판정 (2026-07-15 최정우 추가)
 * @param[in] stMatchEntry 판정 대상 후보
 * @return true(경계 클램프 — 링크 끝/시작에 수선발 스냅), false(내부 수선발)
 * @remark
 *   링크 시작점→수선발 거리(m) = wLenFromLink(링크시작→세그먼트시작) + dfSgmtMatchLen(세그먼트내 거리).
 *   이 값이 0 근처(시작) 또는 링크길이(dfLen) 근처(끝)이면 세그먼트 끝점에 스냅된 경계 클램프.
 *   차량이 링크 끝을 지나 다음 링크로 넘어간 상황에서 발생 → 연결 링크 확장 판단에 사용.
*/
bool CContinueMapMatch::IsBoundaryClamped(const MATCH_ENTRY& stMatchEntry)
{
	if (stMatchEntry.dfLen <= 0.0)
		return false;

	double dfFootFromStart = static_cast<double>(stMatchEntry.wLenFromLink) + stMatchEntry.dfSgmtMatchLen;
	return (dfFootFromStart <= MM_CLAMP_EPS) ||
	       (dfFootFromStart >= (stMatchEntry.dfLen - MM_CLAMP_EPS));
}

/**
 * @brief 최적 후보의 방위각이 심하게 안 맞는지 판정 (2026-07-18 최정우 추가)
 * @param[in] stMatchEntry 판정 대상 후보
 * @return true(방위각 비용이 상한(MM_DIR_MAX_PENALTY)에 도달 — 방향이 거의 안 맞음), false(정상)
 * @remark
 *   dfAngleCost = dfCost - dfIntersectLenSgmt (방위각 비용만 분리, GISUtil::SgmtMatch 참고).
 *   상한 도달 = 방향이 심하게 어긋남에도 직전 링크 위에 내부 수선발이 잡혀 depth 확장이
 *   안 되던 경우(회전·교차로 구간에서 직전 링크에 계속 고정되는 현상) 방지용.
 *   [보완] "방향이 거의 안 맞음" 은 과장이다 — 상한 15m 는 20km/h 이상(가중치 1.0)에서 각도차 15° 만
 *   돼도 도달한다. 반대로 SgmtMatch 의 각도차는 정·역 중 작은 쪽이라 최대 90° 이므로, 가중치가
 *   1/6 미만인 저속(약 7km/h 이하)과 heading 없음(bHasHeading=false)에서는 이 판정이 절대 true 가
 *   되지 않는다 (2026-10-06 최정우 주석 수정)
*/
bool CContinueMapMatch::IsPoorAngleFit(const MATCH_ENTRY& stMatchEntry)
{
	// dfAngleCost = (dfIntersectLenSgmt+cap) - dfIntersectLenSgmt 형태로 역산되어 부동소수점
	//   반올림 오차로 정확히 cap 값이 아닐 수 있어 허용오차(0.01m) 적용 (2026-07-18 최정우 수정)
	return stMatchEntry.dfAngleCost >= (MM_DIR_MAX_PENALTY - 0.01);
}

/**
 * @brief 링크별 세그컨트 맵매칭
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] stDepthLinkInfoData 링크 회전 정보 및 세그먼트 정보
 * @param[out] plistMatchEntryList 검색 정보 목록
 * @param[in] bAllowOppositeCheck 역행 의심 시 opposite link(qwOppositeLinkID) 후보 재귀 평가 허용 여부 —
 *   opposite link 자체를 평가하는 재귀 호출에서는 false 로 넘겨 상호 재귀를 막는다 (2026-08-19 최정우 추가)
 * @return plistMatchEntryList 가 비어 있지 않으면 true.
 *   [주석 정정, 2026-09-21 최정우] 종전 "true(성공), false(실패)" 는 오해를 부른다 — 이 목록은
 *   **호출측이 depth 단위로만 비우는 누적 목록**이라, 이번 링크에서 후보를 하나도 못 찾아도
 *   같은 depth 의 앞선 링크가 넣어둔 것이 있으면 true 가 나온다. 즉 "이 링크가 매칭됐는가" 가
 *   아니다. 현재 호출부(StartMapMatch·TryOppositeLinkCandidate·TryNearbyRoadNameCandidate)는
 *   모두 반환값을 쓰지 않는다 — 쓰려면 호출 전후의 목록 크기를 비교해야 한다.
*/
bool CContinueMapMatch::LinkSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput,
		DEPTH_LINK_INFO_DATA& stDepthLinkInfoData, list<MATCH_ENTRY> *plistMatchEntryList,
		bool bAllowOppositeCheck)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	for (uint32 i=stDepthLinkInfoData.dwStartSgmtOffset; i<stDepthLinkInfoData.dwEndSgmtOffset; ++i)
	{
		SGMT_MATCH_RES stSgmtMatchRes;

		// 링크 세그먼트 정보
		PLINK_SGMT_INFO pstLinkSgmtInfo = m_pcDataLoader->GetLinkSgmtInfo(i);
		if (!pstLinkSgmtInfo)
			continue;

		// 세그먼트 정보 구조체
		SGMT_INFO stSgmtInfo;

		// 세그먼트 X,Y 좌표
		stSgmtInfo.stPoint.dfX = static_cast<double>(pstLinkSgmtInfo->dwX);
		stSgmtInfo.stPoint.dfY = static_cast<double>(pstLinkSgmtInfo->dwY);

		// 세그먼트 진행 방향(방위각)
		stSgmtInfo.nDirAng = static_cast<sint16>(pstLinkSgmtInfo->wDirAng);

		// 세그먼트 길이 형변환
		stSgmtInfo.dfLen = static_cast<double>(pstLinkSgmtInfo->wLenSgmt);

		// 링크 ID
		stSgmtInfo.qwLinkID = pstLinkSgmtInfo->qwLinkID;

		// INTERSECT_LEN(GPS↔세그먼트 교차점 거리)·방위 기본 매칭 (2026-07-08 최정우 주석 추가)
		if (!m_cGISUtil.SgmtMatch(stSgmtMatchInput, stSgmtInfo, &stSgmtMatchRes))
			continue;

		// 매칭 성공이면 링크 정보
		PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(stSgmtInfo.qwLinkID);
		if (!pstLinkInfo) continue;

		// 왕복분리 전용 반대편 링크(qwOppositeLinkID)가 있는데 이번 후보가 "역방향 적합"으로
		//   채택됐으면 버린다 — 전용 반대편 링크가 있다는 건 그 방향으로는 반대편 링크를 타야
		//   정상이라는 뜻이라, 이 링크 자체를 거꾸로 인정하면 안 된다. SgmtMatch()의 역방향 허용은
		//   원래 "링크 하나로 양방향을 표현하는 진짜 양방향 단일 링크"를 위한 것인데, 반대편이
		//   따로 있는 링크에도 똑같이 적용되고 있었다(실측 000376_20260819094414 M45 — 왕복분리
		//   반대편 2040424401 을 역방향으로 오매칭, heading 84°가 그 링크 정방향(약 264°)과는
		//   정반대인데 역방향 허용으로 통과함. 그 결과 RL-Z00002 NODE_STEP 세션이 그 한 틱 때문에
		//   쪼개지는 후속 결함까지 발생). 반대편 없는(진짜 양방향 단일링크) 도로는 기존대로 역방향
		//   허용 유지 (2026-08-24 최정우 추가)
		if (stSgmtMatchRes.bReverseFit && (pstLinkInfo->qwOppositeLinkID != 0))
			continue;

		MATCH_ENTRY stMatchEntry;

		stMatchEntry.dfMatchX = stSgmtMatchRes.stMatchPoint.dfX;
		stMatchEntry.dfMatchY = stSgmtMatchRes.stMatchPoint.dfY;
		stMatchEntry.dfSgmtMatchLen = stSgmtMatchRes.dfSgmtMatchLen;
		stMatchEntry.dfIntersectLenSgmt = stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.bSgmtClamped = stSgmtMatchRes.bSgmtClamped;			// 세그먼트 끝점 스냅 여부 (2026-07-21 최정우 추가)
		stMatchEntry.bHasHeading = stSgmtMatchRes.bHasHeading;				// heading 값 존재 여부 (2026-07-22 최정우 추가)
		stMatchEntry.bClampTrustedByHeading = stSgmtMatchRes.bClampTrustedByHeading;	// 클램프+heading 신뢰 구제 신호 (2026-08-20 최정우 추가)
		// ── 연속 맵매칭 고도 보조 비용 가산 (Begin 미적용) ──
		//   dfCost = INTERSECT_LEN + 방위각비용 + CalcAltRoadPenalty(Δalt, ROAD_TYPE)
		// 값이 작을수록 우선 — 보너스(음수)면 동일 거리·방향 후보보다 유리
		//   예) 기본 25 + 고도−3 = 22 → 같은 고가·Δ6m 후보가 일반도로(+10)보다 선택
		//   [정정] 위 예의 "−3" 은 현행 공식과 맞지 않는다 — |Δalt|≤alt_gap 이고 ROAD_TYPE 이 직전과
		//   같으면 −alt_penalty(config 10 → −10)가 붙는다(GISUtil::CalcAltRoadPenalty). 즉 25−10=15 vs
		//   25+10=35 (2026-10-06 최정우 주석 수정)
		//   (2026-07-08 최정우 추가)
		stMatchEntry.dfAngleCost = stSgmtMatchRes.dfCost - stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfAltAdj = m_cGISUtil.CalcAltRoadPenalty(stSgmtMatchInput, pstLinkInfo->nRoadType, m_stAltitudeConfig);
		stMatchEntry.dfCost = stSgmtMatchRes.dfCost + stMatchEntry.dfAltAdj;
		// 좌표 근접(BridgeNearbyLinkStarts)만으로 발견된 후보는 위상 우선순위 페널티 — 같은 경합에
		//   진짜 TURNINFO 연결 후보가 있으면 근소한 차이로는 못 이기게 한다 (2026-08-21 최정우 추가)
		if (stDepthLinkInfoData.bGeometricBridge)
			stMatchEntry.dfCost += MM_GEOM_BRIDGE_PENALTY;
		stMatchEntry.nDirAngleDiff = stSgmtMatchRes.nDirAngleDiff;
		stMatchEntry.qwLinkID = stSgmtMatchRes.qwLinkID;
		stMatchEntry.wLenFromLink = pstLinkSgmtInfo->wLenFromLink;
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

		// 직전 매칭 위치보다 뒤로 가는 후보 감지 — 같은 링크 위 GPS 노이즈로 인한 역행 스냅(오락가락)
		//   구분용. 링크가 바뀌면(정상 전진) 비교 대상 아님 (2026-07-20 최정우 추가)
		if (stSgmtMatchInput.bHasPrevLinkPos && (stMatchEntry.qwLinkID == stSgmtMatchInput.qwPrevLinkID))
		{
			double dfCurPos = static_cast<double>(stMatchEntry.wLenFromLink) + stMatchEntry.dfSgmtMatchLen;
			if (dfCurPos < stSgmtMatchInput.dfPrevLinkPos)
			{
				double dfBackward = stSgmtMatchInput.dfPrevLinkPos - dfCurPos;

				// 역행 의심(bReverseSuspect) — 위치가 조금이라도 뒤로 갔고 heading 도 세그먼트
				//   역방향에 더 가까울 때만 표시. RawLogWorker 의 연속역행(reverse_confirm)
				//   판정이 GPS 노이즈에 흔들리지 않고 heading 이 뒷받침되는 경우만 스트릭에 반영하게 함
				//   (2026-07-21 최정우 추가)
				if ((dfBackward > MM_REVERSE_SUSPECT_EPS) && stSgmtMatchRes.bReverseFit)
				{
					stMatchEntry.bReverseSuspect = true;

					// 반대방향(왕복분리) 짝 링크가 있으면 추가 후보로 평가 — 그래프(TURNINFO)상
					//   연결 안 된 물리적 짝 링크를 후보에 끌어와, 비용이 더 낮으면(정방향 적합)
					//   listAllEntryList 정렬(operator<)로 자연스럽게 우선 선택되게 한다 (2026-08-19 최정우 추가)
					//   공식 짝이 없으면(CreateData 페어링 실패) 같은 도로명 인접 링크로 대체 탐색
					//   (2026-08-26 최정우 추가)
					//   [정정] 이 분기(같은 링크 역행 + bReverseFit)에서는 짝 링크 쪽이 실행될 수 없다 — 위에서
					//   "bReverseFit && qwOppositeLinkID != 0" 후보를 이미 continue 로 버렸으므로(2026-08-24)
					//   여기 도달하면 qwOppositeLinkID 는 항상 0 이고 실제로는 TryNearbyRoadNameCandidate 만 돈다.
					//   짝 링크 평가는 아래 "링크가 바뀌는 후보(진입 링크)" 분기에서만 실행된다 (2026-10-06 최정우 주석 수정)
					if (bAllowOppositeCheck)
					{
						if (pstLinkInfo->qwOppositeLinkID != 0)
							TryOppositeLinkCandidate(stSgmtMatchInput, pstLinkInfo->qwOppositeLinkID, plistMatchEntryList);
						else
							TryNearbyRoadNameCandidate(stSgmtMatchInput, pstLinkInfo, stSgmtInfo.qwLinkID, plistMatchEntryList);
					}
				}
				else if (dfBackward > MM_REVERSE_SUSPECT_EPS)
				{
					// heading 이 역방향 쪽이 아님(bReverseFit=false) — 이 뒤로 밀린 위치가
					//   "확실한 노이즈"인지 "판단 불가"인지 구분한다.
					//   확실한 노이즈: heading 이 있고, 방위각이 정방향과 잘 맞고(각도비용이 상한 미도달),
					//     GPS 오차(INTERSECT_LEN)도 작음 → 매칭 좌표를 직전 위치보다 살짝 앞으로 보정해
					//     지도에 역주행처럼 튀어 보이지 않게 하고, MATCH_STATUS 는 그대로 둔다(MATCHED 유지).
					//   판단 불가: heading 이 없거나(NO_ANGLE) 각도가 애매/큼 → 노이즈라 단정 못 하므로
					//     좌표는 계산된 값 그대로 두고 SKIP 표시만 얹어 다운스트림이 신뢰하지 않게 한다.
					//   (2026-07-22 최정우 추가)
					// 저속(MM_SPEED_LOW_KMH 이하)은 heading 유무와 무관하게 "확실한 노이즈"로도 인정한다 —
					//   2026-08-26 수정으로 저속일 땐 heading 자체를 없는 것으로 취급(bHasHeading=false)하게
					//   됐는데, 그 결과 heading 기반 확정 조건을 절대 못 만족해 정차 중 GPS 튐이 매번
					//   판단불가(SKIP)로 빠지는 부작용이 생겼다. 저속은 heading보다 오히려 더 강한
					//   "역주행일 리 없다"는 근거이므로(물리적으로 저속 이하 상태에서 후퇴는 GPS 노이즈일
					//   수밖에 없음) heading 부재를 이걸로 대신 메운다 (사용자 지시, 2026-08-28 최정우 추가)
					bool bLowSpeedStationary = (stSgmtMatchInput.nSpeed >= 0)
						&& (stSgmtMatchInput.nSpeed <= MM_SPEED_LOW_KMH);
					bool bPoorAngle = IsPoorAngleFit(stMatchEntry);
					bool bDefiniteNoise = (stMatchEntry.dfIntersectLenSgmt <= MM_CLAMP_SKIP_LEN)
						&& (bLowSpeedStationary || (stSgmtMatchRes.bHasHeading && !bPoorAngle));

					// 원시좌표·방향이 직전 tick과 완전히 동일하면(bSameRawAndHeadingAsPrev) 실제로
					//   정지해 있다는 뜻이라 1m 강제전진을 적용하지 않는다 — 세그먼트 매칭이 이미
					//   계산한 자연 좌표(stMatchEntry.dfMatchX/Y, 위에서 산출됨)를 그대로 쓴다.
					//   강제전진을 걸면 정지 중에도 위치가 계속 앞으로 밀리는 누적 드리프트가 생김
					//   (실측 000376_20260826150010 seq1~38, 최대 15m 드리프트 확인)
					//   (사용자 지시, 2026-09-02 최정우 추가)
					if (bDefiniteNoise && stSgmtMatchInput.bSameRawAndHeadingAsPrev)
					{
						// 아무 것도 하지 않음 — stMatchEntry.dfMatchX/Y/dfSgmtMatchLen 은 이미
						//   자연 계산값이다.
					}
					else if (bDefiniteNoise)
					{
						// 보정 전 값을 남겨둔다 — 결과가 말이 안 되면 되돌린다 (2026-08-23 최정우 추가)
						const double dfKeepX = stMatchEntry.dfMatchX;
						const double dfKeepY = stMatchEntry.dfMatchY;
						const double dfKeepSgmtLen = stMatchEntry.dfSgmtMatchLen;

						double dfNewPos = stSgmtMatchInput.dfPrevLinkPos + MM_NOISE_FORWARD_NUDGE_M;
						if (dfNewPos >= stMatchEntry.dfLen)
						{
							// 링크 끝(END 노드)을 넘으면 END 노드 좌표로 클램프
							stMatchEntry.dfMatchX = stMatchEntry.dfEdNodeX;
							stMatchEntry.dfMatchY = stMatchEntry.dfEdNodeY;
							stMatchEntry.dfSgmtMatchLen = stMatchEntry.dfLen
								- static_cast<double>(stMatchEntry.wLenFromLink);
						}
						else
						{
							// 이번 후보(현재 GPS) 자신의 계산값이 아니라, 마지막으로 신뢰했던
							//   실제 매칭 좌표(dfPrevMatchX/Y)를 기준점으로 삼아 세그먼트 방향으로
							//   정확히 MM_NOISE_FORWARD_NUDGE_M(1m)만 전진시킨다 — 이번 후보의
							//   좌표 자체가 노이즈라 못 믿는 상황이므로, 노이즈가 섞인 값을 기준으로
							//   재보정하지 않고 마지막 확실한 지점에서 다시 계산한다 (2026-07-22 최정우 수정)
							// [버그 수정, 2026-09-11 최정우] dfPrevMatchX/Y 는 좌표스케일(도×360000)인데
							//   MM_NOISE_FORWARD_NUDGE_M(실미터)을 MM_COORD_UNITS_PER_M 변환 없이 그대로
							//   더해 실제로는 1m 의도가 0.31m만 전진했다. GISUtil.cpp
							//   OffsetScaledCoordByMeters()(2026-08-26, 익명 네임스페이스라 여기서 직접
							//   재사용은 못 하고 동일 수식을 인라인) 와 동일하게 경도축 cos(위도) 보정까지
							//   반영 — 안 하면 고위도·동서방향 세그먼트에서 실제 전진거리가 또 틀어진다.
							{
								const double dfR = 6378137.0;								// WGS84 장반경(m)
								const double dfMetersPerDeg = dfR * RAD(1.0);
								const double dfLatRad = RAD(stSgmtMatchInput.dfPrevMatchY / 360000.0);
								const double dfBearingRad = RAD(static_cast<double>(stSgmtInfo.nDirAng));
								const double dfDLatDeg = (MM_NOISE_FORWARD_NUDGE_M * cos(dfBearingRad)) / dfMetersPerDeg;
								const double dfDLonDeg = (MM_NOISE_FORWARD_NUDGE_M * sin(dfBearingRad))
									/ (dfMetersPerDeg * cos(dfLatRad));
								stMatchEntry.dfMatchX = stSgmtMatchInput.dfPrevMatchX + dfDLonDeg * 360000.0;
								stMatchEntry.dfMatchY = stSgmtMatchInput.dfPrevMatchY + dfDLatDeg * 360000.0;
							}
							stMatchEntry.dfSgmtMatchLen = dfNewPos - static_cast<double>(stMatchEntry.wLenFromLink);
						}

						// 온전성 검사 — 보정은 "마지막 신뢰 좌표에서 1m 전진"이라 결과가 현재 GPS
						//   근처여야 한다. 멀면 기준점(dfPrevMatchX/Y)이 유효하지 않았다는 뜻이므로
						//   보정을 포기한다. 자세한 근거는 DataDefine.h MM_NOISE_FIX_SANITY_M 주석.
						//   정상 보정은 결과가 GPS 에서 수 m 이내라 여기 걸릴 수 없다 (2026-08-23 최정우 추가)
						const double dfSanity = MM_NOISE_FIX_SANITY_M * MM_COORD_UNITS_PER_M;
						const double dfGapX = stMatchEntry.dfMatchX - stSgmtMatchInput.stPoint.dfX;
						const double dfGapY = stMatchEntry.dfMatchY - stSgmtMatchInput.stPoint.dfY;
						if ((dfGapX * dfGapX + dfGapY * dfGapY) > (dfSanity * dfSanity))
						{
							LOGFMTW("noise fix rejected! corrected=[%.6f,%.6f] gps=[%.6f,%.6f] link=[%llu]",
								stMatchEntry.dfMatchX / 360000.0, stMatchEntry.dfMatchY / 360000.0,
								stSgmtMatchInput.stPoint.dfX / 360000.0,
								stSgmtMatchInput.stPoint.dfY / 360000.0,
								static_cast<unsigned long long>(stMatchEntry.qwLinkID));
							stMatchEntry.dfMatchX = dfKeepX;
							stMatchEntry.dfMatchY = dfKeepY;
							stMatchEntry.dfSgmtMatchLen = dfKeepSgmtLen;
						}
					}
					else
					{
						stMatchEntry.bAmbiguousReverse = true;
					}
				}
			}
			else if ((pstLinkInfo->szRoadName[0] != '\0') && bAllowOppositeCheck
				&& ((stSgmtMatchInput.nSpeed < 0) || (stSgmtMatchInput.nSpeed > MM_SPEED_LOW_KMH))
				&& (stMatchEntry.dfIntersectLenSgmt > MM_ROADNAME_SEARCH_MIN_M))
			{
				// 뒤로 가지 않는 정상 전진인데도 GPS와 거리가 큼 — 세션이 앵커링된 채 더 가까운
				//   평행 링크(왕복분리 등)를 놓치고 있을 가능성. 도로명 기준 인접 탐색으로 보완
				//   (2026-08-26 최정우 추가, MM_ROADNAME_SEARCH_MIN_M 주석 근거)
				TryNearbyRoadNameCandidate(stSgmtMatchInput, pstLinkInfo, stSgmtInfo.qwLinkID, plistMatchEntryList);
			}
		}
		// 링크가 바뀌는 후보 — 진입/진출 노드 판정 (2026-07-21 최정우 추가 — 진입링크 역행 감지)
		//   위 역행 페널티/의심 판정은 "같은 링크 위에서 뒤로 감"만 잡아서, 후보 링크 자체가
		//   바뀌면(예: 링크 경계 클램프로 depth 확장돼 인접 링크가 선택되는 경우) 전혀 걸리지 않았다.
		//   그 결과 reverse_confirm(연속역행 확정) 스트릭 판정 대상에서 완전히 빠져, 저속+GPS 노이즈만
		//   있어도 진입 링크(직전 링크와 같은 노드로 "들어가는" 링크, 즉 진행 방향상 나올 수 없는 링크)로
		//   단 1건만에 확정 매칭되는 문제가 있었다.
		//   정상 전진(진출)이라면 후보의 시작 노드(qwStNodeID)가 직전 링크의 종료 노드와 같아야 한다.
		//   대신 후보의 "종료" 노드가 직전 링크의 종료 노드와 같다면, 그 후보는 같은 노드로 들어가는
		//   방향(진입)이라는 뜻 — 위상적으로 확실한 역행 신호이므로 heading/거리와 무관하게 표시한다.
		//   (같은 링크 위 판정과 마찬가지로 여기서도 SKIP 이 아니라 "의심" 신호만 세팅 — reverse_confirm
		//   만큼 연속되어야 실제 역행/유턴으로 확정되고, 1건짜리 노이즈는 이 신호만으로 SKIP·앵커 고정된다)
		else if (stSgmtMatchInput.qwPrevEdNodeID != 0 && (stMatchEntry.qwLinkID != stSgmtMatchInput.qwPrevLinkID)
			&& (stMatchEntry.qwStNodeID != stSgmtMatchInput.qwPrevEdNodeID)
			&& (stMatchEntry.qwEdNodeID == stSgmtMatchInput.qwPrevEdNodeID))
		{
			// 위상적으로 확실한 역행이므로 의심 신호는 무조건 세팅(거리·margin 무관) — 후보의
			//   "종료" 노드가 직전 링크의 종료 노드와 같다면, 그 후보는 같은 노드로 들어가는
			//   방향(진입)이라는 뜻 — heading/거리와 무관하게 표시한다 (2026-07-22 최정우 추가)
			stMatchEntry.bReverseSuspect = true;

			// 반대방향(왕복분리) 짝 링크가 있으면 추가 후보로 평가 — 없으면 같은 도로명 인접
			//   링크로 대체 탐색 (2026-08-19 최정우 추가, 2026-08-26 최정우 대체탐색 추가)
			if (bAllowOppositeCheck)
			{
				if (pstLinkInfo->qwOppositeLinkID != 0)
					TryOppositeLinkCandidate(stSgmtMatchInput, pstLinkInfo->qwOppositeLinkID, plistMatchEntryList);
				else
					TryNearbyRoadNameCandidate(stSgmtMatchInput, pstLinkInfo, stSgmtInfo.qwLinkID, plistMatchEntryList);
			}
		}

		plistMatchEntryList->push_back(stMatchEntry);
	}

	return (!plistMatchEntryList->empty()) ? true : false;
}

/**
 * @brief 역행 의심 후보의 반대방향(왕복분리) 짝 링크를 추가 후보로 평가
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] qwOppositeLinkID 짝 링크 ID (LINK_INFO.qwOppositeLinkID, 0=없음)
 * @param[out] plistMatchEntryList 검색 정보 목록 — 짝 링크의 매칭 결과를 추가로 append
 * @return void
 * @remark
 *   그래프(TURNINFO) 기반 depth 확장으로는 도달 불가능한, 물리적으로만 가까운 반대방향 짝
 *   링크(CreateData::CBinaryMaker::ComputeOppositeLinkPairs 가 사전 계산)를 강제로 후보에
 *   포함시킨다. LinkSgmtMapMatch 를 bAllowOppositeCheck=false 로 재귀 호출해, 짝 링크 평가
 *   결과가 다시 자기 자신을 짝으로 끌어오는 상호 재귀를 막는다 (2026-08-19 최정우 추가)
*/
void CContinueMapMatch::TryOppositeLinkCandidate(SGMT_MATCH_INPUT& stSgmtMatchInput,
		const uint64& qwOppositeLinkID, list<MATCH_ENTRY> *plistMatchEntryList)
{
	if (qwOppositeLinkID == 0)
		return;

	PLINK_INFO pstOppLinkInfo = m_pcDataLoader->GetLinkInfo(qwOppositeLinkID);
	if (pstOppLinkInfo == nullptr)
		return;

	DEPTH_LINK_INFO_DATA stOppDepthInfo;
	stOppDepthInfo.qwLinkID = qwOppositeLinkID;
	stOppDepthInfo.dwStartTurnOffset = pstOppLinkInfo->dwTurnOffset;
	stOppDepthInfo.dwEndTurnOffset = stOppDepthInfo.dwStartTurnOffset + pstOppLinkInfo->nTurnCount;
	stOppDepthInfo.dwStartSgmtOffset = pstOppLinkInfo->dwSgmtOffset;
	stOppDepthInfo.dwEndSgmtOffset = stOppDepthInfo.dwStartSgmtOffset + pstOppLinkInfo->wSgmtCount;

	LinkSgmtMapMatch(stSgmtMatchInput, stOppDepthInfo, plistMatchEntryList, false);
}

/**
 * @brief TryOppositeLinkCandidate 의 대상이 없을 때(공식 짝 링크 미등록) GPS 주변에서 같은
 *   도로명의 다른 링크를 찾아 추가 후보로 평가
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] pstCurLinkInfo 현재(역행 의심) 링크 정보 — 도로명 비교 기준
 * @param[in] qwCurLinkID 현재 링크 ID — 자기 자신 제외용
 * @param[out] plistMatchEntryList 검색 정보 목록 — 찾은 후보들의 매칭 결과를 추가로 append
 * @return void
 * @remark BridgeNearbyLinkStarts 와 동일한 그리드 반경 탐색(GetNearGridID)을 쓰지만, "막다른
 *   링크 끝점 근처"가 아니라 "지금 GPS 위치 근처"에서 찾고, 도로명이 같은 링크만 후보로 인정한다.
 *   찾은 후보는 bGeometricBridge=true 로 표시해 위상(TURNINFO) 연결 후보보다 근소한 차이로는
 *   못 이기게 한다(MM_GEOM_BRIDGE_PENALTY, LinkSgmtMapMatch 재사용) (2026-08-26 최정우 추가)
*/
void CContinueMapMatch::TryNearbyRoadNameCandidate(SGMT_MATCH_INPUT& stSgmtMatchInput,
		PLINK_INFO pstCurLinkInfo, const uint64& qwCurLinkID, list<MATCH_ENTRY> *plistMatchEntryList)
{
	if ((pstCurLinkInfo == nullptr) || (pstCurLinkInfo->szRoadName[0] == '\0'))
		return;

	double dfRawX = stSgmtMatchInput.stPoint.dfX / 360000.0;
	double dfRawY = stSgmtMatchInput.stPoint.dfY / 360000.0;

	uint32 dwGridID = m_cGISUtil.GetGridID(dfRawX, dfRawY);
	if (dwGridID == static_cast<uint32>(INVALID_GRID_ID))
		return;

	vector<uint32> vtNearGridIDList;
	vtNearGridIDList.push_back(dwGridID);
	// [버그 수정, 2026-09-15 최정우 — 소스 재검토] GetNearGridID() 에 **도 단위** 좌표를 넘긴다.
	//   종전에는 stSgmtMatchInput 을 그대로 넘겼는데, StartMapMatch() 가 진입부에서 stPoint 를
	//   이미 내부 스케일(×360000)로 바꿔둔 상태다(바로 위 dfRawX/dfRawY 가 /360000 지역변수를 따로 만드는
	//   이유가 그것). GetNearGridID→GridBorderDistance 는 doc 주석대로 도 단위를 전제로
	//   HaversineMetersDeg 를 쓰므로, 스케일 값이 들어가면 경계거리가 미터가 아닌 수천 km 로
	//   나온다(실측: 판교 기준 66.8~267.2m 가 나와야 할 자리에 4,703~11,866km). 그 값이
	//   nRadius(최대 250m)와 비교되니 **인접 셀이 단 한 번도 추가되지 않았다** — 반경과 무관하게
	//   항상 자기 셀 1개만 탐색(정상이면 최대 6개). 셀 크기가 300m 라 셀 경계 근처에서는 바로 옆
	//   반대방향 평행 링크를 영영 못 찾는데, 그 링크를 찾는 것이 이 함수의 존재 목적이다.
	//   에러 로그도 안 남아 지금까지 드러나지 않았다.
	//   나머지 호출부 3곳(BeginMapMatch.cpp:241/529, 아래 BridgeNearbyLinkStarts)은 전부 도 단위로
	//   [정정] "241/529" 는 당시 줄 번호 — CBeginMapMatch::StartMapMatch()·FindGeomNearest() 의
	//   GetNearGridID 호출(둘 다 ×360000 변환 직전)을 가리킨다 (2026-10-06 최정우 주석 수정)
	//   넘기고 있어 이 한 곳만 어긋나 있었다. 호출측 구조체를 건드리지 않도록 복사본을 쓴다
	//   (SGMT_MATCH_INPUT 은 POD 스칼라뿐이라 복사 비용 무시 가능).
	SGMT_MATCH_INPUT stGridInput = stSgmtMatchInput;		// nRadius 등 나머지 조건은 그대로 유지
	stGridInput.stPoint.dfX = dfRawX;
	stGridInput.stPoint.dfY = dfRawY;
	m_cGISUtil.GetNearGridID(dwGridID, stGridInput, vtNearGridIDList);

	// 이 함수는 로그가 하나도 없어서, 위 좌표 단위 버그로 인접 셀 탐색이 3주간 죽어 있었는데도
	//   아무 흔적이 남지 않았다. 진입 사실과 실제 탐색 셀 수를 남긴다 (2026-09-15 최정우 추가)
	LOGFMTD("nearby road name candidate search!link=[%llu] road=[%s] radius=[%d] grids=[%d]",
		static_cast<unsigned long long>(qwCurLinkID), pstCurLinkInfo->szRoadName,
		static_cast<int>(stGridInput.nRadius), static_cast<int>(vtNearGridIDList.size()));

	set<uint64> setTried;
	setTried.insert(qwCurLinkID);

	for (size_t g = 0; g < vtNearGridIDList.size(); ++g)
	{
		PGRID_INFO pstGridInfo = m_pcDataLoader->GetGridInfo(vtNearGridIDList[g]);
		if (!pstGridInfo)
			continue;

		uint32 dwStart = pstGridInfo->dwSgmtOffset;
		uint32 dwEnd = dwStart + pstGridInfo->wSgmtCount;

		for (uint32 s = dwStart; s < dwEnd; ++s)
		{
			PGRID_SGMT_INFO pstSgmt = m_pcDataLoader->GetGridSgmtInfo(s);
			if (!pstSgmt)
				continue;
			// [2026-09-21 최정우 보완] 종전에는 **최종 채택된 링크만** setTried 에 넣어서,
			//   도로명 불일치·위상 미연결로 탈락한 링크는 같은 링크의 다른 세그먼트를 만날 때마다
			//   GetLinkInfo + strncmp + 거리 4회 계산을 처음부터 다시 했다. 링크 하나가 세그먼트
			//   수십 개로 쪼개져 있으므로 그만큼 반복된다. 판정 결과는 세그먼트와 무관하게 링크
			//   단위로 같으므로, 한 번 본 링크는 결과와 상관없이 기록해 건너뛴다(동작 동일).
			if (setTried.find(pstSgmt->qwLinkID) != setTried.end())
				continue;
			setTried.insert(pstSgmt->qwLinkID);

			PLINK_INFO pstCandInfo = m_pcDataLoader->GetLinkInfo(pstSgmt->qwLinkID);
			if (!pstCandInfo)
				continue;
			if (strncmp(pstCandInfo->szRoadName, pstCurLinkInfo->szRoadName, sizeof(pstCandInfo->szRoadName)) != 0)
				continue;

			// 위상 미연결 후보 배제 — 도로명만 같다고 무조건 받아들이면, 교차로·인터체인지
			//   밀집구간에서 실제로는 경로와 무관한 다른 구간이 우연히 가까워서 끼어들 수 있다
			//   (실측 000376_20260826152113 M260: 2520178704 — 직전/직후 링크 어느 쪽과도 위상
			//   연결이 없는데 1m 이내로 가까워 채택됨). CreateData::ComputeOppositeLinkPairs() 와
			//   동일한 원리로 "현재 링크 끝점↔후보 시작점"·"현재 링크 시작점↔후보 끝점"이 서로
			//   가까운(뒤집힌 왕복분리 형태) 경우만 인정한다 (2026-08-26 최정우 추가)
			POINT stCurSt, stCurEd, stCandSt, stCandEd;
			stCurSt.dfX = static_cast<double>(pstCurLinkInfo->dwStNodeX);
			stCurSt.dfY = static_cast<double>(pstCurLinkInfo->dwStNodeY);
			stCurEd.dfX = static_cast<double>(pstCurLinkInfo->dwEdNodeX);
			stCurEd.dfY = static_cast<double>(pstCurLinkInfo->dwEdNodeY);
			stCandSt.dfX = static_cast<double>(pstCandInfo->dwStNodeX);
			stCandSt.dfY = static_cast<double>(pstCandInfo->dwStNodeY);
			stCandEd.dfX = static_cast<double>(pstCandInfo->dwEdNodeX);
			stCandEd.dfY = static_cast<double>(pstCandInfo->dwEdNodeY);

			if ((m_cGISUtil.GetDistanceGEO1(stCurSt, stCandEd) > MM_NODE_BRIDGE_MAX_M)
				|| (m_cGISUtil.GetDistanceGEO1(stCurEd, stCandSt) > MM_NODE_BRIDGE_MAX_M))
				continue;

			DEPTH_LINK_INFO_DATA stDepthInfo;
			stDepthInfo.qwLinkID = pstSgmt->qwLinkID;
			stDepthInfo.dwStartTurnOffset = pstCandInfo->dwTurnOffset;
			stDepthInfo.dwEndTurnOffset = stDepthInfo.dwStartTurnOffset + pstCandInfo->nTurnCount;
			stDepthInfo.dwStartSgmtOffset = pstCandInfo->dwSgmtOffset;
			stDepthInfo.dwEndSgmtOffset = stDepthInfo.dwStartSgmtOffset + pstCandInfo->wSgmtCount;
			stDepthInfo.bGeometricBridge = true;

			LinkSgmtMapMatch(stSgmtMatchInput, stDepthInfo, plistMatchEntryList, false);
		}
	}
}

/**
 * @brief 연결 링크 정보
 * @param[in,out] psetSearchHistoryLinkList 검색된 링크 UID 목록 (중복 검사용)
 * @param[out] plistDepthLinkInfoList 연결 링크 UID 정보
 * @param[out] pmapParentLink 새로 발견된 링크가 거쳐온 직전 링크 기록 (경로 역추적용, 2026-08-20 최정우 추가)
 * @return true(성공), false(실패)
 * @remark (2026-08-21 최정우) 브릿지로 먼저 발견된 링크를 TURNINFO 로 재발견 시 "화이트리스트만
 *   승격"하는 시도를 했으나, 중복 DEPTH_LINK_INFO_DATA 항목이 각자 같은 회전정보를 다시 확장하며
 *   depth 마다 배가돼 지수적으로 폭증(실측: 처리 행 하나에 메모리 수 GB·응답 정지)하는 것 확인 —
 *   되돌림. 이 판정은 psetSearchHistoryLinkList(브릿지 포함 전체) 그대로 사용해 중복 확장을 막는다.
*/
bool CContinueMapMatch::GetLinkDepthInfo(set<uint64> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList,
		unordered_map<uint64, uint64> *pmapParentLink, sint16 nSpeed)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	// 이전 depth 링크 UID 정보 목록
	uint32 dwDepthCount = static_cast<uint32>(plistDepthLinkInfoList->size());
	listDepthLinkInfo::iterator it = plistDepthLinkInfoList->begin();
	for (uint32 dwIndex=0; it != plistDepthLinkInfoList->end() && dwIndex < dwDepthCount; ++dwIndex)
	{
		uint32 dwStartTurnOffset = it->dwStartTurnOffset;
		uint32 dwEndTurnOffset = it->dwEndTurnOffset;
		// erase(it++) 이후엔 it 가 다음 원소를 가리켜 더 이상 이 링크 ID 를 안 가리킴 — 지리적
		//   브릿지 탐색에서 "확장 중이던 링크"로 쓰기 위해 지우기 전에 따로 보관 (2026-08-20 최정우 추가)
		uint64 qwErasedLinkID = it->qwLinkID;

		// 이전 depth 링크 ID 삭제
		plistDepthLinkInfoList->erase(it++);

		for (uint32 i=dwStartTurnOffset; i<dwEndTurnOffset; ++i)
		{
			DEPTH_LINK_INFO_DATA stDepthLinkInfoData;

			// 분기(턴) 정보로 후속 링크 ID 조회 (2026-07-08 최정우 주석 추가)
			PTURN_INFO pstTurnInfo = m_pcDataLoader->GetTurnInfo(i);
			if (!pstTurnInfo) continue;

			// 이미 검사한 링크 ID 이면 제외
			set<uint64>::iterator history_it = psetSearchHistoryLinkList->find(pstTurnInfo->qwOutLinkID);
			if (history_it != psetSearchHistoryLinkList->end())
				continue;

			// 출력 링크 메타(세그먼트·턴 오프셋) 조회 (2026-07-08 최정우 주석 추가)
			PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(pstTurnInfo->qwOutLinkID);
			if (!pstLinkInfo) continue;

			// 회전 정보가 없는(막다른) 링크도 반드시 이번 depth 후보 목록에 넣어야 함 — 아래
			//   push_back 안 하고 여기서 continue 해버리면 주차장 진입로·막힌 골목 같은 막다른
			//   링크는 실제로 그 위에 있어도 LinkSgmtMapMatch 스코어링 대상에서 영원히 빠져
			//   맵매칭이 안 됨(2026-08-14 최정우 수정 — "소스상 문제" 검토 중 발견). dwTurnOffset/
			//   nTurnCount 가 0이면 아래에서 dwStartTurnOffset==dwEndTurnOffset 이 되어 다음 depth
			//   확장은 자연히 안 일어나므로(line 414의 for 루프가 빈 범위) 별도 분기 불필요
			//   [정정] "line 414" 는 당시 줄 번호 — 이 함수의 회전정보 순회 루프
			//   (for i=dwStartTurnOffset..dwEndTurnOffset)를 가리킨다 (2026-10-06 최정우 주석 수정)
			stDepthLinkInfoData.qwLinkID = pstTurnInfo->qwOutLinkID;
			stDepthLinkInfoData.dwStartTurnOffset = pstLinkInfo->dwTurnOffset;
			stDepthLinkInfoData.dwEndTurnOffset = stDepthLinkInfoData.dwStartTurnOffset + pstLinkInfo->nTurnCount;
			stDepthLinkInfoData.dwStartSgmtOffset = pstLinkInfo->dwSgmtOffset;
			stDepthLinkInfoData.dwEndSgmtOffset = stDepthLinkInfoData.dwStartSgmtOffset + pstLinkInfo->wSgmtCount;

			// 이 링크(qwOutLinkID)는 qwErasedLinkID(확장 중인 직전 링크, erase 직전에
			//   미리 저장해둔 값)를 거쳐 도달함 — 경로 역추적용 기록. (2026-08-20 최정우 추가)
			// [버그 수정, 2026-09-10 최정우] 원래 이 자리는 it->qwLinkID 를 썼는데 틀렸다 — 위
			//   erase(it++) 가 이미 it 를 "다음" 원소(리스트에 1개뿐이면 end())로 전진시킨 뒤라,
			//   여기서 it->qwLinkID 는 "확장 중인 직전 링크"가 아니라 무관한 값(심한 경우 list::end()
			//   역참조, 정의되지 않은 동작)이다. 아래 BridgeNearbyLinkStarts() 호출부에는
			//   이 문제를 피하려고 이미 qwErasedLinkID 를 쓰고 있었는데 정작 여기는 빠뜨렸던 것 —
			//   최소 재현(list 1개 원소 + erase(it++) 후 역참조)으로 값이 실제로 어긋남을 실측 확인.
			//   부모 링크가 틀리면 ReconstructPath() 가 만드는 경유 경로 자체가 오염되어 게이트/구역
			//   판정(5종 과금 함수)에 쓰이는 경유 링크가 틀릴 수 있어 과금 누락/오과금으로 이어질 수
			//   있었다. it->qwLinkID 를 qwErasedLinkID 로 교체.
			// 원래 "링크당 최초 1회만 발견되므로 덮어쓸 일 없음"이라 operator[] 를 썼으나 이것도 틀렸다 —
			//   위 history 체크(psetSearchHistoryLinkList)는 "이전 depth 까지" 방문만 걸러내고,
			//   같은 depth 안에서 서로 다른 링크가 같은 후속 링크로 수렴(도로 합류)하는 건 못 걸러낸다.
			//   그 경우 나중에 처리되는 쪽이 parent 를 덮어써 mapParentLink 에 사이클이 생길 수 있고,
			//   ReconstructPath() 의 역추적 루프가 그 사이클을 만나면 무한루프에 빠진다 — 전체
			//   재맵매칭 중 특정 트립에서 메모리가 무한 증가하는 것으로 실측 확인됨. insert()로
			//   바꿔 "최초 발견된 parent만" 유지하면 이런 사이클 자체가 생기지 않는다
			//   (2026-09-10 최정우 수정)
			if (pmapParentLink != nullptr)
				pmapParentLink->insert(std::make_pair(pstTurnInfo->qwOutLinkID, qwErasedLinkID));

			plistDepthLinkInfoList->push_back(stDepthLinkInfoData);
		}

		// 지리적 브릿지 시도 — "턴 정보 아예 없을 때만"(1차 시도)으로는 다른 방향 턴은 있고 하필
		//   필요한 방향만 없는 실측 케이스가 전혀 안 잡혀서(094414/140532 교차로 케이스) 턴 유무와
		//   무관하게 시도하도록 넓혔다가, 정차 중(속도 0, 매 tick 같은 링크를 반복 확장)인 지점까지
		//   매번 새 경쟁 후보가 끼어들어 원래 안정적이던 매칭이 흔들리는 회귀가 실측 확인됨(093337
		//   트립 90→86). "속도 0(정차 확실)일 때만 제외"로 절충 — 정차 아닐 때는 계속 시도, 정차
		//   중엔 안 건드림(2026-08-20 최정우 재수정 — "정확히 0"만 제외로는 회귀가 그대로 재현돼,
		//   저속 전체(MM_SPEED_LOW_KMH 이하 — 방위각 자체를 안 믿는 기존 기준과 동일 임계 재사용)로
		//   범위를 넓힘). qwErasedLinkID 는 위에서 erase(it++) 전에 미리 보관해둔 값(끝점 좌표는
		//   GetLinkInfo() 로 다시 조회).
		//   2026-09-04 시도 — 연쇄 지리적 브릿지 차단(bGeometricBridge 링크에서는 재브릿지 금지)을
		//   시도했으나 특정 트립(000370/000382)에서 메모리가 수십 초 만에 GB 단위로 폭증하는 심각한
		//   회귀가 실측 확인돼(원인 미파악) 즉시 롤백. 이 최적화는 보류 (2026-09-04 최정우 롤백)
		if ((nSpeed < 0) || (nSpeed > MM_SPEED_LOW_KMH))
		{
			PLINK_INFO pstFromLinkInfo = m_pcDataLoader->GetLinkInfo(qwErasedLinkID);
			if (pstFromLinkInfo != nullptr)
			{
				double dfEndRawX = static_cast<double>(pstFromLinkInfo->dwEdNodeX) / 360000.0;
				double dfEndRawY = static_cast<double>(pstFromLinkInfo->dwEdNodeY) / 360000.0;
				BridgeNearbyLinkStarts(qwErasedLinkID, dfEndRawX, dfEndRawY,
					psetSearchHistoryLinkList, plistDepthLinkInfoList, pmapParentLink);
			}
		}
	}

	return (!plistDepthLinkInfoList->empty()) ? true : false;
}

/**
 * @brief 막다른 링크 끝점 근처에서 다른 링크의 시작점을 지리적으로 찾아 depth 후보에 추가
 *        [정정] "막다른 링크" 에 한정되지 않는다 — 2026-08-20 이후 GetLinkDepthInfo() 는 회전정보
 *        유무와 무관하게, 저속(MM_SPEED_LOW_KMH 이하)이 아니면 확장하는 **모든** 링크 끝점에서 이
 *        함수를 부른다 (2026-10-06 최정우 주석 수정)
 * @param[in] qwFromLinkID 확장 중이던(막다른) 링크 ID — 자기 자신 제외, 경로 역추적 부모로 기록
 * @param[in] dfEndRawX qwFromLinkID 끝 노드의 경도(WGS84, **도 단위** — 내부 스케일 미변환 원본)
 * @param[in] dfEndRawY qwFromLinkID 끝 노드의 위도(WGS84, **도 단위** — 내부 스케일 미변환 원본)
 *   (2026-09-21 최정우 주석 정정 — 종전에는 한 줄에 두 이름을 적어 doxygen 이 둘 다 인식하지
 *   못했다. 단위는 이 함수가 GetNearGridID 에 그대로 넘기는 값이라 특히 중요하다 —
 *   TryNearbyRoadNameCandidate 의 2026-09-15 버그가 정확히 이 단위 혼동이었다)
 * @param[in,out] psetSearchHistoryLinkList 이미 발견된 링크 목록(중복 방지) — 새로 찾은 링크 추가
 * @param[out] plistDepthLinkInfoList 새로 찾은 링크를 다음 depth 후보로 추가
 * @param[out] pmapParentLink 경로 역추적용 — qwFromLinkID 를 거쳐 도달한 것으로 기록
 * @return void
 * @remark GRID_SGMT_INFO.wLenFromLink==0 인 세그먼트는 그 링크의 "첫 세그먼트" — 좌표(dwX,dwY)가
 *   곧 그 링크의 시작 노드 좌표와 같다. BeginMapMatch 의 그리드 반경 탐색과 동일한 GetGridID/
 *   GetNearGridID 조합을 재사용하되, 전체 매칭 비용 계산 없이 "링크 시작점이 근처에 있는가"만
 *   가볍게 확인 — 그래프가 막힌 경우에만 호출되므로 평소 매칭 경로엔 비용이 안 붙는다.
 *   [정정] 위 "그래프가 막힌 경우에만 호출" 은 2026-08-20 재수정 전 설명이다. 현재는 저속이 아니면
 *   depth 확장 때마다 호출되므로 평상 매칭 경로에도 그리드 순회 비용이 붙고, 찾은 후보는
 *   bGeometricBridge 로 표시돼 MM_GEOM_BRIDGE_PENALTY 를 받는다 (2026-10-06 최정우 주석 수정)
*/
void CContinueMapMatch::BridgeNearbyLinkStarts(uint64 qwFromLinkID, double dfEndRawX, double dfEndRawY,
		set<uint64> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList,
		unordered_map<uint64, uint64> *pmapParentLink)
{
	SGMT_MATCH_INPUT stGridInput;
	stGridInput.stPoint.dfX = dfEndRawX;
	stGridInput.stPoint.dfY = dfEndRawY;
	stGridInput.nRadius = static_cast<sint16>(MM_NODE_BRIDGE_MAX_M);

	uint32 dwGridID = m_cGISUtil.GetGridID(stGridInput.stPoint.dfX, stGridInput.stPoint.dfY);
	if (dwGridID == static_cast<uint32>(INVALID_GRID_ID))
		return;

	vector<uint32> vtNearGridIDList;
	vtNearGridIDList.push_back(dwGridID);
	m_cGISUtil.GetNearGridID(dwGridID, stGridInput, vtNearGridIDList);

	POINT stEndPoint;
	stEndPoint.dfX = dfEndRawX * 360000.0;
	stEndPoint.dfY = dfEndRawY * 360000.0;

	set<uint64> setBridgedThisCall;			// 여러 그리드에 걸친 같은 링크 중복 추가 방지

	for (size_t g = 0; g < vtNearGridIDList.size(); ++g)
	{
		PGRID_INFO pstGridInfo = m_pcDataLoader->GetGridInfo(vtNearGridIDList[g]);
		if (!pstGridInfo)
			continue;

		uint32 dwStart = pstGridInfo->dwSgmtOffset;
		uint32 dwEnd = dwStart + pstGridInfo->wSgmtCount;

		for (uint32 s = dwStart; s < dwEnd; ++s)
		{
			PGRID_SGMT_INFO pstSgmt = m_pcDataLoader->GetGridSgmtInfo(s);
			if ((!pstSgmt) || (pstSgmt->wLenFromLink != 0))
				continue;						// 링크의 첫 세그먼트(=시작 노드)만 대상

			if (pstSgmt->qwLinkID == qwFromLinkID)
				continue;						// 자기 자신 제외

			if (psetSearchHistoryLinkList->find(pstSgmt->qwLinkID) != psetSearchHistoryLinkList->end())
				continue;						// 이미 발견된 링크
			if (setBridgedThisCall.find(pstSgmt->qwLinkID) != setBridgedThisCall.end())
				continue;

			POINT stCandStart;
			stCandStart.dfX = static_cast<double>(pstSgmt->dwX);
			stCandStart.dfY = static_cast<double>(pstSgmt->dwY);
			double dfDistM = m_cGISUtil.GetDistanceGEO1(stEndPoint, stCandStart);
			if (dfDistM > MM_NODE_BRIDGE_MAX_M)
				continue;

			PLINK_INFO pstBridgeLinkInfo = m_pcDataLoader->GetLinkInfo(pstSgmt->qwLinkID);
			if (!pstBridgeLinkInfo)
				continue;

			DEPTH_LINK_INFO_DATA stDepthLinkInfoData;
			stDepthLinkInfoData.qwLinkID = pstSgmt->qwLinkID;
			stDepthLinkInfoData.dwStartTurnOffset = pstBridgeLinkInfo->dwTurnOffset;
			stDepthLinkInfoData.dwEndTurnOffset = stDepthLinkInfoData.dwStartTurnOffset + pstBridgeLinkInfo->nTurnCount;
			stDepthLinkInfoData.dwStartSgmtOffset = pstBridgeLinkInfo->dwSgmtOffset;
			stDepthLinkInfoData.dwEndSgmtOffset = stDepthLinkInfoData.dwStartSgmtOffset + pstBridgeLinkInfo->wSgmtCount;
			// TURNINFO 아닌 좌표 근접으로 찾은 후보 — 위상 우선순위 페널티 대상 표시 (2026-08-21 최정우 추가)
			stDepthLinkInfoData.bGeometricBridge = true;

			// operator[] 덮어쓰기가 mapParentLink 사이클을 만들 수 있어 insert()로 변경 —
			//   GetLinkDepthInfo() 쪽 동일 수정과 같은 이유 (2026-09-10 최정우 수정)
			if (pmapParentLink != nullptr)
				pmapParentLink->insert(std::make_pair(pstSgmt->qwLinkID, qwFromLinkID));

			plistDepthLinkInfoList->push_back(stDepthLinkInfoData);
			setBridgedThisCall.insert(pstSgmt->qwLinkID);
		}
	}
}

/**
 * @brief 경로 역추적 — mapParentLink 를 최종 확정 링크에서부터 거슬러 올라가며 경유 링크 목록을 만든다
 * @param[in] qwFinalLinkID 이번에 최종 확정된 링크 ID
 * @param[in] mapParentLink GetLinkDepthInfo() 가 채운 "링크→그 직전 링크" 맵
 * @param[out] pvtOut 확장 없이 확정된 경우(최종=시작) 그 링크 1개만, 확장이 있었으면 시작 링크는
 *   제외하고 경유 링크+최종 링크를 순서대로(시작→끝) 채운다 — 어느 경우든 최소 1개는 담긴다
 * @return void
 * @remark 최대 depth 가 config maxstep(작은 값, 보통 2~3)로 제한돼 있어 경로 길이도 그만큼 짧다 (2026-08-20 최정우 추가)
 *   [정정] maxstep 은 2026-08-22 에 2→4 로 올랐고(현행 config.ini maxstep=4), 이동거리에 따라
 *   MM_STEP_EXTEND_MAX(3)까지 더해져 최대 depth 7 까지 갈 수 있다. 경로가 MATCH_LINK_INFO_MAX_PATH(8)
 *   를 넘으면 CMapMatch::ContinueMapMatch() 가 잘라 담는다 (2026-10-06 최정우 주석 수정)
*/
void CContinueMapMatch::ReconstructPath(uint64 qwFinalLinkID, const unordered_map<uint64, uint64>& mapParentLink,
		vector<uint64> *pvtOut)
{
	if (pvtOut == nullptr)
		return;
	pvtOut->clear();

	vector<uint64> vtReversed;
	set<uint64> setWalked;					// 순환 참조 방어 — mapParentLink 에 사이클이 생기면
											//   이 가드가 없으면 아래 while 이 영원히 돈다
											//   (2026-09-10 최정우 추가, 전체 재맵매칭 중 메모리 무한증가로 발견)
	uint64 qwCur = qwFinalLinkID;
	// mapParentLink 에 없는 링크를 만나면 그게 시작 링크(확장 시작점) — 역추적 종료.
	//   링크 수만큼(=depth 만큼, 매우 작음) 도는 루프라 무한루프 없는 게 정상이지만,
	//   GetLinkDepthInfo 가 같은 depth 내 서로 다른 링크에서 같은 후속 링크로 수렴하면
	//   parent 가 덮어써져 사이클이 생길 수 있어(원인은 그쪽에서 별도로 막았음) 방어적으로
	//   재방문을 감지해 즉시 끊는다.
	while (true)
	{
		if (!setWalked.insert(qwCur).second)
		{
			LOGFMTE("ReconstructPath cycle detected! link=[%llu] depth=[%zu]",
				static_cast<unsigned long long>(qwCur), vtReversed.size());
			pvtOut->clear();
			return;
		}
		vtReversed.push_back(qwCur);
		unordered_map<uint64, uint64>::const_iterator it = mapParentLink.find(qwCur);
		if (it == mapParentLink.end())
			break;
		qwCur = it->second;
	}

	if (vtReversed.size() == 1)
	{
		// 확정 링크가 곧 시작 링크(확장 없이 그대로 확정) — 기존 단일 링크 체크와 동일하게
		//   그 자체 1개만 담는다
		pvtOut->push_back(vtReversed[0]);
	}
	else
	{
		// vtReversed 는 [최종, ..., 시작] 순 — 시작 링크(마지막 원소)만 제외하고 뒤집어서 채움
		for (sint32 i = static_cast<sint32>(vtReversed.size()) - 2; i >= 0; --i)
			pvtOut->push_back(vtReversed[static_cast<size_t>(i)]);
	}
}

/**
 * @brief 검색 정보 목록 중 비용 최소 후보 추출 (dfCost 오름차순, 동률 시 INTERSECT_LEN)
 * @param[in,out] plistMatchEntryList 검색 정보 목록. **이 함수가 그 자리에서 정렬한다**
 * @param[out] pstMatchEntry 선택된 최소 비용 후보
 * @param[in] pstTraceCtx 매칭 트레이스 컨텍스트 (nullptr=미기록)
 * @param[in] stSgmtMatchInput 트레이스 기록용 입력 스냅샷
 * @return void
 * @remark 2026-09-21 최정우 주석 보완 — 뒤 두 파라미터에 설명이 없었고, 첫 인자가 in 이 아니라
 *   정렬로 변경되는 in,out 이라는 점도 드러나지 않았다.
*/
void CContinueMapMatch::GetMatchEntry(list<MATCH_ENTRY> *plistMatchEntryList, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx, const SGMT_MATCH_INPUT& stSgmtMatchInput)
{
	plistMatchEntryList->sort();
	if (pstTraceCtx != nullptr)
		CMatchTrace::LogResult(pstTraceCtx, stSgmtMatchInput, *plistMatchEntryList, plistMatchEntryList->front());
	*pstMatchEntry = *plistMatchEntryList->begin();

	// [2026-09-11 최정우 — 시도했다가 되돌림] 정지 중 좌표표류(저주파 GPS 위치표류가 매 tick 재투영에
	//   그대로 반영돼 차가 안 움직이는데 매칭좌표만 흘러가는 현상, 실측 최대 20m)를 막으려고 여기서
	//   dfMatchX/Y·dfSgmtMatchLen 을 직전 신뢰 매칭좌표로 앵커링하는 걸 시도했으나, dfIntersectLenSgmt/
	//   bSgmtClamped/dfCost/nDirAngleDiff 등 원래 후보(GPS 원본 기준으로 계산됨)와 짝을 이루는 나머지
	//   필드는 그대로 둔 채 좌표만 바꿔서 내부 일관성이 깨졌다 — 재매칭 검증에서 MATCH_STATUS 가
	//   전체적으로 78건 추가 SKIP 으로 바뀌는 회귀가 실측 확인돼 되돌렸다(드리프트 자체는 대부분
	//   사라졌지만 대가가 너무 컸음). 다시 시도할 때는 이 함수(최종 후보 확정 지점)가 아니라, 이후
	//   단계에서 "표시/누적용 좌표만" 별도로 덮어써서 여기서 확정되는 내부 매칭 상태(SKIP/클램프 판정
	//   등에 쓰이는 필드들)는 건드리지 않는 방식으로 가야 한다.
}

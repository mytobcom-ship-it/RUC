/**
 * @file MatchTrace.cpp
 * @brief 맵매칭 후보·비용 디버그 로그
*/
#include "MatchTrace.h"
#include "log4z.h"
#include <stdio.h>
#include <string.h>

using namespace zsummer::log4z;

namespace {

/**
 * @brief 고도(m) 값을 로그용 문자열로 변환
 * @param[out] pszBuf 출력 버퍼
 * @param[in] nBufLen 버퍼 크기
 * @param[in] nAlt 고도(m). **음수이면**(NO_ALTITUDE=-1 포함) "-" 로 표기
 *            (2026-09-21 최정우 주석 정정 — 종전 "NO_ALTITUDE 미만" 은 실제 조건
 *            (nAlt >= 0)과 달라, -1 은 숫자로 찍힐 것처럼 읽혔다)
 * @return void
*/
void AltValue(char *pszBuf, size_t nBufLen, sint16 nAlt)
{
	if (nAlt >= 0)
		snprintf(pszBuf, nBufLen, "%d", static_cast<int>(nAlt));
	else
		snprintf(pszBuf, nBufLen, "-");
}

/**
 * @brief 링크 선택 비용 공식 문자열 생성 (match trace formula)
 * @param[out] pszBuf 출력 버퍼
 * @param[in] nBufLen 버퍼 크기
 * @param[in] stEntry 후보·당선 MATCH_ENTRY
 * @param[in] bHasAltAdj Continue 고도 보조 점수 포함 여부
 * @return void
 * @remark
 *   · dist = INTERSECT_LEN (GPS↔세그먼트 교차점 거리, m)
 *   · bHasAltAdj=true  → dist+angle_cost+alt_adj=cost
 *   · bHasAltAdj=false → dist+angle_cost=cost
 *   [보완] 좌변 합이 cost 와 같지 않을 수 있다 — dfCost 에는 공식에 안 찍히는 가산분이 더 붙는다:
 *   Continue 의 depth(hop) 벌점(config hoppenalty × depth)·좌표근접 브릿지 후보 MM_GEOM_BRIDGE_PENALTY,
 *   Begin 연속실패 재검색의 미연결 후보 MM_CONNECT_PENALTY. 차이가 나면 이들 가산분이다
 *   (2026-10-06 최정우 주석 수정)
*/
void FormatCostFormula(char *pszBuf, size_t nBufLen, const MATCH_ENTRY& stEntry, bool bHasAltAdj)
{
	if (bHasAltAdj)
	{
		snprintf(pszBuf, nBufLen, "%.1f+%.1f%+.1f=%.1f",
			stEntry.dfIntersectLenSgmt, stEntry.dfAngleCost, stEntry.dfAltAdj, stEntry.dfCost);
	}
	else
	{
		snprintf(pszBuf, nBufLen, "%.1f+%.1f=%.1f",
			stEntry.dfIntersectLenSgmt, stEntry.dfAngleCost, stEntry.dfCost);
	}
}

} // namespace

/**
 * @brief 맵매칭 추적·input·후보·당선 winner 디버그 로그 출력
 * @param[in] pstCtx GPS 1건 추적 컨텍스트 (ProcessManager → MapMatch)
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 (고도 점수·반경 등)
 * @param[in] listCandidates 정렬된 후보 MATCH_ENTRY 목록
 * @param[in] stWinner 최종 선택 링크
 * @return void
 * @remark
 *   - LOGFMTD 출력 — config [log] level=1(DEBUG) 이하에서만 보임
 *   - 후보는 상위 MATCH_TRACE_TOP_N건만 출력
 *   - pstCtx null 이면 무시
*/
void CMatchTrace::LogResult(const MATCH_TRACE_CTX *pstCtx, const SGMT_MATCH_INPUT& stSgmtMatchInput,
		const std::list<MATCH_ENTRY>& listCandidates, const MATCH_ENTRY& stWinner)
{
	if (pstCtx == nullptr)
		return;

	const bool bHasAltAdj = pstCtx->bContinue && stSgmtMatchInput.bUseAltScore;
	char szAlt[8];
	char szPrevAlt[8];
	AltValue(szAlt, sizeof(szAlt), pstCtx->nAltitudeM);
	AltValue(szPrevAlt, sizeof(szPrevAlt), pstCtx->nPrevAltitude);

	LOGFMTD("[#%02d] match trace device=[%s] trip_id=[%s] seq=[%u] mode=[%s] prev_link=[%llu] step=[%d]",
		pstCtx->nThreadId, pstCtx->szDeviceKey, pstCtx->szTripId, pstCtx->dwSeqNo,
		pstCtx->bContinue ? "Continue" : "Begin",
		static_cast<unsigned long long>(pstCtx->qwPrevLinkId),
		static_cast<int>(pstCtx->nMatchedStep));

	LOGFMTD("[#%02d] match input  seq=[%u] lat=[%.06f] lon=[%.06f] radius=[%d] speed=[%d] heading=[%d] "
		"alt=[%s] prev_alt=[%s] prev_road=[%u]",
		pstCtx->nThreadId, pstCtx->dwSeqNo, pstCtx->dfGpsLat, pstCtx->dfGpsLon,
		static_cast<int>(pstCtx->nRadius),
		(pstCtx->nSpeed >= 0) ? static_cast<int>(pstCtx->nSpeed) : -1,
		(pstCtx->nHeading != NO_ANGLE) ? static_cast<int>(pstCtx->nHeading) : -1,
		szAlt, szPrevAlt, static_cast<unsigned>(pstCtx->nPrevRoadType));

	LOGFMTD("[#%02d] match cand   seq=[%u] total=[%zu]",
		pstCtx->nThreadId, pstCtx->dwSeqNo, listCandidates.size());

	int nRank = 0;
	for (std::list<MATCH_ENTRY>::const_iterator it = listCandidates.begin();
			it != listCandidates.end() && nRank < MATCH_TRACE_TOP_N; ++it, ++nRank)
	{
		char szFormula[64];
		FormatCostFormula(szFormula, sizeof(szFormula), *it, bHasAltAdj);

		const bool bSelect = (it->qwLinkID == stWinner.qwLinkID
			&& it->dfCost == stWinner.dfCost
			&& it->dfIntersectLenSgmt == stWinner.dfIntersectLenSgmt);

		LOGFMTD("[#%02d] match cand   seq=[%u] rank=[%d] %s link=[%llu] cost=[%.1f] intersect_len=[%.1f] "
			"angle_cost=[%.1f] alt_adj=[%+.1f] road=[%03u] rank_road=[%03u] suspect=[%d] formula=[%s]",
			pstCtx->nThreadId, pstCtx->dwSeqNo, nRank + 1, bSelect ? "SELECT" : "      ",
			static_cast<unsigned long long>(it->qwLinkID), it->dfCost,
			it->dfIntersectLenSgmt, it->dfAngleCost, it->dfAltAdj,
			static_cast<unsigned>(it->nRoadType), static_cast<unsigned>(it->nRoadRank),
			it->bReverseSuspect ? 1 : 0,
			szFormula);
	}

	// dfMatchX=경도(X), dfMatchY=위도(Y) — lat/lon 표기 정정 (2026-07-10 최정우 수정)
	const double dfMatchLat = stWinner.dfMatchY / 360000.0;
	const double dfMatchLon = stWinner.dfMatchX / 360000.0;
	const int nIntersectM = (stWinner.dfIntersectLenSgmt >= 0.0)
		? static_cast<int>(stWinner.dfIntersectLenSgmt + 0.5) : -1;

	LOGFMTD("[#%02d] match winner seq=[%u] link=[%llu] cost=[%.1f] match_lat=[%.06f] match_lon=[%.06f] "
		"intersect_len=[%d]",
		pstCtx->nThreadId, pstCtx->dwSeqNo,
		static_cast<unsigned long long>(stWinner.qwLinkID), stWinner.dfCost,
		dfMatchLat, dfMatchLon, nIntersectM);
}

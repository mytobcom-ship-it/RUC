/**
 * @file ChargeDataLoader.cpp
 * @brief 과금 게이트 정보 로딩 클래스 소스 파일
*/
#include "ChargeDataLoader.h"
#include "CoordConvert.h"
#include "GISUtil.h"

// 재조회로 교체된 이전 세대 캐시를 몇 개까지 계속 살려둘지 — 댕글링 포인터 방지용(LoadGates/
//   LoadZones 참고). 조회 함수가 반환한 포인터는 락 해제 직후 짧게(같은 함수 내 몇 줄)만 쓰이므로
//   2세대(현재+직전 재조회분)면 충분한 여유 (2026-08-14 최정우 추가)
static const size_t RETIRED_CACHE_GENERATIONS = 2;

// DistanceToPolygonBoundaryMeters() 의 "거리 없음" 표시값 — 어떤 실제 거리보다도 크게 잡아
//   호출측의 `<= 임계` 비교에서 항상 탈락시킨다 (2026-09-21 최정우 추가, 종전 1e18 리터럴)
static const double POLY_DIST_INFINITE = 1e18;

/**
 * @brief COORDS(jsonb) 파싱 — "[[lon,lat],[lon,lat],...]" 형태에서 정점 목록 추출 (2026-08-13 최정우 추가)
 * @param[in] strJson base_roadlink.coords 원본 텍스트(GEOM_TYPE='POLY')
 * @param[out] pvtOut 파싱된 정점 목록(dfX=경도, dfY=위도)
 * @remark 이 코드베이스에 JSON 파서가 없어 정식 파서 대신 이 컬럼의 고정 포맷([[lon,lat],...], 중첩
 *   1단계)만 가정하고 직접 스캔. '[' 뒤에서 숫자 두 개(strtod) 파싱을 시도해 실패하면(예: 바깥쪽
 *   '[[' 의 첫 '[') 그냥 건너뛰므로 중첩 구조에 안전함
*/
static void ParseCoordsJson(const string& strJson, vector<POINT> *pvtOut)
{
	pvtOut->clear();
	const char *p = strJson.c_str();
	while (*p != '\0')
	{
		if (*p == '[')
		{
			char *pAfterLon = nullptr;
			double dfLon = strtod(p + 1, &pAfterLon);
			if (pAfterLon != (p + 1))
			{
				const char *pLat = pAfterLon;
				while ((*pLat == ',') || (*pLat == ' ')) pLat++;
				char *pAfterLat = nullptr;
				double dfLat = strtod(pLat, &pAfterLat);
				if (pAfterLat != pLat)
				{
					POINT stPt;
					stPt.dfX = dfLon;
					stPt.dfY = dfLat;
					pvtOut->push_back(stPt);
					p = pAfterLat;
					continue;
				}
			}
		}
		p++;
	}
}

/**
 * @brief LINK_IDS(jsonb) 파싱 — ["2040425701", "2040424401"] 형태에서 link_id 목록 추출 (2026-08-13 최정우 추가)
 * @param[in] strJson base_roadlink.link_ids 원본 텍스트(문자열 배열)
 * @param[out] pvtOut 파싱된 link_id 목록(uint64)
 * @remark ParseCoordsJson 과 동일한 이유로 정식 JSON 파서 대신 직접 스캔 — 큰따옴표 안의 숫자만
 *   strtoull 로 추출. 큰따옴표가 아닌 곳(대괄호·쉼표·공백)은 건너뜀
*/
static void ParseLinkIdsJson(const string& strJson, vector<uint64> *pvtOut)
{
	pvtOut->clear();
	const char *p = strJson.c_str();
	while (*p != '\0')
	{
		if (*p == '"')
		{
			char *pAfter = nullptr;
			uint64 qwLinkID = strtoull(p + 1, &pAfter, 10);
			if (pAfter != (p + 1))
			{
				pvtOut->push_back(qwLinkID);
				p = pAfter;
				continue;
			}
		}
		p++;
	}
}

/**
 * @brief 점 P 가 폴리곤 내부인지 — 표준 ray-casting(even-odd) 판정 (2026-08-13 최정우 추가)
 * @param[in] dfLon 점 경도, dfLat 점 위도
 * @param[in] vtPoly 폴리곤 정점 목록. **닫혀 있든(첫 정점 == 끝 정점) 열려 있든 모두 동작한다** —
 *            아래 루프가 j=마지막 정점에서 시작해 마지막↔첫 변을 항상 포함하기 때문이다
 *            (2026-09-21 최정우 주석 정정 — 종전에는 "닫힌 도형 가정" 이라고만 적혀 있어,
 *            열린 폴리라인을 넘기면 틀린 답이 나오는 것처럼 읽혔다. 실제로는 닫힌 입력일 때
 *            마지막 변이 길이 0 이 되어 판정에 영향을 주지 않을 뿐이다)
 * @return true=내부(경계 포함 판정은 버퍼 거리 검사가 별도로 처리)
*/
bool CChargeDataLoader::IsPointInPolygon(double dfLon, double dfLat, const vector<POINT>& vtPoly)
{
	if (vtPoly.size() < 3) return false;

	bool bInside = false;
	size_t nCount = vtPoly.size();
	for (size_t i = 0, j = nCount - 1; i < nCount; j = i++)
	{
		double dfXi = vtPoly[i].dfX, dfYi = vtPoly[i].dfY;
		double dfXj = vtPoly[j].dfX, dfYj = vtPoly[j].dfY;
		if (((dfYi > dfLat) != (dfYj > dfLat)) &&
			(dfLon < (dfXj - dfXi) * (dfLat - dfYi) / (dfYj - dfYi) + dfXi))
		{
			bInside = !bInside;
		}
	}
	return bInside;
}

/**
 * @brief 점 P와 폴리곤 경계(각 변) 사이 최단거리(m) — 적응형 버퍼 판정용 (2026-08-13 최정우 추가)
 * @remark 버퍼 거리가 수 m~십수 m 수준으로 짧아 P 주변 국소 평면(경위도→m 환산)으로 근사해도
 *   오차가 무시할만함 — 하버사인을 변마다 반복 호출할 필요 없음
*/
double CChargeDataLoader::DistanceToPolygonBoundaryMeters(double dfLon, double dfLat, const vector<POINT>& vtPoly)
{
	// [2026-09-21 최정우 정리] 반환 매직넘버 1e18 을 상수로 빼고, 최소 정점 수 기준을 짝 함수인
	//   IsPointInPolygon(3개 미만이면 폴리곤 아님)과 맞춘다. 종전에는 여기만 2 여서 "정점 2개짜리
	//   선분" 에 대해 경계거리는 계산해주고 내부 판정은 무조건 false 인 비대칭이 있었다 —
	//   현재 호출부(GetParkingZoneContaining 계열)가 둘 다 size()<3 으로 먼저 걸러 실害는 없다.
	if (vtPoly.size() < 3) return POLY_DIST_INFINITE;

	const double dfMPerLon = 111320.0 * cos(RAD(dfLat));
	const double dfMPerLat = 111320.0;

	double dfMinDist = POLY_DIST_INFINITE;
	size_t nCount = vtPoly.size();
	for (size_t i = 0, j = nCount - 1; i < nCount; j = i++)
	{
		double dfAx = (vtPoly[j].dfX - dfLon) * dfMPerLon;
		double dfAy = (vtPoly[j].dfY - dfLat) * dfMPerLat;
		double dfBx = (vtPoly[i].dfX - dfLon) * dfMPerLon;
		double dfBy = (vtPoly[i].dfY - dfLat) * dfMPerLat;

		double dfDx = dfBx - dfAx, dfDy = dfBy - dfAy;
		double dfLen2 = dfDx * dfDx + dfDy * dfDy;
		double dfT = (dfLen2 > 0.0) ? (((-dfAx) * dfDx + (-dfAy) * dfDy) / dfLen2) : 0.0;
		if (dfT < 0.0) dfT = 0.0;
		if (dfT > 1.0) dfT = 1.0;

		double dfCx = dfAx + dfT * dfDx, dfCy = dfAy + dfT * dfDy;
		double dfDist = sqrt(dfCx * dfCx + dfCy * dfCy);
		if (dfDist < dfMinDist) dfMinDist = dfDist;
	}
	return dfMinDist;
}

/**
 * @brief 생성자
*/
CChargeDataLoader::CChargeDataLoader() :
	m_pcPostgrePool(nullptr),
	m_bLoad(false),
	m_nParkFineMinSec(0)
{
}

/**
 * @brief 소멸자
*/
CChargeDataLoader::~CChargeDataLoader()
{
	Uninitialize();
}

/**
 * @brief 초기화 — 커넥션 풀·게이트/구역 조회 SQL 보관(2026-08-12 최정우 추가)
 * @param[in] pcPostgrePool DB 커넥션 풀(비소유 — Server 가 생성한 것을 그대로 공유)
 * @param[in] strGateSelectSQL base_tollgate 전량 조회 SQL(query.sql 세션명으로 로드된 실 SQL 문자열)
 * @param[in] strZoneSelectSQL base_roadlink 전량 조회 SQL(비어 있으면 구역 캐시만 비활성 — 게이트
 *            캐시는 그대로 사용 가능) (2026-08-12 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CChargeDataLoader::Initialize(CPostgrePool *pcPostgrePool, const string& strGateSelectSQL,
	const string& strZoneSelectSQL, const string& strParkFineSelectSQL)
{
	if (pcPostgrePool == nullptr)
	{
		LOGFMTE("postgre pool is null!");
		return false;
	}
	if (strGateSelectSQL.empty())
	{
		LOGFMTE("gate select sql is empty!");
		return false;
	}

	m_pcPostgrePool = pcPostgrePool;
	m_strGateSelectSQL = strGateSelectSQL;
	m_strZoneSelectSQL = strZoneSelectSQL;
	m_strParkFineSelectSQL = strParkFineSelectSQL;
	return true;
}

/**
 * @brief 종료 — 캐시 해제
 * @return void
*/
void CChargeDataLoader::Uninitialize()
{
	// [2026-09-21 최정우 보완] 종전에는 주 캐시 2개만 비우고 **역인덱스(현재 5개)·전체등록링크 집합·
	//   retired 세대 보관함은 그대로 남겼다**. 소멸자 경로에서는 멤버가 어차피 파괴돼 누수가 아니지만,
	//   "Uninitialize() 후 다시 Initialize()/Load*()" 로 재사용하는 순간 **이미 지운 구역을 가리키는
	//   낡은 역인덱스가 살아남아** IsLinkChargeRegistered()/GetNodeStepZonesByLinkId() 가 유령
	//   링크를 등록된 것으로 답하게 된다. 지금은 그런 호출 경로가 없지만, 비우는 대상이 반쪽인 것은
	//   그 자체로 함정이라 전부 정리한다.
	// 캐시 교체(Load*)와 같은 락을 잡는다 — 종료 순서상 워커가 먼저 멈춰 경합은 없으나,
	//   같은 자료를 만지는 코드가 한쪽만 무락인 상태를 남기지 않는다.
	{
		lock_guard<CMutex> cLock(m_cGateCacheMutex);
		m_mapGateInfo.clear();
		m_dqRetiredGateInfo.clear();
	}
	{
		lock_guard<CMutex> cLock(m_cZoneCacheMutex);
		m_mapZoneInfo.clear();
		m_dqRetiredZoneInfo.clear();
		m_mapNodeStepLinkToRoadId.clear();
		m_mapExemptLinkToRoadId.clear();
		m_mapOpenLinkToRoadId.clear();
		m_mapSpeedLinkToRoadId.clear();
		m_mapClosedZoneLinkToRoadId.clear();				// (2026-09-23 최정우 추가)
		m_setAllRegisteredLinkIds.clear();
	}
	{
		lock_guard<CMutex> cLock(m_cParkFineMutex);
		m_nParkFineMinSec = 0;
	}
	m_bLoad = false;
}

/**
 * @brief base_tollgate 전량을 조회해 인메모리 캐시로 (재)로드 — 기동 시 1회, 또는 주기
 *        재조회 스레드에서 반복 호출 (2026-08-12 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CChargeDataLoader::LoadGates()
{
	if (m_pcPostgrePool == nullptr)
	{
		LOGFMTE("postgre pool not initialized!");
		return false;
	}

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("get connection failed!");
		return false;
	}

	PGresult *pcResult = PQexec(pcConn, m_strGateSelectSQL.c_str());
	if (PQresultStatus(pcResult) != PGRES_TUPLES_OK)
	{
		LOGFMTE("gate select failed!error=[%s]", PQerrorMessage(pcConn));
		PQclear(pcResult);
		m_pcPostgrePool->releaseConnection(pcConn);
		return false;
	}

	// 컬럼 순서: tollgate_id, road_id, gate_div, lon, lat, link_id
	//   (query.sql [gate_select] 세션과 반드시 일치해야 함) (2026-08-12 최정우 추가)
	CCoordConvert cCoordConvert;
	mapGateInfo mapNewGateInfo;
	// tollgate_id(PK) 중복 방어 — 정상 상황에선 DB PK 제약이 이미 막아주지만, 스키마 변경(제약 삭제/
	//   우회)으로 중복이 들어오는 만일의 경우를 대비. link_id 기준 캐시라 중복이 있어도 그 자체로는
	//   죽지 않지만, tollgate_id 문자열을 세션 추적 키로 쓰는 상위 로직(vtActiveGateIds 등)이 서로
	//   다른 물리적 게이트를 같은 게이트로 오판할 수 있어 방어 필요 (2026-08-14 최정우 추가)
	unordered_set<string> setSeenTollgateIds;
	int nRows = PQntuples(pcResult);
	for (int i = 0; i < nRows; i++)
	{
		const char *pszTollgateID = PQgetvalue(pcResult, i, 0);
		const char *pszRoadID = PQgetvalue(pcResult, i, 1);
		const char *pszGateDiv = PQgetvalue(pcResult, i, 2);
		bool bLinkIdNull = PQgetisnull(pcResult, i, 5);
		const char *pszLinkID = bLinkIdNull ? nullptr : PQgetvalue(pcResult, i, 5);

		// [버그 수정, 2026-09-11 최정우] LON/LAT(BASE_TOLLGATE 스키마상 NULL 허용 컬럼, NOT NULL
		//   제약 없음)은 여태 NULL 체크 없이 바로 atof() 했다 — NULL 이면 PQgetvalue 가 빈 문자열을
		//   반환해 atof("")=0.0 이 되어 게이트가 조용히 (0,0)으로 캐시되고 에러 로그도 없이 넘어갔다.
		//   link_id 처럼 명시적으로 걸러서 이 행 자체를 스킵한다.
		bool bLonNull = PQgetisnull(pcResult, i, 3);
		bool bLatNull = PQgetisnull(pcResult, i, 4);
		if (bLonNull || bLatNull)
		{
			LOGFMTE("gate lon/lat is null!skip row - gate id=[%s] road_id=[%s]", pszTollgateID, pszRoadID);
			continue;
		}
		const char *pszLon = PQgetvalue(pcResult, i, 3);
		const char *pszLat = PQgetvalue(pcResult, i, 4);

		// link_id 없는 게이트는 아직 폴백(좌표거리) 전용 조회 경로가 없어 캐시에서 제외
		//   — GetGateNearby 는 현재 "이미 로드된" 게이트 목록을 선형 탐색하는 구조라 무관하지만,
		//   link_id 기반 캐시 키(unordered_map) 특성상 link_id 없는 행은 별도 vector 보관이 필요함.
		//   지금은 스캐폴드 단계라 link_id 있는 행만 우선 적재 (2026-08-12 최정우 추가, TODO)
		if (bLinkIdNull) continue;

		// tollgate_id 중복(2번째부터) — 첫 행만 유지하고 이후는 스킵, 어떤 ID인지 에러로그로 남김
		//   (2026-08-14 최정우 추가 — 정상 시엔 PK 위반으로 절대 안 나오는 케이스)
		if (!setSeenTollgateIds.insert(pszTollgateID).second)
		{
			LOGFMTE("duplicate tollgate_id!skip 2nd+ row - gate id=[%s] road_id=[%s] link_id=[%s]",
				pszTollgateID, pszRoadID, bLinkIdNull ? "" : pszLinkID);
			continue;
		}

		GATE_INFO stGateInfo;
		strncpy(stGateInfo.szTollgateID, pszTollgateID, sizeof(stGateInfo.szTollgateID) - 1);
		strncpy(stGateInfo.szRoadID, pszRoadID, sizeof(stGateInfo.szRoadID) - 1);
		strncpy(stGateInfo.szGateDiv, pszGateDiv, sizeof(stGateInfo.szGateDiv) - 1);
		stGateInfo.dfLon = atof(pszLon);
		stGateInfo.dfLat = atof(pszLat);
		cCoordConvert.WGS84ToSearchCoord(stGateInfo.dfLat, stGateInfo.dfLon,
			&stGateInfo.dwSearchLat, &stGateInfo.dwSearchLon);
		stGateInfo.qwLinkID = strtoull(pszLinkID, nullptr, 10);

		// push_back — 폐쇄형/구간단속 입/출구가 같은 link_id 를 공유하는 실사례가 있어(TG00007/08,
		// TG00012/13) 대입(map[key]=value)하면 뒤 행이 앞 행을 덮어써 유실됨 (2026-08-12 최정우 수정)
		mapNewGateInfo[stGateInfo.qwLinkID].push_back(stGateInfo);
	}

	PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);

	{
		// 재조회 시점 교체 — m_cGateCacheMutex 로 조회 스레드(GetGateByLinkId 등)와 동기화(doc/README.txt
		// §F 패턴, 2026-08-14 최정우 수정 — 락 없이 swap만 하던 걸 수정, "소스상 문제" 검토 중 발견).
		// 단순 swap 만으로는 부족함 — 이전 세대를 즉시 파괴하지 않고 retired 목록에 보관해 댕글링
		// 포인터를 막음(ChargeDataLoader.h m_dqRetiredGateInfo 주석 참고, 2026-08-14 최정우 추가)
		lock_guard<CMutex> cLock(m_cGateCacheMutex);
		m_dqRetiredGateInfo.push_back(std::move(m_mapGateInfo));
		m_mapGateInfo = std::move(mapNewGateInfo);
		while (m_dqRetiredGateInfo.size() > RETIRED_CACHE_GENERATIONS)
			m_dqRetiredGateInfo.pop_front();
	}
	m_bLoad = true;

	LOGFMTI("gate cache loaded!count=[%zu]", GetGateCount());
	return true;
}

/**
 * @brief link_id로 게이트 조회 — O(1), 게이트 판정 1순위 (2026-08-12 최정우 추가)
 * @remark
 *   같은 link_id를 공유하는 게이트가 있을 수 있어(TG00007/08, TG00012/13 확인됨) cGateDiv 로
 *   걸러야 정확함. cGateDiv=0(생략)이면 해당 link_id의 첫 게이트를 그대로 반환 — 개방형(M, 단독
 *   게이트)처럼 공유 걱정이 없는 호출부 전용, 폐쇄형·구간단속은 반드시 gate_div 지정할 것 (2026-08-12 최정우 수정)
 * @param[in] qwLinkID 매칭된 링크 ID
 * @param[in] cGateDiv 게이트 구분 필터(0=생략, 첫 건 반환)
 * @return 게이트 정보 포인터(없으면 nullptr)
*/
PGATE_INFO CChargeDataLoader::GetGateByLinkId(const uint64 qwLinkID, const char cGateDiv)
{
	lock_guard<CMutex> cLock(m_cGateCacheMutex);
	mapGateInfo::iterator it = m_mapGateInfo.find(qwLinkID);
	if (it == m_mapGateInfo.end())
		return nullptr;

	if (cGateDiv == 0)
		return &(it->second.front());

	for (size_t i = 0; i < it->second.size(); ++i)
	{
		if (cGateDiv == it->second[i].szGateDiv[0])
			return &(it->second[i]);
	}
	return nullptr;
}

/**
 * @brief link_id+gate_div 로 일치하는 게이트 전부 조회 (2026-08-13 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @param[in] cGateDiv 게이트 구분('M'/'I'/'O'/'B')
 * @param[out] pvtOut 일치하는 게이트 포인터 목록(비우고 채움) — 없으면 빈 상태로 반환
 * @remark GetGateByLinkId 는 첫 매치 1개만 반환해 같은 link_id 에 같은 gate_div 게이트가
 *   2개 이상이면 나머지가 조회 자체가 안 됨(개방형 부과 누락 위험) — 이 함수는 전부 수집
*/
void CChargeDataLoader::GetGatesByLinkId(const uint64 qwLinkID, const char cGateDiv, vector<PGATE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	pvtOut->clear();

	lock_guard<CMutex> cLock(m_cGateCacheMutex);
	mapGateInfo::iterator it = m_mapGateInfo.find(qwLinkID);
	if (it == m_mapGateInfo.end())
		return;

	for (size_t i = 0; i < it->second.size(); ++i)
	{
		if (cGateDiv == it->second[i].szGateDiv[0])
			pvtOut->push_back(&(it->second[i]));
	}
}

/**
 * @brief road_id+gate_div로 게이트 조회 — 폐쇄형/구간단속 진입~진출 짝짓기 전용 (2026-08-12 최정우 추가)
 * @remark
 *   별도 인덱스 없이 m_mapGateInfo(link_id 키)를 선형 탐색 — 게이트 총량이 작아(전국 기준도
 *   도로망 대비 극소수, doc/README.txt §F) O(n) 탐색 비용이 무시할 만한 수준이라 인덱스 추가는
 *   보류. 규모가 커지면 road_id 보조 인덱스로 재검토
 * @param[in] strRoadID 진입 시 저장해둔 세션의 road_id(base_roadlink.road_id)
 * @param[in] cGateDiv 찾을 게이트 구분('I' 또는 'O')
 * @return 일치하는 게이트 정보 포인터(없으면 nullptr)
*/
PGATE_INFO CChargeDataLoader::GetGateByRoadId(const string& strRoadID, const char cGateDiv)
{
	lock_guard<CMutex> cLock(m_cGateCacheMutex);
	for (mapGateInfo::iterator it = m_mapGateInfo.begin(); it != m_mapGateInfo.end(); ++it)
	{
		for (size_t i = 0; i < it->second.size(); ++i)
		{
			if ((strRoadID == it->second[i].szRoadID) && (cGateDiv == it->second[i].szGateDiv[0]))
				return &(it->second[i]);
		}
	}
	return nullptr;
}

/**
 * @brief tollgate_id 로 게이트 조회 — 진입 때 세션에 남긴 입구 게이트 ID 로 좌표를 다시 얻는다
 *   (2026-10-04 최정우 추가 — 게이트 간 거리 산출용). GetGateByRoadId 와 같은 선형 탐색
 * @param[in] pszTollgateID 게이트 ID
 * @return 게이트 정보 포인터(없으면 nullptr)
*/
PGATE_INFO CChargeDataLoader::GetGateByTollgateId(const char *pszTollgateID)
{
	if ((pszTollgateID == nullptr) || (pszTollgateID[0] == '\0'))
		return nullptr;
	lock_guard<CMutex> cLock(m_cGateCacheMutex);
	for (mapGateInfo::iterator it = m_mapGateInfo.begin(); it != m_mapGateInfo.end(); ++it)
	{
		for (size_t i = 0; i < it->second.size(); ++i)
		{
			if (strcmp(pszTollgateID, it->second[i].szTollgateID) == 0)
				return &(it->second[i]);
		}
	}
	return nullptr;
}

/**
 * @brief 로드된 게이트 총 개수 — link_id 공유로 vector 에 여러 건 들어간 경우도 전부 합산 (2026-08-12 최정우 추가)
 * @return 게이트 총 개수
*/
size_t CChargeDataLoader::GetGateCount() const
{
	lock_guard<CMutex> cLock(m_cGateCacheMutex);
	size_t nCount = 0;
	for (mapGateInfo::const_iterator it = m_mapGateInfo.begin(); it != m_mapGateInfo.end(); ++it)
		nCount += it->second.size();
	return nCount;
}

/**
 * @brief 좌표거리 폴백 조회 — link_id 매칭 실패 시에만 사용, 게이트 판정 2순위 (2026-08-12 최정우 추가)
 * @param[in] dfLat 현재 위치 위도(WGS84)
 * @param[in] dfLon 현재 위치 경도(WGS84)
 * @param[in] dfGateRadiusM 유효 반경(m) — config.ini [charge] gate_radius(아직 미배치, TODO)
 * @return 반경 내 가장 가까운 게이트 정보 포인터(없으면 nullptr)
*/
PGATE_INFO CChargeDataLoader::GetGateNearby(const double dfLat, const double dfLon, const double dfGateRadiusM)
{
	CCoordConvert cCoordConvert;
	CGISUtil cGISUtil;

	uint32 dwSearchLat = 0, dwSearchLon = 0;
	cCoordConvert.WGS84ToSearchCoord(dfLat, dfLon, &dwSearchLat, &dwSearchLon);
	POINT stCurPos;
	stCurPos.dfX = static_cast<double>(dwSearchLon);
	stCurPos.dfY = static_cast<double>(dwSearchLat);

	lock_guard<CMutex> cLock(m_cGateCacheMutex);
	// 게이트 총량이 작아(전국 기준으로도 도로망 대비 극소수) 선형 탐색으로 충분 — 규모가 커지면
	// GRID 분할(CDataLoader 패턴) 재검토 (2026-08-12 최정우 추가)
	PGATE_INFO pstNearest = nullptr;
	double dfNearestDist = dfGateRadiusM;
	for (mapGateInfo::iterator it = m_mapGateInfo.begin(); it != m_mapGateInfo.end(); ++it)
	{
		for (size_t i = 0; i < it->second.size(); ++i)
		{
			GATE_INFO& stGate = it->second[i];
			POINT stGatePos;
			stGatePos.dfX = static_cast<double>(stGate.dwSearchLon);
			stGatePos.dfY = static_cast<double>(stGate.dwSearchLat);

			double dfDist = cGISUtil.GetDistanceGEO1(stCurPos, stGatePos);
			if (dfDist <= dfNearestDist)
			{
				dfNearestDist = dfDist;
				pstNearest = &stGate;
			}
		}
	}
	return pstNearest;
}

/**
 * @brief base_roadlink 전량을 조회해 인메모리 캐시로 (재)로드 — 기동 시 1회, 또는 주기
 *        재조회 스레드에서 반복 호출 (2026-08-12 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CChargeDataLoader::LoadZones()
{
	if (m_pcPostgrePool == nullptr)
	{
		LOGFMTE("postgre pool not initialized!");
		return false;
	}
	if (m_strZoneSelectSQL.empty())
	{
		LOGFMTW("zone select sql is empty!skip zone load");
		return true;
	}

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("get connection failed!");
		return false;
	}

	PGresult *pcResult = PQexec(pcConn, m_strZoneSelectSQL.c_str());
	if (PQresultStatus(pcResult) != PGRES_TUPLES_OK)
	{
		LOGFMTE("zone select failed!error=[%s]", PQerrorMessage(pcConn));
		PQclear(pcResult);
		m_pcPostgrePool->releaseConnection(pcConn);
		return false;
	}

	// 컬럼 순서: road_id, road_kind, road_nm, geom_type, speed_limit_kmh, use_yn, link_ids, coords,
	//   length_m, first_lon, first_lat, last_lon, last_lat
	//   (query.sql [zone_select] 세션과 반드시 일치해야 함) (2026-08-12 최정우 수정 — first/last 좌표 추가)
	mapZoneInfo mapNewZoneInfo;
	int nRows = PQntuples(pcResult);
	for (int i = 0; i < nRows; i++)
	{
		const char *pszRoadID = PQgetvalue(pcResult, i, 0);
		const char *pszRoadKind = PQgetvalue(pcResult, i, 1);
		const char *pszRoadNm = PQgetvalue(pcResult, i, 2);
		const char *pszGeomType = PQgetvalue(pcResult, i, 3);
		bool bSpeedLimitNull = PQgetisnull(pcResult, i, 4);
		const char *pszSpeedLimit = bSpeedLimitNull ? nullptr : PQgetvalue(pcResult, i, 4);
		const char *pszUseYN = PQgetvalue(pcResult, i, 5);
		const char *pszLinkIds = PQgetvalue(pcResult, i, 6);
		const char *pszCoords = PQgetvalue(pcResult, i, 7);
		const char *pszLengthM = PQgetvalue(pcResult, i, 8);

		// [버그 수정, 2026-09-11 최정우] FIRST/LAST_LON/LAT 는 query.sql [zone_select] 에서
		//   COORDS->0->>0 등으로 계산되는데, COORDS(NOT NULL 컬럼이지만 빈 배열 '[]' 은 허용됨)가
		//   빈 배열이면 인덱스 접근 결과가 NULL 이 된다 — 지금까지 NULL 체크 없이 바로 atof() 해서
		//   조용히 (0,0)으로 캐시됐다. 좌표 폴리라인 자체가 없는(등록이 깨진) 구역이라 이 구역으로는
		//   과금 판정 자체가 의미 없으므로, 통째로 스킵한다(LENGTH_M 만 SQL 의 COALESCE 로 보호돼
		//   있어 이 검사와 무관하게 항상 0 — 값 검사 필요 없음).
		bool bFirstLonNull = PQgetisnull(pcResult, i, 9);
		bool bFirstLatNull = PQgetisnull(pcResult, i, 10);
		bool bLastLonNull = PQgetisnull(pcResult, i, 11);
		bool bLastLatNull = PQgetisnull(pcResult, i, 12);
		if (bFirstLonNull || bFirstLatNull || bLastLonNull || bLastLatNull)
		{
			LOGFMTE("zone coords is empty!skip row - zone id=[%s] road_kind=[%s]", pszRoadID, pszRoadKind);
			continue;
		}
		const char *pszFirstLon = PQgetvalue(pcResult, i, 9);
		const char *pszFirstLat = PQgetvalue(pcResult, i, 10);
		const char *pszLastLon = PQgetvalue(pcResult, i, 11);
		const char *pszLastLat = PQgetvalue(pcResult, i, 12);

		// road_id 중복(2번째부터) — 첫 행만 유지하고 이후는 스킵, 어떤 ID인지 에러로그로 남김.
		//   기존엔 mapNewZoneInfo[road_id]=값 대입이라 뒤 행이 앞 행을 조용히 덮어썼음(정상 시엔
		//   PK 위반으로 절대 안 나오는 케이스지만, 스키마 변경으로 중복이 생기면 그동안은 어느 쪽이
		//   남는지도 모른 채 유실됐음) (2026-08-14 최정우 추가)
		if (mapNewZoneInfo.find(pszRoadID) != mapNewZoneInfo.end())
		{
			LOGFMTE("duplicate road_id!skip 2nd+ row - zone id=[%s] road_kind=[%s]",
				pszRoadID, pszRoadKind);
			continue;
		}

		ZONE_INFO stZoneInfo;
		strncpy(stZoneInfo.szRoadID, pszRoadID, sizeof(stZoneInfo.szRoadID) - 1);
		strncpy(stZoneInfo.szRoadKind, pszRoadKind, sizeof(stZoneInfo.szRoadKind) - 1);
		strncpy(stZoneInfo.szRoadNm, pszRoadNm, sizeof(stZoneInfo.szRoadNm) - 1);
		strncpy(stZoneInfo.szGeomType, pszGeomType, sizeof(stZoneInfo.szGeomType) - 1);
		stZoneInfo.dfSpeedLimitKmh = bSpeedLimitNull ? 0.0 : atof(pszSpeedLimit);
		strncpy(stZoneInfo.szUseYN, pszUseYN, sizeof(stZoneInfo.szUseYN) - 1);
		stZoneInfo.strLinkIdsJson = pszLinkIds;
		stZoneInfo.strCoordsJson = pszCoords;
		stZoneInfo.dfLengthM = atof(pszLengthM);
		stZoneInfo.dfFirstLon = atof(pszFirstLon);
		stZoneInfo.dfFirstLat = atof(pszFirstLat);
		stZoneInfo.dfLastLon = atof(pszLastLon);
		stZoneInfo.dfLastLat = atof(pszLastLat);

		// POLY(주정차)만 PointInPolygon 판정용 정점 파싱 — LINE 은 LENGTH_M/FIRST/LAST 로 충분해
		//   불필요한 파싱·메모리를 아낌 (2026-08-13 최정우 추가)
		if (strcmp(stZoneInfo.szGeomType, "POLY") == 0)
			ParseCoordsJson(stZoneInfo.strCoordsJson, &stZoneInfo.vtCoords);
		// 폐쇄형(2)·구간단속(3) LINE 은 게이트 간 거리 산출용으로 파싱한다 (2026-10-04 최정우 추가, 사용자 확정)
		else if ((stZoneInfo.szRoadKind[0] == '2') || (stZoneInfo.szRoadKind[0] == '3'))
			ParseCoordsJson(stZoneInfo.strCoordsJson, &stZoneInfo.vtLineCoords);

		// 일반도로(ROAD_KIND=0)·면제도로(ROAD_KIND=5)만 link_ids 파싱 — 게이트가 없어 매칭
		//   링크→구역 역인덱스가 유일한 진입/이탈 판정 수단(다른 LINE 유형은 게이트 기반이라 불필요)
		//   (2026-08-13 최정우 추가, 2026-08-14 두 차례 수정 — ROAD_KIND=5 기반 판정을 "모든 미등록
		//   링크" 방식으로 재설계했다가 폐기하고, charge_type=0 통합 출력 방식으로 다시 zone 기반 부활)
		// 개방형(ROAD_KIND=1)도 추가 — 주행거리(dist_m)·주행시간 산출을 위해 "구역 진입~이탈"
		//   범위를 알아야 해서 link_ids 역인덱스가 필요해짐. 게이트는 여전히 있고 과금 유효성
		//   판정(charge_yn/status)에는 계속 쓰지만, 진입/이탈 자체는 이 역인덱스로 판정한다
		//   (2026-08-25 최정우 추가)
		// 폐쇄형(ROAD_KIND=2)·구간단속(ROAD_KIND=3)도 추가 — 이 둘은 여전히 게이트 기반으로
		//   진입/이탈을 판정하지만, 게이트를 못 찾은 채 매칭 링크가 이 구역 link_ids 를 완전히
		//   벗어나면(=확정 이탈) dist_m/speed_kmh/stay_seconds 를 0 대신 실측값으로 채우기 위한
		//   보조 판정에 씀(실측 000376_20260819140856 — 게이트 미확인 이탈 시 전부 0으로 비어있던
		//   문제, 2026-08-25 최정우 추가). 역인덱스는 안 만듦 — CLOSED/SPEED는 "이 특정 구역
		//   안에 있는가"만 확인하면 되고(진행 중인 road_id 를 이미 알고 있음), 링크→구역 전체
		//   매핑이 필요한 NODE_STEP/EXEMPT/OPEN과 달리 GetZoneByRoadId() 로 얻은 그 zone 하나의
		//   vtLinkIds 만 선형 탐색하면 충분함(구역당 링크 수가 적어 성능 문제 없음).
		if ((strcmp(stZoneInfo.szRoadKind, "0") == 0) || (strcmp(stZoneInfo.szRoadKind, "1") == 0)
			|| (strcmp(stZoneInfo.szRoadKind, "2") == 0) || (strcmp(stZoneInfo.szRoadKind, "3") == 0)
			|| (strcmp(stZoneInfo.szRoadKind, "5") == 0))
			ParseLinkIdsJson(stZoneInfo.strLinkIdsJson, &stZoneInfo.vtLinkIds);

		mapNewZoneInfo[stZoneInfo.szRoadID] = stZoneInfo;
	}

	PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);

	// 일반도로(NODE_STEP) link_id → road_id 역인덱스 재구성 (2026-08-14 최정우 추가)
	unordered_map<uint64, vector<string> > mapNewNodeStepLinkToRoadId;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		if (strcmp(it->second.szRoadKind, "0") != 0) continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			mapNewNodeStepLinkToRoadId[it->second.vtLinkIds[i]].push_back(it->second.szRoadID);
	}

	// 면제도로 link_id → road_id 역인덱스 재구성 (2026-08-14 최정우 부활)
	unordered_map<uint64, vector<string> > mapNewExemptLinkToRoadId;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		if (strcmp(it->second.szRoadKind, "5") != 0) continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			mapNewExemptLinkToRoadId[it->second.vtLinkIds[i]].push_back(it->second.szRoadID);
	}

	// 개방형 link_id → road_id 역인덱스 재구성 (2026-08-25 최정우 추가)
	unordered_map<uint64, vector<string> > mapNewOpenLinkToRoadId;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		if (strcmp(it->second.szRoadKind, "1") != 0) continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			mapNewOpenLinkToRoadId[it->second.vtLinkIds[i]].push_back(it->second.szRoadID);
	}

	// 구간단속 link_id → road_id 역인덱스 재구성 — IsCase3EligibleRoadKind() 의 "이 링크가 구간단속
	//   구역 소속인가" O(1) 판정용. 게이트 기반 진입/이탈 판정 자체는 그대로 유지, 이 역인덱스는
	//   그와 별개로 신설 (2026-09-01 최정우 추가)
	unordered_map<uint64, vector<string> > mapNewSpeedLinkToRoadId;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		if (strcmp(it->second.szRoadKind, "3") != 0) continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			mapNewSpeedLinkToRoadId[it->second.vtLinkIds[i]].push_back(it->second.szRoadID);
	}

	// 폐쇄식(ROAD_KIND=2) link_id → road_id 역인덱스 재구성 — 입구게이트를 통과하지 않은 채 그
	//   구역 링크를 지나는 구간에 구역명을 붙이기 위한 조회용. 폐쇄식은 여기가 유일한 역인덱스다.
	//   개방식(1)·구간단속(3)을 뺀 이유는 헤더 멤버 주석 참고 (2026-09-23 최정우 추가, 사용자 확정)
	unordered_map<uint64, vector<string> > mapNewClosedZoneLinkToRoadId;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		if (strcmp(it->second.szRoadKind, "2") != 0) continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			mapNewClosedZoneLinkToRoadId[it->second.vtLinkIds[i]].push_back(it->second.szRoadID);
	}

	// 전체 등록 링크 합집합(ROAD_KIND 0/1/2/3/5) — NODE_STEP 케이스2("어떤 과금유형에도 등록 안 된
	//   도로") 판정용. PARKING(4)은 폴리곤 기반이라 링크ID 매핑 개념이 없어 제외 (2026-09-01 최정우 추가)
	unordered_set<uint64> setNewAllRegisteredLinkIds;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		const char *pszKind = it->second.szRoadKind;
		if ((strcmp(pszKind, "0") != 0) && (strcmp(pszKind, "1") != 0) && (strcmp(pszKind, "2") != 0)
			&& (strcmp(pszKind, "3") != 0) && (strcmp(pszKind, "5") != 0))
			continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			setNewAllRegisteredLinkIds.insert(it->second.vtLinkIds[i]);
	}

	{
		// 재조회 시점 교체 — m_cZoneCacheMutex 로 조회 스레드(GetZoneByRoadId/GetParkingZoneContaining 등)와
		// 동기화(doc/README.txt §F 패턴, 2026-08-14 최정우 수정 — 락 없이 swap만 하던 걸 수정).
		// 구역 맵과 역인덱스 전부를 **같은 락 안에서 함께** 교체해 서로 항상 일관된 스냅샷 유지
		// (역인덱스를 새로 추가할 때는 이 블록·Uninitialize()·m_cZoneCacheMutex 주석 세 곳을 같이 고칠 것).
		// m_mapZoneInfo 만 이전 세대를 즉시 파괴하지 않고 retired 목록에 보관 — GetZoneByRoadId 등이
		// 반환하는 PZONE_INFO 는 그 주소를 외부에 노출하지만, 역인덱스 맵(link_id→road_id 문자열)들은
		// 내부에서 조회 즉시 GetZoneByRoadId 로 넘길 뿐 자기 주소를 외부에 노출한 적이 없어 단순 swap으로
		// 충분함(ChargeDataLoader.h m_dqRetiredZoneInfo 주석 참고, 2026-08-14 최정우 추가)
		lock_guard<CMutex> cLock(m_cZoneCacheMutex);
		m_dqRetiredZoneInfo.push_back(std::move(m_mapZoneInfo));
		m_mapZoneInfo = std::move(mapNewZoneInfo);
		while (m_dqRetiredZoneInfo.size() > RETIRED_CACHE_GENERATIONS)
			m_dqRetiredZoneInfo.pop_front();
		m_mapNodeStepLinkToRoadId.swap(mapNewNodeStepLinkToRoadId);
		m_mapExemptLinkToRoadId.swap(mapNewExemptLinkToRoadId);
		m_mapOpenLinkToRoadId.swap(mapNewOpenLinkToRoadId);
		m_mapSpeedLinkToRoadId.swap(mapNewSpeedLinkToRoadId);
		m_mapClosedZoneLinkToRoadId.swap(mapNewClosedZoneLinkToRoadId);	// (2026-09-23 최정우 추가)
		m_setAllRegisteredLinkIds.swap(setNewAllRegisteredLinkIds);
	}
	m_bLoad = true;

	LOGFMTI("zone cache loaded!count=[%zu]", GetZoneCount());
	return true;
}

/**
 * @brief base_parking_fine 최소 from_min(분)을 초로 환산해 (재)로드 — 기동 시 1회, 또는 게이트·구역과
 *        같은 주기 재조회 스레드에서 반복 호출. 세션 미설정·SQL 없음은 실패가 아니라 "임계 비활성"
 *        (사용자 지시, 2026-08-24 최정우 추가)
 * @return true(조회 성공, 테이블이 비어 있어도 true — 그 경우 임계 0으로 비활성), false(조회 자체 실패)
*/
bool CChargeDataLoader::LoadParkingFine()
{
	if (m_strParkFineSelectSQL.empty())
		return true;								// 세션 미설정 — 애초에 비활성, 실패 아님

	if (m_pcPostgrePool == nullptr)
	{
		LOGFMTE("postgre pool not initialized!");
		return false;
	}

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("get connection failed!");
		return false;
	}

	PGresult *pcResult = PQexec(pcConn, m_strParkFineSelectSQL.c_str());
	if (PQresultStatus(pcResult) != PGRES_TUPLES_OK)
	{
		LOGFMTE("park fine select failed!error=[%s]", PQerrorMessage(pcConn));
		PQclear(pcResult);
		m_pcPostgrePool->releaseConnection(pcConn);
		return false;
	}

	// MIN(from_min) — 테이블이 비어 있으면 NULL(임계 비활성, 0) (2026-08-24 최정우 추가)
	int nMinSec = 0;
	if ((PQntuples(pcResult) > 0) && !PQgetisnull(pcResult, 0, 0))
		nMinSec = atoi(PQgetvalue(pcResult, 0, 0)) * 60;

	PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);

	{
		lock_guard<CMutex> cLock(m_cParkFineMutex);
		m_nParkFineMinSec = nMinSec;
	}

	LOGFMTI("park fine minimum loaded!min_sec=[%d]", nMinSec);
	return true;
}

/**
 * @brief road_id로 구역 조회 — O(1), 개방형 zone_name 조회 등에 사용 (2026-08-12 최정우 추가)
 * @param[in] strRoadID 조회할 구역 ID(base_roadlink.road_id)
 * @return 구역 정보 포인터(없으면 nullptr)
*/
PZONE_INFO CChargeDataLoader::GetZoneByRoadId(const string& strRoadID)
{
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	mapZoneInfo::iterator it = m_mapZoneInfo.find(strRoadID);
	if (it == m_mapZoneInfo.end())
		return nullptr;

	return &(it->second);
}

/**
 * @brief 주정차(POLY) 구역 중 (dfLon,dfLat) 포함 구역 조회 — 게이트가 없어 road_id 선판별 불가,
 *   POLY 전량 순회 (2026-08-13 최정우 추가)
 * @param[in] dfLon 판정 경도(raw GPS, 맵매칭 전)
 * @param[in] dfLat 판정 위도
 * @param[in] dfPadM 폴리곤 경계 바깥 확장 허용거리(m). 0 이하면 미적용(폴리곤 내부만 인정)
 * @return 포함 구역, 없으면 nullptr
*/
PZONE_INFO CChargeDataLoader::GetParkingZoneContaining(const double dfLon, const double dfLat, const double dfPadM)
{
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	for (mapZoneInfo::iterator it = m_mapZoneInfo.begin(); it != m_mapZoneInfo.end(); ++it)
	{
		ZONE_INFO& stZone = it->second;
		if (strcmp(stZone.szGeomType, "POLY") != 0) continue;
		if (stZone.vtCoords.size() < 3) continue;

		if (IsPointInPolygon(dfLon, dfLat, stZone.vtCoords))
			return &stZone;

		if ((dfPadM > 0.0) && (DistanceToPolygonBoundaryMeters(dfLon, dfLat, stZone.vtCoords) <= dfPadM))
			return &stZone;
	}
	return nullptr;
}

/**
 * @brief 주정차 구역 복수 조회 — 폴리곤이 겹쳐 설정될 수 있다(시간대별 규제가 다른 구역 등)
 *   (2026-08-23 최정우 추가)
 * @param[out] pvtOut 좌표를 포함하는 구역 전부(없으면 빈 목록)
*/
void CChargeDataLoader::GetParkingZonesContaining(const double dfLon, const double dfLat,
		const double dfPadM, vector<PZONE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	for (mapZoneInfo::iterator it = m_mapZoneInfo.begin(); it != m_mapZoneInfo.end(); ++it)
	{
		ZONE_INFO& stZone = it->second;
		if (strcmp(stZone.szGeomType, "POLY") != 0) continue;
		if (stZone.vtCoords.size() < 3) continue;

		if (IsPointInPolygon(dfLon, dfLat, stZone.vtCoords)
			|| ((dfPadM > 0.0)
				&& (DistanceToPolygonBoundaryMeters(dfLon, dfLat, stZone.vtCoords) <= dfPadM)))
			pvtOut->push_back(&stZone);
	}
}

/**
 * @brief 일반도로(ROAD_KIND=0, NODE_STEP) 구역 조회 — 매칭 링크 ID로 역인덱스 O(1) 조회 (2026-08-14 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @return 그 링크가 속한 일반도로 구역(없으면 nullptr)
*/
PZONE_INFO CChargeDataLoader::GetNodeStepZoneByLinkId(const uint64 qwLinkID)
{
	// m_cZoneCacheMutex 는 CMutex(재귀 락) — 아래 GetZoneByRoadId() 가 같은 락을 다시 잡아도
	// 같은 스레드면 데드락 없음 (2026-08-14 최정우 추가)
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapNodeStepLinkToRoadId.find(qwLinkID);
	if ((itLink == m_mapNodeStepLinkToRoadId.end()) || itLink->second.empty())
		return nullptr;

	return GetZoneByRoadId(itLink->second[0]);		// 호환용 — 첫 구역만. 복수는 아래 함수 사용
}

/**
 * @brief 일반도로 구역 복수 조회 — 한 링크가 여러 구역에 속할 수 있다 (2026-08-23 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @param[out] pvtOut 그 링크가 속한 구역 전부(없으면 빈 목록). 호출측이 비우지 않아도 되게 clear 하지 않는다
*/
void CChargeDataLoader::GetNodeStepZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapNodeStepLinkToRoadId.find(qwLinkID);
	if (itLink == m_mapNodeStepLinkToRoadId.end()) return;
	for (size_t i = 0; i < itLink->second.size(); ++i)
	{
		PZONE_INFO p = GetZoneByRoadId(itLink->second[i]);
		if (p != nullptr) pvtOut->push_back(p);
	}
}

/**
 * @brief 면제도로(ROAD_KIND=5) 구역 조회 — 매칭 링크 ID로 역인덱스 O(1) 조회 (2026-08-14 최정우 부활)
 * @param[in] qwLinkID 매칭 링크 ID
 * @return 그 링크가 속한 면제도로 구역(없으면 nullptr)
*/
PZONE_INFO CChargeDataLoader::GetExemptZoneByLinkId(const uint64 qwLinkID)
{
	// m_cZoneCacheMutex 는 CMutex(재귀 락) — 아래 GetZoneByRoadId() 가 같은 락을 다시 잡아도
	// 같은 스레드면 데드락 없음
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapExemptLinkToRoadId.find(qwLinkID);
	if ((itLink == m_mapExemptLinkToRoadId.end()) || itLink->second.empty())
		return nullptr;

	return GetZoneByRoadId(itLink->second[0]);		// 호환용 — 첫 구역만
}

/**
 * @brief 개방형(ROAD_KIND=1) 구역 복수 조회 — 매칭 링크 ID로 역인덱스 O(1) 조회 (2026-08-25 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @param[out] pvtOut 그 링크가 속한 개방형 구역 전부(없으면 빈 목록)
*/
void CChargeDataLoader::GetOpenZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapOpenLinkToRoadId.find(qwLinkID);
	if (itLink == m_mapOpenLinkToRoadId.end()) return;
	for (size_t i = 0; i < itLink->second.size(); ++i)
	{
		PZONE_INFO p = GetZoneByRoadId(itLink->second[i]);
		if (p != nullptr) pvtOut->push_back(p);
	}
}

/**
 * @brief 면제도로 구역 복수 조회 (2026-08-23 최정우 추가)
*/
void CChargeDataLoader::GetExemptZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapExemptLinkToRoadId.find(qwLinkID);
	if (itLink == m_mapExemptLinkToRoadId.end()) return;
	for (size_t i = 0; i < itLink->second.size(); ++i)
	{
		PZONE_INFO p = GetZoneByRoadId(itLink->second[i]);
		if (p != nullptr) pvtOut->push_back(p);
	}
}

/**
 * @brief 링크가 어떤 과금유형(ROAD_KIND 0/1/2/3/5)에도 등록돼 있는지 — NODE_STEP 케이스2
 *   ("어떤 과금유형에도 등록 안 된 도로") 판정용 (2026-09-01 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @return false=미등록(케이스2 대상)
*/
bool CChargeDataLoader::IsLinkChargeRegistered(const uint64 qwLinkID)
{
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	return m_setAllRegisteredLinkIds.find(qwLinkID) != m_setAllRegisteredLinkIds.end();
}

/**
 * @brief 링크가 게이트형 구역(ROAD_KIND 1 개방식·2 폐쇄식·3 구간단속)에만 등록돼 있는지 판정
 *   (2026-09-07 최정우 추가, 사용자 지시)
 * @param[in] qwLinkID 매칭 링크 ID
 * @return true = 등록은 돼 있으나 일반도로(0)·면제(5) 역인덱스에는 없는 링크
 * @remark 게이트형 구역은 "게이트를 통과한 구간"만 그 유형으로 과금된다. 게이트를 통과하지 않았거나
 *   이미 진출한 뒤에도 같은 링크 위에 남아 있는 tick 은 그 유형으로 계상되지 않으므로, 사용자
 *   원칙("게이트 미충족 구간은 일반도로 Y/0")에 따라 일반도로(0)로 흡수해야 한다. 그 판단에
 *   필요한 "이 링크는 게이트형에만 묶여 있다"를 여기서 돌려주고, "지금 그 구역 run 이 열려
 *   있는가"는 세션 상태를 아는 CRawLogWorker::IsLinkNodeStepEligible() 이 판단한다.
 *   판정은 개별 map 조회가 아니라 "등록됐는데 0·5 가 아니다"라는 **소거법**이다 — 게이트형 3종
 *   (1·2·3)을 하나씩 조회하지 않아도 같은 답이 나오고, 유형이 늘어도 이 함수는 그대로다.
 *   [2026-09-23 최정우 정정] 종전 주석의 "폐쇄식(2)은 link_id 역인덱스가 없어" 는 더 이상 사실이
 *   아니다 — 구역 중간 진입(입구게이트 미통과) 판정을 위해 m_mapClosedZoneLinkToRoadId 를 신설했다
 *   (GetClosedZoneRoadIdByLinkId 참고). 다만 이 함수의 소거법 자체는 그 역인덱스와 무관하게
 *   유효하므로 구현은 바뀌지 않았다.
 * @remark [2026-09-23 최정우 추가] 이 함수가 true 를 돌려주는 링크라도 **폐쇄식은 그 구역 run 이
 *   중간 진입으로 열릴 수 있다**(ProcessClosedRoadCharge 의 "구역 중간 진입" 블록). 그러면
 *   IsLinkNodeStepEligible() 의 ③ 분기가 false 가 되어 일반도로로 흡수되지 않는다 — 즉 이
 *   함수의 반환값만으로 "일반도로로 계상된다"고 단정하면 안 된다.
*/
bool CChargeDataLoader::IsLinkGateZoneOnly(const uint64 qwLinkID)
{
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	if (m_setAllRegisteredLinkIds.find(qwLinkID) == m_setAllRegisteredLinkIds.end())
		return false;										// 어디에도 미등록 — 기존 케이스2 대상
	if (m_mapNodeStepLinkToRoadId.find(qwLinkID) != m_mapNodeStepLinkToRoadId.end())
		return false;										// 일반도로(0) 정식구역 — 그 트랙이 처리
	if (m_mapExemptLinkToRoadId.find(qwLinkID) != m_mapExemptLinkToRoadId.end())
		return false;										// 면제(5) — 과금 대상이 아니므로 흡수 금지
	return true;
}

/**
 * @brief 링크가 속한 폐쇄식 구역(ROAD_KIND=2)의 road_id 조회
 *   (2026-09-23 최정우 추가, 사용자 확정)
 * @param[in] qwLinkID  매칭 링크 ID
 * @param[out] pszOut   찾은 road_id — 못 찾으면 빈 문자열
 * @param[in] nOutSize  pszOut 버퍼 크기
 * @return true=폐쇄식 구역 소속(pszOut 채움)
 * @remark IsLinkGateZoneOnly() 가 "게이트형에만 묶인 링크인가"를 돌려주는 것과 짝이다. 그쪽이
 *   true 이고 그 유형 run 이 열려 있지 않으면 그 tick 은 일반도로로 계상되는데(사용자 원칙
 *   "게이트 조건 미충족 구간은 일반도로 Y/0"), 그때 **어느 구역을 게이트 없이 지났는지**를
 *   과금 이력에 남기기 위해 이 함수로 구역을 찾는다. 판정 자체는 바꾸지 않는다 — 기록만 붙인다.
*/
bool CChargeDataLoader::GetClosedZoneRoadIdByLinkId(const uint64 qwLinkID, char *pszOut,
		const size_t nOutSize)
{
	if ((pszOut == nullptr) || (nOutSize == 0))
		return false;
	pszOut[0] = '\0';

	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapClosedZoneLinkToRoadId.find(qwLinkID);
	if ((itLink == m_mapClosedZoneLinkToRoadId.end()) || itLink->second.empty())
		return false;

	snprintf(pszOut, nOutSize, "%s", itLink->second[0].c_str());
	return true;
}

/**
 * @brief 링크가 속한 구간단속 구역(ROAD_KIND=3)의 road_id 조회
 *   (2026-09-23 최정우 추가, 사용자 지시)
 * @param[in] qwLinkID  매칭 링크 ID
 * @param[out] pszOut   찾은 road_id — 못 찾으면 빈 문자열
 * @param[in] nOutSize  pszOut 버퍼 크기
 * @return true=구간단속 구역 소속(pszOut 채움)
 * @remark GetClosedZoneRoadIdByLinkId() 와 같은 목적·같은 관례다 — 입구게이트를 통과하지 않고
 *   구역 중간으로 진입한 경우에도 그 유형 run 을 열어 N/3 + 사유코드로 남기기 위한 조회.
 *   폐쇄식은 역인덱스를 새로 만들어야 했지만(게이트 기반이라 없었다) 구간단속은
 *   m_mapSpeedLinkToRoadId 가 IsCase3EligibleRoadKind() 용으로 이미 있어 그대로 재사용한다.
*/
bool CChargeDataLoader::GetSpeedZoneRoadIdByLinkId(const uint64 qwLinkID, char *pszOut,
		const size_t nOutSize)
{
	if ((pszOut == nullptr) || (nOutSize == 0))
		return false;
	pszOut[0] = '\0';

	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapSpeedLinkToRoadId.find(qwLinkID);
	if ((itLink == m_mapSpeedLinkToRoadId.end()) || itLink->second.empty())
		return false;

	snprintf(pszOut, nOutSize, "%s", itLink->second[0].c_str());
	return true;
}

/**
 * @brief 케이스3(SKIP 구간) 전후 확정링크 스코프 판정 — road_kind=0(일반도로) 또는 3(구간단속)
 *   소속, 또는 어디에도 미등록이면 true (2026-09-01 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID(SKIP 구간의 FROM/TO 확정 링크)
 * @return true=이 링크는 NODE_STEP 케이스3 등록 대상 범위 — 그 외(OPEN/CLOSED/PARKING/EXEMPT
 *   소속)는 false, 그 유형 자체 로직이 처리해야 함(질문2 확정 답변)
*/
bool CChargeDataLoader::IsCase3EligibleRoadKind(const uint64 qwLinkID)
{
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	if (m_mapNodeStepLinkToRoadId.find(qwLinkID) != m_mapNodeStepLinkToRoadId.end()) return true;
	if (m_mapSpeedLinkToRoadId.find(qwLinkID) != m_mapSpeedLinkToRoadId.end()) return true;
	return m_setAllRegisteredLinkIds.find(qwLinkID) == m_setAllRegisteredLinkIds.end();
}

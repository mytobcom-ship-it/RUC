/**
 * @file ChargeDataLoader.h
 * @brief 과금 게이트 정보 로딩 클래스 헤더 파일
*/
#ifndef __CHARGEDATALOADER_H__
#define __CHARGEDATALOADER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include "TypeDefine.h"
#include "DataFormat.h"
#include "PostgrePool.h"
#include "Mutex.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

#pragma pack(push, 1)

/**
 * @struct sGateInfo
 * @brief base_tollgate 1행에 대응하는 게이트 정보
 * @remark
 *   - dwSearchLon/dwSearchLat : CCoordConvert::WGS84ToSearchCoord 로 미리 변환해둔 내부 스케일 좌표
 *     (LINK_INFO.dwStNodeX/Y 와 동일한 사전변환 방식) — GetGateNearby 매 호출마다 재변환하지 않기 위함
 *     (2026-08-12 최정우 추가)
*/
typedef struct sGateInfo
{
	char							szTollgateID[20+1];					// 게이트 ID (예: TG00009)
	char							szRoadID[20+1];							// 소속 구역 ID (base_roadlink.road_id)
	char							szGateDiv[1+1];							// 게이트 구분 M/I/O/B
	double							dfLon;									// 경도(WGS84, 원본 도)
	double							dfLat;									// 위도(WGS84, 원본 도)
	uint32							dwSearchLon;							// 경도(내부 스케일, WGS84ToSearchCoord 결과)
	uint32							dwSearchLat;							// 위도(내부 스케일, WGS84ToSearchCoord 결과)
	uint64							qwLinkID;								// 매칭 링크 ID (0=없음)

	sGateInfo() :
		dfLon(0.0),
		dfLat(0.0),
		dwSearchLon(0),
		dwSearchLat(0),
		qwLinkID(0)
	{
		memset(szTollgateID, 0, sizeof(szTollgateID));
		memset(szRoadID, 0, sizeof(szRoadID));
		memset(szGateDiv, 0, sizeof(szGateDiv));
	}
} GATE_INFO, *PGATE_INFO;

#define GATE_INFO_SIZE					sizeof(GATE_INFO)
// vector — 폐쇄형/구간단속 진입·출구 게이트가 같은 link_id 를 공유하는 실사례가 있어(TG00007/08,
//   TG00012/13 확인됨) link_id 당 게이트 1개로 가정한 단순 map 은 later 로 덮여써서 데이터 유실됨.
//   반드시 vector 로 전부 보관하고, gate_div 로 걸러서 원하는 쪽을 골라야 함 (2026-08-12 최정우 수정)
typedef unordered_map<uint64, vector<GATE_INFO>>	mapGateInfo;

#pragma pack(pop)

// ZONE_INFO는 string 멤버(가변길이 jsonb 원문)가 있어 GATE_INFO와 달리 1바이트 패킹 대상이
// 아님 — GATE_INFO/LINK_INFO 계열은 바이너리(psf) 레이아웃과 맞출 필요가 있어 패킹했지만,
// ZONE_INFO는 DB 조회 결과를 필드별로 채우는 구조라 애초에 패킹이 불필요함 (2026-08-12 최정우 추가)
/**
 * @struct sZoneInfo
 * @brief base_roadlink 1행에 대응하는 구역 정보 (2026-08-12 최정우 추가)
 * @remark
 *   - [2026-09-21 최정우 정정] 종전 주석의 "JSON 파서가 아직 없어 지금은 미파싱" 은 낡았다 —
 *     LoadZones() 가 ParseCoordsJson()/ParseLinkIdsJson() 으로 **이미 파싱해** vtCoords(POLY 만)·
 *     vtLinkIds 를 채우고, 구역 진입·이탈 판정이 그 결과를 쓴다. strCoordsJson/strLinkIdsJson 은
 *     파싱 입력으로만 남아 있는 원문 보관 필드다.
*/
typedef struct sZoneInfo
{
	char							szRoadID[20+1];							// 구역 ID (base_roadlink.road_id)
	char							szRoadKind[2+1];						// 유형 0~5(일반/개방/폐쇄/구간단속/주정차/면제도로) (2026-08-13 최정우 수정 — 5 추가,
																				//   2026-09-17 최정우 — 옛 이름 "비과금도로"를 현행 "면제도로"로 통일)
	char							szRoadNm[100+1];						// 구역명 — 개방형 zone_name 으로 사용
	char							szGeomType[4+1];						// LINE/POLY
	double							dfSpeedLimitKmh;						// 제한속도(구간단속용, 해당없음=0)
	char							szUseYN[1+1];
	string							strLinkIdsJson;							// link_ids 원본(jsonb 텍스트) — ParseLinkIdsJson() 입력. 결과는 vtLinkIds
	string							strCoordsJson;							// coords 원본(jsonb 텍스트) — ParseCoordsJson() 입력. 결과는 vtCoords(POLY 만)
	double							dfLengthM;								// coords 폴리라인 실거리(m) — [zone_select] SQL 에서
																			//   하버사인 합산 계산됨. 폐쇄형·구간단속 dist_m 산출에 사용 (2026-08-12 최정우 추가)
	double							dfFirstLon;								// coords 첫 정점 — 구간단속 from_lon (2026-08-12 최정우 추가)
	double							dfFirstLat;								// coords 첫 정점 — 구간단속 from_lat
	double							dfLastLon;								// coords 마지막 정점 — 구간단속 to_lon
	double							dfLastLat;								// coords 마지막 정점 — 구간단속 to_lat
	vector<POINT>					vtCoords;								// coords 파싱 결과 — GEOM_TYPE='POLY'(주정차)에서만 채움,
																			//   PointInPolygon 판정용. LINE 은 LENGTH_M/FIRST/LAST 로 충분해 미파싱 (2026-08-13 최정우 추가)
	vector<POINT>					vtLineCoords;							// coords 파싱 결과 — 폐쇄형(2)·구간단속(3) LINE 만 채움.
																			//   진입·진출 게이트 사이 거리를 구역 선을 따라 재는 데 쓴다
																			//   (CalcZoneSpanM, 2026-10-04 최정우 추가 — 사용자 확정). vtCoords 와
																			//   분리한 이유: vtCoords 는 주정차 폴리곤 판정 루프가 크기만 보고 쓴다
	vector<uint64>					vtLinkIds;								// link_ids 파싱 결과 — ROAD_KIND='0'(일반도로)·'1'(개방형)·'5'(면제
																			//   도로)는 매칭 링크→구역 역인덱스 구성용(게이트가 없거나 진입/이탈
																			//   판정 자체를 이걸로 함, 2026-08-13/25 최정우 추가). '2'(폐쇄형)·
																			//   '3'(구간단속)도 파싱은 되지만 진입/이탈은 여전히 게이트 기반 —
																			//   게이트를 못 찾고 이 링크들을 벗어났는지("확정 이탈")만 보조로
																			//   확인하는 용도(2026-08-25 최정우 추가)
																			//   [2026-09-23 최정우 보완] 폐쇄형(2)은 용도가 하나 늘었다 —
																			//   m_mapClosedZoneLinkToRoadId(링크→구역 역인덱스)를 이 목록으로
																			//   구성해, **입구게이트를 통과하지 않고 구역 중간으로 진입한
																			//   경우**의 진입 판정에 쓴다(ProcessClosedRoadCharge). 즉 폐쇄형의
																			//   진입 경로는 "게이트 통과" 와 "구역 링크 진입(게이트 미확인)"
																			//   두 가지가 됐다. 구간단속(3)은 종전대로 게이트 기반이다.

	sZoneInfo() :
		dfSpeedLimitKmh(0.0),
		dfLengthM(0.0),
		dfFirstLon(0.0),
		dfFirstLat(0.0),
		dfLastLon(0.0),
		dfLastLat(0.0)
	{
		memset(szRoadID, 0, sizeof(szRoadID));
		memset(szRoadKind, 0, sizeof(szRoadKind));
		memset(szRoadNm, 0, sizeof(szRoadNm));
		memset(szGeomType, 0, sizeof(szGeomType));
		memset(szUseYN, 0, sizeof(szUseYN));
	}
} ZONE_INFO, *PZONE_INFO;

#define ZONE_INFO_SIZE					sizeof(ZONE_INFO)
typedef unordered_map<string, ZONE_INFO>	mapZoneInfo;

/**
 * @class CChargeDataLoader
 * @brief base_tollgate 전량을 메모리로 로드하고 조회를 제공하는 클래스
 * @remark
 *   - CDataLoader(link.psf 로딩)와 동일하게 "로드 1회 → 이후 인메모리 조회" 철학을 따름
 *   - link_id 우선 O(1) 조회, 실패 시 좌표거리(gate_radius) 폴백 — 게이트 판정(MatchGate)에서 사용
 *   - LoadGates()/LoadZones()는 기동 시 1회 호출 + config.ini [charge] gate_reload(sec)>0 이면
 *     Server.cpp 타이머 스레드에서 주기 재호출(RawLogWorker 워커 스레드들과는 별도 스레드) — 매번
 *     새 맵을 만들어 std::swap 으로 교체, m_cGateCacheMutex/m_cZoneCacheMutex 로 스왑 구간과 조회
 *     함수(GetGateByLinkId 등) 양쪽을 짧게 잠가 보호(doc/README.txt §F 패턴) (2026-08-12 최정우 추가)
 *   - 2026-08-14 최정우 수정: 원래 이 주석은 "락 경합 최소화"라 적혀 있었으나 실제로는 뮤텍스가
 *     빠져 있어 재조회 스레드의 swap()과 워커 스레드의 조회(특히 GetParkingZoneContaining 의 전체
 *     순회)가 동시에 발생하면 std::unordered_map 동시 접근 미정의동작(UB) 위험이 있었음 — 뮤텍스
 *     추가로 수정. CMutex(Mutex.h)는 PTHREAD_MUTEX_RECURSIVE 라 GetNodeStepZoneByLinkId 가 내부에서
 *     GetZoneByRoadId 를 다시 호출해도(같은 스레드) 데드락 없음
*/
class CChargeDataLoader
{
public:
	CChargeDataLoader();
	virtual ~CChargeDataLoader();

	bool Initialize(CPostgrePool *pcPostgrePool, const string& strGateSelectSQL,
		const string& strZoneSelectSQL, const string& strParkFineSelectSQL = "");
	void Uninitialize();
	bool LoadGates();
	bool LoadZones();
	// base_parking_fine 최소 from_min(분)을 초로 환산해 캐시 — 주정차 체류시간(STAY_SECONDS)이
	//   이 값 미만이면 BuildParkRow() 가 등록 대상에서 제외시킬 때 씀 (사용자 지시, 2026-08-24 최정우 추가)
	bool LoadParkingFine();

	PGATE_INFO GetGateByLinkId(const uint64 qwLinkID, const char cGateDiv = 0);
	// GetGateByLinkId 는 첫 매치 1개만 반환 — 같은 link_id 에 같은 gate_div 게이트가 2개 이상
	//   있으면 그중 하나만 처리되고 나머지는 부과 누락됨. 개방형(M)이 이 상황을 실제로 전부
	//   처리해야 할 때 사용(2026-08-13 최정우 추가)
	void GetGatesByLinkId(const uint64 qwLinkID, const char cGateDiv, vector<PGATE_INFO> *pvtOut);
	PGATE_INFO GetGateNearby(const double dfLat, const double dfLon, const double dfGateRadiusM);
	PGATE_INFO GetGateByRoadId(const string& strRoadID, const char cGateDiv);
	PGATE_INFO GetGateByTollgateId(const char *pszTollgateID);				// (2026-10-04 최정우 추가)
	PZONE_INFO GetZoneByRoadId(const string& strRoadID);
	// 주정차(POLY) 구역 중 (dfLon,dfLat) 을 포함하는 구역 — 게이트가 없어 road_id 를 미리 알 수 없으므로
	//   POLY 전량을 순회하며 판정. dfPadM>0 이면 폴리곤 경계까지 거리가 그 이내인 경우도 포함(ACCURACY_M
	//   오차만큼 바깥으로 확장 허용 — 서행/정차 중 GPS 튐으로 경계 근처에서 순간 이탈로 오판되는 것 방지)
	//   (2026-08-13 최정우 추가, 2026-09-03 dfBufM 에서 개명)
	PZONE_INFO GetParkingZoneContaining(const double dfLon, const double dfLat, const double dfPadM);
	// 일반도로(ROAD_KIND=0, NODE_STEP) — 게이트가 없어 매칭 링크 ID로 직접 역인덱스 조회. 없으면 nullptr
	//   (2026-08-14 최정우 추가)
	PZONE_INFO GetNodeStepZoneByLinkId(const uint64 qwLinkID);
	// 한 링크가 여러 구역에 속할 수 있어 전부 돌려준다 (2026-08-23 최정우 추가)
	void GetNodeStepZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut);
	// 면제도로(ROAD_KIND=5) — 게이트가 없어 매칭 링크 ID로 직접 역인덱스 조회. 없으면 nullptr
	//   (2026-08-13 최초 추가, charge_type=5로 썼다가 2026-08-14 폐기, 다시 2026-08-14 부활 —
	//   출력 charge_type만 0으로 바뀌고 판정 방식은 원래의 zone 기반으로 복귀)
	PZONE_INFO GetExemptZoneByLinkId(const uint64 qwLinkID);
	void GetExemptZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut);
	// 개방형(ROAD_KIND=1) — 주행거리·주행시간 산출용 구역진입 판정에 사용(게이트는 여전히
	//   과금 유효성(charge_yn/status) 판정에만 씀). 매칭 링크 ID로 역인덱스 조회 (2026-08-25 최정우 추가)
	void GetOpenZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut);
	// 폴리곤이 겹쳐 설정될 수 있어 포함하는 구역을 전부 돌려준다 (2026-08-23 최정우 추가)
	void GetParkingZonesContaining(const double dfLon, const double dfLat, const double dfPadM,
		vector<PZONE_INFO> *pvtOut);

	// NODE_STEP 일반도로 등록 확장(케이스1~3) 스코프 판정 — 매칭 링크가 "어떤 과금유형에도 등록
	//   안 된 도로"(케이스2)인지, SKIP 구간 브릿지(케이스3) 대상 road_kind인지 판정용 (2026-09-01 최정우 추가)
	// 링크가 어떤 과금유형(ROAD_KIND 0/1/2/3/5)에도 등록돼 있는지 — PARKING(4)은 폴리곤 기반이라
	//   링크ID 매핑 개념이 없어 제외. false=케이스2(미등록 도로) 대상
	bool IsLinkChargeRegistered(const uint64 qwLinkID);
	// 케이스3(SKIP 구간) 전후 확정링크 스코프 판정 — road_kind=0(일반도로) 또는 3(구간단속) 소속,
	//   또는 어디에도 미등록. true여야 그 SKIP 구간을 NODE_STEP으로 등록 대상 삼음(질문2 확정 답변)
	bool IsCase3EligibleRoadKind(const uint64 qwLinkID);
	// 링크가 "게이트형 구역(1 개방식·2 폐쇄식·3 구간단속)에만" 등록돼 있는지 — 일반도로(0)·면제(5)
	//   역인덱스 어디에도 없는 등록 링크. true 면 그 구역 run 이 열려 있지 않은 동안(게이트 미통과·
	//   진출 후 잔여 tick)에는 일반도로(0)로 계상해야 한다 (2026-09-07 최정우 추가, 사용자 지시)
	bool IsLinkGateZoneOnly(const uint64 qwLinkID);
	// 링크가 속한 폐쇄식 구역(ROAD_KIND=2)의 road_id — 입구게이트 미통과로
	//   일반도로(0)로 계상되는 구간에 "어느 구역을 게이트 없이 지났는가"를 남기기 위한 조회.
	//   여러 구역에 속하면 첫 항목만 돌려준다(실데이터상 폐쇄식 구역끼리 링크를 공유하지 않음).
	//   true=찾음(pszOut 채움), false=폐쇄식 구역 소속이 아님(pszOut 은 빈 문자열)
	//   (2026-09-23 최정우 추가, 사용자 확정)
	bool GetClosedZoneRoadIdByLinkId(const uint64 qwLinkID, char *pszOut, const size_t nOutSize);
	// 링크가 속한 구간단속 구역(ROAD_KIND=3)의 road_id — 위 폐쇄식판과 같은 목적(진입게이트를
	//   거치지 않고 구역 중간으로 진입한 경우의 진입 판정). 이쪽은 m_mapSpeedLinkToRoadId 가
	//   이미 있어 그것을 그대로 쓴다 (2026-09-23 최정우 추가, 사용자 지시)
	bool GetSpeedZoneRoadIdByLinkId(const uint64 qwLinkID, char *pszOut, const size_t nOutSize);

	// 폴리곤 기하 유틸 — 원래 .cpp 파일 내부 static 함수였으나, 주정차 경계 통과 시각 보간
	//   (CRawLogWorker::InterpolateZoneCrossingTime)에서도 같은 판정을 써야 해서 공개 static
	//   메서드로 승격 (2026-08-24 최정우 추가). 순수 함수 — 인스턴스 상태 없음
	static bool IsPointInPolygon(double dfLon, double dfLat, const vector<POINT>& vtPoly);
	static double DistanceToPolygonBoundaryMeters(double dfLon, double dfLat, const vector<POINT>& vtPoly);

	inline bool IsLoad() const { return m_bLoad; }
	size_t GetGateCount() const;
	inline size_t GetZoneCount() const
	{
		lock_guard<CMutex> cLock(m_cZoneCacheMutex);
		return m_mapZoneInfo.size();
	}
	// 0=임계 비활성(테이블 비어있거나 미로드 — 항상 등록) (2026-08-24 최정우 추가)
	inline int GetParkFineMinSec() const
	{
		lock_guard<CMutex> cLock(m_cParkFineMutex);
		return m_nParkFineMinSec;
	}

private:
	CPostgrePool					*m_pcPostgrePool;						// 커넥션 풀(비소유 — Server 가 생성한 것 공유)
	string							m_strGateSelectSQL;						// base_tollgate 전량 조회 SQL(query.sql 세션 로드 결과)
	string							m_strZoneSelectSQL;						// base_roadlink 전량 조회 SQL (2026-08-12 최정우 추가)
	string							m_strParkFineSelectSQL;					// base_parking_fine 최소 from_min 조회 SQL, 비어 있으면 임계 비활성 (2026-08-24 최정우 추가)
	mapGateInfo						m_mapGateInfo;							// link_id → 게이트 정보
	mapZoneInfo						m_mapZoneInfo;							// road_id → 구역 정보 (2026-08-12 최정우 추가)
	// 재조회로 교체된 이전 세대 — 댕글링 포인터 방지용 최근 N세대 보관(2026-08-14 최정우 추가).
	//   Get*() 조회 함수들은 락 "안에서" 얻은 PGATE_INFO/PZONE_INFO 포인터를 호출측이 락 해제 "후"에
	//   짧게 참조하는 게 기존 설계인데, 재조회가 std::swap 으로 이전 세대를 그 즉시 파괴해버리면
	//   그 찰나에 재조회가 겹칠 때 use-after-free 위험이 있었음("소스상 문제" 검토 중 발견) — 이전
	//   세대를 곧바로 버리지 않고 최근 RETIRED_CACHE_GENERATIONS 개만 계속 살려둬서 방지. std::deque
	//   는 양끝 삽입/삭제 시 다른 원소(unordered_map 자체 및 그 안의 노드) 주소를 무효화하지 않음이
	//   보장되므로 안전
	deque<mapGateInfo>				m_dqRetiredGateInfo;
	deque<mapZoneInfo>				m_dqRetiredZoneInfo;
	// 1:N — 같은 유형의 구역이 한 링크를 공유할 수 있다(인접 구역의 경계 링크 등). 예전엔 1:1 이라
	//   나중에 적재된 구역이 앞의 것을 조용히 덮어썼다 (2026-08-23 최정우 수정)
	unordered_map<uint64, vector<string> >	m_mapNodeStepLinkToRoadId;		// 일반도로(ROAD_KIND=0) link_id → road_id 역인덱스,
																			//   LoadZones() 가 link_ids 파싱 후 구성 (2026-08-14 최정우 추가)
	unordered_map<uint64, vector<string> >	m_mapExemptLinkToRoadId;		// 면제도로(ROAD_KIND=5) link_id → road_id 역인덱스,
																			//   LoadZones() 가 link_ids 파싱 후 구성 (2026-08-14 최정우 부활)
	unordered_map<uint64, vector<string> >	m_mapOpenLinkToRoadId;			// 개방형(ROAD_KIND=1) link_id → road_id 역인덱스,
																			//   LoadZones() 가 link_ids 파싱 후 구성 (2026-08-25 최정우 추가)
	unordered_map<uint64, vector<string> >	m_mapSpeedLinkToRoadId;			// 구간단속(ROAD_KIND=3) link_id → road_id 역인덱스 — 기존엔
																			//   게이트 기반이라 불필요했으나, IsCase3EligibleRoadKind() 의
																			//   "이 링크가 구간단속 구역 소속인가" O(1) 판정에 필요해 추가
																			//   (2026-09-01 최정우 추가)
	unordered_map<uint64, vector<string> >	m_mapClosedZoneLinkToRoadId;	// 폐쇄식(ROAD_KIND=2) link_id → road_id 역인덱스 — 폐쇄식은
																			//   게이트 기반이라 종전에 전용 역인덱스가 없었다. 입구게이트를
																			//   통과하지 않은 채 그 구역 링크를 지나는 구간에 "어느
																			//   구역이었나"를 붙이는 GetClosedZoneRoadIdByLinkId() 전용.
																			//   **다른 게이트형 둘은 일부러 뺐다**(2026-09-23 실측 근거):
																			//   · 구간단속(3) — 위반 시 같은 구간을 일반도로 행으로 함께
																			//     등록하는 미러 정책이 이미 있어(미러는 SPEED 처리기가 따로
																			//     만든다) 여기서 또 끊으면 미러와 tick 범위가 어긋나 같은
																			//     유형 구간중복이 8건 생겼다.
																			//   · 개방식(1) — 게이트를 안 지나도 구역 진입만으로 OPEN 행이
																			//     생기고 게이트 미통과는 charge_yn/status(N/3)로 표현한다.
																			//     그래서 일반도로로 흡수되는 구간이 원래 경계 1 tick 뿐이고,
																			//     쪼개면 0~19m 짜리 행 4건과 경계 중복만 남았다.
																			//   폐쇄식만 다르다 — 게이트 통과가 run 개시의 **유일한** 조건
																			//   이라, 게이트 없이 지나면 구간이 통째로 일반도로에 묻힌다
																			//   (실측 최대 3,380m). (2026-09-23 최정우 추가, 사용자 확정)
	unordered_set<uint64>			m_setAllRegisteredLinkIds;				// ROAD_KIND 0/1/2/3/5 link_ids 전체 합집합 — "어떤 과금유형에도
																			//   등록 안 된 도로"(NODE_STEP 케이스2) 판정용. PARKING(4)은
																			//   폴리곤 기반이라 제외 (2026-09-01 최정우 추가)
	bool							m_bLoad;
	mutable CMutex					m_cGateCacheMutex;						// m_mapGateInfo 재조회(swap)/조회 동시접근 보호 (2026-08-14 최정우 추가)
	mutable CMutex					m_cZoneCacheMutex;						// m_mapZoneInfo+m_mapNodeStepLinkToRoadId+m_mapExemptLinkToRoadId+
																			//   m_mapOpenLinkToRoadId+m_mapSpeedLinkToRoadId+
																			//   m_mapClosedZoneLinkToRoadId+m_setAllRegisteredLinkIds
																			//   재조회(swap)/조회 동시접근 보호 — 전부 LoadZones() 에서 항상 같이
																			//   swap 되므로 락도 하나로 공유(항상 일관된 스냅샷 보장) (2026-08-14 최정우 추가,
																			//   2026-09-01 최정우 수정 — 신규 멤버 2개 추가)
	int								m_nParkFineMinSec;						// base_parking_fine 최소 from_min(분)*60 — 0=임계 비활성 (2026-08-24 최정우 추가)
	mutable CMutex					m_cParkFineMutex;						// m_nParkFineMinSec 재조회/조회 동시접근 보호 (2026-08-24 최정우 추가)
};

#endif //__CHARGEDATALOADER_H__

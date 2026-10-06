/**
 * @file DataDefine.h
 * @brief 각 데이터 정의 헤더 파일
*/
#ifndef __DATADEFINE_H__
#define __DATADEFINE_H__

#include <string.h>
#include "TypeDefine.h"

#define UNUSE_LOG_KEEP				-1									// 로그 관리 사용하지 않음
#define MAX_PATH					255									// 경로 최대 길이

// 초기화 코드
#define NO_ANGLE					-1									// 방위각 정보 없음
#define NO_RADIUS					-1									// 검색 반경 입력값 없음
#define NO_SPEED					-1									// 제한속도 입력값없음
#define NO_ACCURACY					-1									// 수평 오차(ACCURACY_M) 미적용
#define NO_ALTITUDE					-1									// 고도(ALTITUDE_M) 미적용
#define NO_PERIOD					-1									// GPS 입력 주기(초) 없음
																		//   [보완] 현재 참조처 없음 (2026-10-06 최정우 주석 추가)

/**
 * @enum eLinkRoadType
 * @brief ROAD_TYPE — LINK_INFO.nRoadType (uint8, DBF 숫자 그대로)
 * @remark MOCT_LINK.ROAD_TYPE: 0=일반, 1=고가, 2=지하, 3=교량, 4=터널 (국토교통부고시
 *   제2023-22호 공식 코드값 — 2026-08-27 최정우 수정. 기존 enum은 001=교량/002=터널/
 *   003=고가/004=지하로 정의돼 있었으나 실측(network.moct_link 전국 155만건, 분포
 *   001:1,405건·002:2,459건·003:35,813건·004:6,461건)과 doc/지능형교통체계ITS+표준+
 *   노드링크+구축+및+운영지침.PDF 24쪽 대조 결과 공식값과 반대로 정의돼 있었음이 확인돼
 *   정정함. IsElevatedRoad(){ELEVATED,BRIDGE} 세트 체크라 이 스왑과 무관하게 결과 동일하지만,
 *   IsUndergroundRoad()는 ROAD_TYPE_UNDERGROUND 단독 체크라 정정 후 대상이 터널→지하차도로
 *   바뀜(정정 전 이 심볼의 값이 4=실데이터의 터널이었기 때문. 정정 후 값은 2=지하차도. 의도된
 *   동작 변경 — RoadTypeDirectionPenalty()의 "고도 상승+지하형" 감점 로직이 원래 지하차도
 *   (움푹 파였다 다시 올라오는 구조)를 겨냥한 설계였는데 정정 전엔 터널에 잘못 적용되고 있었음).
 *   [2026-09-17 최정우 정정] 종전 문구 "값 4 단독 체크" 는 **정정 전** 기준이라 지금 코드를 읽는
 *   사람이 현행도 4를 본다고 오해하기 쉬워 심볼 기준으로 고쳐 적음. 상세:
 *   doc/표준노드링크_시설물_도로_코드_분석.html
*/
enum eLinkRoadType : uint8
{
	ROAD_TYPE_NORMAL				= 0,								// 000 일반도로
	ROAD_TYPE_ELEVATED				= 1,								// 001 고가차도
	ROAD_TYPE_UNDERGROUND			= 2,								// 002 지하차도
	ROAD_TYPE_BRIDGE				= 3,								// 003 교량
	ROAD_TYPE_TUNNEL				= 4									// 004 터널
};

/**
 * @enum LinkConnectCode
 * @brief CONNECT — LINK_INFO 의 nConnect (uint8, DBF 숫자 그대로)
 * @remark LINK_INFO 의 nConnect 0=아님, 1=연결로. 101~108=구 링크 등급별 연결로(레거시)
*/
enum eLinkConnectCode : uint8
{
	LINK_CONNECT_NONE				= 0,								// 연결로 아님
	LINK_CONNECT_YES				= 1,								// MOCT 연결로
	LINK_CONNECT_EXPRESS			= 101,								// 고속도로 연결로 (레거시)
	LINK_CONNECT_URBAN				= 102,								// 도시고속도로 연결로 (레거시)
	LINK_CONNECT_NATIONAL			= 103,								// 일반국도 연결로 (레거시)
	LINK_CONNECT_METRO				= 104,								// 특별·광역시도 연결로 (레거시)
	LINK_CONNECT_NATIONAL_LOCAL		= 105,								// 국가지원지방도 연결로 (레거시)
	LINK_CONNECT_LOCAL				= 106,								// 지방도 연결로 (레거시)
	LINK_CONNECT_CITY				= 107,								// 시·군도 연결로 (레거시)
	LINK_CONNECT_ETC				= 108								// 기타도로 연결로 (레거시)
};

/**
 * @enum eLinkRoadRank
 * @brief ROAD_RANK — LINK_INFO 의 nRoadRank (uint8, DBF 숫자 그대로)
 * @remark 101~108=도로 등급. 0=미설정(생성자 초기값, MOCT 컬럼 값 아님)
*/
enum eLinkRoadRank : uint8
{
	LINK_ROAD_RANK_NONE				= 0,								// 미설정 (초기값)
	LINK_ROAD_RANK_EXPRESS			= 101,								// 101 고속도로
	LINK_ROAD_RANK_URBAN			= 102,								// 102 도시고속화도로
	LINK_ROAD_RANK_NATIONAL			= 103,								// 103 일반국도
	LINK_ROAD_RANK_METRO			= 104,								// 104 특별·광역시도
	LINK_ROAD_RANK_NATIONAL_LOCAL	= 105,								// 105 국가지원지방도
	LINK_ROAD_RANK_LOCAL			= 106,								// 106 지방도
	LINK_ROAD_RANK_CITY				= 107,								// 107 시·군도
	LINK_ROAD_RANK_ETC				= 108								// 108 기타
};

/**
 * @enum eLinkNodeType
 * @brief NODE_TYPE — MOCT_NODE.NODE_TYPE → LINK_INFO.nStNodeType/nEdNodeType (uint8)
 * @remark 0=미설정(초기값). 101~107=MOCT 노드 유형 — 국토교통부고시 제2023-22호 공식
 *   코드값(2026-08-27 최정우 수정). 기존 enum은 102=JC/103=SA/104=IC/105=TG/106=기타/
 *   107=시군도노드등으로 정의돼 있었으나 실측(network.moct_node 전국 1,178,457건,
 *   분포 101:885,309·102:42,566·103:22,057·104:71,692·105:4,329·106:12,106·
 *   107:140,398건 — 공식코드 108/109는 실측 0건)과 대조해 정정. 이 필드는 현재 어떤
 *   조건 분기에도 안 쓰여(저장만 됨) 기능 영향은 없음. 상세: doc/표준노드링크_시설물_
 *   도로_코드_분석.html
*/
enum eLinkNodeType : uint8
{
	NODE_TYPE_NONE					= 0,								// 미설정 (초기값)
	NODE_TYPE_CROSSROAD				= 101,								// 교차로 (도로교차점)
	NODE_TYPE_ROAD_END				= 102,								// 도로 시·종점
	NODE_TYPE_ATTR_CHANGE			= 103,								// 속성변화점
	NODE_TYPE_FACILITY				= 104,								// 도로시설물 (교량·터널·고가·지하차도 등)
	NODE_TYPE_ADMIN_BOUNDARY			= 105,								// 행정경계
	NODE_TYPE_RAMP_CONNECT			= 106,								// 연결로접속부 (IC·JC 등)
	NODE_TYPE_MIN_SPACING			= 107								// 최소노드배치점
};

/**
 * @struct sAltitudeScoreConfig
 * @brief 연속 맵매칭 고도(ALTITUDE_M) 보조 점수 설정 — config [mapmatch] alt_gap/alt_penalty/alt_weight/alt_slope
 * @remark Begin 맵매칭 미적용. alt_weight=0 이면 전체 비활성.
 *
 * 고도 보조 비용(음수=유리·양수=불리)은 기존 dfCost(INTERSECT_LEN+방위각)에 가산.
 *   |Δalt| ≤ alt_gap:
 *     · 같은 ROAD_TYPE  → −alt_penalty (alt_penalty 가 양수인 설정 기준 — 부호는 그대로 뒤집어 적용)
 *     · 호환 ROAD_TYPE  → 0  (고가↔교량)
 *       [보완] 2026-08-28 부터 일반↔교량도 호환(IsRoadTypeCompatible) (2026-10-06 최정우 주석 추가)
 *     · 불일치          → +alt_penalty
 *   |Δalt| > alt_gap:
 *     · alt_weight × (|Δalt| − alt_gap) + 방향 패널티
 *     · 상승(Δ>차이) 시 지하 후보 +alt_penalty / 하강(Δ<−차이) 시 고가·교량 후보 +alt_penalty
 *   |Δalt|/수평거리 > alt_slope → 고도 신호 무시(0)
 *
 * 예) 차이=8, alt_penalty=10, 가중치=0.5 / 직전·현재 100m·106m, 같은 고가
 *     → Δ=6 ≤ 8 → 고도 보조 −10m (후보 우선)
*/
typedef struct sAltitudeScoreConfig
{
	sint16							nGap;								// config alt_gap — 직전 매칭 고도와 허용 차이(m)
	sint16							nAltPenalty;						// config alt_penalty — 양수=ROAD_TYPE 불일치 비용 가산·음수=같은 ROAD_TYPE 비용 감산(m)
	double							dfWeight;							// config alt_weight — 차이 초과 시 고도차 가중. 0=비활성
	double							dfSlope;							// config alt_slope — |Δ고도|/수평거리 상한. 초과 시 고도 무시

	sAltitudeScoreConfig() :
		nGap(8),
		nAltPenalty(10),
		dfWeight(0.5),
		dfSlope(0.12)
	{}
} ALTITUDE_SCORE_CONFIG, *PALTITUDE_SCORE_CONFIG;

// 에러 코드
#define NO_ERROR					0									// 오류 없음
#define INVALID_COORDTYPE			1									// 측지계 코드 오류
#define INVALID_COORDINATE			2									// 좌표 오류
#define INVALID_ANGLE				3									// 방위각 오류
#define INVALID_ROADTYPE			4									// 도로등급 오류
#define INVALID_SEARCHRADIUS		5									// 검색 반경 오류
#define INVALID_LINKID				6									// LinkID 오류
#define INVALID_RESULTCOUNT			7									// 응답 개수 오류
#define INVALID_SEARCHSTEP			8									// 탐색 단계 오류
#define NOT_FOUND_GRIDINFO			9									// GRID 정보 검색 실패
#define NOT_FOUND_LINKID			10									// LinkID 검색 실패
#define MAP_MATCH_FAIL				11									// 맵매칭 실패
#define NOT_LOADED_MAPDATA			12									// 도로망(link.psf) 미로드 — 로드 실패·재로드 중
																		//   (2026-09-15 최정우 추가. 새 코드 추가 시
																		//    MapMatch.cpp 의 ErrorCodeTable 에도 반드시 등록할 것)

// 맵매칭 소프트 비용(방위각 가중) 파라미터 — ±45° 하드컷 대체 (2026-07-08 최정우 추가)
#define MM_DIR_WEIGHT				1.0									// 방위각 1도당 비용(m 환산) 가중치(w_a). 거리(m)+w_a·|방위각차|
// ── BEGIN 왕복분리 짝 링크 heading 교정 (2026-08-22 최정우 추가) ──
//   BEGIN 매칭은 heading 을 무시하고 거리만으로 판정한다(2026-08-19). 그런데 왕복분리
//   도로는 짝 링크가 10m 남짓 옆에 나란히 있어 거리로는 사실상 동전 던지기이고, 방향만이
//   유일한 판별 근거다. 실측(trip 000376_20260819094414 seq1): 정답 링크가 7.68m, 반대방향
//   링크가 4.16m 라 반대방향이 채택됐다 — heading 276° vs 채택 링크 방위각 101°(175° 어긋남).
//   그래서 "채택 링크가 heading 과 거의 정반대이고 그 짝 링크는 heading 과 잘 맞을 때"만
//   짝 링크로 교정한다. 둘 다 애매하면 건드리지 않는다(전국 테스트 21트립에서 2건).
//   정지 중 heading 은 신뢰할 수 없어 속도 하한을 둔다 — 하한 0 이면 교정 76건 중 45건이
//   속도 0~2 구간의 오교정 후보였고, 3km/h 로 자르면 31건만 남는다(BEGIN 4건은 전부 유지).
#define MM_OPP_FIX_MIN_SPEED		3									// (단위: km/h) 짝 링크 교정 적용 최소 속도
#define MM_OPP_FIX_REV_DEG			135									// (단위: 도) 채택 링크가 heading 과 이 이상 어긋나야 교정 검토
#define MM_OPP_FIX_FWD_DEG			45									// (단위: 도) 짝 링크가 heading 과 이 이내여야 교정 확정
#define MM_DIR_MAX_DEG				120									// 방위각 차 하드 상한(초과 후보 배제, 역방향 오매칭 방지)

// BEGIN 후보 목록에 그리드 셀 하나가 올릴 수 있는 최대 후보 수 (2026-09-05 최정우 추가, 사용자 지시)
//   기존엔 GridSgmtMapMatch 가 셀 안에서 정렬 후 1등 하나만 반환해, 후보 목록 크기가 곧 "매칭된
//   그리드 셀 수"였다(최대 9). 그래서 같은 셀 안에 정답 링크가 있어도 더 가까운 링크에 가려
//   목록에 오르지조차 못했다 — 실측 000376_20260821095239 seq1: 역방향 2040424401(2.0m)이 셀
//   1등이라, 8.4m 거리의 정답 2040424301 이 후보 n=1 목록에서 통째로 빠졌다. 반경을 75m 로
//   넓혀도 그건 "다른 셀"의 46.5m·51.4m 를 데려올 뿐 같은 셀 2등은 영영 못 올린다.
//   FixOppositePairByHeading·FixReverseLinkByAzimuth·IsAntiHeadingOpposite 는 전부 이 목록
//   안에서 순서만 바꾸는 계층이라, 목록에 없는 링크는 어떤 교정으로도 뽑을 수 없다(그래서 실측
//   발동 0건이었다). 값 3 = 왕복분리 정·역 한 쌍(2) + 교차로·나란한 도로 여유 1.
//   더 키우면 교정 계층이 엉뚱한 후보를 집어올 위험이 같이 커진다.
#define MM_BEGIN_CELL_TOPN			3									// (단위: 건) 그리드 셀당 BEGIN 후보 최대 수

// 방위각 비용의 최대 상쇄 한도(m) — 이보다 더 가까운 후보는 방위각이 아무리 잘 맞아도 역전 불가 (2026-07-18 최정우 추가)
//   목적: dfCost=거리+w_a·|각도차| 합산 시, 각도차가 커도(하드컷 120° 이내) 비용이 무한정 커져 실거리가
//   훨씬 먼 후보가 선택되는 것을 방지. 예) 5m/100°(cap 미적용 시 cost 105) vs 40m/5° 후보에서,
//   cap=15 적용 시 5m 후보 cost=20 으로 40m 후보(cost 45)를 이김 → 근접 우위가 보존됨.
#define MM_DIR_MAX_PENALTY		15.0								// (단위: m) 방위각 비용 상한
#define MM_SPEED_LOW_KMH			5									// 이하: 저속 → 방위각 불신(w_a=0)
#define MM_SPEED_HIGH_KMH			20									// 이상: 방위각 가중치 최대(w_a=MM_DIR_WEIGHT)

// HEADING/SPEED 가 DB NULL 일 때 직전 매칭좌표로 계산하기 위한 임계 (2026-07-08 최정우 추가)
#define MM_CALC_MAX_GAP_SEC			10									// 직전 매칭점과의 시간간격(초) 초과 시 계산 불신 → 미적용
#define MM_CALC_MIN_DIST			2.0									// (단위: m) 이동거리 미만이면 방위각 노이즈 → 방위각 계산 미적용

// 정식 매칭 실패(반경 밖)·정확도 SKIP 시 최근접 세그먼트 진단 탐색 반경(m) (2026-07-10 최정우 추가)
//   좌표·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)를 참고용으로 남기기 위한 최대 탐색 반경.
// [2026-09-21 최정우 추가 — 값을 바꾸기 전에 반드시 읽을 것]
//   이 상수는 "진단 탐색 반경" 하나만이 아니라 **검색 반경의 전역 상한**도 겸한다. 값을 바꾸면
//   아래 두 곳이 함께 움직인다(종전에는 두 곳이 리터럴 250 으로 박혀 있어 이 값과 끊겨 있었고,
//   2026-09-21 에 이 상수를 참조하도록 묶었다):
//     · CMapMatch::IsValidSearchRadius() — 요청 반경 상한. BeginGeomNearest() 가 nRadius 에
//       이 값을 그대로 넣고 그 검사를 통과해야 하므로, 검사 상한이 이 값보다 작으면 진단
//       최근접 경로가 **전부 INVALID_SEARCHRADIUS 로 조용히 실패**한다(SKIP 행의 MATCH_LAT/LON·
//       INTERSECT_LEN 이 통째로 빈다 — 에러 로그도 거의 안 남아 알아채기 어렵다).
//     · AppMain.cpp 의 [mapmatch] radius 범위 검증 — 이 값을 넘는 설정은 기본값으로 되돌린다.
//   즉 올릴 때는 세 곳이 자동으로 함께 올라가지만, **내릴 때는 config 의 radius/radius_max 가
//   새 상한을 넘지 않는지 먼저 확인**해야 한다.
#define MM_DIAG_RADIUS				250									// (단위: m) 반경 밖 최근접 후보 탐색 반경(진단용, 방위각 무시) 겸 검색반경 상한

// bReverseSuspect(연속역행 스트릭용 신호) 판정 시, 이보다 작은 위치 후퇴는 부동소수점 오차로
//   보고 무시 — margin 과 무관하게 항상 적용되는 최소 임계 (2026-07-21 최정우 추가)
#define MM_REVERSE_SUSPECT_EPS		0.1									// (단위: m) 역행 의심 판정 최소 후퇴거리

// 연속 실패 후 Begin 재검색 시, 직전 성공 링크와 "연결(회전 가능)되지 않은" 후보에 주는 비용 페널티(m 상당) (2026-07-15 최정우 추가)
//   목적: 회전·수렴 구간에서 나란한 도로로 튀는 오매칭 억제. 소프트 페널티라 명백히 더 가까운 도로는 그대로 선택됨.
#define MM_CONNECT_PENALTY			30.0								// (단위: m) 직전 성공 링크와 미연결 후보 cost 가산

// 연속 맵매칭 depth0 최적 후보가 링크 경계(시작/끝)에 스냅(클램프)됐는지 판정 허용오차(m) (2026-07-15 최정우 추가)
//   경계 클램프면 차량이 링크 끝을 지난 것 → 연결 다음 링크에 더 나은 내부 수선발이 있을 수 있어 depth 확장
#define MM_CLAMP_EPS				1.0									// (단위: m) 링크 경계 클램프 판정 허용오차

// 최종 확정 후보가 여전히 경계 클램프(depth 확장해도 더 나은 내부 수선발을 못 찾음) 이면서,
//   GPS↔매칭점 거리(INTERSECT_LEN)가 이 값을 넘으면 "신뢰도 낮은 매칭"으로 SKIP 처리 — 여러 GPS_SEQ가
//   같은 꺾임점으로 뭉개져(클램프) MATCH_LAT/LON 이 정지한 것처럼 보이는 오탐(예: 주정차 오판) 방지용
//   (2026-07-21 최정우 추가 — 클램프 저신뢰 SKIP)
#define MM_CLAMP_SKIP_LEN			10.0								// (단위: m) 클램프+거리 초과 시 SKIP 판정 임계

// 클램프(꺾임점 스냅)됐어도 heading이 도로 방향과 잘 맞으면(MM_CLAMP_HEADING_MAX_DIFF 이내) 위
//   MM_CLAMP_SKIP_LEN 을 넘는 거리까지도 신뢰(SKIP 강등 취소) — 단 MM_CLAMP_HEADING_SKIP_LEN 은
//   넘지 않아야 함(이중 안전장치). 속도 조건은 별도 상수 없이 기존 MM_SPEED_HIGH_KMH(heading
//   가중치가 이미 최대가 되는 속도) 재사용 — 저속에서는 heading 자체가 노이즈라 이 구제를 적용 안 함
//   (2026-08-20 최정우 추가 — 실측: 저신뢰 클램프 SKIP 9건 중 5건이 이 조건으로 정상 구제됨)
#define MM_CLAMP_HEADING_MAX_DIFF	30									// (단위: 도) 클램프 신뢰 구제 인정 heading-도로방향 각도차 상한
#define MM_CLAMP_HEADING_SKIP_LEN	20.0								// (단위: m) heading 일치해도 이 거리를 넘으면 계속 SKIP

// 연속 맵매칭 그래프 확장(TURNINFO)이 막다른 링크(다음 연결 없음)에서 끊길 때, 지리적으로 아주
//   가까운(반경 이내) 다른 링크의 시작점을 런타임에 찾아 그래프 확장을 이어주는 우회용 임계값(m).
//   원본 지도 데이터에서 같은 물리적 교차로가 노드ID 미세 오차(수 m)로 서로 다른 노드로 분리돼
//   TURNINFO 연결이 안 잡힌 경우를 보완 — 지도 바이너리(link.psf)는 그대로 두고 매칭 엔진에서만
//   보강(2026-08-20 최정우 추가, 사용자 지시 — 실측: node 2040155702/2040155703 간 ~9m 갭이
//   여러 트립에서 반복적으로 매칭 불안정을 유발하는 것 확인)
#define MM_NODE_BRIDGE_MAX_M		15.0								// (단위: m) 막다른 링크 끝점↔다른 링크 시작점 브릿지 인정 거리

// BridgeNearbyLinkStarts 로 찾은 후보(TURNINFO 아님, 좌표 근접만으로 발견)에 주는 비용 페널티.
//   위 브릿지 자체는 유지(9m 갭처럼 대체 후보가 없는 경우엔 그대로 채택됨)하되, 진짜 TURNINFO로
//   연결된 후보가 같은 경합에 있을 때는 그쪽이 근소한 차이로도 우선하도록 위상 우선순위를 준다.
//   조밀한 IC/램프 구간에서 서로 무관한(반대방향 등) 노드가 우연히 근접해, 순수 거리비용만으로
//   엉뚱한 링크가 근소하게(예: 0.4) 이기던 사례 확인(trip 000376_20260819094414 seq54) (2026-08-21 최정우 추가)
#define MM_GEOM_BRIDGE_PENALTY		5.0									// (단위: 비용 가중치) 지리적 브릿지 후보 전용 페널티

// 같은 링크 위에서 "뒤로 가지 않고 정상 전진"인데도 GPS↔매칭점 거리(INTERSECT_LEN)가 이 값을
//   넘으면, 같은 도로명의 인접 링크를 추가로 탐색한다(TryNearbyRoadNameCandidate) — 세션이 한번
//   앵커링되면 hop penalty·연속성 때문에 물리적으로 더 가까운 평행 링크(왕복분리 등)가 있어도
//   계속 같은 링크에 붙어있는 사각지대 대응. 정차/저속(MM_SPEED_LOW_KMH 이하)은 제외 —
//   BridgeNearbyLinkStarts 를 턴 유무 무관하게 넓혔다가 정차 중 후보가 매 tick 흔들리는 회귀가
//   났던 전례(위 MM_NODE_BRIDGE_MAX_M 주석 인접 대목) 재발 방지. p50=4m·p75=8m(전체 MATCHED
//   실측 6,861건 분포) 사이로 잡아, 상위 25%에서만 추가 탐색이 돌게 함(2026-08-26 최정우 추가,
//   실측 900376_20260826160622 등 25개 트립·878개 포인트 근거)
#define MM_ROADNAME_SEARCH_MIN_M	7.0									// (단위: m) 인접 도로명 후보 탐색 트리거 최소 오프셋

// 1틱 지연커밋 보류 행이 SKIP/ERROR 틱을 몇 개까지 건너뛰며 "다음 정상 매칭"을 기다릴지 상한.
//   기존엔 다음 틱이 SKIP이면 곧바로(보정판단 없이) 확정해버려, 그 SKIP 한 틱 때문에 경로
//   일관성 보정 기회를 놓치는 사각지대가 있었다(실측 000376_20260826152113 M260 — 위상
//   미연결 링크에 붙었는데 바로 다음 M261도 SKIP이라 M262 까지 못 보고 확정됨). reverse_confirm(3)·
//   opp_streakmax(3 기본) 등 기존 유사 상한과 같은 자릿수로 5 잡음 — 트립이 SKIP만 계속되다
//   끝나는 경우는 무한 보류 방지용 상한(2026-08-26 최정우 추가)
#define MM_PENDING_MAX_HOLD_TICKS	5									// (단위: 틱 수) 보류 연장 최대 허용

// 같은 링크 위 역행이 "확실한 노이즈"(heading 정방향 확인 + 오차 작음 + 후보 1개)로 판정되면,
//   매칭 좌표가 뒤로 밀린 것처럼 보이지 않게 직전 위치보다 이 거리(m)만큼 앞으로 보정한다.
//   링크 끝(END 노드)을 넘어서면 END 노드 좌표로 클램프 (2026-07-22 최정우 추가)
#define MM_NOISE_FORWARD_NUDGE_M	1.0									// (단위: m) 확실한 노이즈 보정 시 전진량
// 노이즈 보정 결과 온전성 검사 — 보정은 "마지막 신뢰 좌표에서 1m 전진"이므로 결과는 반드시
//   현재 GPS 좌표 근처여야 한다. 이보다 멀면 기준점(dfPrevMatchX/Y)이 유효하지 않았다는 뜻이므로
//   보정을 포기하고 계산값을 그대로 쓴다.
//   실측 근거(2026-08-23): ignore_rawvld=1 실험에서 트립 앞부분이 전부 SKIP 이라 기준점이 0 인 채
//   보정이 돌아 매칭 좌표가 (0,0) 으로 찍혔다(000370_20260819093236 seq26·29·31·33,
//   intersect_len 13,205,101m = 13,200km, 그런데 MATCH_STATUS 는 MATCHED). 정상 보정은 결과가
//   GPS 에서 수 m 이내라 이 검사에 걸릴 수 없다 — 원인(앵커 갱신 조건 불일치)을 건드리지 않고
//   결과만 막는 안전장치다
#define MM_NOISE_FIX_SANITY_M		100.0								// (단위: m) 보정 결과와 GPS 좌표의 허용 거리
// 원시좌표가 직전 tick과 완전히 "비트 단위 동일"할 때만 정지로 인정하던 기존 기준(bSameRawAndHeadingAsPrev,
//   2026-09-02 추가)은 실제 GPS 가 정차 중에도 수신기 자체 지터로 매 tick 미세하게(수 m) 흔들리는
//   경우를 못 잡아, 그 구간에서 노이즈 전진보정이 매 tick 누적돼 실측 최대 20m 드리프트가 확인됐다
//   (2026-09-11 사용자 지시 — 재매칭 검증으로 확인, MM_NOISE_FORWARD_NUDGE_M 단위버그 수정과 반드시
//   같이 적용할 것. 단위버그만 고치면 전진량이 ~3.2배 커져 이 드리프트가 더 심해진다). 원시좌표 이동
//   거리가 이 반경(m) 이내면 저속(MM_SPEED_LOW_KMH 이하)일 때 "사실상 정지"로 보고 강제전진을 생략한다.
#define MM_NOISE_SAME_POS_TOL_M	2.0									// (단위: m) 저속 시 "사실상 동일 위치"로 볼 반경
// 내부 좌표 스케일 — 경위도(도) x 360000. 위도 1도 약 111,320m 이므로 1m 는 약 3.2339 단위
#define MM_COORD_UNITS_PER_M		3.2339

// (D) 장시간 공백 시 세션 앵커 폐기 → 초기(Begin) 재획득 임계 (2026-07-15 최정우 추가)
//   직전 "매칭 성공" 이후 경과 시간이 이 값을 넘으면 연속성 신뢰 불가 → 세션 리셋
#define MM_SESSION_RESET_GAP_SEC	30									// 직전 매칭 후 gap(초) 초과 시 세션 리셋

// (B) 연속 맵매칭 공백 적응: 직전 매칭점→현재 이동거리 클수록 탐색 depth(maxstep) 확대 (2026-07-15 최정우 추가)
//   차량이 여러 링크 전진했을 수 있어, 이동거리 MM_STEP_EXTEND_DIST 마다 depth +1 (최대 +MM_STEP_EXTEND_MAX)
#define MM_STEP_EXTEND_DIST			50.0								// (단위: m) depth +1 당 이동거리
#define MM_STEP_EXTEND_MAX			3									// 공백 적응 depth 최대 추가량

// (C) hop 비용: 직전 링크에서 depth(hop) 를 더 건너뛴 후보일수록 비용을 더한다 (2026-08-22 최정우 추가)
//   기존엔 후보를 거리+방위각으로만 겨뤄서, 1 hop 5.2m 후보보다 3 hop 2.3m 후보가 이겼다.
//   실측(연속 3점 42건) 결과 19%(8건)에서 "B 를 거치면 hop 이 3 이상 늘어나는" 우회 매칭이
//   나왔고, maxstep 을 2→4 로 올리면서 탐색 범위가 넓어져 이 현상이 커졌다.
//   hop 1개당 이 값(m)만큼 가산해, 멀리 돌아가는 후보는 그만큼 더 가까워야 이기도록 한다.
//   교차로 분할 링크가 9~12m 인 점을 감안해 8m 로 잡았다 — 정상적인 1 hop 전진은 거의 영향이
//   없고, 3 hop 우회는 24m 를 더 물어 사실상 배제된다.
#define MM_HOP_LEN_RATIO			0.0									// hop 벌점의 링크길이 비례 상한 배율. 0=비활성(현행) (2026-08-23 최정우 추가)
#define MM_HOP_PENALTY				8.0								// (단위: m) depth 1단계당 가산 비용

// (D) 경유경로(aqwPathLinkIDs) 그럴듯함 검증 — 재구성된 경로의 링크 길이 합이 실제 이동거리
//   (dfHorizMove, 직전 확정 매칭 좌표→이번 GPS 좌표 하버사인)보다 비정상적으로 크면 그 경로는
//   버린다(aqwPathLinkIDs 를 채우지 않음 — 각 과금 함수가 최종 확정 링크 1개만 보는 기존 안전
//   기본값으로 자연히 폴백). 그래프 탐색은 "두 확정 링크 사이의 어떤 연결 경로"만 찾을 뿐 실제로
//   그 경로를 달렸는지 검증하지 않아, 복잡한 교차로에서 차량이 실제로 가지 않은 갈림길을 경유
//   링크로 잘못 포함시킬 수 있다(실측 000370_20260824103601 seq5→7, 정지에 가까운 상태(0~5km/h,
//   3초 간격)인데 재구성 경로가 2040425701(93.8m)+2040425301(244.8m)만으로도 338m — 그 시간에
//   물리적으로 이동 불가능한 거리. 결과적으로 그 경로에 우연히 걸린 등록 구역(RL-Z00002)이
//   차량이 실제로 가지 않았는데도 일반도로 과금으로 오등록됨). 직선거리 대비 배율(도로 굴곡
//   감안)+바닥값(짧은 게이트 링크 보정 등 기존 목적은 살림)으로 판정한다 (2026-08-24 최정우 추가)
#define MM_PATH_PLAUSIBLE_SCALE		3.0								// dfHorizMove 대비 허용 배율(도로 굴곡 여유)
#define MM_PATH_PLAUSIBLE_FLOOR_M	10.0								// (단위: m) 최소 허용 경로길이(정지 상태 GPS 튐 여유)

// (D-1) 재구성 경로(위 MM_PATH_PLAUSIBLE_*)와 별개로, "직전 확정 매칭 GPS 시각→이번 GPS 시각"
//   경과초(dfGapSec) 대비 절대 물리속도로 최종 후보 자체의 신뢰도를 판정한다. 위 배율식은
//   dfHorizMove(직전 확정 위치→raw GPS 직선거리) 기준이라, 그 값 자체가 커지면(저신뢰 매칭이
//   앵커로 잡힌 경우 등) 허용폭도 비례해서 커져 정작 걸러야 할 이상치를 놓친다(실측
//   000376_20260826150010 seq405: horizMove=40m 기준 허용 120m인데 재구성 경로는 481.2m로
//   3배 넘게 초과 — 3초 만에 시속 577km — 그런데도 위 검증은 "경로 기록만 버리고 최종 링크는
//   그대로 MATCHED" 로 끝나 잡지 못했다). 반대로 배율식을 강제 SKIP 으로 승격시켜봤더니(2026-09-04
//   1차 시도) 정상 굴곡도로 케이스(예: pathLen 51.6m/horizMove 16.3m, 실제로는 시속 62km 정상
//   주행)까지 대량 오탐(4트립 24,541건 discard, MATCHED 3~5배 급감)으로 되돌렸다.
//   그래서 dfHorizMove 상대 배율이 아니라 dfGapSec 기준 "이 시간에 물리적으로 낼 수 있는
//   속도"라는 절대 상한 하나만 둔다 — 국지도로에서 그 어떤 정상 주행으로도 나올 수 없는
//   명백한 이상치(수백 km/h)만 잡고, 애매한 경계(시속 150~200km 대, 정차구역 저속통과 중
//   반복되던 재구성 경로 포함)는 표본 부족으로 아직 판단 근거가 없어 건드리지 않는다
//   (2026-09-04 최정우 추가)
#define MM_PATH_ABS_MAX_KMH			250.0								// (단위: km/h) dfGapSec 기준 재구성 경로 절대속도 상한

// (D-2) 위 MM_PATH_ABS_MAX_KMH 는 "경로 전체 길이 ÷ 전체 경과시간"의 평균속도만 본다 — 경로
//   구간별로 제한속도가 크게 다르면(예: 좁은 길 하나가 낀 경우) 평균은 상한 이내로 나와도 실제로는
//   그 좁은 구간 하나만으로 불가능한 경우를 못 잡는다(실측 000376_20260826150010 seq132→134:
//   4-hop 경로 중 강변북길 구간(133.4m)의 등록 제한속도가 시속 10km — 그 구간만 제한속도로
//   통과해도 48초가 걸리는데, 전체 경로(345.8m)는 평균 207.5km/h로 절대상한(250) 이내라 위
//   검사를 통과해버렸다). 그래서 구간(hop)별로 등록 제한속도(nMaxSpeed)에 이 배율을 곱한 상한
//   속도로 각 구간의 "최소 소요시간"을 구해 합산하고, 실제 경과시간(dfGapSec)과 비교한다 —
//   합산 최소 소요시간이 dfGapSec 보다 크면(=아무리 서둘러도 이 시간엔 못 지나감) 비현실적.
//   배율(3배)은 정상적인 과속 여유를 감안한 값 — 제한속도 그대로 쓰면 정상 과속 주행까지
//   오탐할 위험이 있어, 위 MM_PATH_PLAUSIBLE_SCALE(도로 굴곡 여유)과 같은 배율을 재사용한다.
//   nMaxSpeed=0(제한속도 미등록)인 구간은 MM_PATH_SPEEDLIMIT_DEFAULT_KMH 로 대체
//   (2026-09-04 최정우 추가, 사용자 지시)
#define MM_PATH_SPEEDLIMIT_MARGIN		3.0									// 등록 제한속도 대비 허용 배율(과속 여유)
#define MM_PATH_SPEEDLIMIT_DEFAULT_KMH	60.0								// (단위: km/h) 제한속도 미등록 구간 대체값

// (E) 폐쇄형/구간단속 SKIP 틱 raw GPS 출구 판정 — 확정매칭 전이로도 못 잡는 잔여 케이스 보완.
//   구역 시작점(ZONE_INFO.dfFirst*) 기준 "raw GPS 까지 거리"가 "출구 게이트까지 거리"보다 이
//   값(m) 이상 크면 이미 게이트를 지나쳐 나간 것으로 본다. 게이트 근처 GPS 튐(신호대기 등)으로
//   오판정하지 않도록 여유를 둔다 — 3초 간격 저~중속 주행에서 흔한 이동거리를 감안 (2026-08-24
//   최정우 추가, 사용자 지시)
#define MM_RAWGPS_EXIT_MARGIN_M		30.0								// (단위: m) 출구 게이트 대비 이 이상 멀어지면 확정

// (F) raw GPS가 등록된 주정차구역 폴리곤 안인데 매칭 좌표는 그 밖으로 나온 경우 — 그래프탐색은
//   전혀 안 건드리고 결과만 사후 비교(ProcessParkingCharge 규칙4와 대칭: 규칙4는 "이동 중엔 매칭
//   좌표가 raw보다 신뢰할 만하다"는 전제이므로 이 필터는 그 반대 — "정지 근처엔 raw가 매칭보다
//   신뢰할 만하다"). 이 상한(km/h) 이하 속도에서만 대상 — 이동 중 정상적으로 폴리곤 경계를
//   가로지르는 도로를 지나는 경우까지 오탐하지 않도록 저속으로 한정한다. 두 가지 폭으로 검증 중:
//   ① 정지 전용(ProcessParkingCharge bLikelyStationary 와 동일 1.0), ② 저속 포함(더 넓은 값) —
//   판교/강릉 실측으로 비교해 결정 (2026-09-04 최정우 추가, 사용자 지시)
// 판교/강릉 실측 결과: ①(1.0, 정지 전용)은 경계가 깨끗하고 국소적(구역 내부 저속 틱만 영향)인
// 반면, ②(5.0, 저속 포함/MM_SPEED_LOW_KMH 재사용)는 구역 진입 직전 구간에서 MATCHED/SKIP이
// 들쭉날쭉 진동하고(seq129~134 예시), 그 여파로 구역과 무관한 먼 지점까지 OPEN 과금 레코드가
// 통째로 재구성/병합되는 부작용(예: 000382 trip에서 5.9km 단일 레코드로 뭉침)이 확인됨.
// → ① 채택 확정 (2026-09-04 최정우 확인)
//
// 이후 속도 게이트 자체를 수선의 발 거리(intersect_len<=30m)로 교체하는 방안도 판교/강릉으로
// 병행 검증했다 — TEMPDBG 계측상 이 폴리곤 불일치는 speed<=1.0 인 69건 외에 speed 2~17km/h 인
// 14건에서도 동일하게 좁은 14~26m 대역(RL-Z00014 하나에만 발생, 다른 구역·정상 통과 도로엔
// 없음)으로 나타나 국소적으로는 seq134 등 추가 포착에 성공했으나, SKIP 구간이 넓어지며
// ResolveSkipGapNodeStep/handoff-gap 등 "SKIP 구간을 사후에 메우는" 로직들이 감당 못 해 이미
// 확정(Y/0, 경로기반 거리)돼 있던 과금 레코드가 감사대상(N/3, 직선거리)으로 다운그레이드되거나
// (000370 실측) 아예 소실되는(000376 실측) 부작용 확인 — 국소적 매칭 개선보다 확정 과금 훼손이
// 더 큰 비용이라 판단해 롤백, ① 유지 확정 (2026-09-04 최정우 확인)
#define MM_ZONE_OUTSIDE_SPEED_MAX_KMH	1.0									// (단위: km/h) 이 값 이하일 때만 검사 대상

// ── NON_CHARGE_REASON 코드 (PRIM_CHARGEHAND.non_charge_reason, smallint) ──
//   CHARGE_YN/CHARGE_STATUS 가 Y/0(정상) 이 아닐 때 그 사유를 남기는 부가 설명 컬럼 — 이 컬럼은
//   과금 판정(CHARGE_YN/STATUS) 자체에는 전혀 영향을 주지 않는다(판정은 그대로, 사유만 기록).
//   도로 유형(charge_type)마다 10개씩 번호 대역을 배정해, 한 유형에 사유가 늘어나도 다른 유형의
//   번호를 안 건드리게 설계했다 — 0(정상)/1~10(NODE_STEP 일반도로)/11~20(OPEN 개방형)/
//   21~30(CLOSED 폐쇄형)/31~40(SPEED 구간단속)/41~50(PARKING 주정차, 현재 고유사유 없음)/
//   51~60(EXEMPT 면제도로)/61~70(공통, TTL·강제종료) — 각 대역 안에 실제 쓰는 값만 정의하고
//   나머지는 예약(값 미정의) (2026-09-01 최정우 추가, 2026-09-11 밤 도로유형별 10단위 대역으로
//   전면 재설계 — [[project_non_charge_reason_code_design_2026_09_10]] 참고, 상세 근거는 메모리에)
#define NCR_NORMAL						0									// 전체 공통 — 정상 과금(Y/0). (2026-09-11 최정우 추가)
																			//   [2026-09-15 최정우 수정, 사용자 지시] DB non_charge_reason
																			//   컬럼에도 이 값이 들어간다 — 종전엔 Y/0 을 빈 값(NULL)로
																			//   두었으나, 코드표에 0 이 정의돼 있는데 DB 에만 안 넣어
																			//   일관성이 없었다. query.sql [charge_insert] 가 빈 문자열을
																			//   0 으로 변환하므로 C++ 쪽은 빈 값으로 두면 된다.
																			//   결과적으로 이 엔진을 거친 INSERT 에는 NULL 이 존재하지 않는다
																			//   [2026-09-17 최정우 보완] 위 문장은 **[charge_insert] 를 타는 경로에 한해서만** 참이었다.
																			//   DB 컬럼에 DEFAULT 도 NOT NULL 도 없어, 수작업 보정·연계 앱·데이터 이관처럼 그 SQL 을
																			//   안 타는 INSERT 가 컬럼을 생략하면 NULL 이 됐다(실측 63행 중 60행 NULL, 그중
																			//   CHARGE_YN='N' 인데 사유 없는 행 18건).
																			//   → doc/deploy_2026-09-17.sql 로 **DEFAULT 0 적용 완료**(2026-09-17, 로컬·실서버 양쪽).
																			//   NOT NULL 승격은 전체 재매칭 뒤 별도로 한다(그 SQL 2절 참고)
#define NCR_NODE_STEP_GAP_ANCHOR_LOST	1									// NODE_STEP SKIP구간 브릿지(케이스3) 시 직전 확정위치 소실
																			//   (세션갭 30초 초과 리셋 등)로 dist_m 은 실측 누적값,
																			//   speed_kmh/stay_seconds 는 산출 근거 없어 0으로 기록
#define NCR_NODE_STEP_GAP_APPROX		2									// NODE_STEP SKIP구간 브릿지(케이스3) — 직전 확정위치는
																			//   유효하나 경로기반 아닌 직선거리로 근사
#define NCR_OPEN_ENTRY_GATE_MISSED		11									// OPEN(개방형) — 트립 중간시작으로 진입게이트 미통과
#define NCR_OPEN_GATE_NOT_ON_PATH		12									// OPEN(개방형) — GPS 간격 사이로 구역을 스쳐 지나 게이트
																			//   통과 미확정. EmitForeignSpan() 이 경로재구성으로만
																			//   복원한 구간 전용 — 트립 시작과 무관한데 11번(트립
																			//   중간시작)으로 잘못 표기되던 것을 분리
																			//   (2026-09-15 최정우 추가)
#define NCR_CLOSED_ENTRY_UNOBSERVED	21									// CLOSED(폐쇄형) — 진입게이트 미확인(트립이 구역 중간에서 시작)
																			//   [보완] 2026-09-23 부터 입구 게이트를 거치지 않은 구역 중간
																			//   진입(교차로·램프)도 이 코드다 (2026-10-06 최정우 주석 추가)
#define NCR_CLOSED_ENTRY_EQUALS_EXIT	22									// CLOSED(폐쇄형) — 입구==출구 동일 게이트(유턴 등)
#define NCR_CLOSED_EXIT_UNCONFIRMED	23									// CLOSED(폐쇄형) — 출구게이트 미확인
#define NCR_SPEED_ENTRY_UNOBSERVED		31									// SPEED(구간단속) — 진입게이트 미확인
#define NCR_SPEED_ENTRY_EQUALS_EXIT	32									// SPEED(구간단속) — 입구==출구 동일 게이트
#define NCR_SPEED_EXIT_UNCONFIRMED		33									// SPEED(구간단속) — 출구게이트 미확인
#define NCR_EXEMPT_TTL_FORCED_CLOSE	51									// EXEMPT(면제도로) — 종료 미확정 강제마감(N/4).
																							//   61 과 동일하게 ①TTL 만료 ②종료신호 이후 잔여 tick
																							//   ③정상 트립종료 시 미확정 구간에 쓴다(아래 61 주석 참고)
#define NCR_EXEMPT_NO_TRIP_END			52									// EXEMPT(면제도로) — 종료신호(TRIP_EVENT=END) 없이 같은
																							//   차량의 다음 운행이 시작돼 강제마감(N/4). 62 의 면제도로판
																							//   (2026-09-16 최정우 추가)
#define NCR_TTL_FORCED_CLOSE			61									// 공통(NODE_STEP/OPEN/CLOSED/PARKING) — 종료 미확정
																			//   강제마감(N/3). (2026-10-04: [trip_abend] SQL 사후전환은 삭제됨)
																			//   [2026-09-15 적용범위 확대] 상수명은 TTL 이지만 실제로는
																			//   FlushOpenRunsAsAbnormalEnd() 등 공용이다 —
																			//   ①TTL 만료·신호두절 ②종료신호 이후 잔여 tick
																			//   ③정상 트립종료 시 미확정 구간. "TTL 이 아닌데 왜 61?" 을
																			//   막으려면 이 주석을 같이 볼 것
																			//   [2026-09-16 분리] 종전에 여기 포함돼 있던 "종료신호 없는
																			//   트립 전환"은 62 로 떼어냈다 — 수집서버 종료신호 이상을
																			//   정산·조사 측에서 식별할 수 있어야 한다는 요구
#define NCR_NO_TRIP_END_FORCED_CLOSE	62									// 공통(NODE_STEP/OPEN/CLOSED/PARKING) — 종료신호
																			//   (TRIP_EVENT=END)를 못 받은 채 같은 차량(DEVICE_KEY)의
																			//   다음 운행이 시작돼, 이전 운행의 열린 구간을 강제마감(N/3).
																			//   세션 키가 DEVICE_KEY 라 TTL 은 이 경우를 못 잡는다 —
																			//   다음 운행이 세션을 그 자리에서 재사용하기 때문.
																			//   마감 주체는 ProcessRawLog() 의 트립전환 분기
																			//   (ResetTripSessionForBegin 직전 호출이 핵심)
																			//   (2026-09-16 최정우 추가)
	//   [2026-09-17 최정우 정정] 61/62 의 적용 유형 목록에서 **SPEED 를 제외**했다 —
	//   AppendExpiredSpeedZoneCharge() 는 2026-09-06 정책 변경 이후 SPEED 행을 만들지 않고
	//   일반도로 미러(Y/0, non_charge_reason=0) 1건만 적재하므로 61/62 가 붙을 행이 없다.
	//   구간단속 고유 사유는 31~33(정상 진출 경로에서만 사용)이다.
	//   [보완] 31~33 은 행에 값을 채우지만 그 SPEED 행 자체가 적재되지 않는다 — 게이트 이상(31·32)은
	//   2026-09-06 적재 조건(양쪽 게이트 확인+위반)에, 출구 미확인(33)은 2026-10-04 "SPEED 행 대신 미러만"
	//   정책에 걸려 DB 에는 구조적으로 나타나지 않고 WARN 로그로만 확인된다 (2026-10-06 최정우 주석 추가)

/**
 * @enum eCoordinateType
 * @brief 측지계 코드
*/
enum eCoordinateType
{
	EPSG3857						= 1,								// 구글지도/빙지도/야후지도/OSM 등에서 사용중인 좌표계
	WGS84GEO,															// 경위도
	KATECH,																// TM128(Transverse Mercator; 횡메카토르) 한국 표준
	BESSELGEO															// GCS Bessel 1841 타원체를 사용
};

/**
 * @struct sMatchEntry
 * @brief 초기 맵매칭 결과
 * @remark
 *	- nRoadRank : LINK_ROAD_RANK_* — MOCT_LINK.ROAD_RANK (101~108)
 *	- nConnect : LINK_CONNECT_* — MOCT 0/1, 구 링크 101~108 (MOCT_LINK.CONNECT → nConnect)
 *	- nRoadType : ROAD_TYPE_* — MOCT_LINK.ROAD_TYPE (0~4)
 *	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sMatchEntry
{
	double							dfMatchX;							// 매핑 X 좌표
	double							dfMatchY;							// 매핑 Y 좌표
	double							dfSgmtMatchLen;						// 세그먼트 시작점부터 교차점까지 거리(m)
	double							dfIntersectLenSgmt;					// GPS 좌표와 세그먼트 교차점까지 거리(m) — DB INTERSECT_LEN
	double							dfCost;								// 소프트 비용 = INTERSECT_LEN(m) + w_a·|방위각차| (링크 선택 기준) (2026-07-08 최정우 추가)
	double							dfAngleCost;						// 방위각 비용(m) — match trace formula용
	double							dfAltAdj;							// 고도 보조 비용(m) — Continue 만, match trace formula용
	bool							bReverseSuspect;					// 위치 역행 + heading 도 역방향 일치 — 연속역행(reverse_confirm) 판정 전용 신호 (2026-07-21 최정우 추가)
	bool							bSgmtClamped;						// 세그먼트 끝점(꺾임점) 스냅 — 클램프 저신뢰 SKIP 판정용 (2026-07-21 최정우 추가)
	bool							bHasHeading;						// heading 값 존재 여부 — 같은 링크 역행 판정 시 노이즈/판단불가 구분용 (2026-07-22 최정우 추가)
	bool							bAmbiguousReverse;					// 같은 링크 역행인데 heading 없음/애매해 노이즈 단정 불가 — SKIP 처리용 (2026-07-22 최정우 추가)
	bool							bClampTrustedByHeading;				// 클램프됐어도 heading이 도로방향과 잘 맞고 고속이면 신뢰 — SKIP 강등 취소용 (2026-08-20 최정우 추가)
	sint16							nDirAngleDiff;						// 주행방향 각도 차이
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점에서 부터 매핑된 세그먼트 시작점까지 거리
	uint8							nMaxSpeed;							// 제한 속도
	double							dfLen;								// 링크 길이
	uint8							nRoadRank;							// 도로 종별[3]
	uint8							nConnect;							// 연결로 코드[3]
	uint8							nRoadType;							// 도로 유형[3]
	uint8							nLanes;								// 차선 정보
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	double							dfStNodeX;							// 시작 노드 X
	double							dfStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성
	uint64							qwEdNodeID;							// 종료 노드 ID
	double							dfEdNodeX;							// 종료 노드 X
	double							dfEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성

	sMatchEntry() : 
		dfMatchX(0.0), 
		dfMatchY(0.0), 
		dfSgmtMatchLen(-1.0), 
		dfIntersectLenSgmt(-1.0), 
		dfCost(-1.0), 
		dfAngleCost(0.0), 
		dfAltAdj(0.0),
		bReverseSuspect(false),
		bSgmtClamped(false),
		bHasHeading(false),
		bAmbiguousReverse(false),
		bClampTrustedByHeading(false),
		nDirAngleDiff(0),
		qwLinkID(0),
		wLenFromLink(0),
		nMaxSpeed(0), 
		dfLen(0.0), 
		nRoadRank(LINK_ROAD_RANK_NONE), 
		nConnect(LINK_CONNECT_NONE), 
		nRoadType(ROAD_TYPE_NORMAL), 
		nLanes(0), 
		qwStNodeID(0), 
		dfStNodeX(0.0), 
		dfStNodeY(0.0), 
		nStNodeType(0), 
		qwEdNodeID(0), 
		dfEdNodeX(0.0), 
		dfEdNodeY(0.0), 
		nEdNodeType(0)
	{
		memset(reinterpret_cast<void *>(szRoadName), 0, sizeof(szRoadName));
	}

	bool operator<(const struct sMatchEntry& data) const
	{
		// 2026-07-08 최정우 주석 처리
		//return dfIntersectLenSgmt < data.dfIntersectLenSgmt;

		// 소프트 비용(거리 + 방위각 가중) 최소 우선.
		// 비용 동일 시 INTERSECT_LEN(GPS↔세그먼트 교차점 거리)로 tie-break (2026-07-08 최정우 수정)
		if (dfCost != data.dfCost)
			return dfCost < data.dfCost;
		return dfIntersectLenSgmt < data.dfIntersectLenSgmt;
	}
} MATCH_ENTRY, *PMATCH_ENTRY;

#define MATCH_ENTRY_SIZE												sizeof(MATCH_ENTRY)
																			// [2026-09-17 최정우 확인] 현재 참조처 없음 — 유일한 사용처였던
																			//   MapMatch.cpp 의 memcpy 통복사가 2026-07-10 에 필드별 복사로
																			//   교체되며(구조체 레이아웃 불일치 버그) 주석 처리됐다.
																			//   같은 통복사를 되살리려는 시도를 막기 위해 그 주석과 함께 남겨 둔다

#endif //__DATADEFINE_H__

/**
 * @file DataFormat.h
 * @brief 세그먼트 및 링크 정보 구조 헤더 파일
*/
#ifndef __DATAFOFORMAT_H__
#define __DATAFOFORMAT_H__

#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataDefine.h"

using namespace std;

#define INVALID_GRID_COL_NO			-1									// 유효하지 않은 GRID X 좌표 번호
#define INVALID_GRID_ROW_NO			-1									// 유효하지 않은 GRID Y 좌표 번호
#define INVALID_GRID_ID				-1									// 유효하지 않은 GRID ID
#define INVALID_GRID_SPLIT_INDEX	0xFF								// 유효하지 않은 GRID 9등분 인덱스 (0~8 범위 밖) (2026-09-21 최정우 추가)

#pragma pack(push, 1)

/**
 * @struct sPoint
 * @brief 좌표 정보
*/
typedef struct sPoint
{
	double							dfX;								// X 좌표
	double							dfY;								// Y 좌표

	sPoint() : 
		dfX(0.0), 
		dfY(0.0)
	{}

	bool operator==(const struct sPoint& stPoint) const
	{
		if (dfX != stPoint.dfX) return false;
		return dfY == stPoint.dfY;
	}

	bool operator!=(const struct sPoint& stPoint) const
	{
		if (dfX != stPoint.dfX) return true;
		return dfY != stPoint.dfY;
	}
} POINT, *PPOINT;

#define POINT_SIZE														sizeof(POINT)

/**
 * @struct sDataFileHead
 * @brief 데이터 파일 헤더
*/
typedef struct sDataFileHead
{
	uint32							dwGridInfoCount;					// 그리드별 세그먼트 범위 개수
	uint32							dwGridSgmtInfoCount;				// 그리드별 세그먼트 정보 개수
	uint32							dwLinkSgmtInfoCount;				// 링크별 세그먼트 정보 개수
	uint32							dwLinkInfoCount;					// 링크별 세그먼트 정보 개수
																		//   [정정] 링크 정보(LINK_INFO_DATA) 개수다 — 위 줄 문구 복사 오기 (2026-10-06 최정우 주석 수정)
	uint32							dwTurnInfoCount;					// 시작 링크 기준으로 연결된 링크 회전 정보 개수
	uint32							dwGridInfoStartOffset;				// 그리드별 세그먼트 범위 시작 Offset
	uint32							dwGridInfoSize;						// 그리드별 세그먼트 범위 크기
	uint32							dwGridSgmtInfoStartOffset;			// 그리드별 세그먼트 정보 시작 Offset
	uint32							dwGridSgmtInfoSize;					// 그리드별 세그먼트 정보 크기
	uint32							dwLinkSgmtInfoStartOffset;			// 링크별 세그먼트 정보 시작 Offset
	uint32							dwLinkSgmtInfoSize;					// 링크별 세그먼트 정보 크기
	uint32							dwLinkInfoStartOffset;				// 링크 정보 시작 Offset
	uint32							dwLinkInfoSize;						// 링크 정보 크기
	uint32							dwTurnInfoStartOffset;				// 시작 링크 기준으로 연결된 링크 회전 정보 시작 Offset
	uint32							dwTurnInfoSize;						// 시작 링크 기준으로 연결된 링크 회전 정보 크기
} DATA_FILE_HEAD, *PDATA_FILE_HEAD;

#define DATA_FILE_HEAD_SIZE												sizeof(DATA_FILE_HEAD)

/**
 * @struct sGridInfo
 * @brief 그리드별 세그먼트 범위
*/
typedef struct sGridInfo
{
	uint32							dwGridID;							// 그리드 아이디
	uint32							dwSgmtOffset;						// 그리드별 세그먼트 시작 Offset
	uint16							wSgmtCount;							// 그리드별 세그먼트 개수

	sGridInfo() :
		dwGridID(0), 
		dwSgmtOffset(0), 
		wSgmtCount(0)
	{}
} GRID_INFO, *PGRID_INFO;

#define GRID_INFO_SIZE													sizeof(GRID_INFO)

/**
 * @struct sGridSgmtInfo
 * @brief GRID 별 세그먼트 정보
*/
typedef struct sGridSgmtInfo
{
	uint32							dwSgmtOffset;						// 세그먼트 Offset (0~n)
	uint32							dwX;								// 세그먼트 X 좌표 (Degree * 360000)
	uint32							dwY;								// 세그먼트 Y 좌표 (Degree * 360000)
	uint16							wDirAng;							// 세그먼트 진행 방향 (방위각, Degree)
	uint16							wLenSgmt;							// 세그먼트의 거리 (m)
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점부터 세그먼트 시작점까지 거리 (m)

	bool operator<(const struct sGridSgmtInfo& stGridSgmtInfo) const
	{
		return qwLinkID < stGridSgmtInfo.qwLinkID;
	}
} GRID_SGMT_INFO, *PGRID_SGMT_INFO;

#define GRID_SGMT_INFO_SIZE												sizeof(GRID_SGMT_INFO)

/**
 * @struct sLinkSgmtInfo
 * @brief 링크별 세그먼트 정보
*/
typedef struct sLinkSgmtInfo
{
	uint32							dwSgmtOffset;						// 세그먼트 Offset (0~n)
	uint32							dwX;								// 세그먼트 X 좌표 (Degree * 360000)
	uint32							dwY;								// 세그먼트 Y 좌표 (Degree * 360000)
	uint16							wDirAng;							// 세그먼트 진행 방향 (방위각, Degree)
	uint16							wLenSgmt;							// 세그먼트 거리 (m)
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점부터 세그먼트 시작점까지 거리 (m)

	sLinkSgmtInfo() : 
		dwSgmtOffset(0), 
		dwX(0), 
		dwY(0), 
		wDirAng(0), 
		wLenSgmt(0),		// [버그 수정, 2026-09-17 최정우] 초기화 누락 — 같은 구조체의 나머지
							//   6개 필드는 모두 초기화되는데 이 필드만 빠져 있었다. 통상 경로는
							//   link.psf 에서 값을 채우지만, 채우기 전에 읽히면 쓰레기 길이가
							//   거리 계산에 들어간다(sGridSgmtInfo 쪽 동명 필드와 혼동 주의)
		qwLinkID(0), 
		wLenFromLink(0)
	{}
} LINK_SGMT_INFO, *PLINK_SGMT_INFO;

#define LINK_SGMT_INFO_SIZE												sizeof(LINK_SGMT_INFO)

/**
 * @struct sLinkInfoData
 * @brief 세그먼트별 링크 정보 데이터
 * @remark
 * 	- nRoadRank : LINK_ROAD_RANK_* — MOCT_LINK.ROAD_RANK (101~108)
 *	- nConnect : 0:연결로 아님, 1:연결로 (MOCT_LINK.CONNECT). 101~108:구 링크 등급별 연결로
 * 	- nRoadType : 000:일반, 001:교량, 002:터널, 003:고가, 004:지하 (MOCT_LINK.ROAD_TYPE)
 * 	  [정정] 공식 코드값은 0:일반, 1:고가, 2:지하, 3:교량, 4:터널 — DataDefine.h eLinkRoadType
 * 	  (2026-08-27 정정분) 기준 (2026-10-06 최정우 주석 수정)
 *	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sLinkInfoData
{
	uint64							qwLinkID;							// 링크 ID
	uint32                          dwSgmtOffset;						// 링크에 해당하는 세그먼트 시작 Offset
	uint16							wSgmtCount;							// 링크에 해당하는 세그먼트 개수
	uint32							dwTurnOffset;						// 진출 링크에 해당하는 회전 정보 Offset
	uint8							nTurnCount;							// 진출 링크의 개수
	uint8							nMaxSpeed;							// 제한 속도 (km/h)
	double							dfLen;								// 링크 길이 (m)
	uint8							nRoadRank;							// 도로 종별
	uint8							nConnect;							// 연결로 코드
	uint8							nRoadType;							// 도로 유형
	uint8							nLanes;								// 차선 정보
	uint8							nRestVeh;							// 통행제한차량 0:모두 3:버스 4:트럭 5:이륜차 (MOCT_LINK.REST_VEH)
																		//   현재 매칭 판정 미사용 — 차종 축 정리 후 활용 (2026-08-22 최정우 추가)
	uint8							nRoadUse;							// 도로사용여부 0:사용, 1:미사용 (MOCT_LINK.ROAD_USE)
																		//   미사용 4,244건은 개통예정·폐지 구간. 현재 매칭 판정 미사용 —
																		//   후보 제외 여부는 추후 결정 (2026-08-22 최정우 추가)
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	uint32							dwStNodeX;							// 시작 노드 X
	uint32							dwStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성
	uint64							qwEdNodeID;							// 종료 노드 ID
	uint32							dwEdNodeX;							// 종료 노드 X
	uint32							dwEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성[3]
	uint64							qwOppositeLinkID;					// 반대방향(왕복분리) 짝 링크 ID, 0=없음 (2026-08-19 최정우 추가)
} LINK_INFO_DATA, *PLINK_INFO_DATA;

#define LINK_INFO_DATA_SIZE												sizeof(LINK_INFO_DATA)

/**
 * @struct sLinkInfo
 * @brief 세그먼트별 링크 정보
 * @remark
 * 	- nRoadRank : LINK_ROAD_RANK_* — MOCT_LINK.ROAD_RANK (101~108)
 *	- nConnect : 0:연결로 아님, 1:연결로 (MOCT_LINK.CONNECT). 101~108:구 링크 등급별 연결로
 * 	- nRoadType : 000:일반, 001:교량, 002:터널, 003:고가, 004:지하 (MOCT_LINK.ROAD_TYPE)
 * 	  [정정] 공식 코드값은 0:일반, 1:고가, 2:지하, 3:교량, 4:터널 — DataDefine.h eLinkRoadType
 * 	  (2026-08-27 정정분) 기준 (2026-10-06 최정우 주석 수정)
 *	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sLinkInfo
{
	uint32                          dwSgmtOffset;						// 링크에 해당하는 세그먼트 시작 Offset
	uint16							wSgmtCount;							// 링크에 해당하는 세그먼트 개수
	uint32							dwTurnOffset;						// 진출 링크에 해당하는 회전 정보 Offset
	uint8							nTurnCount;							// 진출 링크의 개수
	uint8							nMaxSpeed;							// 제한 속도 (km/h)
	double							dfLen;								// 링크 길이 (m)
	uint8							nRoadRank;							// 도로 종별
	uint8							nConnect;							// 연결로 코드
	uint8							nRoadType;							// 도로 유형
	uint8							nLanes;								// 차선 정보
	uint8							nRestVeh;							// 통행제한차량 0:모두 3:버스 4:트럭 5:이륜차 (MOCT_LINK.REST_VEH)
																		//   현재 매칭 판정 미사용 — 차종 축 정리 후 활용 (2026-08-22 최정우 추가)
	uint8							nRoadUse;							// 도로사용여부 0:사용, 1:미사용 (MOCT_LINK.ROAD_USE)
																		//   미사용 4,244건은 개통예정·폐지 구간. 현재 매칭 판정 미사용 —
																		//   후보 제외 여부는 추후 결정 (2026-08-22 최정우 추가)
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	uint32							dwStNodeX;							// 시작 노드 X
	uint32							dwStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성
	uint64							qwEdNodeID;							// 종료 노드 ID
	uint32							dwEdNodeX;							// 종료 노드 X
	uint32							dwEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성
	uint64							qwOppositeLinkID;					// 반대방향(왕복분리) 짝 링크 ID, 0=없음 (2026-08-19 최정우 추가)
} LINK_INFO, *PLINK_INFO;

#define LINK_INFO_SIZE													sizeof(LINK_INFO)
typedef unordered_map<uint64, LINK_INFO>								mapLinkInfo;

/**
 * @struct sTurnInfo
 * @brief 시작 링크 기준으로 연결된 링크 회전 정보
*/
typedef struct sTurnInfo
{
	uint64							qwInLinkID;							// 진입 링크 ID
	uint64							qwOutLinkID;						// 진출 링크 ID
	sint16							nTurnAng;							// 진입 링크에서 진출 링크의 회전 각도 (Degree, -180~180)
	uint16							nTurnType;							// MOCT 회전제한유형 (011 은 11 로 저장, 출력은 %03u)
	uint8							nTurnOper;							// MOCT 회전제한운영 0:전일제, 1:시간제

	bool operator<(const struct sTurnInfo& stTurnInfo) const
	{
		return nTurnAng < stTurnInfo.nTurnAng;
	}
} TURN_INFO, *PTURN_INFO;

#define TURN_INFO_SIZE													sizeof(TURN_INFO)

#pragma pack(pop)

#endif //__DATAFOFORMAT_H__

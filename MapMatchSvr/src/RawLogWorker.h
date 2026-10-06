/**
 * @file RawLogWorker.h
 * @brief 원시 GPS batch 맵매칭·DB 결과 갱신 워커
*/
#ifndef __RAWLOGWORKER_H__
#define __RAWLOGWORKER_H__

#include <string>
#include <vector>
#include <unordered_map>
#include <set>											// 링크 커버리지 대조 (2026-09-22 최정우 추가)
#include <algorithm>									// stable_sort — MergeAdjacentNodeStepRows() 병합 순서 정렬용 (2026-09-15 최정우 추가)
#include "TypeDefine.h"
#include "MessageType.h"
#include "Thread.h"
#include "PostgrePool.h"
#include "ProcessManager.h"
#include "ChargeDataLoader.h"
#include "CodeMap.h"

using namespace std;

// [2026-09-22 최정우 이동] CHARGE_INSERT_ROW 정의를 sVehicleTripSession 앞으로 옮긴다 —
//   세션이 과금 행 방출 대기 큐(vtPendingEmit)를 vector 로 들고 있어야 해서, 세션 정의 시점에
//   이 구조체가 **완전한 타입**이어야 한다. 내용은 그대로이고 위치만 바뀌었다.

/**
 * @struct sChargeInsertRow
 * @brief [charge_insert] bulk INSERT 1행 파라미터 — 6개 CHARGE_TYPE 공용
 *   (2026-08-13 최정우 수정, 2026-09-17 최정우 정정 — 종전 "4유형 공용" 표기는 일반도로(0)·
 *    면제도로(5)가 추가되기 전 서술이라 현행에 맞춤)
 * @remark PRIM_CHARGEHAND 컬럼 순서(query.sql [charge_insert] 와 반드시 일치): trip_id, device_key,
 *   trip_seq, charge_type, charge_unit, link_id, from_id, to_id, from_lat, from_lon, to_lat, to_lon,
 *   zone_id, zone_name, dist_m, speed_kmh, speed_limit_kmh, occur_dt, trip_start_dt, tollgate_id,
 *   entry_tollgate_id, exit_tollgate_id, reg_dt, upd_dt, charge_yn, charge_status, stay_seconds,
 *   trip_end_dt, start_gps_seq, end_gps_seq, non_charge_reason (2026-09-01 최정우 수정)
*/
typedef struct sChargeInsertRow
{
	string							strTripId;
	string							strDeviceKey;
	string							strChargeSeq;						// PRIM_CHARGEHAND.trip_seq
																		//   행 생성 시엔 세션 nChargeSeq(임시값)가 들어가고, 실제 DB
																		//   값은 ReleaseChargeQueue() 가 방출 시점에 nEmitSeq+1 로
																		//   덮어쓴다 (2026-10-06 최정우 주석 추가)
	string							strChargeType;						// 0=NODE_STEP(일반도로), 1=OPEN_ROAD(개방식),
																		//   2=CLOSED_ROAD(폐쇄식), 3=SPEED(구간단속),
																		//   4=PARKING(주정차), 5=EXEMPT(면제도로)
																		//   (2026-09-17 최정우 보완 — 종전엔 1·2만 적혀 있었음)
	string							strChargeUnit;						// 0=NODE(일반도로·개방식), 1=LINK(폐쇄식·구간단속·
																		//   면제도로), 2=POLYGON(주정차)
																		//   (2026-09-17 최정우 보완)
	string							strLinkId;							// 현재 전 유형 공통으로 빈 값(DB NULL) — Build* 전부 "" 고정
																		//   (2026-10-06 최정우 주석 추가)
	string							strFromId;							// 개방형=구역road_id(2026-08-25 게이트ID에서 변경), 폐쇄형=입구게이트ID
	string							strToId;							// 개방형=구역road_id(2026-08-25 게이트ID에서 변경), 폐쇄형=출구게이트ID
																		// [보완] 전 유형: 일반도로(0)=링크ID, 개방·주정차·면제(1·4·5)=
																		//   구역 road_id, 폐쇄·구간단속(2·3)=게이트ID. 출구 미확인
																		//   강제마감 행은 TO_ID 빈 값 (2026-10-06 최정우 주석 수정)
	string							strFromLat;
	string							strFromLon;
	string							strToLat;
	string							strToLon;
	string							strZoneId;							// base_roadlink.road_id (없으면 빈 문자열)
	string							strZoneName;						// base_roadlink.road_nm (없으면 빈 문자열)
	string							strDistM;							// 폐쇄형: 입구~출구 누적거리(m). 개방형(2026-08-25부터) — 정상진입 run은
																		//   구역 전체길이, 트립시작 run은 출발~이탈 실관측 거리 (2026-08-12 최정우 추가)
																		// [보완] 폐쇄·구간단속 정상 진출은 게이트 간 구역 선 길이
																		//   (CalcZoneSpanM), 일반도로·면제·주정차는 실측 누적거리
																		//   (2026-10-06 최정우 주석 추가)
	string							strSpeedKmh;						// 순간속도 — 직전 매칭 위치·시각 있을 때만 계산, 없으면 빈 값 (2026-08-12 최정우 추가)
																		// [정정] 순간속도가 아니라 **평균속도**(dist_m ÷ 체류/경과시간
																		//   ×3.6)다 — 전 유형 Build* 가 이 방식. 시간이 0 이하면 빈 값
																		//   (DB 0). 예외: 인수인계 행 등 시간 산출 불가 행은 그 tick 의
																		//   보고 속도를 쓴다 (2026-10-06 최정우 주석 수정)
	string							strSpeedLimitKmh;					// 매칭 링크 제한속도(MATCH_LINK_INFO.nMaxSpeed) (2026-08-12 최정우 추가)
																		// [정정] 폐쇄형만 매칭(또는 직전) 링크 제한속도(진입 애매 시 "0"),
																		//   구간단속은 구역 등록값(base_roadlink.speed_limit_kmh),
																		//   그 외 유형·출구 미확인 강제마감은 빈 값(DB 0)
																		//   (2026-10-06 최정우 주석 수정)
	string							strOccurDt;							// YYYYMMDDHH24MISS
	string							strTripStartDt;						// YYYYMMDDHH24MISS (trip_id 에서 추출)
	string							strTollgateId;						// 개방형·폐쇄형 모두 실측상 빈 값
																		// [정정] 개방형(1)은 run 중 M게이트를 실제로 지났으면 그 게이트ID를
																		//   넣는다(BuildOpenZoneRow). 그 외 유형은 빈 값 (2026-10-06 최정우 주석 수정)
	string							strEntryTollgateId;					// 폐쇄형 전용: 입구게이트ID. 개방형은 빈 값 (2026-08-12 최정우 추가)
	string							strExitTollgateId;					// 폐쇄형 전용: 출구게이트ID. 개방형은 빈 값 (2026-08-12 최정우 추가)
	string							strRegDt;							// YYYYMMDDHH24MISS — INSERT 실행 시각(벽시계)
																		//   (2026-08-12 최정우 추가)
																		//   Build* 가 넣은 행 생성 시각은 ReleaseChargeQueue() 가 방출
																		//   시점 벽시계로 덮어쓴다 (2026-10-06 최정우 주석 추가)
	string							strUpdDt;							// [2026-09-17 최정우 정정] "strRegDt 와 항상 동일" 이
																		//   아니다. 게이트형·주정차(OPEN/CLOSED/SPEED/PARKING)는
																		//   reg_dt 와 같은 값이지만, 일반도로·면제도로
																		//   (BuildNodeStepRow/BuildNodeStepRowFromLinkRange/
																		//   BuildExemptRow)는 occur_dt 를 넣는다 — 사용자 지시
																		//   (2026-08-14) "구역 이탈과 트립 종료가 같은 tick 이면
																		//   진출시각으로 맞춘다" 의 결과. 트립 정상종료 시
																		//   [trip_end] UPDATE 가 다시 GPS 종료시각으로 덮는다
																		// [정정] 2026-10-04 부터 위 설명은 더 이상 맞지 않는다 — 전 유형
																		//   Build* 가 strRegDt 와 같은 값을 넣고, ReleaseChargeQueue() 가
																		//   방출(INSERT 직전) 시점에 REG_DT·UPD_DT 를 같은 벽시계로 다시
																		//   찍는다. [trip_end] UPDATE 는 UPD_DT 를 그 UPDATE 실행
																		//   벽시계로 갱신한다(GPS 종료시각 아님) (2026-10-06 최정우 주석 수정)
	string							strChargeYn;						// 빈 값=DB 기본(Y). 폐쇄형 입/출구 게이트 이상 시 "N" 명시 (2026-08-12 최정우 추가)
	string							strChargeStatus;					// 빈 값=DB 기본(0=PENDING). 폐쇄형 입/출구 게이트 이상 시 "4"(SKIP) 명시 (2026-08-12 최정우 추가)
																		// [정정] 게이트 이상·강제마감은 "3"(AUDIT)이다(bGateAnomaly ? "3" : "0").
																		//   "4"(SKIP)는 면제도로(5) 강제마감 전용. 엔진은 0/3/4 만 쓰고
																		//   1·2 는 과금서버 소유 (2026-10-06 최정우 주석 수정)
	string							strStaySeconds;						// 체류시간(초) — 주정차 전용(컬럼 코멘트: "체류 시간(초). 주정차 위반 판단"),
																		//   다른 3종은 빈 값(DB 기본 0) (2026-08-13 최정우 추가)
																		// [정정] 현재는 6개 유형 전부 체류/경과시간(초)을 채운다
																		//   (2026-10-06 최정우 주석 수정)
	string							strTripEndDt;						// 주정차 TTL 만료 강제마감 전용 — 더 이상 GPS 수신 불가로 판단한 시각.
																		//   그 외는 빈 값(NULL 유지, 실제 TRIP_EVENT=2 시 [trip_end] UPDATE가 채움) (2026-08-13 최정우 추가)
																		// [정정] 주정차는 채우지 않는다. 직접 채우는 곳은 폐쇄형 출구
																		//   미확인 강제마감(AppendExpiredClosedRoadCharge)·구간단속 미러
																		//   강제마감(AppendExpiredSpeedZoneCharge) 둘뿐이고, 나머지는 빈 값으로
																		//   INSERT 후 [trip_end] UPDATE(정상종료·TTL 마감 공용)가 채운다
																		//   (2026-10-06 최정우 주석 수정)
	string							strStartGpsSeq;						// 진입 시점 PRIM_RAWGPS.GPS_SEQ — 웹뷰어 G순번과 동일 개념
																		//   (2026-08-28 최정우 추가)
	string							strEndGpsSeq;						// 구역 안에서 실제로 마지막 확인된 GPS_SEQ(보간 경계 tick이 아님)
																		//   (2026-08-28 최정우 추가)
	string							strNonChargeReason;					// 빈 값=0(NCR_NORMAL, 정상 과금 — [charge_insert]가 변환.
																		//   2026-09-15 최정우 수정, 종전엔 NULL 이었음).
																		//   ※ 종전엔 그 변환이 유일한 보증이었다 — DB 컬럼에 DEFAULT/
																		//   NOT NULL 이 없어 그 SQL 을 안 타는 INSERT 는 NULL 이 됐다.
																		//   doc/deploy_2026-09-17.sql 로 DEFAULT 0 **적용 완료**
																		//   (2026-09-17, 로컬·실서버 양쪽. 최정우).
																		//   그 외는 NCR_* 코드(DataDefine.h) — 이 행의
																		//   일부 값(주로 speed_kmh/stay_seconds)을 신뢰할 수 없어
																		//   근사·생략 처리했다는 예외사유. 임시 코드 체계, 추후
																		//   정식 에러코드 정리 시 재배정 예정 (2026-09-01 최정우 추가)
	bool							bNoMergeAfter = false;				// DB에 안 실림, 배치 내부 전용. true면 이 행 바로 뒤에
																		//   오는 일반도로(CHARGE_TYPE=0) 행과 MergeAdjacentNodeStepRows()가
																		//   합치면 안 됨 — gps_seq는 가깝지만(디바운스·보류 등으로 벌어진
																		//   "표기상" 공백이 아니라) 사이에 실제 다른 과금유형 등록 링크가
																		//   껴 있어 진짜로 끊긴 구간이기 때문. ApplyGateExitCarryDist()의
																		//   섬 분리에서 세팅 (2026-09-14 최정우 추가)
																		//   [2026-09-23 최정우 추가] 게이트 진출 이월 폐기 시 만드는
																		//   **orphan span 복구 행**도 세팅한다 — 그 행은 진출~다음 구역
																		//   진입 사이의 독립 구간이라, 뒤에 오는 일반도로 행과 합쳐지면
																		//   위치와 경계 링크가 다시 흐려진다

	// [2026-09-22 최정우 추가] 워터마크 큐에서 이미 방출(번호 확정)된 행 표시 — DB에 안 실리는
	//   배치 내부 전용. 트립 전환처럼 배치 도중에 방출된 행이 배치 끝에서 큐로 다시 들어가
	//   이중 처리되는 것을 막는다.
	bool							bEmitted = false;
	// [2026-09-22 최정우 추가] 이 행이 덮은 링크 목록 — DB 에 안 실리는 배치 내부 전용.
	//   트립 마감 시 "실제 경유한 링크" 와 대조해 **어느 행에도 안 덮인 링크**를 찾는 데 쓴다.
	//   sZoneRunSession::vtRunLinks 주석 참고.
	vector<uint64>				vtCoveredLinks;
} CHARGE_INSERT_ROW, *PCHARGE_INSERT_ROW;

/**
 * @struct sVehicleTripSession
 * @brief trip_id 단위 운행 세션 (연속 맵매칭·TTL 유지용)
 * @remark [2026-09-21 최정우 정정] 종전 주석의 "세션 맵 키 = TRIP_ID" 는 **틀렸다** —
 *   실제 키는 **DEVICE_KEY** 다(run() 의 mapSessions.find(strDeviceKey)). 이 차이가 동작을
 *   가르는 지점이 여러 곳이라 오인하면 안 된다 — ①종료신호(TRIP_EVENT=END) 없이 같은 차량의
 *   다음 운행이 시작되면 세션이 **그 자리에서 재사용**되므로 TTL 이 그 상황을 못 잡는다
 *   (그래서 NCR_NO_TRIP_END_FORCED_CLOSE=62 가 따로 있다). ②트립이 바뀌어도 세션 객체는
 *   살아있으므로, 트립 단위 상태를 새로 추가하면 ResetTripSessionForBegin(bFullReset) 에
 *   리셋을 **반드시 같이** 넣어야 한다(실제로 빠뜨려 2026-09-21 에 수정한 전례 있음).
 *   TRIP_ID 자체는 수집서버가 START 시 적재하며, 세션은 szTripId 로 신규 trip 을 감지한다.
*/
// 같은 유형의 과금구역이 한 도로를 공유할 수 있다(인접 구역의 경계 링크, 장구간 안의 단구간 등).
//   예전엔 유형당 세션이 1개뿐이라 겹친 구역 중 하나만 잡히고 나머지는 조용히 사라졌다.
//   구역별로 세션을 따로 들고 있으면 동시에 여러 구역을 진행할 수 있다 (2026-08-23 최정우 추가)
typedef struct sZoneRunSession
{
	char							szRoadID[20+1];						// 진행 중인 구역 road_id
	time_t							dtEntryTime;						// 진입 시각
	uint32							dwEntryGpsSeq;						// dtEntryTime 과 동일 tick 의 GPS_SEQ —
																		//   PRIM_CHARGEHAND.start_gps_seq 원본 (2026-08-28 최정우 추가)
	double							dfEntryX;							// 진입 시점 매칭 위치 경도 — from_lon
	double							dfEntryY;							// 진입 시점 매칭 위치 위도 — from_lat
	double							dfAccumDistM;						// 진입 이후 누적 이동거리(m)
	double							dfLastX;							// 직전 매칭 위치 경도 — to_lon
	double							dfLastY;							// 직전 매칭 위치 위도 — to_lat
	uint64							qwLastLinkID;						// 구역 안에서 마지막으로 매칭된 링크 — 이탈 보정용
	time_t							dtLastInZoneTime;					// 구역 안에서 마지막으로 확정매칭됐던 GPS 시각(dfLastX/Y 와
																		//   같은 시점) — 일반도로(NODE_STEP)만 사용. node_exitcnt
																		//   디바운스로 이탈 확정이 몇 틱 늦게 일어나도, occur_dt·
																		//   stay_seconds 는 이 시각 기준으로 계산해 디바운스 대기
																		//   시간(=이미 다른 구역에 들어간 뒤 시간)이 섞이지 않게 한다
																		//   (실측 000376_20260819094414 RL-Z00002 — 실제 마지막 재
																		//   구역내 확정은 seq38 인데 디바운스 대기 중 seq42~44 가 이미
																		//   RL-Z00003(구간단속)로 넘어가 있어, 이탈 확정 시각(dtGPS)을
																		//   그대로 쓰면 두 구역 범위가 겹쳐 보임) (2026-08-24 최정우 추가)
																		// [정정] "일반도로만 사용"은 현재 틀리다 — 개방형(Process/
																		//   AppendExpired/AppendTripEnd OpenGate)·면제도로도 마감 시각으로
																		//   쓴다 (2026-10-06 최정우 주석 수정)
	uint32							dwLastInZoneGpsSeq;					// dtLastInZoneTime 과 동일 tick 의 GPS_SEQ —
																		//   PRIM_CHARGEHAND.end_gps_seq 원본(OPEN·NODE_STEP)
																		//   (2026-08-28 최정우 추가)
	time_t							dtExitCandidateTime;				// "무존" 최초 감지 시각 — 재진입 유예용(면제도로만 사용)
	int								nExitTicks;							// 이탈 연속 감지 횟수(디바운스) — 일반도로(NODE_STEP)만 사용
																		//   (2026-08-24 최정우 추가 — 순간 오매칭 1틱으로 세션이
																		//   쪼개지는 결함 방지, PARKING park_exitcnt 와 동일 원리)
																		// [정정] 개방형(ProcessOpenGateCharge) run 도 이 카운터로 이탈
																		//   디바운스한다 — "일반도로만" 아님 (2026-10-06 최정우 주석 수정)
	// 이탈 디바운스(node_exitcnt) 스트릭의 "첫" 밖 tick 좌표/시각 — 디바운스가 몇 틱 뒤에야
	//   확정되므로, 확정 시점(마지막 tick)을 그대로 보간 기준으로 쓰면 이미 구역에서 한참 멀어진
	//   지점과 보간하게 돼 엉뚱한 결과가 나온다(PARK_RUN_SESSION dfFirstOutX/Y·dtFirstOut 과 동일
	//   문제·동일 해법). nExitTicks 가 0→1 로 바뀌는 순간(디바운스 시작 전)에 잡아뒀다가, 경계
	//   노드 통과 시각 보간의 "밖" 기준점으로 쓴다(2026-08-25 최정우 추가, NODE_STEP/OPEN 공용)
	//   면제도로(ProcessExemptZoneCharge)도 이탈 경계 보정에 쓴다 (2026-10-06 최정우 주석 추가)
	double							dfFirstOutX;
	double							dfFirstOutY;
	time_t							dtFirstOut;
	// 개방형(ROAD_KIND=1) 전용 — 다른 유형은 기본값(false) 그대로 미사용 (2026-08-25 최정우 추가)
	// 구역 안에서 실측된 tick 수와 그 중 최대 순간속도 — 개방형(OPEN) 체류시간 보정 전용.
	//   구역 안에 tick 이 **하나뿐**이면 진입·진출 보간이 통행 전체를 감쌀 수 없어, 거리는
	//   구역 등록길이(전 구간)인데 경과시간은 보간된 두 경계 사이(수 초)만 남는 모순이 생긴다
	//   — 실측 000994_20250903152350 RL-Z00004(141.6m): seq87 한 tick 만 구역 안에 걸려
	//   142m/1초/512km/h 가 됐다(그 tick 의 보고 속도는 50km/h). 구역이 GPS 수신 간격 동안의
	//   이동거리보다 짧으면 구조적으로 발생한다.
	//   BuildOpenZoneRow() 가 이 둘로 "평균속도는 구역 안에서 관측된 최대 순간속도를 넘을 수
	//   없다"는 물리 한계를 걸어 경과시간을 보정한다 (2026-09-21 최정우 추가)
	int								nInZoneTicks;
	float							fMaxSpeed;
	bool							bStartedByTrip;						// true=트립 자체가 이 구역 도로 위에서 시작(TRIP_EVENT=START
																		//   행의 매칭 링크가 이미 이 구역 link_ids 안). dist_m 산출
																		//   방식이 갈린다 — false 면 구역 전체길이(dfLengthM) 고정값,
																		//   true 면 dfAccumDistM(출발좌표~이탈 실관측 거리)
	bool							bGateCrossed;						// true=이 진행(run) 동안 그 구역의 M게이트를 실제로 지남 —
																		//   bStartedByTrip=true 인 run 의 charge_yn/status(Y/0 vs
																		//   N/3) 판정 기준. bStartedByTrip=false 면 게이트 통과가
																		//   구역 진입의 전제라 항상 Y/0(미참조)
	bool							bSeenBeforeGate;					// true=이 run 안에서 "게이트 이전" 상태를 한 번이라도 확인함
																		//   — 이게 true 여야만 나중에 게이트 위치 도달을 "진짜 통과"로
																		//   확정한다(UpdateOpenGateCrossed). 없으면 이미 게이트를
																		//   지난 뒤 시작한 run(case C)이 첫 틱만으로 곧바로
																		//   "통과함"으로 오판된다 (2026-08-25 최정우 추가)
	// 진입 링크 확정 보류 — 게이트형 구역(폐쇄식·개방식·구간단속) 진출이 확정되는 tick 은 매칭점이
	//   진출게이트로 클램프될 뿐 **아직 그 구역 링크에 매칭**돼 있다. 진출게이트가 링크 끝 노드에
	//   놓인 구역에서는 그 tick 에 run 을 열면 FROM_ID 가 구역 링크 자신이 되어, 폐쇄식 구간에서
	//   일반도로가 시작한 것처럼 보인다(실측 000972_20260916100000: TG00007/TG00008 이 링크
	//   2040423801 의 첫점·끝점이라 링크 전체가 폐쇄식인데 FROM_ID=2040423801). 그 시점엔 다음 링크를
	//   아직 모르므로, 여기에 "진출한 구역 링크"를 담아두고 다음 tick 에 실제 링크가 나오면 그 사이
	//   누락 링크를 복구해 첫 링크를 FROM_ID 로 확정한다. 0 이면 보류 없음
	//   (2026-09-16 최정우 추가, 사용자 지적)
	uint64							qwPendingEntryFromLinkID;
	uint64							qwEntryLinkID;						// 진입 시점 매칭 링크ID — 일반도로(NODE_STEP)의 신규 스코프
																		//   (미등록 도로 pseudo-zone, szRoadID=="") 전용. FROM_ID를
																		//   road_id 대신 링크ID로 채워야 해서 추가(qwLastLinkID가 이미
																		//   "구역 안 마지막 링크"를 들고 있는 것과 대칭). road_kind=0
																		//   정식 구역 run은 그대로 road_id 기반이라 미사용(2026-09-01 최정우 추가)
	uint64							qwFirstOutLinkID;					// dfFirstOutX/Y 와 동일 시점(디바운스 스트릭의 "첫" 밖 tick)의
																		//   매칭 링크ID — 누락링크 보정(FindLinkPathBounded) 목표를
																		//   디바운스"확정" tick(이미 구역에서 몇 틱 더 간 지점)이 아니라
																		//   이 값으로 잡아야 정확한 경계 링크가 나온다(실측
																		//   000376_20260826155015 RL-Z00013 — 확정tick 기준으로 찾다
																		//   구역 안쪽 링크가 잘못 나옴) (2026-09-01 최정우 추가)
	// [2026-09-22 최정우 추가 — 사용자 지시] 이 run 이 실제로 **덮은 링크** 목록.
	//   누락 링크 문제의 공통 뿌리는 "어느 행이 어느 링크를 덮었는지 어디에도 기록되지 않는다"는
	//   것이었다. 그래서 복구는 유형별 전이 경로마다 따로 만들어야 했고(조합마다 구멍이 남는다),
	//   검증은 행의 FROM_ID(경계 보정값)와 경로(매칭값)가 서로 다른 기준이라 대조가 안 됐다.
	//   run 이 열려 있는 동안 매 tick 의 경유 링크(MATCH_LINK_INFO.aqwPathLinkIDs — tick 이 안 찍힌
	//   링크까지 포함)를 여기 쌓아두고, 행을 만들 때 CHARGE_INSERT_ROW.vtCoveredLinks 로 넘긴다.
	//   [2026-09-22 2단계] 이 기록은 계측에 그치지 않는다 — FillUncoveredLinkRows() 가 이것으로
	//   "아무도 안 덮은 링크"를 찾아 **일반도로 거리로 복구**한다(판정 변경 있음).
	vector<uint64>					vtRunLinks;							// 경유 링크(연속 중복 제거)
	// 이 run 의 진입 정보를 **주정차 폴리곤 이탈 보정**이 확정했는지 (2026-09-22 최정우 추가 — 사용자 지적)
	//   두 이월이 같은 run 에 겹쳐 들어올 수 있다. 주정차 이탈은 **폴리곤 경계가 곧 이 구간의
	//   물리적 시작점**이므로, 그보다 앞선 사건(면제 진출 이월 등)이 진입 정보를 덮으면 FROM_ID 가
	//   엉뚱한 링크를 가리킨다 — 실측 000993_20250903152139 seq28: 폴리곤 이탈 링크 2040426201 이어야
	//   하는데 면제 이월이 2040425502 로 덮었다. 이 플래그가 서 있으면 진입 정보는 그대로 두고
	//   **거리만** 더한다.
	bool							bEntryFixedByParkExit;
	// 이 run 의 **진출 이월을 이미 걸었는지** (2026-09-22 최정우 추가 — 사용자 지시)
	//   면제 유예 중에도 이월은 "구역을 벗어난 순간"의 사실이므로 먼저 건다. 재진입하면 취소한다.
	bool							bExitCarryArmed;
	// [2026-09-23 최정우 추가 — 사용자 지시] 구역을 벗어난 뒤 **구역 밖에서 실제로 달린 거리**(m).
	//   재진입 유예(exempt_regrace)는 "잠깐 버퍼 주행 후 복귀"(config.ini 주석)를 한 레코드로
	//   유지하려는 것인데, 60초라는 시간만으로는 경계 흔들림과 **실제 이탈 후 재진입**을 구분하지
	//   못한다 — 실측 000998_20260917090000 은 17초 동안 220m 를 41~53km/h 로 달리고 U턴해
	//   돌아오는데도 유예에 흡수돼 면제 run 이 seq3~19 로 잡혔다(그 사이 일반도로·타 면제구역
	//   레코드를 통째로 감싼다). 구역 밖 주행거리가 exempt_outmax 이상이면 유예를 무효로 보고
	//   그 자리에서 마감한다. 재진입하면 0 으로 리셋한다.
	//   ※ dfAccumDistM(구역 안 주행거리)에는 영향 없다 — 이 값은 판정에만 쓰고 기록하지 않는다.
	double							dfOutDist;
	double							dfOutLastX;							// dfOutDist 누적의 직전 기준점(구역 밖 마지막 매칭 위치).
	double							dfOutLastY;							//   dfLastX/Y 는 구역 **안** 마지막 위치라 이탈 경계 보정에
																		//   쓰이므로 건드리면 안 돼 별도로 든다
	// [2026-10-04 최정우 추가 — 사용자 확정 "일반도로 출발 = 일반도로·구간단속에 매칭된 첫 tick"]
	//   트립 시작 판정 구간(qwLastConfirmedLinkID==0) 동안 이 run 에 들어온 tick 별 기록(최대 8).
	//   출발 후보 해소(ResolveStartAmbiguity)로 앞 tick 이 SKIP 으로 강등되면 이 기록으로 첫 유효 tick
	//   에 출발점을 다시 잡는다(ReanchorRunHead). 판정 구간이 끝나면 더 쌓지 않는다.
	int								nHeadTrail;
	uint32							adwHeadSeq[8];						// tick GPS_SEQ
	double							adfHeadAccum[8];					// 그 tick 처리 직후 dfAccumDistM
	time_t							adtHeadTime[8];						// tick GPS 시각
	double							adfHeadX[8];						// tick 매칭 경도
	double							adfHeadY[8];						// tick 매칭 위도
	uint64							aqwHeadLink[8];						// tick 매칭 링크

	sZoneRunSession() :
		dtEntryTime(0), dwEntryGpsSeq(0), dfEntryX(0.0), dfEntryY(0.0), dfAccumDistM(0.0),
		dfLastX(0.0), dfLastY(0.0), qwLastLinkID(0), dtLastInZoneTime(0), dwLastInZoneGpsSeq(0),
		dtExitCandidateTime(0), nExitTicks(0),
		dfFirstOutX(0.0), dfFirstOutY(0.0), dtFirstOut(0),
		nInZoneTicks(0), fMaxSpeed(0.0f),	// (2026-09-21 최정우 추가)
		bStartedByTrip(false), bGateCrossed(false),
		bSeenBeforeGate(false), qwPendingEntryFromLinkID(0), qwEntryLinkID(0), qwFirstOutLinkID(0),
		bEntryFixedByParkExit(false), bExitCarryArmed(false),	// (2026-09-22 최정우 추가)
		dfOutDist(0.0), dfOutLastX(0.0), dfOutLastY(0.0),	// (2026-09-23 최정우 추가)
		nHeadTrail(0)	// (2026-10-04 최정우 추가)
	{
		szRoadID[0] = '\0';
	}
} ZONE_RUN_SESSION;

// 주정차는 구역별로 디바운스·유예·체류 스냅샷을 따로 들어야 해서 전용 구조체를 쓴다
//   (2026-08-23 최정우 추가 — 폴리곤이 겹쳐 설정될 수 있어 동시 진행 지원)
typedef struct sParkRunSession
{
	char							szRoadID[20+1];						// 진행 중인 구역 road_id
	time_t							dtEntryTime;						// 세션 시작 시각 — occur_dt(진입 시각)
	uint32							dwEntryGpsSeq;						// dtEntryTime 과 동일 tick 의 GPS_SEQ —
																		//   PRIM_CHARGEHAND.start_gps_seq 원본 (2026-08-28 최정우 추가)
	double							dfEntryX;							// 시작 raw GPS 경도 — from_lon
	double							dfEntryY;
	double							dfAccumDistM;						// 누적 이동거리(m)
	double							dfLastX;							// 직전 raw GPS 경도(하버사인 기준점)
	double							dfLastY;
	int								nExitTicks;							// 이탈 연속 감지 횟수(park_exitcnt 디바운스)
	time_t							dtLastInZoneTime;					// 마지막으로 조건을 만족한 시각 — 체류 종료 기준
	double							dfLastInZoneX;
	double							dfLastInZoneY;
	uint32							dwLastInZoneGpsSeq;					// dtLastInZoneTime 과 동일 tick 의 GPS_SEQ —
																		//   PRIM_CHARGEHAND.end_gps_seq 원본(정상 진출) (2026-08-28 최정우 추가)
	time_t							dtLastConfirmedTime;				// 마지막 raw_vld=true 확인 시각 — park_ttl 기준
	double							dfLastConfirmedX;
	double							dfLastConfirmedY;
	uint32							dwLastConfirmedGpsSeq;				// dtLastConfirmedTime 과 동일 tick 의 GPS_SEQ —
																		//   PRIM_CHARGEHAND.end_gps_seq 원본(park_ttl 강제마감)
																		//   (2026-08-28 최정우 추가)
	// 이탈 디바운스(park_exitcnt)·재진입 유예(park_regrace) 판정에 쓰인 시간을 체류시간에서
	//   빼기 위해 dtLastInZoneTime을 종료 시각으로 쓰던 기존 방식은, 실제 경계 통과가 그 이후
	//   ~ 첫 이탈 확인 틱 사이 어딘가라는 사실은 반영 못 해 항상 짧게(under-count) 잡혔다.
	//   nExitTicks 가 0→1 로 바뀌는 첫 이탈 틱(디바운스·유예 시작 전, "밖"으로 처음 찍힌 원시좌표)을
	//   따로 잡아뒀다가, InterpolateZoneCrossingTime() 으로 dtLastInZoneTime(안)과의 사이에서
	//   실제 경계 통과 시각을 선형보간한다 (사용자 지시, 2026-08-24 최정우 추가)
	double							dfFirstOutX;
	double							dfFirstOutY;
	time_t							dtFirstOut;
	// [2026-10-04 최정우 추가 — 정책 대조표 B9] 위 두 지점의 **매칭좌표**(신뢰 매칭일
	//   때만, 아니면 0). 규칙3(원시 밖·매칭 안)이나 여유(park_pad)로 "안" 판정된 tick 은 원시좌표가
	//   경계라인 밖이라 원시 쌍으로는 보간이 안 된다 — 그때 "안" 판정에 실제로 쓴 매칭좌표 쌍으로
	//   경계라인 통과 시각을 보간한다.
	double							dfLastInZoneMX;
	double							dfLastInZoneMY;
	double							dfFirstOutMX;
	double							dfFirstOutMY;

	sParkRunSession() :
		dtEntryTime(0), dwEntryGpsSeq(0), dfEntryX(0.0), dfEntryY(0.0), dfAccumDistM(0.0),
		dfLastX(0.0), dfLastY(0.0),
		nExitTicks(0), dtLastInZoneTime(0), dfLastInZoneX(0.0),
		dfLastInZoneY(0.0), dwLastInZoneGpsSeq(0),
		dtLastConfirmedTime(0), dfLastConfirmedX(0.0), dfLastConfirmedY(0.0), dwLastConfirmedGpsSeq(0),
		dfFirstOutX(0.0), dfFirstOutY(0.0), dtFirstOut(0),
		dfLastInZoneMX(0.0), dfLastInZoneMY(0.0), dfFirstOutMX(0.0), dfFirstOutMY(0.0)	// (2026-10-04 최정우 추가)
	{ szRoadID[0] = '\0'; }
} PARK_RUN_SESSION;

// 세션 개시 전 "연속 충족" 카운터 — 구역별로 따로 센다 (2026-08-23 최정우 추가)
typedef struct sParkCandidate
{
	char							szRoadID[20+1];
	int								nTicks;								// 연속 충족 횟수
	time_t							dtTime;								// 연속의 첫 좌표 시각
	uint32							dwGpsSeq;							// dtTime 과 동일 tick 의 GPS_SEQ — 스트릭이 세션으로
																		//   승격되면 PARK_RUN_SESSION.dwEntryGpsSeq 로 승계
																		//   (2026-08-28 최정우 추가)
	double							dfX;								// 연속의 첫 좌표
	double							dfY;
	bool							bTripHead;							// 이 후보 첫 tick 앞의 같은 트립 tick 이 모두 정확도 초과로
																		//   빠졌는지 — 그러면 출발 시각을 트립 첫 GPS 로 당긴다(E19)

	sParkCandidate() : nTicks(0), dtTime(0), dwGpsSeq(0), dfX(0.0), dfY(0.0), bTripHead(false) { szRoadID[0] = '\0'; }
} PARK_CANDIDATE;

// 링크 폴리라인이 주정차 폴리곤 "안"에 들어가 있는 한 구간 (2026-09-21 최정우 추가)
//   한 링크가 폴리곤을 여러 번 드나들 수 있어(모서리를 스치는 링크 등) 목록으로 돌려준다.
//   거리는 전부 "링크 시작 노드로부터"이며, 좌표는 평문 경위도다.
//   bEntry/bExitIsBoundary 는 그 끝점이 **실제 경계 교차**인지, 아니면 링크 자체의 끝
//   (= 링크가 폴리곤 안에서 시작하거나 안에서 끝남)인지를 구분한다 — 종전
//   FindLinkPolygonExitCrossing() 이 "링크 전체가 폴리곤 안"을 false 로 돌려주던 구분을
//   값으로 보존하기 위함.
typedef struct sLinkPolySpan
{
	double							dfStartDistM;						// 구간 시작 — 링크 시작 노드로부터의 거리(m)
	double							dfEndDistM;							// 구간 끝   — 〃
	double							dfStartX;							// 구간 시작 좌표(경도)
	double							dfStartY;							// 〃 (위도)
	double							dfEndX;								// 구간 끝 좌표(경도)
	double							dfEndY;								// 〃 (위도)
	bool							bEntryIsBoundary;					// 시작이 실제 밖→안 경계인가(false=링크가 안에서 시작)
	bool							bExitIsBoundary;					// 끝이 실제 안→밖 경계인가(false=링크가 안에서 끝남)

	sLinkPolySpan() : dfStartDistM(0.0), dfEndDistM(0.0), dfStartX(0.0), dfStartY(0.0),
		dfEndX(0.0), dfEndY(0.0), bEntryIsBoundary(false), bExitIsBoundary(false) {}
} LINK_POLY_SPAN;

typedef struct sVehicleTripSession
{
	uint64							qwLinkID;							// 직전 맵매칭 링크 ID (연속 맵매칭)
	time_t							dtLastSeen;							// 마지막 처리 시각 (TTL sweep용)
	uint32							dwLastGpsSeq;						// 마지막 처리 GPS_SEQ (역전·리셋 감지)
	bool							bStartWarned;						// START(0) 누락 경고 1회용
	double							dfLastMatchX;						// 직전 매칭 성공 X(경도, WGS84) — HEADING/SPEED 계산 기준 (2026-07-08 최정우 추가)
	double							dfLastMatchY;						// 직전 매칭 성공 Y(위도, WGS84) (2026-07-08 최정우 추가)
	time_t							dtLastMatchGps;						// 직전 매칭 성공 GPS 수신시각 — 속도 계산용 (2026-07-08 최정우 추가)
	uint32							dwLastMatchGpsSeq;					// 직전 매칭 성공 GPS_SEQ — 구역 진입 경계 보정 시 그 tick 과
																//   순번을 공유할지 판정하는 데 씀 (2026-09-07 최정우 추가, 사용자 지시)
	time_t							dtLastGpsEventTime;					// 이 trip 에서 지금까지 처리된 gps_dt 최댓값 — TRIP_EVENT=END
																			//   스퓨리어스(순서역전) 판별용. dtLastSeen(wall-clock)과 달리
																			//   GPS 자체 시각 기준(단조증가만 반영, 역행 행은 갱신 안 함)
																			//   (2026-08-25 최정우 추가)
	bool							bHasLastMatch;						// 직전 매칭 좌표 보유 여부 (2026-07-08 최정우 추가)
	sint16							nPrevAltitude;						// 직전 매칭 성공 GPS 고도(m) — 연속 고도 앵커. NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE (고가/지하/교량 등)
	bool							bHasPrevAlt;						// 직전 고도 앵커 보유 — true일 때만 연속 맵매칭 고도 점수 적용
	double							dfLastMatchLinkPos;					// 직전 매칭 위치 — 링크 시작점부터 거리(m), 역행 페널티용 (2026-07-20 최정우 추가)
	bool							bHasPrevLinkPos;					// dfLastMatchLinkPos 보유 여부 (2026-07-20 최정우 추가)
	int								nReverseStreak;						// 연속 역행(bReverseSuspect) 포인트 수 — reverse_confirm 미만이면 SKIP·앵커 고정 (2026-07-21 최정우 추가)
	bool							bLastPointOk;						// 직전 처리 포인트가 정상 매칭(앵커 갱신)이었는지 — false 면 이상속도 검사 신뢰 못함 (2026-07-21 최정우 추가)

	char							szTripId[60+1];						// 현재 세션의 TRIP_ID — 신규 trip 감지(END/START 누락 대비) (2026-07-08 최정우 추가)
	char							szDeviceKey[32+1];					// 세션 키와 같은 DEVICE_KEY — 세션 안에서 행을 만들 때 필요
																//   (2026-09-22 최정우 추가, 커버리지 복구 행 생성용)

	int								nChargeSeq;							// 행 생성 시점의 **임시** 순번(1부터, 신규 trip 시작 시 리셋).
																// [2026-09-22 최정우 정정] 종전 주석의 "다음 PRIM_CHARGEHAND.trip_seq" 는
																//   **더 이상 맞지 않는다.** 워터마크 큐 도입 후 DB 에 실리는 TRIP_SEQ 는
																//   아래 nEmitSeq 가 **방출 시점에** 부여하며, 이 값은 그때 덮어써진다.
																//   지금은 행 생성 로그(seq=[..])의 표시값·PK 임시 채움 용도로만 남아 있어,
																//   **로그의 seq 와 DB 의 TRIP_SEQ 가 다를 수 있다**(추적 시 주의).

	// ── 과금 행 방출 대기 큐 (워터마크 방식, 2026-09-22 최정우 추가 — 사용자 확정 요구) ────────
	// [요구] PRIM_CHARGEHAND.TRIP_SEQ 는 ①1~N 연속(결번 금지) ②GPS_SEQ 순 ③**등록 후 변경 금지**
	//   세 가지를 동시에 만족해야 한다. ③ 때문에 종전의 사후 재부여([trip_seqoff]/[trip_seqfin])는
	//   쓸 수 없다 — 외부 과금서버가 60초 주기로 폴링하는데, 재부여는 PK(trip_id,device_key,trip_seq)
	//   자체를 UPDATE 하고 번호가 행끼리 **교환**되므로, 옛 번호로 마킹하면 실패가 아니라
	//   **그 번호를 차지한 다른 행이 조용히 갱신**된다.
	// [동작] 마감된 행을 곧바로 INSERT 하지 않고 여기에 담아두고, "그보다 앞선 GPS_SEQ 자리에
	//   더 들어올 행이 없다"가 확정(CalcChargeWatermark)된 것만 꺼내 그 자리에서 번호를 부여한다.
	//   꺼낼 때 비로소 번호를 주므로 미적재 행이 번호를 먹지 않아 **결번도 생기지 않는다**.
	//   큐는 트립 단위 상태 — ResetTripSessionForBegin(bFullReset) 에서 비운다(단 비우기 전에
	//   반드시 전량 방출할 것. 트립 전환·종료·TTL·서버종료 경로 모두 해당).
	//   배치 실패 시 세션이 커밋되지 않으므로 큐·nEmitSeq 도 함께 롤백되어 재처리 때 동일 결과가 된다.
	vector<CHARGE_INSERT_ROW>		vtPendingEmit;						// 방출 대기 행(삽입 순 유지 — 동점 GPS_SEQ 타이브레이크)

	// ── 링크 커버리지 (2026-09-22 최정우 추가 — 사용자 지시) ──────────────────────────────
	//   [주의] 도입 당시에는 계측 전용이었으나 **2단계 적용으로 판정이 바뀐다** —
	//   FillUncoveredLinkRows() 가 안 덮인 구간을 일반도로 거리로 복구한다.
	//   누락 링크 문제(면제·게이트 이월 미적용, 구간단속·주정차 이월 부재, 복구 span 한계 …)의
	//   공통 뿌리를 재는 계측이다. GPS 3초 간격이면 tick 이 하나도 안 찍힌 링크가 생기는데, 그
	//   복구를 유형별 경로에서 제각각 하다 보니 전이 조합마다 구멍이 남는다 — 조합을 가리지 않고
	//   "실제 지나간 링크 중 일반도로로 청구되지 않은 길이"를 트립 단위로 집계해 규모를 먼저 본다.
	//   [2026-09-22 2단계 적용됨] 측정만 하던 단계를 지나, 지금은 FillUncoveredLinkRows() 가
	//   안 덮인 구간을 일반도로 거리로 복구한다. 주정차 폴리곤 안 링크는 제외한다(확정 정책).
	//   [정정] 2026-10-04 E16(MM_PARK_SUPPRESS_NODESTEP=false) 이후 폴리곤 안 링크 제외는 꺼져 있다 —
	//   폴리곤 안 링크도 복구 대상이며, 제외는 트립 첫 tick 링크·타 과금유형 링크뿐이다
	//   (2026-10-06 최정우 주석 수정)
	// 이번 트립이 실제 경유한 링크 시퀀스(연속 중복 제거) — 링크와 함께 **그 tick 의 GPS_SEQ·시각**을
	//   들고 있어야 안 덮인 구간을 행으로 복구할 때 START/END_GPS_SEQ·OCCUR_DT 를 채울 수 있다.
	struct sTripPathLink
	{
		uint64						qwLinkID;
		uint32						dwGpsSeq;						// 이 링크를 지난 것으로 판단한 tick 의 GPS_SEQ
		time_t						dtGps;							// 그 tick 의 GPS 시각
		// [2026-09-23 최정우 추가] 그 tick 의 실측 속도(km/h) — **복구 행의 체류시간 산정**에 쓴다.
		//   tick 이 안 찍힌 링크를 복구할 때 시작·끝 seq 가 같아지면 체류시간이 0~1초로 고정되는데,
		//   거리는 링크 전체를 넣으므로 평균속도가 폭주한다(실측 102m/1초 = 369km/h).
		//   다음 경로 링크의 시각으로 못 메우는 경우(트립 마지막 구간) 이 값으로 역산한다.
		float						fSpeedKmh;
		sTripPathLink() : qwLinkID(0), dwGpsSeq(0), dtGps(0), fSpeedKmh(0.0f) {}
		sTripPathLink(uint64 qwL, uint32 dwS, time_t dtT, float fSpd = 0.0f)
			: qwLinkID(qwL), dwGpsSeq(dwS), dtGps(dtT), fSpeedKmh(fSpd) {}
	};
	vector<sTripPathLink>			vtTripPathLinks;
	double							dfChargedNodeStepM;					// 일반도로(type 0) 행으로 실제 청구한 거리 누적(m)
	// 일반도로 행들이 덮은 링크 전체 — 커버리지 대조용.
	//   [2026-09-22 최정우 정정] 종전엔 (FROM_ID, TO_ID) 쌍으로 경로 구간을 추정했는데, 행의
	//   FROM_ID 는 **경계 보정 결과**(gap-link corrected 등)라 경로 시퀀스(매칭 결과)에 없는
	//   경우가 많아 매칭이 통째로 실패했다 — 그래서 덮인 구간까지 "미덮임" 으로 세어져
	//   트립 78km 라는 과대치가 나왔다. 이제 행이 **자기가 덮은 링크를 직접 들고 오므로**
	//   추정이 필요 없다(CHARGE_INSERT_ROW::vtCoveredLinks).
	vector<uint64>					vtCoveredAll;						// 방출된 일반도로 행들의 덮은 링크 합
	int								nEmitSeq;							// 직전까지 방출한 TRIP_SEQ. 다음 행은 nEmitSeq+1
	// trip_seq 이어 매기기 — run() 이 배치 시작 때 읽어 둔 "그 트립의 DB 최대 TRIP_SEQ". ProcessRawLog() 가
	//   szSeedTripId 트립을 세션에 세팅할 때 nEmitSeq 에 반영하고 비운다 (2026-10-04 최정우 추가)
	char							szSeedTripId[60+1];
	int								nSeedEmitSeq;
	// 이미 방출된 **과금 대상(Y) 일반도로 행**의 GPS_SEQ 범위 — 그 안에 완전히 들어가는 N/3
	//   일반도로 행(SKIP 구간 브릿지)을 걸러내는 데 쓴다. 큐만 봐서는 먼저 나간 행을 못 찾는다.
	//   (2026-09-22 최정우 추가, 사용자 지시)
	vector<pair<uint32, uint32> >	vtEmittedNodeStepRanges;
	uint32							dwLastEmittedStartSeq;				// 직전까지 방출한 행의 최대 START_GPS_SEQ —
																//   커버리지 복구 행이 **이미 나간 행보다 앞 위치**에
																//   생기면 trip_seq 순이 gps_seq 순과 어긋나므로,
																//   그런 구간은 새 행으로 만들지 않는다 (2026-09-22 최정우 추가)

	// 폐쇄형 게이트 트랙 — 입구(I) 게이트 통과 후 출구(O) 게이트 통과 전까지 상태 유지.
	//   개방형(엣지 감지)과 달리 진입~진출 사이 "구간에 머무는 상태"를 실제로 들고 있어야 함 (2026-08-12 최정우 추가)
	bool							bInClosedRoad;						// true=입구 통과, 출구 대기 중
	char							szEntryTollgateId[20+1];				// 입구 게이트 TOLLGATE_ID
	char							szClosedRoadId[20+1];					// 진입한 폐쇄형 구역 road_id — 짝이 맞는 출구만 인정
	double							dfEntryFromLat;						// 입구 게이트 링크의 from_node 위도(입구 시점 캡처).
											//   단, 진입 자체가 애매(bClosedEntryAmbiguous)했으면
											//   대신 실제 첫 매칭 좌표를 담음 — 아래 참고
	double							dfEntryFromLon;						// 입구 게이트 링크의 from_node 경도(위와 동일 예외)
	time_t							dtEntryTime;							// 입구 통과 시각 — occur_dt 로 사용
	uint32							dwEntryGpsSeq;						// dtEntryTime 과 동일 tick 의 GPS_SEQ —
																		//   PRIM_CHARGEHAND.start_gps_seq 원본(폐쇄형) (2026-08-28 최정우 추가)
	// 트립이 이미 폐쇄형 도로 위에서 시작해(버그1, 입구 게이트 확정 못 함) 진입 게이트ID가
	//   비어있는 경우 — 출구가 나중에 확정되더라도 ZONE_INFO.dfLengthM(구역 전체 등록 길이)를
	//   쓰면 안 되고, 실제 출발 지점(dfEntryFromLat/Lon, 이 경우 링크 시작점이 아니라 실제
	//   매칭 좌표)~출구 게이트 간 실거리를 써야 함(사용자 지시, 2026-08-25 최정우 추가)
	// 게이트형 다중링크 구역 이탈 디바운스(config zone_exitcnt) 스트릭 — 구역 링크에서
	//   벗어난 tick 이 **연속으로** 몇 번 나왔는지. 구역 안 tick 이 한 번이라도 나오면 0 으로
	//   리셋된다. 종전에는 링크 2개 이상 구역이 무제한 대기라 실제 우회 주행까지 구역 체류로
	//   흡수됐다(실측 000984_20250903153702 — 진입 직후 90초·500m 우회 후 복귀).
	//   NODE_STEP 의 ZONE_RUN_SESSION.nExitTicks·PARKING 의 PARK_RUN_SESSION.nExitTicks 와
	//   같은 역할이며, 이쪽은 세션당 유형별로 하나씩만 필요해 세션 필드로 둔다
	//   (2026-09-21 최정우 추가)
	int								nClosedExitTicks;
	// 같은 링크 재진입 재발화 차단 — 진출이 확정된 "구역 road_id + 그 tick 의 매칭 링크".
	//   **그 링크를 벗어나면 해제**되므로 다음 통행의 정상 재진입은 막지 않는다.
	//   종전엔 위치 기반 가드(dfCurPosOnLink >= 출구게이트거리 - 3.0m)만 있었는데, 정차 중
	//   GPS 드리프트가 링크 진행거리를 그 임계 안팎으로 오가게 만들어 무력화됐다 — 실측
	//   000994_20250903152350 RL-Z00003: seq215 에 328.4m(임계 딱 도달)로 진출 확정 →
	//   seq216 에 드리프트로 311.4m 로 17m 후퇴 → 재진입 → seq220 링크 이탈로 재진출,
	//   그 3초짜리 재진입분에 구역 등록길이 331m 가 통째로 부과돼 398km/h(제한 20)가 됐다.
	//   2026-08-20 에 폐기된 "세션에 남기는 방식"은 트립 끝까지 막아버린 게 문제였는데,
	//   링크 단위로 키를 잡고 링크 이탈 시 해제하면 그 부작용이 없다 (2026-09-21 최정우 추가)
	uint64							qwClosedExitedLinkID;
	char							szClosedExitedRoadId[20+1];
	// true = 이 폐쇄형 run 은 **입구게이트 통과 근거가 없다**. 두 경로에서 선다 —
	//   ① 입구게이트 링크 위에서 게이트 지점을 이미 지난 채 트립이 시작된 경우(2026-08-25),
	//   ② 입구게이트 링크가 아닌 곳에서 구역에 올라탄 구역 중간 진입(2026-09-23).
	//   둘 다 szEntryTollgateId 를 비우므로 마감 시 N/3 + NCR_CLOSED_ENTRY_UNOBSERVED(21) 이 붙고,
	//   거리도 게이트가 아니라 실측 tick 부터 센다(진입 쪽 dfClosedAccumDistM=0, 이탈 쪽
	//   ApplyZoneExitTailDist 미적용) — 한쪽만 게이트 기준으로 늘리면 행 안에서 값이 어긋난다.
	bool							bClosedEntryAmbiguous;
	// 게이트를 못 찾고 구역을 나간 경우(매칭 링크가 이 구역 link_ids 를 벗어남)에 dist_m·
	//   speed_kmh·stay_seconds 를 0 대신 실측값으로 채우기 위한 실시간 위치·누적거리 추적
	//   (2026-08-25 최정우 추가 — 원래는 dist_m=ZONE_INFO.dfLengthM 고정값이라 불필요했으나,
	//   게이트 미확인 이탈 시엔 그 고정값을 쓸 근거가 없어 NODE_STEP과 동일한 실시간 누적 방식 도입)
	double							dfClosedLastX;						// 마지막 확인 매칭 위치 경도
	double							dfClosedLastY;						// 마지막 확인 매칭 위치 위도
	uint32							dwClosedLastGpsSeq;					// dfClosedLastX/Y 와 동일 tick 의 GPS_SEQ —
																		//   PRIM_CHARGEHAND.end_gps_seq 원본(TTL 강제마감 대체값)
																		//   (2026-08-28 최정우 추가)
	double							dfClosedAccumDistM;					// 진입 이후 누적 이동거리(m)
	// qwLastConfirmedLinkID 는 트립 시작 후보(A/B) 판정이 안 끝나면 0에 계속 머물러 있어(진입
	//   자체가 트립의 첫 확정 링크인 케이스, 실측 000376_20260819140856) 진출 판정의 "직전 링크"
	//   후보로 못 쓰는 경우가 있음 — 그 대체용으로 "이 구역 안에 있었던 마지막 링크/시각"을 별도
	//   추적(2026-08-25 최정우 추가, 사용자 지시 — "다음 맵매칭 좌표/링크로 진출 확인 가능")
	uint64							qwClosedLastZoneLinkID;					// 구역 안에서 마지막으로 확인된 링크 ID(0=없음)
	time_t							dtClosedLastZoneTime;					// 위 링크가 확인됐던 GPS 시각
	uint32							dwClosedLastZoneGpsSeq;					// dtClosedLastZoneTime 과 동일 tick 의 GPS_SEQ —
																			//   PRIM_CHARGEHAND.end_gps_seq 원본(정상 진출,
																			//   bExitOnPrevLink 케이스) (2026-08-28 최정우 추가)
	// [2026-09-15 최정우 추가] 구역 **안**에서 마지막으로 확인된 매칭 위치 — dfClosedLastX/Y 와
	//   구분해야 한다. 그쪽은 구역 밖에서도 매 tick 갱신되는 "다음 구간 측정 기준점"이라, 거리
	//   누적을 구역 내부로 게이팅한 뒤에는 TO_LAT/LON 으로 쓰면 거리·좌표가 서로 다른 구간을
	//   가리킨다. 면제도로(ProcessExemptZoneCharge)의 stRun.dfLastX/Y 와 같은 역할.
	double							dfClosedLastZoneX;						// 구역 안 마지막 확인 위치 경도
	double							dfClosedLastZoneY;						// 구역 안 마지막 확인 위치 위도
	// "방금 출구 처리한 링크/구역으로 즉시 재진입 방지" 가드는 더 이상 세션에 안 둠 — 세션에 두면
	//   트립이 끝날 때까지 안 풀려서 같은 구역 재통과(진짜 재진입)까지 막아버리는 버그였음. 실제로
	//   막아야 하는 범위는 "출구 처리 직후 같은 tick 안에서 바로 이어지는 입구 후보 검사"뿐이라
	//   ProcessClosedRoadCharge()/ProcessSpeedZoneCharge() 함수 호출 범위(지역변수)로 충분함
	//   (2026-08-20 최정우 수정 — qwClosedRoadJustExitedLinkID/szClosedRoadJustExitedRoadId 제거)

	// 구간단속 트랙 — 폐쇄형과 별도 독립 상태(같은 도로 위에 겹쳐 동시에 진행 가능하므로 공유 불가) (2026-08-12 최정우 추가)
	bool							bInSpeedZone;							// true=입구 통과, 출구 대기 중
	char							szSpeedZoneRoadId[20+1];				// 진입한 구간단속 구역 road_id
	time_t							dtSpeedEntryTime;						// 입구 통과 시각 — 평균속도·occur_dt 계산용
	uint32							dwSpeedEntryGpsSeq;						// dtSpeedEntryTime 과 동일 tick 의 GPS_SEQ —
																			//   PRIM_CHARGEHAND.start_gps_seq 원본(구간단속)
																			//   (2026-08-28 최정우 추가)
	char							szSpeedEntryTollgateId[20+1];			// 입구 게이트 TOLLGATE_ID — from_id/to_id·
											//   entry/exit_tollgate_id 게이트ID 기반으로 변경(2026-08-20
											//   최정우 수정, 사용자 지시 — 기존엔 구역 road_id만 쓰고
											//   게이트ID 자체는 안 남겼음)
	uint64							qwSpeedEntryLinkID;						// 입구 통과 시점 매칭 링크ID — NODE_STEP 일반도로 확장
											//   (구간단속 위반=SPEED+NODE_STEP 둘 다, 비위반=NODE_STEP만)
											//   레코드의 FROM_ID(링크ID 기반)용, 게이트ID(szSpeedEntryTollgateId)
											//   와 별도 보관 (2026-09-01 최정우 추가)
	// ProcessClosedRoadCharge() bClosedEntryAmbiguous 동일 근거 참고 — 구간단속은 원래 from/to_lat·lon 이
	//   구역 등록 폴리라인 첫/끝점(ZONE_INFO.dfFirstLat/Lon)이라 별도 실제 진입좌표 필드가 없었는데,
	//   진입 애매 시 실거리 계산에 필요해 신설(2026-08-25 최정우 추가)
	double							dfSpeedEntryFromLat;
	double							dfSpeedEntryFromLon;
	// 위 nClosedExitTicks 의 구간단속판 (2026-09-21 최정우 추가)
	int								nSpeedExitTicks;
	// 위 qwClosedExitedLinkID 의 구간단속판 (2026-09-21 최정우 추가)
	uint64							qwSpeedExitedLinkID;
	char							szSpeedExitedRoadId[20+1];
	bool							bSpeedEntryAmbiguous;
	// 폐쇄형과 동일 이유(2026-08-25 최정우 추가) — ProcessClosedRoadCharge() 필드 주석 참고
	double							dfSpeedLastX;
	double							dfSpeedLastY;
	uint32							dwSpeedLastGpsSeq;						// dfSpeedLastX/Y 와 동일 tick 의 GPS_SEQ —
																			//   PRIM_CHARGEHAND.end_gps_seq 원본(TTL 강제마감 대체값)
																			//   (2026-08-28 최정우 추가)
	double							dfSpeedAccumDistM;
	// 폐쇄형과 동일 이유(2026-08-25 최정우 추가) — qwClosedLastZoneLinkID 주석 참고
	uint64							qwSpeedLastZoneLinkID;
	time_t							dtSpeedLastZoneTime;
	uint32							dwSpeedLastZoneGpsSeq;					// dtSpeedLastZoneTime 과 동일 tick 의 GPS_SEQ —
																			//   PRIM_CHARGEHAND.end_gps_seq 원본(정상 진출,
																			//   bExitOnPrevLink 케이스) (2026-08-28 최정우 추가)
	// [2026-09-15 최정우 추가] 구역 **안** 마지막 확인 위치 — dfClosedLastZoneX/Y 와 동일 근거
	double							dfSpeedLastZoneX;						// 구역 안 마지막 확인 위치 경도
	double							dfSpeedLastZoneY;						// 구역 안 마지막 확인 위치 위도
	// qwSpeedZoneJustExitedLinkID/szSpeedZoneJustExitedRoadId 도 동일 이유로 제거(2026-08-20 최정우 수정) — 위 주석 참고

	// 주정차 트랙 — 게이트/구간단속과 별도 독립 상태(폐쇄형 고속도로 위 정차 등 동시 진행 가능).
	//   맵매칭 전 raw GPS 기준(다른 3종은 매칭 링크 기준)이라 세션 판단 재료도 raw 좌표·raw 속도.
	//   구역판정(위치)+SPEED_KMH(서행 구분)+체류시간만으로 판정 — DRIVE_STATUS 는 안 씀(엔진 on
	//   상태 정차 위반을 놓칠 위험), 위치반경 기반 별도 정지판정도 안 씀(SPEED_KMH로 충분,
	//   [[project_parking_match_pseudocode]] 2026-08-13 개정 참고) (2026-08-13 최정우 추가)
	//   [보완] 2026-08-22 부터 원시좌표에 더해 신뢰 매칭좌표도 본다(규칙 1~5). 속도 게이트
	//   (park_speedmax)는 기본 0=비활성이라 현재는 위치·체류시간만으로 판정한다. "다른 3종"은
	//   현재 다른 5개 유형이다 (2026-10-06 최정우 주석 추가)
	// 주정차 — 구역별 세션·후보 목록 (2026-08-23 최정우 수정)
	vector<PARK_RUN_SESSION>		vtParkRuns;
	vector<PARK_CANDIDATE>			vtParkCands;
	// 주정차 경계 통과 시각 보간용 "직전 GPS 원시 좌표/시각" — 다른 3종과 달리 매칭 링크가 아니라
	//   raw GPS 기준이라 dfLastMatchX/Y 를 못 쓴다. ProcessParkingCharge() 진입 시 매 틱 갱신 —
	//   새 구역 후보(PARK_CANDIDATE)가 열리는 순간 "그 직전엔 밖이었던 좌표"로 쓰인다. 트립의
	//   첫 틱은 bHasLastRawTick=false 라 보간을 건너뛰고 원시 GPS 시각을 그대로 쓴다
	//   (사용자 지시, 2026-08-24 최정우 추가)
	//   [주의] "트립의 첫 틱은 false" 는 **세션이 새로 만들어진 경우에만** 성립한다. 이 4필드(와 아래
	//   dfLastParkMatch* 4필드)는 ResetTripSessionForBegin() 에서 리셋되지 않으므로, 종료신호 없이
	//   같은 DEVICE_KEY 세션에서 트립이 바뀌면 새 트립 첫 tick 이 이전 트립 마지막 tick 과 보간된다
	//   (2026-10-06 최정우 주석 추가)
	double							dfLastRawTickX;
	double							dfLastRawTickY;
	time_t							dtLastRawTick;
	bool							bHasLastRawTick;
	// [2026-10-04 최정우 추가 — 정책 대조표 B9] 위 원시 tick 스냅샷의 매칭좌표판.
	//   ProcessParkingCharge() 가 매 tick 맨 앞에서 갱신한다(신뢰 매칭이 아니면 bHas=false — 보간은
	//   **바로 직전 tick** 과의 쌍이어야 하므로). 세션의 dfLastMatchX 는 1 tick 지연 커밋에서 갱신돼
	//   이 시점엔 두 tick 전 값일 수 있어 쓰지 않는다.
	double							dfLastParkMatchX;
	double							dfLastParkMatchY;
	time_t							dtLastParkMatch;
	bool							bHasLastParkMatch;

	// 같은 링크 노이즈 보정(1m 강제전진, MM_NOISE_FORWARD_NUDGE_M) 억제 판별용 "직전 tick 원시
	//   GPS 좌표·방향(heading)" — RunMapMatch() 가 매 tick 갱신한다. 좌표·방향이 직전 tick과
	//   완전히 같으면 실제로 정지해 있다는 뜻이라 강제전진 대신 세그먼트 매칭이 계산한 실제
	//   좌표를 그대로 쓴다(ContinueMapMatch.cpp 참고) (사용자 지시, 2026-09-02 최정우 추가)
	double							dfPrevTickRawX;
	double							dfPrevTickRawY;
	sint16							nPrevTickAngle;
	bool							bHasPrevTickRaw;

	// NODE_STEP 일반도로의 주정차 폴리곤 배제 판정용 디바운스 — PARKING 자체의 이탈 디바운스
	//   (park_exitcnt)와 동일 기준으로, 폴리곤 밖으로 짧게(park_exitcnt 미만) 튄 tick은 여전히
	//   "주정차 취급"으로 유지해 NODE_STEP이 그 사이만 별도 레코드로 표출하지 않게 한다
	//   (사용자 지시, 2026-09-02 최정우 추가 — 실측 000376_20260826150010 seq158~159, PARKING은
	//   디바운스로 세션을 안 끊는데 NODE_STEP은 디바운스 없이 즉시 표출하던 불일치)
	bool							bNodeStepParkTouch;
	int								nNodeStepParkExitTicks;
	// 위 디바운스 유예 중(park_exitcnt 미만) tick들의 매칭 위치를 임시로 쌓아두는 버퍼 — 이탈이
	//   "확정"되면 이 버퍼의 첫 tick을 새 NODE_STEP run의 진입점으로 소급 적용해, 유예 기간
	//   자체가 통째로 NODE_STEP에서 빠지는 것을 막는다. 확정 안 되고 폴리곤으로 복귀하면
	//   bHasParkTouchCarry=false 로 그냥 버려진다(노이즈는 여전히 안 남음)
	//   (2026-09-03 최정우 추가 — 실측 000376_20260826150010 seq126,127)
	bool							bHasParkTouchCarry;
	ZONE_RUN_SESSION				stParkTouchCarry;
	// 위 접촉 구간 동안 "매칭좌표" 기준으로 폴리곤 안쪽이 한 번이라도 확인됐는지 — 접촉이 확정
	//   이탈될 때, 한 번이라도 안쪽이 확인됐으면(진짜 주정차 접촉) 기존 그대로 소급 등록하고,
	//   한 번도 확인 안 됐으면(접근로만 스친 오검출) 이 접촉 구간 자체를 통째로 취소해 그 전후
	//   run을 끊김 없이 이어붙인다. 원시좌표가 아니라 매칭좌표 기준인 이유: 판교 실측
	//   000376_20260819094414 seq55,56은 원시좌표는 폴리곤 안이지만 매칭좌표는 계속 밖 —
	//   원시좌표 기준으로 판정하면 진짜 오검출 구간까지 "확정 접촉"으로 오분류된다
	//   (사용자 지시, 2026-09-03 최정우 추가)
	bool							bParkTouchEverMatchedInside;
	// ── 확정 접촉 이탈 시 "폴리곤 경계부터" 새 NODE_STEP run 을 여는 데 필요한 스냅샷
	//   (2026-09-05 최정우 추가, 사용자 지시) ──
	//   기존엔 확정 접촉이 이탈 확정되는 tick 부터 새 run 이 열려, 실제 폴리곤 경계와 그 tick
	//   사이(park_exitcnt 디바운스 구간)가 통째로 어느 레코드에도 안 들어갔다. 실측
	//   000376_20260821095239 — 경계는 seq41(안)~42(밖) 사이인데 레코드는 seq45 부터 시작해
	//   81.0m·10.3초가 누락됐다(디바운스 3틱 + 그중 seq44 가 클램프 저신뢰 SKIP 이라 과금
	//   함수가 호출되지 않아 확정이 한 틱 더 밀림). 진입 방향의 인수인계(handoff gap)와 대칭으로
	//   이탈 방향에도 누락 링크 복구 + 폴리곤 교차점 산출을 적용하기 위한 기준점들이다.
	uint64							qwParkTouchLastInLinkID;			// 원시좌표가 폴리곤 안이었던 마지막 tick 의 매칭 링크ID —
																		//   경계 탐색(FindLinkPathBounded)의 출발 링크. 디바운스 유예
																		//   tick(이미 폴리곤 밖)의 링크를 쓰면 출발점이 구역 밖이라
																		//   경계 자체를 못 찾는다
	double							dfParkTouchLastInX;					// 위 tick 의 매칭좌표·시각 — 경계 통과 시각 보간의 "안" 기준점
	double							dfParkTouchLastInY;
	time_t							dtParkTouchLastIn;
	uint32							dwParkTouchLastInGpsSeq;			// 그 tick 의 GPS_SEQ — 복구 링크만으로 구성된
																//   레코드의 순번을 고를 때 후보로 쓴다 (2026-09-07 최정우 추가, 사용자 지시)
	double							dfParkTouchAccumAtLastIn;			// 그 tick 까지의 stParkTouchCarry 누적거리 — 주행 중
																//   폴리곤을 빠져나올 때 폴리곤 안 구간을 "마지막 안쪽 tick 까지"만 일반도로로 넘길 때
																//   쓴다. 그 뒤 이탈 디바운스 tick 은 다른 과금유형 위일 수 있어
																//   (실측 시나리오 980007 seq15~16 면제) 넘기면 이중 계상된다
																//   (2026-10-04 최정우 추가, 정책 대조표 B2)
	double							dfParkTouchFirstOutX;				// 이탈 스트릭의 "첫" 밖 tick — 보간의 "밖" 기준점.
	double							dfParkTouchFirstOutY;				//   확정 tick 을 쓰면 이미 구역에서 한참 멀어진 지점과
	time_t							dtParkTouchFirstOut;				//   보간하게 된다(ZONE_RUN_SESSION dfFirstOut* 과 동일 문제)
	uint32							dwParkTouchFirstOutGpsSeq;
	// 경계 이탈 첫 tick 의 순간속도 — 폴리곤 경계~다음 구역 사이 구간은 GPS tick 이 없어
	//   경과시간을 못 재므로, 이 속도로 소요시간을 역산한다. 이탈 "확정" tick(park_exitcnt
	//   디바운스 뒤)의 속도를 쓰면 실제 통과 시점보다 한참 뒤 값이라 어긋난다 — 실측
	//   000376_20260821094609: 확정 tick 기준 9km/h 였으나 실제 통과 구간은 16~18km/h
	//   (2026-09-06 최정우 추가, 사용자 지시)
	float							fParkTouchFirstOutSpeed;

	// 게이트형 구역 진출 지점 이월 — 진출게이트가 어느 매칭 tick 좌표와
	//   사실상 같으면, 그 지점이 곧 구역의 끝이자 다음 일반도로의 시작이다. 다음 tick 부터
	//   일반도로를 열면 게이트~다음 tick 사이 구간이 어느 레코드에도 안 들어간다(실측
	//   000376_20260821094609: 진출게이트 TG00008 이 seq47 매칭점과 0.23m 인데 일반도로가
	//   seq48 부터라 약 20m 누락). 진입 쪽에서 "게이트 좌표 == 매칭 좌표면 순번을 공유한다"고
	//   정한 것과 대칭이다 (2026-09-06 최정우 추가, 사용자 지시)
	//   [2026-09-23 최정우 정정] 세팅하는 곳은 **개방식·폐쇄식·면제·구간단속 네 유형**이다
	//   (종전 주석의 "폐쇄식·구간단속" 은 처음부터 부정확했고, 구간단속은 2026-09-23 에 비로소
	//   합류했다 — ProcessSpeedZoneCharge 헤더 주석 참고).
	//   소비처도 둘로 늘었다: ① 다음 일반도로 run 이 진입 지점을 게이트로 당겨쓰는 것(원래 용도),
	//   ② 다음 tick 이 다른 과금유형이라 이월이 폐기될 때, 그 사이 **미등록 링크를 독립 일반도로
	//   행으로 건져내는 것**(ProcessNodeStepCharge 의 "gate exit orphan span recovered").
	//   ②는 이월이 **진출 직후(2 tick 이내)** 일 때만 도는데, 오래 방치된 이월로 복구하면 이미
	//   다른 유형이 점유한 구간을 일반도로로 청구하기 때문이다(실측 000994 seq440 이월이 seq493
	//   까지 53 tick 살아남아 124m 를 잘못 청구).
	bool							bHasGateExitCarry;
	double							dfGateExitX;
	double							dfGateExitY;
	time_t							dtGateExit;
	uint32							dwGateExitGpsSeq;
	uint64							qwGateExitLinkID;
	// 진출게이트가 그 tick 의 매칭 좌표와 사실상 일치하는지 — 일치하면 그 tick 을 앞 레코드와
	//   공유하고(START_GPS_SEQ = 그 tick), 아니면 게이트는 구역 안쪽이므로 일반도로의 첫 실측
	//   tick 은 그 다음이다. 실측: 000376_20260821094609 seq47 은 TG00008 과 0.23m 라 공유(47~65),
	//   000376_20260819140856 seq1 은 TG00008 에서 21m 앞이라 비공유(일반도로는 2부터)
	//   (2026-09-06 최정우 추가, 사용자 지시)
	bool							bGateExitAtTick;					// 위 블록 설명 참고 — 진출게이트가 그 tick 매칭
																		//   좌표와 2m 이내면 true(앞뒤 레코드가 순번 공유)
																		//   [2026-09-17 최정우 정정] 여기 붙어 있던
																		//   "새 run 의 start_gps_seq …" 문구는
																		//   dwParkTouchFirstOutGpsSeq 설명이 잘못 내려온
																		//   것이라 제자리로 옮김
	bool							bParkTouchHasFirstOut;				// 이탈 스트릭의 "첫 밖 tick" 을 이미 잡아뒀는지 —
																		//   폴리곤으로 복귀하면 false 로 되돌려 재수집한다.
																		//   경계는 "마지막 안"과 그 tick 사이에 있으므로
																		//   첫 밖 tick(dwParkTouchFirstOutGpsSeq)을 새 run 의
																		//   START_GPS_SEQ 로 쓴다
	// 일반도로 연속 구간 병합용 이월 — ProcessNodeStepCharge() 안에서 run이 닫혔는데 그 순간
	//   받아줄 다른 run이 아직 없으면 여기 잠깐 담아뒀다가, 다음에 새 run이 열릴 때 그대로
	//   이어받는다(2026-09-01 최정우 추가). [버그 수정, 2026-09-10 최정우] 원래 함수 지역변수
	//   였는데, "!bTrustedMatch면 새 run을 안 연다" 가드가 소비 지점보다 먼저 return 해버리면
	//   함수가 반환되는 순간 지역변수째로 소멸해 이월값이 유실됐다(실측 87.3m 규모, 최소
	//   재현으로 확인) — bHasGateExitCarry 와 동일하게 세션 필드로 승격해, 신뢰 못하는 tick을
	//   만나도 없어지지 않고 나중에 신뢰 가능한 tick이 올 때까지 남아있게 한다.
	bool							bHasMergeCarry;
	ZONE_RUN_SESSION				stMergeCarry;
	// 이월값이 설정된 tick 의 순번 — 아래 "타 과금유형 tick 무효화"가 방금 이번 tick 에 담긴
	//   이월값까지 곧바로 확정해버리지 않도록 구분하는 용도다(bHasGateExitCarry 쪽
	//   dwGateExitGpsSeq 와 같은 역할). 설정 지점이 무효화 지점보다 앞뒤로 흩어져 있어
	//   순서만으로는 같은 tick 설정분을 가려낼 수 없다 (2026-09-16 최정우 추가)
	uint32							dwMergeCarrySeq;
	// 접촉 중 이탈 디바운스(node_exitcnt)로 조기 마감된 run — 위 접촉 확정 판정이 나올 때까지
	//   즉시 등록하지 않고 보류한다. 확정 접촉이면 그대로 등록, 미확정이면 폐기하고 진입정보·
	//   누적거리를 접촉 구간의 이월값에 합쳐 다음 run으로 이어붙인다(위와 동일 근거,
	//   2026-09-03 최정우 추가 — 실측 000376_20260819094414 seq54 조기마감)
	bool							bHasHeldNodeStepRun;
	ZONE_RUN_SESSION				stHeldNodeStepRun;
	// 이번 접촉이 시작된 주정차 구역의 road_id — held run을 마감할 때 누락 링크가 이 구역
	//   폴리곤과 교차하는 지점까지의 부분 거리를 계산하는 데 쓴다(FindLinkPolygonCrossing).
	//   접촉 시작 시(bHasParkTouchCarry 새로 열릴 때) 채움 (사용자 지시, 2026-09-03 최정우 추가)
	char							szParkTouchZoneRoadId[20+1];
	// 이번 접촉에서 인수인계 구간(handoff gap) 탐색을 이미 시도했는지 — 접촉 시작 tick엔 아직
	//   링크가 안 바뀌어 있을 수 있어(구간단속 마지막 tick과 같은 링크) 매 tick 재시도하다가,
	//   qwLastConfirmedLinkID 가 실제로 바뀌는 tick에서 한 번만 시도하고 끈다(2026-09-03 최정우 추가)
	bool							bHasHandoffGapChecked;
	// 구간단속 마감 시 같이 등록되는 일반도로 미러 레코드 — 곧바로 등록하지 않고 잠깐 보류한다.
	//   구간단속 마지막 링크 바로 다음에 누락 링크를 거쳐 주정차 폴리곤에 닿는 경우(인수인계 구간,
	//   위 bHasHandoffGapChecked 로직 참고), 별도 레코드로 쪼개지 않고 이 미러에 합쳐서 하나로
	//   등록하기 위함 — 합칠 대상이 없으면(접촉이 없거나 인수인계 탐색이 실패하면) 원래 값 그대로
	//   등록한다(사용자 지시, 2026-09-03 최정우 추가)
	bool							bHasHeldSpeedMirrorRun;
	ZONE_RUN_SESSION				stHeldSpeedMirrorRun;
	// 미러를 보류한 tick 의 GPS_SEQ — 보류 직후 같은 tick 에서 곧바로 등록해버리면 다음 tick 의
	//   주정차 폴리곤 접촉이 이어받을 미러가 남지 않는다. 최소 1 tick 은 들고 있게 하는 표식
	//   (2026-09-06 최정우 추가, 사용자 지시)
	uint32							dwHeldSpeedMirrorSeq;

	// 면제도로 트랙(ROAD_KIND=5, 옛 이름 "비과금도로") — 게이트 없이 매칭 링크→구역 역인덱스로
	//   진입/이탈 판정 (2026-08-13 최초 추가, 2026-08-14 세 차례 재설계 — "모든 미등록 링크" 방식 →
	//   zone 기반 복귀 + charge_type="0" 통합 → 다시 charge_type="5" 고유값 + from_id/to_id를
	//   링크ID에서 zone 의 road_id로 원복(사용자 재지시). 진입~이탈 매칭 위치가 from/to_lat·lon)
	//   [2026-09-17 최정우 정정] 여기 있던 "정상 진행/정상 이탈 시에는 아무것도 INSERT 안 함 —
	//   TTL 만료 때만 1건 기록" 이라는 옛 설명을 삭제했다. 2026-08-30 부터 **정상 이탈·트립종료도
	//   Y/0 으로 매번 1건 기록**하며(ProcessExemptZoneCharge), 강제마감만 N/4(SKIP)다. 과금 제외는
	//   charge_yn 이 아니라 charge_type=5 로 구분한다.
	//   "무존" 상태가 exempt_regrace 초를 넘기면 확정 마감하고, 그 안에 원래 구역으로 복귀하면
	//   취소한다(재진입 유예, 2026-08-14 최정우 추가)
	// 면제도로 — 구역별 세션 목록 (2026-08-23 최정우 수정, 일반도로와 동일 구조)
	vector<ZONE_RUN_SESSION>		vtExemptRuns;

	// 일반도로 트랙(ROAD_KIND=0, NODE_STEP) — 비과금도로와 동일하게 게이트 없는 LINE 구조라
	//   매칭 링크→구역 역인덱스로 진입/이탈 판정. 단, 비과금도로와 달리 실제 과금 대상이라
	//   정상 이탈·트립종료 시에도 Y/0 으로 매번 1건 INSERT(사용자 지시, 2026-08-14 추가 —
	//   RL-Z00002 등 지정 구역 진입~이탈 누적거리 기준, 다른 유형과 겹쳐도 무조건 별도 부과)
	// 일반도로(NODE_STEP) — 구역별 세션 목록. 겹쳐 설정된 구역을 동시에 진행한다 (2026-08-23 최정우 수정)
	vector<ZONE_RUN_SESSION>		vtNodeStepRuns;

	// 개방형(ROAD_KIND=1) — 원래는 게이트 통과 순간의 점 이벤트였으나, 주행거리·주행시간을 함께
	//   적재하기 위해 일반도로/면제도로와 동일한 "구역 진입~이탈" 구조로 전환(사용자 지시,
	//   2026-08-25 추가). 게이트는 여전히 있고 이 진행(run) 동안 실제로 지났는지(bGateCrossed)가
	//   과금 유효성 판정에 쓰인다 — ZONE_RUN_SESSION 상단 주석 참고
	vector<ZONE_RUN_SESSION>		vtOpenRuns;

	// ── 1틱 지연 커밋 버퍼 — 반대편(짝) 링크 1틱 오매칭 보정용 (2026-08-21 최정우 추가) ──
	//   RunMapMatch 로 정상 매칭(bMatched && !bUntrustedMatch)된 행을 곧바로 과금 처리·DB
	//   반영하지 않고 1건 보류했다가, 바로 다음 GPS의 확정 링크가 "역행의심으로 튀기 전" 링크와
	//   같으면(=1틱만 반대편 짝 링크로 튀었다가 즉시 복귀) GPS 노이즈로 판단해 SKIP(미과금)으로
	//   보정한다. 맵매칭 엔진 자체의 연속매칭 앵커(qwLinkID/dfLastMatchLinkPos 등, RunMapMatch
	//   내부에서 실시간 갱신)는 전혀 건드리지 않음 — 오직 "과금 함수 호출 + rawgps_update 반영
	//   타이밍"만 1틱 늦춘다. 세션(디바이스)에 붙어있어 DB fetch 배치 경계를 넘어서도 유지됨.
	bool							bHasPendingCommit;
	RAW_LOG_INFO					stPendingRawLogInfo;					// 보류 행 원본 GPS 입력
	MATCH_LINK_INFO					stPendingMatchLinkInfo;					// 보류 행 매칭 결과
	sint16							nPendingFinalStatus;					// 보류 행의 확정 MATCH_STATUS(보정 전)
	int								nPendingIntersectLen;					// 보류 행 INTERSECT_LEN
	bool							bPendingHasCoords;						// 보류 행 MATCH_LAT/LON 저장 여부
	// 보류 행이 있는 상태에서 SKIP/ERROR 틱이 연속될 때 "몇 틱째 보류 연장 중인지" — 다음
	//   정상 매칭이 나올 때까지 SKIP 을 건너뛰고 계속 보류해 경로 일관성 보정 기회를 넓히되,
	//   MM_PENDING_MAX_HOLD_TICKS 를 넘으면 더는 기다리지 않고 그냥 확정한다(트립이 SKIP만
	//   계속되다 끝나는 경우 무한 보류 방지) (2026-08-26 최정우 추가)
	int								nPendingHoldTicks;
	uint64							qwLastConfirmedLinkID;					// 마지막으로 "신뢰 가능(과금 반영)"하게 커밋된 링크 ID(0=없음) — 보정판단 기준
	time_t							dtLastConfirmedLinkTime;				// qwLastConfirmedLinkID 가 확정됐던 GPS 시각 — 폐쇄형/구간단속
																			//   직전링크 이탈 출구판정(bExitOnPrevLink) 시 "실제로 그 링크에 마지막으로
																			//   있던 시각"으로 stay_seconds 를 계산하기 위함(현재 틱 시각을 쓰면 이미
																			//   다음 링크로 넘어온 뒤라 그만큼 과다계상됨) (2026-08-24 최정우 추가)
	uint32							dwLastConfirmedLinkGpsSeq;				// dtLastConfirmedLinkTime 과 동일 tick 의 GPS_SEQ — dwLastGpsSeq 는
																			//   매 tick(SKIP 포함) 갱신돼 SKIP 구간 동안 못 쓰므로 별도 보관.
																			//   NODE_STEP 케이스3(SKIP 구간 브릿지) start_gps_seq 원본
																			//   (2026-09-01 최정우 추가)
	float							fLastConfirmedLinkSpeed;				// dtLastConfirmedLinkTime 과 동일 tick 에 기기가 보고한 순간속도
																			//   (km/h, raw) — NODE_STEP 인수인계 구간(handoff gap)처럼 이 tick 을
																			//   유일한 실측 시각·앵커로 재사용하는 레코드는 dtEntryTime==dtEnd 라
																			//   dist/시간 평균속도 계산이 성립하지 않는다 — 그 대신 쓸 "그 tick
																			//   자체의" 실측 속도(현재 처리 중인 다른 tick 값을 쓰면 값이 새 버림,
																			//   실측 000376/000382 강릉 두 트립에서 확인) (2026-09-04 최정우 추가)
	// SKIP 갭 해소 tick 에 일반도로 run 이 흡수한 이동거리 — ProcessNodeStepCharge() 가 먼저 돌며
	//   "직전 확정위치 → 재매칭 위치" 직선거리를 run 에 통째로 더하고, 그 뒤 ResolveSkipGapNodeStep()
	//   이 같은 구간을 더 정확한 경로거리로 다시 구한다. 두 값이 그대로 남으면 같은 구간이 두 번
	//   기록되므로(이중 과금), 브릿지 쪽에서 이 값을 빼고 자기 값을 더해 "교체"한다
	//   (2026-09-15 최정우 추가, 사용자 지시)
	double							dfSkipGapTickDistM;						// 그 tick 에 run 에 더해진 직선거리(m)
	uint32							dwSkipGapTickGpsSeq;					// 그 tick 의 GPS_SEQ — 다른 tick 값 오용 방지
	uint32							dwSkipGapTickRunEntry;					// 더해진 run 의 dwEntryGpsSeq (run 식별자)
	// [2026-10-04 최정우 추가, 사용자 확정 — E18] 주정차 폴리곤 안 SKIP 좌표(RAW_VLD=false·정확도 초과·매칭 실패
	//   무관)의 마지막 GPS_SEQ. ProcessParkingCharge() 가 기록하고, ProcessNodeStepCharge() 는 run 마지막 tick 과
	//   현재 tick 사이에 이 순번이 있으면 run 을 끊는다(1틱 지연 확정과 순서가 어긋나도 안전하도록 순번으로 비교).
	uint32							dwSkipInParkSeq;						// 가장 최근 폴리곤 안 SKIP tick 의 GPS_SEQ(0=없음)
	uint32							dwSkipParkGapCutSeq;					// 그 표시로 run 을 끊은 tick 의 GPS_SEQ(SKIP 복구 차단용)
	// [2026-10-04 최정우 추가, 사용자 확정 — E19] 주정차 체류시간은 모든 GPS 수신 시각(gps_dt) 기준 — 정확도 초과
	//   (park_accmax) tick 은 위치 판정에는 쓰지 않되 출발·도착 시각에는 쓴다. 이를 위한 트립 단위 기록.
	uint32							dwParkTripFirstSeq;						// 이 트립에서 주정차 판정에 들어온 첫 tick 의 GPS_SEQ
	time_t							dtParkTripFirstTime;					//   그 tick 의 GPS 시각
	bool							bParkAccOkSeen;							// 이 트립에서 정확도 정상 tick 을 본 적이 있는지
	uint32							dwParkLastAccOkSeq;						// 정확도 정상인 마지막 tick 의 GPS_SEQ
	bool							bParkHeadNear;							// 트립 첫 정상 tick 앞의 정확도 초과 tick 들이 모두
																		//   "정확도 반경 안에 주정차 폴리곤이 있는" 위치였는지
	// 왕복분리 반대편 링크 N틱 연속 오매칭 보정용 (2026-08-24 최정우 추가, opp_streakmax 설정).
	//   스트릭이 시작될 때의 "진짜" 확정 링크를 앵커로 고정해두고(qwLastConfirmedLinkID 는 매 틱
	//   갱신되므로 별도 보관 필요), 스트릭 동안 pvtUpdates 에 커밋한 인덱스를 쌓아뒀다가 앵커로
	//   복귀가 확인되면 한꺼번에 SKIP 재기록한다(CommitPendingRow 참고).
	uint64							qwOppStreakAnchorLinkID;
	vector<size_t>					vtOppStreakUpdateIdx;
	// 같은 링크 ambiguous-reverse SKIP 브릿지 — bAmbiguousReverse 는 정의상 항상 세션 연속매칭
	//   앵커와 같은 링크에서만 발생(ContinueMapMatch.cpp qwPrevLinkID 비교 조건)하므로, 진행 중이던
	//   SKIP 런이 나중에 같은 링크로 확정 복귀하면 그 사이 SKIP 전부를 MATCHED로 소급 정정한다.
	//   링크가 실제로 바뀌면(진짜 이탈) 런을 버리고 SKIP 유지 — 위 vtOppStreakUpdateIdx 와 동일한
	//   "소급 재기록" 패턴(CommitPendingRow 참고). 좌표/링크는 원래 계산값 그대로라 다시 채울 필요
	//   없음, MATCH_STATUS만 바꿈. 과금은 애초에 이 tick들에 대해 호출된 적이 없어(신뢰 못하는
	//   매칭이라 과금 함수 자체를 안 태움) 소급 처리하지 않음 — DB 표시만 정정(사용자 지시,
	//   2026-08-28 최정우 추가)
	uint64							qwAmbigReverseRunLinkID;
	vector<size_t>					vtAmbigReverseRunIdx;
	// 경계 클램프(bClampLowConf) SKIP 브릿지 — 위 ambiguous-reverse 브릿지와 달리 이쪽은 "같은
	//   링크로 복귀"가 아니라 "실제로 인접(1-hop 직결) 링크로 넘어간" 경우다. 클램프는 짧은 링크
	//   끝에서 자주 발생하는데(신규 표준노드링크가 교차로를 여러 노드로 쪼개 전국 링크의 29%가
	//   15m 미만), 실시간 엔진의 후보 선택 자체를 바꾸면 왕복분리 등 다른 애매한 구간까지 건드려
	//   전국 단위로 더 큰 회귀가 남을 확인함(2026-08-28, hop 페널티 완화 2건 모두 검증 실패해 되돌림).
	//   대신 다음 확정 링크를 실제로 확인한 뒤 사후에만(CommitPendingRow) 재매칭하므로 실시간
	//   추적 앵커는 전혀 안 건드림 — RematchBeginBiased() 는 이미 검증된 기존 보정(BEGIN 반대방향
	//   오매칭)과 동일 메커니즘 재사용. 링크 자체가 바뀌므로 좌표/INTERSECT_LEN 도 재계산해서
	//   같이 재기록해야 해서, ambiguous-reverse 브릿지와 달리 원본 GPS 도 tick별로 보관해야 한다
	//   (사용자 지시, 2026-08-28 최정우 추가)
	uint64							qwClampRunLinkID;
	vector<size_t>					vtClampRunUpdateIdx;
	vector<RAW_LOG_INFO>			vtClampRunRawLogInfo;
	// 클램프 런이 "시작"되던 시점의 직전 신뢰 매칭 좌표(dfLastMatchX/Y) 스냅샷 — 노드가 직접
	//   안 이어지는(회전차로 등으로 실제론 연결됐지만 f_node/t_node 비교로는 안 잡히는) 경우까지
	//   구제하기 위해, "다음 확정 링크"로의 강제 재매칭 대신 전(前, 이 좌표)·후(後, 다음 확정
	//   매칭좌표) 확정 좌표를 이어 만든 실측 이동방향으로 클램프 런의 raw heading을 검증하는
	//   대안 경로에 쓴다 — 실측 000376_20260819094414 seq20(2040426801→2040426101, 노드 불일치로
	//   기존 인접 브릿지 실패)에서 이 방향(seq19→seq21 매칭좌표 bearing 220°)이 seq20의 raw
	//   heading(244°)과 24°차(임계 MM_CLAMP_HEADING_MAX_DIFF=30° 이내)로 검증 성공 확인
	//   (2026-09-04 최정우 추가, 사용자 지시)
	double							dfClampRunEntryX;
	double							dfClampRunEntryY;
	bool							bClampRunEntryValid;					// dfClampRunEntryX/Y 가 실좌표인지(트립 첫 tick부터
																			//   바로 클램프면 아직 신뢰 매칭 자체가 없어 무효)
	// 역행 의심(bReverseSkip, reverse_confirm 미만) SKIP 브릿지 — 클램프 브릿지와 동일 패턴이나
	//   "다음 확정 링크와 인접" 사전조건 대신, 스트릭이 reverse_confirm 미달로 끊기고 정상(비역행)
	//   매칭으로 복귀한 순간 자체가 이미 "노이즈였다"는 판정이므로, 그 시점(직전 확정 링크→복귀
	//   링크 방향검증 재매칭)에 소급 재기록한다. G40류 실측(000376_20260826150010) — 커브 구간에서
	//   위상적 역행 신호(후보 링크의 종료 노드가 직전 링크 종료 노드와 동일)가 1틱만 오탐한 경우를
	//   구제 (2026-09-04 최정우 추가, 사용자 지시)
	uint64							qwReverseSkipRunAnchorLinkID;
	vector<size_t>					vtReverseSkipRunUpdateIdx;
	vector<RAW_LOG_INFO>			vtReverseSkipRunRawLogInfo;
	// 완전 매칭실패(진짜 SKIP, bMatched==false) 구간 raw tick 버퍼 — 위 클램프 브릿지와 별개.
	//   NODE_STEP 일반도로 확장 케이스3(SKIP 구간 등록)용 — 클램프는 "매칭은 됐으나 저신뢰"라 매
	//   tick 마다 링크가 있지만, 완전 SKIP은 링크 자체가 없어 지금까지 아무 버퍼도 없었다. SKIP 런
	//   시작 시점의 qwLastConfirmedLinkID 를 앵커로 스냅샷해두고(런 도중 갱신되지 않게), 매 SKIP tick
	//   의 raw GPS 원본을 쌓아뒀다가, 다음 신뢰매칭이 확정되면(CommitPendingRow) RematchBeginBiased()
	//   로 재시도해 실제 매칭 링크를 복구 시도한다(1순위 fallback, 실패하면 그래프 경로탐색→직선거리
	//   순으로 대체) (2026-09-01 최정우 추가)
	uint64							qwSkipRunAnchorLinkID;
	vector<RAW_LOG_INFO>			vtSkipRunRawLogInfo;
	// vtSkipRunRawLogInfo 와 1:1 대응하는 pvtUpdates 인덱스 — ACCURACY_M SKIP 틱 개별 소급 MATCHED
	//   승격(RematchBeginBiasedDirectional)에서, 성공한 틱의 DB 갱신행을 직접 찾아 고쳐쓰는 용도.
	//   vtClampRunUpdateIdx 와 동일 패턴 (2026-09-04 최정우 추가)
	vector<size_t>					vtSkipRunUpdateIdx;
	// 트립 시작(또는 장시간 SKIP 후) 첫 매칭이 왕복분리 어느 쪽인지 불확실한 구간의 "경쟁 후보"
	//   추적용 (2026-08-24 최정우 추가, 실측 21트립 중 3건꼴로 재현 확인). qwLastConfirmedLinkID·
	//   qwOppStreakAnchorLinkID 가 둘 다 0(진짜 앵커도, 확정된 스트릭도 없음)일 때만 쓰인다 —
	//   위 반대편 스트릭 로직과 서로 배타적. A/B 중 더 오래 버틴 쪽이 승리하면 진 쪽만 DB 기록을
	//   SKIP 으로 재기록한다(CommitPendingRow 참고).
	uint64							qwStartCandLinkA;
	uint64							qwStartCandLinkB;
	vector<size_t>					vtStartCandIdxA;
	vector<size_t>					vtStartCandIdxB;
	// 후보 A/B 에 편입되느라 qwLastConfirmedLinkID 로 확정되지 못한 **마지막 tick** — 배치 종료 시
	//   후보를 비울 때 이 tick 을 앵커로 확정한다(run() 의 배치 종료 정리 참고). 실시간 수집에서는
	//   배치가 장치당 1~2 tick 이라 후보가 opp_streakmax 에 못 미친 채 매 배치 비워져,
	//   qwLastConfirmedLinkID 가 트립 내내 0 에 머물렀다(실측 000370_20261006133737 — 주행 13분
	//   뒤 RL-Z00005·RL-Z00003 진입이 "트립 시작"으로 오판돼 진입게이트 공란, 구간단속 위반 미적재)
	//   (2026-10-06 최정우 추가)
	uint64							qwStartCandLastLinkID;
	time_t							dtStartCandLast;
	uint32							dwStartCandLastGpsSeq;
	float							fStartCandLastSpeed;
	// 보류 행 처리 시점의 과금용 "직전 매칭 위치·시각" 스냅샷 — dfLastMatchX/Y 등은 RunMapMatch 가
	//   매 행마다 실시간으로 최신값으로 전진시키므로, 보류 행을 나중에 commit할 때는 그 당시(보류
	//   시점) 값을 써야 이동거리·속도가 정확함(그렇지 않으면 이미 몇 틱 지난 최신 위치를 "직전
	//   위치"로 오인해 이동거리가 잘못 계산됨)
	double							dfPendingPrevMatchX;
	double							dfPendingPrevMatchY;
	time_t							dtPendingPrevMatchGps;
	uint32							dwPendingPrevMatchGpsSeq;			// (2026-09-07 최정우 추가)
	bool							bPendingHadLastMatch;

	sVehicleTripSession() :
		qwLinkID(0),
		dtLastSeen(0),
		dwLastGpsSeq(0),
		bStartWarned(false),
		dfLastMatchX(0.0),	// (2026-07-08 최정우 추가)
		dfLastMatchY(0.0),	// (2026-07-08 최정우 추가)
		dtLastMatchGps(0),	// (2026-07-08 최정우 추가)
		dwLastMatchGpsSeq(0),	// (2026-09-07 최정우 추가)
		dtLastGpsEventTime(0),	// (2026-08-25 최정우 추가)
		bHasLastMatch(false),	// (2026-07-08 최정우 추가)
		nPrevAltitude(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		bHasPrevAlt(false),
		dfLastMatchLinkPos(0.0),	// (2026-07-20 최정우 추가)
		bHasPrevLinkPos(false),	// (2026-07-20 최정우 추가)
		nReverseStreak(0),	// (2026-07-21 최정우 추가)
		bLastPointOk(true),	// (2026-07-21 최정우 추가)
		nChargeSeq(1),	// (2026-08-12 최정우 추가)
		dfChargedNodeStepM(0.0),	// (2026-09-22 최정우 추가 — 선언 순서와 맞출 것, -Wreorder)
		nEmitSeq(0),	// (2026-09-22 최정우 추가)
		nSeedEmitSeq(0),	// (2026-10-04 최정우 추가)
		dwLastEmittedStartSeq(0),	// (2026-09-22 최정우 추가)
		bInClosedRoad(false),	// (2026-08-12 최정우 추가)
		dfEntryFromLat(0.0),	// (2026-08-12 최정우 추가)
		dfEntryFromLon(0.0),	// (2026-08-12 최정우 추가)
		dtEntryTime(0),	// (2026-08-12 최정우 추가)
		dwEntryGpsSeq(0),	// (2026-08-28 최정우 추가)
		nClosedExitTicks(0),			// (2026-09-21 최정우 추가)
		qwClosedExitedLinkID(0),		// (2026-09-21 최정우 추가)
		bClosedEntryAmbiguous(false),	// (2026-08-25 최정우 추가)
		dfClosedLastX(0.0),	// (2026-08-25 최정우 추가)
		dfClosedLastY(0.0),	// (2026-08-25 최정우 추가)
		dwClosedLastGpsSeq(0),	// (2026-08-28 최정우 추가)
		dfClosedAccumDistM(0.0),	// (2026-08-25 최정우 추가)
		qwClosedLastZoneLinkID(0),	// (2026-08-25 최정우 추가)
		dtClosedLastZoneTime(0),	// (2026-08-25 최정우 추가)
		dwClosedLastZoneGpsSeq(0),
		dfClosedLastZoneX(0.0),	// (2026-09-15 최정우 추가)
		dfClosedLastZoneY(0.0),	// (2026-09-15 최정우 추가)
		bInSpeedZone(false),	// (2026-08-12 최정우 추가)
		dtSpeedEntryTime(0),	// (2026-08-12 최정우 추가)
		dwSpeedEntryGpsSeq(0),	// (2026-08-28 최정우 추가)
		qwSpeedEntryLinkID(0),	// (2026-09-01 최정우 추가)
		dfSpeedEntryFromLat(0.0),	// (2026-08-25 최정우 추가)
		dfSpeedEntryFromLon(0.0),	// (2026-08-25 최정우 추가)
		nSpeedExitTicks(0),				// (2026-09-21 최정우 추가)
		qwSpeedExitedLinkID(0),			// (2026-09-21 최정우 추가)
		bSpeedEntryAmbiguous(false),	// (2026-08-25 최정우 추가)
		dfSpeedLastX(0.0),	// (2026-08-25 최정우 추가)
		dfSpeedLastY(0.0),	// (2026-08-25 최정우 추가)
		dwSpeedLastGpsSeq(0),	// (2026-08-28 최정우 추가)
		dfSpeedAccumDistM(0.0),	// (2026-08-25 최정우 추가)
		qwSpeedLastZoneLinkID(0),	// (2026-08-25 최정우 추가)
		dtSpeedLastZoneTime(0),	// (2026-08-25 최정우 추가)
		dwSpeedLastZoneGpsSeq(0),
		dfSpeedLastZoneX(0.0),	// (2026-09-15 최정우 추가)
		dfSpeedLastZoneY(0.0),	// (2026-09-15 최정우 추가)
		dfLastRawTickX(0.0),	// (2026-08-24 최정우 추가)
		dfLastRawTickY(0.0),	// (2026-08-24 최정우 추가)
		dtLastRawTick(0),	// (2026-08-24 최정우 추가)
		bHasLastRawTick(false),	// (2026-08-24 최정우 추가)
		dfLastParkMatchX(0.0),	// (2026-10-04 최정우 추가)
		dfLastParkMatchY(0.0),	// (2026-10-04 최정우 추가)
		dtLastParkMatch(0),	// (2026-10-04 최정우 추가)
		bHasLastParkMatch(false),	// (2026-10-04 최정우 추가)
		dfPrevTickRawX(0.0),	// (2026-09-02 최정우 추가)
		dfPrevTickRawY(0.0),	// (2026-09-02 최정우 추가)
		nPrevTickAngle(-1),	// (2026-09-02 최정우 추가)
		bHasPrevTickRaw(false),	// (2026-09-02 최정우 추가)
		bNodeStepParkTouch(false),	// (2026-09-02 최정우 추가)
		nNodeStepParkExitTicks(0),	// (2026-09-02 최정우 추가)
		bHasParkTouchCarry(false),	// (2026-09-03 최정우 추가)
		bParkTouchEverMatchedInside(false),	// (2026-09-03 최정우 추가)
		// 아래 ParkTouch~/GateExit~ 블록은 위 struct 멤버 선언 순서와 똑같은 순서로 나열해야 한다 —
		//   생성자 초기화 리스트는 선언 순서대로 실행되므로, 여기 순서가 어긋나면 실제 실행 순서와
		//   달라 보여 의도와 다르게 읽히고 -Wreorder 컴파일 경고도 발생한다(2026-09-10 최정우 수정 —
		//   이전엔 fParkTouchFirstOutSpeed~bGateExitAtTick 블록이 qwParkTouchLastInLinkID~
		//   dwParkTouchFirstOutGpsSeq 블록보다 앞에 있어 선언 순서와 반대였음)
		qwParkTouchLastInLinkID(0),	// (2026-09-05 최정우 추가)
		dfParkTouchLastInX(0.0),	// (2026-09-05 최정우 추가)
		dfParkTouchLastInY(0.0),	// (2026-09-05 최정우 추가)
		dtParkTouchLastIn(0),	// (2026-09-05 최정우 추가)
		dwParkTouchLastInGpsSeq(0),	// (2026-09-07 최정우 추가)
		dfParkTouchAccumAtLastIn(0.0),	// (2026-10-04 최정우 추가)
		dfParkTouchFirstOutX(0.0),	// (2026-09-05 최정우 추가)
		dfParkTouchFirstOutY(0.0),	// (2026-09-05 최정우 추가)
		dtParkTouchFirstOut(0),	// (2026-09-05 최정우 추가)
		dwParkTouchFirstOutGpsSeq(0),	// (2026-09-05 최정우 추가)
		fParkTouchFirstOutSpeed(-1.0f),
		bHasGateExitCarry(false),
		dfGateExitX(0.0),
		dfGateExitY(0.0),
		dtGateExit(0),
		dwGateExitGpsSeq(0),
		qwGateExitLinkID(0),
		bGateExitAtTick(false),
		bParkTouchHasFirstOut(false),	// (2026-09-05 최정우 추가)
		bHasMergeCarry(false),	// (2026-09-10 최정우 추가)
		dwMergeCarrySeq(0),		// [버그 수정, 2026-09-17 최정우] 초기화 누락 — 짝인 bHasMergeCarry 와
								//   같은 역할의 dwGateExitGpsSeq 는 0 으로 초기화되는데 이 필드만 빠져
								//   있었다. 현재는 bHasMergeCarry=true 인 지점에서 항상 함께 설정돼
								//   실동작 영향은 없었으나, 미초기화 값을 비교에 쓰는 구조라 위험하다
		bHasHeldNodeStepRun(false),	// (2026-09-03 최정우 추가)
		bHasHandoffGapChecked(false),	// (2026-09-03 최정우 추가)
		bHasHeldSpeedMirrorRun(false),	// (2026-09-03 최정우 추가)
		dwHeldSpeedMirrorSeq(0),	// (2026-09-06 최정우 추가)
		bHasPendingCommit(false),	// (2026-08-21 최정우 추가)
		nPendingFinalStatus(MATCH_STATUS_PENDING),	// (2026-08-21 최정우 추가)
		nPendingIntersectLen(-1),	// (2026-08-21 최정우 추가)
		bPendingHasCoords(false),	// (2026-08-21 최정우 추가)
		nPendingHoldTicks(0),	// (2026-08-26 최정우 추가)
		qwLastConfirmedLinkID(0),	// (2026-08-21 최정우 추가)
		dtLastConfirmedLinkTime(0),	// (2026-08-24 최정우 추가)
		dwLastConfirmedLinkGpsSeq(0),	// (2026-09-01 최정우 추가)
		fLastConfirmedLinkSpeed(0.0f),	// (2026-09-04 최정우 추가)
		dfSkipGapTickDistM(0.0),	// (2026-09-15 최정우 추가)
		dwSkipGapTickGpsSeq(0),	// (2026-09-15 최정우 추가)
		dwSkipGapTickRunEntry(0),	// (2026-09-15 최정우 추가)
		dwSkipInParkSeq(0), dwSkipParkGapCutSeq(0),	// (2026-10-04 최정우 추가 — E18)
		dwParkTripFirstSeq(0), dtParkTripFirstTime(0), bParkAccOkSeen(false), dwParkLastAccOkSeq(0), bParkHeadNear(true),	// (E19)
		qwOppStreakAnchorLinkID(0),	// (2026-08-24 최정우 추가)
		qwAmbigReverseRunLinkID(0),	// (2026-08-28 최정우 추가)
		qwClampRunLinkID(0),	// (2026-08-28 최정우 추가)
		dfClampRunEntryX(0.0),	// (2026-09-04 최정우 추가)
		dfClampRunEntryY(0.0),	// (2026-09-04 최정우 추가)
		bClampRunEntryValid(false),	// (2026-09-04 최정우 추가)
		qwReverseSkipRunAnchorLinkID(0),	// (2026-09-04 최정우 추가)
		qwSkipRunAnchorLinkID(0),	// (2026-09-01 최정우 추가)
		qwStartCandLinkA(0),	// (2026-08-24 최정우 추가)
		qwStartCandLinkB(0),	// (2026-08-24 최정우 추가)
		qwStartCandLastLinkID(0), dtStartCandLast(0), dwStartCandLastGpsSeq(0), fStartCandLastSpeed(0.0f),	// (2026-10-06 최정우 추가)
		dfPendingPrevMatchX(0.0),	// (2026-08-21 최정우 추가)
		dfPendingPrevMatchY(0.0),	// (2026-08-21 최정우 추가)
		dtPendingPrevMatchGps(0),	// (2026-08-21 최정우 추가)
		dwPendingPrevMatchGpsSeq(0),	// (2026-09-07 최정우 추가)
		bPendingHadLastMatch(false)	// (2026-08-21 최정우 추가)
	{
		szTripId[0] = '\0';									// (2026-07-08 최정우 추가)
		szDeviceKey[0] = '\0';								// (2026-09-22 최정우 추가)
		szEntryTollgateId[0] = '\0';							// (2026-08-12 최정우 추가)
		szClosedRoadId[0] = '\0';							// (2026-08-12 최정우 추가)
		szClosedExitedRoadId[0] = '\0';					// (2026-09-21 최정우 추가)
		szSpeedExitedRoadId[0] = '\0';					// (2026-09-21 최정우 추가)
		szSpeedZoneRoadId[0] = '\0';							// (2026-08-12 최정우 추가)
		szSpeedEntryTollgateId[0] = '\0';						// (2026-08-20 최정우 추가)
		szParkTouchZoneRoadId[0] = '\0';						// (2026-09-03 최정우 추가)
		memset(reinterpret_cast<void *>(&stPendingRawLogInfo), 0, RAW_LOG_INFO_SIZE);		// (2026-08-21 최정우 추가)
		memset(reinterpret_cast<void *>(&stPendingMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);	// (2026-08-21 최정우 추가)
	}
} VEHICLE_TRIP_SESSION, *PVEHICLE_TRIP_SESSION;

/**
 * @struct sRawLogUpdateRow
 * @brief rawgps_update 1행 파라미터 (배치 종료 시 일괄 UPDATE)
 * @remark [rawgps_update] $3=match_status:
 *   - 1(MATCHED) / 3(SKIP) / 4(ERROR) : 맵매칭 정상 완료
 *   - 0(PENDING) : bulk 실패 시 예약 해제(release). $4~$6 은 '' 로 MATCH_*·INTERSECT_LEN 미갱신
*/
typedef struct sRawLogUpdateRow
{
	string							strTripId;
	string							strGpsSeq;
	string							strMatchStatus;
	string							strIntersectLen;					// INTERSECT_LEN: GPS↔세그먼트 교차점 거리(m)
	string							strMatchLat;
	string							strMatchLon;
	string							strMatchLinkId;						// 맵매칭 링크 ID (MATCH_LINK_ID) (2026-07-15 최정우 추가)
} RAW_LOG_UPDATE_ROW, *PRAW_LOG_UPDATE_ROW;


/**
 * @struct sTripEndUpdateRow
 * @brief [trip_end] bulk UPDATE 1행 — 트립 종료 시 그 trip_id 의 PRIM_CHARGEHAND 전 행에
 *        trip_end_dt 반영 (2026-08-12 최정우 추가)
 * @remark 과금 INSERT 는 게이트 통과 "즉시"(트립 종료 전) 발생하므로 trip_end_dt 는 그 시점에
 *   알 수 없음 — 트립이 실제로 끝나는 시점(TRIP_EVENT=2)에 별도 UPDATE 로 채운다.
 *   실측(59.11.91.162)은 INSERT 자체를 트립종료 시점에 하는 방식이라 reg_dt=upd_dt=trip_end_dt가
 *   항상 같았지만, 이 구현은 즉시 INSERT 방식을 유지하고 trip_end_dt 만 나중에 UPDATE 하므로
 *   reg_dt(최초 INSERT 시각)와 upd_dt(이 UPDATE 시각)가 달라질 수 있음 — 의도된 차이.
 * @remark [보완] 현재 과금 행은 "즉시"가 아니라 워터마크 큐(vtPendingEmit)에서 방출될 때 INSERT 된다.
 *   이 구조체는 정상 종료(TRIP_EVENT=END)뿐 아니라 TTL·종료신호 없는 트립 전환·서버 종료 마감
 *   (FlushOpenRunsAsAbnormalEnd 의 pvtAbnormalEndUpdates)에도 쓰이며, 둘 다 같은 [trip_end] SQL 이다
 *   ([trip_abend] 는 2026-10-04 삭제). 정상 종료분은 과금 INSERT 와 같은 배치 트랜잭션에서 실행된다
 *   (2026-10-06 최정우 주석 추가)
*/
typedef struct sTripEndUpdateRow
{
	string							strTripId;
	string							strTripEndDt;						// YYYYMMDDHH24MISS — END GPS 의 실제 수신 시각
																		//   강제마감 경로는 마감 기준 시각(TTL=그 트립 마지막 GPS 시각
																		//   dtLastGpsEventTime, 없으면 dtLastSeen) (2026-10-06 최정우 주석 추가)
	string							strUpdDt;							// 이 UPDATE 실행 시각
} TRIP_END_UPDATE_ROW, *PTRIP_END_UPDATE_ROW;

/**
 * @struct sRawLogWorkerConfig
 * @brief 워커 공유 설정
*/
typedef struct sRawLogWorkerConfig
{
	CPostgrePool					*pcPostgrePool;
	CProcessManager					*pcProcessManager;
	CChargeDataLoader				*pcChargeDataLoader;				// 게이트·구역 캐시 — 개방형 과금 판정용(nullptr=과금 비활성) (2026-08-12 최정우 추가)
																		//   [보완] 현재는 6개 과금유형 전부의 판정용(게이트·구역·주정차
																		//   폴리곤·과태료 최소시간) (2026-10-06 최정우 주석 추가)
	CDataLoader						*pcDataLoader;						// 형상정보(LINK_INFO.qwOppositeLinkID 조회) — 반대편 짝 링크 1틱 오매칭 보정용 (2026-08-21 최정우 추가)
	string							strUpdateSQL;						// [rawgps_update] 완료(1/3/4) 및 release(0) 공용
	string							strChargeInsertSQL;					// [charge_insert] 개방형 게이트 통과 bulk INSERT (비어있으면 비활성) (2026-08-12 최정우 수정)
																		//   [보완] 현재는 6개 CHARGE_TYPE 공용 INSERT (2026-10-06 최정우 주석 추가)
	string							strTripEndUpdateSQL;				// [trip_end] 트립 종료 시 trip_end_dt UPDATE (비어있으면 비활성) (2026-08-12 최정우 추가)
	string							strTripSeqMaxSQL;					// [trip_seqmax] 트립의 기존 최대 TRIP_SEQ 조회(읽기 전용, 비어있으면 이어 매기기 비활성) (2026-10-04 최정우, 사용자 확정 — TTL 마감 권장안)
	int								nWorkerThreads;
	int								nTtlSec;							// trip_id 세션 유지 시간 (초, 0=비활성)
																		//   [정정] 세션 키는 DEVICE_KEY — 마지막 처리(dtLastSeen, 벽시계)
																		//   후 이 시간이 지나면 마감 (2026-10-06 최정우 주석 수정)
	int								nMatchTimeoutMs;					// 1 GPS 맵매칭 처리 임계 (ms, 초과 시 ERROR 격리, 0=비활성)
	int								nRetryMax;							// release→PENDING 재시도 상한. 초과 시 ERROR(4) 고정. 0=무제한
	int								nConnRetryMax;						// [database] retrymax — 풀 연결 핸들 확보 재시도 최대 횟수 (회, 2026-07-10 최정우 추가)
	int								nConnRetryWait;						// [database] retrywait — 재시도 사이 대기 (ms, 2026-07-10 최정우 추가)
	int								nRadiusSkip;						// config radius_skip — ACCURACY_M 초과 시 SKIP (m). 0=비활성 (2026-07-08 최정우)
	int								nHeadingMaxDist;					// (단위: m) 연속매칭 heading 계산 이동거리 상한. 초과 시 heading 미사용, 0=비활성 ([mapmatch] distance) (2026-07-15 최정우 추가)
	double							dfSpeedFactor;						// config speed_factor — 이동거리 환산속도/SPEED_KMH 배율 상한. 0=비활성 (2026-07-20 최정우 추가)
	int								nSpeedMargin;						// config speed_margin (km/h) — 노이즈 허용 여유분 (2026-07-20 최정우 추가)
	int								nReverseConfirm;					// config reverse_confirm — 연속 역행 확정 포인트 수 (2026-07-21 최정우 추가)
	int								nOppStreakMax;						// config opp_streakmax — 왕복분리 반대편 링크 연속 오매칭 허용 틱 수 (2026-08-24 최정우 추가)
	int								nParkPad;							// config park_pad — 구역판정 시 폴리곤 바깥 확장 허용거리(m) (2026-08-13 최정우 추가, 2026-09-03 park_buf 에서 개명)
	int								nIgnoreRawVld;						// config ignore_rawvld — RAW_VLD 무시 전량 매칭(검증용) (2026-08-23 최정우 추가)
	int								nParkAccMax;						// config park_accmax — 주정차 판정 좌표 정확도 상한(m), 0=비활성 (2026-08-23 최정우 추가)
	int								nParkExitCnt;						// config park_exitcnt — 구역 이탈 확정 연속 GPS 건수(디바운스) (2026-08-13 최정우 추가)
	int								nZoneExitCnt;						// config zone_exitcnt — 게이트형 다중링크 구역 이탈 확정 연속 GPS 건수(디바운스), 0=비활성 (2026-09-21 최정우 추가)
	int								nNodeExitCnt;						// config node_exitcnt — 일반도로(NODE_STEP) 이탈 확정 연속 GPS 건수(디바운스) (2026-08-24 최정우 추가)
	int								nParkSpeedMax;						// config park_speedmax — 주정차 판정 속도 상한(km/h) (2026-08-22 최정우 추가)
	int								nParkEntryCnt;						// config park_entrycnt — 세션 개시 연속 GPS 건수 (2026-08-22 최정우 추가)
	int								nParkTtlSec;						// config park_ttl — 마지막 신뢰 확인 후 강제 마감까지의 시간(초) (2026-08-19 최정우 추가)
	int								nExemptRegraceSec;					// config exempt_regrace — 재진입 유예시간(초) (2026-08-14 최정우 추가)
	int								nExemptOutMax;						// config exempt_outmax — 이 거리(m) 이상 구역 밖을 주행하면 재진입 유예 무효 (2026-09-23 최정우 추가)
} RAWLOG_WORKER_CONFIG, *PRAWLOG_WORKER_CONFIG;

/**
 * @class CRawLogWorker
 * @brief ThreadPool Runnable – trip_id batch 처리
*/
class CRawLogWorker : public virtual Runnable
{
public:
	CRawLogWorker();
	virtual ~CRawLogWorker();

	void SetConfig(const RAWLOG_WORKER_CONFIG& stConfig);

	// 진행 중이던 모든 과금 구간을 "비정상 종료"로 마감 — 3경로 공용:
	//   (1) TTL 만료  (2) 트립종료 tick 이후 잔여 tick  (3) 종료신호 없는 트립 전환
	//   호출 시점에 stSession.szTripId 가 아직 "마감할 트립" 이어야 한다 (2026-09-15 최정우 추가)
	void FlushOpenRunsAsAbnormalEnd(int nThreadId, const string& strDeviceKey,
		VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, uint32 dwEndGpsSeq, time_t dtNow,
		vector<CHARGE_INSERT_ROW> *pvtOut,
		vector<TRIP_END_UPDATE_ROW> *pvtAbnormalEndUpdates,
		bool bNoTripEnd = false);
	//   bNoTripEnd: true = 종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
	//     non_charge_reason 을 62(면제도로는 52)로 남긴다. false = TTL·잔여tick·트립종료
	//     (61/51). 판정(charge_yn/status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
	// #6: dtLastSeen 경과 세션 제거 (모니터 주기 호출). pcConn 은 TTL 만료 시점에 열려 있는 주정차
	//   세션을 즉시 위반 INSERT 하는 데 씀(2026-08-13 최정우 추가)
	//   [정정] 모니터 스레드가 아니라 워커 자기 스레드에서 호출된다 — run() 배치 끝, idle()(유휴 대기
	//   상한 경과), FlushAllSessionsOnShutdown()(bForceAll=true). pcConn 은 주정차뿐 아니라 열린 전
	//   유형 마감 행 INSERT·[trip_end] UPDATE 트랜잭션에 쓴다 (2026-10-06 최정우 주석 수정)
	int ExpireTtlSessions(int nThreadId, int nTtlSec, PGconn *pcConn, bool bForceAll = false);
	// 서버 종료 시 모든 워커 슬롯의 열린 과금 구간을 마감한다 — 세션은 in-memory 라 재기동 시
	//   그대로 사라지므로(ExpireTtlSessions 주석의 "별도 한계"), 종료 직전에 마감해 기록으로
	//   남긴다. **워커가 전부 멈춘 뒤에만 호출할 것**(락 없이 세션 맵에 접근한다)
	//   (2026-09-16 최정우 추가)
	int FlushAllSessionsOnShutdown();
	// #7/#8: 예약(batch) PROCESSING→PENDING release
	bool ReleaseReservedBatch(PGconn *pcConn, const RAW_LOG_BATCH& vtBatch, int nThreadId);

	virtual void run(int nThreadId, void *context);
	virtual void stop(int nThreadId, void *context);
	// 일감 없이 대기 상한이 지났을 때 워커 스레드에서 호출 — 자기 세션의 TTL 검사 (2026-10-04 최정우 추가)
	virtual void idle(int nThreadId);

private:
	// pstSession: 배치 임시 세션(in-memory). bulk 성공 후에만 m_vtTripSessions 에 반영
	bool ProcessRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		vector<TRIP_END_UPDATE_ROW> *pvtTripEndUpdates,
		VEHICLE_TRIP_SESSION *pstSession, bool *pbTripEnded);
	bool RunMapMatch(int nThreadId, const sRawLogInfo& stRawLogInfo, VEHICLE_TRIP_SESSION *pstSession,
		MATCH_LINK_INFO *pstMatchLinkInfo);
	// 개방형(ROAD_KIND=1) 구역 진입/이탈 판정 — 원래는 게이트 통과 순간의 점 이벤트였으나, 주행
	//   거리(dist_m)·주행시간(stay_seconds)을 함께 적재하기 위해 일반도로(NODE_STEP)와 동일한
	//   "구역 진입~이탈" 구조로 전환(사용자 지시, 2026-08-25 재작성). 매칭 링크→구역 역인덱스로
	//   진입/이탈 판정. 다만 게이트는 여전히 존재하고 이 진행(run) 동안 실제로 지났는지가
	//   과금 유효성(charge_yn/status) 판정에 쓰인다:
	//     - 트립이 구역 밖에서 정상 진입한 run → dist_m=구역 전체길이(ZONE_INFO.dfLengthM 고정값),
	//       charge_yn/status=Y/0 고정(개방형 구역을 물리적으로 통과하려면 게이트를 지날 수밖에 없음)
	//     - 트립 자체가 이 구역 도로 위에서 시작한 run(bStartedByTrip=true) → dist_m=출발좌표~이탈
	//       실관측 누적거리(dfAccumDistM), charge_yn/status는 이 run 중 게이트를 실제로 지났으면
	//       Y/0, 이미 게이트를 지난 뒤 시작이라 못 지났으면 N/3(AUDIT)
	// bTrustedTripEnd — ProcessRawLog() 가 gps_dt 역전(스퓨리어스 END) 검사까지 마친 뒤 넘겨주는
	//   값. 이 값으로 내부 bTripEnding 을 결정해야 스퓨리어스 END 로 세션이 강제마감되는 걸 막을
	//   수 있다 — stRawLogInfo.nTripEvent 를 직접 보면 안 됨 (2026-08-25 최정우 추가,
	//   [[project_duplicate_trip_end_event_speed_charge]] 와 동일 이유)
	void ProcessOpenGateCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd);
	// 개방형 구역의 M게이트를 이 진행(run) 동안 실제로 지났는지 갱신(래치) — HaversineMeters
	//   접근을 위해 멤버 함수로 둠(CollectGateCandidates 류 파일지역 static과 달리) (2026-08-25 최정우 추가)
	void UpdateOpenGateCrossed(const MATCH_LINK_INFO& stMatchLinkInfo, ZONE_RUN_SESSION *pstRun);
	// 폐쇄형 입/출구 게이트 판정 — 입구(I) 통과 시 구간 진입 상태로 전환, 출구(O) 통과 시 누적거리와
	//   함께 1건 적재. road_kind='2'(폐쇄형)인 구역의 게이트만 처리(구간단속 road_kind='3' 등 제외) (2026-08-12 최정우 추가)
	//   2026-08-13 재작성: 한 링크에 같은 방향 게이트 2개 이상/gate_div='B' 겸용 게이트/서로 다른
	//   구역의 출구·입구가 같은 링크를 공유하는 경우까지 처리 — CollectGateCandidates() 참고
	//   [보완] 2026-09-23 부터 입구 게이트 없이 구역 링크에 올라탄 중간 진입도 run 을 열고 N/3(코드 21)로
	//   마감한다. 이탈은 출구 게이트 또는 zone_exitcnt 디바운스(출구 미확인 N/3·23)로 확정
	//   (2026-10-06 최정우 주석 추가)
	void ProcessClosedRoadCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// 구간단속 입/출구 게이트 판정 — road_kind='3'. 평균속도(구역 실거리÷경과시간) 계산,
	//   charge_yn/charge_status 는 기본 Y/0(다른 유형과 동일, 2026-08-13 최정우 수정 — 원래는
	//   항상 N/4 고정이었음) (2026-08-12 최정우 추가). 2026-08-13 재작성 — 폐쇄형과 동일한
	//   멀티게이트/공유링크/gate_div='B' 대응 적용
	//   [보완] 2026-09-06 부터 SPEED 행은 진입·진출 게이트가 모두 확인되고 평균속도 ≥ 제한속도일 때만
	//   만들고, 제한속도가 등록돼 있으면 위반 여부와 무관하게 같은 구간을 일반도로 미러(Y/0)로도 남긴다.
	//   게이트 이상·출구 미확인이면 SPEED 행 없이 미러만. 중간 진입은 run 을 열지 않는다
	//   (2026-10-06 최정우 주석 추가)
	void ProcessSpeedZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// SKIP 틱(맵매칭 실패) 전용 출구 판정 — 확정매칭 전이로도 못 잡는 잔여 케이스 보완. 확정매칭
	//   직전 링크 이탈로도 출구를 못 잡고(예: 그 뒤로 계속 SKIP만 나옴) 세션이 계속 열려 있을 때,
	//   raw GPS 좌표 자체가 구역 시작점 기준 출구 게이트보다 MM_RAWGPS_EXIT_MARGIN_M 이상 더
	//   멀어졌으면(=이미 게이트를 지나쳐 나갔다고 볼 수 있으면) 확정 링크 없이도 출구로 확정한다
	//   (2026-08-24 최정우 추가, 사용자 지시)
	void CheckClosedRoadExitByRawGps(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	void CheckSpeedZoneExitByRawGps(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// [2026-09-17 최정우 정정] 여기 있던 "주정차 세션 시작 … 필드 설정만 분리" 주석은 옛
	//   멤버함수 BeginParkingZoneSession() 설명이었다. 그 함수는 주정차 판정 재작성(2026-08-23,
	//   3fe4fd7 이후) 때 **제거**됐고 세션 개시 코드는 ProcessParkingCharge() 안으로 흡수됐다
	//   (PARK_RUN_SESSION 을 직접 채우는 블록). 지금 이 이름의 함수는 어디에도 없다.
	// 주정차(POLY) 판정 — 맵매칭 전 raw GPS·raw 속도 기준(다른 3종과 달리 매칭 결과 안 씀).
	//   구역판정(위치, ACCURACY_M 적응형 버퍼)+SPEED_KMH(서행 컷오프)+체류시간으로 판정, 구역
	//   이탈은 park_exitcnt 회 연속 확인 후에만 확정(디바운스) — RunMapMatch 호출 "전" 실행 (2026-08-13 최정우 추가)
	//   2026-08-14 재진입 유예 추가: 디바운스 통과(=진짜 이탈 후보) 후에도 다른 구역이 아니라 "무존"
	//   이면 park_regrace 초 동안 즉시 확정하지 않고 대기[2026-09-02 폐지·2026-10-04 설정 삭제 — 현재는 park_exitcnt 연속 확인 즉시 마감] — 그 안에 같은 구역으로 복귀하면 병합,
	//   초과하면 확정 마감. 확정 마감 시점에 이미 다른 구역 위라면 유예 없이 곧바로 그 구역으로
	//   새 세션 시작(경계 전환 병합 — 같은 함수 안 PARK_RUN_SESSION 개시 블록을 그대로 탄다)
	//   2026-08-22 확장 — 규칙 2(매칭 좌표도 폴리곤 내)·규칙 4(매칭 좌표가 폴리곤 밖이면 즉시 해제)를
	//   위해 매칭 결과를 함께 받는다. bMatchTrusted=false 면 매칭 좌표를 보지 않고 원시 좌표만으로 판정.
	//   [정정] 현재 정상 경로의 호출은 RunMapMatch "뒤"(매칭 결과 전달), SKIP 조기반환 경로만 매칭 전
	//   (bMatchTrusted=false) 호출이다. 1 tick 지연 확정(CommitPendingRow)을 거치지 않아 다른 유형보다
	//   한 tick 앞선다. SPEED_KMH 게이트(park_speedmax)는 기본 0=비활성 (2026-10-06 최정우 주석 수정)
	// bTrustedTripEnd — ProcessOpenGateCharge() 주석 참고, 스퓨리어스 END 로 트립종료 강제마감이
	//   오작동하지 않도록 stRawLogInfo.nTripEvent 대신 이 값을 봐야 함 (2026-08-25 최정우 추가)
	void ProcessParkingCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		bool bTrustedTripEnd, bool bMatchTrusted = false, double dfMatchX = 0.0, double dfMatchY = 0.0);
	// 주정차 과금 1행 생성 — 정상 마감·TTL·강제마감 공용 (2026-08-23 최정우 추가)
	// 반환값: STAY_SECONDS 가 base_parking_fine 최소 from_min(초 환산) 이상이면 true(등록 대상),
	//   미만이면 false(호출측은 pvtOut 에 push_back 하지 않음) — pstRow 자체는 반환값과 무관하게
	//   항상 채움(호출측 로그가 STAY_SECONDS 를 그대로 쓸 수 있게) (사용자 지시, 2026-08-24 최정우 추가)
	bool BuildParkRow(const PARK_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, double dfEndX, double dfEndY,
		uint32 dwEndGpsSeq, const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow);
	// 주정차 구역 경계 통과 시각 보간 — dfInX/Y(폴리곤 내부로 판정된 점)와 dfOutX/Y(외부로 판정된 점)
	//   사이를, 각 점의 경계까지 거리 비율로 나눠 실제 경계 통과 시각을 추정한다(등속 직선 이동 가정).
	//   dfInX/Y 가 실제로 폴리곤 내부가 아니거나(버퍼만으로 판정됐거나) dfOutX/Y 가 이미 내부이면
	//   보간 근거가 없어 dtIn 을 그대로 반환한다(무보정). dtOut 이 dtIn 보다 이전/이후 어느 쪽이든
	//   무관 — 진입(현재=In, 직전틱=Out, dtOut<dtIn)과 이탈(마지막 재실=In, 첫 이탈틱=Out, dtOut>dtIn)
	//   양쪽에 동일하게 쓴다 (사용자 지시, 2026-08-24 최정우 추가)
	time_t InterpolateZoneCrossingTime(PZONE_INFO pstZone,
		double dfInX, double dfInY, time_t dtIn, double dfOutX, double dfOutY, time_t dtOut,
		bool *pbInterpolated = nullptr);					// pbInterpolated: 경계 보간 성공 여부(2026-10-04 E20)
	// 폐쇄형/구간단속 게이트 통과 시각 보간 — 위 InterpolateZoneCrossingTime() 과 원리는 같으나
	//   그쪽은 폴리곤 "경계"까지의 거리 기준(주정차 전용)이고, 이쪽은 게이트라는 "점"까지의
	//   직선거리 기준. 직전 확정 tick(dfPrevX/Y, dtPrev)~현재 tick(dfCurX/Y, dtCur) 구간을
	//   등속 직선 이동으로 가정해, 그 사이 어디쯤에서 게이트(dfGateX/Y)를 지났을지 시각을
	//   추정한다. 진입·진출 양쪽에 동일하게 사용(사용자 지시, 2026-08-25 최정우 추가 — "1 tick만의
	//   좌표만 있어도 게이트에서의 통과 시각을 추출할 수 있지 않느냐"는 질문에서 시작)
	time_t InterpolateGateCrossingTime(double dfPrevX, double dfPrevY, time_t dtPrev,
		double dfCurX, double dfCurY, time_t dtCur, double dfGateX, double dfGateY);
	// 강제마감 시점에 아직 열려있는 주정차 세션이 체류 임계 이상이면 위반 1건 적재 — trip END 를
	//   놓쳤든 단말이 전송을 멈췄든 서버는 원인을 구분 못하므로 "계속 정차 중"으로 간주
	//   (사용자 지시, 2026-08-13 추가). 상세는 .cpp 구현부 주석 참고
	//   [2026-09-17 최정우 정정] 이 설명이 BuildParkRow() 앞에 붙어 있던 것을 제자리로 옮김
	void AppendExpiredParkingCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut,
		bool bNoTripEnd = false);
	//   bNoTripEnd: true = 종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
	//     non_charge_reason 을 62(면제도로는 52)로 남긴다. false = TTL·잔여tick·트립종료
	//     (61/51). 판정(charge_yn/status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
	// park_ttl — 세션(디바이스)은 살아있는데 주정차 세션만 마지막 신뢰 확인 후 오래 방치된 경우
	//   좌표 확인 없이 강제 마감 — park_* 필드만 리셋하고 세션 자체는 유지 (2026-08-19 최정우 추가)
	void AppendStaleParkingCharge(int nThreadId, const string& strDeviceKey,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtOut);
	// [2026-09-17 최정우 정정] 여기 있던 "면제도로 세션 시작 … 필드 설정만 분리" 주석도 위 주정차와
	//   같은 고아 주석이라 제거(해당 함수는 헤더에 선언이 없다).
	// 면제도로(ROAD_KIND=5) 진입/이탈 판정 — 게이트 없이 매칭 링크→구역 역인덱스로 판정.
	//   출력은 charge_type="5"(비과금도로 고유값), charge_type/from_id/to_id 는 zone 기반 판정으로
	//   복귀한 최종 원복 상태(사용자 지시, 2026-08-14).
	//   이 함수(정상 이탈·트립종료)가 기록하는 값은 charge_yn='Y'/charge_status='0' 이다 — 다른
	//   과금 유형의 정상 통행과 동일. 과금 제외는 charge_yn 이 아니라 charge_type=5 로 구분된다.
	//   이상 건만 N/4(SKIP): TTL 만료는 AppendExpiredExemptZoneCharge(), 트립 미종료는
	//   [trip_abend] 가 처리하며, 심사 큐(charge_status=3)에는 넣지 않는다 — 면제 건은 사람이
	//   재확인할 대상이 아니기 때문(사용자 지시, 2026-08-30 최정우 수정 — 이전에는 정상 통행까지
	//   N/4 로 고정해 정상/이상이 구분되지 않았다).
	//   [정정] [trip_abend] 는 2026-10-04 삭제됐다 — TTL·잔여 tick·종료신호 없는 트립 전환·서버 종료
	//   마감은 모두 AppendExpiredExemptZoneCharge() 가 N/4(코드 51/52)로 직접 기록한다
	//   (2026-10-06 최정우 주석 수정)
	//   2026-08-14 재진입 유예 추가: "무존" 상태여도 exempt_regrace 초 동안 즉시 확정하지 않고
	//   대기 — 그 안에 같은 구역으로 복귀하면 병합, 초과하면 확정 마감. 확정 마감 시점에 이미 다른
	//   구역 위라면 유예 없이 곧바로 그 구역으로 새 세션 시작(경계 전환 병합, BeginExemptZoneSession
	//   재사용). 진행 중 TTL 만료로 세션이 강제 마감되는 경우는 AppendExpiredExemptZoneCharge() 가 별도 처리
	//   [정정] BeginExemptZoneSession() 은 현재 존재하지 않는다 — 세션 개시는 이 함수 안에서 직접 한다.
	//   유예는 구역 밖 주행거리가 exempt_outmax 이상이면 시간과 무관하게 무효 (2026-10-06 최정우 주석 수정)
	// bTrustedTripEnd — ProcessOpenGateCharge() 주석 참고 (2026-08-25 최정우 추가)
	void ProcessExemptZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd);
	// 면제도로 과금 1행 생성 — 정상 이탈과 TTL 만료 공용 (2026-08-23 최정우 추가)
	//   charge_yn/status 는 호출측이 지정한다(BuildParkRow 와 동일 패턴) — 정상 이탈은 Y/0,
	//   TTL 만료는 N/4. 다른 유형이 TTL flush 를 C++ 에서 직접 N/3 으로 세팅하는 것과 같은 구조로,
	//   면제도로만 SKIP(4) 를 쓴다 (사용자 지시, 2026-08-30 최정우 수정)
	void BuildExemptRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, uint32 dwEndGpsSeq,
		const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow);
	// 강제마감 시점에 아직 열려있는 면제도로 세션이면 N/4 로 1건 기록 — BuildExemptRow 에 "N","4"
	//   를 넘긴다. 다른 유형이 강제마감을 C++ 에서 직접 N/3(AUDIT)으로 세팅하는 것과 같은 구조이며,
	//   면제도로만 N/4(SKIP)를 쓴다: 과금 대상이 아니라 심사 큐에 올릴 필요가 없기 때문
	//   (사용자 판단 계승 2026-08-14, 2026-08-30 재확인). trip_end_dt 를 채운 채 INSERT 하므로
	//   [trip_abend] 와 겹치지 않는다(주정차와 동일 패턴)
	//   [정정] BuildExemptRow·BuildParkRow 는 trip_end_dt 를 채우지 않는다(빈 값 INSERT 후 [trip_end]
	//   UPDATE 가 채움). [trip_abend] 는 2026-10-04 삭제 (2026-10-06 최정우 주석 수정)
	//   [2026-09-17 최정우 정정] 이 설명이 BuildExemptRow() 앞에 붙어 있던 것을 제자리로 옮김
	void AppendExpiredExemptZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut,
		bool bNoTripEnd = false);
	//   bNoTripEnd: true = 종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
	//     non_charge_reason 을 62(면제도로는 52)로 남긴다. false = TTL·잔여tick·트립종료
	//     (61/51). 판정(charge_yn/status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
	// 일반도로(ROAD_KIND=0, NODE_STEP) 진입/이탈 판정 — 비과금도로와 동일 구조(게이트 없이 매칭
	//   링크→구역 역인덱스)지만 실제 과금 대상이라 이탈·트립종료 시 항상 Y/0 으로 1건 기록
	//   (사용자 지시, 2026-08-14 추가)
	//   [보완] 2026-09-01 부터 미등록 링크·게이트형 구역 run 이 닫힌 링크도 대상이다
	//   (IsLinkNodeStepEligible). "항상 Y/0" 은 정상 마감 기준이며, SKIP 구간 직선추정은 N/3(코드 1·2),
	//   강제마감은 N/3(코드 61/62)이다 (2026-10-06 최정우 주석 추가)
	// bTrustedTripEnd — ProcessOpenGateCharge() 주석 참고 (2026-08-25 최정우 추가)
	void ProcessNodeStepCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd,
		// bTrustedMatch — 이 tick 의 최종 상태가 MATCHED 이고 좌표가 있는가(false=SKIP·좌표없음).
		//   false 면 **새 일반도로 run 을 열지 않는다**. 엔진 내부에서는 매칭으로 취급돼도 DB 상
		//   SKIP 인 tick 이 run 을 열면, 실측 tick 이 하나도 없는 구간에 지오메트리 추정치만으로
		//   과금이 생긴다 — 실측 000376_20260819140532 seq4(좌표 없는 SKIP)가 2040425401 로 run 을
		//   열어 seq4~5 에 27m 일반도로가 얹혔다. 기존 run 의 연장·마감은 종전대로 진행한다
		//   (2026-09-07 최정우 추가, 사용자 지시)
		bool bTrustedMatch = true);
	// 보류(pending) 중인 1틱 지연 행을 확정(commit) — 반대편 짝 링크 1틱 오매칭이면 SKIP(미과금)으로
	//   보정 후, 과금 함수 호출(직전 매칭 위치·시각은 보류 시점 스냅샷으로 잠깐 바꿔치기 후 원복) +
	//   rawgps_update 큐잉까지 수행. bHasNextLinkID/qwNextLinkID=보정판단용 "다음" 확정 링크,
	//   없으면 false/0(보정 시도 안 함, 계산된 값 그대로 커밋) (2026-08-21 최정우 추가)
	// dfNextMatchX/Y — bHasNextLinkID 일 때 "다음" 확정 링크의 매칭좌표(호출측 stMatchLinkInfo.
	//   dfMatchX/Y). 클램프 브릿지의 전.후 확정좌표 방향검증 대안 경로 전용 — bHasNextLinkID=false
	//   면 안 쓰임(기본값 그대로 둬도 무방) (2026-09-04 최정우 추가, 사용자 지시)
	void CommitPendingRow(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		bool bHasNextLinkID, uint64 qwNextLinkID,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		double dfNextMatchX = 0.0, double dfNextMatchY = 0.0);
	// 일반도로 과금 1행 생성 — 정상 이탈과 TTL 만료가 같은 형식을 쓰도록 공용화 (2026-08-23 최정우 추가).
	//   charge_yn/status 는 호출측이 지정(BuildParkRow/BuildExemptRow 와 동일 패턴) — 정상 이탈은
	//   Y/0, TTL·비정상종료는 N/3(AUDIT) (2026-09-01 최정우 수정 — 기존엔 항상 Y/0 하드코딩이라
	//   TTL 만료 시에도 심사 큐에 안 올라가던 버그, 사용자 지시로 파라미터화)
	void BuildNodeStepRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, uint32 dwEndGpsSeq,
		const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow);
	// 강제마감 시점에 아직 열려있는 일반도로 세션이면 N/3(AUDIT)로 1건 기록 — 실제 과금 대상이라
	//   폐쇄형/구간단속/주정차와 동일하게 AUDIT(3), 면제도로의 SKIP(4)과는 다름
	//   (사용자 지시 패턴 계승, 2026-08-14 추가)
	//   [2026-09-17 최정우 정정] 이 설명이 BuildNodeStepRow() 앞에 붙어 있던 것을 제자리로 옮김
	// bStationaryTripEnd: 트립이 신뢰 가능한 END 로 끝나면서(TTL 만료 아님) 그 시점 DRIVE_STATUS 가
	//   정차·주차/일시정지인 경우 true — 이때는 심사대상(N/3) 대신 정상마감(Y/0)으로 기록한다
	//   (사용자 지시, 2026-09-14 최정우 추가 — 실측 000376_20260819141002 trip_seq=6: seq102 이후
	//   SKIP 구간이 속도 0·DRIVE_STATUS=PARKED 인 채로 트립이 정상 종료됐는데도 "확정 못 한 채 끝남"
	//   취급돼 N/3 으로 나가던 문제). TTL 스윕(세션 자체 만료, 신뢰할 END 신호 자체가 없음) 호출부는
	//   항상 false 로 호출.
	void AppendExpiredNodeStepCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEnd, uint32 dwEndGpsSeq,
		bool bStationaryTripEnd, vector<CHARGE_INSERT_ROW> *pvtOut,
		bool bNoTripEnd = false);
	//   bNoTripEnd: true = 종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
	//     non_charge_reason 을 62(면제도로는 52)로 남긴다. false = TTL·잔여tick·트립종료
	//     (61/51). 판정(charge_yn/status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
	// 트립종료(TRIP_EVENT=END) 처리 중 "이번 틱이 신뢰 못할 매칭(bUntrustedMatch)이거나 매칭 자체를
	//   못 한" 조기 반환 경로들 전용 — CommitPendingRow() 가 이번 틱이 아니라 훨씬 이전에 보류돼있던
	//   행을 커밋하면서 그 행 자신의(트립종료 아닌) TRIP_EVENT 기준으로 bTrustedTripEnd 를 다시 판정해
	//   버려, 정작 지금 끝나는 트립의 열려있는 NODE_STEP run 은 아무도 안 닫아주는 사각지대를 막는
	//   안전망. ProcessRawLog() 안에서 bTrustedTripEnd 인 조기 반환 지점마다 *pbTripEnded=true 옆에
	//   같이 호출한다(트립종료 트리거로 정상 확정되는 경로는 CommitPendingRow 가 이미 처리하므로
	//   호출 불필요 — 중복 호출 시 원래 정상 처리되던 값을 이 함수의 N/3(AUDIT) 값으로 덮어써버리는
	//   회귀가 실측 확인됨, 2026-09-03) (2026-09-03 최정우 추가)
	void FlushNodeStepRunsAtTripEnd(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// [버그 수정, 2026-09-11 최정우] 개방형·면제구역판 FlushNodeStepRunsAtTripEnd() — 반드시 그 함수와
	//   똑같은 지점(이번 tick 자체가 맵매칭 실패로 곧장 return 하는 조기반환 경로)에서만 호출할 것.
	//   처음엔 CLOSED/SPEED 의 "무조건 매칭 시도 전에 flush" 패턴을 그대로 베꼈다가, OPEN/EXEMPT 는
	//   "트립 마지막 tick 자체가 정상 매칭되며 끝나는" 케이스가 흔해서(bMatched 블록의 정상 Y/0
	//   종료가 이 tick 에서 바로 일어남) 그 정상 종료까지 먼저 가로채 N/3(AUDIT)으로 잘못
	//   강등시키는 회귀를 냈다(재매칭 대조로 발견) — FlushNodeStepRunsAtTripEnd() 선언부 주석의
	//   2026-09-03 사례와 근본원인이 완전히 같다. bMatched 로 정상 확정되는 경로에는 절대 호출하지
	//   말 것(자세한 배경은 .cpp 주석 참고)
	void FlushOpenExemptRunsAtTripEnd(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// NODE_STEP 일반도로 등록 확장(2026-09-01 최정우 추가) — LINK_ID 를 FROM_ID~TO_ID 로 삼는
	//   신규 스코프(구간단속 위반 추가분/SKIP 구간 브릿지) 공용 row 생성. BuildNodeStepRow() 와 달리
	//   road_id 기반 ZONE_RUN_SESSION 없이 호출측이 이미 계산해둔 값을 그대로 채운다.
	//   pszZoneId/pszZoneName 이 nullptr 이면 빈 값(케이스2/3처럼 실제 zone이 없는 경우)
	void BuildNodeStepRowFromLinkRange(const string& strTripId, const string& strDeviceKey,
		int nChargeSeq, uint64 qwFromLink, uint64 qwToLink,
		double dfFromLat, double dfFromLon, double dfToLat, double dfToLon, double dfDistM,
		time_t dtStart, time_t dtEnd, uint32 dwStartGpsSeq, uint32 dwEndGpsSeq,
		const char *pszChargeYn, const char *pszChargeStatus,
		const char *pszZoneId, const char *pszZoneName, CHARGE_INSERT_ROW *pstRow);
	// NODE_STEP 케이스3(SKIP 구간) 브릿지 — CommitPendingRow() 가 새 신뢰매칭을 확정하는 순간,
	//   그 직전까지의 SKIP 구간이 있었고 전후 확정링크가 IsCase3EligibleRoadKind() 범위에 속하면
	//   호출. 3단계 fallback(재매칭→그래프경로탐색→직선거리) 은 함수 내부 주석 참고 (2026-09-01 최정우 추가)
	void ResolveSkipGapNodeStep(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		uint64 qwFromLink, uint64 qwToLink, const sRawLogInfo& stToRawLogInfo,
		const MATCH_LINK_INFO& stToMatchLinkInfo, vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// FROM~TO 링크 사이 방향성 링크그래프(TURN_INFO 기반) bounded BFS — ResolveSkipGapNodeStep() 2순위
	//   fallback용 순수 그래프탐색(맵매칭 스코어링 없음). pvtPathOut 은 qwFromLink~qwToLink 포함
	//   순서대로, 실패 시 비움 (2026-09-01 최정우 추가)
	bool FindLinkPathBounded(uint64 qwFromLink, uint64 qwToLink, int nMaxHops, vector<uint64> *pvtPathOut);
	// 폐쇄식·구간단속 출구 게이트를 누락 링크 복구 경로에서 찾는다 — 구현부 주석 참고 (2026-10-04 최정우 추가)
	PGATE_INFO FindExitGateOnRecoveredPath(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, uint64 qwLastZoneLinkID, const char *pszZoneRoadId);
	// 링크의 시작 노드부터 세그먼트를 순회하며 폴리곤과 처음 교차하는 지점까지의 부분 거리·좌표를
	//   구한다 — 주정차 접촉으로 마감되는 NODE_STEP run의 누락 링크 보정 전용(사용자 지시,
	//   2026-09-03 최정우 추가). 링크 전체가 폴리곤 밖이면 false(호출측이 전체 길이를 더하고
	//   다음 링크로 진행)
	// 링크가 폴리곤 안에 들어가 있는 **모든** 구간을 링크 진행순으로 돌려준다 — 아래 두 함수
	//   (첫 교차만 보는 진입·이탈 전용)의 일반형이며, 그 둘은 이 함수 결과의 첫 구간을 읽어
	//   종전과 동일한 값을 만든다. 한 링크가 폴리곤을 여러 번 드나드는 경우(모서리를 스치는
	//   링크)를 다루려면 이 함수를 써야 한다 (2026-09-21 최정우 추가, 사용자 지시 — 이슈 31)
	// 반환: true=구간을 하나 이상 찾음, false=링크 전체가 폴리곤 밖이거나 링크·폴리곤 정보 없음
	bool FindLinkPolygonSpans(uint64 qwLinkID, const vector<POINT>& vtPolyCoords,
		vector<LINK_POLY_SPAN> *pvtSpans);
	bool FindLinkPolygonCrossing(uint64 qwLinkID, const vector<POINT>& vtPolyCoords,
		double *pdfPartialDistM, double *pdfCrossX, double *pdfCrossY);
	// 위 함수의 이탈 방향 대칭 — 링크를 시작 노드부터 따라가며 폴리곤 "안→밖"으로 벗어나는
	//   지점(링크 시작부터의 거리·좌표)을 구한다. 확정 접촉이 이탈될 때 "폴리곤 경계부터"
	//   NODE_STEP run 을 여는 데 쓴다. 링크 전체가 폴리곤 안이거나 전체가 밖이면 false
	//   (호출측이 다음 링크로 진행) (2026-09-05 최정우 추가, 사용자 지시)
	bool FindLinkPolygonExitCrossing(uint64 qwLinkID, const vector<POINT>& vtPolyCoords,
		double *pdfExitDistM, double *pdfCrossX, double *pdfCrossY);
	// 클램프 저신뢰 SKIP 런을 "전·후 확정좌표 궤적 방향"으로 검증해 MATCH_STATUS 만 복원 —
	//   bConnected 실패 경로와 "같은 링크로 복귀" 경로가 공용으로 쓴다. 상세는 구현부 주석 참고
	//   (2026-09-04 최정우 추가, 2026-09-05 최정우 수정 — 헬퍼로 분리, 사용자 지시)
	size_t BridgeClampRunByTrajectory(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		double dfNextMatchX, double dfNextMatchY, const sRawLogInfo& stRawLogInfo,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates);
	// 개방형 과금 1행 생성 — 정상 이탈과 TTL 만료 공용. bStartedByTrip 에 따라 dist_m 산출
	//   방식·charge_yn/status 가 갈린다 — ZONE_RUN_SESSION 상단 주석 참고 (2026-08-25 최정우 추가)
	void BuildOpenZoneRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, uint32 dwEndGpsSeq, CHARGE_INSERT_ROW *pstRow);
	// TTL 만료로 세션이 지워지기 직전, 아직 열려있는 개방형 run 이면 그 상태 그대로 1건 기록 —
	//   N/3(AUDIT)로 직접 기록한다. [2026-10-04 정정] 종전엔 [trip_abend] 가 뒤이어 정정하는 2단계였으나
	//   [trip_abend] 는 삭제됐다(TTL 마감은 [trip_end] 로 통합 — 판정을 바꾸지 않음) (2026-08-25 최정우 추가)
	void AppendExpiredOpenGateCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut,
		bool bNoTripEnd = false);
	//   bNoTripEnd: true = 종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
	//     non_charge_reason 을 62(면제도로는 52)로 남긴다. false = TTL·잔여tick·트립종료
	//     (61/51). 판정(charge_yn/status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
	// [버그 수정, 2026-09-11 최정우] 트립 정상종료(TRIP_EVENT=2) 시 마지막 tick 자체의 맵매칭이
	//   실패하면 ProcessOpenGateCharge()가 안 불려 개방형 run 이 과금 레코드 없이 사라지던 버그 —
	//   CLOSED/SPEED 의 무조건 flush(ProcessRawLog bTrustedTripEnd 블록)와 동일 안전망. run 별
	//   dtLastInZoneTime/dwLastInZoneGpsSeq(정확한 GPS 종료시각) 사용, AppendExpiredOpenGateCharge()
	//   와 달리 wall-clock(dtLastSeen) 아님 — 자세한 배경은 .cpp 주석 참고
	//   [정정] 현재는 AppendExpiredOpenGateCharge() 도 run 별 dtLastInZoneTime/dwLastInZoneGpsSeq 를
	//   쓰고, 그 값이 없을 때만 dtLastSeen 으로 대체한다 (2026-10-06 최정우 주석 수정)
	void AppendTripEndOpenGateCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut);
	// 세션이 지워지기(TTL) 또는 정리되기(트립 정상종료, TRIP_EVENT=2) 직전, 아직 입구만 통과하고
	//   출구를 못 찾은 폐쇄형 세션이면 N/3(AUDIT)로 1건 기록 — 출구를 못 봐서 dist_m/speed_kmh/
	//   to_id/to_lat·lon은 비워둠(모르는 값을 지어내지 않음, NULLIF(...,'')로 DB에 NULL 저장됨),
	//   확실히 아는 것(입구 게이트·구역·진입시각)만 기록 (2026-08-14 최정우 추가 — 기존에 이 함수
	//   자체가 없어 진행 중 세션이 TTL 시 통째로 유실되던 문제 해결).
	//   dtEndTime — 마감 기준 시각(trip_end_dt/stay_seconds 계산용): TTL 경로는 세션의 마지막
	//   처리 시각(dtLastSeen, wall-clock), 트립 정상종료 경로는 그 tick의 GPS 시각(dtGPS) — 호출
	//   측이 상황에 맞는 값을 넘겨줌(2026-08-20 최정우 수정 — 트립 정상종료 시에도 호출되도록 확장)
	//   [정정] ① 2026-08-25 부터 dist_m·speed_kmh·to_lat/lon 은 비우지 않고 구역 안 실측 누적거리·
	//   마지막 구역 안 위치로 채운다 — 비우는 것은 to_id·exit_tollgate_id·speed_limit_kmh 뿐.
	//   ② TTL 경로의 dtEndTime 은 2026-10-04 부터 dtLastSeen(벽시계)이 아니라 그 트립 마지막 GPS 시각
	//   (dtLastGpsEventTime, 없을 때만 dtLastSeen)이다 (2026-10-06 최정우 주석 수정)
	void AppendExpiredClosedRoadCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut,
		bool bNoTripEnd = false);
	//   bNoTripEnd: true = 종료신호(TRIP_EVENT=END) 없이 다음 운행이 시작돼 마감하는 경로 —
	//     non_charge_reason 을 62(면제도로는 52)로 남긴다. false = TTL·잔여tick·트립종료
	//     (61/51). 판정(charge_yn/status)은 어느 쪽이든 동일하다 (2026-09-16 최정우 추가)
	// 구역 이탈 경계 보정(구간단속·폐쇄형 공용) — 면제도로가 쓰던 것과 동일 원리.
	//   거리 누적을 "구역 안 tick 만"으로 바꾼 뒤 생긴 경계 구간 누락을 메운다
	//   (2026-09-15 최정우 추가, 상세 근거는 .cpp 함수 주석 참고)
	// @warning **"구역 이탈이 확정된" 경로에서만 쓸 것.** 이 함수는 마지막으로 구역 안에서
	//   확인된 지점부터 구역 경계까지의 "꼬리" 거리를 더해준다 — 차가 실제로 그 경계를 지나
	//   나갔다는 것이 전제다. TTL 만료·트립종료처럼 **출구를 못 본 채 끝난** 마감 경로에 넣으면
	//   주행하지도 않은 거리를 청구하게 된다(2026-09-15 에 실제로 폐쇄형·구간단속 강제마감에
	//   넣었다가 같은 날 되돌린 전례가 있다. 구간단속은 더 위험하다 — 부풀려진 거리가 평균속도를
	//   끌어올려 있는 위반을 없는 위반으로 뒤집을 수 있다).
	//   (2026-09-21 최정우 주석 보완 — 되돌린 근거가 .cpp 두 곳에만 흩어져 있어 헤더만 보고
	//    재사용하면 같은 실수를 반복하게 된다)
	double ApplyZoneExitTailDist(uint64 qwLastZoneLinkID,
		double *pdfLastZoneX, double *pdfLastZoneY, double *pdfAccumDistM);
	// 세션이 지워지기(TTL) 또는 정리되기(트립 정상종료) 직전, 아직 입구만 통과하고 출구를 못 찾은
	//   구간단속 세션이면 N/3(AUDIT)로 1건 기록 — 폐쇄형과 동일 이유로 dist_m/speed_kmh/to_lat·lon은
	//   비워둠 (2026-08-14 최정우 추가, 2026-08-20 최정우 수정 — dtEndTime 파라미터화, 근거는
	//   AppendExpiredClosedRoadCharge() 주석 참고)
	//   [정정] 현재 이 함수는 구간단속(CHARGE_TYPE=3) N/3 행을 만들지 않는다 — 제한속도와 무관하게
	//   열린 구간을 일반도로 미러(CHARGE_TYPE=0, Y/0) 1행으로만 기록한다. 아래 2026-09-21 주석 참고
	//   (2026-10-06 최정우 주석 수정)
	// dwEndGpsSeq — 종료 tick 의 GPS_SEQ(0=미지정, 세션값 사용). 트립 종료 이벤트 tick 은
	//   직전 tick 의 복사본이라 구역 갱신 경로를 안 타서 세션의 dwSpeedLastGpsSeq 가 그 앞에서
	//   멈춘다 — 호출측이 실제 종료 seq 를 넘겨 구간 표기를 트립 끝에 맞춘다
	//   (2026-09-06 최정우 추가, 사용자 지시)
	// [2026-09-21 최정우 주석 보완] **이 함수만 bNoTripEnd 파라미터가 없는 것은 누락이 아니라
	//   의도**다. 다른 Append*(Parking/Exempt/NodeStep/Open/Closed)는 강제마감 행에
	//   non_charge_reason 62(면제 52)를 붙이는데, 이 함수가 만드는 유일한 행은 일반도로 미러이고
	//   그 행은 Y/0(코드 0, 정상 과금)이라 붙일 사유 자체가 없다. 구간단속(CHARGE_TYPE=3) 행은
	//   2026-09-06 부터 "진입·진출 게이트 양쪽 통과 + 평균속도 초과" 일 때만 만들어지므로,
	//   정의상 진출 게이트를 못 본 이 경로에서는 아예 생성되지 않는다(구현부 @remark 참고).
	//   헤더만 보고 "5개는 받는데 여기만 빠졌네" 로 오해해 파라미터를 추가하지 말 것.
	void AppendExpiredSpeedZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut,
		uint32 dwEndGpsSeq = 0);
	// 매칭된 tick 이 하나도 없는 일반도로 행을 INSERT 직전에 걸러낸다. 상세는 구현부 주석 참고
	//   (2026-09-07 최정우 추가, 사용자 지시)
	//   [2026-09-17 최정우 정정] 아래 두 선언의 주석이 서로 바뀌어 붙어 있던 것을 맞춤.
	//   호출 순서는 Drop → Merge 이며 뒤집으면 안 된다(run() 주석 참고)
	//   [2026-09-23 최정우 보완] 대상은 일반도로(0) 전부 + **폐쇄형(2) 중 코드21**(구역 중간 진입으로
	//   게이트 근거 없이 열린 행)이다 — 근거·실측은 구현부 헤더 주석 참고
	void DropNodeStepRowsWithoutMatch(vector<CHARGE_INSERT_ROW> *pvtCharges,
		const vector<RAW_LOG_UPDATE_ROW>& vtUpdates);
	// 배치 INSERT 직전, 같은 트립에서 연속으로 이어지는 일반도로(CHARGE_TYPE=0) 행을 하나로
	//   합치고 거리·체류시간·평균속도를 다시 계산한다. 상세 규칙은 구현부 주석 참고
	//   (2026-09-06 최정우 추가, 사용자 지시)
	void MergeAdjacentNodeStepRows(vector<CHARGE_INSERT_ROW> *pvtCharges);
	// @brief 개방형 경로복구 행(NCR_OPEN_GATE_NOT_ON_PATH)을 같은 구역 정규 마감 행에 흡수
	//   (2026-09-22 최정우 추가) — ProcessOpenGateCharge() 안에 있던 흡수 판정을 그대로 떼어낸
	//   독립 패스. 종전에는 **그 배치 벡터 안에 두 행이 같이 있을 때만** 흡수돼, 배치가 갈리면
	//   9m 짜리 N/3 조각이 그대로 남았다(실측 000994_20250903152350 trip_seq=14).
	//   큐(vtPendingEmit)에도 적용해 배치 경계 의존성을 없앤다. **거리는 합산하지 않는다** —
	//   개방형 DIST_M 은 구역 전체 길이 고정값이라 더하면 구역보다 긴 거리가 청구된다.
	static void MergeGapRecoveredOpenRows(vector<CHARGE_INSERT_ROW> *pvtCharges);
	// 이 매칭 링크를 일반도로(CHARGE_TYPE=0) run 으로 계상해도 되는지 판정. 상세는 구현부 주석 참고
	//   (2026-09-07 최정우 추가, 사용자 지시)
	bool IsLinkNodeStepEligible(const uint64 qwLinkID, const VEHICLE_TRIP_SESSION *pstSession);
	// 게이트 좌표의 링크 진행거리(시작노드부터 폴리라인을 따라 잰 m). 상세는 구현부 주석 참고
	//   (2026-09-07 최정우 추가, 사용자 지적)
	double GatePosOnLink(uint64 qwLinkID, double dfLon, double dfLat);
	bool BulkInsertCharges(PGconn *pcConn, const vector<CHARGE_INSERT_ROW>& vtCharges);
	// 트립 종료 시 그 trip_id 의 PRIM_CHARGEHAND 전 행에 trip_end_dt 반영 (2026-08-12 최정우 추가)
	bool UpdateTripEndDt(PGconn *pcConn, const vector<TRIP_END_UPDATE_ROW>& vtRows);
	// TTL 만료(비정상 종료) 시 그 trip_id 의 전 과금유형(0~5) 중 아직 TRIP_END_DT 없는 행을
	//   N/3(AUDIT) + TRIP_END_DT(마지막 확인 시각)으로 마감(사용자 지시, 2026-08-13 추가,
	//   2026-08-13 수정 — 개방형 한정 해제, status 4→3 정정)
	//   [정정] 위 3줄은 삭제된 [trip_abend] 처리 함수의 고아 주석이다 — 2026-10-04 TTL 마감이 [trip_end]
	//   (UpdateTripEndDt, 판정 컬럼 미변경)로 통합되면서 함수·SQL 모두 없어졌다 (2026-10-06 최정우 주석 수정)
	// 그 트립의 기존 최대 TRIP_SEQ 조회(읽기 전용, trip_seq 이어 매기기) — 구현부 주석 참고 (2026-10-04 최정우 추가)
	int QueryTripSeqMax(PGconn *pcConn, const string& strTripId, const string& strDeviceKey);
	// UpdateTripSeqOrder() 삭제 (2026-10-04 최정우 삭제 — 사용자 지시: trip_seq 는 SQL 로 처리하지 않고 엔진이 확정할 때 등록한다)

	// ── 과금 행 워터마크 큐 (2026-09-22 최정우 추가 — 사용자 확정 요구) ────────────────────
	//   TRIP_SEQ 를 적재 시점에 최종 확정하기 위한 3종 세트. 상세는 VEHICLE_TRIP_SESSION 의
	//   vtPendingEmit 주석 참고.
	// @brief 지금 시점에 "순서가 확정된" GPS_SEQ 상한을 구한다. 이 값보다 작은 start_gps_seq 를
	//   가진 행은 뒤늦게 더 생길 수 없으므로 번호를 확정해도 안전하다.
	// @param[in] dwCurGpsSeq 이번 배치에서 마지막으로 처리한 GPS_SEQ
	// @return 확정 상한(exclusive). 0 이면 방출 가능한 행이 없음
	static uint32 CalcChargeWatermark(const VEHICLE_TRIP_SESSION& stSession, uint32 dwCurGpsSeq);
	// @brief 이번 배치가 만든 행을 세션 큐로 옮긴다(이미 방출된 행은 그대로 둔다)
	static void EnqueueChargeRows(VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtRows);
	// @brief 큐에서 start_gps_seq < dwWatermark 인 행을 GPS_SEQ 순으로 꺼내 TRIP_SEQ 를 부여
	// @param[in] dwWatermark 확정 상한. UINT32_MAX 면 전량 방출(트립종료·전환·TTL·서버종료)
	//   ※ static 이 아니다 — 방출 직전에 MergeAdjacentNodeStepRows()(비-static 멤버)를 호출한다
	void ReleaseChargeQueue(int nThreadId, VEHICLE_TRIP_SESSION *pstSession, uint32 dwWatermark,
		vector<CHARGE_INSERT_ROW> *pvtOut);
	// @brief 트립 경유 링크 대비 일반도로 청구 거리 커버리지를 로그로 남긴다(계측 전용, 판정 무변경)
	//   (2026-09-22 최정우 추가) — 상세는 VEHICLE_TRIP_SESSION::vtTripPathLinks 주석 참고
	void LogTripLinkCoverage(int nThreadId, const VEHICLE_TRIP_SESSION& stSession);
	// @brief 어느 행에도 덮이지 않은 링크 구간을 일반도로(charge_type=0) 행으로 복구해 큐에 넣는다
	//   (2026-09-22 최정우 추가 — 사용자 지시, 커버리지 2단계)
	// @param[in,out] pstSession 대상 세션(전량 방출 직전)
	// @return 생성된 행 수
	int FillUncoveredLinkRows(int nThreadId, VEHICLE_TRIP_SESSION *pstSession);

	// @brief 차량 진행 방향이 구역 진행 방향과 **정반대**인지 판정 — 반대편 차선 오매칭 차단
	//   (2026-09-22 최정우 추가 — 사용자 지시)
	// @param[in] strRoadId 대상 구역 road_id
	// @param[in] stRawLogInfo 진입 판정 중인 tick(heading·speed 사용)
	// @return true = 역방향(진입으로 인정하지 않아야 함)
	bool IsZoneDirectionOpposite(const string& strRoadId, const RAW_LOG_INFO& stRawLogInfo);

	// @brief 링크가 주정차 단속 폴리곤 안인지 — 커버리지 복구 제외 판정 (2026-09-22 최정우 추가)
	//   [보완] 2026-10-04 E16 이후 호출부가 MM_PARK_SUPPRESS_NODESTEP(현재 false) && 로 묶여 있어
	//   실제로는 평가되지 않는다 — 되돌릴 때만 다시 쓰인다 (2026-10-06 최정우 주석 추가)
	bool IsLinkInsideParkingPolygon(uint64 qwLinkID);
	// 원시좌표가 주정차 폴리곤 안(경계 park_pad 이내 포함)인지 — 정책 E18(SKIP 좌표는 주정차 정보에만) 판정용
	bool IsRawInParkingZone(double dfLon, double dfLat, int nAccuracyM);
	// SKIP 버퍼(vtSkipRunRawLogInfo)에 폴리곤 안 SKIP 좌표가 하나라도 있는지 (E18)
	bool IsSkipGapInParkingZone(const VEHICLE_TRIP_SESSION *pstSession);

	// @brief 과금 대상 일반도로 행의 구간에 **완전히 포함**되는 N/3 일반도로 행을 제거
	//   (2026-09-22 최정우 추가 — 사용자 지시)
	// @param[in,out] pstSession 대상 세션 — vtPendingEmit 에서 해당 행을 뺀다
	// @return 제거한 행 수
	int DropContainedAuditNodeStepRows(int nThreadId, VEHICLE_TRIP_SESSION *pstSession);
	static string FormatDateTime14(time_t dtValue);
	// @brief FormatDateTime14() 의 역함수 — 14자리 문자열을 time_t 로 (2026-09-22 최정우 추가)
	static time_t ParseDateTime14(const string& strValue);
	// TRIP_ID({6자리 숫자}_{YYYYMMDDHH24MISS}) 에서 시각 부분만 추출 — TRIP_ID 안의 첫 '_'
	//   위치를 직접 찾아 그 다음부터 반환(DEVICE_KEY 길이에 의존하지 않음). 형식이 안 맞으면
	//   nullptr (2026-08-19 최정우 추가 — 기존엔 DEVICE_KEY 길이만큼 건너뛰는 방식이었는데,
	//   TRIP_ID 포맷이 {DEVICE_KEY}_{시각}에서 {CAR_SEQ_NO 6자리}_{시각}로 바뀌면서 길이가
	//   달라져 trip_start_dt 가 엉뚱한 위치부터 잘리는 버그가 있었음, 실측 확인됨)
	static const char* ExtractTripStartDt(const char *szTripId);
	static bool AppendUpdateRow(vector<RAW_LOG_UPDATE_ROW> *pvtUpdates,
		const sRawLogInfo& stRawLogInfo, sint16 nStatus, int nIntersectLen = -1,
		const double *pdfMatchLat = nullptr, const double *pdfMatchLon = nullptr,
		uint64 qwMatchLinkId = 0);
	bool BulkUpdateRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates);
	// bulk update 실패 시 동일 rawgps_update 로 PROCESSING(2)→PENDING(0) 예약 해제
	bool BulkReleaseRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates);
	// 반환 전 미완료 트랜잭션 ROLLBACK 가드 (향후 명시적 트랜잭션 대비)
	//   [보완] 현재 배치·TTL 마감은 이미 명시적 BEGIN/COMMIT 을 쓰므로, 그 도중 실패로 열린 채 남은
	//   트랜잭션을 풀 반환 전에 정리하는 실사용 가드다 (2026-10-06 최정우 주석 추가)
	void ReleaseConnection(PGconn *pcConn);
	static bool AppendReleaseRowFromRawLog(vector<RAW_LOG_UPDATE_ROW> *pvtRelease,
		const sRawLogInfo& stRawLogInfo);
	static bool IsRowInUpdates(const vector<RAW_LOG_UPDATE_ROW>& vtUpdates,
		const string& strTripId, const string& strGpsSeq);
	static int GetPgCmdTuples(PGresult *pcResult);
	static bool CheckPgUpdateAffected(PGresult *pcResult, int nExpected, const char *pszLogTag);
	static string BuildPgTextArray(const vector<string>& vtValues);
	static string EscapePgArrayText(const string& strValue);
	static bool ValidateRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo, sint16 *pnRejectStatus);
	// bIgnoreRawVld=true 면 RAW_VLD 검사를 건너뛴다(config ignore_rawvld, 검증용) (2026-08-23 최정우 추가)
	static bool ShouldSkipGpsInput(int nThreadId, const sRawLogInfo& stRawLogInfo, bool bIgnoreRawVld);
	// 이동거리 환산속도 vs SPEED_KMH 정합성 검사 — 이상치 GPS SKIP 판정 (2026-07-20 최정우 추가)
	bool ShouldSkipImplausibleSpeed(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, int *pnImpliedSpeedKmh);
	// Begin 폴백(위상 연결 미검증) 확정 결과 — 직전 확정 위치→신규 매칭 위치 거리가 raw GPS 이동거리
	//   대비 비현실적인지 검사 (MM_PATH_PLAUSIBLE_SCALE/FLOOR_M 재사용) — SKIP 판정용 (2026-09-04 최정우 추가)
	bool IsFallbackJumpImplausible(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, const MATCH_LINK_INFO& stMatchLinkInfo);
	// raw GPS가 등록된 주정차구역 폴리곤 안인데, 매칭된 좌표는 그 구역 밖으로 나온 경우 — 그래프
	//   탐색 코드는 전혀 건드리지 않고 결과만 사후 비교(ChargeDataLoader::GetParkingZonesContaining
	//   재사용, ProcessParkingCharge 규칙4 로직과 대칭) — SKIP 판정용. MM_ZONE_OUTSIDE_SPEED_MAX_KMH
	//   이하 속도에서만 대상(2026-09-04 최정우 추가, 사용자 지시)
	bool IsMatchOutsideRawZonePolygon(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo);
	static bool IsValidTripIdForDevice(const sRawLogInfo& stRawLogInfo);
	static bool IsValidTripEvent(sint16 nTripEvent);
	// pbSeqRollback: GPS_SEQ 역전(과거·중복 seq) 감지 시 true — 이 경우 반환값은 false(BEGIN 강등 안 함)
	//   이고, 호출측이 해당 행만 SKIP 한다 (2026-08-23 최정우 추가)
	static bool NeedsBeginReset(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, bool *pbFullReset, bool *pbSeqRollback);
	static void ResetTripSessionForBegin(VEHICLE_TRIP_SESSION& stSession, bool bFullReset);
	// TRIP_EVENT=END 신뢰 여부 판정(스퓨리어스 순서역전 검사) — ProcessRawLog()·CommitPendingRow()
	//   양쪽에서 공용으로 씀. 상태 변경(dtLastGpsEventTime 갱신)은 이 함수가 아니라 매 틱 1회
	//   ProcessRawLog() 가 직접 수행 — 이 함수는 순수 판정만 (2026-08-25 최정우 추가,
	//   [[project_duplicate_trip_end_event_speed_charge]] 참고)
	static bool IsTrustedTripEnd(const sRawLogInfo& stRawLogInfo, const VEHICLE_TRIP_SESSION& stSession);
	// 하버사인: WGS84 경위도(도) 두 점 사이 지표거리(m) (2026-07-08 최정우 추가)
	static double HaversineMeters(const POINT& stA, const POINT& stB);
	// INTERSECT_LEN: GPS↔세그먼트 교차점(MATCH_LAT/LON) 하버사인 거리(m) 반올림
	static int CalcIntersectLen(const sRawLogInfo& stRawLogInfo, double dfMatchLon, double dfMatchLat);

private:
	// [버그 수정, 2026-09-17 최정우] `{}` 값 초기화 — 종전에는 생성자에서
	//   memset(&m_stConfig, 0, sizeof(m_stConfig)) 로 0을 채웠는데, 이 구조체에는 std::string 이
	//   6개(strUpdateSQL/strChargeInsertSQL/strTripEndUpdateSQL/strAbnormalTripEndSQL/
	//   strTripSeqOffSQL/strTripSeqFinSQL) 있어 내부 포인터가 nullptr 로 파괴됐다. 그 상태에서
	//   SetConfig() 의 `m_stConfig = stConfig` 대입이 **빈 문자열을 받는 멤버**를 만나면
	//   _M_set_length() 가 nullptr[0] 에 '\0' 을 써서 기동 즉시 세그폴트한다(재현 확인).
	//   지금까지 안 죽은 것은 query.sql 의 6개 섹션이 모두 로드돼 우연히 전부 비어있지 않았기
	//   때문이다 — 그런데 이 값들은 설계상 "비어있으면 그 기능 비활성"으로 허용돼 있어
	//   (각 필드 주석 참고), 섹션 하나가 누락·오타로 안 읽히면 그대로 죽는다.
	//   `{}` 는 string 은 기본 생성자로, POD 멤버는 0 으로 초기화하므로 memset 의 의도를
	//   그대로 지키면서 UB 만 제거한다. POD 값은 어차피 SetConfig() 가 전부 덮어쓴다.
	//   되돌리는 법: `{}` 를 지우고 생성자에 memset 을 되살리면 종전 동작(UB 포함)으로 복귀.
	//   [정정] 현재 string 멤버는 4개다(strUpdateSQL/strChargeInsertSQL/strTripEndUpdateSQL/
	//   strTripSeqMaxSQL) — strAbnormalTripEndSQL·strTripSeqOffSQL·strTripSeqFinSQL 은 2026-10-04 삭제
	//   (2026-10-06 최정우 주석 수정)
	RAWLOG_WORKER_CONFIG				m_stConfig{};
	vector<unordered_map<string, VEHICLE_TRIP_SESSION> > m_vtTripSessions;
	CGISUtil							m_cGISUtil;							// 방위각(GetDirAngleDegree) 계산용, stateless (2026-07-08 최정우 추가)
	CCodeMap							m_cCodeMap;							// NON_CHARGE_REASON 코드→메시지 조회용, stateless
																			//   (MapMatch.cpp 의 ErrorCodeTable/m_cCodeMap 과 동일
																			//   패턴, 2026-09-11 최정우 추가)
};

#endif //__RAWLOGWORKER_H__

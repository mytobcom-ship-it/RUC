/**
 * @file GISUtil.cpp
 * @brief 맵매칭 GIS 유틸리티 클래스 소스 파일 (WGS84 경위도 용)
*/
#include "GISUtil.h"
#include "MessageType.h"
#include <algorithm>

namespace {

/**
 * @brief 두 경위도(WGS84, **도 단위** — ×360000 스케일 아님) 사이 하버사인 거리
 * @param[in] dfLon1 점1 경도(도)
 * @param[in] dfLat1 점1 위도(도)
 * @param[in] dfLon2 점2 경도(도)
 * @param[in] dfLat2 점2 위도(도)
 * @return 거리(m). 지구 반경은 WGS84 장반경 6378137m
 * @remark GridBorderDistance() 전용 — 스케일 좌표를 넣으면 수천 km 단위로 틀린다
 *         (TryNearbyRoadNameCandidate 2026-09-15 버그가 이 단위 혼동이었다) (2026-10-06 최정우 주석 추가)
*/
double HaversineMetersDeg(const double dfLon1, const double dfLat1,
		const double dfLon2, const double dfLat2)
{
	const double dfR = 6378137.0;
	const double dfLat1Rad = RAD(dfLat1);
	const double dfLat2Rad = RAD(dfLat2);
	const double dfDLat = RAD(dfLat2 - dfLat1);
	const double dfDLon = RAD(dfLon2 - dfLon1);

	double dfA = sin(dfDLat / 2.0) * sin(dfDLat / 2.0)
		+ cos(dfLat1Rad) * cos(dfLat2Rad) * sin(dfDLon / 2.0) * sin(dfDLon / 2.0);
	if (dfA > 1.0) dfA = 1.0;
	return 2.0 * dfR * asin(sqrt(dfA));
}

// 좌표정수(도×360000) 두 점 사이 방위각(도, 0=북 시계방향) — 경도축(dfX)에 cos(위도) 보정을
//   곱하지 않으면 고위도·동서방향 세그먼트일수록 각도가 부정확해짐(실측 000376_20260826160622
//   seq102: 저장된 wDirAng=248 vs 실제 243.26, 오차 4.74도가 154m 세그먼트에서 11.9m 옆으로
//   밀림 — GetDirAngle()/GetDirAngleDegree() 둘 다 이 보정이 빠져 있었음) (2026-08-26 최정우 추가)
double BearingDegScaled(const double dfX1Scaled, const double dfY1Scaled,
		const double dfX2Scaled, const double dfY2Scaled)
{
	const double dfLatRad = RAD(((dfY1Scaled + dfY2Scaled) / 2.0) / 360000.0);
	const double dfDx = (dfX2Scaled - dfX1Scaled) * cos(dfLatRad);
	const double dfDy = dfY2Scaled - dfY1Scaled;
	double dfAngle = DEG(atan2(dfDx, dfDy));
	if (dfAngle < 0) dfAngle += 360.0;
	else if (dfAngle >= 360.0) dfAngle -= 360.0;
	return dfAngle;
}

// 좌표정수(도×360000) 지점에서 방위각(dfBearingDeg)·실거리(dfMeters, m)만큼 이동한 좌표정수 반환.
//   평면(등장방형) 근사 — 세그먼트 길이(최대 수백m) 범위에서 오차 무시 가능. 경도축은 cos(위도)
//   보정 필수(안 하면 SgmtMatch() 의 옛 dfVertexDistance 처럼 부풀림/왜곡 발생) (2026-08-26 최정우 추가)
void OffsetScaledCoordByMeters(const double dfXScaled, const double dfYScaled,
		const double dfBearingDeg, const double dfMeters, double *pdfXScaled, double *pdfYScaled)
{
	const double dfR = 6378137.0;
	const double dfMetersPerDeg = dfR * RAD(1.0);
	const double dfLatRad = RAD(dfYScaled / 360000.0);
	const double dfDLatDeg = (dfMeters * cos(RAD(dfBearingDeg))) / dfMetersPerDeg;
	const double dfDLonDeg = (dfMeters * sin(RAD(dfBearingDeg))) / (dfMetersPerDeg * cos(dfLatRad));
	*pdfXScaled = dfXScaled + dfDLonDeg * 360000.0;
	*pdfYScaled = dfYScaled + dfDLatDeg * 360000.0;
}

} // namespace

/**
 * @brief 생성자
*/
CGISUtil::CGISUtil()
{
}

/**
 * @brief 소멸자
*/
CGISUtil::~CGISUtil()
{
}

/**
 * @brief GRID ID 계산
 * @param[in] dfX X 좌표
 * @param[in] dfY Y 좌표
 * @return GRID ID
*/
uint32 CGISUtil::GetGridID(double& dfX, double& dfY)
{
	// GetGridColNo/GetGridRowNo(바로 아래)는 범위를 벗어나면 INVALID_GRID_*_NO 를 반환하는데, 이
	//   함수는 그 방어 없이 바로 곱셈해서 반환하고 있었음 — 그리드 테이블 실제 범위(X_GRID_COUNT×
	//   GRID_CELL_SIZE)가 WGS84GEO_LON_MAX 보다 좁아서, 그 사이 경도값이 들어오면 dwColNo 가
	//   X_GRID_COUNT 를 넘겨 엉뚱한 행의 GRID ID 와 충돌(aliasing)함 — 해당 좌표가 속한 그리드가
	//   아니라 무관한 그리드를 조용히 조회하게 됨. GetGridColNo/GetGridRowNo 를 그대로 재사용해
	//   범위를 벗어나면 INVALID_GRID_ID 반환(호출측인 BeginMapMatch 는 이미 GetGridInfo() null
	//   체크로 "그리드 없음"을 정상 처리하므로 안전) (2026-08-14 최정우 수정 — "소스상 문제" 검토 중 발견)
	sint32 nColNo = GetGridColNo(dfX);
	sint32 nRowNo = GetGridRowNo(dfY);
	if ((nColNo == INVALID_GRID_COL_NO) || (nRowNo == INVALID_GRID_ROW_NO))
		return static_cast<uint32>(INVALID_GRID_ID);

	return (static_cast<uint32>(nRowNo) * X_GRID_COUNT) + static_cast<uint32>(nColNo);
}

/**
 * @brief GRID X 좌표 번호 계산
 * @param[in] dfX X 좌표
 * @return GRID X 좌표 번호
*/
sint32 CGISUtil::GetGridColNo(double& dfX)
{
	sint32 nColNo = floor((dfX - WGS84GEO_LON_MIN) / GRID_CELL_SIZE);
	if ((nColNo < 0) || (nColNo >= X_GRID_COUNT))
		return INVALID_GRID_COL_NO;

	return nColNo;
}

/**
 * @brief GRID Y 좌표 번호 계산
 * @param[in] dfY Y 좌표
 * @return GRID Y 좌표 번호
*/
sint32 CGISUtil::GetGridRowNo(double& dfY)
{
	sint32 nRowNo = floor((dfY - WGS84GEO_LAT_MIN) / GRID_CELL_SIZE);
	if ((nRowNo < 0) || (nRowNo >= Y_GRID_COUNT))
		return INVALID_GRID_ROW_NO;

	return nRowNo;
}

/**
 * @brief 세그먼트 GRID 교차 확인
 * @warning [2026-09-15 최정우 확인] **MapMatchSvr 전체에서 호출부 0건인 dead code 다.**
 *   (CreateData 쪽 동명 함수도 마찬가지로 호출부 0건.) 아래 IsCrossSgmt2Sgmt 가 평행·공선
 *   세그먼트를 교차로 판정하지 못하는 실제 결함을 갖고 있으나(dfSign==0.0 이면 무조건 false),
 *   이 함수가 안 불리므로 맵매칭 결과에 영향을 줄 수 없다.
 *   [2026-09-21 최정우 수정] 2026-09-11 리뷰에서 "겹침까지 기하 케이스 분석이 필요하다"며
 *   보류했던 그 평행·공선 결함은 **이번에 IsCrossSgmt2Sgmt() 안에서 수정 완료**했다(사용자
 *   지시 — 호출부가 없더라도 되살리는 순간 발현하므로 미리 고쳐둔다). 비평행 경로는 그대로
 *   두고 공선 중첩 판정만 덧붙여, 기존에 true 였던 입력의 결과는 바뀌지 않는다.
 *   같은 파일 GetDistanceGEO2()/GetSgmtLength() 도 같은 성격(호출부 0 + 결함 선수정)이다.
 * @param[in] stPoint1 세그먼트 시작 X,Y 좌표
 * @param[in] stPoint2 세그먼트 종료 X,Y 좌표
 * @param[in] dwGridColNo X 좌표 GRID 번호
 * @param[in] dwGridRowNo Y 좌표 GRID 번호
 * @return true(성공), false(실패)
*/
bool CGISUtil::IsCrossSgmt2Grid(POINT& stPoint1, POINT& stPoint2, 
		uint32& dwGridColNo, uint32& dwGridRowNo)
{
	// GRID 번호로 X,Y 최소, 최대 좌표 구하기
	bool bRet = false;
	double dfGridXMin = WGS84GEO_LON_MIN + dwGridColNo * GRID_CELL_SIZE;
	double dfGridXMax = WGS84GEO_LON_MIN + (dwGridColNo + 1) * GRID_CELL_SIZE;
	double dfGridYMin = WGS84GEO_LAT_MIN + dwGridRowNo * GRID_CELL_SIZE;
	double dfGridYMax = WGS84GEO_LAT_MIN + (dwGridRowNo + 1) * GRID_CELL_SIZE;

	// GRID 경계(4변)와 세그먼트 교차 여부 판정 (2026-07-08 최정우 주석 추가)
	if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMin, dfGridYMin, dfGridXMin, dfGridYMax))
		bRet = true;
	else if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMin, dfGridYMax, dfGridXMax, dfGridYMax))
		bRet = true;
	else if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMax, dfGridYMax, dfGridXMax, dfGridYMin))
		bRet = true;
	else if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMax, dfGridYMin, dfGridXMin, dfGridYMin))
		bRet = true;

	return bRet;
}

/**
 * @brief 세그먼트와 세그먼트 교차 확인
 * @param[in] stPoint1 세그먼트 시작 X,Y 좌표
 * @param[in] stPoint2 세그먼트 종료 X,Y 좌표
 * @param[in] dfXMin 세그먼트 시작 X 좌표 (GRID)
 * @param[in] dfYMin 세그먼트 시작 Y 좌표 (GRID)
 * @param[in] dfXMax 세그먼트 종료 X 좌표 (GRID)
 * @param[in] dfYMax 세그먼트 종료 Y 좌표 (GRID)
 * @return true(성공), false(실패)
*/
bool CGISUtil::IsCrossSgmt2Sgmt(POINT& stPoint1, POINT& stPoint2, 
		double& dfXMin, double& dfYMin, double& dfXMax, double& dfYMax)
{
	bool bRet = false;
	double dfSign = (dfYMax - dfYMin) * (stPoint2.dfX - stPoint1.dfX) - (dfXMax - dfXMin) * (stPoint2.dfY - stPoint1.dfY);
	// [주석 정정, 2026-09-21 최정우] 종전 주석이 "교차하지 않음" 이라 **조건과 정반대**로 읽혔다.
	//   dfSign 은 두 세그먼트 방향벡터의 외적이라 0 이면 평행(또는 공선)이라는 뜻이고, 여기 들어오는
	//   것은 그 반대인 "평행하지 않음 = 교차점이 존재할 수 있음" 이다. 실제로 교차하는지는 아래
	//   매개변수 dfSign1·dfSign2 가 둘 다 0~1 범위인지로 판정한다.
	if (dfSign != 0.0)					// 평행이 아님 — 교차점이 존재할 수 있음
	{
		double dfSign1 = ((dfXMax - dfXMin) * (stPoint1.dfY - dfYMin) - (dfYMax - dfYMin) * (stPoint1.dfX - dfXMin)) / dfSign;
		double dfSign2 = ((stPoint2.dfX - stPoint1.dfX) * (stPoint1.dfY - dfYMin) - (stPoint2.dfY - stPoint1.dfY) * (stPoint1.dfX -dfXMin)) / dfSign;

		if ((dfSign1 >= 0.0 && dfSign1 <= 1.0) && (dfSign2 >= 0.0 && dfSign2 <= 1.0))		// 교차함
			bRet = true;

		return bRet;
	}

	// [버그 수정, 2026-09-21 최정우] 여기는 dfSign == 0, 즉 **두 세그먼트가 평행하거나 같은
	//   직선 위에 있는** 경우다. 종전에는 이 경우를 통째로 "교차 안 함"으로 처리했다 — 평행은
	//   맞지만, **같은 직선 위에서 구간이 겹치는 경우(공선 중첩)까지 놓쳤다.** 이 함수의 유일한
	//   호출자인 IsCrossSgmt2Grid() 는 그리드의 네 변과 세그먼트를 비교하므로, 도로 세그먼트가
	//   그리드 경계선과 정확히 겹쳐 놓이면(격자에 나란한 직선 도로) 교차를 못 잡는다.
	//   위 비평행 경로는 한 글자도 바꾸지 않았다 — 기존에 true 였던 입력은 그대로 true 이고,
	//   여기서 늘어나는 것은 종전에 놓치던 공선 중첩뿐이다(false negative 만 감소).
	//   ※ 이 함수 계열은 현재 호출부가 0 이다(IsCrossSgmt2Grid 의 @warning 참고). 되살려 쓸 때
	//     바로 발현할 결함이라 미리 고쳐둔다(사용자 지시).

	// 같은 직선 위인지 — 세그먼트 B(그리드 변)의 방향벡터와 "B 시작점 → A 시작점" 벡터의 외적이
	//   0 이어야 공선이다. 0 이 아니면 나란하기만 할 뿐 절대 만나지 않는다.
	const double dfCollinear = (dfXMax - dfXMin) * (stPoint1.dfY - dfYMin)
		- (dfYMax - dfYMin) * (stPoint1.dfX - dfXMin);
	if (dfCollinear != 0.0)
		return false;					// 평행하지만 다른 직선 — 만나지 않음

	// 공선이므로 두 구간이 실제로 겹치는지만 보면 된다. 축별 투영 구간이 **둘 다** 겹쳐야 한다
	//   (수직·수평선에서도 한쪽 축은 폭이 0 이라 그대로 성립한다).
	const double dfAMinX = (stPoint1.dfX < stPoint2.dfX) ? stPoint1.dfX : stPoint2.dfX;
	const double dfAMaxX = (stPoint1.dfX > stPoint2.dfX) ? stPoint1.dfX : stPoint2.dfX;
	const double dfAMinY = (stPoint1.dfY < stPoint2.dfY) ? stPoint1.dfY : stPoint2.dfY;
	const double dfAMaxY = (stPoint1.dfY > stPoint2.dfY) ? stPoint1.dfY : stPoint2.dfY;
	const double dfBMinX = (dfXMin < dfXMax) ? dfXMin : dfXMax;
	const double dfBMaxX = (dfXMin > dfXMax) ? dfXMin : dfXMax;
	const double dfBMinY = (dfYMin < dfYMax) ? dfYMin : dfYMax;
	const double dfBMaxY = (dfYMin > dfYMax) ? dfYMin : dfYMax;

	// 경계 접촉(끝점만 맞닿음)도 교차로 인정한다 — 위 비평행 경로가 매개변수 범위를
	//   폐구간 [0,1] 로 보는 것과 같은 기준이다.
	bRet = (dfAMinX <= dfBMaxX) && (dfAMaxX >= dfBMinX)
		&& (dfAMinY <= dfBMaxY) && (dfAMaxY >= dfBMinY);

	return bRet;
}

/**
 * @brief 세그먼트 거리 (0.01 sec)
 *        [정정] 2026-09-11 수정 이후 "0.01 sec(도×360000)" 단위가 아니다 — 입력은 **순수 WGS84 도**
 *        (GetDistanceGEO2 위임), 반환은 **미터**(반올림, 최소 1)다. 호출부 0건(dead code) (2026-10-06 최정우 주석 수정)
 * @param[in] stPoint1 세그먼트 시작 좌표
 * @param[in] stPoint2 세그먼트 종료 좌표
 * @return 세그먼트 길이
*/
uint16 CGISUtil::GetSgmtLength(const POINT& stPoint1, const POINT& stPoint2)
{
	// [버그 수정, 2026-09-11 최정우] CreateData/src/GISUtil.cpp가 2026-08-24에 이미 고친 것과
	//   동일 버그 — 기존 구현은 경위도 차이에 360000(내부 좌표 스케일)만 곱하고 실제 미터 환산
	//   (1도≈111,320m, 경도는 cos(위도) 보정 필요)을 하지 않아 위도 37°대에서 약 3.2~4.1배
	//   부풀려진 값을 돌려주고 있었다 — GetDistanceGEO2()(방금 위에서 단위버그를 같이 고침,
	//   입력 단위: 순수 WGS84 도)로 위임해 실제 지리 거리(m)를 구하도록 수정. 현재 이 함수
	//   호출부가 없어(dead code) 실피해는 없었지만, "고쳐진 코드"로 착각하고 재사용할 때 즉시
	//   재발하는 걸 막는다.
	POINT stP1 = stPoint1;
	POINT stP2 = stPoint2;
	uint16 wLenSgmt = static_cast<uint16>(round(GetDistanceGEO2(stP1, stP2)));
	if (wLenSgmt <= 0) wLenSgmt = 1;

	return wLenSgmt;
}

/**
 * @brief GRID 경계·모서리까지의 거리(m) — nRadius(미터)와 단위 정합 (#C-1)
 * @param[in] dwGridID GRID ID
 * @param[in] dfX X 좌표 (도, WGS84)
 * @param[in] dfY Y 좌표 (도, WGS84)
 * @param[out] stGridBorderDist GRID 경계 거리(모서리 거리 포함, m)
 * @return 8방향 중 최소 거리(m)
*/
double CGISUtil::GridBorderDistance(const uint32& dwGridID, const double& dfX, 
		const double& dfY, GRID_BORDER_DIST& stGridBorderDist)
{
	uint32 dwRowNo = floor(dwGridID / X_GRID_COUNT);
	uint32 dwColNo = floor(dwGridID - dwRowNo * X_GRID_COUNT);

	set<double> setDistance;
	set<double>::iterator it;
	setDistance.clear();

	double dfXMin = WGS84GEO_LON_MIN + dwColNo * GRID_CELL_SIZE;
	double dfYMin = WGS84GEO_LAT_MIN + dwRowNo * GRID_CELL_SIZE;
	double dfXMax = WGS84GEO_LON_MIN + (dwColNo + 1) * GRID_CELL_SIZE;
	double dfYMax = WGS84GEO_LAT_MIN + (dwRowNo + 1) * GRID_CELL_SIZE;

	stGridBorderDist.dfLeftDist = HaversineMetersDeg(dfX, dfY, dfXMin, dfY);
	setDistance.insert(stGridBorderDist.dfLeftDist);

	stGridBorderDist.dfBottomDist = HaversineMetersDeg(dfX, dfY, dfX, dfYMin);
	setDistance.insert(stGridBorderDist.dfBottomDist);

	stGridBorderDist.dfRightDist = HaversineMetersDeg(dfX, dfY, dfXMax, dfY);
	setDistance.insert(stGridBorderDist.dfRightDist);

	stGridBorderDist.dfTopDist = HaversineMetersDeg(dfX, dfY, dfX, dfYMax);
	setDistance.insert(stGridBorderDist.dfTopDist);

	stGridBorderDist.dfLeftTopDist = HaversineMetersDeg(dfX, dfY, dfXMin, dfYMax);
	setDistance.insert(stGridBorderDist.dfLeftTopDist);

	stGridBorderDist.dfLeftBottomDist = HaversineMetersDeg(dfX, dfY, dfXMin, dfYMin);
	setDistance.insert(stGridBorderDist.dfLeftBottomDist);

	stGridBorderDist.dfRightTopDist = HaversineMetersDeg(dfX, dfY, dfXMax, dfYMax);
	setDistance.insert(stGridBorderDist.dfRightTopDist);

	stGridBorderDist.dfRightBottomDist = HaversineMetersDeg(dfX, dfY, dfXMax, dfYMin);
	setDistance.insert(stGridBorderDist.dfRightBottomDist);

	it = setDistance.begin();
	return *it;
}

/**
 * @brief GRID 9등분 인덱스 위치 및 경계 좌표
 * @warning [미사용 경고, 2026-09-21 최정우 확인] 이 함수는 전 소스에서 호출부가 0 건인 dead code
 *   다(같은 파일의 IsCrossSgmt2Grid·GetSgmtLength·GetDistanceGEO2 와 같은 성격).
 * @param[in] dwGridID GRID ID
 * @param[in] dfX X 좌표 (도, WGS84)
 * @param[in] dfY Y 좌표 (도, WGS84)
 * @return 인덱스 (0 ~ 8). 좌표가 해당 그리드 밖이면 INVALID_GRID_SPLIT_INDEX(0xFF)
*/
uint8 CGISUtil::GridSplitIndex(const uint32& dwGridID, const double& dfX, const double& dfY)
{
	uint32 dwRowNo = floor(dwGridID / X_GRID_COUNT);
	uint32 dwColNo = floor(dwGridID - dwRowNo * X_GRID_COUNT);

	double dfXMin = WGS84GEO_LON_MIN + dwColNo * GRID_CELL_SIZE;
	double dfYMin = WGS84GEO_LAT_MIN + dwRowNo * GRID_CELL_SIZE;

	// [버그 수정, 2026-09-21 최정우] 종전에는 나눗셈 결과를 곧바로 uint8 로 잘라 인덱스로 썼다.
	//   좌표가 그 그리드의 오른쪽/위쪽 경계에 정확히 걸치거나(비율 3.0) 아예 다른 그리드의
	//   좌표가 들어오면(호출측이 dwGridID 와 좌표를 짝 맞춰 주지 않는 경우) nXIndex·nYIndex 가
	//   3 이상이 되어 **반환값이 문서화된 0~8 범위를 넘는다**(예: 3*3+3=12). 그 값을 9칸짜리
	//   배열 첨자로 쓰면 그대로 버퍼 오버런이다. 음수 쪽도 마찬가지로 uint8 캐스트에서 큰 값으로
	//   뒤집힌다. 범위를 벗어나면 명시적인 실패값을 돌려주도록 고친다.
	//   현재 호출부가 없어 동작 변화는 없다 — 되살릴 때 반환값을 반드시 검사할 것.
	const double dfSplit = GRID_CELL_SIZE / 3.0;
	const double dfXRatio = (dfX - dfXMin) / dfSplit;
	const double dfYRatio = (dfY - dfYMin) / dfSplit;
	if ((dfXRatio < 0.0) || (dfXRatio >= 3.0) || (dfYRatio < 0.0) || (dfYRatio >= 3.0))
		return INVALID_GRID_SPLIT_INDEX;

	uint8 nXIndex = static_cast<uint8>(dfXRatio);
	uint8 nYIndex = static_cast<uint8>(dfYRatio);

	return static_cast<uint8>(nYIndex * 3 + nXIndex);
}

/**
 * @brief 인접 GRID ID 계산 (중심 그리드 제외, 최대 8방향)
 * @param[in] dwGridID GRID ID
 * @param[in] stSgmtMatchInput 맵매칭 입력 정보
 * @param[in,out] vtNearGridIDList 인접 GRID ID 목록
 * @return void
 * @remark 2026-07-08 최정우 수정
 *   유효 인덱스: col 0..X_GRID_COUNT-1, 행 0..Y_GRID_COUNT-1
 *   경계 검사 (off-by-one 방지):
 *     · 서/남서/남: col>0 또는 행>0
 *     · 동/북/대각: col+1<X_GRID_COUNT, 행+1<Y_GRID_COUNT
 *   반경(nRadius)이 셀 경계거리보다 크면 해당 방향 이웃만 push.
 *   잘못된 ID(다음 행 col=0 등) 산출 시 엉뚱한 그리드 검색 → INVALID_GRID_ID 유지.
*/
void CGISUtil::GetNearGridID(const uint32& dwGridID, const SGMT_MATCH_INPUT& stSgmtMatchInput, 
		vector<uint32>& vtNearGridIDList)
{
	uint32 dwRowNo = floor(dwGridID / X_GRID_COUNT);
	uint32 dwColNo = floor(dwGridID - dwRowNo * X_GRID_COUNT);

	GRID_BORDER_DIST stGridBorderDist;
	sint32 nGridLeftID = INVALID_GRID_ID;
	sint32 nGridRightID = INVALID_GRID_ID;
	sint32 nGridBottomID = INVALID_GRID_ID;
	sint32 nGridTopID = INVALID_GRID_ID;
	sint32 nGridLeftTopID = INVALID_GRID_ID;
	sint32 nGridLeftBottomID = INVALID_GRID_ID;
	sint32 nGridRightTopID = INVALID_GRID_ID;
	sint32 nGridRightBottomID = INVALID_GRID_ID;

	// GRID 경계 거리
	GridBorderDistance(dwGridID, stSgmtMatchInput.stPoint.dfX, stSgmtMatchInput.stPoint.dfY, stGridBorderDist);

	// 인접 GRID ID (맵 경계 밖은 INVALID_GRID_ID — BeginMapMatch에서 GetGridInfo null 시 건너뜀)
	if (dwColNo > 0)
		nGridLeftID = static_cast<sint32>(dwRowNo * X_GRID_COUNT + dwColNo - 1);

	if (dwColNo + 1 < X_GRID_COUNT)
		nGridRightID = static_cast<sint32>(dwRowNo * X_GRID_COUNT + dwColNo + 1);

	if (dwRowNo > 0)
		nGridBottomID = static_cast<sint32>((dwRowNo - 1) * X_GRID_COUNT + dwColNo);

	if (dwRowNo + 1 < Y_GRID_COUNT)
		nGridTopID = static_cast<sint32>((dwRowNo + 1) * X_GRID_COUNT + dwColNo);

	if (dwColNo > 0 && dwRowNo + 1 < Y_GRID_COUNT)
		nGridLeftTopID = static_cast<sint32>((dwRowNo + 1) * X_GRID_COUNT + dwColNo - 1);

	if (dwColNo > 0 && dwRowNo > 0)
		nGridLeftBottomID = static_cast<sint32>((dwRowNo - 1) * X_GRID_COUNT + dwColNo - 1);

	if (dwColNo + 1 < X_GRID_COUNT && dwRowNo + 1 < Y_GRID_COUNT)
		nGridRightTopID = static_cast<sint32>((dwRowNo + 1) * X_GRID_COUNT + dwColNo + 1);

	if (dwColNo + 1 < X_GRID_COUNT && dwRowNo > 0)
		nGridRightBottomID = static_cast<sint32>((dwRowNo - 1) * X_GRID_COUNT + dwColNo + 1);

	// 조건에 맞는 인접 GRID ID 구하기
	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfLeftDist) && 
		(nGridLeftID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridLeftID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfRightDist) && 
		(nGridRightID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridRightID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfTopDist) && 
		(nGridTopID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridTopID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfBottomDist) && 
		(nGridBottomID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridBottomID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfLeftTopDist) && 
		(nGridLeftTopID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridLeftTopID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfLeftBottomDist) && 
		(nGridLeftBottomID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridLeftBottomID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfRightTopDist) && 
		(nGridRightTopID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridRightTopID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfRightBottomDist) && 
		(nGridRightBottomID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridRightBottomID));
}

/**
 * @brief 좌표와 세그먼트 매핑
 * @param[in] stSgmtMatchInput 세그먼트 매칭 정보
 *            — stPoint 는 **도×360000 스케일** 이어야 한다(Begin/Continue StartMapMatch 가 변환 후 호출).
 *            nRadius 는 미터 (2026-10-06 최정우 주석 추가)
 * @param[in] stSgmtInfo 세그먼트 정보 (stPoint=도×360000, nDirAng=도, dfLen=wLenSgmt 미터)
 *            (2026-10-06 최정우 주석 수정)
 * @param[out] pstSgmtMatchRes 세그먼트 맵매칭 결과
 * @param[in] bIgnoreRadiusCheck true 이면 nRadius 초과여도 기하 매칭 허용(진단용 최근접) (2026-07-10 최정우 수정)
 * @param[in] bIgnoreHeading true 이면 heading 이 있어도 없는 것으로 취급 — 방위각 하드컷
 *            (MM_DIR_MAX_DEG)·소프트 비용을 모두 적용하지 않고 거리만으로 판정한다.
 *            BEGIN 매칭 경로가 이 값을 true 로 넘긴다 (2026-09-21 최정우 주석 보완 —
 *            설명이 없던 파라미터)
 * @return true(성공), false(실패)
*/
bool CGISUtil::SgmtMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, SGMT_INFO& stSgmtInfo, SGMT_MATCH_RES *pstSgmtMatchRes,
		bool bIgnoreRadiusCheck, bool bIgnoreHeading)
{
	if ((stSgmtMatchInput.stPoint.dfX == stSgmtInfo.stPoint.dfX) && 
		(stSgmtMatchInput.stPoint.dfY == stSgmtInfo.stPoint.dfY))
	{
		pstSgmtMatchRes->stMatchPoint.dfX = stSgmtInfo.stPoint.dfX;
		pstSgmtMatchRes->stMatchPoint.dfY = stSgmtInfo.stPoint.dfY;
		pstSgmtMatchRes->dfSgmtMatchLen = 0;
		pstSgmtMatchRes->dfIntersectLenSgmt = 0;
		pstSgmtMatchRes->dfCost = 0;			// 정확히 일치 → 최소 비용 (2026-07-08 최정우 추가)
		pstSgmtMatchRes->nDirAngleDiff = 0;
		pstSgmtMatchRes->qwLinkID = stSgmtInfo.qwLinkID;
		return true;
	}

	// 2026-07-08 최정우 주석 처리
#if 0
	if (stSgmtMatchInput.nDirAng != NO_ANGLE)
	{
		// 각도 차이
		if (abs(GetAngleDiff(stSgmtInfo.nDirAng, stSgmtMatchInput.nDirAng)) > 45)
			return false;
	}
#endif

	// 차량 방위각 vs 세그먼트 방위각 차이 (heading 있을 때만).
	// ±45 하드컷 대신 소프트 비용 사용 (2026-07-08 최정우 수정)
	sint16 nHeadingDiff = 0;
	bool bReverseFit = false;
	// bIgnoreHeading=true(BEGIN 매칭 경로가 넘긴다)면 heading 있어도 없는 것처럼 취급 —
	//   하드컷(MM_DIR_MAX_DEG)·소프트 비용 전부 미적용, 거리만으로 판정
	// [주석 정정, 2026-09-21 최정우] 종전 주석은 이를 "실험용, 임시 추가" 로 적었으나 임시가
	//   아니라 확정된 설계다 — 근거는 BeginMapMatch.cpp 의 같은 날짜 정정 주석 참고.
	// 저속(MM_SPEED_LOW_KMH 이하)이면 heading 도 없는 것처럼 취급 — 정차 중엔 실제 진행방향을
	//   계산할 수 없어 GPS/OBD 가 관례적으로 0(정북) 등 부정확한 값을 채워 넣는 경우가 흔한데,
	//   이 값을 그대로 믿고 bReverseFit(역방향 적합) 판정에 쓰면 우연히 세그먼트 역방향과 맞아떨어져
	//   정차 중 GPS 노이즈를 "역행 의심"으로 오판, reverse_confirm 스트릭을 채워 SKIP 남발 —
	//   실측 900376_20260826160622 seq123/124/131/132(정차, speed=0, heading=0)로 확인
	//   (2026-08-26 최정우 추가). 저속일 때 heading 을 안 믿는다는 원칙 자체는 아래 dfAnglePenalty
	//   가중치(w_a=0)에 이미 있었으나 이 bReverseFit 판정 경로엔 빠져 있었음 — 동일 임계 재사용
	//   (nSpeed<0=NO_SPEED 는 그대로 heading 신뢰, ContinueMapMatch.cpp:623 브릿지 판정과 동일 관례)
	//   [정정] ":623" 은 당시 줄 번호 — CContinueMapMatch::GetLinkDepthInfo() 의 BridgeNearbyLinkStarts
	//   호출 조건 "(nSpeed < 0) || (nSpeed > MM_SPEED_LOW_KMH)" 를 가리킨다 (2026-10-06 최정우 주석 수정)
	bool bHasHeading = (!bIgnoreHeading) && (stSgmtMatchInput.nDirAng != NO_ANGLE)
		&& ((stSgmtMatchInput.nSpeed < 0) || (stSgmtMatchInput.nSpeed > MM_SPEED_LOW_KMH));
	if (bHasHeading)
	{
		// 양방향 단일 링크 대응: 세그먼트 F→T 방위각과 그 반대(T→F=+180°) 둘 다 비교해
		//   더 가까운 쪽을 heading 차이로 채택 → 반대방향 주행도 정상 매칭(120° 오배제 방지) (2026-07-16 최정우 수정)
		sint16 nSegDirFwd = stSgmtInfo.nDirAng;
		sint16 nSegDirRev = static_cast<sint16>((stSgmtInfo.nDirAng + 180) % 360);
		sint16 nDiffFwd = GetAngleDiff(nSegDirFwd, stSgmtMatchInput.nDirAng);
		sint16 nDiffRev = GetAngleDiff(nSegDirRev, stSgmtMatchInput.nDirAng);
		// 역방향이 더 잘 맞아서 채택되는지 표시 — 링크 방향성 정보가 없는 상태에서
		//   역주행 의심을 사후에 걸러내기 위한 신호(단정 아님) (2026-07-18 최정우 추가)
		bReverseFit = (abs(nDiffRev) < abs(nDiffFwd));
		nHeadingDiff = bReverseFit ? nDiffRev : nDiffFwd;
		// 하드 상한: 정·역 어느 쪽으로도 크게 어긋난(≈수직 이상) 후보만 배제. 그 안은 소프트 비용으로 경쟁
		// [정정] 이 하드컷은 **발동할 수 없다**. nHeadingDiff 는 정·역 두 각도차 중 작은 쪽이고
		//   |정방향차| + |역방향차| = 180° 이므로 그 값은 최대 90° 다 — MM_DIR_MAX_DEG(120°)를 넘을 수 없다.
		//   즉 2026-07-16 양방향 비교 도입 이후 heading 으로 후보를 배제하는 경로는 여기 없고, 각도는
		//   아래 소프트 비용(상한 MM_DIR_MAX_PENALTY)으로만 반영된다. 역방향 링크 차단은 호출측
		//   (Continue 의 짝 링크 보유 시 bReverseFit 후보 제외, Begin 의 FixOppositePairByHeading·FixReverseLinkByAzimuth·IsAntiHeadingOpposite)이 맡는다
		//   (2026-10-06 최정우 주석 수정)
		if (abs(nHeadingDiff) > MM_DIR_MAX_DEG)
			return false;
	}

	// 진행 각도
	sint16 nDirAngle = 0;
	if (!GetDirAngle(stSgmtInfo.stPoint, stSgmtMatchInput.stPoint, &nDirAngle))
		return false;

	// 진행 각도 차이
	sint16 nDirAngleDiff = GetAngleDiff(stSgmtInfo.nDirAng, nDirAngle);

	// 요청 좌표(GPS)와 세그먼트 시작점 사이 거리(m) — 예전엔 좌표정수(도×360000) 그대로 Euclidean
	//   계산해 "좌표정수단위" 값을 썼는데, wLenSgmt(dfSegLenCoord)가 2026-08-24 실제 미터로 수정되면서
	//   양쪽 단위가 어긋나 클램프(bSgmtClamped)가 남발되는 회귀가 생겼다 — 실측 000376_20260819094414
	//   seq39~41(세그먼트 22~76% 지점인데 클램프로 SKIP). GetDistanceGEO1 로 실제 GEO 거리(m)로 통일
	//   (2026-08-26 최정우 수정)
	double dfVertexDistance = GetDistanceGEO1(stSgmtMatchInput.stPoint, stSgmtInfo.stPoint);

	// ── INTERSECT_LEN (dfIntersectLenSgmt) — GPS 좌표와 세그먼트 교차점 거리(m) (2026-07-11 최정우 수정)
	//   stSgmtMatchInput.stPoint = GPS(요청) 좌표, stIntersect = 세그먼트 위 교차점(수선의 발 또는 끝점 snap)
	//   1) nDirAngle     = 세그먼트 시작점 → GPS 방향각
	//   2) nDirAngleDiff = 세그먼트 방위각 − nDirAngle
	//   3) dfIntersectSgmtDistance = dfVertexDistance × cos(nDirAngleDiff)   // 세그먼트 위 투영 길이(m)
	//   3b) dfSegLenCoord = stSgmtInfo.dfLen  (wLenSgmt, 실제 미터 — dfVertexDistance 와 동일 단위, 2026-08-26 수정)
	//   4) stIntersect   = 세그먼트 시작점에서 방위각 방향으로 dfIntersectSgmtDistance(m) 이동 (범위 밖이면 끝점 snap)
	//   5) dfIntersectLenSgmt = GetDistanceGEO1(GPS, stIntersect)             // 최종 GPS↔교차점 거리(m)
	//   ※ DB INTERSECT_LEN = (int)(dfIntersectLenSgmt + 0.5)
	double dfIntersectLenSgmt = 0;
	double dfIntersectSgmtDistance = dfVertexDistance * cos(RAD(static_cast<double>(nDirAngleDiff)));
	const double dfSegLenCoord = stSgmtInfo.dfLen;
	POINT stIntersect;
	// 수선의 발이 세그먼트 밖이라 끝점(꺾임점)으로 스냅되는지 — 여러 GPS_SEQ 가 같은 꺾임점으로
	//   뭉개지는(클램프) 저신뢰 매칭 판정용 신호 (2026-07-21 최정우 추가)
	bool bSgmtClamped = !((dfIntersectSgmtDistance <= dfSegLenCoord) && (dfIntersectSgmtDistance >= 0));

	if ((dfIntersectSgmtDistance <= dfSegLenCoord) && (dfIntersectSgmtDistance >= 0))		// 수선의 발이 세그먼트 위
	{
		// 세그먼트 시작점에서 세그먼트 방위각 방향으로 dfIntersectSgmtDistance(m) 이동 — 좌표정수 반환
		//   (2026-08-26 최정우 수정, dfIntersectSgmtDistance 실제 미터화에 맞춤)
		OffsetScaledCoordByMeters(stSgmtInfo.stPoint.dfX, stSgmtInfo.stPoint.dfY,
			static_cast<double>(stSgmtInfo.nDirAng), fabs(dfIntersectSgmtDistance),
			&stIntersect.dfX, &stIntersect.dfY);
	}
	else
	{
		// 수선 발이 세그먼트 밖이면 가까운 끝점으로 snap (#C-2)
		if (dfIntersectSgmtDistance > dfSegLenCoord)
		{
			OffsetScaledCoordByMeters(stSgmtInfo.stPoint.dfX, stSgmtInfo.stPoint.dfY,
				static_cast<double>(stSgmtInfo.nDirAng), dfSegLenCoord,
				&stIntersect.dfX, &stIntersect.dfY);
		}
		else
		{
			stIntersect.dfX = stSgmtInfo.stPoint.dfX;
			stIntersect.dfY = stSgmtInfo.stPoint.dfY;
		}
	}

	// GPS 좌표와 세그먼트 교차점(stIntersect) 사이 거리(m) → INTERSECT_LEN
	dfIntersectLenSgmt = GetDistanceGEO1(stSgmtMatchInput.stPoint, stIntersect);

	// 맵매칭 유효거리내에 포함되는지 검사 (진단 최근접 시 생략) (2026-07-10 최정우 수정)
	if (!bIgnoreRadiusCheck && (dfIntersectLenSgmt > stSgmtMatchInput.nRadius))
		return false;

	// 세그먼트 시작 좌표부터 매핑좌표까지 거리(m)
	double dfSgmtMatchLen = GetDistanceGEO1(stIntersect, stSgmtInfo.stPoint);

	// ── 링크 선택 비용(값이 작을수록 우선) — 쉬운 설명 (2026-07-08 최정우 추가) ──
	//   비용 = (GPS↔세그먼트 교차점 거리 INTERSECT_LEN, m) + (방위각 비용)
	//   방위각 비용 = 방향가중치 × (차량 방위각과 도로 방위각의 차이, 도)
	//     · 방향가중치 : 정지·저속(5km/h 이하)=0(방위각 무시) → 20km/h 이상=1.0(1도당 1m), 그 사이는 비례
	//     · 예) 거리 10m·각도차 40° → 10 + 1.0×40 = 50
	//            거리 30m·각도차  5° → 30 + 1.0×5 = 35  ⇒ 더 작은 35(방향 맞는 도로) 선택
	//   ※ 방위각 차이가 120°를 넘는 후보는 아예 제외(역방향 오매칭 방지)
	//   [정정] 위 첫 예시(50 vs 35)는 2026-07-18 상한 도입 전 계산이다 — 지금은 10+min(40,15)=25 가
	//   35 를 이겨 **가까운 10m 후보가 선택**된다. 또 120° 제외는 실제로 발동하지 않는다(정·역 중 작은
	//   각도차는 최대 90° — 위 하드컷 [정정] 참고) (2026-10-06 최정우 주석 수정)
	//   ※ 방위각 비용은 MM_DIR_MAX_PENALTY(15m)로 상한 — 근접 후보가 방위각 때문에 훨씬 먼
	//     후보에게 역전당하지 않도록 함(2026-07-18 최정우 추가). 예) 거리 5m·각도차 100° →
	//     5 + min(100, 15) = 20 vs 거리 40m·각도차 5° → 40 + 5 = 45 ⇒ 더 가까운 20(5m) 선택
	//
	//   [변수 매핑] dfCost = dfIntersectLenSgmt + min(dfDirWeight*|nHeadingDiff|, MM_DIR_MAX_PENALTY),
	//              nHeadingDiff = GetAngleDiff(세그먼트 방위각, 차량 방위각),
	//              dfDirWeight  = 속도(nSpeed)로 0~MM_DIR_WEIGHT 사이 결정
	// ────────────────────────────────────────────────────────────────────────────
	double dfDirWeight = MM_DIR_WEIGHT;
	if (stSgmtMatchInput.nSpeed >= 0)
	{
		if (stSgmtMatchInput.nSpeed <= MM_SPEED_LOW_KMH)
			dfDirWeight = 0.0;										// 저속: 방위각 미반영(거리만)
		else if (stSgmtMatchInput.nSpeed < MM_SPEED_HIGH_KMH)
			dfDirWeight = MM_DIR_WEIGHT
				* static_cast<double>(stSgmtMatchInput.nSpeed - MM_SPEED_LOW_KMH)
				/ static_cast<double>(MM_SPEED_HIGH_KMH - MM_SPEED_LOW_KMH);
	}
	double dfAnglePenalty = bHasHeading
		? std::min(dfDirWeight * fabs(static_cast<double>(nHeadingDiff)), MM_DIR_MAX_PENALTY)
		: 0.0;																// 방위각 비용 상한 캡 — 근접 후보 역전 방지 (2026-07-18 최정우 추가)

	pstSgmtMatchRes->stMatchPoint.dfX = stIntersect.dfX;
	pstSgmtMatchRes->stMatchPoint.dfY = stIntersect.dfY;
	pstSgmtMatchRes->dfSgmtMatchLen = dfSgmtMatchLen;
	pstSgmtMatchRes->dfIntersectLenSgmt = dfIntersectLenSgmt;
	pstSgmtMatchRes->dfCost = dfIntersectLenSgmt + dfAnglePenalty;		// 링크 선택 기준 (2026-07-08 최정우 추가)
	pstSgmtMatchRes->nDirAngleDiff = nDirAngleDiff;
	pstSgmtMatchRes->qwLinkID = stSgmtInfo.qwLinkID;
	pstSgmtMatchRes->bReverseFit = bReverseFit;						// heading이 역방향에 더 가까움 (2026-07-21 최정우 추가)
	pstSgmtMatchRes->bSgmtClamped = bSgmtClamped;						// 세그먼트 끝점 스냅 여부 (2026-07-21 최정우 추가)
	pstSgmtMatchRes->bHasHeading = bHasHeading;						// heading 값 존재 여부 (2026-07-22 최정우 추가)
	// 클램프됐어도 heading이 도로 방향과 잘 맞고(각도차 MM_CLAMP_HEADING_MAX_DIFF 이내) 속도가 충분히
	//   빠르면(MM_SPEED_HIGH_KMH 이상 — heading 신뢰도가 이미 최대인 구간, dfDirWeight 산정과 동일
	//   기준 재사용) 클램프 저신뢰 SKIP 판정에서 구제 — 단, 그래도 거리가 너무 멀면(MM_CLAMP_HEADING_SKIP_LEN
	//   초과) 신뢰하지 않음(이중 안전장치) (2026-08-20 최정우 추가)
	pstSgmtMatchRes->bClampTrustedByHeading = bSgmtClamped
		&& bHasHeading
		&& (stSgmtMatchInput.nSpeed >= MM_SPEED_HIGH_KMH)
		&& (abs(nHeadingDiff) <= MM_CLAMP_HEADING_MAX_DIFF)
		&& (dfIntersectLenSgmt <= MM_CLAMP_HEADING_SKIP_LEN);

	return true;
}

namespace {

/**
 * @brief 고가형 도로 여부 — ROAD_TYPE 1(고가차도)·3(교량)
 * @param[in] nRoadType LINK_INFO.nRoadType (eLinkRoadType, 국토교통부고시 제2023-22호 코드)
 * @return true(고가차도 또는 교량)
 * @remark (2026-10-06 최정우 주석 추가)
*/
bool IsElevatedRoad(uint8 nRoadType)
{
	return (nRoadType == ROAD_TYPE_ELEVATED || nRoadType == ROAD_TYPE_BRIDGE);
}

/**
 * @brief 지하형 도로 여부 — ROAD_TYPE 2(지하차도) **단독**. 4(터널)는 포함하지 않는다
 * @param[in] nRoadType LINK_INFO.nRoadType
 * @return true(지하차도)
 * @remark 2026-08-27 ROAD_TYPE 코드 정정으로 대상이 터널→지하차도로 바뀌었다(DataDefine.h
 *         eLinkRoadType 주석 참고) (2026-10-06 최정우 주석 추가)
*/
bool IsUndergroundRoad(uint8 nRoadType)
{
	return (nRoadType == ROAD_TYPE_UNDERGROUND);
}

/**
 * @brief 고도차가 alt_gap 이내일 때 후보·직전 ROAD_TYPE 을 "같은 층" 으로 볼지 판정
 * @param[in] nCandRoadType 후보 링크 ROAD_TYPE
 * @param[in] nPrevRoadType 직전 매칭 링크 ROAD_TYPE
 * @return true(호환 — 동일, 고가차도↔교량, 일반↔교량), false(그 외 — alt_penalty 가산 대상)
 * @remark (2026-10-06 최정우 주석 추가)
*/
bool IsRoadTypeCompatible(uint8 nCandRoadType, uint8 nPrevRoadType)
{
	if (nCandRoadType == nPrevRoadType)
		return true;
	if (IsElevatedRoad(nCandRoadType) && IsElevatedRoad(nPrevRoadType))
		return true;
	// 일반↔교량 호환 — 교량(BRIDGE)은 하천 위를 지나는 평탄한 시내 구간이 흔해(고가차도와 달리
	//   고도 변화가 거의 없음) 이 함수가 불리는 Δalt≈0(alt_gap 이내) 분기에서 후보가 교량이라는
	//   이유만으로 페널티를 주면 정답 후보가 부당하게 배제된다. 실측(link_id=2520216300 교차
	//   트립 4개·277점, 고도 35~44m로 진입~통과~진출 내내 완전히 평탄 — 실제 고가/교량성 고도
	//   변화 패턴 전혀 없음, G79 사례) 확인 후 사용자 지시로 추가 (2026-08-28 최정우 추가).
	//   고가(ELEVATED)는 대상에서 뺌 — 고가차도는 실제로 고도 변화가 뚜렷한 경우가 많아 그
	//   전제(Δalt≈0인데 고가면 의심)가 여전히 유효함
	bool bBridgeNormalPair =
		((nCandRoadType == ROAD_TYPE_BRIDGE) && (nPrevRoadType == ROAD_TYPE_NORMAL))
		|| ((nCandRoadType == ROAD_TYPE_NORMAL) && (nPrevRoadType == ROAD_TYPE_BRIDGE));
	if (bBridgeNormalPair)
		return true;
	return false;
}

/**
 * @brief 고도차가 alt_gap 을 넘을 때 방향과 맞지 않는 ROAD_TYPE 후보에 주는 추가 비용
 * @param[in] dfDeltaAlt 현재 GPS 고도 − 직전 매칭 GPS 고도(m)
 * @param[in] nCandRoadType 후보 링크 ROAD_TYPE
 * @param[in] stAltConfig config [mapmatch] alt_* 설정
 * @return 상승(Δ>+alt_gap)인데 후보가 지하차도면 +alt_penalty, 하강(Δ<−alt_gap)인데 후보가
 *         고가차도·교량이면 +alt_penalty, 그 외 0
 * @remark (2026-10-06 최정우 주석 추가)
*/
double RoadTypeDirectionPenalty(double dfDeltaAlt, uint8 nCandRoadType,
		const ALTITUDE_SCORE_CONFIG& stAltConfig)
{
	if (dfDeltaAlt > static_cast<double>(stAltConfig.nGap))
	{
		if (IsUndergroundRoad(nCandRoadType))
			return static_cast<double>(stAltConfig.nAltPenalty);
	}
	else if (dfDeltaAlt < -static_cast<double>(stAltConfig.nGap))
	{
		if (IsElevatedRoad(nCandRoadType))
			return static_cast<double>(stAltConfig.nAltPenalty);
	}
	return 0.0;
}

} // namespace

/**
 * @brief 연속 맵매칭 고도·ROAD_TYPE 보조 비용 (m 환산, 양수=불리·음수=유리)
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 (고도 컨텍스트)
 * @param[in] nCandRoadType 후보 링크 ROAD_TYPE
 * @param[in] stAltConfig config alt_* 설정
 * @return 고도 보조 비용 (dfCost 가산분)
 * @remark
 * ── 쉬운 설명 (ACCURACY_M 적응 반경 주석과 동일 형식) ──
 *   전제: bUseAltScore, 직전·현재 ALTITUDE_M 유효, TUNNELING 아님, alt_weight>0
 *   Δalt = nAltitudeM − nPrevAltitude  (직전 매칭 성공 시 GPS 고도 앵커)
 *
 *   |Δalt| ≤ alt_gap:
 *     · 후보 ROAD_TYPE = 직전  → −alt_penalty
 *     · 호환(고가↔교량)        → 0
 *       [정정] 2026-08-28 부터 일반↔교량도 호환(0)이다 — IsRoadTypeCompatible 참고 (2026-10-06 최정우 주석 수정)
 *     · 불일치                 → +alt_penalty
 *
 *   |Δalt| > alt_gap:
 *     · alt_weight × (|Δalt| − alt_gap) + 방향 패널티
 *     · Δalt > +차이 이고 후보=지하 → +alt_penalty
 *       [보완] 여기서 "지하" 는 ROAD_TYPE 2(지하차도)만이다 — 4(터널)는 해당 없음 (2026-10-06 최정우 주석 수정)
 *     · Δalt < −차이 이고 후보=고가/교량 → +alt_penalty
 *
 *   |Δalt|/dfHorizMove > alt_slope → 0 (GPS 고도 불신, 폴백)
 *
 *   [변수 매핑]
 *     dfAltAdj = CalcAltRoadPenalty(...)
 *     dfCost   = dfIntersectLenSgmt + dfAnglePenalty + dfAltAdj
 *
 *   예) 차이=8, alt_penalty=10, 가중치=0.5
 *       직전100m·현재106m(Δ=6), 후보=직전과 동일 고가 → −10
 *       직전100m·현재106m(Δ=6), 후보=일반(직전 고가)   → +10
 *       직전100m·현재120m(Δ=20), 후보=지하             → 0.5×(20−8)+10 = +16
 * ────────────────────────────────────────────────────────────────────────────
*/
double CGISUtil::CalcAltRoadPenalty(const SGMT_MATCH_INPUT& stSgmtMatchInput, uint8 nCandRoadType,
		const ALTITUDE_SCORE_CONFIG& stAltConfig) const
{
	if (!stSgmtMatchInput.bUseAltScore || stAltConfig.dfWeight <= 0.0)
		return 0.0;
	if (stSgmtMatchInput.nAltitudeM < 0 || stSgmtMatchInput.nPrevAltitude < 0)
		return 0.0;
	if (stSgmtMatchInput.nDriveStatus == DRIVE_STATUS_TUNNELING)
		return 0.0;

	double dfDeltaAlt = static_cast<double>(stSgmtMatchInput.nAltitudeM)
		- static_cast<double>(stSgmtMatchInput.nPrevAltitude);

	if (stSgmtMatchInput.dfHorizMove >= MM_CALC_MIN_DIST && stAltConfig.dfSlope > 0.0)
	{
		double dfSlope = fabs(dfDeltaAlt) / stSgmtMatchInput.dfHorizMove;
		if (dfSlope > stAltConfig.dfSlope)
			return 0.0;
	}

	if (fabs(dfDeltaAlt) <= static_cast<double>(stAltConfig.nGap))
	{
		if (nCandRoadType == stSgmtMatchInput.nPrevRoadType)
			return -static_cast<double>(stAltConfig.nAltPenalty);
		// 직전·후보 ROAD_TYPE 호환성(고가↔교량 등) 판정 (2026-07-08 최정우 주석 추가)
		if (IsRoadTypeCompatible(nCandRoadType, stSgmtMatchInput.nPrevRoadType))
			return 0.0;
		return static_cast<double>(stAltConfig.nAltPenalty);
	}

	// Δalt 방향·ROAD_TYPE 불일치 추가 패널티 산출 (2026-07-08 최정우 주석 추가)
	return stAltConfig.dfWeight * (fabs(dfDeltaAlt) - static_cast<double>(stAltConfig.nGap))
		+ RoadTypeDirectionPenalty(dfDeltaAlt, nCandRoadType, stAltConfig);
}

/**
 * @brief 각도 차이 계산 (-180 ~ 180)
 * @param[in] nAngle1 이전 진행 방향
 * @param[in] nAngle2 다음 진행 방향
 * @return 각도 차이 (회전각)
*/
sint16 CGISUtil::GetAngleDiff(sint16& nAngle1, sint16& nAngle2)
{
	sint16 nAngleDiff = (nAngle2 - nAngle1) % 360;

	if (nAngleDiff > 180)
		nAngleDiff -= 360;
	else if (nAngleDiff < -180)
		nAngleDiff += 360;

	return nAngleDiff;
}

/**
 * @brief 진행 각도 계산
 * @param[in] stSgmtPoint 세그먼트 X,Y 좌표
 * @param[in] stPoint X,Y 좌표
 * @param[out] pnDirAngle 진행 각도 (방위각)
 * @return true(성공), false(실패)
*/
bool CGISUtil::GetDirAngle(POINT& stSgmtPoint, POINT& stPoint, sint16 *pnDirAngle)
{
	if ((stPoint.dfX == stSgmtPoint.dfX) && (stPoint.dfY == stSgmtPoint.dfY))
		return false;

	*pnDirAngle = static_cast<sint16>(round(
		BearingDegScaled(stSgmtPoint.dfX, stSgmtPoint.dfY, stPoint.dfX, stPoint.dfY)));

	return true;
}

/**
 * @brief 두 좌표를 잇는 진행 방위각을 도(degree) 단위로 계산 — 세그먼트 기준인
 *   GetDirAngle() 과 달리 점 두 개만 받는다 (2026-09-17 최정우 정정 — 두 함수의
 *   @brief 가 같아 구분이 안 됐다)
 * @param[in] stPoint1 진입 좌표
 * @param[in] stPoint2 진출 좌표
 * @return 진행각(0~359도)
*/
sint16 CGISUtil::GetDirAngleDegree(POINT& stPoint1, POINT& stPoint2)
{
	return static_cast<sint16>(round(
		BearingDegScaled(stPoint1.dfX, stPoint1.dfY, stPoint2.dfX, stPoint2.dfY)));
}

/**
 * @brief 경위도간 거리 계산 (Degree * 360000)
 * @param[in] stPoint X,Y 좌표
 * @param[in] stIntersect 세그먼트 교차점 X,Y 좌표
 * @return 세그먼트 시작부터 교차점까지 거리
 *         [보완] 두 점 모두 도×360000 스케일 입력, 반환은 하버사인 거리(m, 반경 6378137m)다.
 *         이름과 달리 "세그먼트 시작↔교차점" 전용이 아니라 임의의 두 점 거리로 쓰인다 (2026-10-06 최정우 주석 수정)
*/
double CGISUtil::GetDistanceGEO1(POINT& stPoint, POINT& stIntersect)
{
	if ((stPoint.dfX == stIntersect.dfX) && (stPoint.dfY == stIntersect.dfY))
		return 0;

	double dfLon = (stIntersect.dfX - stPoint.dfX) / 360000.0;
	double dfLat = (stIntersect.dfY - stPoint.dfY) / 360000.0;

	double dfValue = pow(sin(RAD(dfLat) / 2.0), 2.0) + cos(RAD(stPoint.dfY / 360000.0)) * cos(RAD(stIntersect.dfY / 360000.0)) * pow(sin(RAD(dfLon) / 2.0), 2.0);
	// [버그 수정, 2026-09-11 최정우] 부동소수점 오차로 dfValue 가 1.0 을 살짝 넘으면 sqrt(1-dfValue)
	//   가 음수의 제곱근이 돼 NaN 이 나온다 — 같은 파일의 HaversineMetersDeg() 에 이미 있는 클램프와
	//   동일하게 적용. 도로망 스케일(수백m 이내)에서는 사실상 발현 안 하지만 방어적으로 추가.
	if (dfValue > 1.0) dfValue = 1.0;
	return 2.0 * atan(sqrt(dfValue) / sqrt(1 - dfValue)) * 6378137;
}

/**
 * @brief 경위도간 거리 계산 (360000 곱하기 전)
 * @param[in] stPoint X,Y 좌표
 * @param[in] stIntersect 세그먼트 교차점 X,Y 좌표
 * @return 세그먼트 시작부터 교차점까지 거리
 *         [보완] 입력은 순수 WGS84 도, 반환은 미터. 호출부는 GetSgmtLength()(호출부 0건)뿐이다
 *         (2026-10-06 최정우 주석 수정)
*/
double CGISUtil::GetDistanceGEO2(POINT& stPoint, POINT& stIntersect)
{
	if ((stPoint.dfX == stIntersect.dfX) && (stPoint.dfY == stIntersect.dfY))
		return 0;

	double dfLon = stIntersect.dfX - stPoint.dfX;
	double dfLat = stIntersect.dfY - stPoint.dfY;

	// [버그 수정, 2026-09-11 최정우] CreateData/src/GISUtil.cpp가 2026-08-24에 이미 고친 것과
	//   동일 버그 — GetDistanceGEO1()(입력 단위: 도×360000)을 그대로 복사해오면서 cos(위도) 항의
	//   "/360000.0"을 못 지워, 이 함수(입력 단위: 순수 도)에서는 cos(위도/360000)≈cos(0)≈1 로
	//   사실상 무력화돼 있었다 — 위도 37°대 기준 cos(37°)=0.794 대신 1을 써서 거리가 약 1.2~1.26배
	//   부풀려짐. 현재 이 함수 호출부가 없어(dead code) 실피해는 없었지만, CreateData 쪽과 똑같이
	//   맞춰 "고쳐진 코드"로 착각하고 재사용할 때 즉시 재발하는 걸 막는다.
	double dfValue = pow(sin(RAD(dfLat) / 2.0), 2.0) + cos(RAD(stPoint.dfY)) * cos(RAD(stIntersect.dfY)) * pow(sin(RAD(dfLon) / 2.0), 2.0);
	// GetDistanceGEO1() 과 동일 근거 — dfValue 상한 클램프 (2026-09-11 최정우 추가)
	if (dfValue > 1.0) dfValue = 1.0;
	return 2.0 * atan(sqrt(dfValue) / sqrt(1 - dfValue)) * 6378137;
}

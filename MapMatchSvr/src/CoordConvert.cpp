/**
 * @file CoordConvert.cpp
 * @brief 측지계 변환 클래스 소스 파일
*/
#include "CoordConvert.h"

/**
 * @brief 위도, 경도 -> DB Coord 변환
 * @param[in] inLat 위도
 * @param[in] inLon 경도
 * @param[out] outLat WGS84 위도
 * @param[out] outLon WGS84 경도
 *        [정정] 출력은 도 단위 위경도가 아니라 **검색 좌표(도×360000, 반올림한 uint32 정수)** 다 —
 *        link.psf 세그먼트 좌표와 같은 스케일. 입력은 WGS84 도 (2026-10-06 최정우 주석 수정)
 * @return void
*/
void CCoordConvert::WGS84ToSearchCoord(double inLat, double inLon, uint32 *outLat, uint32 *outLon)
{
	// [버그 수정, 2026-09-11 최정우] 반올림 없이 바로 (uint32) 캐스팅하면 항상 0 방향으로 잘려
	// 최대 1/360000도(약 0.3m) 만큼 매번 남서쪽으로 치우치는 체계적 편향이 생긴다. +0.5 로 반올림.
	*outLat = (uint32)(inLat * 360000.0 + 0.5);	// 위도 (Y)
	*outLon = (uint32)(inLon * 360000.0 + 0.5);	// 경도 (X)
}

/**
 * @brief BESSELGEO -> BESSELTM 변환
 * @param[in] inLat BESSELGEO 위도
 * @param[in] inLon BESSELGEO 경도
 * @param[out] outLat BESSELTM 위도
 * @param[out] outLon BESSELTM 경도
 * @return void
*/
void CCoordConvert::BESSELGEOToBESSELTM(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];
	
	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	double dfA = 6377397.15500;
	double dfEE = 0.006674372;							// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.00672;							// pow(dfE, 2) / (1 - pow(dfE, 2));
	
	
	// 측지계 변수 입력
	// BTM  
	double dfK = 1.0;
	double dfFE = 200000.0;
	double dfFN = 500000.0;
	double dfLON_O = 2.216618595284169;					// 127.0028903 * 3.1415926535897932384626433832795028842 / 180;
	
	// JTM
	double dfM0 = 4207077.7078504800; 

	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	
	// V
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// T
	double dfCS_T = pow(tan(dfCS_LAT_RAD), 2);
	
	// C
	double dfCS_C = dfEE_D * pow(cos(dfCS_LAT_RAD), 2);
	
	// A
	double dfCS_A = (dfCS_LON_RAD - dfLON_O) * cos(dfCS_LAT_RAD);

	// M
	double dfCS_M = dfA * ((1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256) * dfCS_LAT_RAD
		- (3 * dfEE / 8 + 3 * pow(dfEE, 2) / 32 + 45 * pow(dfEE, 3) / 1024) * sin(2 * dfCS_LAT_RAD)
		+ (15 * pow(dfEE, 2) / 256 + 45 * pow(dfEE,3) / 1024) * sin(4 * dfCS_LAT_RAD)
		- (35 * pow(dfEE, 3) / 3072) * sin(6 * dfCS_LAT_RAD));
	
	// 결과 반영
	sprintf(szLon, "%.06lf", dfFE + dfK * dfCS_V * (dfCS_A + (1 - dfCS_T + dfCS_C) * pow(dfCS_A, 3) / 6
		+ (5 - 18 * dfCS_T + pow(dfCS_T, 2) + 72 * dfCS_C - 58 * dfEE_D) * pow(dfCS_A, 5) / 120));
	sprintf(szLat, "%.06lf", dfFN + dfK * (dfCS_M - dfM0 + dfCS_V * tan(dfCS_LAT_RAD) * (pow(dfCS_A, 2) / 2
		+ (5 - dfCS_T + 9 * dfCS_C + 4 * pow(dfCS_C, 2)) * pow(dfCS_A, 4) / 24
		+ (61 - 58 * dfCS_T + pow(dfCS_T, 2) + 600 * dfCS_C - 330 * dfEE_D) * pow(dfCS_A, 6) / 720)));

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief BESSELGEO -> GRS80GEO 변환
 * @param[in] inLat BESSELGEO 위도
 * @param[in] inLon BESSELGEO 경도
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::BESSELGEOToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];

	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	double dfA = 6377397.155;
	double dfF = 0.003342773182175;						// 1 / 299.1528128;
	double dfEE = 0.006674372231802;					// 2 * dfF - pow(dfF, 2);
	
	double dfDX = -145.907;
	double dfDY = 505.034;
	double dfDZ = 685.756;
	double dfDA = 739.845;
	double dfDF = 0.000010037499008;
	
	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	
	// 변수입력(p)
	double dfCS_P = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 1.5);
	
	// 변수입력(v)
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// DELTA_위도(SEECOND)
	double dfCS_LAT_GRS_DS = (-1.0 * dfDX * sin(dfCS_LAT_RAD) * cos(dfCS_LON_RAD)
		- (dfDY) * sin(dfCS_LAT_RAD) * sin(dfCS_LON_RAD) + dfDZ * cos(dfCS_LAT_RAD)
		+ (dfA * dfDF + dfF * dfDA) * sin(2.0 * dfCS_LAT_RAD)) 
		/ (dfCS_P * sin(1.0 / 3600 / 180 * M_PI));
	
	// DELTA_경도(SECOND)
	double dfCS_LON_GRS_DS = (-1.0 * dfDX * sin(dfCS_LON_RAD) + dfDY * cos(dfCS_LON_RAD)) 
		/ (dfCS_V * cos(dfCS_LAT_RAD) * sin(1.0 / 3600 / 180 * M_PI));
	
	// 결과 반영
	sprintf(szLon, "%.06lf", inLon + dfCS_LON_GRS_DS / 3600.0);
	sprintf(szLat, "%.06lf", inLat + dfCS_LAT_GRS_DS / 3600.0);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief BESSELGEO -> KATECH 변환
 * @param[in] inLat BESSELGEO 위도
 * @param[in] inLon BESSELGEO 경도
 * @param[out] outLat KATECH 위도
 * @param[out] outLon KATECH 경도
 * @return void
*/
void CCoordConvert::BESSELGEOToKATECH(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];

	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	double dfA = 6377397.15500;
	double dfEE = 0.006674372;							// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.00672;							// pow(dfE, 2) / (1 - pow(dfE, 2));
	
	
	// 측지계 변수 입력
	// KACTEX 
	double dfK = 0.9999;
	double dfFE = 400000.0;
	double dfFN = 600000.0;
	double dfLON_O = 2.234021442552742;					// 128 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207077.7078504800;
	
	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;

	// v
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// T
	double dfCS_T = pow(tan(dfCS_LAT_RAD), 2);
	
	// C
	double dfCS_C = dfEE_D * pow(cos(dfCS_LAT_RAD), 2);
	
	// A
	double dfCS_A = (dfCS_LON_RAD - dfLON_O) * cos(dfCS_LAT_RAD);
	
	// M
	double dfCS_M = dfA * ((1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256) * dfCS_LAT_RAD
		- (3 * dfEE / 8 + 3 * pow(dfEE, 2) / 32 + 45 * pow(dfEE, 3) / 1024) * sin(2 * dfCS_LAT_RAD)
		+ (15 * pow(dfEE, 2) / 256 + 45 * pow(dfEE, 3) / 1024) * sin(4 * dfCS_LAT_RAD)
		- (35 * pow(dfEE, 3) / 3072) * sin(6 * dfCS_LAT_RAD));

	// 결과 반영
	sprintf(szLon, "%.06lf", dfFE + dfK * dfCS_V * (dfCS_A + (1 - dfCS_T + dfCS_C) * pow(dfCS_A, 3) / 6
		+ (5 - 18 * dfCS_T + pow(dfCS_T, 2) + 72 * dfCS_C - 58 * dfEE_D) * pow(dfCS_A, 5) / 120));
	sprintf(szLat, "%.06lf", dfFN + dfK * (dfCS_M - dfM0 + dfCS_V * tan(dfCS_LAT_RAD) * (pow(dfCS_A, 2) / 2
		+ (5 - dfCS_T + 9 * dfCS_C + 4 * pow(dfCS_C, 2)) * pow(dfCS_A, 4) / 24
		+ (61 - 58 * dfCS_T + pow(dfCS_T, 2) + 600 * dfCS_C - 330 * dfEE_D) * pow(dfCS_A, 6) / 720)));

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief BESSELGEO -> WGS84GEO 변환
 * @param[in] inLat BESSELGEO 위도
 * @param[in] inLon BESSELGEO 경도
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
*/
void CCoordConvert::BESSELGEOToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];

	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));
	
	double dfA = 6377397.155;
	double dfF = 0.0033427731821748;					// 1 / 299.1528128 
	double dfEE = 0.006674372;							// 2 * dfF - pow(dfF, 2);
	    
	double dfDX = -128;
	double dfDY = 481;
	double dfDZ = 664;
	double dfDA = 739.845;
	double dfDF = 0.000010037499008;    
	
	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	
	// 변수입력(p)
	double dfCS_P = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 1.5);
	
	// 변수입력(v)
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// DELTA_위도(SEECOND)
	double dfCS_LAT_GRS_DS = (-1.0 * dfDX * sin(dfCS_LAT_RAD) * cos(dfCS_LON_RAD)
		- (dfDY) * sin(dfCS_LAT_RAD) * sin(dfCS_LON_RAD) + dfDZ * cos(dfCS_LAT_RAD)
		+ (dfA * dfDF + dfF * dfDA) * sin(2.0 * dfCS_LAT_RAD))
		/ (dfCS_P * sin(1.0 / 3600 / 180 * M_PI));
	
	// DELTA_경도(SECOND)
	double dfCS_LON_GRS_DS = (-1.0 * dfDX * sin(dfCS_LON_RAD) + dfDY * cos(dfCS_LON_RAD))
		/ (dfCS_V * cos(dfCS_LAT_RAD) * sin(1.0 / 3600 / 180 * M_PI));
	
	// 결과 반영
	sprintf(szLon, "%.06lf", inLon + dfCS_LON_GRS_DS / 3600.0);
	sprintf(szLat, "%.06lf", inLat + dfCS_LAT_GRS_DS / 3600.0);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief BESSELGEO -> GRS80TM 변환
 * @param[in] inLat BESSELGEO 위도
 * @param[in] inLon BESSELGEO 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 * @return void
*/
void CCoordConvert::BESSELGEOToGRS80TM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	BESSELGEOToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80TM(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief BESSELGEO -> EPSG3857 변환
 * @param[in] inLat BESSELGEO 위도
 * @param[in] inLon BESSELGEO 경도
 * @param[out] outLat EPSG3857 위도
 * @param[out] outLon EPSG3857 경도
 * @return void
*/
void CCoordConvert::BESSELGEOToEPSG3857(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	BESSELGEOToWGS84GEO(inLat, inLon, &tmpLat, &tmpLon);
	WGS84GEOToEPSG3857(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief BESSELGEO -> GRS80UTMK 변환
 * @param[in] inLat BESSELGEO 위도
 * @param[in] inLon BESSELGEO 경도
 * @param[out] outLat GRS80UTMK 위도
 * @param[out] outLon GRS80UTMK 경도
 * @return void
*/
void CCoordConvert::BESSELGEOToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	BESSELGEOToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80UTMK(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80GEO -> BESSELTM 변환
 * @param[in] inLat GRS80GEO 위도
 * @param[in] inLon GRS80GEO 경도
 * @param[out] outLat BESSELTM 위도
 * @param[out] outLon BESSELTM 경도
 * @return void
*/
void CCoordConvert::GRS80GEOToBESSELTM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	GRS80GEOToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToBESSELTM(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80GEO -> BESSELGEO 변환
 * @param[in] inLat GRS80GEO 위도
 * @param[in] inLon GRS80GEO 경도
 * @param[out] outLat BESSELGEO 위도
 * @param[out] outLon BESSELGEO 경도
 * @return void
*/
void CCoordConvert::GRS80GEOToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];

	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	double dfA = 6378137.0;
	double dfF = 0.0033528106811823;					// 1 / 298.257222101
	double dfEE = 0.006694380;							// 2 * dfF - pow(dfF, 2);
	
	double dfDX = 145.907;
	double dfDY = -505.034;
	double dfDZ = -685.756;
	double dfDA = -739.845;
	double dfDF = -0.000010037499008;    
	
	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	
	// 변수입력(p)
	double dfCS_P = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 1.5);
	
	// 변수입력(v)
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// DELTA_위도(SEECOND)
	double dfCS_LAT_GRS_DS = (-1.0 * dfDX * sin(dfCS_LAT_RAD) * cos(dfCS_LON_RAD)
		- (dfDY) * sin(dfCS_LAT_RAD) * sin(dfCS_LON_RAD) + dfDZ * cos(dfCS_LAT_RAD)
		+ (dfA * dfDF + dfF * dfDA) * sin(2.0 * dfCS_LAT_RAD)) 
		/ (dfCS_P * sin(1.0 / 3600 / 180 * M_PI));
	
	// DELTA_경도(SECOND)
	double dfCS_LON_GRS_DS = (-1.0 * dfDX * sin(dfCS_LON_RAD) + dfDY * cos(dfCS_LON_RAD))
		/ (dfCS_V * cos(dfCS_LAT_RAD) * sin(1.0 / 3600 / 180 * M_PI));
	
	// 결과 반영
	sprintf(szLon, "%.06lf", inLon + dfCS_LON_GRS_DS / 3600.0);
	sprintf(szLat, "%.06lf", inLat + dfCS_LAT_GRS_DS / 3600.0);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief GRS80GEO -> KATECH 변환
 * @param[in] inLat GRS80GEO 위도
 * @param[in] inLon GRS80GEO 경도
 * @param[out] outLat KATECH 위도
 * @param[out] outLon KATECH 경도
 * @return void
*/
void CCoordConvert::GRS80GEOToKATECH(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	GRS80GEOToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToKATECH(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80GEO -> WGS84GEO 변환
 * @param[in] inLat GRS80GEO 위도
 * @param[in] inLon GRS80GEO 경도
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
 * @warning GRS80 과 WGS84 는 실용상 같은 측지계(차이 1m 미만)인데, 이 함수는 BESSEL 을 경유한다.
 *   GRS80→BESSEL(GRS80GEOToBESSELGEO, ΔX/Y/Z=145.907/−505.034/−685.756)과 BESSEL→WGS84
 *   (BESSELGEOToWGS84GEO, ΔX/Y/Z=−128/481/664)의 3변수 값이 서로 달라 상쇄되지 않고, 차이분
 *   (약 17.9/−24.0/−21.8m, 3차원 합 약 37m)만큼 좌표가 이동한다. GRS80TM·GRS80TM2010·GRS80UTMK →
 *   WGS84 변환도 모두 이 함수를 거친다. 현재 MapMatchSvr 매칭 경로(CCoordinate)는 이 계열을
 *   쓰지 않고, CreateData 의 동일 복사본도 config coordtype=1(WGS84) 이라 실피해는 없다 —
 *   GRS80 계열 입력을 새로 받게 되면 먼저 검증할 것 (2026-10-06 최정우 주석 추가)
*/
void CCoordConvert::GRS80GEOToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	GRS80GEOToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToWGS84GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80GEO -> GRS80TM 변환
 * @param[in] inLat GRS80GEO 위도
 * @param[in] inLon GRS80GEO 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 * @return void
*/
void CCoordConvert::GRS80GEOToGRS80TM(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];
	
	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));
	
	double dfA = 6378137.0;
	double dfEE = 0.006694380022901;					// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.006739496775479;					// pow(dfE, 2) / (1 - pow(dfE, 2));

	// 측지계 변수 입력
	double dfK = 1;
	double dfFE = 200000.0;
	double dfFN = 500000.0;
	double dfLON_O = 2.21656815003279856;				// 127.0 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207498.0191503200;
	
	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	
	// v
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// T
	double dfCS_T = pow(tan(dfCS_LAT_RAD), 2);
	
	// C
	double dfCS_C = dfEE_D * pow(cos(dfCS_LAT_RAD), 2);
	
	// A
	double dfCS_A = (dfCS_LON_RAD - dfLON_O) * cos(dfCS_LAT_RAD);
	
	// M
	double dfCS_M = dfA * ((1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256) * dfCS_LAT_RAD
		- (3 * dfEE /8 + 3 * pow(dfEE,2) / 32 + 45 * pow(dfEE, 3) / 1024) * sin(2 * dfCS_LAT_RAD)
		+ (15 * pow(dfEE, 2) / 256 + 45 * pow(dfEE, 3) / 1024) * sin(4 * dfCS_LAT_RAD)
		- (35 * pow(dfEE, 3) / 3072) * sin(6 * dfCS_LAT_RAD));

	// 결과 반영
	sprintf(szLon, "%.06lf", dfFE + dfK * dfCS_V * (dfCS_A + (1 - dfCS_T + dfCS_C) * pow(dfCS_A, 3) / 6
		+ (5 - 18 * dfCS_T + pow(dfCS_T, 2) + 72 * dfCS_C - 58 * dfEE_D) * pow(dfCS_A, 5) / 120));
	sprintf(szLat, "%.06lf", dfFN + dfK * (dfCS_M - dfM0 + dfCS_V * tan(dfCS_LAT_RAD) * (pow(dfCS_A, 2) / 2
		+ (5 - dfCS_T + 9 * dfCS_C + 4 * pow(dfCS_C, 2)) * pow(dfCS_A, 4) / 24
		+ (61 - 58 * dfCS_T + pow(dfCS_T, 2) + 600 * dfCS_C - 330 * dfEE_D) * pow(dfCS_A, 6) / 720)));

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief GRS80GEO -> EPSG3857 변환
 * @param[in] inLat GRS80GEO 위도
 * @param[in] inLon GRS80GEO 경도
 * @param[out] outLat EPSG3857 위도
 * @param[out] outLon EPSG3857 경도
 * @return void
*/
void CCoordConvert::GRS80GEOToEPSG3857(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;

	GRS80GEOToBESSELGEO(inLat, inLon, &tmpLat1, &tmpLon1);
	BESSELGEOToWGS84GEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	WGS84GEOToEPSG3857(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief GRS80GEO -> GRS80UTMK 변환
 * @param[in] inLat GRS80GEO 위도
 * @param[in] inLon GRS80GEO 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 *        [정정] 출력은 GRS80TM 이 아니라 GRS80UTMK 의 Northing(outLat)·Easting(outLon), 미터 (2026-10-06 최정우 주석 수정)
 * @return void
*/
void CCoordConvert::GRS80GEOToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];
	
	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));
	
	double dfA = 6378137.0;
	double dfEE = 0.006694380022901;					// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.006739496775479;					// pow(dfE, 2) / (1 - pow(dfE, 2));

	// 측지계 변수 입력
	double dfK = 0.9996;
	double dfFE = 1000000.0;
	double dfFN = 2000000.0;
	double dfLON_O = 2.22529479629277;					// 127.5 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207498.0191503200;
	
	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	
	// v
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// T
	double dfCS_T = pow(tan(dfCS_LAT_RAD), 2);
	
	// C
	double dfCS_C = dfEE_D * pow(cos(dfCS_LAT_RAD), 2);
	
	// A
	double dfCS_A = (dfCS_LON_RAD - dfLON_O) * cos(dfCS_LAT_RAD);
	
	// M
	double dfCS_M = dfA * ((1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256) * dfCS_LAT_RAD
		- (3 * dfEE /8 + 3 * pow(dfEE,2) / 32 + 45 * pow(dfEE, 3) / 1024) * sin(2 * dfCS_LAT_RAD)
		+ (15 * pow(dfEE, 2) / 256 + 45 * pow(dfEE, 3) / 1024) * sin(4 * dfCS_LAT_RAD)
		- (35 * pow(dfEE, 3) / 3072) * sin(6 * dfCS_LAT_RAD));

	// 결과 반영
	sprintf(szLon, "%.06lf", dfFE + dfK * dfCS_V * (dfCS_A + (1 - dfCS_T + dfCS_C) * pow(dfCS_A, 3) / 6
		+ (5 - 18 * dfCS_T + pow(dfCS_T, 2) + 72 * dfCS_C - 58 * dfEE_D) * pow(dfCS_A, 5) / 120));
	sprintf(szLat, "%.06lf", dfFN + dfK * (dfCS_M - dfM0 + dfCS_V * tan(dfCS_LAT_RAD) * (pow(dfCS_A, 2) / 2
		+ (5 - dfCS_T + 9 * dfCS_C + 4 * pow(dfCS_C, 2)) * pow(dfCS_A, 4) / 24
		+ (61 - 58 * dfCS_T + pow(dfCS_T, 2) + 600 * dfCS_C - 330 * dfEE_D) * pow(dfCS_A, 6) / 720)));

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief WGS84GEO -> BESSELTM 변환
 * @param[in] inLat WGS84GEO 위도
 * @param[in] inLon WGS84GEO 경도
 * @param[out] outLat BESSELTM 위도
 * @param[out] outLon BESSELTM 경도
 * @return void
*/
void CCoordConvert::WGS84GEOToBESSELTM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	WGS84GEOToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToBESSELTM(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief WGS84GEO -> GRS80GEO 변환
 * @param[in] inLat WGS84GEO 위도
 * @param[in] inLon WGS84GEO 경도
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::WGS84GEOToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	WGS84GEOToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToGRS80GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief WGS84GEO -> KATECH 변환
 * @param[in] inLat WGS84GEO 위도
 * @param[in] inLon WGS84GEO 경도
 * @param[out] outLat KATECH 위도
 * @param[out] outLon KATECH 경도
 * @return void
*/
void CCoordConvert::WGS84GEOToKATECH(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	WGS84GEOToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToKATECH(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief WGS84GEO -> BESSELGEO 변환
 * @param[in] inLat WGS84GEO 위도
 * @param[in] inLon WGS84GEO 경도
 * @param[out] outLat BESSELGEO 위도
 * @param[out] outLon BESSELGEO 경도
 * @return void
*/
void CCoordConvert::WGS84GEOToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];
	
	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	double dfA = 6378137.0;
	double dfF = 0.0033528106647362;					// 1 / 298.257223564
	double dfEE = 0.006694380;							// 2 * dfF - pow(dfF, 2);

	double dfDX = 128;
	double dfDY = -481;
	double dfDZ = -664;
	double dfDA = -739.845;
	double dfDF = -0.000010037499008;    
	
	// radian화
	double dfCS_LAT_RAD = inLat * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	double dfCS_LON_RAD = inLon * 0.0174532925199433;	// 180 * 3.1415926535897932384626433832795028842;
	
	// 변수입력(p)
	double dfCS_P = dfA * (1 - dfEE) / pow(1- dfEE * pow(sin(dfCS_LAT_RAD), 2), 1.5);
	
	// 변수입력(v)
	double dfCS_V = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT_RAD), 2), 0.5);
	
	// DELTA_위도(SEECOND)
	double dfCS_LAT_GRS_DS = (-1.0 * dfDX * sin(dfCS_LAT_RAD) * cos(dfCS_LON_RAD)
		- (dfDY) * sin(dfCS_LAT_RAD) * sin(dfCS_LON_RAD) + dfDZ * cos(dfCS_LAT_RAD)
		+ (dfA * dfDF + dfF * dfDA) * sin(2.0 * dfCS_LAT_RAD)) / (dfCS_P * sin(1.0 / 3600 / 180 * M_PI));
	
	// DELTA_경도(SECOND)
	double dfCS_LON_GRS_DS = (-1.0 * dfDX * sin(dfCS_LON_RAD) + (dfDY) * cos(dfCS_LON_RAD))
		/ (dfCS_V * cos(dfCS_LAT_RAD) * sin(1.0 / 3600 / 180 * M_PI));
	
	// 결과 반영
	sprintf(szLon, "%.06lf", inLon + dfCS_LON_GRS_DS / 3600.0);
	sprintf(szLat, "%.06lf", inLat + dfCS_LAT_GRS_DS / 3600.0);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief WGS84GEO -> GRS80TM 변환
 * @param[in] inLat WGS84GEO 위도
 * @param[in] inLon WGS84GEO 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 * @return void
*/
void CCoordConvert::WGS84GEOToGRS80TM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;
	
	WGS84GEOToBESSELGEO(inLat, inLon, &tmpLat1, &tmpLon1);
	BESSELGEOToGRS80GEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	GRS80GEOToGRS80TM(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief WGS84GEO -> EPSG3857 변환
 * @param[in] inLat WGS84GEO 위도
 * @param[in] inLon WGS84GEO 경도
 * @param[out] outLat EPSG3857 위도
 * @param[out] outLon EPSG3857 경도
 * @return void
*/
void CCoordConvert::WGS84GEOToEPSG3857(double inLat, double inLon, double *outLat, double *outLon)
{
	*outLon = R_MAJOR * inLon * 0.0174532925199433;
	*outLat = R_MAJOR * log(tan(M_PI / 4 + inLat * (M_PI / 180) / 2));
}

/**
 * @brief WGS84GEO -> GRS80UTMK 변환
 * @param[in] inLat WGS84GEO 위도
 * @param[in] inLon WGS84GEO 경도
 * @param[out] outLat GRS80UTMK 위도
 * @param[out] outLon GRS80UTMK 경도
 * @return void
*/
void CCoordConvert::WGS84GEOToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	WGS84GEOToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80UTMK(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief KATECH -> BESSELTM 변환
 * @param[in] inLat KATECH 위도
 * @param[in] inLon KATECH 경도
 * @param[out] outLat BESSELTM 위도
 * @param[out] outLon BESSELTM 경도
 * @return void
*/
void CCoordConvert::KATECHToBESSELTM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	KATECHToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToBESSELTM(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief KATECH -> GRS80GEO 변환
 * @param[in] inLat KATECH 위도
 * @param[in] inLon KATECH 경도
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::KATECHToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	KATECHToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToGRS80GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief KATECH -> BESSELGEO 변환
 * @param[in] inLat KATECH 위도
 * @param[in] inLon KATECH 경도
 * @param[out] outLat BESSELGEO 위도
 * @param[out] outLon BESSELGEO 경도
 * @return void
*/
void CCoordConvert::KATECHToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];
	
	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	double dfA = 6377397.155;
	double dfEE = 0.006674372231802;					// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.006719218799175;					// pow(dfE, 2) / (1 - pow(dfE, 2));
	double dfK = 0.9999;
	double dfE1 = 0.001674184801115;					// (1 - sqrt(1 - dfEE)) / (1 + sqrt(1 - dfEE));
	
	// 측지계 변수 입력
	double dfFE = 400000.0;
	double dfFN = 600000.0;
	double dfLON_O = 2.2340214425527400;				// 128.0 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207077.70785048;

	// CS_M1
	double dfCS_M1 = dfM0 + (inLat - dfFN ) / dfK;
	
	// CS_U1
	double dfCS_U1 = dfCS_M1 / dfA / (1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256);
	
	// CS_LAT1
	double dfCS_LAT1 = dfCS_U1 + (3 * dfE1 / 2 - 27 * pow(dfE1, 3) / 32) * sin(2 * dfCS_U1)
		+ (21 * pow(dfE1, 2) / 16 - 55 * pow(dfE1, 4) / 32) * sin(4 * dfCS_U1)
		+ (151 * pow(dfE1, 3) / 96) * sin(6 * dfCS_U1)
		+ 1097 * pow(dfE1, 4) / 512 * sin(8 * dfCS_U1);

	// CS_V1
	double dfCS_V1 = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 0.5);
	
	// CS_P1
	double dfCS_P1 = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 1.5);
	
	// CS_T1
	double dfCS_T1 = pow(tan(dfCS_LAT1), 2);
	
	// CS_C1
	double dfCS_C1 = dfEE_D * pow(cos(dfCS_LAT1), 2);
	
	// CS_D
	double dfCS_D = (inLon - dfFE) / dfCS_V1 / dfK;
	
	// CS_LAT
	double dfCS_LAT = dfCS_LAT1 - dfCS_V1 * tan(dfCS_LAT1) / dfCS_P1 * pow(dfCS_D, 2) / 2
		- (5 + 3 * dfCS_T1 + 10 * dfCS_C1 - 4 * pow(dfCS_C1, 2) - 9 * dfEE_D) * pow(dfCS_D, 4) / 24
		+ (61 + 90 * dfCS_T1 + 298 * dfCS_C1 + 45 * pow(dfCS_T1, 2) - 252 * dfEE_D - 3 * pow(dfCS_C1, 2)) * pow(dfCS_D, 6) / 720;
	
	// CS_LON
	double dfCS_LON = dfLON_O + (dfCS_D - (1 + 2 * dfCS_T1 + dfCS_C1) * pow(dfCS_D, 3) / 6
		+ (5 - 2 * dfCS_C1 + 28 * dfCS_T1 - 3 * pow(dfCS_C1, 2) + 8 * dfEE_D + 24 * pow(dfCS_T1, 2)) * pow(dfCS_D, 5) / 120) / cos(dfCS_LAT1);

	// 결과 반영
	sprintf(szLon, "%.06lf", dfCS_LON * 180 / M_PI);
	sprintf(szLat, "%.06lf", dfCS_LAT * 180 / M_PI);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief KATECH -> WGS84GEO 변환
 * @param[in] inLat KATECH 위도
 * @param[in] inLon KATECH 경도
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
*/
void CCoordConvert::KATECHToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	KATECHToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToWGS84GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief KATECH -> GRS80TM 변환
 * @param[in] inLat KATECH 위도
 * @param[in] inLon KATECH 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 * @return void
*/
void CCoordConvert::KATECHToGRS80TM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;

	KATECHToBESSELGEO(inLat, inLon, &tmpLat1, &tmpLon1);
	BESSELGEOToGRS80GEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	GRS80GEOToGRS80TM(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief KATECH -> EPSG3857 변환
 * @param[in] inLat KATECH 위도
 * @param[in] inLon KATECH 경도
 * @param[out] outLat EPSG3857 위도
 * @param[out] outLon EPSG3857 경도
 * @return void
*/
void CCoordConvert::KATECHToEPSG3857(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;

	KATECHToBESSELGEO(inLat, inLon, &tmpLat1, &tmpLon1);
	BESSELGEOToWGS84GEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	WGS84GEOToEPSG3857(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief KATECH -> GRS80UTMK 변환
 * @param[in] inLat KATECH 위도
 * @param[in] inLon KATECH 경도
 * @param[out] outLat GRS80UTMK 위도
 * @param[out] outLon GRS80UTMK 경도
 * @return void
*/
void CCoordConvert::KATECHToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	KATECHToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80UTMK(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief BESSELTM -> BESSELGEO 변환
 * @param[in] inLat BESSELTM 위도
 * @param[in] inLon BESSELTM 경도
 * @param[out] outLat BESSELGEO 위도
 * @param[out] outLon BESSELGEO 경도
 * @return void
*/
void CCoordConvert::BESSELTMToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];

	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));
	
	// 타원체 변수 입력 - BESSEL 
	double dfA = 6377397.155;
	double dfEE = 0.006674372231802;					// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.006719218799175;					// pow(dfE, 2) / (1 - pow(dfE, 2));
	double dfK = 1;
	double dfE1 = 0.001674184801115;					// (1 - sqrt(1 - dfEE)) / (1 + sqrt(1 - dfEE));
	
	// 측지계 변수 입력
	double dfFE = 200000.0;
	double dfFN = 500000.0;
	double dfLON_O = 2.2166185952841689548008491014145;	// 127.0028903 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207077.70785048;
	
	// CS_M1
	double dfCS_M1 = dfM0 + (inLat - dfFN) / dfK;
	
	// CS_U1
	double dfCS_U1 = dfCS_M1 / dfA / (1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256);
	
	// CS_LAT1
	double dfCS_LAT1 = dfCS_U1 + (3 * dfE1 / 2 - 27 * pow(dfE1, 3) / 32) * sin(2 * dfCS_U1)
		+ (21 * pow(dfE1, 2) / 16 - 55 * pow(dfE1, 4) / 32) * sin(4 * dfCS_U1)
		+ (151 * pow(dfE1, 3) / 96) * sin(6 * dfCS_U1)
		+ 1097 * pow(dfE1, 4) / 512 * sin(8 * dfCS_U1);
	
	// CS_V1
	double dfCS_V1 = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 0.5);
	
	// CS_P1
	double dfCS_P1 = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 1.5);
	
	// CS_T1
	double dfCS_T1 = pow(tan(dfCS_LAT1), 2);
	
	// CS_C1
	double dfCS_C1 = dfEE_D * pow(cos(dfCS_LAT1), 2);
	
	// CS_D
	double dfCS_D = (inLon - dfFE) / dfCS_V1 / dfK;
	
	// CS_LAT
	double dfCS_LAT = dfCS_LAT1 - dfCS_V1 * tan(dfCS_LAT1) / dfCS_P1 * pow(dfCS_D, 2) / 2
		- (5 + 3 * dfCS_T1 + 10 * dfCS_C1 - 4 * pow(dfCS_C1, 2) - 9 * dfEE_D) * pow(dfCS_D, 4) / 24
		+ (61 + 90 * dfCS_T1 + 298 * dfCS_C1 + 45 * pow(dfCS_T1, 2) - 252 * dfEE_D
		- 3 * pow(dfCS_C1, 2)) * pow(dfCS_D, 6) / 720;
	
	// CS_LON
	double dfCS_LON = dfLON_O + (dfCS_D - (1 + 2 * dfCS_T1 + dfCS_C1) * pow(dfCS_D, 3) / 6
		+ (5 - 2 * dfCS_C1 + 28 * dfCS_T1 - 3 * pow(dfCS_C1, 2) + 8 * dfEE_D
		+ 24 * pow(dfCS_T1, 2)) * pow(dfCS_D, 5) / 120) / cos(dfCS_LAT1);
	
	// 결과 반영
	sprintf(szLon, "%.06lf", dfCS_LON * 180 / M_PI);
	sprintf(szLat, "%.06lf", dfCS_LAT * 180 / M_PI);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief BESSELTM -> GRS80GEO 변환
 * @param[in] inLat BESSELTM 위도
 * @param[in] inLon BESSELTM 경도
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::BESSELTMToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	BESSELTMToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToGRS80GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief BESSELTM -> KATECH 변환
 * @param[in] inLat BESSELTM 위도
 * @param[in] inLon BESSELTM 경도
 * @param[out] outLat KATECH 위도
 * @param[out] outLon KATECH 경도
 * @return void
*/
void CCoordConvert::BESSELTMToKATECH(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	BESSELTMToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToKATECH(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief BESSELTM -> WGS84GEO 변환
 * @param[in] inLat BESSELTM 위도
 * @param[in] inLon BESSELTM 경도
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
*/
void CCoordConvert::BESSELTMToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	BESSELTMToBESSELGEO(inLat, inLon, &tmpLat, &tmpLon);
	BESSELGEOToWGS84GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief BESSELTM -> GRS80TM 변환
 * @param[in] inLat BESSELTM 위도
 * @param[in] inLon BESSELTM 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 * @return void
*/
void CCoordConvert::BESSELTMToGRS80TM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;
	
	BESSELTMToBESSELGEO(inLat, inLon, &tmpLat1, &tmpLon1);
	BESSELGEOToGRS80GEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	GRS80GEOToGRS80TM(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief BESSELTM -> EPSG3857 변환
 * @param[in] inLat BESSELTM 위도
 * @param[in] inLon BESSELTM 경도
 * @param[out] outLat EPSG3857 위도
 * @param[out] outLon EPSG3857 경도
 * @return void
*/
void CCoordConvert::BESSELTMToEPSG3857(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;

	BESSELTMToBESSELGEO(inLat, inLon, &tmpLat1, &tmpLon1);
	BESSELGEOToWGS84GEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	WGS84GEOToEPSG3857(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief BESSELTM -> GRS80UTMK 변환
 * @param[in] inLat BESSELTM 위도
 * @param[in] inLon BESSELTM 경도
 * @param[out] outLat GRS80UTMK 위도
 * @param[out] outLon GRS80UTMK 경도
 * @return void
*/
void CCoordConvert::BESSELTMToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	BESSELTMToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80UTMK(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80TM -> BESSELTM 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 * @param[out] outLat BESSELTM 위도
 * @param[out] outLon BESSELTM 경도
 * @return void
*/
void CCoordConvert::GRS80TMToBESSELTM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;
	
	GRS80TMToGRS80GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	GRS80GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToBESSELTM(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief GRS80TM -> GRS80GEO 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::GRS80TMToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];
	
	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	// 타원체 변수 입력 - GRS80  
	double dfA = 6378137;
	double dfEE = 0.00669438;							// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.00674;							// pow(dfE, 2) / (1 - pow(dfE, 2));
	double dfK = 1;
	double dfE1 = 0.00167922;							// (1 - sqrt(1 - dfEE)) / (1 + sqrt(1 - dfEE));    
	
	// 측지계 변수 입력 - GRS80 
	double dfFE = 200000.0;
	double dfFN = 500000.0;
	double dfLON_O = 2.216568150032798;					// 127.0 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207498.0191503200;
	
	// CS_M1
	double dfCS_M1 = dfM0 + (inLat - dfFN ) / dfK;
	
	// CS_U1
	double dfCS_U1 = dfCS_M1 / dfA / (1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256);
	
	// CS_LAT1
	double dfCS_LAT1 = dfCS_U1 + (3 * dfE1 / 2 - 27 * pow(dfE1, 3) / 32) * sin(2 * dfCS_U1)
		+ (21 * pow(dfE1, 2) / 16 - 55 * pow(dfE1, 4) / 32) * sin(4 * dfCS_U1)
		+ (151 * pow(dfE1, 3) / 96) * sin(6 * dfCS_U1)
		+ 1097 * pow(dfE1, 4) / 512 * sin(8 * dfCS_U1);
	
	// CS_V1
	double dfCS_V1 = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 0.5);
	
	// CS_P1
	double dfCS_P1 = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 1.5);
	
	// CS_T1
	double dfCS_T1 = pow(tan(dfCS_LAT1), 2);
	
	// CS_C1
	double dfCS_C1 = dfEE_D * pow(cos(dfCS_LAT1), 2);
	
	// CS_D
	double dfCS_D = (inLon - dfFE) / dfCS_V1 / dfK;
	
	// CS_LAT
	double dfCS_LAT = dfCS_LAT1 - dfCS_V1 * tan(dfCS_LAT1) / dfCS_P1 * pow(dfCS_D, 2) / 2
		- (5 + 3 * dfCS_T1 + 10 * dfCS_C1 - 4 * pow(dfCS_C1, 2) - 9 * dfEE_D) * pow(dfCS_D, 4) / 24
		+ (61 + 90 * dfCS_T1 + 298 * dfCS_C1 + 45 * pow(dfCS_T1, 2) - 252 * dfEE_D - 3 * pow(dfCS_C1, 2)) * pow(dfCS_D, 6) / 720;
	
	// CS_LON
	double dfCS_LON = dfLON_O + (dfCS_D - (1 + 2 * dfCS_T1 + dfCS_C1) * pow(dfCS_D, 3) / 6
		+ (5 - 2 * dfCS_C1 + 28 * dfCS_T1 - 3 * pow(dfCS_C1, 2) + 8 * dfEE_D
		+ 24 * pow(dfCS_T1, 2)) * pow(dfCS_D, 5) / 120) / cos(dfCS_LAT1);

	// 결과 반영
	sprintf(szLon, "%.06lf", dfCS_LON * 180 / M_PI);
	sprintf(szLat, "%.06lf", dfCS_LAT * 180 / M_PI);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief GRS80TM -> KATECH 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 * @param[out] outLat KATECH 위도
 * @param[out] outLon KATECH 경도
 * @return void
*/
void CCoordConvert::GRS80TMToKATECH(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;
	
	GRS80TMToGRS80GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	GRS80GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToKATECH(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief GRS80TM -> BESSELGEO 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 * @param[out] outLat BESSELGEO 위도
 * @param[out] outLon BESSELGEO 경도
 * @return void
*/
void CCoordConvert::GRS80TMToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;
	
	GRS80TMToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToBESSELGEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80TM -> WGS84GEO 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
*/
void CCoordConvert::GRS80TMToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;
	
	GRS80TMToGRS80GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	GRS80GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToWGS84GEO(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief GRS80TM2010(Central Belt 2010, EPSG:5186) -> GRS80GEO 역투영
 * @remark GRS80TMToGRS80GEO 와 동일하되 False Northing 만 600000 (5186)
 * @param[in] inLat 입력 Northing(Y)
 * @param[in] inLon 입력 Easting(X)
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::GRS80TM2010ToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];

	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	// 타원체 변수 입력 - GRS80
	double dfA = 6378137;
	double dfEE = 0.00669438;							// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.00674;							// pow(dfE, 2) / (1 - pow(dfE, 2));
	double dfK = 1;
	double dfE1 = 0.00167922;							// (1 - sqrt(1 - dfEE)) / (1 + sqrt(1 - dfEE));

	// 측지계 변수 입력 - GRS80 Central Belt 2010 (EPSG:5186)
	double dfFE = 200000.0;
	double dfFN = 600000.0;								// 5186 : False Northing 600000 (5181 은 500000)
	double dfLON_O = 2.216568150032798;					// 127.0 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207498.0191503200;

	// CS_M1
	double dfCS_M1 = dfM0 + (inLat - dfFN ) / dfK;

	// CS_U1
	double dfCS_U1 = dfCS_M1 / dfA / (1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256);

	// CS_LAT1
	double dfCS_LAT1 = dfCS_U1 + (3 * dfE1 / 2 - 27 * pow(dfE1, 3) / 32) * sin(2 * dfCS_U1)
		+ (21 * pow(dfE1, 2) / 16 - 55 * pow(dfE1, 4) / 32) * sin(4 * dfCS_U1)
		+ (151 * pow(dfE1, 3) / 96) * sin(6 * dfCS_U1)
		+ 1097 * pow(dfE1, 4) / 512 * sin(8 * dfCS_U1);

	// CS_V1
	double dfCS_V1 = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 0.5);

	// CS_P1
	double dfCS_P1 = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 1.5);

	// CS_T1
	double dfCS_T1 = pow(tan(dfCS_LAT1), 2);

	// CS_C1
	double dfCS_C1 = dfEE_D * pow(cos(dfCS_LAT1), 2);

	// CS_D
	double dfCS_D = (inLon - dfFE) / dfCS_V1 / dfK;

	// CS_LAT
	double dfCS_LAT = dfCS_LAT1 - dfCS_V1 * tan(dfCS_LAT1) / dfCS_P1 * pow(dfCS_D, 2) / 2
		- (5 + 3 * dfCS_T1 + 10 * dfCS_C1 - 4 * pow(dfCS_C1, 2) - 9 * dfEE_D) * pow(dfCS_D, 4) / 24
		+ (61 + 90 * dfCS_T1 + 298 * dfCS_C1 + 45 * pow(dfCS_T1, 2) - 252 * dfEE_D - 3 * pow(dfCS_C1, 2)) * pow(dfCS_D, 6) / 720;

	// CS_LON
	double dfCS_LON = dfLON_O + (dfCS_D - (1 + 2 * dfCS_T1 + dfCS_C1) * pow(dfCS_D, 3) / 6
		+ (5 - 2 * dfCS_C1 + 28 * dfCS_T1 - 3 * pow(dfCS_C1, 2) + 8 * dfEE_D
		+ 24 * pow(dfCS_T1, 2)) * pow(dfCS_D, 5) / 120) / cos(dfCS_LAT1);

	// 결과 반영
	sprintf(szLon, "%.06lf", dfCS_LON * 180 / M_PI);
	sprintf(szLat, "%.06lf", dfCS_LAT * 180 / M_PI);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief GRS80TM2010(EPSG:5186) -> WGS84GEO 변환
 * @param[in] inLat 입력 Northing(Y)
 * @param[in] inLon 입력 Easting(X)
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
*/
void CCoordConvert::GRS80TM2010ToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80TM2010ToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToWGS84GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80TM -> EPSG3857 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 * @param[out] outLat EPSG3857 위도
 * @param[out] outLon EPSG3857 경도
 * @return void
*/
void CCoordConvert::GRS80TMToEPSG3857(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;
	double tmpLat3, tmpLon3;

	GRS80TMToGRS80GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	GRS80GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToWGS84GEO(tmpLat2, tmpLon2, &tmpLat3, &tmpLon3);
	WGS84GEOToEPSG3857(tmpLat3, tmpLon3, outLat, outLon);
}

/**
 * @brief GRS80TM -> GRS80UTMK 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 * @param[out] outLat GRS80UTMK 위도
 * @param[out] outLon GRS80UTMK 경도
 * @return void
*/
void CCoordConvert::GRS80TMToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80TMToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80UTMK(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief EPSG3857 -> BESSELTM 변환
 * @param[in] inLat EPSG3857 위도
 * @param[in] inLon EPSG3857 경도
 * @param[out] outLat BESSELTM 위도
 * @param[out] outLon BESSELTM 경도
 * @return void
*/
void CCoordConvert::EPSG3857ToBESSELTM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;

	EPSG3857ToWGS84GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	WGS84GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToBESSELTM(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief EPSG3857 -> GRS80GEO 변환
 * @param[in] inLat EPSG3857 위도
 * @param[in] inLon EPSG3857 경도
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::EPSG3857ToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;

	EPSG3857ToWGS84GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	WGS84GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToGRS80GEO(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief EPSG3857 -> KATECH 변환
 * @param[in] inLat EPSG3857 위도
 * @param[in] inLon EPSG3857 경도
 * @param[out] outLat KATECH 위도
 * @param[out] outLon KATECH 경도
 * @return void
*/
void CCoordConvert::EPSG3857ToKATECH(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;

	EPSG3857ToWGS84GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	WGS84GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToKATECH(tmpLat2, tmpLon2, outLat, outLon);
}

/**
 * @brief EPSG3857 -> WGS84GEO 변환
 * @param[in] inLat EPSG3857 위도
 * @param[in] inLon EPSG3857 경도
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
*/
void CCoordConvert::EPSG3857ToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	*outLon = inLon * 57.29577951 / R_MAJOR;
	*outLat = 180 / M_PI * (2 * atan(exp(inLat / R_MAJOR)) - M_PI_2);
}

/**
 * @brief EPSG3857 -> GRS80TM 변환
 * @param[in] inLat EPSG3857 위도
 * @param[in] inLon EPSG3857 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 * @return void
*/
void CCoordConvert::EPSG3857ToGRS80TM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat1, tmpLon1;
	double tmpLat2, tmpLon2;
	double tmpLat3, tmpLon3;

	EPSG3857ToWGS84GEO(inLat, inLon, &tmpLat1, &tmpLon1);
	WGS84GEOToBESSELGEO(tmpLat1, tmpLon1, &tmpLat2, &tmpLon2);
	BESSELGEOToGRS80GEO(tmpLat2, tmpLon2, &tmpLat3, &tmpLon3);
	GRS80GEOToGRS80TM(tmpLat3, tmpLon3, outLat, outLon);
}

/**
 * @brief EPSG3857 -> BESSELGEO 변환
 * @param[in] inLat EPSG3857 위도
 * @param[in] inLon EPSG3857 경도
 * @param[out] outLat BESSELGEO 위도
 * @param[out] outLon BESSELGEO 경도
 * @return void
*/
void CCoordConvert::EPSG3857ToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	EPSG3857ToWGS84GEO(inLat, inLon, &tmpLat, &tmpLon);
	WGS84GEOToBESSELGEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief EPSG3857 -> GRS80UTMK 변환
 * @param[in] inLat EPSG3857 위도
 * @param[in] inLon EPSG3857 경도
 * @param[out] outLat GRS80UTMK 위도
 * @param[out] outLon GRS80UTMK 경도
 * @return void
*/
void CCoordConvert::EPSG3857ToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	EPSG3857ToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80UTMK(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80UTMK -> BESSELTM 변환
 * @param[in] inLat GRS80UTMK 위도
 * @param[in] inLon GRS80UTMK 경도
 * @param[out] outLat BESSELTM 위도
 * @param[out] outLon BESSELTM 경도
 * @return void
*/
void CCoordConvert::GRS80UTMKToBESSELTM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80UTMKToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToBESSELTM(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80UTMK -> GRS80GEO 변환
 * @param[in] inLat GRS80TM 위도
 * @param[in] inLon GRS80TM 경도
 *        [정정] 입력은 GRS80TM 이 아니라 GRS80UTMK 의 Northing(inLat)·Easting(inLon), 미터 (2026-10-06 최정우 주석 수정)
 * @param[out] outLat GRS80GEO 위도
 * @param[out] outLon GRS80GEO 경도
 * @return void
*/
void CCoordConvert::GRS80UTMKToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	char szLat[256], szLon[256];
	
	memset(reinterpret_cast<void *>(szLat), 0, sizeof(szLat));
	memset(reinterpret_cast<void *>(szLon), 0, sizeof(szLon));

	// 타원체 변수 입력 - GRS80  
	double dfA = 6378137;
	double dfEE = 0.00669438;							// 2 * dfF - pow(dfF, 2);
	double dfEE_D = 0.00674;							// pow(dfE, 2) / (1 - pow(dfE, 2));
	double dfK = 0.9996;
	double dfE1 = 0.00167922;							// (1 - sqrt(1 - dfEE)) / (1 + sqrt(1 - dfEE));    
	
	// 측지계 변수 입력 - GRS80 
	double dfFE = 1000000.0;
	double dfFN = 2000000.0;
	double dfLON_O = 2.22529479629277;					// 127.5 * 3.1415926535897932384626433832795028842 / 180;
	double dfM0 = 4207498.0191503200;
	
	// CS_M1
	double dfCS_M1 = dfM0 + (inLat - dfFN ) / dfK;
	
	// CS_U1
	double dfCS_U1 = dfCS_M1 / dfA / (1 - dfEE / 4 - 3 * pow(dfEE, 2) / 64 - 5 * pow(dfEE, 3) / 256);
	
	// CS_LAT1
	double dfCS_LAT1 = dfCS_U1 + (3 * dfE1 / 2 - 27 * pow(dfE1, 3) / 32) * sin(2 * dfCS_U1)
		+ (21 * pow(dfE1, 2) / 16 - 55 * pow(dfE1, 4) / 32) * sin(4 * dfCS_U1)
		+ (151 * pow(dfE1, 3) / 96) * sin(6 * dfCS_U1)
		+ 1097 * pow(dfE1, 4) / 512 * sin(8 * dfCS_U1);
	
	// CS_V1
	double dfCS_V1 = dfA / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 0.5);
	
	// CS_P1
	double dfCS_P1 = dfA * (1 - dfEE) / pow(1 - dfEE * pow(sin(dfCS_LAT1), 2), 1.5);
	
	// CS_T1
	double dfCS_T1 = pow(tan(dfCS_LAT1), 2);
	
	// CS_C1
	double dfCS_C1 = dfEE_D * pow(cos(dfCS_LAT1), 2);
	
	// CS_D
	double dfCS_D = (inLon - dfFE) / dfCS_V1 / dfK;
	
	// CS_LAT
	double dfCS_LAT = dfCS_LAT1 - dfCS_V1 * tan(dfCS_LAT1) / dfCS_P1 * pow(dfCS_D, 2) / 2
		- (5 + 3 * dfCS_T1 + 10 * dfCS_C1 - 4 * pow(dfCS_C1, 2) - 9 * dfEE_D) * pow(dfCS_D, 4) / 24
		+ (61 + 90 * dfCS_T1 + 298 * dfCS_C1 + 45 * pow(dfCS_T1, 2) - 252 * dfEE_D - 3 * pow(dfCS_C1, 2)) * pow(dfCS_D, 6) / 720;
	
	// CS_LON
	double dfCS_LON = dfLON_O + (dfCS_D - (1 + 2 * dfCS_T1 + dfCS_C1) * pow(dfCS_D, 3) / 6
		+ (5 - 2 * dfCS_C1 + 28 * dfCS_T1 - 3 * pow(dfCS_C1, 2) + 8 * dfEE_D
		+ 24 * pow(dfCS_T1, 2)) * pow(dfCS_D, 5) / 120) / cos(dfCS_LAT1);

	// 결과 반영
	sprintf(szLon, "%.06lf", dfCS_LON * 180 / M_PI);
	sprintf(szLat, "%.06lf", dfCS_LAT * 180 / M_PI);

	*outLon = atof(szLon);
	*outLat = atof(szLat);
}

/**
 * @brief GRS80UTMK -> KATECH 변환
 * @param[in] inLat GRS80UTMK 위도
 * @param[in] inLon GRS80UTMK 경도
 * @param[out] outLat KATECH 위도
 * @param[out] outLon KATECH 경도
 * @return void
*/
void CCoordConvert::GRS80UTMKToKATECH(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80UTMKToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToKATECH(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80UTMK -> WGS84GEO 변환
 * @param[in] inLat GRS80UTMK 위도
 * @param[in] inLon GRS80UTMK 경도
 * @param[out] outLat WGS84GEO 위도
 * @param[out] outLon WGS84GEO 경도
 * @return void
*/
void CCoordConvert::GRS80UTMKToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80UTMKToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToWGS84GEO(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80UTMK -> GRS80TM 변환
 * @param[in] inLat GRS80UTMK 위도
 * @param[in] inLon GRS80UTMK 경도
 * @param[out] outLat GRS80TM 위도
 * @param[out] outLon GRS80TM 경도
 * @return void
*/
void CCoordConvert::GRS80UTMKToGRS80TM(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80UTMKToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToGRS80TM(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80UTMK -> EPSG3857 변환
 * @param[in] inLat GRS80UTMK 위도
 * @param[in] inLon GRS80UTMK 경도
 * @param[out] outLat EPSG3857 위도
 * @param[out] outLon EPSG3857 경도
 * @return void
*/
void CCoordConvert::GRS80UTMKToEPSG3857(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80UTMKToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToEPSG3857(tmpLat, tmpLon, outLat, outLon);
}

/**
 * @brief GRS80UTMK -> BESSELGEO 변환
 * @param[in] inLat GRS80UTMK 위도
 * @param[in] inLon GRS80UTMK 경도
 * @param[out] outLat BESSELGEO 위도
 * @param[out] outLon BESSELGEO 경도
 * @return void
*/
void CCoordConvert::GRS80UTMKToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon)
{
	double tmpLat, tmpLon;

	GRS80UTMKToGRS80GEO(inLat, inLon, &tmpLat, &tmpLon);
	GRS80GEOToBESSELGEO(tmpLat, tmpLon, outLat, outLon);
}

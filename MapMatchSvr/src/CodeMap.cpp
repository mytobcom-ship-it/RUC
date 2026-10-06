/**
 * @file CodeMap.cpp
 * @brief 코드 테이블별 값 구하기 클래스 소스 파일
*/
#include "CodeMap.h"

/**
 * @brief 생성자
*/
CCodeMap::CCodeMap()
{
}

/**
 * @brief 소멸자
*/
CCodeMap::~CCodeMap()
{
}

/**
 * @brief 메시지에 해당하는 코드
 * @param[in] pstCodeEntry 코드 테이블 구조체
 * @param[in] nEntryCount 코드 등록 갯수
 * @param[in] pszValue 코드 해당 값
 * @return 코드
*/
int CCodeMap::GetCode(PCODE_ENTRY pstCodeEntry, int nEntryCount, char *pszValue)
{
	for (int i=0; i<nEntryCount; ++i)
	{
		if (strcmp(pstCodeEntry[i].pszValue, pszValue) == 0)
			return pstCodeEntry[i].nCode;
	}

	return INVALID_CODE;
}

/**
 * @brief 코드에 해당하는 메시지
 * @param[in] pstCodeEntry 코드 테이블 구조체
 * @param[in] nEntryCount 코드 등록 갯수
 * @param[in] nCode 코드
 * @return 메시지
*/
const char *CCodeMap::GetValue(PCODE_ENTRY pstCodeEntry, int nEntryCount, int nCode)
{
	for (int i=0; i<nEntryCount; ++i)
	{
		if (pstCodeEntry[i].nCode == nCode)
			return pstCodeEntry[i].pszValue;
	}

	// [버그 수정, 2026-09-15 최정우] 미등록 코드에 nullptr 을 돌려주던 것을 안전한 문자열로 바꾼다.
	//   호출부 40여 곳이 반환값을 그대로 strcpy(MapMatch.cpp 9곳) 하거나 LOGFMT 의 %s 로 넘기는데,
	//   nullptr 이면 전자는 즉시 SIGSEGV, 후자도 정의되지 않은 동작이다(glibc 가 "(null)" 을 찍어주는
	//   것에 의존하고 있었을 뿐). 지금까지 터지지 않은 건 쓰이는 코드가 전부 테이블에 있었기 때문이고,
	//   **새 코드를 추가하면서 테이블 등록을 빠뜨리는 순간 크래시**가 나는 구조였다.
	//   nullptr 을 기대하는 호출부는 없음을 전수 확인했다(있으면 이 변경이 동작을 바꿨을 것).
	//   szErrorMsg[48](MessageType.h:216) 에 들어가야 하므로 짧게 유지할 것.
	//   [정정] 행 번호는 바뀌었다 — MessageType.h 의 MATCH_LINK_INFO 계열 szErrorMsg[48] 선언을 가리킨다
	//   (2026-10-06 최정우 주석 수정)
	return "알 수 없는 코드";
}

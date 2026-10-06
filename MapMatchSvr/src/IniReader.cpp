/**
 * @file IniReader.cpp
 * @brief ini 파일 읽기 클래스 소스 파일
 */
#include "IniReader.h"
#include "DataDefine.h"

/**
 * @brief 생성자
*/
CIniReader::CIniReader() : 
	m_strPath(""), 
	m_strFile(""), 
	m_fp(nullptr)
{
	m_mapSection.clear();
}

/**
 * @brief 생성자
 * @param[in] strPath 경로
 * @param[in] strFile 파일명
*/
CIniReader::CIniReader(const string strPath, const string strFile) : 
	m_strPath(strPath), 
	m_strFile(strFile), 
	m_fp(nullptr)
{
	m_strFullName = m_strPath + DIR_DELIMITER + m_strFile;
	m_mapSection.clear();
}

/**
 * @brief 생성자
 * @param[in] strFile 경로 포함 파일명
*/
CIniReader::CIniReader(const string strFile) : 
	m_strFullName(strFile), 
	m_fp(nullptr)
{
	string::size_type nIndex = strFile.rfind(DIR_DELIMITER);

	if (nIndex == string::npos)
	{
		m_strPath = "";
		m_strFile = strFile;
	}
	else
	{
		m_strPath = strFile.substr(0, nIndex);
		// [버그 수정, 2026-09-21 최정우] 종전 `substr(nIndex, ...)` 는 구분자 위치부터 잘라
		//   파일명에 구분자가 붙었다("/home/x/config.ini" → m_strFile="/config.ini").
		//   구분자 다음 문자부터(nIndex + 1) 잘라야 한다. 길이 인자도 "남은 전체"를 뜻하는
		//   npos 로 바로잡는다(strFile.length() 는 남은 길이보다 커서 우연히 동작했을 뿐).
		//   m_strPath/m_strFile 은 현재 읽는 곳이 없어(Open() 은 m_strFullName 만 사용)
		//   동작 변화는 없다 — 나중에 이 값을 쓰는 순간 드러날 결함이라 지금 고쳐둔다.
		m_strFile = strFile.substr(nIndex + 1, string::npos);
	}							// if (nIndex == string::npos)

	m_mapSection.clear();
}

/**
 * @brief 소멸자
*/
CIniReader::~CIniReader()
{
	map<string, void *>::iterator it;
	for (it=m_mapSection.begin(); it!=m_mapSection.end(); ++it)
	{
		multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(it->second);
		// [2026-09-21 최정우 정리] 종전에는 clear() 로 **먼저 역참조한 뒤** nullptr 을 검사했다 —
		//   검사가 방어 역할을 전혀 못 하는 순서였다(정말 nullptr 이면 clear() 에서 이미 죽는다).
		//   ReadIniFile() 이 new 성공분만 등록하므로 실제로 nullptr 이 들어올 일은 없지만,
		//   검사하려면 역참조보다 앞이어야 한다.
		if (mapKey == nullptr) continue;
		mapKey->clear();
		delete mapKey;
		mapKey = nullptr;
	}							// for (it=m_mapSection.begin(); it!=m_mapSection.end(); ++it)

	m_mapSection.clear();
}

/**
 * @brief ini 파일 읽기
 * @return true, false
*/
bool CIniReader::Open()
{
	// 파일명이 없거나 최대 길이 초과시 예외 처리 추가 (2025-12-04 최정우 추가)
	if ((m_strFullName.empty()) || 
		(m_strFullName.length() > static_cast<std::size_t>(MAX_PATH)))
	{
		LOGFMTE("file path is invalid!path=[%s]", m_strFullName.c_str());
		return false;
	}							// if ((m_strFullName.empty()) || 
								//	   (m_strFullName.length() > static_cast<std::size_t>(MAX_PATH)))
								// (2026-09-21 최정우 정정 — 닫는 주석이 PATH_MAX 로 적혀 있어
								//  실제 코드가 쓰는 상수 MAX_PATH 와 달랐다)

	// 파일이 존재하는지 확인 처리 (2025-12-04 최정우 추가)
	if (access(m_strFullName.c_str(), F_OK) != 0)
	{
		LOGFMTE("file is not found!path=[%s]", m_strFullName.c_str());
		return false;
	}							// if (access(m_strFullName.c_str(), F_OK) != 0)

	m_fp = fopen(m_strFullName.c_str(), "r");
	if (!m_fp)
	{
		perror("ini file open failed!\n");
		return false;
	}							// if (!m_fp)

	if (!ReadIniFile())
	{
		perror("ini file read failed!\n");
		fclose(m_fp);
		return false;
	}							// if (!ReadIniFile())

	fclose(m_fp);

	return true;
}

/**
 * @brief ini 파일 줄 단위로 읽기
 * @return true, false
*/
bool CIniReader::ReadIniFile()
{
	char szLine[MAX_LINE_BUFF];
	char szToken[MAX_LINE_BUFF];
	char szValue[MAX_LINE_BUFF];
	char *pszPos, *pszToken;

	// 초기화 (2025-12-04 최정우 추가)
	memset(szLine, 0, static_cast<size_t>(MAX_LINE_BUFF));
	memset(szToken, 0, static_cast<size_t>(MAX_LINE_BUFF));
	memset(szValue, 0, static_cast<size_t>(MAX_LINE_BUFF));

	multimap<string, string> *pmapCurrentSection = nullptr;

	while (fgets(szLine, static_cast<int>(MAX_LINE_BUFF), m_fp))
	{
		// ignore whitespace
		pszPos = szLine;

		// 문자열 종료가 아닌지 확인 코드 추가 (2025-12-04 최정우 보완)
		while ((*pszPos != '\0') && (isspace(static_cast<unsigned char>(*pszPos))))
			pszPos++;

		// Check Comment/Section/Key
		if ((*pszPos == '\0') || (*pszPos == '\r') || (*pszPos == '\n') || (*pszPos == '#') || (*pszPos == ';'))
			continue;
		else if (*pszPos == '[')										// Section
		{
			pszPos++;
			pszToken = szToken;
			while (*pszPos && *pszPos != ']')
				*pszToken++ = *pszPos++;
			*pszToken = '\0';

			string strToken(szToken);
			transform(strToken.begin(), strToken.end(), strToken.begin(), ::toupper);

			// [버그 수정, 2026-09-15 최정우] 같은 섹션명이 두 번 나오는 경우를 처리한다.
			//   종전에는 무조건 새 multimap 을 할당해 m_mapSection.insert() 했는데, 이건
			//   std::map 이라 **중복 키에서 삽입이 조용히 실패**한다(반환값도 안 봤다). 그 결과
			//   (a) 새로 할당한 맵은 어디에도 등록되지 않아 그대로 누수되고,
			//   (b) pmapCurrentSection 이 그 고아 맵을 가리켜 **두 번째 블록의 키가 전부
			//       조회 불가**가 된다(전량 기본값 폴백, 경고도 없음).
			//   config.ini 를 편집하다 섹션을 중복으로 만드는 건 흔한 실수이고, 그때 설정이
			//   조용히 무시되면 원인을 찾기 매우 어렵다. 기존 섹션을 찾아 이어서 채운다.
			map<string, void *>::iterator itSection = m_mapSection.find(strToken);
			if (itSection != m_mapSection.end())
			{
				pmapCurrentSection = reinterpret_cast<multimap<string, string> *>(itSection->second);
				// stderr 로도 낸다 — 이 클래스는 **로거 기동 이전**에 돌아간다(AppMain.cpp:149 에서
				//   config 를 읽고, ILog4zManager::start() 는 638 행). 그래서 LOGFMTW 만으로는
				//   아무 데도 안 남는다. run_svr.sh 가 stderr 를 MapMatchSvr_launcher.log 로
				//   리다이렉트하므로 거기서 확인할 수 있다 (2026-09-15 최정우 추가)
				//   [정정] 위 행 번호는 바뀌었다 — main() → Initialize() 의 cIniReader.Open() 과, 그 뒤
				//   main() 의 ILog4zManager::getRef().start() 를 가리킨다 (2026-10-06 최정우 주석 수정)
				fprintf(stderr, "[WARN] ini duplicated section! section=[%s] path=[%s]"
					" - 기존 섹션에 이어서 읽습니다\n", strToken.c_str(), m_strFullName.c_str());
				LOGFMTW("ini duplicated section!section=[%s] path=[%s] — 기존 섹션에 이어서 읽는다",
					strToken.c_str(), m_strFullName.c_str());
			}
			else
			{
				pmapCurrentSection = new (std::nothrow)multimap<string, string>;
				if (pmapCurrentSection == nullptr)
					return false;
				m_mapSection.insert(pair<string, void *>(strToken, pmapCurrentSection));
			}
		}
		else															// Key
		{
			if (pmapCurrentSection == nullptr) continue;

			if (!ReadKeyValue(pszPos, szToken, szValue)) continue;

			string strToken(szToken);
			transform(strToken.begin(), strToken.end(), strToken.begin(), ::toupper);
			pmapCurrentSection->insert(pair<string, string>(strToken, szValue));
		}						// if ((*pszPos == '\0') || (*pszPos == '\r') || (*pszPos == '\n') || (*pszPos == '#') || (*pszPos == ';'))
	}							// while (fgets(szLine, static_cast<int>(MAX_LINE_BUFF), m_fp))

	return true;
}

/**
 * @brief ini 파일에서 Key/Value 로 분리
 * @param[in] pszBuf ini 파일에서 읽은 key/value 한쌍
 * @param[out] pszKey Key 값
 * @param[out] pszVal Value 값
 * @return true, false
 */
bool CIniReader::ReadKeyValue(char *pszBuf, char *pszKey, char *pszVal)
{
	char *pszPos;

	pszPos = pszKey;
	while ((*pszBuf != '\0') && (*pszBuf != '='))
		*pszPos++ = *pszBuf++;

	// = 가 없는 경우
	if (*pszBuf == '\0') return false;

	// Key 우측 공백 무시
	*pszPos = '\0';
	pszPos--;
	while ((pszPos >= pszKey) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))
	{
		*pszPos = '\0';
		pszPos--;
	}							// while ((pszPos >= pszKey) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))

	// Value
	pszPos = pszVal;
	pszBuf++;
	while ((*pszBuf == ' ') || (*pszBuf == '\t'))
		pszBuf++;

	// [버그 수정, 2026-09-11 최정우] 줄 맨 앞 '#'/';' 만 주석으로 인정하던 것과 달리, 값 뒤에
	//   붙는 인라인 주석("key=value # comment")은 지원이 안 돼 주석 텍스트가 그대로 값에
	//   포함됐었다 — 숫자 파라미터라면 Isdigit()/Isdecimal() 검사에서 조용히 기본값으로 폴백.
	//   query.sql(다른 파서 CSQLAccessor 가 읽음, ';' 가 SQL 문장 종결자라 여기 적용하면 안 됨)이
	//   아니라 config.ini 전용이라 값에 '#'/';' 가 올 일이 없음을 확인하고 적용(현재 config.ini
	//   전체에 값 안에 이 두 문자를 쓰는 줄이 0건).
	while ((*pszBuf != '\0') && (*pszBuf != '#') && (*pszBuf != ';'))
		*pszPos++ = *pszBuf++;

	// Value 우측 공백 무시
	*pszPos = '\0';
	pszPos--;
	while ((pszPos >= pszVal) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))
	{
		*pszPos = '\0';
		pszPos--;
	}							// while ((pszPos >= pszVal) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 문자열 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] strDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] strValue Section, Key 에 해당하는 문자열 값
 * @return true, false
 */
bool CIniReader::GetProfileStr(const string strSection, const string strKey, const string strDefault, string& strValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		strValue = strDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
		// [버그 수정, 2026-09-21 최정우] 이 경로만 출력 파라미터를 건드리지 않고 false 를
		//   돌려주고 있었다 — 같은 함수의 다른 실패 경로(섹션 없음/키 없음)는 전부 기본값을
		//   채워준다. 호출측이 반환값을 안 보고 값만 쓰면 **초기화 안 된 변수**를 쓰게 된다
		//   (AppMain.cpp 의 config 읽기가 실제로 반환값을 대부분 무시한다). 계약을 맞춘다.
	if (mapKey == nullptr)
	{
		strValue = strDefault;
		return false;
	}

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		strValue = strDefault;
		return false;
	}							// if (itKey == mapKey->end())

	strValue = itKey->second;

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 숫자형 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] nDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] nValue Section, Key 에 해당하는 숫자형 값
 * @return true, false
 */
bool CIniReader::GetProfileInt(const string strSection, const string strKey, const int nDefault, int &nValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		nValue = nDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
		// [버그 수정, 2026-09-21 최정우] 이 경로만 출력 파라미터를 건드리지 않고 false 를
		//   돌려주고 있었다 — 같은 함수의 다른 실패 경로(섹션 없음/키 없음)는 전부 기본값을
		//   채워준다. 호출측이 반환값을 안 보고 값만 쓰면 **초기화 안 된 변수**를 쓰게 된다
		//   (AppMain.cpp 의 config 읽기가 실제로 반환값을 대부분 무시한다). 계약을 맞춘다.
	if (mapKey == nullptr)
	{
		nValue = nDefault;
		return false;
	}

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		nValue = nDefault;
		return false;
	}							// if (itKey == mapKey->end())

	// 숫자열인지 검사 — CUtil::Isdigit() 은 선행 '-'(음수)를 지원하지 않는다. 공용 함수를 그대로
	//   두고, 부호 있는 정수 설정값을 다루는 여기서만 선행 '-' 를 떼고 나머지를 검사한다
	//   (2026-09-10 최정우 수정 — alt_penalty 의 "음수=보너스" 설정이 Isdigit() 에서 항상 실패해
	//   조용히 기본값으로 되돌아가던 버그. 최소 재현으로 확인)
	// [주석 정정, 2026-09-17 최정우] 종전 주석은 공용 함수를 못 고치는 근거로 "그 함수의 다른
	//   호출부(Util.cpp StringSplit, set<uint16> 파싱용)는 음수를 걸러내는 게 맞는 동작" 을 들었으나
	//   사실이 아니다 — CUtil::StringSplit 은 전 소스에서 호출처가 **하나도 없는** 미사용 함수다
	//   (실사용 CUtil 멤버는 Isdigit·Isdecimal·Sleep 뿐). 즉 Isdigit() 의 실호출부는 이 파일의
	//   GetProfileInt/GetProfileDouble 계열뿐이다. 지금 방식(호출측에서 부호 처리)을 유지하는
	//   근거는 "다른 호출부 보호" 가 아니라 **공용 함수의 계약을 바꾸지 않는 편이 안전해서**다.
	string strDigitCheck = itKey->second;
	if (!strDigitCheck.empty() && (strDigitCheck[0] == '-'))
		strDigitCheck.erase(0, 1);
	if (strDigitCheck.empty() || !m_cUtil.Isdigit(strDigitCheck))
	{
		nValue = nDefault;
		return false;
	}							// if (!m_cUtil.Isdigit(strDigitCheck))

	// 예외 처리 추가 (2025-12-04 최정우 추가)
	try
	{
		std::size_t ztPos = 0;
		nValue = std::stoi(itKey->second, &ztPos);

		// 값 전체를 int 형으로 변환하지 못한 경우 (2025-12-04 최정우 추가)
		if (ztPos != itKey->second.length())
		{
			nValue = nDefault;
			return false;
		}						// if (ztPos != itKey->second.length())
	}
	catch (const std::exception &e)
	{
		nValue = nDefault;
		return false;
	}							// try

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 실수형 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] fDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] fValue Section, Key 에 해당하는 숫자형 값
 * @return true, false
 * @remark 2024-01-04 최정우 추가
 */
bool CIniReader::GetProfileFloat(const string strSection, const string strKey, const float fDefault, float &fValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		fValue = fDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
		// [버그 수정, 2026-09-21 최정우] 이 경로만 출력 파라미터를 건드리지 않고 false 를
		//   돌려주고 있었다 — 같은 함수의 다른 실패 경로(섹션 없음/키 없음)는 전부 기본값을
		//   채워준다. 호출측이 반환값을 안 보고 값만 쓰면 **초기화 안 된 변수**를 쓰게 된다
		//   (AppMain.cpp 의 config 읽기가 실제로 반환값을 대부분 무시한다). 계약을 맞춘다.
	if (mapKey == nullptr)
	{
		fValue = fDefault;
		return false;
	}

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		fValue = fDefault;
		return false;
	}							// if (itKey == mapKey->end())

	// 실수인지 검사 — CUtil::Isdecimal() 은 선행 '-'(음수)를 지원하지 않는다. GetProfileInt() 의
	//   동일 수정(2026-09-10)과 같은 이유로, 부호 있는 실수 설정값을 다루는 여기서도 선행 '-' 를
	//   떼고 나머지만 검사한다 (2026-09-11 최정우 수정 — Isdigit() 만 고쳐지고 Isdecimal() 은
	//   누락돼있던 것을 재분석 중 발견)
	string strDecimalCheck = itKey->second;
	if (!strDecimalCheck.empty() && (strDecimalCheck[0] == '-'))
		strDecimalCheck.erase(0, 1);
	if (strDecimalCheck.empty() || !m_cUtil.Isdecimal(strDecimalCheck))
	{
		fValue = fDefault;
		return false;
	}							// if (!m_cUtil.Isdecimal(strDecimalCheck))

	// 예외 처리 추가 (2025-12-04 최정우 추가)
	try
	{
		std::size_t ztPos = 0;
		fValue = static_cast<float>(std::stod(itKey->second, &ztPos));

		// 값 전체를 float 형으로 변환하지 못한 경우 (2025-12-04 최정우 추가)
		if (ztPos != itKey->second.length())
		{
			fValue = fDefault;
			return false;
		}						// if (ztPos != itKey->second.length())
	}
	catch (const std::exception &e)
	{
		fValue = fDefault;
		return false;
	}							// try

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 실수형 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] dfDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] dfValue Section, Key 에 해당하는 숫자형 값
 * @return true, false
 * @remark 2024-01-04 최정우 추가
 */
bool CIniReader::GetProfileDouble(const string strSection, const string strKey, const double dfDefault, double &dfValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		dfValue = dfDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
		// [버그 수정, 2026-09-21 최정우] 이 경로만 출력 파라미터를 건드리지 않고 false 를
		//   돌려주고 있었다 — 같은 함수의 다른 실패 경로(섹션 없음/키 없음)는 전부 기본값을
		//   채워준다. 호출측이 반환값을 안 보고 값만 쓰면 **초기화 안 된 변수**를 쓰게 된다
		//   (AppMain.cpp 의 config 읽기가 실제로 반환값을 대부분 무시한다). 계약을 맞춘다.
	if (mapKey == nullptr)
	{
		dfValue = dfDefault;
		return false;
	}

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		dfValue = dfDefault;
		return false;
	}							// if (itKey == mapKey->end())

	// 실수인지 검사 — CUtil::Isdecimal() 은 선행 '-'(음수)를 지원하지 않는다. GetProfileInt() 의
	//   동일 수정(2026-09-10)과 같은 이유로, 부호 있는 실수 설정값을 다루는 여기서도 선행 '-' 를
	//   떼고 나머지만 검사한다 (2026-09-11 최정우 수정 — Isdigit() 만 고쳐지고 Isdecimal() 은
	//   누락돼있던 것을 재분석 중 발견)
	string strDecimalCheck = itKey->second;
	if (!strDecimalCheck.empty() && (strDecimalCheck[0] == '-'))
		strDecimalCheck.erase(0, 1);
	if (strDecimalCheck.empty() || !m_cUtil.Isdecimal(strDecimalCheck))
	{
		dfValue = dfDefault;
		return false;
	}							// if (!m_cUtil.Isdecimal(strDecimalCheck))

	// 예외 처리 추가 (2025-12-04 최정우 추가)
	try
	{
		std::size_t ztPos = 0;
		dfValue = std::stod(itKey->second, &ztPos);

		// 값 전체를 double 형으로 변환하지 못한 경우 (2025-12-04 최정우 추가)
		if (ztPos != itKey->second.length())
		{
			dfValue = dfDefault;
			return false;
		}						// if (ztPos != itKey->second.length())
	}
	catch (const std::exception &e)
	{
		dfValue = dfDefault;
		return false;
	}							// try

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 값 목록
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[out] pstrValue Section, Key 에 해당하는 값 목록
 * @param[in,out] nCount 입력 시 pstrValue 배열의 수용 가능 개수(상한), 출력 시 실제 채운 개수
 * @return true, false
 * @remark
 * \t[미사용 경고, 2026-09-21 최정우 확인] 이 함수와 GetProfileFloat() 은 전 소스에서 호출부가
 * \t0 이다. 같은 키가 여러 번 나오는 설정(multimap 의 equal_range)을 배열로 받으려고 만든
 * \t것으로, 현재 config.ini 에는 그런 키가 없다. 되살릴 때 주의할 점:
 * \t  · nCount 는 **입출력 겸용**이다. 호출 전에 반드시 배열 크기를 넣어야 하며, 0 을 넣으면
 * \t    아무것도 채우지 않고 true 를 돌려준다(위 @param 설명이 out 전용으로만 적혀 있어
 * \t    오해를 부르던 것을 이번에 in,out 으로 정정했다).
 * \t    [정정] 0(또는 음수)을 넣으면 true 가 아니라 **false** 를 돌려주고 nCount=0 으로 만든다 —
 * \t    같은 날 추가된 아래 인자 가드 때문이다 (2026-10-06 최정우 주석 수정)
 * \t  · pstrValue 는 호출측이 할당한 배열이어야 한다 — 아래 nullptr 가드 참조.
 */
bool CIniReader::GetProfileArrayStr(const string strSection, const string strKey, string *pstrValue, int& nCount)
{
	// [2026-09-21 최정우 보완] 배열 포인터·상한 검증 — 종전에는 검사 없이 pstrValue[nCnt] 에
	//   바로 썼다. nCount 가 음수면 아래 `nCnt >= nCount` 가 첫 회에 성립해 쓰기는 없지만,
	//   pstrValue 가 nullptr 이면 그대로 역참조한다.
	if (pstrValue == nullptr || nCount <= 0)
	{
		nCount = 0;
		return false;
	}

	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		nCount = 0;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string>::iterator itKey;
	pair<multimap<string, string>::iterator, multimap<string, string>::iterator> range;
	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
	// [2026-09-21 최정우 보완] 위와 같은 이유로 nCount 를 0 으로 확정한 뒤 실패를 돌려준다.
	if (mapKey == nullptr)
	{
		nCount = 0;
		return false;
	}

	int nCnt = 0;
	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	range = mapKey->equal_range(strTmp);
	for (itKey=range.first; itKey!=range.second; ++itKey)
	{
		if (nCnt >= nCount) break;

		pstrValue[nCnt] = itKey->second;
		nCnt++;
	}							// for (itKey=range.first; itKey!=range.second; ++itKey)
	nCount = nCnt;

	return true;
}

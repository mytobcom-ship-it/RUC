/**
 * @file SQLAccessor.cpp
 * @brief SQL 문 파일 읽기 클래스 소스 파일
*/
#include "SQLAccessor.h"

/**
 * @brief 생성자
*/
CSQLAccessor::CSQLAccessor()
{
	m_SQLFile = "";
	m_mapSQLEntries.clear();
}

/**
 * @brief 소멸자
*/
CSQLAccessor::~CSQLAccessor()
{
	Uninitialize();
}

/**
 * @brief 초기화
 * @param[in] strSQLFile SQL 파일명
 * @return true(성공), false(실패)
*/
bool CSQLAccessor::Initialize(string strSQLFile)
{
	m_SQLFile = strSQLFile;
	if (access(m_SQLFile.c_str(), F_OK) != 0)
	{
		LOGFMTE("SQL file not found!file=[%s]", m_SQLFile.c_str());
		return false;
	}

	return Load();
}

/**
 * @brief 메모리 반환
 * @return void
*/
void CSQLAccessor::Uninitialize()
{
	m_mapSQLEntries.clear();
	LOGFMTI("SQL accessor uninitialize!");
}

/**
 * @brief 파일 읽기 및 등록
 * @return true(성공), false(실패)
 * @warning **query.sql 작성 규칙 — 이 파서의 한계에서 나온 것이라 반드시 지킬 것**
 *   (2026-09-21 최정우 주석 보완. 아래 ①은 2026-09-17 에 실제로 [trip_end]/[trip_abend]
 *    두 SQL 을 통째로 실행 불능으로 만든 장애의 원인이다)
 *   ① **SQL 본문 안에 `--` 주석을 쓰지 말 것.** 이 파서는 줄의 개행을 지우고 공백 하나로
 *      이어 붙이기만 할 뿐 `--` 를 주석으로 인식해 제거하지 않는다. 그래서 본문 중간에 `--`
 *      가 있으면 **그 뒤의 모든 줄이 한 줄짜리 주석 안으로 빨려 들어가** 쿼리가 잘린다
 *      (증상: "구문 오류, 입력 끝부분"). 설명은 `[key]` 줄 **위**에 두면 된다 — 섹션 헤더
 *      앞의 주석 줄은 어느 섹션에도 담기지 않기 때문이다.
 *   ② 각 SQL 은 세미콜론으로 끝내야 등록된다. 세미콜론이 없으면 다음 `[key]` 를 만나는 순간
 *      그때까지 모은 본문이 조용히 버려진다(sql="" 로 초기화).
 *   ③ 한 섹션 안에 세미콜론으로 끝나는 문장을 2 개 이상 두지 말 것 — 아래 중복 키 경고 참고.
 *   ④ `[key]` 는 줄 **맨 앞**에서 시작해야 한다(들여쓰면 SQL 본문으로 취급된다).
*/
bool CSQLAccessor::Load()
{
	FILE *fp = fopen(m_SQLFile.c_str(), "r");
	char buf[1024], *ptr, *qtr;
	int read = 0;
	string key = "";
	string sql = "";

	if (fp == nullptr)
	{
		LOGFMTE("[%s] SQL file open fail!", m_SQLFile.c_str());
		return false;
	}

	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	if (size <= 0)
	{
		fclose(fp);
		LOGFMTE("[%s] SQL file empty!", m_SQLFile.c_str());
		return false;
	}
	fseek(fp, 0, SEEK_SET);

	// while(!feof()) 는 고전적인 실수 — 마지막 줄을 성공적으로 읽은 시점엔 아직 feof() 가 안 서서
	//   루프가 한 번 더 돌고, 그 fgets() 는 EOF 에서 실패해 buf 를 안 건드린 채(직전 줄 내용이
	//   그대로 남음) 다시 처리해버림. fgets() 자체의 반환값(nullptr=더 읽을 게 없음)으로 판정하는
	//   게 정석(2026-08-14 최정우 수정 — "소스상 문제" 검토 중 발견)
	while (fgets(buf, sizeof(buf)-1, fp) != nullptr)
	{
		read = (int)strlen(buf);

		if (buf[0] == '#') continue;

		// [버그 수정, 2026-09-10 최정우] isspace() 는 인자가 unsigned char 범위(또는 EOF)여야
		// 하는데 buf 는 (플랫폼 기본) signed char 라, UTF-8 멀티바이트 문자(한글 등)의 0x80 이상
		// 바이트를 그대로 넘기면 음수가 되어 정의되지 않은 동작이다. 실 운영 query.sql(UTF-8,
		// 한글 주석 다수)에서 실제로 73줄이 개행 직전 이런 바이트로 끝나 매번 이 코드를 타는
		// 것을 확인했다 — 같은 패턴을 IniReader.cpp:139 는 이미 unsigned char 캐스트로 고쳐
		// 뒀는데 여기는 빠뜨렸다.
		// [정정] "IniReader.cpp:139" 는 행 번호가 바뀌었다 — CIniReader::ReadIniFile() 의 앞쪽 공백
		//   건너뛰기 루프를 가리킨다 (2026-10-06 최정우 주석 수정)
		for (; read>0 && isspace(static_cast<unsigned char>(buf[read-1])); read--) buf[read-1] = 0x00;
		if (buf[0] == 0x00) continue;

		buf[read] = ' ';
		buf[read+1] = 0x00;

		for (ptr=buf; isspace(static_cast<unsigned char>(*ptr)); ptr++);

		// [2026-09-21 최정우 확인] 바로 위에서 앞쪽 공백을 건너뛴 ptr 을 구해놓고, 섹션 판정은
		//   ptr 이 아니라 buf[0] 으로 한다 — 즉 **들여쓴 `[key]` 는 섹션이 아니라 SQL 본문으로
		//   처리**된다. 현재 query.sql 은 모든 섹션 헤더가 줄 맨 앞이라 문제가 없고, 판정을
		//   ptr 기준으로 바꾸면 "SQL 본문 안의 대괄호로 시작하는 줄"(배열 리터럴 등)이 섹션으로
		//   오인될 수 있어 일부러 그대로 둔다. 위 @warning ④ 로 규칙을 명시했다.
		if (buf[0] == '[')
		{
			ptr++;
			for (qtr=ptr; *qtr && *qtr != ']'; qtr++);
			*qtr = 0x00;
			key = ptr;
			sql = "";
		}
		else
		{
			for (qtr=ptr; *qtr && *qtr != ';'; qtr++);
			if (*qtr == ';')
			{
				*qtr = 0x00;
				sql += ptr;

				// [버그 수정, 2026-09-21 최정우] std::map::insert 는 **이미 있는 키면 아무 것도
				//   하지 않고 조용히 성공한 것처럼 반환**한다(반환값도 안 봤다). 그래서
				//   (a) query.sql 에 같은 `[key]` 섹션을 실수로 두 번 쓰거나
				//   (b) 한 섹션 안에 세미콜론으로 끝나는 문장을 두 개 두면
				//   **뒤에 쓴 SQL 이 흔적 없이 버려지고 앞의 것이 계속 쓰인다.** 설정을 고쳤는데
				//   반영이 안 되는 형태라 원인을 찾기가 매우 어렵다(CIniReader 는 2026-09-15 에
				//   같은 문제를 같은 이유로 고쳤는데 이 파서만 빠져 있었다).
				//   동작(먼저 쓴 것 유지)은 바꾸지 않고 — 바꾸면 기존 파일의 해석이 달라진다 —
				//   대신 어떤 키가 무시됐는지 반드시 남긴다.
				if (!m_mapSQLEntries.insert(pair<string, string>(key, sql)).second)
				{
					LOGFMTE("duplicate SQL key!key=[%s] file=[%s] — 뒤에 나온 SQL 은 무시되고 "
						"먼저 등록된 것이 계속 사용된다(같은 섹션명 중복 또는 한 섹션에 문장 2개)",
						key.c_str(), m_SQLFile.c_str());
				}
				// 섹션 헤더를 만나기 전에 나온 SQL — 어느 키에도 속하지 않아 빈 키로 등록된다
				if (key.empty())
				{
					LOGFMTE("SQL statement before any [key] section!file=[%s] — 빈 키로 등록됨, "
						"섹션 헤더를 먼저 둘 것", m_SQLFile.c_str());
				}
				sql = "";
			}
			else
				sql += ptr;
		}
	}

	fclose(fp);
	return true;
}

/**
 * @brief 키 값을 이용하여 SQL 문 읽기
 * @param[in] key SQL 문 키 값
 * @return SQL 문
 *   키가 없으면 빈 문자열 — 호출측(CServer::Initialize)은 빈 문자열로 필수/선택 SQL 누락을 판정한다
 *   (2026-10-06 최정우 주석 추가)
*/
string CSQLAccessor::GetSQL(string key)
{
	map<string, string>::iterator it;

	it = m_mapSQLEntries.find(key);
	if (it == m_mapSQLEntries.end()) return "";
	return it->second;
}

/**
 * @file LoggerManager.cpp
 * @brief 로그 관리 클래스 소스 파일
*/
#include "LoggerManager.h"

/**
 * @brief 생성자
*/
CLoggerManager::CLoggerManager() : 
	m_nLogKeepRunTime(0), 
	m_nLogKeepDay(0), 
	m_bRun(true)
{
	m_strLogPath.clear();
	m_dtTime = time(nullptr);
}

/**
 * @brief 소멸자
*/
CLoggerManager::~CLoggerManager()
{
}

/**
 * @brief 초기화
 * @param[in] strLogPath 로그 경로
 * @param[in] nLogKeepRunTime 실행 시간
 *            [정정] 만료 로그 삭제를 실행할 시각(시, 0~23). 음수면 삭제 기능 미사용(UNUSE_LOG_KEEP)
 *            (2026-10-06 최정우 주석 수정)
 * @param[in] nLogKeepDay 로그 보관일 (단위 : 날자)
 * @return true(성공), false(실패)
*/
bool CLoggerManager::Initialize(const string strLogPath, 
	const int nLogKeepRunTime, const int nLogKeepDay)
{
	m_strLogPath = strLogPath;
	m_nLogKeepRunTime = (nLogKeepRunTime >= 0) ? nLogKeepRunTime : UNUSE_LOG_KEEP;
	m_nLogKeepDay = nLogKeepDay;
	m_bRun = true;
	m_dtTime = time(nullptr);

	// log path is exist check
	if (m_strLogPath.empty())
	{
		LOGFMTE("log path is empty!path=[%s]", m_strLogPath.c_str());
		return false;
	}

	// log path is exist check
	// 로그 디렉터리 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(m_strLogPath.c_str(), F_OK) != 0)
	{
		LOGFMTE("log path is not exist!path=[%s]", m_strLogPath.c_str());
		return false;
	}

	// runtime check
	if (m_nLogKeepRunTime != UNUSE_LOG_KEEP)
	{
		if ((m_nLogKeepRunTime < 0) || (m_nLogKeepRunTime > 23))
		{
			LOGFMTE("runtime is invalid!runtime=[%d]", m_nLogKeepRunTime);
			return false;
		}

		// log 보관 일수 확인
		if (m_nLogKeepDay <= 0)
		{
			LOGFMTE("log keep day is invalid!day=[%d]", m_nLogKeepDay);
			return false;
		}
	}

	return true;
}

/**
 * @brief 로그 삭제 실행
 * @param[in] dtNow 현재 시각 (초)
 * @return void
*/
void CLoggerManager::LogDeleteRun(time_t dtNow)
{
	struct tm stTm;

	// 실행 안함
	if (m_nLogKeepRunTime <= UNUSE_LOG_KEEP) return;

	// 30초 경과 이전 이면 ...
	if ((dtNow - m_dtTime) <= 30) return;

	m_dtTime = dtNow;
	// 현재 시각을 struct tm 으로 변환 (실행 시각 판별) (2026-07-08 최정우 주석 추가)
	localtime_r(&m_dtTime, &stTm);
	if (stTm.tm_hour != m_nLogKeepRunTime)
	{
		m_bRun = true;
		return;
	}

	// 실행 시간 1 번만 실행 플래그
	// [2026-09-21 최정우 주석 보완] m_bRun 은 이름과 달리 "서버가 실행 중" 이 아니라
	//   **"이번 실행 시각(시간대)에 아직 삭제를 안 돌렸다"** 는 뜻이다(삭제 직후 false,
	//   다음 시간대로 넘어가면 위에서 true 로 복귀). CServer::m_bRun(서버 구동 플래그)과
	//   이름이 같아 혼동하기 쉬운 자리다.
	if (!m_bRun) return;

	if ((m_bRun) && (stTm.tm_hour == m_nLogKeepRunTime))
	{
		time_t dtRmTime = m_dtTime - (m_nLogKeepDay * 60 * 60 * 24);
		m_bRun = false;
		// 보관일 초과 .log 파일 재귀 삭제 (2026-07-08 최정우 주석 추가)
		SetRemoveLogFile(dtRmTime, m_strLogPath);
	}
}

/**
 * @brief 로그 경로내 파일 목록
 *   [정정] 목록 조회가 아니라 삭제 함수다 — 경로 아래(하위 디렉터리 재귀, 심링크 디렉터리 제외)의
 *   ".log" 로 끝나는 파일 중 수정시각이 dtRmTime 보다 이른 것을 remove() 한다 (2026-10-06 최정우 주석 수정)
 * @param[in] dtRmTime 로그 삭제 파일 시간 (초)
 * @param[in] strLogPath 로그 경로
 * @return true(성공), false(실패)
*/
bool CLoggerManager::SetRemoveLogFile(time_t dtRmTime, string strLogPath)
{
	struct stat stStatInfo;

	// 대상 로그 경로 접근 가능 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(strLogPath.c_str(), F_OK) != 0)
	{
		LOGFMTW("file or directory is not exist!path=[%s]", strLogPath.c_str());
		return false;
	}

	// init
	memset(reinterpret_cast<void *>(&stStatInfo), 0, sizeof(struct stat));

	// 로그 경로가 디렉터리인지 lstat 확인 (2026-07-08 최정우 주석 추가)
	// [2026-09-21 최정우 보완] lstat 반환값을 안 보고 st_mode 를 읽고 있었다. 실패하면 memset
	//   해둔 0 이 그대로라 S_ISDIR(0)=false 가 되어 "디렉터리가 아님" 으로 빠지므로 결과적으로는
	//   안전했지만, 실패와 "파일임" 이 구분되지 않아 로그가 원인을 가린다.
	if (lstat(strLogPath.c_str(), &stStatInfo) != 0)
	{
		LOGFMTW("lstat failed!path=[%s] err=[%d : %s]",
			strLogPath.c_str(), errno, strerror(errno));
		return false;
	}
	if (!S_ISDIR(stStatInfo.st_mode))
	{
		LOGFMTW("directory is not found!path=[%s]", strLogPath.c_str());
		return false;
	}

	DIR *pDir = nullptr;
	struct dirent *pstEntry = nullptr;

	// 로그 디렉터리 열거 시작 (2026-07-08 최정우 주석 추가)
	if ((pDir = opendir(strLogPath.c_str())) == nullptr)
	{
		LOGFMTW("directory open failed!path=[%s]", strLogPath.c_str());
		return false;
	}

	while ((pstEntry = readdir(pDir)) != nullptr)
	{
		// init
		memset(reinterpret_cast<void *>(&stStatInfo), 0, sizeof(struct stat));

		string strFilePath = strLogPath + "/" + pstEntry->d_name;
		while (strFilePath.find("//") != string::npos)
			strFilePath.replace(strFilePath.find("//"), 2, "/");

		// [버그 수정, 2026-09-21 최정우] stat 반환값을 안 봤다. 실패하면(권한 변경, 조회 직전에
		//   다른 프로세스가 지운 경우 등) stStatInfo 가 memset 해둔 0 그대로라 **st_mtim=0 이
		//   되어 "아주 오래된 파일" 로 판정**되고, 이름이 .log 로 끝나기만 하면 그대로 remove()
		//   대상이 됐다. 지금 존재를 확인할 수 없는 파일은 건드리지 않는 편이 맞다.
		if (stat(strFilePath.c_str(), &stStatInfo) != 0)
		{
			LOGFMTW("stat failed!skip - file=[%s] err=[%d : %s]",
				strFilePath.c_str(), errno, strerror(errno));
			continue;
		}

		if (S_ISDIR(stStatInfo.st_mode))						// 디렉토리이면 ...
		{
			if ((strcmp(pstEntry->d_name, "..") == 0) || 
				(strcmp(pstEntry->d_name, ".") == 0))
				continue;

			// [버그 수정, 2026-09-21 최정우] 위 stat() 은 심볼릭 링크를 따라간다. 로그 디렉터리
			//   안에 상위(또는 자기 조상)를 가리키는 심링크가 하나라도 있으면 이 재귀가 그 링크를
			//   타고 영원히 내려가 스택을 소진한다(`.`/`..` 만 걸러서는 막히지 않는다).
			//   링크 자체의 정보를 보는 lstat 으로 한 번 더 확인해 심링크면 내려가지 않는다.
			string strSubDir = strLogPath + "/" + pstEntry->d_name;
			struct stat stLinkInfo;
			memset(reinterpret_cast<void *>(&stLinkInfo), 0, sizeof(struct stat));
			if ((lstat(strSubDir.c_str(), &stLinkInfo) == 0) && S_ISLNK(stLinkInfo.st_mode))
			{
				LOGFMTW("symlink directory skipped!path=[%s] — 재귀 순환 방지", strSubDir.c_str());
				continue;
			}

			// 하위 디렉터리 재귀 탐색 (2026-07-08 최정우 주석 추가)
			SetRemoveLogFile(dtRmTime, strSubDir);
		}
		else													// 파일이면 ...
		{
			string strExten = ".log";
			// rfind 는 문자열 어디든 ".log" 가 있으면 매치되어(예: foo.log-20260101) 확장자가
			//   아닌 파일도 삭제 대상에 걸릴 수 있었음 — 반드시 "끝이 .log 로 끝나는지" 검사로
			//   변경(2026-08-14 최정우 수정 — "소스상 문제" 검토 중 발견)
			if ((strFilePath.size() >= strExten.size()) &&
				(strFilePath.compare(strFilePath.size() - strExten.size(), strExten.size(), strExten) == 0))
			{
				if (stStatInfo.st_mtim.tv_sec < dtRmTime)
				{
					// 보관 기한 초과 .log 파일 삭제 (2026-07-08 최정우 주석 추가)
					if (remove(strFilePath.c_str()) != 0)
						LOGFMTW("[%s] log file remove failed!file=[%d : %s]", strFilePath.c_str(), errno, strerror(errno));
					else
						LOGFMTI("[%s] log file remove success!", strFilePath.c_str());
				}
			}
		}
	}

	// 디렉터리 핸들 닫기 (2026-07-08 최정우 주석 추가)
	closedir(pDir);

	return true;
}
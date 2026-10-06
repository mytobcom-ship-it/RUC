/**
 * @file PostgrePool.cpp
 * @brief Postgre DB 연결 관리 클래스 소스 파일
*/
#include "PostgrePool.h"

/**
 * @brief DB 연결 확인 쓰레드
 * @param[in] arg CPostgrePool 클래스 포인터
 * @return void
*/
void *CPostgrePool::KeepAliveThread(void *arg)
{
	CPostgrePool *pcPostgrePool = reinterpret_cast<CPostgrePool *>(arg);
	if (!pcPostgrePool) return nullptr;

	pcPostgrePool->keepPoolAlive();
	return nullptr;
}

/**
 * @brief 생성자
*/
CPostgrePool::CPostgrePool()
{
	m_dqQueue.clear();
	m_nPooledConnections = 0;
	m_bIsValid = false;
	m_bUninitialized = false;					// (2026-09-21 최정우 추가)
}

/**
 * @brief 소멸자
*/
CPostgrePool::~CPostgrePool()
{
	UninitializePool();
}

/**
 * @brief DB 연결 및 초기화
 * @param[in] strUserID 접속 아이디
 * @param[in] strPassword 접속 비밀번호
 * @param[in] strDataBase 데이터베이스
 * @param[in] strHost 접속 아이피
 * @param[in] nPort 접속 포트
 * @param[in] nMinConnect 최소 접속 수
 * @param[in] nMaxConnect 최대 접속 수
 * @param[in] nTimeOut 데이터베이스 연결 검사 (초)
 *            keepPoolAlive() 의 검사 주기이기도 하다(0 이하면 60). CServer 는 이 인자를 넘기지 않아
 *            헤더 기본값(60초)을 쓴다 (2026-10-06 최정우 주석 추가)
 * @return true(성공), false(실패)
*/
bool CPostgrePool::InitializePool(string strUserID, string strPassword, string strDataBase, 
		string strHost, int nPort, int nMinConnect, int nMaxConnect, int nTimeOut)
{
	m_strUserID = strUserID;
	m_strPassword = strPassword;
	m_strDataBase = strDataBase;
	m_strHost = strHost;
	m_strPort = std::to_string(nPort);
	m_nMinConnect = nMinConnect;
	m_nMaxConnect = nMaxConnect;
	m_nTimeOut = nTimeOut;

	if (m_nMinConnect < 1) m_nMinConnect = 1;
	if (m_nMaxConnect < m_nMinConnect) m_nMaxConnect = m_nMinConnect;
	if (m_nTimeOut <= 0) m_nTimeOut = 60;

	for (int i=0; i<m_nMinConnect; ++i)
	{
		if (createConnection() == nullptr)
		{
			UninitializePool();
			return false;
		}
	}

	if (pthread_create(&m_hThread, nullptr, KeepAliveThread, reinterpret_cast<void *>(this)) != 0)
	{
		UninitializePool();
		return false;
	}

	m_bIsValid = true;

	// 기동 시 실제 접속 성공한 커넥션 수를 요청 min/max 와 함께 남긴다 — 접속은 성공했지만
	//   요청보다 적게 붙은 경우를 구분해서 볼 수 있도록 (2026-09-04 최정우 추가, 사용자 지시)
	LOGFMTI("postgre connection pool ready!host=[%s] port=[%s] db=[%s] userid=[%s] "
		"connected=[%d] min=[%d] max=[%d] timeout=[%d]s",
		m_strHost.c_str(), m_strPort.c_str(), m_strDataBase.c_str(), m_strUserID.c_str(),
		m_nPooledConnections, m_nMinConnect, m_nMaxConnect, m_nTimeOut);

	return true;
}

/**
 * @brief DB 연결 종료 및 메모리 반환
 * @return void
*/
void CPostgrePool::UninitializePool()
{
	// [버그 수정, 2026-09-21 최정우] **이 함수는 종료 시 두 번 불린다** —
	//   CServer::Uninitialize() 의 명시적 호출 + delete 가 부르는 소멸자. 그 명시적 호출은
	//   의도된 것이라 없애면 안 된다(m_bSkipDependentTeardown 이 true 면 delete 를 건너뛰는데
	//   그때도 풀은 닫아야 한다 — Server.cpp 해당 주석 참고). 대신 여기서 중복 실행을 막는다.
	//   종전에는 아래 m_bIsValid 가드가 **스레드 cancel/join 만** 덮고 있어서, 두 번째 호출도
	//   (a) 락을 잡아 m_bIsValid 를 다시 내리고 조건변수를 broadcast 하고,
	//   (b) 유휴 커넥션 회수 루프를 다시 돌고,
	//   (c) "postgre connection pool is uninitialized!" 로그를 한 번 더 찍었다.
	//   (c) 는 로그가 두 줄 나오는 정도지만, **(b) 는 첫 호출이 타임아웃(대여 중 커넥션이 반납되지
	//   않아 left > 0)된 경우 두 번째 호출에서 최대 5초를 또 기다려 종료가 그만큼 늦어진다.**
	//   m_bIsValid 로는 이 가드를 대신할 수 없다 — 그 값은 정리 도중에도 false 이기 때문이다.
	if (m_bUninitialized)
		return;
	m_bUninitialized = true;

	int left = m_nPooledConnections;

	if (m_bIsValid)
	{
		// m_cMutex 를 잡은 채로 cancel+join 하지 말 것 — keepPoolAlive() 는 sleep() 뒤 매 주기
		// m_cMutex.lock() 을 다시 시도하는데(pthread_mutex_lock 은 취소 지점이 아님), 여기서 락을
		// 쥔 채 pthread_join() 으로 대기하면 상대 스레드가 락을 못 잡아 취소 지점(sleep)에 영영
		// 도달 못하고, 우리도 join 에서 영영 못 빠져나오는 데드락이 됨(2026-08-14 최정우 수정 —
		// "소스상 문제" 검토 중 발견). cancel+join 구간은 m_dqQueue 등 공유 상태를 안 건드리므로
		// 락 없이도 안전
		pthread_cancel(m_hThread);
		if (pthread_join(m_hThread, nullptr) != 0)
			LOGFMTE("Can not join pthread for PostgreSQL Connection Pool!");
	}

	// [버그 수정, 2026-09-11 최정우] 대여 중(checked-out)인 채 반납 안 된 커넥션이 하나라도 있으면
	//   m_nPooledConnections 가 0으로 안 내려가는데, 이 루프는 sleep/timeout 없이 락만 걸었다 풀었다
	//   하며 계속 재검사해 CPU 100% busy-spin 으로 영구히 멈췄었다 — WaitForActiveIdle() 타임아웃이
	//   만료돼도 그 워커가 커넥션을 쥔 채 계속 돌고 있으면 재현됐음. ThreadPool 소멸자와 동일하게
	//   최대 대기(100ms × 최대횟수)를 두고, 시간 안에 못 비우면 경고만 남기고 진행한다 — 큐에 남아
	//   있는 유휴 커넥션은 그동안 계속 정리해준다.
	// [버그 수정, 2026-09-17 최정우] 유휴 커넥션 회수 전에 m_bIsValid 를 내리고 대기자를 깨운다 —
	//   getConnection() 이 "풀이 가득 참" 조건으로 조건변수에서 자고 있으면 아래 회수 루프가
	//   아무리 돌아도 그 스레드는 영영 안 깨어나고, 그 상태로 풀이 해제되면 깨어난 뒤 이미 파괴된
	//   뮤텍스/조건변수를 만진다. 여기서 먼저 내려야 깨어난 쪽이 술어 재검사에서 종료를 관측한다.
	m_cMutex.lock();
	m_bIsValid = false;
	m_cCondition.broadcast();
	m_cMutex.unlock();

	const int nMaxWaitIter = 50;								// 100ms * 50 = 최대 5초
	for (int i = 0; (left > 0) && (i < nMaxWaitIter); ++i)
	{
		m_cMutex.lock();
		while (static_cast<int>(m_dqQueue.size()) > 0)
		{
			freeConnection(m_dqQueue.front());
			m_dqQueue.pop_front();
		}
		left = m_nPooledConnections;
		m_cMutex.unlock();

		if (left > 0)
			usleep(100 * 1000);
	}

	if (left > 0)
		LOGFMTW("postgre pool uninitialize timeout!still_pooled=[%d] (checked-out connections never released)", left);

	m_dqQueue.clear();
	m_bIsValid = false;
	LOGFMTI("postgre connection pool is uninitialized!");
}

/**
 * @brief DB 연결
 * @return DB 연결 핸들 값, nullptr
*/
PGconn *CPostgrePool::createConnection()
{
	PGconn *pcHandle = PQsetdbLogin(m_strHost.c_str(), m_strPort.c_str(), nullptr, nullptr, 
		m_strDataBase.c_str(), m_strUserID.c_str(), m_strPassword.c_str());
	if (pcHandle == nullptr)
	{
		LOGFMTE("DB connection alloc fail!");
		return nullptr;
	}

	if (PQstatus(pcHandle) != CONNECTION_OK)
	{
		LOGFMTE("DB connection fail!error=[%s]", PQerrorMessage(pcHandle));
		PQfinish(pcHandle);
		return nullptr;
	}

	m_dqQueue.push_back(pcHandle);
	m_nPooledConnections++;
	return pcHandle;
}

/**
 * @brief DB 연결 종료
 * @param[in] pcHandle DB 연결 핸들 값
 * @return void
*/
void CPostgrePool::freeConnection(PGconn *pcHandle)
{
	// [정리, 2026-09-17 최정우] 여기 있던 `pcHandle = nullptr;` 를 제거했다 — 값 전달 파라미터라
	//   호출측 포인터에는 아무 영향이 없는데(지역 사본만 바뀐다) "무효화했다" 는 오해를 부른다.
	//   실제 무효화는 호출측이 큐에서 제거하는 것으로 이뤄진다(m_dqQueue.pop_front()/erase()).
	PQfinish(pcHandle);
	if (m_nPooledConnections > 0) m_nPooledConnections--;
}

/**
 * @brief DB 연결 세션 얻기
 * @return DB 연결 핸들 값
 *   nullptr = 풀 무효·종료 중이거나 새 연결 생성 실패 (2026-10-06 최정우 주석 추가)
 * @remark 유휴 연결이 없고 보유수가 maxconnect 에 도달하면 releaseConnection()·종료 통지가 올 때까지
 *   **타임아웃 없이 블록**한다. 새 연결 생성(PQsetdbLogin)도 풀 뮤텍스를 쥔 채 수행된다
 *   (2026-10-06 최정우 주석 추가)
*/
PGconn *CPostgrePool::getConnection()
{
	PGconn *pcHandle = nullptr;

	m_cMutex.lock();
	if (!m_bIsValid)
	{
		m_cMutex.unlock();
		LOGFMTE("db connection pool is invalid!");
		return nullptr;
	}

	// [버그 수정, 2026-09-17 최정우] 대기 조건에 m_bIsValid 를 넣었다. 종전에는 풀이 가득 찬 상태
	//   (유휴 큐 비었고 보유수 >= maxconnect)에서 대기 중인 스레드를 **종료 시 깨울 방법이 없었다** —
	//   UninitializePool() 이 m_bIsValid 를 false 로 내려도 조건변수 통지가 없어 영구 대기였고,
	//   설령 통지가 와도 이 술어가 그대로 참이라 다시 잠들었다. 아래 UninitializePool() 의
	//   broadcast 와 짝을 이룬다. 종료 중이면 루프를 빠져나와 nullptr 을 돌려준다.
	while (m_bIsValid && (m_dqQueue.empty()) && 
		(m_nPooledConnections >= m_nMaxConnect))
		m_cCondition.wait(m_cMutex);

	if (!m_bIsValid)
	{
		m_cMutex.unlock();
		LOGFMTW("db connection pool is shutting down!getConnection aborted");
		return nullptr;
	}

	if (!m_dqQueue.empty())
	{
		pcHandle = m_dqQueue.front();
		m_dqQueue.pop_front();
	}
	else if (m_nPooledConnections < m_nMaxConnect)
	{
		pcHandle = createConnection();
		if (pcHandle != nullptr)
		{
			// createConnection() 은 유휴 큐에 push_back 하므로, 대여 시 큐에서 제거
			if (!m_dqQueue.empty() && (m_dqQueue.back() == pcHandle))
				m_dqQueue.pop_back();
		}
	}

	m_cMutex.unlock();
	return pcHandle;
}

/**
 * @brief DB 연결 세션 반환
 * @param[in] pcHandle DB 연결 핸들값
 * @return void
*/
void CPostgrePool::releaseConnection(PGconn *pcHandle)
{
	// [버그 수정, 2026-09-17 최정우] nullptr 반납 시 m_nPooledConnections 를 감소시키던 것을
	//   제거했다. nullptr 은 **대여된 적이 없는 값**이라(getConnection() 이 실패를 알리는 반환값),
	//   이걸 반납했다고 보유 수를 깎으면 실제로 살아있는 커넥션 수와 카운터가 어긋난다 —
	//   카운터가 부당하게 줄면 keepPoolAlive() 가 minconnect 를 채우려 커넥션을 계속 새로 만들고,
	//   반대로 getConnection() 의 maxconnect 상한 판정도 틀어진다.
	//   현재 호출부 13곳은 전부 getConnection() 성공분만 반납해(실패 시 곧바로 return) 실제로
	//   발동하지는 않았다 — 잘못된 방어 코드였다. 앞으로 이 경로로 들어오면 로그로 드러나게 한다.
	if (!pcHandle)
	{
		LOGFMTW("releaseConnection called with null handle!ignored (pooled=[%d])",
			m_nPooledConnections);
		return;
	}

	m_cMutex.lock();
	m_dqQueue.push_back(pcHandle);
	m_cCondition.signal();
	m_cMutex.unlock();
}

/**
 * @brief **앞으로 더 만들 수 있는** 연결 세션 수 구하기 (maxconnect - 보유수)
 * @return 추가 생성 가능 수 (0 이상)
 * @remark [주석 정정, 2026-09-17 최정우] 종전 @brief 는 "사용 가능한 연결 세션 수" 였는데, 이 값은
 *   **지금 바로 대여할 수 있는 수가 아니다**. 즉시 대여 가능한 수는 유휴 큐 크기(m_dqQueue.size())
 *   이고 이 함수는 그것을 돌려주지 않는다 — 보유수가 maxconnect 에 도달하면 유휴 커넥션이 남아
 *   있어도 0 을 반환한다. getPooledConnections()(보유수)와 헷갈리지 말 것
 *   (그쪽 @brief 는 2026-09-17 에 먼저 정정했다).
*/
int CPostgrePool::getAvailableConnections()
{
	m_cMutex.lock();
	int nCount = m_nMaxConnect - m_nPooledConnections;
	if (nCount < 0) nCount = 0;
	m_cMutex.unlock();

	return nCount;
}

/**
 * @brief 사용 중인 연결 세션 수 구하기
 * @return 사용 중인 연결 세션 수
*/
int CPostgrePool::getActiveConnections()
{
	m_cMutex.lock();
	int nCount = m_nPooledConnections - static_cast<int>(m_dqQueue.size());
	if (nCount < 0) nCount = 0;
	m_cMutex.unlock();

	return nCount;
}

/**
 * @brief 풀이 현재 보유(생성)하고 있는 연결 세션 수 구하기
 * @return 보유 중인 연결 세션 수 — 더 만들 수 있는 여유분은 getAvailableConnections()
 *   (= maxconnect - 보유수) 이며 이 값과 다르다 (2026-09-17 최정우 정정 — 두 함수의
 *   @brief 가 같아 구분이 안 됐다)
*/
int CPostgrePool::getPooledConnections()
{
	m_cMutex.lock();
	int nCount = m_nPooledConnections;
	m_cMutex.unlock();

	return nCount;
}

/**
 * @brief DB 연결 확인
 * @return void
*/
void CPostgrePool::keepPoolAlive()
{
	sleep(m_nTimeOut);
	while (true)
	{
		// [버그 수정, 2026-09-10 최정우] UninitializePool() 은 이 스레드를 pthread_cancel() 로
		//   깨우는데, 기본 취소 상태(PTHREAD_CANCEL_ENABLE, deferred)라 m_cMutex 를 쥔 채로
		//   pingConnection()/createConnection() 내부의 블로킹 소켓 호출(recv/send/connect, 전부
		//   POSIX 취소 지점)에서 취소가 그대로 먹힐 수 있다 — 그러면 unlock 이 실행될 기회 없이
		//   스레드가 즉시 종료돼 m_cMutex 가 영원히 잠긴 채로 남고, UninitializePool() 뒷부분의
		//   재획득 루프가 그 뮤텍스를 다시 잠그려다 영구 대기(데드락)에 빠진다 — 실제 pthread
		//   취소로 재현 확인(뮤텍스 보유 중 취소 → 이후 lock 시도가 타임아웃 없이 블록).
		//   2026-08-14 수정은 "호출측이 락 쥔 채 취소"만 막았고, "취소당하는 쪽이 락을 쥔 채
		//   취소 지점에서 죽는" 이 경우는 못 막았다. 뮤텍스를 쥐고 있는 구간에서만 취소를
		//   비활성화해, 이 구간 안에서는 취소 요청이 즉시 반영되지 않고 대기했다가 sleep()
		//   에서(취소 지점, 락 밖) 처리되게 한다.
		int nOldCancelState;
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &nOldCancelState);

		m_cMutex.lock();

		while (static_cast<int>(m_dqQueue.size()) > m_nMinConnect)
		{
			freeConnection(m_dqQueue.front());
			m_dqQueue.pop_front();
		}

		for (deque<PGconn *>::iterator it=m_dqQueue.begin(); it!=m_dqQueue.end(); )
		{
			if (!pingConnection(*it))
			{
				freeConnection(*it);
				it = m_dqQueue.erase(it);
			}
			else
				++it;
		}

		while (m_nPooledConnections < m_nMinConnect)
		{
			if (createConnection() == nullptr)
				break;
		}

		m_cMutex.unlock();

		pthread_setcancelstate(nOldCancelState, nullptr);

		sleep(m_nTimeOut);
	}
}

/**
 * @brief DB 연결 상태 확인
 * @param[in] pcHandle DB 연결 핸들 값
 * @return true(성공), false(실패)
*/
bool CPostgrePool::pingConnection(PGconn *pcHandle)
{
	string strSQL = "select 0";

	PGresult *res = PQexec(pcHandle, strSQL.c_str());
	if (!res)
	{
		LOGFMTE("postgre command failed!error=[%s]", PQerrorMessage(pcHandle));
		return false;
	}

	if ((PQresultStatus(res) == PGRES_TUPLES_OK) && PQntuples(res))
	{
		PQgetvalue(res, 0, 0);
		PQclear(res);
	}
	else
	{
		PQclear(res);
		LOGFMTE("postgre health check failed!error=[%s]", PQerrorMessage(pcHandle));
		return false;
	}

	return true;
}

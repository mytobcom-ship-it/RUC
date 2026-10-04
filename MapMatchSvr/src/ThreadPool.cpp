/**
 * @file ThreadPool.cpp
 * @brief 스레드 풀 클래스 소스 파일
*/
#include "ThreadPool.h"

/**
 * @brief 생성자
*/
CThreadPoolWorker::CThreadPoolWorker() : 
	m_bStopped(false), 
	m_nState(EWS_UNKNOWN)
{
}

/**
 * @brief 작업 쓰레드 실행
 * @param[in] nThreadId 쓰레드 아이디
 * @param[in] context 호출 클래스 포인터
 * @return void
*/
void CThreadPoolWorker::run(int nThreadId, void *context)
{
	CThreadPool *pcThreadPool = reinterpret_cast<CThreadPool *>(context);
	RAW_LOG_BATCH vtRawLog;

	// [2026-10-04 최정우 수정, 사용자 확정 — TTL 마감 권장안 (다)] 대기를 무기한(wait)에서 상한 있는
	//   대기(waitTimed)로 바꿨다. 상한 동안 일감이 없으면 **이 워커 스레드에서** Runnable::idle() 을 부른다 —
	//   RawLogWorker 는 거기서 자기 세션의 TTL 을 검사한다. 종전에는 TTL 검사가 "그 워커에 다음 배치가 올
	//   때"만 돌아, 데이터가 끊긴 단말의 트립은 재기동 전까지 마감되지 않았다. 세션은 워커 스레드 전용이라
	//   반드시 같은 스레드에서 호출해야 한다(다른 스레드에서 건드리면 TRIP_SEQ 카운터가 꼬일 수 있다).
	// 되돌리는 법: 아래 waitTimed/bIdleTick 을 종전 wait() 로 되돌린다.
	static const int MM_WORKER_IDLE_TICK_MS = 60000;
	while (!m_bStopped)
	{
		// predicate 검사 + 대기 + Dequeue 를 동일 mutex 로 보호 (lost-wakeup 방지)
		bool bIdleTick = false;
		pcThreadPool->m_paMutex[nThreadId].lock();
		while ((pcThreadPool->m_paQueues[nThreadId].Count() == 0) && !m_bStopped)
		{
			m_nState = EWS_WAITING;
			const bool bSignaled = pcThreadPool->m_paCondition[nThreadId].waitTimed(
				pcThreadPool->m_paMutex[nThreadId], MM_WORKER_IDLE_TICK_MS);
			m_nState = EWS_UNKNOWN;
			if (!bSignaled && (pcThreadPool->m_paQueues[nThreadId].Count() == 0) && !m_bStopped)
			{
				bIdleTick = true;
				break;
			}
		}

		bool bDequeued = (!bIdleTick) && pcThreadPool->m_paQueues[nThreadId].Dequeue(vtRawLog);
		pcThreadPool->m_paMutex[nThreadId].unlock();

		if (bIdleTick)
		{
			m_nState = EWS_ACTIVE;				// 종료 대기(WaitForActiveIdle)가 진행 중인 TTL 마감을 기다리도록
			pcThreadPool->m_pcRunnable->idle(nThreadId);
			m_nState = EWS_UNKNOWN;
			continue;
		}

		if (bDequeued)
		{
			m_nState = EWS_ACTIVE;
			pcThreadPool->m_pcRunnable->run(nThreadId, &vtRawLog);
			m_nState = EWS_UNKNOWN;
		}
	}

	m_nState = EWS_STOPPED;
}

/**
 * @brief 전체 Thread 정지
 * @param[in] nThreadId 쓰레드 아이디
 * @param[in] context 호출 클래스 포인터
 * @return void
*/
void CThreadPoolWorker::stop(int /* nThreadId */, void *context)	// 전체 정지라 특정 쓰레드 아이디는 쓰지 않음
{
	CThreadPool *pcThreadPool = reinterpret_cast<CThreadPool *>(context);

	m_bStopped = true;
	for (int i=0; i<pcThreadPool->m_nMaxThreads; ++i)
	{
		// 반드시 해당 워커의 뮤텍스를 잡은 채로 브로드캐스트할 것 — run()의 대기 루프는 predicate
		// 검사(Count()==0 && !m_bStopped)와 wait() 호출을 같은 뮤텍스로 묶어서 하는데, 여기서 락
		// 없이 브로드캐스트하면 "predicate 검사 통과 후 wait() 호출 직전"의 틈에 신호가 지나가버릴
		// 수 있음 — 그 신호는 대기 등록된 스레드가 없어 유실되고, 뒤늦게 불린 wait()는 더 이상 올
		// 신호가 없어 영영 안 깨어남(missed-wakeup, 셧다운 hang). 뮤텍스를 잡고 브로드캐스트하면
		// worker 는 그 순간 반드시 락 밖(진입 전이거나 wait() 안에서 대기 등록된 상태)에 있으므로
		// 신호를 놓치지 않거나, 다음 predicate 재검사에서 m_bStopped=true 를 직접 관측함
		// (2026-08-14 최정우 수정 — "소스상 문제" 검토 중 발견)
		pcThreadPool->m_paMutex[i].lock();
		pcThreadPool->m_paCondition[i].broadcast();
		pcThreadPool->m_paMutex[i].unlock();
	}
}

/**
 * @brief 생성자
 * @param[in] nMaxThreads 쓰레드 갯수
 * @param[in] pcRunnable 작업용 쓰레드 클래스
*/
CThreadPool::CThreadPool(int nMaxThreads, Runnable *pcRunnable, bool bDetatch) : 
	m_pcRunnable(pcRunnable), 
	m_paQueues(nullptr),
	m_paMutex(nullptr),
	m_paCondition(nullptr),
	m_nMaxThreads(nMaxThreads), 
	m_bDetatch(bDetatch)
{
	if (m_nMaxThreads <= 0)
		return;

	m_paQueues = new CQueue<RAW_LOG_BATCH>[m_nMaxThreads];
	m_paMutex = new CMutex[m_nMaxThreads];
	m_paCondition = new CCondition[m_nMaxThreads];

	for (int i=0; i<m_nMaxThreads; i++)
	{
		ThreadPoolContext context;

		context.worker = new CThreadPoolWorker;
		context.thread = new CThread(i, context.worker);
		m_lstThreadPool.push_back(context);

		context.thread->start(reinterpret_cast<CThreadPool *>(this));
		if (m_bDetatch) context.thread->detach();
	}
}

/**
 * @brief 소멸자
*/
CThreadPool::~CThreadPool()
{
	list<ThreadPoolContext>::iterator it;

	// [버그 수정, 2026-09-10 최정우] start() 가 m_bDetatch(기본값 true, 실 운영 설정 그대로)
	// 이면 각 워커를 즉시 detach() 시키는데, 여기서는 그 여부를 안 가리고 매번 join() 을 불렀다 —
	// detach 된 스레드를 join() 하는 건 POSIX 정의되지 않은 동작이다. 실측: pthread_join() 이
	// EINVAL 을 반환함을 확인(이 glibc 에서는 크래시로는 안 이어졌으나, libc 버전·스레드ID 재사용
	// 타이밍에 따라 다른 스레드를 join하거나 행/크래시로 이어질 수 있는 잠재적 결함). 서버가
	// 정상 종료될 때마다(=매번) 재현되는 문제였다. detach 된 경우는 join() 을 아예 안 부르고,
	// 아래 GetStoppedThreads() 폴링 루프가 이미 "스레드가 실제로 run() 을 빠져나갔는지"를
	// 대기해주므로 완료 보장은 그대로 유지된다.
	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		(*it).thread->stop();
		if (!m_bDetatch)
			(*it).thread->join();
	}

	bool bAllStopped = WaitForAllStopped(3000);

	// [버그 수정, 2026-09-11 최정우] CThread::~CThread() 는 detach 여부와 무관하게 항상
	//   `delete m_pcRunnable`(=여기서는 CThreadPoolWorker*) 을 실행한다 — 위 대기가 타임아웃돼도
	//   지금까지는 그대로 delete (*it).thread 를 강행해서, 아직 run() 루프 안에서 m_bStopped/
	//   m_nState 를 읽고 쓰는 중일 수 있는 CThreadPoolWorker 객체를 실행 중인 네이티브 스레드
	//   발밑에서 해제하는 use-after-free 였다(실측 X, 3초 타임아웃이 실제로 걸리는 상황 자체가
	//   드묾 — 코드 감사로 발견). STOPPED 확인된 워커만 delete 하고, 못 멈춘 워커는 그대로 두어
	//   해당 네이티브 스레드가 계속 안전하게 실행되게 한다(프로세스 종료 직전이라 누수는 감수).
	//   하나라도 못 멈췄으면 공유 자원(m_pcRunnable/큐/뮤텍스/컨디션 배열)도 함께 누수시킨다 —
	//   인덱스로 공유되는 자원이라 어느 워커가 아직 실행 중인지 개별적으로 가려낼 수 없다.
	if (!bAllStopped)
	{
		LOGFMTE("threadpool shutdown timeout!stopped=[%d/%d] some worker(s) still running(detached) — "
			"leaking worker/runnable/shared resources intentionally to avoid use-after-free",
			GetStoppedThreads(), (int)m_lstThreadPool.size());
	}

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).worker && (*it).worker->GetState() != EWS_STOPPED)
			continue;										// 아직 실행 중 — delete 보류(누수)

		if ((*it).thread)
		{
			delete (*it).thread;
			(*it).thread = nullptr;
		}
	}

	m_lstThreadPool.clear();

	if (!bAllStopped)
		return;												// 공유 자원은 아래에서 delete 하지 않고 그대로 둠

	if (m_pcRunnable)
	{
		delete m_pcRunnable;
		m_pcRunnable = nullptr;
	}

	if (m_paQueues)
	{
		delete [] m_paQueues;
		m_paQueues = nullptr;
	}

	if (m_paMutex)
	{
		delete [] m_paMutex;
		m_paMutex = nullptr;
	}

	if (m_paCondition)
	{
		delete [] m_paCondition;
		m_paCondition = nullptr;
	}
}

/**
 * @brief 쓰레드 갯수 구하기
 * @return 쓰레드 갯수
*/
int CThreadPool::GetMaxThreads()
{
	return static_cast<int>(m_lstThreadPool.size());
}

/**
 * @brief 워커 고정 큐에 데이터 넣기 및 해당 Thread 깨우기
 * @param[in] nThreadId 워커(큐) 인덱스
 * @param[in] vtRawLog 큐 데이터(원시 GPS 배치)
 * @return void
*/
void CThreadPool::Enqueue(int nThreadId, const RAW_LOG_BATCH &vtRawLog)
{
	if (m_paQueues == nullptr || nThreadId < 0 || nThreadId >= m_nMaxThreads)
		return;

	if (vtRawLog.empty())
		return;

	// Enqueue + 시그널 을 동일 mutex 로 보호 (lost-wakeup 방지)
	m_paMutex[nThreadId].lock();
	m_paQueues[nThreadId].Enqueue(vtRawLog);
	m_paCondition[nThreadId].signal();
	m_paMutex[nThreadId].unlock();
}

/**
 * @brief 큐 데이터 갯수 구하기
 * @param[in] nThreadId 워커(큐) 인덱스 (-1: 전체 합)
 * @return 큐 데이터 갯수
*/
int CThreadPool::GetQueueCount(int nThreadId)
{
	if (m_paQueues == nullptr)
		return 0;

	if (nThreadId >= 0 && nThreadId < m_nMaxThreads)
	{
		m_paMutex[nThreadId].lock();
		int nCount = m_paQueues[nThreadId].Count();
		m_paMutex[nThreadId].unlock();
		return nCount;
	}

	int nTotal = 0;
	for (int i=0; i<m_nMaxThreads; ++i)
	{
		m_paMutex[i].lock();
		nTotal += m_paQueues[i].Count();
		m_paMutex[i].unlock();
	}

	return nTotal;
}

/**
 * @brief 대기 중인 쓰레드 수 얻기
 * @return 대기 중인 쓰레드 수
*/
int CThreadPool::GetWaitingThreads()
{
	list<ThreadPoolContext>::iterator it;
	int nWaitingThreads = 0;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread->GetState() == ETS_RUNNING && 
			(*it).worker->GetState() == EWS_WAITING)
			nWaitingThreads++;
	}

	return nWaitingThreads;
}

/**
 * @brief 작업 중인 쓰레드 수 얻기
 * @return 작업 중인 쓰레드 수
*/
int CThreadPool::GetActiveThreads()
{
	list<ThreadPoolContext>::iterator it;
	int nActiveThreads = 0;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread->GetState() == ETS_RUNNING && 
			(*it).worker->GetState() == EWS_ACTIVE)
			nActiveThreads++;
	}

	return nActiveThreads;
}

/**
 * @brief 정지된 쓰레드 수 얻기
 * @return 정지된 쓰레드 수
*/
int CThreadPool::GetStoppedThreads()
{
	list<ThreadPoolContext>::iterator it;
	int nStoppedThreads = 0;

	// [버그 수정, 2026-09-11 최정우] GetWaitingThreads()/GetActiveThreads() 와 같은 틀로
	//   "thread->GetState()==ETS_RUNNING &&" 를 그대로 복사해 넣었었는데, WAITING/ACTIVE 와 달리
	//   STOPPED 는 worker->run()(CThreadPoolWorker::run) 맨 끝에서 딱 한 번만 세팅되고 그 직후
	//   worker->run() 이 반환하면서 CThread::run() 이 곧바로 m_nState 를 ETS_STOPPED 로 바꿔버린다
	//   (Thread.cpp) — 즉 "thread==RUNNING && worker==STOPPED" 인 순간은 그 찰나(명령어 몇 개
	//   분량)뿐이고, 그 창을 지나면 thread==RUNNING 조건 자체가 영원히 거짓이 된다. ~CThreadPool()
	//   의 폴링 루프(100ms 간격)가 이 찰나를 잡을 확률은 사실상 0이라, 워커가 즉시 끝났어도 매번
	//   3초(30회) 타임아웃을 전부 소모한 뒤에야 delete 로 넘어갔다 — 실제로 배치 처리가 3초를
	//   넘기면 아직 실행 중인(detach 된) 네이티브 스레드가 이미 delete 된 worker/thread 객체를
	//   참조하는 use-after-free 위험까지 있었다. worker->GetState()==EWS_STOPPED 는 한 번 세팅되면
	//   그대로 유지되는(단조) 값이라 이것만으로 충분하고 정확하다.
	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).worker->GetState() == EWS_STOPPED)
			nStoppedThreads++;
	}

	return nStoppedThreads;
}

/**
 * @brief 워커 스레드 종료 요청 (큐 처리 중단)
 * @return void
 * @remark #8 종료: 신규 Dequeue 중단, 진행 중 run() 은 완료 후 종료
*/
void CThreadPool::RequestShutdown()
{
	list<ThreadPoolContext>::iterator it;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread != nullptr)
			(*it).thread->stop();
	}
}

/**
 * @brief 활성 워커·큐가 비울 때까지 대기
 * @param[in] nMaxWaitMs 최대 대기 (ms)
 * @return true(유휴), false(타임아웃 시 잔여 작업 있음)
*/
bool CThreadPool::WaitForIdle(int nMaxWaitMs)
{
	if (nMaxWaitMs <= 0)
		return (GetActiveThreads() <= 0 && GetQueueCount() <= 0);

	const int nStepMs = 100;
	int nElapsedMs = 0;

	while (nElapsedMs < nMaxWaitMs)
	{
		if (GetActiveThreads() <= 0 && GetQueueCount() <= 0)
			return true;

		CThread::sleep(nStepMs);
		nElapsedMs += nStepMs;
	}

	return (GetActiveThreads() <= 0 && GetQueueCount() <= 0);
}

/**
 * @brief 진행 중(활성) 워커만 유휴 될 때까지 대기
 * @param[in] nMaxWaitMs 최대 대기 (ms)
 * @return true(활성 없음), false(타임아웃 시 진행 중 batch 잔존)
 * @remark 종료 시 RequestShutdown() 이후 큐는 워커가 Dequeue 하지 않으므로
 *         큐 비움은 WaitForIdle 이 아닌 DrainQueuedBatches 로 처리한다.
*/
bool CThreadPool::WaitForActiveIdle(int nMaxWaitMs)
{
	if (nMaxWaitMs <= 0)
		return (GetActiveThreads() <= 0);

	const int nStepMs = 100;
	int nElapsedMs = 0;

	while (nElapsedMs < nMaxWaitMs)
	{
		if (GetActiveThreads() <= 0)
			return true;

		CThread::sleep(nStepMs);
		nElapsedMs += nStepMs;
	}

	return (GetActiveThreads() <= 0);
}

/**
 * @brief 전체 워커가 run() 을 완전히 빠져나가 EWS_STOPPED 에 도달할 때까지 대기
 * @param[in] nMaxWaitMs 최대 대기 (ms)
 * @return true(전부 정지 확인), false(타임아웃 — 여전히 실행 중인 워커 존재 가능)
 * @remark [버그 수정, 2026-09-11 최정우] delete 전에 호출측이 명시적으로 확인할 수 있게 분리 —
 *         WaitForActiveIdle() 은 "현재 batch 처리 중(EWS_ACTIVE)" 만 보고 "대기 루프를 완전히
 *         빠져나와 EWS_STOPPED 로 전이" 하는 건 확인 못한다(RequestShutdown 후 run() 이 대기
 *         루프 최상단 predicate 를 재검사하는 짧은 구간).
*/
bool CThreadPool::WaitForAllStopped(int nMaxWaitMs)
{
	int nTotal = static_cast<int>(m_lstThreadPool.size());

	if (nMaxWaitMs <= 0)
		return (GetStoppedThreads() >= nTotal);

	const int nStepMs = 100;
	int nElapsedMs = 0;

	while (nElapsedMs < nMaxWaitMs)
	{
		if (GetStoppedThreads() >= nTotal)
			return true;

		CThread::sleep(nStepMs);
		nElapsedMs += nStepMs;
	}

	return (GetStoppedThreads() >= nTotal);
}

/**
 * @brief 워커 큐에 남은 batch 전량 추출 (처리·release 용)
 * @param[out] pvtBatches 잔여 batch 목록
 * @return void
 * @remark #8 종료 drain: Dequeue 만 수행, DB release 는 RawLogWorker 가 담당
*/
void CThreadPool::DrainQueuedBatches(vector<RAW_LOG_BATCH> *pvtBatches)
{
	if (pvtBatches == nullptr || m_paQueues == nullptr)
		return;

	for (int i=0; i<m_nMaxThreads; ++i)
	{
		RAW_LOG_BATCH vtBatch;

		m_paMutex[i].lock();
		while (m_paQueues[i].Dequeue(vtBatch))
		{
			if (!vtBatch.empty())
				pvtBatches->push_back(vtBatch);
		}
		m_paMutex[i].unlock();
	}
}

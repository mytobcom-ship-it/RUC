/**
 * @file SingleThread.cpp
 * @brief 싱글 쓰레드 클래스 소스 파일
*/
#include <errno.h>					// pthread_cond_timedwait 의 ETIMEDOUT (2026-09-15 최정우 추가)
#include "SingleThread.h"

// [2026-09-21 최정우 추가] 시그널 핸들러가 "자기 스레드의" 인터럽트 플래그를 세울 수 있게
//   하는 스레드 지역 포인터. 핸들러는 static 멤버라 어느 인스턴스의 시그널인지 알 방법이
//   없는데, SIGUSR1 은 pthread_kill 로 특정 스레드에만 전달되므로 그 스레드의 지역 변수로
//   대상을 특정할 수 있다. threadHandler() 진입 시 1회 설정한다.
static __thread CSingleThread *s_pcCurrentThread = nullptr;

long CSingleThread::m_nId = 0;
pthread_attr_t CSingleThread::m_attr;
long CSingleThread::m_nAttrRefCount = 0;
pthread_mutex_t CSingleThread::m_staticMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief 쓰레드 핸들러
 * @param[in] pParam 쓰레드 전달 포인터
 * @return nullptr
*/
void *CSingleThread::threadHandler(void *pParam)
{
	CSingleThread *pcThread = reinterpret_cast<CSingleThread *>(pParam);
	// 핸들러가 이 스레드의 플래그를 세울 수 있도록 먼저 등록한다 (2026-09-21 최정우 추가)
	s_pcCurrentThread = pcThread;
	signal(SIGUSR1, interruptHandler);
	// [2026-09-21 최정우] try/catch 는 남겨둔다 — run() 본문이 던지는 예외까지 잡아 스레드가
	//   조용히 죽는 대신 인터럽트로 기록되게 하던 기존 계약을 그대로 유지하기 위함이다.
	//   다만 interruptHandler() 가 더 이상 throw 하지 않으므로, 이 catch 로 들어오는 경로는
	//   "run() 안에서 명시적으로 던진 경우" 뿐이다(현재 그런 코드는 없다).
	try
	{
		pcThread->run();
	}
	catch (InterruptedException& e)
	{
		pcThread->m_bIsInterrupted = 1;
	}

	// [버그 수정, 2026-09-15 최정우] 종료 통지를 **뮤텍스 안에서 상태 전이와 함께, 무조건** 한다.
	//   종전 코드는 (a) m_bJoinning 을 락 **밖에서** 읽고 false 면 broadcast 를 건너뛰었고,
	//   (b) ESS_STOPED 로 전이하는 코드가 전 소스에 아예 없었다(grep: 비교문 1곳뿐). 그래서
	//   "워커가 여기까지 와서 m_bJoinning==false 를 보고 종료 → 그 직후 main 이 join() 진입"
	//   순서가 되면, join() 은 m_nState 가 여전히 ESS_RUNNING 이라 대기에 들어가는데 깨워줄
	//   스레드는 이미 없어 **영원히 블록**됐다. 이 스레드들은 PTHREAD_CREATE_DETACHED 라
	//   pthread_join 이라는 대안도 없다(join() 은 순전히 조건변수 핸드셰이크).
	//   실제 영향: Server::Uninitialize() 가 fetcher join 에서 멈춰 그 뒤의 워커 드레인
	//   (PROCESSING→PENDING 반납·트립종료 플러시)이 통째로 실행되지 않고, kill_svr.sh 가
	//   35초 뒤 SIGKILL 한다.
	// [버그 수정, 2026-09-15 최정우] m_nId = -1 대입 제거 — m_nId 는 Initialize() 가
	//   m_staticMutex 아래에서 "최초 인스턴스인가"(m_nId==0)를 판정해 pthread_attr_init 을
	//   한 번만 부르게 하는 가드 겸 카운터다. 여기서 락 없이 -1 로 만들면 다음 인스턴스가
	//   -1→0 이 되고, 그 다음 인스턴스가 m_nId==0 을 보고 **이미 초기화된 attr 을 재초기화**한다.
	//   종료하는 스레드가 생성 측 카운터를 건드릴 이유 자체가 없다.
	pthread_mutex_lock(&pcThread->m_mutex);
	pcThread->m_nState = static_cast<int>(ESS_STOPED);
	pthread_cond_broadcast(&pcThread->m_cond);
	pthread_mutex_unlock(&pcThread->m_mutex);

	return nullptr;
}

/**
 * @brief 시그널 핸들러
 * @param[in] sig 시그널
 * @return void
 * @remark [2026-09-21 최정우 — 아래 @warning 의 지적대로 재설계 완료]
 *   이 핸들러는 이제 **플래그(m_bIsInterrupted)만 세운다.** 실제 종료는 실행 스레드가
 *   IsInterrupted() 를 폴링해 스스로 처리한다(RawLogFetcher·Server 에 그 구조가 이미 있다).
 *
 * @warning [2026-09-17 진단 → 2026-09-21 수정 완료] 종전에는 여기서 곧바로 C++ 예외를 던졌다.
 *   **비동기 시그널 핸들러 안에서 예외를 던지는 것은 정의되지 않은 동작**이다 — 시그널은 임의의
 *   명령어 경계에서 끼어들 수 있어 그 지점이 스택 언와인딩을 견딘다는 보장이 없고, 핸들러 밖으로
 *   예외가 새면 std::terminate 로 직행할 수도 있다.
 *   **2026-09-17 진단에는 한 가지 오류가 있었다** — "interrupt() 호출부가 없으니 실행되지 않는
 *   죽은 경로" 라고 적었는데, 그렇지 않다. threadHandler() 가 스레드마다 이 함수를 SIGUSR1
 *   핸들러로 **항상 등록**하므로, 내부에서 interrupt() 를 부르지 않아도 **외부에서 SIGUSR1 을
 *   한 번 보내면(kill -USR1 <pid>) 그대로 발동**한다. 즉 죽은 코드가 아니라 상시 노출된
 *   경로였다. "호출부가 0" 과 "도달 불가" 는 다르다.
 *   현재 상태: interrupt()(pthread_kill(SIGUSR1))의 호출부는 여전히 0 건이므로(Server.cpp 의
 *   해당 줄은 주석 처리) **정상 동작에는 변화가 없고**, 외부 SIGUSR1 을 받았을 때 죽지 않고
 *   안전하게 무시하게 된 것만 달라졌다.
*/
void CSingleThread::interruptHandler(int /* sig */)		// 시그널 번호와 무관하게 동일 처리
{
	// [버그 수정, 2026-09-21 최정우] 종전에는 여기서 곧바로 C++ 예외를 던졌다 — **시그널 핸들러
	//   안에서의 throw 는 정의되지 않은 동작**이다(핸들러는 비동기 시그널 안전 함수만 호출할 수
	//   있고, 예외 전파에 필요한 언와인딩 자체가 그 범주 밖이다). 게다가 이 함수는 "죽은 코드"가
	//   아니다 — threadHandler() 가 스레드마다 SIGUSR1 핸들러로 **항상 등록**하므로, 내부에서
	//   interrupt() 를 안 부르더라도 외부에서 SIGUSR1 이 한 번 들어오면 그 스레드에서 그대로
	//   발동해 프로세스가 비정상 종료될 수 있었다(kill -USR1 <pid> 만으로 재현 가능한 경로).
	//   위 @warning 이 적어둔 "정석" 대로, 여기서는 플래그만 세우고 실제 종료는 실행 스레드가
	//   IsInterrupted() 를 폴링해 스스로 처리한다. 그 폴링 구조는 RawLogFetcher·Server 에 이미
	//   있다. m_bIsInterrupted 를 volatile sig_atomic_t 로 바꾼 것도 같은 이유다 — 핸들러에서
	//   안전하게 쓸 수 있다고 표준이 보장하는 유일한 타입이다.
	//   동작 변화: interrupt() 호출부가 전 소스에 0 건이라 정상 경로에는 영향이 없다. 달라지는
	//   것은 외부에서 SIGUSR1 을 받았을 때뿐이며, 그때 죽지 않고 안전하게 무시하게 된다.
	CSingleThread *pcThread = s_pcCurrentThread;
	if (pcThread != nullptr)
		pcThread->m_bIsInterrupted = 1;
}

/**
 * @brief 생성자
*/
CSingleThread::CSingleThread()
{
	char name[32];
	sprintf(name, "Thread%ld", m_nId);
	Initialize(name);
}

/**
 * @brief 생성자
 * @param[in] name 쓰레드 이름
*/
CSingleThread::CSingleThread(const string& name)
{
	Initialize(name);
}

/**
 * @brief 소멸자
*/
CSingleThread::~CSingleThread()
{
	// [버그 수정, 2026-09-10 최정우] m_attr 은 static(전 인스턴스 공유)인데 원래 여기서 가드 없이
	// 매번 destroy 했다 — 이 클래스를 파생하는 인스턴스가 2개 이상(CServer/CRawLogFetcher)이면
	// 정상 종료 때마다 이미 파괴된 attr 을 또 destroy 하는 정의되지 않은 동작이었다(실측: 이
	// glibc 에서는 두 번째 destroy도 조용히 성공 반환해 당장 크래시로는 안 이어졌으나, 표준
	// 위반이라 향후 libc 변경이나 이 attr 에 동적 자원을 쓰는 API가 추가되면 실제 이중 해제로
	// 터질 수 있는 구조적 결함). m_nId 는 threadHandler() 에서 다른 용도(-1 리셋)로 이미 쓰이고
	// 있어 재활용하면 위험해, 이 카운터만 별도로 둬서 마지막 살아있는 인스턴스가 소멸할 때만
	// destroy 하도록 고친다.
	// [정정] "m_nId 는 threadHandler() 에서 -1 리셋으로 쓰인다" 는 2026-09-15 에 그 대입이 제거돼 더
	//   이상 사실이 아니다(threadHandler() 주석 참고). 별도 카운터를 두는 결정은 그대로 유효하다
	//   (2026-10-06 최정우 주석 수정)
	// [버그 수정, 2026-09-11 최정우] 위 카운터 자체가 static(전 인스턴스 공유)인데 여기선 아예
	// 락 없이 감소시키고 있었다 — 두 인스턴스가 동시에 소멸하면 감소 자체가 레이스(카운트 유실 →
	// pthread_attr_destroy 누락 또는 다른 인스턴스가 pthread_create(..., &m_attr, ...) 호출 중에
	// 조기 destroy 되는 UB). m_staticMutex 로 감소~destroy 를 원자적으로 묶는다.
	pthread_mutex_lock(&m_staticMutex);
	if (--m_nAttrRefCount <= 0)
		pthread_attr_destroy(&m_attr);
	pthread_mutex_unlock(&m_staticMutex);

	pthread_mutex_destroy(&m_mutex);
	pthread_cond_destroy(&m_cond);
}

/**
 * @brief 초기화
 * @param[in] name 쓰레드 이름
 * @return void
*/
void CSingleThread::Initialize(const string& name)
{
	m_name = name;
	m_bJoinning = false;
	m_bIsInterrupted = 0;
	pthread_mutex_init(&m_mutex, nullptr);
	pthread_cond_init(&m_cond, nullptr);
	m_nState = static_cast<int>(ESS_INITIAL);

	// [버그 수정, 2026-09-11 최정우] m_nId/m_attr/m_nAttrRefCount 는 static(전 인스턴스 공유)인데
	//   원래 여기서 "이 인스턴스" 전용 m_mutex(막 pthread_mutex_init 한 것)로 잠갔다 — 서로 다른
	//   인스턴스는 서로 다른 m_mutex 를 쓰므로 두 인스턴스가 동시에 Initialize() 를 타면 이 락은
	//   static 상태를 전혀 보호하지 못한다(생성 시점이 겹칠 일이 드물어 지금까지 증상은 없었지만
	//   보호 자체가 안 되는 구조였음). static 상태 전용 m_staticMutex 로 교체.
	pthread_mutex_lock(&m_staticMutex);
	if (m_nId == 0)
	{
		pthread_attr_init(&m_attr);
		pthread_attr_setdetachstate(&m_attr, PTHREAD_CREATE_DETACHED);
	}
	m_nId++;
	m_nAttrRefCount++;			// (2026-09-10 최정우 추가) — 소멸자의 destroy-once 가드 카운터
	pthread_mutex_unlock(&m_staticMutex);
}

/**
 * @brief 쓰레드 시작
 * @return void
 * @exception IllegalThreadStateException 이미 시작됨(ESS_RUNNING)·이미 종료됨(ESS_STOPED),
 *   그리고 **pthread_create 실패**(2026-09-15 추가 — 상태를 ESS_INITIAL 로 되돌린 뒤 던지므로
 *   뒤이은 join() 은 즉시 통과한다. 종전에는 ESS_RUNNING 인 채 조용히 return 해 join() 이
 *   영구 블록됐다)
*/
void CSingleThread::start()
{
	if (m_nState == static_cast<int>(ESS_RUNNING))
	{
		throw IllegalThreadStateException("thread already started!");
	}
	else if (m_nState == static_cast<int>(ESS_INITIAL))
	{
		m_nState = static_cast<int>(ESS_RUNNING);
		if (pthread_create(&m_thread, &m_attr, threadHandler, this) != 0)
		{
			// [버그 수정, 2026-09-15 최정우] 생성 실패인데 상태를 ESS_RUNNING 인 채로 두고
			//   조용히 return 했다 — 그러면 아무도 ESS_STOPED 로 바꿔줄 스레드가 없어
			//   뒤이은 join() 이 **100% 영구 블록**된다(AppMain.cpp:666 pcServer->join()).
			//   상태를 되돌려 join() 이 즉시 통과하게 하고, 예외로 실패를 알린다 —
			//   호출측 2곳(AppMain:665, Server.cpp:697)은 이미 try/catch 범위 안에 있고
			//   이 클래스는 다른 실패도 예외로 알리는 규약이다(IllegalThreadStateException).
			//   [정정] 위 행 번호는 바뀌었다 — main() 의 pcServer->start()/join() 과
			//   CServer::Initialize() 끝의 m_pcRawLogFetcher->start() 를 가리킨다 (2026-10-06 최정우 주석 수정)
			m_nState = static_cast<int>(ESS_INITIAL);
			throw IllegalThreadStateException("thread create failed!");
		}
	}
	else if (m_nState == static_cast<int>(ESS_STOPED))
	{
		throw IllegalThreadStateException("thread has been started!");
	}
}

/**
 * @brief 쓰레드 종료
 * @return void
*/
void CSingleThread::join()
{
	// [버그 수정, 2026-09-15 최정우] 술어(predicate) 루프로 교체. 종전은 (a) 상태를 락 밖에서
	//   한 번 보고 (b) pthread_cond_wait 을 단발 호출했다 — spurious wakeup 이 한 번만 나도
	//   **스레드가 아직 살아있는데 join() 이 반환**한다. 호출측은 반환 즉시 해제를 시작하므로
	//   (Server.cpp:731 join → 732 delete m_pcRawLogFetcher) 그 스레드가 아직 쓰고 있는 객체를
	//   지우는 use-after-free 가 된다. 상태 검사·대기를 같은 뮤텍스 안에서 루프로 묶는다.
	//   [정정] 행 번호는 바뀌었다 — CServer::Uninitialize() 의 m_pcRawLogFetcher->join() 직후
	//   delete 를 가리킨다 (2026-10-06 최정우 주석 수정)
	pthread_mutex_lock(&m_mutex);
	m_bJoinning = true;
	while (m_nState == static_cast<int>(ESS_RUNNING))
		pthread_cond_wait(&m_cond, &m_mutex);
	m_bJoinning = false;
	pthread_mutex_unlock(&m_mutex);
}

/**
 * @brief 입력시간 후 쓰레드 종료
 * @param[in] time 밀리세컨드
 * @return void
*/
void CSingleThread::join(unsigned long time)
{
	struct timeval now;
	struct timespec timeout;

	gettimeofday(&now, nullptr);
	ldiv_t t = ldiv(time * 1000000, 1000000000);
	timeout.tv_sec = now.tv_sec + t.quot;
	timeout.tv_nsec = now.tv_usec * 1000 + t.rem;
	// tv_nsec 정규화 — 위 덧셈이 1초를 넘길 수 있는데 넘긴 채로 넘기면 pthread_cond_timedwait 이
	//   EINVAL 로 즉시 반환해 대기가 통째로 무력화된다 (2026-09-15 최정우 추가)
	if (timeout.tv_nsec >= 1000000000L)
	{
		timeout.tv_sec += timeout.tv_nsec / 1000000000L;
		timeout.tv_nsec %= 1000000000L;
	}

	// [버그 수정, 2026-09-15 최정우] 무인자 join() 과 동일 근거로 술어 루프 — 단, 이쪽은 타임아웃이
	//   정상 종료 조건이므로 ETIMEDOUT 이면 스레드가 살아있어도 빠져나온다(호출측이 IsAlive() 로
	//   판별). spurious wakeup 만 걸러내는 것이 목적이다.
	pthread_mutex_lock(&m_mutex);
	m_bJoinning = true;
	while (m_nState == static_cast<int>(ESS_RUNNING))
	{
		if (pthread_cond_timedwait(&m_cond, &m_mutex, &timeout) == ETIMEDOUT)
			break;
	}
	m_bJoinning = false;
	pthread_mutex_unlock(&m_mutex);
}

/**
 * @brief 인터럽트 쓰레드
 * @return void
 * @remark 대상 스레드에 SIGUSR1 을 보낼 뿐이며, 핸들러(interruptHandler)는 플래그만 세운다 — 대기 중인
 *   스레드를 깨우지는 못한다(조건변수 대기는 WakeUp()/RequestShutdown() 이 담당). 현재 호출부 없음
 *   (2026-10-06 최정우 주석 추가)
*/
void CSingleThread::interrupt()
{
	pthread_kill(m_thread, SIGUSR1);
}

/**
 * @brief 인터럽트 여부
 * @return true(성공), false(실패)
 *   [정정] true=인터럽트 플래그가 세워짐(SIGUSR1 수신 또는 run() 이 InterruptedException 을 던짐),
 *   false=세워지지 않음 (2026-10-06 최정우 주석 수정)
*/
bool CSingleThread::IsInterrupted()
{
	return (m_bIsInterrupted != 0);
}

/**
 * @brief 쓰레드가 실행 중인지 여부
 * @return true(성공), false(실패)
 *   [정정] true=ESS_RUNNING(start 이후 run() 종료 전), false=시작 전·종료됨 (2026-10-06 최정우 주석 수정)
*/
bool CSingleThread::IsAlive()
{
	// [버그 수정, 2026-09-15 최정우] m_nState 는 이제 종료 스레드가 m_mutex 아래에서 쓰므로
	//   여기서도 같은 뮤텍스로 읽는다(종전엔 락 없이 읽어 형식상 데이터 레이스였다).
	pthread_mutex_lock(&m_mutex);
	const bool bAlive = (m_nState == static_cast<int>(ESS_RUNNING));
	pthread_mutex_unlock(&m_mutex);
	return bAlive;
}

/**
 * @brief 쓰레드 이름 구하기
 * @return 쓰레드 이름
*/
const string& CSingleThread::getName()
{
	return m_name;
}

/**
 * @brief 쓰레드 이름 설정
 * @param[in] name 쓰레드 이름
 * @return void
*/
void CSingleThread::setName(const string& name)
{
	m_name = name;
}

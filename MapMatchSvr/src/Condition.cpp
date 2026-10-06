/**
 * @file Condition.cpp
 * @brief Thread 접근 제어용 클래스 소스 파일
*/
#include "Condition.h"
#include <errno.h>
#include <time.h>

/**
 * @brief 생성자
*/
CCondition::CCondition()
{
	pthread_cond_init(&m_condition, nullptr);
}

/**
 * @brief 소멸자
*/
CCondition::~CCondition()
{
	pthread_cond_destroy(&m_condition);
}

/**
 * @brief 대기 중인 모든 Thread 깨우기
 * @return void
*/
void CCondition::broadcast()
{
	pthread_cond_broadcast(&m_condition);
}

/**
 * @brief 대기 중인 하나의 Thread 깨우기
 * @return void
*/
void CCondition::signal()
{
	pthread_cond_signal(&m_condition);
}

/**
 * @brief 동작 중인 Thread 잠시 중단
 * @param[in] mutex 동기화용 Mutex
 * @return void
*/
void CCondition::wait(CMutex& mutex)
{
	pthread_cond_wait(&m_condition, &mutex.m_mutex);
}

/**
 * @brief 동작 중인 Thread 를 최대 nWaitMs 밀리초 대기 (시그널 시 즉시 반환)
 * @param[in] mutex 동기화용 Mutex (호출 전 잠금 상태)
 * @param[in] nWaitMs 최대 대기 시간 (ms)
 * @return true(시그널을 받아 깨어남), false(타임아웃 **또는** 오류)
 * @remark
 *   · 기준 시계는 CLOCK_REALTIME 이다 — pthread_cond_timedwait 의 기본값이며, 조건변수에
 *     pthread_condattr_setclock() 을 하지 않았으므로 이것이 맞다. 다만 시스템 시각이 NTP 등으로
 *     점프하면 실제 대기 시간이 늘거나 줄 수 있다. 같은 이유로 CClock 은 2026-09-10 에
 *     CLOCK_MONOTONIC 으로 바꿨지만(그쪽은 경과시간 측정이라 단조 시계가 필수),
 *     여기는 "깨우기 신호를 기다리는 상한" 이라 점프의 영향이 다음 주기에 흡수된다.
 *   · 반환값이 false 라고 반드시 타임아웃인 것은 아니다(EINVAL 등도 false). 현재 호출부
 *     (CRawLogFetcher::run, CServer::WaitForNextCycle)는 둘 다 깨어난 뒤 종료 플래그를 다시
 *     확인하므로 구분할 필요가 없다 — 구분이 필요해지면 errno 가 아니라 이 반환 코드를 나눠야 한다.
 *   (2026-09-21 최정우 주석 보완)
 *   [정정] 호출부는 이제 셋이다 — 2026-10-04 에 CThreadPoolWorker::run() 이 추가됐고, 그쪽은
 *   반환값(false)을 "유휴 tick"(→ Runnable::idle() TTL 검사) 판정에 쓴다. 오류로 인한 false 도
 *   유휴 tick 으로 취급되지만, 큐가 비어 있고 정지 요청이 없을 때만 idle() 을 부르므로 동작상
 *   문제는 없다 (2026-10-06 최정우 주석 수정)
*/
bool CCondition::waitTimed(CMutex& mutex, int nWaitMs)
{
	if (nWaitMs <= 0)
		return false;

	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return false;

	ts.tv_sec += nWaitMs / 1000;
	ts.tv_nsec += static_cast<long>(nWaitMs % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}

	int nRet = pthread_cond_timedwait(&m_condition, &mutex.m_mutex, &ts);
	return (nRet == 0);
}

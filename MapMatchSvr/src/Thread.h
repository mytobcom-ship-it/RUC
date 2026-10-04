/**
 * @file Thread.h
 * @brief Thread 클래스 헤더 파일
*/
#ifndef __THREAD_H__
#define __THREAD_H__

#include <unistd.h>
#include <pthread.h>
#include "TypeDefine.h"

/**
 * @class Runnable
 * @brief 작업용 쓰레드에서 상속 받을 인터페이스 클래스
*/
class Runnable
{
public:
	Runnable() {}
	virtual ~Runnable() {}
	virtual void run(int nThreadId, void *context = nullptr) = 0;
	// 기본 구현은 아무것도 하지 않으므로 인자를 쓰지 않는다. 인터페이스 문서용으로 이름은
	// 남겨두되 주석 처리해 -Wunused-parameter 를 피한다(재정의하는 쪽에서는 실제로 사용).
	virtual void stop(int /* nThreadId */, void * /* context */ = nullptr) {}
	// 워커가 일감 없이 일정 시간(CThreadPoolWorker 의 대기 상한) 기다렸을 때 그 워커 스레드에서 호출된다.
	//   기본 구현은 아무것도 하지 않는다 (2026-10-04 최정우 추가 — RawLogWorker 의 주기 TTL 검사용)
	virtual void idle(int /* nThreadId */) {}
	static uint32 GetThreadHandle();
};

/**
 * @enum ETHREAD_STATE
 * @brief Thread 상태
*/
enum ETHREAD_STATE
{
	ETS_CREATED						= 0,								// Thread 가 생성된 상태
	ETS_RUNNING,														// Thread 가 실행중인 상태
	ETS_STOPPED															// Thread 가 멈춘 상태
};

/**
 * @class CThread
 * @brief 쓰레드 클래스
*/
class CThread
{
public:
	CThread(int nThreadId = 0);
	CThread(int nThreadId, Runnable *pcRunnable);
	virtual ~CThread();
	uint32 GetThreadHandle();
	int GetThreadId();
	bool start(void *context = nullptr);
	virtual void run();
	virtual void stop();
	static void sleep(long millis);
	void detach();
	void join();
	enum ETHREAD_STATE GetState();

protected:
	pthread_t					m_hHandle;
	enum ETHREAD_STATE			m_nState;
	int							m_nThreadId;
	Runnable					*m_pcRunnable;
	void						*m_context;
};

#endif	//__THREAD_H__

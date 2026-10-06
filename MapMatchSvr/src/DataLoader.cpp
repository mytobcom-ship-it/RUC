/**
 * @file DataLoader.cpp
 * @brief 세그먼트 및 링크 정보 로딩 클래스 소스 파일
*/
#include "DataLoader.h"

/**
 * @brief 생성자
*/
CDataLoader::CDataLoader() :
	m_bLoad(false), 
	m_nMaxStep(0), 
	m_dfHopPenalty(MM_HOP_PENALTY), 
	m_dfHopLenRatio(MM_HOP_LEN_RATIO), 
	m_dwGridInfoSize(0), 
	m_dwGridSgmtInfoSize(0), 
	m_dwLinkSgmtInfoSize(0), 
	m_dwLinkInfoSize(0), 
	m_dwTurnInfoSize(0), 
	m_dwGridInfoStartOffset(0), 
	m_dwGridSgmtInfoStartOffset(0), 
	m_dwLinkSgmtInfoStartOffset(0), 
	m_dwLinkInfoStartOffset(0), 
	m_dwTurnInfoStartOffset(0), 
	m_dwGridInfoCount(0), 
	m_dwGridSgmtInfoCount(0), 
	m_dwLinkSgmtInfoCount(0), 
	m_dwLinkInfoCount(0), 
	m_dwTurnInfoCount(0), 
	m_pstDataFileHead(nullptr), 
	m_pstGridInfoList(nullptr), 
	m_pstGridSgmtInfoList(nullptr), 
	m_pstLinkSgmtInfoList(nullptr), 
	m_mapLinkInfoList(nullptr), 
	m_pstTurnInfoList(nullptr)
{
}

/**
 * @brief 소멸자
*/
CDataLoader::~CDataLoader()
{
	Uninitialize();
}

/**
 * @brief 초기화
 * @param[in] strDataFile 데이터 바이너리 파일명
 * @param[in] nMaxStep 연속 맵매칭 최대 검색 단계 설정
 * @return void
*/
void CDataLoader::Initialize(string& strDataFile, const sint16 nMaxStep)
{
	m_strDataFile = strDataFile;
	m_nMaxStep = nMaxStep;			// 연속 맵매칭 최대 검색 단계
}

/**
 * @brief 메모리 삭제
 * @return void
*/
void CDataLoader::Uninitialize()
{
	SetDataInit();
	m_bLoad = false;
}

/**
 * @brief 데이터 업데이트
 * @return true(성공), false(실패)
 * @remark link.psf(m_strDataFile) 를 읽어 GRID·세그먼트·링크·회전정보를 메모리에 전량 적재한다.
 *   현재 호출부는 CServer::Initialize() 기동 1회뿐이며 적재 후에는 읽기 전용이라 조회 함수에 락이 없다.
 *   헤더 크기 검증 실패는 기존 데이터를 보존하지만, SetDataInit() 이후 실패는 기존 데이터까지 비운다
 *   (2026-10-06 최정우 주석 추가)
*/
bool CDataLoader::SetDataUpdate()
{
	// 기동 시 지도 데이터 로딩 시작 지점 — 어느 파일을 읽기 시작하는지 먼저 남겨야 아래
	//   SetDataInfoDisplay() 요약(끝난 뒤 결과만 표시)과 짝을 이뤄 로딩 시작~완료 구간이
	//   로그만으로 구분된다 (2026-09-04 최정우 추가, 사용자 지시)
	LOGFMTI("map data load start!file=[%s] maxstep=[%d]", m_strDataFile.c_str(), m_nMaxStep);

	if (access(m_strDataFile.c_str(), F_OK) != 0)
	{
		LOGFMTE("data binary file not found!file=[%s]", m_strDataFile.c_str());
		return false;
	}

	FILE *fp = fopen(m_strDataFile.c_str(), "rb");
	if (fp == nullptr)
	{
		LOGFMTE("data binary file open failed!");
		return false;
	}

	m_pstDataFileHead = new (std::nothrow)DATA_FILE_HEAD;
	if (m_pstDataFileHead == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("head memory allocate failed!");
		return false;
	}
	// fread 반환값(실제 읽은 바이트 수)을 반드시 확인할 것 — 확인 안 하면 파일이 잘렸거나 디스크
	//   오류로 읽기가 짧게 끝나도 아무 로그·실패 없이 남은 버퍼가 초기화 안 된 값(new(nothrow)라
	//   생성자 없는 POD 타입은 쓰레기값)인 채로 그대로 쓰임(2026-08-14 최정우 수정 — "소스상 문제"
	//   검토 중 발견)
	if (fread(m_pstDataFileHead, 1, DATA_FILE_HEAD_SIZE, fp) != DATA_FILE_HEAD_SIZE)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		LOGFMTE("data file head read failed!short read or file truncated");
		return false;
	}

	// 데이터 크기 검증 — 헤더가 선언한 "개수 x 레코드크기" 와 "총 바이트수" 가 어긋나면 파일이
	//   깨진 것이므로 적재하지 않는다. 이 5개 분기는 아래 SetDataInit() **이전**이라 기존에
	//   적재돼 있던 도로망은 그대로 살아 있다 — 그래서 m_bLoad 는 일부러 건드리지 않는다.
	// [버그 수정, 2026-09-21 최정우] 다만 반환값이 `return m_bLoad;` 였다. 실패 경로인데
	//   "직전 적재 성공 여부"를 돌려주는 셈이라, 재적재(reload) 기능이 생기는 순간 호출측이
	//   **깨진 파일을 읽었는데도 성공으로 오인**한다. 현재는 SetDataUpdate() 호출부가
	//   CServer::Initialize() 기동 1회뿐이라 m_bLoad 가 항상 false 여서 발현하지 않았다
	//   (즉 동작 변화 없음). 의미대로 false 를 명시한다.
	if ((m_pstDataFileHead->dwGridInfoCount * GRID_INFO_SIZE) != m_pstDataFileHead->dwGridInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("grid info size is not same!");
		return false;
	}

	if ((m_pstDataFileHead->dwGridSgmtInfoCount * GRID_SGMT_INFO_SIZE) != m_pstDataFileHead->dwGridSgmtInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("grid segment info size is not same!");
		return false;
	}
	
	if ((m_pstDataFileHead->dwLinkSgmtInfoCount * LINK_SGMT_INFO_SIZE) != m_pstDataFileHead->dwLinkSgmtInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("link segment info size is not same!");
		return false;
	}
	
	if ((m_pstDataFileHead->dwLinkInfoCount * LINK_INFO_DATA_SIZE) != m_pstDataFileHead->dwLinkInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("link info size is not same!");
		return false;
	}

	if ((m_pstDataFileHead->dwTurnInfoCount * TURN_INFO_SIZE) != m_pstDataFileHead->dwTurnInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("turn info size is not same!");
		return false;
	}

	// 데이터 초기화 — 이 아래(new/fread)에서 실패하면 기존에 로딩돼있던 데이터도 이미 지워진
	//   뒤라 복구할 방법이 없다(ChargeDataLoader처럼 "새 데이터 다 만들고 나서 스왑"하는 구조가
	//   아님). 구조 자체를 바꾸는 대신, 최소한 그 심각성이 로그만 보고도 바로 드러나도록 아래
	//   각 실패분기마다 "road network data now empty!" 경고를 추가한다 (2026-09-11 최정우 추가,
	//   사용자 지시)
	SetDataInit();

	m_dwGridInfoSize = m_pstDataFileHead->dwGridInfoSize;
	m_dwGridInfoStartOffset = m_pstDataFileHead->dwGridInfoStartOffset;
	m_dwGridInfoCount = m_pstDataFileHead->dwGridInfoCount;

	m_dwGridSgmtInfoSize = m_pstDataFileHead->dwGridSgmtInfoSize;
	m_dwGridSgmtInfoStartOffset = m_pstDataFileHead->dwGridSgmtInfoStartOffset;
	m_dwGridSgmtInfoCount = m_pstDataFileHead->dwGridSgmtInfoCount;

	m_dwLinkSgmtInfoSize = m_pstDataFileHead->dwLinkSgmtInfoSize;
	m_dwLinkSgmtInfoStartOffset = m_pstDataFileHead->dwLinkSgmtInfoStartOffset;
	m_dwLinkSgmtInfoCount = m_pstDataFileHead->dwLinkSgmtInfoCount;
 
	m_dwLinkInfoSize = m_pstDataFileHead->dwLinkInfoSize;
	m_dwLinkInfoStartOffset = m_pstDataFileHead->dwLinkInfoStartOffset;
	m_dwLinkInfoCount = m_pstDataFileHead->dwLinkInfoCount;

	m_dwTurnInfoSize = m_pstDataFileHead->dwTurnInfoSize;
	m_dwTurnInfoStartOffset = m_pstDataFileHead->dwTurnInfoStartOffset;
	m_dwTurnInfoCount = m_pstDataFileHead->dwTurnInfoCount;
	
	if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
	m_pstDataFileHead = nullptr;

	// 그리드별 세그먼트 범위 로딩 메모리
	LOGFMTT("grid info read start!");
	m_pstGridInfoList = new (std::nothrow)GRID_INFO[m_dwGridInfoCount];
	if (m_pstGridInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("grid info memory allocate failed!");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}

	fseek(fp, m_dwGridInfoStartOffset, SEEK_SET);
	if (fread(m_pstGridInfoList, 1, m_dwGridInfoSize, fp) != m_dwGridInfoSize)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("grid info read failed!short read or file truncated");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	// [2026-09-21 최정우 정정] "grid info read end!" 로그가 종전에는 메모리 할당 직후·fread
	//   **이전**에 찍혀 있었다 — 아래 다른 4개 블록(grid segment/link segment/link data/turn)은
	//   전부 읽기를 마친 뒤에 end 를 찍는데 여기만 어긋나, 로그만 보면 실제로는 아직 안 읽은
	//   상태인데 다 읽은 것으로 보였다. 짧은 읽기로 죽는 위치를 로그로 좁힐 때 오도된다.
	LOGFMTT("grid info read end!");

	// 그리드별 세그먼트 정보 로딩 메모리
	LOGFMTT("grid segment info read start!");
	m_pstGridSgmtInfoList = new (std::nothrow)GRID_SGMT_INFO[m_dwGridSgmtInfoCount];
	if (m_pstGridSgmtInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("grid segment info memory allocate failed!");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	fseek(fp, m_dwGridSgmtInfoStartOffset, SEEK_SET);
	if (fread(m_pstGridSgmtInfoList, 1, m_dwGridSgmtInfoSize, fp) != m_dwGridSgmtInfoSize)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("grid segment info read failed!short read or file truncated");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	LOGFMTT("grid segment info read end!");

	// 링크별 세그먼트 정보 로딩 메모리
	LOGFMTT("link segment info read start!");
	m_pstLinkSgmtInfoList = new (std::nothrow)LINK_SGMT_INFO[m_dwLinkSgmtInfoCount];
	if (m_pstLinkSgmtInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link segment info memory allocate failed!");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	fseek(fp, m_dwLinkSgmtInfoStartOffset, SEEK_SET);
	if (fread(m_pstLinkSgmtInfoList, 1, m_dwLinkSgmtInfoSize, fp) != m_dwLinkSgmtInfoSize)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link segment info read failed!short read or file truncated");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	LOGFMTT("link segment info read end!");

	// 세그먼트별 링크 정보
	LOGFMTT("link data information read start!");
	PLINK_INFO_DATA pstLinkInfoData = new (std::nothrow)LINK_INFO_DATA[m_dwLinkInfoCount];
	if (pstLinkInfoData == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link data info memory allocate failed!");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	fseek(fp, m_dwLinkInfoStartOffset, SEEK_SET);
	if (fread(pstLinkInfoData, 1, m_dwLinkInfoSize, fp) != m_dwLinkInfoSize)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		if (pstLinkInfoData != nullptr) delete [] pstLinkInfoData;
		pstLinkInfoData = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link data info read failed!short read or file truncated");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	LOGFMTT("link data information read end!");

	m_mapLinkInfoList = new (std::nothrow)mapLinkInfo;
	if (m_mapLinkInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		if (pstLinkInfoData != nullptr) delete [] pstLinkInfoData;
		pstLinkInfoData = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link data info memory allocate failed!");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}

	LOGFMTT("link information insert start!");
	for (uint32 i=0; i<m_dwLinkInfoCount; ++i)
	{
		LINK_INFO stLinkInfo;

		// init
		memset(reinterpret_cast<void *>(&stLinkInfo), 0, LINK_INFO_SIZE);

		uint64 qwLinkID = pstLinkInfoData[i].qwLinkID;
		memcpy(reinterpret_cast<void *>(&stLinkInfo), reinterpret_cast<const void *>(&pstLinkInfoData[i].dwSgmtOffset), LINK_INFO_SIZE);

		m_mapLinkInfoList->insert(pair<uint64, LINK_INFO>(qwLinkID, stLinkInfo));
	}
	LOGFMTT("link information insert end!");

	if (pstLinkInfoData != nullptr) delete [] pstLinkInfoData;
	pstLinkInfoData = nullptr;

	// 시작 링크 기준으로 연결된 링크 회전 정보 로딩 메모리
	LOGFMTT("turn info read start!");
	m_pstTurnInfoList = new (std::nothrow)TURN_INFO[m_dwTurnInfoCount];
	if (m_pstTurnInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("turn info memory allocate failed!");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	fseek(fp, m_dwTurnInfoStartOffset, SEEK_SET);
	if (fread(m_pstTurnInfoList, 1, m_dwTurnInfoSize, fp) != m_dwTurnInfoSize)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("turn info read failed!short read or file truncated");
		LOGFMTE("road network data now empty!prior loaded data already destroyed at SetDataInit() before this failure, map-matching unavailable until next successful reload");
		return false;
	}
	LOGFMTT("turn info read end!");

	if (fp != nullptr) fclose(fp);
	fp = nullptr;

	m_bLoad = true;
	return true;
}

/**
 * @brief 데이터 정보 출력
 * @return void
*/
void CDataLoader::SetDataInfoDisplay()
{
	LOGFMTI("-----------------------------------------------------------------");
	LOGFMTI("GRID_INFO : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwGridInfoCount, static_cast<uint16>(GRID_INFO_SIZE), m_dwGridInfoSize);
	LOGFMTI("SGMT_INFO_GRID : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwGridSgmtInfoCount, static_cast<uint16>(GRID_SGMT_INFO_SIZE), m_dwGridSgmtInfoSize);
	LOGFMTI("SGMT_INFO_LINK : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwLinkSgmtInfoCount, static_cast<uint16>(LINK_SGMT_INFO_SIZE), m_dwLinkSgmtInfoSize);
	LOGFMTI("LINK_INFO : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwLinkInfoCount, static_cast<uint16>(LINK_INFO_DATA_SIZE), m_dwLinkInfoSize);
	LOGFMTI("TURN_INFO : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwTurnInfoCount, static_cast<uint16>(TURN_INFO_SIZE), m_dwTurnInfoSize);
	LOGFMTI("-----------------------------------------------------------------");
}

/**
 * @brief 데이터 초기화
 * @return void
*/
void CDataLoader::SetDataInit()
{
	// 데이터 byte 크기 초기화
	m_dwGridInfoSize = 0; 
	m_dwGridSgmtInfoSize = 0;
	m_dwLinkSgmtInfoSize = 0;
	m_dwLinkInfoSize = 0;
	m_dwTurnInfoSize = 0;

	// 데이터 시작 위치
	m_dwGridInfoStartOffset = 0;
	m_dwGridSgmtInfoStartOffset = 0;
	m_dwLinkSgmtInfoStartOffset = 0;
	m_dwLinkInfoStartOffset = 0;
	m_dwTurnInfoStartOffset = 0;

	// 데이터 개수 초기화
	m_dwGridInfoCount = 0;
	m_dwGridSgmtInfoCount = 0;
	m_dwLinkSgmtInfoCount = 0;
	m_dwLinkInfoCount = 0;
	m_dwTurnInfoCount = 0;

	// 데이터 삭제
	if (m_pstGridInfoList != nullptr) delete [] m_pstGridInfoList;
	if (m_pstGridSgmtInfoList != nullptr) delete [] m_pstGridSgmtInfoList;
	if (m_pstLinkSgmtInfoList != nullptr) delete [] m_pstLinkSgmtInfoList;
	if (m_mapLinkInfoList != nullptr)
	{
		m_mapLinkInfoList->clear();
		delete m_mapLinkInfoList;
	}
	if (m_pstTurnInfoList != nullptr) delete [] m_pstTurnInfoList;

	m_pstGridInfoList = nullptr;
	m_pstGridSgmtInfoList = nullptr;
	m_pstLinkSgmtInfoList = nullptr;
	m_mapLinkInfoList = nullptr;
	m_pstTurnInfoList = nullptr;
}

/**
 * @brief GRID 별 세그먼트 범위 구하기
 * @param[in] dwGridID GRID ID
 * @return GRID 별 세그먼트 범위 정보
*/
PGRID_INFO CDataLoader::GetGridInfo(const uint32 dwGridID)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (m_dwGridInfoCount <= 0)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (dwGridID >= m_dwGridInfoCount)
		return nullptr;

	return &m_pstGridInfoList[dwGridID];
}

/**
 * @brief GRID 별 세그먼트 정보 구하기
 * @param[in] dwOffset 세그먼트 Offset
 * @return GRID 별 세그먼트 정보
*/
PGRID_SGMT_INFO CDataLoader::GetGridSgmtInfo(const uint32 dwOffset)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (dwOffset >= m_dwGridSgmtInfoCount)
		return nullptr;

	return &m_pstGridSgmtInfoList[dwOffset];
}

/**
 * @brief 세그먼트별 링크 정보 구하기
 * @param[in] dwOffset 세그먼트 Offset
 * @return 세그먼트별 링크 정보
*/
PLINK_SGMT_INFO CDataLoader::GetLinkSgmtInfo(const uint32 dwOffset)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (dwOffset >= m_dwLinkSgmtInfoCount)
		return nullptr;

	return &m_pstLinkSgmtInfoList[dwOffset];
}

/**
 * @brief 링크 ID 로 링크 정보 구하기 (세그먼트 단위는 GetLinkSgmtInfo())
 * @param[in] qwLinkID 링크 ID
 * @return 링크 정보
*/
PLINK_INFO CDataLoader::GetLinkInfo(const uint64 qwLinkID)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if ((qwLinkID <= 0 ) || (m_dwLinkInfoCount <= 0))
		return nullptr;

	mapLinkInfo::iterator it = m_mapLinkInfoList->find(qwLinkID);
	if (it == m_mapLinkInfoList->end())
		return nullptr;

	return &it->second;
}

/**
 * @brief 시작 링크 기준으로 연결된 링크 회전 정보 구하기
 * @param[in] dwOffset 회전정보 Offset
 * @return 시작 링크 기준으로 연결된 링크 회전 정보
*/
PTURN_INFO CDataLoader::GetTurnInfo(const uint32 dwOffset)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	// [경고 정리, 2026-09-15 최정우] 하한 검사(< 0)는 dwOffset 이 uint32(부호 없음)라 절대
	//   성립하지 않는다(-Wtype-limits). 상한 검사만 남긴다 — 인자를 부호 있는 타입으로 바꾸게
	//   되면 하한 검사를 다시 넣을 것.
	if (dwOffset >= m_dwTurnInfoCount)
		return nullptr;

	return &m_pstTurnInfoList[dwOffset];
}

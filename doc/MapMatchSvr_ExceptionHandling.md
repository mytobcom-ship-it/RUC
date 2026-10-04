# MapMatchSvr 예외 처리·경계 케이스 목록

> 기준: `MapMatchSvr` v1.3 파이프라인 (`rawgps_recover` / `rawgps_select` / `rawgps_update`)  
> 최종 갱신: 2026-07-08

---

## 처리 흐름 (참고)

```
PENDING(0) → [rawgps_select reserve] → PROCESSING(2)
  → Worker 맵매칭 → [rawgps_update] → MATCHED(1) / SKIP(3) / ERROR(4)
실패·종료 → release(0) → PENDING (런타임) / rawgps_recover (기동 시)
```

---

## 🔴 High (1~8)

| # | 이슈 | 위치 | 상태 | 비고 |
|---|------|------|------|------|
| 1 | `RECV_DT` 기준 중복 재예약 | `query.sql` `rawgps_select` | ✅ Fixed | PENDING-only reserve + 기동 `rawgps_recover` |
| 2 | Bulk fail → PROCESSING 좀비 | `RawLogWorker::BulkUpdateRawLogs` | ✅ Fixed | `rawgps_update` `$4=0` release |
| 3 | Session bulk 전 갱신 | `RawLogWorker::ProcessRawLog` / `run()` | ✅ Fixed | 배치 임시 세션 → bulk 성공 후 commit |
| 4 | Parse fail → 전체 batch orphan | `RawLogFetcher::ReserveFetchBatch` | ✅ Fixed | 실패 행 skip + release, Worker orphan release |
| 5 | `PQcmdTuples` 미검증 | `BulkUpdateRawLogs`, `ReleaseReservedRows` | ✅ Fixed | `expected == affected` 검증 |
| 6 | TTL sweep (`dtLastSeen`) 없음 | `RawLogWorker::ExpireTtlSessions` (워커 스레드 `run()`) | ✅ Fixed | `[worker] ttl_sec`, 워커 self-sweep(레이스 제거) |
| 7 | Worker 조기 exit → reserve 잔류 | `RawLogWorker::run()` | ✅ Fixed | `ReleaseReservedBatch()` |
| 8 | Shutdown no drain | `Server::Uninitialize`, `ThreadPool` | ✅ Fixed | `shutdown_wait` + 큐 release |

---

## 🟡 Medium (9~16)

### #9 END + ERROR/SKIP 시 세션 미삭제 — ✅ Fixed

| 항목 | 내용 |
|------|------|
| **위치** | `RawLogWorker::ProcessRawLog()` |
| **문제** | TRIP END(2) 시 MATCHED일 때만 `pbTripEnded` → 세션 erase |
| **영향** | END + ERROR/SKIP 후 `qwLinkID`/`dwLastGpsSeq` 메모리 잔류 |
| **조치** | `TRIP_EVENT=END`이면 MATCHED/ERROR/SKIP 무관 `pbTripEnded=true` |

---

### #10 `charge_insert` 로드만, 미호출 — ⏸ 보류

| 항목 | 내용 |
|------|------|
| **위치** | `Server.cpp`, `RawLogWorker`, `query.sql` `[charge_insert]` |
| **현재** | SQL 정의·config 로드만 존재, Worker INSERT 호출 없음 |
| **보류 사유** | `CHARGE_TARGET` 테이블 재설계 후 INSERT·위반탐지 연동 예정 |
| **조치 예정** | 재설계 완료 후 `charge_insert` SQL 수정 + `RawLogWorker` 배선 |

---

### #11 `tripseq_max` 문서만 있고 SQL·배선 없음 — ✅ Fixed

| 항목 | 내용 |
|------|------|
| **위치** | `query.sql` 헤더 |
| **조치** | `config.ini`·`AppMain` 에서 tripseq_max 이미 제거됨. 잔여 `query.sql` 헤더 흐름 주석(운행순번 채번) 삭제 |
| **배경** | v1.3: `TRIP_ID`는 수집서버 START 시 적재 — 위치검증서버 채번 불필요 |
| **결과** | `[tripseq_max]` SQL·config 키·C++ 호출 없음 + 헤더 주석 제거로 v1.3 정합 완료 |

---

### #12 LIMIT이 trip_id 중간에서 분할 — Pending

| 항목 | 내용 |
|------|------|
| **위치** | `rawgps_select` (`LIMIT $1`), `GroupByTripId()` |
| **현재** | `ORDER BY device_key, trip_id, gps_dt, gps_seq` 후 **행 단위** LIMIT |
| **시나리오** | trip A GPS 800건, LIMIT=500 → 1~500만 예약, 501~800은 다음 poll |
| **영향** | trip batch·세션은 유지되나 한 trip이 여러 poll에 분산, 지연 가능 |
| **대안 A** | trip 단위 LIMIT (서브쿼리) |
| **대안 B** | 분할 허용 운영 문서화 + LIMIT 튜닝 |

---

### #13 DB pool `maxconnect` vs 워커 수 — 부분

| 항목 | 내용 |
|------|------|
| **위치** | `config.ini` `[database]`, `AppMain.cpp` |
| **필요 conn** | 워커 N + Feeder 1 (동시 최대 N+1) |
| **자동 보정** | `maxconnect=0` → `threads+1` 이상 (`AppMain`) |
| **잔여 리스크** | `maxconnect` 수동 과소 설정 시 `getConnection()` null → #7 연계 |
| **대안** | `maxconnect >= threads + 2` 권장 또는 초기화 fail-fast |

---

### #14 Worker connection 반환 시 ROLLBACK 없음 — ✅ Fixed

| 항목 | 내용 |
|------|------|
| **위치** | `RawLogWorker::ReleaseConnection()` |
| **조치** | Fetcher와 동일 `PQtransactionStatus` → `ROLLBACK` 후 pool 반환 |

---

### #15 recover 실패 시 WARN만, 기동 계속 — ✅ Fixed

| 항목 | 내용 |
|------|------|
| **위치** | `Server::Initialize()` → `CRawLogFetcher::RunRecover()` |
| **조치** | `RECOVER_RETRY_MAX`(3회) 재시도 후 실패 시 기동 중단(fail-fast) |

---

### #16 반복 실패 시 영구 ERROR / DLQ 없음 — ✅ Fixed (인메모리)

| 항목 | 내용 |
|------|------|
| **위치** | `RawLogWorker::BulkReleaseRawLogs()` |
| **조치** | config `[worker] retry_max` (기본 5). release 횟수 초과 시 `MATCH_STATUS=ERROR(4)` 고정 |
| **한계** | 재시도 카운트는 프로세스 메모리 — 재기동 시 초기화 (DB 컬럼 없음) |

---

## 🟢 Low (17~20)

### #17 실시간 경로 GPS heading만 사용 — Pending

| 항목 | 내용 |
|------|------|
| **위치** | `ProcessManager::buildMapMatchInput()` |
| **현재** | `nAngle = stRawLogInfo.nAngle` (단일 GPS heading). `heading < 0`이면 `NO_ANGLE` |
| **없는 것** | 직전 GPS~현재 GPS **궤적 기반 방위각** 계산 |
| **영향** | heading 노이즈·`-1` 시 Begin 맵매칭 정확도 저하 가능 |
| **대안** | 세션에 직전 좌표 저장 → `atan2` 궤적 heading 보조 |

---

### #18 ERROR 시 `wErrorCode` DB 미저장 — Pending

| 항목 | 내용 |
|------|------|
| **위치** | `ProcessManager::ProcessRawLog()`, `RawLogWorker::RunMapMatch()` |
| **현재** | 맵매칭 실패 시 `MATCH_STATUS=ERROR(4)`만 DB 반영. `wErrorCode`는 로그만 |
| **없는 것** | `raw_gps_log` 에러 코드·메시지 컬럼 갱신 |
| **영향** | 운영·분석 시 실패 원인 추적 어려움 |
| **대안** | 스키마에 `match_error_code` 등 추가 또는 별도 로그 테이블 |

---

### #19 큐 상한 없음 (poll만 backpressure) — Pending

| 항목 | 내용 |
|------|------|
| **위치** | `RawLogFetcher::run()`, `CThreadPool::Enqueue()` |
| **현재** | `queue_pause_count` 이상 시 **DB 조회 중단**만. 큐 자체 max 없음 |
| **시나리오** | burst 적재 시 워커 큐·batch 메모리 증가 |
| **영향** | 극단 부하 시 OOM·지연 증폭 |
| **대안** | `queue_max_batches` 설정 + 초과 시 poll 중단 강화 또는 drop 정책 |

---

### #20 장시간 사용 connection keepalive 미검증 — Pending

| 항목 | 내용 |
|------|------|
| **위치** | `PostgrePool` (pool 관리 스레드 `pingConnection`) |
| **현재** | pool **idle 큐**에 대해 주기 `select 0` ping. **active(대여 중)** conn은 미검증 |
| **시나리오** | 장시간 bulk·맵매칭 중 TCP/DB 세션 끊김 |
| **영향** | `PQexecParams` 실패 → release·재시도 (#2/#7)로 완화되나 빈도 증가 가능 |
| **대안** | 대여 시간·`PQstatus` 검사, 실패 시 conn 폐기 후 재연결 |

---

## 상태 요약

| 구분 | 건수 | 상태 |
|------|------|------|
| 🔴 High | 8 | ✅ 전건 Fixed |
| 🟡 Medium | 8 (#9~#16) | #9·#11·#14·#15·#16 Fixed, #10 보류, #12~#13 Pending |
| 🟢 Low | 4 (#17~#20) | Pending |

---

## 권장 대응 순서 (잔여)

1. **#12** — LIMIT trip 분할 (필요 시)
2. **#13** — maxconnect 검증 강화
3. **#10** — CHARGE_TARGET 재설계 후 INSERT (별도 일정)
4. **Low 17~20** — 운영 요구에 따라 순차 검토

### GIS 정확도 (2026-07-10 반영)

| ID | 항목 | 상태 |
|----|------|------|
| C-1 | `GridBorderDistance` ↔ `nRadius` 단위 정합 (m) | ✅ Fixed |
| C-2 | `SgmtMatch` 종점 snap 보완 | ✅ Fixed |

### Worker 예외 (2026-07-10 반영)

| ID | 항목 | 상태 |
|----|------|------|
| E-1 | Worker 조기 return 시 conn 재시도 + `ReleaseReservedBatch` | ✅ Fixed |
| E-2 | `retry_max` + ERROR 고정 | ✅ Fixed |

---

## 관련 설정 (`config.ini`)

```ini
[feeder]
limit=500                    # rawgps_select $1 — #12 와 연관
fetch_interval=500
queue_pause_count=400
queue_max_count=800
queue_busy_min=2000
queue_busy_max=10000

[worker]
ttl_sec=3600                 # #6 TTL sweep
shutdown_wait=30000          # #8 shutdown drain
retry_max=5                  # #16 release 재시도 상한

[database]
maxconnect=0                 # 0 → threads+1 자동 (#13)

[sql]
rawlog_recover=rawgps_recover
rawlog_select=rawgps_select
rawlog_update=rawgps_update
charge_insert=charge_insert  # #10: 보류
```

---

## 관련 소스

| 파일 | 역할 |
|------|------|
| `MapMatchSvr/bin/query.sql` | SQL 정의 |
| `MapMatchSvr/src/RawLogFetcher.cpp` | poll·reserve·parse release |
| `MapMatchSvr/src/RawLogWorker.cpp` | 맵매칭·bulk·release·세션 |
| `MapMatchSvr/src/Server.cpp` | 기동·recover·monitor·shutdown |
| `MapMatchSvr/src/ThreadPool.cpp` | 워커 큐·drain |
| `MapMatchSvr/src/ProcessManager.cpp` | 맵매칭 입력·실행 |

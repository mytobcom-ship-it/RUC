# RUC 맵매칭 GPS 웹 뷰어

`prim_rawgps` GPS·맵매칭 결과와 주변 `network.moct_link` 도로망을 지도에 표시합니다.

## 실행

```bash
cd web
chmod +x run.sh
sudo ./run.sh    # [web] port=80 — 1024 미만 포트는 root 필요
```

- VM 로컬: http://localhost/
- Windows (NAT **80→80** 포트포워딩): **http://127.0.0.1/**

## 설정 (`config.ini`)

| 키 | 기본 | 설명 |
|----|------|------|
| `port` | **80** | 웹 서버 포트 (게스트 VM). 호스트 포워딩 80→80 과 맞출 것 |
| `road_buffer_m` | **1000** | trip GPS bbox 주변 도로 로드 버퍼(m) |
| `poll_sec` | 5 | 점 자동 갱신(초). 체크박스와 연동 |
| `[database]` | MapMatchSvr 동일 | PostgreSQL 접속 |

## 색상

- 도로: 남색 (`moct_link`, bbox+버퍼)
- GPS: 빨강
- MATCHED: 파랑
- SKIP(좌표 있음): 주황

## 비고

- 전체 페이지 리로드 없음. **레이어 갱신**만 수행.
- GRID 세그먼트(바이너리) 레이어는 추후 추가 예정.

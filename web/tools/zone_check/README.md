# 과금구역 링크·좌표 점검 도구

`ruc.base_roadlink` 의 `link_ids`(링크 연결)와 `coords`(구역 좌표)를 주행 방향 기준으로
교차 검증해 지도 문서를 만든다.

## 생성

```bash
./build.sh            # 문서 재생성 (타일 캐시 재사용)
./build.sh --tiles    # OSM 타일까지 새로 받기 (첫 실행·구역 변경 시)
```

산출물은 `web/docs/` 에 만들어진다.

| 파일 | 용도 |
|---|---|
| `zone-link-map.html` | 사내 조회용 (Leaflet·타일을 CDN 에서 로드) |
| `zone-link-map-offline.html` | 외부 전달용 단독 파일 (약 5.6MB, 인터넷 불필요) |

**DB 를 고쳐도 문서는 자동으로 바뀌지 않는다.** 생성 시점의 스냅샷이므로 반드시 다시 돌려야 한다.

## 구성

| 파일 | 하는 일 |
|---|---|
| `zonechk2.py` | LINE 구역 — 링크 체인 연결·누락 링크 BFS 탐색·coords 방향 검증 → `work/zonedata.json` |
| `poly.py` | POLY 구역 — 폴리곤 유효성·노드 내부 판정·좌표 혼입 점검 → `work/polydata.json` |
| `gettiles.py` | 각 지도 범위의 OSM 타일 수집 (z15·z16·z17) → `work/tiles/` |
| `genhtml2.py` | 온라인 문서 생성 |
| `genoffline.py` | Leaflet(`vendor/`)·타일을 인라인해 단독 파일 생성 |

## analysis/

일회성 조사에 쓴 스크립트. 문서 생성에는 필요 없다.

| 파일 | 하는 일 |
|---|---|
| `detour.py` | 우회 매칭(A→B→C 가 A→C 보다 hop 이 크게 늘어남) 탐지 |
| `quality.py` | 매칭 경로의 물리적 타당성(연결/역행/단절) 측정 |
| `hopchk.py` · `combo.py` | 링크 전이별 hop 분포, maxstep·확장조건 조합별 커버율 |
| `unreach.py` | 도달 불가 전이의 원인 분류 |
| `buf.py` | 구역 버퍼값별 G·M 범위 일치율 |
| `z14.py` · `z1fix.py` · `z8.py` · `swap.py` | 특정 구역 개별 조사 |
| `runtest.sh` | maxstep 값을 바꿔 재매칭하고 결과를 뽑는 실행기 |

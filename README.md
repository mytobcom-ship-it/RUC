# RUC (Road Usage Charge)

맵매칭 서버, 시뮬레이터, 도로망 바이너리 생성기, SQL 스키마.

## 구성

| 디렉터리 | 설명 |
|----------|------|
| `MapMatchSvr/` | GPS raw log poll → 맵매칭 → DB 갱신 |
| `Simulator/` | 테스트용 GPS 생성·DB 적재 |
| `CreateData/` | Shapefile → `link.psf` 바이너리 생성 |
| `roadnet/sql/` | PostgreSQL DDL·매칭 테이블 |

## Git에 포함되지 않는 것

- `**/config.ini` — DB 비밀번호. `config.ini.example`을 복사해 작성
- `*.psf`, `CreateData/data/*.shp` — 대용량 도로망 (약 800MB+). 별도 복사 또는 CreateData로 재생성
- 빌드 산출물 (`MapMatchSvr`, `MakeBinary` 등)

## 다른 PC에서 시작

```bash
git clone https://github.com/mytobcom-ship-it/ruc.git
cd ruc

# config
cp MapMatchSvr/bin/config.ini.example MapMatchSvr/bin/config.ini
cp Simulator/bin/config.ini.example Simulator/bin/config.ini
# config.ini 에 DB host / password 수정

# link.psf: 기존 서버에서 복사하거나 CreateData 로 생성 후
# MapMatchSvr/bin/link.psf 에 배치

# MapMatchSvr 빌드
cd MapMatchSvr/src && make && cd ../bin
./run_svr.sh
```

## Cursor / IDE

동일 Cursor 계정으로 clone 후 해당 폴더를 Open Folder 하면 됩니다. 채팅 기록은 저장소와 별개이며, Rules/Skills는 Cursor 계정 설정을 따릅니다.

### IDE에서 Git·폴더 트리 보기

1. **File → Open Workspace from File…** → `ruc.code-workspace` (하위 프로젝트별 폴더 트리)
2. 또는 **File → Open Folder…** → `ruc` (전체 트리)
3. **Source Control** (`Ctrl+Shift+G`) → 뷰를 **Tree** 로 (설정: `scm.defaultViewMode: tree`)
4. 확장 **GitLens**, **Git Graph** 설치 권장 → Cursor가 `.vscode/extensions.json` 기준 설치 안내
5. **Git Graph** 명령 팔레트 (`Ctrl+Shift+P`) → `Git Graph: View Git Graph` → 커밋·브랜치 트리

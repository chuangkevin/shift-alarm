# AI 接手開發

這份文件讓新的 AI 在只收到「請確認 alarm-shift 後接手開發」時，可以自行辨認專案、確認現況並安全接續。正式儲存庫名稱是 `shift-alarm`；`alarm-shift` 是使用者可能使用的別名。

## 1. 系統資料流

```text
手機 ──同一 Wi-Fi / HTTP 80──> ESP32
                                  │
                                  └──原生 Tailscale──> 私有後端 :8237 ──> New API / Gemini
```

- 手機不必加入 Tailnet。ESP32 必須自行加入 Tailnet，才能使用 AI 圖片辨識與 OTA。
- 手動月曆、鬧鐘、時鐘、顯示設定都存在裝置，Tailscale 或後端不通時仍可操作與響鈴。
- `https://alarm.sisihome.org` 由 GN100 Caddy 代理第一台 ESP32 的 Tailnet port 80，根路徑轉 `/calendar`；裝置離線時改由後端顯示唯讀離線頁。裝置區網 IP 也開同一個 `/calendar`，兩者必須是同一份 ESP32 頁面。

## 2. 現況基準（2026-09-15）

### 版本

| 項目 | 版本 | 狀態 |
|---|---|---|
| 韌體原始碼 `firmware-next/` | 0.3.17 | 已建置，**尚未安裝到任何裝置** |
| 第一台裝置 | 0.3.8 | 離線中（`shiftalarm-01fc`） |
| 第二台裝置 | 0.3.16 | 離線中（`shiftalarm-9ca8`） |
| 主要後端 `:8237` | 0.1.5 | 已部署 |
| 第二台專用後端 `:8238` | 0.1.6 | 已部署（隔離環境） |
| GN100 Caddy 離線 fallback | — | 已部署 |

### 分支

- 本次整理前，local／remote `main` 的共同基準為 `dbbb36a`。接手時必須用 `git fetch`、`git rev-parse HEAD` 與 `git rev-parse origin/main` 重新確認，不要把這個雜湊當成永久現況。
- `fix/tailnet-ui-latency` 的四個功能／研究 commit 已合併進 `main`；遠端分支指標仍停在 `79a306b`，不是待合併工作，不要切回該分支繼續開發。

### 裝置

以下在線狀態是 2026-09-15 的查詢快照，會隨網路改變；接手後先向 Headscale／Tailscale API 重查 `online` 與 `lastSeen`。

- 第一台：`shiftalarm-01fc`，Tailnet `100.90.212.116`，目前離線；2026-09-15 05:44（UTC+8）後未再上線。`alarm.sisihome.org` 代理的是這一台，瀏覽器目前會看到後端離線頁。
- 第二台：`shiftalarm-9ca8`，USB MAC `fc:01:2c:c9:9c:a8`，Tailnet `100.104.66.47`。
  - 已保存兩組 Wi-Fi（`Mark`、`PETER-2.4G`），重開仍為 saved mode。
  - 目前離線；Tailscale 最後上線時間為 2026-09-15 20:50（UTC+8），後端最後收到心跳為 20:52:43，回報區網 IP `192.168.9.54`。離線前最後已知為 `alarmCount=0`、電池 100 %、未充電。
  - 最後一次在線時接手機熱點，Tailscale 走 DERP relay（`direct connection not established`），小請求要 12–40 秒。
  - 畫面「已保存 N 組」只代表 NVS 內有設定，不代表目前掃描得到該 SSID。0.3.16 關閉 Arduino 內建 `AutoReconnect`，由自訂掃描／逐組嘗試／退避流程重連；目前沒有把 disconnect reason 保存到後端，裝置離線後無法遠端區分掃描不到、密碼拒絕或基地台相容性問題。

### 後端與代理

- 兩台後端都在 `rpi-matrix`（`100.126.226.79`）。
  - 主要：`/home/kevin/DockerCompose/shift-alarm`，容器 `shift-alarm`，映像 `shift-alarm:0.1.5`，port 8237。
  - 第二台專用隔離環境：`/home/kevin/DockerCompose/shift-alarm-test-fc012cc99ca8`（`source/`、`build/`、`data/`、`private-build-*`、`.env`），容器 `shift-alarm-test-fc012cc99ca8`，映像 `shift-alarm:0.1.6`，port 8238。
  - 隔離環境使用獨立 `DEVICE_TOKEN` 與獨立資料目錄，不可與主要環境互換。
- GN100（`100.127.82.47`）跑 Caddy，設定來自 `homelab-docs/infra/caddy/`。

## 3. 最優先工作：OTA 為什麼一直失敗

完整研究在 [docs/ota-research.md](ota-research.md)。摘要：

**已定位的程式缺陷**：`main.cpp:980` 的主迴圈每約 10 ms 執行 `alarm_ota_maintenance()`。它在傳輸途中重跑安全檢查，任何一項瞬間不成立就 `discard_transfer()`（`alarm_ota.c:454`、`61`），中止 OTA、進度歸零；下一次寫入拿到 `ESP_ERR_INVALID_STATE` 而中止。第一台的時間點與鬧鐘 quiet window 相符；第二台當次究竟是哪一項護欄瞬斷仍缺序列證據，充電訊號抖動只是最可能推論。

**安全檢查條件**：時鐘、班表就緒、充電中、未響鈴、未貪睡，以及下一次鬧鐘不在 300 秒內。

**被誤導的地方**：`main.cpp:765,768` 把任何非充電中斷的失敗都寫成「更新寫入或安全檢查失敗」＋`lastReason=resume-failed`。所以 `resume-failed`、`reconnectCount=0`、`lastHttpStatus=200` 都不是續傳問題。先前調後端 chunk 大小與節奏的方向是錯的。

**對得上既有現象**：第一台有 15 個鬧鐘，300 秒 quiet window 必然被打到，所以四次失敗停在 `1,309,111`／`356,671`／`760,496`／`358,736` 等不同位置。第二台沒有鬧鐘，最可能是滿電時 GPIO38 充電訊號瞬斷（推論，待序列紀錄確認）。

**另一個獨立缺陷**：`backend_poll_policy.h:43` 讓 OTA 在輪詢進行時一律拒絕。實測 51 次取樣只有 3 次 `canDownload=true`，慢速線路上 OTA 幾乎永遠打不開。另外 `alarm_ota.c:55` 的 `lock()` 是 0 逾時，短暫競用就會讓該次寫入失敗。

**後端已排除**：完整 1,648,432 bytes 本機下載 0.49 秒，`Range: bytes=4096-` 正確回 `206`。

### 修正清單（都需要新的韌體）

1. 護欄失敗改為暫停並等待條件恢復，不要丟棄整個傳輸。
2. 充電訊號去彈跳：連續數秒都非充電才視為拔電。
3. 原因碼細分：寫入失敗、狀態失效、時鐘不可信、鬧鐘接近、充電中斷要分開。
4. OTA 進行時暫停後端輪詢，而不是用 `backend-poll-busy` 拒絕。
5. 寫入路徑的 `lock()` 改為有界等待，不要 0 逾時。
6. 600 秒傳輸期限在量到實際吞吐後再決定是否調整。

### 驗證計畫（一次 USB 就夠）

1. USB 寫入帶診斷的版本，序列輸出每一項護欄結果。
2. 故意設一個 4 分鐘後的鬧鐘，確認傳輸是暫停恢復而不是中止。
3. 滿電狀態跑一次 OTA，確認充電抖動不會中止傳輸。
4. 之後量裝置到後端的實際吞吐，決定 1.6 MB 是否可能。

## 4. 目前裝置端尚未驗證的功能

以下都只有原始碼、host 測試與建置證據，沒有實機驗收：

- GPIO38 充電判定與 fail-closed 行為
- 240×240 主畫面、六項選單、三鍵短按／長按／配網 chord、15 秒返回
- 四種大型 QR（配網、設定、`/calendar`、`/update`）在四個方向的可掃描性
- 兩組 Wi-Fi 的實際 failover、管理頁新增／移除
- staged 映像跨重開保存、部分下載重開從 0、離線安裝
- 新版 OTA 的完整下載／安裝

## 5. 接手後立即執行

```sh
git status --short --branch
git remote -v
git fetch origin
git log --oneline --decorate -5
git rev-list --left-right --count HEAD...origin/main
python3 tools/audit_public_repo.py

if [ ! -x .venv312/bin/python ]; then
  if command -v uv >/dev/null; then
    uv venv --python 3.12 .venv312
  else
    python3.12 -m venv .venv312
  fi
fi
if command -v uv >/dev/null; then
  uv pip install --python .venv312/bin/python -r requirements.txt
else
  .venv312/bin/python -m pip install -r requirements.txt
fi
.venv312/bin/python -m pytest -q
.venv312/bin/python -m unittest discover -s deploy/tests -v
sh firmware-next/components/alarm_ota/tests/run_host_tests.sh
python3 firmware-next/components/alarm_ota/tests/test_manifest_contract.py
node --check static/app.js
node firmware-next/tests/wifi_ui_contract_test.js
```

若已載入 ESP-IDF 5.3.2，再執行：

```sh
idf.py -C firmware-next build
idf.py -C firmware-next size
```

先讀取所有命令結果。若工作樹已有變更，保留並判斷來源；不可 `reset --hard`、`clean -fd` 或用遠端覆蓋。若遠端領先，先理解差異後以 fast-forward 或 rebase 整合。測試或建置失敗時，不發布韌體。

## 6. 私有建置與 USB 寫入

- 私有映像在隔離環境的 `source/` 內建置；`firmware-next/main/provisioning.h` 只存在該處與正式環境，不進 Git。
- 建置指令（在 `rpi-matrix`）：

```sh
docker run --rm --user 1000:1000 -e HOME=/tmp/idf-home \
  -v <隔離環境>/source:/project -v <隔離環境>/build:/build -w /project \
  espressif/idf:v5.3.2 bash -lc '. /opt/esp/idf/export.sh >/dev/null && idf.py -C firmware-next -B /build build'
```

- 發布到裝置可抓的 manifest：`python3 deploy/publish_firmware.py <app.bin> --release-dir <data>/releases`。
- USB 寫入（Mac）：裝置為 `/dev/cu.usbmodem12201`。寫入前先 `read-mac` 確認是 `fc:01:2c:c9:9c:a8`，只寫 `0x10000` app0，不碰 bootloader、partition table、NVS、`alarm_nvs`。
- 刷寫前先重新備份整顆 16 MB flash。先前的備份在 `/var/folders/.../T/opencode/shift-alarm-fc012cc99ca8-backup.bin`，屬暫存目錄，可能已被清除。

## 7. 私密檔案邊界

下列檔案可能存在於開發機，但已被 Git 排除：

- `.env`
- `firmware-next/main/provisioning.h`
- `firmware/src/provisioning.h`
- `data/`
- `firmware-next/build/`
- `firmware/.pio/`

隔離測試環境的 `.env`、`source/firmware-next/main/provisioning.h`、`private-build-*/` 同樣視為私密，不可複製回儲存庫或輸出內容。

不可讀出或貼出其中秘密來「確認設定」。只確認檔案是否存在、權限是否正確及 Git 是否忽略。公開儲存庫只能保存 `provisioning.example.h` 的假值。新增設定時，同步更新 `.gitignore`、範本和這份清單。

## 8. 開發與交付定義

每一項變更至少要做到：

1. 保留上面的資料流與離線能力，新增的使用者文字為繁體中文。
2. 加入能證明行為的必要測試；不要用只複製實作內容的測試湊數。
3. 執行相關 host 測試與完整後端測試；韌體改動完成 ESP-IDF 建置。
4. 提高韌體版本，且只把無憑證建置視為公開產物。實機映像保持私有。
5. 若使用者要求部署，嚴格依 `docs/更新部署.md` 與 `docs/Tailscale-OTA.md`；先預檢，再一次安裝，最後驗收保存資料與實體功能。
6. 更新 README、PLAN 與部署紀錄，提交後推送；確認 `HEAD`、`origin/main` 與 GitHub 公開頁面的 commit 一致。

## 9. 回報原則

- 只把已實測的結果寫成「已完成」。程式完成、測試通過、CI 綠燈都不等於實機可用。
- 無法驗證時明確寫「未驗證」與原因，不要用模糊字眼讓使用者以為已修好。
- 同一條路徑連續失敗時，先停下來把失敗原因做細，不要連續更換策略硬試。

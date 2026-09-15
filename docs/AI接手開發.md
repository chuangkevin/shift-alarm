# AI 接手開發

這份文件讓新的 AI 在只收到「請確認 alarm-shift 後接手開發」時，可以自行辨認專案、確認現況並安全接續。正式儲存庫名稱是 `shift-alarm`；`alarm-shift` 是使用者可能使用的別名。

## 現況基準

- GitHub：`git@github.com:chuangkevin/shift-alarm.git`，預設分支 `main`。
- 實機韌體：0.3.8。已用 USB 僅寫入非執行中的 app0 與單一 OTA 選擇 sector；重開後 Wi-Fi、Tailscale、方向、關屏、亮度、班表與鬧鐘均保留；目前為 90°、5 分鐘關屏、25% 亮度。
- 後端部署於 `rpi-matrix:/home/kevin/DockerCompose/shift-alarm`，只提供辨識、心跳與私有韌體。Gemini `max_tokens` 預設 6000、強制下限 400。
- 後端 0.1.5 與 GN100 Caddy 離線 fallback 已於 2026-09-15 部署。第二台已用隔離 provisioning 經 USB 啟動 0.3.12，配網熱點為 `ShiftAlarm-9CA8`，無 panic／boot loop／storage fault；第一台仍為 0.3.8。
- 目前韌體原始碼：`firmware-next/`，ESP-IDF 5.3.2 / Arduino 3.1.3。
- `firmware/` 是歷史版本，只供追查，不是更新來源。
- 裝置後端固定為 Tailscale 位址 `100.126.226.79:8237`。裝置自己的 Tailscale IP 可能因重新授權而改變，部署前必須讀取當下狀態，不可只抄舊紀錄。

0.3.8 的已驗收功能包括：首次 QR 配網、換 Wi-Fi、port 80 本機月曆與時／分選擇器、圖片辨識草稿、每三小時校時與手動校時、四方向旋轉、持久關屏時間／亮度與永久開啟、任一鍵喚醒／停鈴、鬧鐘強制亮屏、跨月「連續上班只通知第一天」、後續日期外框、每筆時間獨立開關、持續心跳，以及依臺北時間標示今天的紅點與細紅框。今天標記每分鐘原地更新，不重建日期按鈕。OTA 最多三次 Range 續傳已通過 host 測試，尚待下一次實機 OTA 驗收。

0.3.12 延續 0.3.11 的 GPIO38 更新 gate、240×240 主畫面、六項選單、三鍵狀態機與 STA MAC 尾碼，並新增最多四組 Wi-Fi、斷線掃描 failover 與網頁管理。健康連線不主動切換；Wi-Fi 密碼不進 API、頁面或 log。`alarm_nvs` 目前未加密，密碼依硬體政策以裝置本機 plaintext-at-rest 保存。硬體 GPIO38、Wi-Fi failover、完整映像跨重開、離線安裝、實體 UI／按鍵與 live OTA 尚未驗證。

## 接手後立即執行

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

## 系統資料流

```text
手機 ──同一 Wi-Fi / HTTP 80──> ESP32
                                  │
                                  └──原生 Tailscale──> 私有後端 :8237 ──> New API / Gemini
```

手機不必加入 Tailnet。ESP32 必須自行加入 Tailnet，才能使用 AI 圖片辨識與 OTA。手動月曆及鬧鐘直接保存在裝置，即使 Tailscale 未連線也可操作與響鈴。

`https://alarm.sisihome.org` 由 GN100 Caddy 直接代理 ESP32 的 Tailscale port 80，根路徑轉 `/calendar`；裝置區網 IP 也開同一個 `/calendar`。兩者必須保持為同一份 ESP32 頁面，不能再把 rpi-matrix 後端首頁當成第二套管理介面。

## 私密檔案邊界

下列檔案可能存在於開發機，但已被 Git 排除：

- `.env`
- `firmware-next/main/provisioning.h`
- `firmware/src/provisioning.h`
- `data/`
- `firmware-next/build/`
- `firmware/.pio/`

不可讀出或貼出其中秘密來「確認設定」。只確認檔案是否存在、權限是否正確及 Git 是否忽略。公開儲存庫只能保存 `provisioning.example.h` 的假值。新增設定時，同步更新 `.gitignore`、範本和這份清單。

## 開發與交付定義

每一項變更至少要做到：

1. 保留上面的資料流與離線能力，新增的使用者文字為繁體中文。
2. 加入能證明行為的必要測試；不要用只複製實作內容的測試湊數。
3. 執行相關 host 測試與完整後端測試；韌體改動完成 ESP-IDF 建置。
4. 提高韌體版本，且只把無憑證建置視為公開產物。實機映像保持私有。
5. 若使用者要求部署，嚴格依 `docs/更新部署.md` 與 `docs/Tailscale-OTA.md`；先預檢，再一次安裝，最後驗收保存資料與實體功能。
6. 更新 README、PLAN 與部署紀錄，提交後用 SSH 推送；確認 `HEAD`、`origin/main` 與 GitHub 公開頁面的 commit 一致。

沒有明確新需求時，以 `PLAN.md` 第一個仍適用的未完成工作為預設。若清單為空，只進行唯讀健康檢查並回報，不任意更改實機。

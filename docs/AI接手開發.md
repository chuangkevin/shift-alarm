# AI 接手開發

使用者只說「請確認 alarm-shift 後接手開發」時，`alarm-shift` 指本儲存庫 `shift-alarm`。先讀根目錄 `AGENTS.md`，再讀本檔、`README.md`、`PLAN.md`、`docs/更新部署.md`、`docs/Tailscale-OTA.md` 與 `docs/decisions.md`。不要只回覆已閱讀；完成 Git、測試與在線狀態的唯讀檢查後，從 `PLAN.md` 第一個仍適用的項目繼續。

## 資料流

```text
手機 ──同一 Wi-Fi / HTTP 80──> ESP32 100.104.66.47
                                  │
                                  └──原生 Tailscale──> GN100 100.127.82.47:8237
                                                               │
                                                               └──rpi-matrix 100.126.226.79:8237
                                                                 └──New API / Gemini
```

- 手機不必加入 Tailnet；同區網即可使用 ESP32 自己架設的 `/calendar`、`/display`、`/clock`、`/tailnet` 與 `/update`。
- ESP32 自己加入 Tailnet。AI 辨識、心跳與 OTA 固定連 GN100 `100.127.82.47:8237`，再由 Caddy 代理到 rpi-matrix。不要把 rpi-matrix IP 或區網 IP 重新 provision 回裝置。
- `https://alarm.sisihome.org` 由 GN100 Caddy 限 Tailnet 存取，代理 ESP32 `100.104.66.47:80`；根路徑轉 `/calendar`。網域與區網入口必須顯示同一份 ESP32 介面與設定。
- 沒有 Tailscale／後端時，手動月曆、鬧鐘、時鐘、顯示設定與離線響鈴仍可用。

## Kevin 與晴晴的獨立裝置（本分支）

`feat/kevin-dual-alarm` 專供第二顆 MAC `fc:01:2c:ca:15:88`：Kevin 與晴晴都固定週一到週五上班，兩人各有「週一、二、三、五」與「週四」兩組時間，每個時間獨立開關。裝置為 0.4.10、Tailnet `100.90.212.116`；遠端入口 `https://morning.sisihome.org`，裝置後端固定為 GN100 `100.127.82.47:8239`，代理到 rpi-matrix 獨立容器 `100.126.226.79:8239`。此分支的裝置權杖、資料庫與 OTA release 目錄都不可與原輪班裝置共用。

2026-09-17 實機驗證：0.4.6 用 USB 寫入非執行中的 app1，再以 sequence 24 切換，未改 NVS、bootloader 或 partition table；啟動後 Wi-Fi、時鐘、Tailscale、亮度 25%、方向 90°、關屏 5 分鐘均正常。兩人的每週規則已保存，`weeklyProfiles=true`、4 個啟用時段，主畫面分別顯示 Kevin 與晴晴的下一個鬧鐘。「晴」字已加入字型，實體畫面用到的所有非 ASCII 字元由測試與字型表逐一比對；關屏喚醒需穩定按住按鈕 250 ms 以過濾雜訊。0.4.6 將遠端儲存改成有識別碼的等冪工作、750 ms 狀態輪詢、短暫斷線重試與最後讀回比對；實測同一請求從 Tailnet IP 與 `morning.sisihome.org` 各送兩次皆回傳同一工作編號，完成後資料一致。0.4.6 映像已發布到 `shift-alarm-morning/data/releases`，供後續 OTA 使用。

2026-09-18 實機驗證：0.4.8 寫入非執行中的 app1，sequence 26，未改 NVS、bootloader 或 partition table。關屏喚醒改成「全部放開 1 秒 → 穩定按住 700 ms → 放開」，以隔離按鍵雜訊與卡鍵；1 分鐘測試設定到期後 `screenAwake=false`。MicroLink 只主動維持 priority peer GN100 `100.127.82.47`，不再週期探測整個約 60 節點的 Tailnet；其他節點仍可主動連入。實測 12 次連續遠端狀態請求全部回 200（約 0.13–0.91 秒），網域 `/calendar` 回 200。韌體的 `RING_MS=180000`，鬧鐘滿 3 分鐘會自動停止。0.4.8 映像摘要 `15e15f098dabb0b81ca4b280d7f3a35e64e0d775d5ec7559375a006f491d4f29` 已發布到獨立 `shift-alarm-morning/data/releases`。

同日 0.4.9 修正中間鍵喚醒過度嚴格：先全部放開 1 秒後，中間鍵正常按下約 0.1 秒並放開即可亮屏；左右鍵維持 700 ms 門檻。USB 寫入非執行中的 app0、sequence 27，未改 NVS、bootloader 或 partition table；兩人設定與 23 個已啟用鬧鐘保留。0.4.9 已發布，SHA-256 `4a5c00b76bb3079ac4b1583bcf3c15ab5868173ad892e60009417251f0c31879`。

同日 0.4.10 將刺耳方波改為 24 kHz 即時合成的海浪底聲與間歇鳥鳴，峰值由舊方波 5000 限制為 3600，鳥鳴有淡入淡出；`RING_MS=180000` 與任意鍵停止不變。USB 寫入非執行中的 app1、sequence 28，未改 NVS、bootloader 或 partition table；實機透過授權 API 試響後確認開始與停止狀態正確，23 個鬧鐘、Wi-Fi、Tailscale、後端、90°、亮度 25% 與關屏 5 分鐘均保留。0.4.10 已發布，SHA-256 `512d26a7faee0db6de471c522486754e6273a0531624173dfbd5cc59885adf2b`。

私有 `provisioning.h` 對這顆專用裝置是權威來源；0.4.1 起，權杖、後端與管理網址不相符時都會覆寫 NVS 並讀回驗證。這是修正換板後仍保留舊 `:8238`／舊網域的根因。不可將此行為或 0.4.x 韌體直接合併給原輪班裝置。

## 2026-09-16 已驗證基準

| 項目 | 現況 |
|---|---|
| 韌體原始碼 | 0.3.23，`firmware-next/` |
| 在線裝置 | `shiftalarm-9ca8`，MAC `fc:01:2c:c9:9c:a8`，Tailnet `100.104.66.47` |
| 後端 | rpi-matrix `100.126.226.79:8237`，版本 0.1.7 |
| 裝置後端入口 | GN100 `100.127.82.47:8237`，只綁 Tailnet，Caddy 代理到 rpi-matrix |
| 網域入口 | GN100 Caddy → ESP32 `100.104.66.47:80` |
| AI | GN100 New API OpenAI-compatible；優先 `go/deepseek-v4-flash-vision-exp`，兩個 Gemini 路由備援；`max_tokens` 預設 6000、下限 400 |

裝置已實測回報：0.3.23、後端可達、5 個鬧鐘、班表已同步、方向 90°、亮度 25%、關屏 5 分鐘、電池 100% 且充電中。班表 revision 是裝置資料，不應寫死在程式或文件。

0.3.21 已由 0.3.20 做真實兩階段 OTA：先下載 1,653,056 bytes，裝置驗證 staged 版本；再獨立 install、重開、回連。上述顯示與班表設定全部保留，`/api/update` 回到 idle。第一次 install POST 因 Tailnet HTTP timeout 未被裝置接受；確認版本仍為 0.3.20、staged sequence 未變且 `canInstall=true` 後，才人工重送一次。工具本身不得自動重試 mutation。

0.3.22 修正次級設定的連線頁：遠端與後端都正常時顯示「遠端與後端連線正常」，不再固定顯示異常警告。0.3.23 補回 TFT 字型表缺少的「二、四、六」，並把 Arduino 介面迴圈提高到 MicroLink priority-7 工作之上、輪詢間隔縮短為 2 ms。0.3.22 → 0.3.23 的兩階段 OTA、重開、回連與保存設定檢查已通過；星期顯示及實體按鍵體感仍需現場目視與操作確認。

後端 0.1.7 不再因單次 Gemini 503 立即結束辨識。它會對 429／500／502／503／504 在整體辨識期限內有限重試，並依序切換 `NEWAPI_MODELS`。2026-09-16 兩個 Gemini 路由同時持續回 503，因此 New API 的 `shift-alarm` token 已增加 `go/deepseek-v4-flash-vision-exp`；該模型已用九月班表圖片實測能讀取月份與上班日。其他 4xx 仍立即回報，避免把設定或授權錯誤當成暫時壅塞。

## 這次修正的根因

1. 裝置控制面可列出約 60 個 peers，但 MicroLink WireGuard table 只能裝 48 個；原本的 priority peer 只參與滿表淘汰，沒有保證優先安裝，rpi-matrix 因此可能根本不在資料面。
2. `ml_peer_map.c` 現在先安裝設定的 priority peer；裝置 priority peer 改成 GN100 `100.127.82.47`。DERP preferred region 改為香港 20。
3. GN100 新增 Tailnet-only `:8237` Caddy reverse proxy，以後端 Host 轉到主要服務。
4. Embedded WireGuard peer 建立是被動／延遲的；GN100 的 `shift-alarm-device-wake.timer` 每 30 秒讀一次裝置 `/api/status`，協助重開後建立 session。
5. 新板 NVS 曾保留舊 `DEVICE_TOKEN`，所以後端心跳 401。私有 `provisioning.h` 與後端 token 一致時，韌體現在只在值不同時覆寫 NVS token並讀回驗證；後端及管理網址只在空白時 provision。秘密值不可輸出或提交。
6. 後端輪詢先送 heartbeat，成功後才取 schedule；heartbeat 200 才標示後端正常。

## 接手後立即執行

```sh
git status --short --branch
git remote -v
git fetch origin
git rev-list --left-right --count HEAD...origin/main
python3 tools/audit_public_repo.py
PYTHONPATH=. .venv312/bin/pytest -q
bash firmware-next/components/alarm_ota/tests/run_host_tests.sh
bash firmware-next/components/alarm_proxy/test/run_host_tests.sh
python3 firmware-next/components/alarm_ota/tests/test_manifest_contract.py
node --check static/app.js
```

載入 ESP-IDF 5.3.2 後再執行 `idf.py -C firmware-next build` 與 `idf.py -C firmware-next size`。`pytest` 必須從 repo 根目錄執行並設 `PYTHONPATH=.`。工作樹有變更時先理解與保留，不得 `reset --hard` 或 `clean -fd`。遠端是共同基準，整合前先 fetch 與比較。

## 部署規則

- 日常更新只發布 `firmware-next/build/xingzhi-cube-1.54tft-wifi.bin`，經 `deploy/publish_firmware.py` 放進後端私有 release 目錄。
- 依 `docs/Tailscale-OTA.md` 分開執行一次 `--download` 與一次 `--install`。回應遺失先讀 `/api/status` 與 `/api/update`，禁止盲目重送。
- USB 只作首次安裝或救援。先辨認 MAC、執行分區與 OTA metadata；一般救援只寫非執行中 app slot，再更新一個正確的 OTA data sector。不得盲寫 bootloader、partition table、NVS、執行中 app 或整片 flash。
- 正常 OTA 不擦 Wi-Fi、Tailnet 身分、顯示、時鐘、班表與鬧鐘設定。安裝後逐項驗證。
- 後端入口變更時同步更新公開 `provisioning.example.h`、私有 `provisioning.h`、`sdkconfig.defaults`、文件與 GN100 Caddy；任何密鑰仍只能留在私有檔案。

## 私密邊界

不可提交或輸出：`.env`、任何 `provisioning.h`、`DEVICE_TOKEN`、New API key、Wi-Fi 密碼、Tailscale 節點私鑰／狀態、SSH 私鑰、Flash/NVS 備份、SQLite 使用者資料、帶憑證的 `.bin` 或 OTA nonce。公開 repo 只能保留假值範本。提交前執行 `tools/audit_public_repo.py` 並確認忽略規則。

## 尚需實測

- 多組 Wi-Fi 在不同現場的實際 failover，以及管理頁新增／更新／移除。
- 四種大型 QR 在 0／90／180／270 度的實際掃描與裁切。
- staged 映像跨重開、部分下載重開從 0、斷網離線 install。
- 真實 Range 中斷續傳與充滿後 GPIO38 fail-closed 行為。

只有原始碼／測試通過的項目不得寫成已部署。部署後必須記錄版本、裝置、傳輸路徑、保存設定與仍未驗證項目，然後才 commit、push。

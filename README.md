# 班表鬧鐘

> AI 接手：本專案也可能被稱為 `alarm-shift`。新的 AI 只收到「請確認 alarm-shift 後接手開發」時，先讀 [AGENTS.md](AGENTS.md) 與 [AI 接手開發](docs/AI接手開發.md)，再依 [PLAN.md](PLAN.md) 接續；不要使用過時的 0.2.x 紀錄猜測現況。

ESP32-S3 星智 CUBE 1.54 吋獨立鬧鐘。班表與響鈴設定保存在裝置；手機在裝置網頁編輯月曆，也可經裝置的原生 Tailscale 上傳圖片至私有辨識服務。

新版韌體位於 `firmware-next/`。原始碼與在線裝置目前為 0.3.21；2026-09-15 已從 0.3.20 經裝置原生 Tailscale 完成真實 OTA 下載、驗證、安裝、重開與設定保存驗收。裝置 Tailnet 位址為 `100.104.66.47`，裝置到 AI／OTA 的固定入口是 GN100 `100.127.82.47:8237`；GN100 再經 Tailnet 代理到 rpi-matrix 的主要後端。

0.2.8 已實測手機經 ESP32 與原生 Tailscale 上傳九月班表，36.851 秒辨識出正確 11 天。升至 0.3.1 後，Wi-Fi、90 度方向、11 筆鬧鐘與班表 revision 均保留。0.2.9 的首次 OTA 下載中止且保留原版本；0.3.0 起已加入十分鐘總期限、三十秒無進度保護與進度回報。0.3.2 已從 0.3.1 經原生 Tailscale 完整 OTA，下載、驗證、重開、回連與保存資料檢查均通過。

## 第一次使用與換網路

1. 首次開機建立 `ShiftAlarm-XXXX` 熱點並顯示 QR Code；手機加入後開啟 `http://192.168.4.1`。不沿用原廠韌體的 Wi-Fi。
2. 在裝置頁面掃描、選擇 Wi-Fi 並輸入密碼。連線成功後才加入最多四組的保存清單；失敗時保留舊清單並自動恢復連線。
3. 使用畫面上的裝置網址進入「班表與鬧鐘」。手動設定日期與時間不需要辨識伺服器。
4. 要使用圖片辨識或下載更新，先在「Tailscale 連線」完成官方授權，並確認裝置能經原生 Tailscale 到達後端。
5. 更換環境時，同時按住「＋」與「－」十秒，倒數完成後開啟配網；放開會取消。也可從裝置設定重新配網。一般斷網不會自動開放熱點。

Wi-Fi 設定以 `wifi-v2-a`／`wifi-v2-b` 兩個 generation+CRC slot 與 checksummed active selector 保存於 `alarm_nvs`。新 slot 與 selector 都完成 readback 後才套用；selector 結果不明時封鎖修改，reload 只接受 selector 明確授權的 generation。目前硬體政策沒有啟用 NVS 加密，因此 Wi-Fi 密碼是裝置本機 plaintext-at-rest；介面與 API 不回傳密碼、hash 或 token。韌體會明確覆寫可控制的固定 credential buffer、暫存 blob 與舊 slot；HTTP parser／Arduino `String` 的內部配置無法保證完整 heap scrubbing，NVS journal／wear leveling 也無法保證舊 flash page 的物理抹除，因此只縮短生命週期並清除可控制的邏輯紀錄，不宣稱完整清除所有 RAM 或 flash remanence。

裝置不需要與 Pi 或 GN100 位於相同區網。手機到裝置的本地 Wi-Fi 是操作入口；**裝置到辨識／OTA 後端必須使用原生 Tailscale，不得以同區網位址替代驗收或作為備援設計**。

## 月曆與鬧鐘

- `/calendar` 為統一班表頁，`/schedule` 轉至同一頁。選月份、點選上班日期，再用時／分選單設定每天最多八個時間。
- 今天的日期依臺北時間（UTC+8）顯示右上紅點與細紅色內框，每分鐘更新；它不改變上班、休假或連續班的顏色與排程。
- 每個響鈴時間都有獨立開關；關閉只停用該時間，時間仍保留以便再次開啟。沒有全域鬧鐘開關。
- 「連續上班只通知第一天」會跨月份判斷相鄰日。第一天維持深色實心並產生鬧鐘，後續連續上班日保留在月曆中，以同色外框顯示但不產生鬧鐘。
- 初始時間空白；有上班日而沒有響鈴時間時不能儲存。沒有上班日也可保存時間，重新開機仍保留。
- 儲存只替換當月，其他月份保留。月份設定與鬧鐘一起原子保存；最多 120 個月份設定、512 個鬧鐘。
- 手機上傳圖片先在瀏覽器壓成 JPEG（原圖最多 32 MB、長邊最多 1600 像素），減少傳輸時間；後端仍驗證實際圖片與大小。
- 圖片辨識僅填入待儲存草稿，不會直接覆蓋裝置班表。橙色待確認日期須逐日處理；取消、逾時或辨識失敗會保留原有草稿日期。
- 辨識必須包含圖片標示的完整年月與當月所有日期。上班／必上班、休假／必休假按原文字分類，模糊或其他文字需確認。
- 一次性測試鬧鐘不會被轉成每日固定時間。舊月份若每天時間不同，統一時間前會要求確認。
- 所有日期與時間固定使用臺北時間（UTC+8），每三小時 SNTP 校時，也可從 `/clock` 手動調整。斷網可依現有時鐘與已保存排程響鈴；斷電後仍須重新取得可信時間，沒有斷電走時保證。
- 螢幕方向可保存為 0／90／180／270 度；閒置關屏可選 1／5／15／30／60 分鐘或永久開啟。非響鈴時長按機殼頂部中間的 BOOT 鍵 1.2 秒，放開後關屏；關屏後任一鍵只喚醒。
- 鬧鐘到點會強制亮屏並響鈴；響鈴時按任一實體按鈕停止。關屏只控制顯示器，不停止時鐘、排程、網路或喇叭。Wi-Fi、方向、關屏設定、月份設定及排程均須在重啟驗收時確認。
- 非響鈴、非配網時短按左鍵或右鍵開啟選單；中鍵進入 `手機設定`、`連線狀態`、`班表資訊`、`裝置資訊` 或 `檢查更新`。詳細操作見 [實體按鍵](docs/實體按鍵.md)。實體鍵不會下載、安裝或刪除更新。

## 私有後端

Python 3.12 / FastAPI 部署在 `rpi-matrix:/home/kevin/DockerCompose/shift-alarm`，實際服務為 `100.126.226.79:8237`。ESP32 固定連 GN100 的 Tailnet-only HTTP 入口 `100.127.82.47:8237`，由 Caddy 代理至 rpi-matrix；它只提供 AI 辨識、心跳與私有韌體。`https://alarm.sisihome.org` 也由 GN100 Caddy 限 Tailnet 存取，直接代理目前在線 ESP32 `100.104.66.47:80`，根路徑轉到 `/calendar`。因此網域和裝置區網 IP 使用同一份 ESP32 介面與資料。

圖片辨識目前使用 GN100 New API 的 OpenAI-compatible 介面與 `gemini-flash`；`max_tokens` 預設 6000 且強制不低於 400。這個 OpenAI-compatible 入口沒有提供原生 Gemini `thinkingBudget` 欄位，因此保留模型思考並以足夠輸出額度避免空字串。韌體的健康檢查／整體等待為 60／240 秒。逾時不套用班表；辨識金鑰只存在後端。

| 環境變數 | 用途 |
|---|---|
| `NEWAPI_URL` | 介面 base，預設 `https://newapi.sisihome.org/v1` |
| `NEWAPI_KEY` | 此服務專用辨識金鑰 |
| `NEWAPI_MODEL` | 預設 `gemini-flash`，須確認該金鑰有模型權限 |
| `NEWAPI_MAX_TOKENS` | 預設 `6000`，程式強制下限為 `400`，避免 Gemini 思考後沒有剩餘額度輸出 JSON |
| `DEVICE_TOKEN` | 後端／裝置共用授權與 OTA 清單 HMAC 金鑰 |
| `MANAGEMENT_URL` / `REMOTE_URL` | 管理入口 |
| `ALARM_DATA` | 持久資料目錄，預設 `./data` |
| `ALLOWED_HOSTS` | 允許的 HTTP Host |

後端根路徑會轉到裝置 `/calendar`，不再提供第二套班表介面；以裝置保存結果為準。備份 SQLite 請使用 backup API，或停止服務後一併保存 DB／WAL 與圖片；不要只複製正在寫入的主資料庫檔。

裝置心跳可選擇附帶 `battery`：`{schema:1, valid, percent, charging, sample_age_seconds}`。舊版不附電量仍可使用。後端的 `/device-offline` 只顯示最後連線與最後有效電量；未知電量心跳不會清除最後有效快照。

Caddy 離線 fallback 必須晚於後端更新。部署前由 Caddy 主機確認後端 `HEAD /device-offline` 回應 `200`、空 body 與離線頁專用 CSP；只有 `/api/health` 成功不足以證明 fallback 頁可用。

## 開發、首次燒錄與 OTA

新版使用 ESP-IDF 5.3.2 / Arduino 3.1.3。`firmware/` 是舊 PlatformIO 實作，不能作為新版更新映像。操作步驟與回退條件見 [更新部署](docs/更新部署.md)、[Tailscale OTA 操作](docs/Tailscale-OTA.md)、[OTA 元件](firmware-next/components/alarm_ota/README.md) 及 [原生 Tailscale](firmware-next/components/alarm_tailnet/README.md)。

```sh
# 已載入 ESP-IDF 5.3.2 環境
idf.py -C firmware-next build
idf.py -C firmware-next size

# Python 3.12 虛擬環境
python -m pip install -r requirements.txt
python -m pytest -q
node --check static/app.js
```

首次改用新版分區配置需要受控 USB 安裝；先識別實際板子並保存該板完整私有備份。後續 OTA 只發布應用程式 `.bin`，不能發布整片 Flash 備份、bootloader 或 partition table。發布工具先驗證板型、遞增版本、大小、XOR 與內建 SHA-256，再原子切換後端的發布清單；發布不會讓裝置自動安裝。

裝置「裝置更新」先經 Tailscale 下載至備用應用程式分區；完整大小、HMAC、SHA-256、板型與版本驗證通過後，只保存一筆小型 authenticated marker，不重新啟動。完整映像可跨重開保存，之後不依賴 Wi-Fi、Tailnet 或後端即可安裝；安裝前會從 flash 重讀整份映像驗證並再次檢查充電、時鐘與鬧鐘 guard。marker 讀寫結果不明時顯示 `marker-fault` 並封鎖下載／安裝，直到 reload 驗證出 staged record，或 discard 後確認 marker 不存在。部分下載沒有 marker；同一次開機可 Range 續傳，重開後從 byte 0 重新下載。

`provisioning.h`、`.env`、節點身分、Flash 備份及帶憑證映像均為私密檔案，不提交 Git，也不放公開 Releases。CI 使用無裝置憑證的建置；實際部署憑證只在受控環境注入。

GitHub 儲存庫：[chuangkevin/shift-alarm](https://github.com/chuangkevin/shift-alarm)，已使用 SSH 推送；Actions 狀態以儲存庫最新 run 為準。現有主機曾以來源檔部署；`deploy/update.sh` 須在主機改為正確 Git clone 並設定遠端後才適用。

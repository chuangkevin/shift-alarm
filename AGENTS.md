# AI 接手規則

本專案正式名稱是 `shift-alarm`。使用者說「alarm-shift」、「鬧鐘專案」或「請確認 alarm-shift 後接手開發」時，都指這個儲存庫。

## 只有一句接手指令時

不要只回覆已閱讀。依序完成以下工作：

1. 找到遠端為 `git@github.com:chuangkevin/shift-alarm.git` 的工作目錄；不要依資料夾名稱猜測。
2. 讀完本檔、`README.md`、`docs/AI接手開發.md`、`docs/更新部署.md`、`docs/Tailscale-OTA.md` 與 `docs/decisions.md`。
3. 執行 `git status --short --branch`、`git remote -v`、`git fetch origin`，比較本地與 `origin/main`。遠端是共同基準；先保護未提交變更，不可直接覆蓋或清除。
4. 跑 `docs/AI接手開發.md` 的快速驗證。失敗時先定位原因，不可部署已知失敗版本。
5. 讀 `PLAN.md` 的「目前可接續工作」，從第一個仍適用的未完成項目繼續。若沒有明確功能可做，就完成唯讀健康檢查並回報目前版本、測試與阻礙，不要虛構需求。

## 不可破壞的設計

- 手機到 ESP32 可走同一 Wi-Fi 的區網與 port 80；ESP32 到圖片辨識／更新後端固定走裝置本身的原生 Tailscale。
- 沒有 Tailscale 時，本機月曆、手動班表、鬧鐘、時鐘及顯示設定仍須可用。只有 AI 圖片辨識與 OTA 後端會失效，介面要明確說明原因。
- 第一次使用先顯示配網 QR，手機加入 `ShiftAlarm-XXXX` 後在 `http://192.168.4.1` 選 Wi-Fi。換網路可用設定頁，或同按「＋」「－」十秒。
- 設定必須持久化。正常 OTA 不可擦除 NVS、Wi-Fi、Tailnet 身分、方向、關屏時間、班表或鬧鐘。
- 平時畫面不可每秒全畫面重繪。鬧鐘響時畫面閃爍；任一實體鍵停鈴。平時短按中間 BOOT 鍵關屏，關屏後任一鍵喚醒。關屏不能停止計時、網路、排程或響鈴。
- 全部使用者介面使用繁體中文。

## 安全與發布

- 永遠不要提交或輸出 `.env`、`provisioning.h`、`DEVICE_TOKEN` 真值、New API 金鑰、Wi-Fi 密碼、Tailscale 節點狀態／私鑰、SSH 私鑰、Flash/NVS 備份、SQLite 使用者資料或帶憑證的 `.bin`。
- `firmware-next/` 是目前韌體；`firmware/` 是舊 PlatformIO 實作，不得拿來產生新版 OTA。
- 一般更新只能發布應用程式 `.bin`，先做 Tailscale OTA 唯讀預檢，再由操作者確認穩定供電後送出一次安裝請求。不得自動重送 POST、改用區網後端、執行 `erase-flash`，或盲寫 bootloader、partition table、NVS、OTA 選擇區。
- 只有使用者已要求部署或推送時才執行對應動作。部署後驗證版本、回連、設定保存、校時、畫面、按鍵與測試鬧鐘；通過後再更新文件與推送。
- 公開 CI 必須在沒有私人憑證的環境可執行。裝置專用建置及發布檔一律留在私有環境。

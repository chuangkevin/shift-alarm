# 目前可接續工作

AI 接手規則見 `AGENTS.md`，準確現況見 `docs/AI接手開發.md`。

## 已完成

- [x] 手機以同一 Wi-Fi 與 ESP32 port 80 操作；ESP32 到 AI／OTA 固定走原生 Tailscale。
- [x] QR 配網、換 Wi-Fi、統一月曆、圖片辨識草稿、手動時分選擇、UTC+8 校時、四方向、亮度、關屏與設定持久化。
- [x] 每個鬧鐘獨立開關；連續上班只通知第一天，後續上班日保留外框；今天以紅點與細紅框標示。
- [x] Gemini `max_tokens` 預設 6000 且強制至少 400。
- [x] 0.3.20 已用 USB 寫入非執行中分區，未改 NVS、bootloader 或 partition table。
- [x] MicroLink priority peer 改為真正優先安裝；裝置後端改走 GN100 `100.127.82.47:8237`，GN100 再代理到 rpi-matrix。
- [x] 0.3.21 已由 0.3.20 完成真實 Tailscale OTA。下載、staged 驗證、獨立安裝、重開、回連、後端健康與設定保存皆通過。

## 目前可接續工作

- [ ] 實機驗證兩組以上 Wi-Fi 的斷線 failover，以及管理頁加入／更新／移除。
- [ ] 實測配網、設定、月曆、更新 QR 在 0／90／180／270 度均可掃描且不裁切。
- [ ] 實測 staged 映像跨重開、部分下載重開從 0、斷網離線安裝。
- [ ] 以真實網路中斷驗證 HTTP Range 續傳。
- [ ] 實測 ADC2 channel 6、GPIO38 active-high 與充滿後 fail-closed 行為。

## 已知限制

- 本板沒有已驗證的斷電持續走時 RTC；斷電重啟後須用 SNTP 或手動校時。
- 圖片辨識與 OTA 依賴裝置原生 Tailscale、GN100 入口、rpi-matrix 後端及 New API；手動班表與本地響鈴不依賴它們。
- ESP32 重開後的 WireGuard session 可能要等 GN100 30 秒喚醒 timer；短暫離線不可立刻判定失敗。
- Tailscale 裝置 IP 在重新授權後可能改變；部署前要重查，並同步 GN100 網域 upstream 與 wake timer。
- OTA mutation 回應遺失時先用唯讀狀態判斷是否已接受；不得由工具自動重試。

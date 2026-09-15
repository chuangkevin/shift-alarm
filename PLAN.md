# 目前可接續工作

AI 接手規則見 `AGENTS.md`，完整現況見 `docs/AI接手開發.md`。以下只保留仍有效的工作，完成後應更新本檔，避免新接手者執行過時項目。

## 現況

- [x] 0.3.2 已透過裝置原生 Tailscale 從 0.3.1 完成 OTA。
- [x] 手機與 ESP32 可走區網；ESP32 與辨識／更新後端固定走原生 Tailscale。
- [x] QR 配網、換 Wi-Fi、統一月曆、AI 草稿、手動時／分選擇、校時、四方向、關屏、實體鍵及重啟保存均已有實機驗收。
- [x] 九月實圖在 36.851 秒辨識出正確 11 天上班日。
- [x] 0.3.2 OTA 後確認 Wi-Fi、90 度方向、永久開啟、11 筆鬧鐘、班表 revision、校時及 Tailnet 回連均保留。
- [x] 1 分鐘自動關屏、BOOT 關屏、任一鍵喚醒及測試鬧鐘強制亮屏／停鈴均已驗收。
- [x] 網域入口已改為直接代理 ESP32，因此網域與裝置區網 IP 使用同一份介面與資料。
- [x] 0.3.8 已安全寫入非執行中分區並完成實機驗收：跨月連續上班抑制、外框顯示、每筆時間獨立開關、持續心跳、儲存班表堆疊修正、持久亮度設定，以及依臺北時間顯示今天的紅點與細紅框。今天標記每分鐘原地更新，不會清除日期按鈕焦點。OTA Range 續傳只有 host 測試，待下一次實機 OTA 驗收。
- [x] 後端 0.1.2 已部署；Gemini `max_tokens` 預設 6000 且強制不低於 400。
- [x] 後端 0.1.4 與 GN100 Caddy 離線 fallback 已於 2026-09-15 部署；健康檢查與在線網域回應 200。
- [x] 第二台 ESP32-S3 已於 2026-09-15 經 USB 啟動 0.3.12；版本、`ShiftAlarm-9CA8`、寫後 SHA 通過，無 panic／boot loop／storage fault。
- [x] 第二台已用一次性私有 seed 經正式 A/B transaction 保存兩組不同 SSID；刷回不含 Wi-Fi 密碼的一般 0.3.12 後，重開仍為 saved mode，沒有配網 AP 或 storage fault。seed 映像、NVS snapshot 與 source 巨集已清除。
- [x] 0.3.13 已刷入第二台：後端 HTTP 與 schedule 處理移出主迴圈，按鍵與鬧鐘檢查不再等待網路；所有實體 QR 統一放大為 version 8、scale 3。啟動保留兩組 Wi-Fi，無 panic／boot loop／storage fault。

## 目前可接續工作

- [ ] 實機驗證 ADC2 channel 6 曲線、GPIO38 active-high 充電狀態及充滿後 fail-closed 拒絕行為。
- [ ] 第二台裝置實機驗證 240×240 主畫面、六項選單、QR、四方向、按鍵 debounce／長按／配網 chord、響鈴優先及 15 秒返回。
- [ ] 實測配網加入、設定網址、`/calendar` 與 `/update` 四種大型 QR 在 0／90／180／270 度均可掃描且不裁切。
- [ ] 第二台裝置實機驗證兩組已保存 Wi-Fi 的連線與 failover，以及管理頁加入／更新／移除、hidden fallback、斷線退避、AP 持續可用與 IP 變更提示。
- [ ] 實機驗證完整 staged image 跨重開保存、部分下載重開從 0、斷網離線安裝及 marker lifecycle。
- [ ] 用真實中斷完成 HTTP Range 續傳、live OTA 與線上入口 `/device-offline`；在完成前不部署。

新增需求後，把工作拆成可驗收項目放在這裡。完成程式、測試、必要實機驗收、文件、提交與推送後才勾選。

## 已知限制

- 本板沒有已驗證的斷電持續走時 RTC；斷電重啟後須由網路或手動校時取得可信時間。
- 圖片辨識依賴裝置原生 Tailscale、私有後端及 New API；三者任一不可達時，手動月曆仍須正常使用。
- 第一次從原廠韌體遷移仍需受控 USB 安裝；正常後續更新使用 Tailscale OTA。
- Tailscale 裝置 IP 可能在重新授權後改變；每次部署都要從即時狀態取得。
- 0.3.3 下載曾在 666,392 / 1,569,792 位元組停滯後安全中止。0.3.8 以 USB 僅寫入非執行中 app0 與單一 OTA 選擇 sector，未改 NVS、bootloader 或分區表；Range 續傳仍須在下一次實機 OTA 驗收。
- 0.3.8 對 1,617,664-byte 映像的兩次明確單次下載，分別在 1,309,111 與 356,671 bytes 停止；後端 0.1.4 paced stream 部署後，第三次 POST 在開始前被斷線，裝置狀態未重置。三次均未完成，裝置保持 0.3.8 與原設定；停止 OTA 重送，改走 USB bootstrap。
- 第四次由 GN100 送出控制請求後，0.3.8 在 760,496 bytes 停止；後端 0.1.5 改為 4 KiB 連續串流後，第五次仍在 358,736 bytes 停止。server 端三種 chunk 策略均重現裝置端 `read()==0` 即中止；停止 OTA 重送，唯一下一步是第二台裝置以 Mac USB 資料線 bootstrap 目前候選 0.3.12。
- 0.3.13 尚待刷入第二台；GPIO38 gate、staged flash、離線安裝、實體 TFT 排版與按鍵、四組 Wi-Fi failover、真實 Range reconnect、全部大型 QR 與 live OTA 尚未在硬體／實際網路驗證。原始碼、host 測試或建置通過不代表已部署。

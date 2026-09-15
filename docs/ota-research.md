# OTA 為什麼一直失敗：程式碼與實測研究

日期：2026-09-15。範圍：第二台 ESP32-S3（MAC `fc:01:2c:c9:9c:a8`），韌體 `0.3.16`，後端 `0.1.6`（`:8238`）。

## 結論（先講）

**先前把原因歸給「傳輸 chunk 策略」是錯的。真正的原因是 OTA 傳輸途中，裝置每 10 毫秒重跑一次安全檢查，任何一項安全條件一瞬間不成立，就會把整個傳輸丟掉並中止。**

這也解釋了為什麼第一台在四個不同 offset 失敗（1,309,111／356,671／760,496／358,736）：那不是網路斷點，是「下一次安全檢查沒過」的時間點。

## 證據

### 傳輸途中被丟掉的程式路徑

- `firmware-next/main/main.cpp:980`：主迴圈每一輪都呼叫 `refreshOtaGuard(); if(otaReady) alarm_ota_maintenance();`，迴圈週期約 10 ms。
- `firmware-next/components/alarm_ota/alarm_ota.c:454`：`alarm_ota_maintenance()` 在 `RECEIVING` 時檢查期限與 `safe_now()`；只要不是 OK，就呼叫 `discard_transfer()`。
- `firmware-next/components/alarm_ota/alarm_ota.c:61`：`discard_transfer()` 會 `esp_ota_abort`、清掉 SHA、把狀態設回 IDLE、`received = 0`。
- `firmware-next/components/alarm_ota/alarm_ota.c:283`：每次寫入也先走 `active()`；狀態已變 IDLE 時回 `ESP_ERR_INVALID_STATE`。

### 安全檢查包含哪些條件

`firmware-next/components/alarm_ota/alarm_ota_policy.c` 的 `alarm_ota_check_guard()`：

- 時鐘有效、班表就緒、`now_epoch` 合理
- 充電訊號有效且為充電中
- 沒有響鈴、沒有貪睡
- **下一次鬧鐘不在 quiet window 內**（`main.cpp:966` 設為 300 秒）

### 錯誤回報把真正原因吃掉

- `main.cpp:765`：任何不是充電中斷的錯誤，訊息一律寫成「更新寫入或安全檢查失敗，保留原有版本」。
- `main.cpp:768`：任何不是充電中斷的原因，`lastReason` 一律寫成 `resume-failed`。

所以實測看到的 `lastReason=resume-failed`、`reconnectCount=0`、`received=4096`，**不代表續傳失敗**。實際是第一次寫入成功後，維護檢查把傳輸丟掉，第二次寫入拿到 `ESP_ERR_INVALID_STATE`。`lastHttpStatus=200` 只是最初那次韌體 GET 的狀態碼。

### 第一台的隨機 offset 也對得上

`a93ce6c`（0.3.8）的 `alarm_ota_write` 同樣每一塊都呼叫 `safe_now()`，`alarm_ota.c` 也有同樣的維護檢查。第一台有 15 個鬧鐘，quiet window 300 秒；一次十幾分鐘的下載期間，鬧鐘幾乎必然會進入 5 分鐘窗內，於是下一次寫入就中止。四次失敗停在不同位置，符合「時間點」而非「位元組點」。

### 第二台這次的情形

第二台沒有鬧鐘（`alarmCount=0`），所以不是 alarm-near。剩下最可能是充電訊號瞬斷：電池 100 %、充電中，充電器進入涓流／收尾時 GPIO38 可能短暫不表示充電中，一次讀到非充電就足以讓維護檢查丟掉整個傳輸。

同一時間後端已完全排除：

- 本機完整下載 1,648,432 bytes 花 0.49 秒
- `Range: bytes=4096-` 正確回 `206`，`Content-Range: bytes 4096-1648431/1648432`

### 另一個獨立缺陷：更新根本常常打不開

- `firmware-next/main/backend_poll_policy.h:43`：`blocksOta()` 等於「後端輪詢正在進行」。
- 實測 51 次取樣只有 3 次 `canDownload=true`。慢速線路上輪詢幾乎一直佔用，OTA 被自己的閘門鎖住。
- 另外 `alarm_ota.c:55` 的 `lock()` 是 **0 逾時** 的 mutex take，任何短暫競用都會讓那次寫入直接失敗。

## 修正清單

### 必須進韌體（需要 USB 一次）

1. 維護檢查不要靜默丟掉傳輸：改為記錄並回報具體原因，暫停傳輸等待條件恢復，而不是 `discard_transfer()`。
2. 充電訊號加入去彈跳：連續數秒都不是充電中才視為拔電；單次抖動不應中止。
3. 修正原因碼：寫入失敗、狀態失效、時鐘不可信、鬧鐘接近、充電中斷要分開，不再全部寫成 `resume-failed`。
4. OTA 開放下載時暫停後端輪詢，而不是用 `backend-poll-busy` 拒絕。
5. 寫入路徑的 `lock()` 改為有界等待（例如 100 ms），避免競用造成假失敗。
6. 600 秒傳輸期限要能量測後調整；DERP relay 下 1.6 MB 本來就接近上限。

### 後端（已可部署，非本次阻塞）

- 已完成：辨識上限 180 秒、圖片不再被 q92 撐大。
- 後端端點經實測正確，不需要為了 OTA 再改。

## 驗證計畫（一次 USB 就夠）

1. USB 寫入帶診斷的版本，序列輸出每一項護欄結果。
2. 桌上重現：故意設一個 4 分鐘後的鬧鐘，確認傳輸是「暫停後恢復」而不是中止。
3. 滿電狀態下跑一次 OTA，確認充電抖動不會中止傳輸。
4. 之後才量測實際吞吐，決定是否需要提高 600 秒期限。

## 仍未確認

- 第二台這次的護欄失敗點究竟是充電抖動、時鐘、或其他條件，還沒有序列紀錄可證實；上面的判斷是推論，需要用帶診斷的版本確認。
- 裝置到後端的實際吞吐量還沒量到（先前那段是推估），這會決定 1.6 MB 是否可能。

## 這件事的教訓

先前五次 OTA 失敗，我把時間花在調整後端 chunk 大小與節奏，但真正的中止點在裝置端的護欄，而且錯誤訊息把它偽裝成傳輸問題。**在沒有把裝置端失敗原因做細之前，任何傳輸層調整都無法驗證**，這也是為什麼每一版都看起來差一點、卻永遠不會好。

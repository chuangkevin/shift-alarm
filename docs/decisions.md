# 持久決策與驗證

使用者要求首次裝置顯示QR、由手機選Wi-Fi後才區網設定鬧鐘。不可再復用原小智Wi-Fi或要求先登入Tailnet。

後端與硬體分工：本地AP配網完全在ESP32。New API圖像讀取和SQLite班表在Pi；裝置NVS保存epoch排程。UI預覽套用避免模糊班別變正式鬧鐘。時間尚未指定，不設定自動6:30等猜測值。

驗證：backend 8 tests pass；general真圖九月11上班日正確。韌體原16MB完整備份，board marker唯一xingzhi-cube-1.54tft-wifi。Reviewer找出容量/CSRF/remote command reboot去重已修。

韌體0.1.1：離屏緩衝避免平時閃爍、響鈴才500ms紅黑閃；三鍵任意停鈴；＋與－同按十秒倒數配網，放開取消；斷網不自動進AP。裝置設定頁支援四個90度方向並保存；繁體中文點陣字型附OFL授權。

韌體0.1.2：方向設定寫入NVS並讀回成功才套用；NVS異常不自動格式化或清除使用者設定。測試不得覆寫使用者正在操作的方向。

- 韌體 0.3.5：連續上班可只在第一天建立鬧鐘，後續上班日仍以外框顯示；每個響鈴時間可獨立啟停。完整班表儲存曾造成 Arduino `loopTask` 8 KiB 堆疊溢位，實機序列輸出確認後將堆疊提高為 16 KiB，重新儲存與重開機保留均通過。
- 後端 0.1.2：GN100 New API 的 Gemini 請求 `max_tokens` 預設 6000 且強制下限 400，避免模型思考後沒有剩餘額度輸出 JSON。
- 韌體 0.3.6：TFT 背光改用 GPIO 13 的 5 kHz／8-bit PWM，亮度可選 10、25、40、60、80、100%，與方向、關屏時間同一筆 NVS 原子保存。既有 v1 顯示設定第一次讀取後遷移為 40%；實機再設為 25%。關屏輸出 duty 0，喚醒與響鈴恢復使用者亮度。
- 韌體 0.3.7：月曆初版以 `Asia/Taipei` 判定今天，日期格右上顯示紅點並加細紅色內框，且加上 `aria-current="date"` 與「今天」標籤；這版每分鐘會重建月曆，造成日期按鈕焦點可能遺失，已由 0.3.8 修正。
- 韌體 0.3.8：依獨立檢查修正今天標記的定時更新；只原地切換標記與輔助屬性，避免每分鐘重建月曆造成鍵盤焦點遺失。
- 韌體 0.3.9／後端 0.1.3 原始碼：電池採 ADC2 channel 6 oneshot，前三個有效樣本每秒取樣，之後每 60 秒取樣並以最近三個有效值過濾；讀取失敗不重試，最後有效值超過 300 秒才改為未知。GPIO38 依 pinned upstream 採 active-high，只影響充電狀態，不使百分比失效。
- 心跳的電池物件維持 optional；有效電量以伺服器收到時間減去最多 300 秒的樣本年齡形成觀測時間。未知心跳只更新目前狀態，不刪除最後有效快照。
- Tailnet 控制面與後端可達性分開顯示。啟動配置／配置失敗自動限界退避重試；OTA POST 不重送，Range reconnect handshake 每次最多三次退避嘗試。
- 韌體 0.3.10：OTA 分成 `下載更新`、`安裝並重新啟動`、`刪除已下載更新`。下載與安裝都只接受 GPIO38 當下有效且 active-high 的充電狀態，不提供人工 override。GPIO38 不是可靠 VBUS 偵測；充滿時即使 USB 已接上也可 fail-closed 拒絕。
- 完整映像經 exact size、stream SHA、`esp_ota_end` 與 descriptor 驗證後，才把單一 bounded authenticated marker 存入既有 `alarm_nvs`。部分映像不留 marker；重開後從 byte 0 重新下載。同次開機保留 Range reconnect。
- 安裝不依賴後端或網路；它重新驗 manifest/marker HMAC、從 partition table 推導 inactive slot、重讀整片 flash SHA/descriptor、重跑 local guards，清 marker 成功後才切 boot。boot selection 失敗時保留目前版本並要求重新下載。
- marker I/O 結果不明時進入 fail-closed `marker-fault`。它不等同「沒有更新」：不得下載覆寫 target，也不得假設可安裝。GET／boot observation 只 load/validate，絕不 clear/store 或改 flash；可無寫入恢復 authenticated staged record或 confirmed absence。corrupt/incompatible marker 必須由明確 discard 清理，且 clear 後再次 load 確認不存在才回 idle。
- 韌體 0.3.10 的實體更新資訊頁設計已由 0.3.11 六項選單取代；實體鍵仍不會下載、安裝或刪除更新。
- 後端 0.1.4 與 GN100 Caddy 離線 fallback 已於 2026-09-15 部署；目前候選韌體 0.3.12 未安裝，GPIO38、Wi-Fi failover、實體 240×240 UI／按鍵、persisted staged image、離線安裝與 live OTA 均保留為未驗證。
- 0.3.8 從後端 0.1.3 下載 1,617,664-byte 映像的兩次明確單次嘗試，分別在 1,309,111 與 356,671 bytes 停止，裝置回報舊版合併的 `n<=0 || alarm_ota_write` 錯誤；裝置保持健康。停止位置不固定，不能只憑這兩次結果證明單一根因。
- 後端 0.1.4 只對 authenticated firmware binary endpoint 改用 4096-byte chunks，chunk 之間等待 5 ms；1,617,664 bytes 約 395 個 chunks，單是 pacing 約增加 2 秒，連同傳輸與 flash 寫入以約 8 秒完成為目標，仍遠低於舊版 600 秒總期限。這是降低原生 Tailnet/TCP burst pressure、協助 0.3.8 bootstrap 的 mitigation；live retry 前不宣稱已修復。
- 2026-09-15 部署 paced stream 後的第三次 0.3.8 POST 在下載狀態重置前即 `RemoteDisconnected`；裝置仍回報前次 356,671-byte 失敗狀態。這次沒有開始寫入，paced stream 未能完成 bootstrap；當時停止 OTA 重送並改走 USB 路徑。現行待驗證目標是 0.3.12。
- 第四次改由 GN100 送出控制請求，裝置在 760,496 bytes 停止；後端 0.1.5 保留 4 KiB chunk 但移除 5 ms 間隔後，第五次仍在 358,736 bytes 停止。FileResponse、4 KiB paced 與 4 KiB continuous 都無法避免 0.3.8 在 `available()>0` 後 `read()==0` 即中止；不得再重送。現行下一步是第二台裝置以 Mac USB 資料線 bootstrap 0.3.12。
- 韌體 0.3.11：240×240 主畫面只顯示日期、星期、電池、HH:MM、下次上班、響鈴時間與真實倒數；沒有未來鬧鐘時顯示 `尚無下一次鬧鐘`。QR、網址、連線、班表與更新診斷移至六項實體選單。
- 三鍵改為純狀態機輪詢：30 ms debounce，中鍵長按 1.2 秒且放開後關屏，關屏按鍵只喚醒，左右同按 10 秒優先配網，響鈴時任一原始按鍵立即停鈴。選單 15 秒無操作返回主畫面。
- AP 與 Tailnet 名稱尾碼改由 `esp_read_mac(..., ESP_MAC_WIFI_STA)` 的 STA MAC bytes 4、5 產生；`fc:01:2c:c9:9c:a8` 顯示 `9CA8`。目前只有來源碼、host 測試與建置驗證，第二台裝置燒錄前不宣稱硬體完成。
- 韌體 0.3.12：最多四組 Wi-Fi 使用 schema 3 的 A/B generation+CRC slots 與 checksummed active selector 保存在 `alarm_nvs`。inactive slot 與 selector 都完成 exact readback 後才套用，舊 active slot 之後才明確清除。selector 結果不明時進 storage-fault 並封鎖 mutation；reload 只套用 selector 明確授權且 generation 相符的 slot，不依較新 orphan slot猜測。舊 `wifi-v1` 或 `ssid`／`password` 也走同一 transaction，selector+slot 驗證成功後才刪除。
- 斷線後 async scan，以 RSSI 由強到弱嘗試可見保存網路，平手依保存順序；hidden 網路最後依保存順序各試一次。每組 20 秒，整輪失敗以 2 至 60 秒退避。健康連線不因其他 SSID 較強而切換。
- 新 SSID 連線成功後才保存；失敗時不改清單並回復 failover。任何 profile mutation 都先取消 scan／連線、清除 pending credential 並斷線，commit 後從新清單重掃；移除最後一組會斷線並開配網熱點。NVS 未加密，Wi-Fi 密碼依現行硬體政策是裝置本機 plaintext-at-rest，API／UI／log 不輸出。固定 credential buffer、decode/encode blob、pending password 與舊 slot 使用 volatile wipe；Arduino `String`／HTTP parser 的不可控 heap copy 及 NVS journal／wear leveling 的舊 flash page 只縮短生命週期或清除邏輯紀錄，不宣稱完整物理清除。
- Arduino Wi-Fi SDK persistence 明確關閉；連線嘗試不另寫 SDK credential storage，四組清單只由 `alarm_nvs` 的 `wifi-v2-a`／`wifi-v2-b` 與 selector 管理。

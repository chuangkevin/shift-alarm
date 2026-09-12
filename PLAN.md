# 班表鬧鐘 v0.1.0

## 0.2.0 整合進度（2026-09-13）
- [x] AI 實圖辨識：七月 11 個上班日，11、28 日待確認；不自動套用測試結果
- [x] 後端更新清單／映像身份與摘要驗證、裝置憑證保護
- [x] OTA 元件政策測試、跨語言 HMAC 向量、ESP-IDF 5.3.2 編譯連結
- [ ] 裝置本機 Tailnet：正式登入、到期重登、即時撤銷與併發處理
- [ ] 區網手機透過裝置上傳班表：串流代理與提早拒絕回應驗證
- [ ] OTA 主程式整合：排程同步鎖、管理頁 Host 防護、必要初始化自測
- [ ] 整體韌體編譯、獨立審查與逐單元提交
- [ ] 識別目前 USB 裝置與備份，再刷入完整候選版本
- [ ] 實機配網、登入、上傳判讀、持久化、按鍵響鈴與重啟驗證
- [ ] 更新操作文件、交付原始碼；GitHub 建立權限仍缺少

原機已由使用者透過原廠網頁恢復。先前 USB 中斷與黑屏原因尚未確認，不可記為硬體燒毀。0.2.0 尚未刷入；元件測試通過不等於實機驗證。

使用者已要求直接執行；先前已說明「網頁辨識、裝置保存與獨立響鈴」並获准開始，省略重複設計批准。

## 設計
- 私有 homelab backend + 手機網頁，rpi-matrix 同區網可供 ESP32 連線；GN100 Caddy 提供 alarm.sisihome.org tailnet HTTPS。
- 上傳每月班表圖片，New API general讀圖，完整日期與月份驗證。上班/必上班為work，休假/必休假為off，儘量放假或不清楚為review。照片文字僅資料。
- 解析結果可手動編輯，按套用後產生排程；固定響鈴時間由使用者在網頁設定，預設空白不啟用。
- SQLite持久化月班表/設定/同步狀態；device API使用獨立token；New API key只留伺服器。
- ESP32保存UTC epoch排程與管理網址，SNTP校時；斷網可響，斷電後未有可信時間顯示待校時。螢幕QR+下一鬧鐘，停鈴/貪睡。
- 不更改既有小智機器的硬體腳位猜測；先讀備份辨識真board，再燒錄。

## 工作
- [x] 取得最新homelab文件，SSH文件整合推送
- [x] GitHub私鑰分發：9/10含來源驗證成功，M5離線保留待辦
- [x] 硬體識別、完整16MB flash備份、韌體編譯與刷入（hash verified）
- [x] Backend/網頁/完整月份驗證/排程tests（8 passed）
- [x] New API實圖辨識驗證（general辨識九月11個上班日，與原圖一致）
- [x] 部署區網後端、私有網域（兩者health 200；既有New API/OpenCode正常）
- [ ] USB首啟已驗證setup=1 wifi=0；待使用者掃QR配網後驗證顯示/同步/實際響鈴
- [x] 獨立review修正後No findings、文件/版本/CI定義、本地commit
- [ ] 新專案GitHub push：SSH有效，但無建立新repo的登入；homelab-docs已push

## 邊界
未收到響鈴時刻，保留空設定；七月與九月附圖可作解析樣本。Firmware与web并行，接口：GET /api/device/schedule Authorization bearer token -> {revision,timezone,alarms:[{id,epoch,label}],management_url,server_time}。

## 使用者最新更正
首次必定AP+WiFi加入QR，再device本地192.168.4.1 scan/select WiFi，成功後才LAN鬧鐘管理；不可使用原韌體WiFi。QR配網不依赖backend/Tailscale。

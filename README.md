# 班表鬧鐘 v0.1.0

ESP32 星智 CUBE 1.54 吋版 + 私有區網班表管理服務。

## 第一次使用

1. 裝置第一次開機建立 `ShiftAlarm-XXXX` Wi-Fi 熱點，顯示加入熱點的 QR Code。
2. 手機掃描加入裝置熱點；按裝置 `+` 切換網頁 QR，打開 `http://192.168.4.1`。
3. 在裝置本地網頁掃描、選取家中 Wi-Fi 並輸入密碼。此步驟不需要 Tailscale、New API 或 Pi。
4. Wi-Fi 測試成功才保存；失敗會保留配網模式。成功後螢幕顯示裝置區網網址 QR 與下一次鬧鐘。
5. 從區網管理入口上傳月班表；檢查辨識結果、套用，再設定每天固定響鈴時間並啟用。

原小智韌體的 Wi-Fi 不會自動帶入。同時按住＋和－十秒重新配網方法見 `firmware/README.md`。

## 班表與響鈴

- New API `general` 讀圖；上班、必上班視為上班，休假、必休假視為休假。
- 儘量放假、不清楚或其他班別先標待確認；待確認不產生鬧鐘，可手動調整。
- 必須完整列出指定月份，缺日、重複、錯誤月份會拒絕，不會覆蓋原有班表。
- 每天最多四個響鈴時間；時區 Asia/Taipei。初始時間空白且未啟用。
- 裝置保存排程，斷網仍可依現有可信時鐘響鈴；斷電後未完成校時不能保證時間正確。
- 網頁的「已同步」依裝置回報revision，不能只因後端保存成功就宣稱裝置收到。
- 測試按鈕只新增一次20秒後測試鬧鐘；物理停鈴/貪睡詳見firmware文件。

## 後端

Python 3.12，`pip install -r requirements.txt`，`uvicorn app:app --host 127.0.0.1 --port 8237`。
使用環境變數（不提交秘密）：

| 變數 | 用途 |
|---|---|
| NEWAPI_URL | OpenAI-compatible base，預設 `https://newapi.sisihome.org/v1` |
| NEWAPI_KEY | 本應用獨立 general-only token |
| NEWAPI_MODEL | 預設 general |
| DEVICE_TOKEN | 獨立裝置token，與韌體配對 |
| MANAGEMENT_URL | 區網管理網址 |
| REMOTE_URL | 可選Tailnet遠端網址 |
| ALARM_DATA | 持久資料目錄，預設 ./data |
| ALLOWED_HOSTS | 允許HTTP Host，逗號分隔 |

既有部署：rpi-matrix `/home/kevin/DockerCompose/shift-alarm`，Compose project `shift-alarm`，LAN `192.168.18.31:8237`、tailnet `100.126.226.79:8237`，GN100 Caddy入口 `alarm.sisihome.org`。部署時依實際主機修改compose的IP；不要綁不受保護的公網介面。

資料在 `data/alarm.sqlite3`（WAL）和正規化班表图片。備份請用SQLite backup API或停止本服務後一併保存DB/WAL與圖片。API key與DEVICE_TOKEN僅放伺服器環境和device私有設定，不返回管理頁。

## 驗證

`python -m pytest -q`；`node --check static/app.js`；`docker build -t shift-alarm:0.1.0 .`；`pio run -d firmware`。
CI定義包含backend、Docker和firmware；尚未推送新專案，因此未有GitHub Actions執行結果。目前主機以來源檔部署，待建立Git遠端並改為clone後，才可使用 `deploy/update.sh`。GitHub SSH認證可用，但目前沒有可建立新儲存庫的API或網頁登入。

初次實測New API辨識2026-09附圖：4、8、9、11、13、14、17、20、24、25、29上班，完整30日；正式鬧鐘保持未啟用，等待使用者設定時間。

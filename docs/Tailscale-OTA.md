# 經原生 Tailscale 更新

後續應用程式更新只經 Tailscale。`deploy/ota_tailnet.py` 不呼叫 esptool、不寫 USB、不發布映像、不改引導程式或分區表，也不擦除 NVS。首次安裝仍依 `更新部署.md` 的獨立 bootstrap 流程；裝置無法啟動或沒有雙 OTA 分區時，此工具無法修復。

## 前置條件

- 裝置與操作電腦均已加入 Tailnet；ACL 允許電腦到裝置 TCP 80、裝置到後端 TCP 8237。
- 已安裝的韌體須固定使用原生 Tailscale 後端，`/api/status` 回報 `backendTransport: "tailscale"` 與 `backendHost: "100.126.226.79"`。缺少此資訊的旧版會遭工具拒絕，不能以 LAN 通道代替。這是現存版本的 bootstrap 限制，不能只靠待安裝版本新增此欄位。
- 已完成新版審查、測試與應用程式建置；版本必須遞增。使用 `deploy/publish_firmware.py` 在後端私有 `data/releases` 目錄發布應用程式映像，詳見 `更新部署.md`。發布和安裝是兩個獨立動作。
- `DEVICE_TOKEN` 放在私有檔案（權限 `0600`），不放命令列、Git 或公開附件。帶 token 的編譯映像亦不可公開。
- 穩定供電、時鐘與排程已恢復，未響鈴、未貪睡，五分鐘內無鬧鐘。工具檢查可見狀態，韌體在寫入與啟動前仍會再次檢查完整 guard。

## 操作

先執行唯讀預檢。以下位址與版本請依實際裝置及已審查發布版本填寫：

```sh
python3 deploy/ota_tailnet.py \
  --device http://100.90.212.116 \
  --backend http://100.126.226.79:8237 \
  --token-file /私有目錄/device-token.txt \
  --version 0.2.8
```

預檢會透過裝置 Tailnet IP 取得本機頁與 nonce、確認原生 Tailnet 身分和 ACL、檢查固定後端及透過裝置代理取得 `/api/health`，並從後端驗證預期版本的 HMAC 清單。它不使用 DNS、LAN fallback、環境 HTTP proxy 或 HTTP redirect。後端清單由現有 DEVICE_TOKEN 按 `board\nversion\nsize\nsha256\n` 簽署；token 與 nonce 不會輸出。

確認已接穩定電源後，同一命令加上 `--start --power-confirmed`，才會送出一次安裝請求。電源旗標是操作者確認，不是電壓量測。命令不重試安裝 POST，即使回應遺失也不重送。

工具預設最多等 900 秒（可用 `--wait-seconds 60..1800` 調整），透過同一 Tailnet IP 驗證目標版本、方向、班表模式、本地班表的 revision／鬧鐘數、原生連線身分和固定 Tailscale 後端資料通道。逾時表示尚未完成驗收，不能解讀為映像必定失敗；先唯讀檢查裝置與 OTA 狀態。工具不自動改回 LAN、刷機或重新授權。

## 保存與驗收範圍

OTA 元件只將映像寫到備用 app partition，驗證後切換啟動；NVS、Wi-Fi、Tailnet 身分、方向與排程不在寫入範圍。新韌體仍可能有資料遷移錯誤，因此版本可達不等於所有功能已驗收：另檢查班表內容、時鐘、喇叭與按鈕。工具不會因正常後端同步使 revision 改變就報錯，也不會為測試而新增響鈴。

回退依賴 bootstrap 安装的回退引導程式、雙 OTA 分區與有效舊映像。開機自測確認硬體初始化、NVS 排程恢復與本機服務；它不保證外部 Tailscale 服務當時可達。命令完成後的資料通道檢查補上此項驗收，但不是硬體測試報告。

## 開發驗證

```sh
python3 -m unittest discover -s deploy/tests -v
sh firmware-next/components/alarm_ota/tests/run_host_tests.sh
```

工具測試以假的 API 回應驗證拒絕規則、HMAC 與預檢唯讀性；不是實機 OTA 或真實 WireGuard/TLS 傳输驗證。尚未執行實機更新時，不得把 host 測試或成功編譯記為 OTA 成功。


下載最多十分鐘；三十秒沒有成功寫入的新資料就中止。OTA 元件從 begin 到 activate 的總期限同為十分鐘，包含下載前準備與驗證，因此可用下載時間略少於十分鐘。`/api/update` 的 `received`／`total` 為已成功寫入與預期的位元組數，介面顯示百分比，錯誤區分總期限、停滯、連線與寫入／安全檢查。工具在舊版仍運行且更新已停止時提早結束，不重試 POST。此修正不推定先前未具進度資訊的失敗一定由超時造成。

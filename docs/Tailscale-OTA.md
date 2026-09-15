# 經原生 Tailscale 更新

後續應用程式更新只經裝置原生 Tailscale。`deploy/ota_tailnet.py` 不發布映像、不使用 USB、不改 bootloader／partition table／NVS 分區，也不自動串接下載與安裝。

## 供電 gate

下載及安裝都要求 GPIO38 當下取樣有效且 active-high `charging=true`。沒有人工確認、checkbox 或 CLI override。

這是刻意 fail-closed 的充電狀態，不是可靠的 USB／VBUS 偵測。電池充滿時可能停止回報充電，因此即使 USB 仍接著，裝置也可能回覆 `charging-required`。先保留已下載更新，等裝置再次顯示充電再安裝。

## 兩階段流程

1. 唯讀預檢只讀取 `/update`、`/api/status` 與 `/api/update`：

```sh
python3 deploy/ota_tailnet.py --device http://裝置-Tailscale-IP
```

2. 下載前另驗證裝置原生 Tailnet、固定後端、後端健康狀態及 signed manifest。只送一次 `/api/update/download`：

```sh
python3 deploy/ota_tailnet.py \
  --device http://裝置-Tailscale-IP \
  --backend http://100.127.82.47:8237 \
  --token-file /私有路徑/device-token.txt \
  --version 新版本 --download --wait-seconds 900
```

下載完成後不會重開。完整大小、stream SHA-256、`esp_ota_end` 與 image descriptor 通過後，裝置把小型 authenticated marker 寫入既有 `alarm_nvs`。映像留在 inactive OTA slot，不把 binary 放進 NVS。

3. 安裝只讀本地 staged image，不連後端。只送一次 `/api/update/install`：

```sh
python3 deploy/ota_tailnet.py \
  --device http://裝置-Tailscale-IP \
  --version 新版本 --install --wait-seconds 900
```

安裝會重新驗 manifest/marker HMAC，從 partition table 推導 inactive slot，確認 marker target 相符且不是 running slot，從 flash 重讀完整映像計算 SHA-256，重讀 board/version descriptor，並在驗證期間及 boot selection 前重查充電、時鐘、排程與鬧鐘 guard。清除 marker 成功後才選擇 boot partition。若 boot selection 失敗，裝置留在目前版本並要求重新下載。

4. 不要安裝時可只清 marker；這不擦除 app partition：

```sh
python3 deploy/ota_tailnet.py --device http://裝置-Tailscale-IP --discard
```

三個 mutation mode 互斥。工具不會重試 mutation POST，也不會把 `--download` 自動接成 `--install`。回應遺失時先執行唯讀預檢。

## 中斷與重開

- 同一次開機的 HTTP stream 中斷，最多三輪 Range reconnect，每輪 handshake 有界重試。
- 未完成映像沒有 marker；重開後從 byte 0 重新下載。
- 完整 staged image 與 marker 跨正常重開保存，可在 Wi-Fi、Tailnet及後端不可用時從裝置本地 `/update` 安裝。
- target 已成為 running partition、schema 不相容、marker 損毀、HMAC 錯誤、board/version/target 不符時，不會提供安裝；唯讀檢查只顯示 `marker-fault`，不會清除或改寫 NVS。必須明確執行 discard，再確認 marker 不存在。
- marker load/store/readback/clear 只要無法確認結果，狀態就是 `marker-fault`，下載與安裝都封鎖。唯讀 `/api/update` 只會再次 load，不呼叫 marker clear/store，也不改 flash；讀到完整 authenticated record 或明確不存在時可無寫入恢復。`刪除已下載更新` 是唯一清理 corrupt/incompatible marker 的操作，但只有 clear 後 load 明確回覆不存在才恢復 idle。
- OTA boot rollback self-test 不依賴 Internet，行為不變。

## 狀態契約

`GET /api/update` 回傳 current version、staged metadata、phase、busy、`markerFault`、`canDownload`／`downloadReason`、`canInstall`／`installReason`、received/total、Range reconnect 診斷、boot session 與 sequence。`marker-fault` 是穩定 reason。成功 discard、boot-set failure 或其他 confirmed no-staged terminal state會清除 received/total/progress；後端 manifest latest version另行保存。狀態不含 token、nonce、credential URL 或 private Tailnet raw state。

`/update` 以 nonce 保護三個 POST；唯讀狀態可跨 reboot/session 讀取。瀏覽器用 boot session、sequence 與 request ordering 拒絕 stale response，mutation 不自動重送。

## 驗證狀態

```sh
python3 -m unittest discover -s deploy/tests -v
sh firmware-next/components/alarm_ota/tests/run_host_tests.sh
python3 firmware-next/components/alarm_ota/tests/test_manifest_contract.py
```

2026-09-15，裝置 `100.104.66.47` 已從 0.3.20 經原生 Tailscale 升級到 0.3.21。1,653,056-byte 映像完整下載並由裝置驗證後，另一次 install POST 完成切換、重開與回連；方向 90°、亮度 25%、關屏 5 分鐘、班表 revision、5 個鬧鐘及 Wi-Fi／Tailnet 身分均保留。更新狀態回到 `idle`，後端健康檢查回覆 200。第一次 install POST 在 HTTP 逾時前未被接受；唯讀狀態證明裝置仍為 0.3.20、staged sequence 未變且 `canInstall=true` 後，才由人工作出第二次送出決定。工具不得自行重試。

2026-09-16，0.3.22 與 0.3.23 均依相同兩階段流程經原生 Tailscale 安裝。0.3.23 映像為 1,655,296 bytes、SHA-256 `bfc0cd6bda76850ed4c6c7e979fd72b55ccab8576f4a89618fc2f47096cb805a`；裝置重新上線後回報後端可達、5 個鬧鐘、方向 90°、亮度 25%、關屏 5 分鐘與原班表版次，更新狀態回到 `idle`。

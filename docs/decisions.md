# 持久決策與驗證

使用者要求首次裝置顯示QR、由手機選Wi-Fi後才區網設定鬧鐘。不可再復用原小智Wi-Fi或要求先登入Tailnet。

後端與硬體分工：本地AP配網完全在ESP32。New API圖像讀取和SQLite班表在Pi；裝置NVS保存epoch排程。UI預覽套用避免模糊班別變正式鬧鐘。時間尚未指定，不設定自動6:30等猜測值。

驗證：backend 8 tests pass；general真圖九月11上班日正確。韌體原16MB完整備份，board marker唯一xingzhi-cube-1.54tft-wifi。Reviewer找出容量/CSRF/remote command reboot去重已修。

韌體0.1.1：離屏緩衝避免平時閃爍、響鈴才500ms紅黑閃；三鍵任意停鈴；＋與－同按十秒倒數配網，放開取消；斷網不自動進AP。裝置設定頁支援四個90度方向並保存；繁體中文點陣字型附OFL授權。

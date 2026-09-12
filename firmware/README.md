# Shift Alarm firmware v0.1.2

## Verified hardware

ESP32-S3 rev 0.2, 16 MB QIO flash, 8 MB PSRAM. The original application contains `xingzhi-cube-1.54tft-wifi` and its matching source filename. Hardware configuration follows the upstream [78/xiaozhi-esp32 board](https://github.com/78/xiaozhi-esp32/tree/d91d6824309d12df5fc3db5edf8145ee829135d7/main/boards/nologo/xingzhi-cube-1.54tft-wifi): ST7789 240×240 (MOSI10/SCLK9/DC8/CS14/RST18/backlight13), power latch21, speaker I2S DOUT7/BCLK15/LRCK16, BOOT0/plus40/minus39. No external audio codec is needed for this board.

## First boot and daily use

1. First boot always starts `ShiftAlarm-XXXX`, using a randomly generated WPA password printed on the display. Original WiFi credentials are **not imported**.
2. Scan the first QR to join this setup WiFi. Accept the phone's “no Internet” network. Press **plus** to switch to the second QR, which opens `http://192.168.4.1`.
3. Select a scanned SSID or type a hidden network name, enter its password, and connect. An unsuccessful attempt remains in setup. Successful connection persists credentials; setup AP closes after 20 seconds.
4. Rejoin your home WiFi and scan the normal screen QR. It opens the device's LAN page, which links to the LAN shift-management website. No Tailscale installation is required on the phone.
5. While ringing, **any of the three buttons stops the alarm immediately**, including during a backend request. Outside ringing, **Plus** switches the setup QR. Hold **Plus and Minus together for ten seconds** to reopen WiFi setup; the screen counts down and releasing either button cancels. Saved networks never automatically enter setup after a connection loss.
6. Open **Display settings** on the device local page (also available during WiFi setup), choose 0°, 90°, 180°, or 270°, and save. Orientation survives reboot.

TFT frames are composed in a memory buffer; the visible screen is never cleared between drawing the QR and text. Identical frames are not sent again. Only during ringing, the background alternates dark red/black every half-second.

Firmware polls the backend every five seconds, persists only changed schedule data, and sends heartbeat state. The alarm sound runs in an independent I2S task. A ring times out after three minutes. Alarms delayed by up to 90 seconds are caught up; older alarms are skipped. Handled timestamps and snooze deadlines persist across reboot. Device UI uses a bundled Traditional Chinese bitmap subset; the source and OFL license are in `tools/generate_glyphs.py` and `fonts/OFL.txt`. Alarm display uses a consistent Traditional Chinese label.

After complete power loss the ESP32 has no battery-backed wall clock. It waits for NTP or authenticated backend time before scheduling. After synchronization it continues keeping time and ringing stored alarms without WiFi while powered. Do not rely on an unsynchronized offline cold boot.

## Build

Install PlatformIO, then `pio run -e cube-tft`. The platform and libraries are pinned in `platformio.ini`. Host scheduler checks: `c++ -std=c++17 test/scheduler_test.cpp -o /tmp/shift-alarm-test && /tmp/shift-alarm-test`.

For an installation, generate ignored `src/provisioning.h` containing `PROVISION_BACKEND`, `PROVISION_TOKEN`, and `PROVISION_MANAGE` string macros. It must contain **no WiFi SSID/password**. Provisioning values are saved once into the dedicated `alarm_nvs` partition. The token is a secret; never commit that header or publish a provisioned binary. The initial backend is an HTTP address on the trusted LAN; HTTPS transport is not implemented in this prototype.

Do not flash another CUBE variant using these pins. Before upload, require a complete local original-flash backup and matching board evidence. The partition layout intentionally changes from the original XiaoZhi firmware; recovery uses the full 16 MB original image, not just its application region.

## Device protocol

- `GET /api/status`: safe diagnostic status, clock readiness, revision, ring state, alarm count.
- `POST /api/schedule`: Bearer-authenticated `{revision, timezone:"Asia/Taipei", alarms:[{id,epoch,label}]}`; maximum 512 alarms / 96 KB. Fully validates before replacing the current schedule.
- `POST /api/test`, `POST /api/stop`: Bearer-authenticated speaker control.
- Backend `GET /api/device/schedule`: same JSON plus optional `server_time` (Unix seconds), `management_url`, and `command:{type:"stop",id,issued_at}`. Stop commands are accepted once and only within two minutes.
- Backend `POST /api/device/heartbeat`: `{revision,status,next_alarm?,ip}`; status is `ready`, `waiting_for_time`, or `ringing`.

The setup form is available only after entering setup mode. Setup credentials are kept in NVS. The setup AP password is shown physically and is generated per device. Schedule storage uses a 256 KB NVS partition and byte blobs; server timestamps and heartbeat metadata do not cause repeated schedule flash writes.

Orientation is applied only after a successful NVS write and read-back. Startup storage errors stop startup without automatically erasing saved configuration.

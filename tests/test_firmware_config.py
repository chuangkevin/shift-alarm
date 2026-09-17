from pathlib import Path
import re


def test_calendar_save_has_sufficient_arduino_loop_stack():
    settings = {}
    for line in Path("firmware-next/sdkconfig.defaults").read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            settings[key] = value

    assert int(settings["CONFIG_ARDUINO_LOOP_STACK_SIZE"]) >= 16384


def test_battery_hardware_and_ui_contract_is_wired():
    source = Path("firmware-next/main/main.cpp").read_text()
    assert "ADC_UNIT_2" in source
    assert "ADC_CHANNEL_6" in source
    assert "ADC_BITWIDTH_12" in source
    assert "ADC_ATTEN_DB_12" in source
    assert "GPIO_NUM_38" in source
    assert "ESP_ERROR_CHECK(adc" not in source
    assert source.count("adc_oneshot_read(") == 1
    assert 'd["battery"]' in source
    assert 'd["backendReachable"]' in source
    assert 'd["tailnetLifecycle"]' in source
    assert 'd["tailnetLifecycleLabel"]' in source
    assert 'min-height:44px' in source
    assert 'id="device-health"' in source
    assert "charging_active(level)" in source
    assert "maximum-scale" not in source
    assert "min-height:42px" not in source
    assert source.count(".calendar button{") == 1


def test_reliability_contract_and_versions_are_wired():
    source = Path("firmware-next/main/main.cpp").read_text()
    assert 'd["canDownload"]' in source and 'd["downloadReason"]' in source
    assert 'd["canInstall"]' in source and 'd["installReason"]' in source
    assert 'd["markerFault"]=status.marker_fault' in source
    assert '"marker-fault"' in source
    assert 'server.on("/api/update/download",HTTP_POST' in source
    assert 'server.on("/api/update/install",HTTP_POST' in source
    assert 'server.on("/api/update/discard",HTTP_POST' in source
    assert 'server.on("/api/update/start",HTTP_POST' not in source
    assert 'id="download-update"' in source and 'id="install-update"' in source and 'id="discard-update"' in source
    assert '完整充電時即使接著 USB，也可能因未顯示充電而拒絕' in source
    assert '重新開機後會從 0 重新下載' in source
    assert 'GPIO_NUM_38' in source and 'out->charging_valid' in source and 'out->charging' in source
    assert 'otaConfig.marker_load=otaMarkerLoad' in source
    assert 'alarm_ota_load_staged()' in source
    assert 'nvs_open_from_partition("alarm_nvs","shift-alarm"' in source
    assert 'nvs_set_blob(handle,OTA_MARKER_KEY,record,sizeof(*record))' in source
    assert 'nvs_commit(handle)' in source
    assert 'if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_ERR_NOT_FOUND' in source
    assert 'otaReceived=status.staged.manifest.size' in source
    assert 'void otaResetTerminalNoStaged(){otaReceived=0;otaTotal=0;otaLastProgressMs=0;}' in source
    assert 'otaBackendLatestVersion' in source
    assert 'otaLatestVersion' not in source
    assert 'if(err==ESP_OK){otaResetTerminalNoStaged();' in source
    assert 'event==buttons::StopAlarm)stopRing(false)' in source
    assert 'uiState.page=deviceui::Page::Main;buttons::beginRinging(buttonState,ringStarted,!digitalRead(BUTTON_SNOOZE),!digitalRead(BUTTON_STOP),!digitalRead(BUTTON_TEST))' in source
    assert 'ringing=false;buttons::endRinging(buttonState)' in source
    assert source.index('for(auto &alarm:alarms)if(alarmclock::due') < source.index(
        'buttons::pairingAllowed(event,ringing)'
    )
    assert 'void closePortal()' in source
    start_ring = source.split('void startRing(String label)', 1)[1].split(
        'void stopRing', 1
    )[0]
    assert 'closePortal()' in start_ring
    assert 'event==buttons::CenterLong)setScreenAwake(false)' in source
    assert 'buttons::update(buttonState,ms,!digitalRead(BUTTON_SNOOZE),!digitalRead(BUTTON_STOP),!digitalRead(BUTTON_TEST)' in source
    assert 'buttons::pairingAllowed(event,ringing)' in source
    component = Path("firmware-next/components/alarm_ota/alarm_ota.c").read_text()
    assert component.index("err = esp_ota_end(g.writer)") < component.index("g.config.marker_store(&record")
    assert component.index("activation_clear") < component.index("activation_boot")
    assert "esp_partition_read(c->target" in component
    assert "target->subtype != record->target_subtype" in component
    assert "target->address != record->target_address" in component
    assert "if (g.state == ALARM_OTA_RECEIVING)" in component
    assert "esp_ota_set_boot_partition(c->target)" in component
    load_body = component.split("esp_err_t alarm_ota_load_staged(void)", 1)[1].split("typedef struct", 1)[0]
    assert "marker_load" in load_body
    assert "marker_clear" not in load_body
    assert "marker_store" not in load_body
    assert 'd["phase"]' in source and 'd["reconnectCount"]' in source
    assert 'd["session"]=otaBootSession' in source
    assert "OTA_STATUS_POLICY_START" in source
    assert 'd["lastHttpStatus"]' in source and 'd["lastProgressAgeSeconds"]' in source
    assert "displaySettingsValid" in source
    assert "settingsLoadValid" in source
    assert "ota_manifest::available" in source
    assert 'set(PROJECT_VER "0.4.2")' in Path("firmware-next/CMakeLists.txt").read_text()
    ota_fixture = Path("firmware-next/components/alarm_ota/tests/test_real_component.c").read_text()
    assert ota_fixture.count('version="0.4.2"') == 2
    assert 'version="0.3.12"' not in ota_fixture
    assert "VERSION = '0.1.10'" in Path("app.py").read_text()


def test_physical_menu_draw_and_gpio_are_integrated():
    source = Path("firmware-next/main/main.cpp").read_text()
    assert 'GFXcanvas16(240,240)' in source
    assert 'screen.init(240,240)' in source
    assert 'screen.setRotation(displayRotation)' in source
    assert 'screen.drawRGBBitmap(0,0,pixels,240,240)' in source
    assert 'uiState.page==deviceui::Page::Main' in source
    assert 'uiState.page==deviceui::Page::Menu' in source
    assert 'remoteOk&&backendOk?"遠端與後端連線正常":"連線異常不影響本機鬧鐘"' in source
    assert 'line(8,154,"連線異常不影響本機鬧鐘")' not in source
    for label in ('手機設定', '連線狀態', '鬧鐘資訊', '裝置資訊', '檢查更新', '返回主畫面'):
        assert label in source


def test_shared_page_shell_keeps_markup_before_page_local_style():
    source = Path("firmware-next/main/main.cpp").read_text()
    assert 'content.remove(styleStart,styleEnd+8-styleStart)' in source
    assert 'content=content.substring(styleEnd+8)' not in source
    assert '尚無下一次鬧鐘' in source
    assert 'vTaskPrioritySet(nullptr,8)' in source
    assert 'delay(2)' in source
    assert 'deviceui::countdown(now,next' in source
    assert 'BUTTON_STOP=0, BUTTON_SNOOZE=39, BUTTON_TEST=40' in source
    assert 'pinMode(BUTTON_STOP,INPUT_PULLUP)' in source
    assert 'pinMode(BUTTON_SNOOZE,INPUT_PULLUP)' in source
    assert 'pinMode(BUTTON_TEST,INPUT_PULLUP)' in source
    assert 'attachInterrupt' not in source
    assert 'esp_read_mac(staMac,ESP_MAC_WIFI_STA)' in source
    assert 'ESP.getEfuseMac()' not in source
    assert 'server.on("/api/update/download",HTTP_POST' in source
    assert 'event==buttons::CenterShort&&!portal)deviceui::center' in source
    qr_calls = re.findall(r"\bqr\(([^;]+)\);", source)
    assert len(qr_calls) == 3
    assert all(
        call.endswith("deviceui::QR_X,deviceui::QR_Y,deviceui::QR_SCALE")
        for call in qr_calls
    )
    assert not re.search(r"\bqr\([^;]+,\s*[12]\s*\);", source)
    assert 'line(deviceui::BOTTOM_TEXT_X,deviceui::BOTTOM_TEXT_Y,"掃碼設定班表")' in source
    assert 'portalView=(portalView+1)%3' in source
    update_draw = source.split('uiState.page==deviceui::Page::Update)', 1)[1].split(
        'const bool qrPage=', 1
    )[0]
    assert 'deviceui::QR_X,deviceui::QR_Y,deviceui::QR_SCALE' in update_draw
    assert 'alarm_ota_download' not in update_draw
    assert 'alarm_ota_activate' not in update_draw
    assert 'alarm_ota_discard' not in update_draw
    assert 'const bool mainMinute=' in source
    assert 'uiState.page!=deviceui::Page::Menu' in source
    main_draw = source.split('uiState.page==deviceui::Page::Main){', 1)[1].split(
        '} else if(uiState.page==deviceui::Page::Menu)', 1
    )[0]
    assert 'http://' not in main_draw
    assert 'alarm_tailnet_get_status' not in main_draw
    assert 'alarms.size()' not in main_draw


def test_tft_font_contains_every_weekday_glyph():
    glyphs = Path("firmware-next/main/zh_glyphs.h").read_text()
    for character in "日一二三四五六（）":
        assert f"{{{ord(character)}," in glyphs


def test_backend_poll_requires_tailnet_before_http_allocation():
    source = Path("firmware-next/main/main.cpp").read_text()
    start = source.split("void startBackendPoll(", 1)[1].split("\n}", 1)[0]
    worker = source.split("void backendPollWorker(", 1)[1].split("\n}", 1)[0]
    drain = source.split("void drainBackendPollResult(", 1)[1].split("\n}", 1)[0]
    loop = source.split("void loop() {", 1)[1]
    assert "connectivity::should_poll_backend" in start
    assert "otaBusy.load()" in start
    assert "backendpoll::canStart" in start
    assert "xQueueSend(backendRequestQueue,&request,0)" in start
    assert "measureJson(d)" in start and "HEARTBEAT_CAP=1024" in source
    assert "HTTPClient" not in start
    assert "HTTPClient" in worker and ".GET()" in worker and ".POST(" in worker
    assert "boundedHttpBody(schedule,MAX_JSON" in worker
    for forbidden in ("applySchedule", "prefs", "surface", "screen", "alarms", "handled", "ringing", "otaActivation"):
        assert forbidden not in worker
    assert "backendpoll::accepts" in drain
    assert "enqueueScheduleWork" in drain
    assert "secureWipeBackendExchange" in drain
    apply_schedule = source.split("void applyScheduleCandidate(", 1)[1].split("\n}", 1)[0]
    assert "backendpoll::invalidate(backendPollState)" in apply_schedule
    button = loop.index("buttons::update(")
    alarm = loop.index("alarmclock::due(")
    drains = [match.start() for match in re.finditer(r"drainBackendPollResult\(\)", loop)]
    assert any(offset < button for offset in drains)
    assert any(button < offset < alarm for offset in drains)
    assert "xQueueCreate(1,sizeof(BackendPollExchange*))" in source
    assert 'xTaskCreate(backendPollWorker,"backend_poll",8192,nullptr,1,nullptr)' in source
    assert "void pollBackend()" not in source


def test_schedule_worker_and_ota_poll_gate_are_integrated():
    source = Path("firmware-next/main/main.cpp").read_text()
    candidate = Path("firmware-next/main/schedule_candidate.h").read_text()
    routes = Path("firmware-next/main/local_calendar_routes.h").read_text()
    page = Path("firmware-next/main/calendar_page.h").read_text()
    worker = source.split("void scheduleWorker(", 1)[1].split("bool initScheduleWorker", 1)[0]
    assert 'return "backend-poll-busy"' not in source
    assert "backendpoll::blocksOta(backendPollState)" not in source
    assert source.index('const String url="http://127.0.0.1/api/device/heartbeat"') < source.index('const String scheduleUrl="http://127.0.0.1/api/device/schedule"')
    assert "request->heartbeat_ok&&request->fetch_schedule" in source
    assert source.count("setTimeout(60000)") >= 2
    assert "schedulecandidate::parse" in worker
    assert "scheduleNvsWriteExact" in worker
    assert "nvs_open_from_partition" in source
    for forbidden in ("applyScheduleCandidate", "alarms", "calendarMonths", "revision=", "syncState", "prefs"):
        assert forbidden not in worker
    post = routes.split('server.on("/api/local-calendar",HTTP_POST', 1)[1].split(
        'server.on("/api/local-calendar/save-status"', 1
    )[0]
    assert "deserializeJson" not in post
    assert "serializeJson" not in post
    assert "prefs" not in post
    assert "enqueueScheduleWork" in post and "server.send(202" in post
    assert "waitForSave" in page and "save-status?id=" in page
    assert "applyScheduleCandidate(work->candidate)" in source
    assert "drainScheduleResult()" in source
    assert "MAX_ALARMS=512" in candidate and "MAX_BYTES=98304" in candidate
    assert "鬧鐘識別碼重複" in candidate and 'doc["timezone"]!="Asia/Taipei"' in candidate
    assert 'schedule-fault-v1' in source
    assert 'scheduleStorageFault.store(scheduleFaultMarker(false))' in source
    assert 'savedScheduleRestored=!scheduleStorageFault.load()' in source
    assert 'latchScheduleStorageFault()' in worker
    assert 'if(work->persisted&&work->origin!=ScheduleOrigin::Rollback&&!scheduleNvsWriteExact(work->previous))latchScheduleStorageFault()' in worker
    assert "schedule-storage-fault" in source and "schedule-storage-fault" in routes
    assert "班表儲存狀態不明；重開前請勿修改" in source
    assert "原設定已保留" not in source


def test_multi_wifi_source_contract_is_wired_and_redacted():
    source = Path("firmware-next/main/main.cpp").read_text()
    profiles = Path("firmware-next/main/wifi_profiles.h").read_text()
    failover = Path("firmware-next/main/wifi_failover_policy.h").read_text()
    page = Path("firmware-next/main/wifi_page.h").read_text()
    assert 'WIFI_PROFILE_SLOT_KEYS[2][10]={"wifi-v2-a","wifi-v2-b"}' in source
    assert 'WIFI_PROFILE_SELECTOR_KEY[]="wifi-v2-sel"' in source
    assert 'nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READONLY,&handle)' in source
    assert 'nvs_open_from_partition("alarm_nvs","shift-alarm",NVS_READWRITE,&handle)' in source
    assert 'nvs_get_blob(handle,key,nullptr,&stored)' in source
    assert 'if(result==ESP_ERR_NVS_NOT_FOUND)' in source
    assert 'nvs_set_blob(handle,key,data,length)' in source
    assert 'nvs_commit(handle)' in source
    assert 'nvs_close(handle)' in source
    storage_adapter = source.split('class WifiNvsStorage', 1)[1].split('WifiNvsStorage wifiStorage', 1)[0]
    assert 'prefs.isKey' not in storage_adapter
    assert 'prefs.getBytesLength' not in storage_adapter
    assert storage_adapter.count('nvs_get_blob(') == 2
    migration = source.split('wifiprofiles::List migrated', 1)[1].split('String currentWifiSsid', 1)[0]
    assert migration.index('persistWifiProfiles(migrated,committed)') < migration.index('prefs.remove(WIFI_CREDENTIALS_KEY)')
    assert 'WiFi.scanNetworks(true,true)' in source
    wifi_setup = source.split('setenv("TZ","CST-8",1)', 1)[1].split('routes();', 1)[0]
    assert wifi_setup.index('WiFi.persistent(false)') < wifi_setup.index('WiFi.mode(WIFI_STA)')
    assert 'WiFi.setAutoReconnect(false)' in source
    assert 'wififailover::CONNECT_TIMEOUT_MS' in source
    assert 'constexpr size_t MAX_PROFILES = 4' in profiles
    assert 'constexpr uint8_t SCHEMA = 3' in profiles
    assert 'checksum(' in profiles
    assert 'secureWipe(' in profiles
    assert 'volatile uint8_t *bytes' in profiles
    assert 'CommittedStorageFault' in profiles
    assert 'storage.writeSlot(inactive' in profiles
    assert profiles.index('storage.writeSlot(inactive') < profiles.index('storage.writeSelector(')
    assert profiles.index('storage.writeSelector(') < profiles.index('storage.eraseSlot(current->slot)')
    assert 'BACKOFF_MAX_MS = 60000' in failover
    assert 'if (state.failed_cycles < UINT8_MAX)' in failover
    assert 'WiFi.scanDelete();WiFi.disconnect(false,false);clearPendingWifi();wififailover::cancelForMutation' in source
    assert 'write(key,zeros,length)' in source
    assert 'pendingPassword.clear();pendingSsid.clear()' in source
    remove_route = source.split('server.on("/api/wifi/remove",HTTP_POST', 1)[1].split('server.on("/api/schedule"', 1)[0]
    assert remove_route.index('cancelWifiSelection(now)') < remove_route.index('persistWifiProfiles(next,committed)')
    assert remove_route.index('persistWifiProfiles(next,committed)') < remove_route.index('wifiProfiles=next')
    assert source.count('server.on("/api/wifi/remove",HTTP_POST') == 1
    assert 'server.on("/api/wifi/remove",HTTP_GET' not in source
    assert 'profile["password"]' not in source
    assert 'profile["hash"]' not in source
    assert 'profile["token"]' not in source
    assert '已保存 Wi-Fi' in page
    assert 'min-height:44px' in page
    assert 'confirm(' not in page and 'alert(' not in page


def test_fixed_weekday_profiles_are_managed_on_one_device_page():
    page = Path("firmware-next/main/calendar_page.h").read_text()
    assert "Kevin 與晴晴" in page
    assert "週一、二、三、五" in page
    assert "週四" in page
    assert "(disabled?'disabled_':'')+group+'_times'" in page
    assert "weekly_profiles" in page
    source = Path("app.py").read_text()
    assert "RECOGNITION_SECONDS', '180'" in source
    assert "im.thumbnail((1600, 1600))" in source
    assert "im.save(b, 'JPEG', quality=80)" in source


def test_wifi_rescan_is_available_from_the_manager_page():
    source = Path("firmware-next/main/main.cpp").read_text()
    page = Path("firmware-next/main/wifi_page.h").read_text()
    route = source.split('server.on("/api/wifi/scan"', 1)[1].split('server.on("/api/wifi/remove"', 1)[0]
    assert "if(!localNonce())return;" in route
    assert "connecting||wifiState.phase==wififailover::Phase::Scanning" in route
    assert "WiFi.scanNetworks(true,true)" in route and "WIFI_SCAN_FAILED" in route
    assert '"scanning"' in source
    assert 'id="wifi-rescan"' in page and "type=\"button\"" in page
    assert "'/api/wifi/scan'" in page and "'X-Setup-Nonce':nonce" in page
    assert page.count("min-height:44px") >= 2
    assert "setInterval" in page and "clearInterval(scanPoll)" in page


def test_manual_wifi_setup_waits_for_disconnect_to_settle():
    source = Path("firmware-next/main/main.cpp").read_text()
    handler = source.split('server.on("/setup"', 1)[1].split('server.onNotFound', 1)[0]
    assert "WiFi.begin(" not in handler
    assert "wifiTrialStarted=false;wifiTrialAt=now+wififailover::DISCONNECT_SETTLE_MS" in handler
    assert 'wifiPendingTrial&&!wifiTrialStarted&&wififailover::deadlineReached(now,wifiTrialAt)' in source
    assert 'wifiTrialStarted=true;connectStarted=now' in source
    assert 'WIFI_TRIAL_BEGIN' in source
    pending = source.split('void clearPendingWifi()', 1)[1].split('}', 1)[0]
    assert "wifiTrialStarted=false;wifiTrialAt=0" in pending

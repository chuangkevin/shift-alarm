from pathlib import Path


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
    assert 'if(events&8)stopRing(false)' in source
    assert 'else if(events&1)setScreenAwake(false)' in source
    assert 'rawChord=!digitalRead(BUTTON_TEST)&&!digitalRead(BUTTON_SNOOZE)' in source
    assert 'updateInfoPage=!updateInfoPage' in source
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
    assert 'set(PROJECT_VER "0.3.10")' in Path("firmware-next/CMakeLists.txt").read_text()
    assert "VERSION = '0.1.3'" in Path("app.py").read_text()

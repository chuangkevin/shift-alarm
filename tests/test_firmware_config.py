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
    assert 'd["canStart"]' in source and 'd["reason"]' in source
    assert 'd["phase"]' in source and 'd["reconnectCount"]' in source
    assert 'd["session"]=otaBootSession' in source
    assert "OTA_STATUS_POLICY_START" in source
    assert 'd["lastHttpStatus"]' in source and 'd["lastProgressAgeSeconds"]' in source
    assert "displaySettingsValid" in source
    assert "settingsLoadValid" in source
    assert "ota_manifest::available" in source
    assert 'set(PROJECT_VER "0.3.9")' in Path("firmware-next/CMakeLists.txt").read_text()
    assert "VERSION = '0.1.3'" in Path("app.py").read_text()

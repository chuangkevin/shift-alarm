from pathlib import Path


def test_calendar_save_has_sufficient_arduino_loop_stack():
    settings = {}
    for line in Path("firmware-next/sdkconfig.defaults").read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            settings[key] = value

    assert int(settings["CONFIG_ARDUINO_LOOP_STACK_SIZE"]) >= 16384

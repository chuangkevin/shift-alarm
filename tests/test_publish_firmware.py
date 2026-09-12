import hashlib
import json
from functools import reduce
from operator import xor
import pytest
from deploy.publish_firmware import BOARD, inspect_image, publish


def image(version='0.2.1', board=BOARD):
    raw=bytearray(288)
    raw[0]=0xe9;raw[1]=1;raw[12:14]=(9).to_bytes(2,'little');raw[23]=1
    raw[24:28]=(0x3c000020).to_bytes(4,'little');raw[28:32]=(256).to_bytes(4,'little')
    raw[32:36]=(0xabcd5432).to_bytes(4,'little')
    raw[48:48+len(version)]=version.encode();raw[80:80+len(board)]=board.encode()
    checksum=reduce(xor,raw[32:],0xef)
    raw+=bytes(15)+bytes([checksum])
    return bytes(raw)+hashlib.sha256(raw).digest()


def test_publish_accepts_valid_image_and_is_idempotent(tmp_path):
    source=tmp_path/'candidate.bin';source.write_bytes(image())
    releases=tmp_path/'releases'
    manifest=publish(source,releases)
    assert json.loads((releases/'active.json').read_text())==manifest
    assert (releases/(manifest['sha256']+'.bin')).read_bytes()==source.read_bytes()
    assert publish(source,releases)==manifest


def test_invalid_image_preserves_previous_release(tmp_path):
    source=tmp_path/'candidate.bin';source.write_bytes(image())
    releases=tmp_path/'releases';published=publish(source,releases)
    for raw in [image('0.2.0'),image('0.2.2', 'wrong-board'),image('0.2.2')[:-1],image('0.2.2')+b'padding']:
        source.write_bytes(raw)
        with pytest.raises(ValueError):publish(source,releases)
        assert json.loads((releases/'active.json').read_text())==published
    newer=image('0.2.2');source.write_bytes(newer)
    assert publish(source,releases)['version']=='0.2.2'


def test_checksum_and_digest_both_checked():
    raw=bytearray(image());raw[120]^=1
    # A fresh outer hash must not conceal a damaged XOR checksum.
    raw[-32:]=hashlib.sha256(raw[:-32]).digest()
    with pytest.raises(ValueError):inspect_image(raw)


def test_version_fits_device_manifest():
    with pytest.raises(ValueError):inspect_image(image('4294967295.4294967295.4294967295'))
    assert inspect_image(image('4294967295.4294967295.429496729'))['version']=='4294967295.4294967295.429496729'
    raw=bytearray(image());raw[-1]^=1
    with pytest.raises(ValueError):inspect_image(raw)


def test_reject_symlink_without_replacing_target(tmp_path):
    source=tmp_path/'candidate.bin';source.write_bytes(image())
    releases=tmp_path/'releases';releases.mkdir()
    victim=tmp_path/'important';victim.write_text('unchanged')
    (releases/'active.json').symlink_to(victim)
    with pytest.raises(ValueError):publish(source,releases)
    assert victim.read_text()=='unchanged'

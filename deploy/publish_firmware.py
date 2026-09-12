"""發布已驗證的應用程式映像；不連接裝置、不執行刷寫。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
from functools import reduce
from operator import xor
import tempfile

BOARD = 'xingzhi-cube-1.54tft-wifi'


def version_tuple(value):
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', value):
        raise ValueError('版本必須為三段數字')
    result = tuple(map(int, value.split('.')))
    if any(n > 0xffffffff for n in result):
        raise ValueError('版本數字超出範圍')
    return result


def inspect_image(raw):
    if not 288 <= len(raw) <= 0x400000:
        raise ValueError('映像大小不符合應用程式分區')
    if raw[0] != 0xe9 or int.from_bytes(raw[12:14], 'little') != 9:
        raise ValueError('必須是 ESP32-S3 應用程式映像')
    if int.from_bytes(raw[32:36], 'little') != 0xabcd5432:
        raise ValueError('找不到應用程式描述；不可發布整片備份或引導程式')
    # ESP image segments, aligned XOR checksum, then mandatory SHA-256 digest.
    if not 1 <= raw[1] <= 16 or raw[23] != 1:
        raise ValueError('映像必須包含有效分段與內建摘要')
    offset, checksum = 24, 0xef
    for _ in range(raw[1]):
        if offset + 8 > len(raw):
            raise ValueError('映像分段不完整')
        length = int.from_bytes(raw[offset+4:offset+8], 'little')
        offset += 8
        if length % 4 or offset + length > len(raw):
            raise ValueError('映像分段長度無效')
        checksum = reduce(xor, raw[offset:offset+length], checksum)
        offset += length
    checksum_at = offset + 15 - offset % 16
    digest_at = checksum_at + 1
    if digest_at + 32 != len(raw) or raw[checksum_at] != checksum:
        raise ValueError('映像校驗或尾端長度不符')
    if hashlib.sha256(raw[:digest_at]).digest() != raw[digest_at:]:
        raise ValueError('映像內建摘要不符')
    if b'\0' not in raw[48:80]:
        raise ValueError('版本欄位必須包含結尾字元')
    version = raw[48:80].split(b'\0', 1)[0].decode('ascii')
    board = raw[80:112].split(b'\0', 1)[0].decode('ascii')
    version_tuple(version)
    if board != BOARD:
        raise ValueError('映像機型不符')
    return dict(board=board, version=version, size=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def atomic_file(path, raw):
    fd, name = tempfile.mkstemp(prefix='.publish-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def publish(image, directory):
    raw = Path(image).read_bytes()
    manifest = inspect_image(raw)
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Serialize operator publishes across processes, keeping version order monotonic.
    import fcntl
    with (directory / '.publish.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        active = directory / 'active.json'
        if active.is_symlink():
            raise ValueError('發布清單不可使用符號連結')
        if active.exists():
            old = json.loads(active.read_text())
            if old == manifest:
                target = directory / (manifest['sha256'] + '.bin')
                if not target.is_symlink() and target.read_bytes() == raw:
                    return manifest
                raise ValueError('既有版本內容損毀，請先調查')
            if version_tuple(manifest['version']) <= version_tuple(old['version']):
                raise ValueError('新版必須高於目前發布版本')
        target = directory / (manifest['sha256'] + '.bin')
        if target.is_symlink():
            raise ValueError('映像不可使用符號連結')
        if target.exists():
            if target.read_bytes() != raw:
                raise ValueError('同摘要映像內容不一致')
        else:
            atomic_file(target, raw)
        atomic_file(active, (json.dumps(manifest, indent=2) + '\n').encode())
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--release-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = publish(args.image, args.release_dir)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'發布失敗：{error}\n')
    print(f"已發布 {result['version']}，摘要 {result['sha256']}。裝置需在本地更新頁手動啟動安裝。")

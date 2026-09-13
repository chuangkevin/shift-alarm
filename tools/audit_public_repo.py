#!/usr/bin/env python3
"""Fail when tracked files contain common private artifacts or credential shapes."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path


MAX_PUBLIC_FILE_SIZE = 4 * 1024 * 1024
ALLOWED_PRIVATE_TEMPLATES = {
    ".env.example",
    "firmware-next/main/provisioning.example.h",
}
SENSITIVE_PATH = re.compile(
    r"(^|/)(\.env($|\.)|provisioning\.h$|.*\.(pem|key|p12|pfx|bin|sqlite3?)$|"
    r"secrets?($|/)|credentials?($|/))",
    re.IGNORECASE,
)
SECRET_PATTERNS = (
    ("私鑰標頭", re.compile(b"-----BEGIN " + b"(?:[A-Z]+ )?PRIVATE KEY-----")),
    ("Tailscale 金鑰", re.compile(b"ts" + b"key-[A-Za-z0-9_-]{20,}")),
    ("GitHub token", re.compile(b"gh" + b"p_[A-Za-z0-9]{20,}")),
    ("GitHub token", re.compile(b"github" + b"_pat_[A-Za-z0-9_]{20,}")),
    ("Google API key", re.compile(b"AI" + b"za[0-9A-Za-z_-]{20,}")),
    ("AWS access key", re.compile(b"AK" + b"IA[0-9A-Z]{16}")),
    ("API key", re.compile(b"s" + b"k-[A-Za-z0-9_-]{20,}")),
    ("網址內嵌密碼", re.compile(rb"https?://[^\s/:]+:[^\s/@]+@")),
)
SECRET_ASSIGNMENT = re.compile(
    rb"(?im)^\s*(?:#define\s+)?"
    rb"(NEWAPI_KEY|DEVICE_TOKEN|PROVISION_TOKEN|TAILSCALE_AUTHKEY|AWS_SECRET_ACCESS_KEY)"
    rb"\s*(?:=|:)\s*[\"']?([^\"'\s#]+)"
)
PLACEHOLDER = re.compile(
    rb"replace|example|dummy|test|change|your|none|null|\$\{|getenv|environ",
    re.IGNORECASE,
)


def tracked_files() -> list[str]:
    output = subprocess.check_output(["git", "ls-files", "-z"])
    return [item.decode() for item in output.split(b"\0") if item]


def main() -> int:
    findings: list[str] = []
    for name in tracked_files():
        path = Path(name)
        if name not in ALLOWED_PRIVATE_TEMPLATES and SENSITIVE_PATH.search(name):
            findings.append(f"不應追蹤的私密檔名：{name}")
        if path.stat().st_size > MAX_PUBLIC_FILE_SIZE:
            findings.append(f"超過 4 MiB 的追蹤檔案：{name}")
        data = path.read_bytes()
        if b"\0" in data[:8192]:
            continue
        for label, pattern in SECRET_PATTERNS:
            if pattern.search(data):
                findings.append(f"疑似{label}：{name}")
        for key, value in SECRET_ASSIGNMENT.findall(data):
            if value and not PLACEHOLDER.search(value):
                findings.append(f"疑似 {key.decode()} 真值：{name}")

    if findings:
        print("公開儲存庫檢查失敗：")
        print("\n".join(f"- {item}" for item in findings))
        return 1
    print("公開儲存庫檢查通過")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

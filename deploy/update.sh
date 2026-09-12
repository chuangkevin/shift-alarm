#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
git pull --ff-only
docker compose -f deploy/docker-compose.yml up -d --build
curl --fail --silent --show-error --max-time 10 http://100.126.226.79:8237/api/health

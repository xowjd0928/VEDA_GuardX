#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP_DIR="${CAMERA_APP_DIR:-${ROOT_DIR}/camera_app/targeting/opensdk_26.05.19/CLI/test}"

APP_NAME="${APP_NAME:-test}"
SDK_VER="${SDK_VER:-26.05.19}"
SOC="${SOC:-cv5}"
ARTIFACT_NAME="${CI_CAMERA_ARTIFACT_NAME:-${APP_NAME}}"
ARTIFACT_DIR="${ROOT_DIR}/artifacts/ci/camera_app/${ARTIFACT_NAME}"
LOG_FILE="${ARTIFACT_DIR}/build_camera_app_${ARTIFACT_NAME}.log"

mkdir -p "${ARTIFACT_DIR}"

{
    echo "[camera_app] root=${ROOT_DIR}"
    echo "[camera_app] app_dir=${APP_DIR}"
    echo "[camera_app] app_name=${APP_NAME} sdk_ver=${SDK_VER} soc=${SOC}"
    echo "[camera_app] artifact_name=${ARTIFACT_NAME}"

    if ! command -v docker >/dev/null; then
        echo "[camera_app] docker command not found. Run this stage on the OpenSDK Docker build host." >&2
        exit 1
    fi

    if [ ! -f "${APP_DIR}/docker-compose.yml" ]; then
        echo "[camera_app] docker-compose.yml not found: ${APP_DIR}" >&2
        exit 1
    fi

    cd "${APP_DIR}"

    if docker compose version >/dev/null 2>&1; then
        APP_NAME="${APP_NAME}" SDK_VER="${SDK_VER}" SOC="${SOC}" docker compose run --rm opensdk
    elif command -v docker-compose >/dev/null; then
        APP_NAME="${APP_NAME}" SDK_VER="${SDK_VER}" SOC="${SOC}" docker-compose run --rm opensdk
    else
        echo "[camera_app] docker compose is not available" >&2
        exit 1
    fi

    find "${APP_DIR}" -maxdepth 2 -type f \
        \( -name '*.cap' -o -name '*.pkg' -o -name '*.tar.gz' \) \
        -exec cp -f {} "${ARTIFACT_DIR}/" \;

    echo "[camera_app] artifacts:"
    ls -al "${ARTIFACT_DIR}"
} 2>&1 | tee "${LOG_FILE}"

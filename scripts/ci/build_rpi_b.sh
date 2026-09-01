#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/ci/rpi_b"
ARTIFACT_DIR="${ROOT_DIR}/artifacts/ci/rpi_b"
LOG_FILE="${ARTIFACT_DIR}/build_rpi_b.log"

mkdir -p "${BUILD_DIR}" "${ARTIFACT_DIR}"

{
    echo "[rpi_b] root=${ROOT_DIR}"
    echo "[rpi_b] build_dir=${BUILD_DIR}"

    command -v cmake >/dev/null
    command -v c++ >/dev/null || command -v g++ >/dev/null

    cmake -S "${ROOT_DIR}/rpi_b" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

    test -x "${BUILD_DIR}/guardx_poller"
    test -x "${BUILD_DIR}/guardx_mqttd"

    cp -f "${BUILD_DIR}/guardx_poller" "${ARTIFACT_DIR}/"
    cp -f "${BUILD_DIR}/guardx_mqttd" "${ARTIFACT_DIR}/"

    echo "[rpi_b] artifacts:"
    ls -al "${ARTIFACT_DIR}"
} 2>&1 | tee "${LOG_FILE}"

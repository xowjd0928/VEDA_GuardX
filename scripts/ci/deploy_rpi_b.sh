#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARTIFACT_DIR="${ROOT_DIR}/artifacts/ci/rpi_b"
LOG_FILE="${ARTIFACT_DIR}/deploy_rpi_b.log"

DEPLOY_DIR="${RPI_B_DEPLOY_DIR:-/opt/guardx/bin}"
BACKUP_ROOT="${RPI_B_BACKUP_ROOT:-/opt/guardx/backups}"
POLLER_SERVICE="${RPI_B_POLLER_SERVICE:-guardx-poller}"
MQTTD_SERVICE="${RPI_B_MQTTD_SERVICE:-guardx-mqttd}"

mkdir -p "${ARTIFACT_DIR}"

{
    echo "[deploy_rpi_b] root=${ROOT_DIR}"
    echo "[deploy_rpi_b] artifact_dir=${ARTIFACT_DIR}"
    echo "[deploy_rpi_b] deploy_dir=${DEPLOY_DIR}"
    echo "[deploy_rpi_b] services=${POLLER_SERVICE} ${MQTTD_SERVICE}"

    test -x "${ARTIFACT_DIR}/guardx_poller"
    test -x "${ARTIFACT_DIR}/guardx_mqttd"

    if [ "$(id -u)" -eq 0 ]; then
        SUDO_CMD=""
    else
        SUDO_CMD="sudo -n"
    fi

    if ! ${SUDO_CMD} true >/dev/null 2>&1; then
        echo "[deploy_rpi_b] passwordless sudo is required for Jenkins deployment." >&2
        echo "[deploy_rpi_b] Add a sudoers rule for install, mkdir, cp, systemctl, and journalctl." >&2
        exit 1
    fi

    TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
    BACKUP_DIR="${BACKUP_ROOT}/${TIMESTAMP}"

    ${SUDO_CMD} install -d -m 0755 "${DEPLOY_DIR}"
    ${SUDO_CMD} install -d -m 0755 "${BACKUP_DIR}"

    if [ -f "${DEPLOY_DIR}/guardx_poller" ]; then
        ${SUDO_CMD} cp -f "${DEPLOY_DIR}/guardx_poller" "${BACKUP_DIR}/guardx_poller"
    fi

    if [ -f "${DEPLOY_DIR}/guardx_mqttd" ]; then
        ${SUDO_CMD} cp -f "${DEPLOY_DIR}/guardx_mqttd" "${BACKUP_DIR}/guardx_mqttd"
    fi

    ${SUDO_CMD} install -m 0755 "${ARTIFACT_DIR}/guardx_poller" "${DEPLOY_DIR}/guardx_poller"
    ${SUDO_CMD} install -m 0755 "${ARTIFACT_DIR}/guardx_mqttd" "${DEPLOY_DIR}/guardx_mqttd"

    ${SUDO_CMD} systemctl restart "${POLLER_SERVICE}"
    ${SUDO_CMD} systemctl restart "${MQTTD_SERVICE}"

    ${SUDO_CMD} systemctl is-active --quiet "${POLLER_SERVICE}"
    ${SUDO_CMD} systemctl is-active --quiet "${MQTTD_SERVICE}"

    echo "[deploy_rpi_b] ${POLLER_SERVICE} status:"
    ${SUDO_CMD} systemctl --no-pager --full status "${POLLER_SERVICE}" | sed -n '1,18p'

    echo "[deploy_rpi_b] ${MQTTD_SERVICE} status:"
    ${SUDO_CMD} systemctl --no-pager --full status "${MQTTD_SERVICE}" | sed -n '1,18p'

    echo "[deploy_rpi_b] deployment completed"
    echo "[deploy_rpi_b] backup_dir=${BACKUP_DIR}"
} 2>&1 | tee "${LOG_FILE}"

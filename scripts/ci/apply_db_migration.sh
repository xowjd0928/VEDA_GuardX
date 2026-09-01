#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARTIFACT_DIR="${ROOT_DIR}/artifacts/ci/database"
LOG_FILE="${ARTIFACT_DIR}/apply_db_migration.log"
MIGRATION_FILE="${DB_MIGRATION_FILE:-rpi_b/Database/migration_trajectory_segments.sql}"
MIGRATION_PATH="${ROOT_DIR}/${MIGRATION_FILE}"
MIGRATION_VERSION="$(basename "${MIGRATION_FILE}")"

mkdir -p "${ARTIFACT_DIR}"

{
    echo "[db_migration] root=${ROOT_DIR}"
    echo "[db_migration] migration_file=${MIGRATION_FILE}"

    if [ -z "${PGCONN:-}" ]; then
        echo "[db_migration] PGCONN is not set. Refusing to apply migration." >&2
        exit 1
    fi

    if ! command -v psql >/dev/null; then
        echo "[db_migration] psql command not found." >&2
        exit 1
    fi

    if [ ! -f "${MIGRATION_PATH}" ]; then
        echo "[db_migration] migration file not found: ${MIGRATION_PATH}" >&2
        exit 1
    fi

    psql "${PGCONN}" -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE IF NOT EXISTS schema_migrations (
    version text PRIMARY KEY,
    applied_at timestamptz NOT NULL DEFAULT now()
);
SQL

    if psql "${PGCONN}" -v version="${MIGRATION_VERSION}" -tAc \
        "SELECT 1 FROM schema_migrations WHERE version = :'version'" | grep -q 1; then
        echo "[db_migration] already applied: ${MIGRATION_VERSION}"
        exit 0
    fi

    psql "${PGCONN}" -v ON_ERROR_STOP=1 -f "${MIGRATION_PATH}"
    psql "${PGCONN}" -v ON_ERROR_STOP=1 -v version="${MIGRATION_VERSION}" \
        -c "INSERT INTO schema_migrations(version) VALUES (:'version')"

    echo "[db_migration] applied: ${MIGRATION_VERSION}"
} 2>&1 | tee "${LOG_FILE}"

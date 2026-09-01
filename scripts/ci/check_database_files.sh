#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATABASE_DIR="${ROOT_DIR}/rpi_b/Database"
ARTIFACT_DIR="${ROOT_DIR}/artifacts/ci/database"
LOG_FILE="${ARTIFACT_DIR}/check_database_files.log"

mkdir -p "${ARTIFACT_DIR}"

{
    echo "[database] database_dir=${DATABASE_DIR}"

    required_files=(
        "schema.sql"
        "migration_trajectory_segments.sql"
        "migration_track_handover_fields.sql"
        "migration_v15_feeds.sql"
        "migration_zones_multich.sql"
    )

    for file_name in "${required_files[@]}"; do
        file_path="${DATABASE_DIR}/${file_name}"
        if [[ ! -f "${file_path}" ]]; then
            echo "[database] missing required file: ${file_path}" >&2
            exit 1
        fi
        echo "[database] found ${file_name}"
    done

    grep -q "CREATE TABLE.*trajectory_segments" "${DATABASE_DIR}/migration_trajectory_segments.sql" \
        || grep -q "CREATE TABLE IF NOT EXISTS trajectory_segments" "${DATABASE_DIR}/migration_trajectory_segments.sql"

    grep -q "trajectory_segments" "${DATABASE_DIR}/schema.sql"
    grep -q "track_path" "${DATABASE_DIR}/schema.sql"
    grep -q "tracks" "${DATABASE_DIR}/schema.sql"

    echo "[database] schema and migration file checks passed"
} 2>&1 | tee "${LOG_FILE}"

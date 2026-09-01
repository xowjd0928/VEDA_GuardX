# GuardX Jenkins CI

This directory contains build scripts used by `Jenkinsfile`.
Each script can also be executed manually from the repository root.

## Linux agent

Required packages for `rpi_b`:

```bash
sudo apt install cmake g++ libcurl4-openssl-dev nlohmann-json3-dev \
    libpqxx-dev libmosquitto-dev libssl-dev
```

Build RPi services:

```bash
bash scripts/ci/build_rpi_b.sh
```

Deploy RPi services after a successful build:

```bash
bash scripts/ci/deploy_rpi_b.sh
```

The deploy script installs `guardx_poller` and `guardx_mqttd`, restarts the
configured systemd services, and checks that both services are active.

Default deploy settings:

```bash
RPI_B_DEPLOY_DIR=/home/juan/7th_VEDA_GROUP2/rpi_b/build
RPI_B_BACKUP_ROOT=/opt/guardx/backups
RPI_B_POLLER_SERVICE=guardx-poller
RPI_B_MQTTD_SERVICE=guardx-mqttd
```

If the actual systemd service names are different, set the matching
environment variables in Jenkins or before running the script.

The Jenkins agent user needs passwordless sudo for deployment commands.
Without it, deployment intentionally fails instead of waiting for a password.

Check database schema/migration files:

```bash
bash scripts/ci/check_database_files.sh
```

Apply one database migration:

```bash
PGCONN='host=localhost dbname=guardx user=guardx_admin password=...' \
DB_MIGRATION_FILE=rpi_b/Database/migration_trajectory_segments.sql \
bash scripts/ci/apply_db_migration.sh
```

Migrations are tracked in `schema_migrations` to avoid applying the same file
twice from Jenkins.

Build OpenSDK camera application:

```bash
APP_NAME=test SDK_VER=26.05.19 SOC=cv5 bash scripts/ci/build_camera_app.sh
```

The OpenSDK build requires Docker and the `opensdk:${SDK_VER}` image.

## Windows agent

Required tools for VMS:

- Visual Studio Build Tools / MSVC
- Qt 6.11.1 MSVC kit
- CMake
- Optional: vcpkg toolchain for OpenCV/GStreamer dependencies

Build VMS:

```powershell
.\scripts\ci\build_vms_windows.ps1
```

Optional environment variables:

```powershell
$env:CMAKE_EXE = "C:\Qt\Tools\CMake_64\bin\cmake.exe"
$env:QT_PREFIX_PATH = "C:\Qt\6.11.1\msvc2022_64"
$env:CMAKE_TOOLCHAIN_FILE = "C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
```

## Jenkins stages

- `Database Files`: validates important DB schema/migration files.
- `Apply DB Migration`: optionally applies one migration file using `PGCONN`.
- `Build rpi_b`: builds `guardx_poller` and `guardx_mqttd`.
- `Deploy rpi_b`: optionally deploys RPi B binaries and restarts systemd services.
- `Build OpenSDK Camera App`: builds and packages the camera CAP with Docker.
- `Build VMS`: builds `gstream_VMS.exe` on Windows agents.

Recommended safe defaults:

```text
DEPLOY_RPI_B=false
APPLY_DB_MIGRATION=false
RPI_B_DEPLOY_DIR=/home/juan/7th_VEDA_GROUP2/rpi_b/build
RPI_B_POLLER_SERVICE=guardx-poller
RPI_B_MQTTD_SERVICE=guardx-mqttd
```

Enable `DEPLOY_RPI_B` only when the latest RPi B binaries should be applied to
the running device.

Enable `APPLY_DB_MIGRATION` only when the selected migration file should be
applied to the configured PostgreSQL database.

Build outputs are copied into:

```text
artifacts/ci/
```

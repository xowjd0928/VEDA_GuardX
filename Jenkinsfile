pipeline {
    agent none

    options {
        timestamps()
        disableConcurrentBuilds()
        buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
    }

    parameters {
        booleanParam(
            name: 'CHECK_DATABASE',
            defaultValue: true,
            description: 'Check database schema and migration files on the rpi-b agent.'
        )

        booleanParam(
            name: 'APPLY_DB_MIGRATION',
            defaultValue: false,
            description: 'Apply one database migration using PGCONN on the rpi-b agent. Use carefully.'
        )

        string(
            name: 'DB_MIGRATION_FILE',
            defaultValue: 'rpi_b/Database/migration_trajectory_segments.sql',
            description: 'Migration file to apply when APPLY_DB_MIGRATION is enabled.'
        )

        booleanParam(
            name: 'BUILD_RPI_B',
            defaultValue: true,
            description: 'Build rpi_b poller and mqtt daemon on RPi B agent.'
        )

        booleanParam(
            name: 'DEPLOY_RPI_B',
            defaultValue: false,
            description: 'Deploy rpi_b binaries and restart systemd services. Use carefully.'
        )

        string(
            name: 'RPI_B_DEPLOY_DIR',
            defaultValue: '/home/juan/7th_VEDA_GROUP2/rpi_b/build',
            description: 'Target directory for RPi B deployment.'
        )

        string(
            name: 'RPI_B_POLLER_SERVICE',
            defaultValue: 'guardx-poller',
            description: 'systemd service name for guardx_poller.'
        )

        string(
            name: 'RPI_B_MQTTD_SERVICE',
            defaultValue: 'guardx-mqttd',
            description: 'systemd service name for guardx_mqttd.'
        )

        booleanParam(
            name: 'BUILD_CAMERA_APP',
            defaultValue: true,
            description: 'Build OpenSDK camera application through Docker on OpenSDK Linux agent.'
        )

        booleanParam(
            name: 'BUILD_VMS',
            defaultValue: true,
            description: 'Build Qt VMS on Windows agent.'
        )
    }

    environment {
        CI_ARTIFACT_DIR = 'artifacts/ci'
        OPENCV_SKIP_TESTS = '1'
    }

    stages {
        stage('Check Database Files') {
            agent {
                label 'rpi-b'
            }

            when {
                expression { return params.CHECK_DATABASE }
            }

            steps {
                checkout scm

                sh '''
                    set -eu
                    mkdir -p "$CI_ARTIFACT_DIR/database"
                    bash scripts/ci/check_database_files.sh
                '''
            }

            post {
                always {
                    archiveArtifacts artifacts: 'artifacts/ci/database/**', allowEmptyArchive: true
                    archiveArtifacts artifacts: 'artifacts/ci/**/*.log', allowEmptyArchive: true
                }
            }
        }

        stage('Apply DB Migration') {
            agent {
                label 'rpi-b'
            }

            when {
                expression { return params.APPLY_DB_MIGRATION }
            }

            steps {
                checkout scm

                sh '''
                    set -eu
                    mkdir -p "$CI_ARTIFACT_DIR/database"
                    DB_MIGRATION_FILE="${DB_MIGRATION_FILE}" bash scripts/ci/apply_db_migration.sh
                '''
            }

            post {
                always {
                    archiveArtifacts artifacts: 'artifacts/ci/database/**', allowEmptyArchive: true
                    archiveArtifacts artifacts: 'artifacts/ci/**/*.log', allowEmptyArchive: true
                }
            }
        }

        stage('Build rpi_b') {
            agent {
                label 'rpi-b'
            }

            when {
                expression { return params.BUILD_RPI_B }
            }

            steps {
                checkout scm

                sh '''
                    set -eu
                    mkdir -p "$CI_ARTIFACT_DIR/rpi_b"
                    bash scripts/ci/build_rpi_b.sh
                '''
            }

            post {
                always {
                    archiveArtifacts artifacts: 'artifacts/ci/rpi_b/**', allowEmptyArchive: true
                    archiveArtifacts artifacts: 'artifacts/ci/**/*.log', allowEmptyArchive: true
                }
            }
        }

        stage('Deploy rpi_b') {
            agent {
                label 'rpi-b'
            }

            when {
                expression { return params.DEPLOY_RPI_B }
            }

            steps {
                checkout scm

                sh '''
                    set -eu

                    if [ "${BUILD_RPI_B}" != "true" ]; then
                        echo "[deploy_rpi_b] DEPLOY_RPI_B requires BUILD_RPI_B=true in the same build."
                        exit 1
                    fi

                    RPI_B_DEPLOY_DIR="${RPI_B_DEPLOY_DIR}" \
                    RPI_B_POLLER_SERVICE="${RPI_B_POLLER_SERVICE}" \
                    RPI_B_MQTTD_SERVICE="${RPI_B_MQTTD_SERVICE}" \
                    bash scripts/ci/deploy_rpi_b.sh
                '''
            }

            post {
                always {
                    archiveArtifacts artifacts: 'artifacts/ci/rpi_b/**', allowEmptyArchive: true
                    archiveArtifacts artifacts: 'artifacts/ci/**/*.log', allowEmptyArchive: true
                }
            }
        }

        stage('Build OpenSDK Camera App - Tracking') {
            agent {
                label 'opensdk-linux'
            }

            when {
                expression { return params.BUILD_CAMERA_APP }
            }

            steps {
                checkout scm

                sh '''
                    set -eu
                    APP_NAME=test \
                    SDK_VER=26.05.19 \
                    SOC=cv5 \
                    CI_CAMERA_ARTIFACT_NAME=tracking \
                    bash scripts/ci/build_camera_app.sh
                '''
            }

            post {
                always {
                    archiveArtifacts artifacts: 'artifacts/ci/camera_app/tracking/**', allowEmptyArchive: true
                    archiveArtifacts artifacts: 'artifacts/ci/**/*.log', allowEmptyArchive: true
                }
            }
        }

        stage('Build OpenSDK Camera App - Juan') {
            agent {
                label 'opensdk-linux'
            }

            when {
                expression { return params.BUILD_CAMERA_APP }
            }

            steps {
                checkout scm

                sh '''
                    set -eu
                    CAMERA_APP_DIR="${WORKSPACE}/camera_app/juan_application" \
                    APP_NAME=juan_application \
                    SDK_VER=26.05.19 \
                    SOC=cv5 \
                    CI_CAMERA_ARTIFACT_NAME=juan_application \
                    bash scripts/ci/build_camera_app.sh
                '''
            }

            post {
                always {
                    archiveArtifacts artifacts: 'artifacts/ci/camera_app/juan_application/**', allowEmptyArchive: true
                    archiveArtifacts artifacts: 'artifacts/ci/**/*.log', allowEmptyArchive: true
                }
            }
        }

        stage('Build VMS') {
            agent {
                label 'windows'
            }

            when {
                expression { return params.BUILD_VMS }
            }

            steps {
                checkout scm

                powershell '''
                    powershell -ExecutionPolicy Bypass -File .\\scripts\\ci\\build_vms_windows.ps1
                '''
            }

            post {
                always {
                    archiveArtifacts artifacts: 'artifacts/ci/vms/**', allowEmptyArchive: true
                    archiveArtifacts artifacts: 'artifacts/ci/**/*.log', allowEmptyArchive: true
                }
            }
        }
    }

    post {
        success {
            echo 'GuardX CI succeeded.'
        }

        failure {
            echo 'GuardX CI failed. Check the failed stage log above.'
        }
    }
}

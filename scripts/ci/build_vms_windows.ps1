$ErrorActionPreference = "Stop"

$rootDir = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$buildDir = if ($env:VMS_BUILD_DIR) {
    $env:VMS_BUILD_DIR
} else {
    Join-Path $rootDir "build\ci\vms"
}
$artifactDir = Join-Path $rootDir "artifacts\ci\vms"
$logFile = Join-Path $artifactDir "build_vms_windows.log"
$packageName = if ($env:VMS_PACKAGE_NAME) {
    $env:VMS_PACKAGE_NAME
} else {
    "GuardX_VMS_windows"
}
$packageDir = Join-Path $artifactDir $packageName
$packageAppDir = Join-Path $packageDir "app"
$packageZip = Join-Path $artifactDir "$packageName.zip"

New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null

function Find-CommandPath {
    param(
        [string] $EnvValue,
        [string[]] $Candidates,
        [string] $CommandName
    )

    if ($EnvValue -and (Test-Path $EnvValue)) {
        return $EnvValue
    }

    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "$CommandName was not found. Set the required environment variable or add it to PATH."
}

function Invoke-CiCommand {
    param(
        [string] $CommandLine,
        [string] $LogFile
    )

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"

    try {
        & cmd.exe /d /c $CommandLine 2>&1 | ForEach-Object {
            "$_"
        } | Tee-Object -FilePath $LogFile -Append
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if ($exitCode -ne 0) {
        throw "Command failed with exit code $exitCode`: $CommandLine"
    }
}

$cmakeExe = Find-CommandPath `
    -EnvValue $env:CMAKE_EXE `
    -Candidates @(
        "C:\Qt\Tools\CMake_64\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    ) `
    -CommandName "cmake.exe"

$vsDevCmd = Find-CommandPath `
    -EnvValue $env:VS_DEV_CMD `
    -Candidates @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    ) `
    -CommandName "VsDevCmd.bat"

$makeProgramExe = Find-CommandPath `
    -EnvValue $env:CMAKE_MAKE_PROGRAM `
    -Candidates @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64\nmake.exe",
        "C:\Qt\Tools\Ninja\ninja.exe",
        "C:\Qt\Tools\QtCreator\bin\jom\jom.exe"
    ) `
    -CommandName "nmake.exe"

$cxxCompiler = Find-CommandPath `
    -EnvValue $env:CMAKE_CXX_COMPILER `
    -Candidates @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64\cl.exe"
    ) `
    -CommandName "cl.exe"

$cCompiler = if ($env:CMAKE_C_COMPILER -and (Test-Path $env:CMAKE_C_COMPILER)) {
    $env:CMAKE_C_COMPILER
} else {
    $cxxCompiler
}

$qtPrefixPath = if ($env:QT_PREFIX_PATH) {
    $env:QT_PREFIX_PATH
} else {
    "C:\Qt\6.11.1\msvc2022_64"
}

$windeployqtExe = Find-CommandPath `
    -EnvValue $env:WINDEPLOYQT_EXE `
    -Candidates @(
        (Join-Path $qtPrefixPath "bin\windeployqt.exe")
    ) `
    -CommandName "windeployqt.exe"

$cmakeGenerator = if ($env:CMAKE_GENERATOR) {
    $env:CMAKE_GENERATOR
} else {
    "NMake Makefiles"
}

$cmakeArgs = @(
    "-G", $cmakeGenerator,
    "-S", (Join-Path $rootDir "vms"),
    "-B", $buildDir,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$qtPrefixPath",
    "-DCMAKE_MAKE_PROGRAM=$makeProgramExe",
    "-DCMAKE_CXX_COMPILER=$cxxCompiler",
    "-DCMAKE_C_COMPILER=$cCompiler",
    "-DCMAKE_CXX_COMPILER_WORKS=TRUE",
    "-DCMAKE_C_COMPILER_WORKS=TRUE",
    "-DCMAKE_AUTOGEN_PARALLEL=1"
)

if ($env:CMAKE_TOOLCHAIN_FILE) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:CMAKE_TOOLCHAIN_FILE"
}

$quotedCmakeArgs = ($cmakeArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join " "
$configureCommand = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && where cl && `"$cmakeExe`" $quotedCmakeArgs"
$buildCommand = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && where cl && `"$cmakeExe`" --build `"$buildDir`" --config Release --target gstream_VMS"

try {
    if ($env:VMS_CLEAN_BUILD -ne "0" -and (Test-Path $buildDir)) {
        Remove-Item -Recurse -Force $buildDir
    }

    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    "[$(Get-Date -Format s)] [vms] root=$rootDir" | Tee-Object -FilePath $logFile
    "[$(Get-Date -Format s)] [vms] build_dir=$buildDir" | Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] qt_prefix=$qtPrefixPath" | Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] generator=$cmakeGenerator" | Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] vs_dev_cmd=$vsDevCmd" | Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] make_program=$makeProgramExe" | Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] cxx_compiler=$cxxCompiler" | Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] windeployqt=$windeployqtExe" | Tee-Object -FilePath $logFile -Append

    Invoke-CiCommand -CommandLine $configureCommand -LogFile $logFile
    Invoke-CiCommand -CommandLine $buildCommand -LogFile $logFile

    $exe = Get-ChildItem -Path $buildDir -Recurse -Filter "gstream_VMS.exe" |
        Select-Object -First 1
    if (-not $exe) {
        throw "gstream_VMS.exe was not found under $buildDir"
    }

    $legacyStandaloneExe = Join-Path $artifactDir $exe.Name
    if (Test-Path $legacyStandaloneExe) {
        Remove-Item -Force $legacyStandaloneExe
    }

    "[$(Get-Date -Format s)] [vms] build_artifact=$($exe.FullName)" |
        Tee-Object -FilePath $logFile -Append

    if (Test-Path $packageDir) {
        Remove-Item -Recurse -Force $packageDir
    }
    if (Test-Path $packageZip) {
        Remove-Item -Force $packageZip
    }

    New-Item -ItemType Directory -Force -Path $packageAppDir | Out-Null
    Copy-Item -Force $exe.FullName $packageAppDir

    $deployedExe = Join-Path $packageAppDir $exe.Name
    $windeployCommand = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && `"$windeployqtExe`" --release --compiler-runtime --no-translations --dir `"$packageAppDir`" `"$deployedExe`""
    Invoke-CiCommand -CommandLine $windeployCommand -LogFile $logFile

    $extraDllDirs = @()
    if ($env:VMS_EXTRA_DLL_DIRS) {
        $extraDllDirs += ($env:VMS_EXTRA_DLL_DIRS -split ';')
    }

    $defaultDllDirs = @(
        "C:\vcpkg\installed\x64-windows\bin",
        "C:\gstreamer\1.0\msvc_x86_64\bin",
        "C:\Program Files\mosquitto",
        "C:\Program Files\VideoLAN\VLC",
        (Join-Path $qtPrefixPath "bin")
    )

    $extraDllDirs += $defaultDllDirs
    $copiedDllCount = 0

    foreach ($dllDir in ($extraDllDirs | Where-Object { $_ } | Select-Object -Unique)) {
        if (-not (Test-Path $dllDir)) {
            "[$(Get-Date -Format s)] [vms] optional_dll_dir_missing=$dllDir" |
                Tee-Object -FilePath $logFile -Append
            continue
        }

        $dllFiles = Get-ChildItem -Path $dllDir -Filter "*.dll" -File -ErrorAction SilentlyContinue
        foreach ($dll in $dllFiles) {
            $targetDll = Join-Path $packageAppDir $dll.Name
            if (-not (Test-Path $targetDll)) {
                Copy-Item -Force $dll.FullName $targetDll
                $copiedDllCount += 1
            }
        }
    }

    $gstreamerPluginCandidates = @(
        "C:\gstreamer\1.0\msvc_x86_64\lib\gstreamer-1.0"
    )
    $gstreamerPluginDir = Join-Path $packageAppDir "gstreamer\plugins"
    $copiedGstreamerPluginCount = 0

    foreach ($pluginSourceDir in $gstreamerPluginCandidates) {
        if (-not (Test-Path $pluginSourceDir)) {
            "[$(Get-Date -Format s)] [vms] optional_gstreamer_plugin_dir_missing=$pluginSourceDir" |
                Tee-Object -FilePath $logFile -Append
            continue
        }

        New-Item -ItemType Directory -Force -Path $gstreamerPluginDir | Out-Null
        $pluginFiles = Get-ChildItem -Path $pluginSourceDir -Filter "*.dll" -File -ErrorAction SilentlyContinue
        foreach ($plugin in $pluginFiles) {
            Copy-Item -Force $plugin.FullName (Join-Path $gstreamerPluginDir $plugin.Name)
            $copiedGstreamerPluginCount += 1
        }
    }

    $launcherPath = Join-Path $packageDir "run_guardx_vms.bat"
    @(
        "@echo off",
        "set APP_DIR=%~dp0app\\",
        "set PATH=%APP_DIR%;%APP_DIR%gstreamer\\plugins;%PATH%",
        "set GST_PLUGIN_PATH=%APP_DIR%gstreamer\\plugins",
        'start "" "%APP_DIR%' + $exe.Name + '" %*'
    ) | Set-Content -Encoding ASCII -Path $launcherPath

    $readmePath = Join-Path $packageDir "README.txt"
    @(
        "GuardX VMS Windows Package",
        "",
        "실행 방법:",
        "1. 이 폴더를 원하는 위치에 압축 해제합니다.",
        "2. run_guardx_vms.bat 파일을 실행합니다.",
        "",
        "주의:",
        "- app 폴더 안의 gstream_VMS.exe와 DLL 파일들은 실행에 필요한 런타임 파일입니다.",
        "- 사용자는 run_guardx_vms.bat만 실행하면 됩니다.",
        "- app 폴더 내부 파일을 삭제하거나 이동하면 실행되지 않을 수 있습니다."
    ) | Set-Content -Encoding UTF8 -Path $readmePath

    "[$(Get-Date -Format s)] [vms] extra_dlls_copied=$copiedDllCount" |
        Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] gstreamer_plugins_copied=$copiedGstreamerPluginCount" |
        Tee-Object -FilePath $logFile -Append

    Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $packageZip -Force
    "[$(Get-Date -Format s)] [vms] package_dir=$packageDir" |
        Tee-Object -FilePath $logFile -Append
    "[$(Get-Date -Format s)] [vms] package_zip=$packageZip" |
        Tee-Object -FilePath $logFile -Append
} catch {
    "[$(Get-Date -Format s)] [vms] ERROR: $_" | Tee-Object -FilePath $logFile -Append
    throw
}

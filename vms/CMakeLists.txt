cmake_minimum_required(VERSION 3.19)
project(gstream_VMS LANGUAGES CXX)

# GPU 렌더링(QRhiWidget)은 Qt 6.7+ 필요. GuiPrivate는 QRhi 타입 때문.
find_package(Qt6 6.7 REQUIRED COMPONENTS
    Core
    Widgets
    Gui
    GuiPrivate
    Multimedia
    MultimediaWidgets
    Network
    ShaderTools
)

qt_standard_project_setup()

qt_add_executable(gstream_VMS
    WIN32 MACOSX_BUNDLE
    main.cpp
    theme.h theme.cpp
    top_bar.h top_bar.cpp
    nav_rail.h nav_rail.cpp
    placeholder_page.h
    panel_chrome.h panel_chrome.cpp
    edge_map_generator.h edge_map_generator.cpp
    mainwindow.h mainwindow.cpp
    live_viewer.h live_viewer.cpp
    tracking_panel.h tracking_panel.cpp
    business_flow_page.h business_flow_page.cpp
    track_history.h track_history.cpp
    track_display_link.h track_display_link.cpp
    crowd_page.h crowd_page.cpp
    floor_plan.h floor_plan.cpp
    crowd_analytics_page.h crowd_analytics_page.cpp
    calibration_store.h calibration_store.cpp
    device_control_page.h device_control_page.cpp
    zone_sensor_store.h zone_sensor_store.cpp
    zone_display_link.h zone_display_link.cpp
    fire_charts.h fire_charts.cpp
    sensor_fields.h
    fire_zone_map.h
    zone_config.h zone_config.cpp
    channel_view.h channel_view.cpp
    box_source.h box_source.cpp
    detection_feed.h detection_feed.cpp
    onvif_meta_source.h onvif_meta_source.cpp
    prediction_feed.h prediction_feed.cpp
    # report_page(Predictions)·business_flow_page(Flow)는 08-20에 탭에서
    # 빠졌다(Analytics가 대신한다). **목록에는 남겨 둔다** — 계속 컴파일돼야
    # 되살릴 때 썩어 있지 않고, analytics_page가 두 헤더의 자료형을 쓴다.
    report_page.h report_page.cpp
    report_chart.h report_chart.cpp
    analytics_page.h analytics_page.cpp
    alert_feed.h alert_feed.cpp
    alert_time.h
    alert_popup_stack.h alert_popup_stack.cpp
    alert_popup.h alert_popup.cpp
    fire_alert_feed.h fire_alert_feed.cpp
    fire_alert_popup.h fire_alert_popup.cpp
    rpi_alert_popup.h rpi_alert_popup.cpp
    audio_alert_card.h audio_alert_card.cpp
    audio_alert_center.h audio_alert_center.cpp
    wall_layout.h wall_layout.cpp
    video_backend.h video_backend.cpp
    qmediaplayer_backend.h qmediaplayer_backend.cpp
    camera_tuner.h camera_tuner.cpp
    camera_status.h camera_status.cpp
    camera_page.h camera_page.cpp
    camera_control.h camera_control.cpp
    app_card.h app_card.cpp
    system_panel.h system_panel.cpp
    profile_panel.h profile_panel.cpp
    image_panel.h image_panel.cpp
    network_panel.h network_panel.cpp
    credentials.h credentials.cpp
    auth.h auth.cpp
    login_page.h login_page.cpp
    site_config.h site_config.cpp
    site_settings_card.h site_settings_card.cpp
    account_settings_card.h account_settings_card.cpp
    # 이벤트 클립 저장 (VEDA-201) — recorder 의 GStreamer 의존부는
    # HAVE_GSTREAMER 가드 안이라 GST 없는 빌드에서도 목록에 남는다
    clip_recorder.h clip_recorder.cpp
    storage_settings_card.h storage_settings_card.cpp
    reauth_dialog.h reauth_dialog.cpp
    sunapi_request.h
    zone_settings_page.h zone_settings_page.cpp
    channel_sync.h channel_sync.cpp
    mqtt_link.h mqtt_link.cpp
)

# Live broadcast stays isolated from the shared DeviceControlPage implementation.
target_sources(gstream_VMS PRIVATE
    broadcast_controller.h broadcast_controller.cpp
    broadcast_control_row.h broadcast_control_row.cpp
    broadcast_rtp_sender.h broadcast_rtp_sender.cpp
    broadcast_pipeline.h broadcast_pipeline.cpp
)
target_include_directories(gstream_VMS PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

# =====================================================================
# IBM Plex 번들 (핸드오프 §Assets) — 폰트 설치를 배포 조건에서 뺀다
#
# 굵기별 **정적** face를 각각 넣는다 (가변 폰트 아님 — Qt가 중간 굵기를
# 못 살린다). 코드가 실제로 쓰는 굵기만:
#   Sans 400/500/600/700  ·  Mono 400/600
# 등록·병합은 Theme::load_bundled_fonts() 참조.
#
# 파일이 없으면 그 face만 빼고 빌드하고(경고), 런타임에는 시스템 폰트로
# 폴백한다 — 폰트가 없다고 빌드를 막을 이유는 없다. 나중에 fonts/ 에
# 파일을 넣고 **CMake를 다시 실행**하면 그대로 잡힌다.
# 출처: github.com/IBM/plex (OFL) — fonts/complete/ttf 의 정적 파일.
# =====================================================================
set(_plex_fonts
    fonts/IBMPlexSans-Regular.ttf
    fonts/IBMPlexSans-Medium.ttf
    fonts/IBMPlexSans-SemiBold.ttf
    fonts/IBMPlexSans-Bold.ttf
    fonts/IBMPlexMono-Regular.ttf
    fonts/IBMPlexMono-SemiBold.ttf
)
set(_plex_present "")
foreach(_f ${_plex_fonts})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        list(APPEND _plex_present "${_f}")
    else()
        message(STATUS "번들 폰트 없음 (시스템 폰트로 폴백): ${_f}")
    endif()
endforeach()
if(_plex_present)
    # ⚠ BASE 를 빼면 안 된다. FILES 의 경로가 alias 에 그대로 남아
    # 리소스가 ":/fonts/fonts/IBMPlexSans-Regular.ttf" 로 들어가는데,
    # Theme::load_bundled_fonts() 는 ":/fonts" **바로 아래**만 훑는다
    # (QDir::Files, 재귀 없음) → 한 개도 못 찾고 조용히 시스템 폰트로 폴백한다.
    # 08-03 에 폰트를 넣은 뒤로 08-10 까지 그 상태였다 — 로그의
    # "[Theme] 번들 폰트 없음"이 유일한 흔적이었다.
    qt_add_resources(gstream_VMS "vms_fonts"
        PREFIX "/fonts"
        BASE "fonts"
        FILES ${_plex_present}
    )
endif()

# =====================================================================
# 마스코트 도담 (:/img/dodam_mascot.png)
#
# 리포에 들어가는 유일한 이미지 자산이다. 공유폴더의 896x1195 원본을
# **576x768 · 8bit 로 줄인 것**만 넣는다(96KB) — 최대 표시 폭 220px 의
# 2배(HiDPI)를 덮는 크기고, 리소스는 실행 파일에 통째로 들어가므로 원본
# 1.6MB 를 그대로 넣을 이유가 없다.
#
# ⚠ 원본은 투명 배경이 **아니다** — alpha=255 전면 불투명에 체커 무늬가
# 그려져 있다. 알파 컷·중앙 정렬·축소를 한 번에 하는 재가공 스크립트는
# 공유폴더 make_mascot_asset.py. 다시 만들 때 반드시 그걸 거칠 것.
#
# ⚠ BASE 를 빼면 안 된다 — 위 폰트 블록과 똑같은 함정이다. 빼면 별칭이
# ":/img/assets/dodam_mascot.png" 가 되어 Theme::mascot() 이 못 찾는다.
# =====================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/dodam_mascot.png")
    qt_add_resources(gstream_VMS "vms_img"
        PREFIX "/img"
        BASE "assets"
        FILES assets/dodam_mascot.png
    )
else()
    message(WARNING "assets/dodam_mascot.png 없음 — 마스코트를 쓰는 자리는 비어서 그려집니다 (Theme::mascot()이 null 반환)")
endif()

# ---------------------------------------------------------------------
# 현장 프롭 아트워크 (:/img/props/*.png) — CROWD 평면도의 가구에 입힌다.
# 출처는 현장 프롭 인쇄 원본(TOP_FRONT.pdf)의 면 아트워크를 화면 크기에
# 맞춰 줄인 것: 세로로 긴 가구는 FRONT(진열장), 가로로 긴 가구는 TOP(카운터).
# ⚠ BASE "assets" 를 빼면 별칭이 ":/img/assets/props/..." 가 된다 (위 함정과 동일).
file(GLOB _prop_art RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "assets/props/*.png")
if(_prop_art)
    qt_add_resources(gstream_VMS "vms_props"
        PREFIX "/img"
        BASE "assets"
        FILES ${_prop_art}
    )
else()
    message(WARNING "assets/props/*.png 없음 — 평면도 가구는 옛 단색 상자로 그려집니다")
endif()

# =====================================================================
# 신뢰 카메라 인증서 (:/certs/*.pem)
#
# 실행 파일 안에 넣는다 — 설치 경로·작업 디렉터리에 의존하지 않게. 팀원은
# pull 만 하면 되고 지문을 손으로 옮겨 적을 필요가 없다. 인증서는 공개
# 정보라 리포에 있어도 새는 비밀이 없다. 근거·갱신법은
# docs/SECURITY_SETUP.md 와 certs/*.pem 머리말 참조.
file(GLOB _camera_certs RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "certs/*.pem")
if(_camera_certs)
    qt_add_resources(gstream_VMS "vms_certs"
        PREFIX "/"
        FILES ${_camera_certs}
    )
else()
    message(WARNING "certs/*.pem 없음 — TLS 핀이 비어 카메라 HTTPS 요청이 전부 거부됩니다 (docs/SECURITY_SETUP.md)")
endif()

# =====================================================================
# MinGW: 수학 함수의 errno 설정을 끈다 (-fno-math-errno)
#
# 없으면 GCC가 sqrt(x)를 "sqrtsd 명령 + 음수일 때 라이브러리 호출" 두 갈래로
# 낸다. 그 폴백 호출이 msvcrt.dll 의 sqrt 로 auto-import 되면서 링커가
# 32비트 의사 재배치 썽크(__fu0_sqrt …)를 만드는데, x64에서는 exe와 DLL이
# ASLR로 4GB 넘게 떨어지면 시작 직후 abort 한다 (종료 코드 3):
#   Mingw-w64 runtime failure: 32 bit pseudo relocation at … out of range
# 주소 배치 운에 따라 떴다 안 떴다 하므로(실측: 12회 중 0회 → 사용자 환경에서
# 재현) 더 고약하다. 우리는 errno를 읽지 않으므로 의미상 손실이 없다.
# 2026-07-30 실측·수정 (report_chart.cpp 의 예측축 √스케일이 유발).
#
# --disable-auto-import 는 같은 사고를 런타임 abort 대신 **링크 에러**로
# 바꾸는 안전망이다 — 다시 생기면 빌드에서 바로 걸린다.
# =====================================================================
if(MINGW)
    target_compile_options(gstream_VMS PRIVATE -fno-math-errno)
    target_link_options(gstream_VMS PRIVATE -Wl,--disable-auto-import)
endif()

# =====================================================================
# MSVC 병렬 빌드(--parallel) + 공유 PDB 쓰기 경쟁 방지
#
# 여러 소스가 동시에 컴파일되면서 같은 .pdb에 디버그 정보를 쓰려 하면
# mspdbsrv.exe 가 응답을 놓쳐 C1090("PDB API를 호출하지 못했습니다")로
# 죽는 경우가 있다. /FS 는 PDB 쓰기를 프로세스 간 동기화해 이 경쟁을
# 없앤다(병렬 빌드 속도는 거의 그대로 유지).
# =====================================================================
if(MSVC)
    target_compile_options(gstream_VMS PRIVATE /FS)
endif()

target_link_libraries(gstream_VMS
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Gui
        Qt::GuiPrivate
        Qt::Multimedia
        Qt::MultimediaWidgets
        Qt::Network
)

# DPAPI(CryptProtectData)로 자격 파일의 비밀값을 사용자 계정에 묶어 암호화
if(WIN32)
    target_link_libraries(gstream_VMS PRIVATE crypt32)
endif()

# =====================================================================
# libmosquitto (MQTT). 발견되면 실제 브로커에 붙고, 없으면 스텁으로 빌드된다.
#
# v6부터 VMS의 데이터 경로는 MQTT 하나뿐이다(DB 직결 제거). 스텁으로 빌드하면
# 빌드·실행은 되지만 정원·날짜목록·히트맵이 비어 있으므로, 배포본에는
# libmosquitto가 반드시 있어야 한다.
#
# Windows 설치법:
#   mosquitto.org/download 에서 "mosquitto-2.x.x-install-windows-x64.exe"
#   설치 (기본 경로 C:/Program Files/mosquitto). devel\ 아래에 mosquitto.h 와
#   mosquitto.lib 가 함께 깔린다. 실행 시 mosquitto.dll 이 PATH 또는
#   exe 옆에 있어야 한다 (아래 POST_BUILD가 자동 복사).
#   vcpkg를 쓰면: vcpkg install mosquitto:x64-windows
# =====================================================================
set(MOSQ_ENABLED FALSE)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(MOSQ IMPORTED_TARGET QUIET libmosquitto)
    if(MOSQ_FOUND)
        target_link_libraries(gstream_VMS PRIVATE PkgConfig::MOSQ)
        set(MOSQ_ENABLED TRUE)
    endif()
endif()

if(NOT MOSQ_ENABLED)
    # (헤더 디렉터리, 라이브러리 디렉터리) 쌍으로 탐색.
    # 공식 Windows 인스톨러는 devel\ 아래, vcpkg는 include\ / lib\ 아래에 둔다.
    foreach(_mosq_root
        "$ENV{MOSQUITTO_DIR}"
        "C:/Program Files/mosquitto"
        "C:/Program Files (x86)/mosquitto"
        "C:/vcpkg/installed/x64-windows")
        if(NOT MOSQ_ENABLED AND _mosq_root)
            foreach(_inc_sub "devel" "include")
                if(NOT MOSQ_ENABLED
                   AND EXISTS "${_mosq_root}/${_inc_sub}/mosquitto.h")
                    set(_lib_dir "${_mosq_root}/${_inc_sub}")
                    if(EXISTS "${_mosq_root}/lib/mosquitto.lib")
                        set(_lib_dir "${_mosq_root}/lib")
                    endif()
                    target_include_directories(gstream_VMS PRIVATE
                        "${_mosq_root}/${_inc_sub}")
                    target_link_directories(gstream_VMS PRIVATE "${_lib_dir}")
                    target_link_libraries(gstream_VMS PRIVATE mosquitto)
                    set(MOSQ_ROOT "${_mosq_root}")
                    set(MOSQ_ENABLED TRUE)
                endif()
            endforeach()
        endif()
    endforeach()
endif()

if(MOSQ_ENABLED)
    target_compile_definitions(gstream_VMS PRIVATE HAVE_MOSQUITTO)
    message(STATUS "libmosquitto 발견 — MQTT 활성화")

    # 실행 시 필요한 DLL을 exe 옆으로 복사한다.
    # mosquitto.dll 하나만으론 안 뜬다 — mosquitto 2.1은 common/cjson/OpenSSL로
    # 쪼개져 있어서 의존 DLL이 함께 있어야 로드된다 (실측 확인).
    if(WIN32 AND MOSQ_ROOT)
        foreach(_dll
            mosquitto.dll
            mosquitto_common.dll
            cjson.dll
            pthreadVC3.dll
            libcrypto-3-x64.dll
            libssl-3-x64.dll)
            if(EXISTS "${MOSQ_ROOT}/${_dll}")
                add_custom_command(TARGET gstream_VMS POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${MOSQ_ROOT}/${_dll}" $<TARGET_FILE_DIR:gstream_VMS>
                )
            endif()
        endforeach()
    endif()
else()
    message(WARNING
        "libmosquitto 미발견 — MQTT는 무동작 스텁으로 빌드됨. "
        "v6부터 이것이 유일한 데이터 경로이므로 화면이 비어 보인다.")
endif()

# =====================================================================
# GStreamer 저지연 백엔드 (발견되면 컴파일, 없으면 QMediaPlayer만)
#
# Windows 설치법:
#   gstreamer.freedesktop.org 에서 MSVC 64-bit "runtime" + "development"
#   인스톨러 둘 다 설치 (Complete 선택). 설치기가 환경변수
#   GSTREAMER_1_0_ROOT_MSVC_X86_64 를 만들어 준다.
#   실행 시 %GSTREAMER_1_0_ROOT_MSVC_X86_64%\bin 이 PATH에 있어야 한다.
# =====================================================================
set(GST_ENABLED FALSE)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(GST IMPORTED_TARGET QUIET
        gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
    if(GST_FOUND)
        target_link_libraries(gstream_VMS PRIVATE PkgConfig::GST)
        set(GST_ENABLED TRUE)
    endif()
endif()

if(NOT GST_ENABLED)
    # 환경변수 -> winget 기본 위치 -> 공식 인스톨러 기본 위치 순으로 탐색
    foreach(_gst_root
        "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}"
        "$ENV{LOCALAPPDATA}/Programs/gstreamer/1.0/msvc_x86_64"
        "C:/gstreamer/1.0/msvc_x86_64")
        if(NOT GST_ENABLED AND _gst_root
           AND EXISTS "${_gst_root}/include/gstreamer-1.0/gst/gst.h")
            target_include_directories(gstream_VMS PRIVATE
                "${_gst_root}/include/gstreamer-1.0"
                "${_gst_root}/include/glib-2.0"
                "${_gst_root}/lib/glib-2.0/include")
            target_link_directories(gstream_VMS PRIVATE "${_gst_root}/lib")
            target_link_libraries(gstream_VMS PRIVATE
                gstreamer-1.0 gstapp-1.0 gstvideo-1.0 gobject-2.0 glib-2.0)
            set(GST_ROOT "${_gst_root}")
            set(GST_ENABLED TRUE)
        endif()
    endforeach()
endif()

if(GST_ENABLED)
    target_sources(gstream_VMS PRIVATE
        gstreamer_backend.h gstreamer_backend.cpp
        direct_sink_backend.h direct_sink_backend.cpp
        rhi_video_widget.h rhi_video_widget.cpp
        frame_queue.h pipeline_stats.h frame_trace.h)

    # NV12 -> RGB 셰이더를 .qsb로 컴파일해 리소스에 넣는다.
    # (:/shaders/nv12.vert.qsb 로 로드)
    # alias에 이미 "shaders/"가 붙으므로 PREFIX는 "/" — 합쳐서
    # :/shaders/nv12.vert.qsb 가 된다 (PREFIX를 "/shaders"로 주면 경로가 겹친다)
    qt_add_shaders(gstream_VMS "vms_shaders"
        PREFIX "/"
        FILES
            shaders/nv12.vert
            shaders/nv12.frag
    )

    target_compile_definitions(gstream_VMS PRIVATE HAVE_GSTREAMER)
    message(STATUS "GStreamer 발견 — 저지연 GPU 백엔드 활성화")

    # -----------------------------------------------------------------
    # FFmpeg 백엔드 (libav 직접 디코드). FFmpeg 개발 파일은 GStreamer MSVC
    # 설치에 함께 들어있으므로 같은 GST_ROOT에서 찾는다.
    #
    # ⚠ 버그 수정 (2026-07-30): GStreamer를 pkg-config로 찾은 경우 GST_ROOT가
    # 비어 FFmpeg가 조용히 빠졌다 (환경에 따라 ffmpeg 백엔드가 있다 없다 하는
    # 비결정성의 원인 — Qt Creator/MinGW 킷에서 실증). 루트를 독립 재탐색한다.
    # -----------------------------------------------------------------
    if(NOT GST_ROOT)
        foreach(_ff_root
            "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}"
            "$ENV{LOCALAPPDATA}/Programs/gstreamer/1.0/msvc_x86_64"
            "C:/gstreamer/1.0/msvc_x86_64")
            if(NOT GST_ROOT AND _ff_root
               AND EXISTS "${_ff_root}/include/libavcodec/avcodec.h")
                set(GST_ROOT "${_ff_root}")
            endif()
        endforeach()
    endif()
    if(GST_ROOT AND EXISTS "${GST_ROOT}/include/libavcodec/avcodec.h")
        target_sources(gstream_VMS PRIVATE ffmpeg_backend.h ffmpeg_backend.cpp)
        # FFmpeg 헤더는 ${GST_ROOT}/include/libavcodec/... — include 루트 추가
        target_include_directories(gstream_VMS PRIVATE "${GST_ROOT}/include")
        # 링크 디렉터리도 여기서 직접 — GST를 pkg-config로 찾은 빌드에선
        # 위의 GST 폴백 분기가 안 돌아 lib 경로가 등록돼 있지 않다
        # ("cannot find -lavformat" 실증, 2026-07-30)
        target_link_directories(gstream_VMS PRIVATE "${GST_ROOT}/lib")
        target_link_libraries(gstream_VMS PRIVATE
            avformat avcodec avutil swscale)
        target_compile_definitions(gstream_VMS PRIVATE HAVE_FFMPEG)
        message(STATUS "FFmpeg 발견 — FFmpeg 디코드 백엔드 활성화 (video_backend=ffmpeg)")
    else()
        message(STATUS "FFmpeg 개발 파일 미발견 — FFmpeg 백엔드 생략")
    endif()

    # -----------------------------------------------------------------
    # 방송 마이크 벤치마크 (benchmark/vmsmic) — 지연·CPU·메모리 측정.
    # 실제 방송과 **같은** 파이프라인(broadcast_pipeline.h)을 세운다.
    # 실행:  ./bench_vmsmic --describe   ·  ./bench_vmsmic --seconds 20
    # -----------------------------------------------------------------
    add_executable(bench_vmsmic
        ../benchmark/vmsmic/bench_vmsmic.cpp
        broadcast_pipeline.h broadcast_pipeline.cpp)
    target_include_directories(bench_vmsmic PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/common)
    target_link_libraries(bench_vmsmic PRIVATE Qt6::Core)
    target_compile_definitions(bench_vmsmic PRIVATE HAVE_GSTREAMER)
    if(GST_FOUND)
        target_link_libraries(bench_vmsmic PRIVATE PkgConfig::GST)
    elseif(GST_ROOT)
        target_include_directories(bench_vmsmic PRIVATE
            "${GST_ROOT}/include/gstreamer-1.0"
            "${GST_ROOT}/include/glib-2.0"
            "${GST_ROOT}/lib/glib-2.0/include")
        target_link_directories(bench_vmsmic PRIVATE "${GST_ROOT}/lib")
        target_link_libraries(bench_vmsmic PRIVATE
            gstreamer-1.0 gobject-2.0 glib-2.0)
    endif()
    if(WIN32)
        # bench_sysstat.h 의 GetProcessMemoryInfo
        target_link_libraries(bench_vmsmic PRIVATE psapi)
    endif()
else()
    message(WARNING
        "GStreamer 미발견 — QMediaPlayer 백엔드만 빌드됨 (지연 1초+). "
        "저지연이 필요하면 GStreamer MSVC runtime+development를 설치할 것.")
endif()

# =====================================================================
# 빌드 후 자동으로 Qt DLL + sqldrivers(qsqlpsql.dll 등) 복사
# =====================================================================
if(WIN32)
    # v6에서 --sql 을 뺐다. VMS는 Qt::Sql 을 링크하지 않으므로 sqldrivers
    # 플러그인(qsqlpsql.dll)도, 그것이 의존하던 libpq.dll 등 PostgreSQL
    # 클라이언트 DLL 5종도 더 이상 필요 없다.
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET gstream_VMS POST_BUILD
            COMMAND ${WINDEPLOYQT_EXECUTABLE} $<TARGET_FILE:gstream_VMS>
        )
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS gstream_VMS
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET gstream_VMS
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

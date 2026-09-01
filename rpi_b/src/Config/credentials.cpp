#include "Config/credentials.hpp"
#include <iostream>

// 카메라 접속 계정을 DB(camera_credentials)에서 로드.
// 행이 있으면 cfg.user/pass 를 덮어씀. 없으면 config.env 값 유지 (폴백 —
// 시드 전이거나 테이블 미생성이어도 폴러가 죽지 않게).
void loadCameraCredentials(Config& cfg, pqxx::connection& db) {
  try {
    pqxx::work tx(db);
    pqxx::result r = tx.exec(
        "SELECT cam_user, cam_pass FROM camera_credentials WHERE camera_id=$1",
        pqxx::params{cfg.camera_id});
    tx.commit();
    if (r.empty()) {
      std::cout << "[cred] no DB row for camera " << cfg.camera_id
                << " — using config.env credentials\n";
      return;
    }
    cfg.user = r[0]["cam_user"].as<std::string>();
    cfg.pass = r[0]["cam_pass"].as<std::string>();
    std::cout << "[cred] camera " << cfg.camera_id
              << " credentials loaded from DB (user=" << cfg.user << ")\n";
  } catch (const std::exception& e) {
    // 테이블 없음/권한 없음 등 — config.env 폴백 유지
    std::cerr << "[cred] DB load failed (" << e.what()
              << ") — using config.env credentials\n";
  }
}
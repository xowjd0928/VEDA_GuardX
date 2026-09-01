#include "Poller/http_client.hpp"
#include <curl/curl.h>
#include <iostream>

static size_t writeCb(char* p, size_t s, size_t n, void* ud) {
  static_cast<std::string*>(ud)->append(p, s * n);
  return s * n;
}

// v16: 스레드당 persistent easy handle — TCP/TLS 세션과 Digest 인증 상태가
// 요청 간 재사용된다 (기존 요청당 init/cleanup은 매번 TLS 핸드셰이크 +
// Digest 401 왕복을 다시 치렀음). thread_local 이라 2-레인 스레딩 도입 시
// 레인마다 자동으로 자기 핸들을 가진다 — easy handle 은 스레드 간 공유 금지.
namespace {
struct CurlHandle {
  CURL* h = curl_easy_init();
  ~CurlHandle() { if (h) curl_easy_cleanup(h); }
};
}  // namespace

HttpResp httpGet(const Config& cfg, const std::string& url) {
  thread_local CurlHandle tl;
  HttpResp r;
  CURL* c = tl.h;
  if (!c) return r;
  // 핸들은 재사용, 옵션은 매 요청 재설정 (상태 잔류 방지 — 값이 같아도 무해)
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_DIGEST);
  curl_easy_setopt(c, CURLOPT_USERPWD, cfg.userpass().c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);   // v15: 대형 백로그 응답 여유 (실측 1.1MB/11.5s)
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);   // 60s 틱 사이 유휴 연결 유지
  // TLS 신원 검증 — 우선순위: 공개키 핀 > CA 고정(strict) > insecure(테스트).
  // 핀 모드: 자체서명 + IP 접속이라 CA·호스트명 검증은 통과 불가 → 끄고,
  // 대신 서버 공개키가 CAM_PINNED_KEY(sha256//...)와 일치해야만 연결된다.
  // 핀 불일치 = CURLE_SSL_PINNEDPUBKEYNOTMATCH 로 즉시 거부 (MITM 차단).
  curl_easy_setopt(c, CURLOPT_PINNEDPUBLICKEY,
                   cfg.pinned_key.empty() ? nullptr : cfg.pinned_key.c_str());
  if (!cfg.pinned_key.empty() || cfg.insecure) {
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  } else {
    if (!cfg.cainfo.empty()) {   // CA 고정 (호스트명이 인증서와 일치할 때만 유효)
      curl_easy_setopt(c, CURLOPT_CAINFO, cfg.cainfo.c_str());
    }
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);   // 재사용 핸들 — 명시적 복원
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  }
  CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.code);
  else std::cerr << "[http] " << url << " : " << curl_easy_strerror(rc) << "\n";
  return r;
}

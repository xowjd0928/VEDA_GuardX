#pragma once
// http_client — libcurl 래퍼 (HTTPS + digest GET 전용)
#include <string>
#include "Config/config.hpp"

struct HttpResp {
  long code = 0;
  std::string body;
  bool ok() const { return code == 200; }
};

HttpResp httpGet(const Config& cfg, const std::string& url);


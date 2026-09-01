#pragma once
// time_util — ISO8601 ↔ epoch. ISO 문자열은 사전식 비교 = 시간순 (커서 비교에 활용)
#include <cstdio>
#include <ctime>
#include <string>

inline time_t isoToEpoch(const std::string& iso) {
  struct tm t {}; int y, mo, d, h, mi, s, ms = 0;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y,&mo,&d,&h,&mi,&s,&ms) < 6) return 0;
  t.tm_year=y-1900; t.tm_mon=mo-1; t.tm_mday=d; t.tm_hour=h; t.tm_min=mi; t.tm_sec=s;
  return timegm(&t);
}

inline std::string epochToIso(time_t sec) {
  struct tm t {}; gmtime_r(&sec, &t); char b[80];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return b;
}


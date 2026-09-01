// test_parsing.cpp — 픽스처(실측 응답)로 파싱·변환 로직 검증. DB·네트워크 불필요.
// 빌드: g++ -std=c++17 -O2 test/test_parsing.cpp -o test_parsing && ./test_parsing
#include <nlohmann/json.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
using json = nlohmann::json;

static time_t isoToEpoch(const std::string& iso) {
  struct tm t{}; int y,mo,d,h,mi,s,ms=0;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y,&mo,&d,&h,&mi,&s,&ms) < 6) return 0;
  t.tm_year=y-1900; t.tm_mon=mo-1; t.tm_mday=d; t.tm_hour=h; t.tm_min=mi; t.tm_sec=s;
  return timegm(&t);
}
static std::string epochToIso(time_t sec) {
  struct tm t{}; gmtime_r(&sec, &t); char b[80];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return b;
}
static uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}
static std::string polygonWkt(const json& ivaarea, int channel) {
  for (const auto& ch : ivaarea["ivaArea"]) {
    if (ch.value("channel",-1) != channel) continue;
    if (ch["definedArea"].empty()) return "";
    const auto& area = ch["definedArea"][0];
    std::string wkt = "POLYGON((", first; bool sep=false;
    for (const auto& pt : area["areaCoordinates"]) {
      char b[32]; snprintf(b,sizeof(b),"%d %d",pt.value("x",0),pt.value("y",0));
      if (sep) wkt += ", "; wkt += b; sep=true; if (first.empty()) first=b;
    }
    return wkt + ", " + first + "))";
  }
  return "";
}
static json load(const char* p){ std::ifstream f(p); json j; f>>j; return j; }

int main() {
  // 1) detections: 필드 매핑 + 커서(최대 ts) + 0-Shape 레코드 통과
  json det = load("test/fixtures/detections_sample.json");
  std::string cursor;
  int n=0;
  for (const auto& d : det["detections"]) {
    assert(d.value("category",0)==1);
    std::string ts=d.value("ts",""); if (ts>cursor) cursor=ts; ++n;
  }
  assert(n==3);
  assert(cursor=="2026-07-13T06:31:26.687Z");   // ISO 문자열비교 = 시간순
  printf("detections: %d rows, cursor=%s\n", n, cursor.c_str());

  // 2) prediction v2 (CAMERA_API_v13): horizon {5,30,60,180}, p50 반올림,
  //    target_ts = served + horizon분, warmup 게이트, p_over_capacity -1 = 불명(NULL)
  json pr = load("test/fixtures/prediction_sample.json");
  time_t served = isoToEpoch(pr.value("served_utc",""));
  assert(served!=0);
  assert(pr["model"].value("version","")=="hw_damped_v1");
  assert(pr["model"].value("warmup",true)==false);   // false 여야 적재 경로
  assert(!pr.contains("config_version"));            // v1 필드는 응답에서 소멸
  const int want_h[4] = {5,30,60,180};
  assert(pr["predictions"].size()==4);
  for (int i=0;i<4;++i) {
    const auto& p = pr["predictions"][i];
    assert(p.value("horizon_min",0)==want_h[i]);
    double p10=p.value("p10",-1.0), p50=p.value("p50",-1.0), p90=p.value("p90",-1.0);
    assert(0.0<=p10 && p10<=p50 && p50<=p90);         // 분위수 순서 불변식
    double poc = p.value("p_over_capacity",-2.0);
    assert(poc==-1.0 || (0.0<=poc && poc<=1.0));      // -1 = 불명 → NULL 적재 (0 아님)
  }
  assert(pr["predictions"][1].value("p_over_capacity",0.0)==-1.0);  // 불명 케이스 존재
  std::string target = epochToIso(served + 5*60);
  assert(target=="2026-07-20T01:23:35Z");
  int cnt = (int)(pr["predictions"][0].value("p50",0.0)+0.5);
  assert(cnt==1);   // 0.74 → 1
  printf("prediction v2: 4 horizons, target(h=5)=%s round(0.74)=%d\n", target.c_str(), cnt);

  // 3) ivaarea → WKT (닫힌 링, 픽셀 좌표)
  json iv = load("test/fixtures/ivaarea_sample.json");
  std::string wkt = polygonWkt(iv, 1);
  assert(wkt=="POLYGON((381 585, 645 585, 645 1147, 381 1147, 381 585))");
  printf("wkt: %s\n", wkt.c_str());

  // 4) 형상 해시: 동일 입력 동일 값 + 좌표 1px 변경 감지
  json lc = load("test/fixtures/linecrossing_sample.json");
  std::string body = lc.dump() + "\x1f" + iv.dump();
  uint64_t h1 = fnv1a(body), h2 = fnv1a(body);
  assert(h1==h2);
  std::string body2 = body; body2.replace(body2.find("1353"),4,"1354");
  assert(fnv1a(body2)!=h1);
  printf("hash: stable + 1px-change detected\n");

  printf("\nALL POLLER PARSING TESTS PASSED\n");
  return 0;
}




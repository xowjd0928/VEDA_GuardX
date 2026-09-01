#include "edge_map_generator.h"

#include <QSettings>
#include <QStack>
#include <QVector>
#include <QtGlobal>

#include <algorithm>

// ⚠ **libm 함수를 부르지 않는다** (exp·ceil·atan2·hypot·sqrt).
//   이 프로젝트는 `-Wl,--disable-auto-import` 로 링크하는데, 인라인되지 않은
//   libm 호출은 그 가드레일에 걸려 `undefined reference to 'exp'` 로 링크가
//   터진다 (2026-08-12 에 sqrt 로 한 번, 08-24 에 exp/ceil/atan2 로 또 한 번).
//   그래서 ①가우시안 대신 **박스 블러 3회**(가우시안 근사) ②크기는 전부
//   **제곱값**으로 비교(sqrt 불필요) ③방향은 atan2 대신 **부호·비율 비교**.
//   덤으로 더 빠르다 — 픽셀당 초월함수가 0 개다.

namespace {

/** @brief 박스 블러 1회 (분리 가능, 가장자리는 클램프) */
void box_blur(QVector<float> &buf, QVector<float> &tmp, int w, int h, int r)
{
    if (r <= 0)
        return;
    const float inv = 1.0f / float(2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const int row = y * w;
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i)
                acc += buf[row + qBound(0, x + i, w - 1)];
            tmp[row + x] = acc * inv;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i)
                acc += tmp[qBound(0, y + i, h - 1) * w + x];
            buf[y * w + x] = acc * inv;
        }
    }
}

/** @brief 가우시안 근사 = 박스 블러 3회 (반지름은 sigma 에서 역산) */
QVector<float> smooth(const QVector<float> &src, int w, int h, double sigma)
{
    // 3회 박스(반지름 r)의 유효 sigma ≈ sqrt((2r+1)^2 - 1) / 2
    // → r ≈ (sqrt(4*sigma^2 + 1) - 1) / 2. sqrt 없이 정수 탐색으로 고른다.
    int r = 0;
    while (r < 32) {
        const double wid = 2.0 * (r + 1) + 1.0;
        if ((wid * wid - 1.0) / 4.0 > sigma * sigma)
            break;
        ++r;
    }
    QVector<float> buf(src), tmp(src.size());
    for (int pass = 0; pass < 3; ++pass)
        box_blur(buf, tmp, w, h, r);
    return buf;
}

/** @brief 3×3 Sobel — **제곱** 크기를 돌려준다 (sqrt 회피) */
QVector<float> sobel2(const QVector<float> &g, int w, int h,
                      QVector<float> *gx_out, QVector<float> *gy_out)
{
    QVector<float> mag2(g.size(), 0.0f);
    if (gx_out) gx_out->fill(0.0f, g.size());
    if (gy_out) gy_out->fill(0.0f, g.size());
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const float a = g[(y-1)*w + x-1], b = g[(y-1)*w + x], c = g[(y-1)*w + x+1];
            const float d = g[ y   *w + x-1],                     f = g[ y   *w + x+1];
            const float e = g[(y+1)*w + x-1], i = g[(y+1)*w + x], j = g[(y+1)*w + x+1];
            const float vx = -a + c - 2*d + 2*f - e + j;
            const float vy = -a - 2*b - c + e + 2*i + j;
            const int idx = y * w + x;
            if (gx_out) (*gx_out)[idx] = vx;
            if (gy_out) (*gy_out)[idx] = vy;
            mag2[idx] = vx * vx + vy * vy;
        }
    }
    return mag2;
}

}  // namespace

QImage EdgeMapGenerator::generate_edge_overlay(const QImage &source,
                                               int threshold,
                                               const QColor &edge_color)
{
    if (source.isNull() || source.width() < 8 || source.height() < 8)
        return QImage();

    // 벽지·조명이 바뀌면 재빌드 없이 맞출 수 있게 (헤더 §tuning)
    // ⚠ sync() 가 필요하다 — QSettings 는 프로세스 안에서 값을 캐시하므로,
    //   앱이 떠 있는 동안 레지스트리를 고쳐도 그냥은 안 읽힌다. 재캡처마다
    //   한 번 읽는 값이라 비용은 무시할 만하고, 이게 있어야 "값 바꾸고 Edge
    //   Map 다시 누르기"로 현장에서 맞출 수 있다 (재시작 없이).
    QSettings s("GuardX", "VMS");
    s.sync();
    const double sharp_min = s.value("edge_sharpness", 3.5).toDouble();
    const double mag_min   = s.value("edge_mag_min", threshold).toDouble();
    const int    min_len   = s.value("edge_min_len", 60).toInt();
    const double sigma_fine   = s.value("edge_sigma_fine", 0.6).toDouble();
    const double sigma_coarse = s.value("edge_sigma_coarse", 3.0).toDouble();

    const float mag_min2  = float(mag_min * mag_min);
    const float sharp_min2 = float(sharp_min * sharp_min);

    const QImage input = source.convertToFormat(QImage::Format_RGB32);
    const int w = input.width(), h = input.height();

    QVector<float> gray(w * h);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(input.constScanLine(y));
        for (int x = 0; x < w; ++x)
            gray[y * w + x] = float(qGray(line[x]));
    }

    // ---- 두 척도의 기울기 (선명도 판정의 근거 — 헤더 §why) ----
    QVector<float> gx, gy;
    const QVector<float> fine2 =
        sobel2(smooth(gray, w, h, sigma_fine), w, h, &gx, &gy);
    const QVector<float> coarse2 =
        sobel2(smooth(gray, w, h, sigma_coarse), w, h, nullptr, nullptr);

    // ---- 선명도 + 비최대 억제 → 1px 선 ----
    QVector<quint8> keep(w * h, 0);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const int idx = y * w + x;
            const float m2 = fine2[idx];
            if (m2 < mag_min2)
                continue;
            // 계단이면 굵은 척도에서 크게 죽는다 — 무른 얼룩은 안 죽는다
            if (m2 < sharp_min2 * std::max(coarse2[idx], 1.0f))
                continue;

            // 그래디언트 방향의 두 이웃과 비교 (atan2 없이 부호·비율로)
            const float ax = std::abs(gx[idx]), ay = std::abs(gy[idx]);
            float p, q;
            if (ay <= 0.4142f * ax) {           // 가로 방향 기울기
                p = fine2[idx - 1];     q = fine2[idx + 1];
            } else if (ay >= 2.4142f * ax) {    // 세로 방향
                p = fine2[idx - w];     q = fine2[idx + w];
            } else if ((gx[idx] > 0) == (gy[idx] > 0)) {   // ↘ 대각
                p = fine2[idx - w - 1]; q = fine2[idx + w + 1];
            } else {                                        // ↗ 대각
                p = fine2[idx - w + 1]; q = fine2[idx + w - 1];
            }
            if (m2 >= p && m2 >= q)
                keep[idx] = 1;
        }
    }

    // ---- 짧은 성분 버리기 (8-이웃 라벨링) ----
    // 얼룩을 걷어낸 뒤에도 남는 점·부스러기를 정리한다. 길이로 자르는 것은
    // 구조/무늬를 가르는 주 수단이 아니라(그건 위의 선명도가 한다) 마무리다.
    QImage output(input.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    const QRgb edge_pixel = edge_color.rgba();

    QVector<quint8> seen(w * h, 0);
    QStack<int> stack;
    QVector<int> pts;
    for (int start = 0; start < w * h; ++start) {
        if (!keep[start] || seen[start])
            continue;
        pts.clear();
        stack.push(start);
        seen[start] = 1;
        while (!stack.isEmpty()) {
            const int idx = stack.pop();
            pts.append(idx);
            const int cx = idx % w, cy = idx / w;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    const int nidx = ny * w + nx;
                    if (keep[nidx] && !seen[nidx]) {
                        seen[nidx] = 1;
                        stack.push(nidx);
                    }
                }
            }
        }
        if (pts.size() < min_len)
            continue;
        for (int idx : pts)
            reinterpret_cast<QRgb *>(output.scanLine(idx / w))[idx % w] = edge_pixel;
    }

    return output;
}

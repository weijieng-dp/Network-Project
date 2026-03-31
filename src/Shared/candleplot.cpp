#include "candleplot.h"
#include <algorithm>

namespace MyImPlot {

    template <typename T>
    int BinarySearch(const T* arr, int l, int r, T x) {
        if (r >= l) {
            int mid = l + (r - l) / 2;
            if (arr[mid] == x)
                return mid;
            if (arr[mid] > x)
                return BinarySearch(arr, l, mid - 1, x);
            return BinarySearch(arr, mid + 1, r, x);
        }
        return -1;
    }


    void PlotCandlestick(const double* xs, const double* opens, const double* closes, const double* lows, const double* highs, int count, float width_pct) {

        ImDrawList* dl = ImPlot::GetPlotDrawList();
        for (int i = 0; i < count; ++i) {
            ImVec2 openPos = ImPlot::PlotToPixels(xs[i], opens[i]);
            ImVec2 closePos = ImPlot::PlotToPixels(xs[i], closes[i]);
            ImVec2 lowPos = ImPlot::PlotToPixels(xs[i], lows[i]);
            ImVec2 highPos = ImPlot::PlotToPixels(xs[i], highs[i]);

            bool bullish = closes[i] >= opens[i];
            ImU32 color = bullish ? IM_COL32(0, 200, 80, 255) : IM_COL32(220, 50, 50, 255);

            float halfW = (count > 1)
                ? (float)std::abs(ImPlot::PlotToPixels(xs[0], 0).x - ImPlot::PlotToPixels(xs[0] + 1, 0).x) * width_pct * 0.5f
                : 8.0f;
            halfW = std::max(halfW, 1.0f);

            // Wick
            dl->AddLine(ImVec2(highPos.x, highPos.y), ImVec2(lowPos.x, lowPos.y), color, 1.0f);
            // Body
            float top = std::min(openPos.y, closePos.y);
            float bot = std::max(openPos.y, closePos.y);
            if (bot - top < 1.0f) bot = top + 1.0f;
            dl->AddRectFilled(ImVec2(openPos.x - halfW, top), ImVec2(openPos.x + halfW, bot), color);
        }
    }
}
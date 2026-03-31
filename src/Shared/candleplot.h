#pragma once
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "implot.h"
#include "implot_internal.h"
#include "imgui_impl_opengl3.h"

// MIT License

// Copyright (c) 2020-2024 Evan Pezent
// Copyright (c) 2025-2026 Breno Cunha Queiroz

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// ImPlot v0.18 WIP

namespace MyImPlot {
    /* This is ImPlot's internal implementation of the candlestick plot. It is not public as of right now.
    *   \param label_id         : the name of the plot
    *   \param xs               : the array of dates
    *   \param opens            : the array of opens
    *   \param closes           : the array of closes
    *   \param lows             : the array of lows
    *   \param highs            : the array of highs
    *   \param count            : size of the arrays
    *   \param tooltip          : whether to display a tooltip on hover
    *   \param width_percent    : 
    *   \param bullCol          : color of bull candles
    *   \param bearCol          : color of bear candles
    * 
    /----------------------------------------------------------------------------------------------------*/
    void PlotCandlestick(const double* xs, const double* opens, const double* closes, const double* lows, const double* highs, int count, float width_pct = 0.6f);
}
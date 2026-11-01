#ifndef QOH99_DDRAW_INPUT_DISPLAY_LAYOUT_H
#define QOH99_DDRAW_INPUT_DISPLAY_LAYOUT_H

namespace qoh99_input_display_layout {

const float kVirtualWidth = 640.0f;
const float kVirtualHeight = 480.0f;
const float kCellUnits = 15.0f;
const float kInsetUnits = 3.0f;
const float kDurationColumnUnits = 30.0f;
const float kMarginUnits = 6.0f;
const int kGlyphColumns = 5;
const float kBandTopFraction = 0.350f;
const float kBandBottomFraction = 0.7875f;
const float kLiveGapUnits = 4.0f;
const float kLiveGapMinPx = 2.0f;

const float kScaleMin = 0.5f;
const float kScaleMax = 8.0f;

inline float clampFloat(float value, float lo, float hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

inline float floorFloat(float value) {
    const int truncated = static_cast<int>(value);
    const float asFloat = static_cast<float>(truncated);
    return (asFloat > value) ? (asFloat - 1.0f) : asFloat;
}

struct Metrics {
    bool renderable = false;

    float scale = 1.0f;
    float cell = 0.0f;
    float inset = 0.0f;
    float durationColumn = 0.0f;
    float panelWidth = 0.0f;
    float margin = 0.0f;

    float imageLeft = 0.0f;
    float imageWidth = 0.0f;

    float bandTop = 0.0f;
    float bandBottom = 0.0f;

    float liveTop = 0.0f;
    float liveBottom = 0.0f;
    float liveGap = 0.0f;

    float historyTop = 0.0f;
    float historyBottom = 0.0f;

    int visibleRows = 0;
};

inline float computeScale(int dstW, int dstH) {
    if (dstW <= 0 || dstH <= 0) return 1.0f;
    const float sx = static_cast<float>(dstW) / kVirtualWidth;
    const float sy = static_cast<float>(dstH) / kVirtualHeight;
    return clampFloat((sx < sy) ? sx : sy, kScaleMin, kScaleMax);
}

inline Metrics compute(int dstX, int dstY, int dstW, int dstH,
                       int historyCapacity) {
    Metrics m;
    m.scale = computeScale(dstW, dstH);
    m.cell = kCellUnits * m.scale;
    m.inset = kInsetUnits * m.scale;
    m.durationColumn = kDurationColumnUnits * m.scale;
    m.panelWidth = 2.0f * m.inset
        + static_cast<float>(kGlyphColumns) * m.cell
        + m.durationColumn;
    m.margin = kMarginUnits * m.scale;

    m.imageLeft = static_cast<float>(dstX);
    m.imageWidth = static_cast<float>(dstW);

    const float imageTop = static_cast<float>(dstY);
    const float imageHeight = static_cast<float>(dstH);
    m.bandTop = imageTop + imageHeight * kBandTopFraction;
    m.bandBottom = imageTop + imageHeight * kBandBottomFraction;

    const float gap = kLiveGapUnits * m.scale;
    m.liveGap = floorFloat(gap);
    if (m.liveGap < kLiveGapMinPx) m.liveGap = kLiveGapMinPx;

    m.liveTop = m.bandTop;
    m.liveBottom = m.bandTop + m.cell;
    m.historyTop = m.liveBottom + m.liveGap;
    m.historyBottom = m.bandBottom;

    if (m.cell <= 0.0f
        || m.bandBottom - m.bandTop < 2.0f * m.cell + m.liveGap
        || m.panelWidth * 2.0f + m.margin * 2.0f > m.imageWidth) {
        m.renderable = false;
        m.visibleRows = 0;
        return m;
    }

    int rows = static_cast<int>(
        floorFloat((m.historyBottom - m.historyTop) / m.cell));
    if (rows < 0) rows = 0;
    if (rows > historyCapacity) rows = historyCapacity;
    m.visibleRows = rows;
    m.renderable = true;
    return m;
}

inline float historyRowTop(const Metrics& m, int row) {
    return m.historyTop + m.cell * static_cast<float>(row);
}

inline bool historyRowFits(const Metrics& m, int row) {
    return historyRowTop(m, row) + m.cell <= m.historyBottom;
}

struct LanePanelX {
    float left = 0.0f;
    float right = 0.0f;
    bool rightAligned = false;
};

inline LanePanelX lanePanelX(const Metrics& m, unsigned lane) {
    LanePanelX p;
    p.rightAligned = (lane & 1u) != 0u;
    const float indent =
        static_cast<float>(lane / 2u) * (m.panelWidth + m.margin);
    p.left = p.rightAligned
        ? (m.imageLeft + m.imageWidth - m.margin - indent - m.panelWidth)
        : (m.imageLeft + m.margin + indent);
    p.right = p.left + m.panelWidth;
    return p;
}

}

#endif

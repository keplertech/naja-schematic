#include "SchematicPainter.h"

#include <cfloat>
#include <cstdio>

namespace {

// "rgb(r,g,b)" plus the alpha as a separate opacity, the form every SVG
// renderer (and SVG-to-PDF converter) understands.
std::string rgb(ImU32 col) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgb(%u,%u,%u)",
                  (col >> IM_COL32_R_SHIFT) & 0xFF, (col >> IM_COL32_G_SHIFT) & 0xFF,
                  (col >> IM_COL32_B_SHIFT) & 0xFF);
    return buf;
}

float alpha(ImU32 col) { return ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f; }

// Coordinates with two decimals: exact enough, and keeps the file small.
std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s = buf;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

std::string fill(ImU32 col) {
    std::string s = " fill=\"" + rgb(col) + "\"";
    if (alpha(col) < 1.0f) s += " fill-opacity=\"" + num(alpha(col)) + "\"";
    return s;
}

std::string stroke(ImU32 col, float thickness) {
    std::string s = " fill=\"none\" stroke=\"" + rgb(col) + "\" stroke-width=\"" + num(thickness) + "\"";
    if (alpha(col) < 1.0f) s += " stroke-opacity=\"" + num(alpha(col)) + "\"";
    return s;
}

std::string escapeXml(const char* text) {
    std::string out;
    for (const char* c = text; *c; ++c) {
        switch (*c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += *c;       break;
        }
    }
    return out;
}

} // namespace

SvgPainter::SvgPainter(ImVec2 size) {
    out_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\""
         << " width=\"" << num(size.x) << "\" height=\"" << num(size.y) << "\""
         << " viewBox=\"0 0 " << num(size.x) << " " << num(size.y) << "\""
         << " font-family=\"'Droid Sans', 'Open Sans', Arial, Helvetica, sans-serif\""
         << " stroke-linecap=\"butt\" stroke-linejoin=\"miter\">\n";
}

void SvgPainter::points(const ImVec2* pts, int n) {
    out_ << " points=\"";
    for (int i = 0; i < n; ++i) out_ << (i ? " " : "") << num(pts[i].x) << "," << num(pts[i].y);
    out_ << "\"";
}

void SvgPainter::AddLine(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness) {
    out_ << "<line x1=\"" << num(a.x) << "\" y1=\"" << num(a.y) << "\" x2=\"" << num(b.x)
         << "\" y2=\"" << num(b.y) << "\"" << stroke(col, thickness) << "/>\n";
}

void SvgPainter::AddPolyline(const ImVec2* pts, int n, ImU32 col, float thickness, ImDrawFlags flags) {
    if (n < 2) return;
    out_ << ((flags & ImDrawFlags_Closed) ? "<polygon" : "<polyline");
    points(pts, n);
    out_ << stroke(col, thickness) << "/>\n";
}

void SvgPainter::AddConvexPolyFilled(const ImVec2* pts, int n, ImU32 col) {
    if (n < 3) return;
    out_ << "<polygon";
    points(pts, n);
    out_ << fill(col) << "/>\n";
}

void SvgPainter::AddConcavePolyFilled(const ImVec2* pts, int n, ImU32 col) {
    AddConvexPolyFilled(pts, n, col);   // SVG fills any simple polygon
}

void SvgPainter::AddRect(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding,
                         ImDrawFlags /*flags*/, float thickness) {
    out_ << "<rect x=\"" << num(rmin.x) << "\" y=\"" << num(rmin.y) << "\" width=\""
         << num(rmax.x - rmin.x) << "\" height=\"" << num(rmax.y - rmin.y) << "\"";
    if (rounding > 0.0f) out_ << " rx=\"" << num(rounding) << "\"";
    out_ << stroke(col, thickness) << "/>\n";
}

void SvgPainter::AddRectFilled(const ImVec2& rmin, const ImVec2& rmax, ImU32 col, float rounding) {
    out_ << "<rect x=\"" << num(rmin.x) << "\" y=\"" << num(rmin.y) << "\" width=\""
         << num(rmax.x - rmin.x) << "\" height=\"" << num(rmax.y - rmin.y) << "\"";
    if (rounding > 0.0f) out_ << " rx=\"" << num(rounding) << "\"";
    out_ << fill(col) << "/>\n";
}

void SvgPainter::AddCircle(const ImVec2& c, float r, ImU32 col, int /*segments*/, float thickness) {
    out_ << "<circle cx=\"" << num(c.x) << "\" cy=\"" << num(c.y) << "\" r=\"" << num(r) << "\""
         << stroke(col, thickness) << "/>\n";
}

void SvgPainter::AddCircleFilled(const ImVec2& c, float r, ImU32 col) {
    out_ << "<circle cx=\"" << num(c.x) << "\" cy=\"" << num(c.y) << "\" r=\"" << num(r) << "\""
         << fill(col) << "/>\n";
}

void SvgPainter::AddText(ImFont* font, float fontSize, const ImVec2& pos, ImU32 col, const char* text) {
    if (!text || !*text) return;
    // ImGui places text by its top-left corner, SVG by its baseline.
    const float ascent = font->GetFontBaked(fontSize)->Ascent;
    const float width  = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text).x;
    out_ << "<text x=\"" << num(pos.x) << "\" y=\"" << num(pos.y + ascent) << "\" font-size=\""
         << num(fontSize) << "\"";
    if (width > 0.0f) out_ << " textLength=\"" << num(width) << "\" lengthAdjust=\"spacingAndGlyphs\"";
    out_ << fill(col) << ">" << escapeXml(text) << "</text>\n";
}

std::string SvgPainter::finish() {
    out_ << "</svg>\n";
    return out_.str();
}

#include "frei0r.hpp"
#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <sstream>

union Color {
  struct {
    uint8_t a;
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };
  uint32_t argb;
};

static inline double lerp(double f, double a, double b) {
  if (f > 1)
    f = 1;
  if (f < 0)
    f = 0;
  return (1 - f) * a + f * b;
}

static inline double ease_in_out_sine(double t) {
  return -(cos(M_PI * t) - 1) / 2;
}

class Loupe : public frei0r::mixer2 {
  static constexpr unsigned numRegions = 3;

  struct Region {
    bool enable;
    f0r_param_position srcCenter, srcSize;
    f0r_param_position dstCenter;
    double dstZoom;
  };

public:
  Loupe(unsigned width, unsigned height) {
    showWireframe = true;
    showMagnified = true;
    outlineWidth = 0.03;
    pointerWidth = 0.06;
    pointerOutlineWidth = 0.03;
    fadeDuration = 0.1;
    endTime = 0.01;

    for (auto &region : regions) {
      region.enable = false;
      region.srcCenter = {0.5, 0.5};
      region.srcSize = {0.5, 0.5};
      region.dstCenter = {0.5, 0.5};
      region.dstZoom = 0.2;
    }

    register_param(showWireframe, "Wire Frame",
                   "Show wire frame for positioning.");
    register_param(showMagnified, "Show Magnified", "Show magnified region.");
    register_param(outlineWidth, "Outline Width",
                   "The width of the outline drawn around the magnified region "
                   "(in 100 pixels at 1080p).");
    register_param(pointerWidth, "Pointer Width",
                   "The width of the pointer line (in 100 pixels at 1080p).");
    register_param(
        pointerOutlineWidth, "Pointer Outline Width",
        "The width of the pointer bubble outline (in 100 pixels at 1080p).");
    register_param(fadeDuration, "Fade Duration",
                   "The duration of the fade in/out (in 10 seconds).");
    register_param(endTime, "End Time",
                   "The time after which the image should be back to normal "
                   "(in 1000 seconds).");

    for (unsigned i = 0; i < numRegions; ++i) {
      auto &region = regions[i];
      std::stringstream os;
      os << "Enable region " << i;
      register_param(region.enable, os.str(),
                     "Enable another magnification region.");
      register_param(region.srcCenter.x, "Source Center X",
                     "The center of the source rectangle.");
      register_param(region.srcCenter.y, "Source Center Y",
                     "The center of the source rectangle.");
      register_param(region.srcSize.x, "Source Size X",
                     "The size of the source rectangle.");
      register_param(region.srcSize.y, "Source Size X",
                     "The size of the source rectangle.");
      register_param(region.dstCenter.x, "Destination Center X",
                     "The center of the destination rectangle.");
      register_param(region.dstCenter.y, "Destination Center Y",
                     "The center of the destination rectangle.");
      register_param(region.dstZoom, "Destination Zoom",
                     "The magnification factor of the destination.");
    }
  }

  void update(double time, uint32_t *rawDst, const uint32_t *rawSrc2,
              const uint32_t *rawSrc) override {
    double fadeDuration = this->fadeDuration * 10;
    double endTime = this->endTime * 1000;
    double fade = std::min(time, endTime - time) / fadeDuration;
    if (fade > 1)
      fade = 1;
    if (fade < 0)
      fade = 0;
    fade = ease_in_out_sine(fade);

    auto stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    cairo_surface_t *dst = cairo_image_surface_create_for_data(
        (unsigned char *)rawDst, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_surface_t *src = cairo_image_surface_create_for_data(
        (unsigned char *)rawSrc, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_t *cr = cairo_create(dst);

    // Draw the original image first.
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    // Compute the boundaries for the regions to be magnified.
    struct {
      int srcWidth, srcHeight, srcX0, srcY0;
      double realZoom;
      int dstWidth, dstHeight, dstX0, dstY0;
    } info[numRegions];

    for (unsigned i = 0; i < numRegions; ++i) {
      info[i].srcWidth = regions[i].srcSize.x * height + 0.5;
      info[i].srcHeight = regions[i].srcSize.y * height + 0.5;
      info[i].srcX0 = (regions[i].srcCenter.x * 2 - 0.5) * width -
                      info[i].srcWidth / 2 + 0.5;
      info[i].srcY0 = (regions[i].srcCenter.y * 2 - 0.5) * height -
                      info[i].srcHeight / 2 + 0.5;

      // Compute the boundaries for the magnified version of the regions.
      info[i].realZoom = lerp(fade, 1, regions[i].dstZoom * 10);
      info[i].dstWidth = info[i].srcWidth * info[i].realZoom + 0.5;
      info[i].dstHeight = info[i].srcHeight * info[i].realZoom + 0.5;
      info[i].dstX0 =
          (lerp(fade, regions[i].srcCenter.x, regions[i].dstCenter.x) * 2 -
           0.5) *
              width -
          info[i].dstWidth / 2 + 0.5;
      info[i].dstY0 =
          (lerp(fade, regions[i].srcCenter.y, regions[i].dstCenter.y) * 2 -
           0.5) *
              height -
          info[i].dstHeight / 2 + 0.5;
    }

    // Draw the actual magnified region.
    for (unsigned n = 0; n < numRegions && showMagnified; ++n) {
      const auto &i = info[n];
      const auto &r = regions[n];
      if (!r.enable)
        continue;
      cairo_save(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

      // Draw the pointer bubble to the source location.
      double bw = lerp(fade, 0, pointerWidth * 100 * height / 1080);
      double bw2 = lerp(fade, 0, pointerOutlineWidth * 100 * height / 1080);
      cairo_save(cr);
      cairo_set_line_width(cr, bw);
      cairo_set_source_rgb(cr, 0, 0, 0);
      cairo_move_to(cr, i.srcX0 + i.srcWidth / 2, i.srcY0 + i.srcHeight / 2);
      cairo_line_to(cr, i.dstX0 + i.dstWidth / 2, i.dstY0 + i.dstHeight / 2);
      cairo_stroke(cr);
      cairo_set_line_width(cr, bw2);
      cairo_arc(cr, i.srcX0 + i.srcWidth / 2, i.srcY0 + i.srcHeight / 2,
                bw + bw2 / 2, 0, 2 * M_PI);
      cairo_set_source_rgb(cr, 0, 0, 0);
      cairo_fill_preserve(cr);
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_stroke(cr);
      cairo_restore(cr);

      // Draw the magnified image.
      cairo_save(cr);
      cairo_rectangle(cr, i.dstX0, i.dstY0, i.dstWidth, i.dstHeight);
      cairo_clip(cr);
      cairo_scale(cr, i.realZoom, i.realZoom);
      cairo_translate(cr, -i.srcX0, -i.srcY0);
      cairo_translate(cr, i.dstX0 / i.realZoom, i.dstY0 / i.realZoom);
      cairo_set_source_surface(cr, src, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);

      // Draw the outline.
      double w = lerp(fade * 3, 0, outlineWidth * 100 * height / 1080);
      cairo_rectangle(cr, i.dstX0 - w / 2, i.dstY0 - w / 2, i.dstWidth + w,
                      i.dstHeight + w);
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_set_line_width(cr, w);
      cairo_stroke(cr);
      cairo_restore(cr);
    }

    // Draw the debug overlays.
    auto drawFrame = [&](unsigned x, unsigned y, unsigned w, unsigned h) {
      double wft = 3;
      int crossSize = 10;
      int cx = x + w / 2;
      int cy = y + h / 2;
      cairo_set_line_width(cr, 3);
      cairo_set_operator(cr, CAIRO_OPERATOR_DIFFERENCE);
      cairo_rectangle(cr, x - wft / 2, y - wft / 2, w + wft, h + wft);
      cairo_stroke(cr);
      cairo_move_to(cr, cx - crossSize, cy - crossSize);
      cairo_line_to(cr, cx + crossSize, cy + crossSize);
      cairo_move_to(cr, cx - crossSize, cy + crossSize);
      cairo_line_to(cr, cx + crossSize, cy - crossSize);
      cairo_stroke(cr);
    };
    for (unsigned n = 0; n < numRegions && showWireframe; ++n) {
      auto &i = info[n];
      if (!regions[n].enable)
        continue;
      cairo_set_source_rgb(cr, 1.0, 0.5, 0.5);
      drawFrame(i.srcX0, i.srcY0, i.srcWidth, i.srcHeight);
      cairo_set_source_rgb(cr, 0.5, 1.0, 0.5);
      drawFrame(i.dstX0, i.dstY0, i.dstWidth, i.dstHeight);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(src);
    cairo_surface_destroy(dst);
  }

  inline bool isInFrame(unsigned x, unsigned y) {
    return 0 <= x && x < width && 0 <= y && y < height;
  }

private:
  // double hsync;
  bool showWireframe = false;
  bool showMagnified = false;
  double outlineWidth;
  double pointerWidth;
  double pointerOutlineWidth;
  double fadeDuration;
  double endTime;
  Region regions[numRegions];
};

frei0r::construct<Loupe> plugin("Loupe", "Magnify individual regions",
                                "Fabian Schuiki", 0, 1);

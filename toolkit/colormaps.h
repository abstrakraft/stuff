
#ifndef __TOOLKIT_COLORMAPS_H__
#define __TOOLKIT_COLORMAPS_H__

namespace Colormap {

  // A colormap function is given x in the range 0..1 and returns an RGB color
  // with all components in the range 0..1.
  typedef void (*Function)(float x, float rgb[3]);

  // Some specific colormaps (the names match matlab color maps).
  void Jet(float x, float rgb[3]);
  void Hot(float x, float rgb[3]);
  void Gray(float x, float rgb[3]);
  void HSV(float x, float rgb[3]);
  void Bone(float x, float rgb[3]);
  void Copper(float x, float rgb[3]);
  void Wheel(float x, float rgb[3]);

  // Utility function that computes a 24 bit palette from a colormap function.
  inline void ComputePalette(Function colormap, uint8_t palette[256][3]) {
    for (int i = 0; i < 256; i++) {
      float rgb[3];
      colormap(float(i) / 255.0f, rgb);
      palette[i][0] = rgb[0] * 255;
      palette[i][1] = rgb[1] * 255;
      palette[i][2] = rgb[2] * 255;
    }
  }

}  // namespace Colormap

#endif

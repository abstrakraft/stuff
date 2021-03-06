
#include <math.h>
#include <algorithm>
#include <setjmp.h>
#include <map>
#include "common.h"
#include "mesh.h"
#include "gl_utils.h"
#include "wxgl_font.h"
#include "colormaps.h"
#include "testing.h"
#include "shaders.h"
extern "C" {
  #include "triangle.h"
}

using Eigen::Vector3f;
using Eigen::Vector4f;
using Eigen::Matrix4d;

static const bool kDebugMesh = false;           // Render debug stuff on mesh
static const double kSharpestAllowableAngle = 1e-4;

//***************************************************************************
// Triangle library support. We use nasty globals here because the triangle
// library does not support passing user data to the triunsuitable() callback.

static double square_of_longest_edge_permitted;

// Function called by the triangle library to see if a triangle is too big and
// needs refinement.
extern "C" int triunsuitable(double *v1, double *v2, double *v3, double area) {
  // Compute edge vectors.
  double dx1 = v1[0] - v3[0];
  double dy1 = v1[1] - v3[1];
  double dx2 = v2[0] - v3[0];
  double dy2 = v2[1] - v3[1];
  double dx3 = v1[0] - v2[0];
  double dy3 = v1[1] - v2[1];

  // Find the squares of the lengths of the triangle's three edges.
  double len1 = dx1 * dx1 + dy1 * dy1;
  double len2 = dx2 * dx2 + dy2 * dy2;
  double len3 = dx3 * dx3 + dy3 * dy3;

  // Find the square of the length of the longest edge.
  double maxlen = std::max(len1, std::max(len2, len3));

  return maxlen > square_of_longest_edge_permitted;
}

// Any printf() that the triangle library does probably represents some kind of
// complaint and ends up here.

extern "C" int triprintf(const char *msg, ...) {
  va_list ap;
  va_start(ap, msg);
  GetErrorHandler()->HandleError(ErrorHandler::Message, msg, ap);
  return 0;
}

// This is called by the triangle library to indicate an error.

static jmp_buf triangle_jmp_buf;
extern "C" int triexit(int status) {
  Error("Triangulation failed");
  longjmp(triangle_jmp_buf, 1);
}

// Free heap-allocated data in a triangulateio structure from the triangle
// library. This comes in two forms, based on free() and delete[]. These two
// forms are mostly identical except that valgrind will complain if you mix
// them up.

static void FreeTriangulateIO(triangulateio *t) {
  free(t->pointlist);
  free(t->pointattributelist);
  free(t->pointmarkerlist);
  free(t->trianglelist);
  free(t->triangleattributelist);
  free(t->trianglearealist);
  free(t->neighborlist);
  free(t->segmentlist);
  free(t->segmentmarkerlist);
  free(t->holelist);
  free(t->regionlist);
  free(t->edgelist);
  free(t->edgemarkerlist);
  free(t->normlist);
}

static void DeleteTriangulateIO(triangulateio *t) {
  delete[] t->pointlist;
  delete[] t->pointattributelist;
  delete[] t->pointmarkerlist;
  delete[] t->trianglelist;
  delete[] t->triangleattributelist;
  delete[] t->trianglearealist;
  delete[] t->neighborlist;
  delete[] t->segmentlist;
  delete[] t->segmentmarkerlist;
  delete[] t->holelist;
  delete[] t->regionlist;
  delete[] t->edgelist;
  delete[] t->edgemarkerlist;
  delete[] t->normlist;
}

//***************************************************************************
// Mesh.

Mesh::Mesh(const Shape &s, double longest_edge_permitted, Lua *lua) {
  Trace trace(__func__);
  valid_mesh_ = false;          // Default assumption
  {
    const char *geometry_error = s.GeometryError();
    if (geometry_error) {
      ERROR_ONCE("Can not create mesh: %s", geometry_error);
      return;
    }
  }
  frexp(longest_edge_permitted, &cell_size_);
  cell_size_ -= 2;

  // If we are asked to make a mesh from shapes with extremely short line
  // segments or extremely small interior angles then the mesher will consume a
  // huge amount of time and memory, mainly because of its desire to generate
  // triangles with low aspect ratios. The "short edge" case should be cleaned
  // up by the caller. Detect and warn about the other cases.
  // @@@ Should warn about this once per optimization run, not once per file
  //     reload.
  JetNum sharpest = s.SharpestAngle();
  if (sharpest < kSharpestAllowableAngle) {
    ERROR_ONCE("Can not create mesh because sharpest angle is %g (min is %g)",
               ToDouble(sharpest), kSharpestAllowableAngle);
    return;
  }

  // Identify negative area pieces that will become holes. For each hole pick
  // an x,y point that is guaranteed to be in the hole so that we can identify
  // it to the triangle library. This cumbersome way to identify holes (and
  // regions) is one of the main annoyances of the triangle library. If we have
  // split the model into unmergeable pieces with Paint() then polygon holes
  // may be enclosed by separate pieces but not actually be represented as
  // negative area polygons. To properly identify the holes to the triangle
  // library we need to run clipper to merge everything together and find any
  // negative area polygons that result.
  vector<RPoint> hole_points;
  {
    Shape hole_finder;
    hole_finder.SetMerge(s);
    for (int i = 0; i < hole_finder.NumPieces(); i++) {
      if (hole_finder.Area(i) < 0) {
        RPoint hole_point;
        AnyPointInPoly(hole_finder.Piece(i), -1, &hole_point.p);
        hole_points.push_back(hole_point);
      }
    }
  }

  // Identify holes in this shape.
  int num_holes = 0;
  vector<bool> is_a_hole(s.NumPieces());
  for (int i = 0; i < s.NumPieces(); i++) {
    if (s.Area(i) < 0) {
      is_a_hole[i] = true;
      num_holes++;
    }
  }

  // Make various point indexes. We remove all duplicate points and give the
  // remaining points 'UPI's (unique point indexes). We make also a mapping
  // from UPIs to Piece(i)[j) indexes (this clunky mapping could be avoided
  // if we had a flattened points array and a pieces array which was offsets
  // into it, but doing that would push some complexities elsewhere). Note that
  // duplicate points might happen if there are unmerged polygons with
  // different material types.
  int count = 0;                                // Count of all polys_ points
  for (int i = 0; i < s.NumPieces(); i++) {
    count += s.Piece(i).size();
  }
  typedef std::map<std::pair<JetNum, JetNum>, int> PointMap;
  PointMap point_map;                           // x,y -> UPI
  vector<std::pair<int, int> > index_map;       // UPI -> polys[i].p[j]
  for (int i = 0; i < s.NumPieces(); i++) {
    for (int j = 0; j < s.Piece(i).size(); j++) {
      JetPoint p = s.Piece(i)[j].p;
      if (point_map.count(std::make_pair(p[0], p[1])) == 0) {
        // This is a new point.
        point_map[std::make_pair(p[0], p[1])] = index_map.size();
        index_map.push_back(std::make_pair(i, j));
      }
    }
  }
  int num_unique_points = index_map.size();

  // Setup data structure for 'Triangle' library. Since we are dealing with one
  // or more closed polygons, the number of vertices is equal to the number of
  // segments. Segment-bounded region attributes are set to the polygon indices
  // in polys_, so that triangle material types can be determined from the
  // output triangle attributes.
  //
  // Setting the marker values in the input is important for identifying
  // boundary edges in the output. Points are marked with their UPI (plus 2
  // since 0 and 1 have a reserved meaning in the triangle library). Segments
  // are marked with -1-(the UPI of the first point in the edge). New vertices
  // in the triangulation will pick up the segment marker values.
  triangulateio tin, tout;
  memset(&tin, 0, sizeof(tin));
  memset(&tout, 0, sizeof(tout));
  tin.pointlist = new double[num_unique_points * 2];  // x,y in UPI order
  tin.pointmarkerlist = new int[num_unique_points];   // marker for all points
  tin.numberofpoints = num_unique_points;
  tin.segmentlist = new int[count * 2];         // RPoint indices for segments
  tin.segmentmarkerlist = new int[count];       // Marker for all segments
  tin.numberofsegments = 0;                     // Set below, will be <= count
  tin.holelist = new double[hole_points.size() * 2];  // x,y inside all holes
  tin.numberofholes = hole_points.size();
  tin.regionlist = new double[s.NumPieces() * 4];     // x,y,attr,area for polys
  tin.numberofregions = s.NumPieces() - num_holes;
  // Copy all unique points.
  for (int upi = 0; upi < index_map.size(); upi++) {
    int i = index_map[upi].first;
    int j = index_map[upi].second;
    tin.pointlist[2*upi+0] = ToDouble(s.Piece(i)[j].p[0]);
    tin.pointlist[2*upi+1] = ToDouble(s.Piece(i)[j].p[1]);
    tin.pointmarkerlist[upi] = 2 + upi;
  }
  {
    // Copy all segments. Filter out redundant segments.
    typedef std::map<std::pair<int, int>, bool> SegmentMap;
    SegmentMap segment_map;
    int offset = 0;
    for (int i = 0; i < s.NumPieces(); i++) {
      for (int j = 0; j < s.Piece(i).size(); j++) {
        JetPoint p1 = s.Piece(i)[j].p;
        JetPoint p2 = s.Piece(i)[(j + 1) % s.Piece(i).size()].p;
        PointMap::iterator it1 = point_map.find(std::make_pair(p1[0], p1[1]));
        PointMap::iterator it2 = point_map.find(std::make_pair(p2[0], p2[1]));
        CHECK(it1 != point_map.end() && it2 != point_map.end());
        if (!segment_map[std::make_pair(it1->second, it2->second)]) {
          tin.segmentlist[2*offset+0] = it1->second;
          tin.segmentlist[2*offset+1] = it2->second;
          tin.segmentmarkerlist[offset] = -1 - it1->second;
          segment_map[std::make_pair(it1->second, it2->second)] = true;
          offset++;
        }
      }
    }
    tin.numberofsegments = offset;
  }
  // Set region coordinates and attributes for all non-hole polygons.
  {
    int offset = 0;
    for (int i = 0; i < s.NumPieces(); i++) {
      if (!is_a_hole[i]) {
        // Use AnyPointInPoly() to find a point inside this non-hole polygon.
        // If we didn't consider the holes we may find a point that is actually
        // in a hole, which would give this polygon the wrong region attribute.
        // Thus we must pass both polygon and hole points to AnyPointInPoly.
        vector<RPoint> poly = s.Piece(i);
        for (int j = 0; j < s.NumPieces(); j++) {
          if (is_a_hole[j]) {
            poly.insert(poly.end(), s.Piece(j).begin(), s.Piece(j).end());
          }
        }
        JetPoint point_in_poly;
        AnyPointInPoly(poly, s.Piece(i).size(), &point_in_poly);
        // offsets 0,1 are x,y coord in polygon region.
        tin.regionlist[offset*4 + 0] = ToDouble(point_in_poly[0]);
        tin.regionlist[offset*4 + 1] = ToDouble(point_in_poly[1]);
        tin.regionlist[offset*4 + 2] = i;    // Region attribute (polygon index)
        tin.regionlist[offset*4 + 3] = -1;   // Region max area (ignored)
        offset++;
      }
    }
    CHECK(offset == s.NumPieces() - num_holes);
  }
  // Set coordinates of all holes.
  for (int i = 0; i < hole_points.size(); i++) {
    tin.holelist[i*2 + 0] = ToDouble(hole_points[i].p[0]);
    tin.holelist[i*2 + 1] = ToDouble(hole_points[i].p[1]);
  }

  // Call 'Triangle' library. Use setjmp/longjmp based error handling to catch
  // if the library calls triexit. If this happens then we will leak some
  // memory (no telling what the triangle library was doing internally), but oh
  // well.
  if (setjmp(triangle_jmp_buf) != 0) {
    // triangulate() called triexit.
    return;
  }
  square_of_longest_edge_permitted = sqr(longest_edge_permitted);
  // Useful options to 'triangulate' are:
  //   * z: Index from zero
  //   * p: Triangulate a PSLG
  //   * A: Assign regional attribute to triangles
  //   * Q: Quiet
  //   * V: Verbose (for debugging)
  //   * q: Quality mesh generation by Delaunay refinement
  //   * u: Use triunsuitable function
  //   * n: Create a triangle neighbor list
  if (longest_edge_permitted > 0) {
    triangulate("zpAQqun", &tin, &tout, NULL);
  } else {
    triangulate("zpAQn", &tin, &tout, NULL);
  }

  // Feed output arrays.
  points_.resize(tout.numberofpoints);
  for (int i = 0; i < tout.numberofpoints; i++) {
    points_[i].p[0] = tout.pointlist[i*2 + 0];
    points_[i].p[1] = tout.pointlist[i*2 + 1];

    // Output points that are copied from input points (i.e. are the vertices
    // of boundaries) use the same EdgeInfo. Output points that are created on
    // input segments (i.e. boundary segments) are assigned an EdgeInfo that
    // contains the correct slot information for that edge. The EdgeInfo of
    // output points in the interior of the mesh will never be checked, so we
    // don't do anything regarding those points.
    EdgeInfo e;
    int marker = tout.pointmarkerlist[i];
    if (marker >= 2) {
      // Output point was copied from input point. Copy EdgeInfo of input.
      int upi = marker - 2;
      CHECK(upi < index_map.size());
      int piece = index_map[upi].first;
      int piece_index = index_map[upi].second;
      e = s.Piece(piece)[piece_index].e;
      points_[i].original_piece = piece;
      points_[i].original_edge = piece_index;
    } else if (marker < 0) {
      // Output point was created on boundary segment. Set both slots of
      // EdgeInfo to the edge kind of the boundary segment.
      int upi = -marker - 1;
      CHECK(upi < index_map.size());
      int piece = index_map[upi].first;
      int piece_index1 = index_map[upi].second;
      int piece_index2 = (piece_index1 + 1) % s.Piece(piece).size();
      float d1, d2;
      const RPoint &p1 = s.Piece(piece)[piece_index1];
      const RPoint &p2 = s.Piece(piece)[piece_index2];
      EdgeKind s = p1.e.SharedKind(p2.e, &d1, &d2);
      e.kind[0] = s;
      // Linearly interpolate distance values.
      JetNum len1 = (points_[i].p - p1.p).squaredNorm();
      JetNum len2 = (p2.p - p1.p).squaredNorm();
      double alpha = ToDouble(sqrt(len1 / len2));
      e.dist[0] = alpha * (d2 - d1) + d1;
      points_[i].original_piece = piece;
      points_[i].original_edge = piece_index1;
    } else if (marker == 1) {
      // A marker value of 1 has a reserved meaning in the triangle library.
      // A marker value of 0 will be assigned to interior points.
      Panic("Internal error, marker==1 found");
    }
    points_[i].e = e;
  }
  CHECK(tout.numberofcorners == 3);
  CHECK(tout.numberoftriangleattributes == 1);  // 1 attr from input regions
  CHECK(tout.triangleattributelist);
  triangles_.resize(tout.numberoftriangles);
  for (int i = 0; i < tout.numberoftriangles; i++) {
    int polygon_index = tout.triangleattributelist[i];
    CHECK(polygon_index == tout.triangleattributelist[i]);       // Is integer?
    CHECK(polygon_index >= 0 && polygon_index < s.NumPieces());  // In range?
    triangles_[i].material = polygon_index;     // Index into materials_
    for (int j = 0; j < 3; j++) {
      triangles_[i].index[j] = tout.trianglelist[i*3 + j];

      // If this edge of the triangle does not have another triangle as a
      // neighbor then it is a boundary edge (indicated by -1 in neighborlist).
      // Note that is it not sufficient to identify boundary edges as ones
      // where both vertices are on the boundary. Interior segments will not
      // end up as accidental boundary edges, so we will not get interior ports
      // in the final mesh.
      triangles_[i].neighbor[j] =
          tout.neighborlist[i*3 + (j + 2) % 3];
    }
  }

  // Copy shape materials
  materials_.resize(s.NumPieces());
  for (int i = 0; i < s.NumPieces(); i++) {
    materials_[i] = s.GetMaterial(i);
  }

  // Free heap allocated data. Note that holelist and regionlist are copied
  // from tin to tout so make sure not to free them twice.
  DeleteTriangulateIO(&tin);
  tout.holelist = 0;
  tout.regionlist = 0;
  FreeTriangulateIO(&tout);

  valid_mesh_ = true;
  UpdateDerivatives(s);

  if (lua) {
    DeterminePointDielectric(lua, &dielectric_);
  }
}

void Mesh::DrawMesh(MeshDrawType draw_type, Colormap::Function colormap,
                    int brightness, const Matrix4d &camera_transform) {
  if (draw_type == MESH_HIDE) {
    return;
  }

  if (draw_type == MESH_DIELECTRIC_REAL || draw_type == MESH_DIELECTRIC_IMAG ||
      draw_type == MESH_DIELECTRIC_ABS) {
    if (dielectric_.empty()) {
      return;                             // Nothing to show
    }
    // Create color map.
    const int kNumColors = 256;           // Should be even
    float rgb[kNumColors][3];
    for (int i = 0; i < kNumColors; i++) {
      colormap(float(i) / (kNumColors - 1), rgb[i]);
    }

    // Map the brightness to a scale used for min/max values.
    double scale = pow(10, -(brightness - 500.0) / 500.0);
    const double minval = -scale;
    const double maxval = +scale;

    gl::PushShader push_shader(gl::SmoothShader());
    vector<Vector3f> points, colors;
    for (int i = 0; i < triangles_.size(); i++) {
      for (int j = 0; j < 3; j++) {
        int k = triangles_[i].index[j];
        double value;
        if (draw_type == MESH_DIELECTRIC_REAL) {
          value = ToDouble(dielectric_[k].real());
        } else if (draw_type == MESH_DIELECTRIC_IMAG) {
          value = ToDouble(dielectric_[k].imag());
        } else {
          value = ToDouble(abs(dielectric_[k]));
        }
        int c = std::max(0, std::min(kNumColors - 1,
          int(round((value - minval) * (kNumColors / (maxval-minval))))));
        colors.push_back(Vector3f(rgb[c][0], rgb[c][1], rgb[c][2]));
        points.push_back(Vector3f(ToDouble(points_[k].p[0]),
                                  ToDouble(points_[k].p[1]), 0));
      }
    }
    gl::Draw(points, colors, GL_TRIANGLES);
    return;
  }

  // Drawing regular mesh, not dielectric.
  gl::SetUniform("color", 1, 0, 0);
  vector<Vector3f> points;
  for (int i = 0; i < triangles_.size(); i++) {
    for (int j = 0; j < 3; j++) {
      int k1 = triangles_[i].index[j];
      int k2 = triangles_[i].index[(j + 1) % 3];
      points.push_back(Vector3f(ToDouble(points_[k1].p[0]),
                                ToDouble(points_[k1].p[1]), 0));
      points.push_back(Vector3f(ToDouble(points_[k2].p[0]),
                                ToDouble(points_[k2].p[1]), 0));
    }
  }
  // @@@ Do GL_LINE_LOOP with element drawing and primitive restarting?
  gl::Draw(points, GL_LINES);

  // Print mesh statistics.
  char s[100];
  snprintf(s, sizeof(s), "%d triangles, %d points",
           (int) triangles_.size(), (int) points_.size());
  DrawString(s, 10, 10, *mesh_statistics_font);

  // Debug rendering. Highlight special edge kinds (e.g. ports, ABCs).
  if (kDebugMesh) {
    gl::PushShader push_shader(gl::SmoothShader());
    vector<Vector3f> points, colors;
    for (BoundaryIterator it(this); !it.done(); ++it) {
      if (!it.kind().IsDefault()) {
        colors.push_back(Vector3f(0, it.dist1(), 1 - it.dist1()));
      } else {
        colors.push_back(Vector3f(it.dist1(), 0, 0));
      }
      points.push_back(Vector3f(ToDouble(points_[it.pindex1()].p[0]),
                                ToDouble(points_[it.pindex1()].p[1]), 0));
      if (!it.kind().IsDefault()) {
        colors.push_back(Vector3f(0, it.dist2(), 1 - it.dist2()));
      } else {
        colors.push_back(Vector3f(it.dist2(), 0, 0));
      }
      points.push_back(Vector3f(ToDouble(points_[it.pindex2()].p[0]),
                                ToDouble(points_[it.pindex2()].p[1]), 0));

      //@@@ Too much information?
      if (it.kind().PortNumber()) {
        char s[100];
        snprintf(s, sizeof(s), "%d %.3f-%.3f", it.kind().PortNumber(),
                 it.dist1(), it.dist2());
        DrawStringM(s,
                (ToDouble(points_[it.pindex1()].p[0]) +
                 ToDouble(points_[it.pindex2()].p[0])) / 2,
                (ToDouble(points_[it.pindex1()].p[1]) +
                 ToDouble(points_[it.pindex2()].p[1])) / 2, 0, camera_transform,
                *port_number_font, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER);
      }
    }
    gl::DrawThick(10, 10, false, [&]() {
      gl::Draw(points, colors, GL_LINES);
    });
  }
}

void Mesh::DrawPointDerivatives(double scale) {
  gl::SetUniform("color", 0, 0, 1);
  glPointSize(5);
  vector<Vector3f> p;
  for (int i = 0; i < points_.size(); i++) {
    if (points_[i].original_piece >= 0) {
      p.push_back(Vector3f(ToDouble(points_[i].p[0]),
                           ToDouble(points_[i].p[1]), 0));
      p.push_back(Vector3f(
        ToDouble(points_[i].p[0]) + scale * points_[i].p[0].Derivative(),
        ToDouble(points_[i].p[1]) + scale * points_[i].p[1].Derivative(), 0));
    }
  }
  gl::Draw(p, GL_POINTS);
  gl::Draw(p, GL_LINES);
}

void Mesh::UpdateDerivatives(const Shape &s) {
  // Update point derivatives.
  for (int i = 0; i < points_.size(); i++) {
    int p = points_[i].original_piece;
    int e = points_[i].original_edge;
    if (p >= 0) {
      CHECK(p < s.NumPieces() && e >= 0 && e < s.Piece(p).size());
      const RPoint &p1 = s.Piece(p)[e];
      const RPoint &p2 = s.Piece(p)[(e + 1) % s.Piece(p).size()];
      double alpha = ToVector2d(points_[i].p - p1.p).norm() /
                     ToVector2d(p2.p - p1.p).norm();
      points_[i].p[0].Derivative() =
          (1 - alpha)*p1.p[0].Derivative() + alpha*p2.p[0].Derivative();
      points_[i].p[1].Derivative() =
          (1 - alpha)*p1.p[1].Derivative() + alpha*p2.p[1].Derivative();
    }
  }

  // Update material derivatives.
  CHECK(materials_.size() == s.NumPieces());
  for (int i = 0; i < materials_.size(); i++) {
    CHECK(materials_[i] == s.GetMaterial(i));   // Doesn't compare derivatives
    materials_[i] = s.GetMaterial(i);           // Updates derivatives
  }
}

static inline uint64 GridIndex(int32 ix, int32 iy) {
  return (uint64(uint32(ix)) << 32) | uint32(iy);
}

int Mesh::FindTriangle(double x, double y) {
  // Build the spatial index the first time through. We use a
  // "longest_edge_permitted" constraint in meshing, so the triangles we're
  // indexing will not have large aspect ratios and are all roughly the same
  // size. Therefore the spatial index is a grid of cells of side comparable to
  // "longest_edge_permitted" where we just record all triangles that intersect
  // each grid cell.
  const double cell = ldexp(1, cell_size_);
  if (spatial_index_.empty()) {
    for (int i = 0; i < triangles_.size(); i++) {
      // Compute the bounding box of the triangle.
      const JetPoint *tp[3];            // Triangle points
      for (int j = 0; j < 3; j++) {
        tp[j] = &points_[triangles_[i].index[j]].p;
      }
      double xmin = __DBL_MAX__, xmax = -__DBL_MAX__;
      double ymin = __DBL_MAX__, ymax = -__DBL_MAX__;
      for (int j = 0; j < 3; j++) {
        xmin = std::min(xmin, ToDouble((*tp[j])[0]));
        xmax = std::max(xmax, ToDouble((*tp[j])[0]));
        ymin = std::min(ymin, ToDouble((*tp[j])[1]));
        ymax = std::max(ymax, ToDouble((*tp[j])[1]));
      }
      // Determine the grid cells occupied by the triangle's bounding box. Add
      // all grid cells occupied by the triangle to the index.
      int ixmin = floor(xmin / cell);
      int ixmax = floor(xmax / cell);
      int iymin = floor(ymin / cell);
      int iymax = floor(ymax / cell);
      for (int ix = ixmin; ix <= ixmax; ix++) {
        for (int iy = iymin; iy <= iymax; iy++) {
          if (TriangleIntersectsBox(tp, ix * cell, (ix + 1) * cell,
                                    iy * cell, (iy + 1) * cell)) {
            spatial_index_[GridIndex(ix, iy)].push_back(i);
          }
        }
      }
    }
  }

  // Query the spatial index.
  int32 ix = floor(x / cell);
  int32 iy = floor(y / cell);
  SpatialIndex::const_iterator it = spatial_index_.find(GridIndex(ix, iy));
  if (it == spatial_index_.end()) {
    return -1;
  }
  JetPoint test_point;
  test_point[0] = x;
  test_point[1] = y;
  for (int i = 0; i < it->second.size(); i++) {
    const Triangle &tri = triangles_[it->second[i]];
    if (PointInTriangle(test_point, points_[tri.index[0]].p,
                        points_[tri.index[1]].p, points_[tri.index[2]].p)) {
      return it->second[i];
    }
  }
  // No intersecting triangle found.
  return -1;
}

void Mesh::DeterminePointDielectric(Lua *lua, vector<JetComplex> *dielectric) {
  Trace trace(__func__);
  bool have_callbacks = false;
  for (int i = 0; i < materials_.size(); i++) {
    have_callbacks = have_callbacks || (!materials_[i].callback.empty());
  }
  if (!have_callbacks) {
    dielectric->clear();
    return;
  }
  dielectric->resize(points_.size());
  // Set default to 1.
  for (int i = 0; i < dielectric->size(); i++) {
    (*dielectric)[i] = 1;
  }
  for (int i = 0; i < materials_.size(); i++) {
    // Skip materials without property callback functions.
    if (materials_[i].callback.empty()) {
      continue;
    }
    // Mark all points that are touched by this material.
    vector<bool> mark(points_.size());
    for (int j = 0; j < triangles_.size(); j++) {
      if (triangles_[j].material == i) {
        for (int k = 0; k < 3; k++) {
          mark[triangles_[j].index[k]] = true;
        }
      }
    }
    // Count points that are touched by this material.
    int count = 0;
    for (int j = 0; j < mark.size(); j++) {
      count += mark[j];
    }
    if (count > 0) {
      // Push the callback function to the lua stack.
      materials_[i].GetCallbackFromRegistry(lua->L());
      // Push vectors of x,y coordinates for marked points to the lua stack.
      LuaVector *x = LuaUserClassCreateObj<LuaVector>(lua->L());
      LuaVector *y = LuaUserClassCreateObj<LuaVector>(lua->L());
      x->resize(count);
      y->resize(count);
      count = 0;
      for (int j = 0; j < mark.size(); j++) {
        if (mark[j]) {
          (*x)[count] = points_[j].p[0];
          (*y)[count] = points_[j].p[1];
        }
        count += mark[j];
      }
      // Call the callback function.
      LuaVector *result[2];
      if (!materials_[i].RunCallback(lua, result)) {
        // A lua error message will have been displayed at this point.
        dielectric->clear();
        return;
      }
      // Set point dielectric properties from the callback function's results.
      count = 0;
      for (int j = 0; j < mark.size(); j++) {
        if (mark[j]) {
          if (result[1]) {
            (*dielectric)[j] = JetComplex((*result[0])[count],
                                          (*result[1])[count]);
          } else {
            (*dielectric)[j] = (*result[0])[count];
          }
        }
        count += mark[j];
      }
      lua_pop(lua->L(), 2);
    }
  }
}

//***************************************************************************
// Testing.

TEST_FUNCTION(SpatialIndex) {
  const double kGridSize = 0.1;
  Shape s;
  s.AddPoint(0, 0);
  s.AddPoint(1, 0);
  s.AddPoint(1, 1);
  s.AddPoint(0, 1);
  Mesh m(s, kGridSize, NULL);

  // Create spatial index and make sure each triangle in each grid cell
  // intersects the cell.
  m.FindTriangle(0, 0);
  const double kCell = ldexp(1, m.cell_size_);
  printf("Spatial index for %d points, %d triangles:\n",
         (int)m.points_.size(), (int)m.triangles_.size());
  for (int y = -2; y <= 34; y++) {
    for (int x = -2; x <= 34; x++) {
      uint64 index = GridIndex(x, y);
      Mesh::SpatialIndex::const_iterator it = m.spatial_index_.find(index);
      if (it == m.spatial_index_.end()) {
        printf(" .");
      } else {
        int n = it->second.size();
        printf("%2d", n);
        for (int i = 0; i < n; i++) {
          int index = it->second[i];
          const JetPoint *p[3];
          for (int k = 0; k < 3; k++) {
            p[k] = &m.points_[m.triangles_[index].index[k]].p;
          }
          CHECK(TriangleIntersectsBox(p, x*kCell, (x+1)*kCell,
                                         y*kCell, (y+1)*kCell));
        }
      }
    }
    printf("\n");
  }

  // Do a bunch of queries and make sure the returned triangles intersect the
  // query points.
  for (int i = 0; i < 10000; i++) {
    double x = RandDouble()*0.998 + 0.001;
    double y = RandDouble()*0.998 + 0.001;
    int t = m.FindTriangle(x, y);
    CHECK(t >= 0 && t < m.triangles_.size());
    JetPoint xy;
    xy[0] = x;
    xy[1] = y;
    CHECK(PointInTriangle(xy, m.points_[m.triangles_[t].index[0]].p,
                              m.points_[m.triangles_[t].index[1]].p,
                              m.points_[m.triangles_[t].index[2]].p) != 0);
  }

  // Do a bunch of queries outside the shape.
  for (int i = 0; i < 10000; i++) {
    double x = RandDouble();
    x += (x > 0.5) ? 0.6 : -0.6;
    double y = RandDouble();
    y += (y > 0.5) ? 0.6 : -0.6;
    int t = m.FindTriangle(x, y);
    CHECK(t == -1);
  }
}


#ifndef __SHAPE_H__
#define __SHAPE_H__

#include <myvector>
#include "lua_util.h"
#include "common.h"
#include "clipper.h"
#include "edge_type.h"
#include "Eigen/Dense"
#include "Eigen/Geometry"
#include "my_jet.h"
#include "lua_vector.h"

using std::vector;

// All 2D coordinates should be JetPoint, so that the derivative of geometry
// with respect to parameters can be tracked.
typedef Eigen::Matrix<JetNum, 2, 1> JetPoint;

// 2D transformations can use this matrix type.
typedef Eigen::Matrix<JetNum, 2, 2> JetMatrix2d;

// A 2D point in a polygon or in a mesh. RPoint means "rama point" and is
// not named Point to avoid conflicts with the same structure in MacTypes.h.
struct RPoint {
  JetPoint p;
  EdgeInfo e;

  // For mesh points on the boundary this links to the piece and edge of the
  // original shape. For all other points these are -1.
  int original_piece, original_edge;

  RPoint() : original_piece(-1), original_edge(-1) { p.setZero(); }
  RPoint(JetNum x, JetNum y) : p(x, y), original_piece(-1), original_edge(-1) {}

  bool operator==(const RPoint &q) const {
    return p == q.p && e == q.e;
  }
  bool operator!=(const RPoint &q) const {
    return p != q.p || e != q.e;
  }
  // Less-than operator used to order points in map<> and similar containers.
  bool operator<(const RPoint &q) const {
    return p[0] < q.p[0] || (p[0] == q.p[0] && p[1] < q.p[1]);
  }
};

struct Material {        // Polygon and triangle material properties
  uint32 color;          // 0xrrggbb color (for drawing only, not simulation)
  JetComplex epsilon;    // For EM simulation: multiplies k^2
  std::string callback;  // MD5 hash of callback function.
  // If 'callback' is not empty then it is the MD5 hash of the callback
  // function that makes parameters from (x,y) coordinates. It is also the key
  // for looking up this function in the registry. It is assumed by operator==
  // that if two functions have the same hash then they will compute the same
  // values (if given the same parameters). Given that a 16 byte hash is used,
  // this is extremely likely to be true.

  Material() {
    color = 0xe0e0ff;
    epsilon = 1;        // i.e. vacuum
  }
  bool operator==(const Material &a) const {
    return color == a.color && epsilon == a.epsilon && callback == a.callback;
  }

  // Mechanism to set parameters from a Lua argument list.
  static int MaxParameters() { return 2; }
  void SetParameters(const vector<JetNum> &list) {
    if (list.size() == 1) {
      epsilon = list[0];
    } else if (list.size() >= 2) {
      epsilon = JetComplex(list[0], list[1]);
    }
  }

  // Pop a function from the lua stack, store its hash in 'callback' and write
  // it to the registry, using the hash as the registry key.
  void SetCallbackToRegistry(lua_State *L);

  // Push a function to the lua stack from the registry, using the hash in
  // 'callback' as the registry key.
  void GetCallbackFromRegistry(lua_State *L);

  // Helper for running the callback function one time. After the callback and
  // x,y vectors are pushed on to the stack, call this to run the callback.
  // Return 1 or 2 vector results (the second one can be 0). Returns true on
  // success or false if there is a problem (in which case LuaError() will have
  // been called).
  static bool RunCallback(Lua *lua, LuaVector *result[2]) MUST_USE_RESULT;
};

struct Triangle {
  int index[3];         // Three point indexes define this triangle
  int material;         // Material index in containing Mesh object
  int neighbor[3];      // [i]=index of neighbor triangle for edge
};                      //   index[i]->index[(i+1)%3], or -1 if boundary edge

class Shape : public LuaUserClass {
 public:
  ~Shape();
  // Default copy constructor and assignment operator are ok.

  // Test for exact [in]equality of two shapes, i.e. not just the same outline
  // but the same order of vertices and same edge kinds.
  bool operator==(const Shape &s) const { return polys_ == s.polys_; }
  bool operator!=(const Shape &s) const { return polys_ != s.polys_; }

  // Return 0 if the shape geometry is well formed, otherwise return an error
  // message string. If enforce_positive_area is true then only positive area
  // shapes with negative area holes are allowed.
  const char *GeometryError(bool enforce_positive_area = true) const;

  // Hook up global functions like 'Rectangle' (etc) to lua.
  static void SetLuaGlobals(lua_State *L);

  // Swap two shapes (a fast way to exchange data).
  void Swap(Shape *s) { polys_.swap(s->polys_); }

  // Set the empty shape.
  void Clear();

  // Dump shape data, for debugging.
  void Dump() const;

  // Draw shape to OpenGL.
  void DrawInterior() const;
  void DrawBoundary(const Eigen::Matrix4d &camera_transform,
                    bool show_lines_and_ports, bool show_vertices,
                    double boundary_derivatives_scale = 0) const;

  // Return true if this shape is completely empty.
  bool IsEmpty() const { return polys_.empty() || polys_[0].p.empty(); }

  // Return the number of separate pieces (polygons) in this shape. Disjoint
  // pieces count, interior holes count.
  int NumPieces() const { return polys_.size(); }

  // Return the polygon for the n'th piece.
  const vector<RPoint>& Piece(int n) const { return polys_[n].p; }

  // Return the material for the n'th piece.
  const Material& GetMaterial(int n) const { return polys_[n].material; }

  // Set this polygon to the n'th piece of 'p' (p can be this).
  void SetToPiece(int n, const Shape &p);

  // Set the given piece,edge to the given edge kind. Return true on success or
  // false if there is a conflict on this edge.
  bool AssignPort(int piece, int edge, EdgeKind kind) MUST_USE_RESULT;

  // Return the orientation of the n'th piece. True is outer (anticlockwise)
  // orientation, false is inner (clockwise) orientation.
  bool Orientation(int n) const { return Area(n) >= 0; }

  // Return the area of the n'th piece. Positive areas are returned for outer
  // pieces, negative areas are returned for inner pieces.
  JetNum Area(int n) const;

  // Return the total area of all pieces.
  JetNum TotalArea() const;

  // Return the sharpest convex angle in the shape, i.e. the angle that would
  // result in the mesher generating the most triangles. 0 is maximum
  // sharpness, pi is least sharp.
  JetNum SharpestAngle() const;

  // Return the largest and smallest side lengths.
  void ExtremeSideLengths(JetNum *length_max, JetNum *length_min) const;

  // Add a point to the last piece in the shape.
  void AddPoint(JetNum x, JetNum y);

  // Turn the last piece of this shape into a polyline by adding points
  // s[#s-1],...,s[2] to the polygon. This makes a zero area polygon to
  // represent the polyline.
  void MakePolyline();

  // Set this shape to the given rectangle.
  void SetRectangle(JetNum x1, JetNum y1, JetNum x2, JetNum y2);

  // Set this shape to the given circle.
  void SetCircle(JetNum x, JetNum y, JetNum radius, int npoints);

  // Set to the intersection, union (etc) of shapes c1 and c2. Either c1 or c2
  // can be this shape.
  void SetIntersect(const Shape &c1, const Shape &c2);
  void SetUnion(const Shape &c1, const Shape &c2);
  void SetDifference(const Shape &c1, const Shape &c2);
  void SetXOR(const Shape &c1, const Shape &c2);

  // Paint material properties into this shape at 's'. This potentially splits
  // the polygons into unmerged pieces with different material properties.
  void Paint(const Shape &s, const Material &mat);

  // Set this shape to 's', but merge together any adjacent pieces, erasing the
  // distinction between different materials. This undoes the effects of
  // Paint().
  void SetMerge(const Shape &s);

  // Get the boundary rectangle of this shape. It is a runtime error if the
  // shape is empty so the caller must check for that.
  void GetBounds(JetNum *min_x, JetNum *min_y,
                 JetNum *max_x, JetNum *max_y) const;

  // Return 0 if x,y is outside the shape, +1 if it is inside the shape, or -1
  // if x,y is exactly on boundary. Note that a point inside a polygon hole
  // will be considered to be outside.
  int Contains(JetNum x, JetNum y);

  // Transform the shape. All transformations except Reverse() preserve the
  // orientation.
  void Offset(JetNum dx, JetNum dy);
  void Scale(JetNum scalex, JetNum scaley);
  void Rotate(JetNum theta);            // theta in degrees
  void MirrorX(JetNum x_coord);         // Mirror about x == x_coord
  void MirrorY(JetNum y_coord);         // Mirror about y == y_coord
  void Reverse();                       // Reverse orientation

  // Grow or shrink the shape by 'delta'.
  //   - Only works with area >= 0 polygons.
  //   - For the miter style, 'limit' is the miter limit (e.g. 2).
  //   - For the round style, 'limit' is the maximum distance allowed between
  //     the polygon approximation of an arc and a true circle.
  enum CornerStyle { SQUARE, ROUND, MITER, BUTT };
  void Grow(JetNum delta, CornerStyle style, JetNum limit,
            CornerStyle endcap_style = BUTT);

  // Clean up the shape: remove vertices that are closer than 'threshold' to
  // other vertices. If threshold is zero then use a default that is a small
  // fraction of the largest side length.
  void Clean(JetNum threshold = 0);

  // Return the piece and edge that is closest to x,y. The edge index is
  // actually a vertex number N such that the edge is from vertex N to vertex
  // N+1. It is a runtime error if the shape is empty.
  void FindClosestEdge(JetNum x, JetNum y, int *piece, int *edge);

  // Return the piece and vertex index that is closest to x,y. It is a runtime
  // error if the shape is empty.
  void FindClosestVertex(JetNum x, JetNum y, int *piece, int *index);

  // Return a point that is guaranteed to be inside the shape. This will
  // succeed and return true for a nonempty single piece polygon of any
  // orientation, or a single positive area polygon with any number of
  // negative area holes. Other unhandled cases will fail and return false.
  bool APointInside(double *x, double *y);

  // Add a fillet of the given radius to the vertex closest to x,y. The limit
  // is the maximum distance allowed between a polygon approximation of an arc
  // and a true circle. It is a runtime error if the shape is empty.
  void FilletVertex(JetNum x, JetNum y, JetNum radius, JetNum limit);

  // Add a chamfer of the given pre- and post-vertex distances to the vertex
  // closest to x,y. It is a runtime error if the shape is empty.
  void ChamferVertex(JetNum x, JetNum y, JetNum predist, JetNum postdist);

  // Save the shape to various file formats.
  void SaveBoundaryAsDXF(const char *filename);
  void SaveBoundaryAsXY(const char *filename);

  //.........................................................................
  // Lua interface. All lua functions that don't return some other value will
  // return the userdata object, so that calls can be chained together like
  // Shape():foo():bar().

  int Index(lua_State *L);
  int FunctionCall(lua_State *L);
  int Length(lua_State *L);
  bool Operator(lua_State *L, int op, int pos);

  int LuaClone(lua_State *L);
  int LuaAddPoint(lua_State *L);
  int LuaMakePolyline(lua_State *L);
  int LuaContains(lua_State *L);
  int LuaOffset(lua_State *L);
  int LuaScale(lua_State *L);
  int LuaRotate(lua_State *L);
  int LuaMirrorX(lua_State *L);
  int LuaMirrorY(lua_State *L);
  int LuaReverse(lua_State *L);
  int LuaGrow(lua_State *L);
  int LuaSelect(lua_State *L);
  int LuaSelectAll(lua_State *L);
  int LuaPort(lua_State *L);
  int LuaABC(lua_State *L);
  int LuaAPointInside(lua_State *L);
  int LuaFilletVertex(lua_State *L);
  int LuaChamferVertex(lua_State *L);
  int LuaPaint(lua_State *L);
  int LuaClean(lua_State *L);

 private:
  // The shape is a vector of pieces. Each piece is a vector of points that is
  // a closed polygon, along with some auxiliary information. Each polygon's
  // winding direction determines whether it is an outer boundary or an inner
  // hole. Each outer boundary is disjoint, though they may share common edges
  // or points, and they may have different material properties.
  struct Polygon {
    vector<RPoint> p;           // Points on polygon boundary
    Material material;          // Material of this polygon interior

    bool operator==(const Polygon &a) const {
      return p == a.p && material == a.material;
    }
    bool operator!=(const Polygon &a) const { return !operator==(a); }

    void Swap(Polygon &a) {
      p.swap(a.p);
      Material tmp = material;
      material = a.material;
      a.material = tmp;
    }
  };
  vector<Polygon> polys_;

  int UpdateBounds(JetNum *min_x, JetNum *min_y, JetNum *max_x, JetNum *max_y)
      const;
  void ToPaths(JetNum scale, JetNum offset_x, JetNum offset_y,
               ClipperLib::Paths *paths) const;
  void FromPaths(JetNum scale, JetNum offset_x, JetNum offset_y,
                 const ClipperLib::Paths &paths);
  void RunClipper(const Shape *c1, const Shape *c2,
                  ClipperLib::ClipType clip_type);
  bool ClipperBounds(const Shape *c1, const Shape *c2, JetNum *offset_x,
                     JetNum *offset_y, JetNum *scale) const;
  void RunClipper(const Shape *c1, const Shape *c2,
                  ClipperLib::ClipType clip_type,
                  JetNum offset_x, JetNum offset_y, JetNum scale);
  const Shape &LuaCheckShape(lua_State *L, int argument_index) const;
};

// ********** Public geometry utility functions.

// Returns 0 if 'p' is not in the triangle defined by a,b,c, +1 if it is, or -1
// if pt is on the triangle boundary.
int PointInTriangle(const JetPoint &p, const JetPoint &a,
                    const JetPoint &b, const JetPoint &c);

// Return true if the triangle (p[0],p[1],p[2]) intersects the box.
bool TriangleIntersectsBox(const JetPoint *p[3], double xmin, double xmax,
                           double ymin, double ymax);

// Given a well formed polygon, return a point that is guaranteed to be inside
// it (regardless of its orientation). If poly_size is -1 then it is ignored.
// If the polygon has positive area and contains holes in which the returned
// point should not lie then poly_size is the number of initial points in
// 'poly' that contain the actual polygon, and the remaining points in 'poly'
// are from all of the holes. This scheme allows the algorithm to not have to
// treat hole polygon points any differently. It is a fatal error if the
// polygon is not well formed.
void AnyPointInPoly(const vector<RPoint> &poly, int poly_size,
                    JetPoint *point);

// Convert JetPoints to Vector2d.
inline Eigen::Vector2d ToVector2d(const JetPoint &p) {
  return Eigen::Vector2d(ToDouble(p[0]), ToDouble(p[1]));
}

#endif

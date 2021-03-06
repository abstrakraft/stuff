
#include "viewer.h"
#include "color_based_selection.h"
#include <algorithm>

using namespace Eigen;

//***************************************************************************
// GLViewer.

BEGIN_EVENT_TABLE(GLViewer, wxGLCanvas)
  EVT_PAINT(GLViewer::OnPaint)
  EVT_SIZE(GLViewer::OnSize)
  EVT_ERASE_BACKGROUND(GLViewer::OnEraseBackground)
  EVT_MOUSE_EVENTS(GLViewer::OnMouseEvent)
  EVT_MOUSE_CAPTURE_LOST(GLViewer::OnCaptureLost)
END_EVENT_TABLE()

GLViewer::GLViewer(wxWindow* parent, wxWindowID id, const wxPoint &pos,
                   const wxSize &size, long style, int gl_type)
    : wxGLCanvas(parent, id, gl::GetAttributeList(gl_type), pos, size,
                 style, "") {
  gl_type_ = gl_type;
  context_ = new wxGLContext(this);
  camera_ = &cameras_[0];
  allow_rotation_ = true;
  last_x_ = last_y_ = buttons_ = the_button_ = 0;
  cmd_click_ = false;
}

GLViewer::~GLViewer() {
  delete context_;
}

void GLViewer::SetPerspective(bool perspective) {
  camera_->perspective = perspective;
  Refresh();
}

void GLViewer::SetViewAngle(double angle) {
  camera_->view_angle = angle;
  Refresh();
}

void GLViewer::ZoomExtents() {
  double bounds[6];
  GetBoundingBox(bounds);
  camera_->EnsureBoxVisible(bounds, GetAspectRatio());
  Refresh();
}

void GLViewer::Zoom(double scale_factor) {
  int width, height;
  GetScaledClientSize(&width, &height);
  Vector3d p;
  PixelToModelCoords(width / 2, height / 2, &p);
  Zoom(scale_factor, p);
}

void GLViewer::Zoom(double scale_factor, Vector3d center) {
  camera_->Zoom(scale_factor, center);
  Refresh();
}

void GLViewer::SwitchCamera(int camera_number) {
  CHECK(camera_number >= 0 && camera_number < MAX_CAMERAS);
  camera_ = &cameras_[camera_number];
  Refresh();
}

void GLViewer::Look(Direction direction) {
  // Set the camera direction then ensure the bounding box is visible.
  switch (direction) {
    case LOOK_AT_XY_PLANE_FROM_PLUS_Z:
      camera_->SetCameraPlane(Vector3d(1, 0, 0), Vector3d(0, 1, 0));
      break;
    case LOOK_AT_YX_PLANE_FROM_PLUS_Z:
      camera_->SetCameraPlane(Vector3d(0, 1, 0), Vector3d(-1, 0, 0));
      break;
    case LOOK_AT_XY_PLANE_FROM_MINUS_Z:
      camera_->SetCameraPlane(Vector3d(-1, 0, 0), Vector3d(0, 1, 0));
      break;
    case LOOK_AT_YX_PLANE_FROM_MINUS_Z:
      camera_->SetCameraPlane(Vector3d(0, 1, 0), Vector3d(1, 0, 0));
      break;
    case LOOK_AT_XZ_PLANE_FROM_PLUS_Y:
      camera_->SetCameraPlane(Vector3d(-1, 0, 0), Vector3d(0, 0, 1));
      break;
    case LOOK_AT_ZX_PLANE_FROM_PLUS_Y:
      camera_->SetCameraPlane(Vector3d(0, 0, 1), Vector3d(1, 0, 0));
      break;
    case LOOK_AT_XZ_PLANE_FROM_MINUS_Y:
      camera_->SetCameraPlane(Vector3d(1, 0, 0), Vector3d(0, 0, 1));
      break;
    case LOOK_AT_ZX_PLANE_FROM_MINUS_Y:
      camera_->SetCameraPlane(Vector3d(0, 0, 1), Vector3d(-1, 0, 0));
      break;
    case LOOK_AT_YZ_PLANE_FROM_PLUS_X:
      camera_->SetCameraPlane(Vector3d(0, 1, 0), Vector3d(0, 0, 1));
      break;
    case LOOK_AT_ZY_PLANE_FROM_PLUS_X:
      camera_->SetCameraPlane(Vector3d(0, 0, 1), Vector3d(0, -1, 0));
      break;
    case LOOK_AT_YZ_PLANE_FROM_MINUS_X:
      camera_->SetCameraPlane(Vector3d(0, -1, 0), Vector3d(0, 0, 1));
      break;
    case LOOK_AT_ZY_PLANE_FROM_MINUS_X:
      camera_->SetCameraPlane(Vector3d(0, 0, 1), Vector3d(0, 1, 0));
      break;
  }
  double bounds[6];
  GetBoundingBox(bounds);
  camera_->EnsureBoxVisible(bounds, GetAspectRatio());
  Refresh();
}

void GLViewer::PixelToModelCoords(int x, int y, Vector3d *model_pt) {
  // Find the model point at x,y. If no model point was found then project the
  // far-clip-plane point returned so that the depth is in the center of the
  // object's bounding volume. This will help the camera adjustment code do
  // sensible things.
  if (gl_type_ & gl::DepthBuffer) {
    // A depth buffer is available to compute the full tranformation.
    if (!gl::PixelToModelCoordinates(x, y, gl::Transform(), model_pt)) {
      // x,y is at maximum depth (the far clip plane) so instead assume a depth
      // at the middle of the bounding box.
      double bounds[6];
      GetBoundingBox(bounds);
      Vector3d center((bounds[0] + bounds[1]) / 2.0,
                      (bounds[2] + bounds[3]) / 2.0,
                      (bounds[4] + bounds[5]) / 2.0);
      Vector3d ev = camera_->EyeVector();
      if (camera_->perspective) {
        Vector3d pos_to_farclip = *model_pt - camera_->pos;
        *model_pt = camera_->pos + (ev.dot(center - camera_->pos) /
                                    ev.dot(pos_to_farclip)) * pos_to_farclip;
      } else {
        *model_pt += ev * ev.dot(center - *model_pt);
      }
    }
  } else {
    // No depth buffer is available, choose a depth to get a model z coordinate
    // of zero.
    Vector3d m, dm_by_ddepth;
    gl::PixelToModelCoordinates(Vector3d(x, y, 0), gl::Transform(),
                                &m, &dm_by_ddepth);
    double alpha = -m[2] / dm_by_ddepth[2];
    *model_pt = m + alpha * dm_by_ddepth;
  }
}

bool GLViewer::ModelToPixelCoords(const Vector3d &model_pt,
                                  double *px, double *py) {
  int width, height;
  GetScaledClientSize(&width, &height);
  Vector3d v;
  gl::ModelToPixelCoordinates(model_pt, gl::Transform(), &v);
  if (v[0] >= 0 && v[0] <= width && v[1] >= 0 && v[1] <= height && v[2] > 0) {
    *px = v[0];
    *py = v[1];
    return true;
  }
  return false;
}

void GLViewer::HandleClick(int x, int y, bool button,
                           const Vector3d &model_pt) {
}

void GLViewer::HandleDrag(int x, int y, const Vector3d &model_pt) {
}

void GLViewer::OnPaint(wxPaintEvent &event) {
  // We ALWAYS need to do this under windows, even for OpenGL windows, because
  // otherwise drawing for *other* windows will get screwed up.
  wxPaintDC dc(this);

  // Nothing to do if the window is not visible.
  if(!IsShown()) {
    return;
  }

  // Reset GL state.
  wxGLCanvas::SetCurrent(*context_);
  ApplyViewport();

  // Draw everything.
  Draw();

  // Complain if there were OpenGL errors.
  int err;
  while ((err = glGetError()) != GL_NO_ERROR) {
    Error("GL error %d (%s)", err, gl::ErrorString(err));
  }

  // Done.
  glFlush();
  SwapBuffers();
}

void GLViewer::OnSize(wxSizeEvent &evt) {
  Refresh();
}

void GLViewer::OnEraseBackground(wxEraseEvent &event) {
  // Do nothing, to avoid flashing on MSW.
}

void GLViewer::OnMouseEvent(wxMouseEvent &event) {
  // Maintain the currently pressed button mask. Doing this using
  // event.*IsDown() functions is not reliable, e.g. on a mac pressing multiple
  // buttons can result in some of those functions incorrectly returning false.
  int last_buttons = buttons_;
  if (event.LeftDown() || event.LeftDClick()) {
    buttons_ |= 1;
  } else if (event.MiddleDown() || event.MiddleDClick()) {
    buttons_ |= 2;
  } else if (event.RightDown() || event.RightDClick()) {
    buttons_ |= 4;
  } else if (event.LeftUp()) {
    buttons_ &= ~1;
  } else if (event.MiddleUp()) {
    buttons_ &= ~2;
  } else if (event.RightUp()) {
    buttons_ &= ~4;
  }

  // Make sure the mouse is captured when any button is pressed. This is
  // required to make drags outside the window work on windows, and it also
  // ensures that button up/down events will be paired and so the buttons_
  // state will be correct.
  if (buttons_ && !HasCapture()) {
    CaptureMouse();
  }
  if (buttons_ == 0 && HasCapture()) {
    ReleaseMouse();
  }

  // Multiple buttons held down at once have no special UI meaning here. To
  // prevent confusion for the user (and the code below), once a button is
  // pressed we ignore the other buttons until all buttons are released. This
  // prevents us from having to specially handle tricky cases like LeftDown ->
  // RightDown -> LeftUp -> RightUp, which could otherwise easily result in
  // inconsistent UI state.
  int last_the_button = the_button_;
  if (last_buttons == 0 && buttons_) {
    // No buttons were pressed, then one button was pressed, this will become
    // "the button" until all buttons are released.
    the_button_ = buttons_;             // Will be 1, 2 or 4
  } else if (buttons_ == 0) {
    the_button_ = 0;
  }

  // The bits in event.GetModifiers() depend on the OS:
  //                    Mac                   Windows
  //                 +---------------------+-----------------+
  // Shift           |  wxMOD_SHIFT        |  wxMOD_SHIFT    |
  // Ctrl            |  wxMOD_RAW_CONTROL  |  wxMOD_CONTROL  |
  // Option/Alt      |  wxMOD_ALT          |  wxMOD_ALT      |
  // Cmd             |  wxMOD_CONTROL      |  n/a            |
  //                 +---------------------+-----------------+
  #ifdef __APPLE__
    const int kCtrlBit = wxMOD_RAW_CONTROL;
    const int kCmdBit = wxMOD_CONTROL;
  #else
    const int kCtrlBit = wxMOD_CONTROL;
    const int kCmdBit = -1;
  #endif

  // Get window properties.
  int width, height;
  GetClientSize(&width, &height);
  double scale = GetContentScaleFactor();       // Usually 1 or 2

  // If we are not dragging and the mouse leaves the window, set the "last
  // model coordinate" to the center of the window, so that zoom in and out
  // will operate with respect to the window center.
  if (buttons_ == 0 && event.GetEventType() == wxEVT_LEAVE_WINDOW) {
    last_x_ = width * scale / 2;
    last_y_ = height * scale / 2;
    return;
  }

  // Ignore enter events.
  if (event.GetEventType() == wxEVT_ENTER_WINDOW) {
    return;
  }

  // Get mouse position in opengl viewport coordinates (Y inverted from window
  // coordinates).
  int x = (event.GetX()) * scale;
  int y = (height - 1 - event.GetY()) * scale;

  #ifdef __APPLE__
    // On OS X retina displays there appears to be a slight vertical shift
    // between the opengl window and the reported mouse coordinates.
    if (scale == 2) {
      y += 5;
    }
    #if wxMAJOR_VERSION != 3 || wxMINOR_VERSION != 1 || wxRELEASE_NUMBER != 0
    #error Verify the above y-shift is still needed for this wxWidgets version
    #endif
  #endif

  // Compute the delta position since the last mouse event.
  int dx = x - last_x_;
  int dy = y - last_y_;

  // Handle events.
  if (last_the_button == 0 && the_button_) {
    SetFocus();

    // Find the model point that was clicked on.
    PixelToModelCoords(x, y, &model_pt_);

    // Handle left button clicks in subclass code. On mac, cmd+left click
    // starts a rotation.
    cmd_click_ = false;
    if (the_button_ == 1) {
      if (event.GetModifiers() == kCmdBit) {
        cmd_click_ = true;
      } else {
        HandleClick(x, y, true, model_pt_);
      }
    }
    last_x_ = x;
    last_y_ = y;
  }
  else if (last_the_button == 1 && the_button_ == 0 && !cmd_click_) {
    HandleClick(x, y, false, model_pt_);
  }
  else if (event.Dragging()) {
    if (buttons_ == 0) {
      // Normally this event is only received if a mouse button is down. On
      // windows however we can be fooled when the user clicks "ok" in a file
      // dialog that is right on top of the viewer window - the dialog
      // disappears leaving the mouse down and dragging on the viewer window,
      // but the viewer window never got the button down message. If we detect
      // this inconsistency then just ignore the event.
      return;
    }
    if (the_button_ == 1 && event.GetModifiers() == 0) {
      HandleDrag(x, y, model_pt_);
    } else if (the_button_ == 2) {
      // Rotating.
      if (allow_rotation_) {
        camera_->Trackball(dx / 100.0, dy / 100.0, model_pt_);
        Refresh();
      }
    } else if (the_button_ == 4 && event.GetModifiers() == 0) {
      // Panning.
      camera_->Pan(dx, dy, model_pt_, width * scale);
      Refresh();
    } else if (the_button_ == 4 && event.GetModifiers() == kCtrlBit) {
      // Zooming.
      camera_->Zoom(pow(2, -dy / 50.0), model_pt_);
      Refresh();
    }
    last_x_ = x;
    last_y_ = y;
  }
  else if (event.Moving()) {
    if (cmd_click_) {
      // Rotating.
      if (event.GetModifiers() == kCmdBit) {
        if (allow_rotation_) {
          camera_->Trackball(dx / 100.0, dy / 100.0, model_pt_);
          Refresh();
        }
      } else {
        cmd_click_ = false;
      }
    }
    last_x_ = x;
    last_y_ = y;
  }
  else if (event.GetWheelRotation()) {
    double r = event.GetWheelRotation();
    PixelToModelCoords(last_x_, last_y_, &model_pt_);
    if (event.GetWheelDelta() == 1) {
      // We have precise scrolling deltas, probably from a mac trackpad. Use
      // this for panning.
      r = (r > 0) ? 4 : -4;
      if (event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL) {
        camera_->Pan(r * 3, 0, model_pt_, width * scale);
      } else {
        camera_->Pan(0, r * 3, model_pt_, width * scale);
      }
    } else {
      // We have imprecise scrolling deltas, probably from a USB 3-button
      // mouse. Use this for zooming.
      camera_->Zoom(pow(2, -r * 0.02), model_pt_);
    }
    Refresh();
    // Don't update last_x_,last_y_ in case it was set by the the handler for
    // wxEVT_LEAVE_WINDOW.
  } else if (event.Magnify()) {
    // OS X pinch-to-zoom events.
    PixelToModelCoords(last_x_, last_y_, &model_pt_);
    camera_->Zoom(pow(4, -event.GetMagnification()), model_pt_);
    Refresh();
    // Don't update last_x_,last_y_ in case it was set by the the handler for
    // wxEVT_LEAVE_WINDOW.
  }
}

void GLViewer::OnCaptureLost(wxMouseCaptureLostEvent &event) {
  // Required to prevent capture handler from complaining.
}

double GLViewer::GetAspectRatio() {
  int width, height;
  GetClientSize(&width, &height);
  return double(width) / double(height);
}

void GLViewer::ApplyViewport() {
  int window_width, window_height;
  GetScaledClientSize(&window_width, &window_height);
  glViewport(0, 0, window_width, window_height);
}

void GLViewer::ApplyCameraTransformations(const Matrix4d &M) {
  double bounds[6];
  GetBoundingBox(bounds);
  gl::ApplyTransform(M * camera_->Projection(bounds, GetAspectRatio()),
                     camera_->ModelView());
}

void GLViewer::GetScaledClientSize(int *window_width, int *window_height) {
  GetClientSize(window_width, window_height);
  double scale = GetContentScaleFactor();       // Usually 1 or 2
  *window_width *= scale;
  *window_height *= scale;
}

//***************************************************************************
// GLViewerWithSelection.

GLViewerWithSelection::GLViewerWithSelection(wxWindow* parent, wxWindowID id,
            const wxPoint &pos, const wxSize &size, long style, int gl_type)
    : GLViewer(parent, id, pos, size, style, gl_type) {
}

GLViewerWithSelection::~GLViewerWithSelection() {
}

int GLViewerWithSelection::FindObject(int x, int y) {
  // Apply new state.
  wxGLCanvas::SetCurrent(*context_);
  ApplyViewport();
  gl::ColorBasedSelection color_based_selection(x, y);
  ApplyCameraTransformations(color_based_selection.PickMatrix());

  // Draw all objects.
  DrawForSelection(color_based_selection);
  return color_based_selection.GetSelection();
}

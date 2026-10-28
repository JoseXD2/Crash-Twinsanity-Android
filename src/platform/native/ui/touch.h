#pragma once

// The touch controls (Android, or $TWIN_TOUCH=1 elsewhere): a floating left stick in the left part of the screen, the face
// buttons, the shoulders, start and select drawn over the picture (touch.cpp draws them through the overlay), and the right
// stick as a drag anywhere else in the right part. What they give is added to the pad's read (pads.cpp)
#include "common.h"

#include <vector>

#include "ui/overlay.h"

namespace NativeUi
{
struct BoundPad;

// Whether the touch controls are in use: Android (and $TWIN_TOUCH=1 to try them elsewhere)
bool TouchControlsEnabled();
// Watches the touch events (once, from the pad's read)
void StartTouchControls();
// The touch controls' buttons and sticks added to the pad's read; the controls are read only while the game's window shows the
// game (not the native options)
void ReadTouchControls(BoundPad* pad);
// The controls drawn (overlayrender.cpp, after the frame's commands)
void AddTouchControls(std::vector<OverlayFrame::Command>* commands, int pixelWidth, int pixelHeight);
}

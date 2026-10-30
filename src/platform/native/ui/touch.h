#pragma once

// The touch controls (Android, or $TWIN_TOUCH=1 elsewhere): a floating left stick, the face buttons, the shoulders, start and
// select drawn over the picture (touch.cpp draws them through the overlay), and the right stick as a drag anywhere free on the
// other side of the screen from the stick. What they give is added to the pad's read (pads.cpp). Their size and opacity are
// options (Controls: OptionTouchSize, OptionTouchOpacity); where each sits and its own size are the layout editor's, kept in
// touch_layout.txt in the settings folder
#include "common.h"

#include <string>
#include <vector>

#include "ui/overlay.h"

namespace NativeUi
{
struct BoundPad;

// Whether the touch controls are in use: Android (and $TWIN_TOUCH=1 to try them elsewhere)
bool TouchControlsEnabled();
// How far down from the window's top the controls' top row reaches (the FPS counter sits below it), 0 without them
f32 TouchControlsTopInset();
// Watches the touch events (once, from the pad's read)
void StartTouchControls();
// The touch controls' buttons and sticks added to the pad's read; the controls are read only while the game's window shows the
// game (the native options take them through TouchMenuPad)
void ReadTouchControls(BoundPad* pad);
// The options navigated with the touch controls (settings.cpp, while they're open): whether they are (the controls on), and the
// buttons held (PadBit*) and the stick's push (-1 to 1 each way); the touches don't point at the options' items then
bool TouchPadDrivesOptions();
void TouchMenuPad(u32* buttons, f32* stickX, f32* stickY);
// The controls drawn (overlayrender.cpp, after the frame's commands)
void AddTouchControls(std::vector<OverlayFrame::Command>* commands, int pixelWidth, int pixelHeight);

// The layout editor (the settings' controls page, "edit layout"): the controls drawn over the paused game, a finger on one drags it,
// a second finger pinches it bigger or smaller, the panel's done and reset buttons. The texts are the game's (the settings give
// them in the game's language). Done (the button, or the settings' back) keeps the layout
void BeginTouchLayoutEdit(const std::string& hint, const std::string& done, const std::string& reset);
bool TouchLayoutEditing();
void EndTouchLayoutEdit();
// Every control back where it was at first, at the size the option gives (kept)
void ResetTouchLayout();
}

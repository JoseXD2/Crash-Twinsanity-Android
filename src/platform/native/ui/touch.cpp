// The touch controls: see touch.h. The screen in the overlay's reference space (1280 x 720 centred in the window; a wider
// screen shows more at the sides): the controls sit at the visible edges, each where the layout editor put it (its centre as a
// share of the visible screen) or where it sits at first, at the size option's scale times its own
#include "ui/touch.h"
#include "ui/bindings.h"
#include "ui/nativeui.h"
#include "ui/overlayinternal.h"
#include "native.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace NativeUi
{
namespace
{
// The controls at the size option's 100%, in reference pixels
constexpr f32 StickRadius = 95.0f;
constexpr f32 KnobRadius = 42.0f;
constexpr f32 FaceRadius = 46.0f;
constexpr f32 FaceSpread = 74.0f;
constexpr f32 ShoulderWidth = 110.0f;
constexpr f32 ShoulderHeight = 52.0f;
constexpr f32 MenuWidth = 118.0f;
constexpr f32 MenuHeight = 44.0f;
constexpr f32 Margin = 24.0f;
constexpr f32 CameraRadius = 110.0f;
// A control's own size from the editor's pinch, times the option's
constexpr f32 SmallestOwnScale = 0.5f;
constexpr f32 LargestOwnScale = 2.0f;
constexpr const char* LayoutFile = "touch_layout.txt";

enum class Control : u32
{
    Cross,
    Circle,
    Square,
    Triangle,
    L1,
    L2,
    R1,
    R2,
    Start,
    Select,
    Count,
};

// What the editor moves: the stick, the face buttons together, each other button
enum class Group : u32
{
    Stick,
    Face,
    L1,
    L2,
    R1,
    R2,
    Start,
    Select,
    Count,
};

constexpr const char* GroupNames[static_cast<u32>(Group::Count)] = {"stick", "face", "l1", "l2", "r1", "r2", "start", "select"};

struct Rect
{
    f32 x;
    f32 y;
    f32 width;
    f32 height;
    bool Holds(f32 px, f32 py, f32 slack = 0.0f) const
    {
        return px >= x - slack && px < x + width + slack && py >= y - slack && py < y + height + slack;
    }
};

// A group's place: moved by the editor (its centre as shares of the visible screen's width and height) or where it sits at first,
// and its own size
struct Placement
{
    bool moved = false;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 scale = 1.0f;
};

struct Layout
{
    // The visible edges of the window in reference pixels
    f32 left;
    f32 top;
    f32 right;
    f32 bottom;
    Rect controls[static_cast<u32>(Control::Count)];
    // The face buttons' centre and their sizes
    f32 faceX;
    f32 faceY;
    f32 faceRadius;
    // The left stick's resting centre and its sizes
    f32 stickX;
    f32 stickY;
    f32 stickRadius;
    f32 knobRadius;
    // Each group's centre and how far it reaches from it (the editor's grab), and its scale (the labels')
    f32 groupX[static_cast<u32>(Group::Count)];
    f32 groupY[static_cast<u32>(Group::Count)];
    f32 groupReach[static_cast<u32>(Group::Count)];
    f32 groupScale[static_cast<u32>(Group::Count)];
    // The editor's panel and its buttons
    Rect panel;
    Rect done;
    Rect reset;
};

std::mutex g_Lock;
Placement g_Placements[static_cast<u32>(Group::Count)];
bool g_PlacementsRead = false;

f32 OptionScale()
{
    return static_cast<f32>(std::clamp(GetOption(OptionTouchSize), 5, 15)) * 0.1f;
}

// The opacity option as a factor of the first look's alphas (6: theirs)
f32 OptionOpacity()
{
    return static_cast<f32>(std::clamp(GetOption(OptionTouchOpacity), 1, 10)) / 6.0f;
}

std::string LayoutPath()
{
    return Native::SettingsFolder() + "/" + LayoutFile;
}

// touch_layout.txt: a line a group, "name=moved,x,y,scale" (moved 0 or 1; x and y the centre's shares of the visible screen)
void ReadPlacements()
{
    if (g_PlacementsRead)
    {
        return;
    }

    g_PlacementsRead = true;
    std::ifstream file(LayoutPath());
    std::string line;
    while (std::getline(file, line))
    {
        size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }

        std::string name = line.substr(0, equals);
        int moved = 0;
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
        if (std::sscanf(line.c_str() + equals + 1, "%d,%f,%f,%f", &moved, &x, &y, &scale) != 4)
        {
            continue;
        }

        for (u32 g = 0; g < static_cast<u32>(Group::Count); g++)
        {
            if (name == GroupNames[g])
            {
                Placement& p = g_Placements[g];
                p.moved = moved != 0 && std::isfinite(x) && std::isfinite(y);
                p.x = std::clamp(x, 0.0f, 1.0f);
                p.y = std::clamp(y, 0.0f, 1.0f);
                p.scale = std::isfinite(scale) ? std::clamp(scale, SmallestOwnScale, LargestOwnScale) : 1.0f;
            }
        }
    }
}

void SavePlacements()
{
    std::ofstream file(LayoutPath(), std::ios::trunc);
    if (!file)
    {
        Native::Log("touch: the layout couldn't be saved to %s", LayoutFile);
        return;
    }

    for (u32 g = 0; g < static_cast<u32>(Group::Count); g++)
    {
        const Placement& p = g_Placements[g];
        char line[96];
        std::snprintf(line, sizeof(line), "%s=%d,%.4f,%.4f,%.3f\n", GroupNames[g], p.moved ? 1 : 0, static_cast<double>(p.x),
                      static_cast<double>(p.y), static_cast<double>(p.scale));
        file << line;
    }
}

Rect Around(f32 cx, f32 cy, f32 width, f32 height)
{
    return Rect{cx - width * 0.5f, cy - height * 0.5f, width, height};
}

Layout MakeLayout(int pixelWidth, int pixelHeight)
{
    ReadPlacements();
    f32 scale;
    f32 offsetX;
    f32 offsetY;
    ReferenceMapping(static_cast<f32>(pixelWidth), static_cast<f32>(pixelHeight), &scale, &offsetX, &offsetY);
    Layout l{};
    l.left = -offsetX / scale;
    l.top = -offsetY / scale;
    l.right = (static_cast<f32>(pixelWidth) - offsetX) / scale;
    l.bottom = (static_cast<f32>(pixelHeight) - offsetY) / scale;
    const f32 option = OptionScale();
    for (u32 g = 0; g < static_cast<u32>(Group::Count); g++)
    {
        l.groupScale[g] = option * g_Placements[g].scale;
    }

    auto s = [&](Group g) { return l.groupScale[static_cast<u32>(g)]; };
    const f32 middle = (l.left + l.right) * 0.5f;
    // Each group's size, and its centre where it sits at first (its distance from the edges grows with it)
    f32 width[static_cast<u32>(Group::Count)];
    f32 height[static_cast<u32>(Group::Count)];
    f32 x[static_cast<u32>(Group::Count)];
    f32 y[static_cast<u32>(Group::Count)];
    const f32 stick = StickRadius * s(Group::Stick);
    width[0] = height[0] = stick * 2.0f;
    x[0] = l.left + Margin + stick + 50.0f * s(Group::Stick);
    y[0] = l.bottom - Margin - stick - 30.0f * s(Group::Stick);
    const f32 faceReach = (FaceSpread + FaceRadius) * s(Group::Face);
    width[1] = height[1] = faceReach * 2.0f;
    x[1] = l.right - Margin - faceReach - 30.0f * s(Group::Face);
    y[1] = l.bottom - Margin - faceReach - 20.0f * s(Group::Face);
    for (Group g : {Group::L1, Group::L2, Group::R1, Group::R2})
    {
        width[static_cast<u32>(g)] = ShoulderWidth * s(g);
        height[static_cast<u32>(g)] = ShoulderHeight * s(g);
    }

    for (Group g : {Group::Start, Group::Select})
    {
        width[static_cast<u32>(g)] = MenuWidth * s(g);
        height[static_cast<u32>(g)] = MenuHeight * s(g);
    }

    auto w = [&](Group g) { return width[static_cast<u32>(g)]; };
    auto h = [&](Group g) { return height[static_cast<u32>(g)]; };
    x[static_cast<u32>(Group::L1)] = l.left + Margin + w(Group::L1) * 0.5f;
    x[static_cast<u32>(Group::L2)] = l.left + Margin + w(Group::L1) + 12.0f + w(Group::L2) * 0.5f;
    x[static_cast<u32>(Group::R1)] = l.right - Margin - w(Group::R1) * 0.5f;
    x[static_cast<u32>(Group::R2)] = l.right - Margin - w(Group::R1) - 12.0f - w(Group::R2) * 0.5f;
    x[static_cast<u32>(Group::Select)] = middle - 8.0f - w(Group::Select) * 0.5f;
    x[static_cast<u32>(Group::Start)] = middle + 8.0f + w(Group::Start) * 0.5f;
    for (Group g : {Group::L1, Group::L2, Group::R1, Group::R2, Group::Start, Group::Select})
    {
        y[static_cast<u32>(g)] = l.top + Margin + h(g) * 0.5f;
    }

    // Moved: the editor's place; either way kept on the screen whole
    const f32 visibleWidth = l.right - l.left;
    const f32 visibleHeight = l.bottom - l.top;
    for (u32 g = 0; g < static_cast<u32>(Group::Count); g++)
    {
        const Placement& p = g_Placements[g];
        if (p.moved)
        {
            x[g] = l.left + p.x * visibleWidth;
            y[g] = l.top + p.y * visibleHeight;
        }

        const f32 halfWidth = std::min(width[g] * 0.5f, visibleWidth * 0.5f);
        const f32 halfHeight = std::min(height[g] * 0.5f, visibleHeight * 0.5f);
        x[g] = std::clamp(x[g], l.left + halfWidth, l.right - halfWidth);
        y[g] = std::clamp(y[g], l.top + halfHeight, l.bottom - halfHeight);
        l.groupX[g] = x[g];
        l.groupY[g] = y[g];
        l.groupReach[g] = std::max(width[g], height[g]) * 0.5f;
    }

    l.stickX = x[0];
    l.stickY = y[0];
    l.stickRadius = stick;
    l.knobRadius = KnobRadius * s(Group::Stick);
    l.faceX = x[1];
    l.faceY = y[1];
    l.faceRadius = FaceRadius * s(Group::Face);
    const f32 spread = FaceSpread * s(Group::Face);
    auto circle = [](f32 cx, f32 cy, f32 r) { return Rect{cx - r, cy - r, r * 2.0f, r * 2.0f}; };
    l.controls[static_cast<u32>(Control::Cross)] = circle(l.faceX, l.faceY + spread, l.faceRadius);
    l.controls[static_cast<u32>(Control::Circle)] = circle(l.faceX + spread, l.faceY, l.faceRadius);
    l.controls[static_cast<u32>(Control::Square)] = circle(l.faceX - spread, l.faceY, l.faceRadius);
    l.controls[static_cast<u32>(Control::Triangle)] = circle(l.faceX, l.faceY - spread, l.faceRadius);
    auto place = [&](Control c, Group g) { l.controls[static_cast<u32>(c)] = Around(x[static_cast<u32>(g)], y[static_cast<u32>(g)], w(g), h(g)); };
    place(Control::L1, Group::L1);
    place(Control::L2, Group::L2);
    place(Control::R1, Group::R1);
    place(Control::R2, Group::R2);
    place(Control::Start, Group::Start);
    place(Control::Select, Group::Select);

    // The editor's panel in the middle of the screen (the controls sit at the edges), its buttons side by side
    const f32 panelWidth = std::min(600.0f, visibleWidth - 40.0f);
    l.panel = Around(middle, (l.top + l.bottom) * 0.5f, panelWidth, 150.0f);
    l.done = Around(middle + 90.0f, l.panel.y + 104.0f, 160.0f, 48.0f);
    l.reset = Around(middle - 90.0f, l.panel.y + 104.0f, 160.0f, 48.0f);
    return l;
}

constexpr u32 ControlBits[static_cast<u32>(Control::Count)] = {
    PadBitCross, PadBitCircle, PadBitSquare, PadBitTriangle, PadBitL1, PadBitL2, PadBitR1, PadBitR2, PadBitStart, PadBitSelect,
};

constexpr Group ControlGroups[static_cast<u32>(Control::Count)] = {
    Group::Face, Group::Face, Group::Face, Group::Face, Group::L1, Group::L2, Group::R1, Group::R2, Group::Start, Group::Select,
};

enum class Role : u32
{
    None,
    Stick,
    Camera,
    Button,
};

struct Finger
{
    SDL_FingerID id;
    Role role;
    // Where it is and where it began, in reference pixels
    f32 x;
    f32 y;
    f32 startX;
    f32 startY;
    // The button it's on (Role::Button; -1 when it slid off)
    s32 control;
};

std::vector<Finger> g_Fingers;
int g_PixelWidth = 0;
int g_PixelHeight = 0;
bool g_Watching = false;
// What the last read gave (drawn)
u32 g_Buttons = 0;
f32 g_StickX = 0.0f;
f32 g_StickY = 0.0f;
f32 g_StickCentreX = 0.0f;
f32 g_StickCentreY = 0.0f;
bool g_StickHeld = false;
f32 g_CameraX = 0.0f;
f32 g_CameraY = 0.0f;
f32 g_CameraCentreX = 0.0f;
f32 g_CameraCentreY = 0.0f;
bool g_CameraHeld = false;

// ------------------------------------------------------------------------------------------------------------ the editor's state
bool g_Editing = false;
std::string g_EditHint;
std::string g_EditDone;
std::string g_EditReset;
// The finger dragging a group (its grab's offset from the group's centre), and a second one pinching it
struct Drag
{
    bool active = false;
    SDL_FingerID finger = 0;
    s32 group = -1;
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 grabX = 0.0f;
    f32 grabY = 0.0f;
    bool pinching = false;
    SDL_FingerID second = 0;
    f32 secondX = 0.0f;
    f32 secondY = 0.0f;
    f32 startDistance = 1.0f;
    f32 startScale = 1.0f;
};
Drag g_Drag;
// A finger down on the panel's buttons: which (0 done, 1 reset; -1 none), taken when it's lifted there
SDL_FingerID g_TapFinger = 0;
s32 g_Tap = -1;

void ToReference(f32 nx, f32 ny, f32* x, f32* y)
{
    f32 scale;
    f32 offsetX;
    f32 offsetY;
    ReferenceMapping(static_cast<f32>(g_PixelWidth), static_cast<f32>(g_PixelHeight), &scale, &offsetX, &offsetY);
    *x = (nx * static_cast<f32>(g_PixelWidth) - offsetX) / scale;
    *y = (ny * static_cast<f32>(g_PixelHeight) - offsetY) / scale;
}

s32 ControlAt(const Layout& l, f32 x, f32 y, f32 slack)
{
    for (u32 i = 0; i < static_cast<u32>(Control::Count); i++)
    {
        if (l.controls[i].Holds(x, y, slack))
        {
            return static_cast<s32>(i);
        }
    }

    return -1;
}

// The group a point is on for the editor: a button's own (the face buttons' as one), the face buttons' circle, the stick's
s32 GroupAt(const Layout& l, f32 x, f32 y)
{
    s32 control = ControlAt(l, x, y, 10.0f);
    if (control >= 0)
    {
        return static_cast<s32>(ControlGroups[control]);
    }

    for (Group g : {Group::Face, Group::Stick})
    {
        const u32 i = static_cast<u32>(g);
        if (std::hypot(x - l.groupX[i], y - l.groupY[i]) <= l.groupReach[i] + 10.0f)
        {
            return static_cast<s32>(i);
        }
    }

    return -1;
}

// A group's centre moved to a point (as shares of the visible screen)
void MoveGroup(const Layout& l, s32 group, f32 cx, f32 cy)
{
    Placement& p = g_Placements[group];
    p.moved = true;
    p.x = std::clamp((cx - l.left) / std::max(1.0f, l.right - l.left), 0.0f, 1.0f);
    p.y = std::clamp((cy - l.top) / std::max(1.0f, l.bottom - l.top), 0.0f, 1.0f);
}

// The editor's part of the finger events (the lock held)
void EditTouch(const SDL_Event* event, SDL_FingerID id, f32 x, f32 y)
{
    Layout l = MakeLayout(g_PixelWidth, g_PixelHeight);
    if (event->type == SDL_EVENT_FINGER_DOWN)
    {
        if (!g_Drag.active)
        {
            if (l.done.Holds(x, y, 6.0f) || l.reset.Holds(x, y, 6.0f))
            {
                g_TapFinger = id;
                g_Tap = l.done.Holds(x, y, 6.0f) ? 0 : 1;
                return;
            }

            s32 group = GroupAt(l, x, y);
            if (group >= 0)
            {
                g_Drag = Drag{};
                g_Drag.active = true;
                g_Drag.finger = id;
                g_Drag.group = group;
                g_Drag.x = x;
                g_Drag.y = y;
                g_Drag.grabX = x - l.groupX[group];
                g_Drag.grabY = y - l.groupY[group];
            }
        }
        else if (!g_Drag.pinching && id != g_Drag.finger)
        {
            // A second finger: the dragged group pinched from here
            g_Drag.pinching = true;
            g_Drag.second = id;
            g_Drag.secondX = x;
            g_Drag.secondY = y;
            g_Drag.startDistance = std::max(20.0f, std::hypot(x - g_Drag.x, y - g_Drag.y));
            g_Drag.startScale = g_Placements[g_Drag.group].scale;
        }

        return;
    }

    if (event->type == SDL_EVENT_FINGER_MOTION)
    {
        if (!g_Drag.active || (id != g_Drag.finger && !(g_Drag.pinching && id == g_Drag.second)))
        {
            return;
        }

        if (id == g_Drag.finger)
        {
            g_Drag.x = x;
            g_Drag.y = y;
        }
        else
        {
            g_Drag.secondX = x;
            g_Drag.secondY = y;
        }

        if (g_Drag.pinching)
        {
            const f32 distance = std::hypot(g_Drag.secondX - g_Drag.x, g_Drag.secondY - g_Drag.y);
            g_Placements[g_Drag.group].scale =
                std::clamp(g_Drag.startScale * distance / g_Drag.startDistance, SmallestOwnScale, LargestOwnScale);
        }
        else
        {
            MoveGroup(l, g_Drag.group, x - g_Drag.grabX, y - g_Drag.grabY);
        }

        return;
    }

    // Lifted (or cancelled)
    if (g_Tap >= 0 && id == g_TapFinger)
    {
        const s32 tap = g_Tap;
        g_Tap = -1;
        const bool there = tap == 0 ? l.done.Holds(x, y, 12.0f) : l.reset.Holds(x, y, 12.0f);
        if (there && event->type == SDL_EVENT_FINGER_UP)
        {
            if (tap == 0)
            {
                SavePlacements();
                g_Editing = false;
                g_Drag = Drag{};
            }
            else
            {
                for (Placement& p : g_Placements)
                {
                    p = Placement{};
                }

                SavePlacements();
            }
        }

        return;
    }

    if (g_Drag.active && g_Drag.pinching && id == g_Drag.second)
    {
        // The pinch over: the drag goes on from where the first finger is now (its grab taken again: no jump)
        g_Drag.pinching = false;
        g_Drag.grabX = g_Drag.x - l.groupX[g_Drag.group];
        g_Drag.grabY = g_Drag.y - l.groupY[g_Drag.group];
    }
    else if (g_Drag.active && id == g_Drag.finger)
    {
        g_Drag = Drag{};
    }
}

bool WatchTouchEvents(void*, SDL_Event* event)
{
    if (event->type != SDL_EVENT_FINGER_DOWN && event->type != SDL_EVENT_FINGER_MOTION && event->type != SDL_EVENT_FINGER_UP &&
        event->type != SDL_EVENT_FINGER_CANCELED)
    {
        return true;
    }

    std::lock_guard lock(g_Lock);
    if (g_PixelWidth <= 0 || g_PixelHeight <= 0)
    {
        return true;
    }

    const SDL_TouchFingerEvent& finger = event->tfinger;
    f32 x;
    f32 y;
    ToReference(finger.x, finger.y, &x, &y);
    if (g_Editing)
    {
        g_Fingers.clear();
        EditTouch(event, finger.fingerID, x, y);
        return true;
    }

    auto found = std::find_if(g_Fingers.begin(), g_Fingers.end(), [&](const Finger& f) { return f.id == finger.fingerID; });
    if (event->type == SDL_EVENT_FINGER_DOWN)
    {
        if (found != g_Fingers.end())
        {
            g_Fingers.erase(found);
        }

        Layout l = MakeLayout(g_PixelWidth, g_PixelHeight);
        Finger f{finger.fingerID, Role::None, x, y, x, y, -1};
        s32 control = ControlAt(l, x, y, 8.0f);
        const f32 middle = (l.left + l.right) * 0.5f;
        if (control >= 0)
        {
            f.role = Role::Button;
            f.control = control;
        }
        else if (std::hypot(x - l.stickX, y - l.stickY) <= l.stickRadius * 1.3f)
        {
            // The stick: a finger landing on it (or just around it) takes it, from where it landed
            f.role = Role::Stick;
            bool other = std::any_of(g_Fingers.begin(), g_Fingers.end(), [](const Finger& g) { return g.role == Role::Stick; });
            if (other)
            {
                f.role = Role::None;
            }
        }
        else if ((x >= middle) != (l.stickX >= middle) && ControlAt(l, x, y, 24.0f) < 0)
        {
            // The camera: a drag anywhere free on the other side of the screen from the stick
            f.role = Role::Camera;
            bool other = std::any_of(g_Fingers.begin(), g_Fingers.end(), [](const Finger& g) { return g.role == Role::Camera; });
            if (other)
            {
                f.role = Role::None;
            }
        }

        g_Fingers.push_back(f);
    }
    else if (event->type == SDL_EVENT_FINGER_MOTION)
    {
        if (found != g_Fingers.end())
        {
            found->x = x;
            found->y = y;
            if (found->role == Role::Button)
            {
                // Sliding between the face buttons (a slide elsewhere lets go)
                Layout l = MakeLayout(g_PixelWidth, g_PixelHeight);
                s32 control = ControlAt(l, x, y, 14.0f);
                if (control != found->control && (control < 0 || control <= static_cast<s32>(Control::Triangle)) &&
                    found->control <= static_cast<s32>(Control::Triangle))
                {
                    found->control = control;
                }
            }
        }
    }
    else if (found != g_Fingers.end())
    {
        g_Fingers.erase(found);
    }

    return true;
}

// A finger's deflection from a centre as a stick's byte (0x80 the middle), the deflection full at the radius
void StickBytes(f32 dx, f32 dy, f32 radius, u8* x, u8* y, f32* fx, f32* fy)
{
    f32 length = std::hypot(dx, dy);
    f32 scale = length > radius ? radius / length : 1.0f;
    *fx = std::clamp(dx * scale / radius, -1.0f, 1.0f);
    *fy = std::clamp(dy * scale / radius, -1.0f, 1.0f);
    // The stick's own dead zone is the game's (bindings.cpp's Squared moves a circle out to the DualShock's square)
    *x = static_cast<u8>(std::clamp(128.0f + *fx * 127.0f, 0.0f, 255.0f));
    *y = static_cast<u8>(std::clamp(128.0f + *fy * 127.0f, 0.0f, 255.0f));
}

void Circle(std::vector<OverlayFrame::Command>* commands, f32 cx, f32 cy, f32 r, Rgba32 fill, Rgba32 border, f32 borderWidth)
{
    OverlayFrame::Command box{};
    box.kind = CommandKind::Box;
    box.x = cx - r;
    box.y = cy - r;
    box.width = r * 2.0f;
    box.height = r * 2.0f;
    box.box.fill = fill;
    box.box.radius = r;
    box.box.border = borderWidth;
    box.box.borderColour = border;
    commands->push_back(box);
}

void Box(std::vector<OverlayFrame::Command>* commands, const Rect& r, f32 radius, Rgba32 fill, Rgba32 border, f32 borderWidth)
{
    OverlayFrame::Command box{};
    box.kind = CommandKind::Box;
    box.x = r.x;
    box.y = r.y;
    box.width = r.width;
    box.height = r.height;
    box.box.fill = fill;
    box.box.radius = radius;
    box.box.border = borderWidth;
    box.box.borderColour = border;
    commands->push_back(box);
}

void Label(std::vector<OverlayFrame::Command>* commands, const std::string& text, f32 cx, f32 cy, f32 size, Rgba32 colour,
           OverlayFont font = OverlayFont::BodyBold, f32 outline = 2.0f)
{
    TextStyle style;
    style.font = font;
    style.size = size;
    style.colour = colour;
    style.outline = outline;
    style.outlineColour = Hex(0x000000, 0.7f);
    f32 width = OverlayTextWidth(text, style);
    OverlayFrame::Command line{};
    line.kind = CommandKind::Text;
    line.x = cx - width * 0.5f;
    line.y = cy;
    line.width = width;
    line.text = GameTextCodepoints(text);
    line.textStyle = style;
    commands->push_back(line);
}

// The game's lower case text with its first letter made a capital (Windows-1252: Latin-1's letters)
std::string FirstCapital(std::string text)
{
    for (char& c : text)
    {
        unsigned char u = static_cast<unsigned char>(c);
        if (u == ' ')
        {
            continue;
        }

        if ((u >= 'a' && u <= 'z') || (u >= 0xE0 && u <= 0xFE && u != 0xF7))
        {
            c = static_cast<char>(u - 0x20);
        }

        break;
    }

    return text;
}

std::string Capitals(std::string text)
{
    for (char& c : text)
    {
        unsigned char u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 0xE0 && u <= 0xFE && u != 0xF7))
        {
            c = static_cast<char>(u - 0x20);
        }
    }

    return text;
}

f32 Faded(f32 alpha, f32 factor)
{
    return std::clamp(alpha * factor, 0.0f, 1.0f);
}
}

f32 TouchControlsTopInset()
{
    return TouchControlsEnabled() && IsOn(OptionTouchControls) ? Margin + ShoulderHeight * OptionScale() + 8.0f : 0.0f;
}

bool TouchControlsEnabled()
{
    static const bool enabled = []
    {
        const char* wanted = std::getenv("TWIN_TOUCH");
        if (wanted != nullptr)
        {
            return wanted[0] != '0';
        }
#if defined(__ANDROID__)
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}

void StartTouchControls()
{
    if (!g_Watching && TouchControlsEnabled())
    {
        g_Watching = true;
        SDL_AddEventWatch(WatchTouchEvents, nullptr);
    }
}

void BeginTouchLayoutEdit(const std::string& hint, const std::string& done, const std::string& reset)
{
    StartTouchControls();
    std::lock_guard lock(g_Lock);
    g_EditHint = hint;
    g_EditDone = done;
    g_EditReset = reset;
    g_Fingers.clear();
    g_Drag = Drag{};
    g_Tap = -1;
    g_Editing = true;
}

bool TouchLayoutEditing()
{
    std::lock_guard lock(g_Lock);
    return g_Editing;
}

void EndTouchLayoutEdit()
{
    std::lock_guard lock(g_Lock);
    if (g_Editing)
    {
        ReadPlacements();
        SavePlacements();
    }

    g_Editing = false;
    g_Drag = Drag{};
    g_Tap = -1;
}

void ResetTouchLayout()
{
    std::lock_guard lock(g_Lock);
    g_PlacementsRead = true;
    for (Placement& p : g_Placements)
    {
        p = Placement{};
    }

    SavePlacements();
}

void ReadTouchControls(BoundPad* pad)
{
    if (!TouchControlsEnabled())
    {
        return;
    }

    std::lock_guard lock(g_Lock);
    // The window's size in pixels, for the reference mapping (the overlay notes it as it draws; before the first draw, SDL's)
    if (g_PixelWidth <= 0)
    {
        int count = 0;
        SDL_Window** windows = SDL_GetWindows(&count);
        if (windows != nullptr && count > 0)
        {
            SDL_GetWindowSizeInPixels(windows[0], &g_PixelWidth, &g_PixelHeight);
        }

        SDL_free(windows);
    }

    // The native options read the controls themselves (TouchMenuPad: their state is the options' then); the game's own menus
    // answer the buttons as in play. Turned off, or being laid out, nothing is read
    if (SettingsOpen())
    {
        return;
    }

    g_Buttons = 0;
    g_StickHeld = false;
    g_CameraHeld = false;
    if (g_Editing || g_PixelWidth <= 0 || !IsOn(OptionTouchControls))
    {
        return;
    }

    const f32 stickRadius = StickRadius * OptionScale() * g_Placements[static_cast<u32>(Group::Stick)].scale;
    for (const Finger& f : g_Fingers)
    {
        switch (f.role)
        {
        case Role::Button:
            if (f.control >= 0)
            {
                g_Buttons |= ControlBits[f.control];
            }
            break;
        case Role::Stick:
        {
            u8 x;
            u8 y;
            StickBytes(f.x - f.startX, f.y - f.startY, stickRadius, &x, &y, &g_StickX, &g_StickY);
            g_StickCentreX = f.startX;
            g_StickCentreY = f.startY;
            g_StickHeld = true;
            if (x != BoundPad::StickMiddle || y != BoundPad::StickMiddle)
            {
                Squared(&x, &y);
                pad->leftX = x;
                pad->leftY = y;
            }
            break;
        }
        case Role::Camera:
        {
            u8 x;
            u8 y;
            StickBytes(f.x - f.startX, f.y - f.startY, CameraRadius, &x, &y, &g_CameraX, &g_CameraY);
            g_CameraCentreX = f.startX;
            g_CameraCentreY = f.startY;
            g_CameraHeld = true;
            if (x != BoundPad::StickMiddle || y != BoundPad::StickMiddle)
            {
                pad->rightX = x;
                pad->rightY = y;
            }
            break;
        }
        default:
            break;
        }
    }

    pad->buttons |= g_Buttons;
    if ((g_Buttons & PadBitL2) != 0)
    {
        pad->l2Pressure = 0xFF;
    }

    if ((g_Buttons & PadBitR2) != 0)
    {
        pad->r2Pressure = 0xFF;
    }
}

bool TouchPadDrivesOptions()
{
    return TouchControlsEnabled() && IsOn(OptionTouchControls);
}

void TouchMenuPad(u32* buttons, f32* stickX, f32* stickY)
{
    std::lock_guard lock(g_Lock);
    g_Buttons = 0;
    g_StickHeld = false;
    g_CameraHeld = false;
    g_StickX = 0.0f;
    g_StickY = 0.0f;
    if (!g_Editing && g_PixelWidth > 0 && IsOn(OptionTouchControls))
    {
        const f32 stickRadius = StickRadius * OptionScale() * g_Placements[static_cast<u32>(Group::Stick)].scale;
        for (const Finger& f : g_Fingers)
        {
            if (f.role == Role::Button && f.control >= 0)
            {
                g_Buttons |= ControlBits[f.control];
            }
            else if (f.role == Role::Stick)
            {
                u8 x;
                u8 y;
                StickBytes(f.x - f.startX, f.y - f.startY, stickRadius, &x, &y, &g_StickX, &g_StickY);
                g_StickCentreX = f.startX;
                g_StickCentreY = f.startY;
                g_StickHeld = true;
            }
        }
    }

    *buttons = g_Buttons;
    *stickX = g_StickX;
    *stickY = g_StickY;
}

void AddTouchControls(std::vector<OverlayFrame::Command>* commands, int pixelWidth, int pixelHeight)
{
    if (!TouchControlsEnabled() || pixelWidth <= 0 || pixelHeight <= 0)
    {
        return;
    }

    std::lock_guard lock(g_Lock);
    g_PixelWidth = pixelWidth;
    g_PixelHeight = pixelHeight;
    if (!g_Editing)
    {
        if (!IsOn(OptionTouchControls))
        {
            return;
        }

        // Hidden while a gamepad is in use and nothing is touched
        bool touched = !g_Fingers.empty();
        if (SDL_HasGamepad() && !touched)
        {
            return;
        }
    }

    Layout l = MakeLayout(pixelWidth, pixelHeight);
    OverlayFrame::Command noClip{};
    noClip.kind = CommandKind::NoClip;
    commands->push_back(noClip);
    if (g_Editing)
    {
        // The paused game behind, dimmed so the controls stand out
        Box(commands, Rect{l.left, l.top, l.right - l.left, l.bottom - l.top}, 0.0f, Hex(0x000000, 0.35f), Hex(0, 0), 0.0f);
    }

    const f32 opacity = g_Editing ? std::max(1.0f, OptionOpacity()) : OptionOpacity();
    const Rgba32 idle = Hex(0x000000, Faded(0.28f, opacity));
    const Rgba32 pressed = Hex(0xffffff, 0.45f);
    const Rgba32 ring = Hex(0xffffff, Faded(0.55f, opacity));
    const Rgba32 grabbed = Hex(0xff9a1f);
    auto ringFor = [&](Group g) { return g_Editing && g_Drag.active && g_Drag.group == static_cast<s32>(g) ? grabbed : ring; };
    auto ringWidth = [&](Group g) { return g_Editing && g_Drag.active && g_Drag.group == static_cast<s32>(g) ? 5.0f : 3.0f; };

    // The left stick: its base where it rests, the knob where the finger holds it
    f32 sx = l.stickX;
    f32 sy = l.stickY;
    Circle(commands, sx, sy, l.stickRadius, idle, ringFor(Group::Stick), ringWidth(Group::Stick));
    Circle(commands, sx + g_StickX * l.stickRadius * (g_StickHeld ? 1.0f : 0.0f), sy + g_StickY * l.stickRadius * (g_StickHeld ? 1.0f : 0.0f),
           l.knobRadius, g_StickHeld ? pressed : Hex(0xffffff, Faded(0.3f, opacity)), ring, 2.0f);
    // (the camera's drag draws nothing: a drag anywhere free on the other side turns it)

    // The face buttons: the PlayStation's art on a disc (their group's outline while it's dragged)
    if (g_Editing && g_Drag.active && g_Drag.group == static_cast<s32>(Group::Face))
    {
        const u32 face = static_cast<u32>(Group::Face);
        Circle(commands, l.groupX[face], l.groupY[face], l.groupReach[face], Hex(0xff9a1f, 0.12f), grabbed, 3.0f);
    }

    const OverlayImage art[4] = {OverlayImage::PlayStationCross, OverlayImage::PlayStationCircle, OverlayImage::PlayStationSquare,
                                 OverlayImage::PlayStationTriangle};
    const f32 inset = 8.0f * l.groupScale[static_cast<u32>(Group::Face)];
    for (u32 i = 0; i < 4; i++)
    {
        const Rect& r = l.controls[i];
        bool down = (g_Buttons & ControlBits[i]) != 0;
        Circle(commands, r.x + r.width * 0.5f, r.y + r.height * 0.5f, l.faceRadius, down ? pressed : idle, ringFor(Group::Face), 3.0f);
        OverlayFrame::Command image{};
        image.kind = CommandKind::Image;
        image.image = art[i];
        image.x = r.x + inset;
        image.y = r.y + inset;
        image.width = r.width - inset * 2.0f;
        image.height = r.height - inset * 2.0f;
        image.alpha = down ? 1.0f : Faded(0.85f, opacity);
        commands->push_back(image);
    }

    // The shoulders, start and select: rounded boxes with their names
    const char* names[static_cast<u32>(Control::Count)] = {"", "", "", "", "L1", "L2", "R1", "R2", "START", "SELECT"};
    for (u32 i = 4; i < static_cast<u32>(Control::Count); i++)
    {
        const Rect& r = l.controls[i];
        const Group group = ControlGroups[i];
        const f32 s = l.groupScale[static_cast<u32>(group)];
        bool down = (g_Buttons & ControlBits[i]) != 0;
        Box(commands, r, 14.0f * s, down ? pressed : idle, ringFor(group), std::max(2.5f, ringWidth(group) - 0.5f));
        Label(commands, names[i], r.x + r.width * 0.5f, r.y + r.height * 0.5f, (i >= static_cast<u32>(Control::Start) ? 18.0f : 22.0f) * s,
              Hex(0xffffff, Faded(0.9f, std::min(1.0f, opacity))));
    }

    if (g_Editing)
    {
        // The editor's panel: what to do, and its buttons (done lit)
        Box(commands, l.panel, 22.0f, Hex(0xffffff), Hex(0xffb43a), 3.0f);
        Label(commands, FirstCapital(g_EditHint), l.panel.x + l.panel.width * 0.5f, l.panel.y + 42.0f, 20.0f, Hex(0x173a6b),
              OverlayFont::Body, 0.0f);
        Box(commands, l.reset, 20.0f, Hex(0xeaf2ff), Hex(0, 0), 0.0f);
        Label(commands, FirstCapital(g_EditReset), l.reset.x + l.reset.width * 0.5f, l.reset.y + l.reset.height * 0.5f, 21.0f,
              Hex(0x1b6fd0), OverlayFont::Body, 0.0f);
        Box(commands, l.done, 20.0f, Hex(0xff9a1f), Hex(0, 0), 0.0f);
        Label(commands, Capitals(g_EditDone), l.done.x + l.done.width * 0.5f, l.done.y + l.done.height * 0.5f, 21.0f, Hex(0xffffff),
              OverlayFont::BodyBold, 0.0f);
    }
}
}

// The touch controls: see touch.h. The screen in the overlay's reference space (1280 x 720 centred in the window; a wider
// screen shows more at the sides): the controls sit at the visible edges
#include "ui/touch.h"
#include "ui/bindings.h"
#include "ui/nativeui.h"
#include "ui/overlayinternal.h"
#include "native.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

namespace NativeUi
{
namespace
{
// The controls, in reference pixels
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

struct Layout
{
    // The visible edges of the window in reference pixels
    f32 left;
    f32 top;
    f32 right;
    f32 bottom;
    Rect controls[static_cast<u32>(Control::Count)];
    // The face buttons' centres
    f32 faceX;
    f32 faceY;
    // The left stick's resting centre
    f32 stickX;
    f32 stickY;
};

Layout MakeLayout(int pixelWidth, int pixelHeight)
{
    f32 scale;
    f32 offsetX;
    f32 offsetY;
    ReferenceMapping(static_cast<f32>(pixelWidth), static_cast<f32>(pixelHeight), &scale, &offsetX, &offsetY);
    Layout l{};
    l.left = -offsetX / scale;
    l.top = -offsetY / scale;
    l.right = (static_cast<f32>(pixelWidth) - offsetX) / scale;
    l.bottom = (static_cast<f32>(pixelHeight) - offsetY) / scale;
    l.faceX = l.right - Margin - FaceRadius - FaceSpread - 30.0f;
    l.faceY = l.bottom - Margin - FaceRadius - FaceSpread - 20.0f;
    l.stickX = l.left + Margin + StickRadius + 50.0f;
    l.stickY = l.bottom - Margin - StickRadius - 30.0f;
    auto circle = [](f32 cx, f32 cy, f32 r) { return Rect{cx - r, cy - r, r * 2.0f, r * 2.0f}; };
    l.controls[static_cast<u32>(Control::Cross)] = circle(l.faceX, l.faceY + FaceSpread, FaceRadius);
    l.controls[static_cast<u32>(Control::Circle)] = circle(l.faceX + FaceSpread, l.faceY, FaceRadius);
    l.controls[static_cast<u32>(Control::Square)] = circle(l.faceX - FaceSpread, l.faceY, FaceRadius);
    l.controls[static_cast<u32>(Control::Triangle)] = circle(l.faceX, l.faceY - FaceSpread, FaceRadius);
    l.controls[static_cast<u32>(Control::L1)] = {l.left + Margin, l.top + Margin, ShoulderWidth, ShoulderHeight};
    l.controls[static_cast<u32>(Control::L2)] = {l.left + Margin + ShoulderWidth + 12.0f, l.top + Margin, ShoulderWidth, ShoulderHeight};
    l.controls[static_cast<u32>(Control::R1)] = {l.right - Margin - ShoulderWidth, l.top + Margin, ShoulderWidth, ShoulderHeight};
    l.controls[static_cast<u32>(Control::R2)] = {l.right - Margin - ShoulderWidth * 2.0f - 12.0f, l.top + Margin, ShoulderWidth, ShoulderHeight};
    f32 middle = (l.left + l.right) * 0.5f;
    l.controls[static_cast<u32>(Control::Select)] = {middle - MenuWidth - 8.0f, l.top + Margin, MenuWidth, MenuHeight};
    l.controls[static_cast<u32>(Control::Start)] = {middle + 8.0f, l.top + Margin, MenuWidth, MenuHeight};
    return l;
}

constexpr u32 ControlBits[static_cast<u32>(Control::Count)] = {
    PadBitCross, PadBitCircle, PadBitSquare, PadBitTriangle, PadBitL1, PadBitL2, PadBitR1, PadBitR2, PadBitStart, PadBitSelect,
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

std::mutex g_Lock;
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
        if (control >= 0)
        {
            f.role = Role::Button;
            f.control = control;
        }
        else if (x < (l.left + l.right) * 0.5f)
        {
            // The stick: a finger landing on it (or just around it) takes it, from where it landed
            bool nearRest = std::hypot(x - l.stickX, y - l.stickY) <= StickRadius * 1.3f;
            if (nearRest)
            {
                f.role = Role::Stick;
                bool other = std::any_of(g_Fingers.begin(), g_Fingers.end(), [](const Finger& g) { return g.role == Role::Stick; });
                if (other)
                {
                    f.role = Role::None;
                }
            }
        }
        else if (y > l.top + Margin + ShoulderHeight + 20.0f)
        {
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

void Label(std::vector<OverlayFrame::Command>* commands, const std::string& text, f32 cx, f32 cy, f32 size, Rgba32 colour)
{
    TextStyle style;
    style.font = OverlayFont::BodyBold;
    style.size = size;
    style.colour = colour;
    style.outline = 2.0f;
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
}

f32 TouchControlsTopInset()
{
    return TouchControlsEnabled() ? Margin + ShoulderHeight + 8.0f : 0.0f;
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

    g_Buttons = 0;
    g_StickHeld = false;
    g_CameraHeld = false;
    // The native options take the touches as the mouse's (SDL's); the game's own menus answer the buttons as in play
    if (SettingsOpen() || g_PixelWidth <= 0)
    {
        return;
    }

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
            StickBytes(f.x - f.startX, f.y - f.startY, StickRadius, &x, &y, &g_StickX, &g_StickY);
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

void AddTouchControls(std::vector<OverlayFrame::Command>* commands, int pixelWidth, int pixelHeight)
{
    if (!TouchControlsEnabled() || pixelWidth <= 0 || pixelHeight <= 0)
    {
        return;
    }

    std::lock_guard lock(g_Lock);
    g_PixelWidth = pixelWidth;
    g_PixelHeight = pixelHeight;
    if (SettingsOpen())
    {
        return;
    }

    // Hidden while a gamepad is in use and nothing is touched
    bool touched = !g_Fingers.empty();
    if (SDL_HasGamepad() && !touched)
    {
        return;
    }

    Layout l = MakeLayout(pixelWidth, pixelHeight);
    OverlayFrame::Command noClip{};
    noClip.kind = CommandKind::NoClip;
    commands->push_back(noClip);

    const Rgba32 idle = Hex(0x000000, 0.28f);
    const Rgba32 pressed = Hex(0xffffff, 0.45f);
    const Rgba32 ring = Hex(0xffffff, 0.55f);
    // The left stick: its base where it rests, or where the finger holds it
    f32 sx = l.stickX;
    f32 sy = l.stickY;
    Circle(commands, sx, sy, StickRadius, idle, ring, 3.0f);
    Circle(commands, sx + g_StickX * StickRadius * (g_StickHeld ? 1.0f : 0.0f), sy + g_StickY * StickRadius * (g_StickHeld ? 1.0f : 0.0f),
           KnobRadius, g_StickHeld ? pressed : Hex(0xffffff, 0.3f), ring, 2.0f);
    // (the camera's drag draws nothing: a drag anywhere free in the right part turns it)

    // The face buttons: the PlayStation's art on a disc
    const OverlayImage art[4] = {OverlayImage::PlayStationCross, OverlayImage::PlayStationCircle, OverlayImage::PlayStationSquare,
                                 OverlayImage::PlayStationTriangle};
    for (u32 i = 0; i < 4; i++)
    {
        const Rect& r = l.controls[i];
        bool down = (g_Buttons & ControlBits[i]) != 0;
        Circle(commands, r.x + r.width * 0.5f, r.y + r.height * 0.5f, FaceRadius, down ? pressed : idle, ring, 3.0f);
        OverlayFrame::Command image{};
        image.kind = CommandKind::Image;
        image.image = art[i];
        image.x = r.x + 8.0f;
        image.y = r.y + 8.0f;
        image.width = r.width - 16.0f;
        image.height = r.height - 16.0f;
        image.alpha = down ? 1.0f : 0.85f;
        commands->push_back(image);
    }

    // The shoulders, start and select: rounded boxes with their names
    const char* names[static_cast<u32>(Control::Count)] = {"", "", "", "", "L1", "L2", "R1", "R2", "START", "SELECT"};
    for (u32 i = 4; i < static_cast<u32>(Control::Count); i++)
    {
        const Rect& r = l.controls[i];
        bool down = (g_Buttons & ControlBits[i]) != 0;
        OverlayFrame::Command box{};
        box.kind = CommandKind::Box;
        box.x = r.x;
        box.y = r.y;
        box.width = r.width;
        box.height = r.height;
        box.box.fill = down ? pressed : idle;
        box.box.radius = 14.0f;
        box.box.border = 2.5f;
        box.box.borderColour = ring;
        commands->push_back(box);
        Label(commands, names[i], r.x + r.width * 0.5f, r.y + r.height * 0.5f, i >= static_cast<u32>(Control::Start) ? 18.0f : 22.0f,
              Hex(0xffffff, 0.9f));
    }
}
}

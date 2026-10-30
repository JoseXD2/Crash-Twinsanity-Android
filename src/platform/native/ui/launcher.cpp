// The launcher: see launcher.h. Drawn in the overlay's 1280 x 720 reference space as the options are (settingsview.cpp's design:
// the blue backdrop with the Twinsanity spiral, the title in Luckiest Guy, the white panel with its orange lip, Fredoka), its
// texts the native table's in the options' language (the game's texts aren't there yet: the game hasn't started). It takes the
// touches (SDL's mouse events made from them), the mouse, the keyboard (arrows, Enter, Escape) and the pads (the D-pad, south to
// pick, east or back to quit)
#include "ui/launcher.h"
#include "graphics/glcompat.h"
#include "ui/overlayinternal.h"
#include "ui/pctext.h"
#include "ui/nativeui.h"

#include "native.h"
#include "platform/system.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace NativeUi
{
namespace
{
// The options' design values (settingsview.cpp)
constexpr Rgba32 Orange = Hex(0xff9a1f);
constexpr Rgba32 Lip = Hex(0xffb43a);
constexpr Rgba32 Brown = Hex(0x3a1200);
constexpr Rgba32 Ink = Hex(0x173a6b);
constexpr Rgba32 PillBlue = Hex(0x1b6fd0);
constexpr Rgba32 PillFill = Hex(0xeaf2ff);
constexpr Rgba32 White = Hex(0xffffff);
constexpr Rgba32 Note = Hex(0x6b86a8);
constexpr Rgba32 Good = Hex(0x2e9e4f);
constexpr Rgba32 Bad = Hex(0xd0342c);

constexpr f32 PanelX = 240.0f;
constexpr f32 PanelY = 186.0f;
constexpr f32 PanelWidth = 800.0f;
constexpr f32 PanelHeight = 366.0f;
constexpr f32 Pad = 40.0f;

enum Button : s32
{
    ButtonChoose,
    ButtonPlay,
    ButtonQuit,
    Buttons,
};

struct Region
{
    f32 x;
    f32 y;
    f32 width;
    f32 height;
    bool Holds(f32 px, f32 py) const
    {
        return px >= x && px <= x + width && py >= y && py <= y + height;
    }
};

// The native texts' language: the options' (or the computer's), as the game will take it
u32 g_Language = 0;

u32 LanguageIndex()
{
    switch (Platform::System::Language())
    {
    case Platform::System::LanguageFrench:
        return 1;
    case Platform::System::LanguageGerman:
        return 2;
    case Platform::System::LanguageSpanish:
        return 3;
    case Platform::System::LanguageItalian:
        return 4;
    default:
        return 0;
    }
}

// A native text in the language, as the game's Windows-1252 (the overlay's text)
std::string Text(u32 id)
{
    for (u32 i = 0; i < g_NativeTextCount; i++)
    {
        if (g_NativeTexts[i].text == id)
        {
            const char* utf8 = g_NativeTexts[i].languages[g_Language];
            std::string out;
            ToGameText(utf8 != nullptr ? utf8 : g_NativeTexts[i].languages[0], &out);
            return out;
        }
    }

    return {};
}

std::string FromUtf8(const std::string& utf8)
{
    std::string out;
    ToGameText(utf8.c_str(), &out);
    return out;
}

unsigned char Upper(unsigned char c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) ? static_cast<unsigned char>(c - 0x20) : c;
}

std::string Capitals(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(Upper(static_cast<unsigned char>(c)));
    }

    return text;
}

// The first letter a capital, and the PAL disc's names as they're written
std::string Sentence(std::string text)
{
    for (const char* word : {"pal", "sles-52568"})
    {
        const size_t length = std::strlen(word);
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + length))
        {
            for (size_t i = at; i < at + length; i++)
            {
                text[i] = static_cast<char>(Upper(static_cast<unsigned char>(text[i])));
            }
        }
    }

    for (char& c : text)
    {
        if (c != ' ')
        {
            c = static_cast<char>(Upper(static_cast<unsigned char>(c)));
            break;
        }
    }

    return text;
}

TextStyle Body(f32 size, const Rgba32& colour, bool bold = false)
{
    TextStyle style;
    style.font = bold ? OverlayFont::BodyBold : OverlayFont::Body;
    style.size = size;
    style.colour = colour;
    return style;
}

// A text's lines no wider than a width, broken at spaces (a word wider than the width on its own line)
std::vector<std::string> Wrapped(const std::string& text, f32 width, const TextStyle& style)
{
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at <= text.size())
    {
        size_t space = text.find(' ', at);
        std::string word = text.substr(at, space == std::string::npos ? std::string::npos : space - at);
        std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && OverlayTextWidth(longer, style) > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line = longer;
        }

        if (space == std::string::npos)
        {
            break;
        }

        at = space + 1;
    }

    if (!line.empty())
    {
        lines.push_back(line);
    }

    return lines;
}

// A text cut to a width, "..." where it's cut
std::string Fitted(const std::string& text, f32 width, const TextStyle& style)
{
    if (OverlayTextWidth(text, style) <= width)
    {
        return text;
    }

    std::string cut = text;
    while (!cut.empty() && OverlayTextWidth(cut + "...", style) > width)
    {
        cut.pop_back();
    }

    return cut + "...";
}

#if defined(__ANDROID__)
// A content:// document's name as the picker shows it (the activity's displayName: the URI itself says nothing)
std::string AndroidDisplayName(const std::string& uri)
{
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr)
    {
        return {};
    }

    std::string name;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(type, "displayName", "(Ljava/lang/String;)Ljava/lang/String;");
    if (method == nullptr)
    {
        env->ExceptionClear();
    }
    else
    {
        jstring given = env->NewStringUTF(uri.c_str());
        auto result = static_cast<jstring>(env->CallObjectMethod(activity, method, given));
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
        }
        else if (result != nullptr)
        {
            const char* chars = env->GetStringUTFChars(result, nullptr);
            if (chars != nullptr)
            {
                name = chars;
                env->ReleaseStringUTFChars(result, chars);
            }

            env->DeleteLocalRef(result);
        }

        env->DeleteLocalRef(given);
    }

    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return name;
}
#endif

// An image's name to show: its file's (a path's last part; a content:// URI's name from the system, else its last part decoded)
std::string ShownName(const std::string& image)
{
    if (image.rfind("content://", 0) == 0)
    {
#if defined(__ANDROID__)
        std::string name = AndroidDisplayName(image);
        if (!name.empty())
        {
            return name;
        }
#endif
        std::string last = image.substr(image.find_last_of('/') + 1);
        std::string decoded;
        for (size_t i = 0; i < last.size(); i++)
        {
            if (last[i] == '%' && i + 2 < last.size())
            {
                decoded += static_cast<char>(std::strtol(last.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            }
            else
            {
                decoded += last[i];
            }
        }

        size_t colon = decoded.find_last_of(":/");
        return colon == std::string::npos ? decoded : decoded.substr(colon + 1);
    }

    size_t slash = image.find_last_of("/\\");
    return slash == std::string::npos ? image : image.substr(slash + 1);
}

struct Screen
{
    std::string image;
    std::string name;
    // "" the game's disc, else what's wrong (none: no image)
    std::string problem;
    bool good = false;
    s32 focus = ButtonChoose;
    Region regions[Buttons] = {};
};

void Check(Screen& screen, const std::function<std::string(const std::string&)>& check)
{
    screen.good = false;
    screen.problem.clear();
    screen.name.clear();
    if (screen.image.empty())
    {
        return;
    }

    screen.name = ShownName(screen.image);
    screen.problem = check(screen.image);
    screen.good = screen.problem.empty();
    Native::Log("launcher: %s %s%s", screen.name.c_str(), screen.good ? "is the game's disc" : "isn't usable: ", screen.problem.c_str());
}

void ButtonBox(OverlayFrame& frame, const Region& r, const std::string& text, bool primary, bool enabled, bool focused)
{
    BoxStyle box;
    box.radius = r.height * 0.5f;
    box.fill = !enabled ? Hex(0xdfe6f0) : primary ? Orange : PillFill;
    if (focused)
    {
        box.border = 3.0f;
        box.borderColour = primary && enabled ? Brown : PillBlue;
        box.lip = 5.0f;
        box.lipColour = primary && enabled ? Lip : Hex(0xc9dcf7);
    }

    frame.Box(r.x, r.y, r.width, r.height, box);
    const Rgba32 colour = !enabled ? Hex(0x9aa8bb) : primary ? White : PillBlue;
    frame.Text(text, r.x + r.width * 0.5f, r.y + r.height * 0.5f, Body(22.0f, colour, primary), OverlayAlign::Centre);
}

f32 Seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<f32>(std::chrono::steady_clock::now() - start).count();
}

void Draw(OverlayFrame& frame, Screen& screen)
{
    frame.Clear();
    frame.Backdrop(Seconds());

    // The title and its line, as the options' "OPTIONS" and "CRASH TWINSANITY"
    TextStyle title;
    title.font = OverlayFont::Display;
    title.size = 66.0f;
    title.colour = Orange;
    title.letterSpacing = 1.0f;
    title.outline = 2.0f;
    title.outlineColour = Brown;
    title.dropY = 4.0f;
    title.dropColour = Brown;
    frame.Text("CRASH TWINSANITY", ReferenceWidth * 0.5f, 52.0f + OverlayLineHeight(title) * 0.5f, title, OverlayAlign::Centre);
    TextStyle sub = Body(18.0f, Hex(0xbcd9ff));
    sub.letterSpacing = 3.0f;
    frame.Text(Capitals(Text(TextLauncherDisc)), ReferenceWidth * 0.5f, 140.0f, sub, OverlayAlign::Centre);

    // The panel
    BoxStyle panel;
    panel.fill = White;
    panel.radius = 22.0f;
    panel.lip = 10.0f;
    panel.lipColour = Lip;
    panel.shadowOffset = 26.0f;
    panel.shadowBlur = 50.0f;
    panel.shadowColour = Hex(0x000000, 0.4f);
    frame.Box(PanelX, PanelY, PanelWidth, PanelHeight, panel);
    const f32 left = PanelX + Pad;
    const f32 inner = PanelWidth - Pad * 2.0f;

    // The image's name in its pill (or none yet)
    frame.Text(Sentence(Text(TextLauncherDisc)), left, PanelY + 44.0f, Body(22.0f, Ink, true));
    BoxStyle pill;
    pill.fill = PillFill;
    pill.radius = 16.0f;
    frame.Box(left, PanelY + 68.0f, inner, 58.0f, pill);
    TextStyle nameStyle = Body(22.0f, screen.image.empty() ? Note : Ink, !screen.image.empty());
    const std::string name = screen.image.empty() ? Sentence(Text(TextLauncherNone)) : FromUtf8(screen.name);
    frame.Text(Fitted(name, inner - 40.0f, nameStyle), left + 20.0f, PanelY + 97.0f, nameStyle);

    // Whether it's the game's disc, or what's wrong with it
    f32 y = PanelY + 158.0f;
    if (!screen.image.empty())
    {
        TextStyle status = Body(screen.good ? 20.0f : 17.0f, screen.good ? Good : Bad, screen.good);
        std::vector<std::string> lines = screen.good ? std::vector<std::string>{Sentence(Text(TextLauncherReady))}
                                                     : Wrapped(FromUtf8(screen.problem), inner, status);
        if (lines.size() > 3)
        {
            lines.resize(3);
            lines.back() += " ...";
        }

        for (const std::string& line : lines)
        {
            frame.Text(line, left, y, status);
            y += 24.0f;
        }
    }

    // What it has to be
    TextStyle hint = Body(16.0f, Note);
    f32 hintY = PanelY + 254.0f;
    for (const std::string& line : Wrapped(Sentence(Text(TextLauncherHint)), inner, hint))
    {
        frame.Text(line, left, hintY, hint);
        hintY += 22.0f;
    }

    // Choose and play, side by side under them; quit in the corner
    const f32 buttonsY = PanelY + PanelHeight - 34.0f - 54.0f;
    screen.regions[ButtonChoose] = {left, buttonsY, 380.0f, 54.0f};
    screen.regions[ButtonPlay] = {PanelX + PanelWidth - Pad - 260.0f, buttonsY, 260.0f, 54.0f};
    ButtonBox(frame, screen.regions[ButtonChoose], Sentence(Text(TextLauncherChoose)), false, true, screen.focus == ButtonChoose);
    ButtonBox(frame, screen.regions[ButtonPlay], Capitals(Text(TextLauncherPlay)), true, screen.good, screen.focus == ButtonPlay);
    TextStyle quit = Body(20.0f, Hex(0xdbe9ff), screen.focus == ButtonQuit);
    const std::string quitText = Sentence(Text(TextLauncherQuit));
    const f32 quitWidth = OverlayTextWidth(quitText, quit) + 40.0f;
    screen.regions[ButtonQuit] = {ReferenceWidth - 40.0f - quitWidth, ReferenceHeight - 40.0f - 44.0f, quitWidth, 44.0f};
    BoxStyle quitBox;
    quitBox.radius = 22.0f;
    quitBox.fill = Hex(0xffffff, screen.focus == ButtonQuit ? 0.22f : 0.08f);
    quitBox.border = 2.0f;
    quitBox.borderColour = Hex(0xffffff, screen.focus == ButtonQuit ? 0.6f : 0.15f);
    const Region& q = screen.regions[ButtonQuit];
    frame.Box(q.x, q.y, q.width, q.height, quitBox);
    frame.Text(quitText, q.x + q.width * 0.5f, q.y + q.height * 0.5f, quit, OverlayAlign::Centre);
}

// The focus moved along the buttons (play skipped while it can't be picked)
void MoveFocus(Screen& screen, s32 step)
{
    for (s32 tries = 0; tries < Buttons; tries++)
    {
        screen.focus = (screen.focus + step + Buttons) % Buttons;
        if (screen.focus != ButtonPlay || screen.good)
        {
            return;
        }
    }
}

using GlClearColor = void (*)(float, float, float, float);
using GlClear = void (*)(unsigned int);
using GlViewport = void (*)(int, int, int, int);
}

bool LauncherWanted()
{
    const char* wanted = std::getenv("TWIN_LAUNCHER");
    if (wanted != nullptr)
    {
        return wanted[0] != '0';
    }
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

bool RunLauncher(SDL_Window* window, const std::string& image, const std::function<std::string(const std::string&)>& check,
                 const std::function<std::string()>& pick, std::string* chosen)
{
    g_Language = LanguageIndex();
    SDL_GLContext context = NativeGl::CreateContext(window);
    if (context == nullptr)
    {
        // No screen of its own: the system's file picker straight away, as without the launcher
        Native::Log("launcher: no GL context (%s): the file picker instead", SDL_GetError());
        std::string picked = image;
        for (;;)
        {
            if (!picked.empty() && check(picked).empty())
            {
                *chosen = picked;
                return true;
            }

            picked = pick();
            if (picked.empty())
            {
                return false;
            }
        }
    }

    NativeGl::MakeCurrentOn(window, context);
    NativeGl::SetSwapInterval(1);
    auto clearColour = reinterpret_cast<GlClearColor>(NativeGl::GetProcAddress("glClearColor"));
    auto clear = reinterpret_cast<GlClear>(NativeGl::GetProcAddress("glClear"));
    auto viewport = reinterpret_cast<GlViewport>(NativeGl::GetProcAddress("glViewport"));
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    std::vector<SDL_Gamepad*> pads;

    Screen screen;
    screen.image = image;
    Check(screen, check);
    screen.focus = screen.good ? ButtonPlay : ButtonChoose;
    Native::Log("launcher: shown (%s)", screen.image.empty() ? "no disc image yet" : screen.good ? "the disc image is ready" : "the disc image isn't usable");

    bool done = false;
    bool play = false;
    auto activate = [&](s32 button)
    {
        switch (button)
        {
        case ButtonChoose:
        {
            std::string picked = pick();
            if (!picked.empty())
            {
                screen.image = picked;
                Check(screen, check);
                screen.focus = screen.good ? ButtonPlay : ButtonChoose;
            }

            break;
        }
        case ButtonPlay:
            if (screen.good)
            {
                play = true;
                done = true;
            }

            break;
        default:
            done = true;
            break;
        }
    };

    while (!done)
    {
        SDL_Event event;
        bool any = SDL_WaitEventTimeout(&event, 16);
        while (any && !done)
        {
            f32 x = 0.0f;
            f32 y = 0.0f;
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                done = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                WindowToReference(event.motion.x, event.motion.y, &x, &y);
                for (s32 b = 0; b < Buttons; b++)
                {
                    if (screen.regions[b].Holds(x, y) && (b != ButtonPlay || screen.good))
                    {
                        screen.focus = b;
                    }
                }

                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    WindowToReference(event.button.x, event.button.y, &x, &y);
                    for (s32 b = 0; b < Buttons; b++)
                    {
                        if (screen.regions[b].Holds(x, y))
                        {
                            screen.focus = b;
                            activate(b);
                            break;
                        }
                    }
                }

                break;
            case SDL_EVENT_KEY_DOWN:
                switch (event.key.scancode)
                {
                case SDL_SCANCODE_LEFT:
                case SDL_SCANCODE_UP:
                    MoveFocus(screen, -1);
                    break;
                case SDL_SCANCODE_RIGHT:
                case SDL_SCANCODE_DOWN:
                case SDL_SCANCODE_TAB:
                    MoveFocus(screen, 1);
                    break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                case SDL_SCANCODE_SPACE:
                    activate(screen.focus);
                    break;
                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_AC_BACK:
                    done = true;
                    break;
                default:
                    break;
                }

                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which))
                {
                    pads.push_back(pad);
                }

                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                switch (event.gbutton.button)
                {
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    MoveFocus(screen, -1);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    MoveFocus(screen, 1);
                    break;
                case SDL_GAMEPAD_BUTTON_SOUTH:
                    activate(screen.focus);
                    break;
                case SDL_GAMEPAD_BUTTON_START:
                    activate(ButtonPlay);
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                case SDL_GAMEPAD_BUTTON_BACK:
                    done = true;
                    break;
                default:
                    break;
                }

                break;
            default:
                break;
            }

            any = SDL_PollEvent(&event);
        }

        if (done)
        {
            break;
        }

        // The frame, at the window's size now (a phone turned, a window resized)
        int pixelWidth = 0;
        int pixelHeight = 0;
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
        SDL_GetWindowSize(window, &width, &height);
        if (pixelWidth <= 0 || pixelHeight <= 0)
        {
            continue;
        }

        NoteWindow(pixelWidth, pixelHeight, width > 0 ? static_cast<f32>(pixelWidth) / static_cast<f32>(width) : 1.0f);
        OverlayFrame& frame = RecordingFrame();
        Draw(frame, screen);
        if (viewport != nullptr && clearColour != nullptr && clear != nullptr)
        {
            viewport(0, 0, pixelWidth, pixelHeight);
            clearColour(0.0f, 0.0f, 0.0f, 1.0f);
            clear(0x00004000);
        }

        DrawOverlayCommands(frame.Commands(), pixelWidth, pixelHeight);
        NativeGl::SwapWindow(window);
    }

    // The screen's context gone before the graphics make theirs (the overlay's renderer made again in theirs)
    RecordingFrame().Clear();
    for (SDL_Gamepad* pad : pads)
    {
        SDL_CloseGamepad(pad);
    }

    NativeGl::MakeCurrentOn(window, nullptr);
    NativeGl::DestroyGlContext(context);
    ForgetOverlayRenderer();
    Native::Log("launcher: %s", play ? "play" : "quit");
    if (play)
    {
        *chosen = screen.image;
    }

    return play;
}
}

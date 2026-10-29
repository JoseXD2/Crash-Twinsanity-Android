#pragma once

// The launcher (Android's start; $TWIN_LAUNCHER=1 elsewhere, 0 never): before the game starts, a screen in the options' look on the
// game's window where the disc image is chosen and the game started. It shows the image remembered (its name, whether it's the
// PAL disc or what's wrong with it), "choose disc image" (the system's file picker) and "play" (once the image chosen is the
// game's); back quits. Drawn with the overlay's renderer in a GL context of its own, destroyed before the graphics make theirs
#include "common.h"

#include <functional>
#include <string>

struct SDL_Window;

namespace NativeUi
{
bool LauncherWanted();

// The launcher's screen on the window until an image that passes the check is chosen and play is picked (true, *chosen its path)
// or it's quit (false). image: the one remembered ("" none), checked first. check: "" when an image is the game's (it's left open,
// as the start-up's check leaves it), else what's wrong (English). pick: the system's file dialog ("" cancelled; it remembers one)
bool RunLauncher(SDL_Window* window, const std::string& image, const std::function<std::string(const std::string&)>& check,
                 const std::function<std::string()>& pick, std::string* chosen);
}

#include "app/app.h"
#include "core/input.h"

namespace game_pad {  // keyboard, pad_script.txt, cmd (src/game/pad.cpp)
void Init();
void Hook(int port, input::Pad& pad);
}
namespace game_diag {  // test runs: sound tap, start level (src/game/diag.cpp)
void Init();
void Loaded();
}

namespace game_watchdog { void Init(); }  // freeze report (src/game/watchdog.cpp)
namespace game_coop {
extern uint32_t g_mode_setting;  // coop.cpp: 0 off, 1 split screen
}
namespace game_view { extern uint32_t g_shape_setting; }  // Display Settings: picture shape (src/game/view.cpp)

// after the settings load, before the window and boot
static void GameInit() {
    game_pad::Init();
    game_diag::Init();
    game_watchdog::Init();
}

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"The Suffering: Ties That Bind",               // window and splash
    .short_title = L"The Suffering 2",               // message boxes
    .name = L"Suffering2",                      // window classes, crash dumps, handover event
    .log = "suffering2",                        // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Suffering2"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"picture_shape", nullptr, &game_view::g_shape_setting}, {"coop", nullptr, &game_coop::g_mode_setting}},  // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = GameInit,                      // after the settings load, before the window and boot
    .loaded = game_diag::Loaded,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = game_pad::Hook,                       // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
};

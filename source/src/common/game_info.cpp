#include "app/app.h"
#include "core/input.h"

#include <toml++/toml.hpp>

namespace game_pad {  // keyboard, pad_script.txt, cmd (src/game/pad.cpp)
void Init();
void Hook(int port, input::Pad& pad);
}
namespace game_diag {  // test runs: sound tap, start level (src/game/diag.cpp)
void Init();
void Loaded();
}

namespace game_watchdog { void Init(); }  // freeze report (src/game/watchdog.cpp)
namespace game_record { void Init(); }    // the session record: clock, load, window, controllers (src/game/record.cpp)
namespace game_detail { void Init(); }    // the detail record: every frame, every object, pictures (src/game/detail.cpp)
namespace game_coop {
void Ui(double fps);  // coop.cpp
void Fonts();         // ... the font of Player 2's ammunition, before the first frame
extern uint32_t g_p2_invert, g_p2_speed;  // Player 2's own look
extern uint32_t g_mode_setting;  // coop.cpp: 0 off, 1 split screen
}
namespace game_menu { extern uint32_t g_invert_pitch; }  // the game's own Invert Pitch Axis, Player 1's, kept between runs (menu.cpp)
namespace game_view { extern uint32_t g_shape_setting; }  // Display Settings: picture shape (src/game/view.cpp)

// after the settings load, before the window and boot
static void GameInit() {
    game_pad::Init();
    game_diag::Init();
    game_watchdog::Init();
    game_detail::Init();
    game_record::Init();
}

// the game window is up, before boot: the overlay's fonts, which the renderer builds with its first frame
static void GameWindow(void*) { game_coop::Fonts(); }

// what the settings file holds beyond the options. Player 2's up and down was "the other way up from the game's
// Invert Pitch Axis" until 2026-11-27 (p2_invert_look); since then it is his own, whatever Player 1 has
// (p2_look_inverted). A file of before is read the old way once: what Player 2 had stays what he has.
static void LoadMore(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    if (t.contains("p2_look_inverted") || !t.contains("p2_invert_look")) return;
    bool swapped = t["p2_invert_look"].value_or(int64_t(0)) != 0, game = game_menu::g_invert_pitch != 0;
    game_coop::g_p2_invert = swapped != game ? 1 : 0;
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
    .options = {{"picture_shape", nullptr, &game_view::g_shape_setting}, {"coop", nullptr, &game_coop::g_mode_setting},
                {"p2_look_inverted", nullptr, &game_coop::g_p2_invert}, {"p2_look_speed", nullptr, &game_coop::g_p2_speed},
                {"invert_pitch", nullptr, &game_menu::g_invert_pitch}},  // game settings kept in the settings file
    .load = LoadMore,                     // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = GameInit,                      // after the settings load, before the window and boot
    .loaded = game_diag::Loaded,                    // image in memory, before its entry point runs
    .window = GameWindow,                 // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = game_coop::Ui,                  // ImGui drawing each frame: the arrow above the partner's head, Player 2's ammunition
    .pad = game_pad::Hook,                       // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
};

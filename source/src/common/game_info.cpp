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
namespace game_meet { void Init(); }      // the meeting point: one short code to join (src/game/meet.cpp)
namespace game_record { void Init(); }    // the session record: clock, load, window, controllers (src/game/record.cpp)
namespace game_detail { void Init(); }    // the detail record: every frame, every object, pictures (src/game/detail.cpp)
namespace game_coop {
void Ui(double fps);  // coop.cpp
extern uint32_t g_p2_invert, g_p2_speed;  // Player 2's own look
extern uint32_t g_p2_volume;             // the host's sound on this PC when it joins a game, per cent
extern uint32_t g_mode_setting;  // coop.cpp: 0 off, 1 split screen
}
namespace game_menu { extern uint32_t g_invert_pitch; }  // the game's own Invert Pitch Axis, kept between runs (menu.cpp)
namespace game_view { extern uint32_t g_shape_setting; }  // Display Settings: picture shape (src/game/view.cpp)

// after the settings load, before the window and boot
static void GameInit() {
    game_pad::Init();
    game_diag::Init();
    game_watchdog::Init();
    game_detail::Init();
    game_record::Init();
    game_meet::Init();
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
                {"p2_invert_look", nullptr, &game_coop::g_p2_invert}, {"p2_look_speed", nullptr, &game_coop::g_p2_speed},
                {"join_volume", nullptr, &game_coop::g_p2_volume}, {"invert_pitch", nullptr, &game_menu::g_invert_pitch}},  // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = GameInit,                      // after the settings load, before the window and boot
    .loaded = game_diag::Loaded,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = game_coop::Ui,                  // ImGui drawing each frame: the arrow above the partner's head
    .pad = game_pad::Hook,                       // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
};

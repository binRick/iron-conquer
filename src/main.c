#include "raylib.h"
#include "game.h"
#include "debug_log.h"

#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif

static GameState g_state;

static void frame(void) {
    float dt = GetFrameTime();
    game_update(&g_state, dt);

    BeginDrawing();
        ClearBackground((Color){ 18, 18, 22, 255 });
        game_draw(&g_state);
    EndDrawing();
    game_post_draw(&g_state);
}

int main(void) {
    const int screen_w = 1280;
    const int screen_h = 720;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(screen_w, screen_h, "iron conquer");

    debug_log_init();
    game_init(&g_state, screen_w, screen_h);

#ifdef PLATFORM_WEB
    emscripten_set_main_loop(frame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose() && !g_state.should_quit) {
        frame();
    }
    game_shutdown(&g_state);
    debug_log_close();
    CloseWindow();
#endif

    return 0;
}

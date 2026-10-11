#include <pebble.h>

#define ATOM_RADIUS 4
#define PLAYBOX_WIDTH 200
#define PLAYBOX_HEIGHT 200
#define HUD_OFFSET 28
#define TICK_RATE_MS 33
#define SWIPE_THRESHOLD 12
#define WALL_GROWTH_SPEED 6
#define WALL_THICKNESS 4
#define TOTAL_AREA (PLAYBOX_WIDTH * PLAYBOX_HEIGHT)
#define WIN_THRESHOLD 70

#define MAX_ATOMS 50
#define MAX_ROOMS 100
#define MAX_WALLS 256
#define STAR_COUNT 40

#define ENABLE_TOUCH 1

// --- Persistent Save Data ---
// New versioned checkpoint save. Legacy keys are kept only for migration from
// earlier PebbleBall builds.
#define PERSIST_KEY_SAVE 100
#define PERSIST_KEY_LEVEL_LEGACY 1
#define PERSIST_KEY_LIVES_LEGACY 2
#define SAVE_MAGIC 0x5042
#define SAVE_VERSION 1

// Front-menu geometry (kept inside the original 200 px game width).
#define MENU_ITEM_CONTINUE 0
#define MENU_ITEM_NEW_GAME 1
#define MENU_ITEM_EXIT 2
#define MENU_ITEM_COUNT 3
#define MENU_ROW_X 25
#define MENU_ROW_W 150
#define MENU_ROW_H 28
#define MENU_CONTINUE_Y 104
#define MENU_NEW_GAME_Y 138
#define MENU_EXIT_Y 172

// --- Structures & States ---

typedef enum {
    STATE_MENU,
    STATE_IN_GAME,
    STATE_LEVEL_COMPLETE,
    STATE_GAME_OVER
} GameState;

typedef enum {
    WALL_STATE_NONE,
    WALL_STATE_BUILDING
} WallState;

typedef enum {
    ORIENTATION_HORIZONTAL,
    ORIENTATION_VERTICAL
} WallOrientation;

typedef struct {
    int x;
    int y;
    int dx;
    int dy;
} Atom;

typedef struct {
    WallState state;
    WallOrientation orientation;
    int room_index;
    int origin_x;
    int origin_y;
    int ext_positive;
    int ext_negative;
} ActiveWall;

typedef struct {
    bool active;
    GRect bounds;
} Room;

typedef struct __attribute__((__packed__)) {
    uint16_t magic;
    uint8_t version;
    uint8_t reserved;
    int16_t level;
    int16_t lives;
} SaveData;

// --- Global App State ---

static Window *s_main_window;
static Layer *s_canvas_layer;
static AppTimer *s_physics_timer;

static GameState s_current_state = STATE_MENU;

static Atom s_atoms[MAX_ATOMS];
static int s_atom_count = 0;

static ActiveWall s_active_wall = {
    .state = WALL_STATE_NONE
};

static Room s_rooms[MAX_ROOMS];
static int s_room_count = 0;

static GRect s_captured_rooms[MAX_WALLS];
static int s_captured_room_count = 0;

static GRect s_built_walls[MAX_WALLS];
static int s_built_wall_count = 0;

static int s_current_level = 1;
static int s_lives = 3;
static int s_total_captured_area = 0;

static bool s_has_save = false;
static int s_menu_index = MENU_ITEM_NEW_GAME;

static GPoint s_stars[STAR_COUNT];

// Hybrid Button Cursor State
static GPoint s_cursor_pos = {100, 100};
static bool s_cursor_axis_x = true;

#if ENABLE_TOUCH && defined(PBL_TOUCH)
static bool s_is_swiping = false;
static int16_t s_touch_start_x = 0;
static int16_t s_touch_start_y = 0;
static bool s_touch_subscribed = false;
#endif

// --- Helper Math & Memory Management ---

static bool point_in_rect(int px, int py, GRect r) {
    // Pebble rectangles are best treated as left/top inclusive and
    // right/bottom exclusive. This prevents x == width / y == height
    // from being accepted as a valid point inside the playfield.
    return (
        px >= r.origin.x &&
        px < r.origin.x + r.size.w &&
        py >= r.origin.y &&
        py < r.origin.y + r.size.h
    );
}

static bool wall_origin_fits_room(
    GRect room,
    int px,
    int py,
    WallOrientation orientation
) {
    const int right = room.origin.x + room.size.w;
    const int bottom = room.origin.y + room.size.h;

    if (orientation == ORIENTATION_HORIZONTAL) {
        // Both resulting rooms must retain positive height.
        return py > room.origin.y &&
               (py + WALL_THICKNESS) < bottom;
    }

    // Both resulting rooms must retain positive width.
    return px > room.origin.x &&
           (px + WALL_THICKNESS) < right;
}

// Garbage collection finder for recycled arrays
static int get_free_room_index() {
    for (int i = 0; i < s_room_count; i++) {
        if (!s_rooms[i].active) {
            return i;
        }
    }

    if (s_room_count < MAX_ROOMS) {
        return s_room_count++;
    }

    return -1;
}

// --- Save System ---

static bool save_values_are_valid(int level, int lives) {
    return level >= 1 &&
           level <= 32767 &&
           lives >= 1 &&
           lives <= 3;
}

static void clear_save(void) {
    persist_delete(PERSIST_KEY_SAVE);
    persist_delete(PERSIST_KEY_LEVEL_LEGACY);
    persist_delete(PERSIST_KEY_LIVES_LEGACY);

    s_has_save = false;
}

static bool write_save_checkpoint_for(int level, int lives) {
    if (!save_values_are_valid(level, lives)) {
        return false;
    }

    SaveData save = {
        .magic = SAVE_MAGIC,
        .version = SAVE_VERSION,
        .reserved = 0,
        .level = (int16_t)level,
        .lives = (int16_t)lives
    };

    int written = persist_write_data(
        PERSIST_KEY_SAVE,
        &save,
        sizeof(save)
    );

    if (written == (int)sizeof(save)) {
        s_has_save = true;
        return true;
    }

    return false;
}

static bool write_save_checkpoint(void) {
    return write_save_checkpoint_for(
        s_current_level,
        s_lives
    );
}

static bool load_save_checkpoint(void) {
    SaveData save;

    int read = persist_read_data(
        PERSIST_KEY_SAVE,
        &save,
        sizeof(save)
    );

    if (
        read == (int)sizeof(save) &&
        save.magic == SAVE_MAGIC &&
        save.version == SAVE_VERSION &&
        save_values_are_valid(save.level, save.lives)
    ) {
        s_current_level = save.level;
        s_lives = save.lives;
        s_has_save = true;

        return true;
    }

    // One-time migration from the original two-key save format.
    if (
        persist_exists(PERSIST_KEY_LEVEL_LEGACY) &&
        persist_exists(PERSIST_KEY_LIVES_LEGACY)
    ) {
        int legacy_level =
            persist_read_int(PERSIST_KEY_LEVEL_LEGACY);

        int legacy_lives =
            persist_read_int(PERSIST_KEY_LIVES_LEGACY);

        if (save_values_are_valid(
                legacy_level,
                legacy_lives
            )) {
            s_current_level = legacy_level;
            s_lives = legacy_lives;

            if (write_save_checkpoint()) {
                persist_delete(
                    PERSIST_KEY_LEVEL_LEGACY
                );

                persist_delete(
                    PERSIST_KEY_LIVES_LEGACY
                );

                return true;
            }
        }
    }

    clear_save();

    s_current_level = 1;
    s_lives = 3;

    return false;
}

static void prepare_menu(void) {
    s_menu_index =
        s_has_save
            ? MENU_ITEM_CONTINUE
            : MENU_ITEM_NEW_GAME;
}

// --- Game Logic & State Transitions ---

static void set_background_atoms() {
    s_atom_count = 10;

    const int margin = ATOM_RADIUS + 2;

    const int usable_w =
        PLAYBOX_WIDTH - (margin * 2);

    const int usable_h =
        PLAYBOX_HEIGHT - (margin * 2);

    const int offset_x =
        rand() % usable_w;

    const int offset_y =
        rand() % usable_h;

    for (int i = 0; i < s_atom_count; i++) {
        // Spread the decorative atoms so duplicate velocity pairs do not
        // permanently overlap and look like a single ball.
        s_atoms[i].x =
            margin +
            ((offset_x + (i * 37)) % usable_w);

        s_atoms[i].y =
            margin +
            ((offset_y + (i * 61)) % usable_h);

        s_atoms[i].dx =
            (rand() & 1) ? 4 : -4;

        s_atoms[i].dy =
            (rand() & 1) ? 4 : -4;
    }

    // Generate new random stars
    for (int i = 0; i < STAR_COUNT; i++) {
        s_stars[i] = GPoint(
            rand() % PLAYBOX_WIDTH,
            rand() % PLAYBOX_HEIGHT
        );
    }
}

static void init_level(int level) {
    s_current_level = level;

    s_atom_count =
        s_current_level + 1;

    if (s_atom_count > MAX_ATOMS) {
        s_atom_count = MAX_ATOMS;
    }

    int speed =
        3 + (s_current_level / 3);

    if (speed > 6) {
        speed = 6;
    }

    const int margin =
        ATOM_RADIUS + 2;

    const int usable_w =
        PLAYBOX_WIDTH - (margin * 2);

    const int usable_h =
        PLAYBOX_HEIGHT - (margin * 2);

    const int offset_x =
        rand() % usable_w;

    const int offset_y =
        rand() % usable_h;

    for (int i = 0; i < s_atom_count; i++) {
        // Unique starting positions stop atoms with matching velocity signs
        // from remaining perfectly stacked for the entire level.
        s_atoms[i].x =
            margin +
            ((offset_x + (i * 37)) % usable_w);

        s_atoms[i].y =
            margin +
            ((offset_y + (i * 61)) % usable_h);

        s_atoms[i].dx =
            (rand() & 1)
                ? speed
                : -speed;

        s_atoms[i].dy =
            (rand() & 1)
                ? speed
                : -speed;
    }

    for (int i = 0; i < MAX_ROOMS; i++) {
        s_rooms[i].active = false;
    }

    s_rooms[0].active = true;

    s_rooms[0].bounds =
        GRect(
            0,
            0,
            PLAYBOX_WIDTH,
            PLAYBOX_HEIGHT
        );

    s_room_count = 1;

    s_captured_room_count = 0;
    s_built_wall_count = 0;
    s_total_captured_area = 0;

    s_active_wall.state =
        WALL_STATE_NONE;

    s_cursor_pos.x = 100;
    s_cursor_pos.y = 100;

    // Generate new random stars for the level.
    for (int i = 0; i < STAR_COUNT; i++) {
        s_stars[i] = GPoint(
            rand() % PLAYBOX_WIDTH,
            rand() % PLAYBOX_HEIGHT
        );
    }
}

static void transition_to_state(
    GameState new_state
) {
    s_current_state = new_state;

    if (new_state == STATE_MENU) {
        prepare_menu();
        set_background_atoms();

    } else if (new_state == STATE_IN_GAME) {
        // Gameplay saves are level checkpoints rather than full board snapshots.
        // This keeps persistence tiny and guarantees a clean room graph on resume.
        write_save_checkpoint();

        init_level(
            s_current_level
        );

    } else if (new_state == STATE_GAME_OVER) {
        set_background_atoms();

    } else if (new_state == STATE_LEVEL_COMPLETE) {
        // Keeps the board visible, overlays UI.
    }

    if (s_canvas_layer) {
        layer_mark_dirty(
            s_canvas_layer
        );
    }
}

static void handle_life_lost() {
    vibes_double_pulse();

    s_lives--;

    if (s_lives <= 0) {
        clear_save();

        transition_to_state(
            STATE_GAME_OVER
        );

    } else {
        // Life loss restarts this level, so immediately checkpoint
        // the reduced life count before rebuilding the board.
        write_save_checkpoint();

        init_level(
            s_current_level
        );
    }
}

static void split_room(
    int target_room,
    ActiveWall wall
) {
    if (
        target_room < 0 ||
        target_room >= s_room_count ||
        !s_rooms[target_room].active
    ) {
        return;
    }

    GRect r =
        s_rooms[target_room].bounds;

    GRect r1;
    GRect r2;
    GRect wall_rect;

    if (
        wall.orientation ==
        ORIENTATION_HORIZONTAL
    ) {
        r1 = GRect(
            r.origin.x,
            r.origin.y,
            r.size.w,
            wall.origin_y - r.origin.y
        );

        r2 = GRect(
            r.origin.x,
            wall.origin_y + WALL_THICKNESS,
            r.size.w,
            (r.origin.y + r.size.h) -
                (wall.origin_y + WALL_THICKNESS)
        );

        wall_rect = GRect(
            r.origin.x,
            wall.origin_y,
            r.size.w,
            WALL_THICKNESS
        );

    } else {
        r1 = GRect(
            r.origin.x,
            r.origin.y,
            wall.origin_x - r.origin.x,
            r.size.h
        );

        r2 = GRect(
            wall.origin_x + WALL_THICKNESS,
            r.origin.y,
            (r.origin.x + r.size.w) -
                (wall.origin_x + WALL_THICKNESS),
            r.size.h
        );

        wall_rect = GRect(
            wall.origin_x,
            r.origin.y,
            WALL_THICKNESS,
            r.size.h
        );
    }

    // Defensive geometry validation.
    if (
        r1.size.w <= 0 ||
        r1.size.h <= 0 ||
        r2.size.w <= 0 ||
        r2.size.h <= 0 ||
        wall_rect.size.w <= 0 ||
        wall_rect.size.h <= 0
    ) {
        return;
    }

    s_rooms[target_room].active =
        false;

    if (
        s_built_wall_count <
        MAX_WALLS
    ) {
        s_built_walls[
            s_built_wall_count++
        ] = wall_rect;
    }

    s_total_captured_area +=
        wall_rect.size.w *
        wall_rect.size.h;

    int atoms1 = 0;
    int atoms2 = 0;

    for (int i = 0; i < s_atom_count; i++) {
        if (
            point_in_rect(
                s_atoms[i].x,
                s_atoms[i].y,
                r1
            )
        ) {
            atoms1++;

        } else if (
            point_in_rect(
                s_atoms[i].x,
                s_atoms[i].y,
                r2
            )
        ) {
            atoms2++;
        }
    }

    bool r1_active =
        atoms1 > 0 &&
        r1.size.w > 0 &&
        r1.size.h > 0;

    bool r2_active =
        atoms2 > 0 &&
        r2.size.w > 0 &&
        r2.size.h > 0;

    // Process captured empty room 1.
    if (
        !r1_active &&
        r1.size.w > 0 &&
        r1.size.h > 0
    ) {
        if (
            s_captured_room_count <
            MAX_WALLS
        ) {
            s_captured_rooms[
                s_captured_room_count++
            ] = r1;
        }

        s_total_captured_area +=
            r1.size.w *
            r1.size.h;
    }

    // Process captured empty room 2.
    if (
        !r2_active &&
        r2.size.w > 0 &&
        r2.size.h > 0
    ) {
        if (
            s_captured_room_count <
            MAX_WALLS
        ) {
            s_captured_rooms[
                s_captured_room_count++
            ] = r2;
        }

        s_total_captured_area +=
            r2.size.w *
            r2.size.h;
    }

    // Slot recycling for active rooms.
    if (r1_active) {
        s_rooms[target_room] =
            (Room){
                .active = true,
                .bounds = r1
            };

        if (r2_active) {
            int free_idx =
                get_free_room_index();

            if (free_idx != -1) {
                s_rooms[free_idx] =
                    (Room){
                        .active = true,
                        .bounds = r2
                    };
            }
        }

    } else if (r2_active) {
        s_rooms[target_room] =
            (Room){
                .active = true,
                .bounds = r2
            };
    }

    int percent =
        (s_total_captured_area * 100) /
        TOTAL_AREA;

    if (percent >= WIN_THRESHOLD) {
        vibes_short_pulse();

        // The completed level is no longer the checkpoint.
        // If the app closes while the clear overlay is visible,
        // Continue should start the next one.
        write_save_checkpoint_for(
            s_current_level + 1,
            s_lives
        );

        transition_to_state(
            STATE_LEVEL_COMPLETE
        );
    }
}

// --- Wall Growth Logic ---

static void start_building_wall(
    int px,
    int raw_y,
    WallOrientation orientation
) {
    if (
        s_active_wall.state !=
        WALL_STATE_NONE
    ) {
        return;
    }

    int py =
        raw_y - HUD_OFFSET;

    // Reject HUD/outside-edge input.
    if (
        px < 0 ||
        px >= PLAYBOX_WIDTH ||
        py < 0 ||
        py >= PLAYBOX_HEIGHT
    ) {
        return;
    }

    int target = -1;

    for (
        int r = 0;
        r < s_room_count;
        r++
    ) {
        if (
            s_rooms[r].active &&
            point_in_rect(
                px,
                py,
                s_rooms[r].bounds
            )
        ) {
            target = r;
            break;
        }
    }

    if (target == -1) {
        return;
    }

    if (
        !wall_origin_fits_room(
            s_rooms[target].bounds,
            px,
            py,
            orientation
        )
    ) {
        return;
    }

    s_active_wall.state =
        WALL_STATE_BUILDING;

    s_active_wall.orientation =
        orientation;

    s_active_wall.room_index =
        target;

    s_active_wall.origin_x =
        px;

    s_active_wall.origin_y =
        py;

    s_active_wall.ext_positive =
        (orientation == ORIENTATION_HORIZONTAL)
            ? px
            : py;

    s_active_wall.ext_negative =
        s_active_wall.ext_positive;
}

static bool update_wall_growth() {
    if (
        s_active_wall.state !=
        WALL_STATE_BUILDING
    ) {
        return false;
    }

    if (
        s_active_wall.room_index < 0 ||
        s_active_wall.room_index >=
            s_room_count ||
        !s_rooms[
            s_active_wall.room_index
        ].active
    ) {
        s_active_wall.state =
            WALL_STATE_NONE;

        return false;
    }

    GRect room_bounds =
        s_rooms[
            s_active_wall.room_index
        ].bounds;

    bool pos_hit = false;
    bool neg_hit = false;

    if (
        s_active_wall.orientation ==
        ORIENTATION_HORIZONTAL
    ) {
        s_active_wall.ext_positive +=
            WALL_GROWTH_SPEED;

        if (
            s_active_wall.ext_positive >=
            room_bounds.origin.x +
            room_bounds.size.w
        ) {
            s_active_wall.ext_positive =
                room_bounds.origin.x +
                room_bounds.size.w;

            pos_hit = true;
        }

        s_active_wall.ext_negative -=
            WALL_GROWTH_SPEED;

        if (
            s_active_wall.ext_negative <=
            room_bounds.origin.x
        ) {
            s_active_wall.ext_negative =
                room_bounds.origin.x;

            neg_hit = true;
        }

    } else {
        s_active_wall.ext_positive +=
            WALL_GROWTH_SPEED;

        if (
            s_active_wall.ext_positive >=
            room_bounds.origin.y +
            room_bounds.size.h
        ) {
            s_active_wall.ext_positive =
                room_bounds.origin.y +
                room_bounds.size.h;

            pos_hit = true;
        }

        s_active_wall.ext_negative -=
            WALL_GROWTH_SPEED;

        if (
            s_active_wall.ext_negative <=
            room_bounds.origin.y
        ) {
            s_active_wall.ext_negative =
                room_bounds.origin.y;

            neg_hit = true;
        }
    }

    // Do NOT split the room here.
    // Physics must first check the fully-grown wall.
    return pos_hit && neg_hit;
}

// --- Menu & Button Controls ---

static bool menu_item_enabled(
    int item
) {
    return item != MENU_ITEM_CONTINUE ||
           s_has_save;
}

static void move_menu_selection(
    int direction
) {
    int candidate =
        s_menu_index;

    for (
        int i = 0;
        i < MENU_ITEM_COUNT;
        i++
    ) {
        candidate += direction;

        if (candidate < 0) {
            candidate =
                MENU_ITEM_COUNT - 1;
        }

        if (
            candidate >=
            MENU_ITEM_COUNT
        ) {
            candidate = 0;
        }

        if (
            menu_item_enabled(
                candidate
            )
        ) {
            s_menu_index =
                candidate;

            layer_mark_dirty(
                s_canvas_layer
            );

            return;
        }
    }
}

static void start_new_game(void) {
    clear_save();

    s_current_level = 1;
    s_lives = 3;

    transition_to_state(
        STATE_IN_GAME
    );
}

static void continue_game(void) {
    if (load_save_checkpoint()) {
        transition_to_state(
            STATE_IN_GAME
        );

    } else {
        // A stale/corrupt save can disappear between
        // drawing and selection.
        prepare_menu();

        layer_mark_dirty(
            s_canvas_layer
        );
    }
}

static void activate_menu_selection(void) {
    switch (s_menu_index) {
        case MENU_ITEM_CONTINUE:
            if (s_has_save) {
                continue_game();
            }
            break;

        case MENU_ITEM_NEW_GAME:
            start_new_game();
            break;

        case MENU_ITEM_EXIT:
            window_stack_pop_all(
                true
            );
            break;
    }
}

static int menu_item_from_y(int y) {
    if (
        y >= MENU_CONTINUE_Y &&
        y < MENU_CONTINUE_Y +
            MENU_ROW_H
    ) {
        return MENU_ITEM_CONTINUE;
    }

    if (
        y >= MENU_NEW_GAME_Y &&
        y < MENU_NEW_GAME_Y +
            MENU_ROW_H
    ) {
        return MENU_ITEM_NEW_GAME;
    }

    if (
        y >= MENU_EXIT_Y &&
        y < MENU_EXIT_Y +
            MENU_ROW_H
    ) {
        return MENU_ITEM_EXIT;
    }

    return -1;
}

static void back_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
    (void)recognizer;
    (void)context;

    if (
        s_current_state ==
        STATE_IN_GAME
    ) {
        write_save_checkpoint();

        transition_to_state(
            STATE_MENU
        );

    } else if (
        s_current_state ==
        STATE_LEVEL_COMPLETE
    ) {
        s_current_level++;

        write_save_checkpoint();

        transition_to_state(
            STATE_MENU
        );

    } else if (
        s_current_state ==
        STATE_GAME_OVER
    ) {
        transition_to_state(
            STATE_MENU
        );

    } else if (
        s_current_state ==
        STATE_MENU
    ) {
        window_stack_pop_all(
            true
        );
    }
}

static void select_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
    (void)recognizer;
    (void)context;

    if (
        s_current_state ==
        STATE_MENU
    ) {
        activate_menu_selection();

    } else if (
        s_current_state ==
        STATE_GAME_OVER
    ) {
        start_new_game();

    } else if (
        s_current_state ==
        STATE_LEVEL_COMPLETE
    ) {
        s_current_level++;

        transition_to_state(
            STATE_IN_GAME
        );

    } else if (
        s_current_state ==
        STATE_IN_GAME
    ) {
        s_cursor_axis_x =
            !s_cursor_axis_x;

        layer_mark_dirty(
            s_canvas_layer
        );
    }
}

static void up_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
    (void)recognizer;
    (void)context;

    if (
        s_current_state ==
        STATE_MENU
    ) {
        move_menu_selection(-1);
        return;
    }

    if (
        s_current_state !=
        STATE_IN_GAME
    ) {
        return;
    }

    if (s_cursor_axis_x) {
        s_cursor_pos.x -= 10;

        if (s_cursor_pos.x < 0) {
            s_cursor_pos.x = 0;
        }

    } else {
        s_cursor_pos.y -= 10;

        if (s_cursor_pos.y < 0) {
            s_cursor_pos.y = 0;
        }
    }

    layer_mark_dirty(
        s_canvas_layer
    );
}

static void down_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
    (void)recognizer;
    (void)context;

    if (
        s_current_state ==
        STATE_MENU
    ) {
        move_menu_selection(1);
        return;
    }

    if (
        s_current_state !=
        STATE_IN_GAME
    ) {
        return;
    }

    if (s_cursor_axis_x) {
        s_cursor_pos.x += 10;

        if (
            s_cursor_pos.x >=
            PLAYBOX_WIDTH
        ) {
            s_cursor_pos.x =
                PLAYBOX_WIDTH - 1;
        }

    } else {
        s_cursor_pos.y += 10;

        if (
            s_cursor_pos.y >=
            PLAYBOX_HEIGHT
        ) {
            s_cursor_pos.y =
                PLAYBOX_HEIGHT - 1;
        }
    }

    layer_mark_dirty(
        s_canvas_layer
    );
}

static void up_long_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
    (void)recognizer;
    (void)context;

    if (
        s_current_state ==
        STATE_IN_GAME
    ) {
        start_building_wall(
            s_cursor_pos.x,
            s_cursor_pos.y +
                HUD_OFFSET,
            ORIENTATION_VERTICAL
        );
    }
}

static void down_long_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
    (void)recognizer;
    (void)context;

    if (
        s_current_state ==
        STATE_IN_GAME
    ) {
        start_building_wall(
            s_cursor_pos.x,
            s_cursor_pos.y +
                HUD_OFFSET,
            ORIENTATION_HORIZONTAL
        );
    }
}

static void click_config_provider(
    void *context
) {
    (void)context;

    window_single_click_subscribe(
        BUTTON_ID_BACK,
        back_click_handler
    );

    window_single_click_subscribe(
        BUTTON_ID_SELECT,
        select_click_handler
    );

    window_single_click_subscribe(
        BUTTON_ID_UP,
        up_click_handler
    );

    window_single_click_subscribe(
        BUTTON_ID_DOWN,
        down_click_handler
    );

    window_long_click_subscribe(
        BUTTON_ID_UP,
        500,
        up_long_click_handler,
        NULL
    );

    window_long_click_subscribe(
        BUTTON_ID_DOWN,
        500,
        down_long_click_handler,
        NULL
    );
}

// --- Unified Touch Input Handler ---

#if ENABLE_TOUCH && defined(PBL_TOUCH)

static void touch_handler(
    const TouchEvent *event,
    void *context
) {
    (void)context;

    int pos_x = event->x;
    int pos_y = event->y;
    int event_type = event->type;

    if (
        event_type ==
        TouchEvent_Touchdown
    ) {
        s_touch_start_x =
            pos_x;

        s_touch_start_y =
            pos_y;

        s_is_swiping =
            true;

        if (
            s_current_state ==
            STATE_MENU
        ) {
            int item =
                menu_item_from_y(
                    pos_y
                );

            if (
                item >= 0 &&
                menu_item_enabled(item)
            ) {
                s_menu_index =
                    item;

                layer_mark_dirty(
                    s_canvas_layer
                );
            }

            return;
        }

        if (
            s_current_state ==
            STATE_IN_GAME
        ) {
            int game_y =
                pos_y - HUD_OFFSET;

            // Ignore touches in the HUD or outside
            // the logical playfield.
            if (
                pos_x < 0 ||
                pos_x >= PLAYBOX_WIDTH ||
                game_y < 0 ||
                game_y >= PLAYBOX_HEIGHT
            ) {
                s_is_swiping =
                    false;

                return;
            }

            s_cursor_pos.x =
                pos_x;

            s_cursor_pos.y =
                game_y;

            layer_mark_dirty(
                s_canvas_layer
            );
        }

    } else if (
        event_type ==
        TouchEvent_PositionUpdate
    ) {
        if (!s_is_swiping) {
            return;
        }

        if (
            s_current_state ==
            STATE_IN_GAME
        ) {
            int16_t dx =
                pos_x -
                s_touch_start_x;

            int16_t dy =
                pos_y -
                s_touch_start_y;

            int16_t adx =
                dx < 0
                    ? -dx
                    : dx;

            int16_t ady =
                dy < 0
                    ? -dy
                    : dy;

            if (
                adx > SWIPE_THRESHOLD ||
                ady > SWIPE_THRESHOLD
            ) {
                if (
                    adx >
                    (ady * 3) / 2
                ) {
                    start_building_wall(
                        s_touch_start_x,
                        s_touch_start_y,
                        ORIENTATION_HORIZONTAL
                    );

                    s_is_swiping =
                        false;

                } else if (
                    ady >
                    (adx * 3) / 2
                ) {
                    start_building_wall(
                        s_touch_start_x,
                        s_touch_start_y,
                        ORIENTATION_VERTICAL
                    );

                    s_is_swiping =
                        false;
                }
            }
        }

    } else if (
        event_type ==
        TouchEvent_Liftoff
    ) {
        if (!s_is_swiping) {
            return;
        }

        s_is_swiping = false;

        int16_t dx =
            pos_x -
            s_touch_start_x;

        int16_t dy =
            pos_y -
            s_touch_start_y;

        int16_t adx =
            dx < 0
                ? -dx
                : dx;

        int16_t ady =
            dy < 0
                ? -dy
                : dy;

        if (
            adx < 15 &&
            ady < 15
        ) {
            if (
                s_current_state ==
                STATE_MENU
            ) {
                int item =
                    menu_item_from_y(
                        pos_y
                    );

                if (
                    item >= 0 &&
                    menu_item_enabled(
                        item
                    )
                ) {
                    s_menu_index =
                        item;

                    activate_menu_selection();
                }

            } else if (
                s_current_state ==
                STATE_GAME_OVER
            ) {
                start_new_game();

            } else if (
                s_current_state ==
                STATE_LEVEL_COMPLETE
            ) {
                s_current_level++;

                transition_to_state(
                    STATE_IN_GAME
                );
            }
        }
    }
}

#endif

// --- Physics Engine ---

static bool check_active_wall_penalty(
    Atom *atom
) {
    if (
        s_active_wall.state !=
        WALL_STATE_BUILDING
    ) {
        return false;
    }

    if (
        s_active_wall.orientation ==
        ORIENTATION_HORIZONTAL
    ) {
        if (
            abs(
                atom->y -
                s_active_wall.origin_y
            ) <=
            ATOM_RADIUS +
            (WALL_THICKNESS / 2)
        ) {
            if (
                atom->x >=
                    s_active_wall.ext_negative &&
                atom->x <=
                    s_active_wall.ext_positive
            ) {
                return true;
            }
        }

    } else {
        if (
            abs(
                atom->x -
                s_active_wall.origin_x
            ) <=
            ATOM_RADIUS +
            (WALL_THICKNESS / 2)
        ) {
            if (
                atom->y >=
                    s_active_wall.ext_negative &&
                atom->y <=
                    s_active_wall.ext_positive
            ) {
                return true;
            }
        }
    }

    return false;
}

static void update_physics_tick(
    void *context
) {
    (void)context;

    // AppTimer callbacks are one-shot.
    s_physics_timer = NULL;

    bool wall_completed_this_tick =
        false;

    if (
        s_current_state ==
        STATE_IN_GAME
    ) {
        wall_completed_this_tick =
            update_wall_growth();

        // Collision must be checked AFTER growth
        // but BEFORE the wall is committed.
        for (
            int i = 0;
            i < s_atom_count;
            i++
        ) {
            if (
                check_active_wall_penalty(
                    &s_atoms[i]
                )
            ) {
                handle_life_lost();

                if (
                    s_current_state ==
                    STATE_IN_GAME
                ) {
                    layer_mark_dirty(
                        s_canvas_layer
                    );
                }

                s_physics_timer =
                    app_timer_register(
                        TICK_RATE_MS,
                        update_physics_tick,
                        NULL
                    );

                return;
            }
        }

        if (
            wall_completed_this_tick &&
            s_active_wall.state ==
                WALL_STATE_BUILDING
        ) {
            ActiveWall completed_wall =
                s_active_wall;

            split_room(
                completed_wall.room_index,
                completed_wall
            );

            s_active_wall.state =
                WALL_STATE_NONE;
        }
    }

    for (
        int i = 0;
        i < s_atom_count;
        i++
    ) {
        GRect bounds;

        if (
            s_current_state ==
                STATE_IN_GAME ||
            s_current_state ==
                STATE_LEVEL_COMPLETE
        ) {
            bounds =
                GRect(
                    0,
                    0,
                    PLAYBOX_WIDTH,
                    PLAYBOX_HEIGHT
                );

            for (
                int r = 0;
                r < s_room_count;
                r++
            ) {
                if (
                    s_rooms[r].active &&
                    point_in_rect(
                        s_atoms[i].x,
                        s_atoms[i].y,
                        s_rooms[r].bounds
                    )
                ) {
                    bounds =
                        s_rooms[r].bounds;

                    break;
                }
            }

        } else {
            bounds =
                GRect(
                    0,
                    0,
                    PLAYBOX_WIDTH,
                    228
                );
        }

        int next_x =
            s_atoms[i].x +
            s_atoms[i].dx;

        int next_y =
            s_atoms[i].y +
            s_atoms[i].dy;

        if (
            next_x - ATOM_RADIUS <=
                bounds.origin.x ||
            next_x + ATOM_RADIUS >=
                bounds.origin.x +
                bounds.size.w
        ) {
            s_atoms[i].dx =
                -s_atoms[i].dx;

            next_x =
                s_atoms[i].x +
                s_atoms[i].dx;
        }

        if (
            next_y - ATOM_RADIUS <=
                bounds.origin.y ||
            next_y + ATOM_RADIUS >=
                bounds.origin.y +
                bounds.size.h
        ) {
            s_atoms[i].dy =
                -s_atoms[i].dy;

            next_y =
                s_atoms[i].y +
                s_atoms[i].dy;
        }

        s_atoms[i].x =
            next_x;

        s_atoms[i].y =
            next_y;
    }

    if (s_canvas_layer) {
        layer_mark_dirty(
            s_canvas_layer
        );
    }

    s_physics_timer =
        app_timer_register(
            TICK_RATE_MS,
            update_physics_tick,
            NULL
        );
}

// --- Canvas Drawing ---

static void draw_text_with_shadow(
    GContext *ctx,
    const char *text,
    const char *font_key,
    GRect box,
    GTextAlignment align,
    GColor text_color
) {
    GRect shadow_box = box;

    shadow_box.origin.x += 2;
    shadow_box.origin.y += 2;

    graphics_context_set_text_color(
        ctx,
        GColorBlack
    );

    graphics_draw_text(
        ctx,
        text,
        fonts_get_system_font(
            font_key
        ),
        shadow_box,
        GTextOverflowModeWordWrap,
        align,
        NULL
    );

    graphics_context_set_text_color(
        ctx,
        text_color
    );

    graphics_draw_text(
        ctx,
        text,
        fonts_get_system_font(
            font_key
        ),
        box,
        GTextOverflowModeWordWrap,
        align,
        NULL
    );
}

static void draw_menu_row(
    GContext *ctx,
    int item,
    int y,
    const char *label
) {
    bool enabled =
        menu_item_enabled(item);

    bool selected =
        s_menu_index == item;

    if (
        selected &&
        enabled
    ) {
        graphics_context_set_fill_color(
            ctx,
            GColorDarkGray
        );

        graphics_fill_rect(
            ctx,
            GRect(
                MENU_ROW_X + 2,
                y + 2,
                MENU_ROW_W,
                MENU_ROW_H
            ),
            6,
            GCornersAll
        );

        graphics_context_set_fill_color(
            ctx,
            GColorPictonBlue
        );

    } else if (enabled) {
        graphics_context_set_fill_color(
            ctx,
            GColorLightGray
        );

    } else {
        graphics_context_set_fill_color(
            ctx,
            GColorDarkGray
        );
    }

    graphics_fill_rect(
        ctx,
        GRect(
            MENU_ROW_X,
            y,
            MENU_ROW_W,
            MENU_ROW_H
        ),
        6,
        GCornersAll
    );

    graphics_context_set_text_color(
        ctx,
        selected && enabled
            ? GColorWhite
            : GColorBlack
    );

    graphics_draw_text(
        ctx,
        label,
        fonts_get_system_font(
            FONT_KEY_GOTHIC_18_BOLD
        ),
        GRect(
            MENU_ROW_X,
            y + 1,
            MENU_ROW_W,
            MENU_ROW_H
        ),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter,
        NULL
    );
}

static void canvas_update_proc(
    Layer *layer,
    GContext *ctx
) {
    // Deep Space Background
    graphics_context_set_fill_color(
        ctx,
        GColorOxfordBlue
    );

    graphics_fill_rect(
        ctx,
        layer_get_bounds(layer),
        0,
        GCornerNone
    );

    // Procedural Stars
    if (
        s_current_state ==
            STATE_IN_GAME ||
        s_current_state ==
            STATE_LEVEL_COMPLETE ||
        s_current_state ==
            STATE_MENU ||
        s_current_state ==
            STATE_GAME_OVER
    ) {
        graphics_context_set_fill_color(
            ctx,
            GColorPictonBlue
        );

        for (
            int i = 0;
            i < STAR_COUNT;
            i++
        ) {
            int y_offset =
                (
                    s_current_state ==
                        STATE_IN_GAME ||
                    s_current_state ==
                        STATE_LEVEL_COMPLETE
                )
                    ? HUD_OFFSET
                    : 0;

            graphics_fill_rect(
                ctx,
                GRect(
                    s_stars[i].x,
                    s_stars[i].y +
                        y_offset,
                    1,
                    1
                ),
                0,
                GCornerNone
            );
        }
    }

    // Procedural 3D Marbles
    for (
        int i = 0;
        i < s_atom_count;
        i++
    ) {
        int y_offset =
            (
                s_current_state ==
                    STATE_IN_GAME ||
                s_current_state ==
                    STATE_LEVEL_COMPLETE
            )
                ? HUD_OFFSET
                : 0;

        int px =
            s_atoms[i].x;

        int py =
            s_atoms[i].y +
            y_offset;

        // Base Shadow/Outline
        graphics_context_set_fill_color(
            ctx,
            GColorDarkCandyAppleRed
        );

        graphics_fill_circle(
            ctx,
            GPoint(px, py),
            ATOM_RADIUS
        );

        // Mid-tone
        graphics_context_set_fill_color(
            ctx,
            GColorRed
        );

        graphics_fill_circle(
            ctx,
            GPoint(
                px - 1,
                py - 1
            ),
            ATOM_RADIUS - 1
        );

        // Bright core
        graphics_context_set_fill_color(
            ctx,
            GColorMelon
        );

        graphics_fill_circle(
            ctx,
            GPoint(
                px - 2,
                py - 2
            ),
            ATOM_RADIUS - 2
        );

        // Specular glint
        graphics_context_set_fill_color(
            ctx,
            GColorWhite
        );

        graphics_fill_circle(
            ctx,
            GPoint(
                px - 2,
                py - 2
            ),
            1
        );
    }

    // --- Main Menu ---

    if (
        s_current_state ==
        STATE_MENU
    ) {
        draw_text_with_shadow(
            ctx,
            "PEBBLE",
            FONT_KEY_BITHAM_30_BLACK,
            GRect(
                0,
                18,
                200,
                40
            ),
            GTextAlignmentCenter,
            GColorCeleste
        );

        draw_text_with_shadow(
            ctx,
            "BALL",
            FONT_KEY_BITHAM_30_BLACK,
            GRect(
                0,
                50,
                200,
                40
            ),
            GTextAlignmentCenter,
            GColorPictonBlue
        );

        draw_menu_row(
            ctx,
            MENU_ITEM_CONTINUE,
            MENU_CONTINUE_Y,
            "CONTINUE"
        );

        draw_menu_row(
            ctx,
            MENU_ITEM_NEW_GAME,
            MENU_NEW_GAME_Y,
            s_has_save
                ? "NEW GAME (RESET)"
                : "NEW GAME"
        );

        draw_menu_row(
            ctx,
            MENU_ITEM_EXIT,
            MENU_EXIT_Y,
            "EXIT"
        );

        char save_text[40];

        if (s_has_save) {
            snprintf(
                save_text,
                sizeof(save_text),
                "Saved: Lvl %d  Lives %d",
                s_current_level,
                s_lives
            );

        } else {
            snprintf(
                save_text,
                sizeof(save_text),
                "No saved game"
            );
        }

        graphics_context_set_text_color(
            ctx,
            GColorWhite
        );

        graphics_draw_text(
            ctx,
            save_text,
            fonts_get_system_font(
                FONT_KEY_GOTHIC_14
            ),
            GRect(
                0,
                204,
                200,
                20
            ),
            GTextOverflowModeTrailingEllipsis,
            GTextAlignmentCenter,
            NULL
        );

    // --- Game Over ---

    } else if (
        s_current_state ==
        STATE_GAME_OVER
    ) {
        graphics_context_set_fill_color(
            ctx,
            GColorWhite
        );

        graphics_fill_rect(
            ctx,
            GRect(
                20,
                60,
                160,
                100
            ),
            8,
            GCornersAll
        );

        graphics_context_set_stroke_color(
            ctx,
            GColorBlack
        );

        graphics_context_set_stroke_width(
            ctx,
            2
        );

        graphics_draw_rect(
            ctx,
            GRect(
                20,
                60,
                160,
                100
            )
        );

        draw_text_with_shadow(
            ctx,
            "GAME OVER",
            FONT_KEY_GOTHIC_24_BOLD,
            GRect(
                0,
                65,
                200,
                30
            ),
            GTextAlignmentCenter,
            GColorRed
        );

        char level_text[32];

        snprintf(
            level_text,
            sizeof(level_text),
            "Level Reached: %d",
            s_current_level
        );

        graphics_context_set_text_color(
            ctx,
            GColorBlack
        );

        graphics_draw_text(
            ctx,
            level_text,
            fonts_get_system_font(
                FONT_KEY_GOTHIC_18
            ),
            GRect(
                0,
                100,
                200,
                30
            ),
            GTextOverflowModeWordWrap,
            GTextAlignmentCenter,
            NULL
        );

        graphics_context_set_fill_color(
            ctx,
            GColorDarkGray
        );

        graphics_fill_rect(
            ctx,
            GRect(
                42,
                182,
                116,
                36
            ),
            8,
            GCornersAll
        );

        graphics_context_set_fill_color(
            ctx,
            GColorLightGray
        );

        graphics_fill_rect(
            ctx,
            GRect(
                40,
                180,
                116,
                36
            ),
            8,
            GCornersAll
        );

        graphics_context_set_text_color(
            ctx,
            GColorBlack
        );

        graphics_draw_text(
            ctx,
            "TAP/SEL TO RETRY",
            fonts_get_system_font(
                FONT_KEY_GOTHIC_14_BOLD
            ),
            GRect(
                0,
                188,
                200,
                30
            ),
            GTextOverflowModeWordWrap,
            GTextAlignmentCenter,
            NULL
        );

    // --- Gameplay / Level Complete ---

    } else if (
        s_current_state ==
            STATE_IN_GAME ||
        s_current_state ==
            STATE_LEVEL_COMPLETE
    ) {
        // HUD
        graphics_context_set_fill_color(
            ctx,
            GColorLightGray
        );

        graphics_fill_rect(
            ctx,
            GRect(
                0,
                0,
                PLAYBOX_WIDTH,
                HUD_OFFSET
            ),
            0,
            GCornerNone
        );

        char hud_text[32];

        int percent =
            (
                s_total_captured_area *
                100
            ) /
            TOTAL_AREA;

        snprintf(
            hud_text,
            sizeof(hud_text),
            "Lvl:%d  Lives:%d  Area:%d%%",
            s_current_level,
            s_lives,
            percent
        );

        graphics_context_set_text_color(
            ctx,
            GColorBlack
        );

        graphics_draw_text(
            ctx,
            hud_text,
            fonts_get_system_font(
                FONT_KEY_GOTHIC_18_BOLD
            ),
            GRect(
                0,
                0,
                PLAYBOX_WIDTH,
                HUD_OFFSET
            ),
            GTextOverflowModeWordWrap,
            GTextAlignmentCenter,
            NULL
        );

        graphics_context_set_stroke_color(
            ctx,
            GColorBlack
        );

        graphics_context_set_stroke_width(
            ctx,
            1
        );

        graphics_draw_rect(
            ctx,
            GRect(
                0,
                HUD_OFFSET,
                PLAYBOX_WIDTH,
                PLAYBOX_HEIGHT
            )
        );

        // Captured Areas (Neon Grid)
        graphics_context_set_stroke_color(
            ctx,
            GColorIndigo
        );

        graphics_context_set_stroke_width(
            ctx,
            1
        );

        for (
            int i = 0;
            i < s_captured_room_count;
            i++
        ) {
            GRect cap =
                s_captured_rooms[i];

            cap.origin.y +=
                HUD_OFFSET;

            graphics_context_set_fill_color(
                ctx,
                GColorDukeBlue
            );

            graphics_fill_rect(
                ctx,
                cap,
                0,
                GCornerNone
            );

            for (
                int x = cap.origin.x;
                x <
                    cap.origin.x +
                    cap.size.w;
                x += 10
            ) {
                graphics_draw_line(
                    ctx,
                    GPoint(
                        x,
                        cap.origin.y
                    ),
                    GPoint(
                        x,
                        cap.origin.y +
                            cap.size.h
                    )
                );
            }

            for (
                int y = cap.origin.y;
                y <
                    cap.origin.y +
                    cap.size.h;
                y += 10
            ) {
                graphics_draw_line(
                    ctx,
                    GPoint(
                        cap.origin.x,
                        y
                    ),
                    GPoint(
                        cap.origin.x +
                            cap.size.w,
                        y
                    )
                );
            }
        }

        // Built Walls (3D Bevels)
        for (
            int i = 0;
            i < s_built_wall_count;
            i++
        ) {
            GRect wall =
                s_built_walls[i];

            wall.origin.y +=
                HUD_OFFSET;

            GRect shadow =
                wall;

            shadow.origin.x += 2;
            shadow.origin.y += 2;

            graphics_context_set_fill_color(
                ctx,
                GColorDarkGray
            );

            graphics_fill_rect(
                ctx,
                shadow,
                0,
                GCornerNone
            );

            graphics_context_set_fill_color(
                ctx,
                GColorBlue
            );

            graphics_fill_rect(
                ctx,
                wall,
                0,
                GCornerNone
            );

            graphics_context_set_fill_color(
                ctx,
                GColorCeleste
            );

            graphics_fill_rect(
                ctx,
                GRect(
                    wall.origin.x,
                    wall.origin.y,
                    wall.size.w,
                    1
                ),
                0,
                GCornerNone
            );

            graphics_fill_rect(
                ctx,
                GRect(
                    wall.origin.x,
                    wall.origin.y,
                    1,
                    wall.size.h
                ),
                0,
                GCornerNone
            );

            graphics_context_set_fill_color(
                ctx,
                GColorOxfordBlue
            );

            graphics_fill_rect(
                ctx,
                GRect(
                    wall.origin.x,
                    wall.origin.y +
                        wall.size.h -
                        1,
                    wall.size.w,
                    1
                ),
                0,
                GCornerNone
            );

            graphics_fill_rect(
                ctx,
                GRect(
                    wall.origin.x +
                        wall.size.w -
                        1,
                    wall.origin.y,
                    1,
                    wall.size.h
                ),
                0,
                GCornerNone
            );
        }

        // Active building wall
        if (
            s_active_wall.state !=
            WALL_STATE_NONE
        ) {
            GRect active_bounds;

            if (
                s_active_wall.orientation ==
                ORIENTATION_HORIZONTAL
            ) {
                int width =
                    s_active_wall.ext_positive -
                    s_active_wall.ext_negative;

                active_bounds =
                    GRect(
                        s_active_wall.ext_negative,
                        s_active_wall.origin_y +
                            HUD_OFFSET,
                        width,
                        WALL_THICKNESS
                    );

            } else {
                int height =
                    s_active_wall.ext_positive -
                    s_active_wall.ext_negative;

                active_bounds =
                    GRect(
                        s_active_wall.origin_x,
                        s_active_wall.ext_negative +
                            HUD_OFFSET,
                        WALL_THICKNESS,
                        height
                    );
            }

            GRect shadow =
                active_bounds;

            shadow.origin.x += 2;
            shadow.origin.y += 2;

            graphics_context_set_fill_color(
                ctx,
                GColorDarkGray
            );

            graphics_fill_rect(
                ctx,
                shadow,
                0,
                GCornerNone
            );

            graphics_context_set_fill_color(
                ctx,
                GColorOrange
            );

            graphics_fill_rect(
                ctx,
                active_bounds,
                0,
                GCornerNone
            );
        }

        if (
            s_current_state ==
            STATE_IN_GAME
        ) {
            // Render D-Pad Cursor Indicator
            graphics_context_set_stroke_color(
                ctx,
                s_cursor_axis_x
                    ? GColorBlue
                    : GColorRed
            );

            graphics_context_set_stroke_width(
                ctx,
                2
            );

            graphics_draw_circle(
                ctx,
                GPoint(
                    s_cursor_pos.x,
                    s_cursor_pos.y +
                        HUD_OFFSET
                ),
                5
            );

            graphics_draw_line(
                ctx,
                GPoint(
                    s_cursor_pos.x - 4,
                    s_cursor_pos.y +
                        HUD_OFFSET
                ),
                GPoint(
                    s_cursor_pos.x + 4,
                    s_cursor_pos.y +
                        HUD_OFFSET
                )
            );

            graphics_draw_line(
                ctx,
                GPoint(
                    s_cursor_pos.x,
                    s_cursor_pos.y +
                        HUD_OFFSET -
                        4
                ),
                GPoint(
                    s_cursor_pos.x,
                    s_cursor_pos.y +
                        HUD_OFFSET +
                        4
                )
            );

        } else if (
            s_current_state ==
            STATE_LEVEL_COMPLETE
        ) {
            // Level Complete Overlay
            graphics_context_set_fill_color(
                ctx,
                GColorWhite
            );

            graphics_fill_rect(
                ctx,
                GRect(
                    20,
                    80,
                    160,
                    80
                ),
                8,
                GCornersAll
            );

            graphics_context_set_stroke_color(
                ctx,
                GColorBlack
            );

            graphics_context_set_stroke_width(
                ctx,
                2
            );

            graphics_draw_rect(
                ctx,
                GRect(
                    20,
                    80,
                    160,
                    80
                )
            );

            draw_text_with_shadow(
                ctx,
                "LEVEL CLEARED!",
                FONT_KEY_GOTHIC_24_BOLD,
                GRect(
                    0,
                    85,
                    200,
                    30
                ),
                GTextAlignmentCenter,
                GColorJaegerGreen
            );

            graphics_context_set_fill_color(
                ctx,
                GColorDarkGray
            );

            graphics_fill_rect(
                ctx,
                GRect(
                    42,
                    117,
                    116,
                    36
                ),
                8,
                GCornersAll
            );

            graphics_context_set_fill_color(
                ctx,
                GColorLightGray
            );

            graphics_fill_rect(
                ctx,
                GRect(
                    40,
                    115,
                    116,
                    36
                ),
                8,
                GCornersAll
            );

            graphics_context_set_text_color(
                ctx,
                GColorBlack
            );

            graphics_draw_text(
                ctx,
                "TAP/SEL NEXT",
                fonts_get_system_font(
                    FONT_KEY_GOTHIC_14_BOLD
                ),
                GRect(
                    0,
                    123,
                    200,
                    30
                ),
                GTextOverflowModeWordWrap,
                GTextAlignmentCenter,
                NULL
            );
        }
    }
}

// --- Lifecycle ---

static void main_window_load(
    Window *window
) {
    Layer *window_layer =
        window_get_root_layer(
            window
        );

    GRect bounds =
        layer_get_bounds(
            window_layer
        );

    s_canvas_layer =
        layer_create(
            bounds
        );

    layer_set_update_proc(
        s_canvas_layer,
        canvas_update_proc
    );

    layer_add_child(
        window_layer,
        s_canvas_layer
    );

    s_has_save =
        load_save_checkpoint();

    prepare_menu();

    transition_to_state(
        STATE_MENU
    );

    s_physics_timer =
        app_timer_register(
            TICK_RATE_MS,
            update_physics_tick,
            NULL
        );

#if ENABLE_TOUCH && defined(PBL_TOUCH)
    if (
        touch_service_is_enabled()
    ) {
        touch_service_subscribe(
            touch_handler,
            NULL
        );

        s_touch_subscribed =
            true;
    }
#endif
}

static void main_window_unload(
    Window *window
) {
    (void)window;

    if (s_physics_timer) {
        app_timer_cancel(
            s_physics_timer
        );

        s_physics_timer =
            NULL;
    }

#if ENABLE_TOUCH && defined(PBL_TOUCH)
    if (s_touch_subscribed) {
        touch_service_unsubscribe();

        s_touch_subscribed =
            false;
    }
#endif

    if (s_canvas_layer) {
        layer_destroy(
            s_canvas_layer
        );

        s_canvas_layer =
            NULL;
    }
}

static void init(void) {
    srand(
        (unsigned)time(NULL)
    );

    s_main_window =
        window_create();

    window_set_click_config_provider(
        s_main_window,
        click_config_provider
    );

    window_set_window_handlers(
        s_main_window,
        (WindowHandlers) {
            .load =
                main_window_load,

            .unload =
                main_window_unload
        }
    );

    window_stack_push(
        s_main_window,
        true
    );
}

static void deinit(void) {
    // Save only stable checkpoints.
    //
    // Active gameplay points to the current level start.
    // Level-clear points to the next level.
    if (
        s_current_state ==
        STATE_IN_GAME
    ) {
        write_save_checkpoint();

    } else if (
        s_current_state ==
        STATE_LEVEL_COMPLETE
    ) {
        write_save_checkpoint_for(
            s_current_level + 1,
            s_lives
        );
    }

    if (s_main_window) {
        window_destroy(
            s_main_window
        );

        s_main_window =
            NULL;
    }
}

int main(void) {
    init();

    app_event_loop();

    deinit();

    return 0;
}
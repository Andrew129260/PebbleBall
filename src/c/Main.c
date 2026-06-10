#include <pebble.h>

#define ATOM_RADIUS 4
#define PLAYBOX_WIDTH 200
#define PLAYBOX_HEIGHT 200
#define HUD_OFFSET 28
#define TICK_RATE_MS 33 
#define SWIPE_THRESHOLD 8 // Lowered for hyper-responsive touch
#define WALL_GROWTH_SPEED 6
#define WALL_THICKNESS 4
#define TOTAL_AREA (PLAYBOX_WIDTH * PLAYBOX_HEIGHT)
#define WIN_THRESHOLD 70 // Changed to 70% per your suggestion

#define ENABLE_TOUCH 1

// --- Structures & States ---

typedef enum {
    STATE_SPLASH,
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
    int x; int y;
    int dx; int dy;
} Atom;

typedef struct {
    WallState state;
    WallOrientation orientation;
    int room_index; 
    int origin_x; int origin_y;
    int ext_positive; 
    int ext_negative;
} ActiveWall;

typedef struct {
    bool active;
    GRect bounds;
} Room;

// --- Global App State ---

static Window *s_main_window;
static Layer *s_canvas_layer;
static AppTimer *s_physics_timer;

static GameState s_current_state = STATE_SPLASH;

static Atom s_atoms[50];
static int s_atom_count = 0;

static ActiveWall s_active_wall = { .state = WALL_STATE_NONE };

static Room s_rooms[100];
static int s_room_count = 0;

static GRect s_captured_rooms[100];
static int s_captured_room_count = 0;

static GRect s_built_walls[100];
static int s_built_wall_count = 0;

static int s_current_level = 1;
static int s_lives = 3;
static int s_total_captured_area = 0;

// Hybrid Button Cursor State
static GPoint s_cursor_pos = {100, 100};
static bool s_cursor_axis_x = true; 

#if ENABLE_TOUCH && defined(PBL_TOUCH)
static bool s_is_swiping = false;
static int16_t s_touch_start_x = 0;
static int16_t s_touch_start_y = 0;
#endif

// --- Helper Math ---

static bool point_in_rect(int px, int py, GRect r) {
    return (px >= r.origin.x && px <= r.origin.x + r.size.w &&
            py >= r.origin.y && py <= r.origin.y + r.size.h);
}

// --- Game Logic & State Transitions ---

static void set_background_atoms() {
    s_atom_count = 10;
    for(int i = 0; i < s_atom_count; i++) {
        s_atoms[i].x = PLAYBOX_WIDTH / 2;
        s_atoms[i].y = PLAYBOX_HEIGHT / 2;
        s_atoms[i].dx = (i % 2 == 0) ? 4 : -4;
        s_atoms[i].dy = (i % 3 == 0) ? 4 : -4;
    }
}

static void init_level(int level) {
    s_current_level = level;
    s_atom_count = s_current_level + 1;
    if (s_atom_count > 50) s_atom_count = 50; 

    int speed = 3 + (s_current_level / 3);
    if (speed > 6) speed = 6; 

    for(int i = 0; i < s_atom_count; i++) {
        s_atoms[i].x = PLAYBOX_WIDTH / 2;
        s_atoms[i].y = PLAYBOX_HEIGHT / 2;
        s_atoms[i].dx = (i % 2 == 0) ? speed : -speed;
        s_atoms[i].dy = (i % 3 == 0) ? speed : -speed;
    }

    s_rooms[0].active = true;
    s_rooms[0].bounds = GRect(0, 0, PLAYBOX_WIDTH, PLAYBOX_HEIGHT);
    s_room_count = 1;

    s_captured_room_count = 0;
    s_built_wall_count = 0;
    s_total_captured_area = 0;
    s_active_wall.state = WALL_STATE_NONE;
    
    s_cursor_pos.x = 100;
    s_cursor_pos.y = 100;
}

static void transition_to_state(GameState new_state) {
    s_current_state = new_state;
    
    if (new_state == STATE_SPLASH) {
        set_background_atoms();
    } else if (new_state == STATE_IN_GAME) {
        init_level(s_current_level); // Uses whatever level is currently set
    } else if (new_state == STATE_GAME_OVER) {
        set_background_atoms();
    } else if (new_state == STATE_LEVEL_COMPLETE) {
        // Keep the current board state visible, just change state
    }
    layer_mark_dirty(s_canvas_layer);
}

static void handle_life_lost() {
    vibes_double_pulse(); 
    s_lives--;
    if (s_lives <= 0) {
        transition_to_state(STATE_GAME_OVER);
    } else {
        init_level(s_current_level);
    }
}

static void split_room(int target_room, ActiveWall wall) {
    GRect r = s_rooms[target_room].bounds;
    s_rooms[target_room].active = false; 

    GRect r1, r2, wall_rect;
    if (wall.orientation == ORIENTATION_HORIZONTAL) {
        r1 = GRect(r.origin.x, r.origin.y, r.size.w, wall.origin_y - r.origin.y);
        r2 = GRect(r.origin.x, wall.origin_y + WALL_THICKNESS, r.size.w, (r.origin.y + r.size.h) - (wall.origin_y + WALL_THICKNESS));
        wall_rect = GRect(r.origin.x, wall.origin_y, r.size.w, WALL_THICKNESS);
    } else {
        r1 = GRect(r.origin.x, r.origin.y, wall.origin_x - r.origin.x, r.size.h);
        r2 = GRect(wall.origin_x + WALL_THICKNESS, r.origin.y, (r.origin.x + r.size.w) - (wall.origin_x + WALL_THICKNESS), r.size.h);
        wall_rect = GRect(wall.origin_x, r.origin.y, WALL_THICKNESS, r.size.h);
    }

    s_built_walls[s_built_wall_count++] = wall_rect;
    s_total_captured_area += (wall_rect.size.w * wall_rect.size.h);

    int atoms1 = 0, atoms2 = 0;
    for(int i = 0; i < s_atom_count; i++) {
        if (point_in_rect(s_atoms[i].x, s_atoms[i].y, r1)) atoms1++;
        else if (point_in_rect(s_atoms[i].x, s_atoms[i].y, r2)) atoms2++;
    }

    if (atoms1 == 0 && r1.size.w > 0 && r1.size.h > 0) {
        s_captured_rooms[s_captured_room_count++] = r1;
        s_total_captured_area += (r1.size.w * r1.size.h);
    } else if (r1.size.w > 0 && r1.size.h > 0) {
        s_rooms[s_room_count++] = (Room){.active = true, .bounds = r1};
    }

    if (atoms2 == 0 && r2.size.w > 0 && r2.size.h > 0) {
        s_captured_rooms[s_captured_room_count++] = r2;
        s_total_captured_area += (r2.size.w * r2.size.h);
    } else if (r2.size.w > 0 && r2.size.h > 0) {
        s_rooms[s_room_count++] = (Room){.active = true, .bounds = r2};
    }

    int percent = (s_total_captured_area * 100) / TOTAL_AREA;
    if (percent >= WIN_THRESHOLD) {
        vibes_short_pulse();
        transition_to_state(STATE_LEVEL_COMPLETE);
    }
}

// --- Wall Growth Logic ---

static void start_building_wall(int px, int raw_y, WallOrientation orientation) {
    if (s_active_wall.state != WALL_STATE_NONE) return; 

    int py = raw_y - HUD_OFFSET;

    int target = -1;
    for (int r = 0; r < s_room_count; r++) {
        if (s_rooms[r].active && point_in_rect(px, py, s_rooms[r].bounds)) {
            target = r;
            break;
        }
    }
    if (target == -1) return; 

    s_active_wall.state = WALL_STATE_BUILDING;
    s_active_wall.orientation = orientation;
    s_active_wall.room_index = target;
    s_active_wall.origin_x = px;
    s_active_wall.origin_y = py; 
    s_active_wall.ext_positive = (orientation == ORIENTATION_HORIZONTAL) ? px : py;
    s_active_wall.ext_negative = s_active_wall.ext_positive;
}

static void update_wall_growth() {
    if (s_active_wall.state != WALL_STATE_BUILDING) return;

    GRect room_bounds = s_rooms[s_active_wall.room_index].bounds;
    bool pos_hit = false;
    bool neg_hit = false;

    if (s_active_wall.orientation == ORIENTATION_HORIZONTAL) {
        s_active_wall.ext_positive += WALL_GROWTH_SPEED;
        if (s_active_wall.ext_positive >= room_bounds.origin.x + room_bounds.size.w) {
            s_active_wall.ext_positive = room_bounds.origin.x + room_bounds.size.w;
            pos_hit = true;
        }
        s_active_wall.ext_negative -= WALL_GROWTH_SPEED;
        if (s_active_wall.ext_negative <= room_bounds.origin.x) {
            s_active_wall.ext_negative = room_bounds.origin.x;
            neg_hit = true;
        }
    } else {
        s_active_wall.ext_positive += WALL_GROWTH_SPEED;
        if (s_active_wall.ext_positive >= room_bounds.origin.y + room_bounds.size.h) {
            s_active_wall.ext_positive = room_bounds.origin.y + room_bounds.size.h;
            pos_hit = true;
        }
        s_active_wall.ext_negative -= WALL_GROWTH_SPEED;
        if (s_active_wall.ext_negative <= room_bounds.origin.y) {
            s_active_wall.ext_negative = room_bounds.origin.y;
            neg_hit = true;
        }
    }

    if (pos_hit && neg_hit) {
        split_room(s_active_wall.room_index, s_active_wall);
        s_active_wall.state = WALL_STATE_NONE;
    }
}

// --- Menu & D-Pad Controls ---

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_current_state == STATE_SPLASH || s_current_state == STATE_GAME_OVER) {
        s_current_level = 1;
        s_lives = 3;
        transition_to_state(STATE_IN_GAME);
    } else if (s_current_state == STATE_LEVEL_COMPLETE) {
        s_current_level++;
        transition_to_state(STATE_IN_GAME);
    } else if (s_current_state == STATE_IN_GAME) {
        s_cursor_axis_x = !s_cursor_axis_x;
        layer_mark_dirty(s_canvas_layer);
    }
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_current_state != STATE_IN_GAME) return;
    if (s_cursor_axis_x) {
        s_cursor_pos.x -= 10;
        if (s_cursor_pos.x < 0) s_cursor_pos.x = 0;
    } else {
        s_cursor_pos.y -= 10;
        if (s_cursor_pos.y < 0) s_cursor_pos.y = 0;
    }
    layer_mark_dirty(s_canvas_layer);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_current_state != STATE_IN_GAME) return;
    if (s_cursor_axis_x) {
        s_cursor_pos.x += 10;
        if (s_cursor_pos.x > PLAYBOX_WIDTH) s_cursor_pos.x = PLAYBOX_WIDTH;
    } else {
        s_cursor_pos.y += 10;
        if (s_cursor_pos.y > PLAYBOX_HEIGHT) s_cursor_pos.y = PLAYBOX_HEIGHT;
    }
    layer_mark_dirty(s_canvas_layer);
}

static void up_long_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_current_state == STATE_IN_GAME) {
        start_building_wall(s_cursor_pos.x, s_cursor_pos.y + HUD_OFFSET, ORIENTATION_VERTICAL);
    }
}

static void down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_current_state == STATE_IN_GAME) {
        start_building_wall(s_cursor_pos.x, s_cursor_pos.y + HUD_OFFSET, ORIENTATION_HORIZONTAL);
    }
}

static void click_config_provider(void *context) {
    window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
    window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
    window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
    
    // Hold Up for Vertical, Hold Down for Horizontal
    window_long_click_subscribe(BUTTON_ID_UP, 500, up_long_click_handler, NULL);
    window_long_click_subscribe(BUTTON_ID_DOWN, 500, down_long_click_handler, NULL);
}

// --- Unified Touch Input Handler ---

#if ENABLE_TOUCH && defined(PBL_TOUCH)
static void touch_handler(const TouchEvent *event, void *context) {
    (void)context;
    
    int pos_x = event->x;
    int pos_y = event->y;
    int event_type = event->type; 

    if (event_type == TouchEvent_Touchdown) {
        s_touch_start_x = pos_x;
        s_touch_start_y = pos_y;
        s_is_swiping = true;
        
        // Sync the D-Pad cursor to where they touched for hybrid play
        if (s_current_state == STATE_IN_GAME) {
            s_cursor_pos.x = pos_x;
            s_cursor_pos.y = pos_y - HUD_OFFSET;
        }
    } 
    else if (event_type == TouchEvent_PositionUpdate) {
        if (!s_is_swiping) return;
        
        if (s_current_state == STATE_IN_GAME) {
            int16_t dx = pos_x - s_touch_start_x;
            int16_t dy = pos_y - s_touch_start_y;
            int16_t adx = dx < 0 ? -dx : dx;
            int16_t ady = dy < 0 ? -dy : dy;
            
            if (adx > SWIPE_THRESHOLD || ady > SWIPE_THRESHOLD) {
                if (adx > ady) {
                    start_building_wall(s_touch_start_x, s_touch_start_y, ORIENTATION_HORIZONTAL);
                } else {
                    start_building_wall(s_touch_start_x, s_touch_start_y, ORIENTATION_VERTICAL);
                }
                s_is_swiping = false; 
            }
        }
    }
    else if (event_type == TouchEvent_Liftoff) {
        if (!s_is_swiping) return;
        s_is_swiping = false;
        
        int16_t dx = pos_x - s_touch_start_x;
        int16_t dy = pos_y - s_touch_start_y;
        int16_t adx = dx < 0 ? -dx : dx;
        int16_t ady = dy < 0 ? -dy : dy;
        
        if (adx < 15 && ady < 15) {
            if (s_current_state == STATE_SPLASH || s_current_state == STATE_GAME_OVER) {
                s_current_level = 1;
                s_lives = 3;
                transition_to_state(STATE_IN_GAME);
            } else if (s_current_state == STATE_LEVEL_COMPLETE) {
                s_current_level++;
                transition_to_state(STATE_IN_GAME);
            }
        }
    }
}
#endif

// --- Physics Engine ---

static bool check_active_wall_penalty(Atom *atom) {
    if (s_active_wall.state != WALL_STATE_BUILDING) return false;

    if (s_active_wall.orientation == ORIENTATION_HORIZONTAL) {
        if (abs(atom->y - s_active_wall.origin_y) <= ATOM_RADIUS + (WALL_THICKNESS/2)) {
            if (atom->x >= s_active_wall.ext_negative && atom->x <= s_active_wall.ext_positive) return true;
        }
    } else {
        if (abs(atom->x - s_active_wall.origin_x) <= ATOM_RADIUS + (WALL_THICKNESS/2)) {
            if (atom->y >= s_active_wall.ext_negative && atom->y <= s_active_wall.ext_positive) return true;
        }
    }
    return false;
}

static void update_physics_tick(void *context) {
    
    if (s_current_state == STATE_IN_GAME) {
        update_wall_growth();
    }

    for (int i = 0; i < s_atom_count; i++) {
        
        if (s_current_state == STATE_IN_GAME && check_active_wall_penalty(&s_atoms[i])) {
            handle_life_lost();
            if (s_current_state == STATE_IN_GAME) layer_mark_dirty(s_canvas_layer);
            s_physics_timer = app_timer_register(TICK_RATE_MS, update_physics_tick, NULL);
            return; 
        }

        GRect bounds = GRect(0, (s_current_state == STATE_IN_GAME || s_current_state == STATE_LEVEL_COMPLETE) ? HUD_OFFSET : 0, PLAYBOX_WIDTH, (s_current_state == STATE_IN_GAME || s_current_state == STATE_LEVEL_COMPLETE) ? PLAYBOX_HEIGHT : 228);
        
        if (s_current_state == STATE_IN_GAME || s_current_state == STATE_LEVEL_COMPLETE) {
            for(int r = 0; r < s_room_count; r++) {
                if (s_rooms[r].active && point_in_rect(s_atoms[i].x, s_atoms[i].y, s_rooms[r].bounds)) {
                    bounds = s_rooms[r].bounds;
                    break;
                }
            }
        }

        int next_x = s_atoms[i].x + s_atoms[i].dx;
        int next_y = s_atoms[i].y + s_atoms[i].dy;
        
        if (next_x - ATOM_RADIUS <= bounds.origin.x || next_x + ATOM_RADIUS >= bounds.origin.x + bounds.size.w) {
            s_atoms[i].dx = -s_atoms[i].dx;
            next_x = s_atoms[i].x + s_atoms[i].dx;
        }
        if (next_y - ATOM_RADIUS <= bounds.origin.y || next_y + ATOM_RADIUS >= bounds.origin.y + bounds.size.h) {
            s_atoms[i].dy = -s_atoms[i].dy;
            next_y = s_atoms[i].y + s_atoms[i].dy;
        }
        
        s_atoms[i].x = next_x;
        s_atoms[i].y = next_y;
    }
    
    layer_mark_dirty(s_canvas_layer);
    s_physics_timer = app_timer_register(TICK_RATE_MS, update_physics_tick, NULL);
}

// --- Canvas Drawing (State Managed) ---

static void draw_text_with_shadow(GContext *ctx, const char *text, const char *font_key, GRect box, GTextAlignment align, GColor text_color) {
    GRect shadow_box = box;
    shadow_box.origin.x += 2;
    shadow_box.origin.y += 2;
    
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, text, fonts_get_system_font(font_key), shadow_box, GTextOverflowModeWordWrap, align, NULL);
    
    graphics_context_set_text_color(ctx, text_color);
    graphics_draw_text(ctx, text, fonts_get_system_font(font_key), box, GTextOverflowModeWordWrap, align, NULL);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

    graphics_context_set_fill_color(ctx, GColorRed);
    for (int i = 0; i < s_atom_count; i++) {
        int y_offset = (s_current_state == STATE_IN_GAME || s_current_state == STATE_LEVEL_COMPLETE) ? HUD_OFFSET : 0;
        graphics_fill_circle(ctx, GPoint(s_atoms[i].x, s_atoms[i].y + y_offset), ATOM_RADIUS);
    }

    if (s_current_state == STATE_SPLASH) {
        draw_text_with_shadow(ctx, "PEBBLE", FONT_KEY_BITHAM_30_BLACK, GRect(0, 50, 200, 40), GTextAlignmentCenter, GColorCobaltBlue);
        draw_text_with_shadow(ctx, "BALL", FONT_KEY_BITHAM_30_BLACK, GRect(0, 85, 200, 40), GTextAlignmentCenter, GColorImperialPurple);
        
        graphics_context_set_fill_color(ctx, GColorDarkGray);
        graphics_fill_rect(ctx, GRect(42, 162, 116, 36), 8, GCornersAll); 
        graphics_context_set_fill_color(ctx, GColorLightGray);
        graphics_fill_rect(ctx, GRect(40, 160, 116, 36), 8, GCornersAll); 
        
        graphics_context_set_text_color(ctx, GColorBlack);
        graphics_draw_text(ctx, "TAP TO PLAY", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), GRect(0, 166, 200, 30), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    } else if (s_current_state == STATE_GAME_OVER) {
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_rect(ctx, GRect(20, 60, 160, 100), 8, GCornersAll);
        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 2);
        graphics_draw_rect(ctx, GRect(20, 60, 160, 100));

        draw_text_with_shadow(ctx, "GAME OVER", FONT_KEY_GOTHIC_24_BOLD, GRect(0, 65, 200, 30), GTextAlignmentCenter, GColorRed);
        
        char level_text[32];
        snprintf(level_text, sizeof(level_text), "Level Reached: %d", s_current_level);
        graphics_context_set_text_color(ctx, GColorBlack);
        graphics_draw_text(ctx, level_text, fonts_get_system_font(FONT_KEY_GOTHIC_18), GRect(0, 100, 200, 30), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

        graphics_context_set_fill_color(ctx, GColorDarkGray);
        graphics_fill_rect(ctx, GRect(42, 182, 116, 36), 8, GCornersAll); 
        graphics_context_set_fill_color(ctx, GColorLightGray);
        graphics_fill_rect(ctx, GRect(40, 180, 116, 36), 8, GCornersAll); 
        graphics_draw_text(ctx, "TAP TO RETRY", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), GRect(0, 186, 200, 30), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    } else if (s_current_state == STATE_IN_GAME || s_current_state == STATE_LEVEL_COMPLETE) {
        graphics_context_set_fill_color(ctx, GColorLightGray);
        graphics_fill_rect(ctx, GRect(0, 0, PLAYBOX_WIDTH, HUD_OFFSET), 0, GCornerNone);
        
        char hud_text[32];
        int percent = (s_total_captured_area * 100) / TOTAL_AREA;
        snprintf(hud_text, sizeof(hud_text), "Lvl:%d  Lives:%d  Area:%d%%", s_current_level, s_lives, percent);
        graphics_context_set_text_color(ctx, GColorBlack);
        graphics_draw_text(ctx, hud_text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), GRect(0, 0, PLAYBOX_WIDTH, HUD_OFFSET), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

        graphics_context_set_stroke_color(ctx, GColorBlack);
        graphics_context_set_stroke_width(ctx, 1);
        graphics_draw_rect(ctx, GRect(0, HUD_OFFSET, PLAYBOX_WIDTH, PLAYBOX_HEIGHT));

        graphics_context_set_fill_color(ctx, GColorImperialPurple);
        for (int i = 0; i < s_captured_room_count; i++) {
            GRect cap = s_captured_rooms[i];
            cap.origin.y += HUD_OFFSET;
            graphics_fill_rect(ctx, cap, 0, GCornerNone);
        }

        for (int i = 0; i < s_built_wall_count; i++) {
            GRect wall = s_built_walls[i];
            wall.origin.y += HUD_OFFSET;

            GRect shadow = wall; shadow.origin.x += 2; shadow.origin.y += 2;
            graphics_context_set_fill_color(ctx, GColorDarkGray);
            graphics_fill_rect(ctx, shadow, 0, GCornerNone);
            
            graphics_context_set_fill_color(ctx, GColorCobaltBlue);
            graphics_fill_rect(ctx, wall, 0, GCornerNone);
        }

        if (s_active_wall.state != WALL_STATE_NONE) {
            GRect active_bounds;
            if (s_active_wall.orientation == ORIENTATION_HORIZONTAL) {
                int width = s_active_wall.ext_positive - s_active_wall.ext_negative;
                active_bounds = GRect(s_active_wall.ext_negative, s_active_wall.origin_y + HUD_OFFSET, width, WALL_THICKNESS);
            } else {
                int height = s_active_wall.ext_positive - s_active_wall.ext_negative;
                active_bounds = GRect(s_active_wall.origin_x, s_active_wall.ext_negative + HUD_OFFSET, WALL_THICKNESS, height);
            }

            GRect shadow = active_bounds; shadow.origin.x += 2; shadow.origin.y += 2;
            graphics_context_set_fill_color(ctx, GColorDarkGray);
            graphics_fill_rect(ctx, shadow, 0, GCornerNone);

            graphics_context_set_fill_color(ctx, GColorOrange);
            graphics_fill_rect(ctx, active_bounds, 0, GCornerNone);
        }

        if (s_current_state == STATE_IN_GAME) {
            // Render D-Pad Cursor Indicator
            graphics_context_set_stroke_color(ctx, s_cursor_axis_x ? GColorBlue : GColorRed);
            graphics_context_set_stroke_width(ctx, 2);
            graphics_draw_circle(ctx, GPoint(s_cursor_pos.x, s_cursor_pos.y + HUD_OFFSET), 5);
            graphics_draw_line(ctx, GPoint(s_cursor_pos.x - 4, s_cursor_pos.y + HUD_OFFSET), GPoint(s_cursor_pos.x + 4, s_cursor_pos.y + HUD_OFFSET));
            graphics_draw_line(ctx, GPoint(s_cursor_pos.x, s_cursor_pos.y + HUD_OFFSET - 4), GPoint(s_cursor_pos.x, s_cursor_pos.y + HUD_OFFSET + 4));
        } else if (s_current_state == STATE_LEVEL_COMPLETE) {
            // Level Complete Overlay
            graphics_context_set_fill_color(ctx, GColorWhite);
            graphics_fill_rect(ctx, GRect(20, 80, 160, 80), 8, GCornersAll);
            graphics_context_set_stroke_color(ctx, GColorBlack);
            graphics_context_set_stroke_width(ctx, 2);
            graphics_draw_rect(ctx, GRect(20, 80, 160, 80));

            draw_text_with_shadow(ctx, "LEVEL CLEARED!", FONT_KEY_GOTHIC_24_BOLD, GRect(0, 85, 200, 30), GTextAlignmentCenter, GColorJaegerGreen);
            
            graphics_context_set_fill_color(ctx, GColorDarkGray);
            graphics_fill_rect(ctx, GRect(42, 117, 116, 36), 8, GCornersAll); 
            graphics_context_set_fill_color(ctx, GColorLightGray);
            graphics_fill_rect(ctx, GRect(40, 115, 116, 36), 8, GCornersAll); 
            graphics_draw_text(ctx, "TAP NEXT", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), GRect(0, 121, 200, 30), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
        }
    }
}

// --- Lifecycle ---

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    transition_to_state(STATE_SPLASH);
    s_physics_timer = app_timer_register(TICK_RATE_MS, update_physics_tick, NULL);
    
#if ENABLE_TOUCH && defined(PBL_TOUCH)
    if (touch_service_is_enabled()) {
        touch_service_subscribe(touch_handler, NULL);
    }
#endif
}

static void main_window_unload(Window *window) {
    layer_destroy(s_canvas_layer);
    if (s_physics_timer) app_timer_cancel(s_physics_timer);
    
#if ENABLE_TOUCH && defined(PBL_TOUCH)
    touch_service_unsubscribe();
#endif
}

static void init() {
    s_main_window = window_create();
    window_set_click_config_provider(s_main_window, click_config_provider);
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload,
    });
    window_stack_push(s_main_window, true);
}

static void deinit() {
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}

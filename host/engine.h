// =============================================================================
// Eyescire Engine - public C ABI shared between editor.exe and script DLLs.
//
// The engine state (window, GDI back buffer, input, entity store, console,
// scripting context) lives in engine.dll. Editor.exe links engine.lib at
// build time; user scripts (scripts/<name>.nxa compiled to bin/<name>.dll
// via NexaC --dll) link the same engine.lib so they call the same API.
//
// All function names are stable C symbols; Nexa wrappers in engine/engine.nxa
// forward into them via extern "C" inline_cpp! declarations.
// =============================================================================

#pragma once

#if defined(_WIN32)
  #if defined(EYESCIRE_BUILDING)
    #define EYESCIRE_API __declspec(dllexport)
  #else
    #define EYESCIRE_API __declspec(dllimport)
  #endif
#else
  #define EYESCIRE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/*  Lifecycle / window                                                      */
/* ======================================================================== */

EYESCIRE_API int  engine_init(int w, int h, const char* title);
EYESCIRE_API void engine_shutdown();
EYESCIRE_API void engine_quit();
EYESCIRE_API int  engine_pump();
EYESCIRE_API void engine_present();
EYESCIRE_API int  engine_vsync();          /* 1 = wait for the monitor */
EYESCIRE_API int  engine_aa();             /* 1 = smooth 3D edges */
EYESCIRE_API int  engine_window_w();
EYESCIRE_API int  engine_window_h();
EYESCIRE_API void engine_set_title(const char* title);
EYESCIRE_API int  engine_resize(int w, int h);
EYESCIRE_API void engine_set_fullscreen(int on);
EYESCIRE_API int  engine_fullscreen();
EYESCIRE_API void engine_set_letterbox(int on);   /* 0 = stretch fill, 1 = aspect-fit black bars */
EYESCIRE_API int  engine_letterbox();
EYESCIRE_API void engine_enable_letterbox();
EYESCIRE_API void engine_disable_letterbox();

/* ======================================================================== */
/*  Music (Windows MCI — WAV/MP3)                                           */
/* ======================================================================== */

EYESCIRE_API int  engine_music_play(const char* path, int loop);
EYESCIRE_API void engine_music_stop();
EYESCIRE_API int  engine_music_playing();
EYESCIRE_API void engine_music_set_volume(int percent); /* 0..100 */
EYESCIRE_API int  engine_music_volume();
EYESCIRE_API void engine_sfx_set_volume(int percent); /* 0..100 */
EYESCIRE_API int  engine_sfx_volume();
EYESCIRE_API int  engine_sfx_play(const char* path); /* one-shot, centered */
EYESCIRE_API int  engine_sfx_play_at(const char* path, float x, float y, float z);
EYESCIRE_API void engine_sfx_set_live(int on); /* 1 while a game is playing */
EYESCIRE_API void engine_set_autosave(int on);
EYESCIRE_API int  engine_autosave();

EYESCIRE_API int  engine_settings_load();
EYESCIRE_API void engine_settings_save(int res, int fullscreen, int music, int sfx,
                                       int parallax, int autosave, int uiscale);
EYESCIRE_API int  engine_settings_res();
EYESCIRE_API int  engine_settings_fullscreen();
EYESCIRE_API int  engine_settings_music();
EYESCIRE_API int  engine_settings_sfx();
EYESCIRE_API int  engine_settings_parallax();
EYESCIRE_API int  engine_settings_autosave();
EYESCIRE_API int  engine_settings_uiscale();

EYESCIRE_API void engine_worlds_refresh();
EYESCIRE_API int  engine_worlds_count();
EYESCIRE_API const char* engine_worlds_name(int i);
EYESCIRE_API const char* engine_worlds_path(int i);
EYESCIRE_API int  engine_worlds_create();
EYESCIRE_API int  engine_worlds_create_from_edit(int size_idx, int diff_idx, int evil_idx);
EYESCIRE_API int  engine_worlds_delete(int i);

/* Incremental world generation (Terraria-style creating screen). */
EYESCIRE_API void        engine_world_gen_begin(int size_idx, int theme_idx);
EYESCIRE_API int         engine_world_gen_step();          /* 0 = working, 1 = ready, -1 = fail */
EYESCIRE_API int         engine_world_gen_active();
EYESCIRE_API int         engine_world_gen_progress();      /* 0..100 */
EYESCIRE_API const char* engine_world_gen_stage();
EYESCIRE_API const char* engine_world_gen_tip();
EYESCIRE_API void        engine_world_gen_draw_preview(int x, int y, int w, int h);
EYESCIRE_API int         engine_worlds_finish_gen(int size_idx, int diff_idx, int theme_idx);
/* Re-declare the generator's tile palette. engine_tilemap_load restores
   cells and sky but not the tile table, so a game loading a saved world
   calls this instead of hardcoding the colours itself. */
EYESCIRE_API void        engine_world_tiles_define();

EYESCIRE_API void engine_players_refresh();
EYESCIRE_API int  engine_players_count();
EYESCIRE_API const char* engine_players_name(int i);
EYESCIRE_API const char* engine_players_path(int i);
EYESCIRE_API int  engine_players_create();
EYESCIRE_API int  engine_players_create_from_edit();
EYESCIRE_API int  engine_players_delete(int i);
EYESCIRE_API int  engine_players_look(int i, int slot);
EYESCIRE_API int  engine_players_look_rgb(int i, int slot);
EYESCIRE_API void engine_edit_set_look(int hair, int shirt, int pants, int hat, int acc);
EYESCIRE_API void engine_edit_set_look_rgb(int skin_rgb, int hair_rgb, int shirt_rgb, int pants_rgb, int hat_rgb, int acc_rgb, int undershirt_rgb, int shoes_rgb);

EYESCIRE_API void engine_edit_clear();
EYESCIRE_API void engine_edit_set(const char* s);
EYESCIRE_API const char* engine_edit_text();
EYESCIRE_API void engine_edit_clear_seed();
EYESCIRE_API const char* engine_edit_seed_text();
EYESCIRE_API void engine_edit_focus(int slot);
EYESCIRE_API void engine_edit_active(int on);
EYESCIRE_API void engine_edit_poll();
EYESCIRE_API int  engine_edit_text_width();

/* ======================================================================== */
/*  Drawing                                                                 */
/* ======================================================================== */

EYESCIRE_API void engine_clear(int r, int g, int b);
EYESCIRE_API void engine_fill_rect(int x, int y, int w, int h, int r, int g, int b);
EYESCIRE_API void engine_fill_rect_a(int x, int y, int w, int h, int r, int g, int b, int a);
EYESCIRE_API void engine_fill_round_a(int x, int y, int w, int h, int radius, int r, int g, int b, int a);
EYESCIRE_API void engine_outline_round_a(int x, int y, int w, int h, int radius, int thickness, int r, int g, int b, int a);
EYESCIRE_API void engine_fill_rect_rot(int x, int y, int w, int h, int r, int g, int b, int deg);
EYESCIRE_API void engine_fill_rect_rot_a(int x, int y, int w, int h, int r, int g, int b, int a, int deg);
EYESCIRE_API int  engine_rgb_hue(int r, int g, int b);
EYESCIRE_API int  engine_rgb_sat(int r, int g, int b);
EYESCIRE_API int  engine_rgb_val(int r, int g, int b);
EYESCIRE_API int  engine_hsv_r(int h, int s, int v);
EYESCIRE_API int  engine_hsv_g(int h, int s, int v);
EYESCIRE_API int  engine_hsv_b(int h, int s, int v);
EYESCIRE_API void engine_picker_sv(int x, int y, int w, int h, int hue);
EYESCIRE_API void engine_picker_hue(int x, int y, int w, int h);
EYESCIRE_API void engine_picker_alpha(int x, int y, int w, int h, int r, int g, int b);
EYESCIRE_API void engine_outline_rect(int x, int y, int w, int h, int thickness, int r, int g, int b);
EYESCIRE_API void engine_line(int x0, int y0, int x1, int y1, int r, int g, int b);
EYESCIRE_API void engine_pixel(int x, int y, int r, int g, int b);
EYESCIRE_API void engine_text(int x, int y, const char* s, int r, int g, int b);
/* 5x7 text, the same glyphs and spacing as std/gfx text (the 3D player's). */
EYESCIRE_API void engine_text_px(int x, int y, const char* s, int r, int g, int b, int scale);
EYESCIRE_API void engine_text_aa(int x, int y, const char* s, int r, int g, int b, int a);
EYESCIRE_API int  engine_text_width(const char* s);
EYESCIRE_API int  engine_text_height();
EYESCIRE_API int  engine_text_baseline_y(int rect_y, int rect_h);
EYESCIRE_API void engine_set_font_size(int px);
EYESCIRE_API void engine_set_font(const char* face, int px);
EYESCIRE_API void engine_set_font_bold(int on);
EYESCIRE_API void engine_text_file(int x, int y, const char* s, int r, int g, int b, int px, const char* path);
EYESCIRE_API int  engine_text_file_width(const char* s, int px, const char* path);
EYESCIRE_API void engine_set_clip(int x, int y, int w, int h);
EYESCIRE_API void engine_clear_clip();

EYESCIRE_API int  engine_image_load(const char* path);
EYESCIRE_API int  engine_image_load_cached(const char* path);
EYESCIRE_API void engine_image_cache_clear();
EYESCIRE_API int  engine_image_w(int id);
EYESCIRE_API int  engine_image_h(int id);
EYESCIRE_API void engine_image_blit(int id, int x, int y);
EYESCIRE_API void engine_image_blit_scaled(int id, int x, int y, int w, int h);
EYESCIRE_API void engine_image_blit_scaled_tint(int id, int x, int y, int w, int h, int r, int g, int b);
EYESCIRE_API void engine_image_blit_scaled_nn(int id, int x, int y, int w, int h);
EYESCIRE_API void engine_image_blit_sub_nn(int id, int src_x, int src_y, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h);
EYESCIRE_API void engine_image_blit_sub_nn_tint(int id, int src_x, int src_y, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h, int r, int g, int b);
EYESCIRE_API void engine_image_blit_pan_nn(int id, double scroll_x, int view_w, int view_h);
EYESCIRE_API void engine_image_blit_scaled_rot(int id, int x, int y, int w, int h, int deg);
EYESCIRE_API void engine_image_blit_scaled_rot_a(int id, int x, int y, int w, int h, int deg, int a);
EYESCIRE_API void engine_image_free(int id);

/* ======================================================================== */
/*  Input                                                                   */
/* ======================================================================== */

EYESCIRE_API int engine_mouse_x();
EYESCIRE_API int engine_mouse_y();
/* Canvas-to-window map, set whenever the UI layout runs. Scales are thousandths. */
EYESCIRE_API void engine_ui_set_map(int ox, int oy, int sx_milli, int sy_milli);
EYESCIRE_API int engine_ui_screen_x(int x);
EYESCIRE_API int engine_ui_screen_y(int y);
EYESCIRE_API int engine_ui_screen_w(int w);
EYESCIRE_API int engine_ui_screen_h(int h);
/* A picture on the 1280x720 canvas, over the 3D view. x/y/w/h are canvas pixels. */
EYESCIRE_API void engine_draw_sprite(int id, int x, int y, int w, int h, int deg, int a);
EYESCIRE_API int engine_ui_hover(int x, int y, int w, int h);
EYESCIRE_API int engine_mouse_down(int btn);
EYESCIRE_API int engine_mouse_pressed(int btn);
EYESCIRE_API int engine_mouse_released(int btn);
EYESCIRE_API int engine_mouse_wheel();
EYESCIRE_API int engine_key_down(int vk);
EYESCIRE_API int engine_key_pressed(int vk);
EYESCIRE_API int engine_key_released(int vk);
EYESCIRE_API int engine_typed_pop();

/* ======================================================================== */
/*  Time                                                                    */
/* ======================================================================== */

EYESCIRE_API double engine_now_ms();
EYESCIRE_API void   engine_sleep_ms(int ms);
EYESCIRE_API double engine_dt_seconds();
EYESCIRE_API void   engine_set_dt_seconds(double dt);  /* called by editor each frame */

/* ======================================================================== */
/*  Entity store                                                            */
/* ======================================================================== */

EYESCIRE_API int  engine_entity_max();
EYESCIRE_API int  engine_entity_count();
EYESCIRE_API int  engine_entity_active(int slot);
EYESCIRE_API int  engine_entity_id(int slot);

EYESCIRE_API int  engine_entity_x(int slot);
EYESCIRE_API int  engine_entity_y(int slot);
EYESCIRE_API int  engine_entity_w(int slot);
EYESCIRE_API int  engine_entity_h(int slot);
EYESCIRE_API int  engine_entity_rot(int slot);
EYESCIRE_API int  engine_entity_rot_x(int slot);
EYESCIRE_API int  engine_entity_rot_y(int slot);
EYESCIRE_API void  engine_entity_set_look(int slot, float yaw, float pitch);
EYESCIRE_API float engine_entity_look_yaw(int slot);
EYESCIRE_API float engine_entity_look_pitch(int slot);
EYESCIRE_API int  engine_entity_is_camera(int slot);
EYESCIRE_API int  engine_find_main_camera();
EYESCIRE_API int  engine_entity_color_r(int slot);
EYESCIRE_API int  engine_entity_color_g(int slot);
EYESCIRE_API int  engine_entity_color_b(int slot);
EYESCIRE_API int  engine_entity_color_a(int slot);
EYESCIRE_API const char* engine_entity_name(int slot);
EYESCIRE_API const char* engine_entity_kind(int slot);
EYESCIRE_API const char* engine_entity_text(int slot);
EYESCIRE_API const char* engine_entity_sprite(int slot);
EYESCIRE_API const char* engine_entity_script(int slot);
EYESCIRE_API int  engine_entity_sprite_id(int slot);

EYESCIRE_API void engine_entity_set_x(int slot, int x);
EYESCIRE_API void engine_entity_set_y(int slot, int y);
EYESCIRE_API void engine_entity_set_w(int slot, int w);
EYESCIRE_API void engine_entity_set_h(int slot, int h);
EYESCIRE_API void engine_entity_set_rot(int slot, int deg);
EYESCIRE_API void engine_entity_set_rot_x(int slot, int deg);
EYESCIRE_API void engine_entity_set_rot_y(int slot, int deg);
/* A 3D scale or rotate drag. begin, add each root, then scale or spin
   from those captured values. axis: 0 all, 1 x, 2 y, 3 z. */
EYESCIRE_API void engine_xform_grab_begin(void);
EYESCIRE_API void engine_xform_grab_add(int slot);
EYESCIRE_API void engine_xform_grab_scale(double factor, int axis);
EYESCIRE_API void engine_xform_grab_spin(int axis, int delta_deg);
/* local 1 turns around the object's own axis. local 0 turns around the scene axis. */
EYESCIRE_API void engine_xform_grab_spin_local(int axis, int delta_deg, int local);
/* The object's orientation applied to a vector. Read the result with engine_orient_x/y/z. */
EYESCIRE_API void engine_orient_vec(int slot, float x, float y, float z);
EYESCIRE_API float engine_orient_x();
EYESCIRE_API float engine_orient_y();
EYESCIRE_API float engine_orient_z();
EYESCIRE_API void engine_entity_set_camera(int slot, int on);
EYESCIRE_API void engine_entity_set_color(int slot, int r, int g, int b);
EYESCIRE_API void engine_entity_set_alpha(int slot, int a);
/* Button tints. which: 0 hover, 1 pressed, 2 toggled on, 3 toggled and hovered.
   channel: 0 r, 1 g, 2 b. custom 0 derives the tint from the base color. */
EYESCIRE_API int  engine_entity_button_color(int slot, int which, int channel);
EYESCIRE_API void engine_entity_set_button_color(int slot, int which, int r, int g, int b);
EYESCIRE_API int  engine_entity_button_custom(int slot, int which);
EYESCIRE_API void engine_entity_set_button_custom(int slot, int which, int on);
EYESCIRE_API int  engine_entity_button_resolved(int slot, int which, int channel);
EYESCIRE_API int  engine_entity_button_fill(int slot, int hovered, int held, int channel);
EYESCIRE_API int  engine_entity_button_toggle(int slot);
EYESCIRE_API void engine_entity_set_button_toggle(int slot, int on);
EYESCIRE_API int  engine_entity_button_toggled(int slot);
EYESCIRE_API void engine_entity_set_button_toggled(int slot, int on);
EYESCIRE_API const char* engine_entity_button_script(int slot);
EYESCIRE_API void engine_entity_set_button_script(int slot, const char* script);
EYESCIRE_API const char* engine_entity_button_fn(int slot);
EYESCIRE_API void engine_entity_set_button_fn(int slot, const char* fn);
EYESCIRE_API void engine_entity_copy_button(int dst, int src);
EYESCIRE_API void engine_entity_set_name(int slot, const char* name);
EYESCIRE_API void engine_entity_set_kind(int slot, const char* kind);
EYESCIRE_API void engine_entity_set_text(int slot, const char* text);
EYESCIRE_API void engine_entity_set_sprite(int slot, const char* path);
EYESCIRE_API void engine_entity_set_script(int slot, const char* script);

EYESCIRE_API int  engine_entity_create(const char* kind, const char* name,
                                        int x, int y, int w, int h,
                                        int r, int g, int b);
EYESCIRE_API void engine_entity_destroy(int slot);
EYESCIRE_API void engine_entity_clear_all();

EYESCIRE_API int engine_entity_find_by_name(const char* name);
EYESCIRE_API int engine_entity_find_by_id(int id);
EYESCIRE_API int engine_entity_iter_next(int from);  /* >= from active slot, or -1 */

EYESCIRE_API int engine_entity_parent(int slot);
EYESCIRE_API int engine_entity_set_parent(int slot, int parent_slot);
EYESCIRE_API int engine_entity_child_count(int slot);
EYESCIRE_API int engine_entity_child(int slot, int index);
EYESCIRE_API int  engine_entity_hier_count();
EYESCIRE_API int  engine_entity_hier_slot(int row);
EYESCIRE_API int  engine_entity_hier_depth(int row);
EYESCIRE_API int  engine_entity_hier_expanded(int slot);
EYESCIRE_API void engine_entity_set_hier_expanded(int slot, int expanded);
EYESCIRE_API int  engine_entity_toggle_hier_expanded(int slot);
EYESCIRE_API void engine_entity_hier_collapse_all();
EYESCIRE_API void engine_entity_hier_expand_all();

/* ======================================================================== */
/*  Scripting context                                                       */
/* ======================================================================== */

EYESCIRE_API int  engine_self();
EYESCIRE_API void engine_set_self(int slot);

/* Public values on an object. The script writes pub_float("speed", 5) and
   that call returns the inspector value, or 5 when the object has none.
   0 = number, 1 = switch, 2 = text. */
EYESCIRE_API int         engine_pub_scan(int slot);
EYESCIRE_API const char* engine_pub_decl_name(int index);
EYESCIRE_API int         engine_pub_decl_type(int index);
EYESCIRE_API int         engine_pub_isset(int slot, int index);
EYESCIRE_API const char* engine_pub_text(int slot, int index);
/* 0 invalid, 1 valid and already stored, 2 valid and different. */
EYESCIRE_API int         engine_pub_text_ok(int slot, int index, const char* text);
EYESCIRE_API int         engine_pub_set_text(int slot, int index, const char* text);
EYESCIRE_API int         engine_pub_bool(int slot, int index);
EYESCIRE_API void        engine_pub_set_bool(int slot, int index, int v);
EYESCIRE_API void        engine_pub_unset(int slot, int index);
EYESCIRE_API void        engine_pub_copy(int dst, int src);
EYESCIRE_API float       engine_pub_float(const char* name, float fallback);
EYESCIRE_API int         engine_pub_switch(const char* name, int fallback);
EYESCIRE_API const char* engine_pub_string(const char* name, const char* fallback);

/* ======================================================================== */
/*  Spatial helpers                                                         */
/* ======================================================================== */

EYESCIRE_API int engine_aabb_overlap(int a, int b);
EYESCIRE_API int engine_first_overlap(int slot, const char* name_prefix);
EYESCIRE_API int engine_hit_at(int x, int y);  /* topmost slot at point or -1 */

/* ======================================================================== */
/*  Console / log                                                           */
/* ======================================================================== */

EYESCIRE_API void        engine_log(const char* msg);
EYESCIRE_API int         engine_log_count();
EYESCIRE_API const char* engine_log_get(int i);  /* i == 0 is oldest in window */
EYESCIRE_API void        engine_log_clear();

/* ======================================================================== */
/*  Physics (Box2D rigidbodies)                                             */
/* ======================================================================== */

/* Scene-wide gravity in pixels/s^2 (screen +y is down). */
EYESCIRE_API void  engine_physics_set_gravity(float gx, float gy);
EYESCIRE_API float engine_physics_gravity_x();
EYESCIRE_API float engine_physics_gravity_y();
EYESCIRE_API float engine_physics_ppm();
EYESCIRE_API void  engine_physics_set_ppm(float ppm);
EYESCIRE_API int   engine_physics_active();

/* Simulation lifecycle: build bodies from entities, step, tear down. */
EYESCIRE_API void engine_physics_begin();
EYESCIRE_API void engine_physics_step(double dt);
EYESCIRE_API void engine_physics_end();

/* Soft move/turn/fade. sec <= 0 snaps. Cleared when physics ends. */
EYESCIRE_API void engine_tween_pose(int slot, float x, float y, float z, float rot, float sec);
EYESCIRE_API void engine_tween_fade(int slot, float alpha, float sec);
EYESCIRE_API void engine_tween_clear();

/* Per-entity rigidbody component. */
EYESCIRE_API int   engine_entity_has_body(int slot);
EYESCIRE_API void  engine_entity_set_body(int slot, int on);
EYESCIRE_API int   engine_entity_body_type(int slot);          /* 0=static 1=dynamic 2=kinematic */
EYESCIRE_API void  engine_entity_set_body_type(int slot, int t);
EYESCIRE_API float engine_entity_density(int slot);
EYESCIRE_API void  engine_entity_set_density(int slot, float v);
EYESCIRE_API float engine_entity_friction(int slot);
EYESCIRE_API void  engine_entity_set_friction(int slot, float v);
EYESCIRE_API float engine_entity_restitution(int slot);
EYESCIRE_API void  engine_entity_set_restitution(int slot, float v);
EYESCIRE_API float engine_entity_gravity_scale(int slot);
EYESCIRE_API void  engine_entity_set_gravity_scale(int slot, float v);
EYESCIRE_API int   engine_entity_fixed_rotation(int slot);
EYESCIRE_API void  engine_entity_set_fixed_rotation(int slot, int on);
EYESCIRE_API int   engine_entity_ui_value(int slot);         /* 0..100, slider and progress bar */
EYESCIRE_API void  engine_entity_set_ui_value(int slot, int v);
EYESCIRE_API const char* engine_ascii_char(int code);
EYESCIRE_API int   engine_entity_hitbox(int slot);          /* 0 = box, 1..5 = mesh detail */
EYESCIRE_API void  engine_entity_set_hitbox(int slot, int level);
EYESCIRE_API int   engine_entity_hitbox_count(int slot);
EYESCIRE_API float engine_entity_hitbox_cx(int slot, int i); /* px from the entity center */
EYESCIRE_API float engine_entity_hitbox_cy(int slot, int i);
EYESCIRE_API float engine_entity_hitbox_hw(int slot, int i);
EYESCIRE_API float engine_entity_hitbox_hh(int slot, int i);

/* Script-facing controls (only affect a body while the sim is active). */
EYESCIRE_API void  engine_entity_apply_force(int slot, float fx, float fy);
EYESCIRE_API void  engine_entity_apply_impulse(int slot, float ix, float iy);
EYESCIRE_API void  engine_entity_set_velocity(int slot, float vx, float vy);
EYESCIRE_API float engine_entity_velocity_x(int slot);
EYESCIRE_API float engine_entity_velocity_y(int slot);

/* ======================================================================== */
/*  Scene serialization                                                     */
/* ======================================================================== */

EYESCIRE_API int engine_scene_save(const char* path);
EYESCIRE_API int engine_scene_load(const char* path);

/* ======================================================================== */
/*  File system helpers (used by editor for project/asset browsing)         */
/* ======================================================================== */

EYESCIRE_API int  engine_fs_make_dir(const char* path);
EYESCIRE_API int  engine_fs_exists(const char* path);
EYESCIRE_API int  engine_fs_is_dir(const char* path);
EYESCIRE_API long long engine_fs_mtime(const char* path);

/* engine_fs_list iterates a directory; call sequentially with i = 0, 1, 2...
 * until it returns NULL. Names are returned without path. */
EYESCIRE_API const char* engine_fs_list(const char* dir, int i);
EYESCIRE_API int engine_fs_copy_file(const char* src, const char* dst);
EYESCIRE_API int engine_fs_copy_tree(const char* src, const char* dst);
EYESCIRE_API int engine_fs_remove_tree(const char* path);
EYESCIRE_API int engine_fs_remove_file(const char* path);
/* Copy src over dst with short retries (for replacing a just-unloaded DLL). */
EYESCIRE_API int engine_fs_replace_file(const char* src, const char* dst);

/* Shipped content encryption (ChaCha20). The random key is generated at export
   and embedded in the game .exe (no loose .key file). ship_* encrypts
   project.eye / scenes / prefabs / assets / script+extension DLLs when
   encrypt!=0 and a key is loaded; otherwise plain copy. */
EYESCIRE_API void engine_content_key_clear();
EYESCIRE_API int  engine_content_key_loaded();
EYESCIRE_API int  engine_content_key_load(const char* path);
EYESCIRE_API int  engine_content_key_generate();
EYESCIRE_API int  engine_content_key_create(const char* path); /* legacy file; prefer generate+embed */
EYESCIRE_API void engine_content_set_protect(int level); /* 0 off, 1 standard, 2 strong, 3 secure */
EYESCIRE_API int  engine_content_embed_key(const char* exe_path);
EYESCIRE_API int  engine_content_embed_key_ex(const char* exe_path, const char* dll_path);
EYESCIRE_API int  engine_content_key_load_self();
EYESCIRE_API int  engine_content_should_encrypt(const char* path);
EYESCIRE_API int  engine_content_encrypt_file(const char* src, const char* dst);
EYESCIRE_API int  engine_content_ship_file(const char* src, const char* dst, int encrypt);
EYESCIRE_API int  engine_content_ship_tree(const char* src, const char* dst, int encrypt);
EYESCIRE_API int  engine_content_ship_extensions(const char* src, const char* dst, int encrypt);
/* Player-only prefs (no editor/export keys). Written as game.cfg. */
EYESCIRE_API int  engine_content_ship_game_cfg(const char* dst, int encrypt);

/* ======================================================================== */
/*  Subprocess (used to invoke NexaC --dll for hot reload)                  */
/* ======================================================================== */

EYESCIRE_API int engine_run_command(const char* cmd_line);
EYESCIRE_API const char* engine_last_command_output();
EYESCIRE_API const char* engine_exe_dir();          /* directory of the running .exe */
EYESCIRE_API const char* engine_engine_lib();       /* absolute path to engine.lib next to the .exe */
EYESCIRE_API const char* engine_engine_dll_path();  /* absolute path to engine.dll next to the .exe */
EYESCIRE_API const char* engine_player_dll_path();  /* absolute path to EyescirePlayer.dll next to the .exe */
EYESCIRE_API const char* engine_runtime_exe_path(); /* absolute path to EyescirePlayer.exe launcher next to the .exe */

/* ======================================================================== */
/*  Dynamic library load + dispatch                                         */
/* ======================================================================== */

/* Returns a non-zero handle on success, 0 on failure.
 * On reload, the previous handle for the same path is unloaded first. */
EYESCIRE_API int  engine_dll_load(const char* path);     /* idempotent: returns existing handle if loaded */
EYESCIRE_API int  engine_dll_reload(const char* path);   /* force unload + reload for hot-reload */
EYESCIRE_API void engine_dll_unload(int handle);
EYESCIRE_API void engine_dll_unload_all();               /* releases every loaded script DLL */
EYESCIRE_API int  engine_dll_unload_path(const char* path); /* unload one path if loaded; 1 if it was */
EYESCIRE_API int  engine_dll_call(int handle, const char* sym);  /* 1 if symbol called, 0 otherwise */
EYESCIRE_API int  engine_dll_call_sig(int handle, const char* sym, const char* sig);

/* Extension trust: mark a loaded DLL as an extension and set sandboxed (0) or full (1).
   While that DLL's entry points run, sandboxed callers are blocked from dangerous APIs.
   Game script DLLs are left unmarked and stay unrestricted. */
EYESCIRE_API void engine_ext_mark(int handle, int is_extension);
EYESCIRE_API void engine_ext_set_trust(int handle, int full_permission);
EYESCIRE_API int  engine_ext_get_trust(int handle);   /* 1 = full, 0 = sandboxed, -1 = not an extension */
EYESCIRE_API int  engine_ext_is_sandboxed();          /* 1 while a sandboxed extension entry is on the stack */

/* Project extensions/ registry (extensions/<id>/ + extensions.cfg). */
EYESCIRE_API void        engine_ext_rescan();
EYESCIRE_API int         engine_ext_count();
EYESCIRE_API const char* engine_ext_id(int i);
EYESCIRE_API const char* engine_ext_name(int i);
EYESCIRE_API const char* engine_ext_version(int i);
EYESCIRE_API const char* engine_ext_desc(int i);
EYESCIRE_API int         engine_ext_enabled(int i);
EYESCIRE_API int         engine_ext_full(int i);
EYESCIRE_API int         engine_ext_loaded(int i);     /* 1 if DLL currently loaded */
EYESCIRE_API void        engine_ext_set_enabled(int i, int enabled);
EYESCIRE_API void        engine_ext_set_full(int i, int full_permission);
EYESCIRE_API void        engine_ext_save_cfg();
EYESCIRE_API int         engine_ext_install_dll(const char* dll_path); /* copy into extensions/<id>/ */
EYESCIRE_API int         engine_ext_uninstall(int i);                 /* unload + delete extensions/<id>/ */
/* Scaffold extensions/<id>/ with extension.json + starter source.
   lang: 0 = Nexa (.nxa), 1 = native C++ (.cpp). full_permission: 1 = Full in extensions.cfg.
   Returns 1 on success, 0 if no project / bad id / folder already exists. */
EYESCIRE_API int         engine_ext_create(const char* id, const char* name, const char* version,
                                          const char* description, int lang, int full_permission);
EYESCIRE_API const char* engine_ext_folder(int i);   /* absolute path to extensions/<id>/ */
EYESCIRE_API const char* engine_ext_source(int i); /* absolute path to starter source, or "" */
EYESCIRE_API const char* engine_ext_blueprint(int i); /* absolute path to ext.ebp if present, or "" */
EYESCIRE_API int         engine_ext_lang(int i);     /* 0 = nexa, 1 = cpp, -1 unknown */
EYESCIRE_API void        engine_ext_load_enabled(int editor_host); /* 1 = also call ext_editor */
EYESCIRE_API void        engine_ext_update_all();
EYESCIRE_API void        engine_ext_unload_all();
/* Unload extensions + any handle for dll_path, then brief sleep so the file can be replaced. */
EYESCIRE_API void        engine_ext_prepare_rebuild(const char* dll_path);

/* ======================================================================== */
/*  Text editor (single-buffer, line oriented)                              */
/* ======================================================================== */

EYESCIRE_API int         engine_te_load(const char* path);
EYESCIRE_API int         engine_te_save();
EYESCIRE_API int         engine_te_save_as(const char* path);
EYESCIRE_API void        engine_te_clear();
EYESCIRE_API const char* engine_te_path();
EYESCIRE_API int         engine_te_dirty();
EYESCIRE_API int         engine_te_line_count();
EYESCIRE_API const char* engine_te_line(int i);
EYESCIRE_API int         engine_te_cursor_line();
EYESCIRE_API int         engine_te_cursor_col();
EYESCIRE_API void        engine_te_set_cursor(int line, int col);
EYESCIRE_API void        engine_te_move_cursor(int dl, int dc);
EYESCIRE_API void        engine_te_home();
EYESCIRE_API void        engine_te_end();
EYESCIRE_API void        engine_te_page(int dir);   /* dir = +/- visible_lines */
EYESCIRE_API void        engine_te_insert_char(int c);
EYESCIRE_API void        engine_te_insert_text(const char* s);
EYESCIRE_API void        engine_te_newline();
EYESCIRE_API void        engine_te_backspace();
EYESCIRE_API void        engine_te_delete();
EYESCIRE_API int         engine_te_scroll_y();
EYESCIRE_API void        engine_te_set_scroll_y(int v);

/* ======================================================================== */
/*  Selection                                                               */
/* ======================================================================== */

EYESCIRE_API int  engine_entity_selected(int slot);
EYESCIRE_API void engine_entity_set_selected(int slot, int v);
EYESCIRE_API void engine_clear_selection();
EYESCIRE_API int  engine_selection_count();
EYESCIRE_API int  engine_selection_first();
EYESCIRE_API int  engine_selection_iter_next(int from);

/* ======================================================================== */
/*  Undo / redo                                                             */
/* ======================================================================== */

/* op codes for the undo stack:
 *   0 = entity snapshot   (modify in-place)
 *   1 = entity created    (undo destroys; redo recreates)
 *   2 = entity deleted    (undo restores; redo destroys)
 */

EYESCIRE_API void engine_undo_save_modify(int slot);
EYESCIRE_API void engine_undo_save_create(int slot);
EYESCIRE_API void engine_undo_save_delete(int slot);
/* Saves between begin and end undo and redo as one step. */
EYESCIRE_API void engine_undo_begin_group();
EYESCIRE_API void engine_undo_end_group();
EYESCIRE_API int  engine_undo();
EYESCIRE_API int  engine_redo();
EYESCIRE_API void engine_undo_clear();
EYESCIRE_API void engine_undo_trim(int keep);
EYESCIRE_API int  engine_undo_depth();
EYESCIRE_API int  engine_redo_depth();

/* ======================================================================== */
/*  Project state                                                           */
/* ======================================================================== */

EYESCIRE_API int         engine_project_loaded();
EYESCIRE_API const char* engine_project_root();
EYESCIRE_API const char* engine_project_name();
EYESCIRE_API void        engine_project_set_name(const char* name);
EYESCIRE_API const char* engine_project_start_scene();
EYESCIRE_API void        engine_project_set_start_scene(const char* rel);
EYESCIRE_API int         engine_pref_int(const char* key, int def);
EYESCIRE_API void        engine_pref_set_int(const char* key, int value);
EYESCIRE_API const char* engine_project_path_join(const char* a, const char* b);
EYESCIRE_API int         engine_project_open(const char* path);
EYESCIRE_API void        engine_project_close();
EYESCIRE_API int         engine_project_create(const char* path, const char* name, const char* template_id);
EYESCIRE_API void        engine_project_save_manifest();

EYESCIRE_API int         engine_project_scene_count();
EYESCIRE_API const char* engine_project_scene_rel(int i);    /* "scenes/main.scene" */
EYESCIRE_API const char* engine_project_scene_abs(int i);    /* full filesystem path */
EYESCIRE_API int         engine_project_active_scene();
EYESCIRE_API void        engine_project_set_active_scene(int i);
EYESCIRE_API int         engine_project_add_scene(const char* rel);
EYESCIRE_API int         engine_project_remove_scene(int i);
EYESCIRE_API void        engine_project_rescan_scenes(); /* add scene files found in subfolders */

EYESCIRE_API int         engine_project_script_count();
EYESCIRE_API const char* engine_project_script_name(int i);  /* "player.nxa" or "ui/hud.nxa" */
EYESCIRE_API const char* engine_project_script_path(int i);  /* full source path */
EYESCIRE_API const char* engine_project_script_dll(int i);   /* full bin/<name>.dll path */
EYESCIRE_API int         engine_project_script_generated(int i); /* 1 if a sibling .ebp owns it */
EYESCIRE_API void        engine_project_rescan_scripts();

/* Visual blueprints. The graph is a binary .ebp; Play and Build write the .nxa. */
EYESCIRE_API int         engine_project_blueprint_count();
EYESCIRE_API const char* engine_project_blueprint_name(int i);   /* "mover.ebp" */
EYESCIRE_API const char* engine_project_blueprint_path(int i);
EYESCIRE_API const char* engine_project_blueprint_script(int i); /* "mover.nxa" */

EYESCIRE_API int         engine_bp_create(const char* name);
EYESCIRE_API int         engine_bp_create_extension(const char* folder); /* writes folder/ext.ebp + generated ext.nxa */
EYESCIRE_API int         engine_bp_translate_file(const char* ebp_path, int force); /* 1 = overwrite even if .nxa was hand-edited */
EYESCIRE_API int         engine_bp_open(const char* path);
EYESCIRE_API void        engine_bp_close();
EYESCIRE_API int         engine_bp_is_open();
EYESCIRE_API const char* engine_bp_path();
EYESCIRE_API int         engine_bp_dirty();
EYESCIRE_API int         engine_bp_save();
EYESCIRE_API int         engine_bp_prepare_build(); /* save, translate every .ebp; returns failures */

EYESCIRE_API int         engine_bp_pan_x();
EYESCIRE_API int         engine_bp_pan_y();
EYESCIRE_API void        engine_bp_set_pan(int x, int y);

EYESCIRE_API int         engine_bp_node_count();
EYESCIRE_API int         engine_bp_node_kind(int i);
EYESCIRE_API const char* engine_bp_node_title(int i);
EYESCIRE_API int         engine_bp_node_x(int i);
EYESCIRE_API int         engine_bp_node_y(int i);
EYESCIRE_API void        engine_bp_node_set_pos(int i, int x, int y);
EYESCIRE_API int         engine_bp_node_op(int i);
EYESCIRE_API void        engine_bp_node_set_op(int i, int op);
EYESCIRE_API const char* engine_bp_node_text(int i);
EYESCIRE_API void        engine_bp_node_set_text(int i, const char* s);
EYESCIRE_API const char* engine_bp_node_name(int i);
EYESCIRE_API void        engine_bp_node_set_name(int i, const char* s);
EYESCIRE_API const char* engine_bp_a_text(int i);
EYESCIRE_API const char* engine_bp_b_text(int i);
EYESCIRE_API int         engine_bp_set_a_text(int i, const char* s);
EYESCIRE_API int         engine_bp_set_b_text(int i, const char* s);
EYESCIRE_API void        engine_bp_node_set_ab(int i, float a, float b);
EYESCIRE_API const char* engine_bp_key_label(int op);
EYESCIRE_API int         engine_bp_set_key_label(int i, const char* s);

EYESCIRE_API int         engine_bp_add_node(int kind, int x, int y); /* index, or -1 */
EYESCIRE_API void        engine_bp_remove_node(int i);
EYESCIRE_API int         engine_bp_link_count();
EYESCIRE_API int         engine_bp_link_from_node(int i);
EYESCIRE_API int         engine_bp_link_from_pin(int i);
EYESCIRE_API int         engine_bp_link_to_node(int i);
EYESCIRE_API int         engine_bp_link_to_pin(int i);
EYESCIRE_API int         engine_bp_add_link(int from_node, int from_pin, int to_node, int to_pin);
EYESCIRE_API void        engine_bp_clear_pin(int node, int pin);

EYESCIRE_API int         engine_bp_pin_count(int kind);
EYESCIRE_API int         engine_bp_pin_dir(int kind, int pin);  /* 0 in, 1 out */
EYESCIRE_API int         engine_bp_pin_type(int kind, int pin); /* 0 exec 1 bool 2 float 3 string 4 entity */
EYESCIRE_API const char* engine_bp_pin_name(int kind, int pin);
EYESCIRE_API const char* engine_bp_kind_name(int kind);
EYESCIRE_API int         engine_bp_node_pin_count(int node);
EYESCIRE_API int         engine_bp_node_pin_dir(int node, int pin);
EYESCIRE_API int         engine_bp_node_pin_type(int node, int pin);
EYESCIRE_API const char* engine_bp_node_pin_name(int node, int pin);
EYESCIRE_API int         engine_bp_fn_count();
EYESCIRE_API const char* engine_bp_fn_name(int i);
EYESCIRE_API int         engine_bp_fn_find(const char* name);
EYESCIRE_API int         engine_bp_editing();
EYESCIRE_API void        engine_bp_edit_fn(int i);
EYESCIRE_API int         engine_bp_add_fn();
EYESCIRE_API void        engine_bp_remove_fn(int i);
EYESCIRE_API int         engine_bp_fn_argc();
EYESCIRE_API void        engine_bp_fn_set_argc(int n);
EYESCIRE_API const char* engine_bp_fn_arg(int slot);
EYESCIRE_API void        engine_bp_fn_set_arg(int slot, const char* name);
EYESCIRE_API void        engine_bp_fn_set_name(const char* name);
EYESCIRE_API int         engine_bp_add_call(const char* name, int x, int y);
EYESCIRE_API void        engine_bp_call_set_fn(int node, int fnIndex);
EYESCIRE_API void        engine_bp_values_forget(int id);
EYESCIRE_API void        engine_bp_values_clear();
EYESCIRE_API float       engine_bp_value_get(int slot, int index, float fallback);
EYESCIRE_API void        engine_bp_value_set(int slot, int index, float v);
EYESCIRE_API int         engine_bp_value_get_bool(int slot, int index, int fallback);
EYESCIRE_API void        engine_bp_value_set_bool(int slot, int index, int v);
EYESCIRE_API const char* engine_bp_value_get_str(int slot, int index, const char* fallback);
EYESCIRE_API void        engine_bp_value_set_str(int slot, int index, const char* v);
EYESCIRE_API int         engine_bp_var_count();
EYESCIRE_API const char* engine_bp_var_name(int i);
EYESCIRE_API int         engine_bp_var_type(int i);
EYESCIRE_API const char* engine_bp_var_num(int i);
EYESCIRE_API const char* engine_bp_var_text(int i);
EYESCIRE_API int         engine_bp_var_bool(int i);
EYESCIRE_API int         engine_bp_var_find(const char* name);
EYESCIRE_API int         engine_bp_add_var(int type);
EYESCIRE_API void        engine_bp_remove_var(int i);
EYESCIRE_API void        engine_bp_var_set_name(int i, const char* name);
EYESCIRE_API void        engine_bp_var_set_type(int i, int type);
EYESCIRE_API int         engine_bp_var_set_num(int i, const char* s);
EYESCIRE_API void        engine_bp_var_set_text(int i, const char* s);
EYESCIRE_API void        engine_bp_var_set_bool(int i, int v);
EYESCIRE_API int         engine_bp_add_get(int varIndex, int x, int y);
EYESCIRE_API int         engine_bp_add_set(int varIndex, int x, int y);

EYESCIRE_API int         engine_project_prefab_count();
EYESCIRE_API const char* engine_project_prefab_name(int i);
EYESCIRE_API const char* engine_project_prefab_path(int i);
EYESCIRE_API void        engine_project_rescan_prefabs();
EYESCIRE_API int         engine_project_save_prefab(const char* name, int slot);
EYESCIRE_API int         engine_project_spawn_prefab(int i, int x, int y);

/* assets (any png/jpg/bmp/gif inside <project>/assets) */
EYESCIRE_API int         engine_project_asset_count();
EYESCIRE_API const char* engine_project_asset_name(int i);   /* "player.png" */
EYESCIRE_API const char* engine_project_asset_path(int i);   /* full filesystem path */
EYESCIRE_API const char* engine_project_asset_rel(int i);    /* "assets/player.png" */
EYESCIRE_API void        engine_project_rescan_assets();
EYESCIRE_API int         engine_project_import_asset(const char* src_path);
EYESCIRE_API const char* engine_project_last_import(); /* assets/... after the last successful import */

/* Files the running game reaches from the launch scene. engine_export_used_count
   rebuilds the list: the start scene, scenes a script opens, scripts on those
   scenes, and any project file whose path is written in those scripts. */
EYESCIRE_API int         engine_export_used_count();
EYESCIRE_API const char* engine_export_used_rel(int i); /* "scenes/main.scene" */
EYESCIRE_API int         engine_export_used_has(const char* rel);
EYESCIRE_API int         engine_export_start_index(); /* scene index the build opens */

/* Splash list lives in the project's splash.cfg. The brand screen is the
   built-in "Made with Eyescire Engine" card. engine_splash_run plays the
   brand screen (unless turned off) and then each image: fade in, hold, fade
   out. With skip on, a click or a key advances to the next screen.
   Returns 0 if the window was closed. */
EYESCIRE_API int         engine_splash_brand();
EYESCIRE_API void        engine_splash_set_brand(int on);
EYESCIRE_API int         engine_splash_keys();          /* 1: a click or a key skips to the next screen */
EYESCIRE_API void        engine_splash_set_keys(int on);
EYESCIRE_API int         engine_splash_count();
EYESCIRE_API const char* engine_splash_image(int i);
EYESCIRE_API const char* engine_splash_hold_text(int i);
EYESCIRE_API const char* engine_splash_fade_text(int i);
EYESCIRE_API void        engine_splash_set_hold_text(int i, const char* text);
EYESCIRE_API void        engine_splash_set_fade_text(int i, const char* text);
EYESCIRE_API int         engine_splash_add(const char* image, int hold_ms, int fade_ms);
EYESCIRE_API void        engine_splash_remove(int i);
EYESCIRE_API int         engine_splash_run();

/* ----- 3D (scene v7) and hosts with their own window (EyescirePlayer3D) ----- */
EYESCIRE_API int  engine_entity_z(int slot);          /* px into the screen */
EYESCIRE_API void engine_entity_set_z(int slot, int v);   /* moves children too */
/* First-person walk: yaw in degrees, forward/strafe in -1..1, jump nonzero.
   Slides on static solids and returns 1 when the feet are on a floor. */
EYESCIRE_API int engine_fps_move(int slot, float yaw_deg, float forward, float strafe,
                                int jump, float speed, float jump_speed);
EYESCIRE_API int  engine_entity_depth(int slot);      /* px; 0 = kind default */
EYESCIRE_API void engine_entity_set_depth(int slot, int v);
/* lights: kinds light_directional / light_point / light_spot / light_ambient */
EYESCIRE_API int  engine_entity_light_intensity(int slot);   /* percent */
EYESCIRE_API int  engine_entity_light_range(int slot);       /* px (point, spot) */
EYESCIRE_API int  engine_entity_light_angle(int slot);       /* spot cone, degrees */
EYESCIRE_API int  engine_entity_light_tilt(int slot);        /* degrees into the screen */
EYESCIRE_API void engine_entity_set_light_intensity(int slot, int v);
EYESCIRE_API void engine_entity_set_light_range(int slot, int v);
EYESCIRE_API void engine_entity_set_light_angle(int slot, int v);
EYESCIRE_API void engine_entity_set_light_tilt(int slot, int v);
EYESCIRE_API int  engine_entity_visible(int slot);   /* this object's own flag */
EYESCIRE_API void engine_entity_set_visible(int slot, int on);
EYESCIRE_API int  engine_entity_shown(int slot);     /* 0 if this or a parent is hidden */
EYESCIRE_API const char* engine_entity_model(int slot);       /* .obj, as stored */
EYESCIRE_API const char* engine_entity_model_path(int slot);  /* resolved */
EYESCIRE_API void engine_entity_set_model(int slot, const char* path);
EYESCIRE_API const char* engine_entity_sound(int slot);
EYESCIRE_API void engine_entity_set_sound(int slot, const char* path);
EYESCIRE_API int  engine_entity_sound_loop(int slot);
EYESCIRE_API void engine_entity_set_sound_loop(int slot, int on);
EYESCIRE_API int  engine_entity_sound_range(int slot);
EYESCIRE_API void engine_entity_set_sound_range(int slot, int px);
EYESCIRE_API int  engine_entity_emit_rate(int slot);
EYESCIRE_API void engine_entity_set_emit_rate(int slot, int v);
EYESCIRE_API int  engine_entity_emit_life(int slot);
EYESCIRE_API void engine_entity_set_emit_life(int slot, int v);
EYESCIRE_API int  engine_entity_emit_speed(int slot);
EYESCIRE_API void engine_entity_set_emit_speed(int slot, int v);
EYESCIRE_API int  engine_entity_emit_spread(int slot);
EYESCIRE_API void engine_entity_set_emit_spread(int slot, int v);
EYESCIRE_API int  engine_entity_emit_size(int slot);
EYESCIRE_API void engine_entity_set_emit_size(int slot, int v);
EYESCIRE_API int  engine_entity_emit_hd(int slot);
EYESCIRE_API void engine_entity_set_emit_hd(int slot, int on);
EYESCIRE_API int  engine_entity_aa(int slot);          /* -1 project, 0 off, 1 on */
EYESCIRE_API void engine_entity_set_aa(int slot, int mode);
EYESCIRE_API int  engine_emit_burst(const char* path, float x, float y, float z, int r, int g, int b, int hd);
EYESCIRE_API void engine_particles_draw_2d();
EYESCIRE_API int  engine_scene_render3d();            /* 1 = play in 3D */
EYESCIRE_API void engine_scene_set_render3d(int on);
EYESCIRE_API int  engine_scene_clear_r();
EYESCIRE_API int  engine_scene_clear_g();
EYESCIRE_API int  engine_scene_clear_b();
EYESCIRE_API void engine_scene_set_clear(int r, int g, int b);
EYESCIRE_API float engine_scene_fov();
EYESCIRE_API void engine_scene_set_fov(float deg);
EYESCIRE_API int  engine_scene_sky();                 /* 0 day, 1 sunset, 2 night, 3 custom, 4 solid */
EYESCIRE_API void engine_scene_set_sky(int mode);
EYESCIRE_API const char* engine_scene_sky_face(int face); /* 0..5: right left up down front back */
EYESCIRE_API void engine_scene_set_sky_face(int face, const char* path);

/* Day/night cycle (drives the first directional + ambient light + clear color). */
EYESCIRE_API int   engine_day_cycle();
EYESCIRE_API void  engine_set_day_cycle(int on);
EYESCIRE_API float engine_day_time();                 /* 0..1 */
EYESCIRE_API void  engine_set_day_time(float t);
EYESCIRE_API float engine_day_length();               /* seconds per full loop */
EYESCIRE_API void  engine_set_day_length(float sec);
EYESCIRE_API int   engine_day_paused();
EYESCIRE_API void  engine_set_day_paused(int on);
EYESCIRE_API void  engine_day_apply();

/* Weather: 0 none, 1 rain, 2 snow. Wetness/snow cover are 0..1 render tints. */
EYESCIRE_API int   engine_weather();
EYESCIRE_API void  engine_set_weather(int mode);
EYESCIRE_API int   engine_weather_intensity();        /* 0..100 */
EYESCIRE_API void  engine_set_weather_intensity(int v);
EYESCIRE_API float engine_wetness();
EYESCIRE_API float engine_snow_cover();
EYESCIRE_API void  engine_set_wetness(float v);
EYESCIRE_API void  engine_set_snow_cover(float v);

EYESCIRE_API void  engine_env_set_focus(float sx, float sy, float sz);
EYESCIRE_API int  engine_init_headless(int w, int h); /* no window; see engine.cpp */
EYESCIRE_API void engine_input_begin_frame();
EYESCIRE_API void engine_input_key(int vk, int down);
EYESCIRE_API void engine_input_mouse(int x, int y);
EYESCIRE_API void engine_input_button(int button, int down);
EYESCIRE_API void engine_input_wheel(int notches);
EYESCIRE_API int  engine_launch(const char* cmd_line, const char* cwd); /* no wait */
/* software 3D view (host/render3d.inl): world units = scene px / 32, y up, z out of screen */
EYESCIRE_API void engine_r3d_begin(int x, int y, int w, int h, double ex, double ey, double ez,
                                   double tx, double ty, double tz, double fov_deg, int br, int bg, int bb);
EYESCIRE_API void engine_r3d_entity(int slot);
/* While on, a first-person camera does not draw the possessed body. */
EYESCIRE_API void engine_r3d_hide_pawn(int on);
/* This slot is a held prop for the current frame: drawn in front of the walls. */
EYESCIRE_API void engine_r3d_view_hold(int slot);
EYESCIRE_API void engine_r3d_hitbox(int slot);
EYESCIRE_API void engine_r3d_cone(double bx, double by, double bz, double tx, double ty, double tz,
                                  double radius, int r, int g, int b);
EYESCIRE_API void engine_r3d_screen_rect(int x, int y, int w, int h, int r, int g, int b, int a);
/* Translucent wedge in a gizmo ring. axis 1 = X, 2 = Y, 3 = Z.
   ang0 and sweep are degrees in that ring's circle. Drawn in front of the scene. */
EYESCIRE_API void engine_r3d_sector(double cx, double cy, double cz, int axis,
                                    double ang0, double sweep, double radius,
                                    int r, int g, int b, int a);
EYESCIRE_API void engine_r3d_line(double x1, double y1, double z1, double x2, double y2, double z2,
                                  int r, int g, int b, int a);
EYESCIRE_API void engine_r3d_end(int outline_r, int outline_g, int outline_b); /* r < 0: no outline */
EYESCIRE_API int  engine_r3d_pick(int x, int y);
EYESCIRE_API int  engine_r3d_project(double wx, double wy, double wz);
EYESCIRE_API double engine_r3d_px();
EYESCIRE_API double engine_r3d_py();
EYESCIRE_API int  engine_r3d_plane_hit(int x, int y, double scene_z);
/* Degrees of the mouse around a world axis through (wx, wy, wz).
   axis 1 = X, 2 = Y, 3 = Z. 100000 when the ray misses the plane. */
EYESCIRE_API int  engine_r3d_axis_angle(int x, int y, int axis, double wx, double wy, double wz);
/* Angle of the cursor in a plane. n is the plane normal. u is angle 0, v is angle 90. */
EYESCIRE_API int  engine_r3d_basis_angle(int x, int y, double cx, double cy, double cz,
                                         double nx, double ny, double nz,
                                         double ux, double uy, double uz,
                                         double vx, double vy, double vz);
EYESCIRE_API int  engine_r3d_ray_point(int x, int y, double dist); /* result: hx / hy / hz, scene px */
EYESCIRE_API double engine_r3d_hz();
EYESCIRE_API double engine_r3d_hx();
EYESCIRE_API double engine_r3d_hy();
EYESCIRE_API int  engine_r3d_backend();   /* 0 none yet, 1 software, 2 OpenGL, 3 Vulkan 1.4 */
EYESCIRE_API int  engine_r3d_renderer(const char* name); /* "OpenGL 1.1", "OpenGL 3.3", "Vulkan" */
EYESCIRE_API const char* engine_r3d_backend_name(); /* what the last frame drew with */
/* Arm the next present to keep a top-down RGBA copy of the picture on screen. */
EYESCIRE_API void engine_r3d_capture_arm();
EYESCIRE_API int  engine_r3d_frame_wh(int* w, int* h);
EYESCIRE_API int  engine_r3d_frame_copy(unsigned char* dst, int bytes);
/* hidden, held cursor with relative motion (look-around cameras) */
EYESCIRE_API void engine_set_paused(int on);
EYESCIRE_API int  engine_paused();
EYESCIRE_API void engine_request_leave_play();
EYESCIRE_API int  engine_take_leave_play();
EYESCIRE_API void engine_mouse_lock(int on);
EYESCIRE_API int  engine_mouse_locked();
EYESCIRE_API void engine_mouse_lock_poll();   /* once a frame while locked */
EYESCIRE_API int  engine_mouse_lock_dx();     /* screen px since the last poll */
EYESCIRE_API int  engine_mouse_lock_dy();
EYESCIRE_API int  engine_client_w();
EYESCIRE_API int  engine_client_h();
EYESCIRE_API int  engine_fit_buffer(int w, int h); /* resize the back buffer, not the window */

/* native folder picker; returns "" if cancelled */
EYESCIRE_API const char* engine_open_folder_dialog(const char* title, const char* initial_dir);
/* first line of clipboard text, trimmed and unquoted ("" if none) */
EYESCIRE_API const char* engine_clipboard_line();
EYESCIRE_API void engine_clipboard_set(const char* text);

/* native open-file dialog (used by the editor's Import Asset flow) */
EYESCIRE_API const char* engine_open_file_dialog(const char* title,
                                                 const char* filter,
                                                 const char* initial_dir);

EYESCIRE_API int         engine_project_recent_count();
EYESCIRE_API const char* engine_project_recent(int i);
EYESCIRE_API void        engine_project_recent_set_file(const char* path);
EYESCIRE_API void        engine_project_recent_load();
EYESCIRE_API void        engine_project_recent_save();
EYESCIRE_API void        engine_project_recent_push(const char* path);
EYESCIRE_API void        engine_project_recent_remove(const char* path);
EYESCIRE_API void        engine_project_recent_clear();

/* ======================================================================== */
/*  Camera view (shared by editor, runtime, and scripts)                    */
/* ======================================================================== */

/* World origin of the top-left of the current view, plus zoom and the
 * on-screen rectangle the world is drawn into. Scripts use this for
 * mouse-to-world and HUD placement; the tilemap renderer uses it too. */
EYESCIRE_API void  engine_view_set(float cam_x, float cam_y, float zoom,
                                   int view_x, int view_y, int view_w, int view_h);
EYESCIRE_API float engine_view_cam_x();
EYESCIRE_API float engine_view_cam_y();
EYESCIRE_API float engine_view_zoom();
EYESCIRE_API int   engine_view_x();
EYESCIRE_API int   engine_view_y();
EYESCIRE_API int   engine_view_w();
EYESCIRE_API int   engine_view_h();
EYESCIRE_API int   engine_screen_to_world_x(int sx);
EYESCIRE_API int   engine_screen_to_world_y(int sy);
EYESCIRE_API int   engine_world_to_screen_x(int wx);
EYESCIRE_API int   engine_world_to_screen_y(int wy);
EYESCIRE_API int   engine_world_to_screen_len(int wl);

/* ======================================================================== */
/*  Tilemap (2D sandbox worlds)                                             */
/* ======================================================================== */

/* Create or replace the active tilemap. tile_size is world pixels per cell. */
EYESCIRE_API int  engine_tilemap_create(int w, int h, int tile_size);
EYESCIRE_API void engine_tilemap_destroy();
EYESCIRE_API int  engine_tilemap_active();
EYESCIRE_API int  engine_tilemap_width();
EYESCIRE_API int  engine_tilemap_height();
EYESCIRE_API int  engine_tilemap_tile_size();

EYESCIRE_API int  engine_tilemap_get(int tx, int ty);
EYESCIRE_API void engine_tilemap_set(int tx, int ty, int id);
EYESCIRE_API void engine_tilemap_fill(int tx, int ty, int tw, int th, int id);
EYESCIRE_API void engine_tilemap_fill_column(int tx, int y0, int y1, int id);

/* Tile type 0 is air (empty, not solid). ids 1..255 are user-defined. */
EYESCIRE_API void engine_tilemap_define(int id, int r, int g, int b, int solid, const char* sprite);
/* Extra sprites for one tile id, chosen per cell at draw time so a textured
   tile does not repeat into an obvious grid. Call after define. */
EYESCIRE_API void engine_tilemap_add_variant(int id, const char* sprite);
/* One atlas for a tile id. cell is the source square, stride is the grid
   step (cell plus gap). Then add cells with engine_tilemap_sheet_cell, or
   2x2 blocks with engine_tilemap_sheet_block so 16px autotile pieces match
   a 32px world tile. */
EYESCIRE_API void engine_tilemap_sheet(int id, const char* path, int cell, int stride);
EYESCIRE_API void engine_tilemap_sheet_cell(int id, int col, int row);
EYESCIRE_API void engine_tilemap_sheet_block(int id, int col, int row);
/* Terraria-format autotile sheet: a grid of `cell`-pixel frames on a `stride`
   pitch, each frame a whole tile drawn for one arrangement of neighbours. The
   arrangement is read off the art (exposed sides are outlined and rounded), so
   no frame table is needed and a replacement sheet works as-is. */
EYESCIRE_API void engine_tilemap_sheet_autotile(int id, const char* path, int cell, int stride,
                                                int use_cols, int use_rows);
/* Pin one frame to one neighbour arrangement (mask bits: 1 up, 2 down, 4 left,
   8 right = that side is open). First call for a mask replaces the derived
   frames; later calls add variants. */
EYESCIRE_API void engine_tilemap_autotile_frame(int id, int mask, int col, int row);

/* Parallax backdrop drawn behind the tiles. par is per-mille of camera
   movement (0 pins to the screen, 1000 moves with the world); fill_view
   stretches the layer over the view in screen space, otherwise its bottom
   sits on world Y anchor_y at scale per-mille of native size. Max 8 layers. */
EYESCIRE_API void engine_tilemap_backdrop_clear();
EYESCIRE_API void engine_tilemap_backdrop_add(int image_id, int par, int anchor_y,
                                              int scale, int fill_view);
EYESCIRE_API int  engine_tilemap_solid(int tx, int ty);
EYESCIRE_API int  engine_tilemap_solid_at(int px, int py);
EYESCIRE_API int  engine_tilemap_collide_rect(int x, int y, int w, int h);

/* Move an AABB through solid tiles. Results are read via the last_* getters. */
EYESCIRE_API void engine_tilemap_move(int x, int y, int w, int h, int dx, int dy);
EYESCIRE_API int  engine_tilemap_last_x();
EYESCIRE_API int  engine_tilemap_last_y();
EYESCIRE_API int  engine_tilemap_last_hit_x();
EYESCIRE_API int  engine_tilemap_last_hit_y();
EYESCIRE_API int  engine_tilemap_last_grounded();
EYESCIRE_API void engine_tilemap_move_entity(int slot, int dx, int dy);

EYESCIRE_API int  engine_tilemap_world_to_tile_x(int px);
EYESCIRE_API int  engine_tilemap_world_to_tile_y(int py);
EYESCIRE_API int  engine_tilemap_tile_to_world_x(int tx);
EYESCIRE_API int  engine_tilemap_tile_to_world_y(int ty);
EYESCIRE_API int  engine_tilemap_surface_y(int tx); /* first solid from the top, or height */

EYESCIRE_API void engine_tilemap_set_sky(int r, int g, int b);
EYESCIRE_API void engine_tilemap_set_cursor(int tx, int ty); /* -1 clears */
EYESCIRE_API void engine_tilemap_draw(int fill_sky);

/* Deterministic value noise, 0..1023. 1D is interpolated for rolling hills. */
EYESCIRE_API int  engine_noise1(int x, int seed);
EYESCIRE_API int  engine_noise2(int x, int y, int seed);

/* Replace `from_id` with `to_id` in [y0, y1) where noise2 > threshold. */
EYESCIRE_API void engine_tilemap_noise_replace(int from_id, int to_id,
                                               int y0, int y1,
                                               int threshold, int seed, int freq);

EYESCIRE_API int  engine_tilemap_save(const char* path);
EYESCIRE_API int  engine_tilemap_load(const char* path);

#ifdef __cplusplus
}
#endif

/*
 * scenes.h - the four screens of the game and how they hand control around.
 *
 *   TITLE  -> TOWN (the hamlet: estate, tavern, abbey, quest board)
 *   TOWN   -> DUNGEON (embark on an expedition)
 *   DUNGEON<->BATTLE (walking into a monster room starts a fight; when it
 *                    ends we return to the dungeon, which reads the result)
 *   DUNGEON-> TOWN (expedition over)
 *
 * Each scene is implemented in scene_<name>.c and exposes enter/update/draw.
 * Scenes talk through a few globals/functions declared here rather than
 * including each other's internals.
 */
#ifndef DD_SCENES_H
#define DD_SCENES_H

typedef enum { SCENE_TITLE, SCENE_TOWN, SCENE_DUNGEON, SCENE_BATTLE } SceneId;

/* Request a scene change (applied next frame, see game.c). */
void scene_switch(SceneId id);
void game_request_quit(void);

/* Battle: start with an encounter (dd_encounters index) in front of a background image.
 * When it ends the dungeon scene resumes and reads battle_scene_result(). */
void battle_scene_start(int encounter, const char *background);
/* Surprise context for the next battle_scene_start (see battle_roll_surprise); reset after each fight. */
void battle_scene_surprise(int scouted, int forced);
/* The dungeon calls this after the battle scene returns. */
int battle_scene_result(void); /* 1 won, 2 lost, 3 retreated */
extern float battle_corr_frac; /* party position in the current corridor, for the battle HUD map */

/* Per-scene callbacks (implemented in scene_*.c). */
void title_enter(void);   void title_update(float dt);   void title_draw(void);
void town_enter(void);    void town_update(float dt);    void town_draw(void);
void dungeon_enter(void); void dungeon_update(float dt); void dungeon_draw(void);
void battle_enter(void);  void battle_update(float dt);  void battle_draw(void);
void battle_leave(void);

/* Colour grade set of the current expedition (scene_dungeon.c), for grade_scene(). */
const char *dungeon_grade_dir(void);
#endif

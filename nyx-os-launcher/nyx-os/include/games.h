// include/games.h
#ifndef GAMES_H
#define GAMES_H

void games_draw(void);
void games_handle_key(char c);
void games_init(void);

// Snake
void snake_init(void);
void snake_draw(void);
void snake_handle_key(char c);
void snake_tick(void);
int  snake_is_running(void);

// Tetris
void tetris_init(void);
void tetris_draw(void);
void tetris_handle_key(char c);
void tetris_tick(void);

// Physics
void physics_init(void);
void physics_draw(void);
void physics_handle_key(char c);
void physics_tick(void);
void physics_stop(void);
//Minesweeper
// Minesweeper
void minesweeper_init(void);
void minesweeper_draw(void);
void minesweeper_handle_key(char c);
void minesweeper_tick(void);

// Mouse handler (called by desktop_draw click dispatch).
// Returns 1 if the click was handled, 0 otherwise.
int games_handle_mouse(int clk_x, int clk_y);

#endif

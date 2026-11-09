/* WOOD: директор волн (см. wood_director.c). */
#ifndef __WOOD_DIRECTOR__
#define __WOOD_DIRECTOR__

/* Вызывается в конце P_SetupLevel: сброс забега, разбор WOODCFG, стартовое оружие. */
void WOOD_LevelStart(void);

/* Вызывается каждый игровой тик из P_Ticker. */
void WOOD_Ticker(void);

/* WOOD-режим включён (в WAD есть WOODCFG). */
int WOOD_Enabled(void);

/* Случайная раскладка леса для нового забега: номер карты 1..N, не равный current. */
int WOOD_PickRunMap(int current);

/* Смерть игрока в WOOD-режиме: вызывается из P_DeathThink каждый тик.
 * Возвращает true, когда игрок нажал «Again?» (пробел/Enter) и пора начать новый забег. */
struct player_s;
int WOOD_DeathThink(struct player_s *player);

/* Рисует «Again?» поверх экрана, пока игрок мёртв. Вызывается из D_Display после HU_Drawer. */
void WOOD_Drawer(void);

#endif

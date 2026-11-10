/* WOOD: директор волн (см. wood_director.c). */
#ifndef __WOOD_DIRECTOR__
#define __WOOD_DIRECTOR__

#include "d_player.h"

/* Вызывается в конце P_SetupLevel: сброс забега, разбор WOODCFG, стартовое оружие. */
void WOOD_LevelStart(void);

/* Вызывается каждый игровой тик из P_Ticker. */
void WOOD_Ticker(void);

/* WOOD-режим включён (в WAD есть WOODCFG). */
int WOOD_Enabled(void);

/* Случайная раскладка леса для нового забега: номер карты 1..N, не равный current. */
int WOOD_PickRunMap(int current);

/* Забег выигран (босс повержен): финальный экран показывает WOOD_VictoryText. */
int WOOD_RunWon(void);
const char *WOOD_VictoryText(void);

/* Финальный экран закрыт — забег окончен. */
void WOOD_EndRun(void);

/* Смерть игрока в WOOD-режиме: вызывается из P_DeathThink каждый тик.
 * Возвращает true, когда игрок нажал «Again?» (пробел/Enter) и пора начать новый забег. */
struct player_s;
int WOOD_DeathThink(struct player_s *player);

/* Рисует экран итогов забега («Again?», время, убийства, деревья, поляны), пока игрок мёртв. Вызывается из D_Display после HU_Drawer. */
void WOOD_Drawer(void);

/* Бензопила не задела врага: если перед игроком дерево из saw_trees, пилим его.
 * Возвращает true, если пила попала в дерево. */
dboolean WOOD_SawTree(player_t *p, angle_t angle, int damage);

#endif

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

/* Забег выигран (босс повержен): финальный экран показывает WOOD_VictoryText. */
int WOOD_RunWon(void);
const char *WOOD_VictoryText(void);

/* Финальный экран закрыт — забег окончен. */
void WOOD_EndRun(void);

#endif

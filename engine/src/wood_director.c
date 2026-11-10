/* WOOD: директор волн.
 *
 * Включается, только если в загруженных WAD есть лампа WOODCFG.
 *
 * Что делает:
 *  - на старте уровня оставляет игроку только бензопилу;
 *  - находит поляны (секторы с тегом > 0);
 *  - когда игрок заходит на ещё не зачищенную поляну, закрывает её невидимой стеной
 *    и спавнит волну врагов на «бюджет угрозы», растущий с каждой зачисткой;
 *  - когда все враги волны мертвы, открывает поляну и выдаёт награду
 *    (следующее оружие из списка, аптечку);
 *  - логово босса (поляна с тегом WOOD_BOSS_TAG) заперто факелами, пока не зачищены
 *    все обычные поляны; в логове — босс из WOODCFG со свитой;
 *  - победа над боссом — конец забега: финальный экран со статистикой, затем титульник;
 *  - деревья можно спилить бензопилой (WOOD_SawTree): спиленное дерево исчезает
 *    и роняет немного патронов (на его месте можно оставить saw_stump);
 *  - часть деревьев — мимики: шуршат, если подойти, и кусаются, если подойти вплотную
 *    или начать пилить (листва краснеет); спиленный мимик роняет аптечку;
 *  - иногда в лесу растёт золотое дерево: светится, пилится дольше, роняет редкий лут.
 *
 * Параметры берутся из WOODCFG (см. wad/lumps/woodcfg.txt).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "doomstat.h"
#include "d_player.h"
#include "info.h"
#include "m_fixed.h"
#include "p_mobj.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_pspr.h"
#include "p_tick.h"
#include "p_inter.h"
#include "r_main.h"
#include "r_state.h"
#include "s_sound.h"
#include "sounds.h"
#include "hu_stuff.h"
#include "v_video.h"
#include "r_patch.h"
#include "w_wad.h"
#include "lprintf.h"
#include "r_draw.h"
#include "g_game.h"
#include "wood_director.h"

#define WOOD_MAX_CLEARINGS   32
#define WOOD_MAX_MONSTERS    32
#define WOOD_MAX_WEAPONS     16
#define WOOD_MAX_REWARDS     8
#define WOOD_MAX_WAVE        64
#define WOOD_MAX_BARRIER     256
#define WOOD_MAX_BOSSES      8
#define WOOD_BOSS_TAG        100      /* тег логова; совпадает с BOSS_TAG в gen_forest.py */
#define WOOD_VICTORY_DELAY   (5 * TICRATE)

typedef struct {
  mobjtype_t type;
  int cost;
  int min_wave;
} wood_monster_t;

typedef struct {
  sector_t *sector;
  fixed_t cx, cy;      /* центр (по габаритам) */
  fixed_t radius;      /* половина меньшей стороны габаритов */
  dboolean cleared;    /* зачищена */
  dboolean boss;       /* логово босса */
} wood_clearing_t;

#define WOOD_MAX_TORCHES     128

/* Огненная изгородь: линии, закрытые ML_BLOCKING, и факелы вдоль них. */
typedef struct {
  int lines[WOOD_MAX_BARRIER];
  int num_lines;
  mobj_t *torches[WOOD_MAX_TORCHES];
  int num_torches;
} wood_fence_t;

/* --- настройки из WOODCFG --- */
static int cfg_budget_base = 4;
static int cfg_budget_step = 3;
static int cfg_max_monsters = 24;
static wood_monster_t cfg_monsters[WOOD_MAX_MONSTERS];
static int cfg_num_monsters;
static mobjtype_t cfg_weapons[WOOD_MAX_WEAPONS];
static int cfg_num_weapons;
static mobjtype_t cfg_rewards[WOOD_MAX_REWARDS];
static int cfg_num_rewards;
static mobjtype_t cfg_gate_rewards[WOOD_MAX_REWARDS];
static int cfg_num_gate_rewards;
static mobjtype_t cfg_bosses[WOOD_MAX_BOSSES];
static int cfg_num_bosses;
static int cfg_boss_escorts = 50;     /* бюджет свиты босса, % от обычной волны */
static mobjtype_t cfg_ammo_rewards[WOOD_MAX_REWARDS];
static int cfg_num_ammo_rewards;

/* --- пила по деревьям --- */
#define WOOD_MAX_SAW_TREES   8
static mobjtype_t cfg_saw_trees[WOOD_MAX_SAW_TREES];
static int cfg_num_saw_trees;
static int cfg_saw_tree_health = 120;
static mobjtype_t cfg_saw_stump = NUMMOBJTYPES;
static mobjtype_t cfg_saw_drops[WOOD_MAX_REWARDS];
static int cfg_num_saw_drops;

/* --- деревья-мимики --- */
#define WOOD_MAX_MIMICS      96
#define WOOD_MIMIC_SAFE      (384 * FRACUNIT)   /* у старта мимиков нет */
#define WOOD_MIMIC_RUSTLE    (192 * FRACUNIT)   /* ближе — мимик шуршит (подсказка) */
#define WOOD_MIMIC_REACH     (80 * FRACUNIT)    /* досягаемость укуса от края ствола */
#define WOOD_MIMIC_RED       ((uint_64_t)3 << MF_TRANSSHIFT) /* листва -> красная */
static int cfg_mimic_chance = 0;              /* % деревьев, которые окажутся мимиками */
static int cfg_mimic_health = 240;
static int cfg_mimic_wake = 64;               /* проснуться, если игрок ближе (от края ствола) */
static int cfg_mimic_dmg_min = 6, cfg_mimic_dmg_max = 14;
static int cfg_mimic_bite_tics = 25;
static mobjtype_t cfg_mimic_drops[WOOD_MAX_REWARDS];
static int cfg_num_mimic_drops;

typedef struct {
  mobj_t *mo;
  dboolean awake;
  int cooldown;      /* тиков до следующего укуса / шороха */
} wood_mimic_t;
static wood_mimic_t mimics[WOOD_MAX_MIMICS];
static int num_mimics;
static dboolean mimic_warned;   /* «THE TREE IS ALIVE!» — один раз за забег */

/* --- золотое дерево --- */
#define WOOD_GOLDEN_SAFE     (768 * FRACUNIT)   /* подальше от старта, чтобы его искать */
static mobjtype_t cfg_golden_type = NUMMOBJTYPES;
static int cfg_golden_chance = 0;             /* % забегов, в которых оно есть */
static int cfg_golden_health = 360;
static mobjtype_t cfg_golden_drops[WOOD_MAX_REWARDS];
static int cfg_num_golden_drops;
static dboolean golden_hint;    /* показать «что-то блестит» на первом тике */

/* --- состояние забега (сбрасывается при каждой загрузке уровня, в т.ч. после смерти) --- */
static dboolean wood_active;
static wood_clearing_t clearings[WOOD_MAX_CLEARINGS];
static int num_clearings;
static int current = -1;              /* индекс поляны с идущим боем, -1 — боя нет */
static mobj_t *wave[WOOD_MAX_WAVE];
static int wave_size;
static wood_fence_t fence;            /* вокруг поляны, где идёт бой */
static wood_fence_t gate;             /* у входа в логово, пока остальные поляны не зачищены */
#define WOOD_TORCH_DOOMEDNUM 46       /* красный факел: видимая «огненная изгородь» */
#define WOOD_TORCH_STEP      (40 * FRACUNIT)
static int boss_idx = -1;             /* индекс логова в clearings, -1 — на карте его нет */
static int waves_cleared;             /* всего зачисток за забег — ось сложности */
static int victory_tics;              /* отсчёт до финального экрана после победы */
static dboolean run_won;              /* забег выигран: финальный экран — наш */
static int trees_felled;              /* спилено деревьев за забег */

/* Итоги забега: снимок в момент смерти или победы, чтобы цифры не менялись,
 * пока игрок смотрит на экран итогов (время идёт, ракета в полёте ещё может убить). */
typedef struct {
  int tics;          /* время в лесу */
  int kills;
  int trees;
  int cleared;       /* зачищено полян, включая логово */
  int clearings;     /* всего полян на карте, включая логово */
} wood_summary_t;
static wood_summary_t summary;
static unsigned int rng_state = 0;

/* --- музыка --- */
#define WOOD_MAX_TRACKS 16
static char cfg_music_calm[WOOD_MAX_TRACKS][9];
static int cfg_num_calm;
static char cfg_music_battle[WOOD_MAX_TRACKS][9];
static int cfg_num_battle;
static int last_calm = -1, last_battle = -1;
static dboolean music_started;

static void Wood_Seed(void)
{
  rng_state = (unsigned int)time(NULL) ^ ((unsigned int)clock() << 11) ^ 0x9E3779B9u;
  if (!rng_state)
    rng_state = 1;
}

/* Свой генератор: P_Random() нельзя трогать, он отвечает за синхронизацию демо. */
static unsigned int Wood_Rand(void)
{
  if (!rng_state)
    Wood_Seed();
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static int Wood_RandRange(int lo, int hi) /* [lo, hi] */
{
  if (hi <= lo)
    return lo;
  return lo + (int)(Wood_Rand() % (unsigned int)(hi - lo + 1));
}

static mobjtype_t Wood_TypeForDoomednum(int num)
{
  int i;
  for (i = 0; i < NUMMOBJTYPES; i++)
    if (mobjinfo[i].doomednum == num)
      return (mobjtype_t)i;
  return NUMMOBJTYPES;
}

/* Читает список doomednum после ключевого слова в массив типов. */
static void Wood_ParseTypeList(const char *rest, mobjtype_t *out, int *count, int max)
{
  char buf[256];
  char *tok;
  strncpy(buf, rest, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  for (tok = strtok(buf, " \t"); tok && *count < max; tok = strtok(NULL, " \t"))
  {
    mobjtype_t t = Wood_TypeForDoomednum(atoi(tok));
    if (t != NUMMOBJTYPES)
      out[(*count)++] = t;
    else
      lprintf(LO_WARN, "WOODCFG: unknown thing %s\n", tok);
  }
}

static void Wood_ParseConfig(int lump)
{
  int len = W_LumpLength(lump);
  const char *data = W_CacheLumpNum(lump);
  char line[256];
  int pos = 0;

  cfg_num_monsters = cfg_num_weapons = cfg_num_rewards = 0;
  cfg_num_gate_rewards = cfg_num_ammo_rewards = cfg_num_bosses = 0;
  cfg_num_calm = cfg_num_battle = 0;
  cfg_num_saw_trees = cfg_num_saw_drops = 0;
  cfg_saw_stump = NUMMOBJTYPES;
  cfg_num_mimic_drops = cfg_num_golden_drops = 0;
  cfg_mimic_chance = cfg_golden_chance = 0;
  cfg_golden_type = NUMMOBJTYPES;

  while (pos < len)
  {
    int n = 0;
    char key[32];
    const char *rest;
    char *hash;

    while (pos < len && data[pos] != '\n' && n < (int)sizeof(line) - 1)
      line[n++] = data[pos++];
    while (pos < len && data[pos] != '\n')
      pos++;
    pos++;
    line[n] = 0;
    if ((hash = strchr(line, '#')) != NULL)
      *hash = 0;
    if (sscanf(line, "%31s", key) != 1)
      continue;
    rest = strstr(line, key) + strlen(key);

    if (!strcmp(key, "budget_base"))
      cfg_budget_base = atoi(rest);
    else if (!strcmp(key, "budget_step"))
      cfg_budget_step = atoi(rest);
    else if (!strcmp(key, "max_monsters"))
      cfg_max_monsters = BETWEEN(1, WOOD_MAX_WAVE, atoi(rest));
    else if (!strcmp(key, "monster") && cfg_num_monsters < WOOD_MAX_MONSTERS)
    {
      int num, cost, min_wave;
      if (sscanf(rest, "%d %d %d", &num, &cost, &min_wave) == 3 && cost > 0)
      {
        mobjtype_t t = Wood_TypeForDoomednum(num);
        if (t != NUMMOBJTYPES)
        {
          cfg_monsters[cfg_num_monsters].type = t;
          cfg_monsters[cfg_num_monsters].cost = cost;
          cfg_monsters[cfg_num_monsters].min_wave = min_wave;
          cfg_num_monsters++;
        }
      }
    }
    else if (!strcmp(key, "weapons"))
      Wood_ParseTypeList(rest, cfg_weapons, &cfg_num_weapons, WOOD_MAX_WEAPONS);
    else if (!strcmp(key, "reward"))
      Wood_ParseTypeList(rest, cfg_rewards, &cfg_num_rewards, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "gate_reward"))
      Wood_ParseTypeList(rest, cfg_gate_rewards, &cfg_num_gate_rewards, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "boss"))
      Wood_ParseTypeList(rest, cfg_bosses, &cfg_num_bosses, WOOD_MAX_BOSSES);
    else if (!strcmp(key, "boss_escorts"))
      cfg_boss_escorts = BETWEEN(0, 400, atoi(rest));
    else if (!strcmp(key, "music_calm") || !strcmp(key, "music_battle"))
    {
      dboolean calm = key[6] == 'c';
      char buf[256], *tok;
      strncpy(buf, rest, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = 0;
      for (tok = strtok(buf, " \t\r"); tok; tok = strtok(NULL, " \t\r"))
      {
        int *cnt = calm ? &cfg_num_calm : &cfg_num_battle;
        char (*list)[9] = calm ? cfg_music_calm : cfg_music_battle;
        if (*cnt >= WOOD_MAX_TRACKS || W_CheckNumForName(tok) < 0)
          continue;
        strncpy(list[*cnt], tok, 8);
        list[*cnt][8] = 0;
        (*cnt)++;
      }
    }
    else if (!strcmp(key, "ammo_reward"))
      Wood_ParseTypeList(rest, cfg_ammo_rewards, &cfg_num_ammo_rewards, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "saw_trees"))
      Wood_ParseTypeList(rest, cfg_saw_trees, &cfg_num_saw_trees, WOOD_MAX_SAW_TREES);
    else if (!strcmp(key, "saw_tree_health"))
      cfg_saw_tree_health = MAX(1, atoi(rest));
    else if (!strcmp(key, "saw_stump"))
      cfg_saw_stump = Wood_TypeForDoomednum(atoi(rest));
    else if (!strcmp(key, "saw_drop"))
      Wood_ParseTypeList(rest, cfg_saw_drops, &cfg_num_saw_drops, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "mimic_chance"))
      cfg_mimic_chance = BETWEEN(0, 100, atoi(rest));
    else if (!strcmp(key, "mimic_health"))
      cfg_mimic_health = MAX(1, atoi(rest));
    else if (!strcmp(key, "mimic_wake"))
      cfg_mimic_wake = BETWEEN(0, 512, atoi(rest));
    else if (!strcmp(key, "mimic_damage"))
    {
      int lo, hi;
      if (sscanf(rest, "%d %d", &lo, &hi) == 2 && lo > 0 && hi >= lo)
        cfg_mimic_dmg_min = lo, cfg_mimic_dmg_max = hi;
    }
    else if (!strcmp(key, "mimic_bite_tics"))
      cfg_mimic_bite_tics = MAX(1, atoi(rest));
    else if (!strcmp(key, "mimic_drop"))
      Wood_ParseTypeList(rest, cfg_mimic_drops, &cfg_num_mimic_drops, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "golden_tree"))
      cfg_golden_type = Wood_TypeForDoomednum(atoi(rest));
    else if (!strcmp(key, "golden_chance"))
      cfg_golden_chance = BETWEEN(0, 100, atoi(rest));
    else if (!strcmp(key, "golden_health"))
      cfg_golden_health = MAX(1, atoi(rest));
    else if (!strcmp(key, "golden_drop"))
      Wood_ParseTypeList(rest, cfg_golden_drops, &cfg_num_golden_drops, WOOD_MAX_REWARDS);
  }
  W_UnlockLumpNum(lump);
  lprintf(LO_INFO, "WOOD: %d monster types, %d weapons in WOODCFG\n",
          cfg_num_monsters, cfg_num_weapons);
}

static void Wood_FindClearings(void)
{
  int s, l;
  num_clearings = 0;
  boss_idx = -1;
  for (s = 0; s < numsectors && num_clearings < WOOD_MAX_CLEARINGS; s++)
  {
    sector_t *sec = &sectors[s];
    fixed_t minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    wood_clearing_t *c;
    if (sec->tag <= 0)
      continue;
    for (l = 0; l < sec->linecount; l++)
    {
      line_t *ln = sec->lines[l];
      minx = MIN(minx, MIN(ln->v1->x, ln->v2->x));
      maxx = MAX(maxx, MAX(ln->v1->x, ln->v2->x));
      miny = MIN(miny, MIN(ln->v1->y, ln->v2->y));
      maxy = MAX(maxy, MAX(ln->v1->y, ln->v2->y));
    }
    c = &clearings[num_clearings++];
    c->sector = sec;
    c->cx = minx / 2 + maxx / 2;
    c->cy = miny / 2 + maxy / 2;
    c->radius = MIN(maxx - minx, maxy - miny) / 2;
    c->cleared = false;
    c->boss = sec->tag == WOOD_BOSS_TAG && boss_idx < 0;
    if (c->boss)
      boss_idx = num_clearings - 1;
  }
}

/* Случайный трек из списка, не повторяя предыдущий. */
static void Wood_PlayFrom(char (*list)[9], int count, int *last)
{
  int pick, lump;
  if (count <= 0)
    return;
  pick = Wood_RandRange(0, count - 1);
  if (count > 1 && pick == *last)
    pick = (pick + 1 + Wood_RandRange(0, count - 2)) % count;
  *last = pick;
  lump = W_CheckNumForName(list[pick]);
  if (lump >= 0)
  {
    lprintf(LO_INFO, "WOOD: music %s\n", list[pick]);
    S_ChangeMusInfoMusic(lump, true);
  }
}

static void Wood_CalmMusic(void)
{
  Wood_PlayFrom(cfg_music_calm, cfg_num_calm, &last_calm);
}

static void Wood_BattleMusic(void)
{
  Wood_PlayFrom(cfg_music_battle, cfg_num_battle, &last_battle);
}

static void Wood_Message(const char *msg, int color, int sfx)
{
  SetCustomMessage(consoleplayer, msg, 0, 3 * TICRATE, color, sfx);
}

static mobj_t *Wood_SpawnWithFog(fixed_t x, fixed_t y, mobjtype_t type)
{
  mobj_t *mo = P_SpawnMobj(x, y, ONFLOORZ, type);
  mobj_t *fog = P_SpawnMobj(x, y, mo->z, MT_TFOG);
  S_StartSound(fog, sfx_telept);
  return mo;
}

/* Случайная точка на поляне, не ближе minDist к игроку. Возвращает false, если не нашлось. */
static dboolean Wood_PickSpot(const wood_clearing_t *c, mobj_t *player, fixed_t minDist,
                               fixed_t *ox, fixed_t *oy)
{
  int tries;
  for (tries = 0; tries < 32; tries++)
  {
    angle_t ang = (angle_t)(Wood_Rand() & 0xFFFF) << 16;
    fixed_t dist = FixedMul(c->radius, Wood_RandRange(FRACUNIT / 5, FRACUNIT * 3 / 4));
    fixed_t x = c->cx + FixedMul(dist, finecosine[ang >> ANGLETOFINESHIFT]);
    fixed_t y = c->cy + FixedMul(dist, finesine[ang >> ANGLETOFINESHIFT]);
    if (R_PointInSubsector(x, y)->sector != c->sector)
      continue;
    if (player && P_AproxDistance(x - player->x, y - player->y) < minDist)
      continue;
    *ox = x;
    *oy = y;
    return true;
  }
  return false;
}

/* Ставит факелы вдоль линии: невидимую стену делаем видимой. */
static void Wood_PlaceTorches(wood_fence_t *f, const line_t *ln)
{
  static mobjtype_t torch_type = NUMMOBJTYPES;
  fixed_t len = P_AproxDistance(ln->dx, ln->dy);
  int n, k;

  if (torch_type == NUMMOBJTYPES)
    torch_type = Wood_TypeForDoomednum(WOOD_TORCH_DOOMEDNUM);
  if (torch_type == NUMMOBJTYPES || len <= 0)
    return;
  n = len / WOOD_TORCH_STEP + 1;
  for (k = 0; k <= n && f->num_torches < WOOD_MAX_TORCHES; k++)
  {
    fixed_t x = ln->v1->x + FixedMul(ln->dx, FixedDiv(k, n));
    fixed_t y = ln->v1->y + FixedMul(ln->dy, FixedDiv(k, n));
    f->torches[f->num_torches++] = Wood_SpawnWithFog(x, y, torch_type);
  }
}

static void Wood_SetBarrier(wood_fence_t *f, const wood_clearing_t *c, dboolean on)
{
  int i;
  if (on)
  {
    f->num_lines = 0;
    for (i = 0; i < c->sector->linecount && f->num_lines < WOOD_MAX_BARRIER; i++)
    {
      line_t *ln = c->sector->lines[i];
      if (ln->backsector && !(ln->flags & ML_BLOCKING))
      {
        ln->flags |= ML_BLOCKING;
        f->lines[f->num_lines++] = ln - lines;
        Wood_PlaceTorches(f, ln);
      }
    }
  }
  else
  {
    for (i = 0; i < f->num_lines; i++)
      lines[f->lines[i]].flags &= ~ML_BLOCKING;
    f->num_lines = 0;
    for (i = 0; i < f->num_torches; i++)
    {
      mobj_t *fog = P_SpawnMobj(f->torches[i]->x, f->torches[i]->y, f->torches[i]->z, MT_IFOG);
      if (!i)
        S_StartSound(fog, sfx_itmbk);
      P_RemoveMobj(f->torches[i]);
      f->torches[i] = NULL;
    }
    f->num_torches = 0;
  }
}

/* Спавнит врага волны на поляне и сразу натравливает на игрока. far — подальше от игрока,
 * у противоположного края (для босса). NULL, если места не нашлось. */
static mobj_t *Wood_SpawnFoe(const wood_clearing_t *c, mobjtype_t type, dboolean far)
{
  mobj_t *pmo = players[consoleplayer].mo;
  mobj_t *mo;
  fixed_t x, y;
  int tries;

  for (tries = 0; tries < (far ? 48 : 1); tries++)
  {
    if (far && pmo && tries < 16)
    {
      /* за центром поляны, если смотреть от игрока, с небольшим разбросом */
      angle_t ang = R_PointToAngle2(pmo->x, pmo->y, c->cx, c->cy) +
                    ((angle_t)Wood_RandRange(-0x1000, 0x1000) << 16);
      fixed_t dist = FixedMul(c->radius, Wood_RandRange(FRACUNIT / 4, FRACUNIT / 2));
      x = c->cx + FixedMul(dist, finecosine[ang >> ANGLETOFINESHIFT]);
      y = c->cy + FixedMul(dist, finesine[ang >> ANGLETOFINESHIFT]);
      if (R_PointInSubsector(x, y)->sector != c->sector)
        continue;
    }
    else if (!Wood_PickSpot(c, pmo, (far ? 512 : 320) * FRACUNIT, &x, &y))
      continue;
    mo = P_SpawnMobj(x, y, ONFLOORZ, type);
    if (!P_CheckPosition(mo, x, y))
    {
      P_RemoveMobj(mo);
      continue;
    }
    {
      mobj_t *fog = P_SpawnMobj(x, y, mo->z, MT_TFOG);
      S_StartSound(fog, sfx_telept);
    }
    /* сразу в бой: без засады, цель — игрок */
    mo->flags &= ~MF_AMBUSH;
    if (pmo)
    {
      P_SetTarget(&mo->target, pmo);
      mo->angle = R_PointToAngle2(x, y, pmo->x, pmo->y);
      if (mo->info->seestate)
        P_SetMobjState(mo, mo->info->seestate);
    }
    if (mo->flags & MF_COUNTKILL)
      totalkills++;
    P_SetTarget(&wave[wave_size++], mo);
    return mo;
  }
  return NULL;
}

static void Wood_StartWave(int idx)
{
  wood_clearing_t *c = &clearings[idx];
  int budget = cfg_budget_base + cfg_budget_step * waves_cleared;
  int guard = 0;
  dboolean boss_spawned = false;
  char msg[64];

  current = idx;
  wave_size = 0;
  Wood_SetBarrier(&fence, c, true);

  if (c->boss)
  {
    if (cfg_num_bosses)
      boss_spawned = Wood_SpawnFoe(c, cfg_bosses[Wood_RandRange(0, cfg_num_bosses - 1)], true) != NULL;
    budget = budget * cfg_boss_escorts / 100;
  }

  while (budget > 0 && wave_size < cfg_max_monsters && guard++ < 256)
  {
    int i, pick, choices = 0;
    int candidates[WOOD_MAX_MONSTERS];

    for (i = 0; i < cfg_num_monsters; i++)
      if (cfg_monsters[i].min_wave <= waves_cleared && cfg_monsters[i].cost <= budget)
        candidates[choices++] = i;
    if (!choices)
      break;
    pick = candidates[Wood_RandRange(0, choices - 1)];
    if (Wood_SpawnFoe(c, cfg_monsters[pick].type, false))
      budget -= cfg_monsters[pick].cost;
  }

  if (!wave_size)
  {
    /* не нашлось места — считаем поляну пустой, чтобы не запереть игрока */
    Wood_SetBarrier(&fence, c, false);
    current = -1;
    c->cleared = true;
    return;
  }

  lprintf(LO_INFO, "WOOD: wave %d at clearing %d%s, %d monsters\n",
          waves_cleared + 1, idx + 1, c->boss ? " (boss)" : "", wave_size);
  if (boss_spawned)
    Wood_Message("THE KEEPER OF THE WOOD AWAKES!", CR_RED, sfx_bossit);
  else
  {
    sprintf(msg, "WAVE %d: %d FOES", waves_cleared + 1, wave_size);
    Wood_Message(msg, CR_RED, sfx_bossit);
  }
  Wood_BattleMusic();
}

static void Wood_GiveRewards(const wood_clearing_t *c)
{
  int i;
  fixed_t x, y;
  mobj_t *pmo = players[consoleplayer].mo;

  /* оружие — в центр поляны, остальное — вокруг */
  if (waves_cleared - 1 < cfg_num_weapons)
    Wood_SpawnWithFog(c->cx, c->cy, cfg_weapons[waves_cleared - 1]);
  else if (cfg_num_ammo_rewards)
    Wood_SpawnWithFog(c->cx, c->cy, cfg_ammo_rewards[Wood_RandRange(0, cfg_num_ammo_rewards - 1)]);

  for (i = 0; i < cfg_num_rewards; i++)
    if (Wood_PickSpot(c, pmo, 0, &x, &y))
      Wood_SpawnWithFog(x, y, cfg_rewards[i]);
}

/* Снимок итогов забега для экрана смерти и финального экрана. */
static void Wood_TakeSummary(const player_t *p)
{
  int i;
  summary.tics = leveltime;
  summary.kills = p->killcount;
  summary.trees = trees_felled;
  summary.cleared = 0;
  for (i = 0; i < num_clearings; i++)
    if (clearings[i].cleared)
      summary.cleared++;
  summary.clearings = num_clearings;
}

/* Победа над боссом: короткая пауза, потом финальный экран (F_StartFinale → WOOD_VictoryText). */
static void Wood_Victory(void)
{
  run_won = true;
  victory_tics = WOOD_VICTORY_DELAY;
  Wood_TakeSummary(&players[consoleplayer]);
  lprintf(LO_INFO, "WOOD: run won after %d waves\n", waves_cleared);
  Wood_Message("THE WOOD FALLS SILENT", CR_GOLD, sfx_secret);
}

static void Wood_OpenGate(const wood_clearing_t *from)
{
  int i;
  Wood_SetBarrier(&gate, &clearings[boss_idx], false);
  for (i = 0; i < cfg_num_gate_rewards; i++)
  {
    fixed_t x, y;
    if (Wood_PickSpot(from, players[consoleplayer].mo, 0, &x, &y))
      Wood_SpawnWithFog(x, y, cfg_gate_rewards[i]);
  }
  lprintf(LO_INFO, "WOOD: boss lair open\n");
  Wood_Message("THE HEART OF THE WOOD OPENS", CR_GOLD, sfx_secret);
}

static void Wood_EndWave(void)
{
  wood_clearing_t *c = &clearings[current];
  int i, left = 0;
  char msg[64];

  for (i = 0; i < wave_size; i++)
    P_SetTarget(&wave[i], NULL);
  wave_size = 0;
  Wood_SetBarrier(&fence, c, false);
  c->cleared = true;
  current = -1;
  waves_cleared++;

  if (c->boss)
  {
    Wood_Victory();
    return;
  }

  Wood_GiveRewards(c);
  Wood_CalmMusic();
  lprintf(LO_INFO, "WOOD: wave %d cleared\n", waves_cleared);

  for (i = 0; i < num_clearings; i++)
    if (!clearings[i].cleared && !clearings[i].boss)
      left++;

  if (left)
  {
    sprintf(msg, "CLEARING CLEARED. %d LEFT", left);
    Wood_Message(msg, CR_GREEN, sfx_getpow);
  }
  else if (boss_idx >= 0)
    Wood_OpenGate(c);
  else
    Wood_Victory();   /* на карте нет логова — забег кончается на последней поляне */
}

/* ------------------------------------------------------------------------ */
/* Пила по деревьям                                                         */
/* ------------------------------------------------------------------------ */

static dboolean Wood_IsSawTree(const mobj_t *mo)
{
  int i;
  if (mo->type == cfg_golden_type)
    return true;
  for (i = 0; i < cfg_num_saw_trees; i++)
    if (mo->type == cfg_saw_trees[i])
      return true;
  return false;
}

/* Деревья не стреляемые, их health движок не трогает — храним в нём «прочность». */
static void Wood_InitTrees(void)
{
  thinker_t *th;
  int n = 0;
  for (th = thinkercap.next; th != &thinkercap; th = th->next)
  {
    mobj_t *mo;
    if (th->function != P_MobjThinker)
      continue;
    mo = (mobj_t *)th;
    if (Wood_IsSawTree(mo))
    {
      mo->health = cfg_saw_tree_health;
      n++;
    }
  }
  lprintf(LO_INFO, "WOOD: %d sawable trees\n", n);
}

static void Wood_InitRedTranslation(void);

static wood_mimic_t *Wood_FindMimic(const mobj_t *mo)
{
  int i;
  for (i = 0; i < num_mimics; i++)
    if (mimics[i].mo == mo)
      return &mimics[i];
  return NULL;
}

/* Золотое дерево: одно случайное дерево подальше от старта заменяем золотым.
 * Мимики: каждое дерево подальше от старта с шансом mimic_chance. */
static void Wood_PlantSpecialTrees(void)
{
  mobj_t *pmo = players[consoleplayer].mo;
  static mobj_t *cand[4096];
  int i = 0, n = 0;
  thinker_t *th;

  num_mimics = 0;
  golden_hint = false;
  Wood_InitRedTranslation();
  if (!pmo)
    return;

  for (th = thinkercap.next; th != &thinkercap && n < (int)(sizeof(cand) / sizeof(*cand)); th = th->next)
  {
    mobj_t *mo;
    if (th->function != P_MobjThinker)
      continue;
    mo = (mobj_t *)th;
    if (Wood_IsSawTree(mo) && mo->type != cfg_golden_type &&
        P_AproxDistance(mo->x - pmo->x, mo->y - pmo->y) > WOOD_MIMIC_SAFE)
      cand[n++] = mo;
  }

  if (cfg_golden_type != NUMMOBJTYPES && n && Wood_RandRange(1, 100) <= cfg_golden_chance)
  {
    int tries;
    for (tries = 0; tries < 32; tries++)
    {
      i = Wood_RandRange(0, n - 1);
      if (P_AproxDistance(cand[i]->x - pmo->x, cand[i]->y - pmo->y) > WOOD_GOLDEN_SAFE)
        break;
    }
    {
      mobj_t *old = cand[i];
      mobj_t *gold = P_SpawnMobj(old->x, old->y, ONFLOORZ, cfg_golden_type);
      gold->radius = old->radius;          /* ствол такой же толщины, как у дерева на его месте */
      gold->health = cfg_golden_health;
      gold->frame |= FF_FULLBRIGHT;        /* светится в тёмном лесу */
      lprintf(LO_INFO, "WOOD: golden tree at %d,%d\n", old->x >> FRACBITS, old->y >> FRACBITS);
      P_RemoveMobj(old);
      cand[i] = cand[--n];
      golden_hint = true;
    }
  }

  /* мимики — случайная выборка из оставшихся деревьев (частичное перемешивание) */
  {
    int want = MIN(WOOD_MAX_MIMICS, n * cfg_mimic_chance / 100);
    for (i = 0; i < want; i++)
    {
      int j = Wood_RandRange(i, n - 1);
      mobj_t *t = cand[j];
      wood_mimic_t *m = &mimics[num_mimics++];
      cand[j] = cand[i];
      cand[i] = t;
      m->mo = t;
        m->awake = false;
        m->cooldown = Wood_RandRange(0, TICRATE * 2);
      m->mo->health = cfg_mimic_health;
    }
  }
  lprintf(LO_INFO, "WOOD: %d mimic trees\n", num_mimics);
}

/* Третья таблица перекраски (в одиночной игре её никто не использует) — «кровавая»:
 * любой цвет -> красный той же яркости. Иначе покраснела бы только зелёная листва,
 * а у голых деревьев (TRE1, TRE2) её нет. */
static void Wood_InitRedTranslation(void)
{
  static dboolean done;
  const byte *pal;
  int i, j;
  if (done || !translationtables)
    return;
  done = true;
  pal = W_CacheLumpName("PLAYPAL");
  for (i = 0; i < 256; i++)
  {
    int lum = (pal[i * 3] * 30 + pal[i * 3 + 1] * 59 + pal[i * 3 + 2] * 11) / 100;
    int r = MIN(255, 60 + lum * 2), g = lum / 4, b = lum / 5;
    int best = i, best_d = INT_MAX;
    for (j = 0; j < 256; j++)
    {
      int dr = pal[j * 3] - r, dg = pal[j * 3 + 1] - g, db = pal[j * 3 + 2] - b;
      int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
      if (d < best_d)
        best_d = d, best = j;
    }
    translationtables[512 + i] = (byte)best;
  }
  W_UnlockLumpName("PLAYPAL");
}

static void Wood_WakeMimic(wood_mimic_t *m)
{
  if (m->awake)
    return;
  m->awake = true;
  m->cooldown = TICRATE / 3;                 /* короткая пауза — и кусает */
  m->mo->flags = (m->mo->flags & ~MF_TRANSLATION) | WOOD_MIMIC_RED;
  S_StartSound(m->mo, sfx_sgtsit);
  if (!mimic_warned)
  {
    mimic_warned = true;
    Wood_Message("THE TREE IS ALIVE!", CR_RED, 0);
  }
  lprintf(LO_INFO, "WOOD: mimic awake\n");
}

/* Мимики: спящие шуршат, когда игрок рядом, и просыпаются вплотную; проснувшиеся кусают. */
static void Wood_MimicThink(player_t *p)
{
  mobj_t *pmo = p->mo;
  int i;

  for (i = 0; i < num_mimics; i++)
  {
    wood_mimic_t *m = &mimics[i];
    fixed_t edge;
    if (!m->mo)
      continue;
    if (m->cooldown > 0)
      m->cooldown--;
    edge = P_AproxDistance(m->mo->x - pmo->x, m->mo->y - pmo->y) - m->mo->radius - pmo->radius;

    if (!m->awake)
    {
      if (edge < cfg_mimic_wake * FRACUNIT && P_CheckSight(m->mo, pmo))
        Wood_WakeMimic(m);
      else if (edge < WOOD_MIMIC_RUSTLE && !m->cooldown)
      {
        m->cooldown = Wood_RandRange(TICRATE * 2, TICRATE * 4);
        S_StartSound(m->mo, sfx_dmact);    /* подозрительное шуршание */
      }
      continue;
    }

    if (m->cooldown || edge > WOOD_MIMIC_REACH || pmo->z > m->mo->z + m->mo->height ||
        !P_CheckSight(m->mo, pmo))
      continue;
    m->cooldown = cfg_mimic_bite_tics;
    S_StartSound(m->mo, sfx_sgtatk);
    /* без inflictor — без отбрасывания, иначе укус выталкивает игрока из-под пилы */
    P_DamageMobj(pmo, NULL, m->mo, Wood_RandRange(cfg_mimic_dmg_min, cfg_mimic_dmg_max));
  }
}

/* Тип патронов предмета-ящика (по спрайту), или NUMAMMO. */
static ammotype_t Wood_AmmoOfItem(mobjtype_t t)
{
  switch (states[mobjinfo[t].spawnstate].sprite)
  {
    case SPR_CLIP: case SPR_AMMO: return am_clip;
    case SPR_SHEL: case SPR_SBOX: return am_shell;
    case SPR_ROCK: case SPR_BROK: return am_misl;
    case SPR_CELL: case SPR_CELP: return am_cell;
    default: return NUMAMMO;
  }
}

/* Что выпадет из дерева: патроны к оружию, которое у игрока есть (кроме пилы и кулака).
 * Если стрелять пока не из чего — любые из списка, пригодятся. */
static mobjtype_t Wood_PickSawDrop(const player_t *p)
{
  mobjtype_t useful[WOOD_MAX_REWARDS];
  int i, w, n = 0;
  if (!cfg_num_saw_drops)
    return NUMMOBJTYPES;
  for (i = 0; i < cfg_num_saw_drops; i++)
  {
    ammotype_t a = Wood_AmmoOfItem(cfg_saw_drops[i]);
    for (w = 0; w < NUMWEAPONS; w++)
      if (p->weaponowned[w] && weaponinfo[w].ammo == a && a != am_noammo)
      {
        useful[n++] = cfg_saw_drops[i];
        break;
      }
  }
  if (n)
    return useful[Wood_RandRange(0, n - 1)];
  return cfg_saw_drops[Wood_RandRange(0, cfg_num_saw_drops - 1)];
}

static void Wood_FellTree(player_t *p, mobj_t *tree)
{
  fixed_t x = tree->x, y = tree->y;
  angle_t away = R_PointToAngle2(p->mo->x, p->mo->y, x, y);
  mobjtype_t drop = Wood_PickSawDrop(p);
  mobjtype_t bonus = NUMMOBJTYPES;
  wood_mimic_t *mimic = Wood_FindMimic(tree);
  dboolean golden = tree->type == cfg_golden_type;
  mobj_t *mo;

  if (mimic)
  {
    mimic->mo = NULL;
    if (cfg_num_mimic_drops)
      bonus = cfg_mimic_drops[Wood_RandRange(0, cfg_num_mimic_drops - 1)];
    S_StartSound(NULL, sfx_sgtdth);   /* предсмертный рёв (без источника — дерево исчезнет) */
  }
  else if (golden)
  {
    if (cfg_num_golden_drops)
      bonus = cfg_golden_drops[Wood_RandRange(0, cfg_num_golden_drops - 1)];
    S_StartSound(NULL, sfx_getpow);
  }
  else
    S_StartSound(tree, sfx_bdcls);   /* глухой удар — дерево падает */
  P_RemoveMobj(tree);
  trees_felled++;
  if (cfg_saw_stump != NUMMOBJTYPES)
    P_SpawnMobj(x, y, ONFLOORZ, cfg_saw_stump);

  if (drop != NUMMOBJTYPES)
  {
    /* патроны кладём между пнём и игроком, чтобы не застряли за стволом */
    fixed_t back = 24 * FRACUNIT;
    mo = P_SpawnMobj(x - FixedMul(back, finecosine[away >> ANGLETOFINESHIFT]),
                     y - FixedMul(back, finesine[away >> ANGLETOFINESHIFT]),
                     ONFLOORZ, drop);
    mo->flags |= MF_DROPPED;   /* как из убитого врага: половина обычного */
    P_SpawnMobj(mo->x, mo->y, mo->z, MT_IFOG);
  }
  if (bonus != NUMMOBJTYPES)
  {
    /* награда — на месте ствола */
    mo = P_SpawnMobj(x, y, ONFLOORZ, bonus);
    P_SpawnMobj(x, y, mo->z, golden ? MT_TFOG : MT_IFOG);
  }
  lprintf(LO_INFO, "WOOD: tree felled%s, drop %d, bonus %d\n",
          mimic ? " (mimic)" : golden ? " (golden)" : "", drop, bonus);
  if (golden)
    Wood_Message("GOLDEN TIMBER!", CR_GOLD, 0);
  else if (mimic)
    Wood_Message("THE MIMIC IS FELLED", CR_GREEN, 0);
  else if (current < 0)
    Wood_Message("TIMBER!", CR_GOLD, 0);
}

dboolean WOOD_SawTree(player_t *p, angle_t angle, int damage)
{
  thinker_t *th;
  mobj_t *best = NULL;
  fixed_t best_dist = INT_MAX;
  mobj_t *pmo = p->mo;

  if (!wood_active || !cfg_num_saw_trees || !pmo)
    return false;

  for (th = thinkercap.next; th != &thinkercap; th = th->next)
  {
    mobj_t *mo;
    fixed_t dist;
    angle_t diff;
    if (th->function != P_MobjThinker)
      continue;
    mo = (mobj_t *)th;
    if (!Wood_IsSawTree(mo))
      continue;
    dist = P_AproxDistance(mo->x - pmo->x, mo->y - pmo->y);
    if (dist - mo->radius > MELEERANGE + 8 * FRACUNIT || dist >= best_dist)
      continue;
    diff = R_PointToAngle2(pmo->x, pmo->y, mo->x, mo->y) - angle;
    if (diff > ANG45 / 2 && diff < (angle_t)-(ANG45 / 2))
      continue;
    best = mo;
    best_dist = dist;
  }
  if (!best || !P_CheckSight(pmo, best))
    return false;

  /* опилки из ствола со стороны игрока */
  {
    angle_t a = R_PointToAngle2(best->x, best->y, pmo->x, pmo->y);
    P_SpawnPuff(best->x + FixedMul(best->radius, finecosine[a >> ANGLETOFINESHIFT]),
                best->y + FixedMul(best->radius, finesine[a >> ANGLETOFINESHIFT]),
                pmo->z + 32 * FRACUNIT);
  }

  {
    wood_mimic_t *m = Wood_FindMimic(best);
    if (m)
      Wood_WakeMimic(m);             /* пилить мимика — разбудить его */
  }
  best->health -= damage;
  if (best->health <= 0)
    Wood_FellTree(p, best);
  return true;
}

static void Wood_SetupPlayer(player_t *p)
{
  int i;
  for (i = 0; i < NUMWEAPONS; i++)
    p->weaponowned[i] = false;
  for (i = 0; i < NUMAMMO; i++)
    p->ammo[i] = 0;
  p->weaponowned[wp_chainsaw] = true;
  p->readyweapon = wp_chainsaw;
  P_SetupPsprites(p);
}

/* Экран смерти: сколько тиков игрок мёртв и отпущена ли кнопка после смерти
 * (чтобы пробел, зажатый в бою, не перезапускал забег мгновенно). */
#define WOOD_AGAIN_DELAY (TICRATE * 3 / 4)
static int dead_tics;
static dboolean again_armed;

int WOOD_DeathThink(player_t *player)
{
  if (!dead_tics)
    Wood_TakeSummary(player);
  if (dead_tics < WOOD_AGAIN_DELAY)
  {
    dead_tics++;
    return false;
  }
  if (!(player->cmd.buttons & BT_USE))
  {
    again_armed = true;
    return false;
  }
  return again_armed;
}

extern patchnum_t hu_font[HU_FONTSIZE];

/* Строка мелким шрифтом HUD с тенью; align: 0 — от x вправо, 1 — до x (выравнивание вправо). */
static int Wood_TextWidth(const char *str)
{
  int w = 0, c;
  for (; *str; str++)
  {
    c = toupper((unsigned char)*str) - HU_FONTSTART;
    w += (c < 0 || c >= HU_FONTSIZE) ? 4 : hu_font[c].width;
  }
  return w;
}

static void Wood_Text(int x, int y, const char *str, int cm, int align)
{
  int c, pass;
  const char *ch;
  if (align)
    x -= Wood_TextWidth(str);
  for (pass = 0; pass < 2; pass++)
  {
    int cx = x + (pass ? 0 : 1), cy = y + (pass ? 0 : 1);
    for (ch = str; *ch; ch++)
    {
      c = toupper((unsigned char)*ch) - HU_FONTSTART;
      if (c < 0 || c >= HU_FONTSIZE)
      {
        cx += 4;
        continue;
      }
      V_DrawNumPatch(cx, cy, 0, hu_font[c].lumpnum, pass ? cm : CR_BLACK,
                     VPT_STRETCH | VPT_TRANS);
      cx += hu_font[c].width;
    }
  }
}

/* Экран итогов забега поверх мёртвого игрока: «Again?», цифры забега, подсказка с клавишами. */
#define WOOD_SUMMARY_LINE  11
#define WOOD_SUMMARY_HALF  64     /* полуширина таблицы итогов */
void WOOD_Drawer(void)
{
  static int lump_again = -2, lump_keys = -2;
  player_t *p = &players[displayplayer];
  int again_h = 0, keys_h = 0, y, secs;
  char buf[32];

  if (!wood_active || gamestate != GS_LEVEL || p->playerstate != PST_DEAD ||
      dead_tics < WOOD_AGAIN_DELAY || menuactive)
    return;
  if (lump_again == -2)
  {
    lump_again = W_CheckNumForName("WAGAIN");
    lump_keys = W_CheckNumForName("WAGAINK");
  }
  if (lump_again >= 0)
    again_h = R_NumPatchHeight(lump_again);
  if (lump_keys >= 0)
    keys_h = R_NumPatchHeight(lump_keys);

  /* всё по центру области над строкой состояния (0..168) */
  y = (168 - (again_h + 6 + 4 * WOOD_SUMMARY_LINE + 6 + keys_h)) / 2;
  if (y < 2)
    y = 2;

  if (lump_again >= 0)
    V_DrawNumPatch((320 - R_NumPatchWidth(lump_again)) / 2, y, 0, lump_again,
                   CR_DEFAULT, VPT_STRETCH);
  y += again_h + 6;

  secs = summary.tics / TICRATE;
  sprintf(buf, "%d:%02d", secs / 60, secs % 60);
  Wood_Text(160 - WOOD_SUMMARY_HALF, y, "TIME", CR_TAN, 0);
  Wood_Text(160 + WOOD_SUMMARY_HALF, y, buf, CR_GOLD, 1);
  y += WOOD_SUMMARY_LINE;
  sprintf(buf, "%d", summary.kills);
  Wood_Text(160 - WOOD_SUMMARY_HALF, y, "FOES FELLED", CR_TAN, 0);
  Wood_Text(160 + WOOD_SUMMARY_HALF, y, buf, CR_GOLD, 1);
  y += WOOD_SUMMARY_LINE;
  sprintf(buf, "%d", summary.trees);
  Wood_Text(160 - WOOD_SUMMARY_HALF, y, "TREES CUT DOWN", CR_TAN, 0);
  Wood_Text(160 + WOOD_SUMMARY_HALF, y, buf, CR_GOLD, 1);
  y += WOOD_SUMMARY_LINE;
  sprintf(buf, "%d/%d", summary.cleared, summary.clearings);
  Wood_Text(160 - WOOD_SUMMARY_HALF, y, "CLEARINGS", CR_TAN, 0);
  Wood_Text(160 + WOOD_SUMMARY_HALF, y, buf, CR_GOLD, 1);
  y += WOOD_SUMMARY_LINE + 6;

  /* подсказка мигает: секунду видна, полсекунды нет */
  if (lump_keys >= 0 && (gametic % (TICRATE * 3 / 2)) < TICRATE)
    V_DrawNumPatch((320 - R_NumPatchWidth(lump_keys)) / 2, y, 0, lump_keys,
                   CR_DEFAULT, VPT_STRETCH);
}

void WOOD_LevelStart(void)
{
  int lump = W_CheckNumForName("WOODCFG");
  int i;

  /* старые указатели принадлежат выгруженному уровню — просто забываем их */
  for (i = 0; i < WOOD_MAX_WAVE; i++)
    wave[i] = NULL;
  wave_size = 0;
  memset(&fence, 0, sizeof(fence));
  memset(&gate, 0, sizeof(gate));
  current = -1;
  waves_cleared = 0;
  victory_tics = 0;
  run_won = false;
  trees_felled = 0;
  memset(&summary, 0, sizeof(summary));
  music_started = false;
  dead_tics = 0;
  again_armed = false;
  for (i = 0; i < WOOD_MAX_MIMICS; i++)
    mimics[i].mo = NULL;
  num_mimics = 0;
  mimic_warned = false;
  golden_hint = false;

  wood_active = lump >= 0;
  if (!wood_active)
    return;

  Wood_ParseConfig(lump);
  Wood_FindClearings();
  Wood_InitTrees();
  Wood_Seed();
  Wood_PlantSpecialTrees();

  if (playeringame[consoleplayer] && players[consoleplayer].mo)
    Wood_SetupPlayer(&players[consoleplayer]);

  /* логово заперто, пока не зачищены остальные поляны */
  if (boss_idx >= 0)
    Wood_SetBarrier(&gate, &clearings[boss_idx], true);

  lprintf(LO_INFO, "WOOD: director active, %d clearings%s\n", num_clearings,
          boss_idx >= 0 ? " + boss lair" : "");
}

void WOOD_Ticker(void)
{
  player_t *p;
  sector_t *sec;
  int i;

  if (!wood_active || gamestate != GS_LEVEL)
    return;

  if (victory_tics > 0)
  {
    if (!--victory_tics)
      gameaction = ga_victory;
    return;
  }
  if (run_won)
    return;

  p = &players[consoleplayer];
  if (!p->mo || p->playerstate != PST_LIVE)
    return;

  /* музыку уровня движок включает после P_SetupLevel, поэтому перебиваем её на первом тике */
  if (!music_started)
  {
    music_started = true;
    Wood_CalmMusic();
    if (golden_hint)
      Wood_Message("SOMETHING GLITTERS IN THE WOOD...", CR_GOLD, 0);
  }

  Wood_MimicThink(p);

  if (current >= 0)
  {
    int alive = 0;
    for (i = 0; i < wave_size; i++)
      if (wave[i] && wave[i]->health > 0)
        alive++;
    if (!alive)
      Wood_EndWave();
    return;
  }

  sec = p->mo->subsector->sector;
  for (i = 0; i < num_clearings; i++)
    if (clearings[i].sector == sec && !clearings[i].cleared &&
        (!clearings[i].boss || !gate.num_lines) &&
        /* ждём, пока игрок целиком зайдёт на поляну, иначе барьер может его зажать */
        P_AproxDistance(p->mo->x - clearings[i].cx, p->mo->y - clearings[i].cy) <
          FixedMul(clearings[i].radius, FRACUNIT * 17 / 20))
    {
      Wood_StartWave(i);
      break;
    }
}

int WOOD_Enabled(void)
{
  static int enabled = -1;
  if (enabled < 0)
    enabled = W_CheckNumForName("WOODCFG") >= 0;
  return enabled;
}

int WOOD_PickRunMap(int current)
{
  int count = 0, pick;
  char name[9];
  while (count < 99)
  {
    sprintf(name, "MAP%02d", count + 1);
    if (W_CheckNumForName(name) < 0)
      break;
    count++;
  }
  if (count <= 1)
    return 1;
  pick = Wood_RandRange(1, count);
  if (pick == current)
    pick = pick % count + 1;
  lprintf(LO_INFO, "WOOD: new run on MAP%02d of %d layouts\n", pick, count);
  return pick;
}

int WOOD_RunWon(void)
{
  return wood_active && run_won;
}

const char *WOOD_VictoryText(void)
{
  static char text[512];
  int secs = summary.tics / TICRATE;
  sprintf(text,
          "THE KEEPER OF THE WOOD HAS FALLEN.\n"
          "THE TREES GO QUIET... FOR NOW.\n"
          "\n"
          "TIME IN THE WOOD: %d:%02d\n"
          "FOES FELLED: %d\n"
          "TREES CUT DOWN: %d\n"
          "CLEARINGS TAMED: %d/%d\n"
          "\n"
          "PRESS FIRE TO LEAVE THE FOREST.",
          secs / 60, secs % 60, summary.kills, summary.trees,
          summary.cleared, summary.clearings);
  return text;
}

void WOOD_EndRun(void)
{
  run_won = false;
}

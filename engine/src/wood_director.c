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
 *  - у каждой поляны свой тип награды (оружие, припасы, тайник с патронами, элитная волна);
 *    у входов на тропы висят полупрозрачные «призраки» наград — игрок сам выбирает маршрут;
 *  - после зачистки поляны игра замирает и предлагает 3 перка на выбор (WOOD_Responder,
 *    WOOD_Drawer); перки живут до конца забега.
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
#include "p_inter.h"
#include "d_event.h"
#include "wood_director.h"

/* m_menu.c */
void M_WriteText(int x, int y, const char *string, int cm);
int  M_StringWidth(const char *string);

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

/* Тип награды поляны: виден заранее по «призраку» у входа на тропу. */
typedef enum {
  ROUTE_NONE,          /* логово босса */
  ROUTE_WEAPON,        /* новое оружие (призрак — само это оружие) */
  ROUTE_SUPPLY,        /* аптечки и броня */
  ROUTE_CACHE,         /* рюкзак и ящики патронов */
  ROUTE_ELITE,         /* элитная волна: тяжелее, но сундук и лишний перк; не обязательна */
} wood_route_t;

typedef struct {
  sector_t *sector;
  fixed_t cx, cy;      /* центр (по габаритам) */
  fixed_t radius;      /* половина меньшей стороны габаритов */
  dboolean cleared;    /* зачищена */
  dboolean boss;       /* логово босса */
  wood_route_t route;  /* что за зачистку */
  mobjtype_t weapon;   /* для ROUTE_WEAPON — какое оружие */
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

/* --- типы полян (развилки) --- */
static mobjtype_t cfg_supply_rewards[WOOD_MAX_REWARDS];
static int cfg_num_supply_rewards;
static mobjtype_t cfg_cache_rewards[WOOD_MAX_REWARDS];
static int cfg_num_cache_rewards;
static mobjtype_t cfg_elite_rewards[WOOD_MAX_REWARDS];
static int cfg_num_elite_rewards;
static int cfg_elite_count = 2;       /* элитных полян на карту */
static int cfg_elite_budget = 160;    /* бюджет элитной волны, % от обычной */
static int cfg_elite_tier = 2;        /* элитная волна берёт врагов на столько волн «вперёд» */

/* --- перки --- */
typedef enum {
  PK_SHARP_CHAIN, PK_SAP_DRINKER, PK_BLOOD_SAP, PK_BUCKSHOT, PK_HEAVY_LEAD,
  PK_THICK_BARK, PK_DEEP_POCKETS, PK_LUMBERJACK, PK_BARK_ARMOR, PK_SECOND_WIND,
  NUMPERKS
} wood_perk_id_t;

typedef struct {
  const char *id;      /* имя в WOODCFG */
  const char *name;    /* на карточке (шрифт Doom: только латиница) */
  const char *desc;
  int max_level;
} wood_perk_t;

static const wood_perk_t perk_info[NUMPERKS] = {
  { "sharp_chain",  "SHARP CHAIN",  "CHAINSAW DEALS +100% DAMAGE",      2 },
  { "sap_drinker",  "SAP DRINKER",  "CHAINSAW HITS HEAL 1 HP",          2 },
  { "blood_sap",    "BLOOD SAP",    "EVERY KILL HEALS 4 HP",            3 },
  { "buckshot",     "BUCKSHOT",     "SHOTGUNS DEAL +30% DAMAGE",        3 },
  { "heavy_lead",   "HEAVY LEAD",   "ALL GUNS DEAL +15% DAMAGE",        3 },
  { "thick_bark",   "THICK BARK",   "TAKE 15% LESS DAMAGE",             3 },
  { "deep_pockets", "DEEP POCKETS", "DOUBLE AMMO CAPACITY + AMMO",      1 },
  { "lumberjack",   "LUMBERJACK",   "TREES FALL FAST, DROP TWICE",      1 },
  { "bark_armor",   "BARK ARMOR",   "+50 ARMOR RIGHT NOW",              3 },
  { "second_wind",  "SECOND WIND",  "FULL HEALTH AFTER EVERY CLEARING", 1 },
};

#define WOOD_MAX_CHOICES 3
static dboolean cfg_perk_enabled[NUMPERKS];
static int cfg_num_perks;             /* 0 — перков нет (нет строки perks в WOODCFG) */
static int perk_level[NUMPERKS];      /* взятые перки за этот забег */
static int perk_picks;                /* сколько выборов ещё впереди (после элиты — два) */
static dboolean choice_open;          /* открыт выбор — игра на паузе */
static int choice[WOOD_MAX_CHOICES], num_choices, choice_sel;
static int choice_tic;                /* gametic открытия: короткая пауза против случайного нажатия */
#define WOOD_CHOICE_ARM (TICRATE / 2)
static int cfg_perk_delay = 1500;     /* мс от смерти последнего врага до выбора перка */
static int choice_delay;              /* тиков до открытия выбора, 0 — не ждём */
static int last_kills;                /* для BLOOD SAP: сколько убийств было на прошлом тике */

/* --- призраки наград у входов на тропы --- */
#define WOOD_MAX_ICONS 96
typedef struct {
  mobj_t *mo;
  int clearing;        /* чья это награда */
  fixed_t z;           /* высота парения */
} wood_icon_t;
static wood_icon_t icons[WOOD_MAX_ICONS];
static int num_icons;
static int hint_icon = -1;            /* у какого призрака уже показали подсказку */

static void Wood_Message(const char *msg, int color, int sfx);
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
  cfg_num_supply_rewards = cfg_num_cache_rewards = cfg_num_elite_rewards = 0;
  cfg_num_perks = 0;
  memset(cfg_perk_enabled, 0, sizeof(cfg_perk_enabled));
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
    else if (!strcmp(key, "supply_reward"))
      Wood_ParseTypeList(rest, cfg_supply_rewards, &cfg_num_supply_rewards, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "cache_reward"))
      Wood_ParseTypeList(rest, cfg_cache_rewards, &cfg_num_cache_rewards, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "elite_reward"))
      Wood_ParseTypeList(rest, cfg_elite_rewards, &cfg_num_elite_rewards, WOOD_MAX_REWARDS);
    else if (!strcmp(key, "elite_count"))
      cfg_elite_count = BETWEEN(0, WOOD_MAX_CLEARINGS, atoi(rest));
    else if (!strcmp(key, "elite_budget"))
      cfg_elite_budget = BETWEEN(100, 400, atoi(rest));
    else if (!strcmp(key, "elite_tier"))
      cfg_elite_tier = BETWEEN(0, 10, atoi(rest));
    else if (!strcmp(key, "perk_delay"))
      cfg_perk_delay = BETWEEN(0, 10000, atoi(rest));
    else if (!strcmp(key, "perks"))
    {
      char buf[256], *tok;
      int k;
      strncpy(buf, rest, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = 0;
      for (tok = strtok(buf, " \t\r"); tok; tok = strtok(NULL, " \t\r"))
      {
        for (k = 0; k < NUMPERKS; k++)
          if (!strcmp(tok, perk_info[k].id))
            break;
        if (k == NUMPERKS)
          lprintf(LO_WARN, "WOODCFG: unknown perk %s\n", tok);
        else if (!cfg_perk_enabled[k])
        {
          cfg_perk_enabled[k] = true;
          cfg_num_perks++;
        }
      }
    }
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

/* ------------------------------------------------------------------------ */
/* Развилки: тип награды каждой поляны и «призраки» наград у входов на тропы */
/* ------------------------------------------------------------------------ */

static int Wood_ClearingOf(const sector_t *sec)
{
  int i;
  for (i = 0; i < num_clearings; i++)
    if (clearings[i].sector == sec)
      return i;
  return -1;
}

/* Раздаёт полянам типы наград. Оружия — столько, сколько его в списке, и в порядке силы
 * от старта вглубь леса (теги полян идут по удалённости); ближайшая к старту — всегда оружие.
 * Ещё elite_count элитных, остальные поровну припасы и тайники; порядок случайный. */
static void Wood_AssignRoutes(void)
{
  int order[WOOD_MAX_CLEARINGS];
  wood_route_t types[WOOD_MAX_CLEARINGS];
  int n = 0, i, j, w, e, k = 0;

  for (i = 0; i < num_clearings; i++)
  {
    clearings[i].route = ROUTE_NONE;
    clearings[i].weapon = NUMMOBJTYPES;
    if (clearings[i].boss)
      continue;
    for (j = n; j > 0 && clearings[order[j - 1]].sector->tag > clearings[i].sector->tag; j--)
      order[j] = order[j - 1];
    order[j] = i;
    n++;
  }
  if (!n)
    return;

  w = MIN(cfg_num_weapons, n);
  e = MIN(cfg_elite_count, n - w);
  j = Wood_RandRange(0, 1);
  for (i = 0; i < n; i++)
    types[i] = i < w ? ROUTE_WEAPON : i < w + e ? ROUTE_ELITE :
               ((i + j) & 1) ? ROUTE_CACHE : ROUTE_SUPPLY;
  for (i = n - 1; i > 0; i--)
  {
    wood_route_t t;
    j = Wood_RandRange(0, i);
    t = types[i]; types[i] = types[j]; types[j] = t;
  }
  /* с одной пилой далеко не уйдёшь: первая поляна от старта даёт оружие */
  if (w && types[0] != ROUTE_WEAPON)
    for (i = 1; i < n; i++)
      if (types[i] == ROUTE_WEAPON)
      {
        types[i] = types[0];
        types[0] = ROUTE_WEAPON;
        break;
      }

  for (i = 0; i < n; i++)
  {
    wood_clearing_t *c = &clearings[order[i]];
    c->route = types[i];
    if (types[i] == ROUTE_WEAPON)
      c->weapon = cfg_weapons[k++];
  }
}

/* Чем «призрак» выглядит: само оружие или первый предмет из списка наград. */
static mobjtype_t Wood_RouteIcon(const wood_clearing_t *c)
{
  switch (c->route)
  {
    case ROUTE_WEAPON: return c->weapon;
    case ROUTE_SUPPLY: return cfg_num_supply_rewards ? cfg_supply_rewards[0] : NUMMOBJTYPES;
    case ROUTE_CACHE:  return cfg_num_cache_rewards ? cfg_cache_rewards[0] : NUMMOBJTYPES;
    case ROUTE_ELITE:  return cfg_num_elite_rewards ? cfg_elite_rewards[0] : NUMMOBJTYPES;
    default:           return NUMMOBJTYPES;
  }
}

static const char *Wood_WeaponName(mobjtype_t t)
{
  switch (states[mobjinfo[t].spawnstate].sprite)
  {
    case SPR_CSAW: return "A CHAINSAW";
    case SPR_SHOT: return "A SHOTGUN";
    case SPR_MGUN: return "A CHAINGUN";
    case SPR_SGN2: return "A SUPER SHOTGUN";
    case SPR_LAUN: return "A ROCKET LAUNCHER";
    case SPR_PLAS: return "A PLASMA GUN";
    case SPR_BFUG: return "THE BFG";
    default:       return "A NEW WEAPON";
  }
}

static const char *Wood_RouteHint(const wood_clearing_t *c)
{
  static char buf[64];
  switch (c->route)
  {
    case ROUTE_WEAPON:
      sprintf(buf, "THIS WAY: %s", Wood_WeaponName(c->weapon));
      return buf;
    case ROUTE_SUPPLY: return "THIS WAY: MEDKITS AND ARMOR";
    case ROUTE_CACHE:  return "THIS WAY: AMMO CACHE";
    case ROUTE_ELITE:  return "THIS WAY: ELITE WAVE AND TREASURE";
    default:           return NULL;
  }
}

static void Wood_SpawnIcon(fixed_t x, fixed_t y, mobjtype_t type, int clearing)
{
  mobj_t *mo;
  if (num_icons >= WOOD_MAX_ICONS)
    return;
  mo = P_SpawnMobj(x, y, ONFLOORZ, type);
  /* только картинка: не подобрать, не считается в предметах, парит над тропой */
  mo->flags &= ~(MF_SPECIAL | MF_COUNTITEM);
  mo->flags |= MF_NOGRAVITY | MF_TRANSLUCENT;
  icons[num_icons].mo = mo;
  icons[num_icons].clearing = clearing;
  icons[num_icons].z = mo->floorz + 40 * FRACUNIT;
  mo->z = icons[num_icons].z;
  num_icons++;
}

/* Тропа — сектор без тега ровно с двумя соседями, хотя бы один из которых поляна
 * (старт тоже без тега, но его соседи — тропы). У каждого конца тропы вешаем призрак
 * награды поляны на другом конце: стоя на развилке, видно, что ждёт на каждой тропе. */
static void Wood_PlaceIcons(void)
{
  int s, l, k;
  num_icons = 0;
  hint_icon = -1;
  for (s = 0; s < numsectors; s++)
  {
    sector_t *sec = &sectors[s], *nb[2] = { NULL, NULL };
    int n = 0;
    fixed_t pminx = INT_MAX, pminy = INT_MAX, pmaxx = INT_MIN, pmaxy = INT_MIN, pcx, pcy;

    if (sec->tag)
      continue;
    for (l = 0; l < sec->linecount && n <= 2; l++)
    {
      line_t *ln = sec->lines[l];
      sector_t *o;
      pminx = MIN(pminx, MIN(ln->v1->x, ln->v2->x));
      pmaxx = MAX(pmaxx, MAX(ln->v1->x, ln->v2->x));
      pminy = MIN(pminy, MIN(ln->v1->y, ln->v2->y));
      pmaxy = MAX(pmaxy, MAX(ln->v1->y, ln->v2->y));
      if (!ln->backsector)
        continue;
      o = ln->frontsector == sec ? ln->backsector : ln->frontsector;
      if (o == sec || o == nb[0] || o == nb[1])
        continue;
      if (n < 2)
        nb[n] = o;
      n++;
    }
    if (n != 2 || (Wood_ClearingOf(nb[0]) < 0 && Wood_ClearingOf(nb[1]) < 0))
      continue;
    pcx = pminx / 2 + pmaxx / 2;
    pcy = pminy / 2 + pmaxy / 2;

    for (k = 0; k < 2; k++)
    {
      int to = Wood_ClearingOf(nb[1 - k]);
      fixed_t minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN, mx, my;
      angle_t ang;
      mobjtype_t type;

      if (to < 0 || clearings[to].boss)
        continue;
      type = Wood_RouteIcon(&clearings[to]);
      if (type == NUMMOBJTYPES)
        continue;
      /* середина общего края тропы и сектора nb[k] */
      for (l = 0; l < sec->linecount; l++)
      {
        line_t *ln = sec->lines[l];
        if (ln->backsector && (ln->frontsector == nb[k] || ln->backsector == nb[k]))
        {
          minx = MIN(minx, MIN(ln->v1->x, ln->v2->x));
          maxx = MAX(maxx, MAX(ln->v1->x, ln->v2->x));
          miny = MIN(miny, MIN(ln->v1->y, ln->v2->y));
          maxy = MAX(maxy, MAX(ln->v1->y, ln->v2->y));
        }
      }
      mx = minx / 2 + maxx / 2;
      my = miny / 2 + maxy / 2;
      ang = R_PointToAngle2(mx, my, pcx, pcy) >> ANGLETOFINESHIFT;
      Wood_SpawnIcon(mx + 56 * finecosine[ang], my + 56 * finesine[ang], type, to);
    }
  }
  lprintf(LO_INFO, "WOOD: %d route icons\n", num_icons);
}

/* Поляна зачищена — её призраки тают. */
static void Wood_RemoveIcons(int clearing)
{
  int i;
  for (i = 0; i < num_icons; i++)
    if (icons[i].mo && icons[i].clearing == clearing)
    {
      P_SpawnMobj(icons[i].mo->x, icons[i].mo->y, icons[i].mo->z, MT_IFOG);
      P_RemoveMobj(icons[i].mo);
      icons[i].mo = NULL;
    }
}

/* Призраки покачиваются; у ближайшего — подсказка, что там. */
static void Wood_TickIcons(const mobj_t *pmo)
{
  int i, near = -1;
  fixed_t best = 192 * FRACUNIT;
  dboolean far_from_hint = true;

  for (i = 0; i < num_icons; i++)
  {
    mobj_t *mo = icons[i].mo;
    fixed_t d;
    if (!mo)
      continue;
    mo->z = icons[i].z + 6 * finesine[((leveltime + i * 13) * 96) & FINEMASK];
    mo->frame |= FF_FULLBRIGHT;   /* светится и в тёмной чаще; анимация кадра флаг сбрасывает */
    /* вне боя призрак время от времени искрится — так его видно с середины поляны */
    if (current < 0 && (leveltime + i * 17) % (2 * TICRATE) == 0)
      P_SpawnMobj(mo->x, mo->y, mo->z, MT_IFOG);
    d = P_AproxDistance(mo->x - pmo->x, mo->y - pmo->y);
    if (d < best)
    {
      best = d;
      near = i;
    }
    if (i == hint_icon && d < 384 * FRACUNIT)
      far_from_hint = false;
  }
  if (current >= 0)
    return;
  if (near >= 0 && near != hint_icon)
  {
    const char *hint = Wood_RouteHint(&clearings[icons[near].clearing]);
    hint_icon = near;
    if (hint)
      Wood_Message(hint, CR_GOLD, 0);
  }
  else if (near < 0 && far_from_hint)
    hint_icon = -1;
}

/* ------------------------------------------------------------------------ */
/* Перки                                                                    */
/* ------------------------------------------------------------------------ */

static void Wood_OpenChoice(void)
{
  int pool[NUMPERKS], n = 0, i;
  for (i = 0; i < NUMPERKS; i++)
    if (cfg_perk_enabled[i] && perk_level[i] < perk_info[i].max_level)
      pool[n++] = i;
  if (!n)
  {
    perk_picks = 0;
    return;
  }
  for (num_choices = 0; num_choices < WOOD_MAX_CHOICES && n > 0; num_choices++)
  {
    int k = Wood_RandRange(0, n - 1);
    choice[num_choices] = pool[k];
    pool[k] = pool[--n];
  }
  choice_sel = 0;
  choice_open = true;
  choice_tic = gametic;
  S_StartSound(NULL, sfx_getpow);
}

static void Wood_Heal(player_t *p, int amount, int cap)
{
  if (p->health >= cap || amount <= 0)
    return;
  p->health = MIN(cap, p->health + amount);
  if (p->mo)
    p->mo->health = p->health;
}

static void Wood_TakePerk(int sel)
{
  player_t *p = &players[consoleplayer];
  int k = choice[sel], i;
  char msg[64];

  perk_level[k]++;
  switch (k)
  {
    case PK_DEEP_POCKETS:
      if (!p->backpack)
      {
        for (i = 0; i < NUMAMMO; i++)
          p->maxammo[i] *= 2;
        p->backpack = true;
      }
      for (i = 0; i < NUMAMMO; i++)
        p->ammo[i] = MIN(p->maxammo[i], p->ammo[i] + clipammo[i] * 2);
      break;
    case PK_BARK_ARMOR:
      p->armorpoints = MIN(200, p->armorpoints + 50);
      if (!p->armortype)
        p->armortype = 1;
      break;
    default:
      break;
  }
  lprintf(LO_INFO, "WOOD: perk %s level %d\n", perk_info[k].id, perk_level[k]);
  if (perk_level[k] > 1)
    sprintf(msg, "%s %d", perk_info[k].name, perk_level[k]);
  else
    strcpy(msg, perk_info[k].name);
  Wood_Message(msg, CR_GOLD, sfx_itemup);
  choice_open = false;
  if (--perk_picks > 0)
    Wood_OpenChoice();
}

int WOOD_Frozen(void)
{
  return wood_active && choice_open && gamestate == GS_LEVEL;
}

int WOOD_Responder(event_t *ev)
{
  dboolean armed;
  if (!WOOD_Frozen() || menuactive)
    return false;
  armed = gametic - choice_tic >= WOOD_CHOICE_ARM;
  if (ev->type == ev_keydown)
  {
    int key = ev->data1;
    if (key >= '1' && key < '1' + num_choices)
    {
      if (armed)
        Wood_TakePerk(key - '1');
    }
    else if (key == KEYD_UPARROW || key == KEYD_LEFTARROW || key == key_up ||
             key == key_strafeleft || key == key_left)
    {
      choice_sel = (choice_sel + num_choices - 1) % num_choices;
      S_StartSound(NULL, sfx_pstop);
    }
    else if (key == KEYD_DOWNARROW || key == KEYD_RIGHTARROW || key == key_down ||
             key == key_straferight || key == key_right)
    {
      choice_sel = (choice_sel + 1) % num_choices;
      S_StartSound(NULL, sfx_pstop);
    }
    else if (key == KEYD_ENTER || key == key_use || key == key_fire || key == key_menu_enter)
    {
      if (armed)
        Wood_TakePerk(choice_sel);
    }
    return true;   /* остальные клавиши (оружие, движение) не уходят в игру */
  }
  if (ev->type == ev_mouse && (ev->data1 & 1))
  {
    if (armed)
      Wood_TakePerk(choice_sel);
    return true;
  }
  return false;
}

int WOOD_AdjustDamage(mobj_t *target, mobj_t *inflictor, mobj_t *source, int damage)
{
  if (!wood_active || damage <= 0 || damage >= 1000)
    return damage;
  if (source && source->player && target != source)
  {
    player_t *p = source->player;
    int pct = 100;
    if (inflictor == source && p->readyweapon == wp_chainsaw)
    {
      pct += 100 * perk_level[PK_SHARP_CHAIN];
      Wood_Heal(p, perk_level[PK_SAP_DRINKER], 100);
    }
    else
    {
      pct += 15 * perk_level[PK_HEAVY_LEAD];
      if (inflictor == source &&
          (p->readyweapon == wp_shotgun || p->readyweapon == wp_supershotgun))
        pct += 30 * perk_level[PK_BUCKSHOT];
    }
    damage = damage * pct / 100;
  }
  if (target->player && perk_level[PK_THICK_BARK])
    damage = MAX(1, damage * (100 - 15 * perk_level[PK_THICK_BARK]) / 100);
  return damage;
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
  int tier = waves_cleared;
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
  else if (c->route == ROUTE_ELITE)
  {
    budget = budget * cfg_elite_budget / 100;
    tier += cfg_elite_tier;
  }

  while (budget > 0 && wave_size < cfg_max_monsters && guard++ < 256)
  {
    int i, pick, choices = 0;
    int candidates[WOOD_MAX_MONSTERS];

    for (i = 0; i < cfg_num_monsters; i++)
      if (cfg_monsters[i].min_wave <= tier && cfg_monsters[i].cost <= budget)
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

  {
    static const char *route_names[] = { "boss", "weapon", "supply", "cache", "elite" };
    lprintf(LO_INFO, "WOOD: wave %d at clearing %d (%s), %d monsters\n",
            waves_cleared + 1, idx + 1, route_names[c->route], wave_size);
  }
  if (boss_spawned)
    Wood_Message("THE KEEPER OF THE WOOD AWAKES!", CR_RED, sfx_bossit);
  else
  {
    sprintf(msg, "%sWAVE %d: %d FOES", c->route == ROUTE_ELITE ? "ELITE " : "",
            waves_cleared + 1, wave_size);
    Wood_Message(msg, CR_RED, sfx_bossit);
  }
  Wood_BattleMusic();
}

static void Wood_GiveRewards(const wood_clearing_t *c)
{
  int i;
  fixed_t x, y;
  mobj_t *pmo = players[consoleplayer].mo;

  const mobjtype_t *extra = NULL;
  int num_extra = 0;

  /* в центр поляны — оружие этой поляны или случайный ящик патронов, остальное — вокруг */
  if (c->route == ROUTE_WEAPON && c->weapon != NUMMOBJTYPES)
    Wood_SpawnWithFog(c->cx, c->cy, c->weapon);
  else if (cfg_num_ammo_rewards)
    Wood_SpawnWithFog(c->cx, c->cy, cfg_ammo_rewards[Wood_RandRange(0, cfg_num_ammo_rewards - 1)]);

  switch (c->route)
  {
    case ROUTE_SUPPLY: extra = cfg_supply_rewards; num_extra = cfg_num_supply_rewards; break;
    case ROUTE_CACHE:  extra = cfg_cache_rewards;  num_extra = cfg_num_cache_rewards;  break;
    case ROUTE_ELITE:  extra = cfg_elite_rewards;  num_extra = cfg_num_elite_rewards;  break;
    default: break;
  }
  for (i = 0; i < num_extra; i++)
    if (Wood_PickSpot(c, pmo, 0, &x, &y))
      Wood_SpawnWithFog(x, y, extra[i]);

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
  Wood_RemoveIcons(current);
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
  if (perk_level[PK_SECOND_WIND])
    Wood_Heal(&players[consoleplayer], 100, 100);

  /* элитные поляны — по желанию: логово ждёт только обычные */
  for (i = 0; i < num_clearings; i++)
    if (!clearings[i].cleared && !clearings[i].boss && clearings[i].route != ROUTE_ELITE)
      left++;

  if (left)
  {
    sprintf(msg, "CLEARING CLEARED. %d LEFT", left);
    Wood_Message(msg, CR_GREEN, sfx_getpow);
  }
  else if (boss_idx >= 0)
  {
    if (gate.num_lines)
      Wood_OpenGate(c);
    else
      Wood_Message("CLEARING CLEARED. THE HEART OF THE WOOD AWAITS", CR_GREEN, sfx_getpow);
  }
  else
  {
    Wood_Victory();   /* на карте нет логова — забег кончается на последней поляне */
    return;
  }

  /* выбор перка; после элитной поляны — два подряд */
  if (cfg_num_perks)
  {
    /* выбор — не сразу: дать увидеть, как падает последний враг и появляются награды */
    perk_picks = c->route == ROUTE_ELITE ? 2 : 1;
    choice_delay = MAX(1, cfg_perk_delay * TICRATE / 1000);
  }
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
  int k, drops = perk_level[PK_LUMBERJACK] ? 2 : 1;

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

  for (k = 0; k < drops && drop != NUMMOBJTYPES; k++, drop = Wood_PickSawDrop(p))
  {
    /* патроны кладём между пнём и игроком, чтобы не застряли за стволом */
    fixed_t back = (24 + 20 * k) * FRACUNIT;
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

  damage *= 1 + perk_level[PK_SHARP_CHAIN];
  if (perk_level[PK_LUMBERJACK])
    damage *= 3;
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

/* Карточки выбора перка поверх игры (координаты — экран 320x200). */
#define WOOD_PAL_PANEL   0     /* чёрный */
#define WOOD_PAL_EDGE    124   /* тёмно-зелёный */
#define WOOD_PAL_ACTIVE  231   /* золотой */

static void Wood_FillRect(int x, int y, int w, int h, byte color)
{
  V_GetWideRect(&x, &y, &w, &h, VPT_STRETCH);
  V_FillRect(0, x, y, w, h, color);
}

static void Wood_WriteCentered(int y, const char *text, int cm)
{
  M_WriteText((320 - M_StringWidth(text)) / 2, y, text, cm);
}

static void Wood_DrawChoice(void)
{
  int i;
  char buf[64];

  Wood_FillRect(16, 22, 288, 36 * num_choices + 44, WOOD_PAL_PANEL);
  Wood_WriteCentered(28, perk_picks > 1 ? "THE WOOD OFFERS TWO GIFTS" : "THE WOOD OFFERS A GIFT",
                     CR_GREEN);
  for (i = 0; i < num_choices; i++)
  {
    const wood_perk_t *pk = &perk_info[choice[i]];
    int y = 44 + i * 36, next = perk_level[choice[i]] + 1;
    dboolean sel = i == choice_sel;

    Wood_FillRect(24, y, 272, 32, sel ? WOOD_PAL_ACTIVE : WOOD_PAL_EDGE);
    Wood_FillRect(26, y + 2, 268, 28, WOOD_PAL_PANEL);
    if (pk->max_level > 1)
      sprintf(buf, "%d  %s  %d/%d", i + 1, pk->name, next, pk->max_level);
    else
      sprintf(buf, "%d  %s", i + 1, pk->name);
    M_WriteText(34, y + 5, buf, sel ? CR_GOLD : CR_GRAY);
    M_WriteText(46, y + 17, pk->desc, sel ? CR_DEFAULT : CR_BROWN);
  }
  Wood_WriteCentered(48 + num_choices * 36, "1-3 OR ARROWS, ENTER TO TAKE", CR_GRAY);
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

  if (WOOD_Frozen() && !menuactive)
  {
    Wood_DrawChoice();
    return;
  }
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
  memset(perk_level, 0, sizeof(perk_level));
  perk_picks = 0;
  choice_open = false;
  choice_delay = 0;
  last_kills = 0;
  memset(icons, 0, sizeof(icons));
  num_icons = 0;
  hint_icon = -1;
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
  Wood_AssignRoutes();
  Wood_PlaceIcons();
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

  /* BLOOD SAP: каждое убийство лечит */
  if (p->killcount > last_kills && perk_level[PK_BLOOD_SAP])
    Wood_Heal(p, 4 * perk_level[PK_BLOOD_SAP] * (p->killcount - last_kills), 100);
  last_kills = p->killcount;

  Wood_TickIcons(p->mo);
  Wood_MimicThink(p);

  if (choice_delay > 0 && !--choice_delay)
  {
    Wood_OpenChoice();
    return;
  }

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

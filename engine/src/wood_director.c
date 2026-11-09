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
 *  - когда зачищены все поляны, начинается новый круг: поляны снова активны,
 *    сложность продолжает расти;
 *  - деревья можно спилить бензопилой (WOOD_SawTree): на месте дерева остаётся пень,
 *    из него выпадает немного патронов.
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

#include "doomstat.h"
#include "d_player.h"
#include "info.h"
#include "m_fixed.h"
#include "p_mobj.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_pspr.h"
#include "p_tick.h"
#include "r_main.h"
#include "r_state.h"
#include "s_sound.h"
#include "sounds.h"
#include "hu_stuff.h"
#include "v_video.h"
#include "w_wad.h"
#include "lprintf.h"
#include "wood_director.h"

#define WOOD_MAX_CLEARINGS   32
#define WOOD_MAX_MONSTERS    32
#define WOOD_MAX_WEAPONS     16
#define WOOD_MAX_REWARDS     8
#define WOOD_MAX_WAVE        64
#define WOOD_MAX_BARRIER     256

typedef struct {
  mobjtype_t type;
  int cost;
  int min_wave;
} wood_monster_t;

typedef struct {
  sector_t *sector;
  fixed_t cx, cy;      /* центр (по габаритам) */
  fixed_t radius;      /* половина меньшей стороны габаритов */
  dboolean cleared;    /* зачищена в текущем круге */
} wood_clearing_t;

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
static mobjtype_t cfg_lap_rewards[WOOD_MAX_REWARDS];
static int cfg_num_lap_rewards;
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

/* --- состояние забега (сбрасывается при каждой загрузке уровня, в т.ч. после смерти) --- */
static dboolean wood_active;
static wood_clearing_t clearings[WOOD_MAX_CLEARINGS];
static int num_clearings;
static int current = -1;              /* индекс поляны с идущим боем, -1 — боя нет */
static mobj_t *wave[WOOD_MAX_WAVE];
static int wave_size;
static int barrier[WOOD_MAX_BARRIER]; /* индексы линий, закрытых на время боя */
static int barrier_size;
#define WOOD_MAX_TORCHES     128
#define WOOD_TORCH_DOOMEDNUM 46       /* красный факел: видимая «огненная изгородь» */
#define WOOD_TORCH_STEP      (40 * FRACUNIT)
static mobj_t *torches[WOOD_MAX_TORCHES];
static int num_torches;
static int waves_cleared;             /* всего зачисток за забег — ось сложности */
static int lap;
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
  cfg_num_lap_rewards = cfg_num_ammo_rewards = 0;
  cfg_num_calm = cfg_num_battle = 0;
  cfg_num_saw_trees = cfg_num_saw_drops = 0;
  cfg_saw_stump = NUMMOBJTYPES;

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
    else if (!strcmp(key, "lap_reward"))
      Wood_ParseTypeList(rest, cfg_lap_rewards, &cfg_num_lap_rewards, WOOD_MAX_REWARDS);
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
  }
  W_UnlockLumpNum(lump);
  lprintf(LO_INFO, "WOOD: %d monster types, %d weapons in WOODCFG\n",
          cfg_num_monsters, cfg_num_weapons);
}

static void Wood_FindClearings(void)
{
  int s, l;
  num_clearings = 0;
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
static void Wood_PlaceTorches(const line_t *ln)
{
  static mobjtype_t torch_type = NUMMOBJTYPES;
  fixed_t len = P_AproxDistance(ln->dx, ln->dy);
  int n, k;

  if (torch_type == NUMMOBJTYPES)
    torch_type = Wood_TypeForDoomednum(WOOD_TORCH_DOOMEDNUM);
  if (torch_type == NUMMOBJTYPES || len <= 0)
    return;
  n = len / WOOD_TORCH_STEP + 1;
  for (k = 0; k <= n && num_torches < WOOD_MAX_TORCHES; k++)
  {
    fixed_t x = ln->v1->x + FixedMul(ln->dx, FixedDiv(k, n));
    fixed_t y = ln->v1->y + FixedMul(ln->dy, FixedDiv(k, n));
    torches[num_torches++] = Wood_SpawnWithFog(x, y, torch_type);
  }
}

static void Wood_SetBarrier(const wood_clearing_t *c, dboolean on)
{
  int i;
  if (on)
  {
    barrier_size = 0;
    for (i = 0; i < c->sector->linecount && barrier_size < WOOD_MAX_BARRIER; i++)
    {
      line_t *ln = c->sector->lines[i];
      if (ln->backsector && !(ln->flags & ML_BLOCKING))
      {
        ln->flags |= ML_BLOCKING;
        barrier[barrier_size++] = ln - lines;
        Wood_PlaceTorches(ln);
      }
    }
  }
  else
  {
    for (i = 0; i < barrier_size; i++)
      lines[barrier[i]].flags &= ~ML_BLOCKING;
    barrier_size = 0;
    for (i = 0; i < num_torches; i++)
    {
      mobj_t *fog = P_SpawnMobj(torches[i]->x, torches[i]->y, torches[i]->z, MT_IFOG);
      if (!i)
        S_StartSound(fog, sfx_itmbk);
      P_RemoveMobj(torches[i]);
      torches[i] = NULL;
    }
    num_torches = 0;
  }
}

static void Wood_StartWave(int idx)
{
  wood_clearing_t *c = &clearings[idx];
  mobj_t *pmo = players[consoleplayer].mo;
  int budget = cfg_budget_base + cfg_budget_step * waves_cleared;
  int guard = 0;
  char msg[64];

  current = idx;
  wave_size = 0;
  Wood_SetBarrier(c, true);

  while (budget > 0 && wave_size < cfg_max_monsters && guard++ < 256)
  {
    int i, pick, choices = 0;
    int candidates[WOOD_MAX_MONSTERS];
    fixed_t x, y;
    mobj_t *mo;

    for (i = 0; i < cfg_num_monsters; i++)
      if (cfg_monsters[i].min_wave <= waves_cleared && cfg_monsters[i].cost <= budget)
        candidates[choices++] = i;
    if (!choices)
      break;
    pick = candidates[Wood_RandRange(0, choices - 1)];

    if (!Wood_PickSpot(c, pmo, 320 * FRACUNIT, &x, &y))
      continue;
    mo = P_SpawnMobj(x, y, ONFLOORZ, cfg_monsters[pick].type);
    if (!P_CheckPosition(mo, x, y))
    {
      P_RemoveMobj(mo);
      continue;
    }
    {
      mobj_t *fog = P_SpawnMobj(x, y, mo->z, MT_TFOG);
      S_StartSound(fog, sfx_telept);
    }
    budget -= cfg_monsters[pick].cost;

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
  }

  if (!wave_size)
  {
    /* не нашлось места — считаем поляну пустой, чтобы не запереть игрока */
    Wood_SetBarrier(c, false);
    current = -1;
    c->cleared = true;
    return;
  }

  lprintf(LO_INFO, "WOOD: wave %d at clearing %d, %d monsters\n",
          waves_cleared + 1, idx + 1, wave_size);
  sprintf(msg, "WAVE %d: %d FOES", waves_cleared + 1, wave_size);
  Wood_Message(msg, CR_RED, sfx_bossit);
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

static void Wood_EndWave(void)
{
  wood_clearing_t *c = &clearings[current];
  int i, left = 0;

  for (i = 0; i < wave_size; i++)
    P_SetTarget(&wave[i], NULL);
  wave_size = 0;
  Wood_SetBarrier(c, false);
  c->cleared = true;
  current = -1;
  waves_cleared++;

  Wood_GiveRewards(c);
  Wood_CalmMusic();
  lprintf(LO_INFO, "WOOD: wave %d cleared, lap %d\n", waves_cleared, lap + 1);

  for (i = 0; i < num_clearings; i++)
    if (!clearings[i].cleared)
      left++;

  if (left)
  {
    Wood_Message("CLEARING CLEARED", CR_GREEN, sfx_getpow);
    return;
  }

  /* все поляны зачищены — новый круг */
  lap++;
  for (i = 0; i < num_clearings; i++)
    clearings[i].cleared = false;
  for (i = 0; i < cfg_num_lap_rewards; i++)
  {
    fixed_t x, y;
    if (Wood_PickSpot(c, players[consoleplayer].mo, 0, &x, &y))
      Wood_SpawnWithFog(x, y, cfg_lap_rewards[i]);
  }
  Wood_Message("THE FOREST GROWS ANGRIER", CR_GOLD, sfx_secret);
}

/* ------------------------------------------------------------------------ */
/* Пила по деревьям                                                         */
/* ------------------------------------------------------------------------ */

static dboolean Wood_IsSawTree(const mobj_t *mo)
{
  int i;
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
  mobj_t *mo;

  S_StartSound(tree, sfx_bdcls);   /* глухой удар — дерево падает */
  P_RemoveMobj(tree);
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
  lprintf(LO_INFO, "WOOD: tree felled, drop %d\n", drop);
  if (current < 0)
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

void WOOD_LevelStart(void)
{
  int lump = W_CheckNumForName("WOODCFG");
  int i;

  /* старые указатели принадлежат выгруженному уровню — просто забываем их */
  for (i = 0; i < WOOD_MAX_WAVE; i++)
    wave[i] = NULL;
  wave_size = barrier_size = 0;
  for (i = 0; i < WOOD_MAX_TORCHES; i++)
    torches[i] = NULL;
  num_torches = 0;
  current = -1;
  waves_cleared = lap = 0;
  music_started = false;

  wood_active = lump >= 0;
  if (!wood_active)
    return;

  Wood_ParseConfig(lump);
  Wood_FindClearings();
  Wood_InitTrees();
  Wood_Seed();

  if (playeringame[consoleplayer] && players[consoleplayer].mo)
    Wood_SetupPlayer(&players[consoleplayer]);

  lprintf(LO_INFO, "WOOD: director active, %d clearings\n", num_clearings);
}

void WOOD_Ticker(void)
{
  player_t *p;
  sector_t *sec;
  int i;

  if (!wood_active || gamestate != GS_LEVEL)
    return;
  p = &players[consoleplayer];
  if (!p->mo || p->playerstate != PST_LIVE)
    return;

  /* музыку уровня движок включает после P_SetupLevel, поэтому перебиваем её на первом тике */
  if (!music_started)
  {
    music_started = true;
    Wood_CalmMusic();
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

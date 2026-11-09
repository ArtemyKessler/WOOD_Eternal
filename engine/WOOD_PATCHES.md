# Движок WOOD

Здесь лежит исходный код [Dwasm](https://github.com/GMH-Code/Dwasm)
(WebAssembly-порт PrBoom+ / PrBoomX), лицензия GPL-2.0 — см. `COPYING`.

Базовая ревизия: `GMH-Code/Dwasm@ddf0347a4fc115b11ffb1c5710768b7c47c46698` (2026-04-14).

## Наши изменения

Все правки помечены в коде комментарием `WOOD:`.

| Файл | Что изменено |
|---|---|
| `src/d_main.c` | `wood.wad` первым в списке стандартных IWAD — игра стартует без параметров. |
| `src/d_main.c` | `CheckIWAD`: IWAD с лампом `WOODCFG` считается Doom II-совместимым, даже если в нём одна карта. |
| `src/s_sound.c` | `S_ChangeMusic`: отсутствующий музыкальный ламп — тишина вместо `I_Error`. |
| `src/wood_director.c`, `.h` | Директор волн: поляны, волны врагов, награды, круги, музыка (спокойная на тропах, боевая на поляне). Включается при наличии `WOODCFG`. |
| `src/p_setup.c` | В конце `P_SetupLevel` вызывается `WOOD_LevelStart()`. |
| `src/p_tick.c` | Каждый тик вызывается `WOOD_Ticker()`. |
| `src/m_menu.c` | Сохранение и загрузка (меню, F2/F3/F6/F9) отключены: вместо них сообщение «saving is disabled». |
| `src/g_game.c` | Новая игра и возрождение после смерти выбирают случайную раскладку леса (`WOOD_PickRunMap`), кроме запуска с `-warp`. |
| `src/CMakeLists.txt` | Добавлен `wood_director.c`. |

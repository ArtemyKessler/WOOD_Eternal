# WOOD

Шуточный rogue-like шутер в браузере. Огромный лес вместо коридоров, на старте только
бензопила, случайные группы врагов с растущей сложностью, новое оружие за каждую зачищенную поляну.

Работает на GPL-движке [Dwasm](https://github.com/GMH-Code/Dwasm) (PrBoom+ в WebAssembly)
с ресурсами из [Freedoom](https://freedoom.github.io). Оригинальные ресурсы id Software не используются.

## Статус

Этап 2: лес v1. Карта MAP01 генерируется скриптом: стартовая площадка, тропы и три поляны
(теги секторов 1–3), стены из берёзовой чащи, деревья вдоль опушки. Врагов и оружия пока нет —
их будет расставлять директор волн (этап 3).

## Структура

| Путь | Что там |
|---|---|
| `engine/` | Исходники движка (Dwasm + наши правки, см. `engine/WOOD_PATCHES.md`). GPL-2.0. |
| `wad/lumps/` | Наши лампы, которые кладутся в `wood.wad` (`WOODCFG` и др.). |
| `tools/mapgen/gen_forest.py` | Генератор лесной карты MAP01 (раскладка зон, деревья). |
| `tools/mapgen/nodebuild.py` | Простой BSP-нодбилдер на Python: сборка карты без внешних утилит. |
| `tools/build_iwad.py` | Собирает `wood.wad` из `freedoom2.wad`: выкидывает карты Freedoom, лишнюю музыку и спрайты, добавляет нашу карту. |
| `tools/build.sh` | Полная сборка в `dist/`. |
| `.github/workflows/pages.yml` | CI: сборка и деплой на GitHub Pages. |
| `LICENSES/` | Тексты лицензий (GPL-2.0, Freedoom BSD-3, авторы Freedoom). |

## Сборка (Linux / WSL)

Нужны: [emsdk](https://emscripten.org/docs/getting_started/downloads.html), `cmake`, `gcc`,
`libsdl2-dev`, `python3`, `curl`, `unzip`.

```sh
source ~/emsdk/emsdk_env.sh
./tools/build.sh
cd dist && python3 -m http.server 8080   # открыть http://localhost:8080
```

Параметры движка передаются через query string: `index.html?-warp&1&-skill&3`.
Управление по умолчанию: WASD — движение, стрелки — поворот, Tab — карта.

## Публикация

Каждый push в `main` собирается и выкладывается на GitHub Pages
(в настройках репозитория: Settings → Pages → Source: GitHub Actions).
Содержимое `dist/` можно также залить zip-архивом на itch.io как HTML5-игру.

## Лицензии

* Движок (`engine/`) — GNU GPL v2, исходники с нашими правками лежат в этом репозитории.
* Ресурсы Freedoom — BSD-3-Clause, © Contributors to the Freedoom project, см. `LICENSES/`.
* Проект не связан с id Software, Bethesda Softworks и ZeniMax Media.

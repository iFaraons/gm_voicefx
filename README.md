# gm_voicefx

Серверный модуль Garry's Mod: эффект «голос из рации» для тех, кто слышит игрока по рации.
Стоящие рядом слышат живой голос.

Основан на [gm_8bit](https://github.com/Meachamp/gm_8bit) (Meachamp), лицензия LGPL-2.1 (см. LICENSE).

## Как работает
Перехватывается `SV_BroadcastVoiceData`. Если у говорящего есть радиослушатели (`voicefx.SetRoute`),
оригинал вызывается дважды: исходный пакет — всем, кроме них; пакет с эффектом
(декодирование Opus -> полосовой фильтр, подъём середины, перегруз, шум -> кодирование) — только им.
Адресаты выбираются временной правкой масок `IVoiceServer` (VoiceServer002), после вызова маски восстанавливаются.

## Lua API (глобальная таблица `voicefx`)
| Функция | Описание |
|---|---|
| `SetRoute(talkerIdx, listenerIdx, viaRadio)` | ent-индексы; слышит ли listener говорящего через рацию |
| `ClearPlayer(idx)` / `ClearAll()` | сброс маршрутов и состояния кодека |
| `SetEnabled(bool)` | глобальный выключатель |
| `SetMaxClients(n)` | обычно `game.MaxPlayers()` |
| `SetParam(name, value)` | `highpass`, `lowpass`, `mid_freq`, `mid_gain`, `mid_q`, `drive`, `crush_bits`, `noise`, `gain` |
| `SetListening(recvIdx, sendIdx, listen, proximity)` | сразу выставить маску движка, не ждать пересчёта (~0,3 с) |
| `GetListening(recvIdx, sendIdx)` | маска движка, для отладки |
| `GetStats()` | обработано пакетов, ошибок |

## Сборка
Нужны [garrysmod_common](https://github.com/danielga/garrysmod_common) (ветка `x86-64-support-sourcesdk`, с сабмодулями) и premake5.

    premake5 --gmcommon=../garrysmod_common gmake    # Linux
    cd projects/linux/gmake && make config=releasewithsymbols_x86_64

    premake5 --gmcommon=../garrysmod_common vs2022   # Windows

Готовый файл кладётся в `garrysmod/lua/bin/` (`gmsv_voicefx_linux64.dll` / `gmsv_voicefx_win32.dll`).
CI: `.github/workflows/c-cpp.yml` собирает linux64, win32 и win64.

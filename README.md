![GitHub all releases](https://img.shields.io/github/downloads/ABKAM2023/cs2-blockerpasses/total?style=for-the-badge)
## RU
**BlockerPasses** - позволяет через меню размещать пропы и beam-стены в нужных точках карты. Каждый раунд они автоматически создаются, если игроков меньше значения, заданного в конфигурации.

**Обязательно указать пути моделей в ResourcePrecacher**
```ini
  "dust_rollupdoor_96x128_surface_lod"    "models/props/de_dust/hr_dust/dust_windows/dust_rollupdoor_96x128_surface_lod.vmdl"
  "chainlink_fence_001_256_capped"    "models/props/de_nuke/hr_nuke/chainlink_fence_001/chainlink_fence_001_256_capped.vmdl"
  "dust_soccer_ball001"   "models/props/de_dust/hr_dust/dust_soccerball/dust_soccer_ball001.vmdl"
```

[![Смотреть видео-демонстрацию](https://img.youtube.com/vi/01Bz4f9rvIY/hqdefault.jpg)](https://www.youtube.com/watch?v=01Bz4f9rvIY "Смотреть демо")

## Команды
- `mm_bp_access steamid64` выдать доступ к команде (если отсутствует Admin System).
- `!bp` - открыть меню 

## Требования
- [Utils](https://github.com/Pisex/cs2-menus/releases)
- [Admin System](https://github.com/Pisex/cs2-admin_system/releases)
- [ResourcePrecacher](https://github.com/Pisex/ResourcePrecacher)

## Конфиг
```ini
"BlockerPasses"
{
	// Минимальное количество игроков для открытия проходов
	"min_players_to_open"	"10"

	// Права доступа к команде !bp
	"access_permission"		"@admin/bp"

	// Включить отладочные сообщения в консоль (0 - выкл, 1 - вкл)
	"debug_log"				"0"

	// Не считать наблюдателей при подсчёте игроков (0 - считать, 1 - не считать)
	"ignore_spectators"		"1"

	// Список моделей для размещения через меню
	"models"
	{
		"model1"
		{
			"label" "Желзеные двери"
			"path"  "models/props/de_dust/hr_dust/dust_windows/dust_rollupdoor_96x128_surface_lod.vmdl"
		}
		"model2"
		{
			"label" "Желзеный забор"
			"path"  "models/props/de_nuke/hr_nuke/chainlink_fence_001/chainlink_fence_001_256_capped.vmdl"
		}
	}
}
```

## EN
**BlockerPasses** - allows you to place props and beam walls at specific map locations via a menu. They are automatically created every round if the number of players is below the value set in the configuration.

**Be sure to specify the model paths in ResourcePrecacher**
```ini
  "dust_rollupdoor_96x128_surface_lod"    "models/props/de_dust/hr_dust/dust_windows/dust_rollupdoor_96x128_surface_lod.vmdl"
  "chainlink_fence_001_256_capped"    "models/props/de_nuke/hr_nuke/chainlink_fence_001/chainlink_fence_001_256_capped.vmdl"
  "dust_soccer_ball001"   "models/props/de_dust/hr_dust/dust_soccerball/dust_soccer_ball001.vmdl"
```

[![Смотреть видео-демонстрацию](https://img.youtube.com/vi/01Bz4f9rvIY/hqdefault.jpg)](https://www.youtube.com/watch?v=01Bz4f9rvIY "Watch the demo")

## Requirements
- [Utils](https://github.com/Pisex/cs2-menus/releases)
- [Admin System](https://github.com/Pisex/cs2-admin_system/releases)
- [ResourcePrecacher](https://github.com/Pisex/ResourcePrecacher)

## Commands
- `mm_bp_access steamid64` grant access to the command (if there is no Admin System).
- `!bp` - open the menu.

## Config
```ini
"BlockerPasses"
{
	// Minimum number of players required to open passages
	"min_players_to_open"	"10"

	// Access permission for the !bp command
	"access_permission"		"@admin/bp"

	// Enable debug messages in console (0 - disabled, 1 - enabled)
	"debug_log"				"0"

	// Do not count spectators when calculating players (0 - count, 1 - ignore)
	"ignore_spectators"		"1"

	// List of models available for placement via menu
	"models"
	{
		"model1"
		{
			"label" "Metal Doors"
			"path"  "models/props/de_dust/hr_dust/dust_windows/dust_rollupdoor_96x128_surface_lod.vmdl"
		}
		"model2"
		{
			"label" "Metal Fence"
			"path"  "models/props/de_nuke/hr_nuke/chainlink_fence_001/chainlink_fence_001_256_capped.vmdl"
		}
	}
}
```

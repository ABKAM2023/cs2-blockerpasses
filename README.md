**BlockerPasses** - позволяет через админ-меню размещать пропы в нужных точках и автоматически создавать их каждый раунд, если игроков меньше значения, заданного в конфигурации.

[![Смотреть видео-демонстрацию](https://img.youtube.com/vi/hdmTZiPLX0o/hqdefault.jpg)](https://youtu.be/hdmTZiPLX0o "Смотреть демо")

## Требования
[Utils](https://github.com/Pisex/cs2-menus/releases)
[Admin System](https://github.com/Pisex/cs2-admin_system/releases)

## Конфиг
```ini
"BlockerPasses"
{
	// Минимум людей (не ботов) на сервере, чтобы проход был ОТКРЫТ
	"min_players_to_open"	"10"

	// Для доступа к !bp пермишен:
	"access_permission"		"@admin/bp"

	// Отладочные логи [0/1]
	"debug_log"				"0"

	// Список моделей, доступных для "Поставить предмет"
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
**BlockerPasses** — an admin-menu tool to place blocking props (or any other props) at predefined spots. It can automatically spawn them at the start of each round when the player count is below the threshold set in the config.

[![Watch the demo](https://img.youtube.com/vi/hdmTZiPLX0o/hqdefault.jpg)](https://youtu.be/hdmTZiPLX0o "Watch the demo")

## Requirements
[Utils](https://github.com/Pisex/cs2-menus/releases)
[Admin System](https://github.com/Pisex/cs2-admin_system/releases)

## Config
```ini
"BlockerPasses"
{
	// Minimum number of human players required for the passage to be OPEN
	"min_players_to_open"	"10"

	// Permission required for !bp access:
	"access_permission"		"@admin/bp"

	// Debug logging [0/1]
	"debug_log"				"0"

	// List of models available for the "Place item" action
	"models"
	{
		"model1"
		{
			"label" "Roll-up metal door"
			"path"  "models/props/de_dust/hr_dust/dust_windows/dust_rollupdoor_96x128_surface_lod.vmdl"
		}
		"model2"
		{
			"label" "Chain-link fence (256, capped)"
			"path"  "models/props/de_nuke/hr_nuke/chainlink_fence_001/chainlink_fence_001_256_capped.vmdl"
		}
	}
}

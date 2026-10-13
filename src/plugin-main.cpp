/*
ShyBoti for OBS
Copyright (C) 2026 ShyBoti

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "agent.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Connects OBS to ShyBoti";
}

// >>> stop early, obs-websocket dies on exit
static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		shy::uiLoaded();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		shy::uiShutdown();
		shy::agentStop();
	}
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

// >>> obs-websocket is only there after every module loaded
void obs_module_post_load(void)
{
	shy::uiInit();
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	shy::agentStart();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	shy::agentStop();
	obs_log(LOG_INFO, "plugin unloaded");
}

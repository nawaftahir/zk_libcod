#include "gsc_extra.hpp"

#if COMPILE_HTTP == 1
#include "gsc_http.hpp"
#include "gsc_websocket.hpp"
#endif

/*
 * Strong definitions of the extension hooks declared weakly in gsc_extra.hpp.
 * Modules that need per-frame or per-level work hook in here.
 */

// Called once per server frame after G_RunFrame
void extra_G_RunFrame_After(int levelTime)
{
	#if COMPILE_HTTP == 1
	gsc_http_poll();
	gsc_websocket_poll();
	#endif
}

// Called after a level's gametype scripts are loaded, before they run
void extra_GScr_LoadGameTypeScript_After()
{
	#if COMPILE_HTTP == 1
	// WebSocket connections are owned by the level that opened them: their
	// script callback handles died with the previous level, so close them
	// before the new level's scripts run. In-flight HTTP requests need no
	// reset; their callbacks are guarded by level id and expire naturally.
	gsc_websocket_shutdown();
	#endif
}

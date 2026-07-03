#ifndef WEBSOCKET_HPP
#define WEBSOCKET_HPP

#include "lib/mongoose/mongoose.h"

#include <functional>
#include <string>
#include <cstdint>

#undef poll

/*
 * WebSocket client on the Mongoose event loop.
 * - ws:// and wss:// (wss uses Mongoose's built-in TLS: encrypted, certificates
 *   not verified, same as CoD2x's http_client.h:601-603 - a best-effort transport)
 * - auto-reconnect on errors/remote close (reconnect_delay_ms 0 disables it)
 * - periodic PING keepalive with a PONG watchdog (ping_interval_ms 0 disables)
 * - poll-driven: call poll(ms) regularly; callbacks fire from within poll()
 *
 * Incoming PING and CLOSE frames are answered by Mongoose itself (PONG reply,
 * CLOSE echo + drain), so this class only tracks the state changes.
 */
class WebSocketClient
{
  public:
	using OnOpen = std::function<void()>;
	using OnMessage = std::function<void(const std::string &)>;
	using OnClose = std::function<void(bool isClosedByRemote, bool isFullyDisconnected)>;
	using OnError = std::function<void(const std::string &)>;

	/*
	 * reconnect_delay_ms: delay before reconnecting after a drop; 0 = connect
	 * once, no retry. ping_interval_ms: keepalive PING interval; 0 = disabled.
	 * pong_timeout_ms: max wait for a PONG after our PING; 0 = auto (half the
	 * ping interval; disabled when pings are disabled).
	 */
	WebSocketClient(std::string headers = "", unsigned reconnect_delay_ms = 2000,
	                unsigned ping_interval_ms = 15000, unsigned pong_timeout_ms = 0)
	{
		m_headers = std::move(headers);
		m_reconnect_ms = reconnect_delay_ms;
		m_ping_interval_ms = ping_interval_ms;
		m_pong_timeout_ms = pong_timeout_ms ? pong_timeout_ms : ( ping_interval_ms ? ping_interval_ms / 2 : 0 );

		mg_log_set(MG_LL_NONE);
		mg_mgr_init(&m_mgr);
	}

	~WebSocketClient()
	{
		close();
		mg_mgr_free(&m_mgr);
	}

	// Start (or replace) a connection to a ws:// or wss:// URL
	bool connect(const std::string &url)
	{
		m_url = url;
		m_disconnect = false;
		m_localClose = false;
		return try_connect_now();
	}

	// Drive networking; call once per server frame
	void poll(int ms = 0)
	{
		uint64_t now = mg_millis();

		// Deliver an error from a connect that failed synchronously inside
		// connect(). It is deferred to here so the script callback never runs
		// re-entrantly from within the webSocketConnect builtin (which would
		// corrupt the VM stack); poll() runs at the safe frame boundary.
		if ( m_pendingError )
		{
			m_pendingError = false;
			if ( m_onError )
				m_onError(m_pendingErrorMsg);
		}

		// Reconnect timer (only when auto-reconnect is enabled)
		if ( m_reconnect_ms > 0 && !m_conn && !m_disconnect && !m_url.empty() && now >= m_nextReconnect )
			try_connect_now();

		// Handshake watchdog: uses its own deadline so it also works when
		// auto-reconnect is disabled
		if ( m_conn && !m_connected && !m_disconnect && now >= m_connectDeadline )
		{
			m_closing = true;
			mg_error(m_conn, "Connect timeout");
		}

		// Keepalive PING
		if ( m_conn && m_connected && !m_closing && m_ping_interval_ms > 0 && now >= m_nextPing )
		{
			mg_ws_send(m_conn, "", 0, WEBSOCKET_OP_PING);
			m_nextPing = now + m_ping_interval_ms;
			m_waitingPong = ( m_pong_timeout_ms > 0 );
			if ( m_waitingPong )
				m_pongDeadline = now + m_pong_timeout_ms;
		}

		// PONG watchdog: no reply in time means the link is dead
		if ( m_conn && m_connected && m_waitingPong && now >= m_pongDeadline )
		{
			m_waitingPong = false;
			m_closing = true;
			mg_error(m_conn, "Ping timeout (no pong received within time limit)");
		}

		mg_mgr_poll(&m_mgr, ms);
	}

	// Send a TEXT frame; false if not currently connected
	bool sendText(const std::string &text)
	{
		if ( !m_conn || !m_connected || m_closing )
			return false;
		mg_ws_send(m_conn, text.c_str(), text.size(), WEBSOCKET_OP_TEXT);
		return true;
	}

	/*
	 * Requests a graceful close: sends a CLOSE frame and drains the socket so
	 * the frame is flushed before the connection is freed. Disables
	 * auto-reconnect until connect() is called again.
	 */
	void close()
	{
		m_disconnect = true;
		m_localClose = true;
		if ( m_conn )
		{
			m_closing = true;
			mg_ws_send(m_conn, "", 0, WEBSOCKET_OP_CLOSE);
			m_conn->is_draining = 1;
		}
	}

	void onOpen(OnOpen cb) { m_onOpen = std::move(cb); }
	void onMessage(OnMessage cb) { m_onMessage = std::move(cb); }
	void onClose(OnClose cb) { m_onClose = std::move(cb); }
	void onError(OnError cb) { m_onError = std::move(cb); }

	bool isConnected() const { return m_connected; }
	// Fully done: no connection and no reconnect pending; safe to delete
	bool isDisconnected() const { return !m_conn && !m_connected && !m_closing && m_disconnect; }

  private:
	bool try_connect_now()
	{
		uint64_t now = mg_millis();
		m_nextReconnect = now + m_reconnect_ms;
		m_connectDeadline = now + m_connectTimeoutMs;

		// mg_ws_connect may fire MG_EV_ERROR synchronously (socket/DNS setup
		// failure); m_connecting tells the error handler to defer that callback
		// instead of running it inside the webSocketConnect builtin.
		m_connecting = true;
		m_conn = mg_ws_connect(&m_mgr, m_url.c_str(), &WebSocketClient::s_ev, this, "%s", m_headers.c_str());
		m_connecting = false;

		if ( !m_conn )
		{
			// Without auto-reconnect a failed connect leaves nothing to retry,
			// so mark the client fully done and reapable
			if ( m_reconnect_ms == 0 )
				m_disconnect = true;
			// Defer the error to poll() so it never fires inside the builtin
			m_pendingError = true;
			m_pendingErrorMsg = "Failed to connect";
			return false;
		}
		return true;
	}

	static void s_ev(mg_connection *c, int ev, void *ev_data)
	{
		WebSocketClient *self = static_cast<WebSocketClient *>(c->fn_data);
		if ( self )
			self->handle_event(c, ev, ev_data);
	}

	void handle_event(mg_connection *c, int ev, void *ev_data)
	{
		switch ( ev )
		{
		case MG_EV_CONNECT:
		{
			// TCP established; for wss:// the TLS handshake starts here.
			// Mongoose upgrades to WebSocket once TLS (if any) completes.
			if ( c->is_tls )
			{
				struct mg_tls_opts opts = {};
				opts.name = mg_url_host(m_url.c_str());
				mg_tls_init(c, &opts);
			}
			break;
		}

		case MG_EV_WS_OPEN:
		{
			m_connected = true;
			m_waitingPong = false;
			m_closing = false;

			if ( m_ping_interval_ms > 0 )
				m_nextPing = mg_millis() + m_ping_interval_ms;
			if ( m_onOpen )
				m_onOpen();
			break;
		}

		case MG_EV_WS_MSG:
		{
			// Deliver TEXT frames only
			mg_ws_message *wm = static_cast<mg_ws_message *>(ev_data);
			uint8_t opcode = (uint8_t)(wm->flags & 0x0F);
			if ( opcode == WEBSOCKET_OP_TEXT && m_onMessage )
				m_onMessage(std::string(wm->data.buf, wm->data.len));
			break;
		}

		case MG_EV_WS_CTL:
		{
			// PING and CLOSE are already answered by Mongoose; only track state
			mg_ws_message *wm = static_cast<mg_ws_message *>(ev_data);
			uint8_t opcode = (uint8_t)(wm->flags & 0x0F);
			if ( opcode == WEBSOCKET_OP_PONG )
				m_waitingPong = false;
			else if ( opcode == WEBSOCKET_OP_CLOSE )
				m_closing = true;
			break;
		}

		case MG_EV_CLOSE:
		{
			if ( c == m_conn )
				m_conn = NULL;

			// Remote-initiated unless our own close() requested it. m_closing is
			// not a reliable signal here: a graceful remote CLOSE frame sets it
			// too, so a dedicated flag distinguishes the two.
			bool closedByRemote = !m_localClose;
			bool wasConnected = m_connected;
			m_connected = false;
			m_closing = false;
			m_waitingPong = false;

			// Schedule a reconnect unless closed manually or auto-reconnect
			// is disabled, in which case the client is fully done
			if ( !m_disconnect && !m_url.empty() && m_reconnect_ms > 0 )
				m_nextReconnect = mg_millis() + m_reconnect_ms;
			else
				m_disconnect = true;

			if ( wasConnected && m_onClose )
				m_onClose(closedByRemote, m_disconnect);
			break;
		}

		case MG_EV_ERROR:
		{
			if ( m_reconnect_ms == 0 )
				m_disconnect = true;

			// Fired synchronously from inside mg_ws_connect: defer so the script
			// callback runs at the frame boundary, not re-entrantly in the builtin.
			if ( m_connecting )
			{
				m_pendingError = true;
				m_pendingErrorMsg = static_cast<const char *>(ev_data);
			}
			else if ( m_onError )
				m_onError(static_cast<const char *>(ev_data));
			break;
		}

		default:
			break;
		}
	}

	mg_mgr m_mgr {};
	mg_connection *m_conn { NULL };
	bool m_connected { false };
	bool m_disconnect { false };
	bool m_closing { false };
	bool m_localClose { false };
	bool m_connecting { false };
	bool m_pendingError { false };
	std::string m_pendingErrorMsg;
	std::string m_url;
	std::string m_headers;

	unsigned m_reconnect_ms;
	unsigned m_ping_interval_ms;
	unsigned m_pong_timeout_ms { 0 };
	unsigned m_connectTimeoutMs { 10000 };
	uint64_t m_nextReconnect { 0 };
	uint64_t m_connectDeadline { 0 };
	uint64_t m_nextPing { 0 };
	bool m_waitingPong { false };
	uint64_t m_pongDeadline { 0 };

	OnOpen m_onOpen;
	OnMessage m_onMessage;
	OnClose m_onClose;
	OnError m_onError;
};

#endif

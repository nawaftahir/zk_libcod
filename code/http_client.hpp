#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

#include "lib/mongoose/mongoose.h"

#include <functional>
#include <string>
#include <map>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cctype>

#undef poll

/*
 * Asynchronous HTTP(S) client on the Mongoose event loop. One connection per
 * request, closed after the response arrives. poll() must be called regularly
 * (once per server frame) to drive I/O; callbacks fire from within poll(), on
 * the calling thread.
 *
 * TLS uses Mongoose's self-contained built-in stack (same as CoD2x): https://
 * traffic is encrypted but certificates are not verified against a CA. This is
 * a best-effort transport with no system TLS dependency; use an application
 * level token for any endpoint that must be authenticated. CoD2x sets only
 * opts.name (no opts.ca) the same way:
 *   https://github.com/callofduty2x/CoD2x/blob/d8c54695a5239ac99d1dfe96212b809b1a54bfee/src/shared/http_client.h#L601-L603
 */
class HttpClient
{
  public:
	struct Response
	{
		int status = 0;
		std::map<std::string, std::string> headers;
		std::string body;
	};
	using Callback = std::function<void(const Response &)>;
	using ErrorCallback = std::function<void(const std::string &error)>;

	HttpClient()
	{
		mg_log_set(MG_LL_NONE);
		mg_mgr_init(&mgr);
	}

	~HttpClient()
	{
		mg_mgr_free(&mgr);
	}

	void poll(int wait_time_ms = 0)
	{
		// Deliver errors from failures that happened synchronously inside
		// request(). They are deferred to here so the script callback never runs
		// re-entrantly from within the httpFetch builtin: Scr_ExecThread requires
		// the VM to be idle (localVars at stack base) and corrupts it otherwise.
		// poll() is driven from the frame boundary where that holds.
		//   CoD2rev Scr_ExecThread precondition:
		//     https://github.com/voron00/CoD2rev_Server/blob/abf692f1fd5697585ffd083e3114b2850c13a1f4/src/script/scr_vm.cpp#L1687
		if ( !deferred_errors.empty() )
		{
			std::vector<std::pair<ErrorCallback, std::string>> pending;
			pending.swap(deferred_errors);
			for ( auto &e : pending )
			{
				if ( e.first )
					e.first(e.second);
			}
		}

		mg_mgr_poll(&mgr, wait_time_ms);
	}

	/*
	 * Sends an HTTP request. headers are extra request headers separated by
	 * \r\n. timeout_ms bounds the whole request; connect_timeout_ms bounds the
	 * initial connect. timeout_ms <= 0 disables both timeouts. Exactly one of
	 * onDone/onError is invoked exactly once per request.
	 */
	void request(const char *method, const char *url, const char *data, size_t data_length,
	             const char *headers, Callback onDone, ErrorCallback onError,
	             int timeout_ms = 60000, int connect_timeout_ms = 5000)
	{
		if ( !is_valid_url(url) )
		{
			if ( onError )
				deferred_errors.emplace_back(std::move(onError), "Invalid URL");
			return;
		}

		// The context owns copies of all request data: the caller's pointers
		// are not valid once this returns
		RequestContext *ctx = new RequestContext();
		ctx->owner = this;
		ctx->url = url;
		ctx->method = ( method && *method ) ? method : "GET";
		ctx->headers = ( headers && *headers ) ? headers : "";
		if ( data_length > 0 )
		{
			ctx->data.resize(data_length);
			memcpy(ctx->data.data(), data, data_length);
		}
		ctx->onDone = std::move(onDone);
		ctx->onError = std::move(onError);

		if ( timeout_ms <= 0 )
			connect_timeout_ms = 0;
		ctx->timeout_ms = timeout_ms;
		ctx->timeout_connect_ms = connect_timeout_ms;

		// mg_http_connect may fire MG_EV_ERROR synchronously; report_error checks
		// this flag and defers any such callback instead of running it here.
		connecting = true;
		struct mg_connection *c = mg_http_connect(&mgr, ctx->url.c_str(), ev_handler, ctx);
		connecting = false;

		if ( !c )
		{
			// Allocation failed before any event fired: no context is registered
			// with Mongoose, so defer the callback and drop the context here.
			if ( ctx->onError )
				deferred_errors.emplace_back(ctx->onError, "Failed to connect");
			delete ctx;
		}
	}

	static bool is_valid_url(const char *url)
	{
		if ( !url || *url == '\0' )
			return false;

		const char *p = url;

		// Scheme: a letter followed by letters/digits/+/-/. and "://"
		if ( !isalpha((unsigned char)*p) )
			return false;
		p++;
		while ( *p && ( isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.' ) )
			p++;
		if ( strncmp(p, "://", 3) != 0 )
			return false;
		p += 3;
		if ( *p == '\0' )
			return false;

		// Reject control chars, spaces and characters never valid in a URL
		while ( *p )
		{
			unsigned char c = (unsigned char)*p;
			if ( c <= 32 || c > 126 )
				return false;
			if ( c == '"' || c == '`' || c == '<' || c == '>' || c == '\\' )
				return false;
			p++;
		}

		return true;
	}

  private:
	struct RequestContext
	{
		HttpClient *owner = NULL;
		std::string url;
		std::string method;
		std::string headers;
		std::vector<char> data;
		Callback onDone;
		ErrorCallback onError;

		int timeout_ms = 0;
		int timeout_connect_ms = 0;
		uint64_t timeout_endtime = 0;
		uint64_t timeout_connect_endtime = 0;
		bool connected = false;
		bool error_reported = false;
		bool finished = false;
	};

	mg_mgr mgr;

	// True only while mg_http_connect() runs inside request(). Mongoose can fire
	// MG_EV_ERROR synchronously there (socket/DNS setup failure) while still
	// returning a non-NULL connection, so errors raised in this window are
	// deferred rather than delivered straight into the httpFetch builtin.
	bool connecting = false;

	// Errors deferred out of the synchronous connect window, drained by poll()
	std::vector<std::pair<ErrorCallback, std::string>> deferred_errors;

	// Reports an error exactly once per request and starts connection teardown
	static void report_error(struct mg_connection *c, RequestContext *ctx, const std::string &msg)
	{
		if ( ctx && !ctx->error_reported && !ctx->finished )
		{
			ctx->error_reported = true;
			if ( ctx->onError )
			{
				// Inside the synchronous connect window the callback would run
				// re-entrantly in the builtin, so defer it to the next poll().
				if ( ctx->owner && ctx->owner->connecting )
					ctx->owner->deferred_errors.emplace_back(ctx->onError, msg);
				else
					ctx->onError(msg);
			}
		}
		if ( c )
			c->is_closing = 1;
	}

	static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
	{
		RequestContext *ctx = (RequestContext *)c->fn_data;

		if ( ev == MG_EV_OPEN )
		{
			uint64_t now = mg_millis();
			ctx->timeout_endtime = now + ctx->timeout_ms;
			ctx->timeout_connect_endtime = now + ctx->timeout_connect_ms;
		}
		else if ( ev == MG_EV_POLL )
		{
			uint64_t now = mg_millis();
			if ( !c->is_closing && !ctx->error_reported && !ctx->finished )
			{
				if ( ctx->timeout_connect_ms > 0 && !ctx->connected && now > ctx->timeout_connect_endtime )
					mg_error(c, "Connection timeout");
				else if ( ctx->timeout_ms > 0 && now > ctx->timeout_endtime )
					mg_error(c, "Timeout");
			}
		}
		else if ( ev == MG_EV_CONNECT )
		{
			ctx->connected = true;

			struct mg_str host = mg_url_host(ctx->url.c_str());
			const char *uri = mg_url_uri(ctx->url.c_str());
			size_t body_len = ctx->data.size();

			if ( c->is_tls )
			{
				struct mg_tls_opts opts = {};
				opts.name = host;
				mg_tls_init(c, &opts);
			}

			// Data queued during the TLS handshake is flushed by Mongoose once
			// the handshake completes
			std::string req;
			req.reserve(256 + ctx->headers.size() + body_len);
			req += ctx->method;
			req += " ";
			req += uri;
			req += " HTTP/1.1\r\nHost: ";
			req.append(host.buf, host.len);
			req += "\r\n";
			if ( !ctx->headers.empty() )
			{
				req += ctx->headers;
				size_t hlen = ctx->headers.size();
				if ( hlen < 2 || ctx->headers.compare(hlen - 2, 2, "\r\n") != 0 )
					req += "\r\n";
			}
			req += "Content-Length: ";
			req += std::to_string(body_len);
			req += "\r\n\r\n";
			if ( body_len > 0 )
				req.append(ctx->data.data(), body_len);

			mg_send(c, req.data(), req.size());
		}
		else if ( ev == MG_EV_HTTP_MSG )
		{
			if ( ctx->error_reported )
				return;

			struct mg_http_message *hm = (struct mg_http_message *)ev_data;

			Response res;
			res.status = mg_http_status(hm);
			res.body.assign(hm->body.buf, hm->body.len);
			for ( int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0; i++ )
			{
				res.headers[std::string(hm->headers[i].name.buf, hm->headers[i].name.len)] =
					std::string(hm->headers[i].value.buf, hm->headers[i].value.len);
			}

			ctx->finished = true;
			if ( ctx->onDone )
				ctx->onDone(res);

			c->is_closing = 1;
		}
		else if ( ev == MG_EV_ERROR )
		{
			report_error(c, ctx, ev_data ? std::string((char *)ev_data) : std::string("Unknown error"));
		}
		else if ( ev == MG_EV_CLOSE )
		{
			if ( !ctx->error_reported && !ctx->finished )
				report_error(NULL, ctx, "Connection closed unexpectedly");
			delete ctx;
		}
	}
};

#endif

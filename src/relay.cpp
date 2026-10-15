// >>> dials out, nothing listens
#include "agent.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#ifdef _WIN32
#include <psa/crypto.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace shy {

static const std::string AGENT_VERSION = std::string("obs-plugin ") + PLUGIN_VERSION;

static std::mutex wsMu;
static std::shared_ptr<ix::WebSocket> ws;
static std::atomic<RelayState> state{RelayState::Off};
static std::atomic<bool> identified{false};
static std::mutex chMu;
static std::string channel;

// vvv one worker for relay requests, keeps them in order vvv
static std::mutex qMu;
static std::condition_variable qCv;
static std::deque<std::function<void()>> queue;
static std::thread worker, ticker;
static std::atomic<bool> running{false};
static std::atomic<bool> busy{false};
static std::mutex tickMu;
static std::condition_variable tickCv;

static void enqueue(std::function<void()> fn)
{
	{
		std::lock_guard<std::mutex> l(qMu);
		if (queue.size() > 200)
			return; // <<< dashboard spam shouldn't eat memory
		queue.push_back(std::move(fn));
	}
	qCv.notify_one();
}

static void workerLoop()
{
	while (running) {
		std::function<void()> fn;
		{
			std::unique_lock<std::mutex> l(qMu);
			qCv.wait(l, [] { return !running || !queue.empty(); });
			if (!running)
				return;
			fn = std::move(queue.front());
			queue.pop_front();
		}
		busy = true;
		try {
			fn();
		} catch (...) {
		}
		busy = false;
	}
}

static void sendJson(const json &j)
{
	std::shared_ptr<ix::WebSocket> s;
	{
		std::lock_guard<std::mutex> l(wsMu);
		s = ws;
	}
	if (s && s->getReadyState() == ix::ReadyState::Open)
		s->sendText(dumpJson(j));
}

void relaySendEvent(const std::string &type, const json &data)
{
	if (!identified)
		return;
	sendJson({{"op", "event"}, {"type", type}, {"data", data}});
}

// >>> false = dropped, slow upload backs up here instead of in obs
bool relaySendBinary(const std::string &buf)
{
	if (!identified)
		return false;
	std::shared_ptr<ix::WebSocket> s;
	{
		std::lock_guard<std::mutex> l(wsMu);
		s = ws;
	}
	if (!s || s->getReadyState() != ix::ReadyState::Open || s->bufferedAmount() > 512 * 1024)
		return false;
	return s->sendBinary(buf).success;
}

RelayState relayState()
{
	return state;
}

std::string relayChannel()
{
	std::lock_guard<std::mutex> l(chMu);
	return channel;
}

// >>> newest release, the server tells us on connect and every pong
static std::string latestPlugin;
static void noteLatest(const json &msg)
{
	std::string v = jstr(msg, "latestPlugin");
	if (v.empty())
		return;
	std::lock_guard<std::mutex> l(chMu);
	latestPlugin = v;
}

std::string relayLatestPlugin()
{
	std::lock_guard<std::mutex> l(chMu);
	return latestPlugin;
}

// >>> same server as the relay, wss://x/api/agent -> https://x/api/agent/download/<os>
std::string relayDownloadUrl()
{
	std::string url = loadConfig().relayUrl;
	if (url.rfind("wss://", 0) == 0)
		url = "https://" + url.substr(6);
	else if (url.rfind("ws://", 0) == 0)
		url = "http://" + url.substr(5);
	while (!url.empty() && url.back() == '/')
		url.pop_back();
#if defined(_WIN32)
	return url + "/download/windows";
#elif defined(__APPLE__)
	return url + "/download/mac";
#else
	return url + "/download/linux";
#endif
}

// vvv incoming vvv
static void handleRequest(const json &msg)
{
	json rid = msg.contains("requestId") ? msg["requestId"] : json();
	std::string type = jstr(msg, "type");
	json data = msg.contains("data") ? msg["data"] : json::object();

	if (type == "AgentQuit") {
		sendJson({{"op", "response"}, {"requestId", rid}, {"ok", true}, {"data", json::object()}});
		log("Disconnect requested from the dashboard.");
		// >>> relayStop joins threads, can't run on this one
		runOnUi([] { relayStop(); });
		return;
	}
	if (type == "CheckForUpdate") {
		// >>> plugin updates come as an installer, not in place
		sendJson({{"op", "response"}, {"requestId", rid}, {"ok", true}, {"data", {{"updated", false}}}});
		return;
	}
	try {
		json result = handleRelayRequest(type, data);
		sendJson({{"op", "response"}, {"requestId", rid}, {"ok", true}, {"data", result}});
	} catch (const std::exception &e) {
		sendJson({{"op", "response"}, {"requestId", rid}, {"ok", false}, {"error", std::string(e.what())}});
	}
}

static void handleMessage(ix::WebSocket *self, const std::string &raw)
{
	json msg = json::parse(raw, nullptr, false);
	if (msg.is_discarded() || !msg.is_object())
		return;
	std::string op = jstr(msg, "op");

	if (op == "identified") {
		noteLatest(msg);
		{
			std::lock_guard<std::mutex> l(chMu);
			channel = jstr(msg, "channel");
		}
		identified = true;
		state = RelayState::Connected;
		log("Connected to ShyBoti as #" + relayChannel() + ".");
		relaySendEvent("ObsConnectionState", {{"connected", obsApiReady()}});
		return;
	}
	if (op == "error") {
		std::string reason = jstr(msg, "reason");
		log("ShyBoti rejected the connection: " + reason);
		if (reason == "invalid_token") {
			state = RelayState::InvalidToken;
			log("The pairing token is wrong or was regenerated. Paste the new one in Tools > ShyBoti.");
			// >>> bad token, don't hammer the server retrying
			self->disableAutomaticReconnection();
			self->close();
		}
		return;
	}
	if (op == "pong") {
		noteLatest(msg);
		return;
	}
	if (op == "rules") {
		if (msg["rules"].is_array()) {
			rulesSet(msg["rules"]);
			log("Synced " + std::to_string(rulesCount()) + " automation rule(s) from the dashboard.");
		}
		return;
	}
	if (op == "request")
		enqueue([msg] { handleRequest(msg); });
}

// vvv connection vvv
void relayStart()
{
	relayStop();
	Config cfg = loadConfig();
	if (cfg.token.empty()) {
		log("No pairing token yet. Add it in Tools > ShyBoti.");
		return;
	}
	auto s = std::make_shared<ix::WebSocket>();
	ix::WebSocket *self = s.get();
	s->setUrl(cfg.relayUrl);
	s->setHandshakeTimeout(10);
	s->setMinWaitBetweenReconnectionRetries(3000);
	s->setMaxWaitBetweenReconnectionRetries(60000);
	s->disablePerMessageDeflate();
	std::string token = cfg.token;
	std::string url = cfg.relayUrl;
	s->setOnMessageCallback([self, token, url](const ix::WebSocketMessagePtr &m) {
		using T = ix::WebSocketMessageType;
		if (m->type == T::Open) {
			self->sendText(
				dumpJson({{"op", "identify"}, {"token", token}, {"agentVersion", AGENT_VERSION}}));
			return;
		}
		if (m->type == T::Message) {
			handleMessage(self, m->str);
			return;
		}
		if (m->type == T::Error) {
			log("Connection error: " + m->errorInfo.reason);
			return;
		}
		if (m->type == T::Close) {
			bool was = identified.exchange(false);
			previewOff();
			uint16_t code = m->closeInfo.code;
			if (code == 4002) {
				// >>> two agents kick each other forever otherwise
				state = RelayState::Replaced;
				log("Another ShyBoti agent took over this channel. Close the old agent, then press Connect.");
				self->disableAutomaticReconnection();
				return;
			}
			if (code == 4001) {
				state = RelayState::InvalidToken;
				log("The pairing token was regenerated. Paste the new one in Tools > ShyBoti.");
				self->disableAutomaticReconnection();
				return;
			}
			if (state == RelayState::InvalidToken || state == RelayState::Off)
				return;
			state = RelayState::Connecting;
			log(was ? "Disconnected from ShyBoti, reconnecting..."
				: "Could not reach " + url + " (code " + std::to_string(code) + "). Retrying...");
		}
	});
	state = RelayState::Connecting;
	{
		std::lock_guard<std::mutex> l(wsMu);
		ws = s;
	}
	s->start();
}

void relayStop()
{
	std::shared_ptr<ix::WebSocket> s;
	{
		std::lock_guard<std::mutex> l(wsMu);
		s.swap(ws);
	}
	identified = false;
	state = RelayState::Off;
	previewOff();
	if (s) {
		s->disableAutomaticReconnection();
		s->stop();
	}
}

// vvv ping, bitrate, event batching vvv
static void tickerLoop()
{
	using clock = std::chrono::steady_clock;
	auto nextPing = clock::now() + std::chrono::seconds(20);
	auto nextBitrate = clock::now() + std::chrono::seconds(3);
	double lastBytes = 0;
	clock::time_point lastAt;
	bool haveLast = false;

	while (running) {
		{
			std::unique_lock<std::mutex> l(tickMu);
			tickCv.wait_for(l, std::chrono::milliseconds(200), [] { return !running.load(); });
		}
		if (!running)
			return;
		auto now = clock::now();
		obsEventsTick();
		previewTick();

		if (now >= nextPing) {
			nextPing = now + std::chrono::seconds(20);
			if (identified)
				sendJson({{"op", "ping"}});
		}

		if (now < nextBitrate)
			continue;
		nextBitrate = now + std::chrono::seconds(3);
		// >>> obs gives total bytes, not live kbps, so diff two samples
		try {
			json d = obsRequest("GetStreamStatus");
			bool streaming = jtruthy(d, "outputActive");
			json kbps;
			double bytes;
			if (streaming && jnum(d, "outputBytes", bytes)) {
				if (haveLast) {
					double secs = std::chrono::duration<double>(now - lastAt).count();
					double delta = bytes - lastBytes;
					// >>> negative = stream restarted, skip
					if (secs > 0 && delta >= 0)
						kbps = (int)std::lround(delta * 8 / 1000 / secs);
				}
				lastBytes = bytes;
				lastAt = now;
				haveLast = true;
			} else {
				haveLast = false;
			}
			double congestion = 0;
			jnum(d, "outputCongestion", congestion);
			relaySendEvent("BitrateUpdate",
				       {{"kbps", kbps}, {"congested", congestion > 0.15}, {"streaming", streaming}});
			previewNoteCongestion(streaming && congestion > 0.15);
			// >>> rules run here even when shyboti is unreachable
			if (kbps.is_number())
				rulesOnBitrate(kbps.get<int>());
		} catch (...) {
		}
	}
}

void agentStart()
{
	if (running)
		return;
#ifdef _WIN32
	// >>> mbedtls 3 needs this before any tls 1.3 handshake
	psa_crypto_init();
#endif
	ix::initNetSystem();
	{
		Config cfg = loadConfig();
		previewConfigure(cfg.previewQuality, cfg.previewAutoLower);
	}
	running = true;
	worker = std::thread(workerLoop);
	ticker = std::thread(tickerLoop);
	rulesLoad();
	if (rulesCount())
		log("Loaded " + std::to_string(rulesCount()) + " cached automation rule(s).");
	if (!obsApiReady())
		log("obs-websocket is missing in this OBS. Scenes and sources won't work.");
	obsEventsStart();
	if (loadConfig().autoConnect)
		relayStart();
	else
		log("Auto connect is off. Press Connect.");
}

void agentStop()
{
	if (!running)
		return;
	relayStop();
	obsEventsStop();
	running = false;
	qCv.notify_all();
	tickCv.notify_all();
	// >>> stuck request shouldn't block obs exit, 3s max
	for (int i = 0; i < 30 && busy; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	if (worker.joinable()) {
		if (busy)
			worker.detach();
		else
			worker.join();
	}
	if (ticker.joinable())
		ticker.join();
	{
		std::lock_guard<std::mutex> l(qMu);
		queue.clear();
	}
	ix::uninitNetSystem();
}

} // namespace shy

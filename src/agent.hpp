#pragma once
// >>> everything the plugin parts share, one header on purpose
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace shy {

// vvv settings + log (settings.cpp) vvv
struct Config {
	std::string token;
	std::string relayUrl = "wss://shyboti.de/api/agent";
	bool autoConnect = true;
	bool dockShown = false;
	int previewQuality = 2; // <<< 0 off, 1 low, 2 medium, 3 high
	bool previewAutoLower = true;
};
Config loadConfig();
void saveConfig(const Config &c);
std::string configPath(const std::string &file);
void log(const std::string &msg);
std::vector<std::string> recentLog();
json readJsonFile(const std::string &path);
bool writeJsonFile(const std::string &path, const json &j);

// vvv safe json reads, the dashboard sends loose types vvv
std::string jstr(const json &j, const char *key, const std::string &def = "");
bool jnum(const json &j, const char *key, double &out);
bool jtruthy(const json &j, const char *key);
std::string dumpJson(const json &j);

// vvv obs side (obs-bridge.cpp) vvv
bool obsApiReady();
json obsRequest(const std::string &type, const json &data = json());
json handleRelayRequest(const std::string &type, const json &data);
void obsEventsStart();
void obsEventsStop();
void obsEventsTick();

// vvv relay (relay.cpp) vvv
enum class RelayState { Off, Connecting, Connected, InvalidToken, Replaced };
void agentStart();
void agentStop();
void relayStart();
void relayStop();
RelayState relayState();
std::string relayChannel();
void relaySendEvent(const std::string &type, const json &data);
bool relaySendBinary(const std::string &buf);

// vvv live preview (preview.cpp) vvv
void previewRegister();
json previewSet(const std::vector<std::string> &wanted);
void previewOff();
void previewTick();
void previewConfigure(int quality, bool autoLower);
void previewNoteCongestion(bool congested);

// vvv rules (rules.cpp) vvv
void rulesLoad();
void rulesSet(const json &rules);
void rulesOnBitrate(int kbps);
size_t rulesCount();

// vvv ui (ui.cpp) vvv
void uiInit();
void uiLoaded();
void uiShutdown();
void runOnUi(std::function<void()> fn);
json probeProjector();

} // namespace shy

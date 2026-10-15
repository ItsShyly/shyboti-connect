#include "agent.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace shy {

// vvv files vvv
std::string configPath(const std::string &file)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path(file.c_str());
	std::string out = p ? p : "";
	bfree(p);
	return out;
}

json readJsonFile(const std::string &path)
{
	// >>> u8path, non-ascii user folders break plain strings on windows
	std::ifstream f(fs::u8path(path), std::ios::binary);
	if (!f)
		return json();
	json j = json::parse(f, nullptr, false);
	return j.is_discarded() ? json() : j;
}

bool writeJsonFile(const std::string &path, const json &j)
{
	std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
	if (!f)
		return false;
	f << j.dump(2, ' ', false, json::error_handler_t::replace);
	return true;
}

// vvv config vvv
static fs::path homeDir()
{
#ifdef _WIN32
	const wchar_t *h = _wgetenv(L"USERPROFILE");
	return h ? fs::path(h) : fs::path();
#else
	const char *h = std::getenv("HOME");
	return h ? fs::path(h) : fs::path();
#endif
}

Config loadConfig()
{
	Config c;
	json j = readJsonFile(configPath("config.json"));
	c.token = jstr(j, "token");
	if (!jstr(j, "relayUrl").empty())
		c.relayUrl = jstr(j, "relayUrl");
	if (j.is_object() && j.contains("autoConnect"))
		c.autoConnect = jtruthy(j, "autoConnect");
	c.dockShown = jtruthy(j, "dockShown");
	double q;
	if (jnum(j, "previewQuality", q))
		c.previewQuality = std::max(0, std::min(3, (int)q));
	if (j.is_object() && j.contains("previewAutoLower"))
		c.previewAutoLower = jtruthy(j, "previewAutoLower");
	// >>> no token yet, check the agent's config
	if (c.token.empty()) {
		fs::path old = homeDir() / ".shyboti-agent" / "config.json";
		std::ifstream f(old, std::ios::binary);
		if (f) {
			json o = json::parse(f, nullptr, false);
			if (!o.is_discarded() && !jstr(o, "token").empty()) {
				c.token = jstr(o, "token");
				saveConfig(c);
				log("Took the pairing token from the old ShyBoti agent.");
			}
		}
	}
	return c;
}

void saveConfig(const Config &c)
{
	json j = readJsonFile(configPath("config.json"));
	if (!j.is_object())
		j = json::object();
	j["token"] = c.token;
	j["autoConnect"] = c.autoConnect;
	j["dockShown"] = c.dockShown;
	j["previewQuality"] = c.previewQuality;
	j["previewAutoLower"] = c.previewAutoLower;
	writeJsonFile(configPath("config.json"), j);
}

// vvv log, ring buffer for the settings window vvv
static std::mutex logMu;
static std::deque<std::string> logLines;

void log(const std::string &msg)
{
	std::time_t t = std::time(nullptr);
	char ts[16] = {0};
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
	obs_log(LOG_INFO, "%s", msg.c_str());
	std::lock_guard<std::mutex> l(logMu);
	logLines.push_back(std::string("[") + ts + "] " + msg);
	while (logLines.size() > 300)
		logLines.pop_front();
}

std::vector<std::string> recentLog()
{
	std::lock_guard<std::mutex> l(logMu);
	return {logLines.begin(), logLines.end()};
}

// vvv json helpers vvv
std::string jstr(const json &j, const char *key, const std::string &def)
{
	if (!j.is_object())
		return def;
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}

bool jnum(const json &j, const char *key, double &out)
{
	if (!j.is_object())
		return false;
	auto it = j.find(key);
	if (it == j.end() || !it->is_number())
		return false;
	out = it->get<double>();
	return std::isfinite(out);
}

bool jtruthy(const json &j, const char *key)
{
	if (!j.is_object())
		return false;
	auto it = j.find(key);
	if (it == j.end())
		return false;
	if (it->is_boolean())
		return it->get<bool>();
	if (it->is_number())
		return it->get<double>() != 0;
	if (it->is_string())
		return !it->get<std::string>().empty();
	return !it->is_null();
}

std::string dumpJson(const json &j)
{
	return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace shy

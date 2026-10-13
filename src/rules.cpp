// >>> bitrate rules, run local, cached to disk
#include "agent.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

namespace shy {

static const int DEBOUNCE_SAMPLES = 2; // <<< needs 2 bad polls to fire
static const int64_t MIN_REFIRE_MS = 5000;

struct RuleState {
	int badStreak = 0;
	bool armed = true;
	int64_t lastFiredAt = 0;
};

static std::mutex mu;
static json rules = json::array();
static std::map<std::string, RuleState> states;

static std::string ruleKey(const json &r)
{
	return r.is_object() && r.contains("id") ? dumpJson(r["id"]) : "";
}

void rulesLoad()
{
	json j = readJsonFile(configPath("rules.json"));
	std::lock_guard<std::mutex> l(mu);
	rules = j.is_array() ? j : json::array();
}

void rulesSet(const json &next)
{
	std::lock_guard<std::mutex> l(mu);
	rules = next.is_array() ? next : json::array();
	if (!writeJsonFile(configPath("rules.json"), rules))
		log("Could not save the automation rules locally.");
	// >>> drop state of removed rules
	std::map<std::string, RuleState> kept;
	for (auto &r : rules) {
		auto it = states.find(ruleKey(r));
		if (it != states.end())
			kept.insert(*it);
	}
	states.swap(kept);
}

size_t rulesCount()
{
	std::lock_guard<std::mutex> l(mu);
	return rules.size();
}

static void execute(const json &rule)
{
	std::string action = jstr(rule, "action");
	std::string target = jstr(rule, "target");
	if (action == "scene") {
		obsRequest("SetCurrentProgramScene", {{"sceneName", target}});
		return;
	}
	// >>> source actions target whatever scene is live
	if (action == "show" || action == "hide" || action == "toggle") {
		json scenes = obsRequest("GetSceneList");
		std::string sceneName = jstr(scenes, "currentProgramSceneName");
		json items = obsRequest("GetSceneItemList", {{"sceneName", sceneName}});
		auto lc = [](std::string s) {
			std::transform(s.begin(), s.end(), s.begin(),
				       [](unsigned char c) { return (char)std::tolower(c); });
			return s;
		};
		json list = items.contains("sceneItems") && items["sceneItems"].is_array() ? items["sceneItems"]
											   : json::array();
		for (auto &it : list) {
			if (lc(jstr(it, "sourceName")) != lc(target))
				continue;
			bool enabled = action == "show"   ? true
				       : action == "hide" ? false
							  : !jtruthy(it, "sceneItemEnabled");
			obsRequest("SetSceneItemEnabled", {{"sceneName", sceneName},
							   {"sceneItemId", it["sceneItemId"]},
							   {"sceneItemEnabled", enabled}});
			return;
		}
		log("Rule target source \"" + target + "\" not found in the current scene.");
		return;
	}
	if (action == "mute" || action == "unmute") {
		obsRequest("SetInputMute", {{"inputName", target}, {"inputMuted", action == "mute"}});
		return;
	}
	if (action == "mutetoggle") {
		obsRequest("ToggleInputMute", {{"inputName", target}});
		return;
	}
	if (action == "volume") {
		double pct = 50;
		jnum(rule, "value", pct);
		pct = std::max(0.0, std::min(100.0, pct));
		obsRequest("SetInputVolume", {{"inputName", target}, {"inputVolumeMul", pct / 100}});
		return;
	}
	log("Unknown rule action \"" + action + "\".");
}

void rulesOnBitrate(int kbps)
{
	int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
			      std::chrono::steady_clock::now().time_since_epoch())
			      .count();
	std::vector<json> toFire;
	{
		std::lock_guard<std::mutex> l(mu);
		for (auto &rule : rules) {
			if (!jtruthy(rule, "enabled"))
				continue;
			std::string trig = jstr(rule, "trigger_type");
			if (!trig.empty() && trig != "bitrate")
				continue;
			double limit = 0;
			if (!jnum(rule, "bitrate_kbps", limit))
				continue;
			RuleState &st = states[ruleKey(rule)];
			bool bad = jstr(rule, "condition") == "above" ? kbps > limit : kbps < limit;
			if (bad) {
				st.badStreak++;
			} else {
				st.badStreak = 0;
				st.armed = true; // <<< condition cleared, can fire again
			}
			if (bad && st.armed && st.badStreak >= DEBOUNCE_SAMPLES &&
			    now - st.lastFiredAt >= MIN_REFIRE_MS) {
				st.armed = false;
				st.lastFiredAt = now;
				toFire.push_back(rule);
			}
		}
	}
	// >>> outside the lock, obs calls can take a moment
	for (auto &rule : toFire) {
		log("Rule fired: bitrate " + std::to_string(kbps) + "kbps " + jstr(rule, "condition") + " -> " +
		    jstr(rule, "action") + " \"" + jstr(rule, "target") + "\"");
		try {
			execute(rule);
			relaySendEvent("RuleFired", {{"ruleId", rule["id"]}});
		} catch (const std::exception &e) {
			log(std::string("Rule failed: ") + e.what());
		}
	}
}

} // namespace shy

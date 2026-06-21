#include <Geode/Geode.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <cvolton.level-id-api/include/EditorIDs.hpp>

#include <sstream>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

static std::string getLevelKey(GJGameLevel* level) {
    std::string key;
    if (level->m_levelType == GJLevelType::Editor) {
        key = std::to_string(EditorIDs::getID(level)) + "-editor";
    } else {
        key = std::to_string(level->m_levelID.value());
        if (level->m_levelType == GJLevelType::Main)
            key += "-local";
    }
    if (level->m_dailyID > 0)   key += "-daily";
    if (level->m_gauntletLevel) key += "-gauntlet";
    return key;
}

static std::string formatCommas(int n) {
    bool neg = n < 0;
    std::string s = std::to_string(std::abs(n));
    for (int i = static_cast<int>(s.length()) - 3; i > 0; i -= 3)
        s.insert(i, ",");
    return neg ? "-" + s : s;
}

static std::optional<std::filesystem::path> dtLevelSaveBase() {
    auto dt = Loader::get()->getLoadedMod("elohmrow.death_tracker");
    if (!dt) return std::nullopt;

    auto settingPath = dt->getSettingValue<std::filesystem::path>("save-path-new");
    if (!settingPath.empty() && std::filesystem::exists(settingPath))
        return settingPath;

    auto defaultPath = dt->getSaveDir() / "levels";
    if (std::filesystem::exists(defaultPath))
        return defaultPath;

    return std::nullopt;
}

static std::optional<matjson::Value> readDTMeta(
    const std::filesystem::path& base,
    const std::string& key
) {
    auto res = file::readJson(base / key / "metadata");
    return res.isOk() ? std::optional(res.unwrap()) : std::nullopt;
}


// --- Level length estimation (mirrors Death Tracker's timeForLevelString) ---
// Derives a level's length in seconds by walking its speed portals: each segment
// between portals is covered at that portal's speed. This is how we convert Death
// Tracker's percentage histograms into a playtime estimate.

static bool objectIDIsSpeedPortal(int id) {
    return id == 200 || id == 201 || id == 202 || id == 203 || id == 1334;
}

static int speedToPortalId(int speed) {
    switch (speed) {
        case 1:  return 200;
        case 2:  return 202;
        case 3:  return 203;
        case 4:  return 1334;
        default: return 201;
    }
}

// Units-per-second travelled at each speed portal's speed.
static float travelForPortalId(int portalId) {
    switch (portalId) {
        case 200:  return 251.16008f;
        case 202:  return 387.42014f;
        case 203:  return 468.00015f;
        case 1334: return 576.00018f;
        default:   return 311.58011f;
    }
}

static float timeForLevelString(const std::string& levelString) {
    struct SpeedPortalObject { int id; float xPos; };

    std::string decompressed = ZipUtils::decompressString(levelString, false, 0);
    std::stringstream responseStream(decompressed);
    std::string currentObject;
    std::vector<SpeedPortalObject> speedPortals;

    float prevPortalX = 0.f;
    int   prevPortalId = 0;
    float timeFull = 0.f;
    float maxPos = 0.f;

    while (std::getline(responseStream, currentObject, ';')) {
        size_t i = 0;
        int   objID = 0;
        float xPos = 0.f;
        bool  checked = false;
        std::string currentKey, keyID;
        std::stringstream objectStream(currentObject);
        while (std::getline(objectStream, currentKey, ',')) {
            if (i % 2 == 0) {
                keyID = currentKey;
            } else {
                if (keyID == "1")        objID = geode::utils::numFromString<int>(currentKey).unwrapOr(0);
                else if (keyID == "2")   xPos  = geode::utils::numFromString<float>(currentKey).unwrapOr(0.f);
                else if (keyID == "13")  checked = geode::utils::numFromString<int>(currentKey).unwrapOr(0) != 0;
                else if (keyID == "kA4") prevPortalId = speedToPortalId(geode::utils::numFromString<int>(currentKey).unwrapOr(0));
            }
            i++;
            if (xPos != 0.f && objID != 0 && checked) break;
        }

        if (maxPos < xPos) maxPos = xPos;
        if (!checked || !objectIDIsSpeedPortal(objID)) continue;
        speedPortals.push_back({objID, xPos});
    }

    std::sort(speedPortals.begin(), speedPortals.end(),
        [](const SpeedPortalObject& a, const SpeedPortalObject& b) { return a.xPos < b.xPos; });

    for (const auto& portal : speedPortals) {
        timeFull += (portal.xPos - prevPortalX) / travelForPortalId(prevPortalId);
        prevPortalId = portal.id;
        prevPortalX  = portal.xPos;
    }
    timeFull += (maxPos - prevPortalX) / travelForPortalId(prevPortalId);

    return timeFull;
}

// Level length in seconds. We always derive it from the level's geometry rather
// than m_timestamp: m_timestamp is only set once you've completed the level (and
// is unreliable), whereas the level string is always available at the endscreen.
static float levelLengthSeconds(GJGameLevel* level) {
    if (level->isPlatformer())
        return 0.f;
    return std::ceil(timeForLevelString(level->m_levelString));
}


static float runKeyLength(const std::string& key) {
    auto dash = key.find('-');
    if (dash == std::string::npos) {
        auto end = geode::utils::numFromString<int>(key);
        return end.isOk() ? static_cast<float>(end.unwrap()) : -1.f;
    }
    auto start = geode::utils::numFromString<int>(key.substr(0, dash));
    auto end   = geode::utils::numFromString<int>(key.substr(dash + 1));
    if (start.isErr() || end.isErr()) return -1.f;
    return static_cast<float>(end.unwrap() - start.unwrap());
}


// Death Tracker's legacy/estimated playtime (aptgen), in seconds: the full level
// length (wtSeconds) scaled by how far each attempt got, summed over every death
// and run in the level's history. Mirrors Death Tracker's calcPlaytime.
static double readDTLegacySeconds(
    const std::filesystem::path& base,
    const std::string& key,
    float wtSeconds
) {
    if (wtSeconds <= 0.f) return 0.0;

    auto res = file::readJson(base / key / "general.dt");
    if (res.isErr()) return 0.0;
    auto json = res.unwrap();

    double seconds = 0.0;
    for (const auto* field : {"deaths", "runs"}) {
        auto map = json[field];
        if (!map.isObject()) continue;
        for (const auto& entry : map) {
            auto entryKey = entry.getKey();
            if (!entryKey) continue;
            int count = static_cast<int>(entry.asInt().unwrapOr(0));
            if (count <= 0) continue;

            float runLength = runKeyLength(*entryKey);
            if (runLength < 0.f) continue;
            if (runLength != 100.f) runLength += 0.5f;

            seconds += static_cast<double>(wtSeconds) * (runLength / 100.0) * count;
        }
    }

    return seconds;
}


static std::string formatTime(uint64_t nanoseconds) {
    uint64_t totalSeconds = nanoseconds / 1'000'000'000ULL;
    uint64_t hours   = totalSeconds / 3600;
    uint64_t minutes = (totalSeconds % 3600) / 60;
    uint64_t seconds = totalSeconds % 60;

    std::string out;
    if (hours > 0)   out += fmt::format("{}h ", hours);
    if (minutes > 0) out += fmt::format("{}m ", minutes);
    out += fmt::format("{}s", seconds);
    return out;
}

static std::set<std::string> getLinkedLevels(
    const std::string& key,
    const std::optional<std::filesystem::path>& dtBase
) {
    if (!Mod::get()->getSettingValue<bool>("ignore-dt-links") && dtBase) {
        if (auto meta = readDTMeta(*dtBase, key)) {
            auto linked = (*meta)["LinkedLevels"].as<std::vector<std::string>>().unwrapOr(std::vector<std::string>{});
            if (!linked.empty())
                return {linked.begin(), linked.end()};
        }
    }

    auto& save = Mod::get()->getSaveContainer();
    if (!save.contains("links") || !save["links"].isObject() || !save["links"].contains(key))
        return {};
    auto v = save["links"][key].as<std::vector<std::string>>().unwrapOr(std::vector<std::string>{});
    return {v.begin(), v.end()};
}

static GJGameLevel* findLevelInCache(const std::string& key) {
    if (key.ends_with("-editor")) return nullptr;

    std::string idStr = key;
    for (const auto* suffix : {"-local", "-daily", "-gauntlet"}) {
        if (idStr.ends_with(suffix)) {
            idStr = idStr.substr(0, idStr.size() - std::string_view(suffix).size());
            break;
        }
    }

    auto idRes = geode::utils::numFromString<int>(idStr);
    if (idRes.isErr()) return nullptr;
    int id = idRes.unwrap();

    auto glm = GameLevelManager::sharedState();
    if (key.ends_with("-local"))
        return glm->getMainLevel(id, false);

    return typeinfo_cast<GJGameLevel*>(
        glm->m_onlineLevels->objectForKey(std::to_string(id))
    );
}

struct Totals {
    int totalAtt;
    int totalJumps;
    std::optional<uint64_t> totalTimeNs;
};

static Totals getTotals(GJGameLevel* level) {
    int totalAtt   = level->m_attempts;
    int totalJumps = level->m_jumps;

    auto dtBase = dtLevelSaveBase();
    auto key = getLevelKey(level);

    // Death Tracker estimates legacy playtime from the *current* level's length,
    // applying it to all (current + linked) attempts, so derive it once.
    float wtSeconds = levelLengthSeconds(level);

    // Use Death Tracker's legacy (estimated) playtime, derived from each level's
    // death/run history scaled by the level length.
    auto levelTimeNs = [&](const std::string& lk) -> uint64_t {
        double seconds = readDTLegacySeconds(*dtBase, lk, wtSeconds);
        return static_cast<uint64_t>(seconds * 1'000'000'000.0);
    };

    uint64_t totalNs = 0;
    if (dtBase)
        totalNs += levelTimeNs(key);

    for (const auto& lk : getLinkedLevels(key, dtBase)) {
        if (auto cached = findLevelInCache(lk)) {
            totalAtt   += cached->m_attempts;
            totalJumps += cached->m_jumps;
        } else if (dtBase) {
            if (auto meta = readDTMeta(*dtBase, lk))
                totalAtt += (*meta)["attempts"].asInt().unwrapOr(0);
        }
        if (dtBase && lk != key)
            totalNs += levelTimeNs(lk);
    }

    std::optional<uint64_t> totalTimeNs;
    if (dtBase)
        totalTimeNs = totalNs;

    return {totalAtt, totalJumps, totalTimeNs};
}

class $modify(MyEndLevelLayer, EndLevelLayer) {

    static void onModify(auto& self) {
        (void) self.setHookPriorityPost("EndLevelLayer::showLayer", Priority::Last);
    }

    void showLayer(bool p0) {
        EndLevelLayer::showLayer(p0);

        auto pl = m_playLayer;
        if (!pl || !pl->m_level || !m_mainLayer) return;

        auto level = pl->m_level;

        if (pl->m_isPracticeMode && !Mod::get()->getSettingValue<bool>("show-practice")) return;
        if (Mod::get()->getSettingValue<bool>("from-zero-only") && pl->m_startPosObject) return;
        if (level->isPlatformer()) return;

        bool showJumps = Mod::get()->getSettingValue<bool>("show-jumps");
        bool showTime  = Mod::get()->getSettingValue<bool>("show-time");
        auto [totalAtt, totalJumps, totalTimeNs] = getTotals(level);

        auto summary = m_mainLayer->getChildByID("summary-container");
        if (!summary) return;

        auto summaryWrapper = CCNode::create();
        summaryWrapper->setID("totals-summary"_spr);
        summaryWrapper->setAnchorPoint({0.5f, 0.5f});
        summaryWrapper->setContentSize({160.f, 50.f});
        auto summaryLayout = ColumnLayout::create();
        summaryLayout->setAxisReverse(true);
        summaryLayout->setAxisAlignment(AxisAlignment::Start);
        summaryLayout->setCrossAxisAlignment(AxisAlignment::Start);
        summaryLayout->setCrossAxisLineAlignment(AxisAlignment::Start);
        summaryLayout->setAutoScale(false);
        summaryLayout->setGap(2.f);
        summaryWrapper->setLayout(summaryLayout);

        auto makeSummaryLabel = [](const std::string& text) {
            auto lbl = CCLabelBMFont::create(text.c_str(), "goldFont.fnt");
            lbl->setScale(0.8f);
            return lbl;
        };

        auto totalAttLabel = makeSummaryLabel(fmt::format("Total Attempts: {}", formatCommas(totalAtt)));
        totalAttLabel->setID("total-attempts-label"_spr);
        summaryWrapper->addChild(totalAttLabel);

        if (showJumps) {
            auto totalJumpsLabel = makeSummaryLabel(fmt::format("Total Jumps: {}", formatCommas(totalJumps)));
            totalJumpsLabel->setID("total-jumps-label"_spr);
            summaryWrapper->addChild(totalJumpsLabel);
        }

        if (showTime && totalTimeNs) {
            auto totalTimeLabel = makeSummaryLabel(fmt::format("Total Time: {}", formatTime(*totalTimeNs)));
            totalTimeLabel->setID("total-time-label"_spr);
            totalTimeLabel->setLayoutOptions(
                AxisLayoutOptions::create()->setCrossAxisAlignment(AxisAlignment::Center)
            );
            summaryWrapper->addChild(totalTimeLabel);
        }

        summaryWrapper->updateLayout();
        summary->addChild(summaryWrapper);
        summary->updateLayout();
    }
};

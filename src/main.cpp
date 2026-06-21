#include <Geode/Geode.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <cvolton.level-id-api/include/EditorIDs.hpp>

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
    auto dt = Loader::get()->getInstalledMod("elohmrow.death_tracker");
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


static uint64_t readDTPlaytimeNs(
    const std::filesystem::path& base,
    const std::string& key
) {
    auto res = file::readJson(base / key / "general.dt");
    if (res.isErr()) return 0;

    auto general    = res.unwrap()["playtimeGeneral"];
    uint64_t fromZero = static_cast<uint64_t>(general["playtimeF0"].asInt().unwrapOr(0));
    uint64_t fromRuns = static_cast<uint64_t>(general["playtimeRuns"].asInt().unwrapOr(0));
    return fromZero + fromRuns;
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
    std::optional<uint64_t> totalTimeNs;
    if (dtBase) totalTimeNs = readDTPlaytimeNs(*dtBase, key);

    for (const auto& lk : getLinkedLevels(key, dtBase)) {
        if (auto cached = findLevelInCache(lk)) {
            totalAtt   += cached->m_attempts;
            totalJumps += cached->m_jumps;
        } else if (dtBase) {
            if (auto meta = readDTMeta(*dtBase, lk))
                totalAtt += (*meta)["attempts"].asInt().unwrapOr(0);
        }
        if (totalTimeNs && lk != key)
            *totalTimeNs += readDTPlaytimeNs(*dtBase, lk);
    }

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
            summaryWrapper->addChild(totalTimeLabel);
        }

        summaryWrapper->updateLayout();
        summary->addChild(summaryWrapper);
        summary->updateLayout();
    }
};

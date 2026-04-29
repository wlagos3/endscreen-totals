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

static std::pair<int, int> getTotals(GJGameLevel* level) {
    int totalAtt   = level->m_attempts;
    int totalJumps = level->m_jumps;

    auto dtBase = dtLevelSaveBase();

    for (const auto& lk : getLinkedLevels(getLevelKey(level), dtBase)) {
        if (auto cached = findLevelInCache(lk)) {
            totalAtt   += cached->m_attempts;
            totalJumps += cached->m_jumps;
            continue;
        }
        if (dtBase) {
            if (auto meta = readDTMeta(*dtBase, lk))
                totalAtt += (*meta)["attempts"].asInt().unwrapOr(0);
        }
    }

    return {totalAtt, totalJumps};
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
        auto [totalAtt, totalJumps] = getTotals(level);

        auto summary = m_mainLayer->getChildByID("summary-container");
        if (!summary) return;

        auto makeLabel = [](const std::string& text) {
            auto lbl = CCLabelBMFont::create(text.c_str(), "goldFont.fnt");
            lbl->setScale(0.3f);
            return lbl;
        };

        auto attLabel = makeLabel(fmt::format("Attempts: {}", formatCommas(totalAtt)));
        attLabel->setID("total-attempts-label"_spr);
        summary->addChild(attLabel);

        if (showJumps) {
            auto jumpsLabel = makeLabel(fmt::format("Jumps: {}", formatCommas(totalJumps)));
            jumpsLabel->setID("total-jumps-label"_spr);
            summary->addChild(jumpsLabel);
        }

        summary->updateLayout();
    }
};

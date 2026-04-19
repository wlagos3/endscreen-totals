
#include <Geode/Geode.hpp>

using namespace geode::prelude;
#include <Geode/modify/EndLevelLayer.hpp>
class $modify(EndLevelLayer) {

	void customSetup() {
		EndLevelLayer::customSetup();
		auto mainLayer = this->getChildByID("main-layer");
		CCLabelBMFont* attemptsLabel = typeinfo_cast<CCLabelBMFont*, CCNode*>(mainLayer->getChildByID("attempts-label"));
		CCLabelBMFont* jumpsLabel = typeinfo_cast<CCLabelBMFont*, CCNode*>(mainLayer->getChildByID("jumps-label"));
		
		if(m_playLayer->m_isPracticeMode || m_playLayer->m_level->isPlatformer()){
			return;
		}

		if(&m_playLayer->m_level->m_attempts != nullptr){
			int attempts = m_playLayer->m_level->m_attempts.value();
			std::string attemptsString = std::to_string(attempts);
			int n = attemptsString.length() - 3;
			int end = (attempts >= 0) ? 0 : 1;
			while (n > end) {
				attemptsString.insert(n, ",");
				n -= 3;
			}
			attemptsLabel->setString(attemptsString.insert(0, "Total Attempts: ").c_str());
		}
		
		if(&m_playLayer->m_level->m_jumps != nullptr){
			int jumps = m_playLayer->m_level->m_jumps.value();

			std::string jumpsString = std::to_string(jumps);

			int n = jumpsString.length() - 3;
			int end = (jumps >= 0) ? 0 : 1;
			while (n > end) {
				jumpsString.insert(n, ",");
				n -= 3;
			}
			jumpsLabel->setString(jumpsString.insert(0, "Total Jumps: ").c_str());
		}
		return;
	}

	

};
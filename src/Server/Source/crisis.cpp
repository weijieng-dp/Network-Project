#include "crisis.h"

namespace {
	static const int64_t CRISIS_SHOCK_MS = 100000;       // 30s flash crash
	static const int64_t CRISIS_PANIC_MS = 200000;       // 1min panic selling
	static const int64_t CRISIS_CRAZE_MS = 100000;		// 30s crazed buying
	static const int64_t CRISIS_RAND_MS = 100000;		// 1min wild wild west
	static const int64_t MAX_CRISIS_MS   = 30000;
}

std::atomic<int>						CrisisManager::currState(0);
CrisisManager::time						CrisisManager::cooldownTime;
CrisisManager::time						CrisisManager::timeSinceCrisis;
CrisisManager::time						CrisisManager::startTime;
CrisisManager::time						CrisisManager::prevTime;
CrisisManager::time						CrisisManager::crisisDuration;

std::mt19937							CrisisManager::gen;

std::uniform_int_distribution<uint32_t>	CrisisManager::randCrisis;
std::uniform_int_distribution<uint32_t>	CrisisManager::shockDur;
std::uniform_int_distribution<uint32_t>	CrisisManager::panicDur;
std::uniform_int_distribution<uint32_t>	CrisisManager::crazeDur;
std::uniform_int_distribution<uint32_t>	CrisisManager::randDur;

CrisisManager::time CrisisManager::now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()); }

void CrisisManager::Init() {
	// Can pass in information from files to initialize the timesincecrisis and cooldown time, can also initialize the currState
	// from persistant data.

	startTime = now();
	prevTime = now();
	timeSinceCrisis = std::chrono::milliseconds(0);
	cooldownTime = std::chrono::milliseconds(300000);

	// Initializing random devices
	std::random_device rd;  // Will be used to obtain a seed for the random number engine
	gen = std::mt19937(rd()); // Standard mersenne_twister_engine seeded with rd()

	using parami = std::uniform_int_distribution<uint32_t>::param_type;

	randCrisis.param(parami(0, MAX_CRISIS_MS));
	shockDur.param(parami((uint32_t)(CRISIS_SHOCK_MS * 0.75), (uint32_t)(CRISIS_SHOCK_MS * 1.25)));
	panicDur.param(parami((uint32_t)(CRISIS_PANIC_MS * 0.75), (uint32_t)(CRISIS_PANIC_MS * 1.25)));
	crazeDur.param(parami((uint32_t)(CRISIS_CRAZE_MS * 0.75), (uint32_t)(CRISIS_CRAZE_MS * 1.25)));
	randDur.param(parami((uint32_t)(CRISIS_RAND_MS * 0.25), (uint32_t)(CRISIS_RAND_MS * 1.75)));
}

void CrisisManager::Update() {
	time dt = now() - prevTime;
	prevTime = now();

	if (cooldownTime > std::chrono::milliseconds(0)) {
		cooldownTime -= dt;
		return;
	}

	timeSinceCrisis += dt;
	
	// If there is no crisis rn...
	if (currState == static_cast<int>(Crisis::NONE)) {

		// randomly select a crisis, chance to not have crisis decreases as timesincecrisis increases.
		if (randCrisis(gen) > timeSinceCrisis.count()) {
			int crisis = randCrisis(gen) % 4;
			crisis++;

			currState.exchange(crisis);
			timeSinceCrisis = std::chrono::milliseconds(0);

			switch (static_cast<Crisis>(crisis)) {
			case Crisis::PANIC:
				crisisDuration = std::chrono::milliseconds(panicDur(gen));
				break;
			case Crisis::SHOCK:
				crisisDuration = std::chrono::milliseconds(shockDur(gen));
				break;
			case Crisis::CRAZE:
				crisisDuration = std::chrono::milliseconds(crazeDur(gen));
				break;
			case Crisis::RANDOM:
				crisisDuration = std::chrono::milliseconds(randDur(gen));
				break;
			}

		}
	}
	// If in a crisis...
	else {
		// Set back to no crisis if timesincecrisis > the duration
		if (timeSinceCrisis > crisisDuration) {
			// 5 mins cooldown period
			cooldownTime = std::chrono::milliseconds(300000);
			timeSinceCrisis = std::chrono::milliseconds(0);
			currState.exchange((int)Crisis::NONE);
		}
	}
}
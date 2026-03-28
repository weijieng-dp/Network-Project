#include "types.h"
#include "global.h"
#include <atomic>
#include <chrono>
#include <random>

class CrisisManager {
	using time = std::chrono::milliseconds;

	static std::atomic<int> currState;
	static time cooldownTime;
	static time timeSinceCrisis;
	static time startTime;
	static time prevTime;
	static time crisisDuration;
	
	static time now();

	static std::mt19937 gen;
	static std::uniform_int_distribution<uint32_t> randCrisis;

	static std::uniform_int_distribution<uint32_t> shockDur;
	static std::uniform_int_distribution<uint32_t> panicDur;
	static std::uniform_int_distribution<uint32_t> crazeDur;
	static std::uniform_int_distribution<uint32_t> randDur;

public:
	enum Crisis : uint8_t {
		NONE, SHOCK, PANIC, CRAZE, RANDOM
	};

	static Crisis getState() { return static_cast<Crisis>(currState.load()); }
	static void setState(Crisis c) { currState.exchange(static_cast<int>(c)); }
	
	static void Init();
	static void Update();

};
#include "util.h"
#include <iostream>
#include <map>
#include <set>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include <thread>
#include <chrono>
#include <stdio.h>
#include <algorithm>
#include <queue>

using namespace std;
using namespace rapidjson;
using namespace std::chrono;

string server = "https://api.steampowered.com/";
string api = "IGameServersService/GetServerList/v1";
string apikey = "";
string appid = "225840"; // Sven Co-op
//string filter = "\\appid\\" + appid + "\\dedicated\\1";
string filter = "\\appid\\" + appid;
string dataPath = "data/";
string statsPath = "data/stats/active/"; // entire stat history for actively tracked servers
string liveDataPath = "data/stats/live/"; // most recent stats
string avgDataPath = "data/stats/avg/"; // all stats but averaged for better speed/size
string archivePath = "data/stats/archive/"; // old files that maybe should be deleted
string rankHistoryPath = "data/stats/rank/"; // past server rankings
string archiveRankPath = "data/stats/archive/rank/"; // archived because ranking formula may change
string rankPath = "data/rankings.txt"; // ordered list of server IDs by rank
string serverInfoPath = "data/tracker.json"; // ordered list of server IDs by rank

const char* statFileMagicBytes = "SVTK";
const char* rankFileMagicBytes = "SVRK";

//#define DEBUG_MODE

#ifdef DEBUG_MODE
	#define SERVER_DEAD_SECONDS (60)
	#define SERVER_UNREACHABLE_TIME (30)
	#define STAT_WRITE_FREQ 10
	#define RANK_FREQ 20
#else
	#define SERVER_DEAD_SECONDS (60*60*24*7) // seconds before a server is considered "dead" and its stats are archived
	#define SERVER_UNREACHABLE_TIME (60*5) // write unreachable stat after this time
	#define STAT_WRITE_FREQ 60 // how often to write stats
	#define RANK_FREQ 60*60 // How often to compute server rankings
#endif

#define STAT_FILE_VERSION 1
#define RANK_STAT_MAX_AGE 60*60*24*14 // ignore stats older than this when computing ranks
#define RANK_STAT_INTERVAL 60 // gaps between rank data points
#define TOTAL_RANK_DATA_POINTS ((RANK_STAT_MAX_AGE) / (RANK_STAT_INTERVAL))

#define MAX_LIVE_STATS_AGE_RAW 60*60*24*30 // max number of raw stats written to live data for the web
#define AVG_STAT_FILE_INTERVAL 60*60 // interval for averaged stats

#pragma pack(push, 1)
struct StatFileHeader {
	uint32_t version;
	char magic[4]; // "SVTK"
};

#define FL_PCNT_TIME16 64		// time delta is 16 bits and relative to the last stat
#define FL_PCNT_TIME32 128		// time is a 32 bit absoulte value
#define PCNT_FL_MASK (FL_PCNT_TIME16|FL_PCNT_TIME32)
#define PCNT_UNREACHABLE (PCNT_FL_MASK) // if both are set, the server is unreachable
// if neither are set, time delta is 8-bits and relative to the last stat

#define FL_RANK_RANK16 64		// ranking is not a delta and so does not fit in this byte
#define FL_RANK_TIME32 128		// time delta is 32 bits instead of 16
#define FL_RANK_MASK (FL_RANK_RANK16|FL_RANK_TIME32)
#define RANK_SIGN_BIT 32		// bit containing the sign of the delta

#pragma pack(pop)

#define FL_SERVER_DEDICATED 1
#define FL_SERVER_SECURE 2
#define FL_SERVER_LINUX 4 // else windows

struct ServerState {
	string addr; // port separator converted to filename safe character
	string name;
	string map;
	uint8_t players;
	uint8_t maxPlayers;
	uint8_t bots;
	bool unreachable;
	uint8_t flags;

	uint32_t lastWriteTime; // last time a player count stat was written (epoch seconds)
	uint32_t lastResponseTime; // last time data was received for this server
	uint32_t rankSum; // sum of player counts over rankDataPoints data points
	
	string getStatFilePath();
	string getStatArchiveFilePath();
	string getLiveStatFilePath();
	string getLiveAvgStatFilePath();
	string getRankHistFilePath();
	string getRankArchiveFilePath();
	uint32_t secondsSinceLastResponse();
	string displayName();
	void init();
}; 

void ServerState::init() {
	addr = "";
	name = "";
	map = "";
	players = 0;
	unreachable = false;
	flags = 0;
	lastWriteTime = 0;
	lastResponseTime = 0;
	maxPlayers = 0;
	bots = 0;
	rankSum = 0;
}

struct WriteStats {
	int bytesWritten = 0;
	int serversUpdated = 0;
	int bytesRead = 0;
};

WriteStats g_writeStats;
map<string, ServerState> g_servers;

uint32_t g_lastRankTime = 0;
uint32_t g_lastUpdateTime = 0;

bool writeServerStat(ServerState& state, int newPlayerCount, bool unreachable, uint32_t now);
bool createServerStatFile(ServerState& newState);
bool loadServerHistory(ServerState& state, uint32_t now, bool programRestarted);
bool validateStatName(string name);

string ServerState::getStatFilePath() {
	return statsPath + addr + ".dat";
}

string ServerState::getStatArchiveFilePath() {
	return archivePath + addr + ".dat";
}

string ServerState::getLiveStatFilePath() {
	return liveDataPath + addr + ".dat";
}

string ServerState::getLiveAvgStatFilePath() {
	return avgDataPath + addr + ".dat";
}

string ServerState::getRankHistFilePath() {
	return rankHistoryPath + addr + ".dat";
}

string ServerState::getRankArchiveFilePath() {
	return archiveRankPath + addr + ".dat";
}

uint32_t ServerState::secondsSinceLastResponse() {
	return getEpochSeconds() - lastResponseTime;
}

string ServerState::displayName() {
	return "[" + addr + "] " + name;
}

bool loadApiKey() {
	int length;
	char* buffer = loadFile("api_key.txt", length);
	if (!buffer) {
		printf("Bad api key file\n");
		return false;
	}
	apikey = buffer;
	delete[] buffer;
	return true;
}

bool getServerListJson(Value& serverList, Document& json) {
	string requestUrl = server + api + "?key=" + apikey + "&filter=" + filter + "&limit=20000";

	string response_string;
	int resp_code = webRequest(requestUrl, response_string);

	if (resp_code != 200) {
		printf("Failed to fetch server list (HTTP response code %d)\n", resp_code);
		return false;
	}

	json.Parse(response_string.c_str());
	
	if (!json.HasMember("response")) {
		printf("Json missing 'response' member:\n%s\n", stringifyJson(json).c_str());
		return false;
	}

	Value& temp = json["response"];

	if (!temp.HasMember("servers")) {
		printf("Json missing 'servers' member:\n%s\n", stringifyJson(json).c_str());
		return false;
	}

	serverList = temp["servers"];
	return true;
}

bool parseSteamServerJson(Value& json, ServerState& state) {
	if (!(json.HasMember("name") && json.HasMember("map") && json.HasMember("players") && json.HasMember("flags")
			&& json.HasMember("max_players") && json.HasMember("addr") && json.HasMember("bots"))) {
		printf("Server json missing values\n");
		return false;
	}

	state.init();
	state.players = json["players"].GetInt();
	state.addr = replaceString(json["addr"].GetString(), ":", "_");
	state.name = json["name"].GetString();
	state.map = json["map"].GetString();
	state.maxPlayers = json["max_players"].GetUint();
	state.bots = json["bots"].GetUint();
	state.flags = json["flags"].GetUint();
	return true;
}

bool parseProgramServerJson(Value& json, ServerState& state) {
	if (!(json.HasMember("name") && json.HasMember("flags") && json.HasMember("max_players") && json.HasMember("time"))) {
		printf("Program json missing values\n");
		return false;
	}

	state.init();
	state.name = json["name"].GetString();
	state.maxPlayers = json["max_players"].GetUint();
	state.flags = json["flags"].GetUint();
	state.lastResponseTime = json["time"].GetUint();
	// players/rank and other live data should be loaded/calculated soon
	return true;
}

// will unarchive the stat file if it exists, and validate the header
FILE* loadStatFile(ServerState& state) {
	string fpath = state.getStatFilePath();
	string archivePath = state.getStatArchiveFilePath();

	FILE* file = NULL;

	if (fileExists(fpath)) {
		errno = 0;
		file = fopen(fpath.c_str(), "rb");
		if (!file) {
			printf("Failed to open stat file (%d): %s\n", errno, fpath.c_str());
			return NULL;
		}
	}
	else if (fileExists(archivePath)) {
		printf("Unarchive revived server: %s\n", state.addr.c_str());
		errno = 0;
		if (rename(archivePath.c_str(), fpath.c_str()) == -1) {
			printf("Unarchive failed. Rename error %d: %s", errno, archivePath.c_str());
			return NULL;
		}

		errno = 0;
		file = fopen(fpath.c_str(), "rb");
		if (!file) {
			printf("Failed to open stat file (%d): %s\n", errno, archivePath.c_str());
			return NULL;
		}
	}
	else {
		printf("Stat file does not exist for: %s\n", state.addr.c_str());
		return NULL;
	}

	StatFileHeader header;

	errno = 0;
	if (!fread(&header, sizeof(StatFileHeader), 1, file)) {
		printf("Failed to read stat file (error %d): %s\n", errno, fpath.c_str());
		fclose(file);
		return NULL;
	}

	if (header.version != STAT_FILE_VERSION) {
		printf("Bad version %d in stat file: %s\n", header.version, fpath.c_str());
		fclose(file);
		return NULL;
	}

	if (strncmp(header.magic, statFileMagicBytes, 4)) {
		string magic = string(header.magic, 4);
		printf("Bad magic bytes '%s' in stat file: %s\n", magic.c_str(), fpath.c_str());
		fclose(file);
		return NULL;
	}

	return file;
}

// will unarchive the rank file if it exists, and validate the header
FILE* loadRankFile(ServerState& state) {
	string fpath = state.getRankHistFilePath();
	string archivePath = state.getRankArchiveFilePath();

	FILE* file = NULL;

	if (fileExists(fpath)) {
		errno = 0;
		file = fopen(fpath.c_str(), "rb");
		if (!file) {
			printf("Failed to open rank file (%d): %s\n", errno, fpath.c_str());
			return NULL;
		}
	}
	else if (fileExists(archivePath)) {
		printf("Unarchive rank file: %s\n", state.addr.c_str());
		errno = 0;
		if (rename(archivePath.c_str(), fpath.c_str()) == -1) {
			printf("Unarchive rank failed. Rename error %d: %s", errno, archivePath.c_str());
			return NULL;
		}

		errno = 0;
		file = fopen(fpath.c_str(), "rb");
		if (!file) {
			printf("Failed to open rank file (%d): %s\n", errno, archivePath.c_str());
			return NULL;
		}
	}
	else {
		printf("Rank file does not exist for: %s\n", state.addr.c_str());
		return NULL;
	}

	StatFileHeader header;

	errno = 0;
	if (!fread(&header, sizeof(StatFileHeader), 1, file)) {
		printf("Failed to read rank file (error %d): %s\n", errno, fpath.c_str());
		fclose(file);
		return NULL;
	}

	if (header.version != STAT_FILE_VERSION) {
		printf("Bad version %d in rank file: %s\n", header.version, fpath.c_str());
		fclose(file);
		return NULL;
	}

	if (strncmp(header.magic, rankFileMagicBytes, 4)) {
		string magic = string(header.magic, 4);
		printf("Bad magic bytes '%s' in rank file: %s\n", magic.c_str(), fpath.c_str());
		fclose(file);
		return NULL;
	}

	return file;
}


// false indicates a problem with the file
bool loadServerHistory(ServerState& state, uint32_t now, bool programRestarted) {
	FILE* file = loadStatFile(state);

	string dispName = state.displayName();
	//printf("History for %s\n", dispName.c_str());

	uint32_t rankStartTime = now - RANK_STAT_MAX_AGE;
	uint32_t nextRankTime = rankStartTime;
	state.rankSum = 0;
	uint32_t rankDataPoints = 0;

	state.players = 0;

	while (1) {
		uint8_t stat;
		if (!fread(&stat, sizeof(uint8_t), 1, file)) {
			break;
		}
		uint8_t flags = stat & PCNT_FL_MASK;
		uint8_t newPlayerCount = 0;

		if ((stat & PCNT_FL_MASK) == PCNT_UNREACHABLE) {
			newPlayerCount = 0;
			state.unreachable = true;
			flags = (stat << 2) & PCNT_FL_MASK;
			if (stat & 0x0f) {
				printf("Invalid flags in unreachable byte %X\n", (int)stat);
			}
		}
		else {
			state.unreachable = false;
			newPlayerCount = stat & ~PCNT_FL_MASK;
			if (state.players > 32) {
				printf("Invalid player count\n");
			}
		}

		int dt = 0;

		if (flags & FL_PCNT_TIME32) {
			uint32_t newTime = 0;
			if (!fread(&newTime, sizeof(uint32_t), 1, file)) {
				printf("Failed to read stat time\n");
				return false;
			}
			state.lastWriteTime = newTime;
		}
		else if (flags & FL_PCNT_TIME16) {
			uint16_t delta;
			if (!fread(&delta, sizeof(uint16_t), 1, file)) {
				printf("Failed to read stat time\n");
				return false;
			}
			state.lastWriteTime += delta;
			dt = delta;
		}
		else {
			uint8_t delta;
			if (!fread(&delta, sizeof(uint8_t), 1, file)) {
				printf("Failed to read stat time\n");
				return false;
			}
			state.lastWriteTime += delta;
			dt = delta;
		}

		int backfills = 0;
		while (state.lastWriteTime >= nextRankTime) { // back-fill gaps in data with last known player count
			state.rankSum += state.players;
			rankDataPoints++;
			backfills++;
			nextRankTime = rankStartTime + rankDataPoints * RANK_STAT_INTERVAL;
		}

		state.players = newPlayerCount;

		//printf("Time %u, delta %d, count %d, unreachable %d\n", state.lastWriteTime, dt, (int)state.players, (int)state.unreachable);
	}

	// now catch up to the current time
	int backfills = 0;
	while (now >= nextRankTime) { // back-fill gaps in data with last known player count
		state.rankSum += state.players;
		rankDataPoints++;
		backfills++;
		nextRankTime = rankStartTime + rankDataPoints * RANK_STAT_INTERVAL;
	}
	rankDataPoints--;
	if (rankDataPoints != TOTAL_RANK_DATA_POINTS) {
		state.rankSum = 0;
		printf("Unexpected rank data points %u / %u\n", rankDataPoints, TOTAL_RANK_DATA_POINTS);
	}

	g_writeStats.bytesRead += ftell(file);
	fclose(file);

	if (state.lastWriteTime > now) {
		printf("Parsed invalid time +%u\n", state.lastWriteTime - now);
		return false;
	}

	if (state.lastResponseTime == 0)
		state.lastResponseTime = state.lastWriteTime;

	// write unreachable stat at the same time as the last stat,
	// which will be when the program was stopped

	uint32_t deadTime = state.secondsSinceLastResponse();
	if (programRestarted && !state.unreachable && deadTime > SERVER_UNREACHABLE_TIME) {
		writeServerStat(state, 0, true, state.lastWriteTime);
		state.players = 0;
		g_writeStats.serversUpdated -= 1;
		printf("append unreachable stat (deadtime %us): %s\n", deadTime, dispName.c_str());
	}
	else {
		//printf("Loaded server state: %s\n", dispName.c_str());
	}

	return true;
}

bool writeStatHeader(FILE* file, const char* magic, string fpath) {
	StatFileHeader header;
	header.version = STAT_FILE_VERSION;
	memcpy(header.magic, magic, 4);

	errno = 0;
	if (!fwrite(&header, sizeof(StatFileHeader), 1, file)) {
		printf("Failed to write to stat file header (error %d): %s\n", errno, fpath.c_str());
		return false;
	}

	return true;
}

bool fwriteVerbose(const void* buffer, size_t sz, FILE* file, const char* desc) {
	errno = 0;
	if (!fwrite(buffer, sz, 1, file)) {
		printf("Failed to write %s (error %d)\n", desc, errno);
		return false;
	}
	return true;
}

uint8_t getDeltaFlags(uint32_t timeDelta) {
	if (timeDelta > 65535) {
		return FL_PCNT_TIME32;
	}
	else if (timeDelta > 255) {
		return FL_PCNT_TIME16;
	}
	else {
		return 0;
	}
}

bool writeDelta(uint32_t timeFrom, uint32_t timeTo, FILE* file) {
	uint32_t timeDelta = timeTo - timeFrom;
	uint8_t flags = getDeltaFlags(timeDelta);

	if (flags & FL_PCNT_TIME32) {
		if (!fwriteVerbose(&timeTo, sizeof(uint32_t), file, "time delta")) {
			return false;
		}
	}
	else if (flags & FL_PCNT_TIME16) {
		if (!fwriteVerbose(&timeDelta, sizeof(uint16_t), file, "time delta")) {
			return false;
		}
	}
	else {
		if (!fwriteVerbose(&timeDelta, sizeof(uint8_t), file, "time delta")) {
			return false;
		}
	}
}

bool writeLiveStatFiles(ServerState& state, uint32_t now) {
	FILE* historyFile = loadStatFile(state);

	string liveDataPath = state.getLiveStatFilePath();
	string liveAvgDataPath = state.getLiveAvgStatFilePath();
	FILE* liveFile = fopen(liveDataPath.c_str(), "wb");
	FILE* avgFile = fopen(liveAvgDataPath.c_str(), "wb");

	if (!liveFile || !avgFile || !historyFile) {
		printf("Failed to write live/avg stats: %s\n", state.addr.c_str());
		if (liveFile) {
			fclose(liveFile);
			remove(liveDataPath.c_str());
		}
		if (avgFile) {
			fclose(avgFile);
			remove(liveAvgDataPath.c_str());
		}
		if (historyFile) {
			fclose(historyFile);
		}
		return false;
	}

	writeStatHeader(liveFile, statFileMagicBytes, liveDataPath);
	writeStatHeader(avgFile, statFileMagicBytes, liveDataPath);

	bool withinLiveStatRange = false;
	uint32_t statTime = 0;

	uint32_t numStatsPerAvg = AVG_STAT_FILE_INTERVAL / STAT_WRITE_FREQ;
	uint32_t lastAvgStatWrite = 0;
	uint32_t avgStatSum = 0;
	uint32_t lastAvgStatTime = 0;
	uint8_t lastPlayerCount = 0;

	deque<uint8_t> avgHistory;
	bool success = true;

	while (1) {		
		uint8_t stat;
		if (!fread(&stat, sizeof(uint8_t), 1, historyFile)) {
			break;
		}
		uint8_t flags = stat & PCNT_FL_MASK;
		uint8_t playerCount = 0;

		if (withinLiveStatRange && !fwriteVerbose(&stat, sizeof(uint8_t), liveFile, "live stat")) {
			success = false;
			break;
		}

		if ((stat & PCNT_FL_MASK) == PCNT_UNREACHABLE) {
			flags = (stat << 2) & PCNT_FL_MASK;
			if (stat & 0x0f) {
				printf("Invalid flags in unreachable byte %X\n", (int)stat);
			}
		}
		else {
			playerCount = stat & ~PCNT_FL_MASK;
			if (playerCount > 32) {
				printf("Invalid player count\n");
			}
		}

		if (flags & FL_PCNT_TIME32) {
			if (!fread(&statTime, sizeof(uint32_t), 1, historyFile)) {
				printf("Failed to read stat time\n");
				success = false;
				break;
			}
			if (withinLiveStatRange && !fwriteVerbose(&statTime, sizeof(uint32_t), liveFile, "live stat")) {
				success = false;
				break;
			}
		}
		else if (flags & FL_PCNT_TIME16) {
			uint16_t delta;
			if (!fread(&delta, sizeof(uint16_t), 1, historyFile)) {
				printf("Failed to read stat time\n");
				success = false;
				break;
			}
			if (withinLiveStatRange && !fwriteVerbose(&delta, sizeof(uint16_t), liveFile, "live stat")) {
				success = false;
				break;
			}
			statTime += delta;
		}
		else {
			uint8_t delta;
			if (!fread(&delta, sizeof(uint8_t), 1, historyFile)) {
				printf("Failed to read stat time\n");
				success = false;
				break;
			}
			if (withinLiveStatRange && !fwriteVerbose(&delta, sizeof(uint8_t), liveFile, "live stat")) {
				success = false;
				break;
			}
			statTime += delta;
		}

		if (!withinLiveStatRange && statTime > now - MAX_LIVE_STATS_AGE_RAW) {
			withinLiveStatRange = true;

			// first stat should always write the full time
			uint8_t firstStat = FL_PCNT_TIME32 | playerCount;
			if ((stat & PCNT_FL_MASK) == PCNT_UNREACHABLE) {
				firstStat = PCNT_UNREACHABLE | (FL_PCNT_TIME32 >> 2);
			}

			fwrite(&firstStat, sizeof(uint8_t), 1, liveFile);
			fwrite(&statTime, sizeof(uint32_t), 1, liveFile);
		}

		// write averaged data
		if (lastAvgStatTime == 0) {
			lastAvgStatTime = statTime;
		}
		while (statTime > lastAvgStatTime) { // back-fill gaps
			avgHistory.push_back(lastPlayerCount);
			if (avgHistory.size() > numStatsPerAvg) {
				avgHistory.pop_front();
			}
			lastAvgStatTime += RANK_STAT_INTERVAL;
		}

		uint32_t avgDelta = statTime - lastAvgStatWrite;
		if (avgDelta >= AVG_STAT_FILE_INTERVAL && avgHistory.size() == numStatsPerAvg) {
			uint8_t avgFlags = getDeltaFlags(avgDelta);

			float total = 0;
			for (uint8_t count : avgHistory) {
				total += count;
			}
			total /= (float)avgHistory.size();

			uint8_t avgCount = (uint8_t)(total + 0.5f);
			if (avgCount > 32) {
				printf("Impossible average: %d > 32\n", (int)avgCount);
				avgCount = 0;
			}

			uint8_t avgStat = avgFlags | avgCount;

			fwrite(&avgStat, sizeof(uint8_t), 1, avgFile);
			writeDelta(lastAvgStatWrite, statTime, avgFile);

			lastAvgStatWrite = statTime;
		}

		lastPlayerCount = playerCount;
	}

	fclose(historyFile);
	fclose(liveFile);
	fclose(avgFile);

	return success;
}

bool createServerStatFile(ServerState& newState) {
	string dispName = newState.displayName();
	printf("New server: %s\n", dispName.c_str());

	string fpath = newState.getStatFilePath();
	string archivePath = newState.getStatArchiveFilePath();
	if (fileExists(fpath) || fileExists(archivePath)) {
		printf("Stat file already exists: %s\n", dispName.c_str());
		return loadServerHistory(newState, getEpochMillis(), true);
	}

	FILE* file = fopen(fpath.c_str(), "wb");
	if (!file) {
		printf("Failed to create stat file: %s\n", fpath.c_str());
		return false;
	}

	writeStatHeader(file, statFileMagicBytes, fpath);

	g_writeStats.bytesWritten += sizeof(StatFileHeader);
	newState.players = 255; // force a stat write
	fclose(file);

	return true;
}

bool writeServerStat(ServerState& state, int newPlayerCount, bool unreachable, uint32_t now) {
	uint32_t writeTimeDelta = now - state.lastWriteTime;
	string dispName = state.displayName();

	if (state.lastWriteTime > now) {
		printf("Invalid last write time! +%u\n", state.lastWriteTime - now);
		return false;
	}
	if (state.players == newPlayerCount && unreachable == state.unreachable) {
		return true; // no delta to write
	}

	string fpath = state.getStatFilePath();
	FILE* file = fopen(fpath.c_str(), "ab");
	if (!file) {
		printf("Failed to reopen stat file: %s\n", fpath.c_str());
		return false;
	}

	if (!unreachable && state.unreachable) {
		uint32_t unresponsiveDelta = now - state.lastResponseTime;
		printf("Server is responding again (%.1f minutes): %s\n", unresponsiveDelta / 60.0f, dispName.c_str());
	}
	else if (state.players != 255 && !unreachable) {
		//printf("%d -> %d after %us: %s\n", state.players, newPlayerCount, writeTimeDelta, dispName.c_str());
	}

	state.players = unreachable ? 0 : newPlayerCount;

	uint8_t stat = state.players;
	uint8_t timeFlag = 0;

	if (writeTimeDelta > 65535) {
		timeFlag = FL_PCNT_TIME32;
	}
	else if (writeTimeDelta > 255) {
		timeFlag = FL_PCNT_TIME16;
	}
	
	if (unreachable) {
		// player count unknown, so use the unused bits for more flags
		stat = PCNT_UNREACHABLE | (timeFlag >> 2);
	}
	else {
		stat |= timeFlag;
	}
	fwrite(&stat, sizeof(uint8_t), 1, file);
	
	if (writeTimeDelta > 65535) {
		g_writeStats.bytesWritten += 1 + 4;
		if (!fwrite(&now, sizeof(uint32_t), 1, file)) {
			printf("Stat write failed\n");
			return false;
		}
	}
	else if (writeTimeDelta > 255) {
		g_writeStats.bytesWritten += 1 + 2;
		if (!fwrite(&writeTimeDelta, sizeof(uint16_t), 1, file)) {
			printf("Stat write failed\n");
			return false;
		}
	}
	else {
		g_writeStats.bytesWritten += 1 + 1;
		if (!fwrite(&writeTimeDelta, sizeof(uint8_t), 1, file)) {
			printf("Stat write failed\n");
			return false;
		}
	}

	fclose(file);

	state.lastWriteTime = now;
	state.unreachable = unreachable;

	writeLiveStatFiles(state, now);

	g_writeStats.serversUpdated++;
	return true;
}

bool archiveFile(string src, string dst) {
	if (fileExists(dst)) {
		printf("Archive failed. Destination exists: %s\n", dst.c_str());
		return false;
	}
	else if (!fileExists(src)) {
		printf("Archive failed. Source file missing: %s\n", src.c_str());
		return false;
	}

	errno = 0;
	if (rename(src.c_str(), dst.c_str()) == -1) {
		printf("Archive failed. Rename error %d: %s\n", errno, src.c_str());
		return false;
	}
}

bool archiveStats(string serverId) {
	if (g_servers.find(serverId) == g_servers.end()) {
		printf("Archive failed. No server with ID: %s\n", serverId.c_str());
		return false;
	}
	ServerState& state = g_servers[serverId];

	if (!archiveFile(state.getStatFilePath(), state.getStatArchiveFilePath())) {
		return false;
	}
	archiveFile(state.getRankHistFilePath(), state.getRankArchiveFilePath());

	// these files can be re-generated lated
	string livePath = state.getLiveStatFilePath();
	string avgPath = state.getLiveAvgStatFilePath();
	remove(livePath.c_str());
	remove(avgPath.c_str());
	
	printf("Archived server: %s\n", serverId.c_str());

	return true;
}

void updateStats(Document& doc, Value& serverList, uint32_t now) {
	int numServers = serverList.GetArray().Size();

	set<string> updatedServers;
	int totalWrites = 0;

	g_writeStats.bytesWritten = 0;
	g_writeStats.serversUpdated = 0;

	for (int i = 0; i < numServers; i++) {
		ServerState newState;
		if (!parseSteamServerJson(serverList[i], newState)) {
			continue;
		}

		string id = newState.addr;
		updatedServers.insert(newState.addr);

		if (g_servers.find(id) == g_servers.end()) {
			g_servers[id] = newState;
			ServerState& state = g_servers[id];
			if (!createServerStatFile(state)) {
				g_servers.erase(id);
				continue;
			}
		}

		g_servers[id].name = newState.name;
		g_servers[id].map = newState.map;
		g_servers[id].bots = newState.bots;
		g_servers[id].flags = newState.flags;
		g_servers[id].maxPlayers = newState.maxPlayers;
		writeServerStat(g_servers[id], newState.players, false, now);
		g_servers[id].lastResponseTime = now;
	}

	vector<string> delKeys;

	for (auto item : g_servers) {
		if (updatedServers.find(item.first) == updatedServers.end()) {
			ServerState& state = g_servers[item.first];
			uint32_t deadTime = state.secondsSinceLastResponse();					
			string dispName = state.displayName();

			if (deadTime > SERVER_DEAD_SECONDS) {
				printf("Server is dead (%.1f hours): %s\n", deadTime / (60.0f * 60.0f), dispName.c_str());
				delKeys.push_back(item.first);
			}
			else if (!state.unreachable && deadTime > SERVER_UNREACHABLE_TIME) {
				printf("Server unreachable (%.1f minutes): %s\n", deadTime / 60.0f, dispName.c_str());
				writeServerStat(state, 0, true, now);
			}
		}
	}

	for (string key : delKeys) {
		if (archiveStats(key)) {
			g_servers.erase(key);
		}
	}
}

bool validateStatName(string name) {
	vector<string> parts = splitString(name, "_");
	vector<string> ipParts = splitString(parts[0], ".");

	if (parts.size() != 2 || ipParts.size() != 4) {
		return false;
	}

	int port = atoi(parts[1].c_str());
	if (port <= 0 || port > 65535) {
		return false;
	}

	for (int i = 0; i < ipParts.size(); i++) {
		int b = atoi(ipParts[i].c_str());
		if (b < 0 || b > 255) {
			return false;
		}
	}

	return true;
}

bool compareByRank(const ServerState& a, const ServerState& b)
{
	return a.rankSum > b.rankSum;
}

bool loadRankHistory(ServerState& state, uint16_t& lastRank, uint32_t& lastRankWriteTime, uint32_t now) {
	FILE* file = loadRankFile(state);

	if (!file) {
		return false;
	}

	lastRank = 0;
	lastRankWriteTime = 0;

	while (1) {
		uint8_t rankChange;
		if (!fread(&rankChange, sizeof(uint8_t), 1, file)) {
			break;
		}
		uint8_t flags = rankChange & FL_RANK_MASK;
		uint8_t rankDelta = rankChange & ~FL_RANK_MASK;

		if (flags & FL_RANK_RANK16) {
			if (rankDelta) {
				printf("Invalid rank bits in 16bit delta\n");
				fclose(file);
				return false;
			}
			if (!fread(&lastRank, sizeof(uint16_t), 1, file)) {
				printf("Failed to read rank\n");
				fclose(file);
				return false;
			}
		}
		else {
			int8_t rankSigned = rankDelta;
			if (rankDelta & RANK_SIGN_BIT) {
				rankSigned |= FL_RANK_MASK; // sign extension to fit the int8
			}
			int newRank = (int)lastRank + (int)rankSigned;
			if (newRank < 0 || newRank > 65535) {
				printf("Invalid new rank: %d\n", newRank);
				fclose(file);
				return false;
			}
			lastRank = newRank;
		}

		if (flags & FL_RANK_TIME32) {
			if (!fread(&lastRankWriteTime, sizeof(uint32_t), 1, file)) {
				printf("Failed to read rank time\n");
				fclose(file);
				return false;
			}
		}
		else {
			uint16_t delta;
			if (!fread(&delta, sizeof(uint16_t), 1, file)) {
				printf("Failed to read rank time delta\n");
				fclose(file);
				return false;
			}
			lastRankWriteTime += delta;
		}
	}

	fclose(file);

	if (lastRankWriteTime > now) {
		printf("Invalid rank time parsed: %u\n", lastRankWriteTime);
		return false;
	}

	return true;
}

bool writeRankFile(ServerState& state, uint16_t rank, uint32_t now) {
	string fpath = state.getRankHistFilePath();

	FILE* file = NULL;
	uint16_t lastRank = 0;
	uint32_t lastRankWriteTime = 0;

	bool createdNewFile = false;

	if (!fileExists(fpath)) {
		errno = 0;
		file = fopen(fpath.c_str(), "wb");
		if (!file) {
			printf("Failed to create rank file (error %d): %s\n", errno, fpath.c_str());
			return false;
		}
		if (!writeStatHeader(file, rankFileMagicBytes, fpath)) {
			printf("Failed to write rank file header: %s\n", fpath.c_str());
			fclose(file);
			return false;
		}
		createdNewFile = true;
	}
	else {
		if (!loadRankHistory(state, lastRank, lastRankWriteTime, now)) {
			printf("Failed to load rank history: %s\n", fpath.c_str());
			return false;
		}

		file = fopen(fpath.c_str(), "ab");
		if (!file) {
			printf("Failed to append rank file: %s\n", fpath.c_str());
			return false;
		}
	}

	if (rank == lastRank && !createdNewFile) {
		fclose(file);
		return true; // no delta to write
	}

	uint8_t rankByte = 0;
	int rankDelta = (int)rank - (int)lastRank;

	if (rankDelta > 31 || rankDelta < -32 || createdNewFile) {
		rankByte |= FL_RANK_RANK16;
	}
	else {
		rankByte = rankDelta & ~FL_RANK_MASK;
	}

	if (now - lastRankWriteTime > 65535 || createdNewFile) {
		rankByte |= FL_RANK_TIME32;
	}

	if (!fwriteVerbose(&rankByte, sizeof(uint8_t), file, "rank delta")) {
		fclose(file);
		return false;
	}

	if (rankByte & FL_RANK_RANK16) {
		if (!fwriteVerbose(&rank, sizeof(uint16_t), file, "rank bytes")) {
			fclose(file);
			return false;
		}
	}
	if (rankByte & FL_RANK_TIME32) {
		if (!fwriteVerbose(&now, sizeof(uint32_t), file, "rank time")) {
			fclose(file);
			return false;
		}
	}
	else {
		uint16_t delta = now - lastRankWriteTime;
		if (!fwriteVerbose(&delta, sizeof(uint16_t), file, "rank time")) {
			fclose(file);
			return false;
		}
	}

	fclose(file);

	return true;
}

void computeRanks() {
	vector<string> statFiles = getDirFiles(statsPath, "dat", "");

	uint32_t now = getEpochSeconds();

	vector<ServerState> rankedServers;

	for (auto item : g_servers) {
		item.second.rankSum = 0;
	}

	for (string path : statFiles) {
		string fname = path.substr(0, path.find_last_of("."));
		if (!validateStatName(fname)) {
			printf("Invalid stat file name: %s", fname.c_str());
			continue;
		}
		if (g_servers.find(fname) == g_servers.end()) {
			printf("Skip ranking untracked server: %s\n", fname.c_str());
			continue;
		}

		ServerState state = ServerState();
		state.init();
		state.addr = fname;
		state.name = g_servers[state.addr].name;

		if (!loadServerHistory(state, now, false)) {
			g_servers.erase(fname);
			printf("File corruption: %s\n", fname.c_str());
			continue;
		}

		g_servers[state.addr].rankSum = state.rankSum;

		rankedServers.push_back(state);
	}

	std::sort(rankedServers.begin(), rankedServers.end(), compareByRank);

	printf("Server ranks:\n");
	for (int i = 0; i < rankedServers.size(); i++) {
		ServerState& serv = rankedServers[i];

		// zero means no players ever joined during the ranking period
		uint16_t writeRank = serv.rankSum ? i+1 : 0;
		writeRankFile(serv, writeRank, now);

		if (i < 10) {
			string dispName = serv.displayName();
			float avg = serv.rankSum / (float)TOTAL_RANK_DATA_POINTS;
			printf("%2d) %.2f = %s\n", i+1, avg, dispName.c_str());
		}
	}
}

void cleanupServerListJson(Document& doc, Value& serverList) {
	int numServers = serverList.GetArray().Size();

	for (int i = 0; i < numServers; i++) {
		Value& server = serverList[i];

		uint32_t flags = 0;
		if (server["dedicated"].GetBool()) {
			flags |= FL_SERVER_DEDICATED;
		}
		if (server["secure"].GetBool()) {
			flags |= FL_SERVER_SECURE;
		}
		if (strcmp(server["os"].GetString(), "l") == 0) {
			flags |= FL_SERVER_LINUX;
		}
		server.AddMember("flags", flags, doc.GetAllocator());

		server.RemoveMember("appid");
		server.RemoveMember("steamid");
		server.RemoveMember("gamedir");
		server.RemoveMember("gameport");
		server.RemoveMember("version");
		server.RemoveMember("product");
		server.RemoveMember("region");
		server.RemoveMember("dedicated");
		server.RemoveMember("secure");
		server.RemoveMember("os");
	}
}

void saveServerInfos() {
	Document infoDoc;
	infoDoc.SetObject();

	auto& allocator = infoDoc.GetAllocator();
	Value serverArray(kArrayType);

	Value serversObj;
	serversObj.SetObject();

	for (auto item : g_servers) {
		ServerState& server = item.second;
		Value obj;
		Value name(server.name.c_str(), allocator);
		Value addr(item.first.c_str(), allocator);
		Value map(server.map.c_str(), allocator);

		obj.SetObject();
		obj.AddMember("name", name, allocator);
		obj.AddMember("flags", server.flags, allocator);
		obj.AddMember("time", server.lastResponseTime, allocator);
		obj.AddMember("max_players", server.maxPlayers, allocator);
		obj.AddMember("players", server.players, allocator);
		obj.AddMember("bots", server.bots, allocator);
		obj.AddMember("map", map, allocator);		
		obj.AddMember("rank", server.rankSum, allocator);		

		serversObj.AddMember(addr, obj, allocator);
	}

	infoDoc.AddMember("updateFreq", STAT_WRITE_FREQ, allocator);
	infoDoc.AddMember("deadTime", SERVER_DEAD_SECONDS, allocator);
	infoDoc.AddMember("unreachableTime", SERVER_UNREACHABLE_TIME, allocator);
	infoDoc.AddMember("rankFreq", RANK_FREQ, allocator);
	infoDoc.AddMember("rankStatMaxAge", RANK_STAT_MAX_AGE, allocator);
	infoDoc.AddMember("rankStatInterval", RANK_STAT_INTERVAL, allocator);
	infoDoc.AddMember("lastRankTime", g_lastRankTime, allocator);
	infoDoc.AddMember("lastUpdateTime", g_lastUpdateTime, allocator);
	infoDoc.AddMember("servers", serversObj, allocator);

	writeJson(serverInfoPath + ".temp", infoDoc);
	remove(serverInfoPath.c_str());
	rename((serverInfoPath + ".temp").c_str(), serverInfoPath.c_str());
}

bool loadServerInfos() {
	vector<string> statFiles = getDirFiles(statsPath, "dat", "");

	Document serverDoc;
	Value& serverInfo = serverDoc;
	if (!loadJson(serverInfoPath, serverDoc) || !serverInfo.IsObject() || !serverInfo.HasMember("servers")) {
		printf("Failed to load: %s\n", serverInfoPath.c_str());
	}
	else {
		serverInfo = serverInfo["servers"];
	}	

	for (string path : statFiles) {
		string fname = path.substr(0, path.find_last_of("."));
		if (!validateStatName(fname)) {
			printf("Invalid stat file name: %s", fname.c_str());
			return false;
		}

		g_servers[fname] = ServerState();
		ServerState& state = g_servers[fname];
		state.init();
		state.addr = fname;

		if (serverInfo.IsObject() && serverInfo.HasMember(fname.c_str())) {
			Value& info = serverInfo[fname.c_str()];
			parseProgramServerJson(info, state);
			state.addr = fname;
		}
		else {
			printf("Missing info for server: %s\n", fname.c_str());
			if (!archiveStats(fname)) {
				return false;
			}
			g_servers.erase(fname);
			continue;
		}

		if (!loadServerHistory(state, getEpochSeconds(), true)) {
			g_servers.erase(fname);
			printf("File corruption: %s\n", fname.c_str());
			return false;
		}
	}

	return true;
}

int main(int argc, char** argv) {
	if (!dirExists(dataPath) && !createDir(dataPath)) {
		printf("Failed to create folder: %s\n", dataPath.c_str());
		return 0;
	}
	if (!dirExists(statsPath) && !createDir(statsPath)) {
		printf("Failed to create folder: %s\n", statsPath.c_str());
		return 0;
	}
	if (!dirExists(archivePath) && !createDir(archivePath)) {
		printf("Failed to create folder: %s\n", archivePath.c_str());
		return 0;
	}
	if (!dirExists(liveDataPath) && !createDir(liveDataPath)) {
		printf("Failed to create folder: %s\n", liveDataPath.c_str());
		return 0;
	}
	if (!dirExists(avgDataPath) && !createDir(avgDataPath)) {
		printf("Failed to create folder: %s\n", avgDataPath.c_str());
		return 0;
	}
	if (!dirExists(rankHistoryPath) && !createDir(rankHistoryPath)) {
		printf("Failed to create folder: %s\n", rankHistoryPath.c_str());
		return 0;
	}
	if (!dirExists(archiveRankPath) && !createDir(archiveRankPath)) {
		printf("Failed to create folder: %s\n", archiveRankPath.c_str());
		return 0;
	}
	
	loadApiKey();

	if (!loadApiKey() || !loadServerInfos()) {
		return 0;
	}
	
	uint32_t lastStatTime = 0;
	uint64_t writeCount = 1;
	uint64_t startTime = (uint64_t)getEpochSeconds() * 1000ULL;
	uint64_t nextWriteTime = startTime + ((uint64_t)(STAT_WRITE_FREQ * 1000) * writeCount);
	
	uint64_t updateStartTime = getEpochMillis();

	printf("Startup finished. Begin scanning\n\n");
	
	while (1) {
		uint64_t fetchStartTime = getEpochMillis();
		Document json;
		Value& serverList = json;
		while (!getServerListJson(serverList, json)) {
			this_thread::sleep_for(seconds(10));
		}	

		cleanupServerListJson(json, serverList);

		uint64_t now = getEpochMillis();

		printf("Server list fetched in %.1fs. Total update time: %.2fs\n\n", 
			(now - fetchStartTime) / 1000.0f, (now - updateStartTime) / 1000.0f);
		
		now = getEpochMillis();
		uint32_t waitTime = nextWriteTime - now;

		if (nextWriteTime > now) {
			printf("Next update in %.1fs\n", waitTime / 1000.0f);
			this_thread::sleep_for(milliseconds(waitTime));
		}

		updateStartTime = getEpochMillis();
		g_lastUpdateTime = getEpochSeconds();
		updateStats(json, serverList, g_lastUpdateTime);

		uint32_t nowSecs = getEpochSeconds();
		if (nowSecs - g_lastRankTime > RANK_FREQ) {
			g_lastRankTime = nowSecs;
			uint64_t start = getEpochMillis();
			g_writeStats.bytesRead = 0;
			computeRanks();
			printf("Updated ranks in %.2fs, %.1f MB read\n", (getEpochMillis() - start) / 1000.0f, g_writeStats.bytesRead / (1024.0f*1024.0f));
		}

		printf("Updated %d/%d servers, wrote %d bytes\n", g_writeStats.serversUpdated, (int)g_servers.size(), g_writeStats.bytesWritten);

		saveServerInfos();

		do {
			writeCount++;
			nextWriteTime = startTime + ((uint64_t)(STAT_WRITE_FREQ*1000) * writeCount);
		} while (nextWriteTime < now);
	}
	

	return 0;
}
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
string statsPath = "data/stats/";
string archivePath = "data/stats/archive/";
string rankPath = "data/rankings.txt"; // ordered list of server IDs by rank
string steamJsonPath = "data/servers.json"; // ordered list of server IDs by rank

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

// bottom 6 bits are current player count

#pragma pack(pop)

struct ServerState {
	string addr; // port separator converted to filename safe character
	string name;
	uint8_t players;
	bool unreachable;

	uint32_t lastWriteTime; // last time a player count stat was written (epoch seconds)
	uint32_t lastResponseTime; // last time data was received for this server
	uint32_t rankSum; // sum of player counts over rankDataPoints data points
	
	string getStatFilePath();
	string getStatArchiveFilePath();
	uint32_t secondsSinceLastResponse();
	string displayName();
	void init();
}; 

void ServerState::init() {
	addr = "";
	name = "";
	players = 0;
	unreachable = false;
	lastWriteTime = 0;
	lastResponseTime = 0;
}

struct WriteStats {
	int bytesWritten = 0;
	int serversUpdated = 0;
	int bytesRead = 0;
};

WriteStats g_writeStats;
map<string, ServerState> g_servers;

bool writeServerStat(ServerState& state, int newPlayerCount, bool unreachable, uint32_t now);
bool createServerStatFile(ServerState& newState);
bool loadServerHistory(ServerState& state, uint32_t now, bool programRestarted);
void updateStats(Value& serverList);
bool validateStatName(string name);
bool parseServerJson(Value& json, ServerState& state);

string ServerState::getStatFilePath() {
	return statsPath + addr + ".dat";
}

string ServerState::getStatArchiveFilePath() {
	return archivePath + addr + ".dat";
}

uint32_t ServerState::secondsSinceLastResponse() {
	return getEpochSeconds() - lastResponseTime;
}

string ServerState::displayName() {
	return "[" + addr + "] " + name;
}

void loadApiKey() {
	int length;
	char* buffer = loadFile("api_key.txt", length);
	if (!buffer) {
		printf("Bad api key file\n");
		return;
	}
	apikey = buffer;
	delete[] buffer;
}

string stringifyJson(Value& v) {
	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);
	v.Accept(writer);
	return buffer.GetString();
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

bool parseServerJson(Value& json, ServerState& state) {
	if (!(json.HasMember("name") && json.HasMember("map") && json.HasMember("players")
			&& json.HasMember("max_players") && json.HasMember("addr"))) {
		printf("Server json missing values\n");
		return false;
	}

	state.init();
	state.players = json["players"].GetInt();
	state.addr = replaceString(json["addr"].GetString(), ":", "_");
	state.name = json["name"].GetString();

	return true;
}

// false indicates a problem with the file
bool loadServerHistory(ServerState& state, uint32_t now, bool programRestarted) {
	string fpath = state.getStatFilePath();
	string archivePath = state.getStatArchiveFilePath();

	FILE* file = NULL;

	if (fileExists(fpath)) {
		errno = 0;
		file = fopen(fpath.c_str(), "rb");
		if (!file) {
			printf("Failed to open stat file (%d): %s\n", errno, fpath.c_str());
			return false;
		}
	}
	else if (fileExists(archivePath)) {
		printf("Unarchive revived server: %s\n", state.addr.c_str());
		errno = 0;
		if (rename(archivePath.c_str(), fpath.c_str()) == -1) {
			printf("Unarchive failed. Rename error %d: %s", errno, archivePath.c_str());
			return false;
		}

		errno = 0;
		file = fopen(fpath.c_str(), "rb");
		if (!file) {
			printf("Failed to open stat file (%d): %s\n", errno, archivePath.c_str());
			return false;
		}
	}
	else {
		return false;
	}
	

	StatFileHeader header;

	if (!fread(&header, sizeof(StatFileHeader), 1, file)) {
		printf("Failed to read stat file: %s\n", fpath.c_str());
		fclose(file);
		file = NULL;
		return false;
	}

	if (header.version != STAT_FILE_VERSION) {
		printf("Bad version %d in stat file: %s\n", header.version, fpath.c_str());
		fclose(file);
		file = NULL;
		return false;
	}

	if (strncmp(header.magic, "SVTK", 4)) {
		string magic = string(header.magic, 4);
		printf("Bad magic bytes '%s' in stat file: %s\n", magic.c_str(), fpath.c_str());
		fclose(file);
		file = NULL;
		return false;
	}

	string dispName = state.displayName();
	//printf("History for %s\n", dispName.c_str());

	uint32_t rankStartTime = now - RANK_STAT_MAX_AGE;
	uint32_t nextRankTime = rankStartTime;
	state.rankSum = 0;
	uint32_t rankDataPoints = 0;

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

	state.lastResponseTime = state.lastWriteTime;

	// write unreachable stat at the same time as the last stat,
	// which will be when the program was stopped

	uint32_t deadTime = state.secondsSinceLastResponse();
	if (programRestarted && !state.unreachable && deadTime > SERVER_UNREACHABLE_TIME) {
		writeServerStat(state, 0, true, state.lastWriteTime);
		state.players = 0;
		g_writeStats.serversUpdated -= 1;
		printf("append unreachable stat: %s\n", dispName.c_str());
	}
	else {
		//printf("Loaded server state: %s\n", dispName.c_str());
	}

	return true;
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

	StatFileHeader header;
	header.version = STAT_FILE_VERSION;
	memcpy(header.magic, "SVTK", 4);

	if (!fwrite(&header, sizeof(StatFileHeader), 1, file)) {
		printf("Failed to write to stat file: %s\n", fpath.c_str());
		return false;
	}

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

	g_writeStats.serversUpdated++;
	return true;
}

void updateStats(Value& serverList) {
	int numServers = serverList.GetArray().Size();

	set<string> updatedServers;
	int totalWrites = 0;

	g_writeStats.bytesWritten = 0;
	g_writeStats.serversUpdated = 0;

	uint32_t now = getEpochSeconds();

	for (int i = 0; i < numServers; i++) {
		ServerState newState;
		if (!parseServerJson(serverList[i], newState)) {
			continue;
		}

		string id = newState.addr;
		updatedServers.insert(newState.addr);

		if (g_servers.find(id) == g_servers.end()) {
			g_servers[id] = ServerState();
			ServerState& state = g_servers[id];
			state.init();
			state.addr = newState.addr;
			state.name = newState.name;
			if (!createServerStatFile(state)) {
				g_servers.erase(id);
				continue;
			}
		}

		g_servers[id].name = newState.name;
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
		ServerState& state = g_servers[key];
		string srcPath = state.getStatFilePath();
		string dstPath = state.getStatArchiveFilePath();
		
		if (fileExists(dstPath)) {
			printf("Archive failed. Destination exists: %s\n", dstPath.c_str());
		}
		else if (!fileExists(srcPath)) {
			printf("Archive failed. Source file missing: %s\n", srcPath.c_str());
		}
		else {
			errno = 0;
			if (rename(srcPath.c_str(), dstPath.c_str()) == -1) {
				printf("Archive failed. Rename error %d: %s\n", errno, srcPath.c_str());
			}
			else {
				g_servers.erase(key);
			}
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

void computeRanks() {
	vector<string> statFiles = getDirFiles(statsPath, "dat", "");

	uint32_t now = getEpochSeconds();

	vector<ServerState> rankedServers;

	for (string path : statFiles) {
		string fname = path.substr(0, path.find_last_of("."));
		if (!validateStatName(fname)) {
			printf("Invalid stat file name: %s", fname.c_str());
			continue;
		}

		ServerState state = ServerState();
		state.init();
		state.addr = fname;
		if (g_servers.find(state.addr) != g_servers.end()) {
			state.name = g_servers[state.addr].name;
		}
		if (!loadServerHistory(state, now, false)) {
			g_servers.erase(fname);
			printf("File corruption: %s", fname.c_str());
			continue;
		}
		if (state.rankSum > 0)
			rankedServers.push_back(state);
	}

	FILE* file = fopen(rankPath.c_str(), "w");
	if (!file) {
		printf("Failed to open rank file (%d): %s\n", errno, rankPath.c_str());
		return;
	}

	std::sort(rankedServers.begin(), rankedServers.end(), compareByRank);

	printf("Server ranks:\n");

	string rankInfo = "rank_age=" + to_string(RANK_STAT_MAX_AGE) + "\nrank_freq=" + to_string(RANK_STAT_INTERVAL) + "\n";
	fwrite(rankInfo.c_str(), rankInfo.size(), 1, file);

	for (int i = 0; i < rankedServers.size(); i++) {
		ServerState& serv = rankedServers[i];
		if (i < 10) {
			string dispName = serv.displayName();
			float avg = serv.rankSum / (float)TOTAL_RANK_DATA_POINTS;
			printf("%2d) %.2f = %s\n", i+1, avg, dispName.c_str());
		}
		string line = to_string(serv.rankSum) + "=" + serv.addr + "=" + serv.name + "\n";
		fwrite(line.c_str(), line.size(), 1, file);
	}

	fclose(file);
}

int main(int argc, char** argv) {
	if (!dirExists(dataPath) && !createDir(dataPath)) {
		printf("Failed to create folder: %s\n", statsPath.c_str());
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
	loadApiKey();

	vector<string> statFiles = getDirFiles(statsPath, "dat", "");

	for (string path : statFiles) {
		string fname = path.substr(0, path.find_last_of("."));
		if (!validateStatName(fname)) {
			printf("Invalid stat file name: %s", fname.c_str());
			return 0;
		}

		g_servers[fname] = ServerState();
		ServerState& state = g_servers[fname];
		state.init();
		state.addr = fname;
		if (!loadServerHistory(state, getEpochSeconds(), true)) {
			g_servers.erase(fname);
			printf("File corruption: %s", fname.c_str());
			return 0;
		}
	}
	
	uint32_t lastStatTime = 0;
	uint64_t writeCount = 1;
	uint64_t startTime = (uint64_t)getEpochSeconds() * 1000ULL;
	uint64_t nextWriteTime = startTime + ((uint64_t)(STAT_WRITE_FREQ * 1000) * writeCount);
	uint32_t lastRankTime = 0;
	uint64_t updateStartTime = getEpochMillis();

	printf("Startup finished. Begin scanning\n\n");
	
	while (1) {
		uint64_t fetchStartTime = getEpochMillis();
		Document json;
		Value& serverList = json;
		while (!getServerListJson(serverList, json)) {
			this_thread::sleep_for(seconds(10));
		}

		FILE* jsonFile = fopen(steamJsonPath.c_str(), "wb");
		if (jsonFile) {
			string jsonString = stringifyJson(serverList);
			if (!fwrite(jsonString.c_str(), jsonString.size(), 1, jsonFile)) {
				printf("Failed to write json file\n");
			}
			fclose(jsonFile);
		}
		else {
			printf("Failed to open json file: %s\n", steamJsonPath.c_str());
		}
		

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
		updateStats(serverList);

		uint32_t nowSecs = getEpochSeconds();
		if (nowSecs - lastRankTime > RANK_FREQ) {
			lastRankTime = nowSecs;
			uint64_t start = getEpochMillis();
			g_writeStats.bytesRead = 0;
			computeRanks();
			printf("Computed ranks in %.2fs, %.1f MB read\n", (getEpochMillis() - start) / 1000.0f, g_writeStats.bytesRead / (1024.0f*1024.0f));
		}

		printf("Updated %d/%d servers, wrote %d bytes\n", g_writeStats.serversUpdated, (int)g_servers.size(), g_writeStats.bytesWritten);

		do {
			writeCount++;
			nextWriteTime = startTime + ((uint64_t)(STAT_WRITE_FREQ*1000) * writeCount);
		} while (nextWriteTime < now);
	}
	

	return 0;
}
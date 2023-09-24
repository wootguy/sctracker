#include "util.h"
#include <iostream>
#include <map>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

using namespace std;
using namespace rapidjson;

string server = "https://api.steampowered.com/";
string api = "IGameServersService/GetServerList/v1";
string key = "";
string appid = "225840"; // Sven Co-op
string filter = "\\appid\\" + appid + "\\dedicated\\1";
string requestUrl = server + api + "?key=" + key + "&filter=" + filter + "&limit=20000";
string statsPath = "stats";

#define STAT_FILE_VERSION 1

#pragma pack(push, 1)
struct StatFileHeader {
	uint32_t version;
	char magic[4]; // "SVTK"
};

#define FL_PCNT_TIME16 64		// time delta is 16 bits and relative to the last stat
#define FL_PCNT_TIME32 128		// time is a 32 bit absoulte value
#define PCNT_UNREACHABLE (FL_PCNT_TIME16|FL_PCNT_TIME32) // if both are set, the server is unreachable
// if neither are set, time delta is 8-bits and relative to the last stat

// bottom 6 bits are current player count

#pragma pack(pop)

struct ServerState {
	uint8_t ip[4];
	uint16_t port;
	char name[64];
	char map[64];
	uint8_t players;
	uint8_t maxPlayers;
	bool unreachable;

	uint32_t lastCountStat; // last time a player count stat was written (epoch seconds)

	string getAddrString();
	string getIdString();
};

string ServerState::getAddrString() {
	return to_string(ip[0]) + "." + to_string(ip[1]) + "." 
		+ to_string(ip[2]) + "." + to_string(ip[3]) + ":" + to_string(port);
}

// string used as an ID for g_servers and file names
string ServerState::getIdString() {
	return to_string(ip[0]) + "." + to_string(ip[1]) + "."
		+ to_string(ip[2]) + "." + to_string(ip[3]) + "_" + to_string(port);
}

string getStatFolderPath(string id) {
	return statsPath + "/" + id;
}

string getStatFilePath(string id) {
	return statsPath + "/" + id + ".dat";
}

map<string, ServerState> g_servers;

string stringifyJson(Value& v) {
	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);
	v.Accept(writer);
	return buffer.GetString();
}

bool getServerListJson(Value& serverList) {
	string response_string;
	int resp_code = webRequest(requestUrl, response_string);

	if (resp_code != 200) {
		printf("Failed to fetch server list (HTTP response code %d)", resp_code);
		return false;
	}

	static Document json;

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

	memset(&state, 0, sizeof(ServerState));

	strncpy(state.name, json["name"].GetString(), 64);
	strncpy(state.map, json["map"].GetString(), 64);
	state.name[63] = 0;
	state.map[63] = 0;

	state.players = json["players"].GetInt();
	state.maxPlayers = json["max_players"].GetInt();

	string addr = json["addr"].GetString();

	int ipIdx = addr.find(":");
	if (ipIdx == -1) {
		printf("Server json missing port in addr: '%s'\n", addr.c_str());
		return false;
	}
	string ip = addr.substr(0, ipIdx);
	int iport = atoi(addr.substr(ipIdx+1).c_str());

	vector<string> ipbytes = splitString(ip, ".");
	if (ipbytes.size() != 4) {
		printf("Server json has invalid ip parts: '%s'\n", addr.c_str());
		return false;
	}

	for (int i = 0; i < 4; i++) {
		int b = atoi(ipbytes[i].c_str());
		if (b > 255 || b < 0) {
			printf("Server json has invalid ip bytes: '%s'\n", addr.c_str());
			return false;
		}
		state.ip[i] = b;
	}

	if (iport > 65535 || iport <= 0) {
		printf("Server json has invalid port: '%s'\n", addr.c_str());
		return false;
	}
	state.port = iport;

	return true;
}

// false indicates a problem with the file
bool loadServerHistory(string id, ServerState& state) {
	string fpath = getStatFilePath(id);

	if (!fileExists(fpath)) {
		return true;
	}
	
	FILE* file = fopen(fpath.c_str(), "rb");
	if (!file) {
		printf("Failed to open stat file: %s\n", fpath.c_str());
		return false;
	}

	StatFileHeader header;

	if (!fread(&header, sizeof(StatFileHeader), 1, file)) {
		printf("Failed to read stat file: %s\n", fpath.c_str());
		fclose(file);
		return false;
	}

	if (header.version != STAT_FILE_VERSION) {
		printf("Bad version %d in stat file: %s\n", header.version, fpath.c_str());
		fclose(file);
		return false;
	}

	if (strcmp(header.magic, "SVTK")) {
		string magic = string(header.magic, 4);
		printf("Bad magic bytes '%s' in stat file: %s\n", magic.c_str(), fpath.c_str());
		fclose(file);
		return false;
	}

	memset(&state, 0, sizeof(ServerState));
}

void writeServerStat(ServerState& currentState, ServerState* oldState) {

}

void convertStats() {
	int length;
	char* buffer = loadFile("player_counts.json", length);
	if (!buffer) {
		printf("Bad convert file\n");
		return;
	}

	Document json;
	json.Parse(buffer);

	Value& utc_data = json["utc"];
	Value& count_data = json["count"];
	int numElements = utc_data.GetArray().Size();

	printf("Loaded %d elements\n", numElements);


	FILE* file = fopen("player_counts.dat", "wb");
	if (!file) {
		printf("Can't open output file\n");
		return;
	}

	StatFileHeader header;
	memcpy(header.magic, "SVTK", 4);
	header.version = STAT_FILE_VERSION;

	fwrite(&header, sizeof(StatFileHeader), 1, file);

	uint32_t lastUtcTime = 0;
	uint32_t larges = 0;

	for (int i = 0; i < numElements; i++) {
		uint8_t count = count_data[i].GetInt();
		uint32_t utc = utc_data[i].GetUint();

		if (utc - lastUtcTime < 50)
			continue;

		if (utc - lastUtcTime > 65535) {
			count |= 128;
			larges++;
			fwrite(&utc, sizeof(uint32_t), 1, file);
		}
		else if (utc - lastUtcTime > 255) {
			count |= 64;
			fwrite(&utc, sizeof(uint16_t), 1, file);
		}
		else {
			fwrite(&utc, sizeof(uint8_t), 1, file);
			
		}

		fwrite(&count, sizeof(uint8_t), 1, file);

		lastUtcTime = utc;
	}
	printf("larges deltas: %u", larges);

	fclose(file);
}

void updateStats(Value& serverList) {
	int numServers = serverList.GetArray().Size();

	for (int i = 0; i < numServers; i++) {
		ServerState newState;
		if (!parseServerJson(serverList[i], newState)) {
			continue;
		}
		string id = newState.getIdString();

		if (g_servers.find(id) == g_servers.end()) {
			ServerState oldState;
			loadServerHistory(id, oldState);
			writeServerStat(newState, NULL);
		}
		else {
			writeServerStat(newState, &g_servers[id]);
		}

		printf("[%2d/%-2d] %s (%s)\n", newState.players, newState.maxPlayers, newState.name, newState.getAddrString().c_str());
		//cout << stringifyJson(s) << endl;
	}

	std::cout << "Total servers: " << numServers << std::endl;
}

int main(int argc, char** argv) {
	//convertStats();
	//if (1)return 0;
	
	Value& serverList = Value();
	
	if (getServerListJson(serverList)) {
		updateStats(serverList);
	}

	return 0;
}
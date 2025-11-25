#include <string>
#include <vector>
#include <unordered_map>

struct Player {
	std::string name;
	int score;
	float duration;
};

struct ServerState {
	std::string addr; // port separator converted to filename safe character
	std::string name;
	std::string map;
	uint8_t players;
	uint8_t maxPlayers;
	uint8_t bots;
	bool unreachable;
	uint8_t flags;

	uint32_t lastWriteTime; // last time a player count stat was written (epoch seconds)
	uint32_t lastResponseTime; // last time data was received for this server
	uint32_t rankSum; // sum of player counts over rankDataPoints data points
	int lastRank; // last rank written to file

	std::vector<Player> a2s_players; // player info from A2S
	bool a2s_success; // true if A2S queries succeeded

	std::string getStatFilePath();
	std::string getStatArchiveFilePath();
	std::string getLiveStatFilePath();
	std::string getLiveAvgStatFilePath();
	std::string getRankHistFilePath();
	std::string getRankArchiveFilePath();
	uint32_t secondsSinceLastResponse();
	std::string displayName();
	void init();
};

extern std::unordered_map<std::string, ServerState> g_servers;
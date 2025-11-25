#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <thread>
#include <chrono>
#include "main.h"
#include "util.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

using namespace std::chrono;

#define MAX_REQ_ATTEMPTS 3 // give up A2S query after this many attempts
#define REQ_TIMEOUT 1000 // milliseconds to wait between A2S query attempts

enum QUERY_JOB_STATE {
    QJ_NOT_STARTED, // no packets have been sent yet
    QJ_WAIT_CHALLENGE, // challenge request sent. Now waiting for a response.
    QJ_GOT_CHALLENGE, // challenge response received.
    QJ_WAIT_PLAYERS, // challenge received. Now waiting for player list.
    QJ_DONE, // job finished or failed.
};

struct QueryJob {
    sockaddr_in addr;
    int32_t challenge = 0;
    int state = 0;
    uint64_t lastReq = 0; // time a request was last sent
    int reqAttempts; // how many times a request was attempted
    std::vector<Player> players;
    bool success = false;
};

int g_a2s_socket;

void sendPacket(const sockaddr_in& addr, const std::vector<uint8_t>& packet) {
    sendto(g_a2s_socket, (const char*)packet.data(), packet.size(), 0,
        (const sockaddr*)&addr, sizeof(addr));
}

std::vector<Player> parsePlayers(std::vector<uint8_t>& data) {
    size_t i = 5;
    int numPlayers = data[i++];
    std::vector<Player> players;

    for (int n = 0; n < numPlayers; n++) {
        uint8_t index = data[i++];

        // read name (null-terminated)
        size_t start = i;
        while (i < data.size() && data[i] != 0)
            i++;
        std::string name((char*)&data[start], i - start);
        i++; // skip null

        int score = *(int*)&data[i];
        i += 4;

        float duration = *(float*)&data[i];
        i += 4;

        players.push_back({ name, score, duration });
    }
    return players;
} 

uint64_t netaddr_to_uint64(sockaddr_in& addr) {
    return (uint64_t)addr.sin_addr.s_addr + ((uint64_t)addr.sin_port << 32);
}

sockaddr_in uint64_to_netaddr(uint64_t addrint) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = (addrint >> 32) & 0xffff;
    addr.sin_addr.s_addr = addrint & 0xffffffff;
    return addr;
}

uint64_t ipstring_to_uint64(const std::string& ipstring) {
    int sep = ipstring.find("_");
    if (sep == -1) {
        return 0;
    }

    std::string ip = ipstring.substr(0, sep);
    std::string port = ipstring.substr(sep + 1);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(port.c_str()));
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    return netaddr_to_uint64(addr);
}

std::string netaddr_to_ipstring(const sockaddr_in& addr)
{
    char ipstr[128] = { 0 };

#ifdef _WIN32
    inet_ntop(AF_INET, (void*)&addr.sin_addr, ipstr, sizeof(ipstr));
#else
    inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof(ipstr));
#endif

    return std::string(ipstr) + "_" + std::to_string(ntohs(addr.sin_port));
}

bool a2s_init() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    g_a2s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_a2s_socket < 0) {
        printf("Failed to create A2S socket\n");
        return false;
    }

#ifdef _WIN32
    u_long mode = 1; // 1 = non-blocking, 0 = blocking
    if (ioctlsocket(g_a2s_socket, FIONBIO, &mode) != 0) {
        printf("ioctlsocket failed: %d\n", WSAGetLastError());
    }
#else
    int flags = fcntl(g_a2s_socket, F_GETFL, 0);
    if (flags == -1) {
        printf("fcntl F_GETFL");
    }

    if (fcntl(g_a2s_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        printf("fcntl F_SETFL");
    }
#endif

    return true;
}

void a2s_cleanup() {
#ifdef _WIN32
    closesocket(g_a2s_socket);
    WSACleanup();
#else
    close(g_a2s_socket);
#endif
}

void a2s_query_all() {
    uint64_t a2sStartTime = getEpochMillis();

    std::unordered_map<uint64_t, QueryJob> jobs;

    for (auto& item : g_servers) {
        if (item.second.unreachable) {
            item.second.a2s_success = false;
            item.second.a2s_players.clear();
            continue;
        }

        uint64_t ip = ipstring_to_uint64(item.first);

        QueryJob job = QueryJob();
        job.addr = uint64_to_netaddr(ip);
        jobs[ip] = job;
    }

    printf("A2S querying %d servers... ", (int)jobs.size());

    std::vector<uint8_t> get_challenge_packet = { 0xFF,0xFF,0xFF,0xFF,0x55,0xFF,0xFF,0xFF,0xFF };
    std::vector<uint8_t> get_players_packet = { 0xFF,0xFF,0xFF,0xFF, 0x55 };

    while (1) {
        int runningJobs = 0;
        int challengeReqs = 0;
        int playerReqs = 0;
        int challengeResp = 0;
        int playerResp = 0;

        uint64_t now = getEpochMillis();

        int sentPackets = 0;

        // send queries
        for (auto& item : jobs) {
            QueryJob& job = item.second;

            if (job.state != QJ_DONE)
                runningJobs++;

            bool sentPacket = false;

            switch (job.state) {
            case QJ_NOT_STARTED:
                //printf("Get challenge: %s\n", netaddr_to_ipstring(job.addr).c_str());
                sendPacket(job.addr, get_challenge_packet);
                job.state = QJ_WAIT_CHALLENGE;
                job.lastReq = now;
                challengeReqs++;
                sentPackets++;
                break;
            case QJ_GOT_CHALLENGE: {
                std::vector<uint8_t> p = get_players_packet;
                p.insert(p.end(), (uint8_t*)&job.challenge, (uint8_t*)&job.challenge + 4);
                sendPacket(job.addr, p);
                //printf("Get players: %s\n", netaddr_to_ipstring(job.addr).c_str());
                job.state = QJ_WAIT_PLAYERS;
                job.lastReq = now;
                playerReqs++;
                sentPackets++;
                break;
            }
            case QJ_WAIT_CHALLENGE:
                if (now - job.lastReq > REQ_TIMEOUT) {
                    job.state = QJ_NOT_STARTED; // retry the request
                    job.reqAttempts++;

                    if (job.reqAttempts >= MAX_REQ_ATTEMPTS) {
                        job.state = QJ_DONE;
                        //printf("A2S_PLAYER failed after %d attempts: %s\n", MAX_REQ_ATTEMPTS, netaddr_to_ipstring(job.addr).c_str());
                    }
                }
                break;
            case QJ_WAIT_PLAYERS:
                if (now - job.lastReq > REQ_TIMEOUT) {
                    job.state = QJ_GOT_CHALLENGE; // retry the request
                    job.reqAttempts++;

                    if (job.reqAttempts >= MAX_REQ_ATTEMPTS) {
                        job.state = QJ_DONE;
                        //printf("A2S_PLAYER failed after %d attempts: %s\n", MAX_REQ_ATTEMPTS, netaddr_to_ipstring(job.addr).c_str());
                    }
                }
                break;
            case QJ_DONE:
                break;
            default:
                printf("Invalid job state %d\n", job.state);
                break;
            }

            if (sentPackets >= 100)
               break; // don't send too many at once
        }

        // receive responses
        while (1) {
            static uint8_t buf[4096];
            socklen_t len = sizeof(sockaddr_in);
            sockaddr_in from;

            int ret = recvfrom(g_a2s_socket, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &len);
            if (ret <= 0)
                break; // no more queued packets

            uint64_t ipint = netaddr_to_uint64(from);
            auto item = jobs.find(ipint);

            if (item == jobs.end()) {
                //printf("Ignored %d byte packet from unknown ip: %s\n", ret, netaddr_to_ipstring(from).c_str());
                continue;
            }

            QueryJob& job = item->second;

            std::vector<uint8_t> data(buf, buf + ret);

            switch (job.state) {
            case QJ_WAIT_CHALLENGE: {
                if (data.size() < 9 || data[4] != 0x41) {
                    if (data.size() > 4 && data[4] == 0x44) {
                        // some servers return the player list without a challenge
                        job.players = parsePlayers(data);
                        job.state = QJ_DONE;
                        job.success = true;
                        //printf("Recv %d players from %s\n", (int)job.players.size(), netaddr_to_ipstring(from).c_str());
                        playerResp++;
                        break;
                    }

                    //printf("unexpected challenge response from %s\n", netaddr_to_ipstring(from).c_str());
                    job.state = QJ_NOT_STARTED;
                    job.reqAttempts++;

                    if (job.reqAttempts >= MAX_REQ_ATTEMPTS) {
                        job.state = QJ_DONE;
                        //printf("A2S_PLAYER failed after %d attempts: %s\n", MAX_REQ_ATTEMPTS, netaddr_to_ipstring(job.addr).c_str());
                    }
                    break;
                }

                job.challenge = *(int*)&data[5];
                job.state = QJ_GOT_CHALLENGE;
                job.reqAttempts = 0;
                //printf("Recv challenge %X from %s\n", job.challenge, netaddr_to_ipstring(from).c_str());
                challengeResp++;
                break;
            }
            case QJ_WAIT_PLAYERS:
                if (data.size() < 6 || data[4] != 0x44) {
                    //printf("unexpected players response from %s\n", netaddr_to_ipstring(from).c_str());
                    job.state = QJ_GOT_CHALLENGE;
                    job.reqAttempts++;

                    if (job.reqAttempts >= MAX_REQ_ATTEMPTS) {
                        job.state = QJ_DONE;
                        //printf("A2S_PLAYER failed after %d attempts: %s\n", MAX_REQ_ATTEMPTS, netaddr_to_ipstring(job.addr).c_str());
                    }
                    break;
                }

                job.players = parsePlayers(data);
                job.state = QJ_DONE;
                job.success = true;
                //printf("Recv %d players from %s\n", (int)job.players.size(), netaddr_to_ipstring(from).c_str());
                playerResp++;
                break;
            case QJ_NOT_STARTED:
            case QJ_GOT_CHALLENGE:
            case QJ_DONE:
                //printf("Received packet while in state %d: %s\n", job.state, netaddr_to_ipstring(from).c_str());
                break;
            default:
                printf("Invalid job state %d\n", job.state);
                break;
            }
        }

        //printf("SENT: %d chg, %d plr | RECV: %d chg, %d plr | JOBS: %d / %d\n",
        //    challengeReqs, playerReqs, challengeResp, playerResp, runningJobs, (int)jobs.size());

        if (runningJobs == 0)
            break;

        std::this_thread::sleep_for(milliseconds(1));
    }


    // update server info player lists
    int numFail = 0;

    for (auto& item : jobs) {
        QueryJob& job = item.second;

        std::string ipstr = netaddr_to_ipstring(job.addr);

        auto serv = g_servers.find(ipstr);
        
        if (serv == g_servers.end()) {
            printf("A2S finished for unknown server: %s\n", ipstr.c_str());
            continue;
        }

        ServerState& state = serv->second;

        state.a2s_players = job.players;
        state.a2s_success = job.success;

        if (!job.success && !state.unreachable)
            numFail++;
    }

    printf("%.2fs (%d failed)\n", (getEpochMillis() - a2sStartTime) / 1000.0f, numFail);
}
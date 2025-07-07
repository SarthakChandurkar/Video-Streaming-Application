// server.cpp – Video Streaming Server Implementation
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <map>
#include <atomic>
#include <signal.h>

const int TIME_QUANTUM_MS = 3000; // 3 seconds
std::map<int, int> chunkProgress; // clientId → current chunk number
std::map<int, int> udpDroppedCount;

#define MAX_BUFFER_SIZE 4096
#define MSG_LEN 1024
#define SIMULATION_DURATION 15    // seconds
#define VIDEO_CHUNK_SIZE 32768    // 32KB chunks

enum MessageType { REQUEST = 1, RESPONSE = 2, VIDEO_DATA = 3, ACK = 4 };

struct VideoResolution {
    std::string name;
    int width, height, bitrate; // Kbps
};

const std::map<std::string, VideoResolution> resolutions = {
    {"480p", {"480p", 720 ,480, 1500}},
    {"720p", {"720p", 1280, 720, 4000}},
    {"1080p",{"1080p",1920,1080,8000}}
};

struct Message {
    int type;
    int length;
    char content[MSG_LEN];
};

struct ClientRequest {
    int clientId;
    int socket;
    struct sockaddr_in address;
    std::string resolution;
    std::string protocol;
    std::chrono::time_point<std::chrono::high_resolution_clock> requestTime;

    // RR streaming state
    int chunkIndex = 0;
    int totalChunks = 0;
    int delayMs = 100;
};


static std::queue<ClientRequest> requestQueue;
static std::mutex queueMutex;
static std::condition_variable queueCondition;
static std::atomic<bool> serverRunning(true);
static std::atomic<int> clientCounter(0);
static std::string schedulingPolicy;




void error(const char *msg) {
    perror(msg);
    exit(1);
}

bool streamPartialTCP(const ClientRequest& req, int& chunkIndex, int maxTimeMs) {
    const auto& res = resolutions.at(req.resolution);
    int cps = (res.bitrate * 1000) / (VIDEO_CHUNK_SIZE * 8);
    cps = std::max(cps, 1);
    int totalChunks = cps * SIMULATION_DURATION;
    int delayMs = req.delayMs;

    // buffer for video data
    static thread_local char videoBuf[VIDEO_CHUNK_SIZE];
    memset(videoBuf, 'V', sizeof(videoBuf));

    auto start = std::chrono::high_resolution_clock::now();
    for (; chunkIndex < totalChunks;) {
        // prepare header
        Message hdr{ VIDEO_DATA, VIDEO_CHUNK_SIZE, {0} };
        snprintf(hdr.content, MSG_LEN, "Chunk %d for %s",
                 chunkIndex, req.resolution.c_str());

        // **SEND HEADER**
        if (send(req.socket, &hdr, sizeof(hdr), 0) < 0) {
            return true;  // socket error → treat as done
        }

        // **SEND PAYLOAD**
        if (send(req.socket, videoBuf, VIDEO_CHUNK_SIZE, 0) < 0) {
            return true;  // socket error → done
        }

        // wait for ACK
        Message ack;
        if (recv(req.socket, &ack, sizeof(ack), 0) < 0) {
            return true;  // socket closed or error → done
        }

        // update progress so next RR slice resumes correctly
        chunkProgress[req.clientId] = chunkIndex + 1;

        std::cout << "[TCP] Sent & ACK chunk "
                  << (chunkIndex + 1)
                  << " to client " << req.clientId << "\n";

        std::this_thread::sleep_for(
            std::chrono::milliseconds(delayMs));

        // time‐slice check
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start).count() >= maxTimeMs) {
            return false;  // yield for RR
        }
    }

    // all chunks done → close once
    close(req.socket);
    return true;
}




void streamVideoTCP(const ClientRequest& req) {
    int sock = req.socket;
    const auto& res = resolutions.at(req.resolution);
    int cps = (res.bitrate * 1000) / (VIDEO_CHUNK_SIZE * 8);
    cps = std::max(cps, 1);
    int totalChunks = cps * SIMULATION_DURATION;
    int delayMs = 100;

    std::cout << "TCP → client " << req.clientId
              << " [" << req.resolution << " @ " << res.bitrate << " Kbps]: "
              << totalChunks << " chunks, " << delayMs << "ms delay\n";

    char buffer[VIDEO_CHUNK_SIZE];
    memset(buffer, 'V', sizeof(buffer));

    for (int i = 0; i < totalChunks && serverRunning; i++) {
        Message hdr{VIDEO_DATA, VIDEO_CHUNK_SIZE, {0}};
        snprintf(hdr.content, MSG_LEN, "Chunk %d for %s", i, req.resolution.c_str());
        if (send(sock, &hdr, sizeof(hdr), 0) < 0) break;
        if (send(sock, buffer, VIDEO_CHUNK_SIZE, 0) < 0) break;
        std::cout<<"TCP Chunk "<<i+1<<" sent to client "<<req.clientId<<"."<<std::endl;
        // wait for ACK
        Message ack;
        if (recv(sock, &ack, sizeof(ack), 0) < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        std::cout<<"Recieved ACK for TCP chunk "<<i+1<<" from client "<<req.clientId<<"."<<std::endl;
    }

    std::cout << "TCP stream done for client " << req.clientId << "\n";
    close(sock);
}


bool streamPartialUDP(const ClientRequest& req, int& currentChunk, int timeMs) {
    int udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSock < 0) {
        perror("UDP socket creation failure");
        return true;
    }

    const auto& res = resolutions.at(req.resolution);
    int cps = (res.bitrate * 1000) / (VIDEO_CHUNK_SIZE * 8);
    cps = std::max(cps, 1);
    int totalChunks = cps * SIMULATION_DURATION;

    char buffer[VIDEO_CHUNK_SIZE];
    memset(buffer, 'V', sizeof(buffer));

    auto start = std::chrono::high_resolution_clock::now();
    srand(time(NULL) + req.clientId);

    while (currentChunk < totalChunks &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::high_resolution_clock::now() - start).count() < timeMs) {

        if ((rand() % 100) < 5) { // 5% packet drop
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "UDP chunk " << currentChunk + 1 << " dropped.\n";
            udpDroppedCount[req.clientId]++;
            currentChunk++;
            continue;
        }

        Message hdr{VIDEO_DATA, VIDEO_CHUNK_SIZE, {0}};
        snprintf(hdr.content, MSG_LEN, "Chunk %d for %s", currentChunk, req.resolution.c_str());

        sendto(udpSock, &hdr, sizeof(hdr), 0,
               (sockaddr*)&req.address, sizeof(req.address));
        sendto(udpSock, buffer, VIDEO_CHUNK_SIZE, 0,
               (sockaddr*)&req.address, sizeof(req.address));

        std::cout << "UDP Chunk " << currentChunk + 1 << " sent to client " << req.clientId << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        currentChunk++;
    }

    bool done = (currentChunk >= totalChunks);

    if (done) {
        Message fin{ACK, sizeof(int), {0}};
        int dropped = udpDroppedCount[req.clientId];
        memcpy(fin.content, &dropped, sizeof(int));
        sendto(udpSock, &fin, sizeof(fin), 0,
               (sockaddr*)&req.address, sizeof(req.address));
        udpDroppedCount.erase(req.clientId); // cleanup
    }

    close(udpSock);
    return done;
}




void streamVideoUDP(const ClientRequest& req) {
    int udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSock < 0) error("UDP socket creation failure");

    const auto& res = resolutions.at(req.resolution);
    int cps = (res.bitrate * 1000) / (VIDEO_CHUNK_SIZE * 8);
    cps = std::max(cps, 1);
    int totalChunks = cps * SIMULATION_DURATION;
    int delayMs = 100;

    std::cout << "UDP → client " << req.clientId
              << " [" << req.resolution << " @ " << res.bitrate << " Kbps]: "
              << totalChunks << " chunks, " << delayMs << "ms delay\n";

    char buffer[VIDEO_CHUNK_SIZE];
    memset(buffer, 'V', sizeof(buffer));
    srand(time(NULL) + req.clientId);
    int dropped = 0;

    for (int i = 0; i < totalChunks && serverRunning; i++) {
        if ((rand() % 100) < 5) { // 5% drop
            dropped++;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            std::cout<<"UDP chunk "<<i+1<<" dropped."<<std::endl;
            continue;
        }
        Message hdr{VIDEO_DATA, VIDEO_CHUNK_SIZE, {0}};
        snprintf(hdr.content, MSG_LEN, "Chunk %d for %s", i, req.resolution.c_str());
        sendto(udpSock, &hdr, sizeof(hdr), 0,
               (sockaddr*)&req.address, sizeof(req.address));
        sendto(udpSock, buffer, VIDEO_CHUNK_SIZE, 0,
               (sockaddr*)&req.address, sizeof(req.address));
        std::cout<<"UDP Chunk "<<i+1<<" sent to client "<<req.clientId<<"."<<std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    // final ACK with drop count
    Message fin{ACK, sizeof(int), {0}};
    memcpy(fin.content, &dropped, sizeof(int));
    sendto(udpSock, &fin, sizeof(fin), 0,
           (sockaddr*)&req.address, sizeof(req.address));

    float rate = (float)dropped / totalChunks * 100;
    std::cout << "UDP done for client " << req.clientId
              << " (dropped " << dropped << "/" << totalChunks
              << " = " << rate << "%)\n";

    close(udpSock);
}

// [MOD] Add control for max concurrent threads
const int MAX_CONCURRENT_STREAMS = 2;
std::mutex streamMutex;
std::condition_variable streamCV;
std::atomic<int> activeStreams(0);

void streamVideoWrapper(const ClientRequest& cr) {
    {
        std::unique_lock<std::mutex> lk(streamMutex);
        streamCV.wait(lk, [] { return activeStreams < MAX_CONCURRENT_STREAMS || !serverRunning; });
        if (!serverRunning) return;
        activeStreams++;
    }

    if (cr.protocol == "TCP")
        streamVideoTCP(cr);
    else
        streamVideoUDP(cr);

    {
        std::lock_guard<std::mutex> lk(streamMutex);
        activeStreams--;
    }
    streamCV.notify_one();
}

void handleTCPConnection(int clientSock, struct sockaddr_in clientAddr) {
    int id = ++clientCounter;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));

    Message reqMsg;
    if (recv(clientSock, &reqMsg, sizeof(reqMsg), 0) <= 0) {
        close(clientSock);
        return;
    }

    // parse resolution, protocol, (optional) UDP port
    char resStr[32], proto[16];
    int udpPort = 0;
    sscanf(reqMsg.content, "%31s %15s %d", resStr, proto, &udpPort);
    std::string resolution(resStr), protocol(proto);

    std::cout << "Client " << id << " @" << ip << ":" << ntohs(clientAddr.sin_port)
              << " → wants " << resolution << " via " << protocol;
    if (protocol == "UDP") {
        clientAddr.sin_port = htons(udpPort);
        std::cout << " (UDP port " << udpPort << ")";
    }
    std::cout << "\n";

    // validate
    if (!resolutions.count(resolution)) {
        std::cerr << "  ✗ bad resolution\n";
        close(clientSock);
        return;
    }

    // send RESPONSE
    Message resp{RESPONSE, 0, {0}};
    const auto& R = resolutions.at(resolution);
    resp.length = snprintf(resp.content, MSG_LEN,
                           "Video %s %dx%d @ %dKbps",
                           R.name.c_str(), R.width, R.height, R.bitrate);
    send(clientSock, &resp, sizeof(resp), 0);

    // enqueue
    ClientRequest cr;
    cr.clientId   = id;
    cr.socket     = clientSock;
    cr.address    = clientAddr;
    cr.resolution = resolution;
    cr.protocol   = protocol;
    cr.requestTime = std::chrono::high_resolution_clock::now();

    {
        std::lock_guard<std::mutex> lg(queueMutex);
        requestQueue.push(cr);
    }
    queueCondition.notify_one();

    // if UDP, we no longer need the TCP socket open
    if (protocol == "UDP") {
        close(clientSock);
    }
}

void processRequests() {
    std::vector<ClientRequest> active;
    size_t idx = 0;

    while (serverRunning) {
        ClientRequest cr;
        bool have = false;

        std::unique_lock<std::mutex> lk(queueMutex);
        if (requestQueue.empty() && active.empty()) {
            queueCondition.wait(lk, [] { return !requestQueue.empty() || !serverRunning; });
            if (!serverRunning) break;
        }

        while (!requestQueue.empty()) {
            active.push_back(requestQueue.front());
            requestQueue.pop();
        }

        if (!active.empty()) {
            if (schedulingPolicy == "FCFS") {
                cr = active.front();
                active.erase(active.begin());
            } else if (schedulingPolicy == "RR") {
                cr = active[idx];
                active.erase(active.begin() + idx);
                if (idx >= active.size()) idx = 0;
            }
            have = true;
        }

        lk.unlock();

        if (have) {
            if (schedulingPolicy == "FCFS") {
                // original FCFS behavior — run until completion
                std::thread(streamVideoWrapper, cr).detach();
            } else if (schedulingPolicy == "RR") {
                // RR behavior — run only for TIME_QUANTUM_MS
                std::thread([cr]() {
                    {
                        std::unique_lock<std::mutex> lk(streamMutex);
                        streamCV.wait(lk, [] { return activeStreams < MAX_CONCURRENT_STREAMS || !serverRunning; });
                        if (!serverRunning) return;
                        activeStreams++;
                    }

                    // auto start = std::chrono::high_resolution_clock::now();
                    int& progress = chunkProgress[cr.clientId];  // this map tracks per-client progress

                    bool done = false;
                    if (cr.protocol == "TCP") {
                        done = streamPartialTCP(cr, progress, TIME_QUANTUM_MS);
                    } else {
                        done = streamPartialUDP(cr, progress, TIME_QUANTUM_MS);
                    }

                    {
                        std::lock_guard<std::mutex> lk(streamMutex);
                        activeStreams--;
                    }
                    streamCV.notify_one();

                    if (!done && serverRunning) {
                        std::lock_guard<std::mutex> lg(queueMutex);
                        requestQueue.push(cr);  // re-queue for next RR slice
                        queueCondition.notify_one();
                    }
                }).detach();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}




int signalListenSock = 0;
void signalHandler(int s) {
    std::cout << "Server shut down\n";
    serverRunning = false;
    close(signalListenSock);
    queueCondition.notify_all();
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <FCFS|RR>\n";
        return 1;
    }
    int port = std::stoi(argv[1]);
    schedulingPolicy = argv[2];
    if (schedulingPolicy != "FCFS" && schedulingPolicy != "RR") {
        std::cerr << "Policy must be FCFS or RR\n";
        return 1;
    }

    signal(SIGINT, signalHandler);

    int listenSock = socket(AF_INET, SOCK_STREAM, 0);
    signalListenSock = listenSock;
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(port);
    bind(listenSock, (sockaddr*)&srv, sizeof(srv));
    listen(listenSock, 10);

    std::cout << "Server listening on port " << port
              << " [" << schedulingPolicy << "]\n";

    std::thread(processRequests).detach();

    while (serverRunning) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int cs = accept(listenSock, (sockaddr*)&cli, &len);
        if (cs >= 0)
            std::thread(handleTCPConnection, cs, cli).detach();
    }

    close(listenSock);
    std::cout << "Server shut down\n";
    return 0;
}
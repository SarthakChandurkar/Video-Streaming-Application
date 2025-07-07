// client.cpp – Video Streaming Client (TCP & UDP)
#include <iostream>
#include <string>
#include <cstring>
#include <numeric>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/time.h>
#include<math.h>
#include<fstream>


#define VIDEO_CHUNK_SIZE 32768
#define MSG_LEN 1024

enum MessageType { REQUEST = 1, RESPONSE, VIDEO_DATA, ACK };

struct Message {
    MessageType type;
    int length;
    char content[MSG_LEN];
};

struct Metrics {
    std::chrono::high_resolution_clock::time_point startTime;
    size_t totalBytes = 0;
    float throughput = 0;
    int received = 0;
    int lost = 0;
    std::vector<double> latencies;
};
static Metrics M;
static std::mutex Mtx;

void error(const char* s) {
    perror(s);
    exit(1);
}

void showMetrics(std::string res,std::string proto) {
    std::lock_guard<std::mutex> lk(Mtx);
    double avg = M.latencies.empty()
        ? 0
        : std::accumulate(M.latencies.begin(), M.latencies.end(), 0.0) / M.latencies.size();
    auto dur = std::chrono::high_resolution_clock::now() - M.startTime;
    std::cout << "\n--- Metrics ---\n"
              << "Bytes:            " << M.totalBytes << "\n"
              << "Throughput:       " << M.totalBytes/ (std::chrono::duration<double>(dur).count() * pow(10,6)) << " Mbps\n"
              << "Pkts Recieved:    " << M.received << "\n"
              << "Pkts Lost:        " << M.lost << "\n"
              << "Avg Lat:          " << avg/float(1000.00) << " s\n"
              << "Duration:         " << std::chrono::duration<double>(dur).count() << " s\n";

}

void streamUDP(const std::string& ip, int port, const std::string& res) {
    // 1) TCP handshake
    int ts = socket(AF_INET, SOCK_STREAM, 0);
    if (ts < 0) error("TCP socket");
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &srv.sin_addr);
    if (connect(ts, (sockaddr*)&srv, sizeof(srv)) < 0) error("connect");

    // 2) bind UDP
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us < 0) error("UDP socket");
    sockaddr_in cli{AF_INET, 0, INADDR_ANY};
    bind(us, (sockaddr*)&cli, sizeof(cli));
    socklen_t l = sizeof(cli);
    getsockname(us, (sockaddr*)&cli, &l);
    int up = ntohs(cli.sin_port);

    // send REQUEST
    Message req{REQUEST,0,{0}};
    req.length = snprintf(req.content, MSG_LEN, "%s UDP %d", res.c_str(), up);
    send(ts, &req, sizeof(req), 0);
    M.startTime = std::chrono::high_resolution_clock::now();
    
    // recv RESPONSE
    Message rsp;
    if (recv(ts, &rsp, sizeof(rsp), 0) <= 0) error("recv RESP");
    std::cout << "Server: " << rsp.content << "\nUDP streaming...\n";
    close(ts);

    // timeout
    timeval tv{100,0};
    setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));


    // recv loop
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    while (1) {
        Message hdr;
        ssize_t h = recvfrom(us, &hdr, sizeof(hdr), 0, (sockaddr*)&from, &fl);
        if (h <= 0) {
            if (errno==EAGAIN||errno==EWOULDBLOCK) break;
            error("recvfrom hdr");
        }
        if (hdr.type == ACK) {
            memcpy(&M.lost, hdr.content, sizeof(int));
            break;
        }
        if (hdr.type != VIDEO_DATA) continue;

        auto now = std::chrono::high_resolution_clock::now();
        double lat = std::chrono::duration<double,std::milli>(now - M.startTime).count();
        {
            std::lock_guard<std::mutex> lk(Mtx);
            M.latencies.push_back(lat);
        }

        // get data
        std::vector<char> buf(hdr.length);
        ssize_t d = recvfrom(us, buf.data(), hdr.length, 0, (sockaddr*)&from, &fl);
        if (d > 0) {
            std::lock_guard<std::mutex> lk(Mtx);
            std::cout<<"Size: "<<d<<std::endl;
            M.totalBytes += d;
            M.received++;
            std::cout<<"Recieved "<<M.received<<" Chunks till now."<<std::endl;
        }
    }
    

    close(us);
    showMetrics(res,"UDP");
}

void streamTCP(const std::string& ip, int port, const std::string& res) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s<0) error("socket");
    sockaddr_in srv{AF_INET, htons(port), 0};
    inet_pton(AF_INET, ip.c_str(), &srv.sin_addr);
    if (connect(s,(sockaddr*)&srv,sizeof(srv))<0) error("connect");

    Message req{REQUEST,0,{0}};
    req.length = snprintf(req.content, MSG_LEN, "%s TCP 0", res.c_str());
    send(s, &req, sizeof(req), 0);
    M.startTime = std::chrono::high_resolution_clock::now();

    Message rsp;
    if (recv(s, &rsp, sizeof(rsp), 0)<=0) error("recv RESP");
    std::cout<<"Server: "<<rsp.content<<"\nTCP streaming...\n";

    timeval tv{100,0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        Message hdr;
        if (recv(s, &hdr, sizeof(hdr), 0)<=0) break;
        if (hdr.type != VIDEO_DATA) break;

        auto now = std::chrono::high_resolution_clock::now();
        double lat = std::chrono::duration<double,std::milli>(now - M.startTime).count();
        {
            std::lock_guard<std::mutex> lk(Mtx);
            M.latencies.push_back(lat);
        }

        std::vector<char> buf(hdr.length);
        ssize_t n = recv(s, buf.data(), hdr.length, 0);
        std::cout<<"Size: "<<n<<std::endl;
        if (n>0) {
            std::lock_guard<std::mutex> lk(Mtx);
            M.totalBytes += n;
            M.received++;
            std::cout<<"Recieved "<<M.received<<" Chunks till now."<<std::endl;
        }
        Message ack{ACK,0,{0}};
        send(s, &ack, sizeof(ack), 0);
    }

    close(s);
    showMetrics(res,"TCP");
}

int main(int argc, char* argv[]) {
    if (argc!=5) {
        std::cerr<<"Usage: "<<argv[0]
                 <<" <server_ip> <port> <480p|720p|1080p> <TCP|UDP>\n";
        return 1;
    }
    std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    std::string res = argv[3];
    std::string proto = argv[4];

    if (proto == "UDP")      streamUDP(ip, port, res);
    else if (proto == "TCP") streamTCP(ip, port, res);
    else {
        std::cerr<<"Protocol must be TCP or UDP\n";
        return 1;
    }
    return 0;
}

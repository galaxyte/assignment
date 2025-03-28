#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdint>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define closesocket close
#endif

class SimpleJSON {
private:
    std::vector<std::map<std::string, std::string>> data;

public:
    void addObject(const std::map<std::string, std::string>& obj) {
        data.push_back(obj);
    }

    std::string stringify() {
        std::stringstream ss;
        ss << "[\n";
        for (size_t i = 0; i < data.size(); ++i) {
            ss << "  {\n";
            size_t count = 0;
            for (const auto& pair : data[i]) {
                ss << "    \"" << pair.first << "\": \"" << pair.second << "\"";
                if (count < data[i].size() - 1) ss << ",";
                ss << "\n";
                count++;
            }
            ss << "  }";
            if (i < data.size() - 1) ss << ",";
            ss << "\n";
        }
        ss << "]\n";
        return ss.str();
    }
};

class ABXExchangeClient {
private:
    int sock;
    const char* SERVER_IP = "127.0.0.1";
    const int SERVER_PORT = 3000;
    std::map<int, std::map<std::string, std::string>> receivedPackets;
    std::set<int> missingSequences;

    uint32_t bytesToUint32(const unsigned char* bytes) {
        return (static_cast<uint32_t>(bytes[0]) << 24) | 
               (static_cast<uint32_t>(bytes[1]) << 16) | 
               (static_cast<uint32_t>(bytes[2]) << 8)  | 
               static_cast<uint32_t>(bytes[3]);
    }

    void sendRequest(uint8_t callType, uint8_t resendSeq = 0) {
        unsigned char request[2] = {callType, resendSeq};
        send(sock, reinterpret_cast<const char*>(request), 2, 0);
    }

    std::map<std::string, std::string> parsePacket(const unsigned char* packetData) {
        std::map<std::string, std::string> packet;
        char symbol[5] = {0};
        memcpy(symbol, packetData, 4);
        packet["symbol"] = std::string(symbol);
        packet["buysellindicator"] = std::string(1, static_cast<char>(packetData[4]));
        packet["quantity"] = std::to_string(bytesToUint32(packetData + 5));
        packet["price"] = std::to_string(bytesToUint32(packetData + 9));
        packet["packetSequence"] = std::to_string(bytesToUint32(packetData + 13));
        return packet;
    }

    void findMissingSequences() {
        int maxSequence = 0;
        for (const auto& pair : receivedPackets) {
            maxSequence = std::max(maxSequence, std::stoi(pair.second.at("packetSequence")));
        }

        for (int i = 1; i <= maxSequence; ++i) {
            if (receivedPackets.find(i) == receivedPackets.end()) {
                missingSequences.insert(i);
            }
        }
    }
   
public:
    ABXExchangeClient() : sock(-1) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed\n";
            exit(1);
        }
#endif
    }

    bool connect() {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == -1) {
            std::cerr << "Socket creation error\n";
            return false;
        }

        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(SERVER_PORT);

#ifdef _WIN32
        serv_addr.sin_addr.s_addr = inet_addr(SERVER_IP);  // ✅ Windows Fix
#else
        inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr); // ✅ macOS/Linux
#endif

        if (::connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
            std::cerr << "Connection failed\n";
            return false;
        }

        return true;
    }

    void fetchData() {
        sendRequest(1);
        unsigned char buffer[17];

        // ✅ Set timeout for recv()
#ifdef _WIN32
        DWORD timeout = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#endif

        while (true) {
            int bytesRead = recv(sock, reinterpret_cast<char*>(buffer), 17, 0);
            if (bytesRead <= 0) {
                std::cout << " No more data. Stopping reception.\n";
                break;
            }

            auto packet = parsePacket(buffer);
            receivedPackets[std::stoi(packet["packetSequence"])] = packet;
            std::cout << " Received packet: " << packet["packetSequence"] << " - " << packet["symbol"] << std::endl;
        }

        findMissingSequences();

        for (int seq : missingSequences) {
            sendRequest(2, seq);
            int bytesRead = recv(sock, reinterpret_cast<char*>(buffer), 17, 0);
            if (bytesRead > 0) {
                auto packet = parsePacket(buffer);
                receivedPackets[std::stoi(packet["packetSequence"])] = packet;
                std::cout << " Received missing packet: " << packet["packetSequence"] << " - " << packet["symbol"] << std::endl;
            }
        }

        std::cout << " All packets received. Saving JSON..." << std::endl;
        saveToJsonFile("output.json");
    }

    void saveToJsonFile(const std::string& filename) {
        SimpleJSON json;
        for (const auto& pair : receivedPackets) {
            json.addObject(pair.second);
        }

        std::ofstream file(filename);
        file << json.stringify();
        file.close();

        std::cout << " JSON file saved: " << filename << std::endl;
    }

    ~ABXExchangeClient() {
        if (sock != -1) {
            closesocket(sock);
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

int main() {
    ABXExchangeClient client;
    if (!client.connect()) {
        std::cerr << " Failed to connect to server" << std::endl;
        return 1;
    }

    client.fetchData();
    return 0;
}

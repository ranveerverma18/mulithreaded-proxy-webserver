#include<bits/stdc++.h>
#include <winsock2.h>
#include <ws2tcpip.h>
using namespace std;
#pragma comment(lib, "ws2_32.lib")

constexpr int BUFFER_SIZE = 8192;

string extractHost(const std::string& request) {
    size_t pos = request.find("Host:");
    if (pos == std::string::npos) return "";

    size_t start = pos + 5;
    while (start < request.size() && request[start] == ' ')
        start++;

    size_t end = request.find("\r\n", start);
    return request.substr(start, end - start);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(9090);

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    listen(serverSocket, SOMAXCONN);
    cout << "Proxy listening on port 9090...\n";

    SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
    if (clientSocket == INVALID_SOCKET) {
        cerr << "Accept failed\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    char buffer[BUFFER_SIZE];
    int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0);
    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 0;
    }

    string request(buffer, bytesReceived);
    string host = extractHost(request);

    if (host.empty()) {
        cerr << "No Host header found\n";
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    cout << "Forwarding request to host: " << host << std::endl;

    // Resolve host
    addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), "80", &hints, &res) != 0) {
        cerr << "DNS resolution failed\n";
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    SOCKET remoteSocket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(remoteSocket, res->ai_addr, res->ai_addrlen) == SOCKET_ERROR) {
        cerr << "Connection to remote server failed\n";
        freeaddrinfo(res);
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(res);

    // Forward request to remote server
    send(remoteSocket, request.c_str(), request.size(), 0);

    // Relay response back to client
    int bytesRead;
    while ((bytesRead = recv(remoteSocket, buffer, BUFFER_SIZE, 0)) > 0) {
        send(clientSocket, buffer, bytesRead, 0);
    }

    closesocket(remoteSocket);
    closesocket(clientSocket);
    closesocket(serverSocket);
    WSACleanup();

    return 0;
}



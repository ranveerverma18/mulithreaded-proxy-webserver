#include<bits/stdc++.h>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
using namespace std;
#pragma comment(lib, "ws2_32.lib")

constexpr int BUFFER_SIZE = 8192;
constexpr int THREAD_POOL_SIZE = 8;
constexpr size_t MAX_QUEUE_SIZE = 100;
constexpr int CLIENT_TIMEOUT_MS = 5000;   // 5s
constexpr int REMOTE_TIMEOUT_MS = 5000;   // 5s

//using cerr for error messages and cout for normal msgs
string extractHost(const std::string& request) {
    size_t pos = request.find("Host:");
    if (pos == string::npos) return "";

    size_t start = pos + 5;
    while (start < request.size() && request[start] == ' ')
        start++;

    size_t end = request.find("\r\n", start);
    return request.substr(start, end - start);
}

string normalizeRequest(const std::string& request, const std::string& host) {
    size_t lineEnd = request.find("\r\n");
    if (lineEnd == string::npos) return request;

    string requestLine = request.substr(0, lineEnd);
    string rest = request.substr(lineEnd);

    // Example:
    // GET http://example.com/path HTTP/1.1
    size_t methodEnd = requestLine.find(' ');
    size_t versionPos = requestLine.rfind(" HTTP/");

    if (methodEnd == string::npos || versionPos == string::npos)
        return request;

    string method = requestLine.substr(0, methodEnd);
    string version = requestLine.substr(versionPos);
    string url = requestLine.substr(methodEnd + 1,
                                         versionPos - methodEnd - 1);

    // Strip scheme + host
    string path = "/";
    size_t pathPos = url.find(host);
    if (pathPos != string::npos) {
        path = url.substr(pathPos + host.length());
        if (path.empty()) path = "/";
    }

    return method + " " + path + version + rest;
}


string buildForwardRequest(const string& request,
                                const string& host) {
    size_t lineEnd = request.find("\r\n");    // \r\n is the standard pattern used in netwroking to indicate eol in HTTP headers, so we can use it to find the end of the request line
    if (lineEnd == string::npos) return request;

    // ---- Request line ----
    string requestLine = request.substr(0, lineEnd);

    // ---- Normalize request line (absolute-form → origin-form) ----
    size_t methodEnd = requestLine.find(' ');
    size_t versionPos = requestLine.rfind(" HTTP/");
    if (methodEnd == string::npos || versionPos == string::npos)
        return request;

    string method = requestLine.substr(0, methodEnd);
    string version = requestLine.substr(versionPos);
    string url = requestLine.substr(
        methodEnd + 1, versionPos - methodEnd - 1);

    string path = "/";
    size_t hostPos = url.find(host);
    if (hostPos != string::npos) {
        path = url.substr(hostPos + host.length());
        if (path.empty()) path = "/";
    }

    std::string result = method + " " + path + version + "\r\n";

    // ---- Headers ----
    size_t pos = lineEnd + 2;
    bool hostSeen = false;

    while (true) {
        size_t next = request.find("\r\n", pos);
        if (next == string::npos) break;

        string line = request.substr(pos, next - pos);
        pos = next + 2;

        if (line.empty()) break;

        if (line.rfind("Host:", 0) == 0) {
            hostSeen = true;
            result += line + "\r\n";
            continue;
        }

        if (line.rfind("Proxy-Connection:", 0) == 0) continue;
        if (line.rfind("Connection:", 0) == 0) continue;

        result += line + "\r\n";
    }

    // ---- Ensure Host exists ----
    if (!hostSeen) {
        result += "Host: " + host + "\r\n";
    }

    // ---- Force connection close ----
    result += "Connection: close\r\n\r\n";

    return result;
}


void setSocketTimeouts(SOCKET sock, int timeoutMs) {
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,                      //RCVTIMEO for recv timeout(flag for input operations)
               (const char*)&timeoutMs, sizeof(timeoutMs));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,                      //SNDTIMEO for send timeout(flag for output operations)
               (const char*)&timeoutMs, sizeof(timeoutMs));
}


// This function runs INSIDE A THREAD
void handleClient(SOCKET clientSocket) {
    setSocketTimeouts(clientSocket, CLIENT_TIMEOUT_MS);   // Set timeouts for client socket  //if client doesn't send data within 5s, recv will return with error and we can close the connection to free resources
    string request;
    char buffer[BUFFER_SIZE];
while (true) {
    int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0);

    if (bytesReceived > 0) {
        request.append(buffer, bytesReceived);

        // Stop once HTTP headers are fully received
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    else if (bytesReceived == 0) {
        // Client closed connection before sending full request
        closesocket(clientSocket);
        return;
    }
    else {
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            cerr << "[Timeout] Client recv timed out (Slowloris protection)\n";
        } else {
            cerr << "[Error] Client recv failed: " << err << "\n";
        }
        closesocket(clientSocket);
        return;
    }
}

if (request.empty()) {
    closesocket(clientSocket);
    return;
}
    string host = extractHost(request);

    if (host.empty()) {
        cerr << "No Host header found\n";
        closesocket(clientSocket);
        return;
    }

    addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), "80", &hints, &res) != 0) {
        cerr << "DNS resolution failed\n";
        closesocket(clientSocket);
        return;
    }

    SOCKET remoteSocket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    setSocketTimeouts(remoteSocket, REMOTE_TIMEOUT_MS);   // Set timeouts for remote socket
    bool connected = false;
    for(int attempt=0;attempt<2;attempt++)
    {
        if(connect(remoteSocket, res->ai_addr, res->ai_addrlen) == 0) {
            connected = true;
            break;
        }
    }
    if(!connected) {
        freeaddrinfo(res);
        closesocket(remoteSocket);
        closesocket(clientSocket);
        return;
    }
    freeaddrinfo(res);

    // Forward request
    string forwardRequest = buildForwardRequest(request, host);
    send(remoteSocket, forwardRequest.c_str(), forwardRequest.size(), 0);


    // Relay response
    int bytesRead;
    while ((bytesRead = recv(remoteSocket, buffer, BUFFER_SIZE, 0)) > 0) {
        send(clientSocket, buffer, bytesRead, 0);
    }

    closesocket(remoteSocket);
    closesocket(clientSocket);
}

queue<SOCKET> taskQueue;
mutex queueMutex;
condition_variable queueCV;
bool shutdownPool = false;

void workerThread() {
    while (true) {
        SOCKET clientSocket;
        {
            unique_lock<mutex> lock(queueMutex);
            queueCV.wait(lock, [] {
                return !taskQueue.empty() || shutdownPool;
            });

            if (shutdownPool && taskQueue.empty()) {
                return; // exit thread
            }

            clientSocket = taskQueue.front();
            taskQueue.pop();
        }

        handleClient(clientSocket);
    }
}

void sendServiceUnavailable(SOCKET clientSocket) {
    const char* response =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Server overloaded. Please try again later.\n";

    send(clientSocket, response, strlen(response), 0);
    closesocket(clientSocket);
}


int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        cerr << "Socket creation failed\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(9090);

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "Bind failed\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    listen(serverSocket, SOMAXCONN);
    cout << "Multithreaded proxy listening on port 9090...\n";

    vector<thread> workers;

    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
    workers.emplace_back(workerThread);
   }

    //loop to accept clients 
    while (true) {
    SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
    if (clientSocket == INVALID_SOCKET) {
        cerr << "Accept failed\n";
        continue;
    }

    bool queued = false;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (taskQueue.size() < MAX_QUEUE_SIZE) {
            taskQueue.push(clientSocket);
            queued = true;
        }
    }

    if (queued) {
        queueCV.notify_one();
    } else {
        // Backpressure: reject client
        sendServiceUnavailable(clientSocket);
    }
}

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}




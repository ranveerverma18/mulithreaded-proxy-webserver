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
constexpr size_t MAX_CACHE_SIZE=10*1024*1024; // 10MB
atomic<bool> shutdownRequested(false);
atomic<int> activeConnections(0);

// Task queue and synchronization primitives for worker threads
queue<SOCKET> taskQueue;
mutex queueMutex;
condition_variable queueCV;
bool shutdownPool = false;

struct CacheEntry {
    string response;
    time_t timestamp;
    size_t size;
};

list<string>lrulist;
unordered_map<string,pair<CacheEntry, list<string>::iterator>> cache;

size_t currentCacheSize = 0;
mutex cacheMutex;

// Control command handler (for admin interface)
string handleControlCommand(const string& cmd) {
    if (cmd == "STATS") {
        lock_guard<mutex> lock(cacheMutex);

        return "{\n"
               "  \"active_connections\": " + to_string(activeConnections.load()) + ",\n"
               "  \"cache_entries\": " + to_string(cache.size()) + ",\n"
               "  \"cache_size_bytes\": " + to_string(currentCacheSize) + "\n"
               "}\n";
    }

    if (cmd == "CLEAR_CACHE") {
        lock_guard<mutex> lock(cacheMutex);
        cache.clear();
        lrulist.clear();
        currentCacheSize = 0;
        return "OK: cache cleared\n";
    }

    if (cmd == "SHUTDOWN") {
        shutdownRequested = true;
        shutdownPool = true;
        queueCV.notify_all();
        return "OK: shutting down\n";
    }

    return "ERROR: unknown command\n";
}

//using cerr for error messages and cout for normal msgs
string extractMethod(const std::string& request) {
    size_t end = request.find(' ');
    if (end == string::npos) return "";
    return request.substr(0, end);
}

string extractpath(const string& request)
{
    size_t methodEnd=request.find(' ');
    if(methodEnd==string::npos) return "";

    size_t pathEnd=request.find(' ',methodEnd+1);
    if(pathEnd==string::npos) return "";

    return request.substr(methodEnd+1,pathEnd-methodEnd-1);
}

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

void sendAll(SOCKET sock, const std::string& data) {
    int totalSent = 0;
    int length = data.size();

    while (totalSent < length) {
        int sent = send(sock, data.data() + totalSent, length - totalSent, 0);
        if (sent == SOCKET_ERROR) {
            return;
        }
        totalSent += sent;
    }
}


void handleConnect(SOCKET clientSocket, const string& request) {
    // Extract host and port from CONNECT request
    // Format: CONNECT example.com:443 HTTP/1.1
    
    size_t methodEnd = request.find(' ');
    if (methodEnd == string::npos) {
        closesocket(clientSocket);
        return;
    }
    
    size_t hostStart = methodEnd + 1;
    size_t hostEnd = request.find(' ', hostStart);
    if (hostEnd == string::npos) {
        closesocket(clientSocket);
        return;
    }
    
    string hostPort = request.substr(hostStart, hostEnd - hostStart);
    
    // Parse host:port
    size_t colonPos = hostPort.find(':');
    string host;
    string port = "443"; // default HTTPS port
    
    if (colonPos != string::npos) {
        host = hostPort.substr(0, colonPos);
        port = hostPort.substr(colonPos + 1);
    } else {
        host = hostPort;
    }
    
    cout << "[CONNECT] Tunneling to " << host << ":" << port << endl;
    
    // Resolve and connect to remote server
    addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        cerr << "[CONNECT] DNS resolution failed for " << host << endl;
        const char* errorResponse = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(clientSocket, errorResponse, strlen(errorResponse), 0);
        closesocket(clientSocket);
        return;
    }
    
    SOCKET remoteSocket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (remoteSocket == INVALID_SOCKET) {
        cerr << "[CONNECT] Socket creation failed\n";
        freeaddrinfo(res);
        const char* errorResponse = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(clientSocket, errorResponse, strlen(errorResponse), 0);
        closesocket(clientSocket);
        return;
    }
    
    // Set timeouts for remote socket
    setSocketTimeouts(remoteSocket, REMOTE_TIMEOUT_MS);
    
    // Connect to remote server
    if (connect(remoteSocket, res->ai_addr, res->ai_addrlen) != 0) {
        cerr << "[CONNECT] Connection failed to " << host << ":" << port << endl;
        freeaddrinfo(res);
        closesocket(remoteSocket);
        const char* errorResponse = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(clientSocket, errorResponse, strlen(errorResponse), 0);
        closesocket(clientSocket);
        return;
    }
    
    freeaddrinfo(res);
    
    // Send success response to client
    const char* successResponse = "HTTP/1.1 200 Connection Established\r\n\r\n";
    send(clientSocket, successResponse, strlen(successResponse), 0);
    
    cout << "[CONNECT] Tunnel established to " << host << ":" << port << endl;
    
    // Now relay data bidirectionally (encrypted SSL/TLS tunnel)
    // We use non-blocking sockets and select() for bidirectional relay
    
    // Set sockets to non-blocking mode
    u_long mode = 1;
    ioctlsocket(clientSocket, FIONBIO, &mode);
    ioctlsocket(remoteSocket, FIONBIO, &mode);
    
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    
    while (true) {
        FD_ZERO(&readfds);
        FD_SET(clientSocket, &readfds);
        FD_SET(remoteSocket, &readfds);
        
        // Set timeout for select
        timeval timeout;
        timeout.tv_sec = 60;  // 60 second timeout
        timeout.tv_usec = 0;
        
        SOCKET maxfd = (clientSocket > remoteSocket) ? clientSocket : remoteSocket;
        int activity = select(maxfd + 1, &readfds, nullptr, nullptr, &timeout);
        
        if (activity < 0) {
            cerr << "[CONNECT] Select error\n";
            break;
        }
        
        if (activity == 0) {
            // Timeout - connection idle too long
            cout << "[CONNECT] Tunnel timeout for " << host << endl;
            break;
        }
        
        // Data from client -> remote server
        if (FD_ISSET(clientSocket, &readfds)) {
            int bytesRead = recv(clientSocket, buffer, BUFFER_SIZE, 0);
            if (bytesRead <= 0) {
                // Client closed connection
                cout << "[CONNECT] Client closed tunnel to " << host << endl;
                break;
            }
            
            int bytesSent = send(remoteSocket, buffer, bytesRead, 0);
            if (bytesSent <= 0) {
                cerr << "[CONNECT] Failed to send to remote\n";
                break;
            }
        }
        
        // Data from remote server -> client
        if (FD_ISSET(remoteSocket, &readfds)) {
            int bytesRead = recv(remoteSocket, buffer, BUFFER_SIZE, 0);
            if (bytesRead <= 0) {
                // Remote server closed connection
                cout << "[CONNECT] Remote closed tunnel to " << host << endl;
                break;
            }
            
            int bytesSent = send(clientSocket, buffer, bytesRead, 0);
            if (bytesSent <= 0) {
                cerr << "[CONNECT] Failed to send to client\n";
                break;
            }
        }
    }
    
    closesocket(remoteSocket);
    closesocket(clientSocket);
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
    
    string method = extractMethod(request);

    // Handle CONNECT method separately for HTTPS tunneling
    if (method == "CONNECT") {
        handleConnect(clientSocket, request);
        return;  // handleConnect() closes sockets
    }

    string path = extractpath(request);
    string host = extractHost(request);

    if (host.empty()) {
        cerr << "No Host header found\n";
        closesocket(clientSocket);
        return;
    }
    
    //CACHE LOOKUP
    if (method == "GET") {
        string cacheKey = host + path;

        {
            lock_guard<std::mutex> lock(cacheMutex);
            auto it = cache.find(cacheKey);
            if (it != cache.end()) {
                cout<<"[Cache Hit] " << cacheKey << "\n";
                // LRU update
                lrulist.erase(it->second.second);
                lrulist.push_front(cacheKey);
                it->second.second = lrulist.begin();

                // Send cached response
                sendAll(clientSocket, it->second.first.response);
                closesocket(clientSocket);
                return;
            }
            cout<<"[Cache Miss] " << cacheKey << "\n";
        }
    }
    
    //this part runs if cache miss
    addrinfo hints{}, *res;      // Resolve hostname to IP address 
    hints.ai_family = AF_INET;   //IPv4
    hints.ai_socktype = SOCK_STREAM;  //TCP

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
    string fullResponse;
    while ((bytesRead = recv(remoteSocket, buffer, BUFFER_SIZE, 0)) > 0) {
        send(clientSocket, buffer, bytesRead, 0);
        fullResponse.append(buffer, bytesRead);
    }

    if (method == "GET" && fullResponse.find("200 OK") != string::npos) {
        string cacheKey = host + path;

        lock_guard<mutex> lock(cacheMutex);
        
        //checking if the response can fit in the cache
        while (currentCacheSize + fullResponse.size() > MAX_CACHE_SIZE) {
        string lruKey = lrulist.back();           //oldest item which is at the very back
        currentCacheSize -= cache[lruKey].first.size;
        lrulist.pop_back();     //remove from the lru list
        cache.erase(lruKey);    //remove from cache
    }
    
    //adding to the front of the lru list as it's the most recently used item now
    lrulist.push_front(cacheKey);
    

    //inserting into cache
    cache[cacheKey] = make_pair(
        CacheEntry{
            fullResponse,
            time(nullptr),              // timestamp
            fullResponse.size()          // size
        },
        lrulist.begin()
    );

    currentCacheSize += fullResponse.size();
    }

    closesocket(remoteSocket);
    closesocket(clientSocket);
}

// Control server for admin commands (runs in separate thread)
void controlServer() {
    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        cerr << "[Control] Socket creation failed\n";
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7070);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cerr << "[Control] Bind failed\n";
        closesocket(server);
        return;
    }

    listen(server, 5);
    cout << "[Control] Listening on 127.0.0.1:7070\n";

    while (!shutdownRequested) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        char buffer[1024]{};
        int n = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            string cmd(buffer);
            cmd.erase(cmd.find_last_not_of("\r\n") + 1); // trim newline
            string response = handleControlCommand(cmd);
            send(client, response.c_str(), response.size(), 0);
        }
        closesocket(client);
    }
    closesocket(server);
}

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
   
   // Start control server thread
   thread controlThread(controlServer);

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

    controlThread.join();
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}




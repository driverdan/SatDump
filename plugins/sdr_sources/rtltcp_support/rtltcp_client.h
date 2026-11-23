#pragma once
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <string>
#include <functional>
#include <mutex>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <unistd.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#endif

// The commands are sent with send() and MSG_NOSIGNAL, so that writing to a connection the
// server closed returns an error instead of raising SIGPIPE, which would end the program.
// macOS has no MSG_NOSIGNAL, SO_NOSIGPIPE is set on the socket there instead.
#if !defined(_WIN32) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

#ifdef _WIN32
#define __attribute__(x)
#pragma pack(push, 1)
#endif
struct command_t {
    unsigned char cmd;
    unsigned int param;
} __attribute__((packed));
#ifdef _WIN32
#pragma pack(pop)
#endif

class RTLTCPClient {
public:
    RTLTCPClient() {
    }

    // With keep_going, the connection is attempted without blocking for more than
    // CONNECT_POLL_MS at a time, and given up once keep_going() returns false (on Windows,
    // connect() still blocks).
    bool connectToRTL(char* host, uint16_t port, std::function<bool()> keep_going = nullptr) {
        if (connected) {
            return true;
        }

#ifdef _WIN32
        struct addrinfo* result = NULL;
        struct addrinfo* ptr = NULL;
        struct addrinfo hints;

        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        char buf[128];
        sprintf(buf, "%hu", port);

        int iResult = getaddrinfo(host, buf, &hints, &result);
        if (iResult != 0) {
            // TODO: log error
            printf("\n%s\n", gai_strerror(iResult));
            WSACleanup();
            return false;
        }
        ptr = result;

        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);

        if (sock == INVALID_SOCKET) {
            // TODO: log error
            printf("B");
            freeaddrinfo(result);
            WSACleanup();
            return false;
        }

        iResult = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (iResult == SOCKET_ERROR) {
            printf("C");
            closesocket(sock);
            freeaddrinfo(result);
            WSACleanup();
            return false;
        }
        freeaddrinfo(result);
#else
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            // TODO: Log error
            return false;
        }
        struct hostent* server = gethostbyname(host);
        if (server == NULL) {
            connect_error = std::string("could not resolve ") + host;
            close(sockfd);
            return false;
        }
        struct sockaddr_in serv_addr;
        bzero(&serv_addr, sizeof(struct sockaddr_in));
        serv_addr.sin_family = AF_INET;
        bcopy((char*)server->h_addr, (char*)&serv_addr.sin_addr.s_addr, server->h_length);
        serv_addr.sin_port = htons(port);
        if (!connectSocket((struct sockaddr*)&serv_addr, sizeof(serv_addr), keep_going)) {
            close(sockfd);
            return false;
        }
#ifdef SO_NOSIGPIPE
        int no_sigpipe = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
#endif

        {
            std::lock_guard<std::mutex> lock(send_mtx);
            connected = true;
        }

        printf("Connected");

        return true;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(send_mtx);
        if (!connected) {
            return;
        }
#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sockfd);
#endif
        connected = false;
    }

    // struct command_t {
    //     uint8_t cmd;
    //     uint32_t arg;
    // };

    void sendCommand(uint8_t command, uint32_t param) {
        command_t cmd;
        cmd.cmd = command;
        cmd.param = htonl(param);
        // Not while the socket is being replaced, and not on a closed one, whose number may
        // already be reused by something else
        std::lock_guard<std::mutex> lock(send_mtx);
        if (!connected) {
            return;
        }
#ifdef _WIN32
        send(sock, (char*)&cmd, sizeof(command_t), 0);
#else
        (void)send(sockfd, &cmd, sizeof(command_t), MSG_NOSIGNAL);
#endif
    }

    // Fills buf with count bytes. Returns false if the server closed the connection or on a
    // read error, receiveError() then says which. The bytes received before are lost.
    bool receiveData(uint8_t* buf, size_t count) {
        int received = 0;
        int ret = 0;
        while (received < (int)count) {
#ifdef _WIN32
            ret = recv(sock, (char*)&buf[received], count - received, 0);
#else
            ret = read(sockfd, &buf[received], count - received);
            if (ret < 0 && errno == EINTR) { continue; }
#endif
            if (ret == 0) {
                receive_error = "the server closed the connection";
                return false;
            }
            if (ret < 0) {
#ifdef _WIN32
                receive_error = "receive error " + std::to_string(WSAGetLastError());
#else
                receive_error = std::string("receive error: ") + strerror(errno);
#endif
                return false;
            }
            received += ret;
        }
        return true;
    }

    // Why the last call to receiveData() failed
    std::string receiveError() const {
        return receive_error;
    }

    // Why the last call to connectToRTL() failed, if it is known
    std::string connectError() const {
        return connect_error;
    }

    void setFrequency(double freq) {
        sendCommand(1, freq);
    }

    void setSampleRate(double sr) {
        sendCommand(2, sr);
    }

    void setGainMode(int mode) {
        sendCommand(3, mode);
    }

    void setGain(double gain) {
        sendCommand(4, gain);
    }

    void setPPM(int ppm) {
        sendCommand(5, (uint32_t)ppm);
    }

    void setAGCMode(int mode) {
        sendCommand(8, mode);
    }

    void setDirectSampling(int mode) {
        sendCommand(9, mode);
    }

    void setOffsetTuning(bool enabled) {
        sendCommand(10, enabled);
    }

    void setGainIndex(int index) {
        sendCommand(13, index);
    }

    void setBiasTee(bool enabled) {
        sendCommand(14, enabled);
    }

private:
#ifndef _WIN32
    static const int CONNECT_POLL_MS = 50;

    bool connectSocket(struct sockaddr* addr, socklen_t len, std::function<bool()>& keep_going) {
        connect_error.clear();
        if (!keep_going) {
            if (connect(sockfd, addr, len) < 0) {
                connect_error = strerror(errno);
                return false;
            }
            return true;
        }

        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
        int ret = connect(sockfd, addr, len);
        if (ret < 0 && errno != EINPROGRESS) {
            connect_error = strerror(errno);
            return false;
        }
        while (ret < 0) {
            if (!keep_going()) {
                connect_error = "cancelled";
                return false;
            }
            pollfd p{sockfd, POLLOUT, 0};
            int n = poll(&p, 1, CONNECT_POLL_MS);
            if (n < 0 && errno != EINTR) {
                connect_error = strerror(errno);
                return false;
            }
            if (n > 0) {
                int err = 0;
                socklen_t err_len = sizeof(err);
                getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &err_len);
                if (err != 0) {
                    connect_error = strerror(err);
                    return false;
                }
                ret = 0;
            }
        }
        fcntl(sockfd, F_SETFL, flags);
        return true;
    }
#endif

#ifdef _WIN32
    SOCKET sock;
#else
    int sockfd;
#endif

    bool connected = false;
    std::mutex send_mtx;
    std::string receive_error;
    std::string connect_error;
};
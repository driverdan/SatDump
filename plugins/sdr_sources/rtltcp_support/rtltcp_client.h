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
    // CONNECT_POLL_MS at a time, and given up once keep_going() returns false or after
    // timeout_ms if it is positive (on Windows, connect() still blocks). connectError() says
    // why it failed.
    bool connectToRTL(char* host, uint16_t port, std::function<bool()> keep_going = nullptr, int timeout_ms = 0) {
        if (connected) {
            return true;
        }
        connect_error.clear();

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
            connect_error = gai_strerror(iResult);
            WSACleanup();
            return false;
        }
        ptr = result;

        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);

        if (sock == INVALID_SOCKET) {
            connect_error = "socket error " + std::to_string(WSAGetLastError());
            freeaddrinfo(result);
            WSACleanup();
            return false;
        }

        iResult = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (iResult == SOCKET_ERROR) {
            connect_error = "connect error " + std::to_string(WSAGetLastError());
            closesocket(sock);
            freeaddrinfo(result);
            WSACleanup();
            return false;
        }
        freeaddrinfo(result);
#else
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            connect_error = strerror(errno);
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
        if (!connectSocket((struct sockaddr*)&serv_addr, sizeof(serv_addr), keep_going, timeout_ms)) {
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

    // Returns false if the command was dropped as there is no connection, or not sent
    bool sendCommand(uint8_t command, uint32_t param) {
        command_t cmd;
        cmd.cmd = command;
        cmd.param = htonl(param);
        // Not while the socket is being replaced, and not on a closed one, whose number may
        // already be reused by something else
        std::lock_guard<std::mutex> lock(send_mtx);
        if (!connected) {
            return false;
        }
#ifdef _WIN32
        int sent = send(sock, (char*)&cmd, sizeof(command_t), 0);
#else
        ssize_t sent = send(sockfd, &cmd, sizeof(command_t), MSG_NOSIGNAL);
#endif
        return sent == (int)sizeof(command_t);
    }

    // Fills buf with count bytes. Returns false if the server closed the connection or on a
    // read error, receiveError() then says which. The bytes received before are lost.
    // With keep_going, the data is waited for RECEIVE_POLL_MS at a time, and the wait given
    // up once keep_going() returns false: a server that stops sending without closing the
    // connection does not block the caller forever.
    bool receiveData(uint8_t* buf, size_t count, std::function<bool()> keep_going = nullptr) {
        int received = 0;
        int ret = 0;
        while (received < (int)count) {
            if (keep_going) {
                if (!keep_going()) {
                    receive_error = "cancelled";
                    return false;
                }
#ifdef _WIN32
                WSAPOLLFD p{sock, POLLRDNORM, 0};
                int n = WSAPoll(&p, 1, RECEIVE_POLL_MS);
#else
                pollfd p{sockfd, POLLIN, 0};
                int n = poll(&p, 1, RECEIVE_POLL_MS);
                if (n < 0 && errno == EINTR) { continue; }
#endif
                if (n == 0) { continue; }
                // On an error, the read below reports it
            }
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

    bool setFrequency(double freq) {
        return sendCommand(1, freq);
    }

    bool setSampleRate(double sr) {
        return sendCommand(2, sr);
    }

    bool setGainMode(int mode) {
        return sendCommand(3, mode);
    }

    bool setGain(double gain) {
        return sendCommand(4, gain);
    }

    bool setPPM(int ppm) {
        return sendCommand(5, (uint32_t)ppm);
    }

    bool setAGCMode(int mode) {
        return sendCommand(8, mode);
    }

    bool setDirectSampling(int mode) {
        return sendCommand(9, mode);
    }

    bool setOffsetTuning(bool enabled) {
        return sendCommand(10, enabled);
    }

    bool setGainIndex(int index) {
        return sendCommand(13, index);
    }

    bool setBiasTee(bool enabled) {
        return sendCommand(14, enabled);
    }

private:
#ifndef _WIN32
    static const int CONNECT_POLL_MS = 50;

    bool connectSocket(struct sockaddr* addr, socklen_t len, std::function<bool()>& keep_going, int timeout_ms) {
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
        int waited_ms = 0;
        while (ret < 0) {
            if (!keep_going()) {
                connect_error = "cancelled";
                return false;
            }
            if (timeout_ms > 0 && waited_ms >= timeout_ms) {
                connect_error = "timed out";
                return false;
            }
            waited_ms += CONNECT_POLL_MS;
            pollfd p{sockfd, POLLOUT, 0};
            int n = poll(&p, 1, CONNECT_POLL_MS);
            if (n < 0 && errno != EINTR) {
                connect_error = strerror(errno);
                return false;
            }
            if (n > 0) {
                int err = 0;
                socklen_t err_len = sizeof(err);
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0) {
                    err = errno;
                }
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

    static const int RECEIVE_POLL_MS = 50;

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
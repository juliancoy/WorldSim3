#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using NetSocket = SOCKET;
using NetSockLen = int;
using NetSSize = int;
inline constexpr NetSocket kInvalidNetSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using NetSocket = int;
using NetSockLen = socklen_t;
using NetSSize = ssize_t;
inline constexpr NetSocket kInvalidNetSocket = -1;
#endif

bool initNetworkSockets();
void shutdownNetworkSockets();
int netClose(NetSocket fd);
NetSSize netRead(NetSocket fd, char* data, size_t len);
NetSSize netRecvFrom(NetSocket fd, char* data, size_t len, sockaddr* from, NetSockLen* from_len);
NetSSize netSendTo(NetSocket fd, const char* data, size_t len, const sockaddr* to, NetSockLen to_len);
bool writeAll(NetSocket fd, const char* data, size_t len);
std::string urlDecode(const std::string& s);
std::string urlEncodeComponent(const std::string& s);

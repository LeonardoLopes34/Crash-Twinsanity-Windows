// Online co-op's transport (coopnet.h)
#include "coopnet.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Coop
{
namespace
{
#ifdef _WIN32
bool StartSockets()
{
    static bool started = false;
    static bool ok = false;
    if (!started)
    {
        started = true;
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    return ok;
}

using SocketHandle = SOCKET;
constexpr SocketHandle NoSocket = INVALID_SOCKET;
int LastError() { return WSAGetLastError(); }
void CloseSocket(SocketHandle handle) { closesocket(handle); }
#else
bool StartSockets() { return true; }
using SocketHandle = int;
constexpr SocketHandle NoSocket = -1;
int LastError() { return errno; }
void CloseSocket(SocketHandle handle) { close(handle); }
#endif

// Sequence numbers compared across their wrap
bool Before(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) < 0; }
}

double g_TestDropShare = 0;

double Now()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string Address::ToString() const
{
    uint32_t host = ntohl(ip);
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", host >> 24, (host >> 16) & 0xFF, (host >> 8) & 0xFF, host & 0xFF,
                  static_cast<unsigned>(port));
    return text;
}

bool ParseAddress(const std::string& text, Address* out, uint16_t defaultPort)
{
    if (!StartSockets() || text.empty())
    {
        return false;
    }

    std::string host = text;
    uint16_t port = defaultPort;
    size_t colon = text.rfind(':');
    if (colon != std::string::npos)
    {
        host = text.substr(0, colon);
        long value = std::strtol(text.c_str() + colon + 1, nullptr, 10);
        if (value <= 0 || value > 65535)
        {
            return false;
        }

        port = static_cast<uint16_t>(value);
    }

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr)
    {
        return false;
    }

    out->ip = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
    out->port = port;
    freeaddrinfo(found);
    return true;
}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::IsOpen() const { return handle != -1; }

bool UdpSocket::Open(uint16_t port)
{
    Close();
    if (!StartSockets())
    {
        error = "the sockets couldn't be started";
        return false;
    }

    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == NoSocket)
    {
        error = "no socket: error " + std::to_string(LastError());
        return false;
    }

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
    {
        error = "port " + std::to_string(port) + " in use or refused: error " + std::to_string(LastError());
        CloseSocket(socketHandle);
        return false;
    }

#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
    // A peer gone makes Windows report the next receive as a reset (WSAECONNRESET) unless this is turned off
    BOOL reports = FALSE;
    DWORD returned = 0;
    WSAIoctl(socketHandle, _WSAIOW(IOC_VENDOR, 12), &reports, sizeof(reports), nullptr, 0, &returned, nullptr, nullptr);
#else
    fcntl(socketHandle, F_SETFL, fcntl(socketHandle, F_GETFL, 0) | O_NONBLOCK);
#endif
    handle = static_cast<intptr_t>(socketHandle);
    return true;
}

void UdpSocket::Close()
{
    if (handle != -1)
    {
        CloseSocket(static_cast<SocketHandle>(handle));
        handle = -1;
    }
}

uint16_t UdpSocket::LocalPort() const
{
    if (handle == -1)
    {
        return 0;
    }

    sockaddr_in local = {};
    socklen_t size = sizeof(local);
    getsockname(static_cast<SocketHandle>(handle), reinterpret_cast<sockaddr*>(&local), &size);
    return ntohs(local.sin_port);
}

bool UdpSocket::Send(const Address& to, const void* data, size_t size)
{
    if (handle == -1 || size > MostPacket)
    {
        return false;
    }

    if (g_TestDropShare > 0)
    {
        static uint32_t seed = 12345;
        seed = seed * 1103515245u + 12345u;
        if (static_cast<double>((seed >> 8) & 0xFFFF) / 65536.0 < g_TestDropShare)
        {
            return true;
        }
    }

    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = to.ip;
    remote.sin_port = htons(to.port);
    int sent = sendto(static_cast<SocketHandle>(handle), static_cast<const char*>(data), static_cast<int>(size), 0,
                      reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
    return sent == static_cast<int>(size);
}

int UdpSocket::Receive(Address* from, void* data, size_t capacity)
{
    for (;;)
    {
        if (handle == -1)
        {
            return -1;
        }

        sockaddr_in remote = {};
        socklen_t size = sizeof(remote);
        int got = recvfrom(static_cast<SocketHandle>(handle), static_cast<char*>(data), static_cast<int>(capacity), 0,
                           reinterpret_cast<sockaddr*>(&remote), &size);
        if (got < 0)
        {
#ifdef _WIN32
            // A reset from an earlier send to a closed port (the peer not started yet): try the next datagram
            if (LastError() == WSAECONNRESET)
            {
                continue;
            }
#endif
            return -1;
        }

        from->ip = remote.sin_addr.s_addr;
        from->port = ntohs(remote.sin_port);
        return got;
    }
}

void Writer::U16(uint16_t value)
{
    U8(static_cast<uint8_t>(value));
    U8(static_cast<uint8_t>(value >> 8));
}

void Writer::U32(uint32_t value)
{
    U16(static_cast<uint16_t>(value));
    U16(static_cast<uint16_t>(value >> 16));
}

void Writer::F32(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    U32(bits);
}

void Writer::F64(double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    U32(static_cast<uint32_t>(bits));
    U32(static_cast<uint32_t>(bits >> 32));
}

void Writer::Bytes(const void* data, size_t size)
{
    const auto* from = static_cast<const uint8_t*>(data);
    bytes->insert(bytes->end(), from, from + size);
}

void Writer::Text(const std::string& text, size_t most)
{
    size_t size = text.size() < most ? text.size() : most;
    U8(static_cast<uint8_t>(size));
    Bytes(text.data(), size);
}

uint8_t Reader::U8()
{
    if (!ok || at >= end)
    {
        ok = false;
        return 0;
    }

    return *at++;
}

uint16_t Reader::U16()
{
    uint16_t low = U8();
    return static_cast<uint16_t>(low | (U8() << 8));
}

uint32_t Reader::U32()
{
    uint32_t low = U16();
    return low | (static_cast<uint32_t>(U16()) << 16);
}

float Reader::F32()
{
    uint32_t bits = U32();
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double Reader::F64()
{
    uint64_t low = U32();
    uint64_t bits = low | (static_cast<uint64_t>(U32()) << 32);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool Reader::Bytes(void* out, size_t size)
{
    if (!ok || static_cast<size_t>(end - at) < size)
    {
        ok = false;
        return false;
    }

    std::memcpy(out, at, size);
    at += size;
    return true;
}

std::string Reader::Text()
{
    size_t size = U8();
    std::string text(size, '\0');
    Bytes(text.data(), size);
    return ok ? text : std::string();
}

void WriteHeader(Writer* writer, PacketKind kind)
{
    writer->U32(Magic);
    writer->U16(ProtocolVersion);
    writer->U8(static_cast<uint8_t>(kind));
}

bool ReadHeader(Reader* reader, PacketKind* kind)
{
    uint32_t magic = reader->U32();
    uint16_t version = reader->U16();
    *kind = static_cast<PacketKind>(reader->U8());
    return reader->Ok() && magic == Magic && version == ProtocolVersion;
}

void ReliableChannel::Reset()
{
    nextNumber = 0;
    expected = 0;
    outgoing.clear();
    early.clear();
}

bool ReliableChannel::Queue(const std::vector<uint8_t>& message)
{
    if (outgoing.size() >= MostInFlight || message.size() > MostPacket - 16)
    {
        return false;
    }

    outgoing.push_back({nextNumber++, message, 0.0, false});
    return true;
}

void ReliableChannel::Due(double now, std::vector<std::vector<uint8_t>>* packets)
{
    for (Outgoing& message : outgoing)
    {
        if (message.sent && now - message.sentAt < ResendSeconds)
        {
            continue;
        }

        std::vector<uint8_t> packet;
        Writer writer(&packet);
        WriteHeader(&writer, PacketKind::Reliable);
        writer.U16(message.number);
        writer.Bytes(message.message.data(), message.message.size());
        packets->push_back(std::move(packet));
        message.sent = true;
        message.sentAt = now;
    }
}

void ReliableChannel::Received(uint16_t number, const uint8_t* data, size_t size, std::vector<uint8_t>* ack,
                               std::vector<std::vector<uint8_t>>* delivered)
{
    // Acknowledged every time (the first acknowledgement may have been lost)
    ack->clear();
    Writer writer(ack);
    WriteHeader(&writer, PacketKind::Ack);
    writer.U16(number);
    if (Before(number, expected))
    {
        return;
    }

    if (number != expected)
    {
        // Early: kept until the ones before it come (a window of MostInFlight)
        if (static_cast<uint16_t>(number - expected) < MostInFlight)
        {
            early.emplace(number, std::vector<uint8_t>(data, data + size));
        }

        return;
    }

    delivered->emplace_back(data, data + size);
    expected++;
    for (auto found = early.find(expected); found != early.end(); found = early.find(expected))
    {
        delivered->push_back(std::move(found->second));
        early.erase(found);
        expected++;
    }
}

void ReliableChannel::Acknowledged(uint16_t number)
{
    for (auto message = outgoing.begin(); message != outgoing.end(); ++message)
    {
        if (message->number == number)
        {
            outgoing.erase(message);
            return;
        }
    }
}
}

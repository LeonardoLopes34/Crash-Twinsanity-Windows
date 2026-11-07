#pragma once

// Online co-op's transport (COOP.md): one UDP socket, two peers. State packets go unreliable (the newest wins); events go on a
// reliable, ordered channel (resent until acknowledged). No game headers here: the co-op tests build it on its own
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace Coop
{
constexpr uint32_t Magic = 0x4F435754; // "TWCO"
constexpr uint16_t ProtocolVersion = 2;
constexpr uint16_t DefaultPort = 27420;
constexpr size_t MostPacket = 1200;

// An address as the socket layer keeps it (IPv4, network order)
struct Address
{
    uint32_t ip = 0;
    uint16_t port = 0;
    bool operator==(const Address& other) const { return ip == other.ip && port == other.port; }
    bool operator!=(const Address& other) const { return !(*this == other); }
    bool Valid() const { return ip != 0 && port != 0; }
    std::string ToString() const;
};

// "192.168.0.10", "26.12.34.56:27420", "localhost": false when it can't be read or looked up
bool ParseAddress(const std::string& text, Address* out, uint16_t defaultPort = DefaultPort);

class UdpSocket
{
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Bound to the port on every interface (0: any free port), non-blocking
    bool Open(uint16_t port);
    void Close();
    bool IsOpen() const;
    uint16_t LocalPort() const;
    bool Send(const Address& to, const void* data, size_t size);
    // A datagram waiting, or -1 when none
    int Receive(Address* from, void* data, size_t capacity);
    const std::string& Error() const { return error; }

private:
    intptr_t handle = -1;
    std::string error;
};

// The packets' kinds
enum class PacketKind : uint8_t
{
    Hello = 1,   // client to host, until welcomed
    Welcome = 2, // host to client
    State = 3,   // unreliable: the player's character as it is
    Reliable = 4,
    Ack = 5,
    Ping = 6,
    Pong = 7,
    Bye = 8,
    Pose = 9,    // unreliable: the character's joints (the newest wins)
};

// Little-endian writing and reading of a packet's fields
class Writer
{
public:
    explicit Writer(std::vector<uint8_t>* out) : bytes(out) {}
    void U8(uint8_t value) { bytes->push_back(value); }
    void U16(uint16_t value);
    void U32(uint32_t value);
    void F32(float value);
    void Bytes(const void* data, size_t size);
    void Text(const std::string& text, size_t most);

private:
    std::vector<uint8_t>* bytes;
};

class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : at(data), end(data + size) {}
    uint8_t U8();
    uint16_t U16();
    uint32_t U32();
    float F32();
    bool Bytes(void* out, size_t size);
    std::string Text();
    bool Ok() const { return ok; }
    size_t Left() const { return ok ? static_cast<size_t>(end - at) : 0; }
    const uint8_t* Here() const { return at; }

private:
    const uint8_t* at;
    const uint8_t* end;
    bool ok = true;
};

// The reliable, ordered channel to one peer: messages numbered, resent every ResendSeconds until acknowledged, delivered once and
// in order
class ReliableChannel
{
public:
    static constexpr double ResendSeconds = 0.15;
    static constexpr size_t MostInFlight = 256;

    void Reset();
    // Queues a message (its bytes, a payload of the Reliable packet): false when too many are unacknowledged
    bool Queue(const std::vector<uint8_t>& message);
    // The packets to send now (new ones and those due again)
    void Due(double now, std::vector<std::vector<uint8_t>>* packets);
    // A Reliable packet's number and message arrived: its Ack packet to send back, and the messages now in order to deliver
    void Received(uint16_t number, const uint8_t* data, size_t size, std::vector<uint8_t>* ack,
                  std::vector<std::vector<uint8_t>>* delivered);
    void Acknowledged(uint16_t number);
    size_t InFlight() const { return outgoing.size(); }

private:
    struct Outgoing
    {
        uint16_t number;
        std::vector<uint8_t> message;
        double sentAt;
        bool sent;
    };

    uint16_t nextNumber = 0;
    uint16_t expected = 0;
    std::deque<Outgoing> outgoing;
    std::map<uint16_t, std::vector<uint8_t>> early;
};

// A packet's header: magic, version, kind
void WriteHeader(Writer* writer, PacketKind kind);
bool ReadHeader(Reader* reader, PacketKind* kind);

// Seconds from a steady clock
double Now();

// Tests: the share of sent datagrams dropped on purpose (0 in the game)
extern double g_TestDropShare;
}

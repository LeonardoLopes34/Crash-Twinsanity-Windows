// Online co-op's self-test (native/coop-tests): a host and a client in one process over the loopback, playing out the session's
// rules: the link, the partner's movement, the cutscene wait, the backpack, the pull, a bad network and leaving
#include "coop/coopsession.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

using namespace Coop;

namespace
{
int g_Failures = 0;
int g_Checks = 0;
double g_Time = 100;

void Check(bool ok, const char* what)
{
    g_Checks++;
    if (!ok)
    {
        g_Failures++;
    }

    std::printf("  [%s] %s\n", ok ? " ok " : "FALHOU", what);
}

struct Pair
{
    Session host;
    Session client;
    PlayerState hostPlayer;
    PlayerState clientPlayer;

    bool Start(uint16_t port, Settings base = Settings())
    {
        Settings hostSettings = base;
        hostSettings.role = Role::Host;
        hostSettings.port = port;
        hostSettings.name = "Host";
        Settings clientSettings = base;
        clientSettings.role = Role::Client;
        clientSettings.port = port;
        clientSettings.join = "127.0.0.1:" + std::to_string(port);
        clientSettings.name = "Amigo";
        clientSettings.skin = 1;
        return host.Start(hostSettings) && client.Start(clientSettings);
    }

    // One frame (1/60 s of the sessions' time); the loopback given a moment to carry the datagrams
    void Step()
    {
        g_Time += 1.0 / 60.0;
        host.Update(g_Time, hostPlayer);
        client.Update(g_Time, clientPlayer);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }

    // Frames until a condition holds (or the most), how many it took (-1: never)
    int Until(const std::function<bool()>& condition, int most = 600)
    {
        for (int frame = 0; frame < most; frame++)
        {
            if (condition())
            {
                return frame;
            }

            Step();
        }

        return condition() ? most : -1;
    }
};

void TestLink()
{
    std::printf("Conexão\n");
    Pair pair;
    Check(pair.Start(37421), "host e cliente abrem as portas");
    int frames = pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    Check(frames >= 0, "o cliente conecta ao host");
    Check(pair.host.PartnerName() == "Amigo" && pair.client.PartnerName() == "Host", "cada um sabe o nome do outro");
    Check(pair.host.PartnerSkin() == 1, "o host sabe que o parceiro é o Carbon Crash");
}

void TestMovement()
{
    std::printf("Movimento do parceiro\n");
    Pair pair;
    pair.Start(37422);
    pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    // The client runs along x at 10 units a second
    double startTime = g_Time;
    for (int frame = 0; frame < 120; frame++)
    {
        pair.clientPlayer.position[0] = static_cast<float>((g_Time - startTime) * 10.0);
        pair.clientPlayer.yaw = 1.0f;
        pair.clientPlayer.flags = PlayerState::Visible | PlayerState::Grounded;
        pair.Step();
    }

    PlayerState seen;
    bool have = pair.host.Partner(g_Time, &seen);
    float expected = static_cast<float>((g_Time - startTime - 0.1) * 10.0);
    Check(have, "o host recebe o estado do parceiro");
    Check(std::fabs(seen.position[0] - expected) < 1.0f, "a posição interpolada fica perto da real (100 ms atrás)");
    Check(seen.Has(PlayerState::Visible) && std::fabs(seen.yaw - 1.0f) < 0.01f, "rotação e estado chegam");

    // Smooth: sampled every frame, the interpolated x never steps back and never jumps more than two frames' worth
    float last = -1e9f;
    bool smooth = true;
    for (int frame = 0; frame < 120; frame++)
    {
        pair.clientPlayer.position[0] = static_cast<float>((g_Time - startTime) * 10.0);
        pair.Step();
        PlayerState now;
        pair.host.Partner(g_Time, &now);
        if (last > -1e8f && (now.position[0] < last - 0.001f || now.position[0] - last > 2 * 10.0f / 60.0f + 0.05f))
        {
            smooth = false;
        }

        last = now.position[0];
    }

    Check(smooth, "o movimento interpolado é liso (sem voltar nem pular) a 60 quadros por segundo");

    // The pose: two around the moment drawn, and a share between them
    for (int frame = 0; frame < 30; frame++)
    {
        pair.client.SetLocalPose(std::vector<uint8_t>(64, static_cast<uint8_t>(frame)));
        pair.Step();
    }

    const std::vector<uint8_t>* from = nullptr;
    const std::vector<uint8_t>* to = nullptr;
    float share = -1;
    bool havePoses = pair.host.PartnerPoses(g_Time, &from, &to, &share) && from != nullptr && to != nullptr;
    Check(havePoses && from != to && (*from)[0] + 1 == (*to)[0] && share >= 0.0f && share <= 1.0f,
          "a pose vem em pares consecutivos para ser interpolada");
}

void TestCutsceneBothArrive()
{
    std::printf("Cutscene: os dois chegam\n");
    Pair pair;
    pair.Start(37423);
    pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    const uint32_t key = HashKey("cutscene da praia");

    // The host reaches it first and waits two seconds
    bool hostGo = false;
    for (int frame = 0; frame < 120; frame++)
    {
        hostGo = pair.host.AskCutscene(key, g_Time) == Session::Gate::Go;
        pair.Step();
    }

    Check(!hostGo, "o host fica esperando enquanto o parceiro não chega");
    Check(pair.host.CutsceneStatus(g_Time).holding, "o host mostra \"aguardando o Jogador 2\"");
    Check(pair.client.CutsceneStatus(g_Time).partnerWaitsForMe, "o cliente é avisado de que o host está esperando");

    // The client arrives
    bool clientGo = false;
    int hostGoFrame = -1;
    int clientGoFrame = -1;
    for (int frame = 0; frame < 120 && (hostGoFrame < 0 || clientGoFrame < 0); frame++)
    {
        if (hostGoFrame < 0 && pair.host.AskCutscene(key, g_Time) == Session::Gate::Go)
        {
            hostGoFrame = frame;
        }

        if (clientGoFrame < 0 && (clientGo = pair.client.AskCutscene(key, g_Time) == Session::Gate::Go))
        {
            clientGoFrame = frame;
        }

        pair.Step();
    }

    Check(hostGoFrame >= 0 && clientGoFrame >= 0, "a cutscene começa nos dois");
    Check(std::abs(hostGoFrame - clientGoFrame) <= 3, "e começa junto (até 3 frames de diferença)");
    Check(!pair.host.CutsceneStatus(g_Time).holding && !pair.client.CutsceneStatus(g_Time).holding,
          "ninguém fica preso esperando depois");
}

void TestCutsceneWalkOff()
{
    std::printf("Cutscene: o jogador sai da área\n");
    Pair pair;
    pair.Start(37424);
    pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    const uint32_t key = HashKey("cutscene da caverna");
    for (int frame = 0; frame < 30; frame++)
    {
        pair.client.AskCutscene(key, g_Time);
        pair.Step();
    }

    Check(pair.host.CutsceneStatus(g_Time).partnerWaitsForMe, "o host vê o cliente esperando");
    // The client walks off: the script stops asking
    pair.Until([&] { return !pair.host.CutsceneStatus(g_Time).partnerWaitsForMe; }, 120);
    Check(!pair.host.CutsceneStatus(g_Time).partnerWaitsForMe, "o aviso some quando o cliente sai da área");
    // Now the host alone at it: it must still wait (the client isn't there any more)
    bool go = false;
    for (int frame = 0; frame < 30; frame++)
    {
        go = go || pair.host.AskCutscene(key, g_Time) == Session::Gate::Go;
        pair.Step();
    }

    Check(!go, "o host não começa sozinho a cutscene");
}

void TestBackpack()
{
    std::printf("Mochila\n");
    Pair pair;
    pair.Start(37425);
    pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    Check(pair.client.EnterBackpack(), "o cliente entra na mochila");
    int frames = pair.Until([&] { return pair.host.PartnerInBackpack(); }, 60);
    Check(frames >= 0, "o host vê o parceiro na mochila");
    Check(!pair.host.EnterBackpack(), "os dois não podem estar na mochila ao mesmo tempo");

    // A cutscene with the partner in the backpack: no wait
    const uint32_t key = HashKey("cutscene com mochila");
    Check(pair.host.AskCutscene(key, g_Time) == Session::Gate::Go, "com o parceiro na mochila a cutscene começa na hora");
    bool clientGo = false;
    for (int frame = 0; frame < 60 && !clientGo; frame++)
    {
        pair.Step();
        clientGo = pair.client.AskCutscene(key, g_Time) == Session::Gate::Go;
    }

    Check(clientGo, "e começa no PC de quem está na mochila também");

    pair.client.LeaveBackpack();
    pair.Until([&] { return !pair.host.PartnerInBackpack(); }, 60);
    Check(!pair.host.PartnerInBackpack(), "o cliente sai da mochila");

    // Both at the same frame: the host keeps it
    pair.host.EnterBackpack();
    pair.client.EnterBackpack();
    pair.Until([&] { return !pair.client.InBackpack(); }, 60);
    pair.Until([&] { return pair.client.PartnerInBackpack(); }, 60);
    Check(pair.host.InBackpack() && !pair.client.InBackpack(), "os dois ao mesmo tempo: fica só o host");
}

void TestPull()
{
    std::printf("Puxar o parceiro\n");
    Settings settings;
    settings.pullOfferSeconds = 1;
    Pair pair;
    pair.Start(37426, settings);
    pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    const uint32_t key = HashKey("cutscene longe");
    for (int frame = 0; frame < 70; frame++)
    {
        pair.host.AskCutscene(key, g_Time);
        pair.Step();
    }

    Check(pair.host.CutsceneStatus(g_Time).canPull, "depois de esperar, o host pode puxar o parceiro");
    pair.host.PullPartner();
    bool pulled = false;
    for (int frame = 0; frame < 60 && !pulled; frame++)
    {
        pair.host.AskCutscene(key, g_Time);
        pair.Step();
        pulled = pair.client.TakePulled();
    }

    Check(pulled, "o cliente recebe o pedido e é puxado");
}

void TestLeash()
{
    std::printf("Coleira (distância máxima)\n");
    Settings settings;
    settings.leashDistance = 50;
    settings.leashSeconds = 1;
    Pair pair;
    pair.Start(37427, settings);
    pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    pair.clientPlayer.position[0] = 10;
    for (int frame = 0; frame < 90; frame++)
    {
        pair.Step();
    }

    Check(!pair.client.LeashWantsBackpack(g_Time, pair.clientPlayer), "perto do host: nada acontece");
    pair.clientPlayer.position[0] = 200;
    bool wants = false;
    for (int frame = 0; frame < 120 && !wants; frame++)
    {
        pair.Step();
        wants = pair.client.LeashWantsBackpack(g_Time, pair.clientPlayer);
    }

    Check(wants, "longe demais por 1 s: o cliente vai para a mochila");
}

void TestBadNetwork()
{
    std::printf("Rede ruim (30%% dos pacotes perdidos)\n");
    Pair pair;
    pair.Start(37428);
    g_TestDropShare = 0.3;
    int frames = pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    Check(frames >= 0, "conecta mesmo perdendo pacotes");
    const uint32_t key = HashKey("cutscene com lag");
    int hostGo = -1;
    int clientGo = -1;
    for (int frame = 0; frame < 600 && (hostGo < 0 || clientGo < 0); frame++)
    {
        if (hostGo < 0 && pair.host.AskCutscene(key, g_Time) == Session::Gate::Go)
        {
            hostGo = frame;
        }

        if (clientGo < 0 && pair.client.AskCutscene(key, g_Time) == Session::Gate::Go)
        {
            clientGo = frame;
        }

        pair.Step();
    }

    Check(hostGo >= 0 && clientGo >= 0, "a cutscene começa nos dois mesmo assim");
    for (int index = 0; index < 20; index++)
    {
        WorldEvent event;
        event.kind = 1;
        event.area = 77;
        event.id = static_cast<uint32_t>(index);
        event.position[1] = static_cast<float>(index) * 0.5f;
        pair.host.SendWorld(event);
    }

    int got = 0;
    bool ordered = true;
    for (int frame = 0; frame < 600 && got < 20; frame++)
    {
        pair.Step();
        WorldEvent event;
        while (pair.client.PopWorld(&event))
        {
            ordered = ordered && event.id == static_cast<uint32_t>(got) && event.area == 77 &&
                      event.position[1] == static_cast<float>(got) * 0.5f;
            got++;
        }
    }

    Check(got == 20 && ordered, "20 eventos do mundo chegam todos, uma vez cada, na ordem");
    g_TestDropShare = 0;
}

void TestLeaving()
{
    std::printf("Saída do jogo\n");
    Settings settings;
    settings.timeoutSeconds = 1;
    Pair pair;
    pair.Start(37429, settings);
    pair.Until([&] { return pair.host.Connected() && pair.client.Connected(); });
    pair.client.EnterBackpack();
    pair.Until([&] { return pair.host.PartnerInBackpack(); }, 60);
    pair.client.Stop();
    pair.Until([&] { return !pair.host.Connected(); }, 60);
    Check(!pair.host.Connected(), "o host percebe na hora que o cliente saiu");
    const uint32_t key = HashKey("cutscene sozinho");
    Check(pair.host.AskCutscene(key, g_Time) == Session::Gate::Go, "sozinho, a cutscene não espera ninguém");

    // A client that vanishes (no goodbye): the timeout
    Pair other;
    other.Start(37430, settings);
    other.Until([&] { return other.host.Connected() && other.client.Connected(); });
    for (int frame = 0; frame < 120 && other.host.Connected(); frame++)
    {
        g_Time += 1.0 / 60.0;
        other.host.Update(g_Time, other.hostPlayer);
    }

    Check(!other.host.Connected(), "um cliente que some sem avisar cai depois do tempo limite");
    // ...and comes back
    int frames = other.Until([&] { return other.host.Connected() && other.client.Connected(); });
    Check(frames >= 0, "e consegue reconectar");
}
}

int main()
{
    std::printf("Crash Twinsanity co-op: teste da sessão\n\n");
    TestLink();
    TestMovement();
    TestCutsceneBothArrive();
    TestCutsceneWalkOff();
    TestBackpack();
    TestPull();
    TestLeash();
    TestBadNetwork();
    TestLeaving();
    std::printf("\n%d de %d verificações passaram\n", g_Checks - g_Failures, g_Checks);
    return g_Failures == 0 ? 0 : 1;
}

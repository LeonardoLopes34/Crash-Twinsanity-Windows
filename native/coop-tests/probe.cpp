// Online co-op's connection probe (native/coop-tests): the session without the game, to check two PCs reach each other (Radmin VPN,
// Hamachi, a LAN or a forwarded port) before playing. Each side "walks" in a circle and prints the partner it sees
//   coopprobe host [port]
//   coopprobe join <address[:port]> [name]
#include "coop/coopsession.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace Coop;

int main(int count, char** arguments)
{
    if (count < 2 || (std::strcmp(arguments[1], "host") != 0 && std::strcmp(arguments[1], "join") != 0) ||
        (std::strcmp(arguments[1], "join") == 0 && count < 3))
    {
        std::printf("Teste de conexão do co-op do Crash Twinsanity\n\n"
                    "  coopprobe host [porta]                 (no PC de quem hospeda; porta padrão %u)\n"
                    "  coopprobe join <ip[:porta]> [nome]     (no PC do amigo; o IP do host no Radmin VPN)\n\n"
                    "Deixe rodando nos dois PCs: aparece o ping e a posição que cada um vê do outro.\n",
                    DefaultPort);
        return 2;
    }

    Settings settings;
    bool host = std::strcmp(arguments[1], "host") == 0;
    settings.role = host ? Role::Host : Role::Client;
    if (host && count >= 3)
    {
        settings.port = static_cast<uint16_t>(std::atoi(arguments[2]));
    }

    if (!host)
    {
        settings.join = arguments[2];
        settings.name = count >= 4 ? arguments[3] : "Amigo";
        settings.skin = 1;
    }
    else
    {
        settings.name = "Host";
    }

    Session session;
    if (!session.Start(settings))
    {
        std::printf("Erro: %s\n", session.Error().c_str());
        return 1;
    }

    std::printf(host ? "Hospedando na porta %u. Libere o programa no Firewall do Windows se ele perguntar.\n"
                     : "Conectando...\n",
                settings.port);
    size_t shown = 0;
    double lastPrint = 0;
    PlayerState local;
    local.flags = PlayerState::Visible | PlayerState::Grounded;
    for (;;)
    {
        double now = Now();
        local.position[0] = static_cast<float>(std::cos(now) * 5.0);
        local.position[2] = static_cast<float>(std::sin(now) * 5.0);
        local.yaw = static_cast<float>(now);
        session.Update(now, local);

        // The notices, each once
        const auto& notices = session.Notices();
        if (notices.size() < shown)
        {
            shown = 0;
        }

        for (size_t index = shown; index < notices.size(); index++)
        {
            std::printf("  > %s\n", notices[index].text.c_str());
        }

        shown = notices.size();
        if (session.GetLink() == Session::Link::Failed)
        {
            std::printf("Erro: %s\n", session.Error().c_str());
            return 1;
        }

        if (now - lastPrint >= 1.0)
        {
            lastPrint = now;
            PlayerState partner;
            if (session.Connected() && session.Partner(now, &partner))
            {
                std::printf("conectado a %-12s ping %5.0f ms   parceiro em (%6.2f, %6.2f)\n", session.PartnerName().c_str(),
                            session.RoundTrip() * 1000.0, partner.position[0], partner.position[2]);
            }
            else if (!session.Connected())
            {
                std::printf(host ? "aguardando o amigo...\n" : "sem resposta do host ainda...\n");
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

#include <cgame_mp/cg_local_mp.h>
#include <cgame_mp/cg_public_mp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
cg_s cgArray[1];
void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("Scoreboard assertion"); }

int main() {
    static_assert(sizeof(score_t) == (sizeof(void*) == 8 ? 48 : 40));
    if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(cgArray) > UINT32_MAX);
    auto& game = cgArray[0];
    game.numScores = MAX_CLIENTS;
    std::memset(game.teamScores, 0xa5, sizeof(game.teamScores));
    for (int index = 0; index < MAX_CLIENTS; ++index) {
        auto& score = game.scores[index];
        score.client = (index * 17) % MAX_CLIENTS;
        score.score = 1000 - index;
        score.hStatusIcon = reinterpret_cast<Material*>(sizeof(void*) == 8 ? UINT64_C(0x123456780000) + index * 256 : 0x12340000 + index * 256);
        score.hRankIcon = reinterpret_cast<Material*>(sizeof(void*) == 8 ? UINT64_C(0x234567890000) + index * 256 : 0x23450000 + index * 256);
        game.bgs.clientinfo[score.client].infoValid = true;
    }
    for (int rank = 1; rank <= MAX_CLIENTS; ++rank) {
        const auto* expected = &game.scores[rank - 1];
        CHECK(UI_GetScoreAtRank(0, rank) == expected);
        CHECK(GetClientScore(0, expected->client) == expected);
        CHECK(UI_GetScoreAtRank(0, rank)->hStatusIcon == expected->hStatusIcon);
        game.clientNum = expected->client;
        CHECK(UI_GetOurClientScore(0) == expected);
    }
    CHECK(!UI_GetScoreAtRank(0, 0) && !UI_GetScoreAtRank(0, -1) && !UI_GetScoreAtRank(0, MAX_CLIENTS + 1));
    game.bgs.clientinfo[game.scores[5].client].infoValid = false;
    CHECK(!UI_GetScoreAtRank(0, 6) && !GetClientScore(0, game.scores[5].client));
    game.scores[6].client = -1; game.scores[7].client = MAX_CLIENTS;
    CHECK(!UI_GetScoreAtRank(0, 7) && !UI_GetScoreAtRank(0, 8));
    CHECK(!GetClientScore(0, -1) && !GetClientScore(0, MAX_CLIENTS));
    game.numScores = MAX_CLIENTS + 100;
    CHECK(!UI_GetScoreAtRank(0, MAX_CLIENTS + 1) && !GetClientScore(0, MAX_CLIENTS + 100));
    game.numScores = 0;
    CHECK(!UI_GetScoreAtRank(0, 1) && !GetClientScore(0, game.scores[0].client));
    std::printf("Production scoreboard: %zu-bit records, all ranks, native icons and invalid client bounds passed\n", sizeof(void*) * 8);
}

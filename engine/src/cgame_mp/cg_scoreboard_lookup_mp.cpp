#include "cg_local_mp.h"
#include "cg_public_mp.h"

const score_t *__cdecl UI_GetOurClientScore(int32_t localClientNum)
{
    return GetClientScore(localClientNum, CG_GetLocalClientGlobals(localClientNum)->clientNum);
}

const score_t *__cdecl GetClientScore(int32_t localClientNum, int32_t clientNum)
{
    int32_t scoreNum; // [esp+4h] [ebp-8h]
    cg_s *cgameGlob;

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    for (scoreNum = 0; scoreNum < cgameGlob->numScores && scoreNum < MAX_CLIENTS; ++scoreNum)
    {
        if (cgameGlob->scores[scoreNum].client >= 0
            && cgameGlob->scores[scoreNum].client < MAX_CLIENTS
            && cgameGlob->bgs.clientinfo[cgameGlob->scores[scoreNum].client].infoValid
            && cgameGlob->scores[scoreNum].client == clientNum)
        {
            return &cgameGlob->scores[scoreNum];
        }
    }
    return 0;
}

const score_t *__cdecl UI_GetScoreAtRank(int32_t localClientNum, int32_t rank)
{
    cg_s *cgameGlob;

    cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    if (rank < 1 || rank > cgameGlob->numScores || rank > MAX_CLIENTS)
        return 0;

    const score_t *score = &cgameGlob->scores[rank - 1];
    if (score->client >= 0 && score->client < MAX_CLIENTS && cgameGlob->bgs.clientinfo[score->client].infoValid)
        return score;

    return 0;
}

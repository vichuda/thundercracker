#include "ScoredCubeState_EndOfRound.h"

#include <sifteo.h>
#include "EventID.h"
#include "EventData.h"
#include "assets.gen.h"
#include "CubeStateMachine.h"
#include "GameStateMachine.h"
#include "ScoredCubeState_EndOfRound.h"
#include "SavedData.h"
#include "WordGame.h"

unsigned ScoredCubeState_EndOfRound::onEvent(unsigned eventID, const EventData& data)
{
    switch (eventID)
    {
    case EventID_EnterState:
    case EventID_Paint:
        paint();
        break;

    case EventID_GameStateChanged:
        switch (data.mGameStateChanged.mNewStateIndex)
        {
        case GameStateIndex_StartOfRoundScored:
            return CubeStateIndex_StartOfRoundScored;
        }
        break;
    }
    return getStateMachine().getCurrentStateIndex();
}

unsigned ScoredCubeState_EndOfRound::update(float dt, float stateTime)
{
    return getStateMachine().getCurrentStateIndex();
}

void ScoredCubeState_EndOfRound::paint()
{
    Cube& c = getStateMachine().getCube();
    // FIXME vertical words
    bool neighbored =
            (c.physicalNeighborAt(SIDE_LEFT) != CUBE_ID_UNDEFINED ||
            c.physicalNeighborAt(SIDE_RIGHT) != CUBE_ID_UNDEFINED);
    VidMode_BG0_SPR_BG1 vid(c.vbuf);
    vid.init();
    WordGame::hideSprites(vid);
    if (GameStateMachine::getTime() <= TEETH_ANIM_LENGTH)
    {
        paintLetters(vid, Font1Letter);
        paintTeeth(vid, ImageIndex_Teeth, true, true);
        return;
    }

    switch (getStateMachine().getCube().id())
    {
    default:
        // paint "Score" asset
        vid.BG0_drawAsset(Vec2(0,0), Score);
        char string[17];
        sprintf(string, "%.5d", GameStateMachine::getScore());
        paintScoreNumbers(vid, Vec2(7,3), string);
        break;

    case 1:
        vid.BG0_drawAsset(Vec2(0,0), StartScreen);
        break;

    case 0:
        // paint "high scores" asset
        vid.BG0_drawAsset(Vec2(0,0), HighScores);
        for (unsigned i = arraysize(SavedData::sHighScores) - 1;
             i >= 0;
             --i)
        {
            if (SavedData::sHighScores[i] == 0)
            {
                break;
            }
            char string[17];
            sprintf(string, "%.5d", SavedData::sHighScores[i]);
            paintScoreNumbers(vid, Vec2(7,3 + (arraysize(SavedData::sHighScores) - i) * 2),
                         string);
        }
        break;
    }
}

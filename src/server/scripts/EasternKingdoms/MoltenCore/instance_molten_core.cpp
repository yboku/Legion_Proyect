/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2006-2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* ScriptData
SDName: Instance_Molten_Core
SD%Complete: 0
SDComment: Place Holder
SDCategory: Molten Core
EndScriptData */

#include "ObjectMgr.h"
#include "ScriptMgr.h"
#include "InstanceScript.h"
#include "CreatureAI.h"
#include "molten_core.h"

Position const SummonPositions[10] =
{
    {737.850f, -1145.35f, -120.288f, 4.71368f},
    {744.162f, -1151.63f, -119.726f, 4.58204f},
    {751.247f, -1152.82f, -119.744f, 4.49673f},
    {759.206f, -1155.09f, -120.051f, 4.30104f},
    {755.973f, -1152.33f, -120.029f, 4.25588f},
    {731.712f, -1147.56f, -120.195f, 4.95955f},
    {726.499f, -1149.80f, -120.156f, 5.24055f},
    {722.408f, -1152.41f, -120.029f, 5.33087f},
    {718.994f, -1156.36f, -119.805f, 5.75738f},
    {838.510f, -829.840f, -232.000f, 2.00000f},
};

class instance_molten_core : public InstanceMapScript
{
    public:
        instance_molten_core() : InstanceMapScript("instance_molten_core", 409) { }

        struct instance_molten_core_InstanceMapScript : public InstanceScript
        {
            instance_molten_core_InstanceMapScript(Map* map) : InstanceScript(map)
            {
                SetBossNumber(MAX_ENCOUNTER);
                _golemaggTheIncineratorGUID.Clear();
                _majordomoExecutusGUID.Clear();
                _cacheOfTheFirelordGUID.Clear();
                _executusSchedule = false;
                _deadBossCount = 0;
                _ragnarosAddDeaths = 0;
                _isLoading = false;
                _summonedExecutus = false;
            }

            void OnPlayerEnter(Player* /*player*/) override
            {
                if (_executusSchedule)
                {
                    SummonMajordomoExecutus(_executusSchedule);
                    _executusSchedule = false;
                }
            }

            void OnCreatureCreate(Creature* creature) override
            {
                switch (creature->GetEntry())
                {
                    case NPC_GOLEMAGG_THE_INCINERATOR:
                        _golemaggTheIncineratorGUID = creature->GetGUID();
                        break;
                    case NPC_MAJORDOMO_EXECUTUS:
                        _majordomoExecutusGUID = creature->GetGUID();
                        break;
                    default:
                        break;
                }
            }

            void OnGameObjectCreate(GameObject* go) override
            {
                switch (go->GetEntry())
                {
                    case GO_CACHE_OF_THE_FIRELORD:
                        _cacheOfTheFirelordGUID = go->GetGUID();
                        break;
                    default:
                        break;
                }
            }

            void SetData(uint32 type, uint32 data) override
            {
                if (type == DATA_RAGNAROS_ADDS)
                {
                    if (data == 1)
                        ++_ragnarosAddDeaths;
                    else if (data == 0)
                        _ragnarosAddDeaths = 0;
                }
            }

            uint32 GetData(uint32 type) const override
            {
                switch (type)
                {
                    case DATA_RAGNAROS_ADDS:
                        return _ragnarosAddDeaths;
                }

                return 0;
            }

            ObjectGuid GetGuidData(uint32 type) const override
            {
                switch (type)
                {
                    case BOSS_GOLEMAGG_THE_INCINERATOR:
                        return _golemaggTheIncineratorGUID;
                    case BOSS_MAJORDOMO_EXECUTUS:
                        return _majordomoExecutusGUID;
                }

                return ObjectGuid::Empty;
            }

            bool SetBossState(uint32 bossId, EncounterState state) override
            {
                if (!InstanceScript::SetBossState(bossId, state))
                    return false;

                if (state == DONE && bossId < BOSS_MAJORDOMO_EXECUTUS)
                    ++_deadBossCount;

                if (_isLoading)
                    return true;

                if (_deadBossCount == 8)
                    SummonMajordomoExecutus(false);

                if (bossId == BOSS_MAJORDOMO_EXECUTUS && state == DONE)
                    DoRespawnGameObject(_cacheOfTheFirelordGUID, 7 * DAY);

                return true;
            }

            void SummonMajordomoExecutus(bool done)
            {
                if (_summonedExecutus)
                    return;

                _summonedExecutus = true;
                if (!done)
                {
                    instance->SummonCreature(NPC_MAJORDOMO_EXECUTUS, SummonPositions[0]);
                    instance->SummonCreature(NPC_FLAMEWAKER_HEALER, SummonPositions[1]);
                    instance->SummonCreature(NPC_FLAMEWAKER_HEALER, SummonPositions[2]);
                    instance->SummonCreature(NPC_FLAMEWAKER_HEALER, SummonPositions[3]);
                    instance->SummonCreature(NPC_FLAMEWAKER_HEALER, SummonPositions[4]);
                    instance->SummonCreature(NPC_FLAMEWAKER_ELITE, SummonPositions[5]);
                    instance->SummonCreature(NPC_FLAMEWAKER_ELITE, SummonPositions[6]);
                    instance->SummonCreature(NPC_FLAMEWAKER_ELITE, SummonPositions[7]);
                    instance->SummonCreature(NPC_FLAMEWAKER_ELITE, SummonPositions[8]);
                }
                else if (TempSummon* summon = instance->SummonCreature(NPC_MAJORDOMO_EXECUTUS, RagnarosTelePos))
                        summon->AI()->DoAction(ACTION_START_RAGNAROS_ALT);
            }

            std::string GetSaveData() override
            {
                OUT_SAVE_INST_DATA;

                std::ostringstream saveStream;
                saveStream << "M C " << GetBossSaveData();

                OUT_SAVE_INST_DATA_COMPLETE;
                return saveStream.str();
            }

            void Load(char const* data) override
            {
                if (!data)
                {
                    OUT_LOAD_INST_DATA_FAIL;
                    return;
                }

                _isLoading = true;
                OUT_LOAD_INST_DATA(data);

                char dataHead1, dataHead2;

                std::istringstream loadStream(data);
                loadStream >> dataHead1 >> dataHead2;

                if (dataHead1 == 'M' && dataHead2 == 'C')
                {
                    EncounterState states[MAX_ENCOUNTER];
                    uint8 executusCounter = 0;

                    // need 2 loops to check spawning executus/ragnaros
                    for (uint8 i = 0; i < MAX_ENCOUNTER; ++i)
                    {
                        uint32 tmpState;
                        loadStream >> tmpState;
                        if (tmpState == IN_PROGRESS || tmpState > TO_BE_DECIDED)
                            tmpState = NOT_STARTED;
                        states[i] = EncounterState(tmpState);

                         if (tmpState == DONE && i < BOSS_MAJORDOMO_EXECUTUS)
                            ++executusCounter;
                   }

                    if (executusCounter >= 8 && states[BOSS_RAGNAROS] != DONE)
                        _executusSchedule = bool(states[BOSS_MAJORDOMO_EXECUTUS] == DONE);

                    for (uint8 i = 0; i < MAX_ENCOUNTER; ++i)
                        SetBossState(i, states[i]);
                }
                else
                    OUT_LOAD_INST_DATA_FAIL;

                OUT_LOAD_INST_DATA_COMPLETE;
                _isLoading = false;
            }

        private:
            ObjectGuid _golemaggTheIncineratorGUID;
            ObjectGuid _majordomoExecutusGUID;
            ObjectGuid _cacheOfTheFirelordGUID;
            bool _executusSchedule;
            uint8 _deadBossCount;
            uint8 _ragnarosAddDeaths;
            bool _isLoading;
            bool _summonedExecutus;
        };

        InstanceScript* GetInstanceScript(InstanceMap* map) const
        {
            return new instance_molten_core_InstanceMapScript(map);
        }
};

void AddSC_instance_molten_core()
{
    new instance_molten_core();
}

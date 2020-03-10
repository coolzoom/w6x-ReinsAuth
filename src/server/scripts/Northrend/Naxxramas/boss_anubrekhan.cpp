/*
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
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

#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Player.h"
#include "naxxramas.h"

enum AnubSays
{
    SAY_AGGRO           = 0,
    SAY_GREET           = 1,
    SAY_SLAY            = 2,

    EMOTE_LOCUST        = 3
};

enum GuardSays
{
    EMOTE_FRENZY        = 0,
    EMOTE_SPAWN         = 1,
    EMOTE_SCARAB        = 2
};

enum Events
{
    EVENT_IMPALE                    = 1,        // Cast Impale on a random target
    EVENT_LOCUST,                               // Begin channeling Locust Swarm
    EVENT_LOCUST_ENDS,                          // Locust swarm dissipates
    EVENT_SPAWN_GUARD,                          // 10-man only - crypt guard has delayed spawn; also used for the locust swarm crypt guard in both modes
    EVENT_SCARABS,                              // spawn corpse scarabs
    EVENT_BERSERK                               // Berserk
};

enum Spells
{
    SPELL_IMPALE                    = 28783,    // 25-man: 56090
    SPELL_LOCUST_SWARM              = 28785,    // 25-man: 54021
    SPELL_SUMMON_CORPSE_SCARABS_PLR = 29105,    // This spawns 5 corpse scarabs on top of player
    SPELL_SUMMON_CORPSE_SCARABS_MOB = 28864,   // This spawns 10 corpse scarabs on top of dead guards
    SPELL_BERSERK                   = 27680
};

enum SpawnGroups
{
    GROUP_INITIAL_25M       = 1,
    GROUP_SINGLE_SPAWN      = 2
};

enum Misc
{
    ACHIEV_TIMED_START_EVENT                      = 9891
};

enum Phases
{
    PHASE_NORMAL    = 1,
    PHASE_SWARM
};

class boss_anubrekhan : public CreatureScript
{
public:
    boss_anubrekhan() : CreatureScript("boss_anubrekhan") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return GetInstanceAI<boss_anubrekhanAI>(creature);
    }

    struct boss_anubrekhanAI : public BossAI
    {
        boss_anubrekhanAI(Creature* creature) : BossAI(creature, BOSS_ANUBREKHAN) { }

        void SummonGuards()
        {
            if (Is25ManRaid())
                me->SummonCreatureGroup(GROUP_INITIAL_25M);
        }

        void InitializeAI() override
        {
            if (!me->isDead())
            {
                Reset();
                SummonGuards();
            }
        }

        void Reset() override
        {
            _Reset();
            guardCorpses.clear();
        }

        void JustReachedHome() override
        {
            _JustReachedHome();
            SummonGuards();
        }

        void JustSummoned(Creature* summon) override
        {
            BossAI::JustSummoned(summon);
            
            if (me->IsInCombat())
                if (summon->GetEntry() == NPC_CRYPT_GUARD)
                    summon->AI()->Talk(EMOTE_SPAWN, me);
        }

        void SummonedCreatureDies(Creature* summon, Unit* killer) override
        {
            BossAI::SummonedCreatureDies(summon, killer);

            if (summon->GetEntry() == NPC_CRYPT_GUARD)
                guardCorpses.insert(summon->GetGUID());
        }

        void SummonedCreatureDespawn(Creature* summon) override
        {
            BossAI::SummonedCreatureDespawn(summon);

            if (summon->GetEntry() == NPC_CRYPT_GUARD)
                guardCorpses.erase(summon->GetGUID());
        }

        void KilledUnit(Unit* victim) override
        {
            if (victim->GetTypeId() == TYPEID_PLAYER)
                victim->CastSpell(victim, SPELL_SUMMON_CORPSE_SCARABS_PLR, true, nullptr, nullptr, me->GetGUID());

            Talk(SAY_SLAY);
        }

        void JustDied(Unit* /*killer*/) override
        {
            _JustDied();

            // start achievement timer (kill Maexna within 20 min)
            instance->DoStartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, ACHIEV_TIMED_START_EVENT);
        }

        void EnterCombat(Unit* /*who*/) override
        {
            _EnterCombat();
            Talk(SAY_AGGRO);

            summons.DoZoneInCombat();
            
            events.SetPhase(PHASE_NORMAL);
            events.ScheduleEvent(EVENT_IMPALE, urand(10 * IN_MILLISECONDS, 20 * IN_MILLISECONDS), 0, PHASE_NORMAL);
            events.ScheduleEvent(EVENT_SCARABS, urand(20 * IN_MILLISECONDS, 30 * IN_MILLISECONDS), 0, PHASE_NORMAL);
            events.ScheduleEvent(EVENT_LOCUST, urand(80,120) * IN_MILLISECONDS, 0, PHASE_NORMAL);
            events.ScheduleEvent(EVENT_BERSERK, 10 * MINUTE * IN_MILLISECONDS);

            if (!Is25ManRaid())
                events.ScheduleEvent(EVENT_SPAWN_GUARD, urand(15, 20) * IN_MILLISECONDS);
        }

        void UpdateAI(uint32 diff) override
        {
            if (!UpdateVictim())
                return;

            events.Update(diff);

            while (uint32 eventId = events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_IMPALE:
                        if (events.GetTimeUntilEvent(EVENT_LOCUST) < 5 * IN_MILLISECONDS) break; // don't chain impale tank -> locust swarm
                        if (Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 0))
                            DoCast(target, SPELL_IMPALE);
                        else
                            EnterEvadeMode();

                        events.ScheduleEvent(EVENT_IMPALE, urand(10 * IN_MILLISECONDS, 20 * IN_MILLISECONDS), 0, PHASE_NORMAL);
                        break;
                    case EVENT_SCARABS:
                        events.ScheduleEvent(EVENT_SCARABS, urand(40 * IN_MILLISECONDS, 60 * IN_MILLISECONDS), 0, PHASE_NORMAL);

                        if (!guardCorpses.empty())
                        {
                            if (Creature* creatureTarget = ObjectAccessor::GetCreature(*me, Trinity::Containers::SelectRandomContainerElement(guardCorpses)))
                            {
                                creatureTarget->CastSpell(creatureTarget, SPELL_SUMMON_CORPSE_SCARABS_MOB, true, nullptr, nullptr, me->GetGUID());
                                creatureTarget->AI()->Talk(EMOTE_SCARAB);
                                creatureTarget->DespawnOrUnsummon();
                            }
                        }
                        break;
                    case EVENT_LOCUST:
                        Talk(EMOTE_LOCUST);
                        DoCast(me, SPELL_LOCUST_SWARM);
                        events.ScheduleEvent(EVENT_SPAWN_GUARD, 3 * IN_MILLISECONDS);
                        
                        events.ScheduleEvent(EVENT_LOCUST_ENDS, RAID_MODE(19, 23) * IN_MILLISECONDS);
                        events.ScheduleEvent(EVENT_LOCUST, 90000);
                        events.SetPhase(PHASE_SWARM);
                        break;
                    case EVENT_LOCUST_ENDS:
                        events.ScheduleEvent(EVENT_IMPALE, urand(10 * IN_MILLISECONDS, 20 * IN_MILLISECONDS), 0, PHASE_NORMAL);
                        events.ScheduleEvent(EVENT_SCARABS, urand(20 * IN_MILLISECONDS, 30 * IN_MILLISECONDS), 0, PHASE_NORMAL);
                        events.SetPhase(PHASE_NORMAL);
                        break;
                    case EVENT_SPAWN_GUARD:
                        me->SummonCreatureGroup(GROUP_SINGLE_SPAWN);
                        break;
                    case EVENT_BERSERK:
                        DoCast(me, SPELL_BERSERK, true);
                        events.ScheduleEvent(EVENT_BERSERK, 600000);
                        break;
                }
            }

            if (events.IsInPhase(PHASE_NORMAL))
                DoMeleeAttackIfReady();
        }
        private:
            GuidSet guardCorpses;
    };

};

class at_anubrekhan_entrance : public AreaTriggerScript
{
    public:
        at_anubrekhan_entrance() : AreaTriggerScript("at_anubrekhan_entrance") { }

        bool OnTrigger(Player* player, AreaTriggerEntry const* /*areaTrigger*/, bool /*entered*/) override
        {
            InstanceScript* instance = player->GetInstanceScript();
            if (!instance || instance->GetData(DATA_HAD_ANUBREKHAN_GREET) || instance->GetBossState(BOSS_ANUBREKHAN) != NOT_STARTED)
                return true;

            if (Creature* anub = ObjectAccessor::GetCreature(*player, instance->GetGuidData(DATA_ANUBREKHAN)))
                anub->AI()->Talk(SAY_GREET);
            instance->SetData(DATA_HAD_ANUBREKHAN_GREET, 1u);

            return true;
        }
};

void AddSC_boss_anubrekhan()
{
    new boss_anubrekhan();

    new at_anubrekhan_entrance();
}
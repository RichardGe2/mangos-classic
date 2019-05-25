/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Loot/LootMgr.h"
#include "Log.h"
#include "ProgressBar.h"
#include "World/World.h"
#include "Util.h"
#include "Globals/SharedDefines.h"
#include "Globals/ObjectAccessor.h"
#include "Globals/ObjectMgr.h"
#include "Server/DBCStores.h"
#include "Server/SQLStorages.h"
#include "BattleGround/BattleGroundAV.h"
#include "Entities/ItemEnchantmentMgr.h"
#include "Entities/Corpse.h"
#include "Tools/Language.h"


#include <fstream>





INSTANTIATE_SINGLETON_1(LootMgr);

static eConfigFloatValues const qualityToRate[MAX_ITEM_QUALITY] =
{
    CONFIG_FLOAT_RATE_DROP_ITEM_POOR,                       // ITEM_QUALITY_POOR
    CONFIG_FLOAT_RATE_DROP_ITEM_NORMAL,                     // ITEM_QUALITY_NORMAL
    CONFIG_FLOAT_RATE_DROP_ITEM_UNCOMMON,                   // ITEM_QUALITY_UNCOMMON
    CONFIG_FLOAT_RATE_DROP_ITEM_RARE,                       // ITEM_QUALITY_RARE
    CONFIG_FLOAT_RATE_DROP_ITEM_EPIC,                       // ITEM_QUALITY_EPIC
    CONFIG_FLOAT_RATE_DROP_ITEM_LEGENDARY,                  // ITEM_QUALITY_LEGENDARY
    CONFIG_FLOAT_RATE_DROP_ITEM_ARTIFACT,                   // ITEM_QUALITY_ARTIFACT
};

LootStore LootTemplates_Creature("creature_loot_template",     "creature entry",                 true);
LootStore LootTemplates_Disenchant("disenchant_loot_template",   "item disenchant id",             true);
LootStore LootTemplates_Fishing("fishing_loot_template",      "area id",                        true);
LootStore LootTemplates_Gameobject("gameobject_loot_template",   "gameobject lootid",              true);
LootStore LootTemplates_Item("item_loot_template",         "item entry with ITEM_FLAG_LOOTABLE", true);
LootStore LootTemplates_Mail("mail_loot_template",         "mail template id",               false);
LootStore LootTemplates_Pickpocketing("pickpocketing_loot_template", "creature pickpocket lootid",     true);
LootStore LootTemplates_Reference("reference_loot_template",    "reference id",                   false);
LootStore LootTemplates_Skinning("skinning_loot_template",     "creature skinning id",           true);

class LootTemplate::LootGroup                               // A set of loot definitions for items (refs are not allowed)
{
    public:
        void AddEntry(LootStoreItem& item);                 // Adds an entry to the group (at loading stage)
        bool HasQuestDrop() const;                          // True if group includes at least 1 quest drop entry
        bool HasQuestDropForPlayer(Player const* player) const;
        // The same for active quests of the player
        void Process(Loot& loot, Player const* lootOwner) const; // Rolls an item from the group (if any) and adds the item to the loot
        float RawTotalChance() const;                       // Overall chance for the group (without equal chanced items)
        float TotalChance() const;                          // Overall chance for the group

        void Verify(LootStore const& lootstore, uint32 id, uint32 group_id) const;
        void CheckLootRefs(LootIdSet* ref_set) const;
    private:
        LootStoreItemList ExplicitlyChanced;                // Entries with chances defined in DB
        LootStoreItemList EqualChanced;                     // Zero chances - every entry takes the same chance

        LootStoreItem const* Roll(Loot const& loot, Player const* lootOwner) const; // Rolls an item from the group, returns nullptr if all miss their chances
};

// Remove all data and free all memory
void LootStore::Clear()
{
    for (LootTemplateMap::const_iterator itr = m_LootTemplates.begin(); itr != m_LootTemplates.end(); ++itr)
        delete itr->second;
    m_LootTemplates.clear();
}

// Checks validity of the loot store
// Actual checks are done within LootTemplate::Verify() which is called for every template
void LootStore::Verify() const
{
    for (const auto& m_LootTemplate : m_LootTemplates)
        m_LootTemplate.second->Verify(*this, m_LootTemplate.first);
}

// Loads a *_loot_template DB table into loot store
// All checks of the loaded template are called from here, no error reports at loot generation required
void LootStore::LoadLootTable()
{
    LootTemplateMap::const_iterator tab;
    uint32 count = 0;

    // Clearing store (for reloading case)
    Clear();

    //                                                 0      1     2                    3        4              5         6
    QueryResult* result = WorldDatabase.PQuery("SELECT entry, item, ChanceOrQuestChance, groupid, mincountOrRef, maxcount, condition_id FROM %s", GetName());

    if (result)
    {
        BarGoLink bar(result->GetRowCount());

        do
        {
            Field* fields = result->Fetch();
            bar.step();

            uint32 entry               = fields[0].GetUInt32();
            uint32 item                = fields[1].GetUInt32();
            float  chanceOrQuestChance = fields[2].GetFloat();
            uint8  group               = fields[3].GetUInt8();
            int32  mincountOrRef       = fields[4].GetInt32();
            uint32 maxcount            = fields[5].GetUInt32();
            uint16 conditionId         = fields[6].GetUInt16();

            if (maxcount > std::numeric_limits<uint8>::max())
            {
                sLog.outErrorDb("Table '%s' entry %u item %u: maxcount value (%u) to large. must be less than %u - skipped", GetName(), entry, item, maxcount, uint32(std::numeric_limits<uint8>::max()));
                continue;                                   // error already printed to log/console.
            }

            if (conditionId)
            {
                const PlayerCondition* condition = sConditionStorage.LookupEntry<PlayerCondition>(conditionId);
                if (!condition)
                {
                    sLog.outErrorDb("Table `%s` for entry %u, item %u has condition_id %u that does not exist in `conditions`, ignoring", GetName(), entry, item, uint32(conditionId));
                    continue;
                }

                if (mincountOrRef < 0 && !PlayerCondition::CanBeUsedWithoutPlayer(conditionId))
                {
                    sLog.outErrorDb("Table '%s' entry %u mincountOrRef %i < 0 and has condition %u that requires a player and is not supported, skipped", GetName(), entry, mincountOrRef, uint32(conditionId));
                    continue;
                }
            }

            LootStoreItem storeitem = LootStoreItem(item, chanceOrQuestChance, group, conditionId, mincountOrRef, maxcount);

            if (!storeitem.IsValid(*this, entry))           // Validity checks
                continue;

            // Looking for the template of the entry
            // often entries are put together
            if (m_LootTemplates.empty() || tab->first != entry)
            {
                // Searching the template (in case template Id changed)
                tab = m_LootTemplates.find(entry);
                if (tab == m_LootTemplates.end())
                {
                    std::pair< LootTemplateMap::iterator, bool > pr = m_LootTemplates.insert(LootTemplateMap::value_type(entry, new LootTemplate));
                    tab = pr.first;
                }
            }
            // else is empty - template Id and iter are the same
            // finally iter refers to already existing or just created <entry, LootTemplate>

            // Adds current row to the template
            tab->second->AddEntry(storeitem);
            ++count;
        }
        while (result->NextRow());

        delete result;

        Verify();                                           // Checks validity of the loot store

        sLog.outString(">> Loaded %u loot definitions (" SIZEFMTD " templates) from table %s", count, m_LootTemplates.size(), GetName());
        sLog.outString();
    }
    else
    {
        sLog.outString();
        sLog.outErrorDb(">> Loaded 0 loot definitions. DB table `%s` is empty.", GetName());
    }
}

bool LootStore::HaveQuestLootFor(uint32 loot_id) const
{
    LootTemplateMap::const_iterator itr = m_LootTemplates.find(loot_id);
    if (itr == m_LootTemplates.end())
        return false;

    // scan loot for quest items
    return itr->second->HasQuestDrop(m_LootTemplates);
}

bool LootStore::HaveQuestLootForPlayer(uint32 loot_id, Player* player) const
{
    LootTemplateMap::const_iterator tab = m_LootTemplates.find(loot_id);
    if (tab != m_LootTemplates.end())
        if (tab->second->HasQuestDropForPlayer(m_LootTemplates, player))
            return true;

    return false;
}

LootTemplate const* LootStore::GetLootFor(uint32 loot_id) const
{
    LootTemplateMap::const_iterator tab = m_LootTemplates.find(loot_id);

    if (tab == m_LootTemplates.end())
        return nullptr;

    return tab->second;
}

void LootStore::LoadAndCollectLootIds(LootIdSet& ids_set)
{
    LoadLootTable();

    for (LootTemplateMap::const_iterator tab = m_LootTemplates.begin(); tab != m_LootTemplates.end(); ++tab)
        ids_set.insert(tab->first);
}

void LootStore::CheckLootRefs(LootIdSet* ref_set) const
{
    for (const auto& m_LootTemplate : m_LootTemplates)
        m_LootTemplate.second->CheckLootRefs(ref_set);
}

void LootStore::ReportUnusedIds(LootIdSet const& ids_set) const
{
    // all still listed ids isn't referenced
    if (!ids_set.empty())
    {
        for (uint32 itr : ids_set)
        sLog.outErrorDb("Table '%s' entry %d isn't %s and not referenced from loot, and then useless.", GetName(), itr, GetEntryName());
        sLog.outString();
    }
}

void LootStore::ReportNotExistedId(uint32 id) const
{
    sLog.outErrorDb("Table '%s' entry %d (%s) not exist but used as loot id in DB.", GetName(), id, GetEntryName());
}

//
// --------- LootStoreItem ---------
//

// Checks if the entry (quest, non-quest, reference) takes it's chance (at loot generation)
// RATE_DROP_ITEMS is no longer used for all types of entries
bool LootStoreItem::Roll(bool rate) const
{
    if (chance >= 100.0f)
        return true;

    if (mincountOrRef < 0)                                  // reference case
        return roll_chance_f(chance * (rate ? sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_ITEM_REFERENCED) : 1.0f));

    if (needs_quest)
        return roll_chance_f(chance * (rate ? sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_ITEM_QUEST) : 1.0f));

    ItemPrototype const* pProto = ObjectMgr::GetItemPrototype(itemid);

    float qualityModifier = pProto && rate ? sWorld.getConfig(qualityToRate[pProto->Quality]) : 1.0f;

    return roll_chance_f(chance * qualityModifier);
}

// Checks correctness of values
bool LootStoreItem::IsValid(LootStore const& store, uint32 entry) const
{
    if (group >= 1 << 7)                                    // it stored in 7 bit field
    {
        sLog.outErrorDb("Table '%s' entry %d item %d: group (%u) must be less %u - skipped", store.GetName(), entry, itemid, group, 1 << 7);
        return false;
    }

    if (mincountOrRef == 0)
    {
        sLog.outErrorDb("Table '%s' entry %d item %d: wrong mincountOrRef (%d) - skipped", store.GetName(), entry, itemid, mincountOrRef);
        return false;
    }

    if (mincountOrRef > 0)                                  // item (quest or non-quest) entry, maybe grouped
    {
        ItemPrototype const* proto = ObjectMgr::GetItemPrototype(itemid);
        if (!proto)
        {
            sLog.outErrorDb("Table '%s' entry %d item %d: item entry not listed in `item_template` - skipped", store.GetName(), entry, itemid);
            return false;
        }

        if (chance == 0 && group == 0)                      // Zero chance is allowed for grouped entries only
        {
            sLog.outErrorDb("Table '%s' entry %d item %d: equal-chanced grouped entry, but group not defined - skipped", store.GetName(), entry, itemid);
            return false;
        }

        if (chance != 0 && chance < 0.000001f)              // loot with low chance
        {
            sLog.outErrorDb("Table '%s' entry %d item %d: low chance (%f) - skipped", store.GetName(), entry, itemid, chance);
            return false;
        }

        if (maxcount < mincountOrRef)                       // wrong max count
        {
            sLog.outErrorDb("Table '%s' entry %d item %d: max count (%u) less that min count (%i) - skipped", store.GetName(), entry, itemid, uint32(maxcount), mincountOrRef);
            return false;
        }
    }
    else                                                    // mincountOrRef < 0
    {
        if (needs_quest)
        {
            sLog.outErrorDb("Table '%s' entry %d item %d: negative chance is given for a reference, skipped", store.GetName(), entry, itemid);
            return false;
        }
        if (chance == 0)                               // no chance for the reference
        {
            sLog.outErrorDb("Table '%s' entry %d item %d: zero chance is given for a reference, reference will never be used, skipped", store.GetName(), entry, itemid);
            return false;
        }
    }
    return true;                                            // Referenced template existence is checked at whole store level
}




// si on voit que tous les loot d'un objet sont negatif, cela veut dire que cet objet n'est QUE pour les quetes
//    et donc qu'on peut le mettre en loot commun
//
// return TRUE si on peut forcer le loot commun
//
bool LootItem::Richard_lootCommunPourObjDeQuest(unsigned int itemID)
{
	//SELECT ChanceOrQuestChance FROM creature_loot_template WHERE item = '5055'
	//QueryResult* result = CharacterDatabase.PQuery("SELECT ChanceOrQuestChance FROM creature_loot_template WHERE item = '%u'", itemID);


	//int aaaaa=0;


	int itemmmCommand =  itemID;  //  5055; // itemID;
	char command1[2048];
	sprintf(command1,"SELECT ChanceOrQuestChance FROM creature_loot_template WHERE item = '%u' ",itemmmCommand);

	int nbNbNeg = 0;
	int nbNbPos = 0;

	if (QueryResult* result1 = WorldDatabase.PQuery( command1 ))
    {
		BarGoLink bar(result1->GetRowCount());
        do
        {
            bar.step();
            Field* fields = result1->Fetch();
            


			int32 entryItem = fields->GetInt32();
			
			if ( entryItem < 0 ) { nbNbNeg++; } else { nbNbPos++; }


			int aaaa=0;

        }
        while (result1->NextRow());
        delete result1;
	}
	else
	{
		int aaa=0;
	}



	if ( nbNbNeg > 0 && nbNbPos == 0 )
	{
		//BASIC_LOG("RICHAR: on force le loot commun pour item = '%d'", itemID );
		return true;
	}


	int aaannn=0;



	return false;
}









//
// --------- LootItem ---------
//

// Constructor, copies most fields from LootStoreItem and generates random count
LootItem::LootItem(LootStoreItem const& li, uint32 _lootSlot, uint32 threshold)
{
    if (li.needs_quest)
        lootItemType = LOOTITEM_TYPE_QUEST;
    else if (li.conditionId)
        lootItemType = LOOTITEM_TYPE_CONDITIONNAL;
    else
        lootItemType = LOOTITEM_TYPE_NORMAL;

    itemProto         = ObjectMgr::GetItemPrototype(li.itemid);
    if (itemProto)
    {
        freeForAll       = (itemProto->Flags & ITEM_FLAG_MULTI_DROP) != 0;
        displayID        = itemProto->DisplayInfoID;
        isUnderThreshold = itemProto->Quality < threshold;

		freeForAll = Richard_lootCommunPourObjDeQuest(li.itemid) ? true : freeForAll;
    }
    else
    {
        sLog.outError("LootItem::LootItem()> item ID(%u) have no prototype!", li.itemid); // maybe i must make an assert here
        freeForAll = false;
        displayID = 0;
        isUnderThreshold = false;
    }


    itemId            = li.itemid;
    conditionId       = li.conditionId;
    lootSlot          = _lootSlot;
    count             = urand(li.mincountOrRef, li.maxcount);     // constructor called for mincountOrRef > 0 only
    randomSuffix      = 0; // TODO:: do we need to implement it? GenerateEnchSuffixFactor(itemId);
    randomPropertyId  = Item::GenerateItemRandomPropertyId(itemId);
    isBlocked         = false;
    currentLooterPass = false;
    isReleased        = false;
}

LootItem::LootItem(uint32 _itemId, uint32 _count, uint32 _randomSuffix, int32 _randomPropertyId, uint32 _lootSlot)
{
    itemProto = ObjectMgr::GetItemPrototype(_itemId);
    if (itemProto)
    {
        freeForAll = (itemProto->Flags & ITEM_FLAG_MULTI_DROP) != 0;
        displayID = itemProto->DisplayInfoID;

		freeForAll = Richard_lootCommunPourObjDeQuest(_itemId) ? true : freeForAll;
    }
    else
    {
        sLog.outError("LootItem::LootItem()> item ID(%u) have no prototype!", _itemId); // maybe i must make an assert here
        freeForAll = false;
        displayID = 0;
    }

    itemId            = _itemId;
    lootSlot          = _lootSlot;
    conditionId       = 0;
    lootItemType      = LOOTITEM_TYPE_NORMAL;
    count             = _count;
    randomSuffix      = _randomSuffix;
    randomPropertyId  = _randomPropertyId;
    isBlocked         = false;
    isUnderThreshold  = false;
    currentLooterPass = false;
    isReleased        = false;
}


// Basic checks for player/item compatibility - if false no chance to see the item in the loot
bool LootItem::AllowedForPlayer(Player const* player, WorldObject const* lootTarget) const
{
    if (!itemProto)
        return false;

    switch (lootItemType)
    {
        case LOOTITEM_TYPE_NORMAL:
            break;

        case LOOTITEM_TYPE_CONDITIONNAL:
            // DB conditions check
            if (!sObjectMgr.IsPlayerMeetToCondition(conditionId, player, player->GetMap(), lootTarget, CONDITION_FROM_LOOT))
                return false;
            break;

        case LOOTITEM_TYPE_QUEST:
            // Checking quests for quest-only drop (check only quests requirements in this case)
            if (!player->HasQuestForItem(itemId))
                return false;
            break;

        default:
            break;
    }

    // Not quest only drop (check quest starting items for already accepted non-repeatable quests)
    if (itemProto->StartQuest && player->GetQuestStatus(itemProto->StartQuest) != QUEST_STATUS_NONE && !player->HasQuestForItem(itemId))
        return false;

    return true;
}

LootSlotType LootItem::GetSlotTypeForSharedLoot(Player const* player, Loot const* loot) const
{
    // Master looter needs to see quest/conditional items above threshold so he can distribute them
    if (!IsAllowed(player, loot) && (loot->m_lootMethod != MASTER_LOOT || player->GetObjectGuid() != loot->m_masterOwnerGuid || allowedGuid.empty()))
        return MAX_LOOT_SLOT_TYPE;

    if (freeForAll)
        return LOOT_SLOT_NORMAL;                                         // player have not yet looted a free for all item

    // quest items and conditional items cases
    if (lootItemType == LOOTITEM_TYPE_QUEST || lootItemType == LOOTITEM_TYPE_CONDITIONNAL)
    {
        switch (loot->m_lootMethod)
        {
            case NOT_GROUP_TYPE_LOOT:
            case FREE_FOR_ALL:
                return LOOT_SLOT_NORMAL;

            default:
                if (loot->m_isChest)
                    return LOOT_SLOT_NORMAL;

                // Check if its turn of that player to loot a not party loot. The loot may be released or the item may be passed by currentLooter
                if (isReleased || currentLooterPass || loot->m_currentLooterGuid == player->GetObjectGuid())
                    return LOOT_SLOT_NORMAL;
                return MAX_LOOT_SLOT_TYPE;
        }
    }

    switch (loot->m_lootMethod)
    {
        case FREE_FOR_ALL:
            return LOOT_SLOT_NORMAL;
        case GROUP_LOOT:
        case NEED_BEFORE_GREED:
        {
            if (!isBlocked)
            {
                if (loot->m_isChest)
                    return LOOT_SLOT_NORMAL;

                if (isReleased || currentLooterPass || player->GetObjectGuid() == loot->m_currentLooterGuid)
                    return LOOT_SLOT_NORMAL;

                return MAX_LOOT_SLOT_TYPE;
            }
            return LOOT_SLOT_VIEW;
        }
        case MASTER_LOOT:
        {
            if (isUnderThreshold)
            {
                if (loot->m_isChest)
                    return LOOT_SLOT_NORMAL;

                if (!IsAllowed(player, loot))
                    return MAX_LOOT_SLOT_TYPE;

                if (isReleased || currentLooterPass || player->GetObjectGuid() == loot->m_currentLooterGuid)
                    return LOOT_SLOT_NORMAL;

                return MAX_LOOT_SLOT_TYPE;
            }

            if (player->GetObjectGuid() == loot->m_masterOwnerGuid)
                return LOOT_SLOT_MASTER;

            if (!IsAllowed(player, loot))
                return MAX_LOOT_SLOT_TYPE;

            // give a chance to let others just see the content of the loot
            if (isBlocked || sWorld.getConfig(CONFIG_BOOL_CORPSE_ALLOW_ALL_ITEMS_SHOW_IN_MASTER_LOOT))
                return LOOT_SLOT_VIEW;

            return MAX_LOOT_SLOT_TYPE;
        }
        case ROUND_ROBIN:
        {
            if (loot->m_isChest)
                return LOOT_SLOT_NORMAL;

            if (isReleased || currentLooterPass || player->GetObjectGuid() == loot->m_currentLooterGuid)
                return LOOT_SLOT_NORMAL;

            return MAX_LOOT_SLOT_TYPE;
        }
        case NOT_GROUP_TYPE_LOOT:
            return LOOT_SLOT_NORMAL;
        default:
            return MAX_LOOT_SLOT_TYPE;
    }
}

bool LootItem::IsAllowed(Player const* player, Loot const* loot) const
{
    if (!loot->m_isChest)
        return allowedGuid.find(player->GetObjectGuid()) != allowedGuid.end();

    if (allowedGuid.empty() || freeForAll && allowedGuid.find(player->GetObjectGuid()) == allowedGuid.end())
        return AllowedForPlayer(player, loot->GetLootTarget());

    return false;
}

//
// ------- Loot Roll -------
//

// Send the roll for the whole group
void GroupLootRoll::SendStartRoll()
{
    WorldPacket data(SMSG_LOOT_START_ROLL, (8 + 4 + 4 + 4 + 4 + 4 + 1));
    data << m_loot->GetLootGuid();                          // creature guid what we're looting
    data << uint32(m_itemSlot);                             // item slot in loot
    data << uint32(m_lootItem->itemId);                     // the itemEntryId for the item that shall be rolled for
    data << uint32(m_lootItem->randomSuffix);               // randomSuffix
    data << uint32(m_lootItem->randomPropertyId);           // item random property ID
    data << uint32(LOOT_ROLL_TIMEOUT);                      // the countdown time to choose "need" or "greed"

    size_t voteMaskPos = data.wpos();
    data << uint8(0);

    for (RollVoteMap::const_iterator itr = m_rollVoteMap.begin(); itr != m_rollVoteMap.end(); ++itr)
    {
        if (itr->second.vote == ROLL_NOT_VALID)
            continue;

        Player* plr = sObjectMgr.GetPlayer(itr->first);
        if (!plr || !plr->GetSession())
            continue;
        // dependent from player
        RollVoteMask mask = m_voteMask;
        // In NEED_BEFORE_GREED need disabled for non-usable item for player
        if (m_loot->m_lootMethod == NEED_BEFORE_GREED && plr->CanUseItem(m_lootItem->itemProto) != EQUIP_ERR_OK)
            mask = RollVoteMask(mask & ~ROLL_VOTE_MASK_NEED);
        data.put<uint8>(voteMaskPos, uint8(mask));

        plr->GetSession()->SendPacket(data);
    }
}

// Send all passed message
void GroupLootRoll::SendAllPassed()
{
    WorldPacket data(SMSG_LOOT_ALL_PASSED, (8 + 4 + 4 + 4 + 4));
    data << m_loot->GetLootGuid();                          // creature guid what we're looting
    data << uint32(m_itemSlot);                             // item slot in loot
    data << uint32(m_lootItem->itemId);                     // the itemEntryId for the item that shall be rolled for
    data << uint32(m_lootItem->randomSuffix);               // randomSuffix
    data << uint32(m_lootItem->randomPropertyId);           // item random property ID

    for (RollVoteMap::const_iterator itr = m_rollVoteMap.begin(); itr != m_rollVoteMap.end(); ++itr)
    {
        if (itr->second.vote == ROLL_NOT_VALID)
            continue;

        Player* plr = sObjectMgr.GetPlayer(itr->first);
        if (!plr || !plr->GetSession())
            continue;

        plr->GetSession()->SendPacket(data);
    }
}

// Send roll 'value' of the whole group and the winner to the whole group
void GroupLootRoll::SendLootRollWon(ObjectGuid const& targetGuid, uint32 rollNumber, RollVote rollType)
{
    WorldPacket data(SMSG_LOOT_ROLL_WON, (8 + 4 + 4 + 4 + 4 + 8 + 1 + 1));
    data << m_loot->GetLootGuid();                          // creature guid what we're looting
    data << uint32(m_itemSlot);                             // item slot in loot
    data << uint32(m_lootItem->itemId);                     // the itemEntryId for the item that shall be rolled for
    data << uint32(m_lootItem->randomSuffix);               // randomSuffix
    data << uint32(m_lootItem->randomPropertyId);           // item random property ID
    data << targetGuid;                                     // guid of the player who won.
    data << uint8(rollNumber);                              // rollnumber related to SMSG_LOOT_ROLL
    data << uint8(rollType);                                // Rolltype related to SMSG_LOOT_ROLL

    for (RollVoteMap::const_iterator itr = m_rollVoteMap.begin(); itr != m_rollVoteMap.end(); ++itr)
    {
        switch (itr->second.vote)
        {
            case ROLL_PASS:
                break;
            case ROLL_NOT_EMITED_YET:
            case ROLL_NOT_VALID:
                SendRoll(itr->first, 128, 128);
                break;
            default:
                SendRoll(itr->first, itr->second.number, itr->second.vote);
                break;
        }
    }

    for (RollVoteMap::const_iterator itr = m_rollVoteMap.begin(); itr != m_rollVoteMap.end(); ++itr)
    {
        if (itr->second.vote == ROLL_NOT_VALID)
            continue;

        Player* plr = sObjectMgr.GetPlayer(itr->first);
        if (!plr || !plr->GetSession())
            continue;
        plr->GetSession()->SendPacket(data);
    }
}

// Send roll of targetGuid to the whole group (included targuetGuid)
void GroupLootRoll::SendRoll(ObjectGuid const& targetGuid, uint32 rollNumber, uint32 rollType)
{
    WorldPacket data(SMSG_LOOT_ROLL, (8 + 4 + 8 + 4 + 4 + 4 + 1 + 1 + 1));
    data << m_loot->GetLootGuid();                          // creature guid what we're looting
    data << uint32(m_itemSlot);                             // item slot in loot
    data << targetGuid;
    data << uint32(m_lootItem->itemId);                     // the itemEntryId for the item that shall be rolled for
    data << uint32(m_lootItem->randomSuffix);               // randomSuffix
    data << uint32(m_lootItem->randomPropertyId);           // item random property ID
    data << uint8(rollNumber);                              // 0: "Need for: [item name]" > 127: "you passed on: [item name]"      Roll number
    data << uint8(rollType);                                // 0: "Need for: [item name]" 0: "You have selected need for [item name] 1: need roll 2: greed roll
    data << uint8(0);                                       // auto pass on loot

    for (RollVoteMap::const_iterator itr = m_rollVoteMap.begin(); itr != m_rollVoteMap.end(); ++itr)
    {
        if (itr->second.vote == ROLL_NOT_VALID)
            continue;

        Player* plr = sObjectMgr.GetPlayer(itr->first);
        if (!plr || !plr->GetSession())
            continue;

        plr->GetSession()->SendPacket(data);
    }
}

GroupLootRoll::~GroupLootRoll()
{
    if (m_isStarted)
        SendAllPassed();
}

// Try to start the group roll for the specified item (it may fail for quest item or any condition
// If this method return false the roll have to be removed from the container to avoid any problem
bool GroupLootRoll::TryToStart(Loot& loot, uint32 itemSlot)
{
    if (!m_isStarted)
    {
        if (itemSlot >= loot.m_lootItems.size())
            return false;

        // initialize the data needed for the roll
        m_lootItem = loot.GetLootItemInSlot(itemSlot);

        if (m_lootItem->lootItemType == LOOTITEM_TYPE_QUEST)
            return false;

        m_loot = &loot;
        m_itemSlot = itemSlot;
        m_lootItem->isBlocked = true;                           // block the item while rolling

        uint32 playerCount = 0;
        for (auto itr : m_loot->m_ownerSet)
        {
            Player* plr = sObjectMgr.GetPlayer(itr);
            if (!plr || !m_lootItem->IsAllowed(plr, m_loot))    // check if player meet the condition to be able to roll this item
            {
                m_rollVoteMap[itr].vote = ROLL_NOT_VALID;
                continue;
            }
            m_rollVoteMap[itr].vote = ROLL_NOT_EMITED_YET;      // initialize player vote map
            ++playerCount;
        }

        // initialize item prototype
        m_voteMask = ROLL_VOTE_MASK_ALL;

        if (playerCount > 1)                                    // check if more than one player can loot this item
        {
            // start the roll
            SendStartRoll();
            m_endTime = time(nullptr) + (LOOT_ROLL_TIMEOUT / 1000);
            m_isStarted = true;
            return true;
        }
        // no need to start roll if one or less player can loot this item so place it under threshold
        m_lootItem->isUnderThreshold = true;
        m_lootItem->isBlocked = false;
    }
    return false;
}

// Add vote from playerGuid
bool GroupLootRoll::PlayerVote(Player* player, RollVote vote)
{
    ObjectGuid const& playerGuid = player->GetObjectGuid();
    RollVoteMap::iterator voterItr = m_rollVoteMap.find(playerGuid);
    if (voterItr == m_rollVoteMap.end())
        return false;

    voterItr->second.vote = vote;

    if (vote != ROLL_PASS && vote != ROLL_NOT_VALID)
        voterItr->second.number = urand(1, 100);

    switch (vote)
    {
        case ROLL_PASS:                                     // Player choose pass
        {
            SendRoll(playerGuid, 128, 128);
            break;
        }
        case ROLL_NEED:                                     // player choose Need
        {
            SendRoll(playerGuid, 0, 0);
            break;
        }
        case ROLL_GREED:                                    // player choose Greed
        {
            SendRoll(playerGuid, 128, 2);
            break;
        }
        default:                                            // Roll removed case
            return false;
    }
    return true;
}

// check if we can found a winner for this roll or if timer is expired
bool GroupLootRoll::UpdateRoll()
{
    RollVoteMap::const_iterator winnerItr = m_rollVoteMap.end();

    if (AllPlayerVoted(winnerItr) || m_endTime <= time(nullptr))
    {
        Finish(winnerItr);
        return true;
    }
    return false;
}

/**
* \brief: Check if all player have voted and return true in that case. Also return current winner.
* \param: RollVoteMap::const_iterator& winnerItr > will be different than m_rollCoteMap.end() if winner exist. (Someone voted greed or need)
* \returns: bool > true if all players voted
**/
bool GroupLootRoll::AllPlayerVoted(RollVoteMap::const_iterator& winnerItr)
{
    uint32 notVoted = 0;
    bool isSomeoneNeed = false;

    winnerItr = m_rollVoteMap.end();
    for (RollVoteMap::const_iterator itr = m_rollVoteMap.begin(); itr != m_rollVoteMap.end(); ++itr)
    {
        switch (itr->second.vote)
        {
            case ROLL_NEED:
                if (!isSomeoneNeed || winnerItr == m_rollVoteMap.end() || itr->second.number > winnerItr->second.number)
                {
                    isSomeoneNeed = true;                                               // first passage will force to set winner because need is prioritized
                    winnerItr = itr;
                }
                break;
            case ROLL_GREED:
            case ROLL_DISENCHANT:
                if (!isSomeoneNeed)                                                      // if at least one need is detected then winner can't be a greed
                {
                    if (winnerItr == m_rollVoteMap.end() || itr->second.number > winnerItr->second.number)
                        winnerItr = itr;
                }
                break;
            // Explicitly passing excludes a player from winning loot, so no action required.
            case ROLL_PASS:
                break;
            case ROLL_NOT_EMITED_YET:
                ++notVoted;
                break;
            default:
                break;
        }
    }

    return notVoted == 0;
}

// terminate the roll
void GroupLootRoll::Finish(RollVoteMap::const_iterator& winnerItr)
{
    m_lootItem->isBlocked = false;
    if (winnerItr == m_rollVoteMap.end())
    {
        SendAllPassed();
        m_lootItem->isReleased = true;
    }
    else
    {
        SendLootRollWon(winnerItr->first, winnerItr->second.number, winnerItr->second.vote);

        Player* plr = sObjectMgr.GetPlayer(winnerItr->first);
        if (plr && plr->GetSession())
        {
            m_loot->SendItem(plr, m_itemSlot);
        }
        else
        {
            // hum the winner is not available
            m_lootItem->isReleased = true;
        }
    }
    m_isStarted = false;
}

GroupLootRoll* Loot::GetRollForSlot(uint32 itemSlot)
{
    GroupLootRollMap::iterator rollItr = m_roll.find(itemSlot);
    if (rollItr == m_roll.end())
        return nullptr;
    return &rollItr->second;
}

//
// --------- Loot ---------
//

// Inserts the item into the loot (called by LootTemplate processors)
void Loot::AddItem(LootStoreItem const& item)
{
    if (m_lootItems.size() < MAX_NR_LOOT_ITEMS)                              // Normal drop
    {
        LootItem* lootItem = new LootItem(item, m_maxSlot++, uint32(m_threshold));

        if (!lootItem->isUnderThreshold)                                    // set flag for later to know that we have an over threshold item
            m_haveItemOverThreshold = true;

        m_lootItems.push_back(lootItem);
    }
}

// Insert item into the loot explicit way. (used for container item and Item::LoadFromDB)
void Loot::AddItem(uint32 itemid, uint32 count, uint32 randomSuffix, int32 randomPropertyId)
{
    if (m_lootItems.size() < MAX_NR_LOOT_ITEMS)                              // Normal drop
    {
        LootItem* lootItem = new LootItem(itemid, count, randomSuffix, randomPropertyId, m_maxSlot++);

        m_lootItems.push_back(lootItem);

        // add permission to pick this item to loot owner
        for (auto allowedGuid : m_ownerSet)
            lootItem->allowedGuid.emplace(allowedGuid);
    }
}

// Calls processor of corresponding LootTemplate (which handles everything including references)
bool Loot::FillLoot(uint32 loot_id, LootStore const& store, Player* lootOwner, bool personal, bool noEmptyError)
{
    // Must be provided
    if (!lootOwner)
        return false;

    LootTemplate const* tab = store.GetLootFor(loot_id);

    if (!tab)
    {
        if (!noEmptyError)
            sLog.outErrorDb("Table '%s' loot id #%u used but it doesn't have records.", store.GetName(), loot_id);
        return false;
    }

    m_lootItems.reserve(MAX_NR_LOOT_ITEMS);

    tab->Process(*this, lootOwner, store, store.IsRatesAllowed()); // Processing is done there, callback via Loot::AddItem()


















	////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//richard : add coin on loot

	{ // DEBUT encapulse TOUT

	// id dans la base de donnée
	const uint32 coinItemID1 = 70010; // YouhaiCoin Paragon
	const uint32 coinItemID2 = 70007; // YouhaiCoin Cadeau


	// on liste tous les joueurs le la team
	bool bugEtrangeDetecte = false;
	std::vector<Player*> playerList;
	if ( 
	lootOwner &&
	lootOwner->GetGroup() && 
	lootOwner->GetGroup()->GetMembersCount() > 1 )
	{
		//si une team
		Group* gr = lootOwner->GetGroup();
		Group::MemberSlotList const& memberList = gr->GetMemberSlots();
        for (const auto& memberItr : memberList)
		{
			Player* looter = ObjectAccessor::FindPlayer(memberItr.guid);
			if ( looter )
			{
				playerList.push_back(looter);
			}
			else
			{
				//ce bug arrive tres rarement mais il peut arriver !
				//je sais pas trop comlment le provoquer, c'est peut etre une histoire de deco reco
				BASIC_LOG("RICHAR - BUGGGGGGGGGGGGGGGGGGGGGGGGGGG !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
				bugEtrangeDetecte = true;
			}
		}
	}
	else
	{
		//si pas d'equipe
		if ( lootOwner )
		{
			playerList.push_back(lootOwner);
		}
	}

	if ( bugEtrangeDetecte )
	{
		if ( playerList.size() > 0 )
		{
			char messageJoueur[4096];
			sprintf(messageJoueur,"BUG POINTEUR NULL RICHARD DETECTE !!!!!!!!!!!!!!!!!!!!!!!!");
			playerList[0]->Say(messageJoueur, LANG_UNIVERSAL);
		}
	}



	//Gestion de la quete d'objet
	{

		//il y a 2 mode de gameplay possible : 
		//
		// mode_itemCanLoot=false ( mode plus mechant )
		// si l'item a 10% de chance de tomber, alors on est sur que les 9 premier kill ne looteront pas, et le 10ieme donnera l'item
		//
		// mode_itemCanLoot=true  ( mode plus gentil )
		// si l'item a 10% de chance de tomber, alors les 9 premier kill on 10% de chance de looter l'item. 
		// s'il tombe pendant ces 9 kill, la quete est reset
		// s'il tombe pas, on a la garantie de l'avoir au 10ieme
		const bool mode_itemCanLoot = true;

		//on liste tous les item qui sont en cours de quete
		struct ITEM_NEED
		{
			ITEM_NEED()
			{
				statisqueDropOfThisItemOnCurrentNPC = 0.0f;
				nbFound = 0;
				itemDropOnThisLoot = false;
			}

			std::vector<Player*> playerNeedThisItem;//liste des joueurs qui on une quete active de cet item
			std::vector<int> playerNeedThisItem_indexQuest;  // meme taille /  meme ordre  que  playerNeedThisItem - juste pour accelerer -   correspond pour chaque Player de playerNeedThisItem à l'index la la quete dans la propre liste du joueur
			float statisqueDropOfThisItemOnCurrentNPC; // entre 0.0 et 1.0
			unsigned int nbFound; // combien de fois cet item est tombé sur les 'nbTry' creations de loot 
			bool itemDropOnThisLoot; // true si l'item est tombé sur la cadavre reel du mob
		};

		//first is an item ID  -  second is a ITEM_NEED
		std::map<uint32_t,ITEM_NEED> listItemThatPlayersNeed;

		for(int iPLayer=0; iPLayer<playerList.size(); iPLayer++)
		{
			const std::vector<Player::RICHA_ITEM_LOOT_QUEST>& lit = playerList[iPLayer]->m_richa_itemLootQuests;
			for(int iQuest=0; iQuest<lit.size(); iQuest++)
			{
				if ( lit[iQuest].active )
				{
					if ( listItemThatPlayersNeed.find(lit[iQuest].itemid) != listItemThatPlayersNeed.end() )
					{
						//si un des joueur est deja sur cet item
						listItemThatPlayersNeed[lit[iQuest].itemid].playerNeedThisItem.push_back(playerList[iPLayer]);
						listItemThatPlayersNeed[lit[iQuest].itemid].playerNeedThisItem_indexQuest.push_back(iQuest);
					}
					else
					{
						//si personne n'est sur cet item
						listItemThatPlayersNeed[lit[iQuest].itemid].playerNeedThisItem.push_back(playerList[iPLayer]);
						listItemThatPlayersNeed[lit[iQuest].itemid].playerNeedThisItem_indexQuest.push_back(iQuest);
						listItemThatPlayersNeed[lit[iQuest].itemid].statisqueDropOfThisItemOnCurrentNPC = 0.0f;
						listItemThatPlayersNeed[lit[iQuest].itemid].nbFound = 0;
						listItemThatPlayersNeed[lit[iQuest].itemid].itemDropOnThisLoot = false;
					}
				}
			}
		}


		//deja il suffit qu'un joueur soit sur un item , pour qu'on le retire du loot du mob, s'il etait dans la loot liste. ( si on est en mode mechant )
		for(auto& iItemNeed : listItemThatPlayersNeed)
		{

			for(int i=0; i<m_lootItems.size(); i++)
			{
				if ( m_lootItems[i] && m_lootItems[i]->itemId == iItemNeed.first )
				{
					iItemNeed.second.itemDropOnThisLoot = true;

					if ( !mode_itemCanLoot ) // si mode mechant : on retire l'item
					{
						LootItem* ii = m_lootItems[i];
						m_lootItems.erase(m_lootItems.begin()+i);
						delete ii;

						BASIC_LOG("RICHAR - ITEM QUEST - l objet etait loote mais on le retire.");
					}
					else
					{
						int a=0;
					}
				}
			}
		}

		// 5000 c'etait trop, on pouvait attendre jusqu'a 1 secondes de pause. (en DEBUG)
		// 1000 quand on sait on voit la pause, mais ca me semble assez subtil
		// je vais mettre 500 pour vraiment que ce soit fluide
		const int nbTry = 500;

		//unsigned int nbFound = 0;

		for(int iTry=0; iTry<nbTry; iTry++)
		{

			
			//on cree une version simplifié de Loot, pour accelerer le process des 'nbTry' fabrications de loot
			class Loot_RICHA : public Loot
			{
			public:
				void AddItem(LootStoreItem const& item) override
				{
					// NOTE : je ne prends pas en compte le count pour l'instant.

					listItem.push_back(item.itemid);
					//item.mincountOrRef
				}

				void AddItem(uint32 itemid, uint32 count, uint32 randomSuffix, int32 randomPropertyId) override
				{
					listItem.push_back(itemid);
				}

				std::vector<uint32_t> listItem;
			};

			Loot_RICHA newLoot ;
			tab->Process(newLoot, lootOwner, store, store.IsRatesAllowed());

			for(auto& iItemNeed : listItemThatPlayersNeed)
			{
				for(int i=0; i<newLoot.listItem.size();i++)
				{
					if ( newLoot.listItem[i] == iItemNeed.first )
					{
						iItemNeed.second.nbFound++;
					}
				}
			}

		}


		struct LIST_QUEST_TO_REMOVE
		{
			Player* player;
			
			// ca paraitrai bcp plus imple de sauvegarder l'index dans m_richa_itemLootQuests plutot que l'item ID
			// mais je ne le fait pas, car si un meme joueur a plusieur quete a delete dans sa liste, alors il faut gerer le fait que les indices changent a chaque delete.
			int itemID_in____m_richa_itemLootQuests;  
			//int index_in___m_richa_itemLootQuests;

			LIST_QUEST_TO_REMOVE()
			{
				player=nullptr;
				itemID_in____m_richa_itemLootQuests = -1;
			}
		};
		std::vector<LIST_QUEST_TO_REMOVE> listQuestToRemove;

		for(auto& iItemNeed : listItemThatPlayersNeed)
		{

			bool itemEnLootCommun = false;

			
			ItemPrototype const* itemProto = sItemStorage.LookupEntry<ItemPrototype>(  iItemNeed.first  );
			if ( itemProto )
			{
				itemEnLootCommun = (itemProto->Flags & ITEM_FLAG_MULTI_DROP) != 0;
			}

			
			// j'ai remarqué que  Richard_lootCommunPourObjDeQuest est assez lent.  si listItemThatPlayersNeed  a genre > 10 objets , ca peut creer un mini lag
			// a 30 objets on voit un vrai bloquage de > 1 seconde
			// donc pour resoudre ca, je fais un petit static moche ici, pour ne chercher que une seule fois.
			static std::map<uint32_t,bool> lootCommunForceParRichard;
			auto& ittt = lootCommunForceParRichard.find(iItemNeed.first);
			if ( ittt == lootCommunForceParRichard.end() )
			{
				lootCommunForceParRichard[iItemNeed.first] = LootItem::Richard_lootCommunPourObjDeQuest(iItemNeed.first);
				ittt = lootCommunForceParRichard.find(iItemNeed.first);
			}
			


			itemEnLootCommun = ittt->second ? true : itemEnLootCommun;
			


			std::string gagantDebug = "";


			//pour chaque item, on ne choisi que 1 seul joueur a incrémenter le score
			//sauf si objet en loot commun, dans ce cas on donne les points toujours au meme joueur.
			//voir diapo dans H: ou je parle plus préciséments des regle de ces quetes.
			//
			//
			//si item en loot commun, et plusieurs joueurs sont sur la quete, alors on donne toujours au meme joueur
			//ceci va garantir que si un item a 10% de chance de tomber, alors il tombera pour tout le monde au bout de 10 kill
			uint32 indexPlayerChosen = 0;
			if ( itemEnLootCommun && iItemNeed.second.playerNeedThisItem.size() > 1 )
			{
				// on va dire par exemple qu'on donne toujours au joueur avec le plus petit GUID
				uint32 lowerGUID = 0xFFffFFff;
				int playerIndexChoosen = -1;
				for(int i=0; i<iItemNeed.second.playerNeedThisItem.size(); i++)
				{
					uint32 guidd = iItemNeed.second.playerNeedThisItem[i]->GetGUIDLow();
					if ( guidd < lowerGUID )
					{
						playerIndexChoosen = i;
						lowerGUID = guidd;
					}
				}
				indexPlayerChosen = playerIndexChoosen;
			}
			else
			{
				indexPlayerChosen = uint32(urand(      0      ,  iItemNeed.second.playerNeedThisItem.size()-1 )  );
			}
			Player* winner = iItemNeed.second.playerNeedThisItem[indexPlayerChosen];
			Player::RICHA_ITEM_LOOT_QUEST& quest = winner->m_richa_itemLootQuests[ iItemNeed.second.playerNeedThisItem_indexQuest[indexPlayerChosen] ];

			std::string itemName = "objet inconnu";
			//ItemPrototype const* itemProto = sItemStorage.LookupEntry<ItemPrototype>(  iItemNeed.first  );
			if ( itemProto )
			{
				itemName = std::string(itemProto->Name1);
			}

			iItemNeed.second.statisqueDropOfThisItemOnCurrentNPC = (float)iItemNeed.second.nbFound / (float)nbTry;

			//si on est en mode gentil et que l'objet est tombé, alors on reset la quete 
			if ( mode_itemCanLoot && iItemNeed.second.itemDropOnThisLoot )
			{
				//for each player on this item quest
				//for(int iPLayer=0; iPLayer<iItemNeed.second.playerNeedThisItem.size(); iPLayer++)
				//
				// en fait on va etre encore plus gentil, et on va reset uniquement la quete de un joueur choisi au hasard.
				// ce joueur sera celui qui remporte ce loot


				{
					//Player* playy = iItemNeed.second.playerNeedThisItem[iPLayer];
					Player* playy = winner;


					//Player::RICHA_ITEM_LOOT_QUEST& quest = playy->m_richa_itemLootQuests[ iItemNeed.second.playerNeedThisItem_indexQuest[iPLayer] ];


					//
					//on cloture la quete :
					//

					if ( quest.LoopQuest )
					{
						quest.currentScore = 0.0f;

						// quest.nbFoisQueteDone ++; <-- on va dire qu'on on incrémente pas si la quete est pas reelement faite.. ca change pas grand chose.
					}
					else
					{
						//even if the quest will be removed, more secure to finish it as if it's a loop
						quest.currentScore = 0.0f;
						quest.active = false;

						LIST_QUEST_TO_REMOVE questRem;
						questRem.player = playy;
						questRem.itemID_in____m_richa_itemLootQuests = iItemNeed.first;
						listQuestToRemove.push_back(questRem);
					}

				}

				char messageJoueur[4096];
				sprintf(messageJoueur,"quete termin\xc3\xa9\e avant la fin - %s est pour moi.",itemName.c_str());
				winner->Say(messageJoueur, LANG_UNIVERSAL);

				gagantDebug += "l'objet est tombe naturellement ";
				gagantDebug += "on reset donc la quete de ";
				//gagantDebug += std::to_string( iItemNeed.second.playerNeedThisItem.size() );
				//gagantDebug += "joueurs.";
				gagantDebug += winner->GetName();

			}
			else
			{

			
				quest.currentScore += iItemNeed.second.statisqueDropOfThisItemOnCurrentNPC;
				if ( quest.currentScore >= 1.0 ) // si le joueur termine sa quete
				{

					if ( quest.LoopQuest )
					{
						quest.currentScore -= 1.0f; // on reinit ( en accordant au joueur l'eventuel delta qu'il avait au dessus de 1.0
						if ( quest.currentScore >= 1.0f )
						{
							//je pense pas que ce cas devrait arriver, mais on sait jamais.
							//on s'il est toujours au dessus de 1, malgrès mon reset, autant faire un vrai reset a 0
							quest.currentScore = 0.0f;
						}

						quest.nbFoisQueteDone ++;
					}
					else
					{
						//even if the quest will be removed, more secure to finish it as if it's a loop
						quest.nbFoisQueteDone ++;
						quest.currentScore = 0.0f;
						quest.active = false;

						LIST_QUEST_TO_REMOVE questRem;
						questRem.player = winner;
						//questRem.index_in___m_richa_itemLootQuests = iItemNeed.second.playerNeedThisItem_indexQuest[indexPlayerChosen];
						questRem.itemID_in____m_richa_itemLootQuests = iItemNeed.first;
						listQuestToRemove.push_back(questRem);
					}

				

					

					char messageJoueur[4096];
					sprintf(messageJoueur,"quete termin\xc3\xa9\e - %s est pour moi.",itemName.c_str());
					winner->Say(messageJoueur, LANG_UNIVERSAL);
				
					//d'un coté c'est mieux de le donner direct au joueur
					//de l'autre c'est bcp plus simple de le mettre dans le loot, ca evite d'avoir a gerer les inventaire pleins
					//ca permet aussi de gerer les item en loot commun
					AddItem(iItemNeed.first, 1, 0,0);

					gagantDebug = "Gagnant=";
					gagantDebug += std::string(winner->GetName());
				}

			}


			
			if ( iItemNeed.second.statisqueDropOfThisItemOnCurrentNPC > 0.0f ) // on va affichier que les item que ce NPC peut looter
			{
				// si besoin : printf pour debug
				BASIC_LOG("RICHAR - ITEM QUEST : probability item=%d  :  %f/100  %s", 
					iItemNeed.first ,
					iItemNeed.second.statisqueDropOfThisItemOnCurrentNPC * 100.0f , 
					gagantDebug.c_str());
			}


		}




		// a la toute fin de cette etape, on retire les eventuelles quete d'item qui doivent etre retiré
		// ( c'est mieux de ne rien deplacer/retirer les elements de  m_richa_itemLootQuests     avant de fin de l'etape de gestion des quete d'item )
		for(int i=0; i<listQuestToRemove.size(); i++)
		{
			Player* player__ = listQuestToRemove[i].player;

			//search the itemID
			int index = -1;
			for(int j=0; j<player__->m_richa_itemLootQuests.size(); j++)
			{
				if ( player__->m_richa_itemLootQuests[j].itemid == listQuestToRemove[i].itemID_in____m_richa_itemLootQuests )
				{
					index = j;
					break;
				}
			}

			if ( index == -1 )
			{
				// ERREUR
				player__->Say("ERROR MQH2 !!!!!!", LANG_UNIVERSAL);
			}
			else
			{

				player__->m_richa_itemLootQuests.erase(   player__->m_richa_itemLootQuests.begin() + index    );
			}
		}


	} // FIN de - Gestion de la quete d'objet







	/*
	bool lootOrigin_creature = false;
	bool lootOrigin_gameobj = false;
	bool lootOrigin_item = false;

	if ( GetLootTarget() )
	{
		lootOrigin_creature = GetLootTarget()->GetObjectGuid().IsCreature();
		lootOrigin_gameobj = GetLootTarget()->GetObjectGuid().IsGameObject();
	}
	//lootOrigin_item = loot->item->GetObjectGuid().IsItem();
	*/
	
	// 0 unknown
	// 1 creature cadavre loot classic
	// 2 gameobjet
	// 3 item
	// 4 depecage
	int lootOrigin = 0;



	if (  strcmp(store.GetName() , "creature_loot_template") == 0 )
	{
		lootOrigin = 1;
	}
	else if (  strcmp(store.GetName() , "disenchant_loot_template") == 0 )
	{
		int aaa=0;
	}
	else if (  strcmp(store.GetName() , "fishing_loot_template") == 0 )
	{
		int aaa=0;
	}
	else if (  strcmp(store.GetName() , "gameobject_loot_template") == 0 )
	{
		lootOrigin = 2;
	}
	else if (  strcmp(store.GetName() , "item_loot_template") == 0 )
	{
		lootOrigin = 3;
	}
	else if (  strcmp(store.GetName() , "pickpocketing_loot_template") == 0 )
	{
		int aaa=0;
	}
	else if (  strcmp(store.GetName() , "skinning_loot_template") == 0 )
	{
		lootOrigin = 4;
	}
	else if (  strcmp(store.GetName() , "mail_loot_template") == 0 )
	{
		int aaa=0;
	}
	else
	{
		int aa=0;
		BASIC_LOG("RICHAR: ERRRROR !!!!!!!!!!!!!!!!   unkonwn type loot  = '%s'", store.GetName() );

	}
	
	

	/*
	if ( lootOrigin_creature && !lootOrigin_gameobj && !lootOrigin_item )
	{

		Creature* creature__ = GetLootTarget()->GetMap()->GetCreature(  GetLootTarget()->GetObjectGuid()  );
		CreatureInfo const* creatureInfo = creature__->GetCreatureInfo();
		if ( creatureInfo->SkinningLootId == loot_id )
		{
			lootOrigin = 4;
		}
		else
		{
			//sLog.outBasic("RICHAR: LOOT - type creature" );
			lootOrigin = 1;
		}

		
	}
	else if ( !lootOrigin_creature && lootOrigin_gameobj  && !lootOrigin_item )
	{
		//sLog.outBasic("RICHAR: LOOT - type gameobj" );
		lootOrigin = 2;
	}
	else if ( !lootOrigin_creature && !lootOrigin_gameobj  && lootOrigin_item )
	{
		// j'ai pas encore reussi a trouver quand ca vient d'un objet (exemple quand on desanchante)
		//sLog.outBasic("RICHAR: LOOT - type item" ); 
		lootOrigin = 3;
	}
	else
	{
		//sLog.outBasic("RICHAR: LOOT - type ???" );
		int aa=0;
	}
	*/


	// parametre richard01_test est la source :  0=unknown   1=cadavre    2=coffre (TODO ?)
	//
	// parametre richard02_test :
	// si cadavre alors correspond au niveau du cadavre  (Negatif si la creature est elite)
	//

	int32 richard01_test = 0;
	int32 richard02_test = 0;

	Creature* creatureLooting = 0;

	if ( lootOrigin ==  1 ) // creature
	{

		creatureLooting = GetLootTarget()->GetMap()->GetCreature(  GetLootTarget()->GetObjectGuid()  );
		richard02_test = creatureLooting->getLevel();
		if ( creatureLooting->IsElite() )
		{
			richard02_test = -richard02_test;
		}

		richard01_test = 1;

	}
	else if ( lootOrigin ==  2 ) // game object  (genre : coffre)
	{
		GameObject* goLooting = GetLootTarget()->GetMap()->GetGameObject(  GetLootTarget()->GetObjectGuid()  );
		//const char* nameGo = goLooting->GetName();

		if ( goLooting )
		{
			GameObjectInfo const* goinfo = goLooting->GetGOInfo();

			if ( goinfo )
			{

				const char* nameGo = goinfo->name;

				//  RICHARD_LOOT_COFFRE

				// comme les Elite gris :
				//
				// 1PA si notre perso est niveau  >=1  et  <10   
				// 10PA si notre perso est niveau  >=10  et  <20   
				// 50PA si notre perso est niveau  >=20  et  <30 
				// 1PO si notre perso est niveau  >=30  et  <40 
				// 2PO si notre perso est niveau  >=40  et  <50 
				// 4PO si notre perso est niveau  >=50  et  <60 
				// 5PO pour 60

				if (   
					nameGo
					&&
					(

						//maintenanir la meme liste en ici et  _______richard_ALL.txt
						// il doit y avoir 37 coffres de ces 9 types  dans tous wowhainy
						strcmp(nameGo , "Solid Chest" ) == 0				||  strcmp(nameGo , "Coffre solide" ) == 0 
					||  strcmp(nameGo , "Battered Chest" ) == 0				||  strcmp(nameGo , "Coffre endommag\xc3\xa9" ) == 0 
					||  strcmp(nameGo , "Locked Chest" ) == 0				||  strcmp(nameGo , "Coffre verrouill\xc3\xa9" ) == 0 
					||  strcmp(nameGo , "Large Solid Chest" ) == 0			||  strcmp(nameGo , "Grand coffre solide" ) == 0 
					||  strcmp(nameGo , "Large Mithril Bound Chest" ) == 0  ||  strcmp(nameGo , "Grand coffre cercl\xc3\xa9 de mithril" ) == 0 
					||  strcmp(nameGo , "Ancient Treasure" ) == 0			||  strcmp(nameGo , "Tr\xc3\xa9sor ancien" ) == 0 
					||  strcmp(nameGo , "Witch Doctor\'s Chest" ) == 0		||  strcmp(nameGo , "Coffre du sorcier-docteur" ) == 0 
					||  strcmp(nameGo , "Large Battered Chest" ) == 0		||  strcmp(nameGo , "Grand coffre endommag\xc3\xa9" ) == 0 
					||  strcmp(nameGo , "Tattered Chest" ) == 0				||  strcmp(nameGo , "Coffre en morceaux" ) == 0 
					||  strcmp(nameGo , "Old Treasure Chest" ) == 0			||  strcmp(nameGo , "Vieux coffre au tr\xc3\xa9sor" ) == 0 
						)
					)
				{
					int aaaa=0;

					// ajout d'une youhaincoin de type cadeau dans tous les coffres
					// note : si un coffre ne donner pas de Youhaicoin, ca veut dire qu'il faut ajouter son nom a la liste
					AddItem(coinItemID2, 1, 0, 0);



					int playerlevel = lootOwner->getLevel();
					
					lootOwner->Say("Coffre gagn\xc3\xa9 !", LANG_UNIVERSAL);


					uint32  goldBase = 0;

					if ( playerlevel >= 1 && playerlevel < 10 )
					{
						goldBase = 1 * 100;
					}
					else if ( playerlevel >= 10 && playerlevel < 20 )
					{
						goldBase = 10 * 100;
					}
					else if ( playerlevel >= 20 && playerlevel < 30 )
					{
						goldBase = 50 * 100;
					}
					else if ( playerlevel >= 30 && playerlevel < 40 )
					{
						goldBase = 1 * 100 * 100;
					}
					else if ( playerlevel >= 40 && playerlevel < 50 )
					{
						goldBase = 2 * 100 * 100;
					}
					else if ( playerlevel >= 50 && playerlevel < 60 )
					{
						goldBase = 4 * 100 * 100;
					}
					else if ( playerlevel >= 60 )
					{
						goldBase = 5 * 100 * 100;
					}

					// + ou - 20%

					int minGoldBase = (int)goldBase-((int)goldBase*20)/100;
					if ( minGoldBase < 0 )
					{
						minGoldBase = 0;
					}

					m_gold = uint32(urand(      minGoldBase      ,  goldBase+(goldBase*20)/100  )  );


					BASIC_LOG("RICHAR: add coin on COFFRE - base=%d  -  entre %d et %d --> %d  ",
					goldBase,
					minGoldBase ,
					goldBase+(goldBase*20)/100,
					m_gold
					);



					//m_gold = uint32(urand(minAmount >> 8, maxAmount >> 8) 
					//m_gold = 10001;

				}
				else if ( goinfo->id  ==  400001 ) // coffre de l'aventurier
				{
					
					// cette ligne ne marche pas
					// j'aimerai bien reduire le temps de respawn qui est de 60 secondes... enfin c'est low priority c'est pas tres grave
					//goLooting->SetRespawnTime(1);


					uint32 zone_id, area_id;
					lootOwner->GetZoneAndAreaId(zone_id, area_id);
					MapEntry const* mapEntry = sMapStore.LookupEntry(lootOwner->GetMapId());
					AreaTableEntry const* zoneEntry = GetAreaEntryByAreaID(zone_id);
					AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(area_id);
					float posXf = lootOwner->GetPositionX();
					int posXi = (int)posXf;

					if ( areaEntry )
					{

						std::ifstream infile("C:\\Users\\Administrator\\Desktop\\wowServ\\_DOC\\youhai_update_mangos_coffreaventure.sql");
						
						struct ITEMCOFFREAVENTURIER
						{
							int item;
							std::string region;
							int x;
						};
						std::vector<ITEMCOFFREAVENTURIER> listItem;

						if ( infile )
						{
							std::string line;
							while (std::getline(infile, line))
							{
								if ( line.size() > 10 ) // si trop petite ligne ca sert a rien
								{

									if (
										   line[0] == '-' && line[1] == '-'  // <-
										|| line[1] == '-' && line[2] == '-'  // <-- c'est très moche, mais ca va suffir
										|| line[3] == '-' && line[4] == '-'  // <-

										)
									{
										/// si c'est une ligne commentaire
										int eee=0;
									}

									else
									{

										const char* addre1 = strstr( line.c_str() ,   "item = '"  );
										if ( addre1 )
										{
											addre1 += 8;
											int item = atoi(addre1);
									
											const char* addre2 = strstr( line.c_str() ,   "-- "  );
									
											std::string regionName;
									
											if ( addre2 )
											{
												addre2 += 3;
										
												for(int i=0; ;i++)
												{
													if ( *addre2 == '*' )
													{
														addre2++;
														break;
													}
													regionName.push_back(*addre2);
													addre2++;
												}
											}

											int x = atoi(addre2);

											ITEMCOFFREAVENTURIER newww;
											newww.item = item;
											newww.region = regionName;
											newww.x = x;
											listItem.push_back(newww);

											int aaa=0;
										}

									}
								}

							}

							infile.close();
						}
						else
						{
							lootOwner->Say("ERROR 538F : can't open coffreaventure.sql", LANG_UNIVERSAL);
						}


						



						bool itemFound = false;
						if ( listItem.size() <= 0 )
						{
							lootOwner->Say("ERROR 548F : coffreaventure.sql - liste vide", LANG_UNIVERSAL);
						}
						else
						{
							for(int i=0; i<listItem.size(); i++)
							{
								if ( listItem[i].region == areaEntry->area_name[0]
									&& abs(posXi-(listItem[i].x))<=7 )
								{
									AddItem(listItem[i].item, 1, 0, 0);
									itemFound = true;
									break;
								}

							}

							if ( !itemFound )
							{
								//on ecrit les 2 caracteristique d'un coffre :  area name, et posX. ca devrait suffir pout pas avoir de conflit
								char messageOut[2048];
								sprintf(messageOut, "ERROR 598K : Pas d'objet de quete. - *%s*%d   -guid=%d",
									areaEntry->area_name[0],
									posXi,
									GetLootTarget()->GetGUIDLow() 
								
								);
								lootOwner->Say(messageOut, LANG_UNIVERSAL);
							}
							else
							{
								if (    strcmp( lootOwner->GetName() , "GrandJuge" ) == 0
									||  strcmp( lootOwner->GetName() , "GrandTroll" ) == 0
									
									)
								{
									//juste un message d'info pour m'aider a debugger
									char messageOut[2048];
									sprintf(messageOut, "INFO : itemFound - listItem.size()=%d - guid=%d",listItem.size() , GetLootTarget()->GetGUIDLow()  );
									lootOwner->Say(messageOut, LANG_UNIVERSAL);
								}
							}
						}


					
					}
					else
					{
						char messageOut[2048];
						sprintf(messageOut, "ERROR 598J : wrong areaEntry");
						lootOwner->Say(messageOut, LANG_UNIVERSAL);
					}


					int aaa=0;
				}

				int affaa=0;

			}

		}

		int aaaa=0;
	}
	else if ( lootOrigin ==  3 ) // clikc droit sur item qui loot un truc ( genre cadeau de noel )
	{

		const unsigned int itemID = loot_id; // ID de l'item qui donne le loot


		//  "Cadeau a emballage multicolore" -  "gaily wrapped present"
		// je vais effacer le loot coté database, et on va gerer ici.
		//car j'arrive pas a mettre les pourcentage que je veux dans la database
		if ( 21310 == itemID ) 
		{
			//on va dire 50% de chance de rien avoir.
			//puis une chance egale d'avoir 1 des 4 item a collectionner.
			uint32 onAUnCadeau = urand(0,1)  ;
			BASIC_LOG("RICHAR: CADEAU NOEL - onAUnCadeau = %d  ",onAUnCadeau);
			if ( onAUnCadeau == 1 )
			{
				uint32 cadeauId = urand(0,3)  ;
				BASIC_LOG("RICHAR: CADEAU NOEL - cadeauId = %d  ",cadeauId);
				if ( cadeauId == 0 )
					AddItem(21301,1,0,0);
				if ( cadeauId == 1 )
					AddItem(21305,1,0,0);
				if ( cadeauId == 2 )
					AddItem(21308,1,0,0);
				if ( cadeauId == 3 )
					AddItem(21309,1,0,0);
				
			}
			
			
			

		}

		// pour les coffre item, on va donner une petite récompense en PO, on va dire qu'on va s'aligner sur les coffre gameobject, mais en divisant par 2 - car faut que ca reste moins fifou
		// et on ne mets pas de youhaicoin ( contrairement au coffre gameobject )
		if (   itemID == 4637 // Coffret d'acier
			|| itemID == 4638 // Coffret d'acier renforcé
			|| itemID == 5759 // Coffret de thorium
			|| itemID == 4634 // Coffret en fer
			|| itemID == 5758 // Coffret en mithril
			|| itemID == 5760 // Coffret en éternium
			|| itemID == 4633 // Coffret lourd en bronze
			|| itemID == 4632 // Coffret orné en bronze
			|| itemID == 4636 // Solide coffret en fer
			)
		{
			
			int playerlevel = lootOwner->getLevel();
					
			//lootOwner->Say("Coffre gagn\xc3\xa9 !", LANG_UNIVERSAL);


			uint32  goldBase = 0;

			if ( playerlevel >= 1 && playerlevel < 10 )
			{
				goldBase = 1 * 100 / 2;
			}
			else if ( playerlevel >= 10 && playerlevel < 20 )
			{
				goldBase = 10 * 100 / 2;
			}
			else if ( playerlevel >= 20 && playerlevel < 30 )
			{
				goldBase = 50 * 100 / 2;
			}
			else if ( playerlevel >= 30 && playerlevel < 40 )
			{
				goldBase = 1 * 100 * 100 / 2;
			}
			else if ( playerlevel >= 40 && playerlevel < 50 )
			{
				goldBase = 2 * 100 * 100 / 2;
			}
			else if ( playerlevel >= 50 && playerlevel < 60 )
			{
				goldBase = 4 * 100 * 100 / 2;
			}
			else if ( playerlevel >= 60 )
			{
				goldBase = 5 * 100 * 100 / 2;
			}

			// + ou - 20%

			int minGoldBase = (int)goldBase-((int)goldBase*20)/100;
			if ( minGoldBase < 0 )
			{
				minGoldBase = 0;
			}

			m_gold = uint32(urand(      minGoldBase      ,  goldBase+(goldBase*20)/100  )  );


			BASIC_LOG("RICHAR: add coin on COFFRE ITEM - base=%d  -  entre %d et %d --> %d  ",
			goldBase,
			minGoldBase ,
			goldBase+(goldBase*20)/100,
			m_gold
			);
		}

		int aa=0;
	}
	else
	{
		int dfdfd=0;
	}





	if (richard01_test == 0)
	{
		//BASIC_LOG("RICHAR: add coin on loot - NO because richard01_test=%d", richard01_test);
	}
	else if (richard01_test == 1) // cadavre
	{

		//jaune, c'est tout le temps entre   lvlPlayer-2  et  lvlPlayer+2
		//vert ca depend : pour un player 10, le vert va etre entre 5 et 7    pour un player lvl 50 le vert va etre entre 40 et 47... donc je vais considere que le vert sera entre  lvl-7 et lvl-3
		//
		//apres un peu plus de test  (en mettant 2 Game Master l'un en face de l'autre), je confirme que Jaune ca change pas.
		//voila en gros ce que j'estime :
		//perso 1->10   vert sera entre lvl-5   et lvl-3 
		//perso 10->19  vert sera entre lvl-5   et lvl-3   <
		//perso 20->29  vert sera entre lvl-6   et lvl-3   <
		//perso 30->39  vert sera entre lvl-7   et lvl-3   <--- c'est pas parfaitement exact mais ca donne une très bonne estimation
		//perso 40->49  vert sera entre lvl-9   et lvl-3   <
		//perso 50->59  vert sera entre lvl-10  et lvl-3   <
		//perso     60  vert sera entre lvl-12  et lvl-3 
		//
		//
		// je vais rester sur ma constante qui considere le NPC vert pour les Youhaicoin aux niveaux  [ -7  ->  -3  ]
		// ca favorise les bas niveau qui vont avoir des youhaicoin sur qq mobs gris
		// et ca défavorise les HL qui ne vont jamais avoir de youhaicoin sur des vert trop bas level
		// j'aime bien
		//


		char typeMobChar[256];
		strcpy(typeMobChar, "ERROR");

		float difficultyDonjon = 0.0;

		Creature::GetRichardModForMap(  creatureLooting->m_richar_lieuOrigin , creatureLooting->GetName(),  creatureLooting->GetOwner() , &difficultyDonjon, NULL , NULL );

		int playerParagon = lootOwner->GetParagonLevelFromItem();
		int playerlevel = lootOwner->getLevel();
		int cadavreLevel = abs(richard02_test);
		bool cadavreElite = richard02_test < 0 ? true : false;
		float scoreRand = (float)(rand() % 100 + 1);     //  1 to 100
		
		
		// plus il est petit, plus ca sera rare pour le joueur a looter la youhaicoin
		// 0.0 :  ne loot pas de youhaicoin
		float scoreToReach = 0.0f;
		
		
		if (false) {}
		else if (!cadavreElite &&    cadavreLevel >= playerlevel - 7 && cadavreLevel <= playerlevel - 3) // creature non elite verte
		{
			strcpy(typeMobChar, "easy-vert");
			scoreToReach = 5.0f;
		}
		else if (!cadavreElite &&   cadavreLevel >= playerlevel - 2 && cadavreLevel <= playerlevel + 2) // creature non elite jaune
		{
			strcpy(typeMobChar, "easy-jaune");
			scoreToReach = 10.0f;
		}
		else if (!cadavreElite &&   cadavreLevel >= playerlevel + 3) // creature non elite orange ou plus
		{
			strcpy(typeMobChar, "easy-orange");
			scoreToReach = 15.0f;
		}
		else if (cadavreElite &&     cadavreLevel >= playerlevel - 7 && cadavreLevel <= playerlevel - 3) // creature  elite verte
		{
			strcpy(typeMobChar, "elite-verte");
			//scoreToReach = 10.0f;
			scoreToReach = 6.0f;  // <-- changement fait le 9 dec 2018 - voir plus bas pour details
		}
		else if (cadavreElite &&   cadavreLevel >= playerlevel - 2 && cadavreLevel <= playerlevel + 2) // creature  elite jaune
		{
			strcpy(typeMobChar, "elite-jaune");
			//scoreToReach = 15.0f;
			scoreToReach = 11.0f;  // <-- changement fait le 9 dec 2018 - voir plus bas pour details
		}
		else if (cadavreElite &&     cadavreLevel >= playerlevel + 3) // creature  elite orange ou plus
		{
			strcpy(typeMobChar, "elite-orange");
			//scoreToReach = 20.0f;
			scoreToReach = 16.0f;  // <-- changement fait le 9 dec 2018 - voir plus bas pour details
		}
		else
		{
			strcpy(typeMobChar, "tooEasy");
			scoreToReach = 0.0f;
		}


		// c'est ICI que je vais mettre les exception de mob qui ne loot PAS de youhaicoin
		if ( creatureLooting->GetEntry() == 10475 ) // execption : Etudiant de scholomance, car on les a simplifié
		{
			scoreToReach = 0.0f;
		}



		// a propos du changement du 9 dec 2018 :
		// je prefere ne plus trop differencier les elites des normaux.
		// Deja, les elites random d'exterieur, on les kill rarement, donc ca change rien aux stats.
		// Concernant les elites de donjon, on va en tuer en masse, et on va equilibrer ces derniers pour qu'ils correspondent a une difficulté 2 joueurs.
		// et donc pas trop different d'un mob non elite. donc je vois pas pourquoi ils devraient looter plus qu'un mob normal.
		// je pense aussi surtout au perso 60 qui vont passer leur temps dans les donjons a tuer que de l'elite.
		// ca fausse donc les stats. quand j'ai fait ca a la base, c'etait dans l'idée qu'un elite est rare, et compliqué a tuer.
		// je vais quand meme laisser un petit +1% pour augmenter legerement le loot en donjon, mais il doit rester relativement rare


		//on va ajuster le score to reach en fonction du paragon
		//cette regle ne marchera QUE pour reduire, pas pour augmenter.
		//un joueur paragon 1 qui s'attaque a un mob fait pour 2 joueurs  aura autant de moins de chance 
		//en gros ce IF ne va concerner que les joueur 60 avec un paragon > 1
		if ( 
			playerlevel >= 60
			&& playerParagon > 1 // si je joueur est paragon assez haut pour devoir etre geré autrement
			) 
		{

			
			// rappel : c'est un malus : ce coefficient ne doit pas depasser 1.0
			// plus il est petit, plus ca sera rare pour le joueur a looter la youhaicoin
			// 0.0 :  ne loot pas de youhaicoin
			float  coeffFacilite = 1.0f;


			// si un joueur paragon 10 s'attaque a un mob fait pour paragon 5, alors on je dirai qu'on divise par 2 la probabilite de youhaicoin ?
			if ( playerParagon > (int)difficultyDonjon )
			{
				if ( difficultyDonjon > 0.001f )
				{
					coeffFacilite =  1.0f  /  ((float)playerParagon / (float)difficultyDonjon);
				}
				else
				{
					// si le mob a une difficulté 0 ( mob d'exterieur ) alors on donne forcément un coeff de 0
					coeffFacilite = 0.0f;
				}
			}


			scoreToReach *= coeffFacilite;  

			BASIC_LOG("RICHAR: add coin on loot - origine=CADAVRE  playerlevel=%d  cadavreLevel=%d cadavreType=%s scoreResult:%.1f<=%.1f/100 -  MalusParagon=%f",
				playerlevel,
				cadavreLevel,
				typeMobChar,
				scoreRand, scoreToReach,
				coeffFacilite
				);
		}
		else
		{
			BASIC_LOG("RICHAR: add coin on loot - origine=CADAVRE  playerlevel=%d  cadavreLevel=%d cadavreType=%s scoreResult:%.1f<=%.1f/100",
				playerlevel,
				cadavreLevel,
				typeMobChar,
				scoreRand, scoreToReach
				);
		}



		

		if (scoreRand <= scoreToReach)
		{

			uint32 newCoin_ = urand(0,1) == 0 ?  coinItemID1  :  coinItemID2 ;


			AddItem(   newCoin_   , 1, 0, 0);


			char messageOut[2048];

			if ( newCoin_ == coinItemID1 )
			{
				sprintf(messageOut, "+1 youhaicoin paragon!");
			}
			else
			{
				sprintf(messageOut, "+1 youhaicoin cadeau!");
			}
			lootOwner->Say(messageOut, LANG_UNIVERSAL);

			// Dommage, je prefererai faire parler le cadavre de la creature, mais ca marche pas bien :  le cadavre va bien parler, on voit le message dans la fenetre de conversation
			// mais.. il n'y a pas une vraie bulle de BD
			//char messageOut[2048];
			//sprintf(messageOut, "TEST");
			//creatureLooting->MonsterSay(messageOut, LANG_UNIVERSAL);
		}

		

	}
	else
	{
		//BASIC_LOG("RICHAR: add coin on loot - NO because ERROR richard01=%d", richard01);
		int dfdfd=0;
	}


	// richard partie 2 : on ajoute peut etre un youhaimon EPIC :
	if ( 
		
		lootOrigin ==  1 &&  // creature

		//c'est + sympa que ca ne tombe que quand on joue ensemble, plutot que quand on joue tout seul
		lootOwner &&
		lootOwner->GetGroup() && 
		lootOwner->GetGroup()->GetMembersCount() > 1 
		) 
	{

		Creature* creatureLooting = GetLootTarget()->GetMap()->GetCreature(  GetLootTarget()->GetObjectGuid()  );
		int itemYouhaimon = 200000 + creatureLooting->Richar_GetOriginalEntry();

		ItemPrototype const* itemProto = ObjectMgr::GetItemPrototype(itemYouhaimon);
		if (itemProto) // si l'objet youhaimon existe
		{
			itemProto->Name1;

			int scoreRandYouhaimon = rand() % 1000 ;
			if ( scoreRandYouhaimon >= 123 && scoreRandYouhaimon <= 124 ) // 2 chances sur 1000 d'avoir un youhaimon
			{
				lootOwner->Say("LOOT YOUHAIMON EPIC !", LANG_UNIVERSAL);
				AddItem(itemYouhaimon, 1, 0, 0);
			}

			BASIC_LOG("RICHAR: -------------------------------------------------------------> score youhaimon : %d",scoreRandYouhaimon);

			
		}
		else
		{
			// objet youhaimon pas trouvé
		}

		
	}

	} // fin encapulse TOUT

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////


























    // fill the loot owners right here so its impossible from this point to change loot result
    Player* masterLooter = nullptr;
    if (m_lootMethod == MASTER_LOOT)
        masterLooter = ObjectAccessor::FindPlayer(m_masterOwnerGuid);

    for (auto playerGuid : m_ownerSet)
    {
        Player* player = ObjectAccessor::FindPlayer(playerGuid);

        for (auto lootItem : m_lootItems)
        {
            // roll for over-threshold item if it's one-player loot
            if (lootItem->freeForAll || lootItem->itemProto->Quality < uint32(m_threshold))
                lootItem->isUnderThreshold = true;
            else
            {
                switch (m_lootMethod)
                {
                    case MASTER_LOOT:
                    {
                        if (!masterLooter)
                            lootItem->isBlocked = true;
                        break;
                    }

                    case GROUP_LOOT:
                    case NEED_BEFORE_GREED:
                    {
                        lootItem->isBlocked = true;
                        break;
                    }

                    default:
                        break;
                }
            }

            if (player && (lootItem->AllowedForPlayer(player, GetLootTarget())))
            {
                if (!m_isChest)
                    lootItem->allowedGuid.emplace(player->GetObjectGuid());
            }
            else
            {
                if (playerGuid == m_currentLooterGuid)
                    lootItem->currentLooterPass = true;         // Some item may not be allowed for current looter, must set this flag to avoid item not distributed to other player
            }
        }
    }

    return true;
}

// Get loot status for a specified player
uint32 Loot::GetLootStatusFor(Player const* player) const
{
    uint32 status = 0;

    if (m_isFakeLoot && m_playersOpened.empty())
        return LOOT_STATUS_FAKE_LOOT;

    if (m_gold != 0)
        status |= LOOT_STATUS_CONTAIN_GOLD;

    for (auto lootItem : m_lootItems)
    {
        LootSlotType slotType = lootItem->GetSlotTypeForSharedLoot(player, this);
        if (slotType == MAX_LOOT_SLOT_TYPE)
            continue;

        status |= LOOT_STATUS_NOT_FULLY_LOOTED;

        if (lootItem->freeForAll)
            status |= LOOT_STATUS_CONTAIN_FFA;

        if (lootItem->isReleased)
            status |= LOOT_STATUS_CONTAIN_RELEASED_ITEMS;
    }
    return status;
}

// Is there is any loot available for provided player
bool Loot::IsLootedFor(Player const* player) const
{
    return (GetLootStatusFor(player) == 0);
}

bool Loot::IsLootedForAll() const
{
    for (auto itr : m_ownerSet)
    {
        Player* player = ObjectAccessor::FindPlayer(itr);
        if (!player)
            continue;

        if (!IsLootedFor(player))
            return false;
    }
    return true;
}

bool Loot::CanLoot(Player const* player)
{
    ObjectGuid const& playerGuid = player->GetObjectGuid();

    // not in Guid list of possible owner mean cheat
    GuidSet::const_iterator itr = m_ownerSet.find(playerGuid);
    if (itr == m_ownerSet.end())
        return false;

    uint32 lootStatus = GetLootStatusFor(player);

    // is already looted?
    if (!lootStatus)
        return false;

    // all player that have right too loot have right to loot dropped money
    if ((lootStatus & LOOT_STATUS_CONTAIN_GOLD) != 0 || (lootStatus & LOOT_STATUS_CONTAIN_FFA) != 0)
        return true;

    if (m_lootMethod == NOT_GROUP_TYPE_LOOT || m_lootMethod == FREE_FOR_ALL)
        return true;

    if (m_haveItemOverThreshold)
    {
        // master loot have always loot right when the loot contain over threshold item
        if (m_lootMethod == MASTER_LOOT && player->GetObjectGuid() == m_masterOwnerGuid)
            return true;

        // player can all loot on 'group loot' or 'need before greed' loot type
        if (m_lootMethod != MASTER_LOOT && m_lootMethod != ROUND_ROBIN)
            return true;
    }

    // if the player is the current looter (his turn to loot under threshold item) or the current looter released the loot then the player can loot
    if ((lootStatus & LOOT_STATUS_CONTAIN_RELEASED_ITEMS) != 0 || player->GetObjectGuid() == m_currentLooterGuid)
        return true;

    return false;
}

void Loot::NotifyItemRemoved(uint32 lootIndex)
{
    // notify all players that are looting this that the item was removed
    GuidSet::iterator i_next;
    for (GuidSet::iterator i = m_playersLooting.begin(); i != m_playersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        Player* plr = ObjectAccessor::FindPlayer(*i);

        if (plr && plr->GetSession())
            NotifyItemRemoved(plr, lootIndex);
        else
            m_playersLooting.erase(i);
    }
}

void Loot::NotifyItemRemoved(Player* player, uint32 lootIndex) const
{
    // notify a player that are looting this that the item was removed
    WorldPacket data(SMSG_LOOT_REMOVED, 1);
    data << uint8(lootIndex);
    player->GetSession()->SendPacket(data);
}

void Loot::NotifyMoneyRemoved()
{
    // notify all players that are looting this that the money was removed
    GuidSet::iterator i_next;
    for (GuidSet::iterator i = m_playersLooting.begin(); i != m_playersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        Player* plr = ObjectAccessor::FindPlayer(*i);
        if (plr && plr->GetSession())
        {
            WorldPacket data(SMSG_LOOT_CLEAR_MONEY, 0);
            plr->GetSession()->SendPacket(data);
        }
        else
            m_playersLooting.erase(i);
    }
}

void Loot::GenerateMoneyLoot(uint32 minAmount, uint32 maxAmount)
{
    if (maxAmount > 0)
    {
        if (maxAmount <= minAmount)
            m_gold = uint32(maxAmount * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));
        else if ((maxAmount - minAmount) < 32700)
            m_gold = uint32(urand(minAmount, maxAmount) * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));
        else
            m_gold = uint32(urand(minAmount >> 8, maxAmount >> 8) * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY)) << 8;
    }
}

void Loot::SendReleaseFor(ObjectGuid const& guid)
{
    Player* plr = sObjectAccessor.FindPlayer(guid);
    if (plr && plr->GetSession())
        SendReleaseFor(plr);
}

// no check for null pointer so it must be valid
void Loot::SendReleaseFor(Player* plr)
{
    WorldPacket data(SMSG_LOOT_RELEASE_RESPONSE, (8 + 1));
    data << m_guidTarget;
    data << uint8(1);
    plr->GetSession()->SendPacket(data);
    SetPlayerIsNotLooting(plr);
}

void Loot::SendReleaseForAll()
{
    GuidSet::iterator itr = m_playersLooting.begin();
    while (itr != m_playersLooting.end())
        SendReleaseFor(*itr++);
}

void Loot::SetPlayerIsLooting(Player* player)
{
    m_playersLooting.insert(player->GetObjectGuid());      // add 'this' player as one of the players that are looting 'loot'
    player->SetLootGuid(m_guidTarget);                     // used to keep track of what loot is opened for that player
    if (m_lootType == LOOT_CORPSE || m_isChest)
    {
        player->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);
        if (m_guidTarget.IsGameObject())
            static_cast<GameObject*>(m_lootTarget)->SetInUse(true);
    }
}

void Loot::SetPlayerIsNotLooting(Player* player)
{
    m_playersLooting.erase(player->GetObjectGuid());
    player->SetLootGuid(ObjectGuid());
    if (m_lootType == LOOT_CORPSE || m_isChest)
    {
        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);
        if (m_guidTarget.IsGameObject())
            static_cast<GameObject*>(m_lootTarget)->SetInUse(false);
    }
}

void Loot::Release(Player* player)
{
    bool updateClients = false;
    if (player->GetObjectGuid() == m_currentLooterGuid)
    {
        // the owner of the loot released his item
        for (auto lootItem : m_lootItems)
        {
            // do not release blocked item (rolling ongoing)
            if (!lootItem->isBlocked && !lootItem->isReleased)
            {
                lootItem->isReleased = true;
                updateClients = true;
            }
        }
    }

    switch (m_guidTarget.GetHigh())
    {
        case HIGHGUID_GAMEOBJECT:
        {
            GameObject* go = (GameObject*) m_lootTarget;
            SetPlayerIsNotLooting(player);

            // GO is mineral vein? so it is not removed after its looted
            switch (go->GetGoType())
            {
                case GAMEOBJECT_TYPE_DOOR:
                    // locked doors are opened with spelleffect openlock, prevent remove its as looted
                    go->UseDoorOrButton();
                    break;
                case GAMEOBJECT_TYPE_CHEST:
                {
                    if (!IsLootedForAll())
                        break;

                    uint32 go_min = go->GetGOInfo()->chest.minSuccessOpens;
                    uint32 go_max = go->GetGOInfo()->chest.maxSuccessOpens;
                    bool refill = false;

                    // only vein pass this check
                    if (go_min != 0 && go_max > go_min)
                    {
                        float amount_rate = sWorld.getConfig(CONFIG_FLOAT_RATE_MINING_AMOUNT);
                        float min_amount = go_min * amount_rate;
                        float max_amount = go_max * amount_rate;

                        go->AddUse();
                        float uses = float(go->GetUseCount());
                        if (uses < max_amount)
                        {
                            if (uses >= min_amount)
                            {
                                float chance_rate = sWorld.getConfig(CONFIG_FLOAT_RATE_MINING_NEXT);

                                int32 ReqValue = 175;
                                LockEntry const* lockInfo = sLockStore.LookupEntry(go->GetGOInfo()->chest.lockId);
                                if (lockInfo)
                                    ReqValue = lockInfo->Skill[0];
                                float skill = float(player->GetSkillValue(SKILL_MINING)) / (ReqValue + 25);
                                double chance = pow(0.8 * chance_rate, 4 * (1 / double(max_amount)) * double(uses));
                                if (roll_chance_f(float(100.0f * chance + skill)))
                                    refill = true;
                            }
                            else
                                refill = true;  // 100% chance until min uses
                        }
                    }

                    if (refill)
                    {
                        // this vein have can be now refilled, as this is a vein no other player are looting it so no need to send them release
                        Clear();                // clear the content and reset some values
                        FillLoot(go->GetGOInfo()->GetLootId(), LootTemplates_Gameobject, player, false); // refill the loot with new items
                        go->SetLootState(GO_READY);
                    }
                    else
                        go->SetLootState(GO_JUST_DEACTIVATED);

                    break;
                }
                case GAMEOBJECT_TYPE_FISHINGHOLE:
                    // The fishing hole used once more
                    go->AddUse();                           // if the max usage is reached, will be despawned at next tick
                    if (go->GetUseCount() >= urand(go->GetGOInfo()->fishinghole.minSuccessOpens, go->GetGOInfo()->fishinghole.maxSuccessOpens))
                    {
                        go->SetLootState(GO_JUST_DEACTIVATED);
                    }
                    else
                        go->SetLootState(GO_READY);
                    break;
                default:
                    go->SetLootState(GO_JUST_DEACTIVATED);
                    break;
            }
            break;
        }
        case HIGHGUID_CORPSE:                               // ONLY remove insignia at BG
        {
            Corpse* corpse = (Corpse*) m_lootTarget;
            if (!corpse || !corpse->IsWithinDistInMap(player, INTERACTION_DISTANCE))
                return;

            if (IsLootedFor(player))
            {
                Clear();
                corpse->RemoveFlag(CORPSE_FIELD_DYNAMIC_FLAGS, CORPSE_DYNFLAG_LOOTABLE);
            }
            break;
        }
        case HIGHGUID_ITEM:
        {
            switch (m_lootType)
            {
                // temporary loot in stacking items, clear loot state, no auto loot move
                case LOOT_PROSPECTING:
                {
                    uint32 count = m_itemTarget->GetCount();

                    // >=5 checked in spell code, but will work for cheating cases also with removing from another stacks.
                    if (count > 5)
                        count = 5;

                    // reset loot for allow repeat looting if stack > 5
                    Clear();
                    m_itemTarget->SetLootState(ITEM_LOOT_REMOVED);

                    player->DestroyItemCount(m_itemTarget, count, true);
                    break;
                }
                // temporary loot, auto loot move
                case LOOT_DISENCHANTING:
                {
                    if (!IsLootedFor(player))
                        AutoStore(player); // can be lost if no space
                    Clear();
                    m_itemTarget->SetLootState(ITEM_LOOT_REMOVED);


					//RICHARD - ne pas delete un tas d'objet quand en en desanchante 1 
					//sauvegarde des states AVANT delete
					//WARNING : code copié collé entre  case LOOT_DISENCHANTING:   et    default:
					uint32 entryItem = m_itemTarget->GetEntry();
					uint32 countBeforeDelete = m_itemTarget->GetCount();
					uint32 bagSlot_beforeDelete = m_itemTarget->GetBagSlot();
					uint32 Slot_beforeDelete = m_itemTarget->GetSlot();
					//FIN MODIF
					///////////////////////////////////////////////////////////




                    player->DestroyItem(m_itemTarget->GetBagSlot(), m_itemTarget->GetSlot(), true);


					//RICHARD - ne pas delete un tas d'objet quand en en desanchante 2
					//on donne ce qui a été delete APRES le delete
					//WARNING : code copié collé entre  case LOOT_DISENCHANTING:   et    default:
					if ( countBeforeDelete > 1 ) // si on avait + d'un objet stacké
					{
						Item* item_AfterDelete = player->GetItemByPos(bagSlot_beforeDelete, Slot_beforeDelete);
						if ( item_AfterDelete != nullptr )
						{
							sLog.outBasic("RICHAR: error 536903");
							*((unsigned int*)0) = 0xDEAD;//on provoque un CRASH - car c'est pas normal
						}
						if ( item_AfterDelete == nullptr
							//||
							//item_AfterDelete->GetCount() < countBeforeDelete-1
							)
						{
							Item* newItem = Item::CreateItem(entryItem,  countBeforeDelete-1 );
							ItemPosCountVec sDest;
							InventoryResult msg = player->CanStoreItem(bagSlot_beforeDelete, Slot_beforeDelete, sDest, newItem, false);
							if (msg == EQUIP_ERR_OK)
							{
								player->StoreItem(sDest, newItem, true);
							}
							else
							{
								sLog.outBasic("RICHAR: error 536904");
								*((unsigned int*)0) = 0xDEAD;//on provoque un CRASH - car c'est pas normal
							}

						}
						
					}
					//FIN MODIF
					///////////////////////////////////////////////////////////




                    break;
                }
                // normal persistence loot
                default:
                {
                    // must be destroyed only if no loot
                    if (IsLootedFor(player))
                    {
                        m_itemTarget->SetLootState(ITEM_LOOT_REMOVED);



						//RICHARD - ne pas delete un objet quand en en ouvre 1 (exemple les coquillages)
						//sauvegarde des states AVANT delete
						//WARNING : code copié collé entre  case LOOT_DISENCHANTING:   et    default:
						uint32 entryItem = m_itemTarget->GetEntry();
						uint32 countBeforeDelete = m_itemTarget->GetCount();
						uint32 bagSlot_beforeDelete = m_itemTarget->GetBagSlot();
						uint32 Slot_beforeDelete = m_itemTarget->GetSlot();
						//FIN MODIF
						///////////////////////////////////////////////////////////




                        player->DestroyItem(m_itemTarget->GetBagSlot(), m_itemTarget->GetSlot(), true);
                    
					

						//RICHARD - ne pas delete un objet quand en en ouvre 2
						//on donne ce qui a été delete APRES le delete
						//WARNING : code copié collé entre  case LOOT_DISENCHANTING:   et    default:
						if ( countBeforeDelete > 1 ) // si on avait + d'un objet stacké
						{
							Item* item_AfterDelete = player->GetItemByPos(bagSlot_beforeDelete, Slot_beforeDelete);
							if ( item_AfterDelete != nullptr )
							{
								sLog.outBasic("RICHAR: error 536901");
								*((unsigned int*)0) = 0xDEAD;//on provoque un CRASH - car c'est pas normal
							}
							if ( item_AfterDelete == nullptr
								//||
								//item_AfterDelete->GetCount() < countBeforeDelete-1
								)
							{
								Item* newItem = Item::CreateItem(entryItem,  countBeforeDelete-1 );
								ItemPosCountVec sDest;
								InventoryResult msg = player->CanStoreItem(bagSlot_beforeDelete, Slot_beforeDelete, sDest, newItem, false);
								if (msg == EQUIP_ERR_OK)
								{
									player->StoreItem(sDest, newItem, true);
								}
								else
								{
									sLog.outBasic("RICHAR: error 536902");
									*((unsigned int*)0) = 0xDEAD;//on provoque un CRASH - car c'est pas normal
								}

							}
						
						}
						//FIN MODIF
						///////////////////////////////////////////////////////////








					}
                    break;
                }
            }
            return;                                         // item can be looted only single player
        }
        case HIGHGUID_UNIT:
        {
            switch (m_lootType)
            {
                case LOOT_PICKPOCKETING:
                {
                    if (IsLootedFor(player))
                    {
                        Creature* creature = (Creature*)m_lootTarget;
                        creature->SetLootStatus(CREATURE_LOOT_STATUS_PICKPOCKETED);
                    }
                    break;
                }
                case LOOT_SKINNING:
                {
                    SetPlayerIsNotLooting(player);
                    Creature* creature = (Creature*)m_lootTarget;
                    if (IsLootedFor(player))
                    {
                        creature->SetLootStatus(CREATURE_LOOT_STATUS_SKINNED);
                    }
                    else
                    {
                        // player released his skin so we can make it available for the member group or for himself
                        Group* grp = player->GetGroup();
                        m_ownerSet.clear();
                        if (grp)
                        {
                            // we need to fill m_ownerSet with player who have access to the loot
                            Group::MemberSlotList const& memberList = grp->GetMemberSlots();
                            for (const auto& memberItr : memberList)
                                m_ownerSet.insert(memberItr.guid);
                        }
                        else
                            m_ownerSet.insert(player->GetObjectGuid());
                        m_lootMethod = FREE_FOR_ALL;
                        creature->SetLootStatus(CREATURE_LOOT_STATUS_SKIN_AVAILABLE);
                        ForceLootAnimationCLientUpdate();
                    }
                    break;
                }
                case LOOT_CORPSE:
                {
                    Creature* creature = (Creature*)m_lootTarget;
                    SetPlayerIsNotLooting(player);

                    if (m_isFakeLoot)
                    {
                        SendReleaseForAll();
                        creature->SetLootStatus(CREATURE_LOOT_STATUS_LOOTED);
                        m_lootTarget->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
                        break;
                    }

                    if (IsLootedForAll())
                    {
                        SendReleaseForAll();
                        creature->SetLootStatus(CREATURE_LOOT_STATUS_LOOTED);
                    }
                    else if (updateClients)
                    {
                        // player have released the corpse and some loot still available, we need to resend loot flags for each players to provide them remaininig loot access
                        ForceLootAnimationCLientUpdate();
                    }

                    break;
                }
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
}

// Popup windows with loot content
void Loot::ShowContentTo(Player* plr)
{
    if (!m_isChest)
    {
        // for item loot that might be empty we should not display error but instead send empty loot window
        if (!m_lootItems.empty() && !CanLoot(plr))
        {
            SendReleaseFor(plr);
            sLog.outError("Loot::ShowContentTo()> %s is trying to open a loot without credential", plr->GetGuidStr().c_str());
            return;
        }

        // add this player to the the openers list of this loot
        m_playersOpened.emplace(plr->GetObjectGuid());
    }
    else
    {
        if (static_cast<GameObject*>(m_lootTarget)->IsInUse())
        {
            SendReleaseFor(plr);
            return;
        }

        if (m_ownerSet.find(plr->GetObjectGuid()) == m_ownerSet.end())
            SetGroupLootRight(plr);
    }

    if (m_lootMethod != NOT_GROUP_TYPE_LOOT && !m_isChecked)
    {
        GroupCheck();
        switch (m_lootMethod)
        {
            case NEED_BEFORE_GREED:
            case GROUP_LOOT:
            {
                CheckIfRollIsNeeded(plr);               // check if there is the need to start a roll
                break;
            }
            default:
                break;
        }
    }

    WorldPacket data(SMSG_LOOT_RESPONSE);
    data << m_guidTarget;
    data << uint8(m_clientLootType);

    GetLootContentFor(plr, data);                           // fill the data with items contained in the loot (may be empty)
    SetPlayerIsLooting(plr);
    if (m_lootTarget)
        m_lootTarget->InspectingLoot();

    plr->SendDirectMessage(data);
}

void Loot::GroupCheck()
{
    m_isChecked = true;

    PlayerList playerList;
    Player* masterLooter = nullptr;
    for (auto playerGuid : m_ownerSet)
    {
        Player* player = sObjectAccessor.FindPlayer(playerGuid);
        if (!player)
            continue;

        if (!player->GetSession())
            continue;

        playerList.emplace_back(player);

        if (m_lootMethod == MASTER_LOOT && !masterLooter && playerGuid == m_masterOwnerGuid)
            masterLooter = player;
    }

    // in master loot case we have to send looter list to client
    if (masterLooter)
    {
        WorldPacket data(SMSG_LOOT_MASTER_LIST);
        data << uint8(playerList.size());
        for (auto itr : playerList)
            data << itr->GetObjectGuid();
        masterLooter->GetSession()->SendPacket(data);
    }
}

// check if there is need to launch a roll
void Loot::CheckIfRollIsNeeded(Player const* plr)
{
    if (!plr)
        return;

    for (auto lootItem : m_lootItems)
    {
        if (!lootItem->isBlocked)
            continue;

        if (lootItem->IsAllowed(plr, this))
        {
            uint32 itemSlot = lootItem->lootSlot;
            if (!m_roll[itemSlot].TryToStart(*this, itemSlot))      // Create and try to start a roll
                m_roll.erase(m_roll.find(itemSlot));                // Cannot start roll so we have to delete it (find will not fail as the item was just created)
        }
    }
}

bool IsEligibleForLoot(Player* looter, WorldObject* lootTarget)
{
    if (looter->IsAtGroupRewardDistance(lootTarget))
        return true;

    if (lootTarget->GetTypeId() == TYPEID_UNIT)
    {
        Unit* creature = (Unit*)lootTarget;
        return creature->getThreatManager().HasThreat(looter);
    }

    return false;
}

// Set the player who have right for this loot
void Loot::SetGroupLootRight(Player* player)
{
    if (m_isChest && !m_ownerSet.empty())
    {
        // chest was already opened so we dont have to change group type
        m_ownerSet.emplace(player->GetObjectGuid());
        return;
    }

    m_ownerSet.clear();
    Group* grp = player->GetGroup();
    if (grp && (!m_isChest || static_cast<GameObject*>(m_lootTarget)->GetGOInfo()->chest.groupLootRules))
    {
        m_lootMethod = grp->GetLootMethod();
        m_threshold = grp->GetLootThreshold();

        // we need to fill m_ownerSet with player who have access to the loot
        Group::MemberSlotList const& memberList = grp->GetMemberSlots();
        ObjectGuid currentLooterGuid = grp->GetCurrentLooterGuid();
        GuidList ownerList;                 // used to keep order of the player (important to get correctly next looter)

        // current looter must be in the group
        Group::MemberSlotList::const_iterator currentLooterItr;
        for (currentLooterItr = memberList.begin(); currentLooterItr != memberList.end(); ++currentLooterItr)
            if (currentLooterItr->guid == currentLooterGuid)
                break;

        // if not then assign first player in group
        if (currentLooterItr == memberList.end())
        {
            currentLooterItr = memberList.begin();
            currentLooterGuid = currentLooterItr->guid;
            grp->SetNextLooterGuid(currentLooterGuid);
        }

        // now that we get a valid current looter iterator we can start to check the loot owner
        Group::MemberSlotList::const_iterator itr = currentLooterItr;
        do
        {
            ++itr;                              // we start by next current looter position in list that way we have directly next looter in result ownerSet (if any exist)
            if (itr == memberList.end())
                itr = memberList.begin();       // reached the end of the list is possible so simply restart from the first element in it

            Player* looter = ObjectAccessor::FindPlayer(itr->guid);

            if (!looter)
                continue;

            if (IsEligibleForLoot(looter, m_lootTarget))
            {
                m_ownerSet.insert(itr->guid);   // save this guid to main owner set
                ownerList.push_back(itr->guid); // save this guid to local ordered GuidList (we need to keep it ordered only here)

                // get enchant skill of authorized looter
                uint32 enchantSkill = looter->GetSkillValue(SKILL_ENCHANTING);
                if (m_maxEnchantSkill < enchantSkill)
                    m_maxEnchantSkill = enchantSkill;
            }

        }
        while (itr != currentLooterItr);

        if (m_lootMethod == MASTER_LOOT)
        {
            m_masterOwnerGuid = grp->GetMasterLooterGuid();
            // Set group method to GROUP_LOOT if no master loot found
            if (m_ownerSet.find(m_masterOwnerGuid) == m_ownerSet.end())
                m_lootMethod = GROUP_LOOT;
        }

        // if more than one player have right to loot than we have to handle group method, round robin, roll, etc..
        if (m_ownerSet.size() > 1 && m_lootMethod != FREE_FOR_ALL)
        {
            // is current looter have right for this loot?
            if (m_ownerSet.find(currentLooterGuid) == m_ownerSet.end())
            {
                // as owner list is filled from NEXT current looter position we can assign first element in list as looter and next element in list as next looter
                m_currentLooterGuid = ownerList.front();                        // set first player who have right to loot as current looter
                grp->SetNextLooterGuid(*(++ownerList.begin()));                 // set second player who have right as next looter
            }
            else
            {
                // as owner set is filled from NEXT current looter position we can assign first element in list as next looter
                m_currentLooterGuid = currentLooterGuid;
                grp->SetNextLooterGuid(ownerList.front());                      // set first player who have right as next looter
            }

            SendAllowedLooter();
            m_isChecked = false;
            return;
        }
        m_currentLooterGuid = player->GetObjectGuid();
        SendAllowedLooter();
    }

    m_ownerSet.insert(player->GetObjectGuid());
    m_lootMethod = NOT_GROUP_TYPE_LOOT;
}

Loot::Loot(Player* player, Creature* creature, LootType type) :
    m_lootTarget(nullptr), m_gold(0), m_maxSlot(0), m_lootType(type),
    m_clientLootType(CLIENT_LOOT_CORPSE), m_lootMethod(NOT_GROUP_TYPE_LOOT), m_threshold(ITEM_QUALITY_UNCOMMON), m_maxEnchantSkill(0), m_haveItemOverThreshold(false),
    m_isChecked(false), m_isChest(false), m_isChanged(false), m_isFakeLoot(false)
{


	m_richard_timeCreated = WorldTimer::getMSTime();
	WorldSession::g_wantLoot[m_richard_timeCreated] = WorldSession::RICHARD_TRY_LOOT_WANT(); //creation de l'election pour ce loot


    // the player whose group may loot the corpse
    if (!player)
    {
        sLog.outError("LootMgr::CreateLoot> Error cannot get looter info to create loot!");
        return;
    }

    if (!creature)
    {
        sLog.outError("Loot::CreateLoot> cannot create loot, no creature passed!");
        return;
    }

    m_lootTarget = creature;
    m_guidTarget = creature->GetObjectGuid();
    CreatureInfo const* creatureInfo = creature->GetCreatureInfo();

    switch (type)
    {
        case LOOT_CORPSE:
        {
            // setting loot right
            SetGroupLootRight(player);
            m_clientLootType = CLIENT_LOOT_CORPSE;

            if ((creatureInfo->LootId && FillLoot(creatureInfo->LootId, LootTemplates_Creature, player, false)) || creatureInfo->MaxLootGold > 0)
            {





                
				////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				//
				// RICHARD :  reduction des PO lootées par les mobs
				//
				// #REDUIT_TES_PO
				//
				// original :
				// GenerateMoneyLoot(creatureInfo->MinLootGold, creatureInfo->MaxLootGold);
				// nouveau :
				{
					float difficultyDonjon = 0.0;
					int nbPlayerThatShouldKillThisCreature = 1; // nombre original de joueurs équilibré par Blizzard
					Creature::GetRichardModForMap(  creature->m_richar_lieuOrigin , creature->GetName(),  creature->GetOwner() , &difficultyDonjon, &nbPlayerThatShouldKillThisCreature, NULL  );


					float newMin = (float)creatureInfo->MinLootGold;
					float newMax = (float)creatureInfo->MaxLootGold;

					/*

					// ANCIENNE METHODE :

					// 1) deja, on multiplie par la difficulté du perso :
					if ( creature->Richar_difficuly_health > 0.0f )
					{
						newMin *= creature->Richar_difficuly_health;
						newMax *= creature->Richar_difficuly_health;
					}

					// 2) et en plus de ca, on va rajouter une sécurite, car les gros boss de raid lootent de base
					//plusieurs centaines de PO.
					//donc meme divisé par la difficulté ca peut toujours faire une grosse somme
					//info un mob Elite 60 hors donjon, va looter en gros 30 silver
					//apres discussion avec Diane, on va partir sur une limite de 50 PA max looté
					//je divise cette limite par 1.5, puisque elle va etre remultiplié derriere.
					const float limitt = 50.0f * 100.0f /  sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY);
					if ( newMin > limitt )
					{
						newMin = limitt;
					}
					
					
					*/


					//nouvelle methode :
					//pourquoi ne pas simplement diviser par le nombre de joueurs qui doivent tuer le mob.
					//et remultiplier par 2, car dans la majorité des cas, on est 2 joueurs.
					//par exemple, tous les mob de Naxramass, auront un taux de loot de  /20
					//
					//Je trouve ca mieux, plutot que comme je faisais avant : multiplier par le niveau de difficulté et mettre une limite de 50PA.
					//surtout que depuis qq temps, le niveau de difficulté, peut etre plus grand que 1.0. Il n'a plus aucun lien avec le nombre de joueurs prévu par le donjon.
					//La au moins avec ma nouvelle methode, on va avoir les PO a la meme vitesse que le Wow de Blizzard original.
					//
					if ( nbPlayerThatShouldKillThisCreature <= 0 )
					{
						//ceci ne devrait jamais arriver, mais on sait jamais ....
						BASIC_LOG("RICHAR: ERROR ERRROOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOORRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR");
						nbPlayerThatShouldKillThisCreature = 1;
					}

					float coeffReduce = 2.0f / (float)nbPlayerThatShouldKillThisCreature;
					if ( coeffReduce > 1.0f ) // le coeff ne doit que faire reduire les PO
						coeffReduce = 1.0;

					newMin *= coeffReduce;
					newMax *= coeffReduce;


					uint32 newMin_int = (uint32)newMin;
					uint32 newMax_int = (uint32)newMax;

					if ( newMax_int < newMin_int )
					{
						newMax_int = newMin_int;
					}

					// 3) regle d'exception qui va s'appliquer en gros uniquement a 47 mobs dans le jeu
					//    ( correspondant en majorité a des gros boss
					// si le mob de base lootait > 10 PO, alors on le ramene a 5 PO de limite
					if ( creatureInfo->MinLootGold > 10 * 100 * 100 )
					{
						newMin_int = (5 * 100 * 100) /  sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY);
						newMax_int = newMin_int;
					}


					if (   newMin_int != creatureInfo->MinLootGold
						|| newMax_int != creatureInfo->MaxLootGold
						)
					{
						
						if ( 
							(float)creatureInfo->MinLootGold * (float)sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY)
							<= 10.0f*100.0f  // en dessous de 10 PA on parle en PC
							)
						{
							int a = (creatureInfo->MinLootGold * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));
							int b = (creatureInfo->MaxLootGold * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));
							int c = (newMin_int * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));
							int d = newMax_int * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY);

							BASIC_LOG("RICHAR: loop de PO de NPC modifie %d-%d PC  -->  %d-%d PC ",a,b,c,d);

						}
						else
						{
							int a = (int)((float)creatureInfo->MinLootGold * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY) / 100.0f);
							int b = (int)((float)creatureInfo->MaxLootGold * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY) / 100.0f);
							int c = (int)((float)newMin_int * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY) / 100.0f);
							int d = (int)((float)newMax_int * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY) / 100.0f);

							BASIC_LOG("RICHAR: loop de PO de NPC modifie %d-%d PA  -->  %d-%d PA ",a,b,c,d);
						}
					}

					GenerateMoneyLoot( newMin_int , newMax_int );
				}
				////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				
				










				
				

                // loot may be anyway empty (loot may be empty or contain items that no one have right to loot)
                bool isLootedForAll = IsLootedForAll();
                if (isLootedForAll)
                {
                    // show sometimes an empty window
                    if (sWorld.getConfig(CONFIG_BOOL_CORPSE_EMPTY_LOOT_SHOW) && urand(0, 2) == 1)
                    {
                        m_isFakeLoot = true;
                        isLootedForAll = false;
                    }
                }

                if (!isLootedForAll)
                    creature->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
                else
                    creature->SetLootStatus(CREATURE_LOOT_STATUS_LOOTED);
                ForceLootAnimationCLientUpdate();
                break;
            }

            sLog.outDebug("Loot::CreateLoot> cannot create corpse loot, FillLoot failed with loot id(%u)!", creatureInfo->LootId);
            creature->SetLootStatus(CREATURE_LOOT_STATUS_LOOTED);
            break;
        }
        case LOOT_PICKPOCKETING:
        {
            m_clientLootType = CLIENT_LOOT_PICKPOCKETING;

            if (!creature->isAlive() || player->getClass() != CLASS_ROGUE)
                return;

            // setting loot right
            m_ownerSet.insert(player->GetObjectGuid());
            m_lootMethod = NOT_GROUP_TYPE_LOOT;

            if (!creatureInfo->PickpocketLootId || !FillLoot(creatureInfo->PickpocketLootId, LootTemplates_Pickpocketing, player, false))
            {
                sLog.outError("Loot::CreateLoot> cannot create pickpocket loot, FillLoot failed with loot id(%u)!", creatureInfo->PickpocketLootId);
                return;
            }

            // Generate extra money for pick pocket loot
            const uint32 a = urand(0, creature->getLevel() / 2);
            const uint32 b = urand(0, player->getLevel() / 2);
            m_gold = uint32(10 * (a + b) * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));

            break;
        }
        case LOOT_SKINNING:
        {
            // setting loot right
            m_ownerSet.insert(player->GetObjectGuid());
            m_clientLootType = CLIENT_LOOT_PICKPOCKETING;
            m_lootMethod = NOT_GROUP_TYPE_LOOT;
            if (!creatureInfo->SkinningLootId || !FillLoot(creatureInfo->SkinningLootId, LootTemplates_Skinning, player, false))
            {
                sLog.outError("Loot::CreateLoot> cannot create skinning loot, FillLoot failed with loot id(%u)!", creatureInfo->SkinningLootId);
                return;
            }
            break;
        }
        default:
            sLog.outError("Loot::CreateLoot> Cannot create loot for %s with invalid LootType(%u)", creature->GetGuidStr().c_str(), uint32(type));
            break;
    }

    return;
}

Loot::Loot(Player* player, GameObject* gameObject, LootType type) :
    m_lootTarget(nullptr), m_gold(0), m_maxSlot(0), m_lootType(type),
    m_clientLootType(CLIENT_LOOT_CORPSE), m_lootMethod(NOT_GROUP_TYPE_LOOT), m_threshold(ITEM_QUALITY_UNCOMMON), m_maxEnchantSkill(0), m_haveItemOverThreshold(false),
    m_isChecked(false), m_isChest(false), m_isChanged(false), m_isFakeLoot(false)
{

	m_richard_timeCreated = WorldTimer::getMSTime();
	WorldSession::g_wantLoot[m_richard_timeCreated] = WorldSession::RICHARD_TRY_LOOT_WANT(); //creation de l'election pour ce loot



    // the player whose group may loot the corpse
    if (!player)
    {
        sLog.outError("LootMgr::CreateLoot> Error cannot get looter info to create loot!");
        return;
    }

    if (!gameObject)
    {
        sLog.outError("Loot::CreateLoot> cannot create game object loot, no game object passed!");
        return;
    }

    m_lootTarget = gameObject;
    m_guidTarget = gameObject->GetObjectGuid();

    // not check distance for GO in case owned GO (fishing bobber case, for example)
    // And permit out of range GO with no owner in case fishing hole
    if ((type != LOOT_FISHINGHOLE &&
            ((type != LOOT_FISHING && type != LOOT_FISHING_FAIL) || gameObject->GetOwnerGuid() != player->GetObjectGuid()) &&
            !gameObject->IsWithinDistInMap(player, INTERACTION_DISTANCE)))
    {
        sLog.outError("Loot::CreateLoot> cannot create game object loot, basic check failed!");
        return;
    }

    // generate loot only if ready for open and spawned in world
    if (gameObject->GetLootState() == GO_READY && gameObject->IsSpawned())
    {
        if ((gameObject->GetEntry() == BG_AV_OBJECTID_MINE_N || gameObject->GetEntry() == BG_AV_OBJECTID_MINE_S))
        {
            if (BattleGround* bg = player->GetBattleGround())
                if (bg->GetTypeID() == BATTLEGROUND_AV)
                    if (!(((BattleGroundAV*)bg)->PlayerCanDoMineQuest(gameObject->GetEntry(), player->GetTeam())))
                    {
                        return;
                    }
        }

        switch (type)
        {
            case LOOT_FISHING_FAIL:
            {
                // setting loot right
                m_ownerSet.insert(player->GetObjectGuid());
                m_lootMethod = NOT_GROUP_TYPE_LOOT;
                m_clientLootType = CLIENT_LOOT_FISHING;

                // Entry 0 in fishing loot template used for store junk fish loot at fishing fail it junk allowed by config option
                // this is overwrite fishinghole loot for example
                FillLoot(0, LootTemplates_Fishing, player, true);
                break;
            }
            case LOOT_FISHING:
            {
                // setting loot right
                m_ownerSet.insert(player->GetObjectGuid());
                m_lootMethod = NOT_GROUP_TYPE_LOOT;
                m_clientLootType = CLIENT_LOOT_FISHING;

                uint32 zone, subzone;
                gameObject->GetZoneAndAreaId(zone, subzone);
                // if subzone loot exist use it

               















				/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				//   RICHARD   -   repare le fishing qui peut donner 0 poissons
				//   voir le ticket que j'ai créé :  894

				// original :
				// if (!     FillLoot(  0,0,     subzone, LootTemplates_Fishing, player, true, (subzone != zone))        && subzone != zone)
				//      // else use zone loot (if zone diff. from subzone, must exist in like case)
				//     FillLoot(  0,0,     zone, LootTemplates_Fishing, player, true);
				{
				bool fillLoot1 = FillLoot(subzone, LootTemplates_Fishing, player, true, (subzone != zone));
				int nbFish1 = m_lootItems.size();
				if (nbFish1 == 0 || (!fillLoot1  &&   subzone != zone))
				{
					FillLoot(zone, LootTemplates_Fishing, player, true);
					int nbFish2 = m_lootItems.size();
					int aaaaa = 0;

					BASIC_LOG("RICHAR: peche de %d poissons dans la zone %d  (no data for SUBzone=%d)", nbFish2, zone, subzone);
				}
				else
				{
					BASIC_LOG("RICHAR: peche de %d poissons dans la SUBzone %d   (no need to loot zone=%d)", nbFish1, subzone, zone);
				}
				}	
				/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
















                if (!FillLoot(subzone, LootTemplates_Fishing, player, true, (subzone != zone)) && subzone != zone)
                    // else use zone loot (if zone diff. from subzone, must exist in like case)
                    FillLoot(zone, LootTemplates_Fishing, player, true);
                break;
            }
            default:
            {
                if (uint32 lootid = gameObject->GetGOInfo()->GetLootId())
                {
                    if (gameObject->GetGOInfo()->type == GAMEOBJECT_TYPE_CHEST)
                        m_isChest = true;

                    SetGroupLootRight(player);
                    FillLoot(lootid, LootTemplates_Gameobject, player, false);
                    GenerateMoneyLoot(gameObject->GetGOInfo()->MinMoneyLoot, gameObject->GetGOInfo()->MaxMoneyLoot);

                    if (m_lootType == LOOT_FISHINGHOLE)
                        m_clientLootType = CLIENT_LOOT_FISHING;
                    else
                        m_clientLootType = CLIENT_LOOT_PICKPOCKETING;
                }
                break;
            }
        }
        gameObject->SetLootState(GO_ACTIVATED);
    }
    return;
}

Loot::Loot(Player* player, Corpse* corpse, LootType type) :
    m_lootTarget(nullptr), m_gold(0), m_maxSlot(0), m_lootType(type),
    m_clientLootType(CLIENT_LOOT_CORPSE), m_lootMethod(NOT_GROUP_TYPE_LOOT), m_threshold(ITEM_QUALITY_UNCOMMON), m_maxEnchantSkill(0), m_haveItemOverThreshold(false),
    m_isChecked(false), m_isChest(false), m_isChanged(false), m_isFakeLoot(false)
{


	m_richard_timeCreated = WorldTimer::getMSTime();
	WorldSession::g_wantLoot[m_richard_timeCreated] = WorldSession::RICHARD_TRY_LOOT_WANT(); //creation de l'election pour ce loot



    // the player whose group may loot the corpse
    if (!player)
    {
        sLog.outError("LootMgr::CreateLoot> Error cannot get looter info to create loot!");
        return;
    }

    if (!corpse)
    {
        sLog.outError("Loot::CreateLoot> cannot create corpse loot, no corpse passed!");
        return;
    }

    m_lootTarget = corpse;
    m_guidTarget = corpse->GetObjectGuid();

    if (type != LOOT_INSIGNIA || corpse->GetType() == CORPSE_BONES)
        return;

    if (!corpse->lootForBody)
    {
        corpse->lootForBody = true;
        uint32 pLevel;
        if (Player* plr = sObjectAccessor.FindPlayer(corpse->GetOwnerGuid()))
            pLevel = plr->getLevel();
        else
            pLevel = player->getLevel(); // TODO:: not correct, need to save real player level in the corpse data in case of logout

         m_ownerSet.insert(player->GetObjectGuid());
         m_lootMethod = NOT_GROUP_TYPE_LOOT;
         m_clientLootType = CLIENT_LOOT_CORPSE;

        if (player->GetBattleGround()->GetTypeID() == BATTLEGROUND_AV)
            FillLoot(0, LootTemplates_Creature, player, false);

        // It may need a better formula
        // Now it works like this: lvl10: ~6copper, lvl70: ~9silver
        m_gold = (uint32)(urand(50, 150) * 0.016f * pow(((float)pLevel) / 5.76f, 2.5f) * sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));
    }
    return;
}

Loot::Loot(Player* player, Item* item, LootType type) :
    m_lootTarget(nullptr), m_gold(0), m_maxSlot(0), m_lootType(type),
    m_clientLootType(CLIENT_LOOT_CORPSE), m_lootMethod(NOT_GROUP_TYPE_LOOT), m_threshold(ITEM_QUALITY_UNCOMMON), m_maxEnchantSkill(0), m_haveItemOverThreshold(false),
    m_isChecked(false), m_isChest(false), m_isChanged(false), m_isFakeLoot(false)
{

	m_richard_timeCreated = WorldTimer::getMSTime();
	WorldSession::g_wantLoot[m_richard_timeCreated] = WorldSession::RICHARD_TRY_LOOT_WANT(); //creation de l'election pour ce loot



    // the player whose group may loot the corpse
    if (!player)
    {
        sLog.outError("LootMgr::CreateLoot> Error cannot get looter info to create loot!");
        return;
    }

    if (!item)
    {
        sLog.outError("Loot::CreateLoot> cannot create item loot, no item passed!");
        return;
    }

    m_itemTarget = item;
    m_guidTarget = item->GetObjectGuid();

    m_ownerSet.insert(player->GetObjectGuid());
    m_lootMethod = NOT_GROUP_TYPE_LOOT;
    m_clientLootType = CLIENT_LOOT_PICKPOCKETING;
    switch (type)
    {
        case LOOT_DISENCHANTING:
            FillLoot(item->GetProto()->DisenchantID, LootTemplates_Disenchant, player, true);
            item->SetLootState(ITEM_LOOT_TEMPORARY);
            break;
        default:
            FillLoot(item->GetEntry(), LootTemplates_Item, player, true, item->GetProto()->MaxMoneyLoot == 0);
            GenerateMoneyLoot(item->GetProto()->MinMoneyLoot, item->GetProto()->MaxMoneyLoot);
            item->SetLootState(ITEM_LOOT_CHANGED);
            break;
    }
    return;
}

Loot::Loot(Unit* unit, Item* item) :
    m_lootTarget(nullptr), m_itemTarget(item), m_gold(0), m_maxSlot(0),
    m_lootType(LOOT_SKINNING), m_clientLootType(CLIENT_LOOT_PICKPOCKETING), m_lootMethod(NOT_GROUP_TYPE_LOOT), m_threshold(ITEM_QUALITY_UNCOMMON), m_maxEnchantSkill(0),
    m_haveItemOverThreshold(false), m_isChecked(false), m_isChest(false), m_isChanged(false), m_isFakeLoot(false)
{

	m_richard_timeCreated = WorldTimer::getMSTime();
	WorldSession::g_wantLoot[m_richard_timeCreated] = WorldSession::RICHARD_TRY_LOOT_WANT(); //creation de l'election pour ce loot


    m_ownerSet.insert(unit->GetObjectGuid());
    m_guidTarget = item->GetObjectGuid();
}

Loot::Loot(Player* player, uint32 id, LootType type) :
    m_lootTarget(nullptr), m_gold(0), m_maxSlot(0), m_lootType(type),
    m_clientLootType(CLIENT_LOOT_CORPSE), m_lootMethod(NOT_GROUP_TYPE_LOOT), m_threshold(ITEM_QUALITY_UNCOMMON), m_maxEnchantSkill(0), m_haveItemOverThreshold(false),
    m_isChecked(false), m_isChest(false), m_isChanged(false), m_isFakeLoot(false)
{

	m_richard_timeCreated = WorldTimer::getMSTime();
	WorldSession::g_wantLoot[m_richard_timeCreated] = WorldSession::RICHARD_TRY_LOOT_WANT(); //creation de l'election pour ce loot




    m_ownerSet.insert(player->GetObjectGuid());
    switch (type)
    {
        case LOOT_MAIL:
            FillLoot(id, LootTemplates_Mail, player, true, true);
            m_clientLootType = CLIENT_LOOT_PICKPOCKETING;
            break;
        case LOOT_SKINNING:
            FillLoot(id, LootTemplates_Skinning, player, true, true);
            m_clientLootType = CLIENT_LOOT_PICKPOCKETING;
            break;
        default:
            sLog.outError("Loot::Loot> invalid loot type passed to loot constructor.");
            break;
    }
}

void Loot::SendAllowedLooter()
{
    if (m_lootMethod == FREE_FOR_ALL || m_lootMethod == NOT_GROUP_TYPE_LOOT)
        return;

    WorldPacket data(SMSG_LOOT_LIST);
    data << GetLootGuid();

    if (m_lootMethod == MASTER_LOOT)
        data << m_masterOwnerGuid.WriteAsPacked();
    else
        data << uint8(0);

    data << m_currentLooterGuid.WriteAsPacked();

    for (auto itr : m_ownerSet)
        if (Player* plr = ObjectAccessor::FindPlayer(itr))
            plr->GetSession()->SendPacket(data);
}

InventoryResult Loot::SendItem(Player* target, uint32 itemSlot)
{
    LootItem* lootItem = GetLootItemInSlot(itemSlot);
    return SendItem(target, lootItem);
}

InventoryResult Loot::SendItem(Player* target, LootItem* lootItem)
{
    if (!lootItem)
    {
        Release(target);
        return EQUIP_ERR_ITEM_NOT_FOUND;
    }

    bool playerGotItem = false;
    InventoryResult msg = EQUIP_ERR_CANT_DO_RIGHT_NOW;
    if (target && target->GetSession())
    {
        ItemPosCountVec dest;
        msg = target->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, lootItem->itemId, lootItem->count);
        if (msg == EQUIP_ERR_OK)
        {
            Item* newItem = target->StoreNewItem(dest, lootItem->itemId, true, lootItem->randomPropertyId);

            if (lootItem->freeForAll)
            {
                NotifyItemRemoved(target, lootItem->lootSlot);
                sLog.outDebug("This item is free for all!!");
            }
            else
                NotifyItemRemoved(lootItem->lootSlot);

            target->SendNewItem(newItem, uint32(lootItem->count), false, false, true);

            if (!m_isChest)
            {
                // for normal loot the players right was set at loot filling so we just have to remove from allowed guids
                if (lootItem->freeForAll)
                    lootItem->allowedGuid.erase(target->GetObjectGuid());
                else
                    lootItem->allowedGuid.clear();
            }
            else
            {
                // for chest as the allowed guid should be empty we will add the looter guid so that mean it was looted from target
                lootItem->allowedGuid.emplace(target->GetObjectGuid());
            }

            playerGotItem = true;
            m_isChanged = true;
        }
        else
            target->SendEquipError(msg, nullptr, nullptr, lootItem->itemId);
    }

    if (!playerGotItem)
    {
        // an error occurred player didn't received his loot
        lootItem->isBlocked = false;                                  // make the item available (was blocked since roll started)
        m_currentLooterGuid = target->GetObjectGuid();                // change looter guid to let only him right to loot
        lootItem->isReleased = false;                                 // be sure the loot was not already released by another player
        SendAllowedLooter();                                          // update the looter right for client
    }
    else
    {
        if (IsLootedForAll())
        {
            SendReleaseForAll();
            if (m_isChest)
            {
                GameObject* go = (GameObject*)m_lootTarget;
                uint32 go_min = go->GetGOInfo()->chest.minSuccessOpens;
                uint32 go_max = go->GetGOInfo()->chest.maxSuccessOpens;
                // only vein pass this check
                if (go_min != 0 && go_max > go_min)
                {
                    // nothing to do refill is handled in Loot::Release()
                }
                else
                    go->SetLootState(GO_JUST_DEACTIVATED);
            }
        }
        else if (IsLootedFor(target))
            SendReleaseFor(target);
        ForceLootAnimationCLientUpdate();
    }
    return msg;
}

bool Loot::AutoStore(Player* player, bool broadcast /*= false*/, uint32 bag /*= NULL_BAG*/, uint32 slot /*= NULL_SLOT*/)
{
    bool result = true;
    for (LootItemList::const_iterator lootItemItr = m_lootItems.begin(); lootItemItr != m_lootItems.end(); ++lootItemItr)
    {
        LootItem* lootItem = *lootItemItr;

        if (!lootItem->IsAllowed(player, this))
            continue; // already looted or not allowed

        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(bag, slot, dest, lootItem->itemId, lootItem->count);
        if (msg != EQUIP_ERR_OK && slot != NULL_SLOT)
            msg = player->CanStoreNewItem(bag, NULL_SLOT, dest, lootItem->itemId, lootItem->count);
        if (msg != EQUIP_ERR_OK && bag != NULL_BAG)
            msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, lootItem->itemId, lootItem->count);
        if (msg != EQUIP_ERR_OK)
        {
            player->SendEquipError(msg, nullptr, nullptr, lootItem->itemId);
            result = false;
            continue;
        }

        if (lootItem->freeForAll)
            lootItem->allowedGuid.erase(player->GetObjectGuid());
        else
            lootItem->allowedGuid.clear();

        Item* pItem = player->StoreNewItem(dest, lootItem->itemId, true, lootItem->randomPropertyId);
        player->SendNewItem(pItem, lootItem->count, false, false, broadcast);
        m_isChanged = true;
    }

    return result;
}

void Loot::Update()
{
    m_isChanged = false;
    GroupLootRollMap::iterator itr = m_roll.begin();
    while (itr != m_roll.end())
    {
        if (itr->second.UpdateRoll())
            m_roll.erase(itr++);
        else
            ++itr;
    }
}

void Loot::ForceLootAnimationCLientUpdate() const
{
    if (m_guidTarget.IsCreature() && m_lootTarget)
        m_lootTarget->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
}

// will return the pointer of item in loot slot provided without any right check
LootItem* Loot::GetLootItemInSlot(uint32 itemSlot)
{
    for (auto lootItem : m_lootItems)
    {
        if (lootItem->lootSlot == itemSlot)
            return lootItem;
    }
    return nullptr;
}

// Will return available loot item for specific player. Use only for own loot like loot in item and mail
void Loot::GetLootItemsListFor(Player* player, LootItemList& lootList)
{
    for (LootItemList::const_iterator lootItemItr = m_lootItems.begin(); lootItemItr != m_lootItems.end(); ++lootItemItr)
    {
        LootItem* lootItem = *lootItemItr;
        if (!lootItem->IsAllowed(player, this))
            continue; // already looted or player have no right to see/loot this item

        lootList.push_back(lootItem);
    }
}

Loot::~Loot()
{
    SendReleaseForAll();
    for (auto& m_lootItem : m_lootItems)
        delete m_lootItem;
}

void Loot::Clear()
{
    for (auto& m_lootItem : m_lootItems)
        delete m_lootItem;
    m_lootItems.clear();
    m_playersLooting.clear();
    m_gold = 0;
    m_ownerSet.clear();
    m_masterOwnerGuid.Clear();
    m_currentLooterGuid.Clear();
    m_roll.clear();
    m_maxEnchantSkill = 0;
    m_haveItemOverThreshold = false;
    m_isChecked = false;
    m_maxSlot = 0;
}

// only used from explicitly loaded loot
void Loot::SetGoldAmount(uint32 _gold)
{
    if (m_lootType == LOOT_SKINNING)
        m_gold = _gold;
}

void Loot::SendGold(Player* player)
{
    NotifyMoneyRemoved();

    if (m_lootMethod != NOT_GROUP_TYPE_LOOT)           // item can be looted only single player
    {
        uint32 money_per_player = uint32(m_gold / (m_ownerSet.size()));

        for (auto itr : m_ownerSet)
        {
            Player* plr = sObjectMgr.GetPlayer(itr);
            if (!plr || !plr->GetSession())
                continue;

            plr->ModifyMoney(money_per_player);

            WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4);
            data << uint32(money_per_player);

            plr->GetSession()->SendPacket(data);
        }
    }
    else
    {
        player->ModifyMoney(m_gold);

        if (m_guidTarget.IsItem())
        {
            if (Item* item = player->GetItemByGuid(m_guidTarget))
                item->SetLootState(ITEM_LOOT_CHANGED);
        }
    }
    m_gold = 0;

    if (IsLootedFor(player))
        Release(player);
    ForceLootAnimationCLientUpdate();
}

bool Loot::IsItemAlreadyIn(uint32 itemId) const
{
    for (auto lootItem : m_lootItems)
    {
        if (lootItem->itemId == itemId)
            return true;
    }
    return false;
}

void Loot::PrintLootList(ChatHandler& chat, WorldSession* session) const
{
    if (!session)
    {
        chat.SendSysMessage("Error you have to be in game for this command.");
        return;
    }

    if (m_gold == 0)
        chat.PSendSysMessage("Loot have no money");
    else
        chat.PSendSysMessage("Loot have (%u)coppers", m_gold);

    if (m_lootItems.empty())
    {
        chat.PSendSysMessage("Loot have no item.");
        return;
    }

    for (auto lootItem : m_lootItems)
    {
        uint32 itemId = lootItem->itemId;
        ItemPrototype const* pProto = sItemStorage.LookupEntry<ItemPrototype >(itemId);
        if (!pProto)
            continue;

        int loc_idx = session->GetSessionDbLocaleIndex();

        std::string name = pProto->Name1;
        sObjectMgr.GetItemLocaleStrings(itemId, loc_idx, &name);
        std::string count = "x" + std::to_string(lootItem->count);
        chat.PSendSysMessage(LANG_ITEM_LIST_CHAT, itemId, itemId, name.c_str(), count.c_str());
    }
}

// fill in the bytebuffer with loot content for specified player
void Loot::GetLootContentFor(Player* player, ByteBuffer& buffer)
{
    uint8 itemsShown = 0;

    // gold
    buffer << uint32(m_gold);

    size_t count_pos = buffer.wpos();                            // pos of item count byte
    buffer << uint8(0);                                          // item count placeholder

    for (LootItemList::const_iterator lootItemItr = m_lootItems.begin(); lootItemItr != m_lootItems.end(); ++lootItemItr)
    {
        LootItem* lootItem = *lootItemItr;
        LootSlotType slot_type = lootItem->GetSlotTypeForSharedLoot(player, this);
        if (slot_type >= MAX_LOOT_SLOT_TYPE)
        {
            sLog.outDebug("Item not visible for %s> itemid(%u) in slot (%u)!", player->GetGuidStr().c_str(), lootItem->itemId, uint32(lootItem->lootSlot));
            continue;
        }

        buffer << uint8(lootItem->lootSlot);
        buffer << *lootItem;
        buffer << uint8(slot_type);                              // 0 - get 1 - look only 2 - master selection
        ++itemsShown;

        sLog.outDebug("Sending loot to %s> itemid(%u) in slot (%u)!", player->GetGuidStr().c_str(), lootItem->itemId, uint32(lootItem->lootSlot));
    }

    // update number of items shown
    buffer.put<uint8>(count_pos, itemsShown);
}

ByteBuffer& operator<<(ByteBuffer& b, LootItem const& li)
{
    b << uint32(li.itemId);
    b << uint32(li.count);                                  // nr of items of this type
    b << uint32(ObjectMgr::GetItemPrototype(li.itemId)->DisplayInfoID);
    b << uint32(0);
    b << uint32(li.randomPropertyId);
    return b;
}

//
// --------- LootTemplate::LootGroup ---------
//

// Adds an entry to the group (at loading stage)
void LootTemplate::LootGroup::AddEntry(LootStoreItem& item)
{
    if (item.chance != 0)
        ExplicitlyChanced.push_back(item);
    else
        EqualChanced.push_back(item);
}

// Rolls an item from the group, returns nullptr if all miss their chances
LootStoreItem const* LootTemplate::LootGroup::Roll(Loot const& loot, Player const* lootOwner) const
{
    if (!ExplicitlyChanced.empty())                         // First explicitly chanced entries are checked
    {
        std::vector <LootStoreItem const*> lootStoreItemVector; // we'll use new vector to make easy the randomization

        // fill the new vector with correct pointer to our item list
        for (auto& itr : ExplicitlyChanced)
            lootStoreItemVector.push_back(&itr);

        // randomize the new vector
        random_shuffle(lootStoreItemVector.begin(), lootStoreItemVector.end());

        float chance = rand_chance_f();

        // as the new vector is randomized we can start from first element and stop at first one that meet the condition
        for (std::vector <LootStoreItem const*>::const_iterator itr = lootStoreItemVector.begin(); itr != lootStoreItemVector.end(); ++itr)
        {
            LootStoreItem const* lsi = *itr;

            if (lsi->conditionId && !LootTemplate::PlayerOrGroupFulfilsCondition(loot, lootOwner, lsi->conditionId))
            {
                sLog.outDebug("In explicit chance -> This item cannot be added! (%u)", lsi->itemid);
                continue;
            }

            if (lsi->chance >= 100.0f)
                return lsi;

            chance -= lsi->chance;
            if (chance < 0)
                return lsi;
        }
    }

    if (!EqualChanced.empty())                              // If nothing selected yet - an item is taken from equal-chanced part
    {
        std::vector <LootStoreItem const*> lootStoreItemVector; // we'll use new vector to make easy the randomization

        // fill the new vector with correct pointer to our item list
        for (auto& itr : EqualChanced)
            lootStoreItemVector.push_back(&itr);

        // randomize the new vector
        random_shuffle(lootStoreItemVector.begin(), lootStoreItemVector.end());

        // as the new vector is randomized we can start from first element and stop at first one that meet the condition
        for (std::vector <LootStoreItem const*>::const_iterator itr = lootStoreItemVector.begin(); itr != lootStoreItemVector.end(); ++itr)
        {
            LootStoreItem const* lsi = *itr;

            //check if we already have that item in the loot list
            if (loot.IsItemAlreadyIn(lsi->itemid))
            {
                // the item is already looted, let's give a 50%  chance to pick another one
                uint32 chance = urand(0, 1);

                if (chance)
                    continue;                               // pass this item
            }

            if (lsi->conditionId && !LootTemplate::PlayerOrGroupFulfilsCondition(loot, lootOwner, lsi->conditionId))
            {
                sLog.outDebug("In equal chance -> This item cannot be added! (%u)", lsi->itemid);
                continue;
            }
            return lsi;
        }
    }

    return nullptr;                                            // Empty drop from the group
}

// True if group includes at least 1 quest drop entry
bool LootTemplate::LootGroup::HasQuestDrop() const
{
    for (auto i : ExplicitlyChanced)
        if (i.needs_quest)
            return true;
    for (auto i : EqualChanced)
        if (i.needs_quest)
            return true;
    return false;
}

// True if group includes at least 1 quest drop entry for active quests of the player
bool LootTemplate::LootGroup::HasQuestDropForPlayer(Player const* player) const
{
    for (auto i : ExplicitlyChanced)
        if (player->HasQuestForItem(i.itemid))
            return true;
    for (auto i : EqualChanced)
        if (player->HasQuestForItem(i.itemid))
            return true;
    return false;
}

// Rolls an item from the group (if any takes its chance) and adds the item to the loot
void LootTemplate::LootGroup::Process(Loot& loot, Player const* lootOwner) const
{
    LootStoreItem const* item = Roll(loot, lootOwner);
    if (item != nullptr)
        loot.AddItem(*item);
}

// Overall chance for the group without equal chanced items
float LootTemplate::LootGroup::RawTotalChance() const
{
    float result = 0;

    for (auto i : ExplicitlyChanced)
        if (!i.needs_quest)
            result += i.chance;

    return result;
}

// Overall chance for the group
float LootTemplate::LootGroup::TotalChance() const
{
    float result = RawTotalChance();

    if (!EqualChanced.empty() && result < 100.0f)
        return 100.0f;

    return result;
}

void LootTemplate::LootGroup::Verify(LootStore const& lootstore, uint32 id, uint32 group_id) const
{
    float chance = RawTotalChance();
    if (chance > 101.0f)                                    // TODO: replace with 100% when DBs will be ready
    {
        sLog.outErrorDb("Table '%s' entry %u group %d has total chance > 100%% (%f)", lootstore.GetName(), id, group_id, chance);
    }

    if (chance >= 100.0f && !EqualChanced.empty())
    {
        sLog.outErrorDb("Table '%s' entry %u group %d has items with chance=0%% but group total chance >= 100%% (%f)", lootstore.GetName(), id, group_id, chance);
    }
}

void LootTemplate::LootGroup::CheckLootRefs(LootIdSet* ref_set) const
{
    for (auto ieItr : ExplicitlyChanced)
    {
        if (ieItr.mincountOrRef < 0)
        {
            if (!LootTemplates_Reference.GetLootFor(-ieItr.mincountOrRef))
                LootTemplates_Reference.ReportNotExistedId(-ieItr.mincountOrRef);
            else if (ref_set)
                ref_set->erase(-ieItr.mincountOrRef);
        }
    }

    for (auto ieItr : EqualChanced)
    {
        if (ieItr.mincountOrRef < 0)
        {
            if (!LootTemplates_Reference.GetLootFor(-ieItr.mincountOrRef))
                LootTemplates_Reference.ReportNotExistedId(-ieItr.mincountOrRef);
            else if (ref_set)
                ref_set->erase(-ieItr.mincountOrRef);
        }
    }
}

//
// --------- LootTemplate ---------
//

// Adds an entry to the group (at loading stage)
void LootTemplate::AddEntry(LootStoreItem& item)
{
    if (item.group > 0 && item.mincountOrRef > 0)           // Group
    {
        if (item.group >= Groups.size())
            Groups.resize(item.group);                      // Adds new group the the loot template if needed
        Groups[item.group - 1].AddEntry(item);              // Adds new entry to the group
    }
    else                                                    // Non-grouped entries and references are stored together
        Entries.push_back(item);
}

// Rolls for every item in the template and adds the rolled items the the loot
void LootTemplate::Process(Loot& loot, Player const* lootOwner, LootStore const& store, bool rate, uint8 groupId) const
{
    if (groupId)                                            // Group reference uses own processing of the group
    {
        if (groupId > Groups.size())
            return;                                         // Error message already printed at loading stage

        Groups[groupId - 1].Process(loot, lootOwner);
        return;
    }

    // Rolling non-grouped items
    for (auto Entrie : Entries)
    {
        // Check condition
        if (Entrie.conditionId && !PlayerOrGroupFulfilsCondition(loot, lootOwner, Entrie.conditionId))
            continue;

        if (!Entrie.Roll(rate))
            continue;                                       // Bad luck for the entry

        if (Entrie.mincountOrRef < 0)                           // References processing
        {
            LootTemplate const* Referenced = LootTemplates_Reference.GetLootFor(-Entrie.mincountOrRef);

            if (!Referenced)
                continue;                                   // Error message already printed at loading stage

            for (uint32 loop = 0; loop < Entrie.maxcount; ++loop) // Ref multiplicator
                Referenced->Process(loot, lootOwner, store, rate, Entrie.group);
        }
        else                                                // Plain entries (not a reference, not grouped)
            loot.AddItem(Entrie);                               // Chance is already checked, just add
    }

    // Now processing groups
    for (const auto& Group : Groups)
        Group.Process(loot, lootOwner);
}

// True if template includes at least 1 quest drop entry
bool LootTemplate::HasQuestDrop(LootTemplateMap const& store, uint8 groupId) const
{
    if (groupId)                                            // Group reference
    {
        if (groupId > Groups.size())
            return false;                                   // Error message [should be] already printed at loading stage
        return Groups[groupId - 1].HasQuestDrop();
    }

    for (auto Entrie : Entries)
    {
        if (Entrie.mincountOrRef < 0)                           // References
        {
            LootTemplateMap::const_iterator Referenced = store.find(-Entrie.mincountOrRef);
            if (Referenced == store.end())
                continue;                                   // Error message [should be] already printed at loading stage
            if (Referenced->second->HasQuestDrop(store, Entrie.group))
                return true;
        }
        else if (Entrie.needs_quest)
            return true;                                    // quest drop found
    }

    // Now processing groups
    for (const auto& Group : Groups)
        if (Group.HasQuestDrop())
            return true;

    return false;
}

// True if template includes at least 1 quest drop for an active quest of the player
bool LootTemplate::HasQuestDropForPlayer(LootTemplateMap const& store, Player const* player, uint8 groupId) const
{
    if (groupId)                                            // Group reference
    {
        if (groupId > Groups.size())
            return false;                                   // Error message already printed at loading stage
        return Groups[groupId - 1].HasQuestDropForPlayer(player);
    }

    // Checking non-grouped entries
    for (auto Entrie : Entries)
    {
        if (Entrie.mincountOrRef < 0)                           // References processing
        {
            LootTemplateMap::const_iterator Referenced = store.find(-Entrie.mincountOrRef);
            if (Referenced == store.end())
                continue;                                   // Error message already printed at loading stage
            if (Referenced->second->HasQuestDropForPlayer(store, player, Entrie.group))
                return true;
        }
        else if (player->HasQuestForItem(Entrie.itemid))
            return true;                                    // active quest drop found
    }

    // Now checking groups
    for (const auto& Group : Groups)
        if (Group.HasQuestDropForPlayer(player))
            return true;

    return false;
}

bool LootTemplate::PlayerOrGroupFulfilsCondition(const Loot& loot, Player const* lootOwner, uint16 conditionId)
{
    auto& ownerSet = loot.GetOwnerSet();
    // optimization - no need to look up when player is solo
    if (ownerSet.size() <= 1)
        return sObjectMgr.IsPlayerMeetToCondition(conditionId, lootOwner, lootOwner->GetMap(), loot.GetLootTarget(), CONDITION_FROM_REFERING_LOOT);

    for (const ObjectGuid& guid : ownerSet)
        if (Player* player = lootOwner->GetMap()->GetPlayer(guid))
            if (sObjectMgr.IsPlayerMeetToCondition(conditionId, player, player->GetMap(), loot.GetLootTarget(), CONDITION_FROM_REFERING_LOOT))
                return true;

    return false;
}

// Checks integrity of the template
void LootTemplate::Verify(LootStore const& lootstore, uint32 id) const
{
    // Checking group chances
    for (uint32 i = 0; i < Groups.size(); ++i)
        Groups[i].Verify(lootstore, id, i + 1);

    // TODO: References validity checks
}

void LootTemplate::CheckLootRefs(LootIdSet* ref_set) const
{
    for (auto Entrie : Entries)
    {
        if (Entrie.mincountOrRef < 0)
        {
            if (!LootTemplates_Reference.GetLootFor(-Entrie.mincountOrRef))
                LootTemplates_Reference.ReportNotExistedId(-Entrie.mincountOrRef);
            else if (ref_set)
                ref_set->erase(-Entrie.mincountOrRef);
        }
    }

    for (const auto& Group : Groups)
        Group.CheckLootRefs(ref_set);
}

void LoadLootTemplates_Creature()
{
    LootIdSet ids_set, ids_setUsed;
    LootTemplates_Creature.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sCreatureStorage.GetMaxEntry(); ++i)
    {
        if (CreatureInfo const* cInfo = sCreatureStorage.LookupEntry<CreatureInfo>(i))
        {
            if (uint32 lootid = cInfo->LootId)
            {
                if (ids_set.find(lootid) == ids_set.end())
                    LootTemplates_Creature.ReportNotExistedId(lootid);
                else
                    ids_setUsed.insert(lootid);
            }
        }
    }
    for (uint32 itr : ids_setUsed)
        ids_set.erase(itr);

    // for alterac valley we've defined Player-loot inside creature_loot_template id=0
    // this hack is used, so that we won't need to create an extra table player_loot_template for just one case
    ids_set.erase(0);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Creature.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Disenchant()
{
    LootIdSet ids_set, ids_setUsed;
    LootTemplates_Disenchant.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sItemStorage.GetMaxEntry(); ++i)
    {
        if (ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(i))
        {
            if (uint32 lootid = proto->DisenchantID)
            {
                if (ids_set.find(lootid) == ids_set.end())
                    LootTemplates_Disenchant.ReportNotExistedId(lootid);
                else
                    ids_setUsed.insert(lootid);
            }
        }
    }
    for (uint32 itr : ids_setUsed)
        ids_set.erase(itr);
    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Disenchant.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Fishing()
{
    LootIdSet ids_set;
    LootTemplates_Fishing.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sAreaStore.GetNumRows(); ++i)
    {
        if (AreaTableEntry const* areaEntry = sAreaStore.LookupEntry(i))
            if (ids_set.find(areaEntry->ID) != ids_set.end())
                ids_set.erase(areaEntry->ID);
    }

    // by default (look config options) fishing at fail provide junk loot, entry 0 use for store this loot
    ids_set.erase(0);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Fishing.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Gameobject()
{
    LootIdSet ids_set, ids_setUsed;
    LootTemplates_Gameobject.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (SQLStorageBase::SQLSIterator<GameObjectInfo> itr = sGOStorage.getDataBegin<GameObjectInfo>(); itr < sGOStorage.getDataEnd<GameObjectInfo>(); ++itr)
    {
        if (uint32 lootid = itr->GetLootId())
        {
            if (ids_set.find(lootid) == ids_set.end())
                LootTemplates_Gameobject.ReportNotExistedId(lootid);
            else
                ids_setUsed.insert(lootid);
        }
    }
    for (uint32 itr : ids_setUsed)
        ids_set.erase(itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Gameobject.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Item()
{
    LootIdSet ids_set;
    LootTemplates_Item.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sItemStorage.GetMaxEntry(); ++i)
    {
        if (ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(i))
        {
            if (!(proto->Flags & ITEM_FLAG_HAS_LOOT))
                continue;

            if (ids_set.find(proto->ItemId) != ids_set.end() || proto->MaxMoneyLoot > 0)
                ids_set.erase(proto->ItemId);
            // wdb have wrong data cases, so skip by default
            else if (!sLog.HasLogFilter(LOG_FILTER_DB_STRICTED_CHECK))
                LootTemplates_Item.ReportNotExistedId(proto->ItemId);
        }
    }

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Item.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Pickpocketing()
{
    LootIdSet ids_set, ids_setUsed;
    LootTemplates_Pickpocketing.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sCreatureStorage.GetMaxEntry(); ++i)
    {
        if (CreatureInfo const* cInfo = sCreatureStorage.LookupEntry<CreatureInfo>(i))
        {
            if (uint32 lootid = cInfo->PickpocketLootId)
            {
                if (ids_set.find(lootid) == ids_set.end())
                    LootTemplates_Pickpocketing.ReportNotExistedId(lootid);
                else
                    ids_setUsed.insert(lootid);
            }
        }
    }
    for (uint32 itr : ids_setUsed)
        ids_set.erase(itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Pickpocketing.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Mail()
{
    LootIdSet ids_set;
    LootTemplates_Mail.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sMailTemplateStore.GetNumRows(); ++i)
        if (sMailTemplateStore.LookupEntry(i))
            if (ids_set.find(i) != ids_set.end())
                ids_set.erase(i);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Mail.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Skinning()
{
    LootIdSet ids_set, ids_setUsed;
    LootTemplates_Skinning.LoadAndCollectLootIds(ids_set);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sCreatureStorage.GetMaxEntry(); ++i)
    {
        if (CreatureInfo const* cInfo = sCreatureStorage.LookupEntry<CreatureInfo>(i))
        {
            if (uint32 lootid = cInfo->SkinningLootId)
            {
                if (ids_set.find(lootid) == ids_set.end())
                    LootTemplates_Skinning.ReportNotExistedId(lootid);
                else
                    ids_setUsed.insert(lootid);
            }
        }
    }
    for (uint32 itr : ids_setUsed)
        ids_set.erase(itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Skinning.ReportUnusedIds(ids_set);
}

void LoadLootTemplates_Reference()
{
    LootIdSet ids_set;
    LootTemplates_Reference.LoadAndCollectLootIds(ids_set);

    // check references and remove used
    LootTemplates_Creature.CheckLootRefs(&ids_set);
    LootTemplates_Fishing.CheckLootRefs(&ids_set);
    LootTemplates_Gameobject.CheckLootRefs(&ids_set);
    LootTemplates_Item.CheckLootRefs(&ids_set);
    LootTemplates_Pickpocketing.CheckLootRefs(&ids_set);
    LootTemplates_Skinning.CheckLootRefs(&ids_set);
    LootTemplates_Disenchant.CheckLootRefs(&ids_set);
    LootTemplates_Mail.CheckLootRefs(&ids_set);
    LootTemplates_Reference.CheckLootRefs(&ids_set);

    // output error for any still listed ids (not referenced from any loot table)
    LootTemplates_Reference.ReportUnusedIds(ids_set);
}

// Vote for an ongoing roll
void LootMgr::PlayerVote(Player* player, ObjectGuid const& lootTargetGuid, uint32 itemSlot, RollVote vote)
{
    Loot* loot = GetLoot(player, lootTargetGuid);

    if (!loot)
    {
        sLog.outError("LootMgr::PlayerVote> Error cannot get loot object info!");
        return;
    }

    GroupLootRoll* roll = loot->GetRollForSlot(itemSlot);
    if (!roll)
    {
        sLog.outError("LootMgr::PlayerVote> Invalid itemSlot!");
        return;
    }

    roll->PlayerVote(player, vote);
}

// Get loot by object guid
// If target guid is not provided, try to find it by recipient or current player target
Loot* LootMgr::GetLoot(Player* player, ObjectGuid const& targetGuid) const
{
    Loot* loot = nullptr;
    ObjectGuid lguid;
    if (targetGuid.IsEmpty())
    {
        lguid = player->GetLootGuid();

        if (lguid.IsEmpty())
        {
            lguid = player->GetSelectionGuid();
            if (lguid.IsEmpty())
                return nullptr;
        }
    }
    else
        lguid = targetGuid;

    switch (lguid.GetHigh())
    {
        case HIGHGUID_GAMEOBJECT:
        {
            GameObject* gob = player->GetMap()->GetGameObject(lguid);

            // not check distance for GO in case owned GO (fishing bobber case, for example)
            if (gob)
                loot = gob->loot;

            break;
        }
        case HIGHGUID_CORPSE:                               // remove insignia ONLY in BG
        {
            Corpse* bones = player->GetMap()->GetCorpse(lguid);

            if (bones)
                loot = bones->loot;

            break;
        }
        case HIGHGUID_ITEM:
        {
            Item* item = player->GetItemByGuid(lguid);
            if (item && item->HasGeneratedLoot())
                loot = item->loot;
            break;
        }
        case HIGHGUID_UNIT:
        {
            Creature* creature = player->GetMap()->GetCreature(lguid);

            if (creature)
                loot = creature->loot;

            break;
        }
        default:
            return nullptr;                                         // unlootable type
    }

    return loot;
}

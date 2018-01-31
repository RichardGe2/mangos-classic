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

#include "Common.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "World.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "NPCHandler.h"
#include "SQLStorages.h"
#include <iostream>
#include <string>
#include <fstream>

void WorldSession::SendNameQueryOpcode(Player* p) const
{
    if (!p)
        return;

    // guess size
    WorldPacket data(SMSG_NAME_QUERY_RESPONSE, (8 + 1 + 4 + 4 + 4 + 10));
    data << p->GetObjectGuid();                             // player guid
    data << p->GetName();                                   // played name
    data << uint8(0);                                       // realm name for cross realm BG usage
    data << uint32(p->getRace());
    data << uint32(p->getGender());
    data << uint32(p->getClass());

    SendPacket(data);
}

void WorldSession::SendNameQueryOpcodeFromDB(ObjectGuid guid) const
{
    CharacterDatabase.AsyncPQuery(&WorldSession::SendNameQueryOpcodeFromDBCallBack, GetAccountId(),
                                  //          0     1     2     3       4
                                  "SELECT guid, name, race, gender, class "
                                  "FROM characters WHERE guid = '%u'",
                                  guid.GetCounter());
}

void WorldSession::SendNameQueryOpcodeFromDBCallBack(QueryResult* result, uint32 accountId)
{
    if (!result)
        return;

    WorldSession* session = sWorld.FindSession(accountId);
    if (!session)
    {
        delete result;
        return;
    }

    Field* fields = result->Fetch();
    uint32 lowguid      = fields[0].GetUInt32();
    std::string name = fields[1].GetCppString();
    uint8 pRace = 0, pGender = 0, pClass = 0;
    if (name.empty())
        name         = session->GetMangosString(LANG_NON_EXIST_CHARACTER);
    else
    {
        pRace        = fields[2].GetUInt8();
        pGender      = fields[3].GetUInt8();
        pClass       = fields[4].GetUInt8();
    }

    // guess size
    WorldPacket data(SMSG_NAME_QUERY_RESPONSE, (8 + 1 + 4 + 4 + 4 + 10));
    data << ObjectGuid(HIGHGUID_PLAYER, lowguid);
    data << name;
    data << uint8(0);                                       // realm name for cross realm BG usage
    data << uint32(pRace);                                  // race
    data << uint32(pGender);                                // gender
    data << uint32(pClass);                                 // class

    session->SendPacket(data);
    delete result;
}

void WorldSession::HandleNameQueryOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;

    recv_data >> guid;

    Player* pChar = sObjectMgr.GetPlayer(guid);

    if (pChar)
        SendNameQueryOpcode(pChar);
    else
        SendNameQueryOpcodeFromDB(guid);
}

void WorldSession::HandleQueryTimeOpcode(WorldPacket& /*recv_data*/)
{
    SendQueryTimeResponse();
}

/// Only _static_ data send in this packet !!!
void WorldSession::HandleCreatureQueryOpcode(WorldPacket& recv_data)
{
    uint32 entry;
    ObjectGuid guid;

    recv_data >> entry;
    recv_data >> guid;

    Creature* unit = _player->GetMap()->GetAnyTypeCreature(guid);

    // if (unit == nullptr)
    //    sLog.outDebug( "WORLD: HandleCreatureQueryOpcode - (%u) NO SUCH UNIT! (GUID: %u, ENTRY: %u)", uint32(GUID_LOPART(guid)), guid, entry );

    CreatureInfo const* ci = ObjectMgr::GetCreatureTemplate(entry);
    if (ci)
    {
        int loc_idx = GetSessionDbLocaleIndex();

        char const* name = ci->Name;
        char const* subName = ci->SubName;
        sObjectMgr.GetCreatureLocaleStrings(entry, loc_idx, &name, &subName);

        DETAIL_LOG("WORLD: CMSG_CREATURE_QUERY '%s' - Entry: %u.", ci->Name, entry);
        // guess size
        WorldPacket data(SMSG_CREATURE_QUERY_RESPONSE, 100);
        data << uint32(entry);                              // creature entry
        data << name;
        data << uint8(0) << uint8(0) << uint8(0);           // name2, name3, name4, always empty
        data << subName;
        data << uint32(ci->CreatureTypeFlags);              // flags
        data << uint32(ci->CreatureType);                   // CreatureType.dbc   wdbFeild8
        data << uint32(ci->Family);                         // CreatureFamily.dbc
        data << uint32(ci->Rank);                           // Creature Rank (elite, boss, etc)
        data << uint32(0);                                  // unknown        wdbFeild11
        data << uint32(ci->PetSpellDataId);                 // Id from CreatureSpellData.dbc    wdbField12
        if (unit)
            data << unit->GetUInt32Value(UNIT_FIELD_DISPLAYID); // DisplayID      wdbFeild13
        else
            data << uint32(Creature::ChooseDisplayId(ci));  // workaround, way to manage models must be fixed

        data << uint16(ci->civilian);                       // wdbFeild14
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_CREATURE_QUERY_RESPONSE");
    }
    else
    {
        DEBUG_LOG("WORLD: CMSG_CREATURE_QUERY - Guid: %s Entry: %u NO CREATURE INFO!",
                  guid.GetString().c_str(), entry);
        WorldPacket data(SMSG_CREATURE_QUERY_RESPONSE, 4);
        data << uint32(entry | 0x80000000);
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_CREATURE_QUERY_RESPONSE");
    }
}

/// Only _static_ data send in this packet !!!
void WorldSession::HandleGameObjectQueryOpcode(WorldPacket& recv_data)
{
    uint32 entryID;
    recv_data >> entryID;
    ObjectGuid guid;
    recv_data >> guid;

    const GameObjectInfo* info = ObjectMgr::GetGameObjectInfo(entryID);
    if (info)
    {
        std::string Name = info->name;

        int loc_idx = GetSessionDbLocaleIndex();
        if (loc_idx >= 0)
        {
            GameObjectLocale const* gl = sObjectMgr.GetGameObjectLocale(entryID);
            if (gl)
            {
                if (gl->Name.size() > size_t(loc_idx) && !gl->Name[loc_idx].empty())
                    Name = gl->Name[loc_idx];
            }
        }
        DETAIL_LOG("WORLD: CMSG_GAMEOBJECT_QUERY '%s' - Entry: %u. ", info->name, entryID);
        WorldPacket data(SMSG_GAMEOBJECT_QUERY_RESPONSE, 150);
        data << uint32(entryID);
        data << uint32(info->type);
        data << uint32(info->displayId);
        data << Name;
        data << uint16(0) << uint8(0) << uint8(0);          // name2, name3, name4
        data.append(info->raw.data, 24);
        // data << float(info->size);                       // go size , to check
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_GAMEOBJECT_QUERY_RESPONSE");
    }
    else
    {
        DEBUG_LOG("WORLD: CMSG_GAMEOBJECT_QUERY - Guid: %s Entry: %u Missing gameobject info!",
                  guid.GetString().c_str(), entryID);
        WorldPacket data(SMSG_GAMEOBJECT_QUERY_RESPONSE, 4);
        data << uint32(entryID | 0x80000000);
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_GAMEOBJECT_QUERY_RESPONSE");
    }
}

void WorldSession::HandleCorpseQueryOpcode(WorldPacket& /*recv_data*/)
{
    DETAIL_LOG("WORLD: Received opcode MSG_CORPSE_QUERY");

    Corpse* corpse = GetPlayer()->GetCorpse();

    if (!corpse)
    {
        WorldPacket data(MSG_CORPSE_QUERY, 1);
        data << uint8(0);                                   // corpse not found
        SendPacket(data);
        return;
    }

    uint32 corpsemapid = corpse->GetMapId();
    float x = corpse->GetPositionX();
    float y = corpse->GetPositionY();
    float z = corpse->GetPositionZ();
    int32 mapid = corpsemapid;

    // if corpse at different map
    if (corpsemapid != _player->GetMapId())
    {
        // search entrance map for proper show entrance
        if (InstanceTemplate const* temp = sObjectMgr.GetInstanceTemplate(mapid))
        {
            if (temp->ghostEntranceMap >= 0)
            {
                // if corpse map have entrance
                if (TerrainInfo const* entranceMap = sTerrainMgr.LoadTerrain(temp->ghostEntranceMap))
                {
                    mapid = temp->ghostEntranceMap;
                    x = temp->ghostEntranceX;
                    y = temp->ghostEntranceY;
                    z = entranceMap->GetHeightStatic(x, y, MAX_HEIGHT);
                }
            }
        }
    }

    WorldPacket data(MSG_CORPSE_QUERY, 1 + (5 * 4));
    data << uint8(1);                                       // corpse found
    data << int32(mapid);
    data << float(x);
    data << float(y);
    data << float(z);
    data << uint32(corpsemapid);
    SendPacket(data);
}

void WorldSession::HandleNpcTextQueryOpcode(WorldPacket& recv_data)
{
    uint32 textID;
    ObjectGuid guid;

    recv_data >> textID;
    recv_data >> guid;

    DETAIL_LOG("WORLD: CMSG_NPC_TEXT_QUERY ID '%u'", textID);

    GossipText const* pGossip = sObjectMgr.GetGossipText(textID);

    WorldPacket data(SMSG_NPC_TEXT_UPDATE, 100);            // guess size
    data << textID;

    if (!pGossip)
    {
        for (uint32 i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
        {
            data << float(0);
            data << "Greetings $N";
            data << "Greetings $N";
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
        }
    }
    else
    {
        std::string Text_0[MAX_GOSSIP_TEXT_OPTIONS], Text_1[MAX_GOSSIP_TEXT_OPTIONS];
        for (int i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
        {
            Text_0[i] = pGossip->Options[i].Text_0;
            Text_1[i] = pGossip->Options[i].Text_1;
        }

        int loc_idx = GetSessionDbLocaleIndex();

        sObjectMgr.GetNpcTextLocaleStringsAll(textID, loc_idx, &Text_0, &Text_1);

        for (int i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
        {
            data << pGossip->Options[i].Probability;

            if (Text_0[i].empty())
                data << Text_1[i];
            else
                data << Text_0[i];

            if (Text_1[i].empty())
                data << Text_0[i];
            else
                data << Text_1[i];

            data << pGossip->Options[i].Language;

            for (int j = 0; j < 3; ++j)
            {
                data << pGossip->Options[i].Emotes[j]._Delay;
                data << pGossip->Options[i].Emotes[j]._Emote;
            }
        }
    }

    SendPacket(data);

    DEBUG_LOG("WORLD: Sent SMSG_NPC_TEXT_UPDATE");
}

void WorldSession::HandlePageTextQueryOpcode(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_PAGE_TEXT_QUERY");

    uint32 pageID;
    recv_data >> pageID;






	/////////////////////////////////////////////
	// RICHARD : decouverte d'un livre

	//a noter que  HandlePageTextQueryOpcode  est appelé QUE si le client n'a pas la page dans ses fichier de cache
	//d'ou l'important ce bien cleaner le cache de client  (dossier WDB)

	//il faudra peut etre que je definisse mieux qu'est ce qu'un texte a decouvrir et s'il y a des textes qu'on prends pas en compte dans l'achievement

	//commane pour voir les gameobject qui ont du texte :
	//SELECT entry,NAME,data0 FROM gameobject_template WHERE TYPE=9 AND data0>0
	//il y en a 108
	//
	//j'avais oublié les objets type GAMEOBJECT_TYPE_GOOBER
	//la bonne commande est donc :
	//SELECT entry,NAME,data0,data7 FROM gameobject_template WHERE TYPE=9 AND data0>0 OR TYPE=10 AND data7>0
	//il y en a 111

	if ( pageID )
	{

		//on chercher si  pageID  est dans  m_richa_pageDiscovered
		bool existInDataBase_ofThisPerso = false;
		bool comesFromObject = false;
		for(int i=0; i<_player->m_richa_pageDiscovered.size(); i++)
		{
			if ( _player->m_richa_pageDiscovered[i].pageId == pageID )
			{
				existInDataBase_ofThisPerso = true;
				if ( _player->m_richa_pageDiscovered[i].objectID != 0 )
				{
					comesFromObject = true;
				}
				break;
			}
		}

		if ( !existInDataBase_ofThisPerso )
		{
			int objectId = 0;  // correspond a   object=XXX  dans  vanillagaming
			int itemId = 0; // correspond a item=xxx

			char command4[2048];
			sprintf(command4,"SELECT entry FROM gameobject_template WHERE TYPE=9 AND data0=%d",pageID);
			if (QueryResult* result4 = WorldDatabase.PQuery( command4 ))
			{
				if ( result4->GetRowCount() != 1 )
				{
					BASIC_LOG("ERROR 8453");
					_player->Say("ERROR : OBJECT NON TROUVE 001", LANG_UNIVERSAL);
				}

				BarGoLink bar4(result4->GetRowCount());
				bar4.step();
				Field* fields = result4->Fetch();
				objectId = fields->GetInt32();
				delete result4;

				comesFromObject = true;
			}
			else
			{
				//si on arrive la, c'est peut etre un item , example d'item :  5088  -  Control Console Operating Manual
				//note : les textes venant d'un  item ne font pas parti du succes de tout lire
				//       mais au cas ou, on va les sauvegarder quand meme - ca coute rien

				char command5[2048];
				sprintf(command5,"SELECT entry FROM item_template WHERE PageText=%d",pageID);
				if (QueryResult* result5 = WorldDatabase.PQuery( command5 ))
				{
					if ( result5->GetRowCount() != 1 )
					{
						BASIC_LOG("ERROR 8453");
						_player->Say("ERROR : OBJECT NON TROUVE 003", LANG_UNIVERSAL);
					}

					BarGoLink bar5(result5->GetRowCount());
					bar5.step();
					Field* fields = result5->Fetch();
					itemId = fields->GetInt32();
					delete result5;
				}
				else
				{
					//si on arrive ici, le text est ni dans un gameobject ni dans un item... a étudier...

					BASIC_LOG("ERROR 8454  ----  pageID = %d", pageID);
					_player->Say("ERROR : OBJECT NON TROUVE 002", LANG_UNIVERSAL);
				}
			

			}



			//si le livre n'est pas connu par ce perso, on l'ajoute a la liste de ce perso
			_player->m_richa_pageDiscovered.push_back( Player::RICHA_PAGE_DISCO_STAT( pageID , objectId, itemId) ); 



			// par curiosite, on regarde si un autre perso du meme joueur humain connait ce texte

			bool knownByOtherPerso = false;

			std::vector<int>  associatedPlayerGUID;

			// #LISTE_ACCOUNT_HERE   -  ce hashtag repere tous les endroit que je dois updater quand je rajoute un nouveau compte - ou perso important
			if ( _player->GetGUID() == 4 )// boulette
			{
				associatedPlayerGUID.push_back(27); // Bouzigouloum
			}
			if ( _player->GetGUID() == 27 )//  Bouzigouloum 
			{
				associatedPlayerGUID.push_back(4); // boulette
			}
			if ( _player->GetGUID() == 5 )// Bouillot
			{
				associatedPlayerGUID.push_back(28); // Adibou
			}
			if ( _player->GetGUID() == 28 )// Adibou 
			{
				associatedPlayerGUID.push_back(5); //  Bouillot
			}

			//juste pour le debug je vais lier grandjuge et grandtroll
			if ( _player->GetGUID() == 19 )// grandjuge
			{
				associatedPlayerGUID.push_back(29); // grandtroll
			}
			if ( _player->GetGUID() == 29 )// grandtroll 
			{
				associatedPlayerGUID.push_back(19); //  grandjuge
			}


			for(int i=0; i<associatedPlayerGUID.size(); i++)
			{
				std::vector<Player::RICHA_NPC_KILLED_STAT> richa_NpcKilled;
				std::vector<Player::RICHA_PAGE_DISCO_STAT> richa_pageDiscovered;
				std::vector<Player::RICHA_LUNARFESTIVAL_ELDERFOUND> richa_lunerFestivalElderFound;
				Player::richard_importFrom_richaracter_(
					associatedPlayerGUID[i],
					richa_NpcKilled,
					richa_pageDiscovered,
					richa_lunerFestivalElderFound
					);

				for(int j=0; j<richa_pageDiscovered.size(); j++)
				{
					if ( richa_pageDiscovered[j].pageId == pageID )
					{
						knownByOtherPerso = true;
						break;
					}
				}//pour chaque page connu du perso associe


				if ( knownByOtherPerso )
				{
					break;
				}


			}//pour chaque perso associé



			if ( !knownByOtherPerso )
			{
				if ( comesFromObject ) // dans le succes, on compte QUE les 111 textes qui viennet d'un object et PAS d'un item
				{
					char messageOut[256];
					sprintf(messageOut, "Decouverte d'un nouveau texte!");
					_player->Say(messageOut, LANG_UNIVERSAL);
				}
			}
			else
			{
				if ( comesFromObject ) // dans le succes, on compte QUE les 111 textes qui viennet d'un object et PAS d'un item
				{
					char messageOut[256];
					sprintf(messageOut, "Texte deja connu par un autre Perso.");
					_player->Say(messageOut, LANG_UNIVERSAL);
				}
			}

		
		}
		else
		{
			if ( comesFromObject ) // dans le succes, on compte QUE les 111 textes qui viennet d'un object et PAS d'un item
			{
				char messageOut[256];
				sprintf(messageOut, "Texte deja connu par ce Perso.");
				_player->Say(messageOut, LANG_UNIVERSAL);
			}
		}

	} // if pageId
	//////////////////////////////////////////







    while (pageID)
    {
        PageText const* pPage = sPageTextStore.LookupEntry<PageText>(pageID);
        // guess size
        WorldPacket data(SMSG_PAGE_TEXT_QUERY_RESPONSE, 50);
        data << pageID;

        if (!pPage)
        {
            data << "Item page missing.";
            data << uint32(0);
            pageID = 0;
        }
        else
        {
            std::string Text = pPage->Text;

            int loc_idx = GetSessionDbLocaleIndex();
            if (loc_idx >= 0)
            {
                PageTextLocale const* pl = sObjectMgr.GetPageTextLocale(pageID);
                if (pl)
                {
                    if (pl->Text.size() > size_t(loc_idx) && !pl->Text[loc_idx].empty())
                        Text = pl->Text[loc_idx];
                }
            }

            data << Text;
            data << uint32(pPage->Next_Page);
            pageID = pPage->Next_Page;
        }
        SendPacket(data);

        DEBUG_LOG("WORLD: Sent SMSG_PAGE_TEXT_QUERY_RESPONSE");
    }
}

void WorldSession::SendQueryTimeResponse() const
{
    WorldPacket data(SMSG_QUERY_TIME_RESPONSE, 4);
    data << uint32(time(nullptr));
    SendPacket(data);
}

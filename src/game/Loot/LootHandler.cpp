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
#include "WorldPacket.h"
#include "Log.h"
#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Entities/ObjectGuid.h"
#include "Server/WorldSession.h"
#include "Loot/LootMgr.h"
#include "Entities/Object.h"
#include "Groups/Group.h"





//  RETURN TRUE si le loot est accepté
//  RETURN FALSE si le loot est refusé
//
// lootTypeItemOrGold = 1  pour item loot
// lootTypeItemOrGold = 2  pour gold loot
bool WorldSession::RichaHandleLootRandom(Loot* loot, int lootTypeItemOrGold )
{

	// a l'epoque de boulette et bouillot on a mis  2000.  c'etait tres bien, pour 2 corp a corp
	// avec adibou et bouzigouloum , c'est trop court.
	const uint32 g___palier_ms = 15000; // richard

	const uint32 maxDice = 1000; // richard

	std::string DEBUG_TEXT_LOOT = "??";
	if ( lootTypeItemOrGold == 1 )
	{
		DEBUG_TEXT_LOOT = "LOOT-ITEM";
	}
	if ( lootTypeItemOrGold == 2 )
	{
		DEBUG_TEXT_LOOT = "LOOT-GOLD";
	}


	//richard : pour eviter d'avantager qqun qui est plus proche du server, tout les auto store loot sont interdit avant X secondes

	bool lootOrigin_creature = false;
	bool lootOrigin_gameobj = false;
	bool lootOrigin_item = false;

	if ( loot->GetLootTarget() )
	{
		lootOrigin_creature = loot->GetLootTarget()->GetObjectGuid().IsCreature();
		lootOrigin_gameobj = loot->GetLootTarget()->GetObjectGuid().IsGameObject();
	}
	//lootOrigin_item = loot->item->GetObjectGuid().IsItem();
	
	LootType lootType = loot->GetLootType();

	// 0   undef
	// 1   creature corpse
	// 2   gameobj
	// 3   item - marche pas
	// 4  skinning
	// 5  pickpocket ( peut etre ouverture d'un item aussi , bug ?)
	int lootOrigin = 0;
	if ( lootOrigin_creature && !lootOrigin_gameobj && !lootOrigin_item )
	{
		if ( lootType == LOOT_SKINNING )
		{
			lootOrigin = 4;
		}
		else if ( lootType == LOOT_CORPSE )
		{
			lootOrigin = 1;
		}
		else
		{
			sLog.outBasic("RICHAR: LOOT - type lootOrigin_creature + ????(%d)" , lootType );
			lootOrigin = 0;
			int aa=0;
		}
		
		
		//sLog.outBasic("RICHAR: LOOT - type creature" );
		
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
		if ( lootType == LOOT_PICKPOCKETING )
		{
			lootOrigin = 5;
		}
		else
		{

			bool isItem = false;
			bool isDynamicObject = false;
			if ( loot && loot->GetLootTarget() && loot->GetLootTarget()->GetObjectGuid() )
			{
				isItem			= loot->GetLootTarget()->GetObjectGuid().IsItem();
				isDynamicObject = loot->GetLootTarget()->GetObjectGuid().IsDynamicObject();
			}
			sLog.outBasic("RICHAR: LOOT - type ??? (object loot?)  -  isItem=%d  isDynamicObject=%d " , isItem , isDynamicObject );
			lootOrigin = 0;
			int aa=0;
		}
	}

	uint32 difference = WorldTimer::getMSTime()  - loot->m_richard_timeCreated;

	g_wantLoot[loot->m_richard_timeCreated].list[_player].nbFois ++;

	Group* groupPlay = _player->GetGroup();
	int nbPLayInGroup = 1;
	if ( groupPlay )
	{
		nbPLayInGroup = groupPlay->GetMembersCount();
	}

	if ( g_wantLoot[loot->m_richard_timeCreated].list[_player].scoreDice == -1 ) // si le dés n'est pas jeté
	{
		uint32 scorede = urand(2, maxDice);
		const char* playerName = _player->GetName();

		g_wantLoot[loot->m_richard_timeCreated].list[_player].scoreDice = scorede;

		//si le joueur se présente en retard, on lui donne un très mauvais dès
		if ( difference >= g___palier_ms  )
		{
			g_wantLoot[loot->m_richard_timeCreated].list[_player].scoreDice = 1; // 1 correspond au score de retard
		}

		if ( !g_wantLoot[loot->m_richard_timeCreated].okWinDoneOnThisLoot  // ca sert a rien d'embrouiller les joueurs avec des infos useless - on s'en fou du score quand c'est pour looter un OKWIN
			&& lootOrigin == 1 // on dit le MOI que sur les cadavres
			&& nbPLayInGroup > 1 // on dit MOI que dans un groupe
			)
		{
			char sayMoiMoiMoi[256];
			sprintf_s(sayMoiMoiMoi,"Moi! %d", g_wantLoot[loot->m_richard_timeCreated].list[_player].scoreDice );
			_player->Say(sayMoiMoiMoi , LANG_UNIVERSAL);
		}

	}

	if (
		//!g_wantLoot[loot->m_richard_timeCreated].okWinDoneOnThisLoot // si un OKWIN a été fait pour ce loot, ---> alors on peut direct aller a l'election du vainqueur  <-- pas besoin de ce check , car maintenant un OKWIN va creer une candidature avec score de 0 pour celui qui a fait le OKWIN
		//&&
		difference < g___palier_ms  // si le temps de présentation est passé ---> alors on peut direct aller a l'election du vainqueur
		&&
		g_wantLoot[loot->m_richard_timeCreated].list.size() < nbPLayInGroup  // si on est deja 2 joueurs a se présenter ---> alors on peut direct aller a l'election du vainqueur
		&&
		lootOrigin == 1 // si le loot est pas de type creature ---> alors on peut direct aller a l'election du vainqueur
		&&
		g_wantLoot[loot->m_richard_timeCreated].winner == nullptr // si le winner a deja été decidé ---> alors on peut direct aller a l'election du vainqueur
	
		// .... dans tous les autres cas, on attends d'avoir une de ces condition de respecter, avant d'elir le winner ...

		) // pour rajouter un peu d'aleatoire, le palier est re-tiré au random a chaque fois 
	{
		sLog.outBasic("RICHAR: DEBUG_TEXT_LOOT - REFUSE a %s - %d < %d  (nbCandidat=%d) (group de %d)", _player->GetName() ,difference, g___palier_ms ,g_wantLoot[loot->m_richard_timeCreated].list.size() , nbPLayInGroup );
		return false;
	}
	else
	{

		//si le gagnant a pas encore été decidé, c'est le moment
		if ( g_wantLoot[loot->m_richard_timeCreated].winner == nullptr )
		{
			
			int nbJoueur = g_wantLoot[loot->m_richard_timeCreated].list.size();


			if ( nbJoueur == 0 )
			{
				//je crois que ce cas est pas possible
				sLog.outBasic("RICHAR: DEBUG_TEXT_LOOT - personne a reclame le loot donc on donne direct a %s"  ,  _player->GetName()  );
				g_wantLoot[loot->m_richard_timeCreated].winner = _player;
			}
			else
			{
				int bestScoreDice = -1;
				for(auto const &ent1 : g_wantLoot[loot->m_richard_timeCreated].list) 
				{
					if ( ent1.second.scoreDice >= bestScoreDice )
					{
						g_wantLoot[loot->m_richard_timeCreated].winner = ent1.first;
						bestScoreDice = ent1.second.scoreDice;
					}
				}

				//si on est dans un groupe et que un seul joueur etait candidat, c'est pas mal
				//de le signaler pour s'assurer que tout le groupe est d'accord qu'il y a des gens qui
				//ne se sont pas présenté aux elections
				if ( groupPlay != NULL 
					&& nbJoueur < nbPLayInGroup 
					&& lootOrigin == 1 
					//&& !g_wantLoot[loot->m_richard_timeCreated].winnerSaidIWinAlone 
					&& !g_wantLoot[loot->m_richard_timeCreated].okWinDoneOnThisLoot // ca sert a rien de /SAY pour looter un okwin
					)
				{
					char messageOut[1024];
					if ( loot->GetLootTarget() )
					{
						sprintf(messageOut, "je gagne seul '%s' !" , loot->GetLootTarget()->GetName());
					}
					else
					{
						sprintf(messageOut, "je gagne seul '????' !");
					}

					g_wantLoot[loot->m_richard_timeCreated].winner->Say(messageOut, LANG_UNIVERSAL);
					//g_wantLoot[loot->m_richard_timeCreated].winnerSaidIWinAlone = true;
				}
				
				sLog.outBasic("RICHAR: DEBUG_TEXT_LOOT - le joueur qui gagne le loot est %s "  , g_wantLoot[loot->m_richard_timeCreated].winner->GetName()   );
			
			}

		}

		if ( lootTypeItemOrGold == 1 ) // si loot de type item
		{
			if ( !g_wantLoot[loot->m_richard_timeCreated].messageSentToPlayer_loot )
			{
				if ( loot->GetLootTarget() 
					&& lootOrigin == 1  // pour les item, on envoie un message QUE si c'est sur un cadavre qu'on loot ca
					)
				{
					//envoie message a tous les joueurs:
					for(auto const &ent1 : g_wantLoot[loot->m_richard_timeCreated].list) 
					{
						if ( ent1.first == g_wantLoot[loot->m_richard_timeCreated].winner )
						{
							char messageOut[1024];
							sprintf(messageOut, "vous gagnez le loot 'objet' (score=%d)",ent1.second.scoreDice);
							if ( !g_wantLoot[loot->m_richard_timeCreated].okWinDoneOnThisLoot )// ca sert a rien de messager pour looter un okwin
							{
								if ( nbPLayInGroup == 1  )
								{
									//si on est tout seul dans le group, ca sert a rien d'annoncer au joueur qu'il a gagné le loot
								}
								else
								{
									ChatHandler(ent1.first).PSendSysMessage(messageOut);
								}
							}
						}
						else
						{
							char messageOut[1024];
							sprintf(messageOut, "vous perdez le loot 'objet' (score=%d)",ent1.second.scoreDice);
							if ( !g_wantLoot[loot->m_richard_timeCreated].okWinDoneOnThisLoot )// ca sert a rien de messager pour looter un okwin
							{
								ChatHandler(ent1.first).PSendSysMessage(messageOut);
							}

						}
					}
				}

				g_wantLoot[loot->m_richard_timeCreated].messageSentToPlayer_loot = true;
			}
		}
		if ( lootTypeItemOrGold == 2 ) // si loot de type gold
		{
			if ( !g_wantLoot[loot->m_richard_timeCreated].messageSentToPlayer_po )
			{
				if ( loot->GetLootTarget() )
				{
					//envoie message a tous les joueurs:
					for(auto const &ent1 : g_wantLoot[loot->m_richard_timeCreated].list) 
					{
						if ( ent1.first == g_wantLoot[loot->m_richard_timeCreated].winner )
						{
							//chuchoter la somme au joueur (car elle est ecrit nul part)
							int goldAmount = loot->GetGoldAmount();
							int nbpo = goldAmount / 10000;
							int nbpa = (goldAmount - nbpo*10000) / 100;
							int nbpc = (goldAmount - nbpo*10000 - nbpa*100) ;

							char messageOut[1024];
							sprintf(messageOut, "vous gagnez les PO %d-%d-%d (score=%d)", nbpo, nbpa, nbpc,ent1.second.scoreDice);
							if ( !g_wantLoot[loot->m_richard_timeCreated].okWinDoneOnThisLoot )// ca sert a rien de messager pour looter un okwin
							{
								if ( nbPLayInGroup == 1 && lootOrigin == 1 )
								{
									// dans le cas spécial d'un cadavre et qu'on est tout seul danbs le groupe, le jeu va deja dire : 
									//"Vous ramassez 6 Argent, 16 Cuivre"
									//donc ca sert a rien de dire en doublon ma phrase.
								}
								else
								{
									ChatHandler(ent1.first).PSendSysMessage(messageOut);
								}
							}
						}
						else
						{
							char messageOut[1024];
							sprintf(messageOut, "vous perdez les PO (score=%d)",ent1.second.scoreDice);
							if ( !g_wantLoot[loot->m_richard_timeCreated].okWinDoneOnThisLoot )// ca sert a rien de messager pour looter un okwin
							{
								ChatHandler(ent1.first).PSendSysMessage(messageOut);
							}

						}
					}
				}

				g_wantLoot[loot->m_richard_timeCreated].messageSentToPlayer_po = true;
			}
		}
		

		if ( _player == g_wantLoot[loot->m_richard_timeCreated].winner )
		{
			//sLog.outBasic("RICHAR: DEBUG_TEXT_LOOT - le winner %s prend son loot "  , g_wantLoot[loot->m_richard_timeCreated].winner->GetName()   );
		}
		else
		{
			//sLog.outBasic("RICHAR: DEBUG_TEXT_LOOT - REFUSE : le looser %s essaye de prendre loot. "  , _player->GetName()   );
			//pour se faire autoriser un loot, le looser devra demander au winner de faire la commande :  .okwin
			return false;
		}
		

		//sLog.outBasic("RICHAR: loot accepete a %s - %d < %d", _player->GetName() ,difference, palier  );
	}



	return true;
}



void WorldSession::HandleAutostoreLootItemOpcode(WorldPacket& recv_data)
{
    uint8 itemSlot;
    recv_data >> itemSlot;

    DEBUG_LOG("WORLD: CMSG_AUTOSTORE_LOOT_ITEM > requesting item in slot %u", uint32(itemSlot));

    Loot* loot = sLootMgr.GetLoot(_player);




	if (!loot)
    {
        sLog.outError("HandleAutostoreLootItemOpcode> Cannot retrieve loot for player %s", _player->GetGuidStr().c_str());
        return;
    }




	bool lootAccepted = RichaHandleLootRandom(loot,1);
	if ( !lootAccepted )
	{
		return;
	}



    ObjectGuid const& lguid = loot->GetLootGuid();

    LootItem* lootItem = loot->GetLootItemInSlot(itemSlot);

    if (!lootItem)
    {
        _player->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, nullptr, nullptr);
        loot->Release(_player);
        return;
    }

    // item may be blocked by roll system or already looted or another cheating possibility
    if (lootItem->isBlocked || lootItem->GetSlotTypeForSharedLoot(_player, loot) == MAX_LOOT_SLOT_TYPE)
    {
        sLog.outError("HandleAutostoreLootItemOpcode> %s have no right to loot itemId(%u)", _player->GetGuidStr().c_str(), lootItem->itemId);
        loot->Release(_player);
        return;
    }

    InventoryResult result = loot->SendItem(_player, lootItem);

    if (result == EQUIP_ERR_OK && lguid.IsItem())
    {
        if (Item* item = _player->GetItemByGuid(lguid))
            item->SetLootState(ITEM_LOOT_CHANGED);
    }
}




std::map<time_t  , WorldSession::RICHARD_TRY_LOOT_WANT  > WorldSession::g_wantLoot ;  // richard






void WorldSession::HandleLootMoneyOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: CMSG_LOOT_MONEY");

    Loot* pLoot = sLootMgr.GetLoot(_player);

    if (!pLoot)
    {
        sLog.outError("HandleLootMoneyOpcode> Cannot retrieve loot for player %s", _player->GetGuidStr().c_str());
        return;
    }



	bool lootAccepted = RichaHandleLootRandom(pLoot,2);
	if ( !lootAccepted )
	{
		return;
	}


    pLoot->SendGold(_player);




}

void WorldSession::HandleLootOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: CMSG_LOOT");

    ObjectGuid lguid;
    recv_data >> lguid;

    // Check possible cheat
    if (!_player->isAlive())
        return;

    if (Loot* loot = sLootMgr.GetLoot(_player, lguid))
    {
        // remove stealth aura
        _player->DoInteraction(lguid);

        loot->ShowContentTo(_player);
    }

    // interrupt cast
    if (GetPlayer()->IsNonMeleeSpellCasted(false))
        GetPlayer()->InterruptNonMeleeSpells(false);

}

void WorldSession::HandleLootReleaseOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: CMSG_LOOT_RELEASE");

    ObjectGuid lguid;
    recv_data >> lguid;

    if (Loot* loot = sLootMgr.GetLoot(_player, lguid))
        loot->Release(_player);

}

void WorldSession::HandleLootMasterGiveOpcode(WorldPacket& recv_data)
{
    uint8      itemSlot;        // slot sent in LOOT_RESPONSE
    ObjectGuid lootguid;        // the guid of the loot object owner
    ObjectGuid targetGuid;      // the item receiver guid

    recv_data >> lootguid >> itemSlot >> targetGuid;

    Player* target = ObjectAccessor::FindPlayer(targetGuid);
    if (!target)
    {
        _player->SendLootError(lootguid, LOOT_ERROR_PLAYER_NOT_FOUND);
        sLog.outError("WorldSession::HandleLootMasterGiveOpcode> Cannot retrieve target %s", targetGuid.GetString().c_str());
        return;
    }

    DEBUG_LOG("WorldSession::HandleLootMasterGiveOpcode> Giver = %s, Target = %s.", _player->GetGuidStr().c_str(), targetGuid.GetString().c_str());

    Loot* pLoot = sLootMgr.GetLoot(_player, lootguid);

    if (!pLoot)
    {
        sLog.outError("WorldSession::HandleLootMasterGiveOpcode> Cannot retrieve loot for player %s", _player->GetGuidStr().c_str());
        return;
    }

    if (_player->GetObjectGuid() != pLoot->GetMasterLootGuid())
    {
        _player->SendLootError(lootguid, LOOT_ERROR_DIDNT_KILL);
        sLog.outError("WorldSession::HandleLootMasterGiveOpcode> player %s is not the loot master!", _player->GetGuidStr().c_str());
        return;
    }

    if (!_player->IsInGroup(target) || !_player->IsInMap(target))
    {
        _player->SendLootError(lootguid, LOOT_ERROR_MASTER_OTHER);
        sLog.outError("WorldSession::HandleLootMasterGiveOpcode> Player %s tried to give an item to ineligible player %s !", _player->GetGuidStr().c_str(), target->GetGuidStr().c_str());
        return;
    }

    LootItem* lootItem = pLoot->GetLootItemInSlot(itemSlot);

    if (!lootItem)
    {
        _player->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, nullptr, nullptr);
        return;
    }

    // item may be already looted or another cheating possibility
    if (lootItem->GetSlotTypeForSharedLoot(_player, pLoot) == MAX_LOOT_SLOT_TYPE)
    {
        sLog.outError("HandleAutostoreLootItemOpcode> %s have no right to loot itemId(%u)", _player->GetGuidStr().c_str(), lootItem->itemId);
        return;
    }

    if (!lootItem->IsAllowed(target, pLoot))
    {
        _player->SendEquipError(EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM, nullptr, nullptr);
        return;
    }

    InventoryResult result = pLoot->SendItem(target, lootItem);
    if (result != EQUIP_ERR_OK)
    {
        if (result == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_UNIQUE_ITEM);
        else if (result == EQUIP_ERR_INVENTORY_FULL)
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_INV_FULL);
        else
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_OTHER);
    }
}

void WorldSession::HandleLootMethodOpcode(WorldPacket& recv_data)
{
    uint32 lootMethod;
    ObjectGuid lootMaster;
    uint32 lootThreshold;
    recv_data >> lootMethod >> lootMaster >> lootThreshold;

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    /** error handling **/
    if (!group->IsLeader(GetPlayer()->GetObjectGuid()))
        return;
    /********************/

    // everything is fine, do it
    group->SetLootMethod((LootMethod)lootMethod);
    group->SetMasterLooterGuid(lootMaster);
    group->SetLootThreshold((ItemQualities)lootThreshold);
    group->SendUpdate();
}

void WorldSession::HandleLootRoll(WorldPacket& recv_data)
{
    ObjectGuid lootedTarget;
    uint32 itemSlot;
    uint8  rollType;
    recv_data >> lootedTarget;                              // guid of the item rolled
    recv_data >> itemSlot;
    recv_data >> rollType;

    sLog.outDebug("WORLD RECIEVE CMSG_LOOT_ROLL, From:%s, rollType:%u", lootedTarget.GetString().c_str(), uint32(rollType));

    Group* group = _player->GetGroup();
    if (!group)
        return;

    if (rollType >= ROLL_NOT_EMITED_YET)
        return;

    sLootMgr.PlayerVote(GetPlayer(), lootedTarget, itemSlot, RollVote(rollType));
}


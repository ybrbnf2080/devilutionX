/**
 * @file multi.cpp
 *
 * Implementation of functions for keeping multiplaye games in sync.
 */

#include <SDL.h>
#include <config.h>

#include <fmt/format.h>

#include "DiabloUI/diabloui.h"
#include "diablo.h"
#include "dthread.h"
#include "engine/point.hpp"
#include "mainmenu.h"
#include "nthread.h"
#include "options.h"
#include "pfile.h"
#include "plrmsg.h"
#include "storm/storm.h"
#include "sync.h"
#include "tmsg.h"
#include "utils/endian.hpp"
#include "utils/language.h"

namespace devilution {

bool gbSomebodyWonGameKludge;
TBuffer sgHiPriBuf;
char szPlayerDescript[128];
uint16_t sgwPackPlrOffsetTbl[MAX_PLRS];
PkPlayerStruct netplr[MAX_PLRS];
bool sgbPlayerTurnBitTbl[MAX_PLRS];
bool sgbPlayerLeftGameTbl[MAX_PLRS];
DWORD sgbSentThisCycle;
bool gbShouldValidatePackage;
BYTE gbActivePlayers;
bool gbGameDestroyed;
bool sgbSendDeltaTbl[MAX_PLRS];
GameData sgGameInitInfo;
bool gbSelectProvider;
int sglTimeoutStart;
int sgdwPlayerLeftReasonTbl[MAX_PLRS];
TBuffer sgLoPriBuf;
DWORD sgdwGameLoops;
/**
 * Specifies the maximum number of players in a game, where 1
 * represents a single player game and 4 represents a multi player game.
 */
bool gbIsMultiplayer;
bool sgbTimeout;
char szPlayerName[128];
BYTE gbDeltaSender;
bool sgbNetInited;
uint32_t player_state[MAX_PLRS];

/**
 * Contains the set of supported event types supported by the multiplayer
 * event handler.
 */
const event_type event_types[3] = {
	EVENT_TYPE_PLAYER_LEAVE_GAME,
	EVENT_TYPE_PLAYER_CREATE_GAME,
	EVENT_TYPE_PLAYER_MESSAGE
};

static void buffer_init(TBuffer *pBuf)
{
	pBuf->dwNextWriteOffset = 0;
	pBuf->bData[0] = byte { 0 };
}

static void multi_copy_packet(TBuffer *buf, byte *packet, uint8_t size)
{
	if (buf->dwNextWriteOffset + size + 2 > 0x1000) {
		return;
	}

	byte *p = &buf->bData[buf->dwNextWriteOffset];
	buf->dwNextWriteOffset += size + 1;
	*p = static_cast<byte>(size);
	p++;
	memcpy(p, packet, size);
	p[size] = byte { 0 };
}

static byte *multi_recv_packet(TBuffer *pBuf, byte *body, DWORD *size)
{
	if (pBuf->dwNextWriteOffset != 0) {
		byte *src_ptr = pBuf->bData;
		while (true) {
			auto chunk_size = static_cast<uint8_t>(*src_ptr);
			if (chunk_size == 0)
				break;
			if (chunk_size > *size)
				break;
			src_ptr++;
			memcpy(body, src_ptr, chunk_size);
			body += chunk_size;
			src_ptr += chunk_size;
			*size -= chunk_size;
		}
		memcpy(pBuf->bData, src_ptr, (pBuf->bData - src_ptr) + pBuf->dwNextWriteOffset + 1);
		pBuf->dwNextWriteOffset += (pBuf->bData - src_ptr);
		return body;
	}
	return body;
}

static void NetRecvPlrData(TPkt *pkt)
{
	const Point target = plr[myplr].GetTargetPosition();

	pkt->hdr.wCheck = LoadBE32("\0\0ip");
	pkt->hdr.px = plr[myplr].position.tile.x;
	pkt->hdr.py = plr[myplr].position.tile.y;
	pkt->hdr.targx = target.x;
	pkt->hdr.targy = target.y;
	pkt->hdr.php = plr[myplr]._pHitPoints;
	pkt->hdr.pmhp = plr[myplr]._pMaxHP;
	pkt->hdr.bstr = plr[myplr]._pBaseStr;
	pkt->hdr.bmag = plr[myplr]._pBaseMag;
	pkt->hdr.bdex = plr[myplr]._pBaseDex;
}

void multi_msg_add(byte *pbMsg, BYTE bLen)
{
	if (pbMsg != nullptr && bLen != 0) {
		tmsg_add(pbMsg, bLen);
	}
}

static void multi_send_packet(int playerId, void *packet, BYTE dwSize)
{
	TPkt pkt;

	NetRecvPlrData(&pkt);
	pkt.hdr.wLen = dwSize + sizeof(pkt.hdr);
	memcpy(pkt.body, packet, dwSize);
	if (!SNetSendMessage(playerId, &pkt.hdr, pkt.hdr.wLen))
		nthread_terminate_game("SNetSendMessage0");
}

void NetSendLoPri(int playerId, byte *pbMsg, BYTE bLen)
{
	if (pbMsg != nullptr && bLen != 0) {
		multi_copy_packet(&sgLoPriBuf, pbMsg, bLen);
		multi_send_packet(playerId, pbMsg, bLen);
	}
}

void NetSendHiPri(int playerId, byte *pbMsg, BYTE bLen)
{
	DWORD size, len;
	TPkt pkt;

	if (pbMsg != nullptr && bLen != 0) {
		multi_copy_packet(&sgHiPriBuf, pbMsg, bLen);
		multi_send_packet(playerId, pbMsg, bLen);
	}
	if (!gbShouldValidatePackage) {
		gbShouldValidatePackage = true;
		NetRecvPlrData(&pkt);
		size = gdwNormalMsgSize - sizeof(TPktHdr);
		byte *hipri_body = multi_recv_packet(&sgHiPriBuf, pkt.body, &size);
		byte *lowpri_body = multi_recv_packet(&sgLoPriBuf, hipri_body, &size);
		size = sync_all_monsters(lowpri_body, size);
		len = gdwNormalMsgSize - size;
		pkt.hdr.wLen = len;
		if (!SNetSendMessage(-2, &pkt.hdr, len))
			nthread_terminate_game("SNetSendMessage");
	}
}

void multi_send_msg_packet(uint32_t pmask, byte *src, BYTE len)
{
	DWORD v, p, t;
	TPkt pkt;

	NetRecvPlrData(&pkt);
	t = len + sizeof(pkt.hdr);
	pkt.hdr.wLen = t;
	memcpy(pkt.body, src, len);
	for (v = 1, p = 0; p < MAX_PLRS; p++, v <<= 1) {
		if ((v & pmask) != 0) {
			if (!SNetSendMessage(p, &pkt.hdr, t) && SErrGetLastError() != STORM_ERROR_INVALID_PLAYER) {
				nthread_terminate_game("SNetSendMessage");
				return;
			}
		}
	}
}

static void multi_mon_seeds()
{
	int i;
	DWORD l;

	sgdwGameLoops++;
	l = (sgdwGameLoops >> 8) | (sgdwGameLoops << 24); // _rotr(sgdwGameLoops, 8)
	for (i = 0; i < MAXMONSTERS; i++)
		monster[i]._mAISeed = l + i;
}

static void multi_handle_turn_upper_bit(int pnum)
{
	int i;

	for (i = 0; i < MAX_PLRS; i++) {
		if ((player_state[i] & PS_CONNECTED) != 0 && i != pnum)
			break;
	}

	if (myplr == i) {
		sgbSendDeltaTbl[pnum] = true;
	} else if (myplr == pnum) {
		gbDeltaSender = i;
	}
}

static void multi_parse_turn(int pnum, uint32_t turn)
{
	DWORD absTurns;

	if ((turn & 0x80000000) != 0)
		multi_handle_turn_upper_bit(pnum);
	absTurns = turn & 0x7FFFFFFF;
	if (sgbSentThisCycle < gdwTurnsInTransit + absTurns) {
		if (absTurns >= 0x7FFFFFFF)
			absTurns &= 0xFFFF;
		sgbSentThisCycle = absTurns + gdwTurnsInTransit;
		sgdwGameLoops = 4 * absTurns * sgbNetUpdateRate;
	}
}

void multi_msg_countdown()
{
	int i;

	for (i = 0; i < MAX_PLRS; i++) {
		if ((player_state[i] & PS_TURN_ARRIVED) != 0) {
			if (gdwMsgLenTbl[i] == 4)
				multi_parse_turn(i, *(DWORD *)glpMsgTbl[i]);
		}
	}
}

static void multi_player_left_msg(int pnum, bool left)
{
	const char *pszFmt;

	if (plr[pnum].plractive) {
		RemovePlrFromMap(pnum);
		RemovePortalMissile(pnum);
		DeactivatePortal(pnum);
		delta_close_portal(pnum);
		RemovePlrMissiles(pnum);
		if (left) {
			pszFmt = _("Player '{:s}' just left the game");
			switch (sgdwPlayerLeftReasonTbl[pnum]) {
			case LEAVE_ENDING:
				pszFmt = _("Player '{:s}' killed Diablo and left the game!");
				gbSomebodyWonGameKludge = true;
				break;
			case LEAVE_DROP:
				pszFmt = _("Player '{:s}' dropped due to timeout");
				break;
			}
			EventPlrMsg(fmt::format(pszFmt, plr[pnum]._pName).c_str());
		}
		plr[pnum].plractive = false;
		plr[pnum]._pName[0] = '\0';
		ResetPlayerGFX(plr[pnum]);
		gbActivePlayers--;
	}
}

static void multi_clear_left_tbl()
{
	int i;

	for (i = 0; i < MAX_PLRS; i++) {
		if (sgbPlayerLeftGameTbl[i]) {
			if (gbBufferMsgs == 1)
				msg_send_drop_pkt(i, sgdwPlayerLeftReasonTbl[i]);
			else
				multi_player_left_msg(i, true);

			sgbPlayerLeftGameTbl[i] = false;
			sgdwPlayerLeftReasonTbl[i] = 0;
		}
	}
}

void multi_player_left(int pnum, int reason)
{
	sgbPlayerLeftGameTbl[pnum] = true;
	sgdwPlayerLeftReasonTbl[pnum] = reason;
	multi_clear_left_tbl();
}

void multi_net_ping()
{
	sgbTimeout = true;
	sglTimeoutStart = SDL_GetTicks();
}

static void multi_check_drop_player()
{
	int i;

	for (i = 0; i < MAX_PLRS; i++) {
		if ((player_state[i] & PS_ACTIVE) == 0 && (player_state[i] & PS_CONNECTED) != 0) {
			SNetDropPlayer(i, LEAVE_DROP);
		}
	}
}

static void multi_begin_timeout()
{
	int i, nTicks, nLowestActive, nLowestPlayer;
	BYTE bGroupPlayers, bGroupCount;

	if (!sgbTimeout) {
		return;
	}
#ifdef _DEBUG
	if (debug_mode_key_i) {
		return;
	}
#endif

	nTicks = SDL_GetTicks() - sglTimeoutStart;
	if (nTicks > 20000) {
		gbRunGame = false;
		return;
	}
	if (nTicks < 10000) {
		return;
	}

	nLowestActive = -1;
	nLowestPlayer = -1;
	bGroupPlayers = 0;
	bGroupCount = 0;
	for (i = 0; i < MAX_PLRS; i++) {
		uint32_t nState = player_state[i];
		if ((nState & PS_CONNECTED) != 0) {
			if (nLowestPlayer == -1) {
				nLowestPlayer = i;
			}
			if ((nState & PS_ACTIVE) != 0) {
				bGroupPlayers++;
				if (nLowestActive == -1) {
					nLowestActive = i;
				}
			} else {
				bGroupCount++;
			}
		}
	}

	assert(bGroupPlayers);
	assert(nLowestActive != -1);
	assert(nLowestPlayer != -1);

	if (bGroupPlayers < bGroupCount) {
		gbGameDestroyed = true;
	} else if (bGroupPlayers == bGroupCount) {
		if (nLowestPlayer != nLowestActive) {
			gbGameDestroyed = true;
		} else if (nLowestActive == myplr) {
			multi_check_drop_player();
		}
	} else if (nLowestActive == myplr) {
		multi_check_drop_player();
	}
}

/**
 * @return Always true for singleplayer
 */
bool multi_handle_delta()
{
	int i;
	bool received;

	if (gbGameDestroyed) {
		gbRunGame = false;
		return false;
	}

	for (i = 0; i < MAX_PLRS; i++) {
		if (sgbSendDeltaTbl[i]) {
			sgbSendDeltaTbl[i] = false;
			DeltaExportData(i);
		}
	}

	sgbSentThisCycle = nthread_send_and_recv_turn(sgbSentThisCycle, 1);
	if (!nthread_recv_turns(&received)) {
		multi_begin_timeout();
		return false;
	}

	sgbTimeout = false;
	if (received) {
		if (!gbShouldValidatePackage) {
			NetSendHiPri(myplr, nullptr, 0);
			gbShouldValidatePackage = false;
		} else {
			gbShouldValidatePackage = false;
			if (sgHiPriBuf.dwNextWriteOffset != 0)
				NetSendHiPri(myplr, nullptr, 0);
		}
	}
	multi_mon_seeds();

	return true;
}

static void multi_handle_all_packets(int pnum, byte *pData, int nSize)
{
	int nLen;

	while (nSize != 0) {
		nLen = ParseCmd(pnum, (TCmd *)pData);
		if (nLen == 0) {
			break;
		}
		pData += nLen;
		nSize -= nLen;
	}
}

static void multi_process_tmsgs()
{
	int cnt;
	TPkt pkt;

	while ((cnt = tmsg_get((byte *)&pkt)) != 0) {
		multi_handle_all_packets(myplr, (byte *)&pkt, cnt);
	}
}

void multi_process_network_packets()
{
	int dx, dy;
	TPktHdr *pkt;
	DWORD dwMsgSize;
	int dwID;
	bool cond;
	char *data;

	multi_clear_left_tbl();
	multi_process_tmsgs();
	while (SNetReceiveMessage(&dwID, &data, (int *)&dwMsgSize)) {
		dwRecCount++;
		multi_clear_left_tbl();
		pkt = (TPktHdr *)data;
		if (dwMsgSize < sizeof(TPktHdr))
			continue;
		if (dwID < 0 || dwID >= MAX_PLRS)
			continue;
		if (pkt->wCheck != LoadBE32("\0\0ip"))
			continue;
		if (pkt->wLen != dwMsgSize)
			continue;
		plr[dwID].position.last = { pkt->px, pkt->py };
		if (dwID != myplr) {
			assert(gbBufferMsgs != 2);
			plr[dwID]._pHitPoints = pkt->php;
			plr[dwID]._pMaxHP = pkt->pmhp;
			cond = gbBufferMsgs == 1;
			plr[dwID]._pBaseStr = pkt->bstr;
			plr[dwID]._pBaseMag = pkt->bmag;
			plr[dwID]._pBaseDex = pkt->bdex;
			if (!cond && plr[dwID].plractive && plr[dwID]._pHitPoints != 0) {
				if (currlevel == plr[dwID].plrlevel && !plr[dwID]._pLvlChanging) {
					dx = abs(plr[dwID].position.tile.x - pkt->px);
					dy = abs(plr[dwID].position.tile.y - pkt->py);
					if ((dx > 3 || dy > 3) && dPlayer[pkt->px][pkt->py] == 0) {
						FixPlrWalkTags(dwID);
						plr[dwID].position.old = plr[dwID].position.tile;
						FixPlrWalkTags(dwID);
						plr[dwID].position.tile = { pkt->px, pkt->py };
						plr[dwID].position.future = { pkt->px, pkt->py };
						dPlayer[plr[dwID].position.tile.x][plr[dwID].position.tile.y] = dwID + 1;
					}
					dx = abs(plr[dwID].position.future.x - plr[dwID].position.tile.x);
					dy = abs(plr[dwID].position.future.y - plr[dwID].position.tile.y);
					if (dx > 1 || dy > 1) {
						plr[dwID].position.future = plr[dwID].position.tile;
					}
					MakePlrPath(dwID, { pkt->targx, pkt->targy }, true);
				} else {
					plr[dwID].position.tile = { pkt->px, pkt->py };
					plr[dwID].position.future = { pkt->px, pkt->py };
				}
			}
		}
		multi_handle_all_packets(dwID, (byte *)(pkt + 1), dwMsgSize - sizeof(TPktHdr));
	}
	if (SErrGetLastError() != STORM_ERROR_NO_MESSAGES_WAITING)
		nthread_terminate_game("SNetReceiveMsg");
}

void multi_send_zero_packet(int pnum, _cmd_id bCmd, byte *pbSrc, DWORD dwLen)
{
	DWORD dwOffset, dwBody, dwMsg;
	TPkt pkt;
	TCmdPlrInfoHdr *p;

	assert(pnum != myplr);
	assert(pbSrc);
	assert(dwLen <= 0x0ffff);

	dwOffset = 0;

	while (dwLen != 0) {
		pkt.hdr.wCheck = LoadBE32("\0\0ip");
		pkt.hdr.px = 0;
		pkt.hdr.py = 0;
		pkt.hdr.targx = 0;
		pkt.hdr.targy = 0;
		pkt.hdr.php = 0;
		pkt.hdr.pmhp = 0;
		pkt.hdr.bstr = 0;
		pkt.hdr.bmag = 0;
		pkt.hdr.bdex = 0;
		p = (TCmdPlrInfoHdr *)pkt.body;
		p->bCmd = bCmd;
		p->wOffset = dwOffset;
		dwBody = gdwLargestMsgSize - sizeof(pkt.hdr) - sizeof(*p);
		if (dwLen < dwBody) {
			dwBody = dwLen;
		}
		assert(dwBody <= 0x0ffff);
		p->wBytes = dwBody;
		memcpy(&pkt.body[sizeof(*p)], pbSrc, p->wBytes);
		dwMsg = sizeof(pkt.hdr);
		dwMsg += sizeof(*p);
		dwMsg += p->wBytes;
		pkt.hdr.wLen = dwMsg;
		if (!SNetSendMessage(pnum, &pkt, dwMsg)) {
			nthread_terminate_game("SNetSendMessage2");
			return;
		}
#if 0
		if((DWORD)pnum >= MAX_PLRS) {
			if(myplr != 0) {
				debug_plr_tbl[0]++;
			}
			if(myplr != 1) {
				debug_plr_tbl[1]++;
			}
			if(myplr != 2) {
				debug_plr_tbl[2]++;
			}
			if(myplr != 3) {
				debug_plr_tbl[3]++;
			}
		} else {
			debug_plr_tbl[pnum]++;
		}
#endif
		pbSrc += p->wBytes;
		dwLen -= p->wBytes;
		dwOffset += p->wBytes;
	}
}

static void multi_send_pinfo(int pnum, _cmd_id cmd)
{
	PkPlayerStruct pkplr;

	PackPlayer(&pkplr, plr[myplr], true);
	dthread_send_delta(pnum, cmd, (byte *)&pkplr, sizeof(pkplr));
}

static dungeon_type InitLevelType(int l)
{
	if (l == 0)
		return DTYPE_TOWN;
	if (l >= 1 && l <= 4)
		return DTYPE_CATHEDRAL;
	if (l >= 5 && l <= 8)
		return DTYPE_CATACOMBS;
	if (l >= 9 && l <= 12)
		return DTYPE_CAVES;
	if (l >= 13 && l <= 16)
		return DTYPE_HELL;
	if (l >= 21 && l <= 24)
		return DTYPE_CATHEDRAL; // Crypt
	if (l >= 17 && l <= 20)
		return DTYPE_CAVES; // Hive

	return DTYPE_CATHEDRAL;
}

static void SetupLocalCoords()
{
	int x, y;

	if (!leveldebug || gbIsMultiplayer) {
		currlevel = 0;
		leveltype = DTYPE_TOWN;
		setlevel = false;
	}
	x = 75;
	y = 68;
#ifdef _DEBUG
	if (debug_mode_key_inverted_v) {
		x = 49;
		y = 23;
	}
#endif
	x += plrxoff[myplr];
	y += plryoff[myplr];
	plr[myplr].position.tile = { x, y };
	plr[myplr].position.future = { x, y };
	plr[myplr].plrlevel = currlevel;
	plr[myplr]._pLvlChanging = true;
	plr[myplr].pLvlLoad = 0;
	plr[myplr]._pmode = PM_NEWLVL;
	plr[myplr].destAction = ACTION_NONE;
}

static void multi_handle_events(_SNETEVENT *pEvt)
{
	DWORD LeftReason;

	switch (pEvt->eventid) {
	case EVENT_TYPE_PLAYER_CREATE_GAME: {
		auto *gameData = (GameData *)pEvt->data;
		if (gameData->size != sizeof(GameData))
			app_fatal("Invalid size of game data: %i", gameData->size);
		sgGameInitInfo = *gameData;
		sgbPlayerTurnBitTbl[pEvt->playerid] = true;
		break;
	}
	case EVENT_TYPE_PLAYER_LEAVE_GAME:
		sgbPlayerLeftGameTbl[pEvt->playerid] = true;
		sgbPlayerTurnBitTbl[pEvt->playerid] = false;

		LeftReason = 0;
		if (pEvt->data != nullptr && pEvt->databytes >= sizeof(DWORD))
			LeftReason = *(DWORD *)pEvt->data;
		sgdwPlayerLeftReasonTbl[pEvt->playerid] = LeftReason;
		if (LeftReason == LEAVE_ENDING)
			gbSomebodyWonGameKludge = true;

		sgbSendDeltaTbl[pEvt->playerid] = false;
		dthread_remove_player(pEvt->playerid);

		if (gbDeltaSender == pEvt->playerid)
			gbDeltaSender = MAX_PLRS;
		break;
	case EVENT_TYPE_PLAYER_MESSAGE:
		ErrorPlrMsg((char *)pEvt->data);
		break;
	}
}

static void multi_event_handler(bool add)
{
	DWORD i;
	bool (*fn)(event_type, SEVTHANDLER);

	if (add)
		fn = SNetRegisterEventHandler;
	else
		fn = SNetUnregisterEventHandler;

	for (i = 0; i < 3; i++) {
		if (!fn(event_types[i], multi_handle_events) && add) {
			app_fatal("SNetRegisterEventHandler:\n%s", SDL_GetError());
		}
	}
}

void NetClose()
{
	if (!sgbNetInited) {
		return;
	}

	sgbNetInited = false;
	nthread_cleanup();
	dthread_cleanup();
	tmsg_cleanup();
	multi_event_handler(false);
	SNetLeaveGame(3);
	if (gbIsMultiplayer)
		SDL_Delay(2000);
}

bool NetInit(bool bSinglePlayer)
{
	while (true) {
		SetRndSeed(0);
		sgGameInitInfo.size = sizeof(sgGameInitInfo);
		sgGameInitInfo.dwSeed = time(nullptr);
		sgGameInitInfo.programid = GAME_ID;
		sgGameInitInfo.versionMajor = PROJECT_VERSION_MAJOR;
		sgGameInitInfo.versionMinor = PROJECT_VERSION_MINOR;
		sgGameInitInfo.versionPatch = PROJECT_VERSION_PATCH;
		sgGameInitInfo.nTickRate = sgOptions.Gameplay.nTickRate;
		sgGameInitInfo.bRunInTown = sgOptions.Gameplay.bRunInTown ? 1 : 0;
		sgGameInitInfo.bTheoQuest = sgOptions.Gameplay.bTheoQuest ? 1 : 0;
		sgGameInitInfo.bCowQuest = sgOptions.Gameplay.bCowQuest ? 1 : 0;
		sgGameInitInfo.bFriendlyFire = sgOptions.Gameplay.bFriendlyFire ? 1 : 0;
		memset(sgbPlayerTurnBitTbl, 0, sizeof(sgbPlayerTurnBitTbl));
		gbGameDestroyed = false;
		memset(sgbPlayerLeftGameTbl, 0, sizeof(sgbPlayerLeftGameTbl));
		memset(sgdwPlayerLeftReasonTbl, 0, sizeof(sgdwPlayerLeftReasonTbl));
		memset(sgbSendDeltaTbl, 0, sizeof(sgbSendDeltaTbl));
		for (auto &player : plr) {
			player.Reset();
		}
		memset(sgwPackPlrOffsetTbl, 0, sizeof(sgwPackPlrOffsetTbl));
		SNetSetBasePlayer(0);
		if (bSinglePlayer) {
			if (!multi_init_single(&sgGameInitInfo))
				return false;
		} else {
			if (!multi_init_multi(&sgGameInitInfo))
				return false;
		}
		sgbNetInited = true;
		sgbTimeout = false;
		delta_init();
		InitPlrMsg();
		buffer_init(&sgHiPriBuf);
		buffer_init(&sgLoPriBuf);
		gbShouldValidatePackage = false;
		sync_init();
		nthread_start(sgbPlayerTurnBitTbl[myplr]);
		dthread_start();
		tmsg_start();
		sgdwGameLoops = 0;
		sgbSentThisCycle = 0;
		gbDeltaSender = myplr;
		gbSomebodyWonGameKludge = false;
		nthread_send_and_recv_turn(0, 0);
		SetupLocalCoords();
		multi_send_pinfo(-2, CMD_SEND_PLRINFO);

		ResetPlayerGFX(plr[myplr]);
		plr[myplr].plractive = true;
		gbActivePlayers = 1;

		if (!sgbPlayerTurnBitTbl[myplr] || msg_wait_resync())
			break;
		NetClose();
		gbSelectProvider = false;
	}
	SetRndSeed(sgGameInitInfo.dwSeed);
	gnTickDelay = 1000 / sgGameInitInfo.nTickRate;

	for (int i = 0; i < NUMLEVELS; i++) {
		glSeedTbl[i] = AdvanceRndSeed();
		gnLevelTypeTbl[i] = InitLevelType(i);
	}
	if (!SNetGetGameInfo(GAMEINFO_NAME, szPlayerName, 128))
		nthread_terminate_game("SNetGetGameInfo1");
	if (!SNetGetGameInfo(GAMEINFO_PASSWORD, szPlayerDescript, 128))
		nthread_terminate_game("SNetGetGameInfo2");

	return true;
}

bool multi_init_single(GameData *gameData)
{
	int unused;

	if (!SNetInitializeProvider(SELCONN_LOOPBACK, gameData)) {
		SErrGetLastError();
		return false;
	}

	unused = 0;
	if (!SNetCreateGame("local", "local", (char *)&sgGameInitInfo, sizeof(sgGameInitInfo), &unused)) {
		app_fatal("SNetCreateGame1:\n%s", SDL_GetError());
	}

	myplr = 0;
	gbIsMultiplayer = false;

	return true;
}

bool multi_init_multi(GameData *gameData)
{
	int playerId;

	while (true) {
		if (gbSelectProvider && !UiSelectProvider(gameData)) {
			return false;
		}

		multi_event_handler(true);
		if (UiSelectGame(gameData, &playerId))
			break;

		gbSelectProvider = true;
	}

	if ((DWORD)playerId >= MAX_PLRS) {
		return false;
	}
	myplr = playerId;
	gbIsMultiplayer = true;

	pfile_read_player_from_save(gszHero, myplr);

	return true;
}

void recv_plrinfo(int pnum, TCmdPlrInfoHdr *p, bool recv)
{
	const char *szEvent;

	if (myplr == pnum) {
		return;
	}
	assert((DWORD)pnum < MAX_PLRS);
	auto &player = plr[pnum];

	if (sgwPackPlrOffsetTbl[pnum] != p->wOffset) {
		sgwPackPlrOffsetTbl[pnum] = 0;
		if (p->wOffset != 0) {
			return;
		}
	}
	if (!recv && sgwPackPlrOffsetTbl[pnum] == 0) {
		multi_send_pinfo(pnum, CMD_ACK_PLRINFO);
	}

	memcpy((char *)&netplr[pnum] + p->wOffset, &p[1], p->wBytes); /* todo: cast? */
	sgwPackPlrOffsetTbl[pnum] += p->wBytes;
	if (sgwPackPlrOffsetTbl[pnum] != sizeof(*netplr)) {
		return;
	}

	sgwPackPlrOffsetTbl[pnum] = 0;
	multi_player_left_msg(pnum, false);
	UnPackPlayer(&netplr[pnum], pnum, true);

	if (!recv) {
		return;
	}

	ResetPlayerGFX(player);
	player.plractive = true;
	gbActivePlayers++;

	if (sgbPlayerTurnBitTbl[pnum]) {
		szEvent = _("Player '{:s}' (level {:d}) just joined the game");
	} else {
		szEvent = _("Player '{:s}' (level {:d}) is already in the game");
	}
	EventPlrMsg(fmt::format(szEvent, player._pName, player._pLevel).c_str());

	SyncInitPlr(pnum);

	if (player.plrlevel != currlevel) {
		return;
	}

	if (player._pHitPoints >> 6 > 0) {
		StartStand(pnum, DIR_S);
		return;
	}

	player._pgfxnum = 0;
	player._pmode = PM_DEATH;
	NewPlrAnim(player, player_graphic::Death, DIR_S, player._pDFrames, 1);
	player.AnimInfo.CurrentFrame = player.AnimInfo.NumberOfFrames - 1;
	dFlags[player.position.tile.x][player.position.tile.y] |= BFLAG_DEAD_PLAYER;
}

} // namespace devilution

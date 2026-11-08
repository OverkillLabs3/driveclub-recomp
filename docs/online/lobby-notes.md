# Private lobbies: what the game expects (work in progress)

Findings from the 1.28 eboot (CUSA00093, sets NP title CUSA00003_00), for building `lobbyService` and
pushed lobby messages on the community server. Guest addresses are 0x800000000 + ELF vaddr.

## Invitations (working)

- Sending: `POST /v1/gameCustomData/items`, multipart: JSON `message` part (`to`, `item.gameDataDetail`),
  32 bytes `game-data-binary-0` (type 6, sender account id, lobby id), a JPEG thumbnail.
- Receiving: system event `GAME_CUSTOM_DATA` (0x10000004). The game's `SystemServiceHandler` thread
  (guest 0x8007aedd0, jump table 0x8007aef7c) copies 32 bytes of parameter: item id (u64), online id
  (16 chars, terminator, 3 pad), user id. It then `GET /v1/users/<id>/gameCustomData/items/<item>/gameData`
  and `PUT /v1/users//gameCustomData/items/0`.
- The runtime only gets asked for events while `sceSystemServiceGetStatus` reports `event_num > 0`.

## Pushed EGS messages (`GET /egs/messaging`)

The body is an XML stream read by `EvoSliccPersistentAction` (guest 0x80062f127 area). Each message is

    <msg t="TypeName" s="Size">BASE64</msg>

- `t` selects the handler registered under that name (registration 0x800628cd0 / 0x800628b80,
  for example `LobbyCallbackMessage`, `SessionCallbackMessage`, `_MatchingCallbackMessage`).
- The content is decoded (0x80079e010, base64 by all signs) and parsed as a protobuf message of type `t`
  from the descriptor the server sends (`getMessageDescriptors`), then turned into a dictionary by
  field names (0x8006303b0).
- Unverified: the root element, whether `s` is the decoded or encoded size, and whether the response
  needs `X-Response-Signature`.

## lobbyService

Methods registered at 0x800f34e1d: `createLobby`, `getLobby`, `joinWithAccount`, `leaveLobbyById`,
`leaveCurrentLobby`, `removeFromLobby`, `changeHost`, `progressToNextRace`, `updateMember`,
`updateLobby`.

`createLobby` request (built at 0x800473600, field paths as set):

- `lobby.name` = "_SCRUTINY_" (string)
- `lobby.lobbyType` (int)
- `member.sessionKey.accountId` (int)
- `member.sessionKey.username` (string)

## LobbyCallbackMessage (handler 0x800f33d60)

Read with dictionary accessors (getInt 0x800255770, getInt64 0x800255110, getDict 0x800254990,
getString 0x800254aa0, list/dict 0x800254750):

- `action` (int, an enum; values handled: 7, 0xa, 0xf, 0x10 and those in mask 0x1d5d0)
- `lobby` (dict): `lobbyType` (int), `matchSlotId` (int64), `eventListId` (int64), `status` (int;
  8 and 2 are handled for action 7), `membersList`, `host.sessionKey.accountId`
- top level: `oldMatchSlotId` (int64), `updatedMembers`
- members: `sessionKey.accountId`, `playerNumber`, `memberData`, `clientVersion`, `aiControlled`

Lobby dictionaries returned from calls are checked for `id` (string), `lobbyType` (int), `status`
(int) and `membersList` (dictionary) (messages near "Lobby Variant does not contain ...").

## What happens when joining (test of 28.10., two players on one PC)

- The game creates a type 1 lobby by itself after login (`createLobby`), and asks `getLobby` for
  types 1 and 0 after every login. Creating a private lobby in the menu sends nothing to the server.
- The invitee shows "Joining lobby" for about 14 s without any request (no lobbyService call, no
  NpSignaling, no Rudp), then "There was a problem communicating with the DRIVECLUB server" exactly
  when the 30 s re-login comes round; both players drop to the main menu.
- The 30 s re-login: `egs_oAuthRefreshTime` (TSS, seconds) sets another timer and does not change it.
  Patching the 30000 ms constants at 0x800630f75, 0x8006318e2, 0x800631b29 and 0x800631cff did not
  change it either. State 0xb (guest 0x8006331f1) is set after a successful `getVersion`, not on an
  error; the re-login path at 0x800633bf7 runs while that state holds.
- `/egs/messaging` is not a persistent stream: with streaming the game restarts the request after
  each piece of data. Streaming stays available behind `BB_COOP_STREAM=1`.

## Fixed: the 30 s disconnect

Found with BB_GUEST_TRACE: guest 0x8006254ba compares the time since the last EGS activity with the
TSS value `egs_timeout` (seconds, default 30) and on expiry logs "EGS Connection Timed Out" and resets
the session (0x8006254f9), which logs in again. `egs_timeout 3600` in override-0.tss ends the cycle.
Cleaner would be the server sending whatever the game counts as activity (probably `<p>` pings on
/egs/messaging); not worked out yet.

Next for joining: the invitee still sends nothing after accepting. Candidates to trace: the invite
alert callback (vtable 0x8015c0150: 0x80024b750, 0x80024b850), the joinWithAccount request builder
(0x800239780), and the code around "Room Join complete - OK" (0x8012e7de4) and "JoinRoom".

## Joining, state of 28.10. afternoon

- After accepting, the invitee PUTs `{"dataUsedFlag":true}` to `/v1/users//gameCustomData/items/0`;
  the server now answers 204 (it answered 404 before). The invitee then shows "Joining lobby"
  indefinitely and starts no EGS action at all (trace of the EGS call builder 0x800627f60), no
  Matching2 call (the game never initializes Matching2) and no NpSignaling connection.
- The host sends nothing either when it creates the private lobby (no createLobby, updateLobby or
  matchingService call); its only lobby on the server is the type 1 one it makes after login.
- New services the game polls: systemService.getTimeInfo and
  configurationService.getGlobalGameConfiguration are answered now; still missing are
  transactionService.newTransaction, accountService.quickSearch, profileService.getFame,
  statsService.getAccountStats, objectiveService.getPlayerAccoladeProgress.
- BB_GUEST_SAMPLE during "Joining lobby" shows the EGS messaging action (0x80062eef0, 0x80062fa40)
  busier and two worker loops (0x800103770, 0x800103c80, ghost data) that are not join logic.
- Guess to check next: the invitee waits for a pushed message (LobbyCallbackMessage or
  SessionCallbackMessage) that on the real servers followed from something the host did.

## Still open

- Request messages of the other lobbyService methods and the response type of each.
- Meaning of the `action` values.
- The in-race connection: rooms (`DCRM`, `RoomJoin`, mesh) over NpSignaling and `sceRudp`, which is a
  stub in this runtime.

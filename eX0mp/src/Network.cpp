#include "globals.h"

ServerConnection *	pServer = NULL;
//SOCKET			nServerTcpSocket = INVALID_SOCKET;
//SOCKET			nServerUdpSocket = INVALID_SOCKET;
sockaddr_in	oLocalUdpAddress;		// Get the local UDP port
socklen_t			nLocalUdpAddressLength = sizeof(oLocalUdpAddress);
//sockaddr_in	oServerAddress;
//volatile int	nJoinStatus = DISCONNECTED;
//u_char			cSignature[SIGNATURE_SIZE];
u_int			nSendUdpHandshakePacketEventId = 0;

GLFWthread		oNetworkThread = -1;
volatile bool	bNetworkThreadRun;
GLFWmutex		oTcpSendMutex;
GLFWmutex		oUdpSendMutex;

u_char			cCurrentCommandSequenceNumber = 0;
//u_char			cLastAckedCommandSequenceNumber = 0;
u_char			cLastUpdateSequenceNumber = 0;
GLFWmutex		oPlayerTick;

IndexedCircularBuffer<Move_t, u_char>	oUnconfirmedMoves;

MovingAverage	oRecentLatency(60.0, 10);
MovingAverage	oRecentTimeDifference(60.0, 10);
HashMatcher<PingData_t, double>	oPongSentTimes(PING_SENT_TIMES_HISTORY);
vector<double>	oSentTimeRequestPacketTimes(256);
u_char			cNextTimeRequestSequenceNumber = 0;
double			dShortestLatency = 1000;
double			dShortestLatencyLocalTime;
double			dShortestLatencyRemoteTime;
u_int			nTrpReceived = 0;
u_int			nSendTimeRequestPacketEventId = 0;

u_char			cCommandRate = 20;
u_char			cUpdateRate = 20;

const float		kfInterpolate = 0.1f;
const float		kfMaxExtrapolate = 0.5f;

bool			bGotPermissionToEnter = false;
bool			bFinishedSyncingClock = false;
GLFWmutex		oJoinGameMutex;

// Initialize the networking component
bool NetworkInit()
{
	oTcpSendMutex = glfwCreateMutex();
	oUdpSendMutex = glfwCreateMutex();
	oPlayerTick = glfwCreateMutex();
	oJoinGameMutex = glfwCreateMutex();

#ifdef WIN32
	WSADATA wsaData;
	int nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (nResult != 0)
		printf("Error at WSAStartup(): %d.\n", nResult);
	else if (!(LOBYTE(wsaData.wVersion) == 2 && HIBYTE(wsaData.wVersion) == 2)) {
		printf("Error: Winsock version 2.2 is not avaliable.\n");
		WSACleanup();
		return false;
	}

	return nResult == 0;
#else // Linux
	// Nothing to be done on Linux
	return true;
#endif
}

void NetworkPrintError(const char *szMessage)
{
#ifdef WIN32
	//printf("%s: %d\n", szMessage, WSAGetLastError());
	printf("%s\n", WSAGetLastErrorMessage(szMessage, WSAGetLastError()));
#else // Linux
	perror(szMessage);
#endif
}

int sendall(SOCKET s, const char *buf, int len, int flags)
{
	glfwLockMutex(oTcpSendMutex);

	int total = 0;        // how many bytes we've sent
	int bytesleft = len; // how many we have left to send
	int n = 0;

	while (total < len) {
		n = send(s, buf+total, bytesleft, flags);
		if (n == SOCKET_ERROR) { break; }
		total += n;
		bytesleft -= n;
	}

	//*len = total; // return number actually sent here

	glfwUnlockMutex(oTcpSendMutex);

	return (n == SOCKET_ERROR || total != len) ? SOCKET_ERROR : total; // return SOCKET_ERROR on failure, bytes sent on success
}

int sendudp(SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen)
{
	glfwLockMutex(oUdpSendMutex);

	int nResult;
	if (to != NULL) nResult = sendto(s, buf, len, flags, to, tolen);
	else nResult = send(s, buf, len, flags);

	glfwUnlockMutex(oUdpSendMutex);

	return nResult;
}

void SendTimeRequestPacket(void *)
{
	static u_int nTrpSent = 0;

	// Send a time request packet
	CPacket oTimeRequestPacket;
	oTimeRequestPacket.pack("cc", (u_char)105, cNextTimeRequestSequenceNumber);
	oSentTimeRequestPacketTimes.at(cNextTimeRequestSequenceNumber) = glfwGetTime();
	++cNextTimeRequestSequenceNumber;
	pServer->SendUdp(oTimeRequestPacket, UDP_CONNECTED);

	//printf("Sent TRqP #%d at %.5lf ms\n", ++nTrpSent, glfwGetTime() * 1000);
}

// Connect to a server
bool NetworkConnect(const char * szHostname, u_short nPort)
{
	pServer = new ServerConnection();
	if (!pServer->Connect(szHostname, nPort)) {
		return false;
	}

	// Create the networking thread
	NetworkCreateThread();

	// Create and send a Join Server Request packet
	pServer->GenerateSignature();
	CPacket oJoinServerRequestPacket;
	oJoinServerRequestPacket.pack("hchs", 0, (u_char)1, 1, "somerandompass01");
	for (int nSignatureByte = 0; nSignatureByte < NetworkConnection::m_knSignatureSize; ++nSignatureByte)
		oJoinServerRequestPacket.pack("c", pServer->GetSignature()[nSignatureByte]);
	oJoinServerRequestPacket.CompleteTpcPacketSize();
	pServer->SendTcp(oJoinServerRequestPacket, TCP_CONNECTED);

	return true;
}

bool NetworkCreateThread()
{
	bNetworkThreadRun = true;
	oNetworkThread = glfwCreateThread(&NetworkThread, NULL);

	printf("Network thread (tid = %d) created.\n", oNetworkThread);

	return (oNetworkThread >= 0);
}

void GLFWCALL NetworkThread(void *)
{
	int fdmax;
	fd_set master;   // master file descriptor list
	fd_set read_fds;

	int			nbytes;
	u_char		buf[2 * MAX_TCP_PACKET_SIZE - 1];
	u_char		cUdpBuffer[MAX_UDP_PACKET_SIZE];
	u_short		snCurrentPacketSize = 0;

	// clear the master and temp sets
	FD_ZERO(&master);
	FD_ZERO(&read_fds);

	// Add the sockets to select on
	FD_SET(pServer->GetTcpSocket(), &master);
	FD_SET(pServer->GetUdpSocket(), &master);

	fdmax = std::max<int>((int)pServer->GetTcpSocket(), (int)pServer->GetUdpSocket());

	while (bNetworkThreadRun)
	{
		read_fds = master; // copy it
		if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == SOCKET_ERROR) {
			NetworkPrintError("select");
			Terminate(1);
		}

		if (!bNetworkThreadRun) {
			break;
		}
		// have TCP data
		else if (FD_ISSET(pServer->GetTcpSocket(), &read_fds))
		{
			// got error or connection closed by server
			if ((nbytes = recv(pServer->GetTcpSocket(), reinterpret_cast<char *>(buf) + snCurrentPacketSize, sizeof(buf) - snCurrentPacketSize, 0)) <= 0) {
				if (nbytes == 0) {
					// Connection closed gracefully by the server
					printf("Connection closed gracefully by the server.\n");
				} else {
					NetworkPrintError("recv");
					printf("Lost connection to the server.\n");
				}
				bNetworkThreadRun = false;
				pServer->SetJoinStatus(DISCONNECTED);
				iGameState = 1;
			// we got some data from a client, process it
			} else {
				//printf("Got %d bytes from server\n", nbytes);

				snCurrentPacketSize += static_cast<u_short>(nbytes);
				eX0_assert(snCurrentPacketSize <= sizeof(buf), "snCurrentPacketSize <= sizeof(buf)");
				// Check if received enough to check the packet size
				u_short snRealPacketSize = MAX_TCP_PACKET_SIZE;
				if (snCurrentPacketSize >= 2)
					snRealPacketSize = 3 + ntohs(*reinterpret_cast<u_short *>(buf));
				if (snRealPacketSize > MAX_TCP_PACKET_SIZE) {		// Make sure the packet is not larger than allowed
					printf("Got a TCP packet that's larger than allowed.\n");
					snRealPacketSize = MAX_TCP_PACKET_SIZE;
				}
				// Received an entire packet
				while (snCurrentPacketSize >= snRealPacketSize)
				{
					// Process it
					CPacket oPacket(buf, snRealPacketSize);
					if (!NetworkProcessTcpPacket(oPacket)) {
						printf("Couldn't process a TCP packet (type %d):\n  ", *reinterpret_cast<u_char *>(buf + 2));
						oPacket.Print();
					}

					//memmove(buf, buf + snRealPacketSize, sizeof(buf) - snRealPacketSize);
					//snCurrentPacketSize -= snRealPacketSize;
					snCurrentPacketSize -= snRealPacketSize;
					eX0_assert(snCurrentPacketSize <= sizeof(buf) - snRealPacketSize, "snCurrentPacketSize <= sizeof(buf) - snRealPacketSize");
					memmove(buf, buf + snRealPacketSize, snCurrentPacketSize);

					if (snCurrentPacketSize >= 2)
						snRealPacketSize = 3 + ntohs(*reinterpret_cast<u_short *>(buf));
					else snRealPacketSize = MAX_TCP_PACKET_SIZE;
					if (snRealPacketSize > MAX_TCP_PACKET_SIZE) {		// Make sure the packet is not larger than allowed
						printf("Got a TCP packet that's larger than allowed.\n");
						snRealPacketSize = MAX_TCP_PACKET_SIZE;
					}
				}
			}
		}

		if (!bNetworkThreadRun) {
			break;
		}
		// have UDP data
		else if (FD_ISSET(pServer->GetUdpSocket(), &read_fds))
		{
			// handle UDP data from the server
			if ((nbytes = recv(pServer->GetUdpSocket(), reinterpret_cast<char *>(cUdpBuffer), sizeof(cUdpBuffer), 0)) == SOCKET_ERROR)
			{
				// Error
				NetworkPrintError("recv");
			} else {
				// Got a UDP packet
				//printf("Got a UDP %d byte packet from server!\n", nbytes);

				// Process the received UDP packet
				CPacket oPacket(cUdpBuffer, nbytes);
				if (!NetworkProcessUdpPacket(oPacket)) {
					printf("Couldn't process a UDP packet (type %d):\n  ", cUdpBuffer[0]);
					oPacket.Print();
				}
			}
		}

		// There's no need to Sleep here, since select will essentially do that automatically whenever there's no data
	}

	// Clean up, if Deinit() hasn't done so already
	if (pTimedEventScheduler != NULL)
		pTimedEventScheduler->RemoveAllEvents();

	printf("Network thread has ended.\n");
	oNetworkThread = -1;
}

// Process a received TCP packet
bool NetworkProcessTcpPacket(CPacket & oPacket)
{
	u_short nDataSize; oPacket.unpack("h", &nDataSize);
	u_char cPacketType; oPacket.unpack("c", &cPacketType);

if (glfwGetKey(GLFW_KEY_TAB) == GLFW_RELEASE)
fTempFloat = static_cast<float>(glfwGetTime());

	switch (cPacketType) {
	// Join Server Accept
	case 2:
		// Check if we have already been accepted by the server
		if (pServer->GetJoinStatus() >= ACCEPTED) {
			printf("Error: Already accepted, but received another Join Server Accept packet.\n");
			return false;
		}

		if (nDataSize != 2) return false;		// Check packet size
		else {
			u_char cLocalPlayerId;
			u_char cPlayerCount;
			oPacket.unpack("cc", &cLocalPlayerId, (char *)&cPlayerCount);

			iLocalPlayerID = (int)cLocalPlayerId;
			nPlayerCount = (int)cPlayerCount;

			//PlayerInit();
			pLocalPlayer = new LCtrlRAuthPlayer(static_cast<u_int>(cLocalPlayerId));

			// TODO: Player name (and other local settings?) needs to be assigned, validated (and corrected if needed) in a better way
			pLocalPlayer->SetName(sLocalPlayerName);

			// Got successfully accepted in game by the server
			pServer->SetJoinStatus(ACCEPTED);
			printf("Got accepted in game: local player id = %d, player count = %d\n", iLocalPlayerID, nPlayerCount);

			// Bind the UDP socket to the server
			// This will have to be done in ServerConnection, if at all... Gotta think if it's worth having two different SendUdp()s
			/*if (connect(pServer->GetUdpSocket(), (const sockaddr *)&pServer->GetUdpAddress(), sizeof(pServer->GetUdpAddress())) == SOCKET_ERROR) {
				NetworkPrintError("connect");
				Terminate(1);
			}
			if (getsockname(pServer->GetUdpSocket(), (sockaddr *)&oLocalUdpAddress, &nLocalUdpAddressLength) == SOCKET_ERROR) {
				NetworkPrintError("getsockname");
				Terminate(1);
			}
			printf("Created a pending UDP connection to server on port %d.\n", ntohs(oLocalUdpAddress.sin_port));*/

			// Send UDP packet with the same signature to initiate the UDP handshake
			// Add timed event (Retransmit UdpHandshake packet every 100 milliseconds)
			CTimedEvent oEvent(glfwGetTime(), 0.1, &NetworkSendUdpHandshakePacket, NULL);
			nSendUdpHandshakePacketEventId = oEvent.GetId();
			pTimedEventScheduler->ScheduleEvent(oEvent);
		}
		break;
	// Join Game Refuse
	case 3:
		// Check if we have already joined the game
		if (pServer->GetJoinStatus() >= IN_GAME) {
			printf("Error: Already in game, but received a Join Game Refuse packet.\n");
			return false;
		}

		if (nDataSize != 1) return false;		// Check packet size
		else {
			char cRefuseReason;
			oPacket.unpack("c", &cRefuseReason);

			printf("Got refused with reason %d.\n", (int)cRefuseReason);

			bNetworkThreadRun = false;
			pServer->SetJoinStatus(DISCONNECTED);		// Server connection status is fully unconnected
			iGameState = 1;
		}
		break;
	// UDP Connection Established
	case 5:
		// Check if we have been accepted by the server
		if (pServer->GetJoinStatus() < ACCEPTED) {
			printf("Error: Not yet accepted by the server.\n");
			return false;
		}

		if (nDataSize != 0) return false;		// Check packet size
		else {
			// The UDP connection with the server is fully established
			pServer->SetJoinStatus(UDP_CONNECTED);
			printf("Established a UDP connection with the server.\n");

			// Stop sending UDP handshake packets
			pTimedEventScheduler->RemoveEventById(nSendUdpHandshakePacketEventId);

			// Start syncing the clock, send a Time Request packet every 50 ms
			CTimedEvent oEvent(glfwGetTime(), 0.05, SendTimeRequestPacket, NULL);
			nSendTimeRequestPacketEventId = oEvent.GetId();
			pTimedEventScheduler->ScheduleEvent(oEvent);

			// Send a Local Player Info packet
			CPacket oLocalPlayerInfoPacket;
			oLocalPlayerInfoPacket.pack("hc", 0, (u_char)30);
			oLocalPlayerInfoPacket.pack("c", (u_char)pLocalPlayer->GetName().length());
			oLocalPlayerInfoPacket.pack("t", &pLocalPlayer->GetName());
			oLocalPlayerInfoPacket.pack("cc", cCommandRate, cUpdateRate);
			oLocalPlayerInfoPacket.CompleteTpcPacketSize();
			pServer->SendTcp(oLocalPlayerInfoPacket, UDP_CONNECTED);

			// We should be a Public client by now
			pServer->SetJoinStatus(PUBLIC_CLIENT);
		}
		break;
	// Enter Game Permission
	case 6:
		// Check if we have fully joined the server
		if (pServer->GetJoinStatus() < PUBLIC_CLIENT) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (nDataSize != 0) return false;		// Check packet size
		else {
			glfwLockMutex(oJoinGameMutex);
			if (bFinishedSyncingClock)
			{
				// Join Game
				NetworkJoinGame();
			}
			else
				bGotPermissionToEnter = true;
			glfwUnlockMutex(oJoinGameMutex);
		}
		break;
	// Broadcast Text Message
	case 11:
		// Check if we have entered the game
		if (pServer->GetJoinStatus() < IN_GAME) {
			printf("Error: Not yet entered the game.\n");
			return false;
		}

		if (nDataSize < 2) return false;		// Check packet size
		else {
			char cPlayerID;
			string sTextMessage;
			oPacket.unpack("cet", (char *)&cPlayerID, nDataSize - 1, &sTextMessage);

			// Print out the text message
			pChatMessages->AddMessage(PlayerGet(cPlayerID)->GetName() + ": " + sTextMessage);
			printf("%s\n", (PlayerGet(cPlayerID)->GetName() + ": " + sTextMessage).c_str());
		}

		break;
	// Load Level
	case 20:
		// Check if we have fully joined the server
		if (pServer->GetJoinStatus() < PUBLIC_CLIENT) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (nDataSize < 1) return false;		// Check packet size
		else {
			string sLevelName;
			oPacket.unpack("et", nDataSize, &sLevelName);

			printf("Loading level '%s'.\n", sLevelName.c_str());
			string sLevelPath = "levels/" + sLevelName + ".wwl";
			GameDataOpenLevel(sLevelPath.c_str());
		}
		break;
	// Current Players Info
	case 21:
		// Check if we have fully joined the server
		if (pServer->GetJoinStatus() < PUBLIC_CLIENT) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		} else if (pServer->GetJoinStatus() >= IN_GAME) {
			printf("Error: Already entered the game, but received a Current Players Info packet.\n");
			return false;
		}

		if (nDataSize < nPlayerCount) return false;		// Check packet size
		else {
			u_char cNameLength;
			string sName;
			u_char cTeam;
			u_char cLastCommandSequenceNumber;
			float fX, fY, fZ;

			int nActivePlayers = 0;
			for (u_int nPlayer = 0; nPlayer < nPlayerCount; ++nPlayer)
			{
				oPacket.unpack("c", &cNameLength);
				if (cNameLength > 0) {
					// Active in-game player
					oPacket.unpack("etc", (int)cNameLength, &sName, &cTeam);

					if (nPlayer != iLocalPlayerID) {
						new RCtrlRAuthPlayer(nPlayer);
					}

					PlayerGet(nPlayer)->SetName(sName);
					PlayerGet(nPlayer)->SetTeam((int)cTeam);
					if (cTeam != 2)
					{
						oPacket.unpack("cfff", &cLastCommandSequenceNumber, &fX, &fY, &fZ);
						//cCurrentCommandSequenceNumber = cLastCommandSequenceNumber;
						PlayerGet(nPlayer)->cLastAckedCommandSequenceNumber = cLastCommandSequenceNumber;
						if (nPlayer == iLocalPlayerID) {
							eX0_assert(false, "local player can't be on a non-spectator team already!");
						} else {
							PlayerGet(nPlayer)->Position(fX, fY, fZ, cLastCommandSequenceNumber);
						}
					}
					// Set the player tick time
					PlayerGet(nPlayer)->fTickTime = 1.0f / cCommandRate;
					if (nPlayer == iLocalPlayerID)
					{
						// Reset the sequence numbers for the local player
						// TODO: cCurrentCommandSequenceNumber will no longer start off at 0, so find a way to set it to the correct value
						cCurrentCommandSequenceNumber = 0;
						//cLastAckedCommandSequenceNumber = 0;
						cLastUpdateSequenceNumber = 0;
						oUnconfirmedMoves.clear();
					} else {
						PlayerGet(nPlayer)->fTicks = 0.0f;
					}

					++nActivePlayers;
				}
			}

			printf("%d player%s already in game.\n", nActivePlayers - 1, (nActivePlayers - 1 == 1 ? "" : "s"));
		}
		break;
	// Player Joined Server
	case 25:
		// Check if we have fully joined the server
		if (pServer->GetJoinStatus() < PUBLIC_CLIENT) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (nDataSize < 3) return false;		// Check packet size
		else {
glfwLockMutex(oPlayerTick);

			u_char cPlayerId;
			u_char cNameLength;
			string sName;
			oPacket.unpack("cc", &cPlayerId, &cNameLength);
			oPacket.unpack("et", (int)cNameLength, &sName);
			if (cPlayerId == iLocalPlayerID)
				printf("Got a Player Joined Server packet, with the local player ID %d.", iLocalPlayerID);

			if (PlayerGet(cPlayerId) != NULL) {
				printf("Got a Player Joined Server packet, but player %d was already in game.\n", cPlayerId);
				return false;
			}

			new RCtrlRAuthPlayer(cPlayerId);

			PlayerGet(cPlayerId)->SetName(sName);
			PlayerGet(cPlayerId)->SetTeam(2);

			// Set the other player tick time
			PlayerGet(cPlayerId)->fTickTime = 1.0f / cCommandRate;

			printf("Player #%d (name '%s') is connecting (in Info Exchange)...\n", cPlayerId, sName.c_str());
			// This is a kinda a lie, he's still connecting, in Info Exchange part; should display this when server gets Entered Game Notification (7) packet
			pChatMessages->AddMessage(PlayerGet(cPlayerId)->GetName() + " is entering game.");

			// TODO: Send an ACK packet

glfwUnlockMutex(oPlayerTick);
		}
		break;
	// Player Left Server
	case 26:
		// Check if we have fully joined the server
		if (pServer->GetJoinStatus() < PUBLIC_CLIENT) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (nDataSize != 1) return false;		// Check packet size
		else {
glfwLockMutex(oPlayerTick);

			u_char cPlayerID;
			oPacket.unpack("c", &cPlayerID);

			if (cPlayerID == iLocalPlayerID)
				printf("Got a Player Left Server packet, with the local player ID %d.", iLocalPlayerID);
			if (PlayerGet(cPlayerID) == NULL) {
				printf("Got a Player Left Server packet, but player %d was not in game.\n", cPlayerID);
				return false;
			}

			printf("Player #%d (name '%s') has left the game.\n", cPlayerID, PlayerGet(cPlayerID)->GetName().c_str());
			pChatMessages->AddMessage(PlayerGet((int)cPlayerID)->GetName() + " left the game.");

			delete PlayerGet((int)cPlayerID);

			// TODO: Send an ACK packet

glfwUnlockMutex(oPlayerTick);
		}
		break;
	// Player Joined Team
	case 28:
		// Check if we have fully joined the server
		if (pServer->GetJoinStatus() < PUBLIC_CLIENT) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (nDataSize < 2) return false;		// Check packet size
		else {
glfwLockMutex(oPlayerTick);

			u_char cPlayerID;
			u_char cTeam;
			u_char cLastCommandSequenceNumber;
			float fX, fY, fZ;
			oPacket.unpack("cc", &cPlayerID, &cTeam);

			eX0_assert(pServer->GetJoinStatus() >= IN_GAME || cPlayerID != iLocalPlayerID, "We should be IN_GAME if we receive a Player Joined Team packet about us.");

			if (PlayerGet(cPlayerID) == NULL) { printf("Got a Player Joined Team packet, but player %d was not connected.\n", cPlayerID); return false; }
			PlayerGet(cPlayerID)->SetTeam(cTeam);
			if (PlayerGet(cPlayerID)->GetTeam() != 2)
			{
				// This resets the variables and increments the Command Series Number
				PlayerGet(cPlayerID)->RespawnReset();

				oPacket.unpack("cfff", &cLastCommandSequenceNumber, &fX, &fY, &fZ);
				PlayerGet(cPlayerID)->cLastAckedCommandSequenceNumber = cLastCommandSequenceNumber;
//printf("cLastAckedCommandSequenceNumber (in packet 28) = %d, while cCurrentCommandSequenceNumber = %d\n", cLastCommandSequenceNumber, cCurrentCommandSequenceNumber);
				PlayerGet(cPlayerID)->Position(fX, fY, fZ, PlayerGet(cPlayerID)->cLastAckedCommandSequenceNumber);
			}

			printf("Player #%d (name '%s') joined team %d.\n", cPlayerID, PlayerGet(cPlayerID)->GetName().c_str(), cTeam);
			pChatMessages->AddMessage(((int)cPlayerID == iLocalPlayerID ? "Joined " : PlayerGet(cPlayerID)->GetName() + " joined ")
				+ (cTeam == 0 ? "team Red" : (cTeam == 1 ? "team Blue" : "Spectators")) + ".");

			if (cPlayerID == iLocalPlayerID)
			{
				oUnconfirmedMoves.clear();
				bSelectTeamReady = true;
			}

glfwUnlockMutex(oPlayerTick);
		}
		break;
	default:
		printf("Error: Got unknown TCP packet of type %d and data size %d.\n", cPacketType, nDataSize);
		return false;
		break;
	}

if (glfwGetKey(GLFW_KEY_TAB) == GLFW_RELEASE)
fTempFloat = static_cast<float>(glfwGetTime()) - fTempFloat;

	return true;
}

void NetworkJoinGame()
{
	// DEBUG: Perform the cCurrentCommandSequenceNumber synchronization
	double d = glfwGetTime() / (256.0 / cCommandRate);
	d -= floor(d);
	d *= 256.0;
	cCurrentCommandSequenceNumber = (u_char)d;
	pLocalPlayer->dNextTickTime = ceil(glfwGetTime() / (1.0 / cCommandRate)) * (1.0 / cCommandRate);
	printf("abc: %f, %d\n", d, cCurrentCommandSequenceNumber);
	d -= floor(d);
	printf("tick %% = %f, nextTickAt = %.10lf\n", d*100, pLocalPlayer->dNextTickTime);
	printf("%.8lf sec: NxtTk=%.15lf, NxtTk/12.8=%.15lf\n", glfwGetTime(), pLocalPlayer->dNextTickTime, pLocalPlayer->dNextTickTime / (256.0 / cCommandRate));
	pLocalPlayer->fTicks = (float)(d * pLocalPlayer->fTickTime);

	pServer->SetJoinStatus(IN_GAME);

	// Send an Entered Game Notification packet
	CPacket oEnteredGameNotificationPacket;
	oEnteredGameNotificationPacket.pack("hc", 0, (u_char)7);
	oEnteredGameNotificationPacket.CompleteTpcPacketSize();
	pServer->SendTcp(oEnteredGameNotificationPacket);

	// Start the game
	printf("Entered the game.\n");
	iGameState = 0;

	bSelectTeamReady = true;
}

void PrintHi(void *)
{
	//printf("%30.20f\n", glfwGetTime());
	printf("===================== %f\n", glfwGetTime());
}

// Process a received UDP packet
bool NetworkProcessUdpPacket(CPacket & oPacket)
{
	u_int nDataSize = oPacket.size() - 1;
	u_char cPacketType; oPacket.unpack("c", &cPacketType);

	switch (cPacketType) {
	// Time Response Packet
	case 106:
		// Check if we have established a UDP connection to the server
		if (pServer->GetJoinStatus() < UDP_CONNECTED) {
			printf("Error: Not yet UDP connected to the server.\n");
			return false;
		}

		if (nDataSize != 9) return false;		// Check packet size
		else {
			u_char cSequenceNumber;
			double dTime;
			oPacket.unpack("cd", &cSequenceNumber, &dTime);

			++nTrpReceived;

			if (nTrpReceived <= 30) {
				double dLatency = glfwGetTime() - oSentTimeRequestPacketTimes.at(cSequenceNumber);
				if (dLatency <= dShortestLatency) {
					dShortestLatency = dLatency;
					dShortestLatencyLocalTime = glfwGetTime();
					dShortestLatencyRemoteTime = dTime;
				}
				//printf("Got a TRP #%d at %.5lf ms latency: %.5lf ms (shortest = %.5lf ms)\n", nTrpReceived, glfwGetTime() * 1000, dLatency * 1000, dShortestLatency * 1000);
			} else printf("Got an unnecessary TRP #%d packet, ignoring.\n", nTrpReceived);

			if (nTrpReceived == 30) {
				pTimedEventScheduler->RemoveEventById(nSendTimeRequestPacketEventId);

				// Adjust local clock
				glfwSetTime((glfwGetTime() - dShortestLatencyLocalTime) + dShortestLatencyRemoteTime
					+ 0.5 * dShortestLatency + 0.0000135);
				dTimePassed = 0;
				dCurTime = glfwGetTime();
				dBaseTime = dCurTime;

				CTimedEvent oEvent(ceil(glfwGetTime()), 1.0, PrintHi, NULL);
				//pTimedEventScheduler->ScheduleEvent(oEvent);

				glfwLockMutex(oJoinGameMutex);
				if (bGotPermissionToEnter)
				{
					// Join Game
					NetworkJoinGame();
				}
				else
					bFinishedSyncingClock = true;
				glfwUnlockMutex(oJoinGameMutex);
			}
		}
		break;
	// Server Update Packet
	case 2:
		// Check if we have entered the game
		if (pServer->GetJoinStatus() < IN_GAME) {
			printf("Error: Not yet entered the game.\n");
			return false;
		}
		/*// Check if we have established a UDP connection to the server
		if (pServer->GetJoinStatus() < UDP_CONNECTED) {
			printf("Error: Not yet UDP connected to the server.\n");
			return false;
		}*/

		if (nDataSize < 1 + (u_int)nPlayerCount) return false;		// Check packet size
		else {
glfwLockMutex(oPlayerTick);

			u_char cUpdateSequenceNumber;
			u_char cLastCommandSequenceNumber;
			u_char cPlayerInfo;
			float fX, fY, fZ;
			oPacket.unpack("c", &cUpdateSequenceNumber);

			if (cUpdateSequenceNumber == cLastUpdateSequenceNumber) {
				printf("Got a duplicate UDP update packet from the server, discarding.\n");
			} else if ((char)(cUpdateSequenceNumber - cLastUpdateSequenceNumber) < 0) {
				printf("Got an out of order UDP update packet from the server, discarding.\n");
			} else
			{
				++cLastUpdateSequenceNumber;
				if (cUpdateSequenceNumber != cLastUpdateSequenceNumber) {
					printf("Lost %d UDP update packet(s) from the server!\n", (char)(cUpdateSequenceNumber - cLastUpdateSequenceNumber));
				}
				cLastUpdateSequenceNumber = cUpdateSequenceNumber;

				for (u_int nPlayer = 0; nPlayer < nPlayerCount; ++nPlayer)
				{
					oPacket.unpack("c", &cPlayerInfo);
					if (cPlayerInfo == 1 && PlayerGet(nPlayer)->GetTeam() != 2) {
						// Active player
						oPacket.unpack("c", &cLastCommandSequenceNumber);
						oPacket.unpack("fff", &fX, &fY, &fZ);

						bool bNewerCommand = (cLastCommandSequenceNumber != PlayerGet(nPlayer)->cLastAckedCommandSequenceNumber);
						if (bNewerCommand) {
							SequencedState_t oSequencedState;
							oSequencedState.cSequenceNumber = cLastCommandSequenceNumber;
							oSequencedState.oState.fX = fX;
							oSequencedState.oState.fY = fY;
							oSequencedState.oState.fZ = fZ;

							eX0_assert(PlayerGet(nPlayer)->m_oAuthUpdatesTEST.push(oSequencedState), "m_oAuthUpdatesTEST.push(oSequencedState) failed, missed latest update!\n");
							//printf("pushed %d\n", cLastCommandSequenceNumber);
						}
					}
				}
			}

glfwUnlockMutex(oPlayerTick);
		}
		break;
	// Ping Packet
	case 10:
		if (pServer->GetJoinStatus() < IN_GAME) {
			printf("Error: Not yet entered the game.\n");
			return false;
		}

		if (nDataSize != 4 + 2 * nPlayerCount) return false;		// Check packet size
		else {
			PingData_t oPingData;
			oPacket.unpack("cccc", &oPingData.cPingData[0], &oPingData.cPingData[1], &oPingData.cPingData[2], &oPingData.cPingData[3]);

			// Make note of the current time (t1), for own latency calculation (RTT = t2 - t1)
			oPongSentTimes.push(oPingData, glfwGetTime());

			// Respond immediately with a Pong packet
			CPacket oPongPacket;
			oPongPacket.pack("ccccc", (u_char)11, oPingData.cPingData[0], oPingData.cPingData[1], oPingData.cPingData[2], oPingData.cPingData[3]);
			pServer->SendUdp(oPongPacket);

			// Update the last latency for all players
			for (u_int nPlayer = 0; nPlayer < nPlayerCount; ++nPlayer)
			{
				u_short nLastLatency;

				oPacket.unpack("h", &nLastLatency);

				if (PlayerGet(nPlayer) != NULL && nPlayer != iLocalPlayerID) {
					PlayerGet(nPlayer)->SetLastLatency(nLastLatency);
				}
			}
		}
		break;
	// Pung Packet
	case 12:
		if (pServer->GetJoinStatus() < IN_GAME) {
			printf("Error: Not yet entered the game.\n");
			return false;
		}

		if (nDataSize != 12) return false;		// Check packet size
		else {
			double dLocalTimeAtPungReceive = glfwGetTime();		// Make note of current time (t2)

			PingData_t oPingData;
			double	dServerTime;
			oPacket.unpack("ccccd", &oPingData.cPingData[0], &oPingData.cPingData[1], &oPingData.cPingData[2], &oPingData.cPingData[3], &dServerTime);

			// Get the time sent of the matching Pong packet
			try {
				double dLocalTimeAtPongSend = oPongSentTimes.MatchAndRemoveAfter(oPingData);

				// Calculate own latency and update it on the scoreboard
				double dLatency = dLocalTimeAtPungReceive - dLocalTimeAtPongSend;
				pLocalPlayer->SetLastLatency(static_cast<u_short>(ceil(dLatency * 10000)));
				oRecentLatency.push(dLatency, dLocalTimeAtPongSend);
				//printf("\nOwn latency is %.5lf ms. LwrQrtl = %.5lf ms\n", dLatency * 1000, oRecentLatency.LowerQuartile() * 1000);

				if (dLatency <= oRecentLatency.LowerQuartile() && oRecentLatency.well_populated())
					// || (dTimeDifference > dLatency && dLatency <= oRecentLatency.Mean() && oRecentLatency.well_populated())
				{
					// Calculate the (local minus remote) time difference: diff = (t1 + t2) / 2 - server_time
					double dTimeDifference = (dLocalTimeAtPongSend + dLocalTimeAtPungReceive) * 0.5 - dServerTime;
					oRecentTimeDifference.push(dTimeDifference, dLocalTimeAtPongSend);
					//printf("Time diff %.5lf ms (l-r) Cnted! Avg=%.5lf ms\n", dTimeDifference * 1000, oRecentTimeDifference.Mean() * 1000);

					// Perform the local time adjustment
					// TODO: Make this more robust, rethink how often to really perform this adjustment, and maybe make it smooth(er)...
					if (oRecentTimeDifference.well_populated() && oRecentTimeDifference.Signum() != 0) {
						double dAverageTimeDifference = -oRecentTimeDifference.WeightedMovingAverage();
						glfwSetTime(dAverageTimeDifference + glfwGetTime());
						oRecentTimeDifference.clear();
						printf("Performed time adjustment by %.5lf ms.\n", dAverageTimeDifference * 1000);
					}
				} //else printf("Time diff %.5lf ms (l-r)\n", ((dLocalTimeAtPongSend + dLocalTimeAtPungReceive) * 0.5 - dServerTime) * 1000);
			} catch (...) {
				printf("Error: We never got a ping packet with this oPingData, but got a pung packet! Doesn't make sense.");
				return false;
			}
		}
		break;
	default:
		printf("Error: Got unknown UDP packet of type %d and data size %d.\n", cPacketType, nDataSize);
		return false;
		break;
	}

	return true;
}

void NetworkSendUdpHandshakePacket(void *)
{
	printf("Sending a UDP Handshake packet...\n");

	// Send UDP packet with the same signature to initiate the UDP handshake
	CPacket oUdpHandshakePacket;
	oUdpHandshakePacket.pack("c", (u_char)100);
	for (int nSignatureByte = 0; nSignatureByte < NetworkConnection::m_knSignatureSize; ++nSignatureByte)
		oUdpHandshakePacket.pack("c", pServer->GetSignature()[nSignatureByte]);
	pServer->SendUdp(oUdpHandshakePacket, ACCEPTED);
}

void NetworkShutdownThread()
{
	if (oNetworkThread >= 0)
	{
		bNetworkThreadRun = false;

		// DEBUG: A hack to send ourselves an empty UDP packet in order to get out of select()
		sendto(pServer->GetUdpSocket(), NULL, 0, 0, (sockaddr *)&oLocalUdpAddress, nLocalUdpAddressLength);

		shutdown(pServer->GetTcpSocket(), SD_BOTH);
		shutdown(pServer->GetUdpSocket(), SD_BOTH);
	}
}

void NetworkDestroyThread()
{
	if (oNetworkThread >= 0)
	{
		glfwWaitThread(oNetworkThread, GLFW_WAIT);
		//glfwDestroyThread(oNetworkThread);
		oNetworkThread = -1;

		printf("Network thread has been destroyed.\n");
	}
}

// Shutdown the networking component
void NetworkDeinit()
{
	NetworkShutdownThread();
	NetworkDestroyThread();

	if (pServer != NULL) {
		delete pServer;
		pServer = NULL;
	}
	//NetworkCloseSocket(pServer->GetTcpSocket());
	//NetworkCloseSocket(pServer->GetUdpSocket());

#ifdef WIN32
	WSACleanup();
#else // Linux
	// Nothing to be done on Linux
#endif

	glfwDestroyMutex(oTcpSendMutex);
	glfwDestroyMutex(oUdpSendMutex);
	glfwDestroyMutex(oPlayerTick);
	glfwDestroyMutex(oJoinGameMutex);
}

// Closes a socket
void NetworkCloseSocket(SOCKET nSocket)
{
#ifdef WIN32
	closesocket(nSocket);
#else // Linux
	close(nSocket);
#endif

	nSocket = INVALID_SOCKET;
}

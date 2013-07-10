#include "globals.h"

SOCKET			nServerTcpSocket = INVALID_SOCKET;
SOCKET			nServerUdpSocket = INVALID_SOCKET;
struct sockaddr_in	oServerAddress;
volatile int	nJoinStatus = DISCONNECTED;
char			cSignature[4];

GLFWthread		oNetworkThread = -1;
volatile bool	bNetworkThreadRun;
GLFWmutex		oTcpSendMutex;

u_char			cLocalMovementSequenceNumber = 0;
u_char			cRemoteUpdateSequenceNumber = 0;
//deque<Input_t>	oLocallyPredictedInputs;
//deque<Input_t>	oUnconfirmedInputs;
GLFWmutex		oPlayerTick;

/*Move_t			oPredictedUnconfirmedMoves[256];
u_char			cPredictedUnconfirmedMovesHead = 0;
u_char			cPredictedUnconfirmedMovesTail = 0;*/
IndexedCircularBuffer<Move_t>	oUnconfirmedMoves(256);

float			fLastLatency = 0;
float			fPingPacketTime;
int				nPingPacketNumber = -1;

// Initialize the networking component
bool NetworkInit()
{
	oTcpSendMutex = glfwCreateMutex();
	oPlayerTick = glfwCreateMutex();

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

int sendall(SOCKET s, char *buf, int len, int flags)
{
	glfwLockMutex(oTcpSendMutex);

	int total = 0;        // how many bytes we've sent
	int bytesleft = len; // how many we have left to send
	int n = 0;

	while(total < len) {
		n = send(s, buf+total, bytesleft, flags);
		if (n == SOCKET_ERROR) { break; }
		total += n;
		bytesleft -= n;
	}

	//*len = total; // return number actually sent here

	glfwUnlockMutex(oTcpSendMutex);

	return (n == SOCKET_ERROR || total != len) ? SOCKET_ERROR : total; // return SOCKET_ERROR on failure, bytes sent on success
}

// Connect to a server
bool NetworkConnect(char *szHost, int nPort)
{
	struct hostent *he;

	if ((he = gethostbyname(szHost)) == NULL) {		// get the host info
		NetworkPrintError("gethostbyname");		// herror
		Terminate(1);
	}

	if ((nServerTcpSocket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) {
	    NetworkPrintError("socket");
	    Terminate(1);
	}
	if ((nServerUdpSocket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
	    NetworkPrintError("socket");
	    Terminate(1);
	}

	/* This is the default behaviour already (graceful shutdown, don't linger)
	struct linger oLinger;
	oLinger.l_onoff = 0;
	oLinger.l_linger = 0;
	if (setsockopt(nServerTcpSocket, SOL_SOCKET, SO_LINGER, (char *)&oLinger, sizeof(oLinger)) == SOCKET_ERROR) {
		NetworkPrintError("setsockopt");
		Terminate(1);
	}*/
	// Disable the Nagle algorithm for send coalescing
	int nNoDelay = 1;
	if (setsockopt(nServerTcpSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&nNoDelay, sizeof(nNoDelay)) == SOCKET_ERROR) {
		NetworkPrintError("setsockopt");
		Terminate(1);
	}

	oServerAddress.sin_family = AF_INET;
	oServerAddress.sin_addr = *((struct in_addr *)he->h_addr);
	oServerAddress.sin_port = htons(nPort);
	memset(oServerAddress.sin_zero, 0, sizeof(oServerAddress.sin_zero));

	printf("Connecting to %s.\n", szHost);
	if (connect(nServerTcpSocket, (struct sockaddr *)&oServerAddress, sizeof(oServerAddress)) == SOCKET_ERROR) {
	    NetworkPrintError("connect");
	    Terminate(1);
	}
	struct sockaddr_in	oLocalTcpAddress;
	socklen_t			nLocalTcpAddressLength = sizeof(oLocalTcpAddress);
	if (getsockname(nServerTcpSocket, (struct sockaddr *)&oLocalTcpAddress, &nLocalTcpAddressLength) == SOCKET_ERROR) {
		NetworkPrintError("getsockname");
		Terminate(1);
	}
	// Successfully connected (TCP) to the server
	nJoinStatus = TCP_CONNECTED;
	printf("Established a TCP connection (local port %d), attempting to join the game.\n", ntohs(oLocalTcpAddress.sin_port));

	// Create the networking thread
	NetworkCreateThread();

	// Create and send a Join Game Request packet
	CPacket oJoinGameRequestPacket;
	oJoinGameRequestPacket.pack("hhhs", 0, 1, 1, "somerandompass01");
	float fSignature = (float)glfwGetTime();
	memcpy(cSignature, (void *)&fSignature, 4);
	for (int nSignatureByte = 0; nSignatureByte < 4; ++nSignatureByte)
		oJoinGameRequestPacket.pack("c", cSignature[nSignatureByte]);
	oJoinGameRequestPacket.CompleteTpcPacketSize();
	oJoinGameRequestPacket.SendTcp(TCP_CONNECTED);

	return true;
}

bool NetworkCreateThread()
{
	bNetworkThreadRun = true;
	oNetworkThread = glfwCreateThread(NetworkThread, NULL);

	printf("Network thread created.\n");

	return oNetworkThread >= 0;
}

void GLFWCALL NetworkThread(void *pArg)
{
	int fdmax;
	fd_set master;   // master file descriptor list
	fd_set read_fds;

	int			nbytes;
	char		buf[2 * MAX_TCP_PACKET_SIZE];
	u_short		snCurrentPacketSize = 0;
	char		cUdpBuffer[MAX_UDP_PACKET_SIZE];

	// clear the master and temp sets
	FD_ZERO(&master);
	FD_ZERO(&read_fds);

	// Add the sockets to select on
	FD_SET(nServerTcpSocket, &master);
	FD_SET(nServerUdpSocket, &master);

	fdmax = __max((int)nServerTcpSocket, (int)nServerUdpSocket);

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
		else if (FD_ISSET(nServerTcpSocket, &read_fds))
		{
			// got error or connection closed by server
			if ((nbytes = recv(nServerTcpSocket, buf + snCurrentPacketSize, sizeof(buf) - snCurrentPacketSize, 0)) <= 0) {
				if (nbytes == 0) {
					// Connection closed gracefully by the server
					printf("Connection closed gracefully by the server.\n");
				} else {
					NetworkPrintError("recv");
					printf("Lost connection to the server.\n");
				}
				bNetworkThreadRun = false;
			// we got some data from a client, process it
			} else {
				//printf("Got %d bytes from server\n", nbytes);

				snCurrentPacketSize += nbytes;
				// Check if received enough to check the packet size
				u_short snRealPacketSize = MAX_TCP_PACKET_SIZE;
				if (snCurrentPacketSize >= 2)
					snRealPacketSize = ntohs(*((short *)buf));
				if (snRealPacketSize > MAX_TCP_PACKET_SIZE) {		// Make sure the packet is not larger than allowed
					printf("Got a TCP packet that's larger than allowed.\n");
					snRealPacketSize = MAX_TCP_PACKET_SIZE;
				}
				// Received an entire packet
				while (snCurrentPacketSize >= snRealPacketSize)
				{
					// Process it
					CPacket oPacket(buf);
					if (!NetworkProcessTcpPacket(oPacket)) {
						printf("Couldn't process a TCP packet (type %d):\n", ntohs(*((u_short *)(buf + 2))));
						oPacket.Print(snRealPacketSize);
					}
					//memmove(buf, buf + snRealPacketSize, sizeof(buf) - snRealPacketSize);
					//snCurrentPacketSize -= snRealPacketSize;
					snCurrentPacketSize -= snRealPacketSize;
					memmove(buf, buf + snRealPacketSize,
						__min(snCurrentPacketSize, sizeof(buf) - snRealPacketSize));

					if (snCurrentPacketSize >= 2)
						snRealPacketSize = ntohs(*((short *)buf));
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
		else if (FD_ISSET(nServerUdpSocket, &read_fds))
		{
			// handle UDP data from the server
			if ((nbytes = recv(nServerUdpSocket, cUdpBuffer, sizeof(cUdpBuffer), 0)) == SOCKET_ERROR)
			{
				// Error
				NetworkPrintError("recv");
			} else {
				// Got a UDP packet
				//printf("Got a UDP %d byte packet from server!\n", nbytes);

				// Process the received UDP packet
				CPacket oPacket(cUdpBuffer);
				if (!NetworkProcessUdpPacket(oPacket, nbytes)) {
					printf("Couldn't process a UDP packet (type %d):\n", *((u_char *)cUdpBuffer));
					oPacket.Print(nbytes);
				}
			}
		}

		// Sleep
		glfwSleep(0.0);
	}

	printf("Network thread has ended.\n");
}

// Process a received TCP packet
bool NetworkProcessTcpPacket(CPacket & oPacket/*, CClient * oClient*/)
{
	u_short snPacketSize; oPacket.unpack("h", &snPacketSize);
	if (snPacketSize < 4) return false;
	u_short snPacketType; oPacket.unpack("h", &snPacketType);

if (glfwGetKey(GLFW_KEY_TAB) == GLFW_RELEASE)
fTempFloat = static_cast<float>(glfwGetTime());

	switch (snPacketType) {
	// Join Game Accept
	case 2:
		// Check if we have already been accepted by the server
		if (nJoinStatus >= ACCEPTED) {
			printf("Error: Already accepted, but received another Join Game Accept packet.\n");
			return false;
		}

		if (snPacketSize != 6) return false;		// Check packet size
		else {
			char cLocalPlayerID;
			char cPlayerCount;
			oPacket.unpack("cc", &cLocalPlayerID, (char *)&cPlayerCount);

			iLocalPlayerID = (int)cLocalPlayerID;
			nPlayerCount = (int)cPlayerCount;
			PlayerInit();
			// DEBUG: Player name (and others) needs to be assigned in a better way
			PlayerGet(iLocalPlayerID)->bConnected = true;
			PlayerGet(iLocalPlayerID)->SetName(sLocalPlayerName);
			// Got successfully accepted in game by the server
			nJoinStatus = ACCEPTED;
			printf("Got accepted in game: local player id = %d, player count = %d\n", iLocalPlayerID, nPlayerCount);

			// Create the UDP connection
			if (connect(nServerUdpSocket, (struct sockaddr *)&oServerAddress, sizeof(oServerAddress)) == SOCKET_ERROR) {
				NetworkPrintError("connect");
				Terminate(1);
			}
			struct sockaddr_in	oLocalUdpAddress;		// Get the local UDP port
			socklen_t			nLocalUdpAddressLength = sizeof(oLocalUdpAddress);
			if (getsockname(nServerUdpSocket, (struct sockaddr *)&oLocalUdpAddress, &nLocalUdpAddressLength) == SOCKET_ERROR) {
				NetworkPrintError("getsockname");
				Terminate(1);
			}
			printf("Created a pending UDP connection to server on port %d.\n", ntohs(oLocalUdpAddress.sin_port));

			// Send UDP packet with the same signature to initiate the UDP handshake
			CPacket oUdpHandshakePacket;
			oUdpHandshakePacket.pack("c", (u_char)100);
			for (int nSignatureByte = 0; nSignatureByte < 4; ++nSignatureByte)
				oUdpHandshakePacket.pack("c", cSignature[nSignatureByte]);
			oUdpHandshakePacket.SendUdp(ACCEPTED);
			// TODO: Need to somehow make sure this UDP packet gets to the server, and retransmit a few times after some timeout
			// Add timed event (Retransmit UdpHandshake packet after 1.0 seconds)
		}
		break;
	// Join Game Refuse
	case 3:
		// Check if we have already been accepted by the server
		if (nJoinStatus >= ACCEPTED) {
			printf("Error: Already accepted, but received a Join Game Refuse packet.\n");
			return false;
		}

		if (snPacketSize != 5) return false;		// Check packet size
		else {
			char cRefuseReason;
			oPacket.unpack("c", &cRefuseReason);

			printf("Got refused with reason %d.\n", (int)cRefuseReason);

			bNetworkThreadRun = false;
			shutdown(nServerTcpSocket, SD_BOTH);

			// Server connection status is fully unconnected
			nJoinStatus = DISCONNECTED;
		}
		break;
	// UDP Connection Established
	case 5:
		// Check if we have been accepted by the server
		if (nJoinStatus < ACCEPTED) {
			printf("Error: Not yet accepted by the server.\n");
			return false;
		}

		if (snPacketSize != 4) return false;		// Check packet size
		else {
			// The UDP connection with the server is fully established
			nJoinStatus = UDP_CONNECTED;
			printf("Established a UDP connection with the server.\n");

			// Send a Local Player Info packet
			CPacket oLocalPlayerInfoPacket;
			oLocalPlayerInfoPacket.pack("hh", 0, (u_short)30);
			oLocalPlayerInfoPacket.pack("c", (u_char)PlayerGet(iLocalPlayerID)->GetName().length());
			oLocalPlayerInfoPacket.pack("t", &PlayerGet(iLocalPlayerID)->GetName());
			oLocalPlayerInfoPacket.CompleteTpcPacketSize();
			oLocalPlayerInfoPacket.SendTcp(UDP_CONNECTED);
		}
		break;
	// Permission To Join
	case 6:
		// Check if we have fully joined the server
		if (nJoinStatus < UDP_CONNECTED) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (snPacketSize != 4) return false;		// Check packet size
		else {
			nJoinStatus = IN_GAME;

			// Send a Client Entered Game packet to tell the server we have entered the game
			CPacket oClientEnteredGamePacket;
			oClientEnteredGamePacket.pack("hh", 0, (u_short)7);
			oClientEnteredGamePacket.CompleteTpcPacketSize();
			oClientEnteredGamePacket.SendTcp(IN_GAME);

			// Start the game
			printf("Entered the game.\n");
			iGameState = 0;
		}
		break;
	// Broadcast Text Message
	case 11:
		// Check if we have entered the game
		if (nJoinStatus < IN_GAME) {
			printf("Error: Not yet entered the game.\n");
			return false;
		}

		if (snPacketSize < 6) return false;		// Check packet size
		else {
			char cPlayerID;
			string sTextMessage;
			oPacket.unpack("cet", (char *)&cPlayerID, snPacketSize - 5, &sTextMessage);

			// Print out the text message
			printf("Player %d sends: '%s'\n", cPlayerID, sTextMessage.c_str());
		}

		break;
	// Load Level
	case 20:
		// Check if we have fully joined the server
		if (nJoinStatus < UDP_CONNECTED) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (snPacketSize < 5) return false;		// Check packet size
		else {
			string sLevelName;
			oPacket.unpack("et", snPacketSize - 4, &sLevelName);

			printf("Loading level '%s'.\n", sLevelName.c_str());
			string sLevelPath = "levels/" + sLevelName + ".wwl";
			GameDataOpenLevel(sLevelPath.c_str());
		}
		break;
	// Current Players Info
	case 21:
		// Check if we have fully joined the server
		if (nJoinStatus < UDP_CONNECTED) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		} else if (nJoinStatus >= IN_GAME) {
			printf("Error: Already entered the game, but received a Current Players Info packet.\n");
			return false;
		}

		if (snPacketSize < 4 + nPlayerCount) return false;		// Check packet size
		else {
			u_char cNameLength;
			string sName;
			float fX, fY, fZ;
			int nActivePlayers = 0;
			for (int nPlayer = 0; nPlayer < nPlayerCount; ++nPlayer)
			{
				oPacket.unpack("c", &cNameLength);
				if (cNameLength > 0) {
					// Active in-game player
					oPacket.unpack("etfff", (int)cNameLength, &sName, &fX, &fY, &fZ);
					PlayerGet(nPlayer)->bConnected = true;
					PlayerGet(nPlayer)->SetName(sName);
					PlayerGet(nPlayer)->Position(fX, fY);
					PlayerGet(nPlayer)->SetZ(fZ);
					if (nPlayer == iLocalPlayerID) {
						// Reset the sequence numbers for the local player
						cLocalMovementSequenceNumber = 0;
						cRemoteUpdateSequenceNumber = 0;
						//oLocallyPredictedInputs.clear();
						//oUnconfirmedInputs.clear();
						oUnconfirmedMoves.clear();
					}

					++nActivePlayers;
				} else {
					// Inactive player slot
					PlayerGet(nPlayer)->bConnected = false;
				}
			}

			printf("%d player%s already in game.\n", nActivePlayers - 1, (nActivePlayers - 1 == 1 ? "" : "s"));
		}
		break;
	// Player Entered Game
	case 25:
		// Check if we have fully joined the server
		if (nJoinStatus < UDP_CONNECTED) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (snPacketSize < 19) return false;		// Check packet size
		else {
			u_char cPlayerID;
			u_char cNameLength;
			string sName;
			float fX, fY, fZ;
			oPacket.unpack("cc", &cPlayerID, &cNameLength);
			oPacket.unpack("etfff", (int)cNameLength, &sName, &fX, &fY, &fZ);
			if (cPlayerID == iLocalPlayerID)
				printf("Got a Player Entered Game packet, with the local player ID %d.", iLocalPlayerID);
			if (PlayerGet(cPlayerID)->bConnected == true)
				printf("Got a Player Entered Game packet, but player %d was already in game.\n", cPlayerID);
			PlayerGet(cPlayerID)->bConnected = true;
			PlayerGet(cPlayerID)->SetName(sName);
			PlayerGet(cPlayerID)->Position(fX, fY);
			PlayerGet(cPlayerID)->SetZ(fZ);

			printf("Player #%d (name '%s') has entered the game.\n", cPlayerID, sName.c_str());

			// Send an ACK packet
			// TODO
		}
		break;
	// Player Left Game
	case 26:
		// Check if we have fully joined the server
		if (nJoinStatus < UDP_CONNECTED) {
			printf("Error: Not yet fully joined the server.\n");
			return false;
		}

		if (snPacketSize != 5) return false;		// Check packet size
		else {
			u_char cPlayerID;
			oPacket.unpack("c", &cPlayerID);
			if (cPlayerID == iLocalPlayerID)
				printf("Got a Player Left Game packet, with the local player ID %d.", iLocalPlayerID);
			if (PlayerGet(cPlayerID)->bConnected == false)
				printf("Got a Player Left Game packet, but player %d was not in game.\n", cPlayerID);
			PlayerGet(cPlayerID)->bConnected = false;

			printf("Player #%d (name '%s') has left the game.\n", cPlayerID, PlayerGet(cPlayerID)->GetName().c_str());

			// Send an ACK packet
			// TODO
		}
		break;
	default:
		printf("Error: Got unknown TCP packet of type %d and size %d.\n", snPacketType, snPacketSize);
		return false;
		break;
	}

if (glfwGetKey(GLFW_KEY_TAB) == GLFW_RELEASE)
fTempFloat = static_cast<float>(glfwGetTime()) - fTempFloat;

	return true;
}

// Process a received UDP packet
bool NetworkProcessUdpPacket(CPacket & oPacket, int nPacketSize/*, CClient * oClient*/)
{
	if (nPacketSize < 1) return false;
	u_char cPacketType; oPacket.unpack("c", &cPacketType);

	switch (cPacketType) {
	// Update Others Position test packet
	case 2:
		// Check if we have entered the game
		if (nJoinStatus < IN_GAME) {
			printf("Error: Not yet entered the game.\n");
			return false;
		}

		if (nPacketSize <= 14) return false;		// Check packet size
		else {
glfwLockMutex(oPlayerTick);

			u_char cSequenceNumber;
			u_char cPlayerInfo;
			float fX, fY, fZ;
			oPacket.unpack("c", &cSequenceNumber);

			if (cSequenceNumber == cRemoteUpdateSequenceNumber) {
				printf("Got a duplicate UDP update packet from the server, discarding.\n");
			} else if ((char)(cSequenceNumber - cRemoteUpdateSequenceNumber) < 0) {
				printf("Got an out of order UDP update packet from the server, discarding.\n");
			} else
			{
				++cRemoteUpdateSequenceNumber;
				if (cSequenceNumber != cRemoteUpdateSequenceNumber) {
					printf("Lost %d UDP update packet(s) from the server!\n", (char)(cSequenceNumber - cRemoteUpdateSequenceNumber));
				}
				cRemoteUpdateSequenceNumber = cSequenceNumber;

				// Ping time calculation
				if (nPingPacketNumber >= 0
				  && (char)(cRemoteUpdateSequenceNumber - nPingPacketNumber) >= 0) {
					fLastLatency = (float)glfwGetTime() - fPingPacketTime;
					nPingPacketNumber = -41;
				}

				for (int nPlayer = 0; nPlayer < nPlayerCount; ++nPlayer)
				{
					oPacket.unpack("c", &cPlayerInfo);
					if (cPlayerInfo == 1) {
						// Active player
						oPacket.unpack("fff", &fX, &fY, &fZ);
						if (nPlayer == iLocalPlayerID) {
							if (cLocalMovementSequenceNumber == cRemoteUpdateSequenceNumber)
							{
								Vector2 oServerPosition(fX, fY);
								Vector2 oClientPrediction(oUnconfirmedMoves.front().oState.fX, oUnconfirmedMoves.front().oState.fY);
								// If the client prediction differs from the server's value by more than a treshold amount, snap to server's value
								if ((oServerPosition - oClientPrediction).SquaredLength() > 0.001f)
									printf("Snapping to server's position (%f difference).\n", (oServerPosition - oClientPrediction).Length());

								// DEBUG: Should make sure the player can't see other players
								// through walls, if he accidents gets warped through a wall
								PlayerGet(iLocalPlayerID)->SetX(fX);
								PlayerGet(iLocalPlayerID)->SetY(fY);

								//oLocallyPredictedInputs.clear();
								//oUnconfirmedInputs.clear();
								oUnconfirmedMoves.clear();
							}
							else if ((char)(cLocalMovementSequenceNumber - cRemoteUpdateSequenceNumber) > 0)
							{
								string str = (string)"inputs empty; " + itos(cLocalMovementSequenceNumber) + ", " + itos(cRemoteUpdateSequenceNumber);
								//eX0_assert(!oLocallyPredictedInputs.empty(), str);
								eX0_assert(!oUnconfirmedMoves.empty(), str);

								// Discard all the locally predicted inputs that got deprecated by this server update
								// TODO: There's a faster way to get rid of all old useless packets at once
								while (!oUnconfirmedMoves.empty()) {
									if ((char)(cRemoteUpdateSequenceNumber - oUnconfirmedMoves.begin()) > 0)
									{
										// This is an outdated predicted input, the server's update supercedes it, thus it's dropped
										oUnconfirmedMoves.pop();
									} else
										break;
								}

								Vector2 oServerPosition(fX, fY);
								Vector2 oClientPrediction(oUnconfirmedMoves.front().oState.fX, oUnconfirmedMoves.front().oState.fY);
								// If the client prediction differs from the server's value by more than a treshold amount, snap to server's value
								if ((oServerPosition - oClientPrediction).SquaredLength() > 0.001f)
								{
									printf("Snapping to server's position (%f difference).\n", (oServerPosition - oClientPrediction).Length());

									oUnconfirmedMoves.pop();

									// DEBUG: Should make sure the player can't see other players
									// through walls, if he accidents gets warped through a wall
									PlayerGet(iLocalPlayerID)->SetX(fX);
									PlayerGet(iLocalPlayerID)->SetY(fY);

									eX0_assert((char)(oUnconfirmedMoves.begin() - cRemoteUpdateSequenceNumber) > 0, "outdated input being used");

									// Run the simulation for all locally predicted inputs after this server update
									float fOriginalOldX = PlayerGet(iLocalPlayerID)->GetOldX();
									float fOriginalOldY = PlayerGet(iLocalPlayerID)->GetOldY();
									float fOriginalZ = PlayerGet(iLocalPlayerID)->GetZ();
									Input_t oInput;
									for (u_char it1 = oUnconfirmedMoves.begin(); it1 != oUnconfirmedMoves.end(); ++it1)
									{
										oInput = oUnconfirmedMoves[it1].oInput;

										// Set inputs
										PlayerGet(iLocalPlayerID)->MoveDirection(oInput.cMoveDirection);
										PlayerGet(iLocalPlayerID)->SetZ(oInput.fZ);

										// Run a tick
										PlayerGet(iLocalPlayerID)->CalcTrajs();
										PlayerGet(iLocalPlayerID)->CalcColResp();
									}
									PlayerGet(iLocalPlayerID)->SetOldX(fOriginalOldX);
									PlayerGet(iLocalPlayerID)->SetOldY(fOriginalOldY);
									PlayerGet(iLocalPlayerID)->SetZ(fOriginalZ);
								}
							} else {
								//eX0_assert(false);
								printf("WTF\n");
							}
						} else {
							// TODO: Create smooth movement, instead of just hard placing the other player
							PlayerGet(nPlayer)->Position(fX, fY);
							PlayerGet(nPlayer)->SetZ(fZ);
						}
					}
				}

				// Drop the moves that have been confirmed now
				// TODO: There's a faster way to get rid of all old useless packets at once
				while (!oUnconfirmedMoves.empty())
				{
					if ((char)(cRemoteUpdateSequenceNumber - oUnconfirmedMoves.begin()) >= 0)
					{
						oUnconfirmedMoves.pop();
					} else
						break;
				}
			}
		}

glfwUnlockMutex(oPlayerTick);
		break;
	default:
		printf("Error: Got unknown UDP packet of type %d and size %d.\n", (int)cPacketType, nPacketSize);
		return false;
		break;
	}

	return true;
}

void NetworkDestroyThread()
{
	if (oNetworkThread < 0)
		return;

	bNetworkThreadRun = false;

	shutdown(nServerTcpSocket, SD_BOTH);

	glfwWaitThread(oNetworkThread, GLFW_WAIT);
	//glfwDestroyThread(oNetworkThread);

	printf("Network thread destroyed.\n");
}

// Shutdown the networking component
void NetworkDeinit()
{
	NetworkDestroyThread();

	NetworkCloseSocket(nServerTcpSocket);
	NetworkCloseSocket(nServerUdpSocket);

#ifdef WIN32
	WSACleanup();
#else // Linux
	// Nothing to be done on Linux
#endif

	glfwDestroyMutex(oTcpSendMutex);
	glfwDestroyMutex(oPlayerTick);
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

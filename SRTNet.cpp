//      __________  ____     _____ ____  ______   _       ______  ___    ____  ____  __________
//     / ____/ __ \/ __ \   / ___// __ \/_  __/  | |     / / __ \/   |  / __ \/ __ \/ ____/ __ \
//    / /   / /_/ / /_/ /   \__ \/ /_/ / / /     | | /| / / /_/ / /| | / /_/ / /_/ / __/ / /_/ /
//   / /___/ ____/ ____/   ___/ / _, _/ / /      | |/ |/ / _, _/ ___ |/ ____/ ____/ /___/ _, _/
//   \____/_/   /_/       /____/_/ |_| /_/       |__/|__/_/ |_/_/  |_/_/   /_/   /_____/_/ |_|
//
// Created by Anders Cedronius on 2019-04-21.
//

#include "SRTNet.h"

#include <optional>

#include "SRTNetInternal.h"

namespace {

/// Wrapper around sockaddr_in
class SocketAddress {
public:
    SocketAddress(const std::string& ip, uint16_t port)
        : mIP(ip)
        , mPort(port) {
    }

    ///
    /// @return True if this SocketAddress is an IPv4 address
    bool isIPv4() {
        sockaddr_in sa{};
        return inet_pton(AF_INET, mIP.c_str(), &sa.sin_addr) != 0;
    }

    ///
    /// @return True if this SocketAddress is an IPv6 address
    bool isIPv6() {
        sockaddr_in6 sa{};
        return inet_pton(AF_INET6, mIP.c_str(), &sa.sin6_addr) != 0;
    }

    ///
    /// @return Get this address as an IPv4 sockaddr_in, nullopt if not a valid IPv4 address
    [[nodiscard]] std::optional<sockaddr_in> getIPv4() const {
        sockaddr_in socketAddressV4;
        memset(&socketAddressV4, 0, sizeof(socketAddressV4));
        socketAddressV4.sin_family = AF_INET;
        socketAddressV4.sin_port = htons(mPort);
        if (inet_pton(AF_INET, mIP.c_str(), &socketAddressV4.sin_addr) != 1) {
            return std::nullopt;
        }
        return socketAddressV4;
    }

    ///
    /// @return Get this address as an IPv6 sockaddr_in6, nullopt if not a valid IPv6 address
    [[nodiscard]] std::optional<sockaddr_in6> getIPv6() const {
        sockaddr_in6 socketAddressV6;
        memset(&socketAddressV6, 0, sizeof(socketAddressV6));
        socketAddressV6.sin6_family = AF_INET6;
        socketAddressV6.sin6_port = htons(mPort);
        if (inet_pton(AF_INET6, mIP.c_str(), &socketAddressV6.sin6_addr) != 1) {
            return std::nullopt;
        }
        return socketAddressV6;
    }

private:
    std::string mIP;
    uint16_t mPort;
};

} // namespace

SRT_LOG_HANDLER_FN* SRTNet::gLogHandler = defaultLogHandler;
int SRTNet::gLogLevel = LOG_DEBUG;

SRTNet::SRTNet(const std::string& logPrefix) : mLogPrefix(logPrefix) {}

SRTNet::~SRTNet() {
    stop();
}

void SRTNet::closeAllClientSockets() {
    std::lock_guard<std::mutex> lock(mClientListMtx);
    for (auto& client : mClientList) {
        SRTSOCKET socket = client.first;
        int result = srt_close(socket);
        if (clientDisconnected) {
            clientDisconnected(client.second, socket);
        }
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_close failed: " << srt_getlasterror_str());
        }
    }
    mClientList.clear();
}

bool SRTNet::startServer(const std::string& ip,
                         uint16_t port,
                         int reorder,
                         int32_t latency,
                         int overhead,
                         int mtu,
                         int32_t peerIdleTimeout,
                         const std::string& psk,
                         bool singleClient,
                         std::shared_ptr<NetworkConnection> ctx) {
    std::lock_guard<std::mutex> lock(mNetMtx);

    SocketAddress socketAddress(ip, port);
    if (!socketAddress.isIPv4() && !socketAddress.isIPv6()) {
        SRT_LOGGER(true, LOGG_ERROR, "Failed to parse socket address");
        return false;
    }

    if (mCurrentMode != Mode::unknown) {
        SRT_LOGGER(true, LOGG_ERROR, "SRTNet mode is already set");
        return false;
    }

    if (!clientConnected) {
        SRT_LOGGER(true, LOGG_ERROR, "waitForSRTClient needs clientConnected callback method terminating server!");
        return false;
    }

    mConnectionContext = ctx; // retain the optional context

    mConfiguration.mLocalHost = ip;
    mConfiguration.mLocalPort = port;
    mConfiguration.mReorder = reorder;
    mConfiguration.mLatency = latency;
    mConfiguration.mOverhead = overhead;
    mConfiguration.mMtu = mtu;
    mConfiguration.mPeerIdleTimeout = peerIdleTimeout;
    mConfiguration.mPsk = psk;

    if (!createServerSocket()) {
        mContext = SRT_INVALID_SOCK;
        SRT_LOGGER(true, LOGG_ERROR, "Failed to create SRT server socket");
        return false;
    }

    mServerActive = true;
    mCurrentMode = Mode::server;

    if (singleClient) {
        mWorkerThread = std::thread(&SRTNet::serverSingleClientWorker, this);
    } else {
        mWorkerThread = std::thread(&SRTNet::waitForSRTClient, this, singleClient);
    }

    return true;
}

void SRTNet::serverSingleClientWorker() {
    while (mServerActive) {
        if (!waitForSRTClient(true)) {
            continue;
        }

        serverEventHandler(true);

        std::lock_guard<std::mutex> lock(mNetMtx);
        SRT_LOGGER(true, LOGG_NOTIFY, "Single client disconnected, wait for new client to connect");
        if (!createServerSocket()) {
            SRT_LOGGER(true, LOGG_ERROR, "Failed to re-create server socket");
            return;
        }
    }
}

void SRTNet::serverEventHandler(bool singleClient) {
    SRT_EPOLL_EVENT ready[MAX_WORKERS];

    while (mServerActive) {
        int ret = srt_epoll_uwait(mPollID, &ready[0], MAX_WORKERS, kEpollTimeoutMs);

        if (ret == -1 && mPollID != 0) {
            // If error and mPollId has not been reset by us, log error message
            SRT_LOGGER(true, LOGG_ERROR, "epoll error: " << srt_getlasterror_str());
            continue;
        }
        // Handle all ready sockets
        for (int i = 0; i < ret; i++) {
            uint8_t msg[2048];
            SRT_MSGCTRL thisMSGCTRL = srt_msgctrl_default;
            SRTSOCKET thisSocket = ready[i].fd;

            int result = SRT_ERROR;
            if (ready[i].events & SRT_EPOLL_IN) {
                result = srt_recvmsg2(thisSocket, reinterpret_cast<char*>(msg), sizeof(msg), &thisMSGCTRL);
            }

            std::lock_guard<std::mutex> lock(mClientListMtx);
            auto iterator = mClientList.find(thisSocket);
            if (iterator == mClientList.end()) {
                continue; // This client has already been removed by closeAllClientSockets()
            }

            if (result <= 0) {
                // 0 means connection was broken, -1 (SRT_ERROR) means error, and we treat it the same way
                SRT_LOGGER(true, LOG_DEBUG, "Connection to client was broken, removing client: " << thisSocket);
                auto ctx = iterator->second;
                mClientList.erase(iterator->first);
                srt_epoll_remove_usock(mPollID, thisSocket);
                srt_close(thisSocket);
                if (clientDisconnected) {
                    clientDisconnected(ctx, thisSocket);
                }

                // Client connection was broken, continue to next client
                continue;
            }

            // Pass the received data to the user
            if (receivedDataNoCopy) {
                receivedDataNoCopy(msg, result, thisMSGCTRL, iterator->second, thisSocket);
            } else if (receivedData) {
                auto pointer = std::make_unique<std::vector<uint8_t>>(msg, msg + result);
                receivedData(pointer, thisMSGCTRL, iterator->second, thisSocket);
            }
        }

        if (singleClient && mClientList.empty()) {
            break;
        }
    }
    SRT_LOGGER(true, LOGG_NOTIFY, "serverEventHandler exit");

    if (mPollID != 0) {
        // May have been released already by stop() function
        srt_epoll_release(mPollID);
    }
}

SRTNet::ClientConnectStatus SRTNet::clientConnectToServer() {
    // Get all remote addresses for connection
    struct addrinfo hints = {};
    struct addrinfo* resolvedAddresses;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_family = AF_UNSPEC;
    std::stringstream portAsString;
    portAsString << mConfiguration.mRemotePort;
    int result = getaddrinfo(mConfiguration.mRemoteHost.c_str(), portAsString.str().c_str(), &hints, &resolvedAddresses);
    if (result) {
        SRT_LOGGER(true, LOGG_ERROR,
                   "Failed getting the IP target for > " <<
                       mConfiguration.mRemoteHost << ":" << mConfiguration.mRemotePort << " Errno: " << result);
        return failToResolveAddress;
    }

    for (struct addrinfo* resolvedAddress = resolvedAddresses;
         resolvedAddress;
         resolvedAddress = resolvedAddress->ai_next) {
        result = srt_connect(mContext, reinterpret_cast<sockaddr*>(resolvedAddress->ai_addr),
                             resolvedAddress->ai_addrlen);
        if (result != SRT_ERROR) {
            mClientConnected = true;
            if (connectedToServer) {
                connectedToServer(mConnectionContext, mContext);
            }
            // Break for-loop on first successful connect call
            break;
        }
    }
    freeaddrinfo(resolvedAddresses);

    return mClientConnected ? success : failToConnect;
}

bool SRTNet::waitForSRTClient(bool singleClient) {
    mPollID = srt_epoll_create();
    srt_epoll_set(mPollID, SRT_EPOLL_ENABLE_EMPTY);
    if (!singleClient) {
        mEventThread = std::thread(&SRTNet::serverEventHandler, this, singleClient);
    }

    closeAllClientSockets();

    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    int serverSocketPollId = srt_epoll_create();
    int result = srt_epoll_add_usock(serverSocketPollId, mContext, &events);
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_add_usock error: " << srt_getlasterror_str());
    }
    SRT_EPOLL_EVENT ready[1];

    struct sockaddr_storage theirAddr;
    int addrSize = sizeof(theirAddr);
    while (mServerActive) {
        memset(&theirAddr, 0, addrSize);

        int ret = srt_epoll_uwait(serverSocketPollId, ready, 1, kEpollTimeoutMs);
        if (ret == 0) {
            // No events yet
            continue;
        } else if (ret < 0) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_uwait error: " << srt_getlasterror_str());
            break;
        } else if (ret > 1) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_uwait returned more than one event");
            break;
        } else if (ready[0].fd != mContext) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_uwait got event on unknown socket");
            break;
        }

        SRT_LOGGER(true, LOGG_NOTIFY, "SRT Server wait for client at port: " << getLocallyBoundPort());
        SRTSOCKET newSocketCandidate = srt_accept(mContext, reinterpret_cast<sockaddr*>(&theirAddr), &addrSize);
        if (newSocketCandidate == -1) {
            continue;
        }

        SRT_LOGGER(true, LOGG_NOTIFY, "Client connected: " << newSocketCandidate);
        auto ctx = clientConnected(*reinterpret_cast<sockaddr*>(&theirAddr), newSocketCandidate, mConnectionContext);

        if (!ctx) {
            // No ctx in return from clientConnected callback means client was rejected by user.
            close(newSocketCandidate);
            continue;
        }

        std::lock_guard<std::mutex> lock(mClientListMtx);
        mClientList[newSocketCandidate] = ctx;
        result = srt_epoll_add_usock(mPollID, newSocketCandidate, &events);
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_add_usock error: " << srt_getlasterror_str());
        }

        if (singleClient) {
            srt_epoll_release(serverSocketPollId);
            SRT_LOGGER(true, LOGG_NOTIFY, "SRT Server removing server socket from epoll");
            // If we're in singleClient mode and have an accepted client, we close the server socket
            // to not allow any other clients to connect to us.
            std::lock_guard<std::mutex> lockContext(mNetMtx);

            result = srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            if (result == SRT_ERROR) {
                SRT_LOGGER(true, LOGG_ERROR, "srt_close failed: " << srt_getlasterror_str());
            }
            return true;
        }
    }

    srt_epoll_release(serverSocketPollId);
    return false;
}

void SRTNet::getActiveClients(
    const std::function<void(std::map<SRTSOCKET, std::shared_ptr<NetworkConnection>>&)>& function) {
    std::lock_guard<std::mutex> lock(mClientListMtx);
    function(mClientList);
}

bool SRTNet::startClient(const std::string& host,
                         uint16_t port,
                         int reorder,
                         int32_t latency,
                         int overhead,
                         std::shared_ptr<NetworkConnection>& ctx,
                         int mtu,
                         bool failOnConnectionError,
                         int32_t peerIdleTimeout,
                         const std::string& psk) {
    return startClient(host, port, "", 0, reorder, latency, overhead, ctx, mtu, failOnConnectionError, peerIdleTimeout, psk);
}

// Host can provide an IP or name meaning any IPv4 or IPv6 address or name type www.google.com
// There is no IP-Version preference if a name is given. the first IP-version found will be used
bool SRTNet::startClient(const std::string& host,
                         uint16_t port,
                         const std::string& localHost,
                         uint16_t localPort,
                         int reorder,
                         int32_t latency,
                         int overhead,
                         std::shared_ptr<NetworkConnection>& ctx,
                         int mtu,
                         bool failOnConnectionError,
                         int32_t peerIdleTimeout,
                         const std::string& psk) {
    std::lock_guard<std::mutex> lock(mNetMtx);
    if (mCurrentMode != Mode::unknown) {
        SRT_LOGGER(true, LOGG_ERROR,
                   " "
                       << "SRTNet mode is already set");
        return false;
    }

    mClientContext = ctx;

    mConfiguration.mLocalHost = localHost;
    mConfiguration.mLocalPort = localPort;
    mConfiguration.mRemoteHost = host;
    mConfiguration.mRemotePort = port;
    mConfiguration.mReorder = reorder;
    mConfiguration.mLatency = latency;
    mConfiguration.mOverhead = overhead;
    mConfiguration.mMtu = mtu;
    mConfiguration.mPeerIdleTimeout = peerIdleTimeout;
    mConfiguration.mPsk = psk;

    if (!createClientSocket()) {
        SRT_LOGGER(true, LOGG_ERROR, "Failed to create caller socket");
        return false;
    }

    // Try to connect to the server
    ClientConnectStatus status = clientConnectToServer();
    if (status == failToResolveAddress) {
        SRT_LOGGER(true, LOGG_ERROR, "Failed to resolve address for " <<
                                         mConfiguration.mRemoteHost << ":" << mConfiguration.mRemotePort);
        srt_close(mContext);
        mContext = SRT_INVALID_SOCK;
        return false;
    } else if (status == failToConnect) {
        SRT_LOGGER(true, LOGG_WARN, "Failed to connect to " <<
                                        mConfiguration.mRemoteHost << ":" << mConfiguration.mRemotePort <<
                                        " " << srt_getlasterror_str());
        int rejectReason = srt_getrejectreason(mContext);
        if (failOnConnectionError ||
            rejectReason == SRT_REJECT_REASON::SRT_REJ_BADSECRET ||
            rejectReason == SRT_REJECT_REASON::SRT_REJ_UNSECURE) {
            srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            return false;
        }
    }

    SRT_LOGGER(true, LOGG_NOTIFY, "Connected to SRT Server");

    mCurrentMode = Mode::client;
    mClientActive = true;
    mWorkerThread = std::thread(&SRTNet::clientWorker, this);

    return true;
}

bool SRTNet::createServerSocket() {
    mContext = srt_create_socket();
    if (mContext == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_socket: " << srt_getlasterror_str());
        return false;
    }

    int32_t yes = 1;
    int result = srt_setsockflag(mContext, SRTO_RCVSYN, &yes, sizeof(yes));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_RCVSYN: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_LATENCY, &mConfiguration.mLatency, sizeof(mConfiguration.mLatency));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_LATENCY: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_LOSSMAXTTL, &mConfiguration.mReorder, sizeof(mConfiguration.mReorder));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_LOSSMAXTTL: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_OHEADBW, &mConfiguration.mOverhead, sizeof(mConfiguration.mOverhead));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_OHEADBW: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_PAYLOADSIZE, &mConfiguration.mMtu, sizeof(mConfiguration.mMtu));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_PAYLOADSIZE: " << srt_getlasterror_str());
        return false;
    }

    if (!mConfiguration.mPsk.empty()) {
        int32_t aes128 = 16;
        result = srt_setsockflag(mContext, SRTO_PBKEYLEN, &aes128, sizeof(aes128));
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_PBKEYLEN: " << srt_getlasterror_str());
            return false;
        }

        result = srt_setsockflag(mContext, SRTO_PASSPHRASE, mConfiguration.mPsk.c_str(), mConfiguration.mPsk.length());
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_PASSPHRASE: " << srt_getlasterror_str());
            return false;
        }
    }

    result = srt_setsockflag(mContext, SRTO_PEERIDLETIMEO, &mConfiguration.mPeerIdleTimeout, sizeof(mConfiguration.mPeerIdleTimeout));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag : SRTO_PEERIDLETIMEO" << srt_getlasterror_str());
        return false;
    }

    SocketAddress socketAddress(mConfiguration.mLocalHost, mConfiguration.mLocalPort);
    if (!socketAddress.isIPv4() && !socketAddress.isIPv6()) {
        SRT_LOGGER(true, LOGG_ERROR, "Failed to parse socket address");
        return false;
    }
    std::optional<sockaddr_in> ipv4Address = socketAddress.getIPv4();
    if (ipv4Address.has_value()) {
        result = srt_bind(mContext, reinterpret_cast<sockaddr*>(&ipv4Address.value()), sizeof(ipv4Address.value()));
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_bind: " << srt_getlasterror_str());
            srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            return false;
        }
    }

    std::optional<sockaddr_in6> ipv6Address = socketAddress.getIPv6();
    if (ipv6Address.has_value()) {
        if (mConfiguration.mLocalHost == "::") {
            result = srt_setsockflag(mContext, SRTO_IPV6ONLY, &yes, sizeof(yes));
            if (result == SRT_ERROR) {
                SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_IPV6ONLY: " << srt_getlasterror_str());
                return false;
            }
        }

        result = srt_bind(mContext, reinterpret_cast<sockaddr*>(&ipv6Address.value()), sizeof(ipv6Address.value()));
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_bind: " << srt_getlasterror_str());
            srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            return false;
        }
    }

    result = srt_listen(mContext, 2);
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_listen: " << srt_getlasterror_str());
        srt_close(mContext);
        mContext = SRT_INVALID_SOCK;
        return false;
    }

    // If server was started with local port 0, update the configuration with the actual port. So in case we're in
    // single client mode, we can re-open the same port if client disconnects.
    if (mConfiguration.mLocalPort == 0) {
        mConfiguration.mLocalPort = getLocallyBoundPort();
    }

    return true;
}

bool SRTNet::createClientSocket() {
    const int32_t yes = 1;

    mContext = srt_create_socket();
    if (mContext == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_socket: " << srt_getlasterror_str());
        return false;
    }

    int result = srt_setsockflag(mContext, SRTO_SENDER, &yes, sizeof(yes));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_SENDER: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_LATENCY, &mConfiguration.mLatency, sizeof(mConfiguration.mLatency));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_LATENCY: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_LOSSMAXTTL, &mConfiguration.mReorder, sizeof(mConfiguration.mReorder));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_LOSSMAXTTL: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_OHEADBW, &mConfiguration.mOverhead, sizeof(mConfiguration.mOverhead));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_OHEADBW: " << srt_getlasterror_str());
        return false;
    }

    result = srt_setsockflag(mContext, SRTO_PAYLOADSIZE, &mConfiguration.mMtu, sizeof(mConfiguration.mMtu));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_PAYLOADSIZE: " << srt_getlasterror_str());
        return false;
    }

    if (!mConfiguration.mPsk.empty()) {
        int32_t aes128 = 16;
        result = srt_setsockflag(mContext, SRTO_PBKEYLEN, &aes128, sizeof(aes128));
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_PBKEYLEN: " << srt_getlasterror_str());
            return false;
        }

        result = srt_setsockflag(mContext, SRTO_PASSPHRASE, mConfiguration.mPsk.c_str(), mConfiguration.mPsk.length());
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag SRTO_PASSPHRASE: " << srt_getlasterror_str());
            return false;
        }
    }

    result = srt_setsockflag(mContext, SRTO_PEERIDLETIMEO, &mConfiguration.mPeerIdleTimeout, sizeof(mConfiguration.mPeerIdleTimeout));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockflag : SRTO_PEERIDLETIMEO" << srt_getlasterror_str());
        return false;
    }

    const int connection_timeout_ms = kConnectionTimeout.count();
    result = srt_setsockopt(mContext, 0, SRTO_CONNTIMEO, &connection_timeout_ms, sizeof(connection_timeout_ms));
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_setsockopt: SRTO_CONNTIMEO" << srt_getlasterror_str());
        return false;
    }

    if (!mConfiguration.mLocalHost.empty() || mConfiguration.mLocalPort != 0) {
        // Set local interface to bind to

        if (mConfiguration.mLocalHost.empty()) {
            SRT_LOGGER(true, LOGG_ERROR,
                       "Local port was provided but local IP is not set, cannot bind to local address");
            srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            return false;
        }

        SocketAddress localSocketAddress(mConfiguration.mLocalHost, mConfiguration.mLocalPort);

        std::optional<sockaddr_in> localIPv4Address = localSocketAddress.getIPv4();
        if (localIPv4Address.has_value()) {
            result = srt_bind(mContext, reinterpret_cast<sockaddr*>(&localIPv4Address.value()),
                              sizeof(localIPv4Address.value()));
            if (result == SRT_ERROR) {
                SRT_LOGGER(true, LOGG_ERROR, "srt_bind: " << srt_getlasterror_str());
                srt_close(mContext);
                mContext = SRT_INVALID_SOCK;
                return false;
            }
        }

        std::optional<sockaddr_in6> localIPv6Address = localSocketAddress.getIPv6();
        if (localIPv6Address.has_value()) {
            result = srt_bind(mContext, reinterpret_cast<sockaddr*>(&localIPv6Address.value()),
                              sizeof(localIPv6Address.value()));
            if (result == SRT_ERROR) {
                SRT_LOGGER(true, LOGG_ERROR, "srt_bind: " << srt_getlasterror_str());
                srt_close(mContext);
                mContext = SRT_INVALID_SOCK;
                return false;
            }
        }

        if (!localIPv4Address.has_value() && !localIPv6Address.has_value()) {
            SRT_LOGGER(true, LOGG_ERROR, "Failed to parse local socket address.");
            srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            return false;
        }
    }

    return true;
}

void SRTNet::clientWorker() {
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    int clientSocketPollId = srt_epoll_create();
    int result = srt_epoll_add_usock(clientSocketPollId, mContext, &events);
    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_add_usock error: " << srt_getlasterror_str());
        return;
    }
    SRT_EPOLL_EVENT ready[1];
    uint8_t msg[2048];
    SRT_MSGCTRL thisMSGCTRL = srt_msgctrl_default;

    while (mClientActive) {
        if (!mClientConnected) {
            // Try to connect to the server
            ClientConnectStatus status = clientConnectToServer();
            if (status == failToConnect) {
                // Failed to connect caller/client, try again.
                int rejectReason = srt_getrejectreason(mContext);
                if (rejectReason == SRT_REJECT_REASON::SRT_REJ_TIMEOUT) {
                    SRT_LOGGER(true, LOGG_WARN, "Failed to connect to: " <<
                                                    mConfiguration.mRemoteHost << ":" << mConfiguration.mRemotePort);
                } else {
                    if (rejectReason == SRT_REJECT_REASON::SRT_REJ_BADSECRET ||
                        rejectReason == SRT_REJECT_REASON::SRT_REJ_UNSECURE) {
                        SRT_LOGGER(true, LOGG_ERROR, "Failed to connect to the server at: " <<
                                                         mConfiguration.mRemoteHost << ":" << mConfiguration.mRemotePort <<
                                                         " (bad PSK): " << srt_getlasterror_str());
                    } else {
                        SRT_LOGGER(true, LOGG_ERROR, "Failed to connect to the server at: "
                                                         << mConfiguration.mRemoteHost << ":" << mConfiguration.mRemotePort <<
                                                         "(" << rejectReason << ": " << srt_getlasterror_str());
                    }
                    // If it didn't fail with a timeout, we need to sleep for a while to not burst the CPU.
                    std::this_thread::sleep_for(kConnectionTimeout);
                }
                continue;
            } else if (status == failToResolveAddress) {
                SRT_LOGGER(true, LOGG_ERROR, "Failed to resolve address for " <<
                                                 mConfiguration.mRemoteHost << ":" << mConfiguration.mRemotePort);
                // If we fail to resolve the address, which previously have resolved fine, we sleep for a while to not
                // burst the CPU, and hope it resolves next time.
                std::this_thread::sleep_for(kConnectionTimeout);
                continue;
            }

            SRT_LOGGER(true, LOGG_NOTIFY, "Connected to SRT Server");
        }

        int ret = srt_epoll_uwait(clientSocketPollId, ready, 1, kEpollTimeoutMs);
        if (ret == 0) {
            // No events yet
            continue;
        } else if (ret < 0) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_uwait error: " << srt_getlasterror_str());
            break;
        } else if (ret > 1) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_uwait returned more than one event");
            break;
        } else if (ready[0].fd != mContext) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_uwait got event on unknown socket");
            break;
        }

        result = SRT_ERROR;

        if (ready[0].events & SRT_EPOLL_IN) {
            // Receive data from the server
            result = srt_recvmsg2(mContext, reinterpret_cast<char*>(msg), sizeof(msg), &thisMSGCTRL);
        }
        if (result <= 0) {
            // 0 means connection was broken, -1 (SRT_ERROR) means error, and we treat it the same way
            mClientConnected = false;

            SRTSOCKET context = mContext;
            if (mClientActive) {
                srt_epoll_remove_usock(clientSocketPollId, mContext);
                SRT_LOGGER(true, LOG_DEBUG, "Client got disconnected from server: " << srt_getlasterror_str());
                srt_close(mContext);
                if (!createClientSocket()) {
                    mContext = SRT_INVALID_SOCK;
                    SRT_LOGGER(true, LOGG_ERROR, "Failed to re-create caller socket");
                    break;
                }
                result = srt_epoll_add_usock(clientSocketPollId, mContext, &events);
                if (result == SRT_ERROR) {
                    SRT_LOGGER(true, LOGG_ERROR, "srt_epoll_add_usock error: " << srt_getlasterror_str());
                    break;
                }
            }
            if (clientDisconnected) {
                clientDisconnected(mClientContext, context);
            }
            continue;
        }

        if (receivedDataNoCopy) {
            receivedDataNoCopy(msg, result, thisMSGCTRL, mClientContext, mContext);
        } else if (receivedData) {
            auto data = std::make_unique<std::vector<uint8_t>>(msg, msg + result);
            receivedData(data, thisMSGCTRL, mClientContext, mContext);
        }
    }
    srt_epoll_release(clientSocketPollId);

    mClientActive = false;
}

std::pair<SRTSOCKET, std::shared_ptr<SRTNet::NetworkConnection>> SRTNet::getConnectedServer() {
    if (mCurrentMode == Mode::client) {
        return {mContext, mClientContext};
    }
    return {0, nullptr};
}

bool SRTNet::isConnectedToServer() const {
    return mClientConnected;
}

SRTSOCKET SRTNet::getBoundSocket() const {
    return mContext;
}

SRTNet::Mode SRTNet::getCurrentMode() const {
    std::lock_guard<std::mutex> lock(mNetMtx);
    return mCurrentMode;
}

void SRTNet::defaultLogHandler(void* opaque, int level, const char* file, int line, const char* area, const char* message) {
    std::cout << message << std::endl;
}

void SRTNet::setLogHandler(SRT_LOG_HANDLER_FN* handler, int loglevel) {
    gLogHandler = handler;
    gLogLevel = loglevel;

    srt_setloghandler(nullptr, handler);
    srt_setlogflags(SRT_LOGF_DISABLE_TIME | SRT_LOGF_DISABLE_THREADNAME | SRT_LOGF_DISABLE_SEVERITY | SRT_LOGF_DISABLE_EOL);
    srt_setloglevel(loglevel);
}


bool SRTNet::sendData(const uint8_t* data, size_t len, SRT_MSGCTRL* msgCtrl, SRTSOCKET targetSystem) {
    int result;

    if (mCurrentMode == Mode::client && mContext != SRT_INVALID_SOCK && mClientActive && mClientConnected) {
        result = srt_sendmsg2(mContext, reinterpret_cast<const char*>(data), len, msgCtrl);
    } else if (mCurrentMode == Mode::server && targetSystem && mServerActive) {
        result = srt_sendmsg2(targetSystem, reinterpret_cast<const char*>(data), len, msgCtrl);
    } else {
        SRT_LOGGER(true, LOGG_WARN, "Can't send data, the client is not active.");
        return false;
    }

    if (result == SRT_ERROR) {
        SRT_LOGGER(true, LOGG_ERROR, "srt_sendmsg2 failed: " << srt_getlasterror_str());
        return false;
    }

    if (size_t(result) != len) {
        SRT_LOGGER(true, LOGG_ERROR, "Failed sending all data");
        return false;
    }

    return true;
}

bool SRTNet::stop() {
    if (mCurrentMode == Mode::server) {
        // Signal the server to stop
        mServerActive = false;

        if (mWorkerThread.joinable()) {
            mWorkerThread.join();
        }

        if (mEventThread.joinable()) {
            mEventThread.join();
        }

        // Lock the mutex before manipulating the server context/socket
        std::unique_lock<std::mutex> lock(mNetMtx);

        if (mContext != SRT_INVALID_SOCK) {
            int result = srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            if (result == SRT_ERROR) {
                SRT_LOGGER(true, LOGG_ERROR, "srt_close failed: " << srt_getlasterror_str());
                return false;
            }
        }
        // By closing the client sockets, any blocking recv calls will return
        closeAllClientSockets();
        // Release the epoll id to "break" the event threads poll call earlier than the 1 second timeout.
        srt_epoll_release(mPollID);
        mPollID = 0;

        SRT_LOGGER(true, LOGG_NOTIFY, "Server stopped");
        mCurrentMode = Mode::unknown;
        return true;
    } else if (mCurrentMode == Mode::client) {
        mClientActive = false;

        if (mWorkerThread.joinable()) {
            mWorkerThread.join();
        }

        std::lock_guard<std::mutex> lock(mNetMtx);
        if (mContext != SRT_INVALID_SOCK) {
            int result = srt_close(mContext);
            mContext = SRT_INVALID_SOCK;
            if (result == SRT_ERROR) {
                SRT_LOGGER(true, LOGG_ERROR, "srt_close failed: " << srt_getlasterror_str());
                return false;
            }
        }
        mClientConnected = false;

        SRT_LOGGER(true, LOGG_NOTIFY, "Client stopped");
        mCurrentMode = Mode::unknown;
        return true;
    }

    return true;
}

bool SRTNet::getStatistics(SRT_TRACEBSTATS* currentStats, int clear, int instantaneous, SRTSOCKET targetSystem) {
    std::lock_guard<std::mutex> lock(mNetMtx);
    if (mCurrentMode == Mode::client && mClientActive && mContext != SRT_INVALID_SOCK) {
        if (!mClientConnected) {
            memset(currentStats, 0, sizeof(SRT_TRACEBSTATS));
            return true;
        }
        int result = srt_bistats(mContext, currentStats, clear, instantaneous);
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_bistats failed: " << srt_getlasterror_str());
            return false;
        }
    } else if (mCurrentMode == Mode::server && mServerActive && targetSystem) {
        int result = srt_bistats(targetSystem, currentStats, clear, instantaneous);
        if (result == SRT_ERROR) {
            SRT_LOGGER(true, LOGG_ERROR, "srt_bistats failed: " << srt_getlasterror_str());
            return false;
        }
    } else {
        SRT_LOGGER(true, LOGG_ERROR, "Statistics not available");
        return false;
    }
    return true;
}

uint16_t SRTNet::getLocallyBoundPort() const {
    sockaddr_storage socketName{};
    int32_t nameLength = sizeof(socketName);
    int32_t srtStatus =
        srt_getsockname(getBoundSocket(), reinterpret_cast<sockaddr*>(&socketName), &nameLength);
    if (srtStatus < 0) {
        return 0;
    }
    if (socketName.ss_family == AF_INET) {
        sockaddr_in* ipv4Address = reinterpret_cast<sockaddr_in*>(&socketName);
        return ntohs(ipv4Address->sin_port);
    } else if (socketName.ss_family == AF_INET6) {
        sockaddr_in6* ipv6Address = reinterpret_cast<sockaddr_in6*>(&socketName);
        return ntohs(ipv6Address->sin6_port);
    }

    return 0;
}

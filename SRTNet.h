//      __________  ____     _____ ____  ______   _       ______  ___    ____  ____  __________
//     / ____/ __ \/ __ \   / ___// __ \/_  __/  | |     / / __ \/   |  / __ \/ __ \/ ____/ __ \
//    / /   / /_/ / /_/ /   \__ \/ /_/ / / /     | | /| / / /_/ / /| | / /_/ / /_/ / __/ / /_/ /
//   / /___/ ____/ ____/   ___/ / _, _/ / /      | |/ |/ / _, _/ ___ |/ ____/ ____/ /___/ _, _/
//   \____/_/   /_/       /____/_/ |_| /_/       |__/|__/_/ |_/_/  |_/_/   /_/   /_____/_/ |_|
//
// Created by Anders Cedronius on 2019-04-21.
//

#pragma once

#include <any>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "srt/srtcore/srt.h"

#ifdef WIN32
#include <Winsock2.h>
#define _WINSOCKAPI_
#include <io.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else

#include <arpa/inet.h>

#endif

#define MAX_WORKERS 5 // Max number of connections to deal with each epoll

namespace SRTNetClearStats {
enum SRTNetClearStats : int { no, yes };
}

namespace SRTNetInstant {
enum SRTNetInstant : int { no, yes };
}

class SRTNet {
public:

    enum class Mode {
        unknown,
        server,
        client
    };

    // Fill this class with all information you need for the duration of the connection both client and server
    class NetworkConnection {
    public:
        std::any mObject;
    };

    SRTNet();

    /**
     *
     * @brief Constructor that also sets a log handler
     * @param handler The log handler to be used
     * @param loglevel The log level to use
     *
     **/
    SRTNet(SRT_LOG_HANDLER_FN* handler, int loglevel);

    virtual ~SRTNet();

    /**
     *
     * Starts an SRT Server
     *
     * @param localIP Listen IP
     * @param localPort Listen Port
     * @param reorder number of packets in re-order window
     * @param latency Max re-send window (ms) / also the delay of transmission
     * @param overhead % extra of the BW that will be allowed for re-transmission packets
     * @param mtu sets the MTU
     * @param peerIdleTimeout Optional Connection considered broken if no packet received before this timeout.
     * Defaults to 5 seconds.
     * @param psk Optional Pre Shared Key (AES-128)
     * @param singleClient set to true to accept just one client connection at the time to the server, otherwise the
     * server will keep waiting and accepting more incoming client connections
     * @param ctx optional context used only in the clientConnected callback
     * @return true if server was able to start
     */
    bool startServer(const std::string& localIP,
                     uint16_t localPort,
                     int reorder,
                     int32_t latency,
                     int overhead,
                     int mtu,
                     int32_t peerIdleTimeout = 5000,
                     const std::string& psk = "",
                     bool singleClient = false,
                     std::shared_ptr<NetworkConnection> ctx = {});

    /**
     *
     * Starts an SRT Client and connects to the server. If the server is currently not listening for connections, or the
     * client can't reach the server over the network, the client may keep on trying to re-connect or fail to start
     * depending on the value or \p failOnConnectionError.
     *
     * @param host Remote host IP or hostname
     * @param port Remote host Port
     * @param reorder number of packets in re-order window
     * @param latency Max re-send window (ms) / also the delay of transmission
     * @param overhead % extra of the BW that will be allowed for re-transmission packets
     * @param ctx the context used in the receivedData and receivedDataNoCopy callback
     * @param mtu sets the MTU
     * @param failOnConnectionError if set to true and the initial connection attempt to the server fails this function
     * will return false, if set to false the client will start an internal thread that will keep on trying to connect
     * to the server until stop() is called. If the initial connection attempt is successful, the client will always
     * try to re-connect to the server in case the client gets disconnected.
     * @param peerIdleTimeout Optional Connection considered broken if no packet received before this timeout.
     * Defaults to 5 seconds.
     * @param psk Optional Pre Shared Key (AES-128)
     * @return true if configuration was accepted and remote IP port could be resolved, false otherwise. It will also
     * return false in case the connection can be made but the provided PSK doesn't match the PSK on the server.
     */
    bool startClient(const std::string& host,
                     uint16_t port,
                     int reorder,
                     int32_t latency,
                     int overhead,
                     std::shared_ptr<NetworkConnection>& ctx,
                     int mtu,
                     bool failOnConnectionError,
                     int32_t peerIdleTimeout = 5000,
                     const std::string& psk = "");

    /**
     *
     * Starts an SRT Client with a specified local address to bind to and connects to the server. If the server is
     * currently not listening for connections, or the client can't reach the server over the network, the client may
     * keep on trying to re-connect or fail to start depending on the value or \p failOnConnectionError.
     *
     * @param host Remote host IP or hostname to connect to
     * @param port Remote host port to connect to
     * @param localHost Local host IP to bind to
     * @param localPort Local port to bind to, use 0 to automatically pick an unused port
     * @param reorder number of packets in re-order window
     * @param latency Max re-send window (ms) / also the delay of transmission
     * @param overhead % extra of the BW that will be allowed for re-transmission packets
     * @param ctx the context used in the receivedData and receivedDataNoCopy callback
     * @param mtu sets the MTU
     * @param failOnConnectionError if set to true and the initial connection attempt to the server fails this function
     * will return false, if set to false the client will start an internal thread that will keep on trying to connect
     * to the server until stop() is called. If the initial connection attempt is successful, the client will always
     * try to re-connect to the server in case the client gets disconnected.
     * @param peerIdleTimeout Optional Connection considered broken if no packet received before this timeout.
     * Defaults to 5 seconds.
     * @param psk Optional Pre Shared Key (AES-128)
     * @return true if configuration was accepted and remote IP port could be resolved, false otherwise. It will also
     * return false in case the connection can be made but the provided PSK doesn't match the PSK on the server.
     */
    bool startClient(const std::string& host,
                     uint16_t port,
                     const std::string& localHost,
                     uint16_t localPort,
                     int reorder,
                     int32_t latency,
                     int overhead,
                     std::shared_ptr<NetworkConnection>& ctx,
                     int mtu,
                     bool failOnConnectionError,
                     int32_t peerIdleTimeout = 5000,
                     const std::string& psk = "");

    /**
     *
     * Stops the service
     *
     * @return true if the service stopped successfully.
     */
    bool stop();

    /**
     *
     * Send data
     *
     * @param data pointer to the data
     * @param size size of the data
     * @param msgCtrl pointer to a SRT_MSGCTRL struct.
     * @param targetSystem the target sending the data to (used in server mode only)
     * @return true if sendData was able to send the data to the target.
     */
    bool sendData(const uint8_t* data, size_t size, SRT_MSGCTRL* msgCtrl, SRTSOCKET targetSystem = 0);

    /**
     *
     * Get connection statistics
     *
     * @param currentStats pointer to the statistics struct
     * @param clear Clears the data after reading SRTNetClearStats::yes or no
     * @param instantaneous Get the parameters now SRTNetInstant::yes or filtered values SRTNetInstant::no
     * @param targetSystem The target connection to get statistics about (used in server mode only)
     * @return true if statistics was populated.
     */
    bool getStatistics(SRT_TRACEBSTATS* currentStats, int clear, int instantaneous, SRTSOCKET targetSystem = 0);

    /**
     *
     * Get active clients (A server method)
     *
     * @param function. pass a function getting the map containing the list of active connections
     * Where the map contains the SRTSocketHandle (SRTSOCKET) and it's associated NetworkConnection you provided.
     */
    void
    getActiveClients(const std::function<void(std::map<SRTSOCKET, std::shared_ptr<NetworkConnection>>&)>& function);

    /**
     *
     * @brief Get the SRT socket and the network connection context object associated with the connected server. This
     * method only works when in client mode.
     * @returns The SRT socket and network connection context of the connected server in case this SRTNet is in client
     * mode and is connected to a SRT server. Returns {0, nullptr} if not in client mode or if in client mode and not
     * connected yet.
     *
     */
    std::pair<SRTSOCKET, std::shared_ptr<NetworkConnection>> getConnectedServer();

    /**
     * 
     * @brief Check if client is connected to remote end
     * @returns True if client is connected to the the remote end, false otherwise or if this instance is in server mode
    */
    bool isConnectedToServer() const;

    /**
     *
     * @brief Get the underlying, bound SRT socket. Works both in client and server mode.
     * @returns The bound socket in case there is one, otherwise 0.
     *
     */
    SRTSOCKET getBoundSocket() const;

    /**
     *
     * @brief Get the current operating mode.
     * @returns The operating mode.
     *
    */
    Mode getCurrentMode() const;

    /**
     *
     * @brief Default log handler which outputs the message to std::cout
     * @param opaque not used
     * @param level the log level of this message
     * @param file name of the file where this message is logged
     * @param line line number in the file
     * @param area not used
     * @param message the line to be logged
     *
     */
    static void defaultLogHandler(void* opaque, int level, const char* file, int line, const char* area, const char* message);

    /**
     *
     * @brief Set log handler
     * @param handler the new log handler to be used
     * @param loglevel the log level to use
     *
     */
    void setLogHandler(SRT_LOG_HANDLER_FN* handler, int loglevel);

    /// Callback handling connecting clients (only server mode)
    std::function<std::shared_ptr<NetworkConnection>(struct sockaddr& sin,
                                                     SRTSOCKET newSocket,
                                                     std::shared_ptr<NetworkConnection>& ctx)>
        clientConnected = nullptr;

    /// Callback receiving data type vector
    std::function<void(std::unique_ptr<std::vector<uint8_t>>& data,
                       SRT_MSGCTRL& msgCtrl,
                       std::shared_ptr<NetworkConnection>& ctx,
                       SRTSOCKET socket)>
        receivedData = nullptr;

    /// Callback receiving data no copy
    std::function<void(const uint8_t* data,
                       size_t size,
                       SRT_MSGCTRL& msgCtrl,
                       std::shared_ptr<NetworkConnection>& ctx,
                       SRTSOCKET socket)>
        receivedDataNoCopy = nullptr;

    /// Callback handling disconnecting clients (server and client mode)
    std::function<void(std::shared_ptr<NetworkConnection>& ctx, SRTSOCKET lSocket)> clientDisconnected = nullptr;

    // delete copy and move constructors and assign operators
    SRTNet(SRTNet const&) = delete;            // Copy construct
    SRTNet(SRTNet&&) = delete;                 // Move construct
    SRTNet& operator=(SRTNet const&) = delete; // Copy assign
    SRTNet& operator=(SRTNet&&) = delete;      // Move assign

private:
    // Internal struct for storing the incoming configuration to startClient/Server
    struct Configuration {
        std::string mLocalHost;
        uint16_t mLocalPort;
        std::string mRemoteHost;
        uint16_t mRemotePort;
        int mReorder;
        int32_t mLatency;
        int mOverhead;
        int mMtu;
        int32_t mPeerIdleTimeout;
        std::string mPsk;
    };

    /** Internal variables and methods
     *
     * The SRT Net class has two base modes, it can act as a server which accepts incoming client connections or as a
     * client which connects to a server. However, the server mode has two versions of it which is controlled by the
     * parameter singleClient (@see startServer method).
     *
     * If singleClient is true, it means that the server will only accept one single client at the time. The
     * server will run the serverSingleClientWorker function in a thread, which will synchronously call the
     * waitForSRTClient function which waits for a successful client connection and then closes the server socket and
     * returns. It will then proceed by calling the serverEventHandler function, which handles the communication with
     * the client exclusively until it disconnects or the server is stopped. If the client disconnects, the server will
     * re-create the server socket and again call the waitForSRTClient function to start waiting for a new connection
     * from a client.
     *
     * If singleClient is false, it means that the server will accept multiple client connections at the same time. The
     * waitForSRTClient function will run in a thread and accept new clients, adding them to an epoll context, and in
     * parallel the serverEventHandler function will run in another thread polling events from all clients from the
     * epoll context. The server socket remains open for incoming clients until the server is stopped.
     */

    /**
     * @brief Server worker thread function when server only accepts a single client.
     */
    void serverSingleClientWorker();

    /**
     * @brief Server worker thread function when server accepts multiple clients, otherwise
     * used as a normal function for accepting one single client connection.
     * @param singleClient If set to true, the function will exit and close the server socket once the first
     * client has successfully connected to the server, if set to false the function will keep on accepting
     * incoming connections until the server is stopped.
     * @return true if singleClient is true and a client has successfully connected, false otherwise.
     */
    bool waitForSRTClient(bool singleClient);

    /**
     * @brief Server thread function when server accepts multiple clients, otherwise
     * used as a normal function for handling events for one single client connection.
     * @param singleClient If set to true, the function will exit if the single accepted client disconnects, if
     * set to false the function will keep on polling for new events on client sockets until the server is stopped.
     */
    void serverEventHandler(bool singleClient);

    /**
     * @brief Client worker thread function.
     */
    void clientWorker();

    /**
     * @brief Server util function that closes all connected client sockets and calls the
     * clientDisconnected callback for each client.
     */
    void closeAllClientSockets();

    /**
     * @brief Util for configuring the server socket.
     * @return true if socket could be configured, false otherwise.
     */
    bool createServerSocket();

    /**
     * @brief Util for configuring the client socket.
     * @return true if socket could be configured, false otherwise.
     */
    bool createClientSocket();

    SRT_LOG_HANDLER_FN* logHandler = defaultLogHandler;

    // Server active? true == yes
    std::atomic<bool> mServerActive = {false};
    // Client active? true == yes
    std::atomic<bool> mClientActive = {false};

    std::thread mWorkerThread;
    std::thread mEventThread;

    SRTSOCKET mContext = 0;
    int mPollID = 0;
    mutable std::mutex mNetMtx;
    Mode mCurrentMode = Mode::unknown;
    std::map<SRTSOCKET, std::shared_ptr<NetworkConnection>> mClientList = {};
    std::mutex mClientListMtx;
    std::shared_ptr<NetworkConnection> mClientContext = nullptr;
    std::shared_ptr<NetworkConnection> mConnectionContext = nullptr;
    std::atomic<bool> mClientConnected = false;

    Configuration mConfiguration;

    const std::chrono::milliseconds kConnectionTimeout{1000};
};

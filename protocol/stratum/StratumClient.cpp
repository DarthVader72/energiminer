#include "StratumClient.h"

#include <energiminer/buildinfo.h>

#ifdef _WIN32
#include <wincrypt.h>
#endif

#define BOOST_ASIO_ENABLE_CANCELIO

static const auto DIFF1_TARGET = arith_uint256("0x00000000ffff0000000000000000000000000000000000000000000000000000");
static constexpr auto DIFF_MULT = 10e4;

static void diffToTarget(arith_uint256 &target, double diff)
{
    target = DIFF1_TARGET;
    uint64_t mdiff = diff * DIFF_MULT;
    target /= mdiff;
    target *= DIFF_MULT;
}

using boost::asio::ip::tcp;

StratumClient::StratumClient(boost::asio::io_service & io_service,
                             int worktimeout,
                             int responsetimeout,
                             bool submitHashrate)
    : PoolClient()
    , m_worktimeout(worktimeout)
    , m_responsetimeout(responsetimeout)
    , m_io_service(io_service)
    , m_io_strand(io_service)
    , m_socket(nullptr)
    , m_workloop_timer(io_service)
    , m_response_plea_times(64)
    , m_resolver(io_service)
    , m_endpoints()
    , m_nextWorkTarget(DIFF1_TARGET)
    , m_submit_hashrate(submitHashrate)
{
    // Initialize workloop_timer to infinite wait
    m_workloop_timer.expires_at(boost::posix_time::pos_infin);
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
                    &StratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));
    clear_response_pleas();
}

StratumClient::~StratumClient()
{
    // Do not stop io service.
    // It's global
}

void StratumClient::init_socket()
{
    // Prepare Socket
    if (m_conn->SecLevel() != SecureLevel::NONE) {
        boost::asio::ssl::context::method method = boost::asio::ssl::context::tls_client;
        if (m_conn->SecLevel() == SecureLevel::TLS12) {
            method = boost::asio::ssl::context::tlsv12;
        }

        boost::asio::ssl::context ctx(method);
        m_securesocket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
            m_io_service, ctx);
        m_socket = &m_securesocket->next_layer();


        m_securesocket->set_verify_mode(boost::asio::ssl::verify_peer);

#ifdef _WIN32
        HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
        if (hStore == nullptr) {
            return;
        }

        X509_STORE* store = X509_STORE_new();
        PCCERT_CONTEXT pContext = nullptr;
        while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != nullptr) {
            X509* x509 = d2i_X509(
                nullptr, (const unsigned char**)&pContext->pbCertEncoded, pContext->cbCertEncoded);
            if (x509 != nullptr) {
                X509_STORE_add_cert(store, x509);
                X509_free(x509);
            }
        }

        CertFreeCertificateContext(pContext);
        CertCloseStore(hStore, 0);

        SSL_CTX_set_cert_store(ctx.native_handle(), store);
#else
        char* certPath = getenv("SSL_CERT_FILE");
        try {
            ctx.load_verify_file(certPath ? certPath : "/etc/ssl/certs/ca-certificates.crt");
        } catch (...) {
            cwarn << "Failed to load ca certificates. Either the file "
                     "'/etc/ssl/certs/ca-certificates.crt' does not exist";
            cwarn << "or the environment variable SSL_CERT_FILE is set to an invalid or "
                     "inaccessible file.";
            cwarn << "It is possible that certificate verification can fail.";
        }
#endif
    } else {
        m_nonsecuresocket = std::make_shared<boost::asio::ip::tcp::socket>(m_io_service);
        m_socket = m_nonsecuresocket.get();
    }

    // Activate keep alive to detect disconnects
    unsigned int keepAlive = 10000;

#if defined(_WIN32)
    int32_t timeout = keepAlive;
    setsockopt(
        m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(
        m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = keepAlive / 1000;
    tv.tv_usec = keepAlive % 1000;
    setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void StratumClient::connect()
{
    // Prevent unnecessary and potentially dangerous recursion
    if (m_connecting.load(std::memory_order::memory_order_relaxed))
        return;

    // Start timing operations
    m_workloop_timer.expires_from_now(boost::posix_time::milliseconds(m_workloop_interval));
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
                    &StratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));

    m_canconnect.store(false, std::memory_order_relaxed);
    m_connected.store(false, std::memory_order_relaxed);
    m_subscribed.store(false, std::memory_order_relaxed);
    m_authorized.store(false, std::memory_order_relaxed);
    m_authpending.store(false, std::memory_order_relaxed);

    /**
     * "Before first job (work) is provided, pool MUST set difficulty by sending mining.set_difficulty
     * If pool does not set difficulty before first job, then miner can assume difficulty 1 was being set."
     * Those above statement imply we MAY NOT receive difficulty thus at each new connection restart from 1
     */
    m_nextWorkTarget = DIFF1_TARGET;
    m_extraNonce1 = "f000000f";

    // Initializes socket and eventually secure stream
    if (!m_socket)
        init_socket();

    // Begin resolve all ips associated to hostname
    // empty queue from any previous listed ip
    // calling the resolver each time is useful as most
    // load balancer will give Ips in different order
    m_endpoints = std::queue<boost::asio::ip::basic_endpoint<boost::asio::ip::tcp>>();
    m_endpoint = boost::asio::ip::basic_endpoint<boost::asio::ip::tcp>();
    m_resolver = tcp::resolver(m_io_service);
    tcp::resolver::query q(m_conn->Host(), toString(m_conn->Port()));

    //! start resolving async
    m_resolver.async_resolve(q,
            m_io_strand.wrap(boost::bind(&StratumClient::resolve_handler, this, boost::asio::placeholders::error, boost::asio::placeholders::iterator)));
}

void StratumClient::disconnect()
{
    // Prevent unnecessary recursion
    if (!m_connected.load(std::memory_order_relaxed) ||
        m_disconnecting.load(std::memory_order_relaxed))
        return;
    m_disconnecting.store(true, std::memory_order_relaxed);

    // Cancel any outstanding async operation
    if (m_socket)
        m_socket->cancel();

    if (m_socket && m_socket->is_open()) {
        try {
            boost::system::error_code sec;
            if (m_conn->SecLevel() != SecureLevel::NONE) {
                // This will initiate the exchange of "close_notify" message among parties.
                // If both client and server are connected then we expect the handler with success
                // As there may be a connection issue we also endorse a timeout
                m_securesocket->async_shutdown(m_io_strand.wrap(boost::bind(&StratumClient::onSSLShutdownCompleted, this, boost::asio::placeholders::error)));

                enqueue_response_plea();
                // Rest of disconnection is performed asynchronously
                return;
            } else {
                m_nonsecuresocket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, sec);
                m_socket->close();
            }
        } catch (const std::exception& ex) {
            cwarn << "Error while disconnecting: " << ex.what();
        }
        disconnect_finalize();
    }
}

void StratumClient::disconnect_finalize()
{
    if (m_conn->SecLevel() != SecureLevel::NONE) {
        if (m_securesocket->lowest_layer().is_open()) {
            // Manage error code if layer is already shut down
            boost::system::error_code ec;
            m_securesocket->lowest_layer().shutdown(
                boost::asio::ip::tcp::socket::shutdown_both, ec);
            m_securesocket->lowest_layer().close();
        }
        m_securesocket = nullptr;
        m_socket = nullptr;
    } else {
        m_socket = nullptr;
        m_nonsecuresocket = nullptr;
    }

    // Release locking flag and set connection status
    cnote << "Socket disconnected from: " << ActiveEndPoint();
    m_connected.store(false, std::memory_order_relaxed);
    m_subscribed.store(false, std::memory_order_relaxed);
    m_authorized.store(false, std::memory_order_relaxed);
    m_authpending.store(false, std::memory_order_relaxed);
    m_disconnecting.store(false, std::memory_order_relaxed);

    if (!m_conn->IsUnrecoverable()) {
        // If we got disconnected during autodetection phase
        // reissue a connect lowering stratum mode checks
        // m_canconnect flag is used to prevent never-ending loop when
        // remote endpoint rejects connections attempts persistently since the first
        if (!m_conn->StratumModeConfirmed() && m_canconnect.load(std::memory_order_relaxed)) {
            // Repost a new connection attempt and advance to next stratum test
            if (m_conn->StratumMode() > 0) {
                m_conn->SetStratumMode(m_conn->StratumMode() - 1);
                m_io_service.post(
                    m_io_strand.wrap(boost::bind(&StratumClient::start_connect, this)));
                return;
            } else {
                // There are no more stratum modes to test
                // Mark connection as unrecoverable and trash it
                m_conn->MarkUnrecoverable();
            }
        }
    }
    // Clear plea queue and stop timing
    std::chrono::steady_clock::time_point m_response_plea_time;
    clear_response_pleas();
    // Put the actor back to sleep
    m_workloop_timer.expires_at(boost::posix_time::pos_infin);
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
                    &StratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));

    // Trigger handlers
    if (m_onDisconnected) {
        m_onDisconnected();
    }
}

void StratumClient::resolve_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
    setThreadName("stratum");
    if (!ec) {
        // Start Connection Process and set timeout timer
        while (i != tcp::resolver::iterator()) {
            m_endpoints.push(i->endpoint());
            ++i;
        }
        m_resolver.cancel();
        // Resolver has finished so invoke connection asynchronously
        m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::start_connect, this)));
    } else {
        // Release locking flag and set connection status
        cwarn << "Could not resolve host: " << m_conn->Host() << ", " << ec.message();
        m_connected.store(false, std::memory_order_relaxed);
        m_connecting.store(false, std::memory_order_relaxed);
        // Trigger handlers
        if (m_onDisconnected) {
            m_onDisconnected();
        }
    }
}

void StratumClient::start_connect()
{
    if (m_connecting.load(std::memory_order_relaxed))
        return;
    m_connecting.store(true, std::memory_order::memory_order_relaxed);

    if (!m_endpoints.empty()) {
        // Pick the first endpoint in list.
        // Eventually endpoints get discarded on connection errors
        m_endpoint = m_endpoints.front();

        // Re-init socket if we need to
        if (m_socket == nullptr)
            init_socket();

        setThreadName("stratum");
        if (g_logVerbosity >= 6)
            cnote << ("Trying " + toString(m_endpoint) + " ...");

        clear_response_pleas();

        m_connecting.store(true, std::memory_order::memory_order_relaxed);
        enqueue_response_plea();

        // Start connecting async
        if (m_conn->SecLevel() != SecureLevel::NONE) {
            m_securesocket->lowest_layer().async_connect(m_endpoint,
                    m_io_strand.wrap(boost::bind(&StratumClient::connect_handler, this, _1)));
        } else {
            m_socket->async_connect(m_endpoint,
                    m_io_strand.wrap(boost::bind(&StratumClient::connect_handler, this, _1)));
        }
    } else {
        setThreadName("stratum");
        m_connecting.store(false, std::memory_order_relaxed);
        cwarn << "No more Ip addresses to try for host: " << m_conn->Host();
        // Trigger handlers
        if (m_onDisconnected) {
            m_onDisconnected();
        }
    }
}

void StratumClient::workloop_timer_elapsed(const boost::system::error_code& ec)
{
    using namespace std::chrono;
    // On timer cancelled or nothing to check for then early exit
    if (ec == boost::asio::error::operation_aborted)
        return;
    // Check whether the deadline has passed. We compare the deadline against
    // the current time since a new asynchronous operation may have moved the
    // deadline before this actor had a chance to run.
    if (m_response_pleas_count.load(std::memory_order_relaxed)) {
        milliseconds response_delay_ms(0);
        steady_clock::time_point m_response_plea_time(m_response_plea_older.load(std::memory_order_relaxed));

        // Check responses while in connection/disconnection phase
        if (isPendingState()) {
            response_delay_ms =
                duration_cast<milliseconds>(steady_clock::now() - m_response_plea_time);
            if ((m_responsetimeout * 1000) >= response_delay_ms.count()) {
                if (m_connecting.load(std::memory_order_relaxed)) {
                    // The socket is closed so that any outstanding
                    // asynchronous connection operations are cancelled.
                    m_socket->close();
                    return;
                }
                // This is set for SSL disconnection
                if (m_disconnecting.load(std::memory_order_relaxed) &&
                        (m_conn->SecLevel() != SecureLevel::NONE)) {
                    if (m_securesocket->lowest_layer().is_open()) {
                        m_securesocket->lowest_layer().close();
                        return;
                    }
                }
            }
		}
        if (isConnected()) {
            response_delay_ms =
                duration_cast<milliseconds>(steady_clock::now() - m_response_plea_time);

            if (response_delay_ms.count() >= (m_responsetimeout * 1000)) {
                if (m_conn->StratumModeConfirmed() == false && m_conn->IsUnrecoverable() == false) {
                    // Waiting for a response from pool to a login request
                    // Async self send a fake error response
                    Json::Value jRes;
                    jRes["id"] = unsigned(1);
                    jRes["result"] = Json::nullValue;
                    jRes["error"] = true;
                    clear_response_pleas();
                    m_io_service.post(m_io_strand.wrap(
                                boost::bind(&StratumClient::processResponse, this, jRes)));
                } else {
                    // Waiting for a response to solution submission
                    setThreadName("stratum");
                    cwarn << "No response received in " << m_responsetimeout << " seconds.";
                    m_endpoints.pop();
                    m_subscribed.store(false, std::memory_order_relaxed);
                    m_authorized.store(false, std::memory_order_relaxed);
                    clear_response_pleas();
                    m_io_service.post(
                            m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                }

            }
             // Check how old is last job received
            if (duration_cast<seconds>(steady_clock::now() - m_current_timestamp).count() >
                m_worktimeout) {
                setThreadName("stratum");
                cwarn << "No new work received in " << m_worktimeout << " seconds.";
                m_endpoints.pop();
                m_subscribed.store(false, std::memory_order_relaxed);
                m_authorized.store(false, std::memory_order_relaxed);
                clear_response_pleas();
                m_io_service.post(
                    m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
            }
        }
	}
    // Resubmit timing operations
    m_workloop_timer.expires_from_now(boost::posix_time::milliseconds(m_workloop_interval));
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
        &StratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));

}

void StratumClient::connect_handler(const boost::system::error_code& ec)
{
    setThreadName("stratum");
    // Set status completion
    m_connecting.store(false, std::memory_order_relaxed);

    // Timeout has run before or we got error
    if (ec || !m_socket->is_open()) {
        cwarn << ("Error  " + toString(m_endpoint) + " [ " + (ec ? ec.message() : "Timeout") +
                  " ]");

        // We need to close the socket used in the previous connection attempt
        // before starting a new one.
        // In case of error, in fact, boost does not close the socket
        // If socket is not opened it means we got timed out
        if (m_socket->is_open()) {
            m_socket->close();
        }

        // Discard this endpoint and try the next available.
        // Eventually is start_connect which will check for an
        // empty list.
        m_endpoints.pop();
        m_canconnect.store(false, std::memory_order_relaxed);
        m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::start_connect, this)));

        return;
    }

    // We got a socket connection established
    m_canconnect.store(true, std::memory_order_relaxed);
    cnote << "Socket connected to: " << ActiveEndPoint();
    if (m_conn->SecLevel() != SecureLevel::NONE) {
        boost::system::error_code hec;
        m_securesocket->lowest_layer().set_option(boost::asio::socket_base::keep_alive(true));
        m_securesocket->lowest_layer().set_option(tcp::no_delay(true));

        m_securesocket->handshake(boost::asio::ssl::stream_base::client, hec);

        if (hec) {
            cwarn << "SSL/TLS Handshake failed: " << hec.message();
            if (hec.value() == 337047686) {  // certificate verification failed
                cwarn << "This can have multiple reasons:";
                cwarn << "* Root certs are either not installed or not found";
                cwarn << "* Pool uses a self-signed certificate";
                cwarn << "Possible fixes:";
                cwarn << "* Make sure the file '/etc/ssl/certs/ca-certificates.crt' exists and "
                         "is accessible";
                cwarn << "* Export the correct path via 'export "
                         "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt' to the correct "
                         "file";
                cwarn << "  On most systems you can install the 'ca-certificates' package";
                cwarn << "  You can also get the latest file here: "
                         "https://curl.haxx.se/docs/caextract.html";
                cwarn << "* Disable certificate verification all-together via command-line "
                         "option.";
            }

            // This is a fatal error
            // No need to try other IPs as the certificate is based on host-name
            // not ip address. Trying other IPs would end up with the very same error.
            m_canconnect.store(false, std::memory_order_relaxed);
            m_conn->MarkUnrecoverable();
            m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
            return;
        }
    } else {
        m_nonsecuresocket->set_option(boost::asio::socket_base::keep_alive(true));
        m_nonsecuresocket->set_option(tcp::no_delay(true));
    }

    // Here is where we're properly connected
    m_connected.store(true, std::memory_order_relaxed);

    // Clean buffer from any previous stale data
    m_sendBuffer.consume(4096);
    clear_response_pleas();

    // Trigger event handlers and begin counting for the next job
    //reset_work_timeout();

    size_t p;
    m_worker.clear();
    p = m_conn->User().find_first_of(".");
    if (p != std::string::npos) {
        m_user = m_conn->User().substr(0, p);
        // There should be at least one char after dot
        // returned p is zero based
        if (p < (m_conn->User().length() -1))
            m_worker = m_conn->User().substr(++p);
    } else {
        m_user = m_conn->User();
    }

    /*
       If connection has been set-up with a specific scheme then
       set it's related stratum version as confirmed.
       Otherwise let's go through an autodetection.
       Autodetection process passes all known stratum modes.
       - 1st pass StratumClient::ENERGISTRATUM  (2)
       - 2nd pass StratumClient::NRGPROXY       (1)
       - 3rd pass StratumClient::STRATUM        (0)
    */

    if (m_conn->Version() < 999) {
        m_conn->SetStratumMode(m_conn->Version(), true);
    } else {
        if (!m_conn->StratumModeConfirmed() && m_conn->StratumMode() == 999)
            m_conn->SetStratumMode(2, false);
    }

    Json::Value jReq;
    jReq["id"] = unsigned(1);
    jReq["method"] = "mining.subscribe";
    jReq["params"] = Json::Value(Json::arrayValue);

    switch (m_conn->StratumMode()) {
        case 0:
            jReq["jsonrpc"] = "2.0";
            break;
        case 1:
            jReq["params"] = Json::Value(Json::arrayValue);
            if (m_worker.length())
                jReq["worker"] = m_worker;
            jReq["params"].append(m_user + m_conn->Path());
            break;
        case 2:
            jReq["params"] = Json::Value(Json::arrayValue);
            if (m_worker.length())
                jReq["worker"] = m_worker;
            jReq["params"].append(m_user + m_conn->Path());
            //jReq["params"].append(energiminer_get_buildinfo()->project_name_with_version);
            //jReq["params"].append("EnergiminerStratum/1.0.0");
            break;
        default:
            break;
    }
    // Begin receive data
    recvSocketData();
    /*
       +        Send first message
       +        NOTE !!
       +        It's been tested that f2pool.com does not respond with json error to wrong
       +        access message (which is needed to autodetect stratum mode).
       +        IT DOES NOT RESPOND AT ALL !!
       +        Due to this we need to set a timeout (arbitrary set to 1 second) and
       +        if no response within that time consider the tentative login failed
       +        and switch to next stratum mode test
       +        */
    sendSocketData(jReq);
}

std::string StratumClient::processError(Json::Value& responseObject)
{
    std::string retVar;

    if (responseObject.isMember("error") &&
            !responseObject.get("error", Json::Value::null).isNull()) {
        if (responseObject["error"].isConvertibleTo(Json::ValueType::stringValue)) {
            retVar = responseObject.get("error", "Unknown error").asString();
        } else if (responseObject["error"].isConvertibleTo(Json::ValueType::arrayValue)) {
            for (auto i : responseObject["error"]) {
                retVar += i.asString() + " ";
            }
        } else if (responseObject["error"].isConvertibleTo(Json::ValueType::objectValue)) {
            for (Json::Value::iterator i = responseObject["error"].begin(); i != responseObject["error"].end(); ++i) {
                Json::Value k = i.key();
                Json::Value v = (*i);
                retVar += (std::string)i.name() + ":" + v.asString() + " ";
            }
        }

    } else {
        retVar = "Unknown error";
    }

    return retVar;

}

void StratumClient::processExtranonce(std::string& enonce)
{
    cnote << "Extranonce set to: " + enonce;
    m_extraNonce1 = enonce;
}

void StratumClient::processResponse(Json::Value& responseObject)
{
    setThreadName("stratum");
	int _rpcVer = responseObject.isMember("jsonrpc") ? 2 : 1;
    bool _isNotification = false;		// Wether or not this message is a reply to previous request or is a broadcast notification
    bool _isSuccess = false;			// Wether or not this is a succesful or failed response (implies _isNotification = false)
    std::string _errReason = "";				// Content of the error reason
    std::string _method = "";				// The method of the notification (or request from pool)
    int _id = 0;						// This SHOULD be the same id as the request it is responding to (known exception is ethermine.org using 999)


    // Retrieve essential values
    _id = responseObject.get("id", unsigned(0)).asUInt();
    _isSuccess = responseObject.get("error", Json::Value::null).empty();
    _errReason = (_isSuccess ? "" : processError(responseObject));
    _method = responseObject.get("method", "").asString();
    _isNotification = ( _method != "" || _id == unsigned(0) );

    // Notifications of new jobs are like responses to get_work requests
    if (_isNotification && _method == "" && m_conn->StratumMode() == StratumClient::NRGPROXY &&
            responseObject["result"].isArray()) {
        _method = "mining.notify";
    }

    // Very minimal sanity checks
    // - For rpc2 member "jsonrpc" MUST be valued to "2.0"
    // - For responses ... well ... whatever
    // - For notifications I must receive "method" member and a not empty "params" or "result" member
    if ((_rpcVer == 2 && (!responseObject["jsonrpc"].isString() ||
                          responseObject.get("jsonrpc", "") != "2.0")) ||
            (_isNotification && (responseObject["params"].empty() && responseObject["result"].empty()))
       )
    {
        cwarn << "Pool sent an invalid jsonrpc message ...";
        cwarn << "Do not blame energiminer for this. Ask pool devs to honor http://www.jsonrpc.org/ specifications ";
        cwarn << "Disconnecting ...";
        m_subscribed.store(false, std::memory_order_relaxed);
        m_authorized.store(false, std::memory_order_relaxed);
        m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
        return;
    }

    // Handle awaited responses to OUR requests (calc response times)
    if (!_isNotification) {
        Json::Value jReq;
        Json::Value jResult = responseObject.get("result", Json::Value::null);
        std::chrono::milliseconds response_delay_ms(0);

        switch (_id) {
        case 1:
            response_delay_ms = dequeue_response_plea();
            /*
               This is the response to very first message after connection.
               I wish I could manage to have different Ids but apparently ethermine.org always replies
               to first message with id=1 regardless the id originally sent.
               */
            if (!m_conn->StratumModeConfirmed()) {
                if (!_isSuccess) {
                    // Disconnect and Proceed with next step of autodetection
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                    return;
                }

                switch (m_conn->StratumMode()) {
                    case StratumClient::ENERGISTRATUM:
                        m_conn->SetStratumMode(2, true);
                        break;
                    case StratumClient::NRGPROXY:
                        m_conn->SetStratumMode(1, true);
                        break;
                    case StratumClient::STRATUM:
                        m_conn->SetStratumMode(0, true);
                        break;
                }
            }
            // Response to "mining.subscribe" (https://en.bitcoin.it/wiki/Stratum_mining_protocol#mining.subscribe)
            // Result should be an array with multiple dimensions, we only care about the data if StratumClient::ENERGISTRATUM
            switch (m_conn->StratumMode()) {
            case StratumClient::STRATUM:
                cnote << "Stratum mode detected: STRATUM";
                m_subscribed.store(_isSuccess, std::memory_order_relaxed);
                if (!m_subscribed) {
                    cnote << "Could not subscribe to stratum server";
                    m_conn->MarkUnrecoverable();
                    m_io_service.post(
                            m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                    return;
                } else {
                    cnote << "Subscribed !";
                    m_authpending.store(true, std::memory_order_relaxed);
                    jReq["id"] = unsigned(3);
                    jReq["jsonrpc"] = "2.0";
                    jReq["method"] = "mining.authorize";
                    jReq["params"] = Json::Value(Json::arrayValue);
                    jReq["params"].append(m_conn->User() + m_conn->Path());
                    jReq["params"].append(m_conn->Pass());
                    enqueue_response_plea();
                }
                break;
            case StratumClient::NRGPROXY:
                cnote << "Stratum mode detected: nrg-proxy";
                m_subscribed.store(_isSuccess, std::memory_order_relaxed);
                if (!m_subscribed) {
                    cnote << "Could not login:" << _errReason;
                    m_conn->MarkUnrecoverable();
                    m_io_service.post(
                            m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                    return;
                } else {
                    cnote << "Logged in to nrg-proxy server";
                    if (!jResult.empty() && jResult.isArray()) {
                        std::string enonce = jResult.get((Json::Value::ArrayIndex)1, "").asString();
                        processExtranonce(enonce);
                    }
                    m_authorized.store(true, std::memory_order_relaxed);

                    // If we get here we have a valid application connection
                    // not only a socket connection
                    if (m_onConnected && m_conn->StratumModeConfirmed()) {
                        m_current_timestamp = std::chrono::steady_clock::now();
                        m_onConnected();
                    }
                    jReq["id"] = unsigned(3);
                    jReq["jsonrpc"] = "2.0";
                    jReq["method"] = "mining.authorize";
                    jReq["params"] = Json::Value(Json::arrayValue);
                    jReq["params"].append(m_conn->User() + m_conn->Path());
                    jReq["params"].append(m_conn->Pass());
                    enqueue_response_plea();
                }
                break;
            case StratumClient::ENERGISTRATUM:
                cnote << "Stratum mode detected: NRGSTRATUM";
                m_subscribed.store(_isSuccess, std::memory_order_relaxed);
                if (!m_subscribed) {
                    cnote << "Could not subscribe to stratum server:" << _errReason;
                    m_conn->MarkUnrecoverable();
                    m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                    return;
                } else {
                    cnote << "Subscribed to stratum server";
                    if (!jResult.empty() && jResult.isArray()) {
                        std::string enonce = jResult.get((Json::Value::ArrayIndex)1, "").asString();
                        processExtranonce(enonce);
                    }
                    // Eventually request authorization
                    jReq["id"] = unsigned(3);
                    jReq["method"] = "mining.authorize";
                    jReq["params"].append(m_conn->User() + m_conn->Path());
                    jReq["params"].append(m_conn->Pass());
                    enqueue_response_plea();
                }
                break;
            }
            sendSocketData(jReq);
            break;
        case 2:
            // This is the response to mining.extranonce.subscribe
            // according to this
            // https://github.com/nicehash/Specifications/blob/master/NiceHash_extranonce_subscribe_extension.txt
            // In all cases, client does not perform any logic when receiving back these replies.
            // With mining.extranonce.subscribe subscription, client should handle extranonce1
            // changes correctly
            // Nothing to do here.
            break;
        case 3:
            response_delay_ms = dequeue_response_plea();
            // Response to "mining.authorize" (https://en.bitcoin.it/wiki/Stratum_mining_protocol#mining.authorize)
            // Result should be boolean, some pools also throw an error, so _isSuccess can be false
            // Due to this reevaluate _isSuccess
            if (_isSuccess && jResult.isBool()) {
                _isSuccess = jResult.asBool();
            }
            m_authpending.store(false, std::memory_order_relaxed);
            m_authorized.store(_isSuccess, std::memory_order_relaxed);
            if (!m_authorized) {
                cnote << "Worker not authorized: " << m_conn->User() << _errReason;
                m_conn->MarkUnrecoverable();
                m_io_service.post(
                        m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                return;
            } else {
                cnote << "Authorized worker: " + m_conn->User();
                // If we get here we have a valid application connection
                // not only a socket connection
                if (m_onConnected && m_conn->StratumModeConfirmed()) {
                    m_current_timestamp = std::chrono::steady_clock::now();
                    m_onConnected();
                }

            }
            break;
        case 4:
            response_delay_ms = dequeue_response_plea();
            // Response to solution submission mining.submit  (https://en.bitcoin.it/wiki/Stratum_mining_protocol#mining.submit)
            // Result should be boolean, some pools also throw an error, so _isSuccess can be false
            // Due to this reevaluate _isSucess
            if (_isSuccess && jResult.isBool()) {
                _isSuccess = jResult.asBool();
            }
            {
                dequeue_response_plea();
                if (_isSuccess) {
                    if (m_onSolutionAccepted) {
                        m_onSolutionAccepted(false, response_delay_ms);
                    }
                } else {
                    if (m_onSolutionRejected) {
                        if (!_errReason.empty()) {
                            cwarn << "Reject reason: " << (_errReason.empty() ? "Unspecified" : _errReason);
                        }
                        m_onSolutionRejected(true, response_delay_ms);
                    }
                }
            }
            break;
        case 5:

            // This is the response we get on first get_work request issued
            // in mode StratumClient::NRGPROXY
            // thus we change it to a mining.notify notification
            if (m_conn->StratumMode() == StratumClient::NRGPROXY && responseObject["result"].isArray()) {
                _method = "mining.notify";
                _isNotification = true;
            }
            break;
        case 9:

            // Response to hashrate submit
            // Shall we do anything ?
            // Hashrate submit is actually out of stratum spec
            if (!_isSuccess) {
                cwarn << "Submit hashRate failed: " << (_errReason.empty() ? "Unspecified error" : _errReason);
            }
            break;
        case 999:
            // This unfortunate case should not happen as none of the outgoing requests is marked with id 999
            // However it has been tested that ethermine.org responds with this id when error replying to
            // either mining.subscribe (1) or mining.authorize requests (3)
            // To properly handle this situation we need to rely on Subscribed/Authorized states
            response_delay_ms = dequeue_response_plea();
            if (!_isSuccess) {
                if (!m_subscribed) {
                    // Subscription pending
                    cnote << "Subscription failed: " << _errReason;
                    m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                    return;
                } else if (m_subscribed && !m_authorized) {
                    // Authorization pending
                    cnote << "Worker not authorized: " << _errReason;
                    m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
                    return;
                }
            };
            break;
        default:
            // Never sent message with such an Id. What is it ?
            cnote << "Got response for unknown message id [" << _id << "] Discarding ...";
            break;
        }
    }
    // Handle unsolicited messages FROM pool
    // AKA notifications
    if (_isNotification && m_conn->StratumModeConfirmed()) {
        Json::Value jReq;
        Json::Value jPrm;
        jPrm = responseObject.get("params", Json::Value::null);
        if (_method == "mining.notify") {
            if (jPrm.isArray()) {
                if (!jPrm.get((Json::Value::ArrayIndex)2, "").asString().empty() &&
                    !jPrm.get((Json::Value::ArrayIndex)3, "").asString().empty()) {

                    bool resetJob = jPrm.get((Json::Value::ArrayIndex)8, "").asBool();

                    auto work = energi::Work(jPrm, m_extraNonce1, m_nextWorkTarget);
                    if (resetJob || m_current != work) {
                        if (m_onResetWork) {
                            m_onResetWork();
                        }
                        m_current = work;
                        m_current_timestamp = std::chrono::steady_clock::now();
                        if (m_onWorkReceived) {
                            m_onWorkReceived(m_current);
                        }
                    }
                }
            }
        } else if (_method == "mining.set_difficulty") {
            jPrm = responseObject.get("params", Json::Value::null);
            if (jPrm.isArray()) {
                double nextWorkDifficulty = std::max(jPrm.get((Json::Value::ArrayIndex)0, 1).asDouble(), 0.0001);
                diffToTarget(m_nextWorkTarget, nextWorkDifficulty);
                cnote << "Difficulty set to: "  << nextWorkDifficulty << " = " << m_nextWorkTarget.GetHex();
                m_current.reset();
            }
        } else if (_method == "mining.set_extranonce") {
            jPrm = responseObject.get("params", Json::Value::null);
            if (jPrm.isArray()) {
                std::string enonce = jPrm.get((Json::Value::ArrayIndex)0, "").asString();
                processExtranonce(enonce);
            }
        } else if (_method == "client.get_version") {
            jReq["id"] = toString(_id);
            jReq["result"] = energiminer_get_buildinfo()->project_name_with_version;
            if (_rpcVer == 1) {
                jReq["error"] = Json::Value::null;
            } else if (_rpcVer == 2) {
                jReq["jsonrpc"] = "2.0";
            }
            sendSocketData(jReq);
        } else {
            cwarn << "Got unknown method [" << _method << "] from pool. Discarding ...";
            // Respond back to issuer
            if (_rpcVer == 2) {
                jReq["jsonrpc"] = "2.0";
            }
            jReq["id"] = toString(_id);
            jReq["error"] = "Method not found";

            sendSocketData(jReq);
        }
    }
}

void StratumClient::submitHashrate(const std::string& rate)
{
    if(rate.empty()) {
        return;
    }
	if (!m_submit_hashrate || !isConnected()) {
		return;
	}

	// There is no stratum method to submit the hashrate so we use the rpc variant.
	// Note !!
	// id = 6 is also the id used by ethermine.org and nanopool to push new jobs
	// thus we will be in trouble if we want to check the result of hashrate submission
	// actually change the id from 6 to 9
}

void StratumClient::submitSolution(const Solution& solution)
{
    if (!m_subscribed.load(std::memory_order_relaxed) ||
            !m_authorized.load(std::memory_order_relaxed)) {
        cwarn << "Not authorized";
        return;
    }

    const auto &work = solution.getWork();

    if (m_current != work) {
        // stale
        return;
    }

    if (m_response_pleas_count.load(std::memory_order_relaxed) > PARALLEL_REQUEST_LIMIT) {
        cwarn << "Reject reason: throttling submitted requests";

        if (m_onSolutionRejected) {
            m_onSolutionRejected(true, std::chrono::milliseconds(0));
        }

        return;
    }

    Json::Value jReq;
    jReq["id"] = unsigned(4);
    jReq["method"] = "mining.submit";
    jReq["params"] = Json::Value(Json::arrayValue);

    jReq["jsonrpc"] = "2.0";
    jReq["params"].append(m_conn->User());
    jReq["params"].append(solution.getJobName());
    jReq["params"].append(solution.getExtraNonce2());
    jReq["params"].append(solution.getTime());
    jReq["params"].append(solution.getNonce());
    jReq["params"].append(solution.getHashMix().GetHex());
    jReq["params"].append(solution.getBlockTransaction());
    jReq["params"].append(work.getMerkleRoot().GetHex());
    if (m_worker.length()) {
        jReq["worker"] = m_worker;
    }
    
    enqueue_response_plea();
    sendSocketData(jReq);
}

void StratumClient::recvSocketData()
{
    if (m_conn->SecLevel() != SecureLevel::NONE) {
        async_read_until(*m_securesocket, m_recvBuffer, "\n",
                m_io_strand.wrap(boost::bind(&StratumClient::onRecvSocketDataCompleted, this,
                        boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)));
    } else {
        async_read_until(*m_nonsecuresocket, m_recvBuffer, "\n",
                m_io_strand.wrap(boost::bind(&StratumClient::onRecvSocketDataCompleted, this,
                        boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)));
    }
}

void StratumClient::onRecvSocketDataCompleted(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    // Due to the nature of io_service's queue and
    // the implementation of the loop this event may trigger
    // late after clean disconnection. Check status of connection
    // before triggering all stack of calls
    setThreadName("stratum");
    if (!ec && bytes_transferred > 0) {
        // Extract received message
        std::istream is(&m_recvBuffer);
        std::string message;
        getline(is, message);
        if (isConnected()) {
            if (!message.empty()) {
                // Test validity of chunk and process
                Json::Value jMsg;
                Json::Reader jRdr;
                if (jRdr.parse(message, jMsg)) {
                    m_io_service.post(boost::bind(&StratumClient::processResponse, this, jMsg));
                } else {
                    if (g_logVerbosity >= 6)
                        cwarn << "Got invalid Json message: " + jRdr.getFormattedErrorMessages();
                }
            }
            // Eventually keep reading from socket
            recvSocketData();
        }
    } else {
        if (isConnected()) {
            if (m_authpending.load(std::memory_order_relaxed)) {
                cwarn << "Error while waiting for authorization from pool";
                cwarn << "Double check your pool credentials.";
                m_conn->MarkUnrecoverable();
            }
            if ((ec.category() == boost::asio::error::get_ssl_category()) &&
                    (ERR_GET_REASON(ec.value()) == SSL_RECEIVED_SHUTDOWN)
               )
            {
                cnote << "SSL Stream remotely closed by " << m_conn->Host();
            } else if (ec == boost::asio::error::eof) {
                cnote << "Connection remotely closed by " << m_conn->Host();
            } else {
                cwarn << "Socket read failed: " << ec.message();
            }
            m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this)));
        }
    }
}

void StratumClient::sendSocketData(Json::Value const & jReq)
{
    if (!isConnected()) {
        return;
    }
    std::ostream os(&m_sendBuffer);
    os << m_jWriter.write(jReq);		// Do not add lf. It's added by writer.
    if (m_conn->SecLevel() != SecureLevel::NONE) {
        async_write(*m_securesocket, m_sendBuffer,
                m_io_strand.wrap(boost::bind(&StratumClient::onSendSocketDataCompleted, this, boost::asio::placeholders::error)));
    } else {
        async_write(*m_nonsecuresocket, m_sendBuffer,
                m_io_strand.wrap(boost::bind(&StratumClient::onSendSocketDataCompleted, this, boost::asio::placeholders::error)));
    }
}

void StratumClient::onSendSocketDataCompleted(const boost::system::error_code& ec)
{
    if (ec) {
        if ((ec.category() == boost::asio::error::get_ssl_category()) && (SSL_R_PROTOCOL_IS_SHUTDOWN == ERR_GET_REASON(ec.value()))) {
            cnote << "SSL Stream error: " << ec.message();
            m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this));
        }
        if (isConnected()) {
            setThreadName("stratum");
            cwarn << "Socket write failed: " + ec.message();
            m_io_strand.wrap(boost::bind(&StratumClient::disconnect, this));
        }
    }
}
void StratumClient::onSSLShutdownCompleted(const boost::system::error_code& ec)
{
    (void)ec;
    // cnote << "onSSLShutdownCompleted Error code is : " << ec.message();
    m_io_service.post(m_io_strand.wrap(boost::bind(&StratumClient::disconnect_finalize, this)));
}

void StratumClient::enqueue_response_plea()
{
    using namespace std::chrono;
    steady_clock::time_point response_plea_time = steady_clock::now();
    if (m_response_pleas_count++ == 0) {
        m_response_plea_older.store(
                response_plea_time.time_since_epoch(), std::memory_order_relaxed);
    }
    m_response_plea_times.push(response_plea_time);
}

std::chrono::milliseconds StratumClient::dequeue_response_plea()
{
    using namespace std::chrono;
    steady_clock::time_point response_plea_time(m_response_plea_older.load(std::memory_order_relaxed));
    milliseconds response_delay_ms =
        duration_cast<milliseconds>(steady_clock::now() - response_plea_time);
    if (m_response_plea_times.pop(response_plea_time)) {
        m_response_plea_older.store(
                response_plea_time.time_since_epoch(), std::memory_order_relaxed);
    }
    if (m_response_pleas_count.load(std::memory_order_relaxed) > 0) {
        m_response_pleas_count--;
        return response_delay_ms;
    } else {
        return milliseconds(0);
    }
}

void StratumClient::clear_response_pleas()
{
    using namespace std::chrono;
    steady_clock::time_point response_plea_time;
    m_response_pleas_count.store(0, std::memory_order_relaxed);
    while (m_response_plea_times.pop(response_plea_time))
    {
    };
    m_response_plea_older.store(((steady_clock::time_point)steady_clock::now()).time_since_epoch(),
            std::memory_order_relaxed);
}

#include "server.h"
#include "cluster.h"
#include "ssl.h"

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
/* -------------------------- private prototypes ---------------------------- */
// Helper functions for the class
ssl_connection * getSslConnectionForFd(int fd);

ssize_t sslRecv(int fd, void *buffer, size_t nbytes, s2n_blocked_status * blocked);

// Functions for normal SLS negotiations
static SslNegotiationStatus
sslNegotiate(aeEventLoop *el, int fd, void *privdata, aeFileProc *post_handshake_handler,
                int post_handshake_handler_mask, aeFileProc *sourceProcedure, char *sourceProcedureName);

static int updateEventHandlerForSslHandshake(s2n_blocked_status blocked, aeEventLoop *el, int fd, void *privdata,
                                             aeFileProc *sourceProc);

// Functions for SSL configuration
static int initClientSslConfig(ssl_t *ssl);

static int initServerSslConfig(ssl_t *ssl);

static struct s2n_config *
initSslConfigForServer(const char *certificate, const char *privateKey, const char *dhParams,
                       const char *cipherPrefs);

static struct s2n_config *
initSslConfigForClient(const char *cipher_prefs,
                       const char *certificate, const char *rootCACertificatesPath);

static struct s2n_config *
initSslConfig(int is_server, const char *certificate, const char *private_key, const char *dh_params,
              const char *cipher_prefs, const char *rootCACertificatesPath);

uint8_t s2nVerifyHost(const char *hostName, size_t length, void *data);

// Functions for SSL connections
int cleanupSslConnection(struct ssl_connection *conn, int fd, int shutdown);

static int shutdownSslConnection(ssl_connection *conn);

static int freeSslConnection(ssl_connection *conn);

/* Functions used for reading and parsing X509 certificates */
static int getCnameFromCertificate(const char *certificate, char *subject_name);    

int updateServerCertificateInformation(const char *certificate, char *not_before_date, char *not_after_date, long *serial);    

int convertASN1TimeToString(ASN1_TIME *time_pointer, char* output_buffer, size_t length);

X509 *getX509FromCertificate(const char *certificate);

static void updateClientsUsingOldCertificate(void);

/* Functions for handling SSL negotiation after a socket BGSave */ 
void waitForSlaveToLoadRdbAfterRdbTransfer(aeEventLoop *el, int fd, void *privdata, int mask);

static void sslNegotiateWithSlaveAfterSocketRdbTransfer(aeEventLoop *el, int fd, void *privdata, int mask);
 
static void sslNegotiateWithMasterAfterSocketRdbLoad(aeEventLoop *el, int fd, void *privdata, int mask);

static SslNegotiationStatus sslNegotiateWithoutPostHandshakeHandler(aeEventLoop *el, int fd, void *privdata, aeFileProc *sourceProcedure,
    char *sourceProcedureName);

// Functions for processoring repeated reads                       
int processRepeatedReads(struct aeEventLoop *eventLoop, long long id, void *clientData);

void addRepeatedRead(ssl_connection *conn);

void removeRepeatedRead(ssl_connection *conn);

/* -------------------------- public function definitions ---------------------------- */

/**
 * Initializes any global level resource required for SSL. This method
 * should be invoked at startup time
 */
void initSsl(ssl_t *ssl) {
    if (ssl->enable_ssl) {
        serverLog(LL_NOTICE, "Initializing SSL configuration");
        setenv("S2N_ENABLE_CLIENT_MODE", "1", 1);
        // MLOCK is used to keep memory from being moved to SWAP. However, S2N can run into 
        // kernel limits for the number distinct mapped ranges assocciated to a process when
        // a large number of clients are connected. Failed mlock calls will not free memory, 
        // so pages will not get unmapped until the engine is rebooted. In order to avoid this, 
        // we are unconditionally disabling MLOCK.
        setenv("S2N_DONT_MLOCK", "1", 1);
        if (s2n_init() < 0) {
            serverLog(LL_WARNING, "Error running s2n_init(): '%s'. Exiting", s2n_strerror(s2n_errno, "EN"));
            serverAssert(0);
        }
        //initialize openssl error strings
        SSL_load_error_strings();
        //required for cert validation
        OpenSSL_add_all_algorithms();
        
        //initialize configuration for Redis to act as Server (regular mode and cluster bus server)
        if (initServerSslConfig(ssl) == C_ERR) {
            serverLog(LL_WARNING, "Error initializing server SSL configuration. Exiting.");
            serverAssert(0);
        }

        //initialize configuration for Redis to act as client (Replica and cluster bus client)
        if (initClientSslConfig(ssl) == C_ERR) {
            serverLog(LL_WARNING, "Error initializing client SSL configuration. Exiting.");
            serverAssert(0);
        }
        // The expected hostname from the certificate to use as part of hostname validation
        ssl->expected_hostname = zmalloc(CERT_CNAME_MAX_LENGTH);
        if (getCnameFromCertificate(ssl->ssl_certificate, ssl->expected_hostname) == C_ERR) {
            serverLog(LL_WARNING, "Error while discovering expected hostname from certificate file");        
            serverAssert(0);
        }
        
        // Allocate space for not before and not after dates
        ssl->certificate_not_after_date = zmalloc(CERT_DATE_MAX_LENGTH);
        ssl->certificate_not_before_date = zmalloc(CERT_DATE_MAX_LENGTH);

        if (updateServerCertificateInformation(ssl->ssl_certificate, 
                ssl->certificate_not_before_date, ssl->certificate_not_after_date,
                &server.ssl_config.certificate_serial) == C_ERR) {
            serverLog(LL_WARNING, "Error while discovering not_after and not_before from certificate file");        
            serverAssert(0);         
        }
        
        //initialize array to store socked fd to SSL connections mapping
        ssl->fd_to_sslconn_size = server.maxclients + CONFIG_FDSET_INCR;
        ssl->fd_to_sslconn = zcalloc(sizeof(ssl_connection *) * ssl->fd_to_sslconn_size);
        ssl->sslconn_with_cached_data = listCreate();
    }
}

/**
 * Cleans any global level resources used by SSL. This method
 * should be invoked at shutdown time
 */
void cleanupSsl(ssl_t *ssl) {
    if (ssl->enable_ssl) {
        if (s2n_cleanup() < 0)
            serverLog(LL_WARNING, "Error cleaning up SSL resources: %s", s2n_strerror(s2n_errno, "EN"));
        if (s2n_config_free(ssl->server_ssl_config) < 0)
            serverLog(LL_WARNING, "Error freeing server SSL config: %s", s2n_strerror(s2n_errno, "EN"));
        if (s2n_config_free(ssl->client_ssl_config) < 0)
            serverLog(LL_WARNING, "Error freeing client SSL config: %s", s2n_strerror(s2n_errno, "EN"));
        ERR_free_strings();
        //removes all ciphers and digests from internal table of digest algorithms and ciphers
        EVP_cleanup();

        listRelease(ssl->sslconn_with_cached_data);
        zfree(ssl->expected_hostname);
        zfree(ssl->fd_to_sslconn);
        zfree(ssl->certificate_not_after_date);
        zfree(ssl->certificate_not_before_date);
    }
}

/**
 * Converts SSL performance mode string to corresponding integer constant.
 */
int getSslPerformanceModeByName(char *name) {
    if (!strcasecmp(name, "low-latency")) return SSL_PERFORMANCE_MODE_LOW_LATENCY;
    else if (!strcasecmp(name, "high-throughput")) return SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT;
    else return -1;
}

/**
 * Converts SSL performance mode integer to corresponding str
 */
char *getSslPerformanceModeStr(int mode) {
    if (mode == SSL_PERFORMANCE_MODE_LOW_LATENCY) return "low-latency";
    else if (mode == SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT) return "high-throughput";
    else return "invalid input";
}

/**
 * SSL compatible read IO method. It will automatically detect if
 * SSL is enabled or not and accordingly use standard
 * read/write or SSL based IO methods. Special note - It is made
 * inline for performance reasons
 */
inline ssize_t sslRead(int fd, void *buffer, size_t nbytes) {
    if (!server.ssl_config.enable_ssl) {
        return read(fd, buffer, nbytes);
    } else {
        s2n_blocked_status blocked;
        ssize_t bytes_read = sslRecv(fd, buffer, nbytes, &blocked);
        ssl_connection *ssl_conn = server.ssl_config.fd_to_sslconn[fd];
        if (bytes_read > 0 && blocked == S2N_BLOCKED_ON_READ) {
            /* Data was returned, but we didn't consume an entire frame, so signal that we need to repeat the event handler. */
            addRepeatedRead(ssl_conn);
        } else {
            /* Either the entire frame was consumed, or nothing was returned because we were blocked on a socket read. */
            removeRepeatedRead(ssl_conn);
        }

        return bytes_read;
    }
}

/**
 * Send a newline ping on a socket used for other purposes.  This is necessary instead of using sslWrite
 * for a ping when SSL is enabled because S2N assumes that a single stream of data is sent.  If a newline 
 * byte is sent as its own SSL frame, it is no longer atomic, and can be partially sent.  S2N assumes the caller
 * will always retry the call until success, whereas Redis just performs best-effort pings.  Therefore we
 * hijack the sending process and ensure that pings are fully flushed when sent.
 * 
 * While negotiation is in progress sending data here will cause the negotiation to break, so
 * that needs to be handled by the caller.
 */
inline void sslPing(int fd) {
    ssize_t bytes_written = sslWrite(fd, "\n", 1);
    if (server.ssl_config.enable_ssl && bytes_written < 0 && errno == EAGAIN) {
        /* A newline ping request is in progress.  We need to make sure this request succeeds before
            * we issue another independent request. */
        ssl_connection *ssl_conn = getSslConnectionForFd(fd);
        ssl_conn->connection_flags |= NEWLINE_PING_IN_PROGRESS_FLAG;
    }
}

/**
 * SSL compatible write IO method. It will automatically detect if
 * SSL is enabled or not and accordingly used standard
 * read/write or SSL based IO methods. Special note - It is made
 * inline for performance reasons
 */
inline ssize_t sslWrite(int fd, const void *buffer, size_t nbytes) {
    if (!server.ssl_config.enable_ssl) {
        return write(fd, buffer, nbytes);
    } else {
        s2n_errno = S2N_ERR_T_OK;
        errno = 0;

        ssl_connection *ssl_conn = getSslConnectionForFd(fd);
        s2n_blocked_status blocked;

        if (ssl_conn->connection_flags & NEWLINE_PING_IN_PROGRESS_FLAG) {
            /* We previously called sslPing and it didn't fully complete the request!
             * We need to flush out that request before continuing since s2n is stateful. */
            ssize_t r = s2n_send(ssl_conn->s2nconn, "\n", 1, &blocked);
            if (r < 0) {
                // Still didn't succeed

                if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
                    errno = EAGAIN;
                }

                return r;
            }

            // Success! Continue to our actual request.
            ssl_conn->connection_flags &= ~NEWLINE_PING_IN_PROGRESS_FLAG;
        }

        ssize_t r = s2n_send(ssl_conn->s2nconn, buffer, nbytes, &blocked);

        //set errno as well in case IO blocked. This is so that calling code treats
        //it like regular blocking IO and does not has to do any special logic for SSL based IO
        if (r < 0 && s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) errno = EAGAIN;
        return r;
    }
}

/**
 * SSL compatible IO error string method. It will automatically detect if
 * SSL is enabled or not and accordingly use standard strerror method
 * or s2n_strerror for SSL related errors
 */
inline const char *sslstrerror(void) {
    if (!server.ssl_config.enable_ssl) {
        return strerror(errno);
    } else if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_IO) {
        //S2N_ERR_T_IO => underlying I/O operation failed, check system errno
        //therefore in this case, returning System IO error string
        return strerror(errno);
    } else {
        return s2n_strerror(s2n_errno, "EN");
    }
}

/**
 * Initialize default values for SSL related global variables. It should be
 * invoked at Redis startup to provide sane default values to SSL related
 * variables
 */
void initSslConfigDefaults(ssl_t *ssl_config) {
    ssl_config->enable_ssl = SSL_ENABLE_DEFAULT;
    ssl_config->ssl_certificate = NULL;
    ssl_config->ssl_certificate_file = NULL;
    ssl_config->ssl_certificate_private_key = NULL;
    ssl_config->ssl_certificate_private_key_file = NULL;
    ssl_config->ssl_dh_params = NULL;
    ssl_config->ssl_dh_params_file = NULL;
    ssl_config->ssl_cipher_prefs = SSL_CIPHER_PREFS_DEFAULT;
    ssl_config->server_ssl_config = NULL;
    ssl_config->server_ssl_config_creation_time = 0;
    ssl_config->ssl_performance_mode = SSL_PERFORMANCE_MODE_DEFAULT;
    ssl_config->client_ssl_config = NULL;
    ssl_config->fd_to_sslconn = NULL;
    ssl_config->fd_to_sslconn_size = 0;
    ssl_config->server_ssl_config_old = NULL;
    ssl_config->root_ca_certs_path = NULL;
    ssl_config->sslconn_with_cached_data = NULL;
    ssl_config->repeated_reads_task_id = AE_ERR;
    ssl_config->total_repeated_reads = 0;
    ssl_config->max_repeated_read_list_length = 0;
    ssl_config->expected_hostname = NULL;
    ssl_config->certificate_not_after_date = NULL;
    ssl_config->certificate_not_before_date = NULL;
    ssl_config->connections_to_current_certificate = 0;
    ssl_config->connections_to_previous_certificate = 0;
    ssl_config->certificate_serial = 0;
}

/**
 * Performs SSL related setup for a client. It includes creating and initializing an SSL connection,
 * and registering an event handler for SSL negotiation
 */
int setupSslOnClient(client *c, int fd, int ssl_performance_mode, struct ssl_connection **fd_to_sslconn,
                        int fd_to_sslconn_size) {

    struct ssl_connection *ssl_conn = initSslConnection(S2N_SERVER, server.ssl_config.server_ssl_config, fd, ssl_performance_mode, NULL,
        fd_to_sslconn, fd_to_sslconn_size);
    if (!ssl_conn) {
        serverLog(LL_WARNING, "Error getting new s2n connection for client with fd: %d, Error: '%s'", fd,

                  s2n_strerror(s2n_errno, "EN"));
        return C_ERR;
    }

    // Increment the number of connections associated with the latest certificate
    server.ssl_config.connections_to_current_certificate++;
    ssl_conn->connection_flags |= CLIENT_CONNECTION_FLAG;

    if (aeCreateFileEvent(server.el, fd, AE_READABLE | AE_WRITABLE,
                          sslNegotiateWithClient, c) == AE_ERR) {
        cleanupSslConnectionForFd(fd);
        return C_ERR;
    }
    return C_OK;
}

/**
 * Creates and initializes an SSL connection. It performs following critical functions on a connection
 * so that it is usable by Redis
 *  - create a new connection in Server or Client mode
 *  - Associates appropriate configuration with the connection
 *  - Associates appropriate socket file descriptor with the connection
 *  - Set a performance mode on the connection
 *  - Create an entry for Socket FD to SSL connection mapping
 */
ssl_connection *
initSslConnection(s2n_mode connection_mode, struct s2n_config *config, int fd, int ssl_performance_mode,
               char *masterhost, struct ssl_connection **fd_to_sslconn, int fd_to_sslconn_size) {

    ssl_connection *sslconn = zmalloc(sizeof(ssl_connection));
    if (!sslconn) {
        serverLog(LL_WARNING, "Error creating new ssl connection.");
        return NULL;
    }
    sslconn->connection_flags = 0;
    sslconn->fd = fd;
    sslconn->cached_data_node = NULL;

    // create a new connection in Server or Client mode
    sslconn->s2nconn = s2n_connection_new(connection_mode);
    if (!sslconn->s2nconn) {
        serverLog(LL_WARNING, "Error creating new s2n connection. Error: '%s'", s2n_strerror(s2n_errno, "EN"));
        goto error;
    }

    // Associates appropriate configuration with the connection
    if (s2n_connection_set_config(sslconn->s2nconn, config) < 0) {
        serverLog(LL_WARNING, "Error setting configuration on s2n connection. Error: '%s'",
                  s2n_strerror(s2n_errno, "EN"));
        goto error;
    }
    // Associates appropriate socket file descriptor with the connection
    if (s2n_connection_set_fd(sslconn->s2nconn, fd) < 0) {
        serverLog(LL_WARNING, "Error setting socket file descriptor: %d on s2n connection. Error:'%s'", fd,
                  s2n_strerror(s2n_errno, "EN"));
        goto error;
    }
    //disable blinding. Blinding could lead to Redis sleeping upto to 10s which is not desirable in a
    //single threaded application
    if (s2n_connection_set_blinding(sslconn->s2nconn, S2N_SELF_SERVICE_BLINDING) < 0) {
        serverLog(LL_WARNING, "Error setting blinding mode: S2N_SELF_SERVICE_BLINDING on s2n connection. Error:'%s'",
                  s2n_strerror(s2n_errno, "EN"));
        goto error;
    }
    // Set a performance mode on the connection
    switch (ssl_performance_mode) {
        case SSL_PERFORMANCE_MODE_HIGH_THROUGHPUT:
            if (s2n_connection_prefer_throughput(sslconn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error setting performance mode of high throughput on SSL connection");
                goto error;
            }
            break;
        case SSL_PERFORMANCE_MODE_LOW_LATENCY:
            if (s2n_connection_prefer_low_latency(sslconn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error setting performance mode of low latency on SSL connection");
                goto error;
            }
            break;
        default:
            serverLog(LL_DEBUG, "Invalid SSL performance mode: %d", ssl_performance_mode);
            goto error;

    }

    //Set master host on the ssl connection
    if (connection_mode == S2N_CLIENT && masterhost != NULL && s2n_set_server_name(sslconn->s2nconn, masterhost) < 0) {
        serverLog(LL_WARNING, "Error setting server name on s2n connection: '%s'", s2n_strerror(s2n_errno, "EN"));
        goto error;
    }

    // Create an entry for Socket FD to SSL connection mapping
    serverAssert(fd < fd_to_sslconn_size);
    fd_to_sslconn[fd] = sslconn;
    serverLog(LL_DEBUG, "SSL Connection setup successfully for fd %d", fd);
    return sslconn;

error:
    freeSslConnection(sslconn);
    return NULL;
}

/**
 * This method should be used for Cleanup a connection. It shuts down the
 * SSL connection (sends a SHUTDOWN TLS alert) for secure shutdown, frees
 * the memory consumed by connection and deletes the mapping from Socket FD
 * to this connection
 */
int cleanupSslConnectionForFd(int fd) {
    return cleanupSslConnection(getSslConnectionForFd(fd), fd, 1);
}


/**
 * This method should be used for cleaning up an SSL connection when shutdown
 * is not desired. This is currently used when re-negotiating an existing connection
 * so there are no race conditions with ssl alerts and negotiating.
 */
int cleanupSslConnectionForFdWithoutShutdown(int fd) {
    return cleanupSslConnection(getSslConnectionForFd(fd), fd, 0);
}


/**
 *  SSL negotiate with a regular Redis client which wants to run commands
 *  on Redis
 */
void sslNegotiateWithClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    client *c = (client *) privdata;

    if (sslNegotiate(el, fd, privdata, readQueryFromClient, AE_READABLE, sslNegotiateWithClient,
                        "sslNegotiateWithClient") == NEGOTIATE_FAILED) {
        freeClient(c);
    }
}

/**
 * SSL negotiate (acting as server) with another cluster node over cluster bus
 */
void sslNegotiateWithClusterNodeAsServer(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    clusterLink *link = (clusterLink *) privdata;
    if (sslNegotiate(el, fd, privdata, clusterReadHandler, AE_READABLE, sslNegotiateWithClusterNodeAsServer,
                        "sslNegotiateWithClusterNodeAsServer") == NEGOTIATE_FAILED) {
        freeClusterLink(link);
    }
}

/**
 * Do an SSL negotiation with another cluster node (acting as client
 * and that cluster node is acting as server)
 */
void sslNegotiateWithClusterNodeAsClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);
    /* Check for errors in the socket. This is because the invoking code does a non-blocking connect
     * and therefore we must check for socket errors before initiating an SSL handshake */
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    if (sockerr) {
        serverLog(LL_WARNING, "Error condition on socket for sslNegotiateWithClusterNodeAsClient: %s",
                  strerror(sockerr));
        //no point in doing SSL handshake if there are socket errors
        aeDeleteFileEvent(el, fd, AE_READABLE | AE_WRITABLE);
        return;
    }

    clusterLink *link = (clusterLink *) privdata;
    if (sslNegotiate(el, fd, privdata, clusterReadHandler, AE_READABLE, sslNegotiateWithClusterNodeAsClient,
                        "sslNegotiateWithClusterNodeAsClient") == NEGOTIATE_DONE) {
        clusterClientSetup(link);
    }
    return;
}

/**
 * Perform SSL negotiation with replication master
 */
void sslNegotiateWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);

    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);
    /* Check for errors in the socket. */
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    if (sockerr) {
        serverLog(LL_WARNING, "Error condition on socket for SYNC: %s",
                  strerror(sockerr));
        goto error;
    }

    SslNegotiationStatus ssl_negotiation_status = sslNegotiate(el, fd, privdata, syncWithMaster,
                                                                  AE_READABLE | AE_WRITABLE, sslNegotiateWithMaster,
                                                                  "sslNegotiateWithMaster");

    switch (ssl_negotiation_status) {
        case NEGOTIATE_FAILED:
            goto error;
        case NEGOTIATE_RETRY:
            return;
        case NEGOTIATE_DONE:
            server.repl_transfer_lastio = server.unixtime;
            server.repl_state = REPL_STATE_CONNECTING;
            break;
        default:
            serverAssert(0);
    }
    return;

error:
    cleanupSslConnectionForFd(fd);
    close(fd);
    aeDeleteFileEvent(el, fd, AE_WRITABLE | AE_READABLE);
    server.repl_state = REPL_STATE_CONNECT;
    return;
}

/**
 * Helper method for SSL negotiation that doesn't involve the event loop, and should block until it
 * has returned. The timeout applies to each individual call, so this call can take a while to 
 * return if the network is slow. Returns C_OK on success and C_ERR on failure.
 */ 
int syncSslNegotiateForFd(int fd, long timeout) {
    ssl_connection *ssl_conn = getSslConnectionForFd(fd);
    while (1) {
        s2n_blocked_status blocked;
        serverLog(LL_DEBUG, "Starting synchronous ssl negotiation.");   
        if (s2n_negotiate(ssl_conn->s2nconn, &blocked) < 0) {
            switch (blocked) {
                case S2N_BLOCKED_ON_READ:
                    serverLog(LL_DEBUG, "Synchronous SSL negotiation blocked on read.");
                    if (!(aeWait(fd, AE_READABLE, timeout) & AE_READABLE)) {
                        serverLog(LL_DEBUG, "Synchronous SSL negotiation timed out waiting for fd to become readable.");
                        return C_ERR;
                    }
                    continue;
                case S2N_BLOCKED_ON_WRITE:
                    serverLog(LL_DEBUG, "Synchronous SSL negotiation blocked on write");
                    if (!(aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE)) {
                        serverLog(LL_DEBUG, "Synchronous SSL negotiation timed out waiting for fd to become writable.");
                        return C_ERR;
                    }
                    continue;
                default:
                    serverLog(LL_WARNING, "Synchronous SSL negotiation unsuccessful due to Error: %s: %s",
                                s2n_strerror(s2n_errno, "EN"), strerror(errno));
                    return C_ERR;
            }
        }
        break;
    }

    //if we are here, it means SSL negotiation is complete and successful
    serverLog(LL_DEBUG, "Synchronous SSL negotiation done successfully with cipher: %s", s2n_connection_get_cipher(ssl_conn->s2nconn));
    return C_OK;
}

/**
 * If SSL is enabled, master and slave need to do SSL handshake
 * again. The reason being that forked bgsave'ed
 * child process has sent data to the slave via forked SSL
 * connection and hence master's SSL connection has gone stale.
 * It needs to reinitialize the SSL connection and do handshake
 * with slave again.
 */
void startSslNegotiateWithSlaveAfterRdbTransfer(struct client *slave) {
    serverLog(LL_DEBUG, "Reinitializing SSL connection for slave with id: %" PRIu64 " socket fd: %d",
              slave->id, slave->fd);
    if (cleanupSslConnectionForFdWithoutShutdown(slave->fd) != C_OK) {
        goto error;
    }
    if (initSslConnection(S2N_SERVER, server.ssl_config.server_ssl_config, slave->fd,
            server.ssl_config.ssl_performance_mode, NULL,
            server.ssl_config.fd_to_sslconn, server.ssl_config.fd_to_sslconn_size) == NULL) {
        goto error;          
    }
    aeDeleteFileEvent(server.el, slave->fd, AE_READABLE | AE_WRITABLE);
    
    if (aeCreateFileEvent(server.el, slave->fd, AE_READABLE | AE_WRITABLE,
                        sslNegotiateWithSlaveAfterSocketRdbTransfer,
                        slave) != AE_OK) {
        goto error;                   
    }
    return;
    
error:
    serverLog(LL_WARNING, "Error reinitializing SSL connection for slave with id: %" PRIu64 " socket fd: %d after rdb transfer: '%s'. Disconnecting slave",
        slave->id, slave->fd, s2n_strerror(s2n_errno, "EN"));
    freeClient(slave);
    return;
}
 
/**
 * If SSL is enabled, master and slave need to do SSL handshake
 * again. This is required because a bgsave'ed
 * child process has sent data to the slave via forked SSL
 * connection and the master's SSL connection has gone stale.
 * It needs to reinitialize the SSL connection and do handshake
 * with slave again.
 */
void startSslNegotiateWithMasterAfterRdbLoad(int fd) {
    serverLog(LL_DEBUG, "Reinitializing SSL connection with master on fd: %d after sync", fd);
    
    // The first task is to send the completion byte, so make sure fd is writeable
    aeDeleteFileEvent(server.el, fd, AE_READABLE | AE_WRITABLE);    
    if (aeCreateFileEvent(server.el, fd, AE_WRITABLE, sslNegotiateWithMasterAfterSocketRdbLoad,
            NULL) == AE_ERR) {
        goto error;
    }
    return;
 
error:
    serverLog(LL_WARNING, "Error reinitializing master SSL connection on fd %d after rdb exchange: '%s'",
        fd, s2n_strerror(s2n_errno, "EN"));
    cancelReplicationHandshake();
    return;
}


/**
 * Wait for the slave to finish reading in all of the data, before proceeding to the ssl negotiation.
 * The master needs to maintain the SSL connection to continue reading in the pings sent by the replica
 * to keep the connection healthy. 
 * 
 * Although this is executed on the parent process after the child has been killed, there won't be SSL issues
 * related to state since s2n is full duplex IO, so it has separate states for writing and reading. The child
 * thread never read any data from the replica, so it didn't break that state. We do need to negotiate at the end
 * to fix the write state.
 **/
void startWaitForSlaveToLoadRdbAfterRdbTransfer(struct client *slave) {
    aeDeleteFileEvent(server.el, slave->fd, AE_READABLE | AE_WRITABLE);    
    if (aeCreateFileEvent(server.el, slave->fd, AE_READABLE, waitForSlaveToLoadRdbAfterRdbTransfer,
            slave) == AE_ERR) {
        freeClient(slave);
    }
    return;
}

/**
 * If SSL is enabled, and slave is waiting for bgsave
 * to finish, then delete the read event handler for this
 * slave. The reason being that in case of TLS, both master
 * and slave will do SSL handshake again after exchanging
 * the rdb file. If slave initiates the handshake before
 * master, then master's read command handler (readQueryFromClient)
 * will get invoked which will cause handshake to fail.
 * To prevent that, we delete the read file handler and
 * add it back after SSL handshake is done
 */
void deleteReadEventHandlerForSlavesWaitingBgsave() {
    if (server.ssl_config.enable_ssl) {
        listIter li;
        listNode *ln;
        listRewind(server.slaves, &li);
        while ((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
                serverLog(LL_DEBUG, "Deleting read handler for slave with id: %" PRIu64 " socket fd: %d",
                          slave->id,
                          slave->fd);
                aeDeleteFileEvent(server.el, slave->fd, AE_READABLE);
            }
        }
    }
}

/**
 * Returns true if fd_to_ssl_conn can be resized from cur_size to new_size
 */
int isResizeAllowed(ssl_connection **fd_to_ssl_conn, int cur_size, int new_size) {
    int max_fd = -1;
    for (int i = cur_size - 1; i >= 0; i--) {
        if (fd_to_ssl_conn[i] != NULL) {
            max_fd = i;
            break;
        }
    }
    return max_fd < new_size;
}

/* Resize the maximum size of the fd_to_ssl_conn.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, C_ERR is returned and the operation is not
 * performed at all.
 * 
 * Otherwise C_OK is returned and the operation is successful. */
int resizeFdToSslConnSize(ssl_t *ssl, unsigned int setsize) {
    
    if (setsize == ssl->fd_to_sslconn_size) {
        return C_OK;
    } 
    
    if (!isResizeAllowed(ssl->fd_to_sslconn, ssl->fd_to_sslconn_size, setsize)) {
        return C_ERR;
    } 

    zrealloc(ssl->fd_to_sslconn, sizeof(ssl_connection *) * setsize);
    return AE_OK;
}

/**
 * Update the certificate/private key pair used by SSL. This method can be used to
 * renew the expiring certificate without bouncing Redis
 */
int renewCertificate(char *new_certificate, char *new_private_key, char *new_certificate_filename,
                     char *new_private_key_filename) {
    serverLog(LL_NOTICE, "Initializing SSL configuration for new certificate");
    struct s2n_config *new_config = initSslConfigForServer(new_certificate, new_private_key,
                                                           server.ssl_config.ssl_dh_params,
                                                           server.ssl_config.ssl_cipher_prefs);
    if (new_config == NULL) {
        serverLog(LL_DEBUG, "Error creating SSL configuration using new certificate");
        return C_ERR;
    }

    char *new_not_before_date = zmalloc(CERT_DATE_MAX_LENGTH);    
    char *new_not_after_date = zmalloc(CERT_DATE_MAX_LENGTH);
    long new_serial = 0;

    // Update the not before and not after date provided in info
    if (updateServerCertificateInformation(new_certificate, new_not_before_date, new_not_after_date, &new_serial) != C_OK) {
        serverLog(LL_DEBUG, "Failed to read not_before and not_after date from new certificate");
        zfree(new_not_before_date);
        zfree(new_not_after_date);
        return C_ERR;
    }

    //After we have validated that new cert is valid, disconnect any
    //clients using the oldest certificate. We don't want to have more that
    // 2 certificates in use at a time. We proactively disconnect any
    //clients using oldest certificate to stay within 2 certificate limit
    updateClientsUsingOldCertificate();

    //save the SSL configuration for the expiring certificate
    //We gotta keep it as there are existing connections using this configuration
    server.ssl_config.server_ssl_config_old = server.ssl_config.server_ssl_config;

    //start using new configuration. Any new connections
    //will start using new certificate from this point onwards
    server.ssl_config.server_ssl_config = new_config;
    server.ssl_config.server_ssl_config_creation_time = time(NULL);

    //free the memory used by old stuff
    zfree((void *) server.ssl_config.ssl_certificate);
    zfree((void *) server.ssl_config.ssl_certificate_file);
    zfree((void *) server.ssl_config.ssl_certificate_private_key);
    zfree((void *) server.ssl_config.ssl_certificate_private_key_file);
    zfree((void *) server.ssl_config.certificate_not_before_date);    
    zfree((void *) server.ssl_config.certificate_not_after_date);

    //save the references to the new stuff
    server.ssl_config.ssl_certificate = new_certificate;
    server.ssl_config.ssl_certificate_file = new_certificate_filename;

    server.ssl_config.ssl_certificate_private_key = new_private_key;
    server.ssl_config.ssl_certificate_private_key_file = new_private_key_filename;

    server.ssl_config.certificate_not_before_date = new_not_before_date;
    server.ssl_config.certificate_not_after_date = new_not_after_date;
    server.ssl_config.certificate_serial = new_serial;
    
    // Update the connection count for redis info
    server.ssl_config.connections_to_previous_certificate = server.ssl_config.connections_to_current_certificate;
    server.ssl_config.connections_to_current_certificate = 0;

    serverLog(LL_NOTICE, "Successfully renewed SSL certificate");

    return C_OK;
}

/* ------------------------- private functions ------------------------------ */
/* Convenience function to fetch an sslConnection from a fd and make sure it exists */
ssl_connection * getSslConnectionForFd(int fd) {
    serverAssert((unsigned int)fd < server.ssl_config.fd_to_sslconn_size);
    serverAssert(server.ssl_config.fd_to_sslconn[fd] != NULL);
    return server.ssl_config.fd_to_sslconn[fd];
}

/*
 * SSL compatible wrapper around recv that is used as an abstraction for sslRead.
 */
ssize_t sslRecv(int fd, void *buffer, size_t nbytes, s2n_blocked_status * blocked) {
    s2n_errno = S2N_ERR_T_OK;
    errno = 0;

    serverAssert((unsigned int)fd < server.ssl_config.fd_to_sslconn_size);
    ssl_connection *ssl_conn = server.ssl_config.fd_to_sslconn[fd];
    serverAssert(ssl_conn != NULL);
    ssize_t bytes_read = s2n_recv(ssl_conn->s2nconn, buffer, nbytes, blocked);

    if (bytes_read < 0 && s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
        /* No data was returned because the socket did not have a full frame.  We can only continue when the socket is readable again. */

        //set errno as well in case IO blocked. This is so that calling code treats
        //it like regular blocking IO and does not has to do any special logic for SSL based IO
        errno = EAGAIN;
    }

    return bytes_read;
}

/**
 * Perform the same verification as open source s2n uses except don't use the connection name
 * since it doesn't have the right endpoint in some cases for cluster bus.
 */
uint8_t s2nVerifyHost(const char *hostName, size_t length, void *data) {
    UNUSED(data);
    /* if present, match server_name of the connection using rules
     * outlined in RFC6125 6.4. */

    if (server.ssl_config.expected_hostname == NULL) {
        return 0;
    }

    /* complete match */
    if (strlen(server.ssl_config.expected_hostname) == length &&
            strncasecmp(server.ssl_config.expected_hostname, hostName, length) == 0) {
        return 1;
    }

    /* match 1 level of wildcard */
    if (length > 2 && hostName[0] == '*' && hostName[1] == '.') {
        const char *suffix = strchr(server.ssl_config.expected_hostname, '.');

        if (suffix == NULL) {
            return 0;
        }

        if (strlen(suffix) == length - 1 &&
                strncasecmp(suffix, hostName + 1, length - 1) == 0) {
            return 1;
        }
    }

    return 0;
}

/**
 * Return an x509 object from a certificate string.
 */
X509 *getX509FromCertificate(const char *certificate) {
    BIO *bio = NULL;
    // Create a read-only BIO backed by the supplied memory buffer
    bio = BIO_new_mem_buf((void *) certificate, -1);
    
    if (!bio) {
        serverLog(LL_WARNING, "Error allocating BIO buffer");
        return NULL;
    }
    
    X509 *x509_cert = NULL;
    //Read a certificate in PEM format from a BIO
    if (!(x509_cert = PEM_read_bio_X509(bio, NULL, NULL, NULL))) {
        BIO_free(bio);
        serverLog(LL_DEBUG, "Error converting certificate from PEM to X509 format");
        return NULL;
    }
    
    // Cleanup. bio is no longer needed
    BIO_free(bio);
    return x509_cert;
}

/**
 * Extract the Cname from a certificate to be used later in hostname validation. We need this
 * because we want to verify the hostname we are connecting to even when we are using the IP address.
 */
static int getCnameFromCertificate(const char *certificate, char *subject_name) {    
    X509 *x509_cert = getX509FromCertificate(certificate);
    if (x509_cert == NULL) {
        return C_ERR;
    }
    
    if (X509_NAME_get_text_by_NID(X509_get_subject_name(x509_cert), NID_commonName, subject_name,
                                  CERT_CNAME_MAX_LENGTH) == -1) {
        X509_free(x509_cert);
        serverLog(LL_DEBUG, "Could not find a CN entry in certificate");
        return C_ERR;
    }
    X509_free(x509_cert);
    serverLog(LL_DEBUG, "Successfully extracted subject name from certificate. Subject Name: %s", subject_name);
    return C_OK;
}

/**
 * Convert an ANSI string to a C String and write it to the output buffer.
 */
int convertASN1TimeToString(ASN1_TIME *time_pointer, char* output_buffer, size_t length) {
    BIO *buffer = BIO_new(BIO_s_mem());
    if (ASN1_TIME_print(buffer, time_pointer) <= 0) {
        BIO_free(buffer);
        return C_ERR;
    }
    if (BIO_gets(buffer, output_buffer, length) <= 0) {
        BIO_free(buffer);
        return C_ERR;
    }
    BIO_free(buffer);
    return C_OK;
}

/**
 * Read the provided certificate file and populate the not_after and not_before dates. The values returned
 * are not guaranteed to be right unless C_OK is returned.
 */
int updateServerCertificateInformation(const char *certificate, char *not_before_date, char *not_after_date, long *serial) {
    X509 *x509_cert = getX509FromCertificate(certificate);
    if (x509_cert == NULL) {
        return C_ERR;
    }
    
    if (convertASN1TimeToString(X509_get_notBefore(x509_cert), not_before_date, CERT_DATE_MAX_LENGTH) == -1) {
        serverLog(LL_DEBUG, "Failed to extract not before date from certificate.");
        X509_free(x509_cert);
        return C_ERR;
    }
    serverLog(LL_DEBUG, "Successfully extracted not before date: %s from certificate.", not_before_date);
    
    if (convertASN1TimeToString(X509_get_notAfter(x509_cert), not_after_date, CERT_DATE_MAX_LENGTH) == -1) {
        serverLog(LL_DEBUG, "Failed to extract not after date from provided certificate.");
        X509_free(x509_cert);
        return C_ERR;
    }
    
    serverLog(LL_DEBUG, "Successfully extracted not after date: %s from certificate.", not_after_date);    
    long new_serial = ASN1_INTEGER_get(X509_get_serialNumber(x509_cert));
    if (new_serial == 0) {
        serverLog(LL_DEBUG, "Failed to extract not before date from provided certificate.");
        X509_free(x509_cert);
        return C_ERR;
    }
    *serial = new_serial;
    serverLog(LL_DEBUG, "Successfully extracted serial: %lx from certificate.", new_serial);
    X509_free(x509_cert);
    return C_OK;
}

/**
 * This method should be used to cleanup a connection. It will shutdown the
 * SSL connection (sends a SHUTDOWN TLS alert) for secure shutdown, free
 * the memory consumed by connection and delete the mapping from Socket FD
 * to this connection
 */
int cleanupSslConnection(struct ssl_connection *conn, int fd, int shutdown) {
    serverLog(LL_DEBUG, "Cleaning up SSL conn for socket fd: %d", fd);
    if (conn->connection_flags & CLIENT_CONNECTION_FLAG) {
        if (conn->connection_flags & OLD_CERTIFICATE_FLAG) {
            server.ssl_config.connections_to_previous_certificate--;
        } else {
            server.ssl_config.connections_to_current_certificate--;
        }
    }
    
    // Don't shutdown if we haven't even initialized anything
    if (shutdown && s2n_connection_get_client_hello(conn->s2nconn) != NULL) {
        shutdownSslConnection(conn);                        
    }
    freeSslConnection(conn);
    serverLog(LL_DEBUG, "Deleting fd: %d from fd_to_sslconn map", fd);
    serverAssert((unsigned int)fd < server.ssl_config.fd_to_sslconn_size);
    server.ssl_config.fd_to_sslconn[fd] = NULL;
    return C_OK;
}

void waitForSlaveToLoadRdbAfterRdbTransfer(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    client *slave = (client *) privdata;

    serverLog(LL_DEBUG, "Checking if slave on fd: %d is done loading RDB file", fd);

    char buffer[1];
    ssize_t bytes_read = sslRead(fd, buffer, 1);
    if (bytes_read <= 0) {
        if (errno == EAGAIN) {
            // No data was received, but the connection blocked so wait for handler to get called again   
            return;
        }else{
            // We have received some other failure that we can't recover from
            serverLog(LL_DEBUG, "Encountered an error while waiting for slave to load RDB file: %s : %s.",
                s2n_strerror(s2n_errno, "EN"), s2n_strerror_debug(s2n_errno, "EN"));
            freeClient(slave);
            return;          
        }
    } else if (bytes_read == 1) {
        slave->repl_ack_time = server.unixtime;
        if (buffer[0] == '+') {
            // Received the completion character
            startSslNegotiateWithSlaveAfterRdbTransfer(slave);
            return;
        } else if (buffer[0] == '\n') {
            // Just a ping, so return since we already updated ack time
            return;
        }else{
            // We have received an unrecognized character
            serverLog(LL_WARNING, "Received an unexpected character while waiting for slave to finish loading RDB");
            freeClient(slave);
            return;       
        }
    }
    return;
}


/**
 * Perform a SSL handshake with slave after streaming rdb file directly to slave sockets
 */
void sslNegotiateWithSlaveAfterSocketRdbTransfer(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    client *slave = (client *) privdata;
 
    SslNegotiationStatus ssl_negotiation_status =
            sslNegotiate(el, fd, privdata, NULL, AE_NONE, sslNegotiateWithSlaveAfterSocketRdbTransfer,
                            "sslNegotiateWithSlaveAfterSocketRdbTransfer");
 
    switch (ssl_negotiation_status) {
        case NEGOTIATE_FAILED:
            serverLog(LL_WARNING,
                      "SSL negotiation with slave after socket based rdb transfer failed. Disconnecting slave");
            freeClient(slave);
            break;
        case NEGOTIATE_RETRY:
            slave->repl_ack_time = server.unixtime;
            break;
        case NEGOTIATE_DONE:
            if (aeCreateFileEvent(server.el, fd, AE_READABLE, readQueryFromClient, slave) == AE_ERR) {
                    freeClient(slave);
                return;          
            }
            serverLog(LL_NOTICE,
                "Streamed RDB transfer and ssl renegotiation with slave %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                replicationGetSlaveName(slave));
            break;
        default:
            serverAssert(0);
    }
    return;
}
 
/**
 * Perform a SSL handshake with master after receiving the rdb file for sync from master
 */
void sslNegotiateWithMasterAfterSocketRdbLoad(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    UNUSED(privdata);
 
    // Send character to master to indicate it's ready to start negotiating
    ssl_connection *ssl_conn = getSslConnectionForFd(fd);
    if (!(ssl_conn->connection_flags & LOAD_NOTIFICATION_SENT_FLAG)) {
        ssize_t bytes_written = sslWrite(fd, "+", 1);
        if (bytes_written <= 0) {
            if (errno == EAGAIN) {
                return;
            }else{
                // Something went wrong, so cancel the handshake
                serverLog(LL_WARNING, "Failed to write load completion character to master node.");
                cancelReplicationHandshake();
                return;
            }
        }

        // We wrote at least one byte, which is all we were attempting to write so continue
        if (cleanupSslConnectionForFdWithoutShutdown(fd) != C_OK) {
            cancelReplicationHandshake();
            return;
        }
        
        if (initSslConnection(S2N_CLIENT, server.ssl_config.client_ssl_config, fd,
                server.ssl_config.ssl_performance_mode, server.masterhost,
                server.ssl_config.fd_to_sslconn, server.ssl_config.fd_to_sslconn_size) == NULL) {
            cancelReplicationHandshake();
            return;
        }
    
        /* Get the ssl_conn again as we have cleaned up old one */
        ssl_conn = getSslConnectionForFd(fd);
        serverLog(LL_DEBUG, "Sent load completion character to master node and cleaned up old ssl connection.");        
        ssl_conn->connection_flags |= LOAD_NOTIFICATION_SENT_FLAG;
    }

    SslNegotiationStatus ssl_negotiation_status = sslNegotiateWithoutPostHandshakeHandler(el, fd, privdata,
                                                                                             sslNegotiateWithMasterAfterSocketRdbLoad,
                                                                                             "sslNegotiateWithMasterAfterSocketRdbLoad"); 
    switch (ssl_negotiation_status) {
        case NEGOTIATE_FAILED:
            serverLog(LL_WARNING, "SSL negotiation with master after socket rdb transfer failed. Disconnecting master");
            cancelReplicationHandshake();
            break;
        case NEGOTIATE_RETRY:
            // Update the last repl transfer time time, since we either received or wrote data
            server.repl_transfer_lastio = server.unixtime;
            break;
        case NEGOTIATE_DONE:
            serverLog(LL_DEBUG,
                "SSL renegotiation with master is complete.");
            finishSyncAfterReceivingBulkPayloadOnSlave();
            break;
        default:
            serverAssert(0);
    }
    return;
}


/**
 * Helper method to see where SSL negotiation is blocked on read or write and register
 * to listen on file descriptor accordingly
 */
int updateEventHandlerForSslHandshake(s2n_blocked_status blocked, aeEventLoop *el, int fd, void *privdata,
                                      aeFileProc *source_proc) {
    int delete_event;
    int listen_event;
    switch (blocked) {
        case S2N_BLOCKED_ON_READ:
            delete_event = AE_WRITABLE;
            listen_event = AE_READABLE;
            break;
        case S2N_BLOCKED_ON_WRITE:
            delete_event = AE_READABLE;
            listen_event = AE_WRITABLE;
            break;
        default:
            return C_OK;

    }
    aeDeleteFileEvent(el, fd, delete_event);
    if (aeGetFileEvents(el, fd) == AE_NONE) {
        if (aeCreateFileEvent(el, fd, listen_event, source_proc, privdata) == AE_ERR) {
            return C_ERR;
        }
    }
    return C_OK;
}

/**
 * Helper method for SSL negotiation. This is a generic method which abstracts the logic
 * of SSL negotiation so that it can be reusable by all places where SSL negotiation needs to
 * happen. Invoker just needs to handle error conditions - NEGOTIATE_FAILED and in case of success,
 * NEGOTIATE_DONE perform any post negotiation handling
 */
SslNegotiationStatus
sslNegotiate(aeEventLoop *el, int fd, void *privdata, aeFileProc *post_handshake_handler,
                int post_handshake_handler_mask, aeFileProc *sourceProcedure, char *sourceProcedureName) {
    ssl_connection *ssl_conn = getSslConnectionForFd(fd);
        
    serverLog(LL_DEBUG, "resuming SSL negotiation from %s", sourceProcedureName);
    s2n_blocked_status blocked;
    if (s2n_negotiate(ssl_conn->s2nconn, &blocked) < 0) {
        if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
            /* Blocked, come back later */
            serverLog(LL_DEBUG, "SSL Negotiation is blocked on IO: %s : %s : %d. Will resume soon",
                        s2n_strerror(s2n_errno, "EN"), s2n_strerror_debug(s2n_errno, "EN"), blocked);
            return updateEventHandlerForSslHandshake(blocked, el, fd, privdata, sourceProcedure) == C_OK
                    ? NEGOTIATE_RETRY
                    : NEGOTIATE_FAILED;
        } else {
             /* Everything else */
            serverLog(LL_WARNING, "SSL Negotiation unsuccessful due to Error: %s: %s : %s",
                        s2n_strerror(s2n_errno, "EN"), s2n_strerror_debug(s2n_errno, "EN"), strerror(errno));
            serverLog(LL_DEBUG, "Deleting SSL negotiation event handler to stop further invocations");
                
            //stop further invocations of this method
            aeDeleteFileEvent(el, fd, AE_READABLE | AE_WRITABLE);
            return NEGOTIATE_FAILED;
        }
    }
    //if we are here, it means SSL negotiation is complete and successful
    serverLog(LL_DEBUG, "negotiation done successfully with cipher: %s", s2n_connection_get_cipher(ssl_conn->s2nconn));
    serverLog(LL_DEBUG, "Installing an event handler for processing commands");
    aeDeleteFileEvent(el, fd, AE_READABLE | AE_WRITABLE);
    if (post_handshake_handler != NULL && aeCreateFileEvent(el, fd, post_handshake_handler_mask,
            post_handshake_handler, privdata) == AE_ERR) {
        return NEGOTIATE_FAILED;
    }
    return NEGOTIATE_DONE;
}


/* Perform ssl negotiation without a callback once ssl is completed */
SslNegotiationStatus
sslNegotiateWithoutPostHandshakeHandler(aeEventLoop *el, int fd, void *privdata, aeFileProc *sourceProcedure,
                                           char *sourceProcedureName) {
    return sslNegotiate(el, fd, privdata, NULL, AE_NONE, sourceProcedure, sourceProcedureName);
}

/*
 * Initialize SSL configuration to act as client
 * (e.g. replication client, cluster bus client)
 */
int initClientSslConfig(ssl_t *ssl) {
    if (ssl->enable_ssl && ssl->client_ssl_config == NULL) {

        ssl->client_ssl_config = initSslConfigForClient(ssl->ssl_cipher_prefs,
                                                           ssl->ssl_certificate,
                                                           ssl->root_ca_certs_path);

        if (!ssl->client_ssl_config) {
            serverLog(LL_WARNING, "Error initializing client SSL configuration");
            return C_ERR;
        }
    }
    return C_OK;
}

/**
 * Initializes SSL configuration to act as server
 * (e.g. replication master, cluster bus master, query processor server)
 */
int initServerSslConfig(ssl_t *ssl) {
    if (ssl->enable_ssl && ssl->server_ssl_config == NULL) {
        ssl->server_ssl_config = initSslConfigForServer(ssl->ssl_certificate, ssl->ssl_certificate_private_key,
                                                           ssl->ssl_dh_params, ssl->ssl_cipher_prefs);
        if (!ssl->server_ssl_config) {
            serverLog(LL_WARNING, "Error initializing server SSL configuration");
            return C_ERR;
        }
        ssl->server_ssl_config_creation_time = time(NULL);
    }
    return C_OK;
}

/**
 * shuts down the SSL connection. It effectively sends
 * a SHUTDOWN tls alert to the peer (as a SSL best practice before
 * we close socket)
 */
int shutdownSslConnection(ssl_connection *conn) {
    serverLog(LL_DEBUG, "Shutting down SSL conn");
    if (conn != NULL && conn->s2nconn != NULL) {
        s2n_blocked_status blocked;
        s2n_shutdown(conn->s2nconn, &blocked);
    }
    return C_OK;
}

/**
 * Frees the memory used by ssl connection.  Returns C_ERR
 * if the underlying S2N connection could not be freed successfully
 * but always frees application memory.
 */
int freeSslConnection(ssl_connection *conn) {
    serverLog(LL_DEBUG, "Freeing up SSL conn");
    int ret = C_OK;
    if (conn != NULL) {
        if (conn->s2nconn != NULL) {
            /*
             * Just doing s2n_connection_free is not sufficient in production.
             * s2n_connection_wipe calls s2n_connection_wipe_io which frees
             * some memory allocated. Just doing s2n_connection_free
             * was causing a memory leak reported by valgrind and after a while, redis
             * would stop accepting new connections
             */            
            if (s2n_connection_wipe(conn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error wiping connection: '%s'", s2n_strerror(s2n_errno, "EN"));
            }

            if (s2n_connection_free(conn->s2nconn) < 0) {
                serverLog(LL_WARNING, "Error freeing connection: '%s'", s2n_strerror(s2n_errno, "EN"));
                ret = C_ERR;
            }
        }
        if (conn->cached_data_node != NULL) {
            removeRepeatedRead(conn);
        }
        zfree(conn);
    }
    return ret;
}

static struct s2n_config *
initSslConfigForServer(const char *certificate, const char *privateKey, const char *dhParams,
                       const char *cipherPrefs) {
    return initSslConfig(1, certificate, privateKey, dhParams, cipherPrefs, NULL);
}

static struct s2n_config *
initSslConfigForClient(const char *cipher_prefs,
                       const char *certificate, const char *rootCACertificatesPath) {
    return initSslConfig(0, certificate, NULL, NULL, cipher_prefs, rootCACertificatesPath);
}

static struct s2n_config *
initSslConfig(int is_server, const char *certificate, const char *private_key, const char *dh_params,
              const char *cipher_prefs, const char *rootCACertificatesPath) {
    serverLog(LL_DEBUG, "Initializing %s SSL configuration", is_server ? "Server" : "Client");
    struct s2n_config *ssl_config = s2n_config_new();
    if (!ssl_config) {
        serverLog(LL_WARNING, "Error getting new s2n config: '%s'.", s2n_strerror(s2n_errno, "EN"));
        return NULL;
    }

    if (is_server && s2n_config_add_cert_chain_and_key(ssl_config, certificate,
                                                               private_key) < 0) {
        serverLog(LL_WARNING, "Error adding certificate/key to s2n config: '%s'.",
                  s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    if (is_server && s2n_config_add_dhparams(ssl_config, dh_params) < 0) {
        serverLog(LL_WARNING, "Error adding DH parameters to s2n config: '%s'.",
                  s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    /* Load the root ca certificate */
    if (!is_server && 
            s2n_config_set_verification_ca_location(ssl_config, NULL, rootCACertificatesPath) < 0) {
        serverLog(LL_WARNING, "Error while loading CA certificates into s2n: '%s'.", s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    /** 
     * Load the intermediate nodes from the provided certificate file, this will also load the leaf nodes
     * but they will be unused.
    */
    if (!is_server && s2n_config_add_pem_to_trust_store(ssl_config, certificate) < 0) {
        serverLog(LL_WARNING, "Error while loading SSL certificate into s2n: '%s'.", s2n_strerror(s2n_errno, "EN"));
        goto config_error;        
    }
    
    if (!is_server && s2n_config_set_verify_host_callback(ssl_config, s2nVerifyHost, NULL) < 0) {
        serverLog(LL_WARNING, "Error while setting host verify callback: '%s'.", s2n_strerror(s2n_errno, "EN"));               
        goto config_error;            
    }

    if (s2n_config_set_cipher_preferences(ssl_config, cipher_prefs) < 0) {
        serverLog(LL_WARNING, "Error setting cipher prefs on s2n config: '%s'.",
                  s2n_strerror(s2n_errno, "EN"));
        goto config_error;
    }

    return ssl_config;

config_error:
    if (s2n_config_free(ssl_config) < 0)
        serverLog(LL_WARNING, "Error freeing server SSL configuration");
    return NULL;
}

/**
 * Disconnect any clients that are still using old certificate and mark all
 * of the connections as using the older connection so that the count of 
 * connections is accurate.
 */
static void updateClientsUsingOldCertificate(void) {
    if (server.ssl_config.enable_ssl) {
        listIter li;
        listRewind(server.clients, &li);
        listNode *ln;
        client *client;

        if (server.ssl_config.server_ssl_config_old != NULL) {
            serverLog(LL_VERBOSE, "Disconnecting clients using very old certificates");
            unsigned int clients_disconnected = 0;
            while ((ln = listNext(&li)) != NULL) {
                client = listNodeValue(ln);
                if (client->ctime <= server.ssl_config.server_ssl_config_creation_time) {
                    freeClient(client);
                    clients_disconnected++;
                }else{
                    // Mark the connection as connected to the old certificate
                    ssl_connection *ssl_conn = getSslConnectionForFd(client->fd);
                    ssl_conn->connection_flags |= OLD_CERTIFICATE_FLAG;
                }
            }
            //Now that no client is using this config, free this config
            s2n_config_free(server.ssl_config.server_ssl_config_old);
            server.ssl_config.server_ssl_config_old = NULL;
            serverLog(LL_WARNING, "Disconnected %d clients using very old certificate", clients_disconnected);
        }else{
            // If there is no old config, just update the connection properties
            while ((ln = listNext(&li)) != NULL) {
                client = listNodeValue(ln);
                ssl_connection *ssl_conn = getSslConnectionForFd(client->fd);
                ssl_conn->connection_flags |= OLD_CERTIFICATE_FLAG;
            }
        }
    }
}

/* A task that will execute in every event loop iteration and invoke the read handler of any SSL connection
 * for which S2N has cached application data. */
int processRepeatedReads(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(id);
    UNUSED(clientData);

    if (!server.ssl_config.enable_ssl || listLength(server.ssl_config.sslconn_with_cached_data) == 0) {
        server.ssl_config.repeated_reads_task_id = AE_ERR;
        return AE_NOMORE;
    }

    // Create a copy of our list so it can be modified arbitrarily during read handler execution
    list *copy = listDup(server.ssl_config.sslconn_with_cached_data);

    // Record maximum list length
    if (listLength(copy) > server.ssl_config.max_repeated_read_list_length) {
        server.ssl_config.max_repeated_read_list_length = listLength(copy);
    }

    listNode *ln;
    listIter li;

    listRewind(copy, &li);
    while ((ln = listNext(&li))) {
        ssl_connection *conn = ln->value;
        // If the descriptor is not processing read events, skip it this time and check next time.  It will remain on our list until
        // drained.
        if (aeGetFileEvents(eventLoop, conn->fd) & AE_READABLE) {
            // The read handler is expected to remove itself from the repeat read list when there is no longer cached data
            aeGetFileProc(eventLoop, conn->fd, AE_READABLE)(eventLoop, conn->fd, aeGetClientData(eventLoop, conn->fd), AE_READABLE);
            server.ssl_config.total_repeated_reads++;
        }
    }

    listRelease(copy);

    if (listLength(server.ssl_config.sslconn_with_cached_data) == 0) {
        /* No more cached data left */
        server.ssl_config.repeated_reads_task_id = AE_ERR;
        return AE_NOMORE;
    } else {
        return 0; /* Run as fast as possible without sleeping next time around */
    }
}

/* Queue an SSL connection to have its read handler invoked outside of the normal
 * socket notification events in case we do not receive one because there is cached
 * application data inside S2N.  If already queued, will do nothing.  The handler
 * will be repeatedly invoked until removeRepeatedRead is called. */
void addRepeatedRead(ssl_connection *conn) {
    if (conn->cached_data_node != NULL) {
        return;
    }

    listAddNodeTail(server.ssl_config.sslconn_with_cached_data, conn);
    conn->cached_data_node = listLast(server.ssl_config.sslconn_with_cached_data);

    if (server.ssl_config.repeated_reads_task_id == AE_ERR) {
        // Schedule the task to process the list
        server.ssl_config.repeated_reads_task_id = aeCreateTimeEvent(server.el, 0, processRepeatedReads, NULL, NULL);
        if (server.ssl_config.repeated_reads_task_id == AE_ERR) {
            serverLog(LL_WARNING, "Can't create the processRepeatedReads time event.");
        }
    }
}

/* Remove the SSL connection from the queue of repeated read handlers if it exists.
 * One must call this to stop subsequent repeated reads. */
void removeRepeatedRead(ssl_connection *conn) {
    if (conn->cached_data_node == NULL) {
        return;
    }

    listDelNode(server.ssl_config.sslconn_with_cached_data, conn->cached_data_node);
    conn->cached_data_node = NULL;

    // processRepeatedReads task is responsible for self-terminating when no more reads
}

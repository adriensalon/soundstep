#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <httplib.h>
#include <mbedtls/x509_crt.h>

#include <core/peer.hpp>
#include <core/protocol.hpp>
#include <core/storage.hpp>

namespace soundstep {
namespace {

    constexpr std::string_view instance_path = "/v1/instance";
    constexpr std::string_view catalog_path = "/v1/catalog";
    constexpr std::string_view assets_path = "/v1/assets/";
    constexpr std::string_view discovery_path = "/v1/discovery";
    constexpr const char* discovery_address = "239.255.83.83";
    constexpr std::uint16_t discovery_port = 51412;
    constexpr std::chrono::seconds announcement_interval { 3 };
    constexpr std::chrono::seconds synchronization_interval { 15 };
    constexpr std::chrono::seconds refresh_interval { 60 };
    constexpr std::size_t maximum_discovery_size = 4096;

#ifdef _WIN32
    using socket_handle = SOCKET;

    constexpr socket_handle invalid_socket = INVALID_SOCKET;

    void close_socket(socket_handle socket)
    {
        closesocket(socket);
    }

    struct socket_runtime {
        socket_runtime()
        {
            WSADATA _data { };
            if (WSAStartup(MAKEWORD(2, 2), &_data) != 0) {
                throw peer_error("Could not initialize LAN discovery sockets");
            }
        }

        ~socket_runtime()
        {
            WSACleanup();
        }
    };
#else
    using socket_handle = int;

    constexpr socket_handle invalid_socket = -1;

    void close_socket(socket_handle socket)
    {
        close(socket);
    }

    struct socket_runtime { };
#endif

    [[nodiscard]] std::uint64_t current_time_ms()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] std::string normalized_host(std::string host)
    {
        constexpr std::string_view _ipv4_mapped_prefix = "::ffff:";
        if (host.size() > _ipv4_mapped_prefix.size() && host.compare(0, _ipv4_mapped_prefix.size(), _ipv4_mapped_prefix) == 0) {
            return host.substr(_ipv4_mapped_prefix.size());
        }
        return host;
    }

    void add_endpoint(std::vector<peer_endpoint>& endpoints, std::string host, std::uint16_t port, peer_endpoint_family family)
    {
        host = normalized_host(std::move(host));
        if (host.empty() || port == 0 || endpoints.size() >= protocol_maximum_endpoint_count) {
            return;
        }
        for (const peer_endpoint& _endpoint : endpoints) {
            if (_endpoint.host == host && _endpoint.port == port) {
                return;
            }
        }
        endpoints.push_back({ std::move(host), port, family, 0, 0 });
    }

    void add_socket_endpoint(std::vector<peer_endpoint>& endpoints, const sockaddr* address, std::uint16_t port)
    {
        if (address == nullptr) {
            return;
        }
        if (address->sa_family == AF_INET) {
            const sockaddr_in* _address = reinterpret_cast<const sockaddr_in*>(address);
            const std::uint32_t _host_address = ntohl(_address->sin_addr.s_addr);
            if (_host_address == 0 || (_host_address >> 24) == 127) {
                return;
            }
            std::array<char, INET_ADDRSTRLEN> _host { };
            if (inet_ntop(AF_INET, &_address->sin_addr, _host.data(), static_cast<socklen_t>(_host.size())) != nullptr) {
                add_endpoint(endpoints, _host.data(), port, peer_endpoint_family::ipv4);
            }
            return;
        }
        if (address->sa_family == AF_INET6) {
            const sockaddr_in6* _address = reinterpret_cast<const sockaddr_in6*>(address);
            if (IN6_IS_ADDR_UNSPECIFIED(&_address->sin6_addr)
                || IN6_IS_ADDR_LOOPBACK(&_address->sin6_addr)
                || IN6_IS_ADDR_MULTICAST(&_address->sin6_addr)
                || IN6_IS_ADDR_LINKLOCAL(&_address->sin6_addr)
                || IN6_IS_ADDR_V4MAPPED(&_address->sin6_addr)) {
                return;
            }
            std::array<char, INET6_ADDRSTRLEN> _host { };
            if (inet_ntop(AF_INET6, &_address->sin6_addr, _host.data(), static_cast<socklen_t>(_host.size())) != nullptr) {
                add_endpoint(endpoints, _host.data(), port, peer_endpoint_family::ipv6);
            }
        }
    }

    [[nodiscard]] std::vector<peer_endpoint> local_endpoints(std::uint16_t port)
    {
        std::vector<peer_endpoint> _result;
#ifdef _WIN32
        ULONG _size = 16 * 1024;
        std::vector<unsigned char> _buffer(_size);
        IP_ADAPTER_ADDRESSES* _adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(_buffer.data());
        ULONG _status = GetAdaptersAddresses(
            AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            _adapters,
            &_size);
        if (_status == ERROR_BUFFER_OVERFLOW) {
            _buffer.resize(_size);
            _adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(_buffer.data());
            _status = GetAdaptersAddresses(
                AF_UNSPEC,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr,
                _adapters,
                &_size);
        }
        if (_status != NO_ERROR) {
            throw peer_error("Could not enumerate local network addresses");
        }
        for (const IP_ADAPTER_ADDRESSES* _adapter = _adapters;
            _adapter != nullptr;
            _adapter = _adapter->Next) {
            if (_adapter->OperStatus != IfOperStatusUp || _adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            for (const IP_ADAPTER_UNICAST_ADDRESS* _address = _adapter->FirstUnicastAddress;
                _address != nullptr;
                _address = _address->Next) {
                add_socket_endpoint(_result, _address->Address.lpSockaddr, port);
            }
        }
#else
        ifaddrs* _addresses = nullptr;
        if (getifaddrs(&_addresses) != 0) {
            throw peer_error("Could not enumerate local network addresses");
        }
        for (const ifaddrs* _address = _addresses;
            _address != nullptr;
            _address = _address->ifa_next) {
            if ((_address->ifa_flags & IFF_UP) == 0 || (_address->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }
            add_socket_endpoint(_result, _address->ifa_addr, port);
        }
        freeifaddrs(_addresses);
#endif
        if (_result.empty()) {
            throw peer_error("No usable LAN or IPv6 address is available");
        }
        std::stable_sort(_result.begin(), _result.end(), [](const peer_endpoint& left, const peer_endpoint& right) {
            return left.family == peer_endpoint_family::ipv6 && right.family != peer_endpoint_family::ipv6;
        });
        return _result;
    }

    [[nodiscard]] bool same_endpoint_set(const std::vector<peer_endpoint>& left, const std::vector<peer_endpoint>& right)
    {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t _index = 0; _index < left.size(); ++_index) {
            if (left[_index].host != right[_index].host
                || left[_index].port != right[_index].port
                || left[_index].family != right[_index].family) {
                return false;
            }
        }
        return true;
    }

    struct discovery_datagram {
        std::string host { };
        std::string payload { };
    };

    struct discovery_socket {
        discovery_socket()
        {
            _handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (_handle == invalid_socket) {
                throw peer_error("Could not create the LAN discovery socket");
            }

            const int _enabled = 1;
            setsockopt(_handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&_enabled), sizeof(_enabled));

            sockaddr_in _local { };
            _local.sin_family = AF_INET;
            _local.sin_port = htons(discovery_port);
            _local.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(_handle, reinterpret_cast<const sockaddr*>(&_local), sizeof(_local)) != 0) {
                close_socket(_handle);
                _handle = invalid_socket;
                throw peer_error("Could not bind the LAN discovery socket");
            }

            ip_mreq _membership { };
            if (inet_pton(AF_INET, discovery_address, &_membership.imr_multiaddr) != 1) {
                close_socket(_handle);
                _handle = invalid_socket;
                throw peer_error("soundstep LAN discovery address is invalid");
            }
            _membership.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&_membership), sizeof(_membership)) != 0) {
                close_socket(_handle);
                _handle = invalid_socket;
                throw peer_error("Could not join the soundstep LAN discovery group");
            }

#ifdef _WIN32
            const DWORD _timeout = 250;
#else
            const timeval _timeout { 0, 250'000 };
#endif
            setsockopt(_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&_timeout), sizeof(_timeout));
        }

        ~discovery_socket()
        {
            if (_handle != invalid_socket) {
                close_socket(_handle);
            }
        }

        discovery_socket(const discovery_socket&) = delete;
        discovery_socket& operator=(const discovery_socket&) = delete;

        void send(std::string_view payload)
        {
            if (payload.empty() || payload.size() > maximum_discovery_size) {
                throw peer_error("LAN discovery announcement has an invalid size");
            }

            sockaddr_in _destination { };
            _destination.sin_family = AF_INET;
            _destination.sin_port = htons(discovery_port);
            if (inet_pton(AF_INET, discovery_address, &_destination.sin_addr) != 1) {
                throw peer_error("soundstep LAN discovery address is invalid");
            }
            const int _sent = sendto(_handle, payload.data(), static_cast<int>(payload.size()), 0, reinterpret_cast<const sockaddr*>(&_destination), sizeof(_destination));
            if (_sent != static_cast<int>(payload.size())) {
                throw peer_error("Could not send a LAN discovery announcement");
            }
        }

        [[nodiscard]] std::optional<discovery_datagram> receive()
        {
            std::array<char, maximum_discovery_size> _buffer { };
            sockaddr_in _sender { };
#ifdef _WIN32
            int _sender_size = sizeof(_sender);
#else
            socklen_t _sender_size = sizeof(_sender);
#endif
            const int _received = recvfrom(_handle, _buffer.data(), static_cast<int>(_buffer.size()), 0, reinterpret_cast<sockaddr*>(&_sender), &_sender_size);
            if (_received <= 0) {
                return std::nullopt;
            }

            std::array<char, INET_ADDRSTRLEN> _host { };
            if (inet_ntop(AF_INET, &_sender.sin_addr, _host.data(), static_cast<socklen_t>(_host.size())) == nullptr) {
                return std::nullopt;
            }
            return discovery_datagram { _host.data(), std::string(_buffer.data(), static_cast<std::size_t>(_received)) };
        }

    private:
        socket_handle _handle { invalid_socket };
    };

    [[nodiscard]] bool is_asset_hash(std::string_view hash)
    {
        if (hash.size() != 64) {
            return false;
        }

        for (const char _character : hash) {
            const bool _digit = _character >= '0' && _character <= '9';
            const bool _lower_hex = _character >= 'a' && _character <= 'f';
            const bool _upper_hex = _character >= 'A' && _character <= 'F';
            if (!_digit && !_lower_hex && !_upper_hex) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] peer_response invalid_hash_response()
    {
        return { 0, { }, "Asset hash must contain exactly 64 hexadecimal characters" };
    }

    [[nodiscard]] transport_identity load_transport_identity(storage& store)
    {
        const std::optional<transport_identity> _stored = store.transport_identity();
        if (_stored) {
            return *_stored;
        }
        transport_identity _created = create_transport_identity();
        store.set_transport_identity(_created);
        return _created;
    }

    void set_error(httplib::Response& response, int status, std::string message)
    {
        response.status = status;
        response.set_content(std::move(message), "text/plain; charset=utf-8");
    }

    bool authorize(const httplib::Request& request, httplib::Response& response, storage& store, std::string* accepted_token = nullptr)
    {
        constexpr std::string_view _prefix = "Bearer ";
        const std::string _authorization = request.get_header_value("Authorization");
        if (_authorization.size() > _prefix.size() && std::string_view(_authorization).substr(0, _prefix.size()) == _prefix) {
            const std::string _token = _authorization.substr(_prefix.size());
            if (store.authorize_access(_token)) {
                if (accepted_token != nullptr) {
                    *accepted_token = _token;
                }
                return true;
            }
        }
        if (accepted_token != nullptr) {
            accepted_token->clear();
        }
        response.set_header("WWW-Authenticate", "Bearer");
        set_error(response, 401, "A valid Soundstep access token is required");
        return false;
    }

    [[nodiscard]] std::string response_message(std::string message, const peer_response& response)
    {
        if (!response.error_message.empty()) {
            return message + ": " + response.error_message;
        }
        if (response.status_code != 0) {
            return message + ": HTTP " + std::to_string(response.status_code);
        }
        return message;
    }

    void merge_endpoint(std::vector<peer_endpoint>& endpoints, peer_endpoint value)
    {
        value.host = normalized_host(std::move(value.host));
        for (peer_endpoint& _endpoint : endpoints) {
            if (_endpoint.host == value.host && _endpoint.port == value.port) {
                _endpoint.family = value.family;
                _endpoint.last_seen_ms = (std::max)(_endpoint.last_seen_ms, value.last_seen_ms);
                _endpoint.last_success_ms = (std::max)(_endpoint.last_success_ms, value.last_success_ms);
                return;
            }
        }
        if (endpoints.size() < protocol_maximum_endpoint_count) {
            endpoints.push_back(std::move(value));
        }
    }

    [[nodiscard]] peer_endpoint_family host_family(std::string_view host)
    {
        return host.find(':') == std::string_view::npos
            ? peer_endpoint_family::ipv4
            : peer_endpoint_family::ipv6;
    }

    [[nodiscard]] peer_record synchronize_peer(storage& store, const peer_endpoint& connection, std::vector<peer_endpoint> endpoints, std::string token, std::string fingerprint, peer_origin origin, std::string_view expected_id)
    {
        peer_client _client(connection.host, connection.port, token, fingerprint);
        peer_response _identity_response = _client.fetch_instance();
        if (!_identity_response) {
            throw peer_error(response_message("Could not read peer identity", _identity_response));
        }

        const instance_info _identity = protocol_decode_instance(_identity_response.body);
        if (!expected_id.empty() && _identity.id != expected_id) {
            throw peer_error("Peer identity does not match the discovered instance ID");
        }
        if (_identity.id == store.instance().id) {
            throw peer_error("The local instance cannot pair with itself");
        }

        const std::uint64_t _now = current_time_ms();
        for (peer_endpoint& _endpoint : endpoints) {
            if (_endpoint.last_seen_ms == 0) {
                _endpoint.last_seen_ms = _now;
            }
        }
        peer_endpoint _successful = connection;
        _successful.host = normalized_host(std::move(_successful.host));
        _successful.family = host_family(_successful.host);
        _successful.last_seen_ms = _now;
        _successful.last_success_ms = _now;
        merge_endpoint(endpoints, std::move(_successful));

        peer_record _record { _identity.id, _identity.name, std::move(token), std::move(fingerprint), origin, _now, true, std::move(endpoints) };
        store.upsert_peer(_record);

        peer_response _catalog_response = _client.fetch_catalog();
        if (!_catalog_response) {
            throw peer_error(response_message("Could not read peer catalog", _catalog_response));
        }

        catalog_snapshot _catalog = protocol_decode_catalog(_catalog_response.body);
        if (_catalog.id != _identity.id || _catalog.owner_instance_id != _identity.id) {
            throw peer_error("Peer catalog does not belong to the verified instance");
        }

        const std::optional<std::uint64_t> _revision = store.catalog_revision(_catalog.id);
        if (!_revision || *_revision != _catalog.revision) {
            store.replace_catalog(_catalog);
        }
        return _record;
    }

    peer_record synchronize_candidates(storage& store, const std::vector<peer_endpoint>& endpoints, std::string token, std::string fingerprint, peer_origin origin, std::string_view expected_id)
    {
        if (endpoints.empty()) {
            throw peer_error("Peer has no usable endpoint");
        }

        std::string _last_error;
        for (const peer_endpoint& _endpoint : endpoints) {
            try {
                return synchronize_peer(store, _endpoint, endpoints, token, fingerprint, origin, expected_id);
            } catch (const std::exception& _exception) {
                _last_error = _exception.what();
            }
        }
        throw peer_error(_last_error.empty() ? "Could not connect to any peer endpoint" : _last_error);
    }

    [[nodiscard]] std::vector<peer_endpoint> with_observed_endpoint(const std::vector<peer_endpoint>& advertised, std::string host)
    {
        if (advertised.empty()) {
            throw peer_error("Peer announcement has no endpoint");
        }
        const std::uint64_t _now = current_time_ms();
        host = normalized_host(std::move(host));
        std::vector<peer_endpoint> _result;
        _result.reserve(advertised.size() + 1);
        merge_endpoint(_result, { host, advertised.front().port, host_family(host), _now, 0 });
        for (peer_endpoint _endpoint : advertised) {
            if (_endpoint.last_seen_ms == 0) {
                _endpoint.last_seen_ms = _now;
            }
            merge_endpoint(_result, std::move(_endpoint));
        }
        return _result;
    }

}

struct peer_client::implementation {
    struct _connection {
        _connection(const peer_endpoint& endpoint, const httplib::Headers& headers, const std::string& fingerprint)
            : _endpoint(endpoint)
            , _headers(headers)
            , _client(_endpoint.host, _endpoint.port)
        {
            _client.enable_server_hostname_verification(false);
            _client.enable_server_certificate_verification(false);
            _client.set_session_verifier([&fingerprint](httplib::tls::session_t session) {
                const httplib::tls::impl::MbedTlsSession* _session = static_cast<const httplib::tls::impl::MbedTlsSession*>(session);
                const mbedtls_x509_crt* _certificate = mbedtls_ssl_get_peer_cert(&_session->ssl);
                if (_certificate == nullptr) {
                    return httplib::SSLVerifierResponse::CertificateRejected;
                }
                try {
                    const bool _matches = data_fingerprint(std::string_view(reinterpret_cast<const char*>(_certificate->raw.p), _certificate->raw.len)) == fingerprint;
                    return _matches ? httplib::SSLVerifierResponse::CertificateAccepted : httplib::SSLVerifierResponse::CertificateRejected;
                } catch (...) {
                    return httplib::SSLVerifierResponse::CertificateRejected;
                }
            });
            _client.set_keep_alive(true);
            _client.set_connection_timeout(std::chrono::seconds(3));
            _client.set_read_timeout(std::chrono::seconds(15));
            _client.set_write_timeout(std::chrono::seconds(15));
        }

        peer_endpoint _endpoint;
        httplib::Headers _headers;
        httplib::SSLClient _client;
    };

    implementation(std::string peer_host, std::uint16_t port, std::string peer_token, std::string fingerprint)
        : _endpoints { { std::move(peer_host), port, peer_endpoint_family::ipv4, 0, 0 } }
        , _headers { { "Authorization", "Bearer " + std::move(peer_token) } }
        , _fingerprint(std::move(fingerprint))
    {
        _endpoints.front().family = host_family(_endpoints.front().host);
        if (_fingerprint.size() != 64) {
            throw peer_error("Peer transport fingerprint is invalid");
        }
    }

    implementation(storage& store, const peer_record& record)
        : _store(&store)
        , _peer_id(record.id)
        , _endpoints(record.endpoints)
        , _headers { { "Authorization", "Bearer " + record.token } }
        , _fingerprint(record.fingerprint)
    {
        if (_peer_id.empty()) {
            throw peer_error("Peer ID cannot be empty");
        }
        if (_endpoints.empty()) {
            throw peer_error("Peer has no usable endpoint");
        }
        if (record.token.empty()) {
            throw peer_error("Peer token cannot be empty");
        }
        if (_fingerprint.size() != 64) {
            throw peer_error("Pair with this instance again before connecting securely");
        }
    }

    static bool retryable(const peer_response& response)
    {
        return response.status_code == 0
            || response.status_code == 408
            || response.status_code == 429
            || response.status_code >= 500
            || (response.status_code >= 200 && response.status_code < 300 && !response.error_message.empty());
    }

    void succeeded(const peer_endpoint& endpoint) noexcept
    {
        if (_store == nullptr) {
            return;
        }
        try {
            _store->mark_peer_endpoint_success(_peer_id, endpoint);
        } catch (...) {
        }
    }

    peer_response request(const std::function<peer_response(_connection&)>& operation)
    {
        peer_response _last_response { 0, { }, "Could not connect to any peer endpoint" };
        for (const peer_endpoint& _endpoint : _endpoints) {
            _connection _remote(_endpoint, _headers, _fingerprint);
            peer_response _response = operation(_remote);
            if (_response) {
                succeeded(_endpoint);
                return _response;
            }
            _last_response = std::move(_response);
            if (!retryable(_last_response)) {
                return _last_response;
            }
        }
        if (_endpoints.size() > 1 && !_last_response.error_message.empty()) {
            _last_response.error_message = "All peer endpoints failed; last error: " + _last_response.error_message;
        }
        return _last_response;
    }

    peer_response get(std::string_view path)
    {
        const std::string _path(path);
        return request([&_path](_connection& remote) {
            const httplib::Result _result = remote._client.Get(_path, remote._headers);
            if (!_result) {
                return peer_response { 0, { }, httplib::to_string(_result.error()) };
            }
            return peer_response { _result->status, std::move(_result->body), { } };
        });
    }

    storage* _store { nullptr };
    std::string _peer_id;
    std::vector<peer_endpoint> _endpoints;
    httplib::Headers _headers;
    std::string _fingerprint;
};

struct peer_server::implementation {
    implementation(storage& store, std::uint16_t requested_port, std::string bind_address)
        : _identity(load_transport_identity(store))
        , _server(std::make_unique<httplib::SSLServer>(httplib::SSLServer::PemMemory {
              _identity.certificate_pem.c_str(),
              _identity.certificate_pem.size(),
              _identity.private_key_pem.c_str(),
              _identity.private_key_pem.size(),
              nullptr,
              0,
              nullptr }))
    {
        if (!_server->is_valid()) {
            throw peer_error("Could not initialize the secure peer server");
        }
        if (bind_address.find(':') != std::string::npos) {
            _server->set_address_family(AF_INET6);
            _server->set_ipv6_v6only(false);
        } else {
            _server->set_address_family(AF_INET);
        }
        _server->new_task_queue = [] {
            return new httplib::ThreadPool(4);
        };

        _server->Get(instance_path.data(), [&store](const httplib::Request& request, httplib::Response& response) {
            if (!authorize(request, response, store)) {
                return;
            }
            response.set_header("Cache-Control", "no-store");
            response.set_content(protocol_encode_instance(store.instance()), std::string(protocol_content_type));
        });

        _server->Get(catalog_path.data(), [&store](const httplib::Request& request, httplib::Response& response) {
            if (!authorize(request, response, store)) {
                return;
            }
            const instance_info _instance = store.instance();
            const std::optional<catalog_snapshot> _catalog = store.catalog(_instance.id);
            if (!_catalog) {
                set_error(response, 500, "Local catalog was not found");
                return;
            }
            response.set_header("Cache-Control", "no-store");
            response.set_content(protocol_encode_catalog(*_catalog), std::string(protocol_content_type));
        });

        _server->Post(discovery_path.data(), [&store](const httplib::Request& request, httplib::Response& response) {
            try {
                std::string _access_token;
                if (!authorize(request, response, store, &_access_token)) {
                    return;
                }
                const peer_discovery _announcement = protocol_decode_discovery(request.body);
                const bool _lan_access = _access_token == store.lan_token();
                synchronize_candidates(
                    store,
                    with_observed_endpoint(_announcement.endpoints, request.remote_addr),
                    _announcement.token,
                    _announcement.fingerprint,
                    _lan_access ? peer_origin::lan : peer_origin::pairing_code,
                    _announcement.instance.id);
                if (!_lan_access) {
                    store.claim_invitation(_access_token, _announcement.instance.id);
                }
                response.status = 204;
            } catch (const std::exception& _exception) {
                set_error(response, 400, _exception.what());
            }
        });

        _server->Get("/v1/covers/:hash", [&store](const httplib::Request& request, httplib::Response& response) {
            if (!authorize(request, response, store)) {
                return;
            }
            const std::unordered_map<std::string, std::string>::const_iterator _hash = request.path_params.find("hash");
            if (_hash == request.path_params.end() || !is_asset_hash(_hash->second)) {
                set_error(response, 400, "Invalid cover hash");
                return;
            }
            const std::optional<cover_art> _cover = store.cover(_hash->second);
            if (!_cover) {
                set_error(response, 404, "Cover was not found");
                return;
            }
            response.set_header("Cache-Control", "public, immutable");
            response.set_header("ETag", "\"" + _cover->hash + "\"");
            response.set_content(
                reinterpret_cast<const char*>(_cover->bytes.data()),
                _cover->bytes.size(),
                _cover->content_type);
        });

        _server->Get("/v1/assets/:hash", [&store](const httplib::Request& request, httplib::Response& response) {
            if (!authorize(request, response, store)) {
                return;
            }
            const std::unordered_map<std::string, std::string>::const_iterator _hash = request.path_params.find("hash");
            if (_hash == request.path_params.end() || !is_asset_hash(_hash->second)) {
                set_error(response, 400, "Invalid asset hash");
                return;
            }

            const std::optional<file_location> _file = store.find_file(_hash->second);
            if (!_file) {
                set_error(response, 404, "Asset was not found");
                return;
            }

            std::error_code _filesystem_error;
            const std::filesystem::path _path = std::filesystem::u8path(_file->path);
            if (!std::filesystem::is_regular_file(_path, _filesystem_error) || _filesystem_error) {
                set_error(response, 404, "Asset file was not found");
                return;
            }

            const std::uint64_t _actual_size = std::filesystem::file_size(_path, _filesystem_error);
            if (_filesystem_error || _actual_size != _file->size_bytes) {
                set_error(response, 409, "Asset file no longer matches the catalog");
                return;
            }

            response.set_header("Accept-Ranges", "bytes");
            response.set_header("Cache-Control", "public, immutable");
            response.set_header("ETag", "\"" + _file->hash + "\"");
            response.set_file_content(_file->path, std::string(audio_content_type(_file->extension)));
        });

        if (requested_port == 0) {
            const int _selected_port = _server->bind_to_any_port(bind_address);
            if (_selected_port <= 0) {
                throw peer_error("Failed to bind the peer HTTP server");
            }
            _bound_port = static_cast<std::uint16_t>(_selected_port);
        } else {
            if (!_server->bind_to_port(bind_address, requested_port)) {
                throw peer_error("Failed to bind the peer HTTP server to port " + std::to_string(requested_port));
            }
            _bound_port = requested_port;
        }

        _listener = std::thread([this] {
            _server->listen_after_bind();
        });
        _server->wait_until_ready();

        if (!_server->is_running()) {
            if (_listener.joinable()) {
                _listener.join();
            }
            throw peer_error("Failed to start the peer HTTP server");
        }
    }

    ~implementation()
    {
        _server->stop();
        if (_listener.joinable()) {
            _listener.join();
        }
    }

    transport_identity _identity;
    std::unique_ptr<httplib::SSLServer> _server;
    std::thread _listener;
    std::uint16_t _bound_port { 0 };
};

struct peer_network::implementation {
    implementation(storage& store, peer_server& server)
        : _store(store)
        , _server(server)
        , _worker([this] { run(); })
    {
    }

    ~implementation()
    {
        _stop.store(true, std::memory_order_release);
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    peer_record synchronize(const std::vector<peer_endpoint>& endpoints, std::string token, std::string fingerprint, peer_origin origin, std::string_view expected_id)
    {
        return synchronize_candidates(
            _store,
            endpoints,
            std::move(token),
            std::move(fingerprint),
            origin,
            expected_id);
    }

    void refresh_catalogs()
    {
        const std::vector<peer_record> _peers = _store.peers();
        for (const peer_record& _record : _peers) {
            try {
                if (!_record.token.empty()
                    && !_record.fingerprint.empty()
                    && !_record.endpoints.empty()) {
                    const peer_record _refreshed = synchronize(
                        _record.endpoints,
                        _record.token,
                        _record.fingerprint,
                        _record.origin,
                        _record.id);
                    if (_refreshed.origin == peer_origin::pairing_code) {
                        peer_client _client(_store, _refreshed);
                        static_cast<void>(_client.announce({ _store.instance(),
                            local_endpoints(_server.port()),
                            _store.access_token_for_peer(_refreshed.id),
                            _server.fingerprint() }));
                    }
                }
            } catch (...) {
            }
        }
    }

    void run() noexcept
    {
        refresh_catalogs();
        while (!_stop.load(std::memory_order_acquire)) {
            try {
                discovery_socket _socket;
                std::chrono::steady_clock::time_point _next_announcement { };
                std::chrono::steady_clock::time_point _next_refresh = std::chrono::steady_clock::now() + refresh_interval;
                std::vector<peer_endpoint> _known_endpoints;

                while (!_stop.load(std::memory_order_acquire)) {
                    const std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now();
                    if (_now >= _next_announcement) {
                        const std::vector<peer_endpoint> _current_endpoints = local_endpoints(_server.port());
                        const bool _network_changed = !_known_endpoints.empty() && !same_endpoint_set(_known_endpoints, _current_endpoints);
                        if (_store.config().lan_discovery_enabled) {
                            _socket.send(protocol_encode_discovery({ _store.instance(), _current_endpoints, _store.lan_token(), _server.fingerprint() }));
                        }
                        _known_endpoints = _current_endpoints;
                        _next_announcement = _now + announcement_interval;
                        if (_network_changed) {
                            refresh_catalogs();
                            _next_refresh = _now + refresh_interval;
                        }
                    }
                    if (_refresh_requested.exchange(false, std::memory_order_acq_rel) || _now >= _next_refresh) {
                        refresh_catalogs();
                        _next_refresh = _now + refresh_interval;
                    }

                    const std::optional<discovery_datagram> _datagram = _socket.receive();
                    if (_datagram) {
                        receive(*_datagram);
                    }
                }
            } catch (...) {
                for (int _attempt = 0; _attempt < 20 && !_stop.load(std::memory_order_acquire); ++_attempt) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
    }

    void receive(const discovery_datagram& datagram) noexcept
    {
        try {
            const peer_discovery _announcement = protocol_decode_discovery(datagram.payload);
            if (_announcement.instance.id == _store.instance().id) {
                return;
            }

            const std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now();
            const std::unordered_map<std::string, std::chrono::steady_clock::time_point>::const_iterator _previous = _last_synchronized.find(_announcement.instance.id);
            if (_previous != _last_synchronized.end() && _now - _previous->second < synchronization_interval) {
                return;
            }
            _last_synchronized.insert_or_assign(_announcement.instance.id, _now);
            const std::vector<peer_endpoint> _endpoints = with_observed_endpoint(_announcement.endpoints, datagram.host);
            const peer_record _record = synchronize(_endpoints, _announcement.token, _announcement.fingerprint, peer_origin::lan, _announcement.instance.id);
            if (_store.config().lan_discovery_enabled) {
                peer_client _client(_store, _record);
                static_cast<void>(_client.announce({ _store.instance(),
                    local_endpoints(_server.port()),
                    _store.lan_token(),
                    _server.fingerprint() }));
            }
        } catch (...) {
        }
    }

    storage& _store;
    peer_server& _server;
    socket_runtime _runtime;
    std::atomic<bool> _stop { false };
    std::atomic<bool> _refresh_requested { false };
    std::thread _worker;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> _last_synchronized;
};

bool peer_response::ok() const noexcept
{
    return error_message.empty() && status_code >= 200 && status_code < 300;
}

peer_response::operator bool() const noexcept
{
    return ok();
}

peer_client::peer_client(std::string host, std::uint16_t port, std::string token, std::string fingerprint)
    : _implementation(std::make_unique<implementation>(std::move(host), port, std::move(token), std::move(fingerprint)))
{
}

peer_client::peer_client(storage& store, const peer_record& record)
    : _implementation(std::make_unique<implementation>(store, record))
{
}

peer_client::~peer_client() = default;
peer_client::peer_client(peer_client&& other) noexcept = default;
peer_client& peer_client::operator=(peer_client&& other) noexcept = default;

peer_response peer_client::fetch_instance()
{
    return _implementation->get(instance_path);
}

peer_response peer_client::fetch_catalog()
{
    return _implementation->get(catalog_path);
}

peer_response peer_client::fetch_cover(std::string_view hash)
{
    if (!is_asset_hash(hash)) {
        return invalid_hash_response();
    }
    return _implementation->get("/v1/covers/" + std::string(hash));
}

peer_response peer_client::announce(const peer_discovery& discovery)
{
    const std::string _body = protocol_encode_discovery(discovery);
    return _implementation->request([&_body](implementation::_connection& remote) {
        const httplib::Result _result = remote._client.Post(std::string(discovery_path), remote._headers, _body, std::string(protocol_content_type));
        if (!_result) {
            return peer_response { 0, { }, httplib::to_string(_result.error()) };
        }
        return peer_response { _result->status, std::move(_result->body), { } };
    });
}

peer_response peer_client::stream_asset(std::string_view hash, const chunk_handler& on_chunk, std::uint64_t offset, std::optional<std::uint64_t> length)
{
    if (!is_asset_hash(hash)) {
        return invalid_hash_response();
    }
    if (!on_chunk) {
        return { 0, { }, "An asset chunk handler is required" };
    }
    if (length && *length == 0) {
        return { 0, { }, "Asset range length must be greater than zero" };
    }
    if (length && *length - 1 > (std::numeric_limits<std::uint64_t>::max)() - offset) {
        return { 0, { }, "Asset byte range is too large" };
    }

    std::uint64_t _delivered = 0;
    peer_response _last_response { 0, { }, "Could not connect to any peer endpoint" };
    for (const peer_endpoint& _endpoint : _implementation->_endpoints) {
        implementation::_connection _remote(_endpoint, _implementation->_headers, _implementation->_fingerprint);
        httplib::Headers _headers = _remote._headers;
        const std::uint64_t _attempt_offset = offset + _delivered;
        const std::optional<std::uint64_t> _attempt_length = length ? std::optional<std::uint64_t>(*length - _delivered) : std::nullopt;
        const bool _range_requested = _attempt_offset != 0 || _attempt_length.has_value();
        if (_range_requested) {
            std::string _range = "bytes=" + std::to_string(_attempt_offset) + "-";
            if (_attempt_length) {
                _range += std::to_string(_attempt_offset + *_attempt_length - 1);
            }
            _headers.emplace("Range", std::move(_range));
        }

        bool _handler_stopped = false;
        peer_response _response;
        const httplib::Result _result = _remote._client.Get(std::string(assets_path) + std::string(hash), _headers, [&_response, _range_requested](const httplib::Response& incoming) {
                _response.status_code = incoming.status;
                const bool _success = incoming.status >= 200 && incoming.status < 300;
                const bool _valid_range = !_range_requested || incoming.status == 206;
                return _success && _valid_range; }, [&on_chunk, &_delivered, &_handler_stopped](const char* data, std::size_t size) {
                if (!on_chunk(data, size)) {
                    _handler_stopped = true;
                    return false;
                }
                _delivered += static_cast<std::uint64_t>(size);
                return true; });

        if (_handler_stopped) {
            _response.error_message = "Asset transfer was cancelled by the receiver";
            return _response;
        }
        if (_result) {
            _response.status_code = _result->status;
            if (!length || _delivered == *length) {
                _implementation->succeeded(_endpoint);
                return _response;
            }
            _response.error_message = "Peer returned an incomplete audio byte range";
        } else if (_response.status_code == 200 && _range_requested) {
            _response.error_message = "Peer did not honor the requested byte range";
        } else if (_response.status_code != 0) {
            _response.error_message = "Peer returned HTTP status " + std::to_string(_response.status_code);
        } else {
            _response.error_message = httplib::to_string(_result.error());
        }

        _last_response = std::move(_response);
        if (!implementation::retryable(_last_response)) {
            return _last_response;
        }
        if (length && _delivered == *length) {
            _implementation->succeeded(_endpoint);
            return { 206, { }, { } };
        }
    }

    if (_implementation->_endpoints.size() > 1 && !_last_response.error_message.empty()) {
        _last_response.error_message = "All peer endpoints failed; last error: " + _last_response.error_message;
    }
    return _last_response;
}

peer_response peer_client::download_asset(std::string_view hash, const std::filesystem::path& destination)
{
    if (!is_asset_hash(hash)) {
        return invalid_hash_response();
    }
    if (std::filesystem::exists(destination)) {
        return { 0, { }, "Download destination already exists" };
    }

    std::filesystem::path _partial = destination;
    _partial += ".part";

    std::error_code _filesystem_error;
    const std::uint64_t _offset = std::filesystem::exists(_partial, _filesystem_error) ? std::filesystem::file_size(_partial, _filesystem_error) : 0;
    if (_filesystem_error) {
        return { 0, { }, "Could not inspect partial download: " + _filesystem_error.message() };
    }

    std::ofstream _output(_partial, std::ios::binary | (_offset == 0 ? std::ios::trunc : std::ios::app));
    if (!_output) {
        return { 0, { }, "Could not open partial download for writing" };
    }

    bool _write_failed = false;
    peer_response _response = stream_asset(hash, [&_output, &_write_failed](const char* data, std::size_t size) {
            _output.write(data, static_cast<std::streamsize>(size));
            _write_failed = !_output;
            return !_write_failed; }, _offset);

    _output.flush();
    if (!_output || _write_failed) {
        _response.error_message = "Failed while writing the partial download";
        return _response;
    }
    _output.close();

    if (!_response) {
        return _response;
    }

    std::filesystem::rename(_partial, destination, _filesystem_error);
    if (_filesystem_error) {
        _response.error_message = "Could not finalize download: " + _filesystem_error.message();
    }
    return _response;
}

const std::string& peer_client::host() const noexcept
{
    return _implementation->_endpoints.front().host;
}

std::uint16_t peer_client::port() const noexcept
{
    return _implementation->_endpoints.front().port;
}

const std::string& peer_client::fingerprint() const noexcept
{
    return _implementation->_fingerprint;
}

peer_server::peer_server(storage& store, std::uint16_t port, std::string bind_address)
    : _implementation(std::make_unique<implementation>(store, port, std::move(bind_address)))
{
}

peer_server::~peer_server() = default;

std::uint16_t peer_server::port() const noexcept
{
    return _implementation->_bound_port;
}

const std::string& peer_server::fingerprint() const noexcept
{
    return _implementation->_identity.fingerprint;
}

peer_network::peer_network(storage& store, peer_server& server)
    : _implementation(std::make_unique<implementation>(store, server))
{
}

peer_network::~peer_network() = default;

std::string peer_network::create_pairing_code()
{
    return protocol_encode_invite({ _implementation->_store.instance(),
        local_endpoints(_implementation->_server.port()),
        _implementation->_store.create_invitation_token(),
        _implementation->_server.fingerprint() });
}

void peer_network::accept_pairing_code(std::string_view code)
{
    const peer_invite _invite = protocol_decode_invite(code);
    const peer_record _record = _implementation->synchronize(_invite.endpoints, _invite.token, _invite.fingerprint, peer_origin::pairing_code, _invite.instance.id);
    peer_client _client(_implementation->_store, _record);
    const peer_response _announcement = _client.announce({ _implementation->_store.instance(), local_endpoints(_implementation->_server.port()), _implementation->_store.access_token_for_peer(_record.id), _implementation->_server.fingerprint() });
    if (!_announcement) {
        throw peer_error(response_message("Could not complete reciprocal pairing", _announcement));
    }
}

void peer_network::refresh_catalogs()
{
    _implementation->refresh_catalogs();
}

void peer_network::refresh_catalogs_async() noexcept
{
    _implementation->_refresh_requested.store(true, std::memory_order_release);
}

}

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>

#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <core/security.hpp>

namespace soundstep {
namespace {

    void require_psa(psa_status_t status, std::string message)
    {
        if (status != PSA_SUCCESS) {
            throw security_error(std::move(message) + ": " + std::to_string(status));
        }
    }

    void require_mbedtls(int status, std::string message)
    {
        if (status != 0) {
            throw security_error(std::move(message) + ": " + std::to_string(status));
        }
    }

    struct key_guard {
        key_guard()
        {
            mbedtls_pk_init(&_value);
        }

        ~key_guard()
        {
            mbedtls_pk_free(&_value);
            if (_id != PSA_KEY_ID_NULL) {
                static_cast<void>(psa_destroy_key(_id));
            }
        }

        mbedtls_pk_context _value;
        psa_key_id_t _id { PSA_KEY_ID_NULL };
    };

    struct certificate_guard {
        certificate_guard()
        {
            mbedtls_x509_crt_init(&_value);
        }

        ~certificate_guard()
        {
            mbedtls_x509_crt_free(&_value);
        }

        mbedtls_x509_crt _value;
    };

    struct certificate_writer_guard {
        certificate_writer_guard()
        {
            mbedtls_x509write_crt_init(&_value);
        }

        ~certificate_writer_guard()
        {
            mbedtls_x509write_crt_free(&_value);
        }

        mbedtls_x509write_cert _value;
    };

    [[nodiscard]] char hex_digit(unsigned value)
    {
        return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
    }

    [[nodiscard]] std::string hex_fingerprint(const std::array<unsigned char, 32>& digest)
    {
        std::string _result;
        _result.resize(digest.size() * 2);
        for (std::size_t _index = 0; _index < digest.size(); ++_index) {
            _result[_index * 2] = hex_digit(digest[_index] >> 4);
            _result[_index * 2 + 1] = hex_digit(digest[_index] & 0x0f);
        }
        return _result;
    }

    struct hash_guard {
        ~hash_guard()
        {
            static_cast<void>(psa_hash_abort(&_operation));
        }

        psa_hash_operation_t _operation = PSA_HASH_OPERATION_INIT;
    };

}

std::string data_fingerprint(std::string_view bytes)
{
    require_psa(psa_crypto_init(), "Could not initialize transport cryptography");
    std::array<unsigned char, 32> _digest { };
    std::size_t _digest_size = 0;
    require_psa(psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), _digest.data(), _digest.size(), &_digest_size), "Could not fingerprint data");
    if (_digest_size != _digest.size()) {
        throw security_error("Data fingerprint has an invalid size");
    }

    return hex_fingerprint(_digest);
}

std::string file_fingerprint(const std::filesystem::path& path)
{
    require_psa(psa_crypto_init(), "Could not initialize transport cryptography");
    std::ifstream _input(path, std::ios::binary);
    if (!_input) {
        throw security_error("Could not open file for fingerprinting: " + path.u8string());
    }

    hash_guard _hash;
    require_psa(psa_hash_setup(&_hash._operation, PSA_ALG_SHA_256), "Could not initialize file fingerprint");
    std::array<unsigned char, 64 * 1024> _buffer { };
    while (_input) {
        _input.read(reinterpret_cast<char*>(_buffer.data()), static_cast<std::streamsize>(_buffer.size()));
        const std::streamsize _size = _input.gcount();
        if (_size > 0) {
            require_psa(psa_hash_update(&_hash._operation, _buffer.data(), static_cast<std::size_t>(_size)), "Could not fingerprint file data");
        }
    }
    if (!_input.eof()) {
        throw security_error("Could not read file for fingerprinting: " + path.u8string());
    }

    std::array<unsigned char, 32> _digest { };
    std::size_t _digest_size = 0;
    require_psa(psa_hash_finish(&_hash._operation, _digest.data(), _digest.size(), &_digest_size), "Could not finish file fingerprint");
    if (_digest_size != _digest.size()) {
        throw security_error("File fingerprint has an invalid size");
    }
    return hex_fingerprint(_digest);
}

std::string transport_certificate_fingerprint(std::string_view certificate)
{
    require_psa(psa_crypto_init(), "Could not initialize transport cryptography");
    const std::string _certificate(certificate);
    certificate_guard _parsed;
    require_mbedtls(mbedtls_x509_crt_parse(&_parsed._value, reinterpret_cast<const unsigned char*>(_certificate.c_str()), _certificate.size() + 1), "Could not parse the transport certificate");
    return data_fingerprint(std::string_view(reinterpret_cast<const char*>(_parsed._value.raw.p), _parsed._value.raw.len));
}

transport_identity create_transport_identity()
{
    require_psa(psa_crypto_init(), "Could not initialize transport cryptography");

    psa_key_attributes_t _attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&_attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&_attributes, 256);
    psa_set_key_usage_flags(&_attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&_attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    key_guard _key;
    require_psa(psa_generate_key(&_attributes, &_key._id), "Could not generate the transport private key");
    psa_reset_key_attributes(&_attributes);
    require_mbedtls(mbedtls_pk_copy_from_psa(_key._id, &_key._value), "Could not export the transport private key");

    std::array<unsigned char, 2048> _key_pem { };
    require_mbedtls(mbedtls_pk_write_key_pem(&_key._value, _key_pem.data(), _key_pem.size()), "Could not encode the transport private key");

    std::array<unsigned char, 16> _serial { };
    require_psa(psa_generate_random(_serial.data(), _serial.size()), "Could not generate the transport certificate serial number");
    _serial.front() &= 0x7f;
    _serial.front() |= 0x01;

    certificate_writer_guard _writer;
    mbedtls_x509write_crt_set_subject_key(&_writer._value, &_key._value);
    mbedtls_x509write_crt_set_issuer_key(&_writer._value, &_key._value);
    require_mbedtls(mbedtls_x509write_crt_set_subject_name(&_writer._value, "CN=Soundstep"), "Could not set the transport certificate subject");
    require_mbedtls(mbedtls_x509write_crt_set_issuer_name(&_writer._value, "CN=Soundstep"), "Could not set the transport certificate issuer");
    mbedtls_x509write_crt_set_version(&_writer._value, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&_writer._value, MBEDTLS_MD_SHA256);
    require_mbedtls(mbedtls_x509write_crt_set_serial_raw(&_writer._value, _serial.data(), _serial.size()), "Could not set the transport certificate serial number");
    require_mbedtls(mbedtls_x509write_crt_set_validity(&_writer._value, "20240101000000", "20491231235959"), "Could not set the transport certificate validity");
    require_mbedtls(mbedtls_x509write_crt_set_basic_constraints(&_writer._value, 0, -1), "Could not set the transport certificate constraints");
    require_mbedtls(mbedtls_x509write_crt_set_key_usage(&_writer._value, MBEDTLS_X509_KU_DIGITAL_SIGNATURE), "Could not set the transport certificate key usage");

    std::array<unsigned char, 4096> _certificate_pem { };
    require_mbedtls(mbedtls_x509write_crt_pem(&_writer._value, _certificate_pem.data(), _certificate_pem.size()), "Could not encode the transport certificate");

    transport_identity _result {
        reinterpret_cast<const char*>(_certificate_pem.data()),
        reinterpret_cast<const char*>(_key_pem.data()),
        { }
    };
    _result.fingerprint = transport_certificate_fingerprint(_result.certificate_pem);
    return _result;
}

}

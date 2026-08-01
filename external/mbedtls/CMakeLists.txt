file(GLOB soundstep_mbedtls_sources CONFIGURE_DEPENDS
    "library/*.c"
    "tf-psa-crypto/core/*.c"
    "tf-psa-crypto/drivers/builtin/src/*.c"
    "tf-psa-crypto/drivers/everest/library/Hacl_Curve25519_joined.c"
    "tf-psa-crypto/drivers/everest/library/x25519.c"
    "tf-psa-crypto/drivers/p256-m/*.c"
    "tf-psa-crypto/drivers/p256-m/p256-m/*.c"
    "tf-psa-crypto/drivers/pqcp/src/*.c"
    "tf-psa-crypto/extras/*.c"
    "tf-psa-crypto/platform/*.c"
    "tf-psa-crypto/utilities/*.c")

add_library(soundstep_mbedtls STATIC ${soundstep_mbedtls_sources})

set_target_properties(soundstep_mbedtls PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

target_include_directories(soundstep_mbedtls SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/builtin/include")

target_include_directories(soundstep_mbedtls PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/library"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/dispatch"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/builtin/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/everest/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/everest/include/tf-psa-crypto/private/everest"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/everest/include/tf-psa-crypto/private/everest/kremlib"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/p256-m"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/p256-m/p256-m"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/pqcp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/pqcp/mldsa-native/mldsa"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/drivers/pqcp/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/extras"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/tf-psa-crypto/utilities")

if(WIN32)
    target_link_libraries(soundstep_mbedtls PUBLIC ws2_32 bcrypt)
endif()

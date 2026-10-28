"""Shared configuration for Mbed TLS 4 and its TF-PSA-Crypto backend."""


def configure(env):
    env.Prepend(
        CPPPATH=[
            "#thirdparty/mbedtls/include",
            "#thirdparty/mbedtls/tf-psa-crypto/include",
            "#thirdparty/mbedtls/tf-psa-crypto/drivers/builtin/include",
        ]
    )
    for macro, filename in [
        ("MBEDTLS_CONFIG_FILE", "godot_module_mbedtls_config.h"),
        ("TF_PSA_CRYPTO_CONFIG_FILE", "godot_psa_crypto_config.h"),
    ]:
        path = "thirdparty/mbedtls/include/" + filename
        path = f"<{path}>" if env["ninja"] and env.msvc else f'\\"{path}\\"'
        env.Append(CPPDEFINES=[(macro, path)])


def crypto_sources(env):
    root = "#thirdparty/mbedtls/tf-psa-crypto/"
    directories = ["core", "drivers/builtin/src", "extras", "platform", "utilities"]
    env.Prepend(CPPPATH=[root + path for path in directories + ["dispatch", "drivers/builtin/include"]])
    return [source for directory in directories for source in env.Glob(root + directory + "/*.c")]

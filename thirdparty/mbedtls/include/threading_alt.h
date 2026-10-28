#if defined(TF_PSA_CRYPTO_VERSION_MAJOR)
typedef struct {
	void *mutex;
} mbedtls_platform_mutex_t;
typedef struct {
	void *condition;
} mbedtls_platform_condition_variable_t;
#else
typedef struct {
	void *mutex;
} mbedtls_threading_mutex_t;
#endif

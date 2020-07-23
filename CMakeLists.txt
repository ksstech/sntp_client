idf_component_register(
	SRCS "x_sntp_client.c"
	INCLUDE_DIRS "."
	PRIV_INCLUDE_DIRS
	REQUIRES
	PRIV_REQUIRES irmacos printf
)
